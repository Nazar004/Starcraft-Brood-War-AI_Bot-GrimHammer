#include "Map/MapTools.h"

#include "Map/Bases.h"
#include "Construction/BuildingPlacer.h"
#include "Intelligence/InformationManager.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"
#include "Economy/WorkerManager.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>

using namespace UAlbertaBot;

MapTools::MapTools()
    : _lastRuntimeUpdateFrame(-1)
    , _runtimeRevision(0)
    , _lastExpansionSafetyFrame(-100000)
    , _expansionSafetyTopologyRevision(0)
    , _pathWorkFrame(-1)
    , _distanceFieldsBuilt(0)
    , _groundWaypoints(0)
    , _groundAStarCalls(0)
{
}

void MapTools::notePathWork(int fields, int waypoints, int astar)
{
    const int frame = BWAPI::Broodwar->getFrameCount();
    if (_pathWorkFrame != frame)
    {
        _pathWorkFrame = frame;
        _distanceFieldsBuilt = _groundWaypoints = _groundAStarCalls = 0;
        _tileDistanceFields = _waypointFields = _placementFields =
            _distanceMapEvictions = 0;
    }
    _distanceFieldsBuilt += fields;
    _groundWaypoints += waypoints;
    _groundAStarCalls += astar;
}

void MapTools::touchDistanceMap(BWAPI::TilePosition tile)
{
    _allMapsLastUse[tile] = ++_allMapsUseClock;
}

void MapTools::evictDistanceMapIfFull()
{
    if (_allMaps.size() < allMapsSize) return;

    // std::map::begin() is the smallest map coordinate, not the oldest entry.
    // Repeated requests for a low-coordinate goal were evicted immediately on
    // a full cache and rebuilt hundreds of times in one production frame.
    auto oldest = std::min_element(_allMapsLastUse.begin(), _allMapsLastUse.end(),
        [](const auto & a, const auto & b) { return a.second < b.second; });
    if (oldest != _allMapsLastUse.end())
    {
        _allMaps.erase(oldest->first);
        _allMapsLastUse.erase(oldest);
        ++_distanceMapEvictions;
    }
}

MapTools::PathWork MapTools::pathWorkAtFrame(int frame) const
{
    return _pathWorkFrame == frame
        ? PathWork{ _distanceFieldsBuilt, _groundWaypoints,
            _groundAStarCalls, _tileDistanceFields, _waypointFields,
            _placementFields, _distanceMapEvictions }
        : PathWork{ 0, 0, 0, 0, 0, 0, 0 };
}

namespace
{
    // Does a 32x32 build tile contain a 2x2 block of terrain-walkable
    // 8x8 mini-tiles? This is a better approximation of unit passage than the
    // old "all 16 mini-tiles must be walkable" rule.
    bool hasTerrainPassage(const BWAPI::TilePosition & tile)
    {
        const int baseX = tile.x * 4;
        const int baseY = tile.y * 4;

        for (int x = 0; x < 3; ++x)
        {
            for (int y = 0; y < 3; ++y)
            {
                if (BWAPI::Broodwar->isWalkable(baseX + x, baseY + y) &&
                    BWAPI::Broodwar->isWalkable(baseX + x + 1, baseY + y) &&
                    BWAPI::Broodwar->isWalkable(baseX + x, baseY + y + 1) &&
                    BWAPI::Broodwar->isWalkable(baseX + x + 1, baseY + y + 1))
                {
                    return true;
                }
            }
        }

        return false;
    }

    // Same test for the current dynamic topology. Require a contiguous 2x2
    // block of mini-tiles: this rejects one-walk-tile cracks without imposing a
    // global clearance threshold that could incorrectly close legitimate ramps.
    bool hasDynamicPassage(const BWAPI::TilePosition & tile)
    {
        const int baseX = tile.x * 4;
        const int baseY = tile.y * 4;

        for (int x = 0; x < 3; ++x)
        {
            for (int y = 0; y < 3; ++y)
            {
                const BWAPI::WalkPosition a(baseX + x, baseY + y);
                const BWAPI::WalkPosition b(baseX + x + 1, baseY + y);
                const BWAPI::WalkPosition c(baseX + x, baseY + y + 1);
                const BWAPI::WalkPosition d(baseX + x + 1, baseY + y + 1);

                if (a.isValid() && b.isValid() && c.isValid() && d.isValid() &&
                    the.partitions.walkable(a) && the.partitions.walkable(b) &&
                    the.partitions.walkable(c) && the.partitions.walkable(d))
                {
                    return true;
                }
            }
        }

        return false;
    }
}

void MapTools::initialize()
{
    _allMaps.clear();
    _allMapsLastUse.clear();
    _allMapsUseClock = 0;
    _runtimeMaps.clear();
    _lastRuntimeUpdateFrame = -1;
    _runtimeRevision = 0;
    _pathWorkFrame = -1;
    _lastExpansionSafetyFrame = -100000;
    _expansionSafetyTopologyRevision = 0;
    _expansionSafetyScores.clear();
    _expansionSafetyDetails.clear();
    _expansionSafetyRoutes.clear();
    setBWAPIMapData();
}

void MapTools::refreshTopology()
{
    _allMaps.clear();
    _allMapsLastUse.clear();
    _runtimeMaps.clear();
    setBWAPIMapData();
    updateRuntimeWalkability(true);
    _lastExpansionSafetyFrame = -100000;
    _expansionSafetyScores.clear();
    _expansionSafetyDetails.clear();
    _expansionSafetyRoutes.clear();
}

