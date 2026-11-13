// cw_scenepkg.h - the install scene's "is she free for the chair?" decision (INSTALL SCENE stream, 2026-11-10).
// Header only, no RE types, so cwtest --selftest checks every case on any box. ASCII only.
//
// The scene's ready guard used to read ONE thing: is her running package one of her station packages
// ([Scene] sReadyPkgs_<doc>, Varga: shop 975 and indoor hold A4C)? With [Clinic] bSchedule=0 (what ships) the engine
// does not re-pick an open-ended package while she stays loaded, so a player who WAITS in the clinic across 10:00 and
// then talks at once finds her still on the office package 973: the guard took the 2D beat for a state that is only stale.
// CLINIC's nudge (cw_clinic.cpp @ 12e590a8) fixes it only if 5-25 s of unpaused world pass first, and it never runs in
// a talk. So the guard also asks the clock: when the running package is one of HER OWN schedule packages and the
// package her clock wants now IS a station package, she is due at her station and the chair scene may run. Its start
// writes CW_ClinicSceneActive = 1 and evaluates her package with the world unpaused, which picks the step-in package
// (FIRST in her list) and walks her to her mark from wherever she is.
//
// What it never does: run the scene on a package that is not hers (dialogue, combat, a quest: not ours to pre-empt),
// or when her clock wants bed or office (before 10:00: the reason the guard exists - never pull her out of bed).
#pragma once

#include <cstdint>
#include <vector>

