#include "Intelligence/ScoutManager.h"

#include "Map/Bases.h"
#include "Core/GameCommander.h"
#include "Map/MapGrid.h"
#include "Map/MapTools.h"
#include "Combat/Micro/Micro.h"
#include "Intelligence/Learning/OpponentModel.h"
#include "Production/ProductionManager.h"
#include "Core/The.h"

// This class is responsible for early game scouting.
// It controls the assigned SCV scout.

using namespace UAlbertaBot;

ScoutManager::ScoutManager() 
    : _workerScout(nullptr)
    , _scoutStatus("None")
    , _gasStealStatus("None")
    , _scoutCommand(MacroCommandType::None)
    , _scoutUnderAttack(false)
    , _tryGasSteal(false)
    , _enemyGeyser(nullptr)
    , _startedGasSteal(false)
    , _queuedGasSteal(false)
    , _gasStealOver(false)
    , _previousScoutHP(0)
    , _nextDestination(BWAPI::Positions::Invalid)
	, _workerGoHome(false)
{
    setScoutTargets();
}

ScoutManager & ScoutManager::Instance() 
{
    static ScoutManager instance;
    return instance;
}

// Target locations are set while we are still looking for the enemy base.
// After the enemy base is found, we unset the targets and go to or ignore the enemy base as appropriate.

// Guarantee: We only set a target if the scout for the target is set.
void ScoutManager::setScoutTargets()
{
    Base * enemyBase = the.bases.enemyStart();
    if (enemyBase && the.bases.enemyStartConfirmed())
    {
        _workerScoutTarget = nullptr;
        return;
    }
    if (enemyBase)
    {
        // An inferred start still needs direct confirmation of its depot footprint.
        _workerScoutTarget = _workerScout ? enemyBase : nullptr;
        return;
    }
    if (!_workerScout) _workerScoutTarget = nullptr;
    for (Base * base : the.bases.getStarting())
    {
        if (the.bases.startLocationScouted(base) && _workerScoutTarget == base)
            _workerScoutTarget = nullptr;
    }
    for (Base * base : the.bases.getStarting())
    {
        if (!the.bases.startLocationScouted(base) && _workerScout && !_workerScoutTarget)
            _workerScoutTarget = base;
    }
}

// Should we send out a worker scout now?
bool ScoutManager::shouldScout()
{
    // If we're stealing gas, it doesn't matter what the scout command is: We need to send a worker.
    if (wantGasSteal())
    {
        return true;
    }

    if (_scoutCommand == MacroCommandType::None)
    {
        return false;
    }

    // If we only want to find the enemy base location and we already know it, don't send a worker.
    if (_scoutCommand == MacroCommandType::ScoutIfNeeded || _scoutCommand == MacroCommandType::ScoutLocation)
    {
        return !the.bases.enemyStartConfirmed();
    }

    return true;
}

void ScoutManager::update()
{
    // If we're not scouting now, minimum effort.
    if (!_workerScout)
    {
        return;
    }

    // If a scout is gone, admit it.
    if (_workerScout &&
        (!_workerScout->exists() || _workerScout->getHitPoints() <= 0 ||     // it died (or became a zerg extractor)
        _workerScout->getPlayer() != the.self()))                            // it got mind controlled!
    {
        _scoutStatus = "worker scout gone";
        _workerScout = nullptr;
    }
    

    
    

    // If we only want to locate the enemy base and we have, send the worker home.
    if (_scoutCommand == MacroCommandType::ScoutLocation &&
        the.bases.enemyStartConfirmed() &&
        !wantGasSteal())
    {
        _scoutStatus = "enemy base located";
		sendWorkerScoutHome();
    }

    // If we're done with a gas steal and we don't want to keep scouting, send the worker home.
    // If we're zerg, the worker may have turned into an extractor. That is handled elsewhere.
    if (_scoutCommand == MacroCommandType::None && _gasStealOver)
    {
        _scoutStatus = "gas steal over";
		sendWorkerScoutHome();
    }

    // Release the worker if it can no longer help.
    if (_workerScout && releaseScoutEarly(_workerScout))
    {
        _scoutStatus = "worker sent home early";
		sendWorkerScoutHome();
    }

    // Do the actual scouting. Also steal gas if called for.
    setScoutTargets();
    if (_workerScout)
    {
		if (_workerGoHome)
		{
			workerGoHome();
		}
		else
		{
			bool moveScout = true;
			if (wantGasSteal())              // implies !_gasStealOver
			{
				if (gasSteal())
				{
					moveScout = false;
				}
				else if (_queuedGasSteal)    // gas steal queued but not finished
				{
					// We're in the midst of stealing gas. Let BuildingManager control the worker.
					moveScout = false;
					_gasStealStatus = "Stealing gas";
				}
			}
			else
			{
				if (_gasStealOver)
				{
					_gasStealStatus = "None or done";
				}
				else
				{
					_gasStealStatus = "Not planned";
				}
			}
			if (moveScout)
			{
				moveGroundScout();
			}
		}
    }
    else if (_gasStealOver)
    {
        // We get here if we're stealing gas as zerg when the worker turns into an extractor.
        _gasStealStatus = "Finished or failed";
    }

    

    drawScoutInformation(200, 320);
}

