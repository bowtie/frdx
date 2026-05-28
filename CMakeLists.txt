cmake_minimum_required(VERSION 3.20)
project(fh6_universal_radio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP /EHsc /wd4324)
    add_compile_options("$<$<CONFIG:Release>:/O2;/GL;/Gy;/Gw>")
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
    src/fmod/metadata_injector.cpp
    src/fmod/dx12_overlay.cpp
    src/http/http_server.cpp
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
    third_party/cpp-httplib
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
