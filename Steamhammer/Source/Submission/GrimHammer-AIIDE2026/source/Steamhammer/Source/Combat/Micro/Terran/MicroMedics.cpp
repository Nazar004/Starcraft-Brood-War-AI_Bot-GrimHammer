#include "Combat/Micro/MicroManager.h"
#include "Combat/Micro/Terran/MicroMedics.h"
#include "Combat/Micro/Terran/TerranSiegeSafety.h"
#include "Map/Bases.h"

#include "Core/The.h"

#include <algorithm>
#include <climits>
#include <vector>

using namespace UAlbertaBot;

MicroMedics::MicroMedics()
{
}

// Unused but required.
void MicroMedics::executeMicro(const BWAPI::Unitset&, const UnitCluster&)
{
}

// Search a small local window instead of allocating full-map path arrays
// for every medic/patient pair. Stop at a safe tile within healing range.
bool MicroMedics::safeHealApproach(BWAPI::Unit medic, BWAPI::Unit patient,
    BWAPI::Position& waypoint) const
{
    waypoint = BWAPI::Positions::Invalid;
    if (medic->getDistance(patient) <= 64) return true;
    const BWAPI::TilePosition start(medic->getPosition());
    if (!start.isValid()) return false;
    constexpr int radius = 8;
    constexpr int side = 2 * radius + 1;
    constexpr int cells = side * side;
    bool visited[side][side] = {};
    BWAPI::TilePosition queue[cells];
    int parent[cells] = {};
    int depth[cells] = {};
    int head = 0, tail = 1;
    queue[0] = start;
    visited[radius][radius] = true;
    const int offsets[][2] = {
        {1,0}, {-1,0}, {0,1}, {0,-1},
        {1,1}, {1,-1}, {-1,1}, {-1,-1}
    };
    while (head < tail)
    {
        const int index = head++;
        const BWAPI::TilePosition tile = queue[index];
        const BWAPI::Position center = BWAPI::Position(tile) + BWAPI::Position(16,16);
        if (index > 0 && patient->getDistance(center) <= 64)
        {
            int first = index;
            while (parent[first] > 0) first = parent[first];
            waypoint = BWAPI::Position(queue[first]) + BWAPI::Position(16,16);
            return true;
        }
        if (depth[index] >= 24) continue;
        for (const auto& offset : offsets)
        {
            const BWAPI::TilePosition next(tile.x + offset[0], tile.y + offset[1]);
            const int x = next.x - start.x + radius;
            const int y = next.y - start.y + radius;
            if (!next.isValid() || x < 0 || y < 0 || x >= side || y >= side ||
                visited[x][y] || the.groundHits(next) > 0 ||
                !the.map.isGroundTransitionRuntimeWalkable(tile, next)) continue;
            visited[x][y] = true;
            queue[tail] = next;
            parent[tail] = index;
            depth[tail++] = depth[index] + 1;
        }
    }
    return false;
}

