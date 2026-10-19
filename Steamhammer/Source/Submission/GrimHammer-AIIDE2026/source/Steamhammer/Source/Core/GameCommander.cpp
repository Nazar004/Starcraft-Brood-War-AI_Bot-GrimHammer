#include "Core/GameCommander.h"

#include "Map/Bases.h"
#include "Construction/BuildingPlacer.h"
#include "Map/MapTools.h"
#include "Intelligence/Learning/OpponentModel.h"
#include "Utilities/UnitUtil.h"

#include "Construction/BuildingManager.h"
#include "Intelligence/InformationManager.h"
#include "Utilities/Logger.h"
#include "Map/MapGrid.h"
#include "Combat/Micro/Terran/MicroTanks.h"
#include "Intelligence/Learning/OpeningTiming.h"
#include "Intelligence/Learning/OpponentModel.h"
#include "Production/ProductionManager.h"
#include "Intelligence/ScoutManager.h"
#include "Strategy/StrategyManager.h"
#include "Economy/WorkerManager.h"

#include <algorithm>
#include <chrono>

using namespace UAlbertaBot;

GameCommander::GameCommander() 
    : _combatCommander(CombatCommander::Instance())
    , _initialScoutTime(0)
    , _surrenderTime(0)
    , _myHighWaterSupply(0)
{
}

