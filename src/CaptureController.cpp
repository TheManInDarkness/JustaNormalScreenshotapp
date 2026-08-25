#include "CaptureController.h"

#include "AppPaths.h"
#include "Clipboard.h"
#include "GalleryWindow.h"
#include "Logger.h"
#include "OcrOverlay.h"
#include "Overlay.h"
#include "ScrollSession.h"
#include "Settings.h"
#include "Toast.h"
#include "Utils.h"

using namespace Gdiplus;

namespace CaptureController {
namespace {

bool g_busy = false;

struct BusyGuard {
    BusyGuard() { g_busy = true; }
    ~BusyGuard() { g_busy = false; }
};

const wchar_t* ExtensionForFormat(ImageFormat fmt) {
    return fmt == ImageFormat::Jpeg ? L"jpg" : L"png";
}

}  // namespace

bool IsBusy() { return g_busy; }

ScreenGrab::Options OptionsFromConfig() {
    const AppConfig& cfg = Settings::Get();
    ScreenGrab::Options opt;
    opt.includeCursor = cfg.includeCursor;
    opt.delayMs = cfg.captureDelayMs;
    return opt;
}

std::wstring SaveWithBaseName(Bitmap* bmp, const std::wstring& baseName) {
    if (!bmp) return std::wstring();

    const AppConfig& cfg = Settings::Get();
    std::wstring folder = cfg.saveFolderPath;
    if (folder.empty()) folder = AppPaths::GetDefaultSaveFolder();

    if (!AppPaths::EnsureDirectory(folder)) {
        Logger::Errorf(L"Cannot create the save folder %s", folder.c_str());
        return std::wstring();
    }

    const std::wstring path =
        Utils::MakeUniquePath(folder, baseName, ExtensionForFormat(cfg.imageFormat));

    if (!Utils::SaveBitmapToFile(bmp, path, cfg.jpegQuality)) {
        Logger::Errorf(L"Failed to save %s", path.c_str());
        return std::wstring();
    }
    Logger::Info(L"Saved " + path);
    return path;
}

std::wstring SaveToConfiguredFolder(Bitmap* bmp) {
    const AppConfig& cfg = Settings::Get();
    return SaveWithBaseName(bmp, Utils::ExpandFilenamePattern(cfg.filenamePattern));
}

void Deliver(Bitmap* bmp, const RECT& flashRegion, const std::wstring& toastTitle) {
    if (!bmp || bmp->GetLastStatus() != Ok) {
        Toast::Show(L"Capture failed", L"Nothing was captured. See app.log for details.");
        return;
    }

    const AppConfig& cfg = Settings::Get();

    if (flashRegion.right > flashRegion.left && flashRegion.bottom > flashRegion.top) {
        Toast::FlashRegion(flashRegion);
    }

    // Always saved: the main window lists the save folder, so a capture that
    // was never written to disk would disappear the moment it was taken.
    const std::wstring savedPath = SaveToConfiguredFolder(bmp);

    bool copied = false;
    if (cfg.copyToClipboard) {
        copied = Clipboard::CopyBitmap(bmp);
        if (!copied) Logger::Error(L"Copying the capture to the clipboard failed");
    }

    if (!savedPath.empty()) Gallery::NotifyCaptureSaved(savedPath);

    if (savedPath.empty()) {
        Toast::Show(L"Capture not saved",
                    copied ? L"It is on the clipboard, but writing the file failed."
                           : L"The file could not be written. See app.log for details.");
        return;
    }

    if (!cfg.showNotifications) return;

    const std::wstring detail =
        (copied ? L"Copied and saved as " : L"Saved as ") +
        Utils::GetFileNameFromPath(savedPath);

    // Clicking the toast reveals the file, which is the thing people reach
    // for immediately after a save.
    Toast::Show(toastTitle, detail + L"  (click to show)",
                [savedPath]() { Utils::OpenFolderAndSelect(savedPath); });
}

void DeliverExtractedText(const OcrOverlay::Result& ocr,
                          const RECT& flashRegion) {
    switch (ocr.outcome) {
        case OcrOverlay::Outcome::Cancelled:
            return;

        case OcrOverlay::Outcome::Copied: {
            if (!Clipboard::PutText(ocr.text)) {
                Toast::Show(L"Copy failed",
                            L"The text could not be placed on the clipboard. "
                            L"See app.log for details.");
                return;
            }
            if (flashRegion.right > flashRegion.left &&
                flashRegion.bottom > flashRegion.top) {
                Toast::FlashRegion(flashRegion);
            }
            if (!Settings::Get().showNotifications) return;
            Toast::Show(L"Text extracted",
                        ocr.copiedWords == 1
                            ? std::wstring(L"1 word copied.")
                            : std::to_wstring(ocr.copiedWords) +
                                  L" words copied.");
            return;
        }

        case OcrOverlay::Outcome::NoText:
            Toast::Show(L"Extract text",
                        L"No readable text was found in the region.");
            return;

        case OcrOverlay::Outcome::Failed:
            if (ocr.engineUnavailable) {
                Toast::Show(L"No OCR language installed",
                            L"Add one in Settings > Time & language > "
                            L"Language & region, then try again.");
            } else {
                Toast::Show(L"Extract text failed", L"See app.log for details.");
            }
            return;
    }
}

void DoCapture() {
    if (g_busy) {
        Toast::Show(L"Busy", L"A capture is already in progress.");
        return;
    }
    BusyGuard guard;

    Overlay::Selection sel;
    if (!Overlay::Show(sel)) {
        Toast::Show(L"Capture failed", L"The selection overlay could not be shown.");
        return;
    }

    switch (sel.result) {
        case Overlay::Result::Cancelled:
            return;

        case Overlay::Result::Region: {
            std::unique_ptr<Bitmap> image(sel.image);
            if (!image) {
                // The overlay normally hands back a crop of its own snapshot;
                // if that failed, capture the region live instead.
                image.reset(ScreenGrab::CaptureRect(sel.rect, OptionsFromConfig()));
            }
            if (!image) {
                Toast::Show(L"Capture failed", L"The selected region could not be captured.");
                return;
            }
            Deliver(image.get(), sel.rect, L"Region captured");
            return;
        }

        case Overlay::Result::AutoScroll:
            delete sel.image;
            ScrollSession::Run(sel.rect, ScrollSession::Mode::Auto);
            return;

        case Overlay::Result::ManualScroll:
            delete sel.image;
            ScrollSession::Run(sel.rect, ScrollSession::Mode::Manual);
            return;

        case Overlay::Result::ExtractText: {
            // The overlay hands back a crop of its own snapshot - the exact
            // pixels the user highlighted, which is what OCR should read.
            std::unique_ptr<Bitmap> image(sel.image);
            if (!image) {
                image.reset(ScreenGrab::CaptureRect(sel.rect, OptionsFromConfig()));
            }
            if (!image) {
                Toast::Show(L"Capture failed",
                            L"The selected region could not be captured.");
                return;
            }

            // Blocks until the word-selection window is done. Produces no
            // file, so the gallery is deliberately not told anything.
            const OcrOverlay::Result ocr = OcrOverlay::Show(sel.rect, image.get());
            DeliverExtractedText(ocr, sel.rect);
            return;
        }
    }
}

}  // namespace CaptureController
