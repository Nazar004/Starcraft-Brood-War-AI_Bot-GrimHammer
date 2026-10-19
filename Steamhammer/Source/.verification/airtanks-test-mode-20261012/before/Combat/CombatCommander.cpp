#include <queue>

#include "Combat/CombatCommander.h"
#include "Strategy/Terran/BioTankAssessment.h"

#include "Map/Bases.h"
#include "Construction/BuildingManager.h"
#include "Construction/BuildingPlacer.h"
#include "Combat/Micro/Terran/ExpansionVultureReservation.h"
#include <chrono>
#include "Map/MapGrid.h"
#include "Combat/Micro/Micro.h"
#include "Intelligence/Learning/OpponentModel.h"
#include "Production/ProductionManager.h"
#include "Utilities/Random.h"
#include "Intelligence/ScoutManager.h"
#include "Utilities/UnitUtil.h"
#include <algorithm>
#include <climits>
#include <map>
#include "Combat/Micro/Terran/MicroTanks.h"
#include "Combat/Micro/MicroRanged.h"

using namespace UAlbertaBot;

bool UAlbertaBot::ExpansionVultureReservation::canReserve(BWAPI::Unit unit)
{
    return CombatCommander::Instance().canReserveExpansionSupportVulture(unit);
}

bool UAlbertaBot::ExpansionVultureReservation::reserve(BWAPI::Unit unit)
{
    return CombatCommander::Instance().reserveExpansionSupportVulture(unit);
}

void UAlbertaBot::ExpansionVultureReservation::release(int unitID)
{
    CombatCommander::Instance().releaseExpansionSupportVulture(unitID);
}

// Squad priorities: Which can steal units from others.
// Anyone can steal from the Idle squad.
const size_t IdlePriority = 0;
const size_t AttackPriority = 2;
const size_t ReconPriority = 3;
const size_t ClearancePriority = 4;
const size_t BaseDefensePriority = 5;
// Base defense must be able to borrow a limited number of PureAir units.
// The remaining AntiAir squad keeps attacking instead of being recalled in full.
const size_t AntiAirPriority = 4;
const size_t ScoutDefensePriority = 7;
const size_t WatchPriority = 8;
const size_t DropPriority = 9;
const size_t VultureHarassPriority = 12;
const size_t BenzeneClearancePriority = 13;
const size_t ProxyFactoryVulturePriority = 14;
const size_t WorkerCounterPriority = 15;

// The attack squads.
const int DefendFrontRadius = 400;
const int AttackRadius = 800;

// Reconnaissance squad.
const int ReconTargetTimeout = 40 * 24;
const int ReconRadius = 400;

// If there are this many carriers or more, carriers are in the flying squad
// and act independently.

CombatCommander::CombatCommander()
    : _initialized(false)
    , _goAggressive(true)
    , _isWatching(false)
    , _reconSquadAlive(false)
    , _reconTarget(nullptr)
    , _lastReconTargetChange(0)
    , _lastUndetectedEnemyPosition(BWAPI::Positions::Invalid)
    , _lastUndetectedEnemyFrame(-100000)
    , _expansionDetectionPosition(BWAPI::Positions::Invalid)
    , _expansionDetectionFrame(-100000)
    , _lastComsatQueueFrame(-100000)
    , _lastAutomaticComsatFrame(-100000)
    , _lastComsatTarget(BWAPI::Positions::Invalid)
    , _lastComsatTargetFrame(-100000)
    , _islandCleanupSacrificeVictim(nullptr)
    , _lastIslandCleanupSupplyReleaseFrame(-100000)
    , _tankDropSeeded(false)
    , _tankDropTotalLaunches(0)
    , _drop1EverAssigned(false)
    , _drop2EverAssigned(false)
    , _genericDropFillStartFrame(-1)
    , _genericDropLastCargoUsed(0)
{

    _tankDropPhase["Drop1"] = 0;
    _tankDropPhase["Drop2"] = 0;

    _tankDropUnloadStartFrame["Drop1"] = -1;
    _tankDropUnloadStartFrame["Drop2"] = -1;

    _tankDropReturnIssued["Drop1"] = false;
    _tankDropReturnIssued["Drop2"] = false;

    _tankDropLaunchCount["Drop1"] = 0;
    _tankDropLaunchCount["Drop2"] = 0;
}

namespace
{
    Base* findDefensiveBaseUnderAttack();

    BWAPI::Position groundAntiAirApproach(
        BWAPI::Position airPosition,
        BWAPI::Position reference,
        int partition,
        int weaponRange)
    {
        if (!airPosition.isValid() || !reference.isValid() ||
            partition <= 0 || weaponRange <= 32)
        {
            return BWAPI::Positions::Invalid;
        }

        BWAPI::Position best = BWAPI::Positions::Invalid;
        int bestScore = MAX_DISTANCE;
        const BWAPI::TilePosition airTile(airPosition);
        const int radius = (weaponRange - 16) / 32;
        for (int dx = -radius; dx <= radius; ++dx)
        {
            for (int dy = -radius; dy <= radius; ++dy)
            {
                const BWAPI::TilePosition tile(airTile.x + dx, airTile.y + dy);
                if (!tile.isValid() || !the.map.isRuntimeWalkable(tile) ||
                    the.partitions.id(tile) != partition)
                {
                    continue;
                }
                const BWAPI::Position candidate = TileCenter(tile);
                if (candidate.getApproxDistance(airPosition) > weaponRange - 16)
                {
                    continue;
                }
                const int score = candidate.getApproxDistance(reference) * 2 +
                    candidate.getApproxDistance(airPosition);
                if (score < bestScore)
                {
                    bestScore = score;
                    best = candidate;
                }
            }
        }
        return best;
    }

    int rememberedEnemyCarrierCountForCombat()
    {
        int count = 0;
        for (const auto& kv : the.info.getUnitData(the.enemy()).getUnits())
        {
            if (kv.second.type == BWAPI::UnitTypes::Protoss_Carrier)
            {
                ++count;
            }
        }
        return count;
    }

}

// Called once at the start of the game.
// You can also create new squads at other times.
void CombatCommander::initializeSquads()
{
    // The idle squad includes workers at work (not idle at all) and unassigned units.
    _squadData.createSquad("Idle", IdlePriority).getOrder().setStatus("Work");

    // These squads don't care what order they are given.
    // They analyze the situation for themselves.
    
    
    // The ground squad will pressure an enemy base.
    _squadData.createSquad("Ground", AttackPriority);

    // The flying squad separates air units so they can act independently.
    _squadData.createSquad("Flying", AttackPriority);

    _squadData.createSquad("AntiAir", AntiAirPriority);

    // The recon squad carries out reconnaissance in force to deny enemy bases.
    // It is filled in when enough units are available.
    Squad & reconSquad = _squadData.createSquad("Recon", ReconPriority);
    reconSquad.setOrder(SquadOrder("Recon"));
    reconSquad.setCombatSimRadius(200);  // combat sim includes units in a smaller radius than for a combat squad
    // reconSquad.setFightVisible(true);    // combat sim sees only visible enemy units (not all known enemies)

    BWAPI::Position ourBasePosition = BWAPI::Position(the.self()->getStartLocation());

    // The scout defense squad chases the enemy worker scout.
    if (Config::Micro::ScoutDefenseRadius > 0)
    {
        _squadData.createSquad("ScoutDefense", ScoutDefensePriority).
            setOrder(SquadOrder(SquadOrderTypes::Defend, ourBasePosition, Config::Micro::ScoutDefenseRadius, false, "Stop that scout"));
    }

    // If we're expecting to drop, create a drop squad.
    // It is initially ordered to hold ground until it can load up and go.
    if (StrategyManager::Instance().dropIsPlanned())
    {
        _squadData.createSquad("Drop", DropPriority).
            setOrder(SquadOrder(SquadOrderTypes::Hold, ourBasePosition, AttackRadius, false, "Wait for transport"));
    }
}

void CombatCommander::update(const BWAPI::Unitset& combatUnits)
{
    const auto combatStarted = std::chrono::steady_clock::now();

    clearDeadLockedHarassVultures();
    clearDeadExpansionSupportVultures();
    if (!_initialized)
    {
        initializeSquads();
        _initialized = true;
    }

    _combatUnits = combatUnits;

    int frame8 = the.now() % 8;
    const bool frame4Reaction = the.now() % 4 == 1;

    // Cheap core squad maintenance remains every frame. Emergency/local
    // reactions used to share the 8-frame strategic cadence, which could leave
    // a Zealot/worker/Carrier hitting the base for roughly a third of a second
    // before defenders were reassigned. Run those local reactions twice as
    // often while keeping the heavier strategic work on the old cadence.
    updateIdleSquad();

    if (frame8 == 1)
    {
        updateDropSquads();
    }

    if (frame4Reaction)
    {
        updateScoutDefenseSquad();
        updateBaseDefenseSquads();
    }

    if (frame8 == 1)
    {
    }

    updateReconSquad();

    if (frame8 == 1)
    {
        updateBenzeneClearanceSquad();
    }

    if (frame4Reaction)
    {
        updateProxyFactoryVultureSquad();
        updateWorkerCounterAttackSquads();
    }

    updateAttackSquads();

    if (the.now() % 20 == 1)
    {
    }

    updateScienceVessels();
    const auto squadStarted = std::chrono::steady_clock::now();
    _lastCombatPreparationMilliseconds =
        std::chrono::duration<double, std::milli>(
            squadStarted - combatStarted).count();
    _squadData.update();
    _lastSquadUpdateMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - squadStarted).count();

    // Include idle/reinforcement Marines and cover every wall piece, even
    // before the wall or its Bunker is complete. Apply after squad retreats.
    MicroRanged::defendTerranWall(_combatUnits);

    // The bunker order must follow squad micro, or the Marine's squad can
    // overwrite the pickup on the same frame and leave a nearby bunker empty.
    loadOrUnloadBunkers();

    // Run after squad micro. MicroTanks, expansion clearing and production
    // reactions have all had a chance to submit their scan requests, so one
    // central decision can preserve the correct emergency reserve.
    if (frame8 % 4 == 2)
    {
        doComsatScan();
    }

    // This is deliberately last: during the exceptional island-cleanup
    // supply release it must override ordinary squad orders for the selected
    // friendly target and its attackers.
    updateIslandCleanupSupplyRelease();

    cancelDyingItems();


}

// Clean up any data structures that may otherwise not be unwound in the correct order.
// This fixes an end-of-game bug diagnosed by Bruce Nielsen.
// This onEnd() doesn't care who won.
void CombatCommander::onEnd()
{
    _squadData.clearSquadData();

    _initialized = false;
    _combatUnits.clear();
    _goAggressive = true;
    _isWatching = false;
    _reconSquadAlive = false;
    _reconTarget = nullptr;
    _lastReconTargetChange = 0;
    _lastUndetectedEnemyPosition = BWAPI::Positions::Invalid;
    _lastUndetectedEnemyFrame = -100000;
    _expansionDetectionPosition = BWAPI::Positions::Invalid;
    _expansionDetectionFrame = -100000;
    _comsatScanRequests.clear();
    _comsatsUsedForStrategicScouting.clear();
    _lastComsatQueueFrame = -100000;
    _lastAutomaticComsatFrame = -100000;
    _lastComsatTarget = BWAPI::Positions::Invalid;
    _lastComsatTargetFrame = -100000;
    _lastComsatReason.clear();
    _islandCleanupSacrificeVictim = nullptr;
    _lastIslandCleanupSupplyReleaseFrame = -100000;
    _genericDropFillStartFrame = -1;
    _genericDropLastCargoUsed = 0;

    _tankDropPhase.clear();
    _tankDropEmergencyMode.clear();
    _tankDropLaunchIssued.clear();
    _tankDropLaunchCount.clear();
    _tankDropUnloadStartFrame.clear();
    _tankDropReturnIssued.clear();
    _tankDropInitialVultureIDs.clear();
    _tankDropUsedVultureIDs.clear();
    _lockedHarassVultures.clear();
    _expansionSupportVultureIDs.clear();
    _expansionFerryDropshipIDs.clear();
    _proxyFactoryVultureIDs.clear();
    _escapingWorkerIDs.clear();

    _tankDropPhase["Drop1"] = 0;
    _tankDropPhase["Drop2"] = 0;
    _tankDropUnloadStartFrame["Drop1"] = -1;
    _tankDropUnloadStartFrame["Drop2"] = -1;
    _tankDropReturnIssued["Drop1"] = false;
    _tankDropReturnIssued["Drop2"] = false;
    _tankDropLaunchCount["Drop1"] = 0;
    _tankDropLaunchCount["Drop2"] = 0;

    _tankDropSeeded = false;
    _tankDropTotalLaunches = 0;
    _drop1EverAssigned = false;
    _drop2EverAssigned = false;

    MicroTanks::ResetForNewGame();
}

bool CombatCommander::groundSquadAttackOrderActive()
{
    if (!getAggression() || !_squadData.squadExists("Ground"))
    {
        return false;
    }

    Squad& ground = _squadData.getSquad("Ground");
    const SquadOrder& order = ground.getOrder();
    return !ground.getUnits().empty() &&
        order.getStatus().compare(0, 7, "Attack ") == 0 &&
        order.getKey() != "fallback" && order.getKey() != "nothing";
}

const BWAPI::Unitset& CombatCommander::groundSquadUnits()
{
    static const BWAPI::Unitset empty;
    return _squadData.squadExists("Ground")
        ? _squadData.getSquad("Ground").getUnits() : empty;
}







void CombatCommander::updateIdleSquad()
{
    Squad& idleSquad = _squadData.getSquad("Idle");

    for (BWAPI::Unit unit : _combatUnits)
    {
        if (isExpansionSupportVulture(unit) ||
            isExpansionFerryDropship(unit) ||
            WorkerManager::Instance().isExpansionFerryWorker(unit))
        {
            continue;
        }

        if (unit->getType() == BWAPI::UnitTypes::Terran_Science_Vessel)
        {
            continue;
        }

        // if it hasn't been assigned to a squad yet, put it in the low priority idle squad
        if (_squadData.canAssignUnitToSquad(unit, idleSquad))
        {
            _squadData.assignUnitToSquad(unit, idleSquad);
        }
    }
}

// Put irradiated organic units into the Irradiated squad.
// Exceptions: Queens and defilers.


// Put all overlords which are not otherwise assigned into the Overlord squad.




// Put all scourge into the Scourge squad.


// Comparison function for a sort done below.
bool compareFirst(const std::pair<int, Base *> & left, const std::pair<int, Base *> & right)
{
    return left.first < right.first;
}

// Update the watch squads, which set a sentry in each free base to see enemy expansions
// and possibly stop them. It also clears spider mines as a side effect.
// One squad per base being watched: A free base may get a watch squad of 1 zergling.
// For now, only zerg keeps watch squads, and only when units are available.


// Update the small recon squad which tries to find and deny enemy bases.
// Units available to the recon squad each have a "weight".
// Weights sum to no more than MaxWeight, set below.
void CombatCommander::updateReconSquad()
{
    // The Recon squad is not needed very early in the game.
    // If rushing, we want to dedicate all units to the rush.
    if (the.now() < 6 * 24 * 60)
    {
        return;
    }

    const int MaxWeight = 12;
    Squad & reconSquad = _squadData.getSquad("Recon");

    // BioTank's first push needs its Marine/Medic screen in the same Ground
    // squad as the Tanks. The normal Recon lease can take that screen away
    // just as the first Siege group starts moving. Resume Recon after a
    // replacement infantry group and Tank core are available.
    if (StrategyManager::Instance().bioTankPlanActive() &&
        (the.my.completed.count(BWAPI::UnitTypes::Terran_Marine) < 18 ||
         the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
             the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) < 2))
    {
        reconSquad.clear();
        _reconSquadAlive = false;
        return;
    }

    // Tanks stay with the Ground army. Medics must stay there too rather than
    // following a Marine assigned to reconnaissance.
    const BWAPI::Unitset previousReconUnits = reconSquad.getUnits();
    for (BWAPI::Unit unit : previousReconUnits)
    {
        if (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
            unit->getType() == BWAPI::UnitTypes::Terran_Medic)
        {
            reconSquad.removeUnit(unit);
        }
    }

    chooseReconTarget(reconSquad);

    // If nowhere needs seeing, disband the squad. We're done for now.
    // It can happen that the Watch squad sees all bases, meeting this condition.
    if (!_reconTarget)
    {
        reconSquad.clear();
        _reconSquadAlive = false;
        return;
    }

    // Issue the order.
    reconSquad.setOrder(SquadOrder(SquadOrderTypes::Attack, _reconTarget, ReconRadius, true, "Reconnaissance in force"));

    // Special case: If we started on an island, then the recon squad consists
    // entirely of one overlord--if we're lucky enough to be zerg.
    if (the.bases.isIslandStart())
    {
        if (reconSquad.getUnits().size() == 0)
        {
            for (BWAPI::Unit unit : _combatUnits)
            {
                if (unit->getType().isDetector() &&
                    unit->getType() != BWAPI::UnitTypes::Terran_Science_Vessel &&
                    _squadData.canAssignUnitToSquad(unit, reconSquad)) {
                }
            }
        }
        _reconSquadAlive = !reconSquad.isEmpty();
        return;
    }

    // What is already in the squad?
    int squadWeight = 0;
    for (BWAPI::Unit unit : reconSquad.getUnits())
    {
        squadWeight += weighReconUnit(unit);
    }

    // If everything except the detector is gone, let the detector go too.
    // It can't carry out reconnaissance in force.
    if (squadWeight == 0 && !reconSquad.isEmpty())
    {
        reconSquad.clear();
    }

    // What is available to put into the squad?
    int availableWeight = 0;
    for (BWAPI::Unit unit : _combatUnits)
    {
        availableWeight += weighReconUnit(unit);
    }

    // The allowed weight of the recon squad. It should steal few units.
    int weightLimit = availableWeight >= 24
        ? 2 + (availableWeight - 24) / 6
        : 0;

    // After six minutes Terran should keep at least a tiny mobile recon group
    // whenever enough light units exist. Tanks have weight 0 and are never
    // pulled into Recon.
    if (the.selfRace() == BWAPI::Races::Terran && availableWeight >= 4)
    {
        weightLimit = std::max(weightLimit, 4);
    }
    if (weightLimit > MaxWeight)
    {
        weightLimit = MaxWeight;
    }

    // If the recon squad weighs more than it should, clear it and continue.
    // Units will be added back in if they should be.
    if (squadWeight > weightLimit)
    {
        reconSquad.clear();
        squadWeight = 0;
    }

    bool hasDetector = reconSquad.hasDetector();
    bool wantDetector = wantSquadDetectors();

    if (hasDetector && !wantDetector)
    {
        for (BWAPI::Unit unit : reconSquad.getUnits())
        {
            if (unit->getType().isDetector())
            {
                reconSquad.removeUnit(unit);
                break;
            }
        }
        hasDetector = false;
    }

    // Add mobile combat units up to the weight limit. Medics are not scouts.
    for (BWAPI::Unit unit : _combatUnits)
    {
        if (squadWeight >= weightLimit)
        {
            break;
        }
        BWAPI::UnitType type = unit->getType();
        int weight = weighReconUnit(type);
        if (weight > 0 && squadWeight + weight <= weightLimit && _squadData.canAssignUnitToSquad(unit, reconSquad))
        {
            _squadData.assignUnitToSquad(unit, reconSquad);
            squadWeight += weight;
        }
        else if (type.isDetector() &&
            type != BWAPI::UnitTypes::Terran_Science_Vessel &&
            wantDetector &&
            !hasDetector &&
            _squadData.canAssignUnitToSquad(unit, reconSquad))
        {
            _squadData.assignUnitToSquad(unit, reconSquad);
            hasDetector = true;
        }
    }

    _reconSquadAlive = !reconSquad.isEmpty();
}

namespace
{
    bool isBenzeneMap()
    {
        const std::string& mapName = BWAPI::Broodwar->mapFileName();
        return
            mapName.find("Benzene") != std::string::npos ||
            mapName.find("benzene") != std::string::npos;
    }

    BWAPI::Unit getOurBenzeneNeutralBlocker()
    {
        if (!isBenzeneMap())
        {
            return nullptr;
        }

        const BWAPI::Position startPosition(the.self()->getStartLocation());
        const int maxDistanceFromStart = 48 * 32;

        BWAPI::Unit best = nullptr;
        int bestDistance = MAX_DISTANCE;
        bool bestIsTargetable = false;

        for (BWAPI::Unit neutral : BWAPI::Broodwar->getStaticNeutralUnits())
        {
            if (!neutral ||
                !neutral->exists() ||
                neutral->isInvincible() ||
                neutral->isFlying())
            {
                continue;
            }

            const BWAPI::UnitType type = neutral->getInitialType();

            // Benzene's worker-route obstacle is a stack of neutral Power
            // Generators. Target that exact map feature and no other scenery.
            if (type != BWAPI::UnitTypes::Special_Power_Generator)
            {
                continue;
            }

            const BWAPI::Position position = neutral->getInitialPosition();
            if (!position.isValid())
            {
                continue;
            }

            const int distance = startPosition.getApproxDistance(position);
            if (distance > maxDistanceFromStart)
            {
                continue;
            }

            const bool isTargetable = neutral->isTargetable();

            if (distance < bestDistance ||
                (distance == bestDistance && isTargetable && !bestIsTargetable) ||
                (distance == bestDistance && isTargetable == bestIsTargetable && best &&
                    (neutral->getHitPoints() < best->getHitPoints() ||
                     (neutral->getHitPoints() == best->getHitPoints() &&
                      neutral->getID() < best->getID()))))
            {
                bestDistance = distance;
                best = neutral;
                bestIsTargetable = isTargetable;
            }
        }

        return best;
    }
}

// Benzene has one destructible neutral building on each player's side. The
// closest one to our start interferes with worker traffic to later bases. Send
// one worker early (as on the mineral-blocker maps), then reinforce it with
// exactly four marines when they become available.
void CombatCommander::updateBenzeneClearanceSquad()
{
    const std::string squadName = "BenzeneClear";

    // Do not disturb the scripted JSON opening. The instant the opening book
    // is finished (or intentionally abandoned), start clearing our Benzene
    // route. One SCV begins immediately; up to four Marines join as soon as
    // they exist.
    if (!ProductionManager::Instance().isOutOfBook())
    {
        if (_squadData.squadExists(squadName))
        {
            _squadData.removeSquad(squadName);
        }
        return;
    }

    BWAPI::Unit blocker = getOurBenzeneNeutralBlocker();

    if (!blocker)
    {
        if (_squadData.squadExists(squadName))
        {
            _squadData.removeSquad(squadName);
        }
        return;
    }

    const int wantedMarines = 4;
    int completedMarines = 0;
    int completedWorkers = 0;

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit &&
            unit->exists() &&
            unit->isCompleted())
        {
            if (unit->getType() == BWAPI::UnitTypes::Terran_Marine)
            {
                ++completedMarines;
            }
            else if (unit->getType() == BWAPI::UnitTypes::Terran_SCV)
            {
                ++completedWorkers;
            }
        }
    }

    // After the JSON opening, do not wait for four Marines. A healthy economy
    // can spare one SCV to start immediately, and Marines reinforce it later.
    if (!_squadData.squadExists(squadName) && completedWorkers < 6)
    {
        return;
    }

    Squad& clearanceSquad = _squadData.squadExists(squadName)
        ? _squadData.getSquad(squadName)
        : _squadData.createSquad(squadName, BenzeneClearancePriority);

    bool hasWorker = false;
    int squadMarines = 0;
    BWAPI::Unit damagedWorker = nullptr;

    for (BWAPI::Unit unit : clearanceSquad.getUnits())
    {
        if (!unit || !unit->exists())
        {
            continue;
        }

        if (unit->getType() == BWAPI::UnitTypes::Terran_SCV)
        {
            if (unit->getHitPoints() * 2 < unit->getType().maxHitPoints())
            {
                damagedWorker = unit;
            }
            else
            {
                hasWorker = true;
            }
        }
        else if (unit->getType() == BWAPI::UnitTypes::Terran_Marine)
        {
            ++squadMarines;
        }
    }

    if (damagedWorker)
    {
        // Return the damaged SCV to WorkerManager and select a healthy
        // replacement below. Do not sacrifice the same worker forever.
        clearanceSquad.removeUnit(damagedWorker);
    }

    if (!hasWorker && completedWorkers >= 6)
    {
        BWAPI::Unit closestWorker = nullptr;
        int closestDistance = MAX_DISTANCE;

        for (BWAPI::Unit worker : the.self()->getUnits())
        {
            if (!worker ||
                !worker->exists() ||
                !worker->isCompleted() ||
                worker->getType() != BWAPI::UnitTypes::Terran_SCV ||
                worker->getHitPoints() * 3 < worker->getType().maxHitPoints() * 2 ||
                !WorkerManager::Instance().isFree(worker) ||
                !_squadData.canAssignUnitToSquad(worker, clearanceSquad))
            {
                continue;
            }

            const int distance = worker->getDistance(blocker);
            if (distance < closestDistance)
            {
                closestDistance = distance;
                closestWorker = worker;
            }
        }

        if (closestWorker)
        {
            WorkerManager::Instance().setCombatWorker(closestWorker);
            _squadData.assignUnitToSquad(closestWorker, clearanceSquad);
        }
    }

    while (squadMarines < wantedMarines)
    {
        BWAPI::Unit closestMarine = nullptr;
        int closestDistance = MAX_DISTANCE;

        for (BWAPI::Unit unit : _combatUnits)
        {
            if (!unit ||
                !unit->exists() ||
                !unit->isCompleted() ||
                unit->getType() != BWAPI::UnitTypes::Terran_Marine ||
                clearanceSquad.containsUnit(unit) ||
                !_squadData.canAssignUnitToSquad(unit, clearanceSquad))
            {
                continue;
            }

            const int distance = unit->getDistance(blocker);
            if (distance < closestDistance)
            {
                closestDistance = distance;
                closestMarine = unit;
            }
        }

        if (!closestMarine)
        {
            break;
        }

        _squadData.assignUnitToSquad(closestMarine, clearanceSquad);
        ++squadMarines;
    }

    clearanceSquad.setOrder(
        SquadOrder(
            SquadOrderTypes::DestroyNeutral,
            blocker->getInitialPosition(),
            5 * 32,
            false,
            "Clear Benzene worker route"));
}

bool CombatCommander::isProxyFactoryVulture(BWAPI::Unit unit) const
{
    return
        unit &&
        unit->exists() &&
        unit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
        _proxyFactoryVultureIDs.find(unit->getID()) !=
            _proxyFactoryVultureIDs.end();
}

