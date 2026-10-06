#pragma once

#include <forward_list>
#include <map>

#include "Core/Common.h"

#include "Production/BOSSManager.h"
#include "Production/BuildOrder.h"
#include "Production/BuildOrderQueue.h"
#include "Construction/BuildingManager.h"
#include "Production/ProductionGoal.h"
#include "Strategy/StrategyManager.h"

namespace UAlbertaBot
{
class ProductionManager
{
    enum class ResearchProducerPreparation { NotApplicable, Scheduled, Waiting };

    ProductionManager();

    BuildOrderQueue						_queue;
    std::forward_list<ProductionGoal>	_goals;

    int					_lastProductionFrame;            // for detecting jams
    BWAPI::TilePosition _predictedTilePosition;
    BWAPI::Unit         _assignedWorkerForThisBuilding;
    BWAPI::UnitType		_typeOfUpcomingBuilding;
    MacroLocation       _locationOfUpcomingBuilding;
    BWAPI::TilePosition _tileOfUpcomingBuilding;
    bool                _haveLocationForThisBuilding;
    int					_delayBuildingPredictionUntilFrame;
    int                 _unplaceableBunkerSinceFrame;
    int                 _nextTerranWallCheckFrame;
    int                 _terranWallGateOpenUntilFrame;
    int                 _terranWallLastGateCommandFrame;
    bool                _terranWallReleasedForVision;
    int                 _nextTerranBarracksVisionFrame;
    bool				_outOfBook;                      // production queue is beyond the opening book
    int					_targetGasAmount;                // for "go gas until <n>"; set to 0 if no target

    // Resources spent to create combat units.
    int                 _combatMineralsSpent;
    int                 _combatGasSpent;

    // Terran out-of-book production is executed as a persistent set of
    // StrategyManager targets. This keeps independent producers working
    // without replacing the JSON opening book or removing BOSS fallback.
    MetaPairVector      _terranRulePlan;
    std::map<int, size_t> _terranRuleBaseline;
    std::map<int, size_t> _terranRuleIssued;
    int                 _terranRulePlanFrame;
    int                 _terranRuleLastProgressFrame;
    int                 _terranRuleLastObservedFrame;
    std::string         _terranRuleDecision;
    // Keep strategic building limits while a queued request disappears from
    // the next goal snapshot because it is already counted as planned.
    std::map<int, size_t> _terranStructuralTargets;
    std::map<int, int>   _productionDisplayFirstSeen;
    std::vector<std::string> _terranDisplayGoals;
    std::string         _terranDisplayDecision;
    int                 _terranDisplayFrame = -100000;
    const char *        _slowestProductionPhase = "";
    double              _slowestProductionPhaseMs = 0.0;
    double              _lastTerranRulePlanMs = 0.0;
    
    BWAPI::Unit         getClosestUnitToPosition(const std::vector<BWAPI::Unit> & units, BWAPI::Position closestTo) const;
    BWAPI::Unit         getFarthestUnitFromPosition(const std::vector<BWAPI::Unit> & units, BWAPI::Position farthest) const;
    BWAPI::Unit         getBestHatcheryForLair(const std::vector<BWAPI::Unit> & hatcheries) const;
    
    void				executeCommand(const MacroAct & act);
    void				updateGoals();
    bool                create(BWAPI::Unit producer, const BuildOrderItem & item);
    void                dropRedundantQueueResearch();
    void                dropExcessQueuedCommandCenters();
    void                dropExcessQueuedUpgradeBuildings();
    void				dropJammedItemsFromQueue();
    bool				itemCanBeProduced(const MacroAct & act) const;
    bool                terranOpeningTypeAvailableOrPlanned(BWAPI::UnitType type) const;
    bool                terranOpeningRequirementsPlanned(const MacroAct & act) const;
    bool                manageTerranOpeningGoals();
    bool                manageBioTankOpeningProduction();
    void                manageBuildOrderQueue();
    void				maybeReorderQueue();
    bool                canMakeNow(BWAPI::Unit producer,MacroAct t);
    Building            makeBuildingForPrediction(const MacroAct & act, bool isGasSteal) const;
    bool                buildingPredictionMatches(const MacroAct & act) const;
    void                clearBuildingPrediction(bool releaseWorker);
    void                predictWorkerMovement(Building & b);

