#include "Strategy/StrategyManager.h"
#include "Strategy/BioProductionPolicy.h"
#include "Strategy/Terran/BioTankAssessment.h"
#include "Strategy/Terran/AirTanks.h"
#include "Strategy/Terran/TvZSparks.h"
#include "Strategy/Terran/TvZPreHive.h"
#include "Strategy/Terran/TvZSK.h"
#include "Strategy/Terran/TvZBioSupport.h"
#include "Construction/BunkerPlacementPolicy.h"

#include "Map/Bases.h"
#include "Combat/CombatCommander.h"
#include "Map/MapTools.h"
#include "Map/Grids/GridZone.h"
#include "Intelligence/Learning/OpponentModel.h"
#include "Production/ProductionManager.h"
#include "Utilities/UnitUtil.h"
#include "Construction/BuildingManager.h"
#include "Economy/WorkerManager.h"
#include <cmath>
#include <vector>
#include <algorithm>
#include <limits>
#include <map>
#include <climits>
#include <chrono>

using namespace UAlbertaBot;

namespace
{
    bool visibleEnemyGroundNearMainOrClosestExpansion();
    bool visibleEnemyAirNearOwnedBase();

    const TvZBioPlan::Policy* tvzBioPolicy(TerranPlanId plan)
    {
        switch (plan)
        {
        case TerranPlanId::TvZ_Sparks: return &TvZSparks::policy();
        case TerranPlanId::TvZ_PreHive: return &TvZPreHive::policy();
        case TerranPlanId::TvZ_SK: return &TvZSK::policy();
        default: return nullptr;
        }
    }

    // A new mineral line is urgent before the last patches disappear. This
    // also catches the stronger signal that SCVs have already begun mining a
    // neutral base without a Command Center there.
    bool terranMineralExpansionEmergency()
    {
        

        int remainingMinerals = 0;
        int remainingPatches = 0;
        int groundedBases = 0;
        for (const Base* base : the.bases.getAll())
        {
            if (!base || base->getOwner() != the.self()) continue;
            BWAPI::Unit depot = base->getDepot();
            if (!depot || !depot->exists() || !depot->isCompleted() ||
                depot->isFlying()) continue;

            ++groundedBases;
            for (BWAPI::Unit mineral : base->getRemainingMinerals())
            {
                if (mineral && mineral->exists())
                {
                    ++remainingPatches;
                    remainingMinerals += mineral->getResources();
                }
            }
        }
        if (groundedBases == 0) return false;

        const WorkerManager& workers = WorkerManager::Instance();
        return workers.getNumRemoteMineralWorkers() > 0 ||
            remainingPatches <= 2 * groundedBases ||
            remainingMinerals <=
                std::max(3200, 140 * workers.getNumMineralWorkers());
    }
}

StrategyManager::StrategyManager()
    : _selfRace(the.self()->getRace())
    , _enemyRace(the.enemy()->getRace())
    , _totalGamesPlayed(0)
    , _emptyBuildOrder(the.self()->getRace())
    , _openingGroup("")
    , _pureAirPlan(false)
    , _hasDropTech(false)
    , _highWaterBases(1)
    , _openingStaticDefenseDropped(false)
    , _defaultTankGather(true)
    , _bioTankFirstPushReleased(false)
    , _tankFirstAttackReleased(false)
    , _airTanksFirstPushReleased(false)
    , _airTanksAirReleased(false)
    , _bioFirstAttackFrame(-1)
    , _openingTerranPlanName("")
    , _terranBaselinePlan(TerranPlanId::None)
    , _terranPlan(TerranPlanId::None)
    , _terranPlanEnteredFrame(0)
    , _terranPlanRevision(0)
    , _terranPlanReason("uninitialized")
    , _tankMassReleaseTanks(15)
    , _tankMassReleaseGoliaths(0)
{
}

StrategyManager & StrategyManager::Instance() 
{
    static StrategyManager instance;
    return instance;
}

const BuildOrder & StrategyManager::getOpeningBookBuildOrder() const
{
    auto buildOrderIt = _strategies.find(Config::Strategy::StrategyName);

    // look for the build order in the build order map
    if (buildOrderIt != std::end(_strategies))
    {
        return (*buildOrderIt).second._buildOrder;
    }
    else
    {
        UAB_ASSERT_WARNING(false, "Strategy not found: %s, returning empty initial build order", Config::Strategy::StrategyName.c_str());
        return _emptyBuildOrder;
    }
}

// This is used for terran and protoss.
const bool StrategyManager::shouldExpandNow() const
{
    // if there is no place to expand to, we can't expand
    // We check mineral expansions only.
    if (the.map.getNextExpansion(false, true, false) == BWAPI::TilePositions::None)
    {
        return false;
    }

    size_t numDepots =
        the.my.all.count(BWAPI::UnitTypes::Terran_Command_Center);
    int minute = the.now() / (24 * 60);
    const bool mineralEmergency = terranMineralExpansionEmergency();

    // Mining the next base without a depot, or running out of usable patches,
    // is already an economic emergency. Do not apply the normal early-third
    // timing and opening-threat gate to a base needed to keep income alive.
    if (mineralEmergency)
    {
        return true;
    }

    // A remembered opening threat must not hold a developed Tanks economy
    // until minute 12 while thousands of minerals accumulate. The plan's
    // earliest-third timing still applies; require two completed depots and
    // workers, no visible attack at our bases, and a legal expansion above.
    const bool tankExpansionSurplus =
        the.selfRace() == BWAPI::Races::Terran &&
        _openingGroup == "tanks" &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Command_Center) >= 2 &&
        the.my.all.count(BWAPI::UnitTypes::Terran_SCV) >= 24 &&
        the.self()->minerals() >= 1800 &&
        !visibleEnemyGroundNearMainOrClosestExpansion() &&
        !visibleEnemyAirNearOwnedBase();

    // Release a contained BioTank opening by current safety and economy,
    // rather than remembering its rush label until minute twelve.
    const bool bioExpansionReady = _terranPlan == TerranPlanId::BioTank &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Command_Center) >= 2 &&
        the.my.all.count(BWAPI::UnitTypes::Terran_SCV) >= 28 &&
        (the.self()->minerals() >= 500 ||
         the.my.all.count(BWAPI::UnitTypes::Terran_SCV) >=
            int(std::ceil(0.82 * WorkerManager::Instance().getMaxWorkers()))) &&
        !BioTankPolicy::homeDefenseRequired();

    // Keep the natural, but delay a greedy third during opening pressure.
    if (the.selfRace() == BWAPI::Races::Terran &&
        numDepots >= 2 &&
        minute <= 11 &&
        !tankExpansionSurplus && !bioExpansionReady &&
        (the.oppModel().hasEnemyOpeningThreat(OpeningThreat::GatewayPressure) ||
         the.oppModel().hasEnemyOpeningThreat(OpeningThreat::FactoryPressure) ||
         the.oppModel().hasEnemyOpeningThreat(OpeningThreat::MassBarracks) ||
         the.oppModel().hasEnemyOpeningThreat(OpeningThreat::MassProduction) ||
         the.oppModel().hasEnemyOpeningThreat(OpeningThreat::ProtossFastRobotics) ||
         the.oppModel().hasEnemyOpeningThreat(OpeningThreat::HydraliskPressure) ||
         the.oppModel().hasEnemyOpeningThreat(OpeningThreat::ZergTwoHatchLing)))
    {
        return false;
    }

    // Terran production should not have to leave 600 minerals idle before it
    // is allowed to grow. A continuously producing army may never create that
    // bank even when every current mineral line is already saturated. Use the
    // current resource capacity as the primary economic signal instead.
    //
    // getMaxWorkers() is based on the remaining mineral patches and completed
    // refineries at owned bases, so the threshold automatically falls as a
    // base mines out and rises when a new base becomes available.
    
    {
        const int completedDepots =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Command_Center);

        const int workers =
            the.my.all.count(BWAPI::UnitTypes::Terran_SCV);

        const int currentWorkerCapacity =
            WorkerManager::Instance().getMaxWorkers();

        const int saturationTrigger =
            std::max(
                12 * std::max(1, completedDepots),
                int(std::ceil(0.82 * currentWorkerCapacity)));

        const int remainingPatches =
            the.bases.remainingMineralPatchCount();

        const bool economyNearlySaturated =
            completedDepots > 0 &&
            workers >= saturationTrigger;

        const bool mineralLinesMiningOut =
            completedDepots > 0 &&
            workers >= 8 * completedDepots &&
            remainingPatches <= 4 * completedDepots;

        if (economyNearlySaturated || mineralLinesMiningOut)
        {
            return true;
        }
    }

    // Do not let either of these generic economy shortcuts bypass the threat
    // gate above and start a third base during a committed opening attack.
    if (WorkerManager::Instance().getNumIdleWorkers() > 3)
    {
        return true;
    }

    if (the.self()->minerals() > 600)
    {
        return true;
    }

    // we will make expansion N after array[N] minutes have passed
    std::vector<int> expansionTimes = {4, 10, 15, 20, 24, 28, 35, 40};

    for (size_t i(0); i < expansionTimes.size(); ++i)
    {
        if (numDepots < (i+2) && minute > expansionTimes[i])
        {
            return true;
        }
    }

    return false;
}

bool StrategyManager::isTerranMineralExpansionEmergency() const
{
    return terranMineralExpansionEmergency();
}

void StrategyManager::addStrategy(const std::string & name, const Strategy & strategy)
{
    if (strategy._race != BWAPI::Races::Terran) return;
    _strategies[name] = strategy;
}

// Set _openingGroup depending on the current strategy, which in principle
// might be from the config file or from opening learning.
// This is part of initialization; it happens early on.
void StrategyManager::setOpeningGroup()
{
    // StrategyManager is a process-wide singleton. Refresh every per-game
    // field here so a second ladder game cannot inherit the previous enemy
    // race, plan, static-defense state or cached drop capability.
    _selfRace = the.self()->getRace();
    _enemyRace = the.enemy()->getRace();
    _hasDropTech = false;
    _highWaterBases = 1;
    _openingStaticDefenseDropped = false;
    _pureAirPlan = false;
    _bioTankFirstPushReleased = false;
    _tankFirstAttackReleased = false;
    _airTanksFirstPushReleased = false;
    _airTanksAirReleased = false;
    _airTanksAttackStatus.clear();
    _airTanksVariant.clear();
    _bioFirstAttackFrame = -1;
    _openingTerranPlanName.clear();
    _terranBaselinePlan = TerranPlanId::None;
    _terranPlan = TerranPlanId::None;
    _terranPlanEnteredFrame = 0;
    _terranPlanRevision = 0;
    _terranPlanReason = "new game";
    _openingGroup = _selfRace == BWAPI::Races::Terran ? "tanks" : "";
    _defaultTankGather = _openingGroup == "tanks";
    _tankMassReleaseTanks = _defaultTankGather ? 15 : 0;
    _tankMassReleaseGoliaths = 0;

    auto buildOrderItr = _strategies.find(Config::Strategy::StrategyName);

    if (buildOrderItr != std::end(_strategies))
    {
        _openingGroup = (*buildOrderItr).second._openingGroup;
        _openingTerranPlanName = (*buildOrderItr).second._terranPlan;
        _airTanksVariant = (*buildOrderItr).second._airTanksVariant;
        _pureAirPlan = false;
        _defaultTankGather = _openingGroup == "tanks";
        _tankMassReleaseTanks = _defaultTankGather ? 15 : 0;
        _tankMassReleaseGoliaths = 0;
    }

    
    {
        initializeTerranPlan();
    }
}

const std::string & StrategyManager::getOpeningGroup() const
{
    return _openingGroup;
}
bool StrategyManager::defaultTankGatherEnabled() const
{
    return _defaultTankGather;
}

int StrategyManager::tankMassReleaseTankCount() const
{
    return _tankMassReleaseTanks;
}

int StrategyManager::tankMassReleaseGoliathCount() const
{
    return _tankMassReleaseGoliaths;
}
bool StrategyManager::pureAirPlanActive() const
{
    return _pureAirPlan;
}

bool StrategyManager::bioTankPlanActive() const
{
    return _selfRace == BWAPI::Races::Terran &&
        _openingGroup == "bio" && !_pureAirPlan;
}

bool StrategyManager::deepSixPlanActive() const
{
    return _selfRace == BWAPI::Races::Terran &&
        _terranPlan == TerranPlanId::TvP_DeepSix;
}

int StrategyManager::bioTankDesiredTankPercent() const
{
    if (!bioTankUsesTanks()) return 0;
    if (_terranPlan == TerranPlanId::BioTank &&
        ProductionManager::Instance().isOutOfBook())
    {
        const auto& mix = BioTankPolicy::enemyMix();
        return BioTankPolicy::tankPercent(mix.heavyGround, mix.lightGround, mix.air);
    }
    const int infantrySupply = 2 * (
        the.my.all.count(BWAPI::UnitTypes::Terran_Marine) +
        the.my.all.count(BWAPI::UnitTypes::Terran_Medic) +
        the.my.all.count(BWAPI::UnitTypes::Terran_Firebat));
    return BioProductionPolicy::tankPercent(the.self()->minerals(), the.self()->gas(),
        infantrySupply, ProductionManager::Instance().isOutOfBook());
}

bool StrategyManager::bioTankUsesTanks() const
{
    const auto* policy = tvzBioPolicy(_terranPlan);
    return !policy || policy->tankFloor > 0;
}

bool StrategyManager::bioTankPushReady()
{
    if (!bioTankPlanActive())
    {
        return true;
    }

    if (_bioTankFirstPushReleased)
    {
        return true;
    }

    // SK starts pressuring with its Marine/Medic core. Waiting for the first
    // Vessel and Irradiate kept an otherwise ready army at home until the
    // midgame; those remain independent production and support goals.
    if (_terranPlan == TerranPlanId::TvZ_SK)
    {
        _bioTankFirstPushReleased =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Marine) >= 16 &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Medic) >=
                BioProductionPolicy::medicTarget(16) &&
            the.self()->hasResearched(BWAPI::TechTypes::Stim_Packs);
        return _bioTankFirstPushReleased;
    }

    if (const auto* policy = tvzBioPolicy(_terranPlan))
    {
        const auto& push = policy->push;
        _bioTankFirstPushReleased =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Marine) >= push.marines &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Medic) >=
                BioProductionPolicy::medicTarget(push.marines) &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
                the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) >= push.tanks &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Vessel) >= push.vessels &&
            the.self()->hasResearched(BWAPI::TechTypes::Stim_Packs) &&
            (!push.siege || the.self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode)) &&
            (!push.range || the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::U_238_Shells) > 0) &&
            (!push.irradiate || the.self()->hasResearched(BWAPI::TechTypes::Irradiate));
        return _bioTankFirstPushReleased;
    }

    const bool deepSix = deepSixPlanActive();
    const int marineTarget = deepSix ? 24 : 16;
    const int tankTarget = deepSix ? 4 : 2;
    _bioTankFirstPushReleased =
        the.my.completed.count(BWAPI::UnitTypes::Terran_Marine) >= marineTarget &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Medic) >=
            BioProductionPolicy::medicTarget(marineTarget) &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) >= tankTarget &&
        the.self()->hasResearched(BWAPI::TechTypes::Stim_Packs) &&
        the.self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode) &&
        (!deepSix ||
            (the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::U_238_Shells) >= 1 &&
             the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::Terran_Infantry_Armor) >= 1)) &&
        BioTankPolicy::coreTogether(marineTarget, tankTarget);
    return _bioTankFirstPushReleased;
}

TerranPlanId StrategyManager::getTerranPlan() const
{
    return _terranPlan;
}

const std::string & StrategyManager::getTerranPlanReason() const
{
    return _terranPlanReason;
}

unsigned int StrategyManager::getTerranPlanRevision() const
{
    return _terranPlanRevision;
}

const MetaPairVector StrategyManager::getBuildOrderGoal()
{
    return getTerranBuildOrderGoal();
}



namespace
{
    enum class EnemyArmyMode
    {
        PureAir,
        HeavyGround,
        LightGround,
        MixedAirHeavyGround,
        MixedAirLightGround,
        Default
    };

    enum class EnemyAirLevel
    {
        NoAir,
        SmallAir,
        CommittedAir,
        CarrierAir
    };

    // This is a safety ceiling, not the normal target. Prediction stops at the
    // three-Turret worker arc, committed air normally uses six, and only the
    // heaviest capital-air defense approaches this per-base maximum.
    constexpr int MAX_TURRETS_PER_OWNED_BASE = 10;
    constexpr int TURRET_RING_MIN_TILE_DISTANCE = 3;

    bool enemyHasOpeningThreat(OpeningThreat threat)
    {
        return OpponentModel::Instance().hasEnemyOpeningThreat(threat);
    }

    bool learnedPureAirExpectedAfterOpening()
    {
        // Saved macro classes are used as a cautious prior only after the JSON
        // opening has completed. A single historical game is not enough.
        return
            ProductionManager::Instance().isOutOfBook() &&
            OpponentModel::Instance().stronglyPredictsStrategyClass(
                StrategyClass::PureAir);
    }

    bool learnedMixedExpectedAfterOpening()
    {
        // Mixed history is also useful, but it never authorizes a full
        // PureAir conversion. It adds only the mobile rear AA layer while the
        // normal Tank/Vulture ground core remains alive.
        return
            ProductionManager::Instance().isOutOfBook() &&
            OpponentModel::Instance().stronglyPredictsStrategyClass(
                StrategyClass::Mixed);
    }

    bool learnedEnemyThreatExpectedAfterOpening(EnemyThreat threat)
    {
        // Threat history is preparation evidence only. It may buy a small
        // counter layer after JSON, but current-game observations still own
        // emergency production and irreversible PureAir conversion.
        return
            ProductionManager::Instance().isOutOfBook() &&
            OpponentModel::Instance().stronglyPredictsEnemyThreat(threat);
    }

    bool enemyOpeningLightPressure()
    {
        return
            enemyHasOpeningThreat(OpeningThreat::ZealotPressure) ||
            enemyHasOpeningThreat(OpeningThreat::MassBarracks) ||
            enemyHasOpeningThreat(OpeningThreat::TerranVultureRush) ||
            enemyHasOpeningThreat(OpeningThreat::LingPressure);
    }

    bool enemyOpeningHeavyPressure()
    {
        return
            enemyHasOpeningThreat(OpeningThreat::DragoonPressure) ||
            enemyHasOpeningThreat(OpeningThreat::FactoryPressure) ||
            enemyHasOpeningThreat(OpeningThreat::HydraliskPressure) ||
            enemyHasOpeningThreat(OpeningThreat::ProtossFastRobotics);
    }

    bool enemyOpeningGroundPressure()
    {
        return
            enemyHasOpeningThreat(OpeningThreat::GatewayPressure) ||
            enemyOpeningLightPressure() ||
            enemyOpeningHeavyPressure();
    }

    bool enemyOpeningRushPressure()
    {
        // A scouted Factory commitment changes our mech plan, but it is not
        // by itself proof that bunkers and eight emergency Marines are needed.
        // The same applies to a normal 1 Gate Core: Prepare one bunker, but do
        // not confuse it with a committed multi-Gateway Dragoon attack.
        const bool normalOneGateCore =
            enemyHasOpeningThreat(OpeningThreat::ProtossOneGateCore) &&
            !enemyHasOpeningThreat(OpeningThreat::GatewayPressure) &&
            !enemyHasOpeningThreat(OpeningThreat::ProtossFourGateDragoon);

        const bool committedDragoonPressure =
            enemyHasOpeningThreat(OpeningThreat::DragoonPressure) &&
            !normalOneGateCore;

        return
            enemyHasOpeningThreat(OpeningThreat::GatewayPressure) ||
            enemyHasOpeningThreat(OpeningThreat::ZealotPressure) ||
            committedDragoonPressure ||
            enemyHasOpeningThreat(OpeningThreat::TerranVultureRush) ||
            enemyHasOpeningThreat(OpeningThreat::MassBarracks);
    }

    BWAPI::Position getMainBaseExitApproxPosition();
    bool tileIsBuildableFor(const BWAPI::TilePosition& tile, BWAPI::UnitType type);
    BWAPI::TilePosition findBuildTileNearBaseExit(BWAPI::UnitType type);
    BWAPI::TilePosition findDefensiveBunkerTile(Base* base, int bunkerIndex);
    bool naturalExpansionStarted();
    bool queueNaturalExpansionBunker(BuildOrderQueue& queue);
    bool queueBioTankMainWallBunker(BuildOrderQueue& queue);
    BWAPI::TilePosition findSiegeExpandBunkerTile(int bunkerIndex);
    bool queueSiegeExpandBunker(BuildOrderQueue& queue);
    bool visibleEnemyAirOnlyNearOwnedBase();

    void addUniqueTurretTile(std::vector<BWAPI::TilePosition>& tiles, const BWAPI::TilePosition& tile);
    BWAPI::Position positionToward(const BWAPI::Position& from, const BWAPI::Position& to, int distance);
    BWAPI::TilePosition tankLineTurretTile();
    BWAPI::Position approximateEnemyDirectionPositionForTurrets(Base* base);
    std::vector<Base*> ownedTerranBasesForTurrets();
    Base* nearestOwnedTurretBase(const BWAPI::TilePosition& tile);
    std::vector<BWAPI::TilePosition> committedTurretTiles();
    int committedTurretCountForBase(Base* base);
    int strategicTurretGlobalLimit();
    bool turretCommittedNearTile(const BWAPI::TilePosition& tile, int maxTileDistance);
    BWAPI::TilePosition findBuildableTurretTileNear(
        const BWAPI::Position& wantedPosition,
        Base* base,
        const std::vector<BWAPI::TilePosition>& alreadyUsed,
        int minTileDistance);
    bool addTurretNearPosition(
        std::vector<BWAPI::TilePosition>& tiles,
        Base* base,
        const BWAPI::Position& wantedPosition,
        int minTileDistance,
        int& remainingNewSlots);
    void addPerimeterTurretsAroundBase(
        std::vector<BWAPI::TilePosition>& tiles,
        Base* base,
        int desiredTurrets,
        int minTileDistance);
    std::vector<BWAPI::TilePosition> strategicAirDefenseTurretTiles(
        int turretsPerBase);
    std::vector<BWAPI::TilePosition> emergencyTurretTiles();
    std::vector<BWAPI::TilePosition> wraithDefenseTurretTiles();
    std::vector<BWAPI::TilePosition> carrierDefenseTurretTiles();
    bool enemyTerranTankCoreDetected();
    bool tileAlreadyCoveredByTurretTile(
        const std::vector<BWAPI::TilePosition>& tiles,
        const BWAPI::TilePosition& tile,
        int minTileDistance);

    const char* enemyArmyModeToString(EnemyArmyMode mode)
    {
        switch (mode)
        {
        case EnemyArmyMode::PureAir:              return "PureAir";
        case EnemyArmyMode::HeavyGround:          return "HeavyGround";
        case EnemyArmyMode::LightGround:          return "LightGround";
        case EnemyArmyMode::MixedAirHeavyGround:  return "MixedAirHeavyGround";
        case EnemyArmyMode::MixedAirLightGround:  return "MixedAirLightGround";
        case EnemyArmyMode::Default:              return "Default";
        default:                                  return "Unknown";
        }
    }

    BWAPI::Position getBaseEntranceApproxPosition(Base* base)
    {
        if (!base || !base->getCenter().isValid())
        {
            return BWAPI::Positions::Invalid;
        }

        const BWAPI::Position basePos = base->getCenter();

        // One source of truth for base topology. Base::initializeFront() already
        // resolves and ground-validates the canonical Area/Choke route; bunker
        // and turret code must not independently invent a second entrance.
        const ChokePoint * frontChoke = base->getFrontChoke();
        if (frontChoke && frontChoke->center().isValid())
        {
            return frontChoke->center();
        }

        // On open maps Base::initializeFront() already found a reachable
        // outward point. A second narrowing search can choose a bend by the
        // minerals and shift the natural Bunker away from the approach.
        if (base->getFront().isValid())
        {
            return base->getFront();
        }

        // Open/unusual maps can legitimately have no choke. Keep the old
        // shortest-path/TileRoom heuristic only as a fallback for that case.
        BWAPI::Position outsidePos = BWAPI::Positions::Invalid;

        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();
        Base* enemyBase = the.bases.enemyStart();

        if (base == mainBase && naturalBase && naturalBase->getCenter().isValid())
        {
            outsidePos = naturalBase->getCenter();
        }
        else if (base->getMain() && base->getMain()->getCenter().isValid())
        {
            // A natural should face the real route toward the opponent/map
            // center. A simple ray away from main can point into a cliff on
            // Benzene and select the wrong exit.
            if (enemyBase && enemyBase->getCenter().isValid() &&
                base->getTileDistance(enemyBase->getCenterTile()) >= 0)
            {
                outsidePos = enemyBase->getCenter();
            }

            BWAPI::Position desired = outsidePos;
            if (!desired.isValid())
            {
                desired = BWAPI::Position(
                    BWAPI::Broodwar->mapWidth() * 16,
                    BWAPI::Broodwar->mapHeight() * 16);
            }

            auto findReachableNear = [&](const BWAPI::Position& wanted) -> BWAPI::Position
            {
                BWAPI::Position wantedValid = wanted;
                wantedValid.makeValid();
                const BWAPI::TilePosition desiredTile(wantedValid);
                BWAPI::Position bestPosition = BWAPI::Positions::Invalid;
                int bestScore = MAX_DISTANCE;
                for (int radius = 0; radius <= 14; ++radius)
                {
                    for (int ox = -radius; ox <= radius; ++ox)
                    {
                        for (int oy = -radius; oy <= radius; ++oy)
                        {
                            if (std::max(std::abs(ox), std::abs(oy)) != radius)
                            {
                                continue;
                            }
                            const BWAPI::TilePosition tile(desiredTile.x + ox, desiredTile.y + oy);
                            if (!tile.isValid() || !the.map.isWalkable(tile) ||
                                base->getTileDistance(tile) < 0)
                            {
                                continue;
                            }
                            const BWAPI::Position p = TileCenter(tile);
                            const int score = p.getApproxDistance(wanted);
                            if (score < bestScore)
                            {
                                bestScore = score;
                                bestPosition = p;
                            }
                        }
                    }
                    if (bestPosition.isValid())
                    {
                        break;
                    }
                }
                return bestPosition;
            };

            if (!outsidePos.isValid())
            {
                outsidePos = findReachableNear(desired);
            }

            if (!outsidePos.isValid())
            {
                const BWAPI::Position mainPos = base->getMain()->getCenter();
                double dx = double(basePos.x - mainPos.x);
                double dy = double(basePos.y - mainPos.y);
                const double len = std::sqrt(dx * dx + dy * dy);
                if (len > 1.0)
                {
                    const BWAPI::Position away(
                        int(basePos.x + dx / len * 20.0 * 32),
                        int(basePos.y + dy / len * 20.0 * 32));
                    outsidePos = findReachableNear(away);
                }
            }
        }
        else if (enemyBase && enemyBase->getCenter().isValid())
        {
            outsidePos = enemyBase->getCenter();
        }
        else if (base->getFront().isValid())
        {
            outsidePos = base->getFront();
        }

        if (!outsidePos.isValid())
        {
            return BWAPI::Positions::Invalid;
        }

        std::vector<BWAPI::TilePosition> path;
        const int maxPathSteps = base == mainBase ? 28 : 34;
        if (!the.map.getGroundPath(
                base->getCenterTile(), BWAPI::TilePosition(outsidePos),
                path, maxPathSteps) || path.size() < 5)
        {
            return base->getFront();
        }

        const int idealIndex = base == mainBase ? 9 : 11;
        const int lastIndex = std::min(int(path.size()) - 1,
            base == mainBase ? 18 : 23);

        const BWAPI::TilePosition mineralTile = base->getMineralLineTile();
        const BWAPI::Position mineralPos = mineralTile.isValid()
            ? TileCenter(mineralTile)
            : BWAPI::Positions::Invalid;

        int bestIndex = -1;
        int bestScore = MAX_DISTANCE;
        for (int i = 4; i <= lastIndex; ++i)
        {
            const BWAPI::TilePosition tile = path[i];
            if (!tile.isValid() || !the.map.isWalkable(tile))
            {
                continue;
            }

            const int room = the.tileRoom.at(tile);
            if (room <= 0)
            {
                continue;
            }

            int score = room * 90 + std::abs(i - idealIndex) * 28;

            // Prefer a LOCAL narrowing, not merely a random low-room tile next
            // to the minerals or map edge.
            if (i > 0 && i + 1 < int(path.size()))
            {
                const int before = std::max(1, int(the.tileRoom.at(path[i - 1])));
                const int after = std::max(1, int(the.tileRoom.at(path[i + 1])));
                const int neighborAverage = (before + after) / 2;
                if (room <= neighborAverage)
                {
                    score -= std::min(500, (neighborAverage - room) * 45);
                }
            }

            if (mineralPos.isValid())
            {
                const int mineralDistance = TileCenter(tile).getApproxDistance(mineralPos);
                if (mineralDistance < 6 * 32)
                {
                    score += 5000;
                }
                else if (mineralDistance < 8 * 32)
                {
                    score += 800;
                }
            }

            if (score < bestScore)
            {
                bestScore = score;
                bestIndex = i;
            }
        }

        if (bestIndex < 0)
        {
            bestIndex = std::min(idealIndex, int(path.size()) - 1);
        }

        const BWAPI::Position entrance = TileCenter(path[bestIndex]);
        DebugQueuePrintf(
            "base entrance path: base=(%d,%d) step=%d room=%d entrance=(%d,%d)",
            base->getTilePosition().x, base->getTilePosition().y,
            bestIndex, int(the.tileRoom.at(path[bestIndex])),
            path[bestIndex].x, path[bestIndex].y);
        return entrance;
    }


    BWAPI::Position getBunkerEntrancePosition(Base* base)
    {
        if (!base) return BWAPI::Positions::Invalid;
        if (base->getFrontChoke()) return getBaseEntranceApproxPosition(base);

        // An eight-tile staging front is not necessarily the ramp. On maps
        // without a recognized ChokePoint, find the first real floor change
        // along the outward ground route before applying any inside offset.
        Base* natural = the.bases.myNatural();
        Base* enemy = the.bases.enemyStart();
        Base* reference = base == the.bases.myMain() ? natural : enemy;
        if (!reference || reference == base ||
            base->getTileDistance(reference->getCenterTile()) < 0)
        {
            int nearest = MAX_DISTANCE;
            reference = nullptr;
            for (Base* start : the.bases.getStarting())
            {
                if (!start || start == base || start == base->getMain()) continue;
                const int distance = base->getTileDistance(start->getCenterTile());
                if (distance >= 0 && distance < nearest)
                {
                    nearest = distance;
                    reference = start;
                }
            }
        }
        if (!reference) return getBaseEntranceApproxPosition(base);
        const BWAPI::Position outside = reference->getCenter();
        struct EntranceCache
        {
            BWAPI::Position origin, outside, entrance;
            unsigned int revision;
        };
        static std::map<int, EntranceCache> cache;
        static int lastFrame = -1;
        if (the.now() < lastFrame) cache.clear();
        lastFrame = the.now();
        const auto previous = cache.find(base->getID());
        if (previous != cache.end() && previous->second.origin == base->getCenter() &&
            previous->second.outside == outside &&
            previous->second.revision == the.partitions.getRevision())
            return previous->second.entrance;
        BWAPI::Position entrance = getBaseEntranceApproxPosition(base);
        std::vector<BWAPI::TilePosition> path;
        if (the.map.getGroundPath(base->getCenterTile(), BWAPI::TilePosition(outside), path, 64))
        {
            const int height = GroundHeight(base->getCenterTile());
            for (int i = 4; i < std::min(32, int(path.size())); ++i)
            {
                if (GroundHeight(path[i]) != height)
                {
                    entrance = TileCenter(path[i - 1]);
                    break;
                }
            }
        }
        cache[base->getID()] = { base->getCenter(), outside, entrance,
            the.partitions.getRevision() };
        return entrance;
    }

    BWAPI::Position approximateEnemyDirectionPositionForTurrets(Base* base)
    {
        if (!base || !base->getPosition().isValid())
        {
            return BWAPI::Position(
                BWAPI::Broodwar->mapWidth() * 16,
                BWAPI::Broodwar->mapHeight() * 16
            );
        }

        BWAPI::Position basePosition = base->getPosition();

        BWAPI::Position bestEnemyPosition = BWAPI::Positions::Invalid;
        int bestDistance = MAX_DISTANCE;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible())
            {
                continue;
            }

            if (!enemy->getPosition().isValid())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (type.isWorker() || type.isSpell())
            {
                continue;
            }

            if (!type.isBuilding() &&
                !UnitUtil::CanAttackGround(enemy) &&
                !UnitUtil::CanAttackAir(enemy))
            {
                continue;
            }

            const int distance = enemy->getDistance(basePosition);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestEnemyPosition = enemy->getPosition();
            }
        }

        if (bestEnemyPosition.isValid())
        {
            return bestEnemyPosition;
        }

        const BWAPI::TilePosition myStartTile =
            BWAPI::Broodwar->self()->getStartLocation();

        if (myStartTile.isValid())
        {
            BWAPI::Position myStartPosition(myStartTile);
            BWAPI::Position farthestStartPosition = BWAPI::Positions::Invalid;
            int farthestDistance = -1;

            for (const BWAPI::TilePosition& startTile : BWAPI::Broodwar->getStartLocations())
            {
                if (!startTile.isValid() || startTile == myStartTile)
                {
                    continue;
                }

                BWAPI::Position startPosition(startTile);
                const int distance = startPosition.getApproxDistance(myStartPosition);

                if (distance > farthestDistance)
                {
                    farthestDistance = distance;
                    farthestStartPosition = startPosition;
                }
            }

            if (farthestStartPosition.isValid())
            {
                return farthestStartPosition;
            }
        }

        return BWAPI::Position(
            BWAPI::Broodwar->mapWidth() * 16,
            BWAPI::Broodwar->mapHeight() * 16
        );
    }

    std::vector<Base*> ownedTerranBasesForTurrets()
    {
        std::vector<Base*> bases;

        const auto addIfEligible = [&](Base* base)
        {
            if (!base ||
                std::find(bases.begin(), bases.end(), base) != bases.end())
            {
                return;
            }

            BWAPI::Unit depot = base->getDepot();
            if (base->getOwner() != the.self() ||
                !depot || !depot->exists() || !depot->isCompleted() ||
                depot->isLifted() ||
                depot->getType() != BWAPI::UnitTypes::Terran_Command_Center)
            {
                return;
            }

            bases.push_back(base);
        };

        // Stable strategic order: protect the main workers first, then the
        // natural/front, then any later economy. This also keeps rule keys from
        // being reordered as ownership information is refreshed.
        addIfEligible(the.bases.myMain());
        addIfEligible(the.bases.myNatural());
        addIfEligible(the.bases.myFront());

        std::vector<Base*> remaining;
        for (Base* base : the.bases.getAll())
        {
            if (base && std::find(bases.begin(), bases.end(), base) == bases.end())
            {
                remaining.push_back(base);
            }
        }
        std::sort(
            remaining.begin(), remaining.end(),
            [](Base* first, Base* second)
            {
                return first && second
                    ? first->getID() < second->getID()
                    : first != nullptr;
            });
        for (Base* base : remaining)
        {
            addIfEligible(base);
        }

        return bases;
    }

    Base* nearestOwnedTurretBase(const BWAPI::TilePosition& tile)
    {
        if (!tile.isValid())
        {
            return nullptr;
        }

        const BWAPI::Position position = TileCenter(tile);
        Base* bestBase = nullptr;
        int bestDistance = MAX_DISTANCE;

        for (Base* base : ownedTerranBasesForTurrets())
        {
            const int distance =
                position.getApproxDistance(base->getCenter());
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestBase = base;
            }
        }

        return bestBase;
    }

    std::vector<BWAPI::TilePosition> committedTurretTiles()
    {
        std::vector<BWAPI::TilePosition> tiles;
        const BWAPI::UnitType turretType =
            BWAPI::UnitTypes::Terran_Missile_Turret;

        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (unit && unit->exists() && unit->getType() == turretType &&
                unit->getTilePosition().isValid())
            {
                addUniqueTurretTile(tiles, unit->getTilePosition());
            }
        }

        for (const BWAPI::TilePosition& tile :
            BuildingManager::Instance().getUnstartedBuildingTiles(turretType))
        {
            addUniqueTurretTile(tiles, tile);
        }

        BuildOrderQueue& queue =
            ProductionManager::Instance().getQueue();
        for (int i = 0; i < int(queue.size()); ++i)
        {
            const MacroAct& act = queue[i].macroAct;
            if (act.isUnit() && act.getUnitType() == turretType &&
                act.getMacroLocation() == MacroLocation::Tile &&
                act.getTileLocation().isValid())
            {
                addUniqueTurretTile(tiles, act.getTileLocation());
            }
        }

        return tiles;
    }

    int committedTurretCountForBase(Base* base)
    {
        if (!base)
        {
            return 0;
        }

        int count = 0;
        for (const BWAPI::TilePosition& tile : committedTurretTiles())
        {
            if (nearestOwnedTurretBase(tile) == base)
            {
                ++count;
            }
        }
        return count;
    }

    int strategicTurretGlobalLimit()
    {
        return MAX_TURRETS_PER_OWNED_BASE *
            std::max(1, int(ownedTerranBasesForTurrets().size()));
    }

    bool turretCommittedNearTile(
        const BWAPI::TilePosition& tile,
        int maxTileDistance)
    {
        if (!tile.isValid())
        {
            return false;
        }

        for (const BWAPI::TilePosition& committed : committedTurretTiles())
        {
            const int dx = committed.x - tile.x;
            const int dy = committed.y - tile.y;
            if (dx * dx + dy * dy <=
                maxTileDistance * maxTileDistance)
            {
                return true;
            }
        }
        return false;
    }

    BWAPI::TilePosition findCommittedTurretTileNear(
        const BWAPI::Position& wantedPosition,
        Base* base,
        const std::vector<BWAPI::TilePosition>& alreadyUsed,
        int minTileDistance)
    {
        BWAPI::TilePosition best = BWAPI::TilePositions::Invalid;
        int bestDistance = MAX_DISTANCE;

        for (const BWAPI::TilePosition& tile : committedTurretTiles())
        {
            if (nearestOwnedTurretBase(tile) != base ||
                tileAlreadyCoveredByTurretTile(
                    alreadyUsed, tile, minTileDistance))
            {
                continue;
            }

            const int distance = TileCenter(tile).getApproxDistance(wantedPosition);
            if (distance <= 8 * 32 && distance < bestDistance)
            {
                bestDistance = distance;
                best = tile;
            }
        }

        return best;
    }

    BWAPI::TilePosition findBuildableTurretTileNear(
        const BWAPI::Position& wantedPosition,
        Base* base,
        const std::vector<BWAPI::TilePosition>& alreadyUsed,
        int minTileDistance)
    {
        if (!wantedPosition.isValid() || !base)
        {
            return BWAPI::TilePositions::Invalid;
        }

        const BWAPI::UnitType turretType = BWAPI::UnitTypes::Terran_Missile_Turret;
        const BWAPI::TilePosition centerTile(wantedPosition);

        BWAPI::TilePosition bestTile = BWAPI::TilePositions::Invalid;
        int bestScore = MAX_DISTANCE;

        const int searchRadius = 9;

        // Search nearest rings first. The previous full 15x15 scan called
        // canBuildHere() for every candidate at every owned base whenever the
        // strategic plan refreshed, causing large Produce-frame spikes.
        for (int radius = 0; radius <= searchRadius; ++radius)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                for (int dy = -radius; dy <= radius; ++dy)
                {
                    if (std::max(std::abs(dx), std::abs(dy)) != radius)
                    {
                        continue;
                    }

                    BWAPI::TilePosition tile(centerTile.x + dx, centerTile.y + dy);
                    if (!tile.isValid())
                    {
                        continue;
                    }

                    // The closest buildable tile can be the same one chosen
                    // for a previous ring slot. Skip it here so the search
                    // can actually find the next distinct turret position.
                    if (tileAlreadyCoveredByTurretTile(
                            alreadyUsed, tile, minTileDistance))
                    {
                        continue;
                    }

                    BWAPI::Position tileCenter(
                        tile.x * 32 + turretType.tileWidth() * 16,
                        tile.y * 32 + turretType.tileHeight() * 16);

                    // Reject cheap geometric failures before the expensive
                    // BWAPI footprint/buildability query.
                    if (tileCenter.getApproxDistance(base->getCenter()) > 11 * 32 ||
                        nearestOwnedTurretBase(tile) != base ||
                        !tileIsBuildableFor(tile, turretType))
                    {
                        continue;
                    }

                    const int score = tileCenter.getApproxDistance(wantedPosition);

                    if (score < bestScore)
                    {
                        bestScore = score;
                        bestTile = tile;
                    }
                }
            }
            if (bestTile.isValid())
            {
                break;
            }
        }

        return bestTile;
    }

    // Cover the former Barracks gate without occupying the gate or its
    // reserved movement lane. Turret detection reaches the gate from here;
    // ground defenders still have to attack any unit it reveals.
    BWAPI::TilePosition findMainWallCloakTurretTile()
    {
        Base* main = the.bases.myMain();
        const BWAPI::Position gate = the.placer.getTerranMainWallGateCenter();
        if (!main || !main->getCenter().isValid() || !gate.isValid())
        {
            return BWAPI::TilePositions::Invalid;
        }

        const BWAPI::Position inside = main->getCenter();
        const double dx = double(inside.x - gate.x);
        const double dy = double(inside.y - gate.y);
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length < 1.0)
        {
            return BWAPI::TilePositions::Invalid;
        }

        const BWAPI::Position wanted(
            gate.x + int(dx / length * 4 * 32),
            gate.y + int(dy / length * 4 * 32));
        const BWAPI::TilePosition centerTile(wanted);
        const BWAPI::UnitType turret = BWAPI::UnitTypes::Terran_Missile_Turret;

        for (int radius = 0; radius <= 5; ++radius)
        {
            BWAPI::TilePosition best = BWAPI::TilePositions::Invalid;
            int bestScore = MAX_DISTANCE;
            for (int x = -radius; x <= radius; ++x)
            {
                for (int y = -radius; y <= radius; ++y)
                {
                    if (std::max(std::abs(x), std::abs(y)) != radius)
                    {
                        continue;
                    }

                    const BWAPI::TilePosition tile(centerTile.x + x, centerTile.y + y);
                    if (!tile.isValid())
                    {
                        continue;
                    }
                    const BWAPI::Position turretCenter(
                        tile.x * 32 + turret.tileWidth() * 16,
                        tile.y * 32 + turret.tileHeight() * 16);
                    const int gateDistance = turretCenter.getApproxDistance(gate);
                    if (gateDistance < 2 * 32 || gateDistance > 6 * 32 ||
                        !the.placer.isInsideTerranMainWall(turretCenter, -32) ||
                        !tileIsBuildableFor(tile, turret))
                    {
                        continue;
                    }

                    Building building(turret, tile);
                    building.macroLocation = MacroLocation::Main;
                    if (!the.placer.buildingOK(building, tile))
                    {
                        continue;
                    }

                    const int score = turretCenter.getApproxDistance(wanted);
                    if (score < bestScore)
                    {
                        bestScore = score;
                        best = tile;
                    }
                }
            }
            if (best.isValid())
            {
                return best;
            }
        }

        return BWAPI::TilePositions::Invalid;
    }

    bool mainWallCloakTurretCommitted()
    {
        const BWAPI::Position gate = the.placer.getTerranMainWallGateCenter();
        if (!gate.isValid())
        {
            return false;
        }
        const BWAPI::UnitType turret = BWAPI::UnitTypes::Terran_Missile_Turret;
        for (const BWAPI::TilePosition& tile : committedTurretTiles())
        {
            const BWAPI::Position center(
                tile.x * 32 + turret.tileWidth() * 16,
                tile.y * 32 + turret.tileHeight() * 16);
            if (center.getApproxDistance(gate) <= 6 * 32)
            {
                return true;
            }
        }
        return false;
    }

    static bool shouldQueueOneSCVForUndersaturatedBase(const BuildOrderQueue& queue)
    {
        

        if (queue.anyInQueue(BWAPI::UnitTypes::Terran_SCV))
        {
            return false;
        }

        if (the.my.all.count(BWAPI::UnitTypes::Terran_SCV) >=
            WorkerManager::Instance().getMaxWorkers())
        {
            return false;
        }

        bool hasIdleCompletedCC = false;

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit ||
                !unit->exists() ||
                !unit->isCompleted() ||
                unit->isLifted() ||
                unit->getType() != BWAPI::UnitTypes::Terran_Command_Center)
            {
                continue;
            }

            if (!unit->isTraining())
            {
                hasIdleCompletedCC = true;
                break;
            }
        }

        if (!hasIdleCompletedCC)
        {
            return false;
        }

        for (Base* base : the.bases.getAll())
        {
            if (!base)
            {
                continue;
            }

            if (base->getOwner() != the.self())
            {
                continue;
            }

            BWAPI::Unit depot = base->getDepot();

            if (!depot ||
                !depot->exists() ||
                depot->getPlayer() != the.self() ||
                depot->getType() != BWAPI::UnitTypes::Terran_Command_Center ||
                !depot->isCompleted() ||
                depot->isLifted())
            {
                continue;
            }

            if (base->inWorkerDanger())
            {
                continue;
            }

            const int currentWorkers =
                base->getNumWorkers();

            const int maxWorkers =
                base->getMaxWorkers();

            if (currentWorkers < maxWorkers)
            {
                return true;
            }
        }

        return false;
    }

    static BWAPI::TilePosition getMainOnlyBunkerTile(int bunkerIndex)
    {
        Base* mainBase = the.bases.myMain();
        return findDefensiveBunkerTile(mainBase, bunkerIndex);
    }

    bool addTurretNearPosition(
        std::vector<BWAPI::TilePosition>& tiles,
        Base* base,
        const BWAPI::Position& wantedPosition,
        int minTileDistance,
        int& remainingNewSlots)
    {
        // Reuse an existing, queued or SCV-assigned Turret first. This makes a
        // positional rule stable while construction is pending and prevents a
        // blocked old slot from spawning a fresh neighbour every second.
        BWAPI::TilePosition tile = findCommittedTurretTileNear(
            wantedPosition, base, tiles, minTileDistance);

        if (tile.isValid())
        {
            tiles.push_back(tile);
            return true;
        }

        if (remainingNewSlots <= 0)
        {
            return false;
        }

        tile = findBuildableTurretTileNear(
            wantedPosition, base, tiles, minTileDistance);

        if (!tile.isValid())
        {
            return false;
        }

        if (tileAlreadyCoveredByTurretTile(tiles, tile, minTileDistance))
        {
            return false;
        }

        tiles.push_back(tile);
        --remainingNewSlots;
        return true;
    }

    void addPerimeterTurretsAroundBase(
        std::vector<BWAPI::TilePosition>& tiles,
        Base* base,
        int desiredTurrets,
        int minTileDistance)
    {
        if (!base || !base->getCenter().isValid() || desiredTurrets <= 0)
        {
            return;
        }

        desiredTurrets = std::min(
            desiredTurrets, MAX_TURRETS_PER_OWNED_BASE);

        const BWAPI::Position center = base->getCenter();
        const BWAPI::Position mineralOffset = base->getMineralOffset();

        // Slot zero points at the mineral line. The first three positions are
        // a compact worker arc; each is inside the other two Turrets' air range.
        // The remaining order fills the opposite side and then the gaps, so six
        // or more positions form a mutually supporting ring rather than a row.
        double dx = double(mineralOffset.x);
        double dy = double(mineralOffset.y);
        double len = std::sqrt(dx * dx + dy * dy);

        if (len < 1.0)
        {
            const BWAPI::Position entrance = getBaseEntranceApproxPosition(base);
            dx = entrance.isValid() ? double(center.x - entrance.x) : 1.0;
            dy = entrance.isValid() ? double(center.y - entrance.y) : 0.0;
            len = std::max(1.0, std::sqrt(dx * dx + dy * dy));
        }

        dx /= len;
        dy /= len;

        const double baseAngle = std::atan2(dy, dx);
        const double pi = 3.14159265358979323846;
        const int workerArcRadius = 4 * 32;
        const int ringRadius = 5 * 32;
        const int angleOrder[MAX_TURRETS_PER_OWNED_BASE] =
        {
            0, 36, -36, 180, 108, -108, 72, -72, 144, -144
        };

        int remainingNewSlots = std::max(
            0,
            MAX_TURRETS_PER_OWNED_BASE -
                committedTurretCountForBase(base));
        const size_t initialTileCount = tiles.size();

        for (int i = 0;
             i < MAX_TURRETS_PER_OWNED_BASE &&
                 int(tiles.size() - initialTileCount) < desiredTurrets;
             ++i)
        {
            const double angle =
                baseAngle + double(angleOrder[i]) * pi / 180.0;
            const int radius = i < 3 ? workerArcRadius : ringRadius;
            const BWAPI::Position wanted(
                center.x + int(std::cos(angle) * radius),
                center.y + int(std::sin(angle) * radius));

            addTurretNearPosition(
                tiles,
                base,
                wanted,
                minTileDistance,
                remainingNewSlots);
        }

        // Highly irregular mineral formations can make one nominal point
        // impossible. Try the half-step gaps at the same safe radius before
        // accepting a smaller ring; the hard per-base cap still applies.
        for (int i = 0;
             i < MAX_TURRETS_PER_OWNED_BASE &&
                 int(tiles.size() - initialTileCount) < desiredTurrets;
             ++i)
        {
            const double angle =
                baseAngle + double(18 + i * 36) * pi / 180.0;
            const BWAPI::Position wanted(
                center.x + int(std::cos(angle) * ringRadius),
                center.y + int(std::sin(angle) * ringRadius));

            addTurretNearPosition(
                tiles,
                base,
                wanted,
                minTileDistance,
                remainingNewSlots);
        }
    }

    std::vector<BWAPI::TilePosition> strategicAirDefenseTurretTiles(
        int turretsPerBase)
    {
        std::vector<std::vector<BWAPI::TilePosition>> layouts;
        for (Base* base : ownedTerranBasesForTurrets())
        {
            std::vector<BWAPI::TilePosition> layout;
            addPerimeterTurretsAroundBase(
                layout,
                base,
                std::min(turretsPerBase, MAX_TURRETS_PER_OWNED_BASE),
                TURRET_RING_MIN_TILE_DISTANCE);
            layouts.push_back(layout);
        }

        std::vector<BWAPI::TilePosition> result;

        // Every mineral line gets its first three points before any one base
        // receives the expensive outer ring.
        for (const auto& layout : layouts)
        {
            for (size_t i = 0; i < std::min<size_t>(3, layout.size()); ++i)
            {
                addUniqueTurretTile(result, layout[i]);
            }
        }
        for (const auto& layout : layouts)
        {
            for (size_t i = 3; i < layout.size(); ++i)
            {
                addUniqueTurretTile(result, layout[i]);
            }
        }

        return result;
    }


    int completedGoliathCount()
    {
        return the.my.completed.count(BWAPI::UnitTypes::Terran_Goliath);
    }

    int completedTankCount()
    {
        return
            the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
    }
    
    int visibleEnemyTerranTankCount()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return 0;
        }

        int count = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            {
                ++count;
            }
        }

        return count;
    }

    int visibleEnemyTerranSiegedTankCount()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return 0;
        }

        int count = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible())
            {
                continue;
            }

            if (enemy->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            {
                ++count;
            }
        }

        return count;
    }

    BWAPI::Position rememberedEnemySiegeWallPosition = BWAPI::Positions::Invalid;
    int rememberedEnemySiegeWallFrame = -100000;
    int lastEnemySiegeWallComsatFrame = -100000;

    BWAPI::Position visibleEnemyTerranSiegeWallCenter()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return BWAPI::Positions::Invalid;
        }

        int sumX = 0;
        int sumY = 0;
        int count = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible())
            {
                continue;
            }

            if (!enemy->getPosition().isValid())
            {
                continue;
            }

            if (enemy->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            {
                continue;
            }

            sumX += enemy->getPosition().x;
            sumY += enemy->getPosition().y;
            ++count;
        }

        if (count < 2)
        {
            sumX = 0;
            sumY = 0;
            count = 0;

            for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
            {
                if (!enemy || !enemy->exists() || !enemy->isVisible())
                {
                    continue;
                }

                if (!enemy->getPosition().isValid())
                {
                    continue;
                }

                const BWAPI::UnitType type = enemy->getType();

                if (type != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
                    type != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
                {
                    continue;
                }

                sumX += enemy->getPosition().x;
                sumY += enemy->getPosition().y;
                ++count;
            }
        }

        if (count <= 0)
        {
            return BWAPI::Positions::Invalid;
        }

        return BWAPI::Position(sumX / count, sumY / count);
    }

    void rememberVisibleEnemySiegeWallPosition()
    {
        const BWAPI::Position center = visibleEnemyTerranSiegeWallCenter();

        if (!center.isValid())
        {
            return;
        }

        rememberedEnemySiegeWallPosition = center;
        rememberedEnemySiegeWallFrame = BWAPI::Broodwar->getFrameCount();
    }

    bool tryComsatRememberedEnemySiegeWall()
    {
        if (!rememberedEnemySiegeWallPosition.isValid())
        {
            return false;
        }

        const int now = BWAPI::Broodwar->getFrameCount();

        if (now < rememberedEnemySiegeWallFrame ||
            now < lastEnemySiegeWallComsatFrame)
        {
            rememberedEnemySiegeWallPosition = BWAPI::Positions::Invalid;
            rememberedEnemySiegeWallFrame = -100000;
            lastEnemySiegeWallComsatFrame = -100000;
            return false;
        }


        if (now - rememberedEnemySiegeWallFrame > 24 * 90)
        {
            return false;
        }

        if (now - lastEnemySiegeWallComsatFrame < 24 * 25)
        {
            return false;
        }


        if (visibleEnemyTerranSiegedTankCount() >= 2)
        {
            return false;
        }

        if (CombatCommander::Instance().requestComsatScan(
                rememberedEnemySiegeWallPosition,
                ComsatScanPriority::TankBattle,
                50,
                0,
                6 * 24,
                "remembered siege line"))
        {
            lastEnemySiegeWallComsatFrame = now;
            return true;
        }

        return false;
    }

    int knownEnemyTerranTankCount()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return 0;
        }

        const int seenTanks =
            the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);

        return std::max(seenTanks, visibleEnemyTerranTankCount());
    }

    int visibleEnemyTerranBioCount()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return 0;
        }

        int count = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (type == BWAPI::UnitTypes::Terran_Marine ||
                type == BWAPI::UnitTypes::Terran_Medic)
            {
                ++count;
            }
        }

        return count;
    }

    int knownEnemyTerranBioCount()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return 0;
        }

        const int seenBio =
            the.your.seen.count(BWAPI::UnitTypes::Terran_Marine) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Medic);

        return std::max(seenBio, visibleEnemyTerranBioCount());
    }

    bool enemyTerranBioTankPushDetected()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return false;
        }

        const int visibleBio = visibleEnemyTerranBioCount();
        const int knownBio = knownEnemyTerranBioCount();

        const int visibleTanks = visibleEnemyTerranTankCount();
        const int knownTanks = knownEnemyTerranTankCount();

        if (visibleBio >= 10 && visibleTanks >= 2)
        {
            return true;
        }

        if (knownBio >= 16 && knownTanks >= 3 && visibleTanks >= 1)
        {
            return true;
        }

        if (visibleBio >= 18 && knownTanks >= 2)
        {
            return true;
        }

        return false;
    }

    bool armoryExistsOrIsBeingBuiltOrQueued(const BuildOrderQueue& queue)
    {
        return
            the.my.all.count(BWAPI::UnitTypes::Terran_Armory) > 0 ||
            BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Armory) ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Armory);
    }

    bool enemyTerranHeavyMechDetected()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return false;
        }

        const int enemyTanks =
            the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);

        const int enemyGoliaths =
            the.your.seen.count(BWAPI::UnitTypes::Terran_Goliath);

        return enemyTanks >= 18 && enemyGoliaths >= 6;
    }

    bool enemyTerranTankCoreDetected()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return false;
        }

        const int enemyTanks =
            the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);

        // Two known Tanks are already a real ground transition. The old
        // heavy-mech check waited for 18 Tanks plus 6 Goliaths, long after a
        // Wraith opener had become a Tank army and could punish an AA lock-in.
        return enemyTanks >= 2;
    }

    int desiredRefineryCountForOwnedTerranBases()
    {
        int desired = 0;

        for (Base* base : the.bases.getAll())
        {
            if (!base)
            {
                continue;
            }

            BWAPI::Unit depot = base->getDepot();

            if (!depot || !depot->exists())
            {
                continue;
            }

            if (depot->getPlayer() != the.self())
            {
                continue;
            }

            if (depot->getType() != BWAPI::UnitTypes::Terran_Command_Center)
            {
                continue;
            }

            if (depot->isLifted())
            {
                continue;
            }

            // The live geyser set is rebuilt from visible units and can be
            // temporarily empty. Initial geysers describe the base's geography
            // even while the live BWAPI handles are unavailable.
            if (!base->getInitialGeysers().empty())
            {
                ++desired;
            }
        }

        return desired;
    }

    bool actualEarlyLightGroundRushDetected()
    {
        const int now = BWAPI::Broodwar->getFrameCount();
        const int RUSH_TIME_LIMIT = 9 * 24 * 60;

        if (now > RUSH_TIME_LIMIT)
        {
            return false;
        }

        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();

        if (!mainBase && !naturalBase)
        {
            return false;
        }

        const int MAIN_RADIUS = 26 * 32;
        const int NATURAL_RADIUS = 22 * 32;

        int lightGroundCount = 0;
        int lightGroundScore = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy ||
                !enemy->exists() ||
                !enemy->isVisible() ||
                !enemy->getPosition().isValid() ||
                enemy->isFlying())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (type.isWorker() || type.isBuilding() || type.isSpell())
            {
                continue;
            }

            bool nearOurBase = false;

            if (mainBase &&
                mainBase->getPosition().isValid() &&
                enemy->getDistance(mainBase->getPosition()) <= MAIN_RADIUS)
            {
                nearOurBase = true;
            }

            if (naturalBase &&
                naturalBase->getPosition().isValid() &&
                enemy->getDistance(naturalBase->getPosition()) <= NATURAL_RADIUS)
            {
                nearOurBase = true;
            }

            if (!nearOurBase)
            {
                continue;
            }

            if (type == BWAPI::UnitTypes::Terran_Marine)
            {
                ++lightGroundCount;
                lightGroundScore += 1;
            }
            else if (type == BWAPI::UnitTypes::Terran_Medic)
            {
                ++lightGroundCount;
                lightGroundScore += 1;
            }
            else if (type == BWAPI::UnitTypes::Protoss_Zealot)
            {
                ++lightGroundCount;
                lightGroundScore += 2;
            }
            else if (type == BWAPI::UnitTypes::Zerg_Zergling)
            {
                ++lightGroundCount;
                lightGroundScore += 1;
            }
            else if (type == BWAPI::UnitTypes::Zerg_Hydralisk)
            {
                ++lightGroundCount;
                lightGroundScore += 2;
            }
        }

        return lightGroundCount >= 2 && lightGroundScore >= 4;
    }

    bool actualEarlyProtossZealotRushDetected()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Protoss)
        {
            return false;
        }

        const int now = BWAPI::Broodwar->getFrameCount();
        const int RUSH_TIME_LIMIT = 8 * 24 * 60;

        if (now > RUSH_TIME_LIMIT)
        {
            return false;
        }

        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();

        if (!mainBase && !naturalBase)
        {
            return false;
        }

        const int MAIN_RADIUS = 30 * 32;
        const int NATURAL_RADIUS = 26 * 32;

        int zealotsNearBase = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy ||
                !enemy->exists() ||
                !enemy->isVisible() ||
                !enemy->getPosition().isValid())
            {
                continue;
            }

            if (enemy->getType() != BWAPI::UnitTypes::Protoss_Zealot)
            {
                continue;
            }

            bool nearOurBase = false;

            if (mainBase &&
                mainBase->getPosition().isValid() &&
                enemy->getDistance(mainBase->getPosition()) <= MAIN_RADIUS)
            {
                nearOurBase = true;
            }

            if (naturalBase &&
                naturalBase->getPosition().isValid() &&
                enemy->getDistance(naturalBase->getPosition()) <= NATURAL_RADIUS)
            {
                nearOurBase = true;
            }

            if (nearOurBase)
            {
                ++zealotsNearBase;
            }
        }

        // 2 zealots early is already dangerous.
        // 3+ means we need serious emergency defense.
        return zealotsNearBase >= 2;
    }

    bool actualProtossDragoonPressureNearBaseDetected()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Protoss)
        {
            return false;
        }

        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();

        if (!mainBase && !naturalBase)
        {
            return false;
        }

        const int MAIN_RADIUS = 34 * 32;
        const int NATURAL_RADIUS = 30 * 32;

        int dragoonCount = 0;
        int heavyProtossScore = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy ||
                !enemy->exists() ||
                !enemy->isVisible() ||
                !enemy->getPosition().isValid() ||
                enemy->isFlying())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            bool nearOurBase = false;

            if (mainBase &&
                mainBase->getPosition().isValid() &&
                enemy->getDistance(mainBase->getPosition()) <= MAIN_RADIUS)
            {
                nearOurBase = true;
            }

            if (naturalBase &&
                naturalBase->getPosition().isValid() &&
                enemy->getDistance(naturalBase->getPosition()) <= NATURAL_RADIUS)
            {
                nearOurBase = true;
            }

            if (!nearOurBase)
            {
                continue;
            }

            if (type == BWAPI::UnitTypes::Protoss_Dragoon)
            {
                ++dragoonCount;
                heavyProtossScore += 3;
            }
            else if (type == BWAPI::UnitTypes::Protoss_Reaver)
            {
                heavyProtossScore += 6;
            }
            else if (type == BWAPI::UnitTypes::Protoss_Archon)
            {
                heavyProtossScore += 4;
            }
            else if (type == BWAPI::UnitTypes::Protoss_Zealot)
            {
                heavyProtossScore += 1;
            }
        }

        const int minute = BWAPI::Broodwar->getFrameCount() / (24 * 60);

        // Early 2-3 dragoons near base are already dangerous.
        if (minute <= 11 && dragoonCount >= 2)
        {
            return true;
        }

        // Later mass dragoon pressure.
        if (dragoonCount >= 4)
        {
            return true;
        }

        // Dragoon + reaver / archon pressure.
        if (dragoonCount >= 2 && heavyProtossScore >= 9)
        {
            return true;
        }

        return false;
    }


    bool ownedCompletedRefineryNeedsWorkers()
    {
        for (const Base* base : the.bases.getAll())
        {
            if (!base || base->getOwner() != the.self())
            {
                continue;
            }

            BWAPI::Unit depot = base->getDepot();

            if (!depot || !depot->exists() || !UnitUtil::IsCompletedResourceDepot(depot))
            {
                continue;
            }

            for (BWAPI::Unit geyser : base->getGeysers())
            {
                if (!geyser ||
                    !geyser->exists() ||
                    !geyser->getType().isRefinery() ||
                    !geyser->isCompleted() ||
                    geyser->getPlayer() != the.self())
                {
                    continue;
                }

                if (WorkerManager::Instance().getNumWorkers(geyser) < 3)
                {
                    return true;
                }
            }
        }

        return false;
    }
    
    static int totalBarracksCountIncludingPlanned(const BuildOrderQueue& queue)
    {
        return
            the.my.all.count(BWAPI::UnitTypes::Terran_Barracks) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Barracks) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Barracks);
    }

    // Remove the LOWEST-priority matching copy. Emergency code often promotes an
    // existing JSON item to the top; removing from the top first would delete the
    // very item we just made urgent and leave an old copy buried in the book.
    static bool removeOneQueuedUnitType(BuildOrderQueue& queue, BWAPI::UnitType type)
    {
        for (int i = 0; i < (int)queue.size(); ++i)
        {
            const BuildOrderItem item = queue[i];
            if (item.macroAct.isUnit() && item.macroAct.getUnitType() == type)
            {
                queue.removeAt((size_t)i);
                return true;
            }
        }
        return false;
    }

    static bool removeOneQueuedTechType(BuildOrderQueue& queue, BWAPI::TechType type)
    {
        for (int i = 0; i < (int)queue.size(); ++i)
        {
            const BuildOrderItem item = queue[i];
            if (item.macroAct.isTech() && item.macroAct.getTechType() == type)
            {
                queue.removeAt((size_t)i);
                return true;
            }
        }
        return false;
    }

    static bool removeOneQueuedUpgradeType(BuildOrderQueue& queue, BWAPI::UpgradeType type)
    {
        for (int i = 0; i < (int)queue.size(); ++i)
        {
            const BuildOrderItem item = queue[i];
            if (item.macroAct.isUpgrade() && item.macroAct.getUpgradeType() == type)
            {
                queue.removeAt((size_t)i);
                return true;
            }
        }
        return false;
    }

    // Emergency reactions must distinguish "somewhere in the JSON" from
    // "available in the next few production actions". If the requested item
    // already exists deeper in the queue, promote it instead of adding a duplicate.
    static bool promoteQueuedUnitToTop(BuildOrderQueue& queue, BWAPI::UnitType type)
    {
        if (queue.isEmpty())
        {
            return false;
        }

        for (int i = (int)queue.size() - 2; i >= 0; --i)
        {
            const MacroAct act = queue[i].macroAct;
            if (act.isUnit() && act.getUnitType() == type)
            {
                queue.pullToTop((size_t)i);
                return true;
            }
        }
        return false;
    }

    static bool promoteQueuedTechToTop(BuildOrderQueue& queue, BWAPI::TechType type)
    {
        if (queue.isEmpty())
        {
            return false;
        }

        const MacroAct& top = queue.getHighestPriorityItem().macroAct;
        if (top.isTech() && top.getTechType() == type)
        {
            return false;
        }

        for (int i = (int)queue.size() - 2; i >= 0; --i)
        {
            const MacroAct act = queue[i].macroAct;
            if (act.isTech() && act.getTechType() == type)
            {
                queue.pullToTop((size_t)i);
                return true;
            }
        }
        return false;
    }

    static bool promoteQueuedUpgradeToTop(
        BuildOrderQueue& queue,
        BWAPI::UpgradeType type)
    {
        if (queue.isEmpty())
        {
            return false;
        }

        for (int i = (int)queue.size() - 2; i >= 0; --i)
        {
            const MacroAct act = queue[i].macroAct;
            if (act.isUpgrade() && act.getUpgradeType() == type)
            {
                queue.pullToTop((size_t)i);
                return true;
            }
        }
        return false;
    }

    static int immediateQueuedUnitCount(
        const BuildOrderQueue& queue,
        BWAPI::UnitType type,
        int lookAhead = 3)
    {
        return (int)queue.numInNextN(type, lookAhead);
    }

    static bool buildingExistsOrIsCommitted(
        BWAPI::UnitType type,
        const BuildOrderQueue& queue,
        int lookAhead = 3)
    {
        return
            the.my.all.count(type) > 0 ||
            BuildingManager::Instance().isBeingBuilt(type) ||
            BuildingManager::Instance().getNumUnstarted(type) > 0 ||
            queue.numInNextN(type, lookAhead) > 0;
    }

    // Gas-dependent emergency plans must own their prerequisite too. Merely
    // promoting a Factory/Armory/Comsat to the top can deadlock forever when
    // the opening has not started gas yet (or a StopGas command was executed).
    // Return true only when we had to promote/queue a Refinery and the caller
    // should yield this frame; an existing/under-construction Refinery only
    // needs gas collection re-enabled and does not block the emergency action.
    static bool ensureUrgentGasIncome(
        BuildOrderQueue& queue,
        int gasNeededForNextAction,
        const char* reason)
    {
        WorkerManager::Instance().setCollectGas(true);

        if (the.self()->gas() >= gasNeededForNextAction)
        {
            return false;
        }

        const BWAPI::UnitType refinery =
            BWAPI::UnitTypes::Terran_Refinery;

        const bool refineryAlreadyCommitted =
            the.my.all.count(refinery) > 0 ||
            BuildingManager::Instance().isBeingBuilt(refinery) ||
            BuildingManager::Instance().getNumUnstarted(refinery) > 0;

        if (refineryAlreadyCommitted)
        {
            return false;
        }

        if (immediateQueuedUnitCount(queue, refinery, 2) > 0)
        {
            return true;
        }

        if (!promoteQueuedUnitToTop(queue, refinery))
        {
            queue.queueAsHighestPriority(refinery);
        }

        ProductionManager::Instance().invalidateTerranRulePlan(
            std::string("urgent gas prerequisite: ") +
            (reason ? reason : "gas-dependent plan"));

        DebugQueuePrintf(
            "queue: urgent Refinery for %s",
            reason ? reason : "gas-dependent plan");

        return true;
    }

    // Urgent reactions may pre-empt the opening only with an action that is
    // actually reachable. If a prerequisite is already under construction,
    // leave the JSON queue running while it finishes. If it is deeper in the
    // queue, promote that prerequisite rather than the blocked final action.
    static bool waitForUrgentTerranPrerequisite(
        BuildOrderQueue& queue,
        const MacroAct& desiredAct,
        const char* reason)
    {
        

        const BWAPI::UnitType prerequisite =
            ProductionManager::Instance().
                getMissingCompletedTerranPrerequisite(desiredAct);

        if (prerequisite == BWAPI::UnitTypes::None ||
            prerequisite == BWAPI::UnitTypes::Unknown)
        {
            return false;
        }

        const bool alreadyStartedOrReserved =
            the.my.all.count(prerequisite) > 0 ||
            BuildingManager::Instance().isBeingBuilt(prerequisite) ||
            BuildingManager::Instance().getNumUnstarted(prerequisite) > 0 ||
            ProductionManager::Instance().getPendingGoalCount(prerequisite) > 0;

        if (alreadyStartedOrReserved)
        {
            return true;
        }

        if (!queue.isEmpty())
        {
            const MacroAct& top = queue.getHighestPriorityItem().macroAct;
            if (top.isUnit() && top.getUnitType() == prerequisite)
            {
                return true;
            }
        }

        const bool promoted =
            promoteQueuedUnitToTop(queue, prerequisite);

        if (!promoted)
        {
            queue.queueAsHighestPriority(MacroAct(prerequisite));
        }

        ProductionManager::Instance().invalidateTerranRulePlan(
            std::string("urgent prerequisite: ") +
            prerequisite.getName());

        DebugQueuePrintf(
            "queue: %s before %s (%s)",
            prerequisite.getName().c_str(),
            desiredAct.getName().c_str(),
            reason ? reason : "urgent reaction");

        return true;
    }

    // Return true while an urgent building requirement is still pending. The
    // caller should return for this frame so a lower-priority emergency cannot
    // jump in front of the prerequisite.
    static bool requireUrgentBuildingNow(
        BuildOrderQueue& queue,
        BWAPI::UnitType type,
        int targetCount = 1,
        MacroLocation location = MacroLocation::Main)
    {
        const int committed =
            the.my.all.count(type) +
            (int)BuildingManager::Instance().getNumUnstarted(type);

        if (committed >= targetCount)
        {
            return false;
        }

        // Add-ons leave the queue before they appear in the unit snapshot.
        // Pending goals already own the request and retry its footprint;
        // do not recreate the same urgent order on every frame.
        if (type.isAddon() && committed +
            int(ProductionManager::Instance().getPendingGoalCount(type)) >= targetCount)
        {
            return true;
        }

        if (waitForUrgentTerranPrerequisite(
                queue,
                MacroAct(type, location),
                "urgent building"))
        {
            return true;
        }

        const int missing = targetCount - committed;
        if (immediateQueuedUnitCount(queue, type, 2) >= missing)
        {
            return true;
        }

        if (promoteQueuedUnitToTop(queue, type))
        {
            return true;
        }

        queue.queueAsHighestPriority(MacroAct(type, location));
        return true;
    }

    static bool requireUrgentTechNow(
        BuildOrderQueue& queue,
        BWAPI::TechType tech)
    {
        if (BWAPI::Broodwar->self()->hasResearched(tech) ||
            BWAPI::Broodwar->self()->isResearching(tech))
        {
            return false;
        }

        if (waitForUrgentTerranPrerequisite(
                queue,
                MacroAct(tech),
                "urgent research"))
        {
            return true;
        }

        if (queue.anyInNextN(tech, 2))
        {
            return true;
        }

        if (promoteQueuedTechToTop(queue, tech))
        {
            return true;
        }

        queue.queueAsHighestPriority(tech);
        return true;
    }

    static bool requireUrgentCombatUnitNow(
        BuildOrderQueue& queue,
        BWAPI::UnitType unitType,
        int targetCount,
        int maxImmediateQueued = 2)
    {
        if (unitType == BWAPI::UnitTypes::Terran_Marine &&
            !ProductionManager::Instance().isOutOfBook())
        {
            ProductionManager::Instance().requestOpeningDefensiveMarines(targetCount);
            return false;
        }

        // PlayerSnapshot::all includes training. A deployed Tank is the
        // same artillery unit, not a deficit to replace with another Tank.
        const int currentOrTraining = the.my.all.count(unitType) +
            (unitType == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode
                ? the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
                : 0);

        if (currentOrTraining >= targetCount)
        {
            return false;
        }

        if (waitForUrgentTerranPrerequisite(
                queue,
                MacroAct(unitType),
                "urgent combat unit"))
        {
            return true;
        }

        const int queuedNow = immediateQueuedUnitCount(queue, unitType, 2);
        if (currentOrTraining + queuedNow >= targetCount ||
            queuedNow >= maxImmediateQueued)
        {
            return true;
        }

        if (promoteQueuedUnitToTop(queue, unitType))
        {
            return true;
        }

        queue.queueAsHighestPriority(unitType);
        return true;
    }

    static bool benzeneClearanceNeeded()
    {
        const std::string& mapName = BWAPI::Broodwar->mapFileName();
        if (mapName.find("Benzene") == std::string::npos &&
            mapName.find("benzene") == std::string::npos)
        {
            return false;
        }

        const BWAPI::Position startPosition(the.self()->getStartLocation());

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
            if (type != BWAPI::UnitTypes::Special_Power_Generator)
            {
                continue;
            }

            if (startPosition.getApproxDistance(neutral->getInitialPosition()) <= 48 * 32)
            {
                return true;
            }
        }

        return false;
    }

    static int benzeneClearanceMarineTarget(
        const BuildOrderQueue& queue,
        bool resetTarget = false)
    {
        static int target = -1;
        static int lastFrame = -1;

        const int now = BWAPI::Broodwar->getFrameCount();
        if (lastFrame > now)
        {
            target = -1;
        }
        lastFrame = now;

        if (resetTarget)
        {
            target = -1;
            return target;
        }

        if (target < 0)
        {
            target =
                the.my.all.count(BWAPI::UnitTypes::Terran_Marine) +
                (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Marine) +
                4;
        }

        return target;
    }

    static bool queueBenzeneClearanceMarine(BuildOrderQueue& queue)
    {
        // Benzene clearance is a post-opening map task. Do not let its four
        // Marines interleave with the deterministic JSON defense.
        if (!ProductionManager::Instance().isOutOfBook())
        {
            return false;
        }

        if (!benzeneClearanceNeeded())
        {
            benzeneClearanceMarineTarget(queue, true);
            return false;
        }

        const bool barracksExistsOrPlanned =
            the.my.all.count(BWAPI::UnitTypes::Terran_Barracks) > 0 ||
            BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Barracks) > 0 ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Barracks);

        if (!barracksExistsOrPlanned)
        {
            if (the.self()->minerals() < BWAPI::UnitTypes::Terran_Barracks.mineralPrice())
            {
                return false;
            }

            queue.queueAsHighestPriority(
                MacroAct(BWAPI::UnitTypes::Terran_Barracks, MacroLocation::Main));
            return true;
        }

        const int target = benzeneClearanceMarineTarget(queue);
        const int planned =
            the.my.all.count(BWAPI::UnitTypes::Terran_Marine) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Marine);

        if (planned >= target ||
            queue.numInQueue(BWAPI::UnitTypes::Terran_Marine) >= 2 ||
            the.self()->minerals() < 50)
        {
            return false;
        }

        if (waitForUrgentTerranPrerequisite(
                queue,
                MacroAct(BWAPI::UnitTypes::Terran_Marine),
                "Benzene clearance Marine"))
        {
            return true;
        }

        queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_Marine);
        return true;
    }

    // A hard promise of 200 supply at 15:00 is impossible after combat losses,
    // but macro must at least create enough production to make that pace
    // possible. Scale only after the opening, only when we have an economy,
    // and add at most one production building per planning cycle.
    static int countIdleTerranProducers(BWAPI::UnitType producerType)
    {
        int idle = 0;

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit ||
                !unit->exists() ||
                !unit->isCompleted() ||
                unit->getType() != producerType ||
                unit->isLifted())
            {
                continue;
            }

            if (!unit->isTraining() &&
                !unit->isResearching() &&
                !unit->isUpgrading() &&
                unit->getRemainingTrainTime() <= 0)
            {
                ++idle;
            }
        }

        return idle;
    }

    static bool terranProductionScalingPressure(
        BWAPI::UnitType producerType,
        int currentCount)
    {
        if (!ProductionManager::Instance().isOutOfBook() ||
            the.self()->getRace() != BWAPI::Races::Terran ||
            the.my.all.count(BWAPI::UnitTypes::Terran_SCV) < 20 ||
            the.self()->supplyUsed() >= 396)
        {
            return false;
        }

        const int minute = BWAPI::Broodwar->getFrameCount() / (24 * 60);

        if (minute < 7)
        {
            return false;
        }

        const int completed =
            the.my.completed.count(producerType);

        // If a new producer is already on the way, wait to measure its effect
        // before adding another one. This prevents bank spikes from creating a
        // staircase of several redundant Factories/Barracks.
        if (currentCount > completed)
        {
            return false;
        }

        const int idle =
            countIdleTerranProducers(producerType);

        const bool allCompletedProducersBusy =
            completed > 0 && idle == 0;

        // Visible supply target: 100 at 8:00, then about +14 per minute up to
        // 198 at 15:00. BWAPI supply values are doubled.
        const int targetSupplyUsed =
            minute < 8
            ? 0
            : std::min(396, 200 + (minute - 8) * 28);

        const bool behindFifteenMinutePace =
            targetSupplyUsed > 0 &&
            the.self()->supplyUsed() < targetSupplyUsed;

        // A large bank alone is not a reason to add another Factory: it can
        // mean the existing Factories are waiting for gas. Other producer
        // types retain their bank-driven scaling behavior.
        if (producerType != BWAPI::UnitTypes::Terran_Factory &&
            the.self()->minerals() >= 1800)
        {
            return true;
        }

        if (!allCompletedProducersBusy)
        {
            return false;
        }

        if (producerType == BWAPI::UnitTypes::Terran_Barracks)
        {
            return
                the.self()->minerals() >= 650 ||
                (behindFifteenMinutePace && the.self()->minerals() >= 450);
        }

        if (producerType == BWAPI::UnitTypes::Terran_Factory)
        {
            // Do not answer a gas shortage by building even more idle
            // Factories. A large mineral bank still permits Vulture capacity.
            const bool resourcesSupportAnotherFactory =
                the.self()->gas() >= 150 ||
                the.self()->minerals() >= 1200;

            return resourcesSupportAnotherFactory &&
                (the.self()->minerals() >= 850 ||
                 (behindFifteenMinutePace && the.self()->minerals() >= 650));
        }

        return
            the.self()->minerals() >= 1200 ||
            (behindFifteenMinutePace && the.self()->minerals() >= 700);
    }

    // Generic bank/pace pressure used only to feed already-idle producers.
    // Keep this separate from terranProductionScalingPressure(type,count):
    // filling an existing idle Factory/Barracks is not the same decision as
    // constructing another production building.
    static bool terranBankedArmyFillPressure()
    {
        if (!ProductionManager::Instance().isOutOfBook() ||
            the.self()->getRace() != BWAPI::Races::Terran ||
            the.my.all.count(BWAPI::UnitTypes::Terran_SCV) < 20 ||
            the.self()->supplyUsed() >= 396)
        {
            return false;
        }

        const int minute = BWAPI::Broodwar->getFrameCount() / (24 * 60);
        if (minute < 7)
        {
            return false;
        }

        const int targetSupplyUsed =
            minute < 8
            ? 0
            : std::min(396, 200 + (minute - 8) * 28);

        const bool behindFifteenMinutePace =
            targetSupplyUsed > 0 &&
            the.self()->supplyUsed() < targetSupplyUsed;

        return
            the.self()->minerals() >= 1200 ||
            (behindFifteenMinutePace && the.self()->minerals() >= 700);
    }

    static int scaleTerranProductionTargetOneStep(
        BWAPI::UnitType producerType,
        int currentCount,
        int normalTarget,
        int hardCap)
    {
        int target = std::min(normalTarget, hardCap);

        if (terranProductionScalingPressure(producerType, currentCount) &&
            currentCount < hardCap)
        {
            target = std::max(target, currentCount + 1);
        }

        return std::min(target, hardCap);
    }

    static bool queuePureAirExpansion(
        BuildOrderQueue& queue,
        bool expansionNeeded)
    {
        if (!StrategyManager::Instance().pureAirPlanActive() ||
            !expansionNeeded ||
            the.self()->minerals() < 650 ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Command_Center) ||
            BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Command_Center))
        {
            return false;
        }

        queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_Command_Center);
        return true;
    }

    static bool queuePureAirRefinery(BuildOrderQueue& queue)
    {
        if (!StrategyManager::Instance().pureAirPlanActive() ||
            the.self()->minerals() < 250 ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Refinery) ||
            BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Refinery))
        {
            return false;
        }

        const int currentRefineries =
            the.my.all.count(BWAPI::UnitTypes::Terran_Refinery);

        if (currentRefineries >= desiredRefineryCountForOwnedTerranBases())
        {
            return false;
        }

        queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_Refinery);
        return true;
    }

    static bool pureAirVehicleUpgradesNeedWork()
    {
        BWAPI::Player self = the.self();

        const BWAPI::UpgradeType weapons =
            BWAPI::UpgradeTypes::Terran_Vehicle_Weapons;
        const BWAPI::UpgradeType plating =
            BWAPI::UpgradeTypes::Terran_Vehicle_Plating;

        return
            self->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) < 1 ||
            self->getUpgradeLevel(weapons) < self->getMaxUpgradeLevel(weapons) ||
            self->getUpgradeLevel(plating) < self->getMaxUpgradeLevel(plating);
    }

    static bool queuePureAirUpgradeInfrastructure(BuildOrderQueue& queue)
    {
        if (!StrategyManager::Instance().pureAirPlanActive() ||
            !pureAirVehicleUpgradesNeedWork())
        {
            return false;
        }

        if (the.my.completed.count(BWAPI::UnitTypes::Terran_Factory) <= 0)
        {
            return false;
        }

        // The first Armory unlocks the unit which keeps us alive, so it must
        // precede Machine Shop/Charon and the second upgrade Armory. The old
        // order built research infrastructure while every Factory sat unable
        // to produce a single Goliath.
        const int armoriesCommitted =
            the.my.all.count(BWAPI::UnitTypes::Terran_Armory) +
            (int)BuildingManager::Instance().getNumUnstarted(
                BWAPI::UnitTypes::Terran_Armory);
        const int completedArmories =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Armory);

        if (armoriesCommitted < 1)
        {
            if (immediateQueuedUnitCount(
                    queue, BWAPI::UnitTypes::Terran_Armory, 2) < 1)
            {
                if (!promoteQueuedUnitToTop(
                        queue, BWAPI::UnitTypes::Terran_Armory))
                {
                    queue.queueAsHighestPriority(
                        MacroAct(
                            BWAPI::UnitTypes::Terran_Armory,
                            MacroLocation::Main));
                }
            }
            return true;
        }

        // A prerequisite hidden deep in the former JSON must not suppress the
        // PureAir transition. Promote it before adding a duplicate.
        if (the.my.all.count(BWAPI::UnitTypes::Terran_Machine_Shop) <= 0 &&
            BuildingManager::Instance().getNumUnstarted(
                BWAPI::UnitTypes::Terran_Machine_Shop) <= 0)
        {
            if (immediateQueuedUnitCount(
                    queue, BWAPI::UnitTypes::Terran_Machine_Shop, 2) <= 0)
            {
                if (!promoteQueuedUnitToTop(
                        queue, BWAPI::UnitTypes::Terran_Machine_Shop))
                {
                    queue.queueAsHighestPriority(
                        BWAPI::UnitTypes::Terran_Machine_Shop);
                }
            }
            return true;
        }

        // Two Armories allow weapon/plating to progress in parallel, but the
        // second one is requested only after the first has completed and
        // Goliath production is already available.
        if (completedArmories > 0 && armoriesCommitted < 2)
        {
            if (immediateQueuedUnitCount(
                    queue, BWAPI::UnitTypes::Terran_Armory, 2) < 1)
            {
                if (!promoteQueuedUnitToTop(
                        queue, BWAPI::UnitTypes::Terran_Armory))
                {
                    queue.queueAsHighestPriority(
                        MacroAct(
                            BWAPI::UnitTypes::Terran_Armory,
                            MacroLocation::Main));
                }
            }
            return true;
        }

        // Vehicle levels 2/3 require Science Facility. Promote Starport and
        // Science Facility instead of accepting a deep JSON copy as ready.
        const bool scienceFacilityCommitted =
            buildingExistsOrIsCommitted(
                BWAPI::UnitTypes::Terran_Science_Facility, queue, 2);

        if (scienceFacilityCommitted)
        {
            return false;
        }

        if (requireUrgentBuildingNow(
                queue, BWAPI::UnitTypes::Terran_Starport, 1))
        {
            return true;
        }

        if (the.my.completed.count(BWAPI::UnitTypes::Terran_Starport) <= 0)
        {
            // A Starport already under construction should not keep completed
            // Armories idle. Their research can run while it finishes.
            return false;
        }

        if (!promoteQueuedUnitToTop(
                queue, BWAPI::UnitTypes::Terran_Science_Facility))
        {
            queue.queueAsHighestPriority(
                MacroAct(
                    BWAPI::UnitTypes::Terran_Science_Facility,
                    MacroLocation::Main));
        }
        return true;
    }

    static bool queuePureAirVehicleUpgrade(BuildOrderQueue& queue)
    {
        if (!StrategyManager::Instance().pureAirPlanActive())
        {
            return false;
        }

        BWAPI::Player self = the.self();

        // Range is the first upgrade in a Carrier response. It uses a Machine
        // Shop, so it can start in parallel with both Armories.
        if (self->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) == 0 &&
            !self->isUpgrading(BWAPI::UpgradeTypes::Charon_Boosters) &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Machine_Shop) > 0)
        {
            if (!queue.anyInNextN(BWAPI::UpgradeTypes::Charon_Boosters, 2))
            {
                if (!promoteQueuedUpgradeToTop(
                        queue, BWAPI::UpgradeTypes::Charon_Boosters))
                {
                    queue.queueAsHighestPriority(
                        BWAPI::UpgradeTypes::Charon_Boosters);
                }
                return true;
            }
            // Charon uses the Machine Shop. Once it is already at the front
            // of the queue, an idle Armory may request its own upgrade too.
        }

        int idleArmories = 0;
        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (unit &&
                unit->exists() &&
                unit->isCompleted() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Armory &&
                !unit->isUpgrading())
            {
                ++idleArmories;
            }
        }

        if (idleArmories <= 0)
        {
            return false;
        }

        const BWAPI::UpgradeType weapons =
            BWAPI::UpgradeTypes::Terran_Vehicle_Weapons;
        const BWAPI::UpgradeType plating =
            BWAPI::UpgradeTypes::Terran_Vehicle_Plating;

        int queuedArmorySlots = 0;
        if (queue.anyInQueue(weapons))
        {
            ++queuedArmorySlots;
        }
        if (queue.anyInQueue(plating))
        {
            ++queuedArmorySlots;
        }
        // Never put a new upgrade in front of a queue that already consumes
        // every currently idle Armory. Charon is deliberately not counted: it
        // runs at the Machine Shop and does not occupy an Armory slot.
        if (queuedArmorySlots >= idleArmories)
        {
            return false;
        }

        const bool advancedLevelsAllowed =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0;

        BWAPI::UpgradeType nextUpgrade = BWAPI::UpgradeTypes::None;
        int nextLevel = 0;

        const int weaponLevel = self->getUpgradeLevel(weapons);
        const int platingLevel = self->getUpgradeLevel(plating);

        if (weaponLevel < 1 &&
            !self->isUpgrading(weapons) &&
            !queue.anyInQueue(weapons))
        {
            nextUpgrade = weapons;
            nextLevel = 1;
        }
        else if (platingLevel < 1 &&
            !self->isUpgrading(plating) &&
            !queue.anyInQueue(plating))
        {
            nextUpgrade = plating;
            nextLevel = 1;
        }
        else if (advancedLevelsAllowed &&
            weaponLevel < 3 &&
            !self->isUpgrading(weapons) &&
            !queue.anyInQueue(weapons))
        {
            nextUpgrade = weapons;
            nextLevel = weaponLevel + 1;
        }
        else if (advancedLevelsAllowed &&
            platingLevel < 3 &&
            !self->isUpgrading(plating) &&
            !queue.anyInQueue(plating))
        {
            nextUpgrade = plating;
            nextLevel = platingLevel + 1;
        }

        if (nextUpgrade == BWAPI::UpgradeTypes::None)
        {
            return false;
        }

        const int mineralReserve = 200;
        if (self->minerals() < nextUpgrade.mineralPrice(nextLevel) + mineralReserve ||
            self->gas() < nextUpgrade.gasPrice(nextLevel))
        {
            return false;
        }

        queue.queueAsHighestPriority(nextUpgrade);
        return true;
    }

    // Tank openings used to leave Armory upgrades behind repeated unit goals.
    // Start one affordable level on each free Armory, while leaving enough
    // minerals and gas for another combat unit. Emergency defenses run first.
    static bool queueTimelyTankVehicleUpgrade(BuildOrderQueue& queue)
    {
        if (StrategyManager::Instance().getOpeningGroup() != "tanks" ||
            StrategyManager::Instance().pureAirPlanActive() ||
            completedTankCount() < 3 ||
            the.my.completed.count(BWAPI::UnitTypes::Terran_Armory) <= 0)
        {
            return false;
        }

        BWAPI::Player self = the.self();
        int idleArmories = 0;
        for (BWAPI::Unit unit : self->getUnits())
        {
            if (unit && unit->exists() && unit->isCompleted() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Armory &&
                !unit->isUpgrading())
            {
                ++idleArmories;
            }
        }
        if (idleArmories == 0)
        {
            return false;
        }

        const BWAPI::UpgradeType upgrades[] = {
            BWAPI::UpgradeTypes::Terran_Vehicle_Weapons,
            BWAPI::UpgradeTypes::Terran_Vehicle_Plating
        };
        int queuedSlots = 0;
        for (BWAPI::UpgradeType upgrade : upgrades)
        {
            if (queue.anyInQueue(upgrade))
            {
                ++queuedSlots;
            }
        }

        const bool advancedLevelsAllowed =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0;
        for (BWAPI::UpgradeType upgrade : upgrades)
        {
            const int nextLevel = self->getUpgradeLevel(upgrade) + 1;
            if (nextLevel > self->getMaxUpgradeLevel(upgrade) ||
                (nextLevel >= 2 && !advancedLevelsAllowed) ||
                self->isUpgrading(upgrade) ||
                self->minerals() < upgrade.mineralPrice(nextLevel) + 150 ||
                self->gas() < upgrade.gasPrice(nextLevel) + 100)
            {
                continue;
            }

            if (queue.anyInQueue(upgrade))
            {
                return promoteQueuedUpgradeToTop(queue, upgrade);
            }
            if (queuedSlots < idleArmories)
            {
                queue.queueAsHighestPriority(upgrade);
                return true;
            }
        }
        return false;
    }

    static int removePureAirConflictsFromQueue(BuildOrderQueue& queue)
    {
        int removed = 0;

        const BWAPI::UnitType conflictingUnits[] =
        {
            BWAPI::UnitTypes::Terran_Marine,
            BWAPI::UnitTypes::Terran_Medic,
            BWAPI::UnitTypes::Terran_Firebat,
            BWAPI::UnitTypes::Terran_Valkyrie,
            BWAPI::UnitTypes::Terran_Dropship,
            BWAPI::UnitTypes::Terran_Battlecruiser
        };

        for (BWAPI::UnitType type : conflictingUnits)
        {
            while (removeOneQueuedUnitType(queue, type))
            {
                ++removed;
            }
        }

        // PureAir uses Tanks/Vultures which already exist (including a unit
        // whose Factory training has already begun), but it must not start a
        // new ground-insurance shell from old JSON/rule queue entries.
        while (removeOneQueuedUnitType(
            queue, BWAPI::UnitTypes::Terran_Vulture))
        {
            ++removed;
        }

        while (removeOneQueuedUnitType(
            queue, BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode))
        {
            ++removed;
        }

        // Existing Tanks must still be able to deploy; Siege Mode is therefore
        // retained even though new Tank queue entries are removed.
        while (removeOneQueuedTechType(queue, BWAPI::TechTypes::Spider_Mines))
        {
            ++removed;
        }
        while (removeOneQueuedUpgradeType(queue, BWAPI::UpgradeTypes::Ion_Thrusters))
        {
            ++removed;
        }

        return removed;
    }

    static int cancelPureAirConflictingTraining()
    {
        int cancelled = 0;

        for (BWAPI::Unit producer : BWAPI::Broodwar->self()->getUnits())
        {
            if (!producer || !producer->exists() || !producer->isTraining())
            {
                continue;
            }

            // Do not call Unit::getTrainingQueue() here. The crash logger proved
            // that BWAPI can fault inside that method. getLastCommand().getUnitType()
            // gives us the trained type when the latest command is a Train command;
            // if it cannot identify the job, leave it alone rather than cancelling
            // an unknown paid unit.
            const BWAPI::UnitType type =
                producer->getLastCommand().getUnitType();

            // A paid Tank/Vulture which is already in a Factory is allowed to
            // finish and becomes part of the synchronized rear support group.
            // The queue cleanup above prevents another one from starting.
            const bool conflicts =
                type == BWAPI::UnitTypes::Terran_Firebat ||
                type == BWAPI::UnitTypes::Terran_Valkyrie ||
                type == BWAPI::UnitTypes::Terran_Dropship ||
                type == BWAPI::UnitTypes::Terran_Battlecruiser ||
                type == BWAPI::UnitTypes::Terran_Medic ||
                type == BWAPI::UnitTypes::Terran_Marine;

            if (conflicts && producer->cancelTrain())
            {
                ++cancelled;
            }
        }

        return cancelled;
    }

    static bool removeExcessTankOpeningBunkersAnywhere(BuildOrderQueue& queue)
    {
        if (StrategyManager::Instance().getOpeningGroup() != "tanks")
        {
            return false;
        }

        const int maxBunkersForTankOpening = naturalExpansionStarted() ? 3 : 2;

        const int existingOrBeingBuilt =
            the.my.all.count(BWAPI::UnitTypes::Terran_Bunker) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Bunker);

        const int allowedQueuedBunkers =
            std::max(0, maxBunkersForTankOpening - existingOrBeingBuilt);

        const int queuedBunkers =
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Bunker);

        if (queuedBunkers <= allowedQueuedBunkers)
        {
            return false;
        }

        if (removeOneQueuedUnitType(queue, BWAPI::UnitTypes::Terran_Bunker))
        {
            DebugQueuePrintf(
                "queue: removed extra Bunker anywhere, existing=%d queued=%d max=%d",
                existingOrBeingBuilt,
                queuedBunkers,
                maxBunkersForTankOpening
            );

            return true;
        }

        return false;
    }

    static bool removeExcessTankOpeningBarracksAnywhere(BuildOrderQueue& queue)
    {
        if (StrategyManager::Instance().getOpeningGroup() != "tanks")
        {
            return false;
        }

        const int maxBarracksForTankOpening =
            ProductionManager::Instance().terranBuildingQueueLimit(
                BWAPI::UnitTypes::Terran_Barracks, 1);

        const int existingOrBeingBuilt =
            the.my.all.count(BWAPI::UnitTypes::Terran_Barracks) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Barracks);

        const int allowedQueuedBarracks =
            std::max(0, maxBarracksForTankOpening - existingOrBeingBuilt);

        const int queuedBarracks =
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Barracks);

        if (queuedBarracks <= allowedQueuedBarracks)
        {
            return false;
        }

        if (removeOneQueuedUnitType(queue, BWAPI::UnitTypes::Terran_Barracks))
        {
            return true;
        }

        return false;
    }

    static int totalBunkerCountIncludingPlanned(const BuildOrderQueue& queue)
    {
        return
            the.my.all.count(BWAPI::UnitTypes::Terran_Bunker) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Bunker) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Bunker);
    }

    static int totalTurretCountIncludingPlanned(const BuildOrderQueue& queue)
    {
        return
            the.my.all.count(BWAPI::UnitTypes::Terran_Missile_Turret) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Missile_Turret) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Missile_Turret);
    }

    static int totalMarineCountIncludingPlanned(const BuildOrderQueue& queue)
    {
        return
            the.my.all.count(BWAPI::UnitTypes::Terran_Marine) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Marine);
    }

    bool hasCompletedLandedBarracks();

    static bool tankOpeningBunkerRequestLimitReached(const BuildOrderQueue& queue, int wantedBunkers)
    {
        if (StrategyManager::Instance().getOpeningGroup() != "tanks")
        {
            return false;
        }

        const int cappedWanted = std::min(std::max(wantedBunkers, 0), 3);
        return totalBunkerCountIncludingPlanned(queue) >= cappedWanted;
    }

    static bool queueTankOpeningBunkerRequest(
        BuildOrderQueue& queue,
        const MacroAct& bunkerAct,
        int wantedBunkers,
        const char* reason)
    {
        if (StrategyManager::Instance().getOpeningGroup() == "tanks" &&
            tankOpeningBunkerRequestLimitReached(queue, wantedBunkers))
        {
            return false;
        }

        if (waitForUrgentTerranPrerequisite(
                queue,
                bunkerAct,
                reason ? reason : "Bunker"))
        {
            return true;
        }

        queue.queueAsHighestPriority(bunkerAct);

        if (Config::Debug::DrawQueueFixInfo && reason)
        {
            DebugQueuePrintf(
                "queue: %s bunker request, planned=%d/2",
                reason,
                totalBunkerCountIncludingPlanned(queue)
            );
        }

        return true;
    }

    static bool hasCompletedLandedProductionBuilding(BWAPI::UnitType type)
    {
        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (unit &&
                unit->exists() &&
                unit->isCompleted() &&
                unit->getType() == type &&
                !unit->isLifted())
            {
                return true;
            }
        }

        return false;
    }

    static bool landingTileLeavesAddonSpace(
        BWAPI::UnitType buildingType,
        const BWAPI::TilePosition& tile)
    {
        if (!buildingType.canBuildAddon())
        {
            return true;
        }

        // Terran addons occupy the lower-right 2x2 tiles next to the parent.
        const int addonX = tile.x + buildingType.tileWidth();
        const int addonY = tile.y + buildingType.tileHeight() - 2;

        for (int x = addonX; x < addonX + 2; ++x)
        {
            for (int y = addonY; y < addonY + 2; ++y)
            {
                const BWAPI::TilePosition addonTile(x, y);

                if (!addonTile.isValid() ||
                    !BWAPI::Broodwar->isBuildable(addonTile, true) ||
                    the.placer.isReserved(addonTile))
                {
                    return false;
                }
            }
        }

        return true;
    }

    // A lifted Barracks which is needed for Marines/Ghosts must return as a
    // producer, but its nearest landable tile may be in the middle of the map,
    // on a tank route, or beside a mineral line. Search around the useful
    // interior/front shoulder of every owned base first. The verified wall
    // Barracks uses this only after its opening-gate job is over and it is the
    // sole producer needed for a queued Marine/Ghost job.
    static BWAPI::TilePosition findUsefulSupportBuildingLandingTile(
        BWAPI::Unit barracks)
    {
        if (!barracks || !barracks->exists() || !barracks->isCompleted() ||
            !barracks->isLifted() ||
            (barracks->getType() != BWAPI::UnitTypes::Terran_Barracks &&
             barracks->getType() != BWAPI::UnitTypes::Terran_Engineering_Bay))
        {
            return BWAPI::TilePositions::Invalid;
        }

        std::vector<std::pair<long long, BWAPI::TilePosition>> candidates;
        const BWAPI::UnitType type = barracks->getType();

        for (Base * base : the.bases.getAll())
        {
            if (!base || base->getOwner() != the.self() ||
                !base->getCenter().isValid())
            {
                continue;
            }

            const BWAPI::Position baseCenter = base->getCenter();
            BWAPI::Position anchor = baseCenter;
            const BWAPI::Position front = base->getFront();
            if (front.isValid())
            {
                double dx = double(front.x - baseCenter.x);
                double dy = double(front.y - baseCenter.y);
                const double length = std::sqrt(dx * dx + dy * dy);
                if (length > 1.0)
                {
                    const double forward = std::min(6.0 * 32.0, length * 0.55);
                    anchor = BWAPI::Position(
                        int(baseCenter.x + dx / length * forward),
                        int(baseCenter.y + dy / length * forward));
                }
            }

            const BWAPI::TilePosition anchorTile(anchor);
            const int searchRadius = 12;
            for (int dx = -searchRadius; dx <= searchRadius; ++dx)
            {
                for (int dy = -searchRadius; dy <= searchRadius; ++dy)
                {
                    const BWAPI::TilePosition tile(
                        anchorTile.x + dx, anchorTile.y + dy);
                    if (!tile.isValid() ||
                        (the.placer.hasTerranMainWall() &&
                         tile == the.placer.getTerranMainWallBarracksTile()) ||
                        tile.x >= BWAPI::Broodwar->mapWidth() ||
                        tile.y >= BWAPI::Broodwar->mapHeight() ||
                        !the.zone.sameArea(tile, base->getCenterTile()) ||
                        !barracks->canLand(tile, false, false))
                    {
                        continue;
                    }

                    const BWAPI::Position center(
                        tile.x * 32 + type.tileWidth() * 16,
                        tile.y * 32 + type.tileHeight() * 16);
                    if (the.groundHitsFixed.inRange(type, tile))
                    {
                        continue;
                    }

                    long long score =
                        4LL * center.getApproxDistance(anchor) +
                        center.getApproxDistance(barracks->getPosition()) / 4;

                    const BWAPI::Position mineralLine(
                        base->getMineralLineTile());
                    if (mineralLine.isValid())
                    {
                        const int mineralDistance =
                            center.getApproxDistance(mineralLine);
                        if (mineralDistance < 6 * 32)
                        {
                            score += 12000 +
                                20LL * (6 * 32 - mineralDistance);
                        }
                    }

                    const int baseDistance =
                        center.getApproxDistance(baseCenter);
                    if (baseDistance < 5 * 32)
                    {
                        score += 8000 +
                            12LL * (5 * 32 - baseDistance);
                    }

                    if (the.placer.hasTerranMainWall() &&
                        the.placer.isNearTerranMainWall(center, 11 * 32))
                    {
                        // Do not recreate the traffic jam which caused the
                        // Barracks to lift. Outside/gate-side positions are
                        // particularly harmful; a deep interior fallback is
                        // still allowed if the base is extremely crowded.
                        score += the.placer.isInsideTerranMainWall(center, -64)
                            ? 18000
                            : 60000;
                    }

                    const int room = the.tileRoom.at(tile);
                    if (room > 0 && room < 8)
                    {
                        score += 3000LL * (8 - room);
                    }

                    candidates.emplace_back(score, tile);
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const std::pair<long long, BWAPI::TilePosition>& a,
               const std::pair<long long, BWAPI::TilePosition>& b)
            {
                return a.first < b.first;
            });
        for (const auto& candidate : candidates)
        {
            const BWAPI::TilePosition tile = candidate.second;
            Building landing(type, tile);
            landing.macroLocation = MacroLocation::Main;
            if (the.placer.buildingOK(landing, tile) &&
                the.placer.preservesGroundExit(type, tile))
            {
                return tile;
            }
        }

        return BWAPI::TilePositions::Invalid;
    }

    static BWAPI::TilePosition findLiftedBuildingLandingTileNear(BWAPI::Unit building)
    {
        if (!building ||
            !building->exists() ||
            !building->isCompleted() ||
            !building->isLifted() ||
            !building->getPosition().isValid())
        {
            return BWAPI::TilePositions::Invalid;
        }

        const BWAPI::UnitType buildingType = building->getType();
        const BWAPI::TilePosition centerTile(building->getPosition());

        // Start close, then widen the search. The old Barracks-only search
        // stopped at 12 tiles; a crowded production area could therefore
        // leave a lifted producer in the air forever.
        const int searchRadii[] = { 8, 16, 28, 40 };

        int previousRadius = -1;
        for (const int searchRadius : searchRadii)
        {
            std::vector<std::pair<int, BWAPI::TilePosition>> candidates;

            for (int dx = -searchRadius; dx <= searchRadius; ++dx)
            {
                for (int dy = -searchRadius; dy <= searchRadius; ++dy)
                {
                    if (previousRadius >= 0 &&
                        std::max(std::abs(dx), std::abs(dy)) <= previousRadius)
                    {
                        continue;
                    }
                    const BWAPI::TilePosition tile(centerTile.x + dx, centerTile.y + dy);

                    if (!tile.isValid() ||
                        (the.placer.hasTerranMainWall() &&
                         tile == the.placer.getTerranMainWallBarracksTile()) ||
                        tile.x >= BWAPI::Broodwar->mapWidth() ||
                        tile.y >= BWAPI::Broodwar->mapHeight() ||
                        !building->canLand(tile, false, false))
                    {
                        continue;
                    }

                    const BWAPI::Position tileCenter(
                        tile.x * 32 + buildingType.tileWidth() * 16,
                        tile.y * 32 + buildingType.tileHeight() * 16
                    );

                    int score = tileCenter.getApproxDistance(building->getPosition());

                    // A Factory/Starport must preferably keep its future addon
                    // area free, otherwise landing succeeds but the next
                    // Machine Shop/Control Tower can stall again.
                    if (!landingTileLeavesAddonSpace(buildingType, tile))
                    {
                        score += 1600;
                    }

                    // Prefer safer, non-mineral-line spots around our bases.
                    for (Base* base : the.bases.getAll())
                    {
                        if (!base || base->getOwner() != the.self() || !base->getPosition().isValid())
                        {
                            continue;
                        }

                        const int distToBase = tileCenter.getApproxDistance(base->getPosition());
                        const int distToMinerals = tileCenter.getApproxDistance(BWAPI::Position(base->getMineralLineTile()));

                        if (distToBase < 4 * 32)
                        {
                            score += 1200;
                        }

                        if (distToMinerals < 5 * 32)
                        {
                            score += 2000;
                        }
                    }

                    candidates.emplace_back(score, tile);
                }
            }

            std::sort(candidates.begin(), candidates.end(),
                [](const std::pair<int, BWAPI::TilePosition>& a,
                   const std::pair<int, BWAPI::TilePosition>& b)
                {
                    return a.first < b.first;
                });
            for (const auto& candidate : candidates)
            {
                const BWAPI::TilePosition tile = candidate.second;
                Building landing(buildingType, tile);
                landing.macroLocation = MacroLocation::Main;
                if (the.placer.buildingOK(landing, tile) &&
                    the.placer.preservesGroundExit(buildingType, tile))
                {
                    return tile;
                }
            }
            previousRadius = searchRadius;
        }

        return BWAPI::TilePositions::Invalid;
    }

    static bool queuePureAirFactoryFillGoliaths(BuildOrderQueue& queue)
    {
        if (!StrategyManager::Instance().pureAirPlanActive())
        {
            return false;
        }

        if (the.my.completed.count(BWAPI::UnitTypes::Terran_Armory) <= 0)
        {
            return false;
        }

        int idleFactories = 0;

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit ||
                !unit->exists() ||
                !unit->isCompleted() ||
                unit->getType() != BWAPI::UnitTypes::Terran_Factory)
            {
                continue;
            }

            if (!unit->isTraining())
            {
                ++idleFactories;
            }
        }

        if (idleFactories <= 0)
        {
            return false;
        }

        const int queuedGoliaths =
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Goliath);

        // Keep the queue small. We do not want 6-10 Goliaths blocking tech/buildings.
        if (queuedGoliaths >= 2)
        {
            return false;
        }

        const int supplyFree =
            BWAPI::Broodwar->self()->supplyTotal() -
            BWAPI::Broodwar->self()->supplyUsed();

        if (supplyFree < BWAPI::UnitTypes::Terran_Goliath.supplyRequired())
        {
            return false;
        }

        const int goliathsToQueue =
            std::min(2 - queuedGoliaths, idleFactories);

        if (goliathsToQueue <= 0)
        {
            return false;
        }

        for (int i = 0; i < goliathsToQueue; ++i)
        {
            queue.queueAsLowestPriority(BWAPI::UnitTypes::Terran_Goliath);
        }

        if (Config::Debug::DrawQueueFixInfo)
        {
            DebugQueuePrintf(
                "queue: PureAir factory fill low-priority Goliath x%d",
                goliathsToQueue
            );
        }

        return true;
    }

    static int idleCompletedLandedTerranProductionBuildings(BWAPI::UnitType type)
    {
        int count = 0;

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (unit &&
                unit->exists() &&
                unit->isCompleted() &&
                unit->getType() == type &&
                !unit->isLifted() &&
                !unit->isTraining())
            {
                ++count;
            }
        }

        return count;
    }

    static int bioTankLurkerTankTarget();

    // Army percentages are measured in supply. Keep the normal 80/20 core,
    // but allow 25-30% Tanks when both resources bank up behind that ceiling.
    static int bioTankTankCeiling()
    {
        if (!StrategyManager::Instance().bioTankUsesTanks()) return 0;
        const int infantrySupply =
            the.my.all.count(BWAPI::UnitTypes::Terran_Marine) *
                BWAPI::UnitTypes::Terran_Marine.supplyRequired() +
            the.my.all.count(BWAPI::UnitTypes::Terran_Medic) *
                BWAPI::UnitTypes::Terran_Medic.supplyRequired() +
            the.my.all.count(BWAPI::UnitTypes::Terran_Firebat) *
                BWAPI::UnitTypes::Terran_Firebat.supplyRequired();
        const int tankSupply =
            BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode.supplyRequired();
        const int percent = StrategyManager::Instance().bioTankDesiredTankPercent();
        const int openingTankFloor =
            StrategyManager::Instance().deepSixPlanActive() ? 4 :
            tvzBioPolicy(StrategyManager::Instance().getTerranPlan())
                ? tvzBioPolicy(StrategyManager::Instance().getTerranPlan())->tankFloor :
                  StrategyManager::Instance().getTerranPlan() == TerranPlanId::BioTank ? 2 : 0;
        return std::max({
            BioProductionPolicy::tankCeiling(infantrySupply, tankSupply, percent),
            bioTankLurkerTankTarget(), openingTankFloor });
    }

    // BOSS plans in waves. While a new search is finishing, completed
    // production buildings could remain idle even with a large bank. Feed only
    // currently idle producers, at low priority and in a small bounded batch,
    // so this cannot bury buildings, supply or upgrades.
    static bool queueBankedTerranArmyFill(BuildOrderQueue& queue)
    {
        if (!ProductionManager::Instance().isOutOfBook() ||
            the.self()->getRace() != BWAPI::Races::Terran ||
            StrategyManager::Instance().pureAirPlanActive() ||
            !terranBankedArmyFillPressure() ||
            queue.size() >= 12)
        {
            return false;
        }

        int freeSupply =
            the.self()->supplyTotal() - the.self()->supplyUsed();

        if (freeSupply < BWAPI::UnitTypes::Terran_Marine.supplyRequired())
        {
            return false;
        }

        const std::string openingGroup =
            StrategyManager::Instance().getOpeningGroup();
        if (BioTankPolicy::reserveGoliathSupply() ||
            StrategyManager::Instance().airTanksPlanActive()) return false;

        const bool nearbyAirOnlyAttack =
            visibleEnemyAirOnlyNearOwnedBase();

        if (openingGroup == "bio")
        {
            const int idleBarracks =
                idleCompletedLandedTerranProductionBuildings(
                    BWAPI::UnitTypes::Terran_Barracks);

            const int queuedBarracksUnits =
                (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Marine) +
                (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Medic) +
                (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Firebat) +
                (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Ghost);

            const int desiredQueuedBarracksUnits =
                std::min(4, idleBarracks);

            int barracksUnitsToQueue =
                std::max(0, desiredQueuedBarracksUnits - queuedBarracksUnits);

            barracksUnitsToQueue = std::min(
                barracksUnitsToQueue,
                std::max(0, (the.self()->minerals() - 400) /
                    BWAPI::UnitTypes::Terran_Marine.mineralPrice()));

            barracksUnitsToQueue = std::min(
                barracksUnitsToQueue,
                freeSupply / BWAPI::UnitTypes::Terran_Marine.supplyRequired());

            if (barracksUnitsToQueue > 0)
            {
                const int plannedMarines =
                    the.my.all.count(BWAPI::UnitTypes::Terran_Marine) +
                    (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Marine);
                const int plannedMedics =
                    the.my.all.count(BWAPI::UnitTypes::Terran_Medic) +
                    (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Medic);
                int missingMedics =
                    the.my.completed.count(BWAPI::UnitTypes::Terran_Academy) > 0
                    ? std::max(0, BioProductionPolicy::medicTarget(plannedMarines,
                        the.my.all.count(BWAPI::UnitTypes::Terran_Firebat) +
                            int(queue.numInQueue(BWAPI::UnitTypes::Terran_Firebat)),
                        the.my.all.count(BWAPI::UnitTypes::Terran_Ghost) +
                            int(queue.numInQueue(BWAPI::UnitTypes::Terran_Ghost))) - plannedMedics)
                    : 0;
                missingMedics = std::min(missingMedics,
                    the.self()->gas() /
                        BWAPI::UnitTypes::Terran_Medic.gasPrice());
                for (int i = 0; i < barracksUnitsToQueue; ++i)
                {
                    if (missingMedics > 0)
                    {
                        queue.queueAsLowestPriority(BWAPI::UnitTypes::Terran_Medic);
                        --missingMedics;
                    }
                    else
                    {
                        queue.queueAsLowestPriority(BWAPI::UnitTypes::Terran_Marine);
                    }
                }

                return true;
            }
        }

        // A real capital fleet gets the Factory gas before optional Tanks.
        if (openingGroup == "bio" &&
            StrategyManager::Instance().getTerranPlan() == TerranPlanId::BioTank &&
            the.my.all.count(BWAPI::UnitTypes::Terran_Goliath) < BioTankPolicy::goliathTarget())
            return false;

        const int idleFactories =
            idleCompletedLandedTerranProductionBuildings(
                BWAPI::UnitTypes::Terran_Factory);

        int queuedTanks =
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode);

        if (openingGroup == "bio" &&
            the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) +
            queuedTanks >= bioTankTankCeiling())
        {
            return false;
        }

        const int queuedFactoryUnits =
            queuedTanks +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Vulture) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Goliath);

        const int desiredQueuedFactoryUnits =
            std::min(3, idleFactories);

        int factoryJobsToQueue =
            std::max(0, desiredQueuedFactoryUnits - queuedFactoryUnits);

        if (factoryJobsToQueue <= 0)
        {
            return false;
        }

        const int completedMachineShops =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Machine_Shop);

        const bool canProduceAirDefense =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Armory) > 0;

        if (nearbyAirOnlyAttack &&
            (openingGroup == "bio" || !canProduceAirDefense))
        {
            return false;
        }

        int availableTankSlots =
            std::max(0, completedMachineShops - queuedTanks);

        int mineralsLeft = the.self()->minerals() - 400;
        int gasLeft = the.self()->gas();
        bool changed = false;

        while (factoryJobsToQueue > 0)
        {
            const BWAPI::UnitType tank =
                BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode;

            const BWAPI::UnitType vulture =
                BWAPI::UnitTypes::Terran_Vulture;

            const BWAPI::UnitType goliath =
                BWAPI::UnitTypes::Terran_Goliath;

            const bool tankSlotAvailable =
                availableTankSlots > 0 &&
                mineralsLeft >= tank.mineralPrice() &&
                gasLeft >= tank.gasPrice() &&
                freeSupply >= tank.supplyRequired();

            if (openingGroup == "bio" &&
                (!tankSlotAvailable ||
                 the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
                 the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) +
                 queuedTanks >= bioTankTankCeiling()))
            {
                break;
            }
            const BWAPI::UnitType nextUnit =
                openingGroup == "bio" ? tank :
                nearbyAirOnlyAttack ? goliath :
                tankSlotAvailable ? tank : vulture;

            if (mineralsLeft < nextUnit.mineralPrice() ||
                gasLeft < nextUnit.gasPrice() ||
                freeSupply < nextUnit.supplyRequired())
            {
                break;
            }

            queue.queueAsLowestPriority(nextUnit);
            mineralsLeft -= nextUnit.mineralPrice();
            gasLeft -= nextUnit.gasPrice();
            freeSupply -= nextUnit.supplyRequired();

            if (nextUnit == tank)
            {
                ++queuedTanks;
                --availableTankSlots;
            }

            --factoryJobsToQueue;
            changed = true;
        }

        return changed;
    }

    static bool requestExistingLiftedBuildingLand(BWAPI::UnitType buildingType)
    {
        static std::map<int, int> nextLandingSearchFrame;
        static int lastExpensiveLandingSearchFrame = -1;

        BWAPI::Unit bestBuilding = nullptr;
        BWAPI::TilePosition bestLandingTile = BWAPI::TilePositions::Invalid;
        int bestDistance = MAX_DISTANCE;
        const int now = BWAPI::Broodwar->getFrameCount();
        if (lastExpensiveLandingSearchFrame > now)
        {
            nextLandingSearchFrame.clear();
            lastExpensiveLandingSearchFrame = -1;
        }

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists())
            {
                continue;
            }

            if (unit->getType() != buildingType)
            {
                continue;
            }

            if (!unit->isCompleted() || !unit->isLifted())
            {
                continue;
            }

            // One authority owns the gate lifecycle. A merely reserved
            // expansion must not let generic producer recovery fly it away.
            if (buildingType == BWAPI::UnitTypes::Terran_Barracks &&
                the.placer.isTerranMainWallBarracks(unit) &&
                ProductionManager::Instance().terranMainWallGateActive())
            {
                continue;
            }

            // Do not replace a valid landing order every frame while the
            // building is flying to its chosen tile.
            if (unit->getOrder() == BWAPI::Orders::BuildingLand)
            {
                return true;
            }

            const auto retryIt = nextLandingSearchFrame.find(unit->getID());

            if (retryIt != nextLandingSearchFrame.end() &&
                retryIt->second > now &&
                retryIt->second - now < 24 * 60)
            {
                continue;
            }

            // Do not run several map-wide landing searches in the same game
            // frame. The other buildings are retried on following frames.
            if (lastExpensiveLandingSearchFrame == now)
            {
                continue;
            }
            lastExpensiveLandingSearchFrame = now;

            // A wide landing search is intentionally throttled. Crowded bases
            // can require thousands of candidate checks, but the map cannot
            // change enough to justify repeating them every frame.
            nextLandingSearchFrame[unit->getID()] = now + 12;

            BWAPI::TilePosition landingTile =
                (buildingType == BWAPI::UnitTypes::Terran_Barracks ||
                 buildingType == BWAPI::UnitTypes::Terran_Engineering_Bay)
                ? findUsefulSupportBuildingLandingTile(unit)
                : BWAPI::TilePositions::Invalid;

            if (!landingTile.isValid())
            {
                landingTile = findLiftedBuildingLandingTileNear(unit);
            }

            if (!landingTile.isValid())
            {
                nextLandingSearchFrame[unit->getID()] = now + 2 * 24;
                continue;
            }

            const BWAPI::Position landingPosition(landingTile);
            const int distance = landingPosition.getApproxDistance(unit->getPosition());

            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestBuilding = unit;
                bestLandingTile = landingTile;
            }
        }

        if (!bestBuilding || !bestLandingTile.isValid())
        {
            return false;
        }

        if (!bestBuilding->land(bestLandingTile))
        {
            return false;
        }

        if (Config::Debug::DrawQueueFixInfo)
        {
            DebugQueuePrintf(
                "queue: landing lifted %s at reserved-safe base position",
                buildingType.getName().c_str()
            );
        }

        // A landing command was issued, so urgent handled the producer problem.
        return true;
    }

    static bool requestExistingBarracksLandForMarines()
    {
        if (hasCompletedLandedBarracks())
        {
            return true;
        }

        return requestExistingLiftedBuildingLand(BWAPI::UnitTypes::Terran_Barracks);
    }

    static bool queueNeedsFactoryProduction(const BuildOrderQueue& queue)
    {
        return
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Vulture) ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Goliath) ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Machine_Shop);
    }

    static bool queueNeedsStarportProduction(const BuildOrderQueue& queue)
    {
        return
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Wraith) ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Dropship) ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Science_Vessel) ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Valkyrie) ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Battlecruiser) ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Control_Tower);
    }

    static bool requestNeededProductionBuildingLanding(const BuildOrderQueue& queue)
    {
        auto & production = ProductionManager::Instance();
        BWAPI::Unit spotter = production.getEngineeringBayTankSpotter();
        // Return the Bay for upgrades, a strategy change, or loss of the tanks.
        if (!spotter &&
            requestExistingLiftedBuildingLand(BWAPI::UnitTypes::Terran_Engineering_Bay))
            return true;
        if (spotter && spotter->isLifted() &&
            !hasCompletedLandedProductionBuilding(BWAPI::UnitTypes::Terran_Barracks) &&
            requestExistingLiftedBuildingLand(BWAPI::UnitTypes::Terran_Barracks))
            return true;

        struct ProducerNeed
        {
            BWAPI::UnitType type;
            bool queueNeedsIt;
        };

        // Factory first: unlike the Barracks, no current strategy intentionally
        // uses a lifted Factory as vision. If it is our only completed Factory,
        // put it back down even when its waiting item has already moved from the
        // queue into ProductionManager's goal list.
        const ProducerNeed needs[] =
        {
            { BWAPI::UnitTypes::Terran_Factory, queueNeedsFactoryProduction(queue) },
            { BWAPI::UnitTypes::Terran_Starport, queueNeedsStarportProduction(queue) }
        };

        for (const ProducerNeed& need : needs)
        {
            const bool noLandedProducer =
                !hasCompletedLandedProductionBuilding(need.type);

            const bool rescueWithoutVisibleQueue =
                (need.type == BWAPI::UnitTypes::Terran_Factory ||
                    need.type == BWAPI::UnitTypes::Terran_Starport) &&
                noLandedProducer;

            if (!need.queueNeedsIt && !rescueWithoutVisibleQueue)
            {
                continue;
            }

            if (requestExistingLiftedBuildingLand(need.type))
            {
                return true;
            }
        }

        return false;
    }

    BWAPI::Unit getMainCommandCenter()
    {
        Base* mainBase = the.bases.myMain();

        if (!mainBase || !mainBase->getPosition().isValid())
        {
            return nullptr;
        }

        BWAPI::Unit bestCC = nullptr;
        int bestDistance = MAX_DISTANCE;

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists())
            {
                continue;
            }

            if (unit->getType() != BWAPI::UnitTypes::Terran_Command_Center)
            {
                continue;
            }

            if (!unit->isCompleted() || unit->isLifted())
            {
                continue;
            }

            const int distance = unit->getDistance(mainBase->getPosition());

            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestCC = unit;
            }
        }

        return bestCC;
    }

    bool mainCommandCenterHasComsat()
    {
        BWAPI::Unit mainCC = getMainCommandCenter();

        if (!mainCC || !mainCC->exists())
        {
            return false;
        }

        BWAPI::Unit addon = mainCC->getAddon();

        return
            addon &&
            addon->exists() &&
            addon->getType() == BWAPI::UnitTypes::Terran_Comsat_Station;
    }

    bool mainCommandCenterCanTakeComsat()
    {
        BWAPI::Unit mainCC = getMainCommandCenter();

        if (!mainCC || !mainCC->exists())
        {
            return false;
        }

        if (!mainCC->isCompleted() || mainCC->isLifted())
        {
            return false;
        }

        if (mainCC->getAddon())
        {
            return false;
        }

        return true;
    }

    int completedComsatCount()
    {
        return the.my.completed.count(BWAPI::UnitTypes::Terran_Comsat_Station);
    }

    bool hasCompletedComsatStation()
    {
        return completedComsatCount() > 0;
    }

    Base* getClosestCompletedExpansionToMain()
    {
        Base* mainBase = the.bases.myMain();

        if (!mainBase || !mainBase->getPosition().isValid())
        {
            return nullptr;
        }

        Base* bestBase = nullptr;
        int bestDistance = MAX_DISTANCE;

        for (Base* base : the.bases.getAll())
        {
            if (!base || base == mainBase)
            {
                continue;
            }

            if (base->getOwner() != the.self())
            {
                continue;
            }

            if (!base->isCompleted())
            {
                continue;
            }

            if (!base->getPosition().isValid())
            {
                continue;
            }

            const int distance = base->getDistance(mainBase->getPosition());

            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestBase = base;
            }
        }

        return bestBase;
    }

    struct NearbyEnemyThreats
    {
        int groundCombatUnits = 0;
        int airCombatUnits = 0;

        bool hasGround() const { return groundCombatUnits > 0; }
        bool hasAir() const { return airCombatUnits > 0; }
        bool airOnly() const { return hasAir() && !hasGround(); }
    };

    const NearbyEnemyThreats& visibleEnemyThreatsNearOwnedBases()
    {
        static int cachedFrame = -1;
        static NearbyEnemyThreats cachedThreats;

        const int now = BWAPI::Broodwar->getFrameCount();

        if (cachedFrame == now)
        {
            return cachedThreats;
        }

        cachedFrame = now;
        cachedThreats = NearbyEnemyThreats();

        const int THREAT_RADIUS = 18 * 32;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible())
            {
                continue;
            }

            if (!enemy->getPosition().isValid())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (type.isWorker() || type.isSpell())
            {
                continue;
            }

            const bool flyingCombatUnit =
                enemy->isFlying() &&
                !type.isBuilding() &&
                HasEnemyThreat(
                    EnemyThreatForUnitType(type),
                    EnemyThreat::GroundAir);

            bool groundCombatUnit =
                !enemy->isFlying() &&
                !type.isBuilding() &&
                (UnitUtil::CanAttackGround(enemy) || UnitUtil::CanAttackAir(enemy));

            if (type.isBuilding())
            {
                groundCombatUnit =
                    type == BWAPI::UnitTypes::Terran_Bunker ||
                    type == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
                    type == BWAPI::UnitTypes::Zerg_Sunken_Colony ||
                    type == BWAPI::UnitTypes::Zerg_Spore_Colony;
            }

            if (!flyingCombatUnit && !groundCombatUnit)
            {
                continue;
            }

            bool nearOwnedBase = false;

            for (Base* base : the.bases.getAll())
            {
                if (!base ||
                    base->getOwner() != the.self() ||
                    !base->isCompleted() ||
                    !base->getDepot() ||
                    !base->getPosition().isValid())
                {
                    continue;
                }

                if (enemy->getDistance(base->getPosition()) <= THREAT_RADIUS)
                {
                    nearOwnedBase = true;
                    break;
                }
            }

            if (!nearOwnedBase)
            {
                continue;
            }

            if (flyingCombatUnit)
            {
                ++cachedThreats.airCombatUnits;
            }
            else
            {
                ++cachedThreats.groundCombatUnits;
            }
        }

        return cachedThreats;
    }

    bool visibleEnemyGroundNearMainOrClosestExpansion()
    {
        return visibleEnemyThreatsNearOwnedBases().hasGround();
    }

    bool visibleEnemyAirNearOwnedBase()
    {
        return visibleEnemyThreatsNearOwnedBases().hasAir();
    }

    bool visibleEnemyAirOnlyNearOwnedBase()
    {
        return visibleEnemyThreatsNearOwnedBases().airOnly();
    }

    // Estimate how much BWAPI supply our currently completed Terran
    // production can consume before a newly-started Supply Depot finishes.
    // This is deliberately based on continuous production: the macro core
    // should build supply for the throughput we want, not for the number of
    // free supply points which happen to exist on one frame.
    static int terranPredictedSupplyDemandBeforeDepotCompletes()
    {
        

        const int horizon =
            BWAPI::UnitTypes::Terran_Supply_Depot.buildTime() +
            std::max(24, 2 * BWAPI::Broodwar->getLatencyFrames() + 8);

        int predictedDemand = 0;

        for (BWAPI::Unit producer : BWAPI::Broodwar->self()->getUnits())
        {
            if (!producer ||
                !producer->exists() ||
                !producer->isCompleted() ||
                producer->isLifted())
            {
                continue;
            }

            BWAPI::UnitType representative = BWAPI::UnitTypes::None;
            const BWAPI::UnitType producerType = producer->getType();

            if (producerType == BWAPI::UnitTypes::Terran_Command_Center)
            {
                // Do not reserve future supply for workers once the economy has
                // reached its current saturation target. Existing SCVs already
                // in training have already consumed their supply.
                if (the.my.all.count(BWAPI::UnitTypes::Terran_SCV) <
                    WorkerManager::Instance().getMaxWorkers())
                {
                    representative = BWAPI::UnitTypes::Terran_SCV;
                }
            }
            else if (producerType == BWAPI::UnitTypes::Terran_Barracks)
            {
                representative = BWAPI::UnitTypes::Terran_Marine;
            }
            else if (producerType == BWAPI::UnitTypes::Terran_Factory)
            {
                // Vulture is the fastest normal Factory consumer of supply.
                // Planning for it is safer than assuming every Factory makes
                // the slower Tank.
                representative = BWAPI::UnitTypes::Terran_Vulture;
            }
            else if (producerType == BWAPI::UnitTypes::Terran_Starport)
            {
                representative = BWAPI::UnitTypes::Terran_Wraith;
            }

            if (representative == BWAPI::UnitTypes::None ||
                representative.buildTime() <= 0 ||
                representative.supplyRequired() <= 0)
            {
                continue;
            }

            int completions = 0;

            if (producer->isTraining() &&
                producer->getRemainingTrainTime() > 0)
            {
                const int firstCompletion =
                    producer->getRemainingTrainTime();

                if (firstCompletion <= horizon)
                {
                    completions = 1 +
                        std::max(0, horizon - firstCompletion) /
                        representative.buildTime();
                }
            }
            else if (!producer->isResearching() &&
                     !producer->isUpgrading())
            {
                // An idle producer can start one unit immediately, consuming
                // supply now, and then start another each time a unit finishes.
                completions =
                    1 + horizon / representative.buildTime();
            }

            predictedDemand +=
                completions * representative.supplyRequired();
        }

        return std::max(0, predictedDemand);
    }

    static int urgentSupplyDepotsNeeded(const BuildOrderQueue& queue)
    {
        

        if (BWAPI::Broodwar->self()->supplyTotal() >= 400)
        {
            return 0;
        }

        const int supplyUsed =
            BWAPI::Broodwar->self()->supplyUsed();

        const int supplyTotal =
            BWAPI::Broodwar->self()->supplyTotal();

        const int depotSupply =
            BWAPI::UnitTypes::Terran_Supply_Depot.supplyProvided();

        // BWAPI supply is doubled.
        // +8 visible supply = +16 BWAPI supply.
        const int RECOVERY_FREE_SUPPLY = 8 * 2;

        int depotsBeingBuilt = 0;

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists())
            {
                continue;
            }

            if (unit->getType() == BWAPI::UnitTypes::Terran_Supply_Depot &&
                !unit->isCompleted())
            {
                ++depotsBeingBuilt;
            }
        }

        const int depotsQueued =
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Supply_Depot);

        // Once ProductionManager hands a depot to BuildingManager it is no
        // longer in BuildOrderQueue, but it also has no BWAPI unit until an
        // SCV starts construction. The emergency branch previously counted
        // neither state and could enqueue two more depots every frame.
        const int depotsWaitingForConstruction =
            (int)BuildingManager::Instance().getNumUnstarted(
                BWAPI::UnitTypes::Terran_Supply_Depot
            );

        const int plannedDepotSupply =
            (depotsBeingBuilt + depotsWaitingForConstruction + depotsQueued) * depotSupply;

        // Emergency recovery after supply depots were destroyed.
        // Example: 108/100 supply. We want enough depots for 108/116.
        if (supplyUsed >= supplyTotal)
        {
            const int MAX_EMERGENCY_DEPOTS_PLANNED =
                ProductionManager::Instance().isOutOfBook() ? 2 : 1;

            const int depotsAlreadyPlanned =
                depotsBeingBuilt + depotsWaitingForConstruction + depotsQueued;

            if (depotsAlreadyPlanned >= MAX_EMERGENCY_DEPOTS_PLANNED)
            {
                return 0;
            }

            const int wantedTotalSupply =
                supplyUsed + RECOVERY_FREE_SUPPLY;

            const int expectedTotalSupply =
                supplyTotal + plannedDepotSupply;

            if (expectedTotalSupply >= wantedTotalSupply)
            {
                return 0;
            }

            const int missingSupply =
                wantedTotalSupply - expectedTotalSupply;

            const int depotsNeeded =
                (missingSupply + depotSupply - 1) / depotSupply;

            const int freeEmergencySlots =
                MAX_EMERGENCY_DEPOTS_PLANNED - depotsAlreadyPlanned;

            return std::min(depotsNeeded, freeEmergencySlots);
        }

        // During the first minute the JSON opening owns the supply timing.
        // The parallel safety rule used to promote future book depots as soon
        // as only four visible supply remained, placing as many as three
        // Depots ahead of the opening SCVs. A real supply block is still
        // handled by the emergency branch above.
        if (BWAPI::Broodwar->getFrameCount() < 24 * 60)
        {
            return 0;
        }

        const bool outOfBook =
            ProductionManager::Instance().isOutOfBook();

        // Keep the JSON opening conservative: the book owns its exact timings
        // and only one safety Depot may be promoted at a time.
        if (!outOfBook)
        {
            if (BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Supply_Depot) ||
                queue.anyInQueue(BWAPI::UnitTypes::Terran_Supply_Depot))
            {
                return 0;
            }

            const int supplyFree =
                supplyTotal - supplyUsed;

            return supplyFree <= 4 * 2 ? 1 : 0;
        }

        // Persistent macro plans supply from producer throughput. A fixed
        // "keep 16 free supply" rule is too large for two producers and too
        // small for eight Factories. Predict what our completed production can
        // consume before a Depot started now would finish, then keep a small
        // post-completion reserve.
        const int predictedDemand =
            terranPredictedSupplyDemandBeforeDepotCompletes();

        const int POST_DEPOT_RESERVE = 6 * 2;
        const int wantedTotalSupply =
            std::min(
                400,
                supplyUsed + predictedDemand + POST_DEPOT_RESERVE);

        const int expectedTotalSupply =
            std::min(400, supplyTotal + plannedDepotSupply);

        if (expectedTotalSupply >= wantedTotalSupply)
        {
            return 0;
        }

        const int depotsAlreadyPlanned =
            depotsBeingBuilt + depotsWaitingForConstruction + depotsQueued;

        const int MAX_PREDICTIVE_DEPOTS_PLANNED = 3;

        if (depotsAlreadyPlanned >= MAX_PREDICTIVE_DEPOTS_PLANNED)
        {
            return 0;
        }

        const int missingSupply =
            wantedTotalSupply - expectedTotalSupply;

        const int depotsNeeded =
            (missingSupply + depotSupply - 1) / depotSupply;

        return std::min(
            depotsNeeded,
            MAX_PREDICTIVE_DEPOTS_PLANNED - depotsAlreadyPlanned);
    }

    // A depot already present deep in the queue used to suppress the urgent
    // supply rule without ever reaching ProductionManager. Promote the
    // highest-priority queued depot instead of adding a duplicate.
    static bool promoteUrgentQueuedSupplyDepot(BuildOrderQueue& queue)
    {
        if (BWAPI::Broodwar->self()->getRace() != BWAPI::Races::Terran ||
            queue.size() < 2 ||
            BWAPI::Broodwar->self()->supplyTotal() >= 400)
        {
            return false;
        }

        const int freeSupply =
            BWAPI::Broodwar->self()->supplyTotal() -
            BWAPI::Broodwar->self()->supplyUsed();

        // Do not pull later JSON depots in front of the opening worker cycle.
        // If the bot is actually blocked, urgentSupplyDepotsNeeded() still
        // handles that case explicitly.
        if (BWAPI::Broodwar->getFrameCount() < 24 * 60 && freeSupply > 0)
        {
            return false;
        }

        const bool outOfBook =
            ProductionManager::Instance().isOutOfBook();

        // During the JSON opening, one promoted Depot must finish/leave the
        // construction pipeline before another future book Depot is promoted;
        // otherwise the old triple-Depot opening regression returns. Persistent
        // macro is different: several busy producers can legitimately require
        // two Depots to be under construction at once, and the throughput check
        // below already counts all supply which is actually on the way.
        if (!outOfBook)
        {
            if (BuildingManager::Instance().isBeingBuilt(
                    BWAPI::UnitTypes::Terran_Supply_Depot) ||
                BuildingManager::Instance().getNumUnstarted(
                    BWAPI::UnitTypes::Terran_Supply_Depot) > 0)
            {
                return false;
            }

            for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
            {
                if (unit && unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Supply_Depot &&
                    !unit->isCompleted())
                {
                    return false;
                }
            }
        }

        if (outOfBook)
        {
            // The old fixed 16-supply promotion threshold defeated the new
            // throughput predictor: two producers built supply too early while
            // eight producers could still build it too late. Decide whether a
            // queued Depot must be pulled forward from the same demand forecast
            // used by urgentSupplyDepotsNeeded(), but deliberately exclude the
            // deep queued Depot itself from "supply arriving in time".
            int depotsAlreadyStarted = 0;

            for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
            {
                if (unit && unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Supply_Depot &&
                    !unit->isCompleted())
                {
                    ++depotsAlreadyStarted;
                }
            }

            const int depotsWaitingForConstruction =
                (int)BuildingManager::Instance().getNumUnstarted(
                    BWAPI::UnitTypes::Terran_Supply_Depot);

            const int depotSupply =
                BWAPI::UnitTypes::Terran_Supply_Depot.supplyProvided();

            const int supplyArrivingWithoutQueuedDepot =
                BWAPI::Broodwar->self()->supplyTotal() +
                (depotsAlreadyStarted + depotsWaitingForConstruction) * depotSupply;

            const int wantedTotalSupply =
                std::min(
                    400,
                    BWAPI::Broodwar->self()->supplyUsed() +
                    terranPredictedSupplyDemandBeforeDepotCompletes() +
                    6 * 2);

            if (supplyArrivingWithoutQueuedDepot >= wantedTotalSupply)
            {
                return false;
            }
        }
        else if (freeSupply > 4 * 2)
        {
            return false;
        }

        const MacroAct& top = queue.getHighestPriorityItem().macroAct;
        if (top.isUnit() &&
            top.getUnitType() == BWAPI::UnitTypes::Terran_Supply_Depot)
        {
            return false;
        }

        for (int i = int(queue.size()) - 2; i >= 0; --i)
        {
            const MacroAct& act = queue[i].macroAct;
            if (act.isUnit() &&
                act.getUnitType() == BWAPI::UnitTypes::Terran_Supply_Depot)
            {
                queue.pullToTop(size_t(i));
                DebugQueuePrintf(
                    "queue: promote urgent Supply Depot @ %d",
                    int(queue.size()) - i);
                return true;
            }
        }

        return false;
    }

    // Close a verified two-depot wall as soon as its Barracks starts when a
    // ground rush is recognized or strongly predicted. Ordinary supply
    // forecasting waits too long here because the first Depot still has room.
    static bool queueEarlyRushWallDepot(BuildOrderQueue& queue)
    {
        if (the.selfRace() != BWAPI::Races::Terran ||
            (the.enemyRace() != BWAPI::Races::Protoss &&
             the.enemyRace() != BWAPI::Races::Zerg &&
             the.enemyRace() != BWAPI::Races::Terran) ||
            !the.placer.hasTerranMainWall() ||
            the.placer.isTerranMainWallReleased() ||
            !the.placer.getTerranMainWallDepot2Tile().isValid() ||
            the.now() > 10 * 60 * 24)
        {
            return false;
        }

        const OpeningPlan plan =
            OpponentModel::Instance().getPreparationEnemyPlan();
        const bool historicalRush =
            OpponentModel::Instance().getEnemyPlan() == OpeningPlan::Unknown &&
            OpponentModel::Instance().stronglyPredictsEarlyGroundRush();
        if (plan != OpeningPlan::WorkerRush &&
            plan != OpeningPlan::FastRush &&
            plan != OpeningPlan::HeavyRush &&
            !historicalRush)
        {
            return false;
        }

        bool barracksStarted = false;
        int wallDepots = 0;
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (!unit || !unit->exists()) continue;
            if (unit->getType() == BWAPI::UnitTypes::Terran_Barracks &&
                the.placer.isTerranMainWallBarracks(unit))
            {
                barracksStarted = true;
            }
            if (unit->getType() == BWAPI::UnitTypes::Terran_Supply_Depot &&
                the.placer.isTerranMainWallBuilding(unit))
            {
                ++wallDepots;
            }
        }
        if (!barracksStarted || wallDepots >= 2) return false;

        const BWAPI::UnitType depot = BWAPI::UnitTypes::Terran_Supply_Depot;
        // A Depot buried in JSON is only a promise, not construction. Pull it
        // forward even when the total number of queued goals already reaches 2.
        if (wallDepots +
            int(BuildingManager::Instance().getNumUnstarted(depot)) >= 2)
        {
            return false;
        }

        if (!queue.isEmpty())
        {
            const MacroAct& top = queue.getHighestPriorityItem().macroAct;
            if (top.isUnit() && top.getUnitType() == depot) return true;
        }
        if (!promoteQueuedUnitToTop(queue, depot))
        {
            queue.queueAsHighestPriority(MacroAct(depot, MacroLocation::Main));
        }
        return true;
    }

    int countCompletedSCVsNearBase(Base* base)
    {
        if (!base || !base->getPosition().isValid())
        {
            return 0;
        }

        const int BASE_WORKER_RADIUS = 12 * 32;

        int count = 0;

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists() || !unit->isCompleted())
            {
                continue;
            }

            if (unit->getType() != BWAPI::UnitTypes::Terran_SCV)
            {
                continue;
            }

            if (!unit->getPosition().isValid())
            {
                continue;
            }

            if (unit->getDistance(base->getPosition()) <= BASE_WORKER_RADIUS)
            {
                ++count;
            }
        }

        return count;
    }

    int countTerranSCVsIncludingTraining()
    {
        int count =
            the.my.all.count(BWAPI::UnitTypes::Terran_SCV);

        // Training SCVs are not units yet and therefore are absent from
        // PlayerSnapshot::all. A Command Center has no other trainable unit,
        // so isTraining() safely counts the committed worker without touching
        // BWAPI's crash-prone training queue.
        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (unit && unit->exists() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Command_Center &&
                unit->isTraining())
            {
                ++count;
            }
        }

        return count;
    }

    static int s_lastUrgentSCVQueueFrame = -100000;

    bool shouldQueueUrgentBaseSCV(const BuildOrderQueue& queue)
    {
        

        const int now = BWAPI::Broodwar->getFrameCount();

        // Static state survives when the same bot process starts a new match.
        if (now < s_lastUrgentSCVQueueFrame)
        {
            s_lastUrgentSCVQueueFrame = -100000;
        }

        // This lane exists to keep *idle* expansion Command Centers
        // producing. Waiting five seconds after every emergency SCV meant a
        // newly-completed third/fourth base could remain empty for a long time.
        // One second is enough to avoid command spam while still filling all
        // free CCs quickly.
        if (now - s_lastUrgentSCVQueueFrame < 24)
        {
            return false;
        }

        if (queue.anyInQueue(BWAPI::UnitTypes::Terran_SCV))
        {
            return false;
        }

        // This urgent lane used to have no global cap. When an old mineral
        // line emptied, its workers transferred away, the local count fell
        // below the minimum again, and the abandoned Command Center ordered a
        // replacement every second forever. Respect the same live-resource
        // cap as normal StrategyManager production before inspecting bases.
        const int currentSCVs =
            countTerranSCVsIncludingTraining();

        const int usefulWorkerCap =
            WorkerManager::Instance().getMaxWorkers();

        if (currentSCVs >= usefulWorkerCap)
        {
            return false;
        }

        if (BWAPI::Broodwar->self()->minerals() < 50)
        {
            return false;
        }

        for (Base* base : the.bases.getAll())
        {
            if (!base)
            {
                continue;
            }

            BWAPI::Unit depot = base->getDepot();

            if (!depot || !depot->exists())
            {
                continue;
            }

            if (depot->getPlayer() != BWAPI::Broodwar->self())
            {
                continue;
            }

            if (depot->getType() != BWAPI::UnitTypes::Terran_Command_Center)
            {
                continue;
            }

            if (!depot->isCompleted() || depot->isLifted())
            {
                continue;
            }

            if (base->inWorkerDanger())
            {
                continue;
            }

            const int remainingMineralPatches =
                int(base->getRemainingMinerals().size());

            // This is a mineral-line bootstrap lane. A completed CC at a
            // mined-out base must not manufacture replacements merely because
            // all useful workers correctly transferred elsewhere.
            if (remainingMineralPatches <= 0)
            {
                continue;
            }

            // Only ask the urgent lane to fill a Command Center which
            // can actually train now. Another CC being busy must not suppress
            // this one (the previous global anyCommandCenterTrainingSCV()
            // check caused the observed "3 of 4 bases make no SCVs" behavior).
            if (depot->isTraining() ||
                depot->getRemainingTrainTime() > 0)
            {
                continue;
            }

            const int scvsNearBase =
                countCompletedSCVsNearBase(base);

            const int assignedMineralWorkers =
                base->getNumMineralWorkers();

            // Establish a useful mineral line quickly, then let the normal
            // global worker target own full saturation. The local bootstrap
            // shrinks with the live patch count: 8 patches -> 10 workers,
            // 4 patches -> 8, 2 patches -> 4, 1 patch -> 2.
            const int minimumWorkersHere =
                std::min(
                    10,
                    std::max(2, 2 * remainingMineralPatches));

            if (std::max(scvsNearBase, assignedMineralWorkers) <
                minimumWorkersHere)
            {
                return true;
            }
        }

        return false;
    }


    bool actualEarlyGroundRushDetected()
    {
        const int now = BWAPI::Broodwar->getFrameCount();
        const int RUSH_TIME_LIMIT = 9 * 24 * 60;

        if (now > RUSH_TIME_LIMIT)
        {
            return false;
        }

        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();

        if (!mainBase && !naturalBase)
        {
            return false;
        }

        const int MAIN_RADIUS = 26 * 32;
        const int NATURAL_RADIUS = 22 * 32;

        int enemyCombatScore = 0;
        int enemyCombatCount = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy ||
                !enemy->exists() ||
                !enemy->isVisible() ||
                !enemy->getPosition().isValid() ||
                enemy->isFlying())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (type.isWorker() || type.isSpell() || type.isBuilding())
            {
                continue;
            }

            if (!UnitUtil::CanAttackGround(enemy))
            {
                continue;
            }

            bool nearOurBase = false;

            if (mainBase &&
                mainBase->getPosition().isValid() &&
                enemy->getDistance(mainBase->getPosition()) <= MAIN_RADIUS)
            {
                nearOurBase = true;
            }

            if (naturalBase &&
                naturalBase->getPosition().isValid() &&
                enemy->getDistance(naturalBase->getPosition()) <= NATURAL_RADIUS)
            {
                nearOurBase = true;
            }

            if (!nearOurBase)
            {
                continue;
            }

            ++enemyCombatCount;

            if (type == BWAPI::UnitTypes::Protoss_Zealot) enemyCombatScore += 2;
            else if (type == BWAPI::UnitTypes::Protoss_Dragoon) enemyCombatScore += 3;
            else if (type == BWAPI::UnitTypes::Zerg_Zergling) enemyCombatScore += 1;
            else if (type == BWAPI::UnitTypes::Zerg_Hydralisk) enemyCombatScore += 2;
            else if (type == BWAPI::UnitTypes::Terran_Marine) enemyCombatScore += 1;
            else if (type == BWAPI::UnitTypes::Terran_Vulture) enemyCombatScore += 2;
            else if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) enemyCombatScore += 4;
            else if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) enemyCombatScore += 5;
            else enemyCombatScore += 2;
        }

        return enemyCombatCount >= 2 && enemyCombatScore >= 4;
    }

    bool visibleTwoOverlordsNearOurBases()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Zerg)
        {
            return false;
        }

        const int OVERLORD_THREAT_RADIUS = 20 * 32;

        int overlordsNearBase = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy ||
                !enemy->exists() ||
                !enemy->isVisible() ||
                !enemy->getPosition().isValid())
            {
                continue;
            }

            if (enemy->getType() != BWAPI::UnitTypes::Zerg_Overlord)
            {
                continue;
            }

            for (Base* base : the.bases.getAll())
            {
                if (!base ||
                    base->getOwner() != BWAPI::Broodwar->self() ||
                    !base->getPosition().isValid())
                {
                    continue;
                }

                if (!base->getDepot())
                {
                    continue;
                }

                if (enemy->getDistance(base->getPosition()) <= OVERLORD_THREAT_RADIUS)
                {
                    ++overlordsNearBase;
                    break;
                }
            }

            if (overlordsNearBase >= 2)
            {
                return true;
            }
        }

        return false;
    }

    static int countOwnedAndTrainingTerranGhosts()
    {
        int count = 0;

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists())
            {
                continue;
            }

            if (unit->getType() == BWAPI::UnitTypes::Terran_Ghost)
            {
                ++count;
                continue;
            }

            if (unit->getType() == BWAPI::UnitTypes::Terran_Barracks &&
                unit->isTraining())
            {
                if (unit->getLastCommand().getUnitType() == BWAPI::UnitTypes::Terran_Ghost)
                {
                    ++count;
                }
            }
        }

        return count;
    }

    int rememberedEnemyUnitCount(BWAPI::UnitType wantedType)
    {
        int count = 0;

        for (const auto& kv :
            InformationManager::Instance().getUnitData(the.enemy()).getUnits())
        {
            if (kv.second.type == wantedType)
            {
                ++count;
            }
        }

        return count;
    }

    static int bioTankLurkerTankTarget()
    {
        if (!StrategyManager::Instance().bioTankPlanActive() ||
            !StrategyManager::Instance().bioTankUsesTanks() ||
            the.enemyRace() != BWAPI::Races::Zerg)
        {
            return 0;
        }
        // UnitData forgets confirmed deaths; an old sighting alone must not
        // keep the maximum artillery floor after the Lurker army is gone.
        const int lurkers =
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Lurker) +
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Lurker_Egg);
        return BioProductionPolicy::lurkerTankTarget(lurkers,
            enemyHasOpeningThreat(OpeningThreat::ZergTwoHatchLurker));
    }

    bool enemyFlyingUnitEverSeen()
    {
        static int lastFrame = -1;
        static bool seen = false;
        const int now = BWAPI::Broodwar->getFrameCount();
        if (now < lastFrame) seen = false;
        lastFrame = now;
        if (seen) return true;

        // A harmless scout can reveal transport tech. Keep the four-Goliath
        // minimum after it leaves vision; do not mistake lifted buildings for
        // an enemy air unit.
        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (enemy && enemy->exists() && enemy->isVisible() &&
                enemy->isFlying() && !enemy->getType().isBuilding())
            {
                seen = true;
                break;
            }
        }
        return seen;
    }

    bool enemyFlyingBuildingEverSeen()
    {
        static int lastFrame = -1;
        static bool seen = false;
        const int now = BWAPI::Broodwar->getFrameCount();
        if (now < lastFrame) seen = false;
        lastFrame = now;
        if (seen) return true;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (enemy && enemy->exists() && enemy->isVisible() &&
                enemy->isFlying() && enemy->getType().isBuilding())
            {
                seen = true;
                break;
            }
        }
        return seen;
    }

    int countDangerousEnemyAirUnits()
    {
        int count = 0;
        for (const auto & kv :
            InformationManager::Instance().getUnitData(the.enemy()).getUnits())
        {
            if (HasEnemyThreat(
                    EnemyThreatForUnitType(kv.second.type),
                    EnemyThreat::GroundAir))
            {
                ++count;
            }
        }
        return count;
    }

    int dangerousEnemyAirStrength()
    {
        // Unit counts are enough for ordinary air, but capital ships must
        // carry more weight when deciding whether a small remembered ground
        // screen should still keep the whole composition in a mixed plan.
        int strength = 0;
        for (const auto & kv :
            InformationManager::Instance().getUnitData(the.enemy()).getUnits())
        {
            const BWAPI::UnitType type = kv.second.type;
            const EnemyThreatMask threats = EnemyThreatForUnitType(type);
            if (!HasEnemyThreat(threats, EnemyThreat::GroundAir))
            {
                continue;
            }

            if (HasEnemyThreat(threats, EnemyThreat::CapitalAir))
            {
                strength += 4;
            }
            else if (type == BWAPI::UnitTypes::Protoss_Arbiter ||
                type == BWAPI::UnitTypes::Zerg_Guardian)
            {
                strength += 2;
            }
            else
            {
                ++strength;
            }
        }
        return strength;
    }

    int visibleDangerousEnemyAirUnits()
    {
        int count = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible())
            {
                continue;
            }

            if (!enemy->isFlying())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (HasEnemyThreat(
                    EnemyThreatForUnitType(type),
                    EnemyThreat::GroundAir))
            {
                ++count;
            }
        }

        return count;
    }

    int visibleZergCombatAirUnits()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Zerg)
        {
            return 0;
        }

        int count = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy ||
                !enemy->exists() ||
                !enemy->isVisible() ||
                !enemy->getPosition().isValid())
            {
                continue;
            }

            if (!enemy->isFlying())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (HasEnemyThreat(
                    EnemyThreatForUnitType(type),
                    EnemyThreat::GroundAir))
            {
                ++count;
            }
        }

        return count;
    }

    int visibleZergCombatAirUnitsNearOwnedBase()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Zerg)
        {
            return 0;
        }

        const int NEAR_BASE_RADIUS = 24 * 32;

        int count = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy ||
                !enemy->exists() ||
                !enemy->isVisible() ||
                !enemy->getPosition().isValid() ||
                !enemy->isFlying())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (!HasEnemyThreat(
                    EnemyThreatForUnitType(type),
                    EnemyThreat::GroundAir))
            {
                continue;
            }

            for (Base* base : the.bases.getAll())
            {
                if (!base ||
                    base->getOwner() != the.self() ||
                    !base->getPosition().isValid() ||
                    !base->getDepot())
                {
                    continue;
                }

                if (enemy->getDistance(base->getPosition()) <= NEAR_BASE_RADIUS)
                {
                    ++count;
                    break;
                }
            }
        }

        return count;
    }

    bool enemyTerranWraithThreatDetected()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return false;
        }

        if (rememberedEnemyUnitCount(BWAPI::UnitTypes::Terran_Wraith) > 0)
        {
            return true;
        }

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible())
            {
                continue;
            }

            if (enemy->getType() == BWAPI::UnitTypes::Terran_Wraith)
            {
                return true;
            }
        }

        return false;
    }

    bool directTerranWraithEvidence()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return false;
        }

        return
            OpponentModel::Instance().getEnemyPlan() == OpeningPlan::Wraith ||
            enemyHasOpeningThreat(OpeningThreat::FastAirTech) ||
            enemyHasOpeningThreat(OpeningThreat::TerranTwoStarport) ||
            enemyTerranWraithThreatDetected();
    }

    bool strongTerranWraithPrediction()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return false;
        }

        // A selected UCB arm is opponent-specific evidence. Otherwise trust
        // the opening prediction only after the shared 3-game/70% gate.
        return
            StrategyManager::Instance().getTerranPlan() ==
                TerranPlanId::TvT_AntiAir ||
            OpponentModel::Instance().stronglyPredictsEnemyPlan(
                OpeningPlan::Wraith);
    }

    bool terranWraithPreparationWindowOpen()
    {
        // A Wraith which is still alive keeps the protection active at any
        // game time. Opening recognition by itself is only a preparation
        // signal: stop renewing it after ten minutes, or once two enemy Tanks
        // prove that the opener has transitioned and no Wraith remains known.
        if (enemyTerranWraithThreatDetected())
        {
            return true;
        }

        const int now = BWAPI::Broodwar->getFrameCount();
        if (now > 10 * 60 * 24 || enemyTerranTankCoreDetected())
        {
            return false;
        }

        if (directTerranWraithEvidence())
        {
            return true;
        }

        if (!strongTerranWraithPrediction())
        {
            return false;
        }

        const int predictedAirFrame =
            OpponentModel::Instance().predictEnemyAirUnitTiming();

        // Begin roughly 90 seconds before the learned first-air timing. With
        // too little timing data, 3:30 leaves enough room for Engineering Bay,
        // Turrets and the first Goliaths without replacing the opening itself.
        const int preparationStart = predictedAirFrame > 0
            ? std::max(0, predictedAirFrame - 90 * 24)
            : 3 * 60 * 24 + 30 * 24;

        return now >= preparationStart;
    }

    int countEnemyDangerousAirTechBuildings()
    {
        return
            // Protoss air tech
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Stargate) +
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Fleet_Beacon) +

            // Terran air tech
            the.your.seen.count(BWAPI::UnitTypes::Terran_Starport) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Control_Tower) +
            the.your.seen.count(BWAPI::UnitTypes::Terran_Physics_Lab) +

            // Zerg air tech
            the.your.seen.count(BWAPI::UnitTypes::Zerg_Spire) +
            the.your.seen.count(BWAPI::UnitTypes::Zerg_Greater_Spire);
    }


    int countEnemyTerranFactories()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return 0;
        }

        return the.your.seen.count(BWAPI::UnitTypes::Terran_Factory);
    }

    int countEnemyTerranStarports()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return 0;
        }

        return the.your.seen.count(BWAPI::UnitTypes::Terran_Starport);
    }

    int countEnemyTerranBarracks()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return 0;
        }

        return the.your.seen.count(BWAPI::UnitTypes::Terran_Barracks);
    }

    int countEnemyProtossStargates()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Protoss)
        {
            return 0;
        }

        return the.your.seen.count(BWAPI::UnitTypes::Protoss_Stargate);
    }

    int countEnemyProtossRobotics()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Protoss)
        {
            return 0;
        }

        return
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Robotics_Facility) +
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Robotics_Support_Bay);
    }

    bool enemyTerranFactoryCommitmentDetected()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Terran)
        {
            return false;
        }

        const int factories = countEnemyTerranFactories();

        // 3+ factories means enemy is clearly committing to mech.
        if (factories >= 3)
        {
            return true;
        }

        // 2 factories + seen tanks/goliaths also means mech direction.
        if (factories >= 2 &&
            (
                the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
                the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) +
                the.your.seen.count(BWAPI::UnitTypes::Terran_Goliath) >= 3
                ))
        {
            return true;
        }

        return false;
    }

    int countEnemyAirProductionBuildings()
    {
        const BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();

        if (enemyRace == BWAPI::Races::Terran)
        {
            return rememberedEnemyUnitCount(BWAPI::UnitTypes::Terran_Starport);
        }

        if (enemyRace == BWAPI::Races::Protoss)
        {
            return rememberedEnemyUnitCount(BWAPI::UnitTypes::Protoss_Stargate);
        }

        if (enemyRace == BWAPI::Races::Zerg)
        {
            return
                rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Spire) +
                rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Greater_Spire);
        }

        return 0;
    }

    bool enemyHasCarrierTech()
    {
        if (rememberedEnemyUnitCount(BWAPI::UnitTypes::Protoss_Carrier) > 0 ||
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Protoss_Fleet_Beacon) > 0)
        {
            return true;
        }

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (type == BWAPI::UnitTypes::Protoss_Carrier ||
                type == BWAPI::UnitTypes::Protoss_Fleet_Beacon)
            {
                return true;
            }
        }

        return false;
    }

    EnemyThreatMask detectEnemyThreatMask()
    {
        EnemyThreatMask mask = 0;

        // Unit mapping is shared with GameRecordNow, so live reactions and
        // next-game learning cannot disagree about Valkyries, spellcasters or
        // capital ships.
        for (const auto & kv :
            InformationManager::Instance().getUnitData(the.enemy()).getUnits())
        {
            mask |= EnemyThreatForUnitType(kv.second.type);
        }

        // Add only strong technology inferences. A plain Stargate/Starport or
        // Spire is deliberately absent: it cannot tell Scout from Corsair,
        // Wraith from Dropship, or Mutalisk from Scourge.
        if (enemyHasCarrierTech())
        {
            mask |= EnemyThreatBit(EnemyThreat::GroundAir);
            mask |= EnemyThreatBit(EnemyThreat::CapitalAir);
        }

        if (rememberedEnemyUnitCount(BWAPI::UnitTypes::Terran_Physics_Lab) > 0)
        {
            mask |= EnemyThreatBit(EnemyThreat::GroundAir);
            mask |= EnemyThreatBit(EnemyThreat::CapitalAir);
            mask |= EnemyThreatBit(EnemyThreat::Spellcaster);
        }

        if (rememberedEnemyUnitCount(BWAPI::UnitTypes::Terran_Covert_Ops) > 0)
        {
            mask |= EnemyThreatBit(EnemyThreat::Spellcaster);
            mask |= EnemyThreatBit(EnemyThreat::Cloak);
        }

        if (rememberedEnemyUnitCount(BWAPI::UnitTypes::Protoss_Arbiter_Tribunal) > 0)
        {
            mask |= EnemyThreatBit(EnemyThreat::GroundAir);
            mask |= EnemyThreatBit(EnemyThreat::Spellcaster);
            mask |= EnemyThreatBit(EnemyThreat::Cloak);
        }

        if (rememberedEnemyUnitCount(BWAPI::UnitTypes::Protoss_Robotics_Support_Bay) > 0)
        {
            mask |= EnemyThreatBit(EnemyThreat::Splash);
        }

        if (rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Defiler_Mound) > 0)
        {
            mask |= EnemyThreatBit(EnemyThreat::Spellcaster);
            mask |= EnemyThreatBit(EnemyThreat::Splash);
        }

        if (InformationManager::Instance().enemyHasCloakTech() ||
            enemyHasOpeningThreat(OpeningThreat::CloakTech))
        {
            mask |= EnemyThreatBit(EnemyThreat::Cloak);
        }

        return mask;
    }

    EnemyThreatMask currentEnemyThreatMask()
    {
        static EnemyThreatMask cachedMask = 0;
        static int cachedFrame = -100000;
        static int lastObservedFrame = -1;

        const int now = BWAPI::Broodwar->getFrameCount();
        if (lastObservedFrame > now)
        {
            cachedMask = 0;
            cachedFrame = -100000;
        }
        lastObservedFrame = now;

        if (now >= cachedFrame + 12)
        {
            cachedMask = detectEnemyThreatMask();
            cachedFrame = now;
        }

        return cachedMask;
    }

    int rememberedEnemyCarrierCount()
    {
        int count = 0;

        for (const auto& kv :
            InformationManager::Instance().getUnitData(the.enemy()).getUnits())
        {
            const UnitInfo& ui = kv.second;

            // Destroyed units are removed from UnitData. A Carrier which left
            // its last visible position is still alive and must remain in the
            // production estimate; otherwise every scan expiration would make
            // the response shrink back to the technology-only minimum.
            if (ui.type == BWAPI::UnitTypes::Protoss_Carrier)
            {
                ++count;
            }
        }

        return count;
    }

    int pureAirGoliathTarget(bool carrierTechnologySeen)
    {
        const BWAPI::Player self = BWAPI::Broodwar->self();
        const BWAPI::UnitType goliath = BWAPI::UnitTypes::Terran_Goliath;

        const int currentGoliaths =
            the.my.all.count(goliath);

        // PureAir is a composition, not a fixed Goliath count. Once the
        // minimum anti-air core exists, spend every currently available supply
        // slot on more Goliaths instead of stopping at 12/20/40 while the bank
        // and Factories are idle. Leave only enough room for the support units
        // which the PureAir plan still wants to finish.
        int supportSupplyReserve = 0;

        const int scienceVessels =
            the.my.all.count(BWAPI::UnitTypes::Terran_Science_Vessel);

        if (scienceVessels < 1)
        {
            supportSupplyReserve +=
                BWAPI::UnitTypes::Terran_Science_Vessel.supplyRequired();
        }

        if (carrierTechnologySeen)
        {
            const int desiredGhosts = 5;
            const int ghosts =
                the.my.all.count(BWAPI::UnitTypes::Terran_Ghost);

            supportSupplyReserve +=
                std::max(0, desiredGhosts - ghosts) *
                BWAPI::UnitTypes::Terran_Ghost.supplyRequired();
        }

        const int freeSupply = std::max(
            0,
            self->supplyTotal() - self->supplyUsed());

        const int supplyAvailableForGoliaths = std::max(
            0,
            freeSupply - supportSupplyReserve);

        const int goliathSupply =
            std::max(1, goliath.supplyRequired());

        const int supplyFillTarget =
            currentGoliaths + supplyAvailableForGoliaths / goliathSupply;

        int threatMinimum = 20;

        if (carrierTechnologySeen)
        {
            const int carriers = rememberedEnemyCarrierCount();

            // Carrier tech must trigger a useful minimum before the first
            // Carrier is visible, but the minimum is no longer a production cap.
            threatMinimum =
                carriers <= 0
                ? 12
                : std::max(8, std::min(40, 3 * carriers + 4));
        }

        return std::max(threatMinimum, supplyFillTarget);
    }

    int mixedAirGoliathTarget(EnemyArmyMode mode)
    {
        if (mode != EnemyArmyMode::MixedAirHeavyGround &&
            mode != EnemyArmyMode::MixedAirLightGround)
        {
            return 0;
        }

        // This is an absolute target derived from the current composition, not
        // a rolling "current + N" goal. A fixed cap of 10-12 Goliaths prevented
        // Mixed compositions from scaling at all: 30 Tanks could still leave the
        // scheduler satisfied with 10 Goliaths. Keep an air-threat floor, then
        // let the deficit scheduler converge toward the requested ratios.
        const int airEstimate = std::max(
            visibleDangerousEnemyAirUnits(),
            countDangerousEnemyAirUnits());

        const int tanks =
            the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
        const int vultures =
            the.my.all.count(BWAPI::UnitTypes::Terran_Vulture);

        if (mode == EnemyArmyMode::MixedAirLightGround)
        {
            const int airFloor = std::max(4, 2 * airEstimate + 2);
            // LightMixed: Tank : Vulture : Goliath ~= 1 : 4 : 4.
            return std::max(airFloor, std::max(vultures, 4 * tanks));
        }

        const int airFloor = std::max(4, 2 * airEstimate);
        // HeavyMixed: Tanks and Goliaths are co-equal cores. Vultures remain a
        // smaller mine/screen layer through their production weight below.
        return std::max(airFloor, tanks);
    }

    bool enemyHasMutaliskTech()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Zerg)
        {
            return false;
        }

        return
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Spire) > 0 ||
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Greater_Spire) > 0 ||
            enemyHasOpeningThreat(OpeningThreat::ZergMutaliskTech) ||
            enemyHasOpeningThreat(OpeningThreat::ZergTwoHatchMuta);
    }

    // Immediate strategic signal for switching the macro identity to PureAir.
    // A recognized 2 Hatch Muta is always an air commitment. For a generic
    // Spire, preserve MixedAir only while a meaningful Zerg ground army is
    // actually visible now; remembered old lings/hydras are not enough.
    bool zergMutaliskPureAirCommitment()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Zerg ||
            !enemyHasMutaliskTech())
        {
            return false;
        }

        const int visibleAir = visibleZergCombatAirUnits();
        const int knownAir =
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Mutalisk) +
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Guardian);

        // A Spire alone is preparation evidence, not proof that the opponent
        // abandoned ground. Full PureAir requires either the recognized 2-Hatch
        // Muta timing or an actual combat-air mass.
        if (!enemyHasOpeningThreat(OpeningThreat::ZergTwoHatchMuta) &&
            visibleAir < 4 &&
            knownAir < 5)
        {
            return false;
        }

        int visibleLightGround = 0;
        int visibleHeavyGround = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                enemy->isFlying() || enemy->getType().isWorker() ||
                enemy->getType().isBuilding() || enemy->getType().isSpell())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (type == BWAPI::UnitTypes::Zerg_Lurker ||
                type == BWAPI::UnitTypes::Zerg_Ultralisk)
            {
                ++visibleHeavyGround;
            }
            else if (type == BWAPI::UnitTypes::Zerg_Zergling ||
                type == BWAPI::UnitTypes::Zerg_Hydralisk)
            {
                ++visibleLightGround;
            }
        }

        const bool currentGroundPressure =
            enemyOpeningGroundPressure() ||
            actualEarlyGroundRushDetected() ||
            actualEarlyLightGroundRushDetected() ||
            visibleEnemyGroundNearMainOrClosestExpansion();

        return
            !currentGroundPressure &&
            visibleHeavyGround < 2 &&
            visibleLightGround < 4;
    }

    bool enemyCommittedToAirProduction()
    {
        const BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();

        const int seenAir = countDangerousEnemyAirUnits();
        const int visibleAir = visibleDangerousEnemyAirUnits();

        if (enemyRace == BWAPI::Races::Protoss)
        {
            // Two Stargates can be Corsair-only. Require a real air-to-ground
            // unit unless Fleet Beacon/Carrier tech is known.
            return
                enemyHasCarrierTech() ||
                (countEnemyAirProductionBuildings() >= 2 &&
                    (visibleAir >= 2 || seenAir >= 2));
        }

        if (enemyRace == BWAPI::Races::Terran)
        {
            return countEnemyTerranStarports() >= 2 &&
                (visibleAir >= 2 || seenAir >= 4);
        }

        if (enemyRace == BWAPI::Races::Zerg)
        {
            return visibleAir >= 4 || seenAir >= 7;
        }

        return false;
    }

    EnemyAirLevel detectEnemyAirLevel()
    {
        const BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();

        if (enemyHasCarrierTech())
        {
            return EnemyAirLevel::CarrierAir;
        }

        const int seenAir = countDangerousEnemyAirUnits();
        const int visibleAir = visibleDangerousEnemyAirUnits();
        const int airProduction = countEnemyAirProductionBuildings();

        if (seenAir <= 0 && visibleAir <= 0 && airProduction <= 0)
        {
            return EnemyAirLevel::NoAir;
        }

        if (enemyRace == BWAPI::Races::Terran)
        {
            const int wraiths = rememberedEnemyUnitCount(BWAPI::UnitTypes::Terran_Wraith);
            const int battlecruisers = rememberedEnemyUnitCount(BWAPI::UnitTypes::Terran_Battlecruiser);

            if (battlecruisers > 0 ||
                wraiths >= 5 ||
                (airProduction >= 2 && seenAir >= 3) ||
                (airProduction >= 2 && visibleAir >= 2))
            {
                return EnemyAirLevel::CommittedAir;
            }

            return EnemyAirLevel::SmallAir;
        }

        if (enemyRace == BWAPI::Races::Protoss)
        {
            const int scouts = rememberedEnemyUnitCount(BWAPI::UnitTypes::Protoss_Scout);
            const int arbiters = rememberedEnemyUnitCount(BWAPI::UnitTypes::Protoss_Arbiter);

            if ((airProduction >= 2 && (seenAir >= 2 || visibleAir >= 2)) ||
                scouts >= 3 ||
                arbiters > 0 ||
                seenAir >= 4 ||
                visibleAir >= 3)
            {
                return EnemyAirLevel::CommittedAir;
            }

            return EnemyAirLevel::SmallAir;
        }

        if (enemyRace == BWAPI::Races::Zerg)
        {
            const int mutalisks = rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Mutalisk);
            const int guardians = rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Guardian);

            if (guardians > 0 ||
                mutalisks >= 5 ||
                seenAir >= 6 ||
                visibleAir >= 4)
            {
                return EnemyAirLevel::CommittedAir;
            }

            return EnemyAirLevel::SmallAir;
        }

        return EnemyAirLevel::SmallAir;
    }

    int countEnemyHeavyGroundUnits()
    {
        BWAPI::Race enemyRace = the.enemy()->getRace();

        if (enemyRace == BWAPI::Races::Terran)
        {
            return
                the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
                the.your.seen.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
        }
        else if (enemyRace == BWAPI::Races::Protoss)
        {
            return
                the.your.seen.count(BWAPI::UnitTypes::Protoss_Dragoon) +
                3 * the.your.seen.count(BWAPI::UnitTypes::Protoss_Reaver);
        }
        else if (enemyRace == BWAPI::Races::Zerg)
        {
            return
                2 * the.your.seen.count(BWAPI::UnitTypes::Zerg_Lurker) +
                3 * the.your.seen.count(BWAPI::UnitTypes::Zerg_Ultralisk);
        }

        return 0;
    }

    int countEnemyLightGroundUnits()
    {
        BWAPI::Race enemyRace = the.enemy()->getRace();

        if (enemyRace == BWAPI::Races::Terran)
        {
            return
                the.your.seen.count(BWAPI::UnitTypes::Terran_Marine) +
                the.your.seen.count(BWAPI::UnitTypes::Terran_Medic);
        }
        else if (enemyRace == BWAPI::Races::Protoss)
        {
            return
                the.your.seen.count(BWAPI::UnitTypes::Protoss_Zealot);
        }
        else if (enemyRace == BWAPI::Races::Zerg)
        {
            return
                the.your.seen.count(BWAPI::UnitTypes::Zerg_Zergling) +
                the.your.seen.count(BWAPI::UnitTypes::Zerg_Hydralisk);
        }

        return 0;
    }

    int visibleEnemyUltraliskCount()
    {
        int count = 0;
        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (enemy && enemy->exists() && enemy->isVisible() &&
                enemy->getType() == BWAPI::UnitTypes::Zerg_Ultralisk)
            {
                ++count;
            }
        }
        return count;
    }

    EnemyArmyMode detectVisibleArmyModeNearOurBases()
    {
        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();

        if (!mainBase && !naturalBase)
        {
            return EnemyArmyMode::Default;
        }

        const int MAIN_RADIUS = 26 * 32;
        const int NATURAL_RADIUS = 22 * 32;

        int airScore = 0;
        int lightScore = 0;
        int heavyScore = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy ||
                !enemy->exists() ||
                !enemy->isVisible() ||
                !enemy->getPosition().isValid())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (type.isWorker() || type.isSpell() || type.isBuilding())
            {
                continue;
            }

            bool nearOurBase = false;

            if (mainBase &&
                mainBase->getPosition().isValid() &&
                enemy->getDistance(mainBase->getPosition()) <= MAIN_RADIUS)
            {
                nearOurBase = true;
            }

            if (naturalBase &&
                naturalBase->getPosition().isValid() &&
                enemy->getDistance(naturalBase->getPosition()) <= NATURAL_RADIUS)
            {
                nearOurBase = true;
            }

            if (!nearOurBase)
            {
                continue;
            }

            if (enemy->isFlying())
            {
                if (HasEnemyThreat(
                        EnemyThreatForUnitType(type),
                        EnemyThreat::GroundAir))
                {
                    airScore += 2;
                }

                continue;
            }

            if (!UnitUtil::CanAttackGround(enemy) && !UnitUtil::CanAttackAir(enemy))
            {
                continue;
            }

            // Heavy ground
            if (type == BWAPI::UnitTypes::Protoss_Dragoon ||
                type == BWAPI::UnitTypes::Protoss_Reaver ||
                type == BWAPI::UnitTypes::Protoss_Archon ||
                type == BWAPI::UnitTypes::Protoss_Dark_Templar ||
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
                type == BWAPI::UnitTypes::Terran_Goliath ||
                type == BWAPI::UnitTypes::Zerg_Lurker ||
                type == BWAPI::UnitTypes::Zerg_Ultralisk)
            {
                heavyScore += 3;
            }
            // Light ground
            else if (type == BWAPI::UnitTypes::Protoss_Zealot ||
                type == BWAPI::UnitTypes::Terran_Marine ||
                type == BWAPI::UnitTypes::Terran_Medic ||
                type == BWAPI::UnitTypes::Terran_Firebat ||
                type == BWAPI::UnitTypes::Terran_Vulture ||
                type == BWAPI::UnitTypes::Zerg_Zergling ||
                type == BWAPI::UnitTypes::Zerg_Hydralisk)
            {
                lightScore += 2;
            }
            else
            {
                lightScore += 1;
            }
        }

        // A large air army with one token ground unit is not a mixed army.
        // Let the global classifier compare the full remembered forces rather
        // than allowing a single nearby Dragoon to pin us in AntiDragoon.
        const int localGroundScore = heavyScore + lightScore;
        const bool localAirMassClearlyDominates =
            airScore >= 12 &&
            (localGroundScore == 0 || airScore >= 2 * localGroundScore);

        if (localAirMassClearlyDominates)
        {
            return EnemyArmyMode::Default;
        }

        if (airScore >= 4 && heavyScore >= lightScore && heavyScore > 0)
        {
            return EnemyArmyMode::MixedAirHeavyGround;
        }

        if (airScore >= 4 && lightScore > heavyScore)
        {
            return EnemyArmyMode::MixedAirLightGround;
        }

        if (airScore >= 4)
        {
            // No local ground army was observed. Zerg has its explicit Spire
            // commitment rule; for Protoss/Terran defer to the global air
            // classifier instead of inventing a MixedAirLightGround army.
            if (zergMutaliskPureAirCommitment())
            {
                return EnemyArmyMode::PureAir;
            }

            return EnemyArmyMode::Default;
        }

        if (heavyScore >= 3 && heavyScore >= lightScore)
        {
            return EnemyArmyMode::HeavyGround;
        }

        if (lightScore >= 4)
        {
            return EnemyArmyMode::LightGround;
        }

        return EnemyArmyMode::Default;
    }

    EnemyArmyMode detectEnemyArmyMode()
    {
        const int enemyHeavyGroundCount = countEnemyHeavyGroundUnits();
        const int enemyLightGroundCount = countEnemyLightGroundUnits();

        const BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();
        const int visibleZergAir = visibleZergCombatAirUnits();
        const int visibleZergAirNearBase = visibleZergCombatAirUnitsNearOwnedBase();

        const int knownZergCombatAir =
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Mutalisk) +
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Guardian);

        // A live Ultralisk is decisive heavy-ground evidence. Cumulative
        // Zergling/Hydralisk sightings must not label the present army Light
        // while Ultras are visible elsewhere on the map (outside base radii).
        if (enemyRace == BWAPI::Races::Zerg &&
            visibleEnemyUltraliskCount() > 0)
        {
            return visibleZergAir > 0 || knownZergCombatAir > 0
                ? EnemyArmyMode::MixedAirHeavyGround
                : EnemyArmyMode::HeavyGround;
        }

        // A confirmed 2 Hatch Muta or actual Mutalisk/Guardian mass is an
        // immediate PureAir commitment. A generic Spire alone prepares AA but
        // no longer erases the Tank/Vulture ground shell.
        // Do NOT use the cumulative `the.your.seen` ground counters here: four
        // Zerglings seen three minutes ago used to keep a later 2 Hatch Muta
        // classified as MixedAirLight forever. Preserve a mixed plan only when
        // meaningful ground units are actually visible now.
        if (zergMutaliskPureAirCommitment())
        {
            int visibleLightGround = 0;
            int visibleHeavyGround = 0;

            for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
            {
                if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                    enemy->isFlying() || enemy->getType().isWorker() ||
                    enemy->getType().isBuilding() || enemy->getType().isSpell())
                {
                    continue;
                }

                const BWAPI::UnitType type = enemy->getType();

                if (type == BWAPI::UnitTypes::Zerg_Lurker ||
                    type == BWAPI::UnitTypes::Zerg_Ultralisk)
                {
                    ++visibleHeavyGround;
                }
                else if (type == BWAPI::UnitTypes::Zerg_Zergling ||
                    type == BWAPI::UnitTypes::Zerg_Hydralisk)
                {
                    ++visibleLightGround;
                }
            }

            if (visibleHeavyGround >= 2 &&
                visibleHeavyGround >= visibleLightGround)
            {
                return EnemyArmyMode::MixedAirHeavyGround;
            }

            if (visibleLightGround >= 4 &&
                visibleLightGround > visibleHeavyGround)
            {
                return EnemyArmyMode::MixedAirLightGround;
            }

            return EnemyArmyMode::PureAir;
        }

        if (enemyRace == BWAPI::Races::Zerg &&
            (
                visibleZergAir >= 4 ||
                visibleZergAirNearBase >= 3 ||
                knownZergCombatAir >= 5
                ))
        {
            if (enemyHeavyGroundCount >= 2 &&
                enemyHeavyGroundCount >= enemyLightGroundCount)
            {
                return EnemyArmyMode::MixedAirHeavyGround;
            }

            if (enemyLightGroundCount >= 4 &&
                enemyLightGroundCount > enemyHeavyGroundCount)
            {
                return EnemyArmyMode::MixedAirLightGround;
            }

            return EnemyArmyMode::PureAir;
        }

        const EnemyArmyMode nearBaseMode = detectVisibleArmyModeNearOurBases();

        const EnemyAirLevel airLevel = detectEnemyAirLevel();

        // Opening labels and an empty Starport are preparation evidence, not
        // proof that an air army still exists. Actual remembered/visible air,
        // committed production or capital-air technology defines live army
        // posture. This allows Wraith -> Tank and Stargate -> ground switches
        // to leave Mixed after the short remembered-air grace period below.
        const bool hasAnyRealAir =
            airLevel == EnemyAirLevel::CommittedAir ||
            airLevel == EnemyAirLevel::CarrierAir ||
            countDangerousEnemyAirUnits() > 0 ||
            visibleDangerousEnemyAirUnits() > 0;

        const bool hasCommittedAir =
            airLevel == EnemyAirLevel::CommittedAir ||
            airLevel == EnemyAirLevel::CarrierAir;

        const bool hasCarrierAir =
            airLevel == EnemyAirLevel::CarrierAir;

        const int enemyAirStrength = std::max(
            dangerousEnemyAirStrength(),
            visibleDangerousEnemyAirUnits());

        const bool heavyGroundDominates =
            enemyHeavyGroundCount >= 2 &&
            enemyHeavyGroundCount >= enemyLightGroundCount;

        const bool lightGroundDominates =
            enemyLightGroundCount >= 8 &&
            enemyLightGroundCount > enemyHeavyGroundCount;

        const OpeningPlan enemyPlan = OpponentModel::Instance().getEnemyPlan();
        const OpeningPlan likelyEnemyPlan = OpponentModel::Instance().getPreparationEnemyPlan();

        const bool enemyHeavyRush =
            enemyPlan == OpeningPlan::HeavyRush ||
            likelyEnemyPlan == OpeningPlan::HeavyRush;

        // Opening recognition is intentionally sticky, but army composition
        // is not. Venator, for example, may reveal 3 Gate Dragoons and later
        // field almost only Scouts/Carriers. Once the real air force clearly
        // outweighs the remembered ground screen, the old opening label must
        // no longer force MixedAirHeavyGround and TvP_AntiDragoon.
        const bool airMassClearlyDominatesGround =
            enemyAirStrength >= 8 &&
            enemyAirStrength >= 2 * enemyHeavyGroundCount &&
            enemyAirStrength >= enemyLightGroundCount;

        // A sieged Tank, or two mobile Tanks, changes how the ground fight
        // must be taken even when many Marines/Medics outnumber them. The
        // nearby-base score otherwise labels this army LightGround and makes
        // our Mixed response mostly Vultures and Goliaths.
        const bool liveTerranTankCore =
            enemyRace == BWAPI::Races::Terran &&
            (visibleEnemyTerranSiegedTankCount() > 0 ||
             visibleEnemyTerranTankCount() >= 2);
        if (liveTerranTankCore && !airMassClearlyDominatesGround)
        {
            return hasAnyRealAir
                ? EnemyArmyMode::MixedAirHeavyGround
                : EnemyArmyMode::HeavyGround;
        }

        if (nearBaseMode != EnemyArmyMode::Default)
        {
            return nearBaseMode;
        }

        // Carrier tech is an early commitment signal. Keep a mixed response
        // only for a genuinely substantial known ground army; a few surviving
        // opening Dragoons are already covered by the Tanks we have produced.
        if (hasCarrierAir)
        {
            const bool substantialHeavyGround =
                enemyHeavyGroundCount >= 6 &&
                !airMassClearlyDominatesGround;

            const bool substantialLightGround =
                enemyLightGroundCount >= 10 &&
                !airMassClearlyDominatesGround;

            if (substantialHeavyGround)
            {
                return EnemyArmyMode::MixedAirHeavyGround;
            }

            if (substantialLightGround)
            {
                return EnemyArmyMode::MixedAirLightGround;
            }

            return EnemyArmyMode::PureAir;
        }

        if (hasCommittedAir && airMassClearlyDominatesGround)
        {
            return EnemyArmyMode::PureAir;
        }

        if (enemyOpeningHeavyPressure())
        {
            return hasAnyRealAir
                ? EnemyArmyMode::MixedAirHeavyGround
                : EnemyArmyMode::HeavyGround;
        }

        // MassProduction alone says only that many production buildings were
        // seen; two Starports must not be reinterpreted as light ground.
        if (enemyOpeningLightPressure())
        {
            return hasAnyRealAir
                ? EnemyArmyMode::MixedAirLightGround
                : EnemyArmyMode::LightGround;
        }

        if (enemyHeavyRush && actualEarlyLightGroundRushDetected())
        {
            if (hasAnyRealAir)
            {
                return EnemyArmyMode::MixedAirLightGround;
            }

            return EnemyArmyMode::LightGround;
        }

        // Any real air + heavy ground = mixed.
        // Even 1 Wraith / 1 Mutalisk must not allow us to stay pure HeavyGround,
        // otherwise tanks can be broken by unsupported air.
        if (hasAnyRealAir && heavyGroundDominates)
        {
            return EnemyArmyMode::MixedAirHeavyGround;
        }

        // Any real air + light ground = mixed.
        if (hasAnyRealAir && lightGroundDominates)
        {
            return EnemyArmyMode::MixedAirLightGround;
        }

        // Committed air without meaningful ground = PureAir.
        if (hasCommittedAir)
        {
            return EnemyArmyMode::PureAir;
        }

        // Small air without known ground should be handled as mixed, not urgent/pure.
        // This gives us tanks + vultures + some goliaths.
        if (hasAnyRealAir)
        {
            return EnemyArmyMode::MixedAirHeavyGround;
        }

        if (enemyTerranFactoryCommitmentDetected())
        {
            return EnemyArmyMode::HeavyGround;
        }

        if (enemyTerranHeavyMechDetected())
        {
            return EnemyArmyMode::HeavyGround;
        }

        if (heavyGroundDominates)
        {
            return EnemyArmyMode::HeavyGround;
        }

        if (lightGroundDominates)
        {
            return EnemyArmyMode::LightGround;
        }

        return EnemyArmyMode::Default;
    }

    bool enemyModeHasAir(EnemyArmyMode mode)
    {
        return
            mode == EnemyArmyMode::PureAir ||
            mode == EnemyArmyMode::MixedAirHeavyGround ||
            mode == EnemyArmyMode::MixedAirLightGround;
    }

    EnemyArmyMode addRememberedAirToMode(EnemyArmyMode mode)
    {
        static int lastProtossAirFrame = -100000;
        static int lastAnyAirFrame = -100000;
        static int lastObservedFrame = -1;

        const int now = BWAPI::Broodwar->getFrameCount();
        const BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();

        if (lastObservedFrame >= 0 && now < lastObservedFrame)
        {
            lastProtossAirFrame = -100000;
            lastAnyAirFrame = -100000;
        }
        lastObservedFrame = now;

        // Do not refresh the air-memory timer merely because UnitData still
        // remembers an air unit which is currently out of vision. That made a
        // single old Wraith/Scout/Mutalisk keep LightGround converted into a
        // MixedAir mode forever. Carrier tech is intentionally persistent.
        const bool currentAirSeen =
            enemyHasCarrierTech() ||
            visibleDangerousEnemyAirUnits() > 0;

        if (currentAirSeen)
        {
            lastAnyAirFrame = now;

            if (enemyRace == BWAPI::Races::Protoss)
            {
                lastProtossAirFrame = now;
            }
        }

        const bool recentlyAnyAir =
            now - lastAnyAirFrame <= 24 * 75;

        const bool recentlyProtossAir =
            enemyRace == BWAPI::Races::Protoss &&
            now - lastProtossAirFrame <= 24 * 90;

        if (!recentlyAnyAir && !recentlyProtossAir)
        {
            return mode;
        }

        if (enemyRace == BWAPI::Races::Protoss && enemyHasCarrierTech())
        {
            if (mode == EnemyArmyMode::HeavyGround)
            {
                return EnemyArmyMode::MixedAirHeavyGround;
            }

            if (mode == EnemyArmyMode::LightGround)
            {
                return EnemyArmyMode::MixedAirLightGround;
            }

            if (mode == EnemyArmyMode::Default)
            {
                return EnemyArmyMode::PureAir;
            }

            return mode;
        }

        if (recentlyProtossAir)
        {
            if (mode == EnemyArmyMode::HeavyGround)
            {
                return EnemyArmyMode::MixedAirHeavyGround;
            }

            if (mode == EnemyArmyMode::LightGround)
            {
                return EnemyArmyMode::MixedAirLightGround;
            }

            if (mode == EnemyArmyMode::Default)
            {
                // Remembered non-carrier air is not enough evidence to drop
                // all ground production. Keep a mixed composition until a
                // committed air army is observed again.
                return EnemyArmyMode::MixedAirHeavyGround;
            }
        }

        if (recentlyAnyAir)
        {
            if (mode == EnemyArmyMode::HeavyGround)
            {
                return EnemyArmyMode::MixedAirHeavyGround;
            }

            if (mode == EnemyArmyMode::LightGround)
            {
                return EnemyArmyMode::MixedAirLightGround;
            }
        }

        return mode;
    }

    int scienceVesselsCurrentlyTraining()
    {
        int count = 0;

        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists())
            {
                continue;
            }

            if (unit->getType() != BWAPI::UnitTypes::Terran_Starport)
            {
                continue;
            }

            if (!unit->isCompleted() || !unit->isTraining())
            {
                continue;
            }

            const BWAPI::UnitType trainedType =
                unit->getLastCommand().getUnitType();

            if (trainedType == BWAPI::UnitTypes::Terran_Science_Vessel)
            {
                ++count;
            }
        }

        return count;
    }

    bool removeExcessTankScienceVesselFromTop(BuildOrderQueue& queue)
    {
        // This is a tank-opening policy. Other Terran openings must retain
        // their own Science Vessel goals and production limits.
        if (StrategyManager::Instance().getOpeningGroup() != "tanks")
        {
            return false;
        }

        if (queue.isEmpty())
        {
            return false;
        }

        const MacroAct& top = queue.getHighestPriorityItem().macroAct;

        if (!top.isUnit())
        {
            return false;
        }

        if (top.getUnitType() != BWAPI::UnitTypes::Terran_Science_Vessel)
        {
            return false;
        }

        const int maxScienceVessels =
            StrategyManager::Instance().pureAirPlanActive() ? 1 : 2;

        const int ownedVessels =
            the.my.all.count(BWAPI::UnitTypes::Terran_Science_Vessel);

        const int queuedVessels =
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Science_Vessel);

        // the.my.all already includes a unit currently being trained.
        const int plannedVessels =
            ownedVessels + queuedVessels;

        if (plannedVessels > maxScienceVessels)
        {
            queue.removeHighestPriorityItem();

            static int lastPrintFrame = -100000;
            const int now = BWAPI::Broodwar->getFrameCount();

            if (now - lastPrintFrame > 24 * 5)
            {
                lastPrintFrame = now;

                DebugQueuePrintf(
                    "queue: removed extra Science Vessel, planned=%d max=%d",
                    plannedVessels,
                    maxScienceVessels
                );
            }

            return true;
        }

        return false;
    }

    bool removeExcessTankOpeningDefenseFromTop(BuildOrderQueue& queue)
    {
        if (StrategyManager::Instance().getOpeningGroup() != "tanks")
        {
            return false;
        }

        if (queue.isEmpty())
        {
            return false;
        }

        const MacroAct& top = queue.getHighestPriorityItem().macroAct;

        if (!top.isUnit())
        {
            return false;
        }

        const BWAPI::UnitType topType = top.getUnitType();

        if (topType == BWAPI::UnitTypes::Terran_Bunker)
        {
            const int plannedBunkers =
                totalBunkerCountIncludingPlanned(queue);

            if (plannedBunkers > 2)
            {
                queue.removeHighestPriorityItem();

                DebugQueuePrintf(
                    "queue: removed extra Bunker, planned=%d max=2",
                    plannedBunkers
                );

                return true;
            }
        }

        if (topType == BWAPI::UnitTypes::Terran_Barracks)
        {
            const int plannedBarracks =
                totalBarracksCountIncludingPlanned(queue);

            const int barracksLimit = ProductionManager::Instance().terranBuildingQueueLimit(
                BWAPI::UnitTypes::Terran_Barracks, 1);
            if (plannedBarracks > barracksLimit)
            {
                queue.removeHighestPriorityItem();

                DebugQueuePrintf(
                    "queue: removed extra Barracks, planned=%d max=%d",
                    plannedBarracks, barracksLimit
                );

                return true;
            }
        }

        return false;
    }

    bool removeExcessUrgentTurretsFromTop(BuildOrderQueue& queue)
    {
        

        if (queue.isEmpty())
        {
            return false;
        }

        const MacroAct& top = queue.getHighestPriorityItem().macroAct;

        if (!top.isUnit())
        {
            return false;
        }

        if (top.getUnitType() != BWAPI::UnitTypes::Terran_Missile_Turret)
        {
            return false;
        }

        const int plannedTurrets =
            totalTurretCountIncludingPlanned(queue);

        // Enforce the cap where it matters: at the base owning this exact tile.
        // A second/third expansion increases the global allowance, but can no
        // longer authorize 20-30 Turrets around the main mineral line.
        if (top.getMacroLocation() == MacroLocation::Tile &&
            top.getTileLocation().isValid())
        {
            Base* base = nearestOwnedTurretBase(top.getTileLocation());
            const int localTurrets = committedTurretCountForBase(base);
            if (base && localTurrets > MAX_TURRETS_PER_OWNED_BASE)
            {
                queue.removeHighestPriorityItem();
                DebugQueuePrintf(
                    "queue: removed extra base Turret, base=%d local=%d max=%d",
                    base->getID(),
                    localTurrets,
                    MAX_TURRETS_PER_OWNED_BASE);
                return true;
            }
        }

        if (plannedTurrets > strategicTurretGlobalLimit())
        {
            queue.removeHighestPriorityItem();

            DebugQueuePrintf(
                "queue: removed extra Turret, planned=%d distributed-max=%d",
                plannedTurrets,
                strategicTurretGlobalLimit());

            return true;
        }

        return false;
    }

    struct TerranInfantryDefense
    {
        int bunkers = 0;
        int marines = 0;
        bool wallProtected = false;
        bool groundEmergency = false;
    };

    // One contract for JSON emergencies, post-book goals and queue cleanup.
    // A layout is not a finished wall; a released wall is no longer a gate.
    TerranInfantryDefense chooseTerranInfantryDefense(
        bool wallProtected, bool normalBunker, bool groundEmergency,
        bool severeRush, bool mechReady, bool airBridge, int airAttackers)
    {
        TerranInfantryDefense result;
        result.wallProtected = wallProtected;
        result.groundEmergency = groundEmergency;
        result.bunkers = normalBunker ? 1 : 0;
        // Once local mech can hold the entrance, keep surviving infantry but
        // do not restart its production just to refill the opening garrison.
        result.marines = normalBunker && !mechReady ? 4 : 0;
        if (groundEmergency && !wallProtected && !mechReady)
        {
            result.bunkers = severeRush ? 2 : 1;
            result.marines = 4 * result.bunkers;
        }
        if (airBridge) result.marines = std::max(result.marines, airAttackers >= 3 ? 8 : 6);
        return result;
    }

    TerranInfantryDefense terranInfantryDefense()
    {
        const int now = the.now();
        static int cachedFrame = -1;
        static TerranInfantryDefense cached;
        if (cachedFrame == now) return cached;
        cachedFrame = now;
        const bool early = now <= 10 * 60 * 24;
        const bool gateActive = ProductionManager::Instance().terranMainWallGateActive();
        bool wallIntact = gateActive && the.placer.isTerranMainWallComplete(true);
        int wallMech = 0;
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (!unit || !unit->exists()) continue;
            if (the.placer.isTerranMainWallBuilding(unit) &&
                unit->getHitPoints() * 3 < unit->getType().maxHitPoints()) wallIntact = false;
            if (gateActive && unit->isCompleted() && !unit->isLoaded() &&
                unit->getDistance(the.placer.getTerranMainWallGateCenter()) <= 9 * 32)
            {
                const BWAPI::UnitType type = unit->getType();
                if (type == BWAPI::UnitTypes::Terran_Vulture || type == BWAPI::UnitTypes::Terran_Goliath) ++wallMech;
                if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                    type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) wallMech += 2;
            }
        }

        int insideWorkers = 0, airAttackers = 0;
        bool breach = false, localMechReady = true, localAAReady = true;
        bool nearbyGateEnemy = false;
        std::set<BWAPI::Unit> counted;
        for (Base * base : the.bases.getAll())
        {
            if (!base || base->getOwner() != the.self() || !base->getDepot() ||
                !base->getDepot()->exists() || base->getDepot()->isFlying()) continue;
            const bool baseInside = gateActive && the.placer.isInsideTerranMainWall(base->getCenter());
            int baseGround = 0, baseAir = 0;
            for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
            {
                if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                    !enemy->getPosition().isValid() || enemy->getDistance(base->getCenter()) > 14 * 32) continue;
                const BWAPI::UnitType type = enemy->getType();
                if (type.isSpell() || !UnitUtil::CanAttackGround(enemy)) continue;
                if (enemy->isFlying())
                {
                    ++baseAir;
                    if (counted.insert(enemy).second) ++airAttackers;
                    continue;
                }
                const bool exposed = !baseInside ||
                    the.placer.isInsideTerranMainWall(enemy->getPosition(), -16);
                if (gateActive && enemy->getDistance(the.placer.getTerranMainWallGateCenter()) < 6 * 32)
                    nearbyGateEnemy = true;
                if (type.isWorker())
                {
                    // A harmless lone scout outside the gate is not a rush.
                    if (exposed && enemy->getDistance(base->getCenter()) < 10 * 32 &&
                        (enemy->isAttacking() || enemy->isConstructing() || enemy->isRepairing() ||
                         enemy->getDistance(base->getCenter()) < 6 * 32))
                    {
                        ++baseGround;
                        if (counted.insert(enemy).second) ++insideWorkers;
                    }
                }
                else
                {
                    if (exposed) { breach = true; ++baseGround; }
                }
            }
            int mech = 0, aa = 0;
            if (baseGround || baseAir)
                for (BWAPI::Unit unit : the.self()->getUnits())
                {
                    if (!unit || !unit->exists() || !unit->isCompleted() || unit->isLoaded() ||
                        unit->getDistance(base->getCenter()) > 14 * 32) continue;
                    const BWAPI::UnitType type = unit->getType();
                    if (type == BWAPI::UnitTypes::Terran_Vulture) ++mech;
                    if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                        type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) mech += 2;
                    if (type == BWAPI::UnitTypes::Terran_Goliath) { ++mech; ++aa; }
                    if (type == BWAPI::UnitTypes::Terran_Missile_Turret) aa += 2;
                }
            if (baseGround && mech < std::max(4, baseGround)) localMechReady = false;
            if (baseAir && aa < std::max(2, baseAir)) localAAReady = false;
        }
        const OpeningPlan plan = OpponentModel::Instance().getEnemyPlan();
        const OpeningPlan guess = OpponentModel::Instance().getPreparationEnemyPlan();
        const bool historicalRush =
            (the.enemyRace() == BWAPI::Races::Protoss ||
             the.enemyRace() == BWAPI::Races::Zerg) &&
            plan == OpeningPlan::Unknown &&
            OpponentModel::Instance().stronglyPredictsEarlyGroundRush();
        auto rush = [](OpeningPlan p) { return p == OpeningPlan::WorkerRush || p == OpeningPlan::Proxy ||
            p == OpeningPlan::HeavyRush || p == OpeningPlan::FastRush ||
            p == OpeningPlan::VultureRush; };
        const bool workerRush = insideWorkers >= 3 || (insideWorkers > 0 && plan == OpeningPlan::WorkerRush);
        const bool groundEmergency = workerRush || breach ||
            (early && (rush(plan) || rush(guess) || historicalRush ||
                enemyOpeningRushPressure() ||
                actualEarlyGroundRushDetected() || actualEarlyLightGroundRushDetected() ||
                actualEarlyProtossZealotRushDetected()));
        const bool severe = workerRush || breach || plan == OpeningPlan::WorkerRush || guess == OpeningPlan::WorkerRush ||
            plan == OpeningPlan::Proxy || guess == OpeningPlan::Proxy ||
            plan == OpeningPlan::HeavyRush || guess == OpeningPlan::HeavyRush ||
            plan == OpeningPlan::FastRush || guess == OpeningPlan::FastRush ||
            actualEarlyGroundRushDetected() || actualEarlyProtossZealotRushDetected();
        const int completedMech = the.my.completed.count(BWAPI::UnitTypes::Terran_Vulture) +
            2 * (the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
                 the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode));
        const bool mechReady = (breach || insideWorkers > 0) ? localMechReady :
            (gateActive ? wallMech >= 6 : completedMech >= 6);
        const bool protectedByWall = wallIntact && !breach && insideWorkers == 0 &&
            (the.placer.isTerranMainWallComplete() || !nearbyGateEnemy);
        const int bunkersStarted = the.my.all.count(BWAPI::UnitTypes::Terran_Bunker) +
            int(BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Bunker));
        const bool normalBunker = bunkersStarted > 0 ||
            (!StrategyManager::Instance().pureAirPlanActive() &&
             (gateActive || the.my.all.count(BWAPI::UnitTypes::Terran_Command_Center) >= 2));
        TerranInfantryDefense result = chooseTerranInfantryDefense(protectedByWall, normalBunker,
            groundEmergency, severe, mechReady, airAttackers > 0 && !localAAReady, airAttackers);
        const BWAPI::Race race = the.enemyRace();
        if (early && (race == BWAPI::Races::Protoss || race == BWAPI::Races::Zerg) &&
            (plan == OpeningPlan::FastRush || plan == OpeningPlan::HeavyRush ||
             guess == OpeningPlan::FastRush || guess == OpeningPlan::HeavyRush) &&
            bunkersStarted > 0)
        {
            result.marines = std::max(result.marines, 4 * bunkersStarted);
        }
        // One or two attacking SCVs can be chased without buying two Bunkers.
        if (insideWorkers > 0 && !workerRush && !localMechReady)
            result.marines = std::max(result.marines, 3);
        cached = result;
        return result;
    }

    bool removeObsoleteTerranInfantry(BuildOrderQueue & queue)
    {
        if (StrategyManager::Instance().getOpeningGroup() != "tanks" ||
            !ProductionManager::Instance().isOutOfBook()) return false;
        const TerranInfantryDefense defense = terranInfantryDefense();
        int marineTarget = defense.marines;
        if (benzeneClearanceNeeded()) marineTarget = std::max(marineTarget, benzeneClearanceMarineTarget(queue));
        int marines = the.my.all.count(BWAPI::UnitTypes::Terran_Marine);
        int bunkers = the.my.all.count(BWAPI::UnitTypes::Terran_Bunker) +
            int(BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Bunker));
        bool removed = false;
        for (int i = int(queue.size()) - 1; i >= 0; --i)
        {
            const MacroAct & act = queue[size_t(i)].macroAct;
            if (!act.isUnit()) continue;
            const BWAPI::UnitType type = act.getUnitType();
            if ((type == BWAPI::UnitTypes::Terran_Marine && ++marines > marineTarget) ||
                (type == BWAPI::UnitTypes::Terran_Bunker && ++bunkers > defense.bunkers))
            { queue.removeAt(size_t(i)); removed = true; }
        }
        return removed;
    }

    bool removeExcessUrgentMarinesFromTop(BuildOrderQueue& queue)
    {
        if (StrategyManager::Instance().getOpeningGroup() != "tanks")
        {
            return false;
        }

        if (queue.isEmpty())
        {
            return false;
        }

        const MacroAct& top = queue.getHighestPriorityItem().macroAct;

        if (!top.isUnit())
        {
            return false;
        }

        if (top.getUnitType() != BWAPI::UnitTypes::Terran_Marine)
        {
            return false;
        }

        const int plannedMarines =
            totalMarineCountIncludingPlanned(queue);

        const TerranInfantryDefense defense = terranInfantryDefense();
        int maxAllowedMarines = ProductionManager::Instance().isOutOfBook()
            ? defense.marines : std::max(4, defense.marines);
        if (ProductionManager::Instance().isOutOfBook() && benzeneClearanceNeeded())
            maxAllowedMarines = std::max(maxAllowedMarines, benzeneClearanceMarineTarget(queue));

        if (plannedMarines > maxAllowedMarines)
        {
            queue.removeHighestPriorityItem();
            return true;
        }

        return false;
    }


    bool removeCompletedTerranTechFromTop(BuildOrderQueue& queue)
    {
        if (queue.isEmpty())
        {
            return false;
        }

        const MacroAct& top = queue.getHighestPriorityItem().macroAct;

        if (!top.isTech())
        {
            return false;
        }

        const BWAPI::TechType tech = top.getTechType();

        if (tech == BWAPI::TechTypes::Tank_Siege_Mode &&
            (
                BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode) ||
                BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::Tank_Siege_Mode)
                ))
        {
            queue.removeHighestPriorityItem();
            return true;
        }

        if (tech == BWAPI::TechTypes::Spider_Mines &&
            (
                BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Spider_Mines) ||
                BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::Spider_Mines)
                ))
        {
            queue.removeHighestPriorityItem();
            return true;
        }

        if (tech == BWAPI::TechTypes::EMP_Shockwave &&
            (
                BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::EMP_Shockwave) ||
                BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::EMP_Shockwave)
                ))
        {
            queue.removeHighestPriorityItem();
            return true;
        }

        return false;
    }

    EnemyArmyMode stabilizeEnemyArmyMode(EnemyArmyMode instantMode)
    {
        static EnemyArmyMode stableMode = EnemyArmyMode::Default;
        static EnemyArmyMode candidateMode = EnemyArmyMode::Default;
        static int candidateStartFrame = -100000;
        static int lastObservedFrame = -1;

        const int now = BWAPI::Broodwar->getFrameCount();

        // Function statics may survive when a tournament runner reuses the
        // bot DLL. A decreasing frame counter marks a new game.
        if (lastObservedFrame >= 0 && now < lastObservedFrame)
        {
            stableMode = EnemyArmyMode::Default;
            candidateMode = EnemyArmyMode::Default;
            candidateStartFrame = now;
        }
        lastObservedFrame = now;

        auto isAirMode = [](EnemyArmyMode mode)
            {
                return
                    mode == EnemyArmyMode::PureAir ||
                    mode == EnemyArmyMode::MixedAirHeavyGround ||
                    mode == EnemyArmyMode::MixedAirLightGround;
            };
        auto pureAirTransitionAllowed = [](EnemyArmyMode from, EnemyArmyMode to)
            {
                if (to != EnemyArmyMode::PureAir)
                {
                    return true;
                }

                const BWAPI::Race enemyRace =
                    BWAPI::Broodwar->enemy()->getRace();

                if (enemyRace == BWAPI::Races::Zerg)
                {
                    return true;
                }

                if (enemyHasCarrierTech())
                {
                    return true;
                }

                if (from == EnemyArmyMode::MixedAirHeavyGround ||
                    from == EnemyArmyMode::MixedAirLightGround)
                {
                    const int dangerousAir =
                        std::max(
                            countDangerousEnemyAirUnits(),
                            visibleDangerousEnemyAirUnits()
                        );

                    return dangerousAir >= 8;
                }

                return true;
            };

        if (!pureAirTransitionAllowed(stableMode, instantMode))
        {
            return stableMode;
        }

        const BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();
        const bool decisiveHeavyGroundVisible =
            (enemyRace == BWAPI::Races::Zerg &&
             visibleEnemyUltraliskCount() > 0) ||
            (enemyRace == BWAPI::Races::Terran &&
             (visibleEnemyTerranSiegedTankCount() > 0 ||
              visibleEnemyTerranTankCount() >= 2));
        if (decisiveHeavyGroundVisible &&
            (instantMode == EnemyArmyMode::HeavyGround ||
             instantMode == EnemyArmyMode::MixedAirHeavyGround))
        {
            // A visible Ultralisk or Tank core outranks the two-second
            // hysteresis used for inferred air composition changes.
            stableMode = instantMode;
            candidateMode = instantMode;
            candidateStartFrame = now;
            return stableMode;
        }

        if (instantMode == stableMode)
        {
            candidateMode = instantMode;
            candidateStartFrame = now;
            return stableMode;
        }

        if (candidateMode != instantMode)
        {
            candidateMode = instantMode;
            candidateStartFrame = now;
            return stableMode;
        }

        int requiredFrames = 24 * 6;

        if (stableMode == EnemyArmyMode::Default)
        {
            requiredFrames = isAirMode(instantMode) ? 24 * 2 : 24 * 5;
        }
        else if (instantMode == EnemyArmyMode::Default)
        {
            // Missing observations are weak evidence. Wait before returning
            // to the neutral composition.
            requiredFrames = 24 * 30;
        }
        else if (stableMode == EnemyArmyMode::PureAir)
        {
            // PureAir may be left again, but only after consistent evidence.
            requiredFrames = isAirMode(instantMode) ? 24 * 12 : 24 * 20;
        }
        else if (isAirMode(stableMode) && !isAirMode(instantMode))
        {
            requiredFrames = 24 * 20;
        }
        else if (isAirMode(instantMode))
        {
            requiredFrames = 24 * 2;
        }

        if (now - candidateStartFrame >= requiredFrames)
        {
            const EnemyArmyMode previousMode = stableMode;
            stableMode = instantMode;
            candidateStartFrame = now;

            DebugQueuePrintf(
                "Enemy mode stabilized: %s -> %s",
                enemyArmyModeToString(previousMode),
                enemyArmyModeToString(stableMode)
            );
        }

        return stableMode;
    }

    // Army classification walks enemy memory and visible units several times.
    // All strategic consumers share one result for half a second; immediate
    // local-attack checks remain uncached and still react on the same frame.
    EnemyArmyMode currentEnemyArmyMode()
    {
        static EnemyArmyMode cachedMode = EnemyArmyMode::Default;
        static int cachedFrame = -100000;
        static int lastObservedFrame = -1;

        const int now = BWAPI::Broodwar->getFrameCount();

        if (lastObservedFrame > now)
        {
            cachedMode = EnemyArmyMode::Default;
            cachedFrame = -100000;
        }
        lastObservedFrame = now;

        if (now >= cachedFrame + 12)
        {
            cachedMode = stabilizeEnemyArmyMode(
                addRememberedAirToMode(detectEnemyArmyMode()));
            cachedFrame = now;
        }

        return cachedMode;
    }

    // The opponent-specific TerranPlan UCB may select TvP_AntiCarrier before
    // the current game has revealed a Stargate or Carrier. Keep the selected
    // macro plan, but let the JSON opening finish before a prediction commits
    // to the full PureAir composition. Current-game air evidence may still
    // trigger the immediate emergency transition separately.
    //
    bool antiCarrierGroundShellReady()
    {
        const int completedTanks =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);

        return
            completedTanks >= 4 &&
            BWAPI::Broodwar->self()->hasResearched(
                BWAPI::TechTypes::Tank_Siege_Mode);
    }

    // Do not make this an irreversible blind counter. A real attack at our
    // bases always disables the predictive commitment. An old/sticky opening
    // label blocks it only until four completed Siege-capable Tanks exist;
    // otherwise a learned Carrier opponent which opens with a few Dragoons
    // would prevent its own counter plan from ever becoming active.
    bool selectedTvPAntiCarrierPlanCanCommit(EnemyArmyMode enemyMode)
    {
        if (!ProductionManager::Instance().isOutOfBook() ||
            BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Protoss ||
            StrategyManager::Instance().getTerranPlan() !=
                TerranPlanId::TvP_AntiCarrier)
        {
            return false;
        }

        const bool classifiedGroundArmy =
            enemyMode == EnemyArmyMode::HeavyGround ||
            enemyMode == EnemyArmyMode::LightGround ||
            enemyMode == EnemyArmyMode::MixedAirHeavyGround ||
            enemyMode == EnemyArmyMode::MixedAirLightGround;

        const bool groundShellReady = antiCarrierGroundShellReady();
        const bool groundArmyExceedsSafetyShell =
            countEnemyHeavyGroundUnits() >= 8 ||
            countEnemyLightGroundUnits() >= 12;

        const OpeningPlan recognizedPlan =
            OpponentModel::Instance().getEnemyPlan();
        const OpeningPlan likelyPlan =
            OpponentModel::Instance().getPreparationEnemyPlan();

        auto isGroundEmergencyPlan = [](OpeningPlan plan)
        {
            return
                plan == OpeningPlan::Contain ||
                plan == OpeningPlan::Proxy ||
                plan == OpeningPlan::WorkerRush ||
                plan == OpeningPlan::FastRush ||
                plan == OpeningPlan::HeavyRush;
        };

        if (actualEarlyGroundRushDetected() ||
            actualEarlyLightGroundRushDetected() ||
            actualEarlyProtossZealotRushDetected() ||
            visibleEnemyGroundNearMainOrClosestExpansion() ||
            groundArmyExceedsSafetyShell)
        {
            return false;
        }

        if (!groundShellReady &&
            (
                classifiedGroundArmy ||
                isGroundEmergencyPlan(recognizedPlan) ||
                isGroundEmergencyPlan(likelyPlan) ||
                enemyOpeningGroundPressure()
                ))
        {
            return false;
        }

        return true;
    }


    // Without a wall, rush defense starts with a shelter close to the main CC.
    // Extend it to the entrance only after the shelter is ready and the site is safe.
    bool noWallRushBunkerPolicy()
    {
        return the.selfRace() == BWAPI::Races::Terran &&
            the.now() <= 10 * 60 * 24 &&
            (!the.placer.hasTerranMainWall() || the.placer.isTerranMainWallReleased()) &&
            (terranInfantryDefense().groundEmergency ||
             OpponentModel::Instance().hasEarlyGroundRushRisk());
    }

    bool completedMainRushShelter()
    {
        Base * main = the.bases.myMain();
        if (!main) return false;
        for (BWAPI::Unit unit : the.self()->getUnits())
            if (unit && unit->exists() && unit->isCompleted() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Bunker &&
                unit->getDistance(main->getCenter()) <= 7 * 32)
                return true;
        return false;
    }

    bool safeForwardRushBunkerTile(BWAPI::TilePosition tile)
    {
        Base * main = the.bases.myMain();
        if (!main || !tile.isValid() || !completedMainRushShelter()) return false;
        const BWAPI::Position center = BWAPI::Position(tile) + BWAPI::Position(48, 32);
        const BWAPI::Position entrance = getBunkerEntrancePosition(main);
        // A safe tile beside the CC is still the wrong place for bunker #2.
        // Keep the second shelter at the main entrance and clear of bunker #1.
        if (!entrance.isValid() ||
            center.getApproxDistance(main->getCenter()) <= 6 * 32 ||
            center.getApproxDistance(entrance) > 4 * 32)
            return false;
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (unit && unit->exists() && unit->isCompleted() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Bunker &&
                unit->getDistance(main->getCenter()) <= 7 * 32)
            {
                if (unit->getDistance(center) < 3 * 32) return false;
            }
        }
        const BWAPI::UnitType bunker = BWAPI::UnitTypes::Terran_Bunker;
        for (int x = 0; x < bunker.tileWidth(); ++x)
            for (int y = 0; y < bunker.tileHeight(); ++y)
            {
                const BWAPI::TilePosition cell = tile + BWAPI::TilePosition(x, y);
                if (!cell.isValid() || !BWAPI::Broodwar->isVisible(cell) ||
                    the.groundHitsFixed.at(cell) > 0 || the.groundHitsMobile.at(cell) > 0)
                    return false;
            }
        if (main->getGroundSafePaths().isInitialized() && main->getSafeTileDistance(tile) < 0)
            return false;
        for (BWAPI::Unit enemy : the.enemy()->getUnits())
        {
            if (enemy && enemy->exists() && enemy->isVisible() &&
                enemy->getPosition().isValid() && UnitUtil::CanAttackGround(enemy) &&
                enemy->getDistance(center) <= std::max(8 * 32,
                    UnitUtil::GetAttackRangeAssumingUpgrades(
                        enemy->getType(), BWAPI::UnitTypes::Terran_SCV) + 3 * 32))
                return false;
        }
        return true;
    }

    bool safeNaturalRushBunkerTile(BWAPI::TilePosition tile)
    {
        Base * natural = the.bases.myNatural();
        if (!naturalExpansionStarted() || !natural || !tile.isValid()) return false;
        const BWAPI::Position center = BWAPI::Position(tile) + BWAPI::Position(48, 32);
        const BWAPI::Position entrance = getBunkerEntrancePosition(natural);
        if (!entrance.isValid() ||
            center.getApproxDistance(entrance) > 6 * 32 ||
            center.getApproxDistance(natural->getCenter()) < 4 * 32 ||
            natural->getTileDistance(tile) < 0) return false;

        const BWAPI::UnitType bunker = BWAPI::UnitTypes::Terran_Bunker;
        for (int x = 0; x < bunker.tileWidth(); ++x)
            for (int y = 0; y < bunker.tileHeight(); ++y)
            {
                const BWAPI::TilePosition cell = tile + BWAPI::TilePosition(x, y);
                if (!cell.isValid() || !BWAPI::Broodwar->isVisible(cell) ||
                    the.groundHitsFixed.at(cell) > 0 || the.groundHitsMobile.at(cell) > 0)
                    return false;
            }
        for (BWAPI::Unit enemy : the.enemy()->getUnits())
            if (enemy && enemy->exists() && enemy->isVisible() &&
                enemy->getPosition().isValid() && UnitUtil::CanAttackGround(enemy) &&
                enemy->getDistance(center) <= std::max(8 * 32,
                    UnitUtil::GetAttackRangeAssumingUpgrades(
                        enemy->getType(), BWAPI::UnitTypes::Terran_SCV) + 3 * 32))
                return false;
        return true;
    }

    BWAPI::TilePosition findCloseMainRushBunkerTile()
    {
        Base * main = the.bases.myMain();
        if (!main) return BWAPI::TilePositions::Invalid;
        const BWAPI::UnitType bunker = BWAPI::UnitTypes::Terran_Bunker;
        static int lastSearchFrame = -100000;
        static BWAPI::TilePosition cachedTile = BWAPI::TilePositions::Invalid;
        if (the.now() >= lastSearchFrame && the.now() - lastSearchFrame < 12)
            return cachedTile.isValid() && tileIsBuildableFor(cachedTile, bunker)
                ? cachedTile : BWAPI::TilePositions::Invalid;
        lastSearchFrame = the.now();
        cachedTile = BWAPI::TilePositions::Invalid;
        const BWAPI::Position center = main->getCenter();
        const BWAPI::Position entrance = getBunkerEntrancePosition(main);
        if (!entrance.isValid()) return cachedTile;
        double dx = entrance.x - center.x;
        double dy = entrance.y - center.y;
        const double length = std::hypot(dx, dy);
        if (length < 1.0) return cachedTile;
        dx /= length;
        dy /= length;
        const BWAPI::Position wanted(int(center.x + dx * 4.5 * 32),
            int(center.y + dy * 4.5 * 32));
        const BWAPI::TilePosition origin(center);
        std::vector<std::pair<int, BWAPI::TilePosition>> candidates;
        const BWAPI::Unitset minerals = main->getRemainingMinerals();
        for (int x = -9; x <= 9; ++x)
            for (int y = -9; y <= 9; ++y)
            {
                const BWAPI::TilePosition tile = origin + BWAPI::TilePosition(x, y);
                if (!tile.isValid() ||
                    tile.x + bunker.tileWidth() > BWAPI::Broodwar->mapWidth() ||
                    tile.y + bunker.tileHeight() > BWAPI::Broodwar->mapHeight())
                    continue;
                const BWAPI::Position pos = BWAPI::Position(tile) + BWAPI::Position(48, 32);
                const int distance = pos.getApproxDistance(center);
                if (distance < 3 * 32 || distance > 7 * 32) continue;
                // The first shelter must face the main's exit. A legal tile
                // beside the minerals or behind the depot is not a useful
                // substitute when the forward slots are occupied.
                const double forward =
                    double(pos.x - center.x) * dx + double(pos.y - center.y) * dy;
                if (forward < 32.0) continue;
                const int groundDistance = main->getTileDistance(tile);
                if (groundDistance < 0 || groundDistance > 14) continue;
                bool sameFloor = true;
                for (int bx = 0; bx < bunker.tileWidth(); ++bx)
                    for (int by = 0; by < bunker.tileHeight(); ++by)
                        if (GroundHeight(tile + BWAPI::TilePosition(bx, by)) !=
                            GroundHeight(main->getCenterTile())) sameFloor = false;
                if (!sameFloor) continue;
                int mineralDistance = MAX_DISTANCE;
                for (BWAPI::Unit mineral : minerals)
                    if (mineral && mineral->exists())
                        mineralDistance = std::min(mineralDistance, mineral->getDistance(pos));
                if (mineralDistance < 3 * 32) continue;
                // buildingOK/canBuildHere are much more expensive than the
                // directional, distance and terrain filters above.
                if (!tileIsBuildableFor(tile, bunker)) continue;
                int barracksDistance = 8 * 32;
                for (BWAPI::Unit unit : the.self()->getUnits())
                    if (unit && unit->exists() && !unit->isFlying() &&
                        unit->getType() == BWAPI::UnitTypes::Terran_Barracks)
                        barracksDistance = std::min(barracksDistance, unit->getDistance(pos));
                candidates.emplace_back(pos.getApproxDistance(wanted) * 2 + barracksDistance +
                    std::max(0, 5 * 32 - mineralDistance) * 4, tile);
            }
        std::sort(candidates.begin(), candidates.end(),
            [](const std::pair<int, BWAPI::TilePosition> & a,
               const std::pair<int, BWAPI::TilePosition> & b) { return a.first < b.first; });
        for (const auto & candidate : candidates)
            if (the.placer.preservesGroundExit(bunker, candidate.second))
            {
                cachedTile = candidate.second;
                break;
            }
        return cachedTile;
    }

    BWAPI::TilePosition findDefensiveBunkerTile(Base* base, int bunkerIndex)
    {
        // The no-wall rush rule owns only the first two shelters at main.
        // Once the natural CC starts, its separate front still needs a bunker.
        const bool noWallRush = noWallRushBunkerPolicy() &&
            base == the.bases.myMain();
        const bool naturalDuringRush = noWallRushBunkerPolicy() &&
            base == the.bases.myNatural() && naturalExpansionStarted();
        if (noWallRush)
        {
            base = the.bases.myMain();
            bunkerIndex = std::max(bunkerIndex, the.my.all.count(BWAPI::UnitTypes::Terran_Bunker));
            if (bunkerIndex == 0) return findCloseMainRushBunkerTile();
            if (!completedMainRushShelter()) return BWAPI::TilePositions::Invalid;
        }
        if (!base || !base->getCenter().isValid())
        {
            return BWAPI::TilePositions::Invalid;
        }

        if (base == the.bases.myMain() && bunkerIndex == 0 &&
            the.placer.hasTerranMainWall() &&
            !the.placer.isTerranMainWallReleased())
        {
            const BWAPI::TilePosition reservedTile =
                the.placer.getTerranMainWallBunkerTile();
            const BWAPI::TilePosition wallTile = the.placer.getTerranMainWallBunkerLocation();
            static BWAPI::TilePosition blockedWallTile = BWAPI::TilePositions::None;
            static int blockedSinceFrame = -1;
            static int previousFrame = -1;
            const int now = the.now();
            if (now < previousFrame || blockedWallTile != reservedTile)
            {
                blockedWallTile = reservedTile;
                blockedSinceFrame = -1;
            }
            previousFrame = now;
            if (reservedTile.isValid())
            {
                Building reservedBunker(BWAPI::UnitTypes::Terran_Bunker, reservedTile);
                reservedBunker.macroLocation = MacroLocation::Main;
                if (wallTile == reservedTile &&
                    the.placer.buildingOK(reservedBunker, reservedTile))
                {
                    blockedSinceFrame = -1;
                    return reservedTile;
                }
                // A passing unit may temporarily block the planned spot, so
                // allow it a short time to clear. A route conflict or a slot
                // blocked for several seconds must not freeze the build book.
                if (blockedSinceFrame < 0)
                {
                    blockedSinceFrame = now;
                }
                if (the.placer.preservesGroundExit(
                        BWAPI::UnitTypes::Terran_Bunker, reservedTile) &&
                    now - blockedSinceFrame < 3 * 24)
                {
                    return BWAPI::TilePositions::Invalid;
                }
                if (wallTile.isValid() && wallTile != reservedTile &&
                    the.placer.buildingOK(reservedBunker, wallTile))
                {
                    return wallTile;
                }
            }
            else if (wallTile.isValid())
            {
                return wallTile;
            }
            // A wall layout without a legal Bunker slot still needs a
            // defensive Bunker. Continue with the regular entrance search.
        }

        const BWAPI::UnitType bunkerType =
            BWAPI::UnitTypes::Terran_Bunker;

        const BWAPI::Position basePos = base->getCenter();
        const BWAPI::Position entrancePos =
            getBunkerEntrancePosition(base);

        if (!entrancePos.isValid())
        {
            return BWAPI::TilePositions::Invalid;
        }

        double forwardX = double(entrancePos.x - basePos.x);
        double forwardY = double(entrancePos.y - basePos.y);
        const double len = std::sqrt(forwardX * forwardX + forwardY * forwardY);

        if (len < 1.0)
        {
            return BWAPI::TilePositions::Invalid;
        }

        forwardX /= len;
        forwardY /= len;

        double sideX = -forwardY;
        double sideY = forwardX;

        // Do not derive the shoulder axis from the high-level Area graph.
        // The entrance itself came from the real shortest path; its base->exit
        // vector is a much safer local forward direction on asymmetric ramps.

        const BWAPI::TilePosition mineralTile = base->getMineralLineTile();
        const BWAPI::Position mineralLine = mineralTile.isValid()
            ? BWAPI::Position(mineralTile) + BWAPI::Position(16, 16)
            : BWAPI::Positions::Invalid;

        // Use the actual patches. The single mineral-line tile is only an
        // approximation and can point at the wrong side on split mineral lines.
        const BWAPI::Unitset mineralPatches = base->getRemainingMinerals();
        auto nearestMineralDistance = [&](const BWAPI::Position& position)
        {
            int nearest = MAX_DISTANCE;
            for (BWAPI::Unit mineral : mineralPatches)
            {
                if (mineral && mineral->exists() && mineral->getPosition().isValid())
                {
                    nearest = std::min(nearest,
                        position.getApproxDistance(mineral->getPosition()));
                }
            }
            return nearest;
        };

        // Pick the shoulder farther away from worker traffic first.
        int safeSide = 1;
        if (!mineralPatches.empty())
        {
            const BWAPI::Position sideA(
                int(entrancePos.x + sideX * 2.0 * 32),
                int(entrancePos.y + sideY * 2.0 * 32));
            const BWAPI::Position sideB(
                int(entrancePos.x - sideX * 2.0 * 32),
                int(entrancePos.y - sideY * 2.0 * 32));

            safeSide = nearestMineralDistance(sideA) >=
                nearestMineralDistance(sideB) ? 1 : -1;
        }
        else if (mineralLine.isValid())
        {
            const BWAPI::Position sideA(
                int(entrancePos.x + sideX * 2.0 * 32),
                int(entrancePos.y + sideY * 2.0 * 32));
            const BWAPI::Position sideB(
                int(entrancePos.x - sideX * 2.0 * 32),
                int(entrancePos.y - sideY * 2.0 * 32));
            safeSide = sideA.getApproxDistance(mineralLine) >=
                sideB.getApproxDistance(mineralLine) ? 1 : -1;
        }

        const int shoulderSide =
            (bunkerIndex % 2 == 0) ? safeSide : -safeSide;

        // Stay slightly INSIDE the entrance. A bunker on the choke center can
        // block our own tanks; a bunker outside can be surrounded too easily.
        // Scale the shoulder offset with the measured choke width. The center
        // corridor therefore grows on wide entrances instead of using one
        // hard-coded lane width on every map.
        const double insideTiles = bunkerIndex < 2 ? 1.5 : 2.0;

        const BWAPI::TilePosition entranceTile(entrancePos);
        const int entranceRoomWalkTiles = entranceTile.isValid()
            ? std::max(1, int(the.tileRoom.at(entranceTile)))
            : 16;
        const double chokeWidthTiles = std::min(
            8.0, std::max(2.0, double(entranceRoomWalkTiles) / 4.0));

        const double lateralTiles = BunkerPlacementPolicy::shoulderTiles(
            chokeWidthTiles, forwardX, forwardY);

        const BWAPI::Position wantedPosition(
            int(entrancePos.x - forwardX * insideTiles * 32 +
                sideX * shoulderSide * lateralTiles * 32),
            int(entrancePos.y - forwardY * insideTiles * 32 +
                sideY * shoulderSide * lateralTiles * 32));

        const BWAPI::TilePosition centerTile(wantedPosition);
        BWAPI::TilePosition bestTile = BWAPI::TilePositions::Invalid;
        std::vector<std::pair<int, BWAPI::TilePosition>> candidates;

        const int baseGroundHeight = GroundHeight(base->getCenterTile());
        const int searchRadius = 5;

        for (int dx = -searchRadius; dx <= searchRadius; ++dx)
        {
            for (int dy = -searchRadius; dy <= searchRadius; ++dy)
            {
                const BWAPI::TilePosition tile(
                    centerTile.x + dx,
                    centerTile.y + dy);
                if (!tile.isValid() ||
                    tile.x + bunkerType.tileWidth() > BWAPI::Broodwar->mapWidth() ||
                    tile.y + bunkerType.tileHeight() > BWAPI::Broodwar->mapHeight())
                    continue;

                // A defensive bunker belongs on OUR side of the entrance.
                // Reject a buildable lower-floor pocket under a cliff even if
                // it happens to be geometrically close to a bad/diagonal choke.
                bool sameFloor = true;
                for (int bx = 0; bx < bunkerType.tileWidth() && sameFloor; ++bx)
                    for (int by = 0; by < bunkerType.tileHeight(); ++by)
                        if (GroundHeight(tile + BWAPI::TilePosition(bx, by)) != baseGroundHeight)
                        {
                            sameFloor = false;
                            break;
                        }
                if (!sameFloor)
                {
                    continue;
                }

                // A ramp shoulder can have a different advisory Area from
                // the CC. Actual floor and ground connectivity decide safety.

                // Base already owns one complete distance field. Calling
                // MapTools::getGroundTileDistance() here with a different
                // candidate destination for every tile can construct hundreds
                // of full GridDistances maps in the bunker-placement frame.
                const int baseGroundDistance =
                    base->getTileDistance(tile);
                if (baseGroundDistance < 0 || baseGroundDistance > 24)
                {
                    continue;
                }

                const BWAPI::Position tileCenter(
                    tile.x * 32 + bunkerType.tileWidth() * 16,
                    tile.y * 32 + bunkerType.tileHeight() * 16);

                const int distanceFromBase =
                    tileCenter.getApproxDistance(basePos);
                const int distanceToEntrance =
                    tileCenter.getApproxDistance(entrancePos);

                // Bunkers belong near the entrance, not beside the CC and not
                // deep outside the base.
                if (distanceFromBase < 4 * 32 ||
                    distanceToEntrance > 6 * 32)
                {
                    continue;
                }

                const int nearestMineral = nearestMineralDistance(tileCenter);
                if (nearestMineral < 4 * 32)
                {
                    continue;
                }

                int score =
                    tileCenter.getApproxDistance(wantedPosition);

                if (nearestMineral < 6 * 32)
                {
                    score += (6 * 32 - nearestMineral) * 15;
                }

                if (mineralLine.isValid())
                {
                    const int mineralDistance =
                        tileCenter.getApproxDistance(mineralLine);

                    if (mineralDistance < 5 * 32)
                    {
                        score += 7000;
                    }
                    else if (mineralDistance < 7 * 32)
                    {
                        score += 1000;
                    }
                }

                const double forwardDistance =
                    double(tileCenter.x - basePos.x) * forwardX +
                    double(tileCenter.y - basePos.y) * forwardY;

                const double entranceForwardDistance =
                    double(entrancePos.x - basePos.x) * forwardX +
                    double(entrancePos.y - basePos.y) * forwardY;

                // Keep the bunker ahead of the Tank line. Both the compact
                // defensive line and MASS GATHER stage farther inside this
                // choke; a rear-side fallback would leave the Tanks exposed.
                // Allow more lateral search room, but never search deeper into
                // the base or across the entrance.
                const double signedSide =
                    double(tileCenter.x - entrancePos.x) * sideX +
                    double(tileCenter.y - entrancePos.y) * sideY;
                if (!BunkerPlacementPolicy::nearEntrance(
                    forwardDistance - entranceForwardDistance, signedSide,
                    chokeWidthTiles, forwardX, forwardY))
                {
                    continue;
                }

                // Strongly prefer being in the front half of the base. This
                // prevents fallback tiles behind the CC from winning merely
                // because they happen to be buildable.
                if (forwardDistance < entranceForwardDistance * 0.45)
                {
                    score += 3500;
                }

                if (noWallRush && !safeForwardRushBunkerTile(tile)) continue;

                // Prefer the requested shoulder. This makes bunker #1/#2
                // visually stable instead of both drifting to one side.

                if ((shoulderSide > 0 && signedSide < 0.0) ||
                    (shoulderSide < 0 && signedSide > 0.0))
                {
                    // An unobstructed same-side slot is better than putting
                    // bunker #2 on top of the opposite mineral line.
                    score += 800;
                }

                // Defer the full placement validation until the cheap
                // geometry has narrowed the candidate set.
                if (!tileIsBuildableFor(tile, bunkerType)) continue;

                candidates.emplace_back(score, tile);
            }
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const std::pair<int, BWAPI::TilePosition> & a,
               const std::pair<int, BWAPI::TilePosition> & b)
            {
                return a.first < b.first;
            });
        for (const auto & candidate : candidates)
        {
            if (the.placer.preservesGroundExit(bunkerType, candidate.second) &&
                (!naturalDuringRush || safeNaturalRushBunkerTile(candidate.second)))
            {
                bestTile = candidate.second;
                break;
            }
        }

        if (bestTile.isValid())
        {
            DebugQueuePrintf(
                "bunker placement: base=(%d,%d) entrance=(%d,%d) index=%d tile=(%d,%d)",
                base->getTilePosition().x,
                base->getTilePosition().y,
                BWAPI::TilePosition(entrancePos).x,
                BWAPI::TilePosition(entrancePos).y,
                bunkerIndex,
                bestTile.x,
                bestTile.y);
        }

        return bestTile;
    }

    BWAPI::TilePosition findSiegeExpandBunkerTile(int bunkerIndex)
    {
        return findDefensiveBunkerTile(the.bases.myNatural(), bunkerIndex);
    }

    bool naturalExpansionStarted()
    {
        const Base * natural = the.bases.myNatural();
        if (!natural || natural == the.bases.myMain())
        {
            return false;
        }
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (unit && unit->exists() && !unit->isFlying() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Command_Center &&
                unit->getTilePosition() == natural->getTilePosition())
            {
                return true;
            }
        }
        return false;
    }

    bool queueBioTankMainWallBunker(BuildOrderQueue& queue)
    {
        if (the.selfRace() != BWAPI::Races::Terran ||
            StrategyManager::Instance().getOpeningGroup() != "bio" ||
            !the.placer.isTerranMainWallComplete() ||
            the.placer.isTerranMainWallReleased())
        {
            return false;
        }

        const BWAPI::TilePosition wallTile =
            the.placer.getTerranMainWallBunkerTile();
        if (!wallTile.isValid())
        {
            return false;
        }

        const auto nearWallBunker = [&](BWAPI::TilePosition tile)
        {
            // Only a Bunker which actually overlaps the displayed slot fills
            // it. A rear Bunker several tiles away must not suppress this one.
            return tile.isValid() &&
                std::abs(tile.x - wallTile.x) < 3 &&
                std::abs(tile.y - wallTile.y) < 2;
        };

        // The debug WALL BUNKER rectangle is a reservation, not a build
        // request. BioTank has no Bunker in its JSON opening, so use the slot
        // as soon as the wall is actually complete. A later natural Bunker is
        // a separate task and remains eligible when its CC starts.
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (unit && unit->exists() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Bunker &&
                nearWallBunker(unit->getTilePosition()))
            {
                return false;
            }
        }
        for (BWAPI::TilePosition planned :
             BuildingManager::Instance().getUnstartedBuildingTiles(
                 BWAPI::UnitTypes::Terran_Bunker))
        {
            if (nearWallBunker(planned))
            {
                return false;
            }
        }
        for (size_t i = 0; i < queue.size(); ++i)
        {
            const MacroAct act = queue[int(i)].macroAct;
            if (act.isUnit() &&
                act.getUnitType() == BWAPI::UnitTypes::Terran_Bunker &&
                act.getMacroLocation() == MacroLocation::Tile &&
                nearWallBunker(act.getTileLocation()))
            {
                return false;
            }
        }

        if (totalBunkerCountIncludingPlanned(queue) >= 3)
        {
            return false;
        }
        if (the.placer.getTerranMainWallBunkerLocation() != wallTile)
        {
            return false;
        }
        return queueTankOpeningBunkerRequest(
            queue,
            MacroAct(BWAPI::UnitTypes::Terran_Bunker, wallTile),
            totalBunkerCountIncludingPlanned(queue) + 1,
            "BIO MAIN WALL");
    }

    bool queueNaturalExpansionBunker(BuildOrderQueue& queue)
    {
        if (the.selfRace() != BWAPI::Races::Terran ||
            !naturalExpansionStarted())
        {
            return false;
        }

        if (noWallRushBunkerPolicy())
        {
            const int count = totalBunkerCountIncludingPlanned(queue);
            if (count < 2)
            {
                const BWAPI::TilePosition tile =
                    findDefensiveBunkerTile(the.bases.myMain(), count);
                return tile.isValid() && queueTankOpeningBunkerRequest(queue,
                    MacroAct(BWAPI::UnitTypes::Terran_Bunker, tile), 2, "NO WALL RUSH");
            }
        }

        if (totalBunkerCountIncludingPlanned(queue) >= 3) return false;

        Base * natural = the.bases.myNatural();
        const BWAPI::UnitType bunker = BWAPI::UnitTypes::Terran_Bunker;
        const BWAPI::Position entrance = getBunkerEntrancePosition(natural);
        if (!entrance.isValid()) return false;
        const BWAPI::Position naturalCenter = natural->getCenter();
        const double forwardX = double(entrance.x - naturalCenter.x);
        const double forwardY = double(entrance.y - naturalCenter.y);
        const double entranceForward = forwardX * forwardX + forwardY * forwardY;
        if (entranceForward < 1.0) return false;
        const auto nearNaturalFront = [&](BWAPI::TilePosition check)
        {
            if (!check.isValid()) return false;
            const int groundDistance = natural->getTileDistance(check);
            if (groundDistance < 0 || groundDistance > 24 ||
                GroundHeight(check) != GroundHeight(natural->getCenterTile()))
                return false;
            const BWAPI::Position pos(
                check.x * 32 + bunker.tileWidth() * 16,
                check.y * 32 + bunker.tileHeight() * 16);
            const double projected =
                double(pos.x - naturalCenter.x) * forwardX +
                double(pos.y - naturalCenter.y) * forwardY;
            return pos.getApproxDistance(entrance) <= 8 * 32 &&
                projected >= entranceForward - 4.0 * 32.0 * std::sqrt(entranceForward);
        };
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (unit && unit->exists() && unit->getType() == bunker &&
                nearNaturalFront(unit->getTilePosition()))
            {
                return false;
            }
        }
        for (BWAPI::TilePosition planned :
             BuildingManager::Instance().getUnstartedBuildingTiles(bunker))
        {
            if (nearNaturalFront(planned)) return false;
        }
        for (size_t i = 0; i < queue.size(); ++i)
        {
            const MacroAct act = queue[int(i)].macroAct;
            if (act.isUnit() && act.getUnitType() == bunker &&
                ((act.getMacroLocation() == MacroLocation::Tile &&
                  nearNaturalFront(act.getTileLocation())) ||
                 act.getMacroLocation() == MacroLocation::Natural))
            {
                return false;
            }
        }

        BWAPI::TilePosition tile = findDefensiveBunkerTile(natural, 0);
        if (!tile.isValid())
        {
            tile = findDefensiveBunkerTile(natural, 1);
        }
        if (!tile.isValid())
        {
            return false;
        }

        const int wantedBunkers =
            std::min(3, totalBunkerCountIncludingPlanned(queue) + 1);
        if (queueTankOpeningBunkerRequest(
                queue, MacroAct(bunker, tile), wantedBunkers,
                "NATURAL EXPANSION"))
        {
            DebugQueuePrintf("queue: natural Bunker when Command Center starts");
            return true;
        }
        return false;
    }

    bool queueSiegeExpandBunker(BuildOrderQueue& queue)
    {
        if (StrategyManager::Instance().getOpeningGroup() != "tanks")
        {
            return false;
        }

        if (StrategyManager::Instance().pureAirPlanActive())
        {
            return false;
        }

        const bool wallDefense =
            the.placer.isTerranMainWallComplete();

        const bool secondCCActuallyStarted = naturalExpansionStarted();

        // A verified wall gets exactly one Bunker as soon as the wall is
        // complete. Without a wall, keep the old natural-expansion timing.
        if (!wallDefense && !secondCCActuallyStarted)
        {
            return false;
        }

        const int wantedBunkers = 1;
        if (totalBunkerCountIncludingPlanned(queue) >= wantedBunkers)
        {
            return false;
        }

        const int bunkerIndex = totalBunkerCountIncludingPlanned(queue);
        BWAPI::TilePosition bunkerTile = BWAPI::TilePositions::Invalid;

        if (secondCCActuallyStarted)
        {
            bunkerTile = findSiegeExpandBunkerTile(bunkerIndex);
        }
        else if (ProductionManager::Instance().terranMainWallGateActive())
        {
            bunkerTile = the.placer.getTerranMainWallBunkerLocation();
        }
        else
        {
            bunkerTile = findSiegeExpandBunkerTile(bunkerIndex);
        }

        if (!bunkerTile.isValid() && !secondCCActuallyStarted)
        {
            bunkerTile =
                findBuildTileNearBaseExit(BWAPI::UnitTypes::Terran_Bunker);
        }

        if (!bunkerTile.isValid())
        {
            return false;
        }

        const bool queued = queueTankOpeningBunkerRequest(
            queue,
            MacroAct(BWAPI::UnitTypes::Terran_Bunker, bunkerTile),
            wantedBunkers,
            secondCCActuallyStarted ? "SIEGE EXPAND" : "MAIN WALL");

        if (!queued)
        {
            return false;
        }

        DebugQueuePrintf(
            secondCCActuallyStarted
            ? "queue: SIEGE EXPAND bunker protecting natural"
            : "queue: MAIN WALL bunker adjacent to wall");

        return true;
    }

    BWAPI::TilePosition findNaturalDefensiveBunkerTile(int bunkerIndex)
    {
        Base* naturalBase = the.bases.myNatural();
        return findDefensiveBunkerTile(naturalBase, bunkerIndex);
    }

    bool earlyRushBeforeTwoFactories(const BuildOrderQueue& queue)
    {
        if (StrategyManager::Instance().getOpeningGroup() != "tanks")
        {
            return false;
        }

        const OpeningPlan enemyPlan =
            OpponentModel::Instance().getEnemyPlan();

        const OpeningPlan likelyEnemyPlan =
            OpponentModel::Instance().getPreparationEnemyPlan();
        const bool rushDetected =
            enemyPlan == OpeningPlan::HeavyRush ||
            likelyEnemyPlan == OpeningPlan::HeavyRush ||
            enemyPlan == OpeningPlan::FastRush ||
            likelyEnemyPlan == OpeningPlan::FastRush ||
            enemyPlan == OpeningPlan::Proxy ||
            likelyEnemyPlan == OpeningPlan::Proxy ||
            enemyOpeningGroundPressure() ||
            actualEarlyGroundRushDetected() ||
            actualEarlyLightGroundRushDetected() ||
            actualEarlyProtossZealotRushDetected() ||
            visibleEnemyGroundNearMainOrClosestExpansion();

        if (!rushDetected)
        {
            return false;
        }

        const int totalFactories =
            the.my.all.count(BWAPI::UnitTypes::Terran_Factory) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Factory) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Factory);

        return totalFactories < 2;
    }

    bool protossEarlySafetyApplies()
    {
        return
            BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Terran &&
            BWAPI::Broodwar->enemy()->getRace() == BWAPI::Races::Protoss &&
            StrategyManager::Instance().getOpeningGroup() == "tanks";
    }

    bool protossAirTechSeen()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Protoss)
        {
            return false;
        }

        if (enemyHasCarrierTech())
        {
            return true;
        }

        if (the.your.seen.count(BWAPI::UnitTypes::Protoss_Stargate) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Fleet_Beacon) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Carrier) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Scout) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Corsair) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Arbiter) > 0)
        {
            return true;
        }

        for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible())
            {
                continue;
            }

            const BWAPI::UnitType type = enemy->getType();

            if (type == BWAPI::UnitTypes::Protoss_Stargate ||
                type == BWAPI::UnitTypes::Protoss_Fleet_Beacon ||
                type == BWAPI::UnitTypes::Protoss_Carrier ||
                type == BWAPI::UnitTypes::Protoss_Scout ||
                type == BWAPI::UnitTypes::Protoss_Corsair ||
                type == BWAPI::UnitTypes::Protoss_Arbiter)
            {
                return true;
            }
        }

        return false;
    }

    bool protossGroundRushSeen()
    {
        if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Protoss)
        {
            return false;
        }

        const OpeningPlan enemyPlan =
            OpponentModel::Instance().getEnemyPlan();

        const OpeningPlan likelyEnemyPlan =
            OpponentModel::Instance().getPreparationEnemyPlan();

        if (enemyPlan == OpeningPlan::Proxy ||
            likelyEnemyPlan == OpeningPlan::Proxy ||
            enemyPlan == OpeningPlan::FastRush ||
            likelyEnemyPlan == OpeningPlan::FastRush ||
            enemyPlan == OpeningPlan::HeavyRush ||
            likelyEnemyPlan == OpeningPlan::HeavyRush)
        {
            return true;
        }

        if (enemyHasOpeningThreat(OpeningThreat::GatewayPressure) ||
            enemyHasOpeningThreat(OpeningThreat::ZealotPressure) ||
            enemyHasOpeningThreat(OpeningThreat::DragoonPressure))
        {
            return true;
        }

        if (actualEarlyProtossZealotRushDetected() ||
            actualEarlyGroundRushDetected() ||
            actualEarlyLightGroundRushDetected() ||
            actualProtossDragoonPressureNearBaseDetected() ||
            visibleEnemyGroundNearMainOrClosestExpansion())
        {
            return true;
        }

        return false;
    }

    bool hasCompletedLandedBarracks()
    {
        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (!unit || !unit->exists())
            {
                continue;
            }

            if (unit->getType() != BWAPI::UnitTypes::Terran_Barracks)
            {
                continue;
            }

            if (!unit->isCompleted())
            {
                continue;
            }

            if (unit->isFlying())
            {
                continue;
            }

            return true;
        }

        return false;
    }

    bool queueUrgentAntiAirForNearbyAttack(BuildOrderQueue& queue)
    {
        const NearbyEnemyThreats& nearbyThreats =
            visibleEnemyThreatsNearOwnedBases();

        if (!nearbyThreats.hasAir())
        {
            return false;
        }

        const bool bioTank = StrategyManager::Instance().bioTankPlanActive();
        const int marineTarget = bioTank
            ? (nearbyThreats.airOnly() ? 14 : 10)
            : StrategyManager::Instance().getOpeningGroup() == "tanks"
                ? terranInfantryDefense().marines
                : (nearbyThreats.airOnly() ? 8 : 6);
        const int goliathTarget = nearbyThreats.airOnly() ? 6 : 4;

        const int plannedGoliaths =
            the.my.all.count(BWAPI::UnitTypes::Terran_Goliath) +
            immediateQueuedUnitCount(
                queue, BWAPI::UnitTypes::Terran_Goliath, 3);

        const bool hasCompletedFactory =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Factory) > 0;

        const bool hasCompletedArmory =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Armory) > 0;

        if (!bioTank && hasCompletedFactory && hasCompletedArmory &&
            plannedGoliaths < goliathTarget)
        {
            if (ensureUrgentGasIncome(queue, 50, "nearby air Goliath"))
            {
                CombatCommander::Instance().goDefensive();
                return true;
            }
            if (promoteQueuedUnitToTop(queue, BWAPI::UnitTypes::Terran_Goliath))
            {
                CombatCommander::Instance().goDefensive();
                return true;
            }

            if (immediateQueuedUnitCount(
                    queue, BWAPI::UnitTypes::Terran_Goliath, 2) < 2)
            {
                queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_Goliath);
                CombatCommander::Instance().goDefensive();
                return true;
            }
        }

        const int plannedMarines =
            the.my.all.count(BWAPI::UnitTypes::Terran_Marine) +
            immediateQueuedUnitCount(
                queue, BWAPI::UnitTypes::Terran_Marine, 3);

        // A wall does not stop air. Use a small Marine bridge only while the
        // local Goliath/turret layer cannot cover the current attack.
        if (plannedMarines < marineTarget &&
            !ProductionManager::Instance().isOutOfBook())
        {
            ProductionManager::Instance().requestOpeningDefensiveMarines(marineTarget);
            CombatCommander::Instance().goDefensive();
        }
        else if (plannedMarines < marineTarget)
        {
            if (!hasCompletedLandedBarracks())
            {
                if (requestExistingBarracksLandForMarines())
                {
                    CombatCommander::Instance().goDefensive();
                    return true;
                }
            }
            else
            {
                if (promoteQueuedUnitToTop(queue, BWAPI::UnitTypes::Terran_Marine))
                {
                    CombatCommander::Instance().goDefensive();
                    return true;
                }

                if (immediateQueuedUnitCount(
                        queue, BWAPI::UnitTypes::Terran_Marine, 2) < 2)
                {
                    queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_Marine);
                    CombatCommander::Instance().goDefensive();
                    return true;
                }
            }
        }

        if (!bioTank && hasCompletedFactory && !hasCompletedArmory)
        {
            if (ensureUrgentGasIncome(queue, 100, "nearby air Armory"))
            {
                CombatCommander::Instance().goDefensive();
                return true;
            }

            const bool armoryCommitted =
                buildingExistsOrIsCommitted(
                    BWAPI::UnitTypes::Terran_Armory, queue, 2);

            if (!armoryCommitted)
            {
                if (promoteQueuedUnitToTop(queue, BWAPI::UnitTypes::Terran_Armory))
                {
                    CombatCommander::Instance().goDefensive();
                    return true;
                }

                queue.queueAsHighestPriority(
                    MacroAct(BWAPI::UnitTypes::Terran_Armory, MacroLocation::Main));
                CombatCommander::Instance().goDefensive();
                return true;
            }
        }

        return false;
    }

    // A modest historical rush chance justifies one insurance Bunker when
    // there is no wall. Do not turn this weak signal into the full rush plan.
    bool queueHistoricalNoWallBunkerAndMarines(BuildOrderQueue& queue)
    {
        const bool predictedVultureRush =
            the.enemyRace() == BWAPI::Races::Terran &&
            OpponentModel::Instance().stronglyPredictsEnemyPlan(
                OpeningPlan::VultureRush);
        if (the.selfRace() != BWAPI::Races::Terran ||
            (the.enemyRace() != BWAPI::Races::Protoss &&
             the.enemyRace() != BWAPI::Races::Zerg &&
             !predictedVultureRush) ||
            (the.placer.hasTerranMainWall() &&
             !the.placer.isTerranMainWallReleased()) ||
            the.now() > 7 * 60 * 24 ||
            (!OpponentModel::Instance().hasEarlyGroundRushRisk() &&
             !predictedVultureRush) ||
            !hasCompletedLandedBarracks() ||
            currentEnemyArmyMode() == EnemyArmyMode::PureAir ||
            visibleEnemyAirOnlyNearOwnedBase())
        {
            return false;
        }

        const OpeningPlan observed = OpponentModel::Instance().getEnemyPlan();
        if (observed == OpeningPlan::Turtle ||
            observed == OpeningPlan::SafeExpand)
        {
            return false;
        }

        const BWAPI::UnitType bunker = BWAPI::UnitTypes::Terran_Bunker;
        const int bunkersStarted =
            the.my.all.count(bunker) +
            int(BuildingManager::Instance().getNumUnstarted(bunker));
        if (bunkersStarted == 0)
        {
            if (!queue.isEmpty() &&
                queue.getHighestPriorityItem().macroAct.isUnit() &&
                queue.getHighestPriorityItem().macroAct.getUnitType() == bunker)
            {
                return false;
            }
            if (promoteQueuedUnitToTop(queue, bunker))
            {
                return true;
            }
            if (immediateQueuedUnitCount(queue, bunker, 3) > 0)
            {
                return false;
            }

            const bool naturalFrontActive = naturalExpansionStarted() ||
                the.bases.completedBaseCount(the.self()) > 1;
            BWAPI::TilePosition tile = naturalFrontActive
                ? findNaturalDefensiveBunkerTile(0)
                : getMainOnlyBunkerTile(0);
            if (!tile.isValid() && !naturalFrontActive)
            {
                tile = findBuildTileNearBaseExit(bunker);
            }
            if (!tile.isValid()) return false;
            queue.queueAsHighestPriority(MacroAct(bunker, tile));
            return true;
        }

        const BWAPI::UnitType marine = BWAPI::UnitTypes::Terran_Marine;
        const int marineTarget = predictedVultureRush ? 4 : 2;
        if (!ProductionManager::Instance().isOutOfBook())
        {
            ProductionManager::Instance().requestOpeningDefensiveMarines(marineTarget);
            return false;
        }
        if (the.my.all.count(marine) < marineTarget && !queue.isEmpty() &&
            !(queue.getHighestPriorityItem().macroAct.isUnit() &&
              queue.getHighestPriorityItem().macroAct.getUnitType() == marine) &&
            promoteQueuedUnitToTop(queue, marine))
        {
            return true;
        }
        if (the.my.all.count(marine) +
            immediateQueuedUnitCount(queue, marine, 3) < marineTarget)
        {
            if (promoteQueuedUnitToTop(queue, marine))
            {
                return true;
            }
            queue.queueAsHighestPriority(marine);
            return true;
        }
        return false;
    }

    bool queueEarlyRushBunkerAndMarines(BuildOrderQueue& queue)
    {
        

        // A local air-only attack is handled by the dedicated AA emergency.
        if (visibleEnemyAirOnlyNearOwnedBase())
        {
            return false;
        }

        const int minute = BWAPI::Broodwar->getFrameCount() / (24 * 60);
        const bool earlyDefenseWindow = minute <= 10;

        const EnemyArmyMode enemyMode =
            currentEnemyArmyMode();

        if (enemyMode == EnemyArmyMode::PureAir || enemyHasCarrierTech())
        {
            return false;
        }

        const bool enemyGroundNearBase =
            visibleEnemyGroundNearMainOrClosestExpansion();

        const OpeningPlan enemyPlan = OpponentModel::Instance().getEnemyPlan();
        const OpeningPlan likelyEnemyPlan =
            OpponentModel::Instance().getPreparationEnemyPlan();
        const bool historicalRush =
            (the.enemyRace() == BWAPI::Races::Protoss ||
             the.enemyRace() == BWAPI::Races::Zerg) &&
            enemyPlan == OpeningPlan::Unknown &&
            OpponentModel::Instance().stronglyPredictsEarlyGroundRush();

        const bool enemyRushPlan =
            enemyPlan == OpeningPlan::WorkerRush ||
            likelyEnemyPlan == OpeningPlan::WorkerRush ||
            enemyPlan == OpeningPlan::HeavyRush ||
            likelyEnemyPlan == OpeningPlan::HeavyRush ||
            enemyPlan == OpeningPlan::FastRush ||
            likelyEnemyPlan == OpeningPlan::FastRush ||
            enemyPlan == OpeningPlan::VultureRush ||
            likelyEnemyPlan == OpeningPlan::VultureRush ||
            enemyPlan == OpeningPlan::Proxy ||
            likelyEnemyPlan == OpeningPlan::Proxy ||
            enemyOpeningGroundPressure() || historicalRush;

        const bool confirmedRushPlan =
            enemyPlan == OpeningPlan::WorkerRush ||
            enemyPlan == OpeningPlan::HeavyRush ||
            enemyPlan == OpeningPlan::FastRush ||
            enemyPlan == OpeningPlan::VultureRush ||
            enemyPlan == OpeningPlan::Proxy ||
            enemyOpeningRushPressure();

        // The two-Factory phase is for a committed ground-unit rush, not a lone
        // worker scout/proxy building. A directly observed early combat rush also
        // qualifies even if OppPlan has not caught up yet.
        const bool heavyRushFactoryPlan =
            enemyPlan == OpeningPlan::HeavyRush ||
            likelyEnemyPlan == OpeningPlan::HeavyRush ||
            actualEarlyGroundRushDetected() ||
            actualEarlyProtossZealotRushDetected();

        const bool groundRushDetected =
            terranInfantryDefense().groundEmergency || confirmedRushPlan ||
            historicalRush ||
            actualEarlyGroundRushDetected() ||
            actualEarlyLightGroundRushDetected() ||
            actualEarlyProtossZealotRushDetected() ||
            (enemyGroundNearBase &&
                (enemyRushPlan ||
                enemyMode == EnemyArmyMode::HeavyGround ||
                enemyMode == EnemyArmyMode::LightGround ||
                enemyMode == EnemyArmyMode::MixedAirHeavyGround ||
                enemyMode == EnemyArmyMode::MixedAirLightGround));

        const TerranInfantryDefense defense = terranInfantryDefense();
        const int bunkersStarted =
            the.my.all.count(BWAPI::UnitTypes::Terran_Bunker) +
            int(BuildingManager::Instance().getNumUnstarted(
                BWAPI::UnitTypes::Terran_Bunker));
        if (groundRushDetected && StrategyManager::Instance().getOpeningGroup() == "tanks" &&
            defense.wallProtected &&
            bunkersStarted >= defense.bunkers &&
            the.my.all.count(BWAPI::UnitTypes::Terran_Marine) >= defense.marines)
        {
            // The completed wall already covers the opening. Keeping the
            // entire army defensive here conflicts with the normal attack
            // decision after the early rush window has passed.
            return false;
        }

        if (!groundRushDetected)
        {
            return false;
        }

        if (!earlyDefenseWindow && !heavyRushFactoryPlan)
        {
            return false;
        }

        const bool mechOpening = StrategyManager::Instance().getOpeningGroup() == "tanks";
        const int BUNKER_TARGET = mechOpening ? defense.bunkers : 2;
        const int MARINE_TARGET = mechOpening ? defense.marines : 8;
        const int VULTURE_TARGET =
            enemyHasOpeningThreat(OpeningThreat::DragoonPressure) ? 2 : 5;

        if (earlyDefenseWindow)
        {
            const int immediateBunkers =
                the.my.all.count(BWAPI::UnitTypes::Terran_Bunker) +
                (int)BuildingManager::Instance().getNumUnstarted(
                    BWAPI::UnitTypes::Terran_Bunker) +
                immediateQueuedUnitCount(
                    queue, BWAPI::UnitTypes::Terran_Bunker, 3);

            // Do not queue bunker #2 while the close shelter is still being
            // built. Marines and the rest of the opening can continue meanwhile.
            const bool waitingForMainRushShelter =
                noWallRushBunkerPolicy() &&
                the.my.all.count(BWAPI::UnitTypes::Terran_Bunker) +
                    (int)BuildingManager::Instance().getNumUnstarted(
                        BWAPI::UnitTypes::Terran_Bunker) > 0 &&
                !completedMainRushShelter();
            if (immediateBunkers < BUNKER_TARGET && !waitingForMainRushShelter)
            {
                if (waitForUrgentTerranPrerequisite(
                        queue,
                        MacroAct(BWAPI::UnitTypes::Terran_Bunker),
                        "early rush Bunker"))
                {
                    CombatCommander::Instance().goDefensive();
                    return true;
                }

                if (promoteQueuedUnitToTop(queue, BWAPI::UnitTypes::Terran_Bunker))
                {
                    CombatCommander::Instance().goDefensive();
                    return true;
                }

                BWAPI::TilePosition bunkerTile = BWAPI::TilePositions::Invalid;
                const bool naturalFrontActive = naturalExpansionStarted() ||
                    the.bases.completedBaseCount(the.self()) > 1;
                const int existingBunkers =
                    the.my.all.count(BWAPI::UnitTypes::Terran_Bunker) +
                    (int)BuildingManager::Instance().getNumUnstarted(
                        BWAPI::UnitTypes::Terran_Bunker);

                if (!naturalFrontActive)
                {
                    bunkerTile = getMainOnlyBunkerTile(existingBunkers);
                }
                else
                {
                    bunkerTile = findNaturalDefensiveBunkerTile(existingBunkers);
                }

                if (!bunkerTile.isValid() && !naturalFrontActive &&
                    !(existingBunkers == 0 &&
                      the.placer.hasTerranMainWall() &&
                      !the.placer.isTerranMainWallReleased() &&
                      the.placer.getTerranMainWallBunkerTile().isValid()))
                {
                    bunkerTile = findBuildTileNearBaseExit(
                        BWAPI::UnitTypes::Terran_Bunker);
                }

                // We already verified that there is no immediately executable
                // copy. Queue directly so a bunker buried deep in JSON cannot
                // suppress the emergency request.
                if (bunkerTile.isValid())
                {
                    queue.queueAsHighestPriority(MacroAct(
                        BWAPI::UnitTypes::Terran_Bunker, bunkerTile));
                    CombatCommander::Instance().goDefensive();
                    return true;
                }
            }

            const int immediateMarines =
                the.my.all.count(BWAPI::UnitTypes::Terran_Marine) +
                immediateQueuedUnitCount(
                    queue, BWAPI::UnitTypes::Terran_Marine, 3);

            if (immediateMarines < MARINE_TARGET &&
                !ProductionManager::Instance().isOutOfBook())
            {
                ProductionManager::Instance().requestOpeningDefensiveMarines(MARINE_TARGET);
                CombatCommander::Instance().goDefensive();
            }
            else if (immediateMarines < MARINE_TARGET)
            {
                if (!hasCompletedLandedBarracks())
                {
                    if (requestExistingBarracksLandForMarines())
                    {
                        CombatCommander::Instance().goDefensive();
                        return true;
                    }
                }
                else
                {
                    if (promoteQueuedUnitToTop(queue, BWAPI::UnitTypes::Terran_Marine))
                    {
                        CombatCommander::Instance().goDefensive();
                        return true;
                    }

                    if (immediateQueuedUnitCount(
                            queue, BWAPI::UnitTypes::Terran_Marine, 2) < 2)
                    {
                        queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_Marine);
                        CombatCommander::Instance().goDefensive();
                        return true;
                    }
                }
            }
        }

        // BioTank answers a ground rush with its Marines, Bunkers and Siege
        // Tanks. The mech opening's five-Vulture emergency stage previously
        // ran here too, stealing both Factory slots from the BioTank core.
        if (!mechOpening)
        {
            return false;
        }

        const int factoriesStarted =
            the.my.all.count(BWAPI::UnitTypes::Terran_Factory) +
            (int)BuildingManager::Instance().getNumUnstarted(
                BWAPI::UnitTypes::Terran_Factory);

        // HeavyRush phase 1: two Factories must actually leave the deep JSON
        // queue before Factory time can be spent on Tanks or Vultures.
        if (heavyRushFactoryPlan && factoriesStarted < 2)
        {
            // Factory costs gas. If the opening never started gas (or JSON
            // stopped it), promoting Factory to the top without a Refinery
            // creates a permanent SAVE 200M/100G wait. Own the gas prerequisite
            // before touching the Factory queue.
            if (ensureUrgentGasIncome(queue, 100, "HeavyRush Factory"))
            {
                CombatCommander::Instance().goDefensive();
                return true;
            }
            while (removeOneQueuedUnitType(
                queue, BWAPI::UnitTypes::Terran_Vulture))
            {
            }
            while (removeOneQueuedUnitType(
                queue, BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode))
            {
            }

            requireUrgentBuildingNow(
                queue,
                BWAPI::UnitTypes::Terran_Factory,
                2,
                MacroLocation::Main);

            ProductionManager::Instance().invalidateTerranRulePlan(
                "HeavyRush: establish two Factories before Factory units");
            CombatCommander::Instance().goDefensive();
            return true;
        }

        const int immediateVultures =
            the.my.all.count(BWAPI::UnitTypes::Terran_Vulture) +
            immediateQueuedUnitCount(
                queue, BWAPI::UnitTypes::Terran_Vulture, 3);

        if (factoriesStarted <= 0 || immediateVultures >= VULTURE_TARGET)
        {
            return false;
        }

        if (heavyRushFactoryPlan)
        {
            int removedQueuedTanks = 0;
            while (removeOneQueuedUnitType(
                queue, BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode))
            {
                ++removedQueuedTanks;
            }

            if (removedQueuedTanks > 0)
            {
                ProductionManager::Instance().invalidateTerranRulePlan(
                    "HeavyRush: Vultures have Factory priority over Tanks");
                DebugQueuePrintf(
                    "queue: HeavyRush removed %d stale Tank jobs before Vultures",
                    removedQueuedTanks);
            }
        }

        if (requireUrgentCombatUnitNow(
                queue,
                BWAPI::UnitTypes::Terran_Vulture,
                VULTURE_TARGET,
                2))
        {
            CombatCommander::Instance().goDefensive();
            return true;
        }

        return false;
    }


    bool queueForwardBunkerForGroundRush(
        BuildOrderQueue& queue,
        EnemyArmyMode enemyMode,
        bool enemyGroundNearBase,
        bool enemyRushPlan,
        bool anyWorkers)
    {
        const TerranInfantryDefense defense = terranInfantryDefense();
        if (StrategyManager::Instance().getOpeningGroup() == "tanks" &&
            defense.wallProtected)
        {
            // One wall Bunker is enough here. Avoid a second urgent Bunker and
            // the extra Marine stream that would compete with Tank production.
            // Wall coverage is not a reason to recall the attacking army.
            return false;
        }

        const bool groundRushMode =
            enemyMode == EnemyArmyMode::LightGround ||
            enemyMode == EnemyArmyMode::HeavyGround ||
            enemyMode == EnemyArmyMode::MixedAirLightGround ||
            enemyMode == EnemyArmyMode::MixedAirHeavyGround;

        if (!groundRushMode && !enemyRushPlan)
        {
            return false;
        }

        if (!enemyGroundNearBase &&
            !enemyRushPlan &&
            !actualEarlyGroundRushDetected() &&
            !actualEarlyLightGroundRushDetected() &&
            !actualEarlyProtossZealotRushDetected())
        {
            return false;
        }

        if (!anyWorkers)
        {
            return false;
        }

        const int minute = BWAPI::Broodwar->getFrameCount() / (24 * 60);

        if (minute > 10)
        {
            return false;
        }

        const bool protossZealotRush =
            actualEarlyProtossZealotRushDetected();

        const bool oneGateCoreOpening =
            enemyHasOpeningThreat(OpeningThreat::ProtossOneGateCore) &&
            !enemyHasOpeningThreat(OpeningThreat::GatewayPressure) &&
            !enemyHasOpeningThreat(OpeningThreat::ProtossFourGateDragoon);

        const bool strongEarlyGroundRush =
            actualEarlyGroundRushDetected() ||
            actualEarlyLightGroundRushDetected() ||
            protossZealotRush ||
            enemyHasOpeningThreat(OpeningThreat::ProtossTwoGateZealot) ||
            enemyHasOpeningThreat(OpeningThreat::ProtossFourGateDragoon) ||
            enemyHasOpeningThreat(OpeningThreat::TerranTwoRax);

        const int marineTargetForRush =
            StrategyManager::Instance().getOpeningGroup() == "tanks" ? defense.marines :
            protossZealotRush ? 6 :
            strongEarlyGroundRush ? 6 :
            oneGateCoreOpening ? 4 :
            1;

        const int currentMarines =
            the.my.all.count(BWAPI::UnitTypes::Terran_Marine);

        if (currentMarines < marineTargetForRush)
        {
            if (!ProductionManager::Instance().isOutOfBook())
            {
                ProductionManager::Instance().requestOpeningDefensiveMarines(marineTargetForRush);
            }
            else if ((strongEarlyGroundRush || oneGateCoreOpening) &&
                hasCompletedLandedBarracks() &&
                !queue.anyInQueue(BWAPI::UnitTypes::Terran_Marine))
            {
                const int marinesToQueue =
                    std::min(2, marineTargetForRush - currentMarines);

                for (int i = 0; i < marinesToQueue; ++i)
                {
                    queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_Marine);
                }

                DebugQueuePrintf(
                    "queue: opening defense, marines %d/%d",
                    currentMarines,
                    marineTargetForRush
                );

                return true;
            }

            // One Marine is enough to make the first bunker useful. Continue
            // with bunker placement while the remaining defenders train,
            // instead of waiting for the entire Marine target to complete.
            if (currentMarines <= 0)
            {
                return false;
            }
        }

        if (!requestExistingBarracksLandForMarines())
        {
            return false;
        }

        const int maxEmergencyBunkers =
            noWallRushBunkerPolicy() && enemyRushPlan ? 2 :
            StrategyManager::Instance().getOpeningGroup() == "tanks" ? defense.bunkers :
            strongEarlyGroundRush ? 2 : 1;

        const int currentOrPlannedBunkers =
            totalBunkerCountIncludingPlanned(queue);

        const int cappedMaxEmergencyBunkers =
            std::min(maxEmergencyBunkers, 2);

        if (currentOrPlannedBunkers >= cappedMaxEmergencyBunkers ||
            tankOpeningBunkerRequestLimitReached(queue, cappedMaxEmergencyBunkers))
        {
            return false;
        }

        BWAPI::TilePosition bunkerTile = BWAPI::TilePositions::Invalid;

        const int completedBases =
            the.bases.completedBaseCount(the.self());

        const bool onlyMainBase =
            completedBases <= 1 && !naturalExpansionStarted();

        if (onlyMainBase)
        {
            bunkerTile = getMainOnlyBunkerTile(currentOrPlannedBunkers);

            DebugQueuePrintf(
                "queue: main-only defensive bunker"
            );
        }
        else
        {
            bunkerTile = findNaturalDefensiveBunkerTile(currentOrPlannedBunkers);
        }

        if (!bunkerTile.isValid() && onlyMainBase &&
            !(currentOrPlannedBunkers == 0 &&
              the.placer.hasTerranMainWall() &&
              !the.placer.isTerranMainWallReleased() &&
              the.placer.getTerranMainWallBunkerTile().isValid()))
        {
            bunkerTile = findBuildTileNearBaseExit(BWAPI::UnitTypes::Terran_Bunker);
        }

        if (bunkerTile.isValid())
        {
            queueTankOpeningBunkerRequest(
                queue,
                MacroAct(BWAPI::UnitTypes::Terran_Bunker, bunkerTile),
                cappedMaxEmergencyBunkers,
                "ground rush"
            );

            DebugQueuePrintf(
                "queue: ground rush bunker at defensive front"
            );

            return true;
        }

        return false;
    }


    BWAPI::Position getMainBaseExitApproxPosition()
    {
        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();

        if (mainBase && mainBase->getCenter().isValid())
        {
            const BWAPI::Position entrance =
                getBaseEntranceApproxPosition(mainBase);
            if (entrance.isValid())
            {
                return entrance;
            }

            if (naturalBase && naturalBase->getCenter().isValid())
            {
                const BWAPI::Position routeExit =
                    the.map.getGroundPathWaypoint(
                        mainBase->getCenter(), naturalBase->getCenter(), 8);
                if (routeExit.isValid())
                {
                    return routeExit;
                }
            }

            return mainBase->getFront();
        }

        return BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());
    }

    bool tileIsBuildableFor(const BWAPI::TilePosition& tile, BWAPI::UnitType type)
    {
        if (!tile.isValid())
        {
            return false;
        }

        for (int x = 0; x < type.tileWidth(); ++x)
        {
            for (int y = 0; y < type.tileHeight(); ++y)
            {
                BWAPI::TilePosition checkTile(tile.x + x, tile.y + y);

                if (!checkTile.isValid())
                {
                    return false;
                }

                if (!BWAPI::Broodwar->isBuildable(checkTile, true))
                {
                    return false;
                }

                // Respect BuildingPlacer's resource/building/movement-lane
                // reservations. Defensive special placement used to bypass
                // this layer and could put a bunker directly on the same exit
                // corridor which normal production placement was preserving.
                if (the.placer.isReserved(checkTile))
                {
                    return false;
                }
            }
        }

        if (!BWAPI::Broodwar->canBuildHere(tile, type, nullptr, false))
        {
            return false;
        }

        // Bunker candidates must pass the same base-footprint and movement-lane
        // checks as BuildingManager. A tile that only passed BWAPI's footprint
        // check could be queued forever while BuildingManager rejected it.
        if (type == BWAPI::UnitTypes::Terran_Bunker)
        {
            Building bunker(type, tile);
            bunker.macroLocation = MacroLocation::Tile;
            bunker.finalPosition = tile;
            return the.placer.buildingOK(bunker, tile);
        }

        return true;
    }

    BWAPI::TilePosition findBuildTileNearBaseExit(BWAPI::UnitType type)
    {
        // A failed close shoulder is not permission to build a rear Bunker.
        // Every emergency/fallback request uses the same strict front search.
        if (type == BWAPI::UnitTypes::Terran_Bunker)
            return findDefensiveBunkerTile(the.bases.myMain(),
                the.my.all.count(BWAPI::UnitTypes::Terran_Bunker));
        const BWAPI::Position exitPos = getMainBaseExitApproxPosition();

        if (!exitPos.isValid())
        {
            return BWAPI::TilePositions::Invalid;
        }

        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();

        const BWAPI::TilePosition centerTile(exitPos);

        BWAPI::TilePosition bestTile = BWAPI::TilePositions::Invalid;
        int bestScore = MAX_DISTANCE;

        const int searchRadius = 12;

        for (int dx = -searchRadius; dx <= searchRadius; ++dx)
        {
            for (int dy = -searchRadius; dy <= searchRadius; ++dy)
            {
                BWAPI::TilePosition tile(centerTile.x + dx, centerTile.y + dy);

                if (!tileIsBuildableFor(tile, type))
                {
                    continue;
                }

                BWAPI::Position pos(
                    tile.x * 32 + type.tileWidth() * 16,
                    tile.y * 32 + type.tileHeight() * 16
                );

                int score = pos.getApproxDistance(exitPos);

                if (mainBase && mainBase->getPosition().isValid())
                {
                    const int distMain = pos.getApproxDistance(mainBase->getPosition());

                    if (distMain < 8 * 32)
                    {
                        score += 5000;
                    }
                }
                if (naturalBase && naturalBase->getPosition().isValid())
                {
                    const int distNatural = pos.getApproxDistance(naturalBase->getPosition());
                    score += distNatural / 4;
                }

                score += std::abs(dx) * 8 + std::abs(dy) * 8;

                if (score < bestScore &&
                    (type != BWAPI::UnitTypes::Terran_Bunker ||
                     the.placer.preservesGroundExit(type, tile)))
                {
                    bestScore = score;
                    bestTile = tile;
                }
            }
        }

        return bestTile;
    }

    void addUniqueTurretTile(std::vector<BWAPI::TilePosition>& tiles, const BWAPI::TilePosition& tile)
    {
        if (!tile.isValid())
        {
            return;
        }

        for (const BWAPI::TilePosition& existing : tiles)
        {
            if (existing == tile)
            {
                return;
            }
        }

        tiles.push_back(tile);
    }

    BWAPI::Position positionToward(const BWAPI::Position& from, const BWAPI::Position& to, int distance)
    {
        const double dx = double(to.x - from.x);
        const double dy = double(to.y - from.y);
        const double len = std::sqrt(dx * dx + dy * dy);

        if (len < 1.0)
        {
            return from;
        }

        return BWAPI::Position(
            from.x + int(dx / len * distance),
            from.y + int(dy / len * distance)
        );
    }

    Base* getNextForwardBaseFromNaturalForTurrets()
    {
        Base* naturalBase = the.bases.myNatural();

        if (!naturalBase)
        {
            return nullptr;
        }

        Base* bestBase = nullptr;
        int bestDist = MAX_DISTANCE;

        for (Base* base : the.bases.getAll())
        {
            if (!base || base == the.bases.myMain() || base == naturalBase)
            {
                continue;
            }

            if (!the.bases.connectedToStart(base->getTilePosition()))
            {
                continue;
            }

            int d = naturalBase->getDistance(base->getPosition());

            if (d < bestDist)
            {
                bestDist = d;
                bestBase = base;
            }
        }

        return bestBase;
    }

    BWAPI::Position getTankHoldPositionForTurrets()
    {
        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();

        const bool naturalStarted =
            naturalBase &&
            naturalBase->getOwner() == BWAPI::Broodwar->self() &&
            naturalBase->getDepot();

        if (naturalStarted)
        {
            Base* nextBase = getNextForwardBaseFromNaturalForTurrets();

            if (nextBase)
            {
                BWAPI::Position naturalPos = naturalBase->getPosition();
                BWAPI::Position nextPos = nextBase->getPosition();

                return BWAPI::Position(
                    (naturalPos.x + nextPos.x) / 2,
                    (naturalPos.y + nextPos.y) / 2
                );
            }
        }

        if (mainBase && naturalBase)
        {
            BWAPI::Position mainPos = mainBase->getPosition();
            BWAPI::Position naturalPos = naturalBase->getPosition();

            return BWAPI::Position(
                (mainPos.x + naturalPos.x) / 2,
                (mainPos.y + naturalPos.y) / 2
            );
        }

        if (the.bases.myStart())
        {
            return the.bases.myStart()->getCenter();
        }

        return BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());
    }

    BWAPI::TilePosition tankLineTurretTile()
    {
        Base* mainBase = the.bases.myMain();
        Base* naturalBase = the.bases.myNatural();

        Base* baseForDirection = naturalBase;

        const bool naturalStarted =
            naturalBase &&
            naturalBase->getOwner() == BWAPI::Broodwar->self() &&
            naturalBase->getDepot();

        if (!naturalStarted || !baseForDirection)
        {
            baseForDirection = mainBase;
        }

        if (!baseForDirection)
        {
            return BWAPI::TilePositions::Invalid;
        }

        const BWAPI::Position tankHoldPosition = getTankHoldPositionForTurrets();

        const BWAPI::Position turretPosition =
            positionToward(tankHoldPosition, baseForDirection->getPosition(), 32);

        return BWAPI::TilePosition(turretPosition);
    }

    std::vector<BWAPI::TilePosition> zergAirEmergencyTurretTiles()
    {
        // Six positions close the first mutually supporting ring without
        // spending the ten-slot emergency ceiling at every expansion.
        return strategicAirDefenseTurretTiles(6);
    }

    std::vector<BWAPI::TilePosition> wraithDefenseTurretTiles()
    {
        std::vector<BWAPI::TilePosition> tiles;

        // The urgent predictor posts only the first main-base worker arc. The
        // normal strategic rule plan applies the same three-point arc to every
        // completed mineral line without flooding the immediate queue.
        Base* mainBase = the.bases.myMain();
        const std::vector<Base*> ownedBases = ownedTerranBasesForTurrets();
        if (mainBase &&
            std::find(ownedBases.begin(), ownedBases.end(), mainBase) !=
                ownedBases.end())
        {
            addPerimeterTurretsAroundBase(
                tiles,
                mainBase,
                3,
                TURRET_RING_MIN_TILE_DISTANCE);
        }

        return tiles;
    }

    std::vector<BWAPI::TilePosition> emergencyTurretTiles()
    {
        std::vector<BWAPI::TilePosition> tiles;
        const std::vector<Base*> ownedBases = ownedTerranBasesForTurrets();
        if (ownedBases.empty())
        {
            return tiles;
        }

        Base* base = nullptr;
        for (Base* preferred : {
            the.bases.myFront(),
            the.bases.myNatural(),
            the.bases.myMain() })
        {
            if (preferred &&
                std::find(ownedBases.begin(), ownedBases.end(), preferred) !=
                    ownedBases.end())
            {
                base = preferred;
                break;
            }
        }
        if (!base)
        {
            base = ownedBases.front();
        }

        addPerimeterTurretsAroundBase(
            tiles,
            base,
            6,
            TURRET_RING_MIN_TILE_DISTANCE
        );

        return tiles;
    }

    bool tileAlreadyCoveredByTurretTile(
        const std::vector<BWAPI::TilePosition>& tiles,
        const BWAPI::TilePosition& tile,
        int minTileDistance)
    {
        for (const BWAPI::TilePosition& existing : tiles)
        {
            const int dx = existing.x - tile.x;
            const int dy = existing.y - tile.y;

            if (dx * dx + dy * dy <= minTileDistance * minTileDistance)
            {
                return true;
            }
        }

        return false;
    }

    std::vector<BWAPI::TilePosition> carrierDefenseTurretTiles()
    {
        // Eight is the normal Carrier shell. Ten is reserved for a visible mass
        // capital-air wave; both values are local to each completed base.
        const int perBaseTarget =
            visibleDangerousEnemyAirUnits() >= 12 ? 10 : 8;
        return strategicAirDefenseTurretTiles(perBaseTarget);
    }
}

BWAPI::TilePosition StrategyManager::getTerranDefensiveBunkerTile(
    int bunkerIndex) const
{
    if (noWallRushBunkerPolicy() &&
        (bunkerIndex < 2 || !naturalExpansionStarted()))
        return findDefensiveBunkerTile(the.bases.myMain(), bunkerIndex);
    BWAPI::TilePosition tile = BWAPI::TilePositions::Invalid;
    const bool naturalFrontActive = naturalExpansionStarted() ||
        the.bases.completedBaseCount(the.self()) > 1;
    if (!naturalFrontActive)
    {
        tile = findDefensiveBunkerTile(the.bases.myMain(), bunkerIndex);
    }
    else
    {
        tile = findDefensiveBunkerTile(the.bases.myNatural(), bunkerIndex);
    }
    const bool waitingForMainWallSlot =
        !naturalFrontActive && bunkerIndex == 0 &&
        the.placer.hasTerranMainWall() &&
        !the.placer.isTerranMainWallReleased() &&
        the.placer.getTerranMainWallBunkerTile().isValid();
    return tile.isValid() || naturalFrontActive || waitingForMainWallSlot
        ? tile : findBuildTileNearBaseExit(BWAPI::UnitTypes::Terran_Bunker);
}

bool StrategyManager::isNoWallRushBunkerPlacementActive() const
{
    return noWallRushBunkerPolicy();
}

bool StrategyManager::canBuildTerranDefensiveBunkerAt(BWAPI::TilePosition tile) const
{
    if (!tile.isValid()) return false;
    if (!noWallRushBunkerPolicy()) return true;
    if (safeNaturalRushBunkerTile(tile)) return true;
    Base * main = the.bases.myMain();
    if (!main) return false;
    const BWAPI::Position center = BWAPI::Position(tile) + BWAPI::Position(48, 32);
    // A second close Bunker must not inherit the first shelter's exception.
    // This also protects generic and previously queued Bunker requests.
    if (center.getApproxDistance(main->getCenter()) <= 6 * 32)
    {
        for (BWAPI::Unit unit : the.self()->getUnits())
            if (unit && unit->exists() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Bunker &&
                unit->getDistance(main->getCenter()) <= 7 * 32 &&
                unit->getTilePosition() != tile)
                return false;
        return true;
    }
    return safeForwardRushBunkerTile(tile);
}

namespace
{
    struct TerranPlanProfile
    {
        TerranPlanId id;
        const char * name;
        const char * hostOpeningGroup;
        int minBarracks;
        int minFactories;
        int minMachineShops;
        int tankWeight;
        int vultureWeight;
        int goliathWeight;
        int minGoliathCap;
        int tankRelease;
        int goliathRelease;
        int earliestThirdMinute;
        bool allowTankDrop;
    };

    const TerranPlanProfile & terranPlanProfile(TerranPlanId id)
    {
        static const TerranPlanProfile none =
            { TerranPlanId::None, "None", "", 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, true };
        static const TerranPlanProfile bioTank =
            { TerranPlanId::BioTank, "BioTank", "bio", 2, 1, 1, 0, 0, 0, 0, 0, 0, 8, true };
        static const TerranPlanProfile airTanks =
            { TerranPlanId::AirTanks, "AirTanks", "airtanks", 1, 1, 1, 0, 0, 0, 0, 0, 0, AirTanks::ThirdMinute, false };
        static const TerranPlanProfile tvpDeepSix =
            { TerranPlanId::TvP_DeepSix, "TvP_DeepSix", "bio", 6, 2, 2, 0, 0, 0, 0, 0, 0, 12, false };
        static const TerranPlanProfile tvpSiege =
            { TerranPlanId::TvP_Siege, "TvP_Siege", "tanks", 1, 2, 1, 5, 2, 1, 3, 15, 0, 8, true };
        static const TerranPlanProfile tvpAntiZealot =
            { TerranPlanId::TvP_AntiZealot, "TvP_AntiZealot", "tanks", 1, 2, 1, 2, 7, 0, 0, 6, 0, 9, false };
        static const TerranPlanProfile tvpAntiDragoon =
            { TerranPlanId::TvP_AntiDragoon, "TvP_AntiDragoon", "tanks", 1, 3, 2, 4, 3, 0, 0, 10, 0, 10, false };
        static const TerranPlanProfile tvpAntiCarrier =
            { TerranPlanId::TvP_AntiCarrier, "TvP_AntiCarrier", "tanks", 2, 4, 0, 0, 0, 10, 16, 0, 0, 9, false };
        static const TerranPlanProfile tvzSparks =
            { TerranPlanId::TvZ_Sparks, "TvZ_Sparks", "bio", 3, 0, 0, 0, 0, 0, 0, 0, 0, TvZSparks::policy().thirdMinute, false };
        static const TerranPlanProfile tvzPreHive =
            { TerranPlanId::TvZ_PreHive, "TvZ_PreHive", "bio", 3, 1, 1, 0, 0, 0, 0, 0, 0, TvZPreHive::policy().thirdMinute, false };
        static const TerranPlanProfile tvzSK =
            { TerranPlanId::TvZ_SK, "TvZ_SK", "bio", 4, 1, 0, 0, 0, 0, 0, 0, 0, TvZSK::policy().thirdMinute, false };
        static const TerranPlanProfile tvzMech =
            { TerranPlanId::TvZ_Mech, "TvZ_Mech", "tanks", 1, 2, 1, 3, 4, 1, 3, 10, 0, 8, true };
        static const TerranPlanProfile tvzAntiLing =
            { TerranPlanId::TvZ_AntiLing, "TvZ_AntiLing", "tanks", 1, 2, 1, 2, 7, 0, 0, 5, 0, 10, false };
        static const TerranPlanProfile tvzVulturePressure =
            { TerranPlanId::TvZ_VulturePressure, "TvZ_VulturePressure", "tanks", 1, 2, 1, 2, 7, 1, 4, 5, 0, 8, false };
        static const TerranPlanProfile tvzAntiHydra =
            { TerranPlanId::TvZ_AntiHydra, "TvZ_AntiHydra", "tanks", 1, 3, 2, 5, 3, 0, 0, 8, 0, 10, false };
        static const TerranPlanProfile tvzAntiMuta =
            { TerranPlanId::TvZ_AntiMuta, "TvZ_AntiMuta", "tanks", 1, 3, 0, 0, 0, 10, 12, 0, 0, 9, false };
        static const TerranPlanProfile tvtMech =
            { TerranPlanId::TvT_Mech, "TvT_Mech", "tanks", 1, 2, 2, 6, 3, 1, 3, 15, 0, 9, true };
        static const TerranPlanProfile tvtAnti2Factory =
            { TerranPlanId::TvT_Anti2Factory, "TvT_Anti2Factory", "tanks", 1, 3, 3, 7, 3, 0, 0, 10, 0, 11, false };
        static const TerranPlanProfile tvtAntiAir =
            { TerranPlanId::TvT_AntiAir, "TvT_AntiAir", "tanks", 1, 3, 2, 5, 2, 3, 6, 5, 4, 9, false };

        switch (id)
        {
        case TerranPlanId::BioTank:            return bioTank;
        case TerranPlanId::AirTanks:           return airTanks;
        case TerranPlanId::TvP_DeepSix:        return tvpDeepSix;
        case TerranPlanId::TvP_Siege:          return tvpSiege;
        case TerranPlanId::TvP_AntiZealot:     return tvpAntiZealot;
        case TerranPlanId::TvP_AntiDragoon:    return tvpAntiDragoon;
        case TerranPlanId::TvP_AntiCarrier:    return tvpAntiCarrier;
        case TerranPlanId::TvZ_Sparks:         return tvzSparks;
        case TerranPlanId::TvZ_PreHive:        return tvzPreHive;
        case TerranPlanId::TvZ_SK:             return tvzSK;
        case TerranPlanId::TvZ_Mech:           return tvzMech;
        case TerranPlanId::TvZ_AntiLing:       return tvzAntiLing;
        case TerranPlanId::TvZ_VulturePressure:return tvzVulturePressure;
        case TerranPlanId::TvZ_AntiHydra:      return tvzAntiHydra;
        case TerranPlanId::TvZ_AntiMuta:       return tvzAntiMuta;
        case TerranPlanId::TvT_Mech:           return tvtMech;
        case TerranPlanId::TvT_Anti2Factory:   return tvtAnti2Factory;
        case TerranPlanId::TvT_AntiAir:        return tvtAntiAir;
        case TerranPlanId::None:
        default:                                return none;
        }
    }

    TerranPlanId terranPlanFromName(const std::string & name)
    {
        const TerranPlanId plans[] =
        {
            TerranPlanId::BioTank,
            TerranPlanId::AirTanks,
            TerranPlanId::TvP_DeepSix,
            TerranPlanId::TvP_Siege,
            TerranPlanId::TvP_AntiZealot,
            TerranPlanId::TvP_AntiDragoon,
            TerranPlanId::TvP_AntiCarrier,
            TerranPlanId::TvZ_Sparks,
            TerranPlanId::TvZ_PreHive,
            TerranPlanId::TvZ_SK,
            TerranPlanId::TvZ_Mech,
            TerranPlanId::TvZ_AntiLing,
            TerranPlanId::TvZ_VulturePressure,
            TerranPlanId::TvZ_AntiHydra,
            TerranPlanId::TvZ_AntiMuta,
            TerranPlanId::TvT_Mech,
            TerranPlanId::TvT_Anti2Factory,
            TerranPlanId::TvT_AntiAir
        };

        for (TerranPlanId plan : plans)
        {
            if (name == terranPlanProfile(plan).name)
            {
                return plan;
            }
        }
        return TerranPlanId::None;
    }

    bool terranPlanMatchesEnemyRace(TerranPlanId plan, BWAPI::Race race)
    {
        if (plan == TerranPlanId::BioTank || plan == TerranPlanId::AirTanks)
        {
            return true;
        }

        if (race == BWAPI::Races::Protoss)
        {
            return
                plan == TerranPlanId::TvP_DeepSix ||
                plan == TerranPlanId::TvP_Siege ||
                plan == TerranPlanId::TvP_AntiZealot ||
                plan == TerranPlanId::TvP_AntiDragoon ||
                plan == TerranPlanId::TvP_AntiCarrier;
        }

        if (race == BWAPI::Races::Zerg)
        {
            return
                plan == TerranPlanId::TvZ_Sparks ||
                plan == TerranPlanId::TvZ_PreHive ||
                plan == TerranPlanId::TvZ_SK ||
                plan == TerranPlanId::TvZ_Mech ||
                plan == TerranPlanId::TvZ_AntiLing ||
                plan == TerranPlanId::TvZ_VulturePressure ||
                plan == TerranPlanId::TvZ_AntiHydra ||
                plan == TerranPlanId::TvZ_AntiMuta;
        }

        if (race == BWAPI::Races::Terran)
        {
            return
                plan == TerranPlanId::TvT_Mech ||
                plan == TerranPlanId::TvT_Anti2Factory ||
                plan == TerranPlanId::TvT_AntiAir;
        }

        return plan == TerranPlanId::None;
    }

    TerranPlanId defaultTerranPlanFor(
        const std::string & openingGroup,
        BWAPI::Race enemyRace)
    {
        if (openingGroup == "airtanks") return TerranPlanId::AirTanks;

        const bool zergDropTransitionsToBio =
            openingGroup == "drop" && enemyRace == BWAPI::Races::Zerg;

        if (openingGroup == "bio" ||
            openingGroup == "anti-rush" ||
            zergDropTransitionsToBio)
        {
            return TerranPlanId::BioTank;
        }

        if (enemyRace == BWAPI::Races::Protoss)
        {
            return TerranPlanId::TvP_Siege;
        }
        if (enemyRace == BWAPI::Races::Zerg)
        {
            return TerranPlanId::TvZ_Mech;
        }
        if (enemyRace == BWAPI::Races::Terran)
        {
            return TerranPlanId::TvT_Mech;
        }

        return TerranPlanId::BioTank;
    }

    int terranPlanPriority(TerranPlanId id)
    {
        switch (id)
        {
        case TerranPlanId::TvP_AntiCarrier:
        case TerranPlanId::TvZ_AntiMuta:
        case TerranPlanId::TvT_AntiAir:
            return 100;
        case TerranPlanId::TvT_Anti2Factory:
        case TerranPlanId::TvP_AntiDragoon:
        case TerranPlanId::TvZ_AntiHydra:
            return 70;
        case TerranPlanId::TvP_AntiZealot:
        case TerranPlanId::TvZ_AntiLing:
            return 60;
        case TerranPlanId::TvZ_Sparks:
        case TerranPlanId::TvZ_PreHive:
        case TerranPlanId::TvZ_SK:
        case TerranPlanId::TvP_DeepSix:
        case TerranPlanId::TvZ_VulturePressure:
            return 40;
        case TerranPlanId::TvP_Siege:
        case TerranPlanId::TvZ_Mech:
        case TerranPlanId::TvT_Mech:
            return 30;
        case TerranPlanId::BioTank:
            return 20;
        case TerranPlanId::None:
        default:
            return 0;
        }
    }

    bool isSpecializedTerranPlan(TerranPlanId id)
    {
        return
            id == TerranPlanId::TvP_AntiDragoon ||
            id == TerranPlanId::TvP_AntiZealot ||
            id == TerranPlanId::TvP_AntiCarrier ||
            id == TerranPlanId::TvZ_AntiLing ||
            id == TerranPlanId::TvZ_VulturePressure ||
            id == TerranPlanId::TvZ_AntiHydra ||
            id == TerranPlanId::TvZ_AntiMuta ||
            id == TerranPlanId::TvT_Anti2Factory ||
            id == TerranPlanId::TvT_AntiAir;
    }

    void applyTerranPlanComposition(
        const TerranPlanProfile & plan,
        bool pureAirPlan,
        bool enemyAirThreat,
        int numGoliaths,
        int & tankWeight,
        int & vultureWeight,
        int & goliathWeight,
        int & goliathCap,
        bool & allowTankDrop)
    {
        allowTankDrop = allowTankDrop && plan.allowTankDrop;

        if (pureAirPlan)
        {
            if (plan.id == TerranPlanId::TvP_AntiCarrier ||
                plan.id == TerranPlanId::TvZ_AntiMuta ||
                plan.id == TerranPlanId::TvT_AntiAir)
            {
                tankWeight = 0;
                vultureWeight = 0;
                goliathWeight = std::max(goliathWeight, plan.goliathWeight);
                goliathCap = std::max(goliathCap, plan.minGoliathCap);
            }
            return;
        }

        switch (plan.id)
        {
        case TerranPlanId::TvP_AntiZealot:
        case TerranPlanId::TvZ_AntiLing:
        case TerranPlanId::TvZ_VulturePressure:
            tankWeight = std::min(tankWeight, plan.tankWeight);
            vultureWeight = std::max(vultureWeight, plan.vultureWeight);
            if (!enemyAirThreat)
            {
                goliathWeight = 0;
                goliathCap = numGoliaths;
            }
            break;

        case TerranPlanId::TvP_AntiDragoon:
        case TerranPlanId::TvZ_AntiHydra:
            tankWeight = std::max(tankWeight, plan.tankWeight);
            vultureWeight = std::max(vultureWeight, plan.vultureWeight);
            if (!enemyAirThreat)
            {
                goliathWeight = 0;
                goliathCap = numGoliaths;
            }
            break;

        case TerranPlanId::TvT_Anti2Factory:
            tankWeight = std::max(tankWeight, plan.tankWeight);
            vultureWeight = std::max(vultureWeight, plan.vultureWeight);
            if (!enemyAirThreat)
            {
                goliathWeight = 0;
                goliathCap = numGoliaths;
            }
            break;

        case TerranPlanId::TvT_AntiAir:
            // Wraith is commonly a transition into Tanks. Keep a real Siege
            // core while adding a bounded Goliath layer, instead of locking
            // every Factory into Goliaths after one early air phase.
            tankWeight = std::max(tankWeight, plan.tankWeight);
            vultureWeight = std::min(vultureWeight, plan.vultureWeight);
            goliathWeight = std::max(goliathWeight, plan.goliathWeight);
            goliathCap = std::max(goliathCap, plan.minGoliathCap);
            allowTankDrop = false;
            break;

        case TerranPlanId::TvP_Siege:
        case TerranPlanId::TvZ_Mech:
        case TerranPlanId::TvT_Mech:
            tankWeight = std::max(tankWeight, plan.tankWeight);
            vultureWeight = std::max(vultureWeight, plan.vultureWeight);
            if (enemyAirThreat)
            {
                goliathWeight = std::max(goliathWeight, plan.goliathWeight);
                goliathCap = std::max(goliathCap, plan.minGoliathCap);
            }
            break;

        default:
            break;
        }
    }
}

const char * StrategyManager::getTerranPlanName() const
{
    return terranPlanProfile(_terranPlan).name;
}

const char * StrategyManager::getEnemyArmyModeName() const
{
    return enemyArmyModeToString(currentEnemyArmyMode());
}

unsigned int StrategyManager::getEnemyThreatMask() const
{
    return currentEnemyThreatMask();
}

TerranArmyPosture StrategyManager::getTerranArmyPosture() const
{
    

    const EnemyArmyMode enemyMode = currentEnemyArmyMode();
    const bool bioAgainstZergAir =
        bioTankPlanActive() &&
        the.enemyRace() == BWAPI::Races::Zerg &&
        enemyMode == EnemyArmyMode::PureAir;
    if (_pureAirPlan ||
        (enemyMode == EnemyArmyMode::PureAir && !bioAgainstZergAir))
    {
        return TerranArmyPosture::PureAir;
    }

    if (bioAgainstZergAir ||
        enemyMode == EnemyArmyMode::MixedAirHeavyGround ||
        enemyMode == EnemyArmyMode::MixedAirLightGround ||
        _terranPlan == TerranPlanId::TvP_AntiCarrier ||
        _terranPlan == TerranPlanId::TvZ_AntiMuta ||
        _terranPlan == TerranPlanId::TvT_AntiAir ||
        learnedMixedExpectedAfterOpening() ||
        learnedPureAirExpectedAfterOpening() ||
        learnedEnemyThreatExpectedAfterOpening(EnemyThreat::GroundAir) ||
        learnedEnemyThreatExpectedAfterOpening(EnemyThreat::CapitalAir))
    {
        return TerranArmyPosture::Mixed;
    }

    return TerranArmyPosture::Ground;
}

bool StrategyManager::isTerranHeavyMixed() const
{
    return
        _selfRace == BWAPI::Races::Terran &&
        !_pureAirPlan &&
        currentEnemyArmyMode() == EnemyArmyMode::MixedAirHeavyGround;
}

bool StrategyManager::isTerranLightMixed() const
{
    return
        _selfRace == BWAPI::Races::Terran &&
        !_pureAirPlan &&
        currentEnemyArmyMode() == EnemyArmyMode::MixedAirLightGround;
}

const char * StrategyManager::getTerranArmyPostureName() const
{
    switch (getTerranArmyPosture())
    {
    case TerranArmyPosture::PureAir: return "PureAir";
    case TerranArmyPosture::Mixed:   return "Mixed";
    case TerranArmyPosture::Ground:  return "Ground";
    default:                         return "Ground";
    }
}

static bool techInQueue(
    BuildOrderQueue& queue,
    BWAPI::TechType tech)
{
    for (int i = 0; i < (int)queue.size(); ++i)
    {
        const BuildOrderItem item = queue[i];

        if (!item.macroAct.isTech())
        {
            continue;
        }

        if (item.macroAct.getTechType() == tech)
        {
            return true;
        }
    }

    return false;
}

// BOSS stores one target count per action type. If the strategy adds the same
// unit, building, technology or upgrade more than once, the last setGoal()
// call silently replaces the earlier one. The Terran goal is assembled by
// several independent policy blocks, so duplicates are expected. Merge them
// here and retain the highest requested count.
static bool sameBossGoalAction(const MacroAct& first, const MacroAct& second)
{
    if (first.isUnit() && second.isUnit())
    {
        if (first.getUnitType() != second.getUnitType())
        {
            return false;
        }

        if (!first.getUnitType().isBuilding())
        {
            return true;
        }

        if (first.getMacroLocation() != second.getMacroLocation())
        {
            return false;
        }

        return
            first.getMacroLocation() != MacroLocation::Tile ||
            first.getTileLocation() == second.getTileLocation();
    }

    if (first.isTech() && second.isTech())
    {
        return first.getTechType() == second.getTechType();
    }

    if (first.isUpgrade() && second.isUpgrade())
    {
        return first.getUpgradeType() == second.getUpgradeType();
    }

    return false;
}

static size_t currentTerranGoalProgress(const MacroAct& act)
{
    if (act.isUnit())
    {
        const BWAPI::UnitType type = act.getUnitType();

        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
        {
            return
                the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
                the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
        }

        size_t count = 0;

        const bool locationSpecific =
            type.isBuilding() &&
            (act.getMacroLocation() == MacroLocation::Main ||
             act.getMacroLocation() == MacroLocation::Natural ||
             act.getMacroLocation() == MacroLocation::Front ||
             act.getMacroLocation() == MacroLocation::Center ||
             act.getMacroLocation() == MacroLocation::Proxy ||
             act.getMacroLocation() == MacroLocation::EnemyMain ||
             act.getMacroLocation() == MacroLocation::EnemyNatural ||
             act.getMacroLocation() == MacroLocation::Tile);

        if (locationSpecific)
        {
            const BWAPI::Position target =
                act.getMacroLocation() == MacroLocation::Tile
                ? BWAPI::Position(act.getTileLocation())
                : the.placer.getMacroLocationPos(act.getMacroLocation());

            if (target.isValid())
            {
                const int radius =
                    act.getMacroLocation() == MacroLocation::Tile
                    ? 7 * 32
                    : 14 * 32;

                for (BWAPI::Unit unit : the.self()->getUnits())
                {
                    if (unit && unit->exists() &&
                        unit->getType() == type &&
                        unit->getPosition().isValid() &&
                        unit->getPosition().getApproxDistance(target) <= radius)
                    {
                        ++count;
                    }
                }

                count += BuildingManager::Instance().getNumUnstarted(
                    type,
                    act.getMacroLocation(),
                    act.getTileLocation());
            }
            else
            {
                count = the.my.all.count(type);
                count += BuildingManager::Instance().getNumUnstarted(type);
            }
        }
        else
        {
            count = the.my.all.count(type);

            // BOSS receives these unstarted BuildingManager jobs as actions in
            // progress. Count them here as well, otherwise a goal which BOSS sees
            // as already complete is submitted again every time the queue empties.
            if (type.isBuilding())
            {
                count += BuildingManager::Instance().getNumUnstarted(type);
            }
        }

        return count;
    }

    if (act.isTech())
    {
        const BWAPI::TechType tech = act.getTechType();

        return
            BWAPI::Broodwar->self()->hasResearched(tech) ||
            BWAPI::Broodwar->self()->isResearching(tech)
            ? 1
            : 0;
    }

    if (act.isUpgrade())
    {
        const BWAPI::UpgradeType upgrade = act.getUpgradeType();
        size_t level = BWAPI::Broodwar->self()->getUpgradeLevel(upgrade);

        if (BWAPI::Broodwar->self()->isUpgrading(upgrade))
        {
            ++level;
        }

        return level;
    }

    return 0;
}

static MetaPairVector normalizeTerranBuildOrderGoal(const MetaPairVector& rawGoal)
{
    MetaPairVector mergedGoal;

    for (const MetaPair& item : rawGoal)
    {
        if (item.second == 0 ||
            (!item.first.isUnit() && !item.first.isTech() && !item.first.isUpgrade()))
        {
            continue;
        }

        bool merged = false;

        for (MetaPair& existing : mergedGoal)
        {
            if (sameBossGoalAction(existing.first, item.first))
            {
                existing.second = std::max(existing.second, item.second);
                merged = true;
                break;
            }
        }

        if (!merged)
        {
            mergedGoal.push_back(item);
        }
    }

    // Structural safety cap before BOSS sees the goal. No Terran strategy in
    // GrimHammer needs more than two Engineering Bays; if independent policy
    // blocks or a future edit accidentally request more, never hand that large
    // target to the search in the first place.
    for (MetaPair& item : mergedGoal)
    {
        if (item.first.isUnit() &&
            item.first.getUnitType() == BWAPI::UnitTypes::Terran_Engineering_Bay)
        {
            item.second = std::min(item.second, size_t(2));
        }
    }

    mergedGoal.erase(
        std::remove_if(
            mergedGoal.begin(),
            mergedGoal.end(),
            [](const MetaPair& item)
            {
                return currentTerranGoalProgress(item.first) >= item.second;
            }),
        mergedGoal.end());

    return mergedGoal;
}


// Decide the next Factory unit from the desired composition instead of
// maintaining three independent rolling "current + N" goals.
//
// Only one Factory-unit target is exposed to ProductionManager at a time.
// After that unit starts, ProductionManager invalidates the rule plan and this
// function is evaluated again with the new in-production counts. That makes
// Tank/Vulture/Goliath share Factory time instead of the first rule starving
// every rule below it.
static BWAPI::UnitType chooseNextTerranFactoryUnit(
    int numTanks,
    int numVultures,
    int numGoliaths,
    int tankWeight,
    int vultureWeight,
    int goliathWeight,
    int goliathTarget,
    int forcedVultureTarget,
    bool forceGoliathUntilTarget)
{
    const BWAPI::UnitType tank =
        BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode;
    const BWAPI::UnitType vulture =
        BWAPI::UnitTypes::Terran_Vulture;
    const BWAPI::UnitType goliath =
        BWAPI::UnitTypes::Terran_Goliath;

    // Emergency layers are absolute requirements, not ratios.
    if (forcedVultureTarget > 0 &&
        numVultures < forcedVultureTarget)
    {
        return vulture;
    }

    if (forceGoliathUntilTarget &&
        goliathTarget > numGoliaths)
    {
        return goliath;
    }

    const bool tankEligible = tankWeight > 0;
    const bool vultureEligible = vultureWeight > 0;
    const bool goliathEligible =
        goliathWeight > 0 &&
        goliathTarget > numGoliaths;

    const int totalWeight =
        (tankEligible ? tankWeight : 0) +
        (vultureEligible ? vultureWeight : 0) +
        (goliathEligible ? goliathWeight : 0);

    if (totalWeight <= 0)
    {
        return BWAPI::UnitTypes::None;
    }

    // Score = how far this unit is below its desired share after one more
    // Factory slot is consumed. The largest score wins.
    //
    // Example for LightGround (Tank 3 : Vulture 4):
    // T=10 V=2 strongly favors Vulture until the ratio catches up. Unlike
    // current+N goals, the target cannot run away every one-second refresh.
    int activeTotal = 0;

    if (tankEligible)
    {
        activeTotal += numTanks;
    }
    if (vultureEligible)
    {
        activeTotal += numVultures;
    }
    if (goliathEligible)
    {
        activeTotal += numGoliaths;
    }

    long long bestScore = LLONG_MIN;
    BWAPI::UnitType bestType = BWAPI::UnitTypes::None;

    auto consider =
        [&](BWAPI::UnitType type, int count, int weight)
        {
            if (weight <= 0)
            {
                return;
            }

            const long long score =
                (long long)weight * (activeTotal + 1) -
                (long long)count * totalWeight;

            if (score > bestScore)
            {
                bestScore = score;
                bestType = type;
            }
        };

    // Tie order intentionally keeps the Tanks identity of SiegeExpand/Tanks.
    consider(tank, numTanks, tankEligible ? tankWeight : 0);
    consider(vulture, numVultures, vultureEligible ? vultureWeight : 0);
    consider(goliath, numGoliaths, goliathEligible ? goliathWeight : 0);

    return bestType;
}

static void appendTerranCarrierGhostPlan(
    MetaPairVector& goal,
    bool requireTwoBarracks,
    bool requireControlTower)
{
    BuildOrderQueue& queue = ProductionManager::Instance().getQueue();
    BWAPI::Player self = the.self();

    const int barracksPlanned =
        the.my.all.count(BWAPI::UnitTypes::Terran_Barracks) +
        (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Barracks) +
        (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Barracks);

    const int desiredBarracks = requireTwoBarracks ? 2 : 1;
    if (barracksPlanned < desiredBarracks)
    {
        goal.push_back(MetaPair(
            BWAPI::UnitTypes::Terran_Barracks,
            desiredBarracks));
    }

    const bool academyExistsOrPlanned =
        the.my.all.count(BWAPI::UnitTypes::Terran_Academy) > 0 ||
        BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Academy) > 0 ||
        queue.anyInQueue(BWAPI::UnitTypes::Terran_Academy);
    if (!academyExistsOrPlanned)
    {
        goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Academy, 1));
    }

    // Do not submit the complete late-tech dependency tree to BOSS on the
    // exact frame PureAir is recognized. This old planner is much more stable
    // when each search receives only the next legal stage. Buildings already
    // under construction count as planned, so we wait for completion before
    // exposing the following stage.
    const bool starportExistsOrPlanned =
        the.my.all.count(BWAPI::UnitTypes::Terran_Starport) > 0 ||
        BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Starport) > 0 ||
        queue.anyInQueue(BWAPI::UnitTypes::Terran_Starport);

    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Starport) <= 0)
    {
        if (!starportExistsOrPlanned)
        {
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, 1));
        }
        return;
    }

    // The Control Tower can be added in parallel with the Science Facility
    // once the Starport is complete. Waiting until after Covert Ops delayed
    // the first Vessel/EMP by an entire extra technology stage.
    if (requireControlTower)
    {
        const bool controlTowerExistsOrPlanned =
            the.my.all.count(BWAPI::UnitTypes::Terran_Control_Tower) > 0 ||
            BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Control_Tower) > 0 ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Control_Tower);
        if (!controlTowerExistsOrPlanned)
        {
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Control_Tower, 1));
        }
    }

    const bool scienceFacilityExistsOrPlanned =
        the.my.all.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0 ||
        BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Science_Facility) > 0 ||
        queue.anyInQueue(BWAPI::UnitTypes::Terran_Science_Facility);

    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Facility) <= 0)
    {
        if (!scienceFacilityExistsOrPlanned)
        {
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Science_Facility, 1));
        }
        return;
    }

    const bool covertOpsExistsOrPlanned =
        the.my.all.count(BWAPI::UnitTypes::Terran_Covert_Ops) > 0 ||
        BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Covert_Ops) > 0 ||
        queue.anyInQueue(BWAPI::UnitTypes::Terran_Covert_Ops);

    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Covert_Ops) <= 0)
    {
        if (!covertOpsExistsOrPlanned)
        {
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Covert_Ops, 1));
        }
        return;
    }

    if (!self->hasResearched(BWAPI::TechTypes::Lockdown) &&
        !self->isResearching(BWAPI::TechTypes::Lockdown) &&
        !techInQueue(queue, BWAPI::TechTypes::Lockdown))
    {
        goal.push_back(MetaPair(BWAPI::TechTypes::Lockdown, 1));
    }

    if (self->hasResearched(BWAPI::TechTypes::Lockdown) &&
        !self->hasResearched(BWAPI::TechTypes::Personnel_Cloaking) &&
        !self->isResearching(BWAPI::TechTypes::Personnel_Cloaking) &&
        !techInQueue(queue, BWAPI::TechTypes::Personnel_Cloaking))
    {
        goal.push_back(MetaPair(BWAPI::TechTypes::Personnel_Cloaking, 1));
    }

    if (self->hasResearched(BWAPI::TechTypes::Personnel_Cloaking) &&
        self->getUpgradeLevel(BWAPI::UpgradeTypes::Moebius_Reactor) == 0 &&
        !self->isUpgrading(BWAPI::UpgradeTypes::Moebius_Reactor) &&
        !queue.anyInQueue(BWAPI::UpgradeTypes::Moebius_Reactor))
    {
        goal.push_back(MetaPair(BWAPI::UpgradeTypes::Moebius_Reactor, 1));
    }

    const int desiredGhosts = 5;
    const int ownedOrTrainingGhosts = countOwnedAndTrainingTerranGhosts();
    const int queuedGhosts =
        (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Ghost);

    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Academy) > 0 &&
        ownedOrTrainingGhosts + queuedGhosts < desiredGhosts &&
        queuedGhosts == 0)
    {
        const int nextWave = requireTwoBarracks ? 2 : 1;
        goal.push_back(MetaPair(
            BWAPI::UnitTypes::Terran_Ghost,
            std::min(desiredGhosts, ownedOrTrainingGhosts + nextWave)));
    }
}

// A failed or trivial BOSS search must never leave all production idle. This
// is intentionally a small, prerequisite-safe production wave. It does not
// replace strategic planning; it keeps existing producers working until the
// next full goal can be searched successfully.
static BuildOrder makeTerranSearchRecoveryBuildOrder(
    const std::string& openingGroup,
    bool pureAirPlan)
{
    BuildOrder recovery(BWAPI::Races::Terran);

    const bool nearbyAirOnlyAttack =
        visibleEnemyAirOnlyNearOwnedBase();

    const bool airDefenseRecovery =
        pureAirPlan || nearbyAirOnlyAttack;

    const int completedCommandCenters =
        the.my.completed.count(BWAPI::UnitTypes::Terran_Command_Center);

    const int completedFactories =
        the.my.completed.count(BWAPI::UnitTypes::Terran_Factory);

    const int completedMachineShops =
        the.my.completed.count(BWAPI::UnitTypes::Terran_Machine_Shop);

    if (openingGroup == "tanks" || openingGroup == "vultures" || openingGroup == "drop")
    {
        if (completedFactories > 0)
        {
            const int factoryJobs = std::min(4, completedFactories);

            if (airDefenseRecovery &&
                the.my.completed.count(BWAPI::UnitTypes::Terran_Armory) > 0)
            {
                for (int i = 0; i < factoryJobs; ++i)
                {
                    recovery.add(BWAPI::UnitTypes::Terran_Goliath);
                }
            }
            else if (!airDefenseRecovery)
            {
                // Only a Factory with a Machine Shop can train a tank. Put one
                // tank in the recovery wave and let the other Factories make
                // Vultures instead of blocking behind several tank requests.
                int remainingFactoryJobs = factoryJobs;

                if (!pureAirPlan && completedMachineShops > 0)
                {
                    recovery.add(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode);
                    --remainingFactoryJobs;
                }

                for (int i = 0; i < remainingFactoryJobs; ++i)
                {
                    recovery.add(BWAPI::UnitTypes::Terran_Vulture);
                }
            }
            else if (the.my.all.count(BWAPI::UnitTypes::Terran_Armory) == 0 &&
                !BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Armory))
            {
                // Do not reintroduce Vultures into a PureAir response merely
                // because the Armory is late. Complete the missing prerequisite
                // and let urgent production fill the Factories with Goliaths.
                recovery.add(BWAPI::UnitTypes::Terran_Armory);
            }
        }
        else if (the.my.all.count(BWAPI::UnitTypes::Terran_Factory) == 0 &&
            !BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Factory))
        {
            recovery.add(BWAPI::UnitTypes::Terran_Factory);
        }
    }
    else
    {
        const int completedBarracks =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Barracks);

        if (completedBarracks > 0)
        {
            for (int i = 0; i < std::min(4, completedBarracks); ++i)
            {
                recovery.add(BWAPI::UnitTypes::Terran_Marine);
            }
        }
        else if (the.my.all.count(BWAPI::UnitTypes::Terran_Barracks) == 0 &&
            !BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Barracks))
        {
            recovery.add(BWAPI::UnitTypes::Terran_Barracks);
        }
    }

    const int scvs = the.my.all.count(BWAPI::UnitTypes::Terran_SCV);
    const int maxSCVs = WorkerManager::Instance().getMaxWorkers();

    if (completedCommandCenters > 0 && scvs < maxSCVs)
    {
        recovery.add(BWAPI::UnitTypes::Terran_SCV);
    }

    return recovery;
}

void StrategyManager::initializeTerranPlan()
{
    

    TerranPlanId defaultPlan =
        defaultTerranPlanFor(_openingGroup, _enemyRace);

    // A strategy may declare its post-book macro identity in JSON. Accept it
    // only for the actual matchup; a typo or a TvZ plan selected in TvP falls
    // back to the safe race baseline instead of corrupting production.
    const TerranPlanId configuredPlan =
        terranPlanFromName(_openingTerranPlanName);

    if (configuredPlan != TerranPlanId::None &&
        terranPlanMatchesEnemyRace(configuredPlan, _enemyRace))
    {
        defaultPlan = configuredPlan;
    }

    // UCB may pre-emptively select only a race-compatible Plan. Specialized
    // arms do not become eligible until past games show that this opponent
    // repeatedly forces that transition, so learning cannot blindly open an
    // anti-air Plan against an opponent with no air history.
    std::vector<std::string> allowedPlans;
    allowedPlans.push_back(terranPlanProfile(defaultPlan).name);
    if (_enemyRace == BWAPI::Races::Protoss)
    {
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvP_Siege).name);
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvP_AntiZealot).name);
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvP_AntiDragoon).name);
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvP_AntiCarrier).name);
    }
    else if (_enemyRace == BWAPI::Races::Zerg)
    {
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvZ_Mech).name);
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvZ_AntiLing).name);
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvZ_VulturePressure).name);
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvZ_AntiHydra).name);
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvZ_AntiMuta).name);
    }
    else if (_enemyRace == BWAPI::Races::Terran)
    {
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvT_Mech).name);
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvT_Anti2Factory).name);
        allowedPlans.push_back(terranPlanProfile(TerranPlanId::TvT_AntiAir).name);
    }

    // Remove duplicates while preserving deterministic order.
    std::vector<std::string> uniquePlans;
    for (const std::string & plan : allowedPlans)
    {
        if (std::find(uniquePlans.begin(), uniquePlans.end(), plan) == uniquePlans.end())
        {
            uniquePlans.push_back(plan);
        }
    }

    TerranPlanId plan = defaultPlan;
    // Opening selection already learns between the allowed Bio books. Do not
    // let a second, independent plan recommendation turn a selected BioTank
    // opening into mech before its Marine/Medic production starts. Deep Six
    // still has its explicit safety cancellation in updateTerranPlan().
    if (_openingGroup != "bio" && defaultPlan != TerranPlanId::TvP_DeepSix &&
        !tvzBioPolicy(defaultPlan) && defaultPlan != TerranPlanId::AirTanks)
    {
        const std::string recommendation = OpponentModel::Instance().recommendTerranPlan(
            terranPlanProfile(defaultPlan).name,
            uniquePlans);
        const TerranPlanId learnedPlan = terranPlanFromName(recommendation);
        if (learnedPlan != TerranPlanId::None)
        {
            plan = learnedPlan;
        }
    }

    _terranBaselinePlan = plan;
    _terranPlan = plan;
    _terranPlanEnteredFrame = BWAPI::BroodwarPtr ? BWAPI::Broodwar->getFrameCount() : 0;
    ++_terranPlanRevision;
    _terranPlanReason = plan == defaultPlan
        ? std::string("opening ") + _openingGroup
        : std::string("opponent UCB from ") + terranPlanProfile(defaultPlan).name;

    OpponentModel::Instance().setInitialTerranPlan(terranPlanProfile(_terranPlan).name);
}

bool StrategyManager::switchTerranPlan(
    TerranPlanId plan,
    const std::string & reason,
    bool invalidateMacro)
{
    if (_selfRace != BWAPI::Races::Terran || plan == TerranPlanId::None)
    {
        return false;
    }

    if (_terranPlan == plan)
    {
        return false;
    }

    // A specialized response may return to the matchup baseline after its
    // evidence has disappeared. EnemyArmyMode already supplies 12-20 seconds
    // of hysteresis; this minimum residence prevents a second source of
    // one-frame plan oscillation without making the plan permanent.
    if (isSpecializedTerranPlan(_terranPlan) &&
        terranPlanPriority(plan) < terranPlanPriority(_terranPlan) &&
        BWAPI::Broodwar->getFrameCount() < _terranPlanEnteredFrame + 20 * 24)
    {
        return false;
    }

    const char * oldName = terranPlanProfile(_terranPlan).name;
    _terranPlan = plan;
    _terranPlanEnteredFrame = BWAPI::Broodwar->getFrameCount();
    ++_terranPlanRevision;
    _terranPlanReason = reason;

    if (invalidateMacro)
    {
        BOSSManager::Instance().reset();
        ProductionManager::Instance().invalidateTerranRulePlan(
            std::string("plan -> ") + terranPlanProfile(plan).name + ": " + reason,
            true);
    }

    BWAPI::Broodwar->printf(
        "Terran plan: %s -> %s (%s)",
        oldName,
        terranPlanProfile(plan).name,
        reason.c_str());

    return true;
}

void StrategyManager::updateTerranPlan()
{
    

    if (_terranPlan == TerranPlanId::None)
    {
        initializeTerranPlan();
    }

    const BWAPI::Race race = the.enemyRace();
    const EnemyArmyMode enemyMode = currentEnemyArmyMode();
    const OpeningPlan preparationPlan =
        OpponentModel::Instance().getPreparationEnemyPlan();

    // Return to the selected opening/learning baseline when no current threat
    // requires a counter plan. Previously this block rebuilt a generic race
    // baseline every update and immediately erased both JSON TerranPlan and
    // the opponent model's learned initial plan.
    if (!terranPlanMatchesEnemyRace(_terranBaselinePlan, race) ||
        (_terranBaselinePlan == TerranPlanId::BioTank &&
         _openingGroup != "bio" &&
         _openingGroup != "anti-rush" &&
         race != BWAPI::Races::Unknown &&
         race != BWAPI::Races::Random))
    {
        const TerranPlanId configuredPlan =
            terranPlanFromName(_openingTerranPlanName);

        _terranBaselinePlan =
            configuredPlan != TerranPlanId::None &&
            terranPlanMatchesEnemyRace(configuredPlan, race)
            ? configuredPlan
            : defaultTerranPlanFor(_openingGroup, race);
    }

    TerranPlanId candidate = _terranBaselinePlan;
    std::string reason = std::string("opening baseline ") +
        terranPlanProfile(candidate).name;

    if (race == BWAPI::Races::Zerg)
    {
        if (const auto* policy = tvzBioPolicy(_terranBaselinePlan))
        {
            const TvZBioPlan::State state = {
                the.now(), _bioFirstAttackFrame,
                the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Vessel),
                enemyHasMutaliskTech() ||
                    enemyHasOpeningThreat(OpeningThreat::ZergTwoHatchLurker) ||
                    enemyHasOpeningThreat(OpeningThreat::HydraliskPressure) ||
                    rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Lurker) > 0 ||
                    rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Lurker_Egg) > 0,
                the.your.seen.count(BWAPI::UnitTypes::Zerg_Hive) > 0 ||
                    the.your.seen.count(BWAPI::UnitTypes::Zerg_Defiler_Mound) > 0 ||
                    the.your.seen.count(BWAPI::UnitTypes::Zerg_Defiler) > 0,
                the.self()->hasResearched(BWAPI::TechTypes::Irradiate)
            };
            const auto next = policy->transition(state);
            if (next != TvZBioPlan::Next::Keep)
            {
                candidate = next == TvZBioPlan::Next::SK
                    ? TerranPlanId::TvZ_SK : TerranPlanId::BioTank;
                reason = std::string(policy->name) + " -> " + terranPlanProfile(candidate).name;
                _terranBaselinePlan = candidate;
                _openingTerranPlanName = terranPlanProfile(candidate).name;
                // Keep an already released Pre-Hive army moving into SK.
                if (next == TvZBioPlan::Next::BioTank)
                {
                    _bioTankFirstPushReleased = false;
                    _bioFirstAttackFrame = -1;
                }
                ProductionManager::Instance().goOutOfBookAndClearQueue();
                switchTerranPlan(candidate, reason, true);
            }
        }
    }

    if (race == BWAPI::Races::Protoss &&
        _terranBaselinePlan == TerranPlanId::TvP_DeepSix)
    {
        // A Robotics Facility alone is not a Reaver. Ordinary Dragoons are
        // exactly what the Marine/Medic switch is intended to punish.
        const bool splashTech =
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Templar_Archives) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_High_Templar) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Reaver) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Protoss_Robotics_Support_Bay) > 0;
        const bool committedDragoonRush =
            enemyHasOpeningThreat(OpeningThreat::ProtossFourGateDragoon) ||
            (actualEarlyGroundRushDetected() &&
             enemyHasOpeningThreat(OpeningThreat::DragoonPressure));
        const bool zealotRush = actualEarlyProtossZealotRushDetected();
        const bool carrierArmy =
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Protoss_Carrier) >= 2;
        const OpeningPlan observedPlan =
            OpponentModel::Instance().getEnemyPlan();
        const bool expansionConfirmed =
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Protoss_Nexus) >= 2 ||
            observedPlan == OpeningPlan::SafeExpand ||
            observedPlan == OpeningPlan::NakedExpand;
        const bool unsuitableEconomy =
            observedPlan == OpeningPlan::Turtle ||
            (ProductionManager::Instance().isOutOfBook() && !expansionConfirmed);

        if (splashTech || committedDragoonRush || zealotRush ||
            carrierArmy || unsuitableEconomy)
        {
            candidate = carrierArmy ? TerranPlanId::TvP_AntiCarrier :
                (splashTech || committedDragoonRush) ? TerranPlanId::TvP_AntiDragoon :
                zealotRush ? TerranPlanId::TvP_AntiZealot : TerranPlanId::TvP_Siege;
            reason = carrierArmy ? "Deep Six cancelled: Carrier army" :
                splashTech ? "Deep Six cancelled: Templar/Reaver tech" :
                committedDragoonRush ? "Deep Six cancelled: committed Dragoon rush" :
                zealotRush ? "Deep Six cancelled: Zealot rush" :
                "Deep Six cancelled: no vulnerable expansion confirmed";

            // Commit the escape so a sticky expansion label cannot restart
            // the bio investment next frame. Preserve construction already
            // in progress, but discard the unstarted opening and rule wave.
            _terranBaselinePlan = candidate;
            _openingTerranPlanName = terranPlanProfile(candidate).name;
            _openingGroup = "tanks";
            _pureAirPlan = false;
            _defaultTankGather = true;
            _tankMassReleaseTanks = terranPlanProfile(candidate).tankRelease;
            _tankMassReleaseGoliaths = terranPlanProfile(candidate).goliathRelease;
            _bioTankFirstPushReleased = false;
            ProductionManager::Instance().goOutOfBookAndClearQueue();
            CombatCommander::Instance().goDefensive();
        }
        else
        {
            candidate = TerranPlanId::TvP_DeepSix;
            reason = expansionConfirmed ? "Deep Six: expanded Protoss" :
                "Deep Six: scouting during Siege Expand";
        }
    }
    else if (race == BWAPI::Races::Protoss)
    {
        if (enemyMode == EnemyArmyMode::PureAir)
        {
            candidate = TerranPlanId::TvP_AntiCarrier;
            reason = enemyHasCarrierTech()
                ? "Carrier/PureAir evidence"
                : "Protoss PureAir evidence";
        }
        else if (_terranBaselinePlan == TerranPlanId::TvP_AntiCarrier &&
            antiCarrierGroundShellReady() &&
            countEnemyHeavyGroundUnits() < 8 &&
            countEnemyLightGroundUnits() < 12 &&
            !actualEarlyGroundRushDetected() &&
            !actualEarlyLightGroundRushDetected() &&
            !actualEarlyProtossZealotRushDetected() &&
            !visibleEnemyGroundNearMainOrClosestExpansion())
        {
            // Resume the opponent-specific AntiCarrier baseline after its
            // four-Tank safety shell. Sticky 3-Gate/Robotics recognition must
            // not hold the plan in AntiDragoon for the rest of the game.
            candidate = TerranPlanId::TvP_AntiCarrier;
            reason = "learned Carrier plan after ground safety shell";
        }
        else if (enemyHasOpeningThreat(OpeningThreat::ProtossFourGateDragoon) ||
            enemyHasOpeningThreat(OpeningThreat::DragoonPressure) ||
            enemyHasOpeningThreat(OpeningThreat::ProtossFastRobotics))
        {
            candidate = TerranPlanId::TvP_AntiDragoon;
            reason = "Dragoon/Robotics pressure";
        }
        else if (enemyHasOpeningThreat(OpeningThreat::ZealotPressure) ||
            actualEarlyProtossZealotRushDetected())
        {
            candidate = TerranPlanId::TvP_AntiZealot;
            reason = "Zealot pressure";
        }
    }
    else if (race == BWAPI::Races::Zerg)
    {
        const bool bioZergBaseline =
            _terranBaselinePlan == TerranPlanId::BioTank &&
            _openingGroup == "bio";
        const bool guardianSeen =
            the.your.seen.count(BWAPI::UnitTypes::Zerg_Guardian) > 0;

        // Marines with Stim and Turrets are a real Mutalisk defense. Keep the
        // selected BioTank plan so ordinary Spire/Hydra/Ling evidence does not
        // silently turn a test Bio opening into the mech macro host.
        if (tvzBioPolicy(_terranBaselinePlan))
        {
            candidate = _terranBaselinePlan;
            reason = std::string(terranPlanProfile(candidate).name) + " bio response";
        }
        else if (bioZergBaseline && !guardianSeen)
        {
            candidate = TerranPlanId::BioTank;
            reason = "BioTank Zerg response";
        }
        else if (enemyMode == EnemyArmyMode::PureAir || enemyHasMutaliskTech())
        {
            candidate = TerranPlanId::TvZ_AntiMuta;
            reason = enemyMode == EnemyArmyMode::PureAir
                ? "Mutalisk air commitment"
                : "Spire/2 Hatch Muta preparation";
        }
        else if (enemyHasOpeningThreat(OpeningThreat::HydraliskPressure) ||
            enemyHasOpeningThreat(OpeningThreat::ZergTwoHatchLurker))
        {
            candidate = TerranPlanId::TvZ_AntiHydra;
            reason = "Hydralisk/Lurker ground pressure";
        }
        else if (enemyHasOpeningThreat(OpeningThreat::ZergTwoHatchLing) ||
            enemyHasOpeningThreat(OpeningThreat::LingPressure))
        {
            candidate = TerranPlanId::TvZ_AntiLing;
            reason = "Zergling pressure";
        }
    }
    else if (race == BWAPI::Races::Terran)
    {
        const bool capitalAirTech =
            the.your.seen.count(BWAPI::UnitTypes::Terran_Physics_Lab) > 0 ||
            the.your.seen.count(BWAPI::UnitTypes::Terran_Battlecruiser) > 0;

        const bool wraithPreparation =
            terranWraithPreparationWindowOpen();

        const bool factoryGroundCommitment =
            enemyHasOpeningThreat(OpeningThreat::TerranTwoFactory) ||
            enemyHasOpeningThreat(OpeningThreat::TerranVultureRush) ||
            enemyTerranTankCoreDetected() ||
            enemyTerranHeavyMechDetected() ||
            preparationPlan == OpeningPlan::VultureRush ||
            preparationPlan == OpeningPlan::Factory;

        // A clear ground-only Tank transition must be able to leave the sticky
        // opening Wraith label. If air is still meaningful, Mixed remains on
        // the AntiAir plan, whose profile now retains its own Tank core.
        if (factoryGroundCommitment && enemyMode == EnemyArmyMode::HeavyGround)
        {
            candidate = TerranPlanId::TvT_Anti2Factory;
            reason = "ground-only Factory/Tank commitment";
        }
        else if (wraithPreparation ||
            (enemyMode == EnemyArmyMode::PureAir && capitalAirTech))
        {
            candidate = TerranPlanId::TvT_AntiAir;
            reason = directTerranWraithEvidence()
                ? "Wraith/2 Starport evidence"
                : capitalAirTech
                ? "Physics Lab/Battlecruiser"
                : "learned Wraith preparation";
        }
        else if (factoryGroundCommitment)
        {
            candidate = TerranPlanId::TvT_Anti2Factory;
            reason = preparationPlan == OpeningPlan::Factory &&
                OpponentModel::Instance().getEnemyPlan() == OpeningPlan::Unknown
                ? "70% predicted Factory preparation"
                : "2 Factory/heavy mech";
        }
    }

    // BioTank owns its reactions for every matchup. A tactical air or ground
    // classification must not silently move this JSON opening to the mech
    // host and start the Goliath/Vulture production rules.
    if (_terranBaselinePlan == TerranPlanId::BioTank &&
        _openingGroup == "bio")
    {
        candidate = TerranPlanId::BioTank;
        reason = "BioTank counter composition";
    }

    if (_terranBaselinePlan == TerranPlanId::AirTanks)
    {
        candidate = TerranPlanId::AirTanks;
        reason = "AirTanks counter composition";
    }

    if (candidate != _terranPlan)
    {
        switchTerranPlan(candidate, reason, true);
    }

    // The opening book is allowed to finish in its native identity. Once it is
    // over, the Plan owns the macro host (Bio or Mech). This is what makes a
    // plan selected during scouting become real production policy without
    // destructively clearing a still-valid JSON opening.
    const TerranPlanProfile & activePlan = terranPlanProfile(_terranPlan);
    if (ProductionManager::Instance().isOutOfBook() &&
        activePlan.hostOpeningGroup[0] != '\0' &&
        _openingGroup != activePlan.hostOpeningGroup)
    {
        const std::string oldHost = _openingGroup;
        _openingGroup = activePlan.hostOpeningGroup;
        _defaultTankGather = _openingGroup == "tanks";
        _tankMassReleaseTanks = activePlan.tankRelease;
        _tankMassReleaseGoliaths = activePlan.goliathRelease;
        _bioTankFirstPushReleased = false;

        BOSSManager::Instance().reset();
        ProductionManager::Instance().invalidateTerranRulePlan(
            std::string("plan host ") + oldHost + " -> " + _openingGroup,
            true);

        BWAPI::Broodwar->printf(
            "Terran plan host: %s -> %s (%s)",
            oldHost.c_str(),
            _openingGroup.c_str(),
            activePlan.name);
    }
}

const MetaPairVector StrategyManager::getTerranBuildOrderGoal()
{
    _slowestGoalStage = "";
    _slowestGoalStageMs = 0.0;
    // Include normalization and early returns in the plan's detailed timing.
    struct GoalStageTimer
    {
        const char *& slowestName;
        double& slowestMs;
        const char * currentName = "plan identity";
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        GoalStageTimer(const char *& name, double& ms)
            : slowestName(name), slowestMs(ms) {}

        void next(const char * name)
        {
            const auto now = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - start).count();
            if (ms > slowestMs)
            {
                slowestName = currentName;
                slowestMs = ms;
            }
            currentName = name;
            start = now;
        }

        ~GoalStageTimer() { next(""); }
    } stage(_slowestGoalStage, _slowestGoalStageMs);
    // Refresh the long-lived macro identity before deriving this rule wave.
    // The selected profile is deliberately stable; EnemyArmyMode below remains
    // the short-term tactical modifier.
    updateTerranPlan();
    const TerranPlanProfile & terranPlan = terranPlanProfile(_terranPlan);
    if (airTanksPlanActive())
    {
        stage.next("AirTanks independent plan");
        const int desiredGas = desiredRefineryCountForOwnedTerranBases();
        if (ownedCompletedRefineryNeedsWorkers()) WorkerManager::Instance().setCollectGas(true);
        return AirTanks::productionGoals(shouldExpandNow(), desiredGas,
            WorkerManager::Instance().needsExpansionFerry(), terranMineralExpansionEmergency(),
            visibleEnemyGroundNearMainOrClosestExpansion() || actualEarlyGroundRushDetected());
    }
    const auto* bioPolicy = tvzBioPolicy(_terranPlan);
    stage.next("threats");

    // the goal to return
    std::vector<MetaPair> goal;

    // These counts include uncompleted units.
    int numSCVs			= the.my.all.count(BWAPI::UnitTypes::Terran_SCV);
    int numCC           = the.my.all.count(BWAPI::UnitTypes::Terran_Command_Center);            
    int numRefineries   = the.my.all.count(BWAPI::UnitTypes::Terran_Refinery);    
    const int desiredRefineries =
        desiredRefineryCountForOwnedTerranBases();


    if (numRefineries < desiredRefineries || ownedCompletedRefineryNeedsWorkers())
    {
        WorkerManager::Instance().setCollectGas(true);
    }
    int numMarines      = the.my.all.count(BWAPI::UnitTypes::Terran_Marine);
    int numMedics       = the.my.all.count(BWAPI::UnitTypes::Terran_Medic);
    int numVultures     = the.my.all.count(BWAPI::UnitTypes::Terran_Vulture);
    int numVessels		= the.my.all.count(BWAPI::UnitTypes::Terran_Science_Vessel);

    const int MAX_SCIENCE_VESSELS = 2;

    // the.my.all already includes a unit currently being trained.
    const int plannedScienceVessels = numVessels;
    int completedVessels =
        the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Vessel);
    int numGoliaths		= the.my.all.count(BWAPI::UnitTypes::Terran_Goliath);
    int numTanks        = the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
                        + the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
    bool hasEBay		= the.my.completed.count(BWAPI::UnitTypes::Terran_Engineering_Bay) > 0;
    bool hasAcademy		= the.my.completed.count(BWAPI::UnitTypes::Terran_Academy) > 0;
    const int numAcademyAll =
        the.my.all.count(BWAPI::UnitTypes::Terran_Academy);

    const int numCCCompleted =
        the.my.completed.count(BWAPI::UnitTypes::Terran_Command_Center);

    const int numComsatAll =
        the.my.all.count(BWAPI::UnitTypes::Terran_Comsat_Station);
    const BWAPI::Race enemyRace =
        BWAPI::Broodwar->enemy()->getRace();
    const EnemyThreatMask enemyThreatMask =
        currentEnemyThreatMask();
    const bool currentGroundAirThreat =
        HasEnemyThreat(enemyThreatMask, EnemyThreat::GroundAir);
    const bool currentSpellcasterThreat =
        HasEnemyThreat(enemyThreatMask, EnemyThreat::Spellcaster);
    const bool currentCloakThreat =
        HasEnemyThreat(enemyThreatMask, EnemyThreat::Cloak);
    const bool currentCapitalAirThreat =
        HasEnemyThreat(enemyThreatMask, EnemyThreat::CapitalAir);
    bool makeVessel =
        InformationManager::Instance().enemyHasCloakTech() ||
        enemyHasOpeningThreat(OpeningThreat::CloakTech) ||
        currentCloakThreat || bioTankLurkerTankTarget() > 0;
    bool hasArmory = the.my.all.count(BWAPI::UnitTypes::Terran_Armory) > 0;
    bool hasFactory = the.my.completed.count(BWAPI::UnitTypes::Terran_Factory) > 0;
    const EnemyArmyMode enemyMode =
        currentEnemyArmyMode();
    const bool enemyAirThreatDetected = enemyModeHasAir(enemyMode);
    const bool enemyFlyerSeen = enemyFlyingUnitEverSeen();
    const bool airIntrusionSeen =
        InformationManager::Instance().enemyHasTransport() ||
        enemyFlyingBuildingEverSeen();
    const bool carrierTechThreat = enemyHasCarrierTech();

    const bool currentPureAirCommitment =
        enemyMode == EnemyArmyMode::PureAir;

    const bool learnedAntiCarrierCommitment =
        !currentPureAirCommitment &&
        selectedTvPAntiCarrierPlanCanCommit(enemyMode);

    const bool bioHandlesZergAir =
        _openingGroup == "bio" &&
        enemyRace == BWAPI::Races::Zerg &&
        the.your.seen.count(BWAPI::UnitTypes::Zerg_Guardian) == 0;

    const bool activePureAirCommitment =
        _openingGroup != "bio" && !deepSixPlanActive() &&
        ((currentPureAirCommitment && !bioHandlesZergAir) ||
         learnedAntiCarrierCommitment);

    // Current Carrier evidence and a UCB-selected AntiCarrier plan both own
    // the full PureAir composition. The predictive path is cancelled by
    // selectedTvPAntiCarrierPlanCanCommit() as soon as ground danger appears.
    _pureAirPlan = activePureAirCommitment;

    const bool learnedPureAirExpectation =
        learnedPureAirExpectedAfterOpening();

    const bool learnedMixedExpectation =
        learnedMixedExpectedAfterOpening();

    const bool learnedGroundAirPreparation =
        learnedEnemyThreatExpectedAfterOpening(EnemyThreat::GroundAir) &&
        !activePureAirCommitment;

    const bool learnedCapitalAirPreparation =
        learnedEnemyThreatExpectedAfterOpening(EnemyThreat::CapitalAir) &&
        !activePureAirCommitment;

    const bool learnedSpellcasterPreparation =
        learnedEnemyThreatExpectedAfterOpening(EnemyThreat::Spellcaster);

    const bool learnedCloakPreparation =
        learnedEnemyThreatExpectedAfterOpening(EnemyThreat::Cloak);

    // Generic PureAir history remains a cautious post-opening prior. The more
    // precise UCB TvP_AntiCarrier choice is handled above as a full commitment;
    // this broad class signal still receives only a bounded preparation package.
    const bool learnedPureAirPreparation =
        learnedPureAirExpectation &&
        !activePureAirCommitment;

    const bool learnedMixedPreparation =
        learnedMixedExpectation &&
        !activePureAirCommitment;

    // A UCB-selected TvZ_AntiMuta Plan is already opponent-specific evidence.
    // Treat it as permission for a small, reversible AA preparation package;
    // the full no-Tank/no-Vulture PureAir conversion still requires current-
    // game Spire/Mutalisk evidence below.
    const bool selectedTvZAntiMutaPreparation =
        enemyRace == BWAPI::Races::Zerg &&
        _terranPlan == TerranPlanId::TvZ_AntiMuta &&
        !activePureAirCommitment;

    const bool selectedTvTAntiAirPreparation =
        enemyRace == BWAPI::Races::Terran &&
        terranWraithPreparationWindowOpen() &&
        !activePureAirCommitment;

    // Bio + PureAir has one macro host. Current air evidence or a selected
    // AntiCarrier UCB arm transfers the match to Goliaths + Ghosts. Generic
    // PureAir history still starts only the bounded preparation package.
    // Bio keeps its Marine/Tank host when air is detected; its own anti-air
    // layer is assembled below instead of inheriting the mech PureAir plan.
    const NearbyEnemyThreats& nearbyEnemyThreats =
        visibleEnemyThreatsNearOwnedBases();
    const bool nearbyAirOnlyAttack =
        nearbyEnemyThreats.airOnly();

    const bool protossCarrierPlan =
        enemyRace == BWAPI::Races::Protoss &&
        carrierTechThreat;

    // A learned AntiCarrier commitment must use the same Ghost, dual-Armory,
    // Engineering Bay and turret plan as a currently scouted Carrier. The
    // only difference is that current-game evidence is not fabricated.
    const bool predictiveProtossCarrierPlan =
        enemyRace == BWAPI::Races::Protoss &&
        learnedAntiCarrierCommitment;

    const bool activeProtossCarrierCounter =
        protossCarrierPlan || predictiveProtossCarrierPlan;

    const bool protossGhostTechPlan =
        enemyRace == BWAPI::Races::Protoss &&
        (
            carrierTechThreat ||
            enemyMode == EnemyArmyMode::PureAir ||
            _pureAirPlan ||
            (learnedCapitalAirPreparation && numTanks >= 4)
            );

    const bool terranCapitalAirGhostPlan =
        enemyRace == BWAPI::Races::Terran &&
        (currentCapitalAirThreat ||
         (learnedCapitalAirPreparation && (numTanks >= 4 || numCC >= 2)));

    const bool enemyMixedAirGround =
        enemyMode == EnemyArmyMode::MixedAirHeavyGround ||
        enemyMode == EnemyArmyMode::MixedAirLightGround;

    const bool protossNonCarrierAirPlan =
        enemyRace == BWAPI::Races::Protoss &&
        !carrierTechThreat &&
        enemyMode == EnemyArmyMode::PureAir;

    const bool genericAirPlan =
        enemyRace != BWAPI::Races::Protoss &&
        enemyAirThreatDetected &&
        enemyMode == EnemyArmyMode::PureAir;

    const bool mixedAirGroundPlan =
        enemyMode == EnemyArmyMode::MixedAirHeavyGround ||
        enemyMode == EnemyArmyMode::MixedAirLightGround;

    const bool enemyBioTankPush =
        enemyTerranBioTankPushDetected();

    int maxSCVs = WorkerManager::Instance().getMaxWorkers();

    BWAPI::Player self = the.self();
    stage.next("economy");

    // Immediate ground danger is expressed as part of the strategic goal,
    // not only as queue surgery. This is especially important after an
    // opening is abandoned for a PureAir response: a simultaneous proxy or
    // zealot/ling/worker rush must remain visible to the new goal plan.
    const OpeningPlan recognizedOpeningPlan =
        OpponentModel::Instance().getEnemyPlan();

    const OpeningPlan likelyOpeningPlan =
        OpponentModel::Instance().getPreparationEnemyPlan();

    const bool rushOpeningPlan =
        recognizedOpeningPlan == OpeningPlan::WorkerRush ||
        recognizedOpeningPlan == OpeningPlan::Proxy ||
        recognizedOpeningPlan == OpeningPlan::FastRush ||
        recognizedOpeningPlan == OpeningPlan::HeavyRush ||
        likelyOpeningPlan == OpeningPlan::WorkerRush ||
        likelyOpeningPlan == OpeningPlan::Proxy ||
        likelyOpeningPlan == OpeningPlan::FastRush ||
        likelyOpeningPlan == OpeningPlan::HeavyRush;

    const bool directGroundEmergency =
        enemyOpeningGroundPressure() ||
        actualEarlyProtossZealotRushDetected() ||
        actualEarlyGroundRushDetected() ||
        actualEarlyLightGroundRushDetected();

    const bool earlyGroundDefensePlan =
        !activePureAirCommitment &&
        (directGroundEmergency ||
         (the.now() <= 10 * 24 * 60 && rushOpeningPlan));

    // Place BioTank's Medic deficit before every Marine source, including the
    // early ground-defense rule below. Goal normalization merges duplicate
    // Marine requests at their FIRST position, so adding the Medic only inside
    // the later Bio block still let an emergency Marine rule take every Rax.
    if (_openingGroup == "bio" && hasAcademy && numMarines >= 5)
    {
        const int medicGoal = BioProductionPolicy::medicTarget(numMarines,
            the.my.all.count(BWAPI::UnitTypes::Terran_Firebat),
            the.my.all.count(BWAPI::UnitTypes::Terran_Ghost));
        if (numMedics < medicGoal)
        {
            goal.push_back(MetaPair(
                BWAPI::UnitTypes::Terran_Medic, medicGoal));
        }
    }

    if (_openingGroup == "tanks")
    {
        const TerranInfantryDefense defense = terranInfantryDefense();
        if (defense.bunkers > 0) goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Bunker, defense.bunkers));
        if (defense.marines > numMarines) goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Marine, defense.marines));
    }
    else if (earlyGroundDefensePlan)
    {
        const bool severeRush =
            recognizedOpeningPlan == OpeningPlan::WorkerRush ||
            recognizedOpeningPlan == OpeningPlan::Proxy ||
            recognizedOpeningPlan == OpeningPlan::HeavyRush ||
            likelyOpeningPlan == OpeningPlan::WorkerRush ||
            likelyOpeningPlan == OpeningPlan::Proxy ||
            likelyOpeningPlan == OpeningPlan::HeavyRush ||
            actualEarlyGroundRushDetected();

        goal.push_back(MetaPair(
            BWAPI::UnitTypes::Terran_Bunker,
            severeRush ? 2 : 1
        ));

        goal.push_back(MetaPair(
            BWAPI::UnitTypes::Terran_Marine,
            std::max(numMarines, severeRush ? 8 : 6)
        ));
    }

    const bool commandCenterAlreadyStarted =
        BuildingManager::Instance().isBeingBuilt(
            BWAPI::UnitTypes::Terran_Command_Center);
    const bool mineralExpansionEmergency =
        terranMineralExpansionEmergency() &&
        the.map.getNextExpansion(false, true, false).isValid();
    bool expansionGoalAdded = false;
    if (mineralExpansionEmergency && !commandCenterAlreadyStarted)
    {
        // Reserve the next mineral line before optional tech, SCVs, Factory
        // scaling or the recurring army wave can spend the last 400 minerals.
        goal.push_back(MetaPair(
            BWAPI::UnitTypes::Terran_Command_Center, numCC + 1));
        expansionGoalAdded = true;
    }

    // Gas on an already owned base is economy, not optional Factory scaling.
    // Put this fixed target before recurring SCV/army waves and normal extra
    // bases/producers. Immediate rush defense and a mineral-line emergency
    // retain their earlier slots; the JSON opening keeps its own ordering.
    if ((_openingGroup == "bio" || _openingGroup == "tanks") &&
        numRefineries < desiredRefineries)
    {
        goal.push_back(MetaPair(
            BWAPI::UnitTypes::Terran_Refinery, desiredRefineries));
    }

    // Detection must remain available against cloak, but paying for an Academy
    // before the natural in every game slows the first economy/army cycle.
    // The six-minute fallback still covers an unseen tech switch.
    const bool earlyDetectionNeeded =
        makeVessel || learnedCloakPreparation ||
        the.now() >= 6 * 24 * 60 || numCC >= 2;
    if (numCC > 0 && numAcademyAll == 0 && earlyDetectionNeeded)
    {
        goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Academy, 1));
    }

    if (hasAcademy && numCCCompleted > 0 && numComsatAll < numCCCompleted)
    {
        goal.push_back(MetaPair(
            BWAPI::UnitTypes::Terran_Comsat_Station,
            numCCCompleted
        ));
    }

    // Last-resort island cleanup is a temporary strategic plan of its own.
    // Stop adding normal economy and army waves, complete exactly one
    // transport chain and let CombatCommander load available ground attackers.
    if (!_pureAirPlan && CombatCommander::Instance().islandCleanupDropNeeded())
    {
        goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, 1));
        goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Control_Tower, 1));
        goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Dropship, 1));
        stage.next("normalize");
        return normalizeTerranBuildOrderGoal(goal);
    }

    // The first SCVs for a flown expansion need a transport while the enemy
    // still holds our exit. Put this chain before recurring army waves so the
    // empty Command Center does not wait behind a long production goal.
    if (WorkerManager::Instance().needsExpansionFerry() &&
        the.my.all.count(BWAPI::UnitTypes::Terran_Dropship) == 0)
    {
        goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, 1));
        goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Control_Tower, 1));
        goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Dropship, 1));
    }

    // Economy is an independent production lane. Put SCVs before the endless
    // army waves so a free Command Center can train while Barracks and
    // Factories continue producing. The former late goal was refreshed behind
    // new combat-unit targets every second and was therefore often never
    // reached (for example 25/27 SCVs on two bases at 12:00).
    //
    // Two SCVs per completed Command Center is a bounded wave rather than one
    // enormous BOSS target. The persistent executor refreshes it once per
    // second and getProducer() distributes the jobs among available bases.
    if (_openingGroup != "anti-rush" &&
        numCCCompleted > 0 &&
        numSCVs < maxSCVs)
    {
        const int scvsAddedThisGoal =
            2 * numCCCompleted;

        goal.push_back(MetaPair(
            BWAPI::UnitTypes::Terran_SCV,
            std::min(maxSCVs, numSCVs + scvsAddedThisGoal)
        ));
    }

    // Expansions are also economic infrastructure, not a leftover mineral
    // dump. Queue the safe timer/saturation decision before recurring army
    // targets. The threat gate in shouldExpandNow() still delays a greedy
    // third against a recognized committed opening attack.
    // Expansion is an economy lane, not a global production barrier. Remember
    // the decision now, but append the Command Center after the strategy's
    // immediate producer lanes. If Factories/Barracks are free they get one
    // job first; when they are busy, the same rule pass can invest the surplus
    // into the expansion.
    const int currentMinute = the.now() / (24 * 60);
    const bool planAllowsExpansion =
        !(bioPolicy && bioPolicy->oneBaseTiming) &&
        (numCC < 2 ||
         terranPlan.earliestThirdMinute <= 0 ||
         currentMinute >= terranPlan.earliestThirdMinute);

    const bool wallTankCoreReadyForExpansion =
        !the.placer.hasTerranMainWall() ||
        _openingGroup != "tanks" ||
        numTanks >= 4;

    const bool wantTerranExpansion =
        !commandCenterAlreadyStarted &&
        (mineralExpansionEmergency ||
         (planAllowsExpansion && wallTankCoreReadyForExpansion)) &&
        shouldExpandNow();

    // ProductionManager keeps the verified P/Z wall as a temporary gate until
    // the natural is committed. The same lift which releases that builder then
    // permanently transfers the wall Barracks to Tank-vision duty.

    stage.next("composition");
    if (_openingGroup == "anti-rush")
    {
        int numRax = the.my.all.count(BWAPI::UnitTypes::Terran_Barracks);

        CombatCommander::Instance().setAggression(false);
        
        goal.push_back(std::pair<MacroAct, int>(BWAPI::UnitTypes::Terran_Marine, numMarines + numRax));
        goal.push_back(std::pair<MacroAct, int>(BWAPI::UnitTypes::Terran_SCV, std::min(maxSCVs, numSCVs + 1)));
        goal.push_back(std::pair<MacroAct, int>(BWAPI::UnitTypes::Terran_Bunker, 1));
        
        // If we survived long enough, transition to something more interesting.
        if (numMarines >= 10)
        {
            _openingGroup = "bio";
            CombatCommander::Instance().setAggression(true);
        }
    }
    else if (_openingGroup == "bio")
    {
        // BioTank owns its matchup reactions and keeps Marine/Medic/Siege as
        // its core instead of entering the mech Goliath composition.
        _pureAirPlan = false;
        _defaultTankGather = false;
        _tankMassReleaseTanks = 0;
        _tankMassReleaseGoliaths = 0;

        const BuildOrderQueue& queue =
            ProductionManager::Instance().getQueue();

        const int completedBases =
            the.bases.completedBaseCount(the.self());

        const int currentBarracks =
            the.my.all.count(BWAPI::UnitTypes::Terran_Barracks) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Barracks) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Barracks);

        const bool deepSix = deepSixPlanActive();
        const bool ordinaryBioTank = _terranPlan == TerranPlanId::BioTank;
        const auto& bioEnemy = BioTankPolicy::enemyMix();
        const int counterValkyries = ordinaryBioTank && completedBases >= 2 &&
            numMarines >= 16
            ? BioTankPolicy::valkyries(bioEnemy.mutalisks,
                bioEnemy.guardians, bioEnemy.devourers) : 0;
        // Once both prerequisites are ready, fund the anti-air Starport lane
        // ahead of recurring Marine/Tank waves and optional upgrades.
        if (counterValkyries > 0 &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Control_Tower) > 0 &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Armory) > 0)
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Valkyrie,
                std::min(counterValkyries,
                    the.my.all.count(BWAPI::UnitTypes::Terran_Valkyrie) + 1)));
        int desiredBarracks = deepSix && completedBases < 2
            ? 2 : std::max(2, terranPlan.minBarracks);
        if (completedBases >= 2)
        {
            desiredBarracks = std::max(desiredBarracks,
                2 * std::min(completedBases, 5));
        }

        const bool bioBankPressure = terranBankedArmyFillPressure();
        desiredBarracks = BioProductionPolicy::barracksTarget(
            desiredBarracks, numSCVs, self->minerals(),
            the.my.completed.count(BWAPI::UnitTypes::Terran_Barracks),
            bioBankPressure);
        if (bioPolicy) desiredBarracks = bioPolicy->barracks(completedBases);

        if (currentBarracks < desiredBarracks)
        {
            goal.push_back(MetaPair(
                BWAPI::UnitTypes::Terran_Barracks,
                desiredBarracks
            ));
        }

        if (bioPolicy)
        {
            TvZBioPlan::appendInfantryTechGoals(goal, *bioPolicy, queue);
        }

        if (bioPolicy && bioPolicy->push.vessels > 0 && numMarines >= 8)
        {
            TvZBioPlan::appendSupportGoals(goal, *bioPolicy, queue, numMarines,
                completedBases, numVessels, scienceVesselsCurrentlyTraining());
        }

        const bool urgentBioAirCounter = ordinaryBioTank && numTanks >= 2 &&
            numMarines >= 16 && numGoliaths < BioTankPolicy::goliathTarget();
        const bool earlyGoliathGoal = urgentBioAirCounter &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Armory) > 0;
        if (earlyGoliathGoal)
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Goliath,
                std::min(BioTankPolicy::goliathTarget(), numGoliaths + 2)));
        const int marinesAddedPerGoal =
            std::max(6, std::min(12, 3 * std::max(1, desiredBarracks)));
        // Do not refill every free slot with a Marine while two needed
        // anti-air units wait for enough supply to train.
        const bool reserveCounterSupply = BioTankPolicy::reserveGoliathSupply();
        if (!reserveCounterSupply)
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Marine,
                numMarines + marinesAddedPerGoal));

        const int academiesPlanned =
            the.my.all.count(BWAPI::UnitTypes::Terran_Academy) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Academy) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Academy);
        if (numMarines >= 4 && academiesPlanned == 0)
        {
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Academy, 1));
        }

        if (hasAcademy)
        {
            if (!self->hasResearched(BWAPI::TechTypes::Stim_Packs) &&
                !self->isResearching(BWAPI::TechTypes::Stim_Packs) &&
                !techInQueue(ProductionManager::Instance().getQueue(), BWAPI::TechTypes::Stim_Packs))
            {
                goal.push_back(MetaPair(BWAPI::TechTypes::Stim_Packs, 1));
            }
            else if ((!bioPolicy || bioPolicy->push.range) &&
                self->hasResearched(BWAPI::TechTypes::Stim_Packs) &&
                self->getUpgradeLevel(BWAPI::UpgradeTypes::U_238_Shells) == 0 &&
                !self->isUpgrading(BWAPI::UpgradeTypes::U_238_Shells) &&
                !queue.anyInQueue(BWAPI::UpgradeTypes::U_238_Shells))
            {
                goal.push_back(MetaPair(BWAPI::UpgradeTypes::U_238_Shells, 1));
            }

            if (enemyRace == BWAPI::Races::Zerg && numMarines >= 8 &&
                !(bioPolicy && bioPolicy->oneBaseTiming))
            {
                const int numFirebats =
                    the.my.all.count(BWAPI::UnitTypes::Terran_Firebat);

                const int firebatGoal = ordinaryBioTank
                    ? BioTankPolicy::firebats(numMarines, bioEnemy.zerglings)
                    : std::max(numFirebats, std::min(4, numMarines / 8));

                if (firebatGoal > numFirebats)
                {
                    goal.push_back(MetaPair(
                        BWAPI::UnitTypes::Terran_Firebat,
                        firebatGoal
                    ));
                }
            }
        }

        const int currentFactories =
            the.my.all.count(BWAPI::UnitTypes::Terran_Factory) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Factory) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Factory);

        int desiredFactories = std::max(1, terranPlan.minFactories);
        if (completedBases >= 2)
        {
            desiredFactories = 2;
        }

        const int completedFactories =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Factory);
        desiredFactories = BioProductionPolicy::factoryTarget(
            desiredFactories, completedFactories, numSCVs,
            self->minerals(), self->gas(), bioBankPressure,
            completedFactories > 0 &&
                countIdleTerranProducers(BWAPI::UnitTypes::Terran_Factory) == 0);

        if (ordinaryBioTank)
        {
            const int factoryArmyTarget = bioTankTankCeiling() +
                BioTankPolicy::goliathTarget(carrierTechThreat);
            desiredFactories = BioTankPolicy::factories(factoryArmyTarget,
                completedFactories, numSCVs, self->minerals(), self->gas(),
                completedFactories > 0 &&
                countIdleTerranProducers(BWAPI::UnitTypes::Terran_Factory) == 0);
        }
        if (bioPolicy) desiredFactories = bioPolicy->factories;

        if (currentFactories < desiredFactories)
        {
            goal.push_back(MetaPair(
                BWAPI::UnitTypes::Terran_Factory,
                desiredFactories
            ));
        }

        const int currentMachineShops =
            the.my.all.count(BWAPI::UnitTypes::Terran_Machine_Shop) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Machine_Shop) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Machine_Shop);

        const int desiredMachineShops = bioPolicy
            ? bioPolicy->machineShops
            : ordinaryBioTank
                ? std::min(desiredFactories, std::max(1, (bioTankTankCeiling() + 3) / 4))
                : std::max(desiredFactories, std::min(4, currentFactories));

        if (currentMachineShops < desiredMachineShops)
        {
            goal.push_back(MetaPair(
                BWAPI::UnitTypes::Terran_Machine_Shop,
                desiredMachineShops
            ));
        }

        const bool fundCapitalAirCounter = ordinaryBioTank && numTanks >= 2 &&
            numGoliaths < BioTankPolicy::goliathTarget();
        if (!fundCapitalAirCounter && numTanks +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) <
            bioTankTankCeiling())
        {
            goal.push_back(MetaPair(
                BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode,
                std::min(numTanks + 1, bioTankTankCeiling())));
        }

        if (bioTankUsesTanks() &&
            !self->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode) &&
            !self->isResearching(BWAPI::TechTypes::Tank_Siege_Mode) &&
            !techInQueue(ProductionManager::Instance().getQueue(), BWAPI::TechTypes::Tank_Siege_Mode))
        {
            goal.push_back(MetaPair(BWAPI::TechTypes::Tank_Siege_Mode, 1));
        }

        const int infantryWeapons =
            self->getUpgradeLevel(BWAPI::UpgradeTypes::Terran_Infantry_Weapons);

        const int infantryArmor =
            self->getUpgradeLevel(BWAPI::UpgradeTypes::Terran_Infantry_Armor);

        const bool infantryUpgradesNeedWork =
            infantryWeapons < self->getMaxUpgradeLevel(BWAPI::UpgradeTypes::Terran_Infantry_Weapons) ||
            infantryArmor < self->getMaxUpgradeLevel(BWAPI::UpgradeTypes::Terran_Infantry_Armor);

        const int engineeringBaysPlanned =
            the.my.all.count(BWAPI::UnitTypes::Terran_Engineering_Bay) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Engineering_Bay) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Engineering_Bay);

        const int desiredEngineeringBays =
            infantryUpgradesNeedWork && completedBases >= 2 ? 2 : 1;

        if (numMarines >= 8 && engineeringBaysPlanned < desiredEngineeringBays &&
            !(bioPolicy && bioPolicy->oneBaseTiming))
        {
            goal.push_back(MetaPair(
                BWAPI::UnitTypes::Terran_Engineering_Bay,
                desiredEngineeringBays
            ));
        }

        const bool prepareAdvancedInfantryEarly = bioBankPressure &&
            hasEBay && self->minerals() >= 1000 && self->gas() >= 600;
        const bool advancedInfantryTechWanted =
            completedBases >= 2 &&
            numMarines >= 12 &&
            (numTanks >= 3 || (bioPolicy && bioPolicy->push.vessels > 0)) &&
            ((infantryWeapons >= 1 && infantryArmor >= 1) ||
                prepareAdvancedInfantryEarly) &&
            infantryUpgradesNeedWork;

        if (advancedInfantryTechWanted)
        {
            const bool starportExistsOrPlanned =
                the.my.all.count(BWAPI::UnitTypes::Terran_Starport) > 0 ||
                BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Starport) > 0 ||
                queue.anyInQueue(BWAPI::UnitTypes::Terran_Starport);

            if (!starportExistsOrPlanned)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, 1));
            }

            const bool scienceFacilityExistsOrPlanned =
                the.my.all.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0 ||
                BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Science_Facility) > 0 ||
                queue.anyInQueue(BWAPI::UnitTypes::Terran_Science_Facility);

            if (the.my.completed.count(BWAPI::UnitTypes::Terran_Starport) > 0 &&
                !scienceFacilityExistsOrPlanned)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Science_Facility, 1));
            }
        }

        if (hasEBay && !(bioPolicy && bioPolicy->oneBaseTiming))
        {
            int idleEngineeringBays = 0;
            for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
            {
                if (unit &&
                    unit->exists() &&
                    unit->isCompleted() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Engineering_Bay &&
                    !unit->isUpgrading())
                {
                    ++idleEngineeringBays;
                }
            }

            int queuedEngineeringBayUpgrades = 0;
            if (queue.anyInQueue(BWAPI::UpgradeTypes::Terran_Infantry_Weapons))
            {
                ++queuedEngineeringBayUpgrades;
            }
            if (queue.anyInQueue(BWAPI::UpgradeTypes::Terran_Infantry_Armor))
            {
                ++queuedEngineeringBayUpgrades;
            }

            int availableEngineeringBaySlots =
                std::max(0, idleEngineeringBays - queuedEngineeringBayUpgrades);

            const bool hasCompletedScienceFacility =
                the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0;

            auto queueInfantryUpgradeIfPossible =
                [&](BWAPI::UpgradeType upgrade) -> bool
                {
                    if (availableEngineeringBaySlots <= 0 ||
                        self->isUpgrading(upgrade) ||
                        queue.anyInQueue(upgrade))
                    {
                        return false;
                    }

                    const int currentLevel =
                        self->getUpgradeLevel(upgrade);

                    const int maxLevel =
                        self->getMaxUpgradeLevel(upgrade);

                    if (currentLevel >= maxLevel)
                    {
                        return false;
                    }

                    const int targetLevel = currentLevel + 1;

                    // Infantry levels 2 and 3 require a completed Science Facility.
                    if (targetLevel >= 2 && !hasCompletedScienceFacility)
                    {
                        return false;
                    }

                    goal.push_back(MetaPair(upgrade, targetLevel));
                    --availableEngineeringBaySlots;
                    return true;
                };

            // Deep Six needs +1 armor for its first timing. Once that is
            // secured, keep both upgrade lines progressing together.
            if (deepSix && infantryArmor == 0)
            {
                queueInfantryUpgradeIfPossible(BWAPI::UpgradeTypes::Terran_Infantry_Armor);
                queueInfantryUpgradeIfPossible(BWAPI::UpgradeTypes::Terran_Infantry_Weapons);
            }
            else if (infantryWeapons <= infantryArmor)
            {
                queueInfantryUpgradeIfPossible(BWAPI::UpgradeTypes::Terran_Infantry_Weapons);
                queueInfantryUpgradeIfPossible(BWAPI::UpgradeTypes::Terran_Infantry_Armor);
            }
            else
            {
                queueInfantryUpgradeIfPossible(BWAPI::UpgradeTypes::Terran_Infantry_Armor);
                queueInfantryUpgradeIfPossible(BWAPI::UpgradeTypes::Terran_Infantry_Weapons);
            }
        }

        // Keep each counter tied to the fleet it can efficiently fight.
        // Do not buy capital-air counters for an Overlord or a dead fleet.
        const int guardianWraiths = BioTankPolicy::guardianWraiths(
            bioEnemy.guardians, bioEnemy.antiAir);
        const int counterGoliaths = !bioPolicy
            ? BioTankPolicy::goliathTarget(carrierTechThreat) : 0;
        const bool needsArmory = numTanks >= 3 || numGoliaths > 0 ||
            counterGoliaths > 0 || counterValkyries > 0;
        const bool armoryExistsOrPlanned =
            the.my.all.count(BWAPI::UnitTypes::Terran_Armory) > 0 ||
            BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Armory) > 0 ||
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Armory);
        if (needsArmory && !armoryExistsOrPlanned)
            goal.push_back(MetaPair(
                MacroAct(BWAPI::UnitTypes::Terran_Armory, MacroLocation::Main), 1));

        if (!earlyGoliathGoal && counterGoliaths > numGoliaths &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Armory) > 0)
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Goliath,
                std::min(counterGoliaths, numGoliaths + 2)));
        if (counterGoliaths > 0 &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Machine_Shop) > 0 &&
            self->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) == 0 &&
            !self->isUpgrading(BWAPI::UpgradeTypes::Charon_Boosters) &&
            !queue.anyInQueue(BWAPI::UpgradeTypes::Charon_Boosters))
            goal.push_back(MetaPair(BWAPI::UpgradeTypes::Charon_Boosters, 1));

        if (!bioPolicy && (guardianWraiths > 0 || counterValkyries > 0))
        {
            const bool portPlanned = the.my.all.count(BWAPI::UnitTypes::Terran_Starport) > 0 ||
                BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Starport) > 0 ||
                queue.anyInQueue(BWAPI::UnitTypes::Terran_Starport);
            if (!portPlanned) goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, 1));
            const bool towerPlanned = the.my.all.count(BWAPI::UnitTypes::Terran_Control_Tower) > 0 ||
                BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Control_Tower) > 0 ||
                queue.anyInQueue(BWAPI::UnitTypes::Terran_Control_Tower);
            if (the.my.completed.count(BWAPI::UnitTypes::Terran_Starport) > 0 && !towerPlanned)
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Control_Tower, 1));
            if (the.my.completed.count(BWAPI::UnitTypes::Terran_Control_Tower) > 0)
            {
                if (guardianWraiths > 0)
                    goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Wraith, guardianWraiths));
            }
        }
    }
    else if (_openingGroup == "vultures")
    {
        goal.push_back(std::pair<MacroAct, int>(BWAPI::UnitTypes::Terran_Vulture, numVultures + 3));
        goal.push_back(std::pair<MacroAct, int>(BWAPI::UpgradeTypes::Ion_Thrusters, 1));

        if (numVultures >= 6)
        {
            // The rush is over, transition out on the next call.
            _openingGroup = "tanks";
        }
        ProductionManager::Instance().liftExcessBarracks(1);
    }
    else if (_openingGroup == "tanks")
    {
        // Factory production is scheduled as a composition lane. These are
        // relative weights, not rolling "current + N" targets.
        int tankWeight = 4;
        int vultureWeight = 2;
        int goliathWeight = 1;
        int goliathCap = 3;
        int forcedVultureTarget = 0;
        bool forceGoliathUntilTarget = false;
        bool allowTankDrop = true;
        bool armoryGoalAdded = false;

        // Normal production scaling is a lower-priority lane than using the
        // Factories we already own. Emergency prerequisites are still posted
        // immediately; these routine extra Factory/Machine Shop goals
        // are appended after the next Factory-unit decision.
        MetaPairVector delayedFactoryInfrastructure;

        const bool protossSafetyAppliesNow =
            protossEarlySafetyApplies();

        const bool protossGroundRushNow =
            protossSafetyAppliesNow && protossGroundRushSeen();

        const bool protossAirTechNow =
            protossSafetyAppliesNow && protossAirTechSeen();

        const bool gasStarvedMech =
            the.self()->gas() < 100 &&
            the.self()->minerals() >= 800 &&
            _openingGroup == "tanks";

        if (gasStarvedMech)
        {
            WorkerManager::Instance().setCollectGas(true);
        }

        const bool enemyHeavyMech =
            enemyTerranHeavyMechDetected();

        const OpeningPlan enemyPlan = OpponentModel::Instance().getEnemyPlan();
        const OpeningPlan likelyEnemyPlan = OpponentModel::Instance().getPreparationEnemyPlan();

        const bool enemyHeavyRush =
            enemyPlan == OpeningPlan::HeavyRush ||
            likelyEnemyPlan == OpeningPlan::HeavyRush ||
            actualEarlyGroundRushDetected();

        const bool protossDragoonPressureNow =
            actualProtossDragoonPressureNearBaseDetected() ||
            (enemyHasOpeningThreat(OpeningThreat::DragoonPressure) &&
             enemyMode != EnemyArmyMode::LightGround &&
             enemyMode != EnemyArmyMode::MixedAirLightGround);

        // One shared HeavyRush Vulture target is used by factory scaling,
        // urgent suppression and the composition scheduler.
        const int heavyRushVultureTarget =
            enemyHasOpeningThreat(OpeningThreat::DragoonPressure) ? 2 : 5;

        const bool wallOpeningDefense = terranInfantryDefense().wallProtected;

        const bool urgentVultureDefenseNeeded =
            enemyHeavyRush &&
            !_pureAirPlan &&
            !enemyAirThreatDetected &&
            !carrierTechThreat &&
            (!wallOpeningDefense || numTanks >= 4) &&
            numVultures < heavyRushVultureTarget;

        const int completedBases =
            the.bases.completedBaseCount(the.self());

        const int currentFactories =
            the.my.all.count(BWAPI::UnitTypes::Terran_Factory) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Factory) +
            (int)ProductionManager::Instance().getQueue().numInQueue(BWAPI::UnitTypes::Terran_Factory);

        // Two Factories can spend a one-base economy. The plan's larger floor
        // should wait for a completed natural unless a large bank proves that
        // existing production is insufficient.
        int desiredFactories =
            completedBases <= 1
            ? 2
            : 2 * completedBases;

        const bool pureAirPlan = _pureAirPlan;

        // Hard ceiling prevents late-game overbuilding while preserving the
        // two-per-base rule through four completed bases.
        desiredFactories = std::min(desiredFactories, 8);

        if (urgentVultureDefenseNeeded)
        {
            // HeavyRush contract: establish two Factories, then spend their
            // time on the Vulture minimum before routine scaling to Factory #3.
            desiredFactories = 2;
        }

        if (pureAirPlan)
        {
            const int freeSupply =
                the.self()->supplyTotal() - the.self()->supplyUsed();

            // PureAir can consume a large bank very quickly if enough Factories
            // are available. Do not leave 10k+ resources idle behind the normal
            // two-Factories-per-base scaling rule while we still have a large
            // amount of army supply to fill.
            if (freeSupply >= 24 &&
                the.self()->minerals() >= 800 &&
                the.self()->gas() >= 300)
            {
                const int bankDrivenFactoryTarget =
                    completedBases >= 2
                    ? (the.self()->minerals() >= 1500 && the.self()->gas() >= 700 ? 8 : 6)
                    : 4;

                desiredFactories = std::max(
                    desiredFactories,
                    bankDrivenFactoryTarget);
            }
        }

        if (!urgentVultureDefenseNeeded &&
            (enemyMode == EnemyArmyMode::HeavyGround ||
             enemyMode == EnemyArmyMode::MixedAirHeavyGround))
        {
            desiredFactories = std::max(
                desiredFactories,
                completedBases <= 1 ? 3 : 4);
        }

        // The plan supplies a strategic infrastructure floor. During the
        // strict HeavyRush Vulture stage keep its explicit two-Factory contract;
        // otherwise the macro identity may require a larger stable backbone.
        if (!urgentVultureDefenseNeeded || terranPlan.minFactories <= 2)
        {
            desiredFactories = std::max(desiredFactories, terranPlan.minFactories);
        }

        // Beyond the base-count floor, add at most one Factory when all
        // completed Factories are actually busy and the economy is still
        // floating resources / behind the 15-minute production pace.
        desiredFactories = scaleTerranProductionTargetOneStep(
            BWAPI::UnitTypes::Terran_Factory,
            currentFactories,
            desiredFactories,
            8);

        if (!pureAirPlan)
        {
            // Tanks and Goliaths need gas. Building six to eight Factories on
            // only two completed Refineries spends the gas needed to use them.
            // Keep the plan-specific minimum, but expand routine capacity
            // only as completed gas income catches up.
            const int gasSupportedFactories = std::max(
                terranPlan.minFactories,
                2 * the.my.completed.count(BWAPI::UnitTypes::Terran_Refinery));
            desiredFactories = std::min(desiredFactories,
                std::max(2, gasSupportedFactories));
        }

        if (completedBases <= 1 && !pureAirPlan &&
            the.self()->minerals() < 800)
        {
            desiredFactories = std::min(desiredFactories, 2);
        }

        if (currentFactories < desiredFactories)
        {
            delayedFactoryInfrastructure.push_back(std::make_pair(
                BWAPI::UnitTypes::Terran_Factory,
                desiredFactories
            ));
        }

        const int currentMachineShops =
            the.my.all.count(BWAPI::UnitTypes::Terran_Machine_Shop) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Machine_Shop) +
            (int)ProductionManager::Instance().getQueue().numInQueue(BWAPI::UnitTypes::Terran_Machine_Shop);

        int desiredMachineShops =
            (urgentVultureDefenseNeeded && numTanks == 0) ? 0 : 1;

        if (!urgentVultureDefenseNeeded &&
            !pureAirPlan && desiredFactories >= 4)
        {
            desiredMachineShops = 2;
        }

        if (!urgentVultureDefenseNeeded &&
            !pureAirPlan &&
            (enemyMode == EnemyArmyMode::HeavyGround ||
            enemyMode == EnemyArmyMode::MixedAirHeavyGround ||
            carrierTechThreat))
        {
            desiredMachineShops = std::min(desiredFactories, 3);
        }

        if (!urgentVultureDefenseNeeded &&
            !pureAirPlan && enemyTerranHeavyMechDetected())
        {
            desiredMachineShops = std::min(desiredFactories, 4);
        }

        if (!urgentVultureDefenseNeeded &&
            !pureAirPlan &&
            the.self()->minerals() >= 1200 && the.self()->gas() >= 500)
        {
            desiredMachineShops = std::min(desiredFactories, std::max(desiredMachineShops, 4));
        }

        if (!pureAirPlan && !urgentVultureDefenseNeeded)
        {
            desiredMachineShops = std::max(
                desiredMachineShops,
                std::min(desiredFactories, terranPlan.minMachineShops));
        }

        if (currentMachineShops < desiredMachineShops)
        {
            delayedFactoryInfrastructure.push_back(std::make_pair(
                BWAPI::UnitTypes::Terran_Machine_Shop,
                desiredMachineShops
            ));
        }

        const int completedGoliaths = completedGoliathCount();

        _pureAirPlan = pureAirPlan;

        _defaultTankGather = true;
        _tankMassReleaseTanks = 15;
        _tankMassReleaseGoliaths = 0;

        if (pureAirPlan)
        {
            _defaultTankGather = false;
            _tankMassReleaseTanks = 0;
            _tankMassReleaseGoliaths = 0;
        }
        else
        {
            switch (enemyMode)
            {
            case EnemyArmyMode::MixedAirHeavyGround:
            case EnemyArmyMode::MixedAirLightGround:
                _defaultTankGather = true;
                _tankMassReleaseTanks = 5;
                _tankMassReleaseGoliaths = 5;
                break;

            case EnemyArmyMode::LightGround:
                _defaultTankGather = true;
                _tankMassReleaseTanks = 5;
                _tankMassReleaseGoliaths = 0;
                break;

            case EnemyArmyMode::HeavyGround:
                // Keep a larger core against heavy ground, without delaying
                // the attack until the old 15-Tank threshold.
                _defaultTankGather = true;
                _tankMassReleaseTanks = 10;
                _tankMassReleaseGoliaths = 0;
                break;

            case EnemyArmyMode::Default:
            default:
                _defaultTankGather = true;
                _tankMassReleaseTanks = 15;
                _tankMassReleaseGoliaths = 0;
                break;
            }
        }

        if (!pureAirPlan && terranPlan.tankRelease > 0)
        {
            _tankMassReleaseTanks = std::min(
                _tankMassReleaseTanks,
                terranPlan.tankRelease);

            if (terranPlan.goliathRelease > 0)
            {
                _tankMassReleaseGoliaths = std::max(
                    _tankMassReleaseGoliaths,
                    terranPlan.goliathRelease);
            }
        }

        if (pureAirPlan)
        {
            allowTankDrop = false;

            // Carrier technology is enough to enter this branch immediately;
            // do not wait for the stabilized enemy mode or a visible Carrier.
            // Existing Tanks/Vultures remain useful as the rear support layer,
            // but PureAir itself must not create a second ground composition.
            // A later Mixed classification will restore their production.
            tankWeight = 0;
            vultureWeight = 0;
            goliathWeight = 10;

            goliathCap = pureAirGoliathTarget(carrierTechThreat);
            forceGoliathUntilTarget = numGoliaths < 6;
        }
        else
        {
            switch (enemyMode)
            {
            case EnemyArmyMode::PureAir:
                // Normally unreachable because pureAirPlan is already true for
                // this mode. Keep the fallback identical to the real PureAir
                // composition anyway; never reintroduce the old fixed 20 cap.
                allowTankDrop = false;
                tankWeight = 0;
                vultureWeight = 0;
                goliathWeight = 10;
                goliathCap = pureAirGoliathTarget(false);
                forceGoliathUntilTarget = numGoliaths < 6;
                break;

            case EnemyArmyMode::HeavyGround:
                tankWeight = 4;
                vultureWeight = 3;
                goliathWeight = 0;
                // Ground-only mode must not grow Goliaths every planning wave.
                goliathCap = numGoliaths;
                allowTankDrop = true;
                break;

            case EnemyArmyMode::LightGround:
                tankWeight = 2;
                vultureWeight = 7;
                goliathWeight = 0;
                // Keep the Tanks strategy focused on Tanks/Vultures until real
                // air evidence moves the classifier into a MixedAir mode.
                goliathCap = numGoliaths;
                allowTankDrop = true;
                break;

            case EnemyArmyMode::MixedAirHeavyGround:
                tankWeight = 4;
                vultureWeight = 2;
                goliathWeight = 4;
                goliathCap = mixedAirGoliathTarget(enemyMode);
                allowTankDrop = false;
                break;

            case EnemyArmyMode::MixedAirLightGround:
                tankWeight = 1;
                vultureWeight = 4;
                goliathWeight = 4;
                goliathCap = mixedAirGoliathTarget(enemyMode);
                allowTankDrop = false;
                break;

            case EnemyArmyMode::Default:
            default:
                tankWeight = 4;
                vultureWeight = 3;
                goliathWeight = 1;
                // Small baseline cover only; this is an absolute cap, not +2
                // on every BOSS refresh.
                goliathCap = 3;
                allowTankDrop = true;
                break;
            }
        }

        // Apply matchup-specific pressure modifiers after the base enemy-mode
        // weights. In the old code this ran before the switch above and was
        // silently overwritten by the mode assignment.
        if (enemyBioTankPush && !pureAirPlan)
        {
            tankWeight = std::max(tankWeight, 6);
            vultureWeight = std::max(vultureWeight, 5);
            allowTankDrop = false;
        }

        if (protossAirTechNow && !protossGroundRushNow && !pureAirPlan)
        {
            goliathCap = std::max(goliathCap, 10);
            goliathWeight = std::max(goliathWeight, 6);
            allowTankDrop = false;

            if (!armoryGoalAdded &&
                the.my.all.count(BWAPI::UnitTypes::Terran_Armory) == 0 &&
                !BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Armory))
            {
                goal.push_back(MetaPair(
                    MacroAct(BWAPI::UnitTypes::Terran_Armory, MacroLocation::Main),
                    1
                ));
                armoryGoalAdded = true;
            }

            tankWeight = std::min(tankWeight, 2);
            vultureWeight = std::min(vultureWeight, 2);
        }

        // Apply the durable plan after generic EnemyArmyMode/matchup weights,
        // but before resource scarcity and immediate local threats. The latter
        // are allowed to override the plan for a few production decisions.
        applyTerranPlanComposition(
            terranPlan,
            pureAirPlan,
            enemyAirThreatDetected,
            numGoliaths,
            tankWeight,
            vultureWeight,
            goliathWeight,
            goliathCap,
            allowTankDrop);

        // Current army evidence outranks an old opening profile. In particular
        // AntiDragoon must not keep a Tank-heavy ratio after a Zealot/Ling switch.
        if (enemyMode == EnemyArmyMode::LightGround && !pureAirPlan &&
            !protossDragoonPressureNow && !enemyBioTankPush)
        {
            tankWeight = 2;
            vultureWeight = 7;
            if (!enemyAirThreatDetected && !protossAirTechNow)
            {
                goliathWeight = 0;
                goliathCap = numGoliaths;
            }
        }

        // The current Mixed classifier outranks the durable opening plan. Keep
        // exact scalable ratios here because applyTerranPlanComposition() may
        // otherwise turn an old Siege/Hydra plan back into a Tank-heavy army.
        if (!pureAirPlan &&
            enemyMode == EnemyArmyMode::MixedAirHeavyGround)
        {
            tankWeight = 4;
            vultureWeight = 2;
            goliathWeight = 4;
            goliathCap = mixedAirGoliathTarget(enemyMode);
            allowTankDrop = false;
        }
        else if (!pureAirPlan &&
            enemyMode == EnemyArmyMode::MixedAirLightGround)
        {
            tankWeight = 1;
            vultureWeight = 4;
            goliathWeight = 4;
            goliathCap = mixedAirGoliathTarget(enemyMode);
            allowTankDrop = false;
        }

        if (gasStarvedMech && !pureAirPlan)
        {
            tankWeight = std::min(tankWeight, 1);

            if (enemyMode != EnemyArmyMode::PureAir &&
                enemyMode != EnemyArmyMode::MixedAirHeavyGround &&
                enemyMode != EnemyArmyMode::MixedAirLightGround &&
                !carrierTechThreat)
            {
                goliathCap = 0;
                goliathWeight = 0;
            }

            vultureWeight = std::max(vultureWeight, 5);
        }

        // An air-only raid gets the first six Goliaths immediately. Once that
        // local AA shell exists, let a confirmed MixedAir composition resume
        // its Tank/Vulture/Goliath ratio instead of forcing Goliaths all the
        // way to the much larger strategic air target.
        if (nearbyAirOnlyAttack && !pureAirPlan)
        {
            if (numGoliaths < 6)
                vultureWeight = 0;
            goliathWeight = std::max(goliathWeight, 10);
            // Local raiders need a quick AA layer, not an endlessly increasing
            // +4 target on every planning cycle.
            goliathCap = std::max(goliathCap, 6);
            forceGoliathUntilTarget = numGoliaths < 6;
            allowTankDrop = false;
        }

        const bool zealotRushNow =
            actualEarlyProtossZealotRushDetected();

        if (zealotRushNow && !pureAirPlan &&
            !enemyAirThreatDetected && !carrierTechThreat &&
            !wallOpeningDefense)
        {
            // Mines are more important than Ion Thrusters against early zealots.
            if (!BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Spider_Mines) &&
                !BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::Spider_Mines))
            {
                goal.push_back(MetaPair(BWAPI::TechTypes::Spider_Mines, 1));
            }

            // The scheduler treats this as an absolute emergency layer.
            // Until five Vultures exist, every newly available Factory slot
            // goes to Vultures. After that, the normal composition resumes.
            forcedVultureTarget = std::max(forcedVultureTarget, 5);
            if (numVultures < forcedVultureTarget)
            {
                vultureWeight = std::max(vultureWeight, 5);
                tankWeight = 0;
            }
            // Once the emergency floor exists, resume the LightGround ratio.
            // A minimum is not a permanent Vulture cap.
        }

        if (protossDragoonPressureNow && !pureAirPlan &&
            !enemyAirThreatDetected && !carrierTechThreat)
        {
            const int minTanksVsDragoons = 6;
            const int minVulturesVsDragoons = numTanks < minTanksVsDragoons
                ? 2 : std::min(12, std::max(4, numTanks / 2));

            if (numTanks < minTanksVsDragoons)
            {
                tankWeight = std::max(tankWeight, minTanksVsDragoons - numTanks);
            }
            else
            {
                tankWeight = std::max(tankWeight, 4);
            }

            // Establish the first Siege core, then scale the mine screen with
            // it. The opening's two-Vulture floor used to zero their weight
            // forever and could produce 50+ Tanks with almost no mine support.
            forcedVultureTarget = std::max(
                forcedVultureTarget,
                minVulturesVsDragoons);

            if (numVultures < minVulturesVsDragoons)
            {
                // First establish the two-Vulture mine/body-block layer.
                vultureWeight = std::max(vultureWeight, 2);
            }
            else if (numTanks < minTanksVsDragoons)
            {
                vultureWeight = 0;
            }
            else
            {
                vultureWeight = std::max(vultureWeight, 3);
            }

            // Goliaths are not the answer to mass dragoons.
            goliathCap = 0;
            goliathWeight = 0;

            // Do not tech into drops while defending the front.
            allowTankDrop = false;

            if (!BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode) &&
                !BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::Tank_Siege_Mode))
            {
                goal.push_back(MetaPair(BWAPI::TechTypes::Tank_Siege_Mode,1));
            }

            if (!BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Spider_Mines) &&
                !BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::Spider_Mines))
            {
                goal.push_back(MetaPair(BWAPI::TechTypes::Spider_Mines, 1));
            }
        }

        if (enemyHeavyRush && !pureAirPlan &&
            !enemyAirThreatDetected && !carrierTechThreat &&
            (!wallOpeningDefense || numTanks >= 4))
        {
            const BuildOrderQueue& liveQueue =
                ProductionManager::Instance().getQueue();

            const int startedFactories =
                the.my.all.count(BWAPI::UnitTypes::Terran_Factory) +
                (int)BuildingManager::Instance().getNumUnstarted(
                    BWAPI::UnitTypes::Terran_Factory);

            const int factoriesStartedOrImmediate =
                startedFactories +
                (int)liveQueue.numInNextN(
                    BWAPI::UnitTypes::Terran_Factory, 2);

            if (startedFactories < 2)
            {
                // HeavyRush ordering is strict:
                // 2 started Factories -> Vulture emergency layer -> Tanks.
                vultureWeight = 0;
                tankWeight = 0;
                goliathWeight = 0;
                forcedVultureTarget = 0;

                if (factoriesStartedOrImmediate < 2)
                {
                    goal.push_back(std::make_pair(
                        BWAPI::UnitTypes::Terran_Factory,
                        2
                    ));
                }
            }
            else if (numVultures < heavyRushVultureTarget)
            {
                forcedVultureTarget = std::max(
                    forcedVultureTarget,
                    heavyRushVultureTarget);

                // No Tank may steal a Factory slot before the emergency
                // Vulture minimum is satisfied.
                vultureWeight = std::max(
                    vultureWeight,
                    heavyRushVultureTarget);
                tankWeight = 0;
            }
        }

        if (protossCarrierPlan && !pureAirPlan)
        {
            if (numTanks >= 4)
            {
                tankWeight = 0;
            }
            else
            {
                tankWeight = std::min(tankWeight, 4 - numTanks);
            }

            if (numVultures >= 2)
            {
                vultureWeight = 0;
            }
            else
            {
                vultureWeight = std::min(vultureWeight, 2 - numVultures);
            }
        }

        // Use saved PureAir history or the opponent-specific TvZ_AntiMuta Plan
        // as a post-opening preparation signal. A Zerg HeavyRush blocks this
        // only until the agreed ground-defense floor (two completed Bunkers and
        // five Vultures) exists; the coarse HeavyRush label must not suppress AA
        // forever while the Zerg is already teching behind the first Ling wave.
        const bool antiMutaGroundDefenseReady =
            numVultures >= heavyRushVultureTarget &&
            (wallOpeningDefense || the.my.completed.count(BWAPI::UnitTypes::Terran_Bunker) >= 2 ||
             numTanks >= 2);

        const bool learnedAirPreparationAllowed =
            !enemyHeavyRush ||
            (enemyRace == BWAPI::Races::Zerg && antiMutaGroundDefenseReady);

        const bool useLearnedPureAirPreparation =
            learnedPureAirPreparation &&
            learnedAirPreparationAllowed &&
            !zealotRushNow &&
            !protossDragoonPressureNow;

        const bool useLearnedMixedPreparation =
            learnedMixedPreparation &&
            learnedAirPreparationAllowed &&
            !zealotRushNow &&
            !protossDragoonPressureNow;

        const bool useLearnedGroundAirPreparation =
            learnedGroundAirPreparation &&
            learnedAirPreparationAllowed &&
            !zealotRushNow &&
            !protossDragoonPressureNow;

        const bool useLearnedCapitalAirPreparation =
            learnedCapitalAirPreparation &&
            learnedAirPreparationAllowed &&
            !zealotRushNow &&
            !protossDragoonPressureNow;

        const bool useTvZAntiMutaPreparation =
            selectedTvZAntiMutaPreparation &&
            (!enemyHeavyRush || antiMutaGroundDefenseReady);

        const bool useTvTAntiAirPreparation =
            selectedTvTAntiAirPreparation;

        const bool usePredictiveAirPreparation =
            useLearnedPureAirPreparation ||
            useLearnedMixedPreparation ||
            useLearnedGroundAirPreparation ||
            useLearnedCapitalAirPreparation ||
            useTvZAntiMutaPreparation ||
            useTvTAntiAirPreparation;

        // Current MixedAir evidence already sets the scalable composition
        // above. A remembered or selected AntiMuta opening must not zero the
        // Vulture lane or force Goliaths up to the entire mixed-air target.
        if (usePredictiveAirPreparation && !pureAirPlan &&
            !enemyMixedAirGround)
        {
            allowTankDrop = false;

            if (useTvTAntiAirPreparation)
            {
                // Expected Wraiths are not permission to abandon the ground
                // war. The common follow-up is Tanks, so keep Siege production
                // live while assembling the first anti-air layer.
                tankWeight = std::max(tankWeight, 5);
            }
            else if (useLearnedMixedPreparation)
            {
                tankWeight = std::max(tankWeight, 4);
            }
            else if (useLearnedGroundAirPreparation ||
                useLearnedCapitalAirPreparation)
            {
                // A remembered capability is never enough to abandon the
                // ground war. Keep the four-Tank shell while preparing AA.
                tankWeight = std::max(tankWeight, 4);
            }
            else
            {
                tankWeight = std::min(tankWeight, 2);
            }

            // Five Vultures remain the completed anti-Ling shell. Do not keep
            // spending Factory time on more of them once AntiMuta preparation
            // is allowed to start.
            if (useTvZAntiMutaPreparation &&
                numVultures >= heavyRushVultureTarget)
            {
                vultureWeight = 0;
            }
            else if (useTvTAntiAirPreparation)
            {
                vultureWeight = std::min(vultureWeight, 2);
            }
            else if (useLearnedMixedPreparation)
            {
                vultureWeight = std::max(vultureWeight, 2);
            }
            else if (useLearnedGroundAirPreparation ||
                useLearnedCapitalAirPreparation)
            {
                vultureWeight = std::max(vultureWeight, 2);
            }
            else
            {
                vultureWeight = std::min(vultureWeight, 1);
            }

            goliathWeight = std::max(
                goliathWeight,
                useTvTAntiAirPreparation ? 6 :
                useLearnedCapitalAirPreparation ? 6 :
                useLearnedGroundAirPreparation ? 4 :
                useLearnedMixedPreparation ? 4 :
                8);

            // Four pre-emptive Goliaths plus Bunker Marines and two Turrets are
            // enough to survive the first Muta contact without blindly buying
            // the full twelve-Goliath PureAir army. Confirmation below removes
            // the cap and switches every free Factory to Goliaths.
            const int preparationGoliathTarget =
                useTvZAntiMutaPreparation ? 4 :
                useTvTAntiAirPreparation ? 6 :
                useLearnedCapitalAirPreparation ? 8 :
                useLearnedGroundAirPreparation ? 4 :
                useLearnedMixedPreparation ? 4 :
                12;

            goliathCap = std::max(
                goliathCap,
                preparationGoliathTarget);
            forceGoliathUntilTarget = true;
        }

        // A visible Overlord, Shuttle, Dropship, Observer or other flying unit
        // is enough evidence to keep a small mobile AA reserve. Apply this
        // after ground-pressure overrides so they cannot erase the minimum.
        if (enemyFlyerSeen && !pureAirPlan)
        {
            goliathCap = std::max(goliathCap, 4);
            goliathWeight = std::max(goliathWeight, 4);
            forceGoliathUntilTarget =
                forceGoliathUntilTarget || numGoliaths < 4;
        }

        // A transport can repeatedly unload cargo, while a lifted building
        // keeps our army revealed. Both need a small mobile AA screen even
        // though neither counts as a combat-air composition.
        if (airIntrusionSeen && !pureAirPlan)
        {
            goliathCap = std::max(goliathCap, 6);
            goliathWeight = std::max(goliathWeight, 6);
            forceGoliathUntilTarget =
                forceGoliathUntilTarget || numGoliaths < 6;
        }

        // A dense Sunken/Cannon line is a Siege problem even when the enemy's
        // mobile army is classified as LightGround. The combat layer waits for
        // two or three Tanks before attacking such a line; keep production in
        // step with that decision instead of replacing lost Vultures forever.
        // Even a gas shortage must not prevent the first three Siege Tanks;
        // after that core exists, cheap Vultures can keep the Factories active
        // while gas income recovers.
        const int knownGroundStaticDefenses =
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Sunken_Colony) +
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Protoss_Photon_Cannon);
        if (!pureAirPlan && knownGroundStaticDefenses >= 3 &&
            (!gasStarvedMech || numTanks < 3))
        {
            tankWeight = std::max(tankWeight, 6);
            vultureWeight = std::min(vultureWeight, 2);
            allowTankDrop = false;
        }

        // Tech belongs to its own lane. Research Siege whenever Tanks are part
        // of the desired composition, but do not let it resurrect Tank
        // production during the strict HeavyRush Vulture phase.
        if (((!pureAirPlan && (tankWeight > 0 || numTanks > 0)) ||
             (pureAirPlan && numTanks > 0)) &&
            (!urgentVultureDefenseNeeded || numTanks > 0) &&
            !BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode) &&
            !BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::Tank_Siege_Mode))
        {
            goal.push_back(MetaPair(
                BWAPI::TechTypes::Tank_Siege_Mode,
                1));
        }

        if (!pureAirPlan &&
            !BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Spider_Mines) &&
            !BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::Spider_Mines))
        {
            goal.push_back(MetaPair(BWAPI::TechTypes::Spider_Mines,1));
        }

        int desiredGoliaths = numGoliaths;

        if (pureAirPlan)
        {
            const int pureAirTarget =
                pureAirGoliathTarget(carrierTechThreat || protossCarrierPlan);

            if (numGoliaths < pureAirTarget)
            {
                desiredGoliaths = pureAirTarget;
            }
        }
        else if (enemyMode == EnemyArmyMode::MixedAirHeavyGround ||
            enemyMode == EnemyArmyMode::MixedAirLightGround)
        {
            desiredGoliaths = std::max(
                numGoliaths,
                mixedAirGoliathTarget(enemyMode));
        }
        else if ((protossAirTechNow ||
            nearbyAirOnlyAttack ||
            currentGroundAirThreat ||
            enemyAirThreatDetected ||
            enemyFlyerSeen ||
            airIntrusionSeen ||
            usePredictiveAirPreparation) &&
            goliathCap > numGoliaths)
        {
            // Explicit current air evidence overrides a stale ground-only mode.
            // Previously protossAirTechNow could raise goliathCap to 10 and the
            // Heavy/LightGround branch immediately below discarded it.
            desiredGoliaths = goliathCap;
        }
        else if (enemyMode == EnemyArmyMode::HeavyGround ||
            enemyMode == EnemyArmyMode::LightGround)
        {
            // No new Goliaths for a truly ground-only classification. Existing
            // ones remain in the army; Tanks/Vultures get the Factory capacity.
            desiredGoliaths = numGoliaths;
        }
        else if (goliathCap > numGoliaths)
        {
            desiredGoliaths = goliathCap;
        }

        const int goliathGoal = desiredGoliaths;

        if (goliathGoal > numGoliaths)
        {
            const BuildOrderQueue& queue =
                ProductionManager::Instance().getQueue();

            if (!armoryGoalAdded &&
                !armoryExistsOrIsBeingBuiltOrQueued(queue))
            {
                goal.push_back(MetaPair(
                    MacroAct(BWAPI::UnitTypes::Terran_Armory, MacroLocation::Main),
                    1
                ));

                armoryGoalAdded = true;
            }
        }

        // The verified wall replaces the old early Vulture/Marine emergency
        // layer. Before the natural, establish four Tanks first. Threat-specific
        // Vultures are allowed again after that core exists. Re-assert this at
        // the end of composition tuning so no rush branch can steal the first
        // Factory slots.
        if (wallOpeningDefense && numCC < 2 && numTanks < 4 && !pureAirPlan)
        {
            forcedVultureTarget = 0;
            vultureWeight = 0;
            tankWeight = std::max(tankWeight, 12);

            if (!enemyAirThreatDetected && !carrierTechThreat &&
                !enemyFlyerSeen && !airIntrusionSeen)
            {
                goliathWeight = 0;
                forceGoliathUntilTarget = false;
            }
        }

        // Expose exactly one Factory-unit decision. ProductionManager will
        // immediately replan after it starts, so the next free Factory asks the
        // composition scheduler again instead of inheriting a stale rolling
        // target.
        BWAPI::UnitType nextFactoryUnit = BWAPI::UnitTypes::None;

        // PureAir can be selected before a Carrier is visible. Ground-pressure
        // rules use visible air evidence, so do not let their emergency Vulture
        // target override the committed plan and strand idle Factories: the
        // executor intentionally rejects Vultures while PureAir is active.
        if (pureAirPlan)
        {
            forcedVultureTarget = 0;
            tankWeight = 0;
            vultureWeight = 0;
            goliathWeight = std::max(goliathWeight, 10);
        }
        nextFactoryUnit = chooseNextTerranFactoryUnit(
            numTanks,
            numVultures,
            numGoliaths,
            tankWeight,
            vultureWeight,
            goliathWeight,
            goliathGoal,
            forcedVultureTarget,
            forceGoliathUntilTarget);

        if (nextFactoryUnit != BWAPI::UnitTypes::None)
        {
            int nextTarget = 1;

            if (nextFactoryUnit == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
            {
                nextTarget = numTanks + 1;
            }
            else if (nextFactoryUnit == BWAPI::UnitTypes::Terran_Vulture)
            {
                nextTarget = numVultures + 1;
            }
            else if (nextFactoryUnit == BWAPI::UnitTypes::Terran_Goliath)
            {
                nextTarget = numGoliaths + 1;
            }

            goal.push_back(MetaPair(
                nextFactoryUnit,
                nextTarget));
        }

        // The natural competes with optional Factory/shop scaling for the
        // same minerals. Give the current combat slot its chance first, then
        // invest in the base before increasing production capacity.
        if (wantTerranExpansion && !expansionGoalAdded)
        {
            goal.push_back(MetaPair(
                BWAPI::UnitTypes::Terran_Command_Center,
                numCC + 1
            ));
            expansionGoalAdded = true;
        }

        for (const MetaPair& infrastructureGoal : delayedFactoryInfrastructure)
        {
            goal.push_back(infrastructureGoal);
        }

        // Carrier PureAir uses the shared, staged Ghost plan below. Keep this
        // legacy branch only for non-Carrier Protoss air so the same tech tree
        // is not submitted twice in one BOSS goal.
        if (protossGhostTechPlan && !activeProtossCarrierCounter)
        {
            const int desiredGhosts = 5;

            const bool starportExistsOrBuilding =
                the.my.all.count(BWAPI::UnitTypes::Terran_Starport) > 0 ||
                BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Starport);

            const bool controlTowerExistsOrBuilding =
                the.my.all.count(BWAPI::UnitTypes::Terran_Control_Tower) > 0 ||
                BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Control_Tower);

            const bool scienceFacilityExistsOrBuilding =
                the.my.all.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0 ||
                BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Science_Facility);

            const bool academyExistsOrBuilding =
                the.my.all.count(BWAPI::UnitTypes::Terran_Academy) > 0 ||
                BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Academy);

            const bool hasCompletedScienceFacility =
                the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0;

            const bool hasCompletedCovertOps =
                the.my.completed.count(BWAPI::UnitTypes::Terran_Covert_Ops) > 0;

            if (!hasCompletedLandedBarracks() &&
                the.my.all.count(BWAPI::UnitTypes::Terran_Barracks) == 0 &&
                !BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Barracks))
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Barracks, 1));
            }

            if (!starportExistsOrBuilding)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, 1));
            }

            if (!controlTowerExistsOrBuilding)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Control_Tower, 1));
            }

            if (!scienceFacilityExistsOrBuilding)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Science_Facility, 1));
            }

            if (!academyExistsOrBuilding)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Academy, 1));
            }

            const BuildOrderQueue& queue =
                ProductionManager::Instance().getQueue();

            const bool covertOpsExistsOrBuildingOrQueued =
                the.my.all.count(BWAPI::UnitTypes::Terran_Covert_Ops) > 0 ||
                BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Covert_Ops) ||
                queue.anyInQueue(BWAPI::UnitTypes::Terran_Covert_Ops);

            if (hasCompletedScienceFacility &&
                !hasCompletedCovertOps &&
                !covertOpsExistsOrBuildingOrQueued)
            {
                goal.push_back(MetaPair(
                    BWAPI::UnitTypes::Terran_Covert_Ops,
                    1
                ));
            }

            if (hasCompletedCovertOps)
            {
                BuildOrderQueue& queue =
                    ProductionManager::Instance().getQueue();

                const bool lockdownDone =
                    self->hasResearched(BWAPI::TechTypes::Lockdown);

                const bool lockdownInProgressOrQueued =
                    self->isResearching(BWAPI::TechTypes::Lockdown) ||
                    techInQueue(queue, BWAPI::TechTypes::Lockdown);

                if (!lockdownDone && !lockdownInProgressOrQueued)
                {
                    goal.push_back(MetaPair(BWAPI::TechTypes::Lockdown, 1));
                }

                const bool personnelCloakingDone =
                    self->hasResearched(BWAPI::TechTypes::Personnel_Cloaking);

                const bool personnelCloakingInProgressOrQueued =
                    self->isResearching(BWAPI::TechTypes::Personnel_Cloaking) ||
                    techInQueue(queue, BWAPI::TechTypes::Personnel_Cloaking);

                if (lockdownDone &&
                    !personnelCloakingDone &&
                    !personnelCloakingInProgressOrQueued)
                {
                    goal.push_back(MetaPair(BWAPI::TechTypes::Personnel_Cloaking, 1));
                }

                if (personnelCloakingDone &&
                    self->getUpgradeLevel(BWAPI::UpgradeTypes::Moebius_Reactor) == 0 &&
                    !self->isUpgrading(BWAPI::UpgradeTypes::Moebius_Reactor) &&
                    !queue.anyInQueue(BWAPI::UpgradeTypes::Moebius_Reactor))
                {
                    goal.push_back(MetaPair(BWAPI::UpgradeTypes::Moebius_Reactor, 1));
                }
            }

            const int ownedOrTrainingGhosts =
                countOwnedAndTrainingTerranGhosts();

            const int queuedGhosts =
                (int)ProductionManager::Instance().getQueue().numInQueue(
                    BWAPI::UnitTypes::Terran_Ghost);

            if (hasAcademy &&
                hasCompletedCovertOps &&
                ownedOrTrainingGhosts + queuedGhosts < desiredGhosts &&
                queuedGhosts == 0)
            {
                // One landed Barracks can only start one Ghost at a time.
                // Request exactly its next job; batching two here plus the
                // urgent request left three Ghosts in front of Factory units.
                const int ghostGoal =
                    std::min(desiredGhosts, ownedOrTrainingGhosts + 1);

                goal.push_back(MetaPair(
                    BWAPI::UnitTypes::Terran_Ghost,
                    ghostGoal
                ));
            }
        }

        const bool anyProtossAirPlan =
            protossCarrierPlan || protossNonCarrierAirPlan;

        // Range matters most against a transport that unloads, fires and
        // leaves before Goliaths can close the distance. Ensure the research
        // producer exists even in a ground-only Tank opening.
        if (airIntrusionSeen && numGoliaths >= 2 && hasFactory &&
            the.my.all.count(BWAPI::UnitTypes::Terran_Machine_Shop) == 0 &&
            BuildingManager::Instance().getNumUnstarted(
                BWAPI::UnitTypes::Terran_Machine_Shop) == 0 &&
            !ProductionManager::Instance().getQueue().anyInQueue(
                BWAPI::UnitTypes::Terran_Machine_Shop))
        {
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Machine_Shop, 1));
        }

        const bool shouldGetCharon =
            hasArmory &&
            self->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) == 0 &&
            !self->isUpgrading(BWAPI::UpgradeTypes::Charon_Boosters) &&
            (
                (protossCarrierPlan && numGoliaths >= 2) ||
                (protossNonCarrierAirPlan && numGoliaths >= 4) ||
                (genericAirPlan && numGoliaths >= 6) ||
                (!anyProtossAirPlan && !genericAirPlan && numGoliaths >= 4) ||
                (airIntrusionSeen && numGoliaths >= 2)
                );

        if (shouldGetCharon)
        {
            goal.push_back(MetaPair(BWAPI::UpgradeTypes::Charon_Boosters, 1));
        }

        if (!pureAirPlan && enemyBioTankPush && completedTankCount() >= 8)
        {
            if (the.my.all.count(BWAPI::UnitTypes::Terran_Starport) == 0)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, 1));
            }

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Control_Tower) == 0)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Control_Tower, 1));
            }

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Science_Facility) == 0)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Science_Facility, 1));
            }

            if (numVessels < MAX_SCIENCE_VESSELS &&
                scienceVesselsCurrentlyTraining() == 0)
            {
                goal.push_back(MetaPair(
                    BWAPI::UnitTypes::Terran_Science_Vessel,
                    numVessels + 1
                ));
            }

            if (completedVessels > 0 &&
                !BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Irradiate) &&
                !BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::Irradiate))
            {
                goal.push_back(MetaPair(BWAPI::TechTypes::Irradiate, 1));
            }
        }

        if (!pureAirPlan &&
            (protossCarrierPlan || protossNonCarrierAirPlan || genericAirPlan) &&
            completedGoliaths >= 10)
        {
            if (the.my.all.count(BWAPI::UnitTypes::Terran_Starport) == 0)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, 1));
            }

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Control_Tower) == 0)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Control_Tower, 1));
            }

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Science_Facility) == 0)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Science_Facility, 1));
            }

            if (!pureAirPlan &&
                numVessels < 1 &&
                scienceVesselsCurrentlyTraining() == 0)
            {
                goal.push_back(MetaPair(
                    BWAPI::UnitTypes::Terran_Science_Vessel,
                    1
                ));
            }

            if (completedVessels > 0)
            {
                const BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();

                if ((enemyRace == BWAPI::Races::Protoss || carrierTechThreat) &&
                    !BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::EMP_Shockwave) &&
                    !BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::EMP_Shockwave))
                {
                    goal.push_back(MetaPair(BWAPI::TechTypes::EMP_Shockwave,1));
                }
                if (enemyRace == BWAPI::Races::Zerg &&
                    !BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Irradiate) &&
                    !BWAPI::Broodwar->self()->isResearching(BWAPI::TechTypes::Irradiate))
                {
                    goal.push_back(MetaPair(BWAPI::TechTypes::Irradiate,1));
                }
            }
        }

        if (enemyAirThreatDetected || carrierTechThreat || goliathGoal > numGoliaths)
        {
            if (!hasFactory)
            {
                goal.push_back(MetaPair(
                    MacroAct(BWAPI::UnitTypes::Terran_Factory, MacroLocation::Main),
                    1
                ));
            }

            if (!urgentVultureDefenseNeeded &&
                the.my.all.count(BWAPI::UnitTypes::Terran_Machine_Shop) == 0 &&
                !BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Machine_Shop))
            {
                goal.push_back(MetaPair(
                    MacroAct(BWAPI::UnitTypes::Terran_Machine_Shop, MacroLocation::Main),
                    1
                ));
            }

            if (goliathCap > 0 &&
                !armoryGoalAdded &&
                the.my.all.count(BWAPI::UnitTypes::Terran_Armory) == 0 &&
                !BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Armory))
            {
                goal.push_back(MetaPair(
                    MacroAct(BWAPI::UnitTypes::Terran_Armory, MacroLocation::Main),
                    1
                ));
                armoryGoalAdded = true;
            }
        }

        if (!pureAirPlan &&
            vultureWeight > 0 &&
            numVultures > 3)
        {
            goal.push_back(std::make_pair(
                BWAPI::UpgradeTypes::Ion_Thrusters,
                1
            ));
        }

        if (makeVessel)
        {
            BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();
            if (enemyRace == BWAPI::Races::Zerg)
            {
                if (!self->hasResearched(BWAPI::TechTypes::Irradiate) &&
                    !self->isResearching(BWAPI::TechTypes::Irradiate))
                {
                    goal.push_back(std::make_pair(BWAPI::TechTypes::Irradiate, 1));
                }
            }
            else if (enemyRace == BWAPI::Races::Protoss)
            {
                if (!self->hasResearched(BWAPI::TechTypes::EMP_Shockwave) &&
                    !self->isResearching(BWAPI::TechTypes::EMP_Shockwave))
                {
                    goal.push_back(std::make_pair(BWAPI::TechTypes::EMP_Shockwave, 1));
                }
            }
        }

        const bool lockdownReadyForBarracks =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Covert_Ops) > 0 &&
            self->hasResearched(BWAPI::TechTypes::Lockdown);

        const bool ghostProductionNeeded =
            protossGhostTechPlan &&
            lockdownReadyForBarracks &&
            the.my.all.count(BWAPI::UnitTypes::Terran_Ghost) < 5;

        const bool keepBarracksForProduction =
            ghostProductionNeeded ||
            ProductionManager::Instance().getQueue().anyInQueue(BWAPI::UnitTypes::Terran_Ghost) ||
            ProductionManager::Instance().getQueue().anyInQueue(BWAPI::UnitTypes::Terran_Marine) ||
            ProductionManager::Instance().getQueue().anyInQueue(BWAPI::UnitTypes::Terran_Medic) ||
            ProductionManager::Instance().getQueue().anyInQueue(BWAPI::UnitTypes::Terran_Firebat);

        // Opening-plan labels are intentionally sticky for learning/strategy
        // purposes, but they must not keep an idle Barracks grounded forever.
        // On tight maps a grounded Barracks can physically trap Tanks/Vultures
        // long after the opening rush is over. Keep it down while Bio is really
        // needed, or while opening pressure still matters; afterwards an idle
        // liftable Barracks is free to lift (and the tank-opening vision logic
        // can move it out of the traffic lane).
        const bool barracksOpeningDefenseWindow =
            the.now() <= 9 * 24 * 60;

        const bool rememberedOpeningNeedsBarracks =
            barracksOpeningDefenseWindow &&
            (enemyOpeningGroundPressure() ||
             enemyPlan == OpeningPlan::HeavyRush ||
             likelyEnemyPlan == OpeningPlan::HeavyRush ||
             enemyPlan == OpeningPlan::FastRush ||
             likelyEnemyPlan == OpeningPlan::FastRush ||
             enemyPlan == OpeningPlan::Proxy ||
             likelyEnemyPlan == OpeningPlan::Proxy);

        const bool keepBarracksForRushDefense =
            keepBarracksForProduction ||
            rememberedOpeningNeedsBarracks ||
            actualEarlyProtossZealotRushDetected() ||
            actualEarlyGroundRushDetected() ||
            actualEarlyLightGroundRushDetected();

        if (!keepBarracksForRushDefense)
        {
            ProductionManager::Instance().liftExcessBarracks(
                std::max(1, terranPlan.minBarracks));
        }
        // Keep Engineering Bays landed. They are persistent infrastructure for
        // infantry upgrades and turrets; auto-lifting them made planners treat
        // the prerequisite as unavailable and could flood the queue with more Bays.

        if (!pureAirPlan && enemyHeavyMech)
        {
            const int desiredVesselsVsMech = 1;

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Starport) == 0)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, 1));
            }

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Control_Tower) == 0)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Control_Tower, 1));
            }

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Science_Facility) == 0)
            {
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Science_Facility, 1));
            }

            if (plannedScienceVessels < desiredVesselsVsMech)
            {
                goal.push_back(MetaPair(
                    BWAPI::UnitTypes::Terran_Science_Vessel,
                    desiredVesselsVsMech
                ));
            }
        }

        const bool tankDropEnabledNow =
            the.bases.completedBaseCount(the.self()) >= 2;

        const bool tankDropLaunchLimitReached =
            CombatCommander::Instance().tankDropLaunchLimitReached();

        if (allowTankDrop && tankDropEnabledNow && !tankDropLaunchLimitReached)
        {
            const int desiredDropShips = 2;

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Starport) == 0)
            {
                goal.push_back(std::make_pair(BWAPI::UnitTypes::Terran_Starport, 1));
            }

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Control_Tower) == 0)
            {
                goal.push_back(std::make_pair(BWAPI::UnitTypes::Terran_Control_Tower, 1));
            }

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Dropship) < desiredDropShips)
            {
                goal.push_back(std::make_pair(
                    BWAPI::UnitTypes::Terran_Dropship,
                    desiredDropShips
                ));
            }
        }


        if (activeProtossCarrierCounter || protossNonCarrierAirPlan || genericAirPlan ||
            mixedAirGroundPlan || usePredictiveAirPreparation)
        {
            MetaPairVector airDefenseGoals;
            if (
                the.my.all.count(BWAPI::UnitTypes::Terran_Engineering_Bay) == 0 &&
                !BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Engineering_Bay))
            {
                airDefenseGoals.push_back(MetaPair(
                    BWAPI::UnitTypes::Terran_Engineering_Bay,
                    1));
            }

            int turretsPerBase = 0;
            if (activeProtossCarrierCounter)
            {
                // A normal Carrier shell is eight per occupied base. A visible
                // mass wave may use all ten slots, but never creates an
                // unbounded global pile at one mineral line.
                turretsPerBase =
                    visibleDangerousEnemyAirUnits() >= 12 ? 10 : 8;
            }
            else if (protossNonCarrierAirPlan)
            {
                turretsPerBase = 6;
            }
            else if (genericAirPlan)
            {
                // Confirmed Wraith/Mutalisk/PureAir receives a complete first
                // ring at every working mineral line.
                turretsPerBase = 6;
            }
            else if (mixedAirGroundPlan)
            {
                // Goliaths remain the mobile answer in Mixed. Three mutually
                // covering Turrets are only the worker-line anchor.
                turretsPerBase = 3;
            }
            else if (usePredictiveAirPreparation)
            {
                // At >=70% confidence prepare the reversible worker arc only.
                // Current-game air evidence is still required for the outer ring.
                turretsPerBase = 3;
            }

            const std::vector<BWAPI::TilePosition> turretTiles =
                strategicAirDefenseTurretTiles(turretsPerBase);
            for (const BWAPI::TilePosition& tile : turretTiles)
            {
                airDefenseGoals.push_back(MetaPair(
                    MacroAct(
                        BWAPI::UnitTypes::Terran_Missile_Turret,
                        tile),
                    1));
            }

            if (usePredictiveAirPreparation &&
                !activeProtossCarrierCounter && !protossNonCarrierAirPlan &&
                !genericAirPlan && !mixedAirGroundPlan)
            {
                // A rolling Goliath/Tank target remains unfinished as the army
                // grows. Give the fixed three-Turret prediction package a
                // chance to start before those recurring unit goals.
                const auto workerGoal = std::find_if(
                    goal.begin(), goal.end(), [](const MetaPair& item)
                    {
                        return item.first.isUnit() &&
                            item.first.getUnitType() == BWAPI::UnitTypes::Terran_SCV;
                    });
                const auto insertAt = workerGoal == goal.end()
                    ? goal.begin() : workerGoal + 1;
                goal.insert(insertAt, airDefenseGoals.begin(), airDefenseGoals.end());
            }
            else
            {
                goal.insert(goal.end(), airDefenseGoals.begin(), airDefenseGoals.end());
            }
        }
     }
    else if (_openingGroup == "drop")
    {
        goal.push_back(std::pair<MacroAct, int>(BWAPI::UpgradeTypes::Ion_Thrusters, 1));
        goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Vulture, numVultures + 1));

        // The drop prep is carried out entirely by the opening book.
        // Immediately transition into something else.
        if (_enemyRace == BWAPI::Races::Zerg)
        {
            _openingGroup = "bio";
        }
        else
        {
            _openingGroup = "tanks";
        }
    }
    else
    {
        BWAPI::Broodwar->printf("Unknown Opening Group: %s", _openingGroup.c_str());
        _openingGroup = "bio";       // we're misconfigured, but try to do something
    }

    stage.next("shared support");
    // Normal Tank/Mech play needs two Armories as well: one for weapons and one
    // for plating. This used to be shared infrastructure, but the two-Armory
    // rule became scoped to the PureAir/Carrier branch and ordinary Tank plans
    // were left with zero or one. Stage construction so the first Armory can
    // start Weapons 1 immediately; request the second after the first completes.
    if (_openingGroup == "tanks" && !_pureAirPlan && numTanks >= 2)
    {
        const BuildOrderQueue& queue =
            ProductionManager::Instance().getQueue();

        const int armoriesPlanned =
            the.my.all.count(BWAPI::UnitTypes::Terran_Armory) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Armory) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Armory);

        const int completedArmories =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Armory);

        const int desiredArmories = completedArmories > 0 ? 2 : 1;
        if (armoriesPlanned < desiredArmories)
        {
            goal.push_back(MetaPair(
                MacroAct(BWAPI::UnitTypes::Terran_Armory, MacroLocation::Main),
                desiredArmories));
        }
    }

    if (_openingGroup != "tanks" && _openingGroup != "bio" &&
        airIntrusionSeen && numGoliaths >= 2 &&
        hasFactory)
    {
        const BuildOrderQueue& queue = ProductionManager::Instance().getQueue();
        const BWAPI::UnitType shop = BWAPI::UnitTypes::Terran_Machine_Shop;
        if (the.my.all.count(shop) == 0 &&
            BuildingManager::Instance().getNumUnstarted(shop) == 0 &&
            !queue.anyInQueue(shop))
        {
            goal.push_back(MetaPair(shop, 1));
        }
        if (hasArmory && the.my.completed.count(shop) > 0 &&
            self->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) == 0 &&
            !self->isUpgrading(BWAPI::UpgradeTypes::Charon_Boosters) &&
            !queue.anyInQueue(BWAPI::UpgradeTypes::Charon_Boosters))
        {
            goal.push_back(MetaPair(BWAPI::UpgradeTypes::Charon_Boosters, 1));
        }
    }

    // Battlecruisers are mechanical capital ships too. The old PureAir path
    // only built Ghost/Lockdown tech against Protoss Carriers, even though the
    // Terran PureAir contract is also Goliaths + Ghosts. Reuse the same staged
    // dependency chain, but do not buy an unnecessary Control Tower.
    if (terranCapitalAirGhostPlan)
    {
        appendTerranCarrierGhostPlan(
            goal,
            true,
            false);
    }

    // Carrier response shared by Bio and Tanks. Keeping it outside the opening
    // branches prevents one composition from silently losing Ghosts, turrets or
    // upgrade infrastructure when the same enemy technology is recognized.
    if (_pureAirPlan && activeProtossCarrierCounter)
    {
        appendTerranCarrierGhostPlan(
            goal,
            _openingGroup == "tanks",
            true);

        BuildOrderQueue& queue = ProductionManager::Instance().getQueue();

        const int armoriesPlanned =
            the.my.all.count(BWAPI::UnitTypes::Terran_Armory) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Armory) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Armory);

        const int completedArmories =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Armory);
        const int nextArmoryTarget =
            completedArmories <= 0 ? 1 : 2;

        if (armoriesPlanned < nextArmoryTarget)
        {
            goal.push_back(MetaPair(
                MacroAct(BWAPI::UnitTypes::Terran_Armory, MacroLocation::Main),
                nextArmoryTarget));
        }

        const int engineeringBaysPlanned =
            the.my.all.count(BWAPI::UnitTypes::Terran_Engineering_Bay) +
            (int)BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Engineering_Bay) +
            (int)queue.numInQueue(BWAPI::UnitTypes::Terran_Engineering_Bay);

        if (engineeringBaysPlanned <= 0)
        {
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Engineering_Bay, 1));
        }
        // Missile Turrets are owned by the tile-specific per-base ring above.
        // Do not append a second global count goal here: it has no base identity
        // and used to fight the positional plan after the fifth/tenth Turret.

        if (self->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) == 0 &&
            !self->isUpgrading(BWAPI::UpgradeTypes::Charon_Boosters) &&
            !queue.anyInQueue(BWAPI::UpgradeTypes::Charon_Boosters) &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Machine_Shop) > 0)
        {
            goal.push_back(MetaPair(BWAPI::UpgradeTypes::Charon_Boosters, 1));
        }

    }

    //if (numCC > 1 || InformationManager::Instance().enemyHasCloakTech())
    //{
    //    goal.push_back(std::pair<MacroAct, int>(BWAPI::UnitTypes::Terran_Academy, 1));
    //    if (numRefineries == 0)
    //    {
    //        goal.push_back(std::pair<MacroAct, int>(BWAPI::UnitTypes::Terran_Refinery, 1));
    //    }
    //}

    stage.next("vessel tech");
    const bool urgentDetectorNeeded =
        InformationManager::Instance().enemyHasCloakTech() ||
        enemyHasOpeningThreat(OpeningThreat::CloakTech) ||
        bioTankLurkerTankTarget() > 0 || currentCloakThreat ||
        learnedCloakPreparation ||
        carrierTechThreat;

    const bool protossEmpThreat =
        enemyRace == BWAPI::Races::Protoss &&
        (currentSpellcasterThreat ||
         currentCapitalAirThreat ||
         learnedSpellcasterPreparation ||
         learnedCapitalAirPreparation);

    const bool terranEmpThreat =
        enemyRace == BWAPI::Races::Terran &&
        (currentSpellcasterThreat ||
         currentCapitalAirThreat ||
         learnedSpellcasterPreparation ||
         learnedCapitalAirPreparation);

    const bool urgentEmpPlan =
        protossEmpThreat || terranEmpThreat;

    const bool zergMutaliskIrradiateThreat =
        enemyRace == BWAPI::Races::Zerg &&
        (
            enemyHasMutaliskTech() ||
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Mutalisk) > 0 ||
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Guardian) > 0 ||
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Devourer) > 0
        );

    const bool zergDefilerIrradiateThreat =
        enemyRace == BWAPI::Races::Zerg &&
        (
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Defiler_Mound) > 0 ||
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Defiler) > 0 ||
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Queen) > 0
        );

    const bool learnedZergSpellcasterPreparation =
        enemyRace == BWAPI::Races::Zerg &&
        learnedSpellcasterPreparation;

    // Mech funds its Goliath screen first. BioTank funds its Marine/Medic
    // screen instead; requiring Goliaths in a bio plan blocked this response.
    const bool bioIrradiateCoreReady = bioTankPlanActive() &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Marine) >= 16 &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Medic) >=
            BioProductionPolicy::medicTarget(16);
    const bool ordinaryBioTankSupport = _terranPlan == TerranPlanId::BioTank &&
        bioTankPlanActive();
    const bool bioVesselCoreReady = ordinaryBioTankSupport && numCCCompleted >= 2 &&
        numMarines >= 16 && numMedics >= BioProductionPolicy::medicTarget(16) && numTanks >= 2;
    const bool bioLurkerIrradiateThreat = bioTankLurkerTankTarget() >= 3;
    const bool urgentZergIrradiatePlan =
        (bioVesselCoreReady && enemyRace == BWAPI::Races::Zerg) ||
        (bioIrradiateCoreReady &&
            (zergMutaliskIrradiateThreat || bioLurkerIrradiateThreat)) ||
        (zergMutaliskIrradiateThreat && completedGoliathCount() >= 4) ||
        ((zergDefilerIrradiateThreat || learnedZergSpellcasterPreparation) &&
            (completedTankCount() >= 4 || numCC >= 2));

    int desiredScienceVessels =
        1 + numTanks / 16;

    desiredScienceVessels = std::min(desiredScienceVessels, 2);

    if (enemyRace == BWAPI::Races::Zerg && enemyAirThreatDetected)
    {
        desiredScienceVessels = std::min(desiredScienceVessels, 1);
    }

    if (urgentDetectorNeeded)
    {
        desiredScienceVessels = std::max(desiredScienceVessels, 1);
    }

    if (urgentEmpPlan || zergDefilerIrradiateThreat)
    {
        desiredScienceVessels = std::max(desiredScienceVessels, 2);
    }

    if (ordinaryBioTankSupport)
    {
        desiredScienceVessels = BioTankPolicy::vessels(numMarines, numCCCompleted,
            enemyRace == BWAPI::Races::Zerg, bioTankLurkerTankTarget() > 0,
            zergDefilerIrradiateThreat, urgentEmpPlan);
    }

    if (_pureAirPlan)
    {
        // One Vessel is enough for ordinary Mutalisk vision. Capital ships or
        // high-value spellcasters justify a second caster so one EMP/Irradiate
        // loss does not remove the whole counter.
        desiredScienceVessels =
            (urgentEmpPlan || zergDefilerIrradiateThreat) ? 2 : 1;
    }

    const bool gasAvailableForVesselTech =
        the.self()->gas() >= 250 ||
        urgentDetectorNeeded ||
        urgentZergIrradiatePlan ||
        urgentEmpPlan;

    const bool vesselTechAllowed =
        gasAvailableForVesselTech &&
        (
            urgentDetectorNeeded ||
            urgentZergIrradiatePlan ||
            urgentEmpPlan || bioVesselCoreReady ||
            (_pureAirPlan && completedGoliathCount() >= 8) ||
            (
                numCC >= 2 &&
                hasCompletedComsatStation() &&
                completedTankCount() >= 8
                )
            );

    if (vesselTechAllowed && !bioPolicy)
    {
        // The shared Carrier plan owns its staged technology chain. Repeating
        // Starport + Control Tower + Science Facility here recreated the full
        // dependency tree on the same transition frame.
        const bool carrierGhostPipelineOwnsVesselTech =
            _pureAirPlan && activeProtossCarrierCounter;

        const int desiredBioPorts = ordinaryBioTankSupport &&
            enemyRace == BWAPI::Races::Zerg &&
            (the.my.completed.count(BWAPI::UnitTypes::Terran_Starport) >= 2 ||
             (desiredScienceVessels >= 3 && numVessels >= 2 &&
              numSCVs >= 40 && self->gas() >= 300)) ? 2 : 1;
        if (!carrierGhostPipelineOwnsVesselTech)
        {
            BuildOrderQueue& vesselQueue =
                ProductionManager::Instance().getQueue();

            const bool starportExistsOrPlanned =
                the.my.all.count(BWAPI::UnitTypes::Terran_Starport) > 0 ||
                BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Starport) > 0 ||
                vesselQueue.anyInQueue(BWAPI::UnitTypes::Terran_Starport);

            if (!starportExistsOrPlanned ||
                (desiredBioPorts > 1 &&
                 the.my.all.count(BWAPI::UnitTypes::Terran_Starport) +
                 BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Starport) +
                 vesselQueue.numInQueue(BWAPI::UnitTypes::Terran_Starport) < desiredBioPorts))
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, desiredBioPorts));

            // Do not hand BOSS a complete multi-building dependency tree in
            // one search. Expose the Tower/Facility only after the Starport is
            // complete, then expose the Vessel after both are complete.
            if (the.my.completed.count(BWAPI::UnitTypes::Terran_Starport) > 0)
            {
                const bool controlTowerExistsOrPlanned =
                    the.my.all.count(BWAPI::UnitTypes::Terran_Control_Tower) > 0 ||
                    BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Control_Tower) > 0 ||
                    vesselQueue.anyInQueue(BWAPI::UnitTypes::Terran_Control_Tower);

                const int towerTarget = std::min(desiredBioPorts,
                    the.my.completed.count(BWAPI::UnitTypes::Terran_Starport));
                if (!controlTowerExistsOrPlanned ||
                    (towerTarget > 1 &&
                     the.my.all.count(BWAPI::UnitTypes::Terran_Control_Tower) +
                     BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Control_Tower) +
                     vesselQueue.numInQueue(BWAPI::UnitTypes::Terran_Control_Tower) < towerTarget))
                    goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Control_Tower, towerTarget));

                const bool scienceFacilityExistsOrPlanned =
                    the.my.all.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0 ||
                    BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Science_Facility) > 0 ||
                    vesselQueue.anyInQueue(BWAPI::UnitTypes::Terran_Science_Facility);

                if (!scienceFacilityExistsOrPlanned)
                {
                    goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Science_Facility, 1));
                }
            }
        }

        const bool vesselPrerequisitesCompleted =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Control_Tower) > 0 &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0;

        const int vesselLanes = std::min(desiredBioPorts,
            the.my.completed.count(BWAPI::UnitTypes::Terran_Control_Tower));
        const bool fundFirstIrradiate = ordinaryBioTankSupport &&
            enemyRace == BWAPI::Races::Zerg && numVessels > 0 &&
            !self->hasResearched(BWAPI::TechTypes::Irradiate) &&
            !self->isResearching(BWAPI::TechTypes::Irradiate);
        if (vesselPrerequisitesCompleted && !fundFirstIrradiate &&
            numVessels < desiredScienceVessels &&
            scienceVesselsCurrentlyTraining() < vesselLanes)
        {
            goal.push_back(std::pair<MacroAct, int>(
                BWAPI::UnitTypes::Terran_Science_Vessel,
                std::min(desiredScienceVessels,
                    numVessels + vesselLanes - scienceVesselsCurrentlyTraining())
            ));
        }


        if (urgentEmpPlan &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0 &&
            (numVessels > 0 || scienceVesselsCurrentlyTraining() > 0) &&
            !self->hasResearched(BWAPI::TechTypes::EMP_Shockwave) &&
            !self->isResearching(BWAPI::TechTypes::EMP_Shockwave) &&
            !techInQueue(ProductionManager::Instance().getQueue(), BWAPI::TechTypes::EMP_Shockwave))
        {
            goal.push_back(MetaPair(BWAPI::TechTypes::EMP_Shockwave, 1));
        }

        // Research while the first Vessel is training. Waiting for the Vessel
        // to finish added almost a minute to the response and left it with
        // Matrix only during the decisive Mutalisk/Defiler timing.
        if (urgentZergIrradiatePlan &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0 &&
            (numVessels > 0 || scienceVesselsCurrentlyTraining() > 0) &&
            !self->hasResearched(BWAPI::TechTypes::Irradiate) &&
            !self->isResearching(BWAPI::TechTypes::Irradiate) &&
            !techInQueue(ProductionManager::Instance().getQueue(), BWAPI::TechTypes::Irradiate))
        {
            goal.push_back(MetaPair(BWAPI::TechTypes::Irradiate, 1));
        }
    }

    stage.next("vehicle upgrades");
    // Continuous Terran vehicle upgrades. Tank/Mech and Carrier plans both
    // provide two Armories, allowing weapons and plating to run in parallel.
    if (hasArmory)
    {
        const BuildOrderQueue& queue =
            ProductionManager::Instance().getQueue();

        const bool hasCompletedScienceFacility =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0;

        const int vehicleWeapons =
            self->getUpgradeLevel(BWAPI::UpgradeTypes::Terran_Vehicle_Weapons);

        const int vehiclePlating =
            self->getUpgradeLevel(BWAPI::UpgradeTypes::Terran_Vehicle_Plating);

        int idleArmories = 0;
        for (BWAPI::Unit unit : BWAPI::Broodwar->self()->getUnits())
        {
            if (unit &&
                unit->exists() &&
                unit->isCompleted() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Armory &&
                !unit->isUpgrading())
            {
                ++idleArmories;
            }
        }

        int queuedArmoryUpgrades = 0;
        if (queue.anyInQueue(BWAPI::UpgradeTypes::Terran_Vehicle_Weapons))
        {
            ++queuedArmoryUpgrades;
        }
        if (queue.anyInQueue(BWAPI::UpgradeTypes::Terran_Vehicle_Plating))
        {
            ++queuedArmoryUpgrades;
        }
        int availableArmoryResearchSlots =
            std::max(0, idleArmories - queuedArmoryUpgrades);

        auto queueVehicleUpgradeIfPossible =
            [&](BWAPI::UpgradeType upgrade, int targetLevel) -> bool
            {
                if (availableArmoryResearchSlots <= 0)
                {
                    return false;
                }

                if (self->getUpgradeLevel(upgrade) >= targetLevel)
                {
                    return false;
                }

                if (self->isUpgrading(upgrade) || queue.anyInQueue(upgrade))
                {
                    return false;
                }

                // Level 2/3 vehicle upgrades need higher tech.
                // This prevents blocking the queue too early.
                if (targetLevel >= 2 && !hasCompletedScienceFacility)
                {
                    return false;
                }

                goal.push_back(MetaPair(upgrade, targetLevel));
                --availableArmoryResearchSlots;

                return true;
            };

        // Keep both lines moving. With two completed Armories, Weapons 1 and
        // Plating 1 start together; later levels still wait for Science Facility.

        if (vehicleWeapons < 1)
        {
            queueVehicleUpgradeIfPossible(
                BWAPI::UpgradeTypes::Terran_Vehicle_Weapons,
                1
            );

            queueVehicleUpgradeIfPossible(
                BWAPI::UpgradeTypes::Terran_Vehicle_Plating,
                1
            );
        }
        else if (vehicleWeapons < 2)
        {
            queueVehicleUpgradeIfPossible(
                BWAPI::UpgradeTypes::Terran_Vehicle_Weapons,
                2
            );

            // If we already have two Armories and Weapons 2 is active/queued,
            // start Plating 1 in parallel.
            queueVehicleUpgradeIfPossible(
                BWAPI::UpgradeTypes::Terran_Vehicle_Plating,
                1
            );
        }
        else if (vehiclePlating < 1)
        {
            queueVehicleUpgradeIfPossible(
                BWAPI::UpgradeTypes::Terran_Vehicle_Plating,
                1
            );
        }
        else if (vehicleWeapons < 3)
        {
            queueVehicleUpgradeIfPossible(
                BWAPI::UpgradeTypes::Terran_Vehicle_Weapons,
                3
            );

            // If there is a second Armory, continue Plating in parallel.
            queueVehicleUpgradeIfPossible(
                BWAPI::UpgradeTypes::Terran_Vehicle_Plating,
                2
            );
        }
        else if (vehiclePlating < 2)
        {
            queueVehicleUpgradeIfPossible(
                BWAPI::UpgradeTypes::Terran_Vehicle_Plating,
                2
            );
        }
        else if (vehiclePlating < 3)
        {
            queueVehicleUpgradeIfPossible(
                BWAPI::UpgradeTypes::Terran_Vehicle_Plating,
                3
            );
        }
    }

    stage.next("expansion finish");
    // Bio and other Terran opening groups use the same expansion decision.
    if (!expansionGoalAdded && wantTerranExpansion)
    {
        goal.push_back(MetaPair(
            BWAPI::UnitTypes::Terran_Command_Center,
            numCC + 1
        ));
    }

    // If the map has islands, get drop after we have 3 bases.
    if (Config::Macro::ExpandToIslands &&
        _openingGroup != "tanks" &&
        numCC >= 3 &&
        the.bases.hasIslandBases() &&
        !_pureAirPlan &&
        !enemyAirThreatDetected &&
        !enemyMixedAirGround &&
        !carrierTechThreat &&
        countDangerousEnemyAirUnits() == 0)
    {
        goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Dropship, 1));
    }

    stage.next("normalize");
    return normalizeTerranBuildOrderGoal(goal);
}

// BOSS method of choosing a zerg production plan. UNUSED!
// See freshProductionPlan() for the current method.


static int countAllOrQueuedUnit(
    BWAPI::UnitType type,
    const BuildOrderQueue& queue)
{
    return
        BWAPI::Broodwar->self()->allUnitCount(type) +
        (int)queue.numInQueue(type);
}

static int countCompletedCombatTanks()
{
    return
        BWAPI::Broodwar->self()->completedUnitCount(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
        BWAPI::Broodwar->self()->completedUnitCount(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
}

static bool dangerousEnemyAirVisibleForGoliathUrgent()
{
    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!enemy ||
            !enemy->exists() ||
            !enemy->isVisible() ||
            !enemy->isFlying())
        {
            continue;
        }

        const BWAPI::UnitType type = enemy->getType();

        if (type == BWAPI::UnitTypes::Zerg_Overlord ||
            type == BWAPI::UnitTypes::Zerg_Scourge ||
            type == BWAPI::UnitTypes::Zerg_Queen ||
            type == BWAPI::UnitTypes::Terran_Dropship ||
            type == BWAPI::UnitTypes::Terran_Science_Vessel ||
            type == BWAPI::UnitTypes::Protoss_Observer ||
            type == BWAPI::UnitTypes::Protoss_Shuttle ||
            type == BWAPI::UnitTypes::Protoss_Corsair ||
            type == BWAPI::UnitTypes::Protoss_Interceptor)
        {
            continue;
        }

        if (!UnitUtil::CanAttackGround(enemy))
        {
            continue;
        }

        return true;
    }

    return false;
}

static bool queueUrgentGoliathsForTankAirCover(BuildOrderQueue& queue)
{
    if (StrategyManager::Instance().getOpeningGroup() != "tanks")
    {
        return false;
    }

    const EnemyArmyMode enemyMode =
        currentEnemyArmyMode();

    const bool airMode =
        enemyMode == EnemyArmyMode::PureAir ||
        enemyMode == EnemyArmyMode::MixedAirHeavyGround ||
        enemyMode == EnemyArmyMode::MixedAirLightGround ||
        enemyHasCarrierTech();
    const bool flyerSeen = enemyFlyingUnitEverSeen();

    if (!airMode && !flyerSeen)
    {
        return false;
    }

    const bool dangerousAirNow =
        dangerousEnemyAirVisibleForGoliathUrgent() ||
        visibleDangerousEnemyAirUnits() > 0 ||
        enemyHasCarrierTech();
    if (!dangerousAirNow && !flyerSeen)
    {
        return false;
    }

    const int completedTanks = countCompletedCombatTanks();

    if (completedTanks < 4 && !enemyHasCarrierTech() &&
        !flyerSeen)
    {
        return false;
    }

    int desiredGoliaths = 0;

    if (!dangerousAirNow)
    {
        // A transport/scout sighting calls for a bounded four-unit reserve,
        // not the full anti-air composition of a confirmed combat-air army.
        desiredGoliaths = 4;
    }
    else if (enemyHasCarrierTech())
    {
        desiredGoliaths = 12;
    }
    else if (enemyMode == EnemyArmyMode::PureAir)
    {
        desiredGoliaths = 12;
    }
    else if (enemyMode == EnemyArmyMode::MixedAirHeavyGround ||
        enemyMode == EnemyArmyMode::MixedAirLightGround)
    {
        desiredGoliaths = mixedAirGoliathTarget(enemyMode);
    }
    else if (flyerSeen)
    {
        desiredGoliaths = 4;
    }
    else
    {
        return false;
    }

    const int currentOrQueuedGoliaths =
        the.my.all.count(BWAPI::UnitTypes::Terran_Goliath) +
        immediateQueuedUnitCount(
            queue, BWAPI::UnitTypes::Terran_Goliath, 3);

    if (currentOrQueuedGoliaths >= desiredGoliaths)
    {
        return false;
    }

    if (ensureUrgentGasIncome(queue, 50, "tank anti-air Goliath"))
    {
        return true;
    }

    const bool hasFactory =
        BWAPI::Broodwar->self()->completedUnitCount(BWAPI::UnitTypes::Terran_Factory) > 0;

    const bool hasArmory =
        BWAPI::Broodwar->self()->completedUnitCount(BWAPI::UnitTypes::Terran_Armory) > 0;

    const bool armoryPlannedOrBuilding =
        buildingExistsOrIsCommitted(
            BWAPI::UnitTypes::Terran_Armory, queue, 2);

    if (!hasFactory)
    {
        return false;
    }

    if (!hasArmory)
    {
        if (!armoryPlannedOrBuilding)
        {
            if (!promoteQueuedUnitToTop(queue, BWAPI::UnitTypes::Terran_Armory))
            {
                queue.queueAsHighestPriority(
                    MacroAct(BWAPI::UnitTypes::Terran_Armory, MacroLocation::Main));
            }
            return true;
        }

        return false;
    }

    const int supplyFree =
        BWAPI::Broodwar->self()->supplyTotal() -
        BWAPI::Broodwar->self()->supplyUsed();

    if (supplyFree <= 8 &&
        !queue.anyInQueue(BWAPI::UnitTypes::Terran_Supply_Depot))
    {
        return false;
    }

    const int queuedGoliaths =
        immediateQueuedUnitCount(
            queue, BWAPI::UnitTypes::Terran_Goliath, 2);

    if (queuedGoliaths >= 2)
    {
        return false;
    }

    if (!promoteQueuedUnitToTop(queue, BWAPI::UnitTypes::Terran_Goliath))
    {
        queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_Goliath);
    }

    return true;
}

static bool queueUrgentTerranWraithPreparation(BuildOrderQueue& queue)
{
    if (the.enemyRace() != BWAPI::Races::Terran ||
        StrategyManager::Instance().getOpeningGroup() != "tanks" ||
        StrategyManager::Instance().pureAirPlanActive() ||
        !terranWraithPreparationWindowOpen())
    {
        return false;
    }

    // This is a bounded preparation package, not a second macro planner. Add
    // at most one immediately useful prerequisite/action per frame and let the
    // ordinary Terran Plan continue producing its Tank core.
    if (!buildingExistsOrIsCommitted(
            BWAPI::UnitTypes::Terran_Engineering_Bay, queue, 3))
    {
        return requireUrgentBuildingNow(
            queue,
            BWAPI::UnitTypes::Terran_Engineering_Bay,
            1,
            MacroLocation::Main);
    }

    if (!buildingExistsOrIsCommitted(
            BWAPI::UnitTypes::Terran_Factory, queue, 3))
    {
        if (ensureUrgentGasIncome(queue, 100, "predicted Wraith Factory"))
        {
            return true;
        }
        return requireUrgentBuildingNow(
            queue,
            BWAPI::UnitTypes::Terran_Factory,
            1,
            MacroLocation::Main);
    }

    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Factory) > 0 &&
        !buildingExistsOrIsCommitted(
            BWAPI::UnitTypes::Terran_Armory, queue, 3))
    {
        if (ensureUrgentGasIncome(queue, 100, "predicted Wraith Armory"))
        {
            return true;
        }
        return requireUrgentBuildingNow(
            queue,
            BWAPI::UnitTypes::Terran_Armory,
            1,
            MacroLocation::Main);
    }

    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Engineering_Bay) > 0)
    {
        const std::vector<BWAPI::TilePosition> tiles =
            wraithDefenseTurretTiles();
        for (const BWAPI::TilePosition& tile : tiles)
        {
            if (turretCommittedNearTile(tile, 2))
            {
                continue;
            }

            queue.queueAsHighestPriority(
                MacroAct(BWAPI::UnitTypes::Terran_Missile_Turret, tile));
            DebugQueuePrintf(
                "AA ring: urgent Wraith worker slot (%d,%d)",
                tile.x, tile.y);
            return true;
        }
    }

    const int currentGoliaths =
        the.my.all.count(BWAPI::UnitTypes::Terran_Goliath);
    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Armory) > 0 &&
        currentGoliaths < 2)
    {
        if (ensureUrgentGasIncome(queue, 50, "predicted Wraith Goliath"))
        {
            return true;
        }
        requireUrgentCombatUnitNow(
            queue,
            BWAPI::UnitTypes::Terran_Goliath,
            2,
            2);
        return true;
    }

    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Factory) > 0 &&
        !buildingExistsOrIsCommitted(
            BWAPI::UnitTypes::Terran_Machine_Shop, queue, 3))
    {
        return requireUrgentBuildingNow(
            queue,
            BWAPI::UnitTypes::Terran_Machine_Shop,
            1,
            MacroLocation::Main);
    }

    // Once the first pair is in production, restore the Tank/Siege lane
    // before filling the full mobile AA target. This prevents Wraith
    // preparation itself from delaying the common Wraith -> Tank response.
    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Armory) > 0 &&
        currentGoliaths < 4)
    {
        if (ensureUrgentGasIncome(queue, 50, "predicted Wraith Goliath"))
        {
            return true;
        }
        requireUrgentCombatUnitNow(
            queue,
            BWAPI::UnitTypes::Terran_Goliath,
            4,
            2);
        return true;
    }

    BWAPI::Player self = the.self();
    if (currentGoliaths >= 2 &&
        the.my.completed.count(BWAPI::UnitTypes::Terran_Machine_Shop) > 0 &&
        self->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) == 0 &&
        !self->isUpgrading(BWAPI::UpgradeTypes::Charon_Boosters) &&
        !queue.anyInQueue(BWAPI::UpgradeTypes::Charon_Boosters))
    {
        if (!promoteQueuedUpgradeToTop(
                queue, BWAPI::UpgradeTypes::Charon_Boosters))
        {
            queue.queueAsHighestPriority(
                BWAPI::UpgradeTypes::Charon_Boosters);
        }
        return true;
    }

    return false;
}

// Comsat is prepared before this call. Get the first artillery pair and
// Siege started before a long Vessel prerequisite chain can monopolize urgent
// production. Further Tanks use the normal BioTank reinforcement lane.
static bool queueBioTankLurkerSiege(BuildOrderQueue& queue)
{
    const int target = bioTankLurkerTankTarget();
    if (target <= 0) return false;
    if (ensureUrgentGasIncome(queue, 100, "BioTank Lurker Siege")) return true;
    if (requireUrgentBuildingNow(queue, BWAPI::UnitTypes::Terran_Factory, 1)) return true;
    if (requireUrgentBuildingNow(queue, BWAPI::UnitTypes::Terran_Machine_Shop, 1)) return true;
    if (requireUrgentTechNow(queue, BWAPI::TechTypes::Tank_Siege_Mode)) return true;
    return requireUrgentCombatUnitNow(queue,
        BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode, std::min(2, target), 1);
}

static bool queueUrgentScienceVesselForCloak(BuildOrderQueue& queue)
{
    if (!InformationManager::Instance().enemyHasCloakTech() &&
        !enemyHasOpeningThreat(OpeningThreat::CloakTech) &&
        bioTankLurkerTankTarget() <= 0)
    {
        return false;
    }

    const int DESIRED_CLOAK_DETECTOR_VESSELS = 1;
    const int immediateVessels =
        the.my.all.count(BWAPI::UnitTypes::Terran_Science_Vessel) +
        immediateQueuedUnitCount(
            queue, BWAPI::UnitTypes::Terran_Science_Vessel, 2);

    if (immediateVessels >= DESIRED_CLOAK_DETECTOR_VESSELS)
    {
        return false;
    }

    if (ensureUrgentGasIncome(queue, 100, "Science Vessel detection"))
    {
        return true;
    }

    if (requireUrgentBuildingNow(
            queue, BWAPI::UnitTypes::Terran_Starport, 1))
    {
        return true;
    }

    if (the.my.completed.count(BWAPI::UnitTypes::Terran_Starport) <= 0)
    {
        return true;
    }

    if (requireUrgentBuildingNow(
            queue, BWAPI::UnitTypes::Terran_Control_Tower, 1))
    {
        return true;
    }

    if (requireUrgentBuildingNow(
            queue, BWAPI::UnitTypes::Terran_Science_Facility, 1))
    {
        return true;
    }

    requireUrgentCombatUnitNow(
        queue,
        BWAPI::UnitTypes::Terran_Science_Vessel,
        DESIRED_CLOAK_DETECTOR_VESSELS,
        1);
    return true;
}

static bool highestPriorityIsTech(
    BuildOrderQueue& queue,
    BWAPI::TechType tech)
{
    if (queue.isEmpty())
    {
        return false;
    }

    const MacroAct& top = queue.getHighestPriorityItem().macroAct;

    return top.isTech() && top.getTechType() == tech;
}

static bool highestPriorityIsUnit(
    BuildOrderQueue& queue,
    BWAPI::UnitType type)
{
    if (queue.isEmpty())
    {
        return false;
    }

    const MacroAct& top = queue.getHighestPriorityItem().macroAct;
    return top.isUnit() && top.getUnitType() == type;
}

// Legacy bio openings may change their macro host. BioTank owns its Siege,
// detection and Irradiate responses and keeps its infantry host even after
// confirmed Lurkers; this path must not add the mech ten-Tank gather gate.
bool StrategyManager::adaptTerranStrategyToEnemyPlan()
{
    if (!Config::Strategy::AdaptStrategyToEnemyPlan ||
        _selfRace != BWAPI::Races::Terran ||
        _openingGroup != "bio" ||
        _pureAirPlan ||
        _terranBaselinePlan == TerranPlanId::BioTank ||
        _terranBaselinePlan == TerranPlanId::TvP_DeepSix ||
        tvzBioPolicy(_terranBaselinePlan))
    {
        return false;
    }

    const BWAPI::Race enemyRace = the.enemyRace();
    const OpeningPlan recognizedEnemyPlan =
        OpponentModel::Instance().getEnemyPlan();
    const OpeningPlan enemyPlan =
        OpponentModel::Instance().getPreparationEnemyPlan();
    const bool predictedPlan =
        recognizedEnemyPlan == OpeningPlan::Unknown &&
        enemyPlan != OpeningPlan::Unknown;
    std::string reason;

    // A prediction may shape the stable post-book plan, but must not erase a
    // valid JSON opening. Current-game recognition is still allowed to make
    // the existing emergency conversion immediately.
    if (predictedPlan &&
        !ProductionManager::Instance().isOutOfBook())
    {
        return false;
    }

    if (enemyPlan == OpeningPlan::Contain)
    {
        reason = predictedPlan
            ? "70% predicted Opp Plan contain"
            : "Opp Plan contain";
    }
    else if (enemyRace == BWAPI::Races::Terran)
    {
        if (enemyHasOpeningThreat(OpeningThreat::TerranVultureRush) ||
            enemyPlan == OpeningPlan::VultureRush)
        {
            reason = "Opp Plan Vulture rush";
        }
        else if (enemyHasOpeningThreat(OpeningThreat::TerranTwoFactory))
        {
            reason = "Opp Plan 2 Factory";
        }
        else if (enemyPlan == OpeningPlan::Factory)
        {
            reason = predictedPlan
                ? "70% predicted Opp Plan Factory"
                : "Opp Plan Factory";
        }
    }
    else if (enemyRace == BWAPI::Races::Protoss)
    {
        if (enemyHasOpeningThreat(OpeningThreat::ProtossFourGateDragoon) ||
            (enemyHasOpeningThreat(OpeningThreat::GatewayPressure) &&
             enemyHasOpeningThreat(OpeningThreat::DragoonPressure)))
        {
            reason = "Opp Plan committed Dragoon pressure";
        }
        else if (enemyHasOpeningThreat(OpeningThreat::ProtossFastRobotics))
        {
            reason = "Opp Plan Fast Robotics";
        }
    }

    if (reason.empty())
    {
        return false;
    }

    TerranPlanId targetPlan = TerranPlanId::TvZ_Mech;
    if (enemyRace == BWAPI::Races::Protoss)
    {
        targetPlan = TerranPlanId::TvP_AntiDragoon;
    }
    else if (enemyRace == BWAPI::Races::Terran)
    {
        targetPlan = TerranPlanId::TvT_Anti2Factory;
    }
    else if (enemyRace == BWAPI::Races::Zerg)
    {
        targetPlan = TerranPlanId::TvZ_Mech;
    }

    switchTerranPlan(targetPlan, reason, false);

    // Keep the new production host as the baseline. The JSON opening still
    // names BioTank, so updateTerranPlan would otherwise restore it next frame.
    _terranBaselinePlan = targetPlan;
    _openingTerranPlanName = terranPlanProfile(targetPlan).name;
    _openingGroup = "tanks";
    _pureAirPlan = false;
    _defaultTankGather = true;
    _tankMassReleaseTanks = 10;
    _tankMassReleaseGoliaths = 0;

    // Discard any time-sliced search and cached bio rule wave before clearing
    // the unstarted JSON tail. Construction and training already in progress
    // are intentionally preserved.
    BOSSManager::Instance().reset();
    ProductionManager::Instance().invalidateTerranRulePlan(
        reason + ": bio -> tanks",
        true);
    ProductionManager::Instance().goOutOfBookAndClearQueue();
    CombatCommander::Instance().goDefensive();

    BWAPI::Broodwar->printf(
        "Strategy response: %s -> Tanks (%s)",
        Config::Strategy::StrategyName.c_str(),
        reason.c_str());

    return true;
}

static bool openingScoutCommandPending(BuildOrderQueue& queue)
{
    for (int i = 0; i < int(queue.size()); ++i)
    {
        const MacroAct& act = queue[i].macroAct;

        if (!act.isCommand())
        {
            continue;
        }

        const MacroCommandType command = act.getCommandType().getType();
        if (command == MacroCommandType::Scout ||
            command == MacroCommandType::ScoutIfNeeded ||
            command == MacroCommandType::ScoutLocation ||
            command == MacroCommandType::ScoutOnceOnly ||
            command == MacroCommandType::ScoutWhileSafe)
        {
            return true;
        }
    }

    return false;
}

static bool queueMainWallCloakTurret(BuildOrderQueue& queue)
{
    // Historical cloak is a 3-game/70% preparation signal. For this small
    // static detector it is useful during the JSON opening, not only after it.
    const bool cloakObserved =
        InformationManager::Instance().enemyCloakedUnitsSeen();
    const bool cloakExpected =
        OpponentModel::Instance().stronglyPredictsEnemyThreat(EnemyThreat::Cloak) ||
        InformationManager::Instance().enemyHasCloakTech() ||
        enemyHasOpeningThreat(OpeningThreat::CloakTech);
    if (!cloakExpected || !the.placer.hasTerranMainWall() ||
        (!cloakObserved &&
         (the.my.completed.count(BWAPI::UnitTypes::Terran_Barracks) == 0 ||
          the.my.completed.count(BWAPI::UnitTypes::Terran_Supply_Depot) == 0)) ||
        mainWallCloakTurretCommitted())
    {
        return false;
    }

    const BWAPI::UnitType engineeringBay =
        BWAPI::UnitTypes::Terran_Engineering_Bay;
    if (the.my.completed.count(engineeringBay) == 0)
    {
        // Do not queue a blocked Turret above an Engineering Bay which has
        // already started. The normal queue can run while it finishes.
        return requireUrgentBuildingNow(queue, engineeringBay, 1,
            MacroLocation::Main);
    }

    Base* main = the.bases.myMain();
    if (!main || committedTurretCountForBase(main) >= MAX_TURRETS_PER_OWNED_BASE)
    {
        return false;
    }

    static int lastFailedSearchFrame = -100000;
    const int now = BWAPI::Broodwar->getFrameCount();
    if (now >= lastFailedSearchFrame && now - lastFailedSearchFrame < 24)
    {
        return false;
    }

    const BWAPI::TilePosition tile = findMainWallCloakTurretTile();
    if (!tile.isValid())
    {
        lastFailedSearchFrame = now;
        return false;
    }

    queue.queueAsHighestPriority(
        MacroAct(BWAPI::UnitTypes::Terran_Missile_Turret, tile));
    DebugQueuePrintf("queue: cloak detector at main wall (%d,%d)",
        tile.x, tile.y);
    return true;
}

static bool queueHistoricalPureAirTurrets(BuildOrderQueue& queue)
{
    const OpponentModel& model = OpponentModel::Instance();
    // A 2-game/100% PureAir history is displayed to the player but fails the
    // general 3-game prediction gate. It is enough for a bounded main-base
    // turret floor, without authorizing a full PureAir army conversion.
    const bool strongHistory =
        model.stronglyPredictsStrategyClass(StrategyClass::PureAir) ||
        (Config::IO::ReadOpponentModel &&
         model.getLikelyStrategyClass() == StrategyClass::PureAir &&
         model.getLikelyStrategyClassGames() >= 2 &&
         model.getLikelyStrategyClassConfidence() >= 0.95);
    if (!strongHistory ||
        the.my.completed.count(BWAPI::UnitTypes::Terran_Factory) == 0)
    {
        return false;
    }

    Base* main = the.bases.myMain();
    const std::vector<Base*> ownedBases = ownedTerranBasesForTurrets();
    if (!main || std::find(ownedBases.begin(), ownedBases.end(), main) ==
            ownedBases.end() ||
        committedTurretCountForBase(main) >= 3)
    {
        return false;
    }

    const BWAPI::UnitType engineeringBay =
        BWAPI::UnitTypes::Terran_Engineering_Bay;
    if (the.my.completed.count(engineeringBay) == 0)
    {
        return requireUrgentBuildingNow(queue, engineeringBay, 1,
            MacroLocation::Main);
    }

    static int lastFailedSearchFrame = -100000;
    const int now = BWAPI::Broodwar->getFrameCount();
    if (now >= lastFailedSearchFrame && now - lastFailedSearchFrame < 24)
    {
        return false;
    }

    std::vector<BWAPI::TilePosition> tiles;
    addPerimeterTurretsAroundBase(
        tiles, main, 3, TURRET_RING_MIN_TILE_DISTANCE);
    for (const BWAPI::TilePosition& tile : tiles)
    {
        if (turretCommittedNearTile(tile, 2))
        {
            continue;
        }

        queue.queueAsHighestPriority(
            MacroAct(BWAPI::UnitTypes::Terran_Missile_Turret, tile));
        DebugQueuePrintf("queue: historical PureAir turret (%d,%d)",
            tile.x, tile.y);
        return true;
    }

    lastFailedSearchFrame = now;
    return false;
}

// The strategic AA ring is a long goal list. After an actual Mutalisk sighting,
// finish its first three main-base worker slots before another SCV, Factory or
// expansion can consume the last minerals. BioTank also prepares two worker
// Turrets per occupied base when Spire or 2 Hatch Muta is known.
static bool queueBioTankAirTurrets(BuildOrderQueue& queue)
{
    if (!StrategyManager::Instance().bioTankPlanActive())
    {
        return false;
    }

    const bool airExpected = enemyHasMutaliskTech() ||
        enemyHasCarrierTech() || terranWraithPreparationWindowOpen();
    const bool airObserved = visibleDangerousEnemyAirUnits() > 0 ||
        the.your.seen.count(BWAPI::UnitTypes::Zerg_Mutalisk) > 0 ||
        the.your.seen.count(BWAPI::UnitTypes::Zerg_Guardian) > 0 ||
        the.your.seen.count(BWAPI::UnitTypes::Terran_Wraith) > 0 ||
        the.your.seen.count(BWAPI::UnitTypes::Terran_Battlecruiser) > 0 ||
        the.your.seen.count(BWAPI::UnitTypes::Protoss_Carrier) > 0;
    if (!airExpected && !airObserved) return false;

    const int turretsPerBase = enemyHasCarrierTech() ? 4 :
        airObserved ? 3 : 2;

    const std::vector<Base*> bases = ownedTerranBasesForTurrets();
    bool needsWorkerCover = false;
    for (Base* base : bases)
    {
        if (committedTurretCountForBase(base) < turretsPerBase)
        {
            needsWorkerCover = true;
            break;
        }
    }
    if (!needsWorkerCover) return false;

    const BWAPI::UnitType bay = BWAPI::UnitTypes::Terran_Engineering_Bay;
    if (the.my.completed.count(bay) == 0)
    {
        if (requireUrgentBuildingNow(queue, bay, 1, MacroLocation::Main))
        {
            return true;
        }
        // The Bay is under construction. Keep Marines and Tanks producing
        // until Turrets are legal instead of blocking the top of the queue.
        return false;
    }

    const BWAPI::UnitType turret = BWAPI::UnitTypes::Terran_Missile_Turret;
    if (!queue.isEmpty())
    {
        const MacroAct& top = queue.getHighestPriorityItem().macroAct;
        if (top.isUnit() && top.getUnitType() == turret)
        {
            return true;
        }
    }

    for (Base* base : bases)
    {
        if (committedTurretCountForBase(base) >= turretsPerBase) continue;

        std::vector<BWAPI::TilePosition> tiles;
        addPerimeterTurretsAroundBase(
            tiles, base, turretsPerBase, TURRET_RING_MIN_TILE_DISTANCE);
        for (const BWAPI::TilePosition& tile : tiles)
        {
            if (turretCommittedNearTile(tile, 2)) continue;
            queue.queueAsHighestPriority(MacroAct(turret, tile));
            DebugQueuePrintf("queue: BioTank air worker turret (%d,%d)",
                tile.x, tile.y);
            return true;
        }
    }
    return false;
}

static bool queueConfirmedMutaliskTurrets(BuildOrderQueue& queue)
{
    if (the.enemyRace() != BWAPI::Races::Zerg ||
        the.your.seen.count(BWAPI::UnitTypes::Zerg_Mutalisk) <= 0)
    {
        return false;
    }

    Base* main = the.bases.myMain();
    if (!main || main->getOwner() != the.self()) return false;

    const BWAPI::UnitType bay = BWAPI::UnitTypes::Terran_Engineering_Bay;
    if (the.my.completed.count(bay) == 0)
    {
        return requireUrgentBuildingNow(queue, bay, 1,
            MacroLocation::Main);
    }

    const BWAPI::UnitType turret = BWAPI::UnitTypes::Terran_Missile_Turret;
    int startedAtMain = 0;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit && unit->exists() && unit->getType() == turret &&
            nearestOwnedTurretBase(unit->getTilePosition()) == main)
        {
            ++startedAtMain;
        }
    }
    for (const BWAPI::TilePosition& tile :
        BuildingManager::Instance().getUnstartedBuildingTiles(turret))
    {
        if (nearestOwnedTurretBase(tile) == main) ++startedAtMain;
    }
    if (startedAtMain >= 3) return false;

    if (!queue.isEmpty())
    {
        const MacroAct& top = queue.getHighestPriorityItem().macroAct;
        if (top.isUnit() && top.getUnitType() == turret &&
            (top.getMacroLocation() != MacroLocation::Tile ||
             nearestOwnedTurretBase(top.getTileLocation()) == main))
        {
            return true;
        }
    }
    for (int i = int(queue.size()) - 2; i >= 0; --i)
    {
        const MacroAct& act = queue[size_t(i)].macroAct;
        if (act.isUnit() && act.getUnitType() == turret &&
            act.getMacroLocation() == MacroLocation::Tile &&
            nearestOwnedTurretBase(act.getTileLocation()) == main)
        {
            queue.pullToTop(size_t(i));
            return true;
        }
    }

    static int lastTileSearchFrame = -100000;
    const int now = the.now();
    if (now >= lastTileSearchFrame && now - lastTileSearchFrame < 12)
    {
        return false;
    }
    lastTileSearchFrame = now;
    std::vector<BWAPI::TilePosition> tiles;
    addPerimeterTurretsAroundBase(tiles, main, 3,
        TURRET_RING_MIN_TILE_DISTANCE);
    for (const BWAPI::TilePosition& tile : tiles)
    {
        if (turretCommittedNearTile(tile, 2)) continue;
        queue.queueAsHighestPriority(MacroAct(turret, tile));
        DebugQueuePrintf("queue: confirmed Muta main turret (%d,%d)",
            tile.x, tile.y);
        return true;
    }
    return false;
}

// Production emergencies can return while a prerequisite is still building.
// Recompute BioTank's field order after that work on every production update,
// so a pending Vessel, Depot or upgrade cannot preserve an old defensive order.
void StrategyManager::updateBioTankAttackState()
{
    if (!bioTankPlanActive())
    {
        _bioTankAttackStatus.clear();
        return;
    }

    bool attack = false;
    if (!bioTankPushReady())
    {
        const auto* policy = tvzBioPolicy(_terranPlan);
        _bioTankAttackStatus = policy ? policy->waiting : deepSixPlanActive()
            ? "WAIT 24M/5Med/4T + Stim/Siege/Range/Armor1"
            : "WAIT mobile core together + Stim/Siege";
    }
    else
    {
        const NearbyEnemyThreats& nearby = visibleEnemyThreatsNearOwnedBases();
        if (_terranPlan == TerranPlanId::BioTank
            ? BioTankPolicy::homeDefenseRequired() : nearby.hasGround())
        {
            _bioTankAttackStatus = _terranPlan == TerranPlanId::BioTank
                ? "DEFEND base: local garrison outmatched" : "DEFEND ground threat at base";
        }
        else if (_terranPlan == TerranPlanId::TvZ_SK &&
            rememberedEnemyUnitCount(BWAPI::UnitTypes::Zerg_Lurker) > 0 &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Vessel) == 0)
        {
            _bioTankAttackStatus = "DEFEND replace Lurker detection Vessel";
        }
        else if (_terranPlan != TerranPlanId::BioTank && nearby.hasAir() &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Marine) < 14)
        {
            _bioTankAttackStatus = "DEFEND air threat: need Marines";
        }
        else
        {
            // Opening labels and their predictions are sticky. Once the core
            // is ready, a historical rush/contain alone must not park it until
            // 11/13 minutes. Local combat and base responders still run.
            attack = true;
            _bioTankAttackStatus = "ATTACK core ready";
            if (_bioFirstAttackFrame < 0) _bioFirstAttackFrame = the.now();
        }
    }
    CombatCommander::Instance().setAggression(attack);
}

bool StrategyManager::airTanksPlanActive() const
{
    return _selfRace == BWAPI::Races::Terran && _terranPlan == TerranPlanId::AirTanks;
}

bool StrategyManager::airTanksPushReady()
{
    if (!airTanksPlanActive() || _airTanksFirstPushReleased) return true;
    if (!AirTanks::coreTogether()) return false;
    _airTanksFirstPushReleased = true;
    return true;
}

bool StrategyManager::airTanksAirPushReady()
{
    if (!airTanksPlanActive()) return true;
    if (AirTanks::homeDefenseRequired()) return false;
    if (_airTanksAirReleased) return true;
    const auto variant = AirTanks::variant();
    if (variant == AirTanks::Variant::TankWraith)
    {
        // The one-port Wraiths are the Tank screen, not a separate early raid.
        if (!airTanksPushReady()) return false;
    }
    else if (variant == AirTanks::Variant::TwoPortWraith)
    {
        // Keep the dedicated two-port harassment opening independent.
        if (AirTanks::completed(BWAPI::UnitTypes::Terran_Wraith) < 2) return false;
    }
    else if (variant == AirTanks::Variant::Battlecruiser)
    {
        // Ground pressure does not wait for this expensive late-game transition.
        if (AirTanks::completed(BWAPI::UnitTypes::Terran_Battlecruiser) < 2) return false;
    }
    else if (!airTanksPushReady()) return false;
    _airTanksAirReleased = true;
    return true;
}

bool StrategyManager::airTanksAirEscortsGround() const
{
    if (!airTanksPlanActive()) return false;
    const auto variant = AirTanks::variant();
    return variant == AirTanks::Variant::TankWraith ||
        variant == AirTanks::Variant::Fantasy || variant == AirTanks::Variant::Valkonic ||
        variant == AirTanks::Variant::FastVessel;
}

void StrategyManager::updateAirTanksAttackState()
{
    if (!airTanksPlanActive())
    {
        _airTanksAttackStatus.clear();
        return;
    }
    bool attack = airTanksPushReady();
    if (!attack) _airTanksAttackStatus = std::string("WAIT AirTanks ") +
        (_airTanksVariant.empty() ? "tank_wraith" : _airTanksVariant) + " mobile core + tech";
    else if (AirTanks::homeDefenseRequired())
    {
        attack = false;
        _airTanksAttackStatus = "DEFEND local garrison outmatched";
    }
    else _airTanksAttackStatus = "ATTACK AirTanks core ready";
    CombatCommander::Instance().setAggression(attack);
}

// Production can return early for a bunker, add-on or replacement unit. The
// field order must still be updated after the army reaches its release size.
void StrategyManager::updateTankAttackState()
{
    if (_selfRace != BWAPI::Races::Terran || _openingGroup != "tanks" ||
        _pureAirPlan || !ProductionManager::Instance().isOutOfBook())
    {
        return;
    }

    const int tanks =
        the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
        the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
    const int goliaths =
        the.my.completed.count(BWAPI::UnitTypes::Terran_Goliath);
    const bool ready =
        tanks >= _tankMassReleaseTanks &&
        goliaths >= _tankMassReleaseGoliaths &&
        (tanks + goliaths) > 0;
    if (ready) _tankFirstAttackReleased = true;
    // Replacements join the existing push; one casualty does not recall it.
    CombatCommander::Instance().setAggression(
        _tankFirstAttackReleased && (tanks + goliaths) >= 2);
}

void StrategyManager::handleUrgentProductionIssues(BuildOrderQueue& queue)
{
    _slowestUrgentStage = "";
    _slowestUrgentStageMs = 0.0;
    // The outer ProductionManager timer only says "urgent". Keep the slowest
    // step even when an emergency branch returns early in this frame.
    struct UrgentStageTimer
    {
        const char *& slowestName;
        double& slowestMs;
        const char * currentName = "queue cleanup";
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        UrgentStageTimer(const char *& name, double& ms)
            : slowestName(name), slowestMs(ms) {}

        void next(const char * name)
        {
            const auto now = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - start).count();
            if (ms > slowestMs)
            {
                slowestName = currentName;
                slowestMs = ms;
            }
            currentName = name;
            start = now;
        }

        ~UrgentStageTimer() { next(""); }
    } stage(_slowestUrgentStage, _slowestUrgentStageMs);

    
    {
        if (removeObsoleteTerranInfantry(queue))
            ProductionManager::Instance().invalidateTerranRulePlan("defensive infantry target changed", false);
        for (int i = 0; i < 8; ++i)
        {
            bool removedSomething = false;

            if (removeCompletedTerranTechFromTop(queue))
            {
                removedSomething = true;
            }

            if (removeExcessTankScienceVesselFromTop(queue))
            {
                removedSomething = true;
            }

            if (removeExcessTankOpeningBarracksAnywhere(queue))
            {
                removedSomething = true;
            }

            if (removeExcessTankOpeningBunkersAnywhere(queue))
            {
                removedSomething = true;
            }

            if (removeExcessTankOpeningDefenseFromTop(queue))
            {
                removedSomething = true;
            }

            if (removeExcessUrgentTurretsFromTop(queue))
            {
                removedSomething = true;
            }

            if (removeExcessUrgentMarinesFromTop(queue))
            {
                removedSomething = true;
            }

            if (!removedSomething)
            {
                break;
            }
        }
    }

    // A hard supply block must be recovered before any opening scout, rush
    // preparation or plan transition can return early from this function.
    // The normal predictor below still handles supply before the hard block.
    const int supplyFrame = BWAPI::Broodwar->getFrameCount();
    const bool hardTerranSupplyBlock =
        _selfRace == BWAPI::Races::Terran &&
        the.my.all.count(BWAPI::UnitTypes::Terran_SCV) > 0 &&
        BWAPI::Broodwar->self()->supplyUsed() >=
            BWAPI::Broodwar->self()->supplyTotal() &&
        BWAPI::Broodwar->self()->supplyTotal() < 400;
    static int hardSupplyBlockedSince = -1;
    if (!hardTerranSupplyBlock ||
        supplyFrame < hardSupplyBlockedSince)
    {
        hardSupplyBlockedSince = -1;
    }
    else if (hardSupplyBlockedSince < 0)
    {
        hardSupplyBlockedSince = supplyFrame;
    }
    if (hardTerranSupplyBlock)
    {
        stage.next("hard supply recovery");
        if (promoteUrgentQueuedSupplyDepot(queue)) return;
        if (highestPriorityIsUnit(
                queue, BWAPI::UnitTypes::Terran_Supply_Depot)) return;
        if (urgentSupplyDepotsNeeded(queue) > 0)
        {
            queue.queueAsHighestPriority(MacroAct(
                BWAPI::UnitTypes::Terran_Supply_Depot,
                MacroLocation::Main));
            DebugQueuePrintf("queue: hard supply recovery Depot");
            return;
        }

        // An unstarted BuildingManager task can keep the emergency cap full
        // even if its worker never reaches a build tile. After a prolonged
        // hard block, try one alternative location; never enqueue a stream.
        const BWAPI::UnitType depot =
            BWAPI::UnitTypes::Terran_Supply_Depot;
        if (hardSupplyBlockedSince >= 0 &&
            supplyFrame - hardSupplyBlockedSince >= 45 * 24 &&
            BuildingManager::Instance().getNumUnstarted(depot) == 1 &&
            queue.numInQueue(depot) == 0 &&
            the.my.all.count(depot) == the.my.completed.count(depot))
        {
            queue.queueAsHighestPriority(MacroAct(depot, MacroLocation::Anywhere));
            DebugQueuePrintf("queue: rescue stalled Supply Depot at alternate location");
            return;
        }
    }

    stage.next("main wall bunker");
    if (_selfRace == BWAPI::Races::Terran &&
        queueBioTankMainWallBunker(queue))
    {
        return;
    }
    stage.next("natural bunker");
    if (_selfRace == BWAPI::Races::Terran &&
        queueNaturalExpansionBunker(queue))
    {
        return;
    }
    stage.next("opening checks");
    const OpponentModel& opponentModel = OpponentModel::Instance();
    const OpeningPlan observedPlan = opponentModel.getEnemyPlan();
    const OpeningPlan preparationPlan = opponentModel.getPreparationEnemyPlan();
    const bool rushFromHistory =
        opponentModel.stronglyPredictsEnemyPlan(OpeningPlan::WorkerRush) ||
        opponentModel.stronglyPredictsEnemyPlan(OpeningPlan::FastRush) ||
        opponentModel.stronglyPredictsEnemyPlan(OpeningPlan::HeavyRush) ||
        opponentModel.stronglyPredictsEarlyGroundRush();
    const bool prepareForHistoricalRush =
        _selfRace == BWAPI::Races::Terran &&
        (the.enemyRace() == BWAPI::Races::Protoss ||
         the.enemyRace() == BWAPI::Races::Zerg ||
         the.enemyRace() == BWAPI::Races::Terran) &&
        the.now() <= 10 * 60 * 24 &&
        rushFromHistory &&
        (observedPlan == OpeningPlan::Unknown ||
         observedPlan == OpeningPlan::WorkerRush ||
         observedPlan == OpeningPlan::FastRush ||
         observedPlan == OpeningPlan::HeavyRush) &&
        (preparationPlan == OpeningPlan::WorkerRush ||
         preparationPlan == OpeningPlan::FastRush ||
         preparationPlan == OpeningPlan::HeavyRush ||
         opponentModel.stronglyPredictsEarlyGroundRush());
    if (prepareForHistoricalRush)
    {
        // Finish the two-Depot wall as soon as its Barracks starts. Once the
        // Barracks completes, prioritize a Bunker and its Marine garrison even
        // if the JSON scout command is still waiting in the opening queue.
        if (queueEarlyRushWallDepot(queue))
        {
            return;
        }
        if (the.my.completed.count(BWAPI::UnitTypes::Terran_Barracks) > 0 &&
            queueEarlyRushBunkerAndMarines(queue))
        {
            return;
        }
    }

    if (queueHistoricalNoWallBunkerAndMarines(queue))
    {
        return;
    }

    // An observed cloaked attacker at an unreleased wall is an immediate
    // detection problem. Do not leave the Engineering Bay/Turret behind a
    // still-pending JSON scout command.
    if (_selfRace == BWAPI::Races::Terran &&
        the.placer.hasTerranMainWall() &&
        !the.placer.isTerranMainWallReleased() &&
        InformationManager::Instance().enemyCloakedUnitsSeen() &&
        queueMainWallCloakTurret(queue))
    {
        return;
    }

    // A JSON scout is an ordered opening action. Until it executes, no dynamic
    // emergency item may normally be inserted ahead of it and no opening item
    // may be removed around it. A currently recognized worker rush is the sole
    // exception: it is already inside our base, so preserving scout timing is
    // no longer worth blocking the bunker/Marine survival response.
    if (_selfRace == BWAPI::Races::Terran &&
        !ProductionManager::Instance().isOutOfBook() &&
        openingScoutCommandPending(queue) &&
        OpponentModel::Instance().getEnemyPlan() != OpeningPlan::WorkerRush)
    {
        return;
    }

    OpeningPlan enemyPlan = OpponentModel::Instance().getEnemyPlan();

    // Drop static defenses from the opening book if enemy is clearly playing macro.
    if (!ProductionManager::Instance().isOutOfBook() && !_openingStaticDefenseDropped)
    {
        const bool contradictoryThreatEvidence =
            enemyPlan == OpeningPlan::WorkerRush ||
            enemyPlan == OpeningPlan::Proxy ||
            enemyPlan == OpeningPlan::FastRush ||
            enemyPlan == OpeningPlan::HeavyRush ||
            enemyPlan == OpeningPlan::Contain ||
            enemyOpeningGroundPressure() ||
            actualEarlyGroundRushDetected() ||
            actualEarlyLightGroundRushDetected() ||
            actualEarlyProtossZealotRushDetected() ||
            visibleEnemyGroundNearMainOrClosestExpansion() ||
            visibleEnemyAirNearOwnedBase() ||
            InformationManager::Instance().enemyHasCloakTech() ||
            enemyHasOpeningThreat(OpeningThreat::CloakTech);

        if ((enemyPlan == OpeningPlan::Turtle ||
             enemyPlan == OpeningPlan::SafeExpand) &&
            !contradictoryThreatEvidence)
        {
            queue.dropStaticDefenses();

            for (BWAPI::Unit unit : the.self()->getUnits())
            {
                if (UnitUtil::IsComingStaticDefense(unit->getType()) &&
                    unit->canCancelConstruction())
                {
                    the.micro.Cancel(unit);
                }
            }

            _openingStaticDefenseDropped = true;
        }
    }

    

    const BWAPI::UnitType resourceDepotType =
        BWAPI::UnitTypes::Terran_Command_Center;

    const int numDepots = the.my.all.count(resourceDepotType);

    if (numDepots > 0 && handleExtremeEmergency(queue))
    {
        return;
    }

    const bool anyWorkers =
        the.my.all.count(
            _selfRace == BWAPI::Races::Terran
            ? BWAPI::UnitTypes::Terran_SCV
            : BWAPI::UnitTypes::Protoss_Probe
        ) > 0;

    // =========================
    // TERRAN URGENT CLEAN PART
    // =========================
    if (_selfRace == BWAPI::Races::Terran && anyWorkers)
    {
        stage.next("lifted landing");
        // A lifted production building cannot satisfy its queue. Restore it
        // before queue sanitizers mistake valid units/addons for a jam. This
        // is deliberately a command-only repair and does not add new items.
        requestNeededProductionBuildingLanding(queue);

        stage.next("update plan");

        const int nowFrame = BWAPI::Broodwar->getFrameCount();

        const bool wasPureAirPlan = _pureAirPlan;

        updateTerranPlan();

        stage.next("plan threats");

        const EnemyArmyMode enemyMode =
            currentEnemyArmyMode();

        const bool carrierTechThreat =
            enemyHasCarrierTech();

        const BWAPI::Race enemyRace =
            BWAPI::Broodwar->enemy()->getRace();

        const bool protossCarrierEmergency =
            enemyRace == BWAPI::Races::Protoss &&
            carrierTechThreat &&
            enemyMode == EnemyArmyMode::PureAir;

        const bool zergPureAirEmergency =
            enemyRace == BWAPI::Races::Zerg &&
            enemyMode == EnemyArmyMode::PureAir &&
            (_openingGroup != "bio" ||
             the.your.seen.count(BWAPI::UnitTypes::Zerg_Guardian) > 0);

        const bool currentPureAirCommitment =
            enemyMode == EnemyArmyMode::PureAir &&
            (enemyRace != BWAPI::Races::Zerg || zergPureAirEmergency);

        const bool learnedAntiCarrierCommitment =
            !currentPureAirCommitment &&
            selectedTvPAntiCarrierPlanCanCommit(enemyMode);

        const bool hardPureAirEmergency =
            _openingGroup != "bio" && !deepSixPlanActive() && !airTanksPlanActive() &&
            (currentPureAirCommitment || learnedAntiCarrierCommitment);

        const bool urgentPureAirPlan =
            hardPureAirEmergency;

        const bool enteringPureAirPlan =
            urgentPureAirPlan && !wasPureAirPlan;

        const bool leavingPureAirPlan =
            !urgentPureAirPlan && wasPureAirPlan;

        if (urgentPureAirPlan)
        {
            _pureAirPlan = true;
            _defaultTankGather = false;
            _tankMassReleaseTanks = 0;
            _tankMassReleaseGoliaths = 0;

            if (enteringPureAirPlan)
            {
                // A time-sliced BOSS search may still contain the former tank
                // goal. Never let that stale result finish after the queue has
                // already switched to Goliaths/Ghosts.
                BOSSManager::Instance().reset();
            }
        }
        else
        {
            _pureAirPlan = false;
            _defaultTankGather = _openingGroup == "tanks";
            const TerranPlanProfile& activePlan = terranPlanProfile(_terranPlan);
            _tankMassReleaseTanks = _defaultTankGather
                ? activePlan.id == TerranPlanId::None ? 15 : activePlan.tankRelease : 0;
            _tankMassReleaseGoliaths = _defaultTankGather
                ? activePlan.goliathRelease : 0;
        }

        if (enteringPureAirPlan)
        {
            const int removedConflicts =
                removePureAirConflictsFromQueue(queue);

            const int cancelledTraining =
                cancelPureAirConflictingTraining();

            // The persistent executor caches one strategic wave for up to a
            // second. Invalidate it now so the same frame cannot issue another
            // Tank from the pre-PureAir goal set.
            ProductionManager::Instance().invalidateTerranRulePlan(
                "strategy changed to PureAir",
                true);

            if (Config::Debug::DrawQueueFixInfo &&
                (removedConflicts > 0 || cancelledTraining > 0))
            {
                DebugQueuePrintf(
                    "PureAir transition: removed %d queued, cancelled %d training",
                    removedConflicts,
                    cancelledTraining);
            }
        }

        if (leavingPureAirPlan)
        {
            BOSSManager::Instance().reset();
            ProductionManager::Instance().invalidateTerranRulePlan(
                "PureAir evidence ended; restore mixed/ground production",
                true);
        }

        // Confirmed PureAir/Carrier is a strategy change. If recognized from
        // current-game evidence during the JSON opening, discard its unstarted
        // tail and let the Terran goal executor take over. A UCB prediction
        // waits for the book to finish in selectedTvPAntiCarrierPlanCanCommit().
        if (urgentPureAirPlan &&
            !ProductionManager::Instance().isOutOfBook())
        {
            ProductionManager::Instance().goOutOfBookAndClearQueue();
            CombatCommander::Instance().goDefensive();
            return;
        }

        // Opp Plan can now change the macro identity, not merely add one
        // emergency unit. PureAir has priority and is handled above.
        if (!urgentPureAirPlan && adaptTerranStrategyToEnemyPlan())
        {
            return;
        }

        const bool barracksProductionQueued =
            queue.anyInQueue(BWAPI::UnitTypes::Terran_Ghost) ||
            (!urgentPureAirPlan &&
                queue.anyInQueue(BWAPI::UnitTypes::Terran_Marine)) ||
            (!urgentPureAirPlan &&
                queue.anyInQueue(BWAPI::UnitTypes::Terran_Medic)) ||
            (!urgentPureAirPlan &&
                queue.anyInQueue(BWAPI::UnitTypes::Terran_Firebat));

        // Issue the landing before any PureAir/supply branch can return for
        // this frame. Queue work may continue while the Barracks flies.
        if (barracksProductionQueued && !hasCompletedLandedBarracks())
        {
            requestExistingBarracksLandForMarines();
        }

        // The released main-wall Barracks is still a Bio producer. Other
        // landed Barracks used to suppress the generic landing recovery,
        // leaving this one flying and idle for the remainder of the game.
        if (bioTankPlanActive() && the.placer.hasTerranMainWall() &&
            !ProductionManager::Instance().terranMainWallGateActive())
        {
            for (BWAPI::Unit unit : the.self()->getUnits())
            {
                if (unit && unit->exists() && unit->isCompleted() &&
                    unit->isLifted() &&
                    the.placer.isTerranMainWallBarracks(unit))
                {
                    requestExistingLiftedBuildingLand(
                        BWAPI::UnitTypes::Terran_Barracks);
                    break;
                }
            }
        }

        // Urgent rules run every frame. Without ownership of the current top
        // item, Supply Depot and Bunker alternately pulled each other upward
        // forever and neither reached ProductionManager. Let a top Bunker be
        // handed off first unless we are already hard supply-blocked.
        const bool hardSupplyBlocked =
            BWAPI::Broodwar->self()->supplyUsed() >=
            BWAPI::Broodwar->self()->supplyTotal() &&
            BWAPI::Broodwar->self()->supplyTotal() < 400;
        if (!hardSupplyBlocked &&
            highestPriorityIsUnit(
                queue, BWAPI::UnitTypes::Terran_Bunker))
        {
            const MacroAct& top =
                queue.getHighestPriorityItem().macroAct;
            waitForUrgentTerranPrerequisite(
                queue,
                top,
                "queued Bunker recovery");
            return;
        }

        stage.next("rush wall depot");
        if (queueEarlyRushWallDepot(queue))
        {
            return;
        }

        stage.next("supply checks");
        if (promoteUrgentQueuedSupplyDepot(queue))
        {
            return;
        }

        // The Depot now owns the queue top until ProductionManager consumes
        // it. Do not let a lower urgent rule pull a Bunker/Marine/tech above it
        // on the very next frame.
        if (highestPriorityIsUnit(
                queue, BWAPI::UnitTypes::Terran_Supply_Depot))
        {
            return;
        }

        const int urgentDepots =
            urgentSupplyDepotsNeeded(queue);

        if (urgentDepots > 0)
        {
            for (int i = 0; i < urgentDepots; ++i)
            {
                queue.queueAsHighestPriority(
                    MacroAct(BWAPI::UnitTypes::Terran_Supply_Depot, MacroLocation::Main)
                );
            }

            DebugQueuePrintf(
                "queue: urgent Supply Depot x%d",
                urgentDepots
            );

            return;
        }

        // Real combat emergencies must outrank economy and map-service jobs.
        // Previously a queued SCV/Benzene marine/SiegeExpand bunker could return
        // from this function while Zealots or other rush units were already at
        // the base.
        stage.next("nearby air response");
        if (!urgentPureAirPlan &&
            visibleEnemyAirNearOwnedBase() &&
            queueUrgentAntiAirForNearbyAttack(queue))
        {
            return;
        }

        stage.next("early rush response");
        if (!urgentPureAirPlan &&
            queueEarlyRushBunkerAndMarines(queue))
        {
            return;
        }

        stage.next("historical rush response");
        if (!urgentPureAirPlan &&
            queueHistoricalNoWallBunkerAndMarines(queue))
        {
            return;
        }

        stage.next("BioTank air turrets");
        if (queueBioTankAirTurrets(queue))
        {
            return;
        }

        stage.next("cloak wall turret");
        if (queueMainWallCloakTurret(queue))
        {
            return;
        }

        stage.next("pure-air history turrets");
        if (queueHistoricalPureAirTurrets(queue))
        {
            return;
        }

        stage.next("confirmed Muta turrets");
        if (queueConfirmedMutaliskTurrets(queue))
        {
            return;
        }

        stage.next("expansion check");
        const BWAPI::UnitType center =
            BWAPI::UnitTypes::Terran_Command_Center;
        const bool airTanksGasShortage =
            airTanksPlanActive() && the.self()->minerals() >= 900 &&
            the.self()->gas() <= 250;
        const bool gasBankCrisis =
            (urgentPureAirPlan || carrierTechThreat || airTanksGasShortage) &&
            the.self()->minerals() >= 900 &&
            the.self()->gas() <= (airTanksGasShortage ? 250 : 125) &&
            desiredRefineryCountForOwnedTerranBases() > 0 &&
            the.my.all.count(BWAPI::UnitTypes::Terran_Refinery) >=
                desiredRefineryCountForOwnedTerranBases();

        // Expansion scoring is comparatively expensive. Recheck a possible
        // gas site only once every two game seconds while the crisis lasts.
        static int lastGasSiteCheckFrame = -100000;
        static bool gasSiteAvailable = false;
        if (gasBankCrisis &&
            (nowFrame < lastGasSiteCheckFrame ||
             nowFrame - lastGasSiteCheckFrame >= 48))
        {
            lastGasSiteCheckFrame = nowFrame;
            const BWAPI::TilePosition gasTile =
                the.map.getNextExpansion(false, true, true);
            Base* gasBase = gasTile.isValid()
                ? the.bases.getBaseAtTilePosition(gasTile) : nullptr;
            // getNextExpansion() may fall back to a mineral-only base.
            gasSiteAvailable = gasBase && gasBase->getInitialGas() > 0 &&
                gasBase->getLastKnownGas() >= 500;
        }
        const bool gasExpansionEmergency = gasBankCrisis && gasSiteAvailable;
        const bool mineralExpansionEmergency =
            terranMineralExpansionEmergency() &&
            the.map.getNextExpansion(false, true, false).isValid();
        if ((gasExpansionEmergency || mineralExpansionEmergency) &&
            !BuildingManager::Instance().isBeingBuilt(
                center))
        {
            if (highestPriorityIsUnit(queue, center))
            {
                // Leave the item and its predicted builder in place while we
                // save up; promoting it again each frame resets prediction.
                return;
            }
            if (!promoteQueuedUnitToTop(queue, center))
            {
                queue.queueAsHighestPriority(center);
            }
            if (gasExpansionEmergency)
            {
                DebugQueuePrintf("queue: urgent gas expansion (min %d gas %d)",
                    the.self()->minerals(), the.self()->gas());
            }
            return;
        }

        stage.next("tech/defense");
        if (!urgentPureAirPlan &&
            queueUrgentTerranWraithPreparation(queue))
        {
            return;
        }

        if (urgentPureAirPlan)
        {
            // Goliaths, Armory, Charon and the later Ghost/Vessel chain all
            // consume gas. A PureAir transition during an early JSON opening
            // may happen before the opening Refinery, so explicitly restore gas
            // income instead of letting the first gas-cost item sit on top.
            if (ensureUrgentGasIncome(queue, 50, "PureAir response"))
            {
                CombatCommander::Instance().goDefensive();
                return;
            }

            // Add at most one support item per frame. Queueing several new
            // producers and upgrades on the transition frame made both the
            // executor and BOSS reason about an unnecessarily large dependency
            // set at once.
            bool queuedPureAirSupport =
                queuePureAirUpgradeInfrastructure(queue);

            if (!queuedPureAirSupport)
            {
                queuePureAirVehicleUpgrade(queue);
            }
        }

        if (!urgentPureAirPlan && queueBenzeneClearanceMarine(queue))
        {
            return;
        }

        stage.next("bunker placement");
        if (queueSiegeExpandBunker(queue))
        {
            return;
        }

        // Keep bases alive economically, but only after immediate combat
        // reactions have had the first chance to claim the queue.
        stage.next("workers/air cover");
        if (shouldQueueUrgentBaseSCV(queue))
        {
            queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_SCV);
            s_lastUrgentSCVQueueFrame = BWAPI::Broodwar->getFrameCount();
            return;
        }

        // Minimal air cover for tank group.
        // This is still acceptable as urgent because without AA tanks can die for free.
        if (!urgentPureAirPlan && queueUrgentGoliathsForTankAirCover(queue))
        {
            return;
        }

        // Detection is an emergency prerequisite when cloak/DT/Lurker tech is
        // known. A future Academy buried in JSON is not good enough: promote it
        // or create an immediate request before waiting for normal macro.
        const bool needEmergencyDetection =
            InformationManager::Instance().enemyHasCloakTech() ||
            enemyHasOpeningThreat(OpeningThreat::CloakTech) ||
            bioTankLurkerTankTarget() > 0;

        const bool academyExists =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Academy) > 0;

        if (!urgentPureAirPlan && needEmergencyDetection && !academyExists &&
            !buildingExistsOrIsCommitted(
                BWAPI::UnitTypes::Terran_Academy, queue, 2))
        {
            requireUrgentBuildingNow(
                queue,
                BWAPI::UnitTypes::Terran_Academy,
                1,
                MacroLocation::Main);
            return;
        }

        // Comsat on main CC as soon as Academy exists.

        if (!urgentPureAirPlan &&
            academyExists &&
            mainCommandCenterCanTakeComsat() &&
            !mainCommandCenterHasComsat() &&
            ProductionManager::Instance().getPendingGoalCount(
                BWAPI::UnitTypes::Terran_Comsat_Station) == 0 &&
            !BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Comsat_Station))
        {
            if (ensureUrgentGasIncome(queue, 50, "Comsat detection"))
            {
                return;
            }
            if (immediateQueuedUnitCount(
                    queue, BWAPI::UnitTypes::Terran_Comsat_Station, 2) <= 0)
            {
                if (!promoteQueuedUnitToTop(
                        queue, BWAPI::UnitTypes::Terran_Comsat_Station))
                {
                    queue.queueAsHighestPriority(
                        BWAPI::UnitTypes::Terran_Comsat_Station);
                }
                return;
            }
        }

        // BioTank can use Comsat while its first Siege pair clears a Lurker
        // contain. Do not bury that short response behind Starport/Facility.
        if (!urgentPureAirPlan && queueBioTankLurkerSiege(queue))
        {
            return;
        }

        // 6. Emergency Science Vessel only for cloak/detection.
        // Not for Carrier/PureAir tech plan.
        if (!urgentPureAirPlan && queueUrgentScienceVesselForCloak(queue))
        {
            return;
        }

        // 7. Retry natural if tank opening failed to expand.
        {
            const int numCommandCenters =
                the.my.all.count(BWAPI::UnitTypes::Terran_Command_Center);

            const int minute =
                BWAPI::Broodwar->getFrameCount() / (24 * 60);

            const bool commandCenterAlreadyPlanned =
                queue.anyInQueue(BWAPI::UnitTypes::Terran_Command_Center) ||
                BuildingManager::Instance().isBeingBuilt(BWAPI::UnitTypes::Terran_Command_Center);

            const OpeningPlan likelyPlanNow =
                OpponentModel::Instance().getPreparationEnemyPlan();

            const bool naturalUnsafeNow =
                enemyPlan == OpeningPlan::WorkerRush ||
                enemyPlan == OpeningPlan::Proxy ||
                enemyPlan == OpeningPlan::FastRush ||
                enemyPlan == OpeningPlan::HeavyRush ||
                enemyPlan == OpeningPlan::Contain ||
                likelyPlanNow == OpeningPlan::WorkerRush ||
                likelyPlanNow == OpeningPlan::Proxy ||
                likelyPlanNow == OpeningPlan::FastRush ||
                likelyPlanNow == OpeningPlan::HeavyRush ||
                likelyPlanNow == OpeningPlan::Contain ||
                enemyOpeningGroundPressure() ||
                actualEarlyGroundRushDetected() ||
                actualEarlyLightGroundRushDetected() ||
                actualEarlyProtossZealotRushDetected() ||
                visibleEnemyGroundNearMainOrClosestExpansion() ||
                visibleEnemyAirNearOwnedBase() ||
                InformationManager::Instance().enemyHasCloakTech();

            const int tanksForWallNatural =
                the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
                the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);

            const bool wallTankCoreReady =
                !the.placer.hasTerranMainWall() ||
                tanksForWallNatural >= 4;

            const bool shouldRetryNatural =
                !urgentPureAirPlan &&
                !naturalUnsafeNow &&
                _openingGroup == "tanks" &&
                wallTankCoreReady &&
                numCommandCenters < 2 &&
                minute >= 5 &&
                the.self()->minerals() >= 350 &&
                !commandCenterAlreadyPlanned;

            if (shouldRetryNatural)
            {
                queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_Command_Center);
                return;
            }
        }

        // Once the tank core and an Armory exist, keep affordable Weapons and
        // Plating research from sitting behind ordinary factory jobs. The
        // natural expansion above retains priority over these upgrades.
        if (!urgentPureAirPlan && queueTimelyTankVehicleUpgrade(queue))
        {
            return;
        }

        // =====================================================
        // PURE AIR / CARRIER: micro state + attack permission only.
        // Production is handled only by getTerranBuildOrderGoal() and the
        // persistent Terran rule executor.
        // =====================================================

        stage.next("army posture");
        const bool anyAirEmergency = urgentPureAirPlan;

        const int completedGoliaths = completedGoliathCount();
        const int completedTanks = completedTankCount();

        const int knownCarriers = rememberedEnemyCarrierCount();
        const int antiAirAggressiveGoliathThreshold =
            protossCarrierEmergency
            ? (knownCarriers <= 0
                ? 10
                : std::max(8, std::min(30, 3 * knownCarriers + 4)))
            : zergPureAirEmergency ? 8 : 10;

        const bool charonCompleted =
            BWAPI::Broodwar->self()->getUpgradeLevel(
                BWAPI::UpgradeTypes::Charon_Boosters) > 0;

        const bool mechAntiAirCoreReadyForAttack =
            completedGoliaths >= antiAirAggressiveGoliathThreshold &&
            (
                !protossCarrierEmergency ||
                charonCompleted
            );

        // Only the mech PureAir host reaches this threshold. BioTank keeps
        // its separate infantry, turret and capital-air reactions.
        const bool antiAirCoreReadyForAttack =
            mechAntiAirCoreReadyForAttack;

        static bool antiAirCoreCompletedOnce = false;

        if (!anyAirEmergency || completedGoliaths < 8)
        {
            antiAirCoreCompletedOnce = false;
        }

        if (anyAirEmergency && !antiAirCoreReadyForAttack)
        {
            static int lastPureAirDefensiveCommandFrame = -100000;

            if (nowFrame < lastPureAirDefensiveCommandFrame)
            {
                lastPureAirDefensiveCommandFrame = -100000;
            }

            if (nowFrame - lastPureAirDefensiveCommandFrame >= 24 * 5)
            {
                CombatCommander::Instance().goDefensive();
                lastPureAirDefensiveCommandFrame = nowFrame;
            }
        }

        if (anyAirEmergency && antiAirCoreReadyForAttack)
        {
            antiAirCoreCompletedOnce = true;

            if (ProductionManager::Instance().isOutOfBook())
            {
                static int lastPureAirAggressiveCommandFrame = -100000;

                if (nowFrame < lastPureAirAggressiveCommandFrame)
                {
                    lastPureAirAggressiveCommandFrame = -100000;
                }

                if (nowFrame - lastPureAirAggressiveCommandFrame >= 24 * 5)
                {
                    CombatCommander::Instance().goAggressive();
                    lastPureAirAggressiveCommandFrame = nowFrame;
                }
            }
        }

        // PureAir production is owned completely by getTerranBuildOrderGoal()
        // and ProductionManager's rule executor. The bank filler is only a
        // recovery aid for normal macro between plan refreshes.
        stage.next("banked army fill");
        if (!urgentPureAirPlan && queueBankedTerranArmyFill(queue))
        {
            return;
        }

        stage.next("turret checks");
        // Optional limited urgent turrets.
        // Keep it small. Turrets should not replace the strategic goal.
        {
            // A confirmed PureAir plan is exactly when emergency Turrets are
            // most valuable. The old !urgentPureAirPlan guard made the Zerg and
            // Carrier branches below unreachable at first contact.
            const bool shouldMakeUrgentAirTurrets =
                visibleDangerousEnemyAirUnits() >= 4;

            const bool enoughAntiAirArmyForTurrets =
                protossCarrierEmergency ||
                zergPureAirEmergency ||
                completedGoliaths >= 6;

            const bool turretProductionAlreadyActive =
                BuildingManager::Instance().isBeingBuilt(
                    BWAPI::UnitTypes::Terran_Missile_Turret) ||
                immediateQueuedUnitCount(
                    queue, BWAPI::UnitTypes::Terran_Missile_Turret, 3) > 0;

            static int lastUrgentTurretQueueFrame = -100000;

            if (nowFrame < lastUrgentTurretQueueFrame)
            {
                lastUrgentTurretQueueFrame = -100000;
            }

            if (shouldMakeUrgentAirTurrets &&
                enoughAntiAirArmyForTurrets &&
                !turretProductionAlreadyActive &&
                nowFrame - lastUrgentTurretQueueFrame >= 24 * 12)
            {
                const bool engineeringBayCommittedNow =
                    buildingExistsOrIsCommitted(
                        BWAPI::UnitTypes::Terran_Engineering_Bay, queue, 2);

                if (!engineeringBayCommittedNow)
                {
                    if (!promoteQueuedUnitToTop(
                            queue, BWAPI::UnitTypes::Terran_Engineering_Bay))
                    {
                        queue.queueAsHighestPriority(
                            MacroAct(
                                BWAPI::UnitTypes::Terran_Engineering_Bay,
                                MacroLocation::Main));
                    }
                    return;
                }

                if (waitForUrgentTerranPrerequisite(
                        queue,
                        MacroAct(BWAPI::UnitTypes::Terran_Missile_Turret),
                        "emergency Missile Turret"))
                {
                    return;
                }

                std::vector<BWAPI::TilePosition> turretTiles =
                    protossCarrierEmergency
                    ? carrierDefenseTurretTiles()
                    : zergPureAirEmergency
                    ? zergAirEmergencyTurretTiles()
                    : emergencyTurretTiles();

                // Queue only missing slots from the same strategic ring. Two
                // parallel SCVs are useful for a real Muta/Carrier wave; a
                // generic incident still posts one Turret at a time.
                const int urgentBatch =
                    (protossCarrierEmergency || zergPureAirEmergency) ? 2 : 1;
                int queuedTurrets = 0;
                for (const BWAPI::TilePosition& tile : turretTiles)
                {
                    if (turretCommittedNearTile(tile, 2))
                    {
                        continue;
                    }

                    Base* base = nearestOwnedTurretBase(tile);
                    if (!base ||
                        committedTurretCountForBase(base) >=
                            MAX_TURRETS_PER_OWNED_BASE)
                    {
                        continue;
                    }

                    queue.queueAsHighestPriority(
                        MacroAct(BWAPI::UnitTypes::Terran_Missile_Turret, tile));
                    ++queuedTurrets;
                    DebugQueuePrintf(
                        "AA ring: urgent slot base=%d tile=(%d,%d)",
                        base->getID(), tile.x, tile.y);

                    if (queuedTurrets >= urgentBatch)
                    {
                        break;
                    }
                }

                if (queuedTurrets > 0)
                {
                    lastUrgentTurretQueueFrame = nowFrame;
                    return;
                }
            }
        }

        // =========================
        // GROUND EMERGENCY SECTION
        // =========================

        stage.next("ground defense");
        // BioTank keeps Marines/Medics as its front line. A Hydra or Lurker
        // commitment needs a small Siege core, not the mech-only Vulture and
        // Factory targets below. Shared Bunker/Marine rush defense ran above.
        const bool bioZergGroundPressure =
            bioTankPlanActive() && bioTankUsesTanks() &&
            enemyRace == BWAPI::Races::Zerg &&
            (enemyHasOpeningThreat(OpeningThreat::HydraliskPressure) ||
             bioTankLurkerTankTarget() > 0);
        if (bioZergGroundPressure && !hardPureAirEmergency)
        {
            if (visibleEnemyGroundNearMainOrClosestExpansion() &&
                completedTanks < 3)
            {
                CombatCommander::Instance().goDefensive();
            }
            if (ensureUrgentGasIncome(queue, 100, "BioTank Siege defense"))
            {
                return;
            }
            if (requireUrgentBuildingNow(
                    queue, BWAPI::UnitTypes::Terran_Factory, 1))
            {
                return;
            }
            if (requireUrgentBuildingNow(
                    queue, BWAPI::UnitTypes::Terran_Machine_Shop, 1))
            {
                return;
            }
            if (requireUrgentTechNow(queue, BWAPI::TechTypes::Tank_Siege_Mode))
            {
                return;
            }
            if (requireUrgentCombatUnitNow(
                    queue,
                    BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode,
                    std::max(bioTankLurkerTankTarget(),
                        std::min(4, bioTankTankCeiling())),
                    2))
            {
                return;
            }
        }

        static bool terranSiegeWallDefenseActive = false;
        static int lastTerranSiegeWallSeenFrame = -100000;

        if (nowFrame < lastTerranSiegeWallSeenFrame)
        {
            terranSiegeWallDefenseActive = false;
            lastTerranSiegeWallSeenFrame = -100000;
        }

        const int visibleEnemyTanksNow =
            visibleEnemyTerranTankCount();

        const int visibleEnemySiegedTanksNow =
            visibleEnemyTerranSiegedTankCount();

        const int knownEnemyTanksNow =
            knownEnemyTerranTankCount();

        const bool enemySiegeWallVisible =
            BWAPI::Broodwar->enemy()->getRace() == BWAPI::Races::Terran &&
            visibleEnemyTanksNow >= 4 &&
            visibleEnemySiegedTanksNow >= 3;

        const bool enemySiegeWallReallyDangerous =
            enemySiegeWallVisible &&
            (
                completedTanks + 3 < knownEnemyTanksNow ||
                visibleEnemySiegedTanksNow >= 7 ||
                completedTanks < 14
                );

        if (enemySiegeWallVisible)
        {
            rememberVisibleEnemySiegeWallPosition();
        }
        else if (
            completedTanks >= 4 &&
            rememberedEnemySiegeWallPosition.isValid() &&
            nowFrame - rememberedEnemySiegeWallFrame > 24 * 3)
        {
            // Vision loss must not make an equal pre-sieged Terran wall look safe.
            // Scanner Sweep remains owned by StrategyManager; the helper itself
            // enforces the normal scan cooldown/energy checks.
            tryComsatRememberedEnemySiegeWall();
        }

        if (enemySiegeWallReallyDangerous)
        {
            terranSiegeWallDefenseActive = true;
            lastTerranSiegeWallSeenFrame = nowFrame;
        }

        const int desiredTanksVsSiegeWall =
            std::max(14, knownEnemyTanksNow + 4);

        const bool enoughTanksToBreakSiegeWall =
            completedTanks >= desiredTanksVsSiegeWall;

        const bool siegeWallMemoryExpired =
            nowFrame - lastTerranSiegeWallSeenFrame > 24 * 45;

        if (terranSiegeWallDefenseActive)
        {
            if (enoughTanksToBreakSiegeWall || siegeWallMemoryExpired)
            {
                terranSiegeWallDefenseActive = false;
            }
        }

        if ((_openingGroup == "tanks" || bioTankPlanActive()) &&
            terranSiegeWallDefenseActive && !hardPureAirEmergency)
        {
            if (ensureUrgentGasIncome(queue, 100, "Terran siege-wall defense"))
            {
                return;
            }

            if (nowFrame - lastTerranSiegeWallSeenFrame > 24 * 3)
            {
                tryComsatRememberedEnemySiegeWall();
            }

            if (requireUrgentBuildingNow(
                    queue, BWAPI::UnitTypes::Terran_Factory, 2))
            {
                return;
            }

            if (requireUrgentBuildingNow(
                    queue, BWAPI::UnitTypes::Terran_Machine_Shop, 1))
            {
                return;
            }

            if (requireUrgentTechNow(queue, BWAPI::TechTypes::Tank_Siege_Mode))
            {
                return;
            }

            if (requireUrgentCombatUnitNow(
                    queue,
                    BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode,
                    bioTankPlanActive()
                        ? std::min(bioTankTankCeiling(),
                            std::min(8, std::max(4, knownEnemyTanksNow + 2)))
                        : desiredTanksVsSiegeWall,
                    2))
            {
                return;
            }
        }

        static bool terranBioTankDefenseActive = false;
        static int lastTerranBioTankSeenFrame = -100000;

        if (nowFrame < lastTerranBioTankSeenFrame)
        {
            terranBioTankDefenseActive = false;
            lastTerranBioTankSeenFrame = -100000;
        }

        const bool enemyBioTankPushNow =
            enemyTerranBioTankPushDetected();

        if (enemyBioTankPushNow && !hardPureAirEmergency)
        {
            terranBioTankDefenseActive = true;
            lastTerranBioTankSeenFrame = nowFrame;
        }

        const int desiredTanksVsBioTank =
            std::max(12, knownEnemyTerranTankCount() + 4);

        const int desiredVulturesVsBioTank =
            10;

        const bool enoughArmyVsBioTank =
            completedTanks >= desiredTanksVsBioTank &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Vulture) >= desiredVulturesVsBioTank;

        const bool bioTankMemoryExpired =
            nowFrame - lastTerranBioTankSeenFrame > 24 * 75;

        if (terranBioTankDefenseActive)
        {
            if (enoughArmyVsBioTank || bioTankMemoryExpired)
            {
                terranBioTankDefenseActive = false;
            }
        }

        if ((_openingGroup == "tanks" || bioTankPlanActive()) &&
            terranBioTankDefenseActive && !hardPureAirEmergency)
        {
            if (ensureUrgentGasIncome(queue, 100, "Terran bio-tank defense"))
            {
                return;
            }

            if (requireUrgentBuildingNow(
                    queue, BWAPI::UnitTypes::Terran_Factory, 2))
            {
                return;
            }

            if (requireUrgentBuildingNow(
                    queue, BWAPI::UnitTypes::Terran_Machine_Shop, 1))
            {
                return;
            }

            if (requireUrgentTechNow(queue, BWAPI::TechTypes::Tank_Siege_Mode))
            {
                return;
            }

            if (!bioTankPlanActive() &&
                requireUrgentTechNow(queue, BWAPI::TechTypes::Spider_Mines))
            {
                return;
            }

            if (requireUrgentCombatUnitNow(
                    queue,
                    BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode,
                    bioTankPlanActive()
                        ? std::min(bioTankTankCeiling(),
                            std::min(8, std::max(4,
                                knownEnemyTerranTankCount() + 2)))
                        : desiredTanksVsBioTank,
                    2))
            {
                return;
            }

            if (!bioTankPlanActive() && requireUrgentCombatUnitNow(
                    queue,
                    BWAPI::UnitTypes::Terran_Vulture,
                    desiredVulturesVsBioTank,
                    2))
            {
                return;
            }
        }

        const bool protossDragoonPressureNow =
            actualProtossDragoonPressureNearBaseDetected();

        if ((_openingGroup == "tanks" || bioTankPlanActive()) &&
            protossDragoonPressureNow && !hardPureAirEmergency)
        {
            if (ensureUrgentGasIncome(queue, 100, "Protoss Dragoon defense"))
            {
                return;
            }

            // Committed Dragoon pressure is a Tank/Siege response. Keep only a
            // small Vulture layer for mines/body-blocking; the old target of six
            // competed directly with the HeavyRush rule that intentionally uses
            // only two Vultures versus Dragoons.
            if (requireUrgentBuildingNow(
                    queue, BWAPI::UnitTypes::Terran_Factory, 2))
            {
                return;
            }

            if (requireUrgentBuildingNow(
                    queue, BWAPI::UnitTypes::Terran_Machine_Shop, 1))
            {
                return;
            }

            if (requireUrgentTechNow(queue, BWAPI::TechTypes::Tank_Siege_Mode))
            {
                return;
            }

            if (!bioTankPlanActive() &&
                requireUrgentTechNow(queue, BWAPI::TechTypes::Spider_Mines))
            {
                return;
            }

            if (requireUrgentCombatUnitNow(
                    queue,
                    BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode,
                    bioTankPlanActive()
                        ? std::min(4, bioTankTankCeiling()) : 6,
                    2))
            {
                return;
            }

            if (!bioTankPlanActive() && requireUrgentCombatUnitNow(
                    queue,
                    BWAPI::UnitTypes::Terran_Vulture,
                    2,
                    2))
            {
                return;
            }
        }

        const bool enemyGroundNearBase =
            visibleEnemyGroundNearMainOrClosestExpansion();

        const OpeningPlan bunkerEnemyPlan =
            OpponentModel::Instance().getEnemyPlan();

        const OpeningPlan bunkerLikelyEnemyPlan =
            OpponentModel::Instance().getPreparationEnemyPlan();

        const bool bunkerEnemyRushPlan =
            bunkerEnemyPlan == OpeningPlan::HeavyRush ||
            bunkerLikelyEnemyPlan == OpeningPlan::HeavyRush ||
            bunkerEnemyPlan == OpeningPlan::FastRush ||
            bunkerLikelyEnemyPlan == OpeningPlan::FastRush ||
            bunkerEnemyPlan == OpeningPlan::Proxy ||
            bunkerLikelyEnemyPlan == OpeningPlan::Proxy ||
            enemyOpeningRushPressure() ||
            enemyHasOpeningThreat(OpeningThreat::ProtossOneGateCore) ||
            actualEarlyProtossZealotRushDetected() ||
            actualEarlyGroundRushDetected() ||
            actualEarlyLightGroundRushDetected();

        if (!hardPureAirEmergency &&
            queueForwardBunkerForGroundRush(
                queue,
                enemyMode,
                enemyGroundNearBase,
                bunkerEnemyRushPlan,
                anyWorkers))
        {
            CombatCommander::Instance().goDefensive();
            return;
        }

        const OpeningPlan enemyPlanNow =
            OpponentModel::Instance().getEnemyPlan();

        const OpeningPlan likelyEnemyPlanNow =
            OpponentModel::Instance().getPreparationEnemyPlan();

        const bool enemyHeavyRushNow =
            enemyPlanNow == OpeningPlan::HeavyRush ||
            likelyEnemyPlanNow == OpeningPlan::HeavyRush ||
            enemyOpeningRushPressure() ||
            actualEarlyGroundRushDetected();

        // LingMutaSafe already contains its complete anti-Ling and anti-Muta
        // infrastructure sequence. Do not replace that deterministic JSON tail
        // with rule goals merely because the first Factory (or the Vulture
        // floor) is ready: doing so skipped Armory, Academy, Comsat, EBay,
        // Siege and the natural. A confirmed PureAir army still leaves the book
        // immediately in the dedicated emergency transition above.

        // If nothing urgent says otherwise, let the bot attack normally.
        const bool heavyRushDefenseStillActive =
            enemyHeavyRushNow &&
            the.now() <= 11 * 24 * 60;

        const bool containDefenseStillActive =
            (enemyPlanNow == OpeningPlan::Contain ||
             likelyEnemyPlanNow == OpeningPlan::Contain) &&
            (the.now() <= 13 * 24 * 60 ||
             visibleEnemyGroundNearMainOrClosestExpansion());

        // The JSON push command is a permission, not a reason to send a few
        // unhealed Marines across the map. Keep this readiness contract local
        // to BioTank; the Tank opening retains its existing release logic.
        const bool bioPushReady = bioTankPushReady();

        const bool shouldStayDefensiveNow =
            !bioPushReady ||
            (_openingGroup == "tanks" && terranBioTankDefenseActive) ||
            heavyRushDefenseStillActive ||
            containDefenseStillActive ||
            (enemyOpeningGroundPressure() && the.now() <= 11 * 24 * 60) ||
            (
                anyAirEmergency &&
                !antiAirCoreCompletedOnce &&
                !antiAirCoreReadyForAttack
                );

        // BioTank's decision runs after urgent production, including frames
        // where an earlier prerequisite branch returns from this function.
        if (!bioTankPlanActive() && !shouldStayDefensiveNow &&
            ProductionManager::Instance().isOutOfBook())
        {
            CombatCommander::Instance().goAggressive();
        }

        // Emergency scan.
        {
            const bool needEmergencyScan =
                InformationManager::Instance().enemyHasCloakTech() ||
                enemyHasOpeningThreat(OpeningThreat::CloakTech);

            const int numCCCompleted =
                the.my.completed.count(BWAPI::UnitTypes::Terran_Command_Center);

            const int numComsatAll =
                the.my.all.count(BWAPI::UnitTypes::Terran_Comsat_Station);

            if (needEmergencyScan &&
                academyExists &&
                numCCCompleted > 0 &&
                numComsatAll < numCCCompleted)
            {
                if (immediateQueuedUnitCount(
                        queue, BWAPI::UnitTypes::Terran_Comsat_Station, 2) <= 0)
                {
                    if (!promoteQueuedUnitToTop(
                            queue, BWAPI::UnitTypes::Terran_Comsat_Station))
                    {
                        queue.queueAsHighestPriority(
                            BWAPI::UnitTypes::Terran_Comsat_Station);
                    }
                    return;
                }
            }
        }
    }

    // =========================
    // COMMON NON-TERRAN / FALLBACK
    // =========================

    

    const MacroAct* nextInQueuePtr =
        queue.isEmpty() ? nullptr : &(queue.getHighestPriorityItem().macroAct);

    // If we need gas, make sure it is turned on.
    int gas = the.self()->gas();

    if (nextInQueuePtr)
    {
        if (nextInQueuePtr->gasPrice() > gas)
        {
            WorkerManager::Instance().setCollectGas(true);
        }
    }

    // If protoss building is stalled for lack of space, schedule a pylon.
    if (BuildingManager::Instance().getStalledForLackOfSpace())
    {
        
    }

    // If we have collected too much gas, turn it off.
    if (ProductionManager::Instance().isOutOfBook() &&
        gas > 400 &&
        gas > 4 * the.self()->minerals())
    {
        if (_selfRace == BWAPI::Races::Terran && _openingGroup == "tanks")
        {
            WorkerManager::Instance().setCollectGas(true);
        }
        else
        {
            int queueMinerals = 0;
            int queueGas = 0;
            queue.totalCosts(queueMinerals, queueGas);

            if (gas >= queueGas)
            {
                WorkerManager::Instance().setCollectGas(false);
            }
        }
    }

    OpeningPlan likelyEnemyPlan =
        OpponentModel::Instance().getPreparationEnemyPlan();

    if (likelyEnemyPlan == OpeningPlan::Proxy ||
        likelyEnemyPlan == OpeningPlan::WorkerRush ||
        likelyEnemyPlan == OpeningPlan::FastRush ||
        likelyEnemyPlan == OpeningPlan::HeavyRush)
    {
        if (_selfRace == BWAPI::Races::Terran &&
            StrategyManager::Instance().getOpeningGroup() != "tanks")
        {
            const bool bunkerCommittedNow =
                the.my.all.count(BWAPI::UnitTypes::Terran_Bunker) > 0 ||
                BuildingManager::Instance().isBeingBuilt(
                    BWAPI::UnitTypes::Terran_Bunker) ||
                BuildingManager::Instance().getNumUnstarted(
                    BWAPI::UnitTypes::Terran_Bunker) > 0 ||
                immediateQueuedUnitCount(
                    queue, BWAPI::UnitTypes::Terran_Bunker, 2) > 0;

            if (!bunkerCommittedNow &&
                the.my.all.count(BWAPI::UnitTypes::Terran_Marine) > 0 &&
                the.my.completed.count(BWAPI::UnitTypes::Terran_Barracks) > 0 &&
                anyWorkers)
            {
                if (!promoteQueuedUnitToTop(
                        queue, BWAPI::UnitTypes::Terran_Bunker))
                {
                    queue.queueAsHighestPriority(
                        MacroAct(
                            BWAPI::UnitTypes::Terran_Bunker,
                            MacroLocation::Front));
                }
                return;
            }
        }
    }

    if (numDepots > _highWaterBases)
    {
        _highWaterBases = numDepots;
    }

    bool makeResourceDepot = false;

    if (numDepots == 0 && the.self()->minerals() >= 400)
    {
        makeResourceDepot = true;
    }

    const OpeningPlan economicEnemyPlan =
        OpponentModel::Instance().getPreparationEnemyPlan();

    const bool predictedEconomicPlan =
        OpponentModel::Instance().getEnemyPlan() == OpeningPlan::Unknown &&
        economicEnemyPlan != OpeningPlan::Unknown;

    if ((!predictedEconomicPlan ||
         ProductionManager::Instance().isOutOfBook()) &&
        (economicEnemyPlan == OpeningPlan::SafeExpand ||
        economicEnemyPlan == OpeningPlan::NakedExpand ||
        economicEnemyPlan == OpeningPlan::Turtle))
    {
        const bool expansionUnsafeNow =
            enemyOpeningGroundPressure() ||
            actualEarlyGroundRushDetected() ||
            actualEarlyLightGroundRushDetected() ||
            actualEarlyProtossZealotRushDetected() ||
            visibleEnemyGroundNearMainOrClosestExpansion() ||
            visibleEnemyAirNearOwnedBase() ||
            InformationManager::Instance().enemyHasCloakTech() ||
            enemyHasOpeningThreat(OpeningThreat::CloakTech);

        const bool normalZergExpansion =
            the.enemy()->getRace() == BWAPI::Races::Zerg &&
            economicEnemyPlan != OpeningPlan::Turtle;

        if (!expansionUnsafeNow &&
            _highWaterBases == 1 &&
            !normalZergExpansion)
        {
            makeResourceDepot = true;
        }
    }

    if (makeResourceDepot &&
        anyWorkers &&
        (!nextInQueuePtr ||
            !nextInQueuePtr->isUnit() ||
            nextInQueuePtr->getUnitType() != resourceDepotType) &&
        !BuildingManager::Instance().isBeingBuilt(resourceDepotType))
    {
        queue.queueAsHighestPriority(MacroAct(resourceDepotType));
        return;
    }
}

// Return true if we're supply blocked and should build supply.
// NOTE This understands zerg supply but is not used when we are zerg.
bool StrategyManager::detectSupplyBlock(BuildOrderQueue & queue) const
{
    // If the _queue is empty or supply is maxed, there is no block.
    if (queue.isEmpty() || the.self()->supplyTotal() >= 400)
    {
        return false;
    }

    // If supply is being built now, there's no block. Return right away.
    // Terran and protoss calculation:
    if (BuildingManager::Instance().isBeingBuilt(the.self()->getRace().getSupplyProvider()))
    {
        return false;
    }

    // Terran and protoss calculation:
    int supplyAvailable = the.self()->supplyTotal() - the.self()->supplyUsed();

    // Zerg calculation:
    // Zerg can create an overlord that doesn't count toward supply until the next check.
    // To work around it, add up the supply by hand, including hatcheries.
    

    int supplyCost = queue.getHighestPriorityItem().macroAct.supplyRequired();
    // Available supply can be negative, which breaks the test below. Fix it.
    supplyAvailable = std::max(0, supplyAvailable);

    // if we don't have enough supply, we're supply blocked
    if (supplyAvailable < supplyCost)
    {
        // If we're zerg, check to see if a building is planned to be built.
        // Only count it as releasing supply very early in the game.
        
        return true;
    }

    return false;
}

// This tries to cope with 1 kind of severe emergency: We have desperately few workers.
// The caller promises that we have a resource depot, so we may be able to make more.
bool StrategyManager::handleExtremeEmergency(BuildOrderQueue & queue)
{
    const int minWorkers = 3;
    const BWAPI::UnitType workerType = _selfRace.getWorker();
    const int nWorkers = the.my.all.count(workerType);

    // NOTE This doesn't check whether the map has resources remaining!
    //      If not, we should produce workers only if needed for another purpose.
    // NOTE If we don't have enough minerals to make a worker, then we don't
    //      have enough minerals to make anything (since we're not zerg and can't make scourge).
    //      So don't bother.
    if (nWorkers < minWorkers && the.self()->minerals() >= 50)
    {
        // 1. If the next item in the queue is a worker, we're good. Otherwise, clear the queue.
        // This is a severe emergency and it doesn't make sense to continue business as usual.
        // But if we don't have enough 
        if (queue.size() > 0)
        {
            const MacroAct & act = queue.getHighestPriorityItem().macroAct;
            if (act.isUnit() && act.getUnitType() == workerType)
            {
                return false;
            }
        }
        // 2. Queue the minimum number of workers.
        for (int i = nWorkers; i < minWorkers; ++i)
        {
            queue.queueAsHighestPriority(workerType);
        }
        return true;
    }

    return false;
}

// Called to refill the production queue when it is empty.
void StrategyManager::freshProductionPlan()
{
    performBuildOrderSearch();
}

void StrategyManager::performBuildOrderSearch()
{
    // Remember whether this planner actually started the currently finishing
    // search. BOSS exposes an empty build order both before any search and after
    // a failed/trivial search, so checking getBuildOrder() alone cannot tell the
    // two states apart.
    static bool waitingForBossResult = false;
    static int lastPlannerFrame = -1;

    const int now = BWAPI::Broodwar->getFrameCount();

    // Static locals survive between games in the same process.
    if (now < lastPlannerFrame)
    {
        waitingForBossResult = false;
        BOSSManager::Instance().reset();
    }

    lastPlannerFrame = now;

    if (!canPlanBuildOrderNow())
    {
        return;
    }

    BuildOrder buildOrder = BOSSManager::Instance().getBuildOrder();

    if (buildOrder.size() > 0)
    {
        ProductionManager::Instance().setBuildOrder(buildOrder);
        BOSSManager::Instance().reset();
        waitingForBossResult = false;
        return;
    }

    if (BOSSManager::Instance().isSearchInProgress())
    {
        waitingForBossResult = true;
        return;
    }

    if (waitingForBossResult)
    {
        // The search finished but produced no actions. Do not submit the same
        // failing goal forever while every production building remains idle.
        BOSSManager::Instance().reset();
        waitingForBossResult = false;

        
        {
            BuildOrder recovery = makeTerranSearchRecoveryBuildOrder(
                _openingGroup,
                _pureAirPlan);

            if (recovery.size() > 0)
            {
                ProductionManager::Instance().setBuildOrder(recovery);
                return;
            }
        }
    }

    // The persistent Terran rule executor already owns PureAir production.
    // If that executor temporarily cannot post a legal item, use its small
    // prerequisite-safe recovery wave instead of sending the large, rapidly
    // changing Carrier goal to the legacy BOSS search. The latter was the only
    // transition path capable of unloading the bot around the frame Carrier
    // technology/units became known.
    if (_selfRace == BWAPI::Races::Terran && _pureAirPlan)
    {
        BuildOrder recovery = makeTerranSearchRecoveryBuildOrder(
            _openingGroup,
            true);

        if (recovery.size() > 0)
        {
            ProductionManager::Instance().setBuildOrder(recovery);
        }

        waitingForBossResult = false;
        BOSSManager::Instance().reset();
        return;
    }

    const MetaPairVector goal = getBuildOrderGoal();

    if (goal.empty())
    {
        
        {
            BuildOrder recovery = makeTerranSearchRecoveryBuildOrder(
                _openingGroup,
                _pureAirPlan);

            if (recovery.size() > 0)
            {
                ProductionManager::Instance().setBuildOrder(recovery);
            }
        }

        return;
    }

    // Set this before startNewSearch(). If construction of the BOSS state throws
    // or the search cannot start, the next frame still takes the recovery path.
    waitingForBossResult = true;
    BOSSManager::Instance().startNewSearch(goal);
}

// this will return true if any unit is on the first frame of its training time remaining
// this can cause issues for the build order search system so don't plan a search on these frames
bool StrategyManager::canPlanBuildOrderNow() const
{
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit->getRemainingTrainTime() == 0)
        {
            continue;
        }

        BWAPI::UnitType trainType = unit->getLastCommand().getUnitType();

        if (unit->getRemainingTrainTime() == trainType.buildTime())
        {
            return false;
        }
    }

    return true;
}

// Do we expect or plan to drop at some point during the game?
bool StrategyManager::dropIsPlanned() const
{
    // Don't drop in ZvZ.
    

    // Otherwise plan drop if the opening says so, or if the map has islands to take.
    return
        getOpeningGroup() == "drop" ||
        (Config::Macro::ExpandToIslands && the.bases.hasIslandBases());
}

// Whether we have the tech and transport to drop.
bool StrategyManager::hasDropTech()
{
    
    
    
    {
        return the.my.completed.count(BWAPI::UnitTypes::Terran_Dropship) > 0;
    }

    return false;
}
