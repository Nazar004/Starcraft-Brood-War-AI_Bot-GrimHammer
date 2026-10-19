#include "Map/Base.h"

#include "Map/Bases.h"
#include "Construction/BuildingManager.h"
#include "Intelligence/InformationManager.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"
#include "Economy/WorkerManager.h"

#include <algorithm>
#include <cmath>

using namespace UAlbertaBot;


namespace
{
BWAPI::Position findWalkableDirectionalTarget(
    const BWAPI::Position & origin,
    const BWAPI::Position & desired)
{
    if (!origin.isValid() || !desired.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::TilePosition originTile(origin);
    BWAPI::Position desiredValid = desired;
    desiredValid.makeValid();
    const BWAPI::TilePosition desiredTile(desiredValid);
    const int partition = originTile.isValid() ? the.partitions.id(originTile) : 0;

    for (int radius = 0; radius <= 14; ++radius)
    {
        BWAPI::TilePosition best = BWAPI::TilePositions::Invalid;
        int bestScore = MAX_DISTANCE;

        for (int dx = -radius; dx <= radius; ++dx)
        {
            for (int dy = -radius; dy <= radius; ++dy)
            {
                if (std::max(std::abs(dx), std::abs(dy)) != radius)
                {
                    continue;
                }

                const BWAPI::TilePosition tile(desiredTile.x + dx, desiredTile.y + dy);
                if (!tile.isValid() || !the.map.isWalkable(tile))
                {
                    continue;
                }
                if (partition > 0 && the.partitions.id(tile) != partition)
                {
                    continue;
                }

                const int distance = the.map.getGroundTileDistance(originTile, tile);
                if (distance < 0)
                {
                    continue;
                }

                const int score = TileCenter(tile).getApproxDistance(desired);
                if (score < bestScore)
                {
                    bestScore = score;
                    best = tile;
                }
            }
        }

        if (best.isValid())
        {
            return TileCenter(best);
        }
    }

    return BWAPI::Positions::Invalid;
}

BWAPI::Position baseOutsideRouteTarget(const Base * base)
{
    if (!base || !base->getCenter().isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position center = base->getCenter();

    if (base->isAStartingBase() && base->getNatural() &&
        base->getNatural()->getCenter().isValid())
    {
        return base->getNatural()->getCenter();
    }

    if (base->getMain() && base->getMain()->getCenter().isValid())
    {
        // For a natural, prefer a real strategic destination rather than a
        // purely geometric "away from main" ray. On Benzene/Andromeda the
        // latter can point into a wall and make the nearest reachable fallback
        // choose the wrong side of the platform.
        Base * knownEnemy = the.bases.enemyStart();
        if (knownEnemy && knownEnemy != base && knownEnemy->getCenter().isValid() &&
            the.map.getGroundTileDistance(base->getCenterTile(), knownEnemy->getCenterTile()) >= 0)
        {
            return knownEnemy->getCenter();
        }

        const BWAPI::Position mapCenter(
            BWAPI::Broodwar->mapWidth() * 16,
            BWAPI::Broodwar->mapHeight() * 16);
        const BWAPI::Position centerTarget =
            findWalkableDirectionalTarget(center, mapCenter);
        if (centerTarget.isValid())
        {
            return centerTarget;
        }

        // Last natural fallback: continue away from the main.
        const BWAPI::Position mainCenter = base->getMain()->getCenter();
        double dx = double(center.x - mainCenter.x);
        double dy = double(center.y - mainCenter.y);
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.0)
        {
            dx /= len;
            dy /= len;
            const BWAPI::Position desired(
                int(center.x + dx * 20.0 * 32),
                int(center.y + dy * 20.0 * 32));
            const BWAPI::Position target = findWalkableDirectionalTarget(center, desired);
            if (target.isValid())
            {
                return target;
            }
        }
    }

    Base * enemy = the.bases.enemyStart();
    if (enemy && enemy != base && enemy->getCenter().isValid() &&
        the.map.getGroundTileDistance(base->getCenterTile(), enemy->getCenterTile()) >= 0)
    {
        return enemy->getCenter();
    }

    // Last fallback: any connected starting base. This is only for unusual
    // maps/bases which are neither a starting main nor its natural.
    Base * best = nullptr;
    int bestDistance = MAX_DISTANCE;
    for (Base * candidate : the.bases.getStarting())
    {
        if (!candidate || candidate == base || candidate == base->getMain())
        {
            continue;
        }
        const int distance = the.map.getGroundTileDistance(
            base->getCenterTile(), candidate->getCenterTile());
        if (distance >= 0 && distance < bestDistance)
        {
            bestDistance = distance;
            best = candidate;
        }
    }

    return best ? best->getCenter() : BWAPI::Positions::Invalid;
}
}

// Is the base one of the map's starting bases?
// The only purpose of this method is to initialize the startingBase flag.
// NOTE This depends on tilePosition, so the startingBase flag must be declared after tilePosition.
bool Base::findIsStartingBase() const
{
    for (BWAPI::TilePosition tile : BWAPI::Broodwar->getStartLocations())
    {
        if (tile == tilePosition)
        {
            return true;
        }
    }
    return false;
}

// Find the explicit terrain choke which represents the outward approach from
// this base. Starting mains aim toward their natural; naturals aim away from
// their main toward another starting location. Other bases use the nearest
// connected starting location as an outside reference.
const ChokePoint * Base::findFrontChoke() const
{
    const BWAPI::Position outside = baseOutsideRouteTarget(this);
    if (!outside.isValid())
    {
        return nullptr;
    }

    std::vector<BWAPI::TilePosition> path;
    if (!the.map.getGroundPath(getCenterTile(), BWAPI::TilePosition(outside), path, 64) ||
        path.size() < 5)
    {
        return nullptr;
    }

    // The canonical Area graph is the primary topology source. Ground-path
    // intersection remains a hard validation layer, so a bad/odd custom-map
    // topology result can never put the front through a cliff or open plateau.
    auto chokePathScore = [&path](const ChokePoint * choke) -> int
    {
        if (!choke || !choke->center().isValid())
        {
            return MAX_DISTANCE;
        }

        const BWAPI::TilePosition chokeTile(choke->center());
        if (!chokeTile.isValid() || !the.map.isWalkable(chokeTile))
        {
            return MAX_DISTANCE;
        }

        const int room = the.tileRoom.at(chokeTile);
        if (room <= 0 || room > 18)
        {
            return MAX_DISTANCE;
        }

        int bestPathIndex = -1;
        int bestPathOffset = MAX_DISTANCE;
        const int pathLimit = std::min(int(path.size()), 28);
        for (int i = 4; i < pathLimit; ++i)
        {
            const int offset = TileBoxDistance(path[i], chokeTile);
            if (offset < bestPathOffset)
            {
                bestPathOffset = offset;
                bestPathIndex = i;
            }
        }

        if (bestPathIndex < 0 || bestPathOffset > 1)
        {
            return MAX_DISTANCE;
        }
        return bestPathIndex * 24 + room * 8 + bestPathOffset * 300;
    };

    const ChokePoint * topologyFirst = the.zone.findFirstChoke(getCenter(), outside);
    if (chokePathScore(topologyFirst) < MAX_DISTANCE)
    {
        return topologyFirst;
    }

    // Fallback for pathological/custom maps: inspect all validated chokes that
    // intersect the real route and choose the earliest narrow crossing.
    const ChokePoint * best = nullptr;
    int bestScore = MAX_DISTANCE;
    for (const ChokePoint & choke : the.zone.chokes())
    {
        const int score = chokePathScore(&choke);
        if (score < bestScore)
        {
            bestScore = score;
            best = &choke;
        }
    }

    return best;
}

// Figure out the "front" of the base where defenses against approach should go.
// Prefer a point a little INSIDE the real terrain choke, leaving the choke
// center itself free for traffic. Fall back to the legacy enemy-facing heuristic
// on completely open or unusual maps with no explicit choke.
BWAPI::TilePosition Base::findFront(const ChokePoint * choke) const
{
    const BWAPI::Position center = getCenter();
    const int baseHeight = GroundHeight(getCenterTile());
    BWAPI::Position desired = BWAPI::Positions::Invalid;

    if (choke && choke->center().isValid())
    {
        const BWAPI::Position chokeCenter = choke->center();
        double dx = double(center.x - chokeCenter.x);
        double dy = double(center.y - chokeCenter.y);
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length > 1.0)
        {
            desired = BWAPI::Position(
                int(chokeCenter.x + dx / length * 2.0 * 32),
                int(chokeCenter.y + dy / length * 2.0 * 32));
        }
    }

