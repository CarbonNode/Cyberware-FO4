// cw_escguard.h - keep Esc / Tab inside the ripperdoc menu (MENU POLISH, 2026-11-08).
//
// In game the focused PrismaUI view gets Esc (as key "Unidentified", keyCode 27; the page reads it through
// CW.keyOf), but the SAME key press also reached the game: MenuControls' MenuOpenHandler opened the Pause menu
// under ours (the lead's in-game finding). While our menu is open we switch MenuOpenHandler (Esc -> Pause) and
// PipboyHandler (Tab -> Pip-Boy) off through BSInputEventUser::inputEventHandlingEnabled, and put back exactly what
// they had 350 ms after the menu closes, so the key press that closed it has passed. A MenuOpenCloseEvent sink is
// the fallback and the test evidence: a Pause / Pip-Boy menu that opens anyway while we hold the guard is closed
// and logged ("escguard: PauseMenu opened ..."); any other Pause / Pip-Boy open is logged at INFO with the time since our
// last close, so an unexplained one can be lined up against cw-in.log.
// Behind [Menu] bEscGuard (Cyberware.ini / Cyberware.Test.ini): 1 by default since the give-back was proven in game
// (cw-int-2 T9-T11, 2026-11-08). Off = nothing is held, no sink.
#pragma once

#include <cstdint>

namespace escguard {
    // kGameDataReady: register the MenuOpenCloseEvent fallback sink.
    void Register();
    // Game thread. true right after the view is shown + focused; false on every path that hides it.
    void SetMenuOpen(bool open);

    // ---------------------------------------------------------------- the guard may never outlive its menu
    // THE SOFTLOCK OF 2026-11-13: the Varga prompt armed this guard, its own state then closed on a false
    // "the view is hidden" read, and nothing disarmed it - so the player could not move and Esc did nothing.
    // These exist so a reconciler can release the guard WITHOUT CONSULTING the state of whatever armed it:
    // no owner's view on screen, past the grace, guard off. A future desync therefore cannot lock the player.
    bool MenuOpen();                 // the guard is holding the handlers right now
    bool ReleasePending();           // a delayed restore is already on its way (NOT the same question as MenuOpen)
    // Give the handlers back NOW, on the game thread, with no 350 ms delay and no worker thread. The delay only
    // exists to let the Esc that closed a menu pass the game's input first; a reconciling release has no key
    // press behind it and nothing of ours on screen, so it has nothing to wait for (release review, 2026-11-13).
    void ReleaseNow(const char* why);
    std::uint64_t HeldForMs();       // ms since it was armed (0 when it is not holding)
}
