#include "ImageListView.h"

#include "DpiHelper.h"
#include "Logger.h"
#include "Theme.h"
#include "Utils.h"

#include <algorithm>

using namespace Gdiplus;

namespace {

// Thumbnails are square in the image list; the picture is letterboxed inside
// so different aspect ratios still line up in a grid.
constexpr int kThumbBase = 72;

HBITMAP RenderThumbnailTile(Bitmap* source, int size, COLORREF backdrop) {
    void* bits = nullptr;
    HBITMAP dib = Utils::CreateDIBSection32(size, size, &bits);
    if (!dib) return nullptr;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ prev = SelectObject(mem, dib);

    {
        Graphics g(mem);
        g.Clear(Color(255, GetRValue(backdrop), GetGValue(backdrop), GetBValue(backdrop)));

        if (source) {
            const int sw = static_cast<int>(source->GetWidth());
            const int sh = static_cast<int>(source->GetHeight());
            if (sw > 0 && sh > 0) {
                const double scale =
                    (std::min)(static_cast<double>(size - 6) / sw,
                               static_cast<double>(size - 6) / sh);
                const int w = (std::max)(1, static_cast<int>(sw * scale));
                const int h = (std::max)(1, static_cast<int>(sh * scale));
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
                ImageAttributes attr;
                attr.SetWrapMode(WrapModeTileFlipXY);
                g.DrawImage(source, Rect((size - w) / 2, (size - h) / 2, w, h), 0, 0, sw,
                            sh, UnitPixel, &attr);

                Pen edge(Color(90, 128, 128, 128));
                g.DrawRectangle(&edge, (size - w) / 2, (size - h) / 2, w - 1, h - 1);
            }
        }
    }

    SelectObject(mem, prev);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return dib;
}

}  // namespace

ImageListView::~ImageListView() { Detach(); }

int ImageListView::ThumbSize() const { return Dpi::Scale(kThumbBase, dpi_); }

void ImageListView::Attach(HWND listView, UINT dpi) {
    list_ = listView;
    dpi_ = dpi ? dpi : 96;
    if (!list_) return;

    ListView_SetExtendedListViewStyle(
        list_, LVS_EX_DOUBLEBUFFER | LVS_EX_BORDERSELECT | LVS_EX_INFOTIP);

    const Theme::Palette& pal = Theme::Current();
    ListView_SetBkColor(list_, pal.surface);
    ListView_SetTextBkColor(list_, pal.surface);
    ListView_SetTextColor(list_, pal.text);

    RebuildImageList();
}

void ImageListView::Detach() {
    if (dragImage_) {
        ImageList_Destroy(dragImage_);
        dragImage_ = nullptr;
    }
    if (list_) {
        ListView_SetImageList(list_, nullptr, LVSIL_NORMAL);
    }
    if (imageList_) {
        ImageList_Destroy(imageList_);
        imageList_ = nullptr;
    }
    entries_.clear();
    list_ = nullptr;
}

void ImageListView::RebuildImageList() {
    if (!list_) return;

    const int size = ThumbSize();
    HIMAGELIST fresh = ImageList_Create(size, size, ILC_COLOR32, 8, 8);
    if (!fresh) return;

    const COLORREF backdrop = Theme::Current().canvasBackdrop;
    for (const auto& entry : entries_) {
        HBITMAP tile = RenderThumbnailTile(entry->image.get(), size, backdrop);
        if (tile) {
            ImageList_Add(fresh, tile, nullptr);
            DeleteObject(tile);
        } else {
            // Keep indices aligned with entries_ even if one render failed.
            HBITMAP blank = RenderThumbnailTile(nullptr, size, backdrop);
            ImageList_Add(fresh, blank, nullptr);
            if (blank) DeleteObject(blank);
        }
    }

    ListView_SetImageList(list_, fresh, LVSIL_NORMAL);
    if (imageList_) ImageList_Destroy(imageList_);
    imageList_ = fresh;
}

void ImageListView::RefreshItems() {
    if (!list_) return;

    const int selected = Selection();
    ListView_DeleteAllItems(list_);

    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = i;
        item.iImage = i;
        // The buffer must stay alive for the duration of the call, hence the
        // local copy rather than pointing straight at the std::wstring.
        std::wstring label = entries_[i]->label;
        item.pszText = const_cast<LPWSTR>(label.c_str());
        ListView_InsertItem(list_, &item);
    }

    if (selected >= 0 && selected < static_cast<int>(entries_.size())) {
        SetSelection(selected);
    }
}

