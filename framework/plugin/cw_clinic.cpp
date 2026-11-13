// cw_clinic.cpp - Varga's daily schedule mover (CLINIC stream; rework 2026-11-09, see clinic/PHASE2-QUALITY.md).
//
// Her packages (Cyberware - Clinic.esp) do the day inside the clinic on foot: bed 0-6, office chair 6-10,
// surgical station 14-24, each conditioned GetInCell so they only run while she is home. What the packages
// cannot do is cross the clinic's load door, because the new door has no navmesh info link (that needs a
// global NAVI edit, which conflicts with every mod touching it). This mover makes that one crossing:
//   10:00-14:00 she belongs in the Diamond City market, the rest of the day in the clinic.
//
// She only ever crosses unseen. A move waits while she is loaded (the player may be looking at her), and
// while the player stands at the destination (inside the clinic, or within kMarketClear of the market anchor).
// A waiting move retries every 5 s until the conditions clear, and if the window ends first the target
// simply changes. There is no state in the save: the engine saves her position and each tick compares her
// actual side with the side the clock wants, so save, reload and a mid-window arrival need no special code.
// Gated by [Clinic] bSchedule: OFF in code and Cyberware.ini, ON in Cyberware.Test.ini until it earns promotion.
// With bSchedule=0 the package NUDGE (below) still runs while she is in the clinic; only the moves are gated.
//
// Package nudge (P14, 2026-11-09): the engine does not re-pick her package after an unseen MoveTo, a cell load or a
// `set gamehour` jump while an open-ended sandbox runs. P14 logged CW_ClinicOutside (the market fallback) running for
// four minutes in the clinic across hours 15, 7, 15 and 1; it changed only when her cell unloaded. So the mover works
// out which of OUR six packages the clock and her side want and, when she runs a different one of ours, asks the
// engine to evaluate (`<ref>.evp`): right after every move, then at most every kNudgeMs, kMaxNudges times per wanted
// package (a WARN names a package that still will not start). It never touches an engine or quest package (dialogue,
// combat, follow), never runs while the ripperdoc talk is open, and never while she is in combat.
//
// Stall check (P16, the lead): a Varga who silently stands still is invisible in play, so while she is loaded in
// the clinic and runs the package she should, the DLL compares her position with that package's target (bed, office
// chair, menu chair). Outside the target radius for kStallMs straight -> one WARN with the package, hour and distance.
// Log only, no corrective action. It runs whether or not the mover is on ([Clinic] bSchedule only gates moves; the nudge runs in the clinic either way).
#include "cw_clinic.h"
#include "../core/cw_clinicguard.h"
#include "../core/cw_evpgate.h"   // the one rule every <ref>.evp obeys: never on an actor whose 3D is away

#include <cmath>
#include <cstdio>
#include <string>

namespace clinic
{
    namespace
    {
        enum class Side { Clinic, Market, Elsewhere };
        const char* SideName(Side s) { return s == Side::Clinic ? "clinic" : s == Side::Market ? "market" : "elsewhere"; }

