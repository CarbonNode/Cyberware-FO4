// tint.cpp - per-ref colour and brightness of the emissive on the worn cyberware (docs/UI-API.md section 9).
//
// How it works: the visual ARMOs are worn by the player only. Every BSGeometry under them whose BSLightingShaderProperty
// has its own emittance (kOwnEmit, from BGSM emitEnabled) gets *emitColor and emitColorScale written per property. The
// BGSM on disk and the material cache are not touched: the colour lives on the property of this clone of the model,
// so no other item using the same BGSM can change. The authored values are kept as the baseline: glow 50 = authored.
//
// Writes are guarded: a property without kOwnEmit may point emitColor at shared engine data, so it is skipped, and a
// pointer inside the game image is refused even with the flag.
//
// Timing: Scan() runs at ~15 Hz so a drawn blade is recoloured within a frame or two of appearing. Frame() only does
// work during a pulse or with the idle glow on. Unverified parts are listed in fallout/cyberware/docs/CUSTOMIZE.md.
#include "PCH.h"

#include "tint.h"

#include <windows.h>

#ifdef ERROR
#    undef ERROR
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cctype>
#include <functional>

namespace tint {
namespace {

using Prop = RE::BSLightingShaderProperty;

struct Look { bool hasColor = false; RE::NiColor color{}; int glow = 50; };

struct Shape {
    RE::NiPointer<Prop> keep;     // holds the property alive so its address cannot be reused while it is in the map
    RE::NiPointer<RE::NiAVObject> geom;   // its geometry, for the where seam (culled or not)
    std::string target, name, key;        // key = target|1P/3P|shape name, into g_authored
    bool fp = false;                      // under the first-person biped
    RE::NiColor baseColor{};      // the material's authored emittance
    float baseScale = 0.0f;
    RE::NiColor wroteColor{};     // what we last wrote; a different value means a material swap re-authored it
    float wroteScale = 0.0f;
    bool wrote = false;
    bool eye = false;             // the player's eye shape (Kiroshi); restored to its authored values when dropped
};

std::unordered_map<std::uint32_t, std::string> g_visual;
std::map<std::string, Look> g_look;
std::unordered_map<Prop*, Shape> g_shapes;
// The authored emittance of every shape we ever held, and what we last wrote to it, for the whole game process
// (cw-customize-5). A clone made after our write can come up carrying our values instead of the material's: P15 T5
// saw the Monowire re-register after a reload as "authored (1.00,0.00,0.82) x5.00", which is magenta at glow 100, and
// the next write doubled it to the x8 cap. Taking that as the baseline compounds the glow on every re-attach and makes
// Stock unreachable. A fresh value equal to our last write is ours, so the remembered authored value stays; any other
// value is a real re-author (an element swap) and replaces it.
struct Authored { RE::NiColor color{}; float scale = 0.0f; RE::NiColor wroteColor{}; float wroteScale = 0.0f; bool wrote = false; };
std::unordered_map<std::string, Authored> g_authored;
bool g_seams = false, g_pulses = true;
std::uint64_t g_pulseAt = 0;
float g_pulseStrength = 0.0f;
std::string g_chargeTarget;
std::uint64_t g_chargeAt = 0, g_chargeFireAt = 0;   // 0 = idle
int g_skippedNoEmit = 0;
// Kiroshi eye glow. Only with a Kiroshi installed and a colour chosen, and never if any loaded actor shares the player's
// eye property or emit colour: g_eyesShared turns it off for the session (see EyeUnsafe).
bool g_eyesAllowed = false, g_eyesShared = false;
std::vector<std::pair<Prop*, std::string>> g_eyeCache;
std::uint64_t g_eyeNext = 0, g_eyeCheckNext = 0;

constexpr std::uint64_t kPulseMs = 900;
constexpr float kPulseGain = 1.6f;       // peak extra brightness, as a multiple of the shape's own scale
constexpr float kMaxScale = 8.0f;        // never blow out, whatever the sliders and a pulse add up to
constexpr float kIdleDepth = 0.14f;      // idle glow: +-14 % ...
constexpr double kIdlePeriodMs = 3400.0; // ... over a slow 3.4 s breath
// PLS charge glow (the lead, 2026-11-09, block 18: the launcher did not read). One target at a time brightens through the throw's
// wind-up, flashes on the shot and settles. Scales the shape's own authored emittance on its glow mask, so only the masked
// accent lines light, never the whole mesh.
constexpr float kChargeGain = 3.0f;          // wind-up peak: 1 + 3 = 4x the shape's scale (LauncherMech 1.5 -> 6, under kMaxScale)
constexpr float kChargeFlash = 4.3f;         // the shot: 1 + 4.3 = 5.3x for an instant (7.95, the most kMaxScale allows), over the 4x peak
// The re-timed 1P PLS clips (retime_pls_1p.py) fire weaponFire.2 at 0.933 s (30 types) or 1.000 s (7). QD block 24: with the
// old 450 ms ramp the glow was flat for the whole hold (about 0.5-0.9 s, the frames the launcher is held up in view), so it
// "does not build toward the shot". The ramp now spans the wind-up and ends just before the earliest shot.
constexpr std::uint64_t kChargeRampMs = 900;
constexpr float kChargeEarly = 0.3f;         // share of the gain on a fast ease-out, so the rise (0.3-0.45 s) is already lit
constexpr std::uint64_t kChargeTailMs = 450; // flash back to authored
constexpr std::uint64_t kChargeTimeoutMs = 2500;   // no shot came (interrupted, menu, swap): fall back as if fired

bool InGameImage(const void* p)
{
    static const auto range = []() {
        auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        return std::pair<const std::uint8_t*, const std::uint8_t*>(base, base + nt->OptionalHeader.SizeOfImage);
    }();
    auto* b = static_cast<const std::uint8_t*>(p);
    return b >= range.first && b < range.second;
}

bool Same(const RE::NiColor& a, const RE::NiColor& b)
{
    return std::fabs(a.r - b.r) < 0.002f && std::fabs(a.g - b.g) < 0.002f && std::fabs(a.b - b.b) < 0.002f;
}

bool ParseHex(const std::string& s, RE::NiColor& out)
{
    if (s.size() != 7 || s[0] != '#') return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str() + 1, &end, 16);
    if (!end || *end) return false;
    out = RE::NiColor((float)((v >> 16) & 255) / 255.0f, (float)((v >> 8) & 255) / 255.0f, (float)(v & 255) / 255.0f);
    return true;
}

float PulseFactor(std::uint64_t now)
{
    if (!g_pulseAt || now < g_pulseAt) return 1.0f;
    const auto dt = now - g_pulseAt;
    if (dt >= kPulseMs) return 1.0f;
    const float x = (float)dt / (float)kPulseMs;                    // fast attack (first 12 %), then an exponential tail
    const float env = x < 0.12f ? x / 0.12f : std::exp(-(x - 0.12f) * 4.2f);
    return 1.0f + kPulseGain * g_pulseStrength * env;
}

float ChargeFactor(std::uint64_t now, const std::string& target)
{
    if (!g_chargeAt || target != g_chargeTarget || now < g_chargeAt) return 1.0f;
    if (!g_chargeFireAt && now - g_chargeAt >= kChargeTimeoutMs) g_chargeFireAt = g_chargeAt + kChargeTimeoutMs;
    if (!g_chargeFireAt) {
        // a fast ease-out share lights the rise; the rest eases IN, so the glow keeps building through the hold into the shot.
        // About 1.5x at 0.2 s, 2.0x at 0.4 s, 2.5x at 0.55 s (hold starts), 3.1x at 0.7 s, 3.8x at 0.85 s, 4x at 0.9 s; then 5.3x.
        const float x = std::min(1.0f, (float)(now - g_chargeAt) / (float)kChargeRampMs);
        const float early = 1.0f - (1.0f - x) * (1.0f - x), late = x * x;
        return 1.0f + kChargeGain * (kChargeEarly * early + (1.0f - kChargeEarly) * late);
    }
    const auto dt = now - g_chargeFireAt;
    if (dt >= kChargeTailMs) { g_chargeAt = g_chargeFireAt = 0; return 1.0f; }
    return 1.0f + kChargeFlash * std::exp(-(float)dt / (float)kChargeTailMs * 4.0f);
}

float IdleFactor(std::uint64_t now)
{
    if (!g_seams) return 1.0f;
    return 1.0f + kIdleDepth * (float)std::sin((double)(now % (std::uint64_t)kIdlePeriodMs) / kIdlePeriodMs * 6.283185307);
}

void Apply(Shape& s, Prop* p, float pulse, float idle)
{
    if (!p->emitColor) return;
    // a material swap (element mod) re-authored this property: its new values become the baseline
    if (s.wrote && (!Same(*p->emitColor, s.wroteColor) || std::fabs(p->emitColorScale - s.wroteScale) > 0.01f)) {
        s.baseColor = *p->emitColor;
        s.baseScale = p->emitColorScale;
        if (!s.key.empty()) { auto& a = g_authored[s.key]; a.color = s.baseColor; a.scale = s.baseScale; a.wrote = false; }
        REX::INFO("tint: {} shape '{}' re-authored under us to ({:.2f},{:.2f},{:.2f}) x{:.2f}", s.target, s.name, s.baseColor.r, s.baseColor.g, s.baseColor.b, s.baseScale);
    }
    const auto it = g_look.find(s.target);
    const Look l = it != g_look.end() ? it->second : Look{};
    const RE::NiColor c = l.hasColor ? l.color : s.baseColor;
    float scale = l.glow <= 0 ? 0.0f : std::min(kMaxScale, s.baseScale * ((float)l.glow / 50.0f) * pulse * idle);
    if (s.wrote && Same(c, s.wroteColor) && std::fabs(scale - s.wroteScale) < 0.003f) return;
    *p->emitColor = c;
    p->emitColorScale = scale;
    s.wroteColor = c;
    s.wroteScale = scale;
    s.wrote = true;
    if (!s.key.empty()) { auto& a = g_authored[s.key]; a.wroteColor = c; a.wroteScale = scale; a.wrote = true; }
}

// The baseline for a shape seen for the first time (a new clone): the remembered authored value when the clone only
// carries our own last write (or the colour the player chose), else the clone's own value, which is then remembered.
void Baseline(Shape& s, Prop* p)
{
    const RE::NiColor now = *p->emitColor;
    const float nowScale = p->emitColorScale;
    auto it = g_authored.find(s.key);
    if (it != g_authored.end()) {
        const Authored& a = it->second;
        const bool same = Same(now, a.color) && std::fabs(nowScale - a.scale) < 0.01f;
        const auto lk = g_look.find(s.target);
        const bool ours = (a.wrote && Same(now, a.wroteColor) && std::fabs(nowScale - a.wroteScale) < 0.01f) ||
                          (lk != g_look.end() && lk->second.hasColor && Same(now, lk->second.color));
        if (same || ours) {
            s.baseColor = a.color;
            s.baseScale = a.scale;
            if (!same)
                REX::INFO("tint: {} shape '{}' {} came back at ({:.2f},{:.2f},{:.2f}) x{:.2f} (our last write), keeping authored ({:.2f},{:.2f},{:.2f}) x{:.2f}",
                          s.target, s.name, s.fp ? "1P" : "3P", now.r, now.g, now.b, nowScale, a.color.r, a.color.g, a.color.b, a.scale);
            return;
        }
    }
    s.baseColor = now;
    s.baseScale = nowScale;
    auto& a = g_authored[s.key];
    a.color = now; a.scale = nowScale; a.wrote = false;
    REX::INFO("tint: {} shape '{}' {} authored ({:.2f},{:.2f},{:.2f}) x{:.2f}", s.target, s.name, s.fp ? "1P" : "3P", now.r, now.g, now.b, nowScale);
}

// Why a shader property of a worn visual is NOT tinted, or nullptr when it is. The one rule Walk and the Worn seam share,
// so the seam reports exactly what the scan decides.
const char* SkipReason(Prop* p, const std::string& key)
{
    if (!p->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit)) return "no kOwnEmit (the BGSM's emitEnabled did not reach the clone)";
    if (!p->emitColor) return "emitColor is null";
    if (InGameImage(p->emitColor)) return "emitColor points into the game image (shared engine default)";
    // kOwnEmit with a black authored emittance (MantisArmskin, cables) has no glow map: writing a colour would light
    // the whole mesh flat (seen in the first in-game dump, 2026-11-08). A remembered non-black authored value means the
    // black is ours (it never is: glow 0 writes the scale, not the colour), so it is only skipped when nothing is known.
    if (p->emitColor->r + p->emitColor->g + p->emitColor->b < 0.001f && !g_shapes.count(p)) {
        const auto a = g_authored.find(key);
        if (a == g_authored.end() || a->second.color.r + a->second.color.g + a->second.color.b < 0.001f) return "black emittance on the clone (no glow map assumed)";
    }
    return nullptr;
}

