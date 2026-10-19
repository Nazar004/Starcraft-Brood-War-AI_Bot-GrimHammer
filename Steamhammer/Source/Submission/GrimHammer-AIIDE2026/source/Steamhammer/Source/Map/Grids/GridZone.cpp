#include "Map/Grids/GridZone.h"

#include "Core/Common.h"
#include "Core/The.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <map>
#include <queue>

using namespace UAlbertaBot;

namespace
{
// A build tile can contain usable ground even when its top-left 8x8 cell
// is a cliff. MapPartitions::id(TilePosition) samples only that corner.
// Use the roomiest actual walk cell for this coarse topology layer.
int buildTileGroundPartition(const BWAPI::TilePosition & tile)
{
    if (!tile.isValid())
    {
        return 0;
    }

    int partition = 0;
    int bestRoom = -1;
    for (int dx = 0; dx < 4; ++dx)
    {
        for (int dy = 0; dy < 4; ++dy)
        {
            const BWAPI::WalkPosition walk(tile.x * 4 + dx, tile.y * 4 + dy);
            const int candidate = the.partitions.id(walk);
            const int room = the.vWalkRoom.at(walk);
            if (candidate > 0 && room > bestRoom)
            {
                partition = candidate;
                bestRoom = room;
            }
        }
    }
    return partition;
}

// Return a real 8x8 walk connection across the common border of two adjacent
// 32x32 build tiles. Merely having walkable pixels somewhere inside both build
// tiles is NOT enough: on cliff maps the two tiles may touch geometrically while
// ground units cannot cross their shared edge. The original Map Analysis 2.0
// area flood made exactly that mistake and could create phantom chokes along a
// cliff wall.
bool getTileEdgeConnection(
    const BWAPI::TilePosition & a,
    const BWAPI::TilePosition & b,
    BWAPI::Position & midpoint,
    int & room)
{
    midpoint = BWAPI::Positions::Invalid;
    room = 0;

    const int dx = b.x - a.x;
    const int dy = b.y - a.y;
    if (std::abs(dx) + std::abs(dy) != 1)
    {
        return false;
    }

    int count = 0;
    int bestCrossingRoom = 0;
    BWAPI::Position bestCrossingPosition = BWAPI::Positions::Invalid;

    if (dx != 0)
    {
        const int walkAX = dx > 0 ? a.x * 4 + 3 : a.x * 4;
        const int walkBX = dx > 0 ? b.x * 4 : b.x * 4 + 3;
        const int baseY = a.y * 4;

        for (int k = 0; k < 4; ++k)
        {
            const BWAPI::WalkPosition wa(walkAX, baseY + k);
            const BWAPI::WalkPosition wb(walkBX, baseY + k);
            if (!wa.isValid() || !wb.isValid() ||
                !the.partitions.walkable(wa) || !the.partitions.walkable(wb))
            {
                continue;
            }

            const BWAPI::Position pa = BWAPI::Position(wa) + BWAPI::Position(4, 4);
            const BWAPI::Position pb = BWAPI::Position(wb) + BWAPI::Position(4, 4);
            const BWAPI::Position m = (pa + pb) / 2;
            ++count;
            const int localRoom =
                std::min(the.vWalkRoom.at(wa), the.vWalkRoom.at(wb));
            if (localRoom > bestCrossingRoom || !bestCrossingPosition.isValid())
            {
                bestCrossingRoom = localRoom;
                bestCrossingPosition = m;
            }
        }
    }
    else
    {
        const int walkAY = dy > 0 ? a.y * 4 + 3 : a.y * 4;
        const int walkBY = dy > 0 ? b.y * 4 : b.y * 4 + 3;
        const int baseX = a.x * 4;

        for (int k = 0; k < 4; ++k)
        {
            const BWAPI::WalkPosition wa(baseX + k, walkAY);
            const BWAPI::WalkPosition wb(baseX + k, walkBY);
            if (!wa.isValid() || !wb.isValid() ||
                !the.partitions.walkable(wa) || !the.partitions.walkable(wb))
            {
                continue;
            }

            const BWAPI::Position pa = BWAPI::Position(wa) + BWAPI::Position(4, 4);
            const BWAPI::Position pb = BWAPI::Position(wb) + BWAPI::Position(4, 4);
            const BWAPI::Position m = (pa + pb) / 2;
            ++count;
            const int localRoom =
                std::min(the.vWalkRoom.at(wa), the.vWalkRoom.at(wb));
            if (localRoom > bestCrossingRoom || !bestCrossingPosition.isValid())
            {
                bestCrossingRoom = localRoom;
                bestCrossingPosition = m;
            }
        }
    }

    if (count <= 0)
    {
        return false;
    }

    midpoint = bestCrossingPosition;
    // We need the best usable crossing on the common edge, not the worst one.
    // A wall-adjacent 8x8 sample must not turn a broad 32px connection into a
    // fake one-walk-tile choke when units can cross through the open samples.
    room = std::max(1, bestCrossingRoom);
    return midpoint.isValid();
}

// Squared perpendicular distance from p to the infinite line a->b. Used only
// as a cheap route-directness tie breaker; topology still decides which areas
// are legal to traverse.
double lineSideDistance(const BWAPI::Position & p,
                        const BWAPI::Position & a,
                        const BWAPI::Position & b)
{
    const double vx = double(b.x - a.x);
    const double vy = double(b.y - a.y);
    const double len = std::sqrt(vx * vx + vy * vy);
    if (len < 1.0)
    {
        return 0.0;
    }
    const double px = double(p.x - a.x);
    const double py = double(p.y - a.y);
    return std::abs(px * vy - py * vx) / len;
}
}

// Zone IDs are >= 0. Zone 0 is the discontinuous unwalkable "not in a zone" zone.
// Positive IDs are high-level terrain zones. Normal zones are areas; narrow
// zones are explicit choke nodes in the area graph.

Zone::Zone(int zoneID)
    : _id(zoneID)
    , _state(ZoneState::Invalid)
    , _groundHeight(-1)
    , _center(BWAPI::Positions::Invalid)
{
}

void Zone::mergeInto(Zone * target)
{
    if (!(isValid() && target && target->isValid() && this != target))
    {
        UAB_ASSERT(false, "invalid zone merger");
        return;
    }

    if (_state == ZoneState::Normal)
    {
        target->_state = ZoneState::Normal;
    }

    if (_groundHeight != target->_groundHeight)
    {
        target->_groundHeight = -1;
    }

    for (BWAPI::TilePosition & tile : _tiles)
    {
        target->_tiles.push_back(tile);
    }
    
    UAB_ASSERT(_neighbors.find(target) != _neighbors.end() && target->_neighbors.find(this) != target->_neighbors.end(), "not neighbors");
    for (Zone * neighbor : neighbors())
    {
        if (neighbor != target)
        {
            target->_neighbors.insert(neighbor);
            neighbor->_neighbors.insert(target);
        }
        neighbor->_neighbors.erase(this);
    }

    invalidate();
}

void Zone::invalidate()
{
    _state = ZoneState::Invalid;
    _groundHeight = -1;
    _tiles.clear();
    _neighbors.clear();
    _center = BWAPI::Positions::Invalid;
}

ChokePoint::ChokePoint()
    : _id(-1)
    , _zone(nullptr)
    , _areaA(nullptr)
    , _areaB(nullptr)
    , _center(BWAPI::Positions::Invalid)
    , _endA(BWAPI::Positions::Invalid)
    , _endB(BWAPI::Positions::Invalid)
    , _widthWalkTiles(0)
{
}

