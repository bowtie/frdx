cmake_minimum_required(VERSION 3.20)
project(forza_radio_dx LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc /wd4324)
    add_compile_options("$<$<CONFIG:Release>:/MD;/O2;/Gy;/Gw>")
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options("$<$<CONFIG:Release>:/GL>")
    endif()
    add_compile_options("$<$<CONFIG:Debug>:/MDd>")
    add_link_options("$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")
    add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX
                            _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

add_library(version SHARED
    src/proxy/dll_main.cpp
    src/bridge.cpp
    src/log.cpp
    src/config.cpp
    src/config_store.cpp
    src/subprocess.cpp
    src/ring_buffer.cpp
    src/audio_source_manager.cpp
    src/fmod/pe_image.cpp
    src/fmod/sig_scanner.cpp
    src/fmod/radio_discovery.cpp
    src/fmod/dsp_bridge.cpp
    src/fmod/dsp_control_loop.cpp
    src/fmod/dx12_overlay.cpp
    src/fmod/game_state_probe.cpp
    src/fmod/metadata_injector.cpp
    src/fmod/dx12_overlay.cpp
    src/sources/shoutcast_source.cpp

    # Dear ImGui
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/imgui_impl_win32.cpp
    third_party/imgui/imgui_impl_dx12.cpp

    # MinHook
    third_party/minhook/buffer.c
    third_party/minhook/hook.c
    third_party/minhook/trampoline.c
    third_party/minhook/hde/hde64.c
)

target_include_directories(version PRIVATE
    include
    third_party/nlohmann
    third_party/toml11
    third_party/wil/include
    third_party/imgui
    third_party/minhook
)

target_link_libraries(version PRIVATE ws2_32 bcrypt psapi winhttp d3d12 dxgi)

set_target_properties(version PROPERTIES
    OUTPUT_NAME version
    PREFIX ""
)

# ─── Sandbox: standalone overlay test harness ────────────────────────────
# Builds a self-contained DX12 + ImGui window with mock audio source for
# rapid UI iteration without launching the game.
#   cmake --build build --target frdx_sandbox
#   ./build/frdx_sandbox.exe   (place fonts in mods/frdx/fonts/ next to exe)
# Uses the SAME sources as the main target (minus dll_main.cpp) so the
# real draw_imgui_ui() runs identically.
add_executable(frdx_sandbox
    src/sandbox_main.cpp
    src/bridge.cpp
    src/log.cpp
    src/config.cpp
    src/config_store.cpp
    src/subprocess.cpp
    src/ring_buffer.cpp
    src/audio_source_manager.cpp
    src/fmod/pe_image.cpp
    src/fmod/sig_scanner.cpp
    src/fmod/radio_discovery.cpp
    src/fmod/dsp_bridge.cpp
    src/fmod/dsp_control_loop.cpp
    src/fmod/dx12_overlay.cpp
    src/fmod/game_state_probe.cpp
    src/fmod/metadata_injector.cpp
    src/sources/shoutcast_source.cpp

    # Dear ImGui
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/imgui_impl_win32.cpp
    third_party/imgui/imgui_impl_dx12.cpp

    # MinHook
    third_party/minhook/buffer.c
    third_party/minhook/hook.c
    third_party/minhook/trampoline.c
    third_party/minhook/hde/hde64.c
)

target_include_directories(frdx_sandbox PRIVATE
    include
    third_party/nlohmann
    third_party/toml11
    third_party/wil/include
    third_party/imgui
    third_party/minhook
)

target_link_libraries(frdx_sandbox PRIVATE ws2_32 bcrypt psapi winhttp d3d12 dxgi)

set_target_properties(frdx_sandbox PROPERTIES
    OUTPUT_NAME frdx_sandbox
    WIN32_EXECUTABLE TRUE
)