void Walk(RE::NiAVObject* o, const std::string& target, bool fp, std::unordered_set<Prop*>& seen)
{
    if (!o) return;
    if (auto* n = o->IsNode()) {
        for (const auto& c : n->children) Walk(c.get(), target, fp, seen);
        return;
    }
    auto* g = o->IsGeometry();
    if (!g) return;
    for (auto& raw : g->properties) {
        auto* p = netimmerse_cast<Prop*>(raw.get());
        if (!p) continue;
        const std::string name = g->name.c_str() ? g->name.c_str() : "";
        const std::string key = target + (fp ? "|1P|" : "|3P|") + name;
        if (SkipReason(p, key)) { ++g_skippedNoEmit; continue; }
        seen.insert(p);
        auto it = g_shapes.find(p);
        if (it == g_shapes.end()) {
            Shape s;
            s.keep = RE::NiPointer<Prop>(p);
            s.geom = RE::NiPointer<RE::NiAVObject>(o);
            s.target = target;
            s.name = name;
            s.key = key;
            s.fp = fp;
            Baseline(s, p);
            it = g_shapes.emplace(p, std::move(s)).first;
        }
    }
}

bool IsEyeShape(const char* raw)
{
    if (!raw) return false;
    std::string n = raw;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (n.find("eyes") == std::string::npos) return false;
    return n.find("lash") == std::string::npos && n.find("wet") == std::string::npos && n.find("brow") == std::string::npos && n.find("ao") == std::string::npos;
}

