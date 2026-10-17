cmake_minimum_required(VERSION 3.21)

project(ps2xTest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Only the offline tools the port uses (upstream's unit tests and studio function table are left out, see
# runtime/README.md): vu1_recomp (VU1 microcode AOT into generated/vu1), gs_replay and vu0_audit, all on the reference
# GS/VU interpreter in gs_reference/.

add_library(ps2_gs_reference STATIC
    gs_reference/gs_reference_backend.cpp
    gs_reference/gs_golden.cpp
)

target_include_directories(ps2_gs_reference PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/gs_reference
    ${CMAKE_CURRENT_LIST_DIR}/../ps2xRuntime/include
)

target_link_libraries(ps2_gs_reference PUBLIC ps2_runtime)

if(MSVC)
    set_source_files_properties(gs_reference/gs_reference_backend.cpp PROPERTIES COMPILE_OPTIONS "/fp:precise")
endif()

add_executable(gs_replay tools/gs_replay.cpp)

set(PS2X_VU0_AUDIT_CORPUS "" CACHE FILEPATH "Generated VU0 macro corpus for the vu0_audit tool")
if(PS2X_VU0_AUDIT_CORPUS)
    add_executable(vu0_audit tools/vu0_audit.cpp)
    target_link_libraries(vu0_audit PRIVATE ps2_gs_reference)
    target_compile_definitions(vu0_audit PRIVATE "VU0_CORPUS=\"${PS2X_VU0_AUDIT_CORPUS}\"")
endif()

add_executable(vu1_recomp tools/vu1_recomp.cpp)
target_link_libraries(vu1_recomp PRIVATE ps2_gs_reference)
target_include_directories(vu1_recomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../ps2xRuntime/src/lib/vu)
target_link_libraries(gs_replay PRIVATE ps2_gs_reference)
if(MSVC)
    target_compile_options(gs_replay PRIVATE /Zi)
    target_link_options(gs_replay PRIVATE /DEBUG)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../ps2xRuntime/cmake/ReleaseMode.cmake")

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_gs_reference)
    EnableFastReleaseMode(gs_replay)
endif()
