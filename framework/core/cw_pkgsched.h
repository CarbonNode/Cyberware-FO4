// cw_pkgsched.h - the game-independent half of the {"cmd":"packages"} test seam (plugin/cw_pkgdiag.cpp): does a package's
// PSDT schedule cover the game clock now, and how do a condition list's per-item results combine. Header only, no RE types,
// so cwtest --selftest checks it on any box. ASCII only.
//
// PSDT (PACK_SCHED_DATA) fields, as the Creation Kit writes them:
//   month      -1 any, 0-11
//   dayOfWeek  -1 any, 0 Sunday .. 6 Saturday, 7 weekdays, 8 weekends, 9 Mon/Wed/Fri, 10 Tue/Thu
//   date        0 any, 1-31
//   hour       -1 any time of day, 0-23 the start hour
//   min         start minute (a negative value reads as 0)
//   duration    minutes from the start (a window past midnight wraps)
// Month, day of week and date are checked against TODAY even when the window started yesterday and wrapped.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cw::pkg {

enum class Tri { No = 0, Yes = 1, Unknown = 2 };
inline const char* TriName(Tri t) { return t == Tri::Yes ? "yes" : t == Tri::No ? "no" : "?"; }

struct Sched { int month = -1, dayOfWeek = -1, date = 0, hour = -1, min = 0, duration = 0; };
struct Clock { float hour = 0; int month = 0, date = 1, dayOfWeek = 0; };   // month 0-11, date 1-31, dayOfWeek 0 Sunday

// Gregorian day of week, 0 Sunday (Sakamoto). month0 is 0-11, as the GameMonth global holds it.
inline int DayOfWeek(int year, int month0, int date)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year, m = month0 + 1;
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + date) % 7;
}

inline bool DayOfWeekMatches(int want, int dow)
{
    switch (want) {
    case -1: return true;
    case 7: return dow >= 1 && dow <= 5;
    case 8: return dow == 0 || dow == 6;
    case 9: return dow == 1 || dow == 3 || dow == 5;
    case 10: return dow == 2 || dow == 4;
    default: return want == dow;
    }
}

// True when the schedule sets nothing at all: the package may run at any time.
inline bool IsAny(const Sched& s) { return s.month < 0 && s.dayOfWeek < 0 && s.date <= 0 && s.hour < 0; }

// The time window as text: "any", "all day", "14:00-24:00", "22:00-06:00", "10:00+0m".
inline std::string WindowText(const Sched& s)
{
    if (IsAny(s)) return "any";
    if (s.hour < 0) return "all day";
    char b[48];
    const int m0 = s.min < 0 ? 0 : s.min;
    if (s.duration <= 0) { std::snprintf(b, sizeof(b), "%02d:%02d+0m", s.hour, m0); return b; }
    const int start = s.hour * 60 + m0, end = start + s.duration;
    const int e = end > 1440 ? end % 1440 : end;   // 24:00 stays 24:00, past midnight wraps
    std::snprintf(b, sizeof(b), "%02d:%02d-%02d:%02d", start / 60, start % 60, e / 60, e % 60);
    return b;
}

// Does the schedule cover the clock now? Unknown only for a set start hour with duration 0 (the CK allows it; what the
// engine does with it is not proven here, so the seam says "?" rather than guess).
inline Tri Matches(const Sched& s, const Clock& c)
{
    if (s.month >= 0 && s.month != c.month) return Tri::No;
    if (s.date > 0 && s.date != c.date) return Tri::No;
    if (!DayOfWeekMatches(s.dayOfWeek, c.dayOfWeek)) return Tri::No;
    if (s.hour < 0) return Tri::Yes;
    if (s.duration <= 0) return Tri::Unknown;
    const double now = c.hour * 60.0;
    const double start = s.hour * 60.0 + (s.min < 0 ? 0 : s.min), end = start + s.duration;
    if (now >= start && now < end) return Tri::Yes;
    if (end > 1440.0 && now < end - 1440.0) return Tri::Yes;   // wrapped past midnight
    return Tri::No;
}

// Condition items in list order: each item's own result and its OR flag ("OR the NEXT item"). Bethesda's rule: a run of
// items joined by OR is one group; the groups are ANDed. Returns the list result and the index of every item in a group
// that failed (so the log can name "which condition fails"). An empty list passes.
struct Item { Tri result = Tri::Unknown; bool orNext = false; };
struct Combined { Tri pass = Tri::Yes; std::vector<int> failing; std::vector<int> unknown; };
inline Combined Combine(const std::vector<Item>& items)
{
    Combined out;
    std::size_t i = 0;
    while (i < items.size()) {
        std::size_t j = i;
        while (j + 1 < items.size() && items[j].orNext) ++j;   // group = i..j
        Tri g = Tri::No;
        for (std::size_t k = i; k <= j; ++k) {
            if (items[k].result == Tri::Yes) { g = Tri::Yes; break; }
            if (items[k].result == Tri::Unknown) g = Tri::Unknown;
        }
        if (g == Tri::No) {
            for (std::size_t k = i; k <= j; ++k) out.failing.push_back((int)k);
            out.pass = Tri::No;
        } else if (g == Tri::Unknown) {
            for (std::size_t k = i; k <= j; ++k) if (items[k].result == Tri::Unknown) out.unknown.push_back((int)k);
            if (out.pass == Tri::Yes) out.pass = Tri::Unknown;
        }
        i = j + 1;
    }
    return out;
}

inline std::string IndexList(const std::vector<int>& v)
{
    if (v.empty()) return "-";
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) s += ','; s += '#' + std::to_string(v[i]); }
    return s;
}

// The seam's verdict words: conditions=pass|fail|? and schedule=any|in|out|?
inline const char* CondWord(Tri t) { return t == Tri::Yes ? "pass" : t == Tri::No ? "fail" : "?"; }
inline const char* SchedWord(const Sched& s, Tri m) { return IsAny(s) ? "any" : m == Tri::Yes ? "in" : m == Tri::No ? "out" : "?"; }

}  // namespace cw::pkg
