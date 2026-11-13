// cw_confirm.cpp - test seam actions wait for proof that they played (TEST HARNESS, 2026-11-10). ASCII only.
//
// Why (ARMS, P15): all four {"cmd":"action","edid":"ActionRightPowerAttack"} calls logged "queued" and the graph produced no
// event at all; a buffered click never reached the input sink either. "queued" / "accepted" only means the call was taken.
// Now an action is PLAYED only when the player's own animation graph emits one of its confirming events in time:
//   confirm: <name> played (<tag> after <n> ms) sent_unix=<ms>
//   confirm: <name> NEVER PLAYED (none of <tags> within <n> ms; graph events seen: <first 8 or none>) sent_unix=<ms>
// cw-framecheck.ps1 marks a clip VOID when a NEVER PLAYED's sent_unix falls inside it, so such a row is never graded PASS or FAIL.
// Default confirming tags (any one = played; a test overrides per call with "expect":"a|b","ms":n). From MANTIS MOTION 4ef7fd17
// (2026-11-10, read from Fallout4.esm's IDLE tree and our clips' hkx annotations); ARMS owns the leap/slam log lines:
//   ActionMelee / *PowerAttack*  POWER mode: meleeattackPowerStart|attackPowerStart within 1500 ms = played. If neither arrives,
//                      the clip's own annotations decide: weaponSwing then AttackStop with NO attackWinStart/initiateStartA/B
//                      before it = the power clip (played); weaponSwing then attackWinStart = a light swing (NEVER PLAYED, named).
//                      In 1P ActionRightPowerAttack only selects a clip in VATS (GetVATSMode == 1): outside VATS NEVER PLAYED is
//                      the correct answer, and the line says so. The player's power attack is ActionMelee (Mouse 4).
//   *Leap*             @leapLaunch (the plugin's "arms: leap launch" line)  1700 ms; "arms: leap strike" follows on landing
//   *Slam*             @slamImpact (the plugin's "arms: slam impact" line)  3000 ms (first guess: ARMS to confirm)
//   *Attack* / *Bash*  weaponSwing|bashStart|bashRelease     1200 ms
//   *Draw*             weaponDraw                            2500 ms
//   *Sheathe*/*Holster* weaponSheathe                        2500 ms
//   anything else      any player graph event                1500 ms   (weak: the line says so)
// "@name" tags are plugin markers (confirm::Mark), not graph events: the leap and slam have no graph event of their own.
// Read-only: it only watches events the arms hook already receives. No input, no Papyrus.
#include "cw_confirm.h"

#if defined(_WIN32)
#include <Windows.h>   // GetTickCount64 (the Linux replay test, test/confirm_replay.cpp, stubs it)
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

namespace confirm
{
    namespace
    {
        struct Pending
        {
            std::string name, tagsText;
            std::vector<std::string> tags;   // lower case; empty = any event
            std::uint64_t sentTick = 0, deadline = 0;
            std::int64_t sentUnix = 0;
            std::vector<std::string> seen;   // first 8 tags after it was sent, for the NEVER PLAYED line
            bool done = false, power = false, swung = false, lightWin = false;
        };
        std::mutex g_mx;
        std::vector<Pending> g_pending;

        std::string Lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
        bool Has(const std::string& hay, const char* needle) { return Lower(hay).find(needle) != std::string::npos; }
        std::uint64_t Tick64() { return GetTickCount64(); }
        std::int64_t UnixMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

        void Defaults(const std::string& name, std::string& tags, int& ms)
        {
            if (Has(name, "powerattack") || Lower(name) == "actionmelee") { tags = "meleeattackPowerStart|attackPowerStart"; ms = 1500; }
            else if (Has(name, "leap")) { tags = "@leapLaunch"; ms = 1700; }
            else if (Has(name, "slam")) { tags = "@slamImpact"; ms = 3000; }
            else if (Has(name, "attack") || Has(name, "bash")) { tags = "weaponSwing|bashStart|bashRelease"; ms = 1200; }
            else if (Has(name, "draw")) { tags = "weaponDraw"; ms = 2500; }
            else if (Has(name, "sheathe") || Has(name, "holster")) { tags = "weaponSheathe"; ms = 2500; }
            else { tags = ""; ms = 1500; }
        }
    }

