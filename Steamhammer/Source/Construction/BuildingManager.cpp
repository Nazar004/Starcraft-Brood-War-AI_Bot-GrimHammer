#include "Construction/BuildingManager.h"

#include "Map/Bases.h"
#include "Construction/BuildingPlacer.h"
#include "Combat/CombatCommander.h"
#include "Combat/Micro/Terran/ExpansionVultureReservation.h"
#include "Production/ProductionManager.h"
#include "Intelligence/ScoutManager.h"
#include "Strategy/StrategyManager.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

using namespace UAlbertaBot;

namespace
{
    struct ExpansionBuilderRouteState
    {
        int lastDistance = 0;
        int lastCheckFrame = 0;
        int stuckFrames = 0;
        int lastMoveFrame = 0;
        int waypointIndex = 0;
    };

    std::map<int, ExpansionBuilderRouteState> expansionBuilderRouteStates;
    std::map<int, BWAPI::TilePosition> fallbackCommandCenters;
    std::map<int, int> fallbackLandingFirstAttemptFrame;
    std::set<int> airRelocatedCommandCenterIDs;
    int fallbackCommandCenterLastFrame = -1;

    BWAPI::Position fallbackEnemySide(Base* main)
    {
        return the.bases.enemyStart()
            ? the.bases.enemyStart()->getPosition()
            : the.bases.myNatural()
            ? the.bases.myNatural()->getPosition()
            : main->getFront();
    }

    double fallbackEnemyAlignment(
        const BWAPI::Position& position,
        const BWAPI::Position& mainPosition,
        const BWAPI::Position& enemySide)
    {
        const double dx = double(position.x - mainPosition.x);
        const double dy = double(position.y - mainPosition.y);
        const double ex = double(enemySide.x - mainPosition.x);
        const double ey = double(enemySide.y - mainPosition.y);
        const double lengths = std::sqrt((dx * dx + dy * dy) *
            (ex * ex + ey * ey));
        return lengths > 0.0 ? (dx * ex + dy * ey) / lengths : 1.0;
    }

    int fallbackDirectionTier(double alignment)
    {
        if (alignment <= -0.5) return 0; // clearly behind our main
        if (alignment < 0.0) return 1;
        if (alignment <= 0.5) return 2; // lateral, if no rear base exists
        return 3;                       // toward the enemy: last resort
    }

