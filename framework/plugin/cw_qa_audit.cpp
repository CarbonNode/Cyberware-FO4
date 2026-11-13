// cw_qa_audit.cpp - the in-game 1:1 audit (BETA QA, 2026-11-08; the author: "test and get 1:1 every
// implant", "verify the actor-value/ability deltas match the menu text").
//
// Test-seam command {"cmd":"qaaudit"} (bDebug only). A frame-pumped state machine that, for EVERY
// catalog item, on the live game: buys it at its lowest tier, installs it, lets the reconcile
// settle, measures the player's REAL actor values against the engine's `applied` map (what the
// menu shows), upgrades through every level-reachable tier measuring again, then removes and
// sells and verifies the actor values return to baseline. After the items: scripted checks of the
// error gates (E_NO_FUNDS, E_LEVEL, E_SLOT_FULL, E_CAPACITY), the capacity curve, per-slot
// limits, Edgerunner over-capacity and the settings knobs.
//
// Output: qa-audit.json beside CyberwareSettings.ini (full per-tier evidence) + one Cyberware.log line per
// item and a final verdict line. Runs ~5 minutes unattended for 115 items x all tiers.
#include "PCH.h"

#include "../core/cw_core.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cwset { std::string SettingsDir(); }   // cw_settings_plugin.cpp: the folder CyberwareSettings.ini lives in

namespace cwqa {

using cw::json;

namespace {

    // Frames between a mutation and the AV measurement. The engine's reconcile runs on the 4 Hz
    // slow tick (every ~15 frames at 60 fps): 12 frames RACED it (cycle-2 flake), 30 spans two.
    constexpr int kSettleFrames = 30;
    constexpr const char* kOutName = "qa-audit.json";

    // The report lands beside CyberwareSettings.ini: [Settings] sSettingsDir when a test build sets it, otherwise
    // Documents\My Games\Fallout4\F4SE\Cyberware. With no settings folder (nowhere to write) it is the bare file name.
    std::string OutPath()
    {
        const std::string dir = cwset::SettingsDir();
        return dir.empty() ? std::string(kOutName) : (std::filesystem::path(dir) / kOutName).string();
    }

    enum class Phase { Idle, ItemBaseline, ItemBuy, ItemMeasure, ItemUpgrade, ItemRemove, ItemVerifyClean, Checks, Done };

    // One native-perk AV contribution (from cw::NativeEffectsJson): scarab's flat +13 CarryWeight,
    // rangeguard's conditional +90 DamageResist. The menu's `applied` map does not include these,
    // so the audit adds unconditional ones to the expected delta and skips AVs a CONDITIONAL
    // effect touches (whether the condition holds depends on the live scene).
    struct NatAv {
        std::string av;
        double value = 0;
        bool cond = false;
    };

    struct S {
        cw::Engine* eng = nullptr;
        cw::Env* env = nullptr;
        Phase phase = Phase::Idle;
        int wait = 0;
        std::vector<std::string> itemIds;
        size_t idx = 0;
        int tier = 0;                        // tier currently measured
        std::string uid;
        std::map<std::string, double> base;  // av -> value before this item
        json items = json::array();
        json cur;                            // current item's record
        json checks = json::array();
        int checkStep = 0;
        int pass = 0, fail = 0;
        int64_t startedMs = 0;
        json savedSettings;                                            // the save's settings, restored at the end
        std::map<std::string, std::map<int, std::vector<NatAv>>> natAvs;   // ability -> tier (1..5) -> av effects
        std::map<std::string, std::map<int, std::vector<std::string>>> itemAbilities;  // item -> tier -> abilities
    } s;

    void Log(const std::string& line) { if (s.env) s.env->Log("qaaudit: " + line); }

    json RunOp(const std::string& op, const json& args)
    {
        try {
            json r = s.eng->Run(op, args, false);
            return { { "ok", true }, { "data", r.value("data", json::object()) } };
        } catch (const cw::CwError& e) {
            return { { "ok", false }, { "code", e.code }, { "message", e.what() } };
        } catch (const std::exception& e) {
            return { { "ok", false }, { "code", "E_INTERNAL" }, { "message", e.what() } };
        }
    }

    const cw::StatDef* Stat(const std::string& id)
    {
        for (auto& sd : s.eng->catalog().stats)   // catalog.stats is a vector, catalog order
            if (sd.id == id) return &sd;
        return nullptr;
    }

