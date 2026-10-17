# The PS2Recomp parts the port uses (runtime/README.md). Replaces upstream's top-level CMakeLists.txt, which also
# builds the analyzer, the studio, the unit tests and the Android/Vita targets.

if(URBAN_BUILD_RECOMP)
    add_subdirectory(ps2xRecomp)
endif()

add_subdirectory(ps2xIOP)
add_subdirectory(ps2xRuntime)

if(URBAN_BUILD_TOOLS)
    add_subdirectory(ps2xTest)
endif()
