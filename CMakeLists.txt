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
    src/proxy/comctl32.def
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
    third_party/kiero/kiero.cpp
    src/overlay/settings.cpp
    src/fmod/game_state_probe.cpp
    src/fmod/metadata_injector.cpp
    src/sources/shoutcast_source.cpp
    src/sources/shoutcast_metadata.cpp
    src/helper/worker_client.cpp
    src/dependency_manager.cpp
    src/texture_injector.cpp
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
    third_party/kiero
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
# entry point instead of WinMain.
target_link_options(frdx-helper PRIVATE -Wl,/ENTRY:wmainCRTStartup)

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
