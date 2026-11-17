// Halo - main window
#pragma once
#include "common.h"
#include "selfupdate.h"

struct StartOptions {
    bool tray = false;                   // --tray: start hidden in the notification area
    std::wstring restoreState;           // --restore-state <file> (a Hangar live update)
    selfupdate::AfterUpdate after;       // --after-update (Halo's own updater)
};

HWND UiCreate(HINSTANCE inst, const StartOptions& opt);
