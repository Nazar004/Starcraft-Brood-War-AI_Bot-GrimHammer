#include "Economy/WorkerManager.h"

#include "Map/Bases.h"
#include "Construction/BuildingManager.h"
#include "Construction/BuildingPlacer.h"
#include "Combat/CombatCommander.h"
#include "Production/MacroAct.h"
#include "Map/MapTools.h"
#include "Combat/Micro/Micro.h"
#include "Production/ProductionManager.h"
#include "Intelligence/ScoutManager.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"

#include <algorithm>
#include <map>
#include <vector>

using namespace UAlbertaBot;

namespace
{
    // Workers which have no usable owned mineral line can still perform
    // long-distance mining from a neutral base. Keep this state outside
    // WorkerData::Minerals because that job assumes a real resource depot at
    // the mineral line.
    struct RemoteMineralState
    {
        BWAPI::Unit patch = nullptr;
        int lastCommandFrame = -100000;
    };

    std::map<BWAPI::Unit, RemoteMineralState> remoteMineralWorkers;

    int remoteWorkersOnPatch(BWAPI::Unit patch)
    {
        int count = 0;
        for (const auto& entry : remoteMineralWorkers)
        {
            if (entry.second.patch == patch)
            {
                ++count;
            }
        }
        return count;
    }
}

WorkerManager::WorkerManager() 
    : previousClosestWorker(nullptr)
    , _collectGas(true)
    , _nextMobileRepairFrame(0)
    , _lastMobileRepairObservedFrame(-1)
    , _ferryTransport(nullptr)
    , _ferryDepot(nullptr)
    , _ferryPhase(0)
    , _ferryStartFrame(-1)
    , _ferryLastCommandFrame(-100000)
    , _ferryCooldownFrame(-100000)
{
}

WorkerManager & WorkerManager::Instance() 
{
    static WorkerManager instance;
    return instance;
}

bool WorkerManager::isBusy(BWAPI::Unit worker) const
{
    return busy.contains(worker);
}

void WorkerManager::makeBusy(BWAPI::Unit worker)
{
    busy.insert(worker);
}

void WorkerManager::update()
{
    UAB_ASSERT(busy.empty(), "unexpected busy workers");

    // NOTE Combat workers are placed in a combat squad and get their orders there.
    //      We ignore them here.
    updateWorkerStatus();
    updateExpansionFerry();
    clearBlockingMinerals();

    // A new expansion should not have to wait for freshly-produced workers to
    // become useful.  The old rebalanceWorkers() implementation only marked an
    // oversaturated worker Idle; handleIdleWorkers() would then normally send
    // it straight back to the same (nearest) depot.  Rebalance periodically so
    // saturated mineral lines explicitly transfer workers to newer, emptier
    // bases instead.  One transfer per second is deliberately gentle: it
    // improves saturation without producing a burst of cross-map SCV orders.
    if (the.selfRace() == BWAPI::Races::Terran &&
        the.bases.completedBaseCount(the.self()) >= 2 &&
        the.now() % 24 == 7)
    {
        rebalanceWorkers();
    }

    handleGasWorkers();
    handleIdleWorkers();
    handleReturnCargoWorkers();
    handleRepairWorkers();
    handleMineralWorkers();
    handleUnblockWorkers();
    handlePostedWorkers();

    drawWorkerInformation();
    workerData.drawDepotDebugInfo();

    // Workers are not busy except while WorkerManager::update() is running, so that
    // other modules can steal workers (which can't be stolen when busy).
    busy.clear();
}

bool WorkerManager::expansionFerryNeededAt(const Base* base) const
{
    Base* main = the.bases.myMain();
    if (the.selfRace() != BWAPI::Races::Terran || !main || !base ||
        base == main || base->getOwner() != the.self() ||
        base->inWorkerDanger())
    {
        return false;
    }

    BWAPI::Unit depot = base->getDepot();
    if (!depot || !depot->exists() || !depot->isCompleted() ||
        depot->isFlying() ||
        depot->getType() != BWAPI::UnitTypes::Terran_Command_Center ||
        workerData.getNumAssignedWorkers(depot) >= 2 ||
        !main->getGroundSafePaths().isInitialized() ||
        (main->getSafeTileDistance(base->getTilePosition()) >= 0 &&
         !BuildingManager::Instance().isAirRelocatedCommandCenter(depot)))
    {
        return false;
    }

    // A closed wall alone can invalidate the safe grid. Require actual enemy
    // pressure at our exit before replacing the normal ground transfer.
    const BWAPI::Position exit = the.bases.myNatural()
        ? the.bases.myNatural()->getPosition() : main->getFront();
    if (!exit.isValid())
    {
        return false;
    }
    for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
        exit, 12 * 32, BWAPI::Filter::IsEnemy))
    {
        if (enemy && enemy->exists() && enemy->isVisible() &&
            UnitUtil::CanAttackGround(enemy))
        {
            return true;
        }
    }
    for (const auto& entry : the.info.getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo& enemy = entry.second;
        if (!enemy.goneFromLastPosition && enemy.lastPosition.isValid() &&
            enemy.lastPosition.getApproxDistance(exit) <= 12 * 32 &&
            enemy.updateFrame <= the.now() &&
            the.now() - enemy.updateFrame <= 15 * 24 &&
            UnitUtil::TypeCanAttackGround(enemy.type))
        {
            return true;
        }
    }
    return false;
}

bool WorkerManager::needsExpansionFerry() const
{
    for (const Base* base : the.bases.getAll())
    {
        if (expansionFerryNeededAt(base))
        {
            return true;
        }
    }
    return false;
}

bool WorkerManager::isExpansionFerryWorker(BWAPI::Unit worker) const
{
    return worker && _ferryWorkers.contains(worker);
}

void WorkerManager::updateExpansionFerry()
{
    const int now = the.now();
    if (_ferryStartFrame > now)
    {
        _ferryTransport = nullptr;
        _ferryDepot = nullptr;
        _ferryWorkers.clear();
        _ferryPhase = 0;
        _ferryCooldownFrame = -100000;
    }

    auto finish = [&](bool delivered)
    {
        for (BWAPI::Unit worker : _ferryWorkers)
        {
            if (worker && worker->exists() && !worker->isLoaded() &&
                workerData.getWorkerJob(worker) == WorkerData::Ferry)
            {
                if (delivered && _ferryDepot && _ferryDepot->exists() &&
                    worker->getDistance(_ferryDepot) <= 10 * 32)
                {
                    workerData.setWorkerJob(worker, WorkerData::Minerals,
                        _ferryDepot);
                }
                else
                {
                    workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
                }
            }
        }
        _ferryWorkers.clear();
        if (_ferryTransport)
        {
            CombatCommander::Instance().releaseExpansionFerryDropship(
                _ferryTransport);
        }
        _ferryTransport = nullptr;
        _ferryDepot = nullptr;
        _ferryPhase = 0;
        _ferryCooldownFrame = now + (delivered ? 12 * 24 : 5 * 24);
    };

    if (_ferryPhase == 0)
    {
        if (now < _ferryCooldownFrame || now % 24 != 7)
        {
            return;
        }
        Base* target = nullptr;
        for (Base* base : the.bases.getAll())
        {
            if (expansionFerryNeededAt(base))
            {
                target = base;
                break;
            }
        }
        Base* main = the.bases.myMain();
        if (!target || !main || !main->getDepot() ||
            workerData.getNumAssignedWorkers(main->getDepot()) < 8 ||
            !target->getAirSafePaths().isInitialized())
        {
            return;
        }

        std::vector<BWAPI::Unit> availableShips;
        for (BWAPI::Unit ship : the.self()->getUnits())
        {
            if (!ship || !ship->exists() || !ship->isCompleted() ||
                ship->getType() != BWAPI::UnitTypes::Terran_Dropship ||
                !ship->getLoadedUnits().empty() ||
                ship->getHitPoints() < 100 ||
                target->getAirSafeTileDistance(ship->getTilePosition()) < 0)
            {
                continue;
            }
            availableShips.push_back(ship);
        }
        std::sort(availableShips.begin(), availableShips.end(),
            [main](BWAPI::Unit a, BWAPI::Unit b)
            {
                return a->getDistance(main->getPosition()) <
                    b->getDistance(main->getPosition());
            });
        BWAPI::Unit bestShip = nullptr;
        for (BWAPI::Unit ship : availableShips)
        {
            if (CombatCommander::Instance().reserveExpansionFerryDropship(ship))
            {
                bestShip = ship;
                break;
            }
        }
        if (!bestShip)
        {
            return;
        }

        _ferryTransport = bestShip;
        _ferryDepot = target->getDepot();
        _ferryStartFrame = now;
        _ferryLastCommandFrame = -100000;
        _ferryPhase = 1;
        for (int i = 0; i < 2; ++i)
        {
            BWAPI::Unit bestWorker = nullptr;
            int workerDistance = MAX_DISTANCE;
            for (BWAPI::Unit worker : workerData.getWorkers())
            {
                if (!worker || !worker->exists() || !worker->isCompleted() ||
                    workerData.getWorkerJob(worker) != WorkerData::Minerals ||
                    workerData.getWorkerDepot(worker) != main->getDepot() ||
                    worker->isCarryingMinerals() || worker->isCarryingGas() ||
                    worker->getHitPoints() < 40 ||
                    _ferryWorkers.contains(worker))
                {
                    continue;
                }
                const int distance = worker->getDistance(bestShip);
                if (distance < workerDistance)
                {
                    bestWorker = worker;
                    workerDistance = distance;
                }
            }
            if (bestWorker)
            {
                workerData.setWorkerJob(bestWorker, WorkerData::Ferry, nullptr);
                _ferryWorkers.insert(bestWorker);
            }
        }
        if (_ferryWorkers.empty())
        {
            finish(false);
        }
        return;
    }

    if (!_ferryTransport || !_ferryTransport->exists())
    {
        finish(false);
        return;
    }

    if (!_ferryDepot || !_ferryDepot->exists() || _ferryDepot->isFlying() ||
        now - _ferryStartFrame > 90 * 24 ||
        _ferryTransport->getHitPoints() < 60)
    {
        // Keep ownership until the passengers are safely unloaded at home.
        // Returning a loaded ship to the combat-drop pool strands the SCVs.
        _ferryPhase = 4;
    }

    Base* target = _ferryDepot && _ferryDepot->exists()
        ? the.bases.getBaseAtTilePosition(_ferryDepot->getTilePosition())
        : nullptr;
    if (!target || target->inWorkerDanger())
    {
        _ferryPhase = 4;
    }

    if (_ferryPhase == 1)
    {
        bool allLoaded = true;
        BWAPI::Unit nextWorker = nullptr;
        for (BWAPI::Unit worker : _ferryWorkers)
        {
            if (worker && worker->exists() && !worker->isLoaded())
            {
                allLoaded = false;
                if (!nextWorker ||
                    worker->getDistance(_ferryTransport) <
                        nextWorker->getDistance(_ferryTransport))
                {
                    nextWorker = worker;
                }
            }
        }
        if (allLoaded && !_ferryTransport->getLoadedUnits().empty())
        {
            _ferryPhase = 2;
        }
        else if (now - _ferryStartFrame > 20 * 24)
        {
            _ferryPhase = 4;
        }
        else if (nextWorker && now - _ferryLastCommandFrame >= 16)
        {
            the.micro.Load(_ferryTransport, nextWorker);
            _ferryLastCommandFrame = now;
        }
        return;
    }

    Base* routeBase = _ferryPhase == 4
        ? the.bases.myMain() : target;
    if (!routeBase ||
        !routeBase->getAirSafePaths().isInitialized())
    {
        return;
    }
    BWAPI::TilePosition tile = _ferryTransport->getTilePosition();
    if (!tile.isValid())
    {
        return;
    }
    if (_ferryPhase == 4 && _ferryTransport->getHitPoints() < 60 &&
        !_ferryTransport->getLoadedUnits().empty() &&
        the.groundHits(tile) == 0 &&
        _ferryTransport->canUnloadAtPosition(_ferryTransport->getPosition()) &&
        now - _ferryLastCommandFrame >= 24)
    {
        the.micro.UnloadAll(_ferryTransport);
        _ferryLastCommandFrame = now;
        return;
    }
    if (_ferryPhase == 4 && _ferryTransport->getLoadedUnits().empty())
    {
        finish(false);
        return;
    }
    const int routeDistance = routeBase->getAirSafeTileDistance(tile);
    if (routeDistance < 0)
    {
        // Wait for a safe route to reopen rather than crossing known AA.
        return;
    }

    const BWAPI::Position destination = routeBase->getPosition();
    if (_ferryTransport->getDistance(destination) > 7 * 32)
    {
        if (now - _ferryLastCommandFrame < 12)
        {
            return;
        }
        // Follow the current threat-aware air distance field, a short segment
        // at a time. Never substitute a straight line through known AA.
        int distance = routeDistance;
        for (int step = 0; step < 8; ++step)
        {
            BWAPI::TilePosition best = tile;
            int bestDistance = distance;
            for (const BWAPI::TilePosition next : {
                BWAPI::TilePosition(tile.x + 1, tile.y),
                BWAPI::TilePosition(tile.x - 1, tile.y),
                BWAPI::TilePosition(tile.x, tile.y + 1),
                BWAPI::TilePosition(tile.x, tile.y - 1) })
            {
                if (next.isValid() && the.airHits(next) == 0)
                {
                    const int candidate = routeBase->getAirSafeTileDistance(next);
                    if (candidate >= 0 && candidate < bestDistance)
                    {
                        best = next;
                        bestDistance = candidate;
                    }
                }
            }
            if (best == tile)
            {
                break;
            }
            tile = best;
            distance = bestDistance;
        }
        if (distance < routeDistance)
        {
            the.micro.Move(_ferryTransport,
                BWAPI::Position(tile) + BWAPI::Position(16, 16));
            _ferryLastCommandFrame = now;
        }
        return;
    }

    BWAPI::Position unload = BWAPI::Positions::Invalid;
    int unloadDistance = MAX_DISTANCE;
    for (int dx : { -5, -4, 4, 5 })
    {
        for (int dy : { -5, -4, 4, 5 })
        {
            BWAPI::Position candidate = destination +
                BWAPI::Position(dx * 32, dy * 32);
            BWAPI::TilePosition spot(candidate);
            if (!candidate.isValid() || !spot.isValid() ||
                the.groundHits(spot) > 0 || the.airHits(spot) > 0 ||
                !_ferryTransport->canUnloadAtPosition(candidate))
            {
                continue;
            }
            int score = _ferryTransport->getDistance(candidate);
            if (score < unloadDistance)
            {
                unload = candidate;
                unloadDistance = score;
            }
        }
    }
    if (!unload.isValid() &&
        the.groundHits(_ferryTransport->getTilePosition()) == 0 &&
        _ferryTransport->canUnloadAtPosition(_ferryTransport->getPosition()))
    {
        unload = _ferryTransport->getPosition();
    }
    if (unload.isValid() && now - _ferryLastCommandFrame >= 24)
    {
        the.micro.UnloadAt(_ferryTransport, unload);
        _ferryLastCommandFrame = now;
    }
    if (_ferryTransport->getLoadedUnits().empty())
    {
        finish(_ferryPhase != 4);
    }
}

