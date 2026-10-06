#include "Map/Bases.h"

#include "Map/MapTools.h"
#include "Intelligence/InformationManager.h"
#include "Core/The.h"
#include <climits>
#include <cstdlib>

using namespace UAlbertaBot;

// These numbers are in tiles.
const int BaseResourceRange = 22;   // max distance of one resource from another
const int BasePositionRange = 15;   // max distance of the base location from the start point
const int DepotTileWidth = 4;
const int DepotTileHeight = 3;

// Each base much meet at least one of these minimum limits to be worth keeping.
const int MinTotalMinerals = 500;
const int MinTotalGas = 500;

// Most initialization happens in initialize() below.
Bases::Bases()
    // These are set to their final values in initialize().
    : startingBase(nullptr)
    , mainBase(nullptr)
    , naturalBase(nullptr)
	, enemyStartingBase(nullptr)
    , enemyStartingBaseConfirmed(false)
    , islandStart(false)
    , islandBases(false)
{
}


bool Bases::startLocationScouted(const Base * base) const
{
    return base && base->isAStartingBase() &&
        explicitlyScoutedStartingBaseIDs.find(base->getID()) !=
            explicitlyScoutedStartingBaseIDs.end();
}

// For sorting bases in lexicographic order by y, x coordinates.
// No two bases are in the same location, so there are no ties.
bool compareBases(const Base * a, const Base * b)
{
    return
        a->getTilePosition().y < b->getTilePosition().y ||
        a->getTilePosition().y == b->getTilePosition().y && a->getTilePosition().x < b->getTilePosition().x;
}

// Give each base its ID number.
// NOTE We write base IDs into opponent model files, so base IDs for a map need to be
//      stable across runs. That's why we sort them first.
void Bases::setBaseIDs()
{
    std::sort(bases.begin(), bases.end(), compareBases);

    // Set the IDs.
    int id = 1;
    for (Base * base : bases)
    {
        base->setID(id);
        ++id;
    }
}

// Check whether we are starting on an island (or semi-island).
// Returns true if no other start position is reachable from ours.
bool Bases::checkIslandStart() const
{
	for (Base * base : startingBases)
	{
		if (base != startingBase && connectedToStart(base->getTilePosition()))
		{
			return false;
		}
	}

	return true;
}

// Check whether the map has islands at all.
bool Bases::checkIslandBases() const
{
    for (Base * base : bases)
    {
        if (!connectedToStart(base->getTilePosition()))
        {
            return true;
        }
    }
    return false;
}

// Each base finds and remembers a set of blockers, neutral units that should be destroyed
// to make best use of the base. Here we create the reverse map blockers -> bases.
void Bases::rememberBaseBlockers()
{
    for (Base * base : bases)
    {
        for (BWAPI::Unit blocker : base->getBlockers())
        {
            baseBlockers[blocker] = base;
        }
    }
}

// Given a set of resources, remove any that are used in the base.
void Bases::removeUsedResources(BWAPI::Unitset & resources, const Base * base) const
{
    UAB_ASSERT(base, "bad base");
    for (BWAPI::Unit mins : base->getMinerals())
    {
        resources.erase(mins);
    }
    for (BWAPI::Unit gas : base->getGeysers())
    {
        resources.erase(gas);
    }
}

// Count the initial minerals and gas available from a resource.
void Bases::countResources(BWAPI::Unit resource, int & minerals, int & gas) const
{
    if (resource->getType().isMineralField())
    {
        minerals += resource->getInitialResources();
    }
    else if (resource->getType() == BWAPI::UnitTypes::Resource_Vespene_Geyser)
    {
        gas += resource->getInitialResources();
    }
}

