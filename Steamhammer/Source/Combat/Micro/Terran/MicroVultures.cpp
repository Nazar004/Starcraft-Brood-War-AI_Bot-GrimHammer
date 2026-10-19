#include "Combat/Micro/Terran/MicroVultures.h"
#include "Combat/Micro/Terran/TerranFormation.h"
#include "Combat/Micro/Terran/TerranSiegeSafety.h"
#include "Utilities/Random.h"
#include "Map/Bases.h"
#include "Intelligence/InformationManager.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"
#include "Economy/WorkerManager.h"
#include "Strategy/StrategyManager.h"
#include "Strategy/Terran/AirTanksPolicy.h"
#include "Strategy/Terran/AirTanksFormation.h"
#include "Combat/CombatCommander.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <set>

using namespace UAlbertaBot;

static BWAPI::Position clampToMap(const BWAPI::Position& p)
{
    const int maxX = BWAPI::Broodwar->mapWidth() * 32 - 1;
    const int maxY = BWAPI::Broodwar->mapHeight() * 32 - 1;

    const int x = std::max(0, std::min(maxX, p.x));
    const int y = std::max(0, std::min(maxY, p.y));

    BWAPI::Position result(x, y);

    if (result.isValid())
    {
        return result;
    }

    return BWAPI::Positions::Invalid;
}

static bool isWorkerHarassOrder(const SquadOrder* order)
{
    return order && (order->getRaid() ||
        order->getKey() == "airtanks-vulture-focus");
}

static bool isDroppedCombatRaid(const SquadOrder* order)
{
    return order && order->getKey() == "opportunistic-drop-raid";
}

// Only a deployed Tank needs a stationary Vulture vision screen. Anchoring
// parked Vultures to a moving Tank makes them occupy the lane it must use.
static BWAPI::Position frontTankAnchor(
    const UnitCluster& cluster, const BWAPI::Position& objective)
{
    if (!objective.isValid()) return BWAPI::Positions::Invalid;

    if (StrategyManager::Instance().airTanksPlanActive() &&
        AirTanks::variantFromName(StrategyManager::Instance().airTanksVariant()) ==
            AirTanks::Variant::TankWraith)
    {
        BWAPI::Unit lead = AirTanksFormation::frontTank(
            CombatCommander::Instance().groundSquadUnits());
        if (lead) return lead->getPosition();
    }

    BWAPI::Unit frontTank = nullptr;
    int bestDistance = MAX_DISTANCE;
    for (BWAPI::Unit unit : cluster.units)
    {
        if (!unit || !unit->exists() || !unit->isCompleted() ||
            !unit->getPosition().isValid() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            continue;
        }
        const int distance = unit->getDistance(objective);
        if (distance < bestDistance ||
            (distance == bestDistance && frontTank &&
             unit->getID() < frontTank->getID()))
        {
            frontTank = unit;
            bestDistance = distance;
        }
    }
    if (!frontTank && cluster.center.isValid())
    {
        // Fast Vultures can briefly become a separate spatial cluster. Keep
        // their front line tied to the nearby army instead of sending that
        // cluster alone toward the distant attack objective.
        int nearestDistance = 18 * 32 + 1;
        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists() || !unit->isCompleted() ||
                !unit->getPosition().isValid() ||
                unit->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            {
                continue;
            }
            const int distance = unit->getDistance(cluster.center);
            if (distance < nearestDistance ||
                (distance == nearestDistance && frontTank &&
                 unit->getID() < frontTank->getID()))
            {
                frontTank = unit;
                nearestDistance = distance;
            }
        }
    }
    return frontTank ? frontTank->getPosition() : BWAPI::Positions::Invalid;
}

static bool isGroundArmyEscortVulture(BWAPI::Unit vulture,
    const SquadOrder* squadOrder)
{
    if (!vulture || !squadOrder ||
        (squadOrder->getType() != SquadOrderTypes::Attack &&
         squadOrder->getType() != SquadOrderTypes::OmniAttack &&
         squadOrder->getType() != SquadOrderTypes::Defend) ||
        isWorkerHarassOrder(squadOrder) || isDroppedCombatRaid(squadOrder) ||
        squadOrder->getKey() == "air target" ||
        TerranFormation::isBaseDefenseOrder(*squadOrder) ||
        StrategyManager::Instance().getTerranArmyPosture() ==
            TerranArmyPosture::PureAir)
    {
        return false;
    }

    const Squad* owner = CombatCommander::Instance().squadDiagnostics().
        getUnitSquad(vulture);
    return owner && owner->getName() == "Ground";
}

static bool isGroundArmyCore(BWAPI::Unit unit)
{
    return unit && unit->exists() && unit->isCompleted() &&
        !unit->isLoaded() && !unit->isFlying() &&
        unit->getPosition().isValid() &&
        unit->getType() != BWAPI::UnitTypes::Terran_Vulture &&
        !unit->getType().isWorker() && !unit->getType().isBuilding() &&
        UnitUtil::CanAttackGround(unit);
}

