// Halo - battery history tests: g++ -std=c++17 -Isrc tests/battery_test.cpp src/battery.cpp
#include "battery.h"
#include <cstdio>
#include <cmath>

static int g_failed = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

using namespace battery;

int main() {
    const int64_t T0 = 1'763'600'000;
    // recording cadence
    std::vector<Sample> h;
    CHECK(Add(h, {T0, 90, 4200, false}));
    CHECK(!Add(h, {T0 + 60, 90, 4200, false}));              // too soon
    CHECK(Add(h, {T0 + 120, 90, 4200, true}));               // charging changed: always
    CHECK(Add(h, {T0 + 120 + kInterval, 92, 4210, true}));
    CHECK(!Add(h, {T0, 50, 3800, false}));                   // out of order
    CHECK(h.size() == 3);

    // CSV round trip, junk tolerated
    auto csv = ToCsv(h);
    auto back = Parse(csv + "garbage\n,,,\n");
    CHECK(back.size() == 3 && back[2].pct == 92 && back[2].charging && back[0].mv == 4200);

    // two weeks kept
    std::vector<Sample> old;
    Add(old, {T0, 80, 4000, false});
    Add(old, {T0 + kKeep + 10, 70, 3950, false});
    CHECK(old.size() == 1 && old[0].pct == 70);

    // estimate: charged, then 3% an hour for 10 h of use with an overnight gap
    std::vector<Sample> d;
    d.push_back({T0, 100, 4380, true});
    int64_t t = T0 + 600;
    double pct = 100;
    for (int i = 0; i < 60; ++i) {                // 5 h at 3%/h
        d.push_back({t, (int)std::lround(pct), 4200, false});
        t += kInterval; pct -= 3.0 * kInterval / 3600;
    }
    t += 10 * 3600;                                // asleep overnight: doesn't count
    for (int i = 0; i < 60; ++i) {
        d.push_back({t, (int)std::lround(pct), 4100, false});
        t += kInterval; pct -= 3.0 * kInterval / 3600;
    }
    int now = (int)std::lround(pct);
    Estimate e = EstimateLeft(d, now);
    std::printf("rate %.2f %%/h, %.1f h left at %d%%\n", e.ratePerHour, e.hoursLeft, now);
    CHECK(std::fabs(e.ratePerHour - 3.0) < 0.3);
    CHECK(std::fabs(e.hoursLeft - now / 3.0) < 2.5);
    CHECK(e.lastCharged == T0);

    // charged while Halo wasn't running: a jump up starts a new discharge
    std::vector<Sample> j;
    for (int i = 0; i < 30; ++i) j.push_back({T0 + i * kInterval, 40 - i / 6, 3800, false});
    j.push_back({T0 + 30 * kInterval + 8 * 3600, 95, 4300, false});
    Estimate e2 = EstimateLeft(j, 95);
    CHECK(e2.hoursLeft < 0);                       // not enough data since that charge yet
    CHECK(e2.lastCharged == T0 + 30 * kInterval + 8 * 3600);

    // not enough data
    CHECK(EstimateLeft({}, 50).hoursLeft < 0);
    std::vector<Sample> s2 = {{T0, 80, 4000, false}, {T0 + 1800, 79, 4000, false}};
    CHECK(EstimateLeft(s2, 79).hoursLeft < 0);

    CHECK(FormatHours(0.5) == L"Under an hour");
    CHECK(FormatHours(6.2) == L"About 6 h");
    CHECK(FormatHours(28) == L"About 1 day 4 h");
    CHECK(FormatHours(24 * 5 + 3) == L"About 5 days");

    std::printf("%d/%d checks passed\n", g_run - g_failed, g_run);
    return g_failed ? 1 : 0;
}