// Adjust worker jobs. This is done first, before handling each job.
// NOTE A mineral worker may go briefly idle after collecting minerals.
// That's OK; we don't change its status then.
void WorkerManager::updateWorkerStatus() 
{
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (!worker ||
            !worker->getType().isWorker() ||
            !worker->exists() ||
            worker->getPlayer() != the.self())
        {
            UAB_ASSERT(false, "bad worker");
            continue;
        }

        if (!worker->isCompleted())
        {
            continue;     // the worker list includes drones in the egg
        }

        if (isBusy(worker))
        {
            continue;
        }

        // Deal with workers burrowed due to irradiate.
        

        

        

        // If it's supposed to be on minerals but is actually collecting gas, fix it.
        // This can happen when we stop collecting gas; the worker can be mis-assigned.
        if (workerData.getWorkerJob(worker) == WorkerData::Minerals &&
            (worker->getOrder() == BWAPI::Orders::MoveToGas ||
             worker->getOrder() == BWAPI::Orders::WaitForGas ||
             worker->getOrder() == BWAPI::Orders::ReturnGas))
        {
            workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
        }

        // The worker's original job. It may change as we go!
        WorkerData::WorkerJob job = workerData.getWorkerJob(worker);

        // A global "anything is being built" check used to protect every Build
        // job. Consequently one unrelated queued structure could hide an SCV
        // whose own bunker task had already ended, leaving it in PlayerGuard
        // forever. Verify ownership of this exact worker instead.
        if ((job == WorkerData::Build ||
             job == WorkerData::PostedBuild) &&
            !worker->isConstructing() &&
            (worker->isIdle() ||
             worker->getOrder() == BWAPI::Orders::PlayerGuard) &&
            !BuildingManager::Instance().ownsBuilder(worker) &&
            !ProductionManager::Instance().ownsPredictedBuilder(worker))
        {
            // Clearing PostedBuild here is intentional: it is an orphan, not a
            // still-owned forward post. Let handleIdleWorkers() send it back to
            // the economy in this same update.
            workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
            job = WorkerData::Idle;
        }

        // Remote mineral mining is owned only while WorkerData still considers
        // the SCV Idle. If construction/scouting/repair/combat takes the worker,
        // immediately forget the remote assignment so two systems never issue
        // orders to the same SCV.
        if (job != WorkerData::Idle)
        {
            remoteMineralWorkers.erase(worker);
        }

        // Idleness.
        // Order can be PlayerGuard for a drone that tries to build and fails.
        // There are other causes, e.g. being done with a job like repair.
        if ((worker->isIdle() || worker->getOrder() == BWAPI::Orders::PlayerGuard) &&
            job != WorkerData::Minerals &&
            job != WorkerData::Build &&
            job != WorkerData::Scout &&
            job != WorkerData::Posted &&
            job != WorkerData::PostedBuild &&
            job != WorkerData::Ferry)
        {
            //BWAPI::Broodwar->printf("idle worker");
            workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
        }

        else if (job == WorkerData::Gas)
        {
            BWAPI::Unit refinery = workerData.getWorkerResource(worker);

            // If the refinery is gone.
            // A missing resource depot is dealt with in handleGasWorkers().
            // Some of the conditions checked here may be impossible, but let's be thorough.
            if (!refinery ||
                !refinery->exists() ||
                refinery->getHitPoints() == 0 ||
                !refinery->getType().isRefinery() ||
                refinery->getPlayer() != the.self())
            {
                workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
            }
            // If the worker is busy mining and an enemy comes near, maybe fight it.
            else if (defendSelf(worker, workerData.getWorkerResource(worker)))
            {
                // defendSelf() does the work.
            }
            else if (maybeFleeDanger(worker))
            {
                // maybeFleeDanger() does the work.
            }
            else if (!UnitUtil::GasOrder(worker))
            {
                workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
            }
        }
        
        else if (job == WorkerData::Minerals)
        {
            // If the worker is busy mining and an enemy comes near, maybe fight it.
            if (defendSelf(worker, workerData.getWorkerResource(worker)))
            {
                // defendSelf() does the work.
            }
            else if (maybeFleeDanger(worker))
            {
                // maybeFleeDanger() does the work.
            }
            else if (
                worker->getOrder() == BWAPI::Orders::MoveToMinerals ||
                worker->getOrder() == BWAPI::Orders::WaitForMinerals)
            {
                // If the mineral patch is mined out, release the worker from its job.
                BWAPI::Unit patch = workerData.getWorkerResource(worker);
                if (!patch || !patch->exists())
                {
                    workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
                }
            }
            else if (!UnitUtil::MineralOrder(worker))
            {
                // The worker is not actually mining. Release it.
                workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
            }
        }
    }
}

void WorkerManager::setRepairWorker(BWAPI::Unit worker, BWAPI::Unit unitToRepair)
{
    workerData.setWorkerJob(worker, WorkerData::Repair, unitToRepair);
}

void WorkerManager::stopRepairing(BWAPI::Unit worker)
{
    returnWorkerToEconomy(worker);
}

// If appropriate, mine out any blocking mineral patches near our bases.
void WorkerManager::clearBlockingMinerals()
{
    if (the.now() % 49 != 0 ||
        the.bases.getSmallMinerals().empty() ||
        the.my.completed.count(the.selfRace().getWorker()) < 18 ||
        the.bases.baseCount(the.self()) < 2 ||
        workerData.anyUnblocker())
    {
        return;
    }

    const int radiusOfConcern = 600;

    // We'll check bases we own, and bases we reserved, and the next base we may want.
    Base * nextExpo = the.map.nextExpansion(false, true, true);
    //BWAPI::Broodwar->printf("check blocking mineral, next base %d", nextExpo ? nextExpo->getID() : -1);

    // Look for a blocking mineral that should be mined out.
    BWAPI::Unit worstBlock = nullptr;
    int closestDist = radiusOfConcern;
    for (BWAPI::Unit block : the.bases.getSmallMinerals())
    {
        if (block->getInitialTilePosition().isValid() && 0 == the.groundHitsFixed.at(block->getInitialTilePosition()))
        {
            for (const Base * base : the.bases.getAll())
            {
                if (base->getOwner() == the.self() ||
                    the.placer.isReserved(base->getTilePosition()) ||
                    base == nextExpo)
                {
                    int dist = base->getCenter().getApproxDistance(block->getInitialPosition());
                    if (dist < closestDist)
                    {
                        //BWAPI::Broodwar->printf("blocking mineral @ %d,%d dist %d", block->getInitialPosition().x, block->getInitialPosition().y, dist);
                        worstBlock = block;
                        closestDist = dist;
                    }
                }
            }
        }
    }

    // If we found one, choose a worker to mine it out.
    if (worstBlock)
    {
        BWAPI::Unit worker = getUnencumberedWorker(worstBlock->getInitialPosition(), MAX_DISTANCE);
        if (worker)
        {
            workerData.setWorkerJob(worker, worstBlock->getInitialTilePosition());
        }
    }
}

