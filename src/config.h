// Halo - settings (DataDir()\settings.json)
#pragma once
#include "common.h"

// A saved set of mouse settings you can switch to in one click (stored in Halo, applied to the mouse).
struct Profile {
    std::wstring name;
    int dpi = 800, polling = 1000, motionSync = 0, liftOff = 1, dongleLed = 1, scrollMode = 0, scrollTimeout = 500;
};

struct Config {
    // appearance
    std::wstring theme = L"hangar";     // "hangar" (match Hangar), a built-in id (dark, light, system…), or custom:<id>
    std::wstring accent = L"theme";     // theme | windows | #RRGGBB (ignored while matching Hangar)
    // window and tray
    int windowX = CW_USEDEFAULT, windowY = CW_USEDEFAULT, windowW = 0, windowH = 0;
    std::wstring page = L"overview";
    bool closeToTray = true;            // the X button hides Halo in the tray instead of quitting
    bool startWithWindows = false;      // starts hidden in the tray
    bool trayPercent = true;            // draw the battery level in the tray icon
    bool trayHintShown = false;         // "Halo is still running in the tray" was shown once
    // battery alerts
    int lowBattery = 15;                // percent; 0 = off
    // DPI presets shown as chips
    std::vector<int> dpiPresets = {400, 800, 1600, 3200, 6400};
    std::vector<Profile> profiles;
    // Halo's own updates
    bool beta = false;
    bool autoUpdate = true;             // install updates by itself when it won't interrupt
    int checkHours = 6;
    std::wstring skipVersion;           // versions that failed to start or were rolled back (comma-separated)
};

extern Config g_cfg;
void ConfigLoad();
void ConfigSave();
std::wstring ConfigPath();
bool IsSkippedVersion(const std::wstring& v);
void SkipVersion(const std::wstring& v);
void PruneSkippedVersions(const std::wstring& current);

// Hangar's chosen theme and accent, read from its settings.json (empty when Hangar isn't installed)
bool ReadHangarTheme(std::wstring& theme, std::wstring& accent);

// Start with Windows (HKCU\...\Run\Halo)
void ApplyStartup(bool on, const std::wstring& exe = L"");   // exe defaults to this one