void GameCommander::update()
{
    const auto crashCheckpoint = [](const char * context)
    {
        if (Config::IO::EnableCrashLog)
        {
            Logger::SetCrashContext(context, the.now());
        }
    };

    crashCheckpoint("GameCommander::update - start timers");
    _timerManager.startTimer(TimerManager::Total);

    // BWAPI can deliver one or more frames after our final unit is destroyed
    // and before onEnd(). There is no possible command left to issue, while
    // managers may still hold references from the previous frame (including
    // a destroyed Command Center). Let onEnd() record the result.
    if (BWAPI::Broodwar->getFrameCount() > 0 &&
        BWAPI::Broodwar->self()->getUnits().empty())
    {
        _timerManager.stopTimer(TimerManager::Total);
        return;
    }

    // populate the unit vectors we will pass into various managers
    _timerManager.startTimer(TimerManager::Assignments);
    crashCheckpoint("GameCommander::handleUnitAssignments");
    handleUnitAssignments();
    _timerManager.stopTimer(TimerManager::Assignments);

    // Decide whether to give up early. Implements config option SurrenderWhenHopeIsLost.
    crashCheckpoint("GameCommander::surrenderMonkey");
    if (!_surrenderTime && surrenderMonkey())
    {
        _surrenderTime = the.now();
        GameMessage("gg");
    }
    if (_surrenderTime)
    {
        if (the.now() - _surrenderTime >= 36)  // 36 frames = 1.5 game seconds
        {
            BWAPI::Broodwar->leaveGame();
        }
        _timerManager.stopTimer(TimerManager::Total);
        return;
    }

    // -- Managers that gather information. --

    _timerManager.startTimer(TimerManager::Information);
    crashCheckpoint("Bases::update");
    Bases::Instance().update();
    crashCheckpoint("InformationManager::update");
    InformationManager::Instance().update();
    _timerManager.stopTimer(TimerManager::Information);

    _timerManager.startTimer(TimerManager::The);
    crashCheckpoint("The::update");
    the.update();
    _timerManager.stopTimer(TimerManager::The);

    _timerManager.startTimer(TimerManager::OpponentModel);
    crashCheckpoint("OpponentModel::update");
    OpponentModel::Instance().update();
    the.placer.tryTerranRushWall();
    crashCheckpoint("SkillKit::update");
    the.skillkit.update();
    _timerManager.stopTimer(TimerManager::OpponentModel);

    // -- Managers that act on information. --

    _timerManager.startTimer(TimerManager::Search);
    crashCheckpoint("BOSSManager::update");
    BOSSManager::Instance().update(35 - _timerManager.getMilliseconds());
    _timerManager.stopTimer(TimerManager::Search);

    // May steal workers from WorkerManager, so run it before WorkerManager.
    _timerManager.startTimer(TimerManager::Production);
    crashCheckpoint("ProductionManager::update");
    ProductionManager::Instance().update();
    _timerManager.stopTimer(TimerManager::Production);

    // May steal workers from WorkerManager, so run it before WorkerManager.
    _timerManager.startTimer(TimerManager::Building);
    crashCheckpoint("BuildingManager::update");
    BuildingManager::Instance().update();
    _timerManager.stopTimer(TimerManager::Building);

    _timerManager.startTimer(TimerManager::Worker);
    crashCheckpoint("WorkerManager::update");
    WorkerManager::Instance().update();
    _timerManager.stopTimer(TimerManager::Worker);

    _timerManager.startTimer(TimerManager::Combat);
    crashCheckpoint("CombatCommander::update");
    _combatCommander.update(_combatUnits);
    crashCheckpoint("ScoutManager::update");
    ScoutManager::Instance().update();      // uses little time
    _timerManager.stopTimer(TimerManager::Combat);

    // Execute micro commands gathered above.
    _timerManager.startTimer(TimerManager::Micro);
    crashCheckpoint("Micro::update");
    the.micro.update();
    _timerManager.stopTimer(TimerManager::Micro);

    // Last so that it can delay tasks that may take too long. (NOTE Not yet implemented.)
    _timerManager.startTimer(TimerManager::Tasks);
    crashCheckpoint("Tasks::update");
    the.tasks.update();
    _timerManager.stopTimer(TimerManager::Tasks);

    _timerManager.stopTimer(TimerManager::Total);
    if (_timerManager.getLastSpikeFrame() == the.now())
    {
        const MapTools::PathWork pathWork = the.map.pathWorkAtFrame(the.now());
        _lastSpikePathFields = pathWork.distanceFields;
        _lastSpikeAllDistanceFields =
            GridDistances::computationsAtFrame(the.now());
        _lastSpikeGroundWaypoints = pathWork.waypoints;
        _lastSpikeAStarCalls = pathWork.astar;
        _lastSpikeDistanceQueryFields = pathWork.tileDistanceFields;
        _lastSpikeWaypointFields = pathWork.waypointFields;
        _lastSpikePlacementFields = pathWork.placementFields;
        _lastSpikeMapEvictions = pathWork.evictions;
        _lastSpikeAssignValidMs = _assignValidMs;
        _lastSpikeAssignScoutMs = _assignScoutMs;
        _lastSpikeAssignCombatMs = _assignCombatMs;
        _lastSpikeProductionPhase =
            ProductionManager::Instance().slowestProductionPhase();
        _lastSpikeProductionPhaseMs =
            ProductionManager::Instance().slowestProductionPhaseMilliseconds();
        _lastSpikeUrgentStage = StrategyManager::Instance().slowestUrgentStage();
        _lastSpikeUrgentStageMs =
            StrategyManager::Instance().slowestUrgentStageMilliseconds();
        _lastSpikeTerranRulePlanMs =
            ProductionManager::Instance().lastTerranRulePlanMilliseconds();
        _lastSpikeTerranRuleGoalMs =
            ProductionManager::Instance().lastTerranRuleGoalMilliseconds();
        _lastSpikeGoalStage = StrategyManager::Instance().slowestGoalStage();
        _lastSpikeGoalStageMs = StrategyManager::Instance().slowestGoalStageMilliseconds();
        _lastSpikeCombatPreparationMs =
            _combatCommander.lastCombatPreparationMilliseconds();
        _lastSpikeSquadsMs =
            _combatCommander.lastSquadUpdateMilliseconds();
        const SquadData& squadDiagnostics = _combatCommander.squadDiagnostics();
        _lastSpikeSquadTotals = squadDiagnostics.lastUpdateProfile();
        _lastSpikeTankDefensePlanMs = MicroTanks::defensivePlanMilliseconds();
        _lastSpikeTankGatherPlanMs = MicroTanks::gatherPlanMilliseconds();
        _lastSpikeTankDefensePlanBuilds = MicroTanks::defensivePlanBuilds();
        _lastSpikeTankGatherPlanBuilds = MicroTanks::gatherPlanBuilds();
        _lastSpikeSlowestSquad = squadDiagnostics.slowestSquadProfile();
        _lastSpikeSlowestSquadMs = squadDiagnostics.slowestSquadMilliseconds();
        _lastSpikeTaskName = the.tasks.slowestTaskName(the.now());
        _lastSpikeTaskMs = the.tasks.slowestTaskMilliseconds(the.now());
        const WorkerManager& workerDiagnostics = WorkerManager::Instance();
        _lastSpikeWorkerPhase = workerDiagnostics.slowestUpdatePhase();
        _lastSpikeWorkerPhaseMs = workerDiagnostics.slowestUpdatePhaseMilliseconds();
        _lastSpikeWorkerDefenseMs = workerDiagnostics.defenseQueryMilliseconds();
        _lastSpikeWorkerDefenseQueries = workerDiagnostics.defenseQueryCount();
        _lastSpikeWorkerDangerMs = workerDiagnostics.dangerCheckMilliseconds();
        _lastSpikeWorkerDangerChecks = workerDiagnostics.dangerCheckCount();
    }

    crashCheckpoint("GameCommander::drawDebugInterface");
    drawDebugInterface();
    crashCheckpoint("GameCommander::update - completed frame");
}

void GameCommander::onEnd(bool isWinner)
{
    OpponentModel::Instance().setWin(isWinner);
    OpponentModel::Instance().write();

    // Clean up any data structures that may otherwise not be unwound in the correct order.
    // This fixes an end-of-game bug diagnosed by Bruce Nielsen.
    _combatCommander.onEnd();
}

