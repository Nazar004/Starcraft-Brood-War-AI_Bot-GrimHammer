#pragma once
#include "Strategy/Terran/TvZBioPlan.h"

namespace UAlbertaBot { namespace TvZSparks
{
    inline int barracks(int) { return 3; }
    inline int vessels(int, int) { return 0; }
    inline int starports(int, int) { return 0; }
    inline TvZBioPlan::Next transition(const TvZBioPlan::State& s)
    {
        // End the Sunken-break window at Lair combat tech or a failed timing.
        return s.lairArmy || s.hiveTech || s.frame >= 7 * TvZBioPlan::Minute ||
            (s.firstAttackFrame >= 0 && s.frame - s.firstAttackFrame >= 24 * 60)
            ? TvZBioPlan::Next::BioTank : TvZBioPlan::Next::Keep;
    }
    inline const TvZBioPlan::Policy& policy()
    {
        static const TvZBioPlan::Policy value = {
            "TvZ_Sparks", {20, 0, 0, false, false, false},
            0, 0, 0, 8, true, "WAIT 20M/4Med + Stim",
            barracks, vessels, starports, transition
        };
        return value;
    }
} }