        constexpr float kMarketStart = 10.0f, kMarketEnd = 14.0f;
        constexpr float kMarketClear = 2000.0f;        // the player this close to the market anchor could see her arrive
        constexpr std::uint64_t kPollMs = 5000, kBackoffMs = 60000;
        // CW_ClinicChair, a record only the --phase2 esp has (id locked in clinic.lock.json). Without it she still
        // lives in Crocker's basement, so moving her would strand her (cw-int-4 boot 2).
        constexpr std::uint32_t kPhase2OnlyRef = 0x970;
        // Her packages, ids locked in clinic.lock.json (regeneration never renumbers). NPC_ list order = first match:
        // Sleep 0-6, Office 6-10, Shop 14-24 (GetInCell clinic == 1), Walk 10-14 (== 0), IndoorHold (== 1, near the chair,
        // the indoor last resort, P15), Outside (== 0). So in the room nothing can sandbox wider than 150 u of the chair.
        constexpr std::uint32_t kPkgSleep = 0x96E, kPkgOffice = 0x973, kPkgWalk = 0x974, kPkgShop = 0x975, kPkgOutside = 0xA09, kPkgHold = 0xA4C;
        // INSTALL SCENE step-in (2026-11-10): FIRST in her list, GetGlobalValue CW_ClinicSceneActive == 1 AND in the
        // clinic -> she walks to CW_ClinicSurgeonMark (A4F) and holds. cw_scene.cpp WRITES the global (1 at commit + evp, 0 at the
        // end, on abort and on load); this file only reads it, so the nudge never pulls her off her mark mid-scene.
        constexpr std::uint32_t kPkgStepIn = 0xA51, kGlobSceneActive = 0xA50;
        constexpr std::uint32_t kOurPackages[] = { kPkgSleep, kPkgOffice, kPkgWalk, kPkgShop, kPkgOutside, kPkgHold, kPkgStepIn };
        constexpr std::uint64_t kNudgeMs = 15000;
        constexpr int kMaxNudges = 3;

        // Package names shared with the HARNESS package seam: "<runtime formid, 8 hex> <EditorID>".
        const char* PackageName(std::uint32_t local)
        {
            switch (local) {
            case kPkgSleep: return "CW_ClinicSleep0to6";     case kPkgOffice: return "CW_ClinicOffice6to10";
            case kPkgWalk: return "CW_ClinicWalk10to14";     case kPkgShop: return "CW_ClinicShop14to24";
            case kPkgOutside: return "CW_ClinicOutside";     case kPkgHold: return "CW_ClinicIndoorHold";
            case kPkgStepIn: return "CW_ClinicStepIn";
            default: return "?";
            }
        }
        // Stall targets for the in-clinic packages: ref local ids locked in clinic.lock.json (verify_clinic checks them).
        // Radius: bed 100 u (she lies on the mattress, her ref is within ~60 u of its centre); office chair 80 u (seated she
        // stands on the chair ref: P14 read exactly (2296,1900)); station and hold 180 u = the packages' sandbox radius 150
        // around the menu chair + 30 u for her capsule and the stool at the edge. The market packages sandbox near herself:
        // no fixed target, no stall check.
        struct StallTarget { std::uint32_t pkg, ref; float radius; const char* refName; };
        constexpr StallTarget kStallTargets[] = {
            { kPkgSleep, 0x972, 100.0f, "CW_ClinicBed" },
            { kPkgOffice, 0x89A, 80.0f, "CW_ClinicOfficeChair" },
            { kPkgShop, 0x970, 180.0f, "CW_ClinicChair" },
            { kPkgHold, 0x970, 180.0f, "CW_ClinicChair" },
        };
        // N: the longest in-clinic walk is 836 u (clinic navmesh, far corner to the bed, by triangle edges), 12 s at a
        // conservative 70 u/s walk; + 5 s poll, + ~5 s for the engine to start the package, + ~5 s sit/lie animation;
        // rounded up to 30 s. A stall WARN therefore lands 30-35 s after she starts the package (poll granularity).
        constexpr std::uint64_t kStallMs = 30000;
        std::uint32_t g_stallPkg = 0;       // local id the timer belongs to
        std::uint64_t g_stallSince = 0;     // last poll she was within the target (or the timer restarted)
        bool g_stallWarned = false;
        std::uint64_t g_lastTick = 0;      // the latest Tick's clock (StatusLine has none of its own)
        float g_stallDist = -1.0f;          // last measured distance, for StatusLine

