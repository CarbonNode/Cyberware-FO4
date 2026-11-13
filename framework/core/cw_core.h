// cw_core.h - Cyberware framework core: catalog, per-save state, stat engine, JSON API.
//
// This layer has NO game dependency. It compiles with g++ (Linux test harness) and MSVC (the F4SE plugin).
// The game is reached only through the Env interface below; the plugin implements it with CommonLibF4,
// the test harness with a fake. Everything the UI sees goes through Api::Handle (docs/UI-API.md).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "../third_party/nlohmann/json.hpp"

namespace cw {

using json = nlohmann::json;

constexpr int kApiVersion = 1;
constexpr int kStateVersion = 1;
constexpr const char* kPluginName = "Cyberware.esp";

// ---------------------------------------------------------------- errors
struct CwError : std::runtime_error {
    std::string code;
    CwError(std::string c, const std::string& msg) : std::runtime_error(msg), code(std::move(c)) {}
};

// ---------------------------------------------------------------- catalog
using StatMap = std::map<std::string, double>;  // stat id -> value

struct RollOption { std::string stat; double min = 0, max = 0, weight = 1; };
struct RollGroup  { int count = 1; std::vector<RollOption> pool; };
struct Requires   { int level = 0; };

struct CompCost { std::string form, name; int count = 0; };   // an upgrade component (vanilla junk/component MISC)

struct TierDef {
    int tier = 1;
    int64_t price = 0;
    std::optional<int64_t> upgradePrice;  // cost to reach THIS tier from the previous one; default price delta
    std::vector<CompCost> components;     // ALSO needed to reach this tier (Settings.upgradeComponents, the lead, 2026-11-08)
    int capacityCost = 0;
    int capacityGrant = 0;              // + max capacity while a piece at this tier is installed (Chrome Compressor)
    int modSlots = 0;
    StatMap stats;
    std::vector<std::string> abilities;
    std::vector<RollGroup> rolls;
    Requires requires_;
};

struct VariantDef { std::string id, name, icon, armo; int64_t price = 0; };

struct ItemDef {
    std::string id, name, slot, manufacturer, description, icon;
    bool iconic = false;
    std::vector<std::string> iconicAbilities;
    std::vector<std::string> modSlotTypes;
    std::vector<VariantDef> variants;
    std::vector<TierDef> tiers;  // ascending by tier
    std::optional<int> chipTier;
    json raw;                    // the source object, passed through to the UI untouched
    const TierDef* FindTier(int t) const;
    const TierDef* NextTier(int t) const;
    const VariantDef* FindVariant(const std::string& v) const;
};

struct ModDef {
    std::string id, name, type, description, icon;
    std::vector<TierDef> tiers;
    json raw;
    const TierDef* FindTier(int t) const;
};

struct SlotDef  { std::string id, name; int count = 1; };
struct StatDef  { std::string id, name, av, avName, format; double step = 1; int bits = 6, negBits = 0; bool custom = false; };
struct AbilityEffect { std::string av; double value = 0; };
struct AbilityDef { std::string id, name, description, perk, spell; std::vector<AbilityEffect> effects; json raw; };

struct Scope {          // which installed pieces an effect touches; empty = all
    std::string slot, item;
    std::optional<bool> iconic;
    bool Matches(const ItemDef& it) const;
};
struct PerkEffect {
    bool base = false;                  // capacityPerAV: use the BASE actor value instead of the current one
    std::string type, stat, slot, ability, av;
    double add = 0, mult = 0, maxPct = 0, healthPctPerPoint = 0;
    Scope scope;
};
struct CyberPerkDef {
    std::string id, name, description, fo4Perk;
    bool autoGrant = false;  // rank comes from a vanilla FO4 perk, not from points
    int maxRank = 1, cost = 1;
    std::vector<std::string> prereq;    // perk ids that must be at rank >= 1
    Requires requires_;
    std::vector<PerkEffect> effects;
    json raw;
};

struct CapacityItemDef {                // consumable that raises max capacity (Neural Matrix shard)
    std::string id, name, form;         // form: "Plugin|id" of the item, or "" = Cyberware.esp CW_CapItem_<id>
    int add = 0, max = 1;
    std::string ItemKey() const { return form.empty() ? "CW_CapItem_" + id : "@" + form; }
};

struct StockEntry { std::string id; std::vector<int> tiers; };
struct RipperdocDef {
    std::string id, name;
    double priceMult = 1.0;
    std::optional<double> sellMult;
    std::vector<StockEntry> stock, modStock;
    std::vector<std::pair<std::string, int64_t>> capacityItemStock;   // capacity item id -> base price (CP shards sold here)
    json raw;
};

struct Catalog {
    int schemaVersion = 1;
    std::string currencyForm = "Fallout4.esm|00000F", currencyName = "Caps";
    int capBase = 40, capPerLevel = 3, capMax = 450;
    struct CapPerAV { std::string av; double add = 0; double min = 0; };
    // capacity.perAV: always on, from the BASE actor value. min == 0: + add x value (linear).
    // min > 0: + add once when value >= min (the CP Renaissance Punk shape: +4 per SPECIAL at 9+).
    std::vector<CapPerAV> capPerAV;
    std::vector<std::string> warnings;                       // non-fatal: unknown keys etc. (cwtest --validate prints them)
    int ppStart = 1, ppPerLevels = 5;
    double sellMult = 0.25;
    double iconicUpgradeMult = 1.0;
    double rerollMult = 0.25;
    bool perkRefund = true;
    json tiersRaw = json::array();
    std::vector<SlotDef> slots;
    std::vector<StatDef> stats;
    std::map<std::string, AbilityDef> abilities;
    std::map<std::string, ItemDef> items;
    std::map<std::string, ModDef> mods;
    std::map<std::string, CyberPerkDef> perks;
    std::map<std::string, RipperdocDef> ripperdocs;
    std::map<std::string, CapacityItemDef> capacityItems;
    std::vector<std::string> itemOrder, modOrder, perkOrder, ripperdocOrder, capacityItemOrder;  // file order, for stable UI lists
    json raw;

