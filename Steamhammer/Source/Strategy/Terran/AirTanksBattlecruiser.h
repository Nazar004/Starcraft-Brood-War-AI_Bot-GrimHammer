#pragma once
#include "Strategy/Terran/AirTanksPolicy.h"

namespace UAlbertaBot { namespace AirTanksBattlecruiser
{
    inline AirTanks::Push push() { return { 4, 0, 0, 0, 0, 0, 2, false, false }; }
    inline bool techReady(int bases, int workers, int tanks, int minerals, int gas,
        bool alreadyCommitted)
    {
        return alreadyCommitted || (bases >= 2 && workers >= 30 && tanks >= 3 &&
            minerals >= 300 && gas >= 200);
    }
    inline bool transitionReady(int bases, int workers, int tanks, int minerals,
        int gas, bool alreadyCommitted)
    {
        return alreadyCommitted || (bases >= 3 && workers >= 40 && tanks >= 4 &&
            minerals >= 600 && gas >= 450);
    }
    inline int fleetTarget(int bases, bool dedicated)
    {
        return dedicated ? std::min(12, std::max(4, 2 * bases))
            : bases >= 4 ? 4 : 2;
    }
    inline int tankTarget() { return 6; }
    inline int starports(int bases) { return std::min(3, std::max(2, bases)); }
} }