// Read the current map data from BWAPI plus GrimHammer's dynamic 8x8 topology.
// A build tile is considered walkable when it contains a usable unit-center
// passage, not only when every one of its 16 walk tiles is clear.
void MapTools::setBWAPIMapData()
{
    const int mapWidth = BWAPI::Broodwar->mapWidth();
    const int mapHeight = BWAPI::Broodwar->mapHeight();

    _terrainWalkable = std::vector< std::vector<bool> >(mapWidth, std::vector<bool>(mapHeight, false));
    _walkable = std::vector< std::vector<bool> >(mapWidth, std::vector<bool>(mapHeight, false));
    _buildable = std::vector< std::vector<bool> >(mapWidth, std::vector<bool>(mapHeight, false));
    _depotBuildable = std::vector< std::vector<bool> >(mapWidth, std::vector<bool>(mapHeight, false));
    _runtimeBlocked = std::vector< std::vector<unsigned char> >(
        mapWidth, std::vector<unsigned char>(mapHeight, 0));
    _groundTransitions = std::vector< std::vector<unsigned char> >(
        mapWidth, std::vector<unsigned char>(mapHeight, 0));

    for (int x = 0; x < mapWidth; ++x)
    {
        for (int y = 0; y < mapHeight; ++y)
        {
            const BWAPI::TilePosition tile(x, y);
            const bool buildable = BWAPI::Broodwar->isBuildable(tile, false);

            _buildable[x][y] = buildable;
            _depotBuildable[x][y] = buildable;
            _terrainWalkable[x][y] = hasTerrainPassage(tile);
            _walkable[x][y] = hasDynamicPassage(tile);
        }
    }

    // Cache every static build-tile transition once. GridDistances visits most
    // tiles on the map and used to repeat the underlying 8x8 walk-tile border
    // checks for every new destination. When an army received its first attack
    // order, several distance fields could therefore consume hundreds of ms in
    // one frame. The static topology changes only after refreshTopology().
    const int actionX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    const int actionY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

    auto cardinalEdgeOpen = [](BWAPI::TilePosition a, BWAPI::TilePosition b) -> bool
    {
        const int dx = b.x - a.x;
        const int dy = b.y - a.y;
        if (std::abs(dx) + std::abs(dy) != 1)
        {
            return false;
        }

        int run = 0;
        int bestRun = 0;
        if (dx != 0)
        {
            const int ax = dx > 0 ? a.x * 4 + 3 : a.x * 4;
            const int bx = dx > 0 ? b.x * 4 : b.x * 4 + 3;
            for (int k = 0; k < 4; ++k)
            {
                const BWAPI::WalkPosition wa(ax, a.y * 4 + k);
                const BWAPI::WalkPosition wb(bx, a.y * 4 + k);
                const bool open = wa.isValid() && wb.isValid() &&
                    the.partitions.walkable(wa) && the.partitions.walkable(wb);
                run = open ? run + 1 : 0;
                bestRun = std::max(bestRun, run);
            }
        }
        else
        {
            const int ay = dy > 0 ? a.y * 4 + 3 : a.y * 4;
            const int by = dy > 0 ? b.y * 4 : b.y * 4 + 3;
            for (int k = 0; k < 4; ++k)
            {
                const BWAPI::WalkPosition wa(a.x * 4 + k, ay);
                const BWAPI::WalkPosition wb(a.x * 4 + k, by);
                const bool open = wa.isValid() && wb.isValid() &&
                    the.partitions.walkable(wa) && the.partitions.walkable(wb);
                run = open ? run + 1 : 0;
                bestRun = std::max(bestRun, run);
            }
        }

        return bestRun >= 2;
    };

    for (int x = 0; x < mapWidth; ++x)
    {
        for (int y = 0; y < mapHeight; ++y)
        {
            const BWAPI::TilePosition from(x, y);
            if (!_walkable[x][y])
            {
                continue;
            }

            for (int direction = 0; direction < 8; ++direction)
            {
                const BWAPI::TilePosition to(
                    x + actionX[direction],
                    y + actionY[direction]);

                if (!to.isValid() || !_walkable[to.x][to.y])
                {
                    continue;
                }

                const int dx = actionX[direction];
                const int dy = actionY[direction];
                bool open = false;
                if (dx == 0 || dy == 0)
                {
                    open = cardinalEdgeOpen(from, to);
                }
                else
                {
                    const BWAPI::TilePosition sideX(from.x + dx, from.y);
                    const BWAPI::TilePosition sideY(from.x, from.y + dy);
                    const bool viaX = sideX.isValid() && _walkable[sideX.x][sideX.y] &&
                        cardinalEdgeOpen(from, sideX) && cardinalEdgeOpen(sideX, to);
                    const bool viaY = sideY.isValid() && _walkable[sideY.x][sideY.y] &&
                        cardinalEdgeOpen(from, sideY) && cardinalEdgeOpen(sideY, to);
                    open = viaX || viaY;
                }

                if (open)
                {
                    _groundTransitions[x][y] |= static_cast<unsigned char>(1u << direction);
                }
            }
        }
    }

    // Remaining immobile static neutrals occupy build tiles. BWAPI's terrain
    // buildability query does not give us the dynamic topology semantics we want
    // here, so mark their exact current footprints explicitly. Resource fields
    // additionally reserve the normal 3-tile no-depot halo.
    for (BWAPI::Unit neutral : BWAPI::Broodwar->getStaticNeutralUnits())
    {
        if (!neutral || !neutral->exists() || neutral->getType().canMove() || neutral->isFlying())
        {
            continue;
        }

        const int tileX = neutral->getTilePosition().x;
        const int tileY = neutral->getTilePosition().y;
        const bool resource = neutral->getType().isResourceContainer();

        for (int x = tileX; x < tileX + neutral->getType().tileWidth(); ++x)
        {
            for (int y = tileY; y < tileY + neutral->getType().tileHeight(); ++y)
            {
                const BWAPI::TilePosition occupied(x, y);
                if (!occupied.isValid())
                {
                    continue;
                }

                _buildable[x][y] = false;

                if (resource)
                {
                    for (int dx = -3; dx <= 3; ++dx)
                    {
                        for (int dy = -3; dy <= 3; ++dy)
                        {
                            const BWAPI::TilePosition depotTile(x + dx, y + dy);
                            if (depotTile.isValid())
                            {
                                _depotBuildable[depotTile.x][depotTile.y] = false;
                            }
                        }
                    }
                }
            }
        }
    }

}

// Keep dynamic unit occupancy separate from terrain topology. A landed Factory
// should affect tactical movement immediately, but it must not redefine Areas,
// Chokes or base-front analysis. Recompute at a short interval; the map is
// tiny at build-tile resolution and the comparison avoids invalidating caches
// when nothing actually changed.
void MapTools::updateRuntimeWalkability(bool force)
{
    const int frame = BWAPI::Broodwar->getFrameCount();
    // Grounded buildings change rarely. Rebuilding a map-sized occupancy buffer
    // every second frame was measurable overhead in large mech games. Eight
    // frames is still fast enough to react to a lift/land without micro hitches.
    if (!force && _lastRuntimeUpdateFrame >= 0 && frame - _lastRuntimeUpdateFrame < 8)
    {
        return;
    }
    _lastRuntimeUpdateFrame = frame;
    const int mapWidth = BWAPI::Broodwar->mapWidth();
    const int mapHeight = BWAPI::Broodwar->mapHeight();
    std::vector< std::vector<unsigned char> > blocked(
        mapWidth, std::vector<unsigned char>(mapHeight, 0));

    auto markBuilding = [&blocked, mapWidth, mapHeight](
        BWAPI::TilePosition topLeft, BWAPI::UnitType type)
    {
        if (!topLeft.isValid() || !type.isBuilding())
        {
            return;
        }

        for (int x = std::max(0, topLeft.x); x < std::min(mapWidth, topLeft.x + type.tileWidth()); ++x)
        {
            for (int y = std::max(0, topLeft.y); y < std::min(mapHeight, topLeft.y + type.tileHeight()); ++y)
            {
                blocked[x][y] = 1;
            }
        }
    };

    // Our buildings are always known exactly, including construction and
    // lift/land state. Ignore flying Terran buildings.
    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->getType().isBuilding() ||
            unit->isFlying() || unit->isLifted() || unit->getOrder() == BWAPI::Orders::LiftingOff)
        {
            continue;
        }
        markBuilding(unit->getTilePosition(), unit->getType());
    }

    // Keep remembered enemy buildings as obstacles until InformationManager
    // confirms that their last location is empty. This prevents units from
    // repeatedly planning through a known bunker/factory under fog of war.
    for (const auto & kv : the.info.getUnitInfo(the.enemy()))
    {
        const UnitInfo & ui = kv.second;
        if (!ui.type.isBuilding() || ui.lifted || ui.goneFromLastPosition || !ui.lastPosition.isValid())
        {
            continue;
        }

        BWAPI::TilePosition topLeft = BWAPI::TilePositions::Invalid;
        if (ui.unit && ui.unit->isVisible() && ui.unit->getTilePosition().isValid())
        {
            topLeft = ui.unit->getTilePosition();
        }
        else
        {
            const BWAPI::Position upperLeft(
                ui.lastPosition.x - ui.type.tileWidth() * 16,
                ui.lastPosition.y - ui.type.tileHeight() * 16);
            topLeft = BWAPI::TilePosition(upperLeft);
        }

        markBuilding(topLeft, ui.type);
    }

    if (blocked != _runtimeBlocked)
    {
        _runtimeBlocked.swap(blocked);
        _runtimeMaps.clear();
        ++_runtimeRevision;
    }
}