void MicroMedics::update(const UnitCluster& cluster,
    const BWAPI::Unitset& squadUnits, const BWAPI::Position& regroupPosition)
{
    const int now = the.now();
    if (now < _lastFrame)
    {
        _healAssignments.clear();
        _claimedPatients.clear();
        _infantryFront.reset();
    }
    _lastFrame = now;
    for (auto it = _claimedPatients.begin(); it != _claimedPatients.end(); )
        if (now - it->second.second > 12) it = _claimedPatients.erase(it);
        else ++it;
    for (auto it = _healAssignments.begin(); it != _healAssignments.end(); )
        if (now - it->second.frame > 24) it = _healAssignments.erase(it);
        else ++it;
    const BWAPI::Unitset medics = Intersection(getUnits(), cluster.units);
    if (medics.empty()) return;

    const bool regrouping = cluster.status == ClusterStatus::Regroup;
    const bool attacking = order &&
        (order->getType() == SquadOrderTypes::Attack ||
         order->getType() == SquadOrderTypes::OmniAttack);
    if (!attacking) _infantryFront.reset();

    BWAPI::Unitset activeMedics;
    BWAPI::Unitset availableMedics;
    BWAPI::Unitset busyMedics;
    const std::vector<BWAPI::Unit> coveredBuildings =
        TerranSiegeSafety::coveredBuildings(the.enemy()->getUnits());
    for (BWAPI::Unit medic : medics)
    {
        if (medic && medic->exists() && medic->isCompleted() &&
            !medic->isLoaded() && medic->getPosition().isValid() &&
            !medic->isStasised() && !medic->isMaelstrommed() &&
            !medic->isLockedDown())
        {
            activeMedics.insert(medic);
            if (medic->getEnergy() > 0)
                availableMedics.insert(medic);

            if (regrouping && regroupPosition.isValid() &&
                (medic->isUnderAttack() || medic->getDistance(regroupPosition) > 3 * 32))
            {
                the.micro.MoveSafely(medic, regroupPosition);
                availableMedics.erase(medic);
                busyMedics.insert(medic);
                _healAssignments.erase(medic->getID());
                continue;
            }

            BWAPI::Unit building =
                TerranSiegeSafety::nearestBuilding(medic, coveredBuildings);
            if (medic->isUnderAttack() &&
                medic->getHitPoints() <= medic->getType().maxHitPoints() / 2)
            {
                BWAPI::Unit danger = the.micro.inWeaponsDanger(medic, 0);
                if (danger)
                {
                    the.micro.fleeEnemy(medic, danger, 96);
                    availableMedics.erase(medic);
                    busyMedics.insert(medic);
                    _healAssignments.erase(medic->getID());
                    continue;
                }
            }
            if (building && medic->getDistance(building) < 3 * 32)
            {
                const BWAPI::Position retreat =
                    TerranSiegeSafety::standoff(medic, building);
                if (retreat.isValid() &&
                    the.partitions.connected(medic->getPosition(), retreat))
                    the.micro.MoveSafely(medic, retreat);
                else
                    the.micro.HoldPosition(medic);
                availableMedics.erase(medic);
                busyMedics.insert(medic);
            }
        }
    }

    // Heal nearby members of this squad even if clustering split the Medic
    // from the front. The six-tile assignment limit below prevents chasing
    // wounded reinforcements across the map.
    std::vector<BWAPI::Unit> wounded;
    const auto patient = [&](BWAPI::Unit unit)
    {
        return unit && unit->exists() && unit->isCompleted() &&
            unit->getPlayer() == the.self() && unit->getType().isOrganic() &&
            !unit->getType().isBuilding() && !unit->isLoaded() &&
            !unit->isStasised() && !unit->isInvincible() &&
            unit->getPosition().isValid() && unit->getHitPoints() > 0 &&
            unit->getHitPoints() < unit->getType().maxHitPoints() &&
            !TerranSiegeSafety::inBlastZone(unit, coveredBuildings);
    };
    BWAPI::Unitset patients = squadUnits;
    // Also help a nearby repair SCV or wounded member of a neighboring squad,
    // without chasing that squad across the map.
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!patient(unit)) continue;
        for (BWAPI::Unit medic : activeMedics)
        {
            if (medic->getDistance(unit) <= 2 * 32)
            {
                patients.insert(unit);
                break;
            }
        }
    }
    for (BWAPI::Unit unit : patients)
        if (patient(unit)) wounded.push_back(unit);

    const auto canReachPatient = [&](BWAPI::Unit medic, BWAPI::Unit target)
    {
        return medic != target && patient(target) &&
            medic->getDistance(target) <= (regrouping ? 64 : 6 * 32) &&
            the.partitions.connected(medic->getPosition(), target->getPosition());
    };

    // Preserve a cast while it travels through command latency, and preserve
    // ongoing heals. The engine's isBeingHealed flag appears after the order.
    BWAPI::Unitset continuing;
    for (BWAPI::Unit medic : availableMedics)
    {
        BWAPI::Unit target = medic->getOrderTarget();
        const bool healingOrder = medic->getOrder() == BWAPI::Orders::MedicHeal ||
            medic->getOrder() == BWAPI::Orders::HealMove;
        const auto assigned = _healAssignments.find(medic->getID());
        const bool pending = assigned != _healAssignments.end() &&
            now - assigned->second.frame <=
                std::max(12, BWAPI::Broodwar->getLatencyFrames() + 6);
        if (pending)
            target = BWAPI::Broodwar->getUnit(assigned->second.patientId);
        if ((healingOrder || pending) && canReachPatient(medic, target))
        {
            const auto claimed = _claimedPatients.find(target->getID());
            if (claimed != _claimedPatients.end() &&
                claimed->second.first != medic->getID()) continue;
            BWAPI::Position waypoint;
            if (!safeHealApproach(medic, target, waypoint))
            {
                if (claimed != _claimedPatients.end()) _claimedPatients.erase(claimed);
                _healAssignments.erase(medic->getID());
                the.micro.HoldPosition(medic);
                continue;
            }
            if (medic->getDistance(target) > 64)
            {
                the.micro.Move(medic, waypoint);
                _healAssignments[medic->getID()] = { target->getID(), now, true };
            }
            else if (assigned != _healAssignments.end() && assigned->second.approaching)
            {
                if (!the.micro.UseTech(medic, BWAPI::TechTypes::Healing, target)) continue;
                _healAssignments[medic->getID()] = { target->getID(), now, false };
            }
            _claimedPatients[target->getID()] = { medic->getID(), now };
            busyMedics.insert(medic);
            continuing.insert(medic);
        }
    }
    for (BWAPI::Unit medic : continuing) availableMedics.erase(medic);

    // Keep Marines under fire first, but let a badly wounded Medic receive
    // care before routine infantry top-ups. Otherwise repeated Stim damage
    // can consume every healer's attention indefinitely.
    std::sort(wounded.begin(), wounded.end(),
        [](BWAPI::Unit a, BWAPI::Unit b)
        {
            if (a->isUnderAttack() != b->isUnderAttack())
                return a->isUnderAttack();
            const int aFraction = 100 * a->getHitPoints() /
                std::max(1, a->getType().maxHitPoints());
            const int bFraction = 100 * b->getHitPoints() /
                std::max(1, b->getType().maxHitPoints());
            const int aUrgency = 100 - aFraction +
                (a->getType() == BWAPI::UnitTypes::Terran_Medic &&
                 aFraction <= 75 ? 25 : 0);
            const int bUrgency = 100 - bFraction +
                (b->getType() == BWAPI::UnitTypes::Terran_Medic &&
                 bFraction <= 75 ? 25 : 0);
            return aUrgency != bUrgency
                ? aUrgency > bUrgency
                : a->getID() < b->getID();
        });

    for (BWAPI::Unit target : wounded)
    {
        if (_claimedPatients.find(target->getID()) != _claimedPatients.end()) continue;
        if (target->isBeingHealed())
        {
            // Keep the current healer assigned, so the fallback movement
            // below does not cancel an ongoing heal every frame.
            for (auto it = availableMedics.begin(); it != availableMedics.end(); ++it)
            {
                if ((*it)->getOrderTarget() == target && (*it)->getDistance(target) <= 64)
                {
                    busyMedics.insert(*it);
                    availableMedics.erase(it);
                    break;
                }
            }
            continue;
        }

        BWAPI::Unit closest = nullptr;
        BWAPI::Position closestWaypoint = BWAPI::Positions::Invalid;
        int closestDistance = 6 * 32 + 1;
        for (BWAPI::Unit medic : availableMedics)
        {
            if (!canReachPatient(medic, target)) continue;
            const int distance = medic->getDistance(target);
            BWAPI::Position waypoint;
            if (distance < closestDistance && safeHealApproach(medic, target, waypoint))
            {
                closest = medic;
                closestWaypoint = waypoint;
                closestDistance = distance;
            }
        }
        if (closest)
        {
            const bool approaching = closestDistance > 64;
            if (approaching) the.micro.Move(closest, closestWaypoint);
            if (approaching || the.micro.UseTech(closest, BWAPI::TechTypes::Healing, target))
            {
                _healAssignments[closest->getID()] = { target->getID(), now, approaching };
                _claimedPatients[target->getID()] = { closest->getID(), now };
                busyMedics.insert(closest);
                availableMedics.erase(closest);
            }
        }
    }

    BWAPI::Unitset safeInfantry;
    for (BWAPI::Unit unit : squadUnits)
    {
        if (unit && unit->exists() &&
            !TerranSiegeSafety::inBlastZone(unit, coveredBuildings))
            safeInfantry.insert(unit);
    }

    for (BWAPI::Unit medic : activeMedics)
    {
        if (busyMedics.find(medic) != busyMedics.end()) continue;

        // Idle healers join the established front, not whichever reinforcement
        // is nearest. Healing above still owns priority over formation travel.
        const BWAPI::Position front =
            attacking && cluster.status != ClusterStatus::Regroup ? _infantryFront.get(
            safeInfantry, medic->getPosition(), *order)
            : BWAPI::Positions::Invalid;
        BWAPI::Unit infantry = nullptr;
        int nearest = INT_MAX;
        if (!front.isValid()) for (BWAPI::Unit candidate : safeInfantry)
        {
            if (!candidate || !candidate->exists() ||
                !candidate->isCompleted() || candidate->isLoaded() ||
                !candidate->getPosition().isValid() ||
                (candidate->getType() != BWAPI::UnitTypes::Terran_Marine &&
                 candidate->getType() != BWAPI::UnitTypes::Terran_Firebat &&
                 candidate->getType() != BWAPI::UnitTypes::Terran_Ghost) ||
                TerranSiegeSafety::inBlastZone(candidate, coveredBuildings) ||
                !the.partitions.connected(
                    medic->getPosition(), candidate->getPosition()))
            {
                continue;
            }
            // In defense, or with too few infantry to establish a front, keep
            // the ordinary local escort. Once a front exists, rear units are
            // not candidates for this fallback.
            const int distance = medic->getDistance(candidate);
            if (distance < nearest)
            {
                nearest = distance;
                infantry = candidate;
            }
        }

        BWAPI::Position goal = regrouping && regroupPosition.isValid() ? regroupPosition
            : front.isValid() ? front : infantry
            ? infantry->getPosition()
            : the.bases.myMain()
            ? the.bases.myMain()->getCenter()
            : BWAPI::Positions::Invalid;
        if (front.isValid() && order)
        {
            const BWAPI::Position enemyGoal = order->getPosition();
            const double dx = double(front.x - enemyGoal.x);
            const double dy = double(front.y - enemyGoal.y);
            const double length = std::hypot(dx, dy);
            if (length > 1.0)
            {
                const BWAPI::Position behind(int(front.x + dx * 48 / length),
                    int(front.y + dy * 48 / length));
                if (behind.isValid() &&
                    the.map.isRuntimeWalkable(BWAPI::TilePosition(behind)) &&
                    the.partitions.connected(medic->getPosition(), behind)) goal = behind;
            }
        }
        if (goal.isValid() && medic->getDistance(goal) > 32)
        {
            the.micro.MoveSafely(medic, goal);
        }
        else if (goal.isValid()) the.micro.HoldPosition(medic);
    }
}

int MicroMedics::getTotalEnergy()
{
    int energy = 0;
    for (BWAPI::Unit unit : getUnits())
    {
        if (unit && unit->exists() && unit->isCompleted() && !unit->isLoaded() &&
            unit->getPosition().isValid() && !unit->isStasised() &&
            !unit->isMaelstrommed() && !unit->isLockedDown())
            energy += unit->getEnergy();
    }
    return energy;
}