    // Parse + validate. Throws CwError("E_CATALOG", ...) listing every problem found.
    static Catalog FromJson(const json& j);
    const SlotDef* FindSlot(const std::string& id) const;
    const StatDef* FindStat(const std::string& id) const;
};

// ---------------------------------------------------------------- per-save state
struct Roll { std::string stat; double value = 0; };

struct Instance {                       // one owned piece of cyberware
    std::string uid, item, variant;
    int tier = 1;
    std::vector<Roll> rolls;
    std::vector<std::string> mods;      // per active mod slot: mod uid or "" (empty)
    std::string slot;                   // installed slot id, "" = in stash
    int slotIndex = -1;
    bool hasPending = false;            // a keep-old reroll waits for rerollResolve
    std::vector<Roll> pendingRolls;
};

struct ModInstance {
    std::string uid, mod;
    int tier = 1;
    std::vector<Roll> rolls;
    std::string attachedTo;             // instance uid or ""
};

// Player-facing gameplay settings (BETA QA, 2026-11-08). Saved in the cosave (per save) and
// mirrored by the plugin to a Documents ini as the default for new games. Applied live:
// every Compute/Price reads them. Defaults = CP 2.x behaviour, everything on.
struct Settings {
    bool capacityEnabled = true;    // off: capacity is not enforced (max shows as unlimited)
    double capacityMult = 1.0;      // scales max capacity (0.1..10)
    double priceMult = 1.0;         // scales every buy/sell/upgrade/reroll price (0..10)
    bool levelPricing = true;       // prices also scale with player level (cw::LevelPriceMult)
    bool slotLimits = true;         // off: every slot holds the schema maximum (16)
    bool overCapacity = true;       // off: Edgerunner over-capacity is disabled (hard block at max)
    bool upgradeComponents = true;  // tier upgrades also cost FO4 components (the lead, 2026-11-08)
    bool installScene = true;       // ripperdoc install/uninstall scene (plugin + view)
    bool hud = true;                // the CyberwareHUD overlay (plugin shows/hides the view)
    bool menuSounds = true;         // ripperdoc menu UI sounds (view: CW.Sound)
    json ToJson() const;
    // Partial update, values clamped. strict (the setSettings op): an unknown key or a wrong type is
    // E_BAD_ARGS. Tolerant (cosave load): unknown keys and wrong types are skipped.
    void MergeFromJson(const json& j, bool strict = true);
};

struct State {
    int version = kStateVersion;
    Settings settings;
    uint64_t rng = 0;                   // splitmix64 state; 0 = not seeded yet
    uint64_t nextUid = 1;
    std::map<std::string, Instance> instances;
    std::map<std::string, ModInstance> modInstances;
    std::map<std::string, int> perkRanks;   // bought cyber perks
    std::map<std::string, int> capacityItemsUsed;
    int bonusPerkPoints = 0;
    json orphans = json::array();       // records whose catalog entry vanished; kept so nothing is lost
    json customize = json::object();    // the player's colour / glow choices (cw_customize.cpp); only what they changed
    // The player's quickhack ranking (D1, docs/QUICKHACK-LOADOUT.md). Only what they chose, as `customize` is:
    // empty = the built-in default order. The deck holds the first N of the EFFECTIVE order after its fixed
    // program, N = qh_slots capped by loadoutUse. Unknown ids are kept (the `orphans` idea) and skipped in use.
    std::vector<std::string> loadout;
    int loadoutUse = -1;                // how many PLAYER slots to fill; -1 = all of them (JSON null)
    std::string hackSelected;           // the hack the key uploads, BY ID not by index; "" = the first slot

