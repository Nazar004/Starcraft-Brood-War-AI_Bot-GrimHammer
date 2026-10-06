#include "Construction/BuildingPlacer.h"

#include "Map/Bases.h"
#include "Map/MapTools.h"
#include "Intelligence/Learning/OpponentModel.h"
#include "Core/The.h"
#include <climits>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <utility>

using namespace UAlbertaBot;

namespace
{
    struct WallBlock
    {
        BWAPI::UnitType type;
        BWAPI::TilePosition tile;

        WallBlock(BWAPI::UnitType t, const BWAPI::TilePosition & p)
            : type(t)
            , tile(p)
        {
        }
    };

    struct WallCandidate
    {
        BWAPI::TilePosition tile;
        BWAPI::Position center;
        double sideProjection;
        int score;
        int terrainSides;
    };

    struct WallCombination
    {
        int barracks;
        int depot1;
        int depot2;
        int score;
        bool centralGate;
        bool mapSeed = false;
    };

    struct WallMoverGrid
    {
        BWAPI::UnitType type;
        std::vector<unsigned char> terrainPassable;
        BWAPI::WalkPosition inside;
        BWAPI::WalkPosition outside;
        BWAPI::WalkPosition deepInside;
        BWAPI::WalkPosition deepOutside;
    };

    struct PixelBox
    {
        int left;
        int top;
        int right;
        int bottom;
    };

    struct WallCrossSection
    {
        bool valid = false;
        double x = 0.0;
        double y = 0.0;
        double sideX = 0.0;
        double sideY = 0.0;
        int width = 0;
    };

    // Measure both banks, rather than the distance from an edge-hugging A*
    // waypoint to its nearest bank. Pixel sampling uses the static walk grid.
    template<typename Walkable>
    WallCrossSection measureWallCrossSection(
        double x, double y, double forwardX, double forwardY,
        const Walkable & walkable)
    {
        WallCrossSection result;
        const double length = std::hypot(forwardX, forwardY);
        if (length < 1e-6) return result;
        result.sideX = -forwardY / length;
        result.sideY = forwardX / length;

        if (!walkable(int(x), int(y)))
        {
            // A route build tile can be partly walkable; its centre need not be.
            double nearest = 1e9;
            double bestX = x, bestY = y;
            for (int dx = -16; dx <= 16; dx += 4)
                for (int dy = -16; dy <= 16; dy += 4)
                    if (dx * dx + dy * dy < nearest &&
                        walkable(int(x + dx), int(y + dy)))
                    {
                        nearest = dx * dx + dy * dy;
                        bestX = x + dx;
                        bestY = y + dy;
                    }
            if (nearest == 1e9) return result;
            x = bestX;
            y = bestY;
        }

        int left = 0, right = 0;
        const int limit = 16 * 32;
        while (left < limit && walkable(
            int(std::lround(x - result.sideX * (left + 4))),
            int(std::lround(y - result.sideY * (left + 4))))) left += 4;
        while (right < limit && walkable(
            int(std::lround(x + result.sideX * (right + 4))),
            int(std::lround(y + result.sideY * (right + 4))))) right += 4;
        if (left == limit || right == limit) return result;

        result.x = x + result.sideX * (right - left) / 2.0;
        result.y = y + result.sideY * (right - left) / 2.0;
        result.width = left + right + 4;
        result.valid = walkable(int(result.x), int(result.y));
        return result;
    }

    BWAPI::Position wallBuildingCenter(
        BWAPI::UnitType type,
        const BWAPI::TilePosition & tile)
    {
        return BWAPI::Position(
            tile.x * 32 + type.tileWidth() * 16,
            tile.y * 32 + type.tileHeight() * 16);
    }

    PixelBox wallBuildingBox(const WallBlock & block)
    {
        const BWAPI::Position center =
            wallBuildingCenter(block.type, block.tile);

        PixelBox box;
        box.left = center.x - block.type.dimensionLeft();
        box.top = center.y - block.type.dimensionUp();
        box.right = center.x + block.type.dimensionRight();
        box.bottom = center.y + block.type.dimensionDown();
        return box;
    }

    bool wallFootprintsOverlap(
        BWAPI::UnitType firstType,
        const BWAPI::TilePosition & first,
        BWAPI::UnitType secondType,
        const BWAPI::TilePosition & second)
    {
        const int firstRight = first.x + firstType.tileWidth() - 1;
        const int firstBottom = first.y + firstType.tileHeight() - 1;
        const int secondRight = second.x + secondType.tileWidth() - 1;
        const int secondBottom = second.y + secondType.tileHeight() - 1;

        return !(
            firstRight < second.x || secondRight < first.x ||
            firstBottom < second.y || secondBottom < first.y);
    }
}

// Don't build in a position that blocks mining. Part of initialization.
void BuildingPlacer::reserveSpaceNearResources()
{
    for (Base * base : the.bases.getAll())
    {
        // A tile close to the center of the resource depot building (which is 4x3 tiles).
        BWAPI::TilePosition baseTile = base->getTilePosition() + BWAPI::TilePosition(2, 1);

        // NOTE This still allows the bot to block mineral mining of some patches, but it's relatively rare.
        for (BWAPI::Unit mineral : base->getMinerals())
        {
            if (!mineral || !mineral->exists())
            {
                continue;
            }
            BWAPI::TilePosition minTile = mineral->getTilePosition();
            if (minTile.x < baseTile.x)
            {
                setReserve(minTile + BWAPI::TilePosition(2, 0), 1, 1, ReserveResource, true);
            }
            else if (minTile.x > baseTile.x)
            {
                setReserve(minTile + BWAPI::TilePosition(-1, 0), 1, 1, ReserveResource, true);
            }
            if (minTile.y < baseTile.y)
            {
                setReserve(minTile + BWAPI::TilePosition(0, 1), 2, 1, ReserveResource, true);
            }
            else if (minTile.y > baseTile.y)
            {
                setReserve(minTile + BWAPI::TilePosition(0, -1), 2, 1, ReserveResource, true);
            }
        }

        for (BWAPI::Unit gas : base->getGeysers())
        {
            if (!gas || !gas->exists())
            {
                continue;
            }
            // Don't build on the right edge or a right corner of a geyser.
            // It causes workers to take a long path around. Other locations are OK.
            BWAPI::TilePosition gasTile = gas->getTilePosition();
            //reserveTiles(gasTile, 4, 2);
            if (gasTile.x - baseTile.x > 2)
            {
                setReserve(gasTile + BWAPI::TilePosition(-1, 1), 3, 2, ReserveResource, true);
            }
            else if (gasTile.x - baseTile.x < -2)
            {
                setReserve(gasTile + BWAPI::TilePosition(2, -1), 3, 2, ReserveResource, true);
            }
            if (gasTile.y - baseTile.y > 2)
            {
                setReserve(gasTile + BWAPI::TilePosition(-1, -1), 2, 3, ReserveResource, true);
            }
            else if (gasTile.y - baseTile.y < -2)
            {
                setReserve(gasTile + BWAPI::TilePosition(3, 0), 2, 3, ReserveResource, true);
            }
        }
    }
}

// Reserve a one-build-tile movement lane from our important base interiors
// toward their terrain front. This is intentionally a light-weight BWEB-style
// safety rail, not a replacement for BWEB Blocks: production can pack tightly
// around the lane, but cannot place a building directly on the route used by
// tanks/vultures to leave the base.
void BuildingPlacer::reserveBaseMovementLanes()
{
    std::vector<Base *> bases;
    for (Base * base : the.bases.getAll())
    {
        if (!base)
        {
            continue;
        }

        // BWEB Stations protect every occupied base. Do the same for movement
        // corridors, while also reserving our natural before it is taken so an
        // early forward building cannot seal the intended expansion exit.
        if (base->getOwner() == the.self() ||
            base == the.bases.myMain() || base == the.bases.myNatural())
        {
            bases.push_back(base);
        }
    }

    for (Base * base : bases)
    {
        if (!base || !base->getCenterTile().isValid() || !base->getFrontTile().isValid())
        {
            continue;
        }

        std::vector<BWAPI::TilePosition> path;
        if (!the.map.getGroundPath(base->getCenterTile(), base->getFrontTile(), path, 48, false) || path.size() < 5)
        {
            continue;
        }

        // Do not reserve the depot footprint itself and stop shortly before the
        // actual front/choke so bunker/turret placement still has freedom there.
        const size_t begin = std::min<size_t>(3, path.size());
        const size_t end = path.size() > 3 ? path.size() - 3 : path.size();
        for (size_t i = begin; i < end; ++i)
        {
            setReserve(path[i], 1, 1, ReserveLane, true);
        }
    }
}

// Once there is no active Barracks gate, keep the whole main-to-natural
// corridor open. The short base-interior lanes above deliberately stop before
// the choke, so by themselves they cannot protect a released wall from a
// returning lifted Barracks.
void BuildingPlacer::reserveOpenMainExitLane()
{
    if (the.selfRace() != BWAPI::Races::Terran ||
        (_terranMainWallValid && !_terranMainWallReleased))
    {
        return;
    }

    const Base * main = the.bases.myMain();
    const Base * natural = the.bases.myNatural();
    if (!main || !natural || !main->getCenterTile().isValid() ||
        !natural->getCenterTile().isValid())
    {
        return;
    }

    std::vector<BWAPI::TilePosition> path;
    if (!the.map.getGroundPath(main->getCenterTile(), natural->getCenterTile(),
        path, 128, true) || path.size() < 7)
    {
        // The Barracks can still be grounded on the frame when the wall is
        // released. Reserve the terrain route now so another building cannot
        // claim the exit before the Barracks has lifted.
        path.clear();
        if (!the.map.getGroundPath(main->getCenterTile(), natural->getCenterTile(),
            path, 128, false) || path.size() < 7)
        {
            return;
        }
    }

    for (size_t i = 3; i + 3 < path.size(); ++i)
    {
        setReserve(path[i], 1, 1, ReserveLane, true);
    }
}

