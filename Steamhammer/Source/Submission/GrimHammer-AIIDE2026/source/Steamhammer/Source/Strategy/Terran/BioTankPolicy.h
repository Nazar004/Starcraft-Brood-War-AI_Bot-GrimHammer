#pragma once

#include <algorithm>

namespace UAlbertaBot { namespace BioTankPolicy
{
    // Ordinary BioTank only. Sparks, Pre-Hive, SK and Deep Six own their targets.
    inline bool recallArmy(int enemyPower, int localDefense, bool depotCritical)
    {
        return (enemyPower >= 6 && enemyPower > localDefense + 4) ||
            (depotCritical && enemyPower > localDefense);
    }

    inline int firebats(int marines, int zerglings)
    {
        return zerglings < 4 ? 0 :
            std::min({ 4, marines / 8, (zerglings + 5) / 6 });
    }

    inline int tankPercent(int heavyGround, int lightGround, int air)
    {
        if (air > heavyGround + lightGround) return 15;
        return heavyGround >= 4 && heavyGround >= lightGround ? 30 : 20;
    }

    inline int factories(int tankTarget, int completed, int workers,
        int minerals, int gas, bool allBusy)
    {
        const int normal = tankTarget >= 5 && workers >= 28 ? 2 : 1;
        if (!allBusy || workers < 40 || minerals < 850 || gas < 400)
            return normal;
        const int capacity = tankTarget >= 10 ? 3 : normal;
        return std::max(normal, std::min(capacity, completed + 1));
    }

    inline int vessels(int marines, int bases, bool zerg,
        bool lurkers, bool defilers, bool enemyCasters)
    {
        if (!zerg) return enemyCasters ? 2 : 1;
        int target = std::max(1, (marines + 15) / 16);
        if (lurkers) target = std::max(2, target);
        if (defilers) target = std::max(3, target);
        return std::min(bases < 2 ? 1 : bases < 3 ? 4 : 6, target);
    }

    inline int guardianWraiths(int guardians, int antiAir)
    {
        return guardians > 0 && antiAir < std::max(4, guardians)
            ? std::min(6, std::max(2, guardians)) : 0;
    }

    inline int goliaths(int carriers, int battlecruisers, int guardians)
    {
        return std::min(20, std::max({ carriers > 0 ? 4 + 3 * carriers : 0,
            2 * battlecruisers, 2 * guardians }));
    }
} }
