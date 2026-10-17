// ImGui configuration for PCSX2's GS in the port (IMGUI_USER_CONFIG): PCSX2's imconfig.h (it has #pragma once, so
// imgui.h's own include of it is then a no-op), with ImGui's built-in stb_truetype font loader instead of FreeType,
// which is not built here.
#pragma once

#include "imconfig.h"

#undef IMGUI_ENABLE_FREETYPE
#define IMGUI_ENABLE_STB_TRUETYPE
