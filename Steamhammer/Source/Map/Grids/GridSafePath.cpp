#include "Map/Grids/GridSafePath.h"

#include "Map/Bases.h"
#include "Map/Grids/GridAttacks.h"
#include "Map/MapTools.h"
#include "Core/The.h"

using namespace UAlbertaBot;

// Safe paths, with no known danger of attack.
// It is a grid of tiles, each tile giving a distance from the starting point
// or -1 if there is no safe path from the starting tile to there.

// NOTE If the starting tile can be attacked, there is no safe path from it!

// Created at the start of the game, when everywhere is safe.
// The initial values are from the ground distances grid which doesn't consider safety.
GridSafePath::GridSafePath(const BWAPI::TilePosition & start, const Grid & distances)
    : Grid(distances)
	, _start(start)
	, _updateFrame(-1)
{
}

// Created at the start of the game, when everywhere is safe.
// The initial values are air distances computed here.
GridSafePath::GridSafePath(const BWAPI::TilePosition & start)
	: Grid(BWAPI::Broodwar->mapWidth(), BWAPI::Broodwar->mapHeight(), 0)
	, _start(start)
	, _updateFrame(-1)
{
	BWAPI::Position origin = BWAPI::Position(start);

	for (int x = 0; x < width; ++x)
	{
		for (int y = 0; y < height; ++y)
		{
			grid[x][y] = origin.getApproxDistance(BWAPI::Position(BWAPI::TilePosition(x, y))) / 32;
		}
	}
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

GridSafePathGround::GridSafePathGround(const BWAPI::TilePosition & start, const Grid & distances)
    : GridSafePath(start, distances)
{
}

void GridSafePathGround::resetFromDistances(const Grid & distances)
{
    width = BWAPI::Broodwar->mapWidth();
    height = BWAPI::Broodwar->mapHeight();
    grid = std::vector< std::vector<short> >(
        width, std::vector<short>(height, short(-1)));

    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            grid[x][y] = short(distances.at(x, y));
        }
    }
    _updateFrame = -1;
}

// `influence` tells what tiles are near mobile enemies that might move to get in range.
void GridSafePathGround::update(GridAttacks * influence)
{
	UAB_ASSERT(_start.isValid() && influence, "uninitialized");

    const size_t LegalActions = 4;
    const int actionX[LegalActions] = { 1, -1, 0, 0 };
    const int actionY[LegalActions] = { 0, 0, 1, -1 };

    // Erase the grid to -1, which means "can't go there".
    for (int x = 0; x < width; ++x)
    {
        std::fill(grid[x].begin(), grid[x].end(), -1);
    }

    if (!_start.isValid() || !the.map.isWalkable(_start) ||
        the.groundHitsFixed.at(_start) + influence->at(_start) != 0)
    {
        // The starting tile is not safe. Can't safely go anywhere, so we're done.
        _updateFrame = the.now();
        return;
    }
    grid[_start.x][_start.y] = 0;

    std::vector<BWAPI::TilePosition> fringe;
    fringe.reserve(width * height);
    fringe.push_back(_start);

    for (size_t fringeIndex=0; fringeIndex<fringe.size(); ++fringeIndex)
    {
        const BWAPI::TilePosition & tile = fringe[fringeIndex];

        int currentDist = 1 + grid[tile.x][tile.y];

        // The legal actions define which tiles are nearest neighbors of this one.
        for (size_t a=0; a<LegalActions; ++a)
        {
            BWAPI::TilePosition nextTile(tile.x + actionX[a], tile.y + actionY[a]);

            if (nextTile.isValid() &&
                grid[nextTile.x][nextTile.y] == -1 &&
                the.map.isWalkable(nextTile) &&
                the.groundHitsFixed.at(nextTile) == 0 &&
				influence->at(nextTile) == 0)
            {
                fringe.push_back(nextTile);
                grid[nextTile.x][nextTile.y] = currentDist;
            }
        }
    }

    _updateFrame = the.now();
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

GridSafePathAir::GridSafePathAir(const BWAPI::TilePosition & start)
    : GridSafePath(start)
{
}

// `influence` tells what tiles are near mobile enemies that might move to get in range.
void GridSafePathAir::update(GridAttacks * influence)
{
	UAB_ASSERT(_start.isValid() && influence, "uninitialized");
	const auto& fixedThreats = the.airHitsFixed.rawGrid();
	const auto& mobileThreats = influence->rawGrid();

	const size_t LegalActions = 4;
	const int actionX[LegalActions] = { 1, -1, 0, 0 };
	const int actionY[LegalActions] = { 0, 0, 1, -1 };

	// Erase the grid to -1, which means "can't go there".
	for (int x = 0; x < width; ++x)
	{
		std::fill(grid[x].begin(), grid[x].end(), -1);
	}

	if (!_start.isValid() ||
		fixedThreats[_start.x][_start.y] +
		mobileThreats[_start.x][_start.y] != 0)
	{
		// The starting tile is not safe. Can't safely go anywhere, so we're done.
		_updateFrame = the.now();
		return;
	}
	grid[_start.x][_start.y] = 0;

	std::vector<int> fringe;
	fringe.reserve(width * height);
	fringe.push_back(_start.x * height + _start.y);

	for (size_t fringeIndex = 0; fringeIndex < fringe.size(); ++fringeIndex)
	{
		const int tileIndex = fringe[fringeIndex];
		const int x = tileIndex / height;
		const int y = tileIndex % height;
		const int currentDist = 1 + grid[x][y];

		// The legal actions define which tiles are nearest neighbors of this one.
		for (size_t a = 0; a < LegalActions; ++a)
		{
			const int nextX = x + actionX[a];
			const int nextY = y + actionY[a];

			if (nextX >= 0 && nextX < width &&
				nextY >= 0 && nextY < height &&
				grid[nextX][nextY] == -1 &&
				fixedThreats[nextX][nextY] == 0 &&
				mobileThreats[nextX][nextY] == 0)
			{
				fringe.push_back(nextX * height + nextY);
				grid[nextX][nextY] = currentDist;
			}
		}
	}

	_updateFrame = the.now();
}
