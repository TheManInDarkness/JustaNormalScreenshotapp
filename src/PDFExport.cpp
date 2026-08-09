#include "PDFExport.h"

#include "AppPaths.h"
#include "Logger.h"
#include "Settings.h"
#include "TextUtil.h"
#include "Toast.h"
#include "Utils.h"

#include <algorithm>

extern "C" {
#include "pdfgen.h"
}

using namespace Gdiplus;

namespace PDFExport {
namespace {

// RAII for the PDFGen document, which is a C struct with its own destroy.
struct PdfDoc {
    struct pdf_doc* doc = nullptr;
    ~PdfDoc() {
        if (doc) pdf_destroy(doc);
    }
};

const wchar_t* MimeFor(const Options& opt) {
    // PDFGen embeds JPEG or *non-alpha* PNG directly. Utils flattens 32bpp
    // sources to 24bpp RGB when encoding PNG, which is what makes the PNG
    // path acceptable to pdf_add_image_data.
    return opt.useJpeg ? L"image/jpeg" : L"image/png";
}

bool AddBandToPage(struct pdf_doc* doc, struct pdf_object* page, Bitmap* source,
                   const pdfslice::SliceBand& band, const Options& opt,
                   std::wstring& error) {
    std::unique_ptr<Bitmap> cropped(Utils::CropBitmap(
        source, Rect(0, band.startY, static_cast<INT>(source->GetWidth()), band.height)));
    if (!cropped) {
        error = L"Could not crop a page band from the image.";
        return false;
    }

    std::vector<uint8_t> encoded;
    if (!Utils::EncodeBitmapToMemory(cropped.get(), MimeFor(opt), opt.jpegQuality,
                                     encoded) ||
        encoded.empty()) {
        error = L"Could not encode a page band.";
        return false;
    }

    const pdfslice::PlacedImage placed = pdfslice::PlaceBandOnPage(
        static_cast<int>(cropped->GetWidth()), static_cast<int>(cropped->GetHeight()),
        opt.slice);

    // PDFGen's origin is the bottom-left of the page; PlaceBandOnPage has
    // already converted "anchored to the top" into that coordinate space.
    if (pdf_add_image_data(doc, page, static_cast<float>(placed.xPt),
                           static_cast<float>(placed.yPt),
                           static_cast<float>(placed.widthPt),
                           static_cast<float>(placed.heightPt), encoded.data(),
                           encoded.size()) < 0) {
        const char* msg = pdf_get_err(doc, nullptr);
        error = L"PDFGen rejected an image: " +
                TextUtil::Widen(msg ? msg : "unknown error");
        return false;
    }
    return true;
}

// One page sized to the image: the page itself is resized, then the image
// fills it between the margins.
bool AddImageAsLongPage(struct pdf_doc* doc, struct pdf_object* page, Bitmap* source,
                        const Options& opt, std::wstring& error) {
    std::vector<uint8_t> encoded;
    if (!Utils::EncodeBitmapToMemory(source, MimeFor(opt), opt.jpegQuality, encoded) ||
        encoded.empty()) {
        error = L"Could not encode the image.";
        return false;
    }

    const pdfslice::LongPage placed = pdfslice::ComputeLongPage(
        static_cast<int>(source->GetWidth()), static_cast<int>(source->GetHeight()),
        opt.slice);
    if (placed.pageHeightPt <= 0) {
        error = L"The image is not a usable size.";
        return false;
    }

    if (pdf_page_set_size(doc, page, static_cast<float>(placed.pageWidthPt),
                          static_cast<float>(placed.pageHeightPt)) < 0) {
        const char* msg = pdf_get_err(doc, nullptr);
        error = L"PDFGen rejected the page size: " +
                TextUtil::Widen(msg ? msg : "unknown error");
        return false;
    }

    if (pdf_add_image_data(doc, page, static_cast<float>(placed.image.xPt),
                           static_cast<float>(placed.image.yPt),
                           static_cast<float>(placed.image.widthPt),
                           static_cast<float>(placed.image.heightPt), encoded.data(),
                           encoded.size()) < 0) {
        const char* msg = pdf_get_err(doc, nullptr);
        error = L"PDFGen rejected the image: " +
                TextUtil::Widen(msg ? msg : "unknown error");
        return false;
    }
    return true;
}

bool AddWholeImageToPage(struct pdf_doc* doc, struct pdf_object* page, Bitmap* source,
                         const Options& opt, std::wstring& error) {
    std::vector<uint8_t> encoded;
    if (!Utils::EncodeBitmapToMemory(source, MimeFor(opt), opt.jpegQuality, encoded) ||
        encoded.empty()) {
        error = L"Could not encode the image.";
        return false;
    }

    const pdfslice::PlacedImage placed = pdfslice::FitWholeImageOnPage(
        static_cast<int>(source->GetWidth()), static_cast<int>(source->GetHeight()),
        opt.slice);

    if (pdf_add_image_data(doc, page, static_cast<float>(placed.xPt),
                           static_cast<float>(placed.yPt),
                           static_cast<float>(placed.widthPt),
                           static_cast<float>(placed.heightPt), encoded.data(),
                           encoded.size()) < 0) {
        const char* msg = pdf_get_err(doc, nullptr);
        error = L"PDFGen rejected the image: " +
                TextUtil::Widen(msg ? msg : "unknown error");
        return false;
    }
    return true;
}

}  // namespace

std::vector<int> ComputeSliceLinePositions(Bitmap* image, const Options& options) {
    std::vector<int> lines;
    // Only the sliced layout has cuts to preview.
    if (!image || options.layout != Layout::SlicedPages) return lines;

    const int w = static_cast<int>(image->GetWidth());
    const int h = static_cast<int>(image->GetHeight());
    const int sliceH = pdfslice::ComputeSliceHeightPx(w, options.slice);
    if (sliceH <= 0) return lines;

    const auto bands = pdfslice::ComputeSliceBands(h, sliceH);
    for (size_t i = 1; i < bands.size(); ++i) lines.push_back(bands[i].startY);
    return lines;
}

Options OptionsFromConfig() {
    const AppConfig& cfg = Settings::Get();

    Options opt;
    opt.slice.pageSize = cfg.pdfPageSize == PdfPageSize::Letter
                             ? pdfslice::PageSize::Letter
                             : pdfslice::PageSize::A4;
    opt.layout = cfg.pdfLayout == PdfLayout::SlicedPages ? Layout::SlicedPages
                                                         : Layout::OneLongPage;
    // Kept in step with the layout: the page-count helper reads this flag.
    opt.slice.sliceTallImages = (opt.layout == Layout::SlicedPages);
    opt.useJpeg = cfg.imageFormat == ImageFormat::Jpeg;
    opt.jpegQuality = cfg.jpegQuality;
    return opt;
}

std::wstring SaveConfiguredPdf(HWND parent, Bitmap* image,
                               const std::wstring& baseName) {
    if (!image) return std::wstring();

    const AppConfig& cfg = Settings::Get();
    std::wstring outputPath;

    if (cfg.pdfAskWhereToSave) {
        wchar_t fileName[MAX_PATH] = {};
        wcsncpy_s(fileName, (baseName + L".pdf").c_str(), _TRUNCATE);

        std::wstring initialDir = cfg.pdfFolderPath;
        if (initialDir.empty()) initialDir = cfg.saveFolderPath;

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = parent;
        ofn.lpstrFilter = L"PDF document\0*.pdf\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = ARRAYSIZE(fileName);
        ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
        ofn.lpstrTitle = L"Save the capture as a PDF";
        ofn.lpstrDefExt = L"pdf";
        ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

        if (!GetSaveFileNameW(&ofn)) return std::wstring();  // cancelled
        outputPath = fileName;
    } else {
        std::wstring folder = cfg.pdfFolderPath;
        if (folder.empty()) folder = cfg.saveFolderPath;
        if (folder.empty()) folder = AppPaths::GetDefaultSaveFolder();

        if (!AppPaths::EnsureDirectory(folder)) {
            Logger::Errorf(L"Cannot create the PDF folder %s", folder.c_str());
            Toast::Show(L"PDF not created",
                        L"The PDF folder could not be created. Check it in Settings.");
            return std::wstring();
        }
        outputPath = Utils::MakeUniquePath(folder, baseName, L"pdf");
    }

    const Result result = ExportImages({image}, outputPath, OptionsFromConfig());
    if (!result.success) {
        Logger::Errorf(L"Automatic PDF failed: %s", result.error.c_str());
        Toast::Show(L"PDF not created", result.error);
        return std::wstring();
    }
    return outputPath;
}

Result ExportImages(const std::vector<Bitmap*>& images, const std::wstring& outputPath,
                    const Options& options) {
    Result result;

    if (images.empty()) {
        result.error = L"No images to convert.";
        return result;
    }

    const pdfslice::PageDims page = pdfslice::GetPageDims(options.slice.pageSize);

    struct pdf_info info = {};
    strncpy(info.creator, "ScreenshotApp", sizeof(info.creator) - 1);
    strncpy(info.producer, "ScreenshotApp (PDFGen)", sizeof(info.producer) - 1);
    strncpy(info.title, "Screenshots", sizeof(info.title) - 1);

    PdfDoc pdf;
    pdf.doc = pdf_create(static_cast<float>(page.widthPt),
                         static_cast<float>(page.heightPt), &info);
    if (!pdf.doc) {
        result.error = L"Could not create the PDF document.";
        return result;
    }

    for (Bitmap* image : images) {
        if (!image || image->GetLastStatus() != Ok) continue;

        const int w = static_cast<int>(image->GetWidth());
        const int h = static_cast<int>(image->GetHeight());
        if (w <= 0 || h <= 0) continue;

        if (options.layout != Layout::SlicedPages) {
            struct pdf_object* p = pdf_append_page(pdf.doc);
            if (!p) {
                result.error = L"Could not append a page.";
                return result;
            }
            const bool ok =
                options.layout == Layout::OneLongPage
                    ? AddImageAsLongPage(pdf.doc, p, image, options, result.error)
                    : AddWholeImageToPage(pdf.doc, p, image, options, result.error);
            if (!ok) return result;
            ++result.pagesWritten;
            continue;
        }

        const int sliceH = pdfslice::ComputeSliceHeightPx(w, options.slice);
        const auto bands = pdfslice::ComputeSliceBands(h, sliceH);
        if (bands.empty()) continue;

        for (const auto& band : bands) {
            struct pdf_object* p = pdf_append_page(pdf.doc);
            if (!p) {
                result.error = L"Could not append a page.";
                return result;
            }
            if (!AddBandToPage(pdf.doc, p, image, band, options, result.error)) {
                return result;
            }
            ++result.pagesWritten;
        }
    }

    if (result.pagesWritten == 0) {
        result.error = L"Nothing usable to write - every image was empty or unreadable.";
        return result;
    }

    // pdf_save takes a narrow path, so a folder with non-ASCII characters
    // needs the active codepage rather than UTF-8.
    const int need = WideCharToMultiByte(CP_ACP, 0, outputPath.c_str(), -1, nullptr, 0,
                                         nullptr, nullptr);
    if (need <= 0) {
        result.error = L"The output path could not be converted for writing.";
        return result;
    }
    std::vector<char> narrowPath(need);
    WideCharToMultiByte(CP_ACP, 0, outputPath.c_str(), -1, narrowPath.data(), need,
                        nullptr, nullptr);

    if (pdf_save(pdf.doc, narrowPath.data()) < 0) {
        const char* msg = pdf_get_err(pdf.doc, nullptr);
        result.error = L"Writing the PDF failed: " +
                       TextUtil::Widen(msg ? msg : "unknown error");
        return result;
    }

    Logger::Infof(L"Wrote %s (%d pages)", outputPath.c_str(), result.pagesWritten);
    result.success = true;
    return result;
}

}  // namespace PDFExport