    bool fallbackLandingThreatened(Base* base)
    {
        const BWAPI::Position position = base->getPosition();
        for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
            position, 10 * 32, BWAPI::Filter::IsEnemy))
        {
            if (enemy && enemy->exists() && enemy->isVisible() &&
                (enemy->getType().isBuilding() ||
                 UnitUtil::CanAttackGround(enemy) ||
                 UnitUtil::CanAttackAir(enemy)))
            {
                return true;
            }
        }

        // A blocking army may have moved into fog before the CC arrives.
        // Keep its recently observed position unsafe until it is disproved.
        const int now = the.now();
        for (const auto& entry : the.info.getUnitData(the.enemy()).getUnits())
        {
            const UnitInfo& enemy = entry.second;
            if (enemy.goneFromLastPosition || !enemy.lastPosition.isValid() ||
                enemy.lastPosition.getApproxDistance(position) > 10 * 32 ||
                (!enemy.type.isBuilding() &&
                 (enemy.updateFrame > now || now - enemy.updateFrame > 20 * 24)))
            {
                continue;
            }
            if (enemy.type.isBuilding() ||
                UnitUtil::TypeCanAttackGround(enemy.type) ||
                UnitUtil::TypeCanAttackAir(enemy.type))
            {
                return true;
            }
        }
        return false;
    }

    bool secondCommandCenterBlockedOnGround()
    {
        if (the.selfRace() != BWAPI::Races::Terran ||
            the.my.all.count(BWAPI::UnitTypes::Terran_Command_Center) != 1 ||
            !the.bases.myMain())
        {
            return false;
        }

        const BWAPI::TilePosition expansion =
            the.map.getNextExpansion(false, true, false);
        if (!expansion.isValid() ||
            the.bases.myMain()->getSafeTileDistance(expansion) >= 0)
        {
            return false;
        }

        const BWAPI::Position approach = the.bases.myNatural()
            ? the.bases.myNatural()->getPosition()
            : the.bases.myMain()->getFront();
        if (!approach.isValid())
        {
            return false;
        }

        for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
            approach, 12 * 32, BWAPI::Filter::IsEnemy))
        {
            if (enemy && enemy->exists() && enemy->isVisible() &&
                UnitUtil::CanAttackGround(enemy))
            {
                return true;
            }
        }
        return false;
    }

    BWAPI::TilePosition chooseFallbackCommandCenterDestination()
    {
        Base* main = the.bases.myMain();
        if (!main)
        {
            return BWAPI::TilePositions::Invalid;
        }

        const BWAPI::Position mainPos = main->getPosition();
        const BWAPI::Position enemySide = fallbackEnemySide(main);

        Base* best = nullptr;
        int bestScore = MAX_DISTANCE;
        int bestDirectionTier = 4;
        bool bestIsNatural = true;
        for (Base* base : the.bases.getAll())
        {
            if (!base || base->getOwner() != BWAPI::Broodwar->neutral() ||
                base == the.bases.enemyStart() ||
                base->isIsland() || !base->getTilePosition().isValid() ||
                base->getLastKnownMinerals() <= 0)
            {
                continue;
            }

            if (fallbackLandingThreatened(base))
            {
                continue;
            }
            if (!the.placer.preservesGroundExit(
                    BWAPI::UnitTypes::Terran_Command_Center,
                    base->getTilePosition()))
            {
                continue;
            }

            const double alignment = fallbackEnemyAlignment(
                base->getPosition(), mainPos, enemySide);
            const int directionTier = fallbackDirectionTier(alignment);
            const bool isNatural = base == the.bases.myNatural();
            const int score = mainPos.getApproxDistance(base->getPosition());
            // The fallback exists because the normal natural route is blocked.
            // Keep that natural only as a last resort even if no enemy is
            // currently visible on its landing tile.
            if (!best || (!isNatural && bestIsNatural) ||
                (isNatural == bestIsNatural &&
                 (directionTier < bestDirectionTier ||
                  (directionTier == bestDirectionTier && score < bestScore))))
            {
                best = base;
                bestScore = score;
                bestDirectionTier = directionTier;
                bestIsNatural = isNatural;
            }
        }
        return best ? best->getTilePosition() : BWAPI::TilePositions::Invalid;
    }

    void moveFallbackCommandCenters()
    {
        const int now = the.now();
        if (now < fallbackCommandCenterLastFrame)
        {
            fallbackCommandCenters.clear();
            fallbackLandingFirstAttemptFrame.clear();
            airRelocatedCommandCenterIDs.clear();
        }
        fallbackCommandCenterLastFrame = now;

        if (fallbackCommandCenters.empty() || now % 12 != 0)
        {
            return;
        }

        for (auto it = fallbackCommandCenters.begin();
             it != fallbackCommandCenters.end();)
        {
            BWAPI::Unit center = nullptr;
            for (BWAPI::Unit unit : the.self()->getUnits())
            {
                if (unit && unit->exists() && unit->getID() == it->first)
                {
                    center = unit;
                    break;
                }
            }
            if (!center || center->getType() != BWAPI::UnitTypes::Terran_Command_Center ||
                (!center->isLifted() &&
                 center->getTilePosition() == it->second))
            {
                fallbackLandingFirstAttemptFrame.erase(it->first);
                it = fallbackCommandCenters.erase(it);
                continue;
            }

            if (!center->isCompleted())
            {
                ++it;
                continue;
            }

            if (!it->second.isValid())
            {
                fallbackLandingFirstAttemptFrame.erase(it->first);
                it->second = chooseFallbackCommandCenterDestination();
            }

            // If the enemy main was learned after takeoff, correct an old
            // natural-facing order before the CC reaches its landing site.
            if (it->second.isValid() && the.bases.enemyStart() &&
                center->getDistance(BWAPI::Position(it->second)) > 8 * 32)
            {
                const BWAPI::TilePosition preferred =
                    chooseFallbackCommandCenterDestination();
                if (preferred.isValid() && preferred != it->second)
                {
                    const BWAPI::Position mainPosition =
                        the.bases.myMain()->getPosition();
                    const BWAPI::Position enemySide =
                        fallbackEnemySide(the.bases.myMain());
                    const double currentAlignment = fallbackEnemyAlignment(
                        BWAPI::Position(it->second), mainPosition, enemySide);
                    const double preferredAlignment = fallbackEnemyAlignment(
                        BWAPI::Position(preferred), mainPosition, enemySide);
                    if ((the.bases.myNatural() &&
                         it->second == the.bases.myNatural()->getTilePosition() &&
                         preferred != it->second) ||
                        fallbackDirectionTier(preferredAlignment) <
                            fallbackDirectionTier(currentAlignment) ||
                        preferredAlignment < currentAlignment - 0.1)
                    {
                        fallbackLandingFirstAttemptFrame.erase(it->first);
                        it->second = preferred;
                    }
                }
            }
            if (!it->second.isValid())
            {
                ++it;
                continue;
            }

            Base* destination = the.bases.getBaseAtTilePosition(it->second);
            if (!destination || destination->getOwner() != BWAPI::Broodwar->neutral())
            {
                fallbackLandingFirstAttemptFrame.erase(it->first);
                it->second = BWAPI::TilePositions::Invalid;
                ++it;
                continue;
            }

            if (!the.placer.preservesGroundExit(center->getType(), it->second))
            {
                fallbackLandingFirstAttemptFrame.erase(it->first);
                it->second = BWAPI::TilePositions::Invalid;
                ++it;
                continue;
            }

            const bool nearLanding =
                center->getDistance(destination->getPosition()) <= 5 * 32;
            // A burrowed unit can make canLand() false without appearing in
            // the visible threat check. Keep the chosen base and clear its
            // exact footprint instead of flying between fallback bases.
            if (center->isLifted() && nearLanding && !center->canLand(it->second))
            {
                CombatCommander::Instance().requestExpansionLandingClearance(it->second);
                ++it;
                continue;
            }
            if (fallbackLandingThreatened(destination))
            {
                fallbackLandingFirstAttemptFrame.erase(it->first);
                it->second = BWAPI::TilePositions::Invalid;
                ++it;
                continue;
            }
            if (!center->isLifted())
            {
                center->lift();
            }
            else if (!nearLanding)
            {
                fallbackLandingFirstAttemptFrame.erase(it->first);
                if (now - center->getLastCommandFrame() >= 24)
                {
                    center->move(destination->getPosition());
                }
            }
            else
            {
                const auto attempt = fallbackLandingFirstAttemptFrame.emplace(
                    it->first, now);
                // canLand() may accept an unseen blocker and the command may
                // still fail. A CC that remains airborne after its first
                // landing order needs the same scan/clearance path.
                if (now - attempt.first->second >= 3 * 24)
                    CombatCommander::Instance().requestExpansionLandingClearance(it->second);
                if (now - center->getLastCommandFrame() >= 24 &&
                    !center->land(it->second))
                    CombatCommander::Instance().requestExpansionLandingClearance(it->second);
            }
            ++it;
        }
    }

    struct BlockedExpansionClearState
    {
        int firstBlockedFrame = -1;
        int lastScanFrame = -100000;
        int lastScanAttemptFrame = -100000;
        int lastCommandFrame = -100000;
        int supportVultureID = -1;
    };

    std::map<int, BlockedExpansionClearState> blockedExpansionClearStates;
    std::set<int> friendlyConstructionBlockerIDs;
    std::vector<BWAPI::Position> friendlyConstructionExits;

    void releaseSupportVulture(BlockedExpansionClearState& state)
    {
        if (state.supportVultureID >= 0)
        {
            ExpansionVultureReservation::release(state.supportVultureID);
            state.supportVultureID = -1;
        }
    }

    void eraseBlockedExpansionClearState(int key)
    {
        auto it = blockedExpansionClearStates.find(key);
        if (it == blockedExpansionClearStates.end())
        {
            return;
        }

        releaseSupportVulture(it->second);
        blockedExpansionClearStates.erase(it);
    }

    enum class BlockedExpansionClearResult
    {
        NotBlocked,
        Handling
    };

    int expansionBlockKey(const BWAPI::TilePosition& tile)
    {
        return tile.x * 10000 + tile.y;
    }

    bool isTerranExpansionCommandCenterTask(const Building& b)
    {
        return
            BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Terran &&
            b.type == BWAPI::UnitTypes::Terran_Command_Center &&
            b.macroLocation != MacroLocation::Main &&
            b.finalPosition.isValid();
    }

    bool unitOverlapsBuildFootprint(
        BWAPI::Unit unit,
        const BWAPI::TilePosition& buildTile,
        BWAPI::UnitType buildingType)
    {
        if (!unit || !unit->exists() || !unit->getPosition().isValid())
        {
            return false;
        }

        const int left = buildTile.x * 32;
        const int top = buildTile.y * 32;
        const int right = (buildTile.x + buildingType.tileWidth()) * 32 - 1;
        const int bottom = (buildTile.y + buildingType.tileHeight()) * 32 - 1;

        return
            unit->getRight() >= left &&
            unit->getLeft() <= right &&
            unit->getBottom() >= top &&
            unit->getTop() <= bottom;
    }

    BWAPI::Position buildingCenterPosition(const Building& b)
    {
        if (!b.finalPosition.isValid())
        {
            return BWAPI::Positions::Invalid;
        }

        return BWAPI::Position(
            b.finalPosition.x * 32 + b.type.tileWidth() * 16,
            b.finalPosition.y * 32 + b.type.tileHeight() * 16
        );
    }

    BWAPI::Position findFriendlyExpansionExit(BWAPI::Unit unit, const Building& b)
    {
        const BWAPI::Position center = buildingCenterPosition(b);
        Base * base = the.bases.getBaseAtTilePosition(b.finalPosition);
        const ChokePoint * front = base ? base->getFrontChoke() : nullptr;
        const BWAPI::Position frontPosition = front && front->center().isValid()
            ? front->center() : center;
        const bool tank = unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
        BWAPI::Position best = BWAPI::Positions::Invalid;
        int bestScore = MAX_DISTANCE;

        // Nearby points on all sides work for every map orientation. Prefer
        // the defended approach, while leaving the CC working ring free.
        for (int radius : { 4, 6, 8 })
        {
            for (int dx = -radius; dx <= radius; dx += 2)
            {
                for (int dy = -radius; dy <= radius; dy += 2)
                {
                    if (std::max(std::abs(dx), std::abs(dy)) != radius)
                    {
                        continue;
                    }
                    const BWAPI::Position candidate(center.x + dx * 32, center.y + dy * 32);
                    const BWAPI::TilePosition tile(candidate);
                    if (!candidate.isValid() || !tile.isValid() ||
                        !BWAPI::Broodwar->isWalkable(candidate.x / 8, candidate.y / 8) ||
                        !the.partitions.connected(unit->getPosition(), candidate) ||
                        (tank && (the.tileRoom.at(tile) <= 14 ||
                                  !UnitUtil::TankPositionHasSafeDepotClearance(candidate))))
                    {
                        continue;
                    }

                    if (std::any_of(friendlyConstructionExits.begin(),
                        friendlyConstructionExits.end(),
                        [&](const BWAPI::Position& reserved)
                        {
                            return candidate.getApproxDistance(reserved) < 64;
                        }))
                    {
                        continue;
                    }

                    bool occupied = false;
                    for (BWAPI::Unit other : BWAPI::Broodwar->getUnitsInRadius(candidate, 40))
                    {
                        if (other && other->exists() && other != unit &&
                            !other->isFlying() && !other->isLoaded() &&
                            (other->getType().isBuilding() || other->isSieged() ||
                             other->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode))
                        {
                            occupied = true;
                            break;
                        }
                    }
                    if (occupied)
                    {
                        continue;
                    }

                    const int score = unit->getDistance(candidate) +
                        candidate.getApproxDistance(frontPosition) / 4;
                    if (score < bestScore)
                    {
                        bestScore = score;
                        best = candidate;
                    }
                }
            }
        }
        return best;
    }

    bool isClearableExpansionBlocker(BWAPI::Unit enemy, const Building& b)
    {
        if (!enemy ||
            !enemy->exists() ||
            !enemy->getPosition().isValid())
        {
            return false;
        }

        const BWAPI::UnitType type = enemy->getType();
        const bool ownMine =
            enemy->getPlayer() == BWAPI::Broodwar->self() &&
            type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine;

        const bool mineral = enemy->getPlayer() == BWAPI::Broodwar->neutral() &&
            type.isMineralField();
        const bool hostile = enemy->getPlayer() == BWAPI::Broodwar->enemy();
        const bool destructibleNeutral = enemy->getPlayer() == BWAPI::Broodwar->neutral() &&
            !enemy->isInvincible() && !type.isResourceContainer();
        // Friendly units move aside; only our own mine is a friendly attack target.
        if (!ownMine && !hostile && !destructibleNeutral && !mineral)
        {
            return false;
        }

        if (type.isSpell() || enemy->isFlying() ||
            (!mineral && enemy->isInvincible()))
        {
            return false;
        }

        if (!unitOverlapsBuildFootprint(enemy, b.finalPosition, b.type))
        {
            return false;
        }

        if (type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine)
        {
            return ownMine || enemy->isDetected();
        }

        if (enemy->isBurrowed() || enemy->isCloaked())
        {
            return enemy->isDetected();
        }

        return enemy->isVisible() && enemy->isDetected();
    }

    BWAPI::Unit findExpansionBlocker(const Building& b)
    {
        BWAPI::Unit bestBlocker = nullptr;
        int bestDistance = MAX_DISTANCE;

        const BWAPI::Position center(
            b.finalPosition.x * 32 + b.type.tileWidth() * 16,
            b.finalPosition.y * 32 + b.type.tileHeight() * 16
        );

        for (BWAPI::Unit enemy : BWAPI::Broodwar->getAllUnits())
        {
            if (!isClearableExpansionBlocker(enemy, b))
            {
                continue;
            }

            const int distance = enemy->getDistance(center);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestBlocker = enemy;
            }
        }

        return bestBlocker;
    }

    // Placement must admit a clearable base slot before assigning its SCV.
    // Otherwise canBuildHere() rejects the mine and clearing is never reached.
    // This exception is restricted to an exact, known expansion footprint.
    bool canClearExpansionFootprint(const Building& b, BWAPI::TilePosition tile)
    {
        Building candidate = b;
        candidate.finalPosition = tile;
        if (!isTerranExpansionCommandCenterTask(candidate))
        {
            return false;
        }

        Base* base = the.bases.getBaseAtTilePosition(tile);
        if (!base || base->getTilePosition() != tile ||
            base->getOwner() != BWAPI::Broodwar->neutral())
        {
            return false;
        }

        for (int x = 0; x < b.type.tileWidth(); ++x)
        {
            for (int y = 0; y < b.type.tileHeight(); ++y)
            {
                const BWAPI::TilePosition cell(tile.x + x, tile.y + y);
                if (!cell.isValid() ||
                    !BWAPI::Broodwar->isBuildable(cell, false) ||
                    BWAPI::Broodwar->hasCreep(cell))
                {
                    return false;
                }
            }
        }

        for (BWAPI::Unit unit : BWAPI::Broodwar->getAllUnits())
        {
            if (!unit || !unit->exists() || unit == b.builderUnit ||
                unit->isFlying() || unit->isLoaded() ||
                !unitOverlapsBuildFootprint(unit, tile, b.type))
            {
                continue;
            }

            const BWAPI::UnitType type = unit->getType();
            const bool ownMobile = unit->getPlayer() == the.self() &&
                !type.isBuilding();
            const bool hostile = unit->getPlayer() == the.enemy() &&
                !unit->isInvincible() && !type.isSpell();
            const bool neutralObstacle = unit->getPlayer() == BWAPI::Broodwar->neutral() &&
                (type.isMineralField() ||
                 (!unit->isInvincible() && !type.isResourceContainer()));
            // Terrain, our own permanent buildings and invincible scenery
            // cannot be cleared by a scan or attack.
            if (!ownMobile && !hostile && !neutralObstacle)
            {
                return false;
            }
        }

        return true;
    }

    BWAPI::Unit getSupportVultureByID(int id)
    {
        if (id < 0)
        {
            return nullptr;
        }

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists())
            {
                continue;
            }

            if (unit->getID() == id &&
                unit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
                unit->isCompleted())
            {
                return unit;
            }
        }

        return nullptr;
    }

    BWAPI::Unit findAvailableSupportVulture(const BWAPI::Position& buildPosition)
    {
        BWAPI::Unit bestVulture = nullptr;
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

            if (unit->getType() != BWAPI::UnitTypes::Terran_Vulture)
            {
                continue;
            }

            if (!ExpansionVultureReservation::canReserve(unit))
            {
                continue;
            }

            if (unit->isLockedDown() ||
                unit->isStasised() ||
                unit->isMaelstrommed() ||
                unit->isLoaded())
            {
                continue;
            }

            if (unit->getHitPoints() < 40)
            {
                continue;
            }

            const int distance = unit->getDistance(buildPosition);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestVulture = unit;
            }
        }

        return bestVulture;
    }

    void commandSupportVultureForBlockedExpansion(
        BlockedExpansionClearState& state,
        const Building& b,
        BWAPI::Unit blocker)
    {
        const BWAPI::Position buildPosition(
            b.finalPosition.x * 32 + b.type.tileWidth() * 16,
            b.finalPosition.y * 32 + b.type.tileHeight() * 16
        );

        BWAPI::Unit vulture = getSupportVultureByID(state.supportVultureID);

        if (!vulture && state.supportVultureID >= 0)
        {
            releaseSupportVulture(state);
        }

        if (!vulture)
        {
            vulture = findAvailableSupportVulture(buildPosition);

            if (vulture && ExpansionVultureReservation::reserve(vulture))
            {
                state.supportVultureID = vulture->getID();

                DebugQueuePrintf(
                    "Expansion blocked: support Vulture assigned id=%d",
                    vulture->getID()
                );
            }
            else
            {
                vulture = nullptr;
            }
        }

        if (!vulture)
        {
            return;
        }

        if (blocker && blocker->exists() && blocker->isDetected())
        {
            the.micro.AttackUnit(vulture, blocker);
        }
        else if (vulture->getDistance(buildPosition) > 3 * 32)
        {
            the.micro.Move(vulture, buildPosition);
        }

        BWAPI::Broodwar->drawLineMap(
            vulture->getPosition(),
            buildPosition,
            BWAPI::Colors::Orange
        );

        DrawUnitTargetTextMap(
            vulture->getPosition() + BWAPI::Position(0, 12),
            "support clear expo"
        );
    }

    BlockedExpansionClearResult tryClearBlockedExpansion(Building& b)
    {
        if (!isTerranExpansionCommandCenterTask(b))
        {
            return BlockedExpansionClearResult::NotBlocked;
        }

        const int blockKey = expansionBlockKey(b.finalPosition);

        if (!b.builderUnit ||
            !b.builderUnit->exists() ||
            !b.builderUnit->getPosition().isValid())
        {
            eraseBlockedExpansionClearState(blockKey);
            return BlockedExpansionClearResult::NotBlocked;
        }

        const BWAPI::Position buildPosition = buildingCenterPosition(b);

        if (!buildPosition.isValid())
        {
            eraseBlockedExpansionClearState(blockKey);
            return BlockedExpansionClearResult::NotBlocked;
        }

        BWAPI::Unit blocker = findExpansionBlocker(b);
        if (!blocker && BWAPI::Broodwar->canBuildHere(
            b.finalPosition,
            b.type,
            b.builderUnit,
            false))
        {
            if (b.blocked)
            {
                // The old build order was interrupted by clearing. Issue it
                // again now instead of waiting on its expired deadline.
                b.buildCommandGiven = false;
                b.startFrame = b.assignedFrame = the.now();
                b.buildersSent = 0;
            }
            b.blocked = false;
            eraseBlockedExpansionClearState(blockKey);
            return BlockedExpansionClearResult::NotBlocked;
        }

        if (!canClearExpansionFootprint(b, b.finalPosition))
        {
            b.blocked = false;
            eraseBlockedExpansionClearState(blockKey);
            return BlockedExpansionClearResult::NotBlocked;
        }

        b.blocked = true;

        const int now = BWAPI::Broodwar->getFrameCount();

        BlockedExpansionClearState& state =
            blockedExpansionClearStates[blockKey];

        if (state.firstBlockedFrame > now)
        {
            releaseSupportVulture(state);
            state = BlockedExpansionClearState();
        }

        if (state.firstBlockedFrame < 0)
        {
            state.firstBlockedFrame = now;
        }

        // An assigned expansion owns this slot until it is cleared. Do not
        // defer it after 12/45 seconds or wait for a second detector.
        const bool needsDetection = !blocker || blocker->isBurrowed() || blocker->isCloaked();
        bool scanActive = MapGrid::Instance().scanIsActiveAt(buildPosition);
        if (needsDetection && !scanActive && now - state.lastScanAttemptFrame >= 24)
        {
            state.lastScanAttemptFrame = now;
            // Cast directly: expansion clearance has no scouting reserve,
            // visibility restriction or dependency on the combat scan queue.
            if (the.micro.Scan(buildPosition))
            {
                state.lastScanFrame = now;
                scanActive = true;
            }
        }
        if (needsDetection && !scanActive)
        {
            CombatCommander::Instance().requestExpansionVesselDetection(buildPosition);
        }

        if (blocker && blocker->exists())
        {
            const bool spiderMine =
                blocker->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine;
            const bool mineral = blocker->getType().isMineralField();

            // Spider Mines never trigger on SCVs.  For an expansion footprint
            // the reliable Terran solution is therefore: reveal the mine with
            // Comsat, let the builder SCV kill the detected mine itself, then
            // retry the Command Center.  In particular, never drive a Vulture
            // onto the mine: that can detonate it and is not needed here.
            if (spiderMine || mineral)
            {
                releaseSupportVulture(state);
            }

            if (now - state.lastCommandFrame >= 12)
            {
                if (spiderMine)
                {
                    DebugQueuePrintf(
                        "Expansion mine detected: builder SCV attacks id=%d",
                        blocker->getID()
                    );
                }
                else
                {
                    DebugQueuePrintf(
                        "Expansion blocked: detected blocker %s, SCV clears",
                        blocker->getType().getName().c_str()
                    );
                }

                if (mineral)
                {
                    if (b.builderUnit->isCarryingMinerals() || b.builderUnit->isCarryingGas())
                    {
                        the.micro.ReturnCargo(b.builderUnit);
                    }
                    else
                    {
                        the.micro.MineMinerals(b.builderUnit, blocker);
                    }
                }
                else
                {
                    the.micro.AttackUnit(b.builderUnit, blocker);
                }

                if (!spiderMine && !mineral &&
                    blocker->getPlayer() != BWAPI::Broodwar->self())
                {
                    commandSupportVultureForBlockedExpansion(state, b, blocker);
                }
                else
                {
                    releaseSupportVulture(state);
                }

                state.lastCommandFrame = now;
            }

            BWAPI::Broodwar->drawCircleMap(
                blocker->getPosition(),
                32,
                BWAPI::Colors::Red,
                false
            );

            DrawUnitTargetTextMap(
                blocker->getPosition() + BWAPI::Position(0, 8),
                mineral ? "SCV clear mineral" : spiderMine ? "SCV clear mine" : "expo blocker"
            );

            return BlockedExpansionClearResult::Handling;
        }

        bool friendlyBlocker = false;
        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists() || unit == b.builderUnit ||
                unit->isFlying() || unit->isLoaded() ||
                !unitOverlapsBuildFootprint(unit, b.finalPosition, b.type))
            {
                continue;
            }

            friendlyBlocker = true;
            friendlyConstructionBlockerIDs.insert(unit->getID());
            if (unit->isConstructing() || unit->isLockedDown() ||
                unit->isStasised() || unit->isMaelstrommed())
            {
                continue;
            }
            if (unit->isSieged())
            {
                if (unit->canUnsiege())
                {
                    the.micro.Unsiege(unit, true);
                }
            }
            else if (unit->canMove() &&
                now >= unit->getLastCommandFrame() + 12)
            {
                const BWAPI::Position exit = findFriendlyExpansionExit(unit, b);
                if (exit.isValid())
                {
                    the.micro.Move(unit, exit);
                    friendlyConstructionExits.push_back(exit);
                }
            }
        }
        if (friendlyBlocker)
        {
            if (now >= state.lastCommandFrame + 12)
            {
                state.lastCommandFrame = now;
            }
            releaseSupportVulture(state);
            return BlockedExpansionClearResult::Handling;
        }

        if (now - state.lastCommandFrame >= 12 &&
            b.builderUnit->getDistance(buildPosition) > 3 * 32)
        {
            the.micro.Move(b.builderUnit, buildPosition);
            state.lastCommandFrame = now;
        }

        // Do not send a Vulture blindly into an unknown hidden blocker.  If the
        // scan reveals a Spider Mine, the builder SCV will attack it on the next
        // update; if it reveals another unit, normal blocker handling applies.
        releaseSupportVulture(state);

        BWAPI::Broodwar->drawCircleMap(
            buildPosition,
            64,
            BWAPI::Colors::Yellow,
            false
        );

        DrawUnitTargetTextMap(
            buildPosition + BWAPI::Position(0, 8),
            "blocked expo scan/clear"
        );

        return BlockedExpansionClearResult::Handling;
    }

    BWAPI::Position tileCenter(const BWAPI::TilePosition& tile)
    {
        return BWAPI::Position(
            tile.x * 32 + 16,
            tile.y * 32 + 16
        );
    }

    BWAPI::Position positionToward(
        const BWAPI::Position& from,
        const BWAPI::Position& to,
        int distance)
    {
        const double dx = double(to.x - from.x);
        const double dy = double(to.y - from.y);
        const double len = std::sqrt(dx * dx + dy * dy);

        if (len < 1.0)
        {
            return from;
        }

        return BWAPI::Position(
            from.x + int(dx / len * distance),
            from.y + int(dy / len * distance)
        );
    }

    std::vector<BWAPI::Position> getExpansionBuilderWaypoints(
        const BWAPI::Position& workerPosition,
        const BWAPI::TilePosition& buildTile)
    {
        std::vector<BWAPI::Position> result;

        const BWAPI::Position buildPosition = tileCenter(buildTile);

        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();

        if (mainBase && naturalBase &&
            mainBase->getPosition().isValid() &&
            naturalBase->getPosition().isValid())
        {
            const BWAPI::Position mainPos = mainBase->getPosition();
            const BWAPI::Position naturalPos = naturalBase->getPosition();

            result.push_back(BWAPI::Position(
                (mainPos.x + naturalPos.x) / 2,
                (mainPos.y + naturalPos.y) / 2
            ));

            result.push_back(positionToward(naturalPos, buildPosition, 160));
        }

        const BWAPI::Position toBuild(
            buildPosition.x - workerPosition.x,
            buildPosition.y - workerPosition.y
        );

        const double len = std::sqrt(
            double(toBuild.x * toBuild.x + toBuild.y * toBuild.y)
        );

        if (len > 1.0)
        {
            const int sideX = int(-toBuild.y / len * 160);
            const int sideY = int(toBuild.x / len * 160);

            result.push_back(BWAPI::Position(
                workerPosition.x + sideX,
                workerPosition.y + sideY
            ));

            result.push_back(BWAPI::Position(
                workerPosition.x - sideX,
                workerPosition.y - sideY
            ));
        }

        result.push_back(positionToward(buildPosition, workerPosition, 96));

        return result;
    }

    bool rerouteStuckExpansionBuilder(
        BWAPI::Unit builder,
        BWAPI::UnitType buildingType,
        const BWAPI::TilePosition& buildTile)
    {
        if (!builder ||
            !builder->exists() ||
            !builder->getPosition().isValid() ||
            !buildTile.isValid())
        {
            return false;
        }

        if (buildingType != BWAPI::UnitTypes::Terran_Command_Center)
        {
            return false;
        }

        const BWAPI::Position buildPosition = tileCenter(buildTile);

        if (builder->getDistance(buildPosition) < 4 * 32)
        {
            expansionBuilderRouteStates.erase(builder->getID());
            return false;
        }

        const int now = BWAPI::Broodwar->getFrameCount();
        const int distance = builder->getDistance(buildPosition);

        ExpansionBuilderRouteState& state =
            expansionBuilderRouteStates[builder->getID()];

        if (now < state.lastCheckFrame)
        {
            state = ExpansionBuilderRouteState();
        }

        if (state.lastCheckFrame == 0)
        {
            state.lastCheckFrame = now;
            state.lastDistance = distance;
            return false;
        }

        if (now - state.lastCheckFrame < 24)
        {
            return false;
        }


        if (distance >= state.lastDistance - 24)
        {
            state.stuckFrames += now - state.lastCheckFrame;
        }
        else
        {
            state.stuckFrames = 0;
            state.waypointIndex = 0;
        }

        state.lastCheckFrame = now;
        state.lastDistance = distance;

        if (state.stuckFrames < 24 * 4)
        {
            return false;
        }

        if (now - state.lastMoveFrame < 24 * 2)
        {
            return true;
        }

        std::vector<BWAPI::Position> waypoints =
            getExpansionBuilderWaypoints(builder->getPosition(), buildTile);

        if (waypoints.empty())
        {
            return false;
        }

        // Each fallback waypoint is tried once. If none works, allow the
        // normal BuildingManager timeout path to release and reassign the SCV.
        if (state.waypointIndex >= int(waypoints.size()))
        {
            expansionBuilderRouteStates.erase(builder->getID());
            return false;
        }

        BWAPI::Position waypoint = waypoints[state.waypointIndex];

        if (!waypoint.isValid())
        {
            state.waypointIndex++;
            return true;
        }

        const BWAPI::Position gate =
            the.placer.terranMainWallTransitWaypoint(
                builder->getPosition(), waypoint);
        builder->move(gate.isValid() ? gate : waypoint);

        BWAPI::Broodwar->drawCircleMap(
            waypoint,
            32,
            BWAPI::Colors::Orange,
            false
        );

        BWAPI::Broodwar->drawLineMap(
            builder->getPosition(),
            waypoint,
            BWAPI::Colors::Orange
        );

        DebugQueuePrintf(
            "Expansion builder stuck, reroute SCV via waypoint %d",
            state.waypointIndex
        );

        state.lastMoveFrame = now;
        state.waypointIndex++;

        return true;
    }
}


