// Halo - settings
#include "config.h"
#include "util.h"
#include "json.h"
#include <algorithm>

Config g_cfg;

std::wstring ConfigPath() { return DataDir() + L"\\settings.json"; }

void ConfigLoad() {
    std::wstring text;
    if (!ReadTextFile(ConfigPath(), text)) return;
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) { Log(L"settings.json is damaged; using defaults"); return; }
    Config c;
    c.theme = v.GetStr(L"theme", c.theme);
    c.accent = v.GetStr(L"accent", c.accent);
    c.page = v.GetStr(L"page", c.page);
    c.closeToTray = v.GetBool(L"closeToTray", c.closeToTray);
    c.startWithWindows = v.GetBool(L"startWithWindows", c.startWithWindows);
    c.trayPercent = v.GetBool(L"trayPercent", c.trayPercent);
    c.trayHintShown = v.GetBool(L"trayHintShown", c.trayHintShown);
    c.lowBattery = std::clamp((int)v.GetNum(L"lowBattery", c.lowBattery), 0, 50);
    c.beta = v.GetBool(L"beta", c.beta);
    c.autoUpdate = v.GetBool(L"autoUpdate", c.autoUpdate);
    c.checkHours = std::clamp((int)v.GetNum(L"checkHours", c.checkHours), 1, 48);
    c.skipVersion = v.GetStr(L"skipVersion");
    if (const json::Value* w = v.Get(L"window"); w && w->type == json::Value::Object) {
        c.windowX = (int)w->GetNum(L"x", CW_USEDEFAULT);
        c.windowY = (int)w->GetNum(L"y", CW_USEDEFAULT);
        c.windowW = (int)w->GetNum(L"w", 0);
        c.windowH = (int)w->GetNum(L"h", 0);
    }
    if (const json::Value* d = v.Get(L"dpiPresets"); d && d->type == json::Value::Array) {
        std::vector<int> list;
        for (auto& x : d->arr) if (x.type == json::Value::Number && x.n >= 50 && x.n <= 26000) list.push_back((int)x.n);
        if (!list.empty()) c.dpiPresets = list;
    }
    if (const json::Value* ps = v.Get(L"profiles"); ps && ps->type == json::Value::Array) {
        for (auto& p : ps->arr) {
            if (p.type != json::Value::Object) continue;
            Profile pr;
            pr.name = p.GetStr(L"name", L"Profile");
            pr.dpi = std::clamp((int)p.GetNum(L"dpi", pr.dpi), 50, 26000);
            pr.polling = (int)p.GetNum(L"polling", pr.polling);
            pr.motionSync = p.GetBool(L"motionSync", pr.motionSync != 0) ? 1 : 0;
            pr.liftOff = (int)p.GetNum(L"liftOff", pr.liftOff) == 2 ? 2 : 1;
            pr.dongleLed = std::clamp((int)p.GetNum(L"dongleLed", pr.dongleLed), 0, 2);
            pr.scrollMode = std::clamp((int)p.GetNum(L"scrollMode", pr.scrollMode), 0, 3);
            pr.scrollTimeout = (int)p.GetNum(L"scrollTimeout", pr.scrollTimeout);
            c.profiles.push_back(pr);
        }
    }
    g_cfg = c;
}

