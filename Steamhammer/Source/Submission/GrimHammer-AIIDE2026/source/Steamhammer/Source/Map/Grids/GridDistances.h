#pragma once

#include <vector>
#include "BWAPI.h"
#include "Map/Grids/Grid.h"

namespace UAlbertaBot
{
class GridDistances : public Grid
{
    std::vector<BWAPI::TilePosition> sortedTilePositions;

    void compute(const BWAPI::TilePosition & start, int limit, bool neutralBlocks, bool runtimeBlocks);

public:
    static int computationsAtFrame(int frame);
    GridDistances();
    GridDistances(const BWAPI::TilePosition & start, bool neutralBlocks = true, bool runtimeBlocks = false);
    GridDistances(const BWAPI::TilePosition & start, int limit, bool neutralBlocks = true, bool runtimeBlocks = false);

    // Rebuild an existing distance map after dynamic map topology changes.
    void recompute(const BWAPI::TilePosition & start, bool neutralBlocks = true, bool runtimeBlocks = false);
    void recompute(const BWAPI::TilePosition & start, int limit, bool neutralBlocks = true, bool runtimeBlocks = false);

    int getStaticUnitDistance(const BWAPI::Unit unit) const;

    const std::vector<BWAPI::TilePosition> & getSortedTiles() const;
};
}
