#include "emulator/core/iop_memory.h"
#include "emulator/devices/iop_spu2.h"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
    using namespace ps2x::iop::detail;

    int g_failures = 0;

    void expect(bool condition, std::string_view what)
    {
        if (!condition)
        {
            ++g_failures;
            std::cerr << "FAILED: " << what << "\n";
        }
    }

    constexpr uint32_t kSpu = 0x1F900000u;
    constexpr uint32_t kBlockHalfwords = 8u;

    struct Rig
    {
        IopMemory memory;
        IopSpu2 spu2;
        uint64_t clock = 0u;

        Rig()
        {
            memory.attachSpu2(&spu2, &clock);
        }

        void write(uint32_t offset, uint16_t value) { memory.write16(kSpu + offset, value); }
        uint16_t read(uint32_t offset) { return memory.read16(kSpu + offset); }
        void samples(uint64_t count) { clock += count * IopSpu2::kCyclesPerSample; }

        void uploadBlocks(uint32_t address, const uint8_t *flags, uint32_t blocks)
        {
            write(0x1A8, static_cast<uint16_t>(address >> 16));
            write(0x1AA, static_cast<uint16_t>(address));
            for (uint32_t block = 0; block < blocks; ++block)
                for (uint32_t half = 0; half < kBlockHalfwords; ++half)
                    write(0x1AC, half == 0u ? static_cast<uint16_t>(flags[block] << 8) : uint16_t{0x1111});
        }

        void setupVoice(uint32_t voice, uint32_t start, uint16_t pitch)
        {
            write(voice * 0x10u + 4u, pitch);
            write(voice * 0x10u + 6u, 0x000Fu);
            write(voice * 0x10u + 8u, 0x0000u);
            write(0x1C0u + voice * 0xCu, static_cast<uint16_t>(start >> 16));
            write(0x1C2u + voice * 0xCu, static_cast<uint16_t>(start));
        }

        uint32_t nax(uint32_t voice)
        {
            return (static_cast<uint32_t>(read(0x1C8u + voice * 0xCu)) << 16) | read(0x1CAu + voice * 0xCu);
        }
    };

    void testStreamLoop()
    {
        Rig rig;
        const uint8_t flags[4] = {0x4, 0x0, 0x0, 0x3};
        rig.uploadBlocks(0x5000u, flags, 4u);
        expect(rig.spu2.ram()[0x5000u] == 0x0400u, "PIO writes the block header into SPU2 RAM");
        expect(rig.spu2.ram()[0x5001u] == 0x1111u, "PIO advances the transfer address");
        rig.setupVoice(3u, 0x5000u, 0x1000u);
        rig.write(0x1A0, 1u << 3);
        expect(rig.nax(3u) == 0x5000u, "key on points NAX at the start address");
        rig.samples(29u);
        expect(rig.nax(3u) >= 0x5008u && rig.nax(3u) < 0x5010u, "NAX is in the second block after 29 samples at pitch 0x1000");
        expect((rig.read(0x340) & (1u << 3)) == 0u, "no ENDX before the loop end");
        rig.samples(28u * 3u);
        expect((rig.read(0x340) & (1u << 3)) != 0u, "ENDX set at the loop-end block");
        expect(rig.nax(3u) >= 0x5000u && rig.nax(3u) < 0x5008u, "loop repeat jumps back to the loop-start block");
        expect(rig.spu2.loopAddress(0u, 3u) == 0x5000u, "loop-start flag sets LSAX");
        expect(rig.read(3u * 0x10u + 0xAu) != 0u, "envelope still up while looping");
        rig.write(0x340, 0xFFFFu);
        expect((rig.read(0x340) & (1u << 3)) == 0u, "writing ENDX clears it");
    }

    void testEndWithoutRepeatStopsEnvelope()
    {
        Rig rig;
        const uint8_t flags[2] = {0x4, 0x1};
        rig.uploadBlocks(0x6000u, flags, 2u);
        rig.setupVoice(0u, 0x6000u, 0x1000u);
        rig.write(0x1A0, 1u);
        rig.samples(10u);
        expect(rig.read(0xAu) != 0u, "attack raises ENVX");
        rig.samples(28u * 2u);
        expect(rig.read(0xAu) == 0u, "loop end without repeat silences the voice");
        expect((rig.read(0x340) & 1u) != 0u, "ENDX set for the stopped voice");
    }

    void testHalfPitch()
    {
        Rig rig;
        const uint8_t flags[4] = {0x4, 0x0, 0x0, 0x3};
        rig.uploadBlocks(0x7000u, flags, 4u);
        rig.setupVoice(20u, 0x7000u, 0x0800u);
        rig.write(0x1A2, 1u << 4);
        rig.samples(54u);
        expect(rig.nax(20u) >= 0x7000u && rig.nax(20u) < 0x7008u, "half pitch consumes half as many samples");
        rig.samples(6u);
        expect(rig.nax(20u) >= 0x7008u && rig.nax(20u) < 0x7010u, "half pitch reaches the second block after 60 samples");
    }

    void testCore1AndKeyOnIsolation()
    {
        Rig rig;
        const uint8_t flags[2] = {0x4, 0x3};
        rig.uploadBlocks(0x8000u, flags, 2u);
        rig.setupVoice(1u, 0x8000u, 0x1000u);
        rig.setupVoice(17u, 0x8000u, 0x1000u);
        rig.write(0x1A0, 1u << 1);
        expect(rig.spu2.envelope(0u, 17u) == 0 && rig.nax(17u) == 0u, "a 16-bit KON0 write does not key on KON1 voices");
        rig.write(0x400u + 0x1C0u, 0u);
        rig.write(0x400u + 0x1C2u, 0x8000u);
        rig.write(0x400u + 4u, 0x1000u);
        rig.write(0x400u + 6u, 0x000Fu);
        rig.write(0x400u + 0x1A0u, 1u);
        rig.samples(40u);
        rig.spu2.advanceTo(rig.clock);
        expect(rig.spu2.nextAddress(1u, 0u) >= 0x8008u, "core 1 voices play");
        expect(rig.spu2.nextAddress(0u, 1u) >= 0x8008u, "core 0 voice keeps playing");
    }

    void testDmaCopiesIntoSpuRam()
    {
        Rig rig;
        for (uint32_t i = 0; i < 64u; ++i)
            rig.memory.write8(0x10000u + i, static_cast<uint8_t>(i));
        rig.write(0x1A8, 0u);
        rig.write(0x1AA, 0x2400u);
        rig.memory.write32(0x1F8010C0u, 0x10000u);
        rig.memory.write32(0x1F8010C4u, (1u << 16) | 16u);
        rig.memory.write32(0x1F8010C8u, 0x01000201u);
        expect(rig.spu2.ram()[0x2400u] == 0x0100u && rig.spu2.ram()[0x241Fu] == 0x3F3Eu, "DMA channel 4 writes IOP RAM into SPU2 RAM at TSA");
        expect(rig.spu2.transferAddress(0u) == 0x2420u, "DMA advances TSA");
        expect((rig.read(0x344) & 0x80u) != 0u, "DMA start sets the STATX ready bit");
        const auto dma = rig.memory.takeDmaStart();
        expect(dma.has_value() && dma->irq == 0x24, "DMA completion interrupt is scheduled");
    }

    void testStereoPcmAndState()
    {
        Rig rig;
        std::vector<int16_t> output;
        rig.spu2.setAudioSink([&](std::span<const int16_t> samples) { output.insert(output.end(), samples.begin(), samples.end()); });
        const uint8_t flags[2] = {0x4, 0x3};
        rig.uploadBlocks(0x5000u, flags, 2u);
        rig.setupVoice(0u, 0x5000u, 0x1000u);
        rig.write(0u, 0x3FFFu);
        rig.write(2u, 0x2000u);
        rig.write(0x188u, 1u);
        rig.write(0x190u, 1u);
        rig.write(0x198u, 0xC00u);
        rig.write(0x19Au, 0x4000u);
        rig.write(0x760u, 0x3FFFu);
        rig.write(0x762u, 0x3FFFu);
        rig.write(0x598u, 0xC0u);
        rig.write(0x59Au, 0x4000u);
        rig.write(0x788u, 0x3FFFu);
        rig.write(0x78Au, 0x3FFFu);
        rig.write(0x790u, 0x7FFFu);
        rig.write(0x792u, 0x7FFFu);
        rig.write(0x1A0u, 1u);
        rig.write(0xAu, 0x7FFFu);
        rig.samples(8u);
        rig.spu2.advanceTo(rig.clock);
        rig.spu2.flushAudio();
        expect(output.size() == 16u && output[14] >= 4000 && output[14] < 4096 && output[15] >= 2000 && output[15] < 2048,
               "Gaussian interpolation preserves steady ADPCM levels and stereo balance through both cores");
        rig.write(4u, 0x1234u);
        rig.samples(17u);
        rig.spu2.advanceTo(rig.clock);
        rig.spu2.flushAudio();
        ps2x::state::Writer saved;
        rig.spu2.serialize(saved);
        output.clear();
        rig.samples(87u);
        rig.spu2.advanceTo(rig.clock);
        rig.spu2.flushAudio();
        const auto expected = output;
        output.clear();
        ps2x::state::Reader reader(saved.data().data(), saved.data().size());
        rig.spu2.serialize(reader);
        rig.spu2.advanceTo(rig.clock);
        rig.spu2.flushAudio();
        expect(output == expected && reader.remaining() == 0u, "save state restores decoder history and fractional pitch exactly");
        output.clear();
        rig.write(0x190u, 0u);
        rig.samples(4u);
        rig.spu2.advanceTo(rig.clock);
        rig.spu2.flushAudio();
        expect(output.size() == 8u && output[0] > 0 && output[1] == 0, "voice mixer masks silence one stereo channel");
        output.clear();
        rig.write(0x1A4u, 1u);
        rig.samples(200u);
        rig.spu2.advanceTo(rig.clock);
        rig.spu2.flushAudio();
        expect(output.size() == 400u && output[398] == 0 && output[399] == 0, "key off releases audible PCM to silence");
    }
}

int main()
{
    testStreamLoop();
    testEndWithoutRepeatStopsEnvelope();
    testHalfPitch();
    testCore1AndKeyOnIsolation();
    testDmaCopiesIntoSpuRam();
    testStereoPcmAndState();
    if (g_failures == 0)
        std::cout << "iop_spu2_tests: all passed\n";
    return g_failures == 0 ? 0 : 1;
}