void CollectEyes(RE::NiAVObject* o, std::vector<std::pair<Prop*, std::string>>& out)
{
    if (!o) return;
    if (auto* n = o->IsNode()) { for (const auto& c : n->children) CollectEyes(c.get(), out); return; }
    auto* g = o->IsGeometry();
    if (!g || !IsEyeShape(g->name.c_str())) return;
    for (auto& raw : g->properties)
        if (auto* p = netimmerse_cast<Prop*>(raw.get()))
            if (p->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit) && p->emitColor && !InGameImage(p->emitColor))
                out.emplace_back(p, g->name.c_str());
}

// True if any other loaded actor uses one of these properties, or the same emit colour storage. Writing to a shared one
// would light that NPC's eyes too, so the feature stays off. out_* report what was compared, for the log.
bool EyeUnsafe(const std::vector<std::pair<Prop*, std::string>>& mine, int* outActors = nullptr, int* outSameName = nullptr, bool log = false)
{
    int actors = 0, sameName = 0;
    bool shared = false;
    auto* pl = RE::PlayerCharacter::GetSingleton();
    if (auto* lists = RE::ProcessLists::GetSingleton()) {
        for (auto& h : lists->highActorHandles) {
            auto a = h.get();
            if (!a || a.get() == pl) continue;
            ++actors;
            std::vector<std::pair<Prop*, std::string>> theirs;
            CollectEyes(a->Get3D(false), theirs);
            for (auto& [tp, tn] : theirs) {
                for (auto& [mp, mn] : mine) {
                    if (tn == mn) ++sameName;
                    if (tp == mp || tp->emitColor == mp->emitColor) shared = true;
                }
                if (log) REX::INFO("tint: npc {:08X} eye '{}' prop {:p} emit {:p} ({:.2f},{:.2f},{:.2f}) x{:.2f}", a->GetFormID(), tn, (void*)tp, (void*)tp->emitColor, tp->emitColor->r, tp->emitColor->g, tp->emitColor->b, tp->emitColorScale);
            }
        }
    }
    if (outActors) *outActors = actors;
    if (outSameName) *outSameName = sameName;
    return shared;
}