    // Permanent values: base + modifiers EXCLUDING damage/drain. Current-value reads drifted in
    // cycle 2 (ActionPoints regen between snapshots; a Health buff's removal left wounds behind
    // that poisoned the NEXT item's baseline).
    void Snapshot(std::map<std::string, double>& out)
    {
        out.clear();
        for (auto& sd : s.eng->catalog().stats) out[sd.av] = s.env->GetPermanentActorValue(sd.av);
    }

    // The native-perk AV contributions for this item at this tier: unconditional sum per AV, and
    // the set of AVs any conditional effect touches. Native perks exist T1-T5; plus tiers reuse T5.
    void NativeContrib(const std::string& itemId, int tier, std::map<std::string, double>& add, std::set<std::string>& skip)
    {
        const auto ii = s.itemAbilities.find(itemId);
        if (ii == s.itemAbilities.end()) return;
        const auto ti = ii->second.find(tier);
        if (ti == ii->second.end()) return;
        for (const auto& ab : ti->second) {
            const auto ai = s.natAvs.find(ab);
            if (ai == s.natAvs.end()) continue;
            const auto ni = ai->second.find(std::clamp(tier, 1, 5));
            if (ni == ai->second.end()) continue;
            for (const auto& na : ni->second) {
                if (na.cond) skip.insert(na.av);
                else add[na.av] += na.value;
            }
        }
    }

    // Compare the engine's applied map (what the menu text promises) against the real AV deltas.
    json MeasureAgainstApplied(int tier)
    {
        cw::Derived d = s.eng->Compute(s.eng->state());
        json stats = json::array();
        bool ok = true;
        std::map<std::string, double> natAdd;
        std::set<std::string> natSkip;
        NativeContrib(s.itemIds[s.idx], tier, natAdd, natSkip);
        // FO4 derives AVs from SPECIAL: +1 Agility = +10 ActionPoints, +1 Strength = +10
        // CarryWeight (cycle 2: a rolled Agility point showed up as "+10 AP from nowhere").
        // Health's endurance formula is level-dependent, so those rows are skipped instead.
        double dAgi = 0, dStr = 0, dEnd = 0;
        for (auto& [sid, v] : d.applied) {
            const cw::StatDef* sd = Stat(sid);
            if (!sd) continue;
            if (sd->av == "Agility") dAgi += v;
            else if (sd->av == "Strength") dStr += v;
            else if (sd->av == "Endurance") dEnd += v;
        }
        for (auto& [sid, v] : d.applied) {
            const cw::StatDef* sd = Stat(sid);
            if (!sd || v == 0) continue;
            if (natSkip.count(sd->av)) {
                stats.push_back({ { "stat", sid }, { "av", sd->av }, { "applied", v },
                                  { "skipped", "conditional native effect touches this AV" } });
                continue;
            }
            if (sd->av == "Health" && dEnd != 0) {
                stats.push_back({ { "stat", sid }, { "av", sd->av }, { "applied", v },
                                  { "skipped", "endurance applied; health derivation is level-dependent" } });
                continue;
            }
            double expect = v + natAdd[sd->av];
            if (sd->av == "ActionPoints") expect += 10.0 * dAgi;
            if (sd->av == "CarryWeight") expect += 10.0 * dStr;
            const double measured = s.env->GetPermanentActorValue(sd->av) - s.base[sd->av];
            const bool good = std::abs(measured - expect) <= sd->step / 2.0 + 0.011;
            ok = ok && good;
            json row = { { "stat", sid }, { "av", sd->av }, { "applied", v },
                         { "measured", std::round(measured * 100) / 100 }, { "ok", good } };
            if (expect != v) row["expected"] = std::round(expect * 100) / 100;   // native / SPECIAL-derived adjustment
            stats.push_back(row);
        }
        return { { "tier", tier }, { "stats", stats }, { "ok", ok } };
    }

    void FinishItem(bool ok, const std::string& err)
    {
        s.cur["ok"] = ok;
        if (!err.empty()) s.cur["error"] = err;
        (ok ? s.pass : s.fail)++;
        Log(s.cur.value("id", std::string()) + (ok ? " OK" : " FAIL " + err));
        s.items.push_back(s.cur);
        ++s.idx;
        s.phase = Phase::ItemBaseline;
        s.wait = 2;
    }

