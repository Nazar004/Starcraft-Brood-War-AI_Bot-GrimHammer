#include "Combat/Micro/Terran/MicroGhost.h"

#include <cmath>
#include <limits>

#include "Map/Bases.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"

using namespace UAlbertaBot;

MicroGhost::MicroGhost()
{
}

void MicroGhost::executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster)
{
    BWAPI::Unitset ghosts = Intersection(getUnits(), cluster.units);
    if (ghosts.empty())
    {
        return;
    }

    updateCasters(ghosts);
    cleanState();

    for (BWAPI::Unit ghost : ghosts)
    {
        if (!ghost || !ghost->exists() || !ghost->isCompleted() || !ghost->getPosition().isValid())
        {
            continue;
        }

        const int now = BWAPI::Broodwar->getFrameCount();
        const auto retreatIt = _retreatUntilFrame.find(ghost->getID());
        if (retreatIt != _retreatUntilFrame.end() && retreatIt->second > now)
        {
            if (cloakForRetreat(ghost))
            {
                continue;
            }

            BWAPI::Position support = getSupportPosition(ghost, cluster);
            if (support.isValid() && ghost->getDistance(support) > 2 * 32)
            {
                the.micro.MoveNear(ghost, support);
            }
            continue;
        }

        // The escape is complete. Decloak out of danger so energy can recover
        // for the next Lockdown instead of being drained while following.
        if (retreatIt != _retreatUntilFrame.end())
        {
            _retreatUntilFrame.erase(retreatIt);
            if (ghost->isCloaked() && ghost->canDecloak())
            {
                ghost->decloak();
                continue;
            }
        }

        // Lockdown is the primary role, but an idle Ghost should still fire
        // its rifle at a detected ground enemy already in range. Do not chase
        // a low-value target away from the Goliath line just to use the gun.
        BWAPI::Unit nearbyGroundTarget = nullptr;
        int nearbyGroundDistance = MAX_DISTANCE;
        for (BWAPI::Unit enemy : targets)
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                !enemy->isDetected() || enemy->isFlying() ||
                !ghost->isInWeaponRange(enemy))
            {
                continue;
            }
            const int distance = ghost->getDistance(enemy);
            if (distance < nearbyGroundDistance)
            {
                nearbyGroundDistance = distance;
                nearbyGroundTarget = enemy;
            }
        }

        BWAPI::Unit target = getTarget(ghost, targets);
        if (target)
        {
            const bool lockdownCandidate =
                BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Lockdown) &&
                ghost->getEnergy() >= BWAPI::TechTypes::Lockdown.energyCost() &&
                target->exists() &&
                target->isVisible() &&
                target->isDetected() &&
                target->getType().isMechanical() &&
                !target->getType().isBuilding() &&
                !target->isLockedDown();

            if (lockdownCandidate)
            {
                if (!hasGoliathSupport(ghost, target))
                {
                    BWAPI::Position support = getSupportPosition(ghost, cluster);
                    if (support.isValid() && ghost->getDistance(support) > 2 * 32)
                    {
                        the.micro.MoveNear(ghost, support);
                    }
                    continue;
                }

                _approachReservationUntilFrame[target->getID()] = now + 3 * 24;
                _approachTargetByGhost[ghost->getID()] = target->getID();

                if (cloakForApproach(ghost, target))
                {
                    continue;
                }

                if (canLockdownTarget(ghost, target) &&
                    spell(ghost, BWAPI::TechTypes::Lockdown, target))
                {
                    _lockdownReservationUntilFrame[target->getID()] = now + 3 * 24;
                    _retreatUntilFrame[ghost->getID()] = now + 7 * 24;
                    _approachReservationUntilFrame.erase(target->getID());
                    _approachTargetByGhost.erase(ghost->getID());
                    continue;
                }

                if (ghost->getDistance(target) > BWAPI::WeaponTypes::Lockdown.maxRange())
                {
                    const BWAPI::Position approach =
                        getLockdownApproachPosition(ghost, target, cluster);
                    if (approach.isValid())
                    {
                        the.micro.MoveNear(ghost, approach);
                    }
                    else
                    {
                        const BWAPI::Position support = getSupportPosition(ghost, cluster);
                        if (support.isValid() && ghost->getDistance(support) > 2 * 32)
                        {
                            the.micro.MoveNear(ghost, support);
                        }
                    }
                    continue;
                }
            }

            if (nearbyGroundTarget && !ghost->isInWeaponRange(target))
            {
                the.micro.AttackUnit(ghost, nearbyGroundTarget);
            }
            else if (!target->isFlying())
            {
                the.micro.CatchAndAttackUnit(ghost, target);
            }
            else
            {
                if (nearbyGroundTarget)
                {
                    the.micro.AttackUnit(ghost, nearbyGroundTarget);
                }
                else
                {
                    BWAPI::Position support = getSupportPosition(ghost, cluster);
                    if (support.isValid() && ghost->getDistance(support) > 2 * 32)
                    {
                        the.micro.MoveNear(ghost, support);
                    }
                }
            }
        }
        else
        {
            if (nearbyGroundTarget)
            {
                the.micro.AttackUnit(ghost, nearbyGroundTarget);
            }
            else
            {
                BWAPI::Position support = getSupportPosition(ghost, cluster);
                if (support.isValid() && ghost->getDistance(support) > 2 * 32)
                {
                    the.micro.MoveNear(ghost, support);
                }
            }
        }
    }
}

