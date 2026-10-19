#pragma once

// Plan and construct Terran bunkers and missile turrets.

#include <BWAPI.h>
#include <vector>

namespace UAlbertaBot
{

// Forward declaration.
class Base;


struct StaticDefensePlan
{
	// Our ground defenses desired.
    // Count of bunkers at different sets of places.
    // An "inner base" is one the enemy cannot reach by ground without passing an outer base.
    int atInnerBases;
    int atOuterBases;
    int atFront;           // front line only (a specific outer base)
    //int atProxy;           // proxy location NOT IMPLEMENTED

	// Our air defenses desired.
	// Count of missile turrets.
    // If airPerBase is true, then antiAir gives the count wanted for each base.
    // If false, the total count, one at each of this many bases. It can be only 0 to 3.
    bool airIsPerBase;
    int antiAir;

    StaticDefensePlan();
};

class StaticDefense
{
    StaticDefensePlan _plan;
    BWAPI::UnitType _ground;
    BWAPI::UnitType _groundPrereq;
    BWAPI::UnitType _air;
    BWAPI::UnitType _airPrereq;
    std::vector<Base *> _airBases;


    void planTerran();


    void plan();

    void startBuilding(BWAPI::UnitType building, const BWAPI::TilePosition & tile);
    bool startFrontGroundBuildings();
    bool startOtherGroundBuildings();
    bool buildGround();

    void chooseAirBases();
    void startAirBuilding();
    void buildAir();

    bool alreadyBuilding(BWAPI::UnitType type);
    bool needPrerequisite(BWAPI::UnitType prereq);
    void build();

    void drawGroundCount(Base * base, int count) const;
    void drawAirCount(Base * base, int count) const;
    void draw() const;

public:

    StaticDefense();

    void update();
};

};