// Move gas workers on or off gas as necessary.
// NOTE A worker inside a refinery does not accept orders.
void WorkerManager::handleGasWorkers() 
{
    if (_collectGas)
    {
        int nBases = the.bases.completedBaseCount(the.self());

        for (const Base * base : the.bases.getAll())
        {
            BWAPI::Unit depot = base->getDepot();

            // Mineral income must survive a worker loss. Reserve the first six
            // economic workers at EACH base for minerals, then add gas workers
            // one at a time. Share this allowance across all local refineries.
            const int mineralFloor = std::min(6, 2 * int(base->getRemainingMinerals().size()));
            int gasAllowance = 0;
            if (the.selfRace() == BWAPI::Races::Terran && depot)
            {
                int localGasWorkers = 0;
                for (BWAPI::Unit refinery : base->getGeysers())
                    localGasWorkers += workerData.getNumAssignedWorkers(refinery);
                gasAllowance = std::max(0,
                    workerData.getNumAssignedWorkers(depot) + localGasWorkers - mineralFloor);
            }

            for (BWAPI::Unit geyser : base->getGeysers())
            {
                // Don't add more workers to a refinery at a base under attack (unless it is
                // the only base). Limit losses to the workers that are already there.
                if (base->getOwner() == the.self() &&
                    geyser->getType().isRefinery() &&
                    geyser->isCompleted() &&
                    geyser->getPlayer() == the.self() &&
                    UnitUtil::IsCompletedResourceDepot(depot) &&
                    (!base->inWorkerDanger() || nBases == 1))
                {
                    const int desiredWorkersPerRefinery =
                        the.selfRace() == BWAPI::Races::Terran
                        ? std::min(Config::Macro::WorkersPerRefinery, gasAllowance)
                        : Config::Macro::WorkersPerRefinery;
                    gasAllowance -= desiredWorkersPerRefinery;

                    int numAssigned = workerData.getNumAssignedWorkers(geyser);

                    if (numAssigned > desiredWorkersPerRefinery)
                    {
                        std::set<BWAPI::Unit> gasWorkers;
                        workerData.getGasWorkers(gasWorkers);

                        int toRemove = numAssigned - desiredWorkersPerRefinery;

                        for (BWAPI::Unit gasWorker : gasWorkers)
                        {
                            if (toRemove <= 0)
                            {
                                break;
                            }

                            if (!gasWorker || !gasWorker->exists())
                            {
                                continue;
                            }

                            if (workerData.getWorkerResource(gasWorker) != geyser)
                            {
                                continue;
                            }

                            if (gasWorker->getOrder() == BWAPI::Orders::HarvestGas)
                            {
                                continue;
                            }

                            workerData.setWorkerJob(gasWorker, WorkerData::Idle, nullptr);
                            --toRemove;
                        }

                        numAssigned = workerData.getNumAssignedWorkers(geyser);
                    }

                    for (int i = 0; i < (desiredWorkersPerRefinery - numAssigned); ++i)
                    {
                        BWAPI::Unit gasWorker = getGasWorker(geyser);
                        if (gasWorker)
                        {
                            workerData.setWorkerJob(gasWorker, WorkerData::Gas, geyser);
                        }
                        else
                        {
                            // Another base may still need to evacuate gas.
                            break;
                        }
                    }
                }
                else
                {
                    // The refinery is gone or otherwise no good. Remove any gas workers.
                    std::set<BWAPI::Unit> gasWorkers;
                    workerData.getGasWorkers(gasWorkers);
                    for (BWAPI::Unit gasWorker : gasWorkers)
                    {
                        if (geyser == workerData.getWorkerResource(gasWorker) &&
                            gasWorker->getOrder() != BWAPI::Orders::HarvestGas)  // not inside the refinery
                        {
                            workerData.setWorkerJob(gasWorker, WorkerData::Idle, nullptr);
                        }
                    }
                }
            }
        }
    }
    else
    {
        // Don't gather gas: If workers are assigned to gas anywhere, take them off.
        std::set<BWAPI::Unit> gasWorkers;
        workerData.getGasWorkers(gasWorkers);
        for (BWAPI::Unit gasWorker : gasWorkers)
        {
            if (gasWorker->getOrder() != BWAPI::Orders::HarvestGas)    // not inside the refinery
            {
                workerData.setWorkerJob(gasWorker, WorkerData::Idle, nullptr);
                // An idle worker carrying gas will become a ReturnCargo worker,
                // so gas will not be lost needlessly.
            }
        }
    }
}

// Is the refinery near a resource depot that it can deliver gas to?
bool WorkerManager::refineryHasDepot(BWAPI::Unit refinery)
{
    // Iterate through units, not bases, because even if the main hatchery is destroyed
    // (so the base is considered gone), a macro hatchery may be close enough.
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (UnitUtil::IsCompletedResourceDepot(unit) &&
            unit->getDistance(refinery) < 400)
        {
            return true;
        }
    }

    return false;
}

void WorkerManager::handleIdleWorkers() 
{
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (workerData.getWorkerJob(worker) != WorkerData::Idle)
        {
            continue;
        }

        auto remoteIt = remoteMineralWorkers.find(worker);
        if (remoteIt != remoteMineralWorkers.end())
        {
            RemoteMineralState& remote = remoteIt->second;

            if (!remote.patch || !remote.patch->exists())
            {
                remoteMineralWorkers.erase(remoteIt);
                setMineralWorker(worker);
                continue;
            }

            if (worker->isBurrowed() || worker->getOrder() == BWAPI::Orders::Burrowing)
            {
                continue;
            }

            if (maybeFleeDanger(worker, WeaponsMarginPlus))
            {
                continue;
            }

            // A usable owned base may have appeared while the SCV was doing
            // long-distance mining. Rejoin normal saturation promptly.
            if ((the.now() + worker->getID()) % 24 == 0)
            {
                BWAPI::Unit normalDepot = getClosestNonFullDepot(worker);
                if (normalDepot)
                {
                    remoteMineralWorkers.erase(remoteIt);
                    workerData.setWorkerJob(worker, WorkerData::Minerals, normalDepot);
                    continue;
                }
            }

            if (worker->isCarryingMinerals() || worker->isCarryingGas())
            {
                BWAPI::Unit depot = getAnyClosestDepot(worker);
                if (depot)
                {
                    the.micro.ReturnCargo(worker);
                    makeBusy(worker);
                }
                else
                {
                    // There is nowhere to return the cargo yet. If a new depot
                    // is under construction, wait near it rather than idling on
                    // top of the mineral line and blocking other SCVs.
                    BWAPI::Unit futureDepot = nullptr;
                    int bestDistance = MAX_DISTANCE;
                    for (BWAPI::Unit own : the.self()->getUnits())
                    {
                        if (!own || !own->exists() || own->isFlying() ||
                            !own->getType().isResourceDepot())
                        {
                            continue;
                        }

                        const int distance = worker->getDistance(own);
                        if (distance < bestDistance)
                        {
                            bestDistance = distance;
                            futureDepot = own;
                        }
                    }

                    if (futureDepot && worker->getDistance(futureDepot) > 3 * 32 &&
                        the.now() - remote.lastCommandFrame >= 24)
                    {
                        the.micro.MoveNear(worker, futureDepot->getPosition());
                        remote.lastCommandFrame = the.now();
                        makeBusy(worker);
                    }
                }
                continue;
            }

            const bool miningThisPatch =
                UnitUtil::MineralOrder(worker) &&
                (worker->getOrderTarget() == remote.patch ||
                 worker->getTarget() == remote.patch);

            // If the SCV arrived and stopped beside the minerals (the failure
            // visible in the screenshot), explicitly re-issue gather. Do not
            // spam the command while it is already mining the assigned patch.
            if (!miningThisPatch && the.now() - remote.lastCommandFrame >= 12)
            {
                the.micro.MineMinerals(worker, remote.patch);
                remote.lastCommandFrame = the.now();
                makeBusy(worker);
            }
            continue;
        }

        if (worker->isBurrowed() || worker->getOrder() == BWAPI::Orders::Burrowing)
        {
            // All burrowed workers are set to Idle.
        }
        else if (maybeFleeDanger(worker, WeaponsMarginPlus))
        {
            // maybeFleeDanger does the work. Notes:
            // 1. A worker fleeing danger is set to Idle.
            // 2. An Idle worker fleeing danger has a wider safety margin.
            //    Thus working workers are reluctant to run, but run far when they do,
            //    while workers once idle try to stay safe.
        }
        else if (worker->isCarryingMinerals() || worker->isCarryingGas())
        {
            // It's carrying something, set it to hand in its cargo.
            setReturnCargoWorker(worker);         // only happens if there's a resource depot
        }
        else
        {
            // Otherwise send it to mine minerals.
            setMineralWorker(worker);             // also supports long-distance mining
        }
    }
}

void WorkerManager::handleReturnCargoWorkers()
{
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (workerData.getWorkerJob(worker) == WorkerData::ReturnCargo)
        {
            // If it still needs to return cargo, return it.
            // We have to make sure it has a resource depot to return cargo to.
            BWAPI::Unit depot;
            if ((worker->isCarryingMinerals() || worker->isCarryingGas()) &&
                (depot = getAnyClosestDepot(worker)) &&
                worker->getDistance(depot) < 600)
            {
                the.micro.ReturnCargo(worker);
                makeBusy(worker);
            }
            else
            {
                // Can't return cargo. Let's be a mineral worker instead--if possible.
                setMineralWorker(worker);
            }
        }
    }
}

