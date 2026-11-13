// cw_smart_game.cpp - see cw_smart_game.h and docs/ARMORY.md §3.
//
// Lock: every frame while a smart WEAP (in armory-formmap.json smart.weapons, or carrying smart.keyword) is equipped
// AND Smart Link is installed, the high-process actors that are alive, hostile, in range and near the crosshair are
// fed to cw::smart::Tracker with the aim ray (AimRay: the camera axis that projects to the screen centre; GetEyeVector
// only as an unverified fallback) and ADS state (gunState sighted). [Smart] bDiag logs why each one does or does not lock.
//
// Steering: MissileProjectile's vtable slot 0xCF (UpdateImpl) is patched, after checking the vtable really is
// MissileProjectile's (its RTTI complete-object-locator must name RTTI::MissileProjectile; otherwise nothing is
// patched and the feature stays lock + log). For a round the PLAYER fired from a smart weapon, the first update
// assigns a locked target (even distribution) and rolls HitProbability; every update before the original runs turns
// `velocity` toward the target's object centre (speed kept, turn rate capped, CP corkscrew), points
// `movementDirection` along it and sets `desiredTarget`. Which of those the engine honours for movement is NOT proven
// in game yet: iSteerMode 1 tests desiredTarget alone, 2 adds the velocity write. Rounds of everyone else, and all
// projectiles while bSmartLock=0, pass straight to the original.
//
// The hook may run off the main thread (projectile jobs), so the lock snapshot and the round table are behind a mutex.
#include "PCH.h"

#include "cw_smart_game.h"
#include "cw_smart.h"

#include <windows.h>

#ifdef ERROR
#    undef ERROR
#endif

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <set>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

using cw::json;

