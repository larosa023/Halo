// Halo - main window: overview, performance, scroll & dongle, profiles, themes, settings.
// Drawn with Direct2D the same way Hangar is. All UI state lives on this thread; the mouse thread,
// the updater and the Hangar pipe report back with posted messages.
#include "ui.h"
#include "gfx.h"
#include "themes.h"
#include "config.h"
#include "util.h"
#include "net.h"
#include "json.h"
#include "mouse.h"
#include "tray.h"
#include "hangar.h"
#include "install.h"
#include "battery.h"
#include "osd.h"
#include <psapi.h>
#include <ctime>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <dbt.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

using namespace gfx;

// ---------------------------------------------------------------- state
enum class Page { Overview, Performance, Extras, Profiles, Themes, Settings };
static const wchar_t* kPageIds[] = {L"overview", L"performance", L"extras", L"profiles", L"themes", L"settings"};

enum class Hit {
    None, Nav, ToastClose, Retry, GoPage,
    DpiPreset, DpiPresetAdd, DpiStep, DpiField, Poll, MotionSync, LiftOff, ScrollMode, ScrollTimeout, DongleLed,
    ProfileSave, ProfileApply, ProfileDelete, ProfileName, ProfileAddApp, ProfileRemoveApp, SetAuto,
    SetHotkeys, SetOsd, PollTest,
    ThemePick, AccentPick, ThemeExport, ThemeFolder, ThemeReload,
    SetClose, SetStartup, SetTrayPct, SetLow, SetMotion, SetFps, SetOpenData, SetOpenLog, SetRepo, SetInstall, SetHangar, SetRefresh,
    SelfRestart, SelfCheck, SelfChannel, SelfAuto, SelfRollback,
};

struct HotRect {
    D2D1_RECT_F r;
    Hit kind;
    int index;
    int arg;
    bool enabled;
};

static HWND g_hwnd;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_br;
static Canvas g_c;
static Theme g_t;
static std::vector<ThemeDef> g_customThemes;
static bool g_hasHangarLive = false;     // a theme Hangar pushed over IPC (apply-theme)
static ThemeDef g_hangarLive;
static float g_scale = 1.0f;
static float g_W = 0, g_H = 0;

static Page g_page = Page::Overview;
static float g_scroll = 0, g_scrollTarget = 0, g_contentH = 0, g_viewH = 0;
static float g_pageAnim = 1.0f;
static std::vector<HotRect> g_hot;
static HotRect g_hover{{0, 0, 0, 0}, Hit::None, -1, 0, false};
static HotRect g_pressed{{0, 0, 0, 0}, Hit::None, -1, 0, false};
static bool g_tracking = false;

static std::wstring g_toast;
static bool g_toastBad = false;
static DWORD g_toastAt = 0;
static float g_toastAnim = 0;

static mouse::State g_ms;
static float g_ring = 0;                 // battery ring, animated (0-1)
static bool g_lowAlerted = false;
static std::vector<battery::Sample> g_history;   // DataDir()\battery-history.csv
static battery::Estimate g_estimate;

// one text field at a time: the DPI box or a profile name
struct Edit { bool active = false; Hit kind = Hit::None; int index = -1; std::wstring text; };
static Edit g_edit;
static bool g_caretOn = true;

static SelfUpdateState g_self;
static DWORD g_selfCheckedTick = 0;
static bool g_applyWhenIdle = false;
static selfupdate::AfterUpdate g_after;
static bool g_preparingUpdate = false;   // Hangar sent prepare-update (and hasn't restarted or cancelled yet)
static DWORD g_preparingTick = 0;
static bool g_quitting = false;
static UINT g_taskbarCreated = 0;
static HDEVNOTIFY g_devNotify = nullptr;

enum : UINT_PTR { TIMER_ANIM = 1, TIMER_CARET, TIMER_TOAST, TIMER_SELF, TIMER_HEALTHY, TIMER_HANGAR_HEALTH, TIMER_MIGRATE,
                  TIMER_FOREGROUND, TIMER_BATTERY };

static const float kSidebar = 220;

static void Invalidate() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }
// ---------------------------------------------------------------- frames and animation
// Halo only draws while something moves. During an animation each frame asks for the next one and
// EndDraw waits for the display to refresh, so motion runs at the monitor's own rate (60, 144,
// 240 Hz) and an idle Halo draws nothing. Every animation is time-based: it looks the same at any rate.
static LARGE_INTEGER g_qpf{}, g_lastFrameQpc{};
static float g_dt = 1.0f / 60;
static bool g_frameMore = false;      // something on screen is still moving: draw another frame
static float g_fps = 0;
static int g_fpsFrames = 0;
static LARGE_INTEGER g_fpsSince{};

static void StartAnim() { Invalidate(); }

static bool g_reducedNow = false;     // read once per frame
static bool Reduced() { return g_reducedNow; }
static bool ReducedSetting() {
    if (g_cfg.reduceMotion) return true;
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    return !anim;
}

// Moves v toward target, exponentially, `speed` times per second. Returns the new value.
static float Ease(float& v, float target, float speed, float settle = 0.002f) {
    if (Reduced()) { v = target; return v; }
    v += (target - v) * (1.0f - std::exp(-speed * g_dt));
    if (std::fabs(target - v) < settle) v = target;
    else g_frameMore = true;
    return v;
}

static inline float EaseOutCubic(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1 - (1 - t) * (1 - t) * (1 - t); }

// hover highlight per control, fading in and out (0-1)
static std::map<uint64_t, float> g_hoverAmt;
static uint64_t HKey(Hit kind, int index, int arg) {
    return ((uint64_t)(uint32_t)kind << 48) ^ ((uint64_t)(uint32_t)(index + 1) << 24) ^ (uint64_t)(uint32_t)(arg + 1);
}
static bool Visible() { return g_hwnd && IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd); }

static void Toast(const std::wstring& text, bool bad = false) {
    g_toast = text;
    g_toastBad = bad;
    g_toastAt = GetTickCount();
    if (g_hwnd) SetTimer(g_hwnd, TIMER_TOAST, bad ? 7000 : 4000, nullptr);
    StartAnim();
}

static std::wstring HexColor(uint32_t c) {
    wchar_t b[16];
    swprintf(b, 16, L"#%06X", c & 0xFFFFFF);
    return b;
}

static void OpenUrl(const std::wstring& url) { ShellExecuteW(g_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ---------------------------------------------------------------- themes
// "Match Hangar": Hangar's live theme (pushed over IPC), else its saved choice, else Dark
static ThemeDef HangarThemeDef(std::wstring* accent, bool* found) {
    std::wstring theme, acc;
    if (found) *found = false;
    if (g_hasHangarLive) { if (accent) *accent = L"theme"; if (found) *found = true; return g_hangarLive; }
    if (ReadHangarTheme(theme, acc)) {
        if (accent) *accent = acc;
        if (found) *found = true;
        return FindTheme(theme, g_customThemes);
    }
    if (accent) *accent = L"theme";
    return BuiltinThemes()[0];
}

static bool MatchingHangar() { return g_cfg.theme == L"hangar"; }

static void ApplyTheme() {
    if (MatchingHangar()) {
        std::wstring acc;
        ThemeDef def = HangarThemeDef(&acc, nullptr);
        g_t = MakeTheme(def, acc);
    } else {
        g_t = MakeTheme(FindTheme(g_cfg.theme, g_customThemes), g_cfg.accent);
    }
    if (g_hwnd) { ApplyWindowTheme(g_hwnd, g_t); Invalidate(); }
}

// ---------------------------------------------------------------- device
static void DiscardDevice() {
    SafeRelease(g_br);
    SafeRelease(g_rt);
}

static bool EnsureDevice() {
    if (g_rt) return true;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    UINT dpi = GetDpiForWindow(g_hwnd);
    props.dpiX = props.dpiY = (float)dpi;
    if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)),
                                           D2D1_PRESENT_OPTIONS_IMMEDIATELY), &g_rt))) return false;
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g_br);
    return true;
}

// ---------------------------------------------------------------- drawing helpers
static void AddHot(const D2D1_RECT_F& r, Hit kind, int index = -1, int arg = 0, bool enabled = true) {
    g_hot.push_back({r, kind, index, arg, enabled});
}
static bool IsHover(Hit kind, int index = -1, int arg = 0) {
    return g_hover.kind == kind && g_hover.index == index && g_hover.arg == arg;
}
static bool IsPressed(Hit kind, int index = -1, int arg = 0) {
    return g_pressed.kind == kind && g_pressed.index == index && g_pressed.arg == arg && IsHover(kind, index, arg);
}
static float HoverAmt(Hit kind, int index = -1, int arg = 0) {
    auto it = g_hoverAmt.find(HKey(kind, index, arg));
    return it == g_hoverAmt.end() ? 0.0f : it->second;
}

enum class Btn { Primary, Secondary, Subtle, Danger };

static float ButtonWidth(const std::wstring& label, wchar_t glyph) {
    float w = MeasureWidth(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 28;
    if (glyph) w += label.empty() ? 6 : 22;
    return std::max(w, label.empty() ? 34.0f : 76.0f);
}

static void Button(const D2D1_RECT_F& r, const std::wstring& label, wchar_t glyph, Btn style, Hit kind, int index = -1, int arg = 0, bool enabled = true) {
    float h = enabled ? HoverAmt(kind, index, arg) : 0.0f;
    bool down = enabled && IsPressed(kind, index, arg);
    D2D1_COLOR_F bg, fg;
    switch (style) {
        case Btn::Primary: bg = Mix(g_t.accent, g_t.accentHover, h); fg = g_t.onAccent; break;
        case Btn::Danger: bg = Mix(Alpha(g_t.bad, 0.12f), Alpha(g_t.bad, 0.22f), h); fg = g_t.bad; break;
        case Btn::Subtle: bg = Mix(Alpha(g_t.surfaceHover, 0.0f), g_t.surfaceHover, h); fg = g_t.text; break;
        default: bg = Mix(Mix(g_t.surface, g_t.surfaceStrong, 0.55f), g_t.surfaceStrong, h); fg = g_t.text; break;
    }
    if (!enabled) { bg = Alpha(g_t.surfaceStrong, 0.5f); fg = g_t.faint; }
    if (down) bg = Mix(bg, g_t.dark ? Rgb(0x000000) : Rgb(0xFFFFFF), 0.12f);
    g_c.FillRound(r, 6, bg);
    if (style == Btn::Secondary && enabled) g_c.StrokeRound(r, 6, g_t.divider);
    float w = r.right - r.left;
    auto* tf = Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (glyph && label.empty()) {
        g_c.Glyph(glyph, Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), r, fg);
    } else if (glyph) {
        float tw = MeasureWidth(label, tf) + 22;
        float x = r.left + (w - tw) / 2;
        g_c.Glyph(glyph, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true), D2D1::RectF(x, r.top, x + 18, r.bottom), fg);
        g_c.Text(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x + 22, r.top, r.right, r.bottom), fg);
    } else {
        g_c.Text(label, tf, r, fg);
    }
    AddHot(r, kind, index, arg, enabled);
}

static float Pill(float x, float y, const std::wstring& text, D2D1_COLOR_F color, bool rightAlign = false) {
    if (text.empty()) return 0;
    auto* f = Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    float w = MeasureWidth(text, f) + 16;
    if (rightAlign) x -= w;
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + 20);
    g_c.FillRound(r, 10, Alpha(color, g_t.dark ? 0.16f : 0.12f));
    g_c.Text(text, f, r, color);
    return w;
}

static float SegmentedWidth(const std::vector<std::wstring>& opts) {
    auto* f = Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    float w = 0;
    for (auto& o : opts) w += MeasureWidth(o, f) + 30 + 6;
    return w - 6;
}

// Segmented control; the selection pill slides to the option you pick
struct SegAnim { float x = 0, w = 0; int page = -1; size_t n = 0; };
static std::map<int, SegAnim> g_seg;

static void Segmented(float x, float y, const std::vector<std::wstring>& opts, int sel, Hit kind, bool enabled = true) {
    std::vector<D2D1_RECT_F> rects;
    float cx = x;
    for (auto& o : opts) {
        float w = MeasureWidth(o, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER)) + 30;
        rects.push_back(D2D1::RectF(cx, y, cx + w, y + 32));
        cx += w + 6;
    }
    for (size_t k = 0; k < opts.size(); ++k) {
        float h = enabled ? HoverAmt(kind, -1, (int)k) : 0.0f;
        g_c.FillRound(rects[k], 6, Mix(Mix(g_t.surface, g_t.surfaceStrong, 0.3f), g_t.surfaceHover, h));
    }
    SegAnim& a = g_seg[(int)kind];
    if (sel >= 0 && sel < (int)opts.size()) {
        float tx = rects[sel].left - x, tw = rects[sel].right - rects[sel].left;
        if (a.page != (int)g_page || a.n != opts.size() || a.w <= 0) { a.x = tx; a.w = tw; }
        else { Ease(a.x, tx, 20, 0.25f); Ease(a.w, tw, 20, 0.25f); }
        a.page = (int)g_page;
        a.n = opts.size();
        D2D1_RECT_F p = D2D1::RectF(x + a.x, y, x + a.x + a.w, y + 32);
        g_c.FillRound(p, 6, Mix(g_t.surface, g_t.accent, enabled ? 0.20f : 0.09f));
        g_c.StrokeRound(p, 6, Alpha(g_t.accent, enabled ? 0.6f : 0.25f));
    } else {
        a.w = 0;
    }
    for (size_t k = 0; k < opts.size(); ++k) {
        bool s = (int)k == sel;
        auto* f = Format(13, s ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        g_c.Text(opts[k], f, rects[k], !enabled ? g_t.faint : s ? g_t.text : g_t.subtext);
        AddHot(rects[k], kind, -1, (int)k, enabled);
    }
}

// An arc from the top, clockwise, `frac` of the way round
static void Arc(float cx, float cy, float r, float frac, D2D1_COLOR_F color, float stroke) {
    frac = std::clamp(frac, 0.0f, 1.0f);
    if (frac <= 0.001f) return;
    if (frac >= 0.999f) {
        g_br->SetColor(color);
        g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_br, stroke);
        return;
    }
    // short line segments rather than an arc segment: identical on screen, and it renders the same
    // under every Direct2D implementation
    float a = frac * 6.2831853f;
    int steps = std::max(8, (int)(a * r / 2.5f));
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(d2d->CreatePathGeometry(&g))) return;
    ID2D1GeometrySink* s = nullptr;
    if (SUCCEEDED(g->Open(&s))) {
        s->BeginFigure(D2D1::Point2F(cx, cy - r), D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i <= steps; ++i) {
            float t = a * i / steps;
            s->AddLine(D2D1::Point2F(cx + r * std::sin(t), cy - r * std::cos(t)));
        }
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        s->Release();
        ID2D1StrokeStyle* style = nullptr;
        d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &style);
        g_br->SetColor(color);
        g_rt->DrawGeometry(g, g_br, stroke, style);
        if (style) style->Release();
    }
    g->Release();
}

static void Logo(float x, float y, float size) {
    D2D1_RECT_F logo = D2D1::RectF(x, y, x + size, y + size);
    g_c.FillRound(logo, size * 0.28f, g_t.accent);
    float c = size / 2;
    g_br->SetColor(g_t.onAccent);
    g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + c, y + c), size * 0.29f, size * 0.29f), g_br, size * 0.1f);
    g_c.Circle(x + c, y + c, size * 0.08f, g_t.onAccent);
}

// ---------------------------------------------------------------- mouse helpers
static bool Connected() { return g_ms.conn == mouse::State::Connected; }
static bool CanWrite() { return Connected() && g_ms.t.linkState != 0; }

static D2D1_COLOR_F BatteryColor(int pct, bool charging) {
    if (charging) return g_t.good;
    if (pct < 0) return g_t.faint;
    if (pct <= 10) return g_t.bad;
    if (pct <= std::max(20, g_cfg.lowBattery)) return g_t.warn;
    return g_t.good;
}

static std::wstring Thousands(int v) {
    std::wstring s = std::to_wstring(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, L",");
    return s;
}

static std::wstring ConnectionLine() {
    if (g_ms.conn == mouse::State::Searching) return L"Looking for your mouse…";
    if (!Connected()) return g_ms.wiredOnly ? L"On cable · plug in the dongle" : L"Dongle not found";
    if (g_ms.Asleep()) return L"Asleep · move it to wake it";
    if (!g_ms.readComplete) return L"Reading settings…";
    int pct = g_ms.BatteryPercent();
    std::wstring s = pct >= 0 ? std::to_wstring(pct) + L"%" : L"Connected";
    if (g_ms.t.charging == 1) s += L" · charging";
    return s;
}

// ---------------------------------------------------------------- sidebar
static void NavItem(float y, Page p, const std::wstring& label, wchar_t glyph) {
    D2D1_RECT_F r = D2D1::RectF(10, y, kSidebar - 10, y + 36);
    bool sel = g_page == p;
    float h = sel ? 0.0f : HoverAmt(Hit::Nav, -1, (int)p);
    if (h > 0.01f) g_c.FillRound(r, 6, g_t.dark ? Rgb(0xFFFFFF, 0.04f * h) : Rgb(0x000000, 0.035f * h));
    g_c.Glyph(glyph, Format(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 10, y, r.left + 34, y + 36),
              sel ? g_t.accent : g_t.subtext);
    g_c.Text(label, Format(13.5f, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 42, y, r.right - 8, y + 36), g_t.text);
    AddHot(r, Hit::Nav, -1, (int)p);
}

