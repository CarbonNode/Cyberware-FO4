// cw_stuck.h - the arms move-stuck watchdog and its recovery policy, pure so cwtest replays episodes
// through the same decision the plugin's Tick runs (beta update 1, 2026-11-12).
//
// THE DEFECT IT COVERS (a player report of being stuck in place with the Mantis arms, 2026-11-12;
// reproduced from the session log): after the install's equip + auto weaponDraw, the player was
// planted on held input. TWELVE "arms: move STUCK"
// lines, 14:38:46.7 to 14:42:10.5, each a ~1000 ms window with 0-3 u moved, the first of them 10 s
// BEFORE his first hit, so the equip edge is the trigger and combat is not. bGraphDrivenTranslation
// read 1 on EIGHT of the twelve and FAILED TO READ ("?" in the log) on FOUR. (An earlier revision of
// this header said eleven and seven: it miscounted by one. Re-counted line by line 2026-11-13 against
// Cyberware.log lines 213, 218, 231, 248, 261, 271, 274, 275, 278, 281, 282, 285.)
//
// These are TWELVE SEPARATE EPISODES, not one: MoveWatch's `logged_` resets whenever moveMode hits 0
// or a gameplay menu opens, and his position moved hundreds of units between them (1307,-807 ->
// 1480,-837 -> 1503,-914 -> 1272,-776). He re-wedged repeatedly. The genuinely rooted stretch is
// 14:40:21-14:41:48: 87 s at (1503,-914), with a sheathe and a re-draw inside it. A recovery that
// fires ONCE is therefore not enough, and one that fires constantly is its own failure (the author's rule) -
// hence Recoveries below, which counts and rate-limits every clear so a grader can see the rate.
//
// The plugin never SETS these flags: the only writers in the whole framework are the test-only Undrive
// (main.cpp) and the `animvar` test seam. The engine leaves them set around an arm weapon's equip /
// weapon-subgraph change. NOT ROOT-CAUSED on the engine side: which internal state holds the flag, and
// why a later draw cleared it at 14:42:11, are unknown. A2's in-game repro is the discriminator.
//
// The "load: SpeedMult base 0.0 perm 0.0 now 0.0" warnings in that log are a READ-TIMING ARTEFACT, not
// the cause: the beta soak's brand-new game read 0.0 the same way (cw-int-5 own-save observation
// 2026-11-11T2103Z) and a new game has the default 100. Do not re-diagnose from that line.
//
// WHAT THIS FIX DOES AND DOES NOT DO, in plain words (the lead's ruling 2026-11-13, after the rig
// operator's negative control):
//   IT DOES free him when the engine's flag READS SET. That is 8 of his 12 readings, including the
//     very first one at 14:38:46, 11.1 s after the equip - so it fires at the earliest moment it could.
//   IT DOES NOTHING when the flag cannot be read ("?"). It logs the refusal with all four flag words.
//   WALKING INTO A WALL NEVER TRIGGERS IT. The operator logged the wall signature four times on
//     2026-11-13 - bAnimationDriven=0 bGraphDriven=0 bGraphDrivenTranslation=? bFreezeSpeedUpdate=0,
//     with and without an arm - and it is BYTE-FOR-BYTE the same snapshot as his four "?" episodes.
//     An earlier revision of this file cleared on exactly that snapshot, so it would have fired every
//     time he leaned on a wall, up to once every kMinGapMs. That branch is deleted.
//   IT DOES NOT COVER a wedge whose flag never reads set. If he is rooted and all three mover flags
//     read a clean 0, the log says "not this defect" and nothing is cleared - because it is not this
//     defect, and something else is holding him.
// The three "?" episodes inside the 87 s rooted stretch are still recovered, 29.5 s later than the
// deleted branch would have: the next episode at the same spot (14:40:50) reads the flag set.
//
// The recovery REUSES Undrive - it is NOT a second clearing mechanism. [Arms] bStuckUndrive keeps its
// old diagnostic force byte for byte (LegacyDriven below is E's own `driven` expression). It was only
// ever a test key because its repro was never graded ("diagnosis only (cw-int-2 T4); no pass",
// docs/GATED-KEYS-arms.md; cw-int-2.md's row reads "T4 inconclusive (player did not move)") - not
// because of a recorded failure. What changes is that the shipped build also recovers when an arm
// weapon is involved (kAutoRecoverArms) and nothing legitimate owns the body.
#pragma once
#include <cmath>
#include <cstdint>

