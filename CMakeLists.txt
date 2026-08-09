cmake_minimum_required(VERSION 3.20)
project(ScreenshotApp LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build the pure-logic unit test suite" ON)

add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX
                -D_CRT_SECURE_NO_WARNINGS)

if(MSVC)
    # /utf-8 so string literals in the sources are read as UTF-8 regardless of
    # the machine's ANSI codepage.
    add_compile_options(/utf-8 /W3)
    # PDFGen is C99 and trips a handful of MSVC C4996/C4244 warnings that are
    # not actionable in vendored code.
    set_source_files_properties(third_party/pdfgen.c PROPERTIES COMPILE_FLAGS "/wd4996 /wd4244 /wd4267")
endif()

# Windows SDK import libraries are linked by bare name: find_library() cannot
# see them under the Visual Studio generator (the SDK lib directory is not on
# CMake's default search path - it is injected by MSBuild at link time), so
# searching for them only produces NOTFOUND.

# NOTE: plain GLOB, not CONFIGURE_DEPENDS - re-run CMake configure after
# adding or removing a source file, a plain rebuild will not pick it up.
file(GLOB_RECURSE SOURCES src/*.cpp)
file(GLOB_RECURSE HEADERS include/*.h)

# PDFGen ships as plain C - compile it alongside the C++ sources
set(THIRD_PARTY_SOURCES third_party/pdfgen.c)

set(RESOURCE_FILE resources/app.rc)

add_executable(ScreenshotApp WIN32
    ${SOURCES}
    ${HEADERS}
    ${THIRD_PARTY_SOURCES}
    ${RESOURCE_FILE}
)

target_include_directories(ScreenshotApp PRIVATE include third_party)

# app.rc embeds resources/manifest.xml itself. Without /MANIFEST:NO the linker
# also generates one, and two RT_MANIFEST resources with the same id is a hard
# error (CVT1100: duplicate resource).
set_target_properties(ScreenshotApp PROPERTIES LINK_FLAGS "/MANIFEST:NO")

# d3d11/dxgi are deliberately absent: Desktop Duplication only ever served
# the whole-screen capture path, which no longer exists. Everything captures
# through GDI BitBlt now.
target_link_libraries(ScreenshotApp
    gdiplus
    dbghelp
    ole32
    oleaut32
    gdi32
    user32
    shell32
    dwmapi
    comctl32
    advapi32
    shlwapi
    uxtheme
    uuid
    windowscodecs
)

# Group sources in the VS solution explorer the way they sit on disk
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    ${SOURCES} ${HEADERS} ${THIRD_PARTY_SOURCES} ${RESOURCE_FILE})

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
