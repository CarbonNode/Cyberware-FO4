// cw_evpgate.h - the ONE rule every `<ref>.evp` in this plugin must pass, pure so cwtest drives it.
//
// THE LAW: never ask the engine to re-evaluate an actor's AI package while that actor's 3D is away.
// `evp` makes the engine pick a package and therefore BUILD A PATH. An unloaded actor's cell and navmesh are not in
// memory, so the pathing server walks freed or absent navmesh data. core/cw_clinicguard.h carries the measurement
// that found this: the beta rehearsal crashed at a constant 15-16 s after a SanctuaryExt arrival, 5 of 6 trials,
// inside BSPathing::BuildPath / PathingTaskData::Process.
//
// WHY THIS HEADER EXISTS SEPARATELY FROM cw_clinicguard.h (2026-11-12, the navmesh-crash attribution).
// cw_clinicguard.h fixed the clinic nudge on 2026-11-10 and stated the law in its own comments -- but it stated it
// for ONE call site. cw_scene.cpp has its OWN copy of the same `%08X.evp` + Console::ExecuteCommand helper, with four
// call sites, and only one of them was gated:
//     cw_scene.cpp:663  scene start   - GUARDED, behind Ready()'s "the doctor is not loaded here"
//     cw_scene.cpp:396  EndRun        - UNGUARDED (EndRun runs on the menu closing, ON AN ABORT, AND ON A LOAD)
//     cw_scene.cpp:1071 back-beat end - UNGUARDED
// Both unguarded sites resolve the doctor from a STORED FormID (g_live.docRef / g_holdDoc) through
// TESForm::GetFormByID, which happily returns a reference whose 3D is long gone. A load is precisely the moment her
// 3D is gone and her cell's navmesh is not in memory, so the worst case sat on the commonest path.
//
// A LAW THAT LIVES IN ONE FILE'S COMMENTS IS NOT A LAW. So: the decision lives here, every evp emitter calls it, and
// tools/check_evp_gated.py refuses a commit where an evp emitter does not. That is what stops the next copy of this
// helper from shipping ungated, which is the actual defect -- the gate was known, written down and measured, and the
// second call site still went out without it.
//
// SKIPPING IS SAFE, AND COSTS NOTHING. A stale package on an unloaded actor is invisible: nothing of hers is
// simulated or drawn. The moment it starts to matter is the moment her 3D loads, and the clinic nudge already
// re-arms on exactly that unloaded -> loaded edge (cw_clinicguard.h ObserveLive's `rearm`, cw_clinic.cpp), then
// evaluates her off a stale package onto the one her clock wants (cw_clinic.cpp's step-in handling covers the
// scene's own package). So a skipped evp here is recovered for free; an evp on an unloaded actor is a crash.
#pragma once

namespace cw::evpgate {

enum class Verdict
{
    Allow,      // she is here and real: the engine may re-pick her package
    NoRef,      // the FormID did not resolve to a reference
    NotLoaded,  // the reference exists but its 3D is away
};

// refResolved: the stored FormID gave us a TESObjectREFR at all.
// live: that reference's Get3D() is non-null RIGHT NOW, read at the moment of the call and never cached.
inline Verdict Check(bool refResolved, bool live)
{
    if (!refResolved) return Verdict::NoRef;
    if (!live) return Verdict::NotLoaded;
    return Verdict::Allow;
}

inline bool Allowed(Verdict v) { return v == Verdict::Allow; }

// The reason, phrased for the log so every site refuses in the same words.
inline const char* Why(Verdict v)
{
    switch (v) {
    case Verdict::NoRef: return "the reference did not resolve";
    case Verdict::NotLoaded: return "its 3D is away (evp would path an unloaded actor)";
    default: return "allowed";
    }
}

}  // namespace cw::evpgate