namespace cw::stuck {

constexpr double kStuckUnits = 20.0;        // under this in a window = stuck (the inline watchdog's threshold, cw-int-2)
constexpr std::uint64_t kWindowMs = 1000;   // the sample window
constexpr std::uint64_t kMinGapMs = 2000;   // no two clears closer than this (the rate a grader judges)
constexpr std::uint64_t kMinuteMs = 60000;  // the window Recoveries counts a firing rate over

// THE RECOVERY IS TWO STEPS, not one (rig operator's A1 boot, 2026-11-13, and the lead's design note on it).
// Measured on the SAME build and save, only the overlay differing:
//   plain Test overlay: `arm equip settle -> clear (flags known set)` at +2.55 s, then `cleared the driven flags`,
//                       then he WALKS DRAWN at 450.4 u of a 540.2 u baseline. No STUCK line at all.
//   beta mix:           every mover flag a positive 0, four refusals, 0.6 u. Nothing to clear, nothing cleared.
// So under the Test overlay the graph DID set the driven flags and the EXISTING clear freed him. The re-pick is
// therefore step one - it selects a graph that then sets the flags - and the clear is step two. A fix that only
// re-picks hands the player straight into the flags-set wedge this file already treats, so the two must chain,
// and the rate limit that exists to stop a clear firing constantly must not be what strands him between them.
constexpr std::uint64_t kPairWindowMs = 4000;   // a clear this soon after a re-pick is STEP TWO, never a repeat

// THE RE-PICK GIVES UP (the lead's requirement, 2026-11-13: "it must not loop"). The rate limit alone bounds the
// INTERVAL, not the TOTAL: a player who stays stuck for any reason the re-pick cannot cure - a console pin during
// SYN-1, or a wedge with a different cause - would otherwise be re-picked every kMinGapMs for as long as he stood
// there, replaying a weaponDraw + attackStop each time. That is the author's own rule failing in the other direction:
// "a recovery that fires constantly is a failure even if he can move". So after this many attempts inside one
// minute the re-pick stands down, says so ONCE, and lets the minute roll over before trying again. Three is chosen
// so the two-step sequence (re-pick, then the clear) always has room to complete twice over before giving up.
constexpr std::uint32_t kMaxRepicksPerMinute = 3;

// The recovery switch, flipped by the fix commit of beta update 1. FALSE is E's shipped behaviour (no
// recovery without the test ini key); the cwtest replay of the beta episode FAILS while it is false and
// PASSES when true, and that fail/pass pair is the defect's harness record.
inline constexpr bool kAutoRecoverArms = true;

// ---------------------------------------------------------------- CASE 6 IS NOW TREATED, 2026-11-13
// THE SECOND MECHANISM, and it is NOT a driven flag. The rig operator measured R1b on 106f23cc under the verified
// beta mix: an arm DRAWN in third person walks 0.6 u (Mantis), 3.3 u (Monowire), 7.8 u (Gorilla) against his own
// same-boot baseline of 509.5 u, with moveMode A6 set and EVERY graph flag reading a positive 0 -
// bAnimationDriven=0 bGraphDriven=0 bGraphDrivenTranslation=0 bGraphDrivenRotation=0 bFreezeSpeedUpdate=0.
// Decide() answered RefusedNothing, correctly: there is nothing to undrive. Nothing drives the actor at all.
//
// WHY IT IS GRAPH SELECTION. RepickWeaponGraph (main.cpp) carries the engine law: a keyword added or removed at
// runtime does NOT re-pick the weapon subgraph - the game re-chooses only when the graph or equip state is
// rebuilt. So a drawn arm can leave the actor on a subgraph that supplies no locomotion: the game believes he is
// moving (moveMode set), no clip translates him, and no flag claims to drive him. A SHEATHE rebuilds the equip
// state, which is the engine's own re-pick trigger - which is exactly why sheathing frees him in one frame.
//
// WHY IT ONLY SHOWED UP NOW, and it is not a regression: the ONLY caller of RepickWeaponGraph in the whole
// framework is the queue, and every path that fed the queue was gated off at beta values - the leap-target loop
// and the slam behind [Arms] bLeapRepick, the PLS behind bPlsRepick, AND the stuck recovery's own re-pick behind
// bLeapRepick as well. Under the beta mix the re-pick was unreachable and the engine was never asked to
// re-choose; under the plain Test overlay it fires from four places and masked the whole defect. The ~450 u
// "fixed" reading of 05:03Z came from that masking, not from any recovery.
//
// SO THE REMEDY IS TO RE-SELECT THE GRAPH, NOT TO CLEAR A FLAG. This is the switch for it. It is deliberately a
// separate constant from kAutoRecoverArms: they treat two different mechanisms, and a future session must be
// able to turn one off without the other.
// MEASURED IN GAME, 2026-11-13 09:35Z, rig operator, on a HEALTHY boot with ZERO `move STUCK` lines:
// install an arm and press W 170 ms later, so the player is WALKING through the draw. At +2.5 s the EQUIP SETTLE
// read `every mover clear` - which is the ordinary healthy reading whenever the draw lands inside the settle
// window - routed it to a re-pick, armed the session flag off that false trigger, and the proactive draw path
// then re-picked ONCE A SECOND, TWELVE TIMES, over 13.2 s. He walked 492.6 u throughout; nothing was wedged.
// Twelve weaponDraw + attackStop replays on an ordinary install-and-walk.
//
// SO THE RELEASE IS REACTIVE-ONLY (the lead's ruling): the settles may CLEAR flags and may never RE-PICK, the
// proactive draw re-pick is gone from the release entirely, and the one and only route to a re-pick is
// MoveWatch's MEASURED `move STUCK` with our arm drawn and every mover clear - rate-limited by kMinGapMs, capped
// at kMaxRepicksPerMinute, then STANDING DOWN. Probe::measuredStuck makes that structural rather than a habit:
// a caller with no measurement behind it cannot reach the branch at all.
//
// WHY IT WAS MISSED, worth keeping: the proactive path had its own guard (ProactiveRepickDue, which refuses a
// MOVING player) and both I and the release review tested THAT guard. The equip settle never passed through it - it
// called Decide() directly. We both verified the gate and neither of us enumerated the CALLERS.
// ⛔ THE AUTOMATIC RE-PICK IS OFF FOR THIS RELEASE. It is the only remedy we have for case 6 and it has NEVER
// been observed working: the wedge it treats could not be reproduced in five boots, and in a HEALTHY boot it
// misfired - twelve draw replays from the settle route, and three more from a measured `move STUCK` raised by
// attacking in place with the camera off axis. A re-pick replays the draw flourish AND its sound, so three of
// them in the middle of a fight is worse than no net for a wedge nobody can summon (the lead, 2026-11-13).
//
// THERE ARE TWO ROUTES AND BOTH MUST BE OFF. Missing the second is exactly the mistake the release review caught:
// "MoveWatch is the only route" was FALSE. Enumerated from `grep -n "RequestRepick(" main.cpp`, every caller:
//   :2539 SetLeapReady   "leap ready"/"leap off"  - GAMEPLAY, behind [Arms] bLeapRepick
//   :2751 PlsRepick                               - GAMEPLAY, behind [Arms] bPlsRepick
//   :2992 SetSlamReady   "slam ready"/"slam off"  - GAMEPLAY, behind (bLeapRepick || bSlamNoRepick)
//   :4956 the SPRINT-stuck watchdog "stuck recovery" - RECOVERY, and it never calls Decide, so
//                                                      kAutoRepickGraph alone does NOT reach it
//   :5025 the WALK watchdog via Decide/recover()      - RECOVERY, gated by kAutoRepickGraph
// The three gameplay routes are features the player's own ini controls and are all 0 in the beta; they are not
// "the mod trying to free him" and they stay as they are. The two RECOVERY routes are these constants.
//
// WHAT STAYS ON, deliberately: MoveWatch still MEASURES and still LOGS `arms: move STUCK ...` with all five
// flags, the sprint watchdog still logs `arms: sprint STUCK ...`, and the policy still logs its decision. If
// the author wedges, his log says so - the lead's explicit condition for shipping with this off. And
// kAutoRecoverArms is UNTOUCHED, so the flags-set CLEAR still runs: that is the one recovery ever observed
// freeing the player (the Test overlay's `clear (flags known set)` at +2.55 s). Only the never-proven re-pick
// is withdrawn.
inline constexpr bool kAutoRepickGraph = false;          // the WALK watchdog's route, through Decide
inline constexpr bool kAutoRepickOnSprintStuck = false;  // the SPRINT watchdog's route, which never asks Decide

// A graph variable read is THREE-valued, because GetGraphVariableImplBool can fail: the log prints "?"
// for that and it happened on 4 of the author's 12 episodes. A failed read is UNKNOWN - it is NOT evidence
// that the flag is clear, and it is NOT evidence that it is set either. Collapsing the five reads into
// one AND-ed `ok` made the first cut of this fix read "nothing is set" on those four; acting on the
// unknown made the second cut clear on walls. Three values, and only a POSITIVE read may act.
enum class Tri : std::uint8_t { Unknown, False, True };

inline bool IsTrue(Tri t) { return t == Tri::True; }
inline bool IsUnknown(Tri t) { return t == Tri::Unknown; }
inline const char* TriWord(Tri t) { return t == Tri::True ? "1" : t == Tri::False ? "0" : "?"; }

// The locomotion graph's "who moves the actor" flags, one snapshot. bGraphDrivenRotation is logged by
// DrivenVars but is deliberately NOT part of any decision here - E's own `driven` excluded it, and
// rotation alone never roots a player.
struct Driven {
    Tri animationDriven = Tri::Unknown;          // a real animation owns the body: a GUARD, never cleared on
    Tri graphDriven = Tri::Unknown;
    Tri graphDrivenTranslation = Tri::Unknown;   // the flag observed set on 7 of 11 episodes, unreadable on 4
    Tri freezeSpeed = Tri::Unknown;