// If zerg, our first overlord is used to scout immediately at the start of the game.


// The worker scout is assigned only when it is time to go scout.
void ScoutManager::setWorkerScout(BWAPI::Unit unit)
{
    releaseWorkerScout();

    _workerScout = unit;
    WorkerManager::Instance().setScoutWorker(_workerScout);
}

// The worker scout's job is done. Set the flag to tell it to go home.
// If it can't get home, it will keep scouting wherever it can.
void ScoutManager::sendWorkerScoutHome()
{
	// This is OK whether the _workerScout is set or not.
	_gasStealOver = true;
	_nextDestination = BWAPI::Positions::None;
	_workerGoHome = true;
}

// Release the worker scout for other duties.
void ScoutManager::releaseWorkerScout()
{
	_gasStealOver = true;
	if (_workerScout)
    {
		WorkerManager::Instance().finishedWithWorker(_workerScout);
        _workerScout = nullptr;
    }
}

// The scouting overlord is near enemy static air defense.


// Send the overlord scout home.


void ScoutManager::setScoutCommand(MacroCommandType cmd)
{
    UAB_ASSERT(
        cmd == MacroCommandType::Scout ||
        cmd == MacroCommandType::ScoutIfNeeded ||
        cmd == MacroCommandType::ScoutLocation ||
        cmd == MacroCommandType::ScoutOnceOnly,
        "bad scout command");

    _scoutCommand = cmd;
}

void ScoutManager::drawScoutInformation(int x, int y)
{
    if (!Config::Debug::DrawScoutInfo)
    {
        return;
    }

    BWAPI::Broodwar->drawTextScreen(x, y, "Scout info: %s", _scoutStatus.c_str());
    BWAPI::Broodwar->drawTextScreen(x, y+10, "Gas steal: %s", _gasStealStatus.c_str());
    std::string more = "not yet";
    if (_scoutCommand == MacroCommandType::Scout)
    {
        more = "and stay";
    }
    else if (_scoutCommand == MacroCommandType::ScoutLocation)
    {
        more = "location";
    }
    else if (_scoutCommand == MacroCommandType::ScoutOnceOnly)
    {
        more = "once around";
    }
    else if (_scoutCommand == MacroCommandType::ScoutWhileSafe)
    {
        more = "while safe";
    }
    else if (wantGasSteal())
    {
        more = "to steal gas";
    }
    // NOTE "go scout if needed" doesn't need to be represented here.
    BWAPI::Broodwar->drawTextScreen(x, y + 20, "Go scout: %s", more.c_str());

    if (_workerScout && _nextDestination.isValid())
    {
        BWAPI::Broodwar->drawLineMap(_workerScout->getPosition(), _nextDestination, BWAPI::Colors::Green);
    }
}