BWAPI::TilePosition MapTools::nearestWalkableTile(
    BWAPI::TilePosition tile,
    bool runtimeBlocks,
    int maxRadius) const
{
    if (!tile.isValid())
    {
        return BWAPI::TilePositions::Invalid;
    }

    auto passable = [this, runtimeBlocks](BWAPI::TilePosition p)
    {
        return p.isValid() && (runtimeBlocks ? isRuntimeWalkable(p) : isWalkable(p));
    };

    if (passable(tile))
    {
        return tile;
    }

    int originalPartition = 0;
    const BWAPI::WalkPosition originalWalk(tile);
    if (originalWalk.isValid())
    {
        originalPartition = the.partitions.id(originalWalk);
    }

    for (int radius = 1; radius <= maxRadius; ++radius)
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

                const BWAPI::TilePosition candidate(tile.x + dx, tile.y + dy);
                if (!passable(candidate))
                {
                    continue;
                }

                if (originalPartition > 0)
                {
                    const BWAPI::WalkPosition walk(candidate);
                    if (!walk.isValid() || the.partitions.id(walk) != originalPartition)
                    {
                        continue;
                    }
                }

                const int score = std::abs(dx) + std::abs(dy);
                if (score < bestScore)
                {
                    bestScore = score;
                    best = candidate;
                }
            }
        }

        if (best.isValid())
        {
            return best;
        }
    }

    return BWAPI::TilePositions::Invalid;
}

bool MapTools::pathTransitionWalkable(
    BWAPI::TilePosition from,
    BWAPI::TilePosition to,
    bool runtimeBlocks) const
{
    return runtimeBlocks
        ? isGroundTransitionRuntimeWalkable(from, to)
        : isGroundTransitionWalkable(from, to);
}

// Ground distance in tiles, -1 if no path exists.
// Runtime distance fields are intentionally computed on the fast build-tile grid.
// High-resolution 8x8 analysis is owned by MapPartitions/GridInset/GridZone; keeping
// it out of this frequently-created cache prevents combat-time frame hitches.
int MapTools::getGroundTileDistance(BWAPI::TilePosition origin, BWAPI::TilePosition destination)
{
    if (!origin.isValid() || !destination.isValid())
    {
        return -1;
    }

    // Normalize blocked endpoints before consulting the symmetric cache. A
    // GridDistances field rooted inside an obstacle is represented by nearby
    // zero-distance seeds, so looking up the blocked tile in the reverse map
    // is not symmetric and can incorrectly return -1.
    origin = nearestWalkableTile(origin, false);
    destination = nearestWalkableTile(destination, false);
    if (!origin.isValid() || !destination.isValid())
    {
        return -1;
    }

    // Do we have a distance map to the destination?
    auto it = _allMaps.find(destination);
    if (it != _allMaps.end())
    {
        touchDistanceMap(destination);
        return (*it).second.at(origin);
    }

    // It's symmetrical. A distance map to the origin is just as good.
    it = _allMaps.find(origin);
    if (it != _allMaps.end())
    {
        touchDistanceMap(origin);
        return (*it).second.at(destination);
    }

    // Make a new map for this destination.
    notePathWork(1, 0, 0);
    ++_tileDistanceFields;
    evictDistanceMapIfFull();
    _allMaps.insert(std::pair<BWAPI::TilePosition, GridDistances>(destination, GridDistances(destination)));
    touchDistanceMap(destination);
    return _allMaps[destination].at(origin);
}

int MapTools::getGroundTileDistance(BWAPI::Position origin, BWAPI::Position destination)
{
    return getGroundTileDistance(BWAPI::TilePosition(origin), BWAPI::TilePosition(destination));
}

// Ground distance in pixels, -1 if no path exists. Choke/Area recognition remains 8x8; this hot-path distance cache is the fast build-tile approximation.
int MapTools::getGroundDistance(BWAPI::Position origin, BWAPI::Position destination)
{
    int tiles = getGroundTileDistance(origin, destination);
    if (tiles > 0)
    {
        return 32 * tiles;
    }
    return tiles;    // 0 or -1
}


// Check the REAL 8x8 crossing between two adjacent 32x32 build tiles.
// A cliff can leave walkable mini-tiles inside both build tiles while exposing
// no ground connection across their shared border. That was the remaining
// source of phantom shortcuts in the fast runtime distance field.
bool MapTools::isGroundTransitionWalkable(
    BWAPI::TilePosition from,
    BWAPI::TilePosition to) const
{
    if (!from.isValid() || !to.isValid())
    {
        return false;
    }

    const int dx = to.x - from.x;
    const int dy = to.y - from.y;
    if (dx == 0 && dy == 0)
    {
        return true;
    }
    if (std::abs(dx) > 1 || std::abs(dy) > 1)
    {
        return false;
    }

    int direction = -1;
    if (dx == 1 && dy == 0) direction = 0;
    else if (dx == -1 && dy == 0) direction = 1;
    else if (dx == 0 && dy == 1) direction = 2;
    else if (dx == 0 && dy == -1) direction = 3;
    else if (dx == 1 && dy == 1) direction = 4;
    else if (dx == 1 && dy == -1) direction = 5;
    else if (dx == -1 && dy == 1) direction = 6;
    else if (dx == -1 && dy == -1) direction = 7;

    return direction >= 0 &&
        (_groundTransitions[from.x][from.y] &
            static_cast<unsigned char>(1u << direction)) != 0;
}

bool MapTools::isGroundTransitionRuntimeWalkable(
    BWAPI::TilePosition from,
    BWAPI::TilePosition to) const
{
    if (!from.isValid() || !to.isValid() ||
        !isRuntimeWalkable(from) || !isRuntimeWalkable(to) ||
        !isGroundTransitionWalkable(from, to))
    {
        return false;
    }

    const int dx = to.x - from.x;
    const int dy = to.y - from.y;
    if (dx == 0 || dy == 0)
    {
        return true;
    }

    // Static terrain may allow a diagonal through either side of a corner.
    // When one side is occupied by a landed building, require the other side
    // to remain a real two-cardinal route instead of cutting through the
    // building corner.
    const BWAPI::TilePosition sideX(from.x + dx, from.y);
    const BWAPI::TilePosition sideY(from.x, from.y + dy);

    const bool viaX = sideX.isValid() && isRuntimeWalkable(sideX) &&
        isGroundTransitionWalkable(from, sideX) &&
        isGroundTransitionWalkable(sideX, to);
    const bool viaY = sideY.isValid() && isRuntimeWalkable(sideY) &&
        isGroundTransitionWalkable(from, sideY) &&
        isGroundTransitionWalkable(sideY, to);

    return viaX || viaY;
}

