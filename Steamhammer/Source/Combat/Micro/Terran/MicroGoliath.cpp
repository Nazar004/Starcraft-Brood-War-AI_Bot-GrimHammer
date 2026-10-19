#include "Combat/Micro/Terran/MicroGoliath.h"
#include "Combat/Micro/Terran/TerranFormation.h"
#include "Combat/Micro/Terran/TerranSiegeSafety.h"
#include "Utilities/Random.h"
#include "Map/Bases.h"
#include "Intelligence/InformationManager.h"
#include "Map/Grids/GridDistances.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"

#include "Strategy/StrategyManager.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

using namespace UAlbertaBot;

static bool isGoliathUnit(BWAPI::Unit unit)
{
    return unit && unit->exists() && unit->getType() == BWAPI::UnitTypes::Terran_Goliath;
}

// BWAPI supply is doubled: 360 == 180/200. In this mode Goliaths are
// intentionally committed. It must override local kite/formation caution,
// otherwise Squad can say "Banzai" while MicroGoliath still waits outside a
// Sunken/bunker/cannon range.
static bool goliathForcePushAtMaxSupply()
{
    return the.self() && the.self()->supplyUsed() >= 360;
}

static int goliathTargetTypePriority(BWAPI::Unit target)
{
    if (!target || !target->exists())
    {
        return 0;
    }

    const BWAPI::UnitType type = target->getType();

    if (type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
    {
        return 130;
    }

    if (type == BWAPI::UnitTypes::Protoss_Interceptor ||
        type == BWAPI::UnitTypes::Protoss_Scarab ||
        type == BWAPI::UnitTypes::Zerg_Larva ||
        type == BWAPI::UnitTypes::Zerg_Egg)
    {
        return 1;
    }

    if (type == BWAPI::UnitTypes::Protoss_Carrier)
    {
        return 100;
    }

    // Stop the carrier before it can repeat a drop. In particular a Shuttle
    // with a Reaver is more urgent than the Reaver after it is unloaded.
    if (type == BWAPI::UnitTypes::Protoss_Shuttle ||
        type == BWAPI::UnitTypes::Terran_Dropship)
    {
        return 115;
    }

    if (type == BWAPI::UnitTypes::Terran_Battlecruiser ||
        type == BWAPI::UnitTypes::Protoss_Arbiter ||
        type == BWAPI::UnitTypes::Protoss_Scout ||
        type == BWAPI::UnitTypes::Terran_Wraith ||
        type == BWAPI::UnitTypes::Terran_Valkyrie ||
        type == BWAPI::UnitTypes::Zerg_Guardian ||
        type == BWAPI::UnitTypes::Zerg_Devourer ||
        type == BWAPI::UnitTypes::Zerg_Mutalisk)
    {
        return 90;
    }

    if (type == BWAPI::UnitTypes::Protoss_Dragoon ||
        type == BWAPI::UnitTypes::Protoss_Reaver ||
        type == BWAPI::UnitTypes::Protoss_Archon ||
        type == BWAPI::UnitTypes::Terran_Goliath ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
        type == BWAPI::UnitTypes::Zerg_Hydralisk ||
        type == BWAPI::UnitTypes::Zerg_Lurker)
    {
        return 70;
    }

    if (type.isWorker())
    {
        return 40;
    }

    if (type.isBuilding())
    {
        return 20;
    }

    return 50;
}

static int goliathWeaknessScore(BWAPI::Unit target)
{
    if (!target || !target->exists())
    {
        return 999999;
    }

    const BWAPI::UnitType type = target->getType();

    const int hp =
        std::max(0, target->getHitPoints()) +
        std::max(0, target->getShields());

    const int armor = std::max(0, type.armor());

    return hp + armor * 35;
}

static bool isBetterGoliathTarget(BWAPI::Unit candidate, BWAPI::Unit currentBest, BWAPI::Unit goliath)
{
    if (!candidate || !candidate->exists())
    {
        return false;
    }

    if (!currentBest || !currentBest->exists())
    {
        return true;
    }

    const int candidatePriority = goliathTargetTypePriority(candidate);
    const int bestPriority = goliathTargetTypePriority(currentBest);

    if (candidatePriority != bestPriority)
    {
        return candidatePriority > bestPriority;
    }

    const int candidateWeakness = goliathWeaknessScore(candidate);
    const int bestWeakness = goliathWeaknessScore(currentBest);

    if (candidateWeakness != bestWeakness)
    {
        return candidateWeakness < bestWeakness;
    }

    if (goliath && goliath->exists())
    {
        return candidate->getDistance(goliath) < currentBest->getDistance(goliath);
    }

    return false;
}

static bool goliathCanAttackTarget(BWAPI::Unit goliath, BWAPI::Unit target)
{
    if (!goliath || !goliath->exists() || !target || !target->exists())
    {
        return false;
    }

    if (goliath->getType() != BWAPI::UnitTypes::Terran_Goliath)
    {
        return false;
    }

    const BWAPI::UnitType targetType = target->getType();

    if (targetType == BWAPI::UnitTypes::Zerg_Larva ||
        targetType == BWAPI::UnitTypes::Protoss_Scarab)
    {
        return false;
    }

    if (target->isFlying())
    {
        return goliath->getType().airWeapon() != BWAPI::WeaponTypes::None;
    }

    return goliath->getType().groundWeapon() != BWAPI::WeaponTypes::None;
}

static bool isImmediateLightGroundThreatForGoliath(BWAPI::Unit target)
{
    if (!target ||
        !target->exists() ||
        !target->isVisible() ||
        target->isFlying() ||
        target->getType().isBuilding())
    {
        return false;
    }

    const BWAPI::UnitType type = target->getType();

    return
        type == BWAPI::UnitTypes::Zerg_Zergling ||
        type == BWAPI::UnitTypes::Protoss_Zealot ||
        type == BWAPI::UnitTypes::Terran_Marine ||
        type == BWAPI::UnitTypes::Terran_Firebat;
}

static BWAPI::Unit getImmediateGroundThreatForGoliath(
    BWAPI::Unit goliath,
    const BWAPI::Unitset& targets)
{
    if (!isGoliathUnit(goliath))
    {
        return nullptr;
    }

    BWAPI::Unit bestTarget = nullptr;
    int bestScore = -MAX_DISTANCE;

    for (BWAPI::Unit target : targets)
    {
        if (!goliathCanAttackTarget(goliath, target) || target->isFlying())
        {
            continue;
        }

        // Workers can technically attack, but a passing miner is not an
        // immediate threat worth switching away from a Carrier or Dropship.
        if (target->getType().isWorker() && !UnitUtil::AttackOrder(target))
        {
            continue;
        }

        const int distance = goliath->getDistance(target);
        const bool canHitGoliath = UnitUtil::CanAttack(target, goliath);
        const bool lightThreat = isImmediateLightGroundThreatForGoliath(target);

        // This override is only for danger already in the Goliath line. A
        // distant Carrier remains the preferred strategic target, but a
        // Zealot/Zergling/Marine in firing distance may not be ignored.
        if (distance > 6 * 32 || (!canHitGoliath && !lightThreat))
        {
            continue;
        }

        int score = -distance;

        if (goliath->isInWeaponRange(target))
        {
            score += 12 * 32;
        }

        if (lightThreat)
        {
            score += 8 * 32;
        }

        if (canHitGoliath)
        {
            score += 6 * 32;
        }

        if (score > bestScore)
        {
            bestScore = score;
            bestTarget = target;
        }
    }

    return bestTarget;
}

static BWAPI::Unit getBestVisibleAntiAirTargetForGoliath(
    BWAPI::Unit goliath,
    BWAPI::Unit preferredCarrier)
{
    if (!isGoliathUnit(goliath))
    {
        return nullptr;
    }

    BWAPI::Unit preferredCarrierInRange = nullptr;
    BWAPI::Unit bestTransportInRange = nullptr;
    BWAPI::Unit bestCarrierInRange = nullptr;
    BWAPI::Unit bestCapitalInRange = nullptr;
    BWAPI::Unit bestInterceptorInRange = nullptr;
    BWAPI::Unit bestAnyAirTarget = nullptr;

    const bool pureAir =
        StrategyManager::Instance().getTerranArmyPosture() ==
            TerranArmyPosture::PureAir;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            !enemy->isDetected() || enemy->isInvincible() ||
            !enemy->getPosition().isValid())
        {
            continue;
        }

        if (!enemy->isFlying())
        {
            continue;
        }

        if (!goliathCanAttackTarget(goliath, enemy))
        {
            continue;
        }

        const BWAPI::UnitType type = enemy->getType();

        const int distance = goliath->getDistance(enemy);

        if (distance > 20 * 32)
        {
            continue;
        }

        const bool inWeaponRange =
            goliath->isInWeaponRange(enemy);

        if ((type == BWAPI::UnitTypes::Protoss_Shuttle ||
             type == BWAPI::UnitTypes::Terran_Dropship) && inWeaponRange &&
            isBetterGoliathTarget(enemy, bestTransportInRange, goliath))
        {
            bestTransportInRange = enemy;
        }

        if (type == BWAPI::UnitTypes::Protoss_Carrier)
        {
            if (inWeaponRange)
            {
                if (isBetterGoliathTarget(enemy, bestCarrierInRange, goliath))
                {
                    bestCarrierInRange = enemy;
                }

                if (enemy == preferredCarrier)
                {
                    preferredCarrierInRange = enemy;
                }
            }

            if (isBetterGoliathTarget(enemy, bestAnyAirTarget, goliath))
            {
                bestAnyAirTarget = enemy;
            }

            continue;
        }

        if (type == BWAPI::UnitTypes::Protoss_Interceptor)
        {
            if (inWeaponRange)
            {
                if (isBetterGoliathTarget(enemy, bestInterceptorInRange, goliath))
                {
                    bestInterceptorInRange = enemy;
                }
            }

            // В PureAir interceptor — нормальная цель, если Carrier не в range.
            if (pureAir && isBetterGoliathTarget(enemy, bestAnyAirTarget, goliath))
            {
                bestAnyAirTarget = enemy;
            }

            continue;
        }

        if (inWeaponRange)
        {
            if (isBetterGoliathTarget(enemy, bestCapitalInRange, goliath))
            {
                bestCapitalInRange = enemy;
            }
        }

        if (isBetterGoliathTarget(enemy, bestAnyAirTarget, goliath))
        {
            bestAnyAirTarget = enemy;
        }
    }

    if (bestTransportInRange)
    {
        return bestTransportInRange;
    }

    if (preferredCarrierInRange)
    {
        return preferredCarrierInRange;
    }

    if (bestCarrierInRange)
    {
        return bestCarrierInRange;
    }

    if (bestCapitalInRange)
    {
        return bestCapitalInRange;
    }

    if (bestInterceptorInRange)
    {
        return bestInterceptorInRange;
    }

    // A visible Wraith or Mutalisk outside weapon range is still a live
    // interception target. Keep distant Carriers on the bounded formation
    // controller below so a small group does not rush into their fleet.
    if (pureAir && bestAnyAirTarget &&
        (bestAnyAirTarget->getType() == BWAPI::UnitTypes::Protoss_Carrier ||
         bestAnyAirTarget->getType() == BWAPI::UnitTypes::Protoss_Interceptor))
    {
        return nullptr;
    }
    return bestAnyAirTarget;
}