    // E's own `driven` expression, preserved exactly for [Arms] bStuckUndrive: any of the four
    // non-rotation flags read OK and true.
    bool LegacyDriven() const
    {
        return IsTrue(animationDriven) || IsTrue(graphDriven) || IsTrue(graphDrivenTranslation) || IsTrue(freezeSpeed);
    }
    // The three flags that actually move the actor and that a clear would touch (animationDriven is a
    // guard, not a target).
    bool AnyMoverKnownSet() const { return IsTrue(graphDriven) || IsTrue(graphDrivenTranslation) || IsTrue(freezeSpeed); }
    int MoversUnknown() const
    {
        return (IsUnknown(graphDriven) ? 1 : 0) + (IsUnknown(graphDrivenTranslation) ? 1 : 0) + (IsUnknown(freezeSpeed) ? 1 : 0);
    }
};

// What the tick knows about the world, per sample. Every field is a POSITIVE probe from a call already
// proven in main.cpp - nothing here is inferred.
//   attackInFlight: the leap clip window, a finisher, the ground slam, or a pending re-pick.
//   sceneBusy:      the install scene, chair or 2D fallback (scene::Active()).
//   menuOpen:       GameplayMenuOpen(), whose list already covers PauseMenu, VATSMenu, DialogueMenu,
//                   PipboyMenu, TerminalMenu, the workbench/barter/container menus and LooksMenu.
//   inPowerArmor:   InPowerArmor(p) (main.cpp:2619, kPowerArmor extra data).
//
// STATES WITH NO PROBE, and therefore UNTESTED here: swimming, furniture other than our own chair,
// knockdown/ragdoll beyond the bAnimationDriven proxy, a grenade throw mid-clip, and a weapon swap in
// flight. CommonLibF4 is not in git (framework/xmake.lua fetches it on the build rig) and is on no other
// machine, so a member name for those cannot be verified off the compile lane; guessing one would
// be a build break nobody here could catch. They are covered only insofar as they set bAnimationDriven
// or bGraphDriven, and they are named in the criteria doc's UNTESTED list rather than silently assumed.
struct Probe {
    std::uint32_t moveMode = 0;
    bool armEquipped = false, menuOpen = false, lungeActive = false, attackInFlight = false;
    bool sceneBusy = false, airborne = false, loadingSave = false, inPowerArmor = false;
    bool testUndrive = false;   // [Arms] bStuckUndrive
    // CASE 6, 2026-11-13. The wedge the rig operator measured is a property of the DRAWN state of one of our arms
    // in third person: sheathing frees the player instantly (526.8 u, 103% of his own baseline) and re-drawing
    // wedges him again. Both are needed, because a drawn arm is what selects the weapon subgraph, and 1P was
    // never measured (his F key would not switch cameras while an arm was drawn, so there is no 1P reading at
    // all - do not assume this happens there).
    bool armDrawn = false;      // our arm weapon is DRAWN (GetWeaponMagicDrawn)
    // A RE-PICK REQUIRES A MEASURED STUCK EPISODE, AND THE TYPE NOW SAYS SO (the lead, 2026-11-13 09:37Z).
    // Only MoveWatch sets this, and only after it has measured under kStuckUnits in kWindowMs. The settle paths
    // leave it false, so they CANNOT reach the re-pick however the graph reads. See the comment on kAutoRepickGraph:
    // in game the equip settle routed the ordinary healthy reading to a re-pick and replayed the draw twelve times.
    bool measuredStuck = false;
    bool firstPerson = false;   // camera in 1P: UNMEASURED for this wedge, so the re-pick stays out of it
};

// Why the policy did what it did. Logged on every stuck episode so the grader can tell a recovery from
// a refusal from "he was stuck and it was NOT these flags", which is a different defect.
enum class Decision : std::uint8_t {
    NoClear,            // the fix is off, no arm is in, or the test key found nothing set
    ClearKnown,         // a mover flag is KNOWN set: clear it. The ONLY decision that clears.
    RefusedGuard,       // something legitimate owns the body
    RefusedIncomplete,  // too little is known to act safely (a guard's own state is unknown)
    RefusedNothing,     // stuck, but every mover flag is KNOWN clear, and nothing else applies
    RepickGraph         // every mover flag KNOWN clear with our arm DRAWN: re-SELECT the graph (case 6)
};

// IS THIS ANSWER FINAL, or does it mean "ask again in a second"? The settle retries once a second until it
// gives up, and before 2026-11-13 it retried through EVERY refusal -- four lines per arming. Two of the five
// answers are complete reads of a healthy graph: RefusedNothing says every mover reads clear, NoClear says the
// policy will not touch this player at all (no arm of ours in, or the feature off). Neither can change inside the
// window, so retrying them only fills the log. RefusedGuard and RefusedIncomplete are the genuinely temporary
// ones -- something owns the body, or the read came back mid-transition -- and they keep their retries.
inline bool Settled(Decision d)
{
    return d == Decision::ClearKnown || d == Decision::RefusedNothing || d == Decision::NoClear;
}

inline const char* Word(Decision d)
{
    switch (d) {
        case Decision::ClearKnown:        return "clear (flags known set)";
        case Decision::RefusedGuard:      return "refused (something owns the body)";
        case Decision::RefusedIncomplete: return "refused (graph read too incomplete to act)";
        case Decision::RefusedNothing:    return "refused (graph flags all known clear: not this defect)";
        case Decision::RepickGraph:       return "re-pick the weapon subgraph (every mover clear: graph selection, not a driven flag)";
        default:                          return "no clear";
    }
}

// May the driven flags be cleared right now, and why? The ONE decision every caller uses (the walk
// episode and the equip settle).
//
// A FAILED READ NEVER CLEARS (the lead's ruling 2026-11-13, which WITHDRAWS the 2026-11-12 ruling that
// an unknown read may clear when every guard is positively clear). The reason is the rig operator's
// negative control: pushing into a wall produces the unknown-translation snapshot every time, with or
// without an arm, so a policy that clears on it fires in ordinary play. Only a flag that POSITIVELY
// reads true may be cleared; any unreadable mover is RefusedIncomplete - do nothing, and log it with
// all four flag words so a grader can see the "?" and the refusal together.
inline Decision Decide(const Probe& pr, const Driven& dr)
{
    // [Arms] bStuckUndrive keeps its old diagnostic FORCE, on E's exact condition, byte for byte: any of the four
    // non-rotation flags reading true clears, with no guard and no arm needed.
    //
    // WHAT CHANGED 2026-11-13, and it was a real defect in its own right: this used to be a single early return,
    // so setting the key to 1 ALSO disabled kAutoRecoverArms and (now) the case-6 re-pick on every path. The test
    // overlay ships it 1, so the test configuration had strictly LESS recovery than the beta - and the only reason
    // nobody noticed is that the test overlay separately masked the wedge through the re-pick. Now the force is
    // tried first and, when E's condition finds nothing, the policy CONTINUES instead of giving up. Identical
    // behaviour whenever the force applies; strictly more recovery when it does not.
    if (pr.testUndrive && dr.LegacyDriven()) return Decision::ClearKnown;

    if (!kAutoRecoverArms) return Decision::NoClear;   // E's shipped behaviour until the fix commit
    if (!pr.armEquipped) return Decision::NoClear;     // never touch a player without our arms in

    // GUARDS, in order. A real animation owning the body outranks everything: a stagger, knockdown,
    // ragdoll or furniture animation must play out, and clearing under one would be the worse bug.
    if (IsTrue(dr.animationDriven)) return Decision::RefusedGuard;
    if (IsUnknown(dr.animationDriven)) return Decision::RefusedIncomplete;   // a guard whose own state is unknown stops us
    if (pr.menuOpen || pr.lungeActive || pr.attackInFlight || pr.sceneBusy || pr.airborne
        || pr.loadingSave || pr.inPowerArmor)
        return Decision::RefusedGuard;

    // Is there anything to clear?
    if (dr.AnyMoverKnownSet()) return Decision::ClearKnown;

    if (dr.MoversUnknown() == 0) {
        // CASE 6. READ THE PREMISE CORRECTLY, because the obvious reading is wrong and the next person will be
        // tempted to "fix" it (release review, 2026-11-13). This branch does NOT claim the movers are truly clear. Two
        // boots showed the SAME drawn-Mantis wedge with OPPOSITE flag readings - one cleared itself at +1.03 s with
        // the flags readable and SET, the other read a positive 0 on every check and persisted past 30 s - so a
        // read that SUCCEEDS can still return the wrong value, and no three-valued type can catch that (Tri only
        // protects against a read that FAILS).
        // What this branch actually means is: OUR ARM IS DRAWN, HE IS STUCK, AND A CLEAR IS NOT AVAILABLE - so try
        // the other remedy. Read that way both boots are covered by design rather than by luck: where the flags
        // read set the clear fires, where they read 0 - truly clear or misread - the re-pick fires, and the player
        // gets a remedy either way. The policy never has to be right about WHY the clear was unavailable.
        // STUCK stays a hard conjunct, and that is not optional: a HEALTHY boot sits in this very state while
        // walking fine (T2 walked 450.5 u with four of these refusals), so "drawn + clear" alone is not a wedge.
        // Sheathed, in 1P, or with the re-pick switched off it stays RefusedNothing, which is still honest.
        // measuredStuck FIRST, because it is the fact everything else rests on: no measurement, no re-pick, and
        // the answer falls back to exactly what shipped before the re-pick existed.
        if (kAutoRepickGraph && pr.measuredStuck && pr.armDrawn && !pr.firstPerson) return Decision::RepickGraph;
        return Decision::RefusedNothing;   // he is stuck, but not by these flags
    }
    // Any unreadable mover: do nothing. This is the wall snapshot as well as his four "?" episodes -
    // they are indistinguishable, which is exactly why neither may clear.
    return Decision::RefusedIncomplete;
}

inline bool Clears(Decision d) { return d == Decision::ClearKnown; }

// ---------------------------------------------------------------- WHEN THE PROACTIVE RE-PICK MAY ACT
// THE WEDGE IS A PROPERTY OF THE BOOT, NOT OF THE DRAW (rig operator, 2026-11-13, and this is measured):
// one boot wedged on all three arms across repeated samples ~30 s apart and did not recover for ~30 s; the
// NEXT boot, on a byte-identical overlay and the same save, WALKED on both draw timings (395.2 u and 450.5 u).
// Within a single boot the same install produced BOTH flag outcomes minutes apart. So it is consistent within a
// boot and opposite across boots.
//
// WHY THAT CHANGES THE DESIGN. `RepickGraph` means "every mover reads a positive 0 with our arm drawn", and a
// HEALTHY boot sits in exactly that state while walking perfectly well - the operator's T2 walked 450.5 u with
// four of those refusals. So the state alone is NOT evidence of a wedge, and a proactive re-pick fired on it
// would replay a weaponDraw + attackStop on EVERY draw of a healthy session for nothing. That is a quality
// failure by the author's own rules, and it also fires the one primitive that has planted the player before (the
// re-pick freeze, 2026-11-08) thousands of times a session with no cause.
//
// SO: THE PROACTIVE PATH ARMS ONLY AFTER THIS SESSION HAS ACTUALLY SEEN THE WEDGE (the lead, 07:35Z) - that is,
// after one REACTIVE re-pick has fired, where MoveWatch had already measured him planted. A healthy boot never
// arms it and pays nothing; a wedged boot pays one reactive episode and then every later draw is pre-empted at
// rest. The reactive watchdog remains the backstop in both.
//
// A SAVE RELOAD DOES NOT CLEAR IT, DELIBERATELY. The state is uniform per BOOT, so a session that has wedged
// once will wedge again; re-learning it after every reload would hand the player the first-episode freeze again
// on each load, which is the worst of both designs. Only the process ending clears it (a function-local static).
inline bool ProactiveRepickDue(const Probe& pr, const Driven& dr, bool wedgeSeenThisSession)
{
    if (!wedgeSeenThisSession) return false;   // a boot that has never wedged pays no replay at all
    if (pr.moveMode != 0) return false;        // only at REST, so the replay never shows; moving is the reactive path's
    return Decide(pr, dr) == Decision::RepickGraph;
}
// A re-pick is NOT a clear and must never be counted as one: it touches no flag, and it carries a cost a clear
// does not (the engine replays a quick weaponDraw + attackStop). Kept apart so a grader reads two rates, not one.
inline bool Repicks(Decision d) { return d == Decision::RepickGraph; }

// Has the re-pick already tried its limit inside this minute? Checked BEFORE Recoveries::Allow, so a stood-down
// attempt is not counted as a suppression - the two mean different things to a grader: suppressed = too soon,
// stood down = tried enough and it is not working.
inline bool RepickStoodDown(std::uint32_t inMinute) { return inMinute >= kMaxRepicksPerMinute; }

// Counts and rate-limits recoveries. The author's rule: "a recovery that fires constantly is a failure even
// if he can move", so every clear is counted, the per-minute rate is kept, and two clears closer than
// kMinGapMs are suppressed (the suppressed ones are counted too - a high suppressed count is itself the
// signal that the engine is re-setting the flag faster than we clear it).
class Recoveries {
public:
    // `paired` = this clear is step two of a re-pick that just fired (see kPairWindowMs). It still counts and is
    // still logged, so a grader sees the real rate; it only escapes the gap, because the gap exists to stop a
    // clear REPEATING, and step two of a two-step recovery is not a repeat. Everything else is unchanged.
    bool Allow(std::uint64_t nowMs, bool paired = false)
    {
        if (!paired && total_ && nowMs - lastAt_ < kMinGapMs) { ++suppressed_; return false; }
        // `started_`, not a zero sentinel: GetTickCount64() can legitimately be small, and a zero-valued
        // minuteStart_ read as "unset" reset the rate on every clear (caught by the rate check below).
        if (!started_ || nowMs - minuteStart_ >= kMinuteMs) { started_ = true; minuteStart_ = nowMs; inMinute_ = 0; }
        lastAt_ = nowMs; ++total_; ++inMinute_;
        return true;
    }
    std::uint32_t Total() const { return total_; }
    std::uint32_t Suppressed() const { return suppressed_; }
    std::uint32_t InMinute() const { return inMinute_; }

private:
    bool started_ = false;
    std::uint64_t lastAt_ = 0, minuteStart_ = 0;
    std::uint32_t total_ = 0, suppressed_ = 0, inMinute_ = 0;
};

enum class Step { None, StuckStart, Unstuck };   // StuckStart: the episode's first stuck sample; Unstuck: once, when it ends

// The walk / run watchdog, extracted behaviour-identically from the inline code (main.cpp, cw-int-2 T4):
// moveMode set without the sprint bit, no lunge, no gameplay menu; under kStuckUnits in kWindowMs is an
// episode. StuckStart fires once per episode; Unstuck once when a window finally moves >= kStuckUnits.
// Verified line by line against E's inline block before extraction; the sprint bit stays the SPRINT
// watchdog's ground and is excluded here, exactly as E had it.
class MoveWatch {
public:
    Step Tick(std::uint64_t nowMs, double x, double y, const Probe& pr)
    {
        const std::uint32_t mm = pr.moveMode;
        if (mm && !(mm & 0x100u) && !pr.lungeActive && !pr.menuOpen) {
            if (!since_) { since_ = nowMs; fromX_ = x; fromY_ = y; return Step::None; }
            if (nowMs - since_ >= kWindowMs) {
                lastUnits_ = std::sqrt((x - fromX_) * (x - fromX_) + (y - fromY_) * (y - fromY_));
                lastWindowMs_ = nowMs - since_;
                since_ = nowMs; fromX_ = x; fromY_ = y;
                if (lastUnits_ < kStuckUnits && !logged_) { logged_ = true; return Step::StuckStart; }
                if (lastUnits_ >= kStuckUnits && logged_) { logged_ = false; return Step::Unstuck; }
            }
            return Step::None;
        }
        since_ = 0; logged_ = false; return Step::None;
    }
    double LastUnits() const { return lastUnits_; }
    std::uint64_t LastWindowMs() const { return lastWindowMs_; }
    bool EpisodeOpen() const { return logged_; }