static void DrawSidebar() {
    g_c.Fill(D2D1::RectF(0, 0, kSidebar, g_H), g_t.sidebar);
    g_c.Line(kSidebar - 0.5f, 0, kSidebar - 0.5f, g_H, g_t.divider);
    Logo(22, 22, 28);
    g_c.Text(L"Halo", Format(17, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(60, 20, kSidebar, 40), g_t.text);
    g_c.Text(L"UltralightX", Format(11.5f), D2D1::RectF(60, 38, kSidebar, 54), g_t.subtext);

    // the selection highlight slides between pages
    float by0 = g_H - 104;
    float targets[6] = {76, 114, 152, 190, by0 - 38, by0};
    static float navY = -1;
    static int navPage = -1;
    float ty = targets[(int)g_page];
    if (navY < 0 || navPage == (int)g_page) navY = ty;   // same page (or a resize): stay put
    else Ease(navY, ty, 18, 0.3f);
    if (navY == ty) navPage = (int)g_page;
    D2D1_RECT_F hl = D2D1::RectF(10, navY, kSidebar - 10, navY + 36);
    g_c.FillRound(hl, 6, g_t.dark ? Rgb(0xFFFFFF, 0.08f) : Rgb(0x000000, 0.06f));
    g_c.FillRound(D2D1::RectF(hl.left, navY + 10, hl.left + 3, navY + 26), 1.5f, g_t.accent);

    float y = 76;
    NavItem(y, Page::Overview, L"Overview", icon::Home); y += 38;
    NavItem(y, Page::Performance, L"Performance", icon::Speed); y += 38;
    NavItem(y, Page::Extras, L"Scroll & dongle", icon::Lightbulb); y += 38;
    NavItem(y, Page::Profiles, L"Profiles", icon::Contact); y += 38;

    float by = g_H - 104;   // Themes + Settings at the bottom
    if (g_self.kind == SelfUpdateState::Ready && y + 120 < by - 38) {
        D2D1_RECT_F b = D2D1::RectF(12, by - 38 - 92, kSidebar - 12, by - 38 - 8);
        g_c.FillRound(b, 8, Alpha(g_t.accent, 0.14f));
        g_c.StrokeRound(b, 8, Alpha(g_t.accent, 0.35f));
        g_c.Text(L"Halo " + g_self.version + L" is ready", Format(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(b.left + 12, b.top + 8, b.right - 8, b.top + 28), g_t.text);
        g_c.Text(L"Takes a second · you stay where you are", Format(11.5f), D2D1::RectF(b.left + 12, b.top + 27, b.right - 8, b.top + 44), g_t.subtext);
        Button(D2D1::RectF(b.left + 10, b.bottom - 34, b.right - 10, b.bottom - 8), L"Restart to update", icon::Update, Btn::Primary, Hit::SelfRestart);
    }
    if (y + 40 < by - 38) NavItem(by - 38, Page::Themes, L"Themes", icon::Color);
    if (y < by) NavItem(by, Page::Settings, L"Settings", icon::Settings);

    // status: a small ring and a line of text
    float sy = g_H - 46;
    int pct = Connected() ? g_ms.BatteryPercent() : -1;
    D2D1_COLOR_F col = Connected() && !g_ms.Asleep() ? BatteryColor(pct, g_ms.t.charging == 1) : g_t.faint;
    g_br->SetColor(Alpha(g_t.text, 0.12f));
    g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(32, sy + 13), 8, 8), g_br, 2.2f);
    if (pct >= 0) Arc(32, sy + 13, 8, pct / 100.0f, col, 2.2f);
    g_c.Text(ConnectionLine(), Format(12), D2D1::RectF(48, sy, kSidebar - 10, sy + 26), Connected() ? g_t.subtext : g_t.faint);
}

// ---------------------------------------------------------------- page scaffolding
struct PageFrame { float left, right, y; };