// Given a set of resources (mineral patches and geysers), find a base position nearby if possible.
// This identifies the spot for the resource depot.
// Return BWAPI::TilePositions::Invalid if no acceptable place is found.
// NOTE The found location affects the eventual ID the base is given, and base IDs must be stable
//      across runs because they are written into opponent model files. So a change that affects
//      where bases are placed may invalidate learned data.
BWAPI::TilePosition Bases::findBasePosition(BWAPI::Unitset resources)
{
    UAB_ASSERT(resources.size() > 0, "no resources");

    // Find the bounding box of the resources.
    int left = MAX_DISTANCE;
    int right = 0;
    int top = MAX_DISTANCE;
    int bottom = 0;
    for (BWAPI::Unit resource : resources)
    {
        left = std::min(left, resource->getLeft());
        right = std::max(right, resource->getRight());
        top = std::min(top, resource->getTop());
        bottom = std::max(bottom, resource->getBottom());
    }

    // The geometric center of the bounding box will be our starting point for placing the resource depot.
    BWAPI::TilePosition centerOfResources(BWAPI::Position(left + (right - left) / 2, top + (bottom - top) / 2));

    potentialBases.push_back(PotentialBase(left, right, top, bottom, centerOfResources));

    GridDistances distances(centerOfResources, BasePositionRange, false);

    int bestScore = INT_MAX;               // smallest is best
    BWAPI::TilePosition bestTile = BWAPI::TilePositions::Invalid;

    for (BWAPI::TilePosition tile : distances.getSortedTiles())
    {
        // NOTE Every resource depot is the same size, 4x3 tiles.
        if (the.map.isBuildable(tile, BWAPI::UnitTypes::Protoss_Nexus))    // TODO deprecated call
        {
            int score = baseLocationScore(tile, resources);
            if (score < bestScore)
            {
                bestScore = score;
                bestTile = tile;
            }
        }
    }

    return bestTile;
}

int Bases::baseLocationScore(const BWAPI::TilePosition & tile, BWAPI::Unitset resources) const
{
    int score = 0;

    for (const BWAPI::Unit resource : resources)
    {
        score += tilesBetweenBoxes
            ( tile
            , tile + BWAPI::TilePosition(DepotTileWidth, DepotTileHeight)
            , resource->getInitialTilePosition()
            , resource->getInitialTilePosition() + BWAPI::TilePosition(2, 2)
            );
    }

    return score;
}

// One-dimensional edge-to-edge distance between two tile rectangles.
// Used to figure out distance from a resource depot location to a resource.
int Bases::tilesBetweenBoxes
    ( const BWAPI::TilePosition & topLeftA
    , const BWAPI::TilePosition & bottomRightA
    , const BWAPI::TilePosition & topLeftB
    , const BWAPI::TilePosition & bottomRightB
    ) const
{
    int leftDist = 0;
    int rightDist = 0;

    if (topLeftB.x >= bottomRightA.x)
    {
        leftDist = topLeftB.x - bottomRightA.x;
    }
    else if (topLeftA.x >= bottomRightB.x)
    {
        leftDist = topLeftA.x - bottomRightB.x;
    }

    if (topLeftA.y >= bottomRightB.y)
    {
        rightDist = topLeftA.y - bottomRightB.y;
    }
    else if (topLeftB.y >= bottomRightA.y)
    {
        rightDist = topLeftB.y - bottomRightA.y;
    }

    UAB_ASSERT(leftDist >= 0 && rightDist >= 0, "bad tile distance");
    return std::max(leftDist, rightDist);
}

// The two possible base positions are close enough together
// that we can say they are "the same place" as a base.
bool Bases::closeEnough(BWAPI::TilePosition a, BWAPI::TilePosition b) const
{
    return abs(a.x - b.x) <= 8 && abs(a.y - b.y) <= 8;
}

// If the opponent is zerg and it's early in the game, we may be able to infer the enemy
// base's location by seeing the first overlord.
// NOTE This doesn't quite extract all the information from an overlord sighting. In principle,
//      we might be able to exclude a base location without being sure which remaining base is
//      the enemy base. Steamhammer doesn't provide a way to exclude bases.
bool Bases::inferEnemyBaseFromOverlord()
{
    const int now = the.now();

    if (the.enemy()->getRace() != BWAPI::Races::Zerg || now > 5 * 60 * 24)
    {
        return false;
    }

    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->getType() != BWAPI::UnitTypes::Zerg_Overlord)
        {
            continue;
        }

        // What bases could the overlord be from? Can we narrow it down to 1 possibility?
        Base * possibleEnemyBase = nullptr;
        int countPossibleBases = 0;
        for (Base * base : startingBases)
        {
            if (base == startingBase || startLocationScouted(base))
            {
                // We have explicitly seen the resource-depot footprint and the
                // enemy was not there. Incidental map exploration is not proof.
                continue;
            }

            // Assume the overlord came from this base.
            // Where did the overlord start from? It starts offset from the hatchery in a specific way.
            BWAPI::Position overlordStartPos;
            overlordStartPos.x = base->getPosition().x + ((base->getPosition().x < 32 * BWAPI::Broodwar->mapWidth() / 2) ? +99 : -99);
            overlordStartPos.y = base->getPosition().y + ((base->getPosition().y < 32 * BWAPI::Broodwar->mapHeight() / 2) ? +65 : -65);

            // How far could it have traveled from there?
            double maxDistance = double(now) * (BWAPI::UnitTypes::Zerg_Overlord).topSpeed();
            if (maxDistance >= double(unit->getDistance(overlordStartPos)))
            {
                // It could have started from this base.
                possibleEnemyBase = base;
                ++countPossibleBases;
                if (countPossibleBases > 1)
                {
                    return false;
                }
            }
        }
        if (countPossibleBases == 1)
        {
            // Success.
            if (Config::Debug::DrawScoutInfo)
            {
                BWAPI::Broodwar->printf("Enemy base deduced by overlord sighting");
            }

            enemyStartingBase = possibleEnemyBase;
            enemyStartingBaseConfirmed = false;
            enemyStartingBase->setInferredEnemyBase();
            return true;
        }
    }

    return false;
}

