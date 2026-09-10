#include "AlertPublisher.h"

namespace fc {

bool AlertPublisher::update(uint16_t globalActive,
                            const uint16_t* groupActive,
                            AlertChanges& changes)
{
    changes.globalRaised = globalActive & ~previousGlobalActive_;
    changes.globalCleared = previousGlobalActive_ & ~globalActive;
    changes.globalActive = globalActive;
    bool changed = changes.globalRaised || changes.globalCleared;

    previousGlobalActive_ = globalActive;

    for (uint8_t group = 0; group < kGroupCount; ++group) {
        changes.groupRaised[group] =
            groupActive[group] & ~previousGroupActive_[group];
        changes.groupCleared[group] =
            previousGroupActive_[group] & ~groupActive[group];
        changes.groupActive[group] = groupActive[group];
        changed |= changes.groupRaised[group] || changes.groupCleared[group];
        previousGroupActive_[group] = groupActive[group];
    }

    return changed;
}

} // namespace fc
