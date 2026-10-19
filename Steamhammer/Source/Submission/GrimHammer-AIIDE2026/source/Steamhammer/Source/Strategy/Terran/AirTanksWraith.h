#pragma once
#include "Strategy/Terran/AirTanksPolicy.h"

namespace UAlbertaBot { namespace AirTanksWraith
{
    inline AirTanks::Push push(bool twoPorts)
    {
        return { twoPorts ? 2 : AirTanks::TankWraithFirstPushTanks,
            4, 0, 0, 0, 0, 0, false, false };
    }
    inline int airTarget(int tanks, bool antiAir)
    {
        return AirTanks::wraithTarget(tanks, antiAir);
    }
} }
