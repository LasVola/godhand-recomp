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
            // A savestate restores all IOP RAM, MCMAN's directory/FAT cache included, but not the card file. After a
            // load the card is gone for MCMAN's own retry count (5 failed probes 0x11, mcman 0x2b20-0x2b70 `slti 5`), as if
            // it had been pulled and put back: MCMAN then reports a changed card, drops its cache and re-reads it,
            // instead of writing a newer card through a stale cache (sweep EE-SILENT-1). Not saved itself.
            if constexpr (Ar::kLoading)
                m_absentProbes = kSwapAbsentProbes;
        }
        static constexpr uint32_t kSwapAbsentProbes = 5u;

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
        uint32_t m_absentProbes = 0u;   // after a savestate load: probes (0x11) still to fail; every card command fails until then
        uint32_t m_ioErrors = 0u;       // host file errors logged so far (capped)
    };
}