// Terran can assign SCVs to repair.
void WorkerManager::handleRepairWorkers()
{
    if (the.self()->getRace() != BWAPI::Races::Terran ||
        the.my.completed.count(BWAPI::UnitTypes::Terran_SCV) == 0)
    {
        return;
    }

    const int now = the.now();
    if (_lastMobileRepairObservedFrame >= 0 &&
        now < _lastMobileRepairObservedFrame)
    {
        _mobileRepairPositions.clear();
        _nextMobileRepairFrame = 0;
    }
    _lastMobileRepairObservedFrame = now;

    auto isMobileRepairType = [](BWAPI::UnitType type)
    {
        return
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
            type == BWAPI::UnitTypes::Terran_Goliath ||
            type == BWAPI::UnitTypes::Terran_Vulture;
    };

    // Remove leases whose target died, changed owner/type, or is already above
    // the intentional 95% mobile-repair release threshold.
    std::map<int, BWAPI::Unit> liveByID;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit && unit->exists())
        {
            liveByID[unit->getID()] = unit;
        }
    }
    for (auto it = _mobileRepairPositions.begin();
         it != _mobileRepairPositions.end(); )
    {
        const auto live = liveByID.find(it->first);
        if (live == liveByID.end() ||
            !isMobileRepairType(live->second->getType()) ||
            live->second->getHitPoints() * 100 >=
                live->second->getType().maxHitPoints() * 95)
        {
            it = _mobileRepairPositions.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Repair assignments need an explicit completion path. BWAPI may keep the
    // Repair order/target for a short time after a bunker reaches full HP, so
    // the generic "worker is idle" test is not sufficient and can strand SCVs.
    // Build a snapshot from the worker job API before releasing completed
    // assignments; changing jobs while iterating the repair set is unsafe.
    std::set<BWAPI::Unit> currentRepairers;
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (worker && worker->exists() &&
            workerData.getWorkerJob(worker) == WorkerData::Repair)
        {
            currentRepairers.insert(worker);
        }
    }
    for (BWAPI::Unit worker : currentRepairers)
    {
        BWAPI::Unit target = workerData.getWorkerRepairUnit(worker);
        const bool mobileRepair = target &&
            _mobileRepairPositions.find(target->getID()) !=
                _mobileRepairPositions.end();
        const int releasePercent = mobileRepair ? 95 : 100;
        const bool targetCanStillBeRepaired =
            target &&
            target->exists() &&
            target->getPlayer() == the.self() &&
            target->getHitPoints() > 0 &&
            target->getHitPoints() * 100 <
                target->getType().maxHitPoints() * releasePercent;

        if (!targetCanStillBeRepaired || the.self()->minerals() == 0)
        {
            if (mobileRepair && target)
            {
                _mobileRepairPositions.erase(target->getID());
            }
            stopRepairing(worker);
            continue;
        }

        // Combat repair never owns an SCV through active fire. The target stays
        // in its original squad; its own micro manager will resume combat while
        // this SCV immediately returns toward the economy.
        if (mobileRepair &&
            (target->isUnderAttack() || the.hits(target) > 0 ||
             maybeFleeDanger(worker, WeaponsMarginPlus)))
        {
            _mobileRepairPositions.erase(target->getID());
            if (workerData.getWorkerJob(worker) == WorkerData::Repair)
            {
                stopRepairing(worker);
            }
            continue;
        }

        const bool stillFollowingRepairTarget =
            worker->isRepairing() ||
            worker->getOrderTarget() == target ||
            worker->getTarget() == target;

        if (!stillFollowingRepairTarget)
        {
            the.micro.Repair(worker, target);
            makeBusy(worker);
        }
    }

    if (the.self()->minerals() == 0)
    {
        return;
    }

    BWAPI::Position carrierThreatBase = BWAPI::Positions::Invalid;
    int visibleCarriersAtBase = 0;

    for (BWAPI::Unit enemy : the.enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            enemy->getType() != BWAPI::UnitTypes::Protoss_Carrier)
        {
            continue;
        }

        for (BWAPI::Unit own : the.self()->getUnits())
        {
            if (own && own->exists() && own->isCompleted() &&
                own->getType() == BWAPI::UnitTypes::Terran_Command_Center &&
                enemy->getDistance(own) <= 14 * 32)
            {
                carrierThreatBase = own->getPosition();
                ++visibleCarriersAtBase;
                break;
            }
        }
    }

    size_t maxRepairers =
        1 + the.my.completed.count(BWAPI::UnitTypes::Terran_SCV) / 6;

    if (carrierThreatBase.isValid() && getNumMineralWorkers() >= 8)
    {
        maxRepairers = std::max(
            maxRepairers,
            size_t(std::min(4, std::max(2, visibleCarriersAtBase + 1))));
    }
    int nAlreadyRepairing = workerData.getNumRepairWorkers();
    if (size_t(nAlreadyRepairing) >= maxRepairers)
    {
        return;
    }
    maxRepairers -= nAlreadyRepairing;

    // During a real Carrier attack at an owned base, repair the mobile AA
    // without pulling the economy across the map. Turrets are still handled by
    // the building pass below; together they use at most four SCVs.
    if (carrierThreatBase.isValid() && maxRepairers > 0)
    {
        std::vector<BWAPI::Unit> damagedGoliaths;
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (unit && unit->exists() && unit->isCompleted() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Goliath &&
                unit->getPosition().isValid() &&
                unit->getDistance(carrierThreatBase) <= 12 * 32 &&
                unit->getHitPoints() < unit->getType().maxHitPoints())
            {
                damagedGoliaths.push_back(unit);
            }
        }

        std::sort(
            damagedGoliaths.begin(),
            damagedGoliaths.end(),
            [](BWAPI::Unit a, BWAPI::Unit b)
            {
                const int aPercent =
                    100 * a->getHitPoints() / a->getType().maxHitPoints();
                const int bPercent =
                    100 * b->getHitPoints() / b->getType().maxHitPoints();
                return aPercent != bPercent
                    ? aPercent < bPercent
                    : a->getID() < b->getID();
            });

        size_t goliathRepairBudget = std::min(maxRepairers, size_t(2));
        for (BWAPI::Unit goliath : damagedGoliaths)
        {
            if (goliathRepairBudget == 0)
            {
                break;
            }

            BWAPI::Unit repairWorker = getClosestMineralWorkerTo(goliath);
            if (!repairWorker)
            {
                break;
            }

            setRepairWorker(repairWorker, goliath);
            --maxRepairers;
            --goliathRepairBudget;
            if (maxRepairers == 0)
            {
                return;
            }
        }
    }

    // The verified Terran wall is combat infrastructure, just like a Bunker.
    // Repair its Barracks and both Depots before spending the remaining repair
    // budget on ordinary damaged buildings. Previously these pieces waited
    // until below 50% HP, which let focused Zealot/Dragoon fire open the wall
    // while SCVs were available.
    for (BWAPI::Unit wall : the.self()->getUnits())
    {
        if (maxRepairers == 0)
        {
            return;
        }

        if (!wall || !wall->exists() || !wall->isCompleted() ||
            wall->isLifted() ||
            !the.placer.isTerranMainWallBuilding(wall) ||
            wall->getHitPoints() <= 0 ||
            wall->getHitPoints() >= wall->getType().maxHitPoints())
        {
            continue;
        }

        size_t existingOnWall = 0;
        for (BWAPI::Unit worker : workerData.getWorkers())
        {
            if (worker && worker->exists() &&
                workerData.getWorkerJob(worker) == WorkerData::Repair &&
                workerData.getWorkerRepairUnit(worker) == wall)
            {
                ++existingOnWall;
            }
        }

        const size_t enemyPressure =
            the.info.getEnemyFireteam(wall).size();
        const size_t desiredRepairers =
            enemyPressure > 0 ||
            wall->getHitPoints() * 4 < wall->getType().maxHitPoints() * 3
            ? 2
            : 1;

        while (existingOnWall < desiredRepairers && maxRepairers > 0)
        {
            BWAPI::Unit repairWorker = getClosestMineralWorkerTo(wall);
            if (!repairWorker)
            {
                break;
            }

            setRepairWorker(repairWorker, wall);
            ++existingOnWall;
            --maxRepairers;
        }
    }

    auto assignBuildingRepairs = [&](bool staticDefenseOnly) -> bool
    {
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (!unit || !unit->exists() ||
                the.placer.isTerranMainWallBuilding(unit))
            {
                continue;
            }

            const bool staticDefense =
                unit->getType() == BWAPI::UnitTypes::Terran_Bunker ||
                unit->getType() == BWAPI::UnitTypes::Terran_Missile_Turret;
            if (staticDefense != staticDefenseOnly ||
                !unit->getType().isBuilding() || !unit->isCompleted() ||
                unit->getHitPoints() <= 0 ||
                unit->getHitPoints() >= unit->getType().maxHitPoints())
            {
                continue;
            }

            size_t desiredRepairers = 0;
            if (staticDefense)
            {
                desiredRepairers = std::max(
                    size_t(1), the.info.getEnemyFireteam(unit).size());
            }
            else if (unit->getHitPoints() < unit->getType().maxHitPoints() / 2)
            {
                desiredRepairers = 1;
            }

            size_t existingRepairers = 0;
            for (BWAPI::Unit worker : workerData.getWorkers())
            {
                if (worker && worker->exists() &&
                    workerData.getWorkerJob(worker) == WorkerData::Repair &&
                    workerData.getWorkerRepairUnit(worker) == unit)
                {
                    ++existingRepairers;
                }
            }

            while (existingRepairers < desiredRepairers && maxRepairers > 0)
            {
                BWAPI::Unit repairWorker = getClosestMineralWorkerTo(unit);
                if (!repairWorker)
                {
                    break;
                }
                setRepairWorker(repairWorker, unit);
                ++existingRepairers;
                --maxRepairers;
            }
            if (maxRepairers == 0)
            {
                return true;
            }
        }
        return false;
    };

    // Existing wall/Bunker/Turret/Carrier-defense repair retains precedence.
    if (assignBuildingRepairs(true))
    {
        return;
    }

    int activeMobileRepairers = 0;
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (!worker || !worker->exists() ||
            workerData.getWorkerJob(worker) != WorkerData::Repair)
        {
            continue;
        }
        BWAPI::Unit target = workerData.getWorkerRepairUnit(worker);
        if (target && _mobileRepairPositions.count(target->getID()))
        {
            ++activeMobileRepairers;
        }
    }

    const int scvs = the.my.completed.count(BWAPI::UnitTypes::Terran_SCV);
    int mobileRepairLimit = scvs >= 40 ? 3 : (scvs >= 26 ? 2 : (scvs >= 18 ? 1 : 0));
    if (getNumMineralWorkers() < 10 || the.self()->minerals() < 25)
    {
        mobileRepairLimit = 0;
    }

    if (now >= _nextMobileRepairFrame && maxRepairers > 0 &&
        activeMobileRepairers < mobileRepairLimit)
    {
        _nextMobileRepairFrame = now + 12;

        struct MobileRepairCandidate
        {
            BWAPI::Unit unit;
            BWAPI::Position rally;
            int priority;
            int healthPercent;
        };
        std::vector<MobileRepairCandidate> candidates;

        auto enemyGroundThreatNear = [](BWAPI::Unit unit) -> bool
        {
            if (!unit || !unit->getPosition().isValid())
            {
                return true;
            }
            for (BWAPI::Unit enemy : the.enemy()->getUnits())
            {
                if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                    enemy->isFlying() || enemy->getType().isSpell() ||
                    !enemy->getPosition().isValid() ||
                    !UnitUtil::CanAttackGround(enemy))
                {
                    continue;
                }
                if (enemy->getDistance(unit) <= 11 * 32)
                {
                    return true;
                }
            }
            return false;
        };

        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (!unit || !unit->exists() || !unit->isCompleted() ||
                unit->isLoaded() || !unit->getPosition().isValid() ||
                !isMobileRepairType(unit->getType()) ||
                _mobileRepairPositions.count(unit->getID()) ||
                unit->getHitPoints() <= 0 || unit->isUnderAttack() ||
                unit->getOrder() == BWAPI::Orders::PlaceMine ||
                the.hits(unit) > 0 || enemyGroundThreatNear(unit))
            {
                continue;
            }

            const int hp = 100 * unit->getHitPoints() /
                std::max(1, unit->getType().maxHitPoints());
            int priority = -1;
            const BWAPI::UnitType type = unit->getType();
            const bool tank =
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
            if (tank && hp < 50) priority = 0;
            else if (tank && hp < 75) priority = 1;
            else if (type == BWAPI::UnitTypes::Terran_Goliath && hp < 50) priority = 2;
            else if (type == BWAPI::UnitTypes::Terran_Goliath && hp < 75) priority = 3;
            else if (type == BWAPI::UnitTypes::Terran_Vulture && hp < 35) priority = 4;
            if (priority < 0)
            {
                continue;
            }

            Base * safeBase = nullptr;
            int safeBaseDistance = MAX_DISTANCE;
            for (Base * base : the.bases.getAll())
            {
                if (!base || base->getOwner() != the.self() ||
                    !base->isMyCompletedBase() || !base->getCenter().isValid())
                {
                    continue;
                }
                const int distance = the.map.getGroundDistance(
                    unit->getPosition(), base->getCenter());
                if (distance >= 0 && distance < safeBaseDistance)
                {
                    safeBaseDistance = distance;
                    safeBase = base;
                }
            }
            if (!safeBase || safeBaseDistance > 42 * 32)
            {
                continue;
            }

            BWAPI::Position wanted = the.map.getGroundPathWaypoint(
                unit->getPosition(), safeBase->getCenter(), 5, false);
            if (!wanted.isValid())
            {
                wanted = safeBase->getFront().isValid()
                    ? safeBase->getFront() : safeBase->getCenter();
            }

            BWAPI::Position rally = BWAPI::Positions::Invalid;
            int rallyScore = MAX_DISTANCE;
            const BWAPI::TilePosition wantedTile(wanted);
            for (int radius = 0; radius <= 5; ++radius)
            {
                for (int dx = -radius; dx <= radius; ++dx)
                {
                    for (int dy = -radius; dy <= radius; ++dy)
                    {
                        if (std::max(std::abs(dx), std::abs(dy)) != radius)
                            continue;
                        const BWAPI::TilePosition tile(
                            wantedTile.x + dx, wantedTile.y + dy);
                        if (!tile.isValid() || !the.map.isWalkable(tile) ||
                            the.groundHitsFixed.at(tile) > 0 ||
                            the.groundHitsMobile.at(tile) > 0 ||
                            the.partitions.id(tile) !=
                                the.partitions.id(unit->getTilePosition()))
                        {
                            continue;
                        }
                        const BWAPI::Position position = TileCenter(tile);
                        const int score = position.getApproxDistance(wanted) +
                            position.getApproxDistance(safeBase->getCenter()) / 8;
                        if (score < rallyScore)
                        {
                            rallyScore = score;
                            rally = position;
                        }
                    }
                }
                if (rally.isValid() && radius >= 2)
                    break;
            }
            if (!rally.isValid())
            {
                continue;
            }

            std::vector<BWAPI::TilePosition> retreatPath;
            if (!the.map.getGroundPath(
                    unit->getTilePosition(), BWAPI::TilePosition(rally),
                    retreatPath, 64, false))
            {
                continue;
            }
            bool safeRetreat = true;
            for (size_t i = 0; i < retreatPath.size(); i += 3)
            {
                if (the.groundHitsFixed.at(retreatPath[i]) > 0 ||
                    the.groundHitsMobile.at(retreatPath[i]) > 0)
                {
                    safeRetreat = false;
                    break;
                }
            }
            if (!safeRetreat)
            {
                continue;
            }
            candidates.push_back({ unit, rally, priority, hp });
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const MobileRepairCandidate & a, const MobileRepairCandidate & b)
            {
                if (a.priority != b.priority) return a.priority < b.priority;
                if (a.healthPercent != b.healthPercent)
                    return a.healthPercent < b.healthPercent;
                return a.unit->getID() < b.unit->getID();
            });

        for (const MobileRepairCandidate & candidate : candidates)
        {
            if (activeMobileRepairers >= mobileRepairLimit || maxRepairers == 0)
            {
                break;
            }

            bool alreadyHasRepairer = false;
            for (BWAPI::Unit worker : workerData.getWorkers())
            {
                if (worker && worker->exists() &&
                    workerData.getWorkerJob(worker) == WorkerData::Repair &&
                    workerData.getWorkerRepairUnit(worker) == candidate.unit)
                {
                    alreadyHasRepairer = true;
                    break;
                }
            }
            if (alreadyHasRepairer)
            {
                continue;
            }

            BWAPI::Unit bestWorker = nullptr;
            int bestWorkerDistance = MAX_DISTANCE;
            for (BWAPI::Unit worker : workerData.getWorkers())
            {
                if (!worker || !worker->exists() || !worker->isCompleted() ||
                    workerData.getWorkerJob(worker) != WorkerData::Minerals ||
                    worker->isCarryingMinerals() || worker->isCarryingGas() ||
                    the.hits(worker) > 0)
                {
                    continue;
                }
                BWAPI::Unit sourceDepot = workerData.getWorkerDepot(worker);
                if (!sourceDepot ||
                    workerData.getNumAssignedWorkers(sourceDepot) <= 6)
                {
                    continue;
                }

                const int distance = worker->getDistance(candidate.rally);
                if (distance > 30 * 32 || distance >= bestWorkerDistance)
                {
                    continue;
                }

                std::vector<BWAPI::TilePosition> path;
                if (!the.map.getGroundPath(
                        worker->getTilePosition(),
                        BWAPI::TilePosition(candidate.rally), path, 96, false))
                {
                    continue;
                }
                bool safePath = true;
                for (size_t i = 0; i < path.size(); i += 3)
                {
                    if (the.groundHitsFixed.at(path[i]) > 0 ||
                        the.groundHitsMobile.at(path[i]) > 0)
                    {
                        safePath = false;
                        break;
                    }
                }
                if (safePath)
                {
                    bestWorker = worker;
                    bestWorkerDistance = distance;
                }
            }

            if (!bestWorker)
            {
                continue;
            }

            _mobileRepairPositions[candidate.unit->getID()] = candidate.rally;
            setRepairWorker(bestWorker, candidate.unit);
            ++activeMobileRepairers;
            --maxRepairers;
        }
    }

    if (Config::Debug::DrawWorkerInfo || Config::Debug::DrawUnitTargets)
    {
        for (const auto & repair : _mobileRepairPositions)
        {
            const auto live = liveByID.find(repair.first);
            if (live == liveByID.end())
            {
                continue;
            }
            BWAPI::Broodwar->drawLineMap(
                live->second->getPosition(), repair.second,
                BWAPI::Colors::Green);
            BWAPI::Broodwar->drawCircleMap(
                repair.second, 24, BWAPI::Colors::Green, false);
            BWAPI::Broodwar->drawTextMap(
                repair.second + BWAPI::Position(6, 8), "REPAIR");
        }
    }

    if (maxRepairers == 0)
    {
        return;
    }
    (void)assignBuildingRepairs(false);
}

