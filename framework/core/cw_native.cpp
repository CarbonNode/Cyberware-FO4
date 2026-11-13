// cw_native.cpp - the native-ability runtime: which C++ handler family a catalog ability belongs to, what a hotkey,
// a kill, a hit or a tick does, cooldowns, and which TIMED perks are on right now.
//
// Families (v1, the lead's first set, 2026-11-08):
//   timeslow   os_sandy_*                 hotkey: Cyberware.esp perk CW_Nat_<ability>_T<tier> (constant SlowTime ability,
//                                         magnitude = world speed) for duration_s, then cooldown_s. toggle, kill_extend_s,
//                                         kill_refund_pct; mod_sandy_overclocked (+1.5 s), mod_sandy_heatsink (-4 s cd)
//   camo       it_optical_camo            hotkey: constant Chameleon ability perk for duration_s, then cooldown_s
//   autocamo   perk_emergency_cloaking    tick: camo fires by itself below 30% health when it is ready
//   scan       face_kiroshi_* (hotkey)    hotkey: constant detect-cloak ability perk (radius from enemy_m/turret_m/radius_m)
//   onkill     params.on = kill|melee_kill: hp_pct / ap_pct restore, cooldown_cut_pct (Axolotl, Newton)
//   onhit      ci_electromag_recycler, ci_feedback_circuit: energy-weapon hits restore hp_pct / ap_pct
// Second set (2026-11-08, the lead's "rest of the 152"):
//   passive    always/equip with translatable params: CW_Nat perk on while installed (cw_effects.cpp decides what it holds)
//   buff       timed CW_Nat perk: Berserk on the OS key; Tyrosine (sneak kill), Adrenaline Converter / Adreno Trigger
//              (combat start), Defenzikov (Kerenzikov ends), Countershell (HP drop in a window), Fury (over-capacity
//              kill roll), Jenkins' Tendons (sprint start)
//   condslow   time slow on a condition: Reflex Tuner / Revulsor (HP below 25%), Kerenzikov (aim while/just after sprinting)
//   heal       Blood Pump on the heal key: flat heal + heal over time
//   secondheart, biomonitor: HP checks on every tick and on every hit the player takes
//   jump       le_* legs: the plugin's character-controller code; the passive part (CarryWeight, fall damage) is a perk
//   arms       ar_mantis / ar_gorilla / ar_monowire / ar_pls: the plugin's arms:: handlers (PLS: charges + recharge, launcher visual)
//   armmod     arm mods (FRAMEWORK params from CP, 2026-11-08): perk entries on the arm WEAP keyword + plugin runtime
//   deck, ram  Cyberdeck RAM pool + quickhacks (Quickhack below): max = deck ram_max + chips' ram_max + the ram stat;
//              regen 0.25/s + the ramRegen stat; Camillo / RAM Reallocator refill at a threshold; Memory Boost on kills,
//              RAM Recoup on hits; the hack key uploads the selected quickhack (hack_next cycles) to the aimed-at hostile
// Everything else is a logged stub in the plugin. The timed perks go through the same reconcile as the stat perks.
#include "cw_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cw {

namespace {
double P(const json& p, const char* k, double def = 0)
{
    auto it = p.find(k);
    return (it != p.end() && it->is_number()) ? it->get<double>() : def;
}

bool PB(const json& p, const char* k)
{
    auto it = p.find(k);
    return it != p.end() && it->is_boolean() && it->get<bool>();
}
const char* FamilyName(NativeFamily f)
{
    switch (f) {
    case NativeFamily::TimeSlow: return "timeslow";
    case NativeFamily::Camo: return "camo";
    case NativeFamily::AutoCamo: return "autocamo";
    case NativeFamily::Scan: return "scan";
    case NativeFamily::OnKill: return "onkill";
    case NativeFamily::OnHit: return "onhit";
    case NativeFamily::Modifier: return "modifier";
    case NativeFamily::Passive: return "passive";
    case NativeFamily::Buff: return "buff";
    case NativeFamily::CondSlow: return "condslow";
    case NativeFamily::Heal: return "heal";
    case NativeFamily::SecondHeart: return "secondheart";
    case NativeFamily::Biomonitor: return "biomonitor";
    case NativeFamily::Jump: return "jump";
    case NativeFamily::Arms: return "arms";
    case NativeFamily::Deck: return "deck";
    case NativeFamily::Ram: return "ram";
    case NativeFamily::ArmMod: return "armmod";
    case NativeFamily::Gate: return "gate";
    case NativeFamily::Hurt: return "hurt";
    case NativeFamily::WeaponMod: return "weaponmod";
    case NativeFamily::Hud: return "hud";
    case NativeFamily::Runtime: return "runtime";
    default: return "stub";
    }
}
}  // namespace

NativeFamily FamilyOf(const std::string& handler, const std::string& activation, const json& params)
{
    auto pre = [&](const char* p) { return handler.rfind(p, 0) == 0; };
    if (pre("os_sandy_")) return NativeFamily::TimeSlow;
    if (handler == "it_optical_camo") return NativeFamily::Camo;
    if (handler == "perk_emergency_cloaking") return NativeFamily::AutoCamo;
    if (pre("face_kiroshi_") && activation == "hotkey") return NativeFamily::Scan;
    if (handler == "mod_sandy_overclocked" || handler == "mod_sandy_heatsink" || handler == "mod_berserk_extended" || handler == "mod_berserk_chained")
        return NativeFamily::Modifier;
    if (handler == "ci_electromag_recycler" || handler == "ci_feedback_circuit") return NativeFamily::OnHit;
    if (params.is_object() && params.contains("on") && params["on"].is_string()) {
        std::string on = params["on"].get<std::string>();
        if ((on == "kill" || on == "melee_kill") &&
            (params.contains("hp_pct") || params.contains("ap_pct") || params.contains("cooldown_cut_pct") || params.contains("ram")))
            return NativeFamily::OnKill;
    }
    // ABILITIES (2026-11-08): Fallout-native chrome driven by the plugin (hack key context on robots / turrets, follower focus)
    if (handler == "fc_robco_override" || handler == "fc_core_overload" || handler == "fc_companion_uplink") return NativeFamily::Runtime;
    if (handler == "mod_optics_apex") return NativeFamily::Hud;
    if (handler == "perk_jailbreak") return NativeFamily::Gate;
    if (handler == "ci_microrotors") return NativeFamily::WeaponMod;
    if (handler == "sk_rara_avis" || handler == "ns_atomic_sensors" || handler == "im_metabolic_editor" || handler == "ha_microgenerator") return NativeFamily::Runtime;   // plugin ticks / hits
    if (handler == "face_kiroshi_basic" || handler == "face_kiroshi_stalker" || handler == "mod_optics_threat_detector" || handler == "mod_optics_target_analysis") return NativeFamily::Hud;
    if (handler == "it_carapace" || handler == "it_painducer" || handler == "it_nano_plating" || handler == "it_shock_n_awe" || handler == "it_rangeguard")
        return NativeFamily::Hurt;
    // a handler whose only params the catalog itself consumes (Kiroshi Mk.1-3: mod_slots open the optics mod slots)
    if (params.is_object() && !params.empty() && std::all_of(params.items().begin(), params.items().end(), [](auto kv) { return kv.key() == "mod_slots"; }))
        return NativeFamily::Gate;
    if (handler == "perk_launch_capacity_override" || handler == "perk_limiter_removal" || handler == "perk_spatial_mapping" || handler == "perk_data_tunneling" ||
        handler == "perk_vulnerability_analytics" || handler == "perk_machine_learning")
        return NativeFamily::ArmMod;   // relic arm perks: plugin runtime (PLS charges, slam knockdown, leap cripple, Monowire hack spread)
    if (!NativeTrigger(handler).empty())
        return (handler == "ns_reflex_tuner" || handler == "ns_revulsor" || handler == "ns_kerenzikov" || handler == "perk_sensory_protocol" || handler == "ns_synaptic")
                   ? NativeFamily::CondSlow : NativeFamily::Buff;
    if (handler == "ci_blood_pump") return NativeFamily::Heal;
    if (handler == "ci_second_heart") return NativeFamily::SecondHeart;
    if (handler == "ci_biomonitor") return NativeFamily::Biomonitor;
    if (handler == "le_reinforced_tendons" || handler == "le_fortified_ankles" || handler == "le_leeroy") return NativeFamily::Jump;
    if (handler == "ar_mantis" || handler == "ar_gorilla" || handler == "ar_monowire" || handler == "ar_pls") return NativeFamily::Arms;
    if (pre("mod_mantis_") || pre("mod_gorilla_") || pre("mod_mono_") || pre("mod_pls_")) return NativeFamily::ArmMod;   // Rin3U: OnKill above
    if (pre("os_deck_")) return NativeFamily::Deck;
    static const std::set<std::string> ram = { "fc_bioconductor", "fc_cox2", "fc_exdisk", "fc_camillo", "fc_ram_reallocator", "fc_self_ice", "sk_ram_recoup", "ns_deep_field" };
    if (ram.count(handler)) return NativeFamily::Ram;
    if ((activation == "always" || activation == "equip" || activation == "condition") && !EffectsFor(handler, NativeFamily::Passive, params).Empty())
        return NativeFamily::Passive;
    return NativeFamily::Stub;
}

