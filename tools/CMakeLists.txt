# OCR benchmark probe - a console tool, not part of the app. It renders a
# synthetic corpus with known ground truth, runs the real TextOcr pipeline
# over it (PP-OCR first, Windows engine fallback - whatever the app would do),
# and scores word/line recall against the truth. `OcrProbe bench <png...>`
# runs the same pipeline over arbitrary real captures and prints what it read.
#
# Kept out of BUILD_TESTS' headless promise: this one exercises live engines,
# so it belongs to the developer loop, not CI.

add_executable(OcrProbe OcrProbe.cpp
    ../src/PpOcr.cpp
    ../src/TextOcr.cpp
    ../src/OcrNormalize.cpp
    ../src/OcrSelection.cpp
    ../src/Utils.cpp
    ../src/Logger.cpp
    ../src/AppPaths.cpp
)

target_include_directories(OcrProbe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/onnxruntime/include
)

target_link_libraries(OcrProbe
    gdiplus
    ole32
    oleaut32
    gdi32
    user32
    shell32
    shlwapi
    windowscodecs
    windowsapp
)

# /MANIFEST:NO for the same reason as the app target: the linker-generated
# manifest would collide with nothing here, but staying identical keeps the
# two link lines honest about each other.
set_target_properties(OcrProbe PROPERTIES LINK_FLAGS "/MANIFEST:NO")

# Sit beside ScreenshotApp.exe so the runtime DLL the POST_BUILD step copies
# there and the ..\..\third_party model paths resolve exactly as they do for
# the app.
set_target_properties(OcrProbe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:ScreenshotApp>")