void MicroGhost::updateMovement(const UnitCluster& cluster, BWAPI::Unit vanguard)
{
    BWAPI::Unitset ghosts = Intersection(getUnits(), cluster.units);
    if (ghosts.empty())
    {
        return;
    }

    cleanState();
    const int now = BWAPI::Broodwar->getFrameCount();

    for (BWAPI::Unit ghost : ghosts)
    {
        if (!ghost || !ghost->exists() || !ghost->isCompleted() || ghost->isLoaded())
        {
            continue;
        }

        const auto retreatIt = _retreatUntilFrame.find(ghost->getID());
        if (retreatIt != _retreatUntilFrame.end() && retreatIt->second > now &&
            cloakForRetreat(ghost))
        {
            continue;
        }

        if (retreatIt != _retreatUntilFrame.end() && retreatIt->second <= now)
        {
            _retreatUntilFrame.erase(retreatIt);
            if (ghost->isCloaked() && ghost->canDecloak())
            {
                ghost->decloak();
                continue;
            }
        }

        // The combat update owns movement while closing for Lockdown. Sending
        // the Ghost back to its escort here made it shuttle between two orders.
        if (_approachTargetByGhost.find(ghost->getID()) !=
                _approachTargetByGhost.end() ||
            _cloakRequestUntilFrame.find(ghost->getID()) !=
                _cloakRequestUntilFrame.end())
        {
            continue;
        }

        BWAPI::Position support = getSupportPosition(ghost, cluster);
        if (!support.isValid() && vanguard && vanguard->exists())
        {
            support = vanguard->getPosition();
        }

        if (support.isValid() && ghost->getDistance(support) > 2 * 32)
        {
            the.micro.MoveNear(ghost, support);
        }
    }
}

