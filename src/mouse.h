// Halo - talks to the UltralightX dongle on a background thread.
//
// The thread owns the HID handle. It keeps one read pending all the time, sends queued commands,
// polls battery / link / signal on a timer, and re-finds the dongle when it's plugged back in.
// Every change is announced with WM_HALO_MOUSE; the UI then calls Snapshot().
#pragma once
#include "common.h"
#include "protocol.h"

namespace mouse {

struct State {
    enum Conn { Searching, NoDongle, Connected } conn = Searching;
    bool wiredOnly = false;          // the mouse is plugged in by cable but its dongle isn't
    ulx::Telemetry t;
    std::wstring product;            // "UltralightX dongle"
    std::wstring error;              // why the dongle couldn't be used (shown in the UI)
    unsigned long lastReplyTick = 0;
    unsigned long connectedTick = 0;
    bool readComplete = false;       // DPI and polling rate have arrived since connecting

    bool Linked() const { return conn == Connected && t.linkState == 1; }
    bool Asleep() const { return conn == Connected && t.linkState == 0; }
    int BatteryPercent() const;      // -1 when unknown
};

void Start(HWND notify);
void Stop();
State Snapshot();

void Rescan();                       // look for the dongle again now (device plugged in / removed)
void Refresh();                      // read every setting again
void SetPollSeconds(int seconds);    // battery / link poll interval (shorter while the window is open)

// Settings. Each is sent to the mouse right away and saved on it (it keeps them without Halo).
void SetDpi(int dpi);
void SetPollingRate(int hz);
void SetMotionSync(bool on);
void SetLiftOff(int mm);             // 1 or 2
void SetDongleLed(int mode);         // 0 off, 1 battery color, 2 solid white
void SetScrollMode(int mode);        // 0 off, 1 up, 2 down, 3 both
void SetScrollTimeout(int ms);       // 100, 500, 1000, 1500

const wchar_t* DongleLedName(int mode);
const wchar_t* ScrollModeName(int mode);

} // namespace mouse
