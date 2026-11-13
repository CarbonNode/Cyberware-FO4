// cw_hudlayout.h - where the player's HUD layout lives on disk (MENU POLISH, 2026-11-09, the author: "Make sure any HUD
// stuff is resizable and draggable around the screen").
//
// The CyberwareHUD page owns the layout (views/CyberwareHUD/layout.js: drag, resize, snap). This file only keeps it:
// a small JSON object, validated here, written atomically to HudLayout.json in
//   Documents\My Games\Fallout4\F4SE\Cyberware\       the player (never under the MO2 VFS, so never in overwrite)
//   [HUD] sLayoutDir                                    the TEST build (Cyberware.Test.ini points it at a test folder), so a
//                                                       test run's layout can never reach a play profile
// It is a player preference, not save data: it survives save, reload, a new game and a restart.
#pragma once

#include <string>

#include "cw_core.h"   // cw::json

namespace hudlayout {
    // ReadIni: the [HUD] sLayoutDir override ("" = Documents). Cheap; nothing is read until Get().
    void Init(const std::string& dirOverride);
    // Absolute path of HudLayout.json, "" when there is nowhere to write.
    const std::string& Path();
    // The player's other preferences next to it (CyberwarePrefs.ini: [Keys] iSetupKey from the Settings tab), "" = none.
    std::string PrefsIni();
    // The saved layout ({} = none: the stock layout). Read from disk once, then cached.
    const cw::json& Get();
    // Keep only the known shape (see Sanitize in the .cpp). Returns false + err when nothing usable is left.
    bool Save(const cw::json& layout, std::string& err);
}