bool ChokePoint::connects(const Zone * a, const Zone * b) const
{
    return a && b &&
        ((_areaA == a && _areaB == b) || (_areaA == b && _areaB == a));
}

Zone * ChokePoint::otherArea(const Zone * area) const
{
    if (area == _areaA)
    {
        return _areaB;
    }
    if (area == _areaB)
    {
        return _areaA;
    }
    return nullptr;
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

void GridZone::clearZones()
{
    for (Zone * zone : zones)
    {
        delete zone;
    }
    zones.clear();
    chokePoints.clear();
    areaGrid.clear();
    areaCanonical.clear();
    areaCenters.clear();
    firstChokeCache.clear();
}

void GridZone::newZoneID(const Zone * zone, int id)
{
    for (const BWAPI::TilePosition & tile : zone->tiles())
    {
        grid[tile.x][tile.y] = id;
    }
}

void GridZone::computeZoneCenters()
{
    for (Zone * zone : zones)
    {
        if (!zone || !zone->isValid() || zone->tiles().empty())
        {
            continue;
        }

        long long sumX = 0;
        long long sumY = 0;
        for (const BWAPI::TilePosition & tile : zone->tiles())
        {
            const BWAPI::Position center = TileCenter(tile);
            sumX += center.x;
            sumY += center.y;
        }

        zone->_center = BWAPI::Position(
            int(sumX / zone->tiles().size()),
            int(sumY / zone->tiles().size()));
    }
}

BWAPI::Position GridZone::findWalkableNear(const BWAPI::Position & pos, const Zone * preferredZone) const
{
    if (!pos.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::WalkPosition origin(pos);
    for (int radius = 0; radius <= 12; ++radius)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            for (int dy = -radius; dy <= radius; ++dy)
            {
                if (std::max(std::abs(dx), std::abs(dy)) != radius)
                {
                    continue;
                }

                const BWAPI::WalkPosition walk(origin.x + dx, origin.y + dy);
                if (!walk.isValid() || !the.partitions.walkable(walk))
                {
                    continue;
                }

                const BWAPI::TilePosition tile(walk);
                const Zone * zone = ptr(tile);
                if (preferredZone && zone != preferredZone)
                {
                    continue;
                }

                return BWAPI::Position(walk) + BWAPI::Position(4, 4);
            }
        }
    }

    return BWAPI::Positions::Invalid;
}

BWAPI::Position GridZone::traceChokeEdge(
    const BWAPI::Position & center,
    double sideX,
    double sideY,
    int sign) const
{
    const BWAPI::WalkPosition centerWalk(center);
    BWAPI::WalkPosition last = centerWalk;
    int lastX = INT_MIN;
    int lastY = INT_MIN;

    for (int step = 1; step <= 48; ++step)
    {
        const int x = centerWalk.x + int(std::floor(sideX * sign * step + (sideX * sign >= 0.0 ? 0.5 : -0.5)));
        const int y = centerWalk.y + int(std::floor(sideY * sign * step + (sideY * sign >= 0.0 ? 0.5 : -0.5)));

        if (x == lastX && y == lastY)
        {
            continue;
        }
        lastX = x;
        lastY = y;

        const BWAPI::WalkPosition walk(x, y);
        if (!walk.isValid() || !the.partitions.walkable(walk))
        {
            break;
        }

        last = walk;
    }

    return BWAPI::Position(last) + BWAPI::Position(4, 4);
}

