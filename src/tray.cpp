// Halo - tray icon (see tray.h)
#include "tray.h"
#include "gfx.h"
#include "config.h"
#include "util.h"
#include <shellapi.h>
#include <wincodec.h>
#include <cmath>
#include <algorithm>

namespace tray {

static HWND s_hwnd = nullptr;
static NOTIFYICONDATAW s_nid{};
static HICON s_icon = nullptr;
static bool s_added = false;
static std::wstring s_lastKey;

static bool TaskbarLight() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return false;
    return v != 0;
}

// Draws the icon with Direct2D into a WIC bitmap, then turns it into an HICON.
static HICON MakeIcon(int pct, bool charging, bool connected, bool asleep, bool showPercent) {
    UINT dpi = GetDpiForSystem();
    int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (size <= 0) size = 16;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
    IWICBitmap* bmp = nullptr;
    wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ID2D1RenderTarget* rt = nullptr;
    if (bmp) {
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        gfx::d2d->CreateWicBitmapRenderTarget(bmp, p, &rt);
    }
    HICON icon = nullptr;
    if (rt) {
        ID2D1SolidColorBrush* br = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        bool light = TaskbarLight();
        D2D1_COLOR_F fg = light ? gfx::Rgb(0x1A1A1A) : gfx::Rgb(0xFFFFFF);
        D2D1_COLOR_F track = light ? gfx::Rgb(0x000000, 0.18f) : gfx::Rgb(0xFFFFFF, 0.25f);
        D2D1_COLOR_F level = fg;
        if (charging) level = gfx::Rgb(0x3FBF74);
        else if (pct >= 0 && pct <= 10) level = gfx::Rgb(0xFF5A5A);
        else if (pct >= 0 && pct <= std::max(g_cfg.lowBattery, 20)) level = gfx::Rgb(0xF2A93B);
        float s = (float)size, c = s / 2, stroke = std::max(1.5f, s / 9.0f), r = c - stroke / 2 - 0.25f;

        if (!connected || pct < 0 || asleep || !showPercent) {
            // the halo ring: a track plus an arc for the level
            br->SetColor(track);
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), r, r), br, stroke);
            if (connected && pct >= 0) {
                float a = std::clamp(pct, 1, 100) / 100.0f * 6.2831853f;
                ID2D1PathGeometry* g = nullptr;
                if (SUCCEEDED(gfx::d2d->CreatePathGeometry(&g))) {
                    ID2D1GeometrySink* k = nullptr;
                    g->Open(&k);
                    k->BeginFigure(D2D1::Point2F(c, c - r), D2D1_FIGURE_BEGIN_HOLLOW);
                    for (int i = 1; i <= 48; ++i) {
                        float t = a * i / 48;
                        k->AddLine(D2D1::Point2F(c + r * std::sin(t), c - r * std::cos(t)));
                    }
                    k->EndFigure(D2D1_FIGURE_END_OPEN);
                    k->Close();
                    k->Release();
                    br->SetColor(asleep ? track : level);
                    rt->DrawGeometry(g, br, stroke);
                    g->Release();
                }
            }
            if (!connected) {
                br->SetColor(fg);
                rt->DrawLine(D2D1::Point2F(s * 0.28f, s * 0.72f), D2D1::Point2F(s * 0.72f, s * 0.28f), br, stroke);
            } else {
                br->SetColor(asleep ? track : level);
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), s * 0.12f, s * 0.12f), br);
            }
        } else {
            // the number itself, as big as fits
            std::wstring t = std::to_wstring(pct);
            float fs = t.size() >= 3 ? s * 0.50f : s * 0.80f;
            IDWriteTextFormat* f = nullptr;
            gfx::dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                      t.size() >= 3 ? DWRITE_FONT_STRETCH_CONDENSED : DWRITE_FONT_STRETCH_SEMI_CONDENSED, fs, L"en-us", &f);
            if (f) {
                f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                br->SetColor(level);
                rt->DrawText(t.c_str(), (UINT32)t.size(), f, D2D1::RectF(-2, -1, s + 2, s), br);
                f->Release();
            }
            if (charging) {
                br->SetColor(level);
                rt->FillRectangle(D2D1::RectF(0, s - std::max(1.5f, s / 10), s, s), br);
            }
        }
        rt->EndDraw();
        if (br) br->Release();
        rt->Release();

        // WIC pixels -> 32-bit DIB with alpha -> icon
        std::vector<BYTE> px((size_t)size * size * 4);
        bmp->CopyPixels(nullptr, size * 4, (UINT)px.size(), px.data());
        BITMAPV5HEADER bi{};
        bi.bV5Size = sizeof(bi);
        bi.bV5Width = size;
        bi.bV5Height = -size;
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
        void* bits = nullptr;
        HDC dc = GetDC(nullptr);
        HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, dc);
        if (color && bits) {
            memcpy(bits, px.data(), px.size());
            HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
            ICONINFO ii{TRUE, 0, 0, mask, color};
            icon = CreateIconIndirect(&ii);
            DeleteObject(mask);
        }
        if (color) DeleteObject(color);
    }
    if (bmp) bmp->Release();
    wic->Release();
    return icon;
}