// The first ProxyFactory Vultures are the attack itself, not reinforcements for
// the normal Ground squad. Keep them under an aggressive order for the whole
// proxy attack. A time/out-of-book cutoff used to dissolve this squad while the
// Vultures were still across the map; Ground then immediately ordered them to
// join the Vultures produced at home.
void CombatCommander::updateProxyFactoryVultureSquad()
{
    const std::string squadName = "ProxyFactoryVultures";
    const bool proxyStrategy =
        Config::Strategy::StrategyName == "ProxyFactory";
    const bool proxyPhaseActive =
        proxyStrategy &&
        !StrategyManager::Instance().pureAirPlanActive();

    if (!proxyPhaseActive)
    {
        if (_squadData.squadExists(squadName))
        {
            _squadData.removeSquad(squadName);
        }
        _proxyFactoryVultureIDs.clear();
        return;
    }

    std::vector<BWAPI::Unit> candidates;
    for (BWAPI::Unit unit : _combatUnits)
    {
        if (unit &&
            unit->exists() &&
            unit->isCompleted() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        {
            candidates.push_back(unit);
        }
    }

    if (candidates.empty() && !_squadData.squadExists(squadName))
    {
        return;
    }

    Squad& proxySquad = _squadData.squadExists(squadName)
        ? _squadData.getSquad(squadName)
        : _squadData.createSquad(squadName, ProxyFactoryVulturePriority);

    Base* enemyMain = the.bases.enemyStart();
    const BWAPI::Position enemyPosition =
        enemyMain && enemyMain->getPosition().isValid()
        ? enemyMain->getPosition()
        : BWAPI::Position(
            BWAPI::Broodwar->mapWidth() * 16,
            BWAPI::Broodwar->mapHeight() * 16);

    std::sort(
        candidates.begin(),
        candidates.end(),
        [enemyPosition](BWAPI::Unit a, BWAPI::Unit b)
        {
            const int da = a->getDistance(enemyPosition);
            const int db = b->getDistance(enemyPosition);
            return da != db ? da < db : a->getID() < b->getID();
        });

    const int wantedVultures = 3;
    for (BWAPI::Unit vulture : candidates)
    {
        if ((int)proxySquad.getUnits().size() >= wantedVultures)
        {
            break;
        }

        if (!proxySquad.containsUnit(vulture) &&
            _squadData.canAssignUnitToSquad(vulture, proxySquad))
        {
            _squadData.assignUnitToSquad(vulture, proxySquad);
        }
    }

    _proxyFactoryVultureIDs.clear();
    for (BWAPI::Unit vulture : proxySquad.getUnits())
    {
        if (vulture && vulture->exists())
        {
            _proxyFactoryVultureIDs.insert(vulture->getID());
        }
    }

    if (!proxySquad.isEmpty())
    {
        proxySquad.setOrder(getAttackOrder(&proxySquad, true));
    }
}

namespace
{
    const std::string WorkerCounterSquadPrefix = "WorkerCounter ";

    bool workerAttackIsInsideOwnedBase(BWAPI::Unit victim)
    {
        if (!victim ||
            !victim->exists() ||
            victim->getPlayer() != the.self() ||
            !victim->getPosition().isValid() ||
            (!victim->getType().isWorker() &&
             !victim->getType().isBuilding()))
        {
            return false;
        }

        // Wall buildings and forward construction can lie outside the base
        // region even though an enemy worker is attacking our property.
        if (victim->getType().isBuilding())
        {
            return true;
        }

        for (Base* base : the.bases.getAll())
        {
            if (base &&
                base->getOwner() == the.self() &&
                (base->isInBase(victim) ||
                 victim->getDistance(base->getCenter()) <= 11 * 32))
            {
                return true;
            }
        }

        return false;
    }

    // Return the friendly worker/building which the enemy worker is actually
    // attacking. Merely walking through the base or scouting is deliberately
    // not enough to pull SCVs off minerals.
    BWAPI::Unit workerRushVictim(BWAPI::Unit enemyWorker)
    {
        static std::map<int, std::pair<int, int>> recentAttacks;
        static int lastObservedFrame = -1;
        const int now = the.now();
        if (now < lastObservedFrame)
        {
            recentAttacks.clear();
        }
        lastObservedFrame = now;

        if (!enemyWorker ||
            !enemyWorker->exists() ||
            !enemyWorker->isVisible() ||
            !enemyWorker->isCompleted() ||
            !enemyWorker->getType().isWorker() ||
            enemyWorker->isFlying())
        {
            return nullptr;
        }

        const bool attackOrder =
            UnitUtil::AttackOrder(enemyWorker) ||
            enemyWorker->isAttacking() ||
            enemyWorker->isAttackFrame();
        BWAPI::Unit victim = enemyWorker->getOrderTarget();
        if (!workerAttackIsInsideOwnedBase(victim))
        {
            victim = enemyWorker->getTarget();
        }

        if (attackOrder && workerAttackIsInsideOwnedBase(victim) &&
            enemyWorker->getDistance(victim) <= 3 * 32)
        {
            recentAttacks[enemyWorker->getID()] =
                std::make_pair(victim->getID(), now);
            return victim;
        }

        // BWAPI can briefly clear a worker's target between swings. A nearby
        // owned worker/building taking damage is direct local evidence.
        for (BWAPI::Unit nearby : BWAPI::Broodwar->getUnitsInRadius(
            enemyWorker->getPosition(), 3 * 32, BWAPI::Filter::IsOwned))
        {
            if (workerAttackIsInsideOwnedBase(nearby) &&
                nearby->isUnderAttack() &&
                enemyWorker->getDistance(nearby) <= 2 * 32)
            {
                recentAttacks[enemyWorker->getID()] =
                    std::make_pair(nearby->getID(), now);
                return nearby;
            }
        }

        const auto found = recentAttacks.find(enemyWorker->getID());
        if (found != recentAttacks.end())
        {
            BWAPI::Unit recentVictim =
                BWAPI::Broodwar->getUnit(found->second.first);
            if (now - found->second.second <= 24 &&
                workerAttackIsInsideOwnedBase(recentVictim) &&
                enemyWorker->getDistance(recentVictim) <= 3 * 32)
            {
                return recentVictim;
            }
            recentAttacks.erase(found);
        }
        return nullptr;
    }
}

// Counter isolated worker harassment with three healthy workers per attacker.
// The attacked worker participates unless it is badly damaged; a damaged one
// is temporarily removed from mining and sent back to the nearest depot.
void CombatCommander::updateWorkerCounterAttackSquads()
{
    std::vector<BWAPI::Unit> attackers;
    std::map<int, BWAPI::Unit> victimsByAttacker;
    if (the.selfRace() == BWAPI::Races::Terran &&
        Config::Micro::WorkersDefendRush)
    {
        for (BWAPI::Unit enemy : the.enemy()->getUnits())
        {
            BWAPI::Unit victim = workerRushVictim(enemy);
            if (victim)
            {
                attackers.push_back(enemy);
                victimsByAttacker[enemy->getID()] = victim;
            }
        }
    }

    std::sort(
        attackers.begin(),
        attackers.end(),
        [](BWAPI::Unit left, BWAPI::Unit right)
        {
            return left->getID() < right->getID();
        });

    std::set<std::string> activeSquadNames;
    for (BWAPI::Unit attacker : attackers)
    {
        activeSquadNames.insert(
            WorkerCounterSquadPrefix + std::to_string(attacker->getID()));
    }

    // Removing the squad calls finishedWithWorker() for every borrowed SCV.
    // Thus an attacker which stops attacking releases its defenders within the
    // four-frame local-reaction cadence, without waiting for OpponentModel to
    // change its learned opening classification.
    std::vector<std::string> obsoleteSquads;
    for (const auto& entry : _squadData.getSquads())
    {
        if (entry.first.compare(
                0,
                WorkerCounterSquadPrefix.size(),
                WorkerCounterSquadPrefix) == 0 &&
            activeSquadNames.find(entry.first) == activeSquadNames.end())
        {
            obsoleteSquads.push_back(entry.first);
        }
    }

    for (const std::string& name : obsoleteSquads)
    {
        _squadData.removeSquad(name);
    }

    struct WorkerCounter
    {
        BWAPI::Unit attacker;
        Squad* squad;
    };

    std::vector<WorkerCounter> counters;
    for (BWAPI::Unit attacker : attackers)
    {
        const std::string squadName =
            WorkerCounterSquadPrefix + std::to_string(attacker->getID());

        Squad& squad = _squadData.squadExists(squadName)
            ? _squadData.getSquad(squadName)
            : _squadData.createSquad(squadName, WorkerCounterPriority);

        SquadOrder counterOrder(
            SquadOrderTypes::Attack,
            attacker->getPosition(),
            3 * 32,
            false,
            "3 SCVs vs attacking worker");
        counterOrder.setKey("worker-counter");
        squad.setOrder(counterOrder);
        squad.setFightVisible(true);
        squad.setMeatgrinder(true);
        squad.setCombatSimRadius(5 * 32);
        counters.push_back({ attacker, &squad });
    }

    // Fill in rounds (one SCV per attacker, then the second, then the third).
    // With six Probes against nine SCVs this gives every attacker attention,
    // instead of letting the first three targets consume the whole mineral line.
    const size_t defendersPerAttacker = 3;
    for (size_t round = 0; round < defendersPerAttacker; ++round)
    {
        for (WorkerCounter& counter : counters)
        {
            if (counter.squad->getUnits().size() > round)
            {
                continue;
            }

            BWAPI::Unit bestWorker = nullptr;
            int bestScore = MAX_DISTANCE;

            for (BWAPI::Unit worker : _combatUnits)
            {
                if (!worker ||
                    !worker->exists() ||
                    !worker->isCompleted() ||
                    worker->getType() != BWAPI::UnitTypes::Terran_SCV ||
                    !WorkerManager::Instance().isFree(worker) ||
                    !_squadData.canAssignUnitToSquad(
                        worker, *counter.squad))
                {
                    continue;
                }

                const int distance = worker->getDistance(counter.attacker);
                if (distance > 14 * 32)
                {
                    continue;
                }

                // Prefer a nearby healthy SCV, then one without cargo. Low-HP
                // workers remain eligible when the base would otherwise die.
                const int missingHitPoints =
                    worker->getType().maxHitPoints() - worker->getHitPoints();
                const int cargoPenalty =
                    worker->isCarryingGas() ? 96 :
                    worker->isCarryingMinerals() ? 64 : 0;
                const int score =
                    distance + 5 * missingHitPoints + cargoPenalty -
                    (victimsByAttacker[counter.attacker->getID()] == worker &&
                     worker->getHitPoints() >= 30 ? 160 : 0);

                if (score < bestScore)
                {
                    bestScore = score;
                    bestWorker = worker;
                }
            }

            if (bestWorker)
            {
                // assignUnitToSquad() may first release the worker from a
                // lower-priority squad, so set the Combat job afterwards.
                _squadData.assignUnitToSquad(
                    bestWorker, *counter.squad);
                WorkerManager::Instance().setCombatWorker(bestWorker);
            }
        }
    }

    // Hand back any legacy emergency escape workers as well. The current
    // implementation does not leave them detached from a squad.
    for (int workerID : _escapingWorkerIDs)
    {
        BWAPI::Unit worker = BWAPI::Broodwar->getUnit(workerID);
        if (worker &&
            worker->exists() &&
            worker->getType().isWorker() &&
            !WorkerManager::Instance().isBuilder(worker) &&
            !WorkerManager::Instance().isWorkerScout(worker))
        {
            WorkerManager::Instance().finishedWithWorker(worker);
        }
    }
    _escapingWorkerIDs.clear();
}

// Zerg only.
// Possibly assign one zergling to clear spider mines or non-attacking enemies from our natural.
// TODO This is disabled (not called). It's not in working order.


// The recon squad is allowed up to a certain "weight" of units.
int CombatCommander::weighReconUnit(const BWAPI::Unit unit) const
{
    if (isExpansionSupportVulture(unit))
    {
        return 0;
    }

    return weighReconUnit(unit->getType());
}

// The recon squad is allowed up to a certain "weight" of units.
int CombatCommander::weighReconUnit(const BWAPI::UnitType type) const
{
    if (type == BWAPI::UnitTypes::Terran_Marine) return 2;
    if (type == BWAPI::UnitTypes::Terran_Vulture) return 4;
    // Tanks stay with the main army; mobile raiders perform reconnaissance.
    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) return 0;

    return 0;
}

// Keep the same reconnaissance target or switch to a new one, depending.
void CombatCommander::chooseReconTarget(const Squad & reconSquad)
{
    bool change = false;       // switch targets?

    Base * nextTarget = getReconLocation();

    // There is nowhere that we need to see. Change to the invalid target.
    if (!nextTarget)
    {
        change = true;
    }

    // If the current target is invalid, find one.
    else if (!_reconTarget)
    {
        change = true;
    }

    // If we have spent too long on one target, then probably we haven't been able to reach it.
    else if (the.now() - _lastReconTargetChange >= ReconTargetTimeout)
    {
        change = true;
    }

    // The recon squad just lost all its members, a sign that it was going somewhere too dangerous.
    // Though it could be that they were all assigned back to the Ground squad.
    else if (_reconSquadAlive && reconSquad.isEmpty())
    {
        change = true;
    }

    // If the target is in sight (of any unit, not only the recon squad) and empty of enemies, we're done.
    else if (BWAPI::Broodwar->isVisible(_reconTarget->getCenterTile().x, _reconTarget->getCenterTile().y))
    {
        BWAPI::Unitset enemies;
        MapGrid::Instance().getUnits(enemies, _reconTarget->getCenter(), ReconRadius, false, true);
        // We don't particularly care about air units, even when we could engage them.
        for (auto it = enemies.begin(); it != enemies.end(); )
        {
            if ((*it)->isFlying())
            {
                it = enemies.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (enemies.empty())
        {
            change = true;
        }
    }

    if (change)
    {
        _reconTarget = nextTarget;
        _lastReconTargetChange = the.now();
    }
}

// Choose an empty base location for the recon squad to check out, or null if none.
// Called only by setReconTarget().
Base * CombatCommander::getReconLocation() const
{
    std::vector<Base *> choices;

    // Visit never-explored neutral expansion locations first. Previously a
    // random base that merely fell out of current vision could be selected
    // again and again while other expansion spots stayed black forever.
    for (Base* base : the.bases.getAll())
    {
        if (base->owner == BWAPI::Broodwar->neutral() &&
            !base->isExplored() &&
            (the.bases.isIslandStart() || the.bases.connectedToStart(base->getTilePosition())))
        {
            choices.push_back(base);
        }
    }

    // After every reachable neutral base has been visited once, ordinary
    // revisits of currently unseen bases are useful again.
    if (choices.empty())
    {
        for (Base* base : the.bases.getAll())
        {
            if (base->owner == BWAPI::Broodwar->neutral() &&
                !base->isVisible() &&
                (the.bases.isIslandStart() || the.bases.connectedToStart(base->getTilePosition())))
            {
                choices.push_back(base);
            }
        }
    }

    if (choices.empty())
    {
        return nullptr;
    }

    // Choose randomly.
    // We may choose the same target we already have. That's OK; if there's another choice,
    // we'll probably switch to it soon.
    Base * base = choices.at(Random::Instance().index(choices.size()));
    return base;
}

// Form the ground squad and the flying squad, the main attack squads.
// NOTE Arbiters and guardians go into the ground squad.
//      Devourers are flying squad if it exists, otherwise ground.
//		Carriers are flying squad if it already exists or if the carrier count is high enough.
//      Detectors are assigned specially.
//      Other air units always go into the flying squad (except scourge, they are in their own squad).
void CombatCommander::updateAttackSquads()
{
    Squad & groundSquad = _squadData.getSquad("Ground");
    Squad & flyingSquad = _squadData.getSquad("Flying");
    Squad& antiAirSquad = _squadData.getSquad("AntiAir");

    const bool pureAirMode =
        StrategyManager::Instance().pureAirPlanActive();
    const bool bioPureAirMode =
        pureAirMode &&
        StrategyManager::Instance().getOpeningGroup() == "bio";

    if (pureAirMode)
    {
        // PureAir has one mobile command owner. Clear earlier harass roles;
        // the expansion-blocker lease remains owned by BuildingManager.
        _lockedHarassVultures.clear();
    }


    bool flyingSquadExists = false;
    for (BWAPI::Unit unit : flyingSquad.getUnits())
    {
        if (isFlyingSquadUnit(unit->getType()))
        {
            flyingSquadExists = true;
            break;
        }
    }

    for (BWAPI::Unit unit : _combatUnits)
    {
        if (isExpansionFerryDropship(unit) ||
            WorkerManager::Instance().isExpansionFerryWorker(unit))
        {
            continue;
        }

        if (isExpansionSupportVulture(unit) ||
            isExpansionFerryDropship(unit) ||
            WorkerManager::Instance().isExpansionFerryWorker(unit))
        {
            Squad* currentSquad = _squadData.getUnitSquad(unit);
            if (currentSquad)
            {
                currentSquad->removeUnit(unit);
            }
            continue;
        }

        if (pureAirMode &&
            (unit->getType() == BWAPI::UnitTypes::Terran_Goliath ||
             unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
             unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
             unit->getType() == BWAPI::UnitTypes::Terran_Vulture ||
             unit->getType() == BWAPI::UnitTypes::Terran_Ghost ||
             unit->getType() == BWAPI::UnitTypes::Terran_Valkyrie ||
             (bioPureAirMode &&
                (unit->getType() == BWAPI::UnitTypes::Terran_Marine ||
                 unit->getType() == BWAPI::UnitTypes::Terran_Medic))))
        {
            // Higher-priority local defense keeps ownership while a base is
            // actually threatened. Remove only stale/offensive owners which
            // would otherwise make this unit launch a separate attack.
            Squad* currentSquad = _squadData.getUnitSquad(unit);
            if (currentSquad && currentSquad != &antiAirSquad && !unit->isLoaded())
            {
                const std::string& owner = currentSquad->getName();
                const bool independentOffensiveOwner =
                    owner == "Ground" ||
                    owner == "Flying" ||
                    owner == "Recon" ||
                    owner == "Drop" ||
                    owner == "Drop1" ||
                    owner == "Drop2" ||
                    owner == "VultureHarass1" ||
                    owner == "VultureHarass2" ||
                    owner == "ProxyFactoryVultures" ||
                    owner.compare(0, 6, "Watch ") == 0;

                if (independentOffensiveOwner)
                {
                    currentSquad->removeUnit(unit);
                }
            }

            if (_squadData.canAssignUnitToSquad(unit, antiAirSquad))
            {
                _squadData.assignUnitToSquad(unit, antiAirSquad);
            }

            continue;
        }

        if (isFlyingSquadUnit(unit->getType()))
        {
            if (_squadData.canAssignUnitToSquad(unit, flyingSquad))
            {
                _squadData.assignUnitToSquad(unit, flyingSquad);
            }
        }

        if (unit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
            isLockedHarassVulture(unit))
        {
            continue;
        }

        // Certain flyers go into the flying squad only if it already exists.
        // Otherwise they go into the ground squad.
        else if (isOptionalFlyingSquadUnit(unit->getType()))
        {
            if (flyingSquadExists)
            {
                if (groundSquad.containsUnit(unit))
                {
                    groundSquad.removeUnit(unit);
                }
                if (_squadData.canAssignUnitToSquad(unit, flyingSquad))
                {
                    _squadData.assignUnitToSquad(unit, flyingSquad);
                }
            }
            else
            {
                if (flyingSquad.containsUnit(unit))
                {
                    flyingSquad.removeUnit(unit);
                }
                if (_squadData.canAssignUnitToSquad(unit, groundSquad))
                {
                    _squadData.assignUnitToSquad(unit, groundSquad);
                }
            }
        }

        // isGroundSquadUnit() is defined as a catchall, so it has to go last.
        else if (isGroundSquadUnit(unit->getType()))
        {
            if (tankDropEnabled() && isReservedTankDropVulture(unit))
            {
                continue;
            }

            if (unit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
                isLockedHarassVulture(unit))
            {
                continue;
            }

            if (_squadData.canAssignUnitToSquad(unit, groundSquad))
            {
                _squadData.assignUnitToSquad(unit, groundSquad);
            }
        }
    }

    // Add or remove detectors. In PureAir the single Science Vessel belongs to
    // the anti-air formation; assigning it to Ground/Flying made it leave the
    // Goliaths and miss both detection and EMP opportunities.
    bool wantDetector = wantSquadDetectors();
    if (pureAirMode)
    {
        maybeAssignDetector(antiAirSquad, true);
    }
    else
    {
        maybeAssignDetector(groundSquad, wantDetector);
        maybeAssignDetector(flyingSquad, wantDetector);
    }

    if (pureAirMode)
    {
        const int completedGoliaths =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Goliath);

        const bool charonCompleted =
            the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) > 0;

        const int knownCarriers = rememberedEnemyCarrierCountForCombat();
        const int attackThreshold = knownCarriers > 0
            ? std::max(8, std::min(30, 3 * knownCarriers + 4))
            : 8;

        const bool mechAntiAirReadyToAttack =
            completedGoliaths >= attackThreshold &&
            (knownCarriers == 0 || charonCompleted);

        // Bio PureAir advances immediately with whatever Marines and
        // Goliaths already exist. Only the tank-opening reaction keeps
        // the conservative Goliath/Charon readiness gate.
        const bool antiAirReadyToAttack =
            bioPureAirMode || mechAntiAirReadyToAttack;

        // Base defense borrows only a bounded local garrison. Never turn the
        // complete AntiAir army around for a recurring Carrier raid.
        antiAirSquad.setOrder(
            getAttackOrder(
                &antiAirSquad,
                antiAirReadyToAttack,
                !antiAirReadyToAttack));
    }
    else
    {
        antiAirSquad.clear();
    }

    // A BioTank book may issue "go aggressive" before the first Marines,
    // Medic, Stim and Siege Tank all finish. Keep the entire Ground squad on
    // the defensive front until that opening push can move as one force.
    groundSquad.setOrder(getAttackOrder(
        &groundSquad,
        false,
        !StrategyManager::Instance().bioTankPushReady()));

    flyingSquad.setOrder(getAttackOrder(&flyingSquad));
}

// Unit definitely belongs in the Flying squad.
bool CombatCommander::isFlyingSquadUnit(const BWAPI::UnitType type) const
{
    return type == BWAPI::UnitTypes::Terran_Wraith ||
        type == BWAPI::UnitTypes::Terran_Valkyrie ||
        type == BWAPI::UnitTypes::Terran_Battlecruiser;
}

// Unit belongs in the Flying squad if the Flying squad exists, otherwise the Ground squad.
// If carriers are independent, then the flying squad exists.
bool CombatCommander::isOptionalFlyingSquadUnit(const BWAPI::UnitType type) const
{
    return false;
}

// Unit belongs in the ground squad.
// With the current definition, it includes everything except workers, so it captures
// everything that is not already taken. It must be the last condition checked.
// NOTE This includes queens.
bool CombatCommander::isGroundSquadUnit(const BWAPI::UnitType type) const
{
    return
        !type.isDetector() &&
        !type.isWorker();
}

