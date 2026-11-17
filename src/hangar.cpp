// Halo - Hangar IPC (see hangar.h)
#include "hangar.h"
#include "util.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>

namespace ipc {

static HWND s_notify = nullptr;
static std::thread s_thread;
static std::atomic<bool> s_stop{false};
static HANDLE s_stopEvent = nullptr;
static std::mutex s_busyLock;
static bool s_busy = false;
static std::wstring s_busyReason;

static DWORD SessionId() {
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    return sid;
}

std::wstring PipeName(const std::wstring& appId) {
    return L"\\\\.\\pipe\\larosa023." + appId + L"." + std::to_wstring(SessionId());
}

static std::wstring RunFile() { return HangarDataDir() + L"\\run\\" HALO_APP_ID L".json"; }

void SetBusy(bool busy, const std::wstring& reason) {
    std::lock_guard<std::mutex> g(s_busyLock);
    s_busy = busy;
    s_busyReason = reason;
}

// json::Write pretty-prints; the protocol wants one object per line
static std::string Line(const json::Value& v) {
    std::wstring out, flat;
    json::Write(v, out);
    flat.reserve(out.size());
    bool inStr = false, esc = false;
    for (wchar_t c : out) {
        if (inStr) {
            flat += c;
            if (esc) esc = false;
            else if (c == L'\\') esc = true;
            else if (c == L'"') inStr = false;
            continue;
        }
        if (c == L'"') { inStr = true; flat += c; continue; }
        if (c == L'\n' || c == L'\r' || c == L' ') continue;
        flat += c;
    }
    return WideToUtf8(flat) + "\n";
}

// Hands a request to the UI thread and waits for the answer.
static json::Value AskUi(const std::wstring& type, const json::Value& msg, DWORD timeoutMs) {
    auto req = std::make_shared<Request>();
    req->type = type;
    req->msg = msg;
    auto* heap = new std::shared_ptr<Request>(req);
    if (!s_notify || !PostMessageW(s_notify, WM_HALO_IPC, 0, (LPARAM)heap)) {
        delete heap;
        json::Value r = json::Value::MakeObject();
        r.Set(L"ok", json::Value::Boolean(false));
        r.Set(L"reason", json::Value::Str(L"Halo is closing"));
        return r;
    }
    HANDLE w[2] = {req->done, s_stopEvent};
    if (WaitForMultipleObjects(2, w, FALSE, timeoutMs) == WAIT_OBJECT_0) return req->reply;
    json::Value r = json::Value::MakeObject();
    r.Set(L"ok", json::Value::Boolean(false));
    r.Set(L"reason", json::Value::Str(L"Halo didn't answer in time"));
    return r;
}

static json::Value Handle(const json::Value& msg) {
    std::wstring type = msg.GetStr(L"type");
    json::Value r = json::Value::MakeObject();
    if (type == L"hello") {
        r.Set(L"protocol", json::Value::Num(1));
        r.Set(L"app", json::Value::Str(HALO_APP_ID));
        r.Set(L"version", json::Value::Str(HALO_VERSION));
        json::Value caps = json::Value::MakeArray();
        for (const wchar_t* c : {L"state", L"theme", L"settings"}) caps.arr.push_back(json::Value::Str(c));
        r.Set(L"capabilities", caps);
    } else if (type == L"status") {
        std::lock_guard<std::mutex> g(s_busyLock);
        r.Set(L"state", json::Value::Str(s_busy ? L"busy" : L"idle"));
        if (s_busy) r.Set(L"reason", json::Value::Str(s_busyReason));
    } else if (type == L"prepare-update" || type == L"restart" || type == L"cancel-update" || type == L"apply-theme" ||
               type == L"apply-settings" || type == L"export-settings") {
        r = AskUi(type, msg, 10000);
    } else {
        r.Set(L"ok", json::Value::Boolean(false));
        r.Set(L"reason", json::Value::Str(L"unknown request type"));
    }
    if (const json::Value* id = msg.Get(L"id")) r.Set(L"id", *id);
    return r;
}

static bool WriteAll(HANDLE h, const std::string& s) {
    DWORD w = 0;
    return WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size();
}

// One client at a time is plenty: only Hangar connects.
static void Serve() {
    std::wstring name = PipeName(HALO_APP_ID);
    while (!s_stop) {
        HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       1, 64 * 1024, 64 * 1024, 0, nullptr);   // default DACL: this user (and SYSTEM / admins)
        if (pipe == INVALID_HANDLE_VALUE) {
            Log(L"ipc: CreateNamedPipe failed (%lu)", GetLastError());
            if (WaitForSingleObject(s_stopEvent, 5000) == WAIT_OBJECT_0) return;
            continue;
        }
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ConnectNamedPipe(pipe, &ov) != 0;
        if (!connected) {
            DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) connected = true;
            else if (e == ERROR_IO_PENDING) {
                HANDLE w[2] = {ov.hEvent, s_stopEvent};
                if (WaitForMultipleObjects(2, w, FALSE, INFINITE) == WAIT_OBJECT_0) { DWORD n; connected = GetOverlappedResult(pipe, &ov, &n, FALSE) != 0; }
                else { CancelIoEx(pipe, &ov); DWORD n; GetOverlappedResult(pipe, &ov, &n, TRUE); }
            }
        }
        if (connected) {
            std::string buf;
            char chunk[4096];
            bool closeAfter = false;
            while (!s_stop && !closeAfter) {
                ResetEvent(ov.hEvent);
                DWORD got = 0;
                BOOL ok = ReadFile(pipe, chunk, sizeof(chunk), nullptr, &ov);
                if (!ok && GetLastError() != ERROR_IO_PENDING) break;
                HANDLE w[2] = {ov.hEvent, s_stopEvent};
                if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) { CancelIoEx(pipe, &ov); DWORD n; GetOverlappedResult(pipe, &ov, &n, TRUE); break; }
                if (!GetOverlappedResult(pipe, &ov, &got, FALSE) || got == 0) break;
                buf.append(chunk, got);
                if (buf.size() > 1024 * 1024 + 4096) break;   // lines are at most 1 MB
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
                    json::Value msg, reply;
                    if (!json::Parser(Utf8ToWide(line)).Parse(msg) || msg.type != json::Value::Object) {
                        reply = json::Value::MakeObject();
                        reply.Set(L"ok", json::Value::Boolean(false));
                        reply.Set(L"reason", json::Value::Str(L"not a JSON object"));
                    } else {
                        reply = Handle(msg);
                    }
                    // "restart": the pipe closes and Halo exits (the UI already started closing)
                    if (msg.GetStr(L"type") == L"restart") { closeAfter = true; break; }
                    // synchronous write is fine: replies are small and Hangar is reading
                    OVERLAPPED wov{};
                    wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    std::string out = Line(reply);
                    DWORD wrote = 0;
                    if (!WriteFile(pipe, out.data(), (DWORD)out.size(), nullptr, &wov) && GetLastError() == ERROR_IO_PENDING)
                        WaitForSingleObject(wov.hEvent, 5000);
                    GetOverlappedResult(pipe, &wov, &wrote, FALSE);
                    CloseHandle(wov.hEvent);
                }
            }
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
    }
}

