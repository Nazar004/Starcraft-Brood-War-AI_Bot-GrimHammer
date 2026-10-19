#pragma once

#include "Core/Common.h"
#include <array>
#include <chrono>
#include "Combat/OpsBoss.h"

#include "Combat/Micro/MicroAirToAir.h"
#include "Combat/Micro/MicroMelee.h"
#include "Combat/Micro/MicroRanged.h"
#include "Combat/Micro/Terran/MicroVultures.h"
#include "Combat/Micro/Terran/MicroGoliath.h"
#include "Combat/Micro/Terran/MicroGhost.h"

#include "Combat/Micro/MicroDetectors.h"
#include "Combat/Micro/Terran/MicroMedics.h"
#include "Combat/Micro/Terran/MicroTanks.h"
#include "Combat/Micro/MicroTransports.h"

namespace UAlbertaBot
{
// Measurements are reset each frame and never change combat decisions.
struct SquadUpdateProfile
{
    enum Stage { Units, Vanguard, Stim, Detectors, Clustering, Status,
        Special, Tanks, Regroup, Medics, Combat, Simulation, StageCount };
    enum MicroStage { Air, Melee, Ghost, Ranged, Vultures, Goliaths, MicroCount };
    std::array<double, StageCount> milliseconds{};
    std::array<double, MicroCount> microMilliseconds{};
    int simulations = 0;
    int units = 0;
    int clusters = 0;
    std::string squad;

    template<typename Work>
    void measure(Stage stage, Work work)
    {
        const auto started = std::chrono::steady_clock::now();
        work();
        milliseconds[stage] += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    }
    template<typename Work>
    void measureMicro(MicroStage stage, Work work)
    {
        const auto started = std::chrono::steady_clock::now();
        work();
        microMilliseconds[stage] += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    }
    int slowestStage() const
    {
        // Simulation is nested within Status; report it separately.
        return int(std::max_element(milliseconds.begin(),
            milliseconds.begin() + Simulation) - milliseconds.begin());
    }
    int slowestMicro() const
    {
        return int(std::max_element(microMilliseconds.begin(),
            microMilliseconds.end()) - microMilliseconds.begin());
    }
    static const char* stageName(int stage)
    {
        static const char* const names[] = { "units", "vanguard", "stim",
            "detectors", "clustering", "status", "special", "tanks",
            "regroup", "medics", "combat", "FAP" };
        return stage >= 0 && stage < StageCount ? names[stage] : "none";
    }
    static const char* microName(int stage)
    {
        static const char* const names[] = { "air", "melee", "ghost",
            "ranged", "vultures", "goliaths" };
        return stage >= 0 && stage < MicroCount ? names[stage] : "none";
    }
};

class Squad
{
    std::string         _name;
    BWAPI::Unitset      _units;
    bool				_combatSquad;
    int					_combatSimRadius;
    bool				_fightVisibleOnly;  // combat sim uses visible enemies only (vs. all known enemies)
    bool				_meatgrinder;		// combat sim says "win" even if you do only modest damage
    bool				_hasAir;
    bool				_hasGround;
    bool				_canAttackAir;
    bool				_canAttackGround;
    std::string         _regroupStatus;
    bool				_attackAtMax;       // turns true when we are at max supply
    size_t              _priority;

    // Values specifically for the combat commander, not otherwise needed.
    int                 _timeMark;          // set and read by combat commander
    int                 _lastAttack;        // last frame a squad cluster attacked
    int                 _lastRetreat;       // last frame a squad cluster retreated
    // NOTE Other cluster actions don't update _lastAttack or _lastRetreat.

    SquadOrder          _order;
    int                 _orderFrame;        // when the order was last changed
    BWAPI::Unit			_vanguard = nullptr;	// the unit closest to the order location, if any
    BWAPI::Position     _regroupPosition;   // cached because it is used in different code locations
    // Medic ID -> reserved healing energy and expiry frame, including latency.
    std::map<int, std::pair<int, int>> _stimHealingReservations;
    int _lastStimReservationFrame = -1;

    struct BioCombatResult
    {
        int frame = -100000;
        BWAPI::Position center = BWAPI::Positions::Invalid;
        BWAPI::Position enemyCenter = BWAPI::Positions::Invalid;
        size_t units = 0, enemyUnits = 0;
        int hp = 0, enemyHP = 0;
        double score = 0.0;
    };
    std::map<int, BioCombatResult> _bioCombatResults;

    MicroAirToAir		_microAirToAir;
    MicroMelee			_microMelee;
    MicroRanged			_microRanged;
    MicroVultures			_microVultures;
    MicroGoliath			_microGoliath;
    MicroGhost          _microGhost;
    MicroDetectors		_microDetectors;
    MicroMedics			_microMedics;
    MicroTanks			_microTanks;
    MicroTransports		_microTransports;