static PageFrame BeginPage(float x0, const std::wstring& title, const std::wstring& sub, float maxW = 900) {
    float pad = 32;
    PageFrame f;
    f.left = x0 + pad;
    f.right = std::min(g_W - pad, x0 + pad + maxW);
    g_viewH = g_H;
    g_rt->PushAxisAlignedClip(D2D1::RectF(x0, 0, g_W, g_H), D2D1_ANTIALIAS_MODE_ALIASED);
    f.y = 22 - g_scroll;
    g_c.Text(title, Format(22, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(f.left, f.y, f.right, f.y + 32), g_t.text);
    if (!sub.empty()) g_c.Text(sub, Format(12.5f), D2D1::RectF(f.left, f.y + 30, f.right, f.y + 48), g_t.subtext);
    f.y += sub.empty() ? 52 : 66;
    return f;
}

static void EndPage(const PageFrame& f) {
    g_contentH = f.y + g_scroll + 10;
    g_rt->PopAxisAlignedClip();
}

static D2D1_RECT_F Section(PageFrame& f, const std::wstring& title, float h) {
    if (!title.empty()) {
        g_c.Text(title, Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(f.left, f.y, f.right, f.y + 20), g_t.subtext);
        f.y += 26;
    }
    D2D1_RECT_F r = D2D1::RectF(f.left, f.y, f.right, f.y + h);
    g_c.FillRound(r, 10, g_t.surface);
    f.y += h + 26;
    return r;
}

static void RowLabel(const D2D1_RECT_F& s, float ry, const std::wstring& t, const std::wstring& sub, float rightReserve = 380) {
    g_c.Text(t, Format(14), D2D1::RectF(s.left + 18, ry + 8, s.right - rightReserve, ry + 28), g_t.text);
    if (!sub.empty()) g_c.Text(sub, Format(12), D2D1::RectF(s.left + 18, ry + 28, s.right - rightReserve, ry + 46), g_t.subtext);
}

static void Divider(const D2D1_RECT_F& s, float y) { g_c.Line(s.left + 18, y, s.right - 18, y, g_t.divider); }

// The card shown instead of a page's controls when there's no mouse to talk to
static bool NoMouseCard(PageFrame& f, bool forSettings) {
    if (Connected() && (!forSettings || !g_ms.Asleep())) return false;
    std::wstring title, body;
    wchar_t glyph = icon::Usb;
    if (g_ms.conn == mouse::State::Searching) { title = L"Looking for your mouse…"; body = L"One moment."; }
    else if (!Connected() && g_ms.wiredOnly) {
        title = L"Your mouse is on its cable";
        body = L"Halo talks to the UltralightX through its 2.4 GHz dongle, like XPANEL does. Plug the dongle in "
               L"(it can sit on the end of the cable) and Halo connects right away.";
    } else if (!Connected()) {
        title = L"Plug in the UltralightX dongle";
        body = L"Halo finds it by itself as soon as it's plugged in. If it's already in, unplug it and plug it back in." +
               (g_ms.error.empty() ? std::wstring() : L"\n\n" + g_ms.error);
    } else {
        glyph = icon::Sleep;
        title = L"Your mouse is asleep";
        body = L"Move it or click to wake it. Halo shows the last values it read until then, and changes you make need the mouse awake.";
    }
    auto* bf = Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    float bh = MeasureHeight(body, bf, f.right - f.left - 110);
    D2D1_RECT_F r = D2D1::RectF(f.left, f.y, f.right, f.y + std::max(120.0f, bh + 84));
    g_c.FillRound(r, 10, g_t.surface);
    g_c.Circle(r.left + 44, r.top + 46, 22, Alpha(g_t.accent, 0.14f));
    g_c.Glyph(glyph, Format(18, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 22, r.top + 24, r.left + 66, r.top + 68), g_t.accent);
    g_c.Text(title, Format(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 84, r.top + 22, r.right - 150, r.top + 46), g_t.text);
    g_c.Text(body, bf, D2D1::RectF(r.left + 84, r.top + 52, r.right - 24, r.top + 52 + bh + 4), g_t.subtext);
    if (!Connected()) Button(D2D1::RectF(r.right - 134, r.top + 20, r.right - 18, r.top + 52), L"Look again", icon::Refresh, Btn::Secondary, Hit::Retry);
    f.y = r.bottom + 26;
    return !Connected();
}

// ---------------------------------------------------------------- overview
static void Tile(const D2D1_RECT_F& r, wchar_t glyph, const std::wstring& label, const std::wstring& value, const std::wstring& unit,
                 const std::wstring& sub, Page target, int arg) {
    g_c.FillRound(r, 10, Mix(g_t.surface, g_t.surfaceHover, target == Page::Overview ? 0.0f : HoverAmt(Hit::GoPage, (int)target, arg)));
    g_c.Glyph(glyph, Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true), D2D1::RectF(r.left + 16, r.top + 14, r.left + 36, r.top + 34), g_t.accent);
    g_c.Text(label, Format(12.5f), D2D1::RectF(r.left + 40, r.top + 14, r.right - 12, r.top + 34), g_t.subtext);
    auto* vf = Format(26, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float vw = MeasureWidth(value, vf);
    g_c.Text(value, vf, D2D1::RectF(r.left + 16, r.top + 40, r.right - 12, r.top + 78), value == L"—" ? g_t.faint : g_t.text);
    if (!unit.empty()) g_c.Text(unit, Format(13), D2D1::RectF(r.left + 20 + vw, r.top + 50, r.right - 12, r.top + 74), g_t.subtext);
    if (!sub.empty()) g_c.Text(sub, Format(12), D2D1::RectF(r.left + 16, r.bottom - 30, r.right - 12, r.bottom - 10), g_t.faint);
    AddHot(r, Hit::GoPage, (int)target, arg);
}

static void SignalBars(float x, float y, int bars, D2D1_COLOR_F on) {
    for (int k = 0; k < 4; ++k) {
        float h = 5.0f + k * 4.0f;
        D2D1_RECT_F b = D2D1::RectF(x + k * 7, y + 18 - h, x + k * 7 + 4.5f, y + 18);
        g_c.FillRound(b, 1.5f, k < bars ? on : Alpha(g_t.text, 0.14f));
    }
}

// ---------------------------------------------------------------- battery history
static std::wstring HistoryPath() { return DataDir() + L"\\battery-history.csv"; }

static void LoadHistory() {
    std::wstring text;
    if (ReadTextFile(HistoryPath(), text)) g_history = battery::Parse(WideToUtf8(text));
}

static void RecordBattery() {
    if (!g_ms.Linked() || g_ms.t.batteryMv <= 0) return;
    battery::Sample smp;
    smp.t = (int64_t)time(nullptr);
    smp.pct = g_ms.BatteryPercent();
    smp.mv = g_ms.t.batteryMv;
    smp.charging = g_ms.t.charging == 1;
    if (smp.pct < 0) return;
    if (battery::Add(g_history, smp)) WriteTextFileAtomic(HistoryPath(), Utf8ToWide(battery::ToCsv(g_history)));
    g_estimate = battery::EstimateLeft(g_history, smp.pct);
}

static std::wstring TrayExtra() {
    if (g_estimate.hoursLeft < 0) return L"";
    std::wstring h = battery::FormatHours(g_estimate.hoursLeft);
    if (!h.empty() && h[0] == L'A') h = L"a" + h.substr(1);   // "about 6 h left"
    return h + L" left";
}

static std::wstring AgoText(int64_t t) {
    int64_t d = (int64_t)time(nullptr) - t;
    if (d < 3600) return L"under an hour ago";
    if (d < 24 * 3600) { long h = (long)(d / 3600); return std::to_wstring(h) + (h == 1 ? L" hour ago" : L" hours ago"); }
    long days = (long)(d / 86400);
    return days == 1 ? L"yesterday" : std::to_wstring(days) + L" days ago";
}

static void DrawBatteryHistory(PageFrame& f) {
    D2D1_RECT_F card = Section(f, L"BATTERY · LAST 7 DAYS", 176);
    float panelW = std::min(250.0f, (card.right - card.left) * 0.34f);
    D2D1_RECT_F plot = D2D1::RectF(card.left + 44, card.top + 18, card.right - panelW - 24, card.bottom - 34);
    int64_t now = (int64_t)time(nullptr), from = now - 7 * 86400;
    auto X = [&](int64_t t) { return plot.left + (float)(t - from) / (float)(now - from) * (plot.right - plot.left); };
    auto Y = [&](int pct) { return plot.bottom - pct / 100.0f * (plot.bottom - plot.top); };
    // grid: 0 / 50 / 100 and midnights
    auto* small = Format(11);
    for (int p : {0, 50, 100}) {
        g_c.Line(plot.left, Y(p), plot.right, Y(p), Alpha(g_t.text, p == 0 ? 0.12f : 0.06f));
        g_c.Text(std::to_wstring(p) + L"%", Format(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
                 D2D1::RectF(card.left, Y(p) - 8, plot.left - 8, Y(p) + 8), g_t.faint);
    }
    static const wchar_t* days[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    time_t tt = (time_t)now;
    tm lt{};
    localtime_s(&lt, &tt);
    int64_t midnight = now - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
    for (int k = 0; k < 7; ++k) {
        int64_t m = midnight - k * 86400;
        if (m < from) break;
        float x = X(m);
        g_c.Line(x, plot.top, x, plot.bottom, Alpha(g_t.text, 0.05f));
        int wd = ((lt.tm_wday - k) % 7 + 7) % 7;
        float nx = std::min(plot.right, X(m + 86400));
        g_c.Text(k == 0 ? L"Today" : days[wd], Format(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                 D2D1::RectF(x, plot.bottom + 6, nx, plot.bottom + 22), g_t.faint);
    }
    // the line, broken where the mouse was off or asleep; charging stretches in green
    size_t first = 0;
    while (first < g_history.size() && g_history[first].t < from) ++first;
    bool any = false;
    for (size_t i = first; i < g_history.size();) {
        size_t j = i + 1;
        while (j < g_history.size() && g_history[j].t - g_history[j - 1].t <= battery::kGap) ++j;
        if (j - i >= 2) {
            any = true;
            ID2D1PathGeometry* area = nullptr;
            ID2D1PathGeometry* line = nullptr;
            d2d->CreatePathGeometry(&area);
            d2d->CreatePathGeometry(&line);
            ID2D1GeometrySink* a = nullptr;
            ID2D1GeometrySink* l = nullptr;
            if (area && line && SUCCEEDED(area->Open(&a)) && SUCCEEDED(line->Open(&l))) {
                a->BeginFigure(D2D1::Point2F(X(g_history[i].t), plot.bottom), D2D1_FIGURE_BEGIN_FILLED);
                l->BeginFigure(D2D1::Point2F(X(g_history[i].t), Y(g_history[i].pct)), D2D1_FIGURE_BEGIN_HOLLOW);
                for (size_t k = i; k < j; ++k) {
                    D2D1_POINT_2F p = D2D1::Point2F(X(g_history[k].t), Y(g_history[k].pct));
                    a->AddLine(p);
                    if (k > i) l->AddLine(p);
                }
                a->AddLine(D2D1::Point2F(X(g_history[j - 1].t), plot.bottom));
                a->EndFigure(D2D1_FIGURE_END_CLOSED);
                l->EndFigure(D2D1_FIGURE_END_OPEN);
                a->Close();
                l->Close();
                g_br->SetColor(Alpha(g_t.accent, 0.13f));
                g_rt->FillGeometry(area, g_br);
                g_br->SetColor(g_t.accent);
                g_rt->DrawGeometry(line, g_br, 2.0f);
            }
            if (a) a->Release();
            if (l) l->Release();
            if (area) area->Release();
            if (line) line->Release();
            for (size_t k = i + 1; k < j; ++k)
                if (g_history[k].charging || g_history[k - 1].charging)
                    g_c.Line(X(g_history[k - 1].t), Y(g_history[k - 1].pct), X(g_history[k].t), Y(g_history[k].pct), g_t.good, 2.5f);
        }
        i = j;
    }
    if (!any) {
        g_c.Text(L"Halo records the battery every 5 minutes while the mouse is awake. The graph fills in over the next few hours.",
                 Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true),
                 D2D1::RectF(plot.left + 20, plot.top + 30, plot.right - 20, plot.bottom - 10), g_t.subtext);
    }
    // the estimate
    D2D1_RECT_F pr = D2D1::RectF(card.right - panelW, card.top + 18, card.right - 20, card.bottom - 16);
    g_c.Line(pr.left - 10, pr.top, pr.left - 10, pr.bottom, g_t.divider);
    bool charging = g_ms.t.charging == 1;
    std::wstring head, sub1, sub2;
    if (charging) { head = L"Charging"; sub1 = L"The level reads high while it charges"; }
    else if (g_estimate.hoursLeft >= 0) {
        head = battery::FormatHours(g_estimate.hoursLeft);
        sub1 = L"left at your usual pace";
        wchar_t r[64];
        swprintf(r, 64, L"Uses about %.1f%% an hour", g_estimate.ratePerHour);
        sub2 = r;
    } else { head = L"Estimating…"; sub1 = L"Needs a couple of hours of use since the last charge"; }
    g_c.Text(head, Format(20, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(pr.left + 8, pr.top, pr.right, pr.top + 30), charging ? g_t.good : g_t.text);
    g_c.Text(sub1, Format(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), D2D1::RectF(pr.left + 8, pr.top + 32, pr.right, pr.top + 64), g_t.subtext);
    if (!sub2.empty()) g_c.Text(sub2, Format(12), D2D1::RectF(pr.left + 8, pr.top + 66, pr.right, pr.top + 84), g_t.subtext);
    if (g_estimate.lastCharged > 0)
        g_c.Text(L"Last charged " + AgoText(g_estimate.lastCharged), Format(12), D2D1::RectF(pr.left + 8, pr.bottom - 20, pr.right, pr.bottom), g_t.faint);
    (void)small;
}

static void DrawOverview(float x0) {
    std::wstring sub = Connected() ? (g_ms.product.empty() ? L"Finalmouse UltralightX" : L"Finalmouse UltralightX · " + g_ms.product) : L"Finalmouse UltralightX";
    PageFrame f = BeginPage(x0, L"Overview", sub);
    if (NoMouseCard(f, false)) { EndPage(f); return; }
    if (g_ms.Asleep()) NoMouseCard(f, true);

    const ulx::Telemetry& t = g_ms.t;
    float w = f.right - f.left;
    float gap = 14;
    bool narrow = w < 600;
    float bw = narrow ? w : 280, bh = 252;
    // battery
    D2D1_RECT_F b = D2D1::RectF(f.left, f.y, f.left + bw, f.y + bh);
    g_c.FillRound(b, 10, g_t.surface);
    int pct = g_ms.BatteryPercent();
    bool charging = t.charging == 1;
    D2D1_COLOR_F col = g_ms.Asleep() ? g_t.faint : BatteryColor(pct, charging);
    float cx = (b.left + b.right) / 2, cy = b.top + 108, rr = 72;
    g_br->SetColor(Alpha(g_t.text, g_t.dark ? 0.08f : 0.07f));
    g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rr, rr), g_br, 12);
    if (pct >= 0) {
        // a soft glow under the arc: the halo
        Arc(cx, cy, rr, g_ring, Alpha(col, 0.18f), 22);
        Arc(cx, cy, rr, g_ring, col, 12);
    }
    std::wstring big = pct >= 0 ? std::to_wstring((int)std::lround(g_ring * 100)) : L"—";
    auto* nf = Format(40, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    float nw = MeasureWidth(big, nf);
    g_c.Text(big, nf, D2D1::RectF(cx - 70, cy - 30, cx + 70, cy + 18), pct >= 0 ? g_t.text : g_t.faint);
    if (pct >= 0) g_c.Text(L"%", Format(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(cx + nw / 2 + 1, cy - 18, cx + 80, cy + 4), g_t.subtext);
    std::wstring state = g_ms.Asleep() ? L"Asleep" : charging ? L"Charging" : t.charging == 0 ? L"On battery" : L"Battery";
    g_c.Text(state, Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(cx - 70, cy + 16, cx + 70, cy + 36), charging ? g_t.good : g_t.subtext);
    std::wstring foot = t.batteryMv > 0 ? Thousands(t.batteryMv) + L" mV" : L"Waiting for the battery reading";
    if (charging) foot += L" · reads high while charging";
    g_c.Text(foot, Format(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(b.left + 10, b.bottom - 44, b.right - 10, b.bottom - 24), g_t.faint);

    // tiles
    float tx = narrow ? f.left : b.right + gap, ty = narrow ? b.bottom + gap : f.y;
    float tw = ((f.right - tx) - gap) / 2, th = (bh - gap) / 2;
    std::wstring dpi = t.dpi > 0 ? Thousands(t.dpi) : L"—";
    std::wstring poll = t.pollingHz > 0 ? Thousands(t.pollingHz) : L"—";
    int bars = ulx::SignalBars(t.rssi, t.rssiKnown, t.linkState);
    std::wstring lod = t.liftOffMm > 0 ? std::to_wstring(t.liftOffMm) : L"—";
    Tile(D2D1::RectF(tx, ty, tx + tw, ty + th), icon::Mouse, L"DPI", dpi, L"", L"Change in Performance", Page::Performance, 0);
    Tile(D2D1::RectF(tx + tw + gap, ty, f.right, ty + th), icon::Speed, L"Polling rate", poll, t.pollingHz > 0 ? L"Hz" : L"",
         t.motionSync == 1 ? L"Motion Sync on" : t.motionSync == 0 ? L"Motion Sync off" : L"", Page::Performance, 1);
    D2D1_RECT_F sig = D2D1::RectF(tx, ty + th + gap, tx + tw, ty + 2 * th + gap);
    Tile(sig, icon::Wifi, L"Signal", bars ? (bars >= 4 ? L"Excellent" : bars == 3 ? L"Good" : bars == 2 ? L"Fair" : L"Weak") : L"—", L"",
         t.rssiKnown ? std::to_wstring(t.rssi) + L" dBm" : L"", Page::Overview, 2);
    if (bars) SignalBars(sig.right - 46, sig.top + 14, bars, g_t.accent);
    Tile(D2D1::RectF(tx + tw + gap, ty + th + gap, f.right, ty + 2 * th + gap), icon::Sliders, L"Lift-off distance", lod, t.liftOffMm > 0 ? L"mm" : L"",
         L"", Page::Performance, 3);
    f.y = (narrow ? ty + 2 * th + gap : b.bottom) + 26;
    DrawBatteryHistory(f);

    // details
    struct Row { std::wstring k, v; };
    std::vector<Row> rows = {
        {L"Connection", L"2.4 GHz dongle" + std::wstring(t.linkState == 1 ? L" · linked" : t.linkState == 0 ? L" · mouse asleep" : L"")},
        {L"Tournament scroll", t.scrollMode >= 0 ? std::wstring(mouse::ScrollModeName(t.scrollMode)) + (t.scrollMode > 0 && t.scrollTimeoutMs > 0 ? L" · " + std::to_wstring(t.scrollTimeoutMs) + L" ms" : L"") : L"—"},
        {L"Dongle light", t.dongleLed >= 0 ? mouse::DongleLedName(t.dongleLed) : L"—"},
        {L"Mouse firmware", t.mouseFirmware.empty() ? L"—" : Utf8ToWide(t.mouseFirmware)},
        {L"Dongle firmware", t.dongleRfFirmware.empty() && t.dongleUsbFirmware.empty() ? L"—"
             : L"RF " + (t.dongleRfFirmware.empty() ? std::wstring(L"?") : Utf8ToWide(t.dongleRfFirmware)) + L" · USB " +
               (t.dongleUsbFirmware.empty() ? std::wstring(L"?") : Utf8ToWide(t.dongleUsbFirmware))},
        {L"Serial number", t.serial.empty() ? L"—" : Utf8ToWide(t.serial)},
    };
    D2D1_RECT_F d = Section(f, L"DETAILS", rows.size() * 40.0f + 12);
    for (size_t k = 0; k < rows.size(); ++k) {
        float ry = d.top + 6 + k * 40;
        if (k) Divider(d, ry);
        g_c.Text(rows[k].k, Format(13.5f), D2D1::RectF(d.left + 18, ry, d.left + 220, ry + 40), g_t.subtext);
        g_c.Text(rows[k].v, Format(13.5f), D2D1::RectF(d.left + 220, ry, d.right - 18, ry + 40), rows[k].v == L"—" ? g_t.faint : g_t.text);
    }
    Button(D2D1::RectF(f.right - 168, f.y - 18, f.right, f.y + 14), L"Read again", icon::Refresh, Btn::Subtle, Hit::SetRefresh);
    f.y += 30;
    EndPage(f);
}

// ---------------------------------------------------------------- polling test
// Counts the raw reports Windows receives from the mouse while you move it. Reports are drained in
// batches (GetRawInputBuffer), so even 8000 Hz costs little.
struct PollTestState {
    bool active = false;
    LARGE_INTEGER bucketStart{};
    int bucketCount = 0;
    float shown = 0;              // eased display value
    float measured = 0;           // last full bucket
    float peak = 0;
    DWORD lastEvent = 0;
    std::vector<float> history;   // last few seconds, one value per bucket
    bool sawUlx = false;
    std::map<HANDLE, bool> isUlx;
};
static PollTestState g_pt;

static void StopPollTest() {
    if (!g_pt.active) return;
    RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_REMOVE, nullptr};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    g_pt.active = false;
}

static void StartPollTest() {
    RAWINPUTDEVICE rid{0x01, 0x02, 0, g_hwnd};   // generic desktop / mouse, while Halo is in front
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) { Toast(L"Windows didn't allow the polling test.", true); return; }
    g_pt = PollTestState{};
    g_pt.active = true;
    QueryPerformanceCounter(&g_pt.bucketStart);
}

static bool RawIsUlx(HANDLE dev) {
    auto it = g_pt.isUlx.find(dev);
    if (it != g_pt.isUlx.end()) return it->second;
    wchar_t name[512] = {};
    UINT n = 512;
    bool ulx = GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, name, &n) != (UINT)-1 && IContains(name, L"vid_361d");
    g_pt.isUlx[dev] = ulx;
    return ulx;
}

static void CountRaw(const RAWINPUT* r) {
    if (r->header.dwType != RIM_TYPEMOUSE) return;
    if (r->data.mouse.lLastX == 0 && r->data.mouse.lLastY == 0) return;   // buttons / wheel only
    bool ulx = RawIsUlx(r->header.hDevice);
    if (ulx && !g_pt.sawUlx) { g_pt.sawUlx = true; g_pt.bucketCount = 0; }   // from now on, only the UltralightX
    if (g_pt.sawUlx && !ulx) return;
    ++g_pt.bucketCount;
    g_pt.lastEvent = GetTickCount();
}

static void OnRawInput(LPARAM lp) {
    if (!g_pt.active) return;
    alignas(8) BYTE one[sizeof(RAWINPUT) + 64];
    UINT size = sizeof(one);
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, one, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) CountRaw((RAWINPUT*)one);
    static std::vector<uint64_t> buf(4096);   // 32 KB, 8-byte aligned
    for (;;) {
        UINT cb = (UINT)(buf.size() * sizeof(uint64_t));
        UINT n = GetRawInputBuffer((RAWINPUT*)buf.data(), &cb, sizeof(RAWINPUTHEADER));
        if (n == 0 || n == (UINT)-1) break;
        RAWINPUT* r = (RAWINPUT*)buf.data();
        for (UINT i = 0; i < n; ++i) {
            CountRaw(r);
            r = (RAWINPUT*)(((uintptr_t)r + r->header.dwSize + 7) & ~(uintptr_t)7);   // NEXTRAWINPUTBLOCK
        }
    }
}

// called every frame while the test runs: close 250 ms buckets
static void TickPollTest() {
    if (!g_pt.active) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double span = double(now.QuadPart - g_pt.bucketStart.QuadPart) / g_qpf.QuadPart;
    if (span >= 0.25) {
        bool moving = GetTickCount() - g_pt.lastEvent < 60;
        float hz = moving && g_pt.bucketCount > 2 ? (float)(g_pt.bucketCount / span) : 0.0f;
        g_pt.measured = hz;
        if (hz > g_pt.peak) g_pt.peak = hz;
        g_pt.history.push_back(hz);
        if (g_pt.history.size() > 24) g_pt.history.erase(g_pt.history.begin());
        g_pt.bucketCount = 0;
        g_pt.bucketStart = now;
    }
    Ease(g_pt.shown, g_pt.measured, 10, 1.0f);
    g_frameMore = true;   // live readout
}

static void DrawPollTest(PageFrame& f) {
    D2D1_RECT_F s = Section(f, L"POLLING TEST", 132);
    TickPollTest();
    bool on = g_pt.active;
    g_c.Text(on ? L"Move the mouse in quick, steady circles" : L"Check the rate your PC actually receives",
             Format(14), D2D1::RectF(s.left + 18, s.top + 14, s.right - 330, s.top + 34), g_t.text);
    g_c.Text(on ? L"Only while Halo is in front. Short pauses read as 0."
                : L"Halo counts the mouse's reports to Windows while you move it",
             Format(12), D2D1::RectF(s.left + 18, s.top + 34, s.right - 330, s.top + 52), g_t.subtext);
    Button(D2D1::RectF(s.left + 18, s.top + 70, s.left + 140, s.top + 102), on ? L"Stop" : L"Start test", on ? icon::Cancel : icon::Play,
           on ? Btn::Secondary : Btn::Primary, Hit::PollTest);
    // readout
    float rx = s.right - 320;
    std::wstring big = on && g_pt.shown >= 1 ? Thousands((int)std::lround(g_pt.shown)) : L"—";
    auto* bf = Format(30, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float bw = MeasureWidth(big, bf);
    g_c.Text(big, bf, D2D1::RectF(rx, s.top + 14, rx + 200, s.top + 56), on ? g_t.text : g_t.faint);
    if (big != L"—") g_c.Text(L"Hz", Format(14), D2D1::RectF(rx + bw + 6, s.top + 28, rx + 260, s.top + 50), g_t.subtext);
    std::wstring line;
    if (on) {
        line = L"Peak " + Thousands((int)std::lround(g_pt.peak));
        if (g_ms.t.pollingHz > 0) line += L" · set to " + Thousands(g_ms.t.pollingHz);
        line += g_pt.sawUlx ? L" · UltralightX" : L" · all mice";
    } else if (g_pt.peak > 0) {
        line = L"Last test peaked at " + Thousands((int)std::lround(g_pt.peak)) + L" Hz";
    }
    g_c.Text(line, Format(12), D2D1::RectF(rx, s.top + 56, s.right - 18, s.top + 74), g_t.subtext);
    // the last few seconds as bars
    float bx = rx, by = s.bottom - 16, bh = 40;
    float scale = std::max({(float)g_ms.t.pollingHz, g_pt.peak, 1000.0f});
    for (size_t k = 0; k < 24; ++k) {
        float v = k < g_pt.history.size() ? g_pt.history[k] : 0.0f;
        float h = std::max(2.0f, v / scale * bh);
        g_c.FillRound(D2D1::RectF(bx + k * 12.5f, by - h, bx + k * 12.5f + 8, by), 2, v > 0 ? g_t.accent : Alpha(g_t.text, 0.10f));
    }
}

// ---------------------------------------------------------------- performance
static void TextField(const D2D1_RECT_F& r, const std::wstring& shown, Hit kind, int index, bool enabled, IDWriteTextFormat* f, D2D1_COLOR_F color,
                      bool quiet = false) {
    bool editing = g_edit.active && g_edit.kind == kind && g_edit.index == index;
    bool hot = enabled && IsHover(kind, index);
    if (editing || hot || !quiet) {
        g_c.FillRound(r, 6, editing ? (g_t.dark ? Rgb(0x000000, 0.25f) : Rgb(0xFFFFFF)) : hot ? g_t.surfaceHover : Mix(g_t.surface, g_t.surfaceStrong, 0.35f));
        g_c.StrokeRound(r, 6, editing ? g_t.accent : g_t.divider, editing ? 1.5f : 1.0f);
    }
    std::wstring text = editing ? g_edit.text : shown;
    D2D1_RECT_F tr = D2D1::RectF(r.left + 12, r.top, r.right - 10, r.bottom);
    g_c.Text(text, f, tr, enabled ? color : g_t.faint);
    if (editing && g_caretOn) {
        float cw = MeasureWidth(text, f);
        float cx = tr.left + cw + 1;
        if (f->GetTextAlignment() == DWRITE_TEXT_ALIGNMENT_CENTER) cx = (tr.left + tr.right) / 2 + cw / 2 + 1;
        g_c.Line(cx, r.top + 9, cx, r.bottom - 9, g_t.text, 1.2f);
    }
    AddHot(r, kind, index, 0, enabled);
}

static void DrawPerformance(float x0) {
    PageFrame f = BeginPage(x0, L"Performance", L"Changes go straight to the mouse and stay on it, with or without Halo.");
    if (NoMouseCard(f, true) ) { EndPage(f); return; }
    const ulx::Telemetry& t = g_ms.t;
    bool en = CanWrite();

    // DPI
    auto* chipF = Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    // lay out the preset chips first to know the card height
    float avail = f.right - f.left - 36;
    std::vector<D2D1_RECT_F> chips;
    float cx = 0, cyRow = 0;
    for (size_t k = 0; k <= g_cfg.dpiPresets.size(); ++k) {
        std::wstring l = k < g_cfg.dpiPresets.size() ? Thousands(g_cfg.dpiPresets[k]) : L"Save " + (t.dpi > 0 ? Thousands(t.dpi) : L"") + L" as preset";
        float w = MeasureWidth(l, chipF) + (k < g_cfg.dpiPresets.size() ? 30 : 46);
        if (cx > 0 && cx + w > avail) { cx = 0; cyRow += 40; }
        chips.push_back(D2D1::RectF(cx, cyRow, cx + w, cyRow + 32));
        cx += w + 8;
    }
    float h = 150 + cyRow + 32 + 18;
    D2D1_RECT_F s = Section(f, L"DPI", h);
    g_c.Text(L"Sensitivity", Format(14), D2D1::RectF(s.left + 18, s.top + 12, s.right - 300, s.top + 32), g_t.text);
    g_c.Text(L"Any value from 50 to 26,000. Click the number to type one.", Format(12), D2D1::RectF(s.left + 18, s.top + 32, s.right - 300, s.top + 50), g_t.subtext);
    // big number + stepper
    float fy = s.top + 66;
    Button(D2D1::RectF(s.left + 18, fy, s.left + 62, fy + 52), L"", icon::Remove, Btn::Secondary, Hit::DpiStep, -1, -1, en && t.dpi > ulx::kDpiMin);
    TextField(D2D1::RectF(s.left + 70, fy, s.left + 230, fy + 52), t.dpi > 0 ? std::to_wstring(t.dpi) : L"—", Hit::DpiField, -1, en,
              Format(26, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), g_t.text);
    Button(D2D1::RectF(s.left + 238, fy, s.left + 282, fy + 52), L"", icon::Add, Btn::Secondary, Hit::DpiStep, -1, 1, en && t.dpi < ulx::kDpiMax);
    g_c.Text(g_edit.active && g_edit.kind == Hit::DpiField ? L"Enter to apply · Esc to cancel" : L"Steps of 50 · Shift for 10",
             Format(12), D2D1::RectF(s.left + 296, fy, s.right - 18, fy + 52), g_t.faint);
    // presets
    float py = s.top + 136;
    g_c.Text(L"PRESETS · right-click one to remove it", Format(11, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(s.left + 18, py - 6, s.right, py + 12), g_t.faint);
    py += 16;
    for (size_t k = 0; k < chips.size(); ++k) {
        D2D1_RECT_F r = D2D1::RectF(s.left + 18 + chips[k].left, py + chips[k].top, s.left + 18 + chips[k].right, py + chips[k].bottom);
        if (k < g_cfg.dpiPresets.size()) {
            bool sel = t.dpi == g_cfg.dpiPresets[k];
            bool hot = en && IsHover(Hit::DpiPreset, (int)k);
            g_c.FillRound(r, 16, sel ? Alpha(g_t.accent, 0.18f) : hot ? g_t.surfaceHover : Mix(g_t.surface, g_t.surfaceStrong, 0.4f));
            if (sel) g_c.StrokeRound(r, 16, g_t.accent, 1.5f);
            g_c.Text(Thousands(g_cfg.dpiPresets[k]), chipF, r, en ? g_t.text : g_t.faint);
            AddHot(r, Hit::DpiPreset, (int)k, 0, en);
        } else {
            bool can = t.dpi > 0 && std::find(g_cfg.dpiPresets.begin(), g_cfg.dpiPresets.end(), t.dpi) == g_cfg.dpiPresets.end() && g_cfg.dpiPresets.size() < 12;
            bool hot = can && IsHover(Hit::DpiPresetAdd);
            g_c.FillRound(r, 16, hot ? g_t.surfaceHover : Alpha(g_t.surface, 0.0f));
            g_c.StrokeRound(r, 16, can ? Alpha(g_t.text, 0.25f) : g_t.divider);
            g_c.Glyph(icon::Add, Format(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 8, r.top, r.left + 30, r.bottom), can ? g_t.subtext : g_t.faint);
            g_c.Text(MeasureWidth(L"x", chipF) > 0 ? (L"Save " + (t.dpi > 0 ? Thousands(t.dpi) : L"") + L" as preset") : L"",
                     Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(r.left + 22, r.top, r.right - 4, r.bottom), can ? g_t.subtext : g_t.faint);
            AddHot(r, Hit::DpiPresetAdd, -1, 0, can);
        }
    }

    // polling, motion sync, lift-off
    D2D1_RECT_F s2 = Section(f, L"TRACKING", 64 * 3);
    std::vector<std::wstring> polls;
    int psel = -1;
    for (int k = 0; k < 5; ++k) { polls.push_back(std::to_wstring(ulx::kPollingRates[k]) + L" Hz"); if (t.pollingHz == ulx::kPollingRates[k]) psel = k; }
    float pw = SegmentedWidth(polls);
    RowLabel(s2, s2.top + 4, L"Polling rate", t.pollingHz >= 4000 ? L"Higher rates use more battery and CPU" : L"How often the mouse reports to the PC", pw + 40);
    Segmented(s2.right - 18 - pw, s2.top + 16, polls, psel, Hit::Poll, en);
    Divider(s2, s2.top + 64);
    RowLabel(s2, s2.top + 68, L"Motion Sync", L"Lines sensor reads up with USB polls: steadier, very slightly later");
    Segmented(s2.right - 18 - SegmentedWidth({L"On", L"Off"}), s2.top + 80, {L"On", L"Off"}, t.motionSync == 1 ? 0 : t.motionSync == 0 ? 1 : -1, Hit::MotionSync, en);
    Divider(s2, s2.top + 128);
    RowLabel(s2, s2.top + 132, L"Lift-off distance", L"How high you can lift the mouse before it stops tracking");
    Segmented(s2.right - 18 - SegmentedWidth({L"1 mm", L"2 mm"}), s2.top + 144, {L"1 mm", L"2 mm"}, t.liftOffMm == 1 ? 0 : t.liftOffMm == 2 ? 1 : -1, Hit::LiftOff, en);
    DrawPollTest(f);
    EndPage(f);
}

// ---------------------------------------------------------------- scroll & dongle
static void DrawExtras(float x0) {
    PageFrame f = BeginPage(x0, L"Scroll & dongle", L"Saved on the mouse, like everything else.");
    if (NoMouseCard(f, true)) { EndPage(f); return; }
    const ulx::Telemetry& t = g_ms.t;
    bool en = CanWrite();

    D2D1_RECT_F s = Section(f, L"TOURNAMENT SCROLL", 64 * 2 + 58);
    auto* bf = Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_c.Text(L"Ignores the first scroll tick after the wheel has been still, so a bumped wheel doesn't jump or "
             L"switch weapons. Handy if the wheel is bound to something in game.",
             bf, D2D1::RectF(s.left + 18, s.top + 12, s.right - 18, s.top + 52), g_t.subtext);
    std::vector<std::wstring> modes = {L"Off", L"Up", L"Down", L"Both"};
    RowLabel(s, s.top + 58, L"Filter", L"Which direction's first tick is ignored", SegmentedWidth(modes) + 40);
    Segmented(s.right - 18 - SegmentedWidth(modes), s.top + 70, modes, t.scrollMode, Hit::ScrollMode, en);
    Divider(s, s.top + 122);
    std::vector<std::wstring> times = {L"100 ms", L"500 ms", L"1 s", L"1.5 s"};
    int tsel = -1;
    for (int k = 0; k < 4; ++k) if (t.scrollTimeoutMs == ulx::kScrollTimeouts[k]) tsel = k;
    RowLabel(s, s.top + 126, L"Still for", L"How long the wheel must rest before the next tick is ignored", SegmentedWidth(times) + 40);
    Segmented(s.right - 18 - SegmentedWidth(times), s.top + 138, times, tsel, Hit::ScrollTimeout, en && t.scrollMode > 0);

    D2D1_RECT_F s2 = Section(f, L"DONGLE", 64);
    std::vector<std::wstring> leds = {L"Off", L"Battery color", L"Solid white"};
    RowLabel(s2, s2.top + 4, L"Dongle light", L"Battery color: green above 50%, yellow, orange, red below 10%", SegmentedWidth(leds) + 40);
    Segmented(s2.right - 18 - SegmentedWidth(leds), s2.top + 16, leds, t.dongleLed, Hit::DongleLed, en);
    EndPage(f);
}

// ---------------------------------------------------------------- profiles
static Profile CurrentAsProfile(const std::wstring& name) {
    Profile p;
    p.name = name;
    const ulx::Telemetry& t = g_ms.t;
    if (t.dpi > 0) p.dpi = t.dpi;
    if (t.pollingHz > 0) p.polling = t.pollingHz;
    if (t.motionSync >= 0) p.motionSync = t.motionSync;
    if (t.liftOffMm > 0) p.liftOff = t.liftOffMm;
    if (t.dongleLed >= 0) p.dongleLed = t.dongleLed;
    if (t.scrollMode >= 0) p.scrollMode = t.scrollMode;
    if (t.scrollTimeoutMs > 0) p.scrollTimeout = t.scrollTimeoutMs;
    return p;
}

static bool ProfileActive(const Profile& p) {
    const ulx::Telemetry& t = g_ms.t;
    return Connected() && t.dpi == p.dpi && t.pollingHz == p.polling && t.motionSync == p.motionSync && t.liftOffMm == p.liftOff &&
           (t.dongleLed < 0 || t.dongleLed == p.dongleLed) && (t.scrollMode < 0 || t.scrollMode == p.scrollMode);
}

static std::wstring ProfileSummary(const Profile& p) {
    std::wstring s = Thousands(p.dpi) + L" DPI · " + Thousands(p.polling) + L" Hz · Motion Sync " + (p.motionSync ? L"on" : L"off") +
                     L" · " + std::to_wstring(p.liftOff) + L" mm lift-off";
    if (p.scrollMode > 0) s += L" · tournament scroll " + Lower(mouse::ScrollModeName(p.scrollMode));
    return s;
}

enum class From { Window, Outside, Auto };   // where a change came from: the window shows a toast, the rest the OSD

static void ShowOsd(const std::wstring& title, const std::wstring& sub, float level) {
    if (!g_cfg.osd) return;
    if (Visible() && GetForegroundWindow() == g_hwnd) return;   // you're looking at Halo already
    osd::Show(title, sub, level, g_t.accent);
}

// automatic per-app profiles
static int g_autoActive = -1;          // the profile switched to for the app in front (-1: none)
static Profile g_baseline;             // what to go back to afterwards
static std::wstring g_foregroundExe;
static HWINEVENTHOOK g_fgHook = nullptr;

static void SendProfile(const Profile& p) {
    mouse::SetDpi(p.dpi);
    mouse::SetPollingRate(p.polling);
    mouse::SetMotionSync(p.motionSync != 0);
    mouse::SetLiftOff(p.liftOff);
    mouse::SetDongleLed(p.dongleLed);
    mouse::SetScrollMode(p.scrollMode);
    if (p.scrollMode > 0) mouse::SetScrollTimeout(p.scrollTimeout);
}

static void ApplyProfile(const Profile& p, From from) {
    SendProfile(p);
    if (from != From::Auto) g_autoActive = -1;   // picked by hand: stay on it
    if (from == From::Window) Toast(L"Switched to " + p.name + L".");
    else ShowOsd(p.name, Thousands(p.dpi) + L" DPI · " + Thousands(p.polling) + L" Hz", -1);
}

static std::wstring ExeOfWindow(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    std::wstring name;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) name = Lower(PathName(buf));
        CloseHandle(p);
    }
    return name;
}

static void CheckForeground() {
    if (!g_cfg.autoSwitch || !CanWrite() || !g_ms.readComplete) return;
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd) return;               // Halo itself doesn't change anything
    std::wstring exe = ExeOfWindow(fg);
    if (exe.empty() || exe == Lower(HALO_EXE_NAME)) return;
    g_foregroundExe = exe;
    int match = -1;
    for (size_t k = 0; k < g_cfg.profiles.size() && match < 0; ++k)
        for (auto& a : g_cfg.profiles[k].apps) if (a == exe) { match = (int)k; break; }
    if (match == g_autoActive) return;
    if (match >= 0) {
        if (g_autoActive < 0) g_baseline = CurrentAsProfile(L"Your settings");
        g_autoActive = match;
        const Profile& p = g_cfg.profiles[match];
        Log(L"auto profile: %ls for %ls", p.name.c_str(), exe.c_str());
        SendProfile(p);
        ShowOsd(p.name, L"Switched for " + exe, -1);
    } else {
        g_autoActive = -1;
        Log(L"auto profile: back to your settings (%ls)", exe.c_str());
        SendProfile(g_baseline);
        ShowOsd(L"Your settings", Thousands(g_baseline.dpi) + L" DPI · " + Thousands(g_baseline.polling) + L" Hz", -1);
    }
    StartAnim();
}

