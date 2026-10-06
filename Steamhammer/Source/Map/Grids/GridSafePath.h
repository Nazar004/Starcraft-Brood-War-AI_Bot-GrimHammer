#pragma once

#include <vector>
#include "BWAPI.h"
#include "Map/Grids/Grid.h"

// Safe paths, with no known danger of attack, for ground and air.

namespace UAlbertaBot
{
class GridAttacks;		// forward declaration

class GridSafePath : public Grid
{
protected:
    BWAPI::TilePosition _start;
    int _updateFrame;

public:
    GridSafePath(const BWAPI::TilePosition & start, const Grid & distances);
	GridSafePath(const BWAPI::TilePosition & start);

	bool isInitialized() const { return _updateFrame >= 0; };
    int getUpdateFrame() const { return _updateFrame; };
    virtual void update(GridAttacks * influence) = 0;
};

class GridSafePathGround : public GridSafePath
{
public:
    GridSafePathGround(const BWAPI::TilePosition & start, const Grid & distances);

    // Replace the topology-only baseline after a neutral blocker changes the
    // ground map. Threat-aware tasks can then update it normally afterwards.
    void resetFromDistances(const Grid & distances);
    void update(GridAttacks * influence);
};

class GridSafePathAir : public GridSafePath
{
public:
    GridSafePathAir(const BWAPI::TilePosition & start);

    void update(GridAttacks * influence);
};

}