namespace cw::scenepkg {

enum class Verdict {
    NoCheck,    // no station packages configured for this doctor: the guard does not apply
    Ready,      // she runs one of her station packages
    StaleDue,   // she runs one of her own schedule packages and her clock wants a station package now
    NotReady,   // anything else: the 2D beat
};

inline const char* VerdictName(Verdict v)
{
    switch (v) {
    case Verdict::NoCheck: return "no-check";
    case Verdict::Ready: return "ready";
    case Verdict::StaleDue: return "stale-due";
    default: return "not-ready";
    }
}

// running  runtime FormID of her running package (0 = none)
// ready    runtime FormIDs of her station packages (empty = no check)
// hers     running is one of her own schedule packages (the clinic decides; false when it has no opinion)
// due      runtime FormID of the package her clock wants now (0 = no opinion: not watched, not in the clinic)
inline Verdict Judge(std::uint32_t running, const std::vector<std::uint32_t>& ready, bool hers, std::uint32_t due)
{
    if (ready.empty()) return Verdict::NoCheck;
    auto isReady = [&](std::uint32_t id) {
        if (!id) return false;
        for (const auto r : ready) if (r == id) return true;
        return false;
    };
    if (isReady(running)) return Verdict::Ready;
    if (running && hers && isReady(due)) return Verdict::StaleDue;
    return Verdict::NotReady;
}

// ---- The sit watch (INSTALL SCENE SIT PATH, 2026-11-10).
//
// The scene used to read the player's sit ONLY until fade-up + 2200 ms, then set a "gave up" flag and never look again.
// Every later line that printed the sit (the "finished ... (player NOT seated)" line, the status line's "now") printed
// that flag, frozen at 2.9 s into the scene, beside a later timestamp. It read as a measurement at the later time and
// was not one. What b5 MEASURED is only that the player was sitting at the ~12 s status read, i.e. the sit landed
// somewhere after 2.9 s; WHEN is not measured (no number here comes from any other run). So the watch below NEVER stops
// observing while the scene runs, and no wait in this file is tuned to any observed sit time.
// Every SIT_SLEEP_STATE change is reported with its ms.
//
// RULE this encodes: before a field counts as evidence at time T, find where it is LAST WRITTEN.

// ActorState::SIT_SLEEP_STATE (commonlibf4). Only 0-3 matter to a sit; the rest are named so a trace is readable.
inline const char* SitStateName(int s)
{
    switch (s) {
    case 0: return "normal";
    case 1: return "want-to-sit";
    case 2: return "waiting-for-sit-anim";
    case 3: return "sitting";
    case 4: return "want-to-stand";
    case 5: return "want-to-sleep";
    case 6: return "waiting-for-sleep-anim";
    case 7: return "sleeping";
    case 8: return "want-to-wake";
    case -1: return "no-read";
    default: return "unknown";
    }
}
constexpr int kSitSitting = 3;
inline bool SitPending(int s) { return s == 1 || s == 2; }

struct SitWatch
{
    std::uint64_t askedAt = 0;     // scene ms of the ask (the ActivateRef at the fade-up)
    std::uint64_t windowMs = 2200; // the scene's own sit window: past it the scene plays on as the fallback
    int last = -1;                 // last state seen; -1 = nothing read yet
    std::uint64_t lastAt = 0;      // scene ms of that change
    bool seated = false;           // the engine reported sitting (3) at some read since the ask
    bool late = false;             // ... and that read came after the window had passed
    bool windowPassed = false;     // the wait ended with no sit seen (the WARN was printed): the guard, or a drop
    std::uint64_t endedAt = 0;     // scene ms of the read that ended the wait (written with windowPassed)
    bool dropped = false;          // ... because the engine went pending (1/2) and back to 0 without a sit
    std::uint64_t seatedAt = 0;    // scene ms of the first sitting read
};

struct SitStep
{
    bool changed = false;   // the state differs from the last read (the first read always counts)
    int from = -1;          // the state before the change
    bool seatedNow = false; // THIS read is the first one that saw sitting
    bool windowNow = false; // THIS read is the one at which the wait ended with no sit
    bool droppedNow = false;// ... and it ended because the engine dropped a pending sit (1/2 -> 0)
};

// One read. Order matches the old code: a sit seen on the frame the window closes counts as in the window.
inline SitStep Observe(SitWatch& w, int state, std::uint64_t el)
{
    SitStep s;
    const bool wasPending = w.last == 1 || w.last == 2;
    if (state != w.last) { s.changed = true; s.from = w.last; w.last = state; w.lastAt = el; }
    if (!w.seated && state == kSitSitting) {
        w.seated = true;
        w.seatedAt = el;
        w.late = w.windowPassed;
        s.seatedNow = true;
    } else if (!w.seated && !w.windowPassed && wasPending && state == 0) {
        // positive evidence that no sit is coming: the engine took the request and let it go
        w.windowPassed = true;
        w.endedAt = el;
        w.dropped = true;
        s.windowNow = true;
        s.droppedNow = true;
    } else if (!w.seated && !w.windowPassed && el >= w.askedAt + w.windowMs) {
        w.windowPassed = true;
        w.endedAt = el;
        s.windowNow = true;
    }
    return s;
}

// After the visit ends: what to do about a stand-up, from the LIVE state (never from what was asked for).
enum class StandPlan {
    NotAsked,     // the caller does not stand the player up (a load, a visit that never held a seat)
    Exit,         // sitting now: ask the seat for its exit, then VERIFY standing with later reads
    WatchPending, // the sit is still on its way: watch, and ask for the exit if it lands
    NotNeeded,    // not sitting and nothing pending: nothing to stand up from
};
inline const char* StandPlanName(StandPlan p)
{
    switch (p) {
    case StandPlan::NotAsked: return "not-asked";
    case StandPlan::Exit: return "exit";
    case StandPlan::WatchPending: return "watch-pending";
    default: return "not-needed";
    }
}
inline StandPlan PlanStand(bool standUp, bool haveSeat, int liveState)
{
    if (!standUp || !haveSeat) return StandPlan::NotAsked;
    if (liveState == kSitSitting) return StandPlan::Exit;
    if (SitPending(liveState)) return StandPlan::WatchPending;
    return StandPlan::NotNeeded;
}

// The post-visit watch, one read at a time, on EVENTS: ExitVerified needs a LIVE read of state 0 after the exit was
// asked; a pending sit ends on 3 (stand them up) or 0 (dropped). The two guards below only stop a watch that would
// otherwise run forever on a stuck engine state; they are never-hang guards, deliberately far beyond any exit or sit
// seen on any path, not timing estimates, and a watch that ends on one says so.
constexpr std::uint64_t kStandGuardMs = 30000;
constexpr std::uint64_t kPendingGuardMs = 30000;
enum class StandEvent { None, ExitVerified, ExitNotVerified, PendingLanded, PendingDropped };
inline StandEvent WatchStand(bool exitAsked, int liveState, std::uint64_t ageMs)
{
    if (exitAsked) {
        if (liveState == 0) return StandEvent::ExitVerified;
        if (ageMs >= kStandGuardMs) return StandEvent::ExitNotVerified;
        return StandEvent::None;
    }
    if (liveState == kSitSitting) return StandEvent::PendingLanded;
    if (!SitPending(liveState) || ageMs >= kPendingGuardMs) return StandEvent::PendingDropped;
    return StandEvent::None;
}

// ---- Fix B: the beats wait for the SIT ITSELF (the lead, 2026-11-10).
// b5: the scene asked at ~700 ms, stopped reading at ~2900 ms, and the status line ~12 s after the commit read
// `player sitting 1` beside the frozen `gave up 1`: the sit landed somewhere after 2.9 s (WHEN is not measured). So the
// beat clock HOLDS just before the work beat (her step-in and her line run on: she talks while the player sits down)
// and resumes on the EVENT: the first sitting read. It is NOT tuned to any observed sit time. It stops waiting only on
//   - positive evidence that no sit is coming: the engine went pending (1/2) and back to 0 (Observe's drop), or
//   - kSitHangMs, a never-hang guard (a broken engine state must not lock the player out for good). It is deliberately
//     far above any sit seen on any path, so that it never decides a normal sit; it is not a timing estimate.
// The hold lengthens the lockout by exactly the time held; the scene logs the new lockout so a capture is read against
// it. The 9 s lockout cap is QD-B's to rule on, not this file's.
constexpr std::uint64_t kSitHangMs = 15000;
constexpr std::uint64_t kSitMaxMs = kSitHangMs;   // the scene's wait = the hang guard; kept as the name the scene uses
inline bool GateHolds(bool asked, const SitWatch& w, bool skipped)
{
    return asked && !w.seated && !w.windowPassed && !skipped;
}

// ---- The native camera switch with an OBSERVED camera (the lead, 2026-11-10; docs/integration/SCENE-CAMERA-NATIVE-3P.md).
// The scene used to ask Papyrus Game.ForceThirdPerson through the VM queue and log only "queued". b2/b3 in-scene stills read
// "camera firstperson" after it. Now the DLL calls PlayerCamera::SetState itself and this watch reads the camera EVERY
// frame from the request: the frame 3P is first seen, every state change with its time, and, after 3P was reached, any
// frame the camera leaves it again (something fights the switch, e.g. a talk-menu camera: the open question this exists
// to answer). Nothing here is tuned to a number: the guard only stops a watch on a stuck state.

// CameraStates::CameraState (commonlibf4 a08c731, include/RE/C/CameraStates.h). -1 = no state matched.
inline const char* CamStateName(int s)
{
    switch (s) {
    case 0: return "firstperson";
    case 1: return "autovanity";
    case 2: return "vats";
    case 3: return "free";
    case 4: return "ironsights";
    case 5: return "pctransition";
    case 6: return "tween";
    case 7: return "animated";
    case 8: return "thirdperson";
    case 9: return "furniture";
    case 10: return "mount";
    case 11: return "bleedout";
    case 12: return "dialogue";
    default: return "unknown";
    }
}
constexpr int kCamFirst = 0, kCamThird = 8;
constexpr std::uint64_t kCamGuardMs = 10000;   // never-hang guard for the "not yet seen" WARN, not an estimate

struct CamWatch
{
    bool asked = false;          // a SetState was issued
    int want = -1;               // the state asked for
    std::uint64_t askedAt = 0;   // ms of the request (same clock as the reads)
    int last = -2;               // last state read; -2 = nothing read yet
    bool seen = false;           // `want` was read at least once after the request
    std::uint64_t seenAt = 0;    // ms of that first read
    int framesToSeen = 0;        // reads from the request to it (frames, when read once a frame)
    int reads = 0;
    bool guardPassed = false;    // the guard passed with `want` never read
    int lost = 0;                // times the state LEFT `want` after it had been seen (a fight)
};

struct CamStep
{
    bool changed = false; int from = -2;
    bool seenNow = false;   // the first read of `want`
    bool guardNow = false;  // the guard passed on this read, `want` never read
    bool lostNow = false;   // this read left `want` after it had been seen
};

inline void CamAsk(CamWatch& w, int want, std::uint64_t at)
{
    w = CamWatch{};
    w.asked = true; w.want = want; w.askedAt = at;
}

inline CamStep CamObserve(CamWatch& w, int state, std::uint64_t t)
{
    CamStep s;
    if (!w.asked) return s;
    ++w.reads;
    if (state != w.last) { s.changed = true; s.from = w.last; }
    const int prev = w.last;
    w.last = state;
    if (!w.seen && state == w.want) {
        w.seen = true; w.seenAt = t; w.framesToSeen = w.reads; s.seenNow = true;
    } else if (w.seen && state != w.want && prev == w.want) {
        ++w.lost; s.lostNow = true;
    } else if (!w.seen && !w.guardPassed && t >= w.askedAt + kCamGuardMs) {
        w.guardPassed = true; s.guardNow = true;
    }
    return s;
}

}  // namespace cw::scenepkg