std::string NativeTrigger(const std::string& h)
{
    if (h.rfind("os_berserk_", 0) == 0) return "key:os";
    static const std::map<std::string, std::string> m = {
        { "ns_tyrosine", "sneak_kill" }, { "ns_adrenaline_converter", "combat_start" }, { "ns_adreno_trigger", "combat_start" },
        { "it_defenzikov", "kerenzikov_end" }, { "it_countershell", "hp_drop" }, { "perk_edgerunner_fury", "kill_overcap" },
        { "le_jenkins_tendons", "sprint_start" }, { "sk_universal_booster", "stimpak" }, { "ns_reflex_tuner", "hp_below" }, { "ns_revulsor", "hp_below" }, { "ns_kerenzikov", "aim_sprint" },
        { "perk_sensory_protocol", "sneak_detected" },
        { "fc_kerenzikov_boost", "kerenzikov_start" },   // runs alongside Kerenzikov
        { "face_bisf", "key:pacify" },   // BISF: the pacify key (plugin: StopCombat on the hostile humans around), 25 min cooldown
        { "ns_synaptic", "combat_start" },   // CP: an enemy starts detecting you (FO4: combat starts)
    };
    auto it = m.find(h);
    return it == m.end() ? "" : it->second;
}

std::string FamilyLabel(NativeFamily f) { return FamilyName(f); }

// The four Kiroshi scans get a baked tier 6 because their range is a field on the detect SPELL, not a value the DLL
// can apply to an actor (cw_native.cpp SpellSpecFor; the SPLIT doc's "scan params are pure DLL" was wrong). 8 records,
// The lead's option 2, 2026-11-10: BOTH tiers, 16 records (a SPEL and a PERK per ability-tier, the shape
// tiers 1-5 already use), ids 0xF08-0xF17, exec line 1,735. His first figure of 8 came from my first count, which
// missed that each ability-tier is two records; he took the real number over his own.
int BakedTierCap(const std::string& ability)
{
    static const std::set<std::string> kScanT7 = { "ab_face_kiroshi_clairvoyant", "ab_face_kiroshi_doomsayer",
                                               "ab_face_kiroshi_oracle", "ab_face_kiroshi_sentry" };
    return kScanT7.count(ability) ? 7 : 5;
}

std::string TimedPerkId(const std::string& ability, int tier) { return "CW_Nat_" + ability + "_T" + std::to_string(std::clamp(tier, 1, BakedTierCap(ability))); }

// The timed effect a hotkey ability gets at a tier (what espgen bakes into CW_Nat_<ability>_T<tier>).
NativeSpellSpec SpellSpecFor(NativeFamily f, const json& params)
{
    NativeSpellSpec s;
    if (f == NativeFamily::TimeSlow) { s.kind = "slowtime"; s.magnitude = std::clamp(P(params, "time_scale", 0.5), 0.05, 1.0); }
    else if (f == NativeFamily::Camo) { s.kind = "chameleon"; s.magnitude = 99; }
    else if (f == NativeFamily::Scan) {
        s.kind = "detect";
        double m = std::max({ P(params, "enemy_m"), P(params, "turret_m"), P(params, "radius_m") });
        s.magnitude = 1;
        s.areaFeet = (int)std::lround((m > 0 ? m : 17.5) * 3.28084);
    }
    return s;
}

// --------------------------------------------------------------------------------------------- runtime
const NativeAbility* Engine::ActiveNative(NativeFamily f, const Derived& d) const
{
    const NativeAbility* best = nullptr;
    for (auto& n : d.desired.native)
        if (FamilyOf(n.handler, n.activation, n.params) == f && (!best || n.tier > best->tier)) best = &n;
    return best;
}

json Engine::NativeKey(const std::string& action)
{
    Derived d = Compute(state_);
    if (action == "hack") return Quickhack(d);
    if (action == "hack_next") {
        auto q = DeckQuickhacks(d);
        if (q.empty()) return { { "ok", false }, { "message", "no cyberdeck installed" } };
        const std::string id = q[(HackSel(q) + 1) % q.size()];
        state_.hackSelected = id;          // by id, so a later reorder keeps THIS hack selected (D1)
        std::string name = id;
        int cost = 0;
        for (auto& h : cat_.raw.value("quickhacks", json::array()))
            if (h.value("id", "") == id) { name = h.value("name", name); cost = (int)std::lround(EffectiveHackRam(d, h)); }
        Notify("Quickhack: " + name + " (" + std::to_string(cost) + " RAM)");
        Sync();                            // the HUD's selected entry follows at once
        return { { "ok", true }, { "quickhack", id } };
    }
    NativeFamily f = action == "os" ? NativeFamily::TimeSlow : action == "camo" ? NativeFamily::Camo : action == "scan" ? NativeFamily::Scan
                   : action == "heal" ? NativeFamily::Heal : action == "pacify" ? NativeFamily::Buff : NativeFamily::Stub;
    if (f == NativeFamily::Stub) return { { "ok", false }, { "message", "unknown native action '" + action + "'" } };
    const NativeAbility* n = action == "pacify" ? nullptr : ActiveNative(f, d);   // pacify: only the key:pacify handler, never any buff
    if (!n && (action == "os" || action == "pacify"))   // the OS slot holds one piece: a Sandevistan or a Berserk; BISF answers the pacify key
        for (auto& m : d.desired.native)
            if (NativeTrigger(m.handler) == "key:" + action && (!n || m.tier > n->tier)) n = &m;
    if (n && f == NativeFamily::Heal) {
        json out = Trigger(*n, d, false);
        if (out.value("ok", false)) {
            if (double heal = P(n->params, "heal"); heal && env_) env_->RestoreActorValue("Health", heal);
            timers_[n->ability].hot = P(n->params, "hot");
        }
        return out;
    }
    if (!n) return { { "ok", false }, { "message", std::string("no installed cyberware answers the ") + action + " key" } };
    return Trigger(*n, d, false);
}

