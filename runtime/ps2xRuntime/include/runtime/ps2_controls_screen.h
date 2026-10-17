#pragma once

// Controls screen (F1, or Back+Start on a controller): rebinds player 1's keyboard, mouse and controller in game, like
// Tokyo Jungle's. Drawn by the host over the game image (the game never sees it); while it is open every pad is idle,
// and after it closes each player stays idle until released. Edits go to the controls file (ps2_input_options.h
// setControlsFile) on close. Everything here runs on the window thread.
namespace ps2x::controls
{
    [[nodiscard]] bool isOpen() noexcept;
    // Once per host frame, before ps2_host_input::sample(). drawable = false (PCSX2's GS covers this window): the
    // screen does not open, so input is never blocked behind an invisible screen.
    void hostTick(bool drawable);
    // Inside BeginDrawing, after the game image.
    void draw();
    // After the run loop (window closed or the run stopped): closes the screen, saving unsaved edits.
    void shutdown();
}
