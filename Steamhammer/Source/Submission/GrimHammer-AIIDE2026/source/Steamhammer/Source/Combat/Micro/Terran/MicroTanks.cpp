#include "MicroTanks.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <climits>
#include <chrono>
#include <utility>

#include "Strategy/StrategyManager.h"
#include "Strategy/Terran/AirTanksPolicy.h"

#include "Core/The.h"
#include "Utilities/UnitUtil.h"
#include "Map/Bases.h"
#include "Combat/CombatCommander.h"
#include "Production/ProductionManager.h"
#include "Construction/BuildingManager.h"
#include "Map/MapGrid.h"
#include "Economy/WorkerManager.h"
#include <map>
#include <set>


using namespace UAlbertaBot;

namespace
{
    struct TankPlanWork
    {
        int frame = -1;
        double defenseMs = 0.0;
        double gatherMs = 0.0;
        int defenseBuilds = 0;
        int gatherBuilds = 0;
    };

    TankPlanWork& tankPlanWork()
    {
        static TankPlanWork work;
        const int frame = BWAPI::Broodwar->getFrameCount();
        if (work.frame != frame)
        {
            work = TankPlanWork();
            work.frame = frame;
        }
        return work;
    }

    struct ScopedTankPlanWork
    {
        double& total;
        int& builds;
        std::chrono::steady_clock::time_point start;

        ScopedTankPlanWork(double& ms, int& count)
            : total(ms), builds(count), start(std::chrono::steady_clock::now())
        {
            ++builds;
        }
        ~ScopedTankPlanWork()
        {
            total += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
        }
    };
}



static int distPointToSegmentSq(
    const BWAPI::Position& p,
    const BWAPI::Position& a,
    const BWAPI::Position& b)
{
    const double px = double(p.x);
    const double py = double(p.y);
    const double ax = double(a.x);
    const double ay = double(a.y);
    const double bx = double(b.x);
    const double by = double(b.y);

    const double dx = bx - ax;
    const double dy = by - ay;
    const double lenSq = dx * dx + dy * dy;

    if (lenSq < 1.0)
    {
        const double ex = px - ax;
        const double ey = py - ay;
        return int(ex * ex + ey * ey);
    }

    double t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    const double cx = ax + t * dx;
    const double cy = ay + t * dy;

    const double ex = px - cx;
    const double ey = py - cy;

    return int(ex * ex + ey * ey);
}

static bool visibleTankGroundTarget(BWAPI::Unit target)
{
    return target && target->exists() && target->isVisible() &&
        target->isDetected() && target->getPosition().isValid() &&
        !target->isFlying() && !target->isInvincible() &&
        !target->getType().isSpell();
}

// Use the real firing boundary, including minimum range. Placement margins
// must never shorten a deployed Tank's weapon range in a mirror matchup.
static bool targetInTankSiegeRange(BWAPI::Unit tank, BWAPI::Unit target)
{
    if (!tank || !tank->exists() || !visibleTankGroundTarget(target))
    {
        return false;
    }
    if (tank->isSieged())
    {
        return tank->isInWeaponRange(target);
    }
    const BWAPI::WeaponType weapon =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon();
    const int distance = tank->getDistance(target);
    return distance >= weapon.minRange() && distance <= weapon.maxRange();
}

static bool preferTankCombatTarget(
    BWAPI::Unit tank, BWAPI::Unit candidate, BWAPI::Unit current,
    int candidatePriority, int currentPriority)
{
    if (!current)
    {
        return true;
    }
    if (tank->isSieged())
    {
        const bool candidateCanFire = targetInTankSiegeRange(tank, candidate);
        const bool currentCanFire = targetInTankSiegeRange(tank, current);
        if (candidateCanFire != currentCanFire)
        {
            return candidateCanFire;
        }
    }
    return candidatePriority > currentPriority ||
        (candidatePriority == currentPriority &&
         tank->getDistance(candidate) < tank->getDistance(current));
}

static bool meaningfulGroundCombatTargetForTanks(BWAPI::Unit target)
{
    if (!visibleTankGroundTarget(target))
    {
        return false;
    }

    if (target->isFlying())
    {
        return false;
    }

    const BWAPI::UnitType type = target->getType();

    // Buildings used to be removed here. Later building-specific siege logic
    // therefore never received a target: tanks drove up to an empty base and
    // then held position beside the remaining structures. Ground buildings
    // are valid cleanup targets; harmless mobile forms remain filtered below.
    if (type.isBuilding())
    {
        return !target->isFlying() && !target->isLifted();
    }

    if (type == BWAPI::UnitTypes::Protoss_Scarab ||
        type == BWAPI::UnitTypes::Zerg_Larva ||
        type == BWAPI::UnitTypes::Zerg_Egg)
    {
        return false;
    }

    // High Templar and Defilers have no weapon, but they are already assigned
    // top Tank priorities below. The old generic weapon test removed them
    // before getAttackPriority() could ever see them. Keep all visible ground
    // spellcasters targetable; unlisted casters receive the shared backstop
    // spellcaster priority.
    if (type.isSpellcaster())
    {
        return true;
    }

    return UnitUtil::CanAttackGround(target) ||
        UnitUtil::CanAttackAir(target);
}

static bool isRealBaseAttackEnemy(BWAPI::Unit enemy)
{
    if (!enemy || !enemy->exists() || !enemy->getPosition().isValid())
    {
        return false;
    }

    if (!enemy->isVisible())
    {
        return false;
    }

    const BWAPI::UnitType type = enemy->getType();

    if (type.isWorker() || type.isSpell())
    {
        return false;
    }

    if (enemy->isFlying())
    {
        return false;
    }

    if (type == BWAPI::UnitTypes::Zerg_Larva ||
        type == BWAPI::UnitTypes::Zerg_Egg ||
        type == BWAPI::UnitTypes::Protoss_Interceptor ||
        type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
    {
        return false;
    }

    return UnitUtil::CanAttackGround(enemy) || !type.isBuilding();
}

// A passing enemy two screens away is not a reason to recall the artillery.
// Actual fire at an owned base is recognized separately, at any distance.
static const int TANK_BASE_DEFENSE_THREAT_RADIUS = 12 * 32;

static bool enemyActivelyAttacksAssetAtBase(BWAPI::Unit enemy, Base* base)
{
    if (!enemy || !enemy->exists() || !base ||
        (!UnitUtil::AttackOrder(enemy) &&
         !enemy->isAttacking() &&
         !enemy->isAttackFrame()))
    {
        return false;
    }

    BWAPI::Unit victim = enemy->getOrderTarget();
    if (!victim || !victim->exists() || victim->getPlayer() != the.self())
    {
        victim = enemy->getTarget();
    }

    if (!victim || !victim->exists() ||
        victim->getPlayer() != the.self() ||
        !victim->getPosition().isValid())
    {
        return false;
    }

    const BWAPI::Position basePos = base->getCenter().isValid()
        ? base->getCenter()
        : base->getPosition();

    return base->isInBase(victim) ||
        (basePos.isValid() && victim->getDistance(basePos) <= 12 * 32);
}

static int enemyBaseAttackScore(BWAPI::Unit enemy)
{
    if (!isRealBaseAttackEnemy(enemy))
    {
        return 0;
    }

    const BWAPI::UnitType type = enemy->getType();

    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) return 8;
    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)  return 6;
    if (type == BWAPI::UnitTypes::Protoss_Reaver)               return 6;
    if (type == BWAPI::UnitTypes::Protoss_Dragoon)              return 4;
    if (type == BWAPI::UnitTypes::Protoss_Zealot)               return 2;
    if (type == BWAPI::UnitTypes::Terran_Goliath)               return 4;
    if (type == BWAPI::UnitTypes::Terran_Vulture)               return 3;
    if (type == BWAPI::UnitTypes::Terran_Marine)                return 2;
    if (type == BWAPI::UnitTypes::Zerg_Lurker)                  return 5;
    if (type == BWAPI::UnitTypes::Zerg_Hydralisk)               return 3;
    if (type == BWAPI::UnitTypes::Zerg_Zergling)                return 1;
    if (type == BWAPI::UnitTypes::Zerg_Ultralisk)               return 6;

    if (UnitUtil::CanAttackGround(enemy))
    {
        return 2;
    }

    return 1;
}

static BWAPI::Position getCurrentlyAttackedOwnedBasePosition()
{
    static int cachedFrame = -1;
    static BWAPI::Position cachedPosition = BWAPI::Positions::Invalid;
    const int now = BWAPI::Broodwar->getFrameCount();
    if (cachedFrame == now) return cachedPosition;
    cachedFrame = now;
    cachedPosition = BWAPI::Positions::Invalid;

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

        if (!base->getDepot() || !base->getDepot()->exists())
        {
            continue;
        }

        const BWAPI::Position basePos = base->getCenter().isValid()
            ? base->getCenter()
            : base->getPosition();

        int score = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!isRealBaseAttackEnemy(enemy))
            {
                continue;
            }

            const bool activeAttack =
                enemyActivelyAttacksAssetAtBase(enemy, base);

            const int groundDistance = base->getTileDistance(enemy->getPosition());
            const bool nearbyGroundThreat =
                enemy->getDistance(basePos) <= TANK_BASE_DEFENSE_THREAT_RADIUS &&
                groundDistance >= 0 && groundDistance <= 16;
            if (!nearbyGroundThreat && !activeAttack)
            {
                continue;
            }

            score += enemyBaseAttackScore(enemy);

            // A single Marine/Zealot which is truly firing at our base must
            // count, while one passing near the edge should not deploy Tanks.
            if (activeAttack)
            {
                score += 3;
            }
        }

        if (score > bestScore)
        {
            bestScore = score;
            bestBase = base;
        }
    }

    if (bestBase && bestScore >= 5)
    {
        cachedPosition = bestBase->getCenter().isValid()
            ? bestBase->getCenter()
            : bestBase->getPosition();
    }

    return cachedPosition;
}

static std::set<int> getTankBaseResponderIDs(
    const BWAPI::Unitset& attackTanks,
    const BWAPI::Position& attackedBasePosition)
{
    std::set<int> result;
    if (attackTanks.empty() || !attackedBasePosition.isValid())
    {
        return result;
    }

    static int cachedFrame = -1;
    static BWAPI::Position cachedBase = BWAPI::Positions::Invalid;
    static unsigned long long cachedSignature = 0;
    static std::set<int> cachedResponders;
    unsigned long long signature = 1469598103934665603ULL;
    for (BWAPI::Unit tank : attackTanks)
    {
        if (!tank || !tank->exists()) continue;
        signature ^= static_cast<unsigned long long>(tank->getID() + 1);
        signature *= 1099511628211ULL;
    }
    const int now = BWAPI::Broodwar->getFrameCount();
    if (cachedFrame == now && cachedBase == attackedBasePosition &&
        cachedSignature == signature)
    {
        return cachedResponders;
    }

    Base* attackedBase = nullptr;
    for (Base* base : the.bases.getAll())
    {
        if (base && base->getOwner() == the.self() &&
            base->getCenter() == attackedBasePosition)
        {
            attackedBase = base;
            break;
        }
    }
    int attackScore = 0;
    bool directAttack = false;
    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!isRealBaseAttackEnemy(enemy)) continue;
        const bool activeAttack =
            attackedBase && enemyActivelyAttacksAssetAtBase(enemy, attackedBase);
        const int groundDistance = attackedBase
            ? attackedBase->getTileDistance(enemy->getPosition()) : -1;
        if (activeAttack ||
            (enemy->getDistance(attackedBasePosition) <=
                TANK_BASE_DEFENSE_THREAT_RADIUS &&
             groundDistance >= 0 && groundDistance <= 16))
        {
            attackScore += enemyBaseAttackScore(enemy);
            directAttack = directAttack || activeAttack;
        }
    }

    int responders = std::max(2, (attackScore + 2) / 3);
    if (!directAttack || attackScore < 18)
    {
        responders = std::min(responders, std::max(2, (int(attackTanks.size()) + 1) / 2));
    }
    responders = std::min(responders, int(attackTanks.size()));

    std::vector<BWAPI::Unit> ordered;
    for (BWAPI::Unit tank : attackTanks)
    {
        if (tank && tank->exists() && tank->getPosition().isValid())
        {
            ordered.push_back(tank);
        }
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [&attackedBasePosition](BWAPI::Unit a, BWAPI::Unit b)
        {
            const int da = a->getDistance(attackedBasePosition);
            const int db = b->getDistance(attackedBasePosition);
            if (da != db) return da < db;
            return a->getID() < b->getID();
        });

    for (int i = 0; i < responders && i < int(ordered.size()); ++i)
    {
        result.insert(ordered[i]->getID());
    }

    cachedFrame = now;
    cachedBase = attackedBasePosition;
    cachedSignature = signature;
    cachedResponders = result;
    return result;
}

static BWAPI::Unit getBestEnemyNearDefensePosition(
    BWAPI::Unit tank,
    const BWAPI::Position& defensePos,
    int radius)
{
    if (!tank || !tank->exists() || !defensePos.isValid())
    {
        return nullptr;
    }

    BWAPI::Unit best = nullptr;
    int bestScore = -1;
    int bestDistance = MAX_DISTANCE;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!isRealBaseAttackEnemy(enemy))
        {
            continue;
        }

        if (enemy->getDistance(defensePos) > radius)
        {
            continue;
        }

        const int score = enemyBaseAttackScore(enemy);
        const int distance = tank->getDistance(enemy);

        if (preferTankCombatTarget(tank, enemy, best, score, bestScore))
        {
            bestScore = score;
            bestDistance = distance;
            best = enemy;
        }
    }

    return best;
}

static BWAPI::Unit getBestEnemyNearReturningTank(
    BWAPI::Unit tank,
    int radius)
{
    if (!tank || !tank->exists() || !tank->getPosition().isValid())
    {
        return nullptr;
    }

    BWAPI::Unit best = nullptr;
    int bestScore = -1;
    int bestDistance = MAX_DISTANCE;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!isRealBaseAttackEnemy(enemy))
        {
            continue;
        }

        if (!enemy->isVisible())
        {
            continue;
        }

        const int distance = tank->getDistance(enemy);

        if (distance > radius)
        {
            continue;
        }

        int score = enemyBaseAttackScore(enemy);

        if (enemy->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            score += 5;
        }
        else if (enemy->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
        {
            score += 4;
        }
        else if (enemy->getType() == BWAPI::UnitTypes::Protoss_Reaver)
        {
            score += 4;
        }
        else if (enemy->getType() == BWAPI::UnitTypes::Zerg_Lurker)
        {
            score += 4;
        }

        if (preferTankCombatTarget(tank, enemy, best, score, bestScore))
        {
            bestScore = score;
            bestDistance = distance;
            best = enemy;
        }
    }

    return best;
}

static bool handleAttackTanksDefendOwnedBase(
    BWAPI::Unit tank,
    const BWAPI::Position& attackedBasePos,
    int now,
    int siegeTankRange,
    int emergencyUnsiegeDistance,
    int tankHoldFrames,
    int postUnsiegeCooldown,
    int& lastUnsiegeFrame,
    int& holdTankUntil,
    int& lastSiegeFrame)
{
    if (!tank || !tank->exists() || !attackedBasePos.isValid())
    {
        return false;
    }

    BWAPI::Unit routeTarget =
        getBestEnemyNearReturningTank(
            tank,
            siegeTankRange + 4 * 32
        );

    BWAPI::Unit baseTarget =
        getBestEnemyNearDefensePosition(
            tank,
            attackedBasePos,
            TANK_BASE_DEFENSE_THREAT_RADIUS
        );

    BWAPI::Unit target = routeTarget ? routeTarget : baseTarget;

    const bool targetIsOnRoute = routeTarget != nullptr;

    const int distToBase = tank->getDistance(attackedBasePos);
    const int distToTarget = target ? tank->getDistance(target) : MAX_DISTANCE;

    const bool targetTooClose =
        target &&
        distToTarget < emergencyUnsiegeDistance &&
        !target->getType().isBuilding();

    const bool targetInSiegeRange =
        target &&
        distToTarget >= emergencyUnsiegeDistance &&
        distToTarget <= siegeTankRange;

    if (Config::Debug::DrawUnitTargets)
    {
        BWAPI::Broodwar->drawCircleMap(
            attackedBasePos,
            12 * 32,
            BWAPI::Colors::Red,
            false
        );
    }

    DrawUnitTargetTextMap(
        attackedBasePos + BWAPI::Position(8, 0),
        "BASE UNDER ATTACK - TANKS RETURN"
    );

    if (tank->isSieged())
    {
        const bool mustHoldAfterSiege =
            now - lastSiegeFrame < 4 * 24;

        if (targetTooClose || tank->isUnderDisruptionWeb())
        {
            DrawUnitTargetTextMap(
                tank->getPosition() + BWAPI::Position(0, 8),
                "def base emergency unsiege"
            );

            if (the.micro.Unsiege(tank, true))
            {
                lastUnsiegeFrame = now;
                holdTankUntil = now + tankHoldFrames;
            }
            return true;
        }

        if (targetInSiegeRange)
        {
            DrawUnitTargetTextMap(
                tank->getPosition() + BWAPI::Position(0, 8),
                "def base fire"
            );

            the.micro.AttackUnit(tank, target);
            return true;
        }

        if (mustHoldAfterSiege)
        {
            DrawUnitTargetTextMap(
                tank->getPosition() + BWAPI::Position(0, 8),
                "def base 4s hold"
            );

            the.micro.HoldPosition(tank);
            return true;
        }

        if (distToBase > 12 * 32)
        {
            if (mustHoldAfterSiege)
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "def route 4s siege"
                );

                the.micro.HoldPosition(tank);
                return true;
            }

            DrawUnitTargetTextMap(
                tank->getPosition() + BWAPI::Position(0, 8),
                "def base continue move"
            );

            if (the.micro.Unsiege(tank))
            {
                lastUnsiegeFrame = now;
                holdTankUntil = now + tankHoldFrames;
            }
            return true;
        }

        the.micro.HoldPosition(tank);
        return true;
    }

    if (targetTooClose && target)
    {
        DrawUnitTargetTextMap(
            tank->getPosition() + BWAPI::Position(0, 8),
            "def base kite"
        );

        the.micro.KiteTarget(tank, target);
        return true;
    }

    if (targetInSiegeRange &&
        tank->canSiege() &&
        now >= holdTankUntil &&
        now - lastUnsiegeFrame >= postUnsiegeCooldown)
    {
        DrawUnitTargetTextMap(
            tank->getPosition() + BWAPI::Position(0, 8),
            "def base siege"
        );

        if (the.micro.Siege(tank))
        {
            lastSiegeFrame = now;
        }
        return true;
    }

    // A newly produced responder is usually already close to the base center.
    // The old code used distToBase as the only movement condition and therefore
    // issued Hold forever when the attackers were on the outer edge of the
    // same base. Close toward the real attacker until Siege range is reached;
    // on the next frame the branch above deploys immediately.
    if (target && distToTarget > siegeTankRange)
    {
        DrawUnitTargetTextMap(
            tank->getPosition() + BWAPI::Position(0, 8),
            "def base reach siege range"
        );

        the.micro.Move(tank, target->getPosition());
        return true;
    }

    if (targetInSiegeRange)
    {
        const bool siegeResearched =
            the.self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode);

        if (!siegeResearched)
        {
            // Before the upgrade finishes, fighting in Tank Mode is still
            // better than waiting beside the Factory.
            the.micro.AttackUnit(tank, target);
        }
        else
        {
            // Siege is researched but a short command/cooldown window prevents
            // the transformation this frame. Do not drive into minimum range;
            // remain here and retry on the next micro update.
            the.micro.HoldPosition(tank);
        }
        return true;
    }

    if (distToBase > 7 * 32)
    {
        if (target &&
            tank->getDistance(target) <=
            BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode.groundWeapon().maxRange())
        {

            the.micro.AttackUnit(tank, target);
            return true;
        }

        if (targetIsOnRoute && target)
        {
            the.micro.AttackUnit(tank, target);
            return true;
        }
        the.micro.Move(tank, attackedBasePos);
        return true;
    }

    if (target && tank->getDistance(target) <= 10 * 32)
    {
        the.micro.AttackUnit(tank, target);
        return true;
    }

    the.micro.HoldPosition(tank);
    return true;
}




// A target is "threatening" if it can attack tanks.
// NOTE All targets are necessarily ground units.
int MicroTanks::nThreats(const BWAPI::Unitset & targets) const
{
    int n = 0;
    for (const BWAPI::Unit target : targets)
    {
        if (UnitUtil::CanAttackGround(target))
        {
            ++n;
        }
    }

    return n;
}

// A unit we should siege for even if there is only one of them.
bool MicroTanks::anySiegeUnits(const BWAPI::Unitset & targets) const
{
    for (const BWAPI::Unit target : targets)
    {
        if (target->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            target->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
            target->getType() == BWAPI::UnitTypes::Terran_Bunker ||
            target->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
            target->getType() == BWAPI::UnitTypes::Protoss_Reaver ||
            target->getType() == BWAPI::UnitTypes::Zerg_Sunken_Colony)
        {
            return true;
        }
    }

    return false;
}

bool MicroTanks::allMeleeAndSameHeight(const BWAPI::Unitset & targets, BWAPI::Unit tank) const
{
    int height = GroundHeight(tank->getTilePosition());

    for (const BWAPI::Unit target : targets)
    {
        if (UnitUtil::GetAttackRange(target, tank) > 32 || height != GroundHeight(target->getTilePosition()))
        {
            return false;
        }
    }

    return true;
}


namespace
{

    int lastTankGroupEnemySeenFrame = -100000;
    BWAPI::Position lastTankGroupEnemyCenter = BWAPI::Positions::Invalid;
    int lastTankGroupEnemyThreatScore = 0;

    struct RememberedEnemyTank
    {
        BWAPI::Position position = BWAPI::Positions::Invalid;
        int lastSeenFrame = -100000;
        bool wasSieged = false;
    };

    // Keep the enemy siege line under fog instead of treating a lost visual as
    // proof that the tanks moved. Entries are removed early when their old
    // position becomes visible and is confirmed empty.
    std::map<int, RememberedEnemyTank> rememberedEnemyTanks;
    const int REMEMBERED_ENEMY_TANK_FRAMES = 24 * 30;
    int lastRememberedEnemyTankUpdateFrame = -100000;

    int lastTankScoutStepFrame = -100000;
    int lastTankMemoryComsatScanFrame = -100000;
    BWAPI::Position lastTankFogFront = BWAPI::Positions::Invalid;
    BWAPI::Position lastTankFogGoal = BWAPI::Positions::Invalid;
    int lastTankFogFrontMoveFrame = -100000;
    int lastTankFogScanFrame = -100000;
    int activeScoutTankID = -1;
    int activeScoutStepUntil = -100000;
    BWAPI::Position activeScoutTarget = BWAPI::Positions::Invalid;

    // Per-frame fire reservations prevent every tank from selecting the same
    // almost-dead unit. Splash values are cached because getTarget() may be
    // queried more than once for the same tank in one frame.
    int coordinatedTankTargetFrame = -100000;
    std::map<int, BWAPI::Unit> coordinatedTargetByTankID;
    std::map<int, int> coordinatedDamageByTankID;
    std::map<int, int> reservedDamageByTargetID;
    std::map<int, int> splashValueByTargetID;
    std::map<int, bool> repairedBuildingByTargetID;

    bool tankMassAttackReleased = false;
    int tankMassAttackReleaseFrame = -100000;
    int tankGatherFirstFrame = -1;
    int tankGatherLastGrowthFrame = -1;
    int tankGatherMaxTankCount = 0;
    int tankGatherMaxGoliathCount = 0;

    std::set<int> pureAirGoliathClusterIDs;
    BWAPI::Position pureAirGoliathClusterCenter = BWAPI::Positions::Invalid;
    int pureAirGoliathClusterCount = 0;
    int pureAirGoliathLastEvaluationFrame = -100000;
    int pureAirGoliathLastSwitchFrame = -100000;
    bool pureAirTankEscortReleased = false;

    BWAPI::Position cachedTankMassGatherPosition = BWAPI::Positions::Invalid;
    BWAPI::Position cachedTankMassGatherStart = BWAPI::Positions::Invalid;
    BWAPI::Position cachedTankMassGatherTarget = BWAPI::Positions::Invalid;
    int cachedTankMassGatherFrame = -100000;
    unsigned int cachedTankMassGatherRevision = 0;

    struct TankGatherPlanCache
    {
        BWAPI::Position anchor = BWAPI::Positions::Invalid;
        BWAPI::Position forward = BWAPI::Positions::Invalid;
        unsigned int partitionRevision = 0;
        unsigned int runtimeRevision = 0;
        int frame = -100000;
        int tankCount = 0;
        unsigned long long tankSignature = 0;
        std::map<int, int> slotIndexByTankID;
        std::vector<BWAPI::Position> slots;
    };

    TankGatherPlanCache tankGatherPlanCache;

    struct TankDefensePlanEntry
    {
        BWAPI::Position slot = BWAPI::Positions::Invalid;
        BWAPI::Position passageCenter = BWAPI::Positions::Invalid;
        BWAPI::Position passageForward = BWAPI::Positions::Invalid;
    };

    struct TankDefensePlanCache
    {
        BWAPI::Position center = BWAPI::Positions::Invalid;
        BWAPI::Position frontDirection = BWAPI::Positions::Invalid;
        unsigned int partitionRevision = 0;
        unsigned int runtimeRevision = 0;
        int frame = -100000;
        int tankCount = 0;
        int bunkerID = -1;
        unsigned long long tankSignature = 0;
        std::map<int, TankDefensePlanEntry> byTankID;
    };

    TankDefensePlanCache tankDefensePlanCache;

    struct TankDefenseAnchorCache
    {
        BWAPI::Position base = BWAPI::Positions::Invalid;
        BWAPI::Position position = BWAPI::Positions::Invalid;
        unsigned int revision = 0;
        unsigned int runtimeRevision = 0;
        int bunkerID = -1;
        int frame = -100000;
    };
    std::map<int, TankDefenseAnchorCache> tankDefenseAnchorCache;
}



// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

MicroTanks::MicroTanks()
{
}

double MicroTanks::defensivePlanMilliseconds() { return tankPlanWork().defenseMs; }
double MicroTanks::gatherPlanMilliseconds() { return tankPlanWork().gatherMs; }
int MicroTanks::defensivePlanBuilds() { return tankPlanWork().defenseBuilds; }
int MicroTanks::gatherPlanBuilds() { return tankPlanWork().gatherBuilds; }

void MicroTanks::ResetForNewGame()
{
    tankMassAttackReleased = false;
    tankMassAttackReleaseFrame = -100000;
    tankGatherFirstFrame = -1;
    tankGatherLastGrowthFrame = -1;
    tankGatherMaxTankCount = 0;
    tankGatherMaxGoliathCount = 0;

    lastTankFogFront = BWAPI::Positions::Invalid;
    lastTankFogGoal = BWAPI::Positions::Invalid;
    lastTankFogFrontMoveFrame = -100000;
    lastTankFogScanFrame = -100000;

    pureAirGoliathClusterIDs.clear();
    pureAirGoliathClusterCenter = BWAPI::Positions::Invalid;
    pureAirGoliathClusterCount = 0;
    pureAirGoliathLastEvaluationFrame = -100000;
    pureAirGoliathLastSwitchFrame = -100000;
    pureAirTankEscortReleased = false;

    cachedTankMassGatherPosition = BWAPI::Positions::Invalid;
    cachedTankMassGatherStart = BWAPI::Positions::Invalid;
    cachedTankMassGatherTarget = BWAPI::Positions::Invalid;
    cachedTankMassGatherFrame = -100000;
    cachedTankMassGatherRevision = 0;
    tankGatherPlanCache = TankGatherPlanCache();
    tankDefensePlanCache = TankDefensePlanCache();
    tankDefenseAnchorCache.clear();

    lastTankGroupEnemySeenFrame = -100000;
    lastTankGroupEnemyCenter = BWAPI::Positions::Invalid;
    lastTankGroupEnemyThreatScore = 0;
    rememberedEnemyTanks.clear();
    lastRememberedEnemyTankUpdateFrame = -100000;

    lastTankScoutStepFrame = -100000;
    lastTankMemoryComsatScanFrame = -100000;
    activeScoutTankID = -1;
    activeScoutStepUntil = -100000;
    activeScoutTarget = BWAPI::Positions::Invalid;

    coordinatedTankTargetFrame = -100000;
    coordinatedTargetByTankID.clear();
    coordinatedDamageByTankID.clear();
    reservedDamageByTargetID.clear();
    splashValueByTargetID.clear();
    repairedBuildingByTargetID.clear();
}

MicroTanks::TankDecisionState& MicroTanks::stateFor(BWAPI::Unit tank)
{
    return _tankState[tank];
}

void MicroTanks::cleanState(const BWAPI::Unitset& activeTanks)
{
    for (auto it = _tankState.begin(); it != _tankState.end(); )
    {
        if (!it->first || !it->first->exists() || activeTanks.find(it->first) == activeTanks.end())
        {
            it = _tankState.erase(it);
        }
        else
        {
            ++it;
        }
    }
}



static int countEnemyCombatNearPosition(const BWAPI::Position& position, int radius)
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

        const BWAPI::UnitType type = enemy->getType();

        if (type.isWorker() || type.isBuilding() || type.isSpell())
        {
            continue;
        }

        if (!UnitUtil::CanAttackGround(enemy) && !UnitUtil::CanAttackAir(enemy))
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

static BWAPI::Position positionBetweenBaseAndHold(
    const BWAPI::Position& basePosition,
    const BWAPI::Position& holdPosition,
    int percentFromBase)
{
    if (!basePosition.isValid() || !holdPosition.isValid())
    {
        return holdPosition;
    }

    percentFromBase = std::max(0, std::min(100, percentFromBase));

    return BWAPI::Position(
        basePosition.x + (holdPosition.x - basePosition.x) * percentFromBase / 100,
        basePosition.y + (holdPosition.y - basePosition.y) * percentFromBase / 100
    );
}

static int countOtherFriendlyTanksNearPosition(
    BWAPI::Unit selfTank,
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
            unit == selfTank)
        {
            continue;
        }

        const BWAPI::UnitType type = unit->getType();

        if (type != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
            type != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            continue;
        }

        if (!unit->getPosition().isValid())
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

static BWAPI::Position getTankGatherFallbackBasePosition()
{
    Base* baseForRetreat = the.bases.myNatural();

    if (!baseForRetreat ||
        baseForRetreat->getOwner() != the.self() ||
        !baseForRetreat->getDepot() ||
        !baseForRetreat->getPosition().isValid())
    {
        baseForRetreat = the.bases.myMain();
    }

    if (baseForRetreat && baseForRetreat->getPosition().isValid())
    {
        return baseForRetreat->getPosition();
    }

    if (the.bases.myStart() && the.bases.myStart()->getCenter().isValid())
    {
        return the.bases.myStart()->getCenter();
    }

    return BWAPI::Positions::Invalid;
}

static BWAPI::Position getTankGather20PercentFallbackPosition(
    const BWAPI::Position& originalHoldPosition)
{
    if (!originalHoldPosition.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position basePosition = getTankGatherFallbackBasePosition();

    if (!basePosition.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position saferPosition =
        positionBetweenBaseAndHold(
            basePosition,
            originalHoldPosition,
            20
        );

    if (!saferPosition.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    return saferPosition;
}

static BWAPI::Position getSaferTankGatherPositionIfNeeded(
    BWAPI::Unit tank,
    const BWAPI::Position& originalHoldPosition)
{
    if (!tank ||
        !tank->exists() ||
        !tank->getPosition().isValid() ||
        !originalHoldPosition.isValid())
    {
        return originalHoldPosition;
    }

    Base* baseForRetreat = the.bases.myNatural();

    if (!baseForRetreat ||
        baseForRetreat->getOwner() != the.self() ||
        !baseForRetreat->getDepot() ||
        !baseForRetreat->getPosition().isValid())
    {
        baseForRetreat = the.bases.myMain();
    }

    if (!baseForRetreat || !baseForRetreat->getPosition().isValid())
    {
        return originalHoldPosition;
    }

    static BWAPI::Position rememberedUnsafeGather = BWAPI::Positions::Invalid;
    static int rememberedUnsafeGatherFrame = -100000;

    const int now = BWAPI::Broodwar->getFrameCount();

    if (now < rememberedUnsafeGatherFrame)
    {
        rememberedUnsafeGather = BWAPI::Positions::Invalid;
        rememberedUnsafeGatherFrame = -100000;
    }

    const int OTHER_TANK_NEAR_HOLD_RADIUS = 7 * 32;
    const int ENEMY_NEAR_HOLD_RADIUS = 13 * 32;
    const int UNSAFE_MEMORY_FRAMES = 24 * 15;

    const int otherFriendlyTanksNearHold =
        countOtherFriendlyTanksNearPosition(
            tank,
            originalHoldPosition,
            OTHER_TANK_NEAR_HOLD_RADIUS
        );

    const int enemiesNearHold =
        countEnemyCombatNearPosition(
            originalHoldPosition,
            ENEMY_NEAR_HOLD_RADIUS
        );

    if (enemiesNearHold > 0 && otherFriendlyTanksNearHold <= 0)
    {
        rememberedUnsafeGather = originalHoldPosition;
        rememberedUnsafeGatherFrame = now;
    }

    const bool rememberedUnsafe =
        rememberedUnsafeGather.isValid() &&
        rememberedUnsafeGather.getApproxDistance(originalHoldPosition) <= 4 * 32 &&
        now - rememberedUnsafeGatherFrame <= UNSAFE_MEMORY_FRAMES;

    const bool gatherIsUnsafe =
        enemiesNearHold > 0 ||
        rememberedUnsafe;

    if (!gatherIsUnsafe)
    {
        return originalHoldPosition;
    }

    if (otherFriendlyTanksNearHold >= 2)
    {
        return originalHoldPosition;
    }

    BWAPI::Position saferPosition =
        positionBetweenBaseAndHold(
            baseForRetreat->getPosition(),
            originalHoldPosition,
            20
        );

    if (!saferPosition.isValid())
    {
        return originalHoldPosition;
    }
    return saferPosition;
}


static BWAPI::Position getBaseFrontPositionForTanks(Base* base);
static BWAPI::Position getBaseApproachPositionForTanks(Base* base);
static BWAPI::Position clampToMapForTanks(const BWAPI::Position& p);
static bool isWalkableTankGatherPosition(const BWAPI::Position& pos);
static void holdGatheringTank(BWAPI::Unit tank);
static BWAPI::Position findNearbyWalkableTankGatherPosition(const BWAPI::Position& desired);
static BWAPI::Position findCompactTankParkingPosition(const BWAPI::Position& desired);

//mine
static int countBuildingsInSiegeRange(BWAPI::Unit tank, const BWAPI::Unitset& targets)
{
    if (!tank)
    {
        return 0;
    }

    const int siegeTankRange =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();

    int count = 0;

    for (BWAPI::Unit enemy : targets)
    {
        if (!enemy || !enemy->exists())
        {
            continue;
        }

        if (enemy->isFlying())
        {
            continue;
        }

        if (!enemy->getType().isBuilding())
        {
        continue;
        }
            
        if (tank->getDistance(enemy) <= siegeTankRange)
        {
        ++count;
        }
    }

   return count;
}

static BWAPI::Position getBaseApproachPositionForTanks(Base* base)
{
    if (!base)
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position front = getBaseFrontPositionForTanks(base);

    if (!front.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position center = base->getCenter();

    if (!center.isValid())
    {
        return front;
    }

    double dx = double(front.x - center.x);
    double dy = double(front.y - center.y);
    double len = std::sqrt(dx * dx + dy * dy);

    if (len < 1.0)
    {
        return front;
    }

    dx /= len;
    dy /= len;

    const int forwardOffset = 4 * 32;

    BWAPI::Position approach(
        int(front.x + dx * forwardOffset),
        int(front.y + dy * forwardOffset)
    );

    approach = clampToMapForTanks(approach);

    if (approach.isValid() && isWalkableTankGatherPosition(approach))
    {
        return approach;
    }

    return findNearbyWalkableTankGatherPosition(approach);
}

static BWAPI::Position getBaseFrontPositionForTanks(Base* base)
{
    if (!base)
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position front = base->getFront();

    if (front.isValid() && front != BWAPI::Positions::Unknown)
    {
        return front;
    }

    if (base->getPosition().isValid())
    {
        return base->getPosition();
    }

    if (base->getCenter().isValid())
    {
        return base->getCenter();
    }

    return BWAPI::Positions::Invalid;
}

static bool isTankClearableEnemyBuilding(BWAPI::Unit enemy)
{
    if (!enemy ||
        !enemy->exists() ||
        !enemy->isVisible() ||
        !enemy->getPosition().isValid())
    {
        return false;
    }

    const BWAPI::UnitType type = enemy->getType();

    if (!type.isBuilding())
    {
        return false;
    }

    if (enemy->isFlying() || enemy->isLifted())
    {
        return false;
    }

    if (type == BWAPI::UnitTypes::Zerg_Larva ||
        type == BWAPI::UnitTypes::Zerg_Egg)
    {
        return false;
    }

    return true;
}

static Base* getKnownBaseForTankGoal(const BWAPI::Position& position)
{
    if (!position.isValid())
    {
        return nullptr;
    }

    Base* base = the.bases.findBaseByPosition(position);
    if (!base)
    {
        // Zone borders can run through the footprint of a large depot on
        // unusual maps. The tile fallback is deliberately tight (Bases uses
        // eight tiles), and also recognizes formation slots just outside the
        // depot without treating arbitrary map positions as base goals.
        base = the.bases.getBaseAtTilePosition(BWAPI::TilePosition(position));
    }
    return base;
}

// Cheap tactical ordering for moving Tanks. This must never create a new
// GridDistances field: it is used once per Tank or per visible building while
// the army is moving. When the destination belongs to a known base, reuse that
// base's startup distance field. A straight-line fallback is acceptable only
// for non-base tactical positions.
//
// The old implementation checked static connectivity and then returned air
// distance. On maps where two bases are close across a cliff (Roadrunner is a
// clear example), a Command Center on the other side of the wall therefore
// looked like the closest ground objective and could pull the whole Tank army
// into our own cliff/buildings.
static int tacticalDistanceForTanks(
    const BWAPI::Position& from,
    const BWAPI::Position& to)
{
    if (!from.isValid() || !to.isValid() || !the.partitions.connected(from, to))
    {
        return MAX_DISTANCE;
    }

    Base* destinationBase = getKnownBaseForTankGoal(to);
    if (destinationBase)
    {
        const int baseDistance = destinationBase->getTileDistance(from);
        return baseDistance >= 0 ? baseDistance : MAX_DISTANCE;
    }

    return std::max(0, from.getApproxDistance(to) / 32);
}

static bool tankGroundRouteIsDirectEnough(
    const BWAPI::Position& from,
    const BWAPI::Position& to,
    int allowedDetourTiles = 6)
{
    const int groundDistance = tacticalDistanceForTanks(from, to);
    if (groundDistance == MAX_DISTANCE)
    {
        return false;
    }

    const int directDistanceTiles =
        std::max(0, (from.getApproxDistance(to) + 31) / 32);
    Base* destinationBase = getKnownBaseForTankGoal(to);
    const int goalOffsetTiles =
        destinationBase && destinationBase->getCenter().isValid()
        ? std::max(
            0,
            (destinationBase->getCenter().getApproxDistance(to) + 31) / 32)
        : 0;

    return groundDistance <=
        directDistanceTiles + goalOffsetTiles + allowedDetourTiles;
}

// A building across a cliff is a valid artillery target when this Tank can
// actually shell it. Otherwise it is a strategic destination, not a local
// AttackUnit target: movement must first follow the real ground route around
// the terrain.
static bool tankCanLocallyEngageBuilding(
    BWAPI::Unit tank,
    BWAPI::Unit building)
{
    if (!tank || !tank->exists() || !building || !building->exists() ||
        !building->getType().isBuilding())
    {
        return false;
    }

    if (targetInTankSiegeRange(tank, building))
    {
        return true;
    }

    return tankGroundRouteIsDirectEnough(
        tank->getPosition(), building->getPosition());
}

static BWAPI::Position getClosestVisibleEnemyBuildingGoalForTanks(
    const BWAPI::Position& referencePosition)
{
    if (!referencePosition.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Unit bestBuilding = nullptr;
    int bestGroundDistance = MAX_DISTANCE;
    int bestAirDistance = MAX_DISTANCE;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!isTankClearableEnemyBuilding(enemy))
        {
            continue;
        }

        const BWAPI::Position buildingPosition = enemy->getPosition();

        const int groundDistance =
            tacticalDistanceForTanks(referencePosition, buildingPosition);

        if (groundDistance == MAX_DISTANCE)
        {
            continue;
        }

        const int airDistance =
            referencePosition.getApproxDistance(buildingPosition);

        if (groundDistance < bestGroundDistance ||
            (groundDistance == bestGroundDistance && airDistance < bestAirDistance))
        {
            bestGroundDistance = groundDistance;
            bestAirDistance = airDistance;
            bestBuilding = enemy;
        }
    }

    if (!bestBuilding)
    {
        return BWAPI::Positions::Invalid;
    }

    return bestBuilding->getPosition();
}

static bool hasVisibleEnemyGroundCombatNearPosition(
    const BWAPI::Position& position,
    int radius)
{
    if (!position.isValid())
    {
        return false;
    }

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!enemy ||
            !enemy->exists() ||
            !enemy->isVisible() ||
            !enemy->getPosition().isValid() ||
            enemy->isFlying())
        {
            continue;
        }

        const BWAPI::UnitType type = enemy->getType();

        if (type.isWorker() || type.isBuilding() || type.isSpell())
        {
            continue;
        }

        if (!UnitUtil::CanAttackGround(enemy))
        {
            continue;
        }

        if (enemy->getDistance(position) <= radius)
        {
            return true;
        }
    }

    return false;
}

static BWAPI::Position getTankAnchorGoal(const BWAPI::Position& fallbackGoal)
{
    if (fallbackGoal.isValid())
    {
        Base* closestEnemyBase = nullptr;
        int bestGroundDistance = MAX_DISTANCE;
        int bestAirDistance = MAX_DISTANCE;

        for (Base* base : the.bases.getAll())
        {
            if (!base || !base->getPosition().isValid())
            {
                continue;
            }

            if (base->getOwner() != BWAPI::Broodwar->enemy())
            {
                continue;
            }

            BWAPI::Position approach = getBaseApproachPositionForTanks(base);

            if (!approach.isValid())
            {
                approach = getBaseFrontPositionForTanks(base);
            }

            if (!approach.isValid())
            {
                continue;
            }

            const int groundDistance =
                tacticalDistanceForTanks(fallbackGoal, approach);

            const int airDistance =
                fallbackGoal.getApproxDistance(approach);

            if (groundDistance < bestGroundDistance ||
                (groundDistance == bestGroundDistance && airDistance < bestAirDistance))
            {
                bestGroundDistance = groundDistance;
                bestAirDistance = airDistance;
                closestEnemyBase = base;
            }
        }

        if (closestEnemyBase && bestGroundDistance != MAX_DISTANCE)
        {
            BWAPI::Position approach =
                getBaseApproachPositionForTanks(closestEnemyBase);

            if (approach.isValid())
            {
                return approach;
            }

            BWAPI::Position front =
                getBaseFrontPositionForTanks(closestEnemyBase);

            if (front.isValid())
            {
                return front;
            }
        }

        return fallbackGoal;
    }

    Base* enemyBase = the.bases.enemyStart();

    if (enemyBase)
    {
        BWAPI::Position approach =
            getBaseApproachPositionForTanks(enemyBase);

        if (approach.isValid())
        {
            return approach;
        }

        BWAPI::Position front =
            getBaseFrontPositionForTanks(enemyBase);

        if (front.isValid())
        {
            return front;
        }

        if (enemyBase->getPosition().isValid())
        {
            return enemyBase->getPosition();
        }

        if (enemyBase->getCenter().isValid())
        {
            return enemyBase->getCenter();
        }
    }

    return BWAPI::Positions::Invalid;
}

static BWAPI::Unit getForwardMostTankTowardEnemyBase(
    const BWAPI::Unitset& tanks,
    const BWAPI::Position& fallbackGoal)
{
    // Once the mass attack is released, keep advancing toward the real order
    // (or a cleanup building) instead of returning to the outside approach
    // point of an enemy base.
    BWAPI::Position goal =
        tankMassAttackReleased && fallbackGoal.isValid()
        ? fallbackGoal
        : getTankAnchorGoal(fallbackGoal);

    if (!goal.isValid())
    {
        goal = fallbackGoal;
    }

    if (!goal.isValid())
    {
        return nullptr;
    }

    BWAPI::Unit bestTank = nullptr;
    int bestGroundDistance = MAX_DISTANCE;
    int bestAirDistance = MAX_DISTANCE;

    for (BWAPI::Unit tank : tanks)
    {
        if (!tank || !tank->exists() || !tank->getPosition().isValid())
        {
            continue;
        }

        const int groundDistance =
            tacticalDistanceForTanks(
                tank->getPosition(),
                goal
            );

        const int airDistance =
            tank->getDistance(goal);

        if (groundDistance == MAX_DISTANCE)
        {
            continue;
        }

        if (groundDistance < bestGroundDistance ||
            (groundDistance == bestGroundDistance && airDistance < bestAirDistance))
        {
            bestGroundDistance = groundDistance;
            bestAirDistance = airDistance;
            bestTank = tank;
        }
    }

    return bestTank;
}

// A single leading tank is too unstable to be the anchor for the whole army:
// a scout step, a blocked tile or one local target can otherwise pull every
// rear tank in a different direction. Use the average of the leading band.
static BWAPI::Position getTankFrontCenterTowardGoal(
    const BWAPI::Unitset& tanks,
    const BWAPI::Position& goal)
{
    if (!goal.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    std::vector<std::pair<int, BWAPI::Unit>> ordered;
    for (BWAPI::Unit tank : tanks)
    {
        if (!tank || !tank->exists() || !tank->getPosition().isValid())
        {
            continue;
        }

        if (tanks.size() > 1 && tank->getID() == activeScoutTankID)
        {
            continue;
        }

        const int groundDistance = tacticalDistanceForTanks(tank->getPosition(), goal);
        if (groundDistance != MAX_DISTANCE)
        {
            ordered.push_back(std::make_pair(groundDistance, tank));
        }
    }

    if (ordered.empty())
    {
        return BWAPI::Positions::Invalid;
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const std::pair<int, BWAPI::Unit>& a, const std::pair<int, BWAPI::Unit>& b)
        {
            if (a.first != b.first) return a.first < b.first;
            return a.second->getID() < b.second->getID();
        });

    const int frontCountLimit = std::min(
        5,
        std::max(1, (int(ordered.size()) + 3) / 4));

    // Average only one coherent leading band. If one Tank has already gone
    // around a cliff while the next four are still on the far side, averaging
    // all five positions places the shared "front" inside the wall. Rear
    // Tanks then repeatedly move at that impossible midpoint. Ground-distance
    // separation is the reliable signal here: across-cliff Tanks can be close
    // by air while still being many route tiles behind the leader.
    int frontCount = 1;
    const int bestGroundDistance = ordered.front().first;
    while (frontCount < frontCountLimit &&
        ordered[frontCount].first <= bestGroundDistance + 6)
    {
        ++frontCount;
    }

    long long sumX = 0;
    long long sumY = 0;
    for (int i = 0; i < frontCount; ++i)
    {
        sumX += ordered[i].second->getPosition().x;
        sumY += ordered[i].second->getPosition().y;
    }

    const BWAPI::Position center(
        int(sumX / frontCount),
        int(sumY / frontCount));

    if (center.isValid() && isWalkableTankGatherPosition(center))
    {
        return center;
    }

    // Averages can still land on a small doodad edge. Keep the leading Tank's
    // known-valid position rather than inventing another unreachable anchor.
    return ordered.front().second->getPosition();
}

static BWAPI::Position getJoinPositionNearTankFront(
    BWAPI::Unit tank,
    const BWAPI::Position& frontCenter,
    const BWAPI::Position& enemyGoal)
{
    if (!tank || !tank->exists() || !frontCenter.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    if (!enemyGoal.isValid())
    {
        return frontCenter;
    }

    double dx = double(frontCenter.x - enemyGoal.x);
    double dy = double(frontCenter.y - enemyGoal.y);
    double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0)
    {
        return frontCenter;
    }

    dx /= len;
    dy /= len;
    const double sideX = -dy;
    const double sideY = dx;

    // Preserve the tank's current lateral lane. Unit-id based columns were
    // stable but geometrically random, so tanks arriving on opposite sides
    // could be ordered through each other while trying to join the front.
    double lateral =
        double(tank->getPosition().x - frontCenter.x) * sideX +
        double(tank->getPosition().y - frontCenter.y) * sideY;
    lateral = std::max(-3.0 * 32.0, std::min(3.0 * 32.0, lateral));

    const int distanceToFront = tank->getDistance(frontCenter);
    const int backOffset = std::max(2 * 32, std::min(4 * 32, distanceToFront / 3));

    const BWAPI::Position desired(
        int(frontCenter.x + dx * backOffset + sideX * lateral),
        int(frontCenter.y + dy * backOffset + sideY * lateral));

    const BWAPI::Position walkable = findNearbyWalkableTankGatherPosition(desired);
    return walkable.isValid() ? walkable : frontCenter;
}

static BWAPI::Position getJoinPositionNearForwardTank(BWAPI::Unit tank, BWAPI::Unit forwardTank, const BWAPI::Position& enemyGoal)
{
    if (!tank || !forwardTank || !forwardTank->exists())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position anchor = forwardTank->getPosition();

    if (!enemyGoal.isValid())
    {
        return anchor;
    }

    double dx = double(anchor.x - enemyGoal.x);
    double dy = double(anchor.y - enemyGoal.y);
    double len = std::sqrt(dx * dx + dy * dy);

    if (len < 1.0)
    {
        return anchor;
    }

    dx /= len;
    dy /= len;

    const int backOffset = 2 * 32;

    const double sx = -dy;
    const double sy = dx;
    double sideOffset =
        double(tank->getPosition().x - anchor.x) * sx +
        double(tank->getPosition().y - anchor.y) * sy;
    sideOffset = std::max(-2.0 * 32.0, std::min(2.0 * 32.0, sideOffset));

    BWAPI::Position result(
        int(anchor.x + dx * backOffset + sx * sideOffset),
        int(anchor.y + dy * backOffset + sy * sideOffset)
    );

    if (result.isValid())
    {
        return result;
    }

    return anchor;
}

static BWAPI::Position getWideFireLinePositionNearTankFront(
    BWAPI::Unit tank,
    const BWAPI::Position& frontCenter,
    const BWAPI::Unitset& allAttackTanks,
    const BWAPI::Position& fireTargetPosition)
{
    if (!tank || !tank->exists() ||
        !frontCenter.isValid() ||
        !fireTargetPosition.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    std::vector<BWAPI::Unit> orderedTanks;
    for (BWAPI::Unit other : allAttackTanks)
    {
        if (other && other->exists() &&
            (other->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
             other->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode))
        {
            orderedTanks.push_back(other);
        }
    }

    std::sort(orderedTanks.begin(), orderedTanks.end(),
        [](BWAPI::Unit a, BWAPI::Unit b) { return a->getID() < b->getID(); });

    int index = 0;
    for (int i = 0; i < int(orderedTanks.size()); ++i)
    {
        if (orderedTanks[i] == tank)
        {
            index = i;
            break;
        }
    }

    double dx = double(frontCenter.x - fireTargetPosition.x);
    double dy = double(frontCenter.y - fireTargetPosition.y);
    double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0)
    {
        return getJoinPositionNearTankFront(tank, frontCenter, fireTargetPosition);
    }
    dx /= len;
    dy /= len;
    const double sideX = -dy;
    const double sideY = dx;

    // Keep the firing block narrow.  The old code created additional side
    // banks after three rows, which is exactly how a large tank army became a
    // single strip across half the map.  Four tanks fill a row; additional
    // tanks add DEPTH.  Deep rows may sit just outside siege range until the
    // front advances, which is preferable to blocking every lateral route.
    const int slotsPerRow = 4;
    const int row = index / slotsPerRow;
    const int column = index % slotsPerRow;

    static const int fireSideSlots[4] = { -132, -44, 44, 132 };
    const double radius = 210.0 + double(row * 40);
    const double side = double(fireSideSlots[column]);

    BWAPI::Position desired(
        int(fireTargetPosition.x + dx * radius + sideX * side),
        int(fireTargetPosition.y + dy * radius + sideY * side));

    BWAPI::Position result = findCompactTankParkingPosition(desired);
    return result.isValid()
        ? result
        : getJoinPositionNearTankFront(tank, frontCenter, fireTargetPosition);
}

static bool tankHasDangerouslyCloseEnemy(BWAPI::Unit tank, const BWAPI::Unitset& targets)
{
    if (!tank || !tank->exists())
    {
        return false;
    }

    for (BWAPI::Unit enemy : targets)
    {
        if (!enemy || !enemy->exists() || enemy->isFlying())
        {
            continue;
        }

        if (enemy->getType().isBuilding())
        {
            continue;
        }

        if (tank->getDistance(enemy) <= 5 * 32 && UnitUtil::CanAttackGround(enemy))
        {
            return true;
        }
    }

    return false;
}

// Adapted from the useful part of Terminus' siege rule: do not start the long
// transformation when a mobile threat can enter the siege minimum range before
// it finishes. A supported front may still deploy, because overlapping tanks
// cover one another while the mode change is in progress.
static bool tankMaySafelyEnterSiege(
    BWAPI::Unit tank,
    const BWAPI::Unitset& targets,
    const BWAPI::Unitset& allAttackTanks)
{
    // Chokepoint safety is checked by the caller, where the MicroManager
    // navigation context is available. Keep this helper focused on the
    // predicted minimum-range danger during the siege transition.
    if (!tank || !tank->exists() || tank->isUnderDisruptionWeb())
    {
        return false;
    }

    int nearbyTankCount = 0;
    for (BWAPI::Unit friendly : allAttackTanks)
    {
        if (friendly && friendly->exists() && tank->getDistance(friendly) <= 9 * 32)
        {
            ++nearbyTankCount;
        }
    }

    const bool supportedFront = nearbyTankCount >= 4;
    const int minimumRange =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().minRange();
    const int siegeTransitionFrames = 70;

    for (BWAPI::Unit enemy : targets)
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() || enemy->isFlying())
        {
            continue;
        }

        const BWAPI::UnitType type = enemy->getType();
        if (type.isBuilding() || type.isWorker() || !UnitUtil::CanAttackGround(enemy))
        {
            continue;
        }

        const int distance = tank->getDistance(enemy);
        if (distance < minimumRange + 24)
        {
            return false;
        }

        // These are exactly the units for which quickly deploying at range is
        // normally the correct answer, not a reason to remain in tank mode.
        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
            type == BWAPI::UnitTypes::Protoss_Reaver ||
            (type == BWAPI::UnitTypes::Zerg_Lurker && enemy->isBurrowed()))
        {
            continue;
        }

        const double speed = enemy->getPlayer()->topSpeed(type);
        const double predictedDistance =
            double(distance) - speed * double(siegeTransitionFrames);

        if (!supportedFront && predictedDistance < minimumRange + 40)
        {
            return false;
        }
    }

    return true;
}

static bool isEnemyTerranTank(BWAPI::Unit enemy)
{
    if (!enemy || !enemy->exists())
    {
        return false;
    }

    return
        enemy->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        enemy->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
}

static int countVisibleEnemyTanksNear(const BWAPI::Position& pos, int radius)
{
    if (!pos.isValid())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!isEnemyTerranTank(enemy))
        {
            continue;
        }

        if (!enemy->isVisible())
        {
            continue;
        }

        if (!enemy->getPosition().isValid())
        {
            continue;
        }

        if (enemy->getDistance(pos) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static int countVisibleEnemySiegedTanksNear(const BWAPI::Position& pos, int radius)
{
    if (!pos.isValid())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!enemy || !enemy->exists())
        {
            continue;
        }

        if (!enemy->isVisible())
        {
            continue;
        }

        if (enemy->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            continue;
        }

        if (!enemy->getPosition().isValid())
        {
            continue;
        }

        if (enemy->getDistance(pos) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static BWAPI::Position getVisibleEnemyTankClusterCenterNear(
    const BWAPI::Position& pos,
    int radius)
{
    if (!pos.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    int count = 0;
    int sumX = 0;
    int sumY = 0;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!isEnemyTerranTank(enemy))
        {
            continue;
        }

        if (!enemy->isVisible())
        {
            continue;
        }

        if (!enemy->getPosition().isValid())
        {
            continue;
        }

        if (enemy->getDistance(pos) > radius)
        {
            continue;
        }

        ++count;
        sumX += enemy->getPosition().x;
        sumY += enemy->getPosition().y;
    }

    if (count <= 0)
    {
        return BWAPI::Positions::Invalid;
    }

    return BWAPI::Position(sumX / count, sumY / count);
}

static int countFriendlyTanksNear(
    const BWAPI::Unitset& tanks,
    const BWAPI::Position& pos,
    int radius)
{
    if (!pos.isValid())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit tank : tanks)
    {
        if (!tank || !tank->exists())
        {
            continue;
        }

        if (tank->getDistance(pos) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static BWAPI::Position positionAwayFrom(
    const BWAPI::Position& from,
    const BWAPI::Position& danger,
    int distance)
{
    if (!from.isValid() || !danger.isValid())
    {
        return from;
    }

    const double dx = double(from.x - danger.x);
    const double dy = double(from.y - danger.y);
    const double len = std::sqrt(dx * dx + dy * dy);

    if (len < 1.0)
    {
        return from;
    }

    BWAPI::Position result(
        from.x + int(dx / len * distance),
        from.y + int(dy / len * distance)
    );

    if (result.isValid())
    {
        return result;
    }

    return from;
}

static BWAPI::Position getFriendlyTankGroupCenter(const BWAPI::Unitset& tanks)
{
    int sumX = 0;
    int sumY = 0;
    int count = 0;

    for (BWAPI::Unit tank : tanks)
    {
        if (!tank || !tank->exists())
        {
            continue;
        }

        if (!tank->getPosition().isValid())
        {
            continue;
        }

        sumX += tank->getPosition().x;
        sumY += tank->getPosition().y;
        ++count;
    }

    if (count <= 0)
    {
        return BWAPI::Positions::Invalid;
    }

    return BWAPI::Position(sumX / count, sumY / count);
}

static BWAPI::Position getLargestFriendlyTankClusterCenter(const BWAPI::Unitset& tanks)
{
    if (tanks.empty())
    {
        return BWAPI::Positions::Invalid;
    }

    const int CLUSTER_RADIUS = 10 * 32;

    BWAPI::Position bestCenter = BWAPI::Positions::Invalid;
    int bestClusterSize = 0;

    for (BWAPI::Unit seed : tanks)
    {
        if (!seed || !seed->exists() || !seed->getPosition().isValid())
        {
            continue;
        }

        int sumX = 0;
        int sumY = 0;
        int count = 0;

        for (BWAPI::Unit tank : tanks)
        {
            if (!tank || !tank->exists() || !tank->getPosition().isValid())
            {
                continue;
            }

            if (seed->getDistance(tank) > CLUSTER_RADIUS)
            {
                continue;
            }

            sumX += tank->getPosition().x;
            sumY += tank->getPosition().y;
            ++count;
        }

        if (count > bestClusterSize)
        {
            bestClusterSize = count;
            bestCenter = BWAPI::Position(sumX / count, sumY / count);
        }
    }

    if (bestCenter.isValid() && Config::Debug::DrawUnitTargets)
    {
        BWAPI::Broodwar->drawCircleMap(
            bestCenter,
            CLUSTER_RADIUS,
            BWAPI::Colors::Green,
            false
        );

        DrawUnitTargetTextMap(
            bestCenter + BWAPI::Position(8, 0),
            "largest tank group %d",
            bestClusterSize
        );
    }

    return bestCenter;
}

static BWAPI::Position clampToMapForTanks(const BWAPI::Position& p)
{
    const int mapW = BWAPI::Broodwar->mapWidth() * 32;
    const int mapH = BWAPI::Broodwar->mapHeight() * 32;

    return BWAPI::Position(
        std::max(0, std::min(mapW - 1, p.x)),
        std::max(0, std::min(mapH - 1, p.y))
    );
}

static bool isWalkableTankGatherPosition(const BWAPI::Position& pos)
{
    if (!pos.isValid())
    {
        return false;
    }

    return the.map.isGroundPositionWalkable(
        pos, BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode);
}

static BWAPI::Position findNearbyWalkableTankGatherPosition(const BWAPI::Position& desired)
{
    if (isWalkableTankGatherPosition(desired))
    {
        return desired;
    }

    static const BWAPI::Position offsets[] =
    {
        BWAPI::Position(0, 0),

        BWAPI::Position(64, 0),
        BWAPI::Position(-64, 0),
        BWAPI::Position(0, 64),
        BWAPI::Position(0, -64),

        BWAPI::Position(96, 0),
        BWAPI::Position(-96, 0),
        BWAPI::Position(0, 96),
        BWAPI::Position(0, -96),

        BWAPI::Position(96, 96),
        BWAPI::Position(96, -96),
        BWAPI::Position(-96, 96),
        BWAPI::Position(-96, -96),

        BWAPI::Position(128, 0),
        BWAPI::Position(-128, 0),
        BWAPI::Position(0, 128),
        BWAPI::Position(0, -128)
    };

    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestDistance = MAX_DISTANCE;

    for (const BWAPI::Position& offset : offsets)
    {
        BWAPI::Position test = clampToMapForTanks(desired + offset);

        if (!isWalkableTankGatherPosition(test))
        {
            continue;
        }

        const int distance = test.getApproxDistance(desired);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = test;
        }
    }

    if (best.isValid())
    {
        return best;
    }

    return desired;
}

// Defensive tanks must never turn the route through our front into a unit wall.
// BWAPI hasPath() only describes terrain connectivity and does not account for
// temporary unit collision, so reserve an explicit local corridor through the
// defensive anchor and park tanks on its flanks.
static bool getDefensePassageSegment(
    const BWAPI::Position& center,
    const BWAPI::Position& forward,
    BWAPI::Position& inside,
    BWAPI::Position& outside)
{
    if (!center.isValid())
    {
        return false;
    }

    const double dx = double(forward.x);
    const double dy = double(forward.y);
    const double length = std::sqrt(dx * dx + dy * dy);

    if (length < 1.0)
    {
        return false;
    }

    const double ux = dx / length;
    const double uy = dy / length;

    inside = clampToMapForTanks(BWAPI::Position(
        center.x - int(ux * 9 * 32),
        center.y - int(uy * 9 * 32)
    ));

    outside = clampToMapForTanks(BWAPI::Position(
        center.x + int(ux * 11 * 32),
        center.y + int(uy * 11 * 32)
    ));

    return inside.isValid() && outside.isValid();
}

static bool positionBlocksDefensePassage(
    const BWAPI::Position& position,
    const BWAPI::Position& center,
    const BWAPI::Position& forward)
{
    if (!position.isValid())
    {
        return false;
    }

    BWAPI::Position inside = BWAPI::Positions::Invalid;
    BWAPI::Position outside = BWAPI::Positions::Invalid;

    if (!getDefensePassageSegment(center, forward, inside, outside))
    {
        return false;
    }

    // Keep a real traffic lane, but do not waste half of a wide platform on it.
    // Two tiles from the center line still leaves a generous corridor between
    // the inner pair of tanks while keeping the formation compact.
    const int passageHalfWidth = 2 * 32;

    return
        distPointToSegmentSq(position, inside, outside) <=
        passageHalfWidth * passageHalfWidth;
}

static bool tankParkingStaticTerrainIsClear(const BWAPI::Position& position)
{
    const BWAPI::UnitType body = BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
    const int left = position.x - body.dimensionLeft();
    const int right = position.x + body.dimensionRight();
    const int top = position.y - body.dimensionUp();
    const int bottom = position.y + body.dimensionDown();
    if (!position.isValid() || left < 0 || top < 0 ||
        right >= BWAPI::Broodwar->mapWidth() * 32 ||
        bottom >= BWAPI::Broodwar->mapHeight() * 32 ||
        !UnitUtil::TankPositionHasSafeDepotClearance(position))
        return false;

    // Check every walk cell touched by the actual deployed body, including
    // interior cells. Corner sampling misses thin obstacles on stepped ramps.
    return the.map.isGroundPositionWalkable(position, body);
}

static bool tankParkingTerrainIsClear(const BWAPI::Position& position)
{
    if (!tankParkingStaticTerrainIsClear(position)) return false;
    const BWAPI::UnitType body = BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
    const int left = position.x - body.dimensionLeft();
    const int right = position.x + body.dimensionRight();
    const int top = position.y - body.dimensionUp();
    const int bottom = position.y + body.dimensionDown();
    for (int x = left / 32; x <= right / 32; ++x)
    {
        for (int y = top / 32; y <= bottom / 32; ++y)
        {
            for (BWAPI::Unit unit : BWAPI::Broodwar->getUnitsOnTile(x, y))
            {
                if (!unit || !unit->exists() || unit->isFlying() ||
                    (!unit->getType().isBuilding() && !unit->getType().isResourceContainer()))
                    continue;
                const BWAPI::Position p = unit->getPosition();
                const BWAPI::UnitType type = unit->getType();
                if (right >= p.x - type.dimensionLeft() && left <= p.x + type.dimensionRight() &&
                    bottom >= p.y - type.dimensionUp() && top <= p.y + type.dimensionDown())
                    return false;
            }
        }
    }
    return true;
}

static bool tankParkingOccupiedByAnotherTank(
    const BWAPI::Position& position,
    BWAPI::Unit ignoredTank)
{
    if (!position.isValid())
    {
        return true;
    }

    // Formation planning probes many candidate points. Querying BWAPI's unit
    // index for every point multiplied the cost of a large Tank line. Unit
    // positions cannot change during this bot's single frame of decisions.
    static int cachedFrame = -1;
    static std::vector<std::pair<int, BWAPI::Position>> cachedTanks;
    const int now = BWAPI::Broodwar->getFrameCount();
    if (cachedFrame != now)
    {
        cachedFrame = now;
        cachedTanks.clear();
        for (int side = 0; side < 2; ++side)
        {
            BWAPI::Player player = side == 0
                ? BWAPI::Broodwar->self() : BWAPI::Broodwar->enemy();
            if (!player) continue;
            for (BWAPI::Unit unit : player->getUnits())
            {
                if (!unit || !unit->exists() || !unit->getPosition().isValid() ||
                    (player != BWAPI::Broodwar->self() && !unit->isVisible()))
                {
                    continue;
                }
                const BWAPI::UnitType type = unit->getType();
                if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                    type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
                {
                    cachedTanks.emplace_back(unit->getID(), unit->getPosition());
                }
            }
        }
    }
    const int ignoredID = ignoredTank ? ignoredTank->getID() : -1;
    for (const auto& other : cachedTanks)
    {
        if (other.first != ignoredID &&
            other.second.getApproxDistance(position) < 56)
        {
            return true;
        }
    }
    return false;
}

// Rare local recovery, used only after a deploy command cannot make progress.
// Keep an artillery shot and enemy clearance while moving sideways/backwards.
static BWAPI::Position findCombatTankSiegePosition(BWAPI::Unit tank, BWAPI::Unit target)
{
    if (!tank || !target) return BWAPI::Positions::Invalid;
    const BWAPI::Position origin = tank->getPosition();
    const int range = BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();
    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestScore = MAX_DISTANCE;
    for (int dx = -4; dx <= 4; ++dx)
        for (int dy = -4; dy <= 4; ++dy)
        {
            const BWAPI::Position candidate = origin + BWAPI::Position(dx * 32, dy * 32);
            if ((!dx && !dy) || !candidate.isValid() ||
                target->getDistance(candidate) < 4 * 32 || target->getDistance(candidate) > range - 16 ||
                !the.partitions.connected(origin, candidate) ||
                !tankParkingTerrainIsClear(candidate) || tankParkingOccupiedByAnotherTank(candidate, tank)) continue;
            bool safe = true;
            for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(candidate, 4 * 32))
                if (enemy && enemy->exists() && enemy->getPlayer() == BWAPI::Broodwar->enemy() &&
                    !enemy->isFlying() && !enemy->getType().isBuilding() && UnitUtil::CanAttackGround(enemy))
                    safe = false;
            if (!safe) continue;
            const int score = candidate.getApproxDistance(origin) * 4 +
                std::max(0, tank->getDistance(target) - target->getDistance(candidate)) * 3;
            if (score < bestScore) { bestScore = score; best = candidate; }
        }
    return best;
}

static BWAPI::Unit getActiveWallBunker()
{
    if (!ProductionManager::Instance().terranMainWallGateActive()) return nullptr;

    // The first Command Center at our actual natural is the new front from
    // its first construction frame. Do not keep Tanks parked at the main-wall
    // bunker until the depot is complete: they need to cover the builder and
    // the unfinished base. A wall placed at the natural is already forward.
    Base* natural = the.bases.myNatural();
    if (!the.placer.terranMainWallCoversNatural() && natural &&
        natural->getOwner() == the.self())
    {
        BWAPI::Unit depot = natural->getDepot();
        if (depot && depot->exists() && !depot->isFlying() &&
            depot->getPlayer() == the.self() &&
            depot->getType() == BWAPI::UnitTypes::Terran_Command_Center)
        {
            return nullptr;
        }
    }

    const BWAPI::Position gate = the.placer.getTerranMainWallGateCenter();
    BWAPI::Unit best = nullptr;
    int bestDistance = 12 * 32 + 1;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isCompleted() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Bunker ||
            (unit->getTilePosition() != the.placer.getTerranMainWallBunkerTile() &&
             !the.placer.isInsideTerranMainWall(unit->getPosition(), 32))) continue;
        const int distance = unit->getDistance(gate);
        if (distance < bestDistance)
        {
            best = unit;
            bestDistance = distance;
        }
    }
    return best;
}

static bool wallTankPositionSafe(
    const BWAPI::Position& position, BWAPI::Unit tank,
    BWAPI::Unit bunker,
    const BWAPI::Position& gate, const BWAPI::Position& forward)
{
    if (!position.isValid() || !tankParkingTerrainIsClear(position) ||
        tankParkingOccupiedByAnotherTank(position, tank) ||
        !the.partitions.connected(tank->getPosition(), position) ||
        !the.placer.isInsideTerranMainWall(position, -16) ||
        positionBlocksDefensePassage(position, gate, forward)) return false;

    if (bunker)
    {
        const BWAPI::Position bunkerPos = bunker->getPosition();
        const double length = std::max(1.0, std::sqrt(
            double(forward.x) * forward.x + double(forward.y) * forward.y));
        const double fx = forward.x / length, fy = forward.y / length;
        const double facing =
            (position.x - bunkerPos.x) * fx +
            (position.y - bunkerPos.y) * fy;
        const BWAPI::Position attackerPos(
            int(bunkerPos.x + fx * 6 * 32),
            int(bunkerPos.y + fy * 6 * 32));
        const int siegeRange =
            BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();
        if (facing > -48.0 ||
            position.getApproxDistance(attackerPos) > siegeRange - 24)
        {
            return false;
        }
    }

    // Leave the entire landing footprint free even while the Barracks is up.
    const BWAPI::TilePosition tile = the.placer.getTerranMainWallBarracksTile();
    const BWAPI::UnitType body = BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
    if (position.x + body.dimensionRight() >= tile.x * 32 - 8 &&
        position.x - body.dimensionLeft() < (tile.x + 4) * 32 + 8 &&
        position.y + body.dimensionDown() >= tile.y * 32 - 8 &&
        position.y - body.dimensionUp() < (tile.y + 3) * 32 + 8) return false;

    for (BWAPI::Unit enemy : the.enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() || enemy->isFlying() ||
            !UnitUtil::CanAttackGround(enemy)) continue;
        // The later Siege command also checks predicted approach during the
        // transition. Never deliberately park inside present minimum range.
        if (enemy->getDistance(position) < 4 * 32) return false;
    }
    return true;
}

static BWAPI::Position findWallBunkerTankSlot(
    BWAPI::Unit tank, BWAPI::Unit bunker,
    const BWAPI::Position& gate, const BWAPI::Position& forward,
    const std::vector<BWAPI::Position>& reserved,
    const BWAPI::Position& rejected = BWAPI::Positions::Invalid)
{
    const BWAPI::UnitType body = BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
    const BWAPI::UnitType type = bunker->getType();
    const BWAPI::Position center = bunker->getPosition();
    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestScore = MAX_DISTANCE;

    // Three empty pixels between collision boxes in the first row. Further
    // rows add 64px only when the bunker perimeter cannot fit another Tank.
    for (int row = 0; row < 4; ++row)
    {
        const int gap = 3 + row * 64;
        const int left = center.x - type.dimensionLeft() - body.dimensionRight() - gap - 1;
        const int right = center.x + type.dimensionRight() + body.dimensionLeft() + gap + 1;
        const int top = center.y - type.dimensionUp() - body.dimensionDown() - gap - 1;
        const int bottom = center.y + type.dimensionDown() + body.dimensionUp() + gap + 1;
        auto consider = [&](int x, int y)
        {
            const BWAPI::Position candidate(x, y);
            if ((rejected.isValid() && candidate.getApproxDistance(rejected) < 40) ||
                candidate.getApproxDistance(gate) > 10 * 32 ||
                !wallTankPositionSafe(candidate, tank, bunker, gate, forward)) return;
            for (const BWAPI::Position& other : reserved)
                if (candidate.getApproxDistance(other) < 64) return;
            const int score = row * 2048 + candidate.getApproxDistance(gate) * 2 +
                candidate.getApproxDistance(tank->getPosition()) / 4;
            if (score < bestScore) { bestScore = score; best = candidate; }
        };
        for (int x = left; x <= right; x += 16) { consider(x, top); consider(x, bottom); }
        for (int y = top; y <= bottom; y += 16) { consider(left, y); consider(right, y); }
        if (best.isValid()) break;
    }
    return best;
}

static bool tankParkingMovementStalled(
    BWAPI::Unit tank,
    const BWAPI::Position& target,
    int now,
    BWAPI::Position& trackedTarget,
    int& bestDistance,
    int& lastProgressFrame,
    int arrivalRadius = 32)
{
    if (!tank || !tank->exists() || !target.isValid())
    {
        return false;
    }

    const int distance = tank->getPosition().getApproxDistance(target);
    if (!trackedTarget.isValid() ||
        trackedTarget.getApproxDistance(target) > 24 ||
        now < lastProgressFrame)
    {
        trackedTarget = target;
        bestDistance = distance;
        lastProgressFrame = now;
        return false;
    }

    // Ignore tiny pathing oscillations. Eight pixels is meaningful progress
    // for a Tank; anything smaller may just be collision separation.
    if (distance + 8 < bestDistance)
    {
        bestDistance = distance;
        lastProgressFrame = now;
    }

    if (distance <= arrivalRadius)
    {
        bestDistance = distance;
        lastProgressFrame = now;
        return false;
    }

    return now - lastProgressFrame >= 72;
}

static BWAPI::Position findPassageSafeTankPosition(
    const BWAPI::Position& desired,
    const BWAPI::Position& center,
    const BWAPI::Position& forward)
{
    if (!center.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position searchCenter = clampToMapForTanks(desired);

    const double dx = double(forward.x);
    const double dy = double(forward.y);
    const double length = std::sqrt(dx * dx + dy * dy);

    if (length < 1.0)
    {
        return findNearbyWalkableTankGatherPosition(searchCenter);
    }

    const double forwardX = dx / length;
    const double forwardY = dy / length;
    const double sideX = -forwardY;
    const double sideY = forwardX;

    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestScore = MAX_DISTANCE;

    // Search around the requested flank slot in a coordinate system aligned
    // with the passage. This makes a building near one flank move the tank
    // farther sideways/backward instead of snapping it into the middle lane.
    for (int sideStep = -4; sideStep <= 4; ++sideStep)
    {
        for (int forwardStep = -4; forwardStep <= 4; ++forwardStep)
        {
            BWAPI::Position candidate(
                searchCenter.x + int(sideX * sideStep * 32 + forwardX * forwardStep * 32),
                searchCenter.y + int(sideY * sideStep * 32 + forwardY * forwardStep * 32)
            );

            candidate = clampToMapForTanks(candidate);

            if (positionBlocksDefensePassage(candidate, center, forward) ||
                !tankParkingTerrainIsClear(candidate) ||
                !the.partitions.connected(center, candidate))
            {
                continue;
            }

            const int room = the.tileRoom.at(BWAPI::TilePosition(candidate));
            const int distance = candidate.getApproxDistance(searchCenter);
            const int narrowPenalty = room <= 12 ? 4 * 32 : 0;
            const int score = distance + narrowPenalty;

            if (score < bestScore)
            {
                bestScore = score;
                best = candidate;
            }
        }
    }

    return best;
}

static bool enemyTankFieldTooDangerousForSoloAdvance(
    BWAPI::Unit tank,
    const BWAPI::Unitset& allAttackTanks,
    const BWAPI::Position& moveGoal)
{
    if (!tank || !tank->exists() || !moveGoal.isValid())
    {
        return false;
    }

    const int enemyTankSearchRadius = 13 * 32;
    const int friendlySupportRadius = 9 * 32;

    const int enemyTanksNearGoal =
        countVisibleEnemyTanksNear(moveGoal, enemyTankSearchRadius);

    if (enemyTanksNearGoal < 3)
    {
        return false;
    }

    // A dangerous destination does not mean this reinforcement is already
    // in danger. Sieging at a distant waypoint caused an endless
    // siege -> no local target -> unsiege -> siege cycle.
    bool localTankContact = tank->isUnderAttack();
    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (isEnemyTerranTank(enemy) && targetInTankSiegeRange(tank, enemy))
        {
            localTankContact = true;
            break;
        }
    }
    if (!localTankContact)
    {
        return false;
    }

    const int friendlyTanksNearTank =
        countFriendlyTanksNear(
            allAttackTanks,
            tank->getPosition(),
            friendlySupportRadius
        );

    return friendlyTanksNearTank < 3;
}

static bool handleSoloTankAvoidEnemyTankField(
    BWAPI::Unit tank,
    const BWAPI::Unitset& allAttackTanks,
    const BWAPI::Position& moveGoal)
{
    if (!enemyTankFieldTooDangerousForSoloAdvance(tank, allAttackTanks, moveGoal))
    {
        return false;
    }

    const BWAPI::Position enemyTankCenter =
        getVisibleEnemyTankClusterCenterNear(moveGoal, 13 * 32);

    if (tank->isSieged())
    {
        the.micro.HoldPosition(tank);
        return true;
    }

    if (tank->canSiege())
    {
        the.micro.Siege(tank);
        return true;
    }

    if (enemyTankCenter.isValid())
    {
        BWAPI::Position retreatPos =
            positionAwayFrom(tank->getPosition(), enemyTankCenter, 4 * 32);

        the.micro.Move(tank, retreatPos);
        return true;
    }

    the.micro.HoldPosition(tank);
    return true;
}

static int tankEnemyThreatScore(BWAPI::Unit enemy)
{
    if (!enemy || !enemy->exists() || enemy->isFlying())
    {
        return 0;
    }

    const BWAPI::UnitType type = enemy->getType();

    if (type.isWorker() || type.isSpell())
    {
        return 0;
    }

    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) return 6;
    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)  return 5;
    if (type == BWAPI::UnitTypes::Protoss_Reaver)               return 5;
    if (type == BWAPI::UnitTypes::Protoss_Dragoon)              return 3;
    if (type == BWAPI::UnitTypes::Protoss_Archon)               return 4;
    if (type == BWAPI::UnitTypes::Protoss_Dark_Templar)         return 4;
    if (type == BWAPI::UnitTypes::Terran_Goliath)               return 3;
    if (type == BWAPI::UnitTypes::Terran_Vulture)               return 2;
    if (type == BWAPI::UnitTypes::Zerg_Lurker)                  return 4;
    if (type == BWAPI::UnitTypes::Zerg_Ultralisk)               return 5;
    if (type == BWAPI::UnitTypes::Zerg_Hydralisk)               return 2;
    if (type == BWAPI::UnitTypes::Protoss_Zealot)               return 2;
    if (type == BWAPI::UnitTypes::Zerg_Zergling)                return 1;
    if (type == BWAPI::UnitTypes::Terran_Marine)                return 1;

    if (type.isBuilding())
    {
        if (UnitUtil::CanAttackGround(enemy))
        {
            return 3;
        }

        return 0;
    }

    if (UnitUtil::CanAttackGround(enemy))
    {
        return 1;
    }

    return 0;
}

static int countSiegedTanksNear(
    BWAPI::Unit centerTank,
    const BWAPI::Unitset& tanks,
    int radius)
{
    if (!centerTank || !centerTank->exists())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit tank : tanks)
    {
        if (!tank || !tank->exists())
        {
            continue;
        }

        if (tank->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            continue;
        }

        if (centerTank->getDistance(tank) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static int countNearbyTanksForMemorySiege(
    BWAPI::Unit tank,
    const BWAPI::Unitset& tanks,
    int radius)
{
    if (!tank || !tank->exists())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit other : tanks)
    {
        if (!other || !other->exists() || other == tank)
        {
            continue;
        }

        if (other->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
            other->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            continue;
        }

        if (tank->getDistance(other) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static BWAPI::Position positionTowardForTankMemory(
    const BWAPI::Position& from,
    const BWAPI::Position& to,
    int distance)
{
    if (!from.isValid() || !to.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const double dx = double(to.x - from.x);
    const double dy = double(to.y - from.y);
    const double len = std::sqrt(dx * dx + dy * dy);

    if (len < 1.0)
    {
        return from;
    }

    BWAPI::Position result(
        from.x + int(dx / len * distance),
        from.y + int(dy / len * distance)
    );

    if (result.isValid())
    {
        return result;
    }

    return BWAPI::Positions::Invalid;
}

static bool isEnemyTankTypeForMemory(BWAPI::UnitType type)
{
    return
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
}

static void updateRememberedEnemyTanks()
{
    const int now = BWAPI::Broodwar->getFrameCount();
    if (now == lastRememberedEnemyTankUpdateFrame)
    {
        return;
    }
    if (now < lastRememberedEnemyTankUpdateFrame)
    {
        rememberedEnemyTanks.clear();
    }
    lastRememberedEnemyTankUpdateFrame = now;

    // Observe all legitimately visible enemy Tanks, including ones spotted by
    // another unit beyond this Tank's own sight range or cluster.
    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            !enemy->getPosition().isValid() ||
            !isEnemyTankTypeForMemory(enemy->getType()))
        {
            continue;
        }

        RememberedEnemyTank& memory = rememberedEnemyTanks[enemy->getID()];
        memory.position = enemy->getPosition();
        memory.lastSeenFrame = now;
        memory.wasSieged =
            enemy->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
            enemy->getOrder() == BWAPI::Orders::Sieging;
    }

    for (auto it = rememberedEnemyTanks.begin(); it != rememberedEnemyTanks.end(); )
    {
        const RememberedEnemyTank& memory = it->second;
        bool erase =
            !memory.position.isValid() ||
            now < memory.lastSeenFrame ||
            now - memory.lastSeenFrame > REMEMBERED_ENEMY_TANK_FRAMES;

        if (!erase)
        {
            bool footprintVisible = true;
            for (int dx : { -20, 20 })
            {
                for (int dy : { -20, 20 })
                {
                    const BWAPI::TilePosition tile(memory.position + BWAPI::Position(dx, dy));
                    footprintVisible = footprintVisible && tile.isValid() &&
                        BWAPI::Broodwar->isVisible(tile);
                }
            }
            if (memory.lastSeenFrame != now && footprintVisible)
            {
                bool tankStillThere = false;
                for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
                {
                    if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                        !enemy->getPosition().isValid() ||
                        !isEnemyTankTypeForMemory(enemy->getType()))
                    {
                        continue;
                    }

                    if (enemy->getDistance(memory.position) <= 64)
                    {
                        tankStillThere = true;
                        break;
                    }
                }

                if (!tankStillThere)
                {
                    erase = true;
                }
            }
        }

        if (erase)
        {
            it = rememberedEnemyTanks.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

static int rememberedEnemyTankCountNear(const BWAPI::Position& center, int radius)
{
    if (!center.isValid())
    {
        return 0;
    }

    int count = 0;
    for (const auto& entry : rememberedEnemyTanks)
    {
        if (entry.second.position.isValid() &&
            entry.second.position.getApproxDistance(center) <= radius)
        {
            ++count;
        }
    }
    return count;
}

static int rememberedEnemySiegedTankCountNear(const BWAPI::Position& center, int radius)
{
    if (!center.isValid())
    {
        return 0;
    }

    int count = 0;
    for (const auto& entry : rememberedEnemyTanks)
    {
        if (entry.second.wasSieged && entry.second.position.isValid() &&
            entry.second.position.getApproxDistance(center) <= radius)
        {
            ++count;
        }
    }
    return count;
}

static BWAPI::Position rememberedEnemyTankCenterNear(
    const BWAPI::Position& center, int radius)
{
    long long sumX = 0;
    long long sumY = 0;
    int count = 0;

    for (const auto& entry : rememberedEnemyTanks)
    {
        const BWAPI::Position pos = entry.second.position;
        if (!pos.isValid())
        {
            continue;
        }
        if (center.isValid() && pos.getApproxDistance(center) > radius)
        {
            continue;
        }
        sumX += pos.x;
        sumY += pos.y;
        ++count;
    }

    return count > 0
        ? BWAPI::Position(int(sumX / count), int(sumY / count))
        : BWAPI::Positions::Invalid;
}

static bool rememberedEnemyTankThreatForTank(BWAPI::Unit tank, int radius = 16 * 32)
{
    if (!tank || !tank->exists() || !tank->getPosition().isValid())
    {
        return false;
    }
    return rememberedEnemyTankCountNear(tank->getPosition(), radius) > 0;
}

static void refreshTankGroupMemoryFromRememberedTanks(const BWAPI::Unitset& tanks)
{
    BWAPI::Position reference = getFriendlyTankGroupCenter(tanks);
    const BWAPI::Position center = rememberedEnemyTankCenterNear(reference, 18 * 32);
    if (!center.isValid())
    {
        return;
    }

    int newestFrame = -100000;
    int count = 0;
    for (const auto& entry : rememberedEnemyTanks)
    {
        if (entry.second.position.isValid() &&
            entry.second.position.getApproxDistance(center) <= 12 * 32)
        {
            newestFrame = std::max(newestFrame, entry.second.lastSeenFrame);
            ++count;
        }
    }

    if (count > 0)
    {
        lastTankGroupEnemyCenter = center;
        lastTankGroupEnemySeenFrame = newestFrame;
        lastTankGroupEnemyThreatScore = std::max(lastTankGroupEnemyThreatScore, count * 5);
    }
}

static void updateTankGroupEnemyMemory(
    const BWAPI::Unitset& tanks,
    const BWAPI::Unitset& tankTargets,
    int siegeTankRange)
{
    if (tanks.empty() || tankTargets.empty())
    {
        return;
    }

    int sumX = 0;
    int sumY = 0;
    int count = 0;
    int threatScore = 0;

    for (BWAPI::Unit enemy : tankTargets)
    {
        if (!enemy ||
            !enemy->exists() ||
            !enemy->isVisible() ||
            enemy->isFlying() ||
            !enemy->getPosition().isValid())
        {
            continue;
        }

        // Static defenses are handled by their actual firing envelope. A
        // Cannon outside our range must not keep refreshing a mobile-army
        // memory lease and pin the whole line at the same spot forever.
        if (enemy->getType().isBuilding()) continue;
        const int enemyScore = tankEnemyThreatScore(enemy);

        if (enemyScore <= 0)
        {
            continue;
        }

        bool closeToAnyTank = false;

        for (BWAPI::Unit tank : tanks)
        {
            if (!tank || !tank->exists())
            {
                continue;
            }

            if (tank->getDistance(enemy) <= siegeTankRange + 5 * 32)
            {
                closeToAnyTank = true;
                break;
            }
        }

        if (!closeToAnyTank)
        {
            continue;
        }

        sumX += enemy->getPosition().x;
        sumY += enemy->getPosition().y;
        ++count;
        threatScore += enemyScore;
    }

    if (count <= 0)
    {
        return;
    }

    lastTankGroupEnemySeenFrame = BWAPI::Broodwar->getFrameCount();
    lastTankGroupEnemyCenter = BWAPI::Position(sumX / count, sumY / count);
    lastTankGroupEnemyThreatScore = threatScore;

    if (Config::Debug::DrawUnitTargets)
    {
        BWAPI::Broodwar->drawCircleMap(
            lastTankGroupEnemyCenter,
            36,
            BWAPI::Colors::Red,
            false
        );
    }

    DrawUnitTargetTextMap(
        lastTankGroupEnemyCenter + BWAPI::Position(8, 0),
        "tank memory %d",
        threatScore
    );
}

static bool tankGroupEnemyMemoryActive(const BWAPI::Unitset& tanks)
{
    const int now = BWAPI::Broodwar->getFrameCount();

    if (!lastTankGroupEnemyCenter.isValid())
    {
        return false;
    }

    const int memoryFrames = rememberedEnemyTanks.empty()
        ? 24 * 5
        : REMEMBERED_ENEMY_TANK_FRAMES;
    if (now - lastTankGroupEnemySeenFrame > memoryFrames)
    {
        return false;
    }

    // Generic ground-contact memory should behave like the Terran Tank line
    // against every race. A single Dragoon/Hydralisk/Zealot or static ground
    // defense is enough; ignore only a lone 1-point Marine/Zergling nuisance.
    if (lastTankGroupEnemyThreatScore < 2)
    {
        return false;
    }

    for (BWAPI::Unit tank : tanks)
    {
        if (!tank || !tank->exists() || !tank->getPosition().isValid())
        {
            continue;
        }

        if (tank->getDistance(lastTankGroupEnemyCenter) <= 16 * 32)
        {
            return true;
        }
    }

    return false;
}


static bool tryComsatScoutTankEnemyMemory(
    const BWAPI::Unitset& tanks,
    const BWAPI::Position& memoryCenter)
{
    if (!memoryCenter.isValid() || tanks.size() < 4)
    {
        return false;
    }

    const BWAPI::TilePosition memoryTile(memoryCenter);
    if (!memoryTile.isValid() || BWAPI::Broodwar->isVisible(memoryTile) ||
        MapGrid::Instance().scanIsActiveAt(memoryCenter))
    {
        return false;
    }

    const int now = BWAPI::Broodwar->getFrameCount();
    const int nearRequired = std::max(3, std::min(8, int(tanks.size() + 2) / 3));
    int tanksNearFight = 0;
    for (BWAPI::Unit tank : tanks)
    {
        if (tank && tank->exists() &&
            tank->getDistance(memoryCenter) <= 12 * 32)
        {
            ++tanksNearFight;
        }
    }
    if (tanksNearFight < nearRequired ||
        now - lastTankGroupEnemySeenFrame > 8 * 24)
    {
        return false;
    }

    if (now - lastTankMemoryComsatScanFrame < 4 * 24)
    {
        return false;
    }

    const bool tankMirrorVision =
        BWAPI::Broodwar->enemy()->getRace() == BWAPI::Races::Terran;

    if (CombatCommander::Instance().requestComsatScan(
            memoryCenter,
            tankMirrorVision
                ? ComsatScanPriority::TankBattle
                : ComsatScanPriority::ArmyVision,
            tankMirrorVision ? 50 : 100,
            tankMirrorVision ? 0 : 50,
            5 * 24,
            tankMirrorVision ? "tank battle" : "tank line vision"))
    {
        lastTankMemoryComsatScanFrame = now;
        DrawUnitTargetTextMap(
            memoryCenter + BWAPI::Position(8, 12),
            "tank line COMSAT");
        return true;
    }

    return false;
}

// An offensive tank line can reach a fogged enemy base without ever seeing a
// defender. The ordinary army-vision scan only follows recent enemy memory,
// while Tanks deliberately do not scout into fog themselves. Give a stalled,
// supported line one scan ahead instead of waiting for strategic scouting.
static void tryComsatScoutStalledTankFront(
    const BWAPI::Unitset& tanks,
    const BWAPI::Position& front,
    const BWAPI::Position& goal,
    bool hasVisibleTargets)
{
    const int now = BWAPI::Broodwar->getFrameCount();
    if (!front.isValid() || !goal.isValid() || tanks.size() < 6 ||
        hasVisibleTargets ||
        front.getApproxDistance(goal) > 20 * 32)
    {
        lastTankFogFront = BWAPI::Positions::Invalid;
        lastTankFogGoal = BWAPI::Positions::Invalid;
        lastTankFogFrontMoveFrame = now;
        return;
    }

    if (!lastTankFogFront.isValid() || !lastTankFogGoal.isValid() ||
        lastTankFogGoal.getApproxDistance(goal) > 4 * 32 ||
        now < lastTankFogFrontMoveFrame ||
        lastTankFogFront.getApproxDistance(front) >= 2 * 32)
    {
        lastTankFogFront = front;
        lastTankFogGoal = goal;
        lastTankFogFrontMoveFrame = now;
        return;
    }

    if (now - lastTankFogFrontMoveFrame < 10 * 24 ||
        now - lastTankFogScanFrame < 30 * 24 ||
        the.my.completed.count(BWAPI::UnitTypes::Terran_Comsat_Station) == 0)
    {
        return;
    }

    int tanksNearFront = 0;
    for (BWAPI::Unit tank : tanks)
    {
        if (tank && tank->exists() && tank->getDistance(front) <= 9 * 32)
        {
            ++tanksNearFront;
        }
    }
    if (tanksNearFront < 5)
    {
        return;
    }

    BWAPI::Position scanPosition = goal;
    if (front.getApproxDistance(goal) > 14 * 32)
    {
        const double dx = double(goal.x - front.x);
        const double dy = double(goal.y - front.y);
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (distance < 1.0)
        {
            return;
        }
        scanPosition = front + BWAPI::Position(
            int(dx * 10 * 32 / distance),
            int(dy * 10 * 32 / distance));
    }

    const BWAPI::TilePosition scanTile(scanPosition);
    if (!scanTile.isValid() || BWAPI::Broodwar->isVisible(scanTile) ||
        MapGrid::Instance().scanIsActiveAt(scanPosition) ||
        CombatCommander::Instance().comsatScanActiveOrPending(scanPosition, 5 * 32))
    {
        return;
    }

    // Keep one charge for detection when enemy cloak tech is known.
    const bool cloakThreat = the.info.enemyHasCloakTech();
    if (CombatCommander::Instance().requestComsatScan(
            scanPosition, ComsatScanPriority::ArmyVision,
            cloakThreat ? 100 : 50,
            cloakThreat ? 50 : 0,
            4 * 24, "stalled tank front"))
    {
        lastTankFogScanFrame = now;
    }
}

static bool tankShouldSiegeOnEnemyMemory(
    BWAPI::Unit tank,
    const BWAPI::Unitset& tanks)
{
    if (!tank || !tank->exists())
    {
        return false;
    }

    if (!lastTankGroupEnemyCenter.isValid())
    {
        return false;
    }

    const int nearbySupport =
        countNearbyTanksForMemorySiege(tank, tanks, 9 * 32);

    const bool hasSiegedSupportNearby =
        nearbySupport >= 2 ||
        countNearbyTanksForMemorySiege(tank, tanks, 6 * 32) >= 1;

    if (!hasSiegedSupportNearby && tanks.size() >= 4)
    {
        return false;
    }

    if (tank->getDistance(lastTankGroupEnemyCenter) > 14 * 32)
    {
        return false;
    }

    return true;
}

void MicroTanks::refreshCombatState(
    const BWAPI::Unitset& tanks,
    const BWAPI::Unitset& targets)
{
    const int now = BWAPI::Broodwar->getFrameCount();
    const int range =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();

    updateRememberedEnemyTanks();

    for (BWAPI::Unit tank : tanks)
    {
        if (!tank || !tank->exists())
        {
            continue;
        }
        TankDecisionState& state = stateFor(tank);
        if (now < state.lastCombatStateFrame)
        {
            state = TankDecisionState();
        }
        if (state.lastCombatStateFrame == now)
        {
            continue;
        }
        state.lastCombatStateFrame = now;

        bool observedCombat = tank->isUnderAttack();
        for (BWAPI::Unit enemy : targets)
        {
            if (visibleTankGroundTarget(enemy) &&
                ((enemy->getType().isBuilding() || enemy->getType().isWorker())
                    ? targetInTankSiegeRange(tank, enemy)
                    : tank->getDistance(enemy) <= range + 48))
            {
                observedCombat = true;
                break;
            }
        }
        if (observedCombat)
        {
            state.lastSawEnemyFrame = now;
            state.holdRecentEnemyUntil = now + 4 * 24;
        }

        if (tank->isSieged() && !state.wasSiegedLastFrame)
        {
            state.lastSiegeFrame = now;
            state.holdSiegeUntil = std::max(state.holdSiegeUntil, now + 4 * 24);
        }
        state.wasSiegedLastFrame = tank->isSieged();

        int holdUntil = std::max(state.holdRecentEnemyUntil, state.holdSiegeUntil);
        for (const auto& entry : rememberedEnemyTanks)
        {
            const RememberedEnemyTank& enemy = entry.second;
            if (enemy.wasSieged && enemy.position.isValid() &&
                tank->getDistance(enemy.position) <= range + 3 * 32)
            {
                holdUntil = std::max(holdUntil,
                    enemy.lastSeenFrame + REMEMBERED_ENEMY_TANK_FRAMES);
            }
        }
        // Replace the lease, rather than extending it blindly: a confirmed
        // empty enemy position must release the long fog-of-war hold.
        the.micro.setSiegeHoldUntil(tank, holdUntil);
    }
}

static bool canStartTankScoutStep(
    BWAPI::Unit tank,
    const BWAPI::Unitset& tanks,
    const BWAPI::Unitset& tankTargets,
    bool defendingAttackedBase)
{
    if (!tank || !tank->exists() ||
        defendingAttackedBase ||
        !tankTargets.empty() ||
        !tankGroupEnemyMemoryActive(tanks))
    {
        return false;
    }

    // Safety policy: after any remembered ground contact, a Siege Tank never
    // becomes the vision scout itself. The existing active-scout machinery is
    // intentionally kept for compatibility, but new scout leases are blocked;
    // the nearby Vulture screen and Comsat provide vision instead.
    return false;
}

static bool isActiveTankScout(BWAPI::Unit tank)
{
    if (!tank || !tank->exists())
    {
        return false;
    }

    const int now = BWAPI::Broodwar->getFrameCount();

    return
        tank->getID() == activeScoutTankID &&
        now < activeScoutStepUntil;
}

static int enemyGroundThreatScoreNearTank(
    BWAPI::Unit tank,
    const BWAPI::Unitset& targets,
    int radius)
{
    if (!tank || !tank->exists())
    {
        return 0;
    }

    int score = 0;

    for (BWAPI::Unit enemy : targets)
    {
        if (!enemy || !enemy->exists() || enemy->isFlying())
        {
            continue;
        }

        const BWAPI::UnitType type = enemy->getType();

        if (type.isWorker() || type.isSpell())
        {
            continue;
        }

        if (tank->getDistance(enemy) > radius)
        {
            continue;
        }

        if (!UnitUtil::CanAttackGround(enemy) && !type.isBuilding())
        {
            continue;
        }

        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) score += 6;
        else if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) score += 5;
        else if (type == BWAPI::UnitTypes::Protoss_Reaver) score += 5;
        else if (type == BWAPI::UnitTypes::Protoss_Dragoon) score += 3;
        else if (type == BWAPI::UnitTypes::Terran_Goliath) score += 3;
        else if (type == BWAPI::UnitTypes::Zerg_Lurker) score += 4;
        else if (type == BWAPI::UnitTypes::Zerg_Hydralisk) score += 2;
        else if (type == BWAPI::UnitTypes::Protoss_Zealot) score += 2;
        else if (type == BWAPI::UnitTypes::Zerg_Zergling) score += 1;
        else if (type == BWAPI::UnitTypes::Terran_Marine) score += 1;
        else if (type == BWAPI::UnitTypes::Terran_Vulture) score += 2;
        else if (type.isBuilding() && UnitUtil::CanAttackGround(enemy)) score += 3;
        else if (!type.isBuilding()) score += 1;
    }

    return score;
}

static int friendlyTankSupportScoreNear(
    BWAPI::Unit tank,
    int radius)
{
    if (!tank || !tank->exists())
    {
        return 0;
    }

    int score = 0;

    for (BWAPI::Unit friendly : BWAPI::Broodwar->self()->getUnits())
    {
        if (!friendly || !friendly->exists() || friendly == tank)
        {
            continue;
        }

        if (!friendly->isCompleted())
        {
            continue;
        }

        if (friendly->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
            friendly->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            continue;
        }

        if (tank->getDistance(friendly) > radius)
        {
            continue;
        }

        if (friendly->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            score += 2;
        }
        else
        {
            score += 1;
        }
    }

    return score;
}

static bool enemyTooCloseForTankRetreat(
    BWAPI::Unit tank,
    const BWAPI::Unitset& targets)
{
    if (!tank || !tank->exists())
    {
        return false;
    }

    for (BWAPI::Unit enemy : targets)
    {
        if (!enemy || !enemy->exists() || enemy->isFlying())
        {
            continue;
        }

        if (enemy->getType().isBuilding())
        {
            continue;
        }

        if (!UnitUtil::CanAttackGround(enemy))
        {
            continue;
        }

        if (tank->getDistance(enemy) <= 4 * 32)
        {
            return true;
        }
    }

    return false;
}

static BWAPI::Position getEnemyPressureCenter(
    BWAPI::Unit tank,
    const BWAPI::Unitset& targets,
    int radius)
{
    if (!tank || !tank->exists())
    {
        return BWAPI::Positions::Invalid;
    }

    int sumX = 0;
    int sumY = 0;
    int count = 0;

    for (BWAPI::Unit enemy : targets)
    {
        if (!enemy || !enemy->exists() || enemy->isFlying())
        {
            continue;
        }

        if (!enemy->getPosition().isValid())
        {
            continue;
        }

        if (tank->getDistance(enemy) > radius)
        {
            continue;
        }

        if (!UnitUtil::CanAttackGround(enemy) && !enemy->getType().isBuilding())
        {
            continue;
        }

        sumX += enemy->getPosition().x;
        sumY += enemy->getPosition().y;
        ++count;
    }

    if (count <= 0)
    {
        return BWAPI::Positions::Invalid;
    }

    return BWAPI::Position(sumX / count, sumY / count);
}

static BWAPI::Position getTankRetreatPosition(
    BWAPI::Unit tank,
    const BWAPI::Unitset& targets,
    const BWAPI::Position& fallbackPosition)
{
    if (!tank || !tank->exists())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position tankPos = tank->getPosition();

    BWAPI::Position enemyCenter =
        getEnemyPressureCenter(tank, targets, 13 * 32);

    if (!enemyCenter.isValid())
    {
        return fallbackPosition;
    }

    double awayX = double(tankPos.x - enemyCenter.x);
    double awayY = double(tankPos.y - enemyCenter.y);
    double awayLen = std::sqrt(awayX * awayX + awayY * awayY);

    if (awayLen < 1.0)
    {
        awayX = 1.0;
        awayY = 0.0;
        awayLen = 1.0;
    }

    awayX /= awayLen;
    awayY /= awayLen;

    double homeX = 0.0;
    double homeY = 0.0;

    if (fallbackPosition.isValid())
    {
        homeX = double(fallbackPosition.x - tankPos.x);
        homeY = double(fallbackPosition.y - tankPos.y);

        const double homeLen = std::sqrt(homeX * homeX + homeY * homeY);

        if (homeLen > 1.0)
        {
            homeX /= homeLen;
            homeY /= homeLen;
        }
        else
        {
            homeX = 0.0;
            homeY = 0.0;
        }
    }

    double finalX = awayX * 0.75 + homeX * 0.25;
    double finalY = awayY * 0.75 + homeY * 0.25;

    const double finalLen = std::sqrt(finalX * finalX + finalY * finalY);

    if (finalLen < 1.0)
    {
        return fallbackPosition;
    }

    finalX /= finalLen;
    finalY /= finalLen;

    const int retreatDistance = 6 * 32;

    BWAPI::Position retreatPosition(
        int(tankPos.x + finalX * retreatDistance),
        int(tankPos.y + finalY * retreatDistance)
    );

    if (retreatPosition.isValid())
    {
        return retreatPosition;
    }

    return fallbackPosition;
}

// Point-blank danger is different from ordinary target priority. A Zealot or
// Zergling inside Siege minimum range must not be ignored merely because a
// farther Dragoon/Tank has a higher attack score. Workers only qualify when
// they are actually attacking a damaged Tank or arrive as a group, so one
// harmless scout cannot drag the whole Tank line around the map.
static BWAPI::Unit getClosestPointBlankTankThreat(
    BWAPI::Unit tank,
    const BWAPI::Unitset& targets,
    int emergencyDistance)
{
    if (!tank || !tank->exists())
    {
        return nullptr;
    }

    BWAPI::Unit closestCombatThreat = nullptr;
    BWAPI::Unit closestWorkerThreat = nullptr;
    int closestCombatDistance = MAX_DISTANCE;
    int closestWorkerDistance = MAX_DISTANCE;
    int closeWorkers = 0;

    for (BWAPI::Unit enemy : targets)
    {
        if (!visibleTankGroundTarget(enemy) ||
            enemy->getType().isBuilding() ||
            !UnitUtil::CanAttackGround(enemy))
        {
            continue;
        }

        const int distance = tank->getDistance(enemy);
        if (distance >= emergencyDistance)
        {
            continue;
        }

        if (enemy->getType().isWorker())
        {
            ++closeWorkers;
            if (distance < closestWorkerDistance)
            {
                closestWorkerDistance = distance;
                closestWorkerThreat = enemy;
            }
        }
        else if (distance < closestCombatDistance)
        {
            closestCombatDistance = distance;
            closestCombatThreat = enemy;
        }
    }

    if (closestCombatThreat)
    {
        return closestCombatThreat;
    }

    const bool workerDanger =
        closeWorkers >= 2 ||
        tank->isUnderAttack() ||
        tank->getHitPoints() <= tank->getType().maxHitPoints() / 2;

    return workerDanger ? closestWorkerThreat : nullptr;
}

static BWAPI::Position getPointBlankTankRetreatPosition(
    BWAPI::Unit tank,
    BWAPI::Unit threat,
    const BWAPI::Position& fallbackPosition)
{
    if (!tank || !tank->exists() ||
        !threat || !threat->exists() ||
        !tank->getPosition().isValid() ||
        !threat->getPosition().isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position tankPosition = tank->getPosition();

    double awayX = double(tankPosition.x - threat->getPosition().x);
    double awayY = double(tankPosition.y - threat->getPosition().y);
    double awayLength = std::hypot(awayX, awayY);

    if (awayLength < 1.0)
    {
        if (fallbackPosition.isValid())
        {
            awayX = double(fallbackPosition.x - tankPosition.x);
            awayY = double(fallbackPosition.y - tankPosition.y);
            awayLength = std::hypot(awayX, awayY);
        }

        if (awayLength < 1.0)
        {
            awayX = (tank->getID() & 1) ? 1.0 : -1.0;
            awayY = 0.0;
            awayLength = 1.0;
        }
    }

    awayX /= awayLength;
    awayY /= awayLength;

    double homeX = 0.0;
    double homeY = 0.0;
    if (fallbackPosition.isValid())
    {
        homeX = double(fallbackPosition.x - tankPosition.x);
        homeY = double(fallbackPosition.y - tankPosition.y);
        const double homeLength = std::hypot(homeX, homeY);
        if (homeLength > 1.0)
        {
            homeX /= homeLength;
            homeY /= homeLength;
        }
        else
        {
            homeX = 0.0;
            homeY = 0.0;
        }
    }

    double retreatX = awayX * 0.85 + homeX * 0.15;
    double retreatY = awayY * 0.85 + homeY * 0.15;
    const double retreatLength = std::hypot(retreatX, retreatY);
    if (retreatLength < 1.0)
    {
        retreatX = awayX;
        retreatY = awayY;
    }
    else
    {
        retreatX /= retreatLength;
        retreatY /= retreatLength;
    }

    const int BACKSTEP_DISTANCE = 4 * 32;
    const BWAPI::Position desired = clampToMapForTanks(BWAPI::Position(
        int(tankPosition.x + retreatX * BACKSTEP_DISTANCE),
        int(tankPosition.y + retreatY * BACKSTEP_DISTANCE)));

    const BWAPI::Position walkable =
        findNearbyWalkableTankGatherPosition(desired);

    if (walkable.isValid() &&
        isWalkableTankGatherPosition(walkable) &&
        the.partitions.connected(tankPosition, walkable))
    {
        return walkable;
    }

    // Native StarCraft pathing can still route around a temporary unit wall;
    // static-component validation above merely prevents an impossible order.
    return the.partitions.connected(tankPosition, desired)
        ? desired
        : fallbackPosition;
}

static bool tankShouldRetreat(
    BWAPI::Unit tank,
    const BWAPI::Unitset& targets,
    bool defendingAttackedBase)
{
    if (!tank || !tank->exists())
    {
        return false;
    }

    if (targets.empty())
    {
        return false;
    }

    const int hp = tank->getHitPoints();
    const int maxHp = tank->getType().maxHitPoints();

    const int enemyScore =
        enemyGroundThreatScoreNearTank(tank, targets, 12 * 32);

    const int supportScore =
        friendlyTankSupportScoreNear(tank, 8 * 32);

    const bool enemyTooClose =
        enemyTooCloseForTankRetreat(tank, targets);

    if (defendingAttackedBase)
    {
        return
            hp <= maxHp / 3 &&
            enemyScore >= 4 &&
            supportScore <= 1;
    }

    if (hp <= maxHp / 3 && enemyScore >= 2 &&
        (tank->isUnderAttack() || enemyTooClose))
    {
        return true;
    }

    if (tank->isSieged() && !tank->isUnderDisruptionWeb())
    {
        for (BWAPI::Unit enemy : targets)
        {
            if (targetInTankSiegeRange(tank, enemy))
            {
                // An established gun line should keep firing. The general
                // support score is for deciding whether a mobile Tank advances.
                return false;
            }
        }
    }

    if (hp <= maxHp / 2 && enemyScore >= 5 && supportScore <= 1)
    {
        return true;
    }

    if (enemyTooClose && supportScore <= 1)
    {
        return true;
    }
    if (enemyScore >= 8 && supportScore <= 2)
    {
        return true;
    }

    return false;
}


static int countNearbyFriendlyTanks(
    BWAPI::Unit tank,
    const BWAPI::Unitset& allAttackTanks,
    int radius)
{
    if (!tank || !tank->exists())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit other : allAttackTanks)
    {
        if (!other || !other->exists() || other == tank)
        {
            continue;
        }

        if (other->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
            other->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            continue;
        }

        if (tank->getDistance(other) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static BWAPI::Unit getClosestOtherAttackTank(
    BWAPI::Unit tank,
    const BWAPI::Unitset& allAttackTanks)
{
    if (!tank || !tank->exists())
    {
        return nullptr;
    }

    BWAPI::Unit closest = nullptr;
    int closestDistance = MAX_DISTANCE;

    for (BWAPI::Unit other : allAttackTanks)
    {
        if (!other || !other->exists() || other == tank)
        {
            continue;
        }

        if (other->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
            other->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            continue;
        }

        const int distance = tank->getDistance(other);

        if (distance < closestDistance)
        {
            closestDistance = distance;
            closest = other;
        }
    }

    return closest;
}

static BWAPI::Position getRegroupPositionNearClosestTank(
    BWAPI::Unit tank,
    const BWAPI::Unitset& allAttackTanks,
    const BWAPI::Position& enemyGoal)
{
    BWAPI::Unit closestTank =
        getClosestOtherAttackTank(tank, allAttackTanks);

    if (!closestTank || !closestTank->exists())
    {
        return BWAPI::Positions::Invalid;
    }

    return getJoinPositionNearForwardTank(
        tank,
        closestTank,
        enemyGoal
    );
}

static bool forwardTankShouldWaitForSupport(
    BWAPI::Unit tank,
    const BWAPI::Unitset& allAttackTanks,
    const BWAPI::Unitset& targets,
    bool defendingAttackedBase)
{
    if (!tank || !tank->exists())
    {
        return false;
    }

    if (defendingAttackedBase)
    {
        return false;
    }

    if (allAttackTanks.size() < 4)
    {
        return false;
    }

    if (tankHasDangerouslyCloseEnemy(tank, targets))
    {
        return false;
    }

    const int nearbySupport =
        countNearbyFriendlyTanks(tank, allAttackTanks, 8 * 32);

    if (nearbySupport >= 2)
    {
        return false;
    }

    BWAPI::Unit closestTank =
        getClosestOtherAttackTank(tank, allAttackTanks);

    if (!closestTank || !closestTank->exists())
    {
        return false;
    }

    return tank->getDistance(closestTank) > 10 * 32;
}

static int countFriendlyAttackTanksNearPosition(
    const BWAPI::Unitset& allAttackTanks,
    const BWAPI::Position& pos,
    int radius)
{
    if (!pos.isValid())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit tank : allAttackTanks)
    {
        if (!tank || !tank->exists())
        {
            continue;
        }

        if (tank->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
            tank->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            continue;
        }

        if (tank->getDistance(pos) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static BWAPI::Position getMainBaseExitApproxPositionForTanks()
{
    Base* mainBase = the.bases.myMain();
    Base* naturalBase = the.bases.myNatural();

    if (mainBase &&
        naturalBase &&
        mainBase->getPosition().isValid() &&
        naturalBase->getPosition().isValid())
    {
        const BWAPI::Position mainPos = mainBase->getPosition();
        const BWAPI::Position naturalPos = naturalBase->getPosition();

        return BWAPI::Position(
            (mainPos.x * 35 + naturalPos.x * 65) / 100,
            (mainPos.y * 35 + naturalPos.y * 65) / 100
        );
    }

    if (mainBase && mainBase->getPosition().isValid())
    {
        return mainBase->getPosition();
    }

    if (the.bases.myStart())
    {
        return the.bases.myStart()->getCenter();
    }

    return BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());
}

static Base* getRealFrontBaseForTanks()
{
    Base* frontBase = the.bases.myFront();

    if (frontBase && frontBase->getPosition().isValid())
    {
        return frontBase;
    }

    Base* mainBase = the.bases.myMain();

    if (mainBase && mainBase->getPosition().isValid())
    {
        return mainBase;
    }

    return nullptr;
}

static BWAPI::Position pointTowardTerrainForTanks(
    const BWAPI::Position& from,
    const BWAPI::Position& to,
    int distance)
{
    if (!from.isValid() || !to.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    double dx = double(to.x - from.x);
    double dy = double(to.y - from.y);
    const double len = std::sqrt(dx * dx + dy * dy);

    if (len < 1.0)
    {
        return from;
    }

    dx /= len;
    dy /= len;

    return clampToMapForTanks(BWAPI::Position(
        int(from.x + dx * distance),
        int(from.y + dy * distance)));
}

static BWAPI::Position getTerrainTankRouteWaypoint(
    const BWAPI::Position& from,
    const BWAPI::Position& finalGoal)
{
    if (!from.isValid() || !finalGoal.isValid())
    {
        return finalGoal;
    }

    // Do not build a distance field for every moving front/join position. That
    // caused the old Fastest-to-Normal slowdown. If a goal lies in a known
    // base and its real ground route is much longer than the straight line,
    // extract a short waypoint from the Base's already-existing startup field.
    // This handles a close base across a cliff without creating any new grid.
    Base* goalBase = getKnownBaseForTankGoal(finalGoal);
    if (goalBase)
    {
        const int groundDistance = goalBase->getTileDistance(from);
        const int directDistanceTiles =
            std::max(0, (from.getApproxDistance(finalGoal) + 31) / 32);
        const int goalOffsetTiles =
            goalBase->getCenter().isValid()
            ? std::max(
                0,
                (goalBase->getCenter().getApproxDistance(finalGoal) + 31) / 32)
            : 0;

        if (groundDistance >= 0 &&
            groundDistance > directDistanceTiles + goalOffsetTiles + 6 &&
            goalBase->getCenter().isValid())
        {
            const GridDistances& distances = goalBase->getDistances();
            BWAPI::TilePosition current(from);
            int currentDistance = distances.at(current);
            const int actionX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
            const int actionY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

            for (int step = 0;
                step < 10 && current.isValid() && currentDistance > 0;
                ++step)
            {
                BWAPI::TilePosition best = BWAPI::TilePositions::Invalid;
                int bestDistance = currentDistance;
                int bestDirectness = MAX_DISTANCE;

                for (int direction = 0; direction < 8; ++direction)
                {
                    const BWAPI::TilePosition next(
                        current.x + actionX[direction],
                        current.y + actionY[direction]);

                    if (!next.isValid() ||
                        !the.map.isGroundTransitionWalkable(current, next))
                    {
                        continue;
                    }

                    const int distance = distances.at(next);
                    if (distance < 0 || distance >= currentDistance)
                    {
                        continue;
                    }

                    const int directness =
                        TileCenter(next).getApproxDistance(goalBase->getCenter());
                    if (distance < bestDistance ||
                        (distance == bestDistance &&
                         directness < bestDirectness))
                    {
                        best = next;
                        bestDistance = distance;
                        bestDirectness = directness;
                    }
                }

                if (!best.isValid())
                {
                    break;
                }

                current = best;
                currentDistance = bestDistance;
            }

            if (current.isValid() && current != BWAPI::TilePosition(from))
            {
                if (currentDistance == 0) return finalGoal;
                const BWAPI::Position waypoint = the.map.getGroundTileWaypoint(
                    current, BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode, true);
                if (waypoint.isValid()) return waypoint;
                // The coarse field can use a narrow corner beside a closed
                // door. Native pathing must receive the real goal in this case.
                return finalGoal;
            }
        }
    }

    // Ordinary local formation motion remains native StarCraft movement.
    return finalGoal;
}

// Return the strategically relevant exits from the area containing our current
// front base.  The base's canonical front choke is always first; a second
// external choke is added when the same defended area has another way out.
// This is especially important on maps such as Benzene, where defending one
// long line leaves the other approach completely open.
static std::vector<const ChokePoint*> getDefensiveTankChokes()
{
    std::vector<const ChokePoint*> result;

    Base* frontBase = getRealFrontBaseForTanks();
    if (!frontBase || !frontBase->getCenter().isValid())
    {
        return result;
    }

    const Zone* frontArea = the.zone.areaAt(frontBase->getCenter());
    const ChokePoint* primary = frontBase->getFrontChoke();

    if (primary && primary->center().isValid())
    {
        result.push_back(primary);
    }

    if (!frontArea)
    {
        return result;
    }

    const Zone* mainArea = nullptr;
    Base* mainBase = the.bases.myMain();
    if (mainBase && mainBase->getCenter().isValid())
    {
        mainArea = the.zone.areaAt(mainBase->getCenter());
    }

    struct Candidate
    {
        const ChokePoint* choke;
        int score;
    };

    std::vector<Candidate> candidates;
    for (const ChokePoint& chokeRef : the.zone.chokes())
    {
        const ChokePoint* choke = &chokeRef;
        if (!choke->center().isValid() || choke == primary ||
            !choke->connects(frontArea, choke->otherArea(frontArea)))
        {
            continue;
        }

        const BWAPI::TilePosition chokeTile(choke->center());
        if (!chokeTile.isValid() ||
            the.tileRoom.at(chokeTile) <= 0 ||
            the.tileRoom.at(chokeTile) > 16)
        {
            continue;
        }

        Zone* other = choke->otherArea(frontArea);
        if (!other)
        {
            continue;
        }

        // If we are defending the natural, the choke back into our main is an
        // internal retreat route, not an enemy approach.
        if (frontBase != mainBase && mainArea && other == mainArea)
        {
            continue;
        }

        const int distance =
            frontBase->getCenter().getApproxDistance(choke->center());

        // Do not claim a remote exit of one enormous area as part of this base.
        if (distance > 26 * 32)
        {
            continue;
        }

        int score = distance;
        Base* enemyBase = the.bases.enemyStart();
        if (enemyBase && the.bases.enemyStartConfirmed() &&
            enemyBase->getCenter().isValid())
        {
            const int baseToEnemy =
                frontBase->getCenter().getApproxDistance(enemyBase->getCenter());
            const int chokeToEnemy =
                choke->center().getApproxDistance(enemyBase->getCenter());

            // Prefer exits which make progress toward the enemy side, but keep
            // the geometric distance dominant so side entrances are retained.
            if (chokeToEnemy < baseToEnemy)
            {
                score -= 4 * 32;
            }
        }

        candidates.push_back({ choke, score });
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b)
        {
            if (a.score != b.score)
            {
                return a.score < b.score;
            }
            return a.choke->id() < b.choke->id();
        });

    // Two defended approaches are enough for the current Terran formation and
    // avoid spreading tanks over every minor crack in a large map area.
    for (const Candidate& candidate : candidates)
    {
        if (result.size() >= 2)
        {
            break;
        }
        result.push_back(candidate.choke);
    }

    return result;
}

// Keep the formation axes orthogonal. The old lateral choke edge and the
// independent base-center vector could point along the same axis on a bend.
static bool getTankChokeAxes(
    const ChokePoint* choke, const BWAPI::Position& baseCenter,
    double& insideX, double& insideY, double& sideX, double& sideY)
{
    if (!choke || !choke->center().isValid() || !baseCenter.isValid())
    {
        return false;
    }
    const double baseX = double(baseCenter.x - choke->center().x);
    const double baseY = double(baseCenter.y - choke->center().y);
    const double baseLength = std::hypot(baseX, baseY);
    if (baseLength < 1.0)
    {
        return false;
    }
    insideX = baseX / baseLength;
    insideY = baseY / baseLength;
    if (choke->endA().isValid() && choke->endB().isValid())
    {
        const double edgeX = double(choke->endB().x - choke->endA().x);
        const double edgeY = double(choke->endB().y - choke->endA().y);
        const double edgeLength = std::hypot(edgeX, edgeY);
        if (edgeLength > 1.0)
        {
            double normalX = -edgeY / edgeLength;
            double normalY = edgeX / edgeLength;
            const double towardBase = normalX * insideX + normalY * insideY;
            if (std::abs(towardBase) >= 0.35)
            {
                const double sign = towardBase >= 0.0 ? 1.0 : -1.0;
                insideX = normalX * sign;
                insideY = normalY * sign;
            }
        }
    }
    sideX = -insideY;
    sideY = insideX;
    return true;
}

static int getTankDefenseAnchorDistanceFromChoke()
{
    const int sight = BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.sightRange();
    const int range =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();

    // The old sight-edge setback placed the first pair almost a full sight
    // radius behind the entrance. They could barely see beyond the choke and
    // on tight maps ended up among the production buildings. Keep the pair
    // six tiles inside so it can actually cover an approach to the passage.
    return std::max(4 * 32,
        std::min(6 * 32, std::min(sight - 3 * 32, range - 5 * 32)));
}

static BWAPI::Unit getCompletedBunkerNearTankChoke(
    const ChokePoint* choke, const BWAPI::Position& baseCenter)
{
    if (!choke || !choke->center().isValid())
    {
        return nullptr;
    }

    const Zone* baseArea = the.zone.areaAt(baseCenter);
    BWAPI::Unit best = nullptr;
    int bestDistance = 8 * 32 + 1;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isCompleted() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Bunker ||
            !unit->getPosition().isValid() ||
            (baseArea && the.zone.areaAt(unit->getPosition()) != baseArea))
        {
            continue;
        }
        const int distance = unit->getDistance(choke->center());
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = unit;
        }
    }
    return best;
}

static bool tankSlotCoversBunkerApproach(
    const BWAPI::Position& slot, BWAPI::Unit bunker,
    double insideX, double insideY)
{
    if (!bunker)
    {
        return true;
    }

    const BWAPI::Position bunkerPos = bunker->getPosition();
    const double behind =
        (slot.x - bunkerPos.x) * insideX +
        (slot.y - bunkerPos.y) * insideY;
    const BWAPI::Position attackerPos(
        int(bunkerPos.x - insideX * 6 * 32),
        int(bunkerPos.y - insideY * 6 * 32));
    const int siegeRange =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();

    return behind >= 48 &&
        slot.getApproxDistance(attackerPos) <= siegeRange - 24;
}

static BWAPI::Position getTankDefenseAnchorForChoke(
    const ChokePoint* choke,
    const BWAPI::Position& baseCenter)
{
    double insideX, insideY, sideX, sideY;
    if (!getTankChokeAxes(choke, baseCenter, insideX, insideY, sideX, sideY))
    {
        return BWAPI::Positions::Invalid;
    }

    const int now = BWAPI::Broodwar->getFrameCount();
    const unsigned int revision = the.partitions.getRevision();
    const unsigned int runtimeRevision = the.map.runtimeRevision();
    BWAPI::Unit bunker = getCompletedBunkerNearTankChoke(choke, baseCenter);
    const int bunkerID = bunker ? bunker->getID() : -1;
    auto& cached = tankDefenseAnchorCache[choke->id()];

    // Static topology/base-side determines the anchor. A normal building
    // occupancy revision should not blindly redo the whole choke search. When
    // buildings changed, validate the one cached footprint once; rebuild only
    // if that exact anchor actually became blocked.
    if (cached.base == baseCenter && cached.revision == revision &&
        cached.bunkerID == bunkerID)
    {
        if (!cached.position.isValid())
        {
            return BWAPI::Positions::Invalid;
        }

        if (cached.runtimeRevision == runtimeRevision)
        {
            return cached.position;
        }

        cached.runtimeRevision = runtimeRevision;
        if (tankParkingTerrainIsClear(cached.position))
        {
            return cached.position;
        }
    }

    cached.base = baseCenter;
    cached.revision = revision;
    cached.runtimeRevision = runtimeRevision;
    cached.bunkerID = bunkerID;
    cached.frame = now;
    cached.position = BWAPI::Positions::Invalid;

    const Zone* baseArea = the.zone.areaAt(baseCenter);
    // A Bunker already occupies the front line. Its Tank row can start closer
    // to the choke than the generic six-tile fallback and still be behind it.
    const int setback = bunker ? 4 * 32 : getTankDefenseAnchorDistanceFromChoke();
    // Correct toward the base and along the defended floor; never snap a
    // blocked nominal anchor forward onto the choke or across a cliff.
    for (int depth = 0; depth <= 5; ++depth)
    {
        for (int side : { 0, -1, 1, -2, 2, -3, 3, -4, 4 })
        {
            const BWAPI::Position candidate(
                int(choke->center().x + insideX * (setback + depth * 32) + sideX * side * 32),
                int(choke->center().y + insideY * (setback + depth * 32) + sideY * side * 32));
            if (!candidate.isValid() ||
                candidate.getApproxDistance(choke->center()) >
                    BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.sightRange() - 24 ||
                !tankParkingTerrainIsClear(candidate) ||
                !tankSlotCoversBunkerApproach(
                    candidate, bunker, insideX, insideY) ||
                (baseArea && the.zone.areaAt(candidate) != baseArea) ||
                !the.partitions.connected(baseCenter, candidate))
            {
                continue;
            }
            cached.position = candidate;
            return candidate;
        }
    }
    return BWAPI::Positions::Invalid;
}

// A strategic route waypoint is allowed to pass through a choke; a mass rally
// is not. Park the rally on our side of the nearest choke at roughly siege
// firing distance so Tanks never assemble in the traffic lane itself.
static BWAPI::Position moveTankMassGatherOffNearbyChoke(
    const BWAPI::Position& position,
    const BWAPI::Position& ourSideReference)
{
    if (!position.isValid() || !ourSideReference.isValid())
    {
        return position;
    }

    const ChokePoint* nearest = nullptr;
    int nearestDistance = MAX_DISTANCE;
    auto considerChoke = [&](const ChokePoint* choke)
    {
        if (!choke || !choke->center().isValid())
        {
            return;
        }
        const int distance = position.getApproxDistance(choke->center());
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearest = choke;
        }
    };

    // First use only the entrances that belong to our current defended base.
    // An unrelated nearby choke elsewhere in a large Area must never pull the
    // rally sideways. Fall back to the global choke list only when the base
    // analysis did not provide any usable entrance at all.
    const std::vector<const ChokePoint*> defensiveChokes = getDefensiveTankChokes();
    for (const ChokePoint* choke : defensiveChokes)
    {
        considerChoke(choke);
    }
    if (!nearest)
    {
        for (const ChokePoint& choke : the.zone.chokes())
        {
            considerChoke(&choke);
        }
    }

    if (!nearest)
    {
        return position;
    }

    double insideX = double(ourSideReference.x - nearest->center().x);
    double insideY = double(ourSideReference.y - nearest->center().y);
    const double length = std::hypot(insideX, insideY);
    if (length < 1.0)
    {
        return position;
    }
    insideX /= length;
    insideY /= length;
    const double sideX = -insideY;
    const double sideY = insideX;

    // Positive projection is OUR side of the choke. Requiring a real setback,
    // instead of checking only Euclidean distance from the choke center, also
    // catches a route waypoint that has already crossed to the enemy side.
    const double projection =
        double(position.x - nearest->center().x) * insideX +
        double(position.y - nearest->center().y) * insideY;

    const int chokeHalfWidth = std::max(0, nearest->widthPixels() / 2);
    const int siegeRange =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();
    const int safeSetback = std::min(
        siegeRange - 3 * 32,
        std::max(7 * 32, chokeHalfWidth + 3 * 32));

    const BWAPI::TilePosition positionTile(position);
    const bool openEnough =
        positionTile.isValid() && the.tileRoom.at(positionTile) > 16;

    if (projection >= safeSetback && openEnough)
    {
        return position;
    }

    static const int sideOffsets[] = { 0, -2, 2, -4, 4, -6, 6, -8, 8 };

    // Search only farther INTO our territory. This makes the safety property
    // explicit: a mass gather can never fall back onto the choke traffic lane
    // or across it merely because the prettiest slot was blocked by a building.
    for (int extraDepth = 0; extraDepth <= 8; ++extraDepth)
    {
        for (int sideTiles : sideOffsets)
        {
            BWAPI::Position candidate(
                int(nearest->center().x + insideX * (safeSetback + extraDepth * 32) +
                    sideX * sideTiles * 32),
                int(nearest->center().y + insideY * (safeSetback + extraDepth * 32) +
                    sideY * sideTiles * 32));
            candidate = clampToMapForTanks(candidate);
            const BWAPI::TilePosition candidateTile(candidate);

            if (!candidate.isValid() || !candidateTile.isValid() ||
                candidate.getApproxDistance(nearest->center()) > siegeRange - 32 ||
                the.tileRoom.at(candidateTile) <= 16 ||
                !tankParkingStaticTerrainIsClear(candidate) ||
                !the.partitions.connected(ourSideReference, candidate))
            {
                continue;
            }

            return candidate;
        }
    }

    // Cramped custom maps may not have a clean point at the requested setback.
    // Fall back near our own base side rather than returning the unsafe choke
    // waypoint. This is less pretty, but it preserves the hard invariant that
    // MASS GATHER is not a traffic blocker.
    BWAPI::Position baseSide = pointTowardTerrainForTanks(
        ourSideReference, nearest->center(), 6 * 32);
    baseSide = findNearbyWalkableTankGatherPosition(baseSide);
    if (baseSide.isValid() &&
        the.partitions.connected(ourSideReference, baseSide))
    {
        static int lastForcedGatherWarningFrame = -100000;
        const int now = BWAPI::Broodwar->getFrameCount();
        if (now < lastForcedGatherWarningFrame ||
            now - lastForcedGatherWarningFrame >= 10 * 24)
        {
            lastForcedGatherWarningFrame = now;
            BWAPI::Broodwar->printf(
                "TANK GATHER WARNING: forced base-side fallback for C%d",
                nearest->id());
        }
        return baseSide;
    }

    BWAPI::Position lastResort =
        findNearbyWalkableTankGatherPosition(ourSideReference);
    if (lastResort.isValid() &&
        the.partitions.connected(ourSideReference, lastResort))
    {
        return lastResort;
    }

    // Only an invalid/disconnected own-side reference can reach this point.
    // Keep the original position rather than manufacturing an unreachable one;
    // GRID/TANK diagnostics above make this exceptional map state visible.
    return position;
}

// Explicit choke recognition is deliberately conservative and can miss a
// broad/irregular traffic lane.  The local TileRoom field is the final guard:
// MASS GATHER must live on open terrain, never in a narrow tile where a Siege
// Tank blocks the army behind it.  Move only toward our side and keep the same
// static map partition, so this cannot invent a rally across a cliff/island.
static BWAPI::Position moveTankMassGatherOutOfNarrowTerrain(
    const BWAPI::Position& position,
    const BWAPI::Position& ourSideReference)
{
    if (!position.isValid() || !ourSideReference.isValid())
    {
        return position;
    }

    const BWAPI::TilePosition tile(position);
    if (!tile.isValid())
    {
        return position;
    }

    const int room = the.tileRoom.at(tile);
    if (room > 14)
    {
        return position;
    }

    double backX = double(ourSideReference.x - position.x);
    double backY = double(ourSideReference.y - position.y);
    const double length = std::hypot(backX, backY);
    if (length < 1.0)
    {
        return position;
    }
    backX /= length;
    backY /= length;
    const double sideX = -backY;
    const double sideY = backX;

    static const int sideOffsets[] = { 0, -2, 2, -4, 4, -6, 6 };
    for (int depthTiles = 4; depthTiles <= 14; ++depthTiles)
    {
        for (int sideTiles : sideOffsets)
        {
            BWAPI::Position candidate(
                int(position.x + backX * depthTiles * 32 + sideX * sideTiles * 32),
                int(position.y + backY * depthTiles * 32 + sideY * sideTiles * 32));
            candidate = clampToMapForTanks(candidate);
            const BWAPI::TilePosition candidateTile(candidate);
            if (!candidate.isValid() || !candidateTile.isValid() ||
                the.tileRoom.at(candidateTile) <= 16 ||
                !tankParkingStaticTerrainIsClear(candidate) ||
                !the.partitions.connected(ourSideReference, candidate))
            {
                continue;
            }
            return candidate;
        }
    }

    // Keep the original point as a last resort, but make the failed correction
    // visible. This is throttled because gather selection may be retried while
    // a building temporarily occupies the preferred open area.
    static int lastNarrowGatherWarningFrame = -100000;
    const int now = BWAPI::Broodwar->getFrameCount();
    if (now < lastNarrowGatherWarningFrame || now - lastNarrowGatherWarningFrame >= 10 * 24)
    {
        lastNarrowGatherWarningFrame = now;
        BWAPI::Broodwar->printf(
            "TANK GATHER WARNING: narrow rally (%d,%d) room=%d; no open fallback",
            tile.x, tile.y, room);
    }
    return position;
}

// Two tanks per row, with subsequent rows toward the defended side. Reserve
// tanks add depth rather than stretching the first eight tanks across a ramp.
static BWAPI::Position getCompactTankSlotAtChoke(
    int localIndex,
    const ChokePoint* choke,
    const BWAPI::Position& baseCenter,
    const std::vector<BWAPI::Position>& reservedPositions)
{
    const BWAPI::Position anchor = getTankDefenseAnchorForChoke(choke, baseCenter);
    double insideX, insideY, sideX, sideY;
    if (localIndex < 0 || !anchor.isValid() ||
        !getTankChokeAxes(choke, baseCenter, insideX, insideY, sideX, sideY))
    {
        return BWAPI::Positions::Invalid;
    }

    // Spread a larger line sideways before adding depth. Five rows of two
    // tanks used to put the rear tanks beyond siege range of the passage.
    const int row = localIndex / 4;
    const int sideSign = localIndex % 2 == 0 ? -1 : 1;
    const int lateralBand = (localIndex % 4) / 2;
    const Zone* baseArea = the.zone.areaAt(baseCenter);
    BWAPI::Unit bunker = getCompletedBunkerNearTankChoke(choke, baseCenter);
    const int minSetback =
        (bunker ? 4 * 32 : getTankDefenseAnchorDistanceFromChoke()) - 8;
    const BWAPI::Position forward(-int(insideX * 320), -int(insideY * 320));

    for (int extraDepth = 0; extraDepth <= 6; ++extraDepth)
    {
        for (int sideChoice = 0; sideChoice < 2; ++sideChoice)
        {
            const int sign = sideChoice == 0 ? sideSign : -sideSign;
            for (int sideStep = 0; sideStep <= 2; ++sideStep)
            {
                // The old 72-pixel center offset left part of a sieged Tank's
                // body inside the nominal 128-pixel traffic lane.
                const double lateral = sign * (96 + lateralBand * 80 + sideStep * 16);
                const double depth = row * 64 + extraDepth * 32;
                const BWAPI::Position candidate(
                    int(anchor.x + sideX * lateral + insideX * depth),
                    int(anchor.y + sideY * lateral + insideY * depth));
                const double setback =
                    (candidate.x - choke->center().x) * insideX +
                    (candidate.y - choke->center().y) * insideY;
                // Every tank in the line must cover the entrance. Terrain can
                // be perfectly clear behind the mineral line, but tanks there
                // let enemies pass the choke without taking a shot.
                if (!candidate.isValid() || setback < minSetback ||
                    candidate.getApproxDistance(choke->center()) >
                        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange() - 32 ||
                    !tankParkingTerrainIsClear(candidate) ||
                    !tankSlotCoversBunkerApproach(
                        candidate, bunker, insideX, insideY) ||
                    positionBlocksDefensePassage(candidate, anchor, forward) ||
                    (baseArea && the.zone.areaAt(candidate) != baseArea))
                {
                    continue;
                }
                bool occupied = false;
                for (const BWAPI::Position& reserved : reservedPositions)
                {
                    if (reserved.isValid() && reserved.getDistance(candidate) < 48.0)
                    {
                        occupied = true;
                        break;
                    }
                }
                if (occupied || !the.partitions.connected(baseCenter, candidate))
                {
                    continue;
                }
                return candidate;
            }
        }
    }
    return BWAPI::Positions::Invalid;
}

static BWAPI::Position getRealFrontPositionForTanks()
{
    Base* frontBase = getRealFrontBaseForTanks();

    if (frontBase)
    {
        const ChokePoint* choke = frontBase->getFrontChoke();
        if (choke && choke->center().isValid() && frontBase->getCenter().isValid())
        {
            const BWAPI::Position tankLine =
                getTankDefenseAnchorForChoke(choke, frontBase->getCenter());
            if (tankLine.isValid())
            {
                return tankLine;
            }
        }

        BWAPI::Position front = getBaseFrontPositionForTanks(frontBase);

        if (front.isValid())
        {
            return clampToMapForTanks(front);
        }
    }

    return getMainBaseExitApproxPositionForTanks();
}

static BWAPI::Position getRealFrontForwardVectorForTanks(
    const BWAPI::Position& frontPosition)
{
    Base* frontBase = getRealFrontBaseForTanks();

    if (frontBase && frontPosition.isValid())
    {
        BWAPI::Position baseCenter = frontBase->getCenter();

        if (baseCenter.isValid() &&
            frontPosition.getApproxDistance(baseCenter) > 0)
        {
            return BWAPI::Position(
                frontPosition.x - baseCenter.x,
                frontPosition.y - baseCenter.y
            );
        }
    }

    Base* mainBase = the.bases.myMain();
    Base* naturalBase = the.bases.myNatural();

    if (mainBase &&
        naturalBase &&
        mainBase->getPosition().isValid() &&
        naturalBase->getPosition().isValid())
    {
        const BWAPI::Position mainPos = mainBase->getPosition();
        const BWAPI::Position naturalPos = naturalBase->getPosition();

        return BWAPI::Position(
            naturalPos.x - mainPos.x,
            naturalPos.y - mainPos.y
        );
    }

    return BWAPI::Position(0, -1);
}

static bool handleTankFightWhileMovingToGather(
    BWAPI::Unit tank,
    const BWAPI::Unitset& tankTargets,
    int now,
    int siegeTankRange,
    int emergencyUnsiegeDistance,
    int tankHoldFrames,
    int postUnsiegeCooldown,
    int& lastUnsiegeFrame,
    int& holdTankUntil,
    int& lastSiegeFrame)
{
    if (!tank || !tank->exists())
    {
        return false;
    }

    if (tankTargets.empty())
    {
        return false;
    }

    BWAPI::Unit target = nullptr;
    int bestPriority = -1;
    int bestDistance = MAX_DISTANCE;

    for (BWAPI::Unit enemy : tankTargets)
    {
        if (!enemy ||
            !enemy->exists() ||
            !enemy->isVisible() ||
            !enemy->getPosition().isValid() ||
            enemy->isFlying())
        {
            continue;
        }

        const BWAPI::UnitType type = enemy->getType();

        if (type.isWorker() || type.isSpell())
        {
            continue;
        }

        const int dist = tank->getDistance(enemy);

        if (dist > siegeTankRange + 3 * 32)
        {
            continue;
        }

        int priority = tankEnemyThreatScore(enemy);

        if (type.isBuilding())
        {
            if (!UnitUtil::CanAttackGround(enemy))
            {
                continue;
            }

            priority = std::max(priority, 2);
        }

        if (priority <= 0)
        {
            continue;
        }

        if (preferTankCombatTarget(tank, enemy, target, priority, bestPriority))
        {
            bestPriority = priority;
            bestDistance = dist;
            target = enemy;
        }
    }

    if (!target)
    {
        return false;
    }

    const bool targetTooClose =
        bestDistance < emergencyUnsiegeDistance &&
        !target->getType().isBuilding();

    const bool targetInSiegeRange =
        bestDistance >= emergencyUnsiegeDistance &&
        bestDistance <= siegeTankRange;

    const bool targetInTankRange =
        bestDistance <= BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode.groundWeapon().maxRange();

    if (tank->isSieged())
    {
        if (targetTooClose || tank->isUnderDisruptionWeb())
        {
            if (tank->canUnsiege())
            {
                if (the.micro.Unsiege(tank, true))
                {
                    lastUnsiegeFrame = now;
                    holdTankUntil = now + tankHoldFrames;
                }
            }
            else
            {
                the.micro.HoldPosition(tank);
            }

            return true;
        }

        if (targetInSiegeRange)
        {
            the.micro.AttackUnit(tank, target);
            return true;
        }

        the.micro.HoldPosition(tank);
        return true;
    }

    if (targetTooClose)
    {
        the.micro.KiteTarget(tank, target);
        holdTankUntil = now + tankHoldFrames;
        return true;
    }

    if (targetInSiegeRange &&
        tank->canSiege() &&
        now >= holdTankUntil &&
        now - lastUnsiegeFrame >= postUnsiegeCooldown)
    {
        if (the.micro.Siege(tank))
        {
            lastSiegeFrame = now;
        }
        return true;
    }

    if (targetInTankRange)
    {
        the.micro.AttackUnit(tank, target);
        return true;
    }

    return false;
}

static bool handleTankEmergencyBeforeForcedMove(
    BWAPI::Unit tank,
    const BWAPI::Unitset& targets,
    int now,
    int siegeTankRange,
    int emergencyUnsiegeDistance,
    int tankHoldFrames,
    int postUnsiegeCooldown,
    int& lastUnsiegeFrame,
    int& holdTankUntil,
    int& lastSiegeFrame)
{
    if (!tank || !tank->exists())
    {
        return false;
    }

    if (targets.empty())
    {
        return false;
    }
    BWAPI::Unit bestTarget = nullptr;
    int bestScore = -1;
    int bestDistance = MAX_DISTANCE;

    int localThreatScore = 0;
    int localThreatCount = 0;

    for (BWAPI::Unit enemy : targets)
    {
        if (!enemy ||
            !enemy->exists() ||
            !enemy->isVisible() ||
            !enemy->getPosition().isValid() ||
            enemy->isFlying())
        {
            continue;
        }

        const BWAPI::UnitType type = enemy->getType();

        if (type.isWorker() || type.isSpell())
        {
            continue;
        }

        if (!UnitUtil::CanAttackGround(enemy) && !type.isBuilding())
        {
            continue;
        }

        const int distance = tank->getDistance(enemy);

        if (distance > siegeTankRange + 4 * 32)
        {
            continue;
        }

        int score = tankEnemyThreatScore(enemy);

        if (type.isBuilding() && UnitUtil::CanAttackGround(enemy))
        {
            score = std::max(score, 3);
        }

        if (score <= 0)
        {
            continue;
        }

        if (distance <= 7 * 32)
        {
            localThreatScore += score;
            ++localThreatCount;
        }

        if (preferTankCombatTarget(tank, enemy, bestTarget, score, bestScore))
        {
            bestScore = score;
            bestDistance = distance;
            bestTarget = enemy;
        }
    }

    if (!bestTarget)
    {
        return false;
    }

    const bool targetIsBuilding =
        bestTarget->getType().isBuilding();

    const bool enemyTooClose =
        bestDistance < emergencyUnsiegeDistance &&
        !targetIsBuilding;

    const bool enemyInSiegeRange =
        bestDistance >= emergencyUnsiegeDistance &&
        bestDistance <= siegeTankRange;

    const bool enemyInTankRange =
        bestDistance <= BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode
        .groundWeapon()
        .maxRange();

    const bool enemyVeryDangerousNearby =
        (bestScore >= 4 && bestDistance <= 7 * 32) ||
        (localThreatScore >= 5 && localThreatCount >= 2);

    if (!enemyTooClose && !enemyInSiegeRange && !enemyVeryDangerousNearby)
    {
        return false;
    }

    if (tank->isSieged())
    {
        if (enemyTooClose || tank->isUnderDisruptionWeb())
        {
            if (tank->canUnsiege())
            {
                if (the.micro.Unsiege(tank, true))
                {
                    lastUnsiegeFrame = now;
                    holdTankUntil = now + tankHoldFrames;
                }
            }
            else
            {
                the.micro.HoldPosition(tank);
            }

            return true;
        }

        if (enemyInSiegeRange)
        {
            the.micro.AttackUnit(tank, bestTarget);
            return true;
        }

        if (enemyVeryDangerousNearby)
        {
            the.micro.HoldPosition(tank);
            return true;
        }

        return false;
    }

    if (enemyTooClose)
    {
        the.micro.KiteTarget(tank, bestTarget);
        holdTankUntil = now + tankHoldFrames;
        return true;
    }

    if (enemyInSiegeRange &&
        tank->canSiege() &&
        now >= holdTankUntil &&
        now - lastUnsiegeFrame >= postUnsiegeCooldown)
    {
        if (the.micro.Siege(tank))
        {
            lastSiegeFrame = now;
        }
        return true;
    }

    if (enemyInTankRange)
    {
        the.micro.AttackUnit(tank, bestTarget);
        holdTankUntil = now + tankHoldFrames;
        return true;
    }

    if (enemyVeryDangerousNearby)
    {
        the.micro.HoldPosition(tank);
        holdTankUntil = now + tankHoldFrames;
        return true;
    }

    return false;
}

static BWAPI::Position getTankMassGatherEnemyReference(
    const BWAPI::Position& enemyGoal)
{
    // MASS GATHER is a strategic staging point, not a chase target. Prefer the
    // known enemy starting base so a moving unit/order cannot rotate the whole
    // formation or invalidate its route cache every few frames.
    Base* enemyBase = the.bases.enemyStart();
    if (enemyBase && the.bases.enemyStartConfirmed())
    {
        if (enemyBase->getCenter().isValid())
        {
            return enemyBase->getCenter();
        }
        if (enemyBase->getPosition().isValid())
        {
            return enemyBase->getPosition();
        }
    }

    // A directly observed enemy depot is a stronger facing reference than an
    // unconfirmed start-location hypothesis. This also covers games where we
    // found an expansion first: the formation will at least face real enemy
    // territory instead of an arbitrary map-centre diagonal.
    Base* observedEnemyBase = nullptr;
    for (Base* base : the.bases.getAll())
    {
        if (!base || base->getOwner() != the.enemy() ||
            !base->getDepot() || !base->getDepot()->exists() ||
            base->getDepot()->isLifted() || !base->getCenter().isValid())
        {
            continue;
        }

        if (base->isAStartingBase())
        {
            return base->getCenter();
        }

        if (!observedEnemyBase)
        {
            observedEnemyBase = base;
        }
    }

    if (observedEnemyBase)
    {
        return observedEnemyBase->getCenter();
    }

    // Do not rotate the complete Tank formation toward an elimination-based
    // enemyStart hypothesis. Until the main is directly confirmed, a stable
    // map-center direction is safer than repeatedly following a wrong scout
    // candidate (notably on Tau Cross).
    if (enemyBase && !the.bases.enemyStartConfirmed())
    {
        return BWAPI::Position(
            BWAPI::Broodwar->mapWidth() * 16,
            BWAPI::Broodwar->mapHeight() * 16);
    }

    return enemyGoal;
}

// The choke-safe anchor can still land beside a large resource depot on a
// compact natural. Find the nearest open point in the same owned area, with a
// mild preference toward the known enemy direction. This runs only when the
// cached anchor fails the hard terrain/depot invariant.
static BWAPI::Position moveTankMassGatherAwayFromResourceDepot(
    const BWAPI::Position& position,
    const BWAPI::Position& ownedBaseReference,
    const BWAPI::Position& enemyReference)
{
    if (!position.isValid() || !ownedBaseReference.isValid() ||
        tankParkingStaticTerrainIsClear(position))
    {
        return position;
    }

    double forwardX = 0.0;
    double forwardY = -1.0;
    double forwardLength = 1.0;
    if (enemyReference.isValid())
    {
        forwardX = double(enemyReference.x - ownedBaseReference.x);
        forwardY = double(enemyReference.y - ownedBaseReference.y);
        forwardLength = std::hypot(forwardX, forwardY);
    }
    if (!enemyReference.isValid() || forwardLength < 1.0)
    {
        forwardX = double(position.x - ownedBaseReference.x);
        forwardY = double(position.y - ownedBaseReference.y);
        forwardLength = std::hypot(forwardX, forwardY);
    }
    if (forwardLength < 1.0)
    {
        forwardX = 0.0;
        forwardY = -1.0;
        forwardLength = 1.0;
    }
    forwardX /= forwardLength;
    forwardY /= forwardLength;
    const double sideX = -forwardY;
    const double sideY = forwardX;

    const Zone* ownedArea = the.zone.areaAt(ownedBaseReference);
    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestScore = MAX_DISTANCE;

    for (int forwardTiles = -10; forwardTiles <= 10; ++forwardTiles)
    {
        for (int sideTiles = -10; sideTiles <= 10; ++sideTiles)
        {
            BWAPI::Position candidate(
                int(position.x + forwardX * forwardTiles * 32 + sideX * sideTiles * 32),
                int(position.y + forwardY * forwardTiles * 32 + sideY * sideTiles * 32));
            candidate = clampToMapForTanks(candidate);

            const BWAPI::TilePosition tile(candidate);
            if (!tile.isValid() || the.tileRoom.at(tile) <= 14 ||
                !tankParkingStaticTerrainIsClear(candidate) ||
                !the.partitions.connected(ownedBaseReference, candidate) ||
                (ownedArea && the.zone.areaAt(candidate) != ownedArea))
            {
                continue;
            }

            const int distance = candidate.getApproxDistance(position);
            const int backwardPenalty = forwardTiles < 0 ? -forwardTiles * 24 : 0;
            const int forwardPreference = forwardTiles > 0 ? forwardTiles * 4 : 0;
            const int score = distance + backwardPenalty - forwardPreference;

            if (score < bestScore)
            {
                bestScore = score;
                best = candidate;
            }
        }
    }

    return best.isValid() ? best : position;
}

static BWAPI::Position getTankMassGatherPosition(
    const BWAPI::Position& enemyGoal)
{
    Base* mainBase = the.bases.myMain();
    Base* naturalBase = the.bases.myNatural();

    Base* stagingBase = nullptr;
    const bool naturalStarted =
        naturalBase &&
        naturalBase->getOwner() == the.self() &&
        naturalBase->getDepot() &&
        naturalBase->getCenter().isValid();

    if (naturalStarted)
    {
        stagingBase = naturalBase;
    }
    else if (mainBase && mainBase->getCenter().isValid())
    {
        stagingBase = mainBase;
    }
    else if (the.bases.myStart())
    {
        stagingBase = the.bases.myStart();
    }

    if (!stagingBase || !stagingBase->getCenter().isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position start = stagingBase->getCenter();
    BWAPI::Position target = getTankMassGatherEnemyReference(enemyGoal);
    if (!target.isValid())
    {
        target = BWAPI::Position(
            BWAPI::Broodwar->mapWidth() * 16,
            BWAPI::Broodwar->mapHeight() * 16);
    }

    const ChokePoint* frontChoke = stagingBase->getFrontChoke();
    const int siegeRange =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();
    const bool cachedGatherStillMatches =
        cachedTankMassGatherPosition.isValid() &&
        cachedTankMassGatherStart.isValid() &&
        cachedTankMassGatherTarget.isValid() &&
        cachedTankMassGatherRevision == the.partitions.getRevision() &&
        cachedTankMassGatherStart.getApproxDistance(start) <= 32 &&
        cachedTankMassGatherTarget.getApproxDistance(target) <= 2 * 32 &&
        tankParkingStaticTerrainIsClear(cachedTankMassGatherPosition) &&
        (!frontChoke || !frontChoke->center().isValid() ||
         cachedTankMassGatherPosition.getApproxDistance(frontChoke->center()) <=
             siegeRange - 32);

    if (cachedGatherStillMatches)
    {
        return cachedTankMassGatherPosition;
    }

    // MASS GATHER is a staging position, not a path query. The old version
    // built full GridDistances fields here to find a waypoint toward the enemy.
    // On Tau Cross/Benzene that was exactly when the heavy hitch started. Use
    // the already-computed front choke and place the rally well INSIDE our side
    // of it; StarCraft pathfinding handles how individual tanks reach the slot.
    BWAPI::Position desired = BWAPI::Positions::Invalid;
    if (frontChoke && frontChoke->center().isValid())
    {
        double insideX = double(start.x - frontChoke->center().x);
        double insideY = double(start.y - frontChoke->center().y);
        const double insideLen = std::hypot(insideX, insideY);
        if (insideLen > 1.0)
        {
            insideX /= insideLen;
            insideY /= insideLen;

            // Nine tiles gives tanks room to deploy without occupying the choke
            // traffic lane. moveTankMassGatherOutOfNarrowTerrain() below makes
            // a final correction if the nominal point is still cramped.
            const int insideDistance = 9 * 32;
            desired = BWAPI::Position(
                int(frontChoke->center().x + insideX * insideDistance),
                int(frontChoke->center().y + insideY * insideDistance));
        }
    }

    // Maps without a useful front-choke analysis use a conservative geometric
    // fallback. It deliberately stays closer to our staging base than the old
    // 35% route waypoint so it cannot land directly on a distant choke.
    if (!desired.isValid())
    {
        const double t = 0.20;
        desired = BWAPI::Position(
            int(start.x + double(target.x - start.x) * t),
            int(start.y + double(target.y - start.y) * t));
    }

    desired = clampToMapForTanks(desired);
    cachedTankMassGatherPosition =
        findNearbyWalkableTankGatherPosition(desired);
    cachedTankMassGatherPosition = moveTankMassGatherAwayFromResourceDepot(
        cachedTankMassGatherPosition, start, target);
    cachedTankMassGatherPosition = moveTankMassGatherOffNearbyChoke(
        cachedTankMassGatherPosition, start);
    cachedTankMassGatherPosition = moveTankMassGatherOutOfNarrowTerrain(
        cachedTankMassGatherPosition, start);

    // Corrections for a choke or narrow tile must preserve the depot ring too.
    if (!tankParkingStaticTerrainIsClear(cachedTankMassGatherPosition))
    {
        cachedTankMassGatherPosition = moveTankMassGatherAwayFromResourceDepot(
            cachedTankMassGatherPosition, start, target);
    }

    // Narrow-terrain/depot corrections may move a valid rally behind the
    // mineral line. Keep it within firing range of the defended entrance.
    if (frontChoke && frontChoke->center().isValid() &&
        cachedTankMassGatherPosition.getApproxDistance(frontChoke->center()) >
            siegeRange - 32)
    {
        cachedTankMassGatherPosition = getCompactTankSlotAtChoke(
            0, frontChoke, start, std::vector<BWAPI::Position>());
        if (!cachedTankMassGatherPosition.isValid())
        {
            cachedTankMassGatherPosition =
                getTankDefenseAnchorForChoke(frontChoke, start);
        }
    }

    cachedTankMassGatherStart = start;
    cachedTankMassGatherTarget = target;
    cachedTankMassGatherFrame = BWAPI::Broodwar->getFrameCount();
    cachedTankMassGatherRevision = the.partitions.getRevision();
    return cachedTankMassGatherPosition;
}

static void drawTankTopologyDebug(const BWAPI::Position& enemyGoal)
{
    if (!Config::Debug::DrawMapInfo && !Config::Debug::DrawTankPositions)
    {
        return;
    }

    static int lastDrawFrame = -100000;
    static int lastGeometryFrame = -100000;
    static unsigned int lastGeometryRevision = 0;
    static BWAPI::Position cachedRealFront = BWAPI::Positions::Invalid;
    static BWAPI::Position cachedMassGather = BWAPI::Positions::Invalid;
    static std::vector<std::pair<int, BWAPI::Position>> cachedDefenseAnchors;

    const int now = BWAPI::Broodwar->getFrameCount();
    if (lastDrawFrame == now)
    {
        return;
    }
    lastDrawFrame = now;

    if (now < lastGeometryFrame || now - lastGeometryFrame >= 5 * 24 ||
        lastGeometryRevision != the.partitions.getRevision())
    {
        lastGeometryFrame = now;
        lastGeometryRevision = the.partitions.getRevision();
        cachedRealFront = getRealFrontPositionForTanks();
        cachedMassGather =
            StrategyManager::Instance().defaultTankGatherEnabled() &&
            !tankMassAttackReleased
            ? getTankMassGatherPosition(enemyGoal)
            : BWAPI::Positions::Invalid;
        cachedDefenseAnchors.clear();

        Base* frontBase = getRealFrontBaseForTanks();
        BWAPI::Position baseCenter =
            frontBase && frontBase->getCenter().isValid()
            ? frontBase->getCenter()
            : the.bases.myMain() && the.bases.myMain()->getCenter().isValid()
            ? the.bases.myMain()->getCenter()
            : BWAPI::Positions::Invalid;

        if (baseCenter.isValid())
        {
            const std::vector<const ChokePoint*> chokes = getDefensiveTankChokes();
            for (const ChokePoint* choke : chokes)
            {
                if (!choke || !choke->center().isValid())
                {
                    continue;
                }

                const BWAPI::Position anchor =
                    getTankDefenseAnchorForChoke(choke, baseCenter);
                if (!anchor.isValid())
                {
                    continue;
                }
                cachedDefenseAnchors.push_back({ choke->id(), anchor });
            }
        }
    }

    // GridZone::draw() already draws every C# choke (including width) under
    // DrawMapInfo/DrawTankPositions. Do not duplicate those calls here; add
    // only Tank-specific cached geometry.

    if (cachedRealFront.isValid())
    {
        BWAPI::Broodwar->drawCircleMap(cachedRealFront, 26, BWAPI::Colors::Orange, false);
        BWAPI::Broodwar->drawTextMap(cachedRealFront + BWAPI::Position(8, 0), "REAL FRONT");
    }
    if (cachedMassGather.isValid())
    {
        BWAPI::Broodwar->drawCircleMap(cachedMassGather, 30, BWAPI::Colors::Cyan, false);
        BWAPI::Broodwar->drawTextMap(cachedMassGather + BWAPI::Position(8, 0), "MASS GATHER");
    }

    const int sight = BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.sightRange();
    const int siegeRange =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();

    for (const auto& entry : cachedDefenseAnchors)
    {
        BWAPI::Broodwar->drawCircleMap(entry.second, 24, BWAPI::Colors::Yellow, false);
        BWAPI::Broodwar->drawCircleMap(entry.second, sight, BWAPI::Colors::Green, false);
        BWAPI::Broodwar->drawCircleMap(entry.second, siegeRange, BWAPI::Colors::Red, false);
        BWAPI::Broodwar->drawTextMap(
            entry.second + BWAPI::Position(8, -8),
            "TANK DEF C%d", entry.first);
    }

    for (const auto& entry : rememberedEnemyTanks)
    {
        const RememberedEnemyTank& memory = entry.second;
        if (!memory.position.isValid())
        {
            continue;
        }
        const int ageSeconds = std::max(0, (now - memory.lastSeenFrame) / 24);
        BWAPI::Broodwar->drawCircleMap(memory.position, 20, BWAPI::Colors::Red, false);
        BWAPI::Broodwar->drawTextMap(
            memory.position + BWAPI::Position(6, 0),
            "MEM %s %ds", memory.wasSieged ? "S" : "T", ageSeconds);
    }
}

static void drawAssignedTankSlot(
    BWAPI::Unit tank, const BWAPI::Position& position, char label, BWAPI::Color color)
{
    if ((!Config::Debug::DrawTankPositions && !Config::Debug::DrawMapInfo &&
         !Config::Debug::DrawUnitTargets) || !position.isValid())
    {
        return;
    }

    // Draw the slot actually used by micro, including traffic corrections.
    // Debug rendering must not run another set of expensive placement searches.
    BWAPI::Broodwar->drawCircleMap(position, 12, color, false);
    BWAPI::Broodwar->drawTextMap(
        position + BWAPI::Position(6, 0), "%c%d", label, tank->getID());
}

void MicroTanks::DrawTacticalInformation()
{
    if (the.selfRace() != BWAPI::Races::Terran || !the.bases.myMain())
    {
        return;
    }

    // Home-defense tanks do not reach executeMicro()'s attack-only path.
    // Draw from the global interface as well, with the once-per-frame guard
    // above preserving the actual enemy goal when attack micro already drew.
    Base* enemyBase = the.bases.enemyStart();
    const BWAPI::Position enemyGoal = enemyBase
        ? enemyBase->getCenter()
        : BWAPI::Position(BWAPI::Broodwar->mapWidth() * 16,
                          BWAPI::Broodwar->mapHeight() * 16);
    drawTankTopologyDebug(enemyGoal);
}

static BWAPI::Unit getClosestCompletedUnitOfType(
    BWAPI::UnitType type,
    const BWAPI::Position& from)
{
    if (!from.isValid())
    {
        return nullptr;
    }

    BWAPI::Unit best = nullptr;
    int bestDistance = MAX_DISTANCE;

    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (!unit ||
            !unit->exists() ||
            !unit->isCompleted() ||
            !unit->getPosition().isValid())
        {
            continue;
        }

        if (unit->getType() != type)
        {
            continue;
        }

        if (unit->getType().isBuilding() && unit->isLifted())
        {
            continue;
        }

        const int distance = unit->getDistance(from);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = unit;
        }
    }

    return best;
}

static BWAPI::Position getTankTurretCoverPosition(const BWAPI::Position& groupPosition)
{
    if (!groupPosition.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Unit closestTurret =
        getClosestCompletedUnitOfType(
            BWAPI::UnitTypes::Terran_Missile_Turret,
            groupPosition
        );

    if (closestTurret)
    {
        return closestTurret->getPosition();
    }

    return BWAPI::Positions::Invalid;
}

static int countCompletedFriendlyGoliaths()
{
    int count = 0;

    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (!unit ||
            !unit->exists() ||
            !unit->isCompleted() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Goliath)
        {
            continue;
        }

        ++count;
    }

    return count;
}

static BWAPI::Position getStablePureAirGoliathClusterCenter(
    int& selectedCount)
{
    selectedCount = 0;
    const int now = BWAPI::Broodwar->getFrameCount();

    if (now < pureAirGoliathLastEvaluationFrame)
    {
        pureAirGoliathClusterIDs.clear();
        pureAirGoliathClusterCenter = BWAPI::Positions::Invalid;
        pureAirGoliathClusterCount = 0;
        pureAirGoliathLastEvaluationFrame = -100000;
        pureAirGoliathLastSwitchFrame = -100000;
    }

    // executeMicro() is called once per spatial tank cluster. Rebuild the
    // global Goliath clustering at most once per second.
    if (pureAirGoliathLastEvaluationFrame >= 0 &&
        now - pureAirGoliathLastEvaluationFrame < 24 &&
        pureAirGoliathClusterCenter.isValid())
    {
        selectedCount = pureAirGoliathClusterCount;
        return pureAirGoliathClusterCenter;
    }

    pureAirGoliathLastEvaluationFrame = now;

    std::vector<BWAPI::Unit> goliaths;

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

        goliaths.push_back(unit);
    }

    if (goliaths.empty())
    {
        pureAirGoliathClusterIDs.clear();
        pureAirGoliathClusterCenter = BWAPI::Positions::Invalid;
        pureAirGoliathClusterCount = 0;
        return BWAPI::Positions::Invalid;
    }

    struct Cluster
    {
        std::vector<BWAPI::Unit> units;
        BWAPI::Position center = BWAPI::Positions::Invalid;
        int overlap = 0;
    };

    const int clusterLinkDistance = 7 * 32;
    std::vector<Cluster> clusters;
    std::set<int> visited;

    for (BWAPI::Unit seed : goliaths)
    {
        if (visited.find(seed->getID()) != visited.end())
        {
            continue;
        }

        Cluster cluster;
        std::vector<BWAPI::Unit> open;
        open.push_back(seed);
        visited.insert(seed->getID());

        while (!open.empty())
        {
            BWAPI::Unit current = open.back();
            open.pop_back();
            cluster.units.push_back(current);

            for (BWAPI::Unit candidate : goliaths)
            {
                if (visited.find(candidate->getID()) != visited.end())
                {
                    continue;
                }

                if (current->getDistance(candidate) <= clusterLinkDistance)
                {
                    visited.insert(candidate->getID());
                    open.push_back(candidate);
                }
            }
        }

        int sumX = 0;
        int sumY = 0;
        for (BWAPI::Unit unit : cluster.units)
        {
            sumX += unit->getPosition().x;
            sumY += unit->getPosition().y;
            if (pureAirGoliathClusterIDs.find(unit->getID()) !=
                pureAirGoliathClusterIDs.end())
            {
                ++cluster.overlap;
            }
        }
        cluster.center = BWAPI::Position(
            sumX / int(cluster.units.size()),
            sumY / int(cluster.units.size()));
        clusters.push_back(cluster);
    }

    BWAPI::Position frontReference = BWAPI::Positions::Invalid;
    Base* enemyStart = the.bases.enemyStart();
    if (enemyStart && enemyStart->getPosition().isValid())
    {
        frontReference = enemyStart->getPosition();
    }
    if (!frontReference.isValid())
    {
        frontReference = BWAPI::Position(
            BWAPI::Broodwar->mapWidth() * 16,
            BWAPI::Broodwar->mapHeight() * 16);
    }
    int largestIndex = 0;
    int currentIndex = -1;

    for (int i = 0; i < int(clusters.size()); ++i)
    {
        const int size = int(clusters[i].units.size());
        const int largestSize = int(clusters[largestIndex].units.size());
        const int distanceFront = frontReference.isValid()
            ? clusters[i].center.getApproxDistance(frontReference)
            : MAX_DISTANCE;
        const int largestDistanceFront = frontReference.isValid()
            ? clusters[largestIndex].center.getApproxDistance(frontReference)
            : MAX_DISTANCE;

        if (size > largestSize ||
            (size == largestSize && distanceFront < largestDistanceFront))
        {
            largestIndex = i;
        }

        if (clusters[i].overlap > 0 &&
            (currentIndex < 0 ||
                clusters[i].overlap > clusters[currentIndex].overlap))
        {
            currentIndex = i;
        }
    }

    int selectedIndex = largestIndex;
    if (currentIndex >= 0)
    {
        selectedIndex = currentIndex;
        const int currentSize = int(clusters[currentIndex].units.size());
        const int largestSize = int(clusters[largestIndex].units.size());
        const bool switchCooldownPassed =
            now - pureAirGoliathLastSwitchFrame >= 10 * 24;
        const bool replacementClearlyLarger =
            largestIndex != currentIndex &&
            largestSize * 100 >= currentSize * 140;

        if (switchCooldownPassed && replacementClearlyLarger)
        {
            selectedIndex = largestIndex;
        }
    }

    std::set<int> newIDs;
    for (BWAPI::Unit unit : clusters[selectedIndex].units)
    {
        newIDs.insert(unit->getID());
    }

    if (newIDs != pureAirGoliathClusterIDs)
    {
        bool sameCore = false;
        for (int id : newIDs)
        {
            if (pureAirGoliathClusterIDs.find(id) !=
                pureAirGoliathClusterIDs.end())
            {
                sameCore = true;
                break;
            }
        }
        if (!sameCore)
        {
            pureAirGoliathLastSwitchFrame = now;
        }
    }

    pureAirGoliathClusterIDs = newIDs;
    pureAirGoliathClusterCenter = clusters[selectedIndex].center;
    pureAirGoliathClusterCount = int(clusters[selectedIndex].units.size());
    selectedCount = pureAirGoliathClusterCount;
    return pureAirGoliathClusterCenter;
}

static BWAPI::Position getPureAirGoliathFrontCenter()
{
    if (!pureAirGoliathClusterCenter.isValid() ||
        pureAirGoliathClusterIDs.empty())
    {
        return BWAPI::Positions::Invalid;
    }

    std::vector<BWAPI::Unit> core;
    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (!unit ||
            !unit->exists() ||
            !unit->isCompleted() ||
            !unit->getPosition().isValid() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Goliath ||
            pureAirGoliathClusterIDs.find(unit->getID()) ==
                pureAirGoliathClusterIDs.end())
        {
            continue;
        }

        core.push_back(unit);
    }

    if (core.empty())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position forward =
        getRealFrontForwardVectorForTanks(pureAirGoliathClusterCenter);

    double fx = double(forward.x);
    double fy = double(forward.y);
    double length = std::sqrt(fx * fx + fy * fy);

    if (length < 1.0)
    {
        Base* enemyStart = the.bases.enemyStart();
        if (enemyStart && enemyStart->getPosition().isValid())
        {
            fx = double(enemyStart->getPosition().x - pureAirGoliathClusterCenter.x);
            fy = double(enemyStart->getPosition().y - pureAirGoliathClusterCenter.y);
            length = std::sqrt(fx * fx + fy * fy);
        }
    }

    if (length < 1.0)
    {
        fx = 0.0;
        fy = -1.0;
    }

    std::sort(
        core.begin(),
        core.end(),
        [fx, fy](BWAPI::Unit a, BWAPI::Unit b)
        {
            const double aProjection =
                double(a->getPosition().x) * fx +
                double(a->getPosition().y) * fy;
            const double bProjection =
                double(b->getPosition().x) * fx +
                double(b->getPosition().y) * fy;

            if (aProjection != bProjection)
            {
                return aProjection > bProjection;
            }

            return a->getID() < b->getID();
        });

    // Follow a small front section, not the center of every Goliath. Fresh
    // reinforcements at home therefore cannot pull the tanks backward.
    const int frontCount = std::min(
        int(core.size()),
        std::min(6, std::max(3, (int(core.size()) + 2) / 3)));

    long long sumX = 0;
    long long sumY = 0;
    for (int i = 0; i < frontCount; ++i)
    {
        sumX += core[i]->getPosition().x;
        sumY += core[i]->getPosition().y;
    }

    return BWAPI::Position(
        int(sumX / frontCount),
        int(sumY / frontCount));
}

static BWAPI::Position getPureAirTankEscortGoal()
{
    const BWAPI::Position frontCenter = getPureAirGoliathFrontCenter();
    if (!frontCenter.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position forward =
        getRealFrontForwardVectorForTanks(frontCenter);

    const double fx = double(forward.x);
    const double fy = double(forward.y);
    const double length = std::sqrt(fx * fx + fy * fy);

    if (length < 1.0)
    {
        return frontCenter;
    }

    // Put the Tank front behind the rear of the local Goliath core. An
    // offset from its vanguard fell INSIDE a deep Goliath formation.
    double rear = 0.0;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || unit->getType() != BWAPI::UnitTypes::Terran_Goliath ||
            !pureAirGoliathClusterIDs.count(unit->getID()) ||
            unit->getDistance(frontCenter) > 10 * 32) continue;
        const BWAPI::Position p = unit->getPosition();
        rear = std::min(rear, (p.x - frontCenter.x) * fx / length +
            (p.y - frontCenter.y) * fy / length);
    }
    BWAPI::Position desired(
        int(frontCenter.x + fx / length * (rear - 4 * 32)),
        int(frontCenter.y + fy / length * (rear - 4 * 32)));

    desired = clampToMapForTanks(desired);
    const BWAPI::Position walkable =
        findNearbyWalkableTankGatherPosition(desired);

    return walkable.isValid() ? walkable : frontCenter;
}

static int countFriendlyGoliathsNearPosition(
    const BWAPI::Position& pos,
    int radius)
{
    if (!pos.isValid())
    {
        return 0;
    }

    int count = 0;

    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (!unit ||
            !unit->exists() ||
            !unit->isCompleted() ||
            !unit->getPosition().isValid())
        {
            continue;
        }

        if (unit->getType() != BWAPI::UnitTypes::Terran_Goliath)
        {
            continue;
        }

        if (unit->getDistance(pos) <= radius)
        {
            ++count;
        }
    }

    return count;
}

static BWAPI::Position findCompactTankParkingPosition(const BWAPI::Position& desired)
{
    if (!desired.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position clamped = clampToMapForTanks(desired);
    if (tankParkingTerrainIsClear(clamped))
    {
        return clamped;
    }

    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestDistance = MAX_DISTANCE;

    // Do not let one blocked slot throw a tank half a screen away. Search only
    // a compact local neighborhood; if it is full, normal movement will choose
    // another route/anchor on a later update.
    for (int dx = -3; dx <= 3; ++dx)
    {
        for (int dy = -3; dy <= 3; ++dy)
        {
            BWAPI::Position candidate = clampToMapForTanks(
                BWAPI::Position(clamped.x + dx * 32, clamped.y + dy * 32));

            if (!tankParkingTerrainIsClear(candidate))
            {
                continue;
            }

            const int distance = candidate.getApproxDistance(clamped);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = candidate;
            }
        }
    }

    return best;
}

static BWAPI::Position findReachableTankGatherPosition(
    BWAPI::Unit tank,
    const BWAPI::Position& desired,
    const BWAPI::Position& anchor,
    const std::vector<BWAPI::Position>* reserved = nullptr,
    const BWAPI::Position* passageCenter = nullptr,
    const BWAPI::Position* passageForward = nullptr)
{
    if (!tank || !tank->exists() || !tank->getPosition().isValid() ||
        !desired.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestScore = MAX_DISTANCE;

    // This is a static-terrain recovery only. BWAPI::Broodwar->hasPath() is
    // intentionally NOT used here: calling it dozens of times per Tank from a
    // per-frame gather loop caused the large recurrent frame spikes seen on
    // Benzene. MapPartitions already answers the needed connectivity question.
    for (int radius = 0; radius <= 6; ++radius)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            for (int dy = -radius; dy <= radius; ++dy)
            {
                if (radius > 0 && std::abs(dx) != radius && std::abs(dy) != radius)
                {
                    continue;
                }

                BWAPI::Position candidate = clampToMapForTanks(
                    BWAPI::Position(desired.x + dx * 32, desired.y + dy * 32));

                bool claimed = false;
                if (reserved)
                    for (const BWAPI::Position& other : *reserved)
                        if (candidate.getApproxDistance(other) < 64) { claimed = true; break; }
                if (claimed || !candidate.isValid() ||
                    !tankParkingTerrainIsClear(candidate) ||
                    tankParkingOccupiedByAnotherTank(candidate, tank) ||
                    !the.partitions.connected(tank->getPosition(), candidate) ||
                    (passageCenter && passageForward &&
                     positionBlocksDefensePassage(
                         candidate, *passageCenter, *passageForward)))
                {
                    continue;
                }

                const int desiredDistance = candidate.getApproxDistance(desired);
                const int anchorDistance = anchor.isValid()
                    ? candidate.getApproxDistance(anchor)
                    : 0;
                const int score = desiredDistance + anchorDistance / 4;

                if (score < bestScore)
                {
                    bestScore = score;
                    best = candidate;
                }
            }
        }
        if (best.isValid()) break;
    }
    return best;
}

BWAPI::Position MicroTanks::getHoldPosition() const
{
    BWAPI::Position realFront = getRealFrontPositionForTanks();

    if (realFront.isValid())
    {
        return findNearbyWalkableTankGatherPosition(realFront);
    }

    if (the.bases.myStart() && the.bases.myStart()->getCenter().isValid())
    {
        return findNearbyWalkableTankGatherPosition(the.bases.myStart()->getCenter());
    }

    return findNearbyWalkableTankGatherPosition(
        BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation())
    );
}

// MASS GATHER slot placement uses static terrain only. Dynamic building
// occupancy is deliberately excluded: a new Depot/Factory must not invalidate
// and rebuild the complete Tank formation. Normal StarCraft movement and the
// rare per-Tank recovery path handle a temporarily blocked slot.
static BWAPI::Position findCompactStaticTankGatherSlot(
    const BWAPI::Position& desired,
    const std::vector<BWAPI::Position>* reservedPositions = nullptr)
{
    if (!desired.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    auto isReserved = [reservedPositions](const BWAPI::Position& candidate)
    {
        if (!reservedPositions)
        {
            return false;
        }
        for (const BWAPI::Position& reserved : *reservedPositions)
        {
            if (reserved.isValid() &&
                reserved.getApproxDistance(candidate) < 56)
            {
                return true;
            }
        }
        return false;
    };

    const BWAPI::Position clamped = clampToMapForTanks(desired);
    if (tankParkingStaticTerrainIsClear(clamped) && !isReserved(clamped))
    {
        return clamped;
    }

    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestDistance = MAX_DISTANCE;
    // The desired rear-row slot may fall inside a depot safety ring. Four
    // tiles of local search are enough to move it forward/sideways without
    // collapsing several Tanks onto the shared center.
    for (int dx = -6; dx <= 6; ++dx)
    {
        for (int dy = -6; dy <= 6; ++dy)
        {
            const BWAPI::Position candidate = clampToMapForTanks(
                BWAPI::Position(clamped.x + dx * 32, clamped.y + dy * 32));
            if (!tankParkingStaticTerrainIsClear(candidate) ||
                isReserved(candidate))
            {
                continue;
            }

            const int distance = candidate.getApproxDistance(clamped);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = candidate;
            }
        }
    }
    return best;
}

// Mobile tanks assemble into one compact checkerboard facing the objective.
// After release they remain one attack group; this layout is only the stable
// gathering formation.
static BWAPI::Position getMobileTankFormationSlotPosition(
    int index,
    const BWAPI::Position& center,
    const BWAPI::Position& forward,
    const std::vector<BWAPI::Position>* reservedPositions = nullptr)
{
    if (!center.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    double fx = double(forward.x);
    double fy = double(forward.y);
    double length = std::sqrt(fx * fx + fy * fy);

    if (length < 1.0)
    {
        fx = 0.0;
        fy = -1.0;
        length = 1.0;
    }

    fx /= length;
    fy /= length;
    const double sx = -fy;
    const double sy = fx;

    // Face a useful firing line toward the enemy. Two Tanks per row made only
    // the first pair point outward while every reinforcement accumulated back
    // beside the Command Center. Four compact columns keep the formation much
    // wider than one pair but still narrow enough for ramps and base traffic.
    const int slotsPerRow = 4;
    const int row = index / slotsPerRow;
    const int column = index % slotsPerRow;
    const double side = (double(column) - 1.5) * 72.0;
    const double back = -double(row * 88);

    BWAPI::Position desired(
        int(center.x + sx * side + fx * back),
        int(center.y + sy * side + fy * back));

    BWAPI::Position result = findCompactStaticTankGatherSlot(
        desired, reservedPositions);
    if (!result.isValid())
    {
        return findCompactStaticTankGatherSlot(center, reservedPositions);
    }

    // Never assign a pretty formation slot on the wrong connected component.
    // Do NOT build a full GridDistances field for every decorative slot here:
    // this function is used for many Tanks at once and the old distance query
    // was one of the largest MASS GATHER frame spikes. Static connectivity is
    // all that is required for this validation.
    if (!the.partitions.connected(center, result))
    {
        return findCompactStaticTankGatherSlot(center, reservedPositions);
    }

    return result;
}

BWAPI::Position MicroTanks::getDefensiveFrontTankPosition(
    int index,
    int totalTanks,
    const BWAPI::Position& center,
    const BWAPI::Position& enemyPosition
) const
{
    (void)totalTanks;

    if (!center.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position enemy = enemyPosition;
    if (!enemy.isValid())
    {
        enemy = getTankAnchorGoal(enemyPosition);
    }
    if (!enemy.isValid())
    {
        enemy = center + BWAPI::Position(0, -320);
    }

    double forwardX = double(enemy.x - center.x);
    double forwardY = double(enemy.y - center.y);
    double len = std::sqrt(forwardX * forwardX + forwardY * forwardY);
    if (len < 1.0)
    {
        forwardX = 0.0;
        forwardY = -1.0;
        len = 1.0;
    }
    forwardX /= len;
    forwardY /= len;

    double sideX = -forwardY;
    double sideY = forwardX;

    // Align the line to the actual entrance when available.
    Base* frontBase = getRealFrontBaseForTanks();
    const ChokePoint* frontChoke = frontBase ? frontBase->getFrontChoke() : nullptr;
    if (frontChoke &&
        frontChoke->center().isValid() &&
        center.getApproxDistance(frontChoke->center()) <= 8 * 32 &&
        frontChoke->endA().isValid() && frontChoke->endB().isValid())
    {
        const double edgeX = double(frontChoke->endB().x - frontChoke->endA().x);
        const double edgeY = double(frontChoke->endB().y - frontChoke->endA().y);
        const double edgeLength = std::sqrt(edgeX * edgeX + edgeY * edgeY);
        if (edgeLength > 1.0)
        {
            sideX = edgeX / edgeLength;
            sideY = edgeY / edgeLength;
        }
    }

    static const int sideSlots[2] = { -56, 56 };
    const int slotsPerRow = 2;
    const int row = index / slotsPerRow;
    const int slot = index % slotsPerRow;
    const double localSide = double(sideSlots[slot]);
    const double localForward = -double(row * 88);

    BWAPI::Position desired(
        int(center.x + forwardX * localForward + sideX * localSide),
        int(center.y + forwardY * localForward + sideY * localSide));

    return findPassageSafeTankPosition(
        desired,
        center,
        BWAPI::Position(int(forwardX * 320), int(forwardY * 320)));
}

void MicroTanks::doDefensiveFrontForAttackTanks(
    const BWAPI::Unitset& tanks,
    const BWAPI::Unitset& targets
)
{
    if (tanks.empty())
    {
        return;
    }

    BWAPI::Unit wallBunker = getActiveWallBunker();
    BWAPI::Position center = wallBunker ? wallBunker->getPosition() : getHoldPosition();

    if (!center.isValid())
    {
        Base* defensiveBase = the.bases.myNatural();

        if (!defensiveBase || defensiveBase->getOwner() != BWAPI::Broodwar->self())
        {
            defensiveBase = the.bases.myMain();
        }

        if (defensiveBase && defensiveBase->getPosition().isValid())
        {
            center = defensiveBase->getPosition();
        }
    }

    if (!center.isValid())
    {
        return;
    }

    const BWAPI::Position wallGate = the.placer.getTerranMainWallGateCenter();
    BWAPI::Position frontDirection = wallBunker && the.bases.myMain()
        ? wallGate - the.bases.myMain()->getCenter()
        : getRealFrontForwardVectorForTanks(center);
    BWAPI::Position enemyPosition = center + frontDirection;

    std::vector<BWAPI::Unit> orderedTanks;

    for (BWAPI::Unit tank : getUnits())
    {
        if (!tank || !tank->exists())
        {
            continue;
        }

        orderedTanks.push_back(tank);
    }

    if (orderedTanks.empty())
    {
        return;
    }

    std::sort(
        orderedTanks.begin(),
        orderedTanks.end(),
        [](BWAPI::Unit a, BWAPI::Unit b)
        {
            return a->getID() < b->getID();
        }
    );

    const int now = BWAPI::Broodwar->getFrameCount();

    const int siegeTankRange =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();

    const int FRONT_SLOT_RADIUS = wallBunker ? 6 : 48;

    // Defensive slot generation is also a terrain-search operation. It used
    // to run for every Tank on every frame, including up to 81 candidate tests
    // per slot on irregular terrain. Cache the whole plan for half a second;
    // rebuild immediately when the Tank set, anchor, map partition or front
    // direction or an actually occupied cached slot changes. There is deliberately
    // NO timer expiry: a stable tank line must not wake up every few seconds and redo the
    // same terrain search.
    unsigned long long defenseTankSignature = 1469598103934665603ULL;
    for (BWAPI::Unit defenseTank : orderedTanks)
    {
        defenseTankSignature ^= static_cast<unsigned long long>(defenseTank->getID() + 1);
        defenseTankSignature *= 1099511628211ULL;
    }

    const unsigned int defensePartitionRevision = the.partitions.getRevision();
    const unsigned int defenseRuntimeRevision = the.map.runtimeRevision();
    const bool defenseGeometryStable =
        tankDefensePlanCache.center.isValid() &&
        tankDefensePlanCache.bunkerID == (wallBunker ? wallBunker->getID() : -1) &&
        tankDefensePlanCache.partitionRevision == defensePartitionRevision &&
        tankDefensePlanCache.center.getApproxDistance(center) <= 16 &&
        std::abs(tankDefensePlanCache.frontDirection.x - frontDirection.x) <= 32 &&
        std::abs(tankDefensePlanCache.frontDirection.y - frontDirection.y) <= 32;
    const bool defenseStaticCacheValid =
        defenseGeometryStable &&
        tankDefensePlanCache.tankCount == int(orderedTanks.size()) &&
        tankDefensePlanCache.tankSignature == defenseTankSignature &&
        tankDefensePlanCache.byTankID.size() == orderedTanks.size();

    bool defenseRuntimeCacheValid = defenseStaticCacheValid;
    if (defenseRuntimeCacheValid &&
        tankDefensePlanCache.runtimeRevision != defenseRuntimeRevision)
    {
        // A new/lifted building is common macro activity, not a reason to redo
        // every defensive slot. Check the existing slots once and keep the
        // cached formation unless one of them is actually occupied.
        for (const auto& entry : tankDefensePlanCache.byTankID)
        {
            if (entry.second.slot.isValid() &&
                !tankParkingTerrainIsClear(entry.second.slot))
            {
                defenseRuntimeCacheValid = false;
                break;
            }
        }
        if (defenseRuntimeCacheValid)
        {
            tankDefensePlanCache.runtimeRevision = defenseRuntimeRevision;
        }
    }

    const bool defensePlanCacheValid =
        defenseStaticCacheValid && defenseRuntimeCacheValid;

    if (!defensePlanCacheValid)
    {
        TankPlanWork& work = tankPlanWork();
        ScopedTankPlanWork planTime(work.defenseMs, work.defenseBuilds);
        // A new Tank (or a casualty) does not invalidate the surviving Tanks'
        // positions. Rechecking all terrain candidates here made one frame
        // proportional to the entire army instead of the changed members.
        std::map<int, TankDefensePlanEntry> retained;
        if (defenseGeometryStable)
        {
            for (BWAPI::Unit tank : orderedTanks)
            {
                const auto old = tankDefensePlanCache.byTankID.find(tank->getID());
                if (old != tankDefensePlanCache.byTankID.end() &&
                    old->second.slot.isValid() &&
                    (tankDefensePlanCache.runtimeRevision == defenseRuntimeRevision ||
                     tankParkingTerrainIsClear(old->second.slot)))
                {
                    retained[tank->getID()] = old->second;
                }
            }
        }
        tankDefensePlanCache = TankDefensePlanCache();
        tankDefensePlanCache.center = center;
        tankDefensePlanCache.bunkerID = wallBunker ? wallBunker->getID() : -1;
        tankDefensePlanCache.frontDirection = frontDirection;
        tankDefensePlanCache.partitionRevision = defensePartitionRevision;
        tankDefensePlanCache.runtimeRevision = defenseRuntimeRevision;
        tankDefensePlanCache.frame = now;
        tankDefensePlanCache.tankCount = int(orderedTanks.size());
        tankDefensePlanCache.tankSignature = defenseTankSignature;

        const std::vector<const ChokePoint*> defensiveChokes =
            getDefensiveTankChokes();
        Base* frontBaseForDefense = getRealFrontBaseForTanks();
        const BWAPI::Position frontBaseCenter =
            frontBaseForDefense && frontBaseForDefense->getCenter().isValid()
            ? frontBaseForDefense->getCenter()
            : center;

        std::vector<BWAPI::Position> reservedFrontSlots;
        tankDefensePlanCache.byTankID = retained;
        for (const auto& old : retained)
        {
            reservedFrontSlots.push_back(old.second.slot);
        }
        for (int i = 0; i < int(orderedTanks.size()); ++i)
        {
            BWAPI::Unit tank = orderedTanks[i];
            if (!tank || !tank->exists() ||
                retained.find(tank->getID()) != retained.end())
            {
                continue;
            }

            TankDefensePlanEntry entry;
            entry.passageCenter = center;
            entry.passageForward = frontDirection;

            if (wallBunker)
            {
                entry.passageCenter = wallGate;
                entry.slot = findWallBunkerTankSlot(
                    tank, wallBunker, wallGate, frontDirection, reservedFrontSlots);
            }
            else if (!defensiveChokes.empty())
            {
                const int gateCount = int(defensiveChokes.size());
                const int gateIndex = i % gateCount;
                const int localIndex = i / gateCount;
                const ChokePoint* choke = defensiveChokes[gateIndex];

                entry.slot = getCompactTankSlotAtChoke(
                    localIndex, choke, frontBaseCenter, reservedFrontSlots);
                entry.passageCenter = getTankDefenseAnchorForChoke(
                    choke, frontBaseCenter);
                if (choke && choke->center().isValid() && entry.passageCenter.isValid())
                {
                    entry.passageForward = choke->center() - entry.passageCenter;
                }
            }
            else
            {
                entry.slot = getDefensiveFrontTankPosition(
                    i,
                    int(orderedTanks.size()),
                    center,
                    enemyPosition);
            }

            // Compact choke rows can run out of legal tiles for a large Tank
            // group. Give overflow Tanks a reachable reserve slot instead of
            // leaving them idle at their last position behind the army.
            if (!wallBunker && !entry.slot.isValid())
            {
                BWAPI::Position reserveDesired =
                    getDefensiveFrontTankPosition(
                        i, int(orderedTanks.size()), center, enemyPosition);
                if (!reserveDesired.isValid()) reserveDesired = center;
                const BWAPI::Position reserve =
                    findReachableTankGatherPosition(
                        tank, reserveDesired, center, &reservedFrontSlots,
                        &entry.passageCenter, &entry.passageForward);
                if (reserve.isValid())
                {
                    entry.slot = reserve;
                }
            }

            if (entry.slot.isValid())
            {
                reservedFrontSlots.push_back(entry.slot);
            }
            tankDefensePlanCache.byTankID[tank->getID()] = entry;
        }
    }

    for (BWAPI::Unit tank : orderedTanks)
    {
        if (!tank || !tank->exists())
        {
            continue;
        }

        TankDecisionState& st = stateFor(tank);
        BWAPI::Position passageCenter = center;
        BWAPI::Position passageForward = frontDirection;
        BWAPI::Position slot = BWAPI::Positions::Invalid;

        const auto defensePlan = tankDefensePlanCache.byTankID.find(tank->getID());
        if (defensePlan != tankDefensePlanCache.byTankID.end())
        {
            slot = defensePlan->second.slot;
            passageCenter = defensePlan->second.passageCenter;
            passageForward = defensePlan->second.passageForward;
        }
        if (!tanks.contains(tank) ||
            tank->getOrder() == BWAPI::Orders::Sieging ||
            tank->getOrder() == BWAPI::Orders::Unsieging)
        {
            continue;
        }
        auto alternateSlot = [&](const BWAPI::Position& desired)
        {
            if (!wallBunker)
                return findReachableTankGatherPosition(tank, desired, center,
                    nullptr, &passageCenter, &passageForward);
            std::vector<BWAPI::Position> reserved;
            for (const auto& entry : tankDefensePlanCache.byTankID)
                if (entry.first != tank->getID() && entry.second.slot.isValid())
                    reserved.push_back(entry.second.slot);
            for (const auto& entry : _tankState)
                if (entry.first != tank && entry.second.gatherRecoveryPosition.isValid() &&
                    entry.second.gatherRecoveryAnchor == center)
                    reserved.push_back(entry.second.gatherRecoveryPosition);
            return findWallBunkerTankSlot(tank, wallBunker, wallGate,
                frontDirection, reserved, slot);
        };
        // An invalid cached slot may become available when traffic moves.
        // Retry one Tank at a time on a short cooldown instead of leaving it
        // permanently parked outside the defensive line.
        if (!slot.isValid() &&
            (now < st.lastGatherRecoveryFrame ||
             now - st.lastGatherRecoveryFrame >= 48) &&
            (now + tank->getID()) % 48 == 0)
        {
            slot = alternateSlot(center);
            st.lastGatherRecoveryFrame = now;
            if (defensePlan != tankDefensePlanCache.byTankID.end())
                defensePlan->second.slot = slot;
        }
        if (!slot.isValid())
        {
            handleTankEmergencyBeforeForcedMove(
                tank, targets, now, siegeTankRange, 64, 72, 48,
                st.lastUnsiegeFrame, st.holdTankUntil, st.lastSiegeFrame);
            continue;
        }

        const bool sameRecoveryAnchor =
            st.gatherRecoveryAnchor.isValid() &&
            st.gatherRecoveryAnchor.getApproxDistance(center) <= 32;
        if (!sameRecoveryAnchor)
        {
            st.gatherRecoveryAnchor = BWAPI::Positions::Invalid;
            st.gatherRecoveryPosition = BWAPI::Positions::Invalid;
            st.gatherSiegeBlockedSince = -100000;
        }
        else if (st.gatherRecoveryPosition.isValid() &&
            tankParkingTerrainIsClear(st.gatherRecoveryPosition) &&
            !tankParkingOccupiedByAnotherTank(
                st.gatherRecoveryPosition, tank) &&
            the.partitions.connected(
                tank->getPosition(), st.gatherRecoveryPosition) &&
            !positionBlocksDefensePassage(
                st.gatherRecoveryPosition, passageCenter, passageForward) &&
            (!wallBunker || wallTankPositionSafe(
                st.gatherRecoveryPosition, tank, wallBunker, wallGate, frontDirection)))
        {
            slot = st.gatherRecoveryPosition;
        }

        if (!tank->isSieged())
        {
            const bool slotOccupied =
                tankParkingOccupiedByAnotherTank(slot, tank) ||
                !tankParkingTerrainIsClear(slot) ||
                (wallBunker && !wallTankPositionSafe(
                    slot, tank, wallBunker, wallGate, frontDirection));
            const bool moveStalled = tankParkingMovementStalled(
                tank,
                slot,
                now,
                st.parkingProgressTarget,
                st.parkingBestDistance,
                st.parkingLastProgressFrame, wallBunker ? 6 : 48);

            if ((slotOccupied || moveStalled) &&
                (now < st.lastGatherRecoveryFrame ||
                 now - st.lastGatherRecoveryFrame >= 48))
            {
                BWAPI::Position alternate = alternateSlot(slot + BWAPI::Position(
                    (tank->getID() & 1) ? 80 : -80, 48));
                st.lastGatherRecoveryFrame = now;
                if (alternate.isValid() &&
                    alternate.getApproxDistance(tank->getPosition()) > 24 &&
                    !positionBlocksDefensePassage(
                        alternate, passageCenter, passageForward))
                {
                    st.gatherRecoveryAnchor = center;
                    st.gatherRecoveryPosition = alternate;
                    slot = alternate;
                    st.parkingProgressTarget = alternate;
                    st.parkingBestDistance = tank->getPosition().getApproxDistance(alternate);
                    st.parkingLastProgressFrame = now;
                }
            }
        }

        if (wallBunker && !tank->isSieged() &&
            !wallTankPositionSafe(slot, tank, wallBunker, wallGate, frontDirection))
        {
            if (!handleTankEmergencyBeforeForcedMove(tank, targets, now, siegeTankRange,
                64, 72, 48, st.lastUnsiegeFrame, st.holdTankUntil, st.lastSiegeFrame))
                holdGatheringTank(tank);
            continue;
        }

        drawAssignedTankSlot(tank, slot, 'F', BWAPI::Colors::Green);

        BWAPI::Unitset localTargets;

        for (BWAPI::Unit enemy : targets)
        {
            if (!enemy || !enemy->exists())
            {
                continue;
            }

            if (enemy->isFlying())
            {
                continue;
            }

            if (!enemy->isVisible())
            {
                continue;
            }

            if (tank->getDistance(enemy) <= siegeTankRange + 4 * 32)
            {
                localTargets.insert(enemy);
            }
        }

        if (localTargets.empty())
        {
            for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
                tank->getPosition(),
                13 * 32,
                BWAPI::Filter::IsEnemy &&
                !BWAPI::Filter::IsFlyer&&
                BWAPI::Filter::IsDetected))
            {
                if (enemy && enemy->exists())
                {
                    localTargets.insert(enemy);
                }
            }
        }

        BWAPI::Unit target = nullptr;

        if (!localTargets.empty())
        {
            target = getTarget(tank, localTargets);
        }

        int distToSlot = wallBunker ? tank->getPosition().getApproxDistance(slot) : tank->getDistance(slot);
        bool blocksPassage =
            positionBlocksDefensePassage(
                tank->getPosition(),
                passageCenter,
                passageForward
            );

        if (tank->isSieged())
        {
            BWAPI::Unit tooCloseEnemy = nullptr;

            for (BWAPI::Unit enemy : localTargets)
            {
                if (!enemy || !enemy->exists())
                {
                    continue;
                }

                if (enemy->isFlying() || enemy->getType().isBuilding())
                {
                    continue;
                }

                if (tank->getDistance(enemy) <= 64)
                {
                    tooCloseEnemy = enemy;
                    break;
                }
            }

            if ((tooCloseEnemy && !targetInTankSiegeRange(tank, target)) ||
                tank->isUnderDisruptionWeb())
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "front emergency unsiege"
                );

                if (the.micro.Unsiege(tank, true))
                {
                    st.lastUnsiegeFrame = now;
                    st.holdTankUntil = now + 72;
                }
                continue;
            }

            if (target)
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "front fire"
                );

                the.micro.AttackUnit(tank, target);
            }
            else if (blocksPassage)
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "front clear passage"
                );

                if (the.micro.Unsiege(tank))
                {
                    st.lastUnsiegeFrame = now;
                    st.holdTankUntil = now + 48;
                }
            }
            else if (distToSlot > FRONT_SLOT_RADIUS)
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "front reposition");

                if (the.micro.Unsiege(tank))
                {
                    st.lastUnsiegeFrame = now;
                    st.holdTankUntil = now + 48;
                }
            }
            else
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "front hold siege"
                );

                holdGatheringTank(tank);
            }

            continue;
        }

        const bool parkingClear = tankParkingTerrainIsClear(tank->getPosition());
        const bool safeToDeploy = parkingClear &&
            tankMaySafelyEnterSiege(tank, localTargets, tanks);
        const bool readyForFrontSiege =
            BWAPI::Broodwar->self()->hasResearched(
                BWAPI::TechTypes::Tank_Siege_Mode) &&
            distToSlot <= FRONT_SLOT_RADIUS &&
            !tank->isUnderDisruptionWeb() &&
            now >= st.holdTankUntil &&
            now - st.lastUnsiegeFrame >= 48;

        if (readyForFrontSiege && (!parkingClear || blocksPassage || !tank->canSiege()))
        {
            if (st.gatherSiegeBlockedSince < 0 ||
                now < st.gatherSiegeBlockedSince)
            {
                st.gatherSiegeBlockedSince = now;
                st.gatherRecoveryAnchor = center;
            }

            if (now - st.gatherSiegeBlockedSince >= 12 &&
                (now < st.lastGatherRecoveryFrame ||
                 now - st.lastGatherRecoveryFrame >= 48))
            {
                BWAPI::Position alternate = alternateSlot(slot + BWAPI::Position(
                    (tank->getID() & 1) ? 80 : -80, 64));
                st.lastGatherRecoveryFrame = now;
                st.gatherSiegeBlockedSince = now;
                if (alternate.isValid() &&
                    alternate.getApproxDistance(tank->getPosition()) > 24 &&
                    !positionBlocksDefensePassage(
                        alternate, passageCenter, passageForward))
                {
                    st.gatherRecoveryAnchor = center;
                    st.gatherRecoveryPosition = alternate;
                    slot = alternate;
                    distToSlot = wallBunker ? tank->getPosition().getApproxDistance(slot) : tank->getDistance(slot);
                    blocksPassage = positionBlocksDefensePassage(
                        tank->getPosition(), passageCenter, passageForward);
                }
            }
            else if (!target)
            {
                holdGatheringTank(tank);
                continue;
            }
        }
        else if (tank->canSiege())
        {
            st.gatherSiegeBlockedSince = -100000;
        }

        if (now < st.holdTankUntil)
        {
            if (distToSlot > FRONT_SLOT_RADIUS || blocksPassage)
            {
                if (handleTankEmergencyBeforeForcedMove(
                    tank,
                    localTargets,
                    now,
                    siegeTankRange,
                    64,
                    72,
                    48,
                    st.lastUnsiegeFrame,
                    st.holdTankUntil,
                    st.lastSiegeFrame))
                {
                    continue;
                }

                the.micro.Move(tank, slot);
            }
            else
            {
                holdGatheringTank(tank);
            }

            continue;
        }

        if (target)
        {
            const int distToTarget = tank->getDistance(target);

            const bool enemyTooClose =
                distToTarget < 64 &&
                !target->getType().isBuilding();

            const bool canSiegeHere =
                distToSlot <= FRONT_SLOT_RADIUS &&
                !blocksPassage &&
                safeToDeploy &&
                distToTarget >= 64 &&
                distToTarget <= siegeTankRange &&
                !tank->isUnderDisruptionWeb();

            if (canSiegeHere && tank->canSiege())
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "front siege"
                );

                if (the.micro.Siege(tank))
                {
                    st.lastSiegeFrame = now;
                }
            }
            else if (enemyTooClose)
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "front kite"
                );

                the.micro.KiteTarget(tank, target);
            }
            else if (distToSlot > FRONT_SLOT_RADIUS || blocksPassage)
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "front move slot"
                );

                if (handleTankEmergencyBeforeForcedMove(
                    tank,
                    localTargets,
                    now,
                    siegeTankRange,
                    64,
                    72,
                    48,
                    st.lastUnsiegeFrame,
                    st.holdTankUntil,
                    st.lastSiegeFrame))
                {
                    continue;
                }

                the.micro.Move(tank, slot);
            }
            else
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "front guard"
                );

                holdGatheringTank(tank);
            }

            continue;
        }

        if (distToSlot > FRONT_SLOT_RADIUS || blocksPassage)
        {
            if (handleTankEmergencyBeforeForcedMove(
                tank,
                localTargets,
                now,
                siegeTankRange,
                64,
                72,
                48,
                st.lastUnsiegeFrame,
                st.holdTankUntil,
                st.lastSiegeFrame))
            {
                continue;
            }

            the.micro.Move(tank, slot);
        }
        else if (tank->canSiege() && safeToDeploy &&
            distToSlot <= FRONT_SLOT_RADIUS)
        {

            if (the.micro.Siege(tank))
            {
                st.lastSiegeFrame = now;
            }
        }
        else if (distToSlot > FRONT_SLOT_RADIUS)
        {

            the.micro.Move(tank, slot);
        }
        else
        {
            holdGatheringTank(tank);
        }
    }
}

static BWAPI::Unit getBenzenePowerGeneratorInSiegeRange(BWAPI::Unit tank)
{
    if (!tank ||
        !tank->exists() ||
        tank->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
    {
        return nullptr;
    }

    const std::string& mapName = BWAPI::Broodwar->mapFileName();
    if (mapName.find("Benzene") == std::string::npos &&
        mapName.find("benzene") == std::string::npos)
    {
        return nullptr;
    }

    const BWAPI::WeaponType weapon = tank->getType().groundWeapon();
    const int minRange = weapon.minRange();
    const int maxRange = weapon.maxRange() - 8;

    BWAPI::Unit best = nullptr;
    int bestDistance = MAX_DISTANCE;

    for (BWAPI::Unit neutral : BWAPI::Broodwar->getStaticNeutralUnits())
    {
        if (!neutral ||
            !neutral->exists() ||
            !neutral->isVisible() ||
            !neutral->isTargetable() ||
            neutral->isInvincible() ||
            neutral->getType() != BWAPI::UnitTypes::Special_Power_Generator)
        {
            continue;
        }

        const int distance = tank->getDistance(neutral);
        if (distance < minRange || distance > maxRange)
        {
            continue;
        }

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = neutral;
        }
    }

    return best;
}

// All gathering Tanks may clear Benzene's reachable neutral generator. This
// used to be available only to the permanent home-defense subset.
static void holdGatheringTank(BWAPI::Unit tank)
{
    if (tank && tank->exists() && tank->isSieged() &&
        !rememberedEnemyTankThreatForTank(tank) &&
        !hasVisibleEnemyGroundCombatNearPosition(tank->getPosition(), 13 * 32))
    {
        BWAPI::Unit blocker = getBenzenePowerGeneratorInSiegeRange(tank);
        if (blocker)
        {
            the.micro.AttackUnit(tank, blocker);
            return;
        }
    }
    the.micro.HoldPosition(tank);
}

static BWAPI::Unit getNearbyVisibleEnemyBuildingForTanks(
    const BWAPI::Position& groupCenter,
    const int maxRange)
{
    if (!groupCenter.isValid())
    {
        return nullptr;
    }

    BWAPI::Unit bestBuilding = nullptr;
    int bestGroundDistance = MAX_DISTANCE;
    int bestAirDistance = MAX_DISTANCE;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            enemy->isFlying() || enemy->isLifted())
        {
            continue;
        }

        const BWAPI::UnitType type = enemy->getType();

        if (!type.isBuilding())
        {
            continue;
        }

        if (type.isAddon())
        {
            continue;
        }

        if (!enemy->getPosition().isValid())
        {
            continue;
        }

        const int airDistance =
            groupCenter.getApproxDistance(enemy->getPosition());

        if (airDistance > maxRange)
        {
            continue;
        }

        const int groundDistance =
            tacticalDistanceForTanks(groupCenter, enemy->getPosition());

        // This cache is for finishing the base we already entered, not for
        // taking a straight-line shortcut to a cannon across a cliff.
        if (groundDistance == MAX_DISTANCE ||
            !tankGroundRouteIsDirectEnough(
                groupCenter,
                enemy->getPosition()))
        {
            continue;
        }

        if (groundDistance < bestGroundDistance ||
            (groundDistance == bestGroundDistance && airDistance < bestAirDistance))
        {
            bestGroundDistance = groundDistance;
            bestAirDistance = airDistance;
            bestBuilding = enemy;
        }
    }

    return bestBuilding;
}



void MicroTanks::executeMicro(const BWAPI::Unitset& targets, const UnitCluster& cluster)
{
    const auto tankCrashCheckpoint = [](const char * context)
    {
        if (Config::IO::EnableCrashLog)
        {
            Logger::SetCrashContext(
                context,
                BWAPI::Broodwar->getFrameCount());
        }
    };

    tankCrashCheckpoint("MicroTanks::executeMicro - cluster selection");

    const BWAPI::Unitset clusterTanks = Intersection(getUnits(), cluster.units);
    if (clusterTanks.empty())
    {
        return;
    }

    const int now = BWAPI::Broodwar->getFrameCount();

    const int siegeTankRange =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();

    const int TANK_HOLD_FRAMES = 36;
    const int POST_UNSIEGE_COOLDOWN = 48;
    const int SIEGE_ENTER_MARGIN = 16;
    const int SIEGE_EXIT_MARGIN = 48;
    const int EMERGENCY_UNSIEGE_DISTANCE = 64;
    const int EMERGENCY_RETREAT_FRAMES = 48;

    if (_frameContextFrame != now || now < _frameContextFrame)
    {
        _frameContextFrame = now;
        _cachedActiveTanks.clear();
        _cachedAttackTanks.clear();

        // Every Tank belongs to the common army. Base defense borrows a
        // temporary subset below instead of maintaining a separate garrison.
        for (BWAPI::Unit tank : getUnits())
        {
            if (tank && tank->exists())
            {
                _cachedAttackTanks.insert(tank);
            }
        }

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists())
            {
                continue;
            }

            if (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            {
                _cachedActiveTanks.insert(unit);
            }
        }

        cleanState(_cachedActiveTanks);
    }

    BWAPI::Unitset allAttackTanks = _cachedAttackTanks;

    BWAPI::Unitset tanks;
    for (BWAPI::Unit tank : allAttackTanks)
    {
        if (clusterTanks.contains(tank))
        {
            tanks.insert(tank);
        }
    }

    if (tanks.empty())
    {
        return;
    }

    if (!order || !order->isCombatOrder())
    {
        return;
    }

    // A Tank receives one decision per frame even if a caller supplies
    // overlapping spatial clusters. The shared observations may still refresh.
    for (auto it = tanks.begin(); it != tanks.end(); )
    {
        TankDecisionState& state = stateFor(*it);
        if (state.lastDecisionFrame == now)
        {
            it = tanks.erase(it);
        }
        else
        {
            state.lastDecisionFrame = now;
            ++it;
        }
    }
    if (tanks.empty())
    {
        return;
    }

    tankCrashCheckpoint("MicroTanks::executeMicro - target classification");

    const BWAPI::Position groundAttackedBasePosition =
        getCurrentlyAttackedOwnedBasePosition();

    // CombatCommander may correctly send a mixed Ground squad home for an
    // air-only raid because Marines/Goliaths in that squad can shoot up. Tanks
    // must not inherit that order: they cannot help against Mutalisks,
    // Wraiths or Carriers and used to drive all the way onto the Command
    // Center. Only a confirmed ground attacker enables Tank base defense.
    // A confirmed local ground attack owns the selected responder Tanks even
    // if the global Ground squad is currently aggressive or gathering. The
    // old status dependency allowed PureAir/front early returns to run before
    // the responder block and strand fresh Tanks at their production point.
    bool defendingAttackedBase =
        groundAttackedBasePosition.isValid();

    const bool airOnlyBaseDefenseOrder =
        order->getStatus() == "Defend attacked base" &&
        !groundAttackedBasePosition.isValid();

    BWAPI::Unitset tankTargets;
    std::copy_if(
        targets.begin(),
        targets.end(),
        std::inserter(tankTargets, tankTargets.end()),
        [](BWAPI::Unit u)
        {
            return meaningfulGroundCombatTargetForTanks(u) ||
                (visibleTankGroundTarget(u) &&
                 (u->getType() == BWAPI::UnitTypes::Zerg_Egg ||
                  u->getType() == BWAPI::UnitTypes::Zerg_Lurker_Egg));
        }
    );

    // The squad target list normally stops at each unit's sight radius.
    // Tanks can fire farther when an SCV/Vulture/Vessel supplies vision. Share
    // visible ground targets in every mode, including the pre-release gather.
    if (_sharedTargetFrame != now)
    {
        _sharedTargetFrame = now;
        _cachedSharedGroundTargets.clear();
        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!meaningfulGroundCombatTargetForTanks(enemy))
            {
                continue;
            }
            for (BWAPI::Unit member : getUnits())
            {
                if (member && member->exists() && enemy->getDistance(member) <= 16 * 32)
                {
                    _cachedSharedGroundTargets.insert(enemy);
                    break;
                }
            }
        }
    }
    tankTargets.insert(_cachedSharedGroundTargets.begin(), _cachedSharedGroundTargets.end());
    refreshCombatState(allAttackTanks, tankTargets);

    // Point-blank danger owns the Tank before formation, hold leases, target
    // priority or PureAir/front routing. Those later systems used to suppress
    // a valid retreat when another farther target could still be shelled, or
    // when two friendly Tanks made the support score look safe.
    const BWAPI::Position emergencyFallback = getHoldPosition();
    for (auto it = tanks.begin(); it != tanks.end(); )
    {
        BWAPI::Unit tank = *it;
        TankDecisionState& state = stateFor(tank);

        BWAPI::Unit pointBlankThreat =
            getClosestPointBlankTankThreat(
                tank,
                tankTargets,
                EMERGENCY_UNSIEGE_DISTANCE);

        bool retreatInProgress =
            state.retreatPosition.isValid() &&
            now < state.retreatUntil &&
            tank->getDistance(state.retreatPosition) > 48;

        if (!pointBlankThreat && !retreatInProgress)
        {
            ++it;
            continue;
        }

        if (pointBlankThreat)
        {
            const bool existingRetreatEscapesThreat =
                retreatInProgress &&
                state.retreatPosition.getApproxDistance(
                    pointBlankThreat->getPosition()) >
                    tank->getDistance(pointBlankThreat) + 32;

            if (!existingRetreatEscapesThreat)
            {
                state.retreatPosition =
                    getPointBlankTankRetreatPosition(
                        tank,
                        pointBlankThreat,
                        emergencyFallback);
            }

            state.retreatUntil = std::max(
                state.retreatUntil,
                now + EMERGENCY_RETREAT_FRAMES);
            retreatInProgress = state.retreatPosition.isValid();
        }

        state.holdTankUntil = std::max(
            state.holdTankUntil,
            now + TANK_HOLD_FRAMES);

        // A mode transition cannot be cancelled safely. Keep every lower
        // formation/routing branch away from this Tank until the animation
        // finishes; the stored retreat is executed on the first movable frame.
        if (tank->getOrder() == BWAPI::Orders::Sieging ||
            tank->getOrder() == BWAPI::Orders::Unsieging)
        {
            DrawUnitTargetTextMap(
                tank->getPosition() + BWAPI::Position(0, 8),
                pointBlankThreat
                    ? "point blank transition"
                    : "finish retreat transition");
            it = tanks.erase(it);
            continue;
        }

        if (tank->isSieged())
        {
            DrawUnitTargetTextMap(
                tank->getPosition() + BWAPI::Position(0, 8),
                "point blank unsiege");

            if (tank->canUnsiege())
            {
                if (the.micro.Unsiege(tank, true))
                {
                    state.lastUnsiegeFrame = now;
                }
            }
            else
            {
                the.micro.HoldPosition(tank);
            }
        }
        else if (retreatInProgress && state.retreatPosition.isValid())
        {
            DrawUnitTargetTextMap(
                tank->getPosition() + BWAPI::Position(0, 8),
                "point blank retreat");
            the.micro.Move(tank, state.retreatPosition);
        }
        else if (pointBlankThreat)
        {
            // No statically connected backstep was found. Firing is safer than
            // falling through to a formation Hold order beside the attacker.
            the.micro.AttackUnit(tank, pointBlankThreat);
        }

        it = tanks.erase(it);
    }

    // Construction owns a Tank standing inside an assigned CC footprint.
    // Otherwise the formation order immediately cancels the SCV's unsiege or
    // exit move and the same Tank returns to the building site.
    for (auto it = tanks.begin(); it != tanks.end(); )
    {
        if (BuildingManager::Instance().isClearingConstructionFootprint(*it))
        {
            it = tanks.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (tanks.empty())
    {
        return;
    }

    // Mobile repair is an extension of WorkerManager's existing repair owner:
    // it commands only SCVs, while Tank mode/movement stays here. Immediate
    // point-blank danger above always wins. The Tank never leaves its squad.
    for (auto it = tanks.begin(); it != tanks.end(); )
    {
        BWAPI::Unit tank = *it;
        if (!WorkerManager::Instance().isMobileRepairTarget(tank))
        {
            ++it;
            continue;
        }

        const BWAPI::Position repair =
            WorkerManager::Instance().getMobileRepairPosition(tank);
        if (!repair.isValid())
        {
            ++it;
            continue;
        }

        TankDecisionState& state = stateFor(tank);
        state.holdTankUntil = std::max(
            state.holdTankUntil, now + TANK_HOLD_FRAMES);
        DrawUnitTargetTextMap(
            tank->getPosition() + BWAPI::Position(0, 8), "REPAIR");

        if (tank->getOrder() == BWAPI::Orders::Sieging ||
            tank->getOrder() == BWAPI::Orders::Unsieging)
        {
            // Finish the uninterruptible mode transition first.
        }
        else if (tank->isSieged())
        {
            if (tank->canUnsiege() && the.micro.Unsiege(tank, true))
            {
                state.lastUnsiegeFrame = now;
            }
        }
        else if (tank->getDistance(repair) > 48)
        {
            the.micro.Move(tank, getTerrainTankRouteWaypoint(
                tank->getPosition(), repair));
        }
        else
        {
            the.micro.HoldPosition(tank);
        }
        it = tanks.erase(it);
    }

    if (tanks.empty())
    {
        return;
    }

    // Air-only orders must never seize Tank movement or siege state. Tanks
    // cannot shoot Mutalisks/Wraiths/Carriers; Marines/Goliaths/Vessels own
    // that response. Keep running the normal ground-target/front logic even if
    // the mixed Ground squad was temporarily given an air-defense order.
    (void)airOnlyBaseDefenseOrder;

    const TerranArmyPosture posture =
        StrategyManager::Instance().getTerranArmyPosture();
    const bool bioTankArmy =
        StrategyManager::Instance().bioTankPlanActive();

    // PureAir and LightMixed use the existing stable Goliath core as the mobile
    // anchor for inherited/support Tanks. Every local ground target, retreat,
    // siege-distance and defense rule below still wins; only empty-map movement
    // is leashed behind the Goliaths.
    BWAPI::Position pureAirEscortGoal = BWAPI::Positions::Invalid;
    bool pureAirEscortActive = false;

    const bool goliathRearSupportPosture =
        !bioTankArmy &&
        (posture == TerranArmyPosture::PureAir ||
         StrategyManager::Instance().isTerranLightMixed());

    if (goliathRearSupportPosture)
    {
        int goliathCoreSize = 0;
        const BWAPI::Position goliathCore =
            getStablePureAirGoliathClusterCenter(goliathCoreSize);
        if (goliathCore.isValid() && goliathCoreSize >= 4)
        {
            pureAirEscortGoal = getPureAirTankEscortGoal();
            pureAirEscortActive = pureAirEscortGoal.isValid();
        }
        pureAirTankEscortReleased = pureAirEscortActive;
    }
    else
    {
        pureAirTankEscortReleased = false;
    }

    if (!StrategyManager::Instance().defaultTankGatherEnabled() &&
        !tankMassAttackReleased)
    {
        tankMassAttackReleased = true;
        tankMassAttackReleaseFrame = now;
    }

    BWAPI::Position attackedBasePosition = groundAttackedBasePosition;

    if (attackedBasePosition.isValid())
    {
        const std::set<int> responderIDs =
            getTankBaseResponderIDs(_cachedAttackTanks, attackedBasePosition);
        BWAPI::Unitset respondingTanks;
        for (BWAPI::Unit tank : tanks)
        {
            if (responderIDs.find(tank->getID()) == responderIDs.end())
            {
                continue;
            }

            respondingTanks.insert(tank);
            TankDecisionState& state = stateFor(tank);
            if (tank->getOrder() == BWAPI::Orders::Sieging ||
                tank->getOrder() == BWAPI::Orders::Unsieging)
            {
                continue;
            }

            handleAttackTanksDefendOwnedBase(
                tank, attackedBasePosition, now, siegeTankRange,
                EMERGENCY_UNSIEGE_DISTANCE, TANK_HOLD_FRAMES, POST_UNSIEGE_COOLDOWN,
                state.lastUnsiegeFrame, state.holdTankUntil, state.lastSiegeFrame);
        }

        // Only the selected responders leave the army decision below. Returning
        // from the whole cluster here starved all its non-defending Tanks.
        for (BWAPI::Unit tank : respondingTanks)
        {
            tanks.erase(tank);
        }
        for (auto it = allAttackTanks.begin(); it != allAttackTanks.end(); )
        {
            if (responderIDs.count((*it)->getID()) != 0)
            {
                it = allAttackTanks.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (tanks.empty())
        {
            return;
        }

        // The selected responders own the base-defense job. The remaining
        // Tanks resume their ordinary gather/front/escort logic instead of
        // following the squad-wide base order as a second response wave.
        defendingAttackedBase = false;

    }

    // The old ground offensive order survives a strategy transition. Until
    // there is a real Goliath core to escort, inherited Tanks must defend the
    // normal wall/front positions instead of marching alone under enemy air.
    // Selected base responders above still handle an actual attack first.
    if (!bioTankArmy && posture == TerranArmyPosture::PureAir &&
        !pureAirEscortActive && !defendingAttackedBase)
    {
        doDefensiveFrontForAttackTanks(tanks, tankTargets);
        return;
    }

    // AirTanks has its own six-Tank/Wraith release gate. The generic Tank
    // gather system is disabled for this opening and otherwise falls through
    // to an enemy-base goal even while the Ground squad is ordered to hold.
    if (StrategyManager::Instance().airTanksPlanActive() &&
        AirTanks::variantFromName(StrategyManager::Instance().airTanksVariant()) ==
            AirTanks::Variant::TankWraith &&
        !StrategyManager::Instance().airTanksPushReady() &&
        !defendingAttackedBase)
    {
        doDefensiveFrontForAttackTanks(tanks, tankTargets);
        return;
    }

    // Bio disables the mech mass-gather gate. A "Defend front" order must
    // still hold every non-responder Tank at our front; otherwise the first
    // Tank can follow an enemy target through the wall before the Marine push.
    if (bioTankArmy &&
        !defendingAttackedBase &&
        (!order || order->getStatus().compare(0, 7, "Attack ") != 0 ||
         !CombatCommander::Instance().getAggression()))
    {
        _bioInfantryFront.reset();
        doDefensiveFrontForAttackTanks(tanks, tankTargets);
        return;
    }

    // BioTank advances behind its infantry. A distance-only leash still let a
    // Tank drive nine tiles ahead of Marines standing at the base entrance.
    // Keep a moving rear line for travel and let Tanks fire from it when an
    // enemy enters Siege range. Base responders above retain priority.
    BWAPI::Position bioTankRearLine = BWAPI::Positions::Invalid;
    if (!bioTankArmy || !order ||
        order->getStatus().compare(0, 7, "Attack ") != 0 ||
        !CombatCommander::Instance().getAggression())
    {
        _bioInfantryFront.reset();
    }
    if (bioTankArmy &&
        !defendingAttackedBase && order &&
        order->getStatus().compare(0, 7, "Attack ") == 0 &&
        CombatCommander::Instance().getAggression())
    {
        const BWAPI::Position infantryAnchor = _bioInfantryFront.get(
            CombatCommander::Instance().groundSquadUnits(),
            (*tanks.begin())->getPosition(), *order);

        if (!infantryAnchor.isValid())
        {
            // Losing the infantry screen must not cancel a useful artillery
            // shot and send every engaged Tank back to our base.
            for (auto it = tanks.begin(); it != tanks.end(); )
            {
                BWAPI::Unit tank = *it;
                BWAPI::Unit target = getTarget(tank, tankTargets);
                if (tank->isSieged() && target &&
                    targetInTankSiegeRange(tank, target))
                {
                    the.micro.AttackUnit(tank, target);
                    it = tanks.erase(it);
                }
                else if (!tank->isSieged() && target &&
                    tank->getDistance(target) <=
                        BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode
                            .groundWeapon().maxRange())
                {
                    the.micro.AttackUnit(tank, target);
                    it = tanks.erase(it);
                }
                else if (target && tank->canSiege() &&
                    target->getType() != BWAPI::UnitTypes::Terran_Vulture_Spider_Mine &&
                    targetInTankSiegeRange(tank, target) &&
                    !unitNearChokepoint(tank) &&
                    tankMaySafelyEnterSiege(tank, tankTargets, allAttackTanks))
                {
                    the.micro.Siege(tank);
                    it = tanks.erase(it);
                }
                else ++it;
            }
            if (tanks.empty()) return;
            // Tech and the total Marine count can be ready while the Marines
            // are still loaded in bunkers. Never send Tanks out alone.
            doDefensiveFrontForAttackTanks(tanks, tankTargets);
            return;
        }

        const BWAPI::Position attackGoal = order->getPosition();
        const BWAPI::Position routeFront = attackGoal.isValid()
            ? getTerrainTankRouteWaypoint(infantryAnchor, attackGoal)
            : BWAPI::Positions::Invalid;
        const BWAPI::Position formationFront = routeFront.isValid()
            ? routeFront : attackGoal;
        double forwardX = formationFront.isValid()
            ? double(formationFront.x - infantryAnchor.x) : 0.0;
        double forwardY = formationFront.isValid()
            ? double(formationFront.y - infantryAnchor.y) : 0.0;
        const double forwardLength = std::sqrt(
            forwardX * forwardX + forwardY * forwardY);
        if (forwardLength >= 32.0)
        {
            forwardX /= forwardLength;
            forwardY /= forwardLength;
            const BWAPI::Position desiredRearLine = clampToMapForTanks(
                BWAPI::Position(
                    int(infantryAnchor.x - forwardX * 3.5 * 32),
                    int(infantryAnchor.y - forwardY * 3.5 * 32)));
            bioTankRearLine =
                findNearbyWalkableTankGatherPosition(desiredRearLine);
            if (!bioTankRearLine.isValid())
            {
                bioTankRearLine = desiredRearLine;
            }

            // Infantry must pass a narrow lane before the much larger Tanks
            // take it. Look slightly ahead of and behind the Marine core: the
            // rear-line point alone can be open while a Tank on either mouth
            // of the passage still blocks the entire column.
            BWAPI::Position passageCenter = BWAPI::Positions::Invalid;
            int closestPassageStep = INT_MAX;
            for (int step = -10; step <= 4; ++step)
            {
                const BWAPI::Position sample = clampToMapForTanks(
                    BWAPI::Position(
                        int(infantryAnchor.x + forwardX * step * 32),
                        int(infantryAnchor.y + forwardY * step * 32)));
                const BWAPI::TilePosition tile(sample);
                if (!tile.isValid())
                {
                    continue;
                }
                const int room = the.tileRoom.at(tile);
                if (room > 0 && room <= 14 &&
                    std::abs(step) < closestPassageStep)
                {
                    passageCenter = sample;
                    closestPassageStep = std::abs(step);
                }
            }
            int infantryStillCrossing = 0;
            if (passageCenter.isValid())
            {
                for (BWAPI::Unit unit :
                     CombatCommander::Instance().groundSquadUnits())
                {
                    if (!unit || !unit->exists() || !unit->isCompleted() ||
                        unit->isLoaded() ||
                        (unit->getType() != BWAPI::UnitTypes::Terran_Marine &&
                         unit->getType() != BWAPI::UnitTypes::Terran_Medic &&
                         unit->getType() != BWAPI::UnitTypes::Terran_Firebat) ||
                        unit->getDistance(passageCenter) > 8 * 32)
                    {
                        continue;
                    }
                    const double progress =
                        double(unit->getPosition().x - passageCenter.x) * forwardX +
                        double(unit->getPosition().y - passageCenter.y) * forwardY;
                    if (progress <= 3 * 32)
                    {
                        ++infantryStillCrossing;
                    }
                }
            }
            // The centroid can already be beyond the choke while the tail of
            // a large Marine column is still inside it. Wait for the tail.
            const bool infantryNeedsLane = infantryStillCrossing >= 3;
            std::vector<BWAPI::Position> reservedYieldPositions;

            for (auto it = tanks.begin(); it != tanks.end(); )
            {
                BWAPI::Unit tank = *it;
                if (infantryNeedsLane &&
                    tank->getDistance(passageCenter) <= 7 * 32 &&
                    !tank->isUnderAttack())
                {
                    BWAPI::Unit localTarget = getTarget(tank, tankTargets);
                    if (!localTarget ||
                        tank->getDistance(localTarget) > siegeTankRange)
                    {
                        TankDecisionState& state = stateFor(tank);
                        const double tankProgress =
                            double(tank->getPosition().x - passageCenter.x) * forwardX +
                            double(tank->getPosition().y - passageCenter.y) * forwardY;
                        // Tanks at both mouths move AWAY from the lane, each
                        // to open ground on its present side. A Tank inside
                        // the gap backs out toward the incoming infantry.
                        const int side = tankProgress > 32.0 ? 1 : -1;
                        const double sideX = -forwardY;
                        const double sideY = forwardX;
                        const double lateral =
                            double(tank->getPosition().x - passageCenter.x) * sideX +
                            double(tank->getPosition().y - passageCenter.y) * sideY;
                        BWAPI::Position pocket = state.passageYieldPosition;
                        bool reserved = false;
                        for (const BWAPI::Position& other : reservedYieldPositions)
                        {
                            if (pocket.isValid() &&
                                other.getApproxDistance(pocket) < 64)
                            {
                                reserved = true;
                                break;
                            }
                        }
                        if (!pocket.isValid() ||
                            state.passageYieldCenter.getApproxDistance(passageCenter) > 64 ||
                            reserved ||
                            tankParkingOccupiedByAnotherTank(pocket, tank) ||
                            !tankParkingTerrainIsClear(pocket))
                        {
                            pocket = BWAPI::Positions::Invalid;
                            if (now >= state.passageYieldRetryFrame)
                            {
                                static const int lateralOffsets[] =
                                    { 0, 2, -2, 4, -4, 6, -6, 8, -8 };
                                for (int depth = 3; depth <= 12 && !pocket.isValid(); ++depth)
                                {
                                    for (int offset : lateralOffsets)
                                    {
                                        const BWAPI::Position candidate = clampToMapForTanks(
                                            BWAPI::Position(
                                                int(passageCenter.x + forwardX * side * depth * 32 +
                                                    sideX * (lateral + offset * 32)),
                                                int(passageCenter.y + forwardY * side * depth * 32 +
                                                    sideY * (lateral + offset * 32))));
                                        const BWAPI::TilePosition tile(candidate);
                                        if (!tile.isValid() || the.tileRoom.at(tile) <= 16 ||
                                            !tankParkingTerrainIsClear(candidate) ||
                                            tankParkingOccupiedByAnotherTank(candidate, tank) ||
                                            !the.partitions.connected(tank->getPosition(), candidate))
                                        {
                                            continue;
                                        }
                                        bool occupied = false;
                                        for (const BWAPI::Position& other : reservedYieldPositions)
                                        {
                                            if (other.getApproxDistance(candidate) < 64)
                                            {
                                                occupied = true;
                                                break;
                                            }
                                        }
                                        if (!occupied)
                                        {
                                            pocket = candidate;
                                            break;
                                        }
                                    }
                                }
                                state.passageYieldRetryFrame = now + (pocket.isValid() ? 8 : 24);
                            }
                            state.passageYieldPosition = pocket;
                            state.passageYieldCenter = passageCenter;
                        }

                        if (pocket.isValid())
                        {
                            reservedYieldPositions.push_back(pocket);
                        }
                        if (tank->isSieged())
                        {
                            the.micro.Unsiege(tank);
                        }
                        else if (tank->getOrder() != BWAPI::Orders::Unsieging &&
                                 tank->getOrder() != BWAPI::Orders::Sieging &&
                                 pocket.isValid() && tank->getDistance(pocket) > 40)
                        {
                            the.micro.Move(tank, getTerrainTankRouteWaypoint(
                                tank->getPosition(), pocket));
                        }
                        else if (!pocket.isValid() || tank->getDistance(pocket) <= 40)
                        {
                            the.micro.HoldPosition(tank);
                        }
                        it = tanks.erase(it);
                        continue;
                    }
                }
                const double forwardProgress =
                    double(tank->getPosition().x - infantryAnchor.x) * forwardX +
                    double(tank->getPosition().y - infantryAnchor.y) * forwardY;
                const bool aheadOfScreen = forwardProgress > -80.0;
                const bool separatedFromScreen =
                    tank->getDistance(infantryAnchor) > 10 * 32;
                if (!aheadOfScreen && !separatedFromScreen)
                {
                    ++it;
                    continue;
                }

                BWAPI::Unit localTarget = getTarget(tank, tankTargets);
                if (localTarget && tank->isSieged() &&
                    targetInTankSiegeRange(tank, localTarget))
                {
                    the.micro.AttackUnit(tank, localTarget);
                }
                else if (!aheadOfScreen && localTarget && tank->canSiege() &&
                    !tank->isUnderDisruptionWeb() &&
                    tank->getDistance(localTarget) >= EMERGENCY_UNSIEGE_DISTANCE &&
                    tank->getDistance(localTarget) <= siegeTankRange)
                {
                    the.micro.Siege(tank);
                }
                else if (localTarget && !tank->isSieged() &&
                    (tank->isUnderAttack() || !aheadOfScreen) &&
                    tank->getDistance(localTarget) <=
                        BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode
                            .groundWeapon().maxRange())
                {
                    the.micro.AttackUnit(tank, localTarget);
                }
                else if (tank->isSieged())
                {
                    the.micro.Unsiege(tank);
                }
                else if (tank->getOrder() != BWAPI::Orders::Unsieging &&
                         tank->getOrder() != BWAPI::Orders::Sieging &&
                         the.partitions.connected(
                             tank->getPosition(), bioTankRearLine))
                {
                    the.micro.Move(tank, getTerrainTankRouteWaypoint(
                        tank->getPosition(), bioTankRearLine));
                }
                it = tanks.erase(it);
            }
            if (tanks.empty())
            {
                return;
            }
        }
    }

    static BWAPI::Unit cachedNearbyBuildingTarget = nullptr;
    static BWAPI::Position cachedNearbyBuildingPosition = BWAPI::Positions::Invalid;
    static int lastNearbyBuildingCheckFrame = -100000;

    if (now < lastNearbyBuildingCheckFrame)
    {
        cachedNearbyBuildingTarget = nullptr;
        cachedNearbyBuildingPosition = BWAPI::Positions::Invalid;
        lastNearbyBuildingCheckFrame = -100000;
    }

    BWAPI::Position enemyGoal = BWAPI::Positions::Invalid;

    BWAPI::Position tankGroupReference =
        getFriendlyTankGroupCenter(allAttackTanks);

    if (!tankGroupReference.isValid() && order && order->getPosition().isValid())
    {
        tankGroupReference = order->getPosition();
    }

    if (!tankGroupReference.isValid())
    {
        Base* mainBase = the.bases.myMain();

        if (mainBase && mainBase->getPosition().isValid())
        {
            tankGroupReference = mainBase->getPosition();
        }
    }

    const bool cachedBuildingStillValid =
        cachedNearbyBuildingTarget &&
        cachedNearbyBuildingTarget->exists() &&
        cachedNearbyBuildingTarget->isVisible() &&
        cachedNearbyBuildingTarget->getType().isBuilding() &&
        !cachedNearbyBuildingTarget->isFlying() &&
        !cachedNearbyBuildingTarget->isLifted() &&
        cachedNearbyBuildingTarget->getPosition().isValid() &&
        tankGroupReference.isValid() &&
        tankGroupReference.getApproxDistance(cachedNearbyBuildingTarget->getPosition()) <= 20 * 32 &&
        tankGroundRouteIsDirectEnough(
            tankGroupReference,
            cachedNearbyBuildingTarget->getPosition());

    if (!cachedBuildingStillValid)
    {
        cachedNearbyBuildingTarget = nullptr;
        cachedNearbyBuildingPosition = BWAPI::Positions::Invalid;
    }

    if (!cachedNearbyBuildingTarget &&
        tankGroupReference.isValid() &&
        now - lastNearbyBuildingCheckFrame >= 24)
    {
        lastNearbyBuildingCheckFrame = now;

        cachedNearbyBuildingTarget =
            getNearbyVisibleEnemyBuildingForTanks(tankGroupReference, 18 * 32);

        if (cachedNearbyBuildingTarget &&
            cachedNearbyBuildingTarget->getPosition().isValid())
        {
            cachedNearbyBuildingPosition = cachedNearbyBuildingTarget->getPosition();
        }
    }

    if (cachedNearbyBuildingTarget && cachedNearbyBuildingTarget->exists())
    {
        tankTargets.insert(cachedNearbyBuildingTarget);
    }

    // Responders keep their own contact hold, but their distant base fight
    // must not move the attacking army's shared enemy-memory center homeward.
    updateTankGroupEnemyMemory(allAttackTanks, tankTargets, siegeTankRange);
    refreshTankGroupMemoryFromRememberedTanks(allAttackTanks);

    Base* orderedEnemyBase =
        order && CombatCommander::Instance().getAggression()
        ? order->getBase()
        : nullptr;

    if (orderedEnemyBase)
    {
        // The approach point is useful while the mass is gathering. After the
        // attack is released it lies outside the enemy base; keeping it as the
        // formation anchor made the "forward" tank turn around after defenders
        // died and pulled the rest of the tanks back out of the base.
        if (tankMassAttackReleased && order->getPosition().isValid())
        {
            enemyGoal = order->getPosition();
        }
        else
        {
            enemyGoal = getBaseApproachPositionForTanks(orderedEnemyBase);
        }

        if (!enemyGoal.isValid())
        {
            enemyGoal = getBaseFrontPositionForTanks(orderedEnemyBase);
        }

        if (!enemyGoal.isValid() && orderedEnemyBase->getPosition().isValid())
        {
            enemyGoal = orderedEnemyBase->getPosition();
        }

        if (!enemyGoal.isValid() && orderedEnemyBase->getCenter().isValid())
        {
            enemyGoal = orderedEnemyBase->getCenter();
        }
    }
    else if (order && order->getPosition().isValid() &&
        CombatCommander::Instance().getAggression())
    {
        enemyGoal = order->getPosition();
    }
    else
    {
        Base* enemyBase = the.bases.enemyStart();

        if (enemyBase && the.bases.enemyStartConfirmed())
        {
            enemyGoal = getBaseApproachPositionForTanks(enemyBase);

            if (!enemyGoal.isValid())
            {
                enemyGoal = getBaseFrontPositionForTanks(enemyBase);
            }
        }

        if (!enemyGoal.isValid())
        {
            enemyGoal = BWAPI::Position(
                BWAPI::Broodwar->mapWidth() * 16,
                BWAPI::Broodwar->mapHeight() * 16
            );
        }
    }

    if (pureAirEscortActive && pureAirEscortGoal.isValid())
    {
        enemyGoal = pureAirEscortGoal;
    }
    else if (order && order->getKey() == "air target approach" &&
        tankMassAttackReleased && order->getPosition().isValid())
    {
        // Goliaths take the airborne building. Tanks cannot damage it, so
        // inspect nearby ground while staying in the same enemy area instead
        // of parking permanently at the anti-air firing tile.
        static int lastAirSearchFrame = -1;
        static BWAPI::Position lastAirSearchOrder = BWAPI::Positions::Invalid;
        static BWAPI::Position airSearchGoal = BWAPI::Positions::Invalid;
        if (now < lastAirSearchFrame ||
            now - lastAirSearchFrame >= 24 ||
            lastAirSearchOrder != order->getPosition())
        {
            lastAirSearchOrder = order->getPosition();
            lastAirSearchFrame = now;
            airSearchGoal = MapGrid::Instance().getLeastExploredNear(
                order->getPosition(), true, false, 12 * 32);
        }
        if (airSearchGoal.isValid())
        {
            enemyGoal = airSearchGoal;
        }
    }
    else if (order && order->getKey() == "air target" &&
        tankGroupReference.isValid())
    {
        // The mixed squad may select a lifted Barracks or another air-only
        // target for its Goliaths. Tanks must hold their ground instead of
        // treating that airborne position as their own attack destination.
        enemyGoal = tankGroupReference;
    }

    tankCrashCheckpoint("MicroTanks::executeMicro - gathering and defense");

    bool hasVisibleMobileGroundTarget = false;
    for (BWAPI::Unit target : tankTargets)
    {
        if (target && target->exists() && !target->getType().isBuilding() &&
            !target->getType().isWorker())
        {
            hasVisibleMobileGroundTarget = true;
            break;
        }
    }

    if (cachedNearbyBuildingPosition.isValid() && !hasVisibleMobileGroundTarget)
    {
        enemyGoal = cachedNearbyBuildingPosition;
    }

    BWAPI::Position cleanupBuildingPosition = BWAPI::Positions::Invalid;

    if (!defendingAttackedBase &&
        tankMassAttackReleased &&
        tankTargets.empty() &&
        tankGroupReference.isValid() &&
        !hasVisibleEnemyGroundCombatNearPosition(tankGroupReference, 13 * 32))
    {
        cleanupBuildingPosition =
            getClosestVisibleEnemyBuildingGoalForTanks(tankGroupReference);

        if (!cachedNearbyBuildingPosition.isValid() &&
            cleanupBuildingPosition.isValid())
        {
            enemyGoal = cleanupBuildingPosition;
        }
    }

    drawTankTopologyDebug(enemyGoal);

    // The normal mass threshold is an intention, not a permanent parking
    // order. If production stops below that threshold, let an already useful
    // offensive force leave the main after a bounded wait. An attacked base
    // and a deliberately defensive order still keep their Tanks at home.
    const bool offensiveGatherOrder =
        order &&
        (order->getType() == SquadOrderTypes::Attack ||
         order->getType() == SquadOrderTypes::OmniAttack) &&
        order->getStatus().compare(0, 7, "Attack ") == 0;
    if (!tankMassAttackReleased && !defendingAttackedBase &&
        !pureAirEscortActive &&
        StrategyManager::Instance().defaultTankGatherEnabled() &&
        CombatCommander::Instance().getAggression() &&
        offensiveGatherOrder && now % 24 == 7)
    {
        const int attackTankCount = int(allAttackTanks.size());
        const int goliathCount = countCompletedFriendlyGoliaths();
        const bool mineralEmergency =
            StrategyManager::Instance().isTerranMineralExpansionEmergency();
        const int minimumForce = mineralEmergency ? 2 : 4;
        if (attackTankCount >= minimumForce)
        {
            if (tankGatherFirstFrame < 0 || tankGatherFirstFrame > now)
            {
                tankGatherFirstFrame = now;
                tankGatherLastGrowthFrame = now;
                tankGatherMaxTankCount = attackTankCount;
                tankGatherMaxGoliathCount = goliathCount;
            }
            if (attackTankCount > tankGatherMaxTankCount ||
                goliathCount > tankGatherMaxGoliathCount)
            {
                tankGatherMaxTankCount = std::max(
                    tankGatherMaxTankCount, attackTankCount);
                tankGatherMaxGoliathCount = std::max(
                    tankGatherMaxGoliathCount, goliathCount);
                tankGatherLastGrowthFrame = now;
            }

            Base* frontBase = the.bases.myFront();
            if (!frontBase) frontBase = the.bases.myMain();
            const bool frontUnderAttack = frontBase &&
                countEnemyCombatNearPosition(
                    frontBase->getCenter(), 12 * 32) > 0;
            const int stalledFor = now - tankGatherLastGrowthFrame;
            const int gatheredFor = now - tankGatherFirstFrame;
            if (!frontUnderAttack &&
                ((stalledFor >= (mineralEmergency ? 45 : 75) * 24) ||
                 gatheredFor >= 3 * 60 * 24))
            {
                tankMassAttackReleased = true;
                tankMassAttackReleaseFrame = now;
            }
        }
    }

    // A wall bunker is an opening hold, not a permanent veto on the release
    // threshold. With a mature mobile force, let the ordinary attack path run.
    if (!defendingAttackedBase && !tankMassAttackReleased &&
        StrategyManager::Instance().defaultTankGatherEnabled() &&
        CombatCommander::Instance().getAggression())
    {
        const int threshold = StrategyManager::Instance().tankMassReleaseTankCount();
        const int goliathThreshold =
            StrategyManager::Instance().tankMassReleaseGoliathCount();
        if (threshold > 0 && int(allAttackTanks.size()) >= std::max(8, 2 * threshold) &&
            (goliathThreshold <= 0 ||
             countCompletedFriendlyGoliaths() >= 2 * goliathThreshold))
        {
            tankMassAttackReleased = true;
            tankMassAttackReleaseFrame = now;
        }
    }

    if (!defendingAttackedBase && !tankMassAttackReleased && getActiveWallBunker())
    {
        doDefensiveFrontForAttackTanks(tanks, tankTargets);
        return;
    }

    if (!defendingAttackedBase && !pureAirEscortActive &&
        StrategyManager::Instance().defaultTankGatherEnabled() &&
        !tankMassAttackReleased)
    {
        const int ATTACK_TANKS_TO_RELEASE =
            StrategyManager::Instance().tankMassReleaseTankCount();

        const int GOLIATHS_TO_RELEASE =
            StrategyManager::Instance().tankMassReleaseGoliathCount();

        const int TANKS_NEAR_GATHER_TO_RELEASE =
            std::max(1, ATTACK_TANKS_TO_RELEASE - 2);

        const int GOLIATHS_NEAR_GATHER_TO_RELEASE =
            std::max(1, GOLIATHS_TO_RELEASE - 1);

        const int RELEASE_GATHER_RADIUS = 12 * 32;

        const int attackTankCount = int(allAttackTanks.size());

        // Air presence does not alter Tank gathering. Anti-air units handle it;
        // Tanks keep their normal ground formation/release logic.


        BWAPI::Position gatherPosition =
            !tankMassAttackReleased
            ? getTankMassGatherPosition(enemyGoal)
            : BWAPI::Positions::Invalid;

        BWAPI::Position fallbackGatherPosition =
            getTankGather20PercentFallbackPosition(gatherPosition);

        const int tanksNearOriginalGather =
            gatherPosition.isValid()
            ? countFriendlyAttackTanksNearPosition(
                allAttackTanks,
                gatherPosition,
                RELEASE_GATHER_RADIUS
            )
            : 0;

        const int tanksNearFallbackGather =
            fallbackGatherPosition.isValid()
            ? countFriendlyAttackTanksNearPosition(
                allAttackTanks,
                fallbackGatherPosition,
                RELEASE_GATHER_RADIUS
            )
            : 0;

        int tanksAtAssignedSlots = 0;
        for (BWAPI::Unit tank : allAttackTanks)
        {
            const TankDecisionState& state = stateFor(tank);
            if (gatherPosition.isValid() && state.gatherSlotAnchor.isValid() &&
                state.gatherSlotAnchor.getApproxDistance(gatherPosition) <= 2 * 32 &&
                state.gatherSlotPosition.isValid() &&
                tank->getDistance(state.gatherSlotPosition) <= 64)
            {
                ++tanksAtAssignedSlots;
            }
        }
        const int tanksNearGather = std::max(tanksAtAssignedSlots,
            std::max(tanksNearOriginalGather, tanksNearFallbackGather));

        const int completedGoliathsForRelease =
            countCompletedFriendlyGoliaths();

        const int goliathsNearGatherForRelease =
            gatherPosition.isValid()
            ? countFriendlyGoliathsNearPosition(
                gatherPosition,
                RELEASE_GATHER_RADIUS
            )
            : 0;

        const bool tankCountReady =
            ATTACK_TANKS_TO_RELEASE <= 0 ||
            attackTankCount >= ATTACK_TANKS_TO_RELEASE;

        const bool tankPositionReady =
            ATTACK_TANKS_TO_RELEASE <= 0 ||
            tanksNearGather >= TANKS_NEAR_GATHER_TO_RELEASE;

        const bool goliathCountReady =
            GOLIATHS_TO_RELEASE <= 0 ||
            completedGoliathsForRelease >= GOLIATHS_TO_RELEASE;

        const bool goliathPositionReady =
            GOLIATHS_TO_RELEASE <= 0 ||
            goliathsNearGatherForRelease >= GOLIATHS_NEAR_GATHER_TO_RELEASE;

        // A mature army can span several bases and path clusters. Requiring
        // nearly all of its Tanks and Goliaths at one exact gathering point
        // then strands a large rear group while the rest of the army fights.
        const bool overwhelmingMobileForce =
            ATTACK_TANKS_TO_RELEASE > 0 &&
            attackTankCount >= std::max(8, 2 * ATTACK_TANKS_TO_RELEASE) &&
            (GOLIATHS_TO_RELEASE <= 0 ||
             completedGoliathsForRelease >=
                 2 * GOLIATHS_TO_RELEASE);

        if (!tankMassAttackReleased &&
            tankCountReady &&
            goliathCountReady &&
            ((tankPositionReady && goliathPositionReady) ||
             overwhelmingMobileForce))
        {
            tankMassAttackReleased = true;
            tankMassAttackReleaseFrame = now;
        }

        const bool shouldMassGather =
            !tankMassAttackReleased &&
            gatherPosition.isValid();

        if (shouldMassGather)
        {

            if (Config::Debug::DrawUnitTargets)
            {
                BWAPI::Broodwar->drawCircleMap(
                    gatherPosition,
                    RELEASE_GATHER_RADIUS,
                    BWAPI::Colors::Cyan,
                    false
                );
            }

            std::vector<BWAPI::Unit> orderedGatherTanks;
            for (BWAPI::Unit attackTank : allAttackTanks)
            {
                if (attackTank && attackTank->exists())
                {
                    orderedGatherTanks.push_back(attackTank);
                }
            }
            std::sort(
                orderedGatherTanks.begin(),
                orderedGatherTanks.end(),
                [](BWAPI::Unit a, BWAPI::Unit b)
                {
                    return a->getID() < b->getID();
                });

            const BWAPI::Position gatherEnemyReference =
                getTankMassGatherEnemyReference(enemyGoal);
            BWAPI::Position gatherForward =
                gatherEnemyReference.isValid()
                ? gatherEnemyReference - gatherPosition
                : getRealFrontForwardVectorForTanks(gatherPosition);

            // MASS GATHER and CHOKE DEFENSE are intentionally different states.
            // Do not reuse defensive choke slots here: doing so silently pulled
            // Tanks back onto Cxx/DEF positions even after the gather anchor had
            // correctly been moved into open terrain on our side of the choke.

            // Build a stable ID-ordered formation. Old tanks keep their slots and new
            // tanks extend the formation backward; no greedy reassignment or dynamic
            // building revision is allowed to churn MASS GATHER.
            unsigned long long gatherTankSignature = 1469598103934665603ULL;
            for (BWAPI::Unit gatherTank : orderedGatherTanks)
            {
                gatherTankSignature ^= static_cast<unsigned long long>(gatherTank->getID() + 1);
                gatherTankSignature *= 1099511628211ULL;
            }

            const unsigned int gatherPartitionRevision = the.partitions.getRevision();
            const bool gatherGeometryStable =
                tankGatherPlanCache.anchor.isValid() &&
                tankGatherPlanCache.partitionRevision == gatherPartitionRevision &&
                tankGatherPlanCache.anchor.getApproxDistance(gatherPosition) <= 16 &&
                std::abs(tankGatherPlanCache.forward.x - gatherForward.x) <= 32 &&
                std::abs(tankGatherPlanCache.forward.y - gatherForward.y) <= 32;
            const bool gatherPlanCacheValid =
                gatherGeometryStable &&
                tankGatherPlanCache.tankCount == int(orderedGatherTanks.size()) &&
                tankGatherPlanCache.tankSignature == gatherTankSignature &&
                tankGatherPlanCache.slotIndexByTankID.size() == orderedGatherTanks.size();

            std::vector<BWAPI::Position> gatherSlots;
            std::map<int, int> gatherSlotByTankID;

            if (gatherPlanCacheValid)
            {
                gatherSlots = tankGatherPlanCache.slots;
                gatherSlotByTankID = tankGatherPlanCache.slotIndexByTankID;
            }
            else
            {
                TankPlanWork& work = tankPlanWork();
                ScopedTankPlanWork planTime(work.gatherMs, work.gatherBuilds);
                // Keep the old slots when the army changes by one Tank. Vacated
                // indices can be reused without searching the map again.
                std::vector<int> freeSlots;
                if (gatherGeometryStable)
                {
                    gatherSlots = tankGatherPlanCache.slots;
                    std::set<int> occupiedIndices;
                    for (BWAPI::Unit gatherTank : orderedGatherTanks)
                    {
                        const auto old = tankGatherPlanCache.slotIndexByTankID.find(
                            gatherTank->getID());
                        if (old != tankGatherPlanCache.slotIndexByTankID.end() &&
                            old->second >= 0 && old->second < int(gatherSlots.size()))
                        {
                            gatherSlotByTankID[gatherTank->getID()] = old->second;
                            occupiedIndices.insert(old->second);
                        }
                    }
                    for (int i = int(gatherSlots.size()) - 1; i >= 0; --i)
                    {
                        if (occupiedIndices.count(i) == 0) freeSlots.push_back(i);
                    }
                }
                gatherSlots.reserve(orderedGatherTanks.size());
                for (BWAPI::Unit gatherTank : orderedGatherTanks)
                {
                    if (gatherSlotByTankID.count(gatherTank->getID()) != 0) continue;
                    if (!freeSlots.empty())
                    {
                        gatherSlotByTankID[gatherTank->getID()] = freeSlots.back();
                        freeSlots.pop_back();
                    }
                    else
                    {
                        const int slotIndex = int(gatherSlots.size());
                        const BWAPI::Position slot = getMobileTankFormationSlotPosition(
                            slotIndex, gatherPosition, gatherForward, &gatherSlots);
                        gatherSlots.push_back(slot);
                        gatherSlotByTankID[gatherTank->getID()] = slotIndex;
                    }
                }

                tankGatherPlanCache.anchor = gatherPosition;
                tankGatherPlanCache.forward = gatherForward;
                tankGatherPlanCache.partitionRevision = gatherPartitionRevision;
                tankGatherPlanCache.frame = now;
                tankGatherPlanCache.tankCount = int(orderedGatherTanks.size());
                tankGatherPlanCache.tankSignature = gatherTankSignature;
                tankGatherPlanCache.slotIndexByTankID = gatherSlotByTankID;
                tankGatherPlanCache.slots = gatherSlots;
            }

            for (BWAPI::Unit gatherTank : orderedGatherTanks)
            {
                const auto assignment = gatherSlotByTankID.find(gatherTank->getID());
                if (assignment == gatherSlotByTankID.end() ||
                    assignment->second < 0 ||
                    assignment->second >= int(gatherSlots.size()))
                {
                    continue;
                }

                TankDecisionState& gatherState = stateFor(gatherTank);
                gatherState.gatherSlotIndex = assignment->second;
                gatherState.gatherSlotPosition = gatherSlots[assignment->second];
                gatherState.gatherSlotAnchor = gatherPosition;
                gatherState.gatherSlotForward = gatherForward;
            }

            // This observation is shared by the whole gather formation. Doing
            // an enemy-unit scan once per Tank multiplied the cost for no gain.
            const bool gatherUnderVisibleAttack =
                countEnemyCombatNearPosition(gatherPosition, 8 * 32) >= 3;
            const bool gatherSiegeTechReady =
                BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode);

            for (BWAPI::Unit tank : tanks)
            {
                if (!tank || !tank->exists())
                {
                    continue;
                }

                TankDecisionState& st = stateFor(tank);

                if (tank->getOrder() == BWAPI::Orders::Sieging ||
                    tank->getOrder() == BWAPI::Orders::Unsieging)
                {
                    continue;
                }

                BWAPI::Unit localTarget = nullptr;

                if (!tankTargets.empty())
                {
                    localTarget = getTarget(tank, tankTargets);
                }

                // A mass gather must have ONE shared anchor.  The previous
                // per-tank "safer gather" memory could leave the first one or
                // two tanks parked near the base while the rest assembled in
                // the middle of the map.  Only split from the common anchor
                // when it is under an actual visible attack right now.
                BWAPI::Position effectiveGatherPosition = gatherPosition;
                if (gatherUnderVisibleAttack)
                {
                    BWAPI::Position safer =
                        getSaferTankGatherPositionIfNeeded(tank, gatherPosition);
                    if (safer.isValid())
                    {
                        effectiveGatherPosition = safer;
                    }
                }

                const auto gatherSlot = gatherSlotByTankID.find(tank->getID());
                const int slotIndex =
                    gatherSlot == gatherSlotByTankID.end() ? -1 : gatherSlot->second;
                if (effectiveGatherPosition == gatherPosition &&
                    slotIndex >= 0 && slotIndex < int(gatherSlots.size()) &&
                    gatherSlots[slotIndex].isValid())
                {
                    // Use the assigned, validated choke slot. Recomputing a
                    // generic mobile slot here discarded the safe firing arc.
                    effectiveGatherPosition = gatherSlots[slotIndex];
                }
                else
                {
                    effectiveGatherPosition = getMobileTankFormationSlotPosition(
                        std::max(0, slotIndex), effectiveGatherPosition, gatherForward);
                }

                if (!effectiveGatherPosition.isValid())
                {
                    effectiveGatherPosition = gatherPosition;
                }

                // Keep a rare recovery slot once one has been selected for a
                // Tank that could not physically deploy at its normal slot. A
                // persistent alternate avoids repeating the local search every
                // frame and is discarded as soon as the gather anchor moves.
                const bool sameRecoveryAnchor =
                    st.gatherRecoveryAnchor.isValid() &&
                    st.gatherRecoveryAnchor.getApproxDistance(gatherPosition) <= 32;
                if (!sameRecoveryAnchor)
                {
                    st.gatherRecoveryAnchor = BWAPI::Positions::Invalid;
                    st.gatherRecoveryPosition = BWAPI::Positions::Invalid;
                    st.gatherSiegeBlockedSince = -100000;
                }
                else if (!gatherUnderVisibleAttack &&
                    st.gatherRecoveryPosition.isValid() &&
                    the.partitions.connected(
                        tank->getPosition(), st.gatherRecoveryPosition) &&
                    tankParkingTerrainIsClear(st.gatherRecoveryPosition) &&
                    !tankParkingOccupiedByAnotherTank(
                        st.gatherRecoveryPosition, tank))
                {
                    effectiveGatherPosition = st.gatherRecoveryPosition;
                }

                // If this individual tank is on another static component, find
                // a local replacement. Do not run BWAPI hasPath() from this hot
                // per-Tank loop; dynamic blockers are handled by normal unit
                // pathing and static connectivity is already known here.
                if (!the.partitions.connected(
                        tank->getPosition(), effectiveGatherPosition))
                {
                    BWAPI::Position reachable = findReachableTankGatherPosition(
                        tank, gatherPosition, gatherPosition);
                    if (reachable.isValid())
                    {
                        effectiveGatherPosition = reachable;
                    }
                    else
                    {
                        // Different static components mean there is no valid
                        // ground route to this rally. Do not construct a full
                        // GridDistances field trying to prove the impossible;
                        // leave this tank where it is until topology/base state
                        // changes and the gather anchor is rebuilt.
                        effectiveGatherPosition = tank->getPosition();
                    }
                }

                // Static connectivity cannot see another parked Tank, a newly
                // completed building, or a pathing deadlock. Watch real
                // progress and relocate only this Tank when its slot is not
                // becoming reachable. The normal formation stays stable.
                const bool gatherSlotOccupied =
                    tankParkingOccupiedByAnotherTank(
                        effectiveGatherPosition, tank);
                const bool gatherMoveStalled =
                    tankParkingMovementStalled(
                        tank,
                        effectiveGatherPosition,
                        now,
                        st.parkingProgressTarget,
                        st.parkingBestDistance,
                        st.parkingLastProgressFrame);

                if (!gatherUnderVisibleAttack && !localTarget &&
                    (gatherSlotOccupied || gatherMoveStalled) &&
                    (now < st.lastGatherRecoveryFrame ||
                     now - st.lastGatherRecoveryFrame >= 48))
                {
                    double fx = double(gatherForward.x);
                    double fy = double(gatherForward.y);
                    double flen = std::hypot(fx, fy);
                    if (flen < 1.0)
                    {
                        fx = 0.0;
                        fy = -1.0;
                        flen = 1.0;
                    }
                    fx /= flen;
                    fy /= flen;
                    const double sx = -fy;
                    const double sy = fx;
                    const int sideSign = (tank->getID() & 1) ? 1 : -1;
                    const BWAPI::Position recoveryDesired = clampToMapForTanks(
                        BWAPI::Position(
                            int(effectiveGatherPosition.x +
                                sx * sideSign * 80.0 - fx * 48.0),
                            int(effectiveGatherPosition.y +
                                sy * sideSign * 80.0 - fy * 48.0)));

                    const BWAPI::Position alternate =
                        findReachableTankGatherPosition(
                            tank, recoveryDesired, gatherPosition);
                    st.lastGatherRecoveryFrame = now;

                    if (alternate.isValid() &&
                        alternate.getApproxDistance(tank->getPosition()) > 24 &&
                        !positionBlocksDefensePassage(
                            alternate, gatherPosition, gatherForward))
                    {
                        st.gatherRecoveryAnchor = gatherPosition;
                        st.gatherRecoveryPosition = alternate;
                        effectiveGatherPosition = alternate;
                        st.parkingProgressTarget = alternate;
                        st.parkingBestDistance = tank->getPosition().getApproxDistance(alternate);
                        st.parkingLastProgressFrame = now;
                    }
                }

                int distToGather = tank->getDistance(effectiveGatherPosition);

                const bool assignedGatherSlot =
                    slotIndex >= 0 && slotIndex < int(gatherSlots.size()) &&
                    effectiveGatherPosition == gatherSlots[slotIndex];
                const bool validatedRecoverySlot =
                    sameRecoveryAnchor &&
                    st.gatherRecoveryPosition.isValid() &&
                    effectiveGatherPosition == st.gatherRecoveryPosition;
                const bool atValidatedGatherSlot = distToGather <= 32 &&
                    (assignedGatherSlot || validatedRecoverySlot) &&
                    tankParkingTerrainIsClear(effectiveGatherPosition) &&
                    !tankParkingOccupiedByAnotherTank(
                        effectiveGatherPosition, tank) &&
                    !positionBlocksDefensePassage(
                        tank->getPosition(), gatherPosition, gatherForward);
                drawAssignedTankSlot(tank, effectiveGatherPosition, 'G', BWAPI::Colors::Cyan);

                const int distToTarget =
                    localTarget ? tank->getDistance(localTarget) : MAX_DISTANCE;

                const bool targetInSiegeRange =
                    localTarget &&
                    distToTarget >= EMERGENCY_UNSIEGE_DISTANCE &&
                    distToTarget <= siegeTankRange;

                const bool targetTooClose =
                    localTarget &&
                    distToTarget < EMERGENCY_UNSIEGE_DISTANCE &&
                    !localTarget->getType().isBuilding();

                if (handleTankFightWhileMovingToGather(
                    tank,
                    tankTargets,
                    now,
                    siegeTankRange,
                    EMERGENCY_UNSIEGE_DISTANCE,
                    TANK_HOLD_FRAMES,
                    POST_UNSIEGE_COOLDOWN,
                    st.lastUnsiegeFrame,
                    st.holdTankUntil,
                    st.lastSiegeFrame))
                {
                    continue;
                }

                if (tank->isSieged())
                {
                    if (targetTooClose || tank->isUnderDisruptionWeb())
                    {
                        if (the.micro.Unsiege(tank, true))
                        {
                            st.lastUnsiegeFrame = now;
                            st.holdTankUntil = now + TANK_HOLD_FRAMES;
                        }
                        continue;
                    }

                    if (targetInSiegeRange)
                    {
                        the.micro.AttackUnit(tank, localTarget);
                        continue;
                    }

                    if (distToGather > 64)
                    {
                        if (the.micro.Unsiege(tank))
                        {
                            st.lastUnsiegeFrame = now;
                            st.holdTankUntil = now + TANK_HOLD_FRAMES;
                        }
                        continue;
                    }

                    holdGatheringTank(tank);
                    continue;
                }

                if (targetTooClose && localTarget)
                {
                    the.micro.KiteTarget(tank, localTarget);
                    continue;
                }

                const bool enemyNearGather =
                    localTarget &&
                    localTarget->getDistance(effectiveGatherPosition) <= 10 * 32;

                if (targetInSiegeRange &&
                    enemyNearGather &&
                    distToGather <= 10 * 32 &&
                    tank->canSiege() &&
                    (!unitNearChokepoint(tank) || atValidatedGatherSlot) &&
                    now >= st.holdTankUntil &&
                    now - st.lastUnsiegeFrame >= POST_UNSIEGE_COOLDOWN)
                {
                    DrawUnitTargetTextMap(
                        tank->getPosition() + BWAPI::Position(0, 8),
                        "mass defend siege"
                    );

                    if (the.micro.Siege(tank))
                    {
                        st.lastSiegeFrame = now;
                    }
                    continue;
                }

                if (distToGather > 32)
                {

                    DrawUnitTargetTextMap(
                        tank->getPosition() + BWAPI::Position(0, 8),
                        "mass gather"
                    );

                    if (handleTankEmergencyBeforeForcedMove(
                        tank,
                        tankTargets,
                        now,
                        siegeTankRange,
                        EMERGENCY_UNSIEGE_DISTANCE,
                        TANK_HOLD_FRAMES,
                        POST_UNSIEGE_COOLDOWN,
                        st.lastUnsiegeFrame,
                        st.holdTankUntil,
                        st.lastSiegeFrame))
                    {
                        continue;
                    }

                    the.micro.Move(tank, effectiveGatherPosition);
                    continue;
                }

                // Orient a parked Tank toward the enemy exactly once for this
                // gather anchor, even when Siege research is already finished.
                // BWAPI has no explicit turn command; a tiny 16px forward move
                // fixes the chassis facing, then the next frame may Siege. This
                // one-shot replaces the old periodic nudge that caused churn.
                if (!gatherSiegeTechReady && !localTarget &&
                    !gatherUnderVisibleAttack &&
                    (!st.gatherOrientationAnchor.isValid() ||
                     st.gatherOrientationAnchor.getApproxDistance(gatherPosition) > 32))
                {
                    const BWAPI::Position faceTarget = pointTowardTerrainForTanks(
                        effectiveGatherPosition,
                        effectiveGatherPosition + gatherForward,
                        16);
                    st.gatherOrientationAnchor = gatherPosition;
                    if (faceTarget.isValid())
                    {
                        the.micro.Move(tank, faceTarget);
                        continue;
                    }
                }

                // Siege should be the terminal state of MASS GATHER once
                // the upgrade exists. If this exact footprint rejects Siege for
                // several frames, move this Tank to ONE alternate nearby slot.
                // The recovery uses only MapPartitions + local terrain checks;
                // it never calls BWAPI hasPath() or constructs a distance grid.
                const bool readyToDeployAtGather =
                    gatherSiegeTechReady &&
                    !localTarget &&
                    !tank->isUnderDisruptionWeb() &&
                    now >= st.holdTankUntil &&
                    now - st.lastUnsiegeFrame >= POST_UNSIEGE_COOLDOWN;

                const bool gatherParkingBlocked =
                    !tankParkingTerrainIsClear(tank->getPosition()) ||
                    (unitNearChokepoint(tank) && !atValidatedGatherSlot) ||
                    !tank->canSiege();
                if (readyToDeployAtGather && gatherParkingBlocked)
                {
                    if (st.gatherSiegeBlockedSince < 0 ||
                        now < st.gatherSiegeBlockedSince)
                    {
                        st.gatherSiegeBlockedSince = now;
                        st.gatherRecoveryAnchor = gatherPosition;
                    }

                    if (now - st.gatherSiegeBlockedSince >= 12 &&
                        (now < st.lastGatherRecoveryFrame ||
                         now - st.lastGatherRecoveryFrame >= 48))
                    {
                        double fx = double(gatherForward.x);
                        double fy = double(gatherForward.y);
                        double flen = std::hypot(fx, fy);
                        if (flen < 1.0)
                        {
                            fx = 0.0;
                            fy = -1.0;
                            flen = 1.0;
                        }
                        fx /= flen;
                        fy /= flen;
                        const double sx = -fy;
                        const double sy = fx;
                        const int sideSign = (tank->getID() & 1) ? 1 : -1;

                        BWAPI::Position recoveryDesired(
                            int(effectiveGatherPosition.x + sx * sideSign * 72.0 - fx * 48.0),
                            int(effectiveGatherPosition.y + sy * sideSign * 72.0 - fy * 48.0));
                        recoveryDesired = clampToMapForTanks(recoveryDesired);

                        BWAPI::Position alternate = findReachableTankGatherPosition(
                            tank, recoveryDesired, gatherPosition);
                        st.lastGatherRecoveryFrame = now;
                        st.gatherSiegeBlockedSince = now;

                        if (alternate.isValid() &&
                            alternate.getApproxDistance(tank->getPosition()) > 24 &&
                            !positionBlocksDefensePassage(
                                alternate, gatherPosition, gatherForward))
                        {
                            st.gatherRecoveryAnchor = gatherPosition;
                            st.gatherRecoveryPosition = alternate;
                            DrawUnitTargetTextMap(
                                tank->getPosition() + BWAPI::Position(0, 8),
                                "mass siege relocate");
                            the.micro.Move(tank, alternate);
                            continue;
                        }
                    }

                    // Do not convert a temporarily failed Siege command into an
                    // attack-move toward the enemy. Hold here and retry.
                    holdGatheringTank(tank);
                    continue;
                }

                if (!gatherParkingBlocked)
                {
                    st.gatherSiegeBlockedSince = -100000;
                    st.gatherRecoveryAnchor = BWAPI::Positions::Invalid;
                    st.gatherRecoveryPosition = BWAPI::Positions::Invalid;
                }

                if (!gatherParkingBlocked && tank->canSiege() &&
                    (!unitNearChokepoint(tank) || atValidatedGatherSlot) &&
                    now >= st.holdTankUntil &&
                    now - st.lastUnsiegeFrame >= POST_UNSIEGE_COOLDOWN)
                {
                    DrawUnitTargetTextMap(
                        tank->getPosition() + BWAPI::Position(0, 8),
                        "mass wait siege"
                    );

                    if (the.micro.Siege(tank))
                    {
                        st.lastSiegeFrame = now;
                    }
                }
                else
                {
                    holdGatheringTank(tank);
                }
            }

            return;
        }
    }

    if (!defendingAttackedBase &&
        !attackedBasePosition.isValid() &&
        StrategyManager::Instance().defaultTankGatherEnabled() &&
        !CombatCommander::Instance().getAggression())
    {
        // A released Tank mass still needs a real defensive front when the
        // strategy deliberately recalls it. Otherwise the generic enemy-goal
        // fallback can send it toward the map center. Selected responders to
        // an attacked base have already been handled above; do not recall the
        // remaining army for their local fight.
        BWAPI::Unitset groundTargets;

        for (BWAPI::Unit enemy : targets)
        {
            if (!enemy || !enemy->exists())
            {
                continue;
            }

            if (enemy->isFlying())
            {
                continue;
            }

            groundTargets.insert(enemy);
        }

        doDefensiveFrontForAttackTanks(tanks, groundTargets);
        return;
    }

    BWAPI::Unit forwardTank =
        getForwardMostTankTowardEnemyBase(
            allAttackTanks,
            enemyGoal
        );

    const BWAPI::Position tankFrontCenter =
        getTankFrontCenterTowardGoal(allAttackTanks, enemyGoal);

    tryComsatScoutStalledTankFront(
        allAttackTanks,
        orderedEnemyBase && tankMassAttackReleased && !pureAirEscortActive &&
            !defendingAttackedBase && CombatCommander::Instance().getAggression()
            ? (tankFrontCenter.isValid() ? tankFrontCenter
                 : forwardTank ? forwardTank->getPosition()
                   : BWAPI::Positions::Invalid)
            : BWAPI::Positions::Invalid,
        enemyGoal,
        !tankTargets.empty());

    tankCrashCheckpoint("MicroTanks::executeMicro - shared enemy memory");

    const BWAPI::Position rememberedTankCenter =
        rememberedEnemyTankCenterNear(tankFrontCenter, 18 * 32);
    const bool genericEnemyMemoryActive =
        tankGroupEnemyMemoryActive(allAttackTanks);
    const bool groupEnemyMemoryActive = genericEnemyMemoryActive;
    const BWAPI::Position externalVisionCenter =
        rememberedTankCenter.isValid()
        ? rememberedTankCenter
        : genericEnemyMemoryActive
        ? lastTankGroupEnemyCenter
        : BWAPI::Positions::Invalid;

    // Keep Tank vision independent of Vulture combat micro. Vultures form
    // ahead of the army and engage ground targets in their own module.
    const bool memoryAreaAlreadyVisible =
        externalVisionCenter.isValid() &&
        BWAPI::Broodwar->isVisible(BWAPI::TilePosition(externalVisionCenter));

    const bool needsExternalVision =
        externalVisionCenter.isValid() &&
        !memoryAreaAlreadyVisible &&
        allAttackTanks.size() >= 4;

    // Comsat is independent of whether the selected posture uses Vultures.
    // In particular, inherited Tanks in PureAir still need safe vision and
    // must not drive forward just because their Vulture screen is disabled.
    if (needsExternalVision &&
        (posture != TerranArmyPosture::PureAir || bioTankArmy))
    {
        tryComsatScoutTankEnemyMemory(
            allAttackTanks,
            externalVisionCenter);
    }

    tankCrashCheckpoint("MicroTanks::executeMicro - siege wall evaluation");

    BWAPI::Position siegeWallReference =
        tankFrontCenter.isValid()
        ? tankFrontCenter
        : forwardTank && forwardTank->exists()
        ? forwardTank->getPosition()
        : enemyGoal;

    if (!siegeWallReference.isValid())
    {
        siegeWallReference = order->getPosition();
    }

    BWAPI::Position siegeWallDangerCenter = BWAPI::Positions::Invalid;

    if (siegeWallReference.isValid())
    {
        siegeWallDangerCenter =
            getVisibleEnemyTankClusterCenterNear(siegeWallReference, 16 * 32);
    }

    if (!siegeWallDangerCenter.isValid() && enemyGoal.isValid())
    {
        siegeWallDangerCenter =
            getVisibleEnemyTankClusterCenterNear(enemyGoal, 16 * 32);
    }

    if (!siegeWallDangerCenter.isValid())
    {
        siegeWallDangerCenter = rememberedEnemyTankCenterNear(
            siegeWallReference, 18 * 32);
    }

    const BWAPI::Position siegeWallCountCenter =
        siegeWallDangerCenter.isValid()
        ? siegeWallDangerCenter
        : siegeWallReference;

    const int enemyVisibleTanksNearWall =
        countVisibleEnemyTanksNear(siegeWallCountCenter, 10 * 32);

    const int enemyVisibleSiegedTanksNearWall =
        countVisibleEnemySiegedTanksNear(siegeWallCountCenter, 10 * 32);

    const int enemyRememberedTanksNearWall =
        rememberedEnemyTankCountNear(siegeWallCountCenter, 10 * 32);
    const int enemyRememberedSiegedTanksNearWall =
        rememberedEnemySiegedTankCountNear(siegeWallCountCenter, 10 * 32);

    const int enemyKnownTanksNearWall =
        std::max(enemyVisibleTanksNearWall, enemyRememberedTanksNearWall);
    const int enemyKnownSiegedTanksNearWall =
        std::max(enemyVisibleSiegedTanksNearWall, enemyRememberedSiegedTanksNearWall);

    const int friendlyTanksNearWall =
        countFriendlyTanksNear(allAttackTanks, siegeWallCountCenter, siegeTankRange + 4 * 32);

    const bool knownEnemySiegeWall =
        !defendingAttackedBase &&
        siegeWallDangerCenter.isValid() &&
        enemyKnownSiegedTanksNearWall >= 3 &&
        enemyKnownTanksNearWall >= 4;

    // Retreat only when the wall is materially stronger. An equal pre-sieged
    // line is still dangerous, but the correct answer is to hold our own Siege
    // position and acquire vision, not drive forward or flee.
    const bool strongEnemySiegeWall =
        knownEnemySiegeWall &&
        friendlyTanksNearWall + 2 < enemyKnownTanksNearWall;

    if (strongEnemySiegeWall)
    {
        // A point relative to the current group receded every frame, making
        // the rear half drive away indefinitely. This point is on our side of
        // the fixed enemy line; reinforcements approach it from behind.
        BWAPI::Position regroupPosition = positionTowardForTankMemory(
            siegeWallDangerCenter, siegeWallReference, siegeTankRange + 2 * 32);
        BWAPI::Position walkableRegroup =
            findNearbyWalkableTankGatherPosition(regroupPosition);
        if (walkableRegroup.isValid() &&
            the.partitions.connected(siegeWallReference, walkableRegroup))
        {
            regroupPosition = walkableRegroup;
        }
        else
        {
            regroupPosition = siegeWallReference;
        }

        for (BWAPI::Unit tank : tanks)
        {
            if (!tank || !tank->exists())
            {
                continue;
            }

            TankDecisionState& st = stateFor(tank);

            if (tank->getOrder() == BWAPI::Orders::Sieging ||
                tank->getOrder() == BWAPI::Orders::Unsieging)
            {
                continue;
            }

            BWAPI::Unit localTarget = getTarget(tank, tankTargets);
            const bool targetInSiegeRange = targetInTankSiegeRange(tank, localTarget);

            if (tank->isSieged())
            {
                if (targetInSiegeRange)
                {
                    the.micro.AttackUnit(tank, localTarget);
                }
                else if (!the.micro.siegeHoldActive(tank) &&
                    !tankShouldSiegeOnEnemyMemory(tank, allAttackTanks) &&
                    tank->getDistance(regroupPosition) > 4 * 32)
                {
                    if (the.micro.Unsiege(tank))
                    {
                        st.lastUnsiegeFrame = now;
                        st.holdTankUntil = now + TANK_HOLD_FRAMES;
                    }
                }
                else
                {
                    the.micro.HoldPosition(tank);
                }
                continue;
            }

            if (tank->getDistance(regroupPosition) > 96)
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    "regroup vs siege wall"
                );

                if (handleTankEmergencyBeforeForcedMove(
                    tank,
                    tankTargets,
                    now,
                    siegeTankRange,
                    EMERGENCY_UNSIEGE_DISTANCE,
                    TANK_HOLD_FRAMES,
                    POST_UNSIEGE_COOLDOWN,
                    st.lastUnsiegeFrame,
                    st.holdTankUntil,
                    st.lastSiegeFrame))
                {
                    continue;
                }

                the.micro.Move(tank, getTerrainTankRouteWaypoint(tank->getPosition(), regroupPosition));
                continue;
            }

            if (targetInSiegeRange && tank->canSiege())
            {
                if (the.micro.Siege(tank))
                {
                    st.lastSiegeFrame = now;
                }
                continue;
            }

            if (tank->getDistance(siegeWallDangerCenter) <= siegeTankRange + 2 * 32 &&
                tank->canSiege())
            {
                if (the.micro.Siege(tank))
                {
                    st.lastSiegeFrame = now;
                }
            }
            else
            {
                the.micro.HoldPosition(tank);
            }
        }

        return;
    }

    if (groupEnemyMemoryActive)
    {
        if (lastTankGroupEnemyCenter.isValid())
        {
            if (Config::Debug::DrawUnitTargets)
            {
                BWAPI::Broodwar->drawCircleMap(
                    lastTankGroupEnemyCenter,
                    48,
                    BWAPI::Colors::Red,
                    false
                );
            }
        }
    }

    // One real ground threat is enough to deploy. The old >1 rule meant that a
    // single Dragoon/Hydralisk/Goliath/Marine could make some Tanks drive into
    // direct-fire range while the rest of the line correctly sieged.
    const bool threatsExist =
        anySiegeUnits(tankTargets) || nThreats(tankTargets) > 0;

    std::vector<BWAPI::Unit> escortTanks;
    BWAPI::Position escortForward = BWAPI::Positions::Invalid;
    if (pureAirEscortActive)
    {
        escortTanks.assign(allAttackTanks.begin(), allAttackTanks.end());
        std::sort(escortTanks.begin(), escortTanks.end(),
            [](BWAPI::Unit a, BWAPI::Unit b) { return a->getID() < b->getID(); });
        escortForward = getRealFrontForwardVectorForTanks(pureAirEscortGoal);
    }

    tankCrashCheckpoint("MicroTanks::executeMicro - per-tank combat decisions");

    for (BWAPI::Unit tank : tanks)
    {
        if (!tank || !tank->exists())
        {
            continue;
        }

        TankDecisionState& st = stateFor(tank);

        const bool seesEnemyNow = st.lastSawEnemyFrame == now;

        if (tank->getOrder() == BWAPI::Orders::Sieging ||
            tank->getOrder() == BWAPI::Orders::Unsieging)
        {
            continue;
        }

        BWAPI::Unit target = getTarget(tank, tankTargets);

        // In PureAir a remembered/visible building must not turn a support
        // Tank into an independent base attacker. It may fire or deploy from
        // the current Goliath-supported line, but a farther structure is left
        // to the common army objective until the Goliaths bring that line
        // forward. Mobile ground threats retain all normal Tank rules.
        if (posture == TerranArmyPosture::PureAir &&
            pureAirEscortActive && target &&
            target->getType().isBuilding())
        {
            const int supportedFireRange =
                tank->isSieged() || tank->canSiege()
                ? siegeTankRange
                : BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode
                    .groundWeapon().maxRange();
            if (tank->getDistance(target) > supportedFireRange)
            {
                target = nullptr;
            }
        }
        const bool shouldHoldForGroupMemory =
            groupEnemyMemoryActive &&
            tankShouldSiegeOnEnemyMemory(tank, allAttackTanks) &&
            !isActiveTankScout(tank);

        // Static-only cleanup advances by artillery range. It does not treat
        // unseen terrain as proof that the whole enemy base is empty: recent
        // mobile contacts and known Siege Tanks still keep their normal holds.
        const bool staticCleanup = !defendingAttackedBase && !pureAirEscortActive &&
            tankMassAttackReleased && !hasVisibleMobileGroundTarget &&
            !groupEnemyMemoryActive && !rememberedEnemyTankThreatForTank(tank, 18 * 32) &&
            (target ? target->getType().isBuilding() : cachedNearbyBuildingPosition.isValid()) &&
            !hasVisibleEnemyGroundCombatNearPosition(tank->getPosition(), 16 * 32);

        const bool retreatNeeded =
            tankShouldRetreat(tank, tankTargets, defendingAttackedBase);
        const bool retreatInProgress = st.retreatPosition.isValid() &&
            now < st.retreatUntil && tank->getDistance(st.retreatPosition) > 48;

        if (retreatNeeded || retreatInProgress)
        {
            if (!retreatInProgress)
            {
                st.retreatPosition = getTankRetreatPosition(
                    tank, tankTargets, getHoldPosition());
                st.retreatUntil = now + 4 * 24;
            }
            DrawUnitTargetTextMap(tank->getPosition() + BWAPI::Position(0, 8),
                "finish retreat");

            if (tank->isSieged())
            {
                if (the.micro.Unsiege(tank, true))
                {
                    st.lastUnsiegeFrame = now;
                    st.holdTankUntil = now + TANK_HOLD_FRAMES;
                }
            }
            else if (target && tank->getDistance(target) < EMERGENCY_UNSIEGE_DISTANCE &&
                !target->getType().isBuilding())
            {
                the.micro.KiteTarget(tank, target);
            }
            else if (st.retreatPosition.isValid())
            {
                the.micro.Move(tank, getTerrainTankRouteWaypoint(
                    tank->getPosition(), st.retreatPosition));
            }
            st.holdTankUntil = now + TANK_HOLD_FRAMES;
            continue;
        }
        st.retreatPosition = BWAPI::Positions::Invalid;
        st.retreatUntil = -100000;

        // Do not let a Tank left behind by the PureAir transition keep its
        // previous siege/attack objective while the Goliaths are elsewhere.
        // Nearby ground attackers are still handled by the usual combat and
        // base-defense rules.
        if (posture == TerranArmyPosture::PureAir && pureAirEscortActive &&
            tank->getDistance(pureAirEscortGoal) > 8 * 32 &&
            countFriendlyGoliathsNearPosition(tank->getPosition(), 8 * 32) == 0 &&
            !hasVisibleEnemyGroundCombatNearPosition(tank->getPosition(), 6 * 32))
        {
            if (tank->isSieged())
            {
                if (the.micro.Unsiege(tank, true))
                {
                    st.lastUnsiegeFrame = now;
                    st.holdTankUntil = now + TANK_HOLD_FRAMES;
                }
            }
            else
            {
                the.micro.Move(tank, getTerrainTankRouteWaypoint(
                    tank->getPosition(), pureAirEscortGoal));
            }
            continue;
        }

        // One keep-siege decision precedes scouting, regrouping and advancing.
        // Those branches must not undo a useful shot or a local fog hold.
        if (tank->isSieged() && !tank->isUnderDisruptionWeb())
        {
            if (targetInTankSiegeRange(tank, target))
            {
                the.micro.AttackUnit(tank, target);
                continue;
            }
            const bool targetInsideMinimum = target &&
                !target->getType().isBuilding() &&
                tank->getDistance(target) < EMERGENCY_UNSIEGE_DISTANCE;
            if (!targetInsideMinimum &&
                (the.micro.siegeHoldActive(tank) || shouldHoldForGroupMemory))
            {
                DrawUnitTargetTextMap(tank->getPosition() + BWAPI::Position(0, 8),
                    "hold local enemy memory");
                the.micro.HoldPosition(tank);
                continue;
            }
        }

        const bool isForwardTank = forwardTank && tank == forwardTank;
        const bool isFrontBandTank =
            tankFrontCenter.isValid() &&
            tank->getDistance(tankFrontCenter) <= 4 * 32;

        // Tanks do not scout into fog. Recon/Vultures and Comsat provide vision.
        activeScoutTankID = -1;
        activeScoutStepUntil = -100000;
        activeScoutTarget = BWAPI::Positions::Invalid;

        if (tankMassAttackReleased && !pureAirEscortActive && !staticCleanup &&
            isForwardTank &&
            forwardTankShouldWaitForSupport(
                tank,
                allAttackTanks,
                tankTargets,
                defendingAttackedBase))
        {
            BWAPI::Position regroupPosition =
                getRegroupPositionNearClosestTank(
                    tank,
                    allAttackTanks,
                    enemyGoal
                );


            if (tank->isSieged())
            {
                BWAPI::Unit localTarget = nullptr;

                if (!tankTargets.empty())
                {
                    localTarget = getTarget(tank, tankTargets);
                }

                if (localTarget &&
                    tank->getDistance(localTarget) >= 64 &&
                    tank->getDistance(localTarget) <= siegeTankRange)
                {
                    the.micro.AttackUnit(tank, localTarget);
                }
                else
                {
                    the.micro.HoldPosition(tank);
                }
            }
            else
            {
                BWAPI::Unit localTarget = nullptr;

                if (!tankTargets.empty())
                {
                    localTarget = getTarget(tank, tankTargets);
                }

                const bool canSiegeToCover =
                    localTarget &&
                    tank->canSiege() &&
                    tank->getDistance(localTarget) >= 64 &&
                    tank->getDistance(localTarget) <= siegeTankRange &&
                    !tank->isUnderDisruptionWeb();

                if (canSiegeToCover)
                {
                    if (the.micro.Siege(tank))
                    {
                        st.lastSiegeFrame = now;
                    }
                }
                else if (regroupPosition.isValid() &&
                    tank->getDistance(regroupPosition) > 96)
                {
                    if (handleTankEmergencyBeforeForcedMove(
                        tank,
                        tankTargets,
                        now,
                        siegeTankRange,
                        EMERGENCY_UNSIEGE_DISTANCE,
                        TANK_HOLD_FRAMES,
                        POST_UNSIEGE_COOLDOWN,
                        st.lastUnsiegeFrame,
                        st.holdTankUntil,
                        st.lastSiegeFrame))
                    {
                        continue;
                    }

                    the.micro.Move(tank, regroupPosition);
                }
                else
                {
                    the.micro.HoldPosition(tank);
                }
            }

            continue;
        }

        if (!target)
        {
            if (tank->isSieged())
            {
                // The keep-siege decision above already checked this Tank's
                // own memory. A distant battle must not pin rear reinforcements.
                if (the.micro.Unsiege(tank, tank->isUnderDisruptionWeb()))
                {
                    st.lastUnsiegeFrame = now;
                    st.holdTankUntil = now + TANK_HOLD_FRAMES;
                }
                continue;
            }

            if (shouldHoldForGroupMemory)
            {
                if (tank->canSiege() && !unitNearChokepoint(tank) &&
                    now >= st.holdTankUntil &&
                    now - st.lastUnsiegeFrame >= POST_UNSIEGE_COOLDOWN)
                {
                    if (the.micro.Siege(tank))
                    {
                        st.lastSiegeFrame = now;
                    }
                }
                else
                {
                    the.micro.HoldPosition(tank);
                }
                continue;
            }

            if (pureAirEscortActive && pureAirEscortGoal.isValid())
            {
                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 8),
                    StrategyManager::Instance().isTerranLightMixed()
                        ? "LIGHTMIXED REAR"
                        : "PUREAIR REAR");
                const BWAPI::Position forward = escortForward;
                const bool changed = !st.gatherSlotAnchor.isValid() ||
                    st.gatherSlotAnchor.getApproxDistance(pureAirEscortGoal) > 64 ||
                    !st.gatherSlotPosition.isValid() ||
                    !tankParkingTerrainIsClear(st.gatherSlotPosition) ||
                    tankParkingOccupiedByAnotherTank(st.gatherSlotPosition, tank);
                const bool stalled = st.gatherSlotPosition.isValid() && tankParkingMovementStalled(
                    tank, st.gatherSlotPosition, now, st.parkingProgressTarget,
                    st.parkingBestDistance, st.parkingLastProgressFrame);
                if ((changed || stalled) && now - st.lastGatherRecoveryFrame >= 24)
                {
                    std::vector<BWAPI::Position> reserved;
                    for (BWAPI::Unit other : escortTanks)
                    {
                        const TankDecisionState& otherState = stateFor(other);
                        if (other != tank && otherState.gatherSlotPosition.isValid() &&
                            otherState.gatherSlotAnchor.isValid() &&
                            otherState.gatherSlotAnchor.getApproxDistance(pureAirEscortGoal) <= 64)
                            reserved.push_back(otherState.gatherSlotPosition);
                    }
                    const auto found = std::find(escortTanks.begin(), escortTanks.end(), tank);
                    const int index = int(std::distance(escortTanks.begin(), found));
                    BWAPI::Position slot = getMobileTankFormationSlotPosition(
                        index, pureAirEscortGoal, forward, &reserved);
                    if (slot.isValid() && (stalled || tankParkingOccupiedByAnotherTank(slot, tank)))
                        slot = findReachableTankGatherPosition(tank,
                            slot + BWAPI::Position((index & 1) ? 64 : -64, 64), pureAirEscortGoal, &reserved);
                    if (slot.isValid() && tankParkingTerrainIsClear(slot) &&
                        !tankParkingOccupiedByAnotherTank(slot, tank))
                    {
                        st.gatherSlotPosition = slot;
                        st.gatherSlotAnchor = pureAirEscortGoal;
                    }
                    else st.gatherSlotPosition = BWAPI::Positions::Invalid;
                    st.lastGatherRecoveryFrame = now;
                }
                const BWAPI::Position slot = st.gatherSlotPosition;
                if (slot.isValid() && tank->getPosition().getApproxDistance(slot) > 20)
                {
                    const BWAPI::Position routeGoal = tank->getDistance(pureAirEscortGoal) > 12 * 32
                        ? pureAirEscortGoal : slot;
                    the.micro.Move(tank, getTerrainTankRouteWaypoint(tank->getPosition(), routeGoal));
                }
                else the.micro.HoldPosition(tank);
                continue;
            }

            // enemyGoal is the one resolved objective for this army. The old
            // fallback used the squad's home order even after mass release.
            BWAPI::Position moveGoal = bioTankRearLine.isValid()
                ? bioTankRearLine : enemyGoal;
            const bool joiningFront = !bioTankRearLine.isValid() &&
                tankMassAttackReleased && !staticCleanup &&
                tankFrontCenter.isValid() && !isFrontBandTank &&
                tank->getDistance(tankFrontCenter) > 8 * 32;
            if (joiningFront)
            {
                moveGoal = getJoinPositionNearTankFront(tank, tankFrontCenter, enemyGoal);
            }
            if (moveGoal.isValid() && tank->getDistance(moveGoal) > 64)
            {
                DrawUnitTargetTextMap(tank->getPosition() + BWAPI::Position(0, 8),
                    joiningFront ? "join tank front" : "advance army goal");
                if (handleSoloTankAvoidEnemyTankField(tank, allAttackTanks, moveGoal))
                {
                    continue;
                }
                // Far reinforcements route to the SHARED front center. Routing
                // every Tank to its own decorative join slot creates one full
                // distance field per unit and can thrash the MapTools cache.
                // Once close, native pathing handles the final individual slot.
                const BWAPI::Position routeGoal =
                    joiningFront && tankFrontCenter.isValid() &&
                    tank->getDistance(tankFrontCenter) > 14 * 32
                    ? tankFrontCenter
                    : moveGoal;
                the.micro.Move(tank, getTerrainTankRouteWaypoint(
                    tank->getPosition(), routeGoal));
            }
            else
            {
                the.micro.HoldPosition(tank);
            }
            continue;
        }

        // BioTank advances with the Marine screen. A visible target beyond
        // Siege range must not send a rear Tank down the generic chase path;
        // the infantry moves the rear line forward and the Tank follows it.
        if (bioTankRearLine.isValid() && !tank->isSieged() &&
            tank->getDistance(target) > siegeTankRange &&
            !tank->isUnderAttack())
        {
            if (tank->getDistance(bioTankRearLine) > 48)
            {
                the.micro.Move(tank, getTerrainTankRouteWaypoint(
                    tank->getPosition(), bioTankRearLine));
            }
            else
            {
                the.micro.HoldPosition(tank);
            }
            continue;
        }

        const int distanceToTarget = tank->getDistance(target);
        const int siegeEnterMargin = (isEnemyTerranTank(target) || target->getType().isBuilding()) ? 0 : SIEGE_ENTER_MARGIN;

        const bool targetIsBuilding = target->getType().isBuilding();

        if (!tank->isSieged() && tankMassAttackReleased && !staticCleanup &&
            distanceToTarget > siegeTankRange + SIEGE_EXIT_MARGIN &&
            tankFrontCenter.isValid() && !isFrontBandTank &&
            tank->getDistance(tankFrontCenter) > 8 * 32)
        {
            const BWAPI::Position joinPosition =
                getJoinPositionNearTankFront(tank, tankFrontCenter, enemyGoal);
            if (joinPosition.isValid())
            {
                DrawUnitTargetTextMap(tank->getPosition() + BWAPI::Position(0, 8),
                    "reinforce tank front");
                if (!handleSoloTankAvoidEnemyTankField(tank, allAttackTanks, joinPosition))
                {
                    const BWAPI::Position routeGoal =
                        tankFrontCenter.isValid() &&
                        tank->getDistance(tankFrontCenter) > 14 * 32
                        ? tankFrontCenter
                        : joinPosition;
                    the.micro.Move(tank, getTerrainTankRouteWaypoint(
                        tank->getPosition(), routeGoal));
                }
                continue;
            }
        }

        const bool enemyTooClose =
            distanceToTarget < EMERGENCY_UNSIEGE_DISTANCE &&
            !target->getType().isBuilding();

        bool shouldSiege =
            threatsExist &&
            distanceToTarget <= siegeTankRange - siegeEnterMargin;

        if (shouldSiege)
        {
            if (target->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
            {
                shouldSiege = false;
            }

            // Melee units are a reason not to start the long transformation
            // only when they are already close. At artillery distance the Tank
            // should still deploy instead of driving toward them in Tank Mode.
            if (allMeleeAndSameHeight(tankTargets, tank) &&
                distanceToTarget < 5 * 32)
            {
                shouldSiege = false;
            }
        }

        if (isFrontBandTank &&
            !enemyTooClose &&
            distanceToTarget <= siegeTankRange - siegeEnterMargin)
        {
            shouldSiege = true;
        }

        if (targetIsBuilding)
        {
            const int buildingsInRange = countBuildingsInSiegeRange(tank, tankTargets);

            // One surviving production/static-defense building is enough.
            // Requiring two made every tank except the vanguard stay in tank
            // mode and eventually walk away from the last structure.
            shouldSiege =
                buildingsInRange >= 1 &&
                distanceToTarget >= 96 &&
                distanceToTarget <= siegeTankRange - siegeEnterMargin;
        }


        // Remember the tactical intent before transient command/cooldown gates
        // are applied. If Siege tech is ready and this target belongs in the
        // artillery envelope, a Tank that happens to be in a short post-move
        // or post-unsiege cooldown must WAIT and retry Siege, not interpret the
        // failed transform as permission to drive forward in Tank Mode.
        const bool siegeTechReadyForCombat =
            BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode);
        const bool siegeDeploymentPreferred = shouldSiege && siegeTechReadyForCombat;

        bool siegeTransitionUnsafe = false;
        if (shouldSiege &&
            !tankMaySafelyEnterSiege(tank, tankTargets, allAttackTanks))
        {
            shouldSiege = false;
            siegeTransitionUnsafe = true;
        }

        bool shouldUnsiege =
            !target ||
            distanceToTarget < EMERGENCY_UNSIEGE_DISTANCE ||
            distanceToTarget > siegeTankRange + (targetIsBuilding ? 0 : SIEGE_EXIT_MARGIN) ||
            tank->isUnderDisruptionWeb();

        if (tank->isSieged() &&
            rememberedEnemyTankThreatForTank(tank, 18 * 32) &&
            distanceToTarget >= EMERGENCY_UNSIEGE_DISTANCE &&
            !tank->isUnderDisruptionWeb())
        {
            shouldUnsiege = false;
        }

        if (now < st.holdTankUntil || now - st.lastUnsiegeFrame < POST_UNSIEGE_COOLDOWN)
        {
            shouldSiege = false;
        }

        if (tank->isSieged() && now < st.holdSiegeUntil)
        {
            if (!enemyTooClose && !tank->isUnderDisruptionWeb())
            {
                shouldUnsiege = false;
            }
        }

        if (tank->isSieged() && now < st.holdRecentEnemyUntil)
        {
            if (!enemyTooClose && !tank->isUnderDisruptionWeb())
            {
                shouldUnsiege = false;

                DrawUnitTargetTextMap(
                    tank->getPosition() + BWAPI::Position(0, 16),
                    seesEnemyNow ? "enemy visible" : "enemy memory"
                );
            }
        }

        if (tank->isSieged() &&
            target &&
            distanceToTarget >= EMERGENCY_UNSIEGE_DISTANCE &&
            distanceToTarget <= siegeTankRange + (targetIsBuilding ? 0 : SIEGE_EXIT_MARGIN) &&
            !tank->isUnderDisruptionWeb())
        {
            shouldUnsiege = false;
        }

        if (tankFrontCenter.isValid() && !isFrontBandTank &&
            tank->isSieged() &&
            !rememberedEnemyTankThreatForTank(tank, 18 * 32) &&
            tank->getDistance(tankFrontCenter) > 12 * 32 &&
            distanceToTarget > siegeTankRange + SIEGE_EXIT_MARGIN)
        {
            DrawUnitTargetTextMap(
                tank->getPosition() + BWAPI::Position(0, 8),
                "unsiege join"
            );

            if (the.micro.Unsiege(tank))
            {
                st.lastUnsiegeFrame = now;
                st.holdTankUntil = now + TANK_HOLD_FRAMES;
            }
            continue;
        }

        if (!tank->isSieged() && siegeDeploymentPreferred && !siegeTransitionUnsafe &&
            now >= st.holdTankUntil && now - st.lastUnsiegeFrame >= POST_UNSIEGE_COOLDOWN)
        {
            if (st.combatRecoveryTargetID != target->getID())
            {
                st.combatRecoveryTargetID = target->getID();
                st.combatSiegeBlockedSince = -100000;
                st.combatRecoveryPosition = BWAPI::Positions::Invalid;
            }
            if (st.combatRecoveryPosition.isValid())
            {
                if (now - st.combatRecoveryFrame > 72 ||
                    !tankParkingTerrainIsClear(st.combatRecoveryPosition) ||
                    tankParkingOccupiedByAnotherTank(st.combatRecoveryPosition, tank))
                    st.combatRecoveryPosition = BWAPI::Positions::Invalid;
                else if (tank->getPosition().getApproxDistance(st.combatRecoveryPosition) > 12)
                {
                    the.micro.Move(tank, st.combatRecoveryPosition);
                    continue;
                }
                else st.combatRecoveryPosition = BWAPI::Positions::Invalid;
            }
            if (!tankParkingTerrainIsClear(tank->getPosition()) || !tank->canSiege())
            {
                shouldSiege = false;
                if (st.combatSiegeBlockedSince < 0) st.combatSiegeBlockedSince = now;
            }
            if (st.combatSiegeBlockedSince >= 0 && now - st.combatSiegeBlockedSince >= 24 &&
                now - st.combatRecoveryFrame >= 48)
            {
                st.combatRecoveryPosition = findCombatTankSiegePosition(tank, target);
                st.combatRecoveryFrame = now;
                st.combatSiegeBlockedSince = now;
                if (st.combatRecoveryPosition.isValid())
                {
                    DrawUnitTargetTextMap(tank->getPosition() + BWAPI::Position(0, 8), "combat siege relocate");
                    the.micro.Move(tank, st.combatRecoveryPosition);
                    continue;
                }
            }
        }

        if (tank->canSiege() && shouldSiege && !shouldUnsiege)
        {
            DrawUnitTargetTextMap(
                tank->getPosition() + BWAPI::Position(0, 8),
                isFrontBandTank ? "front siege" : "siege"
            );

            if (the.micro.Siege(tank))
            {
                st.lastSiegeFrame = now;
                st.combatSiegeBlockedSince = -100000;
            }
            else if (st.combatSiegeBlockedSince < 0) st.combatSiegeBlockedSince = now;
        }
        else if (tank->canUnsiege() && shouldUnsiege)
        {
            DrawUnitTargetTextMap(
                tank->getPosition() + BWAPI::Position(0, 8),
                enemyTooClose ? "too close" : "unsiege"
            );

            if (the.micro.Unsiege(
                tank,
                enemyTooClose || tank->isUnderDisruptionWeb()))
            {
                st.lastUnsiegeFrame = now;
                st.holdTankUntil = now + TANK_HOLD_FRAMES;
            }
        }
        else
        {
            if (tank->isSieged())
            {
                the.micro.AttackUnit(tank, target);
            }
            else
            {
                if (distanceToTarget < EMERGENCY_UNSIEGE_DISTANCE && !target->getType().isBuilding())
                {
                    the.micro.KiteTarget(tank, target);
                }
                else if ((siegeTransitionUnsafe ||
                          (siegeDeploymentPreferred &&
                           !tank->isUnderDisruptionWeb() &&
                           (!shouldSiege || !tank->canSiege()))) &&
                    distanceToTarget >
                        BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode.groundWeapon().maxRange())
                {
                    // If deploying is temporarily unsafe or merely blocked by
                    // a short cooldown, do not interpret that as permission to
                    // drive forward into direct-fire range. Wait and retry.
                    the.micro.HoldPosition(tank);
                }
                else if (!staticCleanup && tankFrontCenter.isValid() &&
                    !isFrontBandTank &&
                    distanceToTarget > siegeTankRange - siegeEnterMargin)
                {
                    BWAPI::Position fireLinePosition =
                        getWideFireLinePositionNearTankFront(
                            tank,
                            tankFrontCenter,
                            allAttackTanks,
                            target->getPosition()
                        );

                    if (fireLinePosition.isValid())
                    {
                        if (handleTankEmergencyBeforeForcedMove(
                            tank,
                            tankTargets,
                            now,
                            siegeTankRange,
                            EMERGENCY_UNSIEGE_DISTANCE,
                            TANK_HOLD_FRAMES,
                            POST_UNSIEGE_COOLDOWN,
                            st.lastUnsiegeFrame,
                            st.holdTankUntil,
                            st.lastSiegeFrame))
                        {
                            continue;
                        }

                        if (tank->getDistance(fireLinePosition) > 64)
                        {
                            the.micro.Move(tank, fireLinePosition);
                        }
                        else
                        {
                            the.micro.HoldPosition(tank);
                        }
                    }
                    else
                    {
                        the.micro.AttackUnit(tank, target);
                    }
                }
                else
                {
                    the.micro.AttackUnit(tank, target);
                }
            }
        }
    }
}

static int estimatedTankShotDamage(BWAPI::Unit tank)
{
    const BWAPI::WeaponType weapon =
        tank && tank->isSieged()
        ? BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon()
        : BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode.groundWeapon();

    return std::max(1, weapon.damageAmount() * weapon.damageFactor());
}

static int getCachedTankSplashValue(BWAPI::Unit target)
{
    if (!target || !target->exists() || !target->getPosition().isValid())
    {
        return 0;
    }

    const auto cached = splashValueByTargetID.find(target->getID());
    if (cached != splashValueByTargetID.end())
    {
        return cached->second;
    }

    const BWAPI::WeaponType weapon =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon();
    const int outer = weapon.outerSplashRadius();
    const int median = weapon.medianSplashRadius();
    const int inner = weapon.innerSplashRadius();
    int value = 0;

    for (BWAPI::Unit unit : BWAPI::Broodwar->getUnitsInRadius(target->getPosition(), outer))
    {
        if (!unit || !unit->exists() || unit->isFlying() || unit->isInvincible())
        {
            continue;
        }

        const int distance = unit->getDistance(target->getPosition());
        int splashWeight = 1;
        if (distance <= inner) splashWeight = 4;
        else if (distance <= median) splashWeight = 2;

        int unitValue = std::max(2, unit->getType().supplyRequired() + 2);
        if (unit->getType().isWorker()) unitValue += 4;

        if (unit->getPlayer() == BWAPI::Broodwar->enemy())
        {
            value += splashWeight * unitValue;
        }
        else if (unit->getPlayer() == BWAPI::Broodwar->self())
        {
            // Friendly splash is more expensive than a merely imperfect enemy
            // shot. This also discourages deploying into our own infantry line.
            value -= 2 * splashWeight * unitValue;
        }
    }

    splashValueByTargetID[target->getID()] = value;
    return value;
}


static bool tankBuildingHasVisibleRepairer(BWAPI::Unit building)
{
    if (!building || !building->exists() || !building->getType().isBuilding() ||
        building->isFlying() || building->isLifted())
    {
        return false;
    }

    const int buildingID = building->getID();
    const auto cached = repairedBuildingByTargetID.find(buildingID);
    if (cached != repairedBuildingByTargetID.end())
    {
        return cached->second;
    }

    bool repaired = false;
    for (BWAPI::Unit worker : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!worker || !worker->exists() || !worker->isVisible() ||
            !worker->getType().isWorker() || !worker->isRepairing())
        {
            continue;
        }

        if (worker->getOrderTarget() == building || worker->getDistance(building) <= 64)
        {
            repaired = true;
            break;
        }
    }

    repairedBuildingByTargetID[buildingID] = repaired;
    return repaired;
}

static int coordinatedTankCountForTarget(BWAPI::Unit target)
{
    if (!target)
    {
        return 0;
    }

    int count = 0;
    for (const auto& assignment : coordinatedTargetByTankID)
    {
        if (assignment.second == target)
        {
            ++count;
        }
    }
    return count;
}

BWAPI::Unit MicroTanks::getTarget(BWAPI::Unit tank, const BWAPI::Unitset& targets)
{
    if (!tank || !tank->exists())
    {
        return nullptr;
    }

    const int now = BWAPI::Broodwar->getFrameCount();
    if (coordinatedTankTargetFrame != now)
    {
        coordinatedTankTargetFrame = now;
        coordinatedTargetByTankID.clear();
        coordinatedDamageByTankID.clear();
        reservedDamageByTargetID.clear();
        splashValueByTargetID.clear();
        repairedBuildingByTargetID.clear();
    }

    const int siegeTankRange =
        BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode.groundWeapon().maxRange();

    BWAPI::Unitset availableTargets;
    BWAPI::Unitset targetsInSiegeRange;
    BWAPI::Unitset firingTargets;
    for (BWAPI::Unit target : targets)
    {
        if (!visibleTankGroundTarget(target))
        {
            continue;
        }

        if (target->getType().isBuilding() &&
            !tankCanLocallyEngageBuilding(tank, target))
        {
            // Do not turn a building which is close only across a cliff into
            // an AttackUnit order. The no-target branch advances along the
            // shared terrain-aware enemy goal until the building becomes a
            // real local target (or a valid over-cliff artillery shot).
            continue;
        }

        const int distance = tank->getDistance(target);
        // Shared vision supplies local firing opportunities, not individual
        // objectives across the map. Reinforcements follow enemyGoal/front;
        // they must not chase a high-priority target at a different battle.
        if (distance > siegeTankRange + 4 * 32)
        {
            continue;
        }
        availableTargets.insert(target);
        if (distance <= siegeTankRange)
        {
            targetsInSiegeRange.insert(target);
        }
        if (targetInTankSiegeRange(tank, target))
        {
            firingTargets.insert(target);
        }
    }

    // Do not aim a deployed Tank at a unit inside its dead zone or beyond its
    // range while a lower-priority target can actually be hit right now.
    const BWAPI::Unitset& newTargets =
        tank->isSieged() && !firingTargets.empty() ? firingTargets :
        targetsInSiegeRange.empty() ? availableTargets : targetsInSiegeRange;

    bool hasEnemyUnit = false;
    bool hasEnemyCombatUnit = false;
    for (BWAPI::Unit target : newTargets)
    {
        if (!target || !target->exists() || isNeutralEgg(target) || target->getType().isBuilding())
        {
            continue;
        }

        hasEnemyUnit = true;
        if (getAttackPriority(tank, target) >= 7)
        {
            hasEnemyCombatUnit = true;
        }
    }

    auto isUsableCandidate =
        [hasEnemyUnit, hasEnemyCombatUnit, &newTargets](BWAPI::Unit target)
    {
        if (!target || !target->exists() ||
            newTargets.find(target) == newTargets.end())
        {
            return false;
        }

        if (!hasEnemyUnit || !target->getType().isBuilding())
        {
            return true;
        }

        // A repaired structure may keep a small firing section on it only when
        // the visible non-building alternatives are low-priority workers/etc.
        // Real combat threats still take precedence exactly as before.
        return !hasEnemyCombatUnit && tankBuildingHasVisibleRepairer(target);
    };

    const int tankID = tank->getID();
    const auto assigned = coordinatedTargetByTankID.find(tankID);
    if (assigned != coordinatedTargetByTankID.end() &&
        isUsableCandidate(assigned->second))
    {
        return assigned->second;
    }

    if (assigned != coordinatedTargetByTankID.end())
    {
        const BWAPI::Unit oldTarget = assigned->second;
        const int oldDamage = coordinatedDamageByTankID[tankID];
        if (oldTarget)
        {
            reservedDamageByTargetID[oldTarget->getID()] = std::max(
                0,
                reservedDamageByTargetID[oldTarget->getID()] - oldDamage);
        }
        coordinatedTargetByTankID.erase(tankID);
        coordinatedDamageByTankID.erase(tankID);
    }

    TankDecisionState& state = stateFor(tank);
    BWAPI::Unit bestTarget = nullptr;
    long long bestScore = LLONG_MIN;
    int bestPriority = 0;

    for (BWAPI::Unit target : newTargets)
    {
        if (!isUsableCandidate(target))
        {
            continue;
        }

        const int distance = tank->getDistance(target);
        const int priority = getAttackPriority(tank, target);
        if (priority <= 0)
        {
            continue;
        }

        const int health = std::max(1, target->getHitPoints() + target->getShields());
        const int reserved = reservedDamageByTargetID[target->getID()];
        const bool alreadyCovered = reserved >= health;
        const int splashValue =
            target->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine
            ? 0
            : getCachedTankSplashValue(target);

        long long score =
            static_cast<long long>(priority) * 1000000LL +
            static_cast<long long>(splashValue) * 1000LL -
            distance;

        if (target->getType().isBuilding())
        {
            const bool repaired = tankBuildingHasVisibleRepairer(target);
            const int focusCount = coordinatedTankCountForTarget(target);

            // During cleanup, or when SCVs are actively undoing our damage, let
            // a small firing section finish the structure. Three Tanks is enough
            // to beat repair without collapsing the whole safe formation forward.
            if (focusCount < 3 &&
                distance <= siegeTankRange &&
                !hasEnemyCombatUnit &&
                (!hasEnemyUnit || repaired))
            {
                score += repaired ? 5000000LL : 900000LL;
            }
        }

        // Do not waste a second full tank shot when another valid target is
        // available. The penalty is deliberately smaller than the mine/nuking
        // Ghost priority gap, so emergency targets remain emergency targets.
        if (alreadyCovered && newTargets.size() > 1)
        {
            score -= 750000LL;
        }

        if (target == state.preferredTarget && now < state.preferredTargetUntil)
        {
            score += 150000LL;
        }

        if (!bestTarget || score > bestScore ||
            (score == bestScore && target->getID() < bestTarget->getID()))
        {
            bestScore = score;
            bestPriority = priority;
            bestTarget = target;
        }
    }

    // Preserve the previous target for a few frames when it remains nearly as
    // important. This removes target flicker without masking mines or nukes.
    if (bestTarget &&
        state.preferredTarget &&
        now < state.preferredTargetUntil &&
        isUsableCandidate(state.preferredTarget))
    {
        const int previousPriority = getAttackPriority(tank, state.preferredTarget);
        if (previousPriority >= bestPriority - 1 &&
            reservedDamageByTargetID[state.preferredTarget->getID()] <
                state.preferredTarget->getHitPoints() + state.preferredTarget->getShields())
        {
            bestTarget = state.preferredTarget;
        }
    }

    if (!bestTarget)
    {
        state.preferredTarget = nullptr;
        state.preferredTargetUntil = -100000;
        return nullptr;
    }

    bestTarget = preferThreatToStructure(tank, bestTarget, newTargets);

    const int shotDamage = estimatedTankShotDamage(tank);
    coordinatedTargetByTankID[tankID] = bestTarget;
    coordinatedDamageByTankID[tankID] = shotDamage;
    reservedDamageByTargetID[bestTarget->getID()] += shotDamage;

    state.preferredTarget = bestTarget;
    state.preferredTargetUntil = now + 8;
    return bestTarget;
}

// Only targets that the tank can potentially attack go into the target set.
int MicroTanks::getAttackPriority(BWAPI::Unit tank, BWAPI::Unit target)
{
    if (!visibleTankGroundTarget(target) || target->isLifted())
    {
        return 0;
    }

    BWAPI::UnitType targetType = target->getType();

    // A detected Spider Mine can remove several packed vehicles at once. Fire
    // on it before ordinary combat units; tank-mode shots are used because the
    // siege decision explicitly rejects mines.
    if (targetType == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
    {
        return 16;
    }

    if (target->getType() == BWAPI::UnitTypes::Zerg_Larva)
    {
        return 0;
    }

    // If it's under dark swarm, we can't hurt it unless we're sieged.
    if (target->isUnderDarkSwarm() && !tank->isSieged())
    {
        return 0;
    }

    if (isNeutralEgg(target)) return getBackstopAttackPriority(target);

    // A ghost which is nuking is the highest priority by a mile.
    if (targetType == BWAPI::UnitTypes::Terran_Ghost &&
        (target->getOrder() == BWAPI::Orders::NukePaint ||
            target->getOrder() == BWAPI::Orders::NukeTrack))
    {
        return 15;
    }

    // if the target is building something near our base something is fishy
    BWAPI::Position ourBasePosition = BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());
    if (target->getType().isWorker() && (target->isConstructing() || target->isRepairing()) && target->getDistance(ourBasePosition) < 1200)
    {
        return 12;
    }

    if (target->getType().isBuilding() && target->getDistance(ourBasePosition) < 1200)
    {
        return 12;
    }

    const bool isThreat = UnitUtil::TypeCanAttackGround(targetType) && !target->getType().isWorker();    // includes bunkers

    if (tank->isSieged() && tank->getDistance(target) < 64)
    {
        // The potential target is too close to hit in siege mode. Give it a lower priority.
        if (isThreat)
        {
            return 9;		// lower than the default threat priority
        }
        return 0;
    }

    // The most dangerous enemy units.
    if (targetType == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        targetType == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
        targetType == BWAPI::UnitTypes::Protoss_High_Templar ||
        targetType == BWAPI::UnitTypes::Protoss_Dark_Archon ||
        targetType == BWAPI::UnitTypes::Protoss_Archon ||
        targetType == BWAPI::UnitTypes::Protoss_Dark_Templar ||
        targetType == BWAPI::UnitTypes::Protoss_Reaver ||
        targetType == BWAPI::UnitTypes::Zerg_Infested_Terran ||
        targetType == BWAPI::UnitTypes::Zerg_Defiler ||
        targetType == BWAPI::UnitTypes::Zerg_Lurker)
    {
        return 12;
    }
    if (isThreat)
    {
        if (targetType.size() == BWAPI::UnitSizeTypes::Large)
        {
            return 11;
        }
        return 10;
    }
    if (targetType == BWAPI::UnitTypes::Zerg_Nydus_Canal)
    {
        return 11;
    }

    return getBackstopAttackPriority(target);
}
