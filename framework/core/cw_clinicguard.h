// cw_clinicguard.h - the clinic package NUDGE's "is she actually loaded" gate, pure so cwtest drives it.
//
// WHY (2026-11-10, the SanctuaryExt / DiamondCityExt crash hunt).
// clinic::Nudge() asks the engine to re-evaluate Varga's AI package (`<ref>.evp`) when the clock wants a different one
// of our packages than she is running. Its whole gate was:
//     if (!want || !actor || !actor->currentProcess || talking || actor->IsInCombat()) return;
// There is NO check that her 3D is loaded. StallCheck, three functions below, has one (`!ref->Get3D()` -> return).
// So the nudge could ask the engine to pick a package, and therefore BUILD A PATH, for an actor who is not loaded and
// whose cell and navmesh are not in memory. The measured fuse fits: kNudgeMs is 15000, and the beta rehearsal crashed
// at a constant 15-16 s after a SanctuaryExt arrival (5 of 6), inside the pathing server
// (BSPathing::BuildPath / PathingTaskData::Process / BSPathingAvoidNodeArray::ComputeCenter). The ONE trial that
// survived is the one that logged "nudge 2 of 3".
//
// It also explains the asymmetry that broke the earlier theories: arriving at DIAMOND CITY the player is next to
// Varga, her 3D is loaded and she already runs the wanted package, so Nudge returns early and never fires. Arriving at
// SANCTUARY she is far away and unloaded, the engine never re-picks, her package is stale -- and the nudge fires on an
// unloaded actor. DC 0/N, Sanctuary 5/6.
//
// SHOULD THE NUDGE EVER EVALUATE WHILE SHE IS UNLOADED? No. The purpose is to fix a package that went stale across a
// cell load, and a stale package on an unloaded actor is invisible and harmless -- nothing of hers is simulated or
// drawn. The moment it starts to matter is the moment her 3D loads, so that is when the nudge should get its chance.
// Hence: never evaluate while unloaded, and RE-ARM on the unloaded -> loaded edge so the first frame she is real gets
// an immediate nudge rather than waiting out kNudgeMs.
#pragma once

namespace cw::clinicguard {

// Remembers whether she was loaded on the previous observation. Game thread only, like the rest of the plugin.
struct NudgeLive
{
    bool was = false;   // her 3D was loaded last time we looked
};

struct NudgeGate
{
    bool skip = false;    // she is not loaded: do not evaluate anything
    bool rearm = false;   // her 3D has just appeared: let the nudge run now instead of waiting out kNudgeMs
};

inline NudgeGate ObserveLive(NudgeLive& s, bool live)
{
    NudgeGate g;
    if (!live) {            // unloaded: skip, and forget she was ever loaded so the next load re-arms
        s.was = false;
        g.skip = true;
        return g;
    }
    if (!s.was) {           // the unloaded -> loaded edge: a stale package becomes visible exactly now
        s.was = true;
        g.rearm = true;
    }
    return g;
}

}  // namespace cw::clinicguard
