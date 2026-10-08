#pragma once

#include <map>
#include <set>

namespace Stp {

// Session-only appearance. Occurrence indices identify instances, so identical
// part definitions at different placements can have independent overrides.
struct TemporaryAppearance {
    int transparency = 0;
    bool selectionOnly = false;
    std::map<int, int> instanceTransparency;
    std::set<int> hidden;

    bool operator==(const TemporaryAppearance& other) const
    {
        return transparency == other.transparency && selectionOnly == other.selectionOnly
            && instanceTransparency == other.instanceTransparency && hidden == other.hidden;
    }
    bool operator!=(const TemporaryAppearance& other) const { return !(*this == other); }
};

} // namespace Stp