bool MapTools::isGroundPositionWalkable(
    BWAPI::Position position, BWAPI::UnitType body, bool runtimeBlocks) const
{
    const bool minimum = body == BWAPI::UnitTypes::None;
    const int left = position.x - (minimum ? 8 : body.dimensionLeft());
    const int right = position.x + (minimum ? 7 : body.dimensionRight());
    const int top = position.y - (minimum ? 8 : body.dimensionUp());
    const int bottom = position.y + (minimum ? 7 : body.dimensionDown());
    if (!position.isValid() || left < 0 || top < 0 ||
        right >= BWAPI::Broodwar->mapWidth() * 32 ||
        bottom >= BWAPI::Broodwar->mapHeight() * 32)
        return false;

    // isWalkable() from BWAPI only describes terrain. Fortress has closed,
    // invincible neutral doors standing on otherwise walkable ground.
    for (int x = left / 8; x <= right / 8; ++x)
        for (int y = top / 8; y <= bottom / 8; ++y)
            if (!the.partitions.walkable(x, y) ||
                (runtimeBlocks && _runtimeBlocked[x / 4][y / 4])) return false;
    return true;
}

BWAPI::Position MapTools::getGroundTileWaypoint(
    BWAPI::TilePosition tile, BWAPI::UnitType body, bool runtimeBlocks) const
{
    if (!tile.isValid()) return BWAPI::Positions::Invalid;
    const BWAPI::Position center = TileCenter(tile);
    // Prefer the ordinary center. Only a partial edge tile needs the bounded
    // nine-point search; never construct another full-map distance field here.
    static const int offsets[9][2] = {
        { 0, 0 }, { -8, 0 }, { 8, 0 }, { 0, -8 }, { 0, 8 },
        { -8, -8 }, { 8, -8 }, { -8, 8 }, { 8, 8 }
    };
    for (const auto& offset : offsets)
    {
        const BWAPI::Position point(center.x + offset[0], center.y + offset[1]);
        if (isGroundPositionWalkable(point, body, runtimeBlocks)) return point;
    }
    return BWAPI::Positions::Invalid;
}

bool MapTools::getGroundPath(
    BWAPI::TilePosition from,
    BWAPI::TilePosition to,
    std::vector<BWAPI::TilePosition> & path,
    int maxSteps,
    bool runtimeBlocks)
{
    notePathWork(0, 0, 1);
    path.clear();
    if (!from.isValid() || !to.isValid() || maxSteps <= 0)
    {
        return false;
    }

    const BWAPI::TilePosition start = nearestWalkableTile(from, runtimeBlocks);
    const BWAPI::TilePosition goal = nearestWalkableTile(to, runtimeBlocks);
    if (!start.isValid() || !goal.isValid())
    {
        return false;
    }

    if (start == goal)
    {
        path.push_back(start);
        return true;
    }

    // Strategic paths are calculated with weighted A*: cardinal movement costs
    // 10 and diagonal movement 14. This preserves the existing build-tile
    // performance profile while removing the old "diagonal == straight" bias
    // which could prefer visually triangular/needlessly diagonal approaches.
    const int mapWidth = BWAPI::Broodwar->mapWidth();
    const int mapHeight = BWAPI::Broodwar->mapHeight();
    const int cellCount = mapWidth * mapHeight;
    const int INF = INT_MAX / 4;

    auto indexOf = [mapWidth](const BWAPI::TilePosition & tile)
    {
        return tile.y * mapWidth + tile.x;
    };
    auto tileOf = [mapWidth](int index)
    {
        return BWAPI::TilePosition(index % mapWidth, index / mapWidth);
    };
    auto heuristic = [](const BWAPI::TilePosition & a, const BWAPI::TilePosition & b)
    {
        const int dx = std::abs(a.x - b.x);
        const int dy = std::abs(a.y - b.y);
        const int diagonal = std::min(dx, dy);
        const int straight = std::max(dx, dy) - diagonal;
        return 14 * diagonal + 10 * straight;
    };

    std::vector<int> gScore(cellCount, INF);
    std::vector<int> parent(cellCount, -1);
    std::vector<unsigned char> closed(cellCount, 0);

    typedef std::pair<int, int> QueueEntry; // f-score, flattened tile index
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry> > open;

    const int startIndex = indexOf(start);
    const int goalIndex = indexOf(goal);
    gScore[startIndex] = 0;
    parent[startIndex] = startIndex;
    open.push(QueueEntry(heuristic(start, goal), startIndex));

    const int actionX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    const int actionY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

    while (!open.empty())
    {
        const int currentIndex = open.top().second;
        open.pop();

        if (closed[currentIndex])
        {
            continue;
        }
        closed[currentIndex] = 1;

        if (currentIndex == goalIndex)
        {
            break;
        }

        const BWAPI::TilePosition current = tileOf(currentIndex);
        for (int a = 0; a < 8; ++a)
        {
            const BWAPI::TilePosition next(
                current.x + actionX[a],
                current.y + actionY[a]);

            if (!next.isValid() || !pathTransitionWalkable(current, next, runtimeBlocks))
            {
                continue;
            }

            const int nextIndex = indexOf(next);
            if (closed[nextIndex])
            {
                continue;
            }

            const bool diagonal = actionX[a] != 0 && actionY[a] != 0;
            const int tentative = gScore[currentIndex] + (diagonal ? 14 : 10);
            if (tentative >= gScore[nextIndex])
            {
                continue;
            }

            gScore[nextIndex] = tentative;
            parent[nextIndex] = currentIndex;
            open.push(QueueEntry(tentative + heuristic(next, goal), nextIndex));
        }
    }

    if (parent[goalIndex] < 0)
    {
        return false;
    }

    std::vector<BWAPI::TilePosition> reversePath;
    for (int index = goalIndex; ; index = parent[index])
    {
        reversePath.push_back(tileOf(index));
        if (index == startIndex)
        {
            break;
        }
        if (parent[index] < 0 || parent[index] == index)
        {
            path.clear();
            return false;
        }
    }

    std::reverse(reversePath.begin(), reversePath.end());
    const size_t maxTiles = std::min(reversePath.size(), size_t(maxSteps) + 1);
    path.assign(reversePath.begin(), reversePath.begin() + maxTiles);
    return true;
}

