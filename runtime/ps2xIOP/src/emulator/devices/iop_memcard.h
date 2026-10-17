#pragma once

#include "ps2x/state_io.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace ps2x::iop::detail
{
    class VirtualMemoryCard
    {
    public:
        static constexpr uint32_t kPageSize = 512u;
        static constexpr uint32_t kSpareSize = 16u;
        static constexpr uint32_t kRawPageSize = kPageSize + kSpareSize;
        static constexpr uint32_t kPagesPerBlock = 16u;
        static constexpr uint32_t kPageCount = 0x4000u;
        static constexpr uint64_t kImageSize = uint64_t{kRawPageSize} * kPageCount;
        static constexpr uint8_t kTerminatorReady = 0x55u;

        void attach(std::filesystem::path path, bool createIfMissing);
        void reset();
        [[nodiscard]] bool present();

        size_t transfer(const std::vector<uint8_t> &command, std::vector<uint8_t> &response);

        template <typename Ar>
        void serialize(Ar &ar)
        {
            ps2x::state::fields(ar, m_terminator, m_transferAddress);
        }

    private:
        bool open();
        void read(uint64_t address, uint8_t *destination, size_t size);
        void write(uint64_t address, const uint8_t *source, size_t size);
        void eraseBlock(uint64_t address);

        std::filesystem::path m_path;
        bool m_create = false;
        bool m_opened = false;
        bool m_available = false;
        std::fstream m_file;
        uint8_t m_terminator = kTerminatorReady;
        uint64_t m_transferAddress = 0u;
    };
}