    int                 getFreeMinerals() const;
    int                 getFreeGas() const;
    void                getAvailableResources(const MacroAct& act, int& minerals, int& gas) const;
    ResearchProducerPreparation prepareMachineShopForResearch(
        const MacroAct& research,
        std::string& decision);
    void                prepareIdleFactoryMachineShop();

    void fixStuckFactoryTraining();
    void fixStuckCommandCenterTraining();

    void                resetTerranRuleProduction();
    void                refreshTerranRulePlan();
    bool                manageTerranRuleProduction();
    int                 terranRuleKey(const MacroAct & act) const;
    size_t              terranRuleTarget(const MetaPair & rule) const;
    bool                sameTerranRuleAction(const MacroAct & first, const MacroAct & second) const;
    size_t              queuedTerranRuleActionCount(const MacroAct & act);
    size_t              terranRuleProgress(const MacroAct & act);
    int                 plannedTerranTypeCount(BWAPI::UnitType type);
    BWAPI::UnitType     missingTerranPrerequisite(BWAPI::UnitType type, int depth = 0);
    BWAPI::UnitType     missingTerranPrerequisite(const MacroAct & act);
    BWAPI::UnitType     missingCompletedTerranPrerequisite(
                            BWAPI::UnitType type,
                            int depth = 0) const;
    bool                terranMacroProductionActive() const;
    bool                terranOpeningScoutPending();
    void                setTerranRuleDecision(const std::string & decision);
    void                drawTerranRuleInformation(int x, int & y);


    BWAPI::Unit         getProducer(MacroAct t) const;

    void moveLiftedBarracksForTankVision();
    void manageTerranMainWall();
    bool terranWallOpeningTankCorePending() const;

public:

    static ProductionManager &	Instance();

    void initialize();      // called from The

    void	setBuildOrder(const BuildOrder & buildOrder);
    BWAPI::Position getTankVisionPosition(BWAPI::Unit barracks) const;
    bool engineeringBayNeededForUpgrades() const;
    BWAPI::Unit getEngineeringBayTankSpotter() const;
    bool terranMainWallGateActive() const;
    bool terranNaturalArmyStagingReady() const;
    bool terranNaturalTransferReady() const;
    int terranBuildingQueueLimit(BWAPI::UnitType type, int fallback) const;
    void	update();
    const char * slowestProductionPhase() const { return _slowestProductionPhase; }
    double slowestProductionPhaseMilliseconds() const
        { return _slowestProductionPhaseMs; }
    double lastTerranRulePlanMilliseconds() const
        { return _lastTerranRulePlanMs; }
    void	onUnitDestroy(BWAPI::Unit unit);
    void	drawProductionInformation(int x, int y);
    void    liftBuildings(BWAPI::UnitType type) const;
    void    liftExcessBarracks(int keepLanded) const;
    bool    liftTerranMainWallBarracks(
                bool requireInteriorBarracks = true) const;

    void	queueGasSteal();
    bool	isGasStealInQueue() const;

    bool	nextIsBuilding() const;
    bool    ownsPredictedBuilder(BWAPI::Unit worker) const
    {
        return worker && worker == _assignedWorkerForThisBuilding;
    }

    bool    meetsReservedResources(const MacroAct & act) const;
    size_t  getPendingGoalCount(BWAPI::UnitType type) const;
    BWAPI::UnitType getMissingCompletedTerranPrerequisite(
                        const MacroAct & act) const;

    void	goOutOfBookAndClearQueue();
    void	goOutOfBook();
    bool	isOutOfBook() const { return _outOfBook; };
    void    invalidateTerranRulePlan(
        const std::string & reason = "strategy changed",
        bool clearPendingGoals = false);

    int     getCombatMineralsSpent() const { return _combatMineralsSpent; };
    int     getCombatGasSpent() const { return _combatGasSpent; };

    BuildOrderQueue & getQueue() { return _queue; };
};

class CompareWhenStarted
{

public:

    CompareWhenStarted() {}

    // For sorting the display of items under construction.
    // Some redundant code removed here thanks to Andrey Kurdiumov.
    bool operator() (BWAPI::Unit u1,BWAPI::Unit u2)
    {
        int startedU1 = u1->getType().buildTime() - u1->getRemainingBuildTime();
        int startedU2 = u2->getType().buildTime() - u2->getRemainingBuildTime();
        return startedU1 > startedU2;
    }
};

}
