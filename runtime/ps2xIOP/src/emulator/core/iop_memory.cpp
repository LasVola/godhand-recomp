#include "iop_memory.h"
#include "iop_cpu.h"

#include <cstdio>
#include <cstdlib>

#include "../devices/iop_sio2.h"
#include "../devices/iop_spu2.h"

#include <algorithm>
#include <cstring>

namespace ps2x::iop::detail
{
    extern const IopCpuState *g_iopLogCpu; // iop_emulator.cpp (diagnostics only)

    namespace
    {
        constexpr uint32_t kDmaSpu0Chcr = 0x1F8010C8u;
        constexpr uint32_t kDmaSpu1Chcr = 0x1F801508u;
        constexpr uint32_t kDmaStart = 1u << 24u;
        constexpr int kDmaSpu0Irq = 0x24;
        constexpr int kDmaSpu1Irq = 0x28;

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }

        // SPU2 DMA progress (channels 4 and 7). On hardware CHCR.START stays set until the transfer ends and MADR/BCR
        // advance block by block while the SPU2 pulls data; for AutoDMA that is the playback position, which libsd's
        // sceSdBlockTransStatus reports (MADR) and the CRI ADX driver uses to know which half of its buffer is free.
        // The runtime used to clear START at once and leave MADR/BCR at their start values. File-local so the
        // header stays unchanged. PS2X_SPU2_DMA_PROGRESS=0 restores the old registers.
        struct Spu2DmaProgress
        {
            bool active = false;
            uint32_t madr = 0u;
            uint32_t blockWords = 1u;
            uint32_t blocks = 0u;
            uint64_t startCycle = 0u;
            uint64_t endCycle = 0u;
        };
        Spu2DmaProgress g_spu2Dma[2];

        bool spu2DmaProgressEnabled()
        {
            static const bool enabled = [] { const char *v = std::getenv("PS2X_SPU2_DMA_PROGRESS"); return !v || *v != '0'; }();
            return enabled;
        }

        // Blocks already transferred at `cycle` (all of them once the transfer is over).
        uint32_t spu2DmaBlocksDone(const Spu2DmaProgress &dma, uint64_t cycle)
        {
            if (cycle >= dma.endCycle || dma.endCycle <= dma.startCycle)
                return dma.blocks;
            return static_cast<uint32_t>((cycle - dma.startCycle) * dma.blocks / (dma.endCycle - dma.startCycle));
        }