// Move the worker scout.
void ScoutManager::moveGroundScout()
{
    // Once the enemy start is known, stay local to the actual main. The old
    // 30-tile radius became far too broad after high-level Areas were merged:
    // a worker could treat a nearby expansion in the same Area as "inside the
    // enemy base" and wander away from the tech/buildings it was meant to see.
    const int scoutDistanceThreshold = 16;    // in tiles

    if (_workerScoutTarget)
    {
        // The target is valid exactly when we are still looking for the enemy base.
        _scoutStatus = "Seeking enemy base";

		the.micro.MoveToBase(_workerScout, _workerScoutTarget, _workerScoutTarget->getPosition(), true);
    }
    else
    {
		// The classic code with harassment and fleeing features that don't work well.
        const Base * enemyBase = the.bases.enemyStart();

        UAB_ASSERT(enemyBase, "no enemy base");

        int scoutDistanceToEnemy = the.map.getGroundTileDistance(_workerScout->getPosition(), enemyBase->getCenter());
        bool scoutInRangeOfenemy = scoutDistanceToEnemy <= scoutDistanceThreshold;

        int scoutHP = _workerScout->getHitPoints() + _workerScout->getShields();
        if (scoutHP < _previousScoutHP)
        {
            _scoutUnderAttack = true;
        }
        _previousScoutHP = scoutHP;

        if (!_workerScout->isUnderAttack() && !enemyWorkerInRadius())
        {
            _scoutUnderAttack = false;
        }

        if (scoutInRangeOfenemy)
        {
            if (_scoutUnderAttack)
            {
                _scoutStatus = "Under attack, fleeing";
                followGroundPath();
            }
            else
            {
                BWAPI::Unit closestWorker = enemyWorkerToHarass();

                // If configured and reasonable, harass an enemy worker.
                if (Config::Skills::ScoutHarassEnemy && closestWorker &&
                    !wantGasSteal() &&
                    _workerScout->getHitPoints() + _workerScout->getShields() > 20)
                {
                    _scoutStatus = "Harass enemy worker";
                    the.micro.CatchAndAttackUnit(_workerScout, closestWorker);
                }
                // otherwise keep circling the enemy region
                else
                {
                    _scoutStatus = "Scouting interior";
                    followGroundPath();
                }
            }
        }
        // if the scout is not in the enemy region
        else if (_scoutUnderAttack)
        {
            _scoutStatus = "Under attack, fleeing";
            followGroundPath();
        }
        else
        {
            _scoutStatus = "Enemy located, going there";
            followGroundPath();
        }
    }
}

// Explore inside the enemy base. Or, if not there yet, move toward it.
// NOTE This may release the worker scout if scouting is complete!
void ScoutManager::followGroundPath()
{
    // Called only after the enemy base is found.
    Base * enemyBase = the.bases.enemyStart();

    const int distanceToEnemyMain =
        the.map.getGroundTileDistance(
            _workerScout->getPosition(),
            enemyBase->getCenter());

    if (distanceToEnemyMain < 0 || distanceToEnemyMain > 16)
    {
        // High-level Areas can legitimately contain both a main and a nearby
        // expansion. "Same Area" therefore does not mean "still scouting the
        // main". Return to the real enemy start whenever we drift too far.
        the.micro.MoveToBase(
            _workerScout,
            enemyBase,
            enemyBase->getCenter(),
            true);
        return;
    }

    // NOTE Sight range of a worker is 224, except a probe which has 256.
    if (_nextDestination.isValid() && _workerScout->getDistance(_nextDestination) > 96)
    {
        // We're still a fair distance from the next waypoint. Stay the course.
        if (Config::Debug::DrawScoutInfo)
        {
            BWAPI::Broodwar->drawCircleMap(_nextDestination, 3, BWAPI::Colors::Yellow, true);
            BWAPI::Broodwar->drawLineMap(_workerScout->getPosition(), _nextDestination, BWAPI::Colors::Yellow);
        }
		the.micro.MoveToBase(_workerScout, enemyBase, _nextDestination, true);
		return;
    }

    // We're at the enemy base and need another waypoint.
    BWAPI::Position destination =
        the.grid.getLeastExploredNear(
            enemyBase->getPosition(),
            true,
            false,
            14 * 32);
    if (destination.isValid())
    {
        if (_scoutCommand == MacroCommandType::ScoutOnceOnly && !wantGasSteal())
        {
            if (BWAPI::Broodwar->isExplored(BWAPI::TilePosition(destination)))
            {
                sendWorkerScoutHome();
                return;
            }
        }
		_nextDestination = destination;
	}
    else
    {
        _nextDestination = enemyBase->getCenter();
    }
	the.micro.MoveToBase(_workerScout, enemyBase, _nextDestination, true);
}

