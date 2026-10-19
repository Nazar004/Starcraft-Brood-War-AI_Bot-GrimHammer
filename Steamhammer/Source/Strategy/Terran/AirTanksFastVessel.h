#pragma once
#include "Strategy/Terran/AirTanksPolicy.h"

namespace UAlbertaBot { namespace AirTanksFastVessel
{
    inline AirTanks::Push push() { return { 2, 0, 16, 4, 0, 1, 0, true, true }; }
    inline int barracks(int workers) { return workers >= 40 ? 5 : 3; }
    inline int marines(int workers) { return std::min(80, std::max(16, workers + 8)); }
    inline int tankTarget(int marines) { return std::min(8, std::max(2, marines / 8)); }
    inline int vessels(int bases) { return std::min(6, std::max(1, 2 * bases)); }
} }
