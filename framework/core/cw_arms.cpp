// cw_arms.cpp - arm cyberware logic (Mantis leap target + lunge path, drawn-visual toggle). No game calls.
#include "cw_core.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

namespace cw {

namespace { constexpr double kPi = 3.14159265358979323846; }

int PickLeapTarget(const Vec3& from, double headingRad, const std::vector<LeapCandidate>& c, const LeapParams& p)
{
    const double fx = std::sin(headingRad), fy = std::cos(headingRad);
    int best = -1;
    double bestD = 0;
    for (std::size_t i = 0; i < c.size(); ++i) {
        const auto& t = c[i];
        if (!t.alive || !t.hostile || !t.los) continue;
        const double dx = t.pos.x - from.x, dy = t.pos.y - from.y, dz = t.pos.z - from.z;
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz), h = std::sqrt(dx * dx + dy * dy);
        if (d < p.minUnits || d > p.maxUnits || h < 1e-6) continue;
        const double cosA = std::clamp((dx * fx + dy * fy) / h, -1.0, 1.0);
        if (std::acos(cosA) * 180.0 / kPi > p.coneDeg) continue;
        if (best < 0 || d < bestD) { best = (int)i; bestD = d; }
    }
    return best;
}

Vec3 LungePos(const Vec3& start, const Vec3& target, double t, const LeapParams& p)
{
    t = std::clamp(t, 0.0, 1.0);
    const double dx = target.x - start.x, dy = target.y - start.y;
    const double h = std::sqrt(dx * dx + dy * dy);
    const double travel = std::max(0.0, h - p.stopShort);
    const double ux = h > 1e-6 ? dx / h : 0, uy = h > 1e-6 ? dy / h : 0;
    const double arc = std::min(p.arcMax, 0.1 * travel);
    Vec3 r;
    r.x = start.x + ux * travel * t;
    r.y = start.y + uy * travel * t;
    r.z = start.z + (target.z - start.z) * t + arc * 4.0 * t * (1.0 - t);
    return r;
}

double LungeSeconds(const LeapParams& p) { return std::max(0.05, p.impactS - p.launchS); }

LeapShape LeapProfileShape(const Vec3& start, const Vec3& target, const LeapParams& p)
{
    LeapShape s;
    const double dx = target.x - start.x, dy = target.y - start.y, dz = target.z - start.z;
    s.travel = std::max(0.0, std::sqrt(dx * dx + dy * dy) - p.stopShort);
    s.apex = std::clamp(p.profApexBase + p.profApexPerUnit * s.travel, p.profApexMin, p.profApexMax);
    if (dz > 0) s.apex = std::max(s.apex, dz + p.profApexMin * 0.5);
    s.hangS = std::clamp(p.hangMinS + (p.hangMaxS - p.hangMinS) * s.travel / 800.0, p.hangMinS, p.hangMaxS);
    s.totalS = p.loadS + p.riseS + s.hangS + p.dropS;
    return s;
}

Vec3 LeapProfilePos(const Vec3& start, const Vec3& target, double t, const LeapParams& p)
{
    const LeapShape s = LeapProfileShape(start, target, p);
    const double dx = target.x - start.x, dy = target.y - start.y, dz = target.z - start.z;
    const double h = std::sqrt(dx * dx + dy * dy);
    const double ux = h > 1e-6 ? dx / h : 0, uy = h > 1e-6 ? dy / h : 0;
    const double fly = p.riseS + s.hangS + p.dropS;
    const double tf = std::clamp(t - p.loadS, 0.0, fly);
    const double f = fly > 0 ? tf / fly : 1.0;                      // horizontal: steady over the flight, none during the load-up
    double z;
    if (t < p.loadS) z = 0;                                          // load-up: the clip's crouch, feet down
    else if (tf < p.riseS) z = s.apex * std::sin(1.5707963267948966 * tf / p.riseS);   // fast rise, easing into the top
    else if (tf < p.riseS + s.hangS) z = s.apex * (1.0 - 0.1 * (tf - p.riseS) / s.hangS);   // hang: a slow 10% sag
    else {                                                           // drop: accelerating onto the landing height
        const double u = std::clamp((tf - p.riseS - s.hangS) / p.dropS, 0.0, 1.0);
        z = 0.9 * s.apex * (1.0 - u * u) + dz * u;
    }
    return { start.x + ux * s.travel * f, start.y + uy * s.travel * f, start.z + z };
}

LaunchPlan PlanLaunch(const Vec3& start, const Vec3& target, const LeapParams& p)
{
    LaunchPlan r;
    const double dx = target.x - start.x, dy = target.y - start.y, dz = target.z - start.z;
    const double h = std::sqrt(dx * dx + dy * dy);
    r.travel = std::max(0.0, h - p.stopShort);
    r.apex = std::clamp(p.apexBase + p.apexPerUnit * r.travel, p.apexMin, p.apexMax);
    if (dz > 0) r.apex = std::max(r.apex, dz + p.apexMin * 0.5);   // landing higher up: the apex must clear it
    const double g = std::max(1.0, p.gravity);
    const double vz = std::sqrt(2.0 * g * r.apex);
    const double down = std::max(0.0, r.apex - dz);                // apex -> landing height
    r.flightS = vz / g + std::sqrt(2.0 * down / g);
    const double vxy = r.flightS > 1e-6 ? r.travel / r.flightS : 0.0;
    const double ux = h > 1e-6 ? dx / h : 0, uy = h > 1e-6 ? dy / h : 0;
    r.v = { ux * vxy, uy * vxy, vz };
    return r;
}

namespace {
// One spelling per round, so the ini may name either the catalog suffix or the element. Anything else canonicalises to
// itself and therefore matches no round: a typo ships FEWER rounds, never more.
std::string CanonRound(std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (s == "electrical") return "elec";
    if (s == "thermal") return "therm";
    if (s == "chemical") return "chem";
    if (s == "tranquilizing" || s == "tranquilizer") return "tranq";
    return s;
}
}   // namespace

bool PlsRoundListed(const std::string& list, const std::string& round)
{
    if (list.empty()) return false;
    const std::string want = CanonRound(round);
    for (std::size_t i = 0; i <= list.size();) {
        std::size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string tok = list.substr(i, j - i);
        const std::size_t a = tok.find_first_not_of(" \t");
        const std::size_t b = tok.find_last_not_of(" \t");
        if (a != std::string::npos && CanonRound(tok.substr(a, b - a + 1)) == want) return true;
        i = j + 1;
    }
    return false;
}

bool PlsRoundShips(const std::string& list, const std::string& round)
{
    if (!PlsRoundListed(list, round)) return false;
    // ARMORY 2026-11-10: incendiary wears the THERMAL materials, so the two swap the same files and are one gradable unit.
    // A list naming one without the other would half-ship a shared look, so neither ships unless both are named.
    const std::string want = CanonRound(round);
    if (want == "therm" || want == "incendiary")
        return PlsRoundListed(list, "therm") && PlsRoundListed(list, "incendiary");
    return true;
}