int CombatCommander::countReservedTankDropVultures()
{
    int count = 0;

    for (const auto& squadName : { std::string("Drop1"), std::string("Drop2") })
    {
        if (!_squadData.squadExists(squadName))
        {
            continue;
        }

        const Squad& squad = _squadData.getSquad(squadName);
        for (BWAPI::Unit unit : squad.getUnits())
        {
            if (unit &&
                unit->exists() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
            {
                ++count;
            }
        }
    }

    return count;
}

bool CombatCommander::isReservedTankDropVulture(BWAPI::Unit unit)
{
    if (!unit || !unit->exists())
    {
        return false;
    }

    if (unit->getType() != BWAPI::UnitTypes::Terran_Vulture)
    {
        return false;
    }

    if (_tankDropInitialVultureIDs.find(unit->getID()) != _tankDropInitialVultureIDs.end())
    {
        return false;
    }

    if (!_squadData.squadExists("Drop1") || !_squadData.squadExists("Drop2"))
    {
        return false;
    }

    const Squad& drop1 = _squadData.getSquad("Drop1");
    const Squad& drop2 = _squadData.getSquad("Drop2");

    return drop1.containsUnit(unit) || drop2.containsUnit(unit);
}

bool CombatCommander::tankDropEnabled() const
{
    

    if (StrategyManager::Instance().getOpeningGroup() != "tanks")
    {
        return false;
    }

    if (StrategyManager::Instance().pureAirPlanActive())
    {
        return false;
    }

    if (the.bases.completedBaseCount(the.self()) < 2)
    {
        return false;
    }

    if (!the.bases.enemyStart())
    {
        return false;
    }

    // The launch limit prevents a new wave; it must not disable updates for a
    // wave which is already in flight. Otherwise the second launch increments
    // the counter to two and updateDropSquads() abandons both transports at the
    // first waypoint on the next update.
    return
        the.my.completed.count(BWAPI::UnitTypes::Terran_Dropship) > 0 ||
        _drop1EverAssigned ||
        _drop2EverAssigned;
}

BWAPI::Position CombatCommander::getTankDropUnloadPoint(int squadIndex) const
{
    Base* enemyMain = the.bases.enemyStart();
    if (!enemyMain)
    {
        return the.bases.myMain()
            ? the.bases.myMain()->getPosition()
            : BWAPI::Position(the.self()->getStartLocation());
    }

    BWAPI::Position enemyBasePos = enemyMain->getPosition();

    std::vector<BWAPI::Position> mineralPositions;
    for (BWAPI::Unit mineral : enemyMain->getMinerals())
    {
        if (mineral && mineral->exists() && mineral->getPosition().isValid())
        {
            mineralPositions.push_back(mineral->getPosition());
        }
    }

    if (mineralPositions.empty())
    {
        return enemyBasePos;
    }

    int sumX = 0;
    int sumY = 0;
    for (const BWAPI::Position& p : mineralPositions)
    {
        sumX += p.x;
        sumY += p.y;
    }

    BWAPI::Position mineralCenter(
        sumX / int(mineralPositions.size()),
        sumY / int(mineralPositions.size())
    );

    const int minX = 2 * 32;
    const int maxX = BWAPI::Broodwar->mapWidth() * 32 - 3 * 32;
    const int minY = 2 * 32;
    const int maxY = BWAPI::Broodwar->mapHeight() * 32 - 3 * 32;

    auto clampPos = [&](BWAPI::Position p) -> BWAPI::Position
        {
            p.x = std::max(minX, std::min(p.x, maxX));
            p.y = std::max(minY, std::min(p.y, maxY));
            return p;
        };

    int dx = enemyBasePos.x - mineralCenter.x;
    int dy = enemyBasePos.y - mineralCenter.y;

    double len = std::sqrt(double(dx * dx + dy * dy));
    if (len < 1.0)
    {
        len = 1.0;
    }

    int baseOffsetX = int(double(dx) / len * 96.0);
    int baseOffsetY = int(double(dy) / len * 96.0);
    const int sideOffset = squadIndex == 0 ? -48 : 48;

    BWAPI::Position preferred = clampPos(BWAPI::Position(
        mineralCenter.x + baseOffsetX - int(double(dy) / len * sideOffset),
        mineralCenter.y + baseOffsetY + int(double(dx) / len * sideOffset)
    ));

    std::vector<BWAPI::Position> candidates;
    candidates.push_back(preferred);

    static const BWAPI::Position offsets[] =
    {
        BWAPI::Position(96,   0),
        BWAPI::Position(-96,   0),
        BWAPI::Position(0,  96),
        BWAPI::Position(0, -96),
        BWAPI::Position(64,  64),
        BWAPI::Position(64, -64),
        BWAPI::Position(-64,  64),
        BWAPI::Position(-64, -64),
        BWAPI::Position(128,   0),
        BWAPI::Position(-128,  0),
        BWAPI::Position(0, 128),
        BWAPI::Position(0,-128)
    };

    for (const BWAPI::Position& off : offsets)
    {
        candidates.push_back(clampPos(mineralCenter + off));
    }

    auto usable = [&](BWAPI::Position p) -> bool
        {
            if (!p.isValid())
            {
                return false;
            }

            BWAPI::TilePosition tp(p);
            if (!tp.isValid())
            {
                return false;
            }

            return BWAPI::Broodwar->isWalkable(tp.x * 4 + 2, tp.y * 4 + 2);
        };

    for (const BWAPI::Position& p : candidates)
    {
        if (usable(p))
        {
            return p;
        }
    }

    return preferred;
}

void CombatCommander::lockVultureToHarass(BWAPI::Unit unit)
{
    if (unit && unit->exists() && unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
    {
        _lockedHarassVultures.insert(unit);
    }
}

bool CombatCommander::isLockedHarassVulture(BWAPI::Unit unit) const
{
    return _lockedHarassVultures.find(unit) != _lockedHarassVultures.end();
}

void CombatCommander::clearDeadLockedHarassVultures()
{
    for (auto it = _lockedHarassVultures.begin(); it != _lockedHarassVultures.end();)
    {
        if (!(*it) || !(*it)->exists())
        {
            it = _lockedHarassVultures.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool CombatCommander::canReserveExpansionSupportVulture(BWAPI::Unit unit) const
{
    if (!unit ||
        !unit->exists() ||
        !unit->isCompleted() ||
        unit->getType() != BWAPI::UnitTypes::Terran_Vulture)
    {
        return false;
    }

    if (isLockedHarassVulture(unit))
    {
        return false;
    }

    if (_expansionSupportVultureIDs.find(unit->getID()) !=
        _expansionSupportVultureIDs.end())
    {
        return false;
    }

    const Squad* currentSquad = _squadData.getUnitSquad(unit);
    if (!currentSquad)
    {
        return true;
    }

    const std::string& name = currentSquad->getName();

    // Expansion clearing may borrow an ordinary army/recon Vulture, but it
    // must not steal one from a drop, worker harass or active base defense.
    return
        name == "Idle" ||
        name == "Ground" ||
        name == "Recon" ||
        name.compare(0, 6, "Watch ") == 0;
}

bool CombatCommander::reserveExpansionSupportVulture(BWAPI::Unit unit)
{
    if (!canReserveExpansionSupportVulture(unit))
    {
        return false;
    }

    Squad* currentSquad = _squadData.getUnitSquad(unit);
    if (currentSquad)
    {
        currentSquad->removeUnit(unit);
    }

    _expansionSupportVultureIDs.insert(unit->getID());
    return true;
}

void CombatCommander::releaseExpansionSupportVulture(int unitID)
{
    if (unitID >= 0)
    {
        _expansionSupportVultureIDs.erase(unitID);
    }
}

bool CombatCommander::isExpansionSupportVulture(BWAPI::Unit unit) const
{
    return
        unit &&
        unit->exists() &&
        unit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
        _expansionSupportVultureIDs.find(unit->getID()) !=
            _expansionSupportVultureIDs.end();
}

bool CombatCommander::reserveExpansionFerryDropship(BWAPI::Unit ship)
{
    if (!ship || !ship->exists() || !ship->isCompleted() ||
        ship->getType() != BWAPI::UnitTypes::Terran_Dropship ||
        !ship->getLoadedUnits().empty())
    {
        return false;
    }
    if (isExpansionFerryDropship(ship))
    {
        return true;
    }

    Squad* squad = _squadData.getUnitSquad(ship);
    if (squad)
    {
        const std::string& name = squad->getName();
        if (name == "Drop1" || name == "Drop2")
        {
            if (_tankDropPhase[name] != 0)
            {
                return false;
            }
        }
        else if (name == "Drop")
        {
            if (squad->getOrder().getType() == SquadOrderTypes::Drop)
            {
                return false;
            }
        }
        else if (name != "Idle")
        {
            return false;
        }
        squad->removeUnit(ship);
    }
    _expansionFerryDropshipIDs.insert(ship->getID());
    return true;
}

void CombatCommander::releaseExpansionFerryDropship(BWAPI::Unit ship)
{
    if (ship)
    {
        _expansionFerryDropshipIDs.erase(ship->getID());
    }
}

bool CombatCommander::isExpansionFerryDropship(BWAPI::Unit ship) const
{
    return ship && ship->exists() &&
        _expansionFerryDropshipIDs.find(ship->getID()) !=
            _expansionFerryDropshipIDs.end();
}

void CombatCommander::clearDeadExpansionSupportVultures()
{
    std::set<int> liveVultureIDs;

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit &&
            unit->exists() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        {
            liveVultureIDs.insert(unit->getID());
        }
    }

    for (auto it = _expansionSupportVultureIDs.begin();
        it != _expansionSupportVultureIDs.end(); )
    {
        if (liveVultureIDs.find(*it) == liveVultureIDs.end())
        {
            it = _expansionSupportVultureIDs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}


void CombatCommander::updateTankDropSquads()
{
    BWAPI::Position homePos =
        the.bases.myMain() ?
        the.bases.myMain()->getPosition() :
        BWAPI::Position(the.self()->getStartLocation());

    if (!_squadData.squadExists("VultureHarass1"))
    {
        _squadData.createSquad("VultureHarass1", VultureHarassPriority).
            setOrder(SquadOrder(SquadOrderTypes::Hold, homePos, AttackRadius, false, "Wait for vulture harass 1"));
    }

    if (!_squadData.squadExists("VultureHarass2"))
    {
        _squadData.createSquad("VultureHarass2", VultureHarassPriority).
            setOrder(SquadOrder(SquadOrderTypes::Hold, homePos, AttackRadius, false, "Wait for vulture harass 2"));
    }

    Squad& harass1 = _squadData.getSquad("VultureHarass1");
    Squad& harass2 = _squadData.getSquad("VultureHarass2");


    if (!_squadData.squadExists("Drop1"))
    {
        _squadData.createSquad("Drop1", DropPriority).
            setOrder(SquadOrder(SquadOrderTypes::Hold, homePos, AttackRadius, false, "Wait for tank drop 1"));
    }

    if (!_squadData.squadExists("Drop2"))
    {
        _squadData.createSquad("Drop2", DropPriority).
            setOrder(SquadOrder(SquadOrderTypes::Hold, homePos, AttackRadius, false, "Wait for tank drop 2"));
    }


    if (!_tankDropSeeded)
    {
        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (unit &&
                unit->exists() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
            {
                _tankDropInitialVultureIDs.insert(unit->getID());
            }
        }

        _tankDropSeeded = true;
    }

    Squad& drop1 = _squadData.getSquad("Drop1");
    Squad& drop2 = _squadData.getSquad("Drop2");
    Squad& groundSquad = _squadData.getSquad("Ground");

    const int completedDropships = the.my.completed.count(BWAPI::UnitTypes::Terran_Dropship);

    if (completedDropships == 0 || StrategyManager::Instance().pureAirPlanActive())
    {
        for (Squad* dropSquad : { &drop1, &drop2 })
        {
            std::vector<BWAPI::Unit> vulturesToRelease;

            for (BWAPI::Unit unit : dropSquad->getUnits())
            {
                if (!unit || !unit->exists())
                {
                    continue;
                }

                if (unit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
                    !unit->isLoaded())
                {
                    vulturesToRelease.push_back(unit);
                }
            }

            for (BWAPI::Unit unit : vulturesToRelease)
            {
                dropSquad->removeUnit(unit);

                if (_squadData.canAssignUnitToSquad(unit, groundSquad))
                {
                    _squadData.assignUnitToSquad(unit, groundSquad);
                }
            }
        }

        drop1.setOrder(SquadOrder(SquadOrderTypes::Hold, homePos, AttackRadius, false, "Drop disabled"));
        drop2.setOrder(SquadOrder(SquadOrderTypes::Hold, homePos, AttackRadius, false, "Drop disabled"));

        return;
    }

    auto getTransport = [](const Squad& squad) -> BWAPI::Unit
        {
            for (BWAPI::Unit unit : squad.getUnits())
            {
                if (unit &&
                    unit->exists() &&
                    unit->isFlying() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Dropship)
                {
                    return unit;
                }
            }
            return nullptr;
        };

    auto countAssignedVultures = [](const Squad& squad) -> int
        {
            int count = 0;
            for (BWAPI::Unit unit : squad.getUnits())
            {
                if (unit &&
                    unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
                {
                    ++count;
                }
            }
            return count;
        };

    auto hasUnloadedVulture = [](const Squad& squad) -> bool
        {
            for (BWAPI::Unit unit : squad.getUnits())
            {
                if (unit &&
                    unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
                    !unit->isLoaded())
                {
                    return true;
                }
            }
            return false;
        };

    auto finishDropWaveIfAnyVultureUnloaded =
        [&](Squad& dropSquad, Squad& harassSquad, const std::string& squadName)
        {
            BWAPI::Unit transport = getTransport(dropSquad);

            const int phase = _tankDropPhase[squadName];
            const std::string status = dropSquad.getOrder().getStatus();

            const bool unloadingStage =
                (phase >= 5) ||
                (status == "Tank vulture drop") ||
                (status == "Emergency unload") ||
                _tankDropEmergencyMode[squadName];

            if (!unloadingStage)
            {
                return;
            }

            std::vector<BWAPI::Unit> unloadedVultures;
            bool anyLoadedVulturesStillInDropSquad = false;

            for (BWAPI::Unit unit : dropSquad.getUnits())
            {
                if (!unit || !unit->exists() || unit->getType() != BWAPI::UnitTypes::Terran_Vulture)
                {
                    continue;
                }

                if (unit->isLoaded())
                {
                    anyLoadedVulturesStillInDropSquad = true;
                }
                else
                {
                    unloadedVultures.push_back(unit);
                }
            }

            if (unloadedVultures.empty())
            {
                return;
            }

            for (BWAPI::Unit unit : unloadedVultures)
            {
                lockVultureToHarass(unit);
                dropSquad.removeUnit(unit);
                harassSquad.addUnit(unit);
            }

            if (!transport || !transport->exists())
            {
                _tankDropEmergencyMode[squadName] = false;
                _tankDropPhase[squadName] = 0;
                _tankDropUnloadStartFrame[squadName] = -1;
                _tankDropReturnIssued[squadName] = false;
                _tankDropLaunchIssued[squadName] = false;
                return;
            }

            if (anyLoadedVulturesStillInDropSquad)
            {
                return;
            }

            _tankDropEmergencyMode[squadName] = false;

            dropSquad.setOrder(
                SquadOrder(
                    SquadOrderTypes::Drop,
                    homePos,
                    AttackRadius,
                    false,
                    "Return after drop"
                )
            );

            _tankDropPhase[squadName] = 6;
            _tankDropUnloadStartFrame[squadName] = -1;
            _tankDropReturnIssued[squadName] = false;
            _tankDropLaunchIssued[squadName] = false;
        };

    auto handleReturningTransport = [&](Squad& dropSquad, const std::string& squadName)
        {
            BWAPI::Unit transport = getTransport(dropSquad);
            if (!transport || !transport->exists())
            {
                _tankDropEmergencyMode[squadName] = false;
                return;
            }

            if (_tankDropPhase[squadName] != 6 ||
                dropSquad.getOrder().getStatus() != "Return after drop")
            {
                return;
            }

            bool anyVulturesStillInDropSquad = false;
            for (BWAPI::Unit unit : dropSquad.getUnits())
            {
                if (unit &&
                    unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
                {
                    anyVulturesStillInDropSquad = true;
                    break;
                }
            }

            if (anyVulturesStillInDropSquad)
            {
                return;
            }

            DrawUnitTargetTextMap(
                transport->getPosition() + BWAPI::Position(0, 16),
                "tankDrop return"
            );

            if (transport->getDistance(homePos) <= 6 * 32)
            {
                const bool launchLimitReached =
                    (_tankDropLaunchCount[squadName] >= 1) ||
                    (_tankDropTotalLaunches >= 2);

                dropSquad.setOrder(
                    SquadOrder(
                        SquadOrderTypes::Hold,
                        homePos,
                        AttackRadius,
                        false,
                        launchLimitReached ? "Tank drop limit reached" : "Wait for next tank drop"
                    )
                );

                _tankDropPhase[squadName] = 0;
                _tankDropUnloadStartFrame[squadName] = -1;
                _tankDropReturnIssued[squadName] = false;
                _tankDropLaunchIssued[squadName] = false;
            }
        };

    auto getEnemyWorkerType = []() -> BWAPI::UnitType
        {
            BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();

            if (enemyRace == BWAPI::Races::Terran)
            {
                return BWAPI::UnitTypes::Terran_SCV;
            }

            if (enemyRace == BWAPI::Races::Protoss)
            {
                return BWAPI::UnitTypes::Protoss_Probe;
            }

            if (enemyRace == BWAPI::Races::Zerg)
            {
                return BWAPI::UnitTypes::Zerg_Drone;
            }

            return BWAPI::UnitTypes::Unknown;
        };

    auto findNearbyUnloadPos = [](BWAPI::Unit transport, BWAPI::Position center) -> BWAPI::Position
        {
            if (!transport || !transport->exists() || !center.isValid())
            {
                return BWAPI::Positions::Invalid;
            }

            std::vector<BWAPI::Position> candidates =
            {
                center,
                center + BWAPI::Position(32,   0),
                center + BWAPI::Position(-32,   0),
                center + BWAPI::Position(0,  32),
                center + BWAPI::Position(0, -32),
                center + BWAPI::Position(32,  32),
                center + BWAPI::Position(32, -32),
                center + BWAPI::Position(-32,  32),
                center + BWAPI::Position(-32, -32),
                center + BWAPI::Position(64,   0),
                center + BWAPI::Position(-64,   0),
                center + BWAPI::Position(0,  64),
                center + BWAPI::Position(0, -64),
                center + BWAPI::Position(64,  64),
                center + BWAPI::Position(64, -64),
                center + BWAPI::Position(-64,  64),
                center + BWAPI::Position(-64, -64)
            };

            for (const BWAPI::Position& pos : candidates)
            {
                if (pos.isValid() && transport->canUnloadAtPosition(pos))
                {
                    return pos;
                }
            }

            return BWAPI::Positions::Invalid;
        };

    auto findClosestEnemyWorker = [&](BWAPI::Position from) -> BWAPI::Unit
        {
            BWAPI::UnitType workerType = getEnemyWorkerType();
            if (workerType == BWAPI::UnitTypes::Unknown)
            {
                return nullptr;
            }

            BWAPI::Unit best = nullptr;
            int bestDist = 999999;

            for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
            {
                if (!enemy || !enemy->exists() || !enemy->isCompleted())
                {
                    continue;
                }

                if (enemy->getType() != workerType)
                {
                    continue;
                }

                int dist = from.getDistance(enemy->getPosition());
                if (dist < bestDist)
                {
                    bestDist = dist;
                    best = enemy;
                }
            }

            return best;
        };

    auto updateHarassSquad = [&](Squad& harassSquad, int dropIndex)
        {
            auto setHarassOrder = [&](SquadOrder nextOrder, const std::string& key)
            {
                nextOrder.setRaid(true);
                nextOrder.setKey(key);
                harassSquad.setOrder(nextOrder);
            };

            std::vector<BWAPI::Unit> vultures;
            for (BWAPI::Unit unit : harassSquad.getUnits())
            {
                if (unit &&
                    unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
                {
                    vultures.push_back(unit);
                }
            }

            if (vultures.empty())
            {
                harassSquad.setOrder(
                    SquadOrder(
                        SquadOrderTypes::Hold,
                        homePos,
                        AttackRadius,
                        false,
                        "No harass vultures"
                    )
                );
                return;
            }

            BWAPI::Position harassPos = getTankDropUnloadPoint(dropIndex);
            BWAPI::Position center = vultures.front()->getPosition();

            if (center.getDistance(harassPos) > 3 * 32)
            {
                setHarassOrder(
                    SquadOrder(
                        SquadOrderTypes::Attack,
                        harassPos,
                        3 * 32,
                        false,
                        "Approach mineral line"
                    ),
                    "vulture-harass-approach"
                );

                DrawUnitTargetTextMap(
                    center + BWAPI::Position(0, 16),
                    "APPROACH MINERAL LINE"
                );

                BWAPI::Broodwar->drawCircleMap(
                    harassPos,
                    24,
                    BWAPI::Colors::Cyan,
                    false
                );
                return;
            }

            // Workers do not trigger Spider Mines. Do not gate the harass
            // state machine on a mine in their mineral line; combat micro can
            // still lay tactical mines against pursuing ground troops.
            BWAPI::Unit targetWorker = findClosestEnemyWorker(center);

            if (targetWorker)
            {
                setHarassOrder(
                    SquadOrder(
                        SquadOrderTypes::Attack,
                        targetWorker->getPosition(),
                        6 * 32,
                        false,
                        "Focus enemy workers"
                    ),
                    "vulture-harass-focus"
                );

                DrawUnitTargetTextMap(
                    center + BWAPI::Position(0, 16),
                    "FOCUS WORKERS"
                );

                BWAPI::Broodwar->drawCircleMap(
                    targetWorker->getPosition(),
                    20,
                    BWAPI::Colors::Red,
                    false
                );
            }
            else
            {
                setHarassOrder(
                    SquadOrder(
                        SquadOrderTypes::Hold,
                        harassPos,
                        8 * 32,
                        false,
                        "Search enemy workers"
                    ),
                    "vulture-harass-search"
                );

                DrawUnitTargetTextMap(
                    center + BWAPI::Position(0, 16),
                    "SEARCH WORKERS"
                );

                BWAPI::Broodwar->drawCircleMap(
                    harassPos,
                    24,
                    BWAPI::Colors::Orange,
                    false
                );
            }
        };

    auto findEmergencyUnloadPosNear = [](BWAPI::Unit transport, BWAPI::Position center) -> BWAPI::Position
        {
            if (!transport || !transport->exists() || !center.isValid())
            {
                return BWAPI::Positions::Invalid;
            }

            std::vector<BWAPI::Position> candidates =
            {
                center,
                center + BWAPI::Position(32,   0),
                center + BWAPI::Position(-32,   0),
                center + BWAPI::Position(0,  32),
                center + BWAPI::Position(0, -32),
                center + BWAPI::Position(32,  32),
                center + BWAPI::Position(32, -32),
                center + BWAPI::Position(-32,  32),
                center + BWAPI::Position(-32, -32),
                center + BWAPI::Position(64,   0),
                center + BWAPI::Position(-64,   0),
                center + BWAPI::Position(0,  64),
                center + BWAPI::Position(0, -64),
                center + BWAPI::Position(64,  64),
                center + BWAPI::Position(64, -64),
                center + BWAPI::Position(-64,  64),
                center + BWAPI::Position(-64, -64),
                center + BWAPI::Position(96,   0),
                center + BWAPI::Position(-96,   0),
                center + BWAPI::Position(0,  96),
                center + BWAPI::Position(0, -96)
            };

            for (const BWAPI::Position& pos : candidates)
            {
                if (pos.isValid() && transport->canUnloadAtPosition(pos))
                {
                    return pos;
                }
            }

            return BWAPI::Positions::Invalid;
        };


    finishDropWaveIfAnyVultureUnloaded(drop1, harass1, "Drop1");
    finishDropWaveIfAnyVultureUnloaded(drop2, harass2, "Drop2");

    handleReturningTransport(drop1, "Drop1");
    handleReturningTransport(drop2, "Drop2");

    updateHarassSquad(harass1, 0);
    updateHarassSquad(harass2, 1);

    std::vector<Squad*> activeDropSquads;
    activeDropSquads.push_back(&drop1);
    activeDropSquads.push_back(&drop2);

    // The two-launch cap stops only new reservations. Never steal fresh
    // Vultures after both waves have launched, but keep updating any transport
    // which is still flying, unloading or returning.
    if (_tankDropTotalLaunches >= 2)
    {
        for (Squad* squad : activeDropSquads)
        {
            const std::string squadName =
                squad == &drop1 ? "Drop1" : "Drop2";

            if (_tankDropPhase[squadName] != 0)
            {
                continue;
            }

            std::vector<BWAPI::Unit> release;
            for (BWAPI::Unit unit : squad->getUnits())
            {
                if (unit && unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
                    !unit->isLoaded())
                {
                    release.push_back(unit);
                }
            }

            for (BWAPI::Unit unit : release)
            {
                squad->removeUnit(unit);
                if (_squadData.canAssignUnitToSquad(unit, groundSquad))
                {
                    _squadData.assignUnitToSquad(unit, groundSquad);
                }
            }
        }
    }

    auto assignFreeDropshipToSquad = [&](Squad& targetSquad, bool& everAssigned, const std::string& squadName)
        {
            if (getTransport(targetSquad))
            {
                return;
            }

            for (BWAPI::Unit unit : _combatUnits)
            {
                if (!unit || !unit->exists())
                {
                    continue;
                }

                if (unit->getType() != BWAPI::UnitTypes::Terran_Dropship ||
                    !unit->isFlying() || isExpansionFerryDropship(unit))
                {
                    continue;
                }

                if ((_squadData.squadExists("Drop1") && _squadData.getSquad("Drop1").containsUnit(unit)) ||
                    (_squadData.squadExists("Drop2") && _squadData.getSquad("Drop2").containsUnit(unit)))
                {
                    continue;
                }

                if (_squadData.canAssignUnitToSquad(unit, targetSquad))
                {
                    _squadData.assignUnitToSquad(unit, targetSquad);
                    everAssigned = true;
                    DebugTacticalPrintf("Assigned new dropship to %s", squadName.c_str());
                    break;
                }
            }
        };

    if (!_drop1EverAssigned)
    {
        assignFreeDropshipToSquad(drop1, _drop1EverAssigned, "Drop1");
    }

    if (_drop1EverAssigned && !_drop2EverAssigned)
    {
        assignFreeDropshipToSquad(drop2, _drop2EverAssigned, "Drop2");
    }

    if (_drop1EverAssigned && _drop2EverAssigned)
    {
        if (!getTransport(drop1))
        {
            assignFreeDropshipToSquad(drop1, _drop1EverAssigned, "Drop1");
        }
        if (!getTransport(drop2))
        {
            assignFreeDropshipToSquad(drop2, _drop2EverAssigned, "Drop2");
        }
    }


    std::vector<BWAPI::Unit> eligibleNewVultures;
    if (_tankDropTotalLaunches < 2)
    {
        for (BWAPI::Unit unit : _combatUnits)
        {
            if (!unit || !unit->exists() || !unit->isCompleted())
            {
                continue;
            }

            if (unit->getType() != BWAPI::UnitTypes::Terran_Vulture)
            {
                continue;
            }

            if (_tankDropInitialVultureIDs.find(unit->getID()) != _tankDropInitialVultureIDs.end())
            {
                continue;
            }

            if (_tankDropUsedVultureIDs.find(unit->getID()) != _tankDropUsedVultureIDs.end())
            {
                continue;
            }

            if (isExpansionSupportVulture(unit))
            {
                continue;
            }

            eligibleNewVultures.push_back(unit);
        }
    }

    std::sort(
        eligibleNewVultures.begin(),
        eligibleNewVultures.end(),
        [&](BWAPI::Unit a, BWAPI::Unit b)
        {
            return a->getDistance(homePos) < b->getDistance(homePos);
        }
    );

    const int maxReservedVultures = 4;
    int currentlyReserved = countReservedTankDropVultures();
    int freeReserveSlots = std::max(0, maxReservedVultures - currentlyReserved);

    if (int(eligibleNewVultures.size()) > freeReserveSlots)
    {
        eligibleNewVultures.resize(freeReserveSlots);
    }


    int nextVultureIndex = 0;
    for (Squad* squad : activeDropSquads)
    {
        // Never reserve combat Vultures for a transport which does not exist.
        // Previously Drop2 could steal two fresh Vultures while the only
        // Dropship belonged to Drop1, leaving those Vultures AFK at home.
        if (!getTransport(*squad))
        {
            continue;
        }

        int have = countAssignedVultures(*squad);

        // When starting a new payload, reserve the pair together. A lone
        // Vulture should keep fighting with Ground rather than wait idle for
        // an unknown amount of time for the second one.
        if (have == 0 &&
            int(eligibleNewVultures.size()) - nextVultureIndex < 2)
        {
            continue;
        }

        while (have < 2 && nextVultureIndex < int(eligibleNewVultures.size()))
        {
            BWAPI::Unit vulture = eligibleNewVultures[nextVultureIndex++];
            if (_squadData.canAssignUnitToSquad(vulture, *squad))
            {
                _squadData.assignUnitToSquad(vulture, *squad);
                _tankDropUsedVultureIDs.insert(vulture->getID());
                ++have;
            }
        }
    }

    for (int i = 0; i < int(activeDropSquads.size()); ++i)
    {
        Squad* squad = activeDropSquads[i];
        const std::string squadName = (i == 0 ? "Drop1" : "Drop2");
        int& phase = _tankDropPhase[squadName];

        BWAPI::Unit transport = getTransport(*squad);
        int vultureCount = countAssignedVultures(*squad);

        if (transport)
        {
            DrawUnitTargetTextMap(
                transport->getPosition() + BWAPI::Position(0, -16),
                "%s phase=%d assigned=%d loaded=%d order=%d",
                squadName.c_str(),
                phase,
                vultureCount,
                int(transport->getLoadedUnits().size()),
                int(squad->getOrder().getType())
            );
        }

        if (!transport)
        {
            // A drop squad without its own transport must not warehouse
            // Vultures. Return every unloaded one to the normal ground army.
            std::vector<BWAPI::Unit> release;
            for (BWAPI::Unit unit : squad->getUnits())
            {
                if (unit && unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
                    !unit->isLoaded())
                {
                    release.push_back(unit);
                }
            }

            for (BWAPI::Unit unit : release)
            {
                squad->removeUnit(unit);
                if (_squadData.canAssignUnitToSquad(unit, groundSquad))
                {
                    _squadData.assignUnitToSquad(unit, groundSquad);
                }
            }

            squad->setOrder(SquadOrder(SquadOrderTypes::Hold, homePos, AttackRadius, false, "Waiting for dropship"));
            _tankDropPhase[squadName] = 0;
            _tankDropUnloadStartFrame[squadName] = -1;
            _tankDropReturnIssued[squadName] = false;
            _tankDropLaunchIssued[squadName] = false;
            continue;
        }

        const int panicHpThreshold = 100;
        bool hasLoadedUnits = !transport->getLoadedUnits().empty();

        if (hasLoadedUnits && transport->getHitPoints() < panicHpThreshold)
        {
            _tankDropEmergencyMode[squadName] = true;

            if (transport->canUnloadAtPosition(transport->getPosition()))
            {
                the.micro.UnloadAt(transport, transport->getPosition());

                squad->setOrder(
                    SquadOrder(
                        SquadOrderTypes::Drop,
                        transport->getPosition(),
                        160,
                        false,
                        "Emergency unload"
                    )
                );

                DrawUnitTargetTextMap(
                    transport->getPosition() + BWAPI::Position(0, 16),
                    "PANIC UNLOAD NOW"
                );

                BWAPI::Broodwar->drawCircleMap(
                    transport->getPosition(),
                    24,
                    BWAPI::Colors::Red,
                    false
                );
            }
            else
            {
                BWAPI::Position panicPos = findEmergencyUnloadPosNear(transport, transport->getPosition());

                if (panicPos.isValid())
                {
                    if (transport->getDistance(panicPos) <= 2 * 32 &&
                        transport->canUnloadAtPosition(panicPos))
                    {
                        the.micro.UnloadAt(transport, panicPos);
                    }
                    else
                    {
                        the.micro.Move(transport, panicPos);
                    }

                    squad->setOrder(
                        SquadOrder(
                            SquadOrderTypes::Drop,
                            panicPos,
                            160,
                            false,
                            "Emergency unload"
                        )
                    );

                    DrawUnitTargetTextMap(
                        transport->getPosition() + BWAPI::Position(0, 16),
                        "PANIC UNLOAD"
                    );

                    BWAPI::Broodwar->drawCircleMap(
                        panicPos,
                        24,
                        BWAPI::Colors::Red,
                        false
                    );
                }
                else
                {
                    the.micro.UnloadAll(transport);

                    squad->setOrder(
                        SquadOrder(
                            SquadOrderTypes::Drop,
                            transport->getPosition(),
                            160,
                            false,
                            "Emergency unload"
                        )
                    );

                    DrawUnitTargetTextMap(
                        transport->getPosition() + BWAPI::Position(0, 16),
                        "PANIC UNLOAD ALL"
                    );

                    BWAPI::Broodwar->drawCircleMap(
                        transport->getPosition(),
                        24,
                        BWAPI::Colors::Red,
                        false
                    );
                }
            }

            continue;
        }

        if (phase == 0 &&
            ((_tankDropLaunchCount[squadName] >= 1) ||
                (_tankDropTotalLaunches >= 2)))
        {
            squad->setOrder(
                SquadOrder(
                    SquadOrderTypes::Hold,
                    homePos,
                    AttackRadius,
                    false,
                    "Tank drop limit reached"
                )
            );
            _tankDropUnloadStartFrame[squadName] = -1;
            _tankDropReturnIssued[squadName] = false;
            _tankDropLaunchIssued[squadName] = false;
            continue;
        }

        if (phase == 0 && vultureCount < 2)
        {
            squad->setOrder(SquadOrder(SquadOrderTypes::Hold, homePos, AttackRadius, false, "Waiting for 2 vultures"));
            _tankDropPhase[squadName] = 0;
            _tankDropUnloadStartFrame[squadName] = -1;
            _tankDropReturnIssued[squadName] = false;
            _tankDropLaunchIssued[squadName] = false;
            continue;
        }

        if (phase == 0)
        {
            bool needLoad = hasUnloadedVulture(*squad) || int(transport->getLoadedUnits().size()) < 2;

            if (needLoad)
            {
                squad->setOrder(SquadOrder(SquadOrderTypes::Load, transport->getPosition(), AttackRadius, false, "Load tank drop"));
                _tankDropPhase[squadName] = 0;
                _tankDropUnloadStartFrame[squadName] = -1;
                _tankDropReturnIssued[squadName] = false;
                _tankDropLaunchIssued[squadName] = false;
                continue;
            }

            if (transport->getLoadedUnits().size() < 2)
            {
                squad->setOrder(SquadOrder(SquadOrderTypes::Load, transport->getPosition(), AttackRadius, false, "Finishing load"));
                _tankDropPhase[squadName] = 0;
                _tankDropUnloadStartFrame[squadName] = -1;
                _tankDropReturnIssued[squadName] = false;
                _tankDropLaunchIssued[squadName] = false;
                continue;
            }
        }



        BWAPI::Position unloadPos = getTankDropUnloadPoint(i);

        if (phase == 0)
        {
            const bool squadAlreadyLaunched = (_tankDropLaunchCount[squadName] >= 1);
            const bool totalLimitReached = (_tankDropTotalLaunches >= 2);

            if (squadAlreadyLaunched || totalLimitReached)
            {
                squad->setOrder(
                    SquadOrder(
                        SquadOrderTypes::Hold,
                        homePos,
                        AttackRadius,
                        false,
                        "Tank drop limit reached"
                    )
                );

                _tankDropPhase[squadName] = 0;
                _tankDropUnloadStartFrame[squadName] = -1;
                _tankDropReturnIssued[squadName] = false;
                _tankDropLaunchIssued[squadName] = false;
                continue;
            }

            if (vultureCount < 2)
            {
                squad->setOrder(
                    SquadOrder(
                        SquadOrderTypes::Hold,
                        homePos,
                        AttackRadius,
                        false,
                        "Waiting for 2 vultures"
                    )
                );
                continue;
            }

            if (int(transport->getLoadedUnits().size()) < 2)
            {
                squad->setOrder(
                    SquadOrder(
                        SquadOrderTypes::Load,
                        transport->getPosition(),
                        AttackRadius,
                        false,
                        "Finishing load"
                    )
                );

                DrawUnitTargetTextMap(
                    transport->getPosition() + BWAPI::Position(0, 16),
                    "Finishing load"
                );

                continue;
            }

            if (!_tankDropLaunchIssued[squadName])
            {

                squad->setOrder(
                    SquadOrder(
                        SquadOrderTypes::Drop,
                        unloadPos,
                        AttackRadius,
                        false,
                        "tankDrop launch"
                    )
                );

                _tankDropLaunchIssued[squadName] = true;
                _tankDropLaunchCount[squadName]++;
                _tankDropTotalLaunches++;

                // MicroTransports chooses the shorter perimeter arc to the
                // actual unload area. Three fixed corners used to add almost
                // a whole map lap before either payload reached the enemy.
                phase = 4;
                continue;
            }
        }

        if (phase == 6)
        {
            squad->setOrder(
                SquadOrder(
                    SquadOrderTypes::Drop,
                    homePos,
                    AttackRadius,
                    false,
                    "Return after drop"
                )
            );
            continue;
        }

        if (phase == 4)
        {
            BWAPI::Position realUnloadPos = findNearbyUnloadPos(transport, unloadPos);

            if (transport->getDistance(unloadPos) <= 7 * 32 &&
                realUnloadPos.isValid())
            {
                phase = 5;
            }
            else
            {
                squad->setOrder(
                    SquadOrder(
                        SquadOrderTypes::Drop,
                        unloadPos,
                        160,
                        false,
                        "tankDrop unload approach"
                    )
                );

                DrawUnitTargetTextMap(
                    transport->getPosition() + BWAPI::Position(0, 16),
                    "tankDrop unload approach"
                );

                BWAPI::Broodwar->drawCircleMap(
                    unloadPos,
                    24,
                    BWAPI::Colors::Cyan,
                    false
                );

                if (transport->getDistance(unloadPos) <= 7 * 32)
                {
                    phase = 5;
                }
            }

            continue;
        }

        if (phase >= 5)
        {
            BWAPI::Position realUnloadPos = findNearbyUnloadPos(transport, unloadPos);

            if (realUnloadPos.isValid())
            {
                squad->setOrder(
                    SquadOrder(
                        SquadOrderTypes::Drop,
                        realUnloadPos,
                        160,
                        false,
                        "Tank vulture drop"
                    )
                );

                DrawUnitTargetTextMap(
                    transport->getPosition() + BWAPI::Position(0, 16),
                    "tankDrop unload now"
                );

                BWAPI::Broodwar->drawCircleMap(
                    realUnloadPos,
                    24,
                    BWAPI::Colors::Cyan,
                    false
                );
            }
            else
            {
                squad->setOrder(
                    SquadOrder(
                        SquadOrderTypes::Drop,
                        unloadPos,
                        160,
                        false,
                        "tankDrop unload approach"
                    )
                );

                DrawUnitTargetTextMap(
                    transport->getPosition() + BWAPI::Position(0, 16),
                    "tankDrop seek unload spot"
                );
            }

            DrawUnitTargetTextMap(
                transport->getPosition() + BWAPI::Position(0, 16),
                "tankDrop drop minerals"
            );

            BWAPI::Broodwar->drawCircleMap(
                unloadPos,
                24,
                BWAPI::Colors::Cyan,
                false
            );
        }   
    }
}


namespace
{
    BWAPI::Position findIslandCleanupTarget()
    {
        if (the.selfRace() != BWAPI::Races::Terran ||
            the.now() < 10 * 60 * 24 ||
            !the.bases.myStart())
        {
            return BWAPI::Positions::Invalid;
        }

        // A real or remembered enemy depot means the normal attack/base search
        // still owns the game. Island cleanup is only a last-resort finisher.
        for (Base* base : the.bases.getAll())
        {
            if (base && base->getOwner() == the.enemy())
            {
                return BWAPI::Positions::Invalid;
            }
        }

        const BWAPI::Position groundReference =
            the.bases.myStart()->getPosition();
        BWAPI::Position bestTarget = BWAPI::Positions::Invalid;
        int bestDistance = MAX_DISTANCE;

        for (const auto& entry : the.info.getUnitInfo(the.enemy()))
        {
            const UnitInfo& ui = entry.second;

            if (!ui.lastPosition.isValid() ||
                ui.goneFromLastPosition ||
                ui.type.isSpell())
            {
                continue;
            }

            if (ui.type.isResourceDepot())
            {
                return BWAPI::Positions::Invalid;
            }

            if (ui.type.isBuilding())
            {
                // A lifted building is an anti-air cleanup problem, not a
                // ground-island drop target.
                if (ui.lifted)
                {
                    return BWAPI::Positions::Invalid;
                }

                const int groundDistance =
                    the.map.getGroundDistance(
                        groundReference,
                        ui.lastPosition);

                if (groundDistance >= 0)
                {
                    // Finish every reachable building normally first.
                    return BWAPI::Positions::Invalid;
                }

                const int airDistance =
                    groundReference.getApproxDistance(ui.lastPosition);

                if (airDistance < bestDistance)
                {
                    bestDistance = airDistance;
                    bestTarget = ui.lastPosition;
                }

                continue;
            }

            // Do not pull a Dropship away while a recently seen enemy unit can
            // still be cleaned up by the normal army. Units remembered on the
            // same unreachable island do not block the drop; the passengers
            // will fight them after landing.
            if (ui.updateFrame >= the.now() - 10 * 24)
            {
                if (ui.type.isFlyer() ||
                    the.map.getGroundDistance(
                        groundReference,
                        ui.lastPosition) >= 0)
                {
                    return BWAPI::Positions::Invalid;
                }
            }
        }

        return bestTarget;
    }

    int islandCleanupPassengerPriority(BWAPI::Unit unit)
    {
        if (!unit || !unit->exists())
        {
            return -1;
        }

        const BWAPI::UnitType type = unit->getType();

        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            return 60;
        }
        if (type == BWAPI::UnitTypes::Terran_Goliath)
        {
            return 50;
        }
        if (type == BWAPI::UnitTypes::Terran_Vulture)
        {
            return 40;
        }
        if (type == BWAPI::UnitTypes::Terran_Marine ||
            type == BWAPI::UnitTypes::Terran_Ghost ||
            type == BWAPI::UnitTypes::Terran_Firebat)
        {
            return 30;
        }
        if (type.isWorker())
        {
            return 10;
        }

        return UnitUtil::CanAttackGround(unit) ? 20 : -1;
    }
}

bool CombatCommander::islandCleanupDropNeeded() const
{
    return findIslandCleanupTarget().isValid();
}

BWAPI::Position CombatCommander::getIslandCleanupDropTarget() const
{
    return findIslandCleanupTarget();
}

void CombatCommander::updateDropSquads()
{
    const BWAPI::Position islandCleanupTarget =
        getIslandCleanupDropTarget();

    if (islandCleanupTarget.isValid())
    {
        BWAPI::Position home = the.bases.myMain()
            ? the.bases.myMain()->getPosition()
            : BWAPI::Position(the.self()->getStartLocation());

        if (!_squadData.squadExists("Drop"))
        {
            _squadData.createSquad("Drop", DropPriority).
                setOrder(SquadOrder(
                    SquadOrderTypes::Hold,
                    home,
                    AttackRadius,
                    false,
                    "Island cleanup: wait for transport"));
        }

        Squad& cleanupSquad = _squadData.getSquad("Drop");
        BWAPI::Unit transport = nullptr;

        for (BWAPI::Unit unit : cleanupSquad.getUnits())
        {
            if (unit && unit->exists() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Dropship)
            {
                transport = unit;
                break;
            }
        }

        // Old tank-drop squads can retain an empty returned Dropship. Release
        // that transport for the mandatory cleanup mission.
        if (!transport)
        {
            for (const std::string& squadName :
                { std::string("Drop1"), std::string("Drop2") })
            {
                if (!_squadData.squadExists(squadName))
                {
                    continue;
                }

                Squad& oldDrop = _squadData.getSquad(squadName);
                std::vector<BWAPI::Unit> releasable;

                for (BWAPI::Unit unit : oldDrop.getUnits())
                {
                    if (unit && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Terran_Dropship &&
                        !isExpansionFerryDropship(unit) &&
                        unit->getLoadedUnits().empty())
                    {
                        releasable.push_back(unit);
                    }
                }

                for (BWAPI::Unit unit : releasable)
                {
                    oldDrop.removeUnit(unit);
                }
            }
        }

        if (!transport)
        {
            for (BWAPI::Unit unit : _combatUnits)
            {
                if (unit && unit->exists() && unit->isCompleted() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Dropship &&
                    !isExpansionFerryDropship(unit) &&
                    unit->getLoadedUnits().empty() &&
                    _squadData.canAssignUnitToSquad(unit, cleanupSquad))
                {
                    _squadData.assignUnitToSquad(unit, cleanupSquad);
                    transport = unit;
                    break;
                }
            }
        }

        if (!transport)
        {
            cleanupSquad.setOrder(SquadOrder(
                SquadOrderTypes::Hold,
                home,
                AttackRadius,
                false,
                "Island cleanup: producing Dropship"));
            return;
        }

        int reservedSpace = 0;
        bool hasPassenger = false;
        bool hasUnloadedPassenger = false;

        for (BWAPI::Unit unit : cleanupSquad.getUnits())
        {
            if (!unit || !unit->exists() || unit == transport)
            {
                continue;
            }

            hasPassenger = true;
            reservedSpace += unit->getType().spaceRequired();
            hasUnloadedPassenger =
                hasUnloadedPassenger || !unit->isLoaded();
        }

        int freeSpace = std::max(0, 8 - reservedSpace);

        if (freeSpace > 0)
        {
            std::vector<BWAPI::Unit> candidates;

            for (BWAPI::Unit unit : _combatUnits)
            {
                if (!unit || !unit->exists() || !unit->isCompleted() ||
                    unit->isLoaded() || unit->getType().isBuilding() ||
                    unit->isFlying() || unit == _islandCleanupSacrificeVictim ||
                    islandCleanupPassengerPriority(unit) < 0 ||
                    unit->getType().spaceRequired() > freeSpace ||
                    !_squadData.canAssignUnitToSquad(unit, cleanupSquad))
                {
                    continue;
                }

                candidates.push_back(unit);
            }

            std::sort(
                candidates.begin(),
                candidates.end(),
                [transport](BWAPI::Unit left, BWAPI::Unit right)
                {
                    const int leftPriority =
                        islandCleanupPassengerPriority(left);
                    const int rightPriority =
                        islandCleanupPassengerPriority(right);

                    if (leftPriority != rightPriority)
                    {
                        return leftPriority > rightPriority;
                    }

                    return left->getDistance(transport) <
                        right->getDistance(transport);
                });

            for (BWAPI::Unit unit : candidates)
            {
                const int requiredSpace = unit->getType().spaceRequired();
                if (requiredSpace > freeSpace)
                {
                    continue;
                }

                _squadData.assignUnitToSquad(unit, cleanupSquad);
                freeSpace -= requiredSpace;
                hasPassenger = true;
                hasUnloadedPassenger = true;

                if (freeSpace <= 0)
                {
                    break;
                }
            }
        }

        if (!hasPassenger)
        {
            cleanupSquad.setOrder(SquadOrder(
                SquadOrderTypes::Hold,
                transport->getPosition(),
                AttackRadius,
                false,
                "Island cleanup: wait for passenger"));
            return;
        }

        const bool landedAtTarget =
            transport->getLoadedUnits().empty() &&
            transport->getDistance(islandCleanupTarget) <= 11 * 32;

        if (hasUnloadedPassenger && !landedAtTarget)
        {
            cleanupSquad.setOrder(SquadOrder(
                SquadOrderTypes::Load,
                transport->getPosition(),
                AttackRadius,
                false,
                "Island cleanup: load"));
        }
        else
        {
            cleanupSquad.setOrder(SquadOrder(
                SquadOrderTypes::Drop,
                islandCleanupTarget,
                11 * 32,
                false,
                "Island cleanup"));
        }

        return;
    }

    if (tankDropEnabled())
    {
        updateTankDropSquads();
        return;
    }


    if (!_squadData.squadExists("Drop"))
    {
        return;
    }

    Squad& dropSquad = _squadData.getSquad("Drop");

    if (dropSquad.getOrder().getType() == SquadOrderTypes::Drop)
    {
        return;
    }

    BWAPI::Unit transportUnit = nullptr;
    int transportSpotsRemaining = 8;
    bool anyUnloadedUnits = false;
    const auto& dropUnits = dropSquad.getUnits();

    for (BWAPI::Unit unit : dropUnits)
    {
        if (unit->exists())
        {
            if (unit->isFlying() && unit->getType().spaceProvided() > 0)
            {
                transportUnit = unit;
            }
            else
            {
                transportSpotsRemaining -= unit->getType().spaceRequired();
                if (!unit->isLoaded())
                {
                    anyUnloadedUnits = true;
                }
            }
        }
    }

    // Acquire the transport first. The old one-pass loop could see a Vulture
    // before a Dropship in _combatUnits and reserve the Vulture into Drop with
    // no transport at all.
    if (!transportUnit)
    {
        for (BWAPI::Unit unit : _combatUnits)
        {
            if (unit && unit->exists() && unit->isCompleted() &&
                unit->getType().spaceProvided() > 0 && unit->isFlying() &&
                !isExpansionFerryDropship(unit) &&
                _squadData.canAssignUnitToSquad(unit, dropSquad))
            {
                _squadData.assignUnitToSquad(unit, dropSquad);
                transportUnit = unit;
                break;
            }
        }
    }

    if (!transportUnit)
    {
        // Do not let a planned/generic Drop squad park Terran Vultures while
        // waiting for a transport. They remain useful in the Ground squad.
        if (_squadData.squadExists("Ground"))
        {
            Squad& groundSquad = _squadData.getSquad("Ground");
            std::vector<BWAPI::Unit> release;

            for (BWAPI::Unit unit : dropSquad.getUnits())
            {
                if (unit && unit->exists() && !unit->isLoaded() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
                {
                    release.push_back(unit);
                }
            }

            for (BWAPI::Unit unit : release)
            {
                dropSquad.removeUnit(unit);
                if (_squadData.canAssignUnitToSquad(unit, groundSquad))
                {
                    _squadData.assignUnitToSquad(unit, groundSquad);
                }
            }
        }

        dropSquad.setOrder(SquadOrder(
            SquadOrderTypes::Hold,
            the.bases.myMain() ? the.bases.myMain()->getPosition() : BWAPI::Position(the.self()->getStartLocation()),
            AttackRadius,
            false,
            "Wait for transport"));
        return;
    }

    // Once a real transport exists, fill the remaining cargo space.
    if (transportSpotsRemaining > 0)
    {
        for (BWAPI::Unit unit : _combatUnits)
        {
            if (!unit || !unit->exists() || !unit->isCompleted() || unit->isLoaded())
            {
                continue;
            }

            if (unit->getType().spaceRequired() <= transportSpotsRemaining &&
                unitIsGoodToDrop(unit) &&
                _squadData.canAssignUnitToSquad(unit, dropSquad))
            {
                _squadData.assignUnitToSquad(unit, dropSquad);
                transportSpotsRemaining -= unit->getType().spaceRequired();
            }

            if (transportSpotsRemaining <= 0)
            {
                break;
            }
        }
    }

    // Re-evaluate the load state after assigning passengers.
    anyUnloadedUnits = false;
    transportSpotsRemaining = 8;
    for (BWAPI::Unit unit : dropSquad.getUnits())
    {
        if (!unit || !unit->exists() || unit == transportUnit)
        {
            continue;
        }

        transportSpotsRemaining -= unit->getType().spaceRequired();
        anyUnloadedUnits = anyUnloadedUnits || !unit->isLoaded();
    }

    const int cargoUsed = std::max(0, 8 - transportSpotsRemaining);
    const int usefulPartialPayload = 4;
    const int partialFillTimeout = 5 * 24;

    // Start/restart the fill timer only when the payload actually changes.
    if (cargoUsed <= 0)
    {
        _genericDropFillStartFrame = -1;
        _genericDropLastCargoUsed = 0;
    }
    else if (_genericDropFillStartFrame < 0 || cargoUsed != _genericDropLastCargoUsed)
    {
        _genericDropFillStartFrame = the.now();
        _genericDropLastCargoUsed = cargoUsed;
    }

    const bool fullPayload = transportSpotsRemaining <= 0;
    const bool usefulPartial = cargoUsed >= usefulPartialPayload;
    const bool fillTimedOut = cargoUsed > 0 && _genericDropFillStartFrame >= 0 &&
        the.now() - _genericDropFillStartFrame >= partialFillTimeout;

    if (anyUnloadedUnits)
    {
        dropSquad.setOrder(SquadOrder(
            SquadOrderTypes::Load, transportUnit->getPosition(), AttackRadius, false,
            fullPayload ? "Load up" : "Fill drop"));
    }
    else if (fullPayload || usefulPartial || fillTimedOut)
    {
        // Launch with a useful partial load instead of parking forever while
        // waiting for an exact 8/8 composition.  Tiny leftover payloads get a
        // short grace period before they are allowed to leave as well.
        dropSquad.setOrder(SquadOrder(
            SquadOrderTypes::Drop, getDropLocation(dropSquad), 300, false,
            fullPayload ? "Go drop!" : "Go partial drop!"));
        _genericDropFillStartFrame = -1;
        _genericDropLastCargoUsed = 0;
    }
    else
    {
        dropSquad.setOrder(SquadOrder(
            SquadOrderTypes::Load, transportUnit->getPosition(), AttackRadius, false, "Fill drop"));
    }
}

void CombatCommander::updateIslandCleanupSupplyRelease()
{
    if (!islandCleanupDropNeeded())
    {
        _islandCleanupSacrificeVictim = nullptr;
        return;
    }

    const BWAPI::UnitType dropshipType =
        BWAPI::UnitTypes::Terran_Dropship;

    if (the.my.all.count(dropshipType) > 0)
    {
        _islandCleanupSacrificeVictim = nullptr;
        return;
    }

    bool dropshipTraining = false;

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Starport)
        {
            continue;
        }

        if (unit->isTraining() &&
            unit->getLastCommand().getUnitType() == dropshipType)
        {
            dropshipTraining = true;
            break;
        }
    }

    if (dropshipTraining)
    {
        _islandCleanupSacrificeVictim = nullptr;
        return;
    }

    // Do not release supply before the production chain is actually ready to
    // start the Dropship. Otherwise normal macro could refill the space while
    // Starport/Control Tower are still under construction.
    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Starport) == 0 ||
        the.my.completed.count(BWAPI::UnitTypes::Terran_Control_Tower) == 0 ||
        the.self()->minerals() < dropshipType.mineralPrice() ||
        the.self()->gas() < dropshipType.gasPrice())
    {
        return;
    }

    const int freeSupply =
        the.self()->supplyTotal() - the.self()->supplyUsed();

    if (freeSupply >= dropshipType.supplyRequired())
    {
        _islandCleanupSacrificeVictim = nullptr;
        return;
    }

    const int now = the.now();
    if (now < _lastIslandCleanupSupplyReleaseFrame + 6)
    {
        return;
    }
    _lastIslandCleanupSupplyReleaseFrame = now;

    // First prefer cancelling one not-yet-finished combat unit. This frees the
    // same supply without throwing away an already completed veteran.
    for (BWAPI::Unit producer : the.self()->getUnits())
    {
        if (!producer || !producer->exists() || !producer->isTraining())
        {
            continue;
        }

        // Avoid Unit::getTrainingQueue(); this exact BWAPI call has already
        // produced a reproducible access violation in production code.
        const BWAPI::UnitType trainedType =
            producer->getLastCommand().getUnitType();

        if (trainedType == BWAPI::UnitTypes::None ||
            trainedType == BWAPI::UnitTypes::Unknown)
        {
            continue;
        }
        if (trainedType == dropshipType ||
            trainedType.isWorker() ||
            trainedType.supplyRequired() <= 0)
        {
            continue;
        }

        if (producer->cancelTrain())
        {
            return;
        }
    }

    if (!_islandCleanupSacrificeVictim ||
        !_islandCleanupSacrificeVictim->exists())
    {
        BWAPI::Unit bestVictim = nullptr;
        int bestPriority = MAX_DISTANCE;
        int bestHP = MAX_DISTANCE;

        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (!unit || !unit->exists() || !unit->isCompleted() ||
                unit->isLoaded())
            {
                continue;
            }

            const BWAPI::UnitType type = unit->getType();
            int priority = MAX_DISTANCE;

            if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
            {
                priority = 0;
            }
            else if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            {
                priority = 1;
            }
            else if (type == BWAPI::UnitTypes::Terran_Goliath)
            {
                priority = 2;
            }
            else if (type == BWAPI::UnitTypes::Terran_Vulture ||
                type == BWAPI::UnitTypes::Terran_Marine)
            {
                priority = 3;
            }
            else
            {
                continue;
            }

            const int hp = unit->getHitPoints() + unit->getShields();
            if (priority < bestPriority ||
                (priority == bestPriority && hp < bestHP))
            {
                bestPriority = priority;
                bestHP = hp;
                bestVictim = unit;
            }
        }

        _islandCleanupSacrificeVictim = bestVictim;
    }

    BWAPI::Unit victim = _islandCleanupSacrificeVictim;
    if (!victim || !victim->exists())
    {
        return;
    }

    victim->stop();

    std::vector<BWAPI::Unit> attackers;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isCompleted() ||
            unit == victim || unit->isLoaded() ||
            unit->getType().isBuilding() ||
            unit->getType().isWorker() ||
            !UnitUtil::CanAttackGround(unit))
        {
            continue;
        }

        attackers.push_back(unit);
    }

    std::sort(
        attackers.begin(),
        attackers.end(),
        [victim](BWAPI::Unit left, BWAPI::Unit right)
        {
            const bool leftSplash =
                left->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
            const bool rightSplash =
                right->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;

            if (leftSplash != rightSplash)
            {
                return !leftSplash;
            }

            return left->getDistance(victim) < right->getDistance(victim);
        });

    int issued = 0;
    for (BWAPI::Unit attacker : attackers)
    {
        attacker->attack(victim);

        if (++issued >= 3)
        {
            break;
        }
    }
}

void CombatCommander::updateScoutDefenseSquad() 
{
    if (Config::Micro::ScoutDefenseRadius == 0)
    { 
        return; 
    }

    // Get the region of our starting base.
    const int myZone = the.zone.areaIDAt(the.bases.myStart()->getTilePosition());

    // Get all of the visible enemy units in this region.
	// Out of sight, out of mind.
    BWAPI::Unitset enemyUnitsInRegion;
    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (!UnitUtil::IsValidUnit(unit))
        {
            continue;
        }

		if (myZone == the.zone.areaIDAt(unit))
        {
            enemyUnitsInRegion.insert(unit);
			if (enemyUnitsInRegion.size() > 1)
			{
				break;
			}
        }
    }

    Squad & scoutDefenseSquad = _squadData.getSquad("ScoutDefense");

    // Never retain economy workers in ScoutDefense, including workers assigned
    // by an older frame/version before this policy was applied.
    scoutDefenseSquad.releaseWorkers();

    const BWAPI::Unitset previousScoutDefenders = scoutDefenseSquad.getUnits();
    for (BWAPI::Unit unit : previousScoutDefenders)
    {
        if (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            scoutDefenseSquad.removeUnit(unit);
        }
    }

    // Is exactly one enemy worker here?
    bool assignScoutDefender = enemyUnitsInRegion.size() == 1 && (*enemyUnitsInRegion.begin())->getType().isWorker();

    if (assignScoutDefender)
    {
        if (scoutDefenseSquad.isEmpty())
        {
            // Never spend an economy worker just to chase a scouting SCV/Probe.
            // If we already own a real combat unit, borrow the closest one;
            // otherwise ignore the scout and keep mining.
            BWAPI::Unit enemyWorker = *enemyUnitsInRegion.begin();
            BWAPI::Unit bestDefender = nullptr;
            int bestDistance = MAX_DISTANCE;

            for (BWAPI::Unit unit : _combatUnits)
            {
                if (!unit ||
                    !unit->exists() ||
                    !unit->isCompleted() ||
                    unit->getType().isWorker() ||
                    unit->getType().isBuilding() ||
                    unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                    unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
                    !UnitUtil::CanAttackGround(unit) ||
                    !_squadData.canAssignUnitToSquad(unit, scoutDefenseSquad))
                {
                    continue;
                }

                const int distance = unit->getDistance(enemyWorker);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestDefender = unit;
                }
            }

            if (bestDefender)
            {
                _squadData.assignUnitToSquad(bestDefender, scoutDefenseSquad);
            }
        }
    }
    // Otherwise the squad should be empty. If not, make it so.
    else if (!scoutDefenseSquad.isEmpty())
    {
        scoutDefenseSquad.clear();
    }
}

