// cw_catalog.cpp - parse + validate cyberware.json (docs/CATALOG-SCHEMA.md) and (de)serialize State.
#include "cw_core.h"

#include <algorithm>
#include <regex>
#include <sstream>

namespace cw {

namespace {

struct Problems {
    std::vector<std::string> list;
    void add(const std::string& s) { list.push_back(s); }
};

bool ValidId(const std::string& s)
{
    static const std::regex re("^[A-Za-z][A-Za-z0-9_]{0,47}$");
    return std::regex_match(s, re);
}

std::string Str(const json& o, const char* k, const std::string& def = "")
{
    auto it = o.find(k);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : def;
}
template <class T>
T Num(const json& o, const char* k, T def)
{
    auto it = o.find(k);
    return (it != o.end() && it->is_number()) ? it->get<T>() : def;
}
bool Bool(const json& o, const char* k, bool def)
{
    auto it = o.find(k);
    return (it != o.end() && it->is_boolean()) ? it->get<bool>() : def;
}
std::vector<std::string> StrList(const json& o, const char* k)
{
    std::vector<std::string> v;
    auto it = o.find(k);
    if (it != o.end() && it->is_array())
        for (auto& e : *it)
            if (e.is_string()) v.push_back(e.get<std::string>());
    return v;
}

StatMap ParseStats(const json& o, const char* k, const std::string& where, Problems& p)
{
    StatMap m;
    auto it = o.find(k);
    if (it == o.end()) return m;
    if (!it->is_object()) { p.add(where + ": '" + k + "' must be an object of stat -> number"); return m; }
    for (auto& [s, v] : it->items()) {
        if (!v.is_number()) { p.add(where + ": stat '" + s + "' is not a number"); continue; }
        m[s] = v.get<double>();
    }
    return m;
}

std::vector<RollGroup> ParseRolls(const json& o, const std::string& where, Problems& p)
{
    std::vector<RollGroup> out;
    auto it = o.find("rolls");
    if (it == o.end()) return out;
    if (!it->is_array()) { p.add(where + ": 'rolls' must be an array"); return out; }
    for (auto& g : *it) {
        RollGroup rg;
        rg.count = Num<int>(g, "count", 1);
        auto pit = g.find("pool");
        if (pit == g.end() || !pit->is_array() || pit->empty()) { p.add(where + ": roll group has no pool"); continue; }
        for (auto& e : *pit) {
            RollOption ro;
            ro.stat = Str(e, "stat");
            ro.min = Num<double>(e, "min", 0);
            ro.max = Num<double>(e, "max", ro.min);
            ro.weight = Num<double>(e, "weight", 1);
            if (ro.max < ro.min) p.add(where + ": roll '" + ro.stat + "' has max < min");
            if (ro.weight <= 0) p.add(where + ": roll '" + ro.stat + "' weight must be > 0");
            rg.pool.push_back(ro);
        }
        if (rg.count < 1 || rg.count > (int)rg.pool.size())
            p.add(where + ": roll count " + std::to_string(rg.count) + " must be 1.." + std::to_string(rg.pool.size()) + " (rolls draw without repeats)");
        out.push_back(rg);
    }
    return out;
}

std::vector<TierDef> ParseTiers(const json& o, const std::string& where, Problems& p)
{
    std::vector<TierDef> out;
    auto it = o.find("tiers");
    if (it == o.end() || !it->is_array() || it->empty()) { p.add(where + ": needs a non-empty 'tiers' array"); return out; }
    for (auto& t : *it) {
        TierDef td;
        td.tier = Num<int>(t, "tier", 0);
        std::string w = where + " tier " + std::to_string(td.tier);
        if (td.tier < 1) p.add(where + ": tier number must be >= 1");
        td.price = Num<int64_t>(t, "price", 0);
        if (t.contains("upgradePrice") && t["upgradePrice"].is_number()) td.upgradePrice = t["upgradePrice"].get<int64_t>();
        td.capacityCost = Num<int>(t, "capacityCost", 0);
        td.capacityGrant = Num<int>(t, "capacityGrant", 0);
        td.modSlots = Num<int>(t, "modSlots", 0);
        td.stats = ParseStats(t, "stats", w, p);
        td.abilities = StrList(t, "abilities");
        td.rolls = ParseRolls(t, w, p);
        if (t.contains("requires")) td.requires_.level = Num<int>(t["requires"], "level", 0);
        for (auto& ce : t.value("components", json::array())) {
            CompCost cc{ Str(ce, "form"), Str(ce, "name"), Num<int>(ce, "count", 0) };
            if (cc.form.empty() || cc.count < 1) p.add(w + ": components entries need 'form' and a positive 'count'");
            if (cc.name.empty()) cc.name = cc.form;
            td.components.push_back(cc);
        }
        out.push_back(td);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.tier < b.tier; });
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i].tier == out[i - 1].tier) p.add(where + ": tier " + std::to_string(out[i].tier) + " listed twice");
    return out;
}

