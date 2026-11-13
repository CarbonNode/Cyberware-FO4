// cw_smart.h - ARMORY smart guns: target lock + projectile steering, game independent (docs/ARMORY.md §3).
//
// The plugin feeds eye position, aim direction, ADS state and candidate targets every frame; Tracker decides what is
// locked. For each live smart projectile the plugin calls Steer() with the projectile's position and velocity and the
// locked target's aim point. Params arrive in CP's units (metres, m/s, degrees, seconds; keys named after CP's
// BaseStats.SmartGun* stats minus the prefix) and are converted to game units in ONE place, Params::Game().
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cw_core.h"   // Vec3, json

namespace cw::smart {

constexpr double kUnitsPerMetre = 69.99;

// CP numbers. Defaults = HJKE-11 Yukimura with Smart Link installed (Base_Yukimura_SmartGun_Stats +
// Base_Yukimura_SmartGun_SmartLink_Stats, CP 2.x REDmod tweaks, read 2026-11-09). Angles are half-angles off the aim
// axis (assumption: CP does not document whether its LockingAngle is full or half; half reads like the game).
struct Params {
    double HipLockingAngleYaw = 9, HipLockingAnglePitch = 9;         // a target inside this cone builds lock
    double AdsLockingAngleYaw = 9, AdsLockingAnglePitch = 9;
    double HipTargetableAngleYaw = 9, HipTargetableAnglePitch = 9;   // a locked target outside this cone starts unlocking
    double AdsTargetableAngleYaw = 9, AdsTargetableAnglePitch = 9;
    int HipMaxLockedTargets = 3, AdsMaxLockedTargets = 1;
    bool TrackMultipleEntitiesInADS = false;
    double HipTimeToLock = 1.2, AdsTimeToLock = 1.2;                  // s
    double HipTimeToUnlock = 1.6, AdsTimeToUnlock = 1.6;              // s outside the targetable cone before the lock drops
    double TimeToRemoveOccludedTarget = 1.6;                          // s without LOS before the lock drops
    double TargetAcquisitionRange = 50;                               // m (CP base_weapon default; per gun overrides)
    double PlayerProjectileVelocity = 75;                             // m/s (40 base + 35 Smart Link)
    double HitProbability = 0.95;                                     // chance a round homes at all (0.70 + 0.25)
    bool AddSpiralTrajectory = true;
    double SpiralRadius = 0.1;                                        // m
    double SpiralCycleTimeMin = 0.10, SpiralCycleTimeMax = 0.15;      // s per turn of the corkscrew
    double SpiralRampDistanceStart = 0.0, SpiralRampDistanceEnd = 1.0;   // m flown: the corkscrew grows in over this
    double EvenDistributionPeriod = 0.2;                              // s
    bool UseEvenDistributionTargeting = true;
    // Ours, not CP's: how fast a round may turn. CP has no such stat (its rounds follow a spline); 540 deg/s curves
    // visibly at 10-40 m and still connects with a strafing target.
    double TurnRateDeg = 540;

    static Params FromJson(const json& j, Params base);   // missing keys keep base; unknown keys ignored
    static Params FromJson(const json& j);                // ... over the Yukimura defaults

    struct Game {                                     // the same, in game units (units, units/s, radians)
        double hipLockYaw, hipLockPitch, adsLockYaw, adsLockPitch;
        double hipTgtYaw, hipTgtPitch, adsTgtYaw, adsTgtPitch;
        int hipMax, adsMax;
        double hipLockS, adsLockS, hipUnlockS, adsUnlockS, occludedS;
        double rangeU, speedU, hitP;
        bool spiral; double spiralRU, cycleMinS, cycleMaxS, rampStartU, rampEndU;
        double turnRad;
    };
    Game ToGame() const;
};

struct Candidate {
    uint32_t id = 0;
    Vec3 aim;                       // the point a round should fly to (chest / 3D bound centre)
    bool alive = false, hostile = false, los = false;
};

struct Lock {
    uint32_t id = 0;
    Vec3 aim;
    double progress = 0;            // 0..1; 1 = locked
    bool locked = false;
    double outS = 0, occludedS = 0; // time spent outside the targetable cone / without LOS
};

struct LockEvent { enum Kind { Acquired, Lost } kind; uint32_t id; std::string why; };

// Yaw / pitch (radians, signed) of `to` as seen from `eye` looking along `dir` (FO4: +Z up).
void AngleOff(const Vec3& eye, const Vec3& dir, const Vec3& to, double& yaw, double& pitch);

class Tracker {
public:
    void Update(const Vec3& eye, const Vec3& aimDir, bool ads, const std::vector<Candidate>& c, const Params& p, double dt,
                std::vector<LockEvent>* events = nullptr);
    const std::vector<Lock>& Locks() const { return locks_; }
    std::vector<uint32_t> Locked() const;
    void Reset() { locks_.clear(); shots_ = 0; }
    // The target the next fired round goes to (CP's even distribution: round robin over the locked targets), or 0.
    uint32_t NextShotTarget();
    const Lock* Find(uint32_t id) const;
private:
    std::vector<Lock> locks_;
    uint64_t shots_ = 0;
};

// Which locked target the Nth round of a burst goes to (round robin), 0 if none.
uint32_t Distribute(const std::vector<uint32_t>& locked, uint64_t shotIndex);

// One steering step: the new velocity of a round at `pos` flying `vel`, homing on `target` (+ the corkscrew offset),
// turning at most turnRad*dt. Speed is kept exactly. `flownU` = distance flown so far, `ageS` its age, `spiralSign`
// +1/-1 (CP randomizes the direction), `cycleS` this round's spiral period.
Vec3 Steer(const Vec3& pos, const Vec3& vel, const Vec3& target, double dt, const Params::Game& g,
           double flownU = 0, double ageS = 0, double spiralSign = 1, double cycleS = 0.12);

// Deterministic per-round roll (splitmix64 of the seed): does it home at all (HitProbability)?
bool RollHome(uint64_t seed, double hitP);
// This round's spiral period and direction from its seed.
void RollSpiral(uint64_t seed, const Params::Game& g, double& cycleS, double& sign);

double Len(const Vec3& v);
Vec3 Sub(const Vec3& a, const Vec3& b);
double AngleBetween(const Vec3& a, const Vec3& b);   // radians

}  // namespace cw::smart
