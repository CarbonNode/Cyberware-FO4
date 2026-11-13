// cw_talkprompt.h - the ripperdoc's two-choice talk prompt, pure so cwtest drives the same machine the plugin runs
// (Builder 3, 2026-11-13, docs/VARGA-TALK.md "What ships tonight").
//
// WHY: on cand/beta-round2 3f8e37b6 activating Varga opens no menu (right) but no conversation either (wrong, 3 of 3):
// the esp has no greeting topic for the engine to start. Until a real greeting topic ships, the DLL shows its OWN
// prompt in the PrismaUI view: her greeting as text, then "Let me see your cyberware." / "Not right now.".
// This header is the decision only. The plugin (main.cpp talkgate) fills World/Target from the game every tick and
// does what the returned Act says. No game types here: it builds under g++ (cwtest) and MSVC (the DLL) alike.
#pragma once

#include <cstdint>
#include <string>

namespace cw::talkprompt {

    // What is true in the game at the moment of asking. The plugin reads each from the engine; cwtest sets them.
    struct World {
        bool armed = false;            // the talk gate is armed (CW_RipperdocInfoShop is in this build)
        bool menuOpen = false;         // any game menu, or our own view, is up
        bool sceneRunning = false;     // an install scene or any scene owns the player
        bool playerInCombat = false;
        bool dialogueUp = false;       // the engine's own DialogueMenu (a real conversation, combat barks included)
        bool otherPromptUp = false;    // another prompt/message box of anyone's is on screen
    };

    // The ripperdoc this prompt is for, as seen this tick.
    struct Target {
        bool present = false;          // the ref resolves and has 3D
        bool alive = true;
        bool inCombat = false;
        float distance = 0;            // to the player, game units
    };

    enum class Refuse { None, AlreadyOpen, NotArmed, Menu, Scene, Combat, Dialogue, OtherPrompt };
    enum class Choice { Shop, Decline };
    enum class Act { None, OpenMenu, Close };

    constexpr float kRange = 350.0f;   // the same ~5 m the talk guard uses (talkface::kTalkRange)

    inline const char* RefuseWord(Refuse r)
    {
        switch (r) {
            case Refuse::None: return "none";
            case Refuse::AlreadyOpen: return "the prompt is already open";
            case Refuse::NotArmed: return "the talk gate is not armed";
            case Refuse::Menu: return "a menu is open";
            case Refuse::Scene: return "a scene is running";
            case Refuse::Combat: return "the player is in combat";
            case Refuse::Dialogue: return "a dialogue is up";
            case Refuse::OtherPrompt: return "another prompt is up";
        }
        return "?";
    }

    class Prompt {
    public:
        bool Open() const { return open_; }
        const std::string& Doc() const { return doc_; }
        std::uint32_t Ref() const { return ref_; }

        // Activating a ripperdoc. None = the prompt is now open for (doc, ref); anything else = nothing was opened.
        // Already open comes first: pressing E on her again while it is up does nothing (RUN-SHEET 8f VP6).
        Refuse Activate(const std::string& doc, std::uint32_t ref, const World& w)
        {
            if (open_) return Refuse::AlreadyOpen;
            if (!w.armed) return Refuse::NotArmed;
            if (w.menuOpen) return Refuse::Menu;
            if (w.sceneRunning) return Refuse::Scene;
            if (w.playerInCombat) return Refuse::Combat;
            if (w.dialogueUp) return Refuse::Dialogue;
            if (w.otherPromptUp) return Refuse::OtherPrompt;
            open_ = true;
            doc_ = doc;
            ref_ = ref;
            return Refuse::None;
        }

        // The player's answer from the view. Shop -> open the cyberware menu; Decline (and Esc) -> close, open nothing.
        //
        // AN ANSWER WITH NO PROMPT OPEN STILL CLOSES A VISIBLE PANEL (the softlock of 2026-11-13). It used to return
        // None, the caller logged "ignored (no prompt open)" and returned BEFORE its hide - so a panel left on screen
        // by a state/view desync swallowed Esc and Enter for ever, with the escguard still holding the player. Now:
        // no state and no panel = nothing to do; no state but the panel is up = CLOSE IT. Esc must always work.
        Act Choose(Choice c, bool viewVisible = false)
        {
            if (!open_) return viewVisible ? Act::Close : Act::None;
            open_ = false;
            return c == Choice::Shop ? Act::OpenMenu : Act::Close;
        }

