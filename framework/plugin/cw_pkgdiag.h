// cw_pkgdiag.h - the {"cmd":"packages"} test seam: an actor's AI packages, read straight from the forms. See cw_pkgdiag.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace pkgdiag
{
    // From ReadIni: [Clinic] sClinicCell ("Cyberware - Clinic.esp|800"); only its plugin name is used, to resolve Varga's
    // package EditorIDs to runtime form ids (the load-order prefix is read from the loaded plugin, never hardcoded).
    void SetClinicPlugin(const std::string& clinicCellRef);
    // Optional: an extra EditorID lookup for runtime form ids (main.cpp passes Cyberware.esp's formmap). Empty = none.
    void SetNameLookup(std::function<std::string(std::uint32_t)> fn);
    // Logs one header line, one line per package and one per condition item, then a summary. Game thread only.
    // Read-only: no Papyrus, no package evaluation call, nothing written to the actor.
    void Log(RE::Actor* actor);
}