// Steamhammer's mineral locking is modeled after Locutus (but different in detail).
// This implements the "wait for the previous worker to be done" part of mineral locking.
void WorkerManager::handleMineralWorkers()
{
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (workerData.getWorkerJob(worker) == WorkerData::Minerals)
        {
            if (worker->getOrder() == BWAPI::Orders::MoveToMinerals ||
                worker->getOrder() == BWAPI::Orders::WaitForMinerals)
            {
                BWAPI::Unit patch = workerData.getWorkerResource(worker);
                if (patch && patch->exists() && worker->getOrderTarget() != patch)
                {
                    the.micro.MineMinerals(worker, patch);
                    makeBusy(worker);
                }
            }
        }
    }
}

// Workers assigned to mine out blocking minerals.
void WorkerManager::handleUnblockWorkers()
{
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (workerData.getWorkerJob(worker) == WorkerData::Unblock)
        {
            BWAPI::TilePosition tile = workerData.getWorkerTile(worker);
            if (worker->isCarryingMinerals())
            {
                // The blocking mineral patch must have contained minerals. Return what we mined.
                the.micro.ReturnCargo(worker);
                makeBusy(worker);
            }
            else if (!BWAPI::Broodwar->isVisible(tile))
            {
                // Move the worker into sight range of the target tile.
                the.micro.MoveNear(worker, BWAPI::Position(tile));
                makeBusy(worker);
            }
            else
            {
                BWAPI::Unitset patches =
                    BWAPI::Broodwar->getUnitsOnTile(tile, BWAPI::Filter::IsMineralField);
                if (patches.empty() || the.groundHitsFixed.at(tile) > 0)
                {
                    workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
                }
                else if (!worker->getTarget() || worker->getTarget()->getTilePosition() != tile)
                {
                    the.micro.RightClick(worker, *patches.begin());
                    makeBusy(worker);
                }
            }
        }
    }
}

// Workers assigned to specific posts on the map.
void WorkerManager::handlePostedWorkers()
{
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (workerData.getWorkerJob(worker) == WorkerData::Posted)
        {
            if (maybeFleeDanger(worker))
            {
                // maybeFleeDanger() does the work.
            }
            else if (worker->isCarryingMinerals() || worker->isCarryingGas())
            {
                the.micro.ReturnCargo(worker);
                makeBusy(worker);
            }
            else
            {
                BWAPI::Position pos = workerData.getWorkerPostPosition(worker);
                if (worker->getDistance(pos) > 8 * 32)
                {
                    // BWAPI::Broodwar->printf("moving posted worker %d to %d,%d", worker->getID(), pos.x, pos.y);
                    the.micro.MoveNear(worker, pos);
                    makeBusy(worker);
                }
            }
        }
    }
}

// If the worker is in danger from the enemy, try to flee.
// Return true if we fled, otherwise false to continue with regular behavior.
// margin is the margin of safety from enemy weapons fire, in pixels.
// Fleeing workers are considered Idle; they're not doing anything else.
bool WorkerManager::maybeFleeDanger(BWAPI::Unit worker, int margin)
{
    UAB_ASSERT(worker, "Worker was null");

    if (worker->getPosition().isValid() &&          // false if we're e.g. in a refinery
        worker->canMove())
    {
        // We consider only the closest enemy unit that can shoot us.
        BWAPI::Unit enemy = the.micro.inWeaponsDanger(worker, margin);
        if (enemy)
        {
            workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
            
            {
                the.micro.fleeEnemy(worker, enemy);
                //BWAPI::Broodwar->printf("worker %d flees", worker->getID());
            }
            makeBusy(worker);
            return true;
        }
    }

    return false;
}

// Unburrow any burrowed drones in burrowedForSafety[] that it appears safe to.


// Is the worker dangerously near an irradiated unit?
// Used in deciding whether to burrow or unburrow, so ignore the unit's burrow status.


// Used for worker self-defense.
// Only include weak and very close enemy units that can be targeted by workers
// and are not moving, or are stuck and moving randomly to dislodge themselves.
// Returns null if none (the most common case).
BWAPI::Unit WorkerManager::findEnemyTargetForWorker(BWAPI::Unit worker) const
{
    UAB_ASSERT(worker, "Worker was null");

	return BWAPI::Broodwar->getClosestUnit(
		worker->getPosition(),
		BWAPI::Filter::IsEnemy &&
		!BWAPI::Filter::IsFlying &&
		(!BWAPI::Filter::IsMoving || BWAPI::Filter::IsStuck) &&
		BWAPI::Filter::IsDetected &&
		BWAPI::Filter::HP <= 35 &&
		!BWAPI::Filter::IsInvincible &&
		!BWAPI::Filter::IsSpell,
		2 * 32);
}

// The worker is defending itself and wants to mineral walk out of trouble.
// Find a suitable mineral patch, if any.
BWAPI::Unit WorkerManager::findEscapeMinerals(BWAPI::Unit worker) const
{
    BWAPI::Unit farthestMinerals = nullptr;
    int farthestDist = 64;           // ignore anything closer

    // Check all visible mineral patches.
    for (BWAPI::Unit unit : BWAPI::Broodwar->getNeutralUnits())
    {
        int dist;

        if (unit->getType() == BWAPI::UnitTypes::Resource_Mineral_Field &&
            (dist = worker->getDistance(unit)) < 400 &&
            dist > farthestDist)
        {
            farthestMinerals = unit;
            farthestDist = dist;
        }
    }

    return farthestMinerals;
}

// If appropriate, order the worker to defend itself.
// The "resource" is workerData.getWorkerResource(worker), passed in so it needn't be looked up again.
// Return whether self-defense was undertaken.
bool WorkerManager::defendSelf(BWAPI::Unit worker, BWAPI::Unit resource)
{
    // We want to defend ourselves if we are near home and we have a close enemy (the target).
    BWAPI::Unit target = findEnemyTargetForWorker(worker);

	if (resource && worker->getDistance(resource) < 200 && target)
	{
		the.micro.CatchAndAttackUnit(worker, target);
		makeBusy(worker);
		return true;
    }

    return false;
}

