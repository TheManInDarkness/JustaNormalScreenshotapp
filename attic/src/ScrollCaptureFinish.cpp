#include "CaptureController.h"
#include "Clipboard.h"
#include "Common.h"
#include "GalleryWindow.h"
#include "Logger.h"
#include "ScrollCapture.h"
#include "ScrollPreview.h"
#include "ScrollStitcher.h"
#include "Settings.h"
#include "Toast.h"
#include "Utils.h"

using namespace Gdiplus;

namespace ScrollCapture {

Outcome FinishAndDeliver(const std::vector<Bitmap*>& frames, const RECT& region,
                         bool allowRedo) {
    if (frames.empty()) return Outcome::Discarded;

    ScrollStitcher::Report report;
    std::unique_ptr<Bitmap> stitched(ScrollStitcher::StitchScrollFrames(
        frames, Settings::Get().scrollRemoveOverlap, &report));

    if (!stitched) {
        Toast::Show(L"Stitching failed",
                    L"The captured frames could not be joined. See app.log for details.");
        return Outcome::Discarded;
    }

    Logger::Infof(L"Stitched %d frames into %ux%u (header %dpx, footer %dpx, "
                  L"%d uncertain seams)",
                  report.stripCount, stitched->GetWidth(), stitched->GetHeight(),
                  report.fixedHeaderRows, report.fixedFooterRows, report.uncertainSeams);

    // The preview is deliberately in front of the save: a bad seam is much
    // cheaper to notice here than in a file the user has already filed away.
    const ScrollPreview::Choice choice =
        ScrollPreview::Show(Gallery::GetWindow(), stitched.get(), report, allowRedo);

    if (choice == ScrollPreview::Choice::RedoLast) return Outcome::RedoLast;
    if (choice != ScrollPreview::Choice::Accept) {
        Logger::Info(L"Scroll capture result discarded by the user");
        return Outcome::Discarded;
    }

    // Stitched results get their own name so they stand out from ordinary
    // screenshots in the folder and in the gallery.
    const AppConfig& cfg = Settings::Get();
    const std::wstring baseName =
        L"Scrollshot_" + Utils::ExpandFilenamePattern(L"%Y-%m-%d_%H-%M-%S");

    const std::wstring savedPath =
        CaptureController::SaveWithBaseName(stitched.get(), baseName);

    bool copied = false;
    if (cfg.copyToClipboard) copied = Clipboard::CopyBitmap(stitched.get());

    if (savedPath.empty()) {
        Toast::Show(L"Scroll capture not saved",
                    copied ? L"It is on the clipboard, but writing the file failed."
                           : L"The stitched image could not be saved.");
        return Outcome::Discarded;
    }

    Gallery::NotifyCaptureSaved(savedPath);

    if (!cfg.showNotifications) return Outcome::Delivered;

    const std::wstring detail = (copied ? L"Copied and saved as " : L"Saved as ") +
                                Utils::GetFileNameFromPath(savedPath);
    Toast::Show(L"Scroll capture complete", detail + L"  (click to show)",
                [savedPath]() { Utils::OpenFolderAndSelect(savedPath); });
    return Outcome::Delivered;
}

}  // namespace ScrollCapture
