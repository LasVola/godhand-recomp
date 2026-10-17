#pragma once

#include "iop_dualshock2.h"
#include "iop_memcard.h"
#include "ps2x/state_io.h"

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    class IopMemory;

    class IopSio2
    {
    public:
        static constexpr uint32_t kBase = 0x1F808200u;
        static constexpr uint32_t kEnd = 0x1F808284u;
        static constexpr int kIrq = 17;
        static constexpr uint32_t kDmaIn = 11u;
        static constexpr uint32_t kDmaOut = 12u;
        static constexpr uint32_t kPadPorts = 2u;
        static constexpr uint32_t kCardPorts = 2u;
        // Multitap (SCPH-10090): four slots per port. Slot A is m_pads/m_cards; B-D live in the multitap state below.
        static constexpr uint32_t kSlots = 4u;

        struct Completion
        {
            uint64_t delayCycles = 0u;
        };

        struct Stats
        {
            uint64_t transfers = 0u;
            uint64_t padCommands = 0u;
            uint64_t cardCommands = 0u;
            uint64_t unansweredCommands = 0u;
        };

        IopSio2(IopHost &host, IopMemory &memory) noexcept;

        void reset();

        [[nodiscard]] static bool owns(uint32_t physicalAddress) noexcept
        {
            return physicalAddress >= kBase && physicalAddress < kEnd;
        }

        [[nodiscard]] uint32_t read32(uint32_t physicalAddress);
        void write32(uint32_t physicalAddress, uint32_t value);

        bool dmaRequest(uint32_t channel, uint32_t address, uint32_t blockWords, uint32_t blockCount, uint32_t direction);
        bool dmaTransfer(uint32_t channel);

        [[nodiscard]] std::optional<Completion> takeCompletion() noexcept;
        [[nodiscard]] const Stats &stats() const noexcept { return m_stats; }
        [[nodiscard]] const VirtualDualShock2 &pad(uint32_t port) const noexcept { return m_pads[port]; }

        template <typename Ar>
        void serialize(Ar &ar)
        {
            ps2x::state::fields(ar, m_send3, m_portControl, m_control, m_recv1, m_recv2, m_recv3, m_reg278, m_reg27C, m_interruptStatus,
                                m_fifoIn, m_fifoOut, m_dmaIn, m_dmaOut, m_pads, m_smallMotor, m_largeMotor, m_reportedMode, m_completion,
                                m_stats, m_cards);
        }

        // Multitap state, in its own (optional) save-state section so states from before it still load.
        template <typename Ar>
        void serializeMultitap(Ar &ar)
        {
            ps2x::state::fields(ar, m_padSlot, m_cardSlot, m_tapPads, m_tapSmallMotor, m_tapLargeMotor, m_tapReportedMode);
        }

        // Ports with a multitap (bit 0 = controller port 1, bit 1 = port 2), from PS2X_MULTITAP at reset.
        [[nodiscard]] uint32_t multitapPorts() const noexcept { return m_multitapPorts; }

    private:
        struct DmaRequest
        {
            uint32_t address = 0u;
            uint32_t bytes = 0u;
            uint32_t blockBytes = 0u;
            bool armed = false;
        };

        void startTransfer();
        void runCommand(uint32_t send3, std::deque<uint8_t> &input, std::vector<uint8_t> &output, uint32_t &recv1, uint64_t &cycles);
        void attachCards();
        size_t multitapCommand(uint32_t port, const std::vector<uint8_t> &command, std::vector<uint8_t> &response);
        size_t padCommand(uint32_t port, uint32_t slot, const std::vector<uint8_t> &command, std::vector<uint8_t> &response);

        IopHost &m_host;
        IopMemory &m_memory;
        std::array<uint32_t, 16> m_send3{};
        std::array<uint32_t, 8> m_portControl{};
        uint32_t m_control = 0u;
        uint32_t m_recv1 = 0u;
        uint32_t m_recv2 = 0u;
        uint32_t m_recv3 = 0u;
        uint32_t m_reg278 = 0u;
        uint32_t m_reg27C = 0u;
        uint32_t m_interruptStatus = 0u;
        std::deque<uint8_t> m_fifoIn;
        std::deque<uint8_t> m_fifoOut;
        DmaRequest m_dmaIn;
        DmaRequest m_dmaOut;
        std::array<VirtualDualShock2, kPadPorts> m_pads{};
        std::array<uint8_t, kPadPorts> m_smallMotor{};
        std::array<uint8_t, kPadPorts> m_largeMotor{};
        std::array<uint8_t, kPadPorts> m_reportedMode{};
        std::array<VirtualMemoryCard, kCardPorts> m_cards{};
        bool m_cardsAttached = false;
        uint32_t m_multitapPorts = 0u;
        std::array<uint8_t, kPadPorts> m_padSlot{};
        std::array<uint8_t, kPadPorts> m_cardSlot{};
        std::array<std::array<VirtualDualShock2, kSlots - 1u>, kPadPorts> m_tapPads{};
        std::array<std::array<uint8_t, kSlots - 1u>, kPadPorts> m_tapSmallMotor{};
        std::array<std::array<uint8_t, kSlots - 1u>, kPadPorts> m_tapLargeMotor{};
        std::array<std::array<uint8_t, kSlots - 1u>, kPadPorts> m_tapReportedMode{};
        std::optional<Completion> m_completion;
        Stats m_stats;
    };
}