void GridZone::buildChokePoints()
{
    chokePoints.clear();
    areaGrid = std::vector< std::vector<short> >(
        width, std::vector<short>(height, short(0)));

    std::vector< std::vector<int> > tilePartition(
        width, std::vector<int>(height, 0));
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (the.tileRoom.at(x, y) > 0)
            {
                tilePartition[x][y] =
                    buildTileGroundPartition(BWAPI::TilePosition(x, y));
            }
        }
    }

    // The legacy GridZone split is useful for callers which expect an explicit
    // narrow zone, but it is not ideal for identifying the actual gateways.
    // Build a second, area-only layer: every spacious Normal zone is a seed and
    // a multi-source flood expands those areas through all usable terrain. Where
    // two area wavefronts meet, we have a real gateway between those areas.
    std::vector< std::vector<short> > distance(
        width, std::vector<short>(height, short(-1)));
    std::deque<BWAPI::TilePosition> open;

    // Every connected ground partition needs at least one spacious Area seed.
    // This is normally true automatically, but cramped/island/custom maps can
    // consist only of legacy "choke" zones. Promote the largest zone in such a
    // partition so areaAt() never silently returns nullptr on reachable ground.
    std::map<int, bool> partitionHasArea;
    std::map<int, Zone *> partitionFallback;
    for (Zone * zone : zones)
    {
        if (!zone || !zone->isValid() || zone->tiles().empty())
        {
            continue;
        }
        // Use an actual member tile. The arithmetic center of a concave Area
        // can lie over a cliff/void and therefore have partition id 0.
        const BWAPI::TilePosition member = zone->tiles().front();
        const int partition = tilePartition[member.x][member.y];
        if (partition <= 0)
        {
            continue;
        }
        if (zone->isArea())
        {
            partitionHasArea[partition] = true;
        }
        Zone * & fallback = partitionFallback[partition];
        if (!fallback || zone->tiles().size() > fallback->tiles().size())
        {
            fallback = zone;
        }
    }
    for (const auto & kv : partitionFallback)
    {
        if (!partitionHasArea[kv.first] && kv.second)
        {
            kv.second->_state = ZoneState::Normal;
        }
    }

    for (Zone * zone : zones)
    {
        if (!zone || !zone->isValid() || !zone->isArea())
        {
            continue;
        }

        for (const BWAPI::TilePosition & tile : zone->tiles())
        {
            if (!tile.isValid() || the.tileRoom.at(tile) <= 0)
            {
                continue;
            }
            areaGrid[tile.x][tile.y] = short(zone->id());
            distance[tile.x][tile.y] = 0;
            open.push_back(tile);
        }
    }

    struct Contact
    {
        BWAPI::TilePosition a;
        BWAPI::TilePosition b;
        BWAPI::Position midpoint;
        int room;

        Contact(const BWAPI::TilePosition & aa,
                const BWAPI::TilePosition & bb,
                int rr)
            : a(aa)
            , b(bb)
            , midpoint((TileCenter(aa) + TileCenter(bb)) / 2)
            , room(rr)
        {
        }
    };

    typedef std::pair<int, int> AreaPair;
    std::map<AreaPair, std::vector<Contact> > contacts;

    const int actionX[4] = { 1, -1, 0, 0 };
    const int actionY[4] = { 0, 0, 1, -1 };

    while (!open.empty())
    {
        const BWAPI::TilePosition here = open.front();
        open.pop_front();

        const int hereArea = areaGrid[here.x][here.y];
        const int hereDistance = distance[here.x][here.y];
        if (hereArea <= 0)
        {
            continue;
        }

        for (int a = 0; a < 4; ++a)
        {
            const BWAPI::TilePosition next(here.x + actionX[a], here.y + actionY[a]);
            if (!next.isValid() || the.tileRoom.at(next) <= 0)
            {
                continue;
            }

            // Build tiles are only graph-neighbors when ground units can
            // actually cross their COMMON 8x8 border. This rejects adjacent
            // high/low tiles separated by a cliff wall, which used to produce
            // phantom Area contacts and chokes under terrain.
            BWAPI::Position crossingPosition = BWAPI::Positions::Invalid;
            int crossingRoom = 0;
            if (!getTileEdgeConnection(here, next, crossingPosition, crossingRoom))
            {
                continue;
            }

            if (areaGrid[next.x][next.y] == 0)
            {
                areaGrid[next.x][next.y] = short(hereArea);
                distance[next.x][next.y] = short(std::min(32767, hereDistance + 1));
                open.push_back(next);
                continue;
            }

            const int otherArea = areaGrid[next.x][next.y];
            if (otherArea == hereArea || otherArea <= 0)
            {
                continue;
            }

            // Record each contact edge once. The area IDs are legacy Zone IDs,
            // so they remain unique during this topology revision.
            if (hereArea < otherArea)
            {
                const AreaPair key(hereArea, otherArea);
                Contact contact(here, next, crossingRoom);
                contact.midpoint = crossingPosition;
                contacts[key].push_back(contact);
            }
        }
    }

    // Merge area seeds that meet through a genuinely broad/open frontier.
    // Previously such contacts were merely ignored as "not a choke", leaving
    // two disconnected high-level Areas on terrain that is actually open.
    areaCanonical.resize(zones.size(), 0);
    std::vector<int> parent(zones.size(), 0);
    std::vector<int> weight(zones.size(), 0);
    for (Zone * zone : zones)
    {
        if (zone && zone->isValid() && zone->isArea())
        {
            parent[zone->id()] = zone->id();
            weight[zone->id()] = int(zone->tiles().size());
        }
    }

    auto findRoot = [&parent](int id) -> int
    {
        int root = id;
        while (root > 0 && parent[root] != root)
        {
            root = parent[root];
        }
        while (id > 0 && parent[id] != id)
        {
            const int next = parent[id];
            parent[id] = root;
            id = next;
        }
        return root;
    };

    auto unite = [&parent, &weight, &findRoot](int a, int b)
    {
        a = findRoot(a);
        b = findRoot(b);
        if (a <= 0 || b <= 0 || a == b)
        {
            return;
        }
        if (weight[a] < weight[b])
        {
            std::swap(a, b);
        }
        parent[b] = a;
        weight[a] += weight[b];
    };

    for (const auto & kv : contacts)
    {
        const std::vector<Contact> & pairContacts = kv.second;
        std::vector<bool> used(pairContacts.size(), false);
        for (size_t seed = 0; seed < pairContacts.size(); ++seed)
        {
            if (used[seed])
            {
                continue;
            }

            std::vector<size_t> cluster(1, seed);
            used[seed] = true;
            for (size_t i = 0; i < cluster.size(); ++i)
            {
                const BWAPI::Position p0 = pairContacts[cluster[i]].midpoint;
                for (size_t j = 0; j < pairContacts.size(); ++j)
                {
                    if (!used[j] && p0.getApproxDistance(pairContacts[j].midpoint) <= 48)
                    {
                        used[j] = true;
                        cluster.push_back(j);
                    }
                }
            }

            std::vector<int> rooms;
            rooms.reserve(cluster.size());
            for (size_t index : cluster)
            {
                rooms.push_back(pairContacts[index].room);
            }
            std::sort(rooms.begin(), rooms.end());
            const int representativeRoom = rooms.empty()
                ? 0
                : rooms[rooms.size() / 2];

            // A broad meeting is not a gateway at all: it is one Area. Use a
            // median frontier clearance so one wall-adjacent sample cannot keep
            // an otherwise open platform artificially split in two.
            if (representativeRoom > 16)
            {
                unite(kv.first.first, kv.first.second);
            }
        }
    }

    for (size_t id = 1; id < zones.size(); ++id)
    {
        if (parent[id] > 0)
        {
            areaCanonical[id] = short(findRoot(int(id)));
        }
    }

    // Canonicalize every high-level tile.
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            const int id = areaGrid[x][y];
            const int canonical = id > 0 && id < int(areaCanonical.size())
                ? areaCanonical[id]
                : 0;
            areaGrid[x][y] = short(canonical);
        }
    }

    // A high-clearance build tile can occasionally be connected only through a
    // diagonal/irregular 8x8 passage. The cardinal Area flood deliberately does
    // not invent a choke for such a contact, but leaving the tile with Area 0 is
    // worse: every topology rebuild reports it and gameplay callers can receive
    // nullptr for perfectly reachable ground. Attach these rare orphan tiles to
    // the nearest canonical Area in the SAME real ground partition. This does not
    // create a new choke and cannot bridge disconnected terrain.
    std::map<int, std::vector<BWAPI::TilePosition>> areaTilesByPartition;
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (areaGrid[x][y] > 0 && tilePartition[x][y] > 0)
            {
                areaTilesByPartition[tilePartition[x][y]].push_back(
                    BWAPI::TilePosition(x, y));
            }
        }
    }
    int repairedAreaTiles = 0;
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (the.tileRoom.at(x, y) < 2 || areaGrid[x][y] > 0)
            {
                continue;
            }

            const int partition = tilePartition[x][y];
            if (partition <= 0)
            {
                continue;
            }

            int bestArea = 0;
            long long bestDistanceSq = LLONG_MAX;

            const auto candidates = areaTilesByPartition.find(partition);
            if (candidates == areaTilesByPartition.end())
            {
                continue;
            }
            for (const BWAPI::TilePosition & candidate : candidates->second)
            {
                const long long dx = static_cast<long long>(candidate.x) - x;
                const long long dy = static_cast<long long>(candidate.y) - y;
                const long long distanceSq = dx * dx + dy * dy;
                if (distanceSq < bestDistanceSq)
                {
                    bestDistanceSq = distanceSq;
                    bestArea = areaGrid[candidate.x][candidate.y];
                }
            }

            if (bestArea > 0)
            {
                areaGrid[x][y] = short(bestArea);
                ++repairedAreaTiles;
            }
        }
    }

    if (Config::Debug::DrawMapInfo && repairedAreaTiles > 0)
    {
        BWAPI::Broodwar->drawTextScreen(
            5,
            105,
            "GridZone repaired orphan Area tiles: %d",
            repairedAreaTiles);
    }

    // Compute a center for the merged/repaired Area itself (not merely for
    // whichever legacy seed became representative).
    areaCenters.assign(zones.size(), BWAPI::Positions::Invalid);
    std::vector<long long> sumX(zones.size(), 0);
    std::vector<long long> sumY(zones.size(), 0);
    std::vector<int> centerCount(zones.size(), 0);
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            const int canonical = areaGrid[x][y];
            if (canonical > 0)
            {
                const BWAPI::Position center = TileCenter(BWAPI::TilePosition(x, y));
                sumX[canonical] += center.x;
                sumY[canonical] += center.y;
                ++centerCount[canonical];
            }
        }
    }
    for (size_t id = 1; id < areaCenters.size(); ++id)
    {
        if (centerCount[id] > 0)
        {
            areaCenters[id] = BWAPI::Position(
                int(sumX[id] / centerCount[id]),
                int(sumY[id] / centerCount[id]));
        }
    }

    // Snap arithmetic centers back onto an actual member tile. Concave Areas
    // can have a mathematical centroid over void/cliff terrain; topology code
    // and debug labels should always reference a real point inside the Area.
    std::vector<long long> bestCenterDistance(areaCenters.size(), LLONG_MAX);
    std::vector<BWAPI::Position> snappedCenters(
        areaCenters.size(), BWAPI::Positions::Invalid);
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            const int id = areaGrid[x][y];
            if (id <= 0 || id >= int(areaCenters.size()) || !areaCenters[id].isValid())
            {
                continue;
            }
            const BWAPI::Position p = TileCenter(BWAPI::TilePosition(x, y));
            const long long dx = static_cast<long long>(p.x) - areaCenters[id].x;
            const long long dy = static_cast<long long>(p.y) - areaCenters[id].y;
            const long long d2 = dx * dx + dy * dy;
            if (d2 < bestCenterDistance[id])
            {
                bestCenterDistance[id] = d2;
                snappedCenters[id] = p;
            }
        }
    }
    for (size_t id = 1; id < areaCenters.size(); ++id)
    {
        if (snappedCenters[id].isValid())
        {
            areaCenters[id] = snappedCenters[id];
        }
    }

    // Re-key all remaining frontier samples by canonical Area pair. Contacts
    // inside a merged open Area disappear; true narrow contacts are preserved.
    std::map<AreaPair, std::vector<Contact> > canonicalContacts;
    for (const auto & kv : contacts)
    {
        int a = canonicalAreaID(kv.first.first);
        int b = canonicalAreaID(kv.first.second);
        if (a <= 0 || b <= 0 || a == b)
        {
            continue;
        }
        if (a > b)
        {
            std::swap(a, b);
        }
        std::vector<Contact> & destination = canonicalContacts[AreaPair(a, b)];
        destination.insert(destination.end(), kv.second.begin(), kv.second.end());
    }

    int chokeID = 1;

    for (auto & kv : canonicalContacts)
    {
        Zone * areaA = ptr(kv.first.first);
        Zone * areaB = ptr(kv.first.second);
        if (!areaA || !areaB || !areaA->isArea() || !areaB->isArea())
        {
            continue;
        }

        std::vector<Contact> & pairContacts = kv.second;
        std::vector<bool> used(pairContacts.size(), false);

        // One pair of areas may have two physically separate gateways. Cluster
        // the meeting frontier spatially instead of collapsing the whole pair
        // to a single representative choke.
        for (size_t seed = 0; seed < pairContacts.size(); ++seed)
        {
            if (used[seed])
            {
                continue;
            }

            std::vector<size_t> cluster;
            cluster.push_back(seed);
            used[seed] = true;

            for (size_t i = 0; i < cluster.size(); ++i)
            {
                const BWAPI::Position p0 = pairContacts[cluster[i]].midpoint;
                for (size_t j = 0; j < pairContacts.size(); ++j)
                {
                    if (used[j])
                    {
                        continue;
                    }

                    // Adjacent border samples are at most one build tile apart.
                    // A 48px link keeps one continuous cross-section together
                    // without merging two nearby but physically separate ramps.
                    if (p0.getApproxDistance(pairContacts[j].midpoint) <= 48)
                    {
                        used[j] = true;
                        cluster.push_back(j);
                    }
                }
            }

            if (cluster.empty())
            {
                continue;
            }

            int minimumRoom = INT_MAX;
            std::vector<int> clusterRooms;
            clusterRooms.reserve(cluster.size());
            for (size_t index : cluster)
            {
                minimumRoom = std::min(minimumRoom, pairContacts[index].room);
                clusterRooms.push_back(pairContacts[index].room);
            }
            std::sort(clusterRooms.begin(), clusterRooms.end());
            const int representativeRoom = clusterRooms.empty()
                ? 0
                : clusterRooms[clusterRooms.size() / 2];

            // A meeting frontier between two area seeds is not automatically a
            // strategic choke. Classify the whole frontier by its median local
            // clearance; keep the minimum only for locating the actual throat.
            // This rejects open-space Voronoi boundaries with one narrow corner.
            if (minimumRoom == INT_MAX || representativeRoom > 16)
            {
                continue;
            }

            // Average the narrowest samples. This is much more stable than the
            // geometric centroid of a long frontier and naturally picks a ramp
            // throat when one side of the gateway fans out.
            long long sumX = 0;
            long long sumY = 0;
            int count = 0;
            for (size_t index : cluster)
            {
                if (pairContacts[index].room <= minimumRoom + 1)
                {
                    sumX += pairContacts[index].midpoint.x;
                    sumY += pairContacts[index].midpoint.y;
                    ++count;
                }
            }

            if (count == 0)
            {
                continue;
            }

            const BWAPI::Position averageCenter(int(sumX / count), int(sumY / count));

            // Keep the final center ON an observed walkable crossing. The old
            // findWalkableNear() could snap an abstract frontier centroid to a
            // different floor below a cliff, which is exactly how a bunker
            // ended up under the terrain on Benzene.
            BWAPI::Position center = BWAPI::Positions::Invalid;
            int bestCenterDistance = MAX_DISTANCE;
            for (size_t index : cluster)
            {
                if (pairContacts[index].room > minimumRoom + 1)
                {
                    continue;
                }

                const BWAPI::Position sample = pairContacts[index].midpoint;
                const int d = sample.getApproxDistance(averageCenter);
                if (d < bestCenterDistance)
                {
                    bestCenterDistance = d;
                    center = sample;
                }
            }

            if (!center.isValid() ||
                !BWAPI::WalkPosition(center).isValid() ||
                !the.partitions.walkable(BWAPI::WalkPosition(center)))
            {
                continue;
            }

            Zone * legacyZone = ptr(BWAPI::TilePosition(center));
            if (legacyZone && !legacyZone->isChoke())
            {
                legacyZone = nullptr;      // wide ramp/gateway, not a legacy narrow zone
            }

            const BWAPI::Position areaACenter = areaCenter(areaA);
            const BWAPI::Position areaBCenter = areaCenter(areaB);
            if (!areaACenter.isValid() || !areaBCenter.isValid())
            {
                UAB_MESSAGE(
                    "GRID ERROR: invalid Area center while building choke %d/%d near (%d,%d)",
                    areaA ? areaA->id() : -1,
                    areaB ? areaB->id() : -1,
                    center.x, center.y);
                continue;
            }
            const double dx = double(areaBCenter.x - areaACenter.x);
            const double dy = double(areaBCenter.y - areaACenter.y);
            const double length = std::sqrt(dx * dx + dy * dy);

            BWAPI::Position endA = center;
            BWAPI::Position endB = center;
            int widthWalkTiles = std::max(1, minimumRoom);

            // The contact frontier itself is the best evidence for the choke
            // cross-section. Use its farthest two observed crossing samples
            // when possible. This is more reliable than assuming the line is
            // perpendicular to AreaCenter(A)->AreaCenter(B), especially on
            // bent ramps and asymmetric cliff shapes.
            int bestSpan = 0;
            for (size_t ii = 0; ii < cluster.size(); ++ii)
            {
                const BWAPI::Position p0 = pairContacts[cluster[ii]].midpoint;
                for (size_t jj = ii + 1; jj < cluster.size(); ++jj)
                {
                    const BWAPI::Position p1 = pairContacts[cluster[jj]].midpoint;
                    const int span = p0.getApproxDistance(p1);
                    if (span > bestSpan)
                    {
                        bestSpan = span;
                        endA = p0;
                        endB = p1;
                    }
                }
            }

            if (bestSpan >= 8)
            {
                const int observedWidth = std::max(1, bestSpan / 8 + 1);
                widthWalkTiles = std::min(widthWalkTiles, observedWidth);
            }
            else if (length > 1.0)
            {
                // Very narrow/single-sample passage: fall back to a local edge
                // trace, but keep the center fixed on the real crossing sample.
                const double sideX = -dy / length;
                const double sideY = dx / length;
                endA = traceChokeEdge(center, sideX, sideY, +1);
                endB = traceChokeEdge(center, sideX, sideY, -1);

                if (endA.isValid() && endB.isValid())
                {
                    const int tracedWidth = std::max(1, endA.getApproxDistance(endB) / 8 + 1);
                    widthWalkTiles = std::min(widthWalkTiles, tracedWidth);
                }
            }
            else if (bestSpan < 8)
            {
                UAB_MESSAGE(
                    "GRID ERROR: degenerate choke axis areas=%d/%d center=(%d,%d) span=%d",
                    areaA->id(), areaB->id(), center.x, center.y, bestSpan);
                continue;
            }

            ChokePoint choke;
            choke._id = chokeID++;
            choke._zone = legacyZone;
            choke._areaA = areaA;
            choke._areaB = areaB;
            choke._center = center;
            choke._endA = endA;
            choke._endB = endB;
            choke._widthWalkTiles = std::max(1, widthWalkTiles);
            chokePoints.push_back(choke);
        }
    }

    // Every usable high-level tile should now belong to a canonical Area.
    // Warn instead of hard-crashing on unusual custom maps; low-level ground
    // distance remains available as a fallback even if map analysis is odd.
    int unassignedAreaTiles = 0;
    int unassignedFringeTiles = 0;
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            const int room = the.tileRoom.at(x, y);
            if (room >= 2 && areaGrid[x][y] <= 0)
            {
                ++unassignedAreaTiles;
            }
            else if (room > 0 && areaGrid[x][y] <= 0)
            {
                // Clearance-1 build tiles are deliberately below the legacy
                // GridZone minRoom threshold. They are terrain fringe/noise,
                // not missing high-level Areas, and should not trigger the
                // scary topology warning seen on maps such as Python.
                ++unassignedFringeTiles;
            }
        }
    }
    UAB_ASSERT_WARNING(
        unassignedAreaTiles == 0,
        "GRID ERROR: GridZone topology left %d high-level tiles without an Area",
        unassignedAreaTiles);

    if (Config::Debug::DrawMapInfo && unassignedFringeTiles > 0)
    {
        BWAPI::Broodwar->drawTextScreen(
            5,
            115,
            "GridZone fringe (clearance 1, intentionally unassigned): %d",
            unassignedFringeTiles);
    }

    // Validate the explicit graph separately from the legacy zone sanity check.
    // Keep the diagnostics non-destructive: UAB_ASSERT only reports in this
    // build, so dereferencing a bad pointer on the following line would turn a
    // useful Grid warning into a crash. Every malformed choke now prints a
    // concrete "GRID ERROR" with its ID/coordinates and is then skipped safely.
    for (size_t i = 0; i < chokePoints.size(); ++i)
    {
        const ChokePoint & choke = chokePoints[i];
        if (choke.id() != int(i) + 1)
        {
            UAB_MESSAGE(
                "GRID ERROR: choke index %d has id %d",
                int(i), choke.id());
        }

        if (!choke.areaA() || !choke.areaB() || choke.areaA() == choke.areaB())
        {
            UAB_MESSAGE(
                "GRID ERROR: choke C%d has invalid area links at (%d,%d)",
                choke.id(), choke.center().x, choke.center().y);
            continue;
        }

        if (!choke.areaA()->isArea() || !choke.areaB()->isArea())
        {
            UAB_MESSAGE(
                "GRID ERROR: choke C%d connects non-area zones %d/%d at (%d,%d)",
                choke.id(), choke.areaA()->id(), choke.areaB()->id(),
                choke.center().x, choke.center().y);
            continue;
        }

        const BWAPI::WalkPosition centerWalk(choke.center());
        if (!choke.center().isValid() || choke.widthWalkTiles() <= 0 ||
            !centerWalk.isValid() || !the.partitions.walkable(centerWalk))
        {
            UAB_MESSAGE(
                "GRID ERROR: choke C%d bad center/width pos=(%d,%d) width=%d",
                choke.id(), choke.center().x, choke.center().y,
                choke.widthWalkTiles());
            continue;
        }

        if (!choke.endA().isValid() || !choke.endB().isValid())
        {
            UAB_MESSAGE(
                "GRID ERROR: choke C%d bad ends A=(%d,%d) B=(%d,%d)",
                choke.id(), choke.endA().x, choke.endA().y,
                choke.endB().x, choke.endB().y);
        }
    }
}