json Engine::Trigger(const NativeAbility& n, const Derived& d, bool automatic)
{
    const int64_t now = env_ ? env_->NowMs() : 0;
    NativeTimer& t = timers_[n.ability];
    std::string name = cat_.abilities.count(n.ability) ? cat_.abilities.at(n.ability).name : n.ability;
    NativeFamily f = FamilyOf(n.handler, n.activation, n.params);
    // Defaults: the catalog's text where a param is missing (Defenzikov 4 s, Adreno Trigger 10 s, Jenkins 5 s); event
    // buffs without a cooldown_s re-arm at once. Heal: the heal-over-time window. Second Heart / Biomonitor: instant.
    double defDur = 5, defCd = 30;
    if (f == NativeFamily::Buff && NativeTrigger(n.handler) != "key:os") defCd = 0;
    if (n.handler == "it_defenzikov") defDur = 4;
    if (n.handler == "face_bisf") defDur = 0;   // instant: the plugin stops the combat; only the cooldown runs
    if (n.handler == "ns_adreno_trigger") defDur = 10;
    if (f == NativeFamily::Heal) defDur = P(n.params, "hot_s", 0);
    if (f == NativeFamily::SecondHeart) { defDur = 0; defCd = 240; }
    if (f == NativeFamily::Biomonitor) { defDur = 0; defCd = 10; }
    double dur = P(n.params, "duration_s", defDur), cd = P(n.params, "cooldown_s", defCd);
    const bool berserk = n.handler.rfind("os_berserk_", 0) == 0;
    for (auto& m : d.desired.native) {
        if (f == NativeFamily::TimeSlow && m.handler == "mod_sandy_overclocked") dur += 1.5;
        if (f == NativeFamily::TimeSlow && m.handler == "mod_sandy_heatsink") cd = std::max(0.0, cd - 4.0);
        if (berserk && m.handler == "mod_berserk_extended") dur += 4.0;
        if (berserk && m.handler == "mod_berserk_chained") cd = std::max(0.0, cd - 4.5);
    }
    json out = { { "ability", n.ability }, { "handler", n.handler }, { "tier", n.tier } };
    if (t.activeUntil > now) {
        if (f == NativeFamily::TimeSlow && PB(n.params, "toggle") && !automatic) {
            // Apogee: cancelling refunds the unused time to the cooldown.
            double used = (double)(now - t.startedAt) / std::max<int64_t>(1, t.activeUntil - t.startedAt);
            t.activeUntil = now;
            t.readyAt = now + (int64_t)std::llround(cd * 1000.0 * std::clamp(used, 0.0, 1.0));
            Notify(name + " off");
            out["ok"] = true; out["state"] = "cancelled";
            Sync();
            return out;
        }
        out["ok"] = false; out["state"] = "active";
        return out;
    }
    if (t.readyAt > now) {
        double left = (t.readyAt - now) / 1000.0;
        if (!automatic) Notify(name + " recharging: " + std::to_string((int)std::ceil(left)) + " s");
        out["ok"] = false; out["state"] = "cooldown"; out["secondsLeft"] = left;
        return out;
    }
    // D11 (the lead, 2026-11-10): one time-slow at a time. Two SlowTime effects race in the engine (cw_effects.cpp), so a Sandevistan or
    // a conditional slow (Kerenzikov, Reflex Tuner, ...) that would start while another runs refuses, says why, and keeps its cooldown.
    if (f == NativeFamily::TimeSlow || f == NativeFamily::CondSlow) {
        for (auto& m : d.desired.native) {
            if (m.ability == n.ability) continue;
            const NativeFamily mf = FamilyOf(m.handler, m.activation, m.params);
            if (mf != NativeFamily::TimeSlow && mf != NativeFamily::CondSlow) continue;
            auto it = timers_.find(m.ability);
            if (it == timers_.end() || it->second.activeUntil <= now) continue;
            const std::string other = cat_.abilities.count(m.ability) ? cat_.abilities.at(m.ability).name : m.ability;
            Notify(name + " refused: " + other + " is already slowing time");
            if (env_) env_->Log("native: " + n.handler + " refused, " + m.handler + " is already slowing time");
            out["ok"] = false; out["state"] = "slow-active"; out["by"] = m.ability;
            return out;
        }
    }
    // D7: Kerenzikov Boost's buff (VATS AP -100%) lasts exactly as long as the Kerenzikov it rides on, not the Buff family's 5 s default.
    if (n.handler == "fc_kerenzikov_boost" && !(n.params.is_object() && n.params.contains("duration_s")))
        for (auto& m : d.desired.native)
            if (m.handler == "ns_kerenzikov")
                if (auto it = timers_.find(m.ability); it != timers_.end() && it->second.activeUntil > now) dur = (it->second.activeUntil - now) / 1000.0;
    t.startedAt = now;
    t.activeUntil = now + (int64_t)std::llround(dur * 1000.0);
    t.readyAt = t.activeUntil + (int64_t)std::llround(cd * 1000.0);
    t.tier = n.tier;
    t.perk = PerkIsTimed(f) ? TimedPerkId(n.ability, n.tier) : "";
    // Kerenzikov with a Kerenzikov Boost installed: the boosted perk (cw_effects.cpp NativeEffectsJson), a stronger slow
    if (n.handler == "ns_kerenzikov")
        for (auto& m : d.desired.native) if (m.handler == "fc_kerenzikov_boost") { t.perk = TimedPerkId(n.ability + "_boosted", n.tier); break; }
    t.hot = 0;
    t.wasOn = dur > 0;   // so the tick that sees it end reports it (and fires "kerenzikov_end")
    Notify(dur > 0 ? name + (automatic ? " (auto)" : "") + ": " + std::to_string((int)std::lround(dur)) + " s" : name);
    if (env_) env_->Log("native: " + n.handler + " fired t" + std::to_string(n.tier) + " for " + std::to_string(dur) + " s, cooldown " + std::to_string(cd) + " s -> " + t.perk);
    out["ok"] = true; out["state"] = "fired"; out["durationS"] = dur; out["cooldownS"] = cd; out["perk"] = t.perk;
    Sync();
    return out;
}

void Engine::CutCooldowns(double pct)
{
    const int64_t now = env_ ? env_->NowMs() : 0;
    for (auto& [ab, t] : timers_)
        if (t.activeUntil <= now && t.readyAt > now) t.readyAt = now + (int64_t)std::llround((t.readyAt - now) * (1.0 - pct / 100.0));
}

double Engine::DeckParam(const std::string& key)
{
    Derived d = Compute(state_);
    double v = 0;
    for (auto& n : d.desired.native) if (n.handler.rfind("os_deck_", 0) == 0) v += P(n.params, key.c_str());
    return v;
}

void Engine::NativeKill(bool melee, bool sneak)
{
    if (!env_) return;
    if (sneak) NativeEvent("sneak_kill");
    Derived d = Compute(state_);
    // Arasaka Mk.4+ (TweakDB RegenerateRamAfterStealthTakedownAbility): RAM back after a takedown = a sneak-attack kill here
    if (sneak && ram_ >= 0)
        for (auto& n : d.desired.native)
            if (double r = P(n.params, "ram_on_takedown"); r && n.handler.rfind("os_deck_", 0) == 0) { ram_ = std::min(RamMax(d), ram_ + r); env_->Log("native: " + n.handler + " takedown +" + std::to_string(r) + " RAM"); }
    if (d.capacityUsed > d.capacityMax)   // Edgerunner Fury: a chance per kill while over capacity
        for (auto& n : d.desired.native)
            if (NativeTrigger(n.handler) == "kill_overcap") {
                double roll = (double)(NextRand(state_) % 10000) / 100.0;
                if (roll < P(n.params, "chance_pct", 2)) Trigger(n, d, true);
            }
    const int64_t now = env_->NowMs();
    for (auto& n : d.desired.native) {
        NativeFamily f = FamilyOf(n.handler, n.activation, n.params);
        if (f == NativeFamily::OnKill) {
            std::string on = n.params.value("on", "kill");
            if (on == "melee_kill" && !melee) continue;
            if (double hp = P(n.params, "hp_pct")) env_->RestoreActorValuePct("Health", hp);
            if (double ap = P(n.params, "ap_pct")) env_->RestoreActorValuePct("ActionPoints", ap);
            if (double cut = P(n.params, "cooldown_cut_pct")) CutCooldowns(cut);
            if (double ram = P(n.params, "ram"); ram && ram_ >= 0) ram_ = std::min(RamMax(d), ram_ + ram);
            env_->Log("native: " + n.handler + " on " + on);
        } else if (f == NativeFamily::TimeSlow) {
            auto it = timers_.find(n.ability);
            if (it == timers_.end() || it->second.activeUntil <= now) continue;
            if (double ext = P(n.params, "kill_extend_s")) { it->second.activeUntil += (int64_t)std::llround(ext * 1000); it->second.readyAt += (int64_t)std::llround(ext * 1000); }
            if (double ref = P(n.params, "kill_refund_pct")) {
                double cd = P(n.params, "cooldown_s", 30);
                it->second.readyAt -= (int64_t)std::llround(cd * 1000 * ref / 100.0);
            }
        }
    }
}

void Engine::NativeHit(const std::vector<std::string>& weaponKeywords, bool melee)
{
    if (!env_) return;
    Derived d = Compute(state_);
    const int64_t now = env_->NowMs();
    if (melee)   // Berserk (Moore): melee hits heal while it runs
        for (auto& n : d.desired.native)
            if (double h = P(n.params, "melee_heal_pct"))
                if (auto it = timers_.find(n.ability); it != timers_.end() && it->second.activeUntil > now) env_->RestoreActorValuePct("Health", h);
    static const std::vector<std::string> kEnergy = { "WeaponTypeLaser", "WeaponTypePlasma", "WeaponTypeGauss", "WeaponTypeEnergy" };
    for (auto& n : d.desired.native) {
        if (FamilyOf(n.handler, n.activation, n.params) != NativeFamily::OnHit) continue;
        std::vector<std::string> want = kEnergy;
        if (n.params.contains("on_hit_keyword") && n.params["on_hit_keyword"].is_array()) {
            want.clear();
            for (auto& k : n.params["on_hit_keyword"]) if (k.is_string()) want.push_back(k.get<std::string>());
        }
        bool match = false;
        for (auto& w : weaponKeywords) for (auto& k : want) if (w == k) match = true;
        if (!match) continue;
        if (double hp = P(n.params, "hp_pct")) env_->RestoreActorValuePct("Health", hp);
        if (double ap = P(n.params, "ap_pct")) env_->RestoreActorValuePct("ActionPoints", ap);
    }
}