// A starting base is confirmed only by a grounded enemy resource depot on the
// actual start footprint. A broad pixel radius is unsafe on compact maps: a
// natural/forward Command Center can be close enough to a different START to
// make an inferred main look real. Initial depots are placed on the BWAPI start
// tile, so a two-tile tolerance is already generous for observation quirks.
static bool resourceDepotConfirmsStart(BWAPI::Unit depot, const Base * start)
{
    if (!depot || !depot->exists() || !depot->isVisible() || depot->isLifted() ||
        !depot->getType().isResourceDepot() || !start ||
        !depot->getTilePosition().isValid() || !start->getTilePosition().isValid())
    {
        return false;
    }

    const BWAPI::TilePosition depotTile = depot->getTilePosition();
    const BWAPI::TilePosition startTile = start->getTilePosition();
    return std::abs(depotTile.x - startTile.x) <= 2 &&
           std::abs(depotTile.y - startTile.y) <= 2;
}

// If we don't know the enemy's starting base, check to see whether we have just found it.
void Bases::updateEnemyStart()
{
    // Record only explicit observation of the CENTER of a starting depot
    // footprint. BWAPI::isExplored() on one corner is too permissive for
    // elimination scouting and produced false enemy mains on Tau Cross.
    for (Base * start : startingBases)
    {
        if (!start || start == startingBase || !start->getTilePosition().isValid())
        {
            continue;
        }

        const BWAPI::TilePosition topLeft = start->getTilePosition();
        const BWAPI::TilePosition centerA(topLeft.x + 1, topLeft.y + 1);
        const BWAPI::TilePosition centerB(topLeft.x + 2, topLeft.y + 1);

        if (centerA.isValid() && centerB.isValid() &&
            BWAPI::Broodwar->isVisible(centerA) &&
            BWAPI::Broodwar->isVisible(centerB))
        {
            explicitlyScoutedStartingBaseIDs.insert(start->getID());
        }
    }

    if (enemyStartingBase)
    {
        // A directly seen resource depot is historical truth and stays valid
        // after the main is lifted/destroyed. An inferred start is only a
        // hypothesis: if a worker/Comsat later sees its depot footprint empty,
        // revoke it and continue checking the remaining start locations. This
        // matters on maps such as Tau Cross where elimination can be fooled by
        // incidental exploration/pathing around a start.
        if (enemyStartingBaseConfirmed)
        {
            return;
        }

        const BWAPI::TilePosition inferredTopLeft = enemyStartingBase->getTilePosition();
        const BWAPI::TilePosition inferredCenterA(
            inferredTopLeft.x + 1, inferredTopLeft.y + 1);
        const BWAPI::TilePosition inferredCenterB(
            inferredTopLeft.x + 2, inferredTopLeft.y + 1);
        const bool inferredFootprintVisible =
            inferredCenterA.isValid() && inferredCenterB.isValid() &&
            BWAPI::Broodwar->isVisible(inferredCenterA) &&
            BWAPI::Broodwar->isVisible(inferredCenterB);

        BWAPI::Unit visibleDepot = nullptr;
        for (BWAPI::Unit enemy : the.enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                !enemy->getPosition().isValid() ||
                !enemy->getType().isResourceDepot())
            {
                continue;
            }

            if (resourceDepotConfirmsStart(enemy, enemyStartingBase))
            {
                visibleDepot = enemy;
                break;
            }
        }

        if (visibleDepot)
        {
            enemyStartingBaseConfirmed = true;
            explicitlyScoutedStartingBaseIDs.insert(enemyStartingBase->getID());
            return;
        }

        if (!inferredFootprintVisible)
        {
            return;
        }

        // The inferred start has now been explicitly disproved. Undo only the
        // inferred ownership marker; never erase a real depot record.
        explicitlyScoutedStartingBaseIDs.insert(enemyStartingBase->getID());
        if (!enemyStartingBase->getDepot() &&
            enemyStartingBase->getOwner() == the.enemy())
        {
            enemyStartingBase->setOwner(nullptr, BWAPI::Broodwar->neutral());
        }
        enemyStartingBase = nullptr;
        enemyStartingBaseConfirmed = false;
    }

    // Call only when enemyStartingBase is unknown (null).
    if (inferEnemyBaseFromOverlord())
    {
        return;
    }

    size_t nScouted = 0;
    Base * unscoutedBase = nullptr;

    for (Base * start : startingBases)
    {
        if (!start)
        {
            continue;
        }

        if (start == startingBase)
        {
            ++nScouted;
            continue;
        }

        // A proxy or a building at a nearby expansion is NOT proof of the
        // enemy main. Confirm a start directly only from its resource depot.
        // If the Terran main is lifted, normal explicit scouting/elimination
        // will still resolve the remaining starting location safely.
        for (BWAPI::Unit enemy : the.enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                !enemy->getPosition().isValid() || enemy->isLifted() ||
                !enemy->getType().isResourceDepot())
            {
                continue;
            }

            if (resourceDepotConfirmsStart(enemy, start))
            {
                if (Config::Debug::DrawScoutInfo)
                {
                    BWAPI::Broodwar->printf("Enemy base confirmed by resource depot");
                }

                enemyStartingBase = start;
                enemyStartingBaseConfirmed = true;
                enemyStartingBase->setInferredEnemyBase();
                explicitlyScoutedStartingBaseIDs.insert(start->getID());
                return;
            }
        }

        if (startLocationScouted(start))
        {
            ++nScouted;
        }
        else
        {
            unscoutedBase = start;
        }
    }

    // Only one starting location has not been explicitly checked. This is a
    // reliable elimination only because every rejected start had its central
    // depot footprint actually visible, not merely explored at one edge.
    if (unscoutedBase && nScouted + 1 == startingBases.size())
    {
        if (Config::Debug::DrawScoutInfo)
        {
            BWAPI::Broodwar->printf("Enemy base found by explicit elimination");
        }

        enemyStartingBase = unscoutedBase;
        enemyStartingBaseConfirmed = false;
        enemyStartingBase->setInferredEnemyBase();
    }
}