    // RE-ARM THE EPISODE after an action that is expected to CHANGE the graph (2026-11-13). StuckStart fires once
    // per episode, which is right for a clear - it either worked or it did not - but a re-pick deliberately moves
    // the player into a DIFFERENT state, the flags-set wedge, which needs its own StuckStart to be treated. Without
    // this the episode stayed open, no second StuckStart ever came, and the player was left stuck one step short of
    // recovered. The window restarts here too, so the next judgement is made on fresh distance, not on the frozen
    // metres already banked.
    void ReArm(std::uint64_t nowMs, double x, double y) { logged_ = false; since_ = nowMs; fromX_ = x; fromY_ = y; }

private:
    std::uint64_t since_ = 0, lastWindowMs_ = 0;
    bool logged_ = false;
    double fromX_ = 0, fromY_ = 0, lastUnits_ = 0;
};

// WHAT THE PLAYER IS HOLDING, AS THE TICK SEES IT -- the settle's one choke point.
//
// THE DEFECT THIS REPLACES (release tip 1d45fbf2, reproduced 2 of 2 on the author's real path): the watch armed on
// the LEVEL "an arm is equipped", and arms::Tick samples that at ~30 Hz. A ripperdoc buy-with-replace empties
// and refills the weapon slot inside ONE synchronous API call -- `ar_mantis weapons removed (it was equipped)`
// 05:51:54.554 -> `ar_gorilla equipped on install` .564, ten milliseconds -- so no tick ever sampled "no arm",
// `armed_` never reset, and the give-up measured from an equip minutes old had expired long before. A SEAM
// install is the same code with a 1,757 ms gap (05:59:24.734 -> 05:59:26.491), ~53 ticks see the empty slot,
// and it settles normally. That 10 ms vs 1.7 s sampling window IS the whole seam/menu difference -- not a
// different caller, and not the paused game: the pump demonstrably runs while our menu is up (scene::Tick sits
// in the same per-frame lambda and ran its 2000 ms fade on schedule behind the open menu).
//
// SO THE WATCH NO LONGER WATCHES A LEVEL. It watches a SIGNATURE of what is in the player's hands, and re-arms
// on any change of it. The arm's FORM ID changes across that 10 ms swap even though the level never does. There
// is no call site to remember to touch, so a route nobody has thought of cannot miss the settle: the watch does
// not know what a route is.
struct EquipSig {
    std::uint32_t armForm = 0;   // the equipped arm weapon's form id, 0 when none: catches A->B and the empty slot
    std::uint32_t loadGen = 0;   // bumped by every load -- see the comment on kGapMs
    bool plsHeld = false;        // the launcher equips into the GRENADE slot, so armForm never sees it
    bool drawn   = false;        // a draw re-selects the weapon subgraph as surely as an equip does
    // THE CAMERA IS PART OF HOW HE IS DRIVEN (rig operator 6, 2026-11-13). He bought in 1P -- the save loads
    // in 1P -- and the movers read '?' there, 4 of 4 and again 4 of 4. Then F to third person: the 3P graph
    // held bGraphDriven=1 bGraphDrivenTranslation=1, and NOTHING re-armed, because the POV was not in this
    // signature. He walked 92 u against a 277 u baseline, still wedged at +92 s, while 1P walked full speed.
    // A POV change swaps which graph drives the body, so it belongs here beside the equip and the draw.
    bool firstPerson = false;