void ConfigSave() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"theme", json::Value::Str(g_cfg.theme));
    v.Set(L"accent", json::Value::Str(g_cfg.accent));
    v.Set(L"page", json::Value::Str(g_cfg.page));
    v.Set(L"closeToTray", json::Value::Boolean(g_cfg.closeToTray));
    v.Set(L"startWithWindows", json::Value::Boolean(g_cfg.startWithWindows));
    v.Set(L"trayPercent", json::Value::Boolean(g_cfg.trayPercent));
    v.Set(L"trayHintShown", json::Value::Boolean(g_cfg.trayHintShown));
    v.Set(L"lowBattery", json::Value::Num(g_cfg.lowBattery));
    v.Set(L"beta", json::Value::Boolean(g_cfg.beta));
    v.Set(L"autoUpdate", json::Value::Boolean(g_cfg.autoUpdate));
    v.Set(L"checkHours", json::Value::Num(g_cfg.checkHours));
    v.Set(L"skipVersion", json::Value::Str(g_cfg.skipVersion));
    json::Value w = json::Value::MakeObject();
    w.Set(L"x", json::Value::Num(g_cfg.windowX));
    w.Set(L"y", json::Value::Num(g_cfg.windowY));
    w.Set(L"w", json::Value::Num(g_cfg.windowW));
    w.Set(L"h", json::Value::Num(g_cfg.windowH));
    v.Set(L"window", w);
    json::Value d = json::Value::MakeArray();
    for (int x : g_cfg.dpiPresets) d.arr.push_back(json::Value::Num(x));
    v.Set(L"dpiPresets", d);
    json::Value ps = json::Value::MakeArray();
    for (auto& pr : g_cfg.profiles) {
        json::Value p = json::Value::MakeObject();
        p.Set(L"name", json::Value::Str(pr.name));
        p.Set(L"dpi", json::Value::Num(pr.dpi));
        p.Set(L"polling", json::Value::Num(pr.polling));
        p.Set(L"motionSync", json::Value::Boolean(pr.motionSync != 0));
        p.Set(L"liftOff", json::Value::Num(pr.liftOff));
        p.Set(L"dongleLed", json::Value::Num(pr.dongleLed));
        p.Set(L"scrollMode", json::Value::Num(pr.scrollMode));
        p.Set(L"scrollTimeout", json::Value::Num(pr.scrollTimeout));
        ps.arr.push_back(p);
    }
    v.Set(L"profiles", ps);
    std::wstring out;
    json::Write(v, out);
    if (!WriteTextFileAtomic(ConfigPath(), out)) Log(L"couldn't save settings.json");
}

static std::vector<std::wstring> SkipList() {
    std::vector<std::wstring> out;
    size_t i = 0;
    const std::wstring& s = g_cfg.skipVersion;
    while (i < s.size()) {
        size_t e = s.find(L',', i);
        if (e == std::wstring::npos) e = s.size();
        std::wstring v = Trim(s.substr(i, e - i));
        if (!v.empty()) out.push_back(v);
        i = e + 1;
    }
    return out;
}

static void SetSkipList(const std::vector<std::wstring>& list) {
    g_cfg.skipVersion.clear();
    for (auto& v : list) g_cfg.skipVersion += (g_cfg.skipVersion.empty() ? L"" : L",") + v;
}

bool IsSkippedVersion(const std::wstring& v) {
    for (auto& s : SkipList()) if (CompareVersions(s, v) == 0) return true;
    return false;
}

void SkipVersion(const std::wstring& v) {
    if (IsSkippedVersion(v)) return;
    auto list = SkipList();
    list.push_back(NormalizeVersion(v));
    SetSkipList(list);
}

void PruneSkippedVersions(const std::wstring& current) {
    std::vector<std::wstring> keep;
    for (auto& v : SkipList()) if (CompareVersions(v, current) > 0) keep.push_back(v);
    SetSkipList(keep);
}

bool ReadHangarTheme(std::wstring& theme, std::wstring& accent) {
    std::wstring text;
    if (!ReadTextFile(HangarDataDir() + L"\\settings.json", text)) return false;
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) return false;
    theme = v.GetStr(L"theme", L"dark");
    accent = v.GetStr(L"accent", L"theme");
    return true;
}

void ApplyStartup(bool on, const std::wstring& exe) {
    HKEY run;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &run) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + (exe.empty() ? ExePath() : exe) + L"\" --tray";
        RegSetValueExW(run, L"Halo", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(run, L"Halo");
    }
    RegCloseKey(run);
}
