// tint.h - runtime colour / glow of the worn cyberware 3D (see tint.cpp). Game thread only.
#pragma once
#include "cw_core.h"

#include <cstdint>
#include <string>

namespace tint {
    void RegisterVisual(std::uint32_t armoFormId, const std::string& target);   // a visual ARMO -> mantis|monowire|gorilla|launcher
    void Configure(const cw::json& resolved);                                   // cw::CustomizeResolve output
    void Scan();                                                                // ~15 Hz: find new 3D, apply what changed
    void Frame();                                                               // every frame: pulses + idle glow; returns fast when neither runs
    bool Pulse(float strength);                                                 // an ability fired; false when pulses are off
    bool Charge(const std::string& target, bool fire);                          // PLS: fire=false starts the wind-up ramp on target, fire=true flashes and settles it
    void SetEyesAllowed(bool on);                                               // Kiroshi installed: the player's eyes may glow (see tint.cpp)
    void OnLoad();                                                              // a save was loaded: forget every shape
    void Dump();                                                                // test seam: log every shape we hold + the player's eye shapes
    // test seam: is target's tinted part drawn and on screen in the current view? Logs TINT_WHERE_OK with a w x h pixel box for
    // shot-gate.ps1 -Region, or TINT_WHERE_FAIL with the reason. only = "" checks every held target. Returns "ok" or "fail".
    std::string Where(const std::string& only, int w = 2560, int h = 1440);
    // test seam: why is target (or every registered target, "") held or not? Walks every biped slot and logs each shader's verdict
    // (the scan's own SkipReason); ends with TINT_WORN_OK / TINT_WORN_FAIL <target>: <reason>. Returns "ok" or "fail".
    std::string Worn(const std::string& only);
    struct Rgb { float r, g, b; };                                              // 0..1, same space as a BGSM emittance colour (hex / 255, no gamma)
    // The player's choice for mantis|monowire|gorilla|launcher|kiroshi|sandevistan|hud. False = no colour chosen (stock): use your own
    // default. glow (optional) gets 0..100, 50 = authored brightness, also on false. Filled ~250 ms after data ready, updated at 4 Hz.
    bool Color(const std::string& target, Rgb& out, int* glow = nullptr);
    bool PulsesOn();                                                            // the player's Ability pulse switch
    std::string Stats();                                                        // one line for the log / the seam
}
