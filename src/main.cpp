// Halo - a small, fast control panel for the Finalmouse UltralightX
//
//   HaloMouse.exe                         open Halo (or bring the running one to the front)
//   HaloMouse.exe --tray                  start hidden in the notification area (used by "Start with Windows")
//   HaloMouse.exe --quit                  ask the running Halo to close (Hangar uses this before updating)
//   HaloMouse.exe --uninstall             remove Halo (Apps & features and Hangar run this)
//   HaloMouse.exe --restore-state <file>  start where a previous version left off (Hangar live updates)
//   HaloMouse.exe --version               exit with code 0 (smoke tests)
#include "common.h"
#include "ui.h"
#include "util.h"
#include "config.h"
#include "net.h"
#include "install.h"
#include "selfupdate.h"
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

static HANDLE g_mutex;
static const wchar_t* kMutexName = L"Local\\Halo.larosa023.SingleInstance";

void ReleaseInstanceMutex() {
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
}
void ReacquireInstanceMutex() {
    if (!g_mutex) g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
}

static bool HasArg(int argc, wchar_t** argv, const wchar_t* a) {
    for (int i = 1; i < argc; ++i) if (_wcsicmp(argv[i], a) == 0) return true;
    return false;
}

static std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* a) {
    for (int i = 1; i + 1 < argc; ++i) if (_wcsicmp(argv[i], a) == 0) return argv[i + 1];
    return L"";
}

// --quit: close the running copy the way a person would, and wait for it to go
static int QuitRunning() {
    HWND w = FindWindowW(HALO_WINDOW_CLASS, nullptr);
    if (!w) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    PostMessageW(w, WM_HALO_QUIT, 0, 0);
    if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        DWORD r = WaitForSingleObject(p, 8000);
        CloseHandle(p);
        return r == WAIT_OBJECT_0 ? 0 : 1;
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    if (HasArg(argc, argv, L"--version")) return 0;
    if (HasArg(argc, argv, L"--quit")) return QuitRunning();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (HasArg(argc, argv, L"--uninstall")) {
        LogInit();
        int code = install::Uninstall();
        CoUninitialize();
        return code;
    }

    StartOptions opt;
    opt.tray = HasArg(argc, argv, L"--tray");
    opt.restoreState = ArgValue(argc, argv, L"--restore-state");
    opt.after = selfupdate::ParseArgs(GetCommandLineW());
    bool handover = opt.after.active || !opt.restoreState.empty() || HasArg(argc, argv, L"--after-install");
    LocalFree(argv);

    g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS && handover) {
        // the copy we're replacing is letting go; give it a moment
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        for (int i = 0; i < 80 && !g_mutex; ++i) {
            Sleep(100);
            HANDLE m = CreateMutexW(nullptr, TRUE, kMutexName);
            if (GetLastError() != ERROR_ALREADY_EXISTS) g_mutex = m; else CloseHandle(m);
        }
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!opt.tray) {
            if (HWND existing = FindWindowW(HALO_WINDOW_CLASS, nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(existing, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(existing, WM_HALO_ACTIVATE, 0, 0);
            }
        }
        CoUninitialize();
        return 0;
    }

    SetCurrentProcessExplicitAppUserModelID(L"larosa023.Halo");
    LogInit();
    Log(L"==== Halo %ls starting (data: %ls%ls)", HALO_VERSION, DataDir().c_str(), IsPortable() ? L", portable" : L"");
    ConfigLoad();
    net::Init();

    HWND hwnd = UiCreate(inst, opt);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Halo couldn't start. Details are in halo.log.", L"Halo", MB_ICONERROR);
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    net::Shutdown();
    Log(L"Halo exiting");
    CoUninitialize();
    if (g_mutex) CloseHandle(g_mutex);
    return 0;
}