// A Vulture screens the main ground force even when that force has no deployed
// Tank (bio, mobile mech, or the first moving Tank). Use the nearest coherent
// group, not a lone reinforcement at the Factory or the distant attack goal.
static BWAPI::Position frontGroundArmyAnchor(
    const UnitCluster& cluster, const BWAPI::Position& objective)
{
    if (!cluster.center.isValid() || !objective.isValid())
        return BWAPI::Positions::Invalid;

    const bool tankWraith = StrategyManager::Instance().airTanksPlanActive() &&
        AirTanks::variantFromName(StrategyManager::Instance().airTanksVariant()) ==
            AirTanks::Variant::TankWraith;
    bool localDeployedTank = false;
    for (BWAPI::Unit unit : cluster.units)
        if (unit && unit->exists() && unit->isCompleted() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        { localDeployedTank = true; break; }
    if (tankWraith || localDeployedTank)
    {
        const BWAPI::Position tank = frontTankAnchor(cluster, objective);
        if (tank.isValid()) return tank;
    }

    BWAPI::Position routeReference = cluster.center;
    for (BWAPI::Unit unit : cluster.units)
        if (unit && unit->exists() && unit->getPosition().isValid() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        { routeReference = unit->getPosition(); break; }

    const BWAPI::Unitset& ground =
        CombatCommander::Instance().groundSquadUnits();
    BWAPI::Unit seed = nullptr;
    int nearest = MAX_DISTANCE;
    for (BWAPI::Unit unit : cluster.units)
    {
        if (!isGroundArmyCore(unit) || !ground.count(unit)) continue;
        const int distance = unit->getDistance(cluster.center);
        if (distance < nearest ||
            (distance == nearest && seed && unit->getID() < seed->getID()))
        {
            seed = unit;
            nearest = distance;
        }
    }
    if (!seed)
    {
        // A Vulture-only cluster may contain a new reinforcement. Follow the
        // forward army group instead of latching onto a lone unit at home.
        std::vector<BWAPI::Unit> core;
        for (BWAPI::Unit unit : ground)
            if (isGroundArmyCore(unit) &&
                the.partitions.connected(routeReference, unit->getPosition()))
                core.push_back(unit);

        int bestTier = -1;
        int bestProgress = MAX_DISTANCE;
        for (BWAPI::Unit unit : core)
        {
            bool grouped = false;
            for (BWAPI::Unit other : core)
                if (other != unit && unit->getDistance(other) <= 8 * 32)
                { grouped = true; break; }
            const int tier = grouped ? 1 : 0;
            const int progress = unit->getDistance(objective);
            const int distance = unit->getDistance(cluster.center);
            if (tier > bestTier ||
                (tier == bestTier && (progress < bestProgress ||
                 (progress == bestProgress && distance < nearest))))
            {
                seed = unit;
                bestTier = tier;
                bestProgress = progress;
                nearest = distance;
            }
        }
    }
    if (!seed) return BWAPI::Positions::Invalid;

    long long x = 0, y = 0;
    int count = 0;
    for (BWAPI::Unit unit : ground)
    {
        if (!isGroundArmyCore(unit) || unit->getDistance(seed) > 8 * 32 ||
            !the.partitions.connected(seed->getPosition(), unit->getPosition()))
            continue;
        x += unit->getPosition().x;
        y += unit->getPosition().y;
        ++count;
    }
    if (count <= 1) return seed->getPosition();

    // A group center is steadier than switching to whichever Marine or
    // Goliath happens to be one pixel ahead on this frame.
    const BWAPI::Position center(int(x / count), int(y / count));
    const double dx = double(objective.x - center.x);
    const double dy = double(objective.y - center.y);
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length < 32.0) return center;
    return BWAPI::Position(
        int(center.x + dx * 32 / length),
        int(center.y + dy * 32 / length));
}

namespace
{
    enum StrategicMineKind
    {
        MineChoke,
        MineTankLine,
        MineRetreat,
        MineReinforcement,
        MineExpansion,
        MineFlank
    };

    struct StrategicMineCandidate
    {
        BWAPI::Position position;
        int priority;
        StrategicMineKind kind;
    };

    std::vector<StrategicMineCandidate> strategicMineCandidates;
    int strategicMineCandidateFrame = -999999;
    unsigned int strategicMineTopologyRevision = 0;

    const char* strategicMineKindName(StrategicMineKind kind)
    {
        switch (kind)
        {
        case MineChoke:         return "choke";
        case MineTankLine:      return "tank line";
        case MineRetreat:       return "retreat";
        case MineReinforcement: return "reinforce";
        case MineExpansion:     return "expansion";
        case MineFlank:         return "flank";
        default:                return "field";
        }
    }

    bool mineBodyFits(const BWAPI::Position& position)
    {
        if (!position.isValid()) return false;

        const BWAPI::UnitType body = BWAPI::UnitTypes::Terran_Vulture;
        const int left = position.x - body.dimensionLeft();
        const int right = position.x + body.dimensionRight();
        const int top = position.y - body.dimensionUp();
        const int bottom = position.y + body.dimensionDown();
        if (left < 0 || top < 0 ||
            right >= BWAPI::Broodwar->mapWidth() * 32 ||
            bottom >= BWAPI::Broodwar->mapHeight() * 32)
        {
            return false;
        }

        for (int x = left / 8; x <= right / 8; ++x)
            for (int y = top / 8; y <= bottom / 8; ++y)
                if (!BWAPI::Broodwar->isWalkable(x, y)) return false;

        const BWAPI::TilePosition tile(position);
        return tile.isValid() &&
            the.map.isRuntimeWalkable(tile) &&
            !the.placer.isReserved(tile);
    }

    bool minePointKeepsInfrastructureClear(const BWAPI::Position& position)
    {
        if (!position.isValid()) return false;

        const BWAPI::Position wallGate = the.placer.getTerranMainWallGateCenter();
        if (wallGate.isValid() &&
            position.getApproxDistance(wallGate) <= 3 * 32)
        {
            return false;
        }

        // Keep depot footprints, mineral lines and future expansions clear.
        // Own units can technically pass a buried mine, but a triggered mine at
        // one of these traffic hubs would punish our economy and wall traffic.
        for (Base* base : the.bases.getAll())
        {
            if (!base || !base->getCenter().isValid()) continue;
            if (position.getApproxDistance(base->getCenter()) <= 6 * 32)
                return false;

            for (BWAPI::Unit mineral : base->getMinerals())
                if (mineral && mineral->exists() &&
                    mineral->getDistance(position) <= 3 * 32)
                    return false;
            for (BWAPI::Unit geyser : base->getGeysers())
                if (geyser && geyser->exists() &&
                    geyser->getDistance(position) <= 3 * 32)
                    return false;
        }

        return true;
    }

    BWAPI::Position snapStrategicMinePoint(const BWAPI::Position& desired)
    {
        const BWAPI::Position clamped = clampToMap(desired);
        if (!clamped.isValid()) return BWAPI::Positions::Invalid;

        const BWAPI::TilePosition origin(clamped);
        for (int radius = 0; radius <= 4; ++radius)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                for (int dy = -radius; dy <= radius; ++dy)
                {
                    if (radius > 0 && std::abs(dx) != radius &&
                        std::abs(dy) != radius)
                    {
                        continue;
                    }

                    const BWAPI::TilePosition tile(origin.x + dx, origin.y + dy);
                    if (!tile.isValid()) continue;
                    const BWAPI::Position position =
                        BWAPI::Position(tile) + BWAPI::Position(16, 16);
                    if (mineBodyFits(position) &&
                        minePointKeepsInfrastructureClear(position))
                    {
                        return position;
                    }
                }
            }
        }

        return BWAPI::Positions::Invalid;
    }

    void addStrategicMineCandidate(
        const BWAPI::Position& desired,
        int priority,
        StrategicMineKind kind)
    {
        const BWAPI::Position position = snapStrategicMinePoint(desired);
        if (!position.isValid()) return;

        Base* main = the.bases.myMain();
        if (main && main->getCenter().isValid() &&
            !the.partitions.connected(main->getCenter(), position))
        {
            return;
        }

        for (StrategicMineCandidate& existing : strategicMineCandidates)
        {
            if (existing.position.getApproxDistance(position) < 3 * 32)
            {
                if (priority > existing.priority)
                {
                    existing.position = position;
                    existing.priority = priority;
                    existing.kind = kind;
                }
                return;
            }
        }

        strategicMineCandidates.push_back({ position, priority, kind });
    }

    void addChokeMinefield(
        const ChokePoint* choke,
        const BWAPI::Position& protectedSide,
        int priority,
        StrategicMineKind kind)
    {
        if (!choke || !choke->center().isValid() ||
            !protectedSide.isValid())
        {
            return;
        }

        const BWAPI::Position center = choke->center();
        double dx = double(center.x - protectedSide.x);
        double dy = double(center.y - protectedSide.y);
        double length = std::sqrt(dx * dx + dy * dy);
        if (length < 1.0) return;
        dx /= length;
        dy /= length;
        const double sideX = -dy;
        const double sideY = dx;
        // Keep the three points far enough apart for Micro::LaySpiderMine's
        // shared three-tile safety rule. The previous 56px minimum collapsed a
        // narrow choke back to one candidate during de-duplication, so an
        // important entrance could never become an actual minefield.
        const int side = std::max(96, std::min(160,
            choke->widthPixels() / 2 + 32));

        // The center point lies outside the actual choke. Side points make a
        // shallow fan, so separate Vultures cover the route without stacking.
        addStrategicMineCandidate(
            BWAPI::Position(
                int(center.x + dx * 4 * 32),
                int(center.y + dy * 4 * 32)),
            priority,
            kind);
        addStrategicMineCandidate(
            BWAPI::Position(
                int(center.x + dx * 3 * 32 + sideX * side),
                int(center.y + dy * 3 * 32 + sideY * side)),
            priority - 4,
            MineFlank);
        addStrategicMineCandidate(
            BWAPI::Position(
                int(center.x + dx * 3 * 32 - sideX * side),
                int(center.y + dy * 3 * 32 - sideY * side)),
            priority - 4,
            MineFlank);
    }

    void refreshStrategicMineCandidates()
    {
        const int now = the.now();
        const unsigned int topology = the.mapTopologyRevision();
        if (strategicMineCandidateFrame <= now &&
            now - strategicMineCandidateFrame < 4 * 24 &&
            strategicMineTopologyRevision == topology)
        {
            return;
        }

        strategicMineCandidateFrame = now;
        strategicMineTopologyRevision = topology;
        strategicMineCandidates.clear();

        Base* main = the.bases.myMain();
        if (!main || !main->getCenter().isValid()) return;
        const BWAPI::Position home = main->getCenter();

        // Secure the first expansion's approach even before its Command Center
        // finishes. Mines go on the enemy-facing side of the choke, away from
        // the depot footprint and our mineral traffic.
        Base* natural = the.bases.myNatural();
        if (natural && natural->getOwner() != the.enemy())
        {
            addChokeMinefield(
                natural->getFrontChoke(), natural->getCenter(),
                126, MineChoke);
        }

        // Other owned chokes also get an approach net.
        Base* front = the.bases.myFront();
        for (Base* base : the.bases.getAll())
        {
            if (!base || base->getOwner() != the.self() ||
                !base->getCenter().isValid())
            {
                continue;
            }
            const int priority = base == front ? 112 :
                (base == natural ? 106 : 98);
            addChokeMinefield(
                base->getFrontChoke(), base->getCenter(), priority, MineChoke);
        }

        BWAPI::Position enemyReference(
            BWAPI::Broodwar->mapWidth() * 16,
            BWAPI::Broodwar->mapHeight() * 16);
        if (the.bases.enemyStart() &&
            the.bases.enemyStart()->getCenter().isValid())
        {
            enemyReference = the.bases.enemyStart()->getCenter();
        }

        // Chokes on our retreat path are also the paths enemy reinforcements
        // must cross in the opposite direction.
        const BWAPI::Position routeStart =
            front && front->getCenter().isValid() ? front->getCenter() : home;
        const std::vector<const ChokePoint*> route =
            the.zone.findChokePath(routeStart, enemyReference);
        for (size_t i = 0; i < route.size(); ++i)
        {
            const bool homeHalf = i * 2 < route.size();
            addChokeMinefield(
                route[i], routeStart,
                homeHalf ? 94 - int(i) * 2 : 86 - int(i),
                homeHalf ? MineRetreat : MineReinforcement);
        }

        // Form a separate screen for each settled Tank group. Averaging every
        // Tank on the map put the old screen in empty ground between the main
        // base and the actual front when the army was split.
        struct TankGroup
        {
            long long x = 0;
            long long y = 0;
            int count = 0;
            int sieged = 0;
        };
        std::vector<TankGroup> tankGroups;
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (!unit || !unit->exists() || !unit->isCompleted() ||
                !unit->getPosition().isValid()) continue;
            const BWAPI::UnitType type = unit->getType();
            if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            {
                TankGroup* nearest = nullptr;
                int nearestDistance = 10 * 32;
                for (TankGroup& group : tankGroups)
                {
                    const BWAPI::Position center(
                        int(group.x / group.count),
                        int(group.y / group.count));
                    const int distance = center.getApproxDistance(unit->getPosition());
                    if (distance < nearestDistance)
                    {
                        nearest = &group;
                        nearestDistance = distance;
                    }
                }
                if (!nearest)
                {
                    tankGroups.emplace_back();
                    nearest = &tankGroups.back();
                }
                nearest->x += unit->getPosition().x;
                nearest->y += unit->getPosition().y;
                ++nearest->count;
                if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
                    ++nearest->sieged;
            }
        }
        for (const TankGroup& group : tankGroups)
        {
            if (group.count < 2 || group.sieged == 0) continue;
            const BWAPI::Position tankCenter(
                int(group.x / group.count), int(group.y / group.count));
            double dx = double(enemyReference.x - tankCenter.x);
            double dy = double(enemyReference.y - tankCenter.y);
            double length = std::sqrt(dx * dx + dy * dy);
            if (length >= 1.0)
            {
                dx /= length;
                dy /= length;
                const double sideX = -dy;
                const double sideY = dx;
                for (int sign : { -1, 0, 1 })
                {
                    addStrategicMineCandidate(
                        BWAPI::Position(
                            int(tankCenter.x + dx * 6 * 32 + sideX * sign * 3 * 32),
                            int(tankCenter.y + dy * 6 * 32 + sideY * sign * 3 * 32)),
                        118 - std::abs(sign) * 3,
                        MineTankLine);
                }
            }
        }

        // Cover approaches to untaken expansions without putting a mine on the
        // Command Center footprint or mineral traffic lane.
        for (Base* base : the.bases.getAll())
        {
            if (!base || base->getOwner() != the.neutral() ||
                !base->getCenter().isValid() || !base->getFront().isValid())
            {
                continue;
            }
            const BWAPI::Position center = base->getCenter();
            const BWAPI::Position frontPosition = base->getFront();
            double dx = double(frontPosition.x - center.x);
            double dy = double(frontPosition.y - center.y);
            double length = std::sqrt(dx * dx + dy * dy);
            if (length < 1.0) continue;
            dx /= length;
            dy /= length;
            addStrategicMineCandidate(
                BWAPI::Position(
                    int(frontPosition.x + dx * 4 * 32),
                    int(frontPosition.y + dy * 4 * 32)),
                58,
                MineExpansion);
        }

        std::sort(
            strategicMineCandidates.begin(), strategicMineCandidates.end(),
            [](const StrategicMineCandidate& a,
               const StrategicMineCandidate& b)
            {
                if (a.priority != b.priority) return a.priority > b.priority;
                if (a.position.x != b.position.x) return a.position.x < b.position.x;
                return a.position.y < b.position.y;
            });
        if (strategicMineCandidates.size() > 72)
            strategicMineCandidates.resize(72);
    }

    bool strategicMinePointOccupied(const BWAPI::Position& position)
    {
        for (BWAPI::Unit friendly : the.self()->getUnits())
        {
            if (!friendly || !friendly->exists()) continue;
            if (friendly->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine &&
                friendly->getDistance(position) < 3 * 32)
            {
                return true;
            }
            if (friendly->getType() == BWAPI::UnitTypes::Terran_Vulture)
            {
                const BWAPI::UnitCommand command = friendly->getLastCommand();
                if (command.getType() == BWAPI::UnitCommandTypes::Use_Tech_Position &&
                    command.getTechType() == BWAPI::TechTypes::Spider_Mines &&
                    command.getTargetPosition().isValid() &&
                    command.getTargetPosition().getApproxDistance(position) < 3 * 32 &&
                    (friendly->getOrder() == BWAPI::Orders::PlaceMine ||
                     the.now() - friendly->getLastCommandFrame() <= 4 * 24))
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool strategicMineRouteIsSafe(
        BWAPI::Unit vulture,
        const BWAPI::Position& position)
    {
        if (!vulture || !vulture->exists() || !position.isValid() ||
            !the.partitions.connected(vulture->getPosition(), position) ||
            the.groundHits(BWAPI::TilePosition(position)) > 0)
        {
            return false;
        }

        if (the.placer.hasTerranMainWall() &&
            the.placer.isTerranMainWallComplete() &&
            the.placer.crossesTerranMainWall(
                vulture->getPosition(), position))
        {
            return false;
        }

        for (BWAPI::Unit enemy : the.enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                !enemy->getPosition().isValid() || enemy->isFlying() ||
                !UnitUtil::CanAttackGround(enemy))
            {
                continue;
            }
            if (enemy->getDistance(position) <= 10 * 32)
                return false;
        }

        std::vector<BWAPI::TilePosition> path;
        if (!the.map.getGroundPath(
                vulture->getTilePosition(), BWAPI::TilePosition(position),
                path, 24, false) || path.size() > 16)
        {
            return false;
        }
        for (const BWAPI::TilePosition& tile : path)
        {
            if (!tile.isValid() || the.groundHits(tile) > 0)
                return false;
        }
        return true;
    }
}



static bool isWorkerUnitType(BWAPI::UnitType type)
{
    return
        type == BWAPI::UnitTypes::Terran_SCV ||
        type == BWAPI::UnitTypes::Protoss_Probe ||
        type == BWAPI::UnitTypes::Zerg_Drone;
}

// The unit's ranged ground weapon does splash damage, so it works under dark swarm.
// Firebats are not here: They are melee units.
// Tanks and lurkers are not here: They have their own micro managers.



MicroVultures::MicroVultures()
{ 
}
bool MicroVultures::tryLaySpiderMineOnRetreat(BWAPI::Unit vulture, BWAPI::Unit target)
{
    if (!vulture || !vulture->exists() || !target || !target->exists())
    {
        return false;
    }

    if (vulture->getType() != BWAPI::UnitTypes::Terran_Vulture)
    {
        return false;
    }

    if (!vulture->getPlayer()->hasResearched(BWAPI::TechTypes::Spider_Mines))
    {
        return false;
    }

    if (vulture->getSpiderMineCount() <= 0 || vulture->isAttackFrame() || vulture->isStartingAttack())
    {
        return false;
    }

    const BWAPI::UnitType type = target->getType();
    // Hovering units/workers cannot trigger a mine. Do not stop kiting to lay
    // one at point-blank range, or chase a target to spend the last mine.
    if (!target->isVisible() || !target->isDetected() || target->isFlying() ||
        target->isBurrowed() || type.isBuilding() || type.isWorker() || type.isSpell() ||
        type == BWAPI::UnitTypes::Terran_Vulture || type == BWAPI::UnitTypes::Protoss_Archon ||
        type == BWAPI::UnitTypes::Protoss_Dark_Archon ||
        type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
        vulture->getDistance(target) < 3 * 32 || vulture->getDistance(target) > 6 * 32)
        return false;

    const int mineCooldown = 24 * 4;  
    const int now = BWAPI::Broodwar->getFrameCount();

    std::map<int, int>::iterator it = _lastRetreatMineFrame.find(vulture->getID());
    if (it != _lastRetreatMineFrame.end())
    {
        if (now - it->second < mineCooldown)
        {
            return false;
        }
    }

    BWAPI::Position selfPos = vulture->getPosition();
    if (!selfPos.isValid())
    {
        return false;
    }

    if (!vulture->canUseTech(BWAPI::TechTypes::Spider_Mines, selfPos))
    {
        return false;
    }

    DrawUnitTargetTextMap(
        vulture->getPosition() + BWAPI::Position(0, 8),
        "mine retreat"
    );

    if (!the.micro.LaySpiderMine(vulture, selfPos)) return false;
    _lastRetreatMineFrame[vulture->getID()] = now;
    return true;
}

bool MicroVultures::tryLayStrategicMine(BWAPI::Unit vulture)
{
    const int now = the.now();
    if (!vulture || !vulture->exists() || !vulture->isCompleted() ||
        vulture->isLoaded() || !vulture->getPosition().isValid() ||
        vulture->getType() != BWAPI::UnitTypes::Terran_Vulture ||
        !order || !order->isCombatOrder() || isWorkerHarassOrder(order) ||
        isDroppedCombatRaid(order) ||
        WorkerManager::Instance().isMobileRepairTarget(vulture) ||
        !vulture->getPlayer()->hasResearched(BWAPI::TechTypes::Spider_Mines) ||
        vulture->getSpiderMineCount() <= 0 ||
        vulture->isUnderAttack() || the.hits(vulture) > 0 ||
        vulture->isAttackFrame() || vulture->isStartingAttack())
    {
        return false;
    }

    const int id = vulture->getID();
    const BWAPI::UnitCommand current = vulture->getLastCommand();
    if (vulture->getOrder() == BWAPI::Orders::PlaceMine ||
        (current.getType() == BWAPI::UnitCommandTypes::Use_Tech_Position &&
         current.getTechType() == BWAPI::TechTypes::Spider_Mines &&
         now - vulture->getLastCommandFrame() <= 2 * 24))
    {
        return false;
    }

    const auto nextIt = _nextStrategicMineCheckFrame.find(id);
    if (nextIt != _nextStrategicMineCheckFrame.end() && now < nextIt->second)
    {
        return false;
    }
    _nextStrategicMineCheckFrame[id] = now + 24 + id % 12;

    if (isGroundArmyEscortVulture(vulture, order))
        for (BWAPI::Unit friendly :
            CombatCommander::Instance().groundSquadUnits())
            if (friendly && friendly->exists() && friendly->isUnderAttack() &&
                !friendly->getType().isWorker() &&
                !friendly->getType().isBuilding() &&
                friendly->getDistance(vulture) <= 10 * 32)
                return false;

    // Strategic placement never wins over a nearby fight. An unused final
    // charge is worth more in a safe approach net than in the Vulture's hold.
    for (BWAPI::Unit enemy : the.enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            !enemy->getPosition().isValid() || enemy->isFlying() ||
            !UnitUtil::CanAttackGround(enemy))
        {
            continue;
        }
        if (enemy->getDistance(vulture) <= 8 * 32)
            return false;
    }

    refreshStrategicMineCandidates();
    std::vector<std::pair<int, size_t>> ranked;
    ranked.reserve(strategicMineCandidates.size());
    for (size_t i = 0; i < strategicMineCandidates.size(); ++i)
    {
        const StrategicMineCandidate& candidate = strategicMineCandidates[i];
        const int distance = vulture->getDistance(candidate.position);
        if (distance > 12 * 32 ||
            strategicMinePointOccupied(candidate.position))
        {
            continue;
        }
        // Priority dominates distance. The small stable ID/index term spreads
        // simultaneous Vultures over equal-scored flank points.
        const int spread = int((i + size_t(id)) % 3) * 24;
        ranked.push_back(std::make_pair(
            candidate.priority * 1024 - distance - spread, i));
    }

    std::sort(ranked.begin(), ranked.end(),
        [](const std::pair<int, size_t>& a,
           const std::pair<int, size_t>& b)
        {
            return a.first > b.first;
        });

    const size_t checks = std::min<size_t>(ranked.size(), 8);
    for (size_t n = 0; n < checks; ++n)
    {
        const StrategicMineCandidate& candidate =
            strategicMineCandidates[ranked[n].second];
        if (!strategicMineRouteIsSafe(vulture, candidate.position))
            continue;
        if (!the.micro.LaySpiderMine(vulture, candidate.position))
            continue;

        _nextStrategicMineCheckFrame[id] = now + 2 * 24;
        DrawUnitTargetTextMap(
            vulture->getPosition() + BWAPI::Position(0, 8),
            "MINEFIELD %s", strategicMineKindName(candidate.kind));
        if (Config::Debug::DrawUnitTargets)
        {
            BWAPI::Broodwar->drawLineMap(
                vulture->getPosition(), candidate.position,
                BWAPI::Colors::Green);
            BWAPI::Broodwar->drawCircleMap(
                candidate.position, 3 * 32,
                BWAPI::Colors::Green, false);
        }
        return true;
    }

    // If the prepared choke and tank positions are not on this march, lay a
    // mine a few tiles ahead on the way to the attack objective. This uses a
    // short local detour check below rather than building another full-map
    // distance field for each Vulture and frame.
    if ((order->getType() == SquadOrderTypes::Attack ||
         order->getType() == SquadOrderTypes::OmniAttack) &&
        vulture->isMoving() &&
        order->getPosition().isValid() &&
        vulture->getDistance(order->getPosition()) > 14 * 32)
    {
        const BWAPI::Position start = vulture->getPosition();
        double dx = double(order->getPosition().x - start.x);
        double dy = double(order->getPosition().y - start.y);
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length >= 1.0)
        {
            dx /= length;
            dy /= length;
            // Do not interrupt a retreat just because the squad's attack
            // objective remains on the other side of the battlefield.
            if (dx * vulture->getVelocityX() +
                dy * vulture->getVelocityY() <= 0.5)
            {
                return false;
            }
            for (int side : { 0, 1, -1 })
            {
                const BWAPI::Position desired(
                    int(start.x + dx * 4 * 32 - dy * side * 3 * 32),
                    int(start.y + dy * 4 * 32 + dx * side * 3 * 32));
                const BWAPI::Position point = snapStrategicMinePoint(desired);
                if (!point.isValid() ||
                    (point.x - start.x) * dx +
                        (point.y - start.y) * dy < 2 * 32 ||
                    start.getApproxDistance(point) > 8 * 32 ||
                    strategicMinePointOccupied(point) ||
                    !strategicMineRouteIsSafe(vulture, point) ||
                    !the.micro.LaySpiderMine(vulture, point))
                {
                    continue;
                }

                _nextStrategicMineCheckFrame[id] = now + 2 * 24;
                DrawUnitTargetTextMap(
                    start + BWAPI::Position(0, 8), "MINEFIELD route");
                return true;
            }
        }
    }

    return false;
}


