// Halo - working with Hangar, the suite's home base (github.com/larosa023/Hangar).
//
// Halo speaks Hangar's IPC protocol v1 (Hangar docs/IPC.md):
//   - a named-pipe server at \\.\pipe\larosa023.halo.<sessionId>, JSON lines, current user only
//   - a discovery file at %LOCALAPPDATA%\Hangar\run\halo.json: {pid, pipe, version, protocol: 1}
//   - "health" sent to Hangar's pipe about 2 s after a good start, "restored" after --restore-state
//   - hello / status / prepare-update / restart / cancel-update / apply-theme / apply-settings /
//     export-settings
// It also handles Hangar's command-line conventions: --quit and --uninstall.
#pragma once
#include "common.h"
#include "json.h"

namespace ipc {

// A request that has to be answered on the UI thread. WM_HALO_IPC's lParam is a heap
// std::shared_ptr<Request>*; the UI fills `reply`, calls Done() and deletes its pointer.
struct Request {
    std::wstring type;
    json::Value msg;
    json::Value reply = json::Value::MakeObject();
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Request() = default;
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    ~Request() { if (done) CloseHandle(done); }
    void Done() { SetEvent(done); }
};

void Start(HWND notify);             // pipe server + discovery file
void Stop();                         // removes the discovery file
void SendHealth();                   // tell Hangar this version started fine (no-op if Hangar isn't listening)
void SendRestored(const std::wstring& summary);
void SendBusy(bool busy, const std::wstring& reason);

std::wstring PipeName(const std::wstring& appId);

// Set by the UI: while true, "status" answers busy (Hangar won't restart Halo then)
void SetBusy(bool busy, const std::wstring& reason);

} // namespace ipc