// Find a Terran main wall during map initialization (and once more if a
// destructible neutral changes the topology). GridZone supplies the preferred
// strategic choke, while an independent route/clearance pass is the fallback.
// A candidate must block the matchup's smallest ground body (both Probe and
// Zergling for Random/Unknown), and open a usable Tank gate when the Barracks
// is removed. Nothing in this analysis depends on fog of war.
void BuildingPlacer::findTerranMainWall(
    bool forceRouteFallback, int sectionAttempt, bool naturalFallback)
{
    _terranMainWallValid = false;
    _terranMainWallBarracksID = -1;
    _terranMainWallOpened = false;
    _terranMainWallReleased = false;
    _terranMainWallCentralGate = false;
    _terranMainWallCoversNatural = false;
    _terranMainWallUsedRouteFallback = false;
    _terranMainWallUsedDestructibleRoute = false;
    _terranMainWallStatus = "WALL: checking map";
    _terranMainWallBarracks = BWAPI::TilePositions::None;
    _terranMainWallDepot1 = BWAPI::TilePositions::None;
    _terranMainWallDepot2 = BWAPI::TilePositions::None;
    _terranMainWallInside = BWAPI::Positions::Invalid;
    _terranMainWallOutside = BWAPI::Positions::Invalid;
    _terranMainWallAnalysisCenter = BWAPI::Positions::Invalid;
    _terranMainWallChokeEndA = BWAPI::Positions::Invalid;
    _terranMainWallChokeEndB = BWAPI::Positions::Invalid;

    auto reportFailure = [this](const std::string & message)
    {
        _terranMainWallStatus = message;
        BWAPI::Broodwar->printf("%s", _terranMainWallStatus.c_str());
    };

    if (the.selfRace() != BWAPI::Races::Terran ||
        !BWAPI::Broodwar->enemy())
    {
        reportFailure("WALL: disabled (not Terran/no enemy)");
        return;
    }

    // A Terran wall is worthwhile only against a confirmed or strongly
    // predicted early ground rush. A small historical risk can still justify
    // a Bunker, but must not lock the mech army behind a wall.
    const BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();
    if (enemyRace == BWAPI::Races::Terran)
    {
        const OpponentModel & model = OpponentModel::Instance();
        const OpeningPlan observed = model.getEnemyPlan();
        const bool observedRush =
            observed == OpeningPlan::WorkerRush ||
            observed == OpeningPlan::FastRush ||
            observed == OpeningPlan::HeavyRush;
        const bool predictedRush =
            model.stronglyPredictsEnemyPlan(OpeningPlan::WorkerRush) ||
            model.stronglyPredictsEnemyPlan(OpeningPlan::FastRush) ||
            model.stronglyPredictsEnemyPlan(OpeningPlan::HeavyRush) ||
            model.stronglyPredictsEarlyGroundRush();
        if (!observedRush && !predictedRush)
        {
            reportFailure("WALL: no confident Terran rush");
            return;
        }
    }

    // A known Protoss wall needs to stop Probes, not the smaller Zergling.
    // Keep both tests until a Random opponent's race is known.
    const bool stopProbe = enemyRace != BWAPI::Races::Zerg;
    const bool stopZergling = enemyRace != BWAPI::Races::Protoss &&
        enemyRace != BWAPI::Races::Terran;

    Base * main = the.bases.myMain();
    Base * natural = the.bases.myNatural();
    if (!main)
    {
        reportFailure("WALL: no main base");
        return;
    }

    const int mapWidth = BWAPI::Broodwar->mapWidth();
    const int mapHeight = BWAPI::Broodwar->mapHeight();
    const int walkWidth = mapWidth * 4;
    const int walkHeight = mapHeight * 4;
    const int walkCellCount = walkWidth * walkHeight;

    // This second walkability view describes terrain after destroyable map
    // blockers are opened. Resources and invincible neutrals remain blocked.
    // It is used only when the live main->natural topology is disconnected,
    // notably on Benzene-like starts. Current normal routes continue to use
    // MapPartitions, so a still-closed rear door is not treated as an opening.
    std::vector<unsigned char> openedTerrainWalkable;
    auto ensureOpenedTerrainWalkable = [&]()
    {
        if (!openedTerrainWalkable.empty()) return;
        openedTerrainWalkable.assign(size_t(walkCellCount), 0);
        for (int y = 0; y < walkHeight; ++y)
            for (int x = 0; x < walkWidth; ++x)
                if (BWAPI::Broodwar->isWalkable(x, y))
                    openedTerrainWalkable[size_t(y) * walkWidth + size_t(x)] = 1;

        for (BWAPI::Unit neutral : BWAPI::Broodwar->getStaticNeutralUnits())
        {
            if (!neutral || !neutral->exists() || neutral->getType().canMove() ||
                neutral->isFlying() || neutral->getType().isSpell()) continue;
            if (!neutral->getType().isResourceContainer() &&
                neutral->getType() != BWAPI::UnitTypes::Resource_Vespene_Geyser &&
                !neutral->isInvincible()) continue;
            for (int x = neutral->getLeft() / 8; x <= neutral->getRight() / 8; ++x)
                for (int y = neutral->getTop() / 8; y <= neutral->getBottom() / 8; ++y)
                    if (x >= 0 && y >= 0 && x < walkWidth && y < walkHeight)
                        openedTerrainWalkable[size_t(y) * walkWidth + size_t(x)] = 0;
        }
    };

    auto openedTilePassable =
        [&openedTerrainWalkable, &ensureOpenedTerrainWalkable, walkWidth, walkHeight](
            const BWAPI::TilePosition & tile) -> bool
    {
        ensureOpenedTerrainWalkable();
        if (!tile.isValid())
        {
            return false;
        }

        const int baseX = tile.x * 4;
        const int baseY = tile.y * 4;
        for (int dx = 0; dx < 3; ++dx)
        {
            for (int dy = 0; dy < 3; ++dy)
            {
                const int x = baseX + dx;
                const int y = baseY + dy;
                if (x < 0 || y < 0 || x + 1 >= walkWidth ||
                    y + 1 >= walkHeight)
                {
                    continue;
                }

                const size_t a = size_t(y) * walkWidth + size_t(x);
                const size_t b = a + 1;
                const size_t c = a + size_t(walkWidth);
                const size_t d = c + 1;
                if (openedTerrainWalkable[a] && openedTerrainWalkable[b] &&
                    openedTerrainWalkable[c] && openedTerrainWalkable[d])
                {
                    return true;
                }
            }
        }
        return false;
    };

    auto nearestOpenedTile =
        [&openedTilePassable](const BWAPI::TilePosition & wanted)
        -> BWAPI::TilePosition
    {
        for (int radius = 0; radius <= 16; ++radius)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                for (int dy = -radius; dy <= radius; ++dy)
                {
                    if (radius > 0 &&
                        std::abs(dx) != radius && std::abs(dy) != radius)
                    {
                        continue;
                    }

                    const BWAPI::TilePosition tile(
                        wanted.x + dx, wanted.y + dy);
                    if (openedTilePassable(tile))
                    {
                        return tile;
                    }
                }
            }
        }
        return BWAPI::TilePositions::Invalid;
    };

    auto getOpenedTerrainPath =
        [mapWidth, mapHeight, &openedTilePassable, &nearestOpenedTile](
            const BWAPI::TilePosition & from,
            const BWAPI::TilePosition & to,
            std::vector<BWAPI::TilePosition> & path) -> bool
    {
        path.clear();
        const BWAPI::TilePosition start = nearestOpenedTile(from);
        const BWAPI::TilePosition goal = nearestOpenedTile(to);
        if (!start.isValid() || !goal.isValid())
        {
            return false;
        }

        const int cellCount = mapWidth * mapHeight;
        const int infinity = INT_MAX / 4;
        auto indexOf = [mapWidth](const BWAPI::TilePosition & tile)
        {
            return tile.y * mapWidth + tile.x;
        };
        auto tileOf = [mapWidth](int index)
        {
            return BWAPI::TilePosition(index % mapWidth, index / mapWidth);
        };
        auto heuristic = [](const BWAPI::TilePosition & a,
                            const BWAPI::TilePosition & b)
        {
            const int dx = std::abs(a.x - b.x);
            const int dy = std::abs(a.y - b.y);
            const int diagonal = std::min(dx, dy);
            return diagonal * 14 + (std::max(dx, dy) - diagonal) * 10;
        };

        std::vector<int> cost(size_t(cellCount), infinity);
        std::vector<int> parent(size_t(cellCount), -1);
        std::vector<unsigned char> closed(size_t(cellCount), 0);
        typedef std::pair<int, int> QueueEntry;
        std::priority_queue<
            QueueEntry,
            std::vector<QueueEntry>,
            std::greater<QueueEntry> > open;

        const int startIndex = indexOf(start);
        const int goalIndex = indexOf(goal);
        cost[size_t(startIndex)] = 0;
        parent[size_t(startIndex)] = startIndex;
        open.push(QueueEntry(heuristic(start, goal), startIndex));

        const int actionX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
        const int actionY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
        while (!open.empty())
        {
            const int currentIndex = open.top().second;
            open.pop();
            if (closed[size_t(currentIndex)])
            {
                continue;
            }
            closed[size_t(currentIndex)] = 1;
            if (currentIndex == goalIndex)
            {
                break;
            }

            const BWAPI::TilePosition current = tileOf(currentIndex);
            for (int action = 0; action < 8; ++action)
            {
                const BWAPI::TilePosition next(
                    current.x + actionX[action],
                    current.y + actionY[action]);
                if (!openedTilePassable(next))
                {
                    continue;
                }

                if (action >= 4)
                {
                    const BWAPI::TilePosition sideX(
                        current.x + actionX[action], current.y);
                    const BWAPI::TilePosition sideY(
                        current.x, current.y + actionY[action]);
                    if (!openedTilePassable(sideX) &&
                        !openedTilePassable(sideY))
                    {
                        continue;
                    }
                }

                const int nextIndex = indexOf(next);
                const int nextCost =
                    cost[size_t(currentIndex)] + (action >= 4 ? 14 : 10);
                if (nextCost >= cost[size_t(nextIndex)])
                {
                    continue;
                }

                cost[size_t(nextIndex)] = nextCost;
                parent[size_t(nextIndex)] = currentIndex;
                open.push(QueueEntry(
                    nextCost + heuristic(next, goal), nextIndex));
            }
        }

        if (parent[size_t(goalIndex)] < 0)
        {
            return false;
        }

        for (int index = goalIndex; ; index = parent[size_t(index)])
        {
            path.push_back(tileOf(index));
            if (index == startIndex)
            {
                break;
            }
            if (parent[size_t(index)] < 0 ||
                parent[size_t(index)] == index)
            {
                path.clear();
                return false;
            }
        }
        std::reverse(path.begin(), path.end());
        return true;
    };

    std::vector<BWAPI::TilePosition> route;
    if (natural)
    {
        the.map.getGroundPath(
            main->getCenterTile(), natural->getCenterTile(),
            route, 512, false);
    }

    if (route.size() < 11)
    {
        route.clear();
        _terranMainWallUsedDestructibleRoute = true;

        if (natural)
        {
            getOpenedTerrainPath(
                main->getCenterTile(), natural->getCenterTile(), route);
        }

        // A neutral blocker can make Bases deliberately leave myNatural null.
        // Pick the closest resource base in the opened-terrain model instead
        // of abandoning wall analysis altogether.
        if (route.size() < 11)
        {
            std::vector<BWAPI::TilePosition> bestRoute;
            int bestScore = MAX_DISTANCE;
            for (Base * candidate : the.bases.getAll())
            {
                if (!candidate || candidate == main ||
                    candidate->isAStartingBase() ||
                    candidate->getInitialMinerals() <= 0)
                {
                    continue;
                }

                std::vector<BWAPI::TilePosition> candidateRoute;
                if (!getOpenedTerrainPath(
                        main->getCenterTile(), candidate->getCenterTile(),
                        candidateRoute) ||
                    candidateRoute.size() < 11)
                {
                    continue;
                }

                int score = int(candidateRoute.size()) * 10;
                if (candidate->getInitialGas() <= 0)
                {
                    score *= 2;
                }
                if (score < bestScore)
                {
                    bestScore = score;
                    bestRoute.swap(candidateRoute);
                }
            }
            route.swap(bestRoute);
        }

        if (route.size() < 11)
        {
            reportFailure("WALL: no usable route from main");
            return;
        }
    }

    const std::vector<BWAPI::TilePosition> routeToNatural = route;

    // A main->natural path normally stops in the middle of the natural and
    // therefore never exposes the natural's OUTWARD choke to the wall search.
    // Extend the same proven ground route through that explicit GridZone choke.
    // The exact wall validator below still has to prove that a layout closes all
    // alternate routes, so this only adds a candidate and cannot weaken a wall.
    const ChokePoint * naturalFrontChoke =
        natural ? natural->getFrontChoke() : nullptr;
    if (naturalFrontChoke && naturalFrontChoke->center().isValid())
    {
        std::vector<BWAPI::TilePosition> bestExtendedRoute;
        int bestExtendedScore = MAX_DISTANCE;
        for (Base * destination : the.bases.getAll())
        {
            if (!destination || destination == main || destination == natural ||
                destination->getInitialMinerals() <= 0)
            {
                continue;
            }

            std::vector<BWAPI::TilePosition> candidate;
            const bool found = _terranMainWallUsedDestructibleRoute
                ? getOpenedTerrainPath(
                    main->getCenterTile(), destination->getCenterTile(), candidate)
                : the.map.getGroundPath(
                    main->getCenterTile(), destination->getCenterTile(),
                    candidate, 512, false);
            if (!found || candidate.size() < route.size() + 6)
            {
                continue;
            }

            int chokeIndex = -1;
            int chokeDistance = MAX_DISTANCE;
            for (size_t i = 0; i < candidate.size(); ++i)
            {
                const int distance = TileCenter(candidate[i]).getApproxDistance(
                    naturalFrontChoke->center());
                if (distance < chokeDistance)
                {
                    chokeDistance = distance;
                    chokeIndex = int(i);
                }
            }
            if (chokeIndex < 5 || chokeIndex + 6 >= int(candidate.size()) ||
                chokeDistance > 3 * 32)
            {
                continue;
            }

            // Prefer a short route which definitely continues beyond the
            // natural choke. Enemy start is a useful tie-breaker once known.
            int score = int(candidate.size()) * 8 + chokeDistance;
            if (destination == the.bases.enemyStart())
            {
                score -= 128;
            }
            if (score < bestExtendedScore)
            {
                bestExtendedScore = score;
                bestExtendedRoute.swap(candidate);
            }
        }
        if (!bestExtendedRoute.empty())
        {
            route.swap(bestExtendedRoute);
        }
    }

    // A main->natural route can end BEFORE the actual exit (Heartbreak Ridge).
    // After that search fails, examine the route out toward another start. A
    // small cliff pocket on the way to our own natural is not a choke.
    if (forceRouteFallback)
    {
        std::vector<BWAPI::TilePosition> extended;
        for (Base * destination : the.bases.getAll())
        {
            if (!destination || destination == main || !destination->isAStartingBase()) continue;
            std::vector<BWAPI::TilePosition> candidate;
            if (_terranMainWallUsedDestructibleRoute)
                getOpenedTerrainPath(main->getCenterTile(), destination->getCenterTile(), candidate);
            else
                the.map.getGroundPath(main->getCenterTile(), destination->getCenterTile(), candidate, 512, false);
            if (candidate.size() > route.size() &&
                (extended.empty() || candidate.size() < extended.size())) extended.swap(candidate);
        }
        if (!extended.empty()) route.swap(extended);
    }

    if (naturalFallback)
    {
        // The ordinary route often ends at the natural's Command Center. It
        // cannot reveal the outward passage. Search a second leg from the
        // natural toward the nearest reachable base beyond its front choke.
        if (!natural || routeToNatural.empty() ||
            TileCenter(routeToNatural.back()).getApproxDistance(
                natural->getCenter()) > 4 * 32)
        {
            reportFailure("WALL: no route through natural");
            return;
        }

        std::vector<BWAPI::TilePosition> bestOutsideLeg;
        int bestScore = MAX_DISTANCE;
        for (Base * destination : the.bases.getAll())
        {
            if (!destination || destination == main || destination == natural ||
                destination->getInitialMinerals() <= 0)
            {
                continue;
            }
            std::vector<BWAPI::TilePosition> leg;
            const bool found = _terranMainWallUsedDestructibleRoute
                ? getOpenedTerrainPath(natural->getCenterTile(),
                    destination->getCenterTile(), leg)
                : the.map.getGroundPath(natural->getCenterTile(),
                    destination->getCenterTile(), leg, 512, false);
            if (!found || leg.size() < 12) continue;

            int chokeDistance = 0;
            if (naturalFrontChoke && naturalFrontChoke->center().isValid())
            {
                chokeDistance = MAX_DISTANCE;
                for (const BWAPI::TilePosition & tile : leg)
                {
                    chokeDistance = std::min(chokeDistance,
                        TileCenter(tile).getApproxDistance(
                            naturalFrontChoke->center()));
                }
                if (chokeDistance > 3 * 32) continue;
            }
            const int score = int(leg.size()) * 8 + chokeDistance;
            if (score < bestScore)
            {
                bestScore = score;
                bestOutsideLeg.swap(leg);
            }
        }
        if (bestOutsideLeg.empty())
        {
            reportFailure("WALL: no outward route from natural");
            return;
        }
        route = routeToNatural;
        route.insert(route.end(), bestOutsideLeg.begin() + 1,
            bestOutsideLeg.end());
    }

    const ChokePoint * mainFrontChoke = main->getFrontChoke();
    auto findChokeOnRoute = [&route](const ChokePoint * choke,
                                     int & index, int & distance)
    {
        index = -1;
        distance = MAX_DISTANCE;
        if (!choke || !choke->center().isValid())
        {
            return;
        }
        for (size_t i = 0; i < route.size(); ++i)
        {
            const int candidateDistance =
                TileCenter(route[i]).getApproxDistance(choke->center());
            if (candidateDistance < distance)
            {
                distance = candidateDistance;
                index = int(i);
            }
        }
    };

    int mainChokeRouteIndex = -1;
    int mainChokeRouteDistance = MAX_DISTANCE;
    int naturalChokeRouteIndex = -1;
    int naturalChokeRouteDistance = MAX_DISTANCE;
    findChokeOnRoute(
        mainFrontChoke, mainChokeRouteIndex, mainChokeRouteDistance);
    findChokeOnRoute(
        naturalFrontChoke, naturalChokeRouteIndex, naturalChokeRouteDistance);

    int naturalCenterRouteIndex = -1;
    int naturalCenterRouteDistance = MAX_DISTANCE;
    if (natural)
    {
        for (size_t i = 0; i < route.size(); ++i)
        {
            const int distance = TileCenter(route[i]).getApproxDistance(
                natural->getCenter());
            if (distance < naturalCenterRouteDistance)
            {
                naturalCenterRouteDistance = distance;
                naturalCenterRouteIndex = int(i);
            }
        }
    }

    const bool mainTopologyChokeUsable =
        !naturalFallback && !forceRouteFallback &&
        !_terranMainWallUsedDestructibleRoute &&
        mainChokeRouteIndex >= 5 &&
        mainChokeRouteIndex + 5 < int(route.size()) &&
        mainChokeRouteDistance <= 3 * 32;
    const bool naturalTopologyChokeUsable =
        !forceRouteFallback &&
        !_terranMainWallUsedDestructibleRoute &&
        naturalChokeRouteIndex >= 5 &&
        naturalChokeRouteIndex + 5 < int(route.size()) &&
        naturalChokeRouteDistance <= 3 * 32;
    const bool topologyChokeUsable =
        mainTopologyChokeUsable || naturalTopologyChokeUsable;

    int sectionCount = 0;
    auto retryUsingRouteChoke =
        [this, forceRouteFallback, sectionAttempt, naturalFallback,
         natural, &sectionCount]() -> bool
    {
        const BWAPI::Position center = _terranMainWallAnalysisCenter;
        const BWAPI::Position bankA = _terranMainWallChokeEndA;
        const BWAPI::Position bankB = _terranMainWallChokeEndB;
        bool retried = false;
        if (sectionAttempt + 1 < std::min(sectionCount, 2))
        {
            findTerranMainWall(forceRouteFallback, sectionAttempt + 1,
                naturalFallback);
            retried = true;
        }
        else if (!forceRouteFallback)
        {
            findTerranMainWall(true, 0, naturalFallback);
            retried = true;
        }
        else if (!naturalFallback && natural)
        {
            findTerranMainWall(false, 0, true);
            retried = true;
        }
        // When every placement fails, draw the best real passage, not the
        // last low-ranked fallback examined on another height/side of the ramp.
        if (retried && !_terranMainWallValid && center.isValid())
        {
            _terranMainWallAnalysisCenter = center;
            _terranMainWallChokeEndA = bankA;
            _terranMainWallChokeEndB = bankB;
        }
        return retried;
    };

    auto terrainWalkable = [&](int x, int y) -> bool
    {
        if (x < 0 || y < 0 || x >= mapWidth * 32 || y >= mapHeight * 32)
            return false;
        return _terranMainWallUsedDestructibleRoute
            ? openedTerrainWalkable[size_t(y / 8) * walkWidth + size_t(x / 8)] != 0
            : the.partitions.walkable(x / 8, y / 8);
    };

    const int mainHeight = GroundHeight(main->getCenterTile());
    int rampIndex = -1;
    int routeSampleLimit = naturalFallback
        ? std::min(int(route.size()) - 6,
            int(routeToNatural.size()) + 52)
        : (forceRouteFallback ? 64 : 40);
    if (naturalTopologyChokeUsable)
    {
        routeSampleLimit = std::max(
            routeSampleLimit,
            std::min(72, naturalChokeRouteIndex + 7));
    }
    const int lastRouteSample =
        std::min(int(route.size()) - 6, routeSampleLimit);
    for (int i = 5; i <= lastRouteSample; ++i)
    {
        if (GroundHeight(route[size_t(i)]) != mainHeight)
        {
            rampIndex = i;
            break;
        }
    }
    if (naturalFallback) rampIndex = -1;

    // A strategic choke is a hint, not an exact building anchor. Re-centre
    // each cross-section between its two terrain banks. On the fallback path
    // prefer the first elevation transition out of the main; never equate
    // "close to one cliff" with "narrow passage".
    WallCrossSection throat;
    int selectedIndex = -1;
    double forwardX = 0.0, forwardY = 0.0;
    struct SectionChoice
    {
        WallCrossSection section;
        int index;
        int score;
        double fx;
        double fy;
        bool mainLine;
        bool naturalLine;
        int supportTiles;
    };
    std::vector<SectionChoice> sections;
    const int firstSample = 5;
    const int lastSample = lastRouteSample;
    for (int i = firstSample; i <= lastSample; ++i)
    {
        if (naturalFallback &&
            (naturalCenterRouteIndex < 0 ||
             i <= naturalCenterRouteIndex + 3 ||
             i > naturalCenterRouteIndex + 48))
        {
            continue;
        }
        const BWAPI::Position before = TileCenter(route[size_t(i - 2)]);
        const BWAPI::Position after = TileCenter(route[size_t(i + 2)]);
        const double dx = double(after.x - before.x);
        const double dy = double(after.y - before.y);
        const double length = std::hypot(dx, dy);
        if (length < 1.0) continue;
        const BWAPI::Position point = TileCenter(route[size_t(i)]);
        const int maximumWallDistance =
            naturalTopologyChokeUsable ? 48 * 32 : 32 * 32;
        if (naturalFallback)
        {
            if (point.getApproxDistance(natural->getCenter()) > 28 * 32 ||
                (naturalFrontChoke && naturalFrontChoke->center().isValid() &&
                 point.getApproxDistance(naturalFrontChoke->center()) > 8 * 32))
            {
                continue;
            }
        }
        else if (point.getApproxDistance(main->getCenter()) > maximumWallDistance)
        {
            continue;
        }
        // Look for the banks across the local corridor, not perpendicular to
        // a chord spanning a turn several tiles before/after this ramp.
        for (int turn = -3; turn <= 3; ++turn)
        {
            const double angle = turn * 3.14159265358979323846 / 12.0;
            const double fx = (dx * std::cos(angle) - dy * std::sin(angle)) / length;
            const double fy = (dx * std::sin(angle) + dy * std::cos(angle)) / length;
            const WallCrossSection section = measureWallCrossSection(
                point.x, point.y, fx, fy, terrainWalkable);
            if (!section.valid || section.width < 32 || section.width > 14 * 32)
                continue;
            // A diagonal across the corner next to the Command Center can
            // separate the route too, but encloses the depot instead of the
            // base exit. Do not promote that pocket when the real ramp fails.
            if (BWAPI::Position(int(section.x), int(section.y)).getApproxDistance(main->getCenter()) < 5 * 32)
                continue;
            // Reject a ray across a shallow pocket beside the route: the
            // middle of the measured section must have a forward approach.
            if (!terrainWalkable(int(section.x - fx * 24), int(section.y - fy * 24)) ||
                !terrainWalkable(int(section.x + fx * 24), int(section.y + fy * 24)))
                continue;
            // Compare adjacent sections: one stair-step corner can make an
            // oblique ray look shorter than the actual corridor normal.
            // A small angular error grows when we step back to build.
            int widthSum = section.width;
            for (int offset : { -24, 24 })
            {
                const WallCrossSection adjacent = measureWallCrossSection(
                    point.x + fx * offset, point.y + fy * offset,
                    fx, fy, terrainWalkable);
                widthSum += adjacent.valid ? adjacent.width : section.width + 128;
            }
            int score = widthSum * 4 / 3 + i * 2 + std::abs(turn) * 48;
            if (rampIndex >= 0) score += std::abs(i - rampIndex) * 24;
            const bool mainLine = mainTopologyChokeUsable &&
                std::abs(i - mainChokeRouteIndex) <= 7;
            const bool naturalLine = naturalTopologyChokeUsable &&
                std::abs(i - naturalChokeRouteIndex) <= 7 &&
                (naturalCenterRouteIndex < 0 || i > naturalCenterRouteIndex);
            if (topologyChokeUsable)
            {
                int topologyPenalty = MAX_DISTANCE;
                if (mainTopologyChokeUsable)
                    topologyPenalty = std::min(
                        topologyPenalty,
                        std::abs(i - mainChokeRouteIndex) * 20);
                if (naturalTopologyChokeUsable)
                    topologyPenalty = std::min(
                        topologyPenalty,
                        std::abs(i - naturalChokeRouteIndex) * 20 + 64);
                score += topologyPenalty;
            }

            // Estimate room on the protected side for a Bunker, Turrets and a
            // small Siege line. Exact footprints are validated after a layout
            // wins; this cheap count only ranks equally narrow chokes.
            const int sectionHeight = GroundHeight(
                route[size_t(std::max(0, i - 3))]);
            int supportTiles = 0;
            for (int backTiles = 2; backTiles <= 9; ++backTiles)
            {
                for (int sideTiles = -6; sideTiles <= 6; sideTiles += 2)
                {
                    const BWAPI::Position support(
                        int(section.x - fx * backTiles * 32 +
                            section.sideX * sideTiles * 32),
                        int(section.y - fy * backTiles * 32 +
                            section.sideY * sideTiles * 32));
                    const BWAPI::TilePosition tile(support);
                    if (tile.isValid() && the.map.isBuildable(tile) &&
                        the.map.isWalkable(tile) &&
                        GroundHeight(tile) == sectionHeight)
                    {
                        ++supportTiles;
                    }
                }
            }
            score += std::max(0, 22 - supportTiles) * 18;
            sections.push_back({ section, i, score, fx, fy,
                                 mainLine, naturalLine, supportTiles });
        }
    }

    // Prefer the natural line only when its raw defensive quality is close to
    // the best main line and there is enough protected buildable room. A wider
    // or cramped natural remains a candidate only after the main alternatives.
    int bestMainSection = MAX_DISTANCE;
    int bestNaturalSection = MAX_DISTANCE;
    for (const SectionChoice & choice : sections)
    {
        if (choice.mainLine)
            bestMainSection = std::min(bestMainSection, choice.score);
        if (choice.naturalLine && choice.section.width <= 11 * 32 &&
            choice.supportTiles >= 16)
            bestNaturalSection = std::min(bestNaturalSection, choice.score);
    }
    const bool preferNaturalLine =
        bestNaturalSection < MAX_DISTANCE &&
        (bestMainSection == MAX_DISTANCE ||
         bestNaturalSection <= bestMainSection + 128);
    for (SectionChoice & choice : sections)
    {
        if (choice.naturalLine)
        {
            choice.score += preferNaturalLine &&
                choice.section.width <= 11 * 32 &&
                choice.supportTiles >= 16
                ? -224 : 256;
        }
    }
    std::stable_sort(sections.begin(), sections.end(),
        [](const SectionChoice & a, const SectionChoice & b) { return a.score < b.score; });

    // A cross-section must separate the two route ends even before buildings
    // are considered. This rejects short rays through a side pocket or a cliff
    // corner. It is a search filter only: exact unit/pixel validation stays below.
    auto separatesRoute = [&](const SectionChoice & choice)
    {
        auto passable = [&](int x, int y)
        {
            const double dx = x * 8 + 4 - choice.section.x;
            const double dy = y * 8 + 4 - choice.section.y;
            const bool barrier = std::abs(dx * choice.fx + dy * choice.fy) <= 12 &&
                std::abs(dx * choice.section.sideX + dy * choice.section.sideY) <= choice.section.width / 2 + 12;
            return !barrier && terrainWalkable(x * 8 + 4, y * 8 + 4);
        };
        auto seed = [&](BWAPI::TilePosition tile)
        {
            const BWAPI::WalkPosition p(TileCenter(tile));
            for (int r = 0; r <= 8; ++r)
                for (int dx = -r; dx <= r; ++dx)
                    for (int dy = -r; dy <= r; ++dy)
                    {
                        const int x = p.x + dx, y = p.y + dy;
                        if (x >= 0 && y >= 0 && x < walkWidth && y < walkHeight && passable(x, y))
                            return y * walkWidth + x;
                    }
            return -1;
        };
        const int start = seed(route.front()), goal = seed(route.back());
        if (start < 0 || goal < 0 || start == goal) return false;
        std::vector<unsigned char> seen(size_t(walkCellCount), 0);
        std::vector<int> open{ start };
        seen[size_t(start)] = 1;
        for (size_t at = 0; at < open.size(); ++at)
        {
            const int cell = open[at], x = cell % walkWidth, y = cell / walkWidth;
            if (cell == goal) return false;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                {
                    const int nx = x + dx, ny = y + dy;
                    if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= walkWidth || ny >= walkHeight) continue;
                    const int next = ny * walkWidth + nx;
                    if (!seen[size_t(next)] && passable(nx, ny))
                    { seen[size_t(next)] = 1; open.push_back(next); }
                }
        }
        return true;
    };
    std::vector<SectionChoice> distinct;
    int separationTests = 0;
    for (const SectionChoice & candidate : sections)
    {
        bool duplicate = false;
        int nearbyOrientations = 0;
        for (const SectionChoice & prior : distinct)
        {
            const bool samePlace = std::abs(candidate.index - prior.index) <= 4 ||
                std::hypot(candidate.section.x - prior.section.x,
                    candidate.section.y - prior.section.y) < 96;
            if (!samePlace) continue;
            ++nearbyOrientations;
            // On diagonal ramps the shortest ray is often a poor building
            // axis. Preserve another orientation of the same passage so its
            // candidate footprints get a chance at the exact wall tests.
            const double alignment = candidate.fx * prior.fx +
                candidate.fy * prior.fy;
            if (alignment > 0.87 || nearbyOrientations >= 2)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        if (++separationTests > 24) break;
        if (!separatesRoute(candidate)) continue;
        distinct.push_back(candidate);
        if (sectionCount++ == sectionAttempt)
        {
            throat = candidate.section;
            selectedIndex = candidate.index;
            forwardX = candidate.fx;
            forwardY = candidate.fy;
        }
        if (sectionCount >= 2) break;
    }
    if (selectedIndex < 0)
    {
        if (retryUsingRouteChoke()) return;
        reportFailure("WALL: no two-bank main passage");
        return;
    }
    const int chokeRouteIndex = selectedIndex;
    // Always use the height on the protected side of the selected passage.
    // Requiring mainHeight silently rejected every legitimate natural wall on
    // maps where the natural lies one level below the main.
    const int wallHeight =
        GroundHeight(route[size_t(std::max(0, selectedIndex - 3))]);
    _terranMainWallUsedRouteFallback = !topologyChokeUsable;
    const double sideX = throat.sideX;
    const double sideY = throat.sideY;
    _terranMainWallChokeEndA = BWAPI::Position(
        int(throat.x - sideX * throat.width / 2),
        int(throat.y - sideY * throat.width / 2));
    _terranMainWallChokeEndB = BWAPI::Position(
        int(throat.x + sideX * throat.width / 2),
        int(throat.y + sideY * throat.width / 2));

    // Move back from the ramp to the first main-side shoulder that has room
    // for a complete Barracks footprint. A walkable ramp tile is not buildable.
    BWAPI::Position chokeCenter(int(throat.x), int(throat.y));
    bool foundBuildableShoulder = false;
    const BWAPI::UnitType barracksType = BWAPI::UnitTypes::Terran_Barracks;
    const BWAPI::UnitType depotType = BWAPI::UnitTypes::Terran_Supply_Depot;
    for (int back = 0; back <= 6 * 32 && !foundBuildableShoulder; back += 8)
    {
        // Keep the throat axis fixed. Recentring each larger cross-section
        // behind the ramp moved the anchor toward the middle of the plateau.
        const BWAPI::Position center(
            int(std::lround(throat.x - forwardX * back)),
            int(std::lround(throat.y - forwardY * back)));
        if (!terrainWalkable(center.x, center.y)) continue;
        const BWAPI::TilePosition centerTile(center);
        if (!centerTile.isValid() || !the.map.isBuildable(centerTile) ||
            GroundHeight(centerTile) != wallHeight) continue;
        for (int dx = -2; dx <= 2 && !foundBuildableShoulder; ++dx)
        {
            for (int dy = -2; dy <= 2 && !foundBuildableShoulder; ++dy)
            {
                const BWAPI::TilePosition tile(centerTile.x - 2 + dx, centerTile.y - 1 + dy);
                bool fits = tile.isValid() && tile.x + 4 <= mapWidth && tile.y + 3 <= mapHeight;
                for (int x = tile.x; x < tile.x + 4 && fits; ++x)
                    for (int y = tile.y; y < tile.y + 3 && fits; ++y)
                        fits = the.map.isBuildable(BWAPI::TilePosition(x, y)) &&
                            GroundHeight(x, y) == wallHeight;
                if (fits)
                {
                    chokeCenter = center;
                    foundBuildableShoulder = true;
                }
            }
        }
    }
    _terranMainWallAnalysisCenter = chokeCenter;
    if (!foundBuildableShoulder)
    {
        if (retryUsingRouteChoke()) return;
        reportFailure("WALL: no buildable main-side shoulder");
        return;
    }

    // Use the main itself for the inner endpoint. A near seed can fall under
    // an otherwise valid wall, especially when the ramp approach bends.
    const int sideSteps = 9;
    const BWAPI::Position insidePosition = TileCenter(route.front());
    const BWAPI::Position outsidePosition = TileCenter(
        route[size_t(std::min(int(route.size()) - 1, chokeRouteIndex + sideSteps))]);
    const BWAPI::Position deepInsidePosition = TileCenter(route.front());
    const BWAPI::Position deepOutsidePosition = TileCenter(route.back());
    // These two points define gate orientation for runtime crossing requests;
    // path-validation seeds stay independent of this local geometric axis.
    _terranMainWallInside = BWAPI::Position(
        int(chokeCenter.x - forwardX * 64), int(chokeCenter.y - forwardY * 64));
    _terranMainWallOutside = BWAPI::Position(
        int(chokeCenter.x + forwardX * 64), int(chokeCenter.y + forwardY * 64));

    // Integral image of unwalkable 8x8 cells.  It lets us inflate terrain by
    // each moving unit's real collision dimensions in O(1) per walk cell.
    const int prefixWidth = walkWidth + 1;
    std::vector<int> blockedPrefix(
        size_t(prefixWidth) * size_t(walkHeight + 1), 0);
    for (int y = 0; y < walkHeight; ++y)
    {
        int rowBlocked = 0;
        for (int x = 0; x < walkWidth; ++x)
        {
            const bool passable = _terranMainWallUsedDestructibleRoute
                ? openedTerrainWalkable[
                    size_t(y) * walkWidth + size_t(x)] != 0
                : the.partitions.walkable(x, y);
            if (!passable)
            {
                ++rowBlocked;
            }
            blockedPrefix[size_t(y + 1) * prefixWidth + size_t(x + 1)] =
                blockedPrefix[size_t(y) * prefixWidth + size_t(x + 1)] +
                rowBlocked;
        }
    }

    auto rectangleBlockedCount =
        [&blockedPrefix, prefixWidth](int left, int top, int right, int bottom)
    {
        return
            blockedPrefix[size_t(bottom + 1) * prefixWidth + size_t(right + 1)] -
            blockedPrefix[size_t(top) * prefixWidth + size_t(right + 1)] -
            blockedPrefix[size_t(bottom + 1) * prefixWidth + size_t(left)] +
            blockedPrefix[size_t(top) * prefixWidth + size_t(left)];
    };

    auto makeMoverGrid =
        [&](BWAPI::UnitType mover) -> WallMoverGrid
    {
        WallMoverGrid grid;
        grid.type = mover;
        grid.terrainPassable.assign(size_t(walkCellCount), 0);
        grid.inside = BWAPI::WalkPosition(-1, -1);
        grid.outside = BWAPI::WalkPosition(-1, -1);
        grid.deepInside = BWAPI::WalkPosition(-1, -1);
        grid.deepOutside = BWAPI::WalkPosition(-1, -1);

        const int mapPixelWidth = walkWidth * 8;
        const int mapPixelHeight = walkHeight * 8;
        for (int y = 0; y < walkHeight; ++y)
        {
            for (int x = 0; x < walkWidth; ++x)
            {
                const int centerX = x * 8 + 4;
                const int centerY = y * 8 + 4;
                const int leftPixel = centerX - mover.dimensionLeft();
                const int rightPixel = centerX + mover.dimensionRight();
                const int topPixel = centerY - mover.dimensionUp();
                const int bottomPixel = centerY + mover.dimensionDown();

                if (leftPixel < 0 || topPixel < 0 ||
                    rightPixel >= mapPixelWidth || bottomPixel >= mapPixelHeight)
                {
                    continue;
                }

                const int leftWalk = leftPixel / 8;
                const int rightWalk = rightPixel / 8;
                const int topWalk = topPixel / 8;
                const int bottomWalk = bottomPixel / 8;
                if (rectangleBlockedCount(
                        leftWalk, topWalk, rightWalk, bottomWalk) == 0)
                {
                    grid.terrainPassable[size_t(y) * walkWidth + size_t(x)] = 1;
                }
            }
        }

        auto nearestPassable =
            [&](const BWAPI::WalkPosition & wanted) -> BWAPI::WalkPosition
        {
            for (int radius = 0; radius <= 24; ++radius)
            {
                for (int dx = -radius; dx <= radius; ++dx)
                {
                    for (int dy = -radius; dy <= radius; ++dy)
                    {
                        if (radius > 0 &&
                            std::abs(dx) != radius && std::abs(dy) != radius)
                        {
                            continue;
                        }

                        const int x = wanted.x + dx;
                        const int y = wanted.y + dy;
                        if (x >= 0 && y >= 0 &&
                            x < walkWidth && y < walkHeight &&
                            grid.terrainPassable[size_t(y) * walkWidth + size_t(x)])
                        {
                            return BWAPI::WalkPosition(x, y);
                        }
                    }
                }
            }
            return BWAPI::WalkPosition(-1, -1);
        };

        grid.inside = nearestPassable(BWAPI::WalkPosition(insidePosition));
        grid.outside = nearestPassable(BWAPI::WalkPosition(outsidePosition));
        grid.deepInside = nearestPassable(
            BWAPI::WalkPosition(deepInsidePosition));
        grid.deepOutside = nearestPassable(
            BWAPI::WalkPosition(deepOutsidePosition));
        return grid;
    };

    WallMoverGrid probeGrid =
        makeMoverGrid(BWAPI::UnitTypes::Protoss_Probe);
    WallMoverGrid zerglingGrid =
        makeMoverGrid(BWAPI::UnitTypes::Zerg_Zergling);
    WallMoverGrid scvGrid =
        makeMoverGrid(BWAPI::UnitTypes::Terran_SCV);
    WallMoverGrid tankGrid =
        makeMoverGrid(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode);

    auto moverSeedsValid = [](const WallMoverGrid & grid)
    {
        return grid.inside.isValid() && grid.outside.isValid() &&
            grid.deepInside.isValid() && grid.deepOutside.isValid();
    };
    if (!moverSeedsValid(probeGrid) ||
        !moverSeedsValid(zerglingGrid) ||
        !moverSeedsValid(scvGrid) ||
        !moverSeedsValid(tankGrid))
    {
        if (retryUsingRouteChoke())
        {
            return;
        }
        reportFailure("WALL: unit-sized route seeds failed");
        return;
    }

    const BWAPI::WalkPosition chokeWalk(chokeCenter);
    const int localRadius = 15 * 4;

    auto pathExists =
        [&](const WallMoverGrid & grid,
            const std::vector<WallBlock> & blocks,
            bool localOnly, bool pixelPrecise = false,
            BWAPI::WalkPosition from = BWAPI::WalkPosition(-1, -1),
            BWAPI::WalkPosition to = BWAPI::WalkPosition(-1, -1)) -> bool
    {
        BWAPI::WalkPosition start = from.isValid() ? from
            : (localOnly ? grid.inside : grid.deepInside);
        BWAPI::WalkPosition goal = to.isValid() ? to
            : (localOnly ? grid.outside : grid.deepOutside);

        int minX = 0;
        int minY = 0;
        int maxX = walkWidth - 1;
        int maxY = walkHeight - 1;

        if (localOnly)
        {
            minX = std::max(0, std::min(
                chokeWalk.x - localRadius,
                std::min(start.x, goal.x) - 4));
            minY = std::max(0, std::min(
                chokeWalk.y - localRadius,
                std::min(start.y, goal.y) - 4));
            maxX = std::min(walkWidth - 1, std::max(
                chokeWalk.x + localRadius,
                std::max(start.x, goal.x) + 4));
            maxY = std::min(walkHeight - 1, std::max(
                chokeWalk.y + localRadius,
                std::max(start.y, goal.y) + 4));
        }

        PixelBox forbidden[3];
        int forbiddenCount = 0;
        for (const WallBlock & block : blocks)
        {
            if (forbiddenCount >= 3)
            {
                return false;
            }
            const PixelBox building = wallBuildingBox(block);
            PixelBox centerForbidden;
            centerForbidden.left =
                building.left - grid.type.dimensionRight();
            centerForbidden.right =
                building.right + grid.type.dimensionLeft();
            centerForbidden.top =
                building.top - grid.type.dimensionDown();
            centerForbidden.bottom =
                building.bottom + grid.type.dimensionUp();
            forbidden[forbiddenCount++] = centerForbidden;
        }

        std::vector<int> pixelX, pixelY;
        if (pixelPrecise)
        {
            // Exact integer-pixel collision space, compressed into intervals
            // of constant passability. Terrain edges lie on 8px boundaries;
            // adding mover extents and building edges preserves even a 1px
            // legal centre lane, without a full 4096x4096 pixel flood fill.
            auto makeAxis = [&](int lo, int hi, int walkSize, int negativeExtent,
                                int positiveExtent, bool horizontal)
            {
                const int first = lo * 8;
                const int end = (hi + 1) * 8;
                std::vector<int> axis{ first, end };
                auto add = [&](int value)
                {
                    if (value > first && value < end) axis.push_back(value);
                };
                for (int k = 0; k <= walkSize; ++k)
                {
                    add(k * 8 - positiveExtent);
                    add(k * 8 + negativeExtent);
                }
                for (int i = 0; i < forbiddenCount; ++i)
                {
                    add(horizontal ? forbidden[i].left : forbidden[i].top);
                    add((horizontal ? forbidden[i].right : forbidden[i].bottom) + 1);
                }
                std::sort(axis.begin(), axis.end());
                axis.erase(std::unique(axis.begin(), axis.end()), axis.end());
                return axis;
            };
            pixelX = makeAxis(minX, maxX, walkWidth,
                grid.type.dimensionLeft(), grid.type.dimensionRight(), true);
            pixelY = makeAxis(minY, maxY, walkHeight,
                grid.type.dimensionUp(), grid.type.dimensionDown(), false);
            auto intervalAt = [](const std::vector<int> & axis, int pixel)
            {
                return int(std::upper_bound(axis.begin(), axis.end(), pixel) - axis.begin()) - 1;
            };
            start = BWAPI::WalkPosition(intervalAt(pixelX, start.x * 8 + 4),
                intervalAt(pixelY, start.y * 8 + 4));
            goal = BWAPI::WalkPosition(intervalAt(pixelX, goal.x * 8 + 4),
                intervalAt(pixelY, goal.y * 8 + 4));
            minX = minY = 0;
            maxX = int(pixelX.size()) - 2;
            maxY = int(pixelY.size()) - 2;
        }

        const int regionWidth = maxX - minX + 1;
        const int regionHeight = maxY - minY + 1;
        if (regionWidth <= 0 || regionHeight <= 0)
        {
            return false;
        }

        auto passable = [&](int x, int y) -> bool
        {
            if (x < minX || y < minY || x > maxX || y > maxY)
            {
                return false;
            }

            const int centerX = pixelPrecise ? pixelX[size_t(x)] : x * 8 + 4;
            const int centerY = pixelPrecise ? pixelY[size_t(y)] : y * 8 + 4;
            if (pixelPrecise)
            {
                const int left = centerX - grid.type.dimensionLeft();
                const int right = centerX + grid.type.dimensionRight();
                const int top = centerY - grid.type.dimensionUp();
                const int bottom = centerY + grid.type.dimensionDown();
                if (left < 0 || top < 0 || right >= walkWidth * 8 || bottom >= walkHeight * 8 ||
                    rectangleBlockedCount(left / 8, top / 8, right / 8, bottom / 8) != 0)
                    return false;
            }
            else if (!grid.terrainPassable[size_t(y) * walkWidth + size_t(x)])
                return false;
            for (int obstacle = 0; obstacle < forbiddenCount; ++obstacle)
            {
                const PixelBox & box = forbidden[obstacle];
                if (centerX >= box.left && centerX <= box.right &&
                    centerY >= box.top && centerY <= box.bottom)
                {
                    return false;
                }
            }
            return true;
        };

        if (!passable(start.x, start.y) ||
            !passable(goal.x, goal.y))
        {
            return false;
        }

        const int localCellCount = regionWidth * regionHeight;
        std::vector<unsigned char> visited(size_t(localCellCount), 0);
        std::vector<int> fringe;
        fringe.reserve(size_t(localCellCount));

        auto localIndex = [minX, minY, regionWidth](int x, int y)
        {
            return (y - minY) * regionWidth + (x - minX);
        };
        const int startIndex = localIndex(start.x, start.y);
        visited[size_t(startIndex)] = 1;
        fringe.push_back(startIndex);

        const int actionX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
        const int actionY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
        for (size_t next = 0; next < fringe.size(); ++next)
        {
            const int current = fringe[next];
            const int x = current % regionWidth + minX;
            const int y = current / regionWidth + minY;
            if (x == goal.x && y == goal.y)
            {
                return true;
            }

            for (int action = 0; action < 8; ++action)
            {
                const int nx = x + actionX[action];
                const int ny = y + actionY[action];
                if (!passable(nx, ny))
                {
                    continue;
                }

                // A diagonal may round an obstacle corner through either
                // cardinal side, but never teleport across two blocked sides.
                if (action >= 4 &&
                    !passable(x + actionX[action], y) &&
                    !passable(x, y + actionY[action]))
                {
                    continue;
                }

                const int index = localIndex(nx, ny);
                if (visited[size_t(index)])
                {
                    continue;
                }

                visited[size_t(index)] = 1;
                fringe.push_back(index);
            }
        }
        return false;
    };

    const std::vector<WallBlock> noBlocks;
    if (!pathExists(probeGrid, noBlocks, true) ||
        !pathExists(zerglingGrid, noBlocks, true) ||
        !pathExists(scvGrid, noBlocks, true) ||
        !pathExists(tankGrid, noBlocks, true) ||
        !pathExists(probeGrid, noBlocks, false) ||
        !pathExists(zerglingGrid, noBlocks, false) ||
        !pathExists(scvGrid, noBlocks, false) ||
        !pathExists(tankGrid, noBlocks, false))
    {
        if (retryUsingRouteChoke())
        {
            return;
        }
        reportFailure("WALL: baseline route validation failed");
        return;
    }

    // The construction SCV does not need to walk THROUGH a two-piece wall to
    // place the third building. Requiring that (the old test) rejected valid
    // Terran walls whenever Barracks + one Depot already blocked an SCV-sized
    // body. It only needs a reachable standing cell next to the missing piece.
    auto scvCanReachBuildSide =
        [&](const std::vector<WallBlock> & placed,
            const WallBlock & target) -> bool
    {
        const PixelBox building = wallBuildingBox(target);
        PixelBox noOverlap;
        noOverlap.left =
            building.left - scvGrid.type.dimensionRight();
        noOverlap.right =
            building.right + scvGrid.type.dimensionLeft();
        noOverlap.top =
            building.top - scvGrid.type.dimensionDown();
        noOverlap.bottom =
            building.bottom + scvGrid.type.dimensionUp();

        const int accessMargin = 24;
        const int minX = std::max(0, (noOverlap.left - accessMargin) / 8);
        const int minY = std::max(0, (noOverlap.top - accessMargin) / 8);
        const int maxX = std::min(
            walkWidth - 1, (noOverlap.right + accessMargin) / 8);
        const int maxY = std::min(
            walkHeight - 1, (noOverlap.bottom + accessMargin) / 8);

        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                if (!scvGrid.terrainPassable[
                        size_t(y) * walkWidth + size_t(x)])
                {
                    continue;
                }

                const int centerX = x * 8 + 4;
                const int centerY = y * 8 + 4;
                if (centerX >= noOverlap.left && centerX <= noOverlap.right &&
                    centerY >= noOverlap.top && centerY <= noOverlap.bottom)
                {
                    continue;
                }

                const int dx = centerX < noOverlap.left
                    ? noOverlap.left - centerX
                    : (centerX > noOverlap.right
                        ? centerX - noOverlap.right : 0);
                const int dy = centerY < noOverlap.top
                    ? noOverlap.top - centerY
                    : (centerY > noOverlap.bottom
                        ? centerY - noOverlap.bottom : 0);
                if (std::max(dx, dy) > accessMargin)
                {
                    continue;
                }

                if (pathExists(scvGrid, placed, true, false,
                    scvGrid.inside, BWAPI::WalkPosition(x, y)))
                {
                    return true;
                }
            }
        }
        return false;
    };

    auto candidateBuildable =
        [&](BWAPI::UnitType type, const BWAPI::TilePosition & tile) -> bool
    {
        const int right = tile.x + type.tileWidth() - 1;
        const int bottom = tile.y + type.tileHeight() - 1;
        if (!tile.isValid() || right >= BWAPI::Broodwar->mapWidth() ||
            bottom >= BWAPI::Broodwar->mapHeight() ||
            boxOverlapsBase(tile.x, tile.y, right, bottom) ||
            !the.bases.connectedToStart(tile))
        {
            return false;
        }

        for (int x = tile.x; x <= right; ++x)
        {
            for (int y = tile.y; y <= bottom; ++y)
            {
                const BWAPI::TilePosition cell(x, y);
                if (!the.map.isBuildable(cell) ||
                    GroundHeight(cell) != wallHeight ||
                    !BWAPI::Broodwar->isBuildable(cell, true) ||
                    (_reserveMap[x][y] &
                        (ReserveBuilding | ReserveResource)) != 0)
                {
                    return false;
                }
            }
        }
        return true;
    };

    const BWAPI::UnitType tightMover = stopZergling
        ? BWAPI::UnitTypes::Zerg_Zergling : BWAPI::UnitTypes::Protoss_Probe;

    auto makeCandidate = [&](BWAPI::UnitType type, const BWAPI::TilePosition & tile)
    {
        WallCandidate candidate;
        candidate.tile = tile;
        candidate.center = wallBuildingCenter(type, tile);
        const double dx = double(candidate.center.x - chokeCenter.x);
        const double dy = double(candidate.center.y - chokeCenter.y);
        const BWAPI::Position throatCenter(int(throat.x), int(throat.y));
        const double fromThroat =
            (candidate.center.x - throat.x) * forwardX +
            (candidate.center.y - throat.y) * forwardY;
        candidate.sideProjection = dx * sideX + dy * sideY;
        candidate.score = candidate.center.getApproxDistance(throatCenter) +
            int(std::abs(fromThroat) * 2.0) +
            int(std::max(0.0, fromThroat - 32.0) * 5.0);
        candidate.terrainSides = 0;

        // Find collision-tight contacts with the two terrain banks. These
        // contacts guide the search only; every layout still gets the exact
        // closed-wall and open-gate path tests below.
        const PixelBox box = wallBuildingBox(WallBlock(type, tile));
        const int width = tightMover.dimensionLeft() + tightMover.dimensionRight() + 1;
        const int height = tightMover.dimensionUp() + tightMover.dimensionDown() + 1;
        // The map boundary is also a bank. Include its first outside cell;
        // terrainWalkable() safely reports those coordinates as blocked.
        const int minX = std::max(-1, box.left / 8 - (width + 7) / 8 - 1);
        const int minY = std::max(-1, box.top / 8 - (height + 7) / 8 - 1);
        const int maxX = std::min(walkWidth, box.right / 8 + (width + 7) / 8 + 1);
        const int maxY = std::min(walkHeight, box.bottom / 8 + (height + 7) / 8 + 1);
        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                if (terrainWalkable(x * 8 + 4, y * 8 + 4)) continue;
                const int gapX = std::max(0, std::max(x * 8 - box.right - 1,
                    box.left - (x * 8 + 7) - 1));
                const int gapY = std::max(0, std::max(y * 8 - box.bottom - 1,
                    box.top - (y * 8 + 7) - 1));
                if (gapX >= width || gapY >= height) continue;
                const double side = (x * 8 + 4 - chokeCenter.x) * sideX +
                    (y * 8 + 4 - chokeCenter.y) * sideY;
                candidate.terrainSides |= side < -8.0 ? 1 : (side > 8.0 ? 2 : 3);
            }
        }
        return candidate;
    };

    auto collectCandidates = [&](BWAPI::UnitType type)
    {
        std::vector<WallCandidate> result;
        const BWAPI::TilePosition chokeTile(chokeCenter);
        const int searchRadius = 12;
        const double throatForward =
            (throat.x - chokeCenter.x) * forwardX +
            (throat.y - chokeCenter.y) * forwardY;
        // The buildable shoulder may be six tiles behind the actual throat.
        // A fixed +3-tile cutoff there dropped Depots at the narrow lip,
        // leaving only placements on the open plateau.
        const double forwardLimit = std::max(3.0 * 32.0,
            throatForward + 2.0 * 32.0);
        for (int x = chokeTile.x - searchRadius; x <= chokeTile.x + searchRadius; ++x)
        {
            for (int y = chokeTile.y - searchRadius; y <= chokeTile.y + searchRadius; ++y)
            {
                const BWAPI::TilePosition tile(x, y);
                if (!candidateBuildable(type, tile)) continue;
                WallCandidate candidate = makeCandidate(type, tile);
                const double along = (candidate.center.x - chokeCenter.x) * forwardX +
                    (candidate.center.y - chokeCenter.y) * forwardY;
                if (along < -7.0 * 32.0 || along > forwardLimit) continue;
                result.push_back(candidate);
            }
        }
        std::sort(result.begin(), result.end(), [](const WallCandidate & a, const WallCandidate & b)
        {
            if (a.score != b.score) return a.score < b.score;
            if (a.tile.y != b.tile.y) return a.tile.y < b.tile.y;
            return a.tile.x < b.tile.x;
        });
        // Keep every legal anchor in this bounded area. Taking only the 96
        // nearest positions discarded bank contacts on flared/diagonal ramps.
        return result;
    };

    std::vector<WallCandidate> barracksCandidates = collectCandidates(barracksType);
    std::vector<WallCandidate> depotCandidates = collectCandidates(depotType);
    std::vector<WallCombination> combinations;

    auto canJoin = [&](const WallCandidate & a, BWAPI::UnitType aType,
                        const WallCandidate & b, BWAPI::UnitType bType)
    {
        if (wallFootprintsOverlap(aType, a.tile, bType, b.tile)) return false;
        const PixelBox first = wallBuildingBox(WallBlock(aType, a.tile));
        const PixelBox second = wallBuildingBox(WallBlock(bType, b.tile));
        const int width = tightMover.dimensionLeft() + tightMover.dimensionRight() + 1;
        const int height = tightMover.dimensionUp() + tightMover.dimensionDown() + 1;
        const int gapX = std::max(0, std::max(second.left - first.right - 1,
            first.left - second.right - 1));
        const int gapY = std::max(0, std::max(second.top - first.bottom - 1,
            first.top - second.bottom - 1));
        return gapX < width && gapY < height;
    };

    auto makeCombination = [&](int r, int d1, int d2, bool mapSeed)
    {
        const WallCandidate & barracks = barracksCandidates[size_t(r)];
        const WallCandidate & depot1 = depotCandidates[size_t(d1)];
        WallCombination combination;
        combination.barracks = r;
        combination.depot1 = d1;
        combination.depot2 = d2;
        combination.mapSeed = mapSeed;
        combination.score = barracks.score + depot1.score +
            int(std::abs(barracks.sideProjection) * 4.0);
        combination.centralGate = std::abs(barracks.sideProjection) <=
            std::abs(depot1.sideProjection);
        if (d2 >= 0)
        {
            const WallCandidate & depot2 = depotCandidates[size_t(d2)];
            combination.score += depot2.score;
            // A central gate joins the two bank-anchored Depots itself.
            // Projections on a remote ramp axis can mislabel a stepped edge
            // gate as central, so use the actual collision-chain connections.
            const bool oppositeBanks =
                ((depot1.terrainSides & 1) && (depot2.terrainSides & 2)) ||
                ((depot1.terrainSides & 2) && (depot2.terrainSides & 1));
            combination.centralGate = oppositeBanks &&
                canJoin(barracks, barracksType, depot1, depotType) &&
                canJoin(barracks, barracksType, depot2, depotType);
        }
        return combination;
    };

    // Known map coordinates are search hints, never an exemption from the
    // validator. Iron also uses map-specific walls on these difficult ramps.
    // Coordinate reference: Iron, territory/wall.cpp, Wall::MapSpecific().
    // https://github.com/bmnielsen/Stardust/blob/main/test/3rdparty/opponents/Iron/src/territory/wall.cpp
    struct MapWallSeed
    {
        const char * hash;
        BWAPI::TilePosition barracks, depot1, depot2;
    };
    const MapWallSeed mapSeeds[] = {
        { "af618ea3ed8a8926ca7b17619eebcb9126f0d8b1", {23, 85}, {20, 81}, {22, 83} }, // Benzene SW
        { "af618ea3ed8a8926ca7b17619eebcb9126f0d8b1", {104, 28}, {103, 26}, {101, 24} }, // Benzene NE
        { "4e24f217d2fe4dbfa6799bc57f74d8dc939d425b", {48, 10}, {47, 8}, {47, 6} }, // Destination N
        { "4e24f217d2fe4dbfa6799bc57f74d8dc939d425b", {44, 118}, {46, 116}, {46, 114} }, // Destination S
        { "d9757c0adcfd61386dff8fe3e493e9e8ef9b45e3", {112, 44}, {110, 42}, {115, 42} },
        { "d9757c0adcfd61386dff8fe3e493e9e8ef9b45e3", {17, 69}, {21, 65}, {19, 67} },
        { "6f8da3c3cc8d08d9cf882700efa049280aedca8c", {17, 69}, {21, 65}, {19, 67} }
    };
    const MapWallSeed * mapSeed = nullptr;
    int seedDistance = 32 * 32;
    const std::string mapHash = BWAPI::Broodwar->mapHash();
    for (const MapWallSeed & seed : mapSeeds)
    {
        if (mapHash != seed.hash) continue;
        const int distance = wallBuildingCenter(barracksType, seed.barracks)
            .getApproxDistance(main->getCenter());
        if (distance < seedDistance)
        {
            mapSeed = &seed;
            seedDistance = distance;
        }
    }
    std::string mapSeedStatus = mapSeed ? "placement" : "none";
    if (mapSeed && candidateBuildable(barracksType, mapSeed->barracks) &&
        candidateBuildable(depotType, mapSeed->depot1) &&
        candidateBuildable(depotType, mapSeed->depot2) &&
        !wallFootprintsOverlap(barracksType, mapSeed->barracks, depotType, mapSeed->depot1) &&
        !wallFootprintsOverlap(barracksType, mapSeed->barracks, depotType, mapSeed->depot2) &&
        !wallFootprintsOverlap(depotType, mapSeed->depot1, depotType, mapSeed->depot2))
    {
        auto addAnchor = [&](BWAPI::UnitType type, const BWAPI::TilePosition & tile,
                             std::vector<WallCandidate> & candidates)
        {
            for (size_t i = 0; i < candidates.size(); ++i)
                if (candidates[i].tile == tile) return int(i);
            candidates.push_back(makeCandidate(type, tile));
            return int(candidates.size()) - 1;
        };
        const int r = addAnchor(barracksType, mapSeed->barracks, barracksCandidates);
        const int d1 = addAnchor(depotType, mapSeed->depot1, depotCandidates);
        const int d2 = addAnchor(depotType, mapSeed->depot2, depotCandidates);
        combinations.push_back(makeCombination(r, std::min(d1, d2), std::max(d1, d2), true));
        mapSeedStatus = "pending";
    }

    if (barracksCandidates.empty() || depotCandidates.empty())
    {
        if (retryUsingRouteChoke()) return;
        reportFailure("WALL: buildable tiles R=" + std::to_string(barracksCandidates.size()) +
            " D=" + std::to_string(depotCandidates.size()));
        return;
    }

    // Construct tight chains between terrain banks, as in BWEM/BWEB wall
    // searches. Do not sample unrelated triples out of hundreds of thousands
    // of mostly disconnected combinations.
    std::vector<std::vector<int>> depotLinks(depotCandidates.size());
    for (size_t i = 0; i < depotCandidates.size(); ++i)
        for (size_t j = i + 1; j < depotCandidates.size(); ++j)
            if (canJoin(depotCandidates[i], depotType, depotCandidates[j], depotType))
            {
                depotLinks[i].push_back(int(j));
                depotLinks[j].push_back(int(i));
            }

    for (size_t r = 0; r < barracksCandidates.size(); ++r)
    {
        const WallCandidate & barracks = barracksCandidates[r];
        std::vector<int> neighbors;
        std::vector<unsigned char> joinsBarracks(depotCandidates.size(), 0);
        for (size_t d = 0; d < depotCandidates.size(); ++d)
            if (canJoin(barracks, barracksType, depotCandidates[d], depotType))
            {
                neighbors.push_back(int(d));
                joinsBarracks[d] = 1;
            }

        auto addChain = [&](int first, int second)
        {
            const WallCandidate & depot1 = depotCandidates[size_t(first)];
            int sides = barracks.terrainSides | depot1.terrainSides;
            if (second >= 0)
            {
                const WallCandidate & depot2 = depotCandidates[size_t(second)];
                if (wallFootprintsOverlap(barracksType, barracks.tile, depotType, depot2.tile) ||
                    wallFootprintsOverlap(depotType, depot1.tile, depotType, depot2.tile)) return;
                sides |= depot2.terrainSides;
            }
            if (sides != 3) return;
            combinations.push_back(makeCombination(int(r),
                second >= 0 ? std::min(first, second) : first,
                second >= 0 ? std::max(first, second) : -1, false));
        };

        for (size_t i = 0; i < neighbors.size(); ++i)
        {
            const int first = neighbors[i];
            addChain(first, -1);
            // Barracks between the two Depots.
            for (size_t j = i + 1; j < neighbors.size(); ++j)
                addChain(first, neighbors[j]);
            // A Depot is the middle link. Skip triangles already added above.
            for (int second : depotLinks[size_t(first)])
                if (!joinsBarracks[size_t(second)]) addChain(first, second);
        }
    }

    std::sort(
        combinations.begin(), combinations.end(),
        [](const WallCombination & a, const WallCombination & b)
        {
            if (a.centralGate != b.centralGate) return a.centralGate;
            if ((a.depot2 < 0) != (b.depot2 < 0)) return a.depot2 < 0;
            if (a.mapSeed != b.mapSeed) return a.mapSeed;
            if (a.score != b.score) return a.score < b.score;
            if (a.barracks != b.barracks) return a.barracks < b.barracks;
            if (a.depot1 != b.depot1) return a.depot1 < b.depot1;
            return a.depot2 < b.depot2;
        });

    // Search connected layouts in score order. The old scattered sample
    // skipped good neighbors even when almost no full checks had been used.
    // Keep central gates preferred and bound the work in each category.
    const size_t fullMapVerificationLimit = 512;
    size_t fullMapVerifications = 0;
    size_t categoryVerifications[4] = { 0, 0, 0, 0 };
    auto categoryOf = [](const WallCombination & combination)
    {
        return (combination.centralGate ? 0 : 2) + (combination.depot2 < 0 ? 0 : 1);
    };
    std::vector<size_t> verificationOrder;
    size_t categoryBegin = 0;
    for (int category = 0; category < 4; ++category)
    {
        size_t end = categoryBegin;
        while (end < combinations.size() && categoryOf(combinations[end]) == category) ++end;
        const size_t prefixEnd = std::min(end, categoryBegin + 2048);
        for (size_t i = categoryBegin; i < prefixEnd; ++i) verificationOrder.push_back(i);
        categoryBegin = end;
    }

    // A blocked endpoint proves nothing about passage closure. Keep the
    // geometric test explicit before interpreting a failed path as a seal.
    auto coversRouteSeed = [&](const WallMoverGrid & grid,
                               const std::vector<WallBlock> & blocks) -> bool
    {
        for (const BWAPI::WalkPosition seed : {
            grid.inside, grid.outside, grid.deepInside, grid.deepOutside })
        {
            const int x = seed.x * 8 + 4, y = seed.y * 8 + 4;
            for (const WallBlock & block : blocks)
            {
                const PixelBox box = wallBuildingBox(block);
                if (x >= box.left - grid.type.dimensionRight() &&
                    x <= box.right + grid.type.dimensionLeft() &&
                    y >= box.top - grid.type.dimensionDown() &&
                    y <= box.bottom + grid.type.dimensionUp()) return true;
            }
        }
        return false;
    };
    auto tankGateIsUsable = [&](const WallCandidate & barracks,
                                const std::vector<WallBlock> & openWall)
    {
        const auto tank = tankGrid.type;
        // The Tank must fit in the middle of the vacated Barracks and reach
        // both sides with its complete collision box. A legal path may turn:
        // demanding a straight line parallel to the ramp rejected stepped
        // walls even when a collision-free route through the gate existed.
        const int left = barracks.center.x - tank.dimensionLeft();
        const int right = barracks.center.x + tank.dimensionRight();
        const int top = barracks.center.y - tank.dimensionUp();
        const int bottom = barracks.center.y + tank.dimensionDown();
        if (left < 0 || top < 0 || right >= walkWidth * 8 || bottom >= walkHeight * 8 ||
            rectangleBlockedCount(left / 8, top / 8, right / 8, bottom / 8) != 0)
            return false;
        for (const WallBlock & block : openWall)
        {
            const PixelBox box = wallBuildingBox(block);
            if (right >= box.left && left <= box.right &&
                bottom >= box.top && top <= box.bottom) return false;
        }
        const BWAPI::WalkPosition gate(barracks.center);
        // Both sides must reach the middle of the gate. The full-map check
        // below still verifies the base-to-natural connection as well.
        return pathExists(tankGrid, openWall, true, true, tankGrid.inside, gate) &&
            pathExists(tankGrid, openWall, true, true, gate, tankGrid.outside);
    };

    size_t seedRejects = 0;
    size_t pixelGapRejects = 0;
    size_t localLeakRejects = 0;
    size_t globalLeakRejects = 0;
    size_t builderAccessRejects = 0;
    size_t tankGateRejects = 0;
    size_t tankRouteRejects = 0;
    size_t tankCenterRejects = 0;
    for (size_t orderIndex = 0;
         orderIndex < verificationOrder.size(); ++orderIndex)
    {
        const size_t i = verificationOrder[orderIndex];
        const WallCombination & combination = combinations[i];
        const WallCandidate & barracks =
            barracksCandidates[size_t(combination.barracks)];
        const WallCandidate & depot1 =
            depotCandidates[size_t(combination.depot1)];
        const WallCandidate * depot2 = combination.depot2 >= 0
            ? &depotCandidates[size_t(combination.depot2)] : nullptr;

        std::vector<WallBlock> fullWall;
        fullWall.push_back(WallBlock(barracksType, barracks.tile));
        fullWall.push_back(WallBlock(depotType, depot1.tile));
        if (depot2) fullWall.push_back(WallBlock(depotType, depot2->tile));
        if ((stopProbe && coversRouteSeed(probeGrid, fullWall)) ||
            (stopZergling && coversRouteSeed(zerglingGrid, fullWall)))
        {
            ++seedRejects;
            if (combination.mapSeed) mapSeedStatus = "endpoint";
            continue;
        }

        // Check the smallest relevant enemy body locally and across the map.
        if ((stopProbe && pathExists(probeGrid, fullWall, true)) ||
            (stopZergling && pathExists(zerglingGrid, fullWall, true)))
        {
            ++localLeakRejects;
            if (combination.mapSeed) mapSeedStatus = "local-leak";
            continue;
        }

        size_t & categoryVerificationsUsed = categoryVerifications[categoryOf(combination)];
        if (categoryVerificationsUsed >= 128) continue;
        ++categoryVerificationsUsed;
        ++fullMapVerifications;

        if ((stopProbe && pathExists(probeGrid, fullWall, false)) ||
            (stopZergling && pathExists(zerglingGrid, fullWall, false)))
        {
            ++globalLeakRejects;
            if (combination.mapSeed) mapSeedStatus = "bypass";
            continue;
        }

        // Do not count a coarse-grid seal as closed until exact collision
        // space confirms that there is no one-pixel lane around the wall.
        if ((stopProbe && pathExists(probeGrid, fullWall, true, true)) ||
            (stopZergling && pathExists(zerglingGrid, fullWall, true, true)) ||
            (stopProbe && pathExists(probeGrid, fullWall, false, true)) ||
            (stopZergling && pathExists(zerglingGrid, fullWall, false, true)))
        {
            ++pixelGapRejects;
            if (combination.mapSeed) mapSeedStatus = "pixel-gap";
            continue;
        }

        // Validate the real construction order: D1 -> Barracks -> optional
        // D2. The construction worker must also be able to place D1 itself.
        const WallCandidate * firstDepotCandidate = &depot1;
        const WallCandidate * secondDepotCandidate = depot2;
        if (depot2 && depot2->center.getApproxDistance(main->getCenter()) <
            depot1.center.getApproxDistance(main->getCenter()))
            std::swap(firstDepotCandidate, secondDepotCandidate);
        const WallBlock wallBarracks(barracksType, barracks.tile);
        auto buildOrderReachable = [&]() -> bool
        {
            const WallBlock first(depotType, firstDepotCandidate->tile);
            if (!scvCanReachBuildSide(noBlocks, first) ||
                !scvCanReachBuildSide({ first }, wallBarracks)) return false;
            return !secondDepotCandidate || scvCanReachBuildSide(
                { wallBarracks, first }, WallBlock(depotType, secondDepotCandidate->tile));
        };
        bool builderCanFinish = buildOrderReachable();
        if (!builderCanFinish && secondDepotCandidate)
        {
            std::swap(firstDepotCandidate, secondDepotCandidate);
            builderCanFinish = buildOrderReachable();
        }
        if (!builderCanFinish)
        {
            ++builderAccessRejects;
            if (combination.mapSeed) mapSeedStatus = "builder";
            continue;
        }

        std::vector<WallBlock> openWall{ WallBlock(depotType, depot1.tile) };
        if (depot2) openWall.push_back(WallBlock(depotType, depot2->tile));
        auto openRouteExists = [&](const WallMoverGrid & grid, bool local)
        {
            // A legal narrow lane may lie between 8px sample centers. Failure
            // of the cheap test needs exact confirmation, not rejection.
            return pathExists(grid, openWall, local) ||
                pathExists(grid, openWall, local, true);
        };
        if (!openRouteExists(tankGrid, true) || !openRouteExists(tankGrid, false) ||
            !openRouteExists(scvGrid, true) || !openRouteExists(scvGrid, false))
        {
            ++tankGateRejects;
            ++tankRouteRejects;
            if (combination.mapSeed) mapSeedStatus = "tank-path";
            continue;
        }
        if (!tankGateIsUsable(barracks, openWall))
        {
            ++tankGateRejects;
            ++tankCenterRejects;
            if (combination.mapSeed) mapSeedStatus = "gate-center";
            continue;
        }

        _terranMainWallValid = true;
        _terranMainWallCentralGate = combination.centralGate;
        _terranMainWallBarracks = barracks.tile;
        _terranMainWallDepot1 = firstDepotCandidate->tile;
        _terranMainWallDepot2 = secondDepotCandidate
            ? secondDepotCandidate->tile : BWAPI::TilePositions::None;
        _terranMainWallCoversNatural = natural &&
            natural->getCenter().isValid() &&
            isInsideTerranMainWall(natural->getCenter(), -16);

        // The forward line is accepted only if it leaves a real Bunker slot on
        // the protected side while preserving the Tank gate. Main-choke walls
        // retain the old permissive fallback for unusual legacy maps, but a
        // natural wall must never trade away its most important support tile.
        if (_terranMainWallCoversNatural &&
            !findTerranMainWallBunkerLocation(true).isValid())
        {
            _terranMainWallValid = false;
            _terranMainWallCoversNatural = false;
            continue;
        }

        setReserve(
            _terranMainWallBarracks,
            barracksType.tileWidth(), barracksType.tileHeight(),
            ReserveWall, true);
        setReserve(
            _terranMainWallDepot1,
            depotType.tileWidth(), depotType.tileHeight(),
            ReserveWall, true);
        if (_terranMainWallDepot2.isValid())
            setReserve(
                _terranMainWallDepot2,
                depotType.tileWidth(), depotType.tileHeight(),
                ReserveWall, true);

        _terranMainWallStatus = terranMainWallReadyStatus();
        if (combination.mapSeed) _terranMainWallStatus += " (validated map seed)";
        BWAPI::Broodwar->printf(
            "%s Rax=(%d,%d) D1=(%d,%d) D2=(%d,%d)",
            _terranMainWallStatus.c_str(),
            _terranMainWallBarracks.x, _terranMainWallBarracks.y,
            _terranMainWallDepot1.x, _terranMainWallDepot1.y,
            _terranMainWallDepot2.x, _terranMainWallDepot2.y);
        return;
    }

    if (retryUsingRouteChoke())
    {
        return;
    }

    reportFailure(
        "WALL: no layout " + std::string(naturalFallback ? "natural" : "main") +
        (forceRouteFallback ? "/route" : "/choke") +
        " R=" + std::to_string(barracksCandidates.size()) +
        " D=" + std::to_string(depotCandidates.size()) +
        " C=" + std::to_string(combinations.size()) +
        " W=" + std::to_string(int(throat.width / 32)) +
        " at=" + std::to_string(chokeCenter.x / 32) +
        "," + std::to_string(chokeCenter.y / 32) +
        " section=" + std::to_string(sectionAttempt + 1) + "/" + std::to_string(sectionCount) +
        " hint=" + mapSeedStatus +
        " search=" + std::to_string(verificationOrder.size()) +
        " leak=" + std::to_string(localLeakRejects) +
        "/" + std::to_string(globalLeakRejects) +
        " test=" + std::to_string(fullMapVerifications) +
        "/" + std::to_string(fullMapVerificationLimit) +
        " pixel=" + std::to_string(pixelGapRejects) +
        " seed=" + std::to_string(seedRejects) +
        " scv=" + std::to_string(builderAccessRejects) +
        " tank=" + std::to_string(tankGateRejects) +
        "(path=" + std::to_string(tankRouteRejects) +
        ",core=" + std::to_string(tankCenterRejects) + ")");
}