void CombatCommander::updateBaseDefenseSquads()
{
    const int baseDefenseRadius = 19 * 32;
    const int baseDefenseHysteresis = 10 * 32;

    // NOTE This feature ameliorates some problems but causes others.
    //      That's why the duration is only 1 second.
    const int extraFrames = 1 * 24;     // stay this long after the last enemy is gone

    const bool pureAirMode =
        StrategyManager::Instance().pureAirPlanActive();
    const int totalGoliaths = pureAirMode
        ? the.my.completed.count(BWAPI::UnitTypes::Terran_Goliath)
        : 0;
    const int mobilePureAirReserve = totalGoliaths >= 8
        ? std::max(4, totalGoliaths / 2)
        : 0;
    int remainingPureAirCarrierGarrison =
        std::max(0, totalGoliaths - mobilePureAirReserve);

    // Bio may see its first Carrier before its first Goliath finishes. Four
    // Marines can cover temporarily without freezing the complete Bio army.
    if (pureAirMode &&
        remainingPureAirCarrierGarrison == 0 &&
        StrategyManager::Instance().getOpeningGroup() == "bio")
    {
        remainingPureAirCarrierGarrison = 4;
    }

    if (_combatUnits.empty()) 
    { 
        return; 
    }

    for (Base * base : the.bases.getAll())
    {
        std::stringstream squadName;
        squadName << "Base " << base->getTilePosition().x << "," << base->getTilePosition().y;

        // Don't defend inside the enemy region.
        // It will end badly when we are stealing gas or otherwise proxying.
        if (base->getOwner() != the.self())
        {
            // Clear any defense squad.
            if (_squadData.squadExists(squadName.str()))
            {
                _squadData.removeSquad(squadName.str());
            }
            continue;
        }

        // Start to defend when enemy comes within baseDefenseRadius.
        // Stop defending when enemy leaves baseDefenseRadius + baseDefenseHysteresis.
        const int defenseRadius = _squadData.squadExists(squadName.str())
            ? baseDefenseRadius + baseDefenseHysteresis
            : baseDefenseRadius;

        const Zone * zone = the.zone.areaAt(base->getTilePosition());
        UAB_ASSERT(zone, "bad base location");

        // Assume for now that the base is not in danger.
        // We may prove otherwise below.
        base->setAirDanger(false);
        base->setWorkerDanger(false);
        base->setDoomed(false);

        // Find any enemy units that are bothering us.
        // Also note how far away the closest one is.
        int closestEnemyDistance = MAX_DISTANCE;
        BWAPI::Unit closestEnemy = nullptr;
        int nEnemySupply = 0;
        int nEnemyWorkers = 0;
        int nEnemyGround = 0;
        int nEnemyAir = 0;
        int nEnemyZealots = 0;
        int nEnemyZerglings = 0;
        int nEnemyCarriers = 0;
        const bool bioTankDefense =
            StrategyManager::Instance().getTerranPlan() == TerranPlanId::BioTank;
        bool enemyTransportAtBase = false;
        bool sawEnemyInterceptor = false;
        bool enemyHitsGround = false;
        bool enemyHitsAir = false;
        bool enemyHasCloak = false;

        for (BWAPI::Unit unit : the.enemy()->getUnits())
        {
            if (unit->isInvincible() || unit->getType().isSpell())
            {
                continue;
            }
            const int dist = unit->getDistance(base->getCenter());
            if (dist < defenseRadius ||
                dist < defenseRadius + 384 && zone == the.zone.areaAt(unit->getTilePosition()))
            {
                if (bioTankDefense && !unit->isFlying() &&
                    !BioTankPolicy::canThreatenBase(unit, base, defenseRadius + 192)) continue;
                if (dist < closestEnemyDistance)
                {
                    closestEnemyDistance = dist;
                    closestEnemy = unit;
                }

                if (unit->getType() == BWAPI::UnitTypes::Protoss_Zealot)
                {
                    ++nEnemyZealots;
                }
                else if (unit->getType() == BWAPI::UnitTypes::Zerg_Zergling)
                {
                    ++nEnemyZerglings;
                }

                // Count some non-attack buildings (not overlords) for later. We'll treat them specially,
                // to leave more units free to attack the enemy main.
                if (!unit->isFlying() &&
                    (unit->getType() == BWAPI::UnitTypes::Terran_Supply_Depot ||
                    unit->getType() == BWAPI::UnitTypes::Terran_Engineering_Bay ||
                    unit->getType() == BWAPI::UnitTypes::Protoss_Pylon))
                {
                    ++nEnemySupply;
                }
                else if (unit->getType().isWorker())
                {
                    ++nEnemyWorkers;
                }
                else if (unit->isFlying())
                {
                    if (unit->getType() == BWAPI::UnitTypes::Protoss_Interceptor)
                    {
                        // Interceptors belong to their Carrier. Counting all
                        // of them as independent aircraft recalls nearly the
                        // entire anti-air army for one harassment group.
                        sawEnemyInterceptor = true;
                    }
                    else if (unit->getType() == BWAPI::UnitTypes::Protoss_Carrier)
                    {
                        // A Carrier needs a meaningful local response, but
                        // not (Carrier + every interceptor) * 2 defenders.
                        nEnemyAir += 3;
                        ++nEnemyCarriers;
                    }
                    else if (unit->getType() == BWAPI::UnitTypes::Terran_Battlecruiser ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Arbiter)
                    {
                        nEnemyAir += 4;
                    }
                    else if (unit->getType() == BWAPI::UnitTypes::Protoss_Scout)
                    {
                        nEnemyAir += 3;
                    }
                    else if (unit->getType() == BWAPI::UnitTypes::Protoss_Shuttle ||
                        unit->getType() == BWAPI::UnitTypes::Terran_Dropship)
                    {
                        // A transport can briefly unload a high-damage unit
                        // and pick it up again. Defend while the carrier is
                        // visible rather than waiting for the first shot.
                        nEnemyAir += 3;
                        enemyTransportAtBase = true;
                    }
                    else if (unit->getType() == BWAPI::UnitTypes::Zerg_Guardian ||
                        unit->getType() == BWAPI::UnitTypes::Zerg_Devourer)
                    {
                        nEnemyAir += 2;
                    }
                    else
                    {
                        ++nEnemyAir;
                    }
                }
                else
                {
                    // Workers don't count as ground units here.
                    if (unit->getType() == BWAPI::UnitTypes::Terran_Goliath ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Zealot ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Dragoon ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar ||
                        unit->getType() == BWAPI::UnitTypes::Zerg_Lurker ||
                        unit->getType() == BWAPI::UnitTypes::Zerg_Creep_Colony)
                    {
                        nEnemyGround += 2;
                    }
                    else if (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                        unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Archon ||
                        unit->getType() == BWAPI::UnitTypes::Protoss_Reaver ||
                        unit->getType() == BWAPI::UnitTypes::Zerg_Ultralisk)
                    {
                        nEnemyGround += 4;
                    }
                    else
                    {
                        ++nEnemyGround;
                    }
                }
                if (UnitUtil::CanAttackGround(unit))
                {
                    enemyHitsGround = true;
                }
                if (UnitUtil::CanAttackAir(unit))
                {
                    enemyHitsAir = true;
                }
                if (unit->isBurrowed() ||
                    unit->isCloaked() ||
                    unit->getType().hasPermanentCloak() ||
                    unit->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
                    unit->getType() == BWAPI::UnitTypes::Protoss_Arbiter ||
                    unit->getType() == BWAPI::UnitTypes::Zerg_Lurker ||
                    unit->getType() == BWAPI::UnitTypes::Zerg_Lurker_Egg)
                {
                    enemyHasCloak = true;
                }
            }
        }

        // The Carrier may briefly disappear into fog while its interceptors
        // are still attacking. Keep a small response without treating every
        // interceptor as a complete enemy aircraft.
        if (sawEnemyInterceptor && nEnemyCarriers == 0)
        {
            nEnemyAir += 2;
        }

        // If the enemy has no dangerouus ground units, assign defenders to clear enemy "supply" units instead.
        // See above for what that includes.
        if (nEnemyGround == 0)
        {
            nEnemyGround = nEnemySupply;
        }

        if (!closestEnemy &&
            _squadData.squadExists(squadName.str()) &&
            the.now() > _squadData.getSquad(squadName.str()).getTimeMark() + extraFrames)
        {
            // No enemies and the extra time margin has passed. Drop the defense squad.
            _squadData.removeSquad(squadName.str());
            continue;
        }

        // We need a defense squad. If there isn't one, create it.
        // Its goal is not the base location itself, but the enemy closest to it, to ensure
        // that defenders will get close enough to the enemy to act.
        if (closestEnemy && !_squadData.squadExists(squadName.str()))
        {
            _squadData.createSquad(squadName.str(), BaseDefensePriority);
        }
        if (!_squadData.squadExists(squadName.str()))
        {
            continue;
        }
        if (!closestEnemy)
        {
            // Retain the existing members of the squad for extraFrames in case the enemy returns.
            continue;
        }
        Squad & defenseSquad = _squadData.getSquad(squadName.str());
        BWAPI::Position targetPosition = closestEnemy ? closestEnemy->getPosition() : base->getPosition();
        defenseSquad.setOrder(SquadOrder(SquadOrderTypes::Defend, targetPosition, defenseRadius, false, "Defend base"));
        // There is somebody to fight, so remember that the squad is active as of this frame.
        defenseSquad.setTimeMark(the.now());

        // Next, figure out how many units we need to assign.

        // A simpleminded way of figuring out how much defense we need.
        const int numDefendersPerEnemyUnit = 2;

        int flyingDefendersNeeded = numDefendersPerEnemyUnit * nEnemyAir;
        int groundDefendersNeeded = nEnemyWorkers + numDefendersPerEnemyUnit * nEnemyGround;

        // Count static defense as defenders.
        // Ignore bunkers; they're more complicated.
        // Cannons are double-counted as air and ground, which can be a mistake.
        bool sunkenDefender = false;
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if ((unit->getType() == BWAPI::UnitTypes::Terran_Missile_Turret ||
                unit->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
                unit->getType() == BWAPI::UnitTypes::Zerg_Spore_Colony) &&
                unit->isCompleted() && unit->isPowered() &&
                zone == the.zone.areaAt(unit->getTilePosition()))
            {
                flyingDefendersNeeded -= 3;
            }
            if ((unit->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
                unit->getType() == BWAPI::UnitTypes::Zerg_Sunken_Colony) &&
                unit->isCompleted() && unit->isPowered() &&
                zone == the.zone.areaAt(unit->getTilePosition()))
            {
                sunkenDefender = true;
                groundDefendersNeeded -= 4;
            }
        }

        // Don't let the number of defenders go negative.
        flyingDefendersNeeded = nEnemyAir ? std::max(flyingDefendersNeeded, 2) : 0;
        if (enemyTransportAtBase)
        {
            flyingDefendersNeeded = std::max(flyingDefendersNeeded, 4);
        }

        const bool boundedPureAirCarrierDefense =
            pureAirMode &&
            (nEnemyCarriers > 0 || sawEnemyInterceptor);

        if (boundedPureAirCarrierDefense)
        {
            flyingDefendersNeeded =
                std::min(flyingDefendersNeeded, remainingPureAirCarrierGarrison);
            remainingPureAirCarrierGarrison =
                std::max(0, remainingPureAirCarrierGarrison - flyingDefendersNeeded);
        }

        if (nEnemyGround > 0)
        {
            groundDefendersNeeded = std::max(groundDefendersNeeded, 2 + nEnemyWorkers / 2);
        }
        else if (nEnemyWorkers > 0)
        {
            // Workers only, no other attackers.
            groundDefendersNeeded = std::max(groundDefendersNeeded, 1 + nEnemyWorkers / 2);
        }
        else
        {
            groundDefendersNeeded = 0;
        }

        // Carrier harassment can otherwise accumulate the complete AntiAir
        // squad in base defense over several passes. Release excess air
        // defenders immediately when the bounded quota becomes smaller.
        if (boundedPureAirCarrierDefense && groundDefendersNeeded <= 0)
        {
            std::vector<BWAPI::Unit> airDefenders;
            for (BWAPI::Unit defender : defenseSquad.getUnits())
            {
                if (defender && defender->exists() &&
                    !defender->getType().isDetector() &&
                    UnitUtil::CanAttackAir(defender))
                {
                    airDefenders.push_back(defender);
                }
            }

            std::sort(
                airDefenders.begin(),
                airDefenders.end(),
                [targetPosition](BWAPI::Unit left, BWAPI::Unit right)
                {
                    const bool leftGoliath =
                        left->getType() == BWAPI::UnitTypes::Terran_Goliath;
                    const bool rightGoliath =
                        right->getType() == BWAPI::UnitTypes::Terran_Goliath;

                    // Release non-Goliaths first so Bio keeps moving, then
                    // release the Goliaths farthest from the defended base.
                    if (leftGoliath != rightGoliath)
                    {
                        return !leftGoliath;
                    }

                    return left->getDistance(targetPosition) >
                        right->getDistance(targetPosition);
                });

            while (int(airDefenders.size()) > flyingDefendersNeeded)
            {
                defenseSquad.removeUnit(airDefenders.front());
                airDefenders.erase(airDefenders.begin());
            }
        }

        // Drop unneeded defenders.
        if (groundDefendersNeeded <= 0 && flyingDefendersNeeded <= 0)
        {
            defenseSquad.clear();
            continue;
        }
        if (groundDefendersNeeded <= 0)
        {
            // Drop any defenders which can't shoot air.
            BWAPI::Unitset drop;
            for (BWAPI::Unit unit : defenseSquad.getUnits())
            {
                if (!unit->getType().isDetector() &&
                    unit->getType() != BWAPI::UnitTypes::Terran_Medic && !UnitUtil::CanAttackAir(unit))
                {
                    drop.insert(unit);
                }
            }
            for (BWAPI::Unit unit : drop)
            {
                defenseSquad.removeUnit(unit);
            }
            // And carry on. We may still want to add air defenders.
        }
        if (flyingDefendersNeeded <= 0)
        {
            // Drop any defenders which can't shoot ground.
            BWAPI::Unitset drop;
            for (BWAPI::Unit unit : defenseSquad.getUnits())
            {
                if (!unit->getType().isDetector() &&
                    unit->getType() != BWAPI::UnitTypes::Terran_Medic && !UnitUtil::CanAttackGround(unit))
                {
                    drop.insert(unit);
                }
            }
            for (BWAPI::Unit unit : drop)
            {
                defenseSquad.removeUnit(unit);
            }
            // And carry on. We may still want to add ground defenders.
        }

        // SCV pulling is owned exclusively by
        // updateWorkerCounterAttackSquads(), which requires a visible worker
        // to be actively attacking one of our workers/buildings. Base defense
        // still assigns Marines/Vultures, but must not keep SCVs fighting merely
        // because the learned plan remains WorkerRush after the attack stops.
        const bool pullWorkers = false;

        const bool wePulledWorkers =
            std::any_of(
                defenseSquad.getUnits().begin(),
                defenseSquad.getUnits().end(),
                BWAPI::Filter::IsWorker);

        if (wePulledWorkers && !pullWorkers)
        {
            defenseSquad.releaseWorkers();
        }

        // Now find the actual units to assign.
        updateDefenseSquadUnits(defenseSquad, flyingDefendersNeeded, groundDefendersNeeded, pullWorkers, enemyHitsAir);

        // Assign a detector if appropriate.
        const bool wantDetector =
            !enemyHitsAir ||
            enemyHasCloak && int(defenseSquad.getUnits().size()) >= flyingDefendersNeeded + groundDefendersNeeded;
        maybeAssignDetector(defenseSquad, wantDetector);

        // Estimate roughly whether overlords spawned here may be in danger.
        if (the.airHitsFixed.inRange(base->getDepot())
                ||
            enemyHitsAir &&
            groundDefendersNeeded + flyingDefendersNeeded > 1 &&
            closestEnemyDistance <= 7 * 32 &&
            int(defenseSquad.getUnits().size()) / 2 < groundDefendersNeeded + flyingDefendersNeeded)
        {
            base->setAirDanger(true);
        }

        // Estimate roughly whether the workers may be in danger.
        // If they are not at immediate risk, they should keep mining and we should even be willing to transfer in more.
        if (the.groundHitsFixed.inRange(base->getDepot())
                ||
            enemyHitsGround &&
            groundDefendersNeeded > 1 &&
            closestEnemyDistance <= (the.info.enemyHasSiegeMode() ? 10 * 32 : 6 * 32) &&
            int(defenseSquad.getUnits().size()) / 2 < groundDefendersNeeded + flyingDefendersNeeded)
        {
            base->setWorkerDanger(true);
        }

        // Decide whether the base cannot be saved and shold be abandoned.
        // NOTE Doomed does not imply worker danger.
        // NOTE This does not disband the defense squad. It is meant to reduce wasted spending on the base.
        //      A doomed base may be saved after all, especially if the enemy messes up.
        if (enemyHitsGround &&
            groundDefendersNeeded + flyingDefendersNeeded >= 8 &&
            int(defenseSquad.getUnits().size()) * 6 < groundDefendersNeeded + flyingDefendersNeeded &&
            (closestEnemyDistance <= 6 * 32 || the.groundHitsFixed.inRange(base->getDepot())) &&
            base->getNumUnits(UnitUtil::GetGroundStaticDefenseType(the.selfRace())) == 0)
        {
            base->setDoomed(true);
        }

        // Final check: If the squad is empty, clear it after all.
        // No advantage in having it stick around for extraFrames time.
        if (defenseSquad.getUnits().empty())
        {
            _squadData.removeSquad(squadName.str());
        }
    }
}

