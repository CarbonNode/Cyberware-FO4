// cw_scene.h - the ripperdoc INSTALL SCENE sequencer (INSTALL SCENE stream, 2026-11-10).
//
// The contract is docs/INSTALL-SCENE.md: beats, pass/fail ids SCN-1..12 and DOC-3a, every case. Read it before
// changing a time or a beat here. In one sentence: on the first accepted install or removal of a visit the player is
// seated in the doctor's chair, the camera frames it, she walks to her mark and speaks her line, a 1 s flare hands
// over the new chrome, a second angle reveals it, and the menu comes back with the player still in the chair.
//
// THE DLL SEQUENCES, THE VIEW PAINTS. Nothing here draws: each beat pushes
//   cwOnEvent {type:"scene", phase:"bars|dip|up|line|flare|reveal|back|end|abort", ...}
// and ui/views/Cyberware/scene.js draws letterbox bars, the subtitle in the lower bar, a flat-fill flare and the
// lower-third. There is never a centre black card (SCN-1); the only full-screen black is the 0.45 s dip that covers
// moving a standing player to the chair.
//
// Three modes, all answered to the view's {op:"scene"}:
//   "chair"  the full scene (at most 9 s; 7.8 s when the player was already seated, beats 2-3 being only the camera move)
//   "short"  the second and later commits of a visit: a 0.8 s flare in place + the lower-third, 2.0 s, no camera cut
//   "fade"   the 2D fallback, same 2.0 s look with the WORLD STILL VISIBLE, whenever the chair path cannot run
//            (no seat record, combat, power armor, the doctor away, the sit refused, [Scene] bChairScene=0)
//
// Rules this file obeys, each one a law somebody paid for:
//   - NOTHING of the scene is in the save. No global stage, no flag, no cosave field: every byte is in this
//     translation unit, so a save taken mid-scene loads as a player sitting in a chair with full control. OnLoad()
//     drops everything (restores the handlers, the camera, the view, stops the imagespace).
//   - NEVER BSInputEnableLayer. Its state IS saved and a stranded layer leaves a save with dead controls. The input
//     lock is PlayerControls' per-handler inputEventHandlingEnabled flags, saved and put back exactly (the
//     cw_escguard.cpp pattern).
//   - Papyrus dispatch only through cw_voice.cpp (voice::Say, voice::ForcePerson). The leash refuses it here.
//   - Risky defaults OFF in code and Cyberware.ini, ON only in Cyberware.Test.ini: bChairScene, bCamPitch, iFlare.
//   - The install is committed BEFORE beat 1, so a skip, an abort, a reload or a crash can never lose the piece.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cw_core.h"

namespace scene
{
    // One doctor's scene data, all from [Scene] in the ini so no record id is ever hard-coded here.
    //   seat    "Plugin.esp|hex"  the furniture ref the player is seated in (Varga: "Cyberware - Clinic.esp|A4E")
    //   mark    "Plugin.esp|hex"  the XMarkerHeading she stands on (Varga: "Cyberware - Clinic.esp|A4F")
    //   shotA   "yawDeg,pitchDeg,zoom"  beat 3's framing, relative to the seated player's facing
    //   shotB   "yawDeg,pitchDeg,zoom"  beat 7's reveal angle
    //   entry   "x,y,z"  where a standing player is put during the dip; empty = 90 u off the seat's foot end
    //   global  "Plugin.esp|hex"  a float GLOB the DLL writes 1/0 so her step-in package can condition on it
    //                             (CLINIC's CW_ClinicSceneActive). The DLL only ever writes it.
    //   pkgs    "Plugin.esp|hex,hex,..."  her packages that mean "at her station and free" (CLINIC: 975, A4C).
    //                             Empty = no check. Before 10:00 she is in bed or the office chair, and starting the
    //                             scene then would pull her out of bed, so with pkgs set and none of them running
    //                             the scene takes the fallback (CLINIC, 2026-11-10).
    struct DocIni
    {
        std::string doc, seat, mark, shotA, shotB, entry, global, pkgs;
    };

