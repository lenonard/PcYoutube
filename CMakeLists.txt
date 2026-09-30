cmake_minimum_required(VERSION 3.24)

project(PcYoutube VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PCYOUTUBE_OUT_DIR "${CMAKE_SOURCE_DIR}/out")

add_library(pcyoutube_core STATIC
    src/music/core/music_app.cpp
)

target_include_directories(pcyoutube_core PUBLIC
    src/music/core
)

if(WIN32)
    include(FetchContent)

    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.9b-docking
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    )

    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )

    target_compile_definitions(imgui PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    find_program(PCYOUTUBE_POWERSHELL NAMES powershell.exe powershell REQUIRED)
    set(PCYOUTUBE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${PCYOUTUBE_GENERATED_DIR}")
    set(PCYOUTUBE_APP_ICON "${PCYOUTUBE_GENERATED_DIR}/PcYoutube.ico")
    set(APP_ICON_RESOURCE_PATH "${PCYOUTUBE_APP_ICON}")
    set(PCYOUTUBE_APP_RC "${PCYOUTUBE_GENERATED_DIR}/PcYoutube.rc")
    configure_file(cmake/app.rc.in "${PCYOUTUBE_APP_RC}" @ONLY)

    add_custom_command(
        OUTPUT "${PCYOUTUBE_APP_ICON}"
        COMMAND "${PCYOUTUBE_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/cmake/generate_app_icon.ps1"
                -Output "${PCYOUTUBE_APP_ICON}"
        DEPENDS cmake/generate_app_icon.ps1
        VERBATIM
    )
    add_custom_target(pcyoutube_app_icon DEPENDS "${PCYOUTUBE_APP_ICON}")

    add_executable(PcYoutube WIN32
        src/music/platform/windows/main_win32.cpp
        src/music/platform/windows/audio_backend.cpp
        src/music/platform/windows/playlist_store.cpp
        src/music/platform/windows/thumbnail_cache.cpp
        "${PCYOUTUBE_APP_RC}"
    )
    add_dependencies(PcYoutube pcyoutube_app_icon)

    target_include_directories(PcYoutube PRIVATE
        src/music/platform/windows
    )

    target_link_libraries(PcYoutube PRIVATE
        pcyoutube_core
        imgui
        nlohmann_json::nlohmann_json
        d3d11
        dxgi
        dwmapi
        user32
        gdi32
        shell32
        winhttp
        windowscodecs
        ole32
    )

    target_compile_definitions(PcYoutube PRIVATE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    if(MSVC)
        target_compile_options(pcyoutube_core PRIVATE /W4 /permissive- /utf-8)
        target_compile_options(imgui PRIVATE /W3 /utf-8)
        target_compile_options(PcYoutube PRIVATE /W4 /permissive- /utf-8)
    endif()
else()
    message(FATAL_ERROR "PcYoutube currently has a Windows frontend only. The portable pcyoutube_core target is reusable on other platforms.")
endif()

set_target_properties(PcYoutube PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PCYOUTUBE_OUT_DIR}"
)

foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(PcYoutube PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${PCYOUTUBE_OUT_DIR}"
    )
endforeach()

enable_testing()
add_test(NAME MusicCoreSelfTest COMMAND PcYoutube --self-test)
