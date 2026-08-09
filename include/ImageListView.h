#pragma once

#include "Common.h"

// A ListView in LVS_ICON mode backed by real thumbnails.
//
// Replaces a LISTBOX of file names: a name tells you nothing about which
// screenshot is which, and the control gives native scrolling and
// drag-reorder for free rather than needing hand-rolled equivalents.
class ImageListView {
public:
    ImageListView() = default;
    ~ImageListView();

    ImageListView(const ImageListView&) = delete;
    ImageListView& operator=(const ImageListView&) = delete;

    // Sets up icon mode, the image list and the thumbnail size for `dpi`.
    void Attach(HWND listView, UINT dpi);
    void Detach();

    // Decodes and adds a file. Returns its index, or -1 on failure.
    int AddFile(const std::wstring& path);

    // Adds an in-memory image; the view takes ownership.
    int AddBitmap(const std::wstring& label, Gdiplus::Bitmap* owned);

    void RemoveAt(int index);
    void Clear();

    // Moves an item, keeping it selected. Used by drag-reorder and the
    // Up/Down buttons.
    bool MoveItem(int from, int to);

    int Count() const { return static_cast<int>(entries_.size()); }
    Gdiplus::Bitmap* ImageAt(int index) const;
    std::wstring PathAt(int index) const;

    // In display order.
    std::vector<Gdiplus::Bitmap*> AllImages() const;

    int Selection() const;
    void SetSelection(int index);

    // Call from the parent's WM_NOTIFY for LVN_BEGINDRAG to start a
    // drag-reorder; returns true when the notification was consumed.
    bool OnBeginDrag(NMHDR* hdr);
    bool OnMouseMove(POINT clientPt);
    bool OnLButtonUp(POINT clientPt);
    bool IsDragging() const { return dragIndex_ >= 0; }

    // Re-renders every thumbnail at a new DPI.
    void Rescale(UINT dpi);

private:
    struct Entry {
        std::wstring label;
        std::wstring path;
        std::unique_ptr<Gdiplus::Bitmap> image;
    };

    void RebuildImageList();
    void RefreshItems();
    int ThumbSize() const;

    HWND list_ = nullptr;
    HIMAGELIST imageList_ = nullptr;
    std::vector<std::unique_ptr<Entry>> entries_;
    UINT dpi_ = 96;
    int dragIndex_ = -1;
    HIMAGELIST dragImage_ = nullptr;
};
