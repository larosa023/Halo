// Halo - on-screen display (see osd.h)
#include "osd.h"
#include "util.h"
#include <wincodec.h>
#include <cmath>
#include <algorithm>

namespace osd {

static HWND s_wnd = nullptr;
static HDC s_mem = nullptr;
static HBITMAP s_bmp = nullptr, s_oldBmp = nullptr;
static SIZE s_size{};
static POINT s_pos{};
static DWORD s_shownAt = 0;
static float s_alpha = 0;
enum { kHoldMs = 1100, kFadeInMs = 120, kFadeOutMs = 260, TIMER_FADE = 1 };

static void Push(BYTE alpha) {
    if (!s_wnd || !s_mem) return;
    HDC screen = GetDC(nullptr);
    POINT zero{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(s_wnd, screen, &s_pos, &s_size, s_mem, &zero, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == TIMER_FADE) {
        DWORD t = GetTickCount() - s_shownAt;
        float a;
        if (t < kFadeInMs) a = t / (float)kFadeInMs;
        else if (t < kFadeInMs + kHoldMs) a = 1;
        else a = 1 - (t - kFadeInMs - kHoldMs) / (float)kFadeOutMs;
        a = std::clamp(a, 0.0f, 1.0f);
        Push((BYTE)std::lround(a * 255));
        if (a <= 0 && t > kFadeInMs) { KillTimer(h, TIMER_FADE); ShowWindow(h, SW_HIDE); }
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

static bool EnsureWindow() {
    if (s_wnd) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Halo.larosa023.Osd";
    RegisterClassExW(&wc);
    s_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                            wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    return s_wnd != nullptr;
}

static void FreeBitmap() {
    if (s_mem) { SelectObject(s_mem, s_oldBmp); DeleteDC(s_mem); s_mem = nullptr; }
    if (s_bmp) { DeleteObject(s_bmp); s_bmp = nullptr; }
}

void Show(const std::wstring& title, const std::wstring& sub, float level, D2D1_COLOR_F accent) {
    if (!EnsureWindow()) return;
    // where: bottom centre of the monitor the pointer is on
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE sh = GetModuleHandleW(L"shcore.dll"))
        if (auto f = (Fn)GetProcAddress(sh, "GetDpiForMonitor")) f(mon, 0, &dpiX, &dpiY);
    float s = dpiX / 96.0f;

    auto* tf = gfx::Format(20, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto* sf = gfx::Format(12.5f);
    float textW = std::max(gfx::MeasureWidth(title, tf), gfx::MeasureWidth(sub, sf));
    float wDip = std::clamp(textW + (level >= 0 ? 92.0f : 48.0f), 180.0f, 420.0f), hDip = 68;
    int w = (int)std::ceil(wDip * s), h = (int)std::ceil(hDip * s);

    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return;
    IWICBitmap* bmp = nullptr;
    wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ID2D1RenderTarget* rt = nullptr;
    if (bmp) {
        auto p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * s, 96 * s);
        gfx::d2d->CreateWicBitmapRenderTarget(bmp, p, &rt);
    }
    if (!rt) { if (bmp) bmp->Release(); wic->Release(); return; }
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    gfx::Canvas c;
    c.rt = rt;
    c.br = br;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    D2D1_RECT_F r = D2D1::RectF(1, 1, wDip - 1, hDip - 1);
    c.FillRound(r, 16, gfx::Rgb(0x18181D, 0.94f));
    c.StrokeRound(r, 16, gfx::Rgb(0xFFFFFF, 0.10f));
    float tx = 22;
    if (level >= 0) {
        float cx = 40, cy = hDip / 2, rr = 15;
        br->SetColor(gfx::Rgb(0xFFFFFF, 0.14f));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rr, rr), br, 4);
        float a = std::clamp(level, 0.02f, 1.0f) * 6.2831853f;
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(gfx::d2d->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* k = nullptr;
            g->Open(&k);
            k->BeginFigure(D2D1::Point2F(cx, cy - rr), D2D1_FIGURE_BEGIN_HOLLOW);
            for (int i = 1; i <= 48; ++i) { float t = a * i / 48; k->AddLine(D2D1::Point2F(cx + rr * std::sin(t), cy - rr * std::cos(t))); }
            k->EndFigure(D2D1_FIGURE_END_OPEN);
            k->Close();
            k->Release();
            br->SetColor(accent);
            rt->DrawGeometry(g, br, 4);
            g->Release();
        }
        tx = 72;
    }
    c.Text(title, tf, D2D1::RectF(tx, 9, wDip - 16, 38), gfx::Rgb(0xF5F5F8));
    c.Text(sub, sf, D2D1::RectF(tx, 38, wDip - 16, 58), gfx::Rgb(0xA9A9B4));
    rt->EndDraw();
    br->Release();
    rt->Release();

    // pixels -> DIB section for UpdateLayeredWindow
    FreeBitmap();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    s_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    s_mem = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (s_bmp && bits) {
        bmp->CopyPixels(nullptr, w * 4, (UINT)(w * h * 4), (BYTE*)bits);
        s_oldBmp = (HBITMAP)SelectObject(s_mem, s_bmp);
    }
    bmp->Release();
    wic->Release();

    s_size = {w, h};
    s_pos = {mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.bottom - h - (int)(96 * s)};
    bool visible = IsWindowVisible(s_wnd) != 0;
    // already up: keep it at full opacity and restart the hold
    s_shownAt = GetTickCount() - (visible ? kFadeInMs : 0);
    Push(visible ? 255 : 0);
    if (!visible) ShowWindow(s_wnd, SW_SHOWNOACTIVATE);
    SetWindowPos(s_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(s_wnd, TIMER_FADE, 15, nullptr);
    (void)s_alpha;
}

void Destroy() {
    FreeBitmap();
    if (s_wnd) DestroyWindow(s_wnd);
    s_wnd = nullptr;
}

} // namespace osd
