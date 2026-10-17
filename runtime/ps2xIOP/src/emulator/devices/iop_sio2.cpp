#include "iop_sio2.h"

#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kSend3 = 0x1F808200u;
        constexpr uint32_t kPortControl = 0x1F808240u;
        constexpr uint32_t kDataIn = 0x1F808260u;
        constexpr uint32_t kDataOut = 0x1F808264u;
        constexpr uint32_t kControl = 0x1F808268u;
        constexpr uint32_t kRecv1 = 0x1F80826Cu;
        constexpr uint32_t kRecv2 = 0x1F808270u;
        constexpr uint32_t kRecv3 = 0x1F808274u;
        constexpr uint32_t kReg278 = 0x1F808278u;
        constexpr uint32_t kReg27C = 0x1F80827Cu;
        constexpr uint32_t kInterruptStatus = 0x1F808280u;

        constexpr uint32_t kControlStart = 0x1u;
        constexpr uint32_t kControlReset = 0xCu;
        constexpr uint32_t kRecv1Connected = 0x1100u;
        constexpr uint32_t kRecv1NoResponse = 0x1D100u;
        constexpr uint32_t kRecv2Default = 0xFu;
        constexpr uint32_t kInterruptTransferDone = 0x1u;

        constexpr uint32_t kSend3Port = 0x3u;
        constexpr uint32_t kSend3LengthMask = 0x1FFu;
        constexpr uint32_t kSend3TxShift = 8u;
        constexpr uint32_t kSend3RxShift = 18u;

        constexpr uint32_t kDmaToDevice = 1u;
        constexpr uint64_t kCompletionBaseCycles = 1'000u;
        constexpr uint64_t kCyclesPerByte = 1'200u;
        constexpr uint64_t kCardCyclesPerByte = 150u;
    }

    IopSio2::IopSio2(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    void IopSio2::reset()
    {
        m_send3.fill(0u);
        m_portControl.fill(0u);
        m_control = 0u;
        m_recv1 = 0u;
        m_recv2 = 0u;
        m_recv3 = 0u;
        m_reg278 = 0u;
        m_reg27C = 0u;
        m_interruptStatus = 0u;
        m_fifoIn.clear();
        m_fifoOut.clear();
        m_dmaIn = {};
        m_dmaOut = {};
        for (VirtualDualShock2 &pad : m_pads)
            pad.reset();
        m_smallMotor.fill(0u);
        m_largeMotor.fill(0u);
        m_reportedMode.fill(0u);
        for (VirtualMemoryCard &card : m_cards)
            card.reset();
        m_completion.reset();
        m_stats = {};
        // PS2X_MULTITAP: 0 = none (default), 1 = controller port 1, 2 = port 2, 12 = both.
        const char *multitap = std::getenv("PS2X_MULTITAP");
        const std::string ports = multitap ? multitap : "0";
        m_multitapPorts = (ports.find('1') != std::string::npos ? 1u : 0u) | (ports.find('2') != std::string::npos ? 2u : 0u);
        m_padSlot.fill(0u);
        m_cardSlot.fill(0u);
        for (auto &port : m_tapPads)
            for (VirtualDualShock2 &pad : port)
                pad.reset();
        for (auto &port : m_tapSmallMotor)
            port.fill(0u);
        for (auto &port : m_tapLargeMotor)
            port.fill(0u);
        for (auto &port : m_tapReportedMode)
            port.fill(0u);
        if (m_multitapPorts != 0u)
        {
            std::ostringstream message;
            message << "[SIO2] multitap on port" << ((m_multitapPorts & 1u) ? " 1" : "") << ((m_multitapPorts & 2u) ? " 2" : "");
            m_host.log(LogLevel::Info, message.str());
        }
    }

    // Multitap commands (first byte 0x21), as PCSX2's MultitapProtocol: 0x12/0x13 ask whether pads/memory cards are
    // supported, 0x21/0x22 select the pad/memory card slot the following commands on this port go to.
    size_t IopSio2::multitapCommand(uint32_t port, const std::vector<uint8_t> &command, std::vector<uint8_t> &response)
    {
        if ((m_multitapPorts & (1u << port)) == 0u || command.size() < 2u)
            return 0u;
        std::fill(response.begin(), response.end(), uint8_t{0u});
        response[0] = 0x00u;
        response[1] = 0x80u;
        auto put = [&](size_t index, uint8_t value)
        {
            if (index < response.size())
                response[index] = value;
        };
        switch (command[1])
        {
        case 0x12u: // pad support check
        case 0x13u: // memory card support check
            put(2u, 0x5Au);
            put(3u, 0x04u);
            put(4u, 0x00u);
            put(5u, 0x5Au);
            break;
        case 0x21u: // select pad slot
        case 0x22u: // select memory card slot
        {
            const uint8_t slot = command.size() > 2u ? command[2] : 0xFFu;
            const bool valid = slot < kSlots;
            if (valid)
                (command[1] == 0x21u ? m_padSlot : m_cardSlot)[port] = slot;
            put(2u, 0x5Au);
            put(3u, 0x00u);
            put(4u, 0x00u);
            put(5u, valid ? slot : 0xFFu);
            put(6u, valid ? 0x5Au : 0x66u);
            break;
        }
        default:
            break;
        }
        return response.size();
    }

    size_t IopSio2::padCommand(uint32_t port, uint32_t slot, const std::vector<uint8_t> &command, std::vector<uint8_t> &response)
    {
        VirtualDualShock2 &pad = slot == 0u ? m_pads[port] : m_tapPads[port][slot - 1u];
        uint8_t &reportedMode = slot == 0u ? m_reportedMode[port] : m_tapReportedMode[port][slot - 1u];
        uint8_t &smallMotor = slot == 0u ? m_smallMotor[port] : m_tapSmallMotor[port][slot - 1u];
        uint8_t &largeMotor = slot == 0u ? m_largeMotor[port] : m_tapLargeMotor[port][slot - 1u];
        PadInputState state;
        const bool connected = m_host.readPadSlot(port, slot, state) && state.connected;
        if (!connected)
        {
            pad.reset();
            reportedMode = 0u;
            return 0u;
        }
        const size_t acknowledged = pad.transfer(command, response, state);
        ++m_stats.padCommands;
        // PS2X_PAD_RATE=1: pad commands per second and the last command byte, once a second (input after loads).
        static const bool rate = [] { const char *v = std::getenv("PS2X_PAD_RATE"); return v && *v == '1'; }();
        if (rate)
        {
            static uint64_t count = 0u;
            static auto windowStart = std::chrono::steady_clock::now();
            ++count;
            const auto now = std::chrono::steady_clock::now();
            if (now - windowStart >= std::chrono::seconds(1))
            {
                std::ostringstream message;
                message << "[SIO2] pad commands/s=" << count << " last=0x" << std::hex
                        << static_cast<uint32_t>(command.size() > 1u ? command[1] : 0u) << " buttons=0x" << state.buttons;
                m_host.log(LogLevel::Info, message.str());
                count = 0u;
                windowStart = now;
            }
        }
        const uint8_t mode = pad.modeId();
        if (mode != reportedMode)
        {
            std::ostringstream message;
            message << "[SIO2] pad " << port;
            if (slot != 0u)
                message << static_cast<char>('A' + slot);
            message << " mode 0x" << std::hex << static_cast<uint32_t>(mode);
            m_host.log(LogLevel::Info, message.str());
            reportedMode = mode;
        }
        if (pad.smallMotor() != smallMotor || pad.largeMotor() != largeMotor)
        {
            smallMotor = pad.smallMotor();
            largeMotor = pad.largeMotor();
            m_host.padVibrationSlot(port, slot, smallMotor, largeMotor);
        }
        return acknowledged;
    }

    uint32_t IopSio2::read32(uint32_t address)
    {
        if (address >= kSend3 && address < kPortControl)
            return m_send3[(address - kSend3) / 4u];
        if (address >= kPortControl && address < kDataIn)
            return m_portControl[(address - kPortControl) / 4u];
        switch (address)
        {
        case kDataOut:
        {
            if (m_fifoOut.empty())
                return 0u;
            const uint8_t value = m_fifoOut.front();
            m_fifoOut.pop_front();
            return value;
        }
        case kControl:
            return m_control;
        case kRecv1:
            return m_recv1;
        case kRecv2:
            return m_recv2;
        case kRecv3:
            return m_recv3;
        case kReg278:
            return m_reg278;
        case kReg27C:
            return m_reg27C;
        case kInterruptStatus:
            return m_interruptStatus;
        default:
            return 0u;
        }
    }

    void IopSio2::write32(uint32_t address, uint32_t value)
    {
        if (address >= kSend3 && address < kPortControl)
        {
            m_send3[(address - kSend3) / 4u] = value;
            return;
        }
        if (address >= kPortControl && address < kDataIn)
        {
            m_portControl[(address - kPortControl) / 4u] = value;
            return;
        }
        switch (address)
        {
        case kDataIn:
            m_fifoIn.push_back(static_cast<uint8_t>(value));
            return;
        case kControl:
            if ((value & kControlReset) != 0u)
            {
                m_fifoIn.clear();
                m_fifoOut.clear();
            }
            m_control = value & ~(kControlReset | kControlStart);
            if ((value & kControlStart) != 0u)
                startTransfer();
            return;
        case kReg278:
            m_reg278 = value;
            return;
        case kReg27C:
            m_reg27C = value;
            return;
        case kInterruptStatus:
            m_interruptStatus &= ~value;
            return;
        default:
            return;
        }
    }

    bool IopSio2::dmaRequest(uint32_t channel, uint32_t address, uint32_t blockWords, uint32_t blockCount, uint32_t direction)
    {
        if (channel != kDmaIn && channel != kDmaOut)
            return false;
        DmaRequest &request = channel == kDmaIn ? m_dmaIn : m_dmaOut;
        if ((direction == kDmaToDevice) != (channel == kDmaIn))
            return false;
        request.address = IopMemory::physicalAddress(address);
        request.bytes = blockWords * blockCount * 4u;
        request.blockBytes = blockWords * 4u;
        request.armed = false;
        return true;
    }

    bool IopSio2::dmaTransfer(uint32_t channel)
    {
        if (channel != kDmaIn && channel != kDmaOut)
            return false;
        DmaRequest &request = channel == kDmaIn ? m_dmaIn : m_dmaOut;
        request.armed = request.bytes != 0u;
        return true;
    }

    std::optional<IopSio2::Completion> IopSio2::takeCompletion() noexcept
    {
        std::optional<Completion> result = m_completion;
        m_completion.reset();
        return result;
    }

    void IopSio2::startTransfer()
    {
        std::deque<uint8_t> input;
        input.swap(m_fifoIn);
        std::vector<uint8_t> inputBlocks;
        const bool blockInput = m_dmaIn.armed && m_dmaIn.blockBytes != 0u && m_dmaIn.blockBytes < m_dmaIn.bytes;
        if (m_dmaIn.armed)
        {
            std::vector<uint8_t> bytes(m_dmaIn.bytes);
            if (m_memory.readRam(m_dmaIn.address, bytes.data(), bytes.size()))
            {
                if (blockInput)
                    inputBlocks = std::move(bytes);
                else
                    input.insert(input.end(), bytes.begin(), bytes.end());
            }
            m_dmaIn.armed = false;
        }

        std::vector<uint8_t> output;
        std::vector<std::pair<size_t, size_t>> commandOutputs;
        uint32_t recv1 = kRecv1Connected;
        uint64_t cycles = kCompletionBaseCycles;
        for (size_t index = 0; index < m_send3.size() && m_send3[index] != 0u; ++index)
        {
            if (blockInput)
            {
                const size_t begin = std::min(index * m_dmaIn.blockBytes, inputBlocks.size());
                const size_t end = std::min(begin + m_dmaIn.blockBytes, inputBlocks.size());
                input.assign(inputBlocks.begin() + static_cast<std::ptrdiff_t>(begin), inputBlocks.begin() + static_cast<std::ptrdiff_t>(end));
            }
            const size_t before = output.size();
            runCommand(m_send3[index], input, output, recv1, cycles);
            commandOutputs.emplace_back(before, output.size() - before);
        }

        size_t delivered = 0u;
        if (m_dmaOut.armed)
        {
            std::vector<uint8_t> bytes(m_dmaOut.bytes, uint8_t{0xFFu});
            const size_t block = m_dmaOut.blockBytes;
            if (block != 0u && block < bytes.size() && commandOutputs.size() > 1u)
            {
                for (size_t index = 0; index < commandOutputs.size(); ++index)
                {
                    const size_t begin = index * block;
                    if (begin >= bytes.size())
                        break;
                    const size_t count = std::min({commandOutputs[index].second, block, bytes.size() - begin});
                    std::copy_n(output.begin() + static_cast<std::ptrdiff_t>(commandOutputs[index].first), count,
                                bytes.begin() + static_cast<std::ptrdiff_t>(begin));
                }
                delivered = output.size();
            }
            else
            {
                delivered = std::min(output.size(), bytes.size());
                std::copy_n(output.begin(), delivered, bytes.begin());
            }
            (void)m_memory.writeRam(m_dmaOut.address, bytes.data(), bytes.size());
            m_dmaOut.armed = false;
        }
        m_fifoOut.insert(m_fifoOut.end(), output.begin() + static_cast<std::ptrdiff_t>(delivered), output.end());

        static uint32_t transferTraceLeft = []
        {
            const char *value = std::getenv("PS2X_PAD_TRACE");
            return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
        }();
        if (transferTraceLeft != 0u && !commandOutputs.empty() && (m_send3[0] & kSend3Port) < kPadPorts)
        {
            --transferTraceLeft;
            std::ostringstream message;
            message << "[SIO2] xfer cmds=" << commandOutputs.size() << " dmaIn=0x" << std::hex << m_dmaIn.address << std::dec
                    << "/" << m_dmaIn.bytes << "/" << m_dmaIn.blockBytes << (blockInput ? " blk" : "")
                    << " dmaOut=0x" << std::hex << m_dmaOut.address << std::dec << "/" << m_dmaOut.bytes << "/" << m_dmaOut.blockBytes
                    << " outBytes=" << output.size() << " delivered=" << delivered << " fifoOut=" << m_fifoOut.size()
                    << " recv1=0x" << std::hex << recv1 << " ctrl=0x" << m_control;
            m_host.log(LogLevel::Info, message.str());
        }

        m_recv1 = recv1;
        m_recv2 = kRecv2Default;
        m_recv3 = 0u;
        m_interruptStatus |= kInterruptTransferDone;
        ++m_stats.transfers;
        m_completion = Completion{cycles};
    }

    void IopSio2::attachCards()
    {
        m_cardsAttached = true;
        const std::filesystem::path root = std::filesystem::path(m_host.hostPath(HostPathKind::MemoryCardRoot));
        const char *enabled = std::getenv("PS2X_MEMCARD");
        if (root.empty() || (enabled && *enabled == '0'))
            return;
        const std::filesystem::path folder = root.has_parent_path() ? root.parent_path() : root;
        m_cards[0].attach(folder / "Mcd001.ps2", true);
        m_cards[1].attach(folder / "Mcd002.ps2", false);
        std::ostringstream message;
        message << "[SIO2] memory card slot 1: " << (folder / "Mcd001.ps2").string()
                << (m_cards[0].present() ? "" : " (unavailable)");
        m_host.log(LogLevel::Info, message.str());
    }

    void IopSio2::runCommand(uint32_t send3, std::deque<uint8_t> &input, std::vector<uint8_t> &output, uint32_t &recv1, uint64_t &cycles)
    {
        const uint32_t port = send3 & kSend3Port;
        const size_t tx = (send3 >> kSend3TxShift) & kSend3LengthMask;
        const size_t rx = (send3 >> kSend3RxShift) & kSend3LengthMask;
        const size_t length = std::max(tx, rx);

        std::vector<uint8_t> command(length, uint8_t{0u});
        for (size_t i = 0; i < tx && !input.empty(); ++i)
        {
            command[i] = input.front();
            input.pop_front();
        }
        std::vector<uint8_t> response(length, uint8_t{0xFFu});

        size_t acknowledged = 0u;
        const bool multitapCommandByte = !command.empty() && command[0] == 0x21u;
        if (port < kPadPorts)
        {
            if (multitapCommandByte)
                acknowledged = multitapCommand(port, command, response);
            else
                acknowledged = padCommand(port, (m_multitapPorts & (1u << port)) != 0u ? m_padSlot[port] : 0u, command, response);
            // PS2X_PAD_TRACE=<n>: log the first n pad transfers (command and response bytes).
            static uint32_t traceLeft = []
            {
                const char *value = std::getenv("PS2X_PAD_TRACE");
                return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
            }();
            if (traceLeft != 0u)
            {
                --traceLeft;
                std::ostringstream message;
                message << "[SIO2] pad " << port << " send3=0x" << std::hex << send3 << std::dec << " tx=" << tx << " rx=" << rx
                        << " ack=" << acknowledged << " in=" << std::hex;
                for (size_t i = 0; i < std::min<size_t>(command.size(), 21u); ++i)
                    message << ' ' << static_cast<uint32_t>(command[i]);
                message << " out=";
                for (size_t i = 0; i < std::min<size_t>(response.size(), 21u); ++i)
                    message << ' ' << static_cast<uint32_t>(response[i]);
                m_host.log(LogLevel::Info, message.str());
            }
        }
        else if (port - kPadPorts < kCardPorts)
        {
            if (!m_cardsAttached)
                attachCards();
            const uint32_t cardPort = port - kPadPorts;
            if (multitapCommandByte)
                acknowledged = multitapCommand(cardPort, command, response);
            else if ((m_multitapPorts & (1u << cardPort)) == 0u || m_cardSlot[cardPort] == 0u)
                acknowledged = m_cards[cardPort].transfer(command, response); // multitap slots B-D have no card
            if (acknowledged != 0u)
                ++m_stats.cardCommands;
            static const bool trace = []
            {
                const char *value = std::getenv("PS2X_MC_TRACE");
                return value && *value && *value != '0';
            }();
            if (trace)
            {
                std::ostringstream message;
                message << "[SIO2] card " << (port - kPadPorts) << " tx=" << tx << " rx=" << rx << " ack=" << acknowledged << " in=" << std::hex;
                for (size_t i = 0; i < std::min<size_t>(command.size(), 12u); ++i)
                    message << ' ' << static_cast<uint32_t>(command[i]);
                message << " out=";
                for (size_t i = 0; i < std::min<size_t>(response.size(), 12u); ++i)
                    message << ' ' << static_cast<uint32_t>(response[i]);
                m_host.log(LogLevel::Info, message.str());
            }
        }
        cycles += rx * (port < kPadPorts ? kCyclesPerByte : kCardCyclesPerByte);
        // Experimental (PS2X_SIO2_RECV1): how one unanswered command in a multi-command transfer shows in RECV1.
        // 0 = sticky (default: any miss reports no response), 1 = last command wins (PCSX2), 2 = connected if any answered.
        static const int recv1Mode = []
        {
            const char *value = std::getenv("PS2X_SIO2_RECV1");
            return value ? std::atoi(value) : 0;
        }();
        if (acknowledged == 0u)
        {
            if (recv1Mode != 2 || recv1 != kRecv1Connected || output.empty())
                recv1 = kRecv1NoResponse;
            ++m_stats.unansweredCommands;
        }
        else if (recv1Mode == 1 || recv1Mode == 2)
        {
            recv1 = kRecv1Connected;
        }
        output.insert(output.end(), response.begin(), response.begin() + static_cast<std::ptrdiff_t>(rx));
    }
}