BWAPI::Unit MicroVultures::getBestWorkerHarassTarget(BWAPI::Unit rangedUnit) const
{
    if (!rangedUnit || !order || !isWorkerHarassOrder(order))
    {
        return nullptr;
    }

    BWAPI::Position center = order->getPosition().isValid() ? order->getPosition() : rangedUnit->getPosition();

    BWAPI::Unit best = nullptr;
    int bestDist = 999999;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
        center,
        12 * 32,
        BWAPI::Filter::IsEnemy &&
        !BWAPI::Filter::IsFlyer))
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            !enemy->isDetected() || !enemy->getPosition().isValid())
        {
            continue;
        }

        if (!isWorkerUnitType(enemy->getType()))
        {
            continue;
        }

        int dist = rangedUnit->getDistance(enemy->getPosition());
        if (dist > 9 * 32)
            continue;
        if (dist < bestDist)
        {
            bestDist = dist;
            best = enemy;
        }
    }

    return best;
}

void MicroVultures::cleanVultureState()
{
    const int now = BWAPI::Broodwar->getFrameCount();
    if (_lastStateFrame == now) return;
    if (_lastStateFrame > now)
    {
        _lastRetreatMineFrame.clear();
        _mineOrderStartFrame.clear();
        _mineOrderStartCount.clear();
        _mineRetryBlockedUntil.clear();
        _nextStrategicMineCheckFrame.clear();
    }
    _lastStateFrame = now;

    std::set<int> liveVultureIDs;
    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (unit &&
            unit->exists() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        {
            liveVultureIDs.insert(unit->getID());
        }
    }

    for (auto it = _lastRetreatMineFrame.begin(); it != _lastRetreatMineFrame.end(); )
    {
        if (liveVultureIDs.find(it->first) == liveVultureIDs.end())
        {
            it = _lastRetreatMineFrame.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto* state : {
        &_mineOrderStartFrame,
        &_mineOrderStartCount,
        &_mineRetryBlockedUntil,
        &_nextStrategicMineCheckFrame })
    {
        for (auto it = state->begin(); it != state->end(); )
        {
            if (liveVultureIDs.find(it->first) == liveVultureIDs.end())
            {
                it = state->erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

bool MicroVultures::monitorVultureMineOrder(
    BWAPI::Unit vulture,
    bool& retryBlocked)
{
    retryBlocked = false;
    if (!vulture || !vulture->exists() ||
        vulture->getType() != BWAPI::UnitTypes::Terran_Vulture)
    {
        return false;
    }

    const int now = the.now();
    const int id = vulture->getID();
    const BWAPI::UnitCommand command = vulture->getLastCommand();
    // A distant placement can travel for more than twelve frames before the
    // unit enters PlaceMine. Keep its command lease until the mine is spent or
    // the placement really times out, instead of letting combat micro replace
    // the command halfway to the selected point.
    const bool placingMine =
        vulture->getOrder() == BWAPI::Orders::PlaceMine ||
        (command.getType() == BWAPI::UnitCommandTypes::Use_Tech_Position &&
         command.getTechType() == BWAPI::TechTypes::Spider_Mines &&
         now - vulture->getLastCommandFrame() <= 4 * 24);

    if (placingMine)
    {
        if (_mineOrderStartFrame.find(id) == _mineOrderStartFrame.end())
        {
            _mineOrderStartFrame[id] = now;
            _mineOrderStartCount[id] = vulture->getSpiderMineCount();
        }

        const bool mineWasPlaced =
            vulture->getSpiderMineCount() < _mineOrderStartCount[id];
        const bool placementTimedOut =
            !mineWasPlaced && now - _mineOrderStartFrame[id] >= 4 * 24;

        if (placementTimedOut)
        {
            _mineRetryBlockedUntil[id] = now + 5 * 24;
            _mineOrderStartFrame.erase(id);
            _mineOrderStartCount.erase(id);

            BWAPI::Position failedPosition = command.getTargetPosition();
            BWAPI::Position recovery = vulture->getPosition();
            if (!failedPosition.isValid())
            {
                const int mapCenterX = BWAPI::Broodwar->mapWidth() * 16;
                failedPosition = BWAPI::Position(
                    recovery.x + (recovery.x < mapCenterX ? -32 : 32),
                    recovery.y);
            }
            if (failedPosition.isValid())
            {
                double dx = double(recovery.x - failedPosition.x);
                double dy = double(recovery.y - failedPosition.y);
                double length = std::sqrt(dx * dx + dy * dy);
                if (length < 1.0)
                {
                    dx = 1.0;
                    dy = 0.0;
                    length = 1.0;
                }
                recovery = clampToMap(BWAPI::Position(
                    int(recovery.x + dx / length * 3 * 32),
                    int(recovery.y + dy / length * 3 * 32)));
            }
            if (recovery.isValid()) the.micro.Move(vulture, recovery);
            return true;
        }

        if (!mineWasPlaced) return true;
        if (vulture->getOrder() == BWAPI::Orders::PlaceMine &&
            now - _mineOrderStartFrame[id] < 4 * 24)
            return true;
        _mineOrderStartFrame.erase(id);
        _mineOrderStartCount.erase(id);
    }
    else
    {
        _mineOrderStartFrame.erase(id);
        _mineOrderStartCount.erase(id);
    }

    const auto blockedIt = _mineRetryBlockedUntil.find(id);
    if (blockedIt != _mineRetryBlockedUntil.end())
    {
        if (now < blockedIt->second)
            retryBlocked = true;
        else
            _mineRetryBlockedUntil.erase(blockedIt);
    }
    return false;
}

void MicroVultures::executeStrategicMines(const UnitCluster& cluster)
{
    if (the.self()->getRace() != BWAPI::Races::Terran ||
        !order || !order->isCombatOrder() || isWorkerHarassOrder(order) ||
        isDroppedCombatRaid(order))
    {
        return;
    }

    bool hasVulture = false;
    for (BWAPI::Unit unit : cluster.units)
    {
        if (unit && unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        {
            hasVulture = true;
            break;
        }
    }
    if (!hasVulture) return;

    cleanVultureState();
    const BWAPI::Unitset units = Intersection(getUnits(), cluster.units);
    std::vector<BWAPI::Unit> vultures;
    bool mineInProgress = false;
    for (BWAPI::Unit unit : units)
    {
        if (!unit || !unit->exists() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Vulture)
        {
            continue;
        }

        bool retryBlocked = false;
        if (monitorVultureMineOrder(unit, retryBlocked))
        {
            mineInProgress = true;
        }
        else if (!retryBlocked)
        {
            vultures.push_back(unit);
        }
    }
    if (mineInProgress || vultures.empty()) return;

    // A single miner completes its first two placements before another one
    // leaves the formation. The remaining charge is considered only after
    // every available Vulture in this cluster has used its normal allowance.
    std::sort(vultures.begin(), vultures.end(),
        [](BWAPI::Unit a, BWAPI::Unit b) { return a->getID() < b->getID(); });
    bool hasNormalAmmo = false;
    for (BWAPI::Unit vulture : vultures)
        if (vulture->getSpiderMineCount() > 1)
            hasNormalAmmo = true;

    for (BWAPI::Unit vulture : vultures)
    {
        if (hasNormalAmmo != (vulture->getSpiderMineCount() > 1))
            continue;
        if (tryLayStrategicMine(vulture))
            break;
    }
}

bool MicroVultures::HasPriorityMineCommand(BWAPI::Unit unit)
{
    if (!unit || !unit->exists() ||
        unit->getType() != BWAPI::UnitTypes::Terran_Vulture)
    {
        return false;
    }

    // Strategic mine control runs before generic cluster movement. Preserve
    // a live mine order and the command issued earlier in this frame.
    if (unit->getLastCommandFrame() >= the.now()) return true;
    if (unit->getOrder() == BWAPI::Orders::PlaceMine) return true;

    const BWAPI::UnitCommand command = unit->getLastCommand();
    return
        command.getType() == BWAPI::UnitCommandTypes::Use_Tech_Position &&
        command.getTechType() == BWAPI::TechTypes::Spider_Mines &&
        the.now() - unit->getLastCommandFrame() <= 4 * 24;
}

void MicroVultures::executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster)
{
    cleanVultureState();

    BWAPI::Unitset units = Intersection(getUnits(), cluster.units);
    if (units.empty())
    {
        return;
    }
    assignTargets(units, targets, cluster);
}

void MicroVultures::regroup(const BWAPI::Position & regroupPosition,
    const UnitCluster & cluster)
{
    if (!regroupPosition.isValid()) return;

    std::vector<BWAPI::Unit> vultures;
    for (BWAPI::Unit unit : Intersection(getUnits(), cluster.units))
    {
        if (unit && unit->exists() && unit->getPosition().isValid())
            vultures.push_back(unit);
    }
    std::sort(vultures.begin(), vultures.end(),
        [](BWAPI::Unit a, BWAPI::Unit b) { return a->getID() < b->getID(); });

    for (int index = 0; index < int(vultures.size()); ++index)
    {
        BWAPI::Unit vulture = vultures[index];
        bool retryBlocked = false;
        if (!vulture->isUnderAttack() && the.hits(vulture) <= 0 &&
            monitorVultureMineOrder(vulture, retryBlocked))
        {
            // Complete a safe placement instead of changing its destination
            // every time the cluster briefly enters Regroup.
            continue;
        }
        if (the.micro.fleeDT(vulture)) continue;
        if (!the.partitions.connected(
                vulture->getPosition(), regroupPosition)) continue;

        const BWAPI::Position objective = order && order->getPosition().isValid()
            ? order->getPosition() : cluster.center;
        const BWAPI::Position slot = TerranFormation::vultureLineDestination(
            vulture, index, int(vultures.size()), regroupPosition,
            objective, -32, regroupPosition);
        TerranFormation::moveVultureToFormationSlot(vulture, slot);
    }
}

void MicroVultures::moveCluster(const UnitCluster & cluster,
    const BWAPI::Unitset & squadUnits,
    const BWAPI::Position & destination,
    bool lazy, bool baseDefenseSquad)
{
    if (!order || !destination.isValid()) return;
    const auto* routeDistances = destination == order->getPosition()
        ? order->distances() : nullptr;

    std::vector<BWAPI::Unit> vultures;
    long long goliathX = 0, goliathY = 0;
    int goliathCount = 0;
    for (BWAPI::Unit unit : Intersection(getUnits(), cluster.units))
        if (unit && unit->exists()) vultures.push_back(unit);
    if (vultures.empty()) return;

    std::sort(vultures.begin(), vultures.end(),
        [](BWAPI::Unit a, BWAPI::Unit b) { return a->getID() < b->getID(); });

    const TerranArmyPosture posture =
        StrategyManager::Instance().getTerranArmyPosture();
    const bool airOrder =
        posture != TerranArmyPosture::PureAir &&
        order->getKey() == "air target" &&
        destination == order->getPosition();
    const bool baseDefense =
        baseDefenseSquad &&
        TerranFormation::isBaseDefenseOrder(*order) &&
        order->getBase() &&
        destination == order->getPosition();
    const bool escort = std::any_of(vultures.begin(), vultures.end(),
        [this](BWAPI::Unit unit)
        { return isGroundArmyEscortVulture(unit, order); });
    const BWAPI::Position groundEscortAnchor =
        escort && !airOrder && !baseDefense &&
        posture != TerranArmyPosture::PureAir &&
        destination == order->getPosition()
        ? frontGroundArmyAnchor(cluster, destination)
        : BWAPI::Positions::Invalid;

    BWAPI::Position goliathCenter = BWAPI::Positions::Invalid;
    if (posture == TerranArmyPosture::PureAir &&
        destination == order->getPosition())
    {
        for (BWAPI::Unit unit : cluster.units)
            if (unit && unit->exists() && unit->getPosition().isValid() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Goliath)
            {
                goliathX += unit->getPosition().x;
                goliathY += unit->getPosition().y;
                ++goliathCount;
            }
        if (goliathCount > 0)
            goliathCenter = BWAPI::Position(
                int(goliathX / goliathCount), int(goliathY / goliathCount));
        else if (cluster.center.isValid())
        {
            BWAPI::Unit nearest = nullptr;
            int bestDistance = MAX_DISTANCE;
            for (BWAPI::Unit unit : squadUnits)
                if (unit && unit->exists() && unit->isCompleted() &&
                    unit->getPosition().isValid() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Goliath &&
                    unit->getDistance(cluster.center) < bestDistance)
                {
                    nearest = unit;
                    bestDistance = unit->getDistance(cluster.center);
                }
            if (nearest) goliathCenter = nearest->getPosition();
        }
    }

    std::vector<BWAPI::Unit> defensiveVultures;
    if (baseDefense)
    {
        for (BWAPI::Unit unit : squadUnits)
            if (unit && unit->exists() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
                defensiveVultures.push_back(unit);
        std::sort(defensiveVultures.begin(), defensiveVultures.end(),
            [](BWAPI::Unit a, BWAPI::Unit b) { return a->getID() < b->getID(); });
    }

    for (BWAPI::Unit vulture : vultures)
    {
        if (HasPriorityMineCommand(vulture)) continue;
        if (UnitUtil::MobilizeUnit(vulture)) continue;

        BWAPI::Position slot = destination;
        bool useFormationSlot = false;
        if (airOrder)
        {
            Base* safeBase = the.bases.myNatural();
            if (!safeBase || safeBase->getOwner() != BWAPI::Broodwar->self())
                safeBase = the.bases.myMain();
            const auto it = std::lower_bound(vultures.begin(), vultures.end(),
                vulture, [](BWAPI::Unit a, BWAPI::Unit b)
                { return a->getID() < b->getID(); });
            slot = TerranFormation::defensiveVultureDestination(
                vulture, int(std::distance(vultures.begin(), it)),
                int(vultures.size()), safeBase, vulture->getPosition());
            useFormationSlot = true;
        }
        else if (goliathCenter.isValid())
        {
            const auto it = std::lower_bound(vultures.begin(), vultures.end(),
                vulture, [](BWAPI::Unit a, BWAPI::Unit b)
                { return a->getID() < b->getID(); });
            slot = TerranFormation::vultureLineDestination(
                vulture, int(std::distance(vultures.begin(), it)),
                int(vultures.size()), goliathCenter, destination,
                -5 * 32, goliathCenter);
            useFormationSlot = true;
        }
        else if (baseDefense)
        {
            const auto it = std::lower_bound(
                defensiveVultures.begin(), defensiveVultures.end(), vulture,
                [](BWAPI::Unit a, BWAPI::Unit b)
                { return a->getID() < b->getID(); });
            slot = TerranFormation::defensiveVultureDestination(
                vulture, int(std::distance(defensiveVultures.begin(), it)),
                int(defensiveVultures.size()), order->getBase(), destination);
            useFormationSlot = true;
        }
        else if (groundEscortAnchor.isValid())
        {
            const auto it = std::lower_bound(vultures.begin(), vultures.end(),
                vulture, [](BWAPI::Unit a, BWAPI::Unit b)
                { return a->getID() < b->getID(); });
            slot = TerranFormation::vultureLineDestination(
                vulture, int(std::distance(vultures.begin(), it)),
                int(vultures.size()), groundEscortAnchor, destination,
                3 * 32, vulture->getPosition());
            useFormationSlot = true;
        }
        if (useFormationSlot && slot.isValid())
            TerranFormation::moveVultureToFormationSlot(
                vulture, slot, groundEscortAnchor.isValid() ? 40 : 16);
        else if (lazy)
            the.micro.MoveNear(vulture, destination, routeDistances);
        else
            the.micro.Move(vulture, destination, routeDistances);
    }
}

void MicroVultures::attackMove(BWAPI::Unit vulture,
    const BWAPI::Position & destination)
{
    if (!vulture || !vulture->exists() ||
        vulture->getType() != BWAPI::UnitTypes::Terran_Vulture ||
        !destination.isValid() || HasPriorityMineCommand(vulture)) return;
    if (!UnitUtil::MobilizeUnit(vulture))
        the.micro.AttackMove(vulture, destination);
}

void MicroVultures::moveSafely(BWAPI::Unit vulture,
    const BWAPI::Position & destination)
{
    if (!vulture || !vulture->exists() ||
        vulture->getType() != BWAPI::UnitTypes::Terran_Vulture ||
        !destination.isValid() || HasPriorityMineCommand(vulture)) return;
    if (!UnitUtil::MobilizeUnit(vulture))
        the.micro.MoveSafely(vulture, destination);
}

void MicroVultures::assignTargets(
    const BWAPI::Unitset & rangedUnits,
    const BWAPI::Unitset & targets,
    const UnitCluster & cluster)
{
    // The set of potential targets.
    BWAPI::Unitset rangedUnitTargets;
    std::copy_if(targets.begin(), targets.end(), std::inserter(rangedUnitTargets, rangedUnitTargets.end()),
        [=](BWAPI::Unit u) {
        return
            u && u->exists() && u->isVisible() && u->isDetected() &&
            u->getPosition().isValid() &&
            u->getType() != BWAPI::UnitTypes::Zerg_Larva;
    });

    const std::vector<BWAPI::Unit> tankCoveredBuildings =
        TerranSiegeSafety::coveredBuildings(rangedUnitTargets);
    BWAPI::Unitset vultureTargets = rangedUnitTargets;
    for (BWAPI::Unit building : tankCoveredBuildings)
    {
        vultureTargets.erase(building);
    }

    // Figure out if the enemy is ready to attack ground or air.
    bool enemyHasAntiGround = false;
    bool enemyHasAntiAir = false;
    for (BWAPI::Unit target : rangedUnitTargets)
    {
        // If the enemy unit is retreating or whatever, it won't attack.
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


    
    // Are any enemies in range to shoot at the ranged units?
    bool underThreat = order->isCombatOrder() && anyUnderThreat(rangedUnits);

    const bool pureAirMode =
        StrategyManager::Instance().getTerranArmyPosture() ==
            TerranArmyPosture::PureAir;
    std::vector<BWAPI::Unit> pureAirVultures;
    long long goliathX = 0;
    long long goliathY = 0;
    int goliathCount = 0;

    if (pureAirMode)
    {
        for (BWAPI::Unit unit : rangedUnits)
        {
            if (unit && unit->exists() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
            {
                pureAirVultures.push_back(unit);
            }
        }
        std::sort(
            pureAirVultures.begin(),
            pureAirVultures.end(),
            [](BWAPI::Unit a, BWAPI::Unit b)
            {
                return a->getID() < b->getID();
            });

        for (BWAPI::Unit unit : cluster.units)
        {
            if (unit && unit->exists() && unit->isCompleted() &&
                unit->getPosition().isValid() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Goliath)
            {
                goliathX += unit->getPosition().x;
                goliathY += unit->getPosition().y;
                ++goliathCount;
            }
        }
    }

    BWAPI::Position pureAirGoliathCenter = goliathCount > 0
        ? BWAPI::Position(
            int(goliathX / goliathCount),
            int(goliathY / goliathCount))
        : BWAPI::Positions::Invalid;

    if (pureAirMode && !pureAirGoliathCenter.isValid() &&
        cluster.center.isValid())
    {
        BWAPI::Unit nearestGoliath = nullptr;
        int nearestDistance = MAX_DISTANCE;
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (!unit || !unit->exists() || !unit->isCompleted() ||
                !unit->getPosition().isValid() ||
                unit->getType() != BWAPI::UnitTypes::Terran_Goliath)
            {
                continue;
            }

            const int distance = unit->getDistance(cluster.center);
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestGoliath = unit;
            }
        }
        if (nearestGoliath)
        {
            pureAirGoliathCenter = nearestGoliath->getPosition();
        }
    }

    std::vector<BWAPI::Unit> formationVultures;
    for (BWAPI::Unit unit : rangedUnits)
        if (unit && unit->exists() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
            formationVultures.push_back(unit);
    std::sort(formationVultures.begin(), formationVultures.end(),
        [](BWAPI::Unit a, BWAPI::Unit b) { return a->getID() < b->getID(); });
    const bool escort = std::any_of(formationVultures.begin(),
        formationVultures.end(), [this](BWAPI::Unit unit)
        { return isGroundArmyEscortVulture(unit, order); });
    const BWAPI::Position groundEscortAnchor =
        escort && order->getPosition().isValid()
        ? frontGroundArmyAnchor(cluster, order->getPosition())
        : BWAPI::Positions::Invalid;

    // One threat scan per cluster, shared by every escort Vulture. A worker or
    // building remains an opportunity only until a mobile enemy threatens the
    // actual ground force nearby.
    BWAPI::Unitset armyThreats;
    std::vector<BWAPI::Unit> enemySiegeTanks;
    BWAPI::Unitset siegeCoveredTargets;
    if (groundEscortAnchor.isValid())
    {
        std::vector<BWAPI::Unit> nearbyCore;
        for (BWAPI::Unit friendly :
            CombatCommander::Instance().groundSquadUnits())
            if (friendly && friendly->exists() && friendly->isCompleted() &&
                !friendly->isFlying() && !friendly->isLoaded() &&
                !friendly->getType().isWorker() &&
                !friendly->getType().isBuilding() &&
                friendly->getType() != BWAPI::UnitTypes::Terran_Vulture &&
                friendly->getDistance(groundEscortAnchor) <= 6 * 32)
                nearbyCore.push_back(friendly);

        for (BWAPI::Unit enemy : vultureTargets)
        {
            if (!enemy || enemy->isFlying() ||
                enemy->getDistance(groundEscortAnchor) > 10 * 32)
                continue;
            if (enemy->getType() ==
                BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
                enemySiegeTanks.push_back(enemy);
            if (enemy->getType().isBuilding() ||
                !UnitUtil::CanAttackGround(enemy))
                continue;

            BWAPI::Unit victim = enemy->getOrderTarget();
            if (!victim) victim = enemy->getTarget();
            for (BWAPI::Unit friendly : nearbyCore)
            {
                if (victim == friendly ||
                    (friendly->isUnderAttack() &&
                     enemy->getDistance(friendly) <=
                         UnitUtil::GetAttackRange(enemy, friendly) + 64))
                {
                    armyThreats.insert(enemy);
                    break;
                }
            }
        }

        for (BWAPI::Unit enemy : vultureTargets)
            if (enemy && !enemy->isFlying())
                for (BWAPI::Unit siege : enemySiegeTanks)
                    if (siege->getDistance(enemy) <= 7 * 32)
                    { siegeCoveredTargets.insert(enemy); break; }
    }

    for (BWAPI::Unit rangedUnit : rangedUnits)
    {
        bool mineRetryBlocked = false;
        if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
            monitorVultureMineOrder(rangedUnit, mineRetryBlocked))
        {
            continue;
        }
        if (rangedUnit->isBurrowed())
        {
            // For now, it would burrow only if irradiated. Leave it.
            // Lurkers are controlled elsewhere.
            continue;
        }

        if (the.micro.fleeDT(rangedUnit))
        {
            // We fled from an undetected dark templar.
            continue;
        }

        // WorkerManager owns the SCV; this manager keeps ownership of the
        // Goliath/Vulture. A leased damaged unit falls back to the safe rally,
        // waits for 95% HP, then automatically resumes its unchanged squad.
        if (WorkerManager::Instance().isMobileRepairTarget(rangedUnit))
        {
            const BWAPI::Position repair =
                WorkerManager::Instance().getMobileRepairPosition(rangedUnit);
            if (repair.isValid())
            {
                DrawUnitTargetTextMap(
                    rangedUnit->getPosition() + BWAPI::Position(0, 8),
                    "REPAIR");
                if (rangedUnit->getDistance(repair) > 48)
                    the.micro.Move(rangedUnit, repair);
                else
                    the.micro.HoldPosition(rangedUnit);
                continue;
            }
        }

        // Carriers stay at home until they have enough interceptors to be useful,
        // or retreat toward home to rebuild them if they run low.
        // On attack-move so that they're not helpless, but that can cause problems too....
        // Potentially useful for other units.
        // NOTE Regrouping can cause the carriers to move away from home.
        if (stayHomeUntilReady(rangedUnit))
        {
            BWAPI::Position fleeTo(the.bases.myMain()->getPosition());
            the.micro.MoveSafely(rangedUnit, fleeTo);
            continue;
        }

        BWAPI::Unit coveredBuilding = nullptr;
        if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
            order->isCombatOrder())
        {
            coveredBuilding = TerranSiegeSafety::nearestBuilding(
                rangedUnit, tankCoveredBuildings);
            if (coveredBuilding &&
                rangedUnit->getDistance(coveredBuilding) < 3 * 32)
            {
                const BWAPI::Position retreat =
                    TerranSiegeSafety::standoff(rangedUnit, coveredBuilding);
                if (retreat.isValid() &&
                    the.partitions.connected(rangedUnit->getPosition(), retreat))
                    the.micro.MoveNear(rangedUnit, retreat);
                else
                    the.micro.HoldPosition(rangedUnit);
                continue;
            }
        }

        if (order->isCombatOrder())
        {
            const BWAPI::Unitset* targetPool =
                rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture
                ? &vultureTargets : &rangedUnitTargets;
            BWAPI::Unitset localPureAirGroundThreats;
            BWAPI::Unitset localEscortTargets;
            BWAPI::Unitset localEscortThreats;

            if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture)
            {
                if (pureAirMode)
                {
                    // Existing Vultures are rear support for the Goliath army.
                    // They may answer a nearby mobile ground threat, but never
                    // select a Town Hall/static building or continue an old
                    // worker-harass order on their own.
                    BWAPI::Position defensiveFront = the.bases.front();
                    if (!defensiveFront.isValid() && the.bases.myMain())
                        defensiveFront = the.bases.myMain()->getCenter();
                    const bool noGoliathEscort =
                        !pureAirGoliathCenter.isValid();
                    for (BWAPI::Unit enemy : rangedUnitTargets)
                    {
                        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                            !enemy->isDetected() || enemy->isFlying() ||
                            enemy->getType().isBuilding() ||
                            !UnitUtil::CanAttackGround(enemy) ||
                            enemy->getDistance(rangedUnit) >
                                (noGoliathEscort ? 4 * 32 : 8 * 32) ||
                            (noGoliathEscort && defensiveFront.isValid() &&
                             enemy->getDistance(defensiveFront) > 10 * 32))
                        {
                            continue;
                        }
                        localPureAirGroundThreats.insert(enemy);
                    }

                    if (localPureAirGroundThreats.empty())
                    {
                        BWAPI::Position staging = pureAirGoliathCenter;
                        if (staging.isValid() && order->getPosition().isValid())
                        {
                            const auto it = std::lower_bound(
                                pureAirVultures.begin(),
                                pureAirVultures.end(),
                                rangedUnit,
                                [](BWAPI::Unit a, BWAPI::Unit b)
                                {
                                    return a->getID() < b->getID();
                                });
                            const int index = int(std::distance(
                                pureAirVultures.begin(), it));
                            staging = TerranFormation::vultureLineDestination(
                                rangedUnit,
                                index,
                                int(pureAirVultures.size()),
                                pureAirGoliathCenter,
                                order->getPosition(),
                                -5 * 32,
                                pureAirGoliathCenter);
                        }

                        if (!staging.isValid())
                        {
                            staging = the.bases.front();
                        }
                        if (!staging.isValid() && the.bases.myMain())
                        {
                            staging = the.bases.myMain()->getCenter();
                        }

                        DrawUnitTargetTextMap(
                            rangedUnit->getPosition() + BWAPI::Position(0, 8),
                            "PUREAIR REAR");
                        if (staging.isValid() &&
                            rangedUnit->getDistance(staging) > 32)
                        {
                            TerranFormation::moveVultureToFormationSlot(
                                rangedUnit, staging);
                        }
                        else
                        {
                            the.micro.HoldPosition(rangedUnit);
                        }
                        continue;
                    }

                    targetPool = &localPureAirGroundThreats;
                }
                else if (groundEscortAnchor.isValid() &&
                    isGroundArmyEscortVulture(rangedUnit, order))
                {
                    const int fromArmy =
                        rangedUnit->getDistance(groundEscortAnchor);
                    for (BWAPI::Unit enemy : vultureTargets)
                    {
                        if (!enemy || enemy->isFlying() ||
                            enemy->getDistance(rangedUnit) > 11 * 32 ||
                            (enemy->getDistance(groundEscortAnchor) > 9 * 32 &&
                             !((fromArmy <= 10 * 32 ||
                                rangedUnit->isUnderAttack()) &&
                               enemy->getDistance(rangedUnit) <= 3 * 32)) ||
                            (fromArmy > 12 * 32 &&
                             !rangedUnit->isUnderAttack()) ||
                            (fromArmy > 12 * 32 &&
                             enemy->getType().isBuilding()) ||
                            (!rangedUnit->isInWeaponRange(enemy) &&
                             !the.partitions.connected(
                                 rangedUnit->getPosition(),
                                 enemy->getPosition())))
                            continue;

                        // Don't start a sortie into fixed defenses or an enemy
                        // siege line. A Vulture already in weapon range can
                        // still fire while it kites back toward the army.
                        if (the.groundHitsFixed.inRange(enemy) &&
                            (enemy->getType().isBuilding() ||
                             !rangedUnit->isInWeaponRange(enemy)))
                            continue;
                        if (!rangedUnit->isInWeaponRange(enemy))
                        {
                            if (siegeCoveredTargets.count(enemy)) continue;
                        }

                        localEscortTargets.insert(enemy);
                        if (armyThreats.count(enemy))
                            localEscortThreats.insert(enemy);
                    }
                    targetPool = &localEscortTargets;
                }

                // Dedicated raids keep their own combat and mine decisions;
                // the ground-army escort above never takes ownership of them.
                if (order && isWorkerHarassOrder(order))
                {
                    // Workers do not trigger Spider Mines. Keep harass on
                    // direct fire; tactical mines are for pursuing ground troops.
                    bool laidCoverMine = false;
                    if (!mineRetryBlocked)
                        for (BWAPI::Unit pursuer : rangedUnitTargets)
                            if (tryLaySpiderMineOnRetreat(rangedUnit, pursuer))
                            { laidCoverMine = true; break; }
                    if (laidCoverMine) continue;
                    BWAPI::Unit immediateThreat = nullptr;
                    int threatDistance = MAX_DISTANCE;
                    for (BWAPI::Unit enemy : vultureTargets)
                    {
                        if (!enemy || !enemy->exists() ||
                            !UnitUtil::CanAttack(enemy, rangedUnit) ||
                            !UnitUtil::CanAttack(rangedUnit, enemy))
                            continue;
                        BWAPI::Unit victim = enemy->getOrderTarget();
                        if (!victim) victim = enemy->getTarget();
                        const bool attackingUs = victim && victim->exists() &&
                            victim->getPlayer() == the.self();
                        const int distance = rangedUnit->getDistance(enemy);
                        if (distance > UnitUtil::GetAttackRange(
                                enemy, rangedUnit) + 64 ||
                            (!attackingUs && !enemy->isAttacking() &&
                             !rangedUnit->isUnderAttack()))
                            continue;
                        if (distance < threatDistance)
                        {
                            immediateThreat = enemy;
                            threatDistance = distance;
                        }
                    }
                    if (immediateThreat)
                    {
                        if (Config::Micro::KiteWithRangedUnits)
                            the.micro.KiteTarget(rangedUnit, immediateThreat);
                        else
                            the.micro.CatchAndAttackUnit(rangedUnit, immediateThreat);
                        continue;
                    }
                    BWAPI::Unit workerTarget = getBestWorkerHarassTarget(rangedUnit);
                    if (workerTarget)
                    {
                        if (coveredBuilding &&
                            workerTarget->getDistance(coveredBuilding) < 3 * 32 &&
                            !rangedUnit->isInWeaponRange(workerTarget))
                        {
                            the.micro.HoldPosition(rangedUnit);
                            continue;
                        }
                        if (Config::Debug::DrawUnitTargets)
                        {
                            BWAPI::Broodwar->drawLineMap(
                                rangedUnit->getPosition(),
                                workerTarget->getPosition(),
                                BWAPI::Colors::Orange
                            );
                        }

                        if (rangedUnit->isInWeaponRange(workerTarget))
                        {
                            the.micro.AttackUnit(rangedUnit, workerTarget);
                        }
                        else
                        {
                            the.micro.Move(rangedUnit, workerTarget->getPosition());
                        }

                        continue;
                    }

                    // With no nearby worker, use ordinary local combat and
                    // pathing below. A forced Move to the mineral line used
                    // to overwrite a fight with its defenders every frame.
                }

            }

            BWAPI::Unit target = localEscortThreats.empty()
                ? nullptr : getTarget(rangedUnit, localEscortThreats, true);
            if (!target)
                target = getTarget(rangedUnit, *targetPool, underThreat);
            if (coveredBuilding && target &&
                !rangedUnit->isInWeaponRange(target) &&
                !target->isFlying() &&
                target->getDistance(coveredBuilding) < 3 * 32)
            {
                the.micro.HoldPosition(rangedUnit);
                continue;
            }
            if (coveredBuilding && !target)
            {
                the.micro.HoldPosition(rangedUnit);
                continue;
            }
            if (target)
            {
                if (Config::Debug::DrawUnitTargets)
                {
                    BWAPI::Broodwar->drawLineMap(
                        rangedUnit->getPosition(),
                        target->getPosition(),
                        BWAPI::Colors::Purple
                    );
                }

                bool kite = rangedUnit->isFlying() ? enemyHasAntiAir : enemyHasAntiGround;

                if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture)
                {
                    if (!mineRetryBlocked &&
                        tryLaySpiderMineOnRetreat(rangedUnit, target))
                    {
                        continue;
                    }
                }

                if (Config::Micro::KiteWithRangedUnits && kite)
                {
                    the.micro.KiteTarget(rangedUnit, target);
                }
                else
                {
                    the.micro.CatchAndAttackUnit(rangedUnit, target);
                }
            }
            else
            {
                if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
                    order->getKey() == "air target")
                {
                    // Vultures must not chase an air-only objective, but simply
                    // holding wherever the squad happened to switch orders can
                    // leave them AFK in the middle of the map. First take any
                    // nearby ground opportunity; otherwise fall back to our
                    // real defensive front and wait there.
                    BWAPI::Unitset localGroundTargets;
                    for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
                        rangedUnit->getPosition(),
                        9 * 32,
                        BWAPI::Filter::IsEnemy &&
                        !BWAPI::Filter::IsFlyer &&
                        BWAPI::Filter::IsDetected))
                    {
                        if (enemy && enemy->exists())
                        {
                            localGroundTargets.insert(enemy);
                        }
                    }

                    BWAPI::Unit localTarget =
                        getTarget(rangedUnit, localGroundTargets, true);
                    if (localTarget)
                    {
                        the.micro.CatchAndAttackUnit(rangedUnit, localTarget);
                        continue;
                    }

                    BWAPI::Position staging = the.bases.front();
                    if (!staging.isValid() && the.bases.myMain())
                    {
                        staging = the.bases.myMain()->getCenter();
                    }

                    if (staging.isValid() &&
                        rangedUnit->getDistance(staging) > 4 * 32)
                    {
                        the.micro.MoveNear(rangedUnit, staging);
                    }
                    else
                    {
                        the.micro.HoldPosition(rangedUnit);
                    }
                    continue;
                }

                if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
                    TerranFormation::isBaseDefenseOrder(*order))
                {
                    int index = 0, total = 0;
                    for (BWAPI::Unit unit : getUnits())
                        if (unit && unit->exists() && unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
                        { ++total; if (unit->getID() < rangedUnit->getID()) ++index; }
                    const BWAPI::Position slot = TerranFormation::defensiveVultureDestination(
                        rangedUnit, index, total, order->getBase(), order->getPosition());
                    TerranFormation::moveVultureToFormationSlot(
                        rangedUnit, slot);
                    continue;
                }

                if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
                    groundEscortAnchor.isValid() &&
                    isGroundArmyEscortVulture(rangedUnit, order))
                {
                    const auto it = std::lower_bound(
                        formationVultures.begin(), formationVultures.end(),
                        rangedUnit, [](BWAPI::Unit a, BWAPI::Unit b)
                        { return a->getID() < b->getID(); });
                    const BWAPI::Position slot =
                        TerranFormation::vultureLineDestination(
                            rangedUnit,
                            int(std::distance(formationVultures.begin(), it)),
                            int(formationVultures.size()), groundEscortAnchor,
                            order->getPosition(), 3 * 32,
                            rangedUnit->getPosition());
                    TerranFormation::moveVultureToFormationSlot(
                        rangedUnit, slot, 40);
                    continue;
                }

                // No target found. If we're not near the order position, go there.
                if (rangedUnit->getDistance(order->getPosition()) > 100)
                {
                    the.micro.MoveNear(
                        rangedUnit, order->getPosition(), order->distances());
                }
            }
        }
    }
}

