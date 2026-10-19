#include "Map/Grids/GridAirDistances.h"

#include "Core/Common.h"
#include "Utilities/UABAssert.h"

using namespace UAlbertaBot;

// Create an empty, unitialized, unusable grid.
// Necessary if a Grid subclass is created before BWAPI is initialized.
GridAirDistances::GridAirDistances()
    : Grid()
{
}

// Create an initialized grid, given the starting point.
GridAirDistances::GridAirDistances(const BWAPI::TilePosition & start)
    : Grid(0, 0, 0)
	, _start(TileCenter(start))
{
}

int GridAirDistances::at(int x, int y) const
{
    return at(BWAPI::TilePosition(BWAPI::Position(x, y)));
}

int GridAirDistances::at(const BWAPI::TilePosition & pos) const
{
    return _start.getApproxDistance(TileCenter(pos));
}

int GridAirDistances::at(const BWAPI::WalkPosition & pos) const
{
    return at(BWAPI::TilePosition(pos));
}

int GridAirDistances::at(const BWAPI::Position & pos) const
{
    return at(BWAPI::TilePosition(pos));
}

int GridAirDistances::at(BWAPI::Unit unit) const
{
    return at(unit->getTilePosition());
}

void GridAirDistances::draw() const
{
	for (int x = 0; x < BWAPI::Broodwar->mapWidth(); ++x)
	{
		for (int y = 0; y < BWAPI::Broodwar->mapHeight(); ++y)
		{
			BWAPI::TilePosition tile = BWAPI::TilePosition(x, y);
			BWAPI::Broodwar->drawTextMap(BWAPI::Position(tile) + BWAPI::Position(5, 10), "%d", at(tile));
		}
	}
}
