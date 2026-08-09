#include "IconCache.h"

#include "Logger.h"
#include "Utils.h"
#include "resource.h"

#include <map>

using namespace Gdiplus;

namespace Icons {
namespace {

struct CacheKey {
    int id;
    int size;
    COLORREF tint;

    bool operator<(const CacheKey& o) const {
        if (id != o.id) return id < o.id;
        if (size != o.size) return size < o.size;
        return tint < o.tint;
    }
};

std::map<int, Bitmap*> g_originals;          // decoded source art, by resource id
std::map<CacheKey, Bitmap*> g_scaled;        // rendered variants

int ResourceIdFor(Id id) {
    switch (id) {
        case Id::Check:  return IDR_ICON_CHECK;
        case Id::Gear:   return IDR_ICON_GEAR;
        case Id::Cancel: return IDR_ICON_CANCEL;
        case Id::Delete: return IDR_ICON_DELETE;
        case Id::Redo:   return IDR_ICON_REDO;
    }
    return 0;
}

Bitmap* Original(int resourceId) {
    auto it = g_originals.find(resourceId);
    if (it != g_originals.end()) return it->second;

    Bitmap* bmp = Utils::LoadEmbeddedPng(resourceId);
    g_originals[resourceId] = bmp;  // cache nullptr too, so a missing resource
                                    // is not re-decoded on every paint
    return bmp;
}

Bitmap* Render(Id id, int sizePx, bool tinted, Color color) {
    if (sizePx <= 0) return nullptr;

    const int resourceId = ResourceIdFor(id);
    const CacheKey key{resourceId, sizePx, tinted ? color.ToCOLORREF() : 0xFFFFFFFF};

    auto it = g_scaled.find(key);
    if (it != g_scaled.end()) return it->second;

    Bitmap* src = Original(resourceId);
    Bitmap* result = nullptr;
    if (src) {
        // Scale first, then tint: tinting a large source and scaling the
        // result would resample the tinted edges twice.
        std::unique_ptr<Bitmap> scaled(Utils::ScaleBitmap(src, sizePx, sizePx));
        if (scaled) {
            if (tinted) {
                result = Utils::TintBitmap(scaled.get(), color);
            } else {
                result = scaled.release();
            }
        }
    }

    g_scaled[key] = result;
    return result;
}

}  // namespace

Bitmap* Get(Id id, int sizePx) {
    return Render(id, sizePx, false, Color(255, 255, 255, 255));
}

Bitmap* GetTinted(Id id, int sizePx, Color color) {
    return Render(id, sizePx, true, color);
}

void Shutdown() {
    for (auto& kv : g_scaled) delete kv.second;
    g_scaled.clear();
    for (auto& kv : g_originals) delete kv.second;
    g_originals.clear();
}

}  // namespace Icons