const ChokePoint * GridZone::chokeForZone(const Zone * zone) const
{
    if (!zone)
    {
        return nullptr;
    }

    for (const ChokePoint & choke : chokePoints)
    {
        if (choke.zone() == zone)
        {
            return &choke;
        }
    }

    return nullptr;
}

int GridZone::canonicalAreaID(int id) const
{
    if (id <= 0 || id >= int(zones.size()))
    {
        return 0;
    }
    if (!areaCanonical.empty() && id < int(areaCanonical.size()) && areaCanonical[id] > 0)
    {
        id = areaCanonical[id];
    }
    return id;
}

const Zone * GridZone::areaFromID(int id) const
{
    id = canonicalAreaID(id);
    if (id <= 0 || id >= int(zones.size()))
    {
        return nullptr;
    }

    const Zone * zone = zones[id];
    return zone && zone->isValid() && zone->isArea() ? zone : nullptr;
}

BWAPI::Position GridZone::areaCenter(const Zone * area) const
{
    if (!area)
    {
        return BWAPI::Positions::Invalid;
    }
    const int id = canonicalAreaID(area->id());
    if (id > 0 && id < int(areaCenters.size()) && areaCenters[id].isValid())
    {
        return areaCenters[id];
    }
    return area->center();
}

void GridZone::sanityCheck()
{
    std::map<int, size_t> tileCount;
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            Zone * zone = ptr(x, y);
            if (zone)
            {
                UAB_ASSERT(zone->id() > 0 && zone->id() < int(zones.size()), "bad zone id");
                UAB_ASSERT(zone->id() == grid[x][y], "zone id mismatch");
                tileCount[zone->id()] += 1;
            }
            else
            {
                UAB_ASSERT(grid[x][y] == 0, "non-zero non-zone");
                zone = zones[0];
                UAB_ASSERT(zone->id() == 0 && !zone->isValid(), "non-zone is valid zone");
            }
        }
    }

    int totalTiles = 0;
    for (Zone * zone : zones)
    {
        if (zone->isValid())
        {
            UAB_ASSERT(zone->_state == ZoneState::Choke || zone->_state == ZoneState::Normal, "bad zone state");

            totalTiles += int(zone->tiles().size());
            UAB_ASSERT(zone->tiles().size() > 0, "valid zone has no tiles");
            UAB_ASSERT(zone->tiles().size() == tileCount[zone->id()], "wrong tile count");

            for (const BWAPI::TilePosition & tile : zone->tiles())
            {
                UAB_ASSERT(tile.isValid() && the.tileRoom.at(tile) > 0, "bad tile in zone");
            }

            for (Zone * neighbor : zone->neighbors())
            {
                UAB_ASSERT(neighbor, "zone %d is neighbor of nullptr", zone->id());
                if (neighbor)
                {
                    UAB_ASSERT(neighbor->isValid(), "invalid neighbor %d of %d", neighbor->id(), zone->id());
                    UAB_ASSERT(zone != neighbor, "zone %d is neighbor of itself", zone->id());
                    UAB_ASSERT(ptr(neighbor->id()) == neighbor, "neighbor of %d is not a zone", zone->id());
                    UAB_ASSERT(neighbor->neighbors().find(zone) != neighbor->neighbors().end(), "non-mutual neighbors %d %d", zone->id(), neighbor->id());
                }
            }
        }
        else
        {
            UAB_ASSERT(zone->_state == ZoneState::Invalid, "bad zone state");
            UAB_ASSERT(zone->groundHeight() == -1, "invalid zone has ground height");
            UAB_ASSERT(zone->tiles().empty(), "invalid zone has tiles");
            UAB_ASSERT(zone->neighbors().empty(), "invalid zone has neighbors");
        }
    }
    UAB_ASSERT(totalTiles <= height * width, "too many tiles in zones %d", totalTiles);
}

