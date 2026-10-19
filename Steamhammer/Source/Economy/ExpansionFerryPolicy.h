#pragma once

#include <algorithm>

namespace UAlbertaBot { namespace ExpansionFerryPolicy
{
    // Worker freight has its own capacity; combat-drop unit limits do not apply.
    inline int workersToLoad(int cargoSpace, int workerSpace, int targetDeficit,
        int sourceWorkers, int sourceReserve)
    {
        if (workerSpace <= 0) return 0;
        return std::max(0, std::min({ cargoSpace / workerSpace, targetDeficit,
            sourceWorkers - sourceReserve }));
    }
} }