// The worker scout has finished its job. It should go home if it can.
// If it can't go home, it should keep scouting wherever it can.
void ScoutManager::workerGoHome()
{
	UAB_ASSERT(_workerScout && _workerGoHome, "can't go home");

	// 1. Find a friendly base that it looks like the worker can reach.
	Base * home = the.bases.myFront()->getSafeTileDistance(_workerScout->getTilePosition()) >= 0
		? the.bases.myFront()
		: nullptr;

	if (!home)
	{
		// Maybe we can get to some other friendly base.
		for (Base * base : the.bases.getAll())
		{
			if (base->getOwner() == the.self() && base->getSafeTileDistance(_workerScout->getTilePosition()) >= 0)
			{
				home = base;
				break;
			}
		}
	}

	// We found a home base to return to. Go there.
	if (home)
	{
		if (home->getSafeTileDistance(_workerScout->getTilePosition()) <= 14)
		{
			// We're there.
			releaseWorkerScout();
		}
		else
		{
			_scoutStatus = "Going home";
			the.micro.MoveToBase(_workerScout, home, home->getPosition(), true);
			if (Config::Debug::DrawScoutInfo)
			{
				BWAPI::Broodwar->drawLineMap(_workerScout->getPosition(), home->getPosition(), BWAPI::Colors::Grey);
			}
		}
		return;
	}

	// 2. We can't find a safe way home. Keep scouting.
	// NOTE This can happen because we're already under fire. If here is not safe, no path from here is safe.
	if (the.bases.enemyStart())
	{
		_scoutStatus = "Can't get home, scouting";
		followGroundPath();
	}
	else
	{
		// Sets the scout status itself.
		moveGroundScout();
	}
}

// Move the overlord scout.


// Called after the overlord has reached the enemy resource depot,
// or has been turned away from it by air defense.


// Called only when a gas steal is requested.
// Return true to say that gas stealing controls the worker, and
// false if the caller gets control.
bool ScoutManager::gasSteal()
{
    Base * enemyBase = the.bases.enemyStart();
    if (!enemyBase)
    {
        _gasStealStatus = "Enemy base not found";
        return false;
    }

    _enemyGeyser = getTheEnemyGeyser();
    if (!_enemyGeyser || !_enemyGeyser->getInitialTilePosition().isValid())
    {
        // No need to set status. It will change on the next frame.
        _gasStealOver = true;
        return false;
    }

    // The conditions are met. Do it!
    _startedGasSteal = true;

    if (_enemyGeyser->isVisible() && _enemyGeyser->getType() != BWAPI::UnitTypes::Resource_Vespene_Geyser)
    {
        // We can see the geyser and it has become something else.
        // That should mean that the geyser has been taken since we first saw it.
        _gasStealOver = true;    // give up
        // No need to set status. It will change on the next frame.
        return false;
    }
    else if (_enemyGeyser->isVisible() && _workerScout->getDistance(_enemyGeyser) < 300)
    {
        // We see the geyser. Queue the refinery, if it's not already done.
        // We can't rely on _queuedGasSteal to know, because the queue may be cleared
        // if a surprise occurs.
        // Therefore _queuedGasSteal affects mainly the debug display for the UI.
        if (!ProductionManager::Instance().isGasStealInQueue())
        {
            // NOTE Queueing the gas steal orders the building constructed.
            // Control of the worker passes to the BuildingManager until it releases the
            // worker with a call to setGasStealOver().
            ProductionManager::Instance().queueGasSteal();
            _queuedGasSteal = true;
            // Regardless, make sure we are moving toward the geyser.
            // It makes life easier on the building manager.
            the.micro.Move(_workerScout, _enemyGeyser->getInitialPosition());
        }
        _gasStealStatus = "Stealing gas";
    }
    else
    {
        // We don't see the geyser yet. Move toward it.
		the.micro.MoveToBase(_workerScout, enemyBase, _enemyGeyser->getInitialPosition(), true);
		_gasStealStatus = "Moving to steal gas";
    }
    return true;
}

// Choose an enemy worker to harass, or none.
BWAPI::Unit ScoutManager::enemyWorkerToHarass() const
{
    // First look for any enemy worker that is building.
    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->getType().isWorker() && unit->isConstructing())
        {
            return unit;
        }
    }

    BWAPI::Unit enemyWorker = nullptr;
    int maxDist = 500;    // ignore any beyond this range

    // Failing that, find the enemy worker closest to the gas.
    BWAPI::Unit geyser = getAnyEnemyGeyser();
    if (geyser)
    {
        for (BWAPI::Unit unit : the.enemy()->getUnits())
        {
            if (unit->getType().isWorker())
            {
                int dist = unit->getDistance(geyser->getInitialPosition());

                if (dist < maxDist)
                {
                    maxDist = dist;
                    enemyWorker = unit;
                }
            }
        }
    }

    return enemyWorker;
}

