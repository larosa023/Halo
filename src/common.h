// Halo - a small, fast control panel for the Finalmouse UltralightX
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

#ifndef HALO_VERSION
#define HALO_VERSION L"0.1.0"
#endif
#define HALO_APP_ID L"halo"
#define HALO_REPO_OWNER L"larosa023"
#define HALO_REPO_NAME L"Halo"
#define HALO_WINDOW_CLASS L"Halo.larosa023.Main"

// Messages posted to the main window from other threads. When lParam carries a heap object,
// the window procedure takes it over and deletes it.
enum : UINT {
    WM_HALO_MOUSE = WM_APP + 1,    // the mouse state changed; read mouse::Snapshot()
    WM_HALO_SELF,                  // lParam: SelfUpdateState* (Halo's own updater)
    WM_HALO_ACTIVATE,              // another Halo.exe asked us to come to the front
    WM_HALO_TRAY,                  // tray icon callback
    WM_HALO_IPC,                   // lParam: ipc::Request* (a message from Hangar that needs the UI thread)
    WM_HALO_QUIT,                  // `Halo.exe --quit` asked us to close
};