bool BuildingManager::isAirRelocatedCommandCenter(BWAPI::Unit center) const
{
    return center && center->exists() &&
        center->getType() == BWAPI::UnitTypes::Terran_Command_Center &&
        airRelocatedCommandCenterIDs.find(center->getID()) !=
            airRelocatedCommandCenterIDs.end();
}

BuildingManager::BuildingManager()
    : _reservedMinerals(0)
    , _reservedGas(0)
    , _stalledForLackOfSpace(false)
{
}

// Called every frame from GameCommander.
void BuildingManager::update()
{
    friendlyConstructionBlockerIDs.clear();
    friendlyConstructionExits.clear();
    validateBuildings();					// check to see if assigned workers have died en route or while constructing
    assignWorkersToUnassignedBuildings();   // assign workers to the unassigned buildings and label them 'planned'    
    constructAssignedBuildings();           // for each planned building, if the worker isn't constructing, send the command    
    checkForStartedConstruction();          // check to see if any buildings have started construction and update data structures    
    checkForDeadTerranBuilders();           // if we are terran and a building is under construction without a worker, assign a new one    
    checkForCompletedBuildings();           // check to see if any buildings have completed and update data structures
    moveFallbackCommandCenters();
    checkReservedResources();               // verify that reserved minerals and gas are counted correctly

    if (the.now() % 72 == 0 && the.selfRace() == BWAPI::Races::Terran)
    {
        clearAbandonedTerranBuildings();
    }
}

