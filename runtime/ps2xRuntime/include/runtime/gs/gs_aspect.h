#pragma once

// The aspect ratio of the picture while the game runs (PCSX2's GS only; nothing with the built-in GS).
// Its own header so that changing it does not rebuild the generated code.
namespace ps2x::gs
{
    enum class Aspect
    {
        R4_3,
        R16_9,
        Stretch, // fill the window
    };

    void setAspect(Aspect aspect);
}
