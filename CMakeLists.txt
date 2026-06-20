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

add_library(version SHARED
    src/proxy/dll_main.cpp
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
    src/overlay/blur.cpp
    src/overlay/toasts.cpp
    src/overlay/console.cpp
    src/overlay/dualsense.cpp
    src/overlay/themes.cpp
    src/overlay/textures.cpp
    src/overlay/hooks.cpp
    src/overlay/player_bar.cpp
    src/overlay/settings.cpp
    src/fmod/game_state_probe.cpp
    src/fmod/metadata_injector.cpp
    src/sources/shoutcast_source.cpp
    third_party/hidapi/windows/hid.c

    # Dear ImGui
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_impl_win32.cpp
    third_party/imgui/imgui_impl_dx12.cpp

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
    third_party/minhook
    third_party/stb
    third_party/imgui-animated
    third_party/imanim
    third_party/hidapi
    third_party/hidapi/hidapi
)

target_link_libraries(version PRIVATE ws2_32 bcrypt psapi winhttp d3d12 dxgi hid setupapi cfgmgr32
                                      user32)

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
    src/dualsense.cpp
    src/audio_source_manager.cpp
    src/fmod/pe_image.cpp
    src/fmod/sig_scanner.cpp
    src/fmod/radio_discovery.cpp
    src/fmod/dsp_bridge.cpp
    src/fmod/dsp_control_loop.cpp
    src/overlay/overlay.cpp
    src/overlay/blur.cpp
    src/overlay/toasts.cpp
    src/overlay/console.cpp
    src/overlay/dualsense.cpp
    src/overlay/themes.cpp
    src/overlay/textures.cpp
    src/overlay/hooks.cpp
    src/overlay/player_bar.cpp
    src/overlay/settings.cpp
    src/fmod/game_state_probe.cpp
    src/fmod/metadata_injector.cpp
    src/sources/shoutcast_source.cpp
    third_party/hidapi/windows/hid.c

    # Dear ImGui
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_impl_win32.cpp
    third_party/imgui/imgui_impl_dx12.cpp

    # MinHook
    third_party/minhook/buffer.c
    third_party/minhook/hook.c
    third_party/minhook/trampoline.c
    third_party/minhook/hde/hde64.c

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
    third_party/minhook
    third_party/stb
    third_party/imgui-animated
    third_party/imanim
    third_party/hidapi
    third_party/hidapi/hidapi
)

target_link_libraries(frdx_sandbox PRIVATE ws2_32 bcrypt psapi winhttp d3d12 dxgi hid setupapi
                                           cfgmgr32 user32)

set_target_properties(frdx_sandbox PROPERTIES
    OUTPUT_NAME frdx_sandbox
    WIN32_EXECUTABLE TRUE
)

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

add_executable(frdx_dualsense_broker
    src/dualsense_broker_main.cpp
    src/dualsense.cpp
    src/config.cpp
    src/log.cpp
    third_party/hidapi/windows/hid.c
)

target_include_directories(frdx_dualsense_broker PRIVATE
    include
    third_party/toml11
    third_party/wil/include
    third_party/hidapi
    third_party/hidapi/hidapi
)

target_link_libraries(frdx_dualsense_broker PRIVATE ws2_32 hid setupapi cfgmgr32 user32 shell32)

set_target_properties(frdx_dualsense_broker PROPERTIES
    OUTPUT_NAME frdx_dualsense_broker
    WIN32_EXECUTABLE FALSE
)

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
