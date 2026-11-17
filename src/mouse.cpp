// Halo - UltralightX dongle over HID (see mouse.h)
#include "mouse.h"
#include "util.h"
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include <algorithm>

namespace mouse {

int State::BatteryPercent() const {
    if (t.stateOfCharge >= 0) return t.stateOfCharge;
    return ulx::BatteryPercent(t.batteryMv);
}

const wchar_t* DongleLedName(int mode) {
    switch (mode) {
        case 0: return L"Off";
        case 1: return L"Battery color";
        case 2: return L"Solid white";
        default: return L"Unknown";
    }
}

const wchar_t* ScrollModeName(int mode) {
    switch (mode) {
        case 0: return L"Off";
        case 1: return L"Scroll up";
        case 2: return L"Scroll down";
        case 3: return L"Both directions";
        default: return L"Unknown";
    }
}

// ---------------------------------------------------------------- shared state
static std::mutex s_lock;
static State s_state;
static std::deque<std::vector<uint8_t>> s_queue;   // reports waiting to be sent
static bool s_wantFull = false, s_wantPoll = false, s_wantRescan = false;
static int s_pollSeconds = 15;
static DWORD s_verifyAt = 0;                        // re-read settings after writes (debounced)
static HWND s_notify = nullptr;
static HANDLE s_wake = nullptr, s_stop = nullptr;
static std::thread s_thread;
static std::atomic<bool> s_posted{false};

static void Notify() {
    // coalesce: one WM_HALO_MOUSE in the queue at a time
    if (s_notify && !s_posted.exchange(true)) {
        if (!PostMessageW(s_notify, WM_HALO_MOUSE, 0, 0)) s_posted = false;
    }
}

State Snapshot() {
    s_posted = false;
    std::lock_guard<std::mutex> g(s_lock);
    return s_state;
}

static void Enqueue(std::vector<uint8_t> report, bool verify) {
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_queue.push_back(std::move(report));
        if (verify) s_verifyAt = GetTickCount() + 600;
    }
    if (s_wake) SetEvent(s_wake);
}

void Rescan() { { std::lock_guard<std::mutex> g(s_lock); s_wantRescan = true; } if (s_wake) SetEvent(s_wake); }
void Refresh() { { std::lock_guard<std::mutex> g(s_lock); s_wantFull = true; } if (s_wake) SetEvent(s_wake); }
void SetPollSeconds(int seconds) {
    bool sooner;
    {
        std::lock_guard<std::mutex> g(s_lock);
        sooner = seconds < s_pollSeconds;
        s_pollSeconds = std::max(3, seconds);
        if (sooner) s_wantPoll = true;
    }
    if (sooner && s_wake) SetEvent(s_wake);
}

// Settings are applied to the shown state immediately, then confirmed by a re-read.
template <class F> static void Optimistic(F f) {
    { std::lock_guard<std::mutex> g(s_lock); f(s_state.t); }
    Notify();
}