void CombatCommander::updateDefenseSquadUnits(Squad & defenseSquad, const size_t & flyingDefendersNeeded, const size_t & groundDefendersNeeded, bool pullWorkers, bool enemyHasAntiAir)
{
    const BWAPI::Unitset & squadUnits = defenseSquad.getUnits();

    // Count what is already in the squad, being careful not to double-count a unit as air and ground defender.
    size_t flyingDefendersInSquad = 0;
    size_t groundDefendersInSquad = 0;
    size_t versusBoth = 0;
    for (BWAPI::Unit defender : squadUnits)
    {
        bool versusAir = UnitUtil::CanAttackAir(defender);
        bool versusGround = UnitUtil::CanAttackGround(defender);
        if (versusGround && versusAir)
        {
            ++versusBoth;
        }
        else if (versusGround)
        {
            ++groundDefendersInSquad;
        }
        else if (versusAir)
        {
            ++flyingDefendersInSquad;
        }
    }
    // Assign dual-purpose units to whatever side needs them, priority to ground.
    if (groundDefendersNeeded > groundDefendersInSquad)
    {
        size_t add = std::min(versusBoth, groundDefendersNeeded - groundDefendersInSquad);
        groundDefendersInSquad += add;
        versusBoth -= add;
    }
    if (flyingDefendersNeeded > flyingDefendersInSquad)
    {
        size_t add = std::min(versusBoth, flyingDefendersNeeded - flyingDefendersInSquad);
        flyingDefendersInSquad += add;
    }

    //DebugTacticalPrintf("defenders %d/%d %d/%d",
    //	groundDefendersInSquad, groundDefendersNeeded, flyingDefendersInSquad, flyingDefendersNeeded);

    // Add flying defenders.
    size_t flyingDefendersAdded = 0;
    BWAPI::Unit defenderToAdd;
    while (flyingDefendersNeeded > flyingDefendersInSquad + flyingDefendersAdded &&
        (defenderToAdd = findClosestDefender(defenseSquad, defenseSquad.getOrder().getPosition(), true, false, enemyHasAntiAir)))
    {
        _squadData.assignUnitToSquad(defenderToAdd, defenseSquad);
        ++flyingDefendersAdded;
    }

    // Add ground defenders.
    size_t groundDefendersAdded = 0;
    while (groundDefendersNeeded > groundDefendersInSquad + groundDefendersAdded &&
        (defenderToAdd = findClosestDefender(defenseSquad, defenseSquad.getOrder().getPosition(), false, pullWorkers, enemyHasAntiAir)))
    {
        if (defenderToAdd->getType().isWorker())
        {
            UAB_ASSERT(pullWorkers, "pulled worker defender mistakenly");
            WorkerManager::Instance().setCombatWorker(defenderToAdd);
        }
        _squadData.assignUnitToSquad(defenderToAdd, defenseSquad);
        ++groundDefendersAdded;
    }

    int infantryCount = 0;
    BWAPI::Position escortPosition = BWAPI::Positions::Invalid;
    int frontDistance = MAX_DISTANCE;
    BWAPI::Unitset medics;
    for (BWAPI::Unit unit : defenseSquad.getUnits())
    {
        if (!unit || !unit->exists() || unit->isLoaded()) continue;
        const BWAPI::UnitType type = unit->getType();
        if (type == BWAPI::UnitTypes::Terran_Marine || type == BWAPI::UnitTypes::Terran_Firebat)
        {
            ++infantryCount;
            const int distance = unit->getDistance(defenseSquad.getOrder().getPosition());
            if (unit->getPosition().isValid() && distance < frontDistance)
            {
                frontDistance = distance;
                escortPosition = unit->getPosition();
            }
        }
        else if (type == BWAPI::UnitTypes::Terran_Medic) medics.insert(unit);
    }
    const int wantedMedics = (infantryCount + 4) / 5;
    // Release support when the infantry is released or replaced by vehicles.
    while (int(medics.size()) > wantedMedics)
    {
        BWAPI::Unit medic = *medics.begin();
        defenseSquad.removeUnit(medic);
        medics.erase(medic);
    }
    while (int(medics.size()) < wantedMedics)
    {
        BWAPI::Unit closest = nullptr;
        int nearest = MAX_DISTANCE;
        for (BWAPI::Unit unit : _combatUnits)
        {
            if (!unit || !unit->exists() || !unit->isCompleted() || unit->isLoaded() ||
                !unit->getPosition().isValid() || unit->getType() != BWAPI::UnitTypes::Terran_Medic ||
                unit->isLockedDown() || unit->isStasised() || unit->isMaelstrommed() ||
                !_squadData.canAssignUnitToSquad(unit, defenseSquad) ||
                !escortPosition.isValid() ||
                !the.partitions.connected(unit->getPosition(), escortPosition))
                continue;
            const int distance = unit->getDistance(escortPosition);
            if (distance < nearest) { closest = unit; nearest = distance; }
        }
        if (!closest) break;
        _squadData.assignUnitToSquad(closest, defenseSquad);
        medics.insert(closest);
    }
}

// Choose a defender to join the base defense squad.
BWAPI::Unit CombatCommander::findClosestDefender(const Squad & defenseSquad, BWAPI::Position pos, bool flyingDefender, bool pullWorkers, bool enemyHasAntiAir)
{
    BWAPI::Unit closestDefender = nullptr;
    int minDistance = MAX_DISTANCE;

    for (BWAPI::Unit unit : _combatUnits)
    {
        if (isExpansionSupportVulture(unit) ||
            isExpansionFerryDropship(unit) ||
            WorkerManager::Instance().isExpansionFerryWorker(unit))
        {
            continue;
        }

        if (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            continue;
        }
        if (flyingDefender && !UnitUtil::CanAttackAir(unit) ||
            !flyingDefender && !UnitUtil::CanAttackGround(unit))
        {
            continue;
        }

        if (!_squadData.canAssignUnitToSquad(unit, defenseSquad))
        {
            continue;
        }

        int dist = unit->getDistance(pos);

        if (flyingDefender &&
            StrategyManager::Instance().pureAirPlanActive())
        {
            // Prefer a compact Goliath garrison. Marines remain available for
            // the attacking Bio + PureAir group unless no Goliath can respond.
            if (unit->getType() == BWAPI::UnitTypes::Terran_Goliath)
            {
                dist -= 16 * 32;
            }
            else if (unit->getType() == BWAPI::UnitTypes::Terran_Marine)
            {
                dist += 8 * 32;
            }
        }

        // Pull workers only if requested, and not from distant bases.
        if (unit->getType().isWorker())
        {
            if (!pullWorkers ||
                dist > 18 * 32 ||
                WorkerManager::Instance().getNumCombatWorkers() >= 6)
            {
                continue;
            }
            // Pull workers only if other units are considerably farther away.
            dist += 12 * 32;
        }

        // If the enemy can't shoot up, prefer air units as defenders.
        if (!enemyHasAntiAir && unit->isFlying())
        {
            dist -= 12 * 32;     // may become negative - that's OK
        }

        if (dist < minDistance)
        {
            closestDefender = unit;
            minDistance = dist;
        }
    }

    return closestDefender;
}

// NOTE This implementation is kind of cheesy. Orders ought to be delegated to a squad.
//      It can cause double-commanding, which is bad.
void CombatCommander::loadOrUnloadBunkers()
{
    

    const BWAPI::Race enemyRace = the.enemy()->getRace();
    const OpeningPlan enemyPlan =
        OpponentModel::Instance().getPreparationEnemyPlan();
    const bool rushGarrison =
        the.now() <= 10 * 60 * 24 &&
        (enemyRace == BWAPI::Races::Protoss ||
         enemyRace == BWAPI::Races::Zerg) &&
        (enemyPlan == OpeningPlan::FastRush ||
         enemyPlan == OpeningPlan::HeavyRush);

    const bool noWall = !the.placer.hasTerranMainWall();
    const bool bioTankFieldPush =
        StrategyManager::Instance().bioTankPlanActive() &&
        StrategyManager::Instance().bioTankPushReady() &&
        _squadData.squadExists("Ground") &&
        _squadData.getSquad("Ground").getOrder().getStatus()
            .compare(0, 7, "Attack ") == 0;
    const bool fieldArmyReleased =
        bioTankFieldPush || the.placer.isTerranMainWallReleased();
    std::set<BWAPI::Unit> marinesAssignedThisFrame;
    // A bunker load order is issued to the bunker, not the Marine. If that
    // Marine is still travelling, repeating the same order every frame
    // interrupts its approach and floods the replay with PickupBunker orders.
    static std::map<int, int> lastLoadAttempt;
    static int lastLoadFrame = -1;
    if (lastLoadFrame >= 0 && the.now() < lastLoadFrame)
    {
        lastLoadAttempt.clear();
    }
    lastLoadFrame = the.now();

    for (const auto bunker : the.self()->getUnits())
    {
        if (bunker->getType() == BWAPI::UnitTypes::Terran_Bunker)
        {
            // BWAPI::Broodwar->drawCircleMap(bunker->getPosition(), 12 * 32, BWAPI::Colors::Cyan);
            // BWAPI::Broodwar->drawCircleMap(bunker->getPosition(), 18 * 32, BWAPI::Colors::Orange);
            
            // Are there enemies close to the bunker?
            bool enemyIsNear = false;

            // 1. Is any enemy unit within a small radius?
            BWAPI::Unitset enemiesNear = BWAPI::Broodwar->getUnitsInRadius(bunker->getPosition(), 12 * 32,
                BWAPI::Filter::IsEnemy);
            if (enemiesNear.empty())
            {
                // 2. Is a fast enemy unit within a wider radius?
                enemiesNear = BWAPI::Broodwar->getUnitsInRadius(bunker->getPosition(), 18 * 32,
                    BWAPI::Filter::IsEnemy &&
                        (BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Vulture ||
                         BWAPI::Filter::GetType == BWAPI::UnitTypes::Zerg_Mutalisk)
                    );
                enemyIsNear = !enemiesNear.empty();
            }
            else
            {
                enemyIsNear = true;
            }

            const bool keepBunkerLoadedBecauseDefensive =
                !getAggression();

            bool guardUnwalledBase = false;
            if (noWall && the.now() <= 10 * 60 * 24 &&
                enemyRace != BWAPI::Races::Terran)
            {
                for (Base * base : the.bases.getAll())
                {
                    if (base && base->getOwner() == the.self() &&
                        base->getDepot() && base->getDepot()->exists() &&
                        !base->getDepot()->isFlying() &&
                        bunker->getDistance(base->getDepot()) <= 14 * 32)
                    {
                        guardUnwalledBase = true;
                        break;
                    }
                }
            }

            const bool keepMainWallBunkerLoaded =
                the.placer.hasTerranMainWall() &&
                (bunker->getTilePosition() ==
                    the.placer.getTerranMainWallBunkerTile() ||
                 the.placer.isNearTerranMainWall(
                    bunker->getPosition(), 7 * 32));

            // BioTank needs its opening Marines outside the wall Bunker when
            // the field push starts. Otherwise the Tanks receive the attack
            // order alone while the screen remains loaded behind the gate.
            // Keep the garrison during an immediate local threat.
            if (((keepMainWallBunkerLoaded &&
                  (the.placer.isTerranMainWallReleased() ||
                   bioTankFieldPush)) ||
                 (noWall && !guardUnwalledBase &&
                  !keepBunkerLoadedBecauseDefensive)) &&
                !enemyIsNear && !rushGarrison)
            {
                if (!bunker->getLoadedUnits().empty() &&
                    the.now() - bunker->getLastCommandFrame() >= 24)
                {
                    the.micro.UnloadAll(bunker);
                }
                continue;
            }

            // Before wall release, the four opening Marines guard the gate.
            if (enemyIsNear || (!fieldArmyReleased && rushGarrison) ||
                (!fieldArmyReleased && keepBunkerLoadedBecauseDefensive) ||
                keepMainWallBunkerLoaded || guardUnwalledBase)
            {
                // Load one marine at a time if there is free space.
                if (bunker->getSpaceRemaining() > 0)
                {
                    const auto previousAttempt = lastLoadAttempt.find(bunker->getID());
                    // Let the Marine finish the current trip. Replacing the
                    // PickupBunker target every 12 frames made several nearby
                    // Marines swap between their squad rally and this bunker.
                    BWAPI::Unit pendingPassenger = bunker->getOrder() ==
                        BWAPI::Orders::PickupBunker
                        ? bunker->getOrderTarget() : nullptr;
                    if (pendingPassenger && pendingPassenger->exists() &&
                        !pendingPassenger->isLoaded() &&
                        previousAttempt != lastLoadAttempt.end() &&
                        the.now() - previousAttempt->second < 4 * 24)
                    {
                        continue;
                    }
                    if (previousAttempt != lastLoadAttempt.end() &&
                        the.now() - previousAttempt->second < 12)
                    {
                        continue;
                    }
                    BWAPI::Unit marine = nullptr;
                    int bestDistance = rushGarrison ? 18 * 32 : 12 * 32;
                    for (BWAPI::Unit candidate : the.self()->getUnits())
                    {
                        if (!candidate || !candidate->exists() ||
                            candidate->getType() != BWAPI::UnitTypes::Terran_Marine ||
                            !candidate->isCompleted() ||
                            candidate->isLoaded() ||
                            !candidate->getPosition().isValid() ||
                            marinesAssignedThisFrame.count(candidate)) continue;
                        // Once the field army has been released, a bunker may
                        // recruit only its local defense squad. Taking a
                        // Ground/Recon Marine creates competing orders: the
                        // Marine runs toward the bunker, then back to the
                        // attack/rally point every squad update.
                        Squad* owner = _squadData.getUnitSquad(candidate);
                        if (fieldArmyReleased &&
                            (!owner ||
                             owner->getOrder().getType() != SquadOrderTypes::Defend))
                        {
                            continue;
                        }
                        BWAPI::Unit assignedBunker = candidate->getOrderTarget();
                        if (assignedBunker && assignedBunker->exists() &&
                            assignedBunker->getType() == BWAPI::UnitTypes::Terran_Bunker &&
                            assignedBunker != bunker) continue;
                        const int distance = candidate->getDistance(bunker);
                        if (distance < bestDistance)
                        {
                            bestDistance = distance;
                            marine = candidate;
                        }
                    }

                    if (marine)
                    {
                        marinesAssignedThisFrame.insert(marine);
                        lastLoadAttempt[bunker->getID()] = the.now();
                        the.micro.Load(bunker, marine);
                    }
                }
            }
        }
    }
}


