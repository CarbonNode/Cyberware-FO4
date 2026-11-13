// cw_effects.cpp - what each native ability's CW_Nat_<ability>_T<tier> perk carries, and which handlers are real.
//
// This is the ONE place a catalog param becomes a game effect. cwtest --native-effects writes the result for every
// ability and tier (native-effects.json) and espgen bakes it into Cyberware.esp; espgen holds no param logic itself.
//
//   param                     effect (FO4)                                         notes
//   DamageResist / CarryWeight value modifier on the AV
//   speed_pct, sneak_speed_pct SpeedMult +v (sneak_speed only while sneaking)       arrays: the first band
//   noise_pct                 DetectionMovementMod +v/100
//   sneak_dmg_pct             entry Mod Sneak Attack Mult x(1+v/100) (Lynx Paws; vanilla Ninja uses the same entry point)
//   crit_chance_pct / crit_dmg_pct / mitigation_chance_pct  CW_CritChance / CW_CritDamage / CW_MitigationChance,
//                             read by the always-on CW_ChromeCrit perk (espgen)
//   dmg_taken_pct             entry Mod Incoming Damage x(1+v/100)
//   dmg_pct                   entry Mod Attack Damage x(1+v/100); target_keywords -> target-tab HasKeyword (OR)
//   melee_dmg_pct / ranged_dmg_pct / ballistic_dmg_pct   Mod Attack Damage with weapon-tab keyword conditions
//   ap_cost_pct / vats_ap_cost_pct  entry power-attack / VATS AP cost x(1+v/100)
//   vats_hit_pct              entry VATS hit chance x(1+v/100)
//   stagger                   entries: target stagger x1.5, incoming stagger x0.5
//   grenade_trajectory        entry Show Grenade Trajectory = 1 (vanilla DemolitionExpert02; Explosive Analysis)
// Arm mods (ArmMod family, CP 2.x params, tools/catalog_data.py MOD_PARAMS). The arm is the handler prefix; every entry
// carries a weapon-tab HasKeyword on that arm WEAP's CW_Anims* keyword:
//   dmg_pct                   Mod Attack Damage x(1+v/100)                         (Low / Medium batteries)
//   charged_dmg_pct           the same + owner IsPowerAttacking (CP charged attack = FO4 power attack)
//   status + status_chance_pct  Apply Combat Hit Spell (vanilla bleed / burn / poison / shock spell), GetRandomPercent < v
//   damage_type physical      nothing to do (the default); thermal / chemical / electrical: the damage CONVERSION is the
//                             element OMOD (MSWP + ENCH, ARMS 5.1) and stays pending; the status hit spell is real
//   attack_speed_pct, round, charges_add  plugin runtime (arm WEAP speed, PLS projectile explosion, PLS charges)
// Handler-wide conditions (the effect's own conditions, re-checked by the engine): Scarab while sneaking, Scar
// Coalescer below 50% HP, Threat Evac below 25% HP, Kinetic Frame above 85% AP.
// Anything else is `pending`: listed per handler so the stub count stays honest.
#include "cw_core.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <sstream>