void GameCommander::drawDebugInterface()
{
    InformationManager::Instance().drawExtendedInterface();
    InformationManager::Instance().drawUnitInformation(425,30);
    drawUnitCounts(345, 30);
    Bases::Instance().drawBaseInfo();
    MicroTanks::DrawTacticalInformation();
    Bases::Instance().drawBaseOwnership(575, 30);
    the.map.drawExpoScores();
    InformationManager::Instance().drawResourceAmounts();
    BuildingManager::Instance().drawBuildingInformation(200, 50);
    the.placer.drawReservedTiles();
    the.tasks.draw(200, 50);
    // Game information has conditional rows (wall, threats and learned
    // strategy). Draw it first and anchor production below its actual bottom;
    // the old fixed y=60 made the two panels overwrite each other.
    const int gameInformationBottom = drawGameInformation(4, 1);
    ProductionManager::Instance().drawProductionInformation(
        30,
        gameInformationBottom + 2);
    BOSSManager::Instance().drawSearchInformation(490, 100);
    the.map.drawHomeDistances();
    drawTerrainHeights();
    drawDefenseClusters();
    
    _combatCommander.drawSquadInformation(170, 70);
    _timerManager.drawModuleTimers(490, 205);

    drawUnitOrders();
    the.skillkit.draw();
}

