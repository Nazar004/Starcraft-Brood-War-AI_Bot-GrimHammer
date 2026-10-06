#pragma once

#include "Utilities/UnitUtil.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace UAlbertaBot
{
// Keep mobile allies clear of a structure that a friendly deployed Tank can
// shell. Shared by infantry, their healers and the two light vehicles.
class TerranSiegeSafety
{
public:
    static std::vector<BWAPI::Unit> coveredBuildings(
        const BWAPI::Unitset& targets)
    {
        std::vector<BWAPI::Unit> tanks;
        for (BWAPI::Unit tank : BWAPI::Broodwar->self()->getUnits())
        {
            if (tank && tank->exists() && tank->isCompleted() &&
                tank->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            {
                tanks.push_back(tank);
            }
        }

        std::vector<BWAPI::Unit> buildings;
        if (tanks.empty()) return buildings;
        for (BWAPI::Unit building : targets)
        {
            if (!building || !building->exists() ||
                !building->getType().isBuilding() || building->isFlying() ||
                !building->isVisible() || !building->getPosition().isValid())
            {
                continue;
            }
            for (BWAPI::Unit tank : tanks)
            {
                if (tank->isInWeaponRange(building))
                {
                    buildings.push_back(building);
                    break;
                }
            }
        }
        return buildings;
    }

    static BWAPI::Unit nearestBuilding(
        BWAPI::Unit unit, const std::vector<BWAPI::Unit>& buildings,
        int maxDistance = 7 * 32)
    {
        BWAPI::Unit nearest = nullptr;
        int bestDistance = maxDistance + 1;
        if (!unit || !unit->getPosition().isValid()) return nullptr;
        for (BWAPI::Unit building : buildings)
        {
            if (!building || !building->exists()) continue;
            const int distance = unit->getDistance(building);
            if (distance < bestDistance)
            {
                nearest = building;
                bestDistance = distance;
            }
        }
        return nearest;
    }

    static bool inBlastZone(
        BWAPI::Unit unit, const std::vector<BWAPI::Unit>& buildings)
    {
        const BWAPI::Unit nearest = nearestBuilding(unit, buildings, 3 * 32);
        return nearest && unit->getDistance(nearest) < 3 * 32;
    }

    static BWAPI::Position standoff(BWAPI::Unit unit, BWAPI::Unit building)
    {
        if (!unit || !building || !unit->getPosition().isValid() ||
            !building->getPosition().isValid())
        {
            return BWAPI::Positions::Invalid;
        }
        const BWAPI::Position from = unit->getPosition();
        const BWAPI::Position center = building->getPosition();
        const double dx = double(from.x - center.x);
        const double dy = double(from.y - center.y);
        const double length = std::hypot(dx, dy);
        if (length < 1.0) return BWAPI::Positions::Invalid;
        const int step = std::max(32, 4 * 32 - unit->getDistance(building) + 24);
        const BWAPI::Position destination(
            int(from.x + dx * step / length),
            int(from.y + dy * step / length));
        return destination.isValid() ? destination : BWAPI::Positions::Invalid;
    }
};
}