void SetDpi(int dpi) {
    dpi = std::clamp(dpi, ulx::kDpiMin, ulx::kDpiMax);
    Optimistic([&](ulx::Telemetry& t) { t.dpi = dpi; });
    Enqueue(ulx::BuildReport16(ulx::kReportMouse, ulx::kSetDpi, (uint16_t)dpi), true);
}
void SetPollingRate(int hz) {
    if (std::find(std::begin(ulx::kPollingRates), std::end(ulx::kPollingRates), hz) == std::end(ulx::kPollingRates)) return;
    Optimistic([&](ulx::Telemetry& t) { t.pollingHz = hz; });
    Enqueue(ulx::BuildReport16(ulx::kReportMouse, ulx::kSetPolling, (uint16_t)hz), true);
}
void SetMotionSync(bool on) {
    Optimistic([&](ulx::Telemetry& t) { t.motionSync = on ? 1 : 0; });
    Enqueue(ulx::BuildReport8(ulx::kReportMouse, ulx::kMotionSync, on ? 1 : 0), true);
}
void SetLiftOff(int mm) {
    if (mm != 1 && mm != 2) return;
    Optimistic([&](ulx::Telemetry& t) { t.liftOffMm = mm; });
    Enqueue(ulx::BuildReport8(ulx::kReportMouse, ulx::kLiftOff, (uint8_t)mm), true);
}
void SetDongleLed(int mode) {
    if (mode < 0 || mode > 2) return;
    Optimistic([&](ulx::Telemetry& t) { t.dongleLed = mode; });
    Enqueue(ulx::BuildReport8(ulx::kReportMouse, ulx::kDongleLed, (uint8_t)mode), true);
}
void SetScrollMode(int mode) {
    if (mode < 0 || mode > 3) return;
    Optimistic([&](ulx::Telemetry& t) { t.scrollMode = mode; });
    Enqueue(ulx::BuildReport8(ulx::kReportMouse, ulx::kScrollMode, (uint8_t)mode), true);
}
void SetScrollTimeout(int ms) {
    if (std::find(std::begin(ulx::kScrollTimeouts), std::end(ulx::kScrollTimeouts), ms) == std::end(ulx::kScrollTimeouts)) return;
    Optimistic([&](ulx::Telemetry& t) { t.scrollTimeoutMs = ms; });
    Enqueue(ulx::BuildReport8(ulx::kReportMouse, ulx::kScrollTimeout, (uint8_t)(ms / 100)), true);
}

// ---------------------------------------------------------------- the device
struct Device {
    HANDLE h = INVALID_HANDLE_VALUE;
    USHORT outLen = 0, inLen = 0;
    bool useSetOutputReport = false;      // WriteFile was refused; use the control pipe instead
    std::wstring product;
};

static bool Matches(const std::wstring& path) {
    return IContains(path, L"vid_361d");
}

// Finds the dongle's vendor collection. Also reports whether the mouse is on its cable alone.
static bool OpenDongle(Device& dev, bool& wiredSeen, std::wstring& error) {
    wiredSeen = false;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO di = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(di, nullptr, &hidGuid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, detail, need, nullptr, nullptr)) continue;
        std::wstring path = detail->DevicePath;
        if (!Matches(path)) continue;

        // attributes can be read without access rights, even for the mouse collection Windows owns
        HANDLE q = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{sizeof(attr)};
        bool attrOk = HidD_GetAttributes(q, &attr);
        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA pp = nullptr;
        bool capsOk = HidD_GetPreparsedData(q, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS;
        if (pp) HidD_FreePreparsedData(pp);
        wchar_t prod[128] = {};
        HidD_GetProductString(q, prod, sizeof(prod));
        CloseHandle(q);
        if (!attrOk || attr.VendorID != ulx::kVendorId) continue;
        if (attr.ProductID == ulx::kWiredProductId) { wiredSeen = true; continue; }
        if (attr.ProductID != ulx::kDongleProductId || !capsOk) continue;
        if (caps.UsagePage != ulx::kUsagePage || caps.Usage != ulx::kUsage) continue;

        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            error = L"Windows wouldn't let Halo open the dongle (error " + std::to_wstring(e) + L").";
            Log(L"open %ls: error %lu", path.c_str(), e);
            continue;
        }
        dev.h = h;
        dev.outLen = std::max<USHORT>(caps.OutputReportByteLength, (USHORT)(ulx::kReportBody + 1));
        dev.inLen = std::max<USHORT>(caps.InputReportByteLength, (USHORT)(ulx::kReportBody + 1));
        dev.product = prod[0] ? prod : L"UltralightX dongle";
        Log(L"dongle open: %ls (out %u, in %u)", dev.product.c_str(), caps.OutputReportByteLength, caps.InputReportByteLength);
        found = true;
    }
    SetupDiDestroyDeviceInfoList(di);
    return found;
}