void CombatCommander::updateScienceVessels()
{
    BWAPI::Unitset vesselUnits;
    BWAPI::Unitset escortUnits;

    for (BWAPI::Unit unit : _combatUnits)
    {
        if (!unit || !unit->exists() || !unit->isCompleted() || !unit->getPosition().isValid())
        {
            continue;
        }

        if (unit->getType() == BWAPI::UnitTypes::Terran_Science_Vessel)
        {
            Squad* squad = _squadData.getUnitSquad(unit);
            if (squad)
            {
                squad->removeUnit(unit);
            }

            vesselUnits.insert(unit);
            continue;
        }

        if (unit->getType().isDetector())
        {
            continue;
        }

        if (unit->getType().spaceProvided() > 0)
        {
            continue;
        }

        escortUnits.insert(unit);
    }

    if (vesselUnits.empty())
    {
        _scienceVesselMicro.setUnits(BWAPI::Unitset());
        return;
    }

    // In PureAir the AntiAir squad is the actual attacking army. Following
    // Ground made vessels (and, indirectly, the tanks using their vision)
    // turn back toward the old tank group as soon as the strategy changed.
    // Fall back to Ground if the squad is not present yet on the transition
    // frame.
    Squad* escortSquad = &_squadData.getSquad("Ground");
    if (StrategyManager::Instance().pureAirPlanActive() &&
        _squadData.squadExists("AntiAir"))
    {
        Squad& antiAirSquad = _squadData.getSquad("AntiAir");
        BWAPI::Unitset antiAirEscorts;

        for (BWAPI::Unit unit : antiAirSquad.getUnits())
        {
            if (!unit ||
                !unit->exists() ||
                !unit->isCompleted() ||
                !unit->getPosition().isValid() ||
                unit->getType() == BWAPI::UnitTypes::Terran_Science_Vessel ||
                unit->getType().isDetector() ||
                unit->getType().spaceProvided() > 0)
            {
                continue;
            }

            antiAirEscorts.insert(unit);
        }

        if (!antiAirEscorts.empty())
        {
            escortSquad = &antiAirSquad;
            escortUnits = antiAirEscorts;
        }
    }

    _scienceVesselMicro.setOrder(escortSquad->getOrder());
    _scienceVesselMicro.setUnits(vesselUnits);
    _scienceVesselMicro.setSquadSize(int(vesselUnits.size() + escortUnits.size()));

    BWAPI::Unit vanguard = nullptr;
    int bestDist = MAX_DISTANCE;
    BWAPI::Position goal = escortSquad->getOrder().getPosition();

    if (goal.isValid())
    {
        for (BWAPI::Unit unit : escortUnits)
        {
            int d = unit->getDistance(goal);
            if (d < bestDist)
            {
                bestDist = d;
                vanguard = unit;
            }
        }
    }

    _scienceVesselMicro.setUnitClosestToTarget(vanguard);
    _scienceVesselMicro.setDetectionRequest(
        _lastUndetectedEnemyPosition,
        _lastUndetectedEnemyFrame);
    _scienceVesselMicro.setExpansionDetectionRequest(
        _expansionDetectionPosition,
        _expansionDetectionFrame);
    _scienceVesselMicro.go(escortUnits);
}

void CombatCommander::requestExpansionVesselDetection(
    const BWAPI::Position& position)
{
    if (position.isValid())
    {
        _expansionDetectionPosition = position;
        _expansionDetectionFrame = the.now();
    }
}

// Should squads have detectors assigned? Does not apply to all squads.
// Yes if the enemy has cloaked units. Also yes if the enemy is protoss and has observers
// and we have cloaked units--we want to shoot down those observers.
// Otherwise no if the detectors are in danger of dying.
bool CombatCommander::wantSquadDetectors() const
{
    if (StrategyManager::Instance().getOpeningGroup() == "tanks")
    {
        return true;
    }

    if (the.enemy()->getRace() == BWAPI::Races::Protoss &&
        the.info.enemyHasMobileDetection() &&
        the.info.weHaveCloakTech())
    {
        return true;
    }

    return
                !the.info.enemyHasAntiAir() ||
        the.info.enemyCloakedUnitsSeen();
}

// Add or remove a given squad's detector, subject to availability.
// Because this checks the content of the squad, it should be called
// after any other units are added or removed.
void CombatCommander::maybeAssignDetector(Squad& squad, bool wantDetector)
{
    if (squad.hasDetector())
    {
        // Science vessels are controlled globally, never by squads.
        for (BWAPI::Unit unit : squad.getUnits())
        {
            if (unit->getType() == BWAPI::UnitTypes::Terran_Science_Vessel)
            {
                squad.removeUnit(unit);
                return;
            }
        }

        // If the detector is the only thing left in the squad, we don't want to keep it.
        if (!wantDetector || squad.getUnits().size() == 1)
        {
            for (BWAPI::Unit unit : squad.getUnits())
            {
                if (unit->getType().isDetector())
                {
                    squad.removeUnit(unit);
                    return;
                }
            }
        }
    }
    else
    {
        // Don't add a detector to an empty squad.
        if (wantDetector && !squad.getUnits().empty())
        {
            for (BWAPI::Unit unit : _combatUnits)
            {
                if (!unit->getType().isDetector())
                {
                    continue;
                }

                if (unit->getType() == BWAPI::UnitTypes::Terran_Science_Vessel)
                {
                    continue;
                }

                if (_squadData.canAssignUnitToSquad(unit, squad))
                {
                    _squadData.assignUnitToSquad(unit, squad);
                    return;
                }
            }
        }
    }
}

namespace
{
    bool comsatAreaFullyVisible(const BWAPI::Position& position)
    {
        if (!position.isValid())
        {
            return false;
        }

        const int sampleOffset = 5 * 32;
        const BWAPI::Position samples[] =
        {
            position,
            position + BWAPI::Position(sampleOffset, 0),
            position + BWAPI::Position(-sampleOffset, 0),
            position + BWAPI::Position(0, sampleOffset),
            position + BWAPI::Position(0, -sampleOffset)
        };

        for (BWAPI::Position sample : samples)
        {
            sample.makeValid();
            const BWAPI::TilePosition tile(sample);
            if (!tile.isValid() || !BWAPI::Broodwar->isVisible(tile))
            {
                return false;
            }
        }

        return true;
    }
}

bool CombatCommander::comsatScanActiveOrPending(
    const BWAPI::Position& position,
    int radius) const
{
    if (!position.isValid())
    {
        return false;
    }

    if (MapGrid::Instance().scanIsActiveAt(position))
    {
        return true;
    }

    const int now = BWAPI::Broodwar->getFrameCount();
    for (const ComsatScanRequest& request : _comsatScanRequests)
    {
        if (request.expiryFrame >= now &&
            request.position.isValid() &&
            request.position.getApproxDistance(position) <= std::max(0, radius))
        {
            return true;
        }
    }

    return false;
}

bool CombatCommander::requestComsatScan(
    const BWAPI::Position& position,
    ComsatScanPriority priority,
    int minimumEnergy,
    int reserveEnergy,
    int lifetimeFrames,
    const std::string& reason,
    bool allowVisibleArea,
    bool requireFreshComsat)
{
    if (the.selfRace() != BWAPI::Races::Terran || !position.isValid())
    {
        return false;
    }

    if (MapGrid::Instance().scanIsActiveAt(position))
    {
        return true;
    }

    const int now = BWAPI::Broodwar->getFrameCount();
    if (_lastComsatQueueFrame > now)
    {
        _comsatScanRequests.clear();
        _comsatsUsedForStrategicScouting.clear();
    }
    _lastComsatQueueFrame = now;

    _comsatScanRequests.erase(
        std::remove_if(
            _comsatScanRequests.begin(),
            _comsatScanRequests.end(),
            [now](const ComsatScanRequest& request)
            {
                return request.expiryFrame < now ||
                    !request.position.isValid() ||
                    MapGrid::Instance().scanIsActiveAt(request.position);
            }),
        _comsatScanRequests.end());

    const int mergeRadius = 5 * 32;
    for (ComsatScanRequest& request : _comsatScanRequests)
    {
        if (request.position.getApproxDistance(position) > mergeRadius)
        {
            continue;
        }

        request.expiryFrame = std::max(
            request.expiryFrame,
            now + std::max(1, lifetimeFrames));

        if (int(priority) >= int(request.priority))
        {
            request.position = position;
            request.priority = priority;
            request.minimumEnergy = std::max(50, minimumEnergy);
            request.reserveEnergy = std::max(0, reserveEnergy);
            request.requestedFrame = now;
            request.allowVisibleArea = allowVisibleArea;
            request.requireFreshComsat = requireFreshComsat;
            request.reason = reason;
        }
        return true;
    }

    ComsatScanRequest request;
    request.position = position;
    request.priority = priority;
    request.minimumEnergy = std::max(50, minimumEnergy);
    request.reserveEnergy = std::max(0, reserveEnergy);
    request.requestedFrame = now;
    request.expiryFrame = now + std::max(1, lifetimeFrames);
    request.allowVisibleArea = allowVisibleArea;
    request.requireFreshComsat = requireFreshComsat;
    request.reason = reason;
    _comsatScanRequests.push_back(request);
    return true;
}

void CombatCommander::processComsatScanRequests()
{
    const int now = BWAPI::Broodwar->getFrameCount();

    _comsatScanRequests.erase(
        std::remove_if(
            _comsatScanRequests.begin(),
            _comsatScanRequests.end(),
            [now](const ComsatScanRequest& request)
            {
                return request.expiryFrame < now ||
                    !request.position.isValid() ||
                    MapGrid::Instance().scanIsActiveAt(request.position) ||
                    (!request.allowVisibleArea &&
                     comsatAreaFullyVisible(request.position));
            }),
        _comsatScanRequests.end());

    if (_comsatScanRequests.empty())
    {
        return;
    }

    auto bestRequest = _comsatScanRequests.end();
    BWAPI::Unit bestComsat = nullptr;
    int bestEnergy = -1;
    for (auto request = _comsatScanRequests.begin();
        request != _comsatScanRequests.end(); ++request)
    {
        const int requiredEnergy = std::max(
            request->minimumEnergy, 50 + request->reserveEnergy);
        for (BWAPI::Unit comsat : the.self()->getUnits())
        {
            if (!comsat || !comsat->exists() || !comsat->isCompleted() ||
                comsat->getType() != BWAPI::UnitTypes::Terran_Comsat_Station ||
                comsat->getEnergy() < requiredEnergy ||
                (request->requireFreshComsat &&
                 _comsatsUsedForStrategicScouting.find(comsat->getID()) !=
                    _comsatsUsedForStrategicScouting.end()) ||
                !comsat->canUseTech(
                    BWAPI::TechTypes::Scanner_Sweep,
                    request->position))
            {
                continue;
            }
            if (bestRequest == _comsatScanRequests.end() ||
                int(request->priority) > int(bestRequest->priority) ||
                (request->priority == bestRequest->priority &&
                 request->requestedFrame < bestRequest->requestedFrame) ||
                (request == bestRequest && comsat->getEnergy() > bestEnergy))
            {
                bestRequest = request;
                bestComsat = comsat;
                bestEnergy = comsat->getEnergy();
            }
        }
    }

    if (bestRequest == _comsatScanRequests.end())
    {
        return;
    }

    const BWAPI::Position target = bestRequest->position;
    const std::string reason = bestRequest->reason;
    const bool freshStrategicScan = bestRequest->requireFreshComsat;

    if (!the.micro.ScanWithComsat(bestComsat, target))
    {
        return;
    }

    if (freshStrategicScan)
    {
        _comsatsUsedForStrategicScouting.insert(bestComsat->getID());
    }

    _lastComsatTarget = target;
    _lastComsatTargetFrame = now;
    _lastComsatReason = reason;

    DebugTacticalPrintf(
        "COMSAT priority=%d reason=%s energy=%d pos=(%d,%d)",
        int(bestRequest->priority),
        reason.c_str(),
        bestEnergy,
        target.x,
        target.y);

    _comsatScanRequests.erase(
        std::remove_if(
            _comsatScanRequests.begin(),
            _comsatScanRequests.end(),
            [target](const ComsatScanRequest& request)
            {
                return request.position.getApproxDistance(target) <= 6 * 32;
            }),
        _comsatScanRequests.end());
}

namespace
{
    const int MainBaseScoutScanCooldown = 24 * 18;
    const int OptionalScoutScanMinEnergy = 100;
    const int FirstStrategicScanMinEnergy = 50;
    const int AttackLocationRefreshFrames = 12;

    BWAPI::Unit getStrategicScoutComsat(
        const BWAPI::Position& targetPosition,
        int minimumEnergy,
        const std::set<int>& alreadyUsedForStrategicScouting,
        bool requireUnusedComsat)
    {
        BWAPI::Unit bestComsat = nullptr;
        int bestEnergy = -1;

        for (BWAPI::Unit comsat : the.self()->getUnits())
        {
            if (!comsat ||
                !comsat->exists() ||
                !comsat->isCompleted() ||
                comsat->getType() != BWAPI::UnitTypes::Terran_Comsat_Station ||
                comsat->getEnergy() < minimumEnergy)
            {
                continue;
            }

            if (requireUnusedComsat &&
                alreadyUsedForStrategicScouting.find(comsat->getID()) !=
                    alreadyUsedForStrategicScouting.end())
            {
                continue;
            }

            if (!comsat->canUseTech(
                BWAPI::TechTypes::Scanner_Sweep,
                targetPosition))
            {
                continue;
            }

            if (comsat->getEnergy() > bestEnergy)
            {
                bestEnergy = comsat->getEnergy();
                bestComsat = comsat;
            }
        }

        return bestComsat;
    }

    BWAPI::Unit getAnyComsat()
    {
        BWAPI::Unit bestComsat = nullptr;
        int bestEnergy = -1;

        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (!unit ||
                !unit->exists() ||
                unit->getType() != BWAPI::UnitTypes::Terran_Command_Center)
            {
                continue;
            }

            BWAPI::Unit addon = unit->getAddon();

            if (!addon ||
                !addon->exists() ||
                addon->getType() != BWAPI::UnitTypes::Terran_Comsat_Station)
            {
                continue;
            }

            if (addon->getEnergy() > bestEnergy)
            {
                bestEnergy = addon->getEnergy();
                bestComsat = addon;
            }
        }

        return bestComsat;
    }

    bool knownEnemyResourceDepotExists()
    {
        for (Base* base : the.bases.getAll())
        {
            if (base && base->getOwner() == the.enemy())
            {
                return true;
            }
        }

        for (const auto& kv : the.info.getUnitInfo(the.enemy()))
        {
            const UnitInfo& ui = kv.second;
            if (ui.type.isResourceDepot() &&
                ui.lastPosition.isValid() &&
                !ui.goneFromLastPosition)
            {
                return true;
            }
        }

        return false;
    }

    bool lateGameCleanupSweepNeeded()
    {
        return
            the.now() >= 12 * 60 * 24 &&
            !knownEnemyResourceDepotExists();
    }

    bool tryLateGameCleanupScan()
    {
        static int lastScanFrame = -100000;
        static int lastObservedFrame = -1;
        static int sweepLane = 0;

        const int now = the.now();
        if (lastObservedFrame >= 0 && now < lastObservedFrame)
        {
            lastScanFrame = -100000;
            sweepLane = 0;
        }
        lastObservedFrame = now;

        if (now - lastScanFrame < 7 * 24)
        {
            return false;
        }

        const BWAPI::Position scanPosition =
            MapGrid::Instance().getLeastExploredForSweep(
                false,
                1,
                sweepLane++);

        if (!scanPosition.isValid() ||
            MapGrid::Instance().scanIsActiveAt(scanPosition))
        {
            return false;
        }

        if (!CombatCommander::Instance().requestComsatScan(
                scanPosition,
                ComsatScanPriority::Cleanup,
                OptionalScoutScanMinEnergy,
                50,
                10 * 24,
                "late cleanup"))
        {
            return false;
        }

        lastScanFrame = now;
        return true;
    }

    bool baseHasKnownEnemyBuildings(Base* base)
    {
        if (!base || !base->getPosition().isValid())
        {
            return false;
        }

        const int radius = 12 * 32;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->getPosition().isValid())
            {
                continue;
            }

            if (!enemy->getType().isBuilding() || enemy->isFlying())
            {
                continue;
            }

            if (enemy->getDistance(base->getPosition()) <= radius)
            {
                return true;
            }
        }

        // BWAPI's visible unit set loses a bunker as soon as fog covers it.
        // Strategic scouting must also use InformationManager's remembered
        // buildings, otherwise a base inferred from a previously seen bunker
        // can never qualify for a confirming Comsat scan.
        for (const auto& kv : the.info.getUnitInfo(the.enemy()))
        {
            const UnitInfo& ui = kv.second;
            if (!ui.type.isBuilding() || ui.type.isAddon() || ui.lifted ||
                ui.goneFromLastPosition || !ui.lastPosition.isValid())
            {
                continue;
            }

            if (ui.lastPosition.getApproxDistance(base->getPosition()) <= radius)
            {
                return true;
            }
        }

        return false;
    }

    Base* enemyBuildingSupportedStart()
    {
        Base* inferredEnemyMain = the.bases.enemyStart();
        if (inferredEnemyMain &&
            inferredEnemyMain->isAStartingBase() &&
            inferredEnemyMain != the.bases.myStart() &&
            baseHasKnownEnemyBuildings(inferredEnemyMain))
        {
            return inferredEnemyMain;
        }

        for (Base* base : the.bases.getStarting())
        {
            if (base &&
                base != the.bases.myStart() &&
                base->getOwner() != the.self() &&
                base->getCenter().isValid() &&
                baseHasKnownEnemyBuildings(base))
            {
                return base;
            }
        }

        return nullptr;
    }

    bool baseWasRecentlyVisibleAndEmpty(Base* base)
    {
        if (!base || !base->getCenterTile().isValid())
        {
            return false;
        }

        if (!BWAPI::Broodwar->isVisible(base->getCenterTile()))
        {
            return false;
        }

        return !baseHasKnownEnemyBuildings(base);
    }

    BWAPI::Position getEnemyMainScanPosition(int scanLane)
    {
        Base* enemyMain = the.bases.enemyStart();

        // Only a directly confirmed enemy main is final. An elimination-based
        // enemyStart is still a candidate and must pass through the same START
        // location verification path as every other candidate. This prevents
        // Comsat from reinforcing a bad inference on maps such as Tau Cross.
        if (enemyMain && the.bases.enemyStartConfirmed() &&
            enemyMain->getCenter().isValid())
        {
            if (baseWasRecentlyVisibleAndEmpty(enemyMain))
            {
                return BWAPI::Positions::Invalid;
            }

            // Repeatedly scanning only the depot misses Stargates, Fleet
            // Beacons, Robotics and other transition tech placed around the
            // edge of the main. Cover the center first, then rotate over four
            // overlapping edge sectors before returning to the center.
            const int offset = 7 * 32;
            const BWAPI::Position scanOffsets[] =
            {
                BWAPI::Position(0, 0),
                BWAPI::Position(offset, 0),
                BWAPI::Position(0, offset),
                BWAPI::Position(-offset, 0),
                BWAPI::Position(0, -offset)
            };

            BWAPI::Position result = enemyMain->getCenter() +
                scanOffsets[scanLane % 5];
            result.makeValid();
            return result;
        }

        // A remembered grounded enemy building at a START location is direct
        // evidence, unlike elimination alone. Scan it to confirm the depot and
        // opening even when Bases has not yet promoted it to enemyStart.
        Base* buildingSupportedStart = enemyBuildingSupportedStart();
        if (buildingSupportedStart)
        {
            return buildingSupportedStart->getCenter();
        }

        // If elimination produced a candidate, verify that exact START first.
        // A sweep which reveals it empty will cause Bases::updateEnemyStart()
        // to revoke the inference on the next frame.
        if (enemyMain && !the.bases.enemyStartConfirmed() &&
            enemyMain->isAStartingBase() && enemyMain->getCenter().isValid() &&
            !the.bases.startLocationScouted(enemyMain))
        {
            return enemyMain->getCenter();
        }

        // Before the enemy start is known, only unexplored STARTING locations
        // are legitimate strategic-scout targets. Expansion sites are handled
        // later by the dedicated late-game cleanup sweep.
        Base* best = nullptr;
        int bestLastSeen = MAX_FRAME;

        for (Base* base : the.bases.getStarting())
        {
            if (!base ||
                base == the.bases.myStart() ||
                !base->getPosition().isValid() ||
                the.bases.startLocationScouted(base))
            {
                continue;
            }

            const int lastSeen =
                MapGrid::Instance().getCell(base->getCenter()).timeLastVisited;

            if (!best || lastSeen < bestLastSeen)
            {
                best = base;
                bestLastSeen = lastSeen;
            }
        }

        return best ? best->getCenter() : BWAPI::Positions::Invalid;
    }

    bool tryMainBaseScoutScan()
    {
        static int lastMainBaseScoutScanFrame = -100000;
        static int lastObservedFrame = -1;
        static bool usedLowEnergyUnknownPlanScan = false;
        static bool usedLowEnergyAntiMutaTechScan = false;
        static int strategicScanLane = 0;
        static std::set<int> comsatsUsedForStrategicScouting;

        const int now = BWAPI::Broodwar->getFrameCount();

        // Static state survives when the same bot process starts a new match.
        if (lastObservedFrame >= 0 && now < lastObservedFrame)
        {
            lastMainBaseScoutScanFrame = -100000;
            usedLowEnergyUnknownPlanScan = false;
            usedLowEnergyAntiMutaTechScan = false;
            strategicScanLane = 0;
            comsatsUsedForStrategicScouting.clear();
        }
        lastObservedFrame = now;

        if (now - lastMainBaseScoutScanFrame < MainBaseScoutScanCooldown)
        {
            return false;
        }

        Base* enemyMain = the.bases.enemyStart();
        const bool enemyMainConfirmed =
            enemyMain && the.bases.enemyStartConfirmed();
        Base* buildingSupportedStart = enemyBuildingSupportedStart();

        // Elimination alone is still not enough to spend a scan on Tau Cross.
        // A bunker or another remembered grounded building at that START is
        // concrete evidence and is now allowed to trigger the confirming scan.
        // Emergency detection and Tank-line vision remain independent.
        if (!enemyMainConfirmed && !buildingSupportedStart)
        {
            return false;
        }

        BWAPI::Unit workerScout = ScoutManager::Instance().getWorkerScout();

        const bool workerScoutUnavailable =
            !workerScout ||
            !workerScout->exists() ||
            workerScout->getHitPoints() <= 0;

        const bool enemyOpeningUnknown =
            OpponentModel::Instance().getEnemyPlan() == OpeningPlan::Unknown;

        // Normally keep enough energy for one emergency detection scan. If
        // the worker scout died and the opening is still Unknown, allow one
        // immediate 50-energy strategic scan. The information revealed by the
        // sweep is consumed by InformationManager/OpponentModel on frame update.
        const bool allowFirstLowEnergyStrategicScan =
            !usedLowEnergyUnknownPlanScan &&
            workerScoutUnavailable &&
            enemyOpeningUnknown;

        // When UCB deliberately opened TvZ_AntiMuta, spend one early 50-energy
        // required scan on the known enemy main during the normal Lair/Spire
        // window. Heavy Ling pressure often kills or blocks the worker scout,
        // and waiting for the optional 100-energy scan reveals the first Muta
        // wave instead of the technology which creates it.
        const bool antiMutaTechAlreadyKnown =
            OpponentModel::Instance().hasEnemyOpeningThreat(
                OpeningThreat::ZergMutaliskTech) ||
            OpponentModel::Instance().hasEnemyOpeningThreat(
                OpeningThreat::ZergTwoHatchMuta);

        const bool allowFirstLowEnergyAntiMutaTechScan =
            !usedLowEnergyAntiMutaTechScan &&
            BWAPI::Broodwar->enemy()->getRace() == BWAPI::Races::Zerg &&
            StrategyManager::Instance().getTerranPlan() ==
                TerranPlanId::TvZ_AntiMuta &&
            !antiMutaTechAlreadyKnown &&
            now >= 4 * 60 * 24 &&
            now <= 8 * 60 * 24;

        const BWAPI::Position scanPosition =
            getEnemyMainScanPosition(strategicScanLane);

        if (!scanPosition.isValid())
        {
            return false;
        }

        const bool earlyFrequentScoutWindow =
            now < 10 * 60 * 24;

        const bool cloakThreatKnown =
            the.info.enemyHasCloakTech() ||
            OpponentModel::Instance().hasEnemyOpeningThreat(
                OpeningThreat::CloakTech);

        // Until 10:00, retain a smaller 25-energy reserve so the enemy main is
        // checked often enough to see its transition. Once cloak is known, or
        // after 10:00, restore the normal complete 50-energy emergency reserve.
        int regularMinimumEnergy =
            earlyFrequentScoutWindow && !cloakThreatKnown
            ? 75
            : OptionalScoutScanMinEnergy;

        // Once a PureAir force is preparing to fight, routine base scouting
        // must leave two scan charges for the imminent air engagement and
        // possible cloak detection. Unknown openings keep their one-shot
        // emergency exception below.
        const bool protectPureAirEngagementEnergy =
            StrategyManager::Instance().getTerranArmyPosture() ==
                TerranArmyPosture::PureAir &&
            !enemyOpeningUnknown;
        if (protectPureAirEngagementEnergy)
        {
            regularMinimumEnergy = std::max(150, regularMinimumEnergy);
        }

        BWAPI::Unit comsat = getStrategicScoutComsat(
            scanPosition,
            regularMinimumEnergy,
            comsatsUsedForStrategicScouting,
            false);

        bool usingFreshComsatAtFifty = false;
        bool usingUnknownPlanFallback = false;
        bool usingAntiMutaFallback = false;

        // A newly completed Comsat starts with enough energy for one scan.
        // Before 10:00, let each Comsat contribute that first scan immediately;
        // later scans from the same station must meet the 75/100 energy rule.
        if (!comsat && earlyFrequentScoutWindow && !cloakThreatKnown &&
            !protectPureAirEngagementEnergy)
        {
            comsat = getStrategicScoutComsat(
                scanPosition,
                FirstStrategicScanMinEnergy,
                comsatsUsedForStrategicScouting,
                true);
            usingFreshComsatAtFifty = comsat != nullptr;
        }

        // Preserve the old one-shot recovery cases outside the general early
        // schedule: unknown opening after scout loss and learned AntiMuta tech
        // confirmation may still spend a 50-energy scan when no regular scan
        // is currently possible.
        if (!comsat && allowFirstLowEnergyStrategicScan)
        {
            comsat = getStrategicScoutComsat(
                scanPosition,
                FirstStrategicScanMinEnergy,
                comsatsUsedForStrategicScouting,
                false);
            usingUnknownPlanFallback = comsat != nullptr;
        }

        if (!comsat && allowFirstLowEnergyAntiMutaTechScan)
        {
            comsat = getStrategicScoutComsat(
                scanPosition,
                FirstStrategicScanMinEnergy,
                comsatsUsedForStrategicScouting,
                false);
            usingAntiMutaFallback = comsat != nullptr;
        }

        if (!comsat)
        {
            return false;
        }

        const int requestedMinimumEnergy =
            (usingFreshComsatAtFifty ||
             usingUnknownPlanFallback ||
             usingAntiMutaFallback)
            ? FirstStrategicScanMinEnergy
            : regularMinimumEnergy;

        if (CombatCommander::Instance().requestComsatScan(
                scanPosition,
                ComsatScanPriority::StrategicScout,
                requestedMinimumEnergy,
                std::max(0, requestedMinimumEnergy - 50),
                MainBaseScoutScanCooldown,
                usingAntiMutaFallback
                    ? "anti-muta tech"
                    : enemyOpeningUnknown
                    ? "unknown opening"
                    : "enemy main tech",
                false,
                usingFreshComsatAtFifty))
        {
            lastMainBaseScoutScanFrame = now;
            comsatsUsedForStrategicScouting.insert(comsat->getID());
            strategicScanLane = (strategicScanLane + 1) % 5;

            if (usingUnknownPlanFallback)
            {
                usedLowEnergyUnknownPlanScan = true;
            }

            if (usingAntiMutaFallback)
            {
                usedLowEnergyAntiMutaTechScan = true;
            }

            DebugTacticalPrintf(
                "COMSAT scout scan enemy base lane=%d energy=%d%s at (%d,%d)",
                (strategicScanLane + 4) % 5,
                comsat->getEnergy(),
                usingFreshComsatAtFifty ? " fresh" : "",
                scanPosition.x,
                scanPosition.y
            );

            BWAPI::Broodwar->drawCircleMap(
                scanPosition,
                40,
                BWAPI::Colors::Yellow,
                false
            );

            return true;
        }

        return false;
    }
}