static BWAPI::Position goliathFormationAdvancePosition(
    const BWAPI::Position& clusterCenter,
    const BWAPI::Position& targetPosition,
    int advancePixels)
{
    if (!clusterCenter.isValid() || !targetPosition.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const double dx = double(targetPosition.x - clusterCenter.x);
    const double dy = double(targetPosition.y - clusterCenter.y);
    const double length = std::sqrt(dx * dx + dy * dy);

    if (length < 1.0)
    {
        return clusterCenter;
    }

    const int step = std::min(advancePixels, int(length));
    int x = int(clusterCenter.x + dx / length * step);
    int y = int(clusterCenter.y + dy / length * step);
    x = std::max(0, std::min(x, BWAPI::Broodwar->mapWidth() * 32 - 1));
    y = std::max(0, std::min(y, BWAPI::Broodwar->mapHeight() * 32 - 1));
    return BWAPI::Position(x, y);
}


// For a large PureAir army the arithmetic cluster center is usually far behind
// the actual fighting line. If detached/rear Goliaths are ordered back to that
// center, the front keeps fighting while most of the army repeatedly regroups
// in the rear. Use a small group of the Goliaths closest to the objective as
// the formation front instead.
static BWAPI::Position getPureAirGoliathFrontCenter(
    const std::vector<BWAPI::Unit>& goliaths,
    const BWAPI::Position& objective,
    const GridDistances* routeDistances)
{
    if (goliaths.empty() || !objective.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    std::vector<std::pair<int, BWAPI::Unit>> byDistance;
    byDistance.reserve(goliaths.size());

    for (BWAPI::Unit unit : goliaths)
    {
        if (!isGoliathUnit(unit) || !unit->getPosition().isValid())
        {
            continue;
        }

        int distance = unit->getDistance(objective);

        // When the strategic objective has a ground distance map, use it.
        // Euclidean distance makes a Goliath standing against the wrong side
        // of a cliff look like the "front" unit even though the ramp is far
        // away. That was feeding the whole PureAir formation into the cliff.
        if (routeDistances && unit->getTilePosition().isValid())
        {
            const int tileDistance = routeDistances->at(unit->getTilePosition());
            if (tileDistance >= 0)
            {
                distance = tileDistance * 32;
            }
        }

        byDistance.push_back(std::make_pair(distance, unit));
    }

    if (byDistance.empty())
    {
        return BWAPI::Positions::Invalid;
    }

    std::sort(
        byDistance.begin(),
        byDistance.end(),
        [](const std::pair<int, BWAPI::Unit>& a,
           const std::pair<int, BWAPI::Unit>& b)
        {
            if (a.first != b.first)
            {
                return a.first < b.first;
            }

            return a.second->getID() < b.second->getID();
        });

    // Average several front units rather than following one lucky/pathing
    // outlier. Scale slowly with army size, but keep the reference compact.
    const size_t frontCount =
        std::min(
            byDistance.size(),
            std::max<size_t>(3, std::min<size_t>(10, goliaths.size() / 6 + 2)));

    long long sumX = 0;
    long long sumY = 0;

    for (size_t i = 0; i < frontCount; ++i)
    {
        const BWAPI::Position position = byDistance[i].second->getPosition();
        sumX += position.x;
        sumY += position.y;
    }

    return BWAPI::Position(
        int(sumX / frontCount),
        int(sumY / frontCount));
}

static BWAPI::Position nearestWalkableGoliathPoint(
    const BWAPI::Position& desired,
    const BWAPI::Position& reference);

// Move each Goliath from its own current position. Rear units aim for a point
// just behind/at the fighting front, while front units keep advancing toward
// the objective. This avoids both the old "return to cluster center" behavior
// and dozens of units receiving one identical destination.
static BWAPI::Position getPureAirGoliathAdvanceDestination(
    BWAPI::Unit goliath,
    const BWAPI::Position& frontCenter,
    const BWAPI::Position& objective,
    bool lockedDownTarget)
{
    if (!isGoliathUnit(goliath) ||
        !goliath->getPosition().isValid() ||
        !objective.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position current = goliath->getPosition();

    if (!frontCenter.isValid())
    {
        return goliathFormationAdvancePosition(
            current,
            objective,
            lockedDownTarget ? 8 * 32 : 6 * 32);
    }

    const int distanceToFront = goliath->getDistance(frontCenter);

    if (distanceToFront > 6 * 32)
    {
        // Catch the front, not the center of the entire blob. Preserve the
        // unit's lateral lane instead of sending every rear Goliath through
        // one catch-point, which creates a dense traffic cone behind the
        // fighting line.
        BWAPI::Position catchPoint =
            goliathFormationAdvancePosition(
                frontCenter,
                objective,
                2 * 32);
        if (!catchPoint.isValid())
        {
            catchPoint = frontCenter;
        }

        const double fx = double(objective.x - frontCenter.x);
        const double fy = double(objective.y - frontCenter.y);
        const double length = std::sqrt(fx * fx + fy * fy);
        if (length > 1.0)
        {
            const double sideX = -fy / length;
            const double sideY = fx / length;
            double lateral =
                double(current.x - frontCenter.x) * sideX +
                double(current.y - frontCenter.y) * sideY;

            // Keep several lanes, but don't let an old detached flank remain
            // half a screen away from the actual army.
            lateral = std::max(-3.0 * 32.0, std::min(3.0 * 32.0, lateral));
            BWAPI::Position laneCatch(
                int(catchPoint.x + sideX * lateral),
                int(catchPoint.y + sideY * lateral));

            laneCatch = nearestWalkableGoliathPoint(laneCatch, current);
            if (laneCatch.isValid())
            {
                catchPoint = laneCatch;
            }
        }

        return goliathFormationAdvancePosition(
            current,
            catchPoint,
            7 * 32);
    }

    // Units already near the fighting line continue to advance. Because the
    // step starts from each unit's own position, they retain their natural
    // lateral spread instead of converging onto one pixel.
    return goliathFormationAdvancePosition(
        current,
        objective,
        lockedDownTarget ? 7 * 32 : 5 * 32);
}

static bool isRealBaseAttackEnemyForGoliath(BWAPI::Unit enemy)
{
    if (!enemy || !enemy->exists() || !enemy->isVisible() || !enemy->getPosition().isValid())
    {
        return false;
    }

    const BWAPI::UnitType type = enemy->getType();

    if (type == BWAPI::UnitTypes::Zerg_Larva ||
        type == BWAPI::UnitTypes::Zerg_Egg ||
        type == BWAPI::UnitTypes::Protoss_Scarab ||
        type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
        type == BWAPI::UnitTypes::Protoss_Interceptor)
    {
        return false;
    }

    if (type == BWAPI::UnitTypes::Zerg_Overlord ||
        type == BWAPI::UnitTypes::Protoss_Observer)
    {
        return false;
    }

    if (type.isBuilding())
    {
        return false;
    }

    return UnitUtil::CanAttackGround(enemy) ||
        UnitUtil::CanAttackAir(enemy) ||
        type == BWAPI::UnitTypes::Protoss_Carrier ||
        type == BWAPI::UnitTypes::Terran_Dropship ||
        type == BWAPI::UnitTypes::Protoss_Shuttle;
}

static int baseAttackScoreForGoliath(BWAPI::Unit enemy)
{
    if (!isRealBaseAttackEnemyForGoliath(enemy))
    {
        return 0;
    }

    const BWAPI::UnitType type = enemy->getType();

    if (type == BWAPI::UnitTypes::Protoss_Carrier)              return 10;
    if (type == BWAPI::UnitTypes::Terran_Battlecruiser)         return 9;
    if (type == BWAPI::UnitTypes::Protoss_Arbiter)              return 8;
    if (type == BWAPI::UnitTypes::Zerg_Guardian)                return 8;
    if (type == BWAPI::UnitTypes::Zerg_Mutalisk)                return 7;
    if (type == BWAPI::UnitTypes::Terran_Wraith)                return 7;
    if (type == BWAPI::UnitTypes::Protoss_Scout)                return 7;
    if (type == BWAPI::UnitTypes::Zerg_Devourer)                return 7;
    if (type == BWAPI::UnitTypes::Terran_Dropship)              return 6;
    if (type == BWAPI::UnitTypes::Protoss_Shuttle)              return 6;

    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) return 8;
    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)  return 7;
    if (type == BWAPI::UnitTypes::Protoss_Reaver)               return 7;
    if (type == BWAPI::UnitTypes::Protoss_Dragoon)              return 5;
    if (type == BWAPI::UnitTypes::Zerg_Hydralisk)               return 5;
    if (type == BWAPI::UnitTypes::Zerg_Lurker)                  return 6;
    if (type == BWAPI::UnitTypes::Terran_Goliath)               return 5;
    if (type == BWAPI::UnitTypes::Terran_Vulture)               return 4;
    if (type == BWAPI::UnitTypes::Protoss_Zealot)               return 3;
    if (type == BWAPI::UnitTypes::Zerg_Zergling)                return 2;

    if (UnitUtil::CanAttackAir(enemy) || UnitUtil::CanAttackGround(enemy))
    {
        return 3;
    }

    return 1;
}

static BWAPI::Position getCurrentlyAttackedOwnedBasePositionForGoliath()
{
    Base* bestBase = nullptr;
    int bestScore = 0;

    for (Base* base : the.bases.getAll())
    {
        if (!base || !base->getPosition().isValid())
        {
            continue;
        }

        if (base->getOwner() != the.self())
        {
            continue;
        }

        if (!base->getDepot())
        {
            continue;
        }

        const BWAPI::Position basePos = base->getPosition();

        int score = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!isRealBaseAttackEnemyForGoliath(enemy))
            {
                continue;
            }

            if (enemy->getDistance(basePos) > 14 * 32)
            {
                continue;
            }

            score += baseAttackScoreForGoliath(enemy);
        }

        if (score > bestScore)
        {
            bestScore = score;
            bestBase = base;
        }
    }

    if (bestBase && bestScore >= 5)
    {
        return bestBase->getPosition();
    }

    return BWAPI::Positions::Invalid;
}

