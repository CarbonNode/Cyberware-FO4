// cw_smart_game.h - ARMORY smart guns in the game: target lock from the frame pump, steering of the player's smart
// rounds from a MissileProjectile::UpdateImpl vtable hook. Logic lives in core/cw_smart (selftested on Linux).
// OFF unless [Smart] bSmartLock=1 (Cyberware.Test.ini only, until an integration cycle proves it).
#pragma once
#include "cw_core.h"

#include <functional>
#include <string>

namespace smartgame {
    struct Config {
        bool enabled = false;                                   // [Smart] bSmartLock
        int steerMode = 2;                                      // [Smart] iSteerMode: 0 lock + log only, 1 + set the round's desiredTarget, 2 + steer its velocity
        bool diag = false;                                      // [Smart] bDiag: once a second while a smart gun is up, log the aim ray and every actor near the crosshair with why it does or does not lock
        bool hudEvents = true;                                  // [Smart] bHudEvents: push "smartLocks" HUD events (HUD view ignores unknown types)
        std::string formmapPath;                                // [Paths] sArmoryFormMap
        std::function<bool()> smartLinkInstalled;               // the engine: is cw_ha_smart_link installed?
        std::function<void(const cw::json&)> hudEvent;          // hud::Event
        std::function<void(const std::string&)> notify;         // a one-line HUD note
        std::function<void(std::uint32_t)> stageBAnim;          // [Smart] bStageBAnims: give this WEAP the keyword that selects the CP 1P clips
        std::function<void(const char*)> playSound;             // a Cyberware SNDR at the player, by EditorID
    };
    void Init(Config c);    // data ready: load armory-formmap.json, resolve forms, install the projectile hook. Inert (one log line) without them
    void Frame();           // every frame from the pump
    void OnLoad();          // a save was loaded: drop locks and tracked rounds
    void CycleSkippyMode(); // Skippy drawn: heads <-> limbs (CP's Stone Cold Killer / Puppy-Friendly). Lasts until the game closes
    std::string Stats();    // one line for the log / the test seam
}