    // No trustworthy explicit choke: use the actual shortest ground route.
    // Eight tiles outward is a stable defensive-front reference and cannot
    // jump to an unrelated Area boundary on the other side of the map.
    if (!desired.isValid())
    {
        const BWAPI::Position outside = baseOutsideRouteTarget(this);
        if (outside.isValid())
        {
            desired = the.map.getGroundPathWaypoint(center, outside, 8);
        }
    }

    if (desired.isValid())
    {
        const BWAPI::TilePosition desiredTile(desired);
        BWAPI::TilePosition best = BWAPI::TilePositions::Invalid;
        int bestScore = MAX_DISTANCE;
        const BWAPI::TilePosition mineralTile = getMineralLineTile();
        const BWAPI::Position mineralLine = mineralTile.isValid()
            ? TileCenter(mineralTile)
            : BWAPI::Positions::Invalid;

        for (int radius = 0; radius <= 6; ++radius)
        {
            for (int ox = -radius; ox <= radius; ++ox)
            {
                for (int oy = -radius; oy <= radius; ++oy)
                {
                    if (std::max(std::abs(ox), std::abs(oy)) != radius)
                    {
                        continue;
                    }

                    const BWAPI::TilePosition tile(desiredTile.x + ox, desiredTile.y + oy);
                    if (!tile.isValid() || !the.map.isWalkable(tile) ||
                        GroundHeight(tile) != baseHeight)
                    {
                        continue;
                    }

                    const int groundDistance = the.map.getGroundTileDistance(getCenterTile(), tile);
                    if (groundDistance < 0 || groundDistance > 18)
                    {
                        continue;
                    }

                    const BWAPI::Position tileCenter = TileCenter(tile);
                    int score = tileCenter.getApproxDistance(desired);
                    if (mineralLine.isValid())
                    {
                        const int mineralDistance = tileCenter.getApproxDistance(mineralLine);
                        if (mineralDistance < 5 * 32)
                        {
                            score += 4000;
                        }
                    }

                    if (score < bestScore)
                    {
                        bestScore = score;
                        best = tile;
                    }
                }
            }

            if (best.isValid() && radius >= 2)
            {
                break;
            }
        }

        if (best.isValid())
        {
            return best;
        }
    }

