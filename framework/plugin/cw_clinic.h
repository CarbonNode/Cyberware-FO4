// cw_clinic.h - Varga's daily schedule mover (CLINIC stream). See cw_clinic.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace clinic
{
    // From ReadIni: [Clinic] bSchedule, the refs ("Plugin.esp|hex") and the two landing anchors ("x,y,z,rz").
    void Configure(bool enabled, const std::string& clinicCell, const std::string& marketWorldspace,
                   const std::string& clinicAnchor, const std::string& marketAnchor);
    // From the formmap loader: CW_RipperdocRef_diamondcity's plugin + local id.
    void SetVargaRef(const std::string& plugin, std::uint32_t localId);
    // From the frame pump's slow section; internally 0.2 Hz. talking = the ripperdoc talk/menu is open (no package nudge).
    void Tick(std::uint64_t now, bool talking);
    // INSTALL SCENE's ready guard: the runtime FormID of the package Varga's clock wants now while she is in the clinic
    // (0 = no opinion), and hers = `running` is one of her own clinic packages. Read only. actorFormId must be Varga's ref.
    std::uint32_t DueInClinic(std::uint32_t actorFormId, std::uint32_t running, bool& hers);
    // One line for the {"cmd":"clinic"} test command.
    std::string StatusLine();
}
