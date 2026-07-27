cmake_minimum_required(VERSION 3.20)
project(forza_radio_dx LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── ASan/UBSan: instrument the sandbox with clang-cl sanitizers ─────────
# Enable with: cmake -DFRDX_ASAN=ON -DCMAKE_BUILD_TYPE=Debug ...
# Requires clang-cl (the default toolchain).  Catches runtime OOB / UB in
# the DSP callback, ring buffer, and mpg123 decode paths.
option(FRDX_ASAN "Enable AddressSanitizer + UBSan on frdx_sandbox (requires clang-cl)" OFF)
if(FRDX_ASAN AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "FRDX_ASAN requires clang-cl (current compiler: ${CMAKE_CXX_COMPILER_ID})")
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc /wd4324)
    add_compile_options("$<$<CONFIG:Release>:/MD;/O2;/Gy;/Gw>")
    set_source_files_properties(third_party/imanim/im_anim.cpp PROPERTIES COMPILE_OPTIONS /W0)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-Wthread-safety)
    else()
        add_compile_options("$<$<CONFIG:Release>:/GL>")
    endif()
    add_compile_options("$<$<CONFIG:Debug>:/MDd>")
    add_link_options("$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")
endif()

add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX
                        _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)

# ── FreeType: fetch and build as static library for ImGui font rasterizing ─
# Used by imgui_freetype.cpp for high-quality anti-aliased font rendering
# (Geist Variable + Kenney Game Icons).  The stb_truetype default rasterizer
# produces poor results for variable fonts at fractional scales.
include(FetchContent)
FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG        VER-2-13-3
    GIT_SHALLOW    TRUE
)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
FetchContent_MakeAvailable(freetype)

# ── mpg123: fetch source and build as static library ────────────────────
# Eliminates the runtime DLL dependency by compiling libmpg123 directly
# into version.dll and frdx_sandbox.exe.
FetchContent_Declare(
    mpg123
    GIT_REPOSITORY https://github.com/madebr/mpg123.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  ports/cmake
)
set(BUILD_LIBOUT123 OFF CACHE BOOL "Build libout123" FORCE)
FetchContent_MakeAvailable(mpg123)
# Suppress warnings in third-party C code.
if(TARGET libmpg123)
    target_compile_options(libmpg123 PRIVATE /W0)
endif()
if(TARGET compat)
    target_compile_options(compat PRIVATE /W0)
endif()
if(TARGET libsyn123)
    target_compile_options(libsyn123 PRIVATE /W0)
endif()

add_library(version SHARED
    src/proxy/dll_main.cpp
    src/d3d12_debug.cpp
    src/fastboot.cpp
    src/bridge.cpp
    src/log.cpp
    src/config.cpp
    src/config_store.cpp
    src/subprocess.cpp
    src/ring_buffer.cpp
    src/dualsense.cpp
    src/audio_source_manager.cpp
    src/fmod/pe_image.cpp
    src/fmod/sig_scanner.cpp
    src/fmod/radio_discovery.cpp
    src/fmod/dsp_bridge.cpp
    src/fmod/dsp_control_loop.cpp
    src/overlay/overlay.cpp
    src/overlay/toasts.cpp
    src/overlay/console.cpp
    src/overlay/dualsense.cpp
    src/overlay/themes.cpp
    src/overlay/textures.cpp
    src/overlay/hooks.cpp
    src/overlay/tex_hooks.cpp
    src/overlay/player_bar.cpp
    third_party/kiero2/kiero_intern.cpp
    third_party/kiero2/kiero_d3d12.cpp
    third_party/bc7enc/bc7enc.c
    src/overlay/settings.cpp
    src/fmod/game_state_probe.cpp
    src/fmod/metadata_injector.cpp
    src/sources/shoutcast_source.cpp
    src/sources/shoutcast_metadata.cpp
    src/sources/mpg123_stream.cpp
    src/helper/worker_client.cpp
    src/texture_injector.cpp
    third_party/hidapi/windows/hid.c

    # Dear ImGui
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_impl_win32.cpp
    third_party/imgui/imgui_impl_dx12.cpp
    third_party/imgui/misc/freetype/imgui_freetype.cpp

    # MinHook
    third_party/minhook/buffer.c
    third_party/minhook/hook.c
    third_party/minhook/trampoline.c
    third_party/minhook/hde/hde64.c

    # ImGui Animated
    third_party/imgui-animated/imgui_animated.cpp
    third_party/imanim/im_anim.cpp
)

target_include_directories(version PRIVATE
    include
    third_party/nlohmann
    third_party/toml11
    third_party/wil/include
    third_party/imgui
    third_party/imgui/misc/freetype
    third_party/IconFontCppHeaders
    third_party/minhook
    third_party/kiero2
    third_party/stb
    third_party/bc7enc
    third_party/imgui-animated
    third_party/imanim
    third_party/hidapi
    third_party/hidapi/hidapi
    ${mpg123_SOURCE_DIR}/src/include
)

