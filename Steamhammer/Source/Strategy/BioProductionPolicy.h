#pragma once

#include <algorithm>

namespace UAlbertaBot
{
namespace BioProductionPolicy
{
    inline int medicTarget(int marines, int firebats = 0, int ghosts = 0)
    {
        return (std::max(0, marines) + std::max(0, firebats) + std::max(0, ghosts) + 4) / 5;
    }

    // Normal income keeps the original two-Barracks-per-base plan. Surplus
    // funds more throughput, with a bounded number of simultaneous projects.
    inline int barracksTarget(int normal, int workers, int minerals,
        int completed, bool pressure)
    {
        if (!pressure) return normal;
        const int extra = minerals >= 1800 ? 4 : minerals >= 1200 ? 2 : 1;
        const int economyCap = std::min(12, std::max(normal, (workers + 3) / 4));
        const int pendingLimit = minerals >= 1800 ? 3 : 2;
        return std::max(normal, std::min({ normal + extra,
            economyCap, completed + pendingLimit }));
    }

    inline int tankPercent(int minerals, int gas, int infantrySupply, bool macro)
    {
        if (!macro || infantrySupply < 32 || minerals < 700 || gas < 600) return 20;
        return minerals >= 1200 && gas >= 1200 ? 30 : 25;
    }

    inline int tankCeiling(int infantrySupply, int tankSupply, int percent)
    {
        return std::max(1, infantrySupply * percent / ((100 - percent) * tankSupply));
    }

    inline int factoryTarget(int normal, int completed, int workers,
        int minerals, int gas, bool pressure, bool allBusy)
    {
        // Fill current Factories first. Gas starvation or an idle Factory is
        // not a reason to pay for another one. Add at most one pending Factory.
        if (!pressure || !allBusy || workers < 28 || minerals < 850 || gas < 600)
            return normal;
        const int extra = minerals >= 1800 && gas >= 1200 ? 2 : 1;
        return std::max(normal, std::min({ 4, normal + extra, completed + 1 }));
    }
}
}