BWAPI::Position MapTools::getGroundPathWaypoint(
    BWAPI::Position from,
    BWAPI::Position to,
    int lookAheadTiles,
    bool runtimeBlocks,
    BWAPI::UnitType body)
{
    notePathWork(0, 1, 0);
    if (!from.isValid() || !to.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::TilePosition start = nearestWalkableTile(BWAPI::TilePosition(from), runtimeBlocks);
    const BWAPI::TilePosition goal = nearestWalkableTile(BWAPI::TilePosition(to), runtimeBlocks);
    if (!start.isValid() || !goal.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    // Keep the reusable distance gradient independent of temporary building
    // occupancy. Runtime maps used to be invalidated whenever any building was
    // started, completed, lifted, landed, discovered or forgotten. In mech
    // games that caused the same 9-15 ms full-map GridDistances fields to be
    // rebuilt over and over and made Fastest look like Normal speed.
    //
    // Dynamic blockers are still respected below for EVERY extracted step via
    // pathTransitionWalkable(..., runtimeBlocks). If a static gradient reaches
    // a newly blocked edge, the existing runtime A* detour handles that rare
    // local case. This preserves building avoidance without a full runtime
    // distance field per moving objective.
    std::map<BWAPI::TilePosition, GridDistances> & cache = _allMaps;
    auto it = cache.find(goal);
    if (it == cache.end())
    {
        notePathWork(1, 0, 0);
        ++_waypointFields;
        evictDistanceMapIfFull();
        it = cache.insert(std::pair<BWAPI::TilePosition, GridDistances>(
            goal, GridDistances(goal, true, false))).first;
    }
    touchDistanceMap(goal);

    const GridDistances & distances = it->second;
    int currentDistance = distances.at(start);
    if (currentDistance < 0)
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::TilePosition current = start;
    const int steps = std::max(1, lookAheadTiles);
    const int actionX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    const int actionY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

    for (int step = 0; step < steps && currentDistance > 0; ++step)
    {
        BWAPI::TilePosition best = BWAPI::TilePositions::Invalid;
        int bestDistance = currentDistance;
        int bestTie = INT_MAX;

        for (int a = 0; a < 8; ++a)
        {
            const BWAPI::TilePosition next(
                current.x + actionX[a],
                current.y + actionY[a]);
            if (!next.isValid() || !pathTransitionWalkable(current, next, runtimeBlocks))
            {
                continue;
            }

            const int d = distances.at(next);
            if (d < 0 || d >= currentDistance)
            {
                continue;
            }

            const int dx = std::abs(goal.x - next.x);
            const int dy = std::abs(goal.y - next.y);
            const int tie = 14 * std::min(dx, dy) + 10 * (std::max(dx, dy) - std::min(dx, dy));
            if (d < bestDistance || (d == bestDistance && tie < bestTie))
            {
                bestDistance = d;
                bestTie = tie;
                best = next;
            }
        }

        if (!best.isValid())
        {
            // A static route can become blocked by a newly landed building.
            // In a packed army many units can hit the same blocked distance
            // gradient in one frame. Each full-map A* costs tens of ms, so
            // spread those detours over eight frames and cap the work per
            // frame. The caller can use native movement to its final goal
            // until the scheduled detour runs.
            if (runtimeBlocks)
            {
                const int frame = BWAPI::Broodwar->getFrameCount();
                const int phase = (start.x * 3 + start.y * 5 + frame / 8) & 7;
                if (((frame + phase) & 7) == 0 && _groundAStarCalls < 4)
                {
                    std::vector<BWAPI::TilePosition> detour;
                    if (getGroundPath(current, goal, detour, steps - step, true) && detour.size() >= 2)
                    {
                        return getGroundTileWaypoint(detour.back(), body, runtimeBlocks);
                    }
                }
            }
            return BWAPI::Positions::Invalid;
        }

        current = best;
        currentDistance = bestDistance;
    }

    if (currentDistance == 0 && goal == BWAPI::TilePosition(to) &&
        isGroundPositionWalkable(to, body, runtimeBlocks))
    {
        return to;
    }
    // A passable build tile need not have a passable center, and a normalized
    // goal must not turn back into the original position inside an obstacle.
    return getGroundTileWaypoint(current, body, runtimeBlocks);
}

const std::vector<BWAPI::TilePosition> & MapTools::getClosestTilesTo(BWAPI::TilePosition pos)
{
    // Explicitly create a map rooted at pos. The symmetric-distance shortcut in
    // getGroundTileDistance() is valid for a single distance query, but it does
    // not guarantee that _allMaps[pos] itself exists for sorted-tile iteration.
    auto it = _allMaps.find(pos);
    if (it == _allMaps.end())
    {
        notePathWork(1, 0, 0);
        ++_placementFields;
        evictDistanceMapIfFull();
        it = _allMaps.insert(
            std::pair<BWAPI::TilePosition, GridDistances>(pos, GridDistances(pos))).first;
    }

    touchDistanceMap(pos);

    return it->second.getSortedTiles();
}

const std::vector<BWAPI::TilePosition> & MapTools::getClosestTilesTo(BWAPI::Position pos)
{
    return getClosestTilesTo(BWAPI::TilePosition(pos));
}

// TODO deprecated method
bool MapTools::isBuildable(BWAPI::TilePosition tile, BWAPI::UnitType type) const
{
    if (!tile.isValid())
    {
        return false;
    }

    int startX = tile.x;
    int endX = tile.x + type.tileWidth();
    int startY = tile.y;
    int endY = tile.y + type.tileHeight();

    for (int x = startX; x<endX; ++x)
    {
        for (int y = startY; y<endY; ++y)
        {
            BWAPI::TilePosition tile(x, y);

            if (!tile.isValid() || !isBuildable(tile) || type.isResourceDepot() && !isDepotBuildable(tile))
            {
                return false;
            }
        }
    }

    return true;
}

// Recompute the strategic part of expansion selection at a low cadence. The
// ordinary resource/occupancy checks in nextExpansion() remain authoritative;
// this cache only answers "how defendable and connected is this location now?".
// Keeping it here extends the existing expansion selector instead of creating a
// second ProductionManager policy which could disagree with reservations.
void MapTools::refreshExpansionSafetyScores() const
{
    const int now = the.now();
    const unsigned int topologyRevision = the.mapTopologyRevision();
    if (now >= _lastExpansionSafetyFrame &&
        now - _lastExpansionSafetyFrame < 48 &&
        topologyRevision == _expansionSafetyTopologyRevision)
    {
        return;
    }

    _lastExpansionSafetyFrame = now;
    _expansionSafetyTopologyRevision = topologyRevision;
    _expansionSafetyScores.clear();
    _expansionSafetyDetails.clear();

    BWAPI::Position ownArmyCenter = BWAPI::Positions::Invalid;
    long long ownX = 0;
    long long ownY = 0;
    int ownWeight = 0;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isCompleted() ||
            !unit->getPosition().isValid() || unit->getType().isWorker() ||
            unit->getType().isBuilding() || !UnitUtil::IsCombatUnit(unit))
        {
            continue;
        }
        const int weight = std::max(1, unit->getType().supplyRequired() / 2);
        ownX += static_cast<long long>(unit->getPosition().x) * weight;
        ownY += static_cast<long long>(unit->getPosition().y) * weight;
        ownWeight += weight;
    }
    if (ownWeight > 0)
    {
        ownArmyCenter = BWAPI::Position(
            int(ownX / ownWeight), int(ownY / ownWeight));
    }

    BWAPI::Position enemyArmyCenter = BWAPI::Positions::Invalid;
    long long enemyX = 0;
    long long enemyY = 0;
    int enemyWeight = 0;
    for (const auto & entry : the.info.getUnitInfo(the.enemy()))
    {
        const UnitInfo & enemy = entry.second;
        if (enemy.goneFromLastPosition || !enemy.lastPosition.isValid() ||
            enemy.type.isWorker() || enemy.type.isBuilding() ||
            enemy.type.isSpell() || !UnitUtil::IsCombatUnit(enemy.type) ||
            now - enemy.updateFrame > 45 * 24)
        {
            continue;
        }
        const int weight = std::max(1, enemy.type.supplyRequired() / 2);
        enemyX += static_cast<long long>(enemy.lastPosition.x) * weight;
        enemyY += static_cast<long long>(enemy.lastPosition.y) * weight;
        enemyWeight += weight;
    }
    if (enemyWeight > 0)
    {
        enemyArmyCenter = BWAPI::Position(
            int(enemyX / enemyWeight), int(enemyY / enemyWeight));
    }

    std::vector<Base *> ownedBases;
    for (Base * base : the.bases.getAll())
    {
        if (base && base->getOwner() == the.self() && base->getDepot() &&
            base->getDepot()->exists() && !base->getDepot()->isFlying())
        {
            ownedBases.push_back(base);
        }
    }

    Base * enemyMain = the.bases.enemyStart();
    for (Base * candidate : the.bases.getAll())
    {
        // nextExpansion() never scores occupied, reserved or depleted bases.
        // Avoid computing terrain routes for these locations as well.
        if (!candidate || !candidate->getCenter().isValid() ||
            candidate->getOwner() != BWAPI::Broodwar->neutral() ||
            candidate->isReserved() ||
            (candidate->getLastKnownMinerals() < 500 &&
             candidate->getInitialGas() == 0))
        {
            continue;
        }

        double score = 0.0;
        Base * routeBase = nullptr;
        int nearestOwnedDistance = MAX_DISTANCE;
        int connectedOwnedBases = 0;
        for (Base * owned : ownedBases)
        {
            // Ground distance is symmetric; put the fixed source in the
            // destination argument because MapTools creates a missing field
            // for that argument. All expansion candidates then share one
            // field per owned base, instead of one field per candidate.
            const int distance = the.map.getGroundDistance(
                candidate->getCenter(), owned->getCenter());
            if (distance < 0)
            {
                continue;
            }
            if (distance <= 30 * 32)
            {
                ++connectedOwnedBases;
            }
            if (distance < nearestOwnedDistance)
            {
                nearestOwnedDistance = distance;
                routeBase = owned;
            }
        }

        if (!routeBase)
        {
            _expansionSafetyScores[candidate->getID()] = -10000.0;
            _expansionSafetyDetails[candidate->getID()] = "disconnected";
            continue;
        }

        // Connected territory is preferable to isolated Command Centers. A
        // farther site can still win when enemy distance and route safety make
        // up for this deliberately moderate proximity term.
        score += 70.0 * std::min(3, connectedOwnedBases);
        score -= std::min(420.0, nearestOwnedDistance / 5.0);
        if (the.zone.sameArea(routeBase->getCenter(), candidate->getCenter()))
        {
            score += 80.0;
        }

        if (ownArmyCenter.isValid())
        {
            const int distance = ownArmyCenter.getApproxDistance(candidate->getCenter());
            score += std::max(-260.0, std::min(220.0,
                (26.0 * 32.0 - distance) / 4.0));
        }

        int enemyMainDistance = -1;
        if (enemyMain && enemyMain->getCenter().isValid())
        {
            // Reuse the enemy-main field for every candidate as well.
            enemyMainDistance = the.map.getGroundDistance(
                candidate->getCenter(), enemyMain->getCenter());
            if (enemyMainDistance < 0)
            {
                enemyMainDistance = enemyMain->getCenter().getApproxDistance(
                    candidate->getCenter());
            }
            score += std::max(-320.0, std::min(520.0,
                (enemyMainDistance - 15.0 * 32.0) / 4.0));
        }

        int enemyArmyDistance = -1;
        if (enemyArmyCenter.isValid())
        {
            enemyArmyDistance = enemyArmyCenter.getApproxDistance(
                candidate->getCenter());
            score += std::max(-520.0, std::min(420.0,
                (enemyArmyDistance - 13.0 * 32.0) / 3.0));
            if (enemyArmyDistance < 9 * 32)
            {
                score -= 700.0;
            }
        }

        const std::pair<int, int> routeKey(
            routeBase->getID(), candidate->getID());
        auto routeIt = _expansionSafetyRoutes.find(routeKey);
        if (routeIt == _expansionSafetyRoutes.end())
        {
            std::vector<BWAPI::TilePosition> route;
            // A failed search is cached as an empty route too. Static terrain
            // changes clear the cache in refreshTopology().
            the.map.getGroundPath(routeBase->getCenterTile(),
                candidate->getCenterTile(), route, 512, false);
            routeIt = _expansionSafetyRoutes.emplace(
                routeKey, std::move(route)).first;
        }
        int routeDanger = 0;
        if (!routeIt->second.empty())
        {
            const std::vector<BWAPI::TilePosition>& route = routeIt->second;
            for (size_t i = 2; i + 2 < route.size(); i += 3)
            {
                const BWAPI::TilePosition tile = route[i];
                routeDanger += 2 * the.groundHitsFixed.at(tile) +
                    the.groundHitsMobile.at(tile);
            }
        }
        else
        {
            routeDanger += 20;
        }
        score -= std::min(900, routeDanger * 38);

        const std::vector<const ChokePoint *> chokePath =
            the.zone.findChokePath(routeBase->getCenter(), candidate->getCenter());
        const int chokeCount = int(chokePath.size());
        score += 35.0 * std::min(2, chokeCount);
        score -= 30.0 * std::max(0, chokeCount - 3);

        const ChokePoint * frontChoke = candidate->getFrontChoke();
        if (frontChoke)
        {
            const int width = frontChoke->widthWalkTiles();
            score += 18.0 * std::max(0, 16 - width);
            score -= 20.0 * std::max(0, width - 20);
        }

        // Count a few 2x2 buildable anchors near the defensive front. They are
        // potential Turret/Tank support space, not reservations.
        const BWAPI::Position front = candidate->getFront().isValid()
            ? candidate->getFront() : candidate->getCenter();
        const BWAPI::TilePosition frontTile(front);
        int defenseAnchors = 0;
        for (int dx = -7; dx <= 7; dx += 2)
        {
            for (int dy = -7; dy <= 7; dy += 2)
            {
                const BWAPI::TilePosition tile(frontTile.x + dx, frontTile.y + dy);
                bool buildable = tile.isValid();
                for (int x = 0; x < 2 && buildable; ++x)
                    for (int y = 0; y < 2 && buildable; ++y)
                    {
                        const BWAPI::TilePosition cell(tile.x + x, tile.y + y);
                        buildable = cell.isValid() && the.map.isBuildable(cell);
                    }
                if (buildable)
                {
                    ++defenseAnchors;
                }
            }
        }
        score += 18.0 * std::min(10, defenseAnchors);

        score -= 220.0 * the.groundHitsFixed.at(candidate->getCenterTile());
        score -= 120.0 * the.groundHitsMobile.at(candidate->getCenterTile());
        score += 0.003 * candidate->getLastKnownMinerals();
        score += 0.006 * candidate->getLastKnownGas();

        _expansionSafetyScores[candidate->getID()] = score;
        _expansionSafetyDetails[candidate->getID()] =
            "S=" + std::to_string(int(score)) +
            " route=" + std::to_string(routeDanger) +
            " choke=" + std::to_string(chokeCount) +
            " own=" + std::to_string(connectedOwnedBases) +
            " e=" + std::to_string(enemyArmyDistance / 32);
    }
}