int GameCommander::drawGameInformation(int x, int y)
{
    if (!Config::Debug::DrawGameInfo)
    {
        // Keep the production panel at its traditional position when this
        // summary is disabled.
        return 58;
    }

    const OpponentModel::OpponentSummary & summary = OpponentModel::Instance().getSummary();
    BWAPI::Broodwar->drawTextScreen(x, y, "%c%s %c%d-%d %c%s",
        BWAPI::Broodwar->self()->getTextColor(), BWAPI::Broodwar->self()->getName().c_str(),
        white, summary.totalWins, summary.totalGames - summary.totalWins,
        BWAPI::Broodwar->enemy()->getTextColor(), BWAPI::Broodwar->enemy()->getName().c_str());
    y += 12;
    
    const std::string & openingGroup = StrategyManager::Instance().getOpeningGroup();
    const auto openingInfoIt = summary.openingInfo.find(Config::Strategy::StrategyName);
    const int wins = openingInfoIt == summary.openingInfo.end() ? 0 : openingInfoIt->second.sameWins + openingInfoIt->second.otherWins;
    const int games = openingInfoIt == summary.openingInfo.end() ? 0 : openingInfoIt->second.sameGames + openingInfoIt->second.otherGames;
    bool gasSteal = ScoutManager::Instance().wantGasSteal();
    BWAPI::Broodwar->drawTextScreen(x, y, "\x03%s%s%s%s %c%d-%d",
        Config::Strategy::StrategyName.c_str(),
        openingGroup != "" ? (" (" + openingGroup + ")").c_str() : "",
        gasSteal ? " + steal gas" : "",
        Config::Strategy::FoundEnemySpecificStrategy ? " - enemy specific" : "",
        white, wins, games - wins);
    BWAPI::Broodwar->setTextSize();
    y += 12;

    
    {
        BWAPI::Broodwar->drawTextScreen(
            x, y, "\x03Plan %c%s %c[%u] %c%s",
            yellow,
            StrategyManager::Instance().getTerranPlanName(),
            white,
            StrategyManager::Instance().getTerranPlanRevision(),
            gray,
            StrategyManager::Instance().getTerranPlanReason().c_str());
        y += 12;

        if (openingGroup == "tanks" && the.selfRace() == BWAPI::Races::Terran)
        {
            const int tankCount =
                the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
                the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
            const int goliathCount =
                the.my.completed.count(BWAPI::UnitTypes::Terran_Goliath);
            BWAPI::Broodwar->drawTextScreen(
                x, y, "%cTank push %s T%d/%d G%d/%d",
                CombatCommander::Instance().getAggression() ? green : orange,
                CombatCommander::Instance().getAggression() ? "ATTACK" : "WAIT",
                tankCount, StrategyManager::Instance().tankMassReleaseTankCount(),
                goliathCount, StrategyManager::Instance().tankMassReleaseGoliathCount());
            y += 12;
        }

        const std::string & wallStatus =
            the.placer.getTerranMainWallStatus();
        if (!wallStatus.empty())
        {
            BWAPI::Broodwar->drawTextScreen(
                x, y, "%c%s",
                the.placer.hasTerranMainWall() ? green : orange,
                wallStatus.c_str());
            y += 12;
        }

        const std::string & planLearning =
            OpponentModel::Instance().getTerranPlanLearningText();
        if (!planLearning.empty())
        {
            BWAPI::Broodwar->drawTextScreen(
                x, y, "%c%s", gray, planLearning.c_str());
            y += 12;
        }

        BWAPI::Broodwar->drawTextScreen(
            x, y, "%cEnemy army %c%s %c| formation %c%s",
            white,
            yellow,
            StrategyManager::Instance().getEnemyArmyModeName(),
            white,
            cyan,
            StrategyManager::Instance().getTerranArmyPostureName());
        y += 12;

        const EnemyThreatMask liveThreats =
            StrategyManager::Instance().getEnemyThreatMask();
        const EnemyThreatMask learnedThreats =
            OpponentModel::Instance().getLikelyEnemyThreats();
        if (liveThreats != 0 || learnedThreats != 0)
        {
            const std::string liveThreatText =
                EnemyThreatMaskString(liveThreats);
            const std::string learnedThreatText =
                EnemyThreatMaskString(learnedThreats);

            BWAPI::Broodwar->drawTextScreen(
                x, y, "%cThreats %c%s %c| history %c%s",
                white,
                red,
                liveThreatText.c_str(),
                gray,
                yellow,
                learnedThreatText.c_str());
            y += 12;
        }

        const StrategyClass learnedClass =
            OpponentModel::Instance().getLikelyStrategyClass();
        if (learnedClass != StrategyClass::Unknown)
        {
            const int learnedGames =
                OpponentModel::Instance().getLikelyStrategyClassGames();
            const int confidence = int(
                OpponentModel::Instance().getLikelyStrategyClassConfidence() *
                100.0 + 0.5);
            const bool activePrior =
                OpponentModel::Instance().stronglyPredictsStrategyClass(
                    learnedClass);

            BWAPI::Broodwar->drawTextScreen(
                x, y, "%cHistory army %c%s %c%dg %d%%%s",
                gray,
                yellow,
                StrategyClassString(learnedClass),
                gray,
                learnedGames,
                confidence,
                activePrior ? " active" : " collecting");
            y += 12;
        }
    }

    if (OpponentModel::Instance().getEnemyPlan() == OpeningPlan::Unknown &&
        OpponentModel::Instance().getExpectedEnemyPlan() != OpeningPlan::Unknown)
    {
        const OpeningPlan prediction =
            OpponentModel::Instance().getExpectedEnemyPlan();
        const int confidence = int(
            OpponentModel::Instance().getExpectedEnemyPlanConfidence() *
            100.0 + 0.5);
        const int games =
            OpponentModel::Instance().getExpectedEnemyPlanGames();
        const bool preparationActive =
            OpponentModel::Instance().stronglyPredictsEnemyPlan(prediction);

        BWAPI::Broodwar->drawTextScreen(
            x, y, "%cOpp Plan %cpredict %s %c%d%% %dg %c%s",
            white,
            orange,
            OpponentModel::Instance().getExpectedEnemyPlanString().c_str(),
            gray,
            confidence,
            games,
            preparationActive ? green : yellow,
            preparationActive ? "PREP" : "collecting");
    }
    else
    {
        BWAPI::Broodwar->drawTextScreen(
            x, y, "%cOpp Plan %c%s",
            white,
            yellow,
            OpponentModel::Instance().getEnemyPlanString().c_str());
    }
    y += 12;

    std::string island = "";
    if (Bases::Instance().isIslandStart())
    {
        island = " (island)";
    }
    BWAPI::Broodwar->drawTextScreen(x, y, "%c%s%c%s", yellow, BWAPI::Broodwar->mapFileName().c_str(), orange, island.c_str());
    BWAPI::Broodwar->setTextSize();
    y += 12;

    int frame = BWAPI::Broodwar->getFrameCount();
    BWAPI::Broodwar->drawTextScreen(x, y, "\x04%d %2u:%02u mean %.1fms max %.1fms",
        frame,
        int(frame / (23.8 * 60)),
        int(frame / 23.8) % 60,
        _timerManager.getMeanMilliseconds(),
        _timerManager.getMaxMilliseconds());
    y += 12;

    // Preserve the worst measured frame and its expensive subsystem so a
    // later minor hitch cannot hide the cause of the large maximum on screen.
    const int spikeFrame = _timerManager.getLastSpikeFrame();
    if (spikeFrame >= 0 && frame >= spikeFrame)
    {
        BWAPI::Broodwar->drawTextScreen(x, y,
            "\x08peak f%d %.1fms %s %.1fms",
            spikeFrame,
            _timerManager.getLastSpikeMilliseconds(),
            _timerManager.getLastSpikeModuleName(),
            _timerManager.getLastSpikeModuleMilliseconds());
        y += 12;
        BWAPI::Broodwar->drawTextScreen(x, y, "\x08parts %s",
            _timerManager.getLastSpikeModuleBreakdown().c_str());
        y += 12;
        if (_lastSpikeWorkerPhaseMs >= 5.0)
        {
            BWAPI::Broodwar->drawTextScreen(x, y, "\x08worker %s %.1fms defense %.1f/%d danger %.1f/%d",
                _lastSpikeWorkerPhase.c_str(), _lastSpikeWorkerPhaseMs,
                _lastSpikeWorkerDefenseMs, _lastSpikeWorkerDefenseQueries,
                _lastSpikeWorkerDangerMs, _lastSpikeWorkerDangerChecks);
            y += 12;
        }
        BWAPI::Broodwar->drawTextScreen(x, y,
            "\x08combat prep %.1f squads %.1f maps %d all %d wp %d A* %d",
            _lastSpikeCombatPreparationMs,
            _lastSpikeSquadsMs,
            _lastSpikePathFields,
            _lastSpikeAllDistanceFields,
            _lastSpikeGroundWaypoints,
            _lastSpikeAStarCalls);
        y += 12;
        if (_lastSpikeSquadsMs >= 5.0 && !_lastSpikeSlowestSquad.squad.empty())
        {
            const int hotStage = _lastSpikeSlowestSquad.slowestStage();
            const int hotMicro = _lastSpikeSlowestSquad.slowestMicro();
            BWAPI::Broodwar->drawTextScreen(x, y,
                "\x08squad %s %.1fms %du/%dc hot %s %.1fms",
                _lastSpikeSlowestSquad.squad.c_str(), _lastSpikeSlowestSquadMs,
                _lastSpikeSlowestSquad.units, _lastSpikeSlowestSquad.clusters,
                SquadUpdateProfile::stageName(hotStage),
                _lastSpikeSlowestSquad.milliseconds[hotStage]);
            y += 12;
            BWAPI::Broodwar->drawTextScreen(x, y,
                "\x08squad totals status %.1f tanks %.1f combat %.1f FAP %.1f/%d",
                _lastSpikeSquadTotals.milliseconds[SquadUpdateProfile::Status],
                _lastSpikeSquadTotals.milliseconds[SquadUpdateProfile::Tanks],
                _lastSpikeSquadTotals.milliseconds[SquadUpdateProfile::Combat],
                _lastSpikeSquadTotals.milliseconds[SquadUpdateProfile::Simulation],
                _lastSpikeSquadTotals.simulations);
            y += 12;
            if (_lastSpikeSquadTotals.milliseconds[SquadUpdateProfile::Tanks] >= 5.0)
            {
                BWAPI::Broodwar->drawTextScreen(x, y,
                    "\x08tank plans defense %.1fms/%d gather %.1fms/%d",
                    _lastSpikeTankDefensePlanMs, _lastSpikeTankDefensePlanBuilds,
                    _lastSpikeTankGatherPlanMs, _lastSpikeTankGatherPlanBuilds);
                y += 12;
            }
            if (_lastSpikeSlowestSquad.microMilliseconds[hotMicro] >= 1.0)
            {
                BWAPI::Broodwar->drawTextScreen(x, y,
                    "\x08squad micro %s %.1fms",
                    SquadUpdateProfile::microName(hotMicro),
                    _lastSpikeSlowestSquad.microMilliseconds[hotMicro]);
                y += 12;
            }
        }
        BWAPI::Broodwar->drawTextScreen(x, y,
            "\x08field sources dist %d move %d place %d evict %d",
            _lastSpikeDistanceQueryFields, _lastSpikeWaypointFields,
            _lastSpikePlacementFields, _lastSpikeMapEvictions);
        y += 12;
        BWAPI::Broodwar->drawTextScreen(x, y,
            "\x08assign valid %.1f scout %.1f combat %.1f",
            _lastSpikeAssignValidMs, _lastSpikeAssignScoutMs,
            _lastSpikeAssignCombatMs);
        y += 12;
        BWAPI::Broodwar->drawTextScreen(x, y,
            "\x08produce %s %.1fms plan %.1fms",
            _lastSpikeProductionPhase.c_str(), _lastSpikeProductionPhaseMs,
            _lastSpikeTerranRulePlanMs);
        y += 12;
        if (_lastSpikeTerranRulePlanMs >= 5.0)
        {
            BWAPI::Broodwar->drawTextScreen(x, y,
                "\x08plan %s %.1fms goals %.1fms rest %.1fms",
                _lastSpikeGoalStage.c_str(), _lastSpikeGoalStageMs,
                _lastSpikeTerranRuleGoalMs,
                std::max(0.0, _lastSpikeTerranRulePlanMs - _lastSpikeTerranRuleGoalMs));
            y += 12;
        }
        if (_lastSpikeProductionPhase == "urgent")
        {
            BWAPI::Broodwar->drawTextScreen(x, y,
                "\x08urgent %s %.1fms",
                _lastSpikeUrgentStage.c_str(), _lastSpikeUrgentStageMs);
            y += 12;
        }
        if (!_lastSpikeTaskName.empty())
        {
            BWAPI::Broodwar->drawTextScreen(x, y,
                "\x08task %s %.1fms",
                _lastSpikeTaskName.c_str(), _lastSpikeTaskMs);
            y += 12;
        }
    }

    /*
    // latency display
    y += 12;
    BWAPI::Broodwar->drawTextScreen(x + 50, y, "\x04%d max %d",
        BWAPI::Broodwar->getRemainingLatencyFrames(),
        BWAPI::Broodwar->getLatencyFrames());
    */

    return y;
}