    json ToJson() const;
    static State FromJson(const json& j);
};

// A Windows virtual-key code as the player would read it on screen ("F8", "Delete", "'", "" for 0 / unknown).
// D1 prints the key BOUND to the setup view in its benched-quickhacks notice rather than a literal (MENU/HUD AAA).
std::string VkName(unsigned vk);

// Colour / glow customization (cw_customize.cpp, docs/UI-API.md section 9)
json CustomizeResolve(const json& stored);                  // stored over the defaults: {targets{id:{color,glow}}, seams, pulses}
void CustomizeSet(json& stored, const json& args);          // validates; throws CwError
void CustomizeReset(json& stored, const json& args);        // args.target = one target, none = everything

// ---------------------------------------------------------------- game boundary
struct NativeAbility {                  // an ability whose effect is C++ code in the plugin (catalog spec.native)
    std::string ability, handler, activation;
    int tier = 1;                       // highest tier among the installed sources granting it
    json params;                        // spec.native.paramsT<tier>, or paramsT5 scaled by tier/5 when scaleWithTier
};

struct Derived;
double EffectiveHackRam(const Derived& d, const json& def);   // D6: catalog RAM minus the deck's covert/ultimate reduction (cw_native.cpp)

enum class NativeFamily {
    Stub, TimeSlow, Camo, AutoCamo, Scan, OnKill, OnHit, Modifier,   // v1 (time slow, camo, scan, on-kill/on-hit, OS mods)
    Passive,      // always/equip: the ability's CW_Nat perk is on while installed (AV + perk-entry effects, maybe conditioned)
    Buff,         // timed CW_Nat perk with effects; started by a key (Berserk: the OS key) or an event (NativeTrigger)
    CondSlow,     // timed time slow started by a condition: low HP (Reflex Tuner, Revulsor), aim-while-sprinting (Kerenzikov)
    Heal,         // Blood Pump: the heal key -> flat heal + heal over time, cooldown
    SecondHeart,  // HP at or below the threshold -> full heal, cooldown
    Biomonitor,   // HP below the threshold -> uses a Stimpak from the inventory, lockout
    Jump,         // legs: double jump / charged jump / air dash (plugin, character controller); passive perk for CarryWeight etc.
    Arms,         // ar_mantis / ar_gorilla / ar_monowire: the arm WEAP follows the installed cyberware (plugin, arms::)
    Deck,         // os_deck_*: RAM pool + quickhack slots; the hack key uploads the selected quickhack to the aimed-at hostile
    Ram,          // RAM chips: max RAM, refills at a threshold (Camillo, Reallocator), RAM on hit (Recoup), quickhack crits,
                  // quickhack range (Deep Field)
    Hud,          // plugin HUD / camera: Kiroshi zoom (hold the scan key), Threat Detector and Target Analysis HUD lines
    Runtime,      // plugin player modifiers through cw::RuntimeMods (Rara Avis: max Health %, AP regen)
    WeaponMod,    // plugin scales the equipped weapon's instance data: reload_pct (reloadSpeed), melee_speed_pct (speed)
    Hurt,         // when the player is hit (plugin): Carapace (flank hits), Painducer (defer), Nano Plating (negate),
                  // Shock-n-Awe (burst), Rangeguard (DR while no hostile is near)
    Gate,         // no runtime effect of its own: perk_jailbreak (its sub-perks need it, cyberPerks prereq), Kiroshi Mk.1-3
                  // (their mod_slots are opened by the catalog)
    ArmMod,       // mod_mantis_* / mod_gorilla_* / mod_mono_* / mod_pls_*: perk entries conditioned on the arm WEAP's CW_Anims*
                  // keyword (damage, charged damage, element hit spell) + plugin runtime (rotor speed, PLS round / charges)
};
NativeFamily FamilyOf(const std::string& handler, const std::string& activation, const json& params);
std::string FamilyLabel(NativeFamily f);
std::string NativeTrigger(const std::string& handler);           // Buff/CondSlow start: "key:os", "sneak_kill", "combat_start", ...
std::string TimedPerkId(const std::string& ability, int tier);   // CW_Nat_<ability>_T<tier>
// The highest tier a CW_Nat perk is BAKED for, per ability. 5 for everything except the four Kiroshi scans, whose
// scan RANGE lives in the detect spell's area (SpellSpecFor) and therefore cannot be computed at runtime: the lead
// ruled 8 records for their tier 6 (2026-11-10). Every site that bakes, resolves or cross-checks a native perk reads
// THIS, so the three places that used to say "1..5" cannot drift apart.
int BakedTierCap(const std::string& ability);
struct NativeSpellSpec { std::string kind; double magnitude = 0; int areaFeet = 0; };  // kind: slowtime|chameleon|detect|""
NativeSpellSpec SpellSpecFor(NativeFamily f, const json& params);
json ScaleNativeParams(const json& paramsT5, int tier);           // the scaleWithTier rule (docs/CATALOG-SCHEMA.md)
json NativeParamsFor(const json& specNative, int tier);          // paramsT<tier>, else paramsT5 (scaled when scaleWithTier)

// What a CW_Nat_<ability>_T<tier> perk carries (cw_effects.cpp). cwtest --native-effects writes it for every ability and
// tier; espgen bakes it. One table in one language: espgen holds no family or param logic of its own.
struct NatCond { std::string fn; double value = 0; };   // sneaking | combat | sprinting | hp_below | hp_above | ap_above (0..1)
                                                        // | power_attack (IsPowerAttacking) | chance (GetRandomPercent < value)
                                                        // | aiming (GetInIronSights)
struct NatAvFx { std::string av; double value = 0; std::vector<NatCond> when; };            // value-modifier effect on an AV
// The timed debuff a quickhack applies whatever any gated key says. The item text binds its numbers to this table
// (tools/catalog_data.py QH_GATED_TEXT, checked by cwtest --validate): amount, and ms / 1000 as the seconds.
struct HackDebuff { const char* av; double amount; int ms; };
const std::map<std::string, HackDebuff>& HackDebuffs();

struct NatEntryFx {                                     // a perk entry point, always Multiply
    std::string entry;                                  // attack | incoming | power_ap | vats_ap | vats_hit | fall | stagger_out | stagger_in | sneak_attack
                                                        // | hit_spell (Apply Combat Hit Spell: `spell`) | grenade_trajectory (Set 1)
    double value = 1;
    std::vector<std::string> weaponAny, weaponNone, targetAny;   // keyword conditions on the weapon / target tab
    std::vector<NatCond> when;                                   // perk-owner conditions
    std::string spell;                                           // hit_spell: bleed | burn | poison | shock | paralyze (espgen maps it)
    double distMin = -1, distMax = -1;                           // FO4 units: GetDistance(PlayerRef) on the attacker (incoming)
                                                                 // or target (attack) tab; -1 = no bound
};
struct NativeEffects {
    NativeSpellSpec fx;                                 // the base effect of a timed perk (slow time, chameleon, detect)
    std::vector<NatCond> fxWhen;                        // conditions on that effect (Stalker: aiming down sights)
    std::vector<NatAvFx> avs;
    std::vector<NatEntryFx> entries;
    std::vector<std::string> pending;                   // param keys with no game effect yet (reported, counted as stub work)
    std::vector<std::string> na;                        // CP params with no FO4 counterpart (Breach Protocol buffer, upload time,
                                                        // enemy netrunners, device hacks, Overclock): documented, not pending
    bool Empty() const { return fx.kind.empty() && avs.empty() && entries.empty(); }
};
NativeEffects EffectsFor(const std::string& handler, NativeFamily f, const json& params);
// What `GetRandomPercent < stated` applies: the engine rolls whole percents, so 12.25 is applied as 13 (cw_effects.cpp).
double EngineChancePct(double stated);
// Stated chances that are not what the engine applies. problems fail --validate (Stabber); warnings are reported.
struct EngineChanceReport { std::vector<std::string> problems, warnings; };
EngineChanceReport EngineChanceProblems(const Catalog& cat);
bool PerkIsTimed(NativeFamily f);                       // the CW_Nat perk is on only while a timer runs (else: while installed)
json NativeEffectsJson(const Catalog& cat);             // {perks:{CW_Nat_..:{...}}, handlers:{h:{family, real, pending}}}
// Real = the handler's family does something in game and no param is left pending; partial = some params pending.
struct HandlerStatus { std::string family; bool real = false, partial = false; std::vector<std::string> pending, na; };
HandlerStatus StatusOf(const std::string& handler, const std::string& activation, const json& paramsT5);

// The always-on shared models (docs/DESIGN.md 3.5): CW_CritChance/CW_CritDamage drive out-of-VATS crits, CW_Mitigation*
// a chance to shrug off part of a hit. espgen bakes CW_ChromeCrit; the engine wants it on whenever the catalog loads.
constexpr const char* kChromeCritPerk = "CW_ChromeCrit";

struct NativeTimer { int64_t startedAt = 0, activeUntil = 0, readyAt = 0; int tier = 1; std::string perk; bool wasOn = false; double hot = 0;
                     std::string cue; };   // cue: the handler whose "ready" cue is owed when readyAt passes (Second Heart)

struct Desired {                        // what the game should look like for the current state
    std::set<std::string> perkEditorIds;    // every Cyberware.esp perk that should be on the player
    std::vector<std::pair<std::string, std::string>> visuals;  // (instance uid, armo form) to equip
    std::vector<NativeAbility> native;
};

struct ChipFound { std::string kind, id; int count = 0; };  // kind "item" | "mod"

class Env {
public:
    virtual ~Env() = default;
    virtual int64_t GetCurrency() = 0;
    virtual bool SpendCurrency(int64_t amount) = 0;   // false = not enough / failed, nothing taken
    virtual void AddCurrency(int64_t amount) = 0;
    virtual int GetLevel() = 0;
    virtual int GetFo4PerkRank(const std::string& formKey) = 0;  // 0 when absent or unresolvable
    virtual void ApplyDesired(const Desired& d) = 0;
    virtual std::vector<ChipFound> TakeChips() { return {}; }   // loot tokens in inventory -> removed + reported
    virtual double GetActorValue(const std::string& av) { (void)av; return 0; }      // current (buffed) value, by AVIF EditorID
    virtual double GetBaseActorValue(const std::string& av) { (void)av; return 0; }  // base value
    // base + permanent modifiers, EXCLUDING damage/drain (wounds, spent AP). The qaaudit measures
    // with this so regenerating AVs (ActionPoints, Health) do not drift between snapshots.
    virtual double GetPermanentActorValue(const std::string& av) { return GetActorValue(av); }
    // Inventory items by key: "EditorID" of a Cyberware.esp record, or "@Plugin|id".
    virtual int CountItem(const std::string& key) { (void)key; return 0; }
    virtual bool ConsumeItem(const std::string& key, int n) { (void)key; (void)n; return false; }
    // FO4-crafting-style component counting: the component itself PLUS what the junk in the
    // inventory breaks down into. The plugin overrides these; the defaults fall back to plain
    // item counting so the headless harness works.
    virtual int CountComponent(const std::string& key) { return CountItem(key); }
    virtual bool ConsumeComponents(const std::vector<std::pair<std::string, int>>& need) {
        for (auto& [k, n] : need) if (CountComponent(k) < n) return false;
        for (auto& [k, n] : need) if (!ConsumeItem(k, n)) return false;
        return true;
    }
    virtual int64_t NowMs() { return 0; }                                               // real-time clock for cooldowns
    virtual double ActorValuePct(const std::string& av) { (void)av; return 100; }        // current / max x 100
    virtual void RestoreActorValuePct(const std::string& av, double pct) { (void)av; (void)pct; }
    virtual void Notify(const std::string& text) { (void)text; }                         // HUD message
    virtual void RestoreActorValue(const std::string& av, double amount) { (void)av; (void)amount; }  // flat heal
    virtual bool UseItem(const std::string& key) { (void)key; return false; }
    virtual bool GiveItem(const std::string& key, int n) { (void)key; (void)n; return false; }   // add n of "@Plugin|id" to the player (ARMORY gear)          // equip (= consume) an ALCH, "@Plugin|id"
    // Player state for native conditions: "sneaking", "combat", "sprinting", "aiming", "airborne".
    virtual bool PlayerIs(const std::string& what) { (void)what; return false; }
    // True while the player cannot read a HUD message: a gameplay menu, the ripperdoc view or the install scene is
    // up. D1 holds the benched-quickhacks notice until this is false, so it never plays behind the menu (QD N7).
    virtual bool UiBusy() { return false; }
    // The name of the key BOUND to the setup view right now ("F8" for players, "F7" on the test build), or "" when
    // it has none. D1's benched notice prints this, never a literal key (MENU/HUD AAA, 2026-11-10).
    virtual std::string SetupKeyName() { return ""; }
    // Quickhack target: the hostile, living actor closest to the view direction within rangeM (and in line of sight).
    struct HackTarget { bool ok = false; uint32_t id = 0; bool machine = false; double hpPct = 100; };
    virtual HackTarget FindHackTarget(double rangeM) { (void)rangeM; return {}; }
    virtual bool HackTargetValid(uint32_t id, double rangeM) { (void)id; (void)rangeM; return true; }   // still alive and in range during an upload
    // what the player sees. phase: "upload" (seconds = upload time), "applied" (seconds = how long the effect runs), "cancel"
    virtual void HackVisual(const std::string& phase, const std::string& id, uint32_t target, double seconds) { (void)phase; (void)id; (void)target; (void)seconds; }
    // up to n other living hostiles within radiusM of actor `around` (quickhack spread, CP SpreadQuickhacksOnStart)
    virtual std::vector<HackTarget> NearbyHostiles(uint32_t around, double radiusM, int n) { (void)around; (void)radiusM; (void)n; return {}; }
    virtual void TargetAV(uint32_t id, const std::string& av, double delta) { (void)id; (void)av; (void)delta; }  // < 0 damages
    // What a quickhack does beyond an actor value (the menu text is the reference, READY-abilities-aaa-1 D4/D5). kind: "blind" (the
    // target stops fighting and detects nothing for `seconds`), "stagger" (one hit reaction now), "ping" (hostiles within radiusM of
    // `target`, or of the player when target is 0, are shown for `seconds`).
    virtual void HackEffect(const std::string& kind, uint32_t target, double seconds, double radiusM) { (void)kind; (void)target; (void)seconds; (void)radiusM; }
    // A cue the player cannot miss for an ability that fires by itself (AAA-GAPS row 25: Second Heart refilled health with no
    // sound and no screen effect). phase: "trigger" (it just fired), "ready" (its cooldown is over).
    virtual void AbilityCue(const std::string& handler, const std::string& phase) { (void)handler; (void)phase; }
    virtual uint64_t Entropy() = 0;
    virtual bool DebugEnabled() { return false; }
    virtual void Log(const std::string& line) = 0;
};

// Level-based price multiplier (Settings::levelPricing; BALANCE.md "Level pricing"):
// prices grow with the player like CP items level with V. 0.55 at L1, 1.0 at L20, 1.5 at L40,
// capped 1.75 from L50.
double LevelPriceMult(int level);

// ---------------------------------------------------------------- engine
struct Derived {                         // everything computed from catalog + state + env
    StatMap totals, applied;
    std::map<std::string, StatMap> perInstance;      // uid -> stats contributed (after scoped perk mults)
    std::map<std::string, int> instanceCost;          // uid -> capacity cost after perk mults
    std::map<std::string, int> slotCounts;
    std::map<std::string, int> perkRanksEffective;    // bought + auto
    std::set<std::string> abilities;
    int capacityMax = 0, capacityUsed = 0, capacityBonus = 0, capacityHardMax = 0;
    double overCapMaxPct = 0, overCapHealthPct = 0, overCapPenalty = 0;
    std::string overCapStat;
    int extraRolls = 0;
    double rerollCostMult = 1.0;
    bool rerollKeepOld = false;
    std::map<std::string, int> abilityTier;   // ability -> highest granting tier
    int perkPointsTotal = 0, perkPointsSpent = 0;
    int level = 0;
    Settings settings;                   // copied from the state it was computed from (prices read it)
    std::vector<std::pair<const PerkEffect*, int>> activeEffects;   // (effect, rank) of every active cyber perk
    Desired desired;
};

// ARMORY gear (docs/ARMORY.md): a weapon / armour / consumable from "Cyberware - Armory.esp" that a ripperdoc sells as a
// plain inventory item. Not catalog cyberware: no state, no slot. Loaded by the plugin from armory-formmap.json's stock
// (stock-ready entries only); none when the Armory plugin is absent.
struct GearDef {
    std::string id;      // EditorID, e.g. CW_AR_Yukimura
    std::string kind;    // weapon | armor | consumable
    std::string name;
    std::string doc;     // ripperdoc id that sells it
    std::string key;     // "@Cyberware - Armory.esp|xxx" for Env::GiveItem
    int64_t price = 0;   // before the doc's priceMult
    std::string description;   // what it does (the stock row's `description`, or its older mixed `desc`); "" = none
    std::string lore;          // what it is (the stock row's `lore`); "" = none. Both go to the page as they are.
};

class Engine {
public:
    Engine(Catalog cat, Env* env);