GridZone::GridZone()
    : Grid()
{
}

GridZone::~GridZone()
{
    clearZones();
}

void GridZone::initialize()
{
    const size_t LegalActions = 4;
    const int actionX[LegalActions] = { 1, -1, 0, 0 };
    const int actionY[LegalActions] = { 0, 0, 1, -1 };

    const int minRoom = 2;
    const int chokeWidth = 12;
    const int minArea = 3;

    clearZones();

    width = BWAPI::Broodwar->mapWidth();
    height = BWAPI::Broodwar->mapHeight();
    grid = std::vector< std::vector<short> >(width, std::vector<short>(height, short(0)));

    zones.push_back(new Zone(0));

    int zoneID = 1;
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            const BWAPI::TilePosition xy(x, y);
            if (grid[x][y] == 0 && the.tileRoom.at(xy) >= minRoom)
            {
                grid[x][y] = zoneID;
                zones.push_back(new Zone(zoneID));
                Zone * zone(zones.back());
                zone->_state = the.tileRoom.at(xy) <= chokeWidth ? ZoneState::Choke : ZoneState::Normal;
                zone->_groundHeight = GroundHeight(x, y);
                zone->_tiles.push_back(xy);

                std::vector<BWAPI::TilePosition> fringe;
                fringe.reserve(width * height);
                fringe.push_back(xy);

                for (size_t fringeIndex = 0; fringeIndex < fringe.size(); ++fringeIndex)
                {
                    const BWAPI::TilePosition & tile = fringe[fringeIndex];
                    for (size_t a = 0; a < LegalActions; ++a)
                    {
                        const BWAPI::TilePosition nextTile(tile.x + actionX[a], tile.y + actionY[a]);

                        if (!nextTile.isValid())
                        {
                            continue;
                        }

                        BWAPI::Position crossing;
                        int crossingRoom = 0;
                        if (!getTileEdgeConnection(tile, nextTile, crossing, crossingRoom))
                        {
                            continue;
                        }

                        const int id = grid[nextTile.x][nextTile.y];
                        if (id != 0)
                        {
                            if (id != zoneID)
                            {
                                Zone * neighbor = ptr(id);
                                zone->_neighbors.insert(neighbor);
                                neighbor->_neighbors.insert(zone);
                            }
                        }
                        else if (the.tileRoom.at(nextTile) >= minRoom &&
                            (zone->isChoke()
                                ? the.tileRoom.at(nextTile) <= chokeWidth
                                : the.tileRoom.at(nextTile) > chokeWidth && GroundHeight(nextTile) == zone->_groundHeight))
                        {
                            zone->_tiles.push_back(nextTile);
                            fringe.push_back(nextTile);
                            grid[nextTile.x][nextTile.y] = zoneID;
                        }
                    }
                }

                UAB_ASSERT(zoneID + 1 == int(zones.size()) && zones[zoneID]->id() == zoneID, "bad zone id");
                ++zoneID;
            }
        }
    }

    // Remove tiny/noise zones while preserving actual narrow connectors.
    bool changed;
    do
    {
        changed = false;
        for (Zone * zone : zones)
        {
            if (!zone->isValid())
            {
                continue;
            }

            if (zone->neighbors().size() == 1)
            {
                Zone * neighbor = *zone->_neighbors.begin();
                if (zone->tiles().size() <= neighbor->tiles().size())
                {
                    newZoneID(zone, neighbor->id());
                    zone->mergeInto(neighbor);
                    changed = true;
                }
            }
            else if (zone->tiles().size() < size_t(minArea))
            {
                if (zone->neighbors().empty())
                {
                    // This is still usable ground (room >= minRoom). A tiny
                    // isolated component needs its own Area; deleting its only
                    // seed leaves an orphan that no neighboring Area can repair.
                    zone->_state = ZoneState::Normal;
                }
                else
                {
                    Zone * parent = nullptr;
                    size_t biggest = 0;
                    if (zone->isChoke())
                    {
                        for (Zone * neighbor : zone->neighbors())
                        {
                            if (neighbor->isChoke() && neighbor->tiles().size() > biggest)
                            {
                                parent = neighbor;
                                biggest = neighbor->tiles().size();
                            }
                        }
                    }
                    if (!parent)
                    {
                        for (Zone * neighbor : zone->neighbors())
                        {
                            if (neighbor->tiles().size() > biggest)
                            {
                                parent = neighbor;
                                biggest = neighbor->tiles().size();
                            }
                        }
                    }
                    UAB_ASSERT(parent, "no parent found");
                    newZoneID(zone, parent->id());
                    zone->mergeInto(parent);
                    changed = true;
                }
            }
        }
    } while (changed);

    computeZoneCenters();
    sanityCheck();
    buildChokePoints();

    // Retained for compatibility/debugging. Exact first-choke selection now
    // uses the real from/to positions instead of one arbitrary result cached
    // for an entire Area pair.
    firstChokeCache.assign(
        zones.size(),
        std::vector<short>(zones.size(), short(-2)));
}

