#pragma once
// RIPPERDOC VOICES (docs/RIPPERDOC-VOICE.md): a doctor speaks a written line with lip sync. The view sends
// {op:"say", doc, key} and {op:"hush"}; this plays the line's topic from "Cyberware - Voices.esp" on the doctor through
// Papyrus ObjectReference.Say, so the engine plays the .fuz from her head and drives the face from its .lip.
// [Voice] bEnabled=0 in Cyberware.ini (OFF for players); 1 only in Cyberware.Test.ini. Off, or no clip: text only.
#include <functional>
#include <string>

#include "cw_core.h"

namespace voice {
    // voices.json written by tools/ripperdoc_voice.py pack: {plugin, hush:{topic}, lines:{key:{doc, topic, ms}}}
    // legacyCallables: the OG runtime (1.10.163) wants the old MSVC std::function layout for Papyrus argument callables
    // probe: [Voice] bProbe, test ini only: trace the voice files the engine opens after each Say (docs/RIPPERDOC-VOICE.md)
    void Load(const std::string& path, bool enabled, bool legacyCallables, bool probe);
    // the doctor the player is talking to (set on activation); a finder resolves a doc id to its loaded actor otherwise
    void SetSpeaker(RE::TESObjectREFR* ref, const std::string& doc);
    void SetFinder(std::function<RE::Actor*(const std::string& doc)> finder);
    // {ok:true, ms[, queued:true]} or {ok:false, reason: disabled|no-clip|no-actor|paused|no-topic|vm}
    // queued: she was mid-line, so Tick stops that line and says this one
    cw::json Say(const std::string& doc, const std::string& key, bool paused);
    cw::json Hush();   // stops the line playing and drops a waiting one
    bool Enabled();
    void Tick();   // per frame on the game thread: stops a line, says a waiting one, samples the probe
    // test seam {"cmd":"voiceprobe",...}: Say any topic on a ref (0 = the ripperdoc) with no view or line involved
    bool ProbeSay(const std::string& plugin, std::uint32_t local, std::uint32_t refId);
    // Papyrus Actor.PlayIdleAction(action, None) through the legacy-safe dispatch (never commonlibf4's DispatchMethodCall
    // template on the OG runtime: it crashes, see cw_voice.cpp). Works whether or not [Voice] is enabled.
    bool PlayIdleAction(RE::Actor* actor, RE::BGSAction* action);
    // Game.ShakeCamera(None, strength, secs) + Game.ShakeController through the same legacy-safe callable
    void LandingShake(float strength, float secs, bool& camera, bool& controller);
    // Game.ForceThirdPerson() / Game.ForceFirstPerson(): zero-argument statics through the same legacy-safe callable.
    // commonlibf4 a08c731 has no native force-POV, and the install scene must run in third person (docs/INSTALL-SCENE.md
    // SCN-9 restores the view it found). Papyrus dispatch lives ONLY in this file (the leash refuses it elsewhere).
    bool ForcePerson(bool third);
}