void BuildingPlacer::clearReserveBits(unsigned char bits)
{
    for (size_t x = 0; x < _reserveMap.size(); ++x)
    {
        for (size_t y = 0; y < _reserveMap[x].size(); ++y)
        {
            _reserveMap[x][y] &= static_cast<unsigned char>(~bits);
        }
    }
}

// Reserve or unreserve tiles.
// Tilepositions off the map are silently ignored; initialization code depends on it.
void BuildingPlacer::setReserve(
    const BWAPI::TilePosition & position,
    int width,
    int height,
    unsigned char bits,
    bool flag)
{
    int rwidth = _reserveMap.size();
    int rheight = _reserveMap[0].size();

    for (int x = std::max(position.x, 0); x < std::min(position.x + width, rwidth); ++x)
    {
        for (int y = std::max(position.y, 0); y < std::min(position.y + height, rheight); ++y)
        {
            if (flag)
            {
                _reserveMap[x][y] |= bits;
            }
            else
            {
                _reserveMap[x][y] &= static_cast<unsigned char>(~bits);
            }
        }
    }
}

// We want to build near the given tile, but it may not be walkable, which makes it awkward
// to find nearby buildable tiles. Find a nearby buildable tile to start with.
// NOTE This can fail in theory, but it should not happen on any reasonable map.
BWAPI::TilePosition BuildingPlacer::connectedWalkableTileNear(const BWAPI::TilePosition & start) const
{
    BWAPI::TilePosition closestTile = BWAPI::TilePositions::None;
    int closestDist = MAX_DISTANCE;

    // A building can start on either odd or even build-tile coordinates.
    // Skipping every other tile can miss the only connected buildable anchor
    // in a tight main or near a choke.
    for (int x = 0; x < BWAPI::Broodwar->mapWidth(); ++x)
    {
        for (int y = 0; y < BWAPI::Broodwar->mapHeight(); ++y)
        {
            if (_buildable.at(x,y) > 0 && the.bases.connectedToStart(BWAPI::TilePosition(x, y)))
            {
                int dist = abs(x - start.x) + abs(y - start.y);
                if (dist < closestDist)
                {
                    closestTile = BWAPI::TilePosition(x, y);
                    closestDist = dist;
                }
            }
        }
    }

    return closestTile;
}