        // PS2X_SPU2_STACK_LOG=<n>: for the first n ADMAS writes and SPU2 DMA starts, print the IOP pc/ra/sp and the
        // words on the stack that look like code addresses, to see which driver call led there.
        void dumpIopStackForSpu2Log(const uint8_t *ram, uint32_t phys)
        {
            static uint32_t left = [] { const char *v = std::getenv("PS2X_SPU2_STACK_LOG"); return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u; }();
            const uint32_t local = phys & 0x7FEu;
            const bool admas = phys >= 0x1F900000u && phys < 0x1F900800u && (local == 0x1B0u || local == 0x5B0u);
            const bool dma = phys == kDmaSpu0Chcr || phys == kDmaSpu1Chcr;
            if (left == 0u || (!admas && !dma) || !g_iopLogCpu)
                return;
            --left;
            const uint32_t sp = g_iopLogCpu->gpr[29] & (IopMemory::RamSize - 1u);
            std::fprintf(stderr, "[spu2-stack] %s pc=%06x ra=%06x sp=%06x code ptrs:", admas ? "ADMAS write" : "DMA start", g_iopLogCpu->pc,
                         g_iopLogCpu->gpr[31], sp);
            for (uint32_t i = 0; i < 64u && sp + i * 4u + 4u <= IopMemory::RamSize; ++i)
            {
                uint32_t word = 0u;
                std::memcpy(&word, ram + sp + i * 4u, 4u);
                if (word >= 0x10000u && word < 0x80000u && (word & 3u) == 0u)
                    std::fprintf(stderr, " +%x:%06x", i * 4u, word);
            }
            std::fputc('\n', stderr);
        }
    }

    IopMemory::IopMemory()
        : m_ram(RamSize), m_owned(RamSize), m_scratch(ScratchSize)
    {
        reset();
    }

    void IopMemory::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        std::fill(m_owned.begin(), m_owned.end(), uint8_t{0});
        std::fill(m_scratch.begin(), m_scratch.end(), uint8_t{0});
        m_hardware.clear();
        m_allocations.clear();
        m_heapCursor = HeapBase;
        m_interruptStatus = 0;
        m_interruptMask = 0;
        m_interruptControl = 1;
        m_dmaStart.reset();
    }

    uint8_t IopMemory::read8Slow(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
            return m_ram[phys];
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
            return m_scratch[phys - ScratchBase];
        if (spu2Owns(phys))
            return static_cast<uint8_t>(readSpu2(phys & ~1u) >> ((phys & 1u) * 8u));
        const uint32_t value = readHardware32(phys & ~3u);
        return static_cast<uint8_t>(value >> ((phys & 3u) * 8u));
    }

    uint16_t IopMemory::read16Slow(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            uint16_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 1u) == 0u && spu2Owns(phys))
            return readSpu2(phys);
        return static_cast<uint16_t>(read8(address) | (static_cast<uint16_t>(read8(address + 1u)) << 8u));
    }

    uint32_t IopMemory::read32Slow(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            uint32_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            uint32_t value;
            std::memcpy(&value, m_scratch.data() + (phys - ScratchBase), sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && spu2Owns(phys))
            return static_cast<uint32_t>(readSpu2(phys)) | (static_cast<uint32_t>(readSpu2(phys + 2u)) << 16u);
        if ((phys & 3u) == 0u && isHardwareAddress(phys))
            return readHardware32(phys);

        return static_cast<uint32_t>(read8(address)) |
               (static_cast<uint32_t>(read8(address + 1u)) << 8u) |
               (static_cast<uint32_t>(read8(address + 2u)) << 16u) |
               (static_cast<uint32_t>(read8(address + 3u)) << 24u);
    }

    void IopMemory::write8(uint32_t address, uint8_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
        {
            m_ram[phys] = value;
            markOwned(phys, sizeof(value));
            return;
        }
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
        {
            m_scratch[phys - ScratchBase] = value;
            return;
        }
        if (spu2Owns(phys))
        {
            const uint32_t shift = (phys & 1u) * 8u;
            const uint16_t current = readSpu2(phys & ~1u);
            writeSpu2(phys & ~1u, static_cast<uint16_t>((current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift)));
            return;
        }
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 3u) * 8u;
        current = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
    }

    void IopMemory::write16(uint32_t address, uint16_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 1u) == 0u && spu2Owns(phys))
        {
            writeSpu2(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }

    void IopMemory::write32(uint32_t address, uint32_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            std::memcpy(m_scratch.data() + (phys - ScratchBase), &value, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && spu2Owns(phys))
        {
            writeSpu2(phys, static_cast<uint16_t>(value));
            writeSpu2(phys + 2u, static_cast<uint16_t>(value >> 16u));
            return;
        }
        if ((phys & 3u) == 0u)
        {
            writeHardware32(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
        write8(address + 2u, static_cast<uint8_t>(value >> 16u));
        write8(address + 3u, static_cast<uint8_t>(value >> 24u));
    }

    bool IopMemory::readRam(uint32_t address, void *destination, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((!destination && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
            std::memcpy(destination, m_ram.data() + phys, size);
        return true;
    }

    bool IopMemory::writeRam(uint32_t address, const void *source, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if ((!source && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memcpy(m_ram.data() + phys, source, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::zeroRam(uint32_t address, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memset(m_ram.data() + phys, 0, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::ownsRamRange(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        return std::all_of(m_owned.begin() + phys, m_owned.begin() + phys + size,
                           [](uint8_t value)
                           { return value != 0u; });
    }

    void IopMemory::markOwned(uint32_t address, size_t size)
    {
        if (address > RamSize || size > RamSize - address)
            return;
        std::fill(m_owned.begin() + address, m_owned.begin() + address + size, uint8_t{1});
    }

    bool IopMemory::isHardwareAddress(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        return (phys >= HardwareBase && phys < HardwareEnd) ||
               (phys >= Spu2Base && phys < Spu2End) ||
               (phys >= SifBase && phys < SifEnd);
    }

    uint32_t IopMemory::readHardware32(uint32_t address) const
    {
        if (m_sio2 && IopSio2::owns(address))
            return m_sio2->read32(address);
        if (spu2DmaProgressEnabled() && m_clock &&
            ((address >= kDmaSpu0Chcr - 8u && address <= kDmaSpu0Chcr) || (address >= kDmaSpu1Chcr - 8u && address <= kDmaSpu1Chcr)))
        {
            const bool second = address >= kDmaSpu1Chcr - 8u;
            const Spu2DmaProgress &dma = g_spu2Dma[second ? 1 : 0];
            const uint32_t chcrAddress = second ? kDmaSpu1Chcr : kDmaSpu0Chcr;
            if (dma.active)
            {
                const uint64_t now = *m_clock;
                const uint32_t done = spu2DmaBlocksDone(dma, now);
                const bool running = now < dma.endCycle;
                uint32_t stored = 0u;
                if (const auto it = m_hardware.find(address); it != m_hardware.end())
                    stored = it->second;
                if (address == chcrAddress - 8u) // MADR
                    return dma.madr + done * dma.blockWords * 4u;
                if (address == chcrAddress - 4u) // BCR: block count counts down
                    return (stored & 0xFFFFu) | ((dma.blocks - done) << 16u);
                if (address == chcrAddress) // CHCR
                    return running ? (stored | kDmaStart) : (stored & ~kDmaStart);
            }
        }
        const auto value = m_hardware.find(address);
        if (value != m_hardware.end())
            return value->second;
        switch (address)
        {
        case 0x1F801070u:
            return m_interruptStatus;
        case 0x1F801074u:
            return m_interruptMask;
        case 0x1F801078u:
            return m_interruptControl;
        default:
            return 0u;
        }
    }

    void IopMemory::writeHardware32(uint32_t address, uint32_t value)
    {
        if (m_sio2 && IopSio2::owns(address))
        {
            m_sio2->write32(address, value);
            if (const auto completion = m_sio2->takeCompletion())
                m_dmaStart = DmaStart{IopSio2::kIrq, completion->delayCycles};
            return;
        }
        switch (address)
        {
        case 0x1F801070u:
            m_interruptStatus &= value;
            return;
        case 0x1F801074u:
            m_interruptMask = value;
            return;
        case 0x1F801078u:
            m_interruptControl = value & 1u;
            return;
        default:
            break;
        }

        m_hardware[address] = value;
        // A new MADR/BCR/CHCR value from the IOP replaces the progress view of the previous transfer.
        if ((address >= kDmaSpu0Chcr - 8u && address <= kDmaSpu0Chcr) || (address >= kDmaSpu1Chcr - 8u && address <= kDmaSpu1Chcr))
            g_spu2Dma[address >= kDmaSpu1Chcr - 8u ? 1 : 0].active = false;
        // CHCR written without START (libsd's sceSdBlockTrans STOP clears it while an AutoDMA block is in flight): the
        // transfer is cancelled, so its completion interrupt must not fire. A stale completion made libsd's handler
        // re-arm a stopped loop, and after the next start two chains ran (twice the ADMA rate, ring halves toggling
        // twice as fast) and the CRI ADX stream stalled - Urban Reign's story mission load then hung at random.
        // PS2X_SPU2_DMA_CANCEL=0 keeps the old behaviour.
        if ((address == kDmaSpu0Chcr || address == kDmaSpu1Chcr) && (value & kDmaStart) == 0u)
        {
            static const bool cancel = [] { const char *v = std::getenv("PS2X_SPU2_DMA_CANCEL"); return !v || *v != '0'; }();
            if (cancel)
                m_dmaStart = DmaStart{address == kDmaSpu1Chcr ? kDmaSpu1Irq : kDmaSpu0Irq, kDmaCancelDelay};
            return;
        }
        if ((address != kDmaSpu0Chcr && address != kDmaSpu1Chcr) || (value & kDmaStart) == 0u)
            return;

        const bool secondCore = address == kDmaSpu1Chcr;
        dumpIopStackForSpu2Log(m_ram.data(), address);
        m_hardware[address] = value & ~kDmaStart;
        if (m_spu2)
            startSpu2Dma(address, value);

        const uint32_t statusAddress = 0x1F900344u + (secondCore ? 0x400u : 0u);
        const uint32_t alignedStatus = statusAddress & ~3u;
        const uint32_t shift = (statusAddress & 2u) * 8u;
        if (!m_spu2)
        {
            uint32_t status = 0u;
            if (const auto current = m_hardware.find(alignedStatus); current != m_hardware.end())
                status = current->second;
            status |= 0x80u << shift;
            m_hardware[alignedStatus] = status;
        }

        const uint32_t blockControlAddress = address - sizeof(uint32_t);
        uint32_t blockControl = 0u;
        if (const auto current = m_hardware.find(blockControlAddress); current != m_hardware.end())
            blockControl = current->second;
        const uint32_t wordsPerBlock = std::max<uint32_t>(blockControl & 0xFFFFu, 1u);
        const uint32_t blockCount = std::max<uint32_t>(blockControl >> 16u, 1u);
        const uint64_t transferWords = static_cast<uint64_t>(wordsPerBlock) * blockCount;
        // ADMA plays 4 bytes (one stereo sample) per 48 kHz tick: completion after transferWords samples.
        static const bool admaTiming = [] { const char *v = std::getenv("PS2X_SPU2_ADMA"); return !v || *v != '0'; }();
        const bool adma = admaTiming && m_spu2 && m_spu2->autoDma(secondCore ? 1u : 0u) && (value & 1u) != 0u;
        m_dmaStart = DmaStart{
            secondCore ? kDmaSpu1Irq : kDmaSpu0Irq,
            adma ? transferWords * IopSpu2::kCyclesPerSample : std::max<uint64_t>(transferWords * 2u, 64u),
        };
        if (m_clock)
        {
            Spu2DmaProgress &dma = g_spu2Dma[secondCore ? 1 : 0];
            uint32_t madr = 0u;
            if (const auto it = m_hardware.find(address - 8u); it != m_hardware.end())
                madr = it->second;
            dma.active = true;
            dma.madr = madr;
            dma.blockWords = wordsPerBlock;
            dma.blocks = blockCount;
            dma.startCycle = *m_clock;
            dma.endCycle = *m_clock + m_dmaStart->delayCycles;
        }
    }

    bool IopMemory::spu2Owns(uint32_t phys) const noexcept
    {
        return m_spu2 && IopSpu2::owns(phys);
    }

    uint16_t IopMemory::readSpu2(uint32_t phys) const
    {
        return m_spu2->read16(phys, m_clock ? *m_clock : 0u);
    }

    void IopMemory::writeSpu2(uint32_t phys, uint16_t value)
    {
        dumpIopStackForSpu2Log(m_ram.data(), phys);
        m_spu2->write16(phys, value, m_clock ? *m_clock : 0u);
    }

    void IopMemory::startSpu2Dma(uint32_t chcrAddress, uint32_t chcr)
    {
        const uint32_t core = chcrAddress == kDmaSpu1Chcr ? 1u : 0u;
        const uint64_t cycle = m_clock ? *m_clock : 0u;
        m_spu2->dmaStarted(core);
        uint32_t madr = 0u;
        if (const auto it = m_hardware.find(chcrAddress - 8u); it != m_hardware.end())
            madr = it->second & (RamSize - 1u);
        uint32_t bcr = 0u;
        if (const auto it = m_hardware.find(chcrAddress - 4u); it != m_hardware.end())
            bcr = it->second;
        const uint64_t words = static_cast<uint64_t>(std::max<uint32_t>(bcr & 0xFFFFu, 1u)) * std::max<uint32_t>(bcr >> 16u, 1u);
        const size_t bytes = static_cast<size_t>(std::min<uint64_t>(words * 4u, RamSize - madr));
        static const bool admaEnabled = [] { const char *v = std::getenv("PS2X_SPU2_ADMA"); return !v || *v != '0'; }();
        // PS2X_SPU2_DMA_LOG=<n>: log the first n SPU2 DMA starts (core, CHCR, MADR, BCR, AutoDMA state).
        static uint32_t dmaLogLeft = [] { const char *v = std::getenv("PS2X_SPU2_DMA_LOG"); return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u; }();
        if (dmaLogLeft != 0u)
        {
            --dmaLogLeft;
            std::fprintf(stderr, "[spu2-dma] core %u chcr=%08x madr=%06x bcr=%08x adma=%d cycle %llu\n", core, chcr, madr, bcr,
                         m_spu2->autoDma(core) ? 1 : 0, static_cast<unsigned long long>(cycle));
        }
        if (m_spu2->autoDma(core) && !admaEnabled)
            return; // old behaviour: ADMA transfers dropped
        if (m_spu2->autoDma(core) && (chcr & 1u) != 0u)
        {
            // ADMA streaming (CRI ADX music/voices): played from IOP RAM as the core mixes it; the DMA completes when
            // the last block has been consumed (scheduled in writeHardware).
            m_spu2->admaStart(core, m_ram.data() + madr, bytes, cycle);
            return;
        }
        if ((chcr & 1u) != 0u)
            m_spu2->dmaWrite(core, m_ram.data() + madr, bytes, cycle);
        else
        {
            m_spu2->dmaRead(core, m_ram.data() + madr, bytes, cycle);
            markOwned(madr, bytes);
        }
    }

    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {
        std::optional<DmaStart> result = m_dmaStart;
        m_dmaStart.reset();
        return result;
    }

    uint32_t IopMemory::allocate(uint32_t size, uint32_t alignment, std::optional<uint32_t> fixed)
    {
        size = alignUp(std::max(size, 1u), 16u);
        alignment = std::max<uint32_t>(alignment, 4u);
        if (fixed)
        {
            const uint32_t address = *fixed;
            if (address < HeapBase || address + size > HeapLimit)
                return 0u;
            for (const auto &block : m_allocations)
                if (address < block.address + block.size && block.address < address + size)
                    return 0u;
            m_allocations.push_back({address, size});
            markOwned(address, size);
            return address;
        }

        uint32_t candidate = alignUp(HeapBase, alignment);
        for (;;)
        {
            bool overlap = false;
            for (const auto &block : m_allocations)
            {
                if (candidate < block.address + block.size && block.address < candidate + size)
                {
                    candidate = alignUp(block.address + block.size, alignment);
                    overlap = true;
                }
            }
            if (!overlap)
                break;
        }
        if (candidate > HeapLimit || size > HeapLimit - candidate)
            return 0u;
        m_allocations.push_back({candidate, size});
        markOwned(candidate, size);
        m_heapCursor = std::max(m_heapCursor, candidate + size);
        return candidate;
    }

    bool IopMemory::freeAllocation(uint32_t address)
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        { return candidate.address == address; });
        if (block == m_allocations.end())
            return false;
        std::fill(m_owned.begin() + block->address,
                  m_owned.begin() + block->address + block->size,
                  uint8_t{0});
        m_allocations.erase(block);
        return true;
    }

    uint32_t IopMemory::maxFreeMemory() const
    {
        return m_heapCursor < HeapLimit ? HeapLimit - m_heapCursor : 0u;
    }

    std::optional<IopMemory::Allocation> IopMemory::allocationContaining(uint32_t address) const
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        {
                                            return address >= candidate.address &&
                                                   address < candidate.address + candidate.size;
                                        });
        if (block == m_allocations.end())
            return std::nullopt;
        return *block;
    }

    std::string IopMemory::readString(uint32_t address, size_t limit) const
    {
        std::string result;
        result.reserve(std::min<size_t>(limit, 64u));
        for (size_t i = 0; i < limit; ++i)
        {
            const char ch = static_cast<char>(read8(address + static_cast<uint32_t>(i)));
            if (ch == '\0')
                break;
            result.push_back(ch);
        }
        return result;
    }
}