    // IS THERE ANYTHING TO SETTLE? The launcher counts, so a launcher equip re-arms the watch where before it was
    // invisible to it entirely. Decide still refuses with only the PLS held (`if (!pr.armEquipped) return
    // NoClear`), and that gate is deliberately NOT widened here: a graph clear with only the launcher in hand has
    // never been tested anywhere (release review, 2026-11-13). The launcher gap is NARROWED, not closed.
    bool Holds() const { return armForm != 0 || plsHeld; }

    bool operator==(const EquipSig& o) const
    {
        return armForm == o.armForm && loadGen == o.loadGen && plsHeld == o.plsHeld && drawn == o.drawn
            && firstPerson == o.firstPerson;
    }
    bool operator!=(const EquipSig& o) const { return !(*this == o); }
};

// The observed wedge lands WITH the equip's subgraph change (install 14:38:35.268 -> draw 14:38:35.611
// -> first stuck walk 14:38:46.7, eleven seconds later), so a change in what the player holds schedules
// settle checks: if the graph still holds the actor kSettleMs later and the policy agrees, clear it BEFORE
// the player ever tries to walk. A gated check retries every kRetryMs until kGiveUpMs; Satisfied() stops
// the watch. It never arms while a save is loading - handlers re-run on load, and a load is not an equip
// the player made.
class EquipSettleWatch {
public:
    static constexpr std::uint64_t kSettleMs = 2500, kRetryMs = 1000, kGiveUpMs = 6000;