BWAPI::Position MicroGhost::getSupportPosition(BWAPI::Unit ghost, const UnitCluster& cluster) const
{
    long sumX = 0;
    long sumY = 0;
    int count = 0;

    for (BWAPI::Unit unit : cluster.units)
    {
        if (!unit || !unit->exists() || unit->isLoaded() ||
            !unit->getPosition().isValid() ||
            unit->getType() == BWAPI::UnitTypes::Terran_Ghost ||
            unit->getType().isWorker() ||
            unit->getType().isBuilding())
        {
            continue;
        }

        sumX += unit->getPosition().x;
        sumY += unit->getPosition().y;
        ++count;
    }

    BWAPI::Position armyCenter = count > 0
        ? BWAPI::Position(int(sumX / count), int(sumY / count))
        : BWAPI::Positions::Invalid;

    // A detached cluster can consist entirely of Ghosts. Its own center is
    // not a support position: following it leaves the Ghosts idle wherever
    // clustering separated them from the Goliaths.
    if (!armyCenter.isValid() && ghost && ghost->getPosition().isValid())
    {
        BWAPI::Unit bestGoliath = nullptr;
        BWAPI::Unit bestGroundSupport = nullptr;
        int goliathScore = MAX_DISTANCE;
        int supportScore = MAX_DISTANCE;
        const BWAPI::Position objective =
            order && order->getPosition().isValid()
            ? order->getPosition()
            : BWAPI::Positions::Invalid;
        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists() || !unit->isCompleted() ||
                !unit->getPosition().isValid() || unit->isFlying() ||
                unit->getType().isWorker() || unit->getType().isBuilding() ||
                unit->getType() == BWAPI::UnitTypes::Terran_Ghost ||
                !the.partitions.connected(ghost->getPosition(), unit->getPosition()))
            {
                continue;
            }

            // Follow the forward army, not the newest Goliath at home. A
            // Ghost-only cluster used to choose the unit nearest the Ghost,
            // so it could keep circling the base while the attack marched on.
            const int score = ghost->getDistance(unit) +
                (objective.isValid()
                    ? 3 * unit->getDistance(objective)
                    : 0);
            if (unit->getType() == BWAPI::UnitTypes::Terran_Goliath &&
                score < goliathScore)
            {
                goliathScore = score;
                bestGoliath = unit;
            }
            if (score < supportScore)
            {
                supportScore = score;
                bestGroundSupport = unit;
            }
        }
        BWAPI::Unit support = bestGoliath
            ? bestGoliath : bestGroundSupport;
        if (support)
        {
            armyCenter = support->getPosition();
        }
    }

    if (!armyCenter.isValid() && the.bases.myMain())
    {
        armyCenter = the.bases.myMain()->getCenter();
    }

    if (!armyCenter.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position forwardTarget =
        order && order->getPosition().isValid()
        ? order->getPosition()
        : armyCenter;

    double dx = double(forwardTarget.x - armyCenter.x);
    double dy = double(forwardTarget.y - armyCenter.y);
    double length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0)
    {
        dx = 0.0;
        dy = -1.0;
        length = 1.0;
    }

    dx /= length;
    dy /= length;

    const int rearDistance = 4 * 32;
    const int lateralSlot = ghost ? (ghost->getID() % 5) - 2 : 0;
    const int lateralDistance = lateralSlot * 20;

    int x = int(armyCenter.x - dx * rearDistance - dy * lateralDistance);
    int y = int(armyCenter.y - dy * rearDistance + dx * lateralDistance);

    const int maxX = BWAPI::Broodwar->mapWidth() * 32 - 1;
    const int maxY = BWAPI::Broodwar->mapHeight() * 32 - 1;
    x = std::max(0, std::min(x, maxX));
    y = std::max(0, std::min(y, maxY));

    const BWAPI::Position desired(x, y);
    const BWAPI::TilePosition desiredTile(desired);
    const BWAPI::TilePosition centerTile(armyCenter);
    if (!desiredTile.isValid() || !centerTile.isValid() ||
        !the.map.isRuntimeWalkable(desiredTile) ||
        the.partitions.id(desiredTile) != the.partitions.id(centerTile))
    {
        return armyCenter;
    }
    return desired;
}