void RestoreShape(Prop* p, const Shape& s)
{
    if (!s.wrote || !p->emitColor) return;
    *p->emitColor = s.baseColor;
    p->emitColorScale = s.baseScale;
}

template <class F> void ForEachWorn(F fn)
{
    auto* pl = RE::PlayerCharacter::GetSingleton();
    if (!pl) return;
    const RE::BipedAnim* fpb = pl->firstPersonBipedAnim.get();
    const RE::BipedAnim* tpb = pl->biped.get();
    for (const RE::BipedAnim* b : { fpb, tpb }) {
        if (!b) continue;
        for (int i = 0; i < std::to_underlying(RE::BIPED_OBJECT::kTotal); ++i) {
            const auto& o = b->object[i];
            if (!o.parent.object || !o.partClone) continue;
            const auto it = g_visual.find(o.parent.object->GetFormID());
            if (it != g_visual.end()) fn(it->second, o.partClone.get(), b == fpb);
        }
    }
}

}  // namespace

void RegisterVisual(std::uint32_t id, const std::string& target) { g_visual[id] = target; }

void SetEyesAllowed(bool on) { g_eyesAllowed = on; }

void Configure(const cw::json& r)
{
    g_seams = r.value("seams", false);
    g_pulses = r.value("pulses", true);
    g_look.clear();
    for (auto& [t, v] : r.value("targets", cw::json::object()).items()) {
        Look l;
        if (v.contains("color") && v["color"].is_string()) l.hasColor = ParseHex(v["color"].get<std::string>(), l.color);
        l.glow = v.value("glow", 50);
        g_look[t] = l;
    }
    REX::INFO("tint: configured, {} shape(s) held, idle glow {}, pulses {}", g_shapes.size(), g_seams, g_pulses);
}

