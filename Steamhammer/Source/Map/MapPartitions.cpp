#include "Map/MapPartitions.h"

#include "Utilities/UABAssert.h"

#include <algorithm>

using namespace UAlbertaBot;

namespace
{
    void reportPartitionGridError(const char* caller, int x, int y, int width, int height)
    {
        static int lastReportFrame = -100000;
        const int frame = BWAPI::BroodwarPtr ? BWAPI::Broodwar->getFrameCount() : -1;
        if (frame < lastReportFrame || frame - lastReportFrame >= 24)
        {
            lastReportFrame = frame;
            UAB_MESSAGE(
                "GRID ERROR: MapPartitions::%s bad walk tile (%d,%d), size=%dx%d",
                caller, x, y, width, height);
        }
    }
}

// Calculate the unwalkability map, which for each walk tile stores the number of obstacles
// which make the tile unwalkable. Unwalkable terrain counts as one obstacle. Immobile neutral
// units (whether destructible or not) count as one obstacle each.
// A tile with value 0 is walkable.
// Keeping a count (rather than a bool) lets us remove one overlapping neutral blocker safely.
void MapPartitions::findUnwalkability()
{
    unwalkability = std::vector< std::vector<unsigned short> >(width, std::vector<unsigned short>(height, 0));

    // First count terrain.
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (!BWAPI::Broodwar->isWalkable(x, y))
            {
                unwalkability[x][y] = 1;
            }
        }
    }

    // Then count neutral units. Remember the exact walk-tile footprint used at
    // startup, so an onUnitDestroy event can remove exactly the same cells.
    neutralFootprints.clear();
    for (BWAPI::Unit unit : BWAPI::Broodwar->getStaticNeutralUnits())
    {
        if (!unit || unit->getType().canMove() || unit->isFlying() || unit->getType().isSpell())
        {
            continue;
        }

        std::vector<BWAPI::WalkPosition> & footprint = neutralFootprints[unit->getID()];
        for (int x = unit->getLeft() / 8; x <= unit->getRight() / 8; ++x)
        {
            for (int y = unit->getTop() / 8; y <= unit->getBottom() / 8; ++y)
            {
                const BWAPI::WalkPosition walk(x, y);
                if (walk.isValid())
                {
                    footprint.push_back(walk);
                    ++unwalkability[x][y];
                }
            }
        }
    }
}

// Mark a partition: Fill a connected region with the value of numPartitions.
void MapPartitions::markOnePartition(const BWAPI::WalkPosition & start)
{
    const size_t LegalActions = 4;
    const int actionX[LegalActions] = { 1, -1, 0, 0 };
    const int actionY[LegalActions] = { 0, 0, 1, -1 };

    std::vector<BWAPI::WalkPosition> fringe;
    fringe.reserve(width * height);
    fringe.push_back(start);

    partition[start.x][start.y] = numPartitions;

    for (size_t fringeIndex = 0; fringeIndex < fringe.size(); ++fringeIndex)
    {
        const BWAPI::WalkPosition & tile = fringe[fringeIndex];

        for (size_t a = 0; a < LegalActions; ++a)
        {
            BWAPI::WalkPosition nextTile(tile.x + actionX[a], tile.y + actionY[a]);

            if (nextTile.isValid() &&
                partition[nextTile.x][nextTile.y] == 0 &&
                walkable(nextTile))
            {
                fringe.push_back(nextTile);
                partition[nextTile.x][nextTile.y] = numPartitions;
            }
        }
    }
}

void MapPartitions::rebuildPartitions()
{
    numPartitions = 0;
    partition = std::vector< std::vector<unsigned short> >(width, std::vector<unsigned short>(height, 0));

    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (partition[x][y] == 0 && walkable(x, y))
            {
                ++numPartitions;
                markOnePartition(BWAPI::WalkPosition(x, y));
            }
        }
    }

    UAB_ASSERT(numPartitions > 0, "no partitions");
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

MapPartitions::MapPartitions()
    : width(0)
    , height(0)
    , numPartitions(0)
    , revision(0)
{
}

void MapPartitions::initialize()
{
    width = 4 * BWAPI::Broodwar->mapWidth();
    height = 4 * BWAPI::Broodwar->mapHeight();
    removedNeutralIDs.clear();
    neutralFootprints.clear();
    revision = 0;

    findUnwalkability();
    rebuildPartitions();
}

