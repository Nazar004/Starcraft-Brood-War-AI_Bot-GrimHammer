#include "Map/Grids/GridDistances.h"

#include "Core/The.h"

#include <algorithm>
#include <cstdlib>

using namespace UAlbertaBot;

namespace
{
    int distanceComputeFrame = -1;
    int distanceComputeCount = 0;
}

int GridDistances::computationsAtFrame(int frame)
{
    return distanceComputeFrame == frame ? distanceComputeCount : 0;
}

GridDistances::GridDistances()
    : Grid()
{
}

GridDistances::GridDistances(const BWAPI::TilePosition & start, bool neutralBlocks, bool runtimeBlocks)
    : Grid(BWAPI::Broodwar->mapWidth(), BWAPI::Broodwar->mapHeight(), -1)
{
    compute(start, MAX_DISTANCE, neutralBlocks, runtimeBlocks);
}

GridDistances::GridDistances(const BWAPI::TilePosition & start, int limit, bool neutralBlocks, bool runtimeBlocks)
    : Grid(BWAPI::Broodwar->mapWidth(), BWAPI::Broodwar->mapHeight(), -1)
{
    compute(start, limit, neutralBlocks, runtimeBlocks);
}

void GridDistances::recompute(const BWAPI::TilePosition & start, bool neutralBlocks, bool runtimeBlocks)
{
    recompute(start, MAX_DISTANCE, neutralBlocks, runtimeBlocks);
}

void GridDistances::recompute(const BWAPI::TilePosition & start, int limit, bool neutralBlocks, bool runtimeBlocks)
{
    width = BWAPI::Broodwar->mapWidth();
    height = BWAPI::Broodwar->mapHeight();
    grid = std::vector< std::vector<short> >(width, std::vector<short>(height, short(-1)));
    sortedTilePositions.clear();
    compute(start, limit, neutralBlocks, runtimeBlocks);
}

int GridDistances::getStaticUnitDistance(const BWAPI::Unit unit) const
{
    BWAPI::TilePosition upperLeft(unit->getInitialTilePosition());
    int dist = at(upperLeft);
    if (dist < 0)
    {
        dist = at(upperLeft.x + unit->getType().tileWidth() - 1, upperLeft.y + unit->getType().tileHeight() - 1);
    }
    return dist;
}

const std::vector<BWAPI::TilePosition> & GridDistances::getSortedTiles() const
{
    return sortedTilePositions;
}

