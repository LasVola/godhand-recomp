#pragma once

#include <cstddef>
#include "ps2x/state_io.h"
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    class IopSio2;
    class IopSpu2;

    class IopMemory
    {
    public:
        static constexpr uint32_t RamSize = 2u * 1024u * 1024u;
        static constexpr uint32_t ScratchBase = 0x1F800000u;
        static constexpr uint32_t ScratchSize = 0x400u;
        static constexpr uint32_t HardwareBase = 0x1F801000u;
        static constexpr uint32_t HardwareEnd = 0x1F900000u;
        static constexpr uint32_t Spu2Base = 0x1F900000u;
        static constexpr uint32_t Spu2End = 0x1FA00000u;
        static constexpr uint32_t SifBase = 0x1D000000u;
        static constexpr uint32_t SifEnd = 0x1D001000u;
        static constexpr uint32_t HeapBase = 0x00080000u;
        static constexpr uint32_t HeapLimit = 0x001F0000u;

        struct Allocation
        {
            uint32_t address = 0;
            uint32_t size = 0;
        };

        struct DmaStart
        {
            int irq = 0;
            uint64_t delayCycles = 0;
        };
        // DmaStart::delayCycles value meaning "cancel the pending completion of this IRQ" (DMA stopped).
        static constexpr uint64_t kDmaCancelDelay = ~0ull;

        IopMemory();

        void reset();

        // RAM reads inline (the interpreter fetches and loads through these for every instruction); everything else
        // goes to the out-of-line *Slow versions.
        [[nodiscard]] uint8_t read8(uint32_t address) const
        {
            const uint32_t phys = physicalAddress(address);
            return phys < RamSize ? m_ram[phys] : read8Slow(address);
        }
        [[nodiscard]] uint16_t read16(uint32_t address) const
        {
            const uint32_t phys = physicalAddress(address);
            if (phys + 1u < RamSize)
            {
                uint16_t value;
                std::memcpy(&value, m_ram.data() + phys, sizeof(value));
                return value;
            }
            return read16Slow(address);
        }
        [[nodiscard]] uint32_t read32(uint32_t address) const
        {
            const uint32_t phys = physicalAddress(address);
            if ((phys & 3u) == 0u && phys + 3u < RamSize)
            {
                uint32_t value;
                std::memcpy(&value, m_ram.data() + phys, sizeof(value));
                return value;
            }
            return read32Slow(address);
        }
        [[nodiscard]] uint8_t read8Slow(uint32_t address) const;
        [[nodiscard]] uint16_t read16Slow(uint32_t address) const;
        [[nodiscard]] uint32_t read32Slow(uint32_t address) const;
        void write8(uint32_t address, uint8_t value);
        void write16(uint32_t address, uint16_t value);
        void write32(uint32_t address, uint32_t value);

        [[nodiscard]] bool readRam(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeRam(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroRam(uint32_t address, size_t size);
        [[nodiscard]] bool ownsRamRange(uint32_t address, size_t size) const;
        [[nodiscard]] bool isHardwareAddress(uint32_t address) const;
        [[nodiscard]] std::string readString(uint32_t address, size_t limit = 1024u) const;

        [[nodiscard]] uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt);
        [[nodiscard]] bool freeAllocation(uint32_t address);
        [[nodiscard]] uint32_t maxFreeMemory() const;
        [[nodiscard]] std::optional<Allocation> allocationContaining(uint32_t address) const;

        [[nodiscard]] uint32_t interruptStatus() const noexcept { return m_interruptStatus; }
        [[nodiscard]] uint32_t interruptMask() const noexcept { return m_interruptMask; }
        [[nodiscard]] uint32_t interruptControl() const noexcept { return m_interruptControl; }
        void setInterruptStatus(uint32_t value) noexcept { m_interruptStatus = value; }
        void setInterruptMask(uint32_t value) noexcept { m_interruptMask = value; }
        void setInterruptControl(uint32_t value) noexcept { m_interruptControl = value & 1u; }

        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;
        [[nodiscard]] bool hasDmaStart() const noexcept { return m_dmaStart.has_value(); }
        void attachSio2(IopSio2 *sio2) noexcept { m_sio2 = sio2; }
        void attachSpu2(IopSpu2 *spu2, const uint64_t *clock) noexcept
        {
            m_spu2 = spu2;
            m_clock = clock;
        }
        [[nodiscard]] std::span<const uint8_t> ram() const noexcept { return m_ram; }

        [[nodiscard]] static constexpr uint32_t physicalAddress(uint32_t address) noexcept { return address & 0x1FFFFFFFu; }

        template <typename Ar>
        void serialize(Ar &ar)
        {
            ps2x::state::fields(ar, m_ram, m_owned, m_scratch, m_hardware, m_allocations, m_heapCursor, m_interruptStatus, m_interruptMask,
                                m_interruptControl, m_dmaStart);
        }

    private:
        [[nodiscard]] uint32_t readHardware32(uint32_t address) const;
        void writeHardware32(uint32_t address, uint32_t value);
        void markOwned(uint32_t address, size_t size);
        [[nodiscard]] bool spu2Owns(uint32_t phys) const noexcept;
        [[nodiscard]] uint16_t readSpu2(uint32_t phys) const;
        void writeSpu2(uint32_t phys, uint16_t value);
        void startSpu2Dma(uint32_t chcrAddress, uint32_t chcr);

        std::vector<uint8_t> m_ram;
        std::vector<uint8_t> m_owned;
        std::vector<uint8_t> m_scratch;
        std::unordered_map<uint32_t, uint32_t> m_hardware;
        std::vector<Allocation> m_allocations;
        uint32_t m_heapCursor = HeapBase;
        uint32_t m_interruptStatus = 0;
        uint32_t m_interruptMask = 0;
        uint32_t m_interruptControl = 1;
        std::optional<DmaStart> m_dmaStart;
        IopSio2 *m_sio2 = nullptr;
        IopSpu2 *m_spu2 = nullptr;
        const uint64_t *m_clock = nullptr;
    };
}