BWAPI::Unit WorkerManager::getClosestMineralWorkerTo(BWAPI::Unit enemyUnit)
{
    UAB_ASSERT(enemyUnit, "Unit was null");

    BWAPI::Unit closestMineralWorker = nullptr;
    int closestDist = 100000;

    // Former closest worker may have died or (if zerg) morphed into a building.
    if (UnitUtil::IsValidUnit(previousClosestWorker) &&
        previousClosestWorker->getType().isWorker() &&
        isFree(previousClosestWorker))
    {
        return previousClosestWorker;
    }

    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (isFree(worker)) 
        {
            int dist = worker->getDistance(enemyUnit);
            if (worker->isCarryingMinerals() || worker->isCarryingGas())
            {
                // If it has cargo, pretend it is farther away.
                // That way we prefer empty workers and lose less cargo.
                dist += 64;
            }

            if (dist < closestDist)
            {
                closestMineralWorker = worker;
                closestDist = dist;
            }
        }
    }

    previousClosestWorker = closestMineralWorker;
    return closestMineralWorker;
}

BWAPI::Unit WorkerManager::getWorkerScout()
{
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (workerData.getWorkerJob(worker) == WorkerData::Scout) 
        {
            return worker;
        }
    }

    return nullptr;
}

// Send the worker to mine minerals at the closest resource depot, if any.
void WorkerManager::setMineralWorker(BWAPI::Unit unit)
{
    UAB_ASSERT(unit, "Unit was null");

    auto assignNormalMinerals = [&](BWAPI::Unit depot) -> bool
    {
        if (!depot)
        {
            return false;
        }

        workerData.setWorkerJob(unit, WorkerData::Minerals, depot);
        if (workerData.getWorkerJob(unit) == WorkerData::Minerals &&
            workerData.getWorkerResource(unit))
        {
            remoteMineralWorkers.erase(unit);
            return true;
        }

        return false;
    };

    if (assignNormalMinerals(getClosestNonFullDepot(unit)))
    {
        return;
    }

    // If all normal mineral lines are saturated, oversaturation is still
    // better than leaving an SCV idle. Use the nearest owned depot that still
    // has mineable minerals.
    BWAPI::Unit fallbackDepot = nullptr;
    int fallbackDistance = MAX_DISTANCE;
    for (const Base* base : the.bases.getAll())
    {
        BWAPI::Unit candidate = base->getDepot();
        if (!candidate ||
            base->getOwner() != the.self() ||
            !UnitUtil::IsCompletedResourceDepot(candidate) ||
            base->getRemainingMinerals().empty())
        {
            continue;
        }

        const int distance = unit->getDistance(candidate);
        if (distance < fallbackDistance)
        {
            fallbackDistance = distance;
            fallbackDepot = candidate;
        }
    }

    if (assignNormalMinerals(fallbackDepot))
    {
        return;
    }

    // No owned mineral line can accept this SCV. Do actual long-distance
    // mining from a reachable neutral mineral base. The earlier implementation
    // only moved the SCV to the mineral line, so after arriving it had no gather
    // order and simply stood beside the crystals.
    BWAPI::Unit bestPatch = nullptr;
    int bestScore = MAX_DISTANCE;

    for (Base* base : the.bases.getAll())
    {
        if (!base ||
            base->getOwner() != BWAPI::Broodwar->neutral() ||
            base->getRemainingMinerals().empty() ||
            !the.bases.connectedToStart(base->getTilePosition()) ||
            !the.partitions.connected(unit->getPosition(), base->getCenter()))
        {
            continue;
        }

        for (BWAPI::Unit patch : base->getRemainingMinerals())
        {
            if (!patch || !patch->exists() || !patch->getPosition().isValid() ||
                !the.groundHitsFixed.safeToVisit(patch))
            {
                continue;
            }

            // Spread remote miners over the available patches instead of
            // sending every idle SCV to the same closest crystal.
            const int assigned = remoteWorkersOnPatch(patch);
            const int score = unit->getDistance(patch) + assigned * 12 * 32;
            if (score < bestScore)
            {
                bestScore = score;
                bestPatch = patch;
            }
        }
    }

    if (bestPatch)
    {
        RemoteMineralState& state = remoteMineralWorkers[unit];
        state.patch = bestPatch;
        state.lastCommandFrame = the.now();
        the.micro.MineMinerals(unit, bestPatch);
        makeBusy(unit);
        return;
    }

    remoteMineralWorkers.erase(unit);
}

// Immediately hand a released builder/repairer back to income. This is used
// after handleIdleWorkers() has already run in the current frame, so merely
// marking the worker Idle would leave it standing until a later update.
void WorkerManager::returnWorkerToEconomy(BWAPI::Unit unit)
{
    if (!unit || !unit->exists() ||
        unit->getPlayer() != the.self() ||
        !unit->getType().isWorker())
    {
        return;
    }

    workerData.setWorkerJob(unit, WorkerData::Idle, nullptr);
    remoteMineralWorkers.erase(unit);

    if (unit->isCarryingMinerals() || unit->isCarryingGas())
    {
        setReturnCargoWorker(unit);
    }
    else
    {
        setMineralWorker(unit);
    }
}

// Worker is carrying minerals or gas. Tell it to hand them in.
void WorkerManager::setReturnCargoWorker(BWAPI::Unit unit)
{
    UAB_ASSERT(unit, "Unit was null");

    BWAPI::Unit depot = getAnyClosestDepot(unit);

    if (depot)
    {
        workerData.setWorkerJob(unit, WorkerData::ReturnCargo, depot);
    }
    else
    {
        // BWAPI::Broodwar->printf("No depot to accept return cargo");
    }
}

// Get the closest resource depot with no other consideration.
// TODO iterate through bases, not units
BWAPI::Unit WorkerManager::getAnyClosestDepot(BWAPI::Unit worker)
{
    UAB_ASSERT(worker, "Worker was null");

    BWAPI::Unit closestDepot = nullptr;
    int closestDistance = 0;

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        UAB_ASSERT(unit, "Unit was null");

        if (UnitUtil::IsCompletedResourceDepot(unit))
        {
            int distance = unit->getDistance(worker);
            if (!closestDepot || distance < closestDistance)
            {
                closestDepot = unit;
                closestDistance = distance;
            }
        }
    }

    return closestDepot;
}

// Get the closest resource depot that can accept another mineral worker.
// The depot at a base under attack is treated as unavailable, unless it is the only base to mine at.
BWAPI::Unit WorkerManager::getClosestNonFullDepot(BWAPI::Unit worker)
{
    UAB_ASSERT(worker && worker->getType().isWorker(), "Worker was null");

    BWAPI::Unit closestDepot = nullptr;
    int closestDistance = MAX_DISTANCE;

    int nBases = the.bases.completedBaseCount(the.self());

    for (const Base * base : the.bases.getAll())
    {
        BWAPI::Unit depot = base->getDepot();

        if (depot)
        {
            const int distance = depot->getDistance(worker);                       // air distance
            const int travelTime = int(distance / worker->getType().topSpeed());   // lower limit

            if (base->getOwner() == the.self() &&
                distance < closestDistance &&
                UnitUtil::IsNearlyCompletedResourceDepot(depot, travelTime) &&     // should be complete when workers arrive
                (!base->inWorkerDanger() || nBases == 1) &&
                (distance <= ThisBaseRange || !expansionFerryNeededAt(base)) &&
                !workerData.depotIsFull(depot))
            {
                closestDepot = depot;
                closestDistance = distance;
            }
        }
    }

    return closestDepot;
}

// other managers that need workers call this when they're done with a unit
void WorkerManager::finishedWithWorker(BWAPI::Unit unit) 
{
    UAB_ASSERT(unit, "Unit was null");

    if (workerData.getWorkerJob(unit) == WorkerData::PostedBuild)
    {
        workerData.resetWorkerPost(unit, WorkerData::Posted);
    }
    else
    {
        workerData.setWorkerJob(unit, WorkerData::Idle, nullptr);
    }
}

// Find a worker to be reassigned to gas duty.
// Transferring across the map is bad. Try not to do that.
BWAPI::Unit WorkerManager::getGasWorker(BWAPI::Unit refinery)
{
    UAB_ASSERT(refinery, "Refinery was null");

    BWAPI::Unit localDepot = nullptr;
    int mineralFloor = 0;
    
    {
        for (const Base * base : the.bases.getAll())
            if (base->getOwner() == the.self() && base->getGeysers().count(refinery))
            {
                localDepot = base->getDepot();
                mineralFloor = std::min(6, 2 * int(base->getRemainingMinerals().size()));
                break;
            }
        if (!localDepot || !UnitUtil::IsCompletedResourceDepot(localDepot) || localDepot->isFlying())
            return nullptr;
    }

    BWAPI::Unit closestWorker = nullptr;
    int closestDistance = 0;
    bool workerInBase = false;

    for (BWAPI::Unit unit : workerData.getWorkers())
    {
        UAB_ASSERT(unit, "Unit was null");

        if (isFree(unit))
        {
            if (localDepot)
            {
                const WorkerData::WorkerJob job = workerData.getWorkerJob(unit);
                if (job == WorkerData::Minerals &&
                    (workerData.getWorkerDepot(unit) != localDepot ||
                     workerData.getNumAssignedWorkers(localDepot) <= mineralFloor)) continue;
                if (job == WorkerData::Idle &&
                    (unit->getDistance(localDepot) > ThisBaseRange ||
                     workerData.getNumAssignedWorkers(localDepot) < mineralFloor)) continue;
            }
            int distance = unit->getDistance(refinery);
            if (distance <= ThisBaseRange)
            {
                workerInBase = true;
            }

            // Don't waste minerals. It's OK (though unlikely) to already be carrying gas.
            if (unit->isCarryingMinerals() ||                       // doesn't have minerals and
                unit->getOrder() == BWAPI::Orders::MiningMinerals)  // isn't about to get them
            {
                continue;
            }

            if (!closestWorker || distance < closestDistance)
            {
                closestWorker = unit;
                closestDistance = distance;
            }
        }
    }

    if (closestWorker && closestDistance <= ThisBaseRange)
    {
        // This owrker is in the base and is free.
        return closestWorker;
    }

    if (workerInBase)
    {
        // There is a free worker in the base, but it has minerals
        // We'll wait until it returns the minerals.
        return nullptr;
    }

    // There are no free workers in the base. We may transfer one from elsewhere.
    return closestWorker;
}

// Return the closest free worker in the given range.
BWAPI::Unit	WorkerManager::getAnyWorker(const BWAPI::Position & pos, int range)
{
    BWAPI::Unit closestWorker = nullptr;
    int closestDistance = range;

    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (isFree(worker))
        {
            int distance = worker->getDistance(pos);
            if (distance < closestDistance)
            {
                closestWorker = worker;
                closestDistance = distance;
            }
        }
    }
    return closestWorker;
}

// Return the closest free worker in the given range which is not carrying resources.
BWAPI::Unit	WorkerManager::getUnencumberedWorker(const BWAPI::Position & pos, int range)
{
    BWAPI::Unit closestWorker = nullptr;
    int closestDistance = range;

    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (isFree(worker) &&
            !worker->isCarryingMinerals() &&
            !worker->isCarryingGas() &&
            worker->getOrder() != BWAPI::Orders::MiningMinerals)
        {
            int distance = worker->getDistance(pos);
            if (distance < closestDistance)
            {
                closestWorker = worker;
                closestDistance = distance;
            }
        }
    }
    return closestWorker;
}