void Bases::updateBaseOwners()
{
    bool ourBaseOwnershipChanged = false;

    for (Base * base : bases)
    {
        const bool wasOurs = base->getOwner() == the.self();

        if (base->isVisible())
        {
            // Find the units that overlap the rectangle.
            BWAPI::Unitset units = BWAPI::Broodwar->getUnitsInRectangle(
                BWAPI::Position(base->getTilePosition()),
                BWAPI::Position(base->getTilePosition() + BWAPI::TilePosition(3, 2))
                );
            // Is one of the units a resource depot?
            BWAPI::Unit depot = nullptr;
            for (BWAPI::Unit unit : units)
            {
                if (unit->getType().isResourceDepot() && !unit->isLifted())
                {
                    depot = unit;
                    break;
                }
            }
            if (depot)
            {
                // The base is occupied.
                // NOTE The player might be the neutral player. That's OK.
                base->setOwner(depot, depot->getPlayer());
            }
            else
            {
                // The base is empty.
                base->setOwner(nullptr, the.neutral());
            }
        }
        else
        {
            // The base is out of sight. It's definitely not our base.
            if (base->getOwner() == the.self())
            {
                base->setOwner(nullptr, the.neutral());
            }
        }

        if (wasOurs != (base->getOwner() == the.self()))
        {
            ourBaseOwnershipChanged = true;
        }
    }

    if (ourBaseOwnershipChanged)
    {
        the.placer.refreshMovementLanes();
    }
}

