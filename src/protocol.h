// Halo - the Finalmouse UltralightX wire protocol (no Windows dependencies, so it can be unit tested)
//
// The ULX dongle (USB 361D:0100) exposes a vendor HID collection (usage page 0xFF00, usage 0x0001).
// Every command is a 63-byte report after the report id:
//
//     [report id] [2 + n] [0x80 | command] [n] [payload: n bytes] [zero padding]
//
// Output report 0x04 talks to the mouse (through the dongle) and is answered on input report 0x05.
// Output report 0x02 talks to the dongle itself and is answered on input report 0x03.
// A command with an empty payload is a read ("GET"); with a payload it's a write.
// Replies use the same framing without the 0x80 bit. Some settings read back on a different
// command number than the one that writes them (DPI is written with 16 and read back as 3).
//
// Sources: Finalmouse's own XPANEL web configurator (WebHID), as documented by the open-source
// projects credited in README.md, and replies captured from real hardware.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace ulx {

constexpr uint16_t kVendorId = 0x361D;
constexpr uint16_t kDongleProductId = 0x0100;     // UltralightX 2.4 GHz dongle
constexpr uint16_t kWiredProductId = 0x0102;      // the mouse on its cable (no settings interface)
constexpr uint16_t kUsagePage = 0xFF00;
constexpr uint16_t kUsage = 0x0001;

constexpr uint8_t kReportMouse = 0x04, kReportMouseIn = 0x05;
constexpr uint8_t kReportDongle = 0x02, kReportDongleIn = 0x03;
constexpr size_t kReportBody = 63;                // bytes after the report id

// Commands
enum : uint8_t {
    // reads that report state
    kReadDpi = 3,
    kReadPolling = 4,
    kBatteryVoltage = 5,
    kDongleInfo = 10,        // dongle USB firmware (report 0x02 / 0x03)
    kDongleRfFirmware = 11,
    kMouseFirmware = 12,
    kRssi = 13,
    kSerial = 14,
    kLinkState = 36,
    kCharging = 37,
    kStateOfCharge = 38,     // not answered by current ULX firmware: voltage is the real source
    // settings (written with a payload, read back with an empty one)
    kSetDpi = 16,
    kSetPolling = 17,
    kMotionSync = 18,
    kDongleLed = 20,
    kLiftOff = 21,
    kScrollMode = 23,
    kScrollTimeout = 24,
    // asks the mouse to send everything it knows
    kWakeAll = 96,
};

// Builds the 64-byte buffer for WriteFile (report id first).
std::vector<uint8_t> BuildReport(uint8_t reportId, uint8_t command, const uint8_t* payload = nullptr, size_t n = 0);
std::vector<uint8_t> BuildReport16(uint8_t reportId, uint8_t command, uint16_t value);   // little-endian u16 payload
std::vector<uint8_t> BuildReport8(uint8_t reportId, uint8_t command, uint8_t value);

// Everything the mouse and dongle have told us so far. -1 / empty means "not reported yet".
struct Telemetry {
    int dpi = -1;
    int pollingHz = -1;
    int batteryMv = -1;
    int stateOfCharge = -1;
    int charging = -1;           // 0 / 1
    int linkState = -1;          // 0 = mouse asleep or off, 1 = linked
    int rssi = 0;
    bool rssiKnown = false;
    int motionSync = -1;         // 0 / 1
    int liftOffMm = -1;          // 1 / 2
    int dongleLed = -1;          // 0 off, 1 battery color, 2 solid white
    int scrollMode = -1;         // 0 off, 1 up, 2 down, 3 both
    int scrollTimeoutMs = -1;    // 100 / 500 / 1000 / 1500
    std::string mouseFirmware, dongleRfFirmware, dongleUsbFirmware, serial;
};

// Decodes one input report (buf[0] is the report id). Returns the command it carried, or -1 if the
// report isn't one of ours. Fields it carries are written into `t`.
int Decode(const uint8_t* buf, size_t len, Telemetry& t);

// Finalmouse's voltage-to-percent curve (the one XPANEL uses). While charging the voltage reads high,
// so the level does too; XPANEL shows the same.
int BatteryPercent(int millivolts);

// 0-4 signal bars from RSSI, using XPANEL's thresholds. 0 = no reading.
int SignalBars(int rssi, bool known, int linkState);

constexpr int kDpiMin = 50, kDpiMax = 26000;
extern const int kPollingRates[5];     // 500 1000 2000 4000 8000
extern const int kScrollTimeouts[4];   // 100 500 1000 1500 ms

} // namespace ulx