namespace cw {

// WHAT A PERK-ENTRY CHANCE REALLY APPLIES (track B, ab_ns_stabber; the lead, 2026-11-10).
// espgen bakes a `chance` condition as `GetRandomPercent < value`. GetRandomPercent is a WHOLE roll, 0..99 (the
// Creation Kit reference for this engine line; NOT re-measured in Fallout 4), so `< 12.25` passes for rolls 0..12:
// 13 of 100, not 12.25. A chance that is not a whole percent therefore cannot be applied as stated, and the catalog
// used to state 12.25 / 14.5 / 16.75 for Stabber while the game applied 13 / 15 / 17.
// This is the ONE place that says what the engine applies. Every `chance` condition below goes through it, so the
// baked literal is the applied number (no behaviour change: ceil is what the engine already did), and
// EngineChanceProblems() fails a catalog whose STATED chance differs from it.
double EngineChancePct(double stated)
{
    if (!(stated > 0)) return 0;
    return std::min(100.0, std::ceil(stated - 1e-9));
}

namespace {
// The health fraction at which Threatevac's second speed band comes in. A DECISION (CP ramps continuously and has no
// second breakpoint); the shop text states the same number, so shown equals applied.
constexpr double kThreatevacDeepHp = 0.10;
const std::vector<std::string> kMeleeKw = { "WeaponTypeMelee1H", "WeaponTypeMelee2H", "WeaponTypeUnarmed", "WeaponTypeHandToHand" };

std::optional<double> Num(const json& p, const std::string& k)
{
    auto it = p.find(k);
    if (it == p.end()) return std::nullopt;
    if (it->is_number() && !it->is_boolean()) return it->get<double>();
    if (it->is_array() && !it->empty() && (*it)[0].is_number()) return (*it)[0].get<double>();   // banded: the first band
    return std::nullopt;
}

// Keys a family's runtime code (cw_native.cpp / the plugin) consumes: not effects, not pending.
const std::set<std::string>& RuntimeKeys(NativeFamily f)
{
    static const std::map<NativeFamily, std::set<std::string>> m = {
        { NativeFamily::TimeSlow, { "time_scale", "duration_s", "cooldown_s", "toggle", "kill_extend_s", "kill_refund_pct" } },
        { NativeFamily::Camo, { "duration_s", "cooldown_s" } },
        { NativeFamily::Scan, { "enemy_m", "turret_m", "radius_m", "duration_s", "zoom", "highlight", "trap_m" } },   // traps: the plugin's TrapScan (mines, hazards, trap activators)
        { NativeFamily::OnKill, { "on", "hp_pct", "ap_pct", "cooldown_cut_pct" } },
        { NativeFamily::OnHit, { "on_hit_keyword", "hp_pct", "ap_pct" } },
        { NativeFamily::Buff, { "duration_s", "cooldown_s", "ap_infinite", "melee_heal_pct", "loss_pct", "window_s", "chance_pct", "requires",
                                "ap_regen_pct", "headshot_pct", "slam_m", "combat" } },   // the plugin, while the buff runs (AP restore, head hits, landing slam);
                                                                                          // combat: BISF's "out of combat" flag (the pacify key, plugin)
        { NativeFamily::CondSlow, { "slow_pct", "duration_s", "cooldown_s", "player_exempt" } },   // + Sensory Protocol
        { NativeFamily::Heal, { "heal", "hot", "hot_s", "cooldown_s" } },
        { NativeFamily::SecondHeart, { "cooldown_s" } },
        { NativeFamily::Biomonitor, { "hp_threshold_pct", "item", "heal_bonus_pct" } },
        { NativeFamily::Jump, { "jumps", "jump_mult_max", "dash_m" } },
        { NativeFamily::Arms, { "base_dmg", "speed", "reach", "cleave_reach_m", "damage_type", "charges", "recharge_s", "slot", "explosion_radius_m", "leap_m", "targets" } },   // PLS dmg + radius: espgen bakes the T5 values into CW_PLSExplosion_*
        { NativeFamily::ArmMod, { "attack_speed_pct", "round", "charges_add", "recharge_mult_pct", "knockdown", "cripple", "duration_s", "spread_hack",
                                  "crit_pct", "armor_pen_pct", "emp_m", "emp_dmg", "interval_s", "pop_frac", "freq_pct", "crit_dmg_pct", "stacks", "stack_s" } },
        { NativeFamily::Gate, { "gate", "mod_slots" } },
        { NativeFamily::WeaponMod, { "melee_speed_pct", "reload_pct" } },
        { NativeFamily::Runtime, { "Health_pct", "ap_regen_pct", "speed_pct_max", "convert_pct", "bonus_energy_dmg", "turret_s", "robot_s", "fuse_s", "cooldown_s", "focus_m" } },   // Atomic Sensors, Metabolic Editor, Microgenerator (plugin)
        { NativeFamily::Passive, { "reload_pct", "zoom", "rad_purge_per_s" } },   // the plugin: weapon reloadSpeed; Kiroshi zoom (hold the scan key)
        { NativeFamily::Hud, { "zoom", "highlight_detected", "readout", "range_m", "cone_pct", "apex_m" } },   // Stalker: the plugin's cone highlight while aiming
        { NativeFamily::Hurt, { "dmg_taken_pct", "defer_pct", "over_s", "block_chance_pct", "chance_pct", "dmg", "radius_m", "DamageResist" } },   // mod_slots: the catalog opens the optics mod slots (to_schema)
        { NativeFamily::Deck, { "ram_max", "qh_slots", "tweakdb", "qh_dmg_pct", "qh_dot_dmg_pct", "dot_duration_pct", "qh_duration_pct", "ram_regen",
                                "stealth_cost_red", "ultimate_cost_red", "spread_targets", "spread_dist_pct", "elec_dmg_pct",
                                // batch 7 (decoded TweakDB effectors, tools/tweakdb_decks.py `effects`): Paraline's Monowire damage per missing
                                // RAM (plugin, arms::MonowireCleaveImpl), Arasaka's RAM on a takedown (NativeKill, sneak), Biotech Sigma's DoT
                                // refresh on upload (Quickhack), the Canto's fixed Blackwall Gateway program (DeckQuickhacks / Quickhack)
                                "mono_dmg_per_missing_ram_pct", "mono_dmg_max_pct", "ram_on_takedown", "qh_refresh_dot_m", "program", "program_targets", "program_spread_m" } },
        { NativeFamily::Ram, { "ram_max", "qh_crit_chance_pct", "threshold_pct", "restore_pct", "cooldown_s", "ram_per_hit", "range_pct" } },
    };
    static const std::set<std::string> none;
    auto it = m.find(f);
    return it == m.end() ? none : it->second;
}

// CP params with no FO4 counterpart: documented in the catalog, listed as n/a (not pending) by StatusOf.
const std::set<std::string>& NaKeys()
{
    // qh_cooldown_red_pct: CP 2.x quickhacks have no real cooldown (TweakDB: every QuickHack.*Hack start effect tagged
    // QuickHackCooldown lasts 0.0001 s x (1 - QuickhacksCooldownReduction)), so the reduction has nothing to act on in CP either.
    // qh_upload_pct (Ex-Disk): our quickhacks upload instantly; negate_hostile_hack (Self-ICE): FO4 has no enemy quickhacks.
    // max_charge_pct (Black-Market Battery) and charged_* (PLS rounds): FO4 power attacks / throws have no charge level.
    // upload_on_hit_pct (Paraline Mk.4: smart-weapon hits speed the upload on that target): our uploads are instant.
    // quest_facts (Canto): the Blackwall quest's fact toggles (q305_*), CP bookkeeping with nothing to drive in FO4.
    static const std::set<std::string> k = { "buffer", "upload_pct", "reveal_pct", "device_cost_pct", "overclock", "bonus", "qh_cooldown_red_pct",
                                             "qh_upload_pct", "negate_hostile_hack", "max_charge_pct", "charged_status_chance_pct", "charged_stun",
                                             "upload_on_hit_pct", "quest_facts" };
    return k;
}

// Conditions that apply to every effect of a handler (its description says "while ...").
std::vector<NatCond> HandlerWhen(const std::string& h)
{
    if (h == "sk_scarab") return { { "sneaking", 1 } };
    if (h == "sk_scar_coalescer") return { { "hp_below", 0.5 } };
    if (h == "ci_threatevac") return { { "hp_below", 0.25 } };
    if (h == "sk_kinetic_frame") return { { "ap_above", 0.85 } };
    return {};
}

// Handlers whose numeric params depend on something no perk condition can see (distance bands, attack direction,
// the target's poison state, per-point scaling): their params stay pending instead of being applied unconditionally.
bool Unconditionable(const std::string& h, const std::string& k)
{
    (void)k;
    static const std::set<std::string> whole = {};   // batch 8: Black Mamba and Atomic Sensors moved to plugin runtime
    return whole.count(h) > 0;
}
// The CW_Anims* keyword of the arm WEAP an arm mod (or the arm itself) belongs to (ARMS weapons_arms.json keywords_add),
// "" for the PLS.
std::string ArmKeyword(const std::string& h)
{
    if (h.rfind("mod_mantis_", 0) == 0 || h == "ar_mantis") return "CW_AnimsCyberMantis";
    if (h.rfind("mod_gorilla_", 0) == 0 || h == "ar_gorilla") return "CW_AnimsCyberGorilla";
    if (h.rfind("mod_mono_", 0) == 0 || h == "ar_monowire") return "CW_AnimsCyberMonowire";
    return {};
}
const std::vector<std::string> kElementKw = { "CW_ElementThermal", "CW_ElementChemical", "CW_ElementElectrical" };

// The arm itself (ar_mantis / ar_gorilla / ar_monowire): CP's physical bleed chance as a bleed hit spell that skips the
// element variants (an element mod converts the damage, CW_Element* on the variant WEAP), and Gorilla's charged punch.
NativeEffects ArmsEffects(const std::string& h, const json& p)
{
    NativeEffects e;
    const std::string kw = ArmKeyword(h);
    const auto& rt = RuntimeKeys(NativeFamily::Arms);
    for (auto& [k, raw] : p.items()) {
        if (rt.count(k)) continue;
        auto v = Num(p, k);
        if (k == "bleed_chance_pct" && v && !kw.empty()) {
            NatEntryFx x{ "hit_spell", 1, { kw }, kElementKw, {}, {}, "bleed" };
            if (*v < 100) x.when.push_back({ "chance", EngineChancePct(*v) });
            e.entries.push_back(x);
        }
        // CP charged punch = x charged_mult of a normal one; FO4's power attack already does x2 (race attack data, assumed),
        // so the entry adds the rest.
        else if (k == "charged_mult" && v && !kw.empty()) e.entries.push_back({ "attack", std::max(1.0, *v / 2.0), { kw }, {}, {}, { { "power_attack", 1 } }, {} });
        else e.pending.push_back(k);
    }
    return e;
}

NativeEffects ArmModEffects(const std::string& h, const json& p)
{
    NativeEffects e;
    const std::string kw = ArmKeyword(h);
    const bool pls = h.rfind("mod_pls_", 0) == 0;
    const auto& rt = RuntimeKeys(NativeFamily::ArmMod);
    std::vector<std::string> weap;
    if (!kw.empty()) weap.push_back(kw);
    for (auto& [k, raw] : p.items()) {
        if (rt.count(k)) continue;
        auto v = Num(p, k);
        if (k == "dmg_pct" && v && !pls) e.entries.push_back({ "attack", std::max(0.0, 1.0 + *v / 100.0), weap, {}, {}, {}, {} });
        else if (k == "charged_dmg_pct" && v && !pls) e.entries.push_back({ "attack", std::max(0.0, 1.0 + *v / 100.0), weap, {}, {}, { { "power_attack", 1 } }, {} });
        else if (k == "status_chance_pct" && v && !pls && p.contains("status") && p["status"].is_string()) {
            NatEntryFx x{ "hit_spell", 1, weap, {}, {}, {}, {} };
            if (*v < 100) x.when.push_back({ "chance", EngineChancePct(*v) });
            x.spell = p["status"].get<std::string>();
            e.entries.push_back(x);
        }
        else if (k == "status" && !pls) continue;                                  // read with status_chance_pct
        else if (NaKeys().count(k)) e.na.push_back(k);
        else if (pls && (k == "status" || k == "status_chance_pct" || k == "unconscious" || k == "paralysis_s")) continue;   // the round's EXPL + ENCH
        else if (k == "damage_type" && raw.is_string()) continue;   // the arm's element WEAP variant (plugin) / the PLS round's explosion
        else e.pending.push_back(k);
    }
    return e;
}
}  // namespace

bool PerkIsTimed(NativeFamily f)
{
    return f == NativeFamily::TimeSlow || f == NativeFamily::Camo || f == NativeFamily::Scan || f == NativeFamily::Buff || f == NativeFamily::CondSlow;
}

NativeEffects EffectsFor(const std::string& h, NativeFamily f, const json& params)
{
    NativeEffects e;
    const json p = params.is_object() ? params : json::object();
    if (f == NativeFamily::ArmMod) return ArmModEffects(h, p);
    if (f == NativeFamily::Arms) return ArmsEffects(h, p);
    if (f == NativeFamily::Stub || f == NativeFamily::Modifier || f == NativeFamily::Deck || f == NativeFamily::Ram || f == NativeFamily::Gate || f == NativeFamily::Hurt ||
        f == NativeFamily::WeaponMod || f == NativeFamily::Hud || f == NativeFamily::Runtime) {
        // Basic Kiroshi (batch 8): "turrets and cameras detect you 40% slower" = Mod Detection Light / Movement x0.6 when the
        // detecting actor (perk tab 1) is a turret (ActorTypeTurret). FO4 has no cameras. Vanilla uses both entries with 2 tabs.
        if (h == "face_kiroshi_basic")
            if (auto v = Num(p, "camera_detect_pct"))
                for (const char* en : { "detect_light", "detect_movement" }) e.entries.push_back({ en, std::max(0.0, 1.0 + *v / 100.0), {}, {}, { "ActorTypeTurret" }, {}, {} });
        for (auto& [k, v] : p.items()) if (!RuntimeKeys(f).count(k) && !(h == "face_kiroshi_basic" && k == "camera_detect_pct")) ((f == NativeFamily::Deck || f == NativeFamily::Ram) && NaKeys().count(k) ? e.na : e.pending).push_back(k);
        if (h == "fc_self_ice") e.na.push_back("negate_hostile_hack");   // FO4 has no enemy quickhacks to negate
        return e;
    }
    if (f == NativeFamily::TimeSlow || f == NativeFamily::Camo || f == NativeFamily::Scan) e.fx = SpellSpecFor(f, p);
    if (f == NativeFamily::CondSlow) {
        e.fx.kind = "slowtime";
        e.fx.magnitude = std::clamp(1.0 - Num(p, "slow_pct").value_or(50) / 100.0, 0.05, 1.0);
        // Revulsor: the world slows, the player does not -> a matching SpeedMult boost (DESIGN 3.5 "time slow").
        if (h == "ns_revulsor") e.avs.push_back({ "SpeedMult", std::min(300.0, std::round((1.0 / e.fx.magnitude - 1.0) * 100.0)), {} });
    }
    const std::vector<NatCond> W = HandlerWhen(h);
    const auto& rt = RuntimeKeys(f);
    auto mult = [](double pct) { return std::max(0.0, 1.0 + pct / 100.0); };
    // Distance-banded and conditional-crit items (2026-11-08 batch 4). ~70 FO4 units per metre. A "crit" outside VATS is
    // Chrome Crit's x1.5 on a GetRandomPercent roll, conditioned on the weapon or the target distance.
    {
        auto band = [&](const char* entry, double value, double lo, double hi, std::vector<std::string> weap = {}, std::vector<NatCond> when = {}) {
            NatEntryFx x{ entry, value, std::move(weap), {}, {}, std::move(when), {} };
            x.distMin = lo; x.distMax = hi;
            e.entries.push_back(x);
        };
        if (h == "it_proxishield" && p.contains("dmg_taken_pct") && p["dmg_taken_pct"].is_array() && p["dmg_taken_pct"].size() >= 2) {
            band("incoming", mult(p["dmg_taken_pct"][0].get<double>()), -1, 3 * 70.0);          // within 3 m
            band("incoming", mult(p["dmg_taken_pct"][1].get<double>()), 3 * 70.0, 6 * 70.0);   // 3-6 m
            return e;
        }
        if (h == "it_peripheral_inverse" && Num(p, "dmg_taken_pct")) { band("incoming", mult(*Num(p, "dmg_taken_pct")), Num(p, "min_m").value_or(20) * 70.0, -1); return e; }
        if (h == "ns_visual_cortex" && Num(p, "crit_chance_pct_max")) {
            const double mx = *Num(p, "crit_chance_pct_max"), at = Num(p, "max_at_m").value_or(30);
            for (int k = 1; k <= 4; ++k)   // +mx/4 per at/4 metres to the target
                band("attack", 1.5, k * at / 4 * 70.0, k == 4 ? -1 : (k + 1) * at / 4 * 70.0, {}, { { "chance", EngineChancePct(mx * k / 4) } });
            return e;
        }
        // THREATEVAC'S SECOND BAND (BETA QA truth-list row 14, 2026-11-10). The catalog has always stated
        // speed_pct as TWO bands and only the first was ever applied, because Num() reads element 0: the shop promised a
        // bonus "rising" as health fell and the build gave one flat figure. CP ramps it continuously as HP drops; FO4's
        // perk conditions cannot express a ramp, so two bands is the engine-native approximation, and the SECOND entry
        // carries only the DIFFERENCE so that below the deep threshold the two sum to the stated total (25 + 10 = 35 at
        // tier 5), exactly as banded perks elsewhere in this file work.
        // The deep threshold is a DECISION, not a source value: CP has no second breakpoint to copy. 10% of health is
        // the figure the shop text now states, so shown and applied are the same number.
        if (h == "ci_threatevac" && p.contains("speed_pct") && p["speed_pct"].is_array() && p["speed_pct"].size() >= 2 &&
            p["speed_pct"][0].is_number() && p["speed_pct"][1].is_number()) {
            const double lo = p["speed_pct"][0].get<double>(), hi = p["speed_pct"][1].get<double>();
            e.avs.push_back({ "SpeedMult", lo, { { "hp_below", 0.25 } } });
            if (hi > lo) e.avs.push_back({ "SpeedMult", hi - lo, { { "hp_below", kThreatevacDeepHp } } });
            return e;
        }
        if ((h == "ns_stabber" || h == "ha_handle_wrap") && Num(p, "crit_chance_pct")) {
            std::vector<std::string> kw = h == "ns_stabber" ? std::vector<std::string>{ "WeaponTypeMelee1H", "WeaponTypeMelee2H", "WeaponTypeThrown" }
                                                            : std::vector<std::string>{ "WeaponTypeThrown" };
            const double chance = EngineChancePct(*Num(p, "crit_chance_pct"));   // the whole percent the engine rolls, see EngineChancePct
            e.entries.push_back({ "attack", 1.5, kw, {}, {}, { { "chance", chance } }, {} });
            if (h == "ha_handle_wrap") e.entries.push_back({ "explosion", 1.5, {}, {}, {}, { { "chance", chance } }, {} });   // grenades
            return e;
        }
        // Cellular Adapter: per Endurance point, +energy weapon damage and explosion resistance (stacked Endurance bands)
        if (h == "it_cellular_adapter") {
            for (int k = 1; k <= 15; ++k) {
                std::vector<NatCond> w{ { "endurance_at_least", (double)k } };
                if (auto v = Num(p, "energy_dmg_pct")) e.entries.push_back({ "attack", mult(*v), { "WeaponTypeLaser", "WeaponTypePlasma", "WeaponTypeGammaGun", "WeaponTypeAlienBlaster" }, {}, {}, w, {} });
                if (auto v = Num(p, "explosion_resist_pct")) e.entries.push_back({ "incoming_explosion", mult(-*v), {}, {}, {}, w, {} });
            }
            if (auto v = Num(p, "CarryWeight")) e.avs.push_back({ "CarryWeight", *v, {} });
            return e;
        }
        // Black Mamba (batch 8): "90% less poison damage" = PoisonResist. FO4 resists follow the DR curve (0.5 x (dmg/DR)^0.366),
        // so +495 is ~90% off a typical 5/s poison tick (ESTIMATE). The +18% vs poisoned targets is the plugin (arms::PoisonBonus,
        // a target with an active PoisonResist-typed detrimental effect).
        if (h == "ci_black_mamba") {
            if (auto v = Num(p, "poison_taken_pct")) e.avs.push_back({ "PoisonResist", std::round(-*v * 5.5), {} });
            for (auto& [k, raw] : p.items()) if (k != "poison_taken_pct" && k != "dmg_pct") e.pending.push_back(k);
            return e;
        }
        if (h == "ha_tyger_claws_imprint" && Num(p, "blade_dmg_pct")) {
            e.entries.push_back({ "attack", mult(*Num(p, "blade_dmg_pct")), { "WeaponTypeMelee1H", "WeaponTypeMelee2H" }, {}, {}, W, {} });
            return e;
        }
    }
    // ram_max is the RAM pool's (Engine::RamMax) whatever the family; memory boost's "ram" is its on-kill refill.
    for (auto& [k, raw] : p.items()) {
        if (rt.count(k) || k == "ram_max" || (f == NativeFamily::OnKill && k == "ram")) continue;
        if (k == "target_keywords") continue;   // read with dmg_pct
        // Kerenzikov Boost: a stronger slow while Kerenzikov runs. Baked into CW_Nat_<kerenzikov ability>_boosted_T<t> (the perk
        // Engine::Trigger picks when the boost is installed, NativeEffectsJson below), not into this perk: two SlowTime effects at
        // once would race in the engine.
        if (h == "fc_kerenzikov_boost" && k == "slow_bonus_pct") continue;
        if (Unconditionable(h, k)) { e.pending.push_back(k); continue; }
        auto v = Num(p, k);
        bool done = true;
        if (k == "DamageResist" && v) e.avs.push_back({ "DamageResist", *v, W });
        else if (k == "CarryWeight" && v) e.avs.push_back({ "CarryWeight", *v, {} });
        else if (k == "speed_pct" && v) e.avs.push_back({ "SpeedMult", *v, W });
        else if (k == "sneak_speed_pct" && v) e.avs.push_back({ "SpeedMult", *v, { { "sneaking", 1 } } });
        else if (k == "noise_pct" && v) e.avs.push_back({ "DetectionMovementMod", *v / 100.0, W });
        else if (k == "sneak_dmg_pct" && v) e.entries.push_back({ "sneak_attack", mult(*v), {}, {}, {}, W, {} });
        else if (k == "crit_chance_pct" && v) e.avs.push_back({ "CW_CritChance", *v, W });
        else if (k == "crit_dmg_pct" && v) e.avs.push_back({ "CW_CritDamage", *v, W });
        else if (k == "mitigation_chance_pct" && v) e.avs.push_back({ "CW_MitigationChance", *v, W });
        else if (k == "dmg_taken_pct" && raw.is_number() && v) e.entries.push_back({ "incoming", mult(*v), {}, {}, {}, W, {} });
        else if (k == "dmg_pct" && v) {
            NatEntryFx x{ "attack", mult(*v), {}, {}, {}, W, {} };
            if (p.contains("target_keywords") && p["target_keywords"].is_array())
                for (auto& t : p["target_keywords"]) if (t.is_string()) x.targetAny.push_back(t.get<std::string>());
            e.entries.push_back(x);
        }
        else if (k == "melee_dmg_pct" && v) e.entries.push_back({ "attack", mult(*v), kMeleeKw, {}, {}, W, {} });
        else if (k == "ranged_dmg_pct" && v) e.entries.push_back({ "attack", mult(*v), {}, kMeleeKw, {}, W, {} });
        else if (k == "ballistic_dmg_pct" && v) e.entries.push_back({ "attack", mult(*v), { "WeaponTypeBallistic" }, {}, {}, W, {} });
        else if (k == "ap_cost_pct" && v) {
            e.entries.push_back({ "power_ap", mult(*v), {}, {}, {}, W, {} });
            if (h == "ci_isometric") e.entries.push_back({ "vats_ap", mult(*v), {}, {}, {}, W, {} });   // "melee and VATS"
        }
        else if (k == "vats_ap_cost_pct" && v) e.entries.push_back({ "vats_ap", mult(*v), {}, {}, {}, W, {} });
        else if (k == "vats_hit_pct" && v) e.entries.push_back({ "vats_hit", mult(*v), {}, {}, {}, W, {} });
        else if (k == "rad_resist" && v) e.avs.push_back({ "RadResistExposure", *v, W });
        else if (k == "pa_dr" && v) e.avs.push_back({ "DamageResist", *v, { { "in_power_armor", 1 } } });
        else if (k == "pa_er" && v) e.avs.push_back({ "EnergyResist", *v, { { "in_power_armor", 1 } } });
        else if (k == "pa_core_dmg_pct" && v) e.entries.push_back({ "incoming_battery", mult(*v), {}, {}, {}, W, {} });
        else if (k == "xp_pct" && v) e.entries.push_back({ "xp", mult(*v), {}, {}, {}, W, {} });
        else if (k == "terminal_lockout_pct" && v) e.entries.push_back({ "terminal_lockout", mult(*v), {}, {}, {}, W, {} });
        else if (k == "grenade_trajectory" && raw.is_boolean() && raw.get<bool>()) e.entries.push_back({ "grenade_trajectory", 1, {}, {}, {}, {}, {} });
        else if (k == "crit_meter_pct" && v) e.entries.push_back({ "vats_crit_charge", mult(*v), {}, {}, {}, W, {} });   // VATS critical meter fill
        else if (k == "recoil_pct" && v) e.entries.push_back({ "cone_of_fire", mult(*v), {}, {}, {}, W, {} });   // Militech Berserk: FO4 has no recoil entry; cone of fire (spread) stands in
        else if (k == "attunement") {}   // the catalog's stat attunement (SPECIAL), not a runtime effect
        else if (k == "stagger" && raw.is_boolean() && raw.get<bool>()) {
            e.entries.push_back({ "stagger_out", 1.5, {}, {}, {}, W, {} });
            e.entries.push_back({ "stagger_in", 0.5, {}, {}, {}, W, {} });
        }
        else done = false;
        if (!done) e.pending.push_back(k);
    }
    // Fortified Ankles: "no fall damage from charged jumps" - a perk cannot tell a charged jump, so all falls x0.5.
    if (h == "le_fortified_ankles") e.entries.push_back({ "fall", 0.5, {}, {}, {}, {}, {} });
    return e;
}

HandlerStatus StatusOf(const std::string& handler, const std::string& activation, const json& paramsT5)
{
    HandlerStatus s;
    NativeFamily f = FamilyOf(handler, activation, paramsT5);
    s.family = FamilyLabel(f);
    NativeEffects e = EffectsFor(handler, f, paramsT5);
    s.pending = e.pending;
    s.na = e.na;
    // A family that needs no perk (on-kill, heal, jump, arms, ...) is real by its code; Passive is real when it has effects.
    bool works = f != NativeFamily::Stub && (f != NativeFamily::Passive || !e.Empty());
    s.real = works;
    s.partial = works && !e.pending.empty();
    return s;
}

json NativeEffectsJson(const Catalog& cat)
{
    json perks = json::object(), handlers = json::object();
    // An ability only mods grant runs at its mod's tier (Engine: mt->tier), and every mod has tier 1 only: bake just the
    // tiers its mods list, not T1-T5 (36 arm mods x 5 tiers would spend ~290 ESL ids on perks that never apply).
    std::map<std::string, std::set<int>> modTiers;
    std::set<std::string> elsewhere;
    auto scan = [](const json& arr, std::function<void(const std::string&, int)> fn) {
        if (!arr.is_array()) return;
        for (auto& x : arr)
            for (auto& t : x.value("tiers", json::array()))
                for (auto& a : t.value("abilities", json::array())) if (a.is_string()) fn(a.get<std::string>(), t.value("tier", 1));
    };
    scan(cat.raw.value("mods", json::array()), [&](const std::string& a, int t) { modTiers[a].insert(std::clamp(t, 1, 5)); });
    scan(cat.raw.value("items", json::array()), [&](const std::string& a, int) { elsewhere.insert(a); });
    if (cat.raw.contains("items") && cat.raw["items"].is_array())
        for (auto& it : cat.raw["items"]) for (auto& a : it.value("iconicAbilities", json::array())) if (a.is_string()) elsewhere.insert(a.get<std::string>());
    if (cat.raw.contains("cyberPerks") && cat.raw["cyberPerks"].is_array())
        for (auto& p : cat.raw["cyberPerks"]) for (auto& e : p.value("effects", json::array())) if (e.value("type", "") == "ability") elsewhere.insert(e.value("ability", ""));
    auto conds = [](const std::vector<NatCond>& w) {
        json a = json::array();
        for (auto& c : w) a.push_back({ { "fn", c.fn }, { "value", c.value } });
        return a;
    };
    for (auto& [ab, def] : cat.abilities) {
        const json& nat = def.raw.contains("spec") && def.raw["spec"].is_object() ? def.raw["spec"].value("native", json()) : json();
        if (!nat.is_object() || !nat.contains("handler")) continue;
        std::string h = nat.value("handler", ""), act = nat.value("activation", "");
        // statsHandled params are already baked into the item's STATS (to_schema AV_PARAMS ->
        // tier stats), so the native perk must not carry them again (DESIGN "the handler must
        // not apply them again"; found live by the qaaudit: Smart Link gave its crit damage
        // twice, Fortified Ankles its carry weight, ... 2026-11-09).
        auto dropHandled = [&](json prm) {
            for (auto& k : nat.value("statsHandled", json::array()))
                if (k.is_string()) prm.erase(k.get<std::string>());
            return prm;
        };
        HandlerStatus st = StatusOf(h, act, dropHandled(nat.value("paramsT5", json::object())));
        handlers[h] = { { "family", st.family }, { "real", st.real }, { "partial", st.partial }, { "pending", st.pending }, { "na", st.na }, { "activation", act } };
        const bool modOnly = modTiers.count(ab) && !elsewhere.count(ab);
        for (int t = 1, cap = BakedTierCap(ab); t <= cap; ++t) {
            if (modOnly && !modTiers[ab].count(t)) continue;
            json prm = dropHandled(NativeParamsFor(nat, t));
            NativeFamily f = FamilyOf(h, act, prm);
            NativeEffects e = EffectsFor(h, f, prm);
            if (e.Empty()) continue;
            json avs = json::array(), entries = json::array();
            for (auto& a : e.avs) avs.push_back({ { "av", a.av }, { "value", std::round(a.value * 10000) / 10000 }, { "when", conds(a.when) } });
            for (auto& x : e.entries)
                entries.push_back({ { "entry", x.entry }, { "value", std::round(x.value * 10000) / 10000 }, { "weaponAny", x.weaponAny },
                                    { "weaponNone", x.weaponNone }, { "targetAny", x.targetAny }, { "when", conds(x.when) }, { "spell", x.spell },
                                    { "distMin", x.distMin }, { "distMax", x.distMax } });
            perks[TimedPerkId(ab, t)] = { { "ability", ab }, { "name", def.name }, { "handler", h }, { "family", FamilyLabel(f) }, { "tier", t },
                                          { "timed", PerkIsTimed(f) },
                                          { "fx", { { "kind", e.fx.kind }, { "magnitude", std::round(e.fx.magnitude * 10000) / 10000 }, { "areaFeet", e.fx.areaFeet },
                                                    { "when", conds(e.fxWhen) } } },
                                          { "avs", avs }, { "entries", entries } };
            // Kerenzikov + Kerenzikov Boost (batch 7): one perk per Kerenzikov tier with the boost's slow_bonus_pct added to the slow
            // (CP: "+15% time dilation"). The boost's own tier is taken as Kerenzikov's (ESTIMATE: a 5x5 tier matrix is not worth
            // 50 ESL records). Engine::Trigger swaps this perk in while the boost is installed.
            if (h == "ns_kerenzikov")
                for (auto& [bab, bdef] : cat.abilities) {
                    const json& bn = bdef.raw.contains("spec") && bdef.raw["spec"].is_object() ? bdef.raw["spec"].value("native", json()) : json();
                    if (!bn.is_object() || bn.value("handler", "") != "fc_kerenzikov_boost") continue;
                    double bonus = Num(NativeParamsFor(bn, t), "slow_bonus_pct").value_or(0), slow = Num(prm, "slow_pct").value_or(50);
                    double mag = std::clamp(1.0 - (slow + bonus) / 100.0, 0.05, 1.0);
                    perks[TimedPerkId(ab + "_boosted", t)] = { { "ability", ab + "_boosted" }, { "name", def.name + " + Boost" }, { "handler", h }, { "family", FamilyLabel(f) },
                                                               { "tier", t }, { "timed", true },
                                                               { "fx", { { "kind", "slowtime" }, { "magnitude", std::round(mag * 10000) / 10000 }, { "areaFeet", 0 }, { "when", json::array() } } },
                                                               { "avs", json::array() }, { "entries", json::array() } };
                    break;
                }
        }
    }
    // Ping (quickhack): a 30 m detect perk the hack key starts, like the Kiroshi scan.
    if (cat.raw.contains("quickhacks") && cat.raw["quickhacks"].is_array())
        for (auto& q : cat.raw["quickhacks"])
            if (q.value("id", "") == "qh_ping")
                perks[TimedPerkId("qh_ping", 1)] = { { "ability", "qh_ping" }, { "name", q.value("name", "Ping") }, { "handler", "quickhack" }, { "family", "deck" },
                                                     { "tier", 1 }, { "timed", true }, { "fx", { { "kind", "detect" }, { "magnitude", 1 }, { "areaFeet", 98 } } },
                                                     { "avs", json::array() }, { "entries", json::array() } };
    bool crit = cat.FindStat("critChance") && cat.FindStat("critChance")->av == "CW_CritChance";
    return { { "_comment", "Generated by cwtest --native-effects from the catalog (core/cw_effects.cpp). espgen bakes it. Do not edit." },
             { "chromeCrit", crit }, { "perks", perks }, { "handlers", handlers } };
}

// A chance the shop STATES and a perk entry carries verbatim must be a whole percent, or shown != applied (see
// EngineChancePct). `problems` are the abilities ruled on (Stabber: the visible lie); `warnings` are the same defect
// in other owners' numbers (arm bleed, arm-mod status, Handle Wrap, Visual Cortex's four distance bands), reported and not
// yet failed because their numbers come from the tier ladder, which is not this branch's to change.
EngineChanceReport EngineChanceProblems(const Catalog& cat)
{
    EngineChanceReport r;
    auto fmt = [](double v) { std::ostringstream o; o << v; return o.str(); };
    for (auto& [ab, def] : cat.abilities) {
        const json& nat = def.raw.contains("spec") && def.raw["spec"].is_object() ? def.raw["spec"].value("native", json()) : json();
        if (!nat.is_object() || !nat.contains("handler")) continue;
        const std::string h = nat.value("handler", "");
        for (int t = 1; t <= 7; ++t) {
            if (!nat.contains("paramsT" + std::to_string(t))) continue;   // only tiers the catalog states a number for
            const json p = NativeParamsFor(nat, t);
            auto check = [&](const char* key, double stated, bool ruled, const char* what) {
                const double applied = EngineChancePct(stated);
                if (std::fabs(applied - stated) < 1e-9) return;
                (ruled ? r.problems : r.warnings).push_back(ab + " T" + std::to_string(t) + ": " + key + " " + what + " " + fmt(stated) + "% but the engine's whole roll applies " + fmt(applied) + "%");
            };
            if (h == "ns_stabber" || h == "ha_handle_wrap") { if (auto v = Num(p, "crit_chance_pct")) check("crit_chance_pct", *v, h == "ns_stabber", "states"); }
            if (auto v = Num(p, "bleed_chance_pct"); v && *v < 100) check("bleed_chance_pct", *v, false, "states");
            if (auto v = Num(p, "status_chance_pct"); v && *v < 100) check("status_chance_pct", *v, false, "states");
            if (h == "ns_visual_cortex")
                if (auto v = Num(p, "crit_chance_pct_max")) {   // four distance bands of max / 4 each (EffectsFor)
                    std::string stated, applied;
                    bool differs = false;
                    for (int k = 1; k <= 4; ++k) {
                        const double st = *v * k / 4, ap = EngineChancePct(st);
                        differs = differs || std::fabs(ap - st) > 1e-9;
                        stated += (k > 1 ? " / " : "") + fmt(st);
                        applied += (k > 1 ? " / " : "") + fmt(ap);
                    }
                    if (differs) r.warnings.push_back(ab + " T" + std::to_string(t) + ": crit_chance_pct_max " + fmt(*v) + " over four distance bands states " + stated
                                                      + "% but the engine's whole roll applies " + applied + "%");
                }
        }
    }
    return r;
}

}  // namespace cw
