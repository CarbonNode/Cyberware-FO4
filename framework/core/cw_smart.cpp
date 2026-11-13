// cw_smart.cpp - see cw_smart.h.
#include "cw_smart.h"

#include <algorithm>
#include <cmath>

namespace cw::smart {

namespace {
constexpr double kPi = 3.14159265358979323846;
double Rad(double deg) { return deg * kPi / 180.0; }
Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 Mul(const Vec3& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
Vec3 Norm(const Vec3& a) { double l = Len(a); return l > 1e-12 ? Mul(a, 1.0 / l) : Vec3{ 0, 1, 0 }; }
uint64_t Mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
double U01(uint64_t s) { return (double)(Mix(s) >> 11) / 9007199254740992.0; }
}  // namespace

double Len(const Vec3& v) { return std::sqrt(Dot(v, v)); }
Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
double AngleBetween(const Vec3& a, const Vec3& b)
{
    double c = Dot(Norm(a), Norm(b));
    return std::acos(std::clamp(c, -1.0, 1.0));
}

Params Params::FromJson(const json& j) { return FromJson(j, Params{}); }

Params Params::FromJson(const json& j, Params p)
{
    if (!j.is_object()) return p;
    auto num = [&](const char* k, double& v) { if (j.contains(k) && j[k].is_number()) v = j[k].get<double>(); };
    auto in = [&](const char* k, int& v) { if (j.contains(k) && j[k].is_number()) v = (int)std::lround(j[k].get<double>()); };
    auto flag = [&](const char* k, bool& v) {
        if (!j.contains(k)) return;
        if (j[k].is_boolean()) v = j[k].get<bool>();
        else if (j[k].is_number()) v = j[k].get<double>() != 0;
    };
    num("HipLockingAngleYaw", p.HipLockingAngleYaw); num("HipLockingAnglePitch", p.HipLockingAnglePitch);
    num("AdsLockingAngleYaw", p.AdsLockingAngleYaw); num("AdsLockingAnglePitch", p.AdsLockingAnglePitch);
    num("HipTargetableAngleYaw", p.HipTargetableAngleYaw); num("HipTargetableAnglePitch", p.HipTargetableAnglePitch);
    num("AdsTargetableAngleYaw", p.AdsTargetableAngleYaw); num("AdsTargetableAnglePitch", p.AdsTargetableAnglePitch);
    in("HipMaxLockedTargets", p.HipMaxLockedTargets); in("AdsMaxLockedTargets", p.AdsMaxLockedTargets);
    flag("TrackMultipleEntitiesInADS", p.TrackMultipleEntitiesInADS);
    num("HipTimeToLock", p.HipTimeToLock); num("AdsTimeToLock", p.AdsTimeToLock);
    num("HipTimeToUnlock", p.HipTimeToUnlock); num("AdsTimeToUnlock", p.AdsTimeToUnlock);
    num("TimeToRemoveOccludedTarget", p.TimeToRemoveOccludedTarget);
    num("TargetAcquisitionRange", p.TargetAcquisitionRange);
    num("PlayerProjectileVelocity", p.PlayerProjectileVelocity);
    num("HitProbability", p.HitProbability);
    flag("AddSpiralTrajectory", p.AddSpiralTrajectory);
    num("SpiralRadius", p.SpiralRadius);
    num("SpiralCycleTimeMin", p.SpiralCycleTimeMin); num("SpiralCycleTimeMax", p.SpiralCycleTimeMax);
    num("SpiralRampDistanceStart", p.SpiralRampDistanceStart); num("SpiralRampDistanceEnd", p.SpiralRampDistanceEnd);
    num("EvenDistributionPeriod", p.EvenDistributionPeriod);
    flag("UseEvenDistributionTargeting", p.UseEvenDistributionTargeting);
    num("TurnRateDeg", p.TurnRateDeg);
    return p;
}

// The only place CP units become game units.
Params::Game Params::ToGame() const
{
    Game g{};
    g.hipLockYaw = Rad(HipLockingAngleYaw); g.hipLockPitch = Rad(HipLockingAnglePitch);
    g.adsLockYaw = Rad(AdsLockingAngleYaw); g.adsLockPitch = Rad(AdsLockingAnglePitch);
    // A targetable cone narrower than the locking cone would drop a lock the moment it formed: never narrower.
    g.hipTgtYaw = Rad(std::max(HipTargetableAngleYaw, HipLockingAngleYaw)); g.hipTgtPitch = Rad(std::max(HipTargetableAnglePitch, HipLockingAnglePitch));
    g.adsTgtYaw = Rad(std::max(AdsTargetableAngleYaw, AdsLockingAngleYaw)); g.adsTgtPitch = Rad(std::max(AdsTargetableAnglePitch, AdsLockingAnglePitch));
    g.hipMax = std::max(0, HipMaxLockedTargets);
    g.adsMax = std::max(0, TrackMultipleEntitiesInADS ? AdsMaxLockedTargets : std::min(1, AdsMaxLockedTargets));
    g.hipLockS = std::max(0.0, HipTimeToLock); g.adsLockS = std::max(0.0, AdsTimeToLock);
    g.hipUnlockS = std::max(0.0, HipTimeToUnlock); g.adsUnlockS = std::max(0.0, AdsTimeToUnlock);
    g.occludedS = std::max(0.0, TimeToRemoveOccludedTarget);
    g.rangeU = TargetAcquisitionRange * kUnitsPerMetre;
    g.speedU = PlayerProjectileVelocity * kUnitsPerMetre;
    g.hitP = std::clamp(HitProbability, 0.0, 1.0);
    g.spiral = AddSpiralTrajectory && SpiralRadius > 0;
    g.spiralRU = SpiralRadius * kUnitsPerMetre;
    g.cycleMinS = std::max(0.02, SpiralCycleTimeMin); g.cycleMaxS = std::max(g.cycleMinS, SpiralCycleTimeMax);
    g.rampStartU = SpiralRampDistanceStart * kUnitsPerMetre;
    g.rampEndU = std::max(g.rampStartU, SpiralRampDistanceEnd * kUnitsPerMetre);
    g.turnRad = Rad(std::max(1.0, TurnRateDeg));
    return g;
}

void AngleOff(const Vec3& eye, const Vec3& dir, const Vec3& to, double& yaw, double& pitch)
{
    Vec3 d = Norm(dir), v = Sub(to, eye);
    const double aimYaw = std::atan2(d.x, d.y), aimPitch = std::atan2(d.z, std::hypot(d.x, d.y));
    const double tYaw = std::atan2(v.x, v.y), tPitch = std::atan2(v.z, std::hypot(v.x, v.y));
    yaw = std::remainder(tYaw - aimYaw, 2 * kPi);
    pitch = tPitch - aimPitch;
}

void Tracker::Update(const Vec3& eye, const Vec3& aimDir, bool ads, const std::vector<Candidate>& cands, const Params& p, double dt,
                     std::vector<LockEvent>* ev)
{
    const auto g = p.ToGame();
    const double lockYaw = ads ? g.adsLockYaw : g.hipLockYaw, lockPitch = ads ? g.adsLockPitch : g.hipLockPitch;
    const double tgtYaw = ads ? g.adsTgtYaw : g.hipTgtYaw, tgtPitch = ads ? g.adsTgtPitch : g.hipTgtPitch;
    const double lockS = ads ? g.adsLockS : g.hipLockS, unlockS = ads ? g.adsUnlockS : g.hipUnlockS;
    const int maxN = ads ? g.adsMax : g.hipMax;
    auto drop = [&](size_t i, const char* why) {
        if (ev && locks_[i].locked) ev->push_back({ LockEvent::Lost, locks_[i].id, why });
        locks_.erase(locks_.begin() + (long)i);
    };
    auto offAxis = [&](const Vec3& aim) { double y, pt; AngleOff(eye, aimDir, aim, y, pt); return std::hypot(y, pt); };

    // 1. existing locks
    for (size_t i = 0; i < locks_.size();) {
        auto& L = locks_[i];
        auto it = std::find_if(cands.begin(), cands.end(), [&](const Candidate& c) { return c.id == L.id; });
        if (it == cands.end() || !it->alive || !it->hostile) { drop(i, it == cands.end() ? "gone" : (!it->alive ? "dead" : "not hostile")); continue; }
        L.aim = it->aim;
        if (Len(Sub(it->aim, eye)) > g.rangeU) { drop(i, "out of range"); continue; }
        if (!it->los) { L.occludedS += dt; if (L.occludedS > g.occludedS) { drop(i, "occluded"); continue; } }
        else L.occludedS = 0;
        double yaw, pitch; AngleOff(eye, aimDir, it->aim, yaw, pitch);
        const bool inLock = std::fabs(yaw) <= lockYaw && std::fabs(pitch) <= lockPitch;
        const bool inTgt = std::fabs(yaw) <= tgtYaw && std::fabs(pitch) <= tgtPitch;
        if (!inTgt) { L.outS += dt; if (L.outS > unlockS) { drop(i, "left the cone"); continue; } }
        else L.outS = 0;
        if (!L.locked) {
            if (inLock && it->los) L.progress = lockS <= 0 ? 1.0 : std::min(1.0, L.progress + dt / lockS);
            else if (!inTgt) L.progress = 0;   // a half-built lock that left the cone starts over
            if (L.progress >= 1.0) { L.locked = true; if (ev) ev->push_back({ LockEvent::Acquired, L.id, "" }); }
        }
        ++i;
    }
    // 2. too many (hip -> ADS): keep the ones nearest the crosshair
    if ((int)locks_.size() > maxN) {
        std::stable_sort(locks_.begin(), locks_.end(), [&](const Lock& a, const Lock& b) { return offAxis(a.aim) < offAxis(b.aim); });
        while ((int)locks_.size() > maxN) drop(locks_.size() - 1, "lock slots full");
    }
    // 3. new candidates inside the locking cone, nearest the crosshair first
    std::vector<const Candidate*> fresh;
    for (auto& c : cands) {
        if (!c.alive || !c.hostile || !c.los) continue;
        if (Find(c.id)) continue;
        if (Len(Sub(c.aim, eye)) > g.rangeU) continue;
        double yaw, pitch; AngleOff(eye, aimDir, c.aim, yaw, pitch);
        if (std::fabs(yaw) <= lockYaw && std::fabs(pitch) <= lockPitch) fresh.push_back(&c);
    }
    std::stable_sort(fresh.begin(), fresh.end(), [&](const Candidate* a, const Candidate* b) { return offAxis(a->aim) < offAxis(b->aim); });
    for (auto* c : fresh) {
        if ((int)locks_.size() >= maxN) break;
        Lock L; L.id = c->id; L.aim = c->aim;
        L.progress = lockS <= 0 ? 1.0 : std::min(1.0, dt / lockS);
        if (L.progress >= 1.0) { L.locked = true; if (ev) ev->push_back({ LockEvent::Acquired, L.id, "" }); }
        locks_.push_back(L);
    }
}

std::vector<uint32_t> Tracker::Locked() const
{
    std::vector<uint32_t> r;
    for (auto& l : locks_) if (l.locked) r.push_back(l.id);
    return r;
}
const Lock* Tracker::Find(uint32_t id) const
{
    for (auto& l : locks_) if (l.id == id) return &l;
    return nullptr;
}
uint32_t Tracker::NextShotTarget() { return Distribute(Locked(), shots_++); }

uint32_t Distribute(const std::vector<uint32_t>& locked, uint64_t shotIndex)
{
    return locked.empty() ? 0 : locked[shotIndex % locked.size()];
}

bool RollHome(uint64_t seed, double hitP) { return U01(seed) < hitP; }

void RollSpiral(uint64_t seed, const Params::Game& g, double& cycleS, double& sign)
{
    cycleS = g.cycleMinS + (g.cycleMaxS - g.cycleMinS) * U01(seed ^ 0x51u);
    sign = U01(seed ^ 0xA7u) < 0.5 ? -1.0 : 1.0;
}

Vec3 Steer(const Vec3& pos, const Vec3& vel, const Vec3& target, double dt, const Params::Game& g, double flownU, double ageS, double spiralSign, double cycleS)
{
    const double speed = Len(vel);
    if (speed < 1e-9 || dt <= 0) return vel;
    Vec3 cur = Mul(vel, 1.0 / speed);
    Vec3 to = Sub(target, pos);
    const double dist = Len(to);
    if (dist < 1e-6) return vel;
    Vec3 want = Mul(to, 1.0 / dist);
    if (g.spiral && cycleS > 0) {
        // corkscrew: a lateral offset around the line to the target, grown in over the ramp, faded out in the last 3 m so
        // the round still connects
        double ramp = g.rampEndU > g.rampStartU ? std::clamp((flownU - g.rampStartU) / (g.rampEndU - g.rampStartU), 0.0, 1.0) : 1.0;
        double fade = std::clamp(dist / (3.0 * kUnitsPerMetre), 0.0, 1.0);
        double r = g.spiralRU * ramp * fade;
        if (r > 0) {
            Vec3 up = std::fabs(want.z) > 0.95 ? Vec3{ 1, 0, 0 } : Vec3{ 0, 0, 1 };
            Vec3 a = Norm(Cross(want, up)), b = Cross(a, want);
            double ph = spiralSign * 2 * kPi * ageS / cycleS;
            Vec3 aim = Add(target, Add(Mul(a, r * std::cos(ph)), Mul(b, r * std::sin(ph))));
            want = Norm(Sub(aim, pos));
        }
    }
    const double ang = AngleBetween(cur, want), cap = g.turnRad * dt;
    if (ang <= cap) return Mul(want, speed);
    // rotate cur toward want by cap (slerp on the unit sphere)
    const double s = std::sin(ang);
    if (s < 1e-9) return vel;   // exactly opposite: keep flying, the next frame picks a side
    const double t = cap / ang;
    Vec3 dir = Add(Mul(cur, std::sin((1 - t) * ang) / s), Mul(want, std::sin(t * ang) / s));
    return Mul(Norm(dir), speed);
}

}  // namespace cw::smart
