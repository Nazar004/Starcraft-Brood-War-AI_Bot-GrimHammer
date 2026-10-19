#include "Combat/Micro/MicroRanged.h"
#include "Combat/Micro/Terran/MicroMarines.h"
#include "Combat/Micro/Terran/BioInfantrySafety.h"
#include "Strategy/StrategyManager.h"
#include "Map/Bases.h"
#include "Intelligence/InformationManager.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"
#include "Economy/WorkerManager.h"

#include <algorithm>
#include <climits>
#include <vector>

using namespace UAlbertaBot;

MicroRanged::MicroRanged()
{
}

// The Barracks can see an enemy outside a closed wall while the Marines behind
// it are still in Advance (or are retreating toward the main). Give them a
// reachable firing position on our side, including around the outer Depots.
void MicroRanged::defendTerranWall(const BWAPI::Unitset& availableUnits)
{
    if (the.selfRace() != BWAPI::Races::Terran ||
        !the.placer.hasTerranMainWall() ||
        the.placer.isTerranMainWallReleased())
    {
        return;
    }

    const BWAPI::Position gate = the.placer.getTerranMainWallGateCenter();
    if (!gate.isValid()) return;

    BWAPI::Unitset wallBuildings;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit && unit->exists() && !unit->isLifted() &&
            the.placer.isTerranMainWallBuilding(unit))
            wallBuildings.insert(unit);
    }
    if (wallBuildings.empty()) return;

    BWAPI::Unitset threats;
    for (BWAPI::Unit enemy : the.enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            !enemy->isDetected() || enemy->isFlying() ||
            enemy->getType().isBuilding() ||
            !enemy->getPosition().isValid() ||
            enemy->getType().groundWeapon() == BWAPI::WeaponTypes::None)
        {
            continue;
        }

        for (BWAPI::Unit building : wallBuildings)
        {
            if (enemy->getDistance(building) <= 4 * 32)
            {
                threats.insert(enemy);
                break;
            }
        }
    }
    if (threats.empty()) return;

    const BWAPI::UnitType body = BWAPI::UnitTypes::Terran_Marine;
    const int mapWidth = BWAPI::Broodwar->mapWidth() * 32;
    const int mapHeight = BWAPI::Broodwar->mapHeight() * 32;
    std::vector<BWAPI::Position> assignedSlots;
    for (BWAPI::Unit marine : availableUnits)
    {
        if (!marine || !marine->exists() || !marine->isCompleted() ||
            marine->isLoaded() || !marine->getPosition().isValid() ||
            marine->getType() != body ||
            marine->getDistance(gate) > 16 * 32 ||
            !the.placer.isInsideTerranMainWall(marine->getPosition(), 64))
        {
            continue;
        }

        if (BioInfantrySafety::evade(marine)) continue;
        BWAPI::Unit target = nullptr;
        int targetScore = INT_MAX;
        for (BWAPI::Unit enemy : threats)
        {
            if (!UnitUtil::CanAttack(marine, enemy)) continue;
            const int score = marine->getDistance(enemy) -
                (marine->isInWeaponRange(enemy) ? 8 * 32 : 0);
            if (score < targetScore)
            {
                targetScore = score;
                target = enemy;
            }
        }
        if (!target) continue;
        if (marine->isInWeaponRange(target))
        {
            the.micro.AttackUnit(marine, target);
            continue;
        }

        const int range = UnitUtil::GetAttackRange(marine, target);
        const BWAPI::Position firingCenter = target->getPosition();
        std::vector<std::pair<int, BWAPI::Position>> firingSlots;
        for (int dx = -5; dx <= 5; ++dx)
        {
            for (int dy = -5; dy <= 5; ++dy)
            {
                const BWAPI::Position candidate(
                    firingCenter.x + dx * 32, firingCenter.y + dy * 32);
                if (!candidate.isValid() ||
                    !the.placer.isInsideTerranMainWall(candidate, -16) ||
                    !the.partitions.connected(marine->getPosition(), candidate))
                {
                    continue;
                }

                const int left = candidate.x - body.dimensionLeft();
                const int right = candidate.x + body.dimensionRight();
                const int top = candidate.y - body.dimensionUp();
                const int bottom = candidate.y + body.dimensionDown();
                if (left < 0 || top < 0 || right >= mapWidth ||
                    bottom >= mapHeight)
                {
                    continue;
                }

                bool clear = true;
                for (int x = left / 8; x <= right / 8 && clear; ++x)
                    for (int y = top / 8; y <= bottom / 8; ++y)
                        if (!BWAPI::Broodwar->isWalkable(x, y))
                        {
                            clear = false;
                            break;
                        }
                if (!clear ||
                    !the.map.isRuntimeWalkable(BWAPI::TilePosition(candidate)))
                {
                    continue;
                }
                for (int x = left / 32; x <= right / 32 && clear; ++x)
                    for (int y = top / 32; y <= bottom / 32; ++y)
                        if (the.placer.isReserved(BWAPI::TilePosition(x, y)))
                        {
                            clear = false;
                            break;
                        }
                for (const BWAPI::Position& slot : assignedSlots)
                    if (candidate.getApproxDistance(slot) < 32)
                        clear = false;
                if (!clear) continue;

                const int distance = candidate.getApproxDistance(target->getPosition());
                // Prefer a reachable firing tile. When the enemy is farther
                // away, stage close to the wall and shoot when it approaches.
                const int score =
                    std::max(0, distance - range + 8) * 8 +
                    marine->getDistance(candidate) +
                    candidate.getApproxDistance(gate) / 4;
                firingSlots.emplace_back(score, candidate);
            }
        }

        std::stable_sort(firingSlots.begin(), firingSlots.end(),
            [](const std::pair<int, BWAPI::Position>& a,
               const std::pair<int, BWAPI::Position>& b) { return a.first < b.first; });
        for (const auto& slot : firingSlots)
        {
            const BWAPI::Position best = slot.second;
            // Static partitions do not include our wall buildings. Verify
            // the firing tile against the current closed gate before moving.
            std::vector<BWAPI::TilePosition> route;
            if (marine->getDistance(best) > 32 &&
                !the.map.getGroundPath(BWAPI::TilePosition(marine->getPosition()),
                    BWAPI::TilePosition(best), route, 64, true)) continue;
            assignedSlots.push_back(best);
            if (marine->getDistance(best) > 20)
                the.micro.MoveNear(marine, best);
            else
                the.micro.HoldPosition(marine);
            break;
        }
    }
}