// A static neutral disappeared. Remove only the footprint which was actually
// inserted at startup. This matters for overlapping minerals/buildings and for
// objects whose type dimensions do not line up perfectly with 8x8 boundaries.
bool MapPartitions::removeNeutral(BWAPI::Unit unit)
{
    if (!unit)
    {
        return false;
    }

    const BWAPI::UnitType type = unit->getInitialType();
    if (type.canMove() || type.isFlyer() || type.isSpell())
    {
        return false;
    }

    // A geyser is a permanent terrain obstacle for path-analysis purposes. It
    // may be replaced by a refinery, but the route never becomes walkable.
    if (type == BWAPI::UnitTypes::Resource_Vespene_Geyser)
    {
        return false;
    }

    const int unitID = unit->getID();
    if (removedNeutralIDs.find(unitID) != removedNeutralIDs.end())
    {
        return false;
    }

    const auto footprintIt = neutralFootprints.find(unitID);
    if (footprintIt == neutralFootprints.end() || footprintIt->second.empty())
    {
        return false;
    }

    bool changed = false;
    bool exposedWalkTile = false;
    for (const BWAPI::WalkPosition & walk : footprintIt->second)
    {
        if (walk.isValid() && unwalkability[walk.x][walk.y] > 0)
        {
            --unwalkability[walk.x][walk.y];
            changed = true;

            // Benzene and a few other maps stack neutral blockers on the
            // same walk tiles. Removing one visual layer does not change
            // pathing while another blocker still covers every tile.
            if (unwalkability[walk.x][walk.y] == 0)
            {
                exposedWalkTile = true;
            }
        }
    }

    if (!changed)
    {
        return false;
    }

    removedNeutralIDs.insert(unitID);
    neutralFootprints.erase(footprintIt);

    // Tell The::onNeutralDestroyed() to run the expensive map-analysis
    // rebuild only when at least one walk tile genuinely became free.
    if (!exposedWalkTile)
    {
        return false;
    }

    rebuildPartitions();
    ++revision;
    return true;
}

bool MapPartitions::walkable(int walkX, int walkY) const
{
    if (walkX < 0 || walkY < 0 || walkX >= width || walkY >= height ||
        size_t(walkX) >= unwalkability.size() ||
        size_t(walkY) >= unwalkability[walkX].size())
    {
        reportPartitionGridError("walkable", walkX, walkY, width, height);
        return false;
    }
    return unwalkability[walkX][walkY] == 0;
}

bool MapPartitions::walkable(const BWAPI::WalkPosition & pos) const
{
    return walkable(pos.x, pos.y);
}

int MapPartitions::id(int walkX, int walkY) const
{
    if (walkX < 0 || walkY < 0 || walkX >= width || walkY >= height ||
        size_t(walkX) >= partition.size() ||
        size_t(walkY) >= partition[walkX].size())
    {
        reportPartitionGridError("id", walkX, walkY, width, height);
        return 0;
    }
    return partition[walkX][walkY];
}

int MapPartitions::id(const BWAPI::WalkPosition & pos) const
{
    return id(pos.x, pos.y);
}

int MapPartitions::id(const BWAPI::TilePosition & pos) const
{
    return id(BWAPI::WalkPosition(pos));
}

int MapPartitions::id(const BWAPI::Position & pos) const
{
    return id(BWAPI::WalkPosition(pos));
}

bool MapPartitions::connected(const BWAPI::TilePosition & a, const BWAPI::TilePosition & b) const
{
    const int aID = id(a);
    return aID > 0 && aID == id(b);
}

bool MapPartitions::connected(const BWAPI::Position & a, const BWAPI::Position & b) const
{
    const int aID = id(a);
    return aID > 0 && aID == id(b);
}

void MapPartitions::drawWalkable() const
{
    for (int x = 0; x < width; ++x)
    {
        for (int y = 0; y < height; ++y)
        {
            if (walkable(x, y))
            {
                BWAPI::Position pos = BWAPI::Position(BWAPI::WalkPosition(x, y));
                BWAPI::Broodwar->drawCircleMap(pos.x + 4, pos.y + 4, 1, BWAPI::Colors::Blue);
            }
        }
    }
}

void MapPartitions::drawPartition(int i, BWAPI::Color color) const
{
    if (i >= 0 && i <= numPartitions)
    {
        for (int x = 0; x < width; ++x)
        {
            for (int y = 0; y < height; ++y)
            {
                if (partition[x][y] == i)
                {
                    BWAPI::Position pos = BWAPI::Position(BWAPI::WalkPosition(x, y));
                    BWAPI::Broodwar->drawCircleMap(pos.x + 4, pos.y + 4, 1, color);
                }
            }
        }
    }
}
