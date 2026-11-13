cmake_minimum_required(VERSION 3.20)
project(OrtaRecomp VERSION 0.1.5 LANGUAGES C CXX)

# Orta Recomp: a static recompilation of Panzer Dragoon Orta (Xbox).
#
# The executable always builds. When generated code is present in PDO_GEN_DIR
# (tools/recomp.py writes it there from your own default.xbe), it is compiled
# in together with the xboxrecomp kernel layer. Without it you get the
# launcher alone, which is enough to test the game-file setup.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PDO_RELEASE "A release build: no console window on Windows (tools/package.py)" OFF)

# The C runtime linked in, so the exe needs no Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(PDO_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen" CACHE PATH
    "Directory holding the generated C from tools/recomp.py")

# ── SDL3 (window, dialogs, file I/O, later rendering/audio/input) ────────

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
# Our changes to SDL (patches/SDL/), kept here only: the Vulkan objects
# behind SDL's GPU device, for frame generation (lsfg_present.cpp).
# Applied once; re-running configure finds them already in place.
find_package(Git QUIET)
file(GLOB PDO_SDL_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/SDL/*.patch")
list(SORT PDO_SDL_PATCHES)
foreach(patch ${PDO_SDL_PATCHES})
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --reverse --check "${patch}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/external/SDL"
        RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
    if(NOT already EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply "${patch}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/external/SDL"
            RESULT_VARIABLE failed)
        if(failed)
            message(FATAL_ERROR "Could not apply ${patch} to external/SDL. "
                "Reset it with: git -C external/SDL checkout -- .")
        endif()
        message(STATUS "Orta Recomp: applied ${patch}")
    endif()
endforeach()
add_subdirectory(external/SDL EXCLUDE_FROM_ALL)

# ── Launcher ─────────────────────────────────────────────────────────────

add_executable(OrtaRecomp
    src/main.c
    src/config.c
    src/input_map.c
    src/assets.c
    src/setup_ui.c
    src/xiso.c
    src/xbe.c
    src/sha256.c
)
target_include_directories(OrtaRecomp PRIVATE src)
target_link_libraries(OrtaRecomp PRIVATE SDL3::SDL3-static)
target_compile_definitions(OrtaRecomp PRIVATE PDO_VERSION="${PROJECT_VERSION}")
if(WIN32)
    enable_language(RC)
    target_sources(OrtaRecomp PRIVATE icons/app.rc)
    # A console during development, so logs and --check output show;
    # releases have none (--log still writes the log).
    if(PDO_RELEASE)
        set_target_properties(OrtaRecomp PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
endif()
if(PDO_RELEASE)
    target_compile_definitions(OrtaRecomp PRIVATE PDO_RELEASE=1)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The C++ runtime (frame generation) linked in, so the binary needs
        # only the C library, as before (SteamOS and older distributions).
        target_link_options(OrtaRecomp PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(MSVC)
        # Function pointers to the bodies, not incremental-link thunks: the
        # crash report names game functions from them.
        target_link_options(OrtaRecomp PRIVATE /INCREMENTAL:NO)
    endif()
endif()

if(MSVC)
    target_compile_options(OrtaRecomp PRIVATE /W4 /wd4100)
    target_compile_definitions(OrtaRecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(OrtaRecomp PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ── Recompiled game code (optional until you have generated it) ──────────

if(EXISTS "${PDO_GEN_DIR}/recomp_dispatch.c")
    message(STATUS "Orta Recomp: generated code found in ${PDO_GEN_DIR}")

    # Only the parts of xboxrecomp we use. Its graphics, audio and input
    # libraries are replaced by our own SDL3 ones (later milestones), so they
    # are not built at all.
    set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/xboxrecomp")

    # Fixes we carry until they are merged upstream (patches/xboxrecomp/).
    # Applied once; re-running configure finds them already in place.
    find_package(Git QUIET)
    file(GLOB PDO_XBOXRECOMP_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/xboxrecomp/*.patch")
    list(SORT PDO_XBOXRECOMP_PATCHES)
    foreach(patch ${PDO_XBOXRECOMP_PATCHES})
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check "${patch}"
            WORKING_DIRECTORY "${XBOXRECOMP_DIR}"
            RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
        if(NOT already EQUAL 0)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} apply "${patch}"
                WORKING_DIRECTORY "${XBOXRECOMP_DIR}"
                RESULT_VARIABLE failed)
            if(failed)
                message(FATAL_ERROR "Could not apply ${patch} to external/xboxrecomp. "
                    "Reset it with: git -C external/xboxrecomp checkout -- .")
            endif()
            message(STATUS "Orta Recomp: applied ${patch}")
        endif()
    endforeach()
    add_subdirectory(${XBOXRECOMP_DIR}/src/platform ${CMAKE_BINARY_DIR}/xboxrecomp/platform)
    add_subdirectory(${XBOXRECOMP_DIR}/src/kernel ${CMAKE_BINARY_DIR}/xboxrecomp/kernel)

    file(GLOB PDO_GEN_SOURCES CONFIGURE_DEPENDS "${PDO_GEN_DIR}/*.c")
    # An object library rather than a static one: the kernel layer calls back
    # into the generated dispatch table and our hooks, a circular dependency
    # that single-pass linkers cannot resolve between static archives.
    add_library(pdo_recomp OBJECT ${PDO_GEN_SOURCES}
        src/guest/guest_manual.c
        src/guest/video_hooks.c
        src/guest/crash_report.c
        src/guest/d3d_hle.c
        src/guest/dsound_hle.c
        src/guest/cri_hle.c
        src/guest/input_hle.c
        src/guest/vsh.c
        src/guest/d3d_calltrace.c
        src/guest/wait_trace.c
        src/guest/widescreen.c
        src/guest/unlocks.c
        src/guest/intro_skip.c
        src/guest/launch_guard.c
        src/audio/mixer.c
        src/audio/aix.c
        src/audio/reverb.c
        src/gfx/renderer.c
        src/gfx/overlay.c
        src/gfx/xtex.c)
    target_include_directories(pdo_recomp PUBLIC "${PDO_GEN_DIR}" src)
    target_link_libraries(pdo_recomp PUBLIC xbox_kernel SDL3::SDL3-static)
    if(WIN32)
        target_link_libraries(pdo_recomp PUBLIC dbghelp)
    endif()
    # The vertex program interpreter rounds every operation, as the NV2A
    # does: no fused multiply-add (fp:precise is MSVC's default already).
    if(NOT MSVC)
        set_source_files_properties(src/guest/vsh.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    endif()
    if(MSVC)
        target_compile_options(pdo_recomp PRIVATE /bigobj /w)
    else()
        # The generated code reads guest memory through casted pointers and
        # relies on 32-bit wraparound, so neither assumption may be optimised on.
        target_compile_options(pdo_recomp PRIVATE
            -fno-strict-aliasing -fwrapv -w)
    endif()

    # Lossless Scaling frame generation (lib/lsfg, from BattleTanx: Global
    # Assault Recompiled; the player's own Lossless.dll at run time). Its
    # shader translator needs SPIR-V's headers: the external/SPIRV-Headers
    # submodule (pinned, new enough), else PDO_SPIRV_HEADERS, the Vulkan
    # SDK's or the system's. Without any, the game builds without it.
    set(PDO_SPIRV_HEADERS "" CACHE PATH "Folder holding spirv/unified1/spirv.hpp (SPIRV-Headers' include)")
    set(PDO_SPIRV_FOUND "")
    foreach(dir "${PDO_SPIRV_HEADERS}" "${CMAKE_CURRENT_SOURCE_DIR}/external/SPIRV-Headers/include"
                "$ENV{VULKAN_SDK}/include" "$ENV{VULKAN_SDK}/Include" "/usr/include")
        if(NOT PDO_SPIRV_FOUND AND dir AND EXISTS "${dir}/spirv/unified1/spirv.hpp")
            set(PDO_SPIRV_FOUND "${dir}")
        endif()
    endforeach()
    if(PDO_SPIRV_FOUND)
        message(STATUS "Orta Recomp: Lossless Scaling frame generation built (SPIR-V headers in ${PDO_SPIRV_FOUND})")
        # Vulkan's headers as SDL carries them, and a stand-in for volk.
        add_library(pdo_vk STATIC src/gfx/vk/vk_load.c)
        target_include_directories(pdo_vk PUBLIC src/gfx/vk "${CMAKE_CURRENT_SOURCE_DIR}/external/SDL/src/video/khronos")
        target_compile_definitions(pdo_vk PUBLIC VK_NO_PROTOTYPES)
        target_link_libraries(pdo_vk PUBLIC SDL3::SDL3-static)
        set(LSFG_VULKAN_TARGET pdo_vk)
        set(LSFG_SPIRV_INCLUDE_DIR "${PDO_SPIRV_FOUND}")
        add_subdirectory(lib/lsfg ${CMAKE_BINARY_DIR}/lsfg)
        target_sources(pdo_recomp PRIVATE src/gfx/lsfg_present.cpp)
        target_compile_definitions(pdo_recomp PUBLIC PDO_HAVE_LSFG=1)
        target_link_libraries(pdo_recomp PUBLIC lsfg)
    else()
        message(STATUS "Orta Recomp: no SPIR-V headers (run git submodule update --init, or set PDO_SPIRV_HEADERS); "
                       "building without Lossless Scaling frame generation")
    endif()

    target_sources(OrtaRecomp PRIVATE src/guest/guest.c)
    target_compile_definitions(OrtaRecomp PRIVATE PDO_HAVE_RECOMP=1)
    target_link_libraries(OrtaRecomp PRIVATE pdo_recomp)
else()
    message(STATUS "Orta Recomp: no generated code in ${PDO_GEN_DIR}; "
                   "building the launcher only (run tools/recomp.py first)")
endif()