// If we're building at the enemy base, we don't care if our building overlaps a base location.
bool BuildingPlacer::enemyMacroLocation(MacroLocation loc) const
{
    return
        loc == MacroLocation::Proxy ||
        loc == MacroLocation::EnemyMain ||
        loc == MacroLocation::EnemyNatural;
}

// The rectangle in tile coordinates overlaps with a resource depot location.
bool BuildingPlacer::boxOverlapsBase(int x1, int y1, int x2, int y2) const
{
    for (Base * base : the.bases.getAll())
    {
        // The base location. It's the same size for all races.
        int bx1 = base->getTilePosition().x;
        int by1 = base->getTilePosition().y;
        int bx2 = bx1 + 3;
        int by2 = by1 + 2;

        if (!(x2 < bx1 || x1 > bx2 || y2 < by1 || y1 > by2))
        {
            return true;
        }
    }

    // No base overlaps.
    return false;
}

bool BuildingPlacer::tileBlocksAddon(const BWAPI::TilePosition & position) const
{
    for (int i = 0; i <= 2; ++i)
    {
        for (BWAPI::Unit unit : BWAPI::Broodwar->getUnitsOnTile(position.x - i, position.y))
        {
            if (unit->getType().canBuildAddon())
            {
                return true;
            }
        }
    }

    return false;
}

