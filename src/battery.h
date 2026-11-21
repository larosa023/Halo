// Halo - battery history and the time-left estimate (no Windows dependencies, so it's unit tested)
//
// Halo writes a sample every few minutes while the mouse is awake (and whenever charging starts or
// stops) to DataDir()\battery-history.csv, keeps 14 days, and draws the last 7 on the Overview.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace battery {

struct Sample {
    int64_t t = 0;        // unix seconds
    int pct = 0;          // 0-100
    int mv = 0;           // millivolts
    bool charging = false;
};

constexpr int64_t kInterval = 5 * 60;          // a sample every 5 minutes while awake
constexpr int64_t kKeep = 14 * 24 * 3600;      // two weeks of history
constexpr int64_t kGap = 40 * 60;              // longer than this between samples: the mouse was off or asleep

std::vector<Sample> Parse(const std::string& csv);
std::string ToCsv(const std::vector<Sample>& samples);

// Should `s` be recorded, given the history so far?
bool ShouldRecord(const std::vector<Sample>& history, const Sample& s);
// Adds `s` and drops samples older than kKeep. Returns true if anything changed.
bool Add(std::vector<Sample>& history, const Sample& s);

struct Estimate {
    double hoursLeft = -1;         // -1: not enough data yet
    double ratePerHour = 0;        // percent per hour while in use
    int64_t lastCharged = 0;       // unix seconds of the last charge (0 = not seen)
};

// Fits a line through the current discharge (since the last charge), counting only time the mouse
// was awake. Needs at least 90 minutes of use and a 3% drop.
Estimate EstimateLeft(const std::vector<Sample>& history, int currentPct);

// "About 1 day 4 h", "About 6 h", "Under an hour"
std::wstring FormatHours(double hours);

} // namespace battery