    std::map<BWAPI::Unit, bool> _nearEnemy;

    std::vector<UnitCluster> _clusters;
    SquadUpdateProfile _lastUpdateProfile;

    static const int ImmobileDefenseRadius = 800;

    BWAPI::Unit     unitClosestToPosition(const BWAPI::Position & pos, const BWAPI::Unitset & units) const;
    BWAPI::Unit		unitClosestToTarget(const BWAPI::Unitset & units) const;

    void			updateUnits();
    void			addUnitsToMicroManagers();
    void			setNearEnemyUnits();
    void			setAllUnits();
    void            setOrderForMicroManagers();

    void			setClusterStatus(UnitCluster & cluster);
    void            setLastAttackRetreat();
    bool            resetClusterStatus(UnitCluster & cluster);
    bool            shouldWaitForSiegeAgainstStaticDefense(const UnitCluster & cluster) const;
    void            microSpecialUnits(const UnitCluster & cluster);
    void            clusterCombat(const UnitCluster & cluster,
        const BWAPI::Position& regroupPosition);
    bool			noCombatUnits(const UnitCluster & cluster) const;
    bool			notNearEnemy(const UnitCluster & cluster);
    bool			joinUp(const UnitCluster & cluster);
	bool			canReachMineralLine(const UnitCluster & cluster) const;
	void			moveCluster(const UnitCluster & cluster, const BWAPI::Position & destination, bool lazy = true);
    void			attackMoveCluster(const UnitCluster & cluster, const BWAPI::Position & destination);
    void			safeMoveCluster(const UnitCluster & cluster, const BWAPI::Position & destination);
    bool shouldSkipTankForClusterOrders(BWAPI::Unit unit) const;

    bool			unitNearEnemy(BWAPI::Unit unit);
    bool			needsToRegroup(UnitCluster & cluster);
    BWAPI::Position calcRegroupPosition(const UnitCluster & cluster) const;
    BWAPI::Position finalRegroupPosition() const;
    BWAPI::Unit     nearbyImmobileGroundDefense(const BWAPI::Position & pos) const;
    BWAPI::Unit		nearbyImmobileDefense(const BWAPI::Position & pos) const;
    BWAPI::Unit		nearbyStaticDefense(const BWAPI::Position & pos) const;

    void			loadTransport();
    void			stimIfNeeded();

    void			drawCluster(const UnitCluster & cluster) const;

public:

    Squad();
    Squad(const std::string & name, size_t priority);
    ~Squad();

    void                update();
    const SquadUpdateProfile& lastUpdateProfile() const { return _lastUpdateProfile; }
    void                addUnit(BWAPI::Unit u);
    void                removeUnit(BWAPI::Unit u);
    void				releaseWorkers();
    bool                containsUnit(BWAPI::Unit u) const;
    bool                containsUnitType(BWAPI::UnitType t) const;
    bool                isEmpty() const;
    void                clear();
    size_t              getPriority() const;
    void                setPriority(const size_t & priority);
    const std::string & getName() const;

    int					mapPartition() const;
    BWAPI::Position     calcCenter() const;

    const BWAPI::Unitset &  getUnits() const;
    BWAPI::Unit			getVanguard() const { return _vanguard; };		// may be null
    void                setOrder(SquadOrder so);
    SquadOrder &        getOrder();
    int                 getOrderFrame() const { return _orderFrame; };
    const std::string   getRegroupStatus() const;

    int					getCombatSimRadius() const { return _combatSimRadius; };
    void				setCombatSimRadius(int radius) { _combatSimRadius = radius; };

    bool				getFightVisible() const { return _fightVisibleOnly; };
    void				setFightVisible(bool visibleOnly) { _fightVisibleOnly = visibleOnly; };

    bool				getMeatgrinder() const { return _meatgrinder; };
    void				setMeatgrinder(bool toWound) { _meatgrinder = toWound; };

    int                 getTimeMark() const { return _timeMark; };
    void                setTimeMark(int mark) { _timeMark = mark; };
    int                 getLastAttack() const { return _lastAttack; };
    int                 getLastRetreat() const { return _lastRetreat; };

    bool			    hasAir()			const { return _hasAir; };
    bool			    hasGround()			const { return _hasGround; };
    bool			    canAttackAir()		const { return _canAttackAir; };
    bool			    canAttackGround()	const { return _canAttackGround; };
    bool			    hasDetector()		const { return !_microDetectors.getUnits().empty(); };
    bool			    hasCombatUnits()	const;
    bool			    isOverlordHunterSquad() const;

    // Distance from the given position to the order position.
    int                 getDistance(const BWAPI::TilePosition & tile) const;
    int                 getDistance(const BWAPI::Position & pos) const;

};
}