std::string IniValue(const std::string& text, const std::string& section, const std::string& key, const std::string& def, bool* found)
{
    auto lower = [](std::string s) { for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; };
    auto trim = [](const std::string& s) {
        const std::size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        const std::size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };
    const std::string wantSec = lower(section), wantKey = lower(key);
    bool inSec = false, seenSec = false;
    std::size_t i = 0;
    if (found) *found = false;
    while (i <= text.size()) {
        std::size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(i, e - i));
        i = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            const std::string sec = lower(trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            if (inSec) break;                 // the first matching section is the one that counts
            inSec = (sec == wantSec) && !seenSec;
            if (inSec) seenSec = true;
            continue;
        }
        if (!inSec) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || lower(trim(line.substr(0, eq))) != wantKey) continue;
        std::string v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
        if (found) *found = true;
        return v;
    }
    return def;
}

int PlsFilterCatalog(json& j, const std::string& list, std::vector<std::string>* log)
{
    static const char* kRounds[] = { "explosive", "elec", "therm", "chem", "incendiary", "tranq" };
    std::set<std::string> dropAb, dropMod;
    for (const char* r : kRounds)
        if (!PlsRoundShips(list, r)) { dropAb.insert(std::string("ab_mod_pls_round_") + r); dropMod.insert(std::string("cwm_pls_round_") + r); }
    if (dropAb.empty() || !j.is_object()) return 0;
    int n = 0;
    auto say = [&](const std::string& s) { ++n; if (log) log->push_back(s); };
    auto field = [](const json& e, const char* k) -> std::string {
        return e.is_object() && e.contains(k) && e[k].is_string() ? e[k].get<std::string>() : std::string();
    };
    // keep the elements `why` returns "" for; log the others with the reason
    auto keepIf = [&](json& arr, const std::string& where, const std::function<std::string(const json&)>& why) {
        if (!arr.is_array()) return;
        json keep = json::array();
        for (auto& e : arr) {
            const std::string w = why(e);
            if (w.empty()) keep.push_back(e); else say(where + ": " + w);
        }
        arr = std::move(keep);
    };
    auto refsIn = [&](json& owner, const char* key, const std::set<std::string>& drop, const std::string& where) {
        if (!owner.is_object() || !owner.contains(key)) return;
        keepIf(owner[key], where + " " + key, [&](const json& e) {
            const std::string id = e.is_string() ? e.get<std::string>() : std::string();
            return drop.count(id) ? "removed the reference to dropped '" + id + "'" : std::string();
        });
    };
    if (j.contains("abilities"))
        keepIf(j["abilities"], "abilities", [&](const json& e) { const std::string id = field(e, "id"); return dropAb.count(id) ? "dropped ability '" + id + "' (round not shipped)" : std::string(); });
    if (j.contains("mods"))
        keepIf(j["mods"], "mods", [&](const json& e) { const std::string id = field(e, "id"); return dropMod.count(id) ? "dropped mod '" + id + "' (round not shipped)" : std::string(); });
    for (const char* coll : { "items", "mods" }) {
        if (!j.contains(coll) || !j[coll].is_array()) continue;
        for (auto& x : j[coll]) {
            const std::string w = std::string(coll == std::string("items") ? "item '" : "mod '") + field(x, "id") + "'";
            refsIn(x, "iconicAbilities", dropAb, w);
            if (x.is_object() && x.contains("tiers") && x["tiers"].is_array())
                for (auto& t : x["tiers"]) refsIn(t, "abilities", dropAb, w + " tier " + (t.is_object() && t.contains("tier") ? t["tier"].dump() : std::string("?")));
        }
    }
    if (j.contains("ripperdocs") && j["ripperdocs"].is_array())
        for (auto& r : j["ripperdocs"]) {
            if (!r.is_object() || !r.contains("modStock")) continue;
            keepIf(r["modStock"], "ripperdoc '" + field(r, "id") + "' modStock", [&](const json& e) {
                const std::string id = field(e, "mod");
                return dropMod.count(id) ? "removed the stock entry for dropped mod '" + id + "'" : std::string();
            });
        }
    if (j.contains("cyberPerks") && j["cyberPerks"].is_array())
        for (auto& pk : j["cyberPerks"]) {
            if (!pk.is_object() || !pk.contains("effects")) continue;
            keepIf(pk["effects"], "cyberPerk '" + field(pk, "id") + "' effects", [&](const json& e) {
                const std::string id = field(e, "ability");
                return (field(e, "type") == "ability" && dropAb.count(id)) ? "removed the ability effect for dropped '" + id + "'" : std::string();
            });
        }
    return n;
}

PlsPool PlsPoolStep(double pool, int count, int lastCount, int maxCharges, double dtS, double rechargeS, double regenPct)
{
    PlsPool o;
    const int mx = std::max(1, maxCharges);
    if (count < lastCount) pool = std::min(pool, (double)count / mx);   // a throw spent a charge's share
    const double rate = (1.0 + regenPct / 100.0) / std::max(0.1, rechargeS);   // whole pool per second
    o.pool = std::clamp(pool + std::max(0.0, dtS) * rate, 0.0, 1.0);
    const int want = (int)std::floor(o.pool * mx + 1e-6);
    o.add = std::max(0, want - count);
    return o;
}

double RuntimeMods::Set(const std::string& key, double want)
{
    double have = applied.count(key) ? applied[key] : 0.0;
    if (want == 0) applied.erase(key); else applied[key] = want;
    return want - have;
}
std::map<std::string, double> RuntimeMods::Strip() { stripped = applied; applied.clear(); return stripped; }
std::map<std::string, double> RuntimeMods::Restore() { auto r = stripped; for (auto& [k, v] : r) applied[k] = v; stripped.clear(); return r; }

int LimbGroup(int l)
{
    switch (l) {
    case 0x0: case 0x14: return 0;                              // torso, pelvis
    case 0x1: case 0x2: case 0x5: case 0x10: return 1;          // head, eye, head2, brain
    case 0x6: case 0x7: return 2;
    case 0x8: case 0x9: return 3;
    case 0xA: case 0xB: case 0xC: case 0x17: return 4;          // left leg, left foot
    case 0xD: case 0xE: case 0xF: return 5;
    default: return -1;
    }
}

WeakPoints::Hit WeakPoints::OnHit(uint32_t target, int limbEnum, double dealt, double targetMaxHp, int64_t nowMs, double critMult, bool ml, uint64_t rnd)
{
    Hit h;
    if (stacks > 0 && nowMs >= stacksUntil) stacks = 0;
    const double freq = 1.0 + (ml ? 0.10 * stacks * (stacks >= 5 ? 2 : 1) : 0.0);
    T& t = targets[target];
    if (t.group < 0 || nowMs >= t.nextAt) { t.group = (int)(rnd % 6); t.hit = 0; t.nextAt = nowMs + (int64_t)(intervalS / freq * 1000.0); }
    h.group = t.group;
    if (LimbGroup(limbEnum) != t.group || dealt <= 0) return h;
    h.onWeakPoint = true;
    const double critBonus = ml ? 0.05 * stacks * (stacks >= 5 ? 2 : 1) : 0.0;
    h.bonus = dealt * ((critMult + critBonus) * 1.25 - 1.0);
    t.hit += dealt + h.bonus;
    if (t.hit >= popFrac * std::max(1.0, targetMaxHp)) {
        h.destroyed = true;
        t.group = -1;   // a new one appears at the next hit
        if (ml) { stacks = std::min(5, stacks + 1); stacksUntil = nowMs + 25000; }
    }
    return h;
}