void GameCommander::drawUnitOrders()
{
    if (!Config::Debug::DrawUnitOrders)
    {
        return;
    }

    for (BWAPI::Unit unit : BWAPI::Broodwar->getAllUnits())
    {
        if (!unit->getPosition().isValid())
        {
            continue;
        }

        std::string extra = "";
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Egg ||
            unit->getType() == BWAPI::UnitTypes::Zerg_Cocoon ||
            unit->getType().isBuilding() && !unit->isCompleted())
        {
            extra = UnitTypeName(unit->getBuildType());
        }
        else if (unit->isTraining())
        {
            // Avoid Unit::getTrainingQueue(); use the Train command type for
            // debug text so drawing unit orders cannot trigger the known BWAPI
            // access violation.
            const BWAPI::UnitType trainedType =
                unit->getLastCommand().getUnitType();
            if (trainedType != BWAPI::UnitTypes::None &&
                trainedType != BWAPI::UnitTypes::Unknown)
            {
                extra = UnitTypeName(trainedType);
            }
        }
        else if (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            extra = UnitTypeName(unit);
        }
        else if (unit->isResearching())
        {
            extra = unit->getTech().getName();
        }
        else if (unit->isUpgrading())
        {
            extra = unit->getUpgrade().getName();
        }

        int x = unit->getPosition().x - 8;
        int y = unit->getPosition().y - 2;
        if (extra != "")
        {
            BWAPI::Broodwar->drawTextMap(x, y, "%c%s", yellow, extra.c_str());
        }
        if (unit->getOrder() != BWAPI::Orders::Nothing)
        {
            BWAPI::Broodwar->drawTextMap(x, y + 10, "%c%d %c%s", white, unit->getID(), cyan, unit->getOrder().getName().c_str());
        }
    }
}