// The building took too long to start, or we lost too many workers trying to build it.
// If true, the building will be canceled.
bool BuildingManager::buildingTimedOut(const Building & b) const
{
    if (b.blocked && isTerranExpansionCommandCenterTask(b))
    {
        return false;
    }
    return
        the.now() - b.startFrame > 60 * 24 ||
        b.buildersSent > 2;
}

// STEP 1: DO BOOK KEEPING ON BUILDINGS WHICH MAY HAVE DIED OR TIMED OUT
void BuildingManager::validateBuildings()
{
    // Not in the opening.
    if (!ProductionManager::Instance().isOutOfBook())
    {
        return;
    }

    // The purpose of this vector is to avoid changing the list of buildings
    // while we are in the midst of iterating through it.
    std::vector< std::reference_wrapper<Building> > toRemove;
    
    // Find and remove any buildings which have failed construction and should be deleted.
    for (Building & b : _buildings)
    {
        if (b.status != BuildingStatus::UnderConstruction &&
            buildingTimedOut(b) &&
            (!b.buildingUnit || b.type.getRace() == BWAPI::Races::Terran && !b.builderUnit))
        {
            if (b.macroLocation != MacroLocation::Tile)
                the.placer.avoidFailedProductionLocation(b.type, b.finalPosition);
            toRemove.push_back(b);
        }
        else if (b.status == BuildingStatus::UnderConstruction)
        {
            if (!b.buildingUnit ||
                !b.buildingUnit->exists() ||
                b.buildingUnit->getHitPoints() <= 0 ||
                !b.buildingUnit->getType().isBuilding())
            {
                toRemove.push_back(b);
            }
        }
    }

    undoBuildings(toRemove);
}