namespace smartgame {
namespace {

Config g_cfg;
std::string g_plugin = "Cyberware - Armory.esp";
bool g_ready = false, g_hooked = false;
RE::BGSKeyword* g_kw = nullptr;
std::set<std::uint32_t> g_projForms;                           // the armory smart PROJ form ids
std::map<std::uint32_t, cw::smart::Params> g_weapons;          // smart WEAP form id -> CP params

cw::smart::Tracker g_tracker;                                  // game thread (Frame) only, except NextShotTarget under g_mx
std::mutex g_mx;
struct Snapshot {
    bool active = false;
    std::uint32_t weapon = 0;
    cw::smart::Params::Game game{};
    double hitP = 0.95;
    std::map<std::uint32_t, cw::Vec3> aim;                     // locked target -> aim point
    cw::Vec3 eye{};                                            // the aim ray's origin this frame (rounds move away from it)
} g_snap;
// Steering evidence (SMART GUN, 2026-11-10; the lead: "a smart gun whose rounds do not curve is not a smart gun"). Every
// round's done line says how far its target sat off the bore when it was fired, how far the round actually turned, and
// how many of our velocity writes the engine kept: the template is MissileProjectileSeek ("Seeks Aim Vector"), and whether
// its own seek logic overwrites the velocity we write before the original UpdateImpl is exactly what is unproven.
struct Round {
    std::uint32_t target = 0;
    bool homes = false;
    double cycle = 0.12, sign = 1, flown = 0, age = 0, closest = 1e18;
    int frames = 0;
    std::uint64_t lastSeen = 0;
    cw::Vec3 last{};
    cw::Vec3 v0{};                  // velocity at the first update (the bore line it left on)
    cw::Vec3 vLast{};               // velocity at the latest update
    double offBoreDeg = -1;         // target's angle off v0 at the first update; -1 = no target
    cw::Vec3 wrote{};               // the velocity we wrote last update
    bool haveWrote = false;
    int writes = 0, kept = 0;       // writes checked at the next update / of those, still ours (within 0.5 deg)
};
std::unordered_map<std::uint32_t, Round> g_rounds;             // projectile ref form id -> state
std::uint64_t g_roundsTotal = 0, g_roundsSteered = 0;
std::uint32_t g_skippy = 0;                                    // CW_AR_Skippy
constexpr const char* kToneStart = "CW_SND_SmartLockStart";      // SOUND slots (sounds.json), plugin "sound", by EditorID
constexpr const char* kToneAcquired = "CW_SND_SmartLockAcquired";   // CP w_gun_ui_smart_lock_start / _end
// No lost/break tone: SOUND (2026-11-10) found CP has no such event, so a lost lock is logged and silent.
int g_skippyMode = 0;                                          // 0 heads (Stone Cold Killer), 1 limbs (Puppy-Loving Pacifist)
std::uint64_t g_skippyBusyUntil = 0;                           // it never talks over itself
std::uint64_t g_nextDiag = 0;                                  // [Smart] bDiag: next diag line (GetTickCount64 ms)
// HUD bracket hygiene (QD block 26 item 7: a red half-bracket stayed on screen with no lock, through a cell reload).
// hud::Event drops silently while the view is not ready, and the sender de-duplicates, so one lost "empty" message could
// strand a bracket for good. So: a load forces an empty list through, the list is re-sent once a second while a smart gun
// is up even if unchanged, and for 3 s after the gun goes away (or a load) an empty list is re-sent once a second.
std::atomic<bool> g_hudReset{ false };                         // set by OnLoad, consumed by Frame (game thread)
bool g_wasActive = false;
std::string g_hudLastSent;
std::uint64_t g_hudNextBeat = 0, g_hudClearUntil = 0;

std::uint32_t Resolve(const json& forms, const std::string& edid)
{
    if (!forms.contains(edid) || !forms[edid].is_string()) return 0;
    std::uint32_t local = 0;
    try { local = (std::uint32_t)std::stoul(forms[edid].get<std::string>(), nullptr, 16); } catch (...) { return 0; }
    auto* dh = RE::TESDataHandler::GetSingleton();
    auto* f = dh ? dh->LookupForm(local & 0x00FFFFFF, g_plugin) : nullptr;
    return f ? f->GetFormID() : 0;
}

RE::TESObjectWEAP* Equipped(RE::Actor* a)
{
    auto* mh = a && a->currentProcess ? a->currentProcess->middleHigh : nullptr;
    if (!mh) return nullptr;
    for (auto& e : mh->equippedItems)
        if (auto* w = e.item.object ? e.item.object->As<RE::TESObjectWEAP>() : nullptr) return w;
    return nullptr;
}

const cw::smart::Params* ParamsFor(RE::TESForm* w)
{
    if (!w) return nullptr;
    if (auto it = g_weapons.find(w->GetFormID()); it != g_weapons.end()) return &it->second;
    static const cw::smart::Params kDefault{};
    if (g_kw) if (auto* wp = w->As<RE::TESObjectWEAP>(); wp && static_cast<const RE::BGSKeywordForm*>(wp)->HasKeyword(g_kw)) return &kDefault;
    return nullptr;
}

cw::Vec3 V(const RE::NiPoint3& p) { return { p.x, p.y, p.z }; }

// Screen position of a world point, 0..1 from the top left of the view; false when it is behind the camera.
// Which way the engine's y grows is measured, not assumed: a point 10 units above the target must come out higher.
bool ToScreen(const cw::Vec3& v, double& sx, double& sy, double& perUnit)
{
    auto* cam = RE::Main::WorldRootCamera();
    if (!cam) return false;
    float x = 0, y = 0, d = 0, x2 = 0, y2 = 0, d2 = 0;
    if (!cam->WorldPtToScreenPt3(RE::NiPoint3((float)v.x, (float)v.y, (float)v.z), x, y, d, 1e-5f) || d <= 0) return false;
    if (!cam->WorldPtToScreenPt3(RE::NiPoint3((float)v.x, (float)v.y, (float)v.z + 10.f), x2, y2, d2, 1e-5f)) return false;
    const auto& pr = cam->port;
    const double w = pr.right - pr.left, h = pr.bottom - pr.top;
    sx = w != 0 ? (x - pr.left) / w : x;
    sy = h != 0 ? (y - pr.top) / h : y;
    const double above = h != 0 ? (y2 - pr.top) / h : y2;
    perUnit = std::abs(above - sy) / 10.0;
    if (above > sy) sy = 1.0 - sy;      // the raised point came out lower: this y grows upward
    return true;
}

// Where the crosshair points: the eye point and unit aim direction the lock cones are measured from.
// P14 (cw-int-4, A4): with Actor::GetEyeVector (vtable 0xE7, used nowhere else in Cyberware) as the only source, a raider
// 800 u dead ahead never started a lock and every round logged "no lock". Which way that vector points for the player
// in 1.10.163 is unproven, so it is no longer trusted blind. The camera IS the crosshair: of the candidate forward
// axes (camera rotation column 0, row 0, the eye vector), the first whose point 1000 u out projects to the screen
// centre (within 3% of the view) wins. A wrong guess about the matrix layout can then never point the cone elsewhere;
// it only falls through to the next candidate. `src` names the winner for the log and the diag line.
bool AimRay(RE::PlayerCharacter* p, cw::Vec3& eye, cw::Vec3& dir, cw::Vec3& eyeVecDir, const char*& src)
{
    RE::NiPoint3 eo, ed;
    p->GetEyeVector(eo, ed, true);
    eyeVecDir = V(ed);
    if (auto* cam = RE::Main::WorldRootCamera()) {
        const auto& m = cam->world.rotate;
        const auto& t = cam->world.translate;
        const cw::Vec3 c{ t.x, t.y, t.z };
        const cw::Vec3 cands[3] = { { m.entry[0][0], m.entry[1][0], m.entry[2][0] }, { m.entry[0][0], m.entry[0][1], m.entry[0][2] }, eyeVecDir };
        static const char* names[3] = { "camera col0", "camera row0", "eye vector" };
        for (int i = 0; i < 3; ++i) {
            const double l = cw::smart::Len(cands[i]);
            if (!(l > 0.5 && l < 1.5)) continue;
            const cw::Vec3 d{ cands[i].x / l, cands[i].y / l, cands[i].z / l };
            double sx = 0, sy = 0, pu = 0;
            if (!ToScreen({ c.x + d.x * 1000, c.y + d.y * 1000, c.z + d.z * 1000 }, sx, sy, pu)) continue;   // behind the camera
            if (std::abs(sx - 0.5) < 0.03 && std::abs(sy - 0.5) < 0.03) { eye = c; dir = d; src = names[i]; return true; }
        }
    }
    eye = V(eo);
    dir = eyeVecDir;
    src = "eye vector (NOT verified on screen)";
    return false;
}

// Skippy's voice (lines from CP's mq007, SNDRs CW_SND_Skippy*). A reload always speaks; firing and idle chatter wait their
// cooldowns, so it reads as a companion and not a repeating bark. Timings are first guesses for a listener to judge.
void SkippySay(const char* edid, std::uint64_t now, std::uint64_t busyMs)
{
    if (!g_cfg.playSound) return;
    g_skippyBusyUntil = now + busyMs;
    g_cfg.playSound(edid);
    REX::INFO("smart: Skippy says {}", edid);
}

void SkippyChatter(RE::PlayerCharacter* p, std::uint64_t now)
{
    static RE::GUN_STATE last = RE::GUN_STATE::kDrawn;
    static std::uint64_t nextProvoke = 0, nextFact = 0;
    const RE::GUN_STATE gs = p->gunState;
    if (nextFact == 0) nextFact = now + 60000 + std::rand() % 60000;
    if (now >= g_skippyBusyUntil) {
        const bool firing = gs == RE::GUN_STATE::kFire || gs == RE::GUN_STATE::kFireSighted;
        const bool wasFiring = last == RE::GUN_STATE::kFire || last == RE::GUN_STATE::kFireSighted;
        if (gs == RE::GUN_STATE::kReloading && last != RE::GUN_STATE::kReloading) SkippySay("CW_SND_SkippyReload", now, 5500);
        else if (firing && !wasFiring && now >= nextProvoke) { nextProvoke = now + 20000 + std::rand() % 25000; SkippySay("CW_SND_SkippyProvoke", now, 5000); }
        else if (!firing && now >= nextFact) {
            nextFact = now + 120000 + std::rand() % 120000;
            SkippySay(std::rand() % 4 ? "CW_SND_SkippyFact" : "CW_SND_SkippyHum", now, 8000);
        }
    }
    last = gs;
}

// ------------------------------------------------------------------------------------------------ the hook
using UpdateImpl_t = void (*)(RE::Projectile*, float);
UpdateImpl_t g_orig = nullptr;

// One line per round, homing or not (caller holds g_mx). "turned" is the angle between the velocity it left the muzzle with
// and its last one: a straight round reads about 0, a round that bent onto a target off the bore reads about that angle.
// "writes kept" is how many of our velocity writes were still in place at the next update; 0 of N means the engine
// overwrote every one, and any turn came from the engine's own seek (desiredTarget) or nowhere.
void LogRoundDone(std::uint32_t id, const Round& r, const char* why)
{
    const double turned = cw::smart::Len(r.v0) > 1e-3 && cw::smart::Len(r.vLast) > 1e-3 ? cw::smart::AngleBetween(r.v0, r.vLast) * 57.29578 : -1.0;
    REX::INFO("smart: round {:08X} done ({}): {} -> {:08X}, flew {:.0f} u in {:.2f}s, turned {:.1f} deg{}{}, steered {} frames, closest {:.0f} u, writes kept {}/{}",
              id, why, r.homes ? "homing" : "straight", r.target, r.flown, r.age, turned,
              r.offBoreDeg >= 0 ? std::format(" (target was {:.1f} deg off the bore)", r.offBoreDeg) : "", r.homes && r.frames == 0 ? " (lock gone before its first update)" : "",
              r.frames, r.closest > 1e17 ? -1.0 : r.closest, r.kept, r.writes);
}

void SteerRound(RE::Projectile* pr, float dt)
{
    auto* base = pr->GetObjectReference();
    auto* wsrc = pr->weaponSource.object;
    const bool ours = (base && g_projForms.count(base->GetFormID())) || (wsrc && ParamsFor(wsrc));
    if (!ours) return;
    auto shooter = pr->shooter.get();
    if (!shooter || shooter.get() != RE::PlayerCharacter::GetSingleton()) return;

    std::lock_guard lk(g_mx);
    if (!g_snap.active) return;
    const std::uint32_t id = pr->GetFormID();
    const cw::Vec3 pos = V(pr->GetPosition());
    auto [it, isNew] = g_rounds.try_emplace(id);
    bool fresh = isNew;
    Round& r = it->second;
    // The engine reuses projectile ref ids (P15b: FF0061C4 fired four times in 3 s). A round only ever moves away from
    // the shooter, so an entry whose position jumped back toward the eye by more than 150 u is a new round on an old id.
    if (!fresh && cw::smart::Len(cw::smart::Sub(pos, g_snap.eye)) + 150 < cw::smart::Len(cw::smart::Sub(r.last, g_snap.eye))) {
        LogRoundDone(id, r, "id reused");
        r = Round{};
        fresh = true;
    }
    const cw::Vec3 vel = V(pr->velocity);
    if (fresh) {
        ++g_roundsTotal;
        r.target = g_tracker.NextShotTarget();
        r.homes = r.target && cw::smart::RollHome(((std::uint64_t)id << 20) ^ g_roundsTotal, g_snap.hitP);
        cw::smart::RollSpiral(((std::uint64_t)id << 8) ^ 0x5eedu, g_snap.game, r.cycle, r.sign);
        r.last = pos;
        r.v0 = vel;
        if (r.target)
            if (auto at0 = g_snap.aim.find(r.target); at0 != g_snap.aim.end() && cw::smart::Len(vel) > 1e-3)
                r.offBoreDeg = cw::smart::AngleBetween(vel, cw::smart::Sub(at0->second, pos)) * 57.29578;
        if (r.homes) ++g_roundsSteered;
        REX::INFO("smart: round {:08X} fired -> target {:08X} ({}){}", id, r.target, !r.target ? "no lock, flies straight" : (r.homes ? "homing" : "HitProbability miss, flies straight"),
                  r.offBoreDeg >= 0 ? std::format(", target {:.1f} deg off the bore", r.offBoreDeg) : "");
    }
    // did the engine keep the velocity we wrote last update? (the original UpdateImpl runs between the two reads)
    if (r.haveWrote && cw::smart::Len(vel) > 1e-3 && cw::smart::Len(r.wrote) > 1e-3) {
        ++r.writes;
        if (cw::smart::AngleBetween(vel, r.wrote) * 57.29578 < 0.5) ++r.kept;
    }
    r.haveWrote = false;
    r.vLast = vel;
    r.lastSeen = GetTickCount64();
    r.flown += cw::smart::Len(cw::smart::Sub(pos, r.last));
    r.last = pos;
    r.age += dt;
    if (!r.homes) return;
    auto at = g_snap.aim.find(r.target);
    if (at == g_snap.aim.end()) return;   // lock lost mid-flight: keep the heading it has
    r.closest = std::min(r.closest, cw::smart::Len(cw::smart::Sub(at->second, pos)));
    ++r.frames;
    if (g_cfg.steerMode >= 1 && r.frames == 1)
        if (auto* tf = RE::TESForm::GetFormByID(r.target); tf)
            if (auto* tr = tf->As<RE::TESObjectREFR>()) pr->desiredTarget = tr->GetHandle();
    if (g_cfg.steerMode < 2) return;
    const cw::Vec3 nv = cw::smart::Steer(pos, vel, at->second, dt, g_snap.game, r.flown, r.age, r.sign, r.cycle);
    pr->velocity = RE::NiPoint3((float)nv.x, (float)nv.y, (float)nv.z);
    r.wrote = nv;
    r.haveWrote = true;
    if (const double l = cw::smart::Len(nv); l > 1e-6)
        pr->movementDirection = RE::NiPoint3((float)(nv.x / l), (float)(nv.y / l), (float)(nv.z / l));
}

void Hook(RE::Projectile* a_this, float a_delta)
{
    if (g_ready && g_cfg.enabled && a_this && a_delta > 0) {
        try { SteerRound(a_this, a_delta); } catch (...) {}
    }
    g_orig(a_this, a_delta);
}

// The vtable REL::ID resolves for this runtime must belong to MissileProjectile: its complete-object-locator
// (vtbl[-1]) holds the RVA of the class's TypeDescriptor at +12 (x64 MSVC ABI), which must be RTTI::MissileProjectile.
bool InstallHook()
{
    if (g_hooked) return true;
    REL::Relocation<std::uintptr_t> vt{ RE::VTABLE::MissileProjectile[0] };
    REL::Relocation<std::uintptr_t> rtti{ RE::RTTI::MissileProjectile };
    void** vtbl = reinterpret_cast<void**>(vt.address());
    const std::uintptr_t imageBase = vt.address() - vt.offset();
    const auto* col = reinterpret_cast<const std::uint8_t*>(vtbl[-1]);
    const std::int32_t tdRva = col ? *reinterpret_cast<const std::int32_t*>(col + 12) : 0;
    if (!col || imageBase + (std::uintptr_t)tdRva != rtti.address()) {
        REX::ERROR("smart: MissileProjectile vtable check FAILED (type descriptor {:X} != {:X}); no steering, lock + log only",
                   imageBase + (std::uintptr_t)tdRva, rtti.address());
        return false;
    }
    constexpr std::size_t kUpdateImpl = 0xCF;
    std::uint32_t old = 0;
    if (!REX::W32::VirtualProtect(&vtbl[kUpdateImpl], sizeof(void*), 0x40u /* PAGE_EXECUTE_READWRITE */, &old)) {
        REX::ERROR("smart: VirtualProtect failed; no steering, lock + log only");
        return false;
    }
    g_orig = reinterpret_cast<UpdateImpl_t>(vtbl[kUpdateImpl]);
    vtbl[kUpdateImpl] = reinterpret_cast<void*>(&Hook);
    REX::W32::VirtualProtect(&vtbl[kUpdateImpl], sizeof(void*), old, &old);
    g_hooked = true;
    REX::INFO("smart: MissileProjectile::UpdateImpl hooked (vtable {:X}, slot 0x{:X}, RTTI verified)", vt.address(), kUpdateImpl);
    return true;
}

}  // namespace

void Init(Config c)
{
    g_cfg = std::move(c);
    if (!g_cfg.enabled) { REX::INFO("smart: [Smart] bSmartLock=0, smart guns fire straight (lock and steering off)"); return; }
    std::ifstream f(g_cfg.formmapPath);
    if (!f) { REX::INFO("smart: no {} (ARMORY not installed); smart lock inert", g_cfg.formmapPath); return; }
    json j;
    try { j = json::parse(f); } catch (const std::exception& e) { REX::ERROR("smart: {}: {}", g_cfg.formmapPath, e.what()); return; }
    g_plugin = j.value("plugin", g_plugin);
    const json forms = j.value("forms", json::object());
    const json sm = j.value("smart", json::object());
    if (auto kid = Resolve(forms, sm.value("keyword", std::string("CW_AR_Smart"))))
        if (auto* kf = RE::TESForm::GetFormByID(kid)) g_kw = kf->As<RE::BGSKeyword>();
    const json projs = sm.value("projectiles", json::array()), weaps = sm.value("weapons", json::object());
    for (auto& e : projs)
        if (e.is_string()) if (auto id = Resolve(forms, e.get<std::string>())) g_projForms.insert(id);
    for (auto& [edid, pj] : weaps.items()) {
        auto id = Resolve(forms, edid);
        if (!id) { REX::WARN("smart: weapon {} is not in {}", edid, g_plugin); continue; }
        g_weapons[id] = cw::smart::Params::FromJson(pj);
        if (g_cfg.stageBAnim && (edid == "CW_AR_Yukimura" || edid == "CW_AR_Skippy")) g_cfg.stageBAnim(id);
        if (pj.contains("projectile") && pj["projectile"].is_string())
            if (auto pid = Resolve(forms, pj["projectile"].get<std::string>())) g_projForms.insert(pid);
        const auto g = g_weapons[id].ToGame();
        REX::INFO("smart: {} ({:08X}): hip {} locks / ads {} in {:.1f}/{:.1f} deg out to {:.0f} u, lock {:.2f}s unlock {:.2f}s, round {:.0f} u/s, spiral {:.1f} u, turn {:.0f} deg/s",
                  edid, id, g.hipMax, g.adsMax, g_weapons[id].HipLockingAngleYaw, g_weapons[id].AdsLockingAngleYaw, g.rangeU, g.hipLockS, g.hipUnlockS, g.speedU,
                  g.spiral ? g.spiralRU : 0.0, g_weapons[id].TurnRateDeg);
        if (g.rangeU <= 0) REX::WARN("smart: {} has a lock range of 0 u: nothing will ever lock (armory-formmap.json TargetAcquisitionRange)", edid);
    }
    g_ready = !g_weapons.empty() || g_kw;
    if (!g_ready) { REX::WARN("smart: no smart weapon or keyword of {} resolved (plugin not loaded, or {} stale); inert", g_plugin, g_cfg.formmapPath); return; }
    const bool hooked = g_cfg.steerMode > 0 && InstallHook();
    g_skippy = Resolve(forms, "CW_AR_Skippy");
    REX::INFO("smart: ready: {} weapon(s), {} projectile form(s), keyword {}, steer mode {} ({})", g_weapons.size(), g_projForms.size(),
              g_kw ? "yes" : "no", g_cfg.steerMode, hooked ? "hooked" : "lock + log only");
}

void OnLoad()
{
    std::lock_guard lk(g_mx);
    g_tracker.Reset();
    g_rounds.clear();
    g_snap = {};
    g_hudReset = true;   // Frame pushes an empty lock list through (see g_hudReset)
}

void Frame()
{
    if (!g_ready || !g_cfg.enabled) return;
    static std::uint64_t lastMs = 0, nextHud = 0;
    const std::uint64_t now = GetTickCount64();
    const double dt = lastMs ? std::clamp((double)(now - lastMs) / 1000.0, 0.0, 0.1) : 0.0;
    lastMs = now;

    auto* p = RE::PlayerCharacter::GetSingleton();
    auto* w = p ? Equipped(p) : nullptr;
    const cw::smart::Params* prm = ParamsFor(w);
    const bool link = g_cfg.smartLinkInstalled && g_cfg.smartLinkInstalled();
    bool& wasActive = g_wasActive;
    std::vector<cw::smart::LockEvent> ev;
    if (g_hudReset.exchange(false)) {   // a save was loaded: whatever the view last drew is stale
        wasActive = false;
        g_hudLastSent.clear();
        g_hudClearUntil = now + 3000;
        g_hudNextBeat = 0;
    }

    if (!p || !prm || !link || !p->GetWeaponMagicDrawn()) {
        if (wasActive) {
            std::lock_guard lk(g_mx);
            g_tracker.Reset(); g_snap.active = false; g_snap.aim.clear();
            REX::INFO("smart: off ({})", !prm ? "no smart weapon" : (!link ? "no Smart Link installed" : "weapon holstered"));
            if (g_cfg.hudEvents && g_cfg.hudEvent) g_cfg.hudEvent(json{ { "type", "smartLocks" }, { "locks", json::array() } });
            g_hudLastSent = "[]";
            g_hudClearUntil = now + 3000;
            g_hudNextBeat = now + 1000;
        }
        wasActive = false;
    } else {
        if (!wasActive) REX::INFO("smart: on ({:08X}, Smart Link installed)", w->GetFormID());
        wasActive = true;
        cw::Vec3 eye{}, dir{}, eyeVecDir{};
        const char* aimSrc = "";
        AimRay(p, eye, dir, eyeVecDir, aimSrc);
        static std::string lastSrc;
        if (lastSrc != aimSrc) { lastSrc = aimSrc; REX::INFO("smart: aim from {}", aimSrc); }
        const bool diag = g_cfg.diag && now >= g_nextDiag;
        if (diag) g_nextDiag = now + 1000;
        std::vector<std::string> diagRows;
        int nHigh = 0, nRange = 0;
        const bool ads = p->gunState == RE::GUN_STATE::kSighted || p->gunState == RE::GUN_STATE::kFireSighted;
        const auto g = prm->ToGame();
        const double wide = 1.5 * std::max({ g.hipTgtYaw, g.hipTgtPitch, g.adsTgtYaw, g.adsTgtPitch });

        std::vector<cw::smart::Candidate> cands;
        using host_t = bool (*)(RE::Actor*, RE::Actor*);
        static REL::Relocation<host_t> hostile{ RE::ID::Actor::GetHostileToActor };
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            for (auto& hd : pl->highActorHandles) {
                auto a = hd.get();
                if (!a || a.get() == p || a->lifeState != 0) continue;
                ++nHigh;
                RE::NiPoint3 c; a->GetObjectCenter(c);
                cw::smart::Candidate cd; cd.id = a->GetFormID(); cd.aim = V(c);
                if (const double dCentre = cw::smart::Len(cw::smart::Sub(cd.aim, eye)); dCentre > g.rangeU) {   // listed too: P15's raider fell out of range unseen
                    if (diag && diagRows.size() < 6) diagRows.push_back(std::format("{:08X} {:.0f}u z rel {:+.0f} out of range", cd.id, dCentre, cd.aim.z - eye.z));
                    continue;
                }
                ++nRange;
                {   // a smart gun aims at the chest and Skippy picks the body part; a target without the human node keeps its centre
                    auto* root = a->Get3D();
                    const char* part = w->GetFormID() != g_skippy ? "Chest" : (g_skippyMode ? "RLeg_Calf" : "Head");
                    auto* node = root ? root->GetObjectByName(RE::BSFixedString(part)) : nullptr;
                    if (node) cd.aim = V(node->world.translate);
                }
                double yaw, pitch; cw::smart::AngleOff(eye, dir, cd.aim, yaw, pitch);
                const bool tracked = g_tracker.Find(cd.id) != nullptr;
                const double dist = cw::smart::Len(cw::smart::Sub(cd.aim, eye));
                // z rel = the aim point's height against the eye: a target sinking fast fell through the floor (P15 L2 fog cell)
                auto row = [&](const char* verdict, bool tested) {
                    if (diag) diagRows.push_back(std::format("{:08X} {:.0f}u z rel {:+.0f} yaw {:+.1f} pitch {:+.1f} hostile {} los {} {}", cd.id, dist, cd.aim.z - eye.z,
                                                             yaw * 57.29578, pitch * 57.29578, tested ? (cd.hostile ? "true" : "false") : "-",
                                                             tested ? (cd.los ? "true" : "false") : "-", verdict));
                };
                // outside the cone hostility and LOS are not tested (printed "-"): P15 read the old default "hostile false" as a verdict
                if (!tracked && std::hypot(yaw, pitch) > wide) { if (diag && diagRows.size() < 6) row("outside the cone", false); continue; }   // LOS is expensive: only near the crosshair
                cd.alive = true;
                cd.hostile = hostile(a.get(), p);
                bool pick = false;
                cd.los = cd.hostile && p->HasLOSToTarget(a.get(), &pick);
                row(!cd.hostile ? "not hostile" : (!cd.los ? "no LOS" : "candidate"), true);
                cands.push_back(cd);
            }
        }
        std::lock_guard lk(g_mx);
        g_tracker.Update(eye, dir, ads, cands, *prm, dt, &ev);
        g_snap.active = true;
        g_snap.weapon = w->GetFormID();
        g_snap.game = g;
        g_snap.eye = eye;
        g_snap.hitP = prm->HitProbability;
        g_snap.aim.clear();
        for (auto& l : g_tracker.Locks()) if (l.locked) g_snap.aim[l.id] = l.aim;
        if (diag) {
            // [Smart] bDiag (Cyberware.Test.ini): once a second while a smart gun is up, the aim ray and every actor near the
            // crosshair with the reason it is or is not building a lock. P14 A4 had no such line and could not say why.
            std::string locks;
            for (auto& l : g_tracker.Locks()) locks += std::format(" {:08X}:{:.2f}{}", l.id, l.progress, l.locked ? "L" : "");
            std::string rows;
            for (auto& r : diagRows) rows += " | " + r;
            REX::INFO("smart: diag {} {} eye ({:.0f}, {:.0f}, {:.0f}) dir ({:+.3f}, {:+.3f}, {:+.3f}) eyevec ({:+.3f}, {:+.3f}, {:+.3f}) high {} in range {} (range {:.0f} u) locks [{} ]{}",
                      aimSrc, ads ? "ads" : "hip", eye.x, eye.y, eye.z, dir.x, dir.y, dir.z, eyeVecDir.x, eyeVecDir.y, eyeVecDir.z, nHigh, nRange, g.rangeU, locks, rows);
        }
    }
    // Lock tones (SMT-1, the lead, 2026-11-09: CP's own w_gun_ui_smart_lock_* via SOUND's SNDRs). A lock that starts building
    // ticks once per 150 ms; an acquired lock tones once per lock SESSION (below); a LOST lock has no cue at all, because CP
    // has no lost/break/cancel event. Until SOUND's slot has files the SNDR does not exist and the play is a one-time
    // "no sound handle" WARN, nothing else.
    {
        static std::set<std::uint32_t> building;
        static std::uint64_t nextTick = 0;
        std::set<std::uint32_t> now_b;
        std::vector<std::uint32_t> started;
        {
            std::lock_guard lk(g_mx);
            for (auto& l : g_tracker.Locks()) {
                if (l.locked || l.progress <= 0) continue;
                now_b.insert(l.id);
                if (!building.contains(l.id)) started.push_back(l.id);
            }
        }
        building.swap(now_b);
        if (!wasActive) building.clear();
        for (auto id : started) {
            const bool tone = now >= nextTick && g_cfg.playSound;   // three targets entering the cone together give one tick
            if (tone) { nextTick = now + 150; g_cfg.playSound(kToneStart); }
            REX::INFO("smart: lock building {:08X}{}", id, tone ? std::format(" (tone {})", kToneStart) : "");
        }
    }
    // Acquired tone: ONE cue per lock SESSION, not one per locked target (SOUND ruling 2026-11-10,
    // sound/WIRING-SPEC.md "No double-plays"). It tones on the transition from nothing-locked to something-locked, stays
    // silent while any lock is still held, and re-arms once the lock set empties, so a fully lost and re-acquired lock is a
    // new action and tones again. A time window was REJECTED for this cue: a 3-way split spreads over a few hundred ms, so a
    // 150 ms gate would still let two tones through, and three copies of one wav summing inside that span can reach about
    // +9.5 dB, pushing a -24 dB "ability" cue to roughly -15 and breaching that group's never-covers-dialogue rule.
    // `held` is counted AFTER the tracker update that produced `ev`, so a lock acquired this frame already counts and cannot
    // re-arm itself. Known edge: if the last lock is lost and a new one acquired in the SAME frame, the set never observably
    // empties and the new lock is silent, which matches "stay silent while any lock is held".
    {
        static bool acqArmed = true;
        std::size_t held = 0;
        {
            std::lock_guard lk(g_mx);
            for (auto& l : g_tracker.Locks()) if (l.locked) ++held;
        }
        if (!wasActive || held == 0) acqArmed = true;   // weapon away, or every lock gone: the next lock is a new action
        for (auto& e : ev) {
            const bool acq = e.kind == cw::smart::LockEvent::Acquired;
            const bool tone = acq && acqArmed && g_cfg.playSound;
            if (tone) { acqArmed = false; g_cfg.playSound(kToneAcquired); }
            REX::INFO("smart: lock {} {:08X}{}{}{}", acq ? "ACQUIRED" : "lost", e.id, e.why.empty() ? "" : " - ", e.why,
                      tone ? std::format(" (tone {})", kToneAcquired) : (acq ? " (tone gated: lock session already toned)" : ""));
        }
    }
    if (p && w && g_skippy && w->GetFormID() == g_skippy && p->GetWeaponMagicDrawn()) SkippyChatter(p, now);
    if (wasActive && g_cfg.hudEvents && g_cfg.hudEvent && (now >= nextHud || !ev.empty())) {
        nextHud = now + 33;    // ~30 Hz: the brackets ride the targets (SMT-1 fails on a visible lag); nothing is sent while nothing moves
        json locks = json::array();
        {
            std::lock_guard lk(g_mx);
            for (auto& l : g_tracker.Locks())
            {
                double sx = 0, sy = 0, perUnit = 0;
                if (!ToScreen(l.aim, sx, sy, perUnit)) continue;     // behind the camera: no bracket
                const auto pp = p->GetPosition();
                const double dist = cw::smart::Len(cw::smart::Sub(l.aim, cw::Vec3{ pp.x, pp.y, pp.z }));
                locks.push_back({ { "id", std::format("{:08X}", l.id) }, { "progress", std::round(l.progress * 100) / 100 }, { "locked", l.locked },
                                  { "x", std::round(sx * 1000) / 1000 }, { "y", std::round(sy * 1000) / 1000 }, { "dist", std::round(dist) }, { "u", std::round(perUnit * 1e6) / 1e6 } });
            }
        }
        // re-sent once a second even when unchanged: a message dropped while the view was not ready must not stick
        if (std::string s = locks.dump(); s != g_hudLastSent || !ev.empty() || now >= g_hudNextBeat) {
            g_hudLastSent = s;
            g_hudNextBeat = now + 1000;
            g_cfg.hudEvent(json{ { "type", "smartLocks" }, { "locks", locks } });
        }
    } else if (!wasActive && g_cfg.hudEvents && g_cfg.hudEvent && now < g_hudClearUntil && now >= g_hudNextBeat) {
        g_hudNextBeat = now + 1000;   // weapon away or just loaded: an empty list, once a second for 3 s
        g_hudLastSent = "[]";
        g_cfg.hudEvent(json{ { "type", "smartLocks" }, { "locks", json::array() } });
    }
    // retire rounds not updated for 300 ms (impact or out of range): the closest approach is the FUNCTIONAL evidence
    std::lock_guard lk(g_mx);
    for (auto it = g_rounds.begin(); it != g_rounds.end();) {
        if (now - it->second.lastSeen > 300) {
            LogRoundDone(it->first, it->second, "no update for 300 ms: impact or out of range");
            it = g_rounds.erase(it);
        } else ++it;
    }
}

void CycleSkippyMode()
{
    auto* p = RE::PlayerCharacter::GetSingleton();
    auto* w = p ? Equipped(p) : nullptr;
    if (!g_skippy || !p || !p->GetWeaponMagicDrawn() || !w || w->GetFormID() != g_skippy) return;
    g_skippyMode = g_skippyMode ? 0 : 1;
    const std::string name = g_skippyMode ? "Puppy-Loving Pacifist (limbs)" : "Stone Cold Killer (heads)";
    REX::INFO("smart: Skippy mode {}", name);
    if (g_cfg.notify) g_cfg.notify("Skippy: " + name);
    SkippySay(g_skippyMode ? "CW_SND_SkippyModeLimbs" : "CW_SND_SkippyModeKiller", GetTickCount64(), 7500);
}

std::string Stats()
{
    std::lock_guard lk(g_mx);
    return std::format("smart: enabled {} ready {} hooked {} mode {} weapons {} locks {} rounds live {} total {} homing {}",
                       g_cfg.enabled, g_ready, g_hooked, g_cfg.steerMode, g_weapons.size(), g_tracker.Locked().size(), g_rounds.size(), g_roundsTotal, g_roundsSteered);
}

}  // namespace smartgame