void GameCommander::drawUnitCounts(int x, int y) const
{
    if (!Config::Debug::DrawUnitCounts)
    {
        return;
    }

    const int c1 = 17;
    const int c2 = 38;
    const int e0 = 160;
    int dy = 0;
    for (BWAPI::UnitType t : BWAPI::UnitTypes::allUnitTypes())
    {
        if (the.my.all.count(t) > 0)
        {
            BWAPI::Broodwar->drawTextScreen    (x     , y + dy, "%c%3d" , white , the.my.completed.count(t));
            if (the.my.all.count(t) - the.my.completed.count(t) > 0)
            {
                BWAPI::Broodwar->drawTextScreen(x + c1, y + dy, "%c%+2d", yellow, the.my.all.count(t) - the.my.completed.count(t));
            }
            BWAPI::Broodwar->drawTextScreen    (x + c2, y + dy, "%c%s" , green , UnitTypeName(t).c_str());
            dy += 12;
        }
    }

    dy = 0;
    for (BWAPI::UnitType t : BWAPI::UnitTypes::allUnitTypes())
    {
        if (the.your.seen.count(t) + the.your.inferred.count(t) > 0)
        {
            char color = red;
            int n = the.your.inferred.count(t);
            if (the.your.seen.count(t))
            {
                color = white;
                n = the.your.seen.count(t);
            }

            BWAPI::Broodwar->drawTextScreen    (x + e0          , y + dy, "%c%3d", color , n);
            BWAPI::Broodwar->drawTextScreen    (x + e0 + c2 - 13, y + dy, "%c%s" , orange, UnitTypeName(t).c_str());
            dy += 12;
        }
    }
}

void GameCommander::drawTerrainHeights() const
{
    if (!Config::Debug::DrawTerrainHeights)
    {
        return;
    }

    for (int x = 0; x < BWAPI::Broodwar->mapWidth(); ++x)
    {
        for (int y = 0; y < BWAPI::Broodwar->mapHeight(); ++y)
        {
            int h = BWAPI::Broodwar->getGroundHeight(x, y);
            char color = h % 2 ? purple : gray;

            BWAPI::Position pos(BWAPI::TilePosition(x, y));
            BWAPI::Broodwar->drawTextMap(pos + BWAPI::Position(12, 12), "%c%d", color, h);
        }
    }
}

void GameCommander::drawDefenseClusters()
{
    if (!Config::Debug::DrawDefenseClusters)
    {
        return;
    }

    const std::vector<UnitCluster> & groundClusters = the.ops.getGroundDefenseClusters();

    for (const UnitCluster & cluster : groundClusters)
    {
        cluster.draw(BWAPI::Colors::Brown, "vs ground");
    }

    const std::vector<UnitCluster> & airClusters = the.ops.getAirDefenseClusters();

    for (const UnitCluster & cluster : airClusters)
    {
        cluster.draw(BWAPI::Colors::Grey, "vs air");
    }
}

