#include "Map/Grids/GridInset.h"

#include "Core/The.h"
#include "Utilities/UABAssert.h"

#include <algorithm>
#include <cstdlib>

using namespace UAlbertaBot;

const size_t LegalActions = 4;
const int actionX[LegalActions] = { 1, -1, 0, 0 };
const int actionY[LegalActions] = { 0, 0, 1, -1 };

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// Create an empty, unitialized, unusable grid.
// Necessary if a Grid subclass is created before BWAPI is initialized.
GridInset::GridInset()
    : GridWalk()
{
}

// This depends on the.partitions already being initialized.
//
// Use an 8-neighbor chamfer distance transform instead of the old 4-neighbor
// Manhattan flood. Orthogonal steps cost 10, diagonals 14, then the result is
// converted back to approximate walk-tile units. The old version overstated
// clearance along diagonal walls, which in turn distorted choke width.
void GridInset::initialize()
{
    width = 4 * BWAPI::Broodwar->mapWidth();
    height = 4 * BWAPI::Broodwar->mapHeight();

    const int INF = 30000;
    std::vector< std::vector<int> > distance(
        width, std::vector<int>(height, INF));

    // Unwalkable cells are the zero-distance obstacle set. A walkable cell on
    // the map border is one walk tile away from the outside wall.
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (!the.partitions.walkable(x, y))
            {
                distance[x][y] = 0;
            }
            else if (x == 0 || y == 0 || x == width - 1 || y == height - 1)
            {
                distance[x][y] = 10;
            }
        }
    }

    // Forward chamfer pass.
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (!the.partitions.walkable(x, y))
            {
                continue;
            }

            int best = distance[x][y];
            if (x > 0) best = std::min(best, distance[x - 1][y] + 10);
            if (y > 0) best = std::min(best, distance[x][y - 1] + 10);
            if (x > 0 && y > 0) best = std::min(best, distance[x - 1][y - 1] + 14);
            if (x + 1 < width && y > 0) best = std::min(best, distance[x + 1][y - 1] + 14);
            distance[x][y] = best;
        }
    }

    // Backward chamfer pass.
    for (int y = height - 1; y >= 0; --y)
    {
        for (int x = width - 1; x >= 0; --x)
        {
            if (!the.partitions.walkable(x, y))
            {
                continue;
            }

            int best = distance[x][y];
            if (x + 1 < width) best = std::min(best, distance[x + 1][y] + 10);
            if (y + 1 < height) best = std::min(best, distance[x][y + 1] + 10);
            if (x + 1 < width && y + 1 < height) best = std::min(best, distance[x + 1][y + 1] + 14);
            if (x > 0 && y + 1 < height) best = std::min(best, distance[x - 1][y + 1] + 14);
            distance[x][y] = best;
        }
    }

    grid = std::vector< std::vector<short> >(
        width, std::vector<short>(height, short(0)));

    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (!the.partitions.walkable(x, y))
            {
                grid[x][y] = 0;
            }
            else
            {
                // Rounded chamfer distance. Adjacent orthogonal/diagonal cells
                // both remain inset 1; progressively wider spaces rise smoothly.
                grid[x][y] = short(std::max(1, (distance[x][y] + 5) / 10));
            }
        }
    }
}

// Try to find a position near the start with the given inset.
// Return BWAPI::Positions::None on failure.
// NOTE Currently unused, potentially useful.
BWAPI::Position GridInset::find(const BWAPI::Position & start, int inset)
{
    if (!start.isValid() || at(start) <= 0)
    {
        return BWAPI::Positions::None;
    }

    const int zoneID = the.zone.at(start);

    BWAPI::WalkPosition here(start);
    while (at(here) != inset)
    {
        bool gotOne = false;
        for (size_t a = 0; a < LegalActions; ++a)
        {
            BWAPI::WalkPosition next(here.x + actionX[a], here.y + actionY[a]);

            if (next.isValid() && at(next) > 0 && the.zone.at(next) == zoneID &&
                abs(at(next) - inset) < abs(at(here) - inset))
            {
                here = next;
                gotOne = true;
                break;
            }
        }
        if (!gotOne)
        {
            break;
        }
    }

    if (at(here) == inset)
    {
        return BWAPI::Position(here);
    }

    // None found. We might have 1. run off the edge, or 2. be adrift in a lake, unable to get closer.
    return BWAPI::Positions::None;
}

// Draw the edge ranges as colored contours of dots.
void GridInset::draw() const
{
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            int d = grid[x][y];
            BWAPI::Color color = BWAPI::Colors::Black;
            if (d > 0)
            {
                switch (d % 8)
                {
                case 0: color = BWAPI::Colors::Grey;   break;
                case 1: color = BWAPI::Colors::Brown;  break;
                case 2: color = BWAPI::Colors::Purple; break;
                case 3: color = BWAPI::Colors::Blue;   break;
                case 4: color = BWAPI::Colors::Green;  break;
                case 5: color = BWAPI::Colors::Orange; break;
                case 6: color = BWAPI::Colors::Yellow; break;
                case 7: color = BWAPI::Colors::White;  break;
                }
                BWAPI::Broodwar->drawCircleMap(
                    BWAPI::Position(BWAPI::WalkPosition(x, y)) + BWAPI::Position(4, 4),
                    2, color);
            }
        }
    }
}