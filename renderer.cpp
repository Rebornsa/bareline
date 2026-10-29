#include "renderer.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")

static ID2D1Factory*       s_pD2DFactory = nullptr;
static IWICImagingFactory* s_pWicFactory = nullptr;
static IDWriteFactory*     s_pDWriteFactory = nullptr;
static IDWriteTextFormat*  s_pTextFormat = nullptr;
static ID2D1DCRenderTarget* s_pDCRT = nullptr;

bool InitRenderer() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &s_pD2DFactory);
    if (FAILED(hr)) return false;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_pWicFactory));
    if (FAILED(hr)) return false;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&s_pDWriteFactory));
    if (FAILED(hr)) return false;
    hr = s_pDWriteFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.0f, L"en-us", &s_pTextFormat);
    if (FAILED(hr)) { hr = s_pDWriteFactory->CreateTextFormat(L"Courier New", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.0f, L"en-us", &s_pTextFormat); }
    if (FAILED(hr)) { hr = s_pDWriteFactory->CreateTextFormat(L"Microsoft Sans Serif", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.0f, L"en-us", &s_pTextFormat); if (FAILED(hr)) return false; }
    s_pTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
    hr = s_pD2DFactory->CreateDCRenderTarget(&props, &s_pDCRT);
    return SUCCEEDED(hr);
}

void ShutdownRenderer() {
    if (s_pDCRT) { s_pDCRT->Release(); s_pDCRT = nullptr; }
    if (s_pTextFormat) { s_pTextFormat->Release(); s_pTextFormat = nullptr; }
    if (s_pDWriteFactory) { s_pDWriteFactory->Release(); s_pDWriteFactory = nullptr; }
    if (s_pWicFactory) { s_pWicFactory->Release(); s_pWicFactory = nullptr; }
    if (s_pD2DFactory) { s_pD2DFactory->Release(); s_pD2DFactory = nullptr; }
}

SIZE Render(HWND hwnd, const std::vector<std::wstring>& lines, int opacityPercent, int fontSize) {
    static float s_lastFontSize = -1.0f;
    if (s_pTextFormat && s_lastFontSize != (float)fontSize) { s_pTextFormat->Release(); s_pTextFormat = nullptr; }
    if (!s_pTextFormat) {
        float fs = (float)fontSize;
        HRESULT hr = s_pDWriteFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fs, L"en-us", &s_pTextFormat);
        if (FAILED(hr)) { hr = s_pDWriteFactory->CreateTextFormat(L"Courier New", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fs, L"en-us", &s_pTextFormat); }
        if (FAILED(hr)) { s_pDWriteFactory->CreateTextFormat(L"Microsoft Sans Serif", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fs, L"en-us", &s_pTextFormat); }
        s_lastFontSize = fs;
    }
    float fs = (float)fontSize;
    s_pTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    const float lineHeight = fs + 5; const float padding = fs * 0.6f + 6.0f; float maxWidth = 0.0f;
    for (auto& line : lines) { IDWriteTextLayout* pLayout = nullptr; s_pDWriteFactory->CreateTextLayout(line.c_str(), (UINT32)line.length(), s_pTextFormat, 3000.0f, 20.0f, &pLayout); DWRITE_TEXT_METRICS metrics; pLayout->GetMetrics(&metrics); if (metrics.width > maxWidth) maxWidth = metrics.width; pLayout->Release(); }
    int w = (int)ceil(maxWidth) + (int)(padding * 2) + 10; int h = (int)(lines.size() * lineHeight) + (int)(padding * 2) + 10;
    if (w < 140) w = 140; if (h < 24) h = 24;
    HDC hdcScreen = GetDC(nullptr); HDC hdcMem = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bmi = {}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    void* pBits = nullptr; HBITMAP hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0); HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);
    RECT rc = { 0, 0, w, h }; s_pDCRT->BindDC(hdcMem, &rc); s_pDCRT->BeginDraw(); s_pDCRT->Clear(D2D1::ColorF(0, 0, 0, 0));
    ID2D1SolidColorBrush* pBg = nullptr;
    s_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.02f, 0.02f, 0.25f), &pBg);
    D2D1_RECT_F bgRect = D2D1::RectF(0, 0, (float)w, (float)h);
    s_pDCRT->FillRectangle(&bgRect, pBg);
    pBg->Release();
    ID2D1SolidColorBrush* pWhite = nullptr; ID2D1SolidColorBrush* pBlack = nullptr;
    s_pDCRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &pWhite); s_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &pBlack);
    float y = padding + 1;
    for (auto& line : lines) {
        D2D1_RECT_F r;
        for (int dy = -1; dy <= 1; dy++) { for (int dx = -1; dx <= 1; dx++) { if (dx == 0 && dy == 0) continue;
                r = D2D1::RectF(padding + 1 + dx, y + dy, padding + 1 + maxWidth + 20, y + lineHeight + dy);
                s_pDCRT->DrawText(line.c_str(), (UINT32)line.length(), s_pTextFormat, &r, pBlack, D2D1_DRAW_TEXT_OPTIONS(0x00000004)); } }
        r = D2D1::RectF(padding + 1, y, padding + 1 + maxWidth + 20, y + lineHeight);
        s_pDCRT->DrawText(line.c_str(), (UINT32)line.length(), s_pTextFormat, &r, pWhite, D2D1_DRAW_TEXT_OPTIONS(0x00000004));
        y += lineHeight;
    }
    s_pDCRT->EndDraw(); pWhite->Release(); pBlack->Release();
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, (BYTE)(opacityPercent * 255 / 100), AC_SRC_ALPHA };
    POINT ptSrc = { 0, 0 }; SIZE sz = { w, h }; POINT ptDst = { 0, 0 };
    HDC hdcScr2 = GetDC(nullptr); UpdateLayeredWindow(hwnd, hdcScr2, nullptr, &sz, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA); ReleaseDC(nullptr, hdcScr2);
    SelectObject(hdcMem, hOld); DeleteObject(hBmp); DeleteDC(hdcMem); ReleaseDC(nullptr, hdcScreen);
    SIZE result = { w, h }; return result;
}