// assigns units to various managers
void GameCommander::handleUnitAssignments()
{
    _validUnits.clear();
    _combatUnits.clear();
    // Don't clear the scout units.

    // Only keep units which are completed and usable.
    const auto started = std::chrono::steady_clock::now();
    setValidUnits();
    const auto validDone = std::chrono::steady_clock::now();

    // set each type of unit
    setScoutUnits();
    const auto scoutDone = std::chrono::steady_clock::now();
    setCombatUnits();
    const auto combatDone = std::chrono::steady_clock::now();
    _assignValidMs = std::chrono::duration<double, std::milli>(
        validDone - started).count();
    _assignScoutMs = std::chrono::duration<double, std::milli>(
        scoutDone - validDone).count();
    _assignCombatMs = std::chrono::duration<double, std::milli>(
        combatDone - scoutDone).count();
}

bool GameCommander::isAssigned(BWAPI::Unit unit) const
{
    return _combatUnits.contains(unit) || _scoutUnits.contains(unit);
}

// validates units as usable for distribution to various managers
void GameCommander::setValidUnits()
{
    /*
    // TODO testing
    std::string timingFile = Config::IO::WriteDir + "timing.csv";
    static bool speed = false;
    if (!speed && BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::Metabolic_Boost) > 0)
    {
        BWAPI::Broodwar->printf("zergling speed %d", BWAPI::Broodwar->getFrameCount());
        Logger::LogAppendToFile(timingFile, "%d,%s\n", BWAPI::Broodwar->getFrameCount(), "speed");
        speed = true;
    }
    static BWAPI::Unitset lings;
    size_t nLings = lings.size();
    */

    for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (UnitUtil::IsValidUnit(unit))
        {	
            _validUnits.insert(unit);

            /*
            // TODO testing
            static bool firstTime1 = false;
            if (unit->getType() == BWAPI::UnitTypes::Zerg_Lair && !firstTime1)
            {
                BWAPI::Broodwar->printf("lair timing %d", BWAPI::Broodwar->getFrameCount());
                firstTime1 = true;
            }
            // TODO testing
            //if (unit->getType() == BWAPI::UnitTypes::Zerg_Zergling)
            //{
            //    lings.insert(unit);
            //}
            */

            /*
            // TODO testing
            static bool firstTime1 = false;
            static BWAPI::Unitset reported;
            if (unit->getType() == BWAPI::UnitTypes::Protoss_Gateway && !reported.contains(unit))
            {
                BWAPI::Broodwar->printf("unit timing %d", BWAPI::Broodwar->getFrameCount());
                firstTime1 = true;
                reported.insert(unit);
            }
            */
        }
        else
        {
            /*
            static bool firstTime2 = false;
            if (!firstTime2 && unit->getType() == BWAPI::UnitTypes::Zerg_Hatchery && !unit->isCompleted())
            {
                BWAPI::Broodwar->printf("hatchery timing %d", BWAPI::Broodwar->getFrameCount());
                firstTime2 = true;
            }
            */
        }
    }

    /*
    // TODO testing
    if (lings.size() > nLings)
    {
        BWAPI::Broodwar->printf("%d lings @ %d", lings.size(), BWAPI::Broodwar->getFrameCount());
        Logger::LogAppendToFile(timingFile, "%d,%d,%d\n", BWAPI::Broodwar->getFrameCount(), lings.size(), BWAPI::Broodwar->self()->minerals());
    }
    */
}

void GameCommander::setScoutUnits()
{
    // If we're zerg, assign the first overlord to scout.
    

    // Send a scout worker if we haven't yet and should.
    if (!_initialScoutTime)
    {
        if (ScoutManager::Instance().shouldScout() && !the.bases.isIslandStart())
        {
            BWAPI::Unit workerScout = getAnyFreeWorker();

            // If we find a worker, make it the scout unit.
            if (workerScout)
            {
                ScoutManager::Instance().setWorkerScout(workerScout);
                assignUnit(workerScout, _scoutUnits);
                _initialScoutTime = the.now();
            }
        }
    }
}

// Set combat units to be passed to CombatCommander.
void GameCommander::setCombatUnits()
{
    for (BWAPI::Unit unit : _validUnits)
    {
        if (!isAssigned(unit) && (UnitUtil::IsCombatUnit(unit) || unit->getType().isWorker()))		
        {	
            assignUnit(unit, _combatUnits);
        }
    }
}

// Release the zerg scouting overlord for other duty.


void GameCommander::onUnitShow(BWAPI::Unit unit)			
{ 
    InformationManager::Instance().onUnitShow(unit); 
    WorkerManager::Instance().onUnitShow(unit);
}

void GameCommander::onUnitHide(BWAPI::Unit unit)			
{ 
    InformationManager::Instance().onUnitHide(unit); 
}

