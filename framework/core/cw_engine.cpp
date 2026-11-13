// cw_engine.cpp - the stat engine and every state-changing operation.
//
// Model (docs/DESIGN-FRAMEWORK.md): the player owns Instances (a piece at a tier, with a variant, rolled stats and
// attached ModInstances). Installed instances contribute stats; cyber perks (bought, or auto-granted from a vanilla FO4
// perk) scale them, add flat stats, capacity, slots and abilities. The engine turns the effective totals into a SET of
// Cyberware.esp perks (each stat is bit-encoded: perk P<b> adds step*2^b, N<b> subtracts) plus ability perks, and the
// plugin makes the player's perk list match that set. The game never holds state the engine cannot recompute.
#include "cw_core.h"

#include <cstring>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace cw {

namespace {
const char* Bad = "E_BAD_ARGS";

std::string ArgStr(const json& a, const char* k, bool required = true)
{
    auto it = a.find(k);
    if (it == a.end() || !it->is_string() || it->get<std::string>().empty()) {
        if (required) throw CwError(Bad, std::string("missing string argument '") + k + "'");
        return "";
    }
    return it->get<std::string>();
}
int ArgInt(const json& a, const char* k, std::optional<int> def = std::nullopt)
{
    auto it = a.find(k);
    if (it == a.end() || !it->is_number_integer()) {
        if (!def) throw CwError(Bad, std::string("missing integer argument '") + k + "'");
        return *def;
    }
    return it->get<int>();
}
bool ArgBool(const json& a, const char* k, bool def = false)
{
    auto it = a.find(k);
    return (it != a.end() && it->is_boolean()) ? it->get<bool>() : def;
}
void AddStats(StatMap& into, const StatMap& from, double mul = 1.0)
{
    for (auto& [k, v] : from) into[k] += v * mul;
}
void AddRolls(StatMap& into, const std::vector<Roll>& rolls)
{
    for (auto& r : rolls) into[r.stat] += r.value;
}
json StatsJson(const StatMap& m)
{
    json o = json::object();
    for (auto& [k, v] : m) if (std::fabs(v) > 1e-9) o[k] = std::round(v * 100.0) / 100.0;
    return o;
}
json RollsJson(const std::vector<Roll>& rolls)
{
    json a = json::array();
    for (auto& r : rolls) a.push_back({ { "stat", r.stat }, { "value", r.value } });
    return a;
}
}  // namespace

Engine::Engine(Catalog cat, Env* env) : cat_(std::move(cat)), env_(env) { NewGame(); }

void Engine::NewGame()
{
    timers_.clear();
    tierDeltas_.clear();   // COMPUTE: a revert must never leave the previous game's tier deltas resolved
    state_ = State{};
    state_.rng = env_ ? env_->Entropy() : 0x9E3779B97F4A7C15ull;
    if (state_.rng == 0) state_.rng = 0x9E3779B97F4A7C15ull;
}

void Engine::LoadState(const json& j)
{
    // COMPUTE: drop them before anything else, so a load can only ever apply what the NEW state re-resolves in Sync().
    // The plugin does call Sync() after a load, but failing to NOTHING is the safe direction for a bonus, and this is
    // the only way a forgotten Sync cannot hand the player the previous save's delta (the lead's stale-delta ask).
    tierDeltas_.clear();
    State s = State::FromJson(j);
    if (s.rng == 0) s.rng = env_ ? env_->Entropy() | 1 : 0x9E3779B97F4A7C15ull;
    // Drop records whose catalog entry vanished (catalog edited between saves) into orphans, and repair links.
    for (auto it = s.instances.begin(); it != s.instances.end();) {
        auto& in = it->second;
        auto def = cat_.items.find(in.item);
        if (def == cat_.items.end() || !def->second.FindTier(in.tier)) {
            if (env_) env_->Log("load: instance " + in.uid + " (" + in.item + " tier " + std::to_string(in.tier) + ") is not in the catalog - kept as orphan");
            s.orphans.push_back({ { "kind", "instance" }, { "item", in.item }, { "tier", in.tier }, { "uid", in.uid } });
            it = s.instances.erase(it);
            continue;
        }
        if (!def->second.FindVariant(in.variant)) in.variant = def->second.variants.front().id;
        if (!in.slot.empty() && !cat_.FindSlot(in.slot)) { in.slot.clear(); in.slotIndex = -1; }
        ++it;
    }
    for (auto it = s.modInstances.begin(); it != s.modInstances.end();) {
        auto def = cat_.mods.find(it->second.mod);
        if (def == cat_.mods.end() || !def->second.FindTier(it->second.tier)) {
            s.orphans.push_back({ { "kind", "mod" }, { "mod", it->second.mod }, { "tier", it->second.tier }, { "uid", it->second.uid } });
            it = s.modInstances.erase(it);
            continue;
        }
        if (!it->second.attachedTo.empty() && !s.instances.count(it->second.attachedTo)) it->second.attachedTo.clear();
        ++it;
    }
    for (auto& [uid, in] : s.instances)
        for (auto& m : in.mods)
            if (!m.empty() && (!s.modInstances.count(m) || s.modInstances[m].attachedTo != uid)) m.clear();
    for (auto it = s.perkRanks.begin(); it != s.perkRanks.end();) {
        auto def = cat_.perks.find(it->first);
        if (def == cat_.perks.end() || def->second.autoGrant || it->second <= 0) { it = s.perkRanks.erase(it); continue; }
        it->second = std::min(it->second, def->second.maxRank);
        ++it;
    }
    // If the catalog tightened SLOT counts, uninstall from the end until the state is valid again.
    // Capacity is exempt (the lead, 2026-11-08): a save over the new limit keeps everything and
    // pays the over-cap penalty; only further installs are blocked (prevUsed grandfather).
    for (int guard = 0; guard < 64; ++guard) {
        Derived d = Compute(s);
        try { ValidateInvariants(s, d, d.capacityUsed, 0); break; }
        catch (const CwError& e) {
            Instance* victim = nullptr;
            for (auto& [uid, in] : s.instances) if (!in.slot.empty()) victim = &in;
            if (!victim) break;
            if (env_) env_->Log("load: " + e.code + " " + e.what() + " - uninstalling " + victim->uid + " to the stash");
            victim->slot.clear(); victim->slotIndex = -1;
        }
    }
    state_ = std::move(s);
    timers_.clear();
}

