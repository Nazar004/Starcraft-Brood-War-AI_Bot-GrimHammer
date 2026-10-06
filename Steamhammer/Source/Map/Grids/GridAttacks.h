#pragma once

#include "BWAPI.h"
#include "Map/Grids/Grid.h"
#include "Intelligence/UnitData.h"

namespace UAlbertaBot
{
class GridAttacks : public Grid
{
public:

    enum class Selection { GroundFixed, AirFixed, GroundMobile, AirMobile, GroundInfluence, AirInfluence };

private:

    const Selection _selection;

    void addTilesInRange(const BWAPI::Position & enemy, int range);

    void computeGroundFixed(const std::map<BWAPI::Unit, UnitInfo> & unitsInfo);
    void computeAirFixed(const std::map<BWAPI::Unit, UnitInfo> & unitsInfo);
    void computeGroundMobile(const std::map<BWAPI::Unit, UnitInfo> & unitsInfo);
    void computeAirMobile(const std::map<BWAPI::Unit, UnitInfo> & unitsInfo);
	void computeGroundInfluence(const std::map<BWAPI::Unit, UnitInfo> & unitsInfo);
	void computeAirInfluence(const std::map<BWAPI::Unit, UnitInfo> & unitsInfo);

public:

    GridAttacks(Selection selection);

    void update();			// bring the grid up to date

    bool inRange(const BWAPI::TilePosition & pos) const;
    bool inRange(const BWAPI::TilePosition & topLeft, const BWAPI::TilePosition & bottomRight) const;
    bool inRange(BWAPI::UnitType buildingType, const BWAPI::TilePosition & topLeftTile) const;
    bool inRange(BWAPI::Unit unit) const;

    bool safeToVisit(BWAPI::Unit unit) const;
};

class GroundAttacksFixed : public GridAttacks
{
public:
    GroundAttacksFixed();
};

class AirAttacksFixed : public GridAttacks
{
public:
    AirAttacksFixed();
};

class GroundAttacksMobile : public GridAttacks
{
public:
    GroundAttacksMobile();
};

class AirAttacksMobile : public GridAttacks
{
public:
    AirAttacksMobile();
};

class GroundInfluence : public GridAttacks
{
public:
	GroundInfluence();
};

class AirInfluence : public GridAttacks
{
public:
	AirInfluence();
};

}