    const Catalog& catalog() const { return cat_; }
    State& state() { return state_; }
    const State& state() const { return state_; }

    void NewGame();                               // fresh state (revert)
    void LoadState(const json& j);                // from cosave; tolerant
    json SaveState() const { return state_.ToJson(); }

    // Recompute and push the desired perk set to the game. Call after load and after every commit.
    void Sync();

    Derived Compute(const State& s) const;

    // The ripperdoc whose menu is open (session only, not saved).
    std::string currentRipperdoc;

    // Operations. Each runs on a copy; on success the copy is committed (unless dryRun) and Sync() runs.
    json Run(const std::string& op, const json& args, bool dryRun);

    json StateView(const State& s, const Derived& d, std::optional<int64_t> currency = std::nullopt) const;
    json ShopView(const State& s, const Derived& d, std::optional<int64_t> currency = std::nullopt) const;
    json CatalogView() const { return cat_.raw; }
    void SetGear(std::vector<GearDef> g) { gear_ = std::move(g); }
    const std::vector<GearDef>& gear() const { return gear_; }

    // COMPUTE (track A): the actor-value modifiers the game must hold right now for installed computed abilities above
    // the perk tier clamp, summed per actor value, counting only effects whose conditions hold at this instant. The
    // plugin applies these as a ledger; cwtest and BETA QA's --validate read the same function, so what the shop says,
    // what the check reads and what the game applies are one number.
    std::map<std::string, double> TierDeltaAvNow() const;
    // The deltas themselves are resolved once per Sync() (the installed set can only change on a commit or a load) and
    // only their CONDITIONS are evaluated per tick: TierDeltaAvNow walks this small vector, never the catalog. Without
    // that split a per-frame call would re-derive the whole catalog, which is the kind of cost nobody measures until a
    // firefight (see the AB-E1 row).
    // One resolved delta: which ability owns it, whether that ability's perk is TIMED (a burst, a Sandevistan), and the
    // actor-value effect itself. A timed ability's baked perk is on the player only while its timer runs, so its delta
    // must be too, or a 5 s burst's bonus becomes permanent - which the first version of this did.
    struct TierDelta { std::string ability; bool timed = false; NatAvFx fx; };
    const std::vector<TierDelta>& tierDeltas() const { return tierDeltas_; }

    // SECOND BANDS (BETA QA's truth list, 2026-11-10). A few abilities state their effect as TWO bands and
    // only the first was ever applied, because Num() reads element 0. Where the second band's condition is one a perk
    // can carry it is baked (Threatevac: a deeper health band, cw_effects.cpp). Where it is NOT - Jenkin's Tendons
    // needs "sprinting, and the 5 s burst is over", which no perk condition can see - the DLL applies it, and this is
    // the function that decides it, in the core so cwtest can prove it.
    std::map<std::string, double> SecondBandAvNow() const;

    int RedeemChips();                            // pull loot tokens out of the inventory into the stash
    int RedeemCapacityItems();                    // consume capacity items in the inventory, up to each one's max

