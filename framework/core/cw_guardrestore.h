// cw_guardrestore.h - the arming rule behind the escguard's DELAYED restore, pure so cwtest can hold it.
//
// THE LIVELOCK OF 2026-11-13, measured in game by the rig operator on 03375a02: after the player
// DECLINED the Varga prompt the prompt and the view closed correctly, but the escguard stayed held for
// 28.1 s and then 57.4 s - swallowing Esc and the Pip-Boy with nothing on screen - and one boot's log
// carried 4,005 copies of the backstop's warning.
//
// THE CAUSE IS ARITHMETIC. escguard's close cannot restore the game's menu handlers at once: the Esc that
// closed the menu is still travelling to the game's input, so the restore is deferred by kRestoreDelay
// (350 ms) and tagged with a GENERATION, and it runs only if the generation still matches - that is how a
// menu REOPENED inside the delay keeps its guard. But the generation was bumped by EVERY call, a close as
// well as an open. The arms tick runs every 33 ms, so a caller that asks to close on every tick - which
// the new GuardReconcile backstop did - bumps the generation about ten times inside the 350 ms window, and
// each bump invalidates every restore already sleeping. The release review measured the rate from the log:
// 4,005 warnings over 85.5 s is 46.8 per second, one per frame, so each sleeping restore is outlived by
// ~16 later calls before it wakes and none can ever pass the check. `g_on` can then never become false, so
// the backstop never stops calling. **The spam count was not a side effect, it was the loop counter.**
// The backstop was not LATE: it ran on time, every tick, and starved the restore it was asking for. It
// escaped only when a gap in the ticks longer than 350 ms happened to let one through - the 28 s and 57 s.
//
// THE DEEPER FAULT, and fixing this closes the class rather than the instance (release review): the guard's
// "am I HELD?" and "is a RELEASE IN FLIGHT?" were the same variable. `g_on` stays true while a restore is
// pending, so every observer reads "still held" and asks again. The plugin now keeps them apart.
//
// THE RULE: a close is IDEMPOTENT. Nothing held, or a restore already on its way, means do nothing at all
// and in particular DO NOT TOUCH THE GENERATION - a close invalidating a close is the defect. Only a close
// that really schedules may take a new generation, and only an OPEN may cancel one. Then any number of
// callers may ask to close as often as they like and the first restore still lands on time.
#pragma once
#include <cstdint>

namespace cw::guardrestore {

// What a close should do. `held` = the guard is holding the handlers; `pending` = a delayed restore is
// already on its way. Pure, so the plugin's atomics and this rule cannot drift apart.
struct Plan {
    bool schedule = false;   // schedule a delayed restore, and take a new generation for it
    bool bumpGen = false;    // always equal to schedule: the generation moves only with a real schedule
};

inline Plan PlanClose(bool held, bool pending)
{
    if (!held) return {};      // nothing to give back: a duplicate close must be a no-op
    if (pending) return {};    // already on its way: re-arming is exactly what starved it
    return { true, true };
}

// May the delayed restore run now? It must still be the generation it was scheduled with, and the guard
// must still be held. The caller clears `pending` either way, so a later close can always re-arm.
inline bool MayRestore(std::uint32_t scheduledGen, std::uint32_t currentGen, bool stillHeld)
{
    return stillHeld && scheduledGen == currentGen;
}

// AN OPEN THAT FINDS THE GUARD ALREADY HELD MUST STILL CANCEL A RESTORE IN FLIGHT. The operator's SECOND Varga
// prompt of 2026-11-13 showed why: the guard was never given back after the first decline (the livelock above), so
// the second prompt's open hit the "already held" early-out, never re-armed and never took a new held-since stamp -
// and then the stale pending give-back fired WHILE that prompt was on screen, so Esc opened the game's Pause menu
// under our panel ('escguard: PauseMenu opened, guard idle'). The guard being absent is also why that decline did
// not leak: it is NOT evidence the decline path is safe, it is a SECOND symptom - an unguarded prompt.
inline bool OpenMustCancelPendingRestore(bool alreadyHeld, bool pending) { return alreadyHeld && pending; }

// A RELEASE WITH NO KEY PRESS BEHIND IT NEEDS NO DELAY (release review's second fix). kRestoreDelay exists to
// let the Esc that closed the menu pass the game's input first. The reconciling backstop fires when NO view
// of ours is on screen and nothing was pressed, so it has nothing to wait for: it restores immediately on
// the game thread instead of scheduling, which also removes the thread-per-tick storm. True when a caller
// should take that route rather than the deferred one.
inline bool ImmediateRelease(bool anyViewOfOursVisible, bool fromAKeyPress)
{
    return !anyViewOfOursVisible && !fromAKeyPress;
}

}   // namespace cw::guardrestore