    // A GAP THIS LONG BETWEEN TICKS MEANS THE WORLD CHANGED WITHOUT US, and it is a BACKSTOP, not the load path.
    // Loads are carried deterministically by EquipSig::loadGen, which kPostLoadGame always bumps. The gap catches
    // whatever else stops the pump -- and it is honest about the cost: a save, an alt-tab or a long hitch also
    // re-arms the watch, which is one extra settle check, normally answered RefusedNothing and retired at once by
    // Settled(). Raised to 2000 ms so an ordinary frame hitch does not qualify.
    static constexpr std::uint64_t kGapMs = 2000;

    // AN INCOMPLETE READ IS NOT AN ANSWER, so it may not spend the watch's budget (rig operator 6 /
    // the lead, 2026-11-13). In first person the third-person movers come back '?', so the four retries were
    // burned on reads that could never decide anything and the give-up expired while the question was
    // unanswerable. Now the question stays OWED: Incomplete() pushes the give-up out instead of letting it
    // run down. The retry slows to kOwedRetryMs while owed, because a '?' caused by the camera cannot resolve
    // without a POV change -- and a POV change re-arms the watch outright -- so polling fast buys nothing and
    // only fills the log. Bounded by kOwedMaxMs from the arming, after which the watch says so and stands
    // down; an equip, a draw, a load or a POV change arms a fresh one.
    static constexpr std::uint64_t kOwedRetryMs = 5000, kOwedMaxMs = 30000;