namespace
{
    bool isRememberedCloakThreat(BWAPI::UnitType type)
    {
        return
            type == BWAPI::UnitTypes::Zerg_Lurker ||
            type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
            type == BWAPI::UnitTypes::Protoss_Dark_Templar ||
            type == BWAPI::UnitTypes::Terran_Wraith ||
            type == BWAPI::UnitTypes::Terran_Ghost ||
            type == BWAPI::UnitTypes::Protoss_Arbiter ||
            type.hasPermanentCloak() ||
            type.isCloakable();
    }

    bool isImportantAirScanType(BWAPI::UnitType type)
    {
        return
            type == BWAPI::UnitTypes::Protoss_Carrier ||
            type == BWAPI::UnitTypes::Terran_Battlecruiser ||
            type == BWAPI::UnitTypes::Zerg_Mutalisk ||
            type == BWAPI::UnitTypes::Terran_Wraith ||
            type == BWAPI::UnitTypes::Protoss_Scout ||
            type == BWAPI::UnitTypes::Protoss_Corsair ||
            type == BWAPI::UnitTypes::Protoss_Arbiter ||
            type == BWAPI::UnitTypes::Zerg_Devourer ||
            type == BWAPI::UnitTypes::Zerg_Guardian;
    }

    int rememberedArmyScanValue(BWAPI::UnitType type)
    {
        if (type.isWorker() || type.isBuilding() || type.isSpell())
        {
            return 0;
        }
        if (!UnitUtil::TypeCanAttackGround(type) &&
            !UnitUtil::TypeCanAttackAir(type) &&
            type != BWAPI::UnitTypes::Zerg_Defiler &&
            type != BWAPI::UnitTypes::Protoss_High_Templar &&
            type != BWAPI::UnitTypes::Terran_Science_Vessel)
        {
            return 0;
        }

        if (type == BWAPI::UnitTypes::Protoss_Carrier ||
            type == BWAPI::UnitTypes::Terran_Battlecruiser)
        {
            return 8;
        }
        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            type == BWAPI::UnitTypes::Protoss_Arbiter ||
            type == BWAPI::UnitTypes::Zerg_Defiler ||
            type == BWAPI::UnitTypes::Protoss_High_Templar)
        {
            return 4;
        }

        return std::max(1, type.supplyRequired());
    }
}

void CombatCommander::queueAutomaticComsatRequests()
{
    const int now = the.now();

    // Visible-but-undetected units are the top priority. This covers Lurkers,
    // Dark Templar, Spider Mines, cloaked Wraiths/Ghosts and any other BWAPI
    // cloak/burrow case without maintaining a second type list elsewhere.
    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isVisible() ||
            unit->isInvincible() || unit->getType().isSpell() ||
            !unit->getPosition().isValid() ||
            (unit->isDetected() &&
             unit->getOrder() != BWAPI::Orders::Burrowing))
        {
            continue;
        }

        _lastUndetectedEnemyPosition = unit->getPosition();
        _lastUndetectedEnemyFrame = now;
        the.info.enemySeenBurrowing();

        requestComsatScan(
            unit->getPosition(),
            ComsatScanPriority::EmergencyDetection,
            50,
            0,
            3 * 24,
            "undetected enemy",
            true);
    }

    // The following memory/army scan evaluation is deliberately periodic. It
    // walks remembered enemy units, but never runs every frame.
    if (_lastAutomaticComsatFrame > now)
    {
        _lastAutomaticComsatFrame = -100000;
    }
    if (now - _lastAutomaticComsatFrame < 12)
    {
        return;
    }
    _lastAutomaticComsatFrame = now;

    BWAPI::Position armyCenter = BWAPI::Positions::Invalid;
    BWAPI::Position armyFront = BWAPI::Positions::Invalid;
    int frontDistanceToOrder = MAX_DISTANCE;
    std::vector<BWAPI::Unit> armyFighters;
    bool attacking = false;
    const char* primarySquadName =
        StrategyManager::Instance().pureAirPlanActive() ? "AntiAir" : "Ground";

    if (_squadData.squadExists(primarySquadName))
    {
        Squad& army = _squadData.getSquad(primarySquadName);
        const SquadOrderTypes orderType = army.getOrder().getType();
        attacking =
            orderType == SquadOrderTypes::Attack ||
            orderType == SquadOrderTypes::OmniAttack;

        int sumX = 0;
        int sumY = 0;
        int count = 0;
        for (BWAPI::Unit unit : army.getUnits())
        {
            if (!unit || !unit->exists() || !unit->isCompleted() ||
                !unit->getPosition().isValid() ||
                unit->getType().isWorker() || unit->getType().isDetector() ||
                unit->getType().isBuilding())
            {
                continue;
            }
            sumX += unit->getPosition().x;
            sumY += unit->getPosition().y;
            ++count;
            armyFighters.push_back(unit);
            if (army.getOrder().getPosition().isValid())
            {
                const int distanceToOrder =
                    unit->getDistance(army.getOrder().getPosition());
                if (distanceToOrder < frontDistanceToOrder)
                {
                    frontDistanceToOrder = distanceToOrder;
                    armyFront = unit->getPosition();
                }
            }
        }
        if (count > 0)
        {
            armyCenter = BWAPI::Position(sumX / count, sumY / count);
        }
    }

    const auto& enemyMemory = the.info.getUnitInfo(the.enemy());

    // A recently remembered cloak-capable unit beside our army/base is a real
    // detection request even when it disappeared between two visible frames.
    for (const auto& entry : enemyMemory)
    {
        const UnitInfo& enemy = entry.second;
        if (enemy.goneFromLastPosition || !enemy.lastPosition.isValid() ||
            !isRememberedCloakThreat(enemy.type) ||
            now - enemy.updateFrame > 12 * 24 ||
            (enemy.unit && enemy.unit->exists() && enemy.unit->isVisible()))
        {
            continue;
        }

        bool friendlyNear =
            armyCenter.isValid() &&
            armyCenter.getApproxDistance(enemy.lastPosition) <= 14 * 32;

        if (!friendlyNear)
        {
            for (Base* base : the.bases.getAll())
            {
                if (base && base->getOwner() == the.self() &&
                    base->getCenter().isValid() &&
                    base->getCenter().getApproxDistance(enemy.lastPosition) <= 12 * 32)
                {
                    friendlyNear = true;
                    break;
                }
            }
        }

        if (friendlyNear)
        {
            _lastUndetectedEnemyPosition = enemy.lastPosition;
            _lastUndetectedEnemyFrame = now;
            requestComsatScan(
                enemy.lastPosition,
                ComsatScanPriority::EmergencyDetection,
                50,
                0,
                4 * 24,
                "suspected cloak",
                true);
        }
    }

    if (!attacking || !armyCenter.isValid())
    {
        return;
    }

    const TerranArmyPosture posture =
        StrategyManager::Instance().getTerranArmyPosture();
    const int memoryLimitFrames = posture == TerranArmyPosture::PureAir
        ? 5 * 24
        : posture == TerranArmyPosture::Mixed ? 7 * 24 : 9 * 24;

    auto engagementReadyForScan = [&](BWAPI::Position target, bool air) -> bool
    {
        if (!target.isValid() ||
            BWAPI::Broodwar->isVisible(BWAPI::TilePosition(target)) ||
            comsatScanActiveOrPending(target, 6 * 32) ||
            (_lastComsatTarget.isValid() &&
             now - _lastComsatTargetFrame < 10 * 24 &&
             _lastComsatTarget.getApproxDistance(target) <= 6 * 32))
        {
            return false;
        }

        const int approachRadius = posture == TerranArmyPosture::PureAir
            ? 9 * 32
            : posture == TerranArmyPosture::Mixed ? 12 * 32 : 14 * 32;
        int relevant = 0;
        int nearFight = 0;
        for (BWAPI::Unit unit : armyFighters)
        {
            if (!(air ? UnitUtil::CanAttackAir(unit)
                      : UnitUtil::CanAttackGround(unit)))
            {
                continue;
            }
            ++relevant;
            if (unit->getDistance(target) <= approachRadius)
            {
                ++nearFight;
            }
        }

        // Require a real fighting group, not one scout or the front unit of a
        // formation whose main body is still waiting back at home.
        const int minimumGroup = posture == TerranArmyPosture::PureAir
            ? 5 : posture == TerranArmyPosture::Mixed ? 4 : 3;
        const int requiredNear = std::max(
            minimumGroup, std::min(10, (relevant + 2) / 3));
        return relevant >= minimumGroup && nearFight >= requiredNear;
    };

    BWAPI::Position importantAirCenter = BWAPI::Positions::Invalid;
    int importantAirScore = 0;
    BWAPI::Position rememberedArmyCenter = BWAPI::Positions::Invalid;
    int rememberedArmyScore = 0;

    for (const auto& seedEntry : enemyMemory)
    {
        const UnitInfo& seed = seedEntry.second;
        if (seed.goneFromLastPosition || !seed.lastPosition.isValid() ||
            now - seed.updateFrame > memoryLimitFrames ||
            (seed.unit && seed.unit->exists() && seed.unit->isVisible()) ||
            (armyCenter.getApproxDistance(seed.lastPosition) > 26 * 32 &&
             (!armyFront.isValid() ||
              armyFront.getApproxDistance(seed.lastPosition) > 20 * 32)))
        {
            continue;
        }

        int airScore = 0;
        int airX = 0;
        int airY = 0;
        int armyScore = 0;
        int armyX = 0;
        int armyY = 0;

        for (const auto& memberEntry : enemyMemory)
        {
            const UnitInfo& member = memberEntry.second;
            if (member.goneFromLastPosition || !member.lastPosition.isValid() ||
                now - member.updateFrame > memoryLimitFrames ||
                member.lastPosition.getApproxDistance(seed.lastPosition) > 10 * 32 ||
                (member.unit && member.unit->exists() && member.unit->isVisible()))
            {
                continue;
            }

            if (isImportantAirScanType(member.type))
            {
                const int value =
                    (member.type == BWAPI::UnitTypes::Protoss_Carrier ||
                     member.type == BWAPI::UnitTypes::Terran_Battlecruiser)
                    ? 8 : 2;
                airScore += value;
                airX += member.lastPosition.x * value;
                airY += member.lastPosition.y * value;
            }

            const int value = rememberedArmyScanValue(member.type);
            if (value > 0)
            {
                armyScore += value;
                armyX += member.lastPosition.x * value;
                armyY += member.lastPosition.y * value;
            }
        }

        if (airScore > importantAirScore)
        {
            importantAirScore = airScore;
            importantAirCenter = BWAPI::Position(airX / airScore, airY / airScore);
        }
        if (armyScore > rememberedArmyScore)
        {
            rememberedArmyScore = armyScore;
            rememberedArmyCenter = BWAPI::Position(armyX / armyScore, armyY / armyScore);
        }
    }

    if (importantAirScore >= 4 && importantAirCenter.isValid() &&
        engagementReadyForScan(importantAirCenter, true))
    {
        requestComsatScan(
            importantAirCenter,
            ComsatScanPriority::AirTracking,
            100,
            50,
            12,
            "air engagement");
    }

    if (rememberedArmyScore >= 12 && rememberedArmyCenter.isValid() &&
        engagementReadyForScan(rememberedArmyCenter, false))
    {
        requestComsatScan(
            rememberedArmyCenter,
            ComsatScanPriority::ArmyVision,
            100,
            50,
            12,
            "ground engagement");
    }
}

// Generate every automatic request, then resolve all automatic and external
// requests through one priority/energy gate.
void CombatCommander::doComsatScan()
{
    

    queueAutomaticComsatRequests();

    const bool emergencyPending = std::any_of(
        _comsatScanRequests.begin(),
        _comsatScanRequests.end(),
        [](const ComsatScanRequest& request)
        {
            return request.priority == ComsatScanPriority::EmergencyDetection;
        });

    if (!emergencyPending)
    {
        if (lateGameCleanupSweepNeeded())
        {
            (void)tryLateGameCleanupScan();
        }
        else
        {
            (void)tryMainBaseScoutScan();
        }
    }

    processComsatScanRequests();

    if (Config::Debug::DrawUnitTargets)
    {
        const int now = the.now();
        if (_lastComsatTarget.isValid() &&
            now - _lastComsatTargetFrame <= 12 * 24)
        {
            BWAPI::Broodwar->drawTextMap(
                _lastComsatTarget + BWAPI::Position(8, 12),
                "COMSAT %s",
                _lastComsatReason.c_str());
        }

        if (!_comsatScanRequests.empty())
        {
            const auto pending = std::max_element(
                _comsatScanRequests.begin(),
                _comsatScanRequests.end(),
                [](const ComsatScanRequest& a, const ComsatScanRequest& b)
                {
                    return int(a.priority) < int(b.priority);
                });
            BWAPI::Broodwar->drawCircleMap(
                pending->position, 48, BWAPI::Colors::Purple, false);
            BWAPI::Broodwar->drawTextMap(
                pending->position + BWAPI::Position(8, -12),
                "COMSAT target %s",
                pending->reason.c_str());
        }
    }
}

// Do the larva trick, if conditions are favorable.


// What units do you want to drop into the enemy base from a transport?
bool CombatCommander::unitIsGoodToDrop(const BWAPI::Unit unit) const
{
    if (isExpansionSupportVulture(unit))
    {
        return false;
    }

    return
        unit->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar ||
        unit->getType() == BWAPI::UnitTypes::Terran_Vulture;
}

// Called once per frame from update(), above.
// Get our money back at the last second for stuff that is about to be destroyed.
// It is not ideal: A building which is destined to die only after it is completed
// will be completed and die.
// Special case for a zerg sunken colony while it is morphing: It will lose up to
// 100 hp when the morph finishes, so cancel if it would be weak when it finishes.
// NOTE See BuildingManager::cancelBuilding() for another way to cancel buildings.
void CombatCommander::cancelDyingItems()
{
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        BWAPI::UnitType type = unit->getType();
        if (type.isBuilding() && !unit->isCompleted() ||
            type == BWAPI::UnitTypes::Zerg_Egg ||
            type == BWAPI::UnitTypes::Zerg_Lurker_Egg ||
            type == BWAPI::UnitTypes::Zerg_Cocoon)
        {
            if (UnitUtil::ExpectedSurvivalTime(unit) <= 1 * 24 ||
                type == BWAPI::UnitTypes::Zerg_Sunken_Colony && unit->getHitPoints() < 130 && unit->getRemainingBuildTime() < 24 && unit->isUnderAttack())
            {
                //DebugTacticalPrintf("canceling %s hp %d", UnitTypeName(unit).c_str(), unit->getHitPoints() + unit->getShields());
                (void) the.micro.Cancel(unit);
            }
        }
    }
}

// How good is it to pull this worker for combat?
int CombatCommander::workerPullScore(BWAPI::Unit worker)
{
    return
        (worker->getHitPoints() == worker->getType().maxHitPoints() ? 10 : 0) +
        (worker->getShields() == worker->getType().maxShields() ? 4 : 0) +
        (worker->isCarryingGas() ? -3 : 0) +
        (worker->isCarryingMinerals() ? -2 : 0);
}

// Pull workers off of mining and into the attack squad.
// The argument n can be zero or negative or huge. Nothing awful will happen.
// Tries to pull the "best" workers for combat, as decided by workerPullScore() above.
void CombatCommander::pullWorkers(int n)
{
    struct Compare
    {
        auto operator()(BWAPI::Unit left, BWAPI::Unit right) const -> bool
        {
            return workerPullScore(left) < workerPullScore(right);
        }
    };

    std::priority_queue<BWAPI::Unit, std::vector<BWAPI::Unit>, Compare> workers;

    Squad & groundSquad = _squadData.getSquad("Ground");

    for (BWAPI::Unit unit : _combatUnits)
    {
        if (unit->getType().isWorker() &&
            WorkerManager::Instance().isFree(unit) &&
            _squadData.canAssignUnitToSquad(unit, groundSquad))
        {
            workers.push(unit);
        }
    }

    int nLeft = n;

    while (nLeft > 0 && !workers.empty())
    {
        BWAPI::Unit worker = workers.top();
        workers.pop();
        _squadData.assignUnitToSquad(worker, groundSquad);
        --nLeft;
    }
}

// Release workers from the attack squad.
void CombatCommander::releaseWorkers()
{
    Squad & groundSquad = _squadData.getSquad("Ground");
    groundSquad.releaseWorkers();
}

void CombatCommander::drawSquadInformation(int x, int y)
{
    _squadData.drawSquadInformation(x, y);
}



namespace
{
    const int DefensiveBaseThreatRadius = 768;

    bool isRealBaseThreat(BWAPI::Unit enemy)
    {
        if (!enemy ||
            !enemy->exists() ||
            !enemy->isVisible() ||
            enemy->isInvincible() ||
            !enemy->getPosition().isValid())
        {
            return false;
        }

        const BWAPI::UnitType type = enemy->getType();

        if (type.isSpell())
        {
            return false;
        }

        if (type.isBuilding())
        {
            return
                type == BWAPI::UnitTypes::Terran_Bunker ||
                type == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
                type == BWAPI::UnitTypes::Zerg_Sunken_Colony ||
                type == BWAPI::UnitTypes::Zerg_Spore_Colony;
        }

        if (type.isWorker())
        {
            return true;
        }

        return
            UnitUtil::CanAttackGround(enemy) ||
            UnitUtil::CanAttackAir(enemy);
    }

    int countThreatsNearBase(Base* base)
    {
        if (!base || !base->getPosition().isValid())
        {
            return 0;
        }

        int threats = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!isRealBaseThreat(enemy))
            {
                continue;
            }

            if (enemy->getDistance(base->getPosition()) <= DefensiveBaseThreatRadius)
            {
                ++threats;
            }
        }

        return threats;
    }

    Base* findDefensiveBaseUnderAttack()
    {
        Base* mainBase = the.bases.myMain();

        // Main base has absolute priority.
        if (mainBase && countThreatsNearBase(mainBase) > 0)
        {
            return mainBase;
        }

        Base* bestBase = nullptr;
        int bestThreatCount = 0;
        int bestDistanceToMain = 999999999;

        BWAPI::Position mainPosition = mainBase
            ? mainBase->getPosition()
            : BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());

        for (Base* base : the.bases.getAll())
        {
            if (!base)
            {
                continue;
            }

            if (base == mainBase)
            {
                continue;
            }

            if (base->getOwner() != BWAPI::Broodwar->self())
            {
                continue;
            }

            if (!base->getDepot())
            {
                continue;
            }

            const int threats = countThreatsNearBase(base);

            if (threats <= 0)
            {
                continue;
            }

            const int distanceToMain =
                base->getPosition().getApproxDistance(mainPosition);

            if (threats > bestThreatCount ||
                threats == bestThreatCount && distanceToMain < bestDistanceToMain)
            {
                bestThreatCount = threats;
                bestDistanceToMain = distanceToMain;
                bestBase = base;
            }
        }

        return bestBase;
    }
}

// Create an attack order for the given squad (which may be null).
// For a squad with ground units, ignore targets which are not accessible by ground.
SquadOrder CombatCommander::getAttackOrder(
    Squad * squad,
    bool forceAggressive,
    bool forceDefensive)
{
    // 1. Clear any destructible obstacles around our bases.
    // Most maps don't have any such thing, but see e.g. Arkanoid and Sparkle.
    // Only ground squads are sent to clear obstacles.
    // TODO Disabled. There is a bug affecting the map Pathfinder.
    if (false && squad && squad->getUnits().size() > 0 && squad->hasGround() && squad->canAttackGround())
    {
        // We check our current bases (formerly plus 2 bases we may want to take next).
        /*
        Base * baseToClear1 = nullptr;
        Base * baseToClear2 = nullptr;
        BWAPI::TilePosition nextBasePos = the.map.getNextExpansion(false, true, false);
        if (nextBasePos.isValid())
        {
            // The next mineral-optional expansion.
            baseToClear1 = the.bases.getBaseAtTilePosition(nextBasePos);
        }
        nextBasePos = the.map.getNextExpansion(false, true, true);
        if (nextBasePos.isValid() && nextBasePos != baseToClear1)
        {
            // The next gas expansion.
            baseToClear2 = the.bases.getBaseAtTilePosition(nextBasePos);
        }
        */

        // Then pick any base with blockers and clear it.
        // The blockers are neutral buildings, and we can get their initial positions.
        for (Base * base : the.bases.getAll())
        {
            if (base->getBlockers().size() > 0 &&
                (base->getOwner() == the.self() /* || base == baseToClear1 || base == baseToClear2 */) &&
                squad->mapPartition() == the.partitions.id(base->getPosition()))
            {
                BWAPI::Unit target = *(base->getBlockers().begin());
                return SquadOrder(SquadOrderTypes::DestroyNeutral, target->getInitialPosition(), 64, false, "Destroy neutrals");
            }
        }
    }

    // A "ground squad" is a squad with any ground unit, so that it wants to use a ground distance map if possible.
    const bool isGroundSquad = squad->hasGround();

    // 2. If we're defensive, hold the front.
    // But if our base is attacked, defend it.
    // Priority:
    //   1) main base
    //   2) attacked expansion
    //   3) normal defensive front
    if (!forceAggressive && (forceDefensive || !_goAggressive))
    {
        Base* attackedBase = findDefensiveBaseUnderAttack();

        if (attackedBase)
        {
            if (Config::Debug::DrawSquadInfo)
            {
                BWAPI::Broodwar->drawTextScreen(
                    10,
                    200,
                    "GO DEFENSIVE: defending attacked base"
                );
            }

            return SquadOrder(
                SquadOrderTypes::Attack,
                attackedBase,
                DefendFrontRadius,
                isGroundSquad,
                "Defend attacked base"
            );
        }

        if (Config::Debug::DrawSquadInfo)
        {
            BWAPI::Broodwar->drawTextScreen(
                10,
                200,
                "GO DEFENSIVE: holding front"
            );
        }

        return SquadOrder(
            SquadOrderTypes::Attack,
            getDefensiveBase(),
            DefendFrontRadius,
            isGroundSquad,
            "Defend front"
        );
    }

    // 3. Otherwise we are aggressive. Look for a spot to attack.
    Base * base = nullptr;
    BWAPI::Position pos = BWAPI::Positions::Invalid;
    std::string key;

    // Attack target selection is strategic, not frame-critical. It may consult
    // ground-distance fields for enemy bases/buildings, so doing the complete
    // search for every squad on every frame wastes a large amount of combat
    // time once the army starts moving. Reuse the result for half a second.
    struct CachedAttackLocation
    {
        Base* base;
        BWAPI::Position position;
        std::string key;
        int frame;

        CachedAttackLocation()
            : base(nullptr)
            , position(BWAPI::Positions::Invalid)
            , frame(-100000)
        {
        }
    };

    static std::map<std::string, CachedAttackLocation> attackLocationCache;
    static int lastAttackLocationFrame = -1;
    const int now = the.now();
    if (lastAttackLocationFrame >= 0 && now < lastAttackLocationFrame)
    {
        attackLocationCache.clear();
    }
    lastAttackLocationFrame = now;

    const std::string cacheId = squad ? squad->getName() : std::string("<none>");
    auto cached = attackLocationCache.find(cacheId);
    if (cached != attackLocationCache.end() &&
        now - cached->second.frame < AttackLocationRefreshFrames)
    {
        base = cached->second.base;
        pos = cached->second.position;
        key = cached->second.key;
    }
    else
    {
        getAttackLocation(squad, base, pos, key);

        CachedAttackLocation& entry = attackLocationCache[cacheId];
        entry.base = base;
        entry.position = pos;
        entry.key = key;
        entry.frame = now;
    }

    SquadOrder order;
    if (base)
    {
        order = SquadOrder(SquadOrderTypes::Attack, base, AttackRadius, isGroundSquad, "Attack base");
    }
    else
    {
        // A late-game search can temporarily fail to produce a location
        // while enemy unit/base information is being removed. Never crash
        // the bot over a one-frame invalid target; return to the start and
        // choose a fresh target on the next update.
        if (!pos.isValid())
        {
            pos = BWAPI::Position(the.self()->getStartLocation());
            key = "fallback";
        }

        // A position-based ground order owns a full GridDistances field. The
        // attack squads receive their order every frame, so rebuilding the same
        // field here was the sustained slowdown that began when the army moved
        // toward a remembered building. Transfer the existing field when the
        // destination is unchanged; a new destination still gets a fresh map.
        GridDistances* reusableDistances = nullptr;
        if (isGroundSquad && squad)
        {
            SquadOrder& currentOrder = squad->getOrder();
            if (currentOrder.getType() == SquadOrderTypes::Attack &&
                currentOrder.getBase() == nullptr &&
                currentOrder.getPosition() == pos &&
                currentOrder.distances() != nullptr)
            {
                reusableDistances = currentOrder.releaseDistances();
            }
        }

        order = SquadOrder(
            SquadOrderTypes::Attack,
            pos,
            AttackRadius,
            isGroundSquad && reusableDistances == nullptr,
            "Attack " + key);

        if (reusableDistances)
        {
            order.setDistances(reusableDistances);
        }
    }
    order.setKey(key);
    return order;
}