    return BWAPI::TilePosition((getCenter() - getMineralOffset()).makeValid());
}

// The mean offset of the base's mineral patches from the center of the resource depot.
// This is used to tell what direction the minerals are in.
BWAPI::Position Base::findMineralOffset() const
{
    BWAPI::Position center = getCenter();
    BWAPI::Position offset = BWAPI::Positions::Origin;
    for (BWAPI::Unit mineral : getMinerals())
    {
        offset += mineral->getInitialPosition() - center;
    }
    return BWAPI::Position(offset / getMinerals().size());
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// Create a base given its position and a set of resources that may belong to it.
// The caller is responsible for eliminating resources which are too small to be worth it.
// NOTE groundSafePaths and airSafePaths are not initialized yet. They are created on demand.
Base::Base(BWAPI::TilePosition tile, const BWAPI::Unitset & availableResources)
    : id(-1)        // invalid value, will be reset after bases are sorted
    , tilePosition(tile)
    , distances(tile)
	, groundSafePaths(GridSafePathGround(tile, static_cast<Grid &>(distances)))
	, airSafePaths(GridSafePathAir(tile))
	, startingBase(findIsStartingBase())
    , naturalBase(nullptr)
    , mainBase(nullptr)
    , front(BWAPI::Positions::Unknown)      // filled in by initializeFront() when all info is known
    , frontChoke(nullptr)
    , reserved(false)
    , airDanger(false)
    , workerDanger(false)
    , doomed(false)
    , failedPlacements(0)
    , resourceDepot(nullptr)
    , owner(BWAPI::Broodwar->neutral())
{
	// Calculate distances up to BaseResourceRange. Tiles beyond are initialized to -1.
    GridDistances resourceDistances(tile, BaseResourceRange, false);

    for (BWAPI::Unit resource : availableResources)
    {
        if (resource->getInitialTilePosition().isValid() && resourceDistances.getStaticUnitDistance(resource) >= 0)
        {
            if (resource->getInitialType().isMineralField())
            {
                minerals.insert(resource);
            }
            else if (resource->getInitialType() == BWAPI::UnitTypes::Resource_Vespene_Geyser)
            {
                initialGeysers.insert(resource);
            }
        }
    }
    geysers = initialGeysers;
    mineralOffset = findMineralOffset();

    // Fill in the set of blockers, destructible neutral units that are very close to the base
    // and may interfere with its operation. (Or may protect the base. Who knows?)
    // This does not include the minerals to mine!
    for (BWAPI::Unit unit : BWAPI::Broodwar->getStaticNeutralUnits())
    {
        // NOTE Khaydarin crystals are not destructible, and I don't know any way
        //      to find that out other than to check the name explicitly. Is there a way?
        if (!unit->getInitialType().canMove() &&
            !unit->isInvincible() &&
            unit->isTargetable() &&
            !unit->isFlying() &&
            unit->getInitialType().getName().find("Khaydarin") == std::string::npos)
        {
            int dist = resourceDistances.getStaticUnitDistance(unit);
            if (dist >= 0)		// and necessarily <= BaseResourceRange
            {
                blockers.insert(unit);
            }
        }
    }
}

// This is to be called exactly once at startup time.
// With the initial -1 value set in the constructor, the checks here prevent multiple calls.
void Base::setID(int baseID)
{
    UAB_ASSERT(id == -1, "BUG! base ID reset");
    UAB_ASSERT(baseID >= 1, "BUG! bad base ID");
    id = baseID;
}

// The closest non-starting base to this base is its natural (with other conditions).
// This is only called (by Bases) if this is a starting base. Other bases have no natural.
// Also tell the natural that this is its main base.
// NOTE If the map has two naturals for each main, this will pick the same one each time
//      as "the" natural. We'd need a fancier analysis.
// NOTE If the map has two mains sharing the same natural, this will give bad results.
//      No competitive map should be laid out that way.
void Base::initializeNatural(const std::vector<Base *> & bases)
{
    int bestScore = MAX_DISTANCE;
    Base * bestNatural = nullptr;

    for (Base * base : bases)
    {
        if (base->isAStartingBase() || base->getMain() || base->getInitialMinerals() <= 0)
        {
            continue;
        }

        const int dist = base->getTileDistance(tilePosition);   // -1 if not connected by ground
        if (dist <= 0)
        {
            continue;
        }

        int score = dist;
        if (base->getInitialGas() == 0)
        {
            // A mineral-only may be the natural, but only if it is much closer.
            score *= 2;
        }

        // Natural selection deliberately uses the proven low-level ground
        // distance, not the advisory Area graph. A false/open-space Area
        // boundary must never make us choose the wrong expansion and then
        // derive every front/bunker position from that wrong base.

        if (score < bestScore)
        {
            bestScore = score;
            bestNatural = base;
        }
    }

    if (bestNatural)
    {
        naturalBase = bestNatural;
        naturalBase->mainBase = this;
    }
}

// The "front line" of the base, where static defense and mobile defenders will go
// if the base is the frontmost base.
void Base::initializeFront()
{
    frontChoke = findFrontChoke();
    front = findFront(frontChoke);
}

void Base::refreshDistances()
{
    distances.recompute(tilePosition);
    groundSafePaths.resetFromDistances(distances);
}

void Base::resetTopologyLinks()
{
    naturalBase = nullptr;
    mainBase = nullptr;
    frontChoke = nullptr;
    front = BWAPI::TilePositions::Invalid;
}

// The base is on an island, unconnected by ground to any starting base.
bool Base::isIsland() const
{
    for (const BWAPI::TilePosition & tile : BWAPI::Broodwar->getStartLocations())
    {
        if (tile != getTilePosition() && getTileDistance(tile) > 0)
        {
            return false;
        }
    }

    return true;
}

// The base is completed, that is, its resource depot is completed.
// For an enemy base, we rely on best information.
bool Base::isCompleted() const
{
    if (resourceDepot && resourceDepot->exists())
    {
        if (resourceDepot->isVisible())
        {
            return UnitUtil::IsCompletedResourceDepot(resourceDepot);
        }

        // It's not visible. That implies that the base is enemy.
        // This could give a wrong answer for a terran CC that burned down or lifted.
        UAB_ASSERT(owner == the.enemy(), "unexpected base owner");
        const UnitInfo * ui = the.info.getUnitInfo(resourceDepot);
        if (ui)
        {
            return ui->isCompleted();
        }
    }

    // No resource depot means the base is unowned or inferred to be enemy.
    // We may know it's completed if we know it's the enemy start, but there are tricky cases.
    return false;
}

bool Base::isMyCompletedBase() const
{
    return getOwner() == the.self() && isCompleted();
}

// An "inner base" is one that is potentially protected from ground assault by defenses at
// another base along the way.
// Any base that is not an inner base is an outer base.
// Criteria:
// - The same player must own both bases.
// - Both bases must be completed.
// - For now, the inner base is a main base that has a natural.
bool Base::isInnerBase() const
{
    return
        getNatural() &&
        getOwner() != the.neutral() &&
        getOwner() == getNatural()->getOwner() &&
        isCompleted() &&
        getNatural()->isCompleted();
}

// Recalculate the base's set of geysers, including refineries (completed or not).
// This only works for visible geysers, so it should be called only for bases we own.
// Called to work around BWAPI behavior (maybe not strictly a bug).
void Base::updateGeysers()
{
    geysers.clear();

    for (BWAPI::Unit unit : BWAPI::Broodwar->getAllUnits())
    {
        if ((unit->getType() == BWAPI::UnitTypes::Resource_Vespene_Geyser || unit->getType().isRefinery()) &&
            unit->getPosition().isValid() &&
            unit->getDistance(getCenter()) < 320)
        {
            geysers.insert(unit);
        }
    }
}

// Return a tile near the center of the resource depot location. No tile is at the exact center.
BWAPI::TilePosition Base::getCenterTile() const
{
    return tilePosition + BWAPI::TilePosition(1, 1);
}

// Return the center of the resource depot location.
BWAPI::Position Base::getCenter() const
{
    return BWAPI::Position(tilePosition) + BWAPI::Position(64, 48);
}

// Return a tile near the middle of the mineral line.
// This assume the mineral line is either north, south, east, or west, not in between or split.
BWAPI::TilePosition Base::getMineralLineTile() const
{
    if (abs(mineralOffset.x) > abs(mineralOffset.y))
    {
        return BWAPI::TilePosition(tilePosition.x + (mineralOffset.x > 0 ? 4 : -1), tilePosition.y);
    }

    return BWAPI::TilePosition(tilePosition.x + 1, tilePosition.y + (mineralOffset.y > 0 ? 3 : -1));
}

int Base::getAirTileDistance(const BWAPI::TilePosition & pos) const
{
	return 32 * getAirDistance(pos);
}

int Base::getAirTileDistance(const BWAPI::Position & pos) const
{
	return 32 * getAirDistance(pos);
}

int Base::getAirDistance(const BWAPI::TilePosition & pos) const
{
	return TileCenter(getTilePosition()).getApproxDistance(TileCenter(pos));
}

int Base::getAirDistance(const BWAPI::Position & pos) const
{
	return TileCenter(getTilePosition()).getApproxDistance(pos);
}

// The depot may be null. (That's why player is a separate argument, not depot->getPlayer().)
// A null depot for an owned base means that the base is inferred and hasn't been seen.
void Base::setOwner(BWAPI::Unit depot, BWAPI::Player player)
{
    resourceDepot = depot;
    owner = player;
    reserved = false;

    if (player != the.self() || depot == nullptr)
    {
        airDanger = false;
        workerDanger = false;
        doomed = false;
    }
}

// The resource depot of this base has not been seen, but we think it's enemy owned.
void Base::setInferredEnemyBase()
{
    if (owner == BWAPI::Broodwar->neutral())
    {
        setOwner(nullptr, the.enemy());
    }
}

// Return the set of mineral patches that (still) have minerals.
// FOr bases we own or can see, the result is up to date.
BWAPI::Unitset Base::getRemainingMinerals() const
{
    BWAPI::Unitset mins;

    for (BWAPI::Unit min : minerals)
    {
        if (InformationManager::Instance().getResourceAmount(min) > 0)
        {
            mins.insert(min);
        }
    }

    return mins;
}

// The remaining minerals at the base, as of last report.
// For bases we own, the result is up to date.
int Base::getLastKnownMinerals() const
{
    int total = 0;

    for (BWAPI::Unit min : minerals)
    {
        total += InformationManager::Instance().getResourceAmount(min);
    }

    return total;
}

// The remaining gas at the base, as of last report.
// For bases we own, the result is up to date.
int Base::getLastKnownGas() const
{
    int total = 0;

    for (BWAPI::Unit gas : initialGeysers)
    {
        total += InformationManager::Instance().getResourceAmount(gas);
    }

    return total;
}

int Base::getInitialMinerals() const
{
    int total = 0;
    for (const BWAPI::Unit min : minerals)
    {
        total += min->getInitialResources();
    }
    return total;
}

int Base::getInitialGas() const
{
    int total = 0;
    for (const BWAPI::Unit gas : initialGeysers)
    {
        total += gas->getInitialResources();
    }
    return total;
}

// How many mineral workers to saturate the mineral line?
int Base::getMaxMineralWorkers() const
{
    // WorkerData's mineral-locking assignment deliberately caps useful
    // saturation at two workers per patch. Count only patches which still
    // contain minerals: the Base::minerals set describes the original mineral
    // line and deliberately keeps mined-out resource handles for map history.
    // Using minerals.size() here made an exhausted base look permanently
    // undersaturated and could keep asking its Command Center for new workers.
    const double workersPerPatch =
        std::min(2.0, Config::Macro::WorkersPerPatch);

    const size_t remainingPatches =
        getRemainingMinerals().size();

    return int(workersPerPatch * remainingPatches + 0.5);
}

// How many workers to saturate the base?
// Mineral capacity falls as patches mine out. Gas capacity remains the broad
// base-level estimate; Terran production separately requires a completed
// refinery before adding gas workers to a Command Center's local deficit.
int Base::getMaxWorkers() const
{
    return getMaxMineralWorkers() + Config::Macro::WorkersPerRefinery * geysers.size();
}

// How many workers are assigned to minerals?
int Base::getNumMineralWorkers() const
{
    return WorkerManager::Instance().getNumWorkers(resourceDepot);;\
}

// How many workers are assigned to minerals or gas?
// NOTE Some may be in transit and still far away.
int Base::getNumWorkers() const
{
    // The number of assigned mineral workers.
    int nWorkers = WorkerManager::Instance().getNumWorkers(resourceDepot);

    // Add the assigned gas workers.
    for (BWAPI::Unit geyser : geysers)
    {
        nWorkers += WorkerManager::Instance().getNumWorkers(geyser);
    }

    return nWorkers;
}

// For one of our bases, return the number of our units of the given type there.
// Specifically, the number within a fixed (short) distance of the base center.
// Include buildings that are in the building queue, not yet started, regardless where they are.
// It's used for counting static defense buildings.
int Base::getNumUnits(BWAPI::UnitType type) const
{
    BWAPI::Unitset units = BWAPI::Broodwar->getUnitsInRadius(
        getCenter(),
        8 * 32,
        BWAPI::Filter::GetType == type && BWAPI::Filter::IsOwned
    );

    return int(units.size() + BuildingManager::Instance().getNumUnstarted(type));
}

// Count enemy buildings in the base, leaving out static defense.
int Base::getNumEnemyNondefenseBuildings() const
{
	int count = 0;
	for (const auto & kv : the.info.getUnitData(the.enemy()).getUnits())
	{
		const UnitInfo & ui(kv.second);

		if (ui.type.isBuilding() &&
			isInBase(BWAPI::TilePosition(ui.lastPosition)) &&
			!(UnitUtil::IsStaticDefense(ui.type) || ui.type == BWAPI::UnitTypes::Zerg_Creep_Colony))
		{
			++count;
		}
	}
	return count;
}

// Return whether a given tile is "in the base". It's an approximation.
bool Base::isInBase(const BWAPI::TilePosition tile) const
{
	if (!tile.isValid())
	{
		return false;
	}

	// In the same high-level area and close enough. The legacy zone split can
	// put the ramp/choke itself in a separate zone even though it is still part
	// of the practical base approach.
	const Zone * baseArea = the.zone.areaAt(getCenter());
	const Zone * tileArea = the.zone.areaAt(tile);
	return baseArea && tileArea && baseArea == tileArea &&
		getPosition().getDistance(BWAPI::Position(tile)) < the.bases.BaseRadius;
}

// Return a pylon near the nexus, if any. Let's call 256 pixels "near".
// This only makes a difference for protoss, of course.
// NOTE The pylon may not be complete!


// We're scouting in the early game. Have we seen whether the enemy base is here?
// To turn the scout around as early as possible if the base is empty, we check
// each corner of the resource depot spot.
bool Base::isExplored() const
{
    return
        BWAPI::Broodwar->isExplored(tilePosition) ||
        BWAPI::Broodwar->isExplored(tilePosition + BWAPI::TilePosition(3, 2)) ||
        BWAPI::Broodwar->isExplored(tilePosition + BWAPI::TilePosition(0, 2)) ||
        BWAPI::Broodwar->isExplored(tilePosition + BWAPI::TilePosition(3, 0));
}

// Should we be able to see the resource depot at this base?
// Yes if we can see any corner of its position.
// This is for checking whether an expected enemy base is missing.
bool Base::isVisible() const
{
    return
        BWAPI::Broodwar->isVisible(tilePosition) ||
        BWAPI::Broodwar->isVisible(tilePosition + BWAPI::TilePosition(3, 2)) ||
        BWAPI::Broodwar->isVisible(tilePosition + BWAPI::TilePosition(0, 2)) ||
        BWAPI::Broodwar->isVisible(tilePosition + BWAPI::TilePosition(3, 0));
}

void Base::clearBlocker(BWAPI::Unit blocker)
{
    blockers.erase(blocker);
}

void Base::drawBaseInfo() const
{
    BWAPI::Position center = getCenter();
    if (getNatural())
    {
        BWAPI::Position natural = getNatural()->getCenter();
        BWAPI::Broodwar->drawLineMap(center, natural, BWAPI::Colors::Grey);
        BWAPI::Broodwar->drawCircleMap(natural, 64, BWAPI::Colors::Grey);
    }
    BWAPI::Color color = this == the.bases.myFront() ? BWAPI::Colors::Red : BWAPI::Colors::Blue;
    BWAPI::Broodwar->drawCircleMap(getFront(), 32, color);
    BWAPI::Broodwar->drawLineMap(center, getFront(), color);

    // Debug the source of truth used by movement/front placement. This is the
    // low-level shortest ground route, not the advisory Area graph. Showing it
    // under DrawMapInfo makes a wrong route immediately visible on Benzene.
    if (owner == the.self())
    {
        const BWAPI::Position outside = baseOutsideRouteTarget(this);
        std::vector<BWAPI::TilePosition> route;
        if (outside.isValid() &&
            the.map.getGroundPath(getCenterTile(), BWAPI::TilePosition(outside), route, 36) &&
            route.size() >= 2)
        {
            for (size_t i = 1; i < route.size(); ++i)
            {
                BWAPI::Broodwar->drawLineMap(
                    TileCenter(route[i - 1]),
                    TileCenter(route[i]),
                    BWAPI::Colors::Green);
            }
            BWAPI::Broodwar->drawTextMap(
                TileCenter(route.back()) + BWAPI::Position(6, -6),
                "%cground route", green);
        }
    }
    if (frontChoke && frontChoke->center().isValid())
    {
        BWAPI::Broodwar->drawCircleMap(frontChoke->center(), 12, BWAPI::Colors::Orange);
        BWAPI::Broodwar->drawLineMap(center, frontChoke->center(), BWAPI::Colors::Orange);
        if (frontChoke->endA().isValid() && frontChoke->endB().isValid())
        {
            BWAPI::Broodwar->drawLineMap(frontChoke->endA(), frontChoke->endB(), BWAPI::Colors::Orange);
        }
    }
    
    BWAPI::Position offset(-16, -6);
    for (BWAPI::Unit min : minerals)
    {
        BWAPI::Broodwar->drawTextMap(min->getInitialPosition() + offset, "%c%d", cyan, id);
        // BWAPI::Broodwar->drawTextMap(min->getInitialPosition() + BWAPI::Position(-18, 4), "%c%d", yellow, d.getStaticUnitDistance(min));
    }
    for (BWAPI::Unit gas : geysers)
    {
        BWAPI::Broodwar->drawTextMap(gas->getInitialPosition() + offset, "%cgas %d", cyan, id);
        // BWAPI::Broodwar->drawTextMap(gas->getInitialPosition() + BWAPI::Position(-18, 4), "%cgas %d", yellow, d.getStaticUnitDistance(gas));
    }
    for (BWAPI::Unit blocker : blockers)
    {
        BWAPI::Position pos = blocker->getInitialPosition();
        BWAPI::UnitType type = blocker->getInitialType();
        BWAPI::Broodwar->drawBoxMap(
            pos - BWAPI::Position(type.dimensionLeft(), type.dimensionUp()),
            pos + BWAPI::Position(type.dimensionRight(), type.dimensionDown()),
            BWAPI::Colors::Red);
    }

    BWAPI::Broodwar->drawBoxMap(
        BWAPI::Position(tilePosition),
        BWAPI::Position(tilePosition + BWAPI::TilePosition(4, 3)),
        BWAPI::Colors::Cyan, false);

    if (frontChoke && frontChoke->center().isValid())
    {
        BWAPI::Broodwar->drawCircleMap(frontChoke->center(), 10, BWAPI::Colors::Orange, false);
        BWAPI::Broodwar->drawLineMap(getCenter(), frontChoke->center(), BWAPI::Colors::Orange);
        BWAPI::Broodwar->drawTextMap(
            frontChoke->center() + BWAPI::Position(8, -8),
            "%cchoke %d w%d",
            orange, frontChoke->id(), frontChoke->widthWalkTiles());
    }

	int dx = 35;
    int dy = 40;

	BWAPI::Broodwar->drawTextMap(BWAPI::Position(tilePosition) + BWAPI::Position(dx, dy),
	    "%c%d @ (%d,%d)",
	    cyan, id, tilePosition.x, tilePosition.y);

    if (owner != BWAPI::Broodwar->neutral())
    {
        dy += 12;
        char color = green;
        std::string ownerString = "mine";
        if (owner != BWAPI::Broodwar->self())
        {
            color = orange;
            ownerString = "yours";
        }
        BWAPI::Broodwar->drawTextMap(BWAPI::Position(tilePosition) + BWAPI::Position(dx, dy),
            "%c%s", color, ownerString.c_str());
    }

	if (isDoomed() || inAirDanger() || inWorkerDanger())
	{
		std::string doom = isDoomed() ? "DOOMED" : "";
		std::string ovyDanger = inAirDanger() ? "air danger!" : "";
		std::string workDanger = inWorkerDanger() ? "worker danger!" : "";

		dy += 12;
		BWAPI::Broodwar->drawTextMap(BWAPI::Position(tilePosition) + BWAPI::Position(dx, dy),
			"%c%s %c%s %s", red, doom.c_str(), yellow, ovyDanger.c_str(), workDanger.c_str());
	}
	else if (getOwner() == the.self())
	{
		dy += 12;
		BWAPI::Broodwar->drawTextMap(BWAPI::Position(tilePosition) + BWAPI::Position(dx, dy),
			"%csafe for now", green);
	}

    if (blockers.size() > 0)
    {
        dy += 12;
        BWAPI::Broodwar->drawTextMap(BWAPI::Position(tilePosition) + BWAPI::Position(dx, dy),
            "%cblockers: %c%d",
            red, cyan, blockers.size());
    }
}