Zone * GridZone::ptr(int id)
{
    if (id == 0)
    {
        return nullptr;
    }

    if (id <= 0 || id >= int(zones.size()) || !zones[id] || zones[id]->id() != id)
    {
        static int lastBadZoneFrame = -100000;
        const int frame = BWAPI::BroodwarPtr ? BWAPI::Broodwar->getFrameCount() : -1;
        if (frame < lastBadZoneFrame || frame - lastBadZoneFrame >= 24)
        {
            lastBadZoneFrame = frame;
            UAB_MESSAGE(
                "GRID ERROR: GridZone::ptr bad zone id=%d zones=%d",
                id, int(zones.size()));
        }
        return nullptr;
    }
    return zones[id];
}

Zone * GridZone::ptr(int x, int y)
{
    return ptr(at(x, y));
}

Zone * GridZone::ptr(const BWAPI::TilePosition & tile)
{
    return ptr(tile.x, tile.y);
}

const Zone * GridZone::ptr(int id) const
{
    if (id == 0)
    {
        return nullptr;
    }

    if (id <= 0 || id >= int(zones.size()) || !zones[id] || zones[id]->id() != id)
    {
        static int lastBadZoneFrame = -100000;
        const int frame = BWAPI::BroodwarPtr ? BWAPI::Broodwar->getFrameCount() : -1;
        if (frame < lastBadZoneFrame || frame - lastBadZoneFrame >= 24)
        {
            lastBadZoneFrame = frame;
            UAB_MESSAGE(
                "GRID ERROR: GridZone::ptr const bad zone id=%d zones=%d",
                id, int(zones.size()));
        }
        return nullptr;
    }
    return zones[id];
}

const Zone * GridZone::ptr(int x, int y) const
{
    return ptr(at(x, y));
}

const Zone * GridZone::ptr(const BWAPI::TilePosition & tile) const
{
    return ptr(tile.x, tile.y);
}