// The starting base is the base we began at. Once set, it never changes.
// The main base is initially the starting base. We change it if the main base is lost.
void Bases::updateMainBase()
{
    Base * oldMainBase = mainBase;
    Base * oldNaturalBase = naturalBase;

    if (mainBase->owner != the.self())
    {
        // Choose a base we own which is as far away from the old main as possible.
        // Maybe that will be safer.
        int newMainDist = 0;
        Base * newMain = nullptr;

        for (Base * base : bases)
        {
            if (base->owner == the.self())
            {
                int dist = base->getPosition().getApproxDistance(mainBase->getPosition());
                if (dist > newMainDist)
                {
                    newMainDist = dist;
                    newMain = base;
                }
            }
        }

        // If we didn't find a new main base, we're in deep trouble. We may as well keep the old one.
        // By decree, we always have a main base, even if it is unoccupied. It simplifies the rest.
        if (newMain)
        {
            mainBase = newMain;
            // If we changed mains to a new starting base and our original starting base is gone,
            // it makes sense to declare a new natural.
            if (newMain->getNatural() && startingBase->getOwner() != the.self())
            {
                naturalBase = newMain->getNatural();
            }
        }
    }

    if (mainBase != oldMainBase || naturalBase != oldNaturalBase)
    {
        the.placer.refreshMovementLanes();
    }
}

