#include "Map/Grids/GridRoom.h"

#include "Core/The.h"

#include <algorithm>

using namespace UAlbertaBot;

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

GridRoom::GridRoom()
    : GridWalk()
{
}

// Compute an orientation-independent local passage width for every 8x8 walk tile.
//
// The old implementation measured only vertical sections. That made horizontal
// and diagonal passages look different even when they had the same true width.
// Here we precompute continuous walkable spans on four axes (horizontal,
// vertical and the two diagonals) and use the narrowest cross-section. A long
// corridor therefore reports its actual narrow dimension regardless of angle.
//
// The value is still measured in walk tiles, so existing callers which use
// room <= 12 as a chokepoint heuristic keep the same scale.
void GridRoom::initialize()
{
    width = 4 * BWAPI::Broodwar->mapWidth();
    height = 4 * BWAPI::Broodwar->mapHeight();
    grid = std::vector< std::vector<short> >(width, std::vector<short>(height, short(0)));

    typedef std::vector< std::vector<unsigned short> > SpanGrid;
    SpanGrid left(width, std::vector<unsigned short>(height, 0));
    SpanGrid right(width, std::vector<unsigned short>(height, 0));
    SpanGrid up(width, std::vector<unsigned short>(height, 0));
    SpanGrid down(width, std::vector<unsigned short>(height, 0));
    SpanGrid nw(width, std::vector<unsigned short>(height, 0));
    SpanGrid se(width, std::vector<unsigned short>(height, 0));
    SpanGrid ne(width, std::vector<unsigned short>(height, 0));
    SpanGrid sw(width, std::vector<unsigned short>(height, 0));

    // Horizontal spans.
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (the.partitions.walkable(x, y))
            {
                left[x][y] = 1 + (x > 0 ? left[x - 1][y] : 0);
            }
        }
        for (int x = width - 1; x >= 0; --x)
        {
            if (the.partitions.walkable(x, y))
            {
                right[x][y] = 1 + (x + 1 < width ? right[x + 1][y] : 0);
            }
        }
    }

    // Vertical spans.
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (the.partitions.walkable(x, y))
            {
                up[x][y] = 1 + (y > 0 ? up[x][y - 1] : 0);
            }
        }
        for (int y = height - 1; y >= 0; --y)
        {
            if (the.partitions.walkable(x, y))
            {
                down[x][y] = 1 + (y + 1 < height ? down[x][y + 1] : 0);
            }
        }
    }

    // NW <-> SE diagonal spans.
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (the.partitions.walkable(x, y))
            {
                nw[x][y] = 1 + (x > 0 && y > 0 ? nw[x - 1][y - 1] : 0);
            }
        }
    }
    for (int x = width - 1; x >= 0; --x)
    {
        for (int y = height - 1; y >= 0; --y)
        {
            if (the.partitions.walkable(x, y))
            {
                se[x][y] = 1 + (x + 1 < width && y + 1 < height ? se[x + 1][y + 1] : 0);
            }
        }
    }

    // NE <-> SW diagonal spans. These require the opposite x traversal order.
    for (int x = width - 1; x >= 0; --x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (the.partitions.walkable(x, y))
            {
                ne[x][y] = 1 + (x + 1 < width && y > 0 ? ne[x + 1][y - 1] : 0);
            }
        }
    }
    for (int x = 0; x < width; ++x)
    {
        for (int y = height - 1; y >= 0; --y)
        {
            if (the.partitions.walkable(x, y))
            {
                sw[x][y] = 1 + (x > 0 && y + 1 < height ? sw[x - 1][y + 1] : 0);
            }
        }
    }

    // Combine the four cross-sections. The distance-to-wall inset remains an
    // additional sanity bound: it prevents an isolated long line next to a wall
    // from being mistaken for roomy terrain.
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (!the.partitions.walkable(x, y))
            {
                grid[x][y] = 0;
                continue;
            }

            const int horizontal = int(left[x][y]) + int(right[x][y]) - 1;
            const int vertical = int(up[x][y]) + int(down[x][y]) - 1;
            const int diagonalA = int(nw[x][y]) + int(se[x][y]) - 1;
            const int diagonalB = int(ne[x][y]) + int(sw[x][y]) - 1;

            int room = std::min(std::min(horizontal, vertical), std::min(diagonalA, diagonalB));

            // The inset supplies an independent distance-to-wall bound. It
            // keeps edge pockets and diagonal corners from looking wider than
            // a unit can really use.
            const int insetDiameter = std::max(1, 2 * the.inset.at(x, y) - 1);
            room = std::min(room, insetDiameter);

            grid[x][y] = short(std::min(room, 32767));
        }
    }
}

void GridRoom::draw() const
{
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            int d = grid[x][y];
            if (d > 0)
            {
                BWAPI::Broodwar->drawTextMap(
                    BWAPI::Position(BWAPI::WalkPosition(x, y)) + BWAPI::Position(1, 3),
                    "%d", d);
            }
        }
    }
}