bool Engine::NativeTick()
{
    if (!env_) return false;
    const int64_t now = env_->NowMs();
    const double dt = lastTickMs_ ? std::clamp((now - lastTickMs_) / 1000.0, 0.0, 1.0) : 0.0;
    lastTickMs_ = now;
    bool changed = false;
    std::vector<std::string> ended;
    for (auto& [ab, t] : timers_) {
        bool on = t.activeUntil > now;
        if (t.wasOn && !on) {
            changed = true; ended.push_back(ab);
            std::string nm = cat_.abilities.count(ab) ? cat_.abilities.at(ab).name : ab;
            if (!cat_.abilities.count(ab))   // a quickhack's timer (Ping): its catalog name, not the raw id (D5)
                for (auto& h : cat_.raw.value("quickhacks", json::array())) if (h.value("id", "") == ab) nm = h.value("name", ab);
            Notify(nm + " ended");
        }
        t.wasOn = on;
        if (on && t.hot > 0 && dt > 0) env_->RestoreActorValue("Health", t.hot * dt);   // Blood Pump heal over time
        if (!t.cue.empty() && !on && t.readyAt <= now) {             // an ability that owes a "ready" cue: its cooldown is over
            env_->AbilityCue(t.cue, "ready");
            Notify((cat_.abilities.count(ab) ? cat_.abilities.at(ab).name : ab) + " ready");
            t.cue.clear();
            changed = true;
        }
    }
    Derived d = Compute(state_);
    for (auto& ab : ended)
        for (auto& n : d.desired.native)
            if (n.ability == ab && n.handler == "ns_kerenzikov") NativeEvent("kerenzikov_end");
    // Berserk (Biodyne): unlimited AP while it runs
    for (auto& n : d.desired.native)
        if (n.params.is_object() && PB(n.params, "ap_infinite"))
            if (auto it = timers_.find(n.ability); it != timers_.end() && it->second.activeUntil > now) env_->RestoreActorValuePct("ActionPoints", 100);
    {   // D1 (QD R4): a player who never opens the loadout screen is still told that hacks are benched, and where.
        std::string deck;
        for (auto& n : d.desired.native)
            if (n.handler.rfind("os_deck_", 0) == 0) deck = n.handler;
        if (!deckSeen_) { deckSeen_ = true; lastDeck_ = deck; }   // the first tick after a load: record, do not announce
        else if (deck != lastDeck_) {
            lastDeck_ = deck;
            benchNotice_ = deck.empty() ? 0 : LoadoutBenched(d);
        }
        // Held until the player can actually read it: never behind the ripperdoc menu or during the install scene.
        if (benchNotice_ > 0 && !env_->UiBusy()) {
            // The screen's name is "Quickhack loadout" and the key is whatever is BOUND, never a literal (MENU/HUD).
            const std::string key = env_->SetupKeyName();
            Notify(std::to_string(benchNotice_) + (benchNotice_ == 1 ? " quickhack is benched" : " quickhacks are benched")
                   + (key.empty() ? ": open Quickhack loadout at a ripperdoc to choose which ones you carry."
                                  : ": press " + key + " and open Quickhack loadout to choose which ones you carry."));
            benchNotice_ = 0;
        }
    }
    // Combat start, sprint start, Kerenzikov (aim while sprinting or within 0.6 s of it)
    bool combat = env_->PlayerIs("combat"), sprint = env_->PlayerIs("sprinting");
    if (combat && !wasCombat_) {
        NativeEvent("combat_start");
        if (env_->PlayerIs("sneaking")) NativeEvent("sneak_detected");   // Sensory Protocol: detected while crouched
    }
    wasCombat_ = combat;
    if (sprint && now - lastSprintMs_ > 1000) NativeEvent("sprint_start");
    if (sprint) lastSprintMs_ = now;
    if (env_->PlayerIs("aiming") && now - lastSprintMs_ <= 600) NativeEvent("aim_sprint");
    // "Using a Stimpak" = the Stimpak count dropped since the last tick (drops/sales count too): Universal Booster fires,
    // Biomonitor's heal_bonus_pct adds that share of a Stimpak's heal (vanilla Stimpak ~40% of max HP, estimate).
    for (auto& n : d.desired.native)
        if (NativeTrigger(n.handler) == "stimpak" || (FamilyOf(n.handler, n.activation, n.params) == NativeFamily::Biomonitor && P(n.params, "heal_bonus_pct") > 0)) {
            int c = env_->CountItem("@Fallout4.esm|023736");
            if (lastStimpaks_ >= 0 && c < lastStimpaks_) {
                NativeEvent("stimpak");
                for (auto& b : d.desired.native)
                    if (FamilyOf(b.handler, b.activation, b.params) == NativeFamily::Biomonitor && P(b.params, "heal_bonus_pct") > 0)
                        env_->RestoreActorValuePct("Health", 40.0 * P(b.params, "heal_bonus_pct") / 100.0 * (lastStimpaks_ - c));
            }
            lastStimpaks_ = c;
            break;
        }
    if (ActiveNative(NativeFamily::AutoCamo, d))
        if (const NativeAbility* camo = ActiveNative(NativeFamily::Camo, d))
            if (env_->ActorValuePct("Health") < 30.0) {
                NativeTimer& t = timers_[camo->ability];
                if (t.activeUntil <= now && t.readyAt <= now) { Trigger(*camo, d, true); changed = false; }  // Trigger synced already
            }
    HpChecks(d);
    RamTick(d, dt);
    if (changed) Sync();
    return changed;
}

void Engine::NativeHurt()
{
    if (!env_) return;
    Derived d = Compute(state_);
    HpChecks(d);
    const int64_t now = env_->NowMs();
    for (auto& n : d.desired.native)   // RAM Recoup: being hit restores RAM, 2 s lockout
        if (double r = P(n.params, "ram_per_hit"); r && ram_ >= 0) {
            NativeTimer& t = timers_[n.ability];
            if (t.readyAt > now) continue;
            t.readyAt = now + 2000;
            ram_ = std::min(RamMax(d), ram_ + r);
        }
}

// Low-HP natives. Second Heart cannot intercept the killing blow itself (no damage hook): it refills HP the moment HP is
// at or below its threshold (10%), on a tick or on the hit that took it there.
void Engine::HpChecks(const Derived& d)
{
    const int64_t now = env_->NowMs();
    const double hp = env_->ActorValuePct("Health");
    hpHistory_.push_back({ now, hp });
    while (!hpHistory_.empty() && now - hpHistory_.front().t > 10000) hpHistory_.erase(hpHistory_.begin());
    auto ready = [&](const NativeAbility& n) { auto it = timers_.find(n.ability); return it == timers_.end() || (it->second.activeUntil <= now && it->second.readyAt <= now); };
    for (auto& n : d.desired.native) {
        NativeFamily f = FamilyOf(n.handler, n.activation, n.params);
        std::string trig = NativeTrigger(n.handler);
        if (f == NativeFamily::SecondHeart && hp > 0 && hp <= P(n.params, "threshold_pct", 10) && ready(n)) {
            if (Trigger(n, d, true).value("ok", false)) {
                env_->RestoreActorValuePct("Health", 100);
                env_->AbilityCue(n.handler, "trigger");          // AAA-GAPS row 25: the save was silent and invisible
                timers_[n.ability].cue = n.handler;              // and say so when it is ready again (NativeTick)
            }
        } else if (f == NativeFamily::Biomonitor && hp < P(n.params, "hp_threshold_pct", 35) && ready(n)) {
            const std::string stim = "@Fallout4.esm|023736";
            if (env_->CountItem(stim) > 0 && Trigger(n, d, true).value("ok", false)) env_->UseItem(stim);
        } else if (trig == "hp_below" && hp < 25.0 && ready(n)) {
            Trigger(n, d, true);
        } else if (trig == "hp_drop" && ready(n)) {
            double window = P(n.params, "window_s", 3) * 1000, loss = P(n.params, "loss_pct", 35), peak = hp;
            for (auto& smp : hpHistory_) if (now - smp.t <= window) peak = std::max(peak, smp.pct);
            if (peak - hp >= loss) Trigger(n, d, true);
        }
    }
}

void Engine::NativeEvent(const std::string& ev)
{
    if (!env_) return;
    Derived d = Compute(state_);
    const int64_t now = env_->NowMs();
    bool kerenzikov = false;
    for (auto& n : d.desired.native) {
        if (NativeTrigger(n.handler) != ev) continue;
        auto it = timers_.find(n.ability);
        if (it != timers_.end() && (it->second.activeUntil > now || it->second.readyAt > now)) continue;
        if (Trigger(n, d, true).value("ok", false) && n.handler == "ns_kerenzikov") kerenzikov = true;
    }
    if (kerenzikov) NativeEvent("kerenzikov_start");   // Kerenzikov Boost runs alongside it
}

void Engine::Notify(const std::string& text)
{
    if (env_) env_->Notify(text);
}

double Engine::RamMax(const Derived& d) const
{
    bool deck = false;
    double m = 0;
    for (auto& n : d.desired.native) {
        if (n.handler.rfind("os_deck_", 0) == 0) deck = true;
        m += P(n.params, "ram_max");
    }
    if (!deck) return 0;
    auto it = d.applied.find("ram");
    return std::max(0.0, m + (it != d.applied.end() ? it->second : 0.0));
}