static void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    // Alt+Tab passes through several windows; act on where it lands
    if (g_hwnd) SetTimer(g_hwnd, TIMER_FOREGROUND, 350, nullptr);
}

// hotkeys
enum { HK_DPI_UP = 1, HK_DPI_DOWN, HK_PROFILE };
static std::wstring g_hotkeyError;

static void RegisterHotkeys() {
    UnregisterHotKey(g_hwnd, HK_DPI_UP);
    UnregisterHotKey(g_hwnd, HK_DPI_DOWN);
    UnregisterHotKey(g_hwnd, HK_PROFILE);
    g_hotkeyError.clear();
    if (!g_cfg.hotkeys) return;
    struct { int id; UINT vk; const wchar_t* name; } keys[] = {
        {HK_DPI_UP, VK_UP, L"Ctrl+Alt+Up"}, {HK_DPI_DOWN, VK_DOWN, L"Ctrl+Alt+Down"}, {HK_PROFILE, 'P', L"Ctrl+Alt+P"}};
    for (auto& k : keys)
        if (!RegisterHotKey(g_hwnd, k.id, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, k.vk))
            g_hotkeyError += (g_hotkeyError.empty() ? L"" : L", ") + std::wstring(k.name);
    if (!g_hotkeyError.empty()) g_hotkeyError += L" taken by another app";
}

static void CycleDpi(int dir) {
    if (!CanWrite() || g_cfg.dpiPresets.empty() || g_ms.t.dpi <= 0) return;
    std::vector<int> p = g_cfg.dpiPresets;
    std::sort(p.begin(), p.end());
    int cur = g_ms.t.dpi, next = dir > 0 ? p.front() : p.back();
    if (dir > 0) { for (int v : p) if (v > cur) { next = v; break; } }
    else { for (auto it = p.rbegin(); it != p.rend(); ++it) if (*it < cur) { next = *it; break; } }
    mouse::SetDpi(next);
    size_t idx = std::find(p.begin(), p.end(), next) - p.begin();
    ShowOsd(Thousands(next) + L" DPI", L"Preset " + std::to_wstring(idx + 1) + L" of " + std::to_wstring(p.size()), (idx + 1) / (float)p.size());
}

static void CycleProfile() {
    if (!CanWrite() || g_cfg.profiles.empty()) return;
    int cur = -1;
    for (size_t k = 0; k < g_cfg.profiles.size(); ++k) if (ProfileActive(g_cfg.profiles[k])) { cur = (int)k; break; }
    ApplyProfile(g_cfg.profiles[(cur + 1) % g_cfg.profiles.size()], From::Outside);
}

// the running apps you can attach to a profile: visible top-level windows, by exe
static std::vector<std::wstring> RunningApps() {
    std::vector<std::wstring> out;
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* list = (std::vector<std::wstring>*)lp;
        if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        if (GetWindowTextLengthW(w) == 0) return TRUE;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;
        std::wstring exe = ExeOfWindow(w);
        static const wchar_t* skip[] = {L"explorer.exe", L"applicationframehost.exe", L"textinputhost.exe", L"shellexperiencehost.exe",
                                        L"searchhost.exe", L"startmenuexperiencehost.exe", L"lockapp.exe", L"systemsettings.exe"};
        if (exe.empty() || exe == Lower(HALO_EXE_NAME)) return TRUE;
        for (auto* k : skip) if (exe == k) return TRUE;
        if (std::find(list->begin(), list->end(), exe) == list->end()) list->push_back(exe);
        return TRUE;
    }, (LPARAM)&out);
    std::sort(out.begin(), out.end());
    if (out.size() > 40) out.resize(40);
    return out;
}

static void AddAppToProfile(int index) {
    if (index < 0 || index >= (int)g_cfg.profiles.size()) return;
    auto apps = RunningApps();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Switch to this profile while this app is in front:");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t k = 0; k < apps.size(); ++k) AppendMenuW(m, MF_STRING, 1 + k, apps[k].c_str());
    if (apps.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(no other apps open)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1000, L"Choose an .exe file…");
    POINT pt;
    GetCursorPos(&pt);
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    std::wstring exe;
    if (cmd >= 1 && cmd <= apps.size()) exe = apps[cmd - 1];
    else if (cmd == 1000) {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = g_hwnd;
        ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) exe = Lower(PathName(file));
    }
    if (exe.empty()) return;
    // an app belongs to one profile
    for (auto& p : g_cfg.profiles) p.apps.erase(std::remove(p.apps.begin(), p.apps.end(), exe), p.apps.end());
    g_cfg.profiles[index].apps.push_back(exe);
    ConfigSave();
    Toast(g_cfg.profiles[index].name + L" now switches on for " + exe + L".");
}