// Used in choosing an enemy worker to harass.
// Find an enemy geyser and return it, if there is one.
BWAPI::Unit ScoutManager::getAnyEnemyGeyser() const
{
    Base * enemyBase = the.bases.enemyStart();

    BWAPI::Unitset geysers = enemyBase->getGeysers();
    if (geysers.size() > 0)
    {
        return *(geysers.begin());
    }

    return nullptr;
}

// Used in stealing gas. Only called after the enemy base is found.
// If there is exactly 1 geyser in the enemy base and it may be untaken, return it.
// If 0 we can't steal it, and if >1 then it's no use to steal one.
BWAPI::Unit ScoutManager::getTheEnemyGeyser() const
{
    Base * enemyBase = the.bases.enemyStart();

    BWAPI::Unitset geysers = enemyBase->getGeysers();
    if (geysers.size() == 1)
    {
        BWAPI::Unit geyser = *(geysers.begin());
        // If the geyser is visible, we may be able to reject it as already taken.
        // TODO get the last known status from the.info.isGeyserTaken()
        if (!geyser->isVisible() || geyser->getType() == BWAPI::UnitTypes::Resource_Vespene_Geyser)
        {
            // We see it is untaken, or we don't see the geyser. Assume the best.
            return geyser;
        }
    }

    return nullptr;
}

// Should we release the worker scout early?
// Check for nearby friendly units that can keep watch, or for nearby enemy static defense
// that indicates we won't be able to see anything more this early in the game.
// If we see enemy units, we probably want to keep an eye on them until our combat units arrive.
bool ScoutManager::releaseScoutEarly(BWAPI::Unit worker) const
{
    if (!the.bases.myFront())
    {
        // We have no bases. Send the worker back and hope we can start a new one somewhere.
        // This should be rare.
        return true;
    }

    if (the.your.ever.unitCounts.size() == the.your.ever.initialEverTypeCount())
    {
        // We have seen no enemy unit type beyond those given at the start of the game.
		// Keep looking.
        return false;
    }

	// The worker may be more useful at home facing the rush.
	if (the.oppModel().getEnemyPlan() == OpeningPlan::FastRush)
	{
		return true;
	}

	// If the overlord already has a good view, and we've seen the enemy natural,
	// then we don't need a worker scout.
	

    if (worker->getDistance(the.bases.myFront()->getFront()) < 24 * 32)
    {
        // We're still close to where we started. Keep going.
        return false;
    }

	// The enemy has a unit that can catch the scout.
	// These are the most likely possibilities.
	if (the.your.seen.count(BWAPI::UnitTypes::Terran_Marine) > 0 ||		// marine is slow, but we're bad at dodging
		the.your.seen.count(BWAPI::UnitTypes::Terran_Vulture) > 0 ||
		the.your.seen.count(BWAPI::UnitTypes::Protoss_Dragoon) > 0 ||
		the.your.seen.count(BWAPI::UnitTypes::Zerg_Mutalisk) > 0)
	{
		return true;
	}

    // NOTE Sight range of an SCV or drone is 7. Sight range of a probe is 8.
    return
        BWAPI::Broodwar->getClosestUnit(
            worker->getPosition(),
            ((BWAPI::Filter::CanAttack || BWAPI::Filter::IsDetector) && BWAPI::Filter::IsOwned && !BWAPI::Filter::IsWorker) ||
            ((BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Bunker || BWAPI::Filter::GetType == BWAPI::UnitTypes::Protoss_Photon_Cannon) && BWAPI::Filter::IsEnemy && BWAPI::Filter::IsCompleted),
            8 * 32
        ) != nullptr;
}

bool ScoutManager::enemyWorkerInRadius()
{
    for (BWAPI::Unit unit : the.enemy()->getUnits())
    {
        if (unit->getType().isWorker() && unit->getDistance(_workerScout) < 300)
        {
            return true;
        }
    }

    return false;
}