const Zone * GridZone::areaAt(const BWAPI::TilePosition & tile) const
{
    if (!tile.isValid() || areaGrid.empty())
    {
        return nullptr;
    }

    // areaGrid should match the build-tile dimensions exactly. Guard the read
    // so corrupted/partial topology becomes a useful diagnostic rather than an
    // out-of-range access later in tank or building logic.
    if (tile.x < 0 || tile.y < 0 ||
        size_t(tile.x) >= areaGrid.size() ||
        size_t(tile.y) >= areaGrid[tile.x].size())
    {
        static int lastAreaBoundsErrorFrame = -100000;
        const int frame = BWAPI::BroodwarPtr ? BWAPI::Broodwar->getFrameCount() : -1;
        if (frame < lastAreaBoundsErrorFrame || frame - lastAreaBoundsErrorFrame >= 24)
        {
            lastAreaBoundsErrorFrame = frame;
            UAB_MESSAGE(
                "GRID ERROR: areaAt(%d,%d) outside areaGrid=%dx%d",
                tile.x, tile.y, int(areaGrid.size()),
                areaGrid.empty() ? 0 : int(areaGrid.front().size()));
        }
        return nullptr;
    }

    const int id = areaGrid[tile.x][tile.y];
    const Zone * area = areaFromID(id);

    // Clearance >= 2 is classified as usable high-level terrain during GridZone
    // construction. Such a tile must have an Area. Clearance-1 fringe is
    // intentionally unassigned and therefore does NOT generate this warning.
    if (!area && the.tileRoom.at(tile) >= 2)
    {
        static int lastMissingAreaFrame = -100000;
        const int frame = BWAPI::BroodwarPtr ? BWAPI::Broodwar->getFrameCount() : -1;
        if (frame < lastMissingAreaFrame || frame - lastMissingAreaFrame >= 24)
        {
            lastMissingAreaFrame = frame;
            UAB_MESSAGE(
                "GRID ERROR: areaAt(%d,%d) room=%d areaID=%d has no Area",
                tile.x, tile.y, the.tileRoom.at(tile), id);
        }
    }

    return area;
}

const Zone * GridZone::areaAt(const BWAPI::WalkPosition & walk) const
{
    return walk.isValid() ? areaAt(BWAPI::TilePosition(walk)) : nullptr;
}

const Zone * GridZone::areaAt(const BWAPI::Position & pos) const
{
    return pos.isValid() ? areaAt(BWAPI::TilePosition(pos)) : nullptr;
}

const Zone * GridZone::areaAt(BWAPI::Unit unit) const
{
    return unit && unit->getPosition().isValid() ? areaAt(unit->getTilePosition()) : nullptr;
}

int GridZone::areaIDAt(const BWAPI::TilePosition & tile) const
{
    const Zone * area = areaAt(tile);
    return area ? area->id() : 0;
}

int GridZone::areaIDAt(const BWAPI::WalkPosition & walk) const
{
    const Zone * area = areaAt(walk);
    return area ? area->id() : 0;
}

int GridZone::areaIDAt(const BWAPI::Position & pos) const
{
    const Zone * area = areaAt(pos);
    return area ? area->id() : 0;
}

int GridZone::areaIDAt(BWAPI::Unit unit) const
{
    const Zone * area = areaAt(unit);
    return area ? area->id() : 0;
}

bool GridZone::sameArea(const BWAPI::TilePosition & a, const BWAPI::TilePosition & b) const
{
    const int areaA = areaIDAt(a);
    return areaA > 0 && areaA == areaIDAt(b);
}

bool GridZone::sameArea(const BWAPI::Position & a, const BWAPI::Position & b) const
{
    const int areaA = areaIDAt(a);
    return areaA > 0 && areaA == areaIDAt(b);
}

bool GridZone::sameArea(BWAPI::Unit a, const BWAPI::Position & b) const
{
    const int areaA = areaIDAt(a);
    return areaA > 0 && areaA == areaIDAt(b);
}

std::vector<const ChokePoint *> GridZone::findChokePath(
    const BWAPI::Position & from,
    const BWAPI::Position & to) const
{
    std::vector<const ChokePoint *> result;
    if (!from.isValid() || !to.isValid())
    {
        return result;
    }

    const Zone * start = areaAt(from);
    const Zone * goal = areaAt(to);
    if (!start || !goal || start == goal || chokePoints.empty())
    {
        return result;
    }

    // Prefer real terrain travel cost over pure geometry. Two choke centers
    // can be close in pixels but separated by a cliff or a long ground detour.
    auto routeCost = [](const BWAPI::Position & a, const BWAPI::Position & b) -> int
    {
        const int ground = the.map.getGroundDistance(a, b);
        return ground >= 0 ? ground : a.getApproxDistance(b);
    };

    // Fast/common case: the two Areas touch directly. Select the crossing from
    // the caller's actual positions. This is O(chokes) and avoids running the
    // graph search for every Goliath/Tank crossing one ordinary ramp.
    const ChokePoint* direct = nullptr;
    int directScore = MAX_DISTANCE;
    for (const ChokePoint & choke : chokePoints)
    {
        if (!choke.connects(start, goal) || !choke.center().isValid())
        {
            continue;
        }

        const int score = routeCost(from, choke.center()) +
            routeCost(choke.center(), to) +
            std::max(0, 8 - choke.widthWalkTiles()) * 6;
        if (score < directScore)
        {
            directScore = score;
            direct = &choke;
        }
    }
    if (direct)
    {
        result.push_back(direct);
        return result;
    }

    // Dijkstra over ORIENTED choke states. A state means "we just crossed
    // choke C and are now inside Area X". This is the missing piece in the old
    // Area-center route: when one Area has two exits, the correct next exit
    // depends on where the army actually is and where it is going.
    const int stateCount = int(chokePoints.size()) * 2;
    const int INF = INT_MAX / 4;
    std::vector<int> distance(stateCount, INF);
    std::vector<int> previous(stateCount, -1);
    std::vector<bool> used(stateCount, false);

    auto stateArea = [this](int state) -> const Zone *
    {
        const int chokeIndex = state / 2;
        const bool intoB = (state & 1) != 0;
        if (chokeIndex < 0 || chokeIndex >= int(chokePoints.size()))
        {
            return nullptr;
        }
        return intoB ? chokePoints[chokeIndex].areaB() : chokePoints[chokeIndex].areaA();
    };

    auto stateForArrival = [this](int chokeIndex, const Zone * area) -> int
    {
        if (chokeIndex < 0 || chokeIndex >= int(chokePoints.size()) || !area)
        {
            return -1;
        }
        if (chokePoints[chokeIndex].areaA() == area)
        {
            return chokeIndex * 2;
        }
        if (chokePoints[chokeIndex].areaB() == area)
        {
            return chokeIndex * 2 + 1;
        }
        return -1;
    };

    // Enter the graph through any real choke attached to the start Area. The
    // cost uses the caller's true position, not the center of a huge Area.
    for (size_t i = 0; i < chokePoints.size(); ++i)
    {
        const ChokePoint & choke = chokePoints[i];
        if (!choke.center().isValid())
        {
            continue;
        }

        const Zone * arrival = nullptr;
        if (choke.areaA() == start)
        {
            arrival = choke.areaB();
        }
        else if (choke.areaB() == start)
        {
            arrival = choke.areaA();
        }

        if (!arrival)
        {
            continue;
        }

        const int state = stateForArrival(int(i), arrival);
        if (state < 0)
        {
            continue;
        }

        const int widthPenalty = std::max(0, 8 - choke.widthWalkTiles()) * 6;
        distance[state] = routeCost(from, choke.center()) + widthPenalty;
    }

    int bestGoalState = -1;
    int bestGoalCost = INF;

    for (int iteration = 0; iteration < stateCount; ++iteration)
    {
        int currentState = -1;
        int currentDistance = INF;
        for (int s = 0; s < stateCount; ++s)
        {
            if (!used[s] && distance[s] < currentDistance)
            {
                currentState = s;
                currentDistance = distance[s];
            }
        }

        if (currentState < 0 || currentDistance >= bestGoalCost)
        {
            break;
        }

        used[currentState] = true;
        const int currentChokeIndex = currentState / 2;
        const ChokePoint & currentChoke = chokePoints[currentChokeIndex];
        const Zone * currentArea = stateArea(currentState);
        if (!currentArea)
        {
            continue;
        }

        if (currentArea == goal)
        {
            const int total = currentDistance + routeCost(currentChoke.center(), to);
            if (total < bestGoalCost)
            {
                bestGoalCost = total;
                bestGoalState = currentState;
            }
            continue;
        }

        for (size_t i = 0; i < chokePoints.size(); ++i)
        {
            if (int(i) == currentChokeIndex)
            {
                continue;
            }

            const ChokePoint & nextChoke = chokePoints[i];
            const Zone * arrival = nullptr;
            if (nextChoke.areaA() == currentArea)
            {
                arrival = nextChoke.areaB();
            }
            else if (nextChoke.areaB() == currentArea)
            {
                arrival = nextChoke.areaA();
            }

            if (!arrival || !nextChoke.center().isValid())
            {
                continue;
            }

            const int nextState = stateForArrival(int(i), arrival);
            if (nextState < 0 || used[nextState])
            {
                continue;
            }

            int edgeCost = routeCost(currentChoke.center(), nextChoke.center());
            edgeCost = std::max(16, edgeCost);
            edgeCost += std::max(0, 8 - nextChoke.widthWalkTiles()) * 6;

            // Small directness tie-breaker. It is intentionally tiny compared
            // with actual travel distance; it only prevents two near-equal
            // routes from forming an unnecessary triangle around the target.
            edgeCost += int(lineSideDistance(nextChoke.center(), from, to) * 0.08);

            if (distance[currentState] <= INF - edgeCost &&
                distance[currentState] + edgeCost < distance[nextState])
            {
                distance[nextState] = distance[currentState] + edgeCost;
                previous[nextState] = currentState;
            }
        }
    }

    if (bestGoalState < 0)
    {
        return result;
    }

    std::vector<int> reverseStates;
    for (int state = bestGoalState; state >= 0; state = previous[state])
    {
        reverseStates.push_back(state);
    }

    for (std::vector<int>::reverse_iterator it = reverseStates.rbegin();
        it != reverseStates.rend(); ++it)
    {
        const int chokeIndex = *it / 2;
        if (result.empty() || result.back() != &chokePoints[chokeIndex])
        {
            result.push_back(&chokePoints[chokeIndex]);
        }
    }

    return result;
}