static void DrawProfiles(float x0) {
    PageFrame f = BeginPage(x0, L"Profiles", L"Save your settings as a profile, then switch in one click here or from the tray.");
    bool en = CanWrite();
    {
        float w = ButtonWidth(L"Save current settings", icon::Save);
        Button(D2D1::RectF(f.right - w, f.y - 56, f.right, f.y - 24), L"Save current settings", icon::Save, Btn::Primary, Hit::ProfileSave, -1, 0,
               Connected() && g_ms.readComplete);
    }
    if (g_cfg.profiles.empty()) {
        D2D1_RECT_F r = Section(f, L"", 96);
        g_c.Text(L"No profiles yet", Format(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 20, r.top + 20, r.right - 20, r.top + 42), g_t.text);
        g_c.Text(L"Set the mouse up the way you like it, then press Save current settings. Make one for each game.",
                 Format(12.5f), D2D1::RectF(r.left + 20, r.top + 46, r.right - 20, r.top + 66), g_t.subtext);
        EndPage(f);
        return;
    }
    for (size_t k = 0; k < g_cfg.profiles.size(); ++k) {
        const Profile& p = g_cfg.profiles[k];
        D2D1_RECT_F r = D2D1::RectF(f.left, f.y, f.right, f.y + 118);
        bool active = ProfileActive(p);
        g_c.FillRound(r, 10, g_t.surface);
        if (active) g_c.StrokeRound(r, 10, Alpha(g_t.accent, 0.6f), 1.5f);
        float nameW = std::min(260.0f, r.right - r.left - 300);
        TextField(D2D1::RectF(r.left + 12, r.top + 10, r.left + 12 + nameW, r.top + 40), p.name, Hit::ProfileName, (int)k, true,
                  Format(14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), g_t.text, true);
        float px = r.left + 22 + nameW;
        if (active) px += Pill(px, r.top + 15, L"Active", g_t.accent) + 6;
        if (g_autoActive == (int)k) Pill(px, r.top + 15, L"Auto · " + g_foregroundExe, g_t.info);
        g_c.Text(ProfileSummary(p), Format(12.5f), D2D1::RectF(r.left + 24, r.top + 44, r.right - 230, r.top + 66), g_t.subtext);
        Button(D2D1::RectF(r.right - 186, r.top + 22, r.right - 62, r.top + 54), active ? L"Applied" : L"Apply", icon::Play, Btn::Primary,
               Hit::ProfileApply, (int)k, 0, en && !active);
        Button(D2D1::RectF(r.right - 54, r.top + 22, r.right - 18, r.top + 54), L"", icon::Delete, Btn::Subtle, Hit::ProfileDelete, (int)k);
        // apps that switch to it
        float ay = r.top + 78, ax = r.left + 24;
        g_c.Text(L"Auto-switch for", Format(12), D2D1::RectF(ax, ay, ax + 110, ay + 26), g_t.faint);
        ax += 108;
        auto* cf = Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        for (size_t a = 0; a < p.apps.size(); ++a) {
            float w = MeasureWidth(p.apps[a], cf) + 40;
            if (ax + w > r.right - 140) { g_c.Text(L"+" + std::to_wstring(p.apps.size() - a) + L" more", Format(12), D2D1::RectF(ax, ay, ax + 80, ay + 26), g_t.faint); ax += 70; break; }
            D2D1_RECT_F c = D2D1::RectF(ax, ay, ax + w, ay + 26);
            float h = HoverAmt(Hit::ProfileRemoveApp, (int)k, (int)a);
            g_c.FillRound(c, 13, Mix(Mix(g_t.surface, g_t.surfaceStrong, 0.6f), Alpha(g_t.bad, 0.18f), h));
            g_c.Text(p.apps[a], cf, D2D1::RectF(c.left + 12, c.top, c.right - 24, c.bottom), g_t.text);
            g_c.Glyph(icon::Cancel, Format(8, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true),
                      D2D1::RectF(c.right - 24, c.top, c.right - 6, c.bottom), Mix(g_t.faint, g_t.bad, h));
            AddHot(c, Hit::ProfileRemoveApp, (int)k, (int)a);
            ax += w + 6;
        }
        {
            std::wstring l = p.apps.empty() ? L"Add a game or app" : L"Add";
            float w = MeasureWidth(l, Format(12)) + 34;
            D2D1_RECT_F c = D2D1::RectF(ax, ay, ax + w, ay + 26);
            float h = HoverAmt(Hit::ProfileAddApp, (int)k);
            g_c.FillRound(c, 13, Mix(Alpha(g_t.surfaceHover, 0), g_t.surfaceHover, h));
            g_c.StrokeRound(c, 13, Alpha(g_t.text, 0.22f));
            g_c.Glyph(icon::Add, Format(9, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(c.left + 6, c.top, c.left + 24, c.bottom), g_t.subtext);
            g_c.Text(l, Format(12), D2D1::RectF(c.left + 24, c.top, c.right - 4, c.bottom), g_t.subtext);
            AddHot(c, Hit::ProfileAddApp, (int)k);
        }
        f.y += 118 + 10;
    }
    f.y += 4;
    g_c.Text(L"Click a name to rename it. Ctrl+Alt+P switches to the next profile from anywhere.", Format(12), D2D1::RectF(f.left, f.y, f.right, f.y + 18), g_t.faint);
    f.y += 34;
    D2D1_RECT_F sa = Section(f, L"AUTO-SWITCH", 64);
    RowLabel(sa, sa.top + 4, L"Switch profiles for games and apps",
             L"While an app is in front its profile is used; when you leave it, your settings come back", 160);
    Segmented(sa.right - 18 - SegmentedWidth({L"On", L"Off"}), sa.top + 16, {L"On", L"Off"}, g_cfg.autoSwitch ? 0 : 1, Hit::SetAuto);
    EndPage(f);
}

// ---------------------------------------------------------------- themes
// index 0 is "Match Hangar"; then the built-in themes, then custom files
static std::vector<ThemeDef> GalleryThemes() {
    std::vector<ThemeDef> all;
    bool found = false;
    ThemeDef h = HangarThemeDef(nullptr, &found);
    h.id = L"hangar";
    h.name = L"Match Hangar";
    h.description = g_hasHangarLive ? L"Live from Hangar" : found ? L"Uses Hangar's theme" : L"Hangar isn't installed · Dark";
    h.custom = false;
    all.push_back(h);
    for (auto& t : BuiltinThemes()) all.push_back(t);
    all.insert(all.end(), g_customThemes.begin(), g_customThemes.end());
    return all;
}

static bool PickCustomAccent() {
    static COLORREF custom[16] = {};
    uint32_t cur = ResolveAccent(FindTheme(g_cfg.theme, g_customThemes), g_cfg.accent);
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = g_hwnd;
    cc.rgbResult = RGB((cur >> 16) & 0xFF, (cur >> 8) & 0xFF, cur & 0xFF);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    g_cfg.accent = HexColor((GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult));
    return true;
}

static void ExportTheme() {
    ThemeDef cur;
    std::wstring accent = g_cfg.accent;
    if (MatchingHangar()) cur = HangarThemeDef(&accent, nullptr);
    else cur = FindTheme(g_cfg.theme, g_customThemes);
    if (cur.followWindows) cur = BuiltinThemes()[SystemUsesLightTheme() ? 1 : 0];
    std::wstring base = cur.custom ? cur.name : cur.name + L" (copy)";
    for (auto& ch : base) if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'-';
    EnsureDir(ThemesDir());
    std::wstring path = ThemesDir() + L"\\" + base + L".json";
    for (int n = 2; FileExists(path) && !cur.custom; ++n) path = ThemesDir() + L"\\" + base + L" " + std::to_wstring(n) + L".json";
    if (!WriteTextFileAtomic(path, ThemeToJson(cur, accent))) { Toast(L"Couldn't save the theme file.", true); return; }
    g_customThemes = LoadCustomThemes();
    Toast(L"Saved " + PathName(path) + L". Edit it, then press Reload.");
    ShellExecuteW(g_hwnd, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// A miniature Halo window drawn in the theme's colors
static void DrawThemePreview(const ThemeDef& def0, const D2D1_RECT_F& r, int index, bool selected) {
    ThemeDef def = def0;
    std::wstring accentSetting = selected && !MatchingHangar() ? g_cfg.accent : L"theme";
    if (def0.id == L"hangar") HangarThemeDef(&accentSetting, nullptr);
    if (def.followWindows) def = BuiltinThemes()[SystemUsesLightTheme() ? 1 : 0];
    Theme pt = MakeTheme(def, accentSetting);
    bool hot = IsHover(Hit::ThemePick, -1, index);
    g_c.FillRound(r, 10, hot ? g_t.surfaceHover : g_t.surface);
    D2D1_RECT_F pv = D2D1::RectF(r.left + 10, r.top + 10, r.right - 10, r.bottom - 50);
    ID2D1RoundedRectangleGeometry* clip = nullptr;
    d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(pv, 7, 7), &clip);
    ID2D1Layer* layer = nullptr;
    g_rt->CreateLayer(&layer);
    if (clip && layer) g_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), layer);
    if (def.backdrop == L"acrylic" || def.surfaceOpacity < 1.0f) {
        ID2D1GradientStopCollection* stops = nullptr;
        D2D1_GRADIENT_STOP gs[3] = {{0, Rgb(0x3A5BD9)}, {0.55f, Rgb(0x8E4FD6)}, {1, Rgb(0xE0679A)}};
        g_rt->CreateGradientStopCollection(gs, 3, &stops);
        ID2D1LinearGradientBrush* lg = nullptr;
        if (stops) g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(pv.left, pv.top), D2D1::Point2F(pv.right, pv.bottom)), stops, &lg);
        if (lg) { g_rt->FillRectangle(pv, lg); lg->Release(); }
        if (stops) stops->Release();
        g_c.Fill(pv, Rgb(def.bg, 1.0f - def.transparency));
    } else {
        g_c.Fill(pv, Rgb(def.bg));
    }
    float w = pv.right - pv.left, h = pv.bottom - pv.top;
    D2D1_RECT_F sb = D2D1::RectF(pv.left, pv.top, pv.left + w * 0.27f, pv.bottom);
    g_c.Fill(sb, def.backdrop == L"acrylic" ? Rgb(def.sidebar, 0.35f) : Rgb(def.sidebar));
    g_c.FillRound(D2D1::RectF(sb.left + 6, pv.top + 7, sb.left + 14, pv.top + 15), 2, pt.accent);
    for (int k = 0; k < 4; ++k) {
        float ly = pv.top + 24 + k * 11;
        if (k == 0) g_c.FillRound(D2D1::RectF(sb.left + 4, ly - 3, sb.right - 4, ly + 6), 2, Alpha(pt.text, 0.10f));
        g_c.FillRound(D2D1::RectF(sb.left + 8, ly, sb.left + 8 + (sb.right - sb.left - 18) * (k == 0 ? 0.8f : 0.55f + 0.1f * (k % 2)), ly + 3), 1.5f, k == 0 ? pt.text : pt.subtext);
    }
    // content: a battery ring card and two tiles
    float cx = sb.right + 8;
    g_c.FillRound(D2D1::RectF(cx, pv.top + 8, cx + w * 0.22f, pv.top + 12), 2, pt.text);
    float ch = h - 30;
    D2D1_RECT_F card = D2D1::RectF(cx, pv.top + 20, cx + (pv.right - cx - 8) * 0.48f, pv.top + 20 + ch);
    g_c.FillRound(card, 4, def.surfaceOpacity < 1.0f ? Rgb(def.surface, def.surfaceOpacity * 1.8f) : pt.surface);
    float rr = std::min(card.right - card.left, card.bottom - card.top) * 0.30f;
    float ccx = (card.left + card.right) / 2, ccy = (card.top + card.bottom) / 2;
    g_br->SetColor(Alpha(pt.text, 0.12f));
    g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ccx, ccy), rr, rr), g_br, 3.5f);
    Arc(ccx, ccy, rr, 0.72f, pt.accent, 3.5f);
    float tx = card.right + 5, tw = pv.right - 8 - tx, th = (ch - 5) / 2;
    for (int k = 0; k < 2; ++k) {
        D2D1_RECT_F c = D2D1::RectF(tx, pv.top + 20 + k * (th + 5), tx + tw, pv.top + 20 + k * (th + 5) + th);
        g_c.FillRound(c, 4, def.surfaceOpacity < 1.0f ? Rgb(def.surface, def.surfaceOpacity * 1.8f) : pt.surface);
        g_c.FillRound(D2D1::RectF(c.left + 5, c.top + 5, c.left + 10, c.top + 10), 2, Alpha(pt.accent, 0.9f));
        g_c.FillRound(D2D1::RectF(c.left + 5, c.top + 14, c.left + 5 + tw * 0.5f, c.top + 18), 1.5f, pt.text);
        g_c.FillRound(D2D1::RectF(c.left + 5, c.top + 21, c.left + 5 + tw * 0.3f, c.top + 23.5f), 1.2f, pt.subtext);
    }
    if (layer) { g_rt->PopLayer(); layer->Release(); }
    if (clip) clip->Release();
    g_c.StrokeRound(pv, 7, Alpha(g_t.text, 0.10f));
    g_c.Text(def0.name, Format(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 14, r.bottom - 44, r.right - 40, r.bottom - 24), g_t.text);
    g_c.Text(def0.description, Format(11.5f), D2D1::RectF(r.left + 14, r.bottom - 25, r.right - 12, r.bottom - 8), g_t.subtext);
    if (def0.custom) Pill(r.right - 12, r.bottom - 43, PathStartsWith(def0.file, HangarThemesDir()) ? L"Suite" : L"Custom", g_t.info, true);
    if (def0.id == L"hangar") Pill(r.right - 12, r.bottom - 43, L"Suite", g_t.info, true);
    if (selected) {
        g_c.StrokeRound(r, 10, g_t.accent, 2);
        g_c.Circle(r.right - 22, r.top + 22, 10, g_t.accent);
        g_c.Glyph(icon::Check, Format(10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true),
                  D2D1::RectF(r.right - 32, r.top + 12, r.right - 12, r.top + 32), g_t.onAccent);
    } else if (hot) {
        g_c.StrokeRound(r, 10, Alpha(g_t.accent, 0.5f));
    }
    AddHot(r, Hit::ThemePick, -1, index);
}

