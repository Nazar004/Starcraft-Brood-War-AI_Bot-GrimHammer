#include "Map/Grids/GridTileRoom.h"

#include "Core/The.h"

#include <algorithm>

using namespace UAlbertaBot;

GridTileRoom::GridTileRoom()
    : Grid()
{
}

// Reduce the 8x8 room map to build-tile resolution.
//
// The old code used the maximum of the 16 walk tiles. One roomy corner could
// therefore make a mostly narrow 32x32 tile look wide. We use the median of the
// positive walk-tile values instead; it is stable around ramps/diagonals and
// preserves the same walk-tile unit scale used by callers.
void GridTileRoom::initialize()
{
    width = BWAPI::Broodwar->mapWidth();
    height = BWAPI::Broodwar->mapHeight();
    grid = std::vector< std::vector<short> >(width, std::vector<short>(height, short(0)));

    for (int tileX = 0; tileX < width; ++tileX)
    {
        for (int tileY = 0; tileY < height; ++tileY)
        {
            short values[16];
            int count = 0;
            bool walkable[4][4] = {};

            for (int dx = 0; dx < 4; ++dx)
            {
                for (int dy = 0; dy < 4; ++dy)
                {
                    const int value = the.vWalkRoom.at(tileX * 4 + dx, tileY * 4 + dy);
                    if (value > 0)
                    {
                        walkable[dx][dy] = true;
                        values[count++] = short(value);
                    }
                }
            }

            // A single 8x8 crack is not a useful build-tile passage. Require at
            // least one contiguous 2x2 block of walk tiles before exposing the
            // tile to the high-level zone/choke graph.
            bool hasPassage = false;
            for (int dx = 0; dx < 3 && !hasPassage; ++dx)
            {
                for (int dy = 0; dy < 3 && !hasPassage; ++dy)
                {
                    hasPassage = walkable[dx][dy] && walkable[dx + 1][dy] &&
                        walkable[dx][dy + 1] && walkable[dx + 1][dy + 1];
                }
            }

            if (!hasPassage || count == 0)
            {
                grid[tileX][tileY] = 0;
                continue;
            }

            std::sort(values, values + count);
            grid[tileX][tileY] = values[(count - 1) / 2];
        }
    }
}

void GridTileRoom::draw() const
{
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            int d = grid[x][y];
            if (d > 0)
            {
                BWAPI::Broodwar->drawTextMap(
                    BWAPI::Position(BWAPI::TilePosition(x, y)) + BWAPI::Position(2, 8),
                    "%d", d);
            }
        }
    }
}
