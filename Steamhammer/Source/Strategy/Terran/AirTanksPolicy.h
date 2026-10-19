#pragma once

#include <algorithm>
#include <string>

namespace UAlbertaBot { namespace AirTanks
{
    enum class Variant { TankWraith, TwoPortWraith, Fantasy, Valkonic, FastVessel, Battlecruiser };

    inline Variant variantFromName(const std::string& name)
    {
        if (name == "two_port_wraith") return Variant::TwoPortWraith;
        if (name == "fantasy") return Variant::Fantasy;
        if (name == "valkonic") return Variant::Valkonic;
        if (name == "fast_vessel") return Variant::FastVessel;
        if (name == "battlecruiser") return Variant::Battlecruiser;
        return Variant::TankWraith;
    }

    struct Push
    {
        int tanks, wraiths, marines, medics, goliaths, vessels, battlecruisers;
        bool stim, irradiate;
    };

    // This policy owns AirTanks exclusively. No BioTank ratios or release state.
    constexpr int FirstPushTanks = 4;
    constexpr int TankWraithFirstPushTanks = 6;
    constexpr int FirstPushWraiths = 4;
    constexpr int ThirdMinute = 9;

    inline int tankTarget(int wraiths, int bases, bool strongAntiAir = false)
    {
        return std::min(28, std::max(FirstPushTanks,
            strongAntiAir ? 8 + 4 * bases : 2 * wraiths + 2 * std::max(0, bases - 2)));
    }

    inline int wraithTarget(int tanks, bool strongAntiAir)
    {
        return strongAntiAir ? FirstPushWraiths
            : std::min(16, std::max(FirstPushWraiths, 2 + tanks / 2));
    }

    // Tank/Wraith variants spend excess minerals on a mobile ground screen
    // when gas cannot keep both Factories and Starports busy.
    inline int tankWraithVultureTarget(int tanks, int minerals, int gas)
    {
        if (tanks < 1 || minerals < 350 ||
            (gas >= 200 && minerals < 800)) return 0;
        const int normalScreen = std::min(8, std::max(4, tanks + 2));
        return minerals >= 700 && gas < 200
            ? std::min(16, std::max(normalScreen, 2 * tanks))
            : normalScreen;
    }

    inline int valkyrieTarget(int mutalisks, int bases)
    {
        return mutalisks >= 8 && bases >= 2
            ? std::min(6, (mutalisks + 3) / 4) : 0;
    }

    inline int factoryTarget(int workers, int bases)
    {
        return bases < 2 || workers < 24 ? 1 : workers < 40 ? 2
            : workers < 55 ? 3 : 4;
    }

    inline int starportTarget(int workers, int bases, int tanks, int wraiths, int gas)
    {
        if (bases < 2 || workers < 32 || tanks < TankWraithFirstPushTanks ||
            wraiths < FirstPushWraiths || gas < 250) return 1;
        return bases >= 3 && workers >= 48 && wraiths >= 8 ? 3 : 2;
    }
} }