void Scan()
{
    if (g_visual.empty()) return;
    std::unordered_set<Prop*> seen;
    g_skippedNoEmit = 0;
    ForEachWorn([&](const std::string& target, RE::NiAVObject* root, bool fp) { Walk(root, target, fp, seen); });
    const auto now = GetTickCount64();
    const auto kir = g_look.find("kiroshi");
    if (g_eyesAllowed && !g_eyesShared && kir != g_look.end() && kir->second.hasColor) {
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (pl && now >= g_eyeNext) {
            g_eyeNext = now + 1000;
            g_eyeCache.clear();
            CollectEyes(pl->Get3D(false), g_eyeCache);
            bool isNew = false;
            for (auto& [p, n] : g_eyeCache) if (!g_shapes.count(p)) isNew = true;
            if (isNew || now >= g_eyeCheckNext) {       // before the first write to a shape, then every 5 s (NPCs load in)
                g_eyeCheckNext = now + 5000;
                int actors = 0, same = 0;
                if (EyeUnsafe(g_eyeCache, &actors, &same)) {
                    g_eyesShared = true;
                    g_eyeCache.clear();
                    REX::WARN("tint: Kiroshi eye glow OFF: a loaded actor shares the player's eye property ({} actor(s) checked)", actors);
                } else if (isNew) {
                    REX::INFO("tint: Kiroshi eye glow on: {} eye shape(s), {} actor(s) checked, {} same-named NPC eye(s), none shared", g_eyeCache.size(), actors, same);
                }
            }
        }
        for (auto& [p, n] : g_eyeCache) {
            seen.insert(p);
            if (!g_shapes.count(p)) {
                Shape s;
                s.keep = RE::NiPointer<Prop>(p);
                s.target = "kiroshi";
                s.name = n;
                s.key = "kiroshi|3P|" + n;
                s.eye = true;
                Baseline(s, p);
                g_shapes.emplace(p, std::move(s));
            }
        }
    } else {
        g_eyeCache.clear();
    }
    for (auto it = g_shapes.begin(); it != g_shapes.end();) {
        if (seen.count(it->first)) ++it;
        else { RestoreShape(it->first, it->second); it = g_shapes.erase(it); }   // a detached clone goes back to authored: nothing that shares it keeps our colour
    }
    const float pulse = PulseFactor(now), idle = IdleFactor(now);
    for (auto& [p, s] : g_shapes) Apply(s, p, pulse * ChargeFactor(now, s.target), idle);
}

void Frame()
{
    if (g_shapes.empty()) return;
    const auto now = GetTickCount64();
    const bool pulsing = g_pulseAt && now - g_pulseAt < kPulseMs + 50;
    if (g_chargeAt && now - g_chargeAt > kChargeTimeoutMs + kChargeTailMs + 100) g_chargeAt = g_chargeFireAt = 0;   // no shape of that target held
    const bool charging = g_chargeAt != 0;
    if (!pulsing && !g_seams && !charging) return;
    const float pulse = PulseFactor(now), idle = IdleFactor(now);
    for (auto& [p, s] : g_shapes) Apply(s, p, pulse * ChargeFactor(now, s.target), idle);
}

bool Charge(const std::string& target, bool fire)
{
    const auto now = GetTickCount64();
    if (!fire) {
        g_chargeTarget = target;
        g_chargeAt = now;
        g_chargeFireAt = 0;
        return true;
    }
    if (!g_chargeAt || target != g_chargeTarget || g_chargeFireAt) return false;   // a shot with no wind-up seen: nothing to release
    g_chargeFireAt = now;
    return true;
}

bool Pulse(float strength)
{
    if (!g_pulses) return false;
    g_pulseAt = GetTickCount64();
    g_pulseStrength = std::clamp(strength, 0.0f, 1.0f);
    return true;
}

void OnLoad()
{
    g_shapes.clear();
    g_eyeCache.clear();
    g_eyeNext = g_eyeCheckNext = 0;
    g_pulseAt = 0;
    g_chargeAt = g_chargeFireAt = 0;
}

bool Color(const std::string& target, Rgb& out, int* glow)
{
    const auto it = g_look.find(target);
    if (it == g_look.end()) { if (glow) *glow = 50; return false; }
    if (glow) *glow = it->second.glow;
    if (!it->second.hasColor) return false;
    out = Rgb{ it->second.color.r, it->second.color.g, it->second.color.b };
    return true;
}

bool PulsesOn() { return g_pulses; }

std::string Stats()
{
    std::map<std::string, int> n;
    for (auto& [p, s] : g_shapes) ++n[s.target];
    std::string out = std::to_string(g_shapes.size()) + " shape(s)";
    for (auto& [t, c] : n) out += " " + t + "=" + std::to_string(c);
    return out + ", skipped (no emit) " + std::to_string(g_skippedNoEmit);
}