uint64_t Engine::NextRand(State& s)
{
    uint64_t z = (s.rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::string Engine::NewUid(State& s, char prefix) const
{
    return std::string(1, prefix) + std::to_string(s.nextUid++);
}

std::vector<Roll> Engine::RollGroups(State& s, const std::vector<RollGroup>& groups, int extra) const
{
    std::vector<Roll> out;
    for (auto& g : groups) {
        std::vector<RollOption> pool = g.pool;
        int count = std::min<int>(g.count + std::max(0, extra), (int)pool.size());  // extraRolls, capped at the pool
        for (int n = 0; n < count && !pool.empty(); ++n) {
            double total = 0;
            for (auto& o : pool) total += o.weight;
            double pick = (NextRand(s) >> 11) * (1.0 / 9007199254740992.0) * total;
            size_t idx = 0;
            for (; idx + 1 < pool.size(); ++idx) { if (pick < pool[idx].weight) break; pick -= pool[idx].weight; }
            const RollOption& o = pool[idx];
            double v = o.min;
            if (o.max > o.min) {
                // Whole numbers when both bounds are whole (the common case), else 2 decimals.
                bool whole = std::floor(o.min) == o.min && std::floor(o.max) == o.max;
                double u = (NextRand(s) >> 11) * (1.0 / 9007199254740992.0);
                v = whole ? o.min + std::floor(u * (o.max - o.min + 1)) : std::round((o.min + u * (o.max - o.min)) * 100.0) / 100.0;
                if (v > o.max) v = o.max;
            }
            out.push_back({ o.stat, v });
            pool.erase(pool.begin() + idx);
        }
    }
    return out;
}

Derived Engine::Compute(const State& s) const
{
    Derived d;
    d.level = env_ ? env_->GetLevel() : 1;
    d.settings = s.settings;

    // Effective cyber perk ranks.
    for (auto& id : cat_.perkOrder) {
        const auto& p = cat_.perks.at(id);
        int rank = 0;
        if (p.autoGrant) rank = env_ ? std::min(env_->GetFo4PerkRank(p.fo4Perk), p.maxRank) : 0;
        else if (auto it = s.perkRanks.find(id); it != s.perkRanks.end()) rank = std::min(it->second, p.maxRank);
        if (rank <= 0) continue;
        d.perkRanksEffective[id] = rank;
        if (!p.autoGrant) d.perkPointsSpent += rank * p.cost;
        for (auto& e : p.effects) d.activeEffects.push_back({ &e, rank });
    }
    d.perkPointsTotal = cat_.ppStart + (cat_.ppPerLevels > 0 ? d.level / cat_.ppPerLevels : 0) + s.bonusPerkPoints;

    for (auto& sl : cat_.slots) d.slotCounts[sl.id] = sl.count;
    int capBase = std::min(cat_.capBase + cat_.capPerLevel * d.level, cat_.capMax);
    for (auto& [e, r] : d.activeEffects) {
        if (e->type == "slotCount") d.slotCounts[e->slot] += (int)std::lround(e->add * r);
        else if (e->type == "capacity") d.capacityBonus += (int)std::lround(e->add * r);
        else if (e->type == "capacityPerAV")
            d.capacityBonus += (int)std::lround(e->add * r * (env_ ? (e->base ? env_->GetBaseActorValue(e->av) : env_->GetActorValue(e->av)) : 0.0));
        else if (e->type == "ability") { d.abilities.insert(e->ability); d.abilityTier[e->ability] = std::max(d.abilityTier[e->ability], 1); }
        else if (e->type == "extraRolls") d.extraRolls += (int)std::lround(e->add * r);
        else if (e->type == "rerollCostMult") d.rerollCostMult += e->mult * r;
        else if (e->type == "rerollKeepOld") d.rerollKeepOld = true;
        else if (e->type == "overCapacity" && e->maxPct * r > d.overCapMaxPct) {
            d.overCapMaxPct = e->maxPct * r; d.overCapHealthPct = e->healthPctPerPoint; d.overCapStat = e->stat;
        }
    }
    for (auto& [k, v] : d.slotCounts) v = s.settings.slotLimits ? std::clamp(v, 0, 16) : 16;
    for (auto& pa : cat_.capPerAV) {  // always on, from the BASE value; min>0 = threshold (Renaissance Punk shape)
        const double v = env_ ? env_->GetBaseActorValue(pa.av) : 0.0;
        if (pa.min > 0) d.capacityBonus += v >= pa.min ? (int)std::lround(pa.add) : 0;
        else d.capacityBonus += (int)std::lround(pa.add * v);
    }
    for (auto& [id, used] : s.capacityItemsUsed)
        if (auto ci = cat_.capacityItems.find(id); ci != cat_.capacityItems.end())
            d.capacityBonus += ci->second.add * std::min(used, ci->second.max);
    for (auto& [uid, in] : s.instances)
        if (!in.slot.empty())
            if (const TierDef* t = cat_.items.at(in.item).FindTier(in.tier)) d.capacityBonus += t->capacityGrant;
    d.capacityMax = capBase + d.capacityBonus;
    if (s.settings.capacityMult != 1.0) d.capacityMax = (int)std::lround(d.capacityMax * s.settings.capacityMult);
    if (!s.settings.overCapacity) d.overCapMaxPct = 0;   // Settings: Edgerunner over-capacity off
    d.capacityHardMax = d.overCapMaxPct > 0 ? (int)std::floor(d.capacityMax * (1.0 + d.overCapMaxPct / 100.0)) : d.capacityMax;
    if (!s.settings.capacityEnabled) d.capacityMax = d.capacityHardMax = 999999;  // Settings: capacity off

    // Per-instance stats and capacity cost.
    for (auto& [uid, in] : s.instances) {
        const ItemDef& it = cat_.items.at(in.item);
        const TierDef* t = it.FindTier(in.tier);
        StatMap st;
        AddStats(st, t->stats);
        AddRolls(st, in.rolls);
        int modCost = 0;
        for (auto& mu : in.mods) {
            if (mu.empty()) continue;
            auto mi = s.modInstances.find(mu);
            if (mi == s.modInstances.end()) continue;
            const TierDef* mt = cat_.mods.at(mi->second.mod).FindTier(mi->second.tier);
            AddStats(st, mt->stats);
            AddRolls(st, mi->second.rolls);
            modCost += mt->capacityCost;
            if (!in.slot.empty()) for (auto& a : mt->abilities) { d.abilities.insert(a); d.abilityTier[a] = std::max(d.abilityTier[a], mt->tier); }
        }
        double costMult = 1.0;  // capacityCostMult and statMult stack additively: two +10% = +20%
        for (auto& [e, r] : d.activeEffects)
            if (e->type == "capacityCostMult" && e->scope.Matches(it)) costMult += e->mult * r;
        std::map<std::string, double> mults;
        for (auto& [e, r] : d.activeEffects)
            if (e->type == "statMult" && e->scope.Matches(it)) mults[e->stat] += e->mult * r;
        for (auto& [stat, m] : mults) if (st.count(stat)) st[stat] *= (1.0 + m);
        if (!in.slot.empty())
            for (auto& [e, r] : d.activeEffects)
                if (e->type == "statPerPiece" && e->scope.Matches(it)) st[e->stat] += e->add * r;
        int cost = (int)std::lround(t->capacityCost * std::max(0.0, costMult)) + modCost;
        d.perInstance[uid] = st;
        d.instanceCost[uid] = cost;
        if (!in.slot.empty()) {
            AddStats(d.totals, st);
            d.capacityUsed += cost;
            for (auto& a : t->abilities) { d.abilities.insert(a); d.abilityTier[a] = std::max(d.abilityTier[a], t->tier); }
            if (it.iconic) for (auto& a : it.iconicAbilities) { d.abilities.insert(a); d.abilityTier[a] = std::max(d.abilityTier[a], t->tier); }
            if (const VariantDef* v = it.FindVariant(in.variant); v && !v->armo.empty()) d.desired.visuals.push_back({ uid, v->armo });
        }
    }
    for (auto& [e, r] : d.activeEffects)
        if (e->type == "stat") d.totals[e->stat] += e->add * r;
    // Over-capacity penalty: each point over max costs a % of the penalty stat's BASE value.
    // With Edgerunner this is the perk's numbers; WITHOUT it the CP default (-0.5% max health per
    // point) still applies, so a save grandfathered over the limit (a curve change, a removed
    // perk) keeps its chrome and pays the CP price instead of being auto-stripped (the lead,
    // 2026-11-08). Installs beyond hardMax stay blocked; hardMax > max only with the perk.
    if (d.capacityUsed > d.capacityMax && s.settings.capacityEnabled) {
        const bool edging = d.overCapMaxPct > 0;
        const std::string stat = edging ? d.overCapStat : "health";
        const double pctPerPoint = edging ? d.overCapHealthPct : 0.5;
        const StatDef* ps = cat_.FindStat(stat);
        double base = (ps && env_) ? env_->GetBaseActorValue(ps->av) : 0.0;
        d.overCapPenalty = (d.capacityUsed - d.capacityMax) * pctPerPoint / 100.0 * base;
        d.overCapStat = stat;
        d.totals[stat] -= d.overCapPenalty;
    }

    // Quantize to the bit-encoded perks.
    for (auto& sd : cat_.stats) {
        double v = d.totals.count(sd.id) ? d.totals[sd.id] : 0.0;
        long long steps = std::llround(v / sd.step);
        long long maxPos = (1ll << sd.bits) - 1, maxNeg = sd.negBits ? (1ll << sd.negBits) - 1 : 0;
        if (steps > maxPos) steps = maxPos;
        if (steps < -maxNeg) steps = -maxNeg;
        if (steps != 0) d.applied[sd.id] = steps * sd.step;
        long long mag = steps < 0 ? -steps : steps;
        const char* sign = steps < 0 ? "N" : "P";
        for (int b = 0; b < (steps < 0 ? sd.negBits : sd.bits); ++b)
            if (mag & (1ll << b)) d.desired.perkEditorIds.insert("CW_Stat_" + sd.id + "_" + sign + std::to_string(b));
    }
    for (auto& a : d.abilities) {
        auto it = cat_.abilities.find(a);
        if (it == cat_.abilities.end()) continue;
        d.desired.perkEditorIds.insert(it->second.perk.empty() ? "CW_Ab_" + a : "@" + it->second.perk);
        const json& nat = it->second.raw.contains("spec") && it->second.raw["spec"].is_object() ? it->second.raw["spec"].value("native", json()) : json();
        if (nat.is_object() && nat.contains("handler")) {
            NativeAbility na;
            na.ability = a;
            na.handler = nat.value("handler", "");
            na.activation = nat.value("activation", "");
            na.tier = std::max(1, d.abilityTier.count(a) ? d.abilityTier.at(a) : 1);
            na.params = NativeParamsFor(nat, na.tier);
            // Passive natives: the ability's CW_Nat perk (AV + perk-entry effects) is on for as long as it is installed.
            NativeFamily fam = FamilyOf(na.handler, na.activation, na.params);
            if (!PerkIsTimed(fam) && !EffectsFor(na.handler, fam, na.params).Empty()) d.desired.perkEditorIds.insert(TimedPerkId(a, na.tier));
            d.desired.native.push_back(na);
        }
    }
    // The Chrome Crit / mitigation perk reads the custom crit AVs (default 0): on while any cyberware is installed.
    if (const StatDef* cc = cat_.FindStat("critChance"); cc && cc->av == "CW_CritChance")
        for (auto& [uid, in] : s.instances)
            if (!in.slot.empty()) { d.desired.perkEditorIds.insert(kChromeCritPerk); break; }
    // Timed native effects that are running now (only while the ability that started them is still installed).
    const int64_t now = env_ ? env_->NowMs() : 0;
    for (auto& [ab, t] : timers_)
        if (t.activeUntil > now && (d.abilities.count(ab) || ab.rfind("qh_", 0) == 0) && !t.perk.empty()) d.desired.perkEditorIds.insert(t.perk);
    return d;
}

json NativeParamsFor(const json& nat, int tier)
{
    if (!nat.is_object()) return json::object();
    std::string key = "paramsT" + std::to_string(tier);
    json p = nat.contains(key)        ? nat[key]
             : nat.contains("paramsT5") ? (nat.value("scaleWithTier", false) ? ScaleNativeParams(nat["paramsT5"], tier) : nat["paramsT5"])
                                        : json::object();
    // statsHandled params are already the item's STATS (to_schema AV_PARAMS -> tier stats): no
    // consumer of the native params — the CW_Nat perk baker NOR a plugin runtime handler — may
    // apply them again (DESIGN; qaaudit 2026-11-09: Rara Avis stacked Health_pct's runtime ledger
    // on top of its health stat, Smart Link its crit damage).
    if (p.is_object())
        for (auto& k : nat.value("statsHandled", json::array()))
            if (k.is_string()) p.erase(k.get<std::string>());
    return p;
}

json ScaleNativeParams(const json& p5, int tier)
{
    // Benefits scale by tier/5. Lower-is-better values must not shrink at low tiers: time_scale moves toward 1
    // (no slowdown) and cooldowns grow by 25% per tier below 5. Counts, flags and strings are kept as written.
    if (!p5.is_object()) return p5;
    json p = p5;
    double f = std::clamp(tier, 1, 5) / 5.0;
    for (auto& [k, v] : p.items()) {
        if (!v.is_number() || v.is_boolean()) continue;
        double x = v.get<double>();
        // charges / recharge_s: CP's PLS is 1 charge, 25 s at every tier (TweakDB), so neither scales
        bool count = k.find("slots") != std::string::npos || k == "targets" || k == "zoom" || k == "buffer" || k == "ram_max" || k == "charges" || k == "recharge_s";
        if (count) continue;
        auto ends = [&](const char* suf) { std::size_t n = std::strlen(suf); return k.size() >= n && k.compare(k.size() - n, n, suf) == 0; };
        if (k == "time_scale") v = 1.0 - (1.0 - x) * f;
        else if (ends("cooldown_s")) v = x * (1.0 + (5 - std::clamp(tier, 1, 5)) * 0.25);   // lower is better: grows below T5
        else v = x * f;
    }
    return p;
}

void Engine::ValidateInvariants(const State& s, const Derived& d, int prevUsed, int prevHardMax) const
{
    const bool grandfathered = prevUsed >= 0 && d.capacityUsed <= prevUsed && d.capacityHardMax >= prevHardMax;
    if (d.capacityUsed > d.capacityHardMax && !grandfathered)
        throw CwError("E_CAPACITY", "cyberware capacity " + std::to_string(d.capacityUsed) + "/" + std::to_string(d.capacityMax) +
                                        (d.capacityHardMax > d.capacityMax ? " (over-capacity limit " + std::to_string(d.capacityHardMax) + ")" : "") + " exceeded");
    std::map<std::string, std::set<int>> used;
    std::map<std::string, std::string> installedItems;
    for (auto& [uid, in] : s.instances) {
        if (in.slot.empty()) continue;
        int cnt = d.slotCounts.count(in.slot) ? d.slotCounts.at(in.slot) : 0;
        if (in.slotIndex < 0 || in.slotIndex >= cnt)
            throw CwError("E_SLOT_FULL", "slot " + in.slot + " has " + std::to_string(cnt) + " position(s); " + uid + " is in position " + std::to_string(in.slotIndex));
        if (!used[in.slot].insert(in.slotIndex).second)
            throw CwError("E_SLOT_FULL", "slot " + in.slot + " position " + std::to_string(in.slotIndex) + " holds two pieces");
        const ItemDef& it = cat_.items.at(in.item);
        if (!it.raw.value("allowDuplicate", false)) {
            auto [p, fresh] = installedItems.insert({ in.item, uid });
            if (!fresh) throw CwError("E_DUPLICATE", it.name + " is already installed (" + p->second + ")");
        }
    }
    if (d.perkPointsSpent > d.perkPointsTotal)
        throw CwError("E_PERK_POINTS", "cyber perk points " + std::to_string(d.perkPointsSpent) + "/" + std::to_string(d.perkPointsTotal) + " overspent");
}

const RipperdocDef* Engine::OpenDoc() const
{
    auto it = cat_.ripperdocs.find(currentRipperdoc);
    return it == cat_.ripperdocs.end() ? nullptr : &it->second;
}

void Engine::RequireDoc() const
{
    if (!OpenDoc() && !(env_ && env_->DebugEnabled()))
        throw CwError("E_NO_RIPPERDOC", "this needs a ripperdoc (no ripperdoc menu is open)");
}

bool Engine::InStock(const std::vector<StockEntry>& list, const std::string& id, int tier) const
{
    for (auto& e : list) if (e.id == id && std::find(e.tiers.begin(), e.tiers.end(), tier) != e.tiers.end()) return true;
    return false;
}

int64_t Engine::Price(int64_t base, const ItemDef* scopeItem, const Derived& d) const
{
    double m = 1.0;
    if (const RipperdocDef* doc = OpenDoc()) m *= doc->priceMult;
    double pm = 1.0;
    for (auto& [e, r] : d.activeEffects)
        if (e->type == "costMult" && (!scopeItem || e->scope.Matches(*scopeItem))) pm += e->mult * r;
    m *= std::max(0.0, pm);
    m *= std::max(0.0, d.settings.priceMult);                      // Settings: price multiplier
    if (d.settings.levelPricing) m *= LevelPriceMult(d.level);     // Settings: level-based pricing
    return (int64_t)std::llround((double)base * m);
}

int64_t Engine::SellPrice(int64_t base, const Derived& d) const
{
    double sm = cat_.sellMult;
    if (const RipperdocDef* doc = OpenDoc(); doc && doc->sellMult) sm = *doc->sellMult;
    // The same global multipliers as Price, so sell-back stays proportional to what was paid.
    sm *= std::max(0.0, d.settings.priceMult);
    if (d.settings.levelPricing) sm *= LevelPriceMult(d.level);
    return (int64_t)std::llround((double)base * sm);
}

// COMPUTE (track A, the lead, 2026-11-10): what the DLL must apply right now for installed computed abilities
// above the perk tier clamp. The numbers come from TierDeltaAvs, i.e. from the catalog's own explicit paramsT<t>; this
// function only decides WHICH of them hold at this instant, because the clamp tier's perk carries its own conditions in
// the record and the delta must be conditioned the same way (Threatevac's +speed applies only below 25% HP).
std::map<std::string, double> Engine::TierDeltaAvNow() const
{
    std::map<std::string, double> out;
    if (!env_) return out;
    const int64_t nowMs = env_->NowMs();
    for (auto& td : tierDeltas_) {
        const NatAvFx& f = td.fx;
        {
            bool holds = true;
            // A timed ability's perk is on the player only while its timer runs (Adreno Trigger's 10 s, a Sandevistan,
            // Jenkins' 5 s burst). Without this a burst tier's delta would be a permanent bonus.
            if (td.timed) {
                auto it = timers_.find(td.ability);
                holds = it != timers_.end() && it->second.activeUntil > nowMs;
            }
            for (auto& w : f.when) {
                if (w.fn == "hp_below") holds = holds && env_->ActorValuePct("Health") < w.value * 100.0;
                else if (w.fn == "hp_above") holds = holds && env_->ActorValuePct("Health") > w.value * 100.0;
                else if (w.fn == "ap_above") holds = holds && env_->ActorValuePct("ActionPoints") > w.value * 100.0;
                else if (w.fn == "sneaking" || w.fn == "combat" || w.fn == "sprinting" || w.fn == "aiming" || w.fn == "airborne") holds = holds && env_->PlayerIs(w.fn);
                else holds = false;   // only the engine can evaluate it (a chance roll, a power attack, a distance band)
            }
            if (holds) out[f.av] += f.value;
        }
    }
    for (auto it = out.begin(); it != out.end();) {
        if (std::fabs(it->second) < 1e-9) it = out.erase(it);
        else ++it;
    }
    return out;
}

// Jenkin's Tendons' second band. Its first band is a 5 s burst on sprint_start, carried by the TIMED perk, so the
// second band is "while sprinting, once that burst has ended". A perk condition can see sprinting but not "5 s into the
// sprint", and a second perk record would cost an id for one number - so the DLL holds it, like Atomic Sensors.
// Both numbers come from the catalog's own speed_pct bands; nothing here decides a value.
std::map<std::string, double> Engine::SecondBandAvNow() const
{
    std::map<std::string, double> out;
    if (!env_ || !env_->PlayerIs("sprinting")) return out;
    const int64_t now = env_->NowMs();
    const Derived d = Compute(state_);
    for (auto& na : d.desired.native) {
        if (na.handler != "le_jenkins_tendons") continue;
        auto it = timers_.find(na.ability);
        if (it != timers_.end() && it->second.activeUntil > now) continue;   // the burst is running: its perk carries it
        if (!na.params.contains("speed_pct") || !na.params["speed_pct"].is_array() || na.params["speed_pct"].size() < 2) continue;
        const json& b = na.params["speed_pct"][1];
        if (b.is_number() && !b.is_boolean() && b.get<double>() != 0) out["SpeedMult"] += b.get<double>();
    }
    return out;
}

void Engine::Sync()
{
    if (!env_) return;
    Derived d = Compute(state_);
    // COMPUTE (track A): resolve the actor-value deltas for installed computed abilities above the perk tier clamp.
    // Done here, not per tick, because only a commit or a load can change which abilities are installed and at what
    // tier. TierDeltaAvNow() then evaluates just these few conditions each frame.
    tierDeltas_.clear();
    if (!ComputedAbilities().empty())
        for (auto& na : d.desired.native) {
            if (!ComputedAbilities().count(na.ability)) continue;
            const bool timed = PerkIsTimed(FamilyOf(na.handler, na.activation, na.params));
            for (auto& f : TierDeltaAvs(cat_, na.ability, na.tier)) tierDeltas_.push_back({ na.ability, timed, f });
        }
    env_->ApplyDesired(d.desired);
}

int Engine::RedeemChips()
{
    if (!env_) return 0;
    auto found = env_->TakeChips();
    int n = 0;
    for (auto& c : found) {
        for (int i = 0; i < c.count; ++i) {
            if (c.kind == "item") {
                auto it = cat_.items.find(c.id);
                if (it == cat_.items.end()) continue;
                const TierDef* t = it->second.chipTier ? it->second.FindTier(*it->second.chipTier) : &it->second.tiers.front();
                Instance in;
                in.uid = NewUid(state_, 'c'); in.item = c.id; in.tier = t->tier; in.variant = it->second.variants.front().id;
                in.rolls = RollGroups(state_, t->rolls, Compute(state_).extraRolls);
                in.mods.assign(t->modSlots, "");
                state_.instances[in.uid] = in;
                ++n;
            } else if (c.kind == "mod") {
                auto mt = cat_.mods.find(c.id);
                if (mt == cat_.mods.end()) continue;
                const TierDef& t = mt->second.tiers.front();
                ModInstance m;
                m.uid = NewUid(state_, 'm'); m.mod = c.id; m.tier = t.tier; m.rolls = RollGroups(state_, t.rolls, Compute(state_).extraRolls);
                state_.modInstances[m.uid] = m;
                ++n;
            }
        }
    }
    if (n) { env_->Log("redeemed " + std::to_string(n) + " cyberware chip(s) into the stash"); Sync(); }
    return n;
}

int Engine::RedeemCapacityItems()
{
    if (!env_) return 0;
    int n = 0;
    for (auto& id : cat_.capacityItemOrder) {
        const CapacityItemDef& ci = cat_.capacityItems.at(id);
        int room = ci.max - state_.capacityItemsUsed[id];
        int have = room > 0 ? env_->CountItem(ci.ItemKey()) : 0;
        int take = std::min(room, have);
        if (take > 0 && env_->ConsumeItem(ci.ItemKey(), take)) { state_.capacityItemsUsed[id] += take; n += take; }
    }
    if (n) { env_->Log("used " + std::to_string(n) + " capacity item(s)"); Sync(); }
    return n;
}

json Engine::Run(const std::string& op, const json& args, bool dryRun)
{
    State s = state_;
    int64_t delta = 0;  // currency change, applied at commit
    std::vector<std::pair<std::string, int>> consumeAtCommit;   // upgrade components (forms), taken at commit
    json data = json::object();
    Derived d0 = Compute(s);
    const int64_t cash = env_ ? env_->GetCurrency() : 0;
    auto pay = [&](int64_t price) {
        if (cash + delta < price) throw CwError("E_NO_FUNDS", "needs " + std::to_string(price) + " " + cat_.currencyName + ", have " + std::to_string(cash + delta));
        delta -= price;
    };
    auto inst = [&](const std::string& uid) -> Instance& {
        auto it = s.instances.find(uid);
        if (it == s.instances.end()) throw CwError("E_NOT_OWNED", "no owned cyberware with uid '" + uid + "'");
        if (it->second.hasPending && op != "rerollResolve")
            throw CwError("E_PENDING_REROLL", uid + " has a reroll waiting: send rerollResolve first");
        return it->second;
    };
    const int extra = d0.extraRolls;
    std::vector<std::pair<const GearDef*, int>> gives;   // ARMORY gear handed over at commit, after the caps are taken
    auto modi = [&](const std::string& uid) -> ModInstance& {
        auto it = s.modInstances.find(uid);
        if (it == s.modInstances.end()) throw CwError("E_NOT_OWNED", "no owned mod with uid '" + uid + "'");
        return it->second;
    };
    auto install = [&](Instance& in, int index, bool replace) {
        const ItemDef& it = cat_.items.at(in.item);
        Derived d = Compute(s);
        int cnt = d.slotCounts[it.slot];
        if (index < 0) {
            std::set<int> taken;
            for (auto& [u, o] : s.instances) if (o.slot == it.slot && u != in.uid) taken.insert(o.slotIndex);
            for (int i = 0; i < cnt; ++i) if (!taken.count(i)) { index = i; break; }
            if (index < 0) {
                if (!replace) throw CwError("E_SLOT_FULL", "every " + cat_.FindSlot(it.slot)->name + " position is taken");
                index = 0;
            }
        }
        if (index >= cnt) throw CwError("E_SLOT_FULL", "slot " + it.slot + " has no position " + std::to_string(index));
        for (auto& [u, o] : s.instances)
            if (u != in.uid && o.slot == it.slot && o.slotIndex == index) {
                if (!replace) throw CwError("E_SLOT_FULL", cat_.FindSlot(it.slot)->name + " position " + std::to_string(index) + " holds " + u + " (pass replace:true)");
                o.slot.clear(); o.slotIndex = -1;
                data["replaced"] = u;
            }
        in.slot = it.slot;
        in.slotIndex = index;
    };

    if (op == "buy") {
        RequireDoc();
        std::string id = ArgStr(args, "item");
        auto itp = cat_.items.find(id);
        if (itp == cat_.items.end()) throw CwError("E_UNKNOWN", "unknown item '" + id + "'");
        const ItemDef& it = itp->second;
        int tier = ArgInt(args, "tier", it.tiers.front().tier);
        const TierDef* t = it.FindTier(tier);
        if (!t) throw CwError("E_UNKNOWN", it.name + " has no tier " + std::to_string(tier));
        if (const RipperdocDef* doc = OpenDoc(); doc && !InStock(doc->stock, id, tier) && !(env_ && env_->DebugEnabled()))
            throw CwError("E_NOT_IN_STOCK", doc->name + " does not sell " + it.name + " tier " + std::to_string(tier));
        if (d0.level < t->requires_.level) throw CwError("E_LEVEL", "needs level " + std::to_string(t->requires_.level));
        if (it.iconic) for (auto& [u, o] : s.instances) if (o.item == id) throw CwError("E_OWNED", "iconic " + it.name + " is already owned");
        std::string variant = ArgStr(args, "variant", false);
        const VariantDef* v = variant.empty() ? &it.variants.front() : it.FindVariant(variant);
        if (!v) throw CwError("E_UNKNOWN", it.name + " has no variant '" + variant + "'");
        pay(Price(t->price, &it, d0) + Price(v->price, nullptr, d0));
        Instance in;
        in.uid = NewUid(s, 'c'); in.item = id; in.tier = tier; in.variant = v->id;
        in.rolls = RollGroups(s, t->rolls, extra);
        in.mods.assign(t->modSlots, "");
        s.instances[in.uid] = in;
        if (ArgBool(args, "install")) install(s.instances[in.uid], ArgInt(args, "index", -1), ArgBool(args, "replace"));
        data["uid"] = in.uid;
    } else if (op == "buyGear") {
        RequireDoc();
        std::string id = ArgStr(args, "gear");
        int count = ArgInt(args, "count", 1);
        if (count < 1 || count > 20) throw CwError("E_BAD_ARGS", "count must be 1-20");
        const GearDef* g = nullptr;
        for (auto& x : gear_) if (x.id == id) g = &x;
        if (!g) throw CwError("E_UNKNOWN", "unknown gear '" + id + "'");
        if (const RipperdocDef* doc = OpenDoc(); doc && g->doc != doc->id && !(env_ && env_->DebugEnabled()))
            throw CwError("E_NOT_IN_STOCK", doc->name + " does not sell " + g->name);
        pay(Price(g->price, nullptr, d0) * count);
        gives.emplace_back(g, count);
        data["gear"] = id;
        data["count"] = count;
    } else if (op == "buyMod") {
        RequireDoc();
        std::string id = ArgStr(args, "mod");
        auto mp = cat_.mods.find(id);
        if (mp == cat_.mods.end()) throw CwError("E_UNKNOWN", "unknown mod '" + id + "'");
        int tier = ArgInt(args, "tier", mp->second.tiers.front().tier);
        const TierDef* t = mp->second.FindTier(tier);
        if (!t) throw CwError("E_UNKNOWN", mp->second.name + " has no tier " + std::to_string(tier));
        if (const RipperdocDef* doc = OpenDoc(); doc && !InStock(doc->modStock, id, tier) && !(env_ && env_->DebugEnabled()))
            throw CwError("E_NOT_IN_STOCK", doc->name + " does not sell " + mp->second.name + " tier " + std::to_string(tier));
        if (d0.level < t->requires_.level) throw CwError("E_LEVEL", "needs level " + std::to_string(t->requires_.level));
        pay(Price(t->price, nullptr, d0));
        ModInstance m;
        m.uid = NewUid(s, 'm'); m.mod = id; m.tier = tier; m.rolls = RollGroups(s, t->rolls, extra);
        s.modInstances[m.uid] = m;
        data["uid"] = m.uid;
        if (args.contains("attachTo")) {
            Instance& in = inst(ArgStr(args, "attachTo"));
            int ms = ArgInt(args, "modSlot");
            const ItemDef& it = cat_.items.at(in.item);
            if (ms < 0 || ms >= (int)in.mods.size()) throw CwError("E_BAD_ARGS", "modSlot out of range");
            if (it.modSlotTypes[ms] != mp->second.type) throw CwError("E_MOD_TYPE", "slot takes '" + it.modSlotTypes[ms] + "' mods");
            if (!in.mods[ms].empty()) throw CwError("E_SLOT_FULL", "mod slot is occupied");
            in.mods[ms] = m.uid; s.modInstances[m.uid].attachedTo = in.uid;
        }
    } else if (op == "sell") {
        RequireDoc();
        Instance& in = inst(ArgStr(args, "uid"));
        const ItemDef& it = cat_.items.at(in.item);
        if (it.iconic) throw CwError("E_ICONIC", "iconic cyberware cannot be sold");
        if (!in.slot.empty()) throw CwError("E_INSTALLED", "remove it before selling");
        for (auto& m : in.mods) if (!m.empty()) s.modInstances[m].attachedTo.clear();
        delta += SellPrice(it.FindTier(in.tier)->price, d0);
        data["uid"] = in.uid;
        s.instances.erase(in.uid);
    } else if (op == "sellMod") {
        RequireDoc();
        ModInstance& m = modi(ArgStr(args, "uid"));
        if (!m.attachedTo.empty()) throw CwError("E_INSTALLED", "detach it before selling");
        delta += SellPrice(cat_.mods.at(m.mod).FindTier(m.tier)->price, d0);
        data["uid"] = m.uid;
        s.modInstances.erase(m.uid);
    } else if (op == "install") {
        RequireDoc();
        Instance& in = inst(ArgStr(args, "uid"));
        if (!in.slot.empty() && !args.contains("index")) throw CwError("E_INSTALLED", "already installed");
        install(in, ArgInt(args, "index", -1), ArgBool(args, "replace"));
        data["slot"] = in.slot; data["index"] = in.slotIndex;
    } else if (op == "remove") {
        RequireDoc();
        Instance& in = inst(ArgStr(args, "uid"));
        if (in.slot.empty()) throw CwError("E_NOT_INSTALLED", "not installed");
        in.slot.clear(); in.slotIndex = -1;
    } else if (op == "upgrade") {
        RequireDoc();
        Instance& in = inst(ArgStr(args, "uid"));
        const ItemDef& it = cat_.items.at(in.item);
        const TierDef* cur = it.FindTier(in.tier);
        const TierDef* next = it.NextTier(in.tier);
        if (!next) throw CwError("E_MAX_TIER", it.name + " is at its highest tier");
        if (d0.level < next->requires_.level) throw CwError("E_LEVEL", "needs level " + std::to_string(next->requires_.level));
        int64_t base = next->upgradePrice ? *next->upgradePrice : std::max<int64_t>(0, next->price - cur->price);
        if (it.iconic) base = (int64_t)std::llround(base * cat_.iconicUpgradeMult);
        // Components (the lead, 2026-11-08): the new tier also costs FO4 components, counted the
        // way FO4 crafting counts them (raw components + what the junk in the inventory yields).
        if (s.settings.upgradeComponents && !next->components.empty()) {
            std::string missing;
            for (auto& cc : next->components) {
                const int have = env_ ? env_->CountComponent(cc.form) : 0;
                if (have < cc.count)
                    missing += (missing.empty() ? "" : ", ") + cc.name + " " + std::to_string(have) + "/" + std::to_string(cc.count);
            }
            if (!missing.empty()) throw CwError("E_COMPONENTS", "needs components: " + missing);
            for (auto& cc : next->components) consumeAtCommit.push_back({ cc.form, cc.count });
        }
        pay(Price(base, &it, d0));
        in.tier = next->tier;
        in.rolls = RollGroups(s, next->rolls, extra);  // CP2077: an upgraded piece rolls fresh for its new tier
        while ((int)in.mods.size() < next->modSlots) in.mods.push_back("");
        while ((int)in.mods.size() > next->modSlots) {
            if (!in.mods.back().empty()) s.modInstances[in.mods.back()].attachedTo.clear();
            in.mods.pop_back();
        }
        data["tier"] = in.tier;
    } else if (op == "attachMod") {
        RequireDoc();
        Instance& in = inst(ArgStr(args, "uid"));
        ModInstance& m = modi(ArgStr(args, "modUid"));
        int ms = ArgInt(args, "modSlot");
        const ItemDef& it = cat_.items.at(in.item);
        if (ms < 0 || ms >= (int)in.mods.size()) throw CwError("E_BAD_ARGS", "modSlot " + std::to_string(ms) + " is not active at this tier (" + std::to_string(in.mods.size()) + " active)");
        if (it.modSlotTypes[ms] != cat_.mods.at(m.mod).type) throw CwError("E_MOD_TYPE", "slot takes '" + it.modSlotTypes[ms] + "' mods, this is '" + cat_.mods.at(m.mod).type + "'");
        if (!m.attachedTo.empty()) {  // moving it from another piece
            Instance& old = inst(m.attachedTo);
            for (auto& x : old.mods) if (x == m.uid) x.clear();
        }
        if (!in.mods[ms].empty()) {
            if (!ArgBool(args, "replace")) throw CwError("E_SLOT_FULL", "mod slot holds " + in.mods[ms] + " (pass replace:true)");
            s.modInstances[in.mods[ms]].attachedTo.clear();
            data["replaced"] = in.mods[ms];
        }
        in.mods[ms] = m.uid;
        m.attachedTo = in.uid;
    } else if (op == "detachMod") {
        RequireDoc();
        Instance& in = inst(ArgStr(args, "uid"));
        int ms = ArgInt(args, "modSlot");
        if (ms < 0 || ms >= (int)in.mods.size() || in.mods[ms].empty()) throw CwError("E_BAD_ARGS", "no mod in that slot");
        s.modInstances[in.mods[ms]].attachedTo.clear();
        data["modUid"] = in.mods[ms];
        in.mods[ms].clear();
    } else if (op == "setVariant") {
        Instance& in = inst(ArgStr(args, "uid"));
        const ItemDef& it = cat_.items.at(in.item);
        std::string vid = ArgStr(args, "variant");
        const VariantDef* v = it.FindVariant(vid);
        if (!v) throw CwError("E_UNKNOWN", it.name + " has no variant '" + vid + "'");
        if (v->price > 0) { RequireDoc(); pay(Price(v->price, nullptr, d0)); }
        in.variant = vid;
    } else if (op == "buyCyberPerk") {
        std::string id = ArgStr(args, "perk");
        auto p = cat_.perks.find(id);
        if (p == cat_.perks.end()) throw CwError("E_UNKNOWN", "unknown cyber perk '" + id + "'");
        if (p->second.autoGrant) throw CwError("E_AUTO_PERK", p->second.name + " comes from a Fallout 4 perk and cannot be bought");
        if (d0.level < p->second.requires_.level) throw CwError("E_LEVEL", p->second.name + " needs level " + std::to_string(p->second.requires_.level));
        for (auto& pre : p->second.prereq)
            if (!d0.perkRanksEffective.count(pre)) throw CwError("E_PREREQ", p->second.name + " needs " + cat_.perks.at(pre).name + " first");
        int& r = s.perkRanks[id];
        if (r >= p->second.maxRank) throw CwError("E_MAX_RANK", p->second.name + " is at max rank");
        ++r;
        data["rank"] = r;
    } else if (op == "refundCyberPerk") {
        if (!cat_.perkRefund) throw CwError("E_DISABLED", "perk refunds are off in this catalog");
        std::string id = ArgStr(args, "perk");
        auto it = s.perkRanks.find(id);
        if (it == s.perkRanks.end() || it->second <= 0) throw CwError("E_BAD_ARGS", "no rank of '" + id + "' to refund");
        if (it->second == 1)
            for (auto& [other, rank] : s.perkRanks)
                if (rank > 0 && other != id)
                    for (auto& pre : cat_.perks.at(other).prereq)
                        if (pre == id) throw CwError("E_PREREQ", cat_.perks.at(other).name + " depends on it: refund that first");
        if (--it->second == 0) s.perkRanks.erase(it);
    } else if (op == "reroll") {
        RequireDoc();
        Instance& in = inst(ArgStr(args, "uid"));
        const ItemDef& it = cat_.items.at(in.item);
        const TierDef* t = it.FindTier(in.tier);
        if (t->rolls.empty()) throw CwError("E_NO_ROLLS", it.name + " tier " + std::to_string(in.tier) + " has no rolled stats");
        pay(Price((int64_t)std::llround(t->price * cat_.rerollMult * std::max(0.0, d0.rerollCostMult)), &it, d0));
        std::vector<Roll> fresh = RollGroups(s, t->rolls, extra);
        data["old"] = RollsJson(in.rolls);
        data["new"] = RollsJson(fresh);
        if (d0.rerollKeepOld) { in.pendingRolls = fresh; in.hasPending = true; data["pending"] = true; }
        else { in.rolls = fresh; data["pending"] = false; }
    } else if (op == "rerollResolve") {
        Instance& in = inst(ArgStr(args, "uid"));
        if (!in.hasPending) throw CwError("E_BAD_ARGS", "no reroll is waiting on " + in.uid);
        std::string keep = ArgStr(args, "keep");
        if (keep != "old" && keep != "new") throw CwError("E_BAD_ARGS", "keep must be \"old\" or \"new\"");
        if (keep == "new") in.rolls = in.pendingRolls;
        in.pendingRolls.clear();
        in.hasPending = false;
        data["kept"] = keep;
    } else if (op == "useCapacityItem") {
        std::string id = ArgStr(args, "id");
        auto ci = cat_.capacityItems.find(id);
        if (ci == cat_.capacityItems.end()) throw CwError("E_UNKNOWN", "unknown capacity item '" + id + "'");
        if (s.capacityItemsUsed[id] >= ci->second.max) throw CwError("E_MAX_USES", ci->second.name + " is used up (" + std::to_string(ci->second.max) + " max)");
        bool free = env_ && env_->DebugEnabled() && ArgBool(args, "free");
        if (!free && !(env_ && env_->CountItem(ci->second.ItemKey()) > 0)) throw CwError("E_NOT_OWNED", "no " + ci->second.name + " in the inventory");
        if (!free && !dryRun && !env_->ConsumeItem(ci->second.ItemKey(), 1)) throw CwError("E_NOT_OWNED", "the game refused to take " + ci->second.name);
        s.capacityItemsUsed[id] += 1;
        data["used"] = s.capacityItemsUsed[id];
    } else if (op == "buyCapacityItem") {
        // A shard bought at the ripperdoc applies immediately (BETA QA, the lead, 2026-11-08:
        // "a few sold at ripperdocs"). Price scales like everything else; max uses still bind.
        RequireDoc();
        std::string id = ArgStr(args, "id");
        auto ci = cat_.capacityItems.find(id);
        if (ci == cat_.capacityItems.end()) throw CwError("E_UNKNOWN", "unknown capacity item '" + id + "'");
        const RipperdocDef* doc = OpenDoc();
        // RequireDoc() lets bDebug through with no doc open (seam testing), but this op has no
        // price source without one: refuse, never dereference. (cw-int-3 boot-3 crash, +0xD7451.)
        if (!doc) throw CwError("E_NO_RIPPERDOC", "this needs a ripperdoc (no ripperdoc menu is open)");
        int64_t base = -1;
        for (auto& [cid, pr] : doc->capacityItemStock) if (cid == id) base = pr;
        if (base < 0) throw CwError("E_NOT_IN_STOCK", doc->name + " does not sell " + ci->second.name);
        if (s.capacityItemsUsed[id] >= ci->second.max) throw CwError("E_MAX_USES", ci->second.name + " is used up (" + std::to_string(ci->second.max) + " max)");
        pay(Price(base, nullptr, d0));
        s.capacityItemsUsed[id] += 1;
        data["used"] = s.capacityItemsUsed[id];
    } else if (op == "setLoadout") {
        // D1: the quickhack loadout. Free, no ripperdoc (docs/QUICKHACK-LOADOUT.md 6.2), but NOT a combat action:
        // The lead's ruling, 2026-11-10 - re-ranking mid-fight would erase the slot limit. A refusal changes
        // nothing, so a running upload is never cancelled or refunded by it.
        if (env_ && env_->PlayerIs("combat")) throw CwError("E_BUSY", "Not now - you are in combat. Change your deck when the shooting stops.");
        if (HacksBusy()) throw CwError("E_BUSY", "Not now - a quickhack is running. Wait for it to finish.");
        LoadoutSet(s, args);
        {   // the stored pick may have left the deck: report what it is now, and keep it an id, never an index
            Derived nd = Compute(s);
            const std::vector<std::string> eq = DeckQuickhacks(s, nd), order = LoadoutOrder(s);
            if (!eq.empty() && std::find(eq.begin(), eq.end(), s.hackSelected) == eq.end()) s.hackSelected = eq[0];
            json benched = json::array();
            for (auto& q : order)
                if (std::find(eq.begin(), eq.end(), q) == eq.end()) benched.push_back(q);
            data = { { "equipped", eq }, { "benched", benched }, { "custom", !s.loadout.empty() } };
        }
    } else if (op == "setCustomize") {          // appearance only: free, no ripperdoc needed
        CustomizeSet(s.customize, args);
    } else if (op == "resetCustomize") {
        CustomizeReset(s.customize, args);
    } else if (op == "debugGrant") {
        if (!(env_ && env_->DebugEnabled())) throw CwError("E_DISABLED", "debug ops are off (bDebug=0 in Cyberware.ini)");
        delta += args.value("currency", (int64_t)0);
        s.bonusPerkPoints += args.value("perkPoints", 0);
    } else if (op == "debugReset") {
        if (!(env_ && env_->DebugEnabled())) throw CwError("E_DISABLED", "debug ops are off (bDebug=0 in Cyberware.ini)");
        uint64_t rng = s.rng;
        Settings keep = s.settings;   // player preferences survive a debug wipe
        s = State{};
        s.rng = rng;
        s.settings = keep;
    } else if (op == "setSettings") {
        // Settings tab (BETA QA): partial update, clamped, applied live (no ripperdoc needed).
        s.settings.MergeFromJson(args, /*strict=*/true);
        data = s.settings.ToJson();
    } else {
        throw CwError("E_UNKNOWN_OP", "unknown op '" + op + "'");
    }

    Derived d = Compute(s);
    // An already-over state may stay (penalty, not a brick); it may not grow, and a player op may
    // not shrink hardMax under it (perk refund). setSettings is fully lenient: the player may
    // always tighten their own knobs and accept the over-cap penalty.
    if (op == "setSettings") ValidateInvariants(s, d, d.capacityUsed, 0);
    else ValidateInvariants(s, d, d0.capacityUsed, d0.capacityHardMax);
    if (!dryRun) {
        if (delta < 0 && !env_->SpendCurrency(-delta)) throw CwError("E_NO_FUNDS", "the game refused to take " + std::to_string(-delta) + " " + cat_.currencyName);
        if (!consumeAtCommit.empty() && env_ && !env_->ConsumeComponents(consumeAtCommit)) {
            if (delta < 0) env_->AddCurrency(-delta);   // give the caps back; nothing else committed yet
            throw CwError("E_COMPONENTS", "the components could not be taken from the inventory");
        }
        if (delta > 0) env_->AddCurrency(delta);
        for (auto& [g, n] : gives)
            if (!env_->GiveItem(g->key, n)) {
                if (delta < 0) env_->AddCurrency(-delta);   // refund: the caps went, the item did not
                throw CwError("E_INTERNAL", "the game could not hand over " + g->name + " (is " + g->key.substr(1, g->key.find('|') - 1) + " loaded?)");
            }
        state_ = std::move(s);
        Sync();
        return { { "data", data }, { "state", StateView(state_, d) } };
    }
    data["currencyDelta"] = delta;
    return { { "data", data }, { "state", StateView(s, d, cash + delta) } };
}

json Engine::StateView(const State& s, const Derived& d, std::optional<int64_t> currency) const
{
    json v;
    v["apiVersion"] = kApiVersion;
    v["customize"] = CustomizeResolve(s.customize);
    v["ripperdoc"] = OpenDoc() ? json(currentRipperdoc) : json(nullptr);
    v["currency"] = { { "name", cat_.currencyName }, { "amount", currency ? *currency : (env_ ? env_->GetCurrency() : 0) } };
    v["level"] = d.level;
    v["capacity"] = { { "used", d.capacityUsed }, { "max", d.capacityMax }, { "bonus", d.capacityBonus }, { "hardMax", d.capacityHardMax },
                      { "overCapacityPenalty", d.overCapPenalty > 0 ? json{ { "stat", d.overCapStat }, { "value", std::round(d.overCapPenalty * 100) / 100 } } : json(nullptr) } };
    v["perkPoints"] = { { "total", d.perkPointsTotal }, { "spent", d.perkPointsSpent }, { "available", d.perkPointsTotal - d.perkPointsSpent } };
    v["settings"] = s.settings.ToJson();   // additive v1 field (UI-API §3; the Settings tab reads and setSettings writes it)

    json slots = json::array();
    for (auto& sl : cat_.slots) {
        int cnt = d.slotCounts.at(sl.id);
        json pos = json::array();
        for (int i = 0; i < cnt; ++i) pos.push_back(nullptr);
        for (auto& [uid, in] : s.instances)
            if (in.slot == sl.id && in.slotIndex >= 0 && in.slotIndex < cnt) pos[in.slotIndex] = uid;
        slots.push_back({ { "id", sl.id }, { "name", sl.name }, { "count", cnt }, { "installed", pos } });
    }
    v["slots"] = slots;

    json insts = json::object(), stash = json::array();
    for (auto& [uid, in] : s.instances) {
        const ItemDef& it = cat_.items.at(in.item);
        const TierDef* t = it.FindTier(in.tier);
        json o;
        o["uid"] = uid; o["item"] = in.item; o["name"] = it.name; o["tier"] = in.tier; o["variant"] = in.variant; o["iconic"] = it.iconic;
        o["installed"] = in.slot.empty() ? json(nullptr) : json{ { "slot", in.slot }, { "index", in.slotIndex } };
        json mods = json::array();
        for (auto& m : in.mods) mods.push_back(m.empty() ? json(nullptr) : json(m));
        o["mods"] = mods;
        o["modSlotTypes"] = json(std::vector<std::string>(it.modSlotTypes.begin(), it.modSlotTypes.begin() + std::min(in.mods.size(), it.modSlotTypes.size())));
        o["rolls"] = RollsJson(in.rolls);
        o["pendingRolls"] = in.hasPending ? RollsJson(in.pendingRolls) : json(nullptr);
        if (!t->rolls.empty()) {
            int64_t rp = Price((int64_t)std::llround(t->price * cat_.rerollMult * std::max(0.0, d.rerollCostMult)), &it, d);
            int64_t have = currency ? *currency : (env_ ? env_->GetCurrency() : 0);
            std::string rr = in.hasPending ? "E_PENDING_REROLL" : (have < rp ? "E_NO_FUNDS" : "");
            o["reroll"] = { { "price", rp }, { "keepOld", d.rerollKeepOld }, { "available", rr.empty() }, { "reason", rr.empty() ? json(nullptr) : json(rr) } };
        } else {
            o["reroll"] = nullptr;
        }
        o["capacityGrant"] = t->capacityGrant;
        o["stats"] = StatsJson(d.perInstance.at(uid));
        o["capacityCost"] = d.instanceCost.at(uid);
        o["sellPrice"] = it.iconic ? json(nullptr) : json(SellPrice(t->price, d));
        if (const TierDef* next = it.NextTier(in.tier)) {
            int64_t base = next->upgradePrice ? *next->upgradePrice : std::max<int64_t>(0, next->price - t->price);
            if (it.iconic) base = (int64_t)std::llround(base * cat_.iconicUpgradeMult);
            int64_t price = Price(base, &it, d);
            int64_t have = currency ? *currency : (env_ ? env_->GetCurrency() : 0);
            std::string reason;
            json comps = json::array();   // the new tier's component bill: have / need per component
            if (s.settings.upgradeComponents)
                for (auto& cc : next->components) {
                    const int haveN = env_ ? env_->CountComponent(cc.form) : 0;
                    comps.push_back({ { "form", cc.form }, { "name", cc.name }, { "need", cc.count }, { "have", haveN } });
                    if (haveN < cc.count && reason.empty()) reason = "E_COMPONENTS";
                }
            if (d.level < next->requires_.level) reason = "E_LEVEL";
            else if (have < price) reason = "E_NO_FUNDS";
            else if (reason.empty() && !in.slot.empty()) {
                State probe = s;  // exact check: recompute with the piece at its next tier
                probe.instances[uid].tier = next->tier;
                Derived pd = Compute(probe);
                if (pd.capacityUsed > pd.capacityMax) reason = "E_CAPACITY";
            }
            o["upgrade"] = { { "tier", next->tier }, { "price", price }, { "requiresLevel", next->requires_.level },
                             { "components", comps },
                             { "available", reason.empty() }, { "reason", reason.empty() ? json(nullptr) : json(reason) } };
        } else {
            o["upgrade"] = nullptr;
        }
        insts[uid] = o;
        if (in.slot.empty()) stash.push_back(uid);
    }
    v["instances"] = insts;
    v["stash"] = stash;
    v["quickhacks"] = QuickhacksView(s, d);   // D1: the quickhack loadout (docs/QUICKHACK-LOADOUT.md 6.1)

    json mods = json::object(), modStash = json::array();
    for (auto& [uid, m] : s.modInstances) {
        const ModDef& md = cat_.mods.at(m.mod);
        const TierDef* t = md.FindTier(m.tier);
        StatMap st; AddStats(st, t->stats); AddRolls(st, m.rolls);
        int slotIdx = -1;
        if (!m.attachedTo.empty())
            if (auto pi = s.instances.find(m.attachedTo); pi != s.instances.end())
                for (size_t i = 0; i < pi->second.mods.size(); ++i) if (pi->second.mods[i] == uid) slotIdx = (int)i;
        mods[uid] = { { "uid", uid }, { "mod", m.mod }, { "name", md.name }, { "type", md.type }, { "tier", m.tier },
                      { "rolls", RollsJson(m.rolls) }, { "stats", StatsJson(st) },
                      { "attachedTo", m.attachedTo.empty() ? json(nullptr) : json{ { "uid", m.attachedTo }, { "modSlot", slotIdx } } },
                      { "sellPrice", SellPrice(t->price, d) } };
        if (m.attachedTo.empty()) modStash.push_back(uid);
    }
    v["mods"] = mods;
    v["modStash"] = modStash;

    json perks = json::array();
    int avail = d.perkPointsTotal - d.perkPointsSpent;
    for (auto& id : cat_.perkOrder) {
        const auto& p = cat_.perks.at(id);
        int rank = d.perkRanksEffective.count(id) ? d.perkRanksEffective.at(id) : 0;
        std::string why;
        if (p.autoGrant) why = "E_AUTO_PERK";
        else if (rank >= p.maxRank) why = "E_MAX_RANK";
        else if (d.level < p.requires_.level) why = "E_LEVEL";
        else for (auto& pre : p.prereq) if (!d.perkRanksEffective.count(pre)) { why = "E_PREREQ"; break; }
        if (why.empty() && avail < p.cost) why = "E_PERK_POINTS";
        bool dependents = false;
        for (auto& [other, r] : s.perkRanks)
            if (r > 0 && other != id) for (auto& pre : cat_.perks.at(other).prereq) if (pre == id) dependents = true;
        perks.push_back({ { "id", id }, { "name", p.name }, { "rank", rank }, { "maxRank", p.maxRank }, { "cost", p.cost }, { "auto", p.autoGrant },
                          { "prereq", p.prereq }, { "requiresLevel", p.requires_.level },
                          { "canBuy", why.empty() }, { "reason", why.empty() ? json(nullptr) : json(why) },
                          { "canRefund", !p.autoGrant && cat_.perkRefund && rank > 0 && !(rank == 1 && dependents) } });
    }
    v["cyberPerks"] = perks;
    v["totals"] = StatsJson(d.totals);
    v["applied"] = StatsJson(d.applied);
    v["abilities"] = json(std::vector<std::string>(d.abilities.begin(), d.abilities.end()));
    json nat = json::array();
    for (auto& na : d.desired.native) nat.push_back({ { "ability", na.ability }, { "handler", na.handler }, { "activation", na.activation }, { "tier", na.tier }, { "params", na.params } });
    v["nativeAbilities"] = nat;
    json caps = json::array();
    for (auto& id : cat_.capacityItemOrder) {
        const CapacityItemDef& ci = cat_.capacityItems.at(id);
        int used = s.capacityItemsUsed.count(id) ? s.capacityItemsUsed.at(id) : 0;
        caps.push_back({ { "id", id }, { "name", ci.name }, { "add", ci.add }, { "used", used }, { "max", ci.max },
                         { "inInventory", env_ ? env_->CountItem(ci.ItemKey()) : 0 } });
    }
    v["capacityItems"] = caps;
    v["orphans"] = s.orphans.size();
    return v;
}

json Engine::ShopView(const State& s, const Derived& d, std::optional<int64_t> currency) const
{
    json v;
    const RipperdocDef* doc = OpenDoc();
    v["ripperdoc"] = doc ? json{ { "id", doc->id }, { "name", doc->name }, { "priceMult", doc->priceMult } } : json(nullptr);
    int64_t have = currency ? *currency : (env_ ? env_->GetCurrency() : 0);
    json items = json::array(), mods = json::array();
    if (doc) {
        for (auto& e : doc->stock) {
            const ItemDef& it = cat_.items.at(e.id);
            bool owned = false;
            for (auto& [u, o] : s.instances) if (o.item == e.id) owned = true;
            for (int tier : e.tiers) {
                const TierDef* t = it.FindTier(tier);
                int64_t price = Price(t->price, &it, d);
                std::string reason;
                if (it.iconic && owned) reason = "E_OWNED";
                else if (d.level < t->requires_.level) reason = "E_LEVEL";
                else if (have < price) reason = "E_NO_FUNDS";
                items.push_back({ { "item", e.id }, { "tier", tier }, { "price", price }, { "capacityCost", t->capacityCost },
                                  { "requiresLevel", t->requires_.level }, { "owned", owned },
                                  { "available", reason.empty() }, { "reason", reason.empty() ? json(nullptr) : json(reason) } });
            }
        }
        for (auto& e : doc->modStock) {
            const ModDef& md = cat_.mods.at(e.id);
            for (int tier : e.tiers) {
                const TierDef* t = md.FindTier(tier);
                int64_t price = Price(t->price, nullptr, d);
                std::string reason;
                if (d.level < t->requires_.level) reason = "E_LEVEL";
                else if (have < price) reason = "E_NO_FUNDS";
                mods.push_back({ { "mod", e.id }, { "tier", tier }, { "price", price }, { "requiresLevel", t->requires_.level },
                                 { "available", reason.empty() }, { "reason", reason.empty() ? json(nullptr) : json(reason) } });
            }
        }
    }
    json gear = json::array();
    if (doc)
        for (auto& g : gear_) {
            if (g.doc != doc->id) continue;
            int64_t price = Price(g.price, nullptr, d);
            gear.push_back({ { "gear", g.id }, { "kind", g.kind }, { "name", g.name }, { "price", price },
                             { "description", g.description }, { "lore", g.lore },
                             { "available", have >= price }, { "reason", have >= price ? json(nullptr) : json("E_NO_FUNDS") } });
        }
    v["items"] = items;
    v["mods"] = mods;
    json capItems = json::array();   // CP shards sold at this doc (additive v1 field, BETA QA)
    if (doc)
        for (auto& [cid, basePrice] : doc->capacityItemStock) {
            const auto& ci = cat_.capacityItems.at(cid);
            const int used = s.capacityItemsUsed.count(cid) ? s.capacityItemsUsed.at(cid) : 0;
            const int64_t price = Price(basePrice, nullptr, d);
            std::string reason;
            if (used >= ci.max) reason = "E_MAX_USES";
            else if (have < price) reason = "E_NO_FUNDS";
            capItems.push_back({ { "id", cid }, { "name", ci.name }, { "price", price }, { "add", ci.add },
                                 { "used", used }, { "max", ci.max },
                                 { "available", reason.empty() }, { "reason", reason.empty() ? json(nullptr) : json(reason) } });
        }
    v["capacityItems"] = capItems;
    v["gear"] = gear;
    return v;
}

}  // namespace cw