Scope ParseScope(const json& e)
{
    Scope s;
    auto it = e.find("scope");
    if (it == e.end() || !it->is_object()) return s;
    s.slot = Str(*it, "slot");
    s.item = Str(*it, "item");
    if (it->contains("iconic") && (*it)["iconic"].is_boolean()) s.iconic = (*it)["iconic"].get<bool>();
    return s;
}

}  // namespace

bool Scope::Matches(const ItemDef& it) const
{
    if (!slot.empty() && slot != it.slot) return false;
    if (!item.empty() && item != it.id) return false;
    if (iconic && *iconic != it.iconic) return false;
    return true;
}

const TierDef* ItemDef::FindTier(int t) const
{
    for (auto& x : tiers) if (x.tier == t) return &x;
    return nullptr;
}
const TierDef* ItemDef::NextTier(int t) const
{
    for (auto& x : tiers) if (x.tier > t) return &x;
    return nullptr;
}
const VariantDef* ItemDef::FindVariant(const std::string& v) const
{
    for (auto& x : variants) if (x.id == v) return &x;
    return nullptr;
}
const TierDef* ModDef::FindTier(int t) const
{
    for (auto& x : tiers) if (x.tier == t) return &x;
    return nullptr;
}
const SlotDef* Catalog::FindSlot(const std::string& id) const
{
    for (auto& s : slots) if (s.id == id) return &s;
    return nullptr;
}
const StatDef* Catalog::FindStat(const std::string& id) const
{
    for (auto& s : stats) if (s.id == id) return &s;
    return nullptr;
}