// Return the closest free worker which is not carrying resources,
// and (when we can tell) has a safe path to the destination.
BWAPI::Unit	WorkerManager::getLongDistanceWorker(const BWAPI::Position & pos)
{
	// Figure out what base we want to build at, if any.
	Base * base = the.bases.findBaseByPosition(pos);

	BWAPI::Unit closestWorker = nullptr;
	int closestDistance = MAX_DISTANCE;
	BWAPI::Unit miningWorker = nullptr;
	int miningDistance = MAX_DISTANCE;

	for (BWAPI::Unit worker : workerData.getWorkers())
	{
		if (isFree(worker) &&
			!worker->isCarryingMinerals() &&
			!worker->isCarryingGas())
		{
			// Can the worker (currently) get there without running into the enemy?
			// We only check if we found a closest base.
			const bool safeGridRoute =
				!base || base->getSafeTileDistance(worker->getTilePosition()) >= 0;
			const bool ownWallIsOnlyRouteBlock =
				!safeGridRoute && base &&
				the.placer.isTerranMainWallComplete(true) &&
				the.partitions.connected(worker->getPosition(), pos) &&
				the.placer.terranMainWallTransitWaypoint(
					worker->getPosition(), pos).isValid();
			if (safeGridRoute || ownWallIsOnlyRouteBlock)
			{
				int distance = worker->getDistance(pos);
				if (worker->getOrder() == BWAPI::Orders::MiningMinerals)
				{
					// Mining is the normal state of a healthy economy, not a
					// reason to rule out every expansion builder. Prefer an
					// idle SCV, but allow a mining SCV when none exists.
					if (distance < miningDistance)
					{
						miningWorker = worker;
						miningDistance = distance;
					}
				}
				else if (distance < closestDistance)
				{
					closestWorker = worker;
					closestDistance = distance;
				}
			}
		}
	}
	return closestWorker ? closestWorker : miningWorker;
}

// Return the posted worker whose post is closest to the given position,
// if any is reasonably close.
BWAPI::Unit WorkerManager::getPostedWorker(const BWAPI::Position & pos)
{
    BWAPI::Unit closestWorker = nullptr;
    int closestDistance = 14 * 32;      // be closer than this

    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (workerData.getWorkerJob(worker) == WorkerData::Posted)
        {
            BWAPI::Position post = workerData.getWorkerPostPosition(worker);
            int distance = pos.getApproxDistance(post);
            if (distance < closestDistance)
            {
                closestWorker = worker;
                closestDistance = distance;
            }
        }
    }
    return closestWorker;
}

// There is at least one posted worker which is busy building something.
// Call this only after getPostedWorker() fails to find any free posted worker,
// to decide whether to wait for a busy posted worker to become free again.
bool WorkerManager::postedWorkerBusy(const BWAPI::Position & pos)
{
    BWAPI::Unit closestWorker = nullptr;
    int closestDistance = 12 * 32;      // be closer than this

    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (workerData.getWorkerJob(worker) == WorkerData::PostedBuild)
        {
            BWAPI::Position post = workerData.getWorkerPostPosition(worker);
            int distance = pos.getApproxDistance(post);
            if (distance < closestDistance)
            {
                return true;
            }
        }
    }
    return false;
}

// Get a builder for BuildingManager, based on the building's final or desired position.
// It is the caller's job to setBuildWorker() (or not, as needed).
// Reject workers carrying resources, unless we are protoss. We can wait.
BWAPI::Unit WorkerManager::getBuilder(const Building & b)
{
    // 1. If this is a gas steal, return the scout worker, or null if none.
    if (b.isGasSteal)
    {
        return ScoutManager::Instance().getWorkerScout();
    }

    // 2. Return a free worker which is posted near the target position.
    const BWAPI::Position pos(
        b.finalPosition.isValid() ? b.finalPosition : b.desiredPosition
    );
    UAB_ASSERT(pos.isValid(), "bad position");

    BWAPI::Unit builder = getPostedWorker(pos);
    if (builder)
    {
        return builder;
    }

    // 3. If there is a busy worker posted near the position, wait for now.
    // We'll assign that worker when it becomes free (or move on if it dies).
    if (b.status != BuildingStatus::UnderConstruction &&
        b.type != BWAPI::UnitTypes::Terran_Bunker &&
        postedWorkerBusy(pos))
    {
        return nullptr;
    }

    // 4. Return a worker which is close enough to be "at this base".
    builder = getUnencumberedWorker(pos, ThisBaseRange);
	if (builder)
	{
		return builder;
	}

    // MiningMinerals can occupy almost every frame of a healthy mineral line.
    // An affordable Terran structure must be able to take a nearby, empty-handed
    // mineral worker instead of waiting indefinitely for a different order.
    if (b.type.getRace() == BWAPI::Races::Terran &&
        b.status != BuildingStatus::UnderConstruction)
    {
        BWAPI::Unit closest = nullptr;
        int closestDistance = ThisBaseRange;
        for (BWAPI::Unit worker : workerData.getWorkers())
        {
            if (!isFree(worker) || worker->isCarryingMinerals() ||
                worker->isCarryingGas() || !worker->getPosition().isValid() ||
                !the.partitions.connected(worker->getPosition(), pos))
            {
                continue;
            }
            const int distance = worker->getDistance(pos);
            if (distance < closestDistance)
            {
                closest = worker;
                closestDistance = distance;
            }
        }
        if (closest)
        {
            return closest;
        }
    }

    // A Terran building already exists and needs an SCV now. A worker mining
    // nearby is suitable as soon as it has unloaded its cargo, even though
    // getUnencumberedWorker() rejects the MiningMinerals order. Otherwise the
    // following "worker at this base" wait can last forever.
    if (b.status == BuildingStatus::UnderConstruction &&
        b.type.getRace() == BWAPI::Races::Terran)
    {
        BWAPI::Unit closest = nullptr;
        int closestDistance = ThisBaseRange;
        for (BWAPI::Unit worker : workerData.getWorkers())
        {
            if (!isFree(worker) || worker->isCarryingMinerals() ||
                worker->isCarryingGas())
            {
                continue;
            }
            const int distance = worker->getDistance(pos);
            if (distance < closestDistance)
            {
                closest = worker;
                closestDistance = distance;
            }
        }
        if (closest)
        {
            return closest;
        }
    }
	

    // 5. If any worker is at this base, we're done for now.
    // We'll wait for the worker to return its cargo and select it on a later frame.
	if (getAnyWorker(pos, ThisBaseRange))
    {
        return nullptr;
    }

    // 6. This base seems to be barren of workers. Return a worker from anywhere.
    // In this case, we try to check whether the enemy can intercept the worker.
    builder = getLongDistanceWorker(pos);
    if (builder)
    {
        return builder;
    }

    // Expansion deadlock fallback. A mineral SCV spends most frames in the
    // MiningMinerals order, so the strict long-distance selector can miss all
    // workers indefinitely. Only for resource depots, allow a non-carrying
    // mineral worker with a real ground path to become the builder.
    if (b.type.isResourceDepot())
    {
        BWAPI::Unit closest = nullptr;
        int closestDistance = MAX_DISTANCE;
        for (BWAPI::Unit worker : workerData.getWorkers())
        {
            if (!isFree(worker) ||
                worker->isCarryingMinerals() ||
                worker->isCarryingGas() ||
                !worker->getPosition().isValid() ||
                !the.partitions.connected(worker->getPosition(), pos))
            {
                continue;
            }

            const int distance = worker->getDistance(pos);
            if (distance < closestDistance)
            {
                closestDistance = distance;
                closest = worker;
            }
        }
        return closest;
    }

    return nullptr;
}

// Called by outsiders to assign a worker to a construction job.
void WorkerManager::setBuildWorker(BWAPI::Unit worker)
{
    UAB_ASSERT(worker, "Worker was null");

    if (workerData.getWorkerJob(worker) == WorkerData::Build ||
        workerData.getWorkerJob(worker) == WorkerData::PostedBuild)
    {
        // This is already a build worker. Do nothing.
    }
    else if (workerData.getWorkerJob(worker) == WorkerData::Posted)
    {
        workerData.resetWorkerPost(worker, WorkerData::PostedBuild);
    }
    else
    {
        workerData.setWorkerJob(worker, WorkerData::Build);
    }
}

void WorkerManager::setScoutWorker(BWAPI::Unit worker)
{
    UAB_ASSERT(worker, "Worker was null");

    workerData.setWorkerJob(worker, WorkerData::Scout, nullptr);
}

// Will we have the required resources by the time a worker can travel the given distance?
bool WorkerManager::willHaveResources(int mineralsRequired, int gasRequired, double distance)
{
    // if we don't require anything, we will have it
    if (mineralsRequired <= 0 && gasRequired <= 0)
    {
        return true;
    }

    double speed = the.self()->getRace().getWorker().topSpeed();

    // how many frames it will take us to move to the building location
    // add a little to account for worker getting stuck. better early than late
    double framesToMove = (distance / speed) + 24;

    // magic numbers to predict income rates
    double mineralRate = getNumMineralWorkers() * 0.045;
    double gasRate     = getNumGasWorkers() * 0.07;

    // calculate if we will have enough by the time the worker gets there
    return
        mineralRate * framesToMove >= mineralsRequired &&
        gasRate * framesToMove >= gasRequired;
}

void WorkerManager::setCombatWorker(BWAPI::Unit worker)
{
    UAB_ASSERT(worker, "Worker was null");

    workerData.setWorkerJob(worker, WorkerData::Combat, nullptr);
}

// Post the given worker to the given macro location.
void WorkerManager::postGivenWorker(BWAPI::Unit worker, MacroLocation loc)
{
    //BWAPI::Broodwar->printf("posting worker %d", worker->getID());
    workerData.setWorkerPost(worker, loc);
}

// Post the closest free worker to the given macro location.
// In case of failure (which should be rare), do nothing and return null.
BWAPI::Unit WorkerManager::postWorker(MacroLocation loc)
{
    BWAPI::Position pos = the.placer.getMacroLocationPos(loc);
    UAB_ASSERT(pos.isValid(), "post to bad macroloc");

    BWAPI::Unit worker = getUnencumberedWorker(pos, MAX_DISTANCE);
    if (!worker)
    {
        worker = getAnyWorker(pos, MAX_DISTANCE);
    }

    if (worker)
    {
        // BWAPI::Broodwar->printf("posting worker %d", worker->getID());
        postGivenWorker(worker, loc);
    }
    return worker;
}

// Release all workers at the given macro location from their posts.
// If the location is Anywhere (the default), release all posted workers.
void WorkerManager::unpostWorkers(MacroLocation loc)
{
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (loc == MacroLocation::Anywhere || loc == workerData.getWorkerPostLocation(worker))
        {
            WorkerData::WorkerJob job = workerData.getWorkerJob(worker);
            if (job == WorkerData::Posted)
            {
                //BWAPI::Broodwar->printf("unposting worker %d", worker->getID());
                workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
            }
            else if (job == WorkerData::PostedBuild)
            {
                //BWAPI::Broodwar->printf("unposting build worker %d", worker->getID());
                workerData.setWorkerJob(worker, WorkerData::Build);
            }
        }
    }
}

void WorkerManager::onUnitMorph(BWAPI::Unit unit)
{
    UAB_ASSERT(unit, "Unit was null");

    // if something morphs into a worker, add it
    if (unit->getType().isWorker() && unit->getPlayer() == the.self() && unit->getHitPoints() > 0)
    {
        workerData.addWorker(unit);
    }

    // if something morphs into a building, was it a drone?
    if (unit->getType().isBuilding() && unit->getPlayer() == the.self() && unit->getPlayer()->getRace() == BWAPI::Races::Zerg)
    {
        workerData.workerDestroyed(unit);
    }
}

