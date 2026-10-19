#pragma once

#include "Core/Common.h"
#include "Core/The.h"
#include "Map/Base.h"
#include "Combat/Squads/SquadOrder.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace UAlbertaBot
{
namespace TerranFormation
{
    struct TankCore
    {
        BWAPI::Position center = BWAPI::Positions::Invalid;
        int count = 0;
        int total = 0;
    };

    // Choose the densest group of Tanks in this squad and ground partition.
    // A nearby stray Tank must not become the anchor for all Goliaths.
    inline TankCore dominantTankCore(
        const BWAPI::Unitset& units,
        const BWAPI::Position& reference,
        int partition)
    {
        struct Candidate
        {
            BWAPI::Position center;
            int count;
        };
        struct Scan
        {
            int total = 0;
            std::vector<Candidate> candidates;
        };

        // Several clusters in one squad ask for the same Tank distribution.
        // Keep the quadratic proximity scan once per frame/partition; only the
        // cheap tie-break against each cluster's position is repeated.
        static int cachedFrame = -1;
        static std::map<std::pair<const BWAPI::Unitset*, int>, Scan> scans;
        const int now = the.now();
        if (cachedFrame != now)
        {
            scans.clear();
            cachedFrame = now;
        }

        const auto key = std::make_pair(&units, partition);
        auto found = scans.find(key);
        if (found == scans.end())
        {
            Scan scan;
            std::vector<BWAPI::Unit> tanks;
            for (BWAPI::Unit unit : units)
            {
                if (!unit || !unit->exists() || !unit->getPosition().isValid() ||
                    (unit->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
                     unit->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) ||
                    (partition > 0 && the.partitions.id(unit->getPosition()) != partition))
                {
                    continue;
                }
                tanks.push_back(unit);
            }
            scan.total = int(tanks.size());
            if (scan.total >= 3)
            {
                for (BWAPI::Unit seed : tanks)
                {
                    long long sumX = 0;
                    long long sumY = 0;
                    int nearby = 0;
                    for (BWAPI::Unit tank : tanks)
                    {
                        if (tank->getDistance(seed) <= 9 * 32)
                        {
                            sumX += tank->getPosition().x;
                            sumY += tank->getPosition().y;
                            ++nearby;
                        }
                    }
                    if (nearby >= 3)
                    {
                        scan.candidates.push_back({
                            BWAPI::Position(int(sumX / nearby), int(sumY / nearby)),
                            nearby });
                    }
                }
            }
            found = scans.emplace(key, std::move(scan)).first;
        }

        TankCore result;
        result.total = found->second.total;
        int bestDistance = MAX_DISTANCE;
        for (const Candidate& candidate : found->second.candidates)
        {
            const int distance = reference.isValid()
                ? candidate.center.getApproxDistance(reference) : 0;
            if (candidate.count > result.count ||
                (candidate.count == result.count && distance < bestDistance))
            {
                result.count = candidate.count;
                result.center = candidate.center;
                bestDistance = distance;
            }
        }
        return result;
    }

    inline bool isBaseDefenseOrder(const SquadOrder & order)
    {
        return order.getBase() && order.getBase()->getOwner() == the.self() &&
            (order.getStatus() == "Defend front" || order.getStatus() == "Defend attacked base");
    }

    inline bool defensiveVultureSlotIsOpen(
        BWAPI::Unit vulture,
        const BWAPI::Position& slot,
        const BWAPI::Position& baseCenter)
    {
        if (!vulture ||
            !vulture->exists() ||
            !slot.isValid() ||
            !baseCenter.isValid())
        {
            return false;
        }

        const BWAPI::UnitType body = BWAPI::UnitTypes::Terran_Vulture;
        const int left = slot.x - body.dimensionLeft(), right = slot.x + body.dimensionRight();
        const int top = slot.y - body.dimensionUp(), bottom = slot.y + body.dimensionDown();
        if (left < 0 || top < 0 || right >= BWAPI::Broodwar->mapWidth() * 32 ||
            bottom >= BWAPI::Broodwar->mapHeight() * 32 ||
            !the.partitions.connected(vulture->getPosition(), slot)) return false;
        for (int x = left / 8; x <= right / 8; ++x)
            for (int y = top / 8; y <= bottom / 8; ++y)
                if (!BWAPI::Broodwar->isWalkable(x, y)) return false;
        for (int x = left / 32; x <= right / 32; ++x)
            for (int y = top / 32; y <= bottom / 32; ++y)
                if (the.placer.isReserved(BWAPI::TilePosition(x, y))) return false;
        for (BWAPI::Unit blocker : BWAPI::Broodwar->getUnitsInRadius(slot, 96))
        {
            if (!blocker || blocker == vulture || !blocker->exists() || blocker->isFlying()) continue;
            const BWAPI::UnitType type = blocker->getType();
            const bool tank = type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
            if (type == BWAPI::UnitTypes::Terran_Vulture &&
                (blocker->getVelocityX() != 0.0 ||
                 blocker->getVelocityY() != 0.0)) continue;
            // A parked Vulture occupies a real slot too. Ignoring it made
            // several units choose the same fallback point behind the Tanks.
            if (!tank && type != BWAPI::UnitTypes::Terran_Vulture &&
                !type.isBuilding() && !type.isResourceContainer() &&
                type.canMove()) continue;
            const BWAPI::Position pos = blocker->getPosition();
            const int gap = tank ? 24 :
                type == BWAPI::UnitTypes::Terran_Vulture ? 12 : 4;
            if (right + gap >= pos.x - type.dimensionLeft() && left - gap <= pos.x + type.dimensionRight() &&
                bottom + gap >= pos.y - type.dimensionUp() && top - gap <= pos.y + type.dimensionDown()) return false;
        }

        return true;
    }

    // The normal defensive order points at the resource depot. Slow units have
    // their own defensive-front handling, but Vultures otherwise drive directly
    // into the base and can block production and construction. Give each ordinary
    // Ground-squad Vulture a stable slot in a shallow arc toward the base front.
    inline BWAPI::Position defensiveVultureDestination(
        BWAPI::Unit vulture,
        int index,
        int total,
        Base* base,
        const BWAPI::Position& fallback)
    {
        if (!vulture ||
            !vulture->exists() ||
            index < 0 ||
            total <= 0 ||
            !base)
        {
            return fallback;
        }

        const BWAPI::Position baseCenter = base->getCenter();
        BWAPI::Position front = base->getFront();

        if (!baseCenter.isValid())
        {
            return fallback;
        }

        if (!front.isValid() || front == BWAPI::Positions::Unknown)
        {
            front = fallback;
        }

        double forwardX = double(front.x - baseCenter.x);
        double forwardY = double(front.y - baseCenter.y);
        double length = std::sqrt(forwardX * forwardX + forwardY * forwardY);

        if (length < 1.0)
        {
            const BWAPI::Position mineralOffset = base->getMineralOffset();
            forwardX = double(-mineralOffset.x);
            forwardY = double(-mineralOffset.y);
            length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        }

        if (length < 1.0)
        {
            return fallback;
        }

        forwardX /= length;
        forwardY /= length;

        const int slotsPerRow = 5;
        const int row = index / slotsPerRow;
        const int column = index % slotsPerRow;
        const int unitsInRow = std::min(slotsPerRow, total - row * slotsPerRow);
        const double centeredColumn =
            double(column) - double(unitsInRow - 1) / 2.0;
        // Use the real entrance, not a fixed five-tile radius around the CC.
        // Tanks normally deploy behind it; the Vulture screen belongs ahead.
        double baseRange = std::max(5.0 * 32, length + 32.0);
        const bool closedGate = !the.placer.isTerranMainWallReleased() &&
            the.placer.isTerranMainWallComplete(true) &&
            the.placer.isInsideTerranMainWall(baseCenter);
        if (closedGate)
        {
            front = the.placer.getTerranMainWallGateCenter();
            forwardX = double(front.x - baseCenter.x);
            forwardY = double(front.y - baseCenter.y);
            length = std::hypot(forwardX, forwardY);
            if (length < 1.0) return fallback;
            forwardX /= length; forwardY /= length;
            baseRange = std::max(3.0 * 32, length - 96.0);
        }
        const int rowSpacing = 56;
        const int sideSpacing = 72;

        const double sideX = -forwardY, sideY = forwardX;
        BWAPI::Position desired(
            int(baseCenter.x +
                forwardX * (baseRange - row * rowSpacing) +
                sideX * centeredColumn * sideSpacing),
            int(baseCenter.y +
                forwardY * (baseRange - row * rowSpacing) +
                sideY * centeredColumn * sideSpacing));

        if (desired.isValid() &&
            (!closedGate || the.placer.isInsideTerranMainWall(desired, -48)) &&
            defensiveVultureSlotIsOpen(vulture, desired, baseCenter))
        {
            return desired;
        }

        // A planned building may reserve the nominal slot. Search nearby along
        // the same front-facing arc instead of falling back into the base center.
        BWAPI::Position best = BWAPI::Positions::Invalid;
        int bestDistance = MAX_DISTANCE;

        for (int forwardStep = -2; forwardStep <= 3; ++forwardStep)
        {
            for (int sideStep = -3; sideStep <= 3; ++sideStep)
            {
                BWAPI::Position candidate(
                    int(desired.x + forwardX * forwardStep * 32 + sideX * sideStep * 32),
                    int(desired.y + forwardY * forwardStep * 32 + sideY * sideStep * 32));

                if (!candidate.isValid() ||
                    (closedGate && !the.placer.isInsideTerranMainWall(candidate, -48)) ||
                    !defensiveVultureSlotIsOpen(vulture, candidate, baseCenter))
                {
                    continue;
                }

                const int distance = candidate.getApproxDistance(desired);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = candidate;
                }
            }
        }

        if (!best.isValid())
        {
            // A mass of Tanks can occupy the whole shallow arc. Try wider
            // shoulders of the same front before giving up on parking.
            const int sideOrder = index % 2 == 0 ? 1 : -1;
            for (int lateral = 4; lateral <= 8 && !best.isValid(); lateral += 2)
            {
                for (int turn = 0; turn < 2 && !best.isValid(); ++turn)
                {
                    const int sideSign = turn == 0 ? sideOrder : -sideOrder;
                    for (int back = 0; back <= 4; ++back)
                    {
                        const BWAPI::Position candidate(
                            int(desired.x + sideX * sideSign * lateral * 32 -
                                forwardX * back * 32),
                            int(desired.y + sideY * sideSign * lateral * 32 -
                                forwardY * back * 32));
                        if (candidate.isValid() &&
                            (!closedGate ||
                             the.placer.isInsideTerranMainWall(candidate, -48)) &&
                            defensiveVultureSlotIsOpen(vulture, candidate, baseCenter))
                        {
                            best = candidate;
                            break;
                        }
                    }
                }
            }
        }

        // The base-center fallback sends every overflow Vulture into the same
        // Tank/production bottleneck. If the front really has no free slot,
        // keep this unit out of the traffic instead of issuing that order.
        return best.isValid() ? best : vulture->getPosition();
    }

    // Stable Vulture rows relative to a mobile army core. Positive offsets are
    // a forward screen; negative offsets are rear support. Squad movement and
    // MicroVultures share this geometry so they do not replace each other's
    // destination with two different formations on alternating frames.
    inline BWAPI::Position vultureLineDestination(
        BWAPI::Unit vulture,
        int index,
        int total,
        const BWAPI::Position& anchor,
        const BWAPI::Position& objective,
        int forwardOffset,
        const BWAPI::Position& fallback)
    {
        if (!vulture || !vulture->exists() || index < 0 || total <= 0 ||
            !anchor.isValid() || !objective.isValid())
        {
            return fallback;
        }

        double fx = double(objective.x - anchor.x);
        double fy = double(objective.y - anchor.y);
        const double length = std::sqrt(fx * fx + fy * fy);
        if (length < 1.0)
        {
            return fallback;
        }
        fx /= length;
        fy /= length;
        const double sx = -fy;
        const double sy = fx;

        const int columns = std::min(
            5,
            std::max(1, int(std::ceil(std::sqrt(double(total))))));
        const int row = index / columns;
        const int column = index % columns;
        const int count = std::min(columns, total - row * columns);
        const double centered = double(column) - double(count - 1) / 2.0;
        const int rowSpacing = 56;
        const int sideSpacing = 68;

        const BWAPI::Position desired(
            int(anchor.x + fx * (forwardOffset - row * rowSpacing) +
                sx * centered * sideSpacing),
            int(anchor.y + fy * (forwardOffset - row * rowSpacing) +
                sy * centered * sideSpacing));

        for (int radius = 0; radius <= 3; ++radius)
        {
            for (int sideStep = -radius; sideStep <= radius; ++sideStep)
            {
                if (radius && std::abs(sideStep) != radius)
                {
                    continue;
                }

                for (int back = 0; back <= radius; ++back)
                {
                    const BWAPI::Position candidate(
                        int(desired.x + sx * sideStep * 24 - fx * back * 24),
                        int(desired.y + sy * sideStep * 24 - fy * back * 24));

                    if (defensiveVultureSlotIsOpen(
                        vulture, candidate, anchor))
                    {
                        return candidate;
                    }
                }
            }
        }

        return fallback;
    }

    // Formation positions are individual destinations, so the squad's distance
    // field (rooted at its order/base) cannot route to them. Extract a short
    // ground waypoint instead of driving straight across walls or cliffs.
    inline void moveVultureToFormationSlot(
        BWAPI::Unit vulture,
        const BWAPI::Position& slot,
        int arrivalRadius = 16)
    {
        if (!vulture || !vulture->exists() || !slot.isValid()) return;
        if (vulture->getPosition().getApproxDistance(slot) <= arrivalRadius)
        {
            the.micro.HoldPosition(vulture);
            return;
        }

        BWAPI::Position destination = slot;
        if (vulture->getDistance(slot) > 4 * 32)
        {
            const BWAPI::Position waypoint = the.map.getGroundPathWaypoint(
                vulture->getPosition(), slot, 6, true);
            if (waypoint.isValid() &&
                vulture->getPosition().getApproxDistance(waypoint) > 16)
            {
                destination = waypoint;
            }
        }
        the.micro.MoveNear(vulture, destination);
    }

    constexpr int GoliathSpacing = 64;
    constexpr int MixedRearGap = 5 * 32;
    constexpr int HeavyMixedRearGap = 2 * 32;
    constexpr int GroundRearGap = 4 * 32;

    inline BWAPI::Position tankRearAnchor(
        const BWAPI::Position& center,
        const BWAPI::Position& objective,
        int gap)
    {
        if (!center.isValid() || !objective.isValid()) return BWAPI::Positions::Invalid;
        double fx = double(objective.x - center.x), fy = double(objective.y - center.y);
        const double length = std::sqrt(fx * fx + fy * fy);
        if (length < 1.0) return BWAPI::Positions::Invalid;
        fx /= length;
        fy /= length;
        double rear = 0.0;
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (!unit || !unit->exists() || !unit->getPosition().isValid() ||
                unit->getDistance(center) > 10 * 32) continue;
            const BWAPI::UnitType type = unit->getType();
            if (type != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
                type != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) continue;
            const BWAPI::Position p = unit->getPosition();
            rear = std::min(rear, (p.x - center.x) * fx + (p.y - center.y) * fy);
        }
        return BWAPI::Position(int(center.x + fx * (rear - gap)),
            int(center.y + fy * (rear - gap)));
    }

    inline bool goliathSlotClear(BWAPI::Unit unit, const BWAPI::Position& slot)
    {
        if (!slot.isValid()) return false;
        const BWAPI::UnitType body = BWAPI::UnitTypes::Terran_Goliath;
        const int left = slot.x - body.dimensionLeft(), right = slot.x + body.dimensionRight();
        const int top = slot.y - body.dimensionUp(), bottom = slot.y + body.dimensionDown();
        if (left < 0 || top < 0 || right >= BWAPI::Broodwar->mapWidth() * 32 ||
            bottom >= BWAPI::Broodwar->mapHeight() * 32) return false;
        for (int x = left / 8; x <= right / 8; ++x)
            for (int y = top / 8; y <= bottom / 8; ++y)
                if (!BWAPI::Broodwar->isWalkable(x, y)) return false;
        for (BWAPI::Unit obstacle : BWAPI::Broodwar->getUnitsInRadius(slot, 96))
        {
            if (!obstacle || !obstacle->exists() || obstacle == unit || obstacle->isFlying()) continue;
            const BWAPI::UnitType type = obstacle->getType();
            const bool tank = type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
            // A parked Goliath is a real obstacle for the next member of a
            // large row. The ID tie-break keeps slot selection deterministic.
            const bool parkedGoliath =
                type == BWAPI::UnitTypes::Terran_Goliath &&
                obstacle->getPlayer() == unit->getPlayer() &&
                obstacle->getID() < unit->getID() &&
                !obstacle->isMoving();
            if (!tank && !parkedGoliath &&
                !type.isBuilding() && !type.isResourceContainer()) continue;
            const BWAPI::Position p = obstacle->getPosition();
            const int gap = tank ? 24 : parkedGoliath ? 8 : 4;
            if (right + gap >= p.x - type.dimensionLeft() && left - gap <= p.x + type.dimensionRight() &&
                bottom + gap >= p.y - type.dimensionUp() && top - gap <= p.y + type.dimensionDown())
                return false;
        }
        return true;
    }

    // Shared by Squad movement and combat micro so both use the same row
    // spacing and obstacle policy instead of repeatedly replacing each other.
    inline BWAPI::Position goliathSlot(
        BWAPI::Unit unit, int index, int total,
        const BWAPI::Position& anchor, const BWAPI::Position& objective)
    {
        if (!unit || !anchor.isValid() || !objective.isValid() || total <= 0)
            return BWAPI::Positions::Invalid;
        double fx = double(objective.x - anchor.x), fy = double(objective.y - anchor.y);
        const double length = std::sqrt(fx * fx + fy * fy);
        if (length < 1.0) return BWAPI::Positions::Invalid;
        fx /= length;
        fy /= length;
        const int columns = std::min(5, std::max(1, int(std::ceil(std::sqrt(double(total))))));
        const int row = index / columns, column = index % columns;
        const int count = std::min(columns, total - row * columns);
        const double side = (column - (count - 1) / 2.0) * GoliathSpacing;
        const BWAPI::Position desired(int(anchor.x - fy * side - fx * row * GoliathSpacing),
            int(anchor.y + fx * side - fy * row * GoliathSpacing));
        for (int radius = 0; radius <= 3; ++radius)
        {
            for (int sideStep = -radius; sideStep <= radius; ++sideStep)
            {
                if (radius && std::abs(sideStep) != radius) continue;
                // Search sideways/backward, never forward through the Tank line.
                for (int back = 0; back <= radius; ++back)
                {
                    const BWAPI::Position p(int(desired.x - fy * sideStep * 24 - fx * back * 24),
                        int(desired.y + fx * sideStep * 24 - fy * back * 24));
                    if (goliathSlotClear(unit, p) && the.partitions.connected(unit->getPosition(), p))
                        return p;
                }
            }
        }
        // A narrow passage may have no room for this layout. Let ordinary
        // routed movement handle it rather than pinning a member forever.
        return BWAPI::Positions::Invalid;
    }
}
}
