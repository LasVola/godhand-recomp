#pragma once

#include <cstdint>
#include <vector>

namespace ps2_host_display
{
    void setAspect(float aspect);
    float aspect();
    void setFitWindow(bool enabled);
    void setStartMaximized(bool enabled);
    // Borderless fullscreen at start (like F11).
    void setStartFullscreen(bool enabled);
    // Game frames for the FPS overlay (F3): the port calls this once per iteration of the game's main loop.
    void noteGameFrame();
    // A line shown on screen (top left) until replaced or cleared with an empty string, e.g. "capturing profile".
    void setOverlayNote(const char *text);
    // The FPS overlay's latest once-per-second line ("Jogo N FPS | Tela N | Velocidade N% | pior quadro N ms"), and
    // a counter that changes each time it is refreshed.
    const char *fpsLine(unsigned &serial);

    // One record per real present (after the swap): host time (steady_clock, seconds since its epoch), the emulated
    // field (vsync tick), the GS image sequence shown (a new image or the previous one again) and the game-frame count.
    struct PresentRecord
    {
        double hostSeconds;
        uint64_t field;
        uint64_t imageSequence;
        uint64_t gameFrames;
    };
    // Copies the records made at or after `sinceHostSeconds` (a ring of the last 65536) into `out`.
    void copyPresents(double sinceHostSeconds, std::vector<PresentRecord> &out);
}