static bool Send(Device& dev, const std::vector<uint8_t>& report) {
    std::vector<uint8_t> buf(dev.outLen, 0);
    std::copy(report.begin(), report.begin() + std::min(report.size(), buf.size()), buf.begin());
    if (!dev.useSetOutputReport) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD written = 0;
        BOOL ok = WriteFile(dev.h, buf.data(), (DWORD)buf.size(), nullptr, &ov);
        DWORD err = ok ? 0 : GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0) ok = GetOverlappedResult(dev.h, &ov, &written, FALSE);
            else { CancelIoEx(dev.h, &ov); GetOverlappedResult(dev.h, &ov, &written, TRUE); err = ERROR_TIMEOUT; }
            if (!ok && err != ERROR_TIMEOUT) err = GetLastError();
        }
        CloseHandle(ov.hEvent);
        if (ok) return true;
        if (err == ERROR_DEVICE_NOT_CONNECTED) return false;
        if (err == ERROR_TIMEOUT) { Log(L"write timed out"); return true; }   // busy radio; the next poll retries
        Log(L"WriteFile failed (%lu); trying HidD_SetOutputReport", err);
    }
    if (HidD_SetOutputReport(dev.h, buf.data(), (ULONG)buf.size())) {
        if (!dev.useSetOutputReport) Log(L"using HidD_SetOutputReport from now on");
        dev.useSetOutputReport = true;
        return true;
    }
    Log(L"HidD_SetOutputReport failed (%lu)", GetLastError());
    return false;
}

// ---------------------------------------------------------------- worker
static std::vector<std::vector<uint8_t>> FullRead() {
    // the same sequence XPANEL sends when it connects: lift-off first, then "send everything"
    // to the mouse and a firmware query to the dongle
    return {
        ulx::BuildReport(ulx::kReportMouse, ulx::kLiftOff),
        ulx::BuildReport(ulx::kReportMouse, ulx::kWakeAll),
        ulx::BuildReport(ulx::kReportDongle, ulx::kDongleInfo),
        ulx::BuildReport(ulx::kReportMouse, ulx::kLinkState),
        ulx::BuildReport(ulx::kReportMouse, ulx::kBatteryVoltage),
        ulx::BuildReport(ulx::kReportMouse, ulx::kCharging),
    };
}

static std::vector<std::vector<uint8_t>> PollRead() {
    return {
        ulx::BuildReport(ulx::kReportMouse, ulx::kLinkState),
        ulx::BuildReport(ulx::kReportMouse, ulx::kBatteryVoltage),
        ulx::BuildReport(ulx::kReportMouse, ulx::kCharging),
        ulx::BuildReport(ulx::kReportMouse, ulx::kRssi),
    };
}

static void SetConn(State::Conn c, bool wired, const std::wstring& error, const std::wstring& product = L"") {
    {
        std::lock_guard<std::mutex> g(s_lock);
        bool changed = s_state.conn != c || s_state.wiredOnly != wired || s_state.error != error;
        if (!changed && c != State::Connected) return;
        if (c != State::Connected) { s_state.t = ulx::Telemetry{}; s_state.readComplete = false; }
        else if (s_state.conn != State::Connected) { s_state.connectedTick = GetTickCount(); s_state.readComplete = false; s_state.t = ulx::Telemetry{}; }
        s_state.conn = c;
        s_state.wiredOnly = wired;
        s_state.error = error;
        if (!product.empty()) s_state.product = product;
    }
    Notify();
}