// A virtual-key code as the player reads it. Only the keys this mod can actually be bound to are named; anything
// else comes back as "key 0xNN", which is still better on screen than a bare number.
std::string VkName(unsigned vk)
{
    if (vk == 0) return "";
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);          // F1..F24
    if (vk >= 0x30 && vk <= 0x39) return std::string(1, (char)vk);                      // 0..9
    if (vk >= 0x41 && vk <= 0x5A) return std::string(1, (char)vk);                      // A..Z
    switch (vk) {
        case 0x08: return "Backspace";
        case 0x09: return "Tab";
        case 0x0D: return "Enter";
        case 0x1B: return "Esc";
        case 0x20: return "Space";
        case 0x21: return "Page Up";
        case 0x22: return "Page Down";
        case 0x23: return "End";
        case 0x24: return "Home";
        case 0x25: return "Left";
        case 0x26: return "Up";
        case 0x27: return "Right";
        case 0x28: return "Down";
        case 0x2D: return "Insert";
        case 0x2E: return "Delete";
        case 0xBA: return ";";
        case 0xBB: return "=";
        case 0xBC: return ",";
        case 0xBD: return "-";
        case 0xBE: return ".";
        case 0xBF: return "/";
        case 0xC0: return "`";
        case 0xDB: return "[";
        case 0xDC: return "\\";
        case 0xDD: return "]";
        case 0xDE: return "'";
        default: break;
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "key 0x%02X", vk);
    return buf;
}

// D1 (docs/QUICKHACK-LOADOUT.md): the built-in default ranking - the order a deck's slots take when the player has
// chosen nothing. All 8 general hacks are in it, and all 9 catalog hacks are implemented in Quickhack below, so a
// hack only ever needs SLOTTING, never new code. A deck's fixed program is never part of a player's ranking.
static const std::vector<std::string>& DefaultHackOrder()
{
    static const std::vector<std::string> order = { "qh_short_circuit", "qh_reboot_optics", "qh_ping", "qh_overheat",
                                                    "qh_cyberware_malfunction", "qh_cripple_movement", "qh_contagion",
                                                    "qh_synapse_burnout" };
    return order;
}

// Blackwall Gateway is the Militech Canto's fixed program and is LOCKED on every other deck (spec section 3, QD R2):
// slottable anywhere would make a 12 RAM instant kill universal, which is a rebalance and out of D1's scope.
bool Engine::HackRankable(const std::string& id) { return id != "qh_blackwall_gateway"; }

// The player's EFFECTIVE ranking, and it is always TOTAL: what they stored (valid, rankable, de-duplicated), then
// every remaining rankable catalog hack in the built-in order. An empty stored list therefore IS today's order, so
// the first edit can neither empty nor scramble the bench (QD R1).
std::vector<std::string> Engine::LoadoutOrder(const State& s) const
{
    std::set<std::string> have;
    for (auto& h : cat_.raw.value("quickhacks", json::array())) have.insert(h.value("id", ""));
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto push = [&](const std::string& id) {
        if (id.empty() || !have.count(id) || !HackRankable(id)) return;   // an unknown stored id stays in State, skipped here
        if (!seen.insert(id).second) return;
        out.push_back(id);
    };
    for (auto& id : s.loadout) push(id);               // what the player chose, in their order
    for (auto& id : DefaultHackOrder()) push(id);      // then the rest, in the built-in order
    for (auto& h : cat_.raw.value("quickhacks", json::array())) push(h.value("id", ""));   // any hack the default order misses
    return out;
}

std::vector<std::string> Engine::DeckQuickhacks(const Derived& d) const { return DeckQuickhacks(state_, d); }

std::vector<std::string> Engine::DeckQuickhacks(const State& s, const Derived& d) const
{
    int slots = 0;
    std::string program;   // a deck's fixed program (the Canto's Blackwall Gateway) takes its first slot
    for (auto& n : d.desired.native)
        if (n.handler.rfind("os_deck_", 0) == 0) { slots = std::max(slots, (int)P(n.params, "qh_slots", 2)); if (n.params.is_object()) program = n.params.value("program", program); }
    if (slots <= 0) return {};
    std::set<std::string> have;
    for (auto& h : cat_.raw.value("quickhacks", json::array())) have.insert(h.value("id", ""));
    std::vector<std::string> out;
    if (!program.empty() && have.count(program)) out.push_back(program);
    // `use` caps the PLAYER's slots only: the program always keeps its own, so use 0 on a Canto leaves Blackwall alone.
    const int playerSlots = std::max(0, slots - (int)out.size());
    const int use = s.loadoutUse < 0 ? playerSlots : std::max(0, std::min(s.loadoutUse, playerSlots));
    int filled = 0;
    for (auto& q : LoadoutOrder(s)) {
        if (filled >= use) break;
        out.push_back(q);
        ++filled;
    }
    return out;
}

// How many ranked hacks this deck cannot hold: what the benched notice and the screen count (QD R4).
int Engine::LoadoutBenched(const Derived& d) const
{
    const std::vector<std::string> eq = DeckQuickhacks(state_, d), order = LoadoutOrder(state_);
    int slotted = 0;
    for (auto& q : order)
        if (std::find(eq.begin(), eq.end(), q) != eq.end()) ++slotted;
    return std::max(0, (int)order.size() - slotted);
}

// The lead's MN-28 ruling: ONE place produces the words for a hack this deck can never pay for, so the HUD's refusal
// and the screen's row cannot say different things. Empty when the hack is payable on this deck.
std::string Engine::HackUnfireableWhy(const Derived& d, const json& def) const
{
    const double mx = RamMax(d);
    if (mx <= 0) return "";                       // no deck: "no cyberdeck installed" is the right message, not this
    const double cost = EffectiveHackRam(d, def);
    if (cost <= mx + 1e-9) return "";
    return "needs " + std::to_string((int)std::lround(cost)) + " RAM, this deck holds " + std::to_string((int)std::floor(mx));
}

// The selection is an id, not an index, so reordering never moves the player's pick to a different hack (QD R5).
size_t Engine::HackSel(const std::vector<std::string>& q) const
{
    if (q.empty()) return 0;
    if (!state_.hackSelected.empty())
        for (size_t i = 0; i < q.size(); ++i)
            if (q[i] == state_.hackSelected) return i;   // the player's own pick is honoured even if unfireable
    // The stored pick is not in this deck (slots shrank, benched, use lowered): fall back to the first slot the deck
    // can actually pay for, never to a hack it could never fire (the lead's MN-28 ruling, guarantee 1). Only an
    // AUTOMATIC choice is filtered: a hack the player selected on purpose stays selected and refuses with words.
    const Derived d = Compute(state_);
    for (size_t i = 0; i < q.size(); ++i) {
        json def;
        for (auto& h : cat_.raw.value("quickhacks", json::array())) if (h.value("id", "") == q[i]) def = h;
        if (HackUnfireableWhy(d, def).empty()) return i;
    }
    return 0;   // every slot is unfireable: slot 1, and the press says why
}

void Engine::RamTick(const Derived& d, double dt)
{
    const int64_t now = env_->NowMs();
    // timed quickhack debuffs end (the damage is restored), damage over time ticks
    for (auto it = hackFx_.begin(); !hacksHeld_ && it != hackFx_.end();) {
        if (it->perSec > 0) env_->TargetAV(it->target, it->av, -it->perSec * dt);
        if (it->until <= now) {
            if (it->perSec <= 0) env_->TargetAV(it->target, it->av, it->amount);
            it = hackFx_.erase(it);
        } else ++it;
    }
    double mx = RamMax(d);
    if (upload_ && !hacksHeld_) {
        if (!env_->HackTargetValid(upload_->target, 60.0)) {   // dead, gone or out of reach: no effect, the RAM comes back
            ram_ = std::min(mx, ram_ + upload_->cost);
            env_->HackVisual("cancel", upload_->id, upload_->target, 0);
            Notify("Upload interrupted");
            upload_.reset();
        } else if (now >= upload_->end) {
            HackUpload u = *upload_;
            upload_.reset();
            Quickhack(d, &u);
        }
    }
    if (mx <= 0) { ram_ = -1; return; }
    if (ram_ < 0) ram_ = mx;
    auto it = d.applied.find("ramRegen");
    double deckRegen = 0;   // CP MemoryRegenRate x2 (DESIGN 3.5), TweakDB deck params
    for (auto& n : d.desired.native) if (n.handler.rfind("os_deck_", 0) == 0) deckRegen += P(n.params, "ram_regen");
    const double boost = now < ramBoostUntil_ ? ramBoost_ : 0.0;   // ARMORY ram_regen food
    ram_ = std::min(mx, ram_ + dt * (0.25 + deckRegen + boost + (it != d.applied.end() ? it->second : 0.0)));
    for (auto& n : d.desired.native) {   // Camillo / RAM Reallocator
        double th = P(n.params, "threshold_pct"), rs = P(n.params, "restore_pct");
        if (!th || !rs || ram_ > mx * th / 100.0) continue;
        NativeTimer& t = timers_[n.ability];
        if (t.readyAt > now) continue;
        t.readyAt = now + (int64_t)std::llround(P(n.params, "cooldown_s", 60) * 1000);
        ram_ = std::min(mx, ram_ + mx * rs / 100.0);
        Notify((cat_.abilities.count(n.ability) ? cat_.abilities.at(n.ability).name : n.ability) + ": RAM " + std::to_string((int)ram_) + "/" + std::to_string((int)mx));
    }
}