BWAPI::Position MicroGhost::getLockdownApproachPosition(
    BWAPI::Unit ghost,
    BWAPI::Unit target,
    const UnitCluster& cluster) const
{
    if (!ghost || !ghost->exists() || !ghost->getPosition().isValid() ||
        !target || !target->exists() || !target->getPosition().isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position armyCenter = cluster.center;
    if (!armyCenter.isValid())
    {
        armyCenter = ghost->getPosition();
    }

    double dx = double(target->getPosition().x - armyCenter.x);
    double dy = double(target->getPosition().y - armyCenter.y);
    double length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0)
    {
        dx = double(target->getPosition().x - ghost->getPosition().x);
        dy = double(target->getPosition().y - ghost->getPosition().y);
        length = std::sqrt(dx * dx + dy * dy);
        if (length < 1.0)
        {
            return BWAPI::Positions::Invalid;
        }
    }

    dx /= length;
    dy /= length;

    const int castRange = BWAPI::WeaponTypes::Lockdown.maxRange();
    const double sideX = -dy;
    const double sideY = dx;
    static const int sideOffsets[] = { 0, -32, 32, -64, 64, -96, 96 };
    for (int setback = 24; setback <= 88; setback += 32)
    {
        const int distance = castRange - setback;
        for (const int side : sideOffsets)
        {
            const BWAPI::Position candidate(
                int(target->getPosition().x - dx * distance + sideX * side),
                int(target->getPosition().y - dy * distance + sideY * side));
            const BWAPI::TilePosition tile(candidate);
            if (candidate.isValid() && tile.isValid() &&
                candidate.x < BWAPI::Broodwar->mapWidth() * 32 &&
                candidate.y < BWAPI::Broodwar->mapHeight() * 32 &&
                candidate.getApproxDistance(target->getPosition()) <= castRange - 16 &&
                the.map.isRuntimeWalkable(tile) &&
                the.partitions.connected(ghost->getPosition(), candidate))
            {
                return candidate;
            }
        }
    }
    return BWAPI::Positions::Invalid;
}

bool MicroGhost::targetIsReserved(BWAPI::Unit target, BWAPI::Unit ghost) const
{
    if (!target)
    {
        return false;
    }

    const int now = BWAPI::Broodwar->getFrameCount();
    const int targetID = target->getID();

    const auto lockdownIt = _lockdownReservationUntilFrame.find(targetID);
    if (lockdownIt != _lockdownReservationUntilFrame.end() &&
        lockdownIt->second > now)
    {
        return true;
    }

    const auto approachIt = _approachReservationUntilFrame.find(targetID);
    if (approachIt == _approachReservationUntilFrame.end() ||
        approachIt->second <= now)
    {
        return false;
    }

    if (ghost)
    {
        const auto ownIt = _approachTargetByGhost.find(ghost->getID());
        if (ownIt != _approachTargetByGhost.end() && ownIt->second == targetID)
        {
            return false;
        }
    }

    return true;
}

bool MicroGhost::enemyDetectorNear(BWAPI::Position position, int radius) const
{
    if (!position.isValid())
    {
        return true;
    }

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (enemy && enemy->exists() && enemy->isVisible() &&
            enemy->getPosition().isValid() && enemy->getType().isDetector() &&
            enemy->getDistance(position) <= radius)
        {
            return true;
        }
    }

    return false;
}

bool MicroGhost::hasGoliathSupport(BWAPI::Unit ghost, BWAPI::Unit target) const
{
    if (!ghost || !target ||
        ghost->getDistance(target) > 14 * 32)
    {
        return false;
    }

    int nearbyGoliaths = 0;
    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (unit && unit->exists() && unit->isCompleted() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Goliath &&
            (unit->getDistance(target) <= 12 * 32 ||
             (ghost->getDistance(target) <= 10 * 32 &&
              unit->getDistance(ghost) <= 8 * 32)))
        {
            ++nearbyGoliaths;
        }
    }

    return nearbyGoliaths >= 3;
}

