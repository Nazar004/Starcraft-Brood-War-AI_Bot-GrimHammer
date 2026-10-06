#pragma once

#include "Core/Common.h"
#include "Combat/Squads/Squad.h"
#include "Combat/Squads/SquadData.h"
#include "Intelligence/InformationManager.h"
#include "Strategy/StrategyManager.h"
#include "Core/The.h"
#include <set>
#include <vector>

namespace UAlbertaBot
{
enum class ComsatScanPriority
{
    Cleanup = 10,
    StrategicScout = 20,
    ArmyVision = 30,
    AirTracking = 40,
    TankBattle = 50,
    ExpansionClear = 60,
    EmergencyDetection = 70
};

class CombatCommander
{
    struct ComsatScanRequest
    {
        BWAPI::Position position;
        ComsatScanPriority priority;
        int minimumEnergy;
        int reserveEnergy;
        int requestedFrame;
        int expiryFrame;
        bool allowVisibleArea;
        bool requireFreshComsat;
        std::string reason;
    };

    SquadData       _squadData;
    double          _lastSquadUpdateMilliseconds = 0.0;
    double          _lastCombatPreparationMilliseconds = 0.0;
    BWAPI::Unitset  _combatUnits;
    bool            _initialized;

    bool			_goAggressive;                  // set by opening book and/or code


    std::map<std::string, int> _tankDropPhase;

    std::map<std::string, bool> _tankDropEmergencyMode;
    std::map<std::string, bool> _tankDropLaunchIssued;

    std::map<std::string, int> _tankDropLaunchCount;
    int _tankDropTotalLaunches;


    std::map<std::string, int> _tankDropUnloadStartFrame;
    std::map<std::string, bool> _tankDropReturnIssued;

    BWAPI::Position getTankDropUnloadPoint(int squadIndex) const;

    bool            _isWatching;                    // watch squad activated

    bool            _reconSquadAlive;               // recon squad activated
    Base *          _reconTarget;
    int				_lastReconTargetChange;         // frame number

    MicroDetectors  _scienceVesselMicro;
    BWAPI::Position _lastUndetectedEnemyPosition;
    int             _lastUndetectedEnemyFrame;
    BWAPI::Position _expansionDetectionPosition;
    int             _expansionDetectionFrame;
    std::vector<ComsatScanRequest> _comsatScanRequests;
    std::set<int>   _comsatsUsedForStrategicScouting;
    int             _lastComsatQueueFrame;
    int             _lastAutomaticComsatFrame;
    BWAPI::Position _lastComsatTarget;
    int             _lastComsatTargetFrame;
    std::string     _lastComsatReason;
    BWAPI::Unit     _islandCleanupSacrificeVictim;
    int             _lastIslandCleanupSupplyReleaseFrame;
    int countReservedTankDropVultures();
    void            updateIdleSquad();
    void            updateScienceVessels();		

    bool            _tankDropSeeded;
    std::set<BWAPI::Unit> _lockedHarassVultures;
    std::set<int> _expansionSupportVultureIDs;
    std::set<int> _expansionFerryDropshipIDs;
    std::set<int> _proxyFactoryVultureIDs;
    std::set<int> _escapingWorkerIDs;
    void lockVultureToHarass(BWAPI::Unit unit);
    bool isLockedHarassVulture(BWAPI::Unit unit) const;
    bool isProxyFactoryVulture(BWAPI::Unit unit) const;
    void clearDeadLockedHarassVultures();
    void clearDeadExpansionSupportVultures();
    bool _drop1EverAssigned;
    bool _drop2EverAssigned;
    int _genericDropFillStartFrame;
    int _genericDropLastCargoUsed;

    bool isReservedTankDropVulture(BWAPI::Unit unit) ;
    std::set<int>   _tankDropInitialVultureIDs;
    std::set<int>   _tankDropUsedVultureIDs;

    void            updateTankDropSquads();
    bool            tankDropEnabled() const;


    void            updateAttackSquads();
    void			updateReconSquad();
    void            updateBenzeneClearanceSquad();
    void            updateProxyFactoryVultureSquad();
    void            updateWorkerCounterAttackSquads();
    void            updateBaseDefenseSquads();
    void            updateScoutDefenseSquad();
    void            updateDropSquads();
    void            updateIslandCleanupSupplyRelease();

