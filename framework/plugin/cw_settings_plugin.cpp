// cw_settings_plugin.cpp - game-side glue for the player Settings (BETA QA, 2026-11-08).
//
// The engine owns the settings (core/cw_settings.cpp: cosave + setSettings op). This unit:
//   - persists them to Documents\My Games\Fallout4\F4SE\Cyberware\CyberwareSettings.ini on every
//     setSettings, so a NEW game starts from the player's last choices. Documents, NOT Data:
//     a Data\F4SE write lands in MO2 overwrite, which EVERY profile reads — a test run's settings
//     leaked into the play profile (INTEGRATION cycle 2 T4, 2026-11-09). The per-save truth stays
//     in the cosave; this ini only seeds a fresh save. The shipped Cyberware.ini is never written;
//   - loads that ini into a fresh state (no cosave record);
//   - runs main.cpp's apply callback (HUD view show/hide) whenever they change;
//   - answers the view's fire-and-forget {"op":"uiSound","args":{"sound":"<SNDR EditorID>"}}
//     before the engine sees it (menu clicks, install-scene cues; agreed with MENU POLISH
//     e6baf9f9 2026-11-08 — one owner so the branches do not both patch main.cpp's dispatch).
#include "PCH.h"

#include <Windows.h>   // Get/WritePrivateProfileStringA (PCH.h does not pull the profile APIs in)
#include <ShlObj.h>    // SHGetKnownFolderPath: the real Documents folder (never under the MO2 VFS)
#ifdef ERROR
#    undef ERROR  // wingdi.h defines ERROR 0, which breaks REX::ERROR (same guard as main.cpp)
#endif
#ifdef GetObject
#    undef GetObject
#endif

#include "../core/cw_core.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>

namespace cwsnd { void UiCue(const std::string& sound, const std::string& fallback); }
namespace cwset {

constexpr const char* kSec = "Settings";

static cw::Engine* g_eng = nullptr;
static std::function<void(const cw::Settings&)> g_apply;
static std::string g_dir;   // [Settings] sSettingsDir override (the TEST build sets it; see Init)

// Where CyberwareSettings.ini persists — resolved once. Default: Documents\My Games\Fallout4\
// F4SE\Cyberware (never under the MO2 VFS, so nothing lands in overwrite). The [Settings]
// sSettingsDir override exists for the TEST build (Cyberware.Test.ini points it at a test folder), so a
// test run's writes can never seed the play build's new-game defaults (the lead, cw-int-2).
// Profile APIs need an ABSOLUTE path or they fall back to C:\Windows. Empty result = nowhere to
// write (headless edge case): persistence is skipped, the cosave still carries the settings.
static const std::string& IniPath()
{
    static const std::string path = [] {
        std::filesystem::path p;
        if (!g_dir.empty()) {
            p = g_dir;
        } else {
            PWSTR w = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &w)) || !w) return std::string();
            p = w;
            CoTaskMemFree(w);
            p /= "My Games\\Fallout4\\F4SE\\Cyberware";
        }
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        if (ec) return std::string();
        return (p / "CyberwareSettings.ini").string();
    }();
    return path;
}

// The folder CyberwareSettings.ini lives in ("" = nowhere to write). The in-game audit (cw_qa_audit.cpp) writes its
// report beside it, so a test build's report follows [Settings] sSettingsDir like every other test write.
std::string SettingsDir()
{
    const std::string& ini = IniPath();
    return ini.empty() ? std::string() : std::filesystem::path(ini).parent_path().string();
}

void Init(cw::Engine* e, std::function<void(const cw::Settings&)> apply, const std::string& settingsDir)
{
    g_eng = e;
    g_apply = std::move(apply);
    g_dir = settingsDir;
}

void Apply()
{
    if (g_eng && g_apply) g_apply(g_eng->state().settings);
}

static void Persist(const cw::Settings& s)
{
    const std::string& ini = IniPath();
    if (ini.empty()) return;
    const cw::json j = s.ToJson();
    for (auto& [k, v] : j.items()) {
        std::string val = v.is_boolean() ? (v.get<bool>() ? "1" : "0") : std::to_string(v.get<double>());
        WritePrivateProfileStringA(kSec, k.c_str(), val.c_str(), ini.c_str());
    }
}

void LoadDefaults()
{
    if (!g_eng) return;
    const std::string& ini = IniPath();
    std::error_code ec;
    if (ini.empty() || !std::filesystem::exists(ini, ec)) { Apply(); return; }
    cw::json j = cw::json::object();
    const cw::json shape = cw::Settings{}.ToJson();
    char buf[64];
    for (auto& [k, v] : shape.items()) {
        GetPrivateProfileStringA(kSec, k.c_str(), "", buf, sizeof(buf), ini.c_str());
        if (!buf[0]) continue;
        try {
            j[k] = v.is_boolean() ? cw::json(std::stoi(buf) != 0) : cw::json(std::stod(buf));
        } catch (const std::exception&) {}
    }
    g_eng->state().settings.MergeFromJson(j, /*strict=*/false);
    REX::INFO("settings: {} default(s) loaded from {}", j.size(), ini);
    Apply();
}

void AfterOp(const std::string& req, const std::string& res)
{
    if (!g_eng || req.find("\"setSettings\"") == std::string::npos) return;
    try {
        cw::json r = cw::json::parse(res);
        if (r.value("op", "") != "setSettings" || !r.value("ok", false)) return;
    } catch (const std::exception&) { return; }
    Persist(g_eng->state().settings);
    REX::INFO("settings: changed -> persisted to {}", IniPath().empty() ? "(no Documents folder - cosave only)" : IniPath());
    Apply();
}

// {"op":"uiSound","args":{"sound":"UIMenuOK"}}: play a sound descriptor by EditorID, no response.
// Returns true when the request was this op (the caller then skips the engine).
bool MaybeUiSound(const std::string& req)
{
    if (req.find("\"uiSound\"") == std::string::npos) return false;
    try {
        cw::json r = cw::json::parse(req);
        if (r.value("op", "") != "uiSound") return false;
        const cw::json a = r.value("args", cw::json::object());
        std::string snd = a.value("sound", ""), fallback = a.value("fallback", "");
        // EditorIDs only: letters, digits, underscore. Nothing else reaches the game.
        if (!snd.empty() && snd.size() <= 64 &&
            std::all_of(snd.begin(), snd.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; })) {
            const bool fbOk = fallback.size() <= 64 && std::all_of(fallback.begin(), fallback.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
            if (g_eng && g_eng->state().settings.menuSounds) cwsnd::UiCue(snd, fbOk ? fallback : std::string());
        }
        return true;
    } catch (const std::exception&) { return true; }
}

}  // namespace cwset
