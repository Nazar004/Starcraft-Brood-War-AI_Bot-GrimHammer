#pragma once
#include "Strategy/Terran/TvZBioPlan.h"

namespace UAlbertaBot { namespace TvZSK
{
    inline int barracks(int bases) { return bases < 2 ? 4 : std::min(10, 3 * bases); }
    inline int vessels(int marines, int bases)
    {
        return std::min(bases < 2 ? 3 : 10, std::max(2, marines / 8));
    }
    inline int starports(int marines, int bases) { return bases >= 2 && marines >= 24 ? 2 : 1; }
    inline TvZBioPlan::Next transition(const TvZBioPlan::State&)
    {
        // Replace lost Vessels without changing to Tank production.
        return TvZBioPlan::Next::Keep;
    }
    inline const TvZBioPlan::Policy& policy()
    {
        static const TvZBioPlan::Policy value = {
            // Vessel/Irradiate remain production goals. The first sortie has
            // a separate earlier readiness check in StrategyManager.
            "TvZ_SK", {24, 0, 1, false, true, true},
            1, 0, 0, 8, false, "WAIT 16M/4Med + Stim",
            barracks, vessels, starports, transition
        };
        return value;
    }
} }