    bool            wantSquadDetectors() const;
    void			maybeAssignDetector(Squad & squad, bool wantDetector);

    void			loadOrUnloadBunkers();
    void			doComsatScan();
    void            queueAutomaticComsatRequests();
    void            processComsatScanRequests();

    int				weighReconUnit(const BWAPI::Unit unit) const;
    int				weighReconUnit(const BWAPI::UnitType type) const;

    bool			isFlyingSquadUnit(const BWAPI::UnitType type) const;
    bool			isOptionalFlyingSquadUnit(const BWAPI::UnitType type) const;
    bool			isGroundSquadUnit(const BWAPI::UnitType type) const;

    bool			unitIsGoodToDrop(const BWAPI::Unit unit) const;

    void			cancelDyingItems();

    BWAPI::Unit     findClosestDefender(const Squad & defenseSquad, BWAPI::Position pos, bool flyingDefender, bool pullWoekers, bool enemyHasAntiAir);
    BWAPI::Unit     findClosestWorkerToTarget(BWAPI::Unitset & unitsToAssign, BWAPI::Unit target);

    void			chooseReconTarget(const Squad & squad);
    Base *          getReconLocation() const;
    SquadOrder		getAttackOrder(
        Squad * squad,
        bool forceAggressive = false,
        bool forceDefensive = false);
    void            getAttackLocation(Squad * squad, Base * & base, BWAPI::Position & pos, std::string & returnKey);
    bool            defendedTarget(const BWAPI::Position & pos, bool vsGround, bool vsAir) const;
    BWAPI::Position getDropLocation(const Squad & squad);
    Base *          getDefensiveBase();

    void            initializeSquads();

    void            updateDefenseSquadUnits(Squad & defenseSquad, const size_t & flyingDefendersNeeded, const size_t & groundDefendersNeeded, bool pullWorkers, bool enemyHasAntiAir);

    int             numZerglingsInOurBase() const;
    bool            buildingRush() const;

    static int		workerPullScore(BWAPI::Unit worker);

public:
    double lastSquadUpdateMilliseconds() const { return _lastSquadUpdateMilliseconds; }
    double lastCombatPreparationMilliseconds() const { return _lastCombatPreparationMilliseconds; }

    CombatCommander();

    void update(const BWAPI::Unitset & combatUnits);
    void onEnd();

    bool tankDropLaunchLimitReached() const { return _tankDropTotalLaunches >= 2; }
    bool islandCleanupDropNeeded() const;
    BWAPI::Position getIslandCleanupDropTarget() const;

    void setAggression(bool aggressive) { _goAggressive = aggressive; }
    bool getAggression() const { return _goAggressive; };
    bool groundSquadAttackOrderActive();
    const BWAPI::Unitset& groundSquadUnits();

    void goDefensive() { setAggression(false); }
    void goAggressive() { setAggression(true); }

    // Scanner Sweep has one owner. Other systems submit requests here and the
    // combat update resolves them by priority while preserving energy for
    // emergency detection.
    bool requestComsatScan(
        const BWAPI::Position& position,
        ComsatScanPriority priority,
        int minimumEnergy,
        int reserveEnergy,
        int lifetimeFrames,
        const std::string& reason,
        bool allowVisibleArea = false,
        bool requireFreshComsat = false);
    bool comsatScanActiveOrPending(
        const BWAPI::Position& position,
        int radius = 5 * 32) const;
    void requestExpansionVesselDetection(const BWAPI::Position& position);

    
    void pullWorkers(int n);
    void releaseWorkers();

    bool canReserveExpansionSupportVulture(BWAPI::Unit unit) const;
    bool reserveExpansionSupportVulture(BWAPI::Unit unit);
    void releaseExpansionSupportVulture(int unitID);
    bool isExpansionSupportVulture(BWAPI::Unit unit) const;
    bool reserveExpansionFerryDropship(BWAPI::Unit ship);
    void releaseExpansionFerryDropship(BWAPI::Unit ship);
    bool isExpansionFerryDropship(BWAPI::Unit ship) const;
    bool vultureHasDedicatedRole(BWAPI::Unit unit)
    {
        return
            isLockedHarassVulture(unit) ||
            isReservedTankDropVulture(unit) ||
            isExpansionSupportVulture(unit) ||
            isProxyFactoryVulture(unit);
    }
    
    void drawSquadInformation(int x, int y);

    static CombatCommander & Instance();
};
}