static void WriteRunFile() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"pid", json::Value::Num(GetCurrentProcessId()));
    v.Set(L"pipe", json::Value::Str(PipeName(HALO_APP_ID)));
    v.Set(L"version", json::Value::Str(HALO_VERSION));
    v.Set(L"protocol", json::Value::Num(1));
    v.Set(L"exe", json::Value::Str(ExePath()));
    std::wstring out;
    json::Write(v, out);
    EnsureDir(HangarDataDir() + L"\\run");
    if (!WriteTextFileAtomic(RunFile(), out)) Log(L"ipc: couldn't write %ls", RunFile().c_str());
}

void Start(HWND notify) {
    if (s_thread.joinable()) return;
    s_notify = notify;
    s_stop = false;
    s_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_thread = std::thread(Serve);
    WriteRunFile();
}

void Stop() {
    // only remove the discovery file if it's still ours (a newer copy may have replaced it)
    std::wstring text;
    if (ReadTextFile(RunFile(), text)) {
        json::Value v;
        if (json::Parser(text).Parse(v) && (DWORD)v.GetNum(L"pid", 0) == GetCurrentProcessId()) DeleteFileW(RunFile().c_str());
    }
    if (!s_thread.joinable()) return;
    s_stop = true;
    SetEvent(s_stopEvent);
    s_thread.join();
    CloseHandle(s_stopEvent);
    s_stopEvent = nullptr;
    s_notify = nullptr;
}

// Messages to Hangar's own pipe. Hangar may not be running (or may not listen yet); that's fine.
static void SendToHangar(const json::Value& v) {
    std::string line = Line(v);
    std::thread([line] {
        std::wstring name = PipeName(L"hangar");
        for (int attempt = 0; attempt < 3; ++attempt) {
            HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                WriteAll(h, line);
                CloseHandle(h);
                return;
            }
            if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), 1000)) return;
        }
    }).detach();
}

void SendHealth() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"type", json::Value::Str(L"health"));
    v.Set(L"app", json::Value::Str(HALO_APP_ID));
    v.Set(L"version", json::Value::Str(HALO_VERSION));
    v.Set(L"pid", json::Value::Num(GetCurrentProcessId()));
    SendToHangar(v);
}

void SendRestored(const std::wstring& summary) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"type", json::Value::Str(L"restored"));
    v.Set(L"app", json::Value::Str(HALO_APP_ID));
    v.Set(L"summary", json::Value::Str(summary));
    SendToHangar(v);
}

void SendBusy(bool busy, const std::wstring& reason) {
    json::Value v = json::Value::MakeObject();
    v.Set(L"type", json::Value::Str(busy ? L"busy" : L"idle"));
    v.Set(L"app", json::Value::Str(HALO_APP_ID));
    v.Set(L"reason", json::Value::Str(reason));
    SendToHangar(v);
}

} // namespace ipc