static void DrawThemes(float x0) {
    PageFrame f = BeginPage(x0, L"Themes", L"Click a theme to try it. Match Hangar keeps Halo in step with the rest of your suite.", 1100);
    auto all = GalleryThemes();
    float gap = 14, minW = 200, avail = f.right - f.left;
    int cols = std::max(1, (int)((avail + gap) / (minW + gap)));
    float cw = (avail - gap * (cols - 1)) / cols, ch = std::min(190.0f, cw * 0.62f + 50);
    for (size_t k = 0; k < all.size(); ++k) {
        int c = (int)(k % cols), row = (int)(k / cols);
        D2D1_RECT_F r = D2D1::RectF(f.left + c * (cw + gap), f.y + row * (ch + gap), f.left + c * (cw + gap) + cw, f.y + row * (ch + gap) + ch);
        if (r.bottom > 0 && r.top < g_H) DrawThemePreview(all[k], r, (int)k, all[k].id == g_cfg.theme);
    }
    f.y += ((int)all.size() + cols - 1) / cols * (ch + gap) + 18;

    g_c.Text(L"ACCENT COLOR", Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(f.left, f.y, f.right, f.y + 20), g_t.subtext);
    f.y += 28;
    D2D1_RECT_F box = D2D1::RectF(f.left, f.y, f.right, f.y + 64);
    g_c.FillRound(box, 10, g_t.surface);
    if (MatchingHangar()) {
        g_c.Text(L"Hangar's accent color is used while Halo matches Hangar. Pick another theme to choose your own.",
                 Format(12.5f), D2D1::RectF(f.left + 18, f.y, f.right - 18, f.y + 64), g_t.subtext);
    } else {
        float ax = f.left + 16, ay = f.y + 16;
        ThemeDef cur = FindTheme(g_cfg.theme, g_customThemes);
        auto chip = [&](const std::wstring& label, uint32_t color, int arg, bool sel) {
            auto* tf = Format(12.5f, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
            float w = MeasureWidth(label, tf) + 42;
            D2D1_RECT_F r = D2D1::RectF(ax, ay, ax + w, ay + 32);
            g_c.FillRound(r, 16, sel ? Alpha(g_t.accent, 0.18f) : (IsHover(Hit::AccentPick, -1, arg) ? g_t.surfaceHover : Mix(g_t.surface, g_t.surfaceStrong, 0.4f)));
            if (sel) g_c.StrokeRound(r, 16, g_t.accent, 1.5f);
            g_c.Circle(r.left + 16, ay + 16, 7, Rgb(color));
            g_c.Text(label, tf, D2D1::RectF(r.left + 29, ay, r.right - 8, ay + 32), g_t.text);
            AddHot(r, Hit::AccentPick, -1, arg);
            ax += w + 8;
        };
        chip(L"Theme's own", ResolveAccent(cur, L"theme"), 0, g_cfg.accent == L"theme");
        chip(L"Windows", SystemAccent(), 1, g_cfg.accent == L"windows");
        ax += 6;
        bool customSel = g_cfg.accent.size() == 7 && g_cfg.accent[0] == L'#';
        for (int k = 0; k < kAccentSwatchCount; ++k) {
            bool sel = IEquals(g_cfg.accent, HexColor(kAccentSwatches[k]));
            if (sel) customSel = false;
            float cxx = ax + 16;
            if (cxx + 16 > f.right - 110) break;
            if (sel) g_c.Circle(cxx, ay + 16, 15, g_t.text);
            else if (IsHover(Hit::AccentPick, -1, 2 + k)) g_c.Circle(cxx, ay + 16, 15, Alpha(g_t.text, 0.3f));
            g_c.Circle(cxx, ay + 16, sel ? 12.5f : 13, Rgb(kAccentSwatches[k]));
            AddHot(D2D1::RectF(ax, ay, ax + 32, ay + 32), Hit::AccentPick, -1, 2 + k);
            ax += 38;
        }
        ax += 4;
        chip(customSel ? g_cfg.accent : L"Custom…", customSel ? ResolveAccent(cur, g_cfg.accent) : 0x888888, 100, customSel);
    }
    f.y += 64 + 26;

    g_c.Text(L"THEME FILES", Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(f.left, f.y, f.right, f.y + 20), g_t.subtext);
    f.y += 28;
    D2D1_RECT_F b2 = D2D1::RectF(f.left, f.y, f.right, f.y + 64);
    g_c.FillRound(b2, 10, g_t.surface);
    g_c.Text(L"Same format as Hangar", Format(14), D2D1::RectF(f.left + 18, f.y + 12, f.right - 420, f.y + 32), g_t.text);
    g_c.Text(L"Files in Halo's themes folder and in Hangar's appear above.", Format(12), D2D1::RectF(f.left + 18, f.y + 32, f.right - 420, f.y + 50), g_t.subtext);
    float bx = f.right - 18;
    auto btn = [&](const std::wstring& l, wchar_t g, Hit kind) {
        float w = ButtonWidth(l, g);
        Button(D2D1::RectF(bx - w, f.y + 16, bx, f.y + 48), l, g, Btn::Secondary, kind);
        bx -= w + 8;
    };
    btn(L"Reload", icon::Refresh, Hit::ThemeReload);
    btn(L"Open folder", icon::Folder, Hit::ThemeFolder);
    btn(L"Save as file", icon::Download, Hit::ThemeExport);
    f.y += 64 + 20;
    EndPage(f);
}

// ---------------------------------------------------------------- settings
static const int kLowLevels[] = {0, 10, 15, 20, 25};

static bool HangarRunning() { return FindWindowW(L"Hangar.larosa023.Main", nullptr) != nullptr; }
// Hangar writes settings.json the first time it runs (Halo itself only ever creates Hangar's run\ folder)
static bool HangarInstalled() {
    return FileExists(HangarDataDir() + L"\\settings.json") || FileExists(KnownFolder(FOLDERID_Programs) + L"\\Hangar.lnk");
}

static void DrawSettings(float x0) {
    PageFrame f = BeginPage(x0, L"Settings", L"", 820);

    D2D1_RECT_F s1 = Section(f, L"WINDOW AND TRAY", 64 * 5);
    RowLabel(s1, s1.top + 4, L"Close button", L"Halo keeps battery alerts and the tray menu going when hidden");
    Segmented(s1.right - 18 - SegmentedWidth({L"Hide to tray", L"Quit"}), s1.top + 16, {L"Hide to tray", L"Quit"}, g_cfg.closeToTray ? 0 : 1, Hit::SetClose);
    Divider(s1, s1.top + 64);
    RowLabel(s1, s1.top + 68, L"Start with Windows", L"Starts hidden in the tray");
    Segmented(s1.right - 18 - SegmentedWidth({L"On", L"Off"}), s1.top + 80, {L"On", L"Off"}, g_cfg.startWithWindows ? 0 : 1, Hit::SetStartup);
    Divider(s1, s1.top + 128);
    RowLabel(s1, s1.top + 132, L"Tray icon", L"The battery level as a number, or as a ring");
    Segmented(s1.right - 18 - SegmentedWidth({L"Number", L"Ring"}), s1.top + 144, {L"Number", L"Ring"}, g_cfg.trayPercent ? 0 : 1, Hit::SetTrayPct);
    Divider(s1, s1.top + 192);
    RowLabel(s1, s1.top + 196, L"Hotkeys", g_hotkeyError.empty() ? L"Ctrl+Alt+Up / Down: next DPI preset · Ctrl+Alt+P: next profile" : g_hotkeyError);
    Segmented(s1.right - 18 - SegmentedWidth({L"On", L"Off"}), s1.top + 208, {L"On", L"Off"}, g_cfg.hotkeys ? 0 : 1, Hit::SetHotkeys);
    Divider(s1, s1.top + 256);
    RowLabel(s1, s1.top + 260, L"On-screen display", L"A small popup when DPI or the profile changes outside Halo");
    Segmented(s1.right - 18 - SegmentedWidth({L"On", L"Off"}), s1.top + 272, {L"On", L"Off"}, g_cfg.osd ? 0 : 1, Hit::SetOsd);

    D2D1_RECT_F s2 = Section(f, L"BATTERY", 64);
    std::vector<std::wstring> lows = {L"Off", L"10%", L"15%", L"20%", L"25%"};
    int li = 0;
    for (int k = 0; k < 5; ++k) if (kLowLevels[k] == g_cfg.lowBattery) li = k;
    RowLabel(s2, s2.top + 4, L"Low battery alert", L"A Windows notification when the mouse needs charging", SegmentedWidth(lows) + 40);
    Segmented(s2.right - 18 - SegmentedWidth(lows), s2.top + 16, lows, li, Hit::SetLow);

    // updates
    auto prev = selfupdate::PreviousVersions();
    D2D1_RECT_F s3 = Section(f, L"UPDATES", 64 * 3 + (prev.empty() ? 0 : 64));
    {
        std::wstring status;
        switch (g_self.kind) {
            case SelfUpdateState::Checking: status = L"Checking…"; break;
            case SelfUpdateState::UpToDate: status = L"Up to date"; break;
            case SelfUpdateState::Downloading: status = L"Downloading " + g_self.version + (g_self.percent >= 0 ? L" · " + std::to_wstring(g_self.percent) + L"%" : L""); break;
            case SelfUpdateState::Ready: status = L"Halo " + g_self.version + L" is downloaded and verified"; break;
            case SelfUpdateState::Error: status = g_self.message; break;
            default: status = L"Checks GitHub every few hours"; break;
        }
        RowLabel(s3, s3.top + 4, std::wstring(L"Halo ") + HALO_VERSION, status, 200);
        if (g_self.kind == SelfUpdateState::Ready)
            Button(D2D1::RectF(s3.right - 170, s3.top + 16, s3.right - 18, s3.top + 48), L"Restart to update", icon::Update, Btn::Primary, Hit::SelfRestart);
        else
            Button(D2D1::RectF(s3.right - 150, s3.top + 16, s3.right - 18, s3.top + 48), L"Check now", icon::Refresh, Btn::Secondary, Hit::SelfCheck, -1, 0,
                   g_self.kind != SelfUpdateState::Checking && g_self.kind != SelfUpdateState::Downloading);
        Divider(s3, s3.top + 64);
        RowLabel(s3, s3.top + 68, L"Channel", L"Beta gets new features first");
        Segmented(s3.right - 18 - SegmentedWidth({L"Stable", L"Beta"}), s3.top + 80, {L"Stable", L"Beta"}, g_cfg.beta ? 1 : 0, Hit::SelfChannel);
        Divider(s3, s3.top + 128);
        RowLabel(s3, s3.top + 132, L"Install updates automatically", L"Only while Halo is in the tray or you're away from the PC");
        Segmented(s3.right - 18 - SegmentedWidth({L"On", L"Off"}), s3.top + 144, {L"On", L"Off"}, g_cfg.autoUpdate ? 0 : 1, Hit::SelfAuto);
        if (!prev.empty()) {
            Divider(s3, s3.top + 192);
            RowLabel(s3, s3.top + 196, L"Previous versions", L"Go back if a new version gives you trouble");
            float bx = s3.right - 18;
            for (size_t k = 0; k < prev.size() && k < 2; ++k) {
                std::wstring l = L"Roll back to " + prev[k].version;
                float w = ButtonWidth(l, 0);
                Button(D2D1::RectF(bx - w, s3.top + 208, bx, s3.top + 240), l, 0, Btn::Secondary, Hit::SelfRollback, -1, (int)k);
                bx -= w + 8;
            }
        }
    }

    // Hangar
    bool installedHere = install::RunningFromInstallDir();
    D2D1_RECT_F s4 = Section(f, L"HANGAR", 64 * 2);
    {
        bool running = HangarRunning(), installed = HangarInstalled();
        RowLabel(s4, s4.top + 4, running ? L"Hangar is running" : installed ? L"Hangar is installed" : L"Hangar isn't installed",
                 running || installed ? L"It can update Halo in place, theme it and back up its settings" : L"Hangar installs and updates your whole suite of apps", 200);
        if (!installed) Button(D2D1::RectF(s4.right - 150, s4.top + 16, s4.right - 18, s4.top + 48), L"Get Hangar", icon::Globe, Btn::Secondary, Hit::SetHangar);
        else Pill(s4.right - 18, s4.top + 22, running ? L"Connected" : L"Ready", running ? g_t.good : g_t.info, true);
        Divider(s4, s4.top + 64);
        if (IsPortable()) RowLabel(s4, s4.top + 68, L"Portable copy", L"Settings are kept in the data folder next to HaloMouse.exe", 200);
        else if (installedHere) RowLabel(s4, s4.top + 68, L"Installed for this user", install::InstallDir(), 200);
        else {
            RowLabel(s4, s4.top + 68, L"Install Halo", L"Adds Halo to Start and to Apps & features, where Hangar looks for it", 200);
            Button(D2D1::RectF(s4.right - 150, s4.top + 80, s4.right - 18, s4.top + 112), L"Install", icon::Download, Btn::Secondary, Hit::SetInstall);
        }
    }

    D2D1_RECT_F s5 = Section(f, L"ADVANCED", 64 * 4);
    RowLabel(s5, s5.top + 4, L"Data folder", DataDir());
    Button(D2D1::RectF(s5.right - 140, s5.top + 16, s5.right - 18, s5.top + 48), L"Open", icon::Folder, Btn::Secondary, Hit::SetOpenData);
    Divider(s5, s5.top + 64);
    RowLabel(s5, s5.top + 68, L"Log", L"halo.log, for troubleshooting");
    Button(D2D1::RectF(s5.right - 140, s5.top + 80, s5.right - 18, s5.top + 112), L"Open log", 0, Btn::Secondary, Hit::SetOpenLog);
    Divider(s5, s5.top + 128);
    RowLabel(s5, s5.top + 132, L"Animations", L"Smooth runs at your monitor's refresh rate; nothing is drawn while Halo is idle");
    Segmented(s5.right - 18 - SegmentedWidth({L"Smooth", L"Reduced"}), s5.top + 144, {L"Smooth", L"Reduced"}, g_cfg.reduceMotion ? 1 : 0, Hit::SetMotion);
    Divider(s5, s5.top + 192);
    RowLabel(s5, s5.top + 196, L"Frame rate counter", L"Shows the frame rate in the corner while something moves");
    Segmented(s5.right - 18 - SegmentedWidth({L"On", L"Off"}), s5.top + 208, {L"On", L"Off"}, g_cfg.showFps ? 0 : 1, Hit::SetFps);

    D2D1_RECT_F s6 = Section(f, L"ABOUT", 64);
    RowLabel(s6, s6.top + 4, std::wstring(L"Halo ") + HALO_VERSION + (IsPortable() ? L" (portable)" : L""), L"Not made by or affiliated with Finalmouse");
    Button(D2D1::RectF(s6.right - 230, s6.top + 16, s6.right - 18, s6.top + 48), L"github.com/larosa023/Halo", 0, Btn::Subtle, Hit::SetRepo);
    EndPage(f);
}

// ---------------------------------------------------------------- toast, scrollbar, paint
static void DrawToast() {
    if (g_toastAnim <= 0.01f || g_toast.empty()) return;
    auto* f = Format(13.5f);
    float w = std::min(g_W - kSidebar - 60, MeasureWidth(g_toast, f) + 70);
    float cx = kSidebar + (g_W - kSidebar) / 2;
    float y = g_H - 20 - 44 * g_toastAnim;
    D2D1_RECT_F r = D2D1::RectF(cx - w / 2, y, cx + w / 2, y + 40);
    g_c.FillRound(r, 8, Alpha(g_t.dark ? Rgb(0x2E2E36) : Rgb(0x26262C), g_toastAnim));
    g_c.Glyph(g_toastBad ? icon::Warning : icon::Check, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true),
              D2D1::RectF(r.left + 8, r.top, r.left + 36, r.bottom), Alpha(g_toastBad ? Rgb(0xFF8A8A) : Rgb(0x7BE0A6), g_toastAnim));
    g_c.Text(g_toast, f, D2D1::RectF(r.left + 38, r.top, r.right - 30, r.bottom), Alpha(Rgb(0xF4F4F6), g_toastAnim));
    g_c.Glyph(icon::Cancel, Format(9, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true),
              D2D1::RectF(r.right - 30, r.top + 8, r.right - 6, r.bottom - 8), Alpha(Rgb(0xB0B0BA), g_toastAnim));
    AddHot(r, Hit::ToastClose);
}

static void ClampScroll() {
    float maxS = std::max(0.0f, g_contentH - g_viewH);
    g_scrollTarget = std::clamp(g_scrollTarget, 0.0f, maxS);
    g_scroll = std::clamp(g_scroll, 0.0f, maxS);
}

static void DrawScrollbar() {
    if (g_contentH <= g_viewH + 1) return;
    float trackH = g_viewH - 8;
    float thumbH = std::max(40.0f, trackH * g_viewH / g_contentH);
    float t = g_scroll / (g_contentH - g_viewH);
    float y = 4 + (trackH - thumbH) * t;
    g_c.FillRound(D2D1::RectF(g_W - 8, y, g_W - 4, y + thumbH), 2, g_t.dark ? Rgb(0xFFFFFF, 0.18f) : Rgb(0x000000, 0.2f));
}

static void Animate() {
    if (Reduced()) g_scroll = g_scrollTarget;
    else if (std::fabs(g_scrollTarget - g_scroll) > 0.4f) { g_scroll += (g_scrollTarget - g_scroll) * (1.0f - std::exp(-16.0f * g_dt)); g_frameMore = true; }
    else g_scroll = g_scrollTarget;
    int pct = g_ms.BatteryPercent();
    Ease(g_ring, pct >= 0 ? pct / 100.0f : 0.0f, 5.0f, 0.0015f);
    if (g_pageAnim < 1.0f) {
        g_pageAnim = Reduced() ? 1.0f : std::min(1.0f, g_pageAnim + g_dt / 0.26f);
        if (g_pageAnim < 1.0f) g_frameMore = true;
    }
    float toastTarget = (!g_toast.empty() && g_toastAt) ? 1.0f : 0.0f;
    Ease(g_toastAnim, toastTarget, 14, 0.01f);
    if (toastTarget == 0 && g_toastAnim == 0) g_toast.clear();
    uint64_t hk = g_hover.kind != Hit::None && g_hover.enabled ? HKey(g_hover.kind, g_hover.index, g_hover.arg) : 0;
    if (hk && !g_hoverAmt.count(hk)) g_hoverAmt[hk] = 0;
    for (auto it = g_hoverAmt.begin(); it != g_hoverAmt.end();) {
        float t = it->first == hk ? 1.0f : 0.0f;
        Ease(it->second, t, 16, 0.01f);
        if (it->second == 0 && t == 0) it = g_hoverAmt.erase(it);
        else ++it;
    }
}

static void DrawFps(bool continuous) {
    if (!g_cfg.showFps) return;
    std::wstring t = continuous && g_fps > 0 ? std::to_wstring((int)std::lround(g_fps)) + L" fps" : L"idle · 0 fps";
    auto* f = Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    float w = MeasureWidth(t, f) + 16;
    D2D1_RECT_F r = D2D1::RectF(g_W - w - 12, 10, g_W - 12, 30);
    g_c.FillRound(r, 10, Alpha(g_t.surfaceStrong, 0.9f));
    g_c.Text(t, f, r, g_t.subtext);
}

static void Paint() {
    if (!EnsureDevice()) return;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double gap = g_lastFrameQpc.QuadPart ? double(now.QuadPart - g_lastFrameQpc.QuadPart) / g_qpf.QuadPart : 1.0;
    bool continuous = gap < 0.1;                     // the next frame of a running animation
    g_dt = continuous ? (float)std::clamp(gap, 0.001, 0.05) : 1.0f / 60;
    g_lastFrameQpc = now;
    if (!continuous) { g_fpsFrames = 0; g_fpsSince = now; }
    else if (++g_fpsFrames >= 10) {
        double span = double(now.QuadPart - g_fpsSince.QuadPart) / g_qpf.QuadPart;
        if (span > 0) g_fps = (float)(g_fpsFrames / span);
        g_fpsFrames = 0;
        g_fpsSince = now;
    }
    g_frameMore = false;
    g_reducedNow = ReducedSetting();
    Animate();

    D2D1_SIZE_F sz = g_rt->GetSize();
    g_W = sz.width;
    g_H = sz.height;
    g_c.rt = g_rt;
    g_c.br = g_br;
    g_hot.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(g_t.bg);
    float x0 = kSidebar;
    float e = EaseOutCubic(g_pageAnim);
    float slide = (1.0f - e) * 16;
    if (slide > 0.05f) g_rt->SetTransform(D2D1::Matrix3x2F::Translation(0, slide));
    switch (g_page) {
        case Page::Overview: DrawOverview(x0); break;
        case Page::Performance: DrawPerformance(x0); break;
        case Page::Extras: DrawExtras(x0); break;
        case Page::Profiles: DrawProfiles(x0); break;
        case Page::Themes: DrawThemes(x0); break;
        case Page::Settings: DrawSettings(x0); break;
    }
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (e < 0.999f) { D2D1_COLOR_F o = g_t.bg; o.a = (1.0f - e) * 0.9f; g_c.Fill(D2D1::RectF(x0, 0, g_W, g_H), o); }
    DrawScrollbar();
    DrawSidebar();
    DrawToast();
    DrawFps(continuous);
    HRESULT hr = g_rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { DiscardDevice(); g_frameMore = true; }
    ClampScroll();

    if (g_frameMore) {
        if (g_rt && (g_rt->CheckWindowState() & D2D1_WINDOW_STATE_OCCLUDED)) {
            SetTimer(g_hwnd, TIMER_ANIM, 50, nullptr);   // covered: no point drawing at full speed
        } else {
            DwmFlush();                                   // wait for the compositor's next frame (= the display's refresh)
            InvalidateRect(g_hwnd, nullptr, FALSE);
        }
    }
}

