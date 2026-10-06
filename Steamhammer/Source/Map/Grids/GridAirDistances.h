#pragma once

#include "Map/Grids/Grid.h"

// A class that calculates air distances from a tile position.
// It's a grid, but it stores no data.

namespace UAlbertaBot
{
class GridAirDistances : public Grid
{
	BWAPI::Position _start;

public:
	GridAirDistances();
	GridAirDistances(const BWAPI::TilePosition & start);
	
	int at(int x, int y) const;
    int at(const BWAPI::TilePosition & pos) const;
    int at(const BWAPI::WalkPosition & pos) const;
    int at(const BWAPI::Position & pos) const;
    int at(BWAPI::Unit unit) const;

	void draw() const;
};
}