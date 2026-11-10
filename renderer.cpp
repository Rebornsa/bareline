#include "renderer.h"
#include <d2d1.h>
#include <dwrite.h>
#include <cmath>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")

// Rendering strategy
// ------------------
// Everything is laid out in DIPs and drawn through a DCRenderTarget whose DPI
// is set to the window's effective DPI, so the composited bitmap is always the
// right pixel size on 100% / 125% / 150% / 200% displays.
//
// The memory DC, DIB section and brushes are created once and reused. When the
// formatted text, size, DPI, font size and opacity are all unchanged, Render()
// returns immediately without touching the GPU/CPU at all -- that is what keeps
// the process at a few milliseconds of CPU per minute while idle.

namespace {

ID2D1Factory*        g_pD2DFactory = nullptr;
IDWriteFactory*      g_pDWriteFactory = nullptr;
ID2D1DCRenderTarget* g_pDCRT = nullptr;
IDWriteTextFormat*   g_pTextFormat = nullptr;
float                g_textFormatSize = -1.0f;
bool                 g_comInitialized = false;

// Cached composition surface, rebuilt only when the pixel size or DPI changes.
HDC      g_hdcMem = nullptr;
HBITMAP  g_hBmp = nullptr;
HGDIOBJ  g_hOldBmp = nullptr;
void*    g_pBits = nullptr;
int      g_bmpW = 0;
int      g_bmpH = 0;
UINT     g_bmpDpi = 0;

ID2D1SolidColorBrush* g_brBg = nullptr;
ID2D1SolidColorBrush* g_brFg = nullptr;
ID2D1SolidColorBrush* g_brShadow = nullptr;

// Dirty tracking.
std::wstring g_lastText;
float        g_lastFontSize = -1.0f;
int          g_lastOpacity = -1;
int          g_lastW = 0;
int          g_lastH = 0;
UINT         g_lastDpi = 0;
bool         g_firstDraw = true;

bool EnsureTextFormat(float fontSize) {
    if (g_pTextFormat && g_textFormatSize == fontSize) return true;
    if (g_pTextFormat) { g_pTextFormat->Release(); g_pTextFormat = nullptr; }
    const wchar_t* families[] = { L"Consolas", L"Courier New", L"Microsoft Sans Serif" };
    HRESULT hr = E_FAIL;
    for (const wchar_t* family : families) {
        hr = g_pDWriteFactory->CreateTextFormat(
            family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us", &g_pTextFormat);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) return false;
    g_pTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_textFormatSize = fontSize;
    return true;
}

void DestroySurface() {
    if (g_hdcMem) {
        if (g_hOldBmp) SelectObject(g_hdcMem, g_hOldBmp);
        g_hOldBmp = nullptr;
    }
    if (g_hBmp) { DeleteObject(g_hBmp); g_hBmp = nullptr; }
    if (g_hdcMem) { DeleteDC(g_hdcMem); g_hdcMem = nullptr; }
    g_pBits = nullptr;
    g_bmpW = g_bmpH = 0;
    g_bmpDpi = 0;
}

bool EnsureSurface(int w, int h, UINT dpi) {
    if (g_hdcMem && g_bmpW == w && g_bmpH == h && g_bmpDpi == dpi) return true;
    DestroySurface();

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;
    g_hdcMem = CreateCompatibleDC(hdcScreen);
    ReleaseDC(nullptr, hdcScreen);
    if (!g_hdcMem) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    g_hBmp = CreateDIBSection(g_hdcMem, &bmi, DIB_RGB_COLORS, &g_pBits, nullptr, 0);
    if (!g_hBmp || !g_pBits) { DestroySurface(); return false; }
    g_hOldBmp = SelectObject(g_hdcMem, g_hBmp);

    RECT rc = { 0, 0, w, h };
    if (FAILED(g_pDCRT->BindDC(g_hdcMem, &rc))) { DestroySurface(); return false; }
    g_pDCRT->SetDpi((float)dpi, (float)dpi);
    g_bmpW = w;
    g_bmpH = h;
    g_bmpDpi = dpi;
    return true;
}

UINT WindowDpi(HWND hwnd) {
    UINT dpi = 96;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        auto fn = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        if (fn) {
            UINT v = fn(hwnd);
            if (v >= 48 && v <= 768) dpi = v;
        }
    }
    return dpi;
}

} // namespace

bool InitRenderer() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_comInitialized = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pD2DFactory))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g_pDWriteFactory)))) return false;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0, 0, D2D1_RENDER_TARGET_USAGE_NONE);
    if (FAILED(g_pD2DFactory->CreateDCRenderTarget(&props, &g_pDCRT))) return false;

    // Brushes belong to the render target and stay valid for its whole lifetime.
    if (FAILED(g_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.02f, 0.02f, 0.25f), &g_brBg))) return false;
    if (FAILED(g_pDCRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g_brFg))) return false;
    if (FAILED(g_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &g_brShadow))) return false;
    return true;
}

