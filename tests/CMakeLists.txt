# Headless unit tests for the pure-logic parts of the app.
#
# Only sources that never create a window or a message loop are listed here,
# so this target builds and runs on a CI agent with no display.

add_executable(ScreenshotAppTests
    test_main.cpp
    test_stitcher_core.cpp
    test_live_capture.cpp
    test_pdf_slicer.cpp
    test_settings.cpp
    test_ocr_selection.cpp
    test_ocr_normalize.cpp
    ../src/ScrollStitcherCore.cpp
    ../src/PDFSlicer.cpp
    ../src/OcrSelection.cpp
    ../src/OcrNormalize.cpp
    ../src/Settings.cpp
    ../src/AppPaths.cpp
    ../src/Logger.cpp
)

target_include_directories(ScreenshotAppTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
)

target_link_libraries(ScreenshotAppTests shlwapi shell32 ole32)

add_test(NAME ScreenshotAppTests COMMAND ScreenshotAppTests)
