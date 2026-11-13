// confirm_replay.cpp - plugin/cw_confirm.cpp replayed against a real game log, no game. ASCII only.
// Build and run (Linux or any C++20 compiler; from framework/):
//   g++ -std=c++20 -O1 -Wall -o confirm_replay test/confirm_replay.cpp && ./confirm_replay test/p15-action-excerpt.log
// The fixture is P15's Cyberware.log, 21:49:40-21:50:15 (evidence/cw-int-4/p15-Cyberware.log): 3 ActionRightAttack, 3
// ActionLeftAttack and 3 ActionRightPowerAttack seam calls, all "queued". Each "cmd: action ... queued" line becomes an Expect at
// that log time, each "anim: <tag>" line an animation event, and the clock follows the log. Expected:
//   ActionRightPowerAttack  NEVER PLAYED x3   (the graph emitted nothing: the P15 finding, ARMS)
//   ActionRightAttack       played x3, on weaponSwing, under 300 ms
//   ActionLeftAttack        NEVER PLAYED x3   (unarmed left = a block: attackRelease, Retract, blockEnd, no swing)
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ---- stubs for what the plugin gets from PCH.h
static std::uint64_t g_now = 0;
inline std::uint64_t GetTickCount64() { return g_now; }
static std::vector<std::string> g_out;
namespace detail {
    inline void Fmt(std::ostringstream& o, const char* f) { o << f; }
    template <class T, class... R> void Fmt(std::ostringstream& o, const char* f, T&& v, R&&... r)
    {
        const char* p = std::strstr(f, "{}");
        if (!p) { o << f; return; }
        o.write(f, p - f);
        o << v;
        Fmt(o, p + 2, std::forward<R>(r)...);
    }
}
namespace REX {
    template <class... A> void INFO(const char* f, A&&... a) { std::ostringstream o; detail::Fmt(o, f, std::forward<A>(a)...); g_out.push_back(o.str()); }
    template <class... A> void WARN(const char* f, A&&... a) { INFO(f, std::forward<A>(a)...); }
}
#include <cstring>
#include "../plugin/cw_confirm.cpp"

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const std::string& what, const std::string& detail = "")
{
    if (ok) { ++g_pass; std::cout << "PASS " << what << "\n"; }
    else { ++g_fail; std::cout << "FAIL " << what << (detail.empty() ? "" : "  -- " + detail) << "\n"; }
}
static int Count(const std::string& needle)
{
    int n = 0;
    for (auto& l : g_out) if (l.find(needle) != std::string::npos) ++n;
    return n;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: confirm_replay <Cyberware.log excerpt>\n"; return 2; }
    std::ifstream in(argv[1]);
    if (!in) { std::cerr << "cannot read " << argv[1] << "\n"; return 2; }
    int actions = 0, events = 0;
    for (std::string line; std::getline(in, line);) {
        int hh, mm, ss, ms;
        if (line.size() < 14 || std::sscanf(line.c_str(), "[%d:%d:%d.%d]", &hh, &mm, &ss, &ms) != 4) continue;
        g_now = (std::uint64_t)(((hh * 60 + mm) * 60 + ss) * 1000 + ms);
        confirm::Tick(g_now);
        const auto at = line.find("] [I] ");
        if (at == std::string::npos) continue;
        const std::string msg = line.substr(at + 6);
        if (msg.rfind("cmd: action '", 0) == 0 && msg.find("queued") != std::string::npos) {
            const auto q = msg.find('\'', 13);
            confirm::Expect(msg.substr(13, q - 13));
            ++actions;
        } else if (msg.rfind("anim: ", 0) == 0) {
            const std::string rest = msg.substr(6);
            confirm::OnAnimEvent(rest.substr(0, rest.find(' ')).c_str());
            ++events;
        }
    }
    g_now += 20000;
    confirm::Tick(g_now);
    for (auto& l : g_out) if (l.find("waiting up to") == std::string::npos) std::cout << "  " << l << "\n";
    Check(actions == 9, "fixture has the 9 queued P15 actions", std::to_string(actions));
    Check(events > 40, "fixture has the graph events", std::to_string(events));
    Check(Count("confirm: ActionRightPowerAttack NEVER PLAYED") == 3, "P15 power attack x3: NEVER PLAYED (not 'queued')");
    Check(Count("confirm: ActionRightPowerAttack played") == 0, "no power attack is reported played");
    Check(Count("confirm: ActionRightAttack played (weaponSwing after") == 3, "P15 right attack x3: played on weaponSwing");
    Check(Count("confirm: ActionLeftAttack NEVER PLAYED") == 3, "P15 left attack x3: NEVER PLAYED (unarmed left ran a block, no swing)");
    bool fast = true;
    for (auto& l : g_out)
        if (l.find("ActionRightAttack played (weaponSwing after ") != std::string::npos) {
            const int t = std::atoi(l.c_str() + l.find("after ") + 6);
            if (t <= 0 || t > 300) fast = false;
        }
    Check(fast, "right attacks confirmed within 300 ms of the call");
    // the override and the weak default, on a synthetic stream
    g_out.clear(); g_now = 100000;
    confirm::Expect("x1-power", "attackPowerStart", 800);
    g_now += 200; confirm::OnAnimEvent("weaponSwing");
    g_now += 700; confirm::Tick(g_now);
    Check(Count("confirm: x1-power NEVER PLAYED") == 1 && Count("graph events seen: weaponSwing") == 1, "override: a swing that is not the power attack is NEVER PLAYED and named");
    confirm::Expect("SomethingElse");
    g_now += 50; confirm::OnAnimEvent("FootLeft");
    Check(Count("confirm: SomethingElse played (FootLeft after 50 ms, weak: any graph event)") == 1, "an unknown action is played on any event, marked weak");
    confirm::Expect("ActionDraw"); g_now += 300; confirm::OnAnimEvent("WEAPONDRAW");
    Check(Count("confirm: ActionDraw played (WEAPONDRAW") == 1, "tags match case-insensitively");
    // MANTIS MOTION's rules for the real power attack (ActionMelee) and the leap
    g_out.clear();
    confirm::Expect("ActionMelee"); g_now += 600; confirm::OnAnimEvent("meleeattackPowerStart");
    Check(Count("confirm: ActionMelee played (meleeattackPowerStart after 600 ms)") == 1, "ActionMelee: played on meleeattackPowerStart");
    confirm::Expect("ActionMelee");
    for (const char* t : { "weaponSwing", "preHitFrame", "HitFrame" }) { g_now += 150; confirm::OnAnimEvent(t); }
    g_now += 1200; confirm::Tick(g_now);   // past the 1500 ms window: the swing extended it for the late AttackStop
    confirm::OnAnimEvent("AttackStop");
    Check(Count("confirm: ActionMelee played (by the clip: weaponSwing then AttackStop with no attack window") == 1, "ActionMelee: no power tag, but the power clip's annotations (no attack window) = played");
    confirm::Expect("ActionMelee");
    g_now += 120; confirm::OnAnimEvent("weaponSwing"); g_now += 900; confirm::OnAnimEvent("attackWinStart");
    Check(Count("confirm: ActionMelee NEVER PLAYED (a light swing played instead") == 1, "ActionMelee: weaponSwing then attackWinStart = a light swing, NEVER PLAYED");
    confirm::Expect("ActionRightPowerAttack"); g_now += 1600; confirm::Tick(g_now);
    Check(Count("only plays in VATS, the power attack is ActionMelee") == 1, "ActionRightPowerAttack outside VATS says why");
    confirm::Expect("MantisLeap"); g_now += 140; confirm::OnAnimEvent("weaponSwing"); g_now += 20; confirm::Mark("leapLaunch");
    Check(Count("confirm: MantisLeap played (@leapLaunch after 160 ms)") == 1, "leap: confirmed by the plugin's leap launch marker, not by weaponSwing");
    confirm::Expect("GorillaSlam"); g_now += 3100; confirm::Tick(g_now);
    Check(Count("confirm: GorillaSlam NEVER PLAYED (none of @slamImpact") == 1, "slam: no impact marker in 3 s = NEVER PLAYED");
    std::cout << g_pass << " of " << (g_pass + g_fail) << " checks passed\n";
    return g_fail ? 1 : 0;
}
