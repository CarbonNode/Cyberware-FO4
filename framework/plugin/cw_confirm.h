// cw_confirm.h - "did the action really play?" for the test seam. See cw_confirm.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace confirm
{
    // Start waiting for one of `tags` (| separated player graph event tags, case-insensitive) within `ms`.
    // Empty tags / ms 0 = the default for that action name (cw_confirm.cpp). Logs one line when it plays or times out.
    void Expect(const std::string& name, const std::string& tags = "", int ms = 0);
    // Every player graph event tag (the arms anim hook calls this; any thread).
    void OnAnimEvent(const char* tag);
    // A plugin marker for an action that has no graph event of its own (the leap launch, the slam impact): matches tag "@<name>".
    void Mark(const char* name);
    // The 4 Hz slow tick (game thread): times out what never played.
    void Tick(std::uint64_t nowMs);
}