// The quickhacks (catalog "quickhacks", RAM cost from there). Damage x (1 + qhDamage stat %) x2 on a quickhack crit
// (Bioconductor / CoX-2 qh_crit_chance_pct), Short Circuit x2 vs robots/turrets/synths. Debuffs damage an AV for a
// time and restore it after; Overheat / Contagion are damage over time. Range 30 m x (1 + Deep Field range_pct).
const std::map<std::string, HackDebuff>& HackDebuffs()
{
    static const std::map<std::string, HackDebuff> deb = { { "qh_reboot_optics", { "Perception", 10, 12000 } },
                                                           { "qh_cyberware_malfunction", { "DamageResist", 30, 15000 } },
                                                           { "qh_cripple_movement", { "SpeedMult", 90, 8000 } } };
    return deb;
}

json Engine::Quickhack(const Derived& d, const HackUpload* done)
{
    if (!env_) return { { "ok", false } };
    auto q = DeckQuickhacks(d);
    if (q.empty()) return { { "ok", false }, { "message", "no cyberdeck installed" } };
    if (upload_ && !done) { Notify("Upload in progress"); return { { "ok", false }, { "state", "uploading" } }; }
    const std::string id = done ? done->id : q[HackSel(q)];
    json def;
    for (auto& h : cat_.raw.value("quickhacks", json::array())) if (h.value("id", "") == id) def = h;
    const std::string name = def.value("name", id);
    // The deck's CP bonuses (TweakDB, catalog paramsT<tier>): damage, DoT damage/duration, debuff duration, costs, spread.
    struct DeckMods { double dmg = 0, dot = 0, dotDur = 0, dur = 0, stealth = 0, ult = 0, spreadN = 0, spreadPct = 0, elec = 0, refreshM = 0, progTargets = 0, progSpreadM = 0; } dm;
    for (auto& n : d.desired.native)
        if (n.handler.rfind("os_deck_", 0) == 0) {
            dm.dmg += P(n.params, "qh_dmg_pct"); dm.dot += P(n.params, "qh_dot_dmg_pct"); dm.dotDur += P(n.params, "dot_duration_pct");
            dm.dur += P(n.params, "qh_duration_pct"); dm.stealth += P(n.params, "stealth_cost_red"); dm.ult += P(n.params, "ultimate_cost_red");
            dm.spreadN += P(n.params, "spread_targets"); dm.spreadPct += P(n.params, "spread_dist_pct"); dm.elec += P(n.params, "elec_dmg_pct");
            dm.refreshM = std::max(dm.refreshM, P(n.params, "qh_refresh_dot_m")); dm.progTargets = std::max(dm.progTargets, P(n.params, "program_targets"));
            dm.progSpreadM = std::max(dm.progSpreadM, P(n.params, "program_spread_m"));
        }
    const double cost = EffectiveHackRam(d, def), mx = RamMax(d);
    if (ram_ < 0) ram_ = mx;
    // A hack this deck can NEVER pay for: say so in the SAME words the screen's row uses, with both numbers, and
    // call it what it is rather than "not enough RAM", which sounds like waiting would help (the lead's MN-28).
    if (const std::string why = HackUnfireableWhy(d, def); !done && !why.empty()) {
        Notify(name + ": " + why);
        return { { "ok", false }, { "state", "unfireable" }, { "message", why }, { "ram", ram_ } };
    }
    if (!done && ram_ + 1e-9 < cost) { Notify(name + ": not enough RAM (" + std::to_string((int)std::floor(ram_)) + "/" + std::to_string((int)mx) + ")"); return { { "ok", false }, { "state", "ram" }, { "ram", ram_ } }; }
    double range = 30, critPct = 0;
    for (auto& n : d.desired.native) { range *= 1.0 + P(n.params, "range_pct") / 100.0; critPct = std::max(critPct, P(n.params, "qh_crit_chance_pct")); }
    const int64_t now = env_->NowMs();
    json out = { { "quickhack", id } };
    if (id == "qh_ping") {   // a player-side detect perk: no target needed
        NativeTimer& t = timers_["qh_ping"];
        t.startedAt = now; t.activeUntil = now + 30000; t.readyAt = t.activeUntil; t.perk = TimedPerkId("qh_ping", 1); t.wasOn = true;
        // D4/D5: the text says "within 30 m of the target", and Ping had no cue at all. The aimed hostile is the centre when there
        // is one (the player otherwise, so Ping still works as a blind scan); the cue and the highlight come from the plugin.
        const Env::HackTarget pt = env_->FindHackTarget(range);
        const uint32_t centre = pt.ok ? pt.id : 0;
        env_->HackVisual("applied", id, centre, 30.0);
        env_->HackEffect("ping", centre, 30.0, 30.0);
        out["target"] = centre;
        env_->Log("native: quickhack qh_ping centred on " + (centre ? std::to_string(centre) : std::string("the player")));
    } else {
        Env::HackTarget tg;
        if (done) { tg.ok = true; tg.id = done->target; tg.machine = done->machine; tg.hpPct = done->hpPct; }
        else tg = env_->FindHackTarget(range);
        if (!tg.ok) { Notify(name + ": no hostile target in sight"); return { { "ok", false }, { "state", "no-target" } }; }
        double uploadS = done ? 0.0 : def.value("upload_s", 0.0);
        if (uploadS > 0) {   // Ex-Disk and decks speed the upload up, never below 0.25 s. The two keys have opposite signs:
            // qh_upload_pct (Ex-Disk, ours) is "% faster", positive; upload_pct (decks) is CP's UploadQuickHackMod x 100, a
            // change to the upload TIME, so -25 means 25% faster. Adding it made Seacho and Stephenson 25% SLOWER (BETA QA 2
            // item audit, 2026-11-10).
            double faster = 0;
            for (auto& n : d.desired.native) faster += P(n.params, "qh_upload_pct") - P(n.params, "upload_pct");
            uploadS = std::max(0.25, uploadS * (1.0 - std::min(faster, 90.0) / 100.0));
            ram_ -= cost;
            upload_ = HackUpload{ id, tg.id, tg.machine, tg.hpPct, cost, now, now + (int64_t)std::llround(uploadS * 1000.0) };
            env_->HackVisual("upload", id, tg.id, uploadS);
            Notify(name + ": uploading");
            Sync();
            return { { "ok", true }, { "state", "uploading" }, { "quickhack", id }, { "target", tg.id }, { "etaS", uploadS }, { "ram", ram_ } };
        }
        auto it = d.applied.find("qhDamage");
        double mult = (1.0 + (it != d.applied.end() ? it->second : 0.0) / 100.0) * (1.0 + dm.dmg / 100.0);
        bool crit = critPct > 0 && (double)(NextRand(state_) % 10000) / 100.0 < critPct;
        if (crit) mult *= 2;
        using Debuff = HackDebuff;
        const auto& deb = HackDebuffs();
        const bool dot = id == "qh_overheat" || id == "qh_contagion", blackwall = id == "qh_blackwall_gateway";
        if (!dot && !blackwall && id != "qh_short_circuit" && id != "qh_synapse_burnout" && !deb.count(id)) return { { "ok", false }, { "message", id + " is not implemented" } };
        const int64_t dotMs = (int64_t)std::llround(6000.0 * (1.0 + dm.dotDur / 100.0));
        // Blackwall Gateway (TweakDB QuickHack.BlackWallHack, the Canto's fixed program): BaseBlackWallHackEffect force-kills the
        // target (CP exempts bosses / MaxTac / Exo; FO4's essential flag is the only exemption here, ESTIMATE) and chains to
        // QuickHackBlackWallSpreadNumber - 1 more hostiles within 20 m. 12 RAM, no cooldown.
        constexpr double kForceKill = 99999;
        auto apply = [&](const Env::HackTarget& t) {
            double dmg = 0;
            if (blackwall) { dmg = kForceKill; env_->TargetAV(t.id, "Health", -dmg); }
            else if (id == "qh_short_circuit") { dmg = 40 * mult * (1.0 + dm.elec / 100.0) * (t.machine ? 2 : 1); env_->TargetAV(t.id, "Health", -dmg); }
            else if (id == "qh_synapse_burnout") { dmg = 30 * mult * (1.0 + 2.0 * (100.0 - t.hpPct) / 100.0); env_->TargetAV(t.id, "Health", -dmg); }
            else if (dot) {
                dmg = (id == "qh_overheat" ? 60 : 48) * mult * (1.0 + dm.dot / 100.0);
                hackFx_.push_back({ t.id, "Health", 0, dmg / (dotMs / 1000.0), now + dotMs });
            } else {
                const Debuff& db = deb.at(id);
                const double secs = db.ms / 1000.0 * (1.0 + dm.dur / 100.0);
                env_->TargetAV(t.id, db.av, -db.amount);
                hackFx_.push_back({ t.id, db.av, db.amount, 0, now + (int64_t)std::llround(db.ms * (1.0 + dm.dur / 100.0)) });
                // D4: what the item text promises beyond the value. Reboot Optics blinds (machines too: their sensors); Cyberware
                // Malfunction staggers once as it lands.
                if (id == "qh_reboot_optics") { env_->HackEffect("blind", t.id, secs, 0); hackFx_.back().effect = "blind"; }   // remembered for a load (HacksRestore)
                else if (id == "qh_cyberware_malfunction") env_->HackEffect("stagger", t.id, 0, 0);
            }
            return dmg;
        };
        double dmg = apply(tg);
        double shownS = 1.5;
        if (dot) shownS = dotMs / 1000.0;
        else if (deb.count(id)) shownS = deb.at(id).ms / 1000.0 * (1.0 + dm.dur / 100.0);
        env_->HackVisual("applied", id, tg.id, shownS);
        // spread (CP SpreadQuickhacksOnStart / SpreadAreaEffector): the same hack on n more hostiles near the target,
        // 6 m x (1 + QuickHackSpreadDistance)
        int spread = 0;
        if (blackwall && dm.progTargets >= 2)
            for (auto& t2 : env_->NearbyHostiles(tg.id, dm.progSpreadM > 0 ? dm.progSpreadM : 20.0, (int)std::lround(dm.progTargets) - 1)) { apply(t2); ++spread; }
        else if (dm.spreadN >= 1 || id == "qh_contagion") {
            // D4: Contagion's text says it jumps to 2 nearby enemies; that is the hack's own spread, and a deck's spread adds to it (ESTIMATE)
            const int n = (int)std::lround(dm.spreadN) + (id == "qh_contagion" ? 2 : 0);
            for (auto& t2 : env_->NearbyHostiles(tg.id, 6.0 * (1.0 + dm.spreadPct / 100.0), n)) { apply(t2); ++spread; }
        }
        // Biotech Sigma Mk.5 (TweakDB RefreshQhWithTagInAreaEffector, DoT|Overload + Quickhack, 10 m): an upload refreshes the DoT
        // hacks running on hostiles within range of the target (CP gates it on Overclock; FO4 has none, so always: ESTIMATE)
        int refreshed = 0;
        if (dm.refreshM > 0) {
            std::set<uint32_t> near{ tg.id };
            for (auto& t2 : env_->NearbyHostiles(tg.id, dm.refreshM, 64)) near.insert(t2.id);
            for (auto& fx : hackFx_)
                if (fx.perSec > 0 && near.count(fx.target) && fx.until > now && fx.until < now + dotMs) { fx.until = now + dotMs; ++refreshed; }
        }
        out["target"] = tg.id; out["damage"] = dmg; out["crit"] = crit; out["spread"] = spread; out["refreshed"] = refreshed;
        if (env_) env_->Log("native: quickhack " + id + " on " + std::to_string(tg.id) + " dmg " + std::to_string(dmg) + (crit ? " (crit)" : ""));
    }
    if (!done) ram_ -= cost;
    Notify(name + " - RAM " + std::to_string((int)ram_) + "/" + std::to_string((int)mx));
    out["ok"] = true; out["ram"] = ram_;
    Sync();
    return out;
}

