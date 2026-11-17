// Halo - install / uninstall (see install.h)
#include "install.h"
#include "util.h"
#include "config.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <objbase.h>
#include <commctrl.h>

namespace install {

static const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Halo";

std::wstring InstallDir() { return KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\Halo"; }

bool RunningFromInstallDir() { return PathStartsWith(ExePath(), InstallDir()); }

static std::wstring ShortcutPath() { return KnownFolder(FOLDERID_Programs) + L"\\Halo.lnk"; }

static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target) {
    IShellLinkW* link = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(PathParent(target).c_str());
        link->SetDescription(L"Halo - battery, DPI and polling for your Finalmouse UltralightX");
        link->SetIconLocation(target.c_str(), 0);
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    }
    if (pf) pf->Release();
    if (link) link->Release();
    return ok;
}

static void SetRegStr(HKEY key, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

static void Register(const std::wstring& exe) {
    if (!FileExists(ShortcutPath())) MakeShortcut(ShortcutPath(), exe);
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    SetRegStr(h, L"DisplayName", L"Halo");
    SetRegStr(h, L"DisplayVersion", HALO_VERSION);
    SetRegStr(h, L"Publisher", HALO_REPO_OWNER);
    SetRegStr(h, L"DisplayIcon", exe + L",0");
    SetRegStr(h, L"InstallLocation", PathParent(exe));
    SetRegStr(h, L"UninstallString", L"\"" + exe + L"\" --uninstall");
    SetRegStr(h, L"URLInfoAbout", L"https://github.com/larosa023/Halo");
    DWORD one = 1;
    RegSetValueExW(h, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegSetValueExW(h, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegCloseKey(h);
}

void RefreshRegistration() {
    if (!RunningFromInstallDir() || IsPortable()) return;
    Register(ExePath());
    if (g_cfg.startWithWindows) ApplyStartup(true);   // the path in the Run key follows the exe
}

bool InstallSelf(std::wstring* error) {
    std::wstring dir = InstallDir(), target = dir + L"\\Halo.exe";
    EnsureDir(dir);
    if (!CopyFileW(ExePath().c_str(), target.c_str(), FALSE)) {
        *error = L"Couldn't copy Halo.exe into " + dir + L" (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    Register(target);
    return true;
}

// 1 = remove Halo, 2 = remove Halo and its settings, 0 = keep it
static int AskUninstall() {
    TASKDIALOG_BUTTON buttons[] = {
        {101, L"Remove Halo\nYour profiles and settings stay, in case you come back"},
        {102, L"Remove Halo and its settings\nProfiles, presets and themes are deleted too"},
    };
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"Uninstall Halo";
    tc.pszMainIcon = TD_INFORMATION_ICON;
    tc.pszMainInstruction = L"Remove Halo from this PC?";
    tc.pszContent = L"Your mouse keeps every setting you made: they're stored on the mouse itself.";
    tc.pButtons = buttons;
    tc.cButtons = 2;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.nDefaultButton = 101;
    int pressed = 0;
    using Fn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE cc = LoadLibraryW(L"comctl32.dll");
    auto fn = cc ? (Fn)GetProcAddress(cc, "TaskDialogIndirect") : nullptr;
    if (fn && SUCCEEDED(fn(&tc, &pressed, nullptr, nullptr))) return pressed == 101 ? 1 : pressed == 102 ? 2 : 0;
    // no task dialogs (very old comctl32): a plain message box
    int choice = MessageBoxW(nullptr,
        L"Remove Halo from this PC?\n\nYour mouse keeps every setting you made; they're stored on the mouse.\n\n"
        L"Yes: remove Halo and its settings\nNo: remove Halo, keep its settings\nCancel: keep Halo",
        L"Uninstall Halo", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    return choice == IDYES ? 2 : choice == IDNO ? 1 : 0;
}

int Uninstall() {
    int choice = AskUninstall();
    if (choice == 0) return 1;

    // close the running copy the normal way
    if (HWND w = FindWindowW(HALO_WINDOW_CLASS, nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId()) {
            PostMessageW(w, WM_HALO_QUIT, 0, 0);
            if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid)) { WaitForSingleObject(p, 8000); CloseHandle(p); }
        }
    }
    ApplyStartup(false);
    DeleteFileW(ShortcutPath().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    DeleteFileW((HangarDataDir() + L"\\run\\halo.json").c_str());
    if (choice == 2 && !IsPortable()) {
        // DataDir() is %LOCALAPPDATA%\Halo: settings, log, staged updates, previous versions
        std::wstring d = DataDir();
        d.push_back(0);
        d.push_back(0);
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = d.c_str();
        op.fFlags = FOF_NO_UI;
        SHFileOperationW(&op);
    }
    // a running exe can't delete itself: a hidden cmd waits for us to exit, then removes the exe
    std::wstring exe = ExePath(), dir = ExeDir();
    bool removeDir = RunningFromInstallDir();
    std::wstring cmd = L"cmd.exe /d /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + exe + L"\" & del /f /q \"" + exe + L".old\"";
    if (removeDir) cmd += L" & rmdir \"" + dir + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        MoveFileExW(exe.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    return 0;
}

} // namespace install