void WorkerManager::onUnitShow(BWAPI::Unit unit)
{
    UAB_ASSERT(unit && unit->exists(), "bad unit");

    if (unit->getType().isWorker() && unit->getPlayer() == the.self() && unit->getHitPoints() > 0 &&
        workerData.getWorkers().find(unit) == workerData.getWorkers().end())
    {
        // A BWAPI unit can generate both Create and Show notifications. Do not
        // re-add an already tracked SCV: addWorker() deliberately initializes
        // its job to Idle, which would otherwise erase a mining/building job.
        workerData.addWorker(unit);
    }
}

void WorkerManager::onUnitDestroy(BWAPI::Unit unit)
{
    UAB_ASSERT(unit, "Unit was null");

    if (unit->getType().isResourceDepot() && unit->getPlayer() == the.self())
    {
        workerData.removeDepot(unit);
    }

    if (unit->getType().isWorker() && unit->getPlayer() == the.self())
    {
        remoteMineralWorkers.erase(unit);
        workerData.workerDestroyed(unit);
    }

    if (unit->getType() == BWAPI::UnitTypes::Resource_Mineral_Field)
    {
        rebalanceWorkers();
    }
}

// Possibly transfer workers to other bases.
// Prefer balancing mineral saturation directly instead of marking a worker Idle:
// an idle worker normally chooses the nearest non-full depot, which is commonly
// the same depot it just left and therefore does not perform a real transfer.
void WorkerManager::rebalanceWorkers()
{
    // First repair stale mineral assignments whose depot disappeared.
    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (workerData.getWorkerJob(worker) != WorkerData::Minerals)
        {
            continue;
        }

        BWAPI::Unit depot = workerData.getWorkerDepot(worker);

        if (!depot || !depot->exists())
        {
            // BWAPI::Broodwar->printf("resource depot gone");
            workerData.setWorkerJob(worker, WorkerData::Idle, nullptr);
        }
    }

    struct MineralBaseState
    {
        Base * base;
        BWAPI::Unit depot;
        int workers;
        int capacity;
        double saturation;
    };

    std::vector<MineralBaseState> states;
    states.reserve(the.bases.getAll().size());

    for (Base * base : the.bases.getAll())
    {
        if (!base || base->getOwner() != the.self())
        {
            continue;
        }

        BWAPI::Unit depot = base->getDepot();
        if (!depot || !UnitUtil::IsCompletedResourceDepot(depot) || depot->isFlying())
        {
            continue;
        }

        // WorkerData's mineral locking is designed around two workers per
        // patch.  Use the currently visible mineral count so the target shrinks
        // naturally as a base mines out, rather than Base::getMaxMineralWorkers()
        // (which follows the configurable three-workers-per-patch macro cap).
        const int patches = workerData.getMineralsNearDepot(depot);
        if (patches <= 0)
        {
            continue;
        }

        const int capacity = std::max(1, 2 * patches);
        const int workers = workerData.getNumAssignedWorkers(depot);
        states.push_back({ base, depot, workers, capacity,
            double(workers) / double(capacity) });
    }

    if (states.size() < 2)
    {
        return;
    }

    MineralBaseState * target = nullptr;
    for (MineralBaseState & state : states)
    {
        if (state.workers >= state.capacity || state.base->inWorkerDanger())
        {
            continue;
        }

        if (expansionFerryNeededAt(state.base))
        {
            continue;
        }

        // With a main-choke Terran wall, the natural is deliberately staged:
        // the Command Center may finish before the army has crossed the gate.
        // Do not send isolated SCVs until the organized defense is in position.
        if (the.selfRace() == BWAPI::Races::Terran &&
            state.base == the.bases.myNatural() &&
            !ProductionManager::Instance().terranNaturalTransferReady())
        {
            continue;
        }

        if (!target || state.saturation < target->saturation)
        {
            target = &state;
        }
    }

    if (!target)
    {
        return;
    }

    MineralBaseState * source = nullptr;
    for (MineralBaseState & state : states)
    {
        if (&state == target || state.workers <= 1)
        {
            continue;
        }

        // A transfer must be possible on the ground.  This avoids ordering an
        // SCV to walk to an island expansion or across a disconnected map area.
        if (the.map.getGroundDistance(state.depot->getPosition(), target->depot->getPosition()) < 0 ||
            (state.base->getGroundSafePaths().isInitialized() &&
             state.base->getSafeTileDistance(target->base->getTilePosition()) < 0))
        {
            continue;
        }

        if (!source || state.saturation > source->saturation)
        {
            source = &state;
        }
    }

    if (!source)
    {
        return;
    }

    // Do not shuffle workers for tiny differences.  The exception is a nearly
    // empty expansion: giving it its first worker layer has a much higher
    // marginal mineral return than keeping second-layer workers at the main.
    const int targetFirstLayer = std::max(1, target->capacity / 2);
    const bool targetNeedsFirstLayer = target->workers < targetFirstLayer;
    if (!targetNeedsFirstLayer && source->saturation - target->saturation < 0.12)
    {
        return;
    }

    // Do not strip the source below its first worker layer.  Once all bases have
    // one worker per patch, newly produced SCVs and later balancing can fill the
    // second layer smoothly.
    const int sourceFirstLayer = std::max(1, source->capacity / 2);
    if (source->workers <= sourceFirstLayer)
    {
        return;
    }

    BWAPI::Unit transfer = nullptr;
    int bestDistance = MAX_DISTANCE;
    bool foundWithoutCargo = false;

    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        if (!worker || !worker->exists() || !worker->isCompleted() ||
            workerData.getWorkerJob(worker) != WorkerData::Minerals ||
            workerData.getWorkerDepot(worker) != source->depot)
        {
            continue;
        }

        const bool withoutCargo = !worker->isCarryingMinerals() && !worker->isCarryingGas();
        if (foundWithoutCargo && !withoutCargo)
        {
            continue;
        }

        const int distance = worker->getDistance(target->depot);
        if (!transfer || (withoutCargo && !foundWithoutCargo) || distance < bestDistance)
        {
            transfer = worker;
            bestDistance = distance;
            foundWithoutCargo = withoutCargo;
        }
    }

    if (transfer)
    {
        workerData.setWorkerJob(transfer, WorkerData::Minerals, target->depot);
    }
}

void WorkerManager::drawWorkerInformation() 
{
    if (!Config::Debug::DrawWorkerInfo)
    {
        return;
    }

    for (BWAPI::Unit worker : workerData.getWorkers())
    {
        BWAPI::Position pos = worker->getPosition();
        if (pos.isValid())
        {
            int color = isBusy(worker) ? orange : cyan;
            BWAPI::Broodwar->drawTextMap(pos.x - 4, pos.y - 7, "%c%c", color, workerData.getJobCode(worker));

            BWAPI::Position target = worker->getTargetPosition();
            if (target.isValid())
            {
                BWAPI::Broodwar->drawLineMap(pos, target, BWAPI::Colors::Brown);
            }

            BWAPI::Unit depot = workerData.getWorkerDepot(worker);
            if (depot)
            {
                BWAPI::Broodwar->drawLineMap(pos, depot->getPosition() /*+ BWAPI::Position(32, 24)*/, BWAPI::Colors::Grey);
            }
        }
    }
}

bool WorkerManager::isFree(BWAPI::Unit worker)
{
    UAB_ASSERT(worker, "Worker was null");

    WorkerData::WorkerJob job = workerData.getWorkerJob(worker);
    return
        (job == WorkerData::Minerals || job == WorkerData::Idle && !(worker->isBurrowed() || worker->getOrder() == BWAPI::Orders::Burrowing)) &&
        !isBusy(worker) &&
        worker->isCompleted();
}

bool WorkerManager::isMobileRepairTarget(BWAPI::Unit unit) const
{
    return unit && unit->exists() &&
        _mobileRepairPositions.find(unit->getID()) !=
            _mobileRepairPositions.end();
}

BWAPI::Position WorkerManager::getMobileRepairPosition(BWAPI::Unit unit) const
{
    if (!unit || !unit->exists())
    {
        return BWAPI::Positions::Invalid;
    }
    const auto found = _mobileRepairPositions.find(unit->getID());
    return found == _mobileRepairPositions.end()
        ? BWAPI::Positions::Invalid : found->second;
}

bool WorkerManager::isWorkerScout(BWAPI::Unit worker)
{
    UAB_ASSERT(worker, "Worker was null");

    return workerData.getWorkerJob(worker) == WorkerData::Scout;
}

bool WorkerManager::isCombatWorker(BWAPI::Unit worker)
{
    UAB_ASSERT(worker, "Worker was null");

    return workerData.getWorkerJob(worker) == WorkerData::Combat;
}

bool WorkerManager::isBuilder(BWAPI::Unit worker)
{
    UAB_ASSERT(worker, "Worker was null");

    return workerData.getWorkerJob(worker) == WorkerData::Build;
}

int WorkerManager::getNumMineralWorkers() const
{
    return workerData.getNumMineralWorkers();	
}

int WorkerManager::getNumGasWorkers() const
{
    return workerData.getNumGasWorkers();
}

int WorkerManager::getNumReturnCargoWorkers() const
{
    return workerData.getNumReturnCargoWorkers();
}

int WorkerManager::getNumCombatWorkers() const
{
    return workerData.getNumCombatWorkers();
}

int WorkerManager::getNumIdleWorkers() const
{
    return workerData.getNumIdleWorkers();
}

int WorkerManager::getNumRemoteMineralWorkers() const
{
    int count = 0;
    for (const auto& entry : remoteMineralWorkers)
    {
        BWAPI::Unit worker = entry.first;
        BWAPI::Unit patch = entry.second.patch;
        if (worker && worker->exists() && patch && patch->exists() &&
            workerData.getWorkerJob(worker) == WorkerData::Idle)
        {
            ++count;
        }
    }
    return count;
}

int WorkerManager::getNumPostedWorkers() const
{
    return workerData.getNumPostedWorkers();
}

// The largest number of workers that it is efficient to have right now.
// Does not take into account possible preparations for future expansions.
// May not exceed Config::Macro::AbsoluteMaxWorkers.
int WorkerManager::getMaxWorkers() const
{
    int patches = the.bases.remainingMineralPatchCount();
    int refineries, geysers;
    the.bases.gasCounts(refineries, geysers);

    // Match WorkerData::depotIsFull() and Base::getMaxMineralWorkers(). With
    // mineral locking, producing toward the old default of three workers per
    // patch created surplus SCVs after every depot had already refused a third
    // mineral worker. Keep one additional worker as the existing builder/future
    // expansion reserve below.
    const double workersPerPatch =
        std::min(2.0, Config::Macro::WorkersPerPatch);

    // Never let the max number of workers fall to 0!
    // Set aside 1 for future opportunities.
    // SCV_MAX is a ceiling, not a production target. The live patch/refinery
    // capacity below normally stops Terran well before 68 on two bases. Keep
    // the shared configuration unchanged for Protoss/Zerg.
    const int absoluteWorkerCap = the.selfRace() == BWAPI::Races::Terran
        ? std::min(68, Config::Macro::AbsoluteMaxWorkers)
        : Config::Macro::AbsoluteMaxWorkers;
    return std::min(
            absoluteWorkerCap,
            1 + int(std::round(workersPerPatch * patches + Config::Macro::WorkersPerRefinery * refineries))
        );
}

// The number of workers assigned to this resource depot to mine minerals, or to this refinery to mine gas.
int WorkerManager::getNumWorkers(BWAPI::Unit jobUnit) const
{
    return workerData.getNumAssignedWorkers(jobUnit);
}