std::vector<const Zone *> GridZone::findZonePath(
    const BWAPI::Position & from,
    const BWAPI::Position & to) const
{
    std::vector<const Zone *> result;
    const Zone * start = areaAt(from);
    const Zone * goal = areaAt(to);
    if (!start || !goal)
    {
        return result;
    }

    result.push_back(start);
    if (start == goal)
    {
        return result;
    }

    const std::vector<const ChokePoint *> path = findChokePath(from, to);
    const Zone * current = start;
    for (const ChokePoint * choke : path)
    {
        if (!choke)
        {
            continue;
        }

        const Zone * next = choke->areaA() == current ? choke->areaB() : choke->areaA();
        if (!next)
        {
            result.clear();
            return result;
        }
        result.push_back(next);
        current = next;
    }

    if (result.empty() || result.back() != goal)
    {
        result.clear();
    }
    return result;
}

const ChokePoint * GridZone::findFirstChoke(
    const BWAPI::Position & from,
    const BWAPI::Position & to) const
{
    const std::vector<const ChokePoint *> path = findChokePath(from, to);
    return path.empty() ? nullptr : path.front();
}

const ChokePoint * GridZone::findNearestChoke(
    const BWAPI::Position & from,
    const BWAPI::Position & toward) const
{
    if (!from.isValid())
    {
        return nullptr;
    }

    const Zone * fromArea = areaAt(from);
    const ChokePoint * best = nullptr;
    int bestScore = MAX_DISTANCE;

    double dirX = 0.0;
    double dirY = 0.0;
    double dirLength = 0.0;
    if (toward.isValid())
    {
        dirX = double(toward.x - from.x);
        dirY = double(toward.y - from.y);
        dirLength = std::sqrt(dirX * dirX + dirY * dirY);
        if (dirLength > 1.0)
        {
            dirX /= dirLength;
            dirY /= dirLength;
        }
    }

    for (const ChokePoint & choke : chokePoints)
    {
        if (!choke.center().isValid())
        {
            continue;
        }

        if (fromArea)
        {
            if (choke.areaA() != fromArea && choke.areaB() != fromArea)
            {
                continue;
            }
        }
        else if (the.partitions.id(from) != the.partitions.id(choke.center()))
        {
            continue;
        }

        const int d = from.getApproxDistance(choke.center());
        if (d > 28 * 32)
        {
            continue;
        }

        int score = d;
        if (dirLength > 1.0)
        {
            const double dx = double(choke.center().x - from.x);
            const double dy = double(choke.center().y - from.y);
            const double forward = dx * dirX + dy * dirY;
            if (forward < 0.0)
            {
                score += 12 * 32;
            }
            else
            {
                const double side = std::abs(-dx * dirY + dy * dirX);
                score += int(side / 3.0);
            }
        }

        // For otherwise similar choices, a more distinct/narrow gateway is a
        // better defense reference than an arbitrary broad area boundary.
        score += std::min(10, choke.widthWalkTiles()) * 4;

        if (score < bestScore)
        {
            bestScore = score;
            best = &choke;
        }
    }

    return best;
}

void GridZone::draw()
{
    // Do not spam one legacy zone id on every build tile. The canonical Area
    // graph is what gameplay now consumes, so debug output should show that
    // graph directly and remain readable in a replay screenshot.

    // Draw canonical high-level Area centers separately from the legacy zone
    // labels above. This makes it immediately visible in screenshots whether
    // an open platform was accidentally split or a real choke was missed.
    for (size_t id = 1; id < areaCenters.size(); ++id)
    {
        if (canonicalAreaID(int(id)) != int(id) || !areaCenters[id].isValid())
        {
            continue;
        }
        BWAPI::Broodwar->drawCircleMap(areaCenters[id], 16, BWAPI::Colors::Cyan);
        BWAPI::Broodwar->drawTextMap(
            areaCenters[id] + BWAPI::Position(8, 8),
            "%cA%d", cyan, int(id));
    }

    for (const ChokePoint & choke : chokePoints)
    {
        if (!choke.center().isValid())
        {
            continue;
        }

        BWAPI::Broodwar->drawCircleMap(choke.center(), 10, BWAPI::Colors::Red);
        if (choke.endA().isValid() && choke.endB().isValid())
        {
            BWAPI::Broodwar->drawLineMap(choke.endA(), choke.endB(), BWAPI::Colors::Orange);
        }
        BWAPI::Broodwar->drawTextMap(
            choke.center() + BWAPI::Position(8, -8),
            "%cC%d w%d",
            red,
            choke.id(),
            choke.widthWalkTiles());
    }
}
