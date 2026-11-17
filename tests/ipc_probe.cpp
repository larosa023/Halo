// Halo - talks to a running Halo over the Hangar IPC pipe, the way Hangar will.
// Build: zig c++ -target x86_64-windows-gnu -std=c++17 -municode tests/ipc_probe.cpp -o dist/ipc_probe.exe
// Usage: ipc_probe.exe <json line> [<json line> ...]   (prints each reply)
#include <windows.h>
#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    std::wstring name = L"\\\\.\\pipe\\larosa023.halo." + std::to_wstring(sid);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 50 && h == INVALID_HANDLE_VALUE; ++i) {
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) Sleep(100);
    }
    if (h == INVALID_HANDLE_VALUE) { std::printf("no pipe (%lu)\n", GetLastError()); return 2; }
    std::string buf;
    for (int i = 1; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string line(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &line[0], n, nullptr, nullptr);
        line += "\n";
        DWORD w;
        WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
        // read one reply line (a "restart" gets none: the pipe closes)
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) { std::printf("%s\n", buf.substr(0, nl).c_str()); buf.erase(0, nl + 1); break; }
            char c[4096];
            DWORD got = 0;
            if (!ReadFile(h, c, sizeof(c), &got, nullptr) || !got) { std::printf("(pipe closed)\n"); CloseHandle(h); return 0; }
            buf.append(c, got);
        }
    }
    CloseHandle(h);
    return 0;
}
