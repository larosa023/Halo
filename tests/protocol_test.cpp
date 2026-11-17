// Halo - protocol tests. Builds and runs anywhere: g++ -std=c++17 -Isrc tests/protocol_test.cpp src/protocol.cpp
//
// The expected bytes come from real UltralightX hardware (replies captured by HaloBattery issue #283)
// and from the settings writes captured in johnklucinec/ulx_reverse.
#include "protocol.h"
#include <cstdio>
#include <vector>

static int g_failed = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::vector<uint8_t> Pad(std::vector<uint8_t> v) { v.resize(64, 0); return v; }

static bool Starts(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want) {
    if (got.size() != 64) return false;
    for (size_t i = 0; i < want.size(); ++i) if (got[i] != want[i]) return false;
    for (size_t i = want.size(); i < got.size(); ++i) if (got[i] != 0) return false;   // zero padded
    return true;
}

int main() {
    using namespace ulx;
    // ---- writes (ulx_reverse captures)
    CHECK(Starts(BuildReport16(kReportMouse, kSetPolling, 1000), {0x04, 0x04, 0x91, 0x02, 0xE8, 0x03}));
    CHECK(Starts(BuildReport16(kReportMouse, kSetPolling, 500), {0x04, 0x04, 0x91, 0x02, 0xF4, 0x01}));
    CHECK(Starts(BuildReport16(kReportMouse, kSetPolling, 4000), {0x04, 0x04, 0x91, 0x02, 0xA0, 0x0F}));
    CHECK(Starts(BuildReport16(kReportMouse, kSetDpi, 1600), {0x04, 0x04, 0x90, 0x02, 0x40, 0x06}));
    CHECK(Starts(BuildReport8(kReportMouse, kMotionSync, 1), {0x04, 0x03, 0x92, 0x01, 0x01}));
    CHECK(Starts(BuildReport8(kReportMouse, kLiftOff, 2), {0x04, 0x03, 0x95, 0x01, 0x02}));
    CHECK(Starts(BuildReport8(kReportMouse, kDongleLed, 0), {0x04, 0x03, 0x94, 0x01, 0x00}));
    // reads: empty payload
    CHECK(Starts(BuildReport(kReportMouse, kWakeAll), {0x04, 0x02, 0xE0, 0x00}));
    CHECK(Starts(BuildReport(kReportDongle, kDongleInfo), {0x02, 0x02, 0x8A, 0x00}));
    CHECK(Starts(BuildReport(kReportMouse, kBatteryVoltage), {0x04, 0x02, 0x85, 0x00}));

    // ---- replies (real hardware)
    Telemetry t;
    auto link = Pad({0x05, 0x03, 0x24, 0x01, 0x01});
    CHECK(Decode(link.data(), link.size(), t) == kLinkState);
    CHECK(t.linkState == 1);
    auto volt = Pad({0x05, 0x04, 0x05, 0x02, 0x64, 0x10});
    CHECK(Decode(volt.data(), volt.size(), t) == kBatteryVoltage);
    CHECK(t.batteryMv == 4196);
    CHECK(BatteryPercent(4196) == 78);      // what XPANEL shows for this reading
    CHECK(BatteryPercent(4276) == 88);
    CHECK(BatteryPercent(4188) == 77);
    auto chg0 = Pad({0x05, 0x03, 0x25, 0x01, 0x00});
    CHECK(Decode(chg0.data(), chg0.size(), t) == kCharging && t.charging == 0);
    auto chg1 = Pad({0x05, 0x03, 0x25, 0x01, 0x01});
    CHECK(Decode(chg1.data(), chg1.size(), t) == kCharging && t.charging == 1);
    auto asleep = Pad({0x05, 0x03, 0x24, 0x01, 0x00});
    CHECK(Decode(asleep.data(), asleep.size(), t) == kLinkState && t.linkState == 0);

    // DPI and polling read back on commands 3 and 4
    auto dpi = Pad({0x05, 0x04, 0x03, 0x02, 0x20, 0x03});
    CHECK(Decode(dpi.data(), dpi.size(), t) == kReadDpi && t.dpi == 800);
    auto poll = Pad({0x05, 0x04, 0x04, 0x02, 0x40, 0x1F});
    CHECK(Decode(poll.data(), poll.size(), t) == kReadPolling && t.pollingHz == 8000);
    // RSSI is signed
    auto rssi = Pad({0x05, 0x03, 0x0D, 0x01, 0xD3});
    CHECK(Decode(rssi.data(), rssi.size(), t) == kRssi && t.rssi == -45 && t.rssiKnown);
    CHECK(SignalBars(-45, true, 1) == 3);
    CHECK(SignalBars(-38, true, 1) == 4);
    CHECK(SignalBars(-70, true, 1) == 1);
    CHECK(SignalBars(-38, true, 0) == 0);
    // firmware strings
    auto fw = Pad({0x05, 0x07, 0x0C, 0x05, '1', '.', '2', '.', '3'});
    CHECK(Decode(fw.data(), fw.size(), t) == kMouseFirmware && t.mouseFirmware == "1.2.3");
    auto usb = Pad({0x03, 0x06, 0x0A, 0x04, 'v', '2', '.', '0'});
    CHECK(Decode(usb.data(), usb.size(), t) == kDongleInfo && t.dongleUsbFirmware == "v2.0");
    // tournament scroll timeout is in 100 ms units
    auto sto = Pad({0x05, 0x03, 0x18, 0x01, 0x05});
    CHECK(Decode(sto.data(), sto.size(), t) == kScrollTimeout && t.scrollTimeoutMs == 500);
    // state of charge: not answered by current firmware, but decoded if it ever is
    auto soc = Pad({0x05, 0x05, 0x26, 0x03, 0x4E, 0x64, 0x10});
    Telemetry t2;
    CHECK(Decode(soc.data(), soc.size(), t2) == kStateOfCharge && t2.stateOfCharge == 78 && t2.batteryMv == 4196);
    // garbage is ignored
    auto junk = Pad({0x01, 0x02, 0x03});
    CHECK(Decode(junk.data(), junk.size(), t) == -1);
    auto shortr = std::vector<uint8_t>{0x05, 0x04, 0x05};
    CHECK(Decode(shortr.data(), shortr.size(), t) == -1);
    auto trunc = Pad({0x05, 0x03, 0x05, 0x02, 0x64});   // claims 2 bytes, frame only holds 1
    Telemetry t3;
    CHECK(Decode(trunc.data(), trunc.size(), t3) == -1 && t3.batteryMv == -1);

    std::printf("%d/%d checks passed\n", g_run - g_failed, g_run);
    return g_failed ? 1 : 0;
}