// Choose a point of attack for the given squad (which may be null--no squad at all).
// For a squad with ground units, ignore targets which are not accessible by ground.
// Return either a base or a position, not both.
void CombatCommander::getAttackLocation(Squad * squad, Base * & returnBase, BWAPI::Position & returnPos, std::string & returnKey)
{
    // If the squad is empty, minimize effort.
    if (squad && squad->getUnits().empty())
    {
        returnBase = the.bases.myMain();
        returnKey = "nothing";
        return;
    }

    // Ground and air considerations.
    // NOTE The squad doesn't recalculate these values until it updates, which hasn't happened yet.
    //      So any changes we just made to the squad's units won't be reflected here yet. The slight
    //      delay doesn't cause any known problem (and changes that matter don't happen often).
    bool hasGround = true;
    bool hasAir = false;
    bool canAttackGround = true;
    bool canAttackAir = false;
    if (squad)
    {
        hasGround = squad->hasGround();
        hasAir = squad->hasAir();
        canAttackGround = squad->canAttackGround();
        canAttackAir = squad->canAttackAir();
    }

    // If the squad has no combat units, or is unable to attack, return home.
    if (!hasGround && !hasAir || !canAttackGround && !canAttackAir)
    {
        returnBase = the.bases.myMain();
        returnKey = "nothing";
        return;
    }

    // Know where the squad is. If it's empty or unknown, assume it is at our start position.
    // NOTE In principle, different members of the squad may be in different map partitions,
    //      unable to reach each others' positions by ground. We ignore that complication.
    // NOTE Since we aren't doing islands, all squads are reachable from the start position,
    //      except in the case of accidentally pushing units through a barrier.
    int squadPartition = the.partitions.id(the.bases.myStart()->getTilePosition());
    BWAPI::Position groundReferencePosition = BWAPI::Positions::Invalid;

    // Use the actual ground army position, not our starting location. This matters
    // after transports/barriers and also keeps attack target selection consistent
    // with the route the current squad can really use. For an all-air squad the
    // value is irrelevant, so only ground units are considered here.
    if (squad && hasGround)
    {
        BWAPI::Unit vanguard = squad->getVanguard();
        if (vanguard &&
            vanguard->exists() &&
            !vanguard->isFlying() &&
            vanguard->getPosition().isValid())
        {
            groundReferencePosition = vanguard->getPosition();
            squadPartition = the.partitions.id(groundReferencePosition);
        }
        else
        {
            for (BWAPI::Unit unit : squad->getUnits())
            {
                if (unit &&
                    unit->exists() &&
                    !unit->isFlying() &&
                    unit->getPosition().isValid())
                {
                    groundReferencePosition = unit->getPosition();
                    squadPartition = the.partitions.id(groundReferencePosition);
                    break;
                }
            }
        }
    }

    // 1. If we haven't been able to attack, look for an undefended target on the ground, either
    // a building or a ground unit that can't shoot up.
    // Only if the squad is all air. This is mainly for mutalisks.
	// NOTE Turned off. It does not improve play.
    if (false &&
		squad &&
        !hasGround &&
        canAttackGround &&
        squad->getVanguard() &&
        the.now() - squad->getLastAttack() > 2 * 24 &&		// no cluster has attacked in this time
		the.now() - squad->getLastRetreat() <= 8 &&			// at least one cluster has retreated in this short time
        the.now() - squad->getOrderFrame() > 3 * 24)		// the order was long enough ago for an attack to have happened
    {
        int bestScore = -MAX_DISTANCE;      // higher is better
        BWAPI::Position target = BWAPI::Positions::None;

        for (const auto & kv : the.info.getUnitInfo(the.enemy()))
        {
            const UnitInfo & ui = kv.second;

            if (!UnitUtil::TypeCanAttackAir(ui.type) &&
                ui.lastPosition.isValid() &&
                !ui.goneFromLastPosition &&
				(canAttackAir || !ui.lifted) &&		// guardians can't hit a lifted barracks
                !defendedTarget(ui.lastPosition, false, true))
            {
				// Higher score is better.
                int score = -squad->getVanguard()->getDistance(ui.lastPosition);		// far away is bad

				// Extra score for valuable units.
				if (ui.type.isResourceDepot())
				{
					score += 16 * 32;
				}
                else if (ui.type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
                {
                    score += 10 * 32;
                }
                else if (ui.type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                         ui.type == BWAPI::UnitTypes::Protoss_High_Templar ||
                         ui.type == BWAPI::UnitTypes::Protoss_Reaver ||
                         ui.type == BWAPI::UnitTypes::Zerg_Lurker ||
					     ui.type == BWAPI::UnitTypes::Zerg_Defiler)
                {
                    score += 6 * 32;
                }

                if (score > bestScore)
                {
                    bestScore = score;
                    target = ui.lastPosition;
					//DebugTacticalPrintf("    %s: candidate undefended %s @ %d,%d", squad->getName().c_str(), ui.type.getName().c_str(), target.x, target.y);
				}
            }
        }

        if (target.isValid())
        {
            //DebugTacticalPrintf("  %s: undefended target @ %d,%d", squad->getName().c_str(), target.x, target.y);

            returnPos = target;
            returnKey = "undefended";
            return;
        }
    }

    // Finish a base before switching to the next one. The base loses its enemy
    // owner as soon as its depot dies, although other buildings can remain
    // behind the depot (and may not have been in sight yet).
    struct LocalBaseCleanup
    {
        Base* base = nullptr;
        int started = -1;
        int targetIndex = -1;
        int targetFrame = -1;
        unsigned int skipped = 0;
    };
    static std::map<std::string, LocalBaseCleanup> localCleanup;
    static int lastCleanupFrame = -1;
    if (lastCleanupFrame >= 0 && the.now() < lastCleanupFrame)
    {
        localCleanup.clear();
    }
    lastCleanupFrame = the.now();

    LocalBaseCleanup* cleanup = nullptr;
    if (squad && hasGround && canAttackGround && groundReferencePosition.isValid())
    {
        LocalBaseCleanup& state = localCleanup[squad->getName()];
        Base* previousBase = squad->getOrder().getBase();
        if (squad->getOrder().getStatus() == "Attack base" &&
            previousBase && previousBase->getOwner() != the.enemy() &&
            previousBase->getOwner() != the.self() &&
            groundReferencePosition.getApproxDistance(previousBase->getCenter()) <= 20 * 32)
        {
            state = LocalBaseCleanup();
            state.base = previousBase;
            state.started = the.now();
        }
        if (state.base &&
            (state.base->getOwner() == the.self() ||
             the.now() - state.started > 60 * 24))
        {
            state = LocalBaseCleanup();
        }
        if (state.base)
        {
            cleanup = &state;
        }
    }

    // A known building near the army must not lose priority to a distant
    // enemy-owned base. Check only a few nearby candidates by ground route so
    // target selection remains cheap even in a very large late-game army.
    if (hasGround && canAttackGround && groundReferencePosition.isValid())
    {
        std::vector<std::pair<int, BWAPI::Position>> localBuildings;
        for (const auto& kv : the.info.getUnitInfo(the.enemy()))
        {
            const UnitInfo& ui = kv.second;
            if (!ui.type.isBuilding() || ui.type.isAddon() ||
                ui.type.isResourceDepot() || ui.lifted ||
                ui.goneFromLastPosition || !ui.lastPosition.isValid())
            {
                continue;
            }
            if (cleanup &&
                ui.lastPosition.getApproxDistance(cleanup->base->getCenter()) > 24 * 32)
            {
                continue;
            }
            const int distance = groundReferencePosition.getApproxDistance(ui.lastPosition);
            if (distance <= (cleanup ? 26 : 18) * 32 &&
                the.partitions.id(ui.lastPosition) == squadPartition)
            {
                localBuildings.emplace_back(distance, ui.lastPosition);
            }
        }
        std::sort(localBuildings.begin(), localBuildings.end());
        for (size_t i = 0; i < localBuildings.size() && i < 8; ++i)
        {
            const int routeDistance = the.map.getGroundDistance(
                groundReferencePosition, localBuildings[i].second);
            if (routeDistance >= 0 &&
                routeDistance <= (cleanup ? 36 : 22) * 32)
            {
                returnPos = localBuildings[i].second;
                returnKey = "local building";
                return;
            }
        }
    }

    if (cleanup)
    {
        // Inspect fogged tiles around the destroyed depot once. This catches
        // production hidden behind it without sending the army on an endless
        // search of the same base. A timed-out or unreachable point is skipped.
        const BWAPI::Position center = cleanup->base->getCenter();
        const int dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        const int dy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
        if (cleanup->targetIndex >= 0)
        {
            const int radius = (cleanup->targetIndex / 8 == 0 ? 9 :
                cleanup->targetIndex / 8 == 1 ? 16 : 22) * 32;
            const int direction = cleanup->targetIndex % 8;
            const BWAPI::Position target(
                center.x + dx[direction] * radius,
                center.y + dy[direction] * radius);
            if (the.now() - cleanup->targetFrame >= 8 * 24 ||
                (target.isValid() && BWAPI::Broodwar->isVisible(BWAPI::TilePosition(target))))
            {
                cleanup->skipped |= 1u << cleanup->targetIndex;
                cleanup->targetIndex = -1;
            }
        }

        if (cleanup->targetIndex < 0)
        {
            int bestIndex = -1;
            int bestDistance = MAX_DISTANCE;
            for (int i = 0; i < 24; ++i)
            {
                if (cleanup->skipped & (1u << i))
                {
                    continue;
                }
                const int radius = (i / 8 == 0 ? 9 : i / 8 == 1 ? 16 : 22) * 32;
                const int direction = i % 8;
                const BWAPI::Position candidate(
                    center.x + dx[direction] * radius,
                    center.y + dy[direction] * radius);
                const BWAPI::TilePosition tile(candidate);
                if (!candidate.isValid() || !tile.isValid() ||
                    tile.x >= BWAPI::Broodwar->mapWidth() ||
                    tile.y >= BWAPI::Broodwar->mapHeight() ||
                    !the.map.isWalkable(tile) ||
                    the.partitions.id(tile) != squadPartition ||
                    BWAPI::Broodwar->isVisible(tile))
                {
                    cleanup->skipped |= 1u << i;
                    continue;
                }
                const int distance = groundReferencePosition.getApproxDistance(candidate);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestIndex = i;
                }
            }
            if (bestIndex >= 0)
            {
                cleanup->targetIndex = bestIndex;
                cleanup->targetFrame = the.now();
            }
        }
        if (cleanup->targetIndex >= 0)
        {
            const int radius = (cleanup->targetIndex / 8 == 0 ? 9 :
                cleanup->targetIndex / 8 == 1 ? 16 : 22) * 32;
            const int direction = cleanup->targetIndex % 8;
            const BWAPI::Position target(
                center.x + dx[direction] * radius,
                center.y + dy[direction] * radius);
            const int routeDistance = the.map.getGroundDistance(
                groundReferencePosition, target);
            if (routeDistance >= 0 && routeDistance <= 42 * 32)
            {
                returnPos = target;
                returnKey = "clear former base";
                return;
            }
            cleanup->skipped |= 1u << cleanup->targetIndex;
            cleanup->targetIndex = -1;
        }
        *cleanup = LocalBaseCleanup();
    }

    // 2. Attack the enemy base with the weakest defense.
	// An "enemy" base is one that either the enemy owns, or has multiple enemy
	// buildings other than static defense.
	// Only if the squad can attack ground.
    if (canAttackGround)
    {
        Base * targetBase = nullptr;
        int bestScore = -MAX_DISTANCE;
        for (Base * base : the.bases.getAll())
        {
			const bool enemyOwned = base->getOwner() == the.enemy();
			// The disabled building-count heuristic below means neutral and friendly
			// bases cannot be selected. Reject them before asking MapTools for a
			// full distance field; the old order built one field for every base on
			// the map at the exact frame the army received its first attack order.
			if (!enemyOwned)
			{
				continue;
			}

			// Ground squads ignore enemy bases which they cannot reach.
			if (hasGround && squadPartition != the.partitions.id(base->getTilePosition()))
			{
				continue;
			}

            // For a ground army, geometric/air distance is a bad measure of
            // which enemy base is actually close. A base across a cliff can be
            // only a few pixels away but require a long route around the map.
            // Measure from the current ground vanguard along walkable terrain
            // and reject bases which have no ground route at all.
            int groundRouteDistance = -1;
            if (hasGround && groundReferencePosition.isValid())
            {
                groundRouteDistance = the.map.getGroundDistance(
                    groundReferencePosition,
                    base->getCenter());

                if (groundRouteDistance < 0)
                {
                    continue;
                }
            }

			// TODO The "attack base buildings" feature needs more work.
			if (enemyOwned /* || nEnemyBuildings >= 3 */)
            {
				// Higher is better. The score is normally negative.
                int score = 0;

				// A base owned by the enemy is higher priority.
				if (enemyOwned)
				{
					score += 4;
				}

				// A base with an undefended mineral line is higher priority.
				if (hasGround && the.bases.myMain()->getSafeTileDistance(base->getTilePosition()) >= 0 ||
					hasAir && the.bases.myMain()->getAirSafeTileDistance(base->getTilePosition()) >= 0)
				{
					score += 2;
				}

				// A base with many enemy buildings has higher priority.
				int nEnemyBuildings = base->getNumEnemyNondefenseBuildings();
				score += nEnemyBuildings / 8;

				// A base with low remaining minerals is lower priority (even if they don't own it).
				if (base->getLastKnownMinerals() < 300)
				{
					score -= 2;
				}

				// Ground squads mildly prefer to attack a base other than the enemy main. It's a simple heuristic.
                // Air squads prefer to go for the main first.
                if (base == the.bases.enemyStart())
                {
                    score += hasGround ? -1 : 2;
                }

                // The squad vanguard is the unit closest to the current attack location.
                // Prefer a new attack location which is close, preferably even closer.
                if (squad && squad->getVanguard())
                {
                    const int routeDistance =
                        hasGround && groundRouteDistance >= 0
                        ? groundRouteDistance
                        : squad->getVanguard()->getDistance(base->getCenter());

                    score -= routeDistance / (16 * 32);
                }

                std::vector<UnitInfo> enemies;
                int enemyDefenseRange = the.info.enemyHasSiegeMode() ? 12 * 32 : 8 * 32;
                the.info.getNearbyForce(enemies, base->getCenter(), the.enemy(), enemyDefenseRange);
                for (const UnitInfo & enemy : enemies)
                {
                    // Count enemies that are buildings or slow-moving units good for defense.
                    if (enemy.type.isBuilding() ||
                        enemy.type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                        enemy.type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
						enemy.type == BWAPI::UnitTypes::Protoss_Arbiter ||
						enemy.type == BWAPI::UnitTypes::Protoss_Carrier ||
                        enemy.type == BWAPI::UnitTypes::Protoss_High_Templar ||
						enemy.type == BWAPI::UnitTypes::Protoss_Reaver ||
						enemy.type == BWAPI::UnitTypes::Zerg_Lurker ||
                        enemy.type == BWAPI::UnitTypes::Zerg_Guardian)
                    {
                        // If the enemy unit could attack (some units of) the squad, count it.
                        if (hasGround && UnitUtil::TypeCanAttackGround(enemy.type) ||			// doesn't recognize casters
                            hasAir && UnitUtil::TypeCanAttackAir(enemy.type) ||					// doesn't recognize casters
                            enemy.type == BWAPI::UnitTypes::Protoss_High_Templar)				// spellcaster
                        {
                            --score;
                        }
                    }
                }
                if (score > bestScore)
                {
                    targetBase = base;
                    bestScore = score;
                }
            }
        }
        if (targetBase)
        {
            returnBase = targetBase;
            returnKey = "base";
            return;
        }
    }

    const BWAPI::Position groundAttackReference =
        groundReferencePosition.isValid()
        ? groundReferencePosition
        : the.bases.myStart()->getPosition();
    int groundAirRange = 0;
    if (squad && hasGround)
    {
        for (BWAPI::Unit member : squad->getUnits())
        {
            if (!member || !member->exists() || member->isFlying())
            {
                continue;
            }
            int range = member->getType().airWeapon().maxRange();
            if (member->getType() == BWAPI::UnitTypes::Terran_Goliath &&
                the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) > 0)
            {
                range += 3 * 32;
            }
            groundAirRange = std::max(groundAirRange, range);
        }
    }

    // 3. Attack known enemy buildings.
    // A mixed AntiAir squad contains ground Goliaths and a flying Vessel. The
    // Vessel must not make a lifted building over a void a ground move goal.
    // Clear reachable grounded buildings first, then approach a lifted one
    // only at a walkable position from which a ground unit can fire at it.
    if (canAttackGround || the.enemyRace() == BWAPI::Races::Terran)
    {
        const BWAPI::Position buildingReference =
            hasGround
            ? groundAttackReference
            : squad && squad->getVanguard() && squad->getVanguard()->getPosition().isValid()
            ? squad->getVanguard()->getPosition()
            : squad && squad->calcCenter().isValid()
                ? squad->calcCenter()
                : the.bases.myStart()->getPosition();

        BWAPI::Position bestGroundBuilding = BWAPI::Positions::Invalid;
        BWAPI::Position bestAirApproach = BWAPI::Positions::Invalid;
        BWAPI::Position bestLiftedSearch = BWAPI::Positions::Invalid;
        BWAPI::Position lastLiftedMemory = BWAPI::Positions::Invalid;
        int bestLiftedSearchFrame = -1;
        int bestGroundDistance = MAX_DISTANCE;
        int bestAirDistance = MAX_DISTANCE;

        for (const auto & kv : the.info.getUnitInfo(the.enemy()))
        {
            const UnitInfo & ui = kv.second;

            if (!ui.type.isBuilding() || ui.type.isAddon() ||
                !ui.lastPosition.isValid())
            {
                continue;
            }

            if (ui.lifted && ui.goneFromLastPosition &&
                hasGround && canAttackAir)
            {
                // The lifted building still exists; only its old coordinates
                // were disproved. Search the last enemy area before the army
                // falls back to an unrelated map-wide cleanup cell.
                if (ui.updateFrame > bestLiftedSearchFrame)
                {
                    lastLiftedMemory = ui.lastPosition;
                    bestLiftedSearchFrame = ui.updateFrame;
                }
                continue;
            }
            if (ui.goneFromLastPosition)
            {
                continue;
            }

            if (ui.lifted)
            {
                if (!canAttackAir)
                {
                    continue;
                }
                const BWAPI::Position approach =
                    hasGround
                    ? groundAntiAirApproach(
                        ui.lastPosition, buildingReference,
                        squadPartition, groundAirRange)
                    : ui.lastPosition;
                if (!approach.isValid())
                {
                    continue;
                }
                const int distance = buildingReference.getApproxDistance(approach);
                if (distance < bestAirDistance)
                {
                    bestAirDistance = distance;
                    bestAirApproach = approach;
                }
            }
            else
            {
                if (!canAttackGround)
                {
                    continue;
                }

                int distance = MAX_DISTANCE;
                if (hasGround)
                {
                    distance =
                        the.map.getGroundDistance(
                            buildingReference,
                            ui.lastPosition);

                    if (distance < 0)
                    {
                        continue;
                    }
                }
                else
                {
                    distance = buildingReference.getApproxDistance(ui.lastPosition);
                }
                if (distance < bestGroundDistance)
                {
                    bestGroundDistance = distance;
                    bestGroundBuilding = ui.lastPosition;
                }
            }
        }

        if (bestGroundBuilding.isValid())
        {
            returnPos = bestGroundBuilding;
            returnKey = "nearest building";
            return;
        }
        if (bestAirApproach.isValid())
        {
            returnPos = bestAirApproach;
            returnKey = "air target approach";
            return;
        }
        if (lastLiftedMemory.isValid())
        {
            const BWAPI::Position anchor = groundAntiAirApproach(
                lastLiftedMemory, buildingReference, squadPartition,
                12 * 32);
            if (anchor.isValid())
            {
                const BWAPI::Position search =
                    MapGrid::Instance().getLeastExploredNear(
                        anchor, true, false, 14 * 32);
                if (search.isValid() &&
                    MapGrid::Instance().getCell(search).timeLastVisited <
                        std::max(bestLiftedSearchFrame, the.now() - 90 * 24))
                {
                    bestLiftedSearch = search;
                }
            }
        }
        if (bestLiftedSearch.isValid())
        {
            returnPos = bestLiftedSearch;
            returnKey = "lifted search";
            return;
        }
    }

    // 4. Attack visible enemy units.
    const BWAPI::Position squadCenter = squad
        ? squad->calcCenter()
        : the.bases.myStart()->getPosition();
    BWAPI::Unit bestUnit = nullptr;
    BWAPI::Position bestUnitGoal = BWAPI::Positions::Invalid;
    int bestDistance = MAX_DISTANCE;
    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Larva ||
            unit->getType().isBuilding() ||
            !unit->isDetected() ||
            unit->isInvincible() ||
            unit->getType().isSpell())
        {
            continue;
        }

        BWAPI::Position unitGoal = unit->getPosition();
        if (hasGround && unit->isFlying())
        {
            unitGoal = groundAntiAirApproach(
                unit->getPosition(), groundAttackReference,
                squadPartition, groundAirRange);
            if (!unitGoal.isValid())
            {
                continue;
            }
        }

        // A nearby enemy across a barrier may still be in weapon range.
        const BWAPI::Position targetReference = hasGround
            ? groundAttackReference
            : squad && squad->getVanguard()
                ? squad->getVanguard()->getPosition()
                : squadCenter;
        const int distance = unitGoal.getApproxDistance(targetReference);
        if (hasGround &&
            !unit->isFlying() &&
            squadPartition != the.partitions.id(unitGoal) &&
            distance > 300)
        {
            continue;
        }

        if (unit->isFlying() && canAttackAir || !unit->isFlying() && canAttackGround)
        {
            if (distance < bestDistance)
            {
                bestUnit = unit;
                bestUnitGoal = unitGoal;
                bestDistance = distance;
            }
        }
    }
    if (bestUnit)
    {
        returnPos = bestUnitGoal;
        returnKey = bestUnit->isFlying() ? "air target" : "unit";
        return;
    }

    // 5. Attack the remembered locations of unseen enemy units which might still be there.
    // Choose the one most recently seen.
    int lastSeenFrame = 0;
    BWAPI::Position lastSeenPos = BWAPI::Positions::None;
    BWAPI::UnitType lastSeenType = BWAPI::UnitTypes::None;      // for debugging only
    bool lastSeenTargetIsFlying = false;
    for (const auto & kv : the.info.getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo & ui(kv.second);

        // A remembered aircraft position is not a valid ground destination.
        // Contact micro handles air units when they are actually visible.
        if (hasGround && (ui.type.isFlyer() || ui.lifted))
        {
            continue;
        }

        if (ui.updateFrame < the.now() &&
            ui.updateFrame > lastSeenFrame &&
            !ui.goneFromLastPosition &&
            !ui.type.isSpell() && !ui.type.isBuilding() &&
            (!hasGround || the.partitions.id(ui.lastPosition) == squadPartition) &&
            ((ui.type.isFlyer() || ui.lifted) && canAttackAir || (!ui.type.isFlyer() || !ui.lifted) && canAttackGround))
        {
            lastSeenFrame = ui.updateFrame;
            lastSeenPos = ui.lastPosition;
            lastSeenType = ui.type;
            lastSeenTargetIsFlying = ui.type.isFlyer() || ui.lifted;
        }
    }
    if (lastSeenPos.isValid())
    {
        returnPos = lastSeenPos;
        returnKey = lastSeenTargetIsFlying
            ? "air target"
            : "possible unit";
        return;
    }

    // 6. We can't see anything, so explore the map until we find something.
    // When all known enemy depots are gone, spread squads across several old
    // cells instead of sending the whole army to the same location.
    if (lateGameCleanupSweepNeeded())
    {
        int sweepLane = 0;
        if (squad)
        {
            for (char ch : squad->getName())
            {
                sweepLane = (sweepLane * 33 + int((unsigned char)ch)) & 0x7fffffff;
            }
        }

        returnPos = MapGrid::Instance().getLeastExploredForSweep(
            hasGround,
            squadPartition,
            sweepLane);
        returnKey = "cleanup sweep";
        return;
    }

    returnPos = MapGrid::Instance().getLeastExplored(hasGround, squadPartition);
    returnKey = "explore";
}

// Does the enemy have defenders (static or mobile) near the given position?
bool CombatCommander::defendedTarget(const BWAPI::Position & pos, bool vsGround, bool vsAir) const
{
	const int ShortTime = 4 * 24;		// time in frames

	if (vsGround)
	{
		if (the.groundHits(BWAPI::TilePosition(pos)) > 0)
		{
			return true;
		}

		const std::vector<UnitCluster> & antigroundClusters = the.ops.getGroundDefenseClusters();
		for (const UnitCluster & cluster : antigroundClusters)
		{
			int dist = pos.getApproxDistance(cluster.center);

			// The enemy defenders are "too close" if they can get within 8 tiles in a short time.
			if (dist <= 8 * 32 + ShortTime * cluster.speed)
			{
				return true;
			}
		}
	}

	if (vsAir)
    {
        if (the.airHits(BWAPI::TilePosition(pos)) > 0)
        {
            return true;
        }

        const std::vector<UnitCluster> & antiairClusters = the.ops.getAirDefenseClusters();
        for (const UnitCluster & cluster : antiairClusters)
        {
            int dist = pos.getApproxDistance(cluster.center);

            // The enemy defenders are "too close" if they can get within 8 tiles in a short time.
            if (dist <= 8 * 32 + ShortTime * cluster.speed)
            {
                return true;
            }
        }
    }

    return false;
}

// Choose a point of attack for the given drop squad.
BWAPI::Position CombatCommander::getDropLocation(const Squad &)
{
    // 0. If we're defensive, stay at the start location.
    /* unneeded
    if (!_goAggressive)
    {
        return BWAPI::Position(the.self()->getStartLocation());
    }
    */

    // Otherwise we are aggressive. Look for a spot to attack.

    // 1. The enemy main base, if known.
    Base * enemyMain = the.bases.enemyStart();
    // if (enemyMain && enemyMain->getOwner() == the.enemy())
    if (enemyMain)
    {
        return enemyMain->getPosition();
    }

    // 2. Any known enemy base.
    /* TODO not ready yet
    Base * targetBase = nullptr;
    int bestScore = INT_MIN;
    for (Base * base : the.bases.getBases())
    {
        if (base->getOwner() == the.enemy())
        {
            int score = 0;     // the final score will be 0 or negative
            std::vector<UnitInfo> enemies;
            the.info.getNearbyForce(enemies, base->getCenter(), the.enemy(), 600);
            for (const auto & enemy : enemies)
            {
                if (enemy.type.isBuilding() && (UnitUtil::TypeCanAttackGround(enemy.type) || enemy.type.isDetector()))
                {
                    --score;
                }
            }
            if (score > bestScore)
            {
                targetBase = base;
                bestScore = score;
            }
        }
        if (targetBase)
        {
            return targetBase->getCenter();
        }
    }
    */

    // 3. Any known enemy buildings.
    for (const auto & kv : the.info.getUnitInfo(the.enemy()))
    {
        const UnitInfo & ui = kv.second;

        if (ui.type.isBuilding() && ui.lastPosition.isValid() && !ui.goneFromLastPosition)
        {
            return ui.lastPosition;
        }
    }

    // 4. We can't see anything, so explore the map until we find something.
    return MapGrid::Instance().getLeastExplored();
}

// We're being defensive. Get the location to defend.
Base * CombatCommander::getDefensiveBase()
{
    // We are guaranteed to always have a main base location, even if it has been destroyed.
    Base * base = the.bases.myMain();

    // We may have taken our natural. If so, call that the front line.
    Base * natural = the.bases.myNatural();
    if (natural && the.self() == natural->getOwner() &&
        ProductionManager::Instance().terranNaturalArmyStagingReady())
    {
        base = natural;
    }

    return base;
}

// Choose one worker to pull for scout defense.
BWAPI::Unit CombatCommander::findClosestWorkerToTarget(BWAPI::Unitset & unitsToAssign, BWAPI::Unit target)
{
    UAB_ASSERT(target != nullptr, "target was null");

    if (!target)
    {
        return nullptr;
    }

    BWAPI::Unit closestMineralWorker = nullptr;
    int closestDist = Config::Micro::ScoutDefenseRadius + 128;    // more distant workers do not get pulled
    
    for (BWAPI::Unit unit : unitsToAssign)
    {
        if (unit->getType().isWorker() && WorkerManager::Instance().isFree(unit))
        {
            int dist = unit->getDistance(target);
            if (unit->isCarryingMinerals())
            {
                dist += 96;
            }

            if (dist < closestDist)
            {
                closestMineralWorker = unit;
                closestDist = dist;
            }
        }
    }

    return closestMineralWorker;
}

int CombatCommander::numZerglingsInOurBase() const
{
    const int concernRadius = 300;
    int zerglings = 0;
    
    BWAPI::Position myBasePosition(the.bases.myStart()->getPosition());

    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Zergling &&
            unit->getDistance(myBasePosition) < concernRadius)
        {
            ++zerglings;
        }
    }

    return zerglings;
}

// Is an enemy building near our base? If so, we may pull workers.
// Don't pull against a completed bunker, cannon, or sunken; we'll only lose workers.
bool CombatCommander::buildingRush() const
{
    // If we have units, there will likely be no gain in pulling workers.
    if (the.info.weHaveCombatUnits())
    {
        return false;
    }

    BWAPI::Position myBasePosition(the.bases.myStart()->getPosition());

    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->getType().isBuilding() &&
            unit->getDistance(myBasePosition) < 600 &&
            !unit->isLifted() &&
            (!unit->isCompleted() || unit->getType().groundWeapon() == BWAPI::WeaponTypes::None))
        {
            return true;
        }
    }

    return false;
}

CombatCommander & CombatCommander::Instance()
{
    static CombatCommander instance;
    return instance;
}