// Does this rectangle consist entirely of buildable terrain?
// The test is very fast and can be done before other buildability checks.
bool BuildingPlacer::buildableTerrain(int x0, int y0, int width, int height) const
{
    for (int y = y0; y < y0 + height; ++y)
    {
        if (_buildable.at(x0, y) < width)
        {
            return false;
        }
    }
    return true;
}

// The tile is free of permanent obstacles, including future ones from planned buildings.
// There might be a unit passing through, though.
// The caller must ensure that x and y are in range!
bool BuildingPlacer::isFreeTile(int x, int y) const
{
    UAB_ASSERT(BWAPI::TilePosition(x,y).isValid(), "bad tile");

    if (!BWAPI::Broodwar->isBuildable(x, y, true) || _reserveMap[x][y])
    {
        return false;
    }
    if (the.self()->getRace() == BWAPI::Races::Terran && tileBlocksAddon(BWAPI::TilePosition(x, y)))
    {
        return false;
    }

    return true;
}

// Check that nothing obstructs the top of the building, including the corners.
// For example, if the building is o, then nothing must obstruct the tiles marked x:
//
//  x x x x
//    o o
//    o o
//
// Unlike canBuildHere(), the freeOn...() functions do not care if mobile units are on the tiles.
// They only care that the tiles are buildable (which implies walkable) and not reserved for future buildings.
bool BuildingPlacer::freeOnTop(const BWAPI::TilePosition & tile, BWAPI::UnitType buildingType) const
{
    int x1 = tile.x - 1;
    int x2 = tile.x + buildingType.tileWidth();
    int y = tile.y - 1;
    if (y < 0 || x1 < 0 || x2 >= BWAPI::Broodwar->mapWidth())
    {
        return false;
    }

    for (int x = x1; x <= x2; ++x)
    {
        if (!isFreeTile(x,y))
        {
            return false;
        }
    }
    return true;
}

//      x
//  o o x
//  o o x
//      x
bool BuildingPlacer::freeOnRight(const BWAPI::TilePosition & tile, BWAPI::UnitType buildingType) const
{
    int x = tile.x + buildingType.tileWidth();
    int y1 = tile.y - 1;
    int y2 = tile.y + buildingType.tileHeight();
    if (x >= BWAPI::Broodwar->mapWidth() || y1 < 0 || y2 >= BWAPI::Broodwar->mapHeight())
    {
        return false;
    }

    for (int y = y1; y <= y2; ++y)
    {
        if (!isFreeTile(x, y))
        {
            return false;
        }
    }
    return true;
}

//  x
//  x o o
//  x o o
//  x
bool BuildingPlacer::freeOnLeft(const BWAPI::TilePosition & tile, BWAPI::UnitType buildingType) const
{
    int x = tile.x - 1;
    int y1 = tile.y - 1;
    int y2 = tile.y + buildingType.tileHeight();
    if (x < 0 || y1 < 0 || y2 >= BWAPI::Broodwar->mapHeight())
    {
        return false;
    }

    for (int y = y1; y <= y2; ++y)
    {
        if (!isFreeTile(x, y))
        {
            return false;
        }
    }
    return true;
}

//    o o
//    o o
//  x x x x
bool BuildingPlacer::freeOnBottom(const BWAPI::TilePosition & tile, BWAPI::UnitType buildingType) const
{
    int x1 = tile.x - 1;
    int x2 = tile.x + buildingType.tileWidth();
    int y = tile.y + buildingType.tileHeight();
    if (y >= BWAPI::Broodwar->mapHeight() || x1 < 0 || x2 >= BWAPI::Broodwar->mapWidth())
    {
        return false;
    }

    for (int x = x1; x <= x2; ++x)
    {
        if (!isFreeTile(x, y))
        {
            return false;
        }
    }
    return true;
}

bool BuildingPlacer::freeOnAllSides(BWAPI::Unit building) const
{
    return
        freeOnTop(building->getTilePosition(), building->getType()) &&
        freeOnRight(building->getTilePosition(), building->getType()) &&
        freeOnLeft(building->getTilePosition(), building->getType()) &&
        freeOnBottom(building->getTilePosition(), building->getType());
}

// Can a building can be built here?
// This does not check all conditions! Other code must check for possible overlaps
// with planned or future buildings.
bool BuildingPlacer::canBuildHere(const BWAPI::TilePosition & position, const Building & b) const
{
    for (const auto& failed : _failedProductionLocations)
    {
        if (failed.first.first != b.type.getID() || failed.second <= the.now()) continue;
        const int x = failed.first.second % BWAPI::Broodwar->mapWidth();
        const int y = failed.first.second / BWAPI::Broodwar->mapWidth();
        if (position.x < x + b.type.tileWidth() &&
            position.x + b.type.tileWidth() > x &&
            position.y < y + b.type.tileHeight() &&
            position.y + b.type.tileHeight() > y) return false;
    }
    return
        // BWAPI thinks the space is buildable.
        // This includes looking for units which may be in the way.
        BWAPI::Broodwar->canBuildHere(position, b.type, b.builderUnit) &&

        // A worker can reach the place.
        // NOTE This simplified check disallows building on islands!
        the.bases.connectedToStart(position) &&
        
        // Enemy static defense/immobile units cannot fire on the building location.
        // This is part of the response to e.g. cannon rushes.
        !the.groundHitsFixed.inRange(b.type, position);
}

// Keep Terran building footprints away from the reserved exit lane.
// A single build-tile line is enough to forbid direct overlap, but a Factory
// immediately beside that line can still leave less than a tank-width gap.
// This one-tile margin is the light-weight equivalent of BWEB Blocks reserving
// a usable path through a production layout.
bool BuildingPlacer::keepsMovementLaneClear(
    int x1, int y1, int x2, int y2, BWAPI::UnitType type) const
{
    if (the.self()->getRace() != BWAPI::Races::Terran ||
        type.isResourceDepot() || type.isRefinery())
    {
        return true;
    }

    const int left = std::max(0, x1 - 1);
    const int top = std::max(0, y1 - 1);
    const int right = std::min(BWAPI::Broodwar->mapWidth() - 1, x2 + 1);
    const int bottom = std::min(BWAPI::Broodwar->mapHeight() - 1, y2 + 1);
    for (int x = left; x <= right; ++x)
    {
        for (int y = top; y <= bottom; ++y)
        {
            if ((_reserveMap[x][y] & ReserveLane) != 0)
            {
                return false;
            }
        }
    }
    return true;
}

// Can we build this building here with the specified amount of space around it?
bool BuildingPlacer::canBuildWithSpace(const BWAPI::TilePosition & position, const Building & b, int extraSpace) const
{
    // Can the building go here? This does not check the extra space or worry about future
    // buildings, but does all necessary checks for current obstructions of the building area itself.
    if (!canBuildHere(position, b))
    {
        return false;
    }

    // Is the entire area, including the extra space, free of obstructions
    // from possible future buildings?

    // Height and width of the building.
    int width(b.type.tileWidth());
    int height(b.type.tileHeight());

    // Allow space for terran addons. The space may be taller than necessary; it's easier that way.
    // All addons are 2x2 tiles.
    if (b.type.canBuildAddon())
    {
        width += 2;
    }

    // A rectangle covering the building spot.
    int x1 = position.x - extraSpace;
    int y1 = position.y - extraSpace;
    int x2 = position.x + width + extraSpace - 1;
    int y2 = position.y + height + extraSpace - 1;

    // The rectangle must fit on the map.
    if (x1 < 0 || x2 >= BWAPI::Broodwar->mapWidth() ||
        y1 < 0 || y2 >= BWAPI::Broodwar->mapHeight())
    {
        return false;
    }
    
    if (!enemyMacroLocation(b.macroLocation) && boxOverlapsBase(x1, y1, x2, y2))
    {
        return false;
    }

    if (!keepsMovementLaneClear(x1, y1, x2, y2, b.type))
    {
        return false;
    }

    // Every tile must be buildable and unreserved.
    for (int x = x1; x <= x2; ++x)
    {
        for (int y = y1; y <= y2; ++y)
        {
            // Only the planned Bunker may consume its advance reservation.
            const bool ownBunkerSlot = b.type == BWAPI::UnitTypes::Terran_Bunker &&
                position == _terranMainWallBunker &&
                (_reserveMap[x][y] & ~ReserveWallBunker) == 0;
            if (!isFreeTile(x, y) && !ownBunkerSlot)
            {
                return false;
            }
        }
    }

    return true;
}

// Buildings of these types should be grouped together,
// not grouped with buildings of other types.
bool BuildingPlacer::groupTogether(BWAPI::UnitType type) const
{
    return
        type == BWAPI::UnitTypes::Terran_Barracks ||
        type == BWAPI::UnitTypes::Terran_Factory;
}

bool BuildingPlacer::terranMainWallSlotAvailable(
    const BWAPI::TilePosition & tile,
    BWAPI::UnitType type) const
{
    if (!_terranMainWallValid || !tile.isValid())
    {
        return false;
    }

    // A lifted wall Barracks keeps its original slot claimed.  That prevents
    // a replacement Barracks from closing the army exit again.
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || unit->getType() != type)
        {
            continue;
        }

        if ((type == BWAPI::UnitTypes::Terran_Barracks &&
             tile == _terranMainWallBarracks && isTerranMainWallBarracks(unit)) ||
            (!unit->isLifted() && unit->getTilePosition() == tile))
        {
            return false;
        }
    }

    const int right = tile.x + type.tileWidth() - 1;
    const int bottom = tile.y + type.tileHeight() - 1;
    if (right >= BWAPI::Broodwar->mapWidth() ||
        bottom >= BWAPI::Broodwar->mapHeight())
    {
        return false;
    }

    for (int x = tile.x; x <= right; ++x)
    {
        for (int y = tile.y; y <= bottom; ++y)
        {
            const BWAPI::TilePosition cell(x, y);
            if (!cell.isValid() ||
                !the.map.isBuildable(cell) ||
                !BWAPI::Broodwar->isBuildable(cell, true) ||
                (_reserveMap[x][y] &
                    (ReserveBuilding | ReserveResource)) != 0)
            {
                return false;
            }
        }
    }
    return true;
}