    // HOW LONG THE CLOCK MAY BE HELD WHILE HE CANNOT ACT. The hold is what stops the give-up expiring behind a
    // menu that stays up 6-17 s, but a hold that never ends would keep a watch armed for ever on a stuck menu
    // flag. After this the clock simply runs: the settle is late rather than never.
    static constexpr std::uint64_t kHoldMaxMs = 60000;

    // The give-up is per instance (2026-11-13). The equip watch keeps the proven 6000; the DRAW watch needs longer,
    // because its recovery is two steps - re-pick, then clear the flags the new graph sets - and 6 s from the edge
    // can leave no retry after the first check at +2.5 s. Its own default stays 6000 so nothing already tested moves.
    explicit EquipSettleWatch(std::uint64_t giveUpMs = kGiveUpMs) : giveUp_(giveUpMs) {}
    std::uint64_t GiveUpMs() const { return giveUp_; }

    // menuUp: a gameplay menu or OUR ripperdoc view is on screen, so the player cannot walk.
    bool Due(std::uint64_t nowMs, const EquipSig& sig, bool menuUp, bool loadingSave)
    {
        return Core(nowMs, sig, menuUp, loadingSave, true);
    }

    // The level-only form every pre-2026-11-13 row was written against: an arm is equipped, or it is not. It never
    // applies the tick-gap backstop, because only a real ~30 Hz pump knows what a gap in ITS OWN cadence means --
    // a test that calls this twice, seconds apart, has not observed a load.
    bool Due(std::uint64_t nowMs, bool armEquippedNow, bool loadingSave = false)
    {
        EquipSig s;
        s.armForm = armEquippedNow ? 1u : 0u;
        return Core(nowMs, s, false, loadingSave, false);
    }

