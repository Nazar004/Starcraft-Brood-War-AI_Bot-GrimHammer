#pragma once

#include <map>
#include <vector>
#include "Economy/WorkerData.h"

namespace UAlbertaBot
{
class Building;

class WorkerManager
{
    WorkerData  workerData;
    BWAPI::Unit previousClosestWorker;
    bool		_collectGas;
    const char* _slowestUpdatePhase = "";
    double _slowestUpdatePhaseMs = 0.0;
    mutable double _defenseQueryMs = 0.0;
    mutable int _defenseQueries = 0;
    std::vector<BWAPI::Unit> _workerDefenseTargets;
    double _dangerCheckMs = 0.0;
    int _dangerChecks = 0;
    BWAPI::Unitset busy;    // units with special orders this frame
    std::map<int, BWAPI::Position> _mobileRepairPositions;
    int         _nextMobileRepairFrame;
    int         _lastMobileRepairObservedFrame;
    BWAPI::Unit _ferryTransport;
    BWAPI::Unit _ferryDepot;
    BWAPI::Unitset _ferryWorkers;
    int         _ferryPhase;
    int         _ferryStartFrame;
    int         _ferryLastCommandFrame;
    int         _ferryCooldownFrame;

    bool        expansionFerryNeededAt(const class Base* base) const;
    void        updateExpansionFerry();

    // Used in calls to maybeFleeDanger() and inWeaponsDanger().
    static const int WeaponsMargin = 2 * 32;
    static const int WeaponsMarginPlus = WeaponsMargin + 2 * 32;    // for hysteresis

    bool        isBusy(BWAPI::Unit worker) const;
    void        makeBusy(BWAPI::Unit worker);

    void        setMineralWorker(BWAPI::Unit unit);
    void        setReturnCargoWorker(BWAPI::Unit unit);
    void        returnWorkerToEconomy(BWAPI::Unit unit);
    bool		refineryHasDepot(BWAPI::Unit refinery);

    void        clearBlockingMinerals();

    void        handleGasWorkers();
    void        handleIdleWorkers();
    void		handleReturnCargoWorkers();
    void        handleRepairWorkers();
    void		handleMineralWorkers();
    void        handleUnblockWorkers();
    void		handlePostedWorkers();

    bool        maybeFleeDanger(BWAPI::Unit worker, int margin = WeaponsMargin);

    // A worker within this distance of a target location is considered to be
    // "in the same base" as the target. Used by worker selection routines to avoid
    // transferring workers between bases.
    static const int ThisBaseRange = 14 * 32;


    BWAPI::Unit findEnemyTargetForWorker(BWAPI::Unit worker) const;
    BWAPI::Unit findEscapeMinerals(BWAPI::Unit worker) const;
    bool		defendSelf(BWAPI::Unit worker, BWAPI::Unit resource);

    BWAPI::Unit getAnyClosestDepot(BWAPI::Unit worker);      // don't care whether it's full
    BWAPI::Unit getClosestNonFullDepot(BWAPI::Unit worker);  // only if it can accept more mineral workers

    BWAPI::Unit	getAnyWorker(const BWAPI::Position & pos, int range);
    BWAPI::Unit	getUnencumberedWorker(const BWAPI::Position & pos, int range);
	BWAPI::Unit getPostedWorker(const BWAPI::Position & pos);
    bool        postedWorkerBusy(const BWAPI::Position & pos);

    void        drawWorkerInformation();
    
    WorkerManager();

public:

    void        update();
    const char* slowestUpdatePhase() const { return _slowestUpdatePhase; }
    double slowestUpdatePhaseMilliseconds() const { return _slowestUpdatePhaseMs; }
    double defenseQueryMilliseconds() const { return _defenseQueryMs; }
    int defenseQueryCount() const { return _defenseQueries; }
    double dangerCheckMilliseconds() const { return _dangerCheckMs; }
    int dangerCheckCount() const { return _dangerChecks; }
    void        onUnitMorph(BWAPI::Unit unit);
    void        onUnitShow(BWAPI::Unit unit);
	void        onUnitDestroy(BWAPI::Unit unit);

    void        finishedWithWorker(BWAPI::Unit unit);

    void        updateWorkerStatus();

    int         getNumMineralWorkers() const;
    int         getNumGasWorkers() const;
    int         getNumReturnCargoWorkers() const;
    int			getNumCombatWorkers() const;
    int         getNumIdleWorkers() const;
    int         getNumRemoteMineralWorkers() const;
    int         getNumPostedWorkers() const;
    int			getMaxWorkers() const;

    int			getNumWorkers(BWAPI::Unit jobUnit) const;

    void        setScoutWorker(BWAPI::Unit worker);

    // NOTE _collectGas == false allows that a little more gas may still be collected.
    bool		isCollectingGas()              { return _collectGas; };
    void		setCollectGas(bool collectGas) { _collectGas = collectGas; };

    bool        isWorkerScout(BWAPI::Unit worker);
    bool		isCombatWorker(BWAPI::Unit worker);
    bool        isFree(BWAPI::Unit worker);
    bool        isBuilder(BWAPI::Unit worker);
    bool        isMobileRepairTarget(BWAPI::Unit unit) const;
    BWAPI::Position getMobileRepairPosition(BWAPI::Unit unit) const;

    BWAPI::Unit getBuilder(const Building & b);
    void        setBuildWorker(BWAPI::Unit worker);
	BWAPI::Unit	getLongDistanceWorker(const BWAPI::Position & pos);
	BWAPI::Unit getGasWorker(BWAPI::Unit refinery);
    BWAPI::Unit getClosestMineralWorkerTo(BWAPI::Unit enemyUnit);
    BWAPI::Unit getWorkerScout();

    void        setRepairWorker(BWAPI::Unit worker,BWAPI::Unit unitToRepair);
    void        stopRepairing(BWAPI::Unit worker);
    void        setCombatWorker(BWAPI::Unit worker);
    void        postGivenWorker(BWAPI::Unit worker, MacroLocation loc);
    BWAPI::Unit postWorker(MacroLocation loc);
    void        unpostWorkers(MacroLocation loc);

    bool        willHaveResources(int mineralsRequired,int gasRequired,double distance);
    void        rebalanceWorkers();
    bool        needsExpansionFerry() const;
    bool        isExpansionFerryWorker(BWAPI::Unit worker) const;

    static WorkerManager &  Instance();
};
}
