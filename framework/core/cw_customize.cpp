// cw_customize.cpp - player colour / glow choices (docs/UI-API.md section 9). Game-independent: the plugin reads the
// resolved view (plugin/tint.cpp applies it to the worn 3D, the HUD feed sends the HUD colours).
//
// What is saved is only what the player changed (State.customize, in the cosave JSON). CustomizeResolve lays that over
// the defaults, so a target added later has a sane value in an old save, and "color": null means "the material's own
// colour" (the element look the BGSM was authored with).
#include "cw_core.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace cw {

namespace {

constexpr int kGlowDefault = 50;   // 50 = the brightness the material was authored with (see tint.cpp)
const char* const kTargets[] = { "mantis", "monowire", "gorilla", "launcher", "kiroshi", "sandevistan", "hud" };

bool KnownTarget(const std::string& t)
{
    return std::any_of(std::begin(kTargets), std::end(kTargets), [&](const char* k) { return t == k; });
}

json DefaultTarget(const std::string& t)
{
    if (t == "hud") return { { "color", "#fcee0a" }, { "glow", kGlowDefault } };   // the HUD's signal yellow
    return { { "color", nullptr }, { "glow", kGlowDefault } };
}

// "#RRGGBB", any case, normalised to lower case. Anything else is refused: this string reaches the HUD's CSS.
std::string NormalColor(const std::string& s)
{
    bool ok = s.size() == 7 && s[0] == '#';
    for (size_t i = 1; ok && i < s.size(); ++i) ok = std::isxdigit((unsigned char)s[i]) != 0;
    if (!ok) throw CwError("E_BAD_ARGS", "color must be #RRGGBB, got '" + s + "'");
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

}  // namespace

json CustomizeResolve(const json& stored)
{
    json targets = json::object();
    const json saved = stored.is_object() ? stored.value("targets", json::object()) : json::object();
    for (const char* t : kTargets) {
        json v = DefaultTarget(t);
        if (saved.is_object() && saved.contains(t) && saved[t].is_object()) {
            const json& s = saved[t];
            if (s.contains("color") && (s["color"].is_string() || s["color"].is_null())) v["color"] = s["color"];
            if (s.contains("glow") && s["glow"].is_number()) v["glow"] = std::clamp(s["glow"].get<int>(), 0, 100);
        }
        targets[t] = v;
    }
    bool seams = stored.is_object() && stored.contains("seams") && stored["seams"].is_boolean() ? stored["seams"].get<bool>() : false;
    bool pulses = stored.is_object() && stored.contains("pulses") && stored["pulses"].is_boolean() ? stored["pulses"].get<bool>() : true;
    return { { "targets", targets }, { "seams", seams }, { "pulses", pulses } };
}

void CustomizeSet(json& stored, const json& args)
{
    if (!stored.is_object()) stored = json::object();
    bool any = false;
    if (args.contains("seams")) {
        if (!args["seams"].is_boolean()) throw CwError("E_BAD_ARGS", "seams must be true or false");
        stored["seams"] = args["seams"]; any = true;
    }
    if (args.contains("pulses")) {
        if (!args["pulses"].is_boolean()) throw CwError("E_BAD_ARGS", "pulses must be true or false");
        stored["pulses"] = args["pulses"]; any = true;
    }
    if (args.contains("target")) {
        if (!args["target"].is_string() || !KnownTarget(args["target"].get<std::string>()))
            throw CwError("E_UNKNOWN", "no customize target '" + (args["target"].is_string() ? args["target"].get<std::string>() : args["target"].dump()) + "'");
        const std::string t = args["target"].get<std::string>();
        if (!stored.contains("targets") || !stored["targets"].is_object()) stored["targets"] = json::object();
        json& e = stored["targets"][t];
        if (!e.is_object()) e = json::object();
        if (args.contains("color")) {
            if (args["color"].is_null()) {
                if (t == "hud") throw CwError("E_BAD_ARGS", "the HUD accent needs a colour");
                e["color"] = nullptr;
            }
            else if (args["color"].is_string()) e["color"] = NormalColor(args["color"].get<std::string>());
            else throw CwError("E_BAD_ARGS", "color must be #RRGGBB or null");
            any = true;
        }
        if (args.contains("glow")) {
            if (!args["glow"].is_number()) throw CwError("E_BAD_ARGS", "glow must be a number from 0 to 100");
            e["glow"] = std::clamp((int)std::lround(args["glow"].get<double>()), 0, 100);
            any = true;
        }
    }
    if (!any) throw CwError("E_BAD_ARGS", "setCustomize needs target + color/glow, or seams, or pulses");
}

void CustomizeReset(json& stored, const json& args)
{
    if (args.contains("target")) {
        if (!args["target"].is_string() || !KnownTarget(args["target"].get<std::string>()))
            throw CwError("E_UNKNOWN", "no customize target");
        if (stored.is_object() && stored.contains("targets") && stored["targets"].is_object()) stored["targets"].erase(args["target"].get<std::string>());
    } else {
        stored = json::object();
    }
}

}  // namespace cw
