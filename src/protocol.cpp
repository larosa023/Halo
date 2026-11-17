// Halo - Finalmouse UltralightX wire protocol
#include "protocol.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ulx {

const int kPollingRates[5] = {500, 1000, 2000, 4000, 8000};
const int kScrollTimeouts[4] = {100, 500, 1000, 1500};

std::vector<uint8_t> BuildReport(uint8_t reportId, uint8_t command, const uint8_t* payload, size_t n) {
    if (n > kReportBody - 3) n = kReportBody - 3;
    std::vector<uint8_t> b(kReportBody + 1, 0);
    b[0] = reportId;
    b[1] = (uint8_t)(2 + n);
    b[2] = (uint8_t)(0x80 | (command & 0x7F));
    b[3] = (uint8_t)n;
    for (size_t i = 0; i < n; ++i) b[4 + i] = payload[i];
    return b;
}

std::vector<uint8_t> BuildReport16(uint8_t reportId, uint8_t command, uint16_t value) {
    uint8_t p[2] = {(uint8_t)(value & 0xFF), (uint8_t)(value >> 8)};
    return BuildReport(reportId, command, p, 2);
}

std::vector<uint8_t> BuildReport8(uint8_t reportId, uint8_t command, uint8_t value) {
    return BuildReport(reportId, command, &value, 1);
}

static int U16(const uint8_t* p, size_t n) { return n < 2 ? -1 : (int)(p[0] | (p[1] << 8)); }

static std::string Ascii(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == 0) break;
        if (p[i] >= 0x20 && p[i] < 0x7F) s += (char)p[i];
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

int Decode(const uint8_t* buf, size_t len, Telemetry& t) {
    if (!buf || len < 4) return -1;
    uint8_t id = buf[0];
    if (id != kReportMouseIn && id != kReportDongleIn) return -1;
    size_t inner = std::min<size_t>(buf[1], len - 2);       // bytes from the command byte on
    if (inner < 2) return -1;
    const uint8_t* body = buf + 2;
    int cmd = body[0] & 0x7F;
    size_t n = std::min<size_t>(body[1], inner - 2);
    const uint8_t* p = body + 2;

    if (id == kReportDongleIn) {
        if (cmd == kDongleInfo) { t.dongleUsbFirmware = Ascii(p, n); return cmd; }
        return -1;
    }
    switch (cmd) {
        case kReadDpi: case kSetDpi: { int v = U16(p, n); if (v < 0) return -1; t.dpi = v; return cmd; }
        case kReadPolling: case kSetPolling: { int v = U16(p, n); if (v < 0) return -1; t.pollingHz = v; return cmd; }
        case kBatteryVoltage: { int v = U16(p, n); if (v < 0) return -1; t.batteryMv = v; return cmd; }
        case kDongleRfFirmware: t.dongleRfFirmware = Ascii(p, n); return cmd;
        case kMouseFirmware: t.mouseFirmware = Ascii(p, n); return cmd;
        case kRssi: if (n < 1) return -1; t.rssi = p[0] > 127 ? (int)p[0] - 256 : (int)p[0]; t.rssiKnown = true; return cmd;
        case kSerial: t.serial = Ascii(p, n); return cmd;
        case kMotionSync: if (n < 1) return -1; t.motionSync = p[0] == 1 ? 1 : 0; return cmd;
        case kDongleLed: if (n < 1) return -1; t.dongleLed = p[0]; return cmd;
        case kLiftOff: if (n < 1) return -1; t.liftOffMm = p[0]; return cmd;
        case kScrollMode: if (n < 1) return -1; t.scrollMode = p[0]; return cmd;
        case kScrollTimeout: if (n < 1) return -1; t.scrollTimeoutMs = p[0] * 100; return cmd;
        case kLinkState: if (n < 1) return -1; t.linkState = p[0] ? 1 : 0; if (!p[0]) t.rssiKnown = false; return cmd;
        case kCharging: if (n < 1) return -1; t.charging = p[0] ? 1 : 0; return cmd;
        case kStateOfCharge: {
            if (n < 1 || p[0] > 100) return -1;
            t.stateOfCharge = p[0];
            if (n >= 3) t.batteryMv = U16(p + 1, n - 1);
            return cmd;
        }
        default: return -1;
    }
}

int BatteryPercent(int millivolts) {
    if (millivolts <= 0) return -1;
    static const double v[] = {3.00, 3.62, 3.66, 3.74, 3.88, 4.17, 4.38};
    static const double pc[] = {0.2, 5, 10, 25, 50, 75, 100};
    double x = millivolts / 1000.0;
    if (x >= v[6]) return 100;
    if (x <= v[0]) return 0;
    for (int i = 0; i < 6; ++i) {
        if (x < v[i] || x > v[i + 1]) continue;
        double f = (x - v[i]) / (v[i + 1] - v[i]);
        return (int)std::lround(pc[i] + (pc[i + 1] - pc[i]) * f);
    }
    return 0;
}

int SignalBars(int rssi, bool known, int linkState) {
    if (!known || linkState == 0) return 0;
    int m = std::abs(rssi);
    if (m < 40) return 4;
    if (m < 50) return 3;
    if (m < 65) return 2;
    return 1;
}

} // namespace ulx