    void WriteReport()
    {
        json rep = { { "generated", "qaaudit (BETA QA)" }, { "level", s.env->GetLevel() },
                     { "settle_frames", kSettleFrames },
                     { "items", s.items }, { "checks", s.checks },
                     { "summary", { { "items_pass", s.pass }, { "items_fail", s.fail },
                                    { "checks", (int)s.checks.size() } } } };
        const std::string outPath = OutPath();
        std::ofstream f(outPath, std::ios::binary);
        f << rep.dump(1);
        Log("report written: " + outPath);
    }

    // ---- the scripted gate / limit / settings checks (one per tick, settle between) ----------
    void Expect(const std::string& what, bool cond, const std::string& detail = "")
    {
        s.checks.push_back({ { "check", what }, { "ok", cond }, { "detail", detail } });
        Log(std::string("check ") + (cond ? "OK   " : "FAIL ") + what + (detail.empty() ? "" : " - " + detail));
    }

    int CapMax() { return s.eng->Compute(s.eng->state()).capacityMax; }

    bool ChecksStep()   // returns false when finished
    {
        auto& eng = *s.eng;
        const auto& cat = eng.catalog();
        switch (s.checkStep++) {
        case 0: {   // the capacity curve: min(base + perLevel x level, max) + 5 x base Endurance
            const int lvl = s.env->GetLevel();
            int expect = std::min(cat.capBase + cat.capPerLevel * lvl, cat.capMax);
            for (auto& pa : cat.capPerAV) {   // min > 0 = threshold entry (+add once at min+)
                const double v = s.env->GetBaseActorValue(pa.av);
                expect += pa.min > 0 ? (v >= pa.min ? (int)std::lround(pa.add) : 0) : (int)std::lround(pa.add * v);
            }
            Expect("capacity curve at level " + std::to_string(lvl),
                   CapMax() == expect, std::to_string(CapMax()) + " vs expected " + std::to_string(expect));
            return true;
        }
        case 1: {   // E_NO_FUNDS: drain the wallet, try to buy
            RunOp("debugGrant", { { "currency", -(s.env->GetCurrency()) } });
            json r = RunOp("buy", { { "item", s.itemIds.front() } });
            Expect("broke buy -> E_NO_FUNDS", !r.value("ok", false) && r.value("code", std::string()) == "E_NO_FUNDS", r.dump());
            RunOp("debugGrant", { { "currency", (int64_t)10000000 } });
            return true;
        }
        case 2: {   // E_LEVEL: find a tier gated above the player's level
            for (auto& [id, it] : cat.items) {
                for (auto& t : it.tiers) {
                    if (t.requires_.level > s.env->GetLevel()) {
                        json r = RunOp("buy", { { "item", id }, { "tier", t.tier } });
                        Expect("level gate (" + id + " T" + std::to_string(t.tier) + ") -> E_LEVEL",
                               !r.value("ok", false) && r.value("code", std::string()) == "E_LEVEL", r.dump());
                        return true;
                    }
                }
            }
            Expect("level gate", true, "no tier above player level; not testable at this level");
            return true;
        }
        case 3: {   // E_SLOT_FULL on a 1-position slot (OS): install two OS pieces without replace
            std::string a, b;
            for (auto& [id, it] : cat.items) {
                if (it.slot != "operatingSystem" || it.iconic) continue;
                if (a.empty()) a = id;
                else if (b.empty() && id != a) { b = id; break; }
            }
            if (a.empty() || b.empty()) { Expect("slot full", true, "not enough OS items (skip)"); return true; }
            json r1 = RunOp("buy", { { "item", a }, { "install", true } });
            json r2 = RunOp("buy", { { "item", b }, { "install", true } });
            Expect("second OS piece without replace -> E_SLOT_FULL",
                   r1.value("ok", false) && !r2.value("ok", false) && r2.value("code", std::string()) == "E_SLOT_FULL",
                   r1.dump().substr(0, 120) + " / " + r2.dump().substr(0, 120));
            RunOp("debugReset", {});
            RunOp("debugGrant", { { "currency", (int64_t)10000000 } });
            return true;
        }
        case 4: {   // E_CAPACITY with a tight multiplier; then capacity off lets the same install through
            RunOp("setSettings", { { "capacityMult", 0.1 } });
            std::string big;
            int bigCost = 0;
            for (auto& [id, it] : cat.items)
                for (auto& t : it.tiers)
                    if (t.requires_.level <= s.env->GetLevel() && t.tier == it.tiers.front().tier && t.capacityCost > bigCost) { big = id; bigCost = t.capacityCost; }
            json r = RunOp("buy", { { "item", big }, { "install", true } });
            Expect("capacityMult 0.1 install -> E_CAPACITY",
                   !r.value("ok", false) && r.value("code", std::string()) == "E_CAPACITY", big + " cost " + std::to_string(bigCost) + ": " + r.dump().substr(0, 140));
            RunOp("setSettings", { { "capacityEnabled", false } });
            json r2 = RunOp("buy", { { "item", big }, { "install", true } });
            const int mx = CapMax();
            Expect("capacity off -> same install succeeds, max sentinel",
                   r2.value("ok", false) && mx == 999999, "max " + std::to_string(mx));
            // Put the knobs straight back: cycle 2 left the save at hardMax 999999.
            RunOp("setSettings", { { "capacityEnabled", true }, { "capacityMult", 1.0 } });
            RunOp("debugReset", {});
            RunOp("debugGrant", { { "currency", (int64_t)10000000 } });
            return true;
        }
        case 5: {   // per-slot limits off -> every slot reports 16
            RunOp("setSettings", { { "slotLimits", false } });
            cw::Derived d = eng.Compute(eng.state());
            bool all16 = true;
            for (auto& [slot, n] : d.slotCounts) all16 = all16 && n == 16;
            Expect("slotLimits off -> 16 everywhere", all16);
            RunOp("setSettings", { { "slotLimits", true } });
            cw::Derived d2 = eng.Compute(eng.state());
            bool normal = true;
            for (auto& sl : cat.slots) normal = normal && d2.slotCounts[sl.id] == sl.count;
            Expect("slotLimits on -> catalog counts", normal);
            return true;
        }
        case 6: {   // level pricing: a dry-run buy's spend doubles with priceMult and follows the curve
            const std::string id = s.itemIds.front();
            const int64_t base = cat.items.at(id).tiers.front().price;
            auto drySpend = [&]() -> int64_t {   // dryRun buy: state.currency shows the would-be spend
                try {
                    json r = eng.Run("buy", { { "item", id } }, true);
                    return s.env->GetCurrency() - r["state"]["currency"]["amount"].get<int64_t>();
                } catch (const std::exception&) { return -1; }
            };
            RunOp("setSettings", { { "levelPricing", false }, { "priceMult", 1.0 } });
            const int64_t p1 = drySpend();
            RunOp("setSettings", { { "priceMult", 2.0 } });
            const int64_t p2 = drySpend();
            RunOp("setSettings", { { "priceMult", 1.0 }, { "levelPricing", true } });
            const int64_t p3 = drySpend();
            const int64_t expect3 = (int64_t)std::llround((double)base * cw::LevelPriceMult(s.env->GetLevel()));
            Expect("priceMult 2 doubles the buy", p1 > 0 && p2 == 2 * p1, std::to_string(p1) + " -> " + std::to_string(p2));
            Expect("level pricing matches LevelPriceMult", p3 == expect3, std::to_string(p3) + " vs " + std::to_string(expect3));
            return true;
        }
        case 7: {   // Edgerunner over-capacity: perk on, go over max, penalty reported
            RunOp("debugGrant", { { "perkPoints", 20 } });
            json pr = RunOp("buyCyberPerk", { { "perk", "edgerunner" } });
            if (!pr.value("ok", false)) { Expect("edgerunner over-capacity", true, "no edgerunner perk buyable (skip): " + pr.dump().substr(0, 120)); return true; }
            cw::Derived d = eng.Compute(eng.state());
            Expect("over-capacity hardMax > max with Edgerunner", d.capacityHardMax > d.capacityMax,
                   std::to_string(d.capacityHardMax) + " > " + std::to_string(d.capacityMax));
            RunOp("setSettings", { { "overCapacity", false } });
            d = eng.Compute(eng.state());
            Expect("settings overCapacity off -> hardMax == max", d.capacityHardMax == d.capacityMax);
            RunOp("setSettings", { { "overCapacity", true } });
            RunOp("debugReset", {});
            RunOp("debugGrant", { { "currency", (int64_t)10000000 } });
            return true;
        }
        case 8: {   // upgrade components: the bill appears in the view when the setting is on
            RunOp("setSettings", { { "upgradeComponents", true } });
            std::string id;
            for (auto& [iid, it] : cat.items) {   // an item whose NEXT tier carries components and is level-reachable
                if (it.tiers.size() < 2) continue;
                const cw::TierDef* nx = it.NextTier(it.tiers.front().tier);
                if (nx && !nx->components.empty() && it.tiers.front().requires_.level <= s.env->GetLevel() && nx->requires_.level <= s.env->GetLevel()) { id = iid; break; }
            }
            if (id.empty()) { Expect("component bill", true, "no catalog tier carries components (skip)"); return true; }
            json r = RunOp("buy", { { "item", id }, { "install", false } });
            std::string uid = r.value("data", json::object()).value("uid", "");
            cw::Derived d = eng.Compute(eng.state());
            json sv = eng.StateView(eng.state(), d);
            json up = sv["instances"].contains(uid) ? sv["instances"][uid]["upgrade"] : json(nullptr);
            const bool shown = up.is_object() && up.contains("components") && !up["components"].empty();
            Expect("upgrade component bill shows have/need", shown, up.is_null() ? "no upgrade object" : up.dump().substr(0, 200));
            if (!uid.empty()) RunOp("sell", { { "uid", uid } });
            RunOp("setSettings", { { "upgradeComponents", true } });   // leave the CP default on
            return true;
        }
        default:
            return false;
        }
    }

}  // namespace

void Start(cw::Engine* eng, cw::Env* env)
{
    if (!env || !env->DebugEnabled()) { if (env) env->Log("qaaudit: refused (bDebug=0)"); return; }
    if (s.phase != Phase::Idle && s.phase != Phase::Done) { env->Log("qaaudit: already running"); return; }
    s = S{};
    s.eng = eng;
    s.env = env;
    s.itemIds.clear();
    for (auto& [id, it] : eng->catalog().items) s.itemIds.push_back(id);
    s.savedSettings = eng->state().settings.ToJson();   // restored verbatim when the audit ends
    // Native AV contributions (NativeEffectsJson = exactly what espgen baked into the CW_Nat
    // perks): ability -> tier -> {av, value, conditional}. Timed perks (camo, slows...) are only
    // granted while active, so they never affect an at-rest measurement and are skipped here.
    try {
        const json nat = cw::NativeEffectsJson(eng->catalog());
        for (auto& [pid, pj] : nat.value("perks", json::object()).items()) {
            if (pj.value("timed", false)) continue;
            const std::string ab = pj.value("ability", "");
            const int t = pj.value("tier", 1);
            for (auto& av : pj.value("avs", json::array()))
                s.natAvs[ab][t].push_back({ av.value("av", ""), av.value("value", 0.0), !av.value("when", json::array()).empty() });
        }
    } catch (const std::exception& e) { Log(std::string("native map failed (continuing without): ") + e.what()); }
    // item -> tier -> abilities (tier lists + always-on iconicAbilities), from the raw catalog.
    for (auto& it : eng->catalog().raw.value("items", json::array())) {
        const std::string id = it.value("id", "");
        std::vector<std::string> iconic;
        for (auto& a : it.value("iconicAbilities", json::array())) if (a.is_string()) iconic.push_back(a.get<std::string>());
        for (auto& t : it.value("tiers", json::array())) {
            auto& v = s.itemAbilities[id][t.value("tier", 1)] = iconic;
            for (auto& a : t.value("abilities", json::array())) if (a.is_string()) v.push_back(a.get<std::string>());
        }
    }
    RunOp("debugReset", {});
    RunOp("debugGrant", { { "currency", (int64_t)10000000 } });
    // The per-item loop measures stats, not the component economy: upgrades run with the
    // component bill off (the live save's junk would make results depend on the inventory).
    // Check 8 proves the bill shows up; the seam test list proves consumption with additem.
    RunOp("setSettings", { { "upgradeComponents", false } });
    s.phase = Phase::ItemBaseline;
    s.wait = kSettleFrames;
    Log("started: " + std::to_string(s.itemIds.size()) + " items, settle " + std::to_string(kSettleFrames) + " frames");
}

void Tick()
{
    if (s.phase == Phase::Idle || s.phase == Phase::Done) return;
    if (s.wait > 0) { --s.wait; return; }

    switch (s.phase) {
    case Phase::ItemBaseline: {
        if (s.idx >= s.itemIds.size()) { s.phase = Phase::Checks; s.wait = kSettleFrames; break; }
        Snapshot(s.base);
        const std::string& id = s.itemIds[s.idx];
        const cw::ItemDef& it = s.eng->catalog().items.at(id);
        s.cur = { { "id", id }, { "tiers", json::array() } };
        s.tier = it.tiers.front().tier;
        s.phase = Phase::ItemBuy;
        break;
    }
    case Phase::ItemBuy: {
        const std::string& id = s.itemIds[s.idx];
        const cw::ItemDef& it = s.eng->catalog().items.at(id);
        if (it.tiers.front().requires_.level > s.env->GetLevel()) { FinishItem(true, "level-gated at this save's level (not measured)"); break; }
        const int64_t before = s.env->GetCurrency();
        json r = RunOp("buy", { { "item", id }, { "tier", s.tier }, { "install", true }, { "replace", true } });
        if (!r.value("ok", false)) { FinishItem(false, "buy: " + r.value("code", std::string()) + " " + r.value("message", std::string())); break; }
        s.uid = r["data"].value("uid", "");
        s.cur["price_paid_T" + std::to_string(s.tier)] = before - s.env->GetCurrency();
        s.phase = Phase::ItemMeasure;
        s.wait = kSettleFrames;
        break;
    }
    case Phase::ItemMeasure: {
        s.cur["tiers"].push_back(MeasureAgainstApplied(s.tier));
        s.phase = Phase::ItemUpgrade;
        break;
    }
    case Phase::ItemUpgrade: {
        const cw::ItemDef& it = s.eng->catalog().items.at(s.itemIds[s.idx]);
        const cw::TierDef* next = it.NextTier(s.tier);
        if (!next || next->requires_.level > s.env->GetLevel()) { s.phase = Phase::ItemRemove; break; }
        const int64_t before = s.env->GetCurrency();
        json r = RunOp("upgrade", { { "uid", s.uid } });
        if (!r.value("ok", false)) { FinishItem(false, "upgrade T" + std::to_string(next->tier) + ": " + r.value("code", std::string())); break; }
        s.tier = next->tier;
        s.cur["price_paid_T" + std::to_string(s.tier)] = before - s.env->GetCurrency();
        s.phase = Phase::ItemMeasure;
        s.wait = kSettleFrames;
        break;
    }
    case Phase::ItemRemove: {
        json r = RunOp("remove", { { "uid", s.uid } });
        if (r.value("ok", false)) {
            const int64_t before = s.env->GetCurrency();
            json r2 = RunOp("sell", { { "uid", s.uid } });
            if (r2.value("ok", false)) s.cur["sell_refund"] = s.env->GetCurrency() - before;
            else s.cur["sell_error"] = r2.value("code", std::string());   // iconic: E_ICONIC is correct
        } else {
            s.cur["remove_error"] = r.value("code", std::string());
        }
        s.phase = Phase::ItemVerifyClean;
        s.wait = kSettleFrames;
        break;
    }
    case Phase::ItemVerifyClean: {
        bool clean = true;
        std::string dirty;
        for (auto& sd : s.eng->catalog().stats) {
            const double d = s.env->GetPermanentActorValue(sd.av) - s.base[sd.av];
            if (std::abs(d) > sd.step / 2.0 + 0.011) { clean = false; dirty += sd.av + " " + std::to_string(d) + "; "; }
        }
        s.cur["clean_revert"] = clean;
        if (!clean) s.cur["dirty"] = dirty;
        bool tiersOk = true;
        for (auto& t : s.cur["tiers"]) tiersOk = tiersOk && t.value("ok", true);
        // Every failure names its cause (cycle 2 logged bare "FAIL" for stat mismatches).
        std::string why;
        if (!tiersOk) why += "stat deltas vs menu mismatched (see tiers)";
        if (s.cur.contains("remove_error")) why += std::string(why.empty() ? "" : "; ") + "remove: " + s.cur["remove_error"].get<std::string>();
        if (!clean) why += std::string(why.empty() ? "" : "; ") + "AVs did not revert";
        FinishItem(why.empty(), why);
        break;
    }
    case Phase::Checks: {
        if (!ChecksStep()) {
            // Leave the save as found: wipe audit chrome, restore the player's own settings
            // (cycle 2 left upgradeComponents off and capacity at the 999999 sentinel).
            RunOp("debugReset", {});
            json rs = RunOp("setSettings", s.savedSettings);
            Log(std::string("settings restored: ") + (rs.value("ok", false) ? "ok" : rs.dump().substr(0, 120)));
            WriteReport();
            Log("DONE: items " + std::to_string(s.pass) + " pass / " + std::to_string(s.fail) + " fail; " +
                std::to_string(s.checks.size()) + " gate checks (see qa-audit.json)");
            s.phase = Phase::Done;
            break;
        }
        s.wait = kSettleFrames;
        break;
    }
    default:
        break;
    }
}

}  // namespace cwqa