// STEP 2: ASSIGN WORKERS TO BUILDINGS WITHOUT THEM
// Also places the building.
void BuildingManager::assignWorkersToUnassignedBuildings()
{
    // For each building that doesn't have a builder, assign one.
    for (Building & b : _buildings)
    {
        if (b.status != BuildingStatus::Unassigned)
        {
            continue;
        }

        // Skip protoss buildings that need pylon power if there is no space for them.
        if (typeIsStalled(b.type))
        {
            continue;
        }

        

        // The builder may or may not have been decided before we got this far.
        // If we don't already have a valid builder, choose one.
        // NOTE There is a dependency loop here. To get the builder, we want to know the
        //      building's position. But to check its final position we need to know the
        //      builder unit, because other units block the building placement while the
        //      builder unit itself does not. To solve this, we get the builder first based
        //      on the desired rather than the final position.
        //      Better solutions are possible!
		if (!validBuilder(b.builderUnit))
        {
            releaseBuilderUnit(b);		           // does nothing if it's null
            setBuilderUnit(b);
            if (b.builderUnit)
            {
                //DebugQueuePrintf("(re)assign builder %d to %s", b.builderUnit->getID(), UnitTypeName(b.type).c_str());
                ++b.buildersSent;
            }
        }

        // No good, we can't get a worker. Give up on the building for this frame.
        if (!b.builderUnit)
        {
            continue;
        }

        // We want the worker before the location, so that we can avoid counting the worker
        // as an obstacle for the location it is already in--ProductionManager sends the worker early.
        // Otherwise we may have to re-place the building elsewhere, causing unnecessary movement.
        if (b.type == BWAPI::UnitTypes::Terran_Command_Center &&
            b.macroLocation != MacroLocation::Main &&
            secondCommandCenterBlockedOnGround())
        {
            b.macroLocation = MacroLocation::Main;
            b.desiredPosition = the.bases.myMain()->getTilePosition();
        }

        BWAPI::TilePosition testLocation = getBuildingLocation(b);
        if (!testLocation.isValid())
        {
            // The building could not be placed (or was placed incorrectly due to a bug, which should not happen).
            // Recognize the case where protoss building placement is stalled for lack of space.
            // In principle, terran or zerg could run out of space, but it's rare in practice.
            
            if (b.builderUnit && b.buildersSent > 0)
            {
                //DebugQueuePrintf("bad location, clear builder %d of %s", b.builderUnit->getID(), UnitTypeName(b.type).c_str());
                --b.buildersSent;
            }
            releaseBuilderUnit(b);
            continue;
        }
        b.finalPosition = testLocation;

        //DebugQueuePrintf("assign builder %d to %s", b.builderUnit->getID(), UnitTypeName(b.type).c_str());
        the.placer.reserveTiles(b);

        b.status = BuildingStatus::Assigned;
        b.assignedFrame = the.now();
    }
}

// STEP 3: ISSUE CONSTRUCTION ORDERS TO ASSIGNED BUILDINGS AS NEEDED
void BuildingManager::constructAssignedBuildings()
{
    for (Building & b : _buildings)
    {
        if (b.status != BuildingStatus::Assigned)
        {
            continue;
        }
        // A gate evacuation owns this worker for a few frames; retain its
        // construction task instead of sending it straight back underneath.
        if (the.micro.landingClearanceActive(b.builderUnit)) continue;

        const bool flexibleProduction = ProductionManager::Instance().isOutOfBook() &&
            (b.type == BWAPI::UnitTypes::Terran_Barracks ||
             b.type == BWAPI::UnitTypes::Terran_Factory ||
             b.type == BWAPI::UnitTypes::Terran_Starport) &&
            (b.macroLocation == MacroLocation::Anywhere ||
             b.macroLocation == MacroLocation::Main ||
             b.macroLocation == MacroLocation::Macro) &&
            !(b.type == BWAPI::UnitTypes::Terran_Barracks &&
              the.placer.hasTerranMainWall() &&
              b.finalPosition == the.placer.getTerranMainWallBarracksTile());
        if (flexibleProduction && !b.buildingUnit && validBuilder(b.builderUnit) &&
            !b.builderUnit->getBuildUnit() && the.now() - b.assignedFrame >= 20 * 24)
        {
            // Do not cancel and recreate the same blocked task every minute.
            // Keep its reservation but let another plot and SCV start it.
            the.placer.avoidFailedProductionLocation(b.type, b.finalPosition);
            the.placer.freeTiles(b);
            releaseBuilderUnit(b);
            b.finalPosition = BWAPI::TilePositions::Invalid;
            b.status = BuildingStatus::Unassigned;
            b.buildCommandGiven = false;
            b.startFrame = the.now();
            b.buildersSent = std::max(0, b.buildersSent - 1);
            continue;
        }

        // A ground expansion task may have been assigned before the enemy
        // sealed the exit. Replan the still-unstarted second CC inside main
        // instead of leaving its SCV waiting indefinitely at the choke.
        if (b.type == BWAPI::UnitTypes::Terran_Command_Center &&
            b.macroLocation != MacroLocation::Main &&
            secondCommandCenterBlockedOnGround())
        {
            the.placer.freeTiles(b);
            releaseBuilderUnit(b);
            b.macroLocation = MacroLocation::Main;
            b.desiredPosition = the.bases.myMain()->getTilePosition();
            b.finalPosition = BWAPI::TilePositions::Invalid;
            b.status = BuildingStatus::Unassigned;
            b.buildCommandGiven = false;
            b.buildersSent = std::max(0, b.buildersSent - 1);
            continue;
        }

        if (!b.builderUnit ||
            b.builderUnit->getPlayer() != the.self() ||
            !b.builderUnit->exists() && b.type != BWAPI::UnitTypes::Zerg_Extractor ||
            b.builderUnit->isLockedDown() ||
            b.builderUnit->isStasised() ||
            b.builderUnit->isMaelstrommed() ||
            b.builderUnit->isBurrowed())
        {
            // NOTE A zerg drone builderUnit no longer exists() after starting an extractor.
            //      The geyser unit changes into the building, the drone vanishes.
            //      For other zerg buildings, the drone changes into the building.
            releaseBuilderUnit(b);

            //DebugQueuePrintf("b.builderUnit gone, b.type = %s", UnitTypeName(b.type).c_str());

            b.buildCommandGiven = false;
            b.status = BuildingStatus::Unassigned;
            the.placer.freeTiles(b);
        }
        else if (!b.builderUnit->isConstructing())
        {
            // Threats can arrive after placement was approved. Release the
            // unstarted forward Bunker's builder instead of walking it into fire.
            if (b.type == BWAPI::UnitTypes::Terran_Bunker &&
                !StrategyManager::Instance().canBuildTerranDefensiveBunkerAt(b.finalPosition))
            {
                if (the.bases.myMain())
                    the.micro.MoveSafely(b.builderUnit, the.bases.myMain()->getCenter());
                releaseBuilderUnit(b);
                the.placer.freeTiles(b);
                b.status = BuildingStatus::Unassigned;
                b.buildCommandGiven = false;
                b.buildersSent = std::max(0, b.buildersSent - 1);
                continue;
            }
            const BlockedExpansionClearResult clearResult = tryClearBlockedExpansion(b);
            if (clearResult == BlockedExpansionClearResult::Handling)
            {
                // Preserve clearing/moving orders until the footprint is free.
                continue;
            }
            const bool positionExplored = isBuildingPositionExplored(b);

            if (!positionExplored)
            {
                // We haven't explored the build position. Go there.
                // If we're going to start a new base, use the base's distance map.
                // NOTE getBaseAtTilePosition() returns null if there is no base at or very near the position.
                Base * base = b.type.isResourceDepot() ? the.bases.getBaseAtTilePosition(b.finalPosition) : nullptr;
				if (base)
				{
					if (base->getSafeTileDistance(base->getTilePosition()) >= 0)
					{
						the.micro.MoveToBase(b.builderUnit, base, b.getCenter(), true);
					}
					// Else do nothing for now. Don't send the worker to die.
				}
				else
				{
					the.micro.MoveSafely(b.builderUnit, b.getCenter());
				}
            }
            // if this is not the first time we've sent this guy to build this
            // it must be the case that something was in the way
            else if (b.buildCommandGiven)
            {

                // Special handling for Terran expansion Command Center.
                // If the SCV is stuck on the way, do not immediately release it.
                // First try to send it through alternative waypoints.
                if (b.type == BWAPI::UnitTypes::Terran_Command_Center &&
                    b.macroLocation != MacroLocation::Main)
                {
                    if (rerouteStuckExpansionBuilder(
                        b.builderUnit,
                        b.type,
                        b.finalPosition))
                    {
                        // Give the SCV more time after rerouting.
                        b.placeBuildingDeadline = the.now() + 24 * 4;
                        continue;
                    }

                }

                // Give it a little time to happen.
                // This mainly prevents over-frequent retrying.
                if (the.now() > b.placeBuildingDeadline)
                {
                    releaseBuilderUnit(b);

                    b.buildCommandGiven = false;
                    b.status = BuildingStatus::Unassigned;

                    b.buildersSent = std::max(0, b.buildersSent - 1);

                    the.placer.freeTiles(b);
                }
            }
            else
            {
                if (b.type == BWAPI::UnitTypes::Terran_Command_Center &&
                    b.macroLocation != MacroLocation::Main)
                {
                    if (rerouteStuckExpansionBuilder(
                        b.builderUnit,
                        b.type,
                        b.finalPosition))
                    {
                        b.placeBuildingDeadline = the.now() + 24 * 4;
                        continue;
                    }
                }
                // Special case for sunkens and spores: Build the creep colony then morph it.
                BWAPI::UnitType t = b.type;
                
                
                // Issue the build order and record whether it succeeded.
                // If the builderUnit is zerg, it changes to !exists() when it builds.
                b.buildCommandGiven = the.micro.Build(b.builderUnit, t, b.finalPosition);

                if (b.buildCommandGiven)
                {
                    b.placeBuildingDeadline =
                        the.now() + 10 * BWAPI::Broodwar->getLatencyFrames();
                }
            }
        }
    }
}

// STEP 4: UPDATE DATA STRUCTURES FOR BUILDINGS STARTING CONSTRUCTION
void BuildingManager::checkForStartedConstruction()
{
    for (BWAPI::Unit buildingStarted : the.self()->getUnits())
    {
        // filter out units which aren't buildings under construction
        if (!buildingStarted->getType().isBuilding() || !buildingStarted->isBeingConstructed())
        {
            continue;
        }

        // check all our building status objects to see if we have a match and if we do, update it
        for (Building & b : _buildings)
        {
            if (b.status != BuildingStatus::Assigned)
            {
                continue;
            }
        
            // check if the positions match
            if (b.finalPosition == buildingStarted->getTilePosition())
            {
                if (b.builderUnit)
                {
                    expansionBuilderRouteStates.erase(b.builderUnit->getID());
                }

                eraseBlockedExpansionClearState(expansionBlockKey(b.finalPosition));
                // the resources should now be spent, so unreserve them
                _reservedMinerals -= buildingStarted->getType().mineralPrice();
                _reservedGas      -= buildingStarted->getType().gasPrice();

                // flag it as started and set the buildingUnit
                b.underConstruction = true;
                b.buildingUnit = buildingStarted;

                // The building is started; handle zerg and protoss builders.
                // Terran builders are dealt with after the building finishes.
                
                

                b.status = BuildingStatus::UnderConstruction;

                the.placer.freeTiles(b);

                // Only one Building will match.
                // We keep running the outer loop to look for more buildings starting construction.
                break;
            }
        }
    }
}

