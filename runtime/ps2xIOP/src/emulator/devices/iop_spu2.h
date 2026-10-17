#pragma once

#include <array>
#include "ps2x/state_io.h"
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ps2x::iop::detail
{
    class IopSpu2
    {
    public:
        static constexpr uint32_t kBase = 0x1F900000u;
        static constexpr uint32_t kEnd = 0x1F900800u;
        static constexpr uint32_t kRamHalfwords = 0x100000u;
        static constexpr uint64_t kCyclesPerSample = 768u;
        static constexpr uint32_t kCores = 2u;
        static constexpr uint32_t kVoices = 24u;

        IopSpu2();

        void reset();

        [[nodiscard]] static bool owns(uint32_t physicalAddress) noexcept
        {
            return physicalAddress >= kBase && physicalAddress < kEnd;
        }

        [[nodiscard]] uint16_t read16(uint32_t physicalAddress, uint64_t cycle);
        void write16(uint32_t physicalAddress, uint16_t value, uint64_t cycle);

        [[nodiscard]] bool autoDma(uint32_t core) const noexcept;
        void dmaStarted(uint32_t core) noexcept;
        // AutoDMA (ADMA): `source` (IOP RAM) holds blocks of 256 left + 256 right samples (0x400 bytes) that the core
        // plays as its sound data input at 48 kHz. The data is read lazily as it is mixed, like the hardware FIFO.
        void admaStart(uint32_t core, const uint8_t *source, size_t bytes, uint64_t cycle);
        void dmaWrite(uint32_t core, const uint8_t *source, size_t bytes, uint64_t cycle);
        void dmaRead(uint32_t core, uint8_t *destination, size_t bytes, uint64_t cycle);

        void advanceTo(uint64_t cycle);
        void setAudioSink(std::function<void(std::span<const int16_t>)> sink) { m_audioSink = std::move(sink); }
        void flushAudio();

        [[nodiscard]] uint32_t nextAddress(uint32_t core, uint32_t voice) const noexcept { return m_voices[core][voice].nextA; }
        [[nodiscard]] uint32_t loopAddress(uint32_t core, uint32_t voice) const noexcept { return m_voices[core][voice].loopStartA; }
        [[nodiscard]] int32_t envelope(uint32_t core, uint32_t voice) const noexcept { return m_voices[core][voice].envelope; }
        [[nodiscard]] uint32_t endFlags(uint32_t core) const noexcept { return m_endx[core]; }
        [[nodiscard]] uint32_t transferAddress(uint32_t core) const noexcept { return m_transferAddress[core]; }
        [[nodiscard]] std::span<const uint16_t> ram() const noexcept { return m_ram; }

        template <typename Ar>
        void serialize(Ar &ar)
        {
            ps2x::state::fields(ar, m_regs, m_ram, m_voices, m_endx, m_transferAddress, m_cycle);
            constexpr uint32_t tag = ps2x::state::fourcc("SPCM");
            if constexpr (Ar::kLoading)
            {
                for (auto &core : m_decoders)
                    core.fill(Decoder{});
                m_interpolation = {};
                m_audioFrames = 0u;
                if (!ar.nextSectionIs(tag))
                    return;
            }
            ar.section(tag);
            ps2x::state::fields(ar, m_decoders);
            constexpr uint32_t interpolationTag = ps2x::state::fourcc("SINT");
            if constexpr (Ar::kLoading)
            {
                for (uint32_t core = 0u; core < kCores; ++core)
                    for (uint32_t voice = 0u; voice < kVoices; ++voice)
                    {
                        m_interpolation[core][voice].fill(m_decoders[core][voice].previous);
                        m_interpolation[core][voice][3] = m_decoders[core][voice].current;
                    }
                if (!ar.nextSectionIs(interpolationTag))
                    return;
            }
            ar.section(interpolationTag);
            ps2x::state::fields(ar, m_interpolation);
        }

    private:
        struct Voice
        {
            uint32_t startA = 0u;
            uint32_t loopStartA = 0u;
            uint32_t nextA = 0u;
            uint32_t sampleIndex = 28u;
            int32_t counter = 0;
            uint8_t loopFlags = 0u;
            bool manualLoop = false;
            int32_t envelope = 0;
            uint8_t phase = 0u;
        };

        struct Decoder
        {
            std::array<int16_t, 28> pcm{};
            int32_t history1 = 0;
            int32_t history2 = 0;
            int32_t previous = 0;
            int32_t current = 0;
        };

        [[nodiscard]] uint16_t &reg(uint32_t offset) noexcept { return m_regs[(offset & 0x7FEu) >> 1u]; }
        [[nodiscard]] uint16_t reg(uint32_t offset) const noexcept { return m_regs[(offset & 0x7FEu) >> 1u]; }

        void keyOn(uint32_t core, uint32_t voice);
        void keyOff(uint32_t core, uint32_t voice);
        int32_t runVoice(uint32_t core, uint32_t voice);
        void stepAddress(uint32_t core, uint32_t voice);
        void stepEnvelope(Voice &voice, uint16_t adsr1, uint16_t adsr2);
        void decodeBlock(uint32_t core, uint32_t voice, uint32_t address);
        std::array<int32_t, 2> mixCore(uint32_t core, const std::array<int32_t, 2> &external);
        std::array<int32_t, 2> reverbFrame(uint32_t core, const std::array<int32_t, 2> &wet);
        std::array<int32_t, 2> admaSample(uint32_t core);

        struct Adma
        {
            const uint8_t *source = nullptr;
            size_t bytes = 0u;
            size_t position = 0u;
            uint32_t index = 256u;
            std::array<int16_t, 256> left{};
            std::array<int16_t, 256> right{};
        };
        std::array<Adma, kCores> m_adma{};

        // Reverb unit state per core (iop_spu2.cpp, reverbFrame): 48 kHz wet input and zero-stuffed 24 kHz output
        // histories for the 39-tap resampling filter, the work-area position and a frame counter.
        struct Reverb
        {
            std::array<std::array<int16_t, 64>, 2> in{};
            std::array<std::array<int16_t, 64>, 2> out{};
            uint32_t current = 0u;
            uint64_t frame = 0u;
        };
        std::array<Reverb, kCores> m_reverb{};

        std::array<uint16_t, 0x400> m_regs{};
        std::vector<uint16_t> m_ram;
        std::array<std::array<Voice, kVoices>, kCores> m_voices{};
        std::array<uint32_t, kCores> m_endx{};
        std::array<uint32_t, kCores> m_transferAddress{};
        uint64_t m_cycle = 0u;
        std::array<std::array<Decoder, kVoices>, kCores> m_decoders{};
        std::array<std::array<std::array<int16_t, 4>, kVoices>, kCores> m_interpolation{};
        std::array<int16_t, 1024> m_audioBuffer{};
        size_t m_audioFrames = 0u;
        std::function<void(std::span<const int16_t>)> m_audioSink;
    };
}