    void Expect(const std::string& name, const std::string& tagsIn, int msIn)
    {
        std::string tags = tagsIn;
        int ms = msIn;
        std::string dTags; int dMs = 0;
        Defaults(name, dTags, dMs);
        if (tags.empty()) tags = dTags;
        if (ms <= 0) ms = dMs;
        ms = std::clamp(ms, 100, 15000);
        Pending p;
        p.name = name.empty() ? std::string("action") : name;
        p.tagsText = tags.empty() ? std::string("any graph event") : tags;
        for (size_t a = 0; a <= tags.size() && !tags.empty();) {
            const size_t b = tags.find('|', a);
            const std::string t = Lower(tags.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (!t.empty()) p.tags.push_back(t);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        p.power = Has(p.name, "powerattack") || Lower(p.name) == "actionmelee";
        p.sentTick = Tick64(); p.deadline = p.sentTick + (std::uint64_t)ms; p.sentUnix = UnixMs();
        REX::INFO("confirm: {} waiting up to {} ms for {} sent_unix={}", p.name, ms, p.tagsText, p.sentUnix);
        std::scoped_lock l(g_mx);
        g_pending.push_back(std::move(p));
    }

    void OnAnimEvent(const char* tag)
    {
        if (!tag || !*tag) return;
        std::scoped_lock l(g_mx);
        if (g_pending.empty()) return;
        const std::string t = Lower(tag);
        const auto now = Tick64();
        for (auto& p : g_pending) {
            if (p.done) continue;
            const bool match = p.tags.empty() || std::find(p.tags.begin(), p.tags.end(), t) != p.tags.end();
            if (!match && p.power) {
                // MANTIS's fallback, from the clips' annotations: the power clip never opens an attack window, the light swings do
                if (t == "weaponswing") { p.swung = true; p.deadline = std::max(p.deadline, now + 2500); }   // its AttackStop comes late
                else if (p.swung && (t == "attackwinstart" || t == "initiatestarta" || t == "initiatestartb")) {
                    p.done = true;
                    REX::WARN("confirm: {} NEVER PLAYED (a light swing played instead: weaponSwing then {} after {} ms) sent_unix={}", p.name, tag, now - p.sentTick, p.sentUnix);
                    continue;
                } else if (p.swung && t == "attackstop") {
                    p.done = true;
                    REX::INFO("confirm: {} played (by the clip: weaponSwing then AttackStop with no attack window, {} ms) sent_unix={}", p.name, now - p.sentTick, p.sentUnix);
                    continue;
                }
            }
            if (match) {
                p.done = true;
                REX::INFO("confirm: {} played ({} after {} ms{}) sent_unix={}", p.name, tag, now - p.sentTick, p.tags.empty() ? ", weak: any graph event" : "", p.sentUnix);
            } else if (p.seen.size() < 8) {
                p.seen.emplace_back(tag);
            }
        }
        g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [](const Pending& p) { return p.done; }), g_pending.end());
    }

    void Mark(const char* name)
    {
        if (!name || !*name) return;
        OnAnimEvent((std::string("@") + name).c_str());
    }

    void Tick(std::uint64_t)
    {
        const auto now = Tick64();
        std::scoped_lock l(g_mx);
        for (auto& p : g_pending) {
            if (p.done || now < p.deadline) continue;
            p.done = true;
            std::string seen;
            for (size_t i = 0; i < p.seen.size(); ++i) seen += (i ? "," : "") + p.seen[i];
            const bool vatsOnly = Lower(p.name) == "actionrightpowerattack" || Lower(p.name) == "actionleftpowerattack";
            REX::WARN("confirm: {} NEVER PLAYED (none of {} within {} ms; graph events seen: {}{}) sent_unix={}", p.name, p.tagsText, p.deadline - p.sentTick,
                      seen.empty() ? std::string("none") : seen, vatsOnly ? "; in 1P this action only plays in VATS, the power attack is ActionMelee" : "", p.sentUnix);
        }
        g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [](const Pending& p) { return p.done; }), g_pending.end());
    }
}