void GameCommander::onUnitCreate(BWAPI::Unit unit)		
{ 
    InformationManager::Instance().onUnitCreate(unit); 
	// WorkerManager has no separate create callback. Reuse its idempotent show
	// handler so a newly created SCV is registered immediately. The old code
	// accidentally called onUnitDestroy() here and removed the worker instead.
	WorkerManager::Instance().onUnitShow(unit);
}

void GameCommander::onUnitComplete(BWAPI::Unit unit)
{
    InformationManager::Instance().onUnitComplete(unit);
}

void GameCommander::onUnitRenegade(BWAPI::Unit unit)		
{ 
    InformationManager::Instance().onUnitRenegade(unit); 
}

void GameCommander::onUnitDestroy(BWAPI::Unit unit)		
{ 	
    if (!unit)
    {
        return;
    }
    if (Config::IO::EnableCrashLog)
    {
        Logger::SetCrashContext("onUnitDestroy: production", the.now());
    }
    ProductionManager::Instance().onUnitDestroy(unit);
    if (Config::IO::EnableCrashLog)
    {
        Logger::SetCrashContext("onUnitDestroy: workers", the.now());
    }
    WorkerManager::Instance().onUnitDestroy(unit);
    if (Config::IO::EnableCrashLog)
    {
        Logger::SetCrashContext("onUnitDestroy: information", the.now());
    }
    InformationManager::Instance().onUnitDestroy(unit); 
}

void GameCommander::onUnitMorph(BWAPI::Unit unit)		
{ 
    InformationManager::Instance().onUnitMorph(unit);
    WorkerManager::Instance().onUnitMorph(unit);
}

// Used only to choose a worker to scout.
BWAPI::Unit GameCommander::getAnyFreeWorker()
{
    for (BWAPI::Unit unit : _validUnits)
    {
        if (unit->getType().isWorker() &&
            !isAssigned(unit) &&
            WorkerManager::Instance().isFree(unit) &&
            !unit->isCarryingMinerals() &&
            !unit->isCarryingGas() &&
            unit->getOrder() != BWAPI::Orders::MiningMinerals)
        {
            return unit;
        }
    }

    return nullptr;
}

void GameCommander::assignUnit(BWAPI::Unit unit, BWAPI::Unitset & set)
{
    if (_scoutUnits.contains(unit)) { _scoutUnits.erase(unit); }
    else if (_combatUnits.contains(unit)) { _combatUnits.erase(unit); }

    set.insert(unit);
}

// Decide whether to give up early. See config option SurrenderWhenHopeIsLost.
// This depends on _validUnits, so call it after handleUnitAssignments().
// Giving up early is important in testing, to save time. In a serious tournament,
// it's a question of taste.
bool GameCommander::surrenderMonkey()
{
    if (!Config::Skills::SurrenderWhenHopeIsLost)
    {
        return false;
    }

    // Only check once every five seconds. No hurry to give up.
    if (BWAPI::Broodwar->getFrameCount() % (5 * 24) != 0)
    {
        return false;
    }

    // Don't surrender right at the start.
    if (BWAPI::Broodwar->getFrameCount() < 24 * 60)
    {
        return false;
    }

    // We are playing against a human. Surrender earlier to reduce frustration.
    if (Config::Skills::HumanOpponent)
    {
        int mySupply = BWAPI::Broodwar->self()->supplyUsed();
        PlayerSnapshot enemySnap(BWAPI::Broodwar->enemy());
        int knownEnemySupply = enemySnap.getSupply();

        // BWAPI::Broodwar->printf("supply %d < %d vs enemy %d", mySupply, _myHighWaterSupply, knownEnemySupply);

        if (mySupply > _myHighWaterSupply)
        {
            _myHighWaterSupply = mySupply;
            return false;
        }

        // Surrender if the enemy is way ahead AND we have been hurt.
        // We don't check that we were RECENTLY hurt.
        return mySupply < knownEnemySupply / 2 && mySupply < _myHighWaterSupply / 2;
    }

    // We assume we are playing against another bot.
    // The only reason to gg is to save time, so be conservative.
    // Surrender if all conditions are met:
    // 1. We don't have the cash to make a worker.
    // 2. We have no completed unit that can attack. (We may have incomplete units.)
    // 3. The enemy has at least one visible unit that can destroy buildings.
    // Terran does not float buildings, so we check whether the enemy can attack ground.

    // 1. Our cash.
    if (BWAPI::Broodwar->self()->minerals() >= 50)
    {
        return false;
    }

    // 2. Our units.
    for (BWAPI::Unit unit : _validUnits)
    {
        if (unit->canAttack())
        {
            return false;
        }
    }

    // 3. Enemy units.
    bool safe = true;
    for (BWAPI::Unit unit : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (unit->isVisible() && UnitUtil::CanAttackGround(unit))
        {
            safe = false;
            break;
        }
    }
    if (safe)
    {
        return false;
    }

    // Surrender monkey says surrender!
    return true;
}

GameCommander & GameCommander::Instance()
{
    static GameCommander instance;
    return instance;
}