bool FinisherEligible(const FinisherHit& h, double hpMax)
{
    return h.alive && h.human && h.power && h.hpPct > 0 && h.hpPct <= hpMax && (h.unaware || h.staggered || h.sneak);
}

Placed FinisherPlace(double px, double py, double headingRad, double lx, double ly, double yawDeg)
{
    const double fx = std::sin(headingRad), fy = std::cos(headingRad);   // forward
    const double rx = std::cos(headingRad), ry = -std::sin(headingRad);  // right
    Placed o;
    o.x = px + lx * rx + ly * fx;
    o.y = py + lx * ry + ly * fy;
    o.heading = std::fmod(headingRad + yawDeg * kPi / 180.0, 2 * kPi);
    if (o.heading < 0) o.heading += 2 * kPi;
    return o;
}

// ---- the engine's measured damage model (cw_core.h: FINISHERS 2026-11-10, boot p15b-1 of d50d6620)
double EngineArmourFactor(double totalDamage)
{
    if (totalDamage <= 0) return 0.0;
    return 0.563 * std::pow(totalDamage / 42.1, 0.366);
}

double EngineLimbFactor(int limbEnum)
{
    return limbEnum == 1 ? 5.0 : 1.0;   // limb 1 = the head; 5.12 and 5.01 measured, one constant fits both
}

double PredictApplied(double totalDamage, int limbEnum)
{
    return totalDamage * EngineArmourFactor(totalDamage) * EngineLimbFactor(limbEnum);
}

bool HoldSurvives(double hp, double buffer, double engineApplied)
{
    return hp + buffer - engineApplied >= 1.0;
}

FinisherHoldPlan PlanFinisherHold(double victimHp, double hitTotalDamage, double bufferAmount)
{
    FinisherHoldPlan p;
    p.buffer = victimHp > 0 && bufferAmount > 0;
    p.bufferAmount = p.buffer ? bufferAmount : 0.0;
    p.writeDamageField = false;   // never: the engine does not read the field we can write, and writing it skews the rest
    p.netDamage = hitTotalDamage;   // the net applies the ORIGINAL blow, never a held figure
    return p;
}

// ---- sliding the victim in instead of teleporting it (FIN-1)
static double TurnToward(double from, double to, double t)
{
    double d = std::fmod(to - from, 2 * kPi);
    if (d > kPi) d -= 2 * kPi;
    if (d < -kPi) d += 2 * kPi;
    double h = std::fmod(from + d * t, 2 * kPi);
    if (h < 0) h += 2 * kPi;
    return h;
}

Slide FinisherSlideStep(double fromX, double fromY, double fromHeading, const Placed& to, int step, int steps)
{
    Slide s;
    if (steps <= 0) { s.x = to.x; s.y = to.y; s.heading = to.heading; return s; }
    const double raw = (double)(std::min)((std::max)(step, 0), steps) / (double)steps;
    const double t = 1.0 - (1.0 - raw) * (1.0 - raw);   // ease out: the pull decelerates into the grab
    s.x = fromX + (to.x - fromX) * t;
    s.y = fromY + (to.y - fromY) * t;
    s.heading = TurnToward(fromHeading, to.heading, t);
    return s;
}

int FinisherSlideSteps(double fromX, double fromY, double fromHeading, const Placed& to, double maxStepU, double maxTurnDeg, int cap)
{
    const double dx = to.x - fromX, dy = to.y - fromY;
    const double dist = std::sqrt(dx * dx + dy * dy);
    double turn = std::fabs(std::remainder(to.heading - fromHeading, 2 * kPi)) * 180.0 / kPi;
    int need = 1;
    if (maxStepU > 0) need = (std::max)(need, (int)std::ceil(dist / maxStepU));
    if (maxTurnDeg > 0) need = (std::max)(need, (int)std::ceil(turn / maxTurnDeg));
    // ease-out puts the largest move in the FIRST step (about 2/steps of the distance), so double the count to stay under the bar
    need *= 2;
    if (cap > 0) need = (std::min)(need, cap);
    return (std::max)(need, 1);
}

// ---- pinning the victim for the set piece (FIN-2 / FIN-5)
Placed FinisherChoreographed(const Placed& placed, double headingRad, double rmX, double rmY, double elapsedS, double victimS)
{
    Placed w = placed;
    if (victimS <= 0 || (rmX == 0 && rmY == 0)) return w;
    const double t = (std::min)((std::max)(elapsedS, 0.0) / victimS, 1.0);
    const double fx = std::sin(headingRad), fy = std::cos(headingRad);   // the player's forward
    const double rx = std::cos(headingRad), ry = -std::sin(headingRad);  // the player's right
    w.x += (rmX * rx + rmY * fx) * t;
    w.y += (rmX * ry + rmY * fy) * t;
    return w;
}

