// Halo - notification-area icon: the battery level at a glance, a quick menu, and alerts
#pragma once
#include "common.h"
#include "mouse.h"

namespace tray {

enum : UINT {
    kCmdOpen = 100, kCmdQuit, kCmdRefresh,
    kCmdDpiBase = 1000,        // + preset index
    kCmdPollBase = 1100,       // + index into ulx::kPollingRates
    kCmdProfileBase = 1200,    // + profile index
};

void Create(HWND hwnd);
void Remove();
void Recreate();               // Explorer restarted (TaskbarCreated)
void Update(const mouse::State& s, bool showPercent);
void Notify(const std::wstring& title, const std::wstring& text, bool warning);
void ShowMenu(HWND hwnd, const mouse::State& s);   // the chosen command arrives as WM_COMMAND

} // namespace tray
