#ifndef STITCHTOOL_H
#define STITCHTOOL_H

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>

class StitchToolDialog {
public:
    static void Show(HWND hParent);

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void UpdatePreview(HWND hwnd);
    static Gdiplus::Bitmap* GenerateStitchedResult(HWND hwnd);
};

#endif // STITCHTOOL_H
