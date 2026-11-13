// cw_armhint.h - WHEN the "you just bought an arm, here is how to use it" hint is shown, and what it says.
// Pure, so cwtest holds every case the lead listed and the plugin only wires it up.
//
// THE AUTHOR'S ASK F: "a hint for how to use an arm after buying it". It was built, and in game it was INVISIBLE.
// Measured by the rig operator on `a7648c6c`: `ar_monowire equipped on install` at 03:33:20.186, the ripperdoc
// menu closed at 03:33:26.344, and stills at +2/+4/+6 s show nothing.
//
// THE CAUSE, from the view code and that timeline, not from a guess: the hint WAS drawn. It is raised at INSTALL as
// a CyberwareHUD `note`, and `.note` lives for `nshow 2600ms` before `animationend` removes it. The menu covered the
// screen for 6.16 s, so the note was born behind it and had already removed itself about 3.5 s BEFORE the world came
// back. The view was never at fault and neither was `[HUD] bMessages` (0 in all three of the author's folders): the hint
// was shown to a screen he could not see.
//
// SO THE RULE IS ABOUT TIMING, NOT DRAWING: raise it when the player can actually READ it - the moment our view is
// down and no other menu is up - and hold it long enough to read. Everything below is that rule, and nothing else.
#pragma once
#include <string>

namespace cw::armhint {

// Long enough to read a sentence from a couch, and the lead's floor is 5 s (2026-11-13). The view holds the
// node for this long; the plugin does not time it.
inline constexpr int kHoldMs = 6000;

// What the plugin knows when it asks. Every field is a positive probe main.cpp already makes for other reasons.
struct World {
    bool ourViewUp = false;         // the ripperdoc / setup view is on screen: he cannot see the HUD behind it
    bool gameplayMenuOpen = false;  // Pip-Boy, pause, VATS, a terminal, a container...: never draw over one
    bool loadingSave = false;       // a load is in flight: say nothing, and see Forget() below
    bool armDrawn = false;          // the arm is ALREADY drawn, so "press R to draw" would be wrong
};

// The hint the player is owed, if any. One slot on purpose: see Raise().
class Hint {
public:
    // At the install. TWO INSTALLS IN ONE MENU VISIT: the LAST one wins, because the hint is about the thing he just
    // did, and two notes queued behind one menu would show the first and then talk about an arm he bought before it.
    void Raise(const std::string& armName) { name_ = armName; }

    // A LOAD MUST NOT REPLAY IT. The hint is a one-shot about an action, not a piece of saved state: it is never
    // written to the cosave, and a load drops whatever was pending so a reloaded save is silent.
    void Forget() { name_.clear(); }

    bool Pending() const { return !name_.empty(); }
    const std::string& Name() const { return name_; }

    // The text. With the arm ALREADY DRAWN the draw instruction is not merely redundant, it is wrong - he would press
    // R and put them away - so that clause is dropped rather than reworded.
    // THE DRAWN FORM NAMES A CONTROL TOO (reviewer + the lead, 2026-11-13). It used to read just
    // "<Arm> equipped." -- true, and useless: a player whose arm came up drawn was never told how to put it away.
    // It was not an oversight but a DESIGN that misfired, which is why it survived so long: the drawn branch drops
    // the draw clause on the reasoning that he can see the arm already, and a menu swap KEEPS the arm drawn, so
    // every buy after the first took it. Thirteen hints in the 1147Z boot log; the long form appeared exactly once.
    // Each form names only a control that is TRUE in its own state: drawn offers "hold R to put them away" first,
    // because that is the one he needs now, and the redraw after it.
    static std::string Text(const std::string& armName, bool armDrawn)
    {
        return armDrawn ? armName + " equipped and drawn. Hold R to put them away, press R to draw them again."
                        : armName + " equipped. Press R to draw them, hold R to put them away.";
    }

    // May it be shown RIGHT NOW? Shown once, then forgotten. A false answer is never a refusal, only "not yet":
    // the caller asks again on the next tick, so a Pip-Boy opened the instant the menu closed makes the hint WAIT
    // rather than lose it.
    bool TakeIfDue(const World& w, std::string* out)
    {
        if (!Pending()) return false;
        if (w.loadingSave) return false;      // mid-load: not now (and Forget() is called on the load itself)
        if (w.ourViewUp) return false;        // our own view is covering the screen - the original defect
        if (w.gameplayMenuOpen) return false; // never over a modal: wait for it to go
        if (out) *out = Text(name_, w.armDrawn);
        name_.clear();                        // one shot
        return true;
    }

private:
    std::string name_;
};

// ---------------------------------------------------------------- WHICH ROUTE THE HINT TAKES (HINT-1, 2026-11-13)
// BOOT 2 ON bb849313: LOG PASS, SCREEN FAIL. `arms: arm-use hint shown` logged 57 ms after `menu: closed`, and the
// note is in NO lossless still at +1/+3/+5/+8 s and in no clip frame. The hint was handed to the CyberwareHUD view,
// which the operator has never once seen draw.
//
// THE CAUSE, in one line of the plugin: `optics::hudView = view != 0;` (main.cpp, hud::Create). That flag is the
// CreateView HANDLE - it records that the view OBJECT EXISTS. Nothing ever clears it when the view is hidden, is
// not ready, or is never composited; the only path that sets it false is the view FILE being missing. `HudText()`
// then asks "does the view exist" and treats the answer as "the player can see it". EXISTENCE WAS BEING USED AS
// VISIBILITY, and for a one-shot hint that is the whole defect.
//
// WHY THE EXACT DOWNSTREAM REASON DOES NOT CHANGE THE FIX, and this is the point worth keeping: there are at least
// three ways the view can exist and still not paint - the in-menu Settings toggle calling Hide (`wantHud = s.hud &&
// g_ini.hud`), the view never becoming `ready` so every Invoke silently no-ops, and the view being ordered below the
// menu (`SetOrder(view, GetOrder(g_view) - 1)`, the operator's `hud order -1`). ALL THREE produce the same thing:
// the object is there and the player sees nothing. So the fix must not try to name which one happened - it must stop
// inferring visibility from existence and ASK.
//
// THE RULING, 2026-11-13 ~11:20Z (the lead), WHICH SUPERSEDES THE VISIBILITY ROUTE ABOVE:
// **the arm-use hint ALWAYS goes out as the GAME'S HUD LINE, and never to our own view.**
//
// The first cut of this fix routed on `hud::Visible()`. That was still a bet: it asked whether our view CAN be
// seen, and "can be seen" is a weaker claim than "was seen" - the probe cannot know whether a composited view is
// behind something, and the whole defect came from trusting a flag about our own view. For a ONE-SHOT message
// answering the author's ask F, the only honest route is the one the game itself has always drawn.
//
// THE PARAMETERS ARE KEPT ON PURPOSE, unused. They state in the signature that neither the player's bMessages
// choice NOR our view's visibility may route this message anywhere else, and they let cwtest assert exactly that
// for every combination rather than for the one that happens to occur. `constexpr`, so the call site can
// `static_assert` the rule and the compiler holds it rather than a reviewer.
//
// This is the RULE. The MECHANISM in main.cpp is stronger still: the branch to `hud::Event` is DELETED, so there
// is no code path to our view to go wrong - "the hint cannot reach the view" is readable there, not inferable.
inline constexpr bool HintUsesGameHudLine(bool /*hudTextPreferred*/, bool /*ourViewVisible*/)
{
    return true;
}

}   // namespace cw::armhint
