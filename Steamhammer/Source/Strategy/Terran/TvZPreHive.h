#pragma once
#include "Strategy/Terran/TvZBioPlan.h"

namespace UAlbertaBot { namespace TvZPreHive
{
    inline int barracks(int bases) { return bases >= 2 ? 5 : 3; }
    inline int vessels(int, int) { return 2; }
    inline int starports(int, int) { return 1; }
    inline TvZBioPlan::Next transition(const TvZBioPlan::State& s)
    {
        // A failed Vessel chain must not indefinitely park a large army.
        if (s.firstAttackFrame < 0 && s.frame >= 11 * TvZBioPlan::Minute &&
            (s.vessels == 0 || !s.irradiate)) return TvZBioPlan::Next::BioTank;
        if (s.hiveTech || (s.firstAttackFrame >= 0 &&
            s.frame - s.firstAttackFrame >= 24 * 90)) return TvZBioPlan::Next::SK;
        return TvZBioPlan::Next::Keep;
    }
    inline const TvZBioPlan::Policy& policy()
    {
        static const TvZBioPlan::Policy value = {
            "TvZ_PreHive", {24, 3, 1, true, true, true},
            1, 1, 3, 9, false, "WAIT 24M/5Med/3T/1V + Stim/Siege/Range/Irradiate",
            barracks, vessels, starports, transition
        };
        return value;
    }
} }