Catalog Catalog::FromJson(const json& j)
{
    Problems p;
    Catalog c;
    if (!j.is_object()) throw CwError("E_CATALOG", "catalog root is not an object");
    c.raw = j;
    c.schemaVersion = Num<int>(j, "schemaVersion", 1);
    if (c.schemaVersion != 1) p.add("schemaVersion " + std::to_string(c.schemaVersion) + " is not supported (expected 1)");
    if (j.contains("currency")) {
        c.currencyForm = Str(j["currency"], "form", c.currencyForm);
        c.currencyName = Str(j["currency"], "name", c.currencyName);
    }
    if (j.contains("capacity")) {
        c.capBase = Num<int>(j["capacity"], "base", c.capBase);
        c.capPerLevel = Num<int>(j["capacity"], "perLevel", c.capPerLevel);
        c.capMax = Num<int>(j["capacity"], "max", c.capMax);
        for (auto& pa : j["capacity"].value("perAV", json::array())) {
            std::string av = Str(pa, "av");
            if (av.empty()) p.add("capacity.perAV entry needs 'av'");
            c.capPerAV.push_back({ av, Num<double>(pa, "add", 0), Num<double>(pa, "min", 0) });
        }
        for (auto& [k, v] : j["capacity"].items())
            if (k != "base" && k != "perLevel" && k != "max" && k != "perAV") c.warnings.push_back("capacity: unknown key '" + k + "' (ignored)");
    }
    static const std::set<std::string> kTop = { "schemaVersion", "_comment", "currency", "capacity", "capacityItems", "rerollMult", "perkPoints",
                                                "sellMult", "iconicUpgradeMult", "perkRefund", "tiers", "slots", "stats", "abilities", "items",
                                                "mods", "cyberPerks", "ripperdocs", "chipModel", "chairModel", "tierLadder" };
    for (auto& [k, v] : j.items())
        if (!kTop.count(k)) c.warnings.push_back("top level: key '" + k + "' is not used by the engine (ignored)");
    if (j.contains("perkPoints")) {
        c.ppStart = Num<int>(j["perkPoints"], "start", c.ppStart);
        c.ppPerLevels = Num<int>(j["perkPoints"], "perLevels", c.ppPerLevels);
    }
    c.sellMult = Num<double>(j, "sellMult", c.sellMult);
    c.iconicUpgradeMult = Num<double>(j, "iconicUpgradeMult", c.iconicUpgradeMult);
    c.rerollMult = Num<double>(j, "rerollMult", c.rerollMult);
    c.perkRefund = Bool(j, "perkRefund", c.perkRefund);
    if (j.contains("tiers") && j["tiers"].is_array()) c.tiersRaw = j["tiers"];

    for (auto& s : j.value("slots", json::array())) {
        SlotDef sd{ Str(s, "id"), Str(s, "name"), Num<int>(s, "count", 1) };
        if (!ValidId(sd.id)) p.add("slot id '" + sd.id + "' is not a valid id ([A-Za-z][A-Za-z0-9_]*)");
        if (sd.count < 1 || sd.count > 16) p.add("slot '" + sd.id + "' count must be 1..16");
        if (c.FindSlot(sd.id)) p.add("slot '" + sd.id + "' defined twice");
        c.slots.push_back(sd);
    }
    if (c.slots.empty()) p.add("no slots defined");

    for (auto& s : j.value("stats", json::array())) {
        StatDef sd;
        sd.id = Str(s, "id"); sd.name = Str(s, "name", sd.id); sd.av = Str(s, "av"); sd.format = Str(s, "format", "+{v}");
        sd.step = Num<double>(s, "step", 1); sd.bits = Num<int>(s, "bits", 6); sd.negBits = Num<int>(s, "negBits", 0);
        sd.custom = Bool(s, "custom", false); sd.avName = Str(s, "avName", sd.name);
        if (sd.custom && sd.av.rfind("CW_", 0) != 0) p.add("stat '" + sd.id + "' is custom, so its av '" + sd.av + "' must start with CW_ (espgen creates it)");
        if (sd.custom && !ValidId(sd.av)) p.add("stat '" + sd.id + "' custom av '" + sd.av + "' is not a valid EditorID");
        if (!ValidId(sd.id)) p.add("stat id '" + sd.id + "' is not a valid id");
        if (sd.av.empty()) p.add("stat '" + sd.id + "' has no 'av' (ActorValueInfo EditorID)");
        if (sd.step <= 0) p.add("stat '" + sd.id + "' step must be > 0");
        if (sd.bits < 1 || sd.bits > 12 || sd.negBits < 0 || sd.negBits > 12) p.add("stat '" + sd.id + "' bits must be 1..12 and negBits 0..12");
        if (c.FindStat(sd.id)) p.add("stat '" + sd.id + "' defined twice");
        c.stats.push_back(sd);
    }

    auto checkStats = [&](const StatMap& m, const std::string& where) {
        for (auto& [s, v] : m) if (!c.FindStat(s)) p.add(where + ": unknown stat '" + s + "'");
    };
    auto checkRolls = [&](const std::vector<RollGroup>& g, const std::string& where) {
        for (auto& rg : g) for (auto& ro : rg.pool) if (!c.FindStat(ro.stat)) p.add(where + ": roll uses unknown stat '" + ro.stat + "'");
    };

    for (auto& a : j.value("abilities", json::array())) {
        AbilityDef ad;
        ad.id = Str(a, "id"); ad.name = Str(a, "name", ad.id); ad.description = Str(a, "description");
        ad.perk = Str(a, "perk"); ad.spell = Str(a, "spell"); ad.raw = a;
        for (auto& e : a.value("effects", json::array())) ad.effects.push_back({ Str(e, "av"), Num<double>(e, "value", 0) });
        if (!ValidId(ad.id)) p.add("ability id '" + ad.id + "' is not a valid id");
        if (ad.perk.empty() && ad.spell.empty() && ad.effects.empty()) p.add("ability '" + ad.id + "' needs effects, a spell or a perk");
        if (c.abilities.count(ad.id)) p.add("ability '" + ad.id + "' defined twice");
        c.abilities[ad.id] = ad;
    }
    auto checkAbilities = [&](const std::vector<std::string>& v, const std::string& where) {
        for (auto& a : v) if (!c.abilities.count(a)) p.add(where + ": unknown ability '" + a + "'");
    };

    for (auto& it : j.value("items", json::array())) {
        ItemDef d;
        d.id = Str(it, "id"); d.name = Str(it, "name", d.id); d.slot = Str(it, "slot");
        d.manufacturer = Str(it, "manufacturer"); d.description = Str(it, "description"); d.icon = Str(it, "icon");
        d.iconic = Bool(it, "iconic", false);
        d.iconicAbilities = StrList(it, "iconicAbilities");
        d.modSlotTypes = StrList(it, "modSlotTypes");
        d.raw = it;
        if (it.contains("chipTier") && it["chipTier"].is_number()) d.chipTier = it["chipTier"].get<int>();
        std::string w = "item '" + d.id + "'";
        if (!ValidId(d.id)) p.add(w + ": id is not a valid id");
        if (!c.FindSlot(d.slot)) p.add(w + ": unknown slot '" + d.slot + "'");
        for (auto& v : it.value("variants", json::array()))
            d.variants.push_back({ Str(v, "id"), Str(v, "name"), Str(v, "icon"), Str(v, "armo"), Num<int64_t>(v, "price", 0) });
        if (d.variants.empty()) d.variants.push_back({ "default", "Standard", "", "", 0 });
        d.tiers = ParseTiers(it, w, p);
        for (auto& t : d.tiers) {
            std::string tw = w + " tier " + std::to_string(t.tier);
            checkStats(t.stats, tw); checkRolls(t.rolls, tw); checkAbilities(t.abilities, tw);
            if (t.modSlots > (int)d.modSlotTypes.size()) p.add(tw + ": modSlots " + std::to_string(t.modSlots) + " > modSlotTypes length");
        }
        checkAbilities(d.iconicAbilities, w + " iconicAbilities");
        if (d.chipTier && !d.FindTier(*d.chipTier)) p.add(w + ": chipTier is not one of its tiers");
        if (c.items.count(d.id)) p.add(w + ": defined twice");
        c.items[d.id] = d;
        c.itemOrder.push_back(d.id);
    }

    for (auto& m : j.value("mods", json::array())) {
        ModDef d;
        d.id = Str(m, "id"); d.name = Str(m, "name", d.id); d.type = Str(m, "type");
        d.description = Str(m, "description"); d.icon = Str(m, "icon"); d.raw = m;
        std::string w = "mod '" + d.id + "'";
        if (!ValidId(d.id)) p.add(w + ": id is not a valid id");
        if (d.type.empty()) p.add(w + ": needs a 'type' (matches an item's modSlotTypes entry)");
        d.tiers = ParseTiers(m, w, p);
        for (auto& t : d.tiers) {
            std::string tw = w + " tier " + std::to_string(t.tier);
            checkStats(t.stats, tw); checkRolls(t.rolls, tw); checkAbilities(t.abilities, tw);
        }
        if (c.mods.count(d.id)) p.add(w + ": defined twice");
        c.mods[d.id] = d;
        c.modOrder.push_back(d.id);
    }

    static const std::set<std::string> kEffectTypes = { "capacity", "stat", "statMult", "statPerPiece", "slotCount", "capacityCostMult", "costMult", "ability",
                                                         "capacityPerAV", "overCapacity", "extraRolls", "rerollCostMult", "rerollKeepOld" };
    for (auto& pk : j.value("cyberPerks", json::array())) {
        CyberPerkDef d;
        d.id = Str(pk, "id"); d.name = Str(pk, "name", d.id); d.description = Str(pk, "description");
        d.fo4Perk = Str(pk, "fo4Perk"); d.autoGrant = Bool(pk, "auto", false);
        d.maxRank = Num<int>(pk, "maxRank", 1); d.cost = Num<int>(pk, "cost", 1); d.raw = pk;
        d.prereq = StrList(pk, "prereq");
        if (pk.contains("requires")) d.requires_.level = Num<int>(pk["requires"], "level", 0);
        std::string w = "cyberPerk '" + d.id + "'";
        if (!ValidId(d.id)) p.add(w + ": id is not a valid id");
        if (d.autoGrant && d.fo4Perk.empty()) p.add(w + ": auto perks need 'fo4Perk'");
        if (d.maxRank < 1) p.add(w + ": maxRank must be >= 1");
        for (auto& e : pk.value("effects", json::array())) {
            PerkEffect pe;
            pe.type = Str(e, "type"); pe.stat = Str(e, "stat"); pe.slot = Str(e, "slot"); pe.ability = Str(e, "ability");
            pe.add = Num<double>(e, "add", 0); pe.mult = Num<double>(e, "mult", 0); pe.scope = ParseScope(e);
            pe.base = Bool(e, "base", false);
            pe.av = Str(e, "av"); pe.maxPct = Num<double>(e, "maxPct", 0); pe.healthPctPerPoint = Num<double>(e, "healthPctPerPoint", 0);
            if (pe.type == "capacityPerAV" && pe.av.empty()) p.add(w + ": capacityPerAV needs 'av'");
            if (pe.type == "overCapacity") {
                if (pe.stat.empty()) pe.stat = "health";
                if (!c.FindStat(pe.stat)) p.add(w + ": overCapacity penalty stat '" + pe.stat + "' is not a stat");
                if (pe.maxPct <= 0) p.add(w + ": overCapacity needs maxPct > 0");
            }
            if (!kEffectTypes.count(pe.type)) p.add(w + ": unknown effect type '" + pe.type + "'");
            if ((pe.type == "stat" || pe.type == "statMult" || pe.type == "statPerPiece") && !c.FindStat(pe.stat)) p.add(w + ": effect uses unknown stat '" + pe.stat + "'");
            if (pe.type == "slotCount" && !c.FindSlot(pe.slot)) p.add(w + ": slotCount uses unknown slot '" + pe.slot + "'");
            if (pe.type == "ability" && !c.abilities.count(pe.ability)) p.add(w + ": unknown ability '" + pe.ability + "'");
            if (!pe.scope.slot.empty() && !c.FindSlot(pe.scope.slot)) p.add(w + ": scope uses unknown slot '" + pe.scope.slot + "'");
            if (!pe.scope.item.empty() && !c.items.count(pe.scope.item)) p.add(w + ": scope uses unknown item '" + pe.scope.item + "'");
            d.effects.push_back(pe);
        }
        if (c.perks.count(d.id)) p.add(w + ": defined twice");
        c.perks[d.id] = d;
        c.perkOrder.push_back(d.id);
    }

    for (auto& [id, pd] : c.perks)
        for (auto& pre : pd.prereq) {
            if (!c.perks.count(pre)) p.add("cyberPerk '" + id + "': prereq '" + pre + "' is not a cyber perk");
            if (pre == id) p.add("cyberPerk '" + id + "' lists itself as prereq");
        }

    for (auto& ci : j.value("capacityItems", json::array())) {
        CapacityItemDef d{ Str(ci, "id"), Str(ci, "name"), Str(ci, "form"), Num<int>(ci, "add", 0), Num<int>(ci, "max", 1) };
        if (d.name.empty()) d.name = d.id;
        std::string w = "capacityItem '" + d.id + "'";
        if (!ValidId(d.id)) p.add(w + ": id is not a valid id");
        if (d.add <= 0 || d.max < 1) p.add(w + ": needs add > 0 and max >= 1");
        if (!d.form.empty() && d.form.find('|') == std::string::npos) p.add(w + ": form must look like Plugin.esm|00ABCD");
        if (c.capacityItems.count(d.id)) p.add(w + ": defined twice");
        c.capacityItems[d.id] = d;
        c.capacityItemOrder.push_back(d.id);
    }

    for (auto& r : j.value("ripperdocs", json::array())) {
        RipperdocDef d;
        d.id = Str(r, "id"); d.name = Str(r, "name", d.id); d.priceMult = Num<double>(r, "priceMult", 1.0); d.raw = r;
        if (r.contains("sellMult") && r["sellMult"].is_number()) d.sellMult = r["sellMult"].get<double>();
        std::string w = "ripperdoc '" + d.id + "'";
        if (!ValidId(d.id)) p.add(w + ": id is not a valid id");
        for (auto& s : r.value("stock", json::array())) {
            StockEntry se{ Str(s, "item"), {} };
            for (auto& t : s.value("tiers", json::array())) if (t.is_number_integer()) se.tiers.push_back(t.get<int>());
            auto iit = c.items.find(se.id);
            if (iit == c.items.end()) p.add(w + ": stock lists unknown item '" + se.id + "'");
            else for (int t : se.tiers) if (!iit->second.FindTier(t)) p.add(w + ": item '" + se.id + "' has no tier " + std::to_string(t));
            d.stock.push_back(se);
        }
        for (auto& s : r.value("capacityItemStock", json::array())) {
            std::string cid = Str(s, "id");
            int64_t price = Num<int64_t>(s, "price", 0);
            if (!c.capacityItems.count(cid)) p.add(w + ": capacityItemStock lists unknown capacity item '" + cid + "'");
            if (price < 1) p.add(w + ": capacityItemStock '" + cid + "' needs a positive price");
            d.capacityItemStock.push_back({ cid, price });
        }
        for (auto& s : r.value("modStock", json::array())) {
            StockEntry se{ Str(s, "mod"), {} };
            for (auto& t : s.value("tiers", json::array())) if (t.is_number_integer()) se.tiers.push_back(t.get<int>());
            auto mit = c.mods.find(se.id);
            if (mit == c.mods.end()) p.add(w + ": modStock lists unknown mod '" + se.id + "'");
            else for (int t : se.tiers) if (!mit->second.FindTier(t)) p.add(w + ": mod '" + se.id + "' has no tier " + std::to_string(t));
            d.modStock.push_back(se);
        }
        if (c.ripperdocs.count(d.id)) p.add(w + ": defined twice");
        c.ripperdocs[d.id] = d;
        c.ripperdocOrder.push_back(d.id);
    }

    if (!p.list.empty()) {
        std::ostringstream os;
        os << p.list.size() << " catalog problem(s):";
        for (auto& s : p.list) os << "\n  - " << s;
        throw CwError("E_CATALOG", os.str());
    }
    return c;
}

