#include "Map/MapGrid.h"

#include "Map/Bases.h"
#include "Core/The.h"

#include <algorithm>

using namespace UAlbertaBot;

// MapGrid divides the map into large square cells (cells at the right or bottom may be cut off
// by the edge of the map). It keeps track of friendly and visible enemy units in each cell,
// updated each frame, as well as when the cell was last seen and whether there is a comsat scan
// active there.
// Also the last time an enemy was seen in the cell, but that is unused so far.

MapGrid & MapGrid::Instance() 
{
    static MapGrid instance(BWAPI::Broodwar->mapWidth()*32, BWAPI::Broodwar->mapHeight()*32, Config::Tools::MAP_GRID_SIZE);
    return instance;
}

MapGrid::MapGrid()
{
}

MapGrid::MapGrid(int mapWidth, int mapHeight, int cellSize) 
    : mapWidth(mapWidth)
    , mapHeight(mapHeight)
    , cellSize(cellSize)
    , cols((mapWidth + cellSize - 1) / cellSize)
    , rows((mapHeight + cellSize - 1) / cellSize)
    , cells(rows * cols)
    , lastUpdated(0)
{
    calculateCellCenters();
}

// Return the first of:
// 1. Any starting base location that has not been explored.
// 2. The least-recently explored accessible cell (with attention to byGround).
// In either case, break ties with the cell farthest from our starting base location.
// Item 1 ensures that, if early game scouting failed, we scout with force.
// If byGround, only locations that are reachable by ground (i.e., in the given partition).
// If not byGround, the partition is not used.
BWAPI::Position MapGrid::getLeastExplored(bool byGround, int partition) 
{
    // 1. Any starting location that has not been explored.
    for (Base * base : the.bases.getStarting())
    {
		BWAPI::TilePosition tile = base->getTilePosition();
        if (!BWAPI::Broodwar->isExplored(tile) &&
            (!byGround || partition == the.partitions.id(tile)))
        {
            return BWAPI::Position(tile);
        }
    }

    // 2. The most distant of the least-recently explored tiles.
    int minSeen = MAX_FRAME;
    int minSeenDist = 0;
    int leastRow(0), leastCol(0);

    for (int r=0; r<rows; ++r)
    {
        for (int c=0; c<cols; ++c)
        {
            BWAPI::Position cellCenter = getCellCenter(r,c);

            // Skip places that we can't get to.
            if (byGround && partition != the.partitions.id(cellCenter))
            {
                continue;
            }

            BWAPI::Position home(the.self()->getStartLocation());
            int dist = home.getApproxDistance(getCellByIndex(r, c).center);
            int lastVisited = getCellByIndex(r, c).timeLastVisited;
			if (lastVisited < minSeen || lastVisited == minSeen && dist > minSeenDist)
            {
                leastRow = r;
                leastCol = c;
                minSeen = lastVisited;
                minSeenDist = dist;
            }
        }
    }

    return getCellCenter(leastRow, leastCol);
}

// Late-game cleanup needs several squads and scans to cover different parts
// of the map instead of selecting the same single least-visited cell. Choose a
// lane from the oldest small band; a recent scan counts as observation too.
BWAPI::Position MapGrid::getLeastExploredForSweep(bool byGround, int partition, int lane)
{
    struct Candidate
    {
        BWAPI::Position position;
        int lastObserved;
        int distanceFromHome;
    };

    std::vector<Candidate> candidates;
    const BWAPI::Position home(the.self()->getStartLocation());

    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            const BWAPI::Position center = getCellCenter(r, c);
            if (!center.isValid() ||
                (byGround && partition != the.partitions.id(center)))
            {
                continue;
            }

            const GridCell& cell = getCellByIndex(r, c);
            candidates.push_back(
                Candidate{
                    center,
                    std::max(cell.timeLastVisited, cell.timeLastScan),
                    home.getApproxDistance(center)
                });
        }
    }

    if (candidates.empty())
    {
        return BWAPI::Positions::Invalid;
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& a, const Candidate& b)
        {
            if (a.lastObserved != b.lastObserved)
            {
                return a.lastObserved < b.lastObserved;
            }
            return a.distanceFromHome > b.distanceFromHome;
        });

    const int bandSize = std::min(8, int(candidates.size()));
    const int index = ((lane % bandSize) + bandSize) % bandSize;
    return candidates[index].position;
}