void Dump()
{
    REX::INFO("tint: dump: {}", Stats());
    for (auto& [p, s] : g_shapes)
        REX::INFO("tint: shape {:p} {} '{}' flags 0x{:X} emit ({:.2f},{:.2f},{:.2f}) x{:.2f}, authored ({:.2f},{:.2f},{:.2f}) x{:.2f}", (void*)p, s.target, s.name,
                  p->flags.underlying(), p->emitColor ? p->emitColor->r : -1.0f, p->emitColor ? p->emitColor->g : -1.0f, p->emitColor ? p->emitColor->b : -1.0f, p->emitColorScale,
                  s.baseColor.r, s.baseColor.g, s.baseColor.b, s.baseScale);
    ForEachWorn([](const std::string& t, RE::NiAVObject* root, bool fp) { REX::INFO("tint: worn {} {} root '{}'", t, fp ? "1P" : "3P", root->name.c_str() ? root->name.c_str() : ""); });
    auto* pl = RE::PlayerCharacter::GetSingleton();
    std::function<void(RE::NiAVObject*)> eyes = [&](RE::NiAVObject* o) {     // can the player's eyes glow? (Kiroshi)
        if (!o) return;
        if (auto* n = o->IsNode()) { for (const auto& c : n->children) eyes(c.get()); return; }
        auto* g = o->IsGeometry();
        if (!g || !g->name.c_str()) return;
        std::string nm = g->name.c_str();
        std::transform(nm.begin(), nm.end(), nm.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (nm.find("eye") == std::string::npos) return;
        for (auto& raw : g->properties)
            if (auto* p = netimmerse_cast<Prop*>(raw.get()))
                REX::INFO("tint: eye shape '{}' flags 0x{:X} emitColor {:p} inImage {} x{:.2f}", g->name.c_str(), p->flags.underlying(), (void*)p->emitColor, p->emitColor ? InGameImage(p->emitColor) : false, p->emitColorScale);
    };
    if (pl) eyes(pl->Get3D(false));
    std::vector<std::pair<Prop*, std::string>> mine;
    if (pl) CollectEyes(pl->Get3D(false), mine);
    int actors = 0, same = 0;
    const bool shared = EyeUnsafe(mine, &actors, &same, true);
    REX::INFO("tint: eye sharing: {} player eye shape(s), {} actor(s) checked, {} same-named NPC eye(s), shared {}, allowed {}, disabled {}", mine.size(), actors, same, shared, g_eyesAllowed, g_eyesShared);
}

namespace {
// Screen position of a world point, 0..1 from the top left of the view; false when it is behind the camera. Same
// measurement as cw_smart_game ToScreen: which way the engine's y grows is measured with a point 10 units higher.
// perUnit = screen heights per world unit at that depth.
bool ToScreen(const RE::NiPoint3& v, float& sx, float& sy, float& perUnit)
{
    auto* wc = RE::Main::WorldRootCamera();
    if (!wc) return false;
    float x = 0, y = 0, d = 0, x2 = 0, y2 = 0, d2 = 0;
    if (!wc->WorldPtToScreenPt3(RE::NiPoint3(v.x, v.y, v.z), x, y, d, 1e-5f) || d <= 0) return false;
    if (!wc->WorldPtToScreenPt3(RE::NiPoint3(v.x, v.y, v.z + 10.f), x2, y2, d2, 1e-5f)) return false;
    const auto& pr = wc->port;
    const float pw = (float)(pr.right - pr.left), ph = (float)(pr.bottom - pr.top);
    sx = pw != 0 ? (x - (float)pr.left) / pw : x;
    sy = ph != 0 ? (y - (float)pr.top) / ph : y;
    const float above = ph != 0 ? (y2 - (float)pr.top) / ph : y2;
    perUnit = std::fabs(above - sy) / 10.0f;
    if (above > sy) sy = 1.0f - sy;   // the raised point came out lower: this y grows upward
    return true;
}

bool CulledUp(RE::NiAVObject* o)
{
    for (; o; o = o->parent)
        if (o->GetAppCulled()) return true;
    return false;
}
}  // namespace

// Test seam (cw-customize-5): is the tinted part where the camera can see it? P15 shot T1 with the camera inside the
// player's head (cam3p zoom 0.12: the player is not drawn) and T4 3P against a wall, and both read as "tint FAIL".
// Checks, for the current view (1P or 3P camera): the target has tinted shapes held under that view's biped, they are not
// culled, the anchor bones project on screen, and in 3P the camera is not so close that the player is not drawn.
// Logs one TINT_WHERE_OK line with the pixel box to pass to shot-gate.ps1 -Region, or TINT_WHERE_FAIL with the reason.
// It cannot see walls between the camera and the part: the shot gate's two-colour check still decides that.
std::string Where(const std::string& only, int w, int h)
{
    auto* pl = RE::PlayerCharacter::GetSingleton();
    auto* pc = RE::PlayerCamera::GetSingleton();
    auto* wc = RE::Main::WorldRootCamera();
    if (!pl || !pc || !wc) { REX::INFO("TINT_WHERE_FAIL {}: no player or camera", only); return "fail"; }
    const bool fpView = pc->QCameraEquals(RE::CameraStates::kFirstPerson);
    std::map<std::string, std::pair<int, int>> held;   // target -> (shapes under this view, not culled)
    for (auto& [p, s] : g_shapes) {
        if (!only.empty() && s.target != only) continue;
        if (s.eye ? fpView : s.fp != fpView) continue;  // eyes are on the 3P head only
        auto& n = held[s.target];
        ++n.first;
        if (s.eye || !CulledUp(s.geom.get())) ++n.second;
    }
    if (!only.empty() && !held.count(only)) held[only] = { 0, 0 };
    std::string verdict = "ok";
    RE::NiAVObject* root = pl->Get3D(fpView);
    const auto& cp = wc->world.translate;
    for (auto& [target, n] : held) {
        const char* view = fpView ? "1P" : "3P";
        if (n.first == 0) { REX::INFO("TINT_WHERE_FAIL {} {}: no tinted shape of it held under the {} biped (not worn, not drawn, or no colour path)", target, view, view); verdict = "fail"; continue; }
        if (n.second == 0) { REX::INFO("TINT_WHERE_FAIL {} {}: all {} shape(s) are culled", target, view, n.first); verdict = "fail"; continue; }
        // anchor bones and how far around them the part reaches (world units)
        std::vector<const char*> bones;
        float pad = 40.0f;
        if (target == "kiroshi") { bones = { "Head" }; pad = 14.0f; }
        else { bones = { "LArm_Hand", "RArm_Hand", "LArm_ForeArm2", "RArm_ForeArm2" }; if (target == "monowire") pad = 60.0f; }
        int found = 0, on = 0;
        float minD = 1e9f, x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const char* b : bones) {
            RE::NiAVObject* o = root ? root->GetObjectByName(RE::BSFixedString(b)) : nullptr;
            if (!o) continue;
            ++found;
            const auto& bp = o->world.translate;
            const float dist = std::sqrt((bp.x - cp.x) * (bp.x - cp.x) + (bp.y - cp.y) * (bp.y - cp.y) + (bp.z - cp.z) * (bp.z - cp.z));
            minD = std::min(minD, dist);
            float sx = 0, sy = 0, pu = 0;
            if (!ToScreen(bp, sx, sy, pu)) continue;
            if (sx < 0.0f || sx > 1.0f || sy < 0.0f || sy > 1.0f) continue;
            ++on;
            const float r = pad * pu * (float)h;
            x0 = std::min(x0, sx * (float)w - r); x1 = std::max(x1, sx * (float)w + r);
            y0 = std::min(y0, sy * (float)h - r); y1 = std::max(y1, sy * (float)h + r);
        }
        if (found == 0) { REX::INFO("TINT_WHERE_FAIL {} {}: no anchor bone found in the {} skeleton", target, view, view); verdict = "fail"; continue; }
        if (on == 0) { REX::INFO("TINT_WHERE_FAIL {} {}: off screen ({} anchor(s), nearest {:.0f} u from the camera)", target, view, found, minD); verdict = "fail"; continue; }
        // 3P only: a camera this close is inside or against the player, who is then faded out or clipped by the near plane
        // (P15 T1, cam3p zoom 0.12). 40 u is a first guess from that shot; the line prints the distance to calibrate it.
        if (!fpView && minD < 40.0f) { REX::INFO("TINT_WHERE_FAIL {} {}: camera {:.0f} u from the part (under 40 u the player is not drawn)", target, view, minD); verdict = "fail"; continue; }
        const int bx = std::max(0, (int)x0), by = std::max(0, (int)y0);
        const int bw = std::min(w, (int)x1) - bx, bh = std::min(h, (int)y1) - by;
        REX::INFO("TINT_WHERE_OK {} {} box {},{},{},{} (of {}x{}) anchors on screen {}/{} nearest {:.0f} u shapes drawn {}/{}", target, view, bx, by, bw, bh, w, h, on, found, minD, n.second, n.first);
    }
    return verdict;
}

