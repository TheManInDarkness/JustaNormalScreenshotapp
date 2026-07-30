#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include <windows.h>
#include <ole2.h>
#include <objidl.h>
#include <gdiplus.h>

class CClipboardDataObject : public IDataObject {
public:
    CClipboardDataObject(Gdiplus::Bitmap* bitmap);
    virtual ~CClipboardDataObject();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IDataObject
    STDMETHODIMP GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override;
    STDMETHODIMP GetDataHere(FORMATETC* pformatetc, STGMEDIUM* pmedium) override;
    STDMETHODIMP QueryGetData(FORMATETC* pformatetc) override;
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC* pformatectIn, FORMATETC* pformatetcOut) override;
    STDMETHODIMP SetData(FORMATETC* pformatetc, STGMEDIUM* pmedium, BOOL fRelease) override;
    STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override;
    STDMETHODIMP DAdvise(FORMATETC* pformatetc, DWORD advf, IAdviseSink* pAdvSink, DWORD* pdwConnection) override;
    STDMETHODIMP DUnadvise(DWORD dwConnection) override;
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA** ppenumAdvise) override;

    static bool CopyToClipboard(Gdiplus::Bitmap* bitmap);

private:
    LONG m_refCount = 1;
    Gdiplus::Bitmap* m_bitmap = nullptr;
    UINT m_cfPng = 0;

    HGLOBAL RenderDIBV5();
    HGLOBAL RenderPNG();
};

#endif // CLIPBOARD_H