// Return the least-recently explored cell "near" the given position.
// In case of ties, the nearest one.
// This one does not care about starting positions or distance from home.
// NOTE If you ask for a safe cell, there may not be one. Check the result for validity.
BWAPI::Position MapGrid::getLeastExploredNear(
    const BWAPI::Position & center,
    bool byGround,
    bool safe,
    int maxDistancePixels)
{
    int minSeen = MAX_FRAME;
	int minSeenDist = MAX_DISTANCE;
	int leastRow(-1), leastCol(-1);

    for (int r = 0; r<rows; ++r)
    {
        for (int c = 0; c<cols; ++c)
        {
            BWAPI::Position cellCenter = getCellCenter(r, c);

            // Skip places that we can't get to.
            if (byGround && the.partitions.id(center) != the.partitions.id(cellCenter))
            {
                continue;
            }

			// Skip unsafe places if requested to.
			if (safe && the.bases.myMain()->getSafeTileDistance(!byGround, cellCenter) < 0)
			{
				continue;
			}

            if (center.getApproxDistance(cellCenter) <= maxDistancePixels &&
                the.zone.sameArea(center, cellCenter))
            {
                int lastVisited = getCellByIndex(r, c).timeLastVisited;
				int dist = center.getApproxDistance(cellCenter);
                if (lastVisited < minSeen || lastVisited == minSeen && dist < minSeenDist)
                {
					minSeen = lastVisited;
					minSeenDist = dist;
					leastRow = r;
                    leastCol = c;
                }
            }
        }
    }

    if (leastRow < 0 || leastCol < 0)
    {
        return BWAPI::Positions::Invalid;
    }

    return getCellCenter(leastRow, leastCol);
}

// Return the least-recently explored cell "near" the given base, and its natural if any.
// In case of ties, the nearest one.
// NOTE If you ask for a safe cell, there may not be one. Check the result for validity.
BWAPI::Position MapGrid::getLeastExploredNearBase(Base * base, bool byGround, bool safe)
{
    if (!base->getNatural())
    {
        // No natural. Use the plain method.
        return getLeastExploredNear(base->getPosition(), byGround, safe);
    }

    // Look at the base and its natural.
    BWAPI::Position center1 = base->getPosition();
    BWAPI::Position center2 = base->getNatural()->getPosition();

	int minSeen = MAX_FRAME;
	int minSeenDist = MAX_DISTANCE;
	int leastRow(-1), leastCol(-1);

    for (int r = 0; r<rows; ++r)
    {
        for (int c = 0; c<cols; ++c)
        {
            BWAPI::Position cellCenter = getCellCenter(r, c);

            // Skip places that we can't get to.
            if (byGround &&
                the.partitions.id(center1) != the.partitions.id(cellCenter) &&
                the.partitions.id(center2) != the.partitions.id(cellCenter))
            {
                continue;
            }

			// Skip unsafe places if requested to.
			if (safe && the.bases.myMain()->getSafeTileDistance(!byGround, cellCenter) < 0)
			{
				continue;
			}

			if (center1.getApproxDistance(cellCenter) <= 32 * 32 && the.zone.sameArea(center1, cellCenter) ||
                center2.getApproxDistance(cellCenter) <= 24 * 32 && the.zone.sameArea(center2, cellCenter))
            {
                int lastVisited = getCellByIndex(r, c).timeLastVisited;
				int dist = std::min(center1.getApproxDistance(cellCenter), center2.getApproxDistance(cellCenter));
				if (lastVisited < minSeen || lastVisited == minSeen && dist < minSeenDist)
				{
					minSeen = lastVisited;
					minSeenDist = dist;
					leastRow = r;
					leastCol = c;
				}
			}
		}
    }

    if (leastRow < 0 || leastCol < 0)
    {
        return BWAPI::Positions::Invalid;
    }

    return getCellCenter(leastRow, leastCol);
}

void MapGrid::calculateCellCenters()
{
    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            GridCell & cell = getCellByIndex(r, c);

            int centerX = (c * cellSize) + (cellSize / 2);
            int centerY = (r * cellSize) + (cellSize / 2);

            // if the x position goes past the end of the map
            if (centerX > mapWidth)
            {
                // when did the last cell start
                int lastCellStart = c * cellSize;

                // how wide did we go
                int tooWide = mapWidth - lastCellStart;

                // go half the distance between the last start and how wide we were
                centerX = lastCellStart + (tooWide / 2);
            }
            else if (centerX == mapWidth)
            {
                centerX -= 50;
            }

            if (centerY > mapHeight)
            {
                // when did the last cell start
                int lastCellStart = r * cellSize;

                // how high did we go
                int tooHigh = mapHeight - lastCellStart;

                // go half the distance between the last start and how wide we were
                centerY = lastCellStart + (tooHigh / 2);
            }
            else if (centerY == mapHeight)
            {
                centerY -= 50;
            }

            cell.center = BWAPI::Position(centerX, centerY);
        }
    }
}

BWAPI::Position MapGrid::getCellCenter(int row, int col)
{
    if (row < 0 || row >= rows || col < 0 || col >= cols)
    {
        return BWAPI::Positions::Invalid;
    }
    return getCellByIndex(row, col).center;
}

// clear the vectors in the grid
void MapGrid::clearGrid() { 

    for (size_t i(0); i<cells.size(); ++i) 
    {
        cells[i].ourUnits.clear();
        cells[i].oppUnits.clear();
    }
}