double FinisherDrift(double vx, double vy, const Placed& want)
{
    const double dx = vx - want.x, dy = vy - want.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool FinisherPin(double vx, double vy, const Placed& want, double maxDriftU, double& px, double& py)
{
    if (maxDriftU <= 0 || FinisherDrift(vx, vy, want) <= maxDriftU) return false;
    px = want.x;
    py = want.y;
    return true;
}

FinisherExitResult FinisherOnExit(const FinisherState& s, FinisherExit e)
{
    FinisherExitResult r;
    // A lift can only still be on an actor if the design left one applied across a frame. The shipped hold cannot, so this
    // is false for every exit; a standing buffer makes it true for every exit, which is the whole argument for the change.
    r.liftOutstanding = s.liftApplied;
    // The victim always sits on the health it had when the finisher was decided, never on 1, and never immortal: the floor
    // is restored with the engine's own heal and no modifier of ours is left behind.
    r.victimStuckAtOne = s.active && s.floorHp > 0 && s.floorHp <= 1.0;
    r.victimImmortal = s.liftApplied;
    switch (e) {
        case FinisherExit::Completed:
            break;   // killAt owned the kill; the net stands down
        case FinisherExit::NeverStarted:
        case FinisherExit::Interrupted:
            r.netOwesKill = s.netArmed;   // the net applies the blow the player actually landed
            r.netDamage = s.netArmed ? s.netDamage : 0.0;
            break;
        case FinisherExit::VictimGone:
        case FinisherExit::GameClosed:
            break;   // nothing left to kill, and nothing of ours on the actor to undo
        case FinisherExit::SavedAndLoaded:
            r.liftOutstanding = false;   // the loaded actor state comes from the save, and the save holds no lift
            r.victimImmortal = false;
            break;
        case FinisherExit::VictimEssential:
            break;   // it could never have been killed; it keeps its own health and the net cannot help
        case FinisherExit::SecondFinisher:
            break;   // the first finisher owns the victim; the second is refused, so nothing is applied twice
    }
    return r;
}

double ZoomTarget(double heldS, double maxZoom)
{
    return (std::min)(maxZoom, heldS < 0.5 ? 1.5 : heldS < 1.2 ? 2.2 : heldS < 2.0 ? 4.0 : heldS < 2.8 ? 7.0 : 10.0);
}

double ZoomEase(double current, double target, double dtS)
{
    return current + (target - current) * (1.0 - std::exp(-11.0 * dtS));
}

double ZoomFovDeg(double baseDeg, double zoom)
{
    return 2.0 * std::atan(std::tan(baseDeg * kPi / 360.0) / (std::max)(zoom, 1.0)) * 180.0 / kPi;
}

double AimError(double px, double py, double headingRad, double tx, double ty, double hullU)
{
    const double dx = tx - px, dy = ty - py;
    const double ang = std::fabs(std::remainder(std::atan2(dx, dy) - headingRad, 2 * kPi));
    return (std::max)(0.0, ang - std::atan2(hullU, std::sqrt(dx * dx + dy * dy)));
}

bool BetterAim(double err, double dist, double bestErr, double bestDist)
{
    return err < bestErr || (err == bestErr && dist < bestDist);
}

int ArmsVisualState::OnAnimEvent(const std::string& tag, bool visualWeapon)
{
    if (tag == "weaponDraw" && visualWeapon && !on) { on = true; mismatchSince = 0; return +1; }
    if (tag == "weaponDraw" && !visualWeapon && on) { on = false; mismatchSince = 0; return -1; }   // swapped to a gun or another weapon: no ghost blades for 1.5 s
    if (tag == "weaponSheathe" && on) { on = false; mismatchSince = 0; return -1; }
    return 0;
}

int ArmsVisualState::Reconcile(bool weaponDrawn, bool visualWeapon, std::uint64_t nowMs, std::uint64_t graceMs)
{
    const bool want = weaponDrawn && visualWeapon;
    if (want == on) { mismatchSince = 0; return 0; }
    if (graceMs > 0) {
        if (mismatchSince == 0) { mismatchSince = nowMs ? nowMs : 1; return 0; }
        if (nowMs < mismatchSince + graceMs) return 0;
    }
    mismatchSince = 0;
    on = want;
    return want ? +1 : -1;
}

// ---- arm weapon damage by tier (BETA QA 2, 2026-11-10; decl in cw_core.h) ----
int ArmWeaponDamage(const NativeAbility& a)
{
    if (a.handler.rfind("ar_", 0) != 0 || !a.params.is_object()) return 0;
    auto it = a.params.find("base_dmg");
    if (it == a.params.end() || !it->is_number() || it->is_boolean()) return 0;
    return std::max(1, (int)std::lround(it->get<double>()));
}

double ArmTierFactor(int tier)
{
    const int t = std::clamp(tier, 1, 7);
    return t <= 5 ? 0.4 + 0.15 * (t - 1) : 1.0 + 0.1 * (t - 5);
}

double ArmLadderAt(const std::vector<double>& byTier, int tier, double fallback)
{
    if (byTier.empty()) return fallback;
    const std::size_t i = (std::size_t)std::max(tier, 1) - 1;
    return i < byTier.size() ? byTier[i] : byTier.back();
}

std::string ArmHandlerOfWeapon(const std::string& e)
{
    return e == "CW_MantisBlades" ? "ar_mantis" : e == "CW_GorillaArms" ? "ar_gorilla" : e == "CW_Monowire" ? "ar_monowire" : "";
}

std::vector<std::string> ArmRecordProblems(const Catalog& c, const json* weaponsArms)
{
    std::vector<std::string> out;
    auto num = [](const json& p, const char* k) -> std::optional<double> {
        if (!p.is_object() || !p.contains(k) || !p[k].is_number() || p[k].is_boolean()) return std::nullopt;
        return p[k].get<double>();
    };
    auto fmt = [](double v) { std::string s = std::to_string(v); s.erase(s.find_last_not_of('0') + 1); if (!s.empty() && s.back() == '.') s.pop_back(); return s; };
    std::map<std::string, json> t5;   // ar_* handler -> its T5 params
    const auto perkKeys = PerkCarriedKeys(c);
    for (auto& [aid, ab] : c.abilities) {
        if (!ab.raw.is_object() || !ab.raw.contains("spec") || !ab.raw["spec"].is_object() || !ab.raw["spec"].contains("native")) continue;
        const json& nat = ab.raw["spec"]["native"];
        const std::string h = nat.value("handler", "");
        if (h.rfind("ar_", 0) != 0) continue;
        if (!num(NativeParamsFor(nat, 5), "base_dmg")) continue;   // not a weapon
        t5[h] = NativeParamsFor(nat, 5);
        std::set<int> tiers;                                        // the tiers a shop item really grants it at
        for (auto& [iid, it] : c.items)
            for (auto& t : it.tiers)
                if (std::find(t.abilities.begin(), t.abilities.end(), aid) != t.abilities.end()) tiers.insert(t.tier);
        std::optional<double> prevDmg;
        std::map<std::string, std::optional<double>> flat0;
        std::map<std::string, std::vector<std::pair<int, double>>> series;   // every numeric param, by tier
        std::map<std::string, std::vector<std::pair<int, double>>> effect;   // every perk entry it becomes, by tier
        const std::string act = nat.value("activation", "equip");
        int prevTier = 0;
        for (int t : tiers) {
            const json p = NativeParamsFor(nat, t);
            const auto dmg = num(p, "base_dmg");
            if (!dmg) { out.push_back("arm " + h + ": no base_dmg at tier " + std::to_string(t)); continue; }
            if (prevDmg && *dmg <= *prevDmg)
                out.push_back("arm " + h + ": base_dmg does not rise from tier " + std::to_string(prevTier) + " (" + fmt(*prevDmg) + ") to tier " + std::to_string(t) +
                              " (" + fmt(*dmg) + "): an upgrade must buy hit damage");
            prevDmg = dmg; prevTier = t;
            for (auto& [k, v] : p.items())
                if (v.is_number() && !v.is_boolean()) series[k].push_back({ t, v.get<double>() });
            {   // what the tier's params become in game: one line per perk entry, its strength = the multiplier or the chance
                const NativeEffects fx = EffectsFor(h, FamilyOf(h, act, p), p);
                std::map<std::string, int> seen;
                for (auto& e : fx.entries) {
                    double strength = e.value;
                    for (auto& w : e.when) if (w.fn == "chance") strength = w.value;
                    std::string key = e.entry + (e.spell.empty() ? "" : ":" + e.spell);
                    for (auto& w : e.when) if (w.fn != "chance") key += "|" + w.fn;
                    key += "#" + std::to_string(seen[key]++);
                    effect[key].push_back({ t, strength });
                    if (e.entry != "hit_spell" && std::fabs(e.value - 1.0) < 1e-9)
                        out.push_back("arm " + h + ": the " + e.entry + " entry at tier " + std::to_string(t) + " multiplies by 1.0: it does nothing");
                }
            }
            for (const char* k : { "speed", "reach" }) {   // record values: the same at every tier
                const auto v = num(p, k);
                if (!v) continue;
                auto& first = flat0[k];
                if (!first) first = v;
                else if (std::fabs(*v - *first) > 1e-9)
                    out.push_back("arm " + h + ": " + k + " " + fmt(*v) + " at tier " + std::to_string(t) + " differs from " + fmt(*first) + ": nothing scales " +
                                  (std::string(k) == "speed" ? "swing speed" : "reach") + " by tier");
            }
        }
        // A field that moves with the tier must move at EVERY tier, in one direction, and so must what it becomes in game.
        // (tier/5 capped at T5 left T5+ and T5++ with nothing new; max(1, charged_mult / 2) left T1-T4 with nothing at all.)
        auto ladder = [&](const std::string& what, const std::vector<std::pair<int, double>>& v, std::size_t nTiers) {
            if (v.size() != nTiers) { out.push_back("arm " + h + ": " + what + " is present at " + std::to_string(v.size()) + " of " + std::to_string(nTiers) + " tiers"); return; }
            bool flat = true;
            for (auto& x : v) if (std::fabs(x.second - v[0].second) > 1e-9) flat = false;
            if (flat) return;
            const double dir = v.back().second - v.front().second;
            for (std::size_t i = 1; i < v.size(); ++i) {
                const double step = v[i].second - v[i - 1].second;
                if (std::fabs(step) < 1e-9 || step * dir < 0)
                    out.push_back("arm " + h + ": " + what + " is tier-scaled but goes " + fmt(v[i - 1].second) + " -> " + fmt(v[i].second) + " from tier " +
                                  std::to_string(v[i - 1].first) + " to tier " + std::to_string(v[i].first) + ": that upgrade buys nothing");
            }
        };
        // Above the perk tier clamp the engine runs the clamp tier's perk (TimedPerkId; abilities defect D9), so a
        // perk-carried param and every perk entry stop there in game: the ladder is checked up to the clamp only, and
        // TierLadderProblems requires the catalog to carry the clamp tier's value above it (shown = applied).
        const int clamp = PerkTierClamp();
        const auto pkIt = perkKeys.find(aid);
        auto carried = [&](const std::string& k) { return pkIt != perkKeys.end() && std::find(pkIt->second.begin(), pkIt->second.end(), k) != pkIt->second.end(); };
        auto upToClamp = [&](const std::vector<std::pair<int, double>>& v) { std::vector<std::pair<int, double>> r; for (auto& x : v) if (x.first <= clamp) r.push_back(x); return r; };
        std::size_t nUpToClamp = 0;
        for (int t : tiers) if (t <= clamp) ++nUpToClamp;
        for (auto& [k, v] : series)
            if (k != "base_dmg" && k != "speed" && k != "reach") { if (carried(k)) ladder(k, upToClamp(v), nUpToClamp); else ladder(k, v, tiers.size()); }
        for (auto& [k, v] : effect) ladder("the effect " + k.substr(0, k.find('#')), upToClamp(v), nUpToClamp);
    }
    if (!weaponsArms) return out;
    std::set<std::string> seen;
    if (weaponsArms->is_object() && weaponsArms->contains("weapons") && (*weaponsArms)["weapons"].is_array())
        for (auto& w : (*weaponsArms)["weapons"]) {
            if (!w.is_object()) continue;
            const std::string edid = w.value("edid", "");
            std::string h = w.value("handler", "");
            if (h.empty()) h = ArmHandlerOfWeapon(edid);
            if (h.empty() || !w.contains("stats_proposal") || !w["stats_proposal"].is_object()) continue;   // the PLS: espgen builds it from the catalog
            seen.insert(h);
            auto c5 = t5.find(h);
            if (c5 == t5.end()) { out.push_back("arm record " + edid + ": the catalog has no weapon ability with handler " + h); continue; }
            const json& st = w["stats_proposal"];
            const auto rd = num(st, "damage"), cd = num(c5->second, "base_dmg");
            if (!rd) out.push_back("arm record " + edid + ": stats_proposal has no damage");
            else if (cd && std::fabs(*rd - *cd) > 1e-9)
                out.push_back("arm record " + edid + ": damage " + fmt(*rd) + " in arms/data/weapons_arms.json but the catalog's T5 base_dmg is " + fmt(*cd));
            for (const char* k : { "speed", "reach" }) {
                const auto rv = num(st, k), cv = num(c5->second, k);
                if (!rv) out.push_back("arm record " + edid + ": stats_proposal has no " + k);
                else if (!cv) out.push_back("arm record " + edid + ": the catalog's " + h + " has no " + k);
                else if (std::fabs(*rv - *cv) > 1e-9)
                    out.push_back("arm record " + edid + ": " + k + " " + fmt(*rv) + " in arms/data/weapons_arms.json but the catalog says " + fmt(*cv));
            }
        }
    for (auto& [h, p] : t5)
        if (h != "ar_pls" && !seen.count(h)) out.push_back("arm " + h + ": no WEAP record with a stats_proposal in arms/data/weapons_arms.json");
    // The Gorilla knockback table: one entry per tier the Gorilla is sold at, rising (ARMS review R2: it had 5 entries, so a
    // T5+ or T5++ Gorilla fell back to the stagger value and lost its knockback).
    if (t5.count("ar_gorilla") && weaponsArms->is_object()) {
        int maxTier = 0;
        for (auto& [aid, ab] : c.abilities)
            if (ab.raw.is_object() && ab.raw.contains("spec") && ab.raw["spec"].is_object() && ab.raw["spec"].contains("native") && ab.raw["spec"]["native"].value("handler", "") == "ar_gorilla")
                for (auto& [iid, it] : c.items)
                    for (auto& t : it.tiers)
                        if (std::find(t.abilities.begin(), t.abilities.end(), aid) != t.abilities.end()) maxTier = std::max(maxTier, t.tier);
        const json combat = weaponsArms->value("combat", json::object());
        const json by = combat.is_object() ? combat.value("gorillaKnockback", json::object()).value("ragdollByTier", json::array()) : json::array();
        if (!by.is_array() || (int)by.size() < maxTier)
            out.push_back("arm ar_gorilla: combat.gorillaKnockback.ragdollByTier has " + std::to_string(by.is_array() ? by.size() : 0) + " entries but the Gorilla is sold up to tier " +
                          std::to_string(maxTier) + ": a higher tier would lose its knockback");
        if (by.is_array())
            for (std::size_t i = 1; i < by.size(); ++i)
                if (!by[i].is_number() || !by[i - 1].is_number() || by[i].get<double>() <= by[i - 1].get<double>())
                    out.push_back("arm ar_gorilla: ragdollByTier does not rise from tier " + std::to_string(i) + " to tier " + std::to_string(i + 1) + ": that upgrade buys no knockback");
    }
    return out;
}

// ---- the catalog-wide tier ladder (decl in cw_core.h; the rule is data: catalog top-level tierLadder) ----
std::vector<std::string> TierLadderProblems(const Catalog& c)
{
    std::vector<std::string> out;
    if (!c.raw.is_object() || !c.raw.contains("tierLadder") || !c.raw["tierLadder"].is_object()) return out;
    const json& L = c.raw["tierLadder"];
    auto strs = [&](const char* k) { std::vector<std::string> v; if (L.contains(k) && L[k].is_array()) for (auto& x : L[k]) if (x.is_string()) v.push_back(x.get<std::string>()); return v; };
    const auto flat = strs("flat"), flatContains = strs("flatContains"), flatSuffix = strs("flatSuffix"), negGood = strs("negativeIsBetter");
    const std::string cdSuffix = L.value("cooldownSuffix", "cooldown_s"), timeScale = L.value("timeScale", "time_scale");
    {
        const json eff = L.value("effect", json::array());
        bool ok = eff.is_array() && eff.size() == 7;
        if (ok) for (std::size_t i = 0; i < 7; ++i) ok = ok && eff[i].is_number() && (i == 0 || eff[i].get<double>() > eff[i - 1].get<double>());
        if (!ok || std::fabs(eff[4].get<double>() - 1.0) > 1e-9) out.push_back("tierLadder.effect must be seven strictly rising numbers with tier 5 = 1.0");
    }
    auto ends = [](const std::string& k, const std::string& suf) { return !suf.empty() && k.size() >= suf.size() && k.compare(k.size() - suf.size(), suf.size(), suf) == 0; };
    auto base = [](const std::string& k) { const auto b = k.find('['); return b == std::string::npos ? k : k.substr(0, b); };
    // higher = better for the player
    auto goodness = [&](const std::string& key, double v) {
        const std::string k = base(key);
        if (ends(k, cdSuffix) || k == timeScale) return -v;
        if (std::find(flat.begin(), flat.end(), k) != flat.end()) return 0.0;
        for (auto& s : flatContains) if (k.find(s) != std::string::npos) return 0.0;
        for (auto& s : flatSuffix) if (ends(k, s)) return 0.0;
        for (auto& s : negGood) if (k.find(s) != std::string::npos) return -v;
        return v;
    };
    auto fmt = [](double v) { std::string s = std::to_string(v); s.erase(s.find_last_not_of('0') + 1); if (!s.empty() && s.back() == '.') s.pop_back(); return s; };
    auto nativeOf = [&](const std::string& aid) -> const json* {
        auto it = c.abilities.find(aid);
        if (it == c.abilities.end() || !it->second.raw.is_object() || !it->second.raw.contains("spec") || !it->second.raw["spec"].is_object() ||
            !it->second.raw["spec"].contains("native")) return nullptr;
        return &it->second.raw["spec"]["native"];
    };
    // the numbers a tier applies: stats + the numeric native params of its abilities (lists as key[i])
    auto numbers = [&](const TierDef& t) {
        std::map<std::string, double> n;
        for (auto& [k, v] : t.stats) n["stat:" + k] = v;
        for (auto& aid : t.abilities)
            if (const json* nat = nativeOf(aid)) {
                const json p = NativeParamsFor(*nat, t.tier);
                for (auto& [k, v] : p.items()) {
                    if (v.is_number() && !v.is_boolean()) n.emplace(k, v.get<double>());
                    else if (v.is_array()) { int i = 0; for (auto& e : v) { if (e.is_number() && !e.is_boolean()) n.emplace(k + "[" + std::to_string(i) + "]", e.get<double>()); ++i; } }
                }
            }
        return n;
    };
    std::map<std::string, std::set<int>> sold;
    auto walk = [&](const std::vector<TierDef>& tiers) { for (auto& t : tiers) for (auto& a : t.abilities) sold[a].insert(t.tier); };
    for (auto& [id, it] : c.items) walk(it.tiers);
    for (auto& [id, m] : c.mods) walk(m.tiers);
    for (auto& [aid, tiers] : sold) {
        const json* nat = nativeOf(aid);
        if (!nat || !nat->contains("paramsT5") || !(*nat)["paramsT5"].is_object()) continue;
        if (nat->value("scaleWithTier", false)) out.push_back("ability " + aid + " still has scaleWithTier: every sold tier must carry explicit params");
        bool numeric = false;
        for (auto& [k, v] : (*nat)["paramsT5"].items()) numeric = numeric || (v.is_number() && !v.is_boolean()) || v.is_array();
        if (!numeric) continue;
        for (int t : tiers)
            if (!nat->contains("paramsT" + std::to_string(t)))
                out.push_back("ability " + aid + " is sold at tier " + std::to_string(t) + " but has no paramsT" + std::to_string(t) + " (the engine would use the unscaled tier 5 values)");
    }
    // Shown = applied (the lead, 2026-11-10): above the perk tier clamp the engine runs the clamp tier's perk, so a
    // perk-carried param must carry the clamp tier's value there, and the catalog's copy of the perk keys must be current.
    if (!L.contains("perkTiers") || !L["perkTiers"].is_number_integer() || !L.contains("perkKeys") || !L["perkKeys"].is_object())
        out.push_back("tierLadder has no perkTiers / perkKeys: run cwtest <catalog> --perk-keys data/perk-keys.json, then rebuild the catalog");
    else {
        const int clamp = L["perkTiers"].get<int>();
        if (clamp != PerkTierClamp())
            out.push_back("tierLadder.perkTiers is " + std::to_string(clamp) + " but perks exist up to tier " + std::to_string(PerkTierClamp()) +
                          ": regenerate data/perk-keys.json (cwtest --perk-keys) and rebuild the catalog");
        const auto live = PerkCarriedKeys(c);
        std::map<std::string, std::vector<std::string>> have;
        for (auto& [aid, ks] : L["perkKeys"].items()) for (auto& k : ks) if (k.is_string()) have[aid].push_back(k.get<std::string>());
        for (auto& [aid, ks] : have) std::sort(ks.begin(), ks.end());
        auto liveSorted = live;
        for (auto& [aid, ks] : liveSorted) std::sort(ks.begin(), ks.end());
        if (have != liveSorted)
            out.push_back("tierLadder.perkKeys differs from what the perk baker carries: regenerate data/perk-keys.json (cwtest --perk-keys) and rebuild the catalog");
        std::set<std::string> computed;   // abilities whose tier value above the clamp the DLL applies (ARMORY's compute path)
        for (auto& x : L.value("computed", json::array())) if (x.is_string()) computed.insert(x.get<std::string>());
        for (auto& [aid, ks] : liveSorted) {
            // Per ability: an ability whose perk is BAKED above the global clamp (the four Kiroshi scans, whose range
            // is a field on the detect spell) carries its own, higher clamp. Without this the check reports "perks
            // exist up to tier 5" for a tier whose perk is a record in this very plugin.
            const int abClamp = BakedTierCap(aid);
            const json* nat = nativeOf(aid);
            if (!nat || !nat->contains("paramsT" + std::to_string(abClamp)) || computed.count(aid)) continue;
            const json pc = NativeParamsFor(*nat, abClamp);
            for (int t = abClamp + 1; t <= 7; ++t) {
                if (!nat->contains("paramsT" + std::to_string(t)) || !sold[aid].count(t)) continue;
                const json pt = NativeParamsFor(*nat, t);
                for (auto& k : ks)
                    if (pt.contains(k) && pc.contains(k) && pt[k] != pc[k])
                        out.push_back("ability " + aid + ": " + k + " is carried by a perk and perks exist up to tier " + std::to_string(abClamp) + ", but tier " + std::to_string(t) +
                                      " says " + pt[k].dump() + " (the build applies " + pc[k].dump() + "): the shop would show a number the build does not apply");
            }
        }
    }
    for (auto& [id, it] : c.items) {
        for (std::size_t i = 1; i < it.tiers.size(); ++i) {
            const TierDef &a = it.tiers[i - 1], &b = it.tiers[i];
            const auto na = numbers(a), nb = numbers(b);
            bool same = a.abilities == b.abilities && a.modSlots == b.modSlots && a.capacityGrant == b.capacityGrant && na.size() == nb.size();
            if (same) for (auto& [k, v] : na) { auto f = nb.find(k); if (f == nb.end() || std::fabs(f->second - v) > 1e-9) { same = false; break; } }
            if (same) out.push_back("item " + id + ": tier " + std::to_string(b.tier) + " is identical to tier " + std::to_string(a.tier) + ": an upgrade must buy something");
            for (auto& [k, v] : na) {
                if (k.rfind("stat:", 0) == 0) continue;
                auto f = nb.find(k);
                if (f != nb.end() && goodness(k, f->second) < goodness(k, v) - 1e-9)
                    out.push_back("item " + id + ": " + k + " goes " + fmt(v) + " -> " + fmt(f->second) + " from tier " + std::to_string(a.tier) + " to tier " + std::to_string(b.tier) +
                                  ": that upgrade makes it worse");
            }
        }
    }
    return out;
}

// ---- which params a baked perk carries (decl in cw_core.h) ----
namespace {
std::string EffectSig(const NativeEffects& e)
{
    json j = json::array();
    j.push_back(e.fx.kind); j.push_back(e.fx.magnitude); j.push_back(e.fx.areaFeet);
    for (auto& c : e.fxWhen) j.push_back({ c.fn, c.value });
    for (auto& a : e.avs) { json w = json::array(); for (auto& c : a.when) w.push_back({ c.fn, c.value }); j.push_back({ a.av, a.value, w }); }
    for (auto& x : e.entries) { json w = json::array(); for (auto& c : x.when) w.push_back({ c.fn, c.value }); j.push_back({ x.entry, x.value, x.spell, w, x.distMin, x.distMax }); }
    return j.dump();
}
}  // namespace

// ---- COMPUTE: the tier value the DLL applies above the perk clamp (decls in cw_core.h) ----

// ARMORY's list. The pilot the lead named is the only entry: nothing else goes in until its result passes.
// cwtest --perk-keys emits it so data/perk-keys.json cannot drift from this table.
const std::set<std::string>& ComputedAbilities()
{
    // Every entry here is one --validate proves computable: its tier difference above the clamp is expressible as an
    // actor-value delta and nothing in a record changes. The abilities deliberately NOT here, with the reason the code
    // gave (not the reason a doc gave):
    //   - the four Kiroshi scans (clairvoyant, doomsayer, oracle, sentry): SpellSpecFor bakes the scan RANGE as the
    //     detect spell's areaFeet, so a longer range at tier 6 means a different RECORD. The SPLIT doc called these
    //     "pure DLL"; the code disagrees, and the code wins. Only Oracle's trap_m is actually plugin-read.
    //   - ab_face_kiroshi_basic, ab_os_berserk_militech, ab_os_berserk_zetatech, ab_sk_pa_interface, and since
    //     2026-11-10 ab_le_lynx_paws (the true-up's sneak_dmg_pct; the lead ruled option 1): a perk ENTRY's
    //     value changes with the tier. Their actor-value halves ARE computable (--validate prints the deltas), but
    //     "computed" is per ABILITY in BETA QA's contract, so listing them would unclamp the entry half too and the
    //     shop would claim an improvement the build does not make. Per-key granularity is proposed, not assumed.
    static const std::set<std::string> k = {
        "ab_ci_rad_scrubber", "ab_ci_threatevac", "ab_le_jenkins_tendons", "ab_ns_adreno_trigger",
        "ab_ns_tyrosine", "ab_os_sandy_dynalar", "ab_os_sandy_militech_falcon", "ab_sk_kinetic_frame", "ab_sk_scar_coalescer", "ab_sk_scarab",
    };
    return k;
}

namespace {
std::string WhenSig(const std::vector<NatCond>& w)
{
    json j = json::array();
    for (auto& c : w) j.push_back({ c.fn, c.value });
    return j.dump();
}
// The conditions a per-frame runtime check can actually evaluate (Env::PlayerIs / ActorValuePct). A chance roll, a
// power attack or a distance band is evaluated by the ENGINE inside a perk entry and has no runtime equivalent here.
bool RuntimeEvaluable(const std::string& fn)
{
    static const std::set<std::string> ok = { "sneaking", "combat", "sprinting", "aiming", "airborne", "hp_below", "hp_above", "ap_above" };
    return ok.count(fn) > 0;
}
const json* NativeOf(const Catalog& c, const std::string& ability)
{
    auto it = c.abilities.find(ability);
    if (it == c.abilities.end() || !it->second.raw.is_object()) return nullptr;
    const json& raw = it->second.raw;
    if (!raw.contains("spec") || !raw["spec"].is_object() || !raw["spec"].contains("native")) return nullptr;
    const json& nat = raw["spec"]["native"];
    return nat.is_object() && nat.contains("handler") ? &nat : nullptr;
}
NativeEffects EffectsAt(const json& nat, int tier)
{
    const json p = NativeParamsFor(nat, tier);
    const std::string h = nat.value("handler", ""), act = nat.value("activation", "");
    return EffectsFor(h, FamilyOf(h, act, p), p);
}
}  // namespace

std::vector<NatAvFx> TierDeltaAvs(const Catalog& c, const std::string& ability, int tier)
{
    std::vector<NatAvFx> out;
    const int clamp = PerkTierClamp();
    if (tier <= clamp || !ComputedAbilities().count(ability)) return out;
    const json* nat = NativeOf(c, ability);
    // Only an EXPLICIT paramsT<tier> may drive this. Without one the catalog is not stating a value for this tier and
    // there is nothing to apply: scaling the clamp tier here would be the engine inventing a number, which the ruling
    // forbids (one source, the catalog).
    if (!nat || !nat->contains("paramsT" + std::to_string(tier)) || !nat->contains("paramsT" + std::to_string(clamp))) return out;
    const NativeEffects at = EffectsAt(*nat, tier), base = EffectsAt(*nat, clamp);
    std::map<std::string, double> baseAv;   // (av, conditions) -> what the clamp tier's perk already applies
    for (auto& a : base.avs) baseAv[a.av + "|" + WhenSig(a.when)] += a.value;
    std::map<std::string, double> want;
    std::map<std::string, NatAvFx> shape;
    for (auto& a : at.avs) { const std::string k = a.av + "|" + WhenSig(a.when); want[k] += a.value; shape[k] = a; }
    for (auto& [k, v] : want) {
        const double d = v - (baseAv.count(k) ? baseAv[k] : 0.0);
        if (std::fabs(d) < 1e-9) continue;
        NatAvFx f = shape[k];
        f.value = d;
        out.push_back(f);
    }
    for (auto& [k, v] : baseAv)   // an effect the clamp tier has and this tier does not: the perk still applies it
        if (!want.count(k) && std::fabs(v) > 1e-9) {
            NatAvFx f;
            f.av = k.substr(0, k.find('|'));
            f.value = -v;
            for (auto& a : base.avs) if (a.av + "|" + WhenSig(a.when) == k) { f.when = a.when; break; }
            out.push_back(f);
        }
    return out;
}

std::vector<std::string> ComputedAbilityProblems(const Catalog& c)
{
    std::vector<std::string> out;
    const int clamp = PerkTierClamp();
    for (auto& ability : ComputedAbilities()) {
        const json* nat = NativeOf(c, ability);
        if (!nat) { out.push_back("computed ability " + ability + " is not an ability with a native spec in this catalog"); continue; }
        const auto keys = PerkCarriedKeys(c);
        auto pk = keys.find(ability);
        if (pk == keys.end() || pk->second.empty()) {
            out.push_back("computed ability " + ability + " carries no perk param, so nothing is clamped and it does not need a compute path");
            continue;
        }
        int applied = 0, stated = 0;   // stated counts tiers the catalog states at all, so the "no tier" message cannot
                                       // fire merely because every stated tier had a different problem (it did, once)
        for (int t = clamp + 1; t <= 7; ++t) {
            if (!nat->contains("paramsT" + std::to_string(t))) continue;
            ++stated;
            const NativeEffects at = EffectsAt(*nat, t), base = EffectsAt(*nat, clamp);
            // Condition (1): nothing above the clamp may need a FORM rewritten. A perk entry, a spell or an area is
            // baked into the record, so a difference there cannot be applied at runtime to an actor.
            if (at.entries.size() != base.entries.size()) {
                out.push_back("computed ability " + ability + " tier " + std::to_string(t) + " changes the number of perk ENTRIES (" +
                              std::to_string(base.entries.size()) + " -> " + std::to_string(at.entries.size()) + "): a perk entry is baked into the record, so this tier cannot be computed");
                continue;
            }
            bool entryDiff = false;
            for (std::size_t i = 0; i < at.entries.size(); ++i)
                if (std::fabs(at.entries[i].value - base.entries[i].value) > 1e-9 || WhenSig(at.entries[i].when) != WhenSig(base.entries[i].when)) entryDiff = true;
            if (entryDiff) {
                out.push_back("computed ability " + ability + " tier " + std::to_string(t) + " changes a perk ENTRY's value or condition: baked in the record, so this tier cannot be computed");
                continue;
            }
            if (at.fx.kind != base.fx.kind || std::fabs(at.fx.magnitude - base.fx.magnitude) > 1e-9 || at.fx.areaFeet != base.fx.areaFeet) {
                out.push_back("computed ability " + ability + " tier " + std::to_string(t) + " changes the baked spell (" + base.fx.kind + " " + std::to_string(base.fx.magnitude) +
                              " -> " + at.fx.kind + " " + std::to_string(at.fx.magnitude) + "): that is a form, so this tier cannot be computed");
                continue;
            }
            const auto d = TierDeltaAvs(c, ability, t);
            // A tier whose computed value equals the clamp tier's is NOT a problem by itself: the tier can be sold on
            // its other numbers (Dynalar's tier 6 buys duration and cooldown while its crit chance holds). What would
            // be wrong is an ability on the list with NOTHING to compute at ANY tier - checked after the loop.
            if (d.empty()) continue;
            for (auto& f : d)
                for (auto& w : f.when)
                    if (!RuntimeEvaluable(w.fn))
                        out.push_back("computed ability " + ability + " tier " + std::to_string(t) + ": the delta on " + f.av + " is conditioned on '" + w.fn +
                                      "', which only the engine can evaluate inside a perk entry: this ability is not computable");
            ++applied;
        }
        if (!stated) out.push_back("computed ability " + ability + " has no explicit paramsT above tier " + std::to_string(clamp) + ": there is no tier for the DLL to apply");
        else if (!applied) out.push_back("computed ability " + ability + " is listed as computed but NO tier above " + std::to_string(clamp) +
                                         " has anything to apply: it does not belong on the list");
    }
    return out;
}

int PerkTierClamp()
{
    const std::string id = TimedPerkId("x", 99);
    return std::atoi(id.substr(id.rfind("_T") + 2).c_str());
}

std::map<std::string, std::vector<std::string>> PerkCarriedKeys(const Catalog& c)
{
    std::map<std::string, std::vector<std::string>> out;
    for (auto& [aid, ab] : c.abilities) {
        if (!ab.raw.is_object() || !ab.raw.contains("spec") || !ab.raw["spec"].is_object() || !ab.raw["spec"].contains("native")) continue;
        const json& nat = ab.raw["spec"]["native"];
        const std::string h = nat.value("handler", ""), act = nat.value("activation", "");
        const json p5 = NativeParamsFor(nat, 5);
        if (!p5.is_object()) continue;
        const std::string base = EffectSig(EffectsFor(h, FamilyOf(h, act, p5), p5));
        for (auto& [k, v] : p5.items()) {
            json p = p5;
            if (v.is_number() && !v.is_boolean()) p[k] = v.get<double>() * 1.37 + 0.71;
            else if (v.is_array()) { bool any = false; for (auto& e : p[k]) if (e.is_number() && !e.is_boolean()) { e = e.get<double>() * 1.37 + 0.71; any = true; } if (!any) continue; }
            else continue;
            if (EffectSig(EffectsFor(h, FamilyOf(h, act, p), p)) != base) out[aid].push_back(k);
        }
    }
    return out;
}

// ---- PLS rounds from the catalog (decl in cw_core.h) ----
std::vector<PlsRoundDef> PlsRounds(const Catalog& c, std::vector<std::string>* problems)
{
    std::vector<PlsRoundDef> out;
    for (const auto& mid : c.modOrder) {
        if (mid.rfind("cwm_pls_round_", 0) != 0) continue;
        auto m = c.mods.find(mid);
        if (m == c.mods.end()) continue;
        std::string handler, round;
        for (const auto& t : m->second.tiers)
            for (const auto& ab : t.abilities) {
                auto a = c.abilities.find(ab);
                if (a == c.abilities.end() || !a->second.raw.is_object()) continue;
                const json nat = a->second.raw.value("spec", json::object()).value("native", json::object());
                if (!nat.is_object()) continue;
                const json p5 = nat.value("paramsT5", json::object());
                if (nat.value("handler", "").rfind("mod_pls_round_", 0) == 0 && p5.is_object() && p5.value("round", "") != "") {
                    handler = nat.value("handler", ""); round = p5.value("round", "");
                }
            }
        if (handler.empty() || round.empty()) {
            if (problems) problems->push_back("pls: " + mid + " has no ability with a mod_pls_round_* handler and a paramsT5.round");
            continue;
        }
        out.push_back({ mid, handler, round, round == "incendiary" ? "thermal" : round });
    }
    return out;
}

}  // namespace cw
