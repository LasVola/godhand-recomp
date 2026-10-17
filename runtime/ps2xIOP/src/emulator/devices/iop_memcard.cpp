#include "iop_memcard.h"

#include <algorithm>
#include <array>
#include <system_error>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint8_t kDeviceMemoryCard = 0x81u;
        constexpr uint8_t kAck = 0x2Bu;

        constexpr uint8_t kProbe = 0x11u;
        constexpr uint8_t kWriteDeleteEnd = 0x12u;
        constexpr uint8_t kSetEraseSector = 0x21u;
        constexpr uint8_t kSetWriteSector = 0x22u;
        constexpr uint8_t kSetReadSector = 0x23u;
        constexpr uint8_t kGetSpecs = 0x26u;
        constexpr uint8_t kSetTerminator = 0x27u;
        constexpr uint8_t kGetTerminator = 0x28u;
        constexpr uint8_t kWriteData = 0x42u;
        constexpr uint8_t kReadData = 0x43u;
        constexpr uint8_t kReadWriteEnd = 0x81u;
        constexpr uint8_t kEraseBlock = 0x82u;
        constexpr uint8_t kBoot = 0xBFu;
        constexpr uint8_t kAuthXor = 0xF0u;
        constexpr uint8_t kAuthReset = 0xF3u;
        constexpr uint8_t kAuthF7 = 0xF7u;
    }

    void VirtualMemoryCard::attach(std::filesystem::path path, bool createIfMissing)
    {
        if (m_file.is_open())
            m_file.close();
        m_path = std::move(path);
        m_create = createIfMissing;
        m_opened = false;
        m_available = false;
    }

    void VirtualMemoryCard::reset()
    {
        m_terminator = kTerminatorReady;
        m_transferAddress = 0u;
    }

    bool VirtualMemoryCard::present()
    {
        if (!m_opened)
            m_available = open();
        return m_available;
    }

    bool VirtualMemoryCard::open()
    {
        m_opened = true;
        if (m_path.empty())
            return false;
        std::error_code error;
        const bool exists = std::filesystem::exists(m_path, error);
        if (!exists)
        {
            if (!m_create)
                return false;
            if (m_path.has_parent_path())
                std::filesystem::create_directories(m_path.parent_path(), error);
            std::ofstream created(m_path, std::ios::binary | std::ios::trunc);
            const std::vector<uint8_t> blank(uint64_t{kRawPageSize} * kPagesPerBlock * 64u, uint8_t{0xFFu});
            for (uint64_t written = 0; written < kImageSize; written += blank.size())
                created.write(reinterpret_cast<const char *>(blank.data()), static_cast<std::streamsize>(blank.size()));
            if (!created)
                return false;
        }
        else if (std::filesystem::file_size(m_path, error) != kImageSize)
        {
            return false;
        }
        m_file.open(m_path, std::ios::binary | std::ios::in | std::ios::out);
        return m_file.is_open();
    }

    void VirtualMemoryCard::read(uint64_t address, uint8_t *destination, size_t size)
    {
        std::fill_n(destination, size, uint8_t{0xFFu});
        if (address >= kImageSize)
            return;
        const size_t available = static_cast<size_t>(std::min<uint64_t>(size, kImageSize - address));
        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(address));
        m_file.read(reinterpret_cast<char *>(destination), static_cast<std::streamsize>(available));
    }

    void VirtualMemoryCard::write(uint64_t address, const uint8_t *source, size_t size)
    {
        if (address >= kImageSize)
            return;
        const size_t available = static_cast<size_t>(std::min<uint64_t>(size, kImageSize - address));
        m_file.clear();
        m_file.seekp(static_cast<std::streamoff>(address));
        m_file.write(reinterpret_cast<const char *>(source), static_cast<std::streamsize>(available));
        m_file.flush();
    }

    void VirtualMemoryCard::eraseBlock(uint64_t address)
    {
        const std::vector<uint8_t> blank(uint64_t{kRawPageSize} * kPagesPerBlock, uint8_t{0xFFu});
        write(address, blank.data(), blank.size());
    }

    size_t VirtualMemoryCard::transfer(const std::vector<uint8_t> &command, std::vector<uint8_t> &response)
    {
        if (command.size() < 2u || command[0] != kDeviceMemoryCard || !present())
            return 0u;

        size_t cursor = 2u;
        auto next = [&]() -> uint8_t
        { return cursor < command.size() ? command[cursor++] : uint8_t{0u}; };

        std::vector<uint8_t> out{0x00u, 0x00u};
        auto acknowledge = [&](size_t length)
        {
            while (out.size() + 2u < length)
                out.push_back(0x00u);
            out.push_back(kAck);
            out.push_back(m_terminator);
        };

        switch (command[1])
        {
        case kProbe:
        case kWriteDeleteEnd:
        case kReadWriteEnd:
            acknowledge(4u);
            break;
        case kSetEraseSector:
        case kSetWriteSector:
        case kSetReadSector:
        {
            std::array<uint8_t, 4> bytes{};
            for (uint8_t &byte : bytes)
                byte = next();
            (void)next();
            const uint32_t sector = bytes[0] | (bytes[1] << 8u) | (bytes[2] << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
            m_transferAddress = uint64_t{kRawPageSize} * sector;
            acknowledge(9u);
            break;
        }
        case kGetSpecs:
        {
            const std::array<uint8_t, 8> specs{static_cast<uint8_t>(kPageSize), static_cast<uint8_t>(kPageSize >> 8u),
                                               static_cast<uint8_t>(kPagesPerBlock), static_cast<uint8_t>(kPagesPerBlock >> 8u),
                                               static_cast<uint8_t>(kPageCount), static_cast<uint8_t>(kPageCount >> 8u),
                                               static_cast<uint8_t>(kPageCount >> 16u), static_cast<uint8_t>(kPageCount >> 24u)};
            uint8_t checksum = 0u;
            out.push_back(kAck);
            for (const uint8_t byte : specs)
            {
                out.push_back(byte);
                checksum ^= byte;
            }
            out.push_back(checksum);
            out.push_back(m_terminator);
            break;
        }
        case kSetTerminator:
            m_terminator = next();
            out.push_back(0x00u);
            out.push_back(kAck);
            out.push_back(m_terminator);
            break;
        case kGetTerminator:
            out.push_back(kAck);
            out.push_back(m_terminator);
            out.push_back(m_terminator);
            break;
        case kWriteData:
        {
            const uint8_t length = next();
            std::vector<uint8_t> data(length);
            uint8_t checksum = 0u;
            for (uint8_t &byte : data)
            {
                byte = next();
                checksum ^= byte;
            }
            write(m_transferAddress, data.data(), data.size());
            m_transferAddress += length;
            out.push_back(0x00u);
            out.push_back(kAck);
            out.insert(out.end(), length, uint8_t{0x00u});
            out.push_back(checksum);
            out.push_back(m_terminator);
            break;
        }
        case kReadData:
        {
            const uint8_t length = next();
            std::vector<uint8_t> data(length);
            read(m_transferAddress, data.data(), data.size());
            m_transferAddress += length;
            uint8_t checksum = 0u;
            for (const uint8_t byte : data)
                checksum ^= byte;
            out.push_back(0x00u);
            out.push_back(kAck);
            out.insert(out.end(), data.begin(), data.end());
            out.push_back(checksum);
            out.push_back(m_terminator);
            break;
        }
        case kEraseBlock:
            eraseBlock(m_transferAddress);
            acknowledge(4u);
            break;
        case kBoot:
        case kAuthF7:
            acknowledge(5u);
            break;
        case kAuthReset:
            m_terminator = kTerminatorReady;
            acknowledge(5u);
            break;
        case kAuthXor:
        {
            const uint8_t mode = next();
            switch (mode)
            {
            case 0x01u:
            case 0x02u:
            case 0x04u:
            case 0x0Fu:
            case 0x11u:
            case 0x13u:
            {
                uint8_t checksum = 0u;
                for (int i = 0; i < 8; ++i)
                    checksum ^= next();
                out.push_back(0x00u);
                out.push_back(kAck);
                out.insert(out.end(), 8u, uint8_t{0x00u});
                out.push_back(checksum);
                out.push_back(m_terminator);
                break;
            }
            case 0x06u:
            case 0x07u:
            case 0x0Bu:
                acknowledge(14u);
                break;
            default:
                acknowledge(5u);
                break;
            }
            break;
        }
        default:
            return 0u;
        }

        std::copy_n(out.begin(), std::min(out.size(), response.size()), response.begin());
        return out.size();
    }
}