// Populate the grid with units.
// Include all buildings, but other units only if they are completed.
// For the enemy, only include visible units (InformationManager remembers units which are out of sight).
void MapGrid::update() 
{
    if (Config::Debug::DrawMapGrid) 
    {
        for (int i=0; i<cols; i++) 
        {
            BWAPI::Broodwar->drawLineMap(i*cellSize, 0, i*cellSize, mapHeight, BWAPI::Colors::Blue);
        }

        for (int j=0; j<rows; j++) 
        {
            BWAPI::Broodwar->drawLineMap(0, j*cellSize, mapWidth, j*cellSize, BWAPI::Colors::Blue);
        }

        for (int r=0; r < rows; ++r)
        {
            for (int c=0; c < cols; ++c)
            {
                GridCell & cell = getCellByIndex(r,c);
            
                BWAPI::Broodwar->drawTextMap(cell.center.x-40, cell.center.y, "Last Visited %d", cell.timeLastVisited);
                BWAPI::Broodwar->drawTextMap(cell.center.x-40, cell.center.y+10, "Row/Col (%d, %d)", r, c);
            }
        }
    }

    clearGrid();

    //BWAPI::Broodwar->printf("MapGrid info: WH(%d, %d)  CS(%d)  RC(%d, %d)  C(%d)", mapWidth, mapHeight, cellSize, rows, cols, cells.size());

    for (BWAPI::Unit unit : the.self()->getUnits()) 
    {
        if (unit->isCompleted() || unit->getType().isBuilding())
        {
            getCell(unit).ourUnits.insert(unit);
            getCell(unit).timeLastVisited = BWAPI::Broodwar->getFrameCount();
        }
    }

    for (BWAPI::Unit unit : BWAPI::Broodwar->enemy()->getUnits()) 
    {
        if ((unit->isCompleted() || unit->getType().isBuilding()) &&
            unit->getHitPoints() > 0 &&
            unit->getType() != BWAPI::UnitTypes::Unknown) 
        {
            getCell(unit).oppUnits.insert(unit);
            getCell(unit).timeLastOpponentSeen = BWAPI::Broodwar->getFrameCount();
        }
    }
}

// Return the set of units in the given circle.
void MapGrid::getUnits(BWAPI::Unitset & units, BWAPI::Position center, int radius, bool ourUnits, bool oppUnits)
{
    const int x0(std::max( (center.x - radius) / cellSize, 0));
    const int x1(std::min( (center.x + radius) / cellSize, cols-1));
    const int y0(std::max( (center.y - radius) / cellSize, 0));
    const int y1(std::min( (center.y + radius) / cellSize, rows-1));
    const int radiusSq(radius * radius);

    for(int y(y0); y<=y1; ++y)
    {
        for(int x(x0); x<=x1; ++x)
        {
            int row = y;
            int col = x;

            const GridCell & cell(getCellByIndex(row,col));
            if(ourUnits)
            {
                for (BWAPI::Unit unit : cell.ourUnits)
                {
                    BWAPI::Position d(unit->getPosition() - center);
                    if(d.x * d.x + d.y * d.y <= radiusSq)
                    {
                        if (!units.contains(unit)) 
                        {
                            units.insert(unit);
                        }
                    }
                }
            }
            if(oppUnits)
            {
                for (BWAPI::Unit unit : cell.oppUnits) if (unit->getType() != BWAPI::UnitTypes::Unknown)
                {
                    BWAPI::Position d(unit->getPosition() - center);
                    if(d.x * d.x + d.y * d.y <= radiusSq)
                    {
                        if (!units.contains(unit))
                        { 
                            units.insert(unit); 
                        }
                    }
                }
            }
        }
    }
}

// Has the cell covering the given position ever been seen?
bool MapGrid::hasBeenScouted(const BWAPI::Position & pos)
{
	GridCell & cell = getCell(pos);
	return cell.timeLastVisited > 0;
}

// We scanned the given position. Record it so we don't scan the same position
// again before it wears off.
void MapGrid::scanAtPosition(const BWAPI::Position & pos)
{
    const int now = BWAPI::Broodwar->getFrameCount();
    GridCell & cell = getCell(pos);
    cell.timeLastScan = now;

    // The coarse grid is useful for scouting, but a scan does not reveal an
    // entire 320-pixel cell. Keep the actual scan centers for detection checks.
    recentScans.erase(
        std::remove_if(recentScans.begin(), recentScans.end(),
            [now](const std::pair<BWAPI::Position, int>& scan)
            {
                return scan.second > now ||
                    now - scan.second >= GridCell::ScanDuration;
            }),
        recentScans.end());
    recentScans.emplace_back(pos, now);
}

// Is a comsat scan already active at the given position?
bool MapGrid::scanIsActiveAt(const BWAPI::Position & pos)
{
    const int now = BWAPI::Broodwar->getFrameCount();
    const int scanRadius = 8 * 32;
    const int scanRadiusSquared = scanRadius * scanRadius;

    for (const auto& scan : recentScans)
    {
        const int age = now - scan.second;
        if (age < 0 || age >= GridCell::ScanDuration)
        {
            continue;
        }

        const int dx = pos.x - scan.first.x;
        const int dy = pos.y - scan.first.y;
        if (dx * dx + dy * dy <= scanRadiusSquared)
        {
            return true;
        }
    }

    return false;
}
