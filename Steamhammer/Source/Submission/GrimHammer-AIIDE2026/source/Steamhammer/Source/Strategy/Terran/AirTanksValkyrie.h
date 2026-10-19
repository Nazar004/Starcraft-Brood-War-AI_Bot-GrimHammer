#pragma once
#include "Strategy/Terran/AirTanksPolicy.h"

namespace UAlbertaBot { namespace AirTanksValkyrie
{
    // Fantasy keeps a mech army. Valkonic keeps its own Marine/Medic/Tank army.
    inline AirTanks::Push push(bool valkonic)
    {
        return valkonic ? AirTanks::Push{ 5, 0, 24, 5, 0, 1, 0, true, false }
            : AirTanks::Push{ 4, 0, 0, 0, 4, 0, 0, false, false };
    }
    inline int airTarget(int mutalisks, int combatAir, bool spire)
    {
        return combatAir > 0 || spire
            ? std::min(6, std::max(2, (mutalisks + 3) / 4)) : 0;
    }
    inline int tankTarget(int marines, bool valkonic, int bases)
    {
        return valkonic ? std::min(12, std::max(5, marines / 6))
            : std::min(20, 4 + 4 * std::max(0, bases - 1));
    }
    inline int barracks(int workers) { return workers >= 40 ? 5 : 3; }
    inline int marines(int workers) { return std::min(80, std::max(16, workers + 8)); }
} }