// STEP 5: IF THE SCV DIED DURING CONSTRUCTION, ASSIGN A NEW ONE
void BuildingManager::checkForDeadTerranBuilders()
{
    

    for (Building & b : _buildings)
    {
        if (b.status != BuildingStatus::UnderConstruction)
        {
            continue;
        }

        UAB_ASSERT(b.buildingUnit, "null buildingUnit");

        if (!b.buildingUnit || !b.buildingUnit->exists())
        {
            continue;
        }

        // A live SCV may have been interrupted or its resume order may have
        // failed. Treat that case like a missing builder instead of waiting
        // indefinitely for the worker to disappear.
        if (validBuilder(b.builderUnit) && b.builderUnit->isConstructing())
        {
            continue;
        }

        // A replacement sent into the same ground-fire pocket usually dies.
        // Recheck every frame so construction resumes when the threat leaves.
        bool unsafeToResume = false;
        if (b.buildingUnit->getPosition().isValid())
        {
            for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
                b.buildingUnit->getPosition(), 4 * 32,
                BWAPI::Filter::IsEnemy))
            {
                if (enemy && enemy->exists() && enemy->isVisible() &&
                    !enemy->isInvincible() &&
                    (enemy->getType().isWorker() ||
                     UnitUtil::CanAttackGround(enemy)))
                {
                    unsafeToResume = true;
                    break;
                }
            }
        }
        if (unsafeToResume)
        {
            releaseBuilderUnit(b);
            continue;
        }

        bool assignedNow = false;
        if (!validBuilder(b.builderUnit))
        {
            releaseBuilderUnit(b);
            setBuilderUnit(b);      // don't count it in buildersSent
            assignedNow = b.builderUnit != nullptr;
        }

        // Do not spam an SCV that is travelling to the site. A missed or
        // interrupted right-click gets another attempt after a short delay.
        if (b.builderUnit && !the.micro.landingClearanceActive(b.builderUnit) &&
            (assignedNow ||
             (the.now() - b.builderUnit->getLastCommandFrame() >= 48 &&
              the.now() % 24 == 0)))
        {
            b.builderUnit->rightClick(b.buildingUnit);
        }
    }
}

// STEP 6: CHECK FOR COMPLETED BUILDINGS
// In case of terran gas steal, stop construction a little early,
// so we can cancel the refinery later and recover resources. 
// Zerg and protoss can't do that.
void BuildingManager::checkForCompletedBuildings()
{
    std::vector< std::reference_wrapper<Building> > toRemove;

    for (Building & b : _buildings)
    {
        if (b.status != BuildingStatus::UnderConstruction)
        {
            continue;       
        }

        UAB_ASSERT(b.buildingUnit, "null buildingUnit");

        if (b.buildingUnit->isCompleted())
        {
            if (b.type == BWAPI::UnitTypes::Terran_Command_Center &&
                b.macroLocation == MacroLocation::Main &&
                the.bases.myMain() &&
                b.finalPosition != the.bases.myMain()->getTilePosition())
            {
                fallbackCommandCenters[b.buildingUnit->getID()] =
                    BWAPI::TilePositions::Invalid;
                airRelocatedCommandCenterIDs.insert(
                    b.buildingUnit->getID());
            }

            // If we are terran, give the worker back to worker manager.
            // Zerg and protoss are handled when the building starts.
            
            {
                releaseBuilderUnit(b);
            }

            // Special case for a spore colony or sunken colony.
            // Further special case: Don't do the morph if the building will end up very weak.
            // NOTE A sunken colony loses 100 hp when it finishes its morph.
            
            {
                // Done with the building record.
                toRemove.push_back(b);
            }
        }
        else
        {
            // The building, whatever it is, is not completed.
            // If it is a terran gas steal, stop construction early.
            if (b.isGasSteal &&
                the.selfRace() == BWAPI::Races::Terran &&
                b.builderUnit &&
                b.builderUnit->canHaltConstruction() &&
                b.buildingUnit->getRemainingBuildTime() < 24)
            {
                b.builderUnit->haltConstruction();
                releaseBuilderUnit(b);

                // Call the building done. It's as finished as we want it to be.
                toRemove.push_back(b);
            }
        }
    }

    removeBuildings(toRemove);
}

// Error check: Bugs could cause resources to be reserved and never released.
// We correct the values as a workaround.
// Currently there are no known bugs causing this.
void BuildingManager::checkReservedResources()
{
    // Check for errors.
    int minerals = 0;
    int gas = 0;

    for (Building & b : _buildings)
    {
        if (b.status == BuildingStatus::Assigned || b.status == BuildingStatus::Unassigned)
        {
            minerals += b.type.mineralPrice();
            gas += b.type.gasPrice();
        }
    }

    if (minerals != _reservedMinerals || gas != _reservedGas)
    {
        // This should ideally never happen. If it does, we correct the error and carry on.
        //DebugQueuePrintf("reserves wrong: %d %d should be %d %d", _reservedMinerals, _reservedGas, minerals, gas);
        _reservedMinerals = minerals;
        _reservedGas = gas;
    }
}

// It's possible for a terran building to be abandoned unfinished and never canceled,
// due to a bug somewhere. Work around it: Cancel the building anyway.
void BuildingManager::clearAbandonedTerranBuildings()
{
    for (BWAPI::Unit building : the.self()->getUnits())
    {
        // Add-ons are built by their parent building and are tracked by
        // ProductionGoal, not by this worker-building queue. Treating one as
        // abandoned can cancel it every time this recovery check runs.
        if (building->getType().isBuilding() &&
            !building->getType().isAddon() &&
            !building->isCompleted() &&
            !building->isBeingConstructed() &&
            building->canCancelConstruction())
        {
            // The building is abandoned if it is not in the building queue.
            for (const Building & b : _buildings)
            {
                if (b.buildingUnit == building)
                {
                    goto not_abandoned;
                }
            }
            // If we got here, it was abandoned.
            if (Config::Debug::DrawQueueFixInfo)
            {
                DebugQueuePrintf("queue: cancel abandoned %s", UnitTypeName(building).c_str());
            }
            building->cancelConstruction();
        }
not_abandoned:
        ;
    }
}

// Add a new building to be constructed and return it.
// The builder may be null. In that case, the building manager will assign a worker on its own later.
Building & BuildingManager::addTrackedBuildingTask(const MacroAct & act, BWAPI::TilePosition desiredLocation, BWAPI::Unit builder, bool isGasSteal)
{
    UAB_ASSERT(act.isBuilding(), "bad building");

    if (isGasSteal)
    {
        //DebugQueuePrintf("gas steal into building manager");
    }

    BWAPI::UnitType type = act.getUnitType();

    _reservedMinerals += type.mineralPrice();
    _reservedGas += type.gasPrice();

    Building b(type, desiredLocation);
    b.macroLocation = act.getMacroLocation();
    if (b.macroLocation == MacroLocation::Tile)
    {
        b.desiredPosition = b.finalPosition = act.getTileLocation();
    }
    b.isGasSteal = isGasSteal;
    b.status = BuildingStatus::Unassigned;

    // The builder, if provided, may have been killed, or be otherwise unavailable.
    if (validBuilder(builder))
    {
        //DebugQueuePrintf("build man receives worker %d for %s", builder->getID(), UnitTypeName(type).c_str());
        b.builderUnit = builder;
        b.buildersSent = 1;
        // Ensure that our builder is assigned to be a builder. The caller may not have done it.
        WorkerManager::Instance().setBuildWorker(builder);
    }

    _buildings.push_back(b);      // make a "permanent" copy of the Building object
    return _buildings.back();     // return a reference to the permanent copy
}

// Add a new building to be constructed.
void BuildingManager::addBuildingTask(const MacroAct & act, BWAPI::TilePosition desiredLocation, BWAPI::Unit builder, bool isGasSteal)
{
    (void) addTrackedBuildingTask(act, desiredLocation, builder, isGasSteal);
}

bool BuildingManager::isBuildingPositionExplored(const Building & b) const
{
    BWAPI::TilePosition tile = b.finalPosition;

    // For each tile where the building will be built, is the tile explored?
    for (int x=0; x<b.type.tileWidth(); ++x)
    {
        for (int y=0; y<b.type.tileHeight(); ++y)
        {
            if (!BWAPI::Broodwar->isExplored(tile.x + x,tile.y + y))
            {
                return false;
            }
        }
    }

    return true;
}

// Do we have a builder unit that can build?
bool BuildingManager::validBuilder(BWAPI::Unit worker) const
{
    return
        worker &&
        worker->exists() &&
        !worker->isLockedDown() &&
        !worker->isStasised() &&
        !worker->isMaelstrommed() &&
        !worker->isBurrowed() &&
        worker->getPlayer() == the.self() &&     // not mind controlled
        worker->getType().isWorker();            // not morphed into a building already
}

// Assign a worker to contruct the building.
void BuildingManager::setBuilderUnit(Building & b)
{
    UAB_ASSERT(!b.builderUnit, "incorrectly replacing builder");

    // Grab the closest worker from WorkerManager.
    // It knows to return the scout worker if this is a gas steal.
    b.builderUnit = WorkerManager::Instance().getBuilder(b);
    if (b.builderUnit)
    {
        WorkerManager::Instance().setBuildWorker(b.builderUnit);
        //DebugQueuePrintf("build man assigns worker %d for %s", b.builderUnit ? b.builderUnit->getID() : -1, UnitTypeName(b.type).c_str());
    }
}