// HALO_DEMO=1: a pretend mouse, for screenshots and trying Halo without the hardware.
// Writes are "answered" by echoing them back the way the mouse confirms a setting.
static void DemoWorker() {
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_state.conn = State::Connected;
        s_state.product = L"UltralightX dongle (demo)";
        s_state.connectedTick = s_state.lastReplyTick = GetTickCount();
        ulx::Telemetry& t = s_state.t;
        t.dpi = 1600; t.pollingHz = 4000; t.batteryMv = 4012; t.charging = 0; t.linkState = 1;
        t.rssi = -42; t.rssiKnown = true; t.motionSync = 0; t.liftOffMm = 1; t.dongleLed = 1; t.scrollMode = 0; t.scrollTimeoutMs = 500;
        t.mouseFirmware = "1.4.2"; t.dongleRfFirmware = "2.1.0"; t.dongleUsbFirmware = "2.0.7"; t.serial = "ULX-DEMO-0001";
        s_state.readComplete = true;
    }
    Notify();
    while (WaitForSingleObject(s_stop, 0) != WAIT_OBJECT_0) {
        HANDLE w[2] = {s_stop, s_wake};
        if (WaitForMultipleObjects(2, w, FALSE, 30000) == WAIT_OBJECT_0) break;
        std::lock_guard<std::mutex> g(s_lock);
        while (!s_queue.empty()) {
            std::vector<uint8_t> r = std::move(s_queue.front());
            s_queue.pop_front();
            if (r.size() > 3 && r[3] > 0) { r[0] = ulx::kReportMouseIn; ulx::Decode(r.data(), r.size(), s_state.t); }
        }
        s_wantFull = s_wantPoll = s_wantRescan = false;
        s_verifyAt = 0;
    }
}