    // Native abilities (cw_native.cpp). The plugin feeds keys, kills, hits and ticks; timed perks come out of Compute.
    json NativeKey(const std::string& action);    // "os" | "camo" | "scan" | "heal" | "hack" | "hack_next"
    void NativeKill(bool melee, bool sneak);
    void NativeHit(const std::vector<std::string>& weaponKeywords, bool melee = false);
    bool NativeTick();                            // true when a timed effect ended (already synced)
    // Data Tunneling: copy the quickhack effects running on `from` (debuffs, DoTs) to each of `to`. Returns how many got one.
    int SpreadHacks(uint32_t from, const std::vector<uint32_t>& to);
    // A save never holds a running quickhack debuff: the target's damage is given back at kPreSaveGame and taken again at
    // kPostSaveGame. The list lives in memory only, so a load or new game drops it (the loaded save holds none).
    void HacksPreSave();
    void HacksPostSave();
    void HacksOnLoad();
    // D2 (2026-11-10): a save or a load never cancels a running quickhack. HacksSave() is the cosave form (time LEFT, never
    // clock times); HacksRestore() re-applies it after a load. The caller resolves form ids and drops dead or missing targets.
    json HacksSave() const;
    int HacksRestore(const json& saved);
    json HackSelected();   // { name, cost } of the hack the key will upload, null with no deck
    void NativeHurt();                            // the player was hit: run the HP checks now, not at the next tick
    void NativeEvent(const std::string& ev);      // start every Buff/CondSlow whose trigger is ev
    json NativeView() const;
    // Cyberdeck RAM (session only: full on load) and the quickhacks the installed deck holds, in slot order.
    double RamMax(const Derived& d) const;
    std::vector<std::string> DeckQuickhacks(const Derived& d) const;
    std::vector<std::string> DeckQuickhacks(const State& s, const Derived& d) const;
    // D1: the player's EFFECTIVE ranking - stored valid rankable ids, then every remaining rankable catalog hack in
    // the built-in default order. Always total, so a first edit can neither empty nor scramble the bench.
    std::vector<std::string> LoadoutOrder(const State& s) const;
    static bool HackRankable(const std::string& id);   // false for a deck's fixed program (Blackwall: Canto only)
    json QuickhacksView(const State& s, const Derived& d) const;        // StateView's `quickhacks` block
    void LoadoutSet(State& s, const json& args) const;                  // the setLoadout op; throws CwError
    bool HacksBusy() const;             // an upload or a hack effect is running (setLoadout is refused: E_BUSY)
    // The lead's MN-28 ruling: a hack the deck can NEVER pay for stays slottable and is marked unfireable. ONE place
    // produces the words, so the HUD refusal and the screen's row say exactly the same thing:
    // "needs 12 RAM, this deck holds 8". Empty string when the hack can be paid for on this deck.
    std::string HackUnfireableWhy(const Derived& d, const json& def) const;
    int LoadoutBenched(const Derived& d) const;                         // how many ranked hacks the deck cannot hold
    double Ram() const { return ram_; }
    // ARMORY consumables (docs/ARMORY.md 2a): "ram_restore" = half the deck's RAM now; "ram_regen" = +0.5 RAM/s for `seconds`.
    // Returns false (and says so on the HUD) when no cyberdeck is installed: the item is eaten, nothing happens.
    bool NativeConsume(const std::string& what, double seconds);
    double RamMaxNow() { return RamMax(Compute(state_)); }
    double DeckParam(const std::string& key);     // the installed cyberdeck's numeric param (0 when none / absent); plugin runtime hooks
    // Running quickhack effects for the HUD queue: [{ quickhack target, av, secondsLeft }] (CP 2.x: no upload, no cooldown)
    json HackView() const;

private:
    std::vector<TierDelta> tierDeltas_;   // COMPUTE: resolved in Sync(), conditions evaluated per tick
    Catalog cat_;
    std::vector<GearDef> gear_;
    Env* env_;
    State state_;
    std::map<std::string, NativeTimer> timers_;   // per ability, session only
    struct HpSample { int64_t t; double pct; };
    std::vector<HpSample> hpHistory_;             // Countershell: HP over the last few seconds
    bool wasCombat_ = false;
    // D1 (QD R4): tell the player once when a newly installed deck cannot hold everything they ranked. The first
    // tick after a load records the deck WITHOUT a notice, so loading a save never re-announces it.
    bool deckSeen_ = false;
    std::string lastDeck_;
    int benchNotice_ = 0;               // hacks benched by the deck just installed; shown once UiBusy() goes false
    int64_t lastSprintMs_ = -1000000, lastTickMs_ = 0;
    int lastStimpaks_ = -1;
    void HpChecks(const Derived& d);
    double ram_ = -1;
    int64_t ramBoostUntil_ = 0;                   // ARMORY ram_regen: extra RAM/s until this NowMs
    double ramBoost_ = 0;                             // < 0: fill on the next tick
    // D1: the selection is state_.hackSelected (an id), not an index, so reordering never moves the player's pick
    // to a different hack. HackSel resolves it against the deck's current slot order.
    size_t HackSel(const std::vector<std::string>& q) const;
    struct HackFx { uint32_t target; std::string av; double amount, perSec; int64_t until; std::string effect = {}; };   // effect: the Env::HackEffect kind this debuff asked for ("blind"), asked again when a load restores it
    std::vector<HackFx> hackFx_;                  // timed debuffs (restored at the end) and damage over time
    struct HackUpload { std::string id; uint32_t target = 0; bool machine = false; double hpPct = 100, cost = 0; int64_t start = 0, end = 0; };
    std::optional<HackUpload> upload_;            // a quickhack on its way: RAM is paid, the effect lands at `end` unless the target is lost
    bool hacksHeld_ = false;                      // between pre-save and post-save: debuffs are given back, nothing ends or ticks
    json Quickhack(const Derived& d, const HackUpload* done = nullptr);
    void RamTick(const Derived& d, double dt);
    const NativeAbility* ActiveNative(NativeFamily f, const Derived& d) const;
    json Trigger(const NativeAbility& n, const Derived& d, bool automatic);
    void CutCooldowns(double pct);
    void Notify(const std::string& text);

