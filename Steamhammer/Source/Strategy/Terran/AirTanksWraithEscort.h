#pragma once

#include "Strategy/Terran/AirTanksFormation.h"
#include "Utilities/UnitUtil.h"
#include <algorithm>
#include <vector>

namespace UAlbertaBot { namespace AirTanksWraithEscort
{
    struct State
    {
        int frame = -1;
        int lastThreatFrame = -100000;
        BWAPI::Unit frontTank = nullptr;
        BWAPI::Unit guardedTank = nullptr;
        BWAPI::Unit threat = nullptr;
        bool defending = false;
    };

    inline State& state()
    {
        static State current;
        return current;
    }

    // One assessment per frame for the Tank/Wraith formation. The short
    // memory keeps the air screen with the Tanks until a threat has cleared,
    // instead of alternating between a building and a defender each frame.
    inline const State& update(const BWAPI::Unitset& groundUnits)
    {
        State& result = state();
        const int now = the.now();
        if (result.frame == now) return result;
        if (result.frame > now) result = State();
        result.frame = now;
        result.frontTank = AirTanksFormation::frontTank(groundUnits);
        result.guardedTank = result.frontTank;
        result.threat = nullptr;
        if (!result.frontTank)
        {
            result.defending = false;
            return result;
        }

        bool tankUnderAttack = false;
        int bestThreatScore = -100000;
        struct NearbyThreat
        {
            BWAPI::Unit unit;
            int reach;
            bool dangerousAir;
            bool wraithCanAttack;
        };
        std::vector<NearbyThreat> nearbyThreats;
        for (BWAPI::Unit enemy : the.enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                !enemy->getPosition().isValid() ||
                enemy->getDistance(result.frontTank) > 26 * 32 ||
                (enemy->getType().isWorker() && !enemy->isAttacking()))
                continue;
            const bool threatensTanks = UnitUtil::CanAttackGround(enemy);
            const bool dangerousAir = enemy->isFlying() &&
                UnitUtil::CanAttackAir(enemy);
            if (!threatensTanks && !dangerousAir) continue;
            nearbyThreats.push_back({ enemy,
                threatensTanks
                    ? UnitUtil::GetAttackRangeAssumingUpgrades(
                        enemy->getType(), BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
                    : 0,
                dangerousAir,
                enemy->isDetected() &&
                    UnitUtil::CanAttack(BWAPI::UnitTypes::Terran_Wraith, enemy) });
        }
        for (BWAPI::Unit tank : groundUnits)
        {
            if (!AirTanksFormation::isAvailableTank(tank) ||
                tank->getDistance(result.frontTank) > 12 * 32)
                continue;

            if (tank->isUnderAttack())
            {
                tankUnderAttack = true;
                result.guardedTank = tank;
            }
            for (const NearbyThreat& candidate : nearbyThreats)
            {
                BWAPI::Unit enemy = candidate.unit;
                const int distance = enemy->getDistance(tank);
                if (distance > 14 * 32) continue;
                if (distance > std::max(6 * 32, candidate.reach + 2 * 32))
                    continue;

                result.lastThreatFrame = now;
                const int score =
                    ((enemy->getOrderTarget() == tank ||
                      enemy->getTarget() == tank) ? 400 : 0) +
                    (tank->isUnderAttack() ? 300 : 0) +
                    (candidate.dangerousAir ? 120 : 0) - distance;
                if (score > bestThreatScore && candidate.wraithCanAttack)
                {
                    bestThreatScore = score;
                    result.guardedTank = tank;
                    result.threat = enemy;
                }
            }
        }
        if (tankUnderAttack) result.lastThreatFrame = now;
        result.defending = now - result.lastThreatFrame <= 2 * 24;
        return result;
    }
} }