// Notify the worker manager that the worker is free again,
// but not if the scout manager owns the worker.
void BuildingManager::releaseBuilderUnit(Building & b)
{
    b.blocked = false;
    if (isTerranExpansionCommandCenterTask(b))
    {
        eraseBlockedExpansionClearState(expansionBlockKey(b.finalPosition));
    }

    if (b.isGasSteal)
    {
        ScoutManager::Instance().setGasStealOver();
    }
    else
    {
        if (b.builderUnit)
        {
            expansionBuilderRouteStates.erase(b.builderUnit->getID());

            WorkerManager::Instance().finishedWithWorker(b.builderUnit);
        }
    }
    b.builderUnit = nullptr;
}

int BuildingManager::getReservedMinerals() const
{
    return _reservedMinerals;
}

int BuildingManager::getReservedGas() const
{
    return _reservedGas;
}

void BuildingManager::getStalledReservations(int& minerals, int& gas) const
{
    minerals = 0;
    gas = 0;
    for (const Building& b : _buildings)
    {
        if (b.type.getRace() == BWAPI::Races::Terran &&
            the.now() >= b.startFrame + 8 * 24 &&
            (b.status == BuildingStatus::Unassigned ||
             (b.status == BuildingStatus::Assigned && b.blocked)))
        {
            minerals += b.type.mineralPrice();
            gas += b.type.gasPrice();
        }
    }
}

// In the building queue with any status.
bool BuildingManager::isBeingBuilt(BWAPI::UnitType type) const
{
    for (const Building & b : _buildings)
    {
        if (b.type == type)
        {
            return true;
        }
    }

    return false;
}

// Whether this exact worker is still attached to a live BuildingManager task.
// WorkerManager uses this to distinguish a pre-positioned/constructing SCV
// from an orphaned Build job after a task has completed or been canceled.
bool BuildingManager::ownsBuilder(BWAPI::Unit worker) const
{
    if (!worker)
    {
        return false;
    }

    for (const Building& building : _buildings)
    {
        if (building.builderUnit == worker)
        {
            return true;
        }
    }

    return false;
}

// Number in the building queue with status other than "under constrution".
size_t BuildingManager::getNumUnstarted() const
{
    size_t count = 0;

    for (const Building & b : _buildings)
    {
        if (b.status != BuildingStatus::UnderConstruction)
        {
            ++count;
        }
    }

    return count;
}

// Number of a given type in the building queue with status other than "under constrution".
size_t BuildingManager::getNumUnstarted(BWAPI::UnitType type) const
{
    size_t count = 0;

    for (const Building & b : _buildings)
    {
        if (b.type == type && b.status != BuildingStatus::UnderConstruction)
        {
            ++count;
        }
    }

    return count;
}

// Number of a given type still waiting in BuildingManager at one strategic
// location. Terran rule goals must keep, for example, a main bunker distinct
// from a natural bunker instead of satisfying both with the same structure.
size_t BuildingManager::getNumUnstarted(
    BWAPI::UnitType type,
    MacroLocation location,
    BWAPI::TilePosition tile) const
{
    size_t count = 0;

    for (const Building & b : _buildings)
    {
        if (b.type == type &&
            b.macroLocation == location &&
            (location != MacroLocation::Tile ||
             !tile.isValid() ||
             b.desiredPosition == tile) &&
            b.status != BuildingStatus::UnderConstruction)
        {
            ++count;
        }
    }

    return count;
}

// Return the concrete positions already owned by BuildingManager but not yet
// represented by a BWAPI building unit. Strategic static-defense layouts use
// this to keep the same slots while an SCV is travelling or waiting to build;
// otherwise a temporarily occupied tile can make the layout drift every plan
// refresh and post a second copy nearby.
std::vector<BWAPI::TilePosition> BuildingManager::getUnstartedBuildingTiles(
    BWAPI::UnitType type) const
{
    std::vector<BWAPI::TilePosition> tiles;

    for (const Building & b : _buildings)
    {
        if (b.type != type || b.status == BuildingStatus::UnderConstruction)
        {
            continue;
        }

        const BWAPI::TilePosition tile = b.finalPosition.isValid()
            ? b.finalPosition
            : b.desiredPosition;

        if (tile.isValid() &&
            std::find(tiles.begin(), tiles.end(), tile) == tiles.end())
        {
            tiles.push_back(tile);
        }
    }

    return tiles;
}

bool BuildingManager::isGasStealInQueue() const
{
    for (const Building & b : _buildings)
    {
        if (b.isGasSteal)
        {
            return true;
        }
    }

    return false;
}

// We have queued an expansion to the given base.
// (Or at least some building at the same location.)
bool BuildingManager::isBasePlanned(const Base * base) const
{
    for (const Building & b : _buildings)
    {
        if (b.finalPosition == base->getTilePosition())
        {
            return true;
        }
    }

    return false;
}

bool BuildingManager::isClearingConstructionFootprint(BWAPI::Unit unit) const
{
    return unit && friendlyConstructionBlockerIDs.count(unit->getID()) != 0;
}

void BuildingManager::drawBuildingInformation(int x, int y)
{
    if (!Config::Debug::DrawBuildingInfo)
    {
        return;
    }

    /*
    // Enemy building completion times, as best we can tell.
    for (const auto & kv : the.info.getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo & ui(kv.second);

        if (ui.type.isBuilding())
        {
            BWAPI::Broodwar->drawTextMap(ui.lastPosition.x,ui.lastPosition.y+17,"\x07%d", ui.completeBy);
        }
    }
    */

    /*
    // Unit IDs for our own units.
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        BWAPI::Broodwar->drawTextMap(unit->getPosition().x,unit->getPosition().y+5,"\x07%d",unit->getID());
    }
    */

    BWAPI::Broodwar->drawTextScreen(x,y+20,"\x04 Building");
    BWAPI::Broodwar->drawTextScreen(x+150,y+20,"\x04 State");

    int yspace = 0;

    for (const Building & b : _buildings)
    {
        std::string steal = b.isGasSteal ? " (steal)" : "";
        if (b.status == BuildingStatus::Unassigned)
        {
            int x1 = b.desiredPosition.x * 32;
            int y1 = b.desiredPosition.y * 32;
            int x2 = (b.desiredPosition.x + b.type.tileWidth()) * 32;
            int y2 = (b.desiredPosition.y + b.type.tileHeight()) * 32;

            char color = yellow;
            if (typeIsStalled(b.type))
            {
                color = red;
            }

            BWAPI::Broodwar->drawTextScreen(x, y + 40 + ((yspace)* 10), "%c %s%s", color, NiceMacroActName(b.type.getName()).c_str(), steal.c_str());
            BWAPI::Broodwar->drawTextScreen(x + 150, y + 40 + ((yspace++) * 10), "%c Waiting", color);
            BWAPI::Broodwar->drawBoxMap(x1, y1, x2, y2, BWAPI::Colors::Green, false);
        }
        else if (b.status == BuildingStatus::Assigned)
        {
            BWAPI::Broodwar->drawTextScreen(x, y + 40 + ((yspace)* 10), "\x03 %s%s %d", NiceMacroActName(b.type.getName()).c_str(), steal.c_str(), b.builderUnit->getID());
            BWAPI::Broodwar->drawTextScreen(x+150,y+40+((yspace++)*10),"\x03 Assigned (%d,%d)",b.finalPosition.x,b.finalPosition.y);

            int x1 = b.finalPosition.x*32;
            int y1 = b.finalPosition.y*32;
            int x2 = (b.finalPosition.x + b.type.tileWidth())*32;
            int y2 = (b.finalPosition.y + b.type.tileHeight())*32;

            BWAPI::Broodwar->drawLineMap(b.builderUnit->getPosition().x,b.builderUnit->getPosition().y,(x1+x2)/2,(y1+y2)/2,BWAPI::Colors::Orange);
            BWAPI::Broodwar->drawBoxMap(x1,y1,x2,y2,BWAPI::Colors::Red,false);
        }
        else if (b.status == BuildingStatus::UnderConstruction)
        {
            BWAPI::Broodwar->drawTextScreen(x, y + 40 + ((yspace)* 10), "\x03 %s%s %d", NiceMacroActName(b.type.getName()).c_str(), steal.c_str(), b.buildingUnit->getID());
            BWAPI::Broodwar->drawTextScreen(x+150,y+40+((yspace++)*10),"\x03 Const");
        }
    }
}

BuildingManager & BuildingManager::Instance()
{
    static BuildingManager instance;
    return instance;
}

// The buildings queued and not yet started.
std::vector<BWAPI::UnitType> BuildingManager::buildingsQueued()
{
    std::vector<BWAPI::UnitType> buildingsQueued;

    for (const Building & b : _buildings)
    {
        if (b.status == BuildingStatus::Unassigned || b.status == BuildingStatus::Assigned)
        {
            buildingsQueued.push_back(b.type);
        }
    }

    return buildingsQueued;
}

// Cancel a given building when possible.
// Used as part of the extractor trick or in an emergency.
// NOTE CombatCommander::cancelDyingItems() can also cancel buildings, including
//      morphing zerg structures which the BuildingManager does not handle.
void BuildingManager::cancelBuilding(Building & b)
{
    std::vector< std::reference_wrapper<Building> > toRemove;

    // No matter the status, we don't want to keep the worker.
    releaseBuilderUnit(b);

    if (b.status == BuildingStatus::Unassigned)
    {
        toRemove.push_back(b);
        undoBuildings(toRemove);
    }
    else if (b.status == BuildingStatus::Assigned)
    {
        toRemove.push_back(b);
        undoBuildings(toRemove);
    }
    else if (b.status == BuildingStatus::UnderConstruction)
    {
        if (b.buildingUnit && b.buildingUnit->exists() && !b.buildingUnit->isCompleted())
        {
            the.micro.Cancel(b.buildingUnit);
        }
        toRemove.push_back(b);
        undoBuildings(toRemove);
    }
    else
    {
        UAB_ASSERT(false, "unexpected building status");
    }
}