// ---------------------------------------------------------------- actions
static void EndEdit(bool commit);

static void Navigate(Page p) {
    if (g_edit.active) EndEdit(false);
    if (g_page == p) return;
    StopPollTest();
    g_page = p;
    g_cfg.page = kPageIds[(int)p];
    g_scroll = g_scrollTarget = 0;
    g_pageAnim = 0.0f;
    StartAnim();
}

static void BeginEdit(Hit kind, int index, const std::wstring& text) {
    g_edit.active = true;
    g_edit.kind = kind;
    g_edit.index = index;
    g_edit.text = text;
    g_caretOn = true;
    SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    StartAnim();
}

static void EndEdit(bool commit) {
    if (!g_edit.active) return;
    Edit e = g_edit;
    g_edit = Edit{};
    KillTimer(g_hwnd, TIMER_CARET);
    if (commit) {
        if (e.kind == Hit::DpiField) {
            int v = _wtoi(e.text.c_str());
            if (v < ulx::kDpiMin || v > ulx::kDpiMax) Toast(L"DPI has to be between 50 and 26,000.", true);
            else if (v != g_ms.t.dpi) mouse::SetDpi(v);
        } else if (e.kind == Hit::ProfileName && e.index >= 0 && e.index < (int)g_cfg.profiles.size()) {
            std::wstring n = Trim(e.text);
            if (!n.empty()) { g_cfg.profiles[e.index].name = n; ConfigSave(); }
        }
    }
    Invalidate();
}

static void StepDpi(int dir) {
    if (g_ms.t.dpi <= 0) return;
    int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 50;
    int v = g_ms.t.dpi;
    v = dir > 0 ? (v / step + 1) * step : ((v + step - 1) / step - 1) * step;
    mouse::SetDpi(std::clamp(v, ulx::kDpiMin, ulx::kDpiMax));
}

static void ShowMain() {
    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_SHOW);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    mouse::SetPollSeconds(10);
    Invalidate();
}

static void HideToTray() {
    if (g_edit.active) EndEdit(false);
    StopPollTest();
    ShowWindow(g_hwnd, SW_HIDE);
    mouse::SetPollSeconds(60);
    if (!g_cfg.trayHintShown) {
        g_cfg.trayHintShown = true;
        ConfigSave();
        tray::Notify(L"Halo is still running", L"It's in the notification area, keeping an eye on the battery. Change this in Settings.", false);
    }
}

static void SaveWindowPlacement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    RECT r = wp.rcNormalPosition;
    g_cfg.windowX = r.left;
    g_cfg.windowY = r.top;
    g_cfg.windowW = r.right - r.left;
    g_cfg.windowH = r.bottom - r.top;
}

static void Quit() {
    g_quitting = true;
    if (g_hwnd) {
        SaveWindowPlacement();
        ConfigSave();
        DestroyWindow(g_hwnd);
    }
}

// Where you are, for a restart into another version (Halo's updater or Hangar's live update)
static std::wstring RestoreJson() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"page", json::Value::Str(kPageIds[(int)g_page]));
    v.Set(L"scroll", json::Value::Num(g_scrollTarget));
    v.Set(L"hidden", json::Value::Boolean(!IsWindowVisible(g_hwnd)));
    v.Set(L"minimized", json::Value::Boolean(IsIconic(g_hwnd) != 0));
    v.Set(L"background", json::Value::Boolean(GetForegroundWindow() != g_hwnd));
    std::wstring out;
    json::Write(v, out);
    return out;
}

static Page PageFromId(const std::wstring& id) {
    for (int k = 0; k < 6; ++k) if (id == kPageIds[k]) return (Page)k;
    return Page::Overview;
}

static int ApplyRestore(const std::wstring& text, std::wstring* summary) {
    json::Value v;
    int show = SW_SHOWNORMAL;
    if (text.empty() || !json::Parser(text).Parse(v) || v.type != json::Value::Object) return show;
    g_page = PageFromId(v.GetStr(L"page"));
    g_scroll = g_scrollTarget = (float)v.GetNum(L"scroll", 0);
    if (v.GetBool(L"hidden", false)) show = SW_HIDE;
    else if (v.GetBool(L"minimized", false)) show = SW_SHOWMINNOACTIVE;
    else if (v.GetBool(L"background", false)) show = SW_SHOWNOACTIVATE;
    if (summary) {
        static const wchar_t* names[] = {L"Overview", L"Performance", L"Scroll & dongle", L"Profiles", L"Themes", L"Settings"};
        *summary = show == SW_HIDE ? L"back in the tray" : std::wstring(L"opened on ") + names[(int)g_page];
    }
    return show;
}

static void ApplySelf(const std::wstring& exe, const std::wstring& version, bool userAsked) {
    // Hangar is in the middle of its own update of Halo (a prepare-update older than 2 minutes was abandoned)
    if (g_preparingUpdate && GetTickCount() - g_preparingTick < 2 * 60 * 1000) return;
    g_preparingUpdate = false;
    g_applyWhenIdle = false;
    SaveWindowPlacement();
    ConfigSave();
    selfupdate::SaveRestoreState(RestoreJson());
    Log(L"self-update: restarting into %ls", version.c_str());
    std::wstring err;
    bool wasVisible = IsWindowVisible(g_hwnd) != 0;
    tray::Remove();
    if (selfupdate::Apply(g_hwnd, exe, version, &err)) {
        g_quitting = true;
        DestroyWindow(g_hwnd);
        return;
    }
    tray::Create(g_hwnd);
    tray::Update(g_ms, g_cfg.trayPercent, TrayExtra());
    if (!wasVisible) ShowWindow(g_hwnd, SW_HIDE);
    DeleteFileW((DataDir() + L"\\restore.json").c_str());
    if (IStartsWith(err, L"Halo " + version + L" didn't start")) {
        SkipVersion(version);
        ConfigSave();
        g_self = SelfUpdateState{};
        g_self.kind = SelfUpdateState::Error;
        g_self.message = err;
    }
    if (userAsked || wasVisible) Toast(err, true);
    Log(L"self-update: %ls", err.c_str());
}

static void MaybeAutoApplySelf() {
    if (g_self.kind != SelfUpdateState::Ready || g_edit.active) return;
    if (!g_cfg.autoUpdate && !g_applyWhenIdle) return;
    LASTINPUTINFO li{sizeof(li)};
    bool idle = GetLastInputInfo(&li) && GetTickCount() - li.dwTime > 2 * 60 * 1000;
    bool away = !IsWindowVisible(g_hwnd) || IsIconic(g_hwnd) || idle;
    if (g_applyWhenIdle || away) ApplySelf(g_self.staged, g_self.version, false);
}

static void CheckSelf(bool force) {
    g_selfCheckedTick = GetTickCount();
    selfupdate::Check(g_hwnd, force);
}