static BWAPI::Unit getClosestCompletedMissileTurretNearPositionForGoliath(
    const BWAPI::Position& position,
    int maxRadius)
{
    if (!position.isValid())
    {
        return nullptr;
    }

    BWAPI::Unit best = nullptr;
    int bestDistance = 999999;

    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (!unit ||
            !unit->exists() ||
            !unit->isCompleted() ||
            !unit->getPosition().isValid() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Missile_Turret)
        {
            continue;
        }

        const int distance = unit->getDistance(position);

        if (distance > maxRadius)
        {
            continue;
        }

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = unit;
        }
    }

    return best;
}

static int countFriendlyGoliathsNearPositionForBaseDefense(
    const BWAPI::Position& position,
    int radius)
{
    if (!position.isValid())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (!unit ||
            !unit->exists() ||
            !unit->isCompleted() ||
            !unit->getPosition().isValid() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Goliath)
        {
            continue;
        }

        if (unit->getDistance(position) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static int countEnemyCarriersNearPositionForGoliath(
    const BWAPI::Position& position,
    int radius)
{
    if (!position.isValid())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!enemy ||
            !enemy->exists() ||
            !enemy->isVisible() ||
            !enemy->getPosition().isValid())
        {
            continue;
        }

        if (enemy->getType() != BWAPI::UnitTypes::Protoss_Carrier)
        {
            continue;
        }

        if (enemy->getDistance(position) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static int countEnemyInterceptorsNearPositionForGoliath(
    const BWAPI::Position& position,
    int radius)
{
    if (!position.isValid())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!enemy ||
            !enemy->exists() ||
            !enemy->isVisible() ||
            !enemy->getPosition().isValid())
        {
            continue;
        }

        if (enemy->getType() != BWAPI::UnitTypes::Protoss_Interceptor)
        {
            continue;
        }

        if (enemy->getDistance(position) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static BWAPI::Unit getBestEnemyNearGoliathDefendedBase(
    BWAPI::Unit goliath,
    const BWAPI::Position& basePosition,
    const BWAPI::Unitset& targets)
{
    if (!goliath || !goliath->exists() || !basePosition.isValid())
    {
        return nullptr;
    }

    BWAPI::Unit best = nullptr;
    int bestScore = -1;
    int bestDistance = 999999;

    for (BWAPI::Unit enemy : targets)
    {
        if (!isRealBaseAttackEnemyForGoliath(enemy))
        {
            continue;
        }

        if (!goliathCanAttackTarget(goliath, enemy))
        {
            continue;
        }

        if (enemy->getDistance(basePosition) > 16 * 32)
        {
            continue;
        }

        int score = baseAttackScoreForGoliath(enemy) * 100;

        if (enemy->isFlying())
        {
            score += 200;
        }

        if (enemy->getType() == BWAPI::UnitTypes::Protoss_Carrier)
        {
            score += 500;
        }

        const int distance = goliath->getDistance(enemy);
        score -= distance / 8;

        if (score > bestScore ||
            (score == bestScore && distance < bestDistance))
        {
            bestScore = score;
            bestDistance = distance;
            best = enemy;
        }
    }

    return best;
}

static bool goliathCanConfidentlyFightCarrierNearBase(
    BWAPI::Unit goliath,
    BWAPI::Unit carrier,
    const BWAPI::Position& basePosition)
{
    if (!goliath ||
        !goliath->exists() ||
        !carrier ||
        !carrier->exists() ||
        carrier->getType() != BWAPI::UnitTypes::Protoss_Carrier)
    {
        return true;
    }

    const int nearbyGoliaths =
        countFriendlyGoliathsNearPositionForBaseDefense(
            carrier->getPosition(),
            14 * 32
        );

    const int nearbyCarriers =
        countEnemyCarriersNearPositionForGoliath(
            carrier->getPosition(),
            16 * 32
        );

    const int nearbyInterceptors =
        countEnemyInterceptorsNearPositionForGoliath(
            goliath->getPosition(),
            10 * 32
        );

    BWAPI::Unit turretNearBase =
        getClosestCompletedMissileTurretNearPositionForGoliath(
            basePosition,
            10 * 32
        );

    int requiredGoliaths = 5;

    requiredGoliaths += std::max(0, nearbyCarriers - 1) * 4;
    requiredGoliaths += std::max(0, nearbyInterceptors - 8) / 3;

    if (turretNearBase)
    {
        requiredGoliaths -= 2;
    }

    requiredGoliaths = std::max(3, requiredGoliaths);

    const int carrierHp =
        std::max(0, carrier->getHitPoints()) +
        std::max(0, carrier->getShields());

    if (carrierHp <= 120 && nearbyGoliaths >= 3)
    {
        return true;
    }

    return nearbyGoliaths >= requiredGoliaths;
}



// Find a walkable ground point close to an air/abstract objective. This keeps
// Goliaths from being ordered to the unwalkable side of a cliff simply because
// a Carrier or Wraith is hovering there.
static BWAPI::Position getReachableGroundObjectiveForGoliath(
    const BWAPI::Position& reference,
    const BWAPI::Position& objective,
    const BWAPI::Position& preferredAreaPosition = BWAPI::Positions::Invalid)
{
    if (!reference.isValid() || !objective.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::TilePosition referenceTile(reference);
    const BWAPI::TilePosition objectiveTile(objective);
    if (!referenceTile.isValid() || !objectiveTile.isValid())
    {
        return objective;
    }

    const int referencePartition = the.partitions.id(referenceTile);
    const Zone* preferredArea = preferredAreaPosition.isValid()
        ? the.zone.areaAt(preferredAreaPosition)
        : nullptr;

    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestScore = MAX_DISTANCE;

    for (int radius = 0; radius <= 7; ++radius)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            for (int dy = -radius; dy <= radius; ++dy)
            {
                if (radius > 0 && std::max(std::abs(dx), std::abs(dy)) != radius)
                {
                    continue;
                }

                const BWAPI::TilePosition tile(
                    objectiveTile.x + dx,
                    objectiveTile.y + dy);

                if (!tile.isValid() ||
                    !the.map.isWalkable(tile) ||
                    the.partitions.id(tile) != referencePartition)
                {
                    continue;
                }

                const BWAPI::Position candidate =
                    BWAPI::Position(tile) + BWAPI::Position(16, 16);
                if (!candidate.isValid())
                {
                    continue;
                }

                int score = candidate.getApproxDistance(objective);
                if (preferredArea)
                {
                    const Zone* candidateArea = the.zone.areaAt(candidate);
                    if (candidateArea == preferredArea)
                    {
                        score -= 4 * 32;
                    }
                }

                if (score < bestScore)
                {
                    bestScore = score;
                    best = candidate;
                }
            }
        }

        if (best.isValid() && bestScore <= radius * 32 + 24)
        {
            break;
        }
    }

    if (best.isValid())
    {
        return best;
    }

    // An aircraft can hover over an unreachable cliff or island. Returning
    // its air position here made ground Goliaths repeatedly run into the
    // terrain. Keep the army's ground order if it is on our partition.
    const BWAPI::TilePosition preferredTile(preferredAreaPosition);
    if (preferredTile.isValid() &&
        the.map.isWalkable(preferredTile) &&
        the.partitions.id(preferredTile) == referencePartition)
    {
        return preferredAreaPosition;
    }
    return reference;
}

static BWAPI::Position nearestWalkableGoliathPoint(
    const BWAPI::Position& desired,
    const BWAPI::Position& reference)
{
    if (!desired.isValid() || !reference.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::TilePosition referenceTile(reference);
    const int referencePartition = referenceTile.isValid()
        ? the.partitions.id(referenceTile)
        : 0;

    const BWAPI::TilePosition desiredTile(desired);
    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestDistance = MAX_DISTANCE;

    for (int radius = 0; radius <= 4; ++radius)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            for (int dy = -radius; dy <= radius; ++dy)
            {
                if (radius > 0 && std::max(std::abs(dx), std::abs(dy)) != radius)
                {
                    continue;
                }

                const BWAPI::TilePosition tile(
                    desiredTile.x + dx,
                    desiredTile.y + dy);

                if (!tile.isValid() || !the.map.isWalkable(tile))
                {
                    continue;
                }

                if (referencePartition > 0 &&
                    the.partitions.id(tile) != referencePartition)
                {
                    continue;
                }

                const BWAPI::Position candidate =
                    BWAPI::Position(tile) + BWAPI::Position(16, 16);
                const int distance = candidate.getApproxDistance(desired);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = candidate;
                }
            }
        }

        if (best.isValid())
        {
            break;
        }
    }

    return best.isValid() ? best : desired;
}

static bool isTankForMixedFormation(BWAPI::Unit unit)
{
    if (!unit || !unit->exists() || !unit->getPosition().isValid())
    {
        return false;
    }

    const BWAPI::UnitType type = unit->getType();
    return
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
}

static BWAPI::Position getMixedFormationTankCenter(
    const UnitCluster& cluster,
    const BWAPI::Unitset& formationUnits)
{
    long long sumX = 0;
    long long sumY = 0;
    int count = 0;

    for (BWAPI::Unit unit : cluster.units)
    {
        if (!isTankForMixedFormation(unit))
        {
            continue;
        }

        sumX += unit->getPosition().x;
        sumY += unit->getPosition().y;
        ++count;
    }

    if (count >= 3)
    {
        return BWAPI::Position(int(sumX / count), int(sumY / count));
    }

    // Share the Tank core with Squad movement. Even a small Goliath cluster
    // must not inherit one detached Tank when the rest of its squad has a line.
    const int partition = cluster.center.isValid()
        ? the.partitions.id(cluster.center) : 0;
    const TerranFormation::TankCore core =
        TerranFormation::dominantTankCore(
            formationUnits, cluster.center, partition);
    if (core.center.isValid())
    {
        return cluster.center.isValid() &&
            cluster.center.getApproxDistance(core.center) <= 18 * 32
            ? core.center : BWAPI::Positions::Invalid;
    }

    if (count > 0)
    {
        return BWAPI::Position(int(sumX / count), int(sumY / count));
    }

    // A reinforcement group may be a separate spatial cluster even though it
    // is already close enough to the main Tank line to use it as its anchor.
    // Without this bridge, those Goliaths received the raw attack destination
    // and could run through the Tanks before the clusters merged.
    if (!cluster.center.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Unit nearestTank = nullptr;
    int nearestDistance = MAX_DISTANCE;
    if (core.total > 2)
    {
        return BWAPI::Positions::Invalid;
    }
    for (BWAPI::Unit unit : formationUnits)
    {
        if (!isTankForMixedFormation(unit))
        {
            continue;
        }

        const int distance = unit->getDistance(cluster.center);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearestTank = unit;
        }
    }

    return nearestTank && nearestDistance <= 18 * 32
        ? nearestTank->getPosition()
        : BWAPI::Positions::Invalid;
}

static BWAPI::Position getMixedGoliathRearSlot(
    BWAPI::Unit goliath,
    size_t index,
    size_t total,
    const BWAPI::Position& tankCenter,
    const BWAPI::Position& objective)
{
    if (!isGoliathUnit(goliath) ||
        total == 0 ||
        !tankCenter.isValid() ||
        !objective.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const bool heavyMixed =
        StrategyManager::Instance().isTerranHeavyMixed();
    const bool mixed =
        StrategyManager::Instance().getTerranArmyPosture() ==
            TerranArmyPosture::Mixed;
    const int rearGap = heavyMixed
        ? TerranFormation::HeavyMixedRearGap
        : mixed
        ? TerranFormation::MixedRearGap
        : TerranFormation::GroundRearGap;
    const BWAPI::Position anchor = TerranFormation::tankRearAnchor(tankCenter, objective,
        rearGap);
    return TerranFormation::goliathSlot(goliath, int(index), int(total), anchor, objective);
}

// Return a terrain waypoint only when the objective lies across an explicit
// Area/Choke boundary. Never build a GridDistances field whose destination is
// a moving Carrier/Mutalisk: that produced a full-map cache miss whenever the
// target entered a new tile. The Area graph gives us a stable choke waypoint;
// cache it briefly per Area pair so a large Goliath squad shares one route.
static BWAPI::Position getGoliathChokeWaypoint(
    BWAPI::Unit goliath,
    const BWAPI::Position& objective)
{
    if (!isGoliathUnit(goliath) ||
        !goliath->getPosition().isValid() ||
        !objective.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position current = goliath->getPosition();
    if (current.getApproxDistance(objective) <= 5 * 32)
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::TilePosition currentTile(current);
    const BWAPI::TilePosition objectiveTile(objective);
    if (!currentTile.isValid() || !objectiveTile.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const int currentPartition = the.partitions.id(currentTile);
    const int objectivePartition = the.partitions.id(objectiveTile);
    if (currentPartition <= 0 || currentPartition != objectivePartition)
    {
        return BWAPI::Positions::Invalid;
    }

    const Zone* currentArea = the.zone.areaAt(current);
    const Zone* objectiveArea = the.zone.areaAt(objective);
    if (!currentArea || !objectiveArea || currentArea == objectiveArea)
    {
        return BWAPI::Positions::Invalid;
    }

    struct CachedChokeWaypoint
    {
        BWAPI::Position position;
        int frame;
    };

    static std::map<std::pair<int, int>, CachedChokeWaypoint> cache;
    static int lastFrame = -1;

    const int now = BWAPI::Broodwar->getFrameCount();
    if (now < lastFrame)
    {
        cache.clear();
    }
    lastFrame = now;

    const std::pair<int, int> key(currentArea->id(), objectiveArea->id());
    BWAPI::Position waypoint = BWAPI::Positions::Invalid;

    const auto found = cache.find(key);
    if (found != cache.end() && now - found->second.frame <= 2 * 24)
    {
        waypoint = found->second.position;
    }
    else
    {
        const ChokePoint* choke = the.zone.findFirstChoke(current, objective);
        if (!choke)
        {
            choke = the.zone.findNearestChoke(current, objective);
        }

        waypoint = choke && choke->center().isValid()
            ? choke->center()
            : BWAPI::Positions::Invalid;

        cache[key] = CachedChokeWaypoint{ waypoint, now };

        if (cache.size() > 64)
        {
            cache.erase(cache.begin());
        }
    }

    if (!waypoint.isValid() || waypoint.getApproxDistance(current) < 24)
    {
        return BWAPI::Positions::Invalid;
    }

    return nearestWalkableGoliathPoint(waypoint, current);
}

static Base* getOwnedBaseForGoliathDefense(const BWAPI::Position& position)
{
    if (!position.isValid())
    {
        return nullptr;
    }

    Base* best = nullptr;
    int bestDistance = MAX_DISTANCE;

    for (Base* base : the.bases.getAll())
    {
        if (!base ||
            base->getOwner() != the.self() ||
            !base->getDepot() ||
            !base->getCenter().isValid())
        {
            continue;
        }

        const int distance = base->getCenter().getApproxDistance(position);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = base;
        }
    }

    return bestDistance <= 8 * 32 ? best : nullptr;
}

// Defensive Goliaths form the mobile screen near the actual entrance. Tanks are
// intentionally deeper (their terrain defense line is ~5 tiles inside), while
// Goliaths wait ~2.5 tiles inside so they can intercept air/ground pressure
// without clogging the bunker/choke center.
static BWAPI::Position getGoliathDefenseScreenPosition(
    const BWAPI::Position& attackedBasePosition)
{
    Base* base = getOwnedBaseForGoliathDefense(attackedBasePosition);
    if (!base)
    {
        return attackedBasePosition;
    }

    const BWAPI::Position center = base->getCenter();
    const ChokePoint* choke = base->getFrontChoke();

    if (choke && choke->center().isValid() && center.isValid())
    {
        BWAPI::Position screen = goliathFormationAdvancePosition(
            choke->center(),
            center,
            80);
        screen = nearestWalkableGoliathPoint(screen, center);
        if (screen.isValid())
        {
            return screen;
        }
    }

    const BWAPI::Position front = base->getFront();
    if (front.isValid() && front != BWAPI::Positions::Unknown)
    {
        return nearestWalkableGoliathPoint(front, center);
    }

    return attackedBasePosition;
}

static bool handleGoliathDefendAttackedBase(
    BWAPI::Unit goliath,
    const BWAPI::Position& attackedBasePosition,
    const BWAPI::Unitset& targets)
{
    if (!goliath ||
        !goliath->exists() ||
        !goliath->getPosition().isValid() ||
        !attackedBasePosition.isValid())
    {
        return false;
    }

    const BWAPI::Position defenseScreen =
        getGoliathDefenseScreenPosition(attackedBasePosition);

    BWAPI::Unit target =
        getBestEnemyNearGoliathDefendedBase(
            goliath,
            attackedBasePosition,
            targets
        );

    if (Config::Debug::DrawUnitTargets)
    {
        BWAPI::Broodwar->drawCircleMap(
            defenseScreen.isValid() ? defenseScreen : attackedBasePosition,
            12 * 32,
            BWAPI::Colors::Red,
            false
        );
    }

    if (target)
    {
        if (target->getType() == BWAPI::UnitTypes::Protoss_Carrier &&
            !goliathCanConfidentlyFightCarrierNearBase(
                goliath,
                target,
                attackedBasePosition) &&
            !goliath->isInWeaponRange(target))
        {
            // The base is under attack, so do not retreat away from it. Group
            // at the defended base until enough Goliaths arrive; if the
            // Carrier enters weapon range, the condition above immediately
            // allows stand-and-fire.
            const BWAPI::Position groupPosition =
                defenseScreen.isValid() ? defenseScreen : attackedBasePosition;

            if (goliath->getDistance(groupPosition) > 3 * 32)
            {
                the.micro.MoveNear(goliath, groupPosition);
            }
            else
            {
                the.micro.HoldPosition(goliath);
            }

            DrawUnitTargetTextMap(
                goliath->getPosition() + BWAPI::Position(0, -12),
                "base carrier group"
            );

            return true;
        }

        if (Config::Debug::DrawUnitTargets)
        {
            BWAPI::Broodwar->drawLineMap(
                goliath->getPosition(),
                target->getPosition(),
                BWAPI::Colors::Orange
            );
        }

        the.micro.AttackUnit(goliath, target);
        return true;
    }

    const BWAPI::Position defenseDestination =
        defenseScreen.isValid() ? defenseScreen : attackedBasePosition;

    if (goliath->getDistance(defenseDestination) > 96)
    {
        if (Config::Debug::DrawUnitTargets)
        {
            BWAPI::Broodwar->drawLineMap(
                goliath->getPosition(),
                defenseDestination,
                BWAPI::Colors::Yellow
            );
        }

        the.micro.MoveNear(goliath, defenseDestination);
        return true;
    }

    the.micro.HoldPosition(goliath);
    return true;
}



MicroGoliath::MicroGoliath()
    : _formationUnits(nullptr)
{
}

void MicroGoliath::executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster)
{
    BWAPI::Unitset units = Intersection(getUnits(), cluster.units);
    if (units.empty())
    {
        return;
    }

    assignTargets(units, targets, cluster);
}

void MicroGoliath::assignTargets(
    const BWAPI::Unitset & goliathUnits,
    const BWAPI::Unitset & targets,
    const UnitCluster& cluster)
{
    BWAPI::Unitset goliathTargets;
    std::copy_if(targets.begin(), targets.end(), std::inserter(goliathTargets, goliathTargets.end()),
        [=](BWAPI::Unit u) {
            return
                u &&
                u->exists() &&
                u->isVisible() &&
                u->isDetected() &&
                !u->isInvincible() &&
                u->getPosition().isValid() &&
                u->getType() != BWAPI::UnitTypes::Zerg_Larva;
        });

    bool enemyHasAntiGround = false;
    bool enemyHasAntiAir = false;
    for (BWAPI::Unit target : goliathTargets)
    {
        if (UnitUtil::AttackOrder(target))
        {
            if (UnitUtil::CanAttackGround(target))
            {
                enemyHasAntiGround = true;
            }
            if (UnitUtil::CanAttackAir(target))
            {
                enemyHasAntiAir = true;
            }
        }
    }

    const std::vector<BWAPI::Unit> tankCoveredBuildings =
        TerranSiegeSafety::coveredBuildings(goliathTargets);
    for (BWAPI::Unit building : tankCoveredBuildings)
    {
        goliathTargets.erase(building);
    }

    bool underThreat = order->isCombatOrder() && anyUnderThreat(goliathUnits);

    BWAPI::Position attackedBasePosition =
        getCurrentlyAttackedOwnedBasePositionForGoliath();

    const TerranArmyPosture armyPosture =
        StrategyManager::Instance().getTerranArmyPosture();
    const bool pureAirMode =
        armyPosture == TerranArmyPosture::PureAir;
    const bool lightMixedMode =
        StrategyManager::Instance().isTerranLightMixed();
    const bool forcePush = goliathForcePushAtMaxSupply();

    const BWAPI::Position mixedTankCenter =
        getMixedFormationTankCenter(
            cluster, _formationUnits ? *_formationUnits : cluster.units);
    const bool mixedFormation =
        armyPosture != TerranArmyPosture::PureAir &&
        !lightMixedMode &&
        !TerranFormation::isBaseDefenseOrder(*order) &&
        mixedTankCenter.isValid() &&
        order->getPosition().isValid() &&
        (order->getType() == SquadOrderTypes::Attack ||
         order->getType() == SquadOrderTypes::OmniAttack);

    std::vector<BWAPI::Unit> orderedGoliaths;
    orderedGoliaths.reserve(goliathUnits.size());
    for (BWAPI::Unit unit : goliathUnits)
    {
        if (isGoliathUnit(unit))
        {
            orderedGoliaths.push_back(unit);
        }
    }
    std::sort(
        orderedGoliaths.begin(),
        orderedGoliaths.end(),
        [](BWAPI::Unit a, BWAPI::Unit b)
        {
            return a->getID() < b->getID();
        });
    if (orderedGoliaths.empty())
    {
        return;
    }

    std::vector<BWAPI::Unit> formationGoliaths = orderedGoliaths;
    if (mixedFormation)
    {
        for (BWAPI::Unit unit : the.self()->getUnits())
            if (isGoliathUnit(unit) && unit->isCompleted() &&
                unit->getDistance(mixedTankCenter) <= 18 * 32 &&
                std::find(formationGoliaths.begin(), formationGoliaths.end(), unit) == formationGoliaths.end())
                formationGoliaths.push_back(unit);
        std::sort(formationGoliaths.begin(), formationGoliaths.end(),
            [](BWAPI::Unit a, BWAPI::Unit b) { return a->getID() < b->getID(); });
    }

    std::vector<BWAPI::Unit> carrierTargets;
    for (BWAPI::Unit target : goliathTargets)
    {
        if (target && target->exists() && target->isVisible() &&
            target->isDetected() &&
            target->getType() == BWAPI::UnitTypes::Protoss_Carrier)
        {
            carrierTargets.push_back(target);
        }
    }
    std::sort(
        carrierTargets.begin(),
        carrierTargets.end(),
        [](BWAPI::Unit a, BWAPI::Unit b)
        {
            if (a->isLockedDown() != b->isLockedDown())
            {
                return a->isLockedDown();
            }

            const int aHealth = a->getHitPoints() + a->getShields();
            const int bHealth = b->getHitPoints() + b->getShields();
            return aHealth != bHealth
                ? aHealth < bHealth
                : a->getID() < b->getID();
        });


    BWAPI::Position pureAirObjective = BWAPI::Positions::Invalid;

    if (pureAirMode)
    {
        // Prefer the current visible Carrier line as the front direction. For
        // non-Carrier PureAir (Mutalisks/Wraiths), the squad combat order is the
        // stable objective until a local target becomes available.
        if (!carrierTargets.empty() &&
            carrierTargets.front() &&
            carrierTargets.front()->exists() &&
            carrierTargets.front()->getPosition().isValid())
        {
            pureAirObjective = carrierTargets.front()->getPosition();
        }
        else if (order->getPosition().isValid())
        {
            pureAirObjective = order->getPosition();
        }
        else if (cluster.center.isValid())
        {
            pureAirObjective = cluster.center;
        }
    }

    const BWAPI::Position pureAirGroundObjective =
        pureAirMode && pureAirObjective.isValid()
        ? getReachableGroundObjectiveForGoliath(
            // The arithmetic center can lie inside a cliff or a building,
            // yielding no ground partition even though every Goliath is on
            // walkable ground. Use an actual unit as the route reference.
            orderedGoliaths.front()->getPosition(),
            pureAirObjective,
            order->getPosition())
        : pureAirObjective;

    // SquadOrder already owns a GridDistances map for ground attack orders.
    // Reuse it whenever the PureAir objective is the strategic base (or a
    // Carrier hovering near that base). This gives each Goliath a waypoint
    // through ramps/chokes instead of advancing in a straight air-distance
    // line into cliffs.
    const GridDistances* pureAirRouteDistances = nullptr;
    if (pureAirMode &&
        order->distances() &&
        order->getPosition().isValid() &&
        pureAirGroundObjective.isValid() &&
        pureAirGroundObjective.getApproxDistance(order->getPosition()) <= 12 * 32)
    {
        pureAirRouteDistances = order->distances();
    }

    const BWAPI::Position pureAirFrontCenter =
        pureAirMode
        ? getPureAirGoliathFrontCenter(
            orderedGoliaths,
            pureAirGroundObjective,
            pureAirRouteDistances)
        : BWAPI::Positions::Invalid;

    for (size_t goliathIndex = 0;
        goliathIndex < orderedGoliaths.size();
        ++goliathIndex)
    {
        BWAPI::Unit goliath = orderedGoliaths[goliathIndex];
        if (!goliath || !goliath->exists() || !goliath->getPosition().isValid())
        {
            continue;
        }

        if (goliath->isBurrowed())
        {
            continue;
        }

        if (the.micro.fleeDT(goliath))
        {
            continue;
        }

        BWAPI::Unit coveredBuilding = nullptr;
        if (order->isCombatOrder())
        {
            coveredBuilding = TerranSiegeSafety::nearestBuilding(
                goliath, tankCoveredBuildings);
            if (coveredBuilding &&
                goliath->getDistance(coveredBuilding) < 3 * 32)
            {
                const BWAPI::Position retreat =
                    TerranSiegeSafety::standoff(goliath, coveredBuilding);
                if (retreat.isValid() &&
                    the.partitions.connected(goliath->getPosition(), retreat))
                    the.micro.MoveNear(goliath, retreat);
                else
                    the.micro.HoldPosition(goliath);
                continue;
            }
        }

        const BWAPI::Position mixedRearSlot = mixedFormation
            ? getMixedGoliathRearSlot(
                goliath,
                size_t(std::distance(formationGoliaths.begin(),
                    std::find(formationGoliaths.begin(), formationGoliaths.end(), goliath))),
                formationGoliaths.size(),
                mixedTankCenter,
                order->getPosition())
            : BWAPI::Positions::Invalid;

        if (order->isCombatOrder())
        {
            // In PureAir the whole AntiAir squad already receives the attacked
            // base as its order from CombatCommander. Running the separate
            // per-Goliath base/Carrier controller here caused two systems to
            // overwrite each other's commands.
            if (!pureAirMode && !coveredBuilding &&
                attackedBasePosition.isValid() &&
                handleGoliathDefendAttackedBase(
                    goliath,
                    attackedBasePosition,
                    goliathTargets))
            {
                continue;
            }

            // True 180+ force push. If this Goliath can see an attackable
            // target, do not let choke staging, formation movement or ranged
            // kiting overwrite the attack. Buildings are especially important
            // here: a maxed Goliath army must walk into one Sunken/bunker/
            // cannon and kill it instead of forming outside its range forever.
            if (forcePush)
            {
                BWAPI::Unit forceTarget = getTarget(goliath, goliathTargets, true);
                if (forceTarget)
                {
                    if (coveredBuilding &&
                        !goliath->isInWeaponRange(forceTarget) &&
                        !forceTarget->isFlying() &&
                        forceTarget->getDistance(coveredBuilding) < 3 * 32)
                    {
                        the.micro.HoldPosition(goliath);
                        continue;
                    }
                    const BWAPI::TilePosition hereTile(goliath->getPosition());
                    const BWAPI::TilePosition targetTile(forceTarget->getPosition());
                    const bool sameGroundPartition =
                        hereTile.isValid() &&
                        targetTile.isValid() &&
                        the.partitions.id(hereTile) > 0 &&
                        the.partitions.id(hereTile) == the.partitions.id(targetTile);

                    if (sameGroundPartition || goliath->isInWeaponRange(forceTarget))
                    {
                        the.micro.CatchAndAttackUnit(goliath, forceTarget);
                        continue;
                    }

                    // Only preserve terrain routing when the target is truly
                    // across a different ground region. This avoids straight
                    // commands into cliffs while never using a choke waypoint
                    // as a reason to fear nearby static defense.
                    const BWAPI::Position groundObjective =
                        getReachableGroundObjectiveForGoliath(
                            goliath->getPosition(),
                            forceTarget->getPosition(),
                            order->getPosition());
                    const BWAPI::Position chokeWaypoint =
                        getGoliathChokeWaypoint(goliath, groundObjective);

                    if (chokeWaypoint.isValid())
                    {
                        the.micro.MoveNear(goliath, chokeWaypoint);
                    }
                    else
                    {
                        the.micro.CatchAndAttackUnit(goliath, forceTarget);
                    }
                    continue;
                }

                if (coveredBuilding)
                {
                    the.micro.HoldPosition(goliath);
                    continue;
                }

                // No target is currently in the cluster's sight. Keep pushing
                // with attack-move rather than Hold/formation staging so the
                // next static defense or unit is engaged immediately.
                if (!pureAirMode && order->getPosition().isValid())
                {
                    // At max supply the squad is explicitly ordered to push.
                    // Keeping the mixed rear slot here made its Goliaths hold
                    // behind stationary Tanks despite having no local target.
                    the.micro.AttackMove(goliath, order->getPosition());
                    continue;
                }
            }

            BWAPI::Unit immediateGroundThreat =
                getImmediateGroundThreatForGoliath(goliath, goliathTargets);

            BWAPI::Unit preferredCarrier = nullptr;
            if (!carrierTargets.empty())
            {
                // Six Goliaths per Carrier give focused volleys without making
                // the entire army overkill one target while the rest fire.
                const size_t carrierIndex =
                    (goliathIndex / 6) % carrierTargets.size();
                preferredCarrier = carrierTargets[carrierIndex];
            }

            BWAPI::Unit antiAirTarget =
                getBestVisibleAntiAirTargetForGoliath(
                    goliath,
                    preferredCarrier);

            // When a Reaver is being picked up after each shot, shooting its
            // temporary ground position loses the next volley. Destroy the
            // Shuttle while it is in range; still handle other ground units
            // already attacking the Goliath line first.
            if (antiAirTarget && goliath->isInWeaponRange(antiAirTarget) &&
                (antiAirTarget->getType() == BWAPI::UnitTypes::Protoss_Shuttle ||
                 antiAirTarget->getType() == BWAPI::UnitTypes::Terran_Dropship) &&
                (!immediateGroundThreat ||
                 immediateGroundThreat->getType() == BWAPI::UnitTypes::Protoss_Reaver))
            {
                the.micro.AttackUnit(goliath, antiAirTarget);
                continue;
            }

            if (immediateGroundThreat)
            {
                if (coveredBuilding &&
                    !goliath->isInWeaponRange(immediateGroundThreat) &&
                    immediateGroundThreat->getDistance(coveredBuilding) < 3 * 32)
                {
                    the.micro.HoldPosition(goliath);
                    continue;
                }
                if (goliath->isInWeaponRange(immediateGroundThreat))
                {
                    the.micro.AttackUnit(goliath, immediateGroundThreat);
                }
                else
                {
                    the.micro.CatchAndAttackUnit(goliath, immediateGroundThreat);
                }

                continue;
            }

            if (antiAirTarget)
            {
                if (goliath->isInWeaponRange(antiAirTarget))
                {
                    the.micro.AttackUnit(goliath, antiAirTarget);
                }
                else
                {
                    // Mixed Goliaths may step forward to intercept air around
                    // the Tank line, but may not chase one raider across the
                    // map and expose the Tanks to the rest of the air army.
                    const bool airOutsideTankScreen =
                        mixedFormation &&
                        antiAirTarget->getPosition().isValid() &&
                        antiAirTarget->getPosition().getApproxDistance(
                            mixedTankCenter) > 13 * 32;
                    const bool orderedLiftedBuilding =
                        antiAirTarget->getType().isBuilding() &&
                        antiAirTarget->isFlying() &&
                        order->getKey() == "air target approach" &&
                        antiAirTarget->getDistance(order->getPosition()) <= 12 * 32;

                    int nearbyGoliaths = 0;
                    for (BWAPI::Unit ally : orderedGoliaths)
                    {
                        if (ally && ally->exists() &&
                            ally->getDistance(antiAirTarget) <= 10 * 32)
                        {
                            ++nearbyGoliaths;
                        }
                    }
                    bool groundThreatNearAir = false;
                    for (BWAPI::Unit enemy : goliathTargets)
                    {
                        if (enemy && enemy->exists() && enemy->isVisible() &&
                            !enemy->isFlying() && UnitUtil::CanAttackGround(enemy) &&
                            enemy->getDistance(antiAirTarget) <= 7 * 32)
                        {
                            groundThreatNearAir = true;
                            break;
                        }
                    }
                    const bool localAirIntercept =
                        airOutsideTankScreen &&
                        nearbyGoliaths >= 4 &&
                        !groundThreatNearAir &&
                        antiAirTarget->getType() != BWAPI::UnitTypes::Protoss_Carrier &&
                        antiAirTarget->getDistance(mixedTankCenter) <= 18 * 32 &&
                        goliath->getDistance(antiAirTarget) <= 12 * 32;

                    if (airOutsideTankScreen && !localAirIntercept &&
                        !orderedLiftedBuilding &&
                        mixedRearSlot.isValid())
                    {
                        if (goliath->getPosition().getApproxDistance(mixedRearSlot) > 16)
                        {
                            the.micro.MoveNear(goliath, mixedRearSlot);
                        }
                        else
                        {
                            the.micro.HoldPosition(goliath);
                        }
                        continue;
                    }

                    const BWAPI::Position groundObjective =
                        getReachableGroundObjectiveForGoliath(
                            goliath->getPosition(),
                            antiAirTarget->getPosition(),
                            order->getPosition());
                    if (groundObjective.getApproxDistance(
                            antiAirTarget->getPosition()) > 8 * 32)
                    {
                        // The visible aircraft is over ground we cannot
                        // reach. Keep the ground route instead of issuing an
                        // attack command into the cliff beneath it.
                        if (groundObjective.getApproxDistance(
                                goliath->getPosition()) > 32)
                            the.micro.MoveNear(goliath, groundObjective,
                                order->distances());
                        else
                            the.micro.HoldPosition(goliath);
                        continue;
                    }
                    const BWAPI::Position chokeWaypoint =
                        getGoliathChokeWaypoint(goliath, groundObjective);

                    if (chokeWaypoint.isValid())
                    {
                        the.micro.MoveNear(goliath, chokeWaypoint);
                    }
                    else
                    {
                        // Same open area: retain the responsive direct chase.
                        the.micro.AttackUnit(goliath, antiAirTarget);
                    }
                }
                continue;
            }

            BWAPI::Unit target =
                pureAirMode && preferredCarrier
                ? preferredCarrier
                : getTarget(goliath, goliathTargets, underThreat);
            if (target)
            {
                if (coveredBuilding && !goliath->isInWeaponRange(target) &&
                    !target->isFlying() &&
                    target->getDistance(coveredBuilding) < 3 * 32)
                {
                    the.micro.HoldPosition(goliath);
                    continue;
                }
                bool kite = target->isFlying() ? enemyHasAntiAir : enemyHasAntiGround;
                if (target->isFlying())
                {
                    if (!pureAirMode || goliath->isInWeaponRange(target))
                    {
                        the.micro.AttackUnit(goliath, target);
                    }
                    else if (target->getPosition().isValid())
                    {
                        // Large PureAir armies must flow toward the fighting
                        // line. The previous code sent every rear Goliath back
                        // to cluster.center, which is deep inside a 40-60 unit
                        // blob and leaves only the first rows actually fighting.
                        const BWAPI::Position localFront =
                            pureAirFrontCenter.isValid()
                            ? pureAirFrontCenter
                            : cluster.center;

                        const BWAPI::Position movementObjective =
                            pureAirGroundObjective.isValid()
                            ? pureAirGroundObjective
                            : getReachableGroundObjectiveForGoliath(
                                goliath->getPosition(),
                                target->getPosition(),
                                order->getPosition());

                        BWAPI::Position destination =
                            getPureAirGoliathAdvanceDestination(
                                goliath,
                                localFront,
                                movementObjective,
                                target->isLockedDown());

                        const BWAPI::Position chokeWaypoint =
                            pureAirRouteDistances
                            ? BWAPI::Positions::Invalid
                            : getGoliathChokeWaypoint(goliath, movementObjective);
                        if (chokeWaypoint.isValid())
                        {
                            destination = chokeWaypoint;
                        }

                        if (destination.isValid())
                        {
                            the.micro.MoveNear(
                                goliath,
                                destination,
                                chokeWaypoint.isValid() ? nullptr : pureAirRouteDistances);
                        }
                        else
                        {
                            the.micro.MoveNear(
                                goliath,
                                movementObjective,
                                pureAirRouteDistances);
                        }
                    }
                    continue;
                }

                if (mixedFormation &&
                    !goliath->isInWeaponRange(target) &&
                    !underThreat &&
                    mixedRearSlot.isValid())
                {
                    // Tanks/Vultures own distant ground contact in Mixed. The
                    // Goliath rear guard fires when contact reaches its range,
                    // but does not run through the Tank line to chase it.
                    if (goliath->getPosition().getApproxDistance(mixedRearSlot) > 16)
                    {
                        the.micro.MoveNear(goliath, mixedRearSlot);
                    }
                    else
                    {
                        the.micro.HoldPosition(goliath);
                    }
                    continue;
                }

                if (!goliath->isInWeaponRange(target))
                {
                    const BWAPI::Position groundObjective =
                        getReachableGroundObjectiveForGoliath(
                            goliath->getPosition(),
                            target->getPosition(),
                            order->getPosition());
                    const BWAPI::Position chokeWaypoint =
                        getGoliathChokeWaypoint(goliath, groundObjective);
                    if (chokeWaypoint.isValid())
                    {
                        the.micro.MoveNear(goliath, chokeWaypoint);
                        continue;
                    }
                }

                if (Config::Micro::KiteWithRangedUnits && kite)
                {
                    the.micro.KiteTarget(goliath, target);
                }
                else
                {
                    the.micro.CatchAndAttackUnit(goliath, target);
                }
            }
            else
            {
                if (coveredBuilding)
                {
                    the.micro.HoldPosition(goliath);
                    continue;
                }
                const bool groundRouteStillAhead =
                    pureAirMode && pureAirRouteDistances &&
                    goliath->getTilePosition().isValid() &&
                    pureAirRouteDistances->at(goliath->getTilePosition()) > 3;
                if (mixedFormation && mixedRearSlot.isValid())
                {
                    if (goliath->getPosition().getApproxDistance(mixedRearSlot) > 16)
                    {
                        the.micro.MoveNear(goliath, mixedRearSlot);
                    }
                    else
                    {
                        the.micro.HoldPosition(goliath);
                    }
                }
                else if (order->getPosition().isValid() &&
                    (goliath->getDistance(order->getPosition()) > 100 ||
                     groundRouteStillAhead))
                {
                    if (pureAirMode)
                    {
                        const BWAPI::Position movementObjective =
                            pureAirGroundObjective.isValid()
                            ? pureAirGroundObjective
                            : order->getPosition();
                        BWAPI::Position destination =
                            getPureAirGoliathAdvanceDestination(
                                goliath,
                                pureAirFrontCenter,
                                movementObjective,
                                false);

                        const BWAPI::Position chokeWaypoint =
                            pureAirRouteDistances
                            ? BWAPI::Positions::Invalid
                            : getGoliathChokeWaypoint(goliath, movementObjective);
                        if (chokeWaypoint.isValid())
                        {
                            destination = chokeWaypoint;
                        }

                        if (destination.isValid())
                        {
                            the.micro.MoveNear(
                                goliath,
                                destination,
                                chokeWaypoint.isValid() ? nullptr : pureAirRouteDistances);
                        }
                        else
                        {
                            the.micro.MoveNear(
                                goliath,
                                movementObjective,
                                order->distances());
                        }
                    }
                    else
                    {
                        const BWAPI::Position chokeWaypoint =
                            getGoliathChokeWaypoint(goliath, order->getPosition());
                        if (chokeWaypoint.isValid())
                        {
                            the.micro.MoveNear(goliath, chokeWaypoint);
                        }
                        else
                        {
                            the.micro.MoveNear(
                                goliath,
                                order->getPosition(),
                                order->distances());
                        }
                    }
                }
                else
                {
                    the.micro.HoldPosition(goliath);
                }
            }
        }
        else if (order->getPosition().isValid())
        {
            if (goliath->getDistance(order->getPosition()) > 100)
            {
                const BWAPI::Position movementObjective =
                    pureAirMode && pureAirGroundObjective.isValid()
                    ? pureAirGroundObjective
                    : order->getPosition();

                const BWAPI::Position chokeWaypoint =
                    pureAirMode && pureAirRouteDistances
                    ? BWAPI::Positions::Invalid
                    : getGoliathChokeWaypoint(goliath, movementObjective);

                if (pureAirMode)
                {
                    BWAPI::Position destination =
                        getPureAirGoliathAdvanceDestination(
                            goliath,
                            pureAirFrontCenter,
                            movementObjective,
                            false);

                    if (chokeWaypoint.isValid())
                    {
                        destination = chokeWaypoint;
                    }

                    if (destination.isValid())
                    {
                        the.micro.MoveNear(
                            goliath,
                            destination,
                            chokeWaypoint.isValid() ? nullptr : pureAirRouteDistances);
                    }
                    else
                    {
                        the.micro.MoveNear(
                            goliath,
                            movementObjective,
                            order->distances());
                    }
                }
                else if (chokeWaypoint.isValid())
                {
                    the.micro.MoveNear(goliath, chokeWaypoint);
                }
                else
                {
                    the.micro.MoveNear(
                        goliath,
                        movementObjective,
                        order->distances());
                }
            }
            else
            {
                the.micro.HoldPosition(goliath);
            }
        }
    }
}

BWAPI::Unit MicroGoliath::getTarget(BWAPI::Unit goliath, const BWAPI::Unitset& targets, bool underThreat) const
{
    int bestScore = -999999;
    BWAPI::Unit bestTarget = nullptr;

    for (BWAPI::Unit target : targets)
    {
        if (!target || !target->exists() || !target->isVisible() ||
            !target->isDetected())
        {
            continue;
        }

        if (!target->getPosition().isValid())
        {
            continue;
        }

        if (!goliathCanAttackTarget(goliath, target))
        {
            continue;
        }

        if (target->isUnderDarkSwarm() &&
            !target->getType().isBuilding())
        {
            continue;
        }

        const int range = goliath->getDistance(target);

        if (range >= 13 * 32)
        {
            continue;
        }

        if (target->getType() == BWAPI::UnitTypes::Protoss_Interceptor &&
            goliathShouldIgnoreInterceptor(goliath, target))
        {
            continue;
        }

        int score = 0;

        score += getAttackPriority(goliath, target) * 5 * 32;
        score += goliathTargetTypePriority(target) * 32;

        score -= range;

        if (order && order->getPosition().isValid())
        {
            const int closerToGoal =
                goliath->getDistance(order->getPosition()) -
                target->getDistance(order->getPosition());

            if (closerToGoal > 0)
            {
                score += 2 * 32;
            }
        }

        if (!underThreat)
        {
            if (!the.groundHitsFixed.inRange(target))
            {
                score += 4 * 32;
            }
        }

        const bool isThreat = UnitUtil::CanAttack(target, goliath);
        const bool canShootBack = isThreat && range <= 32 + UnitUtil::GetAttackRange(target, goliath);

        if (isThreat)
        {
            if (canShootBack)
            {
                score += 7 * 32;
            }
            else if (goliath->isInWeaponRange(target))
            {
                score += 5 * 32;
            }
            else
            {
                score += 4 * 32;
            }
        }
        else if (!target->isMoving())
        {
            if (target->isSieged() ||
                target->getOrder() == BWAPI::Orders::Sieging ||
                target->getOrder() == BWAPI::Orders::Unsieging ||
                target->isBurrowed())
            {
                score += 48;
            }
            else
            {
                score += 24;
            }
        }

        if (target->getType().getRace() == BWAPI::Races::Protoss && target->getShields() <= 5)
        {
            score += 32;
        }

        if (target->getHitPoints() < target->getType().maxHitPoints())
        {
            score += 24;
        }

        if (target->isFlying())
        {
            score += 10 * 32;
        }

        if (target->getType() == BWAPI::UnitTypes::Protoss_Carrier)
        {
            score += 20 * 32;
        }

        const int weakness = goliathWeaknessScore(target);
        score -= weakness / 4;

        if (score > bestScore)
        {
            bestScore = score;
            bestTarget = target;
        }
    }

    if (bestTarget)
    {
        DrawUnitTargetTextMap(
            bestTarget->getPosition() + BWAPI::Position(0, 12),
            "goliath focus hp=%d sh=%d armor=%d",
            bestTarget->getHitPoints(),
            bestTarget->getShields(),
            bestTarget->getType().armor()
        );
    }

    return bestScore > 0 ? preferThreatToStructure(goliath, bestTarget, targets) : nullptr;
}

int MicroGoliath::getAttackPriority(BWAPI::Unit goliath, BWAPI::Unit target) const
{
    if (!goliath || !goliath->exists() || !target || !target->exists())
    {
        return 0;
    }

    if (isNeutralEgg(target)) return getBackstopAttackPriority(target);

    if (target->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
    {
        return 140;
    }

    if (target->getType() == BWAPI::UnitTypes::Protoss_Carrier)
    {
        return 120;
    }

    if (target->getType() == BWAPI::UnitTypes::Protoss_Interceptor)
    {
        if (goliathShouldIgnoreInterceptor(goliath, target))
        {
            return 60;
        }

        return 35;
    }

    if (target->getType() == BWAPI::UnitTypes::Protoss_Arbiter)
    {
        return 110;
    }

    if (target->getType() == BWAPI::UnitTypes::Zerg_Queen)
    {
        // Spawn Broodlings can delete a Tank instantly.
        return 108;
    }

    if (target->getType() == BWAPI::UnitTypes::Protoss_High_Templar ||
        target->getType() == BWAPI::UnitTypes::Protoss_Dark_Archon ||
        target->getType() == BWAPI::UnitTypes::Zerg_Defiler)
    {
        return 105;
    }

    if (target->getType() == BWAPI::UnitTypes::Protoss_Reaver ||
        target->getType() == BWAPI::UnitTypes::Zerg_Lurker)
    {
        return 100;
    }

    if (target->getType() == BWAPI::UnitTypes::Protoss_Scout ||
        target->getType() == BWAPI::UnitTypes::Terran_Battlecruiser ||
        target->getType() == BWAPI::UnitTypes::Zerg_Guardian ||
        target->getType() == BWAPI::UnitTypes::Zerg_Devourer)
    {
        return 100;
    }

    if (target->getType() == BWAPI::UnitTypes::Zerg_Mutalisk ||
        target->getType() == BWAPI::UnitTypes::Terran_Wraith ||
        target->getType() == BWAPI::UnitTypes::Terran_Valkyrie)
    {
        return 95;
    }

    if (target->isFlying())
    {
        return 90;
    }

    if (isImmediateLightGroundThreatForGoliath(target))
    {
        return 80;
    }

    if (UnitUtil::CanAttack(target, goliath))
    {
        return 65;
    }

    return target->getType().isBuilding() ? 15 : 35;
}

bool MicroGoliath::goliathShouldIgnoreInterceptor(BWAPI::Unit goliath, BWAPI::Unit interceptor)
{
    if (!goliath || !interceptor)
    {
        return false;
    }

    if (goliath->getType() != BWAPI::UnitTypes::Terran_Goliath)
    {
        return false;
    }

    if (interceptor->getType() != BWAPI::UnitTypes::Protoss_Interceptor)
    {
        return false;
    }

    if (StrategyManager::Instance().getTerranArmyPosture() ==
        TerranArmyPosture::PureAir)
    {
        return false;
    }

    const int carrierNearbyRadius = 8 * 32;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
        goliath->getPosition(),
        carrierNearbyRadius,
        BWAPI::Filter::IsEnemy &&
        BWAPI::Filter::IsFlyer))
    {
        if (!enemy || !enemy->exists())
        {
            continue;
        }

        if (enemy->getType() == BWAPI::UnitTypes::Protoss_Carrier)
        {
            return true;
        }
    }

    return false;
}