target_link_libraries(version PRIVATE ws2_32 bcrypt psapi winhttp d3d12 dxgi hid setupapi cfgmgr32
                                      user32 dbghelp libmpg123 freetype)

set_target_properties(version PROPERTIES
    OUTPUT_NAME version
    PREFIX ""
)

# ── Auto-deploy: copy version.dll to the game folder after build ───────────
# Enable with: cmake -DFRDX_AUTO_DEPLOY=ON -DFRDX_GAME_DIR="S:/SteamLibrary/steamapps/common/ForzaHorizon6"
option(FRDX_AUTO_DEPLOY "Copy version.dll to the game folder after build" OFF)
set(FRDX_GAME_DIR "" CACHE PATH "Path to the Forza Horizon game install directory")

if(FRDX_AUTO_DEPLOY)
    if(NOT FRDX_GAME_DIR)
        message(FATAL_ERROR "FRDX_AUTO_DEPLOY=ON requires -DFRDX_GAME_DIR=<path to game folder>")
    endif()
    add_custom_command(TARGET version POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DFROM="$<TARGET_FILE:version>"
                -DTO="${FRDX_GAME_DIR}/version.dll"
                -P ${CMAKE_SOURCE_DIR}/cmake/deploy_copy.cmake
        COMMENT "Deploying version.dll → ${FRDX_GAME_DIR}"
    )
    # Deploy font files to the game's mods/frdx/fonts directory.
    add_custom_command(TARGET version POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/dist/mods/frdx/fonts
                ${FRDX_GAME_DIR}/mods/frdx/fonts
        COMMENT "Deploying fonts → ${FRDX_GAME_DIR}/mods/frdx/fonts"
    )
endif()

add_executable(dualsense_self_test
    tests/dualsense_self_test.cpp
    src/log.cpp
    third_party/hidapi/windows/hid.c
)

target_include_directories(dualsense_self_test PRIVATE
    include
    third_party/wil/include
    third_party/hidapi
    third_party/hidapi/hidapi
)

target_link_libraries(dualsense_self_test PRIVATE ws2_32 hid setupapi cfgmgr32 user32)

add_test(NAME dualsense_self_test COMMAND dualsense_self_test)

add_executable(frdx-helper
    src/dualsense_broker_main.cpp
    src/dualsense.cpp
    src/config.cpp
    src/log.cpp
    src/subprocess.cpp
    src/helper/worker_server.cpp
    third_party/hidapi/windows/hid.c
)

target_include_directories(frdx-helper PRIVATE
    include
    third_party/toml11
    third_party/wil/include
    third_party/hidapi
    third_party/hidapi/hidapi
    third_party/nlohmann
)

target_link_libraries(frdx-helper PRIVATE ws2_32 hid setupapi cfgmgr32 user32 shell32 winhttp bcrypt)

set_target_properties(frdx-helper PROPERTIES
    OUTPUT_NAME frdx-helper
    WIN32_EXECUTABLE TRUE
)
# Use the Windows subsystem (no console window) but keep wmain as the
# entry point instead of WinMain.  Use the MSVC linker syntax directly
# (lld-link doesn't understand the GCC-style -Wl, prefix).
target_link_options(frdx-helper PRIVATE /ENTRY:wmainCRTStartup)

if(FRDX_AUTO_DEPLOY)
    add_custom_command(TARGET frdx-helper POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DFROM="$<TARGET_FILE:frdx-helper>"
                -DTO="${FRDX_GAME_DIR}/mods/frdx/frdx-helper.exe"
                -P ${CMAKE_SOURCE_DIR}/cmake/deploy_copy.cmake
        COMMENT "Deploying frdx-helper.exe → ${FRDX_GAME_DIR}/mods/frdx"
    )
endif()

add_executable(dualsense_config_self_test
    tests/dualsense_config_self_test.cpp
    src/config.cpp
    src/log.cpp
)

target_include_directories(dualsense_config_self_test PRIVATE
    include
    third_party/toml11
    third_party/wil/include
)

add_test(NAME dualsense_config_self_test COMMAND dualsense_config_self_test)

add_executable(dualsense_settings_coverage_test
    tests/dualsense_settings_coverage_test.cpp
)