// ---------------------------------------------------------------- State <-> JSON
namespace {
json RollsToJson(const std::vector<Roll>& v)
{
    json a = json::array();
    for (auto& r : v) a.push_back({ { "stat", r.stat }, { "value", r.value } });
    return a;
}
std::vector<Roll> RollsFromJson(const json& a)
{
    std::vector<Roll> v;
    if (a.is_array()) for (auto& r : a) v.push_back({ r.value("stat", ""), r.value("value", 0.0) });
    return v;
}
}  // namespace

json State::ToJson() const
{
    json j;
    j["version"] = version;
    j["rng"] = std::to_string(rng);  // 64-bit as string: JSON numbers above 2^53 do not survive every reader
    j["nextUid"] = nextUid;
    json inst = json::array();
    for (auto& [uid, i] : instances)
        inst.push_back({ { "uid", i.uid }, { "item", i.item }, { "tier", i.tier }, { "variant", i.variant },
                         { "rolls", RollsToJson(i.rolls) }, { "mods", i.mods }, { "slot", i.slot }, { "slotIndex", i.slotIndex } });
    for (auto& o : inst) {
        const Instance& i = instances.at(o["uid"].get<std::string>());
        if (i.hasPending) o["pendingRolls"] = RollsToJson(i.pendingRolls);
    }
    j["instances"] = inst;
    json mods = json::array();
    for (auto& [uid, m] : modInstances)
        mods.push_back({ { "uid", m.uid }, { "mod", m.mod }, { "tier", m.tier }, { "rolls", RollsToJson(m.rolls) }, { "attachedTo", m.attachedTo } });
    j["modInstances"] = mods;
    j["perkRanks"] = perkRanks;
    j["bonusPerkPoints"] = bonusPerkPoints;
    j["capacityItemsUsed"] = capacityItemsUsed;
    j["orphans"] = orphans;
    j["settings"] = settings.ToJson();
    if (!customize.empty()) j["customize"] = customize;
    // D1: only what the player chose, as customize is. An absent key is "no choice made" and loads as the
    // built-in default order, so a save made before this change needs no migration.
    if (!loadout.empty()) j["loadout"] = loadout;
    if (loadoutUse >= 0) j["loadoutUse"] = loadoutUse;
    if (!hackSelected.empty()) j["hackSelected"] = hackSelected;
    return j;
}

