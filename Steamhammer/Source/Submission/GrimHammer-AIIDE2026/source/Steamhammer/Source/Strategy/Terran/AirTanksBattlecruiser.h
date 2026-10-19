#pragma once
#include "Strategy/Terran/AirTanksPolicy.h"

namespace UAlbertaBot { namespace AirTanksBattlecruiser
{
    inline AirTanks::Push push() { return { 4, 0, 0, 0, 4, 0, 0, false, false }; }
    inline bool techReady(int bases, int workers, int tanks, int minerals, int gas,
        bool alreadyCommitted)
    {
        return alreadyCommitted || (bases >= 3 && workers >= 44 && tanks >= 4 &&
            minerals >= 800 && gas >= 600);
    }
    inline int fleetTarget(int bases) { return std::min(16, std::max(2, 4 * bases)); }
    inline int tankTarget(int bases) { return std::min(12, std::max(4, 4 * bases)); }
    inline int starports(int bases) { return std::min(4, std::max(2, bases)); }
} }