json Engine::HackView() const
{
    json a = json::array();
    const int64_t now = env_ ? env_->NowMs() : 0;
    for (auto& fx : hackFx_)
        a.push_back({ { "target", fx.target }, { "av", fx.av }, { "dot", fx.perSec > 0 }, { "secondsLeft", std::max<int64_t>(0, fx.until - now) / 1000.0 } });
    if (upload_) {
        std::string name = upload_->id;
        for (auto& h : cat_.raw.value("quickhacks", json::array())) if (h.value("id", "") == upload_->id) name = h.value("name", name);
        a.push_back({ { "uploading", true }, { "name", name }, { "target", upload_->target }, { "secondsLeft", std::max<int64_t>(0, upload_->end - now) / 1000.0 } });
    }
    return a;
}

void Engine::HacksPreSave()
{
    if (!env_ || hacksHeld_) return;
    for (auto& fx : hackFx_) if (fx.perSec <= 0) env_->TargetAV(fx.target, fx.av, fx.amount);
    hacksHeld_ = true;
}

void Engine::HacksPostSave()
{
    if (!env_ || !hacksHeld_) return;
    for (auto& fx : hackFx_) if (fx.perSec <= 0) env_->TargetAV(fx.target, fx.av, -fx.amount);
    hacksHeld_ = false;   // one that ended during the save finishes on the next tick
}

// D6 (2026-11-10): the RAM a quickhack really costs on this deck: its catalog `ram` minus the deck's covert / ultimate reduction
// (CP stealth_cost_red / ultimate_cost_red). One function, so the HUD, the hack_next notice and the charge never disagree.
double EffectiveHackRam(const Derived& d, const json& def)
{
    double stealth = 0, ult = 0;
    for (auto& n : d.desired.native)
        if (n.handler.rfind("os_deck_", 0) == 0) { stealth += P(n.params, "stealth_cost_red"); ult += P(n.params, "ultimate_cost_red"); }
    const std::string kind = def.value("kind", "");
    const double red = kind == "covert" ? stealth : kind == "ultimate" ? ult : 0.0;
    // D1/QD N2: the cost is a WHOLE number, so the number shown is exactly the number charged on every surface and
    // no rounding can put them a point apart. Catalog `ram` and CP's cost reductions are whole today; lround keeps
    // that true if one ever is not. The matching display rule is the RAM POOL being floored, never rounded, for any
    // integer readout (HUD-API): floor(pool) >= cost then implies the charge succeeds.
    return std::max(0.0, (double)std::lround(def.value("ram", 0.0) - red));
}

// An upload or a hack effect is running: a loadout change is refused (E_BUSY), and the running upload is untouched.
bool Engine::HacksBusy() const { return upload_.has_value() || !hackFx_.empty(); }

// StateView's `quickhacks` block. Agreed in writing with MENU/HUD AAA, docs/QUICKHACK-LOADOUT.md section 6.1.
json Engine::QuickhacksView(const State& s, const Derived& d) const
{
    int slots = 0;
    std::string program, deckItem;
    for (auto& n : d.desired.native)
        if (n.handler.rfind("os_deck_", 0) == 0) {
            slots = std::max(slots, (int)P(n.params, "qh_slots", 2));
            if (n.params.is_object()) program = n.params.value("program", program);
            // Every deck's handler os_deck_X comes from ability ab_os_deck_X on item cw_os_deck_X; checked against
            // the catalog, so a deck that ever breaks the convention reports null rather than a wrong id.
            if (deckItem.empty() && cat_.items.count("cw_" + n.handler)) deckItem = "cw_" + n.handler;
        }
    const std::vector<std::string> eq = DeckQuickhacks(s, d), order = LoadoutOrder(s);
    const double ramMax = slots > 0 ? RamMax(d) : 0.0;
    json benched = json::array();
    for (auto& q : order)
        if (std::find(eq.begin(), eq.end(), q) == eq.end()) benched.push_back(q);
    json all = json::array();
    for (auto& h : cat_.raw.value("quickhacks", json::array())) {
        const std::string id = h.value("id", "");
        if (id.empty()) continue;
        auto it = std::find(eq.begin(), eq.end(), id);
        const double eff = EffectiveHackRam(d, h);
        all.push_back({ { "id", id }, { "name", h.value("name", id) }, { "ram", eff }, { "ramBase", h.value("ram", 0.0) },
                        { "kind", h.value("kind", "") }, { "upload_s", h.value("upload_s", 0.0) },
                        { "equipped", it != eq.end() },
                        { "slot", it != eq.end() ? json((int)(it - eq.begin()) + 1) : json(nullptr) },   // 1-based, null when not slotted
                        { "locked", !HackRankable(id) && id != program },                                // Blackwall off the Canto
                        { "lockedReason", !HackRankable(id) && id != program ? json("Militech Canto only") : json(nullptr) },
                        { "fireable", ramMax <= 0 ? false : eff <= ramMax + 1e-9 },
                        // The SAME words the HUD says on a press, from the same function: the screen never writes
                        // its own version of this sentence (the lead's MN-28, guarantee 2).
                        { "unfireableReason", ramMax <= 0 || eff <= ramMax + 1e-9 ? json(nullptr) : json(HackUnfireableWhy(d, h)) } });
    }
    const size_t selIdx = HackSel(eq);
    // QD N1: the screen shows the lock BEFORE the player drags a row, instead of discovering it on a refusal.
    const bool inCombat = env_ && env_->PlayerIs("combat");
    const bool busy = HacksBusy();
    return { { "locked", inCombat || busy },
             { "lockedReason", inCombat ? json("in combat") : busy ? json("a quickhack is running") : json(nullptr) },
             { "deck", deckItem.empty() ? json(nullptr) : json(deckItem) },
             { "slots", slots },
             { "ramMax", ramMax },
             { "program", program.empty() ? json(nullptr) : json(program) },
             { "equipped", eq },
             { "order", order },
             { "custom", !s.loadout.empty() },
             { "use", s.loadoutUse < 0 ? json(nullptr) : json(s.loadoutUse) },
             { "benched", benched },
             { "selected", eq.empty() ? json(nullptr) : json(eq[selIdx]) },
             { "all", all } };
}