void MicroRanged::executeMicro(const BWAPI::Unitset& targets,
    const UnitCluster& cluster)
{
    const BWAPI::Unitset units = Intersection(getUnits(), cluster.units);
    if (!units.empty())
    {
        assignTargets(units, targets, cluster);
    }
}

void MicroRanged::assignTargets(const BWAPI::Unitset& rangedUnits,
    const BWAPI::Unitset& targets, const UnitCluster& cluster)
{
    BWAPI::Unitset validTargets;
    for (BWAPI::Unit target : targets)
    {
        if (target && target->exists() &&
            target->getType() != BWAPI::UnitTypes::Zerg_Larva)
        {
            validTargets.insert(target);
        }
    }

    bool enemyHasAntiGround = false;
    bool enemyHasAntiAir = false;
    for (BWAPI::Unit target : validTargets)
    {
        if (UnitUtil::AttackOrder(target))
        {
            enemyHasAntiGround |= UnitUtil::CanAttackGround(target);
            enemyHasAntiAir |= UnitUtil::CanAttackAir(target);
        }
    }
    const bool underThreat = order->isCombatOrder() && anyUnderThreat(rangedUnits);

    // BioTank's Marines must not crowd a structure being shelled by our
    // Siege Tanks. Otherwise one Tank volley can splash its own infantry.
    BWAPI::Unitset marineTargets = validTargets;
    std::vector<BWAPI::Unit> tankCoveredBuildings =
        TerranSiegeSafety::coveredBuildings(validTargets);
    for (BWAPI::Unit building : tankCoveredBuildings)
    {
        marineTargets.erase(building);
    }

    for (BWAPI::Unit unit : rangedUnits)
    {
        if (!unit || !unit->exists() || unit->isBurrowed())
        {
            continue;
        }
        if (the.micro.fleeDT(unit))
        {
            continue;
        }
        if (WorkerManager::Instance().isMobileRepairTarget(unit))
        {
            const BWAPI::Position repair =
                WorkerManager::Instance().getMobileRepairPosition(unit);
            if (repair.isValid())
            {
                if (unit->getDistance(repair) > 48)
                    the.micro.Move(unit, repair);
                else
                    the.micro.HoldPosition(unit);
                continue;
            }
        }
        if (stayHomeUntilReady(unit))
        {
            the.micro.MoveSafely(unit, the.bases.myMain()->getCenter());
            continue;
        }
        if (!order->isCombatOrder())
        {
            continue;
        }
        BWAPI::Unit coveredBuilding = nullptr;
        if (unit->getType() == BWAPI::UnitTypes::Terran_Marine)
        {
            int nearest = 7 * 32 + 1;
            for (BWAPI::Unit building : tankCoveredBuildings)
            {
                const int distance = unit->getDistance(building);
                if (distance < nearest)
                {
                    nearest = distance;
                    coveredBuilding = building;
                }
            }
            if (coveredBuilding && nearest < 3 * 32)
            {
                const BWAPI::Position retreat =
                    MicroMarines::siegeBuildingStandoff(unit, coveredBuilding);
                if (retreat.isValid() &&
                    the.partitions.connected(unit->getPosition(), retreat))
                    the.micro.MoveNear(unit, retreat);
                else
                    the.micro.HoldPosition(unit);
                continue;
            }
        }
        BWAPI::Unit target = getTarget(unit,
            unit->getType() == BWAPI::UnitTypes::Terran_Marine
                ? marineTargets : validTargets,
            underThreat);
        if (coveredBuilding && target && !unit->isInWeaponRange(target) &&
            !target->isFlying() &&
            target->getDistance(coveredBuilding) < 3 * 32)
        {
            // Do not chase a defender into the same shell impact area.
            the.micro.HoldPosition(unit);
            continue;
        }
        if (coveredBuilding && !target)
        {
            // Hold a usable screen outside the blast zone. The normal squad
            // order would otherwise move Marines into the building footprint.
            the.micro.HoldPosition(unit);
            continue;
        }
        if (target)
        {
            if (Config::Debug::DrawUnitTargets)
            {
                BWAPI::Broodwar->drawLineMap(
                    unit->getPosition(), target->getPosition(),
                    BWAPI::Colors::Purple);
            }
            const bool kite = unit->isFlying()
                ? enemyHasAntiAir : enemyHasAntiGround;
            const bool marineCommits =
                unit->getType() == BWAPI::UnitTypes::Terran_Marine &&
                !MicroMarines::shouldKite(unit, target);
            if (Config::Micro::KiteWithRangedUnits && kite &&
                !marineCommits)
            {
                if (unit->getType() == BWAPI::UnitTypes::Terran_Marine &&
                    StrategyManager::Instance().bioTankPlanActive())
                {
                    if (unit->isAttackFrame() || unit->isStartingAttack()) continue;
                    const int latency = BWAPI::Broodwar->getRemainingLatencyFrames();
                    if (unit->getGroundWeaponCooldown() <= latency + 2)
                        the.micro.CatchAndAttackUnit(unit, target);
                    else
                        the.micro.KiteTarget(unit, target);
                }
                else the.micro.KiteTarget(unit, target);
            }
            else
                the.micro.CatchAndAttackUnit(unit, target);
        }
        else if (order->getPosition().isValid() &&
            unit->getDistance(order->getPosition()) > 100)
        {
            the.micro.MoveNear(unit, order->getPosition(), order->distances());
        }
    }
}
BWAPI::Unit MicroRanged::getTarget(BWAPI::Unit rangedUnit, const BWAPI::Unitset& targets, bool underThreat)
{
    int bestScore = INT_MIN;
    BWAPI::Unit bestTarget = nullptr;

    // Special target selection for Goliaths:
    // 1) keep tactical priority: Carrier / dangerous air / dangerous ground
    // 2) inside same priority choose the weakest target:
    //    lower HP + shields + armor
    // 3) then choose the closest target
    for (BWAPI::Unit target : targets)
    {
        if (!target || !target->exists() || !target->isVisible() ||
            !target->isDetected() || !target->getPosition().isValid() ||
            target->isInvincible() ||
            !UnitUtil::CanAttack(rangedUnit, target))
        {
            continue;
        }
// Skip targets under dark swarm that we can't hit.
        if (target->isUnderDarkSwarm() && !target->getType().isBuilding())
        {
            continue;
        }

        const int priority = getAttackPriority(rangedUnit, target);		// 0..12
        const int range = rangedUnit->getDistance(target);				// 0..map diameter in pixels
        const int closerToGoal =										// positive if target is closer than us to the goal
            rangedUnit->getDistance(order->getPosition()) - target->getDistance(order->getPosition());
        
        // Skip targets that are too far away to worry about--outside tank range.
        if (range >= 13 * 32)
        {
            continue;
        }

        // TODO disabled - seems to be wrong, skips targets it should not
        // Don't chase targets that we can't catch.
        //if (!CanCatchUnit(meleeUnit, target))
        //{
        //	continue;
        //}

        // Let's say that 1 priority step is worth 160 pixels (5 tiles).
        // We care about unit-target range and target-order position distance.
        int score = 5 * 32 * priority - range;

        // Adjust for special features.
        // A bonus for attacking enemies that are "in front".
        // It helps reduce distractions from moving toward the goal, the order position.
        if (closerToGoal > 0)
        {
            score += 2 * 32;
        }

        if (!underThreat)
        {
            // We're not under threat. Prefer to attack stuff outside enemy static defense range.
            if (rangedUnit->isFlying() ? !the.airHitsFixed.inRange(target) : !the.groundHitsFixed.inRange(target))
            {
                score += 4 * 32;
            }
        }

        const bool isThreat = UnitUtil::CanAttack(target, rangedUnit);   // may include workers as threats
        const bool canShootBack = isThreat && range <= 32 + UnitUtil::GetAttackRange(target, rangedUnit);

        if (isThreat)
        {
            if (canShootBack)
            {
                score += 7 * 32;
            }
            else
            {
                // Both old branches awarded the same score, so the weapon
                // range test had no effect. Keep the established weighting
                // and remove the misleading condition.
                score += 5 * 32;
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
        else if (target->isBraking())
        {
            score += 16;
        }
        else if (target->getPlayer()->topSpeed(target->getType()) >= rangedUnit->getPlayer()->topSpeed(rangedUnit->getType()))
        {
            score -= 4 * 32;
        }
        
        // Prefer targets that are already hurt.
        if (target->getType().getRace() == BWAPI::Races::Protoss && target->getShields() <= 5)
        {
            score += 32;
        }
        if (target->getHitPoints() < target->getType().maxHitPoints())
        {
            score += 24;
        }

        // Prefer to hit air units that have acid spores on them from devourers.
        if (target->getAcidSporeCount() > 0)
        {
            // Especially if we're a mutalisk with a bounce attack.
            
            {
                score += 8 * target->getAcidSporeCount();
            }
        }

        // Take the damage type into account.
        BWAPI::DamageType damage = UnitUtil::GetWeapon(rangedUnit, target).damageType();
        if (damage == BWAPI::DamageTypes::Explosive)
        {
            if (target->getType().size() == BWAPI::UnitSizeTypes::Large)
            {
                score += 48;
            }
        }
        else if (damage == BWAPI::DamageTypes::Concussive)
        {
            if (target->getType().size() == BWAPI::UnitSizeTypes::Small)
            {
                score += 48;
            }
            else if (target->getType().size() == BWAPI::UnitSizeTypes::Large)
            {
                score -= 48;
            }
        }

        if (score > bestScore)
        {
            bestScore = score;
            bestTarget = target;
        }
    }

    return bestScore > 0 ? preferThreatToStructure(rangedUnit, bestTarget, targets) : nullptr;
}

int MicroRanged::getAttackPriority(BWAPI::Unit rangedUnit, BWAPI::Unit target) 
{
    const BWAPI::UnitType rangedType = rangedUnit->getType();
    const BWAPI::UnitType targetType = target->getType();

    if (isNeutralEgg(target)) return getBackstopAttackPriority(target);

    

    // A carrier should not target an enemy interceptor. It's too hard to hit.
    


    // An addon other than a completed comsat is boring.
    // TODO should also check that it is attached
    if (targetType.isAddon() && !(targetType == BWAPI::UnitTypes::Terran_Comsat_Station && target->isCompleted()))
    {
        return 1;
    }

    // A ghost which is nuking is the highest priority by a mile.
    if (targetType == BWAPI::UnitTypes::Terran_Ghost &&
        (
            target->getOrder() == BWAPI::Orders::NukePaint ||
            target->getOrder() == BWAPI::Orders::NukeTrack
            ))
    {
        return 15;
    }

    // if the target is building something near our base, something is fishy
    BWAPI::Position ourBasePosition = BWAPI::Position(the.bases.myMain()->getPosition());
    if (target->getDistance(ourBasePosition) < 1000) {
        if (target->getType().isWorker() && (target->isConstructing() || target->isRepairing()))
        {
            return 12;
        }
        if (target->getType().isBuilding())
        {
            // This includes proxy buildings, which deserve high priority.
            // But when bases are close together, it can include innocent buildings.
            // We also don't want to disrupt priorities in case of proxy buildings
            // supported by units; we may want to target the units first.
            if (UnitUtil::CanAttackGround(target) || UnitUtil::CanAttackAir(target))
            {
                return 10;
            }
            return 8;
        }
    }
    
    if (rangedType.isFlyer()) {
        // Exceptions if we're a flyer.
        if (targetType == BWAPI::UnitTypes::Zerg_Scourge)
        {
            return 12;
        }
    }
    else
    {
        // Exceptions if we're a ground unit.
        if (targetType == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
        {
            // A detected mine can damage several packed units at once. Kill it
            // before ordinary combat targets, whether it is burrowed or rising.
            return 16;
        }

        if (targetType == BWAPI::UnitTypes::Zerg_Infested_Terran)
        {
            return 12;
        }
    }

    // Wraiths, scouts, and goliaths strongly prefer air targets because they do more damage to air units.
    if (rangedType == BWAPI::UnitTypes::Terran_Wraith)
    {
        if (target->getType().isFlyer())    // air units, not floating buildings
        {
            return 11;
        }
    }

    if (targetType == BWAPI::UnitTypes::Protoss_High_Templar ||
        targetType == BWAPI::UnitTypes::Zerg_Defiler)
    {
        return 12;
    }

    if (targetType == BWAPI::UnitTypes::Protoss_Reaver ||
        targetType == BWAPI::UnitTypes::Protoss_Arbiter ||
        targetType == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        targetType == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
    {
        return 11;
    }

    // Short circuit: Give bunkers a lower priority to reduce bunker obsession.
    if (targetType == BWAPI::UnitTypes::Terran_Bunker)
    {
        return 9;
    }

    // Threats can attack us. Exception: Workers are not threats.
    if (UnitUtil::CanAttack(targetType, rangedType) && !targetType.isWorker())
    {
        // Enemy unit which is far enough outside its range is lower priority than a worker.
        if (rangedUnit->getDistance(target) > 48 + UnitUtil::GetAttackRange(target, rangedUnit))
        {
            return 8;
        }
        return 10;
    }
    // Droppers are as bad as threats. They may be loaded and are often isolated and safer to attack.
    if (targetType == BWAPI::UnitTypes::Terran_Dropship ||
        targetType == BWAPI::UnitTypes::Protoss_Shuttle)
    {
        return 10;
    }
    // Also as bad are other dangerous things.
    if (targetType == BWAPI::UnitTypes::Terran_Science_Vessel ||
        targetType == BWAPI::UnitTypes::Zerg_Scourge)
    {
        return 10;
    }
    if (targetType == BWAPI::UnitTypes::Protoss_Observer)
    {
        // If we have cloaked units, observers are worse than threats.
        if (InformationManager::Instance().weHaveCloakTech())
        {
            return 11;
        }
        // Otherwise, they are equal.
        return 10;
    }
    // Next are workers.
    if (targetType.isWorker()) 
    {
        // Repairing or blocking a choke makes you critical.
        if (target->isRepairing() || unitNearChokepoint(target))
        {
            return 11;
        }
        // SCVs constructing are also important.
        if (target->isConstructing())
        {
            return 10;
        }

        return 9;
    }

    // Important combat units that we may not have targeted above.
    if (targetType == BWAPI::UnitTypes::Protoss_Carrier)
    {
        return 8;
    }

    return getBackstopAttackPriority(target);
}

// Should the unit stay (or return) home until ready to move out?
bool MicroRanged::stayHomeUntilReady(const BWAPI::Unit u) const
{
    return
        u->getType() == BWAPI::UnitTypes::Protoss_Carrier && u->getInterceptorCount() < 4;
}
