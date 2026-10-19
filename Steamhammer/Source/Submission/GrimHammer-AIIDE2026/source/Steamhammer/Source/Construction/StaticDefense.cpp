#include "Construction/StaticDefense.h"

#include "Map/Bases.h"
#include "Construction/BuildingManager.h"
#include "Production/MacroAct.h"
#include "Intelligence/Learning/OpponentModel.h"
#include "Production/ProductionManager.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"

using namespace UAlbertaBot;

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

StaticDefensePlan::StaticDefensePlan()
    : atInnerBases(0)
    , atOuterBases(0)
    , atFront(0)
    //, atProxy(0)
    , airIsPerBase(true)
    , antiAir(0)
{
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// -- -- -- -- -- -- -- -- -- -- --
// Private methods.

// The defense plan has been filled in. Make sure it doesn't call for excess defense.
// This tries to limit defenses to what the economy can support, as a separate task
// to simplify the overall code.


// Analyze static defense needs for TvX and PvX.
void StaticDefense::planTerran()
{
    // -- ground defenses --

    _plan.atInnerBases = 0;
    _plan.atOuterBases = 0;
    _plan.atFront = 0;
    //_plan.atProxy = 0;

    // Terran does not make any ground defenses unless the opening build calls for it.
    // NOTE It doesn't know how to land a floating barracks, so a bunker is not always useful.
    // Protoss defends only against vulture raids.
    // NOTE Air defenses help against some ground attacks, though.
    

    // -- air defenses --

    _plan.airIsPerBase = true;

    // NOTE Zerg drop tech is not as easy to detect.
    bool enemyHasDropTech =
        the.your.seen.count(BWAPI::UnitTypes::Terran_Dropship) + the.your.seen.count(BWAPI::UnitTypes::Protoss_Shuttle) > 0;

    int minAir = 0;
    if (enemyHasDropTech)
    {
        minAir = 2;
    }
    else if (the.info.enemyHasCloakTech())
    {
        minAir = 1;
    }

    // Count enemy air to ground power.
    int enemyAir =
        the.your.seen.count(BWAPI::UnitTypes::Terran_Wraith) +
        8 * the.your.seen.count(BWAPI::UnitTypes::Terran_Battlecruiser) +

        2 * the.your.seen.count(BWAPI::UnitTypes::Protoss_Scout) +
        4 * the.your.seen.count(BWAPI::UnitTypes::Protoss_Arbiter) +
        6 * the.your.seen.count(BWAPI::UnitTypes::Protoss_Carrier) +

        2 * the.your.seen.count(BWAPI::UnitTypes::Zerg_Mutalisk) +
        6 * the.your.seen.count(BWAPI::UnitTypes::Zerg_Guardian);

    // Count my mobile anti-air power. Skip spellcasters.
    int myAntiAir =
        the.my.all.count(BWAPI::UnitTypes::Terran_Marine) +
        6 * the.my.all.count(BWAPI::UnitTypes::Terran_Goliath) +
        the.my.all.count(BWAPI::UnitTypes::Terran_Wraith) +
        4 * the.my.all.count(BWAPI::UnitTypes::Terran_Valkyrie) +
        6 * the.my.all.count(BWAPI::UnitTypes::Terran_Battlecruiser);

    int antiAir = std::max(minAir, (enemyAir - myAntiAir) / 4);

     // terran
    {
        _plan.antiAir = antiAir;
    }
}

// How many sunks do we need in total?


// How many sunks do we need in total?


// How many sunks do we need in total?
// Account for likely midgame units only. Other stuff can take care of itself.


// Analyze static defense needs for ZvT ZvP ZvZ.


// Analyze the situation and decide what static defense is needed where.
void StaticDefense::plan()
{
    if (the.enemyRace() == BWAPI::Races::Unknown)
    {
        // No defense needed until we've seen some enemy units.
        return;
    }

    
    {
        planTerran();
    }
}

void StaticDefense::startBuilding(BWAPI::UnitType building, const BWAPI::TilePosition & tile)
{
    BuildOrderQueue & queue = the.production.getQueue();

    // Replace zerg drones if low.
    

    queue.queueAsHighestPriority(MacroAct(building, tile));
}

// The building manager sometimes forgets about sunkens and spores while building them, and
// leaves unmorphed creep colonies. If we still want to the sunk or spore, morph it.
// For spores, this only works when _plan.airIsPerBase is true.


// Urgently start all ground defenses needed at the front base.
bool StaticDefense::startFrontGroundBuildings()
{
    Base * front = the.bases.myFront();

    if (!front || !front->isMyCompletedBase())
    {
        return false;
    }

    if (front->isDoomed())
    {
        return false;
    }

    int nActual = front->getNumUnits(_ground);
    
    // Also count queued buildings, without checking where they are set to be built.
    nActual += the.production.getQueue().numInNextN(_ground, 2 * _plan.atFront);

    int nNeeded = _plan.atFront - nActual;      // new buildings to start
    if (nNeeded <= 0)
    {
        return false;
    }

    //BWAPI::Broodwar->printf("%d needed at front: plan %d - existing %d", nNeeded, _plan.atFront, nActual);

    while (nNeeded > 0)
    {
        startBuilding(_ground, front->getFrontTile());
        --nNeeded;
    }

    return true;
}

// Take our time starting all ground defenses needed at bases other than the front base.
// Don't build more at a base than there are workers there, so that new bases don't overdo it.
bool StaticDefense::startOtherGroundBuildings()
{
    for (Base * base : the.bases.getAll())
    {
        if (base->isMyCompletedBase() &&
                        !base->isDoomed())
        {
            int nUnits = base->getNumUnits(_ground);
            if (base == the.bases.myFront())
            {
                // Handled by startFrontGroundBuildings().
            }
            else if (!base->isInnerBase())  // outer base
            {
                if (nUnits < _plan.atOuterBases && nUnits < base->getNumWorkers())
                {
                    startBuilding(_ground, base->getTilePosition());
                    return true;
                }
            }
            else if (base->isInnerBase())
            {
                if (nUnits < _plan.atInnerBases && nUnits < base->getNumWorkers())
                {
                    startBuilding(_ground, base->getTilePosition());
                    return true;
                }
            }
        }
    }

    return false;
}

bool StaticDefense::buildGround()
{
    if (_plan.atInnerBases + _plan.atInnerBases + _plan.atFront == 0)
    {
        return false;
    }

    // Build barracks/forge/pool if needed.
    // Protoss normally already has a pylon, created by a different mechanism.
    if (needPrerequisite(_groundPrereq)) 
    {
        return true;
    }

    // Morph creep colonies that the building manager forgot about, if needed.
    

    // Build defenses on the front line as fast as possible.
    if (startFrontGroundBuildings())
    {
        return true;
    }

    // Build defenses elsewhere more slowly.
    if (alreadyBuilding(_ground))
    {
        return true;
    }

    return startOtherGroundBuildings();
}

// We're placing one air defense building at a each of limited number of bases. Choose them.
// In practice, only zerg does this, to provide safe havens for overlords.
// First anti-air: vT and vZ, closest base to enemy; vP, natural.
// Second anti-air in the remaining base of main/natural.
// Any remaining in remaining bases.
void StaticDefense::chooseAirBases()
{
    // Figure out the first two bases.
    const bool hasNatural =
        the.bases.myNatural() &&
        the.bases.myNatural()->isMyCompletedBase();
    Base * base1;
    Base * base2;
    if (the.enemyRace() == BWAPI::Races::Protoss)
    {
        // Natural first vP, to help defend against dark templar.
        base1 = hasNatural ? the.bases.myNatural() : the.bases.myMain();
        base2 =  hasNatural ? the.bases.myMain() : nullptr;
    }
    else // vT or vZ
    {
        base1 = the.bases.myMain();
        base2 = hasNatural ? the.bases.myNatural() : nullptr;

        Base * enemyStart = the.bases.enemyStart();
        if (hasNatural && enemyStart)
        {
            // We know where the enemy is. Put the first in the natural if it is significantly closer.
            int mainDist = the.bases.myMain()->getCenter().getApproxDistance(enemyStart->getPosition());
            int natDist = the.bases.myNatural()->getCenter().getApproxDistance(enemyStart->getPosition());
            if (natDist + 8 * 32 < mainDist)
            {
                base1 = the.bases.myNatural();
                base2 = the.bases.myMain();
            }
        }
    }

    // Fill in _airBases.
    _airBases.clear();
    _airBases.push_back(base1);
    if (base2 && _plan.antiAir >= 2)
    {
        _airBases.push_back(base2);
    }

    if (_plan.antiAir > 2)
    {
        int remaining = _plan.antiAir - 2;
        for (Base * base : the.bases.getAll())
        {
            if (base->isMyCompletedBase() && base != base1 && base != base2)
            {
                _airBases.push_back(base);
                --remaining;
                if (remaining <= 0)
                {
                    break;
                }
            }
        }
    }
}

// All checks passed. Find the next base that needs it, and start the building.
void StaticDefense::startAirBuilding()
{
    const std::vector<Base *> & bases =
        _plan.airIsPerBase ? the.bases.getAll() : _airBases;
    
    for (Base * base : bases)
    {
        if (base->isMyCompletedBase() && base->getNumUnits(_air) < (_plan.airIsPerBase ? _plan.antiAir : 1))
        {
            
            {
                startBuilding(_air, base->getMineralLineTile());
                return;
            }
        }
    }
}

// Order terran turrets or zerg spore colonies.
// Protoss doesn't use this, because cannons are general-purpose.
void StaticDefense::buildAir()
{
    if (_plan.antiAir <= 0)
    {
        return;
    }

    // Build ebay/evo if needed.
    if (_plan.antiAir > 0 && needPrerequisite(_airPrereq))
    {
        return;
    }

    // Morph creep colonies that the building manager forgot about, if needed.
    

    // We're not in a big hurry. Wait for the last one to finish.
    if (alreadyBuilding(_air))
    {
        return;
    }

    if (!_plan.airIsPerBase)
    {
        chooseAirBases();
    }

    startAirBuilding();
}

bool StaticDefense::alreadyBuilding(BWAPI::UnitType type)
{
    return
        UnitUtil::GetUncompletedUnitCount(type) > 0 ||
        BuildingManager::Instance().isBeingBuilt(type) ||
        the.production.getQueue().anyInNextN(type, 2);
}

// Add a preprequisite tech buliding, if necessary.
// Return true iff the prerequisite is not available.
bool StaticDefense::needPrerequisite(BWAPI::UnitType prereq)
{
    if (!prereq.isValid())
    {
        return false;
    }

    if (the.my.completed.count(prereq) > 0)
    {
        return false;
    }

    // If we're very low on workers, defense tech will not save us no matter how much we need it.
    if (the.my.completed.countWorkers() <= 5)
    {
        return true;
    }

    if (the.my.all.count(prereq) > 0 ||
        BuildingManager::Instance().isBeingBuilt(prereq) ||
        the.production.getQueue().anyInNextN(prereq, 2))
    {
        // It's building.
        // Build time of a creep colony is 300 frames. If the prereq will finish by then, it's OK.
        

        return true;
    }

    the.production.getQueue().queueAsHighestPriority(prereq);
    return true;
}

// Possibly add static defenses at various bases.
void StaticDefense::build()
{
    if (!buildGround())
    {
        buildAir();
    }
}

void StaticDefense::drawGroundCount(Base * base, int count) const
{
    if (count == 0)
    {
        return;
    }

    std::string building;
    
    {
        building = "bunker";
    }

    BWAPI::Position xy = base->getFront();
    BWAPI::Broodwar->drawTextMap(xy+BWAPI::Position(-26, -6), "%c%d x %s", yellow, count, building.c_str());
    BWAPI::Broodwar->drawCircleMap(xy, 32, BWAPI::Colors::Orange);
}

void StaticDefense::drawAirCount(Base * base, int count) const
{
    if (count == 0)
    {
        return;
    }

    std::string building;
    
    {
        building = "turret";
    }

    BWAPI::Position xy = base->getPosition();
    BWAPI::Broodwar->drawTextMap(xy+BWAPI::Position(-26, -6), "%c%d x %s", yellow, count, building.c_str());
    BWAPI::Broodwar->drawCircleMap(xy, 32, BWAPI::Colors::Orange);
}

void StaticDefense::draw() const
{
    if (!Config::Debug::DrawStaticDefensePlan)
    {
        return;
    }

    for (Base * base : the.bases.getAll())
    {
        if (base->getOwner() == the.self())
        {
            if (base == the.bases.myFront() && _plan.atFront > _plan.atOuterBases)
            {
                drawGroundCount(base, _plan.atFront);
            }
            else if (base->isInnerBase())
            {
                drawGroundCount(base, _plan.atInnerBases);
            }
            else // outer base
            {
                drawGroundCount(base, _plan.atOuterBases);
            }

            if (_plan.airIsPerBase)
            {
                drawAirCount(base, _plan.antiAir);
            }
        }
    }

    if (!_plan.airIsPerBase)
    {
        for (Base * base : _airBases)
        {
            if (base->getOwner() == the.self())
            {
                drawAirCount(base, _plan.antiAir);
            }
        }
    }
}

// -- -- -- -- -- -- -- -- -- -- --
// Public methods.

StaticDefense::StaticDefense()
{
    _ground = BWAPI::UnitTypes::Terran_Bunker;
    _groundPrereq = BWAPI::UnitTypes::Terran_Barracks;
    _air = BWAPI::UnitTypes::Terran_Missile_Turret;
    _airPrereq = BWAPI::UnitTypes::Terran_Engineering_Bay;
}

void StaticDefense::update()
{
    if (!Config::Skills::StaticDefense)
    {
        return;
    }

    int phase = the.now() % 19;

    if (phase == 1)
    {
        plan();
    }
    else if (phase == 2)
    {
        build();
    }

    draw();
}