bool MicroGhost::cloakForApproach(BWAPI::Unit ghost, BWAPI::Unit target)
{
    if (!ghost || !target ||
        !BWAPI::Broodwar->self()->hasResearched(
            BWAPI::TechTypes::Personnel_Cloaking))
    {
        return false;
    }

    if (ghost->isCloaked())
    {
        _cloakRequestUntilFrame.erase(ghost->getID());
        return false;
    }
    if (_cloakRequestUntilFrame.find(ghost->getID()) !=
        _cloakRequestUntilFrame.end())
    {
        return true;
    }

    const int requiredEnergy =
        BWAPI::TechTypes::Lockdown.energyCost() +
        BWAPI::TechTypes::Personnel_Cloaking.energyCost() + 10;

    if (ghost->getEnergy() < requiredEnergy ||
        enemyDetectorNear(ghost->getPosition(), 9 * 32) ||
        ghost->getDistance(target) > 10 * 32)
    {
        return false;
    }

    if (ghost->canCloak() && ghost->cloak())
    {
        _cloakRequestUntilFrame[ghost->getID()] =
            BWAPI::Broodwar->getFrameCount() + 8;
        return true;
    }
    return false;
}

bool MicroGhost::cloakForRetreat(BWAPI::Unit ghost)
{
    if (!ghost ||
        !BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Personnel_Cloaking))
    {
        return false;
    }

    if (ghost->isCloaked())
    {
        _cloakRequestUntilFrame.erase(ghost->getID());
        return false;
    }
    if (_cloakRequestUntilFrame.find(ghost->getID()) !=
        _cloakRequestUntilFrame.end())
    {
        return true;
    }

    if (ghost->getEnergy() >= BWAPI::TechTypes::Personnel_Cloaking.energyCost() &&
        ghost->canCloak() && ghost->cloak())
    {
        _cloakRequestUntilFrame[ghost->getID()] =
            BWAPI::Broodwar->getFrameCount() + 8;
        return true;
    }
    return false;
}

