#ifndef PDFEXPORT_H
#define PDFEXPORT_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>

class PDFExporter {
public:
    static bool ExportImagesToPDF(const std::vector<Gdiplus::Bitmap*>& bitmaps, const std::wstring& pdfFilePath, bool eachImageOnOwnPage = true);
};

#endif // PDFEXPORT_H
