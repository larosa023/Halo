// Halo - battery history (see battery.h)
#include "battery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace battery {

std::vector<Sample> Parse(const std::string& csv) {
    std::vector<Sample> out;
    std::istringstream in(csv);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] < '0' || line[0] > '9') continue;   // header or junk
        Sample s;
        long long t = 0;
        int pct = 0, mv = 0, ch = 0;
        if (std::sscanf(line.c_str(), "%lld,%d,%d,%d", &t, &pct, &mv, &ch) != 4) continue;
        if (t <= 0 || pct < 0 || pct > 100) continue;
        s.t = t;
        s.pct = pct;
        s.mv = mv;
        s.charging = ch != 0;
        out.push_back(s);
    }
    std::stable_sort(out.begin(), out.end(), [](const Sample& a, const Sample& b) { return a.t < b.t; });
    return out;
}

std::string ToCsv(const std::vector<Sample>& samples) {
    std::string out = "time,percent,millivolts,charging\n";
    char buf[96];
    for (auto& s : samples) {
        std::snprintf(buf, sizeof(buf), "%lld,%d,%d,%d\n", (long long)s.t, s.pct, s.mv, s.charging ? 1 : 0);
        out += buf;
    }
    return out;
}

bool ShouldRecord(const std::vector<Sample>& history, const Sample& s) {
    if (s.pct < 0 || s.t <= 0) return false;
    if (history.empty()) return true;
    const Sample& last = history.back();
    if (s.t <= last.t) return false;
    if (s.charging != last.charging) return true;          // charging started or stopped
    return s.t - last.t >= kInterval;
}

bool Add(std::vector<Sample>& history, const Sample& s) {
    if (!ShouldRecord(history, s)) return false;
    history.push_back(s);
    int64_t cutoff = s.t - kKeep;
    size_t drop = 0;
    while (drop < history.size() && history[drop].t < cutoff) ++drop;
    if (drop) history.erase(history.begin(), history.begin() + drop);
    return true;
}

Estimate EstimateLeft(const std::vector<Sample>& history, int currentPct) {
    Estimate e;
    if (history.empty()) return e;
    // walk back to the start of the current discharge: a charging sample, or a jump up of 4%+
    // (charged while Halo wasn't running)
    size_t start = history.size();
    for (size_t i = history.size(); i-- > 0;) {
        const Sample& s = history[i];
        if (s.charging) { e.lastCharged = s.t; break; }
        if (i + 1 < history.size() && history[i + 1].pct >= s.pct + 4) { e.lastCharged = history[i + 1].t; break; }
        start = i;
    }
    if (start >= history.size()) return e;
    // x = hours of use (gaps where the mouse was off or asleep don't count), y = percent
    std::vector<double> xs, ys;
    double x = 0;
    for (size_t i = start; i < history.size(); ++i) {
        if (i > start) {
            int64_t dt = history[i].t - history[i - 1].t;
            x += (dt > kGap ? kInterval : dt) / 3600.0;
        }
        xs.push_back(x);
        ys.push_back(history[i].pct);
    }
    if (x < 1.5 || ys.front() - ys.back() < 3) return e;
    double n = (double)xs.size(), sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < xs.size(); ++i) { sx += xs[i]; sy += ys[i]; sxx += xs[i] * xs[i]; sxy += xs[i] * ys[i]; }
    double den = n * sxx - sx * sx;
    if (std::fabs(den) < 1e-9) return e;
    double slope = (n * sxy - sx * sy) / den;
    if (slope >= -0.05) return e;
    e.ratePerHour = -slope;
    e.hoursLeft = std::max(0, currentPct) / e.ratePerHour;
    return e;
}

std::wstring FormatHours(double h) {
    if (h < 0) return L"";
    if (h < 1) return L"Under an hour";
    long total = std::lround(h);
    long days = total / 24, hours = total % 24;
    if (days == 0) return L"About " + std::to_wstring(hours) + L" h";
    std::wstring s = L"About " + std::to_wstring(days) + (days == 1 ? L" day" : L" days");
    if (hours && days < 4) s += L" " + std::to_wstring(hours) + L" h";
    return s;
}

} // namespace battery
