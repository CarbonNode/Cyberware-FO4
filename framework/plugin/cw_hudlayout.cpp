// cw_hudlayout.cpp - see cw_hudlayout.h. MENU POLISH, 2026-11-09.
#include "PCH.h"
#include "cw_hudlayout.h"

#include <Windows.h>   // MoveFileExA
#include <ShlObj.h>    // SHGetKnownFolderPath: the real Documents folder (never under the MO2 VFS)
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>

namespace hudlayout {
namespace {
    using cw::json;
    std::string g_dir;
    bool g_loaded = false;
    json g_layout = json::object();

    double Num(const json& o, const char* k, double lo, double hi, double def)
    {
        if (!o.is_object() || !o.contains(k) || !o[k].is_number()) return def;
        const double v = o[k].get<double>();
        if (!std::isfinite(v)) return def;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // The one shape the page writes (views/CyberwareHUD/layout.js, "Saved layout"):
    //   {"v":1, "el":{"<id>":{"ax":"l|r","ay":"t|b","x":0..1,"y":0..1,"s":0.5..2.5}},
    //           "ring":{"l":..,"r":..,"t":..,"b":.. (percent of the screen, 0..45),"s":0.5..2.5}}
    // Anything else is dropped, so a hand-edited or damaged file can only lose entries, never break the HUD.
    json Sanitize(const json& in)
    {
        json out = { { "v", 1 }, { "el", json::object() } };
        if (!in.is_object()) return out;
        static const std::regex idOk("^[a-z][a-z0-9_-]{0,23}$");
        if (in.contains("el") && in["el"].is_object()) {
            int n = 0;
            for (auto& [id, e] : in["el"].items()) {
                if (n >= 32 || !e.is_object() || !std::regex_match(id, idOk)) continue;
                const std::string ax = e.value("ax", std::string("l")), ay = e.value("ay", std::string("t"));
                out["el"][id] = { { "ax", ax == "r" ? "r" : "l" }, { "ay", ay == "b" ? "b" : "t" },
                                  { "x", Num(e, "x", 0, 1, 0) }, { "y", Num(e, "y", 0, 1, 0) }, { "s", Num(e, "s", 0.5, 2.5, 1) } };
                ++n;
            }
        }
        if (in.contains("ring") && in["ring"].is_object()) {
            const json& r = in["ring"];
            out["ring"] = { { "l", Num(r, "l", 0, 45, 7.5) }, { "r", Num(r, "r", 0, 45, 7.5) }, { "t", Num(r, "t", 0, 45, 8) },
                            { "b", Num(r, "b", 0, 45, 18) }, { "s", Num(r, "s", 0.5, 2.5, 1) } };
        }
        return out;
    }
}  // namespace

void Init(const std::string& dirOverride) { g_dir = dirOverride; }

const std::string& Path()
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
        return (p / "HudLayout.json").string();
    }();
    return path;
}

std::string PrefsIni()
{
    const std::string& p = Path();
    if (p.empty()) return std::string();
    return (std::filesystem::path(p).parent_path() / "CyberwarePrefs.ini").string();
}

const cw::json& Get()
{
    if (g_loaded) return g_layout;
    g_loaded = true;
    const std::string& p = Path();
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(p, ec)) {
        REX::INFO("hudlayout: none saved ({}): stock layout", p.empty() ? "no Documents folder" : p);
        return g_layout;
    }
    try {
        std::ifstream f(p, std::ios::binary);
        g_layout = Sanitize(json::parse(f));
        REX::INFO("hudlayout: loaded {} element(s){} from {}", g_layout["el"].size(), g_layout.contains("ring") ? " + threat ring" : "", p);
    } catch (const std::exception& e) {
        REX::WARN("hudlayout: {} unreadable ({}): stock layout", p, e.what());
        g_layout = json::object();
    }
    return g_layout;
}

bool Save(const cw::json& layout, std::string& err)
{
    const std::string& p = Path();
    if (p.empty()) { err = "no folder to write the layout to"; return false; }
    json clean = Sanitize(layout);
    // write next to it, then swap: a crash mid-write leaves the old layout, never half a file
    const std::string tmp = p + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + tmp; return false; }
        f << clean.dump(1);
        if (!f.good()) { err = "write failed: " + tmp; return false; }
    }
    if (!MoveFileExA(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = "cannot replace " + p + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    g_layout = clean;
    g_loaded = true;
    REX::INFO("hudlayout: saved {} element(s){} to {}", clean["el"].size(), clean.contains("ring") ? " + threat ring" : "", p);
    return true;
}

}  // namespace hudlayout
