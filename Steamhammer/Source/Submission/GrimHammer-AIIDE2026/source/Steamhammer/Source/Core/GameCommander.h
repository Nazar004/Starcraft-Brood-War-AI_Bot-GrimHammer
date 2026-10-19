#pragma once

#include <BWAPI.h>
#include "Combat/CombatCommander.h"
#include "Utilities/TimerManager.h"

namespace UAlbertaBot
{
class UnitToAssign
{
public:

    BWAPI::Unit unit;
    bool isAssigned;

    UnitToAssign(BWAPI::Unit u)
    {
        unit = u;
        isAssigned = false;
    }
};

class GameCommander 
{
    CombatCommander &	_combatCommander;
    TimerManager		_timerManager;
    int                 _lastSpikePathFields = 0;
    int                 _lastSpikeAllDistanceFields = 0;
    int                 _lastSpikeGroundWaypoints = 0;
    int                 _lastSpikeAStarCalls = 0;
    int                 _lastSpikeDistanceQueryFields = 0;
    int                 _lastSpikeWaypointFields = 0;
    int                 _lastSpikePlacementFields = 0;
    int                 _lastSpikeMapEvictions = 0;
    double              _assignValidMs = 0.0;
    double              _assignScoutMs = 0.0;
    double              _assignCombatMs = 0.0;
    double              _lastSpikeAssignValidMs = 0.0;
    double              _lastSpikeAssignScoutMs = 0.0;
    double              _lastSpikeAssignCombatMs = 0.0;
    std::string         _lastSpikeProductionPhase;
    double              _lastSpikeProductionPhaseMs = 0.0;
    std::string         _lastSpikeUrgentStage;
    double              _lastSpikeUrgentStageMs = 0.0;
    double              _lastSpikeTerranRulePlanMs = 0.0;
    double              _lastSpikeTerranRuleGoalMs = 0.0;
    std::string         _lastSpikeGoalStage;
    double              _lastSpikeGoalStageMs = 0.0;
    double              _lastSpikeCombatPreparationMs = 0.0;
    double              _lastSpikeSquadsMs = 0.0;
    SquadUpdateProfile  _lastSpikeSquadTotals;
    double              _lastSpikeTankDefensePlanMs = 0.0;
    double              _lastSpikeTankGatherPlanMs = 0.0;
    int                 _lastSpikeTankDefensePlanBuilds = 0;
    int                 _lastSpikeTankGatherPlanBuilds = 0;
    SquadUpdateProfile  _lastSpikeSlowestSquad;
    double              _lastSpikeSlowestSquadMs = 0.0;
    double              _lastSpikeTaskMs = 0.0;
    std::string         _lastSpikeTaskName;
    std::string         _lastSpikeWorkerPhase;
    double              _lastSpikeWorkerPhaseMs = 0.0;
    double              _lastSpikeWorkerDefenseMs = 0.0;
    int                 _lastSpikeWorkerDefenseQueries = 0;
    double              _lastSpikeWorkerDangerMs = 0.0;
    int                 _lastSpikeWorkerDangerChecks = 0;

    BWAPI::Unitset      _validUnits;
    BWAPI::Unitset      _combatUnits;
    BWAPI::Unitset      _scoutUnits;

    int					_surrenderTime;     // for giving up early
    int                 _myHighWaterSupply; // used for surrender decisions vs. a human

    int					_initialScoutTime;  // 0 until a scouting worker is assigned

    void                assignUnit(BWAPI::Unit unit, BWAPI::Unitset & set);
    bool                isAssigned(BWAPI::Unit unit) const;

    bool				surrenderMonkey();

    BWAPI::Unit         getAnyFreeWorker();

    void                drawDebugInterface();
    // Draw the variable-height game summary and return the first free row.
    // Other top-left debug panels must use the returned coordinate instead of
    // assuming that this block always has the same number of lines.
    int                 drawGameInformation(int x, int y);
    void                drawUnitOrders();
    void                drawUnitCounts(int x, int y) const;
    void                drawTerrainHeights() const;
    void                drawDefenseClusters();

public:

    GameCommander();
    ~GameCommander() {};

    void update();
    void onEnd(bool isWinner);

    void handleUnitAssignments();
    void setValidUnits();
    void setScoutUnits();
    void setCombatUnits();


    void goScout();
    int getScoutTime() const { return _initialScoutTime; };

    void onUnitShow(BWAPI::Unit unit);
    void onUnitHide(BWAPI::Unit unit);
    void onUnitCreate(BWAPI::Unit unit);
    void onUnitComplete(BWAPI::Unit unit);
    void onUnitRenegade(BWAPI::Unit unit);
    void onUnitDestroy(BWAPI::Unit unit);
    void onUnitMorph(BWAPI::Unit unit);

    static GameCommander & Instance();
};

}