        struct Anchor { bool ok = false; float x = 0, y = 0, z = 0, rz = 0; };
        struct Cfg
        {
            bool enabled = false;
            std::string vargaPlugin;        // from formmap after load
            std::uint32_t vargaId = 0;
            bool watch = false;             // the stall check: on whenever the clinic cell ref parses, mover or not
            std::string clinicPlugin;       // sClinicCell "Plugin.esp|800"
            std::uint32_t clinicCellId = 0;
            std::string marketPlugin;       // sMarketWorldspace "Fallout4.esm|F94"
            std::uint32_t marketWorldId = 0;
            Anchor clinic, market;          // where she lands on each side of the door
        } g_cfg;

        std::uint64_t g_nextAt = 0;
        std::string g_waitReason;           // why the pending move is waiting; logged on change only
        int g_moves = 0;

        bool ParseRef(const std::string& s, std::string& plugin, std::uint32_t& id)
        {
            const auto bar = s.find('|');
            if (bar == std::string::npos) return false;
            plugin = s.substr(0, bar);
            id = (std::uint32_t)std::strtoul(s.c_str() + bar + 1, nullptr, 16) & 0x00FFFFFF;
            return !plugin.empty() && id != 0;
        }

        Anchor ParseAnchor(const std::string& s)
        {
            Anchor a;
            if (std::sscanf(s.c_str(), "%f,%f,%f,%f", &a.x, &a.y, &a.z, &a.rz) == 4) a.ok = true;
            return a;
        }

        Side WantedSide(float hour) { return (hour >= kMarketStart && hour < kMarketEnd) ? Side::Market : Side::Clinic; }

        // worldSpace shares a union with an interior's tempDataOffset, so read it only on exterior cells.
        RE::TESWorldSpace* WorldOf(RE::TESObjectCELL* cell) { return (cell && cell->IsExterior()) ? cell->worldSpace : nullptr; }

        Side SideOf(RE::TESObjectREFR* ref, RE::TESObjectCELL* clinicCell, RE::TESWorldSpace* market)
        {
            auto* cell = ref->GetParentCell();
            if (!cell) return Side::Elsewhere;
            if (cell == clinicCell) return Side::Clinic;
            if (auto* w = WorldOf(cell); w && w == market) return Side::Market;
            return Side::Elsewhere;
        }

        // Her running AI package, for the P12 finding (she left the station at hour 15). Read-only.
        std::uint32_t RunningPackage(RE::TESObjectREFR* ref)
        {
            auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
            if (!actor || !actor->currentProcess) return 0;
            auto* pkg = actor->currentProcess->currentPackage.package;
            return pkg ? pkg->GetFormID() : 0;
        }
        std::uint32_t g_lastPkg = 0xFFFFFFFF;

        // Which of her packages should be running now, as a local id (0 = no opinion: she is elsewhere).
        std::uint32_t WantedPackage(float hour, Side have)
        {
            const bool marketHours = hour >= kMarketStart && hour < kMarketEnd;
            if (have == Side::Clinic)                                          // 10-14 indoors = the mover is waiting: hold
                return marketHours ? kPkgHold : hour < 6.0f ? kPkgSleep : hour < kMarketStart ? kPkgOffice : kPkgShop;
            if (have == Side::Market) return marketHours ? kPkgWalk : kPkgOutside;   // station packages need the clinic cell
            return 0;
        }

        // The install scene holds her on the step-in package while its global is 1. A missing global reads 0: the package cannot
        // pass without it, so nothing of ours is holding her there.
        bool SceneActive(RE::TESDataHandler* dh)
        {
            auto* form = dh->LookupForm(kGlobSceneActive, g_cfg.clinicPlugin);
            auto* glob = form ? form->As<RE::TESGlobal>() : nullptr;
            return glob && glob->value > 0.5f;
        }

        std::uint32_t g_nudgeFor = 0;       // the wanted package the nudge count belongs to
        int g_nudges = 0;
        cw::clinicguard::NudgeLive g_nudgeLive;   // was her 3D loaded last look (the nudge's loaded gate)
        bool g_unloadedLogged = false;            // the "not loaded" line is once per unload, not every poll
        std::uint64_t g_nextNudge = 0;