// This can return null if no target is worth attacking.
// underThreat is true if any of the melee units is under immediate threat of attack.
BWAPI::Unit MicroVultures::getTarget(BWAPI::Unit rangedUnit, const BWAPI::Unitset& targets, bool underThreat)
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
        if (!target || !target->exists() ||
            !target->isVisible() || !target->isDetected() ||
            target->isInvincible() ||
            !UnitUtil::CanAttack(rangedUnit, target)) continue;
        // Vultures have no air weapon. Without this check, high-priority air
        // units such as Carriers, Dropships and Science Vessels could still be
        // selected, and the Vulture would drive after a target it can never hit.
        // Keep all ground targeting and Spider Mine behavior unchanged.
        if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
            target->isFlying())
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
            if (rangedUnit->getType() == BWAPI::UnitTypes::Zerg_Mutalisk)
            {
                score += 16 * target->getAcidSporeCount();
            }
            else
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

int MicroVultures::getAttackPriority(BWAPI::Unit rangedUnit, BWAPI::Unit target) 
{
    const BWAPI::UnitType rangedType = rangedUnit->getType();
    const BWAPI::UnitType targetType = target->getType();

    if (isNeutralEgg(target)) return getBackstopAttackPriority(target);

    if (rangedType == BWAPI::UnitTypes::Zerg_Guardian && target->isFlying())
    {
        // Can't target it.
        return 0;
    }

    // A carrier should not target an enemy interceptor. It's too hard to hit.
    if (rangedType == BWAPI::UnitTypes::Protoss_Carrier && targetType == BWAPI::UnitTypes::Protoss_Interceptor)
    {
        return 0;
    }


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
    if (rangedType == BWAPI::UnitTypes::Terran_Wraith ||
        rangedType == BWAPI::UnitTypes::Protoss_Scout)
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
        if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        {
            return 11;
        }
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
bool MicroVultures::stayHomeUntilReady(const BWAPI::Unit u) const
{
    return
        u->getType() == BWAPI::UnitTypes::Protoss_Carrier && u->getInterceptorCount() < 4;
}
