#pragma once

#include "ps2x/iop/iop_pad.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ps2_host_input
{
    constexpr uint32_t kPadPorts = 2u;
    // Two controller ports, each with a multitap (PS2X_MULTITAP): up to 8 players.
    constexpr uint32_t kMaxPlayers = 8u;

    void setFieldCounter(const std::atomic<uint64_t> *counter);
    void sample();
    void releaseMouse();
    // The pad at a controller port and multitap slot (0 = A, the pad plugged straight in).
    [[nodiscard]] ps2x::iop::PadInputState padState(uint32_t port, uint32_t slot = 0u);
    // The motors the game set on that pad (any thread); played on the controllers of its LargeMotor/SmallMotor bindings.
    void setVibration(uint32_t port, uint32_t slot, uint8_t smallMotor, uint8_t largeMotor);
    // While blocked (the in-game menu is open) every pad stays plugged in but idle, so the game keeps running.
    void setBlocked(bool blocked);

    // Controls screen (F1, ps2_controls_screen.cpp; window thread). Open: every pad idle but plugged in. Closed again:
    // each player stays idle until its own buttons and sticks are released (at most 2 s).
    void setOverlayOpen(bool open);
    [[nodiscard]] bool overlayOpen();
    [[nodiscard]] bool inputHeld(uint32_t player);

    // The bindings model behind the Controls screen (window thread only). Keys are PCSX2's [PadN] keys ("Cross",
    // "LUp"), bindings PCSX2 strings ("Keyboard/K", "SDL-0/FaceSouth", "Pointer-0/LeftButton").
    namespace controls
    {
        enum class Kind
        {
            Keys,       // one keyboard key or mouse button
            Controller, // one controller button or axis half
            Other       // a chord or a binding this build cannot read (kept as written)
        };
        [[nodiscard]] const std::string &path(); // empty: no file, edits last until the game closes
        [[nodiscard]] bool dirty();
        [[nodiscard]] const std::string &fileError(); // why the file could not be read (empty: it was read)
        [[nodiscard]] bool envOverride(uint32_t pad); // PS2X_PAD<n> in the environment wins over the file
        [[nodiscard]] std::vector<std::string> bindings(uint32_t pad, std::string_view key);
        void setBindings(uint32_t pad, std::string_view key, std::vector<std::string> list); // live at once, unsaved
        void resetDefaults(uint32_t pad);
        // Writes the file when something changed (tmp + replacing rename, retried); false = still unsaved.
        bool save(std::string &message);
        [[nodiscard]] Kind kindOf(std::string_view binding);
        [[nodiscard]] bool sameBinding(std::string_view a, std::string_view b);
        [[nodiscard]] std::string displayName(std::string_view binding);
        [[nodiscard]] std::string keyBinding(int raylibKey); // empty: not bindable (Esc, F-keys, unnamed keys)
        [[nodiscard]] const std::vector<int> &bindableKeys();
        [[nodiscard]] std::string mouseBinding(int button);
        [[nodiscard]] std::string padButtonBinding(int device, int button);
        [[nodiscard]] std::string padAxisBinding(int device, int axis, bool negative);
        // Tests: whether the live table holds that binding; a file's bindings for one key; the default layout.
        [[nodiscard]] bool liveBinding(uint32_t pad, std::string_view key, std::string_view binding);
        bool fileBindings(const std::string &file, uint32_t pad, std::string_view key, std::vector<std::string> &out, std::string &error);
        [[nodiscard]] std::string defaultsDump(uint32_t pad);
    }
}