add_test(NAME dualsense_settings_coverage_test COMMAND dualsense_settings_coverage_test)
set_tests_properties(dualsense_settings_coverage_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# ─── Sandbox: standalone overlay test harness ────────────────────────────
# Builds a self-contained DX12 + ImGui window with WASAPI audio for
# rapid UI iteration without launching the game.
#   cmake --build build --target frdx_sandbox
#   ./build/frdx_sandbox.exe   (place fonts in mods/frdx/fonts/ next to exe)
# Uses the SAME overlay sources as the main target (minus dll_main.cpp)
# so the real draw_imgui_ui() runs identically.
add_executable(frdx_sandbox
    src/sandbox_main.cpp
    src/d3d12_debug.cpp
    src/fastboot.cpp
    src/bridge.cpp
    src/log.cpp
    src/config.cpp
    src/config_store.cpp
    src/subprocess.cpp
    src/ring_buffer.cpp
    src/dualsense.cpp
    src/audio_source_manager.cpp
    src/fmod/pe_image.cpp
    src/fmod/sig_scanner.cpp
    src/fmod/radio_discovery.cpp
    src/fmod/dsp_bridge.cpp
    src/fmod/dsp_control_loop.cpp
    src/fmod/game_state_probe.cpp
    src/fmod/metadata_injector.cpp
    src/overlay/overlay.cpp
    src/overlay/toasts.cpp
    src/overlay/console.cpp
    src/overlay/dualsense.cpp
    src/overlay/themes.cpp
    src/overlay/textures.cpp
    src/overlay/hooks.cpp
    src/overlay/tex_hooks.cpp
    src/overlay/player_bar.cpp
    src/overlay/settings.cpp
    src/sources/shoutcast_source.cpp
    src/sources/shoutcast_metadata.cpp
    src/sources/mpg123_stream.cpp
    src/helper/worker_client.cpp
    src/texture_injector.cpp
    third_party/hidapi/windows/hid.c

    # Dear ImGui
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_impl_win32.cpp
    third_party/imgui/imgui_impl_dx12.cpp
    third_party/imgui/misc/freetype/imgui_freetype.cpp

    # MinHook (needed by hooks.cpp for hooked mode)
    third_party/minhook/buffer.c
    third_party/minhook/hook.c
    third_party/minhook/trampoline.c
    third_party/minhook/hde/hde64.c

    # Kiero (needed by hooks.cpp)
    third_party/kiero2/kiero_intern.cpp
    third_party/kiero2/kiero_d3d12.cpp

    # BC7 encoder (needed by textures.cpp)
    third_party/bc7enc/bc7enc.c

    # ImGui Animated
    third_party/imgui-animated/imgui_animated.cpp
    third_party/imanim/im_anim.cpp
)

target_include_directories(frdx_sandbox PRIVATE
    include
    third_party/nlohmann
    third_party/toml11
    third_party/wil/include
    third_party/imgui
    third_party/imgui/misc/freetype
    third_party/IconFontCppHeaders
    third_party/minhook
    third_party/kiero2
    third_party/stb
    third_party/bc7enc
    third_party/imgui-animated
    third_party/imanim
    third_party/hidapi
    third_party/hidapi/hidapi
    ${mpg123_SOURCE_DIR}/src/include
)

target_link_libraries(frdx_sandbox PRIVATE ws2_32 bcrypt psapi winhttp d3d12 dxgi hid setupapi
                                           cfgmgr32 user32 dbghelp libmpg123 freetype)

set_target_properties(frdx_sandbox PROPERTIES
    OUTPUT_NAME frdx_sandbox
    WIN32_EXECUTABLE TRUE
)

# Apply ASan/UBSan to the sandbox target only.  The main version.dll is
# injected into the game process — instrumenting it would require the host
# to also be ASan-aware.  The sandbox runs the same DSP/ring-buffer code
# as a standalone EXE, so it catches the same bugs without that constraint.
if(FRDX_ASAN)
    target_compile_options(frdx_sandbox PRIVATE
        -fsanitize=address,undefined
        /Oy-  # keep frame pointers for readable ASan stack traces
        -O1   # enough optimization to avoid false positives, not enough to inline them away
    )
    # clang-cl ASan requires the release CRT (not /MDd or /MTd).
    # /MT overrides the /MDd set by the global Debug config (last-wins).
    target_compile_options(frdx_sandbox PRIVATE /MT)

    # lld-link doesn't auto-link the sanitizer runtimes the way clang-cl
    # does when used as a linker driver.  Detect the clang resource dir and
    # explicitly link the ASan + UBSan runtime libraries.
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir
        OUTPUT_VARIABLE _clang_resource_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_clang_resource_dir)
        set(_clang_rt_lib_dir "${_clang_resource_dir}/lib/windows")
        target_link_directories(frdx_sandbox PRIVATE "${_clang_rt_lib_dir}")
        # Dynamic ASan: the DLL is copied next to the exe at build time.
        # The thunk redirects CRT functions to the ASan DLL when using /MT.
        target_link_libraries(frdx_sandbox PRIVATE
            clang_rt.asan_dynamic-x86_64
            clang_rt.asan_dynamic_runtime_thunk-x86_64
            clang_rt.ubsan_standalone-x86_64
            clang_rt.ubsan_standalone_cxx-x86_64
        )
        add_custom_command(TARGET frdx_sandbox POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    -DFROM="${_clang_rt_lib_dir}/clang_rt.asan_dynamic-x86_64.dll"
                    -DTO="$<TARGET_FILE_DIR:frdx_sandbox>/clang_rt.asan_dynamic-x86_64.dll"
                    -P ${CMAKE_SOURCE_DIR}/cmake/deploy_copy.cmake
            COMMENT "Copying ASan runtime DLL next to frdx_sandbox.exe"
        )
    else()
        message(FATAL_ERROR "FRDX_ASAN: could not detect clang resource dir")
    endif()
endif()