void ShutdownRenderer() {
    DestroySurface();
    if (g_brBg) { g_brBg->Release(); g_brBg = nullptr; }
    if (g_brFg) { g_brFg->Release(); g_brFg = nullptr; }
    if (g_brShadow) { g_brShadow->Release(); g_brShadow = nullptr; }
    if (g_pDCRT) { g_pDCRT->Release(); g_pDCRT = nullptr; }
    if (g_pTextFormat) { g_pTextFormat->Release(); g_pTextFormat = nullptr; }
    if (g_pDWriteFactory) { g_pDWriteFactory->Release(); g_pDWriteFactory = nullptr; }
    if (g_pD2DFactory) { g_pD2DFactory->Release(); g_pD2DFactory = nullptr; }
    g_textFormatSize = -1.0f;
    g_firstDraw = true;
    if (g_comInitialized) { CoUninitialize(); g_comInitialized = false; }
}

SIZE Render(HWND hwnd, const std::vector<std::wstring>& lines, int opacityPercent, int fontSize) {
    const float fs = (float)fontSize;
    if (!EnsureTextFormat(fs)) return SIZE{ 0, 0 };

    // --- Layout, in DIPs ---------------------------------------------------
    // One text layout per line, reused for both the shadow and the glyph pass.
    // DrawText() would re-shape the string for every call; this shapes once.
    const float lineHeight = fs + 5.0f;
    const float pad = fs * 0.6f + 6.0f;
    float maxWidth = 0.0f;
    std::vector<IDWriteTextLayout*> layouts;
    layouts.reserve(lines.size());
    for (const auto& line : lines) {
        IDWriteTextLayout* pLayout = nullptr;
        if (FAILED(g_pDWriteFactory->CreateTextLayout(line.c_str(), (UINT32)line.length(),
                                                      g_pTextFormat, 3000.0f, 100.0f, &pLayout)))
            continue;
        DWRITE_TEXT_METRICS metrics = {};
        if (SUCCEEDED(pLayout->GetMetrics(&metrics)) && metrics.width > maxWidth)
            maxWidth = metrics.width;
        layouts.push_back(pLayout);
    }
    auto releaseLayouts = [&]() {
        for (auto* p : layouts) p->Release();
        layouts.clear();
    };
    if (maxWidth < 120.0f) maxWidth = 120.0f;
    float dipW = maxWidth + pad * 2.0f + 10.0f;
    float dipH = (float)lines.size() * lineHeight + pad * 2.0f + 10.0f;

    // --- Pixel size --------------------------------------------------------
    const UINT dpi = WindowDpi(hwnd);
    const float scale = (float)dpi / 96.0f;
    const int w = (int)ceilf(dipW * scale);
    const int h = (int)ceilf(dipH * scale);
    const SIZE result = { w, h };

    // --- Dirty check -------------------------------------------------------
    std::wstring text;
    for (const auto& line : lines) text += line;
    if (!g_firstDraw && w == g_lastW && h == g_lastH && dpi == g_lastDpi &&
        fontSize == (int)g_lastFontSize && opacityPercent == g_lastOpacity && text == g_lastText) {
        releaseLayouts();
        return result;   // nothing changed: no draw, no composite
    }

    if (!EnsureSurface(w, h, dpi)) { releaseLayouts(); return result; }

    // --- Draw (DIP coordinates; the render target scales by DPI) -----------
    g_pDCRT->BeginDraw();
    g_pDCRT->Clear(D2D1::ColorF(0, 0, 0, 0));
    D2D1_RECT_F bgRect = D2D1::RectF(0, 0, dipW, dipH);
    g_pDCRT->FillRectangle(&bgRect, g_brBg);

    float y = pad + 1.0f;
    for (auto* pLayout : layouts) {
        g_pDCRT->DrawTextLayout(D2D1::Point2F(pad + 2.0f, y + 1.0f), pLayout, g_brShadow,
                                D2D1_DRAW_TEXT_OPTIONS_NONE);
        g_pDCRT->DrawTextLayout(D2D1::Point2F(pad + 1.0f, y), pLayout, g_brFg,
                                D2D1_DRAW_TEXT_OPTIONS_NONE);
        y += lineHeight;
    }
    g_pDCRT->EndDraw();
    releaseLayouts();

    BLENDFUNCTION blend = { AC_SRC_OVER, 0, (BYTE)(opacityPercent * 255 / 100), AC_SRC_ALPHA };
    POINT ptSrc = { 0, 0 };
    SIZE sz = { w, h };
    HDC hdcScreen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd, hdcScreen, nullptr, &sz, g_hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    if (hdcScreen) ReleaseDC(nullptr, hdcScreen);

    g_lastText = std::move(text);
    g_lastFontSize = fs;
    g_lastOpacity = opacityPercent;
    g_lastW = w;
    g_lastH = h;
    g_lastDpi = dpi;
    g_firstDraw = false;
    return result;
}