// It's an emergency. Cancel all buildings which are not yet started.
void BuildingManager::cancelQueuedBuildings()
{
    std::vector< std::reference_wrapper<Building> > toCancel;

    for (Building & b : _buildings)
    {
        if (b.status == BuildingStatus::Unassigned || b.status == BuildingStatus::Assigned)
        {
            toCancel.push_back(b);
        }
    }

    // Undo the whole batch before erasing anything: each entry refers into
    // _buildings, whose references are invalidated by an earlier erase.
    undoBuildings(toCancel);
}

// It's an emergency. Cancel all buildings of a given type.
void BuildingManager::cancelBuildingType(BWAPI::UnitType t)
{
    std::vector< std::reference_wrapper<Building> > toCancel;

    for (Building & b : _buildings)
    {
        if (b.type == t)
        {
            toCancel.push_back(b);
        }
    }

    undoBuildings(toCancel);
}

BWAPI::TilePosition BuildingManager::getBuildingLocation(const Building & b)
{
    if (b.isGasSteal)
    {
        Base * enemyBase = the.bases.enemyStart();
        UAB_ASSERT(enemyBase, "Should find enemy base before gas steal");
        UAB_ASSERT(enemyBase->getGeysers().size() > 0, "Should have spotted an enemy geyser");

        for (BWAPI::Unit geyser : enemyBase->getGeysers())
        {
            return geyser->getTilePosition();
        }
    }

    

    if (b.type.isRefinery())
    {
        return the.placer.getRefineryPosition();
    }

    MacroLocation loc = b.macroLocation;
    int bunkerIndex = the.my.all.count(BWAPI::UnitTypes::Terran_Bunker);
    if (b.type == BWAPI::UnitTypes::Terran_Bunker)
    {
        // Construction tasks can outnumber started Bunkers. Give each task its
        // own index so two workers cannot both take the close first-Bunker role.
        for (const Building & earlier : _buildings)
        {
            if (&earlier == &b) break;
            if (earlier.type == b.type &&
                (earlier.status == BuildingStatus::Unassigned ||
                 earlier.status == BuildingStatus::Assigned))
                ++bunkerIndex;
        }
        if (bunkerIndex > 0 &&
            StrategyManager::Instance().isNoWallRushBunkerPlacementActive() &&
            !StrategyManager::Instance().getTerranDefensiveBunkerTile(bunkerIndex).isValid())
            return BWAPI::TilePositions::None;
    }

    if (loc == MacroLocation::Tile && b.finalPosition.isValid())
    {
        if (canClearExpansionFootprint(b, b.finalPosition))
        {
            return b.finalPosition;
        }
        if (b.type == BWAPI::UnitTypes::Terran_Bunker)
        {
            if (!StrategyManager::Instance().canBuildTerranDefensiveBunkerAt(b.finalPosition))
            {
                const BWAPI::TilePosition shelter = StrategyManager::Instance().getTerranDefensiveBunkerTile(
                    bunkerIndex);
                return shelter.isValid() && the.placer.buildingOK(b, shelter) &&
                    the.placer.preservesGroundExit(b.type, shelter)
                    ? shelter : BWAPI::TilePositions::None;
            }
            // A reserved front tile may become blocked before the builder
            // arrives. Re-select a defensive slot instead of allowing the
            // generic nearby search to move the Bunker behind the tank line.
            if (the.placer.buildingOK(b, b.finalPosition) &&
                the.placer.preservesGroundExit(b.type, b.finalPosition))
            {
                return b.finalPosition;
            }
            if (the.placer.hasTerranMainWall() &&
                !the.placer.isTerranMainWallReleased() &&
                b.finalPosition == the.placer.getTerranMainWallBunkerTile())
            {
                // Keep the first Bunker on its displayed wall reservation.
                // A passing Marine/SCV may block it for a frame; moving the
                // task to another shoulder leaves WALL BUNKER empty forever.
                return BWAPI::TilePositions::None;
            }
            const BWAPI::TilePosition frontTile =
                StrategyManager::Instance().getTerranDefensiveBunkerTile(
                    bunkerIndex);
            return frontTile.isValid() &&
                   the.placer.buildingOK(b, frontTile) &&
                   the.placer.preservesGroundExit(b.type, frontTile)
                ? frontTile : BWAPI::TilePositions::None;
        }
        const BWAPI::TilePosition candidateTile =
            the.placer.getBuildLocationNear(b, 0);
        return b.type != BWAPI::UnitTypes::Terran_Bunker ||
               (candidateTile.isValid() &&
                the.placer.preservesGroundExit(b.type, candidateTile))
            ? candidateTile : BWAPI::TilePositions::None;
    }

    // Generic Bunker goals (including BOSS and JSON) have no tile attached.
    // Always use the defensive front, including after a natural starts while
    // the main wall is still active. The generic search can place a Bunker
    // behind the tank rally or beside the mineral line.
    if (b.type == BWAPI::UnitTypes::Terran_Bunker &&
        the.selfRace() == BWAPI::Races::Terran &&
        (loc == MacroLocation::Anywhere || loc == MacroLocation::Main ||
         loc == MacroLocation::Front))
    {
        const BWAPI::TilePosition defensiveTile =
            StrategyManager::Instance().getTerranDefensiveBunkerTile(
                bunkerIndex);
        return defensiveTile.isValid()
            ? defensiveTile : BWAPI::TilePositions::None;
    }

    // A resource depot going Anywhere is really going to an expansion.
    if (b.type.isResourceDepot() && loc == MacroLocation::Anywhere)
    {
        loc = MacroLocation::Expo;
    }

    // A resource depot goes at an expansion location, except for certain non-base hatchery locations.
    // Only zerg macro hatcheries and proxy hatcheries are placed away from expo locations.
    if (b.type.isResourceDepot() &&
        b.macroLocation != MacroLocation::Main &&
        b.macroLocation != MacroLocation::Macro &&
        b.macroLocation != MacroLocation::Proxy &&
        b.macroLocation != MacroLocation::Front &&
        b.macroLocation != MacroLocation::Center)
    {
        BWAPI::TilePosition pos = the.placer.getExpoLocationTile(b.macroLocation);
        if (canClearExpansionFootprint(b, pos) ||
            (the.placer.buildingOK(b, pos) &&
             !the.groundHitsFixed.inRange(b.type, pos)))
        {
            return pos;
        }
        return BWAPI::TilePositions::None;
    }

    int distance = Config::Macro::BuildingSpacing;
    if (b.type == BWAPI::UnitTypes::Terran_Bunker ||
        b.type == BWAPI::UnitTypes::Terran_Missile_Turret)
    {
        // Pack defenses tightly together.
        distance = 0;
    }
    

    // The building placer does the rest.
    const BWAPI::TilePosition candidateTile =
        the.placer.getBuildLocationNear(b, distance);
    return b.type != BWAPI::UnitTypes::Terran_Bunker ||
           (candidateTile.isValid() &&
            the.placer.preservesGroundExit(b.type, candidateTile))
        ? candidateTile : BWAPI::TilePositions::None;
}

// The buildings failed or were canceled.
// Undo any connections with other data structures, then delete.
void BuildingManager::undoBuildings(const std::vector< std::reference_wrapper<Building> > & toRemove)
{
    for (Building & b : toRemove)
    {
        // If the building was to establish a base, unreserve the base location.
        if (b.type.isResourceDepot() && b.macroLocation != MacroLocation::Main && b.finalPosition.isValid())
        {
            Base * base = the.bases.getBaseAtTilePosition(b.finalPosition);
            if (base)
            {
                base->unreserve();
            }
        }

        releaseBuilderUnit(b);

        // If the building is not yet under construction, release its resources.
        if (b.status == BuildingStatus::Unassigned || b.status == BuildingStatus::Assigned)
        {
            _reservedMinerals -= b.type.mineralPrice();
            _reservedGas -= b.type.gasPrice();
        }

        // Free tiles that were reserved for the building.
        if (b.status == BuildingStatus::Assigned || b.status == BuildingStatus::UnderConstruction)
        {
            the.placer.freeTiles(b);
        }

        // Cancel a terran building under construction. Zerg and protoss finish on their own,
        // but a terran building will be abandoned unfinished unless canceled.
        if (b.buildingUnit &&
            b.buildingUnit->getType().getRace() == BWAPI::Races::Terran &&
            b.buildingUnit->exists() &&
            b.buildingUnit->canCancelConstruction())
        {
            the.micro.Cancel(b.buildingUnit);
        }
    }

    removeBuildings(toRemove);
}

// Remove buildings from the list of buildings--nothing more, nothing less.
void BuildingManager::removeBuildings(const std::vector< std::reference_wrapper<Building> > & toRemove)
{
    std::vector<size_t> indices;
    for (Building & b : toRemove)
    {
        for (size_t i = 0; i < _buildings.size(); ++i)
        {
            if (&_buildings[i] == &b)
            {
                indices.push_back(i);
                break;
            }
        }
    }

    // Building::operator== treats null unit pointers as equal. Use task
    // identity, and erase backwards only after collecting every index.
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        _buildings.erase(_buildings.begin() + *it);
    }
}

// Buildings of this type are stalled and can't be built yet.
// They are protoss buildings that require pylon power, and can be built after
// a pylon finishes and provides powered space.
bool BuildingManager::typeIsStalled(BWAPI::UnitType type) const
{
    return _stalledForLackOfSpace && type.requiresPsi();
}