    void Satisfied() { done_ = true; }

    // The read could not be taken. Keep the question owed and say whether it still is: false means the bound
    // was reached and the caller should log the stand-down. Never call this for a read that DECIDED anything.
    bool Incomplete(std::uint64_t nowMs)
    {
        if (!armed_ || nowMs - armedAt_ > kOwedMaxMs) { done_ = true; return false; }
        equipAt_ = nowMs;                    // the give-up counts from the last read we could not take
        nextAt_ = nowMs + kOwedRetryMs;      // and we ask again slowly, not four times a second window
        return true;
    }

    std::uint64_t OwedMaxMs() const { return kOwedMaxMs; }

private:
    bool Core(std::uint64_t nowMs, const EquipSig& sig, bool menuUp, bool loadingSave, bool allowGap)
    {
        const bool gap = allowGap && seeded_ && nowMs > lastTick_ && nowMs - lastTick_ > kGapMs;
        const bool changed = !seeded_ || sig != last_ || gap;
        lastTick_ = nowMs;
        last_ = sig;
        seeded_ = true;

        if (loadingSave || !sig.Holds()) { armed_ = false; done_ = false; heldSince_ = 0; return false; }

        // THE CLOCK STARTS WHEN HE CAN ACT, NOT AT THE INSTALL. Our menu pauses the game and stays up 6-17 s --
        // longer than the give-up, which is why the old watch could not have fired even had it armed -- and a
        // graph read taken while it is up is mid-transition anyway (Decide answers RefusedGuard on pr.menuOpen
        // regardless). Held re-armed at `now`, the settle lands kSettleMs after the menu closes, which is the
        // first moment he can walk.
        //
        // THE END OF A HOLD RE-ARMS UNCONDITIONALLY, AND THAT IS NOT THE SAME AS THE HOLD ITSELF (release review,
        // 2026-11-13 -- a defect in the first cut of this fix, and it was the SAME SHAPE as the bug being fixed:
        // a latch that survived the moment the player got control back). kHoldMaxMs used to be the only thing
        // standing between a long menu and a dead watch: past the cap the clock ran WHILE THE MENU WAS STILL UP,
        // the four checks were spent behind it as RefusedGuard, the give-up expired, and the close then found an
        // unchanged signature on an expired watch -- so a 70 s shop produced NO check after the close at all.
        // That is the author's own path: buy, then keep shopping for a minute. So the cap may stop the HOLDING; it may
        // never stop the re-arm that the close itself earns.
        const bool holdEnded = held_ && !menuUp;   // the PREVIOUS tick was held and this one is not: he can act now
        if (menuUp) {
            if (!heldSince_) heldSince_ = nowMs;
            held_ = true;   // set even past the cap, so the close is still seen as the end of a hold
            if (nowMs - heldSince_ <= kHoldMaxMs) { Rearm(nowMs); return false; }
        } else {
            heldSince_ = 0;
            held_ = false;
        }

        if (changed || holdEnded || !armed_) { Rearm(nowMs); return false; }
        if (done_ || nowMs < nextAt_ || nowMs - equipAt_ > giveUp_) return false;
        nextAt_ = nowMs + kRetryMs;
        return true;
    }

    void Rearm(std::uint64_t nowMs) { armed_ = true; done_ = false; equipAt_ = nowMs; nextAt_ = nowMs + kSettleMs; armedAt_ = nowMs; }

    bool armed_ = false, done_ = false, seeded_ = false, held_ = false;
    std::uint64_t equipAt_ = 0, nextAt_ = 0, lastTick_ = 0, heldSince_ = 0, armedAt_ = 0;
    std::uint64_t giveUp_ = kGiveUpMs;
    EquipSig last_{};
};

}   // namespace cw::stuck