BWAPI::TilePosition BuildingPlacer::getTerranMainWallLocation(
    const Building & b) const
{
    if (!_terranMainWallValid ||
        (b.macroLocation != MacroLocation::Anywhere &&
         b.macroLocation != MacroLocation::Main &&
         b.macroLocation != MacroLocation::Front))
    {
        return BWAPI::TilePositions::None;
    }

    // Do not reinterpret a deliberately remote direct request as part of the
    // main wall merely because it used the Anywhere default.
    if (b.macroLocation == MacroLocation::Anywhere &&
        b.desiredPosition.isValid() && the.bases.myMain() &&
        b.desiredPosition.getApproxDistance(
            the.bases.myMain()->getTilePosition()) > 18)
    {
        return BWAPI::TilePositions::None;
    }

    if (b.type == BWAPI::UnitTypes::Terran_Bunker && !_terranMainWallReleased)
    {
        return getTerranMainWallBunkerLocation();
    }

    if (b.type == BWAPI::UnitTypes::Terran_Barracks)
    {
        if (_terranMainWallOpened)
        {
            return BWAPI::TilePositions::None;
        }
        return terranMainWallSlotAvailable(
            _terranMainWallBarracks, b.type)
            ? _terranMainWallBarracks
            : BWAPI::TilePositions::None;
    }

    if (b.type == BWAPI::UnitTypes::Terran_Supply_Depot)
    {
        if (terranMainWallSlotAvailable(_terranMainWallDepot1, b.type))
        {
            return _terranMainWallDepot1;
        }
        if (terranMainWallSlotAvailable(_terranMainWallDepot2, b.type))
        {
            return _terranMainWallDepot2;
        }
    }

    return BWAPI::TilePositions::None;
}

// Seek a location on the edge of the map.
// NOTE
//   This is intended for supply depots and tech buildings.
//   It may go wrong if given a building which can take addons.
BWAPI::TilePosition BuildingPlacer::findEdgeLocation(const Building & b) const
{
    // The building's tile position is its upper left corner.
    int rightEdge = BWAPI::Broodwar->mapWidth() - b.type.tileWidth();
    int bottomEdge = BWAPI::Broodwar->mapHeight() - b.type.tileHeight();

    for (const BWAPI::TilePosition & tile : the.map.getClosestTilesTo(the.bases.myMain()->getTilePosition()))
    {
        // If the position is too far away, skip it.
        if (!the.zone.sameArea(tile, the.bases.myMain()->getTilePosition()) ||
            tile.getApproxDistance(the.bases.myMain()->getTilePosition()) > 18)
        {
            continue;
        }

        // Left.
        if (tile.x == 0)
        {
            if (canBuildWithSpace(tile, b, 0) && freeOnRight(tile, b.type))
            {
                return tile;
            }
        }

        // Top.
        if (tile.y == 0)
        {
            if (canBuildWithSpace(tile, b, 0) && freeOnBottom(tile, b.type))
            {
                return tile;
            }
        }

        // Right.
        else if (tile.x + b.type.tileWidth() == BWAPI::Broodwar->mapWidth())
        {
            if (canBuildWithSpace(tile, b, 0) && freeOnLeft(tile, b.type))
            {
                return tile;
            }
        }

        // Bottom.
        // NOTE The map's bottom row of tiles is not buildable (though other edges are).
        else if (tile.y + b.type.tileHeight() == BWAPI::Broodwar->mapHeight() - 1)
        {
            if (canBuildWithSpace(tile, b, 0) && freeOnTop(tile, b.type))
            {
                return tile;
            }
        }
    }

    // It's not on an edge.
    return BWAPI::TilePositions::None;
}

// Try to put a pylon at every base.


BWAPI::TilePosition BuildingPlacer::findGroupedLocation(const Building & b) const
{
    // Some buildings should not be clustered.
    // That includes resource depots and refineries, which are placed by different code.
    if (b.type == BWAPI::UnitTypes::Terran_Bunker ||
        b.type == BWAPI::UnitTypes::Terran_Missile_Turret)
    {
        return BWAPI::TilePositions::None;
    }

    // Some buildings are best grouped with others of the same type.
    const bool sameType = groupTogether(b.type);

    BWAPI::Unitset choices;
    for (BWAPI::Unit building : the.self()->getUnits())
    {
        if (b.type == BWAPI::UnitTypes::Terran_Barracks &&
            isTerranMainWallBarracks(building))
        {
            continue;
        }

        if (building->getType().isBuilding() &&
            the.zone.sameArea(building->getTilePosition(), b.desiredPosition) &&
            (building->getType().tileWidth() == b.type.tileWidth() || building->getType().tileHeight() == b.type.tileHeight()))
        {
            if (sameType)
            {
                if (b.type == building->getType() && freeOnAllSides(building))
                {
                    choices.insert(building);
                }
            }
            else
            {
                if (!groupTogether(building->getType()) && freeOnAllSides(building))
                {
                    choices.insert(building);
                }
            }
        }
    }

    for (BWAPI::Unit choice : choices)
    {
        BWAPI::TilePosition tile;

        if (choice->getType().tileWidth() == b.type.tileWidth())
        {
            // Above.
            tile = choice->getTilePosition() + BWAPI::TilePosition(0, -b.type.tileHeight());
            if (canBuildWithSpace(tile, b, 0) &&
                freeOnTop(tile, b.type) &&
                freeOnLeft(tile, b.type) &&
                freeOnRight(tile, b.type))
            {
                return tile;
            }

            // Below.
            tile = choice->getTilePosition() + BWAPI::TilePosition(0, choice->getType().tileHeight());
            if (canBuildWithSpace(tile, b, 0) &&
                freeOnBottom(tile, b.type) &&
                freeOnLeft(tile, b.type) &&
                freeOnRight(tile, b.type))
            {
                return tile;
            }
        }

        // Buildings that may have addons in the future should be grouped vertically if at all.
        if (choice->getType().tileHeight() == b.type.tileHeight() &&
            !b.type.canBuildAddon() &&
            !choice->getType().canBuildAddon())
        {
            // Left.
            tile = choice->getTilePosition() + BWAPI::TilePosition(-b.type.tileWidth(), 0);
            if (canBuildWithSpace(tile, b, 0) &&
                freeOnTop(tile, b.type) &&
                freeOnLeft(tile, b.type) &&
                freeOnBottom(tile, b.type))
            {
                return tile;
            }

            // Right.
            tile = choice->getTilePosition() + BWAPI::TilePosition(choice->getType().tileWidth(), 0);
            if (canBuildWithSpace(tile, b, 0) &&
                freeOnTop(tile, b.type) &&
                freeOnRight(tile, b.type) &&
                freeOnBottom(tile, b.type))
            {
                return tile;
            }
        }
    }
    
    return BWAPI::TilePositions::None;
}

// Some buildings get special-case placement.
BWAPI::TilePosition BuildingPlacer::findSpecialLocation(const Building & b) const
{
    BWAPI::TilePosition tile = BWAPI::TilePositions::None;

    if (b.type == BWAPI::UnitTypes::Terran_Supply_Depot ||
        b.type == BWAPI::UnitTypes::Terran_Academy ||
        b.type == BWAPI::UnitTypes::Terran_Armory)
    {
        // These buildings are all 3x2 and will line up neatly.
        tile = findEdgeLocation(b);
        if (!tile.isValid())
        {
            tile = findGroupedLocation(b);
        }
    }
    else 
    {
        tile = findGroupedLocation(b);
    }

    return tile;
}

BWAPI::TilePosition BuildingPlacer::findAnyLocation(const Building & b, int extraSpace) const
{
    // Tiles sorted in order of closeness to the location.
    const std::vector<BWAPI::TilePosition> & closest = the.map.getClosestTilesTo(b.desiredPosition);

    for (const BWAPI::TilePosition & tile : closest)
    {
        if (canBuildWithSpace(tile, b, extraSpace))
        {
            return tile;
        }
    }

    return BWAPI::TilePositions::None;
}

// Mineral patches in range of the position. Calculations in pixels.
int BuildingPlacer::countInRange(const BWAPI::Unitset & patches, BWAPI::Position xy, int range) const
{
    int count = 0;
    for (BWAPI::Unit patch : patches)
    {
        if (xy.getApproxDistance(patch->getInitialPosition()) <= range)
        {
            ++count;
        }
    }
    return count;
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

BuildingPlacer::BuildingPlacer()
    : _terranMainWallValid(false)
    , _terranMainWallOpened(false)
    , _terranMainWallReleased(false)
    , _terranMainWallCentralGate(false)
    , _terranMainWallCoversNatural(false)
    , _terranMainWallUsedRouteFallback(false)
    , _terranMainWallUsedDestructibleRoute(false)
    , _terranMainWallStatus("WALL: not initialized")
    , _terranMainWallBarracks(BWAPI::TilePositions::None)
    , _terranMainWallDepot1(BWAPI::TilePositions::None)
    , _terranMainWallDepot2(BWAPI::TilePositions::None)
    , _terranMainWallBunker(BWAPI::TilePositions::None)
    , _terranMainWallInside(BWAPI::Positions::Invalid)
    , _terranMainWallOutside(BWAPI::Positions::Invalid)
    , _terranMainWallAnalysisCenter(BWAPI::Positions::Invalid)
    , _terranMainWallChokeEndA(BWAPI::Positions::Invalid)
    , _terranMainWallChokeEndB(BWAPI::Positions::Invalid)
{
}

void BuildingPlacer::initialize()
{
    _failedProductionLocations.clear();
    _lastTerranRushWallAttemptFrame = -1;
    _terranMainWallDisabledForOpening = false;
    _reserveMap = std::vector< std::vector<unsigned char> >(
        BWAPI::Broodwar->mapWidth(),
        std::vector<unsigned char>(BWAPI::Broodwar->mapHeight(), 0));

    reserveSpaceNearResources();
    reserveBaseMovementLanes();
    findTerranMainWall();
    reserveOpenMainExitLane();
    reserveTerranMainWallBunker();
    _buildable.compute();
}

void BuildingPlacer::disableTerranMainWallForEarlyExpand()
{
    // A wall around the main leaves a fast natural and its defenders outside.
    // Keep a forward wall only when the verified layout also covers the natural.
    if (terranMainWallCoversNatural())
    {
        return;
    }

    _terranMainWallDisabledForOpening = true;
    _terranMainWallValid = false;
    _terranMainWallBarracksID = -1;
    _terranMainWallOpened = false;
    _terranMainWallReleased = false;
    _terranMainWallCentralGate = false;
    _terranMainWallCoversNatural = false;
    _terranMainWallBarracks = BWAPI::TilePositions::None;
    _terranMainWallDepot1 = BWAPI::TilePositions::None;
    _terranMainWallDepot2 = BWAPI::TilePositions::None;
    _terranMainWallBunker = BWAPI::TilePositions::None;
    _terranMainWallInside = BWAPI::Positions::Invalid;
    _terranMainWallOutside = BWAPI::Positions::Invalid;
    _terranMainWallAnalysisCenter = BWAPI::Positions::Invalid;
    _terranMainWallChokeEndA = BWAPI::Positions::Invalid;
    _terranMainWallChokeEndB = BWAPI::Positions::Invalid;
    _terranMainWallStatus = "WALL: skipped (BioTank early natural)";

    clearReserveBits(ReserveWall | ReserveWallBunker);
    reserveOpenMainExitLane();
    _buildable.compute();
}

void BuildingPlacer::refreshMapAnalysis(bool retryWall)
{
    // Terrain/resource topology changed. Rebuild only the map-derived reserve
    // bits and keep outstanding BuildingManager reservations intact.
    clearReserveBits(ReserveResource | ReserveLane);
    reserveSpaceNearResources();
    reserveBaseMovementLanes();

    // If startup analysis failed because a neutral building separated the
    // route, try once more after the blocker disappears. A successful wall
    // keeps its exact reserved slots and is never recomputed under buildings.
    if (retryWall && !_terranMainWallValid &&
        !_terranMainWallDisabledForOpening &&
        the.selfRace() == BWAPI::Races::Terran &&
        the.now() <= 8 * 60 * 24)
    {
        findTerranMainWall();
        reserveTerranMainWallBunker();
    }
    reserveOpenMainExitLane();
    _buildable.compute();
}

void BuildingPlacer::refreshMovementLanes()
{
    if (_reserveMap.empty())
    {
        return;
    }
    clearReserveBits(ReserveLane);
    reserveBaseMovementLanes();
    reserveOpenMainExitLane();
}

void BuildingPlacer::tryTerranRushWall()
{
    if (_terranMainWallValid || _lastTerranRushWallAttemptFrame >= 0 ||
        the.selfRace() != BWAPI::Races::Terran ||
        !BWAPI::Broodwar->enemy() ||
        BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran ||
        the.now() > 3 * 60 * 24 ||
        the.my.all.count(BWAPI::UnitTypes::Terran_Barracks) > 0)
    {
        return;
    }

    const OpponentModel & model = OpponentModel::Instance();
    const OpeningPlan observed = model.getEnemyPlan();
    if (observed != OpeningPlan::WorkerRush &&
        observed != OpeningPlan::FastRush &&
        observed != OpeningPlan::HeavyRush &&
        !model.stronglyPredictsEnemyPlan(OpeningPlan::WorkerRush) &&
        !model.stronglyPredictsEnemyPlan(OpeningPlan::FastRush) &&
        !model.stronglyPredictsEnemyPlan(OpeningPlan::HeavyRush) &&
        !model.stronglyPredictsEarlyGroundRush())
    {
        return;
    }

    _lastTerranRushWallAttemptFrame = the.now();
    // The first pass reserved an open main exit while history was unknown.
    // Remove that provisional lane before checking wall footprints.
    clearReserveBits(ReserveLane);
    findTerranMainWall();
    reserveBaseMovementLanes();
    reserveOpenMainExitLane();
    if (_terranMainWallValid)
    {
        reserveTerranMainWallBunker();
    }
    _buildable.compute();
}

bool BuildingPlacer::preservesGroundExit(
    BWAPI::UnitType type, BWAPI::TilePosition tile) const
{
    if (!tile.isValid() || !type.isBuilding())
    {
        return false;
    }

    const int left = tile.x;
    const int top = tile.y;
    const int right = left + type.tileWidth();
    const int bottom = top + type.tileHeight();
    const int mapWidth = BWAPI::Broodwar->mapWidth();
    const int mapHeight = BWAPI::Broodwar->mapHeight();
    if (left < 0 || top < 0 || right > mapWidth || bottom > mapHeight)
    {
        return false;
    }

    // Landing searches test many footprints against the same protected
    // routes. Each route uses a full runtime A* search, so calculate it only
    // once per frame and reuse it for the remaining candidates.
    static int routeCacheFrame = -1;
    static unsigned int routeCacheRevision = 0;
    static std::map<std::pair<int, int>, std::vector<BWAPI::TilePosition>>
        routeCache;
    static std::map<std::pair<int, int>, std::vector<BWAPI::TilePosition>>
        terrainRouteCache;
    const int now = BWAPI::Broodwar->getFrameCount();
    if (routeCacheFrame != now ||
        routeCacheRevision != the.map.runtimeRevision())
    {
        routeCache.clear();
        terrainRouteCache.clear();
        routeCacheFrame = now;
        routeCacheRevision = the.map.runtimeRevision();
    }

    const auto covered = [=](BWAPI::TilePosition p)
    {
        return p.x >= left && p.x < right && p.y >= top && p.y < bottom;
    };
    const auto open = [&](BWAPI::TilePosition p)
    {
        return p.isValid() && !covered(p) && the.map.isRuntimeWalkable(p);
    };

    // The active path is a cheap filter: a building away from every protected
    // exit cannot sever one. Only paths that touch its footprint need a flood.
    const auto routeSurvives = [&](BWAPI::TilePosition from,
                                   BWAPI::TilePosition to)
    {
        if (!from.isValid() || !to.isValid())
        {
            return true;
        }
        const auto key = std::make_pair(
            from.y * mapWidth + from.x,
            to.y * mapWidth + to.x);
        auto cached = routeCache.find(key);
        if (cached == routeCache.end())
        {
            std::vector<BWAPI::TilePosition> route;
            the.map.getGroundPath(from, to, route, 160, true);
            cached = routeCache.emplace(key, std::move(route)).first;
        }
        const std::vector<BWAPI::TilePosition>& path = cached->second;
        if (path.size() < 2)
        {
            // A closed wall can make the current route disappear. That does
            // not make the future open gate expendable: at minimum protect
            // its terrain route until the Barracks has lifted.
            if (type == BWAPI::UnitTypes::Terran_Bunker &&
                _terranMainWallValid)
            {
                auto future = terrainRouteCache.find(key);
                if (future == terrainRouteCache.end())
                {
                    std::vector<BWAPI::TilePosition> terrainPath;
                    the.map.getGroundPath(from, to, terrainPath, 160, false);
                    future = terrainRouteCache.emplace(
                        key, std::move(terrainPath)).first;
                }
                if (std::any_of(
                    future->second.begin(), future->second.end(), covered))
                {
                    return false;
                }
            }
            return true;
        }
        if (std::none_of(path.begin(), path.end(), covered))
        {
            return true;
        }
        if (!open(path.front()) || !open(path.back()))
        {
            return false;
        }

        const int cellCount = mapWidth * mapHeight;
        const auto indexOf = [mapWidth](BWAPI::TilePosition p)
        {
            return p.y * mapWidth + p.x;
        };
        std::vector<unsigned char> visited(cellCount, 0);
        std::queue<BWAPI::TilePosition> pending;
        pending.push(path.front());
        visited[indexOf(path.front())] = 1;
        const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
        const int dy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
        while (!pending.empty())
        {
            const BWAPI::TilePosition current = pending.front();
            pending.pop();
            if (current == path.back())
            {
                return true;
            }
            for (int direction = 0; direction < 8; ++direction)
            {
                const BWAPI::TilePosition next(
                    current.x + dx[direction], current.y + dy[direction]);
                if (!open(next) || visited[indexOf(next)] ||
                    !the.map.isGroundTransitionRuntimeWalkable(current, next))
                {
                    continue;
                }
                // An occupied corner cannot be crossed diagonally, even if
                // the current runtime map still sees it as empty.
                if (dx[direction] && dy[direction])
                {
                    const BWAPI::TilePosition sideX(
                        current.x + dx[direction], current.y);
                    const BWAPI::TilePosition sideY(
                        current.x, current.y + dy[direction]);
                    const bool viaX = open(sideX) &&
                        the.map.isGroundTransitionWalkable(current, sideX) &&
                        the.map.isGroundTransitionWalkable(sideX, next);
                    const bool viaY = open(sideY) &&
                        the.map.isGroundTransitionWalkable(current, sideY) &&
                        the.map.isGroundTransitionWalkable(sideY, next);
                    if (!viaX && !viaY) continue;
                }
                visited[indexOf(next)] = 1;
                pending.push(next);
            }
        }
        return false;
    };

    const Base * main = the.bases.myMain();
    const Base * natural = the.bases.myNatural();
    // A depot's canonical tile is the destination of the main-to-natural
    // route, not an obstacle placed across that route.
    const bool landingAtNaturalDepot = type.isResourceDepot() && natural &&
        tile == natural->getTilePosition();
    if (main && natural &&
        !landingAtNaturalDepot &&
        !routeSurvives(main->getCenterTile(), natural->getCenterTile()))
    {
        return false;
    }
    for (const Base * base : the.bases.getAll())
    {
        if (base && type.isResourceDepot() &&
            tile == base->getTilePosition() &&
            !routeSurvives(base->getMineralLineTile(), base->getFrontTile()))
        {
            return false;
        }
        if (base && (base->getOwner() == the.self() || base == natural) &&
            !(type.isResourceDepot() && tile == base->getTilePosition()) &&
            !routeSurvives(base->getCenterTile(), base->getFrontTile()))
        {
            return false;
        }
    }
    return true;
}

// Remember blocked production plots even if their original task times out.
void BuildingPlacer::avoidFailedProductionLocation(BWAPI::UnitType type,
    const BWAPI::TilePosition& tile)
{
    if (!tile.isValid() ||
        (type != BWAPI::UnitTypes::Terran_Barracks &&
         type != BWAPI::UnitTypes::Terran_Factory &&
         type != BWAPI::UnitTypes::Terran_Starport) ||
        (type == BWAPI::UnitTypes::Terran_Barracks &&
         _terranMainWallValid && tile == _terranMainWallBarracks)) return;
    for (auto it = _failedProductionLocations.begin(); it != _failedProductionLocations.end(); )
    {
        if (it->second <= the.now()) it = _failedProductionLocations.erase(it);
        else ++it;
    }
    _failedProductionLocations[{ type.getID(),
        tile.x + tile.y * BWAPI::Broodwar->mapWidth() }] = the.now() + 90 * 24;
}

// Place a building other than an expansion.
// The minimum distance between buildings is extraSpace, the extra space we check
// for accessibility around each potential building location.
BWAPI::TilePosition BuildingPlacer::getBuildLocationNear(const Building & b, int extraSpace) const
{
    // BWAPI::Broodwar->printf("Building Placer seeks position near %d, %d", b.desiredPosition.x, b.desiredPosition.y);

    BWAPI::TilePosition tile = getTerranMainWallLocation(b);
    const bool usedTerranWallSlot = tile.isValid();
    
    if (!tile.isValid())
    {
        tile = findSpecialLocation(b);
    }
    
    if (!tile.isValid())
    {
        tile = findAnyLocation(b, extraSpace);
    }

    // Let Bases decide whether to change which base is the main base.  A wall
    // slot intentionally lies at the border of two Areas, so it must not be
    // mistaken for repeated failure to find room in the main.
    if (!usedTerranWallSlot)
    {
        the.bases.checkBuildingPosition(b.desiredPosition, tile);
    }

    return tile;		// may be None
}

bool BuildingPlacer::isTerranMainWallBarracks(BWAPI::Unit unit) const
{
    if (!_terranMainWallValid || !unit || !unit->exists() ||
        unit->getType() != BWAPI::UnitTypes::Terran_Barracks)
    {
        return false;
    }

    if (!unit->isLifted() && unit->getTilePosition() == _terranMainWallBarracks)
    {
        _terranMainWallBarracksID = unit->getID();
    }
    return unit->getID() == _terranMainWallBarracksID;
}


bool BuildingPlacer::isTerranMainWallBuilding(BWAPI::Unit unit) const
{
    if (!_terranMainWallValid || !unit || !unit->exists())
    {
        return false;
    }

    const BWAPI::UnitType type = unit->getType();
    const BWAPI::TilePosition current = unit->getTilePosition();

    if (type == BWAPI::UnitTypes::Terran_Barracks)
    {
        return isTerranMainWallBarracks(unit);
    }

    if (type != BWAPI::UnitTypes::Terran_Supply_Depot)
    {
        return false;
    }

    return
        current == _terranMainWallDepot1 ||
        current == _terranMainWallDepot2;
}

bool BuildingPlacer::isTerranMainWallComplete(bool allowLiftedGate) const
{
    if (!_terranMainWallValid)
    {
        return false;
    }

    bool barracksReady = false;
    bool depot1Ready = false;
    bool depot2Ready = !_terranMainWallDepot2.isValid();

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isCompleted())
        {
            continue;
        }

        const BWAPI::UnitType type = unit->getType();
        const BWAPI::TilePosition current = unit->getTilePosition();

        if (type == BWAPI::UnitTypes::Terran_Barracks &&
            ((!unit->isLifted() && current == _terranMainWallBarracks) ||
             (allowLiftedGate && unit->isLifted() && isTerranMainWallBarracks(unit) &&
              unit->getDistance(getTerranMainWallGateCenter()) <= 4 * 32)))
        {
            barracksReady = true;
        }
        else if (type == BWAPI::UnitTypes::Terran_Supply_Depot)
        {
            if (current == _terranMainWallDepot1)
            {
                depot1Ready = true;
            }
            if (current == _terranMainWallDepot2)
            {
                depot2Ready = true;
            }
        }
    }

    return barracksReady && depot1Ready && depot2Ready;
}

