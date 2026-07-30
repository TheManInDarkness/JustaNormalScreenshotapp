cmake_minimum_required(VERSION 3.20)
project(ScreenshotApp LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN)

file(GLOB CPP_SOURCES src/*.cpp)
file(GLOB HEADERS include/*.h)

set(RESOURCE_FILE resources/app.rc)

# Create a static library for the C-only pdfgen code, so it gets its own compilation unit
add_library(pdfgen_lib STATIC third_party/pdfgen.c)
target_include_directories(pdfgen_lib PRIVATE third_party)

# Main WIN32 executable (C++ only)
add_executable(ScreenshotApp WIN32
    ${CPP_SOURCES}
    ${HEADERS}
    ${RESOURCE_FILE}
)

target_include_directories(ScreenshotApp PRIVATE include third_party)

# Precompiled header for C++ sources only
target_precompile_headers(ScreenshotApp PRIVATE include/pch.h)

target_link_libraries(ScreenshotApp PRIVATE
    pdfgen_lib
    gdiplus
    d3d11
    dxgi
    ole32
    oleaut32
    gdi32
    user32
    shell32
    dwmapi
    comctl32
    advapi32
    shlwapi
)

# We embed the manifest in the .rc file; disable the linker auto-manifest to avoid duplicates
if(MSVC)
    set_target_properties(ScreenshotApp PROPERTIES
        LINK_FLAGS "/MANIFEST:NO"
    )
endif()