        // GATED (2026-11-12, the navmesh-crash attribution). The 2026-11-10 fix put the "is she loaded" check in
        // Nudge() and nowhere else -- but the MOVER's post-move evp below (Tick, "evaluate package" after the
        // MoveRefToNewSpace) is reached ONLY when she is NOT loaded: Tick returns at `if (ref->Get3D())` a dozen lines
        // above it, which is what "unseen" in that log line means. So the file that documented the law was still
        // calling evp on an unloaded actor every time the mover fired. The gate belongs in this helper, where every
        // caller present and future passes it, and where the 3D state is read at the instant of the call.
        // Skipping is safe: a stale package on an unloaded actor is invisible, and the nudge re-arms on her
        // unloaded -> loaded edge and evaluates her onto the clock's package then (core/cw_evpgate.h has the full
        // reasoning). When a move lands in a cell that IS loaded her 3D attaches and the evp goes through as before.
        void Evaluate(RE::TESObjectREFR* ref)
        {
            const auto v = cw::evpgate::Check(ref != nullptr, ref && ref->Get3D() != nullptr);
            if (!cw::evpgate::Allowed(v)) {
                if (ref)
                    REX::INFO("clinic: no package evaluation on {:08X} - {}", ref->GetFormID(), cw::evpgate::Why(v));
                return;
            }
            char cmd[32];
            std::snprintf(cmd, sizeof(cmd), "%08X.evp", ref->GetFormID());
            RE::Console::ExecuteCommand(cmd);
        }

        void Nudge(RE::TESObjectREFR* ref, RE::TESDataHandler* dh, std::uint32_t running, float hour, Side have, bool talking, std::uint64_t now)
        {
            const std::uint32_t want = WantedPackage(hour, have);
            auto* actor = ref->As<RE::Actor>();
            if (!want || !actor || !actor->currentProcess || talking || actor->IsInCombat()) return;
            // SHE MUST BE LOADED. Without this the nudge asked the engine to pick a package -- and so to BUILD A PATH --
            // for an actor whose 3D, cell and navmesh are not in memory. StallCheck below has always had this check; the
            // nudge never did. See core/cw_clinicguard.h for the measurement that found it.
            {
                const auto gate = cw::clinicguard::ObserveLive(g_nudgeLive, ref->Get3D() != nullptr);
                if (gate.skip) {
                    if (!g_unloadedLogged) {   // once per unload, not every poll
                        g_unloadedLogged = true;
                        REX::INFO("clinic: she is not loaded - no package evaluation while her 3D is away (it would path an unloaded actor)");
                    }
                    g_nudgeFor = 0; g_nudges = 0; g_nextNudge = 0;
                    return;
                }
                if (gate.rearm) {
                    g_unloadedLogged = false;
                    g_nudgeFor = 0; g_nudges = 0; g_nextNudge = 0;   // her 3D just appeared: a stale package may now show
                    REX::INFO("clinic: her 3D loaded - the package nudge is armed again");
                }
            }
            auto* wantForm = dh->LookupForm(want, g_cfg.clinicPlugin);
            if (!wantForm) return;
            if (running == wantForm->GetFormID()) { g_nudgeFor = want; g_nudges = 0; return; }
            bool ours = running == 0;
            for (const auto id : kOurPackages)
                if (auto* f = dh->LookupForm(id, g_cfg.clinicPlugin); f && f->GetFormID() == running) ours = true;
            if (!ours) return;                                                // dialogue, combat, a quest: not ours to change
            // the scene's step-in: hers to hold while the global is 1. Once the scene writes 0 she still runs it until the next
            // pick, and the nudge below evaluates her back onto the clock's package.
            if (auto* f = dh->LookupForm(kPkgStepIn, g_cfg.clinicPlugin); f && f->GetFormID() == running && SceneActive(dh)) {
                g_nudgeFor = 0; g_nudges = 0; g_nextNudge = 0;
                return;
            }
            if (want != g_nudgeFor) { g_nudgeFor = want; g_nudges = 0; g_nextNudge = 0; }
            if (now < g_nextNudge) return;
            if (g_nudges >= kMaxNudges) {
                if (g_nudges == kMaxNudges) {
                    ++g_nudges;
                    REX::WARN("clinic: hour {:.2f} wants package {:08X} but she still runs {:08X} after {} evaluations; check its conditions and target",
                              hour, wantForm->GetFormID(), running, kMaxNudges);
                }
                return;
            }
            ++g_nudges;
            g_nextNudge = now + kNudgeMs;
            REX::INFO("clinic: hour {:.2f} she is {}, wants package {:08X}, runs {:08X}: evaluate package (nudge {} of {})",
                      hour, SideName(have), wantForm->GetFormID(), running, g_nudges, kMaxNudges);
            Evaluate(ref);
            // Written AFTER the call on purpose. The line above is written BEFORE it, so a crash inside the engine's
            // package evaluation leaves the "nudge N of 3" line and no completion line. If a log ever shows the first
            // without the second, the fault is inside EvaluatePackage, not in our bookkeeping around it.
            REX::INFO("clinic: nudge {} evaluated and returned", g_nudges);
        }