static void Worker() {
    wchar_t demo[8] = {};
    if (GetEnvironmentVariableW(L"HALO_DEMO", demo, 8) && demo[0] == L'1') { DemoWorker(); return; }
    Device dev;
    std::vector<uint8_t> rbuf;
    OVERLAPPED rov{};
    rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool readPending = false;
    DWORD nextPoll = 0, nextSearch = 0;
    int linkBefore = -1;

    auto closeDev = [&]() {
        if (dev.h != INVALID_HANDLE_VALUE) {
            if (readPending) { CancelIoEx(dev.h, &rov); DWORD n; GetOverlappedResult(dev.h, &rov, &n, TRUE); }
            CloseHandle(dev.h);
            Log(L"dongle closed");
        }
        readPending = false;
        dev = Device{};
    };

    auto handleReport = [&](const uint8_t* b, size_t n) {
        bool full = false;
        {
            std::lock_guard<std::mutex> g(s_lock);
            int cmd = ulx::Decode(b, n, s_state.t);
            if (cmd < 0) return;
            s_state.lastReplyTick = GetTickCount();
            if (s_state.t.dpi >= 0 && s_state.t.pollingHz >= 0) s_state.readComplete = true;
            if (cmd == ulx::kLinkState) {
                // the mouse just woke up or was switched on: read everything again
                if (s_state.t.linkState == 1 && linkBefore == 0) full = true;
                linkBefore = s_state.t.linkState;
            }
        }
        if (full) { std::lock_guard<std::mutex> g(s_lock); s_wantFull = true; }
        Notify();
    };

    while (WaitForSingleObject(s_stop, 0) != WAIT_OBJECT_0) {
        DWORD now = GetTickCount();
        bool rescan;
        { std::lock_guard<std::mutex> g(s_lock); rescan = s_wantRescan; s_wantRescan = false; }

        if (dev.h == INVALID_HANDLE_VALUE) {
            if (rescan || (int)(now - nextSearch) >= 0) {
                bool wired = false;
                std::wstring err;
                if (OpenDongle(dev, wired, err)) {
                    rbuf.assign(dev.inLen, 0);
                    linkBefore = -1;
                    SetConn(State::Connected, false, L"", dev.product);
                    { std::lock_guard<std::mutex> g(s_lock); s_wantFull = true; s_queue.clear(); }
                    nextPoll = GetTickCount() + 2500;
                } else {
                    SetConn(State::NoDongle, wired, err);
                    nextSearch = GetTickCount() + 10000;   // device notifications usually find it sooner
                }
            }
            if (dev.h == INVALID_HANDLE_VALUE) {
                HANDLE w[2] = {s_stop, s_wake};
                WaitForMultipleObjects(2, w, FALSE, std::max<DWORD>(200, nextSearch - GetTickCount()));
                continue;
            }
        } else if (rescan) {
            // a device came or went; make sure ours is still there (without touching its input queue)
            HIDD_ATTRIBUTES attr{sizeof(attr)};
            bool stillThere = HidD_GetAttributes(dev.h, &attr) || GetLastError() != ERROR_DEVICE_NOT_CONNECTED;
            if (!stillThere) { closeDev(); SetConn(State::NoDongle, false, L""); continue; }
        }

        // keep one read in flight
        if (!readPending) {
            ResetEvent(rov.hEvent);
            DWORD got = 0;
            if (ReadFile(dev.h, rbuf.data(), (DWORD)rbuf.size(), &got, &rov)) {
                handleReport(rbuf.data(), got);
                continue;
            }
            DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING) {
                Log(L"ReadFile failed (%lu); dongle gone", e);
                closeDev();
                SetConn(State::NoDongle, false, L"");
                continue;
            }
            readPending = true;
        }

        // queued work
        std::vector<std::vector<uint8_t>> out;
        {
            std::lock_guard<std::mutex> g(s_lock);
            if (s_wantFull) { auto f = FullRead(); out.insert(out.end(), f.begin(), f.end()); s_wantFull = false; s_wantPoll = false; nextPoll = now + s_pollSeconds * 1000; }
            while (!s_queue.empty()) { out.push_back(std::move(s_queue.front())); s_queue.pop_front(); }
            if (s_verifyAt && (int)(now - s_verifyAt) >= 0) {
                s_verifyAt = 0;
                out.push_back(ulx::BuildReport(ulx::kReportMouse, ulx::kWakeAll));
            }
            if (s_wantPoll || (int)(now - nextPoll) >= 0) {
                auto p = PollRead();
                out.insert(out.end(), p.begin(), p.end());
                s_wantPoll = false;
                nextPoll = now + s_pollSeconds * 1000;
            }
        }
        bool failed = false;
        for (auto& r : out) {
            if (!Send(dev, r)) { failed = true; break; }
            Sleep(12);   // the dongle relays each command over the air; don't flood it
        }
        if (failed) {
            closeDev();
            SetConn(State::NoDongle, false, L"");
            continue;
        }

        // wait for a reply, new work, or the next poll
        DWORD wait = 1000;
        {
            std::lock_guard<std::mutex> g(s_lock);
            DWORD n2 = GetTickCount();
            wait = (int)(nextPoll - n2) > 0 ? nextPoll - n2 : 0;
            if (s_verifyAt) wait = std::min<DWORD>(wait, (int)(s_verifyAt - n2) > 0 ? s_verifyAt - n2 : 0);
        }
        HANDLE w[3] = {s_stop, rov.hEvent, s_wake};
        DWORD r = WaitForMultipleObjects(3, w, FALSE, std::max<DWORD>(10, wait));
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 1) {
            DWORD got = 0;
            readPending = false;
            if (GetOverlappedResult(dev.h, &rov, &got, FALSE)) {
                handleReport(rbuf.data(), got);
            } else {
                DWORD e = GetLastError();
                if (e != ERROR_OPERATION_ABORTED) {
                    Log(L"read failed (%lu); dongle gone", e);
                    closeDev();
                    SetConn(State::NoDongle, false, L"");
                }
            }
        }
    }
    closeDev();
    CloseHandle(rov.hEvent);
}

void Start(HWND notify) {
    if (s_thread.joinable()) return;
    s_notify = notify;
    s_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_thread = std::thread(Worker);
}

void Stop() {
    if (!s_thread.joinable()) return;
    SetEvent(s_stop);
    s_thread.join();
    CloseHandle(s_wake);
    CloseHandle(s_stop);
    s_wake = s_stop = nullptr;
    s_notify = nullptr;
}

} // namespace mouse
