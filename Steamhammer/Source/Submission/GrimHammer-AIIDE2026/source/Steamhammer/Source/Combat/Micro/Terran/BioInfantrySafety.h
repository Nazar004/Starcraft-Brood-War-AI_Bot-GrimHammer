#pragma once

#include "Core/The.h"
#include "Strategy/StrategyManager.h"
#include "Map/MapTools.h"
#include <climits>

namespace UAlbertaBot
{
// Immediate hazards own movement before squad clustering, including Advance
// and Regroup. One snapshot per frame, no full-map searches or retained units.
class BioInfantrySafety
{
    struct Hazard
    {
        BWAPI::Position position;
        int radius;
    };
    struct Snapshot
    {
        int frame = -1;
        std::vector<Hazard> hazards;
        std::map<int, bool> decisions;
    };

    static Snapshot& snapshot()
    {
        static Snapshot cache;
        if (cache.frame == the.now()) return cache;
        cache.frame = the.now();
        cache.hazards.clear();
        cache.decisions.clear();
        for (BWAPI::Bullet bullet : BWAPI::Broodwar->getBullets())
        {
            if (!bullet || bullet->getType() != BWAPI::BulletTypes::Psionic_Storm)
                continue;
            BWAPI::Position position = bullet->getPosition();
            if (!position.isValid()) position = bullet->getTargetPosition();
            if (position.isValid()) cache.hazards.push_back({ position, 80 });
        }
        for (BWAPI::Unit enemy : the.enemy()->getUnits())
        {
            if (enemy && enemy->exists() && enemy->isVisible() &&
                enemy->getType() == BWAPI::UnitTypes::Protoss_Scarab &&
                enemy->getPosition().isValid())
                cache.hazards.push_back({ enemy->getPosition(), 80 });
        }
        return cache;
    }

public:
    static bool evade(BWAPI::Unit unit)
    {
        if (!StrategyManager::Instance().bioTankPlanActive() ||
            !unit || !unit->exists() || !unit->isCompleted() || unit->isLoaded() ||
            !unit->getPosition().isValid() || unit->isStasised() ||
            unit->isMaelstrommed() || unit->isLockedDown() ||
            (unit->getType() != BWAPI::UnitTypes::Terran_Marine &&
             unit->getType() != BWAPI::UnitTypes::Terran_Medic &&
             unit->getType() != BWAPI::UnitTypes::Terran_Firebat)) return false;

        Snapshot& cache = snapshot();
        const auto previous = cache.decisions.find(unit->getID());
        if (previous != cache.decisions.end()) return previous->second;

        const BWAPI::Position start = unit->getPosition();
        // Some BWAPI versions expose the affected-unit flag before the bullet.
        const bool underStorm = unit->isUnderStorm();
        auto clearance = [&cache, &start, underStorm](const BWAPI::Position& position)
        {
            int result = underStorm ? position.getApproxDistance(start) - 64 : INT_MAX;
            for (const Hazard& hazard : cache.hazards)
                result = std::min(result,
                    position.getApproxDistance(hazard.position) - hazard.radius);
            return result;
        };
        const int current = clearance(start);
        if (current >= 0)
        {
            cache.decisions[unit->getID()] = false;
            return false;
        }

        BWAPI::Position best = BWAPI::Positions::Invalid;
        int bestClearance = current;
        int bestCost = INT_MAX;
        const int directions[][2] = {
            {1,0}, {-1,0}, {0,1}, {0,-1},
            {1,1}, {1,-1}, {-1,1}, {-1,-1}
        };
        for (int step : { 48, 80, 112 }) for (int index = 0; index < 8; ++index)
        {
            const auto& direction = directions[(index + unit->getID()) % 8];
            const BWAPI::Position candidate(start.x + step * direction[0],
                start.y + step * direction[1]);
            if (!candidate.isValid() ||
                !the.partitions.connected(start, candidate)) continue;
            bool clear = true;
            // A clear endpoint alone can still lie across a closed gate/wall.
            for (int part = 1; part <= 8; ++part)
            {
                const BWAPI::Position point(
                    start.x + (candidate.x - start.x) * part / 8,
                    start.y + (candidate.y - start.y) * part / 8);
                if (!the.map.isGroundPositionWalkable(point, unit->getType(), true))
                {
                    clear = false;
                    break;
                }
            }
            if (!clear) continue;
            // Once outside every blast, prefer a short exit with less enemy fire.
            const int margin = std::min(0, clearance(candidate));
            const int cost = start.getApproxDistance(candidate) +
                128 * the.groundHits(BWAPI::TilePosition(candidate));
            if (margin > bestClearance ||
                (margin == bestClearance && margin > current && cost < bestCost))
            {
                best = candidate;
                bestClearance = margin;
                bestCost = cost;
            }
        }
        const bool escaping = best.isValid();
        cache.decisions[unit->getID()] = escaping;
        if (escaping) the.micro.Move(unit, best);
        return escaping;
    }
};
}