BWAPI::Position BuildingPlacer::getTerranMainWallCenter() const
{
    if (!_terranMainWallValid ||
        !_terranMainWallBarracks.isValid() ||
        !_terranMainWallDepot1.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position barracksCenter =
        wallBuildingCenter(BWAPI::UnitTypes::Terran_Barracks, _terranMainWallBarracks);
    const BWAPI::Position depot1Center =
        wallBuildingCenter(BWAPI::UnitTypes::Terran_Supply_Depot, _terranMainWallDepot1);
    BWAPI::Position sum = barracksCenter + depot1Center;
    int count = 2;
    if (_terranMainWallDepot2.isValid())
    {
        sum += wallBuildingCenter(BWAPI::UnitTypes::Terran_Supply_Depot, _terranMainWallDepot2);
        ++count;
    }
    return BWAPI::Position(sum.x / count, sum.y / count);
}

std::string BuildingPlacer::terranMainWallReadyStatus() const
{
    std::string ready = isTerranMainWallComplete() ? "WALL: CLOSED" : "WALL: PLAN";
    ready += _terranMainWallDepot2.isValid() ? " 2 depots" : " 1 depot";
    ready += _terranMainWallCentralGate ? " central gate" : " side gate";
    ready += _terranMainWallCoversNatural ? " natural line" : " main line";
    if (_terranMainWallUsedDestructibleRoute)
    {
        return ready + " (opened-neutral route)";
    }
    if (_terranMainWallUsedRouteFallback)
    {
        return ready + " (route fallback)";
    }
    return ready + " (GridZone choke)";
}

bool BuildingPlacer::isInsideTerranMainWall(
    const BWAPI::Position & position,
    int marginPixels) const
{
    const BWAPI::Position wallCenter = getTerranMainWallGateCenter();
    if (!_terranMainWallValid || !position.isValid() ||
        !wallCenter.isValid() || !_terranMainWallInside.isValid() ||
        !_terranMainWallOutside.isValid())
    {
        return false;
    }

    double forwardX = double(
        _terranMainWallOutside.x - _terranMainWallInside.x);
    double forwardY = double(
        _terranMainWallOutside.y - _terranMainWallInside.y);
    const double length = std::sqrt(
        forwardX * forwardX + forwardY * forwardY);
    if (length < 1.0)
    {
        return false;
    }
    forwardX /= length;
    forwardY /= length;

    const double projection =
        double(position.x - wallCenter.x) * forwardX +
        double(position.y - wallCenter.y) * forwardY;
    return projection <= double(marginPixels);
}

bool BuildingPlacer::crossesTerranMainWall(
    const BWAPI::Position & from,
    const BWAPI::Position & to,
    int deadZonePixels) const
{
    const BWAPI::Position wallCenter = getTerranMainWallGateCenter();
    if (!_terranMainWallValid || !from.isValid() || !to.isValid() ||
        !wallCenter.isValid() || !_terranMainWallInside.isValid() ||
        !_terranMainWallOutside.isValid())
    {
        return false;
    }

    double forwardX = double(
        _terranMainWallOutside.x - _terranMainWallInside.x);
    double forwardY = double(
        _terranMainWallOutside.y - _terranMainWallInside.y);
    const double length = std::sqrt(
        forwardX * forwardX + forwardY * forwardY);
    if (length < 1.0)
    {
        return false;
    }
    forwardX /= length;
    forwardY /= length;

    const double fromProjection =
        double(from.x - wallCenter.x) * forwardX +
        double(from.y - wallCenter.y) * forwardY;
    const double toProjection =
        double(to.x - wallCenter.x) * forwardX +
        double(to.y - wallCenter.y) * forwardY;
    const double deadZone = double(std::max(0, deadZonePixels));

    return
        (fromProjection <= -deadZone && toProjection >= deadZone) ||
        (fromProjection >= deadZone && toProjection <= -deadZone);
}

BWAPI::Position BuildingPlacer::terranMainWallTransitWaypoint(
    const BWAPI::Position & from,
    const BWAPI::Position & to,
    int moverID) const
{
    const BWAPI::Position gate = getTerranMainWallGateCenter();
    if (!_terranMainWallValid || !from.isValid() || !to.isValid() ||
        !gate.isValid() ||
        (!isNearTerranMainWall(from, 18 * 32) &&
         !isNearTerranMainWall(to, 18 * 32)))
    {
        return BWAPI::Positions::Invalid;
    }

    // The permanent release is a change of routing policy, not merely an
    // opened temporary gate. Once the old Barracks footprint is physically
    // free, let ground units use their ordinary destination/path. Forcing
    // every Tank and Vulture to one of five pixels inside that footprint
    // kept a large army milling around the former gate indefinitely.
    if (_terranMainWallReleased &&
        the.map.isRuntimeWalkable(BWAPI::TilePosition(gate)))
    {
        return BWAPI::Positions::Invalid;
    }

    double fx = double(_terranMainWallOutside.x - _terranMainWallInside.x);
    double fy = double(_terranMainWallOutside.y - _terranMainWallInside.y);
    const double length = std::hypot(fx, fy);
    if (length < 1.0) return BWAPI::Positions::Invalid;
    fx /= length;
    fy /= length;
    const double sx = -fy, sy = fx;
    const double fromSide = (from.x - gate.x) * fx + (from.y - gate.y) * fy;
    const double toSide = (to.x - gate.x) * fx + (to.y - gate.y) * fy;
    // Once the unit has crossed the middle of the opening, let it continue
    // toward its real destination. The old 80-pixel overlap sent units which
    // were already outside back to the former Barracks footprint.
    const bool outward = toSide >= 24.0 && fromSide <= 16.0;
    const bool inward = toSide <= -24.0 && fromSide >= -16.0;
    if (!outward && !inward) return BWAPI::Positions::Invalid;

    // This helper is queried for many moving units on one frame. Completing
    // the wall scans our buildings, so cache that answer for the frame.
    static int checkedFrame = -1;
    static bool wallComplete = false;
    if (!_terranMainWallReleased)
    {
        if (checkedFrame != BWAPI::Broodwar->getFrameCount())
        {
            checkedFrame = BWAPI::Broodwar->getFrameCount();
            wallComplete = isTerranMainWallComplete(true);
        }
        if (!wallComplete) return BWAPI::Positions::Invalid;
    }
    const double direction = outward ? 1.0 : -1.0;

    // Aim past the Barracks footprint so the gate controller sees a genuine
    // crossing request. Never choose a Depot-side point as the waypoint.
    for (int depth : { 96, 112, 128, 144 })
    {
        for (int side : { 0, -16, 16, -32, 32 })
        {
            const BWAPI::Position candidate(
                int(gate.x + fx * direction * depth + sx * side),
                int(gate.y + fy * direction * depth + sy * side));
            const BWAPI::TilePosition tile(candidate);
            if (candidate.isValid() && tile.isValid() &&
                the.map.isRuntimeWalkable(tile))
            {
                return candidate;
            }
        }
    }
    return BWAPI::Positions::Invalid;
}

void BuildingPlacer::markTerranMainWallOpened()
{
    _terranMainWallOpened = true;
    _terranMainWallStatus = "WALL: OPEN (temporary gate)";
}

void BuildingPlacer::markTerranMainWallClosed()
{
    _terranMainWallOpened = false;
    _terranMainWallStatus = terranMainWallReadyStatus();
}

void BuildingPlacer::markTerranMainWallReleasedForVision()
{
    const bool firstRelease = !_terranMainWallReleased;
    _terranMainWallOpened = true;
    _terranMainWallReleased = true;
    _terranMainWallStatus = "WALL: RELEASED (expansion traffic)";
    if (firstRelease)
    {
        reserveOpenMainExitLane();
    }
}

bool BuildingPlacer::isNearTerranMainWall(
    const BWAPI::Position & position,
    int radius) const
{
    const BWAPI::Position center = getTerranMainWallCenter();
    return
        center.isValid() &&
        position.isValid() &&
        center.getApproxDistance(position) <= std::max(0, radius);
}

void BuildingPlacer::reserveTerranMainWallBunker()
{
    clearReserveBits(ReserveWallBunker);
    _terranMainWallBunker = findTerranMainWallBunkerLocation(true);
    if (_terranMainWallBunker.isValid())
        setReserve(_terranMainWallBunker, 3, 2, ReserveWallBunker, true);
}

BWAPI::TilePosition BuildingPlacer::getTerranMainWallBunkerLocation() const
{
    if (!_terranMainWallValid) return BWAPI::TilePositions::None;
    const BWAPI::UnitType bunkerType = BWAPI::UnitTypes::Terran_Bunker;
    if (_terranMainWallBunker.isValid())
    {
        bool available = true;
        for (int x = _terranMainWallBunker.x; x < _terranMainWallBunker.x + 3; ++x)
            for (int y = _terranMainWallBunker.y; y < _terranMainWallBunker.y + 2; ++y)
                if ((_reserveMap[x][y] & ReserveBuilding) ||
                    !BWAPI::Broodwar->isBuildable(x, y, true)) available = false;
        if (available && preservesGroundExit(bunkerType, _terranMainWallBunker))
            return _terranMainWallBunker;
    }
    const BWAPI::TilePosition fallback = findTerranMainWallBunkerLocation(false);
    return fallback.isValid() && preservesGroundExit(bunkerType, fallback)
        ? fallback : BWAPI::TilePositions::None;
}

BWAPI::TilePosition BuildingPlacer::findTerranMainWallBunkerLocation(bool terrainOnly) const
{
    if (!_terranMainWallValid)
    {
        return BWAPI::TilePositions::None;
    }

    const BWAPI::UnitType bunkerType = BWAPI::UnitTypes::Terran_Bunker;
    Building bunker(bunkerType, _terranMainWallBarracks);
    bunker.macroLocation = MacroLocation::Main;

    const WallBlock barracks(BWAPI::UnitTypes::Terran_Barracks, _terranMainWallBarracks);
    const WallBlock depot1(BWAPI::UnitTypes::Terran_Supply_Depot, _terranMainWallDepot1);
    const WallBlock depot2(BWAPI::UnitTypes::Terran_Supply_Depot, _terranMainWallDepot2);
    std::vector<WallBlock> wall = { barracks, depot1 };
    if (_terranMainWallDepot2.isValid()) wall.push_back(depot2);

    int minX = BWAPI::Broodwar->mapWidth();
    int minY = BWAPI::Broodwar->mapHeight();
    int maxX = 0;
    int maxY = 0;
    for (const WallBlock & block : wall)
    {
        minX = std::min(minX, block.tile.x);
        minY = std::min(minY, block.tile.y);
        maxX = std::max(maxX, block.tile.x + block.type.tileWidth() - 1);
        maxY = std::max(maxY, block.tile.y + block.type.tileHeight() - 1);
    }

    auto pixelGap = [](const PixelBox & a, const PixelBox & b)
    {
        const int dx = std::max(
            0,
            std::max(b.left - a.right - 1, a.left - b.right - 1));
        const int dy = std::max(
            0,
            std::max(b.top - a.bottom - 1, a.top - b.bottom - 1));
        return dx + dy;
    };

    BWAPI::TilePosition best = BWAPI::TilePositions::None;
    long long bestScore = std::numeric_limits<long long>::max();

    // Search only around the wall. A 5-tile skirt is enough for a 3x2 Bunker
    // while keeping this routine deterministic and cheap.
    for (int y = std::max(0, minY - 5);
         y <= std::min(BWAPI::Broodwar->mapHeight() - bunkerType.tileHeight(), maxY + 5);
         ++y)
    {
        for (int x = std::max(0, minX - 5);
             x <= std::min(BWAPI::Broodwar->mapWidth() - bunkerType.tileWidth(), maxX + 5);
             ++x)
        {
            const BWAPI::TilePosition tile(x, y);
            if (terrainOnly)
            {
                // The wall layout is known before its buildings exist and
                // before exploration reaches the ramp. Plan using terrain and
                // reservations, then validate the real build command normally.
                bool free = !boxOverlapsBase(x, y, x + 2, y + 1);
                for (int bx = x; bx < x + 3 && free; ++bx)
                    for (int by = y; by < y + 2 && free; ++by)
                        free = the.map.isBuildable(BWAPI::TilePosition(bx, by)) &&
                            BWAPI::Broodwar->isBuildable(bx, by, true) &&
                            _reserveMap[bx][by] == 0 &&
                            !tileBlocksAddon(BWAPI::TilePosition(bx, by));
                if (!free) continue;
            }
            else if (!canBuildWithSpace(tile, bunker, 0)) continue;

            bool overlapsWall = false;
            for (const WallBlock & block : wall)
            {
                if (wallFootprintsOverlap(
                        bunkerType, tile, block.type, block.tile))
                {
                    overlapsWall = true;
                    break;
                }
            }
            if (overlapsWall)
            {
                continue;
            }

            const WallBlock bunkerBlock(bunkerType, tile);
            const PixelBox bunkerBox = wallBuildingBox(bunkerBlock);
            const int gapDepot1 = pixelGap(bunkerBox, wallBuildingBox(depot1));
            const int gapDepot2 = _terranMainWallDepot2.isValid()
                ? pixelGap(bunkerBox, wallBuildingBox(depot2)) : gapDepot1;
            const int gapBarracks = pixelGap(bunkerBox, wallBuildingBox(barracks));

            const BWAPI::Position bunkerCenter =
                wallBuildingCenter(bunkerType, tile);

            const int dDepot1 = bunkerCenter.getApproxDistance(
                wallBuildingCenter(depot1.type, depot1.tile));
            const int dDepot2 = _terranMainWallDepot2.isValid()
                ? bunkerCenter.getApproxDistance(wallBuildingCenter(depot2.type, depot2.tile))
                : dDepot1;
            const int dBarracks = bunkerCenter.getApproxDistance(
                wallBuildingCenter(barracks.type, barracks.tile));

            if (!isInsideTerranMainWall(bunkerCenter, -16)) continue;
            // Leave a Tank-width corridor behind the actual lift gate. A
            // Bunker on that corridor would invalidate the wall's open route.
            const BWAPI::Position gate = getTerranMainWallGateCenter();
            double fx = double(_terranMainWallOutside.x - _terranMainWallInside.x);
            double fy = double(_terranMainWallOutside.y - _terranMainWallInside.y);
            const double length = std::hypot(fx, fy);
            if (length > 0.0)
            {
                fx /= length; fy /= length;
                const double forward = (bunkerCenter.x - gate.x) * fx + (bunkerCenter.y - gate.y) * fy;
                const double side = -(bunkerCenter.x - gate.x) * fy + (bunkerCenter.y - gate.y) * fx;
                const double halfSide = std::abs(fy) * 48 + std::abs(fx) * 32;
                if (forward > -6 * 32 && forward < 32 && std::abs(side) < halfSide + 24) continue;
            }

            // Main-side placement protects the wall without occupying the
            // outside approach. The two Depots are the primary assets, then
            // the Barracks. Pixel-box gaps are weighted heavily so an actually
            // adjacent tile wins over a merely nearby tile.
            long long score = 0;
            score += 40LL * std::max(gapDepot1, gapDepot2);
            score += 18LL * (gapDepot1 + gapDepot2);
            score += 5LL * gapBarracks;
            score += 2LL * std::max(dDepot1, dDepot2);
            score += dBarracks;

            const bool bunkerInside =
                isInsideTerranMainWall(bunkerCenter, 16);
            if (!bunkerInside)
            {
                score += 200000;
            }

            // Keep the gate itself clear: being close to both Depots is good,
            // but do not put the Bunker directly on the Barracks opening lane.
            if (dBarracks < 3 * 32)
            {
                score += 30000;
            }

            // Prefer a single Bunker that can cover all three wall pieces.
            if (std::max(dBarracks, std::max(dDepot1, dDepot2)) > 7 * 32)
            {
                score += 50000;
            }

            if (score < bestScore && preservesGroundExit(bunkerType, tile))
            {
                bestScore = score;
                best = tile;
            }
        }
    }

    return best;
}

void BuildingPlacer::reserveTiles(const BWAPI::TilePosition & position, int width, int height)
{
    setReserve(position, width, height, ReserveBuilding, true);
}

void BuildingPlacer::reserveTiles(const Building & b)
{
    if (!b.finalPosition.isValid())
    {
        return;
    }
    int width = b.type.tileWidth();
    if (b.type.canBuildAddon())
    {
        width += 2;
    }
    setReserve(b.finalPosition, width, b.type.tileHeight(), ReserveBuilding, true);
}

void BuildingPlacer::freeTiles(const BWAPI::TilePosition & position, int width, int height)
{
    setReserve(position, width, height, ReserveBuilding, false);
}

void BuildingPlacer::freeTiles(const Building & b)
{
    if (!b.finalPosition.isValid())
    {
        return;
    }
    int width = b.type.tileWidth();
    if (b.type.canBuildAddon())
    {
        width += 2;
    }
    freeTiles(b.finalPosition, width, b.type.tileHeight());
}

bool BuildingPlacer::isReserved(int x, int y) const
{
    UAB_ASSERT(BWAPI::TilePosition(x, y).isValid(), "bad tile");

    return _reserveMap[x][y] != 0;
}

void BuildingPlacer::drawReservedTiles() const
{
    // _buildable.draw();
    if (Config::Debug::DrawReservedBuildingTiles && !_reserveMap.empty())
    {
        int rwidth = _reserveMap.size();
        int rheight = _reserveMap[0].size();

        for (int x = 0; x < rwidth; ++x)
        {
            for (int y = 0; y < rheight; ++y)
            {
                if (_reserveMap[x][y])
                {
                    int x1 = x*32 + 3;
                    int y1 = y*32 + 3;
                    int x2 = (x+1)*32 - 3;
                    int y2 = (y+1)*32 - 3;

                    BWAPI::Broodwar->drawBoxMap(
                        x1, y1, x2, y2, BWAPI::Colors::Grey);
                }
            }
        }
    }

    // A failed wall used to leave no map marker and most early-return reasons
    // were visible only when the full game-info overlay was enabled. Keep the
    // analyzed choke/route visible during the opening so a bad map decision is
    // immediately distinguishable from a production-order problem.
    if (!_terranMainWallValid &&
        the.selfRace() == BWAPI::Races::Terran &&
        the.now() <= 5 * 60 * 24 &&
        _terranMainWallAnalysisCenter.isValid())
    {
        BWAPI::Broodwar->drawCircleMap(
            _terranMainWallAnalysisCenter, 18,
            BWAPI::Colors::Red, false);
        if (_terranMainWallChokeEndA.isValid() && _terranMainWallChokeEndB.isValid())
        {
            const BWAPI::Position middle(
                (_terranMainWallChokeEndA.x + _terranMainWallChokeEndB.x) / 2,
                (_terranMainWallChokeEndA.y + _terranMainWallChokeEndB.y) / 2);
            BWAPI::Broodwar->drawLineMap(
                _terranMainWallChokeEndA, _terranMainWallChokeEndB, BWAPI::Colors::Yellow);
            BWAPI::Broodwar->drawLineMap(
                _terranMainWallAnalysisCenter, middle, BWAPI::Colors::Red);
        }
        BWAPI::Broodwar->drawTextMap(
            _terranMainWallAnalysisCenter + BWAPI::Position(8, -14),
            "%s", _terranMainWallStatus.c_str());
    }

    // Wall slots are operational information, not generic placement debug.
    // Show them during the opening even when the noisy all-reservations overlay
    // is disabled, so a failed/incorrect wall is visible in an ordinary test.
    if (_terranMainWallValid &&
        (Config::Debug::DrawReservedBuildingTiles ||
         the.now() <= 10 * 60 * 24))
    {
        auto drawWallSlot = [](
            const BWAPI::TilePosition & tile,
            BWAPI::UnitType type,
            BWAPI::Color color,
            const char * label)
        {
            if (!tile.isValid()) return;
            const BWAPI::Position topLeft(tile);
            const BWAPI::Position bottomRight(
                (tile.x + type.tileWidth()) * 32 - 1,
                (tile.y + type.tileHeight()) * 32 - 1);
            BWAPI::Broodwar->drawBoxMap(
                topLeft, bottomRight, color, false);
            BWAPI::Broodwar->drawTextMap(
                topLeft + BWAPI::Position(4, 4), "%s", label);
        };

        drawWallSlot(_terranMainWallBunker, BWAPI::UnitTypes::Terran_Bunker,
            BWAPI::Colors::Green, "WALL BUNKER");
        drawWallSlot(
            _terranMainWallBarracks,
            BWAPI::UnitTypes::Terran_Barracks,
            BWAPI::Colors::Cyan,
            _terranMainWallOpened ? "WALL OPEN" : "WALL RAX");
        drawWallSlot(
            _terranMainWallDepot1,
            BWAPI::UnitTypes::Terran_Supply_Depot,
            BWAPI::Colors::Orange,
            "WALL D1");
        drawWallSlot(
            _terranMainWallDepot2,
            BWAPI::UnitTypes::Terran_Supply_Depot,
            BWAPI::Colors::Orange,
            "WALL D2");
    }
}

// The building has been placed. Is the placement valid?
// Used when outside callers want to make sure.
bool BuildingPlacer::buildingOK(const Building & b) const
{
    return buildingOK(b, b.finalPosition);
}

// Is the placement valid for the building?
// Used by outside callers.
// TODO does this check for lack of blocks? apparently not
bool BuildingPlacer::buildingOK(const Building & b, const BWAPI::TilePosition & pos) const
{
    if (!pos.isValid())
    {
        return false;
    }

    // Resource depots/refineries are deliberately placed on special map slots
    // which overlap base/resource reservations. Other buildings must pass the
    // same spacing/reservation/lane validation as ordinary placement.
    if (b.type.isResourceDepot() || b.type.isRefinery())
    {
        return canBuildHere(pos, b);
    }
    return canBuildWithSpace(pos, b, 0);
}

// Find the desired location for a command center, nexus, or hatchery from its macro location.
// It's usually the same as usual, but the "Anywhere" default is different.
BWAPI::TilePosition BuildingPlacer::getExpoLocationTile(MacroLocation loc) const
{
    if (loc == MacroLocation::Anywhere)
    {
        if (the.bases.myNatural() &&
            the.bases.myNatural()->owner == the.neutral() &&
            the.bases.myStart()->owner == the.self())
        {
            loc = MacroLocation::Natural;
        }
        else
        {
            loc = MacroLocation::Expo;
        }
    }

    return getMacroLocationTile(loc);
}

// Generic macro locations on the map.
// If used to place a building, the expectation in most cases is that the returned tile
// will be a starting point; we'll look for an open area nearby to build.
// NOTE Some cases must be treated as special cases, e.g. refinery building locations.
//      See BuildingManager::getBuildingLocation().
BWAPI::TilePosition BuildingPlacer::getMacroLocationTile(MacroLocation loc) const
{
    if (loc == MacroLocation::Main)
    {
        // A main base building, including macro hatchery.
        // The main base is always set, even if we don't have any building there at the moment.
        return the.bases.myMain()->getTilePosition();
    }
    else if (loc == MacroLocation::Natural)
    {
        Base * natural = the.bases.myNatural();
        if (natural)
        {
            return natural->getTilePosition();
        }
    }
    else if (loc == MacroLocation::Front)
    {
        BWAPI::TilePosition front = the.bases.frontTile();
        if (front.isValid())
        {
            return front;
        }
    }
    else if (loc == MacroLocation::Expo)
    {
        // Once Terran owns two working gas bases, compare mineral-only sites
        // with gas sites. A large mineral bank with little gas still needs a
        // gas expansion even if we already own two geysers.
        int ownedGasBases = 0;
        
        {
            for (Base* base : the.bases.getAll())
            {
                if (base && base->getOwner() == the.self() &&
                    base->getInitialGas() > 0 && base->getDepot() &&
                    base->getDepot()->exists() &&
                    base->getDepot()->isCompleted() &&
                    !base->getDepot()->isFlying())
                {
                    ++ownedGasBases;
                }
            }
        }
        const bool gasStarved =
            the.selfRace() == BWAPI::Races::Terran &&
            the.self()->minerals() >= 900 && the.self()->gas() <= 125;
        BWAPI::TilePosition pos = the.map.getNextExpansion(
            false, true, ownedGasBases < 2 || gasStarved);
        if (pos.isValid())
        {
            return pos;
        }
    }
    else if (loc == MacroLocation::MinOnly)
    {
        // Mineral base with or without gas geyser.
        BWAPI::TilePosition pos = the.map.getNextExpansion(false, true, false);
        if (pos.isValid())
        {
            return pos;
        }
    }
    else if (loc == MacroLocation::GasOnly)
    {
        // Gas base with or without minerals.
        BWAPI::TilePosition pos = the.map.getNextExpansion(false, false, true);
        if (pos.isValid())
        {
            return pos;
        }
    }
    else if (loc == MacroLocation::Hidden)
    {
        // "Hidden" mineral and gas base.
        BWAPI::TilePosition pos = the.map.getNextExpansion(true, true, true);
        if (pos.isValid())
        {
            return pos;
        }
    }
    else if (loc == MacroLocation::Center)
    {
        // Near the center of the map. Find a walkable tile connected to the start.
        return connectedWalkableTileNear(BWAPI::TilePosition(BWAPI::Broodwar->mapWidth() / 2, BWAPI::Broodwar->mapHeight() / 2));
    }
    else if (loc == MacroLocation::Proxy)
    {
        if (the.bases.enemyStart())
        {
            // We know where the enemy is. We can proxy in or close to the enemy base.
            // Other code should try to find the enemy base first!
            BWAPI::TilePosition proxy = getProxyPosition(the.bases.enemyStart());
            if (proxy.isValid())
            {
                return proxy;
            }
        }
        // We don't know where the enemy is, or can't find a close proxy position,
        // but we can at least proxy to the center of the map.
        return getMacroLocationTile(MacroLocation::Center);
    }
    else if (loc == MacroLocation::EnemyMain)
    {
        if (the.bases.enemyStart())
        {
            return the.bases.enemyStart()->getTilePosition();
        }
        return getMacroLocationTile(MacroLocation::Center);
    }
    else if (loc == MacroLocation::EnemyNatural)
    {
        if (the.bases.enemyStart() && the.bases.enemyStart()->getNatural())
        {
            return the.bases.enemyStart()->getNatural()->getTilePosition();
        }
        return getMacroLocationTile(MacroLocation::Center);
    }

    // Default: Build in the current main base, which is guaranteed to exist (though it may be empty or in enemy hands).
    // MacroLocation::Anywhere falls through to here. So does MacroLocation::Tile.
    return the.bases.myMain()->getTilePosition();
}

BWAPI::Position BuildingPlacer::getMacroLocationPos(MacroLocation loc) const
{
    return TileCenter(getMacroLocationTile(loc));
}

// NOTE This allows building only on visible geysers.
BWAPI::TilePosition BuildingPlacer::getRefineryPosition() const
{
    BWAPI::TilePosition closestGeyser = BWAPI::TilePositions::None;
    int minGeyserDistanceFromHome = 100000;
    BWAPI::Position homePosition = the.bases.myMain()->getPosition();

    for (BWAPI::Unit geyser : BWAPI::Broodwar->getGeysers())
    {
        // Check to see if the geyser is near one of our depots.
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (unit->getType().isResourceDepot() && unit->getDistance(geyser) < 300)
            {
                // Don't take a geyser which is in enemy static defense range. It'll just die.
                // This is rare so we check it only after other checks succeed.
                if (the.groundHitsFixed.inRange(geyser->getType(), geyser->getTilePosition()))
                {
                    break;
                }

                int homeDistance = geyser->getDistance(homePosition);

                if (homeDistance < minGeyserDistanceFromHome)
                {
                    minGeyserDistanceFromHome = homeDistance;
                    closestGeyser = geyser->getTilePosition();      // BWAPI bug workaround by Arrak
                }
                break;
            }
        }
    }
    
    return closestGeyser;
}