    std::string NewUid(State& s, char prefix) const;
    static uint64_t NextRand(State& s);
    std::vector<Roll> RollGroups(State& s, const std::vector<RollGroup>& groups, int extra = 0) const;
    // prevUsed >= 0: a state already over capacity is grandfathered (never auto-stripped, it pays
    // the over-cap penalty) — but only while the op neither grows capacityUsed nor shrinks
    // hardMax below prevHardMax (so a perk refund that would break capacity stays E_CAPACITY,
    // while removing chrome from an over state, or a cosave whose curve tightened, pass).
    // prevHardMax 0 = fully lenient (loads, setSettings).
    void ValidateInvariants(const State& s, const Derived& d, int prevUsed = -1, int prevHardMax = 0) const;
    int64_t Price(int64_t base, const ItemDef* scopeItem, const Derived& d) const;  // ripperdoc + perk cost mults + settings (priceMult, level pricing)
    int64_t SellPrice(int64_t base, const Derived& d) const;
    void RequireDoc() const;
    const RipperdocDef* OpenDoc() const;
    bool InStock(const std::vector<StockEntry>& list, const std::string& id, int tier) const;
};

// ---------------------------------------------------------------- JSON API (docs/UI-API.md)
class Api {
public:
    explicit Api(Engine* e) : engine_(e) {}
    // One request string in, one response string out. Never throws.
    std::string Handle(const std::string& request);
    json HandleJson(const json& req);
    // Payload for window.cwOnEvent.
    json Event(const std::string& type) const;
    std::string pluginVersion = "0.1.0", buildId = "dev";
    std::function<void()> onClose;     // set by the plugin: hides the view
private:
    Engine* engine_;
};


// ---------------------------------------------------------------- arm cyberware (ARMS): drawn visual + Mantis leap
// Game independent: the plugin feeds positions and anim-event tags, these decide. Units are game units (1 m ~ 70).
struct Vec3 { double x = 0, y = 0, z = 0; };
struct LeapParams {
    double minUnits = 280, maxUnits = 840;   // target 4-12 m ahead (ARMS, arms/data/weapons_arms.json "leap")
    double coneDeg = 10;                     // within ~10 deg of the view heading
    double stopShort = 85;                   // the lunge ends ~1.2 m before the target
    double arcMax = 40;                      // a small hop: peak height min(arcMax, 0.1 * distance)
    double launchS = 0.100, impactS = 0.633; // weaponSwing -> HitFrame in the leap clip
    // bLeapLaunch (2026-11-09, the author: "why was that called a leap, the arms just kinda moved"): a real ballistic launch
    // through the character controller instead of the SetPosition slide. Apex above the start = clamp(apexBase +
    // apexPerUnit * travel, apexMin, apexMax); gravity in game units/s^2 (9.8 m/s^2 x 70).
    double apexBase = 40, apexPerUnit = 0.12, apexMin = 60, apexMax = 140, gravity = 686;
    // Authored leap (QUALITY-BAR MAN-3..8, Quality Director 2026-11-09, after CP's NPC leap clip: load-up, +3.3 m in 0.2 s,
    // ~0.6 s hang, 0.27 s drop, deep landing). Peak above the start = clamp(profApexBase + profApexPerUnit * travel,
    // profApexMin, profApexMax): 105 u for a 4 m target, 175 u for 8 m. The hang grows with the travel.
    double loadS = 0.15, riseS = 0.20, dropS = 0.27, hangMinS = 0.25, hangMaxS = 0.60;
    double profApexBase = 56.25, profApexPerUnit = 0.25, profApexMin = 90, profApexMax = 220;
};
// The authored leap: where the player should be tSecs after the launch (weaponSwing), and the profile's length.
struct LeapShape { double apex = 0, hangS = 0, totalS = 0, travel = 0; };
LeapShape LeapProfileShape(const Vec3& start, const Vec3& target, const LeapParams& p = {});
Vec3 LeapProfilePos(const Vec3& start, const Vec3& target, double tSecs, const LeapParams& p = {});
// A ballistic launch from start that lands stopShort before target: launch velocity (game units/s), flight time, apex.
struct LaunchPlan { Vec3 v; double flightS = 0, apex = 0, travel = 0; };
LaunchPlan PlanLaunch(const Vec3& start, const Vec3& target, const LeapParams& p = {});
struct LeapCandidate { Vec3 pos; bool hostile = false, alive = false, los = false; };
// Nearest valid target in range and inside the cone around headingRad (FO4: 0 = +Y, clockwise), or -1.
int PickLeapTarget(const Vec3& from, double headingRad, const std::vector<LeapCandidate>& c, const LeapParams& p = {});
// Where the player is at t in [0,1] of the lunge (weaponSwing = 0, HitFrame = 1).
Vec3 LungePos(const Vec3& start, const Vec3& target, double t, const LeapParams& p = {});
double LungeSeconds(const LeapParams& p = {});

// The catalog-wide tier ladder (BETA QA 2, 2026-11-10; tools/tier_ladder.py writes the rule into the catalog as
// top-level `tierLadder`). One line per problem, for cwtest --validate:
//  - the effect table has seven strictly rising entries with T5 = 1.0;
//  - no ability still relies on scaleWithTier, and every ability with numeric params has explicit paramsT<n> for each tier
//    it is sold at (the engine's fallback to the unscaled T5 values made the cheapest tier equal T5);
//  - no sold tier of an item is identical to the one below (stats, ability numbers, mod slots, capacity grant);
//  - no native param gets worse from one sold tier to the next.
// A catalog without `tierLadder` (the hand-written test catalogs) is not checked: returns empty.
std::vector<std::string> TierLadderProblems(const Catalog& c);
// Which native params a baked CW_Nat perk carries (changing them changes EffectsFor), per ability, and the highest tier a
// perk exists for (TimedPerkId's clamp: 5 today). A perk-carried param cannot differ above that tier in game, whatever the
// catalog says: the shop text and the ladder must use the clamp tier's value there until perks for T5+ / T5++ exist
// (abilities defect D9). cwtest --perk-keys writes this as data/perk-keys.json for tools/tier_ladder.py; --validate fails
// when the catalog's copy (tierLadder.perkKeys / perkTiers) is stale or a perk-carried param differs above the clamp.
std::map<std::string, std::vector<std::string>> PerkCarriedKeys(const Catalog& c);
int PerkTierClamp();

// ---- COMPUTE: the tiers above the perk clamp that the DLL applies instead of a baked perk ----
// The lead's ruling (2026-11-10, corrected): a perk exists only up to PerkTierClamp(), so above it the engine
// runs the clamp tier's perk. For a COMPUTED ability the DLL adds the difference between the catalog's explicit
// paramsT<t> and the clamp tier's as a runtime ACTOR-VALUE modifier. That changes an actor, never a form, which is his
// condition (1); and the numbers are READ from the catalog, never recomputed from a ladder, which is his condition (2).
// ARMORY owns this list; cwtest --perk-keys writes it into data/perk-keys.json as "computed" so it cannot drift from
// the code, and tools/tier_ladder.py then leaves those tiers unclamped, so the tier returns to sale.
const std::set<std::string>& ComputedAbilities();

// The actor-value deltas the DLL must apply for `ability` installed at `tier`, as (av, value, conditions).
// Empty unless the ability is computed, the tier is above the clamp, and the catalog states explicit params for it.
std::vector<NatAvFx> TierDeltaAvs(const Catalog& c, const std::string& ability, int tier);

// --validate: every ability in ComputedAbilities() must really have a compute path in this build (the lead's check
// that the list and the code agree). A tier whose difference is NOT expressible as an actor-value delta is a problem,
// not a silent skip: that is the "no runtime mutation of game forms" condition made mechanical.
std::vector<std::string> ComputedAbilityProblems(const Catalog& c);

// Arm weapon damage by tier (BETA QA 2, 2026-11-10). The catalog is the one source of truth: an installed arm ability's
// params are already resolved for its tier (paramsT<tier>), and its base_dmg is the damage the arm WEAP (or the PLS
// explosion) must do while that tier is installed. The plugin writes it to the form at install, upgrade and load and
// puts the record's own value back at uninstall. Returns 0 for anything that is not an arm weapon with a base_dmg.
int ArmWeaponDamage(const NativeAbility& a);
// PLS element rounds (ARMORY, the lead, 2026-11-10): which rounds EXIST is the catalog's, never a literal in code. One entry per
// [Arms] sPlsRoundsShipped: WHICH launcher round mods a build offers at all (the lead, 2026-11-10: a round ships only if every
// material it swaps is graded; an ungraded round ships OFF). `list` is the ini value, comma separated; `round` is a catalog
// suffix (explosive, elec, therm, chem, incendiary, tranq) or an element name (electrical, thermal, chemical, tranquilizing).
// Pure, so the plugin's gate is testable in cwtest: the plugin reads the ini and the game, these two decide.
//   PlsRoundListed - is the round named in the list at all?
//   PlsRoundShips  - does it SHIP? Listed, AND the thermal/incendiary pair rule: `incendiary` wears the THERMAL materials, so
//                    the two swap the same files, are ONE gradable unit, and neither ships unless BOTH are named.
// An empty list ships nothing, which is also the plugin's default, so a missing key fails CLOSED. An entry that matches no
// round simply matches nothing, so a typo ships FEWER rounds and never more.
bool PlsRoundListed(const std::string& list, const std::string& round);
bool PlsRoundShips(const std::string& list, const std::string& round);
// The catalog half of that gate, PURE so cwtest proves it on the real catalog: drop every launcher round `list` does not ship
// (ability ab_mod_pls_round_<r> and mod cwm_pls_round_<r>) from the catalog JSON BEFORE Catalog::FromJson, AND every reference
// to a dropped id (a ripperdoc's modStock, item and mod tier abilities, an item's iconicAbilities, a cyber perk's ability
// effect). Why (b7 beta rehearsal, operator): with sPlsRoundsShipped='none' the six mods were dropped but
// ripperdoc 'diamondcity' modStock still listed them, so FromJson threw E_CATALOG and the core never loaded. Returns the
// number of entries removed; `log` gets one line per removal saying what and where.
int PlsFilterCatalog(json& j, const std::string& list, std::vector<std::string>* log = nullptr);
// [Arms] sPlsRoundsShipped's default when no ini names it: EMPTY = no round ships, so a missing key fails CLOSED. ONE owner:
// the plugin's ReadIni and `cwtest --validate <catalog> --ini <ini>` both use it, so the boot-free check cannot drift.
inline constexpr const char* kPlsRoundsShippedDefault = "";
// One ini value the way the plugin reads it (GetPrivateProfileStringA): section and key match case-insensitively, the FIRST
// matching key of the FIRST matching section wins, ';' / '#' start a comment line only (never mid-value), the value is trimmed
// and one pair of matching surrounding quotes is removed. `found` says whether the key was there (the overlay rule: a key the
// Test overlay does not name keeps the main ini's value). Pure, for cwtest's --ini mode; the plugin keeps the Win32 call.
std::string IniValue(const std::string& text, const std::string& section, const std::string& key, const std::string& def, bool* found = nullptr);

// cwm_pls_round_* mod: its ability's own spec.native.handler (the catalog's short name, mod_pls_round_elec), the round from
// paramsT5.round (the long name espgen keys pls.explosions by, "electrical"), and the element look the arm wears ("incendiary"
// wears "thermal": it has no material of its own). Mod file order. A mod whose ability has no handler or round is skipped and
// named in `problems` when given. Which rounds SHIP is a different question, answered by the ini ([Arms] sPlsRoundsShipped).
struct PlsRoundDef { std::string mod, handler, round, look; };
std::vector<PlsRoundDef> PlsRounds(const Catalog& c, std::vector<std::string>* problems = nullptr);
// The ar_* handler an arm WEAP record belongs to (espgen's table: CW_MantisBlades, CW_GorillaArms, CW_Monowire), "" else.
std::string ArmHandlerOfWeapon(const std::string& weapEditorId);
// The tier effect factor the arm ladders use (catalog_data.ARM_TIER_FACTOR): 0.40 / 0.55 / 0.70 / 0.85 / 1.00 / 1.10 / 1.20.
// The Gorilla slam scales its damage by it (it stopped at T5: 1.0 for T5+ and T5++).
double ArmTierFactor(int tier);
// A per-tier table (weapons_arms.json ragdollByTier): the entry for the tier, the LAST entry when the tier is past the
// table (a T5+ Gorilla must not fall back to the stagger value and lose its knockback), fallback when the table is empty.
double ArmLadderAt(const std::vector<double>& byTier, int tier, double fallback);
// Build-time check (cwtest --validate, espgen): one line per problem.
//  - catalog only: every arm weapon's base_dmg rises at every tier it is sold at (an upgrade must buy hit damage) and its
//    speed and reach are the same at every tier (nothing scales them by tier);
//  - with arms/data/weapons_arms.json (weaponsArms != nullptr): each arm WEAP's stats_proposal damage, speed and reach
//    equal the catalog's T5 base_dmg, speed and reach, and every catalog arm weapon has its record.
std::vector<std::string> ArmRecordProblems(const Catalog& c, const json* weaponsArms);

// PLS charges as CP's stat pool (TweakDB BaseStatPools.ProjectileLauncherCharges: a 0-100% pool, initial 100, regen
// from 0 to 100 over ProjectileLauncherRechargeDuration / (1 + ChargesRegenMult); one charge = 1/max of it). pool is
// 0..1. Given the charges the player holds now (count; a throw spends one) returns the new pool and how many to add.
struct PlsPool { double pool = 1; int add = 0; };
PlsPool PlsPoolStep(double pool, int count, int lastCount, int maxCharges, double dtS, double rechargeS, double regenPct);

// Runtime modifiers the plugin puts on the player or the equipped weapon (Rangeguard DR, reload / melee speed). The save
// must never contain them, or a load would add them twice (the lead, 2026-11-08): the plugin Strip()s every one at
// kPreSaveGame (undo the returned deltas) and Restore()s them at kPostSaveGame. After a load the ledger starts empty,
// which matches a save that holds none.
struct RuntimeMods {
    std::map<std::string, double> applied;    // key -> the amount applied now
    std::map<std::string, double> stripped;   // between pre-save and post-save
    double Set(const std::string& key, double want);   // returns the delta to apply now
    std::map<std::string, double> Strip();             // amounts to UNDO before the save (ledger moves to `stripped`)
    std::map<std::string, double> Restore();           // amounts to re-apply after the save
    void Reset() { applied.clear(); stripped.clear(); }   // a load / new game
};

// Vulnerability Analytics / Machine Learning (CP relic perks) on FO4 limbs (the lead, 2026-11-08: map CP's weak points onto
// the limb system). Each hostile the player fights gets one vulnerable limb group (head, torso, left/right arm, left/right
// leg) that moves every intervalS. A hit there: CP 100% crit + 25% armor pen = bonus damage dealt x (critMult x 1.25 - 1).
// Enough damage there (popFrac of the target's max HP) destroys it: an EMP (3 m, CP) and a Machine Learning stack
// (+10% frequency, +5% crit damage vs vulnerabilities, 25 s, 5 stacks, doubled at 5). Limb = FO4 LIMB_ENUM.
int LimbGroup(int limbEnum);   // 0 torso, 1 head, 2 left arm, 3 right arm, 4 left leg, 5 right leg, -1 none
struct WeakPoints {
    struct T { int group = -1; double hit = 0; int64_t nextAt = 0; };
    std::map<uint32_t, T> targets;
    int stacks = 0; int64_t stacksUntil = 0;
    double intervalS = 8, popFrac = 0.25;     // estimates: CP gives no frequency or vulnerability health
    struct Hit { bool onWeakPoint = false, destroyed = false; double bonus = 0; int group = -1; };
    Hit OnHit(uint32_t target, int limbEnum, double dealt, double targetMaxHp, int64_t nowMs, double critMult, bool machineLearning, uint64_t rnd);
};

// Finishers (ARMS finishers.json, the lead, 2026-11-08). Eligible: a power-attack hit by the matching arm on a human at or
// below hpMax (0.25) that is unaware (not in combat), staggered by the hit, or sneak-attacked.
struct FinisherHit { bool human = false, power = false, unaware = false, staggered = false, sneak = false, alive = true; double hpPct = 1; };
bool FinisherEligible(const FinisherHit& h, double hpMax = 0.25);
// Where the victim stands: (x, y) in the player's start frame (+Y = the player's facing; FO4 heading 0 = +Y, clockwise),
// facing player heading + yawDeg. Returns {x, y, heading} in world space.
struct Placed { double x = 0, y = 0, heading = 0; };
Placed FinisherPlace(double px, double py, double headingRad, double lx, double ly, double yawDeg);

// ---- The engine's measured damage model, and why the finisher hold no longer writes a damage field.
// FINISHERS, 2026-11-10, from boot p15b-1 of build d50d6620 (docs/FINISHER-ROWS.md part 1). `applied` is the Health drop
// the plugin measures across Actor::DoHitMe. Two independent factors, both the engine's:
//   (A) the armour curve, the QUALITY DIRECTOR's H2c ruling, limb-independent for limbs 0, 6 and 8 and unchanged by power
//       attacks: ratio = 0.563 * (damage / 42.1) ^ 0.366. Predicts 0.651 / 0.786 / 0.881 against 0.652 / 0.787 / 0.881.
//   (B) limb 1 (the head) applies a flat 5x on top of it: 709.0 / (157.2 * 0.881) = 5.12 and 413.1 / (104.8 * 0.787) = 5.01.
// The old hold wrote HitData::totalDamage = (hp - 1) / ratio and trusted the engine to scale it. It cannot work: the SAME
// written value (39.8, 200 hp raider, limb 1, same weapon) gave applied 0.0 once and 3222.1 once, so the engine does not
// read that field for the Health it applies, and writing it also perturbs the engine's arithmetic over the other six damage
// fields (3222.1 is 4.65x even the un-held prediction of 692.5). No value of `ratio` survives a 0.0-to-3222.1 spread.
// So: write nothing, raise the victim's Health BEFORE the engine applies the hit, and let the finisher's killAt kill.
double EngineArmourFactor(double totalDamage);        // (A)
double EngineLimbFactor(int limbEnum);                // (B): 5.0 for limb 1, 1.0 otherwise
double PredictApplied(double totalDamage, int limbEnum);   // (A) * (B) * totalDamage
// Does a victim on `hp` live through a hit the engine applies as `engineApplied`, given a Health buffer of `buffer`?
// The buffer must cover the worst figure ever observed (3222.1) with room to spare, because the engine's number cannot be
// predicted from the HitData we are handed.
bool HoldSurvives(double hp, double buffer, double engineApplied);
// What the hold is allowed to do. `writeDamageField` must stay false for ever: see above.
struct FinisherHoldPlan {
    bool buffer = false;            // raise Health before the engine call
    double bufferAmount = 0;        // by this much
    bool writeDamageField = false;  // never
    double netDamage = 0;           // the blow the held-kill net applies if the finisher never runs: the ORIGINAL hit
};
FinisherHoldPlan PlanFinisherHold(double victimHp, double hitTotalDamage, double bufferAmount);
// Sliding the victim into its placement instead of teleporting it (FIN-1). Returns the position at step `step` of `steps`
// (step 0 = where it stands, step == steps = the placement), easing out so the pull decelerates into the grab.
struct Slide { double x = 0, y = 0, heading = 0; };
Slide FinisherSlideStep(double fromX, double fromY, double fromHeading, const Placed& to, int step, int steps);
// How many steps a slide needs so no single step exceeds maxStepU units or maxTurnDeg degrees.
int FinisherSlideSteps(double fromX, double fromY, double fromHeading, const Placed& to, double maxStepU, double maxTurnDeg, int cap);
// Pinning the victim to its CHOREOGRAPHED position for the set piece (FIN-2 / FIN-5). Observed: pushed from 75 u to 285 u
// during one finisher, a 210 u drift, so the blade was on it for 5 frames of 5.5 s.
//
// It cannot be pinned to a fixed point. Three of the six victim clips end where they began (Mantis01, Monowire02, Gorilla01
// and Gorilla02 all have rootMotionEnd 0), but Mantis02's travels 112 u and Monowire01's 93 u BY DESIGN -- CP's victim keeps
// falling after the player is done. So the target is the placement plus the clip's own root motion at that moment.
// FinisherChoreographed interpolates it LINEARLY from the placement to placement + rootMotionEnd over victimSeconds, in the
// player's frame. For a clip whose rootMotionEnd is 0 that is exact; for the two that travel it is an approximation of CP's
// curve, and the QUALITY DIRECTOR's 15 u bar is judged against it on that understanding.
Placed FinisherChoreographed(const Placed& placed, double headingRad, double rmX, double rmY, double elapsedS, double victimS);
// Returns true and fills px / py when the victim is further than maxDriftU from where the choreography wants it.
bool FinisherPin(double vx, double vy, const Placed& want, double maxDriftU, double& px, double& py);
double FinisherDrift(double vx, double vy, const Placed& want);

// Every way a finisher's hold can end (the lead, 2026-11-10 condition 1: "the buffer can never be left on an actor. An enemy
// alive with 100000 extra health is worse than no finisher"). `liftApplied` is what separates the two designs: a STANDING
// buffer is applied across frames, so every one of these exits has to remember to take it off; the lift this build ships
// lives inside one Actor::DoHitMe call and is gone before any of them can happen, so it is always false.
enum class FinisherExit {
    Completed,        // killAt killed the victim and the clip ended
    NeverStarted,     // the idle was refused, or the task never ran (QD FN2's "gone or dead by the task")
    Interrupted,      // the player was staggered or killed, or the victim was knocked out of the set piece
    VictimGone,       // the victim unloaded, or the cell changed
    SavedAndLoaded,   // a save was made mid-finisher and loaded again
    VictimEssential,  // the victim is essential or protected and cannot be killed
    SecondFinisher,   // another finisher was decided in the same frame
    GameClosed,       // the process ended mid-finisher
};
struct FinisherState { bool active = false; bool liftApplied = false; double floorHp = 0; bool netArmed = false; double netDamage = 0; };
struct FinisherExitResult {
    bool liftOutstanding = false;   // MUST be false after every exit: an actor left with the lift on it
    bool netOwesKill = false;       // the held-kill net still has to apply the original blow
    double netDamage = 0;           // and it is the ORIGINAL blow, never a held figure
    bool victimStuckAtOne = false;  // MUST be false: a victim left on 1 hp, which the old design's `keep` aimed at
    bool victimImmortal = false;    // MUST be false
};
FinisherExitResult FinisherOnExit(const FinisherState& s, FinisherExit e);

// Hack-key machine aim. AimError: radians between the view heading and the nearest edge of a target of radius hullU (0 = the heading
// crosses its hull). BetterAim: the smaller error wins; on a tie the nearer target wins, because it stands in front of the other.
// Kiroshi hold zoom. ZoomTarget: the level for how long the key is held, capped at the optic's own zoom. ZoomEase: frame-rate
// independent easing toward it. ZoomFovDeg: the field of view that gives that optical magnification.
double ZoomTarget(double heldS, double maxZoom);
double ZoomEase(double current, double target, double dtS);
double ZoomFovDeg(double baseDeg, double zoom);
double AimError(double px, double py, double headingRad, double tx, double ty, double hullU);
bool BetterAim(double err, double dist, double bestErr, double bestDist);

// The drawn-weapon visual (CW_MantisBladesVisual): on at weaponDraw, off at weaponSheathe. Returns +1 equip,
// -1 unequip, 0 nothing. `visualWeapon` = the drawn weapon carries a keyword that has a visual.
struct ArmsVisualState {
    bool on = false;
    std::uint64_t mismatchSince = 0;   // ms the drawn state first disagreed with `on` (0 = agrees)
    int OnAnimEvent(const std::string& tag, bool visualWeapon);
    // after a load, or a missed event. Acts only once the disagreement has lasted graceMs: the drawn flag flips at
    // BeginWeaponSheathe / BeginWeaponDraw, ~0.5 s before the weaponSheathe / weaponDraw events that bracket ARMS's
    // fold, so an instant reconcile removed the blades before the 1P fold played (in game 2026-11-08).
    int Reconcile(bool weaponDrawn, bool visualWeapon, std::uint64_t nowMs = 0, std::uint64_t graceMs = 0);
};

// A FAILED CORE LOAD leaves the plugin with no Engine (LoadCore never assigns g_engine), so every menu and seam path must
// then no-op or refuse, never dereference it. b7 beta rehearsal (operator): the catalog failed, the runner's seam
// "close" reached CloseMenu, and g_engine->currentRipperdoc.clear() crashed the game (Cyberware.dll+0098DA0, main.cpp:986).
// These two are the ONLY way CloseMenu and the "scene" seam touch the visit's doc; cwtest calls them with a null engine.
//   VisitDoc    - the doc of the visit in progress, "" when there is no engine (or no visit)
//   EndVisitDoc - clears it; false (and nothing touched) when there is no engine
inline std::string VisitDoc(const Engine* e) { return e ? e->currentRipperdoc : std::string(); }
inline bool EndVisitDoc(Engine* e) { if (!e) return false; e->currentRipperdoc.clear(); return true; }

}  // namespace cw