void MicroGhost::cleanState()
{
    const int now = BWAPI::Broodwar->getFrameCount();

    for (auto it = _lockdownReservationUntilFrame.begin();
        it != _lockdownReservationUntilFrame.end(); )
    {
        if (it->second <= now)
        {
            it = _lockdownReservationUntilFrame.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = _approachReservationUntilFrame.begin();
        it != _approachReservationUntilFrame.end(); )
    {
        if (it->second <= now)
        {
            it = _approachReservationUntilFrame.erase(it);
        }
        else
        {
            ++it;
        }
    }

    std::set<int> liveGhostIDs;
    for (BWAPI::Unit ghost : getUnits())
    {
        if (ghost && ghost->exists())
        {
            liveGhostIDs.insert(ghost->getID());
        }
    }

    for (auto it = _retreatUntilFrame.begin(); it != _retreatUntilFrame.end(); )
    {
        if (liveGhostIDs.find(it->first) == liveGhostIDs.end())
        {
            it = _retreatUntilFrame.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = _approachTargetByGhost.begin();
        it != _approachTargetByGhost.end(); )
    {
        const bool ghostDead =
            liveGhostIDs.find(it->first) == liveGhostIDs.end();
        const auto reservation =
            _approachReservationUntilFrame.find(it->second);
        const bool reservationExpired =
            reservation == _approachReservationUntilFrame.end() ||
            reservation->second <= now;

        if (ghostDead || reservationExpired)
        {
            it = _approachTargetByGhost.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = _cloakRequestUntilFrame.begin();
        it != _cloakRequestUntilFrame.end(); )
    {
        if (it->second <= now ||
            liveGhostIDs.find(it->first) == liveGhostIDs.end())
        {
            it = _cloakRequestUntilFrame.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

BWAPI::Unit MicroGhost::getTarget(BWAPI::Unit ghost, const BWAPI::Unitset& targets) const
{
    int bestScore = std::numeric_limits<int>::min();
    BWAPI::Unit bestTarget = nullptr;

    const auto consider = [&](BWAPI::Unit target)
    {
        if (!target || !target->exists() || !target->isVisible() || !target->isDetected())
        {
            return;
        }

        const BWAPI::UnitType type = target->getType();

        if (type == BWAPI::UnitTypes::Protoss_Interceptor ||
            type == BWAPI::UnitTypes::Protoss_Scarab)
        {
            return;
        }

        if (target->isLockedDown() || targetIsReserved(target, ghost))
        {
            return;
        }

        const int score = getTargetScore(target);
        if (score <= 0)
        {
            return;
        }

        const int distance = ghost->getDistance(target);
        const int value = score * 32 - distance;

        if (value > bestScore)
        {
            bestScore = value;
            bestTarget = target;
        }
    };

    for (BWAPI::Unit target : targets)
    {
        consider(target);
    }

    // A detached Ghost cluster sees only its own sight in MicroManager.
    // Allow a short approach to valuable enemies already visible to the army.
    if (order &&
        BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Lockdown) &&
        ghost->getEnergy() >= BWAPI::TechTypes::Lockdown.energyCost() &&
        (order->getType() == SquadOrderTypes::Attack ||
        order->getType() == SquadOrderTypes::OmniAttack ||
        order->getType() == SquadOrderTypes::Defend))
    {
        for (BWAPI::Unit target : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (target && target->getPosition().isValid() &&
                ghost->getDistance(target) <= 14 * 32)
            {
                consider(target);
            }
        }
    }

    return bestTarget;
}

int MicroGhost::getTargetScore(BWAPI::Unit target) const
{
    if (!target || !target->exists())
    {
        return 0;
    }

    const BWAPI::UnitType type = target->getType();

    if (type == BWAPI::UnitTypes::Protoss_Interceptor ||
        type == BWAPI::UnitTypes::Protoss_Scarab)
    {
        return 0;
    }

    if (type == BWAPI::UnitTypes::Protoss_Carrier)
    {
        return 1000;
    }

    if (type == BWAPI::UnitTypes::Terran_Battlecruiser)
    {
        return 950;
    }

    if (type == BWAPI::UnitTypes::Protoss_Arbiter ||
        type == BWAPI::UnitTypes::Terran_Science_Vessel)
    {
        return 850;
    }

    if (type == BWAPI::UnitTypes::Protoss_Reaver ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
    {
        return 500;
    }

    return 0;
}

bool MicroGhost::canLockdownTarget(BWAPI::Unit ghost, BWAPI::Unit target) const
{
    if (!ghost || !target)
    {
        return false;
    }

    if (!target->exists() ||
        !target->isVisible() ||
        !target->isDetected())
    {
        return false;
    }

    if (!BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Lockdown))
    {
        return false;
    }

    if (ghost->getEnergy() < BWAPI::TechTypes::Lockdown.energyCost())
    {
        return false;
    }

    if (!target->getType().isMechanical())
    {
        return false;
    }

    const BWAPI::UnitType type = target->getType();

    const bool importantLockdownTarget =
        type == BWAPI::UnitTypes::Protoss_Carrier ||
        type == BWAPI::UnitTypes::Terran_Battlecruiser ||
        type == BWAPI::UnitTypes::Protoss_Arbiter ||
        type == BWAPI::UnitTypes::Terran_Science_Vessel ||
        type == BWAPI::UnitTypes::Protoss_Reaver ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;

    if (!importantLockdownTarget)
    {
        return false;
    }

    if (target->getType().isBuilding())
    {
        return false;
    }

    if (target->isLockedDown())
    {
        return false;
    }

    if (ghost->getDistance(target) > BWAPI::WeaponTypes::Lockdown.maxRange())
    {
        return false;
    }

    if (!ghost->canUseTech(BWAPI::TechTypes::Lockdown, target))
    {
        return false;
    }

    return true;
}
