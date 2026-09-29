cmake_minimum_required(VERSION 3.24)

project(PcYoutube VERSION 0.2.0 LANGUAGES CXX)

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

    set(WEBVIEW2_VERSION "1.0.4258.31")
    FetchContent_Declare(
        webview2_sdk
        URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(webview2_sdk)
    if(NOT webview2_sdk_POPULATED)
        FetchContent_Populate(webview2_sdk)
    endif()

    add_executable(PcYoutube WIN32
        src/music/platform/windows/main_win32.cpp
    )

    target_include_directories(PcYoutube PRIVATE
        "${webview2_sdk_SOURCE_DIR}/build/native/include"
    )

    target_link_libraries(PcYoutube PRIVATE
        pcyoutube_core
        user32
        gdi32
        ole32
        "${webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
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
        target_compile_options(PcYoutube PRIVATE /W4 /permissive- /utf-8)
    endif()
else()
    message(FATAL_ERROR "PcYoutube currently has a Windows frontend only. The portable pcyoutube_core target is ready for a future Android/NDK frontend.")
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