double MapTools::expansionSafetyScore(const Base * base) const
{
    refreshExpansionSafetyScores();
    if (!base)
    {
        return -10000.0;
    }
    const auto found = _expansionSafetyScores.find(base->getID());
    return found == _expansionSafetyScores.end() ? -10000.0 : found->second;
}

const std::string & MapTools::expansionSafetyDetail(const Base * base) const
{
    static const std::string unavailable("S=unavailable");
    refreshExpansionSafetyScores();
    if (!base)
    {
        return unavailable;
    }
    const auto found = _expansionSafetyDetails.find(base->getID());
    return found == _expansionSafetyDetails.end() ? unavailable : found->second;
}

void MapTools::drawHomeDistances()
{
    if (Config::Debug::DrawMapDistances)
    {
        the.bases.myMain()->getDistances().draw();
    }
}

// Make the assumption that we are looking for a mineral-only base.
void MapTools::drawExpoScores()
{
    if (Config::Debug::DrawExpoScores)
    {
        (void)nextExpansion(false, true, false);
    }
}

Base * MapTools::nextExpansion(bool hidden, bool wantMinerals, bool wantGas) const
{
    UAB_ASSERT(wantMinerals || wantGas, "unwanted expansion");
    refreshExpansionSafetyScores();

    const int now = the.now();
    if (_lastDeferredExpansionFrame > now)
    {
        _deferredExpansions.clear();
    }
    _lastDeferredExpansionFrame = now;

    // Abbreviations.
    BWAPI::Player player = BWAPI::Broodwar->self();
    BWAPI::Player enemy = BWAPI::Broodwar->enemy();
    BWAPI::Position offset(-24, -20);

    // We'll go through the bases and pick the one with the best score.
    Base * bestBase = nullptr;
    double bestScore = -999999.0;
    
    BWAPI::TilePosition homeTile = the.bases.myStart()->getTilePosition();
    BWAPI::Position myBasePosition(homeTile);

    for (Base * base : the.bases.getAll())
    {
        const auto deferred = _deferredExpansions.find(base->getTilePosition());
        if (deferred != _deferredExpansions.end() && now < deferred->second)
        {
            continue;
        }
        // Don't expand to an existing base, or a reserved base.
        if (base->getOwner() != BWAPI::Broodwar->neutral() || base->isReserved())
        {
			if (Config::Debug::DrawExpoScores)
			{
				BWAPI::Broodwar->drawTextMap(base->getCenter() + offset, "%cexpo: %ctaken", white, yellow);
			}
			continue;
        }

        // Do we demand a gas base?
        if (wantGas && base->getLastKnownGas() < 500)
        {
            if (Config::Debug::DrawExpoScores)
            {
                BWAPI::Broodwar->drawTextMap(base->getCenter()+offset, "%cexpo: %cno gas", white, red);
            }
            continue;
        }

        const int estimatedMinerals = base->getLastKnownMinerals();

        // Do we demand a mineral base?
        // The constant is an arbitrary limit "enough minerals to be worth it".
        if (wantMinerals && estimatedMinerals < 500)
        {
            if (Config::Debug::DrawExpoScores)
            {
                BWAPI::Broodwar->drawTextMap(base->getCenter()+offset, "%cexpo: %cno minerals", white, red);
            }
            continue;
        }

        // If this is to be a hidden base, and the enemy is not found yet, avoid mains and naturals.
        if (hidden && the.bases.enemyStart() == nullptr)
        {
            if (base->isAStartingBase() || base->getMain())
            {
                continue;
            }
        }

        BWAPI::TilePosition topLeft = base->getTilePosition();
        BWAPI::TilePosition bottomRight = topLeft + BWAPI::TilePosition(4, 3);

        // No good if the building location is known to be in range of enemy static defense.
        if (the.groundHitsFixed.inRange(player->getRace().getResourceDepot(), topLeft))
        {
            if (Config::Debug::DrawExpoScores)
            {
                BWAPI::Broodwar->drawTextMap(base->getCenter()+offset, "%cexpo: %cendangered", white, red);
            }
            continue;
        }

		// No good if no worker has a safe path to get there.
		// Getting a long distance worker checks this.
		if (!WorkerManager::Instance().getLongDistanceWorker(base->getPosition()))
		{
			continue;
		}

        // Terran construction clears removable obstacles inside the exact CC
        // footprint, including remembered mines. Nearby guards still affect
        // expansion selection; other races keep the original placement rules.
        bool somethingInTheWay = false;
        for (const auto & kv : InformationManager::Instance().getUnitData(BWAPI::Broodwar->enemy()).getUnits())
        {
            const UnitInfo & ui(kv.second);

            if (!ui.goneFromLastPosition)
            {
                const bool terranClearableObstacle =
                    player->getRace() == BWAPI::Races::Terran &&
                    ui.lastPosition.isValid() && !ui.lifted &&
                    !ui.type.isFlyer() && !ui.type.isInvincible() && !ui.type.isSpell() &&
                    ui.lastPosition.x + ui.type.dimensionRight() >= topLeft.x * 32 &&
                    ui.lastPosition.x - ui.type.dimensionLeft() < bottomRight.x * 32 &&
                    ui.lastPosition.y + ui.type.dimensionDown() >= topLeft.y * 32 &&
                    ui.lastPosition.y - ui.type.dimensionUp() < bottomRight.y * 32;
                if (terranClearableObstacle)
                {
                    // This exact blocker belongs to construction clearance,
                    // including remembered buried mines outside current vision.
                    continue;
                }
                if (ui.type.isBuilding() && !ui.lifted || ui.burrowed)
                {
                    BWAPI::TilePosition lastTilePos(ui.lastPosition);
                    if (lastTilePos.x >= topLeft.x &&
                        lastTilePos.x < bottomRight.x &&
                        lastTilePos.y >= topLeft.y &&
                        lastTilePos.y < bottomRight.y)
                    {
                        if (Config::Debug::DrawExpoScores)
                        {
                            BWAPI::Broodwar->drawTextMap(base->getCenter()+offset, "%cexpo: %cblocked by enemy", white, red);
                        }
                        somethingInTheWay = true;
                        goto buildingLoopExit;
                    }
                }
                if (the.now() - ui.updateFrame < 60 * 24 &&
                    UnitUtil::TypeCanAttackGround(ui.type) &&
                    base->getCenter().getApproxDistance(ui.lastPosition) <= 7 * 32 &&
                    !ui.type.isWorker() &&
                    ui.type != BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
                {
                    if (Config::Debug::DrawExpoScores)
                    {
                        BWAPI::Broodwar->drawTextMap(base->getCenter() + offset, "%cexpo: %cguarded by enemy", white, red);
                    }
                    somethingInTheWay = true;
                    goto buildingLoopExit;

                }
            }
        }

        // No good if a visible building is in the way.
        for (int x = 0; x < player->getRace().getResourceDepot().tileWidth(); ++x)
        {
            for (int y = 0; y < player->getRace().getResourceDepot().tileHeight(); ++y)
            {
                if (the.placer.isReserved(topLeft.x + x, topLeft.y + y))
                {
                    if (Config::Debug::DrawExpoScores)
                    {
                        BWAPI::Broodwar->drawTextMap(base->getCenter()+offset, "%cexpo: %creserved tiles", white, yellow);
                    }
                    // This happens if we were already planning to expand here. Try somewhere else.
                    somethingInTheWay = true;
                    goto buildingLoopExit;
                }

                // This knows about visible units only.
                for (BWAPI::Unit unit : BWAPI::Broodwar->getUnitsOnTile(BWAPI::TilePosition(topLeft.x + x, topLeft.y + y)))
                {
                    const bool terranClearableObstacle =
                        player->getRace() == BWAPI::Races::Terran &&
                        (unit->getPlayer() == enemy ||
                         unit->getPlayer() == BWAPI::Broodwar->neutral()) &&
                        (unit->getType().isMineralField() ||
                         (!unit->isInvincible() && !unit->getType().isResourceContainer()));
                    if (unit->getType().isBuilding() && !unit->isLifted() &&
                        !terranClearableObstacle)
                    {
                        if (Config::Debug::DrawExpoScores)
                        {
                            BWAPI::Broodwar->drawTextMap(base->getCenter()+offset, "%cexpo: %cblocked by building", white, red);
                        }
                        somethingInTheWay = true;
                        goto buildingLoopExit;
                    }
                }
            }
        }
buildingLoopExit:
        if (somethingInTheWay)
        {
            continue;
        }

        double score = 0.0;

        // NOTE Ground distances are computed at tile resolution, which is coarser than the walking
        // resolution. When a map has narrow chokes, two connected points may appear unconnected
        // by ground, meaning their distance is returned as -1. Map partitions are computed at walk
        // resolution, and we use those to decide whether a base is reachable. That can also be wrong
        // if the widest path between the points is too narrow for a worker. But it works for most
        // maps.
        // Anyway, if we get ground distance -1, we check connectivity and switch to air distance
        // as a backup.

        // Want to be close to our own base (unless this is to be a hidden base).
        // Pixel distance.
        int distanceFromUs = the.bases.myStart()->getDistance(topLeft);

        // If it is not connected by ground, skip this potential base.
        if (distanceFromUs < 0)
        {
            if (the.partitions.id(topLeft) == the.partitions.id(myBasePosition))
            {
                // It looks to be connected by a narrow path, use air distance.
                distanceFromUs = myBasePosition.getApproxDistance(BWAPI::Position(topLeft));
            }
            else
            {
                continue;
            }
        }

        // Want to be far from the enemy base.
        Base * enemyBase = the.bases.enemyStart();  // may be null or no longer enemy-owned
        // Pixel distance.
        int distanceFromEnemy = 0;
        if (enemyBase) {
            distanceFromEnemy = enemyBase->getDistance(topLeft);
            if (distanceFromEnemy < 0)
            {
                // No ground distance found, so again substitute air distance.
                BWAPI::TilePosition enemyTile = enemyBase->getTilePosition();
                if (the.partitions.id(topLeft) == the.partitions.id(enemyTile))
                {
                    distanceFromEnemy = enemyTile.getApproxDistance(topLeft);
                }
                else
                {
                    distanceFromEnemy = 0;
                }
            }
        }

        // Add up the score.
        score = hidden ? (distanceFromEnemy + distanceFromUs / 2) : (distanceFromEnemy / 2 - distanceFromUs);

        // Far from the edge of the map -> worse.
        // It's a proxy for "how wide open is this base?" Usually a base on the edge is
        // relatively sheltered and a base in the middle is more open (though not always).
        int edgeXdist = std::min(topLeft.x, BWAPI::Broodwar->mapWidth() - topLeft.x);
        int edgeYdist = std::min(topLeft.y, BWAPI::Broodwar->mapHeight() - topLeft.y);
        int edgeDistance = std::min(edgeXdist, edgeYdist);  // tile distance
        edgeDistance = std::max(0, edgeDistance - 10);      // within this distance is always OK
        if (edgeDistance > std::min(BWAPI::Broodwar->mapWidth(), BWAPI::Broodwar->mapHeight()) / 4)
        {
            // If we're very far from the edge, it's worse.
            edgeDistance *= 4;
        }
        score -= 15.0 * edgeDistance;

        // More resources -> better.
        // NOTE The number of mineral patchers/geysers controls how fast we can mine.
        //      The resource amount controls how long we can keep mining.
        if (wantMinerals)
        {
            score += 5.0 * base->getMinerals().size() + 0.005 * estimatedMinerals;
        }
        if (wantGas)
        {
            score += 20.0 * base->getGeysers().size() + 0.01 * base->getLastKnownGas();
        }
        else
        {
            // We didn't ask for gas, but it may be useful anyway.
            score += 5.0 * base->getGeysers().size() + 0.0025 * base->getLastKnownGas();
        }

        // Strategic safety complements, rather than replaces, the established
        // distance/resource formula. Hidden bases keep their special far-away
        // character, so safety is deliberately a smaller term for them.
        const double safety = expansionSafetyScore(base);
        score += safety * (hidden ? 0.35 : 1.0);

        /* TODO on a flat map, all mains may be in the same zone
        // Big penalty for enemy buildings in the same region.
        if (InformationManager::Instance().isEnemyBuildingInRegion(base->getRegion()))
        {
            score -= 100.0;
        }
        */

        // BWAPI::Broodwar->printf("base score %d, %d -> %f",  tile.x, tile.y, score);
        if (Config::Debug::DrawExpoScores)
        {
            BWAPI::Broodwar->drawTextMap(base->getCenter()+offset, "%cexpo: %c%g", white, green, score);
            BWAPI::Broodwar->drawTextMap(
                base->getCenter() + offset + BWAPI::Position(0, 12),
                "%c%s", cyan, expansionSafetyDetail(base).c_str());
        }

        if (score > bestScore)
        {
            bestBase = base;
            bestScore = score;
        }
    }

    if (bestBase)
    {
        return bestBase;
    }
    if (wantMinerals && wantGas)
    {
        // We wanted a gas base and there isn't one. Try for a mineral-only base.
        return nextExpansion(hidden, true, false);
    }
    return nullptr;
}

void MapTools::deferExpansion(BWAPI::TilePosition tile, int frames)
{
    if (tile.isValid())
    {
        _deferredExpansions[tile] = the.now() + frames;
    }
}

BWAPI::TilePosition MapTools::getNextExpansion(bool hidden, bool wantMinerals, bool wantGas) const
{
    Base * base = nextExpansion(hidden, wantMinerals, wantGas);
    if (base)
    {
        // BWAPI::Broodwar->printf("foresee base @ %d, %d", base->getTilePosition().x, base->getTilePosition().y);
        return base->getTilePosition();
    }
    return BWAPI::TilePositions::None;
}

BWAPI::TilePosition MapTools::reserveNextExpansion(bool hidden, bool wantMinerals, bool wantGas)
{
    Base * base = nextExpansion(hidden, wantMinerals, wantGas);
    if (base)
    {
        // BWAPI::Broodwar->printf("reserve base @ %d, %d", base->getTilePosition().x, base->getTilePosition().y);
        base->reserve();
        return base->getTilePosition();
    }
    return BWAPI::TilePositions::None;
}