        void StallCheck(RE::TESObjectREFR* ref, RE::TESDataHandler* dh, std::uint32_t running, float hour, Side have, bool talking, std::uint64_t now)
        {
            auto restart = [&](std::uint32_t pkg) { g_stallPkg = pkg; g_stallSince = now; g_stallWarned = false; g_stallDist = -1.0f; };
            const std::uint32_t want = WantedPackage(hour, have);
            const StallTarget* tgt = nullptr;
            for (const auto& t : kStallTargets) if (t.pkg == want) tgt = &t;
            auto* actor = ref->As<RE::Actor>();
            auto* wantForm = (tgt && have == Side::Clinic) ? dh->LookupForm(want, g_cfg.clinicPlugin) : nullptr;
            if (!wantForm || !ref->Get3D() || !actor || talking || actor->IsInCombat() || running != wantForm->GetFormID()) { restart(0); return; }
            if (g_stallPkg != want) restart(want);                         // she just started it: the clock starts now
            auto* tform = dh->LookupForm(tgt->ref, g_cfg.clinicPlugin);
            auto* tref = tform ? tform->As<RE::TESObjectREFR>() : nullptr;
            if (!tref) { g_stallSince = now; return; }                     // target not resolvable this poll: no verdict
            const auto p = ref->GetPosition(), q = tref->GetPosition();
            g_stallDist = std::hypot(p.x - q.x, p.y - q.y);
            if (g_stallDist <= tgt->radius) { g_stallSince = now; g_stallWarned = false; return; }
            if (g_stallWarned || now - g_stallSince < kStallMs) return;
            g_stallWarned = true;
            REX::WARN("clinic: STALL hour {:.2f} she runs {:08X} {} for {} s but stands {:.0f} u from {} (radius {:.0f}) at ({:.0f},{:.0f}); log only",
                      hour, wantForm->GetFormID(), PackageName(want), (now - g_stallSince) / 1000, g_stallDist, tgt->refName, tgt->radius, p.x, p.y);
        }

        void Wait(const std::string& why, float hour, Side want)
        {
            if (why == g_waitReason) return;
            g_waitReason = why;
            REX::INFO("clinic: mover hour {:.1f} wants {}, WAITING: {}", hour, SideName(want), why);
        }
    }