void Bases::updateSmallMinerals()
{
    if (the.now() % 27 == 0)
    {
        for (auto it = smallMinerals.begin(); it != smallMinerals.end(); )
        {
            if (the.info.isMineralDestroyed(*it))
            {
                it = smallMinerals.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// Find the bases on the map at the beginning of the game.
void Bases::initialize()
{
    // Find the resources to mine: Mineral patches and geysers.
    BWAPI::Unitset resources;

    for (BWAPI::Unit unit : BWAPI::Broodwar->getStaticMinerals())
    {
        // Skip mineral patches with negligible resources. Bases don't belong there.
        if (unit->getInitialResources() > 64)
        {
            resources.insert(unit);
        }
        else
        {
            smallMinerals.insert(unit);
        }
    }
    for (BWAPI::Unit unit : BWAPI::Broodwar->getStaticGeysers())
    {
        if (unit->getInitialResources() > 0)
        {
            resources.insert(unit);
        }
    }

    // Add the starting bases.
    // Remove their resources from the list.
    for (BWAPI::TilePosition pos : BWAPI::Broodwar->getStartLocations())
    {
		if (!pos.isValid())		// has never happened, but guard against bad maps
		{
			continue;
		}

        Base * base = new Base(pos, resources);
        if (base->getMinerals().empty())
        {
            // The base has no minerals. It's probably an observer slot, not a real base.
            delete base;
            continue;
        }
        bases.push_back(base);
        startingBases.push_back(base);
        removeUsedResources(resources, base);

        if (pos == the.self()->getStartLocation())
        {
            startingBase = base;
            mainBase = base;
        }
    }

    // Add the remaining bases.
    size_t priorResourceSize = resources.size();
    while (resources.size() > 0)
    {
        // 1. Choose a resource, any resource. We'll use ground distances from it.
        BWAPI::Unit startResource = *(resources.begin());
        GridDistances fromStart(startResource->getInitialTilePosition(), BaseResourceRange, false);

        // 2. Form a group of nearby resources that are candidates for placing a base near.
        //    Keep track of how much stuff gets included in the group.
        BWAPI::Unitset resourceGroup;
        int totalMins = 0;
        int totalGas = 0;
        for (BWAPI::Unit otherResource : resources)
        {
            const int dist = fromStart.getStaticUnitDistance(otherResource);
            // All distances are <= baseResourceRange, because we limited the distance map to that distance.
            // dist == -1 means inaccessible by ground.
            if (dist >= 0)
            {
                resourceGroup.insert(otherResource);
                countResources(otherResource, totalMins, totalGas);
            }
        }

        UAB_ASSERT(resourceGroup.size() > 0, "no resources in group");

        // 3. If the base is worth making and can be made, make it.
        //    If not, remove all the resources: They are all useless.
        BWAPI::TilePosition basePosition = BWAPI::TilePositions::Invalid;
        if (totalMins >= MinTotalMinerals || totalGas >= MinTotalGas)
        {
            basePosition = findBasePosition(resourceGroup);
        }

        Base * base = nullptr;

        if (basePosition.isValid())
        {
            // Make the base. The data structure lifetime is normally the program lifetime.
            base = new Base(basePosition, resources);

            // Check whether the base actually grabbed enough resources to be useful.
            if (base->getInitialMinerals() >= MinTotalMinerals || base->getInitialGas() >= MinTotalGas)
            {
                removeUsedResources(resources, base);
                bases.push_back(base);
            }
            else
            {
                // No good. Drop the base after all.
                // This can happen when resources exist but are (or seem) inaccessible.
                delete base;
                base = nullptr;
            }
        }

        if (!base)
        {
            // It's not possible to make the base, or not worth it.
            // All resources in the group should be considered "used up" or "assigned to nothing".
            nonbases.push_back(resourceGroup);
            for (BWAPI::Unit resource : resourceGroup)
            {
                resources.erase(resource);
            }
        }

        // Error check. This should never happen.
        if (resources.size() >= priorResourceSize)
        {
            BWAPI::Broodwar->printf("failed to remove any resources");
            break;
        }
        priorResourceSize = resources.size();
    }

    // Fill in other map properties we want to remember.
    setBaseIDs();
    enemyStartingBase = nullptr;
    enemyStartingBaseConfirmed = false;
    explicitlyScoutedStartingBaseIDs.clear();
    if (startingBase)
    {
        explicitlyScoutedStartingBaseIDs.insert(startingBase->getID());
    }
    islandStart = checkIslandStart();       // we start on an island
    islandBases = checkIslandBases();       // the map includes islands
    rememberBaseBlockers();

    // Pick a natural for each starting base.
    // Other bases have null for the natural pointer.
    for (Base * start : startingBases)
    {
        start->initializeNatural(bases);
    }
    naturalBase = startingBase->getNatural();  // may be null, rarely is

    // The "front line" position for each base has dependencies set above.
    for (Base * base : bases)
    {
        base->initializeFront();
    }
}

void Bases::refreshMapAnalysis()
{
    // Ground-distance maps depend on current neutral blockers. Keep the base
    // objects/IDs stable, but rebuild every distance field.
    for (Base * base : bases)
    {
        base->refreshDistances();
        base->resetTopologyLinks();
    }

    // Re-evaluate global connectivity before choosing naturals/fronts.
    islandStart = checkIslandStart();
    islandBases = checkIslandBases();

    for (Base * start : startingBases)
    {
        start->initializeNatural(bases);
    }
    naturalBase = startingBase ? startingBase->getNatural() : nullptr;

    for (Base * base : bases)
    {
        base->initializeFront();
    }
}

void Bases::update()
{
    updateEnemyStart();
    updateBaseOwners();
    updateMainBase();
    updateSmallMinerals();
}

// When a building is placed, we are told the desired and actual location of the building.
// Buildings are usually placed in the "main" base, so this can give us a hint when the main base
// is full and we need to choose a new main base.
void Bases::checkBuildingPosition(const BWAPI::TilePosition & desired, const BWAPI::TilePosition & actual)
{
    UAB_ASSERT(desired.isValid(), "bad location");
    Base * oldMainBase = mainBase;

    const Zone * desiredArea = the.zone.areaAt(desired);
    const Zone * mainArea = mainBase ? the.zone.areaAt(mainBase->getCenter()) : nullptr;

    if (desiredArea && mainArea && desiredArea == mainArea)
    {
        // We tried to place in the main area, so let's keep checking. Use the
        // high-level area assignment rather than a narrow legacy choke zone.
        const Zone * actualArea = actual.isValid() ? the.zone.areaAt(actual) : nullptr;
        if (!actual.isValid() || actualArea != desiredArea)
        {
            // We failed to place it, or placed it outside the base's zone.

            // 1. Count the failures associated with this base.
            mainBase->placementFailed();
            // BWAPI::Broodwar->printf("placement failed in main base %d (total %d)", mainBase->getID(), mainBase->getFailedPlacements());

            // 2. Look for a new main base. Pick the one with the fewest failures.
            int bestFails = mainBase->getFailedPlacements();
            for (Base * base : bases)
            {
                // Protoss: The base must have a completed pylon.
                // Zerg: The base must be completed, so that it has some creep.
                if (base->getOwner() == the.self() &&
                    true)
                {
                    int fails = base->getFailedPlacements();
                    if (fails < bestFails)
                    {
                        bestFails = fails;
                        mainBase = base;
                    }
                }
            }
            // BWAPI::Broodwar->printf("  new main %d (total %d)", mainBase->getID(), mainBase->getFailedPlacements());
        }
    }

    if (mainBase != oldMainBase)
    {
        the.placer.refreshMovementLanes();
    }
}

void Bases::drawBaseInfo() const
{
    //the.partitions.drawWalkable();
    //the.partitions.drawPartition(
    //	the.partitions.id(the.bases.myMain()->getPosition()),
    //	BWAPI::Colors::Teal);

    //the.inset.draw();
    //the.vWalkRoom.draw();
    //the.tileRoom.draw();
    if (Config::Debug::DrawMapInfo || Config::Debug::DrawTankPositions)
    {
        the.zone.draw();
    }
    if (!Config::Debug::DrawMapInfo)
    {
        return;
    }

    for (const Base * base : bases)
    {
        base->drawBaseInfo();
    }

    for (BWAPI::Unit small : smallMinerals)
    {
        BWAPI::Broodwar->drawCircleMap(small->getInitialPosition() + BWAPI::Position(0, 0), 32, BWAPI::Colors::Green);
    }

    int i = 0;
    for (const auto & rejected : nonbases)
    {
        ++i;
        for (auto resource : rejected)
        {
            BWAPI::Broodwar->drawTextMap(resource->getInitialPosition() + BWAPI::Position(-16, -6), "%c%d", red, i);
            BWAPI::Broodwar->drawCircleMap(resource->getInitialPosition() + BWAPI::Position(0, 0), 32, BWAPI::Colors::Red);
        }
    }

    for (const auto & potential : potentialBases)
    {
        // THe bounding box of the resources.
        BWAPI::Broodwar->drawBoxMap(BWAPI::Position(potential.left, potential.top), BWAPI::Position(potential.right, potential.bottom), BWAPI::Colors::Yellow);

        // The starting tile for base placement.
        //BWAPI::Broodwar->drawBoxMap(BWAPI::Position(potential.startTile), BWAPI::Position(potential.startTile)+BWAPI::Position(31,31), BWAPI::Colors::Yellow);
    }
}

void Bases::drawBaseOwnership(int x, int y) const
{
    if (!Config::Debug::DrawBaseInfo)
    {
        return;
    }

    int yy = y;

    BWAPI::Broodwar->drawTextScreen(x, yy, "%cBases", white);
        
    for (const Base * base : bases)
    {
        yy += 10;

        char color = gray;

        char reservedChar = ' ';
        if (base->isReserved())
        {
            reservedChar = '*';
        }

        char inferredChar = ' ';
        BWAPI::Player player = base->owner;
        if (player == the.self())
        {
            color = green;
        }
        else if (player == the.enemy())
        {
            color = orange;
            if (base->resourceDepot == nullptr)
            {
                inferredChar = '?';
            }
        }

        char baseCode = ' ';
        if (base == startingBase)
        {
            baseCode = 'M';
        }
        else if (base == naturalBase)
        {
            baseCode = 'N';
        }

        BWAPI::TilePosition pos = base->getTilePosition();
        BWAPI::Broodwar->drawTextScreen(x - 8, yy, "%c%c", white, reservedChar);
        BWAPI::Broodwar->drawTextScreen(x, yy, "%c%d, %d%c%c", color, pos.x, pos.y, inferredChar, baseCode);
    }
}

// Our "frontmost" base, the base which we most want to defend from ground attack.
// May be null if we do not own any bases!
Base * Bases::myFront() const
{
    
    {
        // We're not zerg. We may be able to build at any base.
        if (enemyStart() && enemyStart()->getNatural() && enemyStart()->getNatural()->getOwner() == the.self())
        {
            // We took the enemy natural. That's definitely our front base.
            return enemyStart()->getNatural();
        }
        if (naturalBase && naturalBase->getOwner() == the.self())
        {
            return naturalBase;
        }
        if (startingBase->getOwner() == the.self())
        {
            return startingBase;
        }
        if (mainBase->getOwner() == the.self())
        {
            return mainBase;
        }

        // Otherwise look for any base we own.
        for (Base * base : bases)
        {
            if (base->getOwner() == the.self())
            {
                return base;
            }
        }
    }

    // We have no buildable bases. Ouch.
    return nullptr;
}

// Return a position to place static defense or deploy a defensive force.
// If we can't figure out such a place, return no position.
// Not too smart, so far.
BWAPI::Position Bases::front() const
{
    Base * frontBase = myFront();
    if (frontBase)
    {
        return frontBase->getFront();
    }
    return BWAPI::Positions::None;
}

// Return a position to place static defense or deploy a defensive force.
BWAPI::TilePosition Bases::frontTile() const
{
    Base * frontBase = myFront();
    if (frontBase)
    {
        return frontBase->getFrontTile();
    }
    return BWAPI::TilePositions::None;
}

// The given position is reachable by ground from our starting base.
// The caller must ensure that the position passed in is valid.
bool Bases::connectedToStart(const BWAPI::Position & pos) const
{
    return the.partitions.id(pos) == the.partitions.id(startingBase->getTilePosition());
}

// The given tile is reachable by ground from our starting base.
// The caller must ensure that the position passed in is valid.
bool Bases::connectedToStart(const BWAPI::TilePosition & tile) const
{
    return the.partitions.id(tile) == the.partitions.id(startingBase->getTilePosition());
}

// Return the base at or close to the given position, or null if none.
Base * Bases::getBaseAtTilePosition(BWAPI::TilePosition pos)
{
    for (Base * base : bases)
    {
        if (closeEnough(pos, base->getTilePosition()))
        {
            return base;
        }
    }

    return nullptr;
}

// The number of bases believed owned by the given player,
// self, enemy, or neutral.
int Bases::baseCount(BWAPI::Player player) const
{
    int count = 0;

    for (const Base * base : bases)
    {
        if (base->getOwner() == player)
        {
            ++count;
        }
    }

    return count;
}

// The number of completed bases believed owned by the given player,
// self, enemy, or neutral.
int Bases::completedBaseCount(BWAPI::Player player) const
{
    int count = 0;

    for (const Base * base : bases)
    {
        if (base->getOwner() == player && base->isCompleted())
        {
            ++count;
        }
    }

    return count;
}

// The number of reachable expansions that are believed not yet taken.
int Bases::freeLandBaseCount() const
{
    int count = 0;

    for (Base * base : bases)
    {
        if (base->getOwner() == the.neutral() && connectedToStart(base->getTilePosition()))
        {
            ++count;
        }
    }

    return count;
}

// Number of non-zero-amount mineral patches at all of my bases.
// Decreases as patches mine out, increases as new bases are taken.
int Bases::remainingMineralPatchCount() const
{
    int count = 0;

    for (Base * base : bases)
    {
        if (base->getOwner() == the.self())
        {
            // Base::getMinerals() is the original resource set and keeps
            // mined-out handles. Only the live subset is useful worker
            // capacity; otherwise getMaxWorkers() never shrinks in late game.
            count += int(base->getRemainingMinerals().size());
        }
    }

    return count;
}

// Current number of geysers at all bases of the given player, whether taken or not.
int Bases::geyserCount(BWAPI::Player player) const
{
    int count = 0;

    for (Base * base : bases)
    {
        if (base->getOwner() == player)
        {
            count += base->getGeysers().size();
        }
    }

    return count;
}

// Current number of my completed refineries at my completed bases,
// and number of bare geysers available to be taken.
// Not counted: Enemy stolen refineries; refineries under construction.
void Bases::gasCounts(int & nRefineries, int & nFreeGeysers) const
{
    int refineries = 0;
    int geysers = 0;

    for (Base * base : bases)
    {
        if (base->getOwner() == the.self() && base->isCompleted())
        {
            // Recalculate the base's geysers every time.
            // This is a slow but accurate way to work around the BWAPI geyser bug.
            base->updateGeysers();

            for (BWAPI::Unit geyser : base->getGeysers())
            {
                if (geyser->getPlayer() == the.self() &&
                    geyser->getType().isRefinery() &&
                    geyser->isCompleted())
                {
                    ++refineries;
                }
                else if (geyser->getType() == BWAPI::UnitTypes::Resource_Vespene_Geyser)
                {
                    ++geysers;
                }
            }
        }
    }

    nRefineries = refineries;
    nFreeGeysers = geysers;
}

// Find the closest base which contains the given position, if any.
// A point is "in a base" if it is in the same zone and is close enough.
// On some maps, there may be more than one, so we have to find the closest.
Base * Bases::findBaseByPosition(const BWAPI::Position & pos) const
{
	Base * bestBase = nullptr;
	int bestDist = MAX_DISTANCE;

	const Zone * posArea = the.zone.areaAt(pos);
	for (Base * b : getAll())
	{
		const Zone * baseArea = the.zone.areaAt(b->getCenter());
		if (posArea && baseArea && posArea == baseArea)
		{
			int dist = pos.getApproxDistance(b->getPosition());
			if (dist <= BaseRadius && dist < bestDist)
			{
				bestBase = b;
				bestDist = dist;
			}
		}
	}
	return bestBase;
}

// A static neutral has disappeared. Forget it as a base blocker. Topology
// rebuilding itself is handled centrally by The::onNeutralDestroyed().
void Bases::clearNeutral(BWAPI::Unit unit)
{
    if (!unit)
    {
        return;
    }

    auto it = baseBlockers.find(unit);
    if (it != baseBlockers.end())
    {
        it->second->clearBlocker(unit);
        (void)baseBlockers.erase(it);
    }
}

Bases & Bases::Instance()
{
    static Bases instance;
    return instance;
}
