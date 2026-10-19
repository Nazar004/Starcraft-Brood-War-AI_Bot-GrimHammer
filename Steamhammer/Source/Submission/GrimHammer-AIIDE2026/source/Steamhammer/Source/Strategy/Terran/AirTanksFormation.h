#pragma once

#include "Core/The.h"
#include "Map/Bases.h"
#include <vector>

namespace UAlbertaBot { namespace AirTanksFormation
{
    inline bool isAvailableTank(BWAPI::Unit unit)
    {
        return unit && unit->exists() && unit->isCompleted() &&
            !unit->isLoaded() && !unit->isStasised() &&
            !unit->isLockedDown() && !unit->isMaelstrommed() &&
            unit->getPosition().isValid() &&
            (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
             unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
    }

    // Choose the forward meaningful Tank group, not a single reinforcement
    // that appeared beside a Factory. The same anchor is shared by the Flying
    // order and Wraith leash for the current frame.
    inline BWAPI::Unit frontTank(const BWAPI::Unitset& groundUnits)
    {
        static int cachedFrame = -1;
        static BWAPI::Unit cachedTank = nullptr;
        if (cachedFrame == the.now() && isAvailableTank(cachedTank) &&
            groundUnits.count(cachedTank))
            return cachedTank;

        cachedFrame = the.now();
        cachedTank = nullptr;
        std::vector<BWAPI::Unit> tanks;
        for (BWAPI::Unit unit : groundUnits)
            if (isAvailableTank(unit)) tanks.push_back(unit);
        if (tanks.empty()) return nullptr;

        const Base* enemyBase = the.bases.enemyStart();
        const Base* homeBase = the.bases.myMain();
        const BWAPI::Position enemy = enemyBase
            ? enemyBase->getPosition() : BWAPI::Positions::Invalid;
        const BWAPI::Position home = homeBase
            ? homeBase->getPosition() : BWAPI::Positions::Invalid;
        const auto progress = [&](BWAPI::Unit unit)
        {
            return enemy.isValid() ? -unit->getDistance(enemy)
                : home.isValid() ? unit->getDistance(home) : 0;
        };

        BWAPI::Unit bestSeed = nullptr;
        int bestTier = -1, bestProgress = -MAX_DISTANCE, bestCount = -1;
        for (BWAPI::Unit seed : tanks)
        {
            int nearby = 0;
            for (BWAPI::Unit other : tanks)
                if (seed->getDistance(other) <= 10 * 32 &&
                    the.partitions.connected(seed->getPosition(), other->getPosition()))
                    ++nearby;
            const int tier = nearby >= 2 ? 1 : 0;
            const int forward = progress(seed);
            if (tier > bestTier ||
                (tier == bestTier && (forward > bestProgress ||
                    (forward == bestProgress && nearby > bestCount))))
            {
                bestTier = tier;
                bestProgress = forward;
                bestCount = nearby;
                bestSeed = seed;
            }
        }

        cachedTank = bestSeed;
        bestProgress = progress(bestSeed);
        for (BWAPI::Unit unit : tanks)
        {
            if (unit->getDistance(bestSeed) > 10 * 32 ||
                !the.partitions.connected(unit->getPosition(), bestSeed->getPosition()))
                continue;
            const int forward = progress(unit);
            if (forward > bestProgress)
            {
                bestProgress = forward;
                cachedTank = unit;
            }
        }
        return cachedTank;
    }
} }