State State::FromJson(const json& j)
{
    State s;
    s.version = j.value("version", kStateVersion);
    if (j.contains("rng")) {
        if (j["rng"].is_string()) s.rng = std::stoull(j["rng"].get<std::string>());
        else if (j["rng"].is_number_unsigned()) s.rng = j["rng"].get<uint64_t>();
    }
    s.nextUid = j.value("nextUid", (uint64_t)1);
    for (auto& i : j.value("instances", json::array())) {
        Instance x;
        x.uid = i.value("uid", ""); x.item = i.value("item", ""); x.tier = i.value("tier", 1); x.variant = i.value("variant", "");
        x.rolls = RollsFromJson(i.value("rolls", json::array()));
        if (i.contains("mods") && i["mods"].is_array()) for (auto& m : i["mods"]) x.mods.push_back(m.is_string() ? m.get<std::string>() : "");
        x.slot = i.value("slot", ""); x.slotIndex = i.value("slotIndex", -1);
        if (i.contains("pendingRolls")) { x.hasPending = true; x.pendingRolls = RollsFromJson(i["pendingRolls"]); }
        if (!x.uid.empty()) s.instances[x.uid] = x;
    }
    for (auto& m : j.value("modInstances", json::array())) {
        ModInstance x;
        x.uid = m.value("uid", ""); x.mod = m.value("mod", ""); x.tier = m.value("tier", 1);
        x.rolls = RollsFromJson(m.value("rolls", json::array())); x.attachedTo = m.value("attachedTo", "");
        if (!x.uid.empty()) s.modInstances[x.uid] = x;
    }
    if (j.contains("perkRanks") && j["perkRanks"].is_object())
        for (auto& [k, v] : j["perkRanks"].items()) if (v.is_number_integer()) s.perkRanks[k] = v.get<int>();
    s.bonusPerkPoints = j.value("bonusPerkPoints", 0);
    if (j.contains("capacityItemsUsed") && j["capacityItemsUsed"].is_object())
        for (auto& [k, v] : j["capacityItemsUsed"].items()) if (v.is_number_integer()) s.capacityItemsUsed[k] = v.get<int>();
    if (j.contains("orphans") && j["orphans"].is_array()) s.orphans = j["orphans"];
    if (j.contains("settings")) s.settings.MergeFromJson(j["settings"], /*strict=*/false);
    if (j.contains("customize") && j["customize"].is_object()) s.customize = j["customize"];
    // D1, tolerant by design ("nothing in the save you cannot drop safely on load"): anything that is not a list of
    // distinct non-empty strings is dropped, the list is capped, and a bad loadoutUse falls back to -1 (= all).
    if (j.contains("loadout") && j["loadout"].is_array())
        for (auto& e : j["loadout"]) {
            if (!e.is_string()) continue;
            const std::string id = e.get<std::string>();
            if (id.empty() || s.loadout.size() >= 32) continue;
            if (std::find(s.loadout.begin(), s.loadout.end(), id) == s.loadout.end()) s.loadout.push_back(id);
        }
    if (j.contains("loadoutUse") && j["loadoutUse"].is_number_integer()) {
        const int u = j["loadoutUse"].get<int>();
        if (u >= 0 && u <= 32) s.loadoutUse = u;
    }
    if (j.contains("hackSelected") && j["hackSelected"].is_string()) s.hackSelected = j["hackSelected"].get<std::string>();
    return s;
}

}  // namespace cw
