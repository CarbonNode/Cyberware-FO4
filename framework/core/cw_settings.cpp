// cw_settings.cpp - player gameplay settings (BETA QA, 2026-11-08): cosave JSON round trip,
// clamped partial updates for the setSettings op, and the level-pricing curve.
#include "cw_core.h"

#include <algorithm>

namespace cw {

json Settings::ToJson() const
{
    return { { "capacityEnabled", capacityEnabled }, { "capacityMult", capacityMult },
             { "priceMult", priceMult },             { "levelPricing", levelPricing },
             { "slotLimits", slotLimits },           { "overCapacity", overCapacity },
             { "upgradeComponents", upgradeComponents },
             { "installScene", installScene },       { "hud", hud },
             { "menuSounds", menuSounds } };
}

void Settings::MergeFromJson(const json& j, bool strict)
{
    if (!j.is_object()) {
        if (strict) throw CwError("E_BAD_ARGS", "settings must be an object");
        return;
    }
    for (auto& [k, v] : j.items()) {
        bool bad = false;
        auto b = [&](bool& dst) {
            if (v.is_boolean()) dst = v.get<bool>();
            else if (strict) throw CwError("E_BAD_ARGS", "'" + k + "' must be a boolean");
        };
        auto f = [&](double& dst, double lo, double hi) {
            if (v.is_number()) dst = std::clamp(v.get<double>(), lo, hi);
            else if (strict) throw CwError("E_BAD_ARGS", "'" + k + "' must be a number");
        };
        if (k == "capacityEnabled") b(capacityEnabled);
        else if (k == "capacityMult") f(capacityMult, 0.1, 10.0);
        else if (k == "priceMult") f(priceMult, 0.0, 10.0);
        else if (k == "levelPricing") b(levelPricing);
        else if (k == "slotLimits") b(slotLimits);
        else if (k == "overCapacity") b(overCapacity);
        else if (k == "upgradeComponents") b(upgradeComponents);
        else if (k == "installScene") b(installScene);
        else if (k == "hud") b(hud);
        else if (k == "menuSounds") b(menuSounds);
        else bad = true;
        if (bad && strict) throw CwError("E_BAD_ARGS", "unknown setting '" + k + "'");
    }
}

double LevelPriceMult(int level)
{
    return std::clamp(0.50 + 0.025 * level, 0.55, 1.75);
}

}  // namespace cw
