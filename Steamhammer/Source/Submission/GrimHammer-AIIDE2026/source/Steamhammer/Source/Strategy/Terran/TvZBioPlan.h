#pragma once
#include <algorithm>

namespace UAlbertaBot { namespace TvZBioPlan
{
    // Pure contracts; strategy-specific tuning and transitions live separately.
    enum class Next { Keep, BioTank, SK };
    struct Push { int marines, tanks, vessels; bool siege, range, irradiate; };
    struct State
    {
        int frame, firstAttackFrame, vessels;
        bool lairArmy, hiveTech, irradiate;
    };
    struct Policy
    {
        const char* name;
        Push push;
        int factories, machineShops, tankFloor, thirdMinute;
        bool oneBaseTiming;
        const char* waiting;
        int (*barracks)(int bases);
        int (*vessels)(int marines, int bases);
        int (*starports)(int marines, int bases);
        Next (*transition)(const State& state);
    };
    constexpr int Minute = 24 * 60;
} }