// Test seam (ARMS, 2026-11-10; P15b T2: "tint: configured, 0 shape(s) held" with the PLS installed, and the shot
// gate saw 0 changed pixels; no build has ever logged a launcher shape). {"cmd":"tint","worn":"launcher"} walks EVERY occupied
// biped slot of both bipeds (1P, 3P) and logs the form in it, whether it is a registered visual, its clone, and for every
// geometry under a registered one the shader property's flags, emittance and the verdict of the same SkipReason the scan uses.
// Ends with one line per target: TINT_WORN_OK <target> <n> shape(s) tintable (1P a, 3P b), or TINT_WORN_FAIL <target>: the
// first reason, in the order the scan meets them (no slot holds it / no clone / no geometry / every shader skipped: why).
std::string Worn(const std::string& only)
{
    auto* pl = RE::PlayerCharacter::GetSingleton();
    if (!pl) { REX::INFO("TINT_WORN_FAIL {}: no player", only); return "fail"; }
    struct Tally { int slots = 0, clones = 0, geoms = 0, ok1 = 0, ok3 = 0; std::string firstSkip; };
    std::map<std::string, Tally> t;
    if (!only.empty()) t[only];
    else for (auto& [id, tg] : g_visual) t[tg];
    const RE::BipedAnim* fpb = pl->firstPersonBipedAnim.get();
    const RE::BipedAnim* tpb = pl->biped.get();
    for (const RE::BipedAnim* b : { fpb, tpb }) {
        const bool fp = b == fpb;
        if (!b) { REX::INFO("tint: worn {}: no biped", fp ? "1P" : "3P"); continue; }
        for (int i = 0; i < std::to_underlying(RE::BIPED_OBJECT::kTotal); ++i) {
            const auto& o = b->object[i];
            if (!o.parent.object) continue;
            const std::uint32_t id = o.parent.object->GetFormID();
            const auto vit = g_visual.find(id);
            const std::string target = vit != g_visual.end() ? vit->second : std::string();
            const char* ed = o.parent.object->GetFormEditorID();
            REX::INFO("tint: worn {} slot {} form {:08X} '{}' visual '{}' clone {:p}", fp ? "1P" : "3P", i + 30, id, ed ? ed : "", target, (void*)o.partClone.get());
            if (target.empty() || !t.count(target)) continue;
            auto& ty = t[target];
            ++ty.slots;
            if (!o.partClone) continue;
            ++ty.clones;
            std::function<void(RE::NiAVObject*)> walk = [&](RE::NiAVObject* n) {
                if (!n) return;
                if (auto* nd = n->IsNode()) { for (const auto& c : nd->children) walk(c.get()); return; }
                auto* g = n->IsGeometry();
                if (!g) return;
                ++ty.geoms;
                const std::string name = g->name.c_str() ? g->name.c_str() : "";
                int props = 0;
                for (auto& raw : g->properties) {
                    auto* p = netimmerse_cast<Prop*>(raw.get());
                    if (!p) continue;
                    ++props;
                    const char* why = SkipReason(p, target + (fp ? "|1P|" : "|3P|") + name);
                    const bool img = p->emitColor && InGameImage(p->emitColor);
                    REX::INFO("tint: worn {} {} '{}' flags 0x{:X} emit {:p}{} ({:.2f},{:.2f},{:.2f}) x{:.2f} culled {} -> {}", fp ? "1P" : "3P", target, name,
                              p->flags.underlying(), (void*)p->emitColor, img ? " (game image)" : "",
                              p->emitColor && !img ? p->emitColor->r : -1.0f, p->emitColor && !img ? p->emitColor->g : -1.0f, p->emitColor && !img ? p->emitColor->b : -1.0f,
                              p->emitColorScale, g->GetAppCulled(), why ? why : "TINTABLE");
                    if (why) { if (ty.firstSkip.empty()) ty.firstSkip = std::string(name) + ": " + why; }
                    else ++(fp ? ty.ok1 : ty.ok3);
                }
                if (!props) { REX::INFO("tint: worn {} {} '{}' has no BSLightingShaderProperty", fp ? "1P" : "3P", target, name); if (ty.firstSkip.empty()) ty.firstSkip = name + ": no lighting shader property"; }
            };
            walk(o.partClone.get());
        }
    }
    std::string verdict = "ok";
    for (auto& [target, ty] : t) {
        int inv = 0;
        for (auto& [id, tg] : g_visual)
            if (tg == target) if (auto* f = RE::TESForm::GetFormByID(id)) if (auto* bo = f->As<RE::TESBoundObject>()) inv += pl->GetInventoryObjectCount(bo);
        if (ty.ok1 + ty.ok3 > 0) { REX::INFO("TINT_WORN_OK {} {} shape(s) tintable (1P {}, 3P {}), held now {}", target, ty.ok1 + ty.ok3, ty.ok1, ty.ok3, Stats()); continue; }
        verdict = "fail";
        if (!ty.slots) REX::INFO("TINT_WORN_FAIL {}: no biped slot holds a registered {} visual (in inventory: {})", target, target, inv);
        else if (!ty.clones) REX::INFO("TINT_WORN_FAIL {}: {} slot(s) hold it but none has a 3D clone", target, ty.slots);
        else if (!ty.geoms) REX::INFO("TINT_WORN_FAIL {}: its clone(s) hold no geometry", target);
        else REX::INFO("TINT_WORN_FAIL {}: {} geometry, none tintable; first: {}", target, ty.geoms, ty.firstSkip);
    }
    return verdict;
}

}  // namespace tint