static void Add() {
    s_nid = NOTIFYICONDATAW{};
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = s_hwnd;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    s_nid.uCallbackMessage = WM_HALO_TRAY;
    if (!s_icon) s_icon = MakeIcon(-1, false, false, false, false);
    s_nid.hIcon = s_icon;
    wcscpy_s(s_nid.szTip, L"Halo");
    s_added = Shell_NotifyIconW(NIM_ADD, &s_nid) != 0;
    s_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &s_nid);
}

void Create(HWND hwnd) {
    s_hwnd = hwnd;
    Add();
}

void Recreate() {
    s_lastKey.clear();
    Add();
}

void Remove() {
    if (s_added) Shell_NotifyIconW(NIM_DELETE, &s_nid);
    s_added = false;
    if (s_icon) DestroyIcon(s_icon);
    s_icon = nullptr;
}

void Update(const mouse::State& st, bool showPercent, const std::wstring& extra) {
    if (!s_hwnd) return;
    bool connected = st.conn == mouse::State::Connected;
    int pct = connected ? st.BatteryPercent() : -1;
    bool charging = connected && st.t.charging == 1;
    bool asleep = st.Asleep();
    std::wstring tip = L"Halo";
    if (!connected) tip += st.wiredOnly ? L"\nMouse on cable · plug in the dongle" : L"\nDongle not found";
    else if (asleep) tip += L"\nMouse asleep" + (pct >= 0 ? L" · last seen " + std::to_wstring(pct) + L"%" : L"");
    else {
        if (pct >= 0) tip += L"\nBattery " + std::to_wstring(pct) + L"%" + (charging ? L" · charging" : extra.empty() ? L"" : L" · " + extra);
        if (st.t.dpi > 0) tip += L"\n" + std::to_wstring(st.t.dpi) + L" DPI";
        if (st.t.pollingHz > 0) tip += L" · " + std::to_wstring(st.t.pollingHz) + L" Hz";
    }
    std::wstring key = std::to_wstring(pct) + L"|" + std::to_wstring(charging) + std::to_wstring(connected) + std::to_wstring(asleep) +
                       std::to_wstring(showPercent) + std::to_wstring(TaskbarLight()) + L"|" + tip;
    if (key == s_lastKey) return;
    s_lastKey = key;
    HICON icon = MakeIcon(pct, charging, connected, asleep, showPercent);
    s_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    if (icon) s_nid.hIcon = icon;
    wcsncpy_s(s_nid.szTip, tip.c_str(), _TRUNCATE);
    if (!s_added) Add();
    else Shell_NotifyIconW(NIM_MODIFY, &s_nid);
    if (icon) { if (s_icon) DestroyIcon(s_icon); s_icon = icon; }
}

void Notify(const std::wstring& title, const std::wstring& text, bool warning) {
    if (!s_added) return;
    NOTIFYICONDATAW n = s_nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = (warning ? NIIF_WARNING : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void ShowMenu(HWND hwnd, const mouse::State& st) {
    HMENU m = CreatePopupMenu();
    bool connected = st.conn == mouse::State::Connected;
    std::wstring head;
    int pct = st.BatteryPercent();
    if (!connected) head = L"Dongle not found";
    else if (st.Asleep()) head = L"Mouse asleep";
    else head = pct >= 0 ? L"Battery " + std::to_wstring(pct) + L"%" + (st.t.charging == 1 ? L" (charging)" : L"") : L"UltralightX";
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_DEFAULT, kCmdOpen, L"Open Halo");
    UINT live = connected ? 0 : MF_GRAYED;

    HMENU dpi = CreatePopupMenu();
    for (size_t i = 0; i < g_cfg.dpiPresets.size() && i < 50; ++i) {
        int v = g_cfg.dpiPresets[i];
        AppendMenuW(dpi, MF_STRING | (st.t.dpi == v ? MF_CHECKED : 0), kCmdDpiBase + (UINT)i, (std::to_wstring(v) + L" DPI").c_str());
    }
    AppendMenuW(m, MF_POPUP | live, (UINT_PTR)dpi, (L"DPI" + (st.t.dpi > 0 ? L"\t" + std::to_wstring(st.t.dpi) : L"")).c_str());

    HMENU poll = CreatePopupMenu();
    for (int i = 0; i < 5; ++i) {
        int v = ulx::kPollingRates[i];
        AppendMenuW(poll, MF_STRING | (st.t.pollingHz == v ? MF_CHECKED : 0), kCmdPollBase + i, (std::to_wstring(v) + L" Hz").c_str());
    }
    AppendMenuW(m, MF_POPUP | live, (UINT_PTR)poll, (L"Polling rate" + (st.t.pollingHz > 0 ? L"\t" + std::to_wstring(st.t.pollingHz) + L" Hz" : L"")).c_str());

    if (!g_cfg.profiles.empty()) {
        HMENU pr = CreatePopupMenu();
        for (size_t i = 0; i < g_cfg.profiles.size() && i < 50; ++i)
            AppendMenuW(pr, MF_STRING, kCmdProfileBase + (UINT)i, g_cfg.profiles[i].name.c_str());
        AppendMenuW(m, MF_POPUP | live, (UINT_PTR)pr, L"Profiles");
    }
    AppendMenuW(m, MF_STRING | live, kCmdRefresh, L"Read settings again");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kCmdQuit, L"Quit Halo");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);   // destroys the submenus too
}

} // namespace tray