// The setLoadout op. The full ordered list, as agreed with MENU/HUD AAA; ids not listed keep their previous relative
// order AFTER the listed ones, read off the EFFECTIVE order so a first edit on a fresh save is well defined (QD R1).
void Engine::LoadoutSet(State& s, const json& args) const
{
    if (!args.contains("hacks")) throw CwError("E_BAD_ARGS", "setLoadout needs 'hacks': a list of quickhack ids in your order");
    const json& hs = args["hacks"];
    if (!hs.is_array()) throw CwError("E_BAD_ARGS", "'hacks' must be a list of quickhack ids");
    if (hs.size() > 32) throw CwError("E_BAD_ARGS", "'hacks' has more than 32 entries");
    std::set<std::string> have;
    for (auto& h : cat_.raw.value("quickhacks", json::array())) have.insert(h.value("id", ""));
    std::vector<std::string> picked;
    std::set<std::string> seen;
    for (auto& e : hs) {
        if (!e.is_string()) throw CwError("E_BAD_ARGS", "every entry of 'hacks' must be a quickhack id");
        const std::string id = e.get<std::string>();
        if (!have.count(id)) throw CwError("E_UNKNOWN", "no such quickhack '" + id + "'");
        if (!HackRankable(id)) throw CwError("E_BAD_ARGS", "'" + id + "' cannot be chosen: it is a cyberdeck's own program");
        if (!seen.insert(id).second) throw CwError("E_BAD_ARGS", "'" + id + "' is listed twice");
        picked.push_back(id);
    }
    if (args.contains("use") && !args["use"].is_null()) {
        if (!args["use"].is_number_integer()) throw CwError("E_BAD_ARGS", "'use' must be a whole number of slots, or null for all of them");
        const int u = args["use"].get<int>();
        if (u < 0 || u > 32) throw CwError("E_BAD_ARGS", "'use' must be between 0 and 32");
        s.loadoutUse = u;
    } else if (args.contains("use")) {
        s.loadoutUse = -1;
    }
    // "Forget my choice" forgets ALL of it, `use` included, whether or not `use` was sent: it is one Reset to
    // default on the screen, so leaving a stale slot cap behind would be a surprise (MENU/HUD AAA, 2026-11-10).
    if (picked.empty()) { s.loadout.clear(); s.loadoutUse = -1; return; }
    const std::vector<std::string> prev = LoadoutOrder(s);   // the EFFECTIVE order, never the bare stored list
    for (auto& id : prev)
        if (!seen.count(id)) { picked.push_back(id); seen.insert(id); }
    s.loadout = picked;
}

json Engine::HackSelected()
{
    Derived d = Compute(state_);
    auto q = DeckQuickhacks(d);
    if (q.empty()) return nullptr;
    const std::string id = q[HackSel(q)];
    for (auto& h : cat_.raw.value("quickhacks", json::array()))
        if (h.value("id", "") == id) return { { "name", h.value("name", id) }, { "cost", EffectiveHackRam(d, h) } };
    return nullptr;
}

json Engine::HacksSave() const
{
    const int64_t now = env_ ? env_->NowMs() : 0;
    json fx = json::array();
    for (auto& f : hackFx_) {
        fx.push_back({ { "target", f.target }, { "av", f.av }, { "amount", f.amount }, { "perSec", f.perSec }, { "leftMs", std::max<int64_t>(0, f.until - now) } });
        if (!f.effect.empty()) fx.back()["effect"] = f.effect;   // absent for every other effect, so older cosaves and newer ones read the same
    }
    json out = { { "fx", fx } };
    if (upload_)
        out["upload"] = { { "id", upload_->id }, { "target", upload_->target }, { "machine", upload_->machine }, { "hpPct", upload_->hpPct },
                          { "cost", upload_->cost }, { "leftMs", std::max<int64_t>(0, upload_->end - now) }, { "totalMs", upload_->end - upload_->start } };
    return out;
}

int Engine::HacksRestore(const json& saved)
{
    if (!env_ || !saved.is_object()) return 0;
    const int64_t now = env_->NowMs();
    int n = 0;
    for (auto& f : saved.value("fx", json::array())) {
        if (!f.is_object()) continue;
        const int64_t left = f.value("leftMs", (int64_t)0);
        HackFx h{ f.value("target", 0u), f.value("av", std::string()), f.value("amount", 0.0), f.value("perSec", 0.0), now + left, f.value("effect", std::string()) };
        if (left <= 0 || !h.target || h.av.empty()) continue;   // ended during the save: nothing to give back or take
        if (h.perSec <= 0) env_->TargetAV(h.target, h.av, -h.amount);   // the save holds the actor without the debuff (HacksPreSave)
        if (!h.effect.empty()) env_->HackEffect(h.effect, h.target, (double)left / 1000.0, 0);   // and without its blind (chrome::Suspend): asked again, for the time left
        hackFx_.push_back(h);
        ++n;
    }
    if (saved.contains("upload") && saved["upload"].is_object()) {
        const json& u = saved["upload"];
        HackUpload up;
        up.id = u.value("id", std::string());
        up.target = u.value("target", 0u);
        up.machine = u.value("machine", false);
        up.hpPct = u.value("hpPct", 100.0);
        up.cost = u.value("cost", 0.0);
        const int64_t left = std::max<int64_t>(0, u.value("leftMs", (int64_t)0));
        up.end = now + left;
        up.start = up.end - std::max<int64_t>(left, u.value("totalMs", left));
        if (!up.id.empty() && up.target) { upload_ = up; ++n; }   // RamTick lands it, or interrupts it and refunds, as for any upload
    }
    hacksHeld_ = false;
    if (n) env_->Log("native: " + std::to_string(n) + " quickhack(s) restored after the load");
    return n;
}

void Engine::HacksOnLoad()
{
    deckSeen_ = false;   // D1: re-record this save's deck on the next tick without announcing it
    lastDeck_.clear();
    upload_.reset();
    hackFx_.clear();
    hacksHeld_ = false;
}

int Engine::SpreadHacks(uint32_t from, const std::vector<uint32_t>& to)
{
    if (!env_) return 0;
    std::vector<HackFx> add;
    for (auto& fx : hackFx_) if (fx.target == from) add.push_back(fx);
    if (add.empty()) return 0;
    int n = 0;
    for (uint32_t t : to) {
        if (t == from) continue;
        bool got = false;
        for (auto fx : add) {
            bool already = false;
            for (auto& h : hackFx_) if (h.target == t && h.av == fx.av) already = true;
            if (already) continue;
            fx.target = t;
            fx.effect.clear();   // a spread copy was never blinded (D4 asks for the effect on the hacked target only), so a load must not blind it
            if (fx.perSec <= 0) env_->TargetAV(t, fx.av, -fx.amount);   // a debuff takes its amount now, gives it back at the end
            hackFx_.push_back(fx);
            got = true;
        }
        if (got) ++n;
    }
    return n;
}

json Engine::NativeView() const
{
    const int64_t now = env_ ? env_->NowMs() : 0;
    json a = json::array();
    for (auto& [ab, t] : timers_)
        a.push_back({ { "ability", ab }, { "active", t.activeUntil > now }, { "secondsActive", std::max<int64_t>(0, t.activeUntil - now) / 1000.0 },
                      { "secondsToReady", std::max<int64_t>(0, t.readyAt - now) / 1000.0 }, { "perk", t.perk } });
    return a;
}

}  // namespace cw

namespace cw {
bool Engine::NativeConsume(const std::string& what, double seconds)
{
    if (!env_) return false;
    const Derived d = Compute(state_);
    const double mx = RamMax(d);
    if (mx <= 0) { Notify("No cyberdeck: nothing to recharge"); return false; }
    if (ram_ < 0) ram_ = mx;
    if (what == "ram_restore") {
        ram_ = std::min(mx, ram_ + mx * 0.5);
        Notify("RAM " + std::to_string((int)ram_) + "/" + std::to_string((int)mx));
        env_->Log("armory: ram_restore -> " + std::to_string(ram_) + "/" + std::to_string(mx));
        return true;
    }
    if (what == "ram_regen") {
        ramBoost_ = 0.5;
        ramBoostUntil_ = env_->NowMs() + (int64_t)std::llround(std::max(1.0, seconds) * 1000);
        env_->Log("armory: ram_regen +0.5/s for " + std::to_string((int)seconds) + " s");
        return true;
    }
    env_->Log("armory: unknown native consumable '" + what + "'");
    return false;
}
}  // namespace cw
