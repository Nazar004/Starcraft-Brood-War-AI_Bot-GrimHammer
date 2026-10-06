\
#include "Core/The.h"

#include "Map/Bases.h"
#include "Map/Grids/GridTasks.h"
#include "Intelligence/InformationManager.h"
#include "Utilities/Logger.h"
#include "Map/MapGrid.h"
#include "Intelligence/Learning/OpeningTiming.h"
#include "Intelligence/Learning/OpponentModel.h"
#include "Utilities/ParseUtils.h"
#include "Production/ProductionManager.h"
#include "Utilities/Random.h"
#include "Construction/StaticDefense.h"

using namespace UAlbertaBot;

The::The()
    : _airTasksStarted(false)
    , bases(Bases::Instance())
    , grid(MapGrid::Instance())
    , info(InformationManager::Instance())
    , openingTiming(OpeningTiming::Instance())
    , production(ProductionManager::Instance())
    , random(Random::Instance())
    , staticDefense(*new StaticDefense)
{
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// NOTE The skill kit is initialized in OpponentModel and updated in GameCommander.

void The::initialize()
{
    // The root object survives for the lifetime of the BWAPI module.  Reset
    // per-game task state explicitly so a second game cannot inherit tasks,
    // base indices or air-task state from the previous game.
    tasks.clear();
    micro.resetForNewGame();
    _airTasksStarted = false;

    _selfRace = BWAPI::Broodwar->self()->getRace();

    // The order of initialization is important because of dependencies.
    partitions.initialize();
    inset.initialize();				// depends on partitions
    vWalkRoom.initialize();			// depends on inset
    tileRoom.initialize();			// depends on vWalkRoom
    zone.initialize();				// depends on tileRoom
    map.initialize();

    bases.initialize();             // depends on map
    info.initialize();              // depends on bases
    placer.initialize();
    ops.initialize();

    // Read the opening timing file, to make opening decisions.
    // openingTiming.read();

    // Parse the bot's configuration file.
    // Change this file path (in config.cpp) to point to your config file.
    // Any relative path name will be relative to Starcraft installation folder.
    // The config depends on the map and must be read after the map is analyzed.
    // This also reads the opponent model data and decides on the opening.
    ParseUtils::ParseConfigFile(Config::ConfigFile::ConfigFileLocation);
    if (_selfRace == BWAPI::Races::Terran &&
        BWAPI::Broodwar->enemy() &&
        BWAPI::Broodwar->enemy()->getRace() == BWAPI::Races::Zerg &&
        (Config::Strategy::StrategyName == "TvZ_BioTank_1RaxFE" ||
         Config::Strategy::StrategyName == "TvZ_BioTank_2RaxAcademy" ||
         Config::Strategy::StrategyName == "TvZ_BioTank_2RaxTech"))
    {
        placer.disableTerranMainWallForEarlyExpand();
    }
    placer.tryTerranRushWall();

    // Sets the initial production queue to the book opening chosen above.
    production.initialize();

    // Add tasks that will run throughout the game.
    tasks.post(new GroundHitsFixedTask());
    tasks.post(new GroundHitsMobileTask());
	tasks.post(new GroundSafePathTask());
	// The air safe path task is started only if and when we get air units.
	// See maybeStartAirSafePathTask().
}

// Hits from static defense, sieged tanks, and burrowed lurkers.
int The::staticHits(BWAPI::Unit unit, const BWAPI::TilePosition & tile) const
{
    return unit->isFlying()
        ? airHitsFixed.at(tile)
        : groundHitsFixed.at(tile);
}

// Hits from static defense, sieged tanks, and burrowed lurkers.
int The::staticHits(BWAPI::Unit unit) const
{
    return unit->isFlying()
        ? airHitsFixed.at(unit->getTilePosition())
        : groundHitsFixed.at(unit->getTilePosition());
}

// Hits from all enemies on ground units, static and mobile.
int The::groundHits(const BWAPI::TilePosition & tile) const
{
	return groundHitsFixed.at(tile) + groundHitsMobile.at(tile);
}

// Hits from all enemies on air units, static and mobile.
int The::airHits(const BWAPI::TilePosition & tile) const
{
	return airHitsFixed.at(tile) + airHitsMobile.at(tile);
}

// Air or ground hits.
int The::hits(bool air, const BWAPI::TilePosition & tile) const
{
	return air ? the.airHits(tile) : the.groundHits(tile);
}

// Air or ground hits.
int The::hits(bool air, const BWAPI::Position & pos) const
{
	return hits(air, BWAPI::TilePosition(pos));
}

// Hits from all enemies, static and mobile.
int The::hits(BWAPI::Unit unit, const BWAPI::TilePosition& tile) const
{
    if (!unit || !tile.isValid())
    {
        return 0;
    }

    // This overload is used for look-ahead movement checks.  It must inspect
    // the requested future tile, not the unit's current tile.
    return unit->isFlying()
        ? airHits(tile)
        : groundHits(tile);
}

// Hits from all enemies, static and mobile.
int The::hits(BWAPI::Unit unit) const
{
    return unit->isFlying()
        ? airHits(unit->getTilePosition())
        : groundHits(unit->getTilePosition());
}

// Start the tasks to compute various aspects of air safety for our units.
// Zerg has overlords and starts the tasks immediately.
void The::maybeStartAirTasks()
{
	if (!_airTasksStarted && the.info.weHaveAirUnits())
	{
		tasks.post(new AirHitsFixedTask());
		tasks.post(new AirHitsMobileTask());
		tasks.post(new AirSafePathTask());
		_airTasksStarted = true;
	}
}

void The::onNeutralDestroyed(BWAPI::Unit unit)
{
    if (Config::IO::EnableCrashLog)
    {
        Logger::SetCrashContext("The::onNeutralDestroyed - removeNeutral", the.now());
    }

    const BWAPI::UnitType initialType = unit->getInitialType();
    const bool isMineralField = initialType.isMineralField();
    const int initialResources = isMineralField ? unit->getInitialResources() : 0;
    const int partitionsBefore = partitions.getNumPartitions();

    if (!partitions.removeNeutral(unit))
    {
        return;
    }

    const bool connectivityChanged =
        partitions.getNumPartitions() != partitionsBefore;

    // Normal mined-out 1500-mineral patches used to trigger the entire map
    // analysis stack (Inset/Room/Areas/Base distance maps), causing visible
    // 100-300 ms spikes late in a game. They very rarely define topology. Keep
    // the expensive rebuild for real neutral buildings, low-resource blocking
    // minerals, or any removal which changes connected components. Tactical
    // walkability/buildability still refreshes below for ordinary depletion.
    const bool likelyBlockingMineral =
        isMineralField && initialResources > 0 && initialResources <= 200;
    const bool needFullTopologyRefresh =
        !isMineralField || likelyBlockingMineral || connectivityChanged;

    if (!needFullTopologyRefresh)
    {
        if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - light mineral refresh", the.now());
        map.refreshTopology();
        placer.refreshMapAnalysis();
        info.refreshRegionInformation();
        if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - completed light mineral refresh", the.now());
        return;
    }

    // Rebuild in dependency order for real terrain blockers. Correctness here
    // is more valuable than keeping stale local caches (notably on Benzene-like
    // maps with destructible route blockers).
    if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - GridInset", the.now());
    inset.initialize();
    if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - VWalkRoom", the.now());
    vWalkRoom.initialize();
    if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - TileRoom", the.now());
    tileRoom.initialize();
    if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - GridZone", the.now());
    zone.initialize();
    if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - MapTools topology", the.now());
    map.refreshTopology();
    if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - Bases refresh", the.now());
    bases.refreshMapAnalysis();
    if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - BuildingPlacer refresh", the.now());
    placer.refreshMapAnalysis(true);
    if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - InformationManager refresh", the.now());
    info.refreshRegionInformation();
    if (Config::IO::EnableCrashLog) Logger::SetCrashContext("The::onNeutralDestroyed - completed", the.now());

    if (Config::Debug::DrawMapDistances || Config::Debug::DrawUnitTargets)
    {
        BWAPI::Broodwar->printf(
            "map topology rebuilt: revision %u, partitions=%d, chokes=%d",
            partitions.getRevision(),
            partitions.getNumPartitions(),
            int(zone.chokes().size()));
    }
}

void The::update()
{
    my.completed.takeSelf();
    my.all.takeSelfAll();
    your.seen.takeEnemy();
    your.ever.takeEnemyEver(your.seen);
    your.inferred.takeEnemyInferred(your.ever);

    // InformationManager has already updated this frame. Refresh only the
    // lightweight grounded-building occupancy used by tactical pathing; do not
    // rebuild terrain Areas/Chokes when ordinary buildings appear or lift.
    map.updateRuntimeWalkability();

    ops.update();
    staticDefense.update();
    grid.update();

	maybeStartAirTasks();
}

OpponentModel & The::oppModel()
{
	return OpponentModel::Instance();
}