    // Everything this file needs from main.cpp, injected so cw_scene.cpp stays its own translation unit.
    struct Hooks
    {
        std::function<void(const cw::json&)> push;                                  // Push("cwOnEvent", ev)
        std::function<void(const char* edid, const char* vanilla)> cue;             // snd::Cue
        // voice::Say, unpaused. Returns the clip length in ms (0 = text only), because the SUBTITLE has to outlive
        // the scene: her install line runs up to 8.71 s from beat 4, past the 8.5 s end, and SCN-6 says a beat
        // never cuts her off.
        std::function<float(const std::string& doc, const std::string& key)> say;
        std::function<void()> hush;                                                 // voice::Hush
        std::function<RE::Actor*(const std::string& doc)> findDoc;                   // the doctor's loaded actor
        std::function<void()> gesture;                                              // talkface::Gesture
        // talkface::Begin: the proven 0.45 s turn-to-the-player. The scene MUST do this itself, because main.cpp only
        // turns her when the menu was opened by TALKING to her with [Menu] bTalkUnpaused=1 — a chair-opened visit, or
        // any visit with that key off, would otherwise have her facing away through her own line (SCN-3).
        std::function<void(std::uint32_t docRef)> turnTo;
        std::function<void(bool paused)> refocus;                                   // g_ui->Focus(g_view, paused)
        std::function<bool()> menuPaused;                                           // g_menuPaused
        std::function<const char*()> blockingMenu;                                  // OpenGameplayMenu(): abort/hold
        std::function<float()> playerHealth;                                        // main.cpp's FindAV("Health") path
        std::function<bool(float radius)> hostileNear;                              // a hostile in combat within radius u
        // CRASH's talk guard (cw-ab-talk-guard 80bbba9f) ends an unpaused ripperdoc talk when the player is hit or in
        // combat, or when the doctor walks further than 350 units. It must never end a talk an install scene started
        // from, and it learns that from two signals: Varga's CW_ClinicSceneActive GLOB, and talkface::sceneBusy. ONLY
        // VARGA HAS A GLOBAL - Kessler and the Institute doctor are configured with none - so for them this flag is
        // the only exemption there is. Set true at the start of EVERY mode, cleared in EndRun, at the chair scene's
        // end beat and in OnLoad, so it can never be left true and can never miss a mode.
        std::function<void(bool busy)> sceneBusy;
        // clinic::DueInClinic (T19, 2026-11-10): the runtime FormID of the package the doctor's clock wants now (0 = no
        // opinion) and whether `running` is one of her own schedule packages. Read only. Appended LAST, like sceneBusy,
        // so main.cpp's braced init list keeps its order; a doctor with no clinic schedule simply gets 0.
        std::function<std::uint32_t(std::uint32_t docRef, std::uint32_t running, bool& hers)> due;
    };

    // From ReadIni: [Scene] bChairScene, bCamPitch, iFlare, and one DocIni per doctor the ini names.
    void Configure(bool chairScene, bool camPitch, int flare, const std::vector<DocIni>& docs);
    void SetHooks(Hooks h);

    // The view's {op:"scene", args:{kind:"install"|"remove", name, uid, key, text}}.
    // Answers {ok:true, data:{mode:"chair"|"short"|"fade", ms, why}} and starts the beats. Game thread.
    cw::json Start(const cw::json& args);
    // {op:"sceneSkip"} or any key the view saw after 1.0 s: jump to the reveal's last second (SCN-10).
    void Skip();
    // Combat, a hit, a blocking menu, the menu closing, a load: end NOW and put everything back. Safe to call twice.
    void Abort(const char* why);

    void OnMenuClosed();                 // CloseMenu(): stand the player up, restore the camera, close the visit
    void OnLoad();                       // kPostLoadGame / kNewGame: drop everything, keep nothing
    void Tick(std::uint64_t now);        // every frame on the game thread (the pump)
    bool Active();                       // a scene is running: the menu must not re-pause the game
    std::string StatusLine();            // {"cmd":"scene","what":"status"}

    // Test seams, [Debug] bDebug only. Both exist so CLINIC and INTEGRATION get NUMBERS instead of adjectives:
    //   NudgeSeat  move the configured seat ref at runtime (dx, dy, dz, drz) and log its absolute position, so
    //              CLINIC can bake the proven pose into patientSeat.pos/rotZ.
    //   NudgeCam   change the live shot (yaw, pitch, zoom) and log it, so a framing that passes SCN-3 becomes an
    //              sShotA_/sShotB_ line instead of a note.
    std::string NudgeSeat(const std::string& doc, float dx, float dy, float dz, float drz);
    std::string NudgeCam(int shot, float yawDeg, float pitchDeg, float zoom);
    // {"cmd":"scene","what":"sitwatch","ms":8000}: trace the player's SIT_SLEEP_STATE every frame for ms (console time
    // excluded), with the scene's own tracer, and log each change. The positive control for the scene's sit trace.
    std::string WatchSit(std::uint64_t ms);
}