// Compute a fast reusable distance field on the 32x32 build-tile grid.
//
// IMPORTANT: High-resolution terrain recognition remains on the native 8x8
// walk grid in MapPartitions/GridInset/GridZone. GridDistances is a frequently
// used runtime cache (combat, scouting, expansion scoring), so constructing a
// full 8x8 Dijkstra field for every new moving destination caused visible
// frame hitches. The build-tile grid now uses the improved dynamic walkability
// from MapTools plus 8-direction movement and corner-cut prevention. That keeps
// runtime distances responsive while Area/Choke recognition stays precise.
void GridDistances::compute(const BWAPI::TilePosition & start, int limit, bool neutralBlocks, bool runtimeBlocks)
{
    const int frame = BWAPI::Broodwar->getFrameCount();
    if (distanceComputeFrame != frame)
    {
        distanceComputeFrame = frame;
        distanceComputeCount = 0;
    }
    ++distanceComputeCount;
    if (!start.isValid())
    {
        return;
    }

    const int actionX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    const int actionY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

    auto tilePassable = [neutralBlocks, runtimeBlocks](const BWAPI::TilePosition & tile) -> bool
    {
        if (!tile.isValid())
        {
            return false;
        }
        if (runtimeBlocks)
        {
            return the.map.isRuntimeWalkable(tile);
        }
        return neutralBlocks
            ? the.map.isWalkable(tile)
            : the.map.isTerrainWalkable(tile);
    };

    std::vector<BWAPI::TilePosition> fringe;
    fringe.reserve(width * height);

    if (tilePassable(start))
    {
        fringe.push_back(start);
        grid[start.x][start.y] = 0;
        sortedTilePositions.push_back(start);
    }
    else
    {
        // The requested target can legitimately be inside a building footprint
        // (enemy CC/refinery, a base center while runtime blockers are enabled,
        // etc.). Seeding an unwalkable tile produces a distance field with no
        // outgoing edges. Seed the nearest reachable ring instead. Units then
        // route to the edge of the obstacle and the normal attack/move order can
        // handle the final few pixels.
        const int MaxSeedRadius = 6;
        bool seeded = false;
        for (int radius = 1; radius <= MaxSeedRadius && !seeded; ++radius)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                for (int dy = -radius; dy <= radius; ++dy)
                {
                    if (std::max(std::abs(dx), std::abs(dy)) != radius)
                    {
                        continue;
                    }

                    const BWAPI::TilePosition seed(start.x + dx, start.y + dy);
                    if (!seed.isValid() || !tilePassable(seed) || grid[seed.x][seed.y] >= 0)
                    {
                        continue;
                    }

                    grid[seed.x][seed.y] = 0;
                    fringe.push_back(seed);
                    sortedTilePositions.push_back(seed);
                    seeded = true;
                }
            }
        }

        if (!seeded)
        {
            return;
        }
    }

    for (size_t fringeIndex = 0; fringeIndex < fringe.size(); ++fringeIndex)
    {
        const BWAPI::TilePosition here = fringe[fringeIndex];
        const int currentDistance = grid[here.x][here.y];

        if (currentDistance >= limit)
        {
            continue;
        }

        for (int a = 0; a < 8; ++a)
        {
            const BWAPI::TilePosition next(
                here.x + actionX[a],
                here.y + actionY[a]);

            if (!next.isValid() || grid[next.x][next.y] >= 0 || !tilePassable(next))
            {
                continue;
            }

            bool transitionOpen = false;
            if (neutralBlocks)
            {
                transitionOpen = runtimeBlocks
                    ? the.map.isGroundTransitionRuntimeWalkable(here, next)
                    : the.map.isGroundTransitionWalkable(here, next);
            }
            else
            {
                // Terrain-only distance maps are used during base/resource
                // initialization. Static BWAPI walkability is enough here; use
                // the same two-mini-tile border rule without neutral blockers.
                const int dx = next.x - here.x;
                const int dy = next.y - here.y;

                auto cardinalTerrainOpen = [](BWAPI::TilePosition p, BWAPI::TilePosition q) -> bool
                {
                    const int cdx = q.x - p.x;
                    const int cdy = q.y - p.y;
                    if (std::abs(cdx) + std::abs(cdy) != 1)
                    {
                        return false;
                    }

                    int run = 0;
                    int bestRun = 0;
                    if (cdx != 0)
                    {
                        const int px = cdx > 0 ? p.x * 4 + 3 : p.x * 4;
                        const int qx = cdx > 0 ? q.x * 4 : q.x * 4 + 3;
                        for (int k = 0; k < 4; ++k)
                        {
                            const bool open = BWAPI::Broodwar->isWalkable(px, p.y * 4 + k) &&
                                BWAPI::Broodwar->isWalkable(qx, p.y * 4 + k);
                            run = open ? run + 1 : 0;
                            bestRun = std::max(bestRun, run);
                        }
                    }
                    else
                    {
                        const int py = cdy > 0 ? p.y * 4 + 3 : p.y * 4;
                        const int qy = cdy > 0 ? q.y * 4 : q.y * 4 + 3;
                        for (int k = 0; k < 4; ++k)
                        {
                            const bool open = BWAPI::Broodwar->isWalkable(p.x * 4 + k, py) &&
                                BWAPI::Broodwar->isWalkable(p.x * 4 + k, qy);
                            run = open ? run + 1 : 0;
                            bestRun = std::max(bestRun, run);
                        }
                    }
                    return bestRun >= 2;
                };

                if (dx == 0 || dy == 0)
                {
                    transitionOpen = cardinalTerrainOpen(here, next);
                }
                else
                {
                    const BWAPI::TilePosition sideX(here.x + dx, here.y);
                    const BWAPI::TilePosition sideY(here.x, here.y + dy);
                    const bool viaX = sideX.isValid() && tilePassable(sideX) &&
                        cardinalTerrainOpen(here, sideX) && cardinalTerrainOpen(sideX, next);
                    const bool viaY = sideY.isValid() && tilePassable(sideY) &&
                        cardinalTerrainOpen(here, sideY) && cardinalTerrainOpen(sideY, next);
                    transitionOpen = viaX || viaY;
                }
            }

            if (!transitionOpen)
            {
                continue;
            }

            grid[next.x][next.y] = short(std::min(currentDistance + 1, 32767));
            fringe.push_back(next);
            sortedTilePositions.push_back(next);
        }
    }

}