// Near the given base, protected by terrain, in range of the minerals.
// Only possible on a few maps, but a potential game-winner when possible.
// Return an invalid tile on failure.
// TODO in progress
BWAPI::TilePosition BuildingPlacer::getTerrainProxyPosition(const Base * base) const
{
    const int range = 9;

    // A fictitious large building to place.
    Building b(BWAPI::UnitTypes::Protoss_Nexus, BWAPI::TilePositions::None);

    // Compute the bounding box of the minerals, in tiles.
    int left = MAX_DISTANCE;
    int right = -1;
    int top = MAX_DISTANCE;
    int bottom = -1;
    for (BWAPI::Unit patch : base->getMinerals())
    {
        left   = std::min(left,   patch->getInitialTilePosition().x);
        right  = std::max(right,  patch->getInitialTilePosition().x + 1);    // patch size is 2x1
        top    = std::min(top,    patch->getInitialTilePosition().y);
        bottom = std::max(bottom, patch->getInitialTilePosition().y);
    }

    // Find the position which is in range and hits the most mineral patches,
    // with greater ground distance as the tiebreaker.
    BWAPI::TilePosition bestTile = BWAPI::TilePositions::Invalid;
    int bestHits = 2;
    int bestDist = 0;
    for (int x = left - range; x <= right + range; ++x)
    {
        for (int y = top - range; y <= bottom + range; ++y)
        {
            BWAPI::TilePosition xy(x, y);
            if (xy.isValid())
            {
                int groundDist = base->getTileDistance(xy);
                if (groundDist > 20 && canBuildHere(xy, b))
                {
                    int hits = countInRange(base->getMinerals(), TileCenter(xy), 32 * range);
                    if (hits > bestHits || hits == bestHits && groundDist > bestDist)
                    {
                        bestTile = xy;
                        bestHits = hits;
                        bestDist = groundDist;
                        /*
                        BWAPI::Broodwar->printf("xy %d,%d ground dist %d hits %d",
                            xy.x, xy.y, groundDist, hits);
                        BWAPI::Broodwar->printf("   best %d,%d, ground dist %d hits %d",
                            bestTile.x, bestTile.y, bestDist, bestHits);
                        */
                    }
                }
            }
        }
    }

    return bestTile;
}

// In the given base, as far away as possible from the enemy's position
// and from the closest entrance.
// Return an invalid tile on failure.
BWAPI::TilePosition BuildingPlacer::getInBaseProxyPosition(const Base * base) const
{
    // Tiles sorted in order of closeness to the enemy main resource depot.
    const BWAPI::TilePosition enemyCenter = base->getCenterTile();
    const std::vector<BWAPI::TilePosition> & closest = the.map.getClosestTilesTo(enemyCenter);

    // A fictitious large building to place.
    Building b(BWAPI::UnitTypes::Protoss_Nexus, BWAPI::TilePositions::None);

    const int myMinDist = the.bases.myMain()->getTileDistance(base->getTilePosition());
    int bestScore = 16;
    BWAPI::TilePosition bestTile = BWAPI::TilePositions::None;
    for (const BWAPI::TilePosition & tile : closest)
    {
        const int enemyDistX = abs(tile.x - enemyCenter.x);
        const int enemyDistY = abs(tile.y - enemyCenter.y);
        const int myDist = the.bases.myMain()->getTileDistance(tile) - myMinDist;
        const int score = enemyDistX + enemyDistY + myDist;
        if (myDist >= 0 && enemyDistX > 8 && enemyDistY > 8 && score > bestScore)
        {
            if (the.zone.sameArea(tile, enemyCenter) && canBuildHere(tile, b))
            {
                bestScore = score;
                bestTile = tile;
            }
        }
        else if (enemyDistX > 24)
        {
            // Too far away. Stop looking.
            break;
        }
    }
    return bestTile;
}

// Return an invalid tile on failure.
BWAPI::TilePosition BuildingPlacer::getProxyPosition(const Base * base) const
{
    UAB_ASSERT(base, "bad base");

    BWAPI::TilePosition tile = BWAPI::TilePositions::Invalid;

    Base * natural = base->getNatural();
    if (natural)
    {
        // In the enemy natural, hidden by terrain. Only possible on a few maps.
        // TODO disabled for now, needs improved building placement skills + pathing
        // tile = getTerrainProxyPosition(natural);
    }
    if (!tile.isValid())
    {
        // In a far corner of the enemy main.
        tile = getInBaseProxyPosition(base);
    }

    return tile;
}

// A zerg base has been bunkered by a terran opponent.
// Figure out where a sunk can be placed out of bunker range but in range to hit the bunker.
// Return an invalid tile on failure.
BWAPI::TilePosition BuildingPlacer::getAntiBunkerSunkenPosition(const Base * base, BWAPI::Unit bunker) const
{
    UAB_ASSERT(base, "bad base");
    UAB_ASSERT(bunker, "bad unit");

    Building creep(BWAPI::UnitTypes::Zerg_Creep_Colony, BWAPI::TilePositions::None);

    int closestToMain = MAX_DISTANCE;

    for (int x = base->getCenterTile().x - 7; x <= base->getCenterTile().x + 7; ++x)
    {
        for (int y = base->getCenterTile().y - 7; y <= base->getCenterTile().y + 7; ++y)
        {
            BWAPI::TilePosition xy(x, y);
            if (xy.isValid())
            {
                int dist = bunker->getDistance(BWAPI::Position(TileCenter(xy)));
                if (dist > 5 * 32 &&        // sunken is out of bunker range
                    dist < 7 * 32 &&        // bunker is in sunken range
                    canBuildHere(xy, creep))
                {
                    return xy;
                }
            }
        }
    }

    return BWAPI::TilePositions::Invalid;
}

// A zerg base has cannons nearby.
// 1. Find where to place a sunk to hit a pylon, exploiting the sunken range bug.
// 2. Failing that, where to place it to prevent cannons from creeping closer.
// Return an invalid tile on failure.
BWAPI::TilePosition BuildingPlacer::getAntiCannonSunkenPosition(const Base * base, BWAPI::Unit cannon) const
{
    UAB_ASSERT(base, "bad base");
    UAB_ASSERT(cannon, "bad unit");

    Building creep(BWAPI::UnitTypes::Zerg_Creep_Colony, BWAPI::TilePositions::None);

    int totalDist = cannon->getDistance(base->getCenter());
    UAB_ASSERT(totalDist <= 16 * 32, "too distant");
    int bestCannonDist = totalDist;
    int bestPylonDist = totalDist;
    int closestToMainVsCannon = MAX_DISTANCE;
    int closestToMainVsPylon = MAX_DISTANCE;

    BWAPI::TilePosition vsCannonTile = BWAPI::TilePositions::Invalid;
    BWAPI::TilePosition vsPylonTile = BWAPI::TilePositions::Invalid;

    // As close to the target as possible without being in cannon range.
    // As a second consideration, as close as possible to the main base, to protect the entrance.
    int offset = std::min(9, (totalDist + 31)/32);
    for (int x = base->getCenterTile().x - offset; x <= base->getCenterTile().x + offset; ++x)
    {
        for (int y = base->getCenterTile().y - offset; y <= base->getCenterTile().y + offset; ++y)
        {
            BWAPI::TilePosition xy(x, y);
            if (xy.isValid() && BWAPI::Broodwar->hasCreep(xy))
            {
                // The approximate center of where the cannon will be if placed at tile xy.
                const BWAPI::Position xyCenter = BWAPI::Position(xy) + BWAPI::Position(32, 32);

                const int cannonDist = cannon->getDistance(xyCenter);       // in pixels
                if (cannonDist >= 7 * 32 + 31  &&
                    canBuildHere(xy, creep) &&
                    nullptr == BWAPI::Broodwar->getClosestUnit(
                        xyCenter,
                        (BWAPI::Filter::GetType == BWAPI::UnitTypes::Zerg_Sunken_Colony || BWAPI::Filter::GetType == BWAPI::UnitTypes::Zerg_Creep_Colony) && BWAPI::Filter::IsOwned,
                        6 * 32))
                {
                    const int mainDist = base->getMain() ? base->getMain()->getTileDistance(xy) : MAX_DISTANCE;     // in tiles

                    // Place a sunken out of range of the cannon but as close as possible, to prevent cannons from creeping closer.
                    if (cannonDist < bestCannonDist || cannonDist == bestCannonDist && mainDist < closestToMainVsCannon)
                    {
                        bestCannonDist = cannonDist;
                        vsCannonTile = xy;
                        closestToMainVsCannon = mainDist;
                    }

                    if (Config::Skills::UseSunkenRangeBug)
                    {
                        // Place a sunken close enough to hit a pylon. Then when the cannon fires, the sunken will target the
                        // cannon even though the cannon is out of range.
                        BWAPI::Unit pylon = BWAPI::Broodwar->getClosestUnit(
                            xyCenter,
                            BWAPI::Filter::GetType == BWAPI::UnitTypes::Protoss_Pylon && BWAPI::Filter::IsEnemy,
                            7 * 32
                        );
                        if (pylon)
                        {
                            const int pylonDist = pylon->getDistance(xyCenter);     // in pixels
                            if (pylonDist < bestPylonDist || pylonDist == bestPylonDist && mainDist < closestToMainVsPylon)
                            {
                                bestPylonDist = pylonDist;
                                vsPylonTile = xy;
                                closestToMainVsPylon = mainDist;
                            }
                        }
                    }
                }
            }
        }
    }

    return vsPylonTile.isValid() ? vsPylonTile : vsCannonTile;
}