        // Every tick while open: a reason when the prompt must close (and it is closed), else nullptr. menuOpen is not
        // consulted here: our own view is the menu that is up while the prompt shows.
        const char* Watch(const World& w, const Target& t)
        {
            if (!open_) return nullptr;
            const char* why = nullptr;
            if (!t.present) why = "the ripperdoc is gone";
            else if (!t.alive) why = "the ripperdoc is down";
            else if (t.inCombat) why = "the ripperdoc is in combat";
            else if (t.distance > kRange) why = "the ripperdoc is out of range";
            else if (w.playerInCombat) why = "the player is in combat";
            else if (w.dialogueUp) why = "a dialogue opened";
            else if (w.sceneRunning) why = "a scene started";
            if (why) open_ = false;
            return why;
        }

        // ---------------------------------------------------------------- the view and the state may never disagree
        // THE SOFTLOCK OF 2026-11-13, and why this lives here instead of in the plugin. The plugin used to decide
        // visibility inline: `if (IsHidden(view)) { Reset(); return; }`. Two faults, one softlock.
        //   1. PrismaUI's visibility does NOT flip inside Show(), so the first tick after showing reads "hidden" and
        //      the watcher concluded something else had hidden it. False positive, every single time.
        //   2. Its remedy was Reset(), which clears the STATE ONLY - correct if the view really were hidden, fatal
        //      when it is merely not shown yet: the panel stayed drawn and focused, the escguard stayed held, and the
        //      player could not move, could not Esc and could not answer. Only the test seam's close freed him.
        // So: a not-yet-visible read before the view has EVER been seen visible is not evidence of anything, and every
        // close returns a reason the caller must honour with its one close helper (hide, unfocus, release the guard,
        // push close) - never a bare Reset. Reset stays for a load, where there is no view left to hide.

        // The view may take a few frames to become visible. Longer than any observed show, short enough that a prompt
        // can never sit open holding the guard with no panel behind it (the lead's requirement, 06:50Z).
        static constexpr std::uint64_t kShowGraceMs = 1500;

        // Called the moment the view has been ASKED to show. Starts the grace; nothing is believed before it.
        void Shown(std::uint64_t nowMs) { shownAt_ = nowMs; everVisible_ = false; }

        // Every tick while open, BEFORE Watch. nullptr = leave it open. A reason = it is now closed and THE CALLER
        // MUST RUN ITS CLOSE HELPER. Three cases, and the middle one is the bug that was:
        //   seen visible              -> latch it, keep it open
        //   hidden after being seen   -> something really did hide it: close
        //   hidden, never seen, grace  -> Show has not taken effect yet: WAIT (this used to close, and softlock)
        //   hidden, never seen, after  -> it failed to show at all: close, so the guard is never held with no panel
        const char* Observe(bool viewVisible, std::uint64_t nowMs)
        {
            if (!open_) return nullptr;
            if (viewVisible) { everVisible_ = true; return nullptr; }
            if (everVisible_) { open_ = false; return "the view was hidden by something else"; }
            if (nowMs - shownAt_ >= kShowGraceMs) { open_ = false; return "the view never became visible"; }
            return nullptr;
        }

        // THE INVARIANT THE SOFTLOCK VIOLATED, as a predicate so a selftest can assert it: an open prompt implies a
        // visible panel, except inside the grace before the first visible read. False means state and view disagree in
        // a way no caller may leave standing.
        bool AgreesWith(bool viewVisible, std::uint64_t nowMs) const
        {
            if (!open_) return true;   // closed: the caller's close helper has hidden the panel
            if (viewVisible) return true;
            return !everVisible_ && nowMs - shownAt_ < kShowGraceMs;
        }

        bool EverVisible() const { return everVisible_; }

        // A load: the view is gone with the session, so there is nothing to hide. NOT for a visibility mismatch.
        void Reset() { open_ = false; everVisible_ = false; shownAt_ = 0; }

    private:
        bool open_ = false;
        bool everVisible_ = false;
        std::uint64_t shownAt_ = 0;
        std::string doc_;
        std::uint32_t ref_ = 0;
    };
}
