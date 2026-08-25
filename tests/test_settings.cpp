#include "TestFramework.h"

#include "AppPaths.h"
#include "Settings.h"
#include "TextUtil.h"

#include <windows.h>

#include <fstream>
#include <string>

namespace {

// Tests must never touch the developer's real %APPDATA%\ScreenshotApp.
std::wstring TempDir() {
    wchar_t buf[MAX_PATH] = {};
    GetTempPathW(ARRAYSIZE(buf), buf);
    std::wstring dir = AppPaths::Combine(buf, L"ScreenshotAppTests");
    AppPaths::EnsureDirectory(dir);
    return dir;
}

std::wstring TempFile(const wchar_t* name) {
    return AppPaths::Combine(TempDir(), name);
}

void WriteText(const std::wstring& path, const std::string& text) {
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadText(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST(Settings_DefaultsAreSane) {
    const AppConfig c = AppConfig::Defaults();
    CHECK(c.copyToClipboard);
    CHECK(c.imageFormat == ImageFormat::Png);
    CHECK(c.jpegQuality >= 1 && c.jpegQuality <= 100);
    CHECK(!c.saveFolderPath.empty());
    CHECK(!c.filenamePattern.empty());
    // Print Screen first, with a combination that survives Windows already
    // owning it as the second binding.
    CHECK_EQ(c.hotkeyCapture.vk, UINT(VK_SNAPSHOT));
    CHECK_EQ(c.hotkeyCaptureAlt.vk, UINT('S'));
    CHECK_EQ(c.hotkeyCaptureAlt.modifiers, UINT(MOD_CONTROL | MOD_SHIFT));
}

TEST(Settings_JsonRoundTripPreservesEveryField) {
    AppConfig in = AppConfig::Defaults();
    in.copyToClipboard = false;
    in.saveFolderPath = L"D:\\Shots\\Captured";
    in.filenamePattern = L"Shot_%Y%m%d";
    in.imageFormat = ImageFormat::Jpeg;
    in.jpegQuality = 73;
    in.includeCursor = true;
    in.captureDelayMs = 1500;
    in.startWithWindows = true;
    in.showNotifications = false;
    in.followSystemTheme = false;
    in.startMinimizedToTray = true;
    in.autoScrollSettleMs = 640;
    in.scrollRemoveOverlap = false;
    in.autoPdfLongCaptures = true;
    in.pdfPageSize = PdfPageSize::Letter;
    in.pdfLayout = PdfLayout::SlicedPages;
    in.pdfKeepImage = false;
    in.pdfAskWhereToSave = false;
    in.pdfFolderPath = L"D:\\Shots\\PDFs";
    in.hotkeyCapture = {MOD_ALT, 'P', true};
    in.hotkeyCaptureAlt = {MOD_CONTROL, 'R', false};

    AppConfig out;
    CHECK(Settings::FromJson(Settings::ToJson(in), out));

    CHECK_EQ(out.copyToClipboard, in.copyToClipboard);
    CHECK(out.saveFolderPath == in.saveFolderPath);
    CHECK(out.filenamePattern == in.filenamePattern);
    CHECK(out.imageFormat == in.imageFormat);
    CHECK_EQ(out.jpegQuality, in.jpegQuality);
    CHECK_EQ(out.includeCursor, in.includeCursor);
    CHECK_EQ(out.captureDelayMs, in.captureDelayMs);
    CHECK_EQ(out.startWithWindows, in.startWithWindows);
    CHECK_EQ(out.showNotifications, in.showNotifications);
    CHECK_EQ(out.followSystemTheme, in.followSystemTheme);
    CHECK_EQ(out.startMinimizedToTray, in.startMinimizedToTray);
    CHECK_EQ(out.autoScrollSettleMs, in.autoScrollSettleMs);
    CHECK_EQ(out.scrollRemoveOverlap, in.scrollRemoveOverlap);
    CHECK_EQ(out.autoPdfLongCaptures, in.autoPdfLongCaptures);
    CHECK(out.pdfPageSize == in.pdfPageSize);
    CHECK(out.pdfLayout == in.pdfLayout);
    CHECK_EQ(out.pdfKeepImage, in.pdfKeepImage);
    CHECK_EQ(out.pdfAskWhereToSave, in.pdfAskWhereToSave);
    CHECK(out.pdfFolderPath == in.pdfFolderPath);
    CHECK(out.hotkeyCapture == in.hotkeyCapture);
    CHECK(out.hotkeyCaptureAlt == in.hotkeyCaptureAlt);
}

TEST(Settings_AutomaticPdfIsOffByDefaultAndAsksWhereToSave) {
    const AppConfig c = AppConfig::Defaults();
    CHECK(!c.autoPdfLongCaptures);
    CHECK(c.pdfAskWhereToSave);
    // Keeping the image is the safe default: it is the only one of the two
    // that shows up in the main window.
    CHECK(c.pdfKeepImage);
    // A long capture in one piece is the point of the feature; slicing it
    // back into sheets is the opt-in.
    CHECK(c.pdfLayout == PdfLayout::OneLongPage);
    CHECK(c.pdfPageSize == PdfPageSize::A4);
}

TEST(Settings_UnknownPdfEnumsFallBackToDefaults) {
    const std::wstring path = TempFile(L"badpdf.json");
    WriteText(path, R"({"pdf":{"pageSize":"a3","layout":"scroll"}})");

    AppConfig out;
    CHECK(Settings::LoadFromFile(path, out));
    const AppConfig d = AppConfig::Defaults();
    CHECK(out.pdfPageSize == d.pdfPageSize);
    CHECK(out.pdfLayout == d.pdfLayout);
    DeleteFileW(path.c_str());
}

TEST(Settings_VersionOneFileIsMigrated) {
    // The old layout: a tri-state output action and three capture hotkeys.
    // "save" was the only value that meant "do not touch the clipboard", and
    // the two capture keys people had bound should survive the upgrade.
    const std::wstring path = TempFile(L"v1.json");
    WriteText(path,
              R"({"version":1,)"
              R"("output":{"action":"save","jpegQuality":60},)"
              R"("hotkeys":{"fullScreen":{"modifiers":0,"vk":44,"enabled":true},)"
              R"("region":{"modifiers":6,"vk":83,"enabled":true},)"
              R"("window":{"modifiers":6,"vk":87,"enabled":true}}})");

    AppConfig out;
    CHECK(Settings::LoadFromFile(path, out));
    CHECK(!out.copyToClipboard);
    CHECK_EQ(out.jpegQuality, 60);
    CHECK_EQ(out.hotkeyCapture.vk, UINT(VK_SNAPSHOT));
    CHECK_EQ(out.hotkeyCaptureAlt.vk, UINT('S'));
    CHECK_EQ(out.hotkeyCaptureAlt.modifiers, UINT(MOD_CONTROL | MOD_SHIFT));
    DeleteFileW(path.c_str());
}

TEST(Settings_VersionOneCopyOrBothKeepsTheClipboard) {
    const std::wstring path = TempFile(L"v1both.json");
    WriteText(path, R"({"version":1,"output":{"action":"both"}})");

    AppConfig out;
    CHECK(Settings::LoadFromFile(path, out));
    CHECK(out.copyToClipboard);
    DeleteFileW(path.c_str());
}

TEST(Settings_NonAsciiPathsSurviveUtf8) {
    AppConfig in = AppConfig::Defaults();
    in.saveFolderPath = L"D:\\Bilder\\Skärmbilder\\日本語";

    AppConfig out;
    CHECK(Settings::FromJson(Settings::ToJson(in), out));
    CHECK(out.saveFolderPath == in.saveFolderPath);
}

TEST(Settings_CorruptedFileFallsBackToDefaults) {
    const std::wstring path = TempFile(L"corrupt.json");
    WriteText(path, "{ this is not valid json at all ,,,");

    AppConfig out;
    CHECK(!Settings::LoadFromFile(path, out));
    DeleteFileW(path.c_str());
}

TEST(Settings_TruncatedFileFallsBackToDefaults) {
    // What a crash mid-write would leave behind if the write were not atomic.
    const std::wstring path = TempFile(L"truncated.json");
    const std::string full = Settings::ToJson(AppConfig::Defaults());
    WriteText(path, full.substr(0, full.size() / 2));

    AppConfig out;
    CHECK(!Settings::LoadFromFile(path, out));
    DeleteFileW(path.c_str());
}

TEST(Settings_EmptyAndMissingFilesAreRejected) {
    const std::wstring empty = TempFile(L"empty.json");
    WriteText(empty, "");
    AppConfig out;
    CHECK(!Settings::LoadFromFile(empty, out));
    DeleteFileW(empty.c_str());

    CHECK(!Settings::LoadFromFile(TempFile(L"does_not_exist.json"), out));
}

TEST(Settings_ValidJsonOfTheWrongShapeIsRejected) {
    const std::wstring path = TempFile(L"array.json");
    WriteText(path, "[1, 2, 3]");
    AppConfig out;
    CHECK(!Settings::LoadFromFile(path, out));
    DeleteFileW(path.c_str());
}

TEST(Settings_PartialConfigKeepsDefaultsForMissingKeys) {
    const std::wstring path = TempFile(L"partial.json");
    WriteText(path, R"({"version":1,"capture":{"includeCursor":true}})");

    AppConfig out;
    CHECK(Settings::LoadFromFile(path, out));
    CHECK(out.includeCursor);
    // Everything not mentioned falls back to the default, not to zero.
    const AppConfig d = AppConfig::Defaults();
    CHECK(out.imageFormat == d.imageFormat);
    CHECK_EQ(out.jpegQuality, d.jpegQuality);
    CHECK(out.filenamePattern == d.filenamePattern);
    CHECK(out.hotkeyCapture == d.hotkeyCapture);
    DeleteFileW(path.c_str());
}

TEST(Settings_OutOfRangeValuesAreClamped) {
    const std::wstring path = TempFile(L"outofrange.json");
    WriteText(path,
              R"({"output":{"jpegQuality":5000},)"
              R"("capture":{"delayMs":-42},)"
              R"("scroll":{"maxFrames":100000,"settleMs":1}})");

    AppConfig out;
    CHECK(Settings::LoadFromFile(path, out));
    CHECK(out.jpegQuality <= 100);
    CHECK(out.jpegQuality >= 1);
    CHECK(out.captureDelayMs >= 0);
    CHECK(out.autoScrollSettleMs >= 50);
    DeleteFileW(path.c_str());
}

TEST(Settings_UnknownEnumStringsFallBackInsteadOfCrashing) {
    const std::wstring path = TempFile(L"badenum.json");
    WriteText(path, R"({"output":{"format":"tiff"}})");

    AppConfig out;
    CHECK(Settings::LoadFromFile(path, out));
    const AppConfig d = AppConfig::Defaults();
    CHECK(out.imageFormat == d.imageFormat);
    DeleteFileW(path.c_str());
}

TEST(Settings_SaveIsAtomicAndLeavesNoTempFile) {
    const std::wstring path = TempFile(L"atomic.json");
    DeleteFileW(path.c_str());

    AppConfig cfg = AppConfig::Defaults();
    cfg.jpegQuality = 55;
    CHECK(Settings::SaveToFile(path, cfg));

    // The scratch file must be gone once the swap completed.
    const std::wstring tmp = path + L".tmp";
    CHECK_EQ(GetFileAttributesW(tmp.c_str()), DWORD(INVALID_FILE_ATTRIBUTES));

    AppConfig loaded;
    CHECK(Settings::LoadFromFile(path, loaded));
    CHECK_EQ(loaded.jpegQuality, 55);
    DeleteFileW(path.c_str());
}

TEST(Settings_SaveOverAnExistingFileReplacesItWholesale) {
    const std::wstring path = TempFile(L"replace.json");

    AppConfig first = AppConfig::Defaults();
    first.filenamePattern = L"AAAA_%Y";
    CHECK(Settings::SaveToFile(path, first));
    const size_t firstLen = ReadText(path).size();

    AppConfig second = AppConfig::Defaults();
    second.filenamePattern = L"B";
    CHECK(Settings::SaveToFile(path, second));

    AppConfig loaded;
    CHECK(Settings::LoadFromFile(path, loaded));
    CHECK(loaded.filenamePattern == L"B");
    // No leftover tail from the longer previous content.
    CHECK(ReadText(path).size() < firstLen);
    DeleteFileW(path.c_str());
}

TEST(Settings_RoundTripThroughDiskMatchesInMemoryRoundTrip) {
    const std::wstring path = TempFile(L"disk.json");

    AppConfig in = AppConfig::Defaults();
    in.copyToClipboard = false;
    in.captureDelayMs = 250;
    in.hotkeyCaptureAlt = {MOD_ALT | MOD_CONTROL, 'Q', true};
    CHECK(Settings::SaveToFile(path, in));

    AppConfig fromDisk;
    CHECK(Settings::LoadFromFile(path, fromDisk));
    AppConfig fromMemory;
    CHECK(Settings::FromJson(Settings::ToJson(in), fromMemory));

    CHECK_EQ(fromDisk.copyToClipboard, fromMemory.copyToClipboard);
    CHECK_EQ(fromDisk.captureDelayMs, fromMemory.captureDelayMs);
    CHECK(fromDisk.hotkeyCaptureAlt == fromMemory.hotkeyCaptureAlt);
    DeleteFileW(path.c_str());
}

TEST(Settings_DescribeHotkeyIsHumanReadable) {
    CHECK(DescribeHotkey({MOD_CONTROL | MOD_SHIFT, 'S', true}) == L"Ctrl+Shift+S");
    CHECK(DescribeHotkey({0, 0, false}) == L"(none)");
    CHECK(DescribeHotkey({MOD_ALT, VK_F9, true}) == L"Alt+F9");
}

TEST(Settings_DescribeHotkeyNamesPrintScreen) {
    // MapVirtualKey reports a scan code for Print Screen that no layout has
    // a name for, so GetKeyNameText returns the literal "<00>" - which the
    // settings dialog was showing to the user as the name of their own
    // default capture key.
    CHECK(DescribeHotkey({0, VK_SNAPSHOT, true}) == L"Print Screen");
}

TEST(Settings_DescribeHotkeyNeverReportsAPlaceholderName) {
    // Whatever the key, the description has to be something a person can
    // act on. GetKeyNameText's "<nn>" answers are not.
    for (UINT vk = 1; vk < 256; ++vk) {
        const std::wstring name = DescribeHotkey({MOD_CONTROL, vk, true});
        CHECK_MSG(!name.empty(), "vk " + std::to_string(vk));
        CHECK_MSG(name.find(L'<') == std::wstring::npos, "vk " + std::to_string(vk));
    }
}
