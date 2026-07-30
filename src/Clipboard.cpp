#include "Clipboard.h"
#include "Utils.h"
#include "Logger.h"
#include <shlwapi.h>

CClipboardDataObject::CClipboardDataObject(Gdiplus::Bitmap* bitmap) {
    if (bitmap) {
        m_bitmap = bitmap->Clone(0, 0, bitmap->GetWidth(), bitmap->GetHeight(), PixelFormat32bppARGB);
    }
    m_cfPng = RegisterClipboardFormatW(L"PNG");
}

CClipboardDataObject::~CClipboardDataObject() {
    if (m_bitmap) {
        delete m_bitmap;
        m_bitmap = nullptr;
    }
}

STDMETHODIMP CClipboardDataObject::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
        *ppvObject = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CClipboardDataObject::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) CClipboardDataObject::Release() {
    ULONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0) {
        delete this;
    }
    return ref;
}

STDMETHODIMP CClipboardDataObject::QueryGetData(FORMATETC* pformatetc) {
    if (!pformatetc) return E_INVALIDARG;
    if (!(pformatetc->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;

    if (pformatetc->cfFormat == CF_DIBV5 || pformatetc->cfFormat == CF_DIB || pformatetc->cfFormat == m_cfPng) {
        return S_OK;
    }
    return DV_E_FORMATETC;
}

STDMETHODIMP CClipboardDataObject::GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) {
    if (!pformatetcIn || !pmedium) return E_INVALIDARG;
    pmedium->tymed = TYMED_NULL;
    pmedium->hGlobal = NULL;
    pmedium->pUnkForRelease = NULL;

    if (!m_bitmap) return E_FAIL;

    if (pformatetcIn->cfFormat == m_cfPng && (pformatetcIn->tymed & TYMED_HGLOBAL)) {
        HGLOBAL hMem = RenderPNG();
        if (hMem) {
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = hMem;
            return S_OK;
        }
    } else if ((pformatetcIn->cfFormat == CF_DIBV5 || pformatetcIn->cfFormat == CF_DIB) && (pformatetcIn->tymed & TYMED_HGLOBAL)) {
        HGLOBAL hMem = RenderDIBV5();
        if (hMem) {
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = hMem;
            return S_OK;
        }
    }
    return DV_E_FORMATETC;
}

STDMETHODIMP CClipboardDataObject::GetDataHere(FORMATETC*, STGMEDIUM*) { return E_NOTIMPL; }
STDMETHODIMP CClipboardDataObject::GetCanonicalFormatEtc(FORMATETC*, FORMATETC* pformatetcOut) {
    if (pformatetcOut) pformatetcOut->ptd = NULL;
    return E_NOTIMPL;
}
STDMETHODIMP CClipboardDataObject::SetData(FORMATETC*, STGMEDIUM*, BOOL) { return E_NOTIMPL; }
STDMETHODIMP CClipboardDataObject::EnumFormatEtc(DWORD, IEnumFORMATETC**) { return E_NOTIMPL; }
STDMETHODIMP CClipboardDataObject::DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) { return OLE_E_ADVISENOTSUPPORTED; }
STDMETHODIMP CClipboardDataObject::DUnadvise(DWORD) { return OLE_E_ADVISENOTSUPPORTED; }
STDMETHODIMP CClipboardDataObject::EnumDAdvise(IEnumSTATDATA**) { return OLE_E_ADVISENOTSUPPORTED; }

HGLOBAL CClipboardDataObject::RenderPNG() {
    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(NULL, FALSE, &pStream))) return nullptr;

    CLSID clsid;
    Utils::GetEncoderClsid(L"image/png", &clsid);
    m_bitmap->Save(pStream, &clsid, NULL);

    HGLOBAL hGlobalStream = NULL;
    GetHGlobalFromStream(pStream, &hGlobalStream);

    STATSTG stat;
    pStream->Stat(&stat, STATFLAG_NONAME);
    SIZE_T streamSize = (SIZE_T)stat.cbSize.QuadPart;

    HGLOBAL hMem = GlobalAlloc(GHND, streamSize);
    if (hMem) {
        void* pDst = GlobalLock(hMem);
        void* pSrc = GlobalLock(hGlobalStream);
        if (pDst && pSrc) {
            memcpy(pDst, pSrc, streamSize);
        }
        if (pSrc) GlobalUnlock(hGlobalStream);
        if (pDst) GlobalUnlock(hMem);
    }

    pStream->Release();
    return hMem;
}

HGLOBAL CClipboardDataObject::RenderDIBV5() {
    UINT width = m_bitmap->GetWidth();
    UINT height = m_bitmap->GetHeight();

    Gdiplus::BitmapData bmpData;
    Gdiplus::Rect rect(0, 0, width, height);

    if (m_bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData) != Gdiplus::Ok) {
        return nullptr;
    }

    DWORD imageSize = width * height * 4;
    DWORD totalSize = sizeof(BITMAPV5HEADER) + imageSize;

    HGLOBAL hMem = GlobalAlloc(GHND, totalSize);
    if (!hMem) {
        m_bitmap->UnlockBits(&bmpData);
        return nullptr;
    }

    BYTE* pBuffer = static_cast<BYTE*>(GlobalLock(hMem));
    if (!pBuffer) {
        GlobalFree(hMem);
        m_bitmap->UnlockBits(&bmpData);
        return nullptr;
    }

    BITMAPV5HEADER* pBmi = reinterpret_cast<BITMAPV5HEADER*>(pBuffer);
    ZeroMemory(pBmi, sizeof(BITMAPV5HEADER));
    pBmi->bV5Size = sizeof(BITMAPV5HEADER);
    pBmi->bV5Width = width;
    pBmi->bV5Height = -((int)height); // Top-down
    pBmi->bV5Planes = 1;
    pBmi->bV5BitCount = 32;
    pBmi->bV5Compression = BI_BITFIELDS;
    pBmi->bV5RedMask   = 0x00FF0000;
    pBmi->bV5GreenMask = 0x0000FF00;
    pBmi->bV5BlueMask  = 0x000000FF;
    pBmi->bV5AlphaMask = 0xFF000000;

    BYTE* pPixelDest = pBuffer + sizeof(BITMAPV5HEADER);
    BYTE* pPixelSrc = static_cast<BYTE*>(bmpData.Scan0);

    for (UINT y = 0; y < height; ++y) {
        memcpy(pPixelDest + (y * width * 4), pPixelSrc + (y * bmpData.Stride), width * 4);
    }

    GlobalUnlock(hMem);
    m_bitmap->UnlockBits(&bmpData);

    return hMem;
}

bool CClipboardDataObject::CopyToClipboard(Gdiplus::Bitmap* bitmap) {
    if (!bitmap) return false;

    CClipboardDataObject* pDataObject = new CClipboardDataObject(bitmap);
    HRESULT hr = OleSetClipboard(pDataObject);
    if (SUCCEEDED(hr)) {
        OleFlushClipboard();
        pDataObject->Release();
        LOG_INFO("Successfully copied image to clipboard via OleSetClipboard.");
        return true;
    } else {
        LOG_ERROR("OleSetClipboard failed.");
        pDataObject->Release();
        return false;
    }
}
