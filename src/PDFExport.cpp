#include "PDFExport.h"
#include "pdfgen.h"
#include "Utils.h"
#include "Logger.h"
#include <algorithm>

using std::min;

static bool BitmapToJpegBytes(Gdiplus::Bitmap* bitmap, std::vector<uint8_t>& outBytes) {
    if (!bitmap) return false;
    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &pStream))) return false;

    CLSID clsid;
    Utils::GetEncoderClsid(L"image/jpeg", &clsid);

    ULONG quality = 90;
    Gdiplus::EncoderParameters encoderParams;
    encoderParams.Count = 1;
    encoderParams.Parameter[0].Guid = Gdiplus::EncoderQuality;
    encoderParams.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    encoderParams.Parameter[0].NumberOfValues = 1;
    encoderParams.Parameter[0].Value = &quality;

    if (bitmap->Save(pStream, &clsid, &encoderParams) != Gdiplus::Ok) {
        pStream->Release();
        return false;
    }

    STATSTG stat;
    pStream->Stat(&stat, STATFLAG_NONAME);
    size_t size = (size_t)stat.cbSize.QuadPart;

    outBytes.resize(size);
    LARGE_INTEGER liZero = {};
    pStream->Seek(liZero, STREAM_SEEK_SET, NULL);

    ULONG bytesRead = 0;
    pStream->Read(outBytes.data(), (ULONG)size, &bytesRead);
    pStream->Release();

    return bytesRead == (ULONG)size;
}

bool PDFExporter::ExportImagesToPDF(const std::vector<Gdiplus::Bitmap*>& bitmaps, const std::wstring& pdfFilePath, bool eachImageOnOwnPage) {
    if (bitmaps.empty()) return false;

    struct pdf_info info = {};
    strcpy_s(info.creator,  sizeof(info.creator),  "ScreenshotApp");
    strcpy_s(info.producer, sizeof(info.producer), "ScreenshotApp PDF Exporter");
    strcpy_s(info.title,    sizeof(info.title),    "Stitched Screenshots");

    float pageW = 595.28f; // A4 width in points
    float pageH = 841.89f; // A4 height in points

    struct pdf_doc* pdf = pdf_create(pageW, pageH, &info);
    if (!pdf) {
        LOG_ERROR("Failed to create PDF doc.");
        return false;
    }

    if (eachImageOnOwnPage) {
        for (auto bmp : bitmaps) {
            std::vector<uint8_t> jpegBytes;
            if (!BitmapToJpegBytes(bmp, jpegBytes)) continue;

            pdf_append_page(pdf);

            float bmpW = (float)bmp->GetWidth();
            float bmpH = (float)bmp->GetHeight();

            float scale = min((pageW - 40.0f) / bmpW, (pageH - 40.0f) / bmpH);
            float dispW = bmpW * scale;
            float dispH = bmpH * scale;
            float x = (pageW - dispW) / 2.0f;
            float y = (pageH - dispH) / 2.0f;

            pdf_add_image_data(pdf, NULL, x, y, dispW, dispH, jpegBytes.data(), jpegBytes.size());
        }
    } else {
        for (auto bmp : bitmaps) {
            std::vector<uint8_t> jpegBytes;
            if (!BitmapToJpegBytes(bmp, jpegBytes)) continue;

            float bmpW = (float)bmp->GetWidth();
            float bmpH = (float)bmp->GetHeight();
            float docPageH = (bmpH / bmpW) * pageW;
            if (docPageH < 100.0f) docPageH = 100.0f;

            pdf_append_page(pdf);
            pdf_add_image_data(pdf, NULL, 0.0f, 0.0f, pageW, docPageH, jpegBytes.data(), jpegBytes.size());
        }
    }

    std::string utf8Path = Utils::WideToUtf8(pdfFilePath);
    int res = pdf_save(pdf, utf8Path.c_str());
    pdf_destroy(pdf);

    if (res < 0) {
        LOG_ERROR("Failed to save PDF to: " + utf8Path);
        return false;
    }

    LOG_INFO("PDF exported to: " + utf8Path);
    return true;
}