    void Configure(bool enabled, const std::string& clinicCell, const std::string& marketWorldspace,
                   const std::string& clinicAnchor, const std::string& marketAnchor)
    {
        g_cfg.enabled = enabled;
        g_cfg.watch = ParseRef(clinicCell, g_cfg.clinicPlugin, g_cfg.clinicCellId);
        if (!g_cfg.watch) g_cfg.enabled = false;
        if (!ParseRef(marketWorldspace, g_cfg.marketPlugin, g_cfg.marketWorldId)) g_cfg.enabled = false;
        g_cfg.clinic = ParseAnchor(clinicAnchor);
        g_cfg.market = ParseAnchor(marketAnchor);
        if (!(g_cfg.clinic.ok && g_cfg.market.ok)) g_cfg.enabled = false;
        if (enabled && !g_cfg.enabled) REX::WARN("clinic: [Clinic] bSchedule=1 but an anchor/ref failed to parse; mover OFF");
    }

    void SetVargaRef(const std::string& plugin, std::uint32_t localId)
    {
        g_cfg.vargaPlugin = plugin;
        g_cfg.vargaId = localId & 0x00FFFFFF;
    }

    void Tick(std::uint64_t now, bool talking)
    {
        if (!(g_cfg.enabled || g_cfg.watch) || g_cfg.vargaId == 0 || now < g_nextAt) return;
        g_nextAt = now + kPollMs;
        g_lastTick = now;

        auto* cal = RE::Calendar::GetSingleton();
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!cal || !cal->gameHour || !dh || !player) return;
        auto* playerCell = player->GetParentCell();
        if (!playerCell || !player->Get3D()) return;     // no world yet: main menu or mid-load
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->GetMenuOpen(RE::BSFixedString("LoadingMenu"))) return;

        auto* form = dh->LookupForm(g_cfg.vargaId, g_cfg.vargaPlugin);
        auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
        auto* cform = dh->LookupForm(g_cfg.clinicCellId, g_cfg.clinicPlugin);
        auto* clinicCell = cform ? cform->As<RE::TESObjectCELL>() : nullptr;
        auto* wform = dh->LookupForm(g_cfg.marketWorldId, g_cfg.marketPlugin);
        auto* market = wform ? wform->As<RE::TESWorldSpace>() : nullptr;
        if (!ref || !clinicCell || !market) {
            REX::WARN("clinic: mover and stall check OFF, a form is missing (Varga {}, clinic cell {}, market {})", ref != nullptr, clinicCell != nullptr, market != nullptr);
            g_cfg.enabled = false; g_cfg.watch = false;
            return;
        }
        if (!dh->LookupForm(kPhase2OnlyRef, g_cfg.clinicPlugin)) {
            REX::INFO("clinic: mover and stall check OFF, {} is a Phase 1 build (no Varga in the clinic)", g_cfg.clinicPlugin);
            g_cfg.enabled = false; g_cfg.watch = false;
            return;
        }
        if (auto* actor = ref->As<RE::Actor>(); actor && actor->lifeState != 0) return;   // never relocate a body

        const float hour = cal->gameHour->value;
        const Side want = WantedSide(hour);
        const Side have = SideOf(ref, clinicCell, market);
        const auto pk = RunningPackage(ref);
        if (pk != g_lastPkg) {
            g_lastPkg = pk;
            const auto pp = ref->GetPosition();
            // SAYS "OBSERVED" ON PURPOSE (2026-11-12). This line is a pure READ of whatever package the engine is
            // running, and it fires when THE ENGINE re-picks -- we did nothing. Its old wording ("clinic: package now
            // ...") was indistinguishable from a line reporting one of our own writes, and on 2026-11-11 four
            // sessions read it as proof that the nudge had evaluated a package nine seconds before a navmesh crash.
            // It had not: no nudge line precedes it (Nudge logs BEFORE it calls Evaluate, by design, below), she was
            // on a Fallout4.esm package the nudge refuses as "not ours", and the mover was off. A whole day's top
            // suspect came from one ambiguous log line. RULE FOR THIS PLUGIN: a line reporting state we merely
            // OBSERVED says so in that word; a line reporting something WE DID is phrased as the action.
            REX::INFO("clinic: OBSERVED package {:08X} at hour {:.2f} (the engine's own pick, not ours), she is {} at ({:.0f},{:.0f},{:.0f})",
                      pk, hour, SideName(have), pp.x, pp.y, pp.z);
        }
        StallCheck(ref, dh, pk, hour, have, talking, now);   // play and Test alike
        if (!g_cfg.enabled) {
            // [Clinic] bSchedule=0 (the player default): no moves, ever. But the engine does not re-pick an open-ended
            // package while she stays loaded (P14: a stale sandbox ran on across hours until her cell unloaded), so after a
            // wait, a sleep or an hour boundary crossed in the room she could keep a package the clock no longer wants (bed
            // or office chair at 15:00), and the install scene's ready-package guard would take the 2D beat. So the same
            // bounded nudge runs while she is home: only her own clinic packages, at most kMaxNudges evaluations per wanted
            // package, never in talk or combat, never off the scene step-in while CW_ClinicSceneActive is 1 (2026-11-10).
            if (have == Side::Clinic) Nudge(ref, dh, pk, hour, have, talking, now);
            return;
        }
        if (have == want) { g_waitReason.clear(); Nudge(ref, dh, pk, hour, have, talking, now); return; }
        if (ref->Get3D() || (want == Side::Clinic && playerCell == clinicCell)) Nudge(ref, dh, pk, hour, have, talking, now);  // she waits on this side

        if (ref->Get3D()) { Wait("she is loaded near the player", hour, want); return; }
        const Anchor& a = want == Side::Market ? g_cfg.market : g_cfg.clinic;
        if (want == Side::Clinic && playerCell == clinicCell) { Wait("player is inside the clinic", hour, want); return; }
        if (want == Side::Market && WorldOf(playerCell) == market) {
            const auto p = player->GetPosition();
            if (std::hypot(p.x - a.x, p.y - a.y) < kMarketClear) { Wait("player is near the market anchor", hour, want); return; }
        }

        if (want == Side::Market) ref->MoveRefToNewSpace(nullptr, market);
        else ref->MoveRefToNewSpace(clinicCell, nullptr);
        ref->SetLocationOnReference(RE::NiPoint3(a.x, a.y, a.z));
        ref->SetAngleOnReference(RE::NiPoint3(0.0f, 0.0f, a.rz));
        g_waitReason.clear();

        const Side after = SideOf(ref, clinicCell, market);
        const auto pos = ref->GetPosition();
        ++g_moves;
        if (after != want) {
            REX::WARN("clinic: mover hour {:.1f} {} -> {}: read-back says {} at ({:.0f},{:.0f},{:.0f}); backing off", hour, SideName(have), SideName(want), SideName(after), pos.x, pos.y, pos.z);
            g_nextAt = now + kBackoffMs;
            return;
        }
        REX::INFO("clinic: mover hour {:.1f} {} -> {} at ({:.0f},{:.0f},{:.0f}), unseen (move {}); asking for a package evaluation", hour, SideName(have), SideName(want), pos.x, pos.y, pos.z, g_moves);
        Evaluate(ref);                       // the engine keeps the old side's package after a MoveTo (P14). "Asking",
                                             // not "evaluate": the gate in Evaluate refuses while her 3D is away --
                                             // which, past the Get3D() return above, is the normal case for a move
                                             // that is "unseen". It then logs its own refusal, and the nudge picks her
                                             // up on the edge where she loads.
        g_nudgeFor = 0; g_nudges = 0; g_nextNudge = now + kPollMs;   // the next poll checks it took
    }

    // INSTALL SCENE (2026-11-10): the clock's opinion for the scene's ready guard (core/cw_scenepkg.h). The
    // engine does not re-pick her package while she stays loaded, and the nudge above never runs in a talk, so a player
    // who waits past 10:00 and talks at once finds her on the office package with the clock already at the station.
    // Returns the runtime FormID of the package her clock wants now while she is IN THE CLINIC (0 = no opinion: not
    // watched, a Phase 1 build, not Varga, she is not in the clinic), and sets hers = running is one of her own packages.
    // Read only: it moves nothing, nudges nothing and logs nothing. Works with bSchedule 0 and 1 alike.
    std::uint32_t DueInClinic(std::uint32_t actorFormId, std::uint32_t running, bool& hers)
    {
        hers = false;
        if (!g_cfg.watch || g_cfg.vargaId == 0) return 0;
        auto* cal = RE::Calendar::GetSingleton();
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!cal || !cal->gameHour || !dh) return 0;
        auto* form = dh->LookupForm(g_cfg.vargaId, g_cfg.vargaPlugin);
        auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
        if (!ref || ref->GetFormID() != actorFormId) return 0;
        auto* cform = dh->LookupForm(g_cfg.clinicCellId, g_cfg.clinicPlugin);
        auto* clinicCell = cform ? cform->As<RE::TESObjectCELL>() : nullptr;
        if (!clinicCell || ref->GetParentCell() != clinicCell) return 0;
        for (const auto id : kOurPackages)
            if (auto* f = dh->LookupForm(id, g_cfg.clinicPlugin); f && running && f->GetFormID() == running) hers = true;
        const auto wantLocal = WantedPackage(cal->gameHour->value, Side::Clinic);
        auto* wantForm = wantLocal ? dh->LookupForm(wantLocal, g_cfg.clinicPlugin) : nullptr;
        return wantForm ? wantForm->GetFormID() : 0;
    }

    std::string StatusLine()
    {
        if (!g_cfg.enabled && !g_cfg.watch) return "mover off, stall check off";
        auto* cal = RE::Calendar::GetSingleton();
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!cal || !cal->gameHour || !dh) return "no calendar";
        auto* form = dh->LookupForm(g_cfg.vargaId, g_cfg.vargaPlugin);
        auto* ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
        auto* cform = dh->LookupForm(g_cfg.clinicCellId, g_cfg.clinicPlugin);
        auto* wform = dh->LookupForm(g_cfg.marketWorldId, g_cfg.marketPlugin);
        if (!ref || !cform || !wform) return "forms missing";
        const float hour = cal->gameHour->value;
        const auto pos = ref->GetPosition();
        RE::NiPoint3 epos{}, erot{};
        RE::TESForm* eloc = nullptr;
        ref->GetEditorLocation(epos, erot, eloc, nullptr);
        const Side have = SideOf(ref, cform->As<RE::TESObjectCELL>(), wform->As<RE::TESWorldSpace>());
        const auto wantLocal = WantedPackage(hour, have);
        auto* wantForm = wantLocal ? dh->LookupForm(wantLocal, g_cfg.clinicPlugin) : nullptr;
        char buf[640];
        std::snprintf(buf, sizeof(buf), "hour %.2f wants %s, she is %s at (%.0f,%.0f,%.0f), 3D %s, moves %d, waiting '%s', package %08X (wants %08X %s, nudges %d), editor location %08X (%.0f,%.0f,%.0f), mover %s, stall %s %.0f u for %llu s%s",
                      hour, SideName(WantedSide(hour)), SideName(have),
                      pos.x, pos.y, pos.z, ref->Get3D() ? "loaded" : "unloaded", g_moves, g_waitReason.c_str(),
                      RunningPackage(ref), wantForm ? wantForm->GetFormID() : 0u, PackageName(wantLocal), g_nudges, eloc ? eloc->GetFormID() : 0u, epos.x, epos.y, epos.z,
                      g_cfg.enabled ? "on" : "off", g_stallPkg ? "timing" : "idle", g_stallDist,
                      (unsigned long long)(g_stallPkg && g_stallSince ? (g_lastTick - g_stallSince) / 1000 : 0), g_stallWarned ? ", WARNED" : "");
        return buf;
    }
}