static void Click(const HotRect& h, bool right) {
    if (!h.enabled) return;
    if (g_edit.active && !(h.kind == g_edit.kind && h.index == g_edit.index)) EndEdit(true);
    if (right) {
        if (h.kind == Hit::DpiPreset && h.index >= 0 && h.index < (int)g_cfg.dpiPresets.size()) {
            int v = g_cfg.dpiPresets[h.index];
            g_cfg.dpiPresets.erase(g_cfg.dpiPresets.begin() + h.index);
            ConfigSave();
            Toast(L"Removed the " + Thousands(v) + L" preset.");
        }
        StartAnim();
        return;
    }
    switch (h.kind) {
        case Hit::Nav: Navigate((Page)h.arg); break;
        case Hit::GoPage: if ((Page)h.index != Page::Overview) Navigate((Page)h.index); break;
        case Hit::ToastClose: g_toastAt = 0; break;
        case Hit::Retry: mouse::Rescan(); Toast(L"Looking for the dongle…"); break;
        case Hit::SetRefresh: mouse::Refresh(); Toast(L"Reading the mouse's settings again."); break;
        case Hit::DpiPreset: if (h.index >= 0 && h.index < (int)g_cfg.dpiPresets.size()) mouse::SetDpi(g_cfg.dpiPresets[h.index]); break;
        case Hit::DpiPresetAdd:
            if (g_ms.t.dpi > 0) {
                g_cfg.dpiPresets.push_back(g_ms.t.dpi);
                std::sort(g_cfg.dpiPresets.begin(), g_cfg.dpiPresets.end());
                g_cfg.dpiPresets.erase(std::unique(g_cfg.dpiPresets.begin(), g_cfg.dpiPresets.end()), g_cfg.dpiPresets.end());
                ConfigSave();
            }
            break;
        case Hit::DpiStep: StepDpi(h.arg); break;
        case Hit::DpiField: if (!g_edit.active) BeginEdit(Hit::DpiField, -1, g_ms.t.dpi > 0 ? std::to_wstring(g_ms.t.dpi) : L""); break;
        case Hit::PollTest: if (g_pt.active) StopPollTest(); else StartPollTest(); break;
        case Hit::Poll: mouse::SetPollingRate(ulx::kPollingRates[h.arg]); break;
        case Hit::MotionSync: mouse::SetMotionSync(h.arg == 0); break;
        case Hit::LiftOff: mouse::SetLiftOff(h.arg + 1); break;
        case Hit::ScrollMode: mouse::SetScrollMode(h.arg); if (h.arg > 0 && g_ms.t.scrollTimeoutMs <= 0) mouse::SetScrollTimeout(500); break;
        case Hit::ScrollTimeout: mouse::SetScrollTimeout(ulx::kScrollTimeouts[h.arg]); break;
        case Hit::DongleLed: mouse::SetDongleLed(h.arg); break;
        case Hit::ProfileSave: {
            int n = (int)g_cfg.profiles.size() + 1;
            std::wstring name;
            for (;; ++n) {
                name = L"Profile " + std::to_wstring(n);
                bool used = false;
                for (auto& p : g_cfg.profiles) used |= p.name == name;
                if (!used) break;
            }
            g_cfg.profiles.push_back(CurrentAsProfile(name));
            ConfigSave();
            if (g_page != Page::Profiles) Navigate(Page::Profiles);
            BeginEdit(Hit::ProfileName, (int)g_cfg.profiles.size() - 1, name);
            break;
        }
        case Hit::ProfileApply: if (h.index >= 0 && h.index < (int)g_cfg.profiles.size()) ApplyProfile(g_cfg.profiles[h.index], From::Window); break;
        case Hit::ProfileDelete:
            if (h.index >= 0 && h.index < (int)g_cfg.profiles.size()) {
                std::wstring msg = L"Delete the profile \"" + g_cfg.profiles[h.index].name + L"\"?\n\nThe mouse keeps its current settings.";
                if (MessageBoxW(g_hwnd, msg.c_str(), L"Delete profile", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                    g_cfg.profiles.erase(g_cfg.profiles.begin() + h.index);
                    if (g_autoActive == h.index) g_autoActive = -1;
                    else if (g_autoActive > h.index) --g_autoActive;
                    ConfigSave();
                }
            }
            break;
        case Hit::ProfileAddApp: AddAppToProfile(h.index); break;
        case Hit::ProfileRemoveApp:
            if (h.index >= 0 && h.index < (int)g_cfg.profiles.size() && h.arg >= 0 && h.arg < (int)g_cfg.profiles[h.index].apps.size()) {
                auto& apps = g_cfg.profiles[h.index].apps;
                apps.erase(apps.begin() + h.arg);
                ConfigSave();
            }
            break;
        case Hit::SetAuto:
            g_cfg.autoSwitch = h.arg == 0;
            ConfigSave();
            if (!g_cfg.autoSwitch && g_autoActive >= 0) { g_autoActive = -1; SendProfile(g_baseline); }
            break;
        case Hit::SetHotkeys: g_cfg.hotkeys = h.arg == 0; ConfigSave(); RegisterHotkeys(); break;
        case Hit::SetOsd:
            g_cfg.osd = h.arg == 0;
            ConfigSave();
            if (g_cfg.osd) osd::Show(L"On-screen display", L"Shown when you change DPI or profiles outside Halo", 0.6f, g_t.accent);
            break;
        case Hit::ProfileName:
            if (!g_edit.active && h.index >= 0 && h.index < (int)g_cfg.profiles.size()) BeginEdit(Hit::ProfileName, h.index, g_cfg.profiles[h.index].name);
            break;
        case Hit::ThemePick: {
            auto all = GalleryThemes();
            if (h.arg >= 0 && h.arg < (int)all.size()) { g_cfg.theme = all[h.arg].id; ConfigSave(); ApplyTheme(); }
            break;
        }
        case Hit::AccentPick:
            if (h.arg == 0) g_cfg.accent = L"theme";
            else if (h.arg == 1) g_cfg.accent = L"windows";
            else if (h.arg >= 2 && h.arg < 2 + kAccentSwatchCount) g_cfg.accent = HexColor(kAccentSwatches[h.arg - 2]);
            else if (!PickCustomAccent()) break;
            ConfigSave();
            ApplyTheme();
            break;
        case Hit::ThemeExport: ExportTheme(); break;
        case Hit::ThemeFolder:
            EnsureDir(ThemesDir());
            ShellExecuteW(g_hwnd, L"open", ThemesDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case Hit::ThemeReload:
            g_customThemes = LoadCustomThemes();
            ApplyTheme();
            Toast(std::to_wstring(g_customThemes.size()) + (g_customThemes.size() == 1 ? L" theme file loaded." : L" theme files loaded."));
            break;
        case Hit::SetClose: g_cfg.closeToTray = h.arg == 0; ConfigSave(); break;
        case Hit::SetStartup: g_cfg.startWithWindows = h.arg == 0; ApplyStartup(g_cfg.startWithWindows); ConfigSave(); break;
        case Hit::SetTrayPct: g_cfg.trayPercent = h.arg == 0; ConfigSave(); tray::Update(g_ms, g_cfg.trayPercent, TrayExtra()); break;
        case Hit::SetMotion: g_cfg.reduceMotion = h.arg == 1; ConfigSave(); break;
        case Hit::SetFps: g_cfg.showFps = h.arg == 0; ConfigSave(); break;
        case Hit::SetLow: g_cfg.lowBattery = kLowLevels[h.arg]; g_lowAlerted = false; ConfigSave(); break;
        case Hit::SetOpenData: ShellExecuteW(g_hwnd, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case Hit::SetOpenLog: ShellExecuteW(g_hwnd, L"open", LogPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case Hit::SetRepo: OpenUrl(L"https://github.com/larosa023/Halo"); break;
        case Hit::SetHangar: OpenUrl(L"https://github.com/larosa023/Hangar/releases/latest"); break;
        case Hit::SetInstall: {
            std::wstring err;
            if (!install::InstallSelf(&err)) { Toast(err, true); break; }
            std::wstring target = install::InstallDir() + L"\\" HALO_EXE_NAME;
            if (g_cfg.startWithWindows) {
                // the Run key should point at the installed copy
                HKEY run;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &run) == ERROR_SUCCESS) {
                    std::wstring cmd = L"\"" + target + L"\" --tray";
                    RegSetValueExW(run, L"Halo", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
                    RegCloseKey(run);
                }
            }
            if (MessageBoxW(g_hwnd, L"Halo is installed. Switch to the installed copy now?\n\nYou can delete this one afterwards.",
                            L"Halo", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                SaveWindowPlacement();
                ConfigSave();
                tray::Remove();
                ipc::Stop();
                g_quitting = true;
                DestroyWindow(g_hwnd);
                LaunchDetached(target, L"--after-install");
            }
            break;
        }
        case Hit::SelfRestart: if (g_self.kind == SelfUpdateState::Ready) ApplySelf(g_self.staged, g_self.version, true); break;
        case Hit::SelfCheck: CheckSelf(true); break;
        case Hit::SelfChannel: g_cfg.beta = h.arg == 1; ConfigSave(); CheckSelf(true); break;
        case Hit::SelfAuto: g_cfg.autoUpdate = h.arg == 0; ConfigSave(); break;
        case Hit::SelfRollback: {
            auto prev = selfupdate::PreviousVersions();
            if (h.arg < 0 || h.arg >= (int)prev.size()) break;
            std::wstring msg = L"Go back to Halo " + prev[h.arg].version + L"?\n\nHalo restarts and won't install " + HALO_VERSION +
                               L" again automatically. Your mouse and settings aren't touched.";
            if (MessageBoxW(g_hwnd, msg.c_str(), L"Roll back Halo", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) break;
            SkipVersion(HALO_VERSION);
            ApplySelf(prev[h.arg].exe, prev[h.arg].version, true);
            break;
        }
        default: break;
    }
    StartAnim();
}

static HotRect HitAt(float x, float y) {
    for (auto it = g_hot.rbegin(); it != g_hot.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return *it;
    return {{0, 0, 0, 0}, Hit::None, -1, 0, false};
}

static void Scroll(float delta) {
    g_scrollTarget -= delta;
    ClampScroll();
    StartAnim();
}

static void OnKey(WPARAM vk) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (g_edit.active) {
        if (vk == VK_RETURN) EndEdit(true);
        else if (vk == VK_ESCAPE) EndEdit(false);
        else if (vk == VK_UP && g_edit.kind == Hit::DpiField) { EndEdit(false); StepDpi(1); }
        else if (vk == VK_DOWN && g_edit.kind == Hit::DpiField) { EndEdit(false); StepDpi(-1); }
        return;
    }
    if (ctrl && vk >= '1' && vk <= '6') { Navigate((Page)(vk - '1')); return; }
    switch (vk) {
        case VK_F5: mouse::Refresh(); Toast(L"Reading the mouse's settings again."); break;
        case VK_PRIOR: Scroll(g_viewH * 0.8f); break;
        case VK_NEXT: Scroll(-g_viewH * 0.8f); break;
        case VK_HOME: g_scrollTarget = 0; StartAnim(); break;
        case VK_END: g_scrollTarget = g_contentH; ClampScroll(); StartAnim(); break;
        case VK_ESCAPE: if (g_cfg.closeToTray) HideToTray(); break;
        case VK_OEM_PLUS: case VK_ADD: if (CanWrite()) StepDpi(1); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: if (CanWrite()) StepDpi(-1); break;
    }
}

static void OnChar(wchar_t c) {
    if (!g_edit.active) return;
    if (c == 8) { if (!g_edit.text.empty()) g_edit.text.pop_back(); }
    else if (c == 1 /*Ctrl+A*/ || c == 127) g_edit.text.clear();
    else if (g_edit.kind == Hit::DpiField) { if (c >= L'0' && c <= L'9' && g_edit.text.size() < 5) g_edit.text += c; }
    else if (c >= 32 && g_edit.text.size() < 40) g_edit.text += c;
    g_caretOn = true;
    Invalidate();
}

// ---------------------------------------------------------------- mouse updates
static void OnMouse() {
    mouse::State prev = g_ms;
    g_ms = mouse::Snapshot();
    RecordBattery();
    tray::Update(g_ms, g_cfg.trayPercent, TrayExtra());
    if (g_ms.Linked() && g_ms.readComplete && !(prev.Linked() && prev.readComplete)) SetTimer(g_hwnd, TIMER_FOREGROUND, 800, nullptr);
    // battery alert, once per discharge
    int pct = g_ms.BatteryPercent();
    if (g_ms.Linked() && g_ms.t.charging == 1) g_lowAlerted = false;
    if (g_ms.Linked() && pct > g_cfg.lowBattery + 5) g_lowAlerted = false;
    if (g_cfg.lowBattery > 0 && g_ms.Linked() && g_ms.t.charging == 0 && pct >= 0 && pct <= g_cfg.lowBattery && !g_lowAlerted) {
        g_lowAlerted = true;
        tray::Notify(L"Mouse battery low", L"Your UltralightX is at " + std::to_wstring(pct) + L"%. Plug in its cable to charge it.", true);
    }
    if (prev.conn != mouse::State::Connected && g_ms.conn == mouse::State::Connected && prev.conn != mouse::State::Searching && Visible())
        Toast(L"Dongle connected.");
    ipc::SetBusy(false, L"");
    StartAnim();
}

// ---------------------------------------------------------------- Hangar requests
static void OnIpc(ipc::Request& r) {
    json::Value& rep = r.reply;
    if (r.type == L"prepare-update") {
        if (g_edit.active) {
            rep.Set(L"ok", json::Value::Boolean(false));
            rep.Set(L"reason", json::Value::Str(L"You're typing in Halo"));
        } else {
            g_preparingUpdate = true;
            g_preparingTick = GetTickCount();
            SaveWindowPlacement();
            ConfigSave();
            std::wstring file = DataDir() + L"\\state\\hangar-restore.json";
            EnsureDir(PathParent(file));
            WriteTextFileAtomic(file, RestoreJson());
            rep.Set(L"ok", json::Value::Boolean(true));
            rep.Set(L"stateFile", json::Value::Str(file));
        }
    } else if (r.type == L"cancel-update") {
        g_preparingUpdate = false;
        rep.Set(L"ok", json::Value::Boolean(true));
    } else if (r.type == L"restart") {
        rep.Set(L"ok", json::Value::Boolean(true));
        r.Done();
        PostMessageW(g_hwnd, WM_HALO_QUIT, 0, 0);
        return;
    } else if (r.type == L"apply-theme") {
        const json::Value* th = r.msg.Get(L"theme");
        ThemeDef def;
        std::wstring text, err;
        if (th && th->type == json::Value::Object) json::Write(*th, text);
        if (!text.empty() && ParseThemeJson(text, def, &err)) {
            def.custom = false;
            g_hangarLive = def;
            g_hasHangarLive = true;
            if (MatchingHangar()) ApplyTheme();
            rep.Set(L"ok", json::Value::Boolean(true));
            rep.Set(L"applied", json::Value::Boolean(MatchingHangar()));
        } else {
            rep.Set(L"ok", json::Value::Boolean(false));
            rep.Set(L"reason", json::Value::Str(err.empty() ? L"no theme" : err));
        }
    } else if (r.type == L"apply-settings") {
        ConfigLoad();
        ApplyStartup(g_cfg.startWithWindows);
        g_customThemes = LoadCustomThemes();
        ApplyTheme();
        tray::Update(g_ms, g_cfg.trayPercent, TrayExtra());
        rep.Set(L"ok", json::Value::Boolean(true));
        rep.Set(L"restartRequired", json::Value::Boolean(false));
    } else if (r.type == L"export-settings") {
        ConfigSave();
        std::wstring file = DataDir() + L"\\export\\halo-settings.json", text;
        EnsureDir(PathParent(file));
        ReadTextFile(ConfigPath(), text);
        bool ok = WriteTextFileAtomic(file, text);
        rep.Set(L"ok", json::Value::Boolean(ok));
        if (ok) rep.Set(L"file", json::Value::Str(file));
    } else {
        rep.Set(L"ok", json::Value::Boolean(false));
    }
    r.Done();
    Invalidate();
}

// ---------------------------------------------------------------- window
static void RegisterDeviceNotifications(HWND hwnd) {
    DEV_BROADCAST_DEVICEINTERFACE_W f{};
    f.dbcc_size = sizeof(f);
    f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    // GUID_DEVINTERFACE_HID
    f.dbcc_classguid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    g_devNotify = RegisterDeviceNotificationW(hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) { tray::Recreate(); tray::Update(g_ms, g_cfg.trayPercent, TrayExtra()); return 0; }
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            g_scale = GetDpiForWindow(hwnd) / 96.0f;
            g_customThemes = LoadCustomThemes();
            ApplyTheme();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            if (wp == SIZE_MINIMIZED) mouse::SetPollSeconds(60);
            else if (IsWindowVisible(hwnd)) mouse::SetPollSeconds(10);
            Invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            float s = GetDpiForWindow(hwnd) / 96.0f;
            mm->ptMinTrackSize.x = (LONG)(820 * s);
            mm->ptMinTrackSize.y = (LONG)(560 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            g_scale = HIWORD(wp) / 96.0f;
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_rt) g_rt->SetDpi((float)HIWORD(wp), (float)HIWORD(wp));
            Invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) {
                ApplyTheme();
                tray::Update(g_ms, g_cfg.trayPercent, TrayExtra());   // light/dark taskbar
            }
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplyTheme();
            return 0;
        case WM_ACTIVATEAPP:
            // Hangar's theme may have changed while Halo was in the background
            if (wp && MatchingHangar() && !g_hasHangarLive) ApplyTheme();
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) mouse::Rescan();
            return TRUE;
        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            if (!g_tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                g_tracking = true;
            }
            HotRect h = HitAt(x, y);
            if (h.kind != g_hover.kind || h.index != g_hover.index || h.arg != g_hover.arg) {
                g_hover = h;
                StartAnim();
            }
            bool text = h.kind == Hit::DpiField || h.kind == Hit::ProfileName;
            SetCursor(LoadCursorW(nullptr, text && h.enabled ? IDC_IBEAM : (h.kind != Hit::None && h.enabled) ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;
            break;
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_hover = {{0, 0, 0, 0}, Hit::None, -1, 0, false};
            StartAnim();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            g_pressed = HitAt(x, y);
            if (g_pressed.kind == Hit::None && g_edit.active) EndEdit(true);
            SetCapture(hwnd);
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            HotRect up = HitAt(x, y), down = g_pressed;
            g_pressed = {{0, 0, 0, 0}, Hit::None, -1, 0, false};
            if (up.kind == down.kind && up.index == down.index && up.arg == down.arg && up.kind != Hit::None) Click(up, false);
            Invalidate();
            return 0;
        }
        case WM_RBUTTONUP: {
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            HotRect h = HitAt(x, y);
            if (h.kind != Hit::None) Click(h, true);
            return 0;
        }
        case WM_MOUSEWHEEL:
            Scroll(GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 96.0f);
            return 0;
        case WM_KEYDOWN:
            OnKey(wp);
            return 0;
        case WM_CHAR:
            OnChar((wchar_t)wp);
            return 0;
        case WM_KILLFOCUS:
            if (g_edit.active) EndEdit(true);
            return 0;
        case WM_TIMER:
            switch (wp) {
                case TIMER_ANIM:
                    KillTimer(hwnd, TIMER_ANIM);
                    Invalidate();
                    break;
                case TIMER_CARET:
                    g_caretOn = !g_caretOn;
                    Invalidate();
                    break;
                case TIMER_TOAST:
                    KillTimer(hwnd, TIMER_TOAST);
                    g_toastAt = 0;
                    StartAnim();
                    break;
                case TIMER_SELF:
                    SetTimer(hwnd, TIMER_SELF, 60 * 1000, nullptr);
                    if (!g_selfCheckedTick || GetTickCount() - g_selfCheckedTick > (DWORD)g_cfg.checkHours * 3600 * 1000) CheckSelf(false);
                    MaybeAutoApplySelf();
                    break;
                case TIMER_HEALTHY:
                    // the window is up: tell the copy we replaced that this version works
                    KillTimer(hwnd, TIMER_HEALTHY);
                    selfupdate::SignalHealthy(g_after);
                    selfupdate::FinishAfterUpdate(g_after);
                    break;
                case TIMER_MIGRATE:
                    // still called Halo.exe (0.1.0 or older): rename and restart once, so Discord stops
                    // showing "Playing Halo: Combat Evolved"
                    KillTimer(hwnd, TIMER_MIGRATE);
                    if (g_edit.active || g_preparingUpdate) { SetTimer(hwnd, TIMER_MIGRATE, 30 * 1000, nullptr); break; }
                    if (install::MigrateExeName(RestoreJson())) {
                        SaveWindowPlacement();
                        ConfigSave();
                        g_quitting = true;
                        DestroyWindow(hwnd);
                    }
                    break;
                case TIMER_FOREGROUND:
                    KillTimer(hwnd, TIMER_FOREGROUND);
                    CheckForeground();
                    break;
                case TIMER_HANGAR_HEALTH:
                    KillTimer(hwnd, TIMER_HANGAR_HEALTH);
                    ipc::SendHealth();
                    break;
            }
            return 0;
        case WM_HALO_MOUSE:
            OnMouse();
            return 0;
        case WM_HALO_SELF: {
            std::unique_ptr<SelfUpdateState> r((SelfUpdateState*)lp);
            bool wasReady = g_self.kind == SelfUpdateState::Ready;
            if (!(wasReady && (r->kind == SelfUpdateState::Error || r->kind == SelfUpdateState::Checking))) g_self = *r;
            if (r->kind == SelfUpdateState::Ready && !wasReady) {
                if (!g_cfg.autoUpdate && Visible()) Toast(L"Halo " + r->version + L" is ready. Restart Halo to update.");
                MaybeAutoApplySelf();
            }
            Invalidate();
            return 0;
        }
        case WM_HALO_IPC: {
            std::unique_ptr<std::shared_ptr<ipc::Request>> p((std::shared_ptr<ipc::Request>*)lp);
            OnIpc(**p);
            return 0;
        }
        case WM_HALO_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (Visible() && GetForegroundWindow() == hwnd) HideToTray();
                    else ShowMain();
                    break;
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    tray::ShowMenu(hwnd, g_ms);
                    break;
            }
            return 0;
        case WM_COMMAND: {
            UINT id = LOWORD(wp);
            if (id == tray::kCmdOpen) ShowMain();
            else if (id == tray::kCmdQuit) Quit();
            else if (id == tray::kCmdRefresh) mouse::Refresh();
            else if (id >= tray::kCmdDpiBase && id < tray::kCmdDpiBase + 100) {
                size_t k = id - tray::kCmdDpiBase;
                if (k < g_cfg.dpiPresets.size()) {
                    mouse::SetDpi(g_cfg.dpiPresets[k]);
                    ShowOsd(Thousands(g_cfg.dpiPresets[k]) + L" DPI", L"Preset " + std::to_wstring(k + 1) + L" of " + std::to_wstring(g_cfg.dpiPresets.size()),
                            (k + 1) / (float)g_cfg.dpiPresets.size());
                }
            } else if (id >= tray::kCmdPollBase && id < tray::kCmdPollBase + 5) {
                int hz = ulx::kPollingRates[id - tray::kCmdPollBase];
                mouse::SetPollingRate(hz);
                ShowOsd(Thousands(hz) + L" Hz", L"Polling rate", (id - tray::kCmdPollBase + 1) / 5.0f);
            } else if (id >= tray::kCmdProfileBase && id < tray::kCmdProfileBase + 100) {
                size_t k = id - tray::kCmdProfileBase;
                if (k < g_cfg.profiles.size()) {
                    ApplyProfile(g_cfg.profiles[k], From::Outside);
                }
            }
            return 0;
        }
        case WM_INPUT:
            OnRawInput(lp);
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_HOTKEY:
            if (wp == HK_DPI_UP) CycleDpi(1);
            else if (wp == HK_DPI_DOWN) CycleDpi(-1);
            else if (wp == HK_PROFILE) CycleProfile();
            return 0;
        case WM_HALO_ACTIVATE:
            ShowMain();
            return 0;
        case WM_HALO_QUIT:
            Quit();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) { SaveWindowPlacement(); ConfigSave(); }
            return 0;
        case WM_CLOSE:
            if (g_cfg.closeToTray && !g_quitting) { HideToTray(); return 0; }
            Quit();
            return 0;
        case WM_DESTROY:
            if (g_devNotify) UnregisterDeviceNotification(g_devNotify);
            if (g_fgHook) UnhookWinEvent(g_fgHook);
            StopPollTest();
            osd::Destroy();
            tray::Remove();
            ipc::Stop();
            mouse::Stop();
            DiscardDevice();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND UiCreate(HINSTANCE inst, const StartOptions& opt) {
    g_after = opt.after;
    if (!gfx::Init()) return nullptr;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = HALO_WINDOW_CLASS;
    RegisterClassExW(&wc);

    g_page = PageFromId(g_cfg.page);
    UINT dpi = GetDpiForSystem();
    float s = dpi / 96.0f;
    int w = g_cfg.windowW > 0 ? g_cfg.windowW : (int)(1040 * s);
    int h = g_cfg.windowH > 0 ? g_cfg.windowH : (int)(720 * s);
    int x = g_cfg.windowX, y = g_cfg.windowY;
    if (x != CW_USEDEFAULT) {
        RECT r{x, y, x + w, y + h};
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) x = y = CW_USEDEFAULT;
    }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Halo", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return nullptr;
    PruneSkippedVersions(HALO_VERSION);

    int show = opt.tray ? SW_HIDE : SW_SHOWNORMAL;
    std::wstring restoredSummary;
    if (opt.after.active) {
        // Halo's own updater just handed over: open where the previous copy was
        show = ApplyRestore(selfupdate::TakeRestoreState(), nullptr);
        if (CompareVersions(opt.after.fromVersion, HALO_VERSION) < 0) Toast(std::wstring(L"Halo updated to ") + HALO_VERSION + L".");
        else Toast(std::wstring(L"Rolled back to Halo ") + HALO_VERSION + L".");
        Log(L"started after an update from %ls", opt.after.fromVersion.c_str());
    } else if (!opt.restoreState.empty()) {
        // Hangar's live update: same idea, Hangar's state file
        std::wstring text;
        ReadTextFile(opt.restoreState, text);
        show = ApplyRestore(text, &restoredSummary);
        DeleteFileW(opt.restoreState.c_str());
    }

    LoadHistory();
    g_estimate = battery::EstimateLeft(g_history, g_history.empty() ? -1 : g_history.back().pct);
    mouse::Start(hwnd);
    tray::Create(hwnd);
    ipc::Start(hwnd);
    RegisterDeviceNotifications(hwnd);
    RegisterHotkeys();
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundChanged, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    install::RefreshRegistration();
    mouse::SetPollSeconds(show == SW_HIDE ? 60 : 10);

    ShowWindow(hwnd, show);
    if (show != SW_HIDE) UpdateWindow(hwnd);
    if (opt.after.active) SetTimer(hwnd, TIMER_HEALTHY, 400, nullptr);
    SetTimer(hwnd, TIMER_HANGAR_HEALTH, 2000, nullptr);
    if (IEquals(PathName(ExePath()), HALO_LEGACY_EXE_NAME)) SetTimer(hwnd, TIMER_MIGRATE, 6000, nullptr);   // Hangar counts this as "the update worked"
    if (!restoredSummary.empty()) ipc::SendRestored(restoredSummary);
    SetTimer(hwnd, TIMER_SELF, 30 * 1000, nullptr);       // first check 30 s after startup
    return hwnd;
}
