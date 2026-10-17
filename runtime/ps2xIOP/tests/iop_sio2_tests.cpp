#include "emulator/core/iop_memory.h"
#include "emulator/devices/iop_dualshock2.h"
#include "emulator/devices/iop_memcard.h"
#include "emulator/devices/iop_sio2.h"
#include "ps2x/iop/iop_host.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;

    class PadHost final : public IopHost
    {
    public:
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}

        bool readPad(uint32_t port, PadInputState &state) override
        {
            state = port == 0u ? pad : PadInputState{};
            return state.connected;
        }

        void padVibration(uint32_t, uint8_t smallMotor, uint8_t largeMotor) override
        {
            lastSmall = smallMotor;
            lastLarge = largeMotor;
        }

        PadInputState pad = []
        {
            PadInputState state;
            state.connected = true;
            return state;
        }();
        uint8_t lastSmall = 0u;
        uint8_t lastLarge = 0u;
    };

    bool expect(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "FAIL: " << message << '\n';
        return condition;
    }

    std::vector<uint8_t> exchange(VirtualDualShock2 &pad, std::vector<uint8_t> command, const PadInputState &input)
    {
        std::vector<uint8_t> response(command.size());
        (void)pad.transfer(command, response, input);
        return response;
    }

    bool testDigitalPoll()
    {
        VirtualDualShock2 pad;
        PadInputState input;
        input.connected = true;
        input.buttons = PadButton::Cross | PadButton::Start;
        const auto response = exchange(pad, {0x01, 0x42, 0x00, 0x00, 0x00}, input);
        return expect(response == std::vector<uint8_t>{0xFF, 0x41, 0x5A, 0xF7, 0xBF}, "digital poll reply") &&
               expect(exchange(pad, {0x81, 0x42, 0x00}, input) == std::vector<uint8_t>{0xFF, 0xFF, 0xFF}, "memory card address answered by pad") &&
               expect(exchange(pad, {0x01, 0x45, 0x00, 0x00, 0x00}, input)[1] == 0xFF, "config-only command answered outside config");
    }

    bool testAnalogConfiguration()
    {
        VirtualDualShock2 pad;
        PadInputState input;
        input.connected = true;
        input.leftX = 0x00u;
        input.rightY = 0xFFu;
        input.buttons = PadButton::Triangle;
        input.pressure[static_cast<size_t>(PadPressure::Triangle)] = 0xC0u;

        (void)exchange(pad, {0x01, 0x43, 0x00, 0x01, 0x00}, input);
        if (!expect(pad.inConfig() && pad.modeId() == 0xF3u, "enter config"))
            return false;
        const auto model = exchange(pad, {0x01, 0x45, 0x00, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A}, input);
        if (!expect(model == std::vector<uint8_t>{0xFF, 0xF3, 0x5A, 0x03, 0x02, 0x00, 0x02, 0x01, 0x00}, "digital model query"))
            return false;
        (void)exchange(pad, {0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00}, input);
        (void)exchange(pad, {0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}, input);
        const auto act = exchange(pad, {0x01, 0x46, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}, input);
        if (!expect(act == std::vector<uint8_t>{0xFF, 0xF3, 0x5A, 0x00, 0x00, 0x01, 0x01, 0x01, 0x14}, "actuator query selector 1"))
            return false;
        (void)exchange(pad, {0x01, 0x43, 0x00, 0x00, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A}, input);
        if (!expect(!pad.inConfig() && pad.modeId() == 0x73u, "exit config into analog"))
            return false;

        const auto poll = exchange(pad, {0x01, 0x42, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00}, input);
        if (!expect(poll == std::vector<uint8_t>{0xFF, 0x73, 0x5A, 0xFF, 0xEF, 0x80, 0xFF, 0x00, 0x80}, "analog poll reply") ||
            !expect(pad.smallMotor() == 0x01u && pad.largeMotor() == 0x80u, "vibration map applied"))
            return false;

        (void)exchange(pad, {0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}, input);
        (void)exchange(pad, {0x01, 0x4F, 0x00, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0x00}, input);
        (void)exchange(pad, {0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, input);
        std::vector<uint8_t> full(21, 0x00);
        full[0] = 0x01;
        full[1] = 0x42;
        const auto pressure = exchange(pad, full, input);
        return expect(pad.modeId() == 0x79u, "pressure mask selects DualShock 2 mode") &&
               expect(pressure[1] == 0x79u && pressure[2] == 0x5Au, "DualShock 2 poll header") &&
               expect(pressure[9 + static_cast<size_t>(PadPressure::Triangle)] == 0xC0u, "triangle pressure byte");
    }

    uint32_t send3(uint32_t port, uint32_t length)
    {
        return port | (length << 8u) | (length << 18u);
    }

    bool testSio2PioTransfer()
    {
        PadHost host;
        host.pad.buttons = PadButton::Circle;
        IopMemory memory;
        IopSio2 sio2(host, memory);
        memory.attachSio2(&sio2);

        memory.write32(0xBF808268u, 0x3BCu);
        memory.write32(0xBF808200u, send3(0u, 5u));
        memory.write32(0xBF808204u, 0u);
        for (const uint8_t byte : std::array<uint8_t, 5>{0x01, 0x42, 0x00, 0x00, 0x00})
            memory.write8(0xBF808260u, byte);
        memory.write32(0xBF808268u, 0x3B1u);

        const auto start = memory.takeDmaStart();
        if (!expect(start && start->irq == IopSio2::kIrq, "transfer completion raises SIO2 interrupt") ||
            !expect(memory.read32(0xBF80826Cu) == 0x1100u, "RECV1 reports a connected pad") ||
            !expect((memory.read32(0xBF808268u) & 1u) == 0u, "start bit self-clears"))
            return false;

        std::vector<uint8_t> out;
        for (int i = 0; i < 5; ++i)
            out.push_back(memory.read8(0xBF808264u));
        if (!expect(out == std::vector<uint8_t>{0xFF, 0x41, 0x5A, 0xFF, 0xDF}, "DATAOUT carries the pad reply"))
            return false;

        memory.write32(0xBF808200u, send3(1u, 5u));
        for (const uint8_t byte : std::array<uint8_t, 5>{0x01, 0x42, 0x00, 0x00, 0x00})
            memory.write8(0xBF808260u, byte);
        memory.write32(0xBF808268u, 0x3B1u);
        return expect(memory.read32(0xBF80826Cu) == 0x1D100u, "RECV1 reports no device on an empty port") &&
               expect(memory.read8(0xBF808264u) == 0xFFu, "empty port floats high");
    }

    bool testSio2DmaTransfer()
    {
        PadHost host;
        IopMemory memory;
        IopSio2 sio2(host, memory);
        memory.attachSio2(&sio2);

        constexpr uint32_t kIn = 0x00100000u;
        constexpr uint32_t kOut = 0x00100100u;
        const std::array<uint8_t, 8> command{0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (!expect(memory.writeRam(kIn, command.data(), command.size()), "stage DMA input"))
            return false;

        memory.write32(0xBF808200u, send3(0u, 5u));
        memory.write32(0xBF808204u, 0u);
        if (!expect(sio2.dmaRequest(IopSio2::kDmaIn, kIn, 2u, 1u, 1u), "channel 11 request") ||
            !expect(sio2.dmaRequest(IopSio2::kDmaOut, kOut, 2u, 1u, 0u), "channel 12 request") ||
            !expect(!sio2.dmaRequest(IopSio2::kDmaOut, kOut, 2u, 1u, 1u), "channel 12 rejects memory-to-device"))
            return false;
        (void)sio2.dmaTransfer(IopSio2::kDmaIn);
        (void)sio2.dmaTransfer(IopSio2::kDmaOut);
        memory.write32(0xBF808268u, 0x3B1u);

        std::array<uint8_t, 8> out{};
        return expect(memory.readRam(kOut, out.data(), out.size()), "read DMA output") &&
               expect(out == std::array<uint8_t, 8>{0xFF, 0x41, 0x5A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, "channel 12 receives the reply");
    }

    bool testSio2DmaBlocks()
    {
        PadHost host;
        IopMemory memory;
        IopSio2 sio2(host, memory);
        memory.attachSio2(&sio2);

        constexpr uint32_t kIn = 0x00100000u;
        constexpr uint32_t kOut = 0x00100100u;
        const std::array<uint8_t, 16> commands{0x01, 0x42, 0x00, 0x00, 0x00, 0xEE, 0xEE, 0xEE,
                                               0x01, 0x42, 0x00, 0x00, 0x00, 0xEE, 0xEE, 0xEE};
        if (!expect(memory.writeRam(kIn, commands.data(), commands.size()), "stage block DMA input"))
            return false;

        memory.write32(0xBF808200u, send3(0u, 5u));
        memory.write32(0xBF808204u, send3(0u, 5u));
        memory.write32(0xBF808208u, 0u);
        if (!expect(sio2.dmaRequest(IopSio2::kDmaIn, kIn, 2u, 2u, 1u), "channel 11 block request") ||
            !expect(sio2.dmaRequest(IopSio2::kDmaOut, kOut, 2u, 2u, 0u), "channel 12 block request"))
            return false;
        (void)sio2.dmaTransfer(IopSio2::kDmaIn);
        (void)sio2.dmaTransfer(IopSio2::kDmaOut);
        memory.write32(0xBF808268u, 0x3B1u);

        std::array<uint8_t, 16> out{};
        const std::array<uint8_t, 16> expected{0xFF, 0x41, 0x5A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                               0xFF, 0x41, 0x5A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        return expect(memory.readRam(kOut, out.data(), out.size()), "read block DMA output") &&
               expect(out == expected, "each SEND3 command uses its own DMA block, padding is skipped");
    }

    std::vector<uint8_t> cardExchange(VirtualMemoryCard &card, std::vector<uint8_t> command)
    {
        std::vector<uint8_t> response(command.size(), 0xFFu);
        (void)card.transfer(command, response);
        return response;
    }

    bool testMemoryCard()
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "ps2x_sio2_test_card.ps2";
        std::error_code error;
        std::filesystem::remove(path, error);
        bool ok = true;
        {
            VirtualMemoryCard card;
            card.attach(path, true);
            ok = expect(card.present(), "card created") &&
                 expect(std::filesystem::file_size(path) == VirtualMemoryCard::kImageSize, "card image size") &&
                 expect(cardExchange(card, {0x81, 0x11, 0x00, 0x00}) == std::vector<uint8_t>{0x00, 0x00, 0x2B, 0x55}, "probe") &&
                 expect(cardExchange(card, {0x81, 0x26, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) ==
                            std::vector<uint8_t>{0x00, 0x00, 0x2B, 0x00, 0x02, 0x10, 0x00, 0x00, 0x40, 0x00, 0x00, 0x52, 0x55},
                        "specs");
            if (ok)
            {
                (void)cardExchange(card, {0x81, 0x21, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00});
                (void)cardExchange(card, {0x81, 0x82, 0x00, 0x00});
                (void)cardExchange(card, {0x81, 0x22, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00});
                const auto written = cardExchange(card, {0x81, 0x42, 0x04, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00});
                (void)cardExchange(card, {0x81, 0x23, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00});
                const auto read = cardExchange(card, {0x81, 0x43, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
                ok = expect(written == std::vector<uint8_t>{0x00, 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x08, 0x55}, "write reply") &&
                     expect(read == std::vector<uint8_t>{0x00, 0x00, 0x00, 0x2B, 0x12, 0x34, 0x56, 0x78, 0x08, 0x55}, "read back");
            }
        }
        std::filesystem::remove(path, error);
        return ok;
    }
}

int main()
{
    if (!testDigitalPoll() || !testAnalogConfiguration() || !testSio2PioTransfer() || !testSio2DmaTransfer() || !testSio2DmaBlocks() || !testMemoryCard())
        return 1;
    std::cout << "ps2xIOP SIO2 tests passed\n";
    return 0;
}