int ImageListView::AddFile(const std::wstring& path) {
    std::unique_ptr<Bitmap> image(Utils::LoadImageFromFile(path));
    if (!image) {
        Logger::Warnf(L"Could not load %s", path.c_str());
        return -1;
    }

    auto entry = std::make_unique<Entry>();
    entry->label = Utils::GetFileNameFromPath(path);
    entry->path = path;
    entry->image = std::move(image);
    entries_.push_back(std::move(entry));

    RebuildImageList();
    RefreshItems();
    return static_cast<int>(entries_.size()) - 1;
}

int ImageListView::AddBitmap(const std::wstring& label, Bitmap* owned) {
    if (!owned) return -1;

    auto entry = std::make_unique<Entry>();
    entry->label = label;
    entry->image.reset(owned);
    entries_.push_back(std::move(entry));

    RebuildImageList();
    RefreshItems();
    return static_cast<int>(entries_.size()) - 1;
}

void ImageListView::RemoveAt(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return;
    entries_.erase(entries_.begin() + index);
    RebuildImageList();
    RefreshItems();

    if (!entries_.empty()) {
        SetSelection((std::min)(index, static_cast<int>(entries_.size()) - 1));
    }
}

void ImageListView::Clear() {
    entries_.clear();
    RebuildImageList();
    RefreshItems();
}

bool ImageListView::MoveItem(int from, int to) {
    const int count = static_cast<int>(entries_.size());
    if (from < 0 || from >= count) return false;
    to = (std::max)(0, (std::min)(to, count - 1));
    if (from == to) return false;

    auto moved = std::move(entries_[from]);
    entries_.erase(entries_.begin() + from);
    entries_.insert(entries_.begin() + to, std::move(moved));

    RebuildImageList();
    RefreshItems();
    SetSelection(to);
    return true;
}

Bitmap* ImageListView::ImageAt(int index) const {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return nullptr;
    return entries_[index]->image.get();
}

std::wstring ImageListView::PathAt(int index) const {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return std::wstring();
    return entries_[index]->path;
}

std::vector<Bitmap*> ImageListView::AllImages() const {
    std::vector<Bitmap*> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        if (e->image) out.push_back(e->image.get());
    }
    return out;
}

int ImageListView::Selection() const {
    if (!list_) return -1;
    return ListView_GetNextItem(list_, -1, LVNI_SELECTED);
}

void ImageListView::SetSelection(int index) {
    if (!list_ || index < 0) return;
    ListView_SetItemState(list_, index, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list_, index, FALSE);
}

bool ImageListView::OnBeginDrag(NMHDR* hdr) {
    if (!list_ || !hdr || hdr->hwndFrom != list_) return false;

    NMLISTVIEW* nm = reinterpret_cast<NMLISTVIEW*>(hdr);
    dragIndex_ = nm->iItem;
    if (dragIndex_ < 0) return false;

    POINT hotspot = {};
    if (dragImage_) ImageList_Destroy(dragImage_);
    dragImage_ = ListView_CreateDragImage(list_, dragIndex_, &hotspot);

    if (dragImage_) {
        POINT pt = nm->ptAction;
        ImageList_BeginDrag(dragImage_, 0, 0, 0);
        ClientToScreen(list_, &pt);
        ImageList_DragEnter(nullptr, pt.x, pt.y);
    }

    SetCapture(GetParent(list_));
    return true;
}

bool ImageListView::OnMouseMove(POINT clientPt) {
    if (dragIndex_ < 0) return false;

    POINT screen = clientPt;
    ClientToScreen(GetParent(list_), &screen);
    if (dragImage_) ImageList_DragMove(screen.x, screen.y);
    return true;
}

bool ImageListView::OnLButtonUp(POINT clientPt) {
    if (dragIndex_ < 0) return false;

    if (dragImage_) {
        ImageList_DragLeave(nullptr);
        ImageList_EndDrag();
        ImageList_Destroy(dragImage_);
        dragImage_ = nullptr;
    }
    ReleaseCapture();

    // Translate the drop point into a list index.
    POINT listPt = clientPt;
    ClientToScreen(GetParent(list_), &listPt);
    ScreenToClient(list_, &listPt);

    LVHITTESTINFO hit = {};
    hit.pt = listPt;
    int target = ListView_HitTest(list_, &hit);

    if (target < 0) {
        // Dropped past the last item: treat it as "move to the end", which is
        // what dragging into the empty space below the icons means.
        RECT rc;
        GetClientRect(list_, &rc);
        if (PtInRect(&rc, listPt)) target = static_cast<int>(entries_.size()) - 1;
    }

    const int from = dragIndex_;
    dragIndex_ = -1;

    if (target >= 0 && target != from) MoveItem(from, target);
    return true;
}

void ImageListView::Rescale(UINT dpi) {
    if (dpi == dpi_) return;
    dpi_ = dpi ? dpi : 96;
    RebuildImageList();
    RefreshItems();
}
