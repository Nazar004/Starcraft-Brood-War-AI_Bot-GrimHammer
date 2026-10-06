#pragma once

#include "Construction/BuildingPlacer.h"
#include "Combat/Simulation/CombatSimulation.h"
#include "Map/Grids/GridAttacks.h"
#include "Map/Grids/GridInset.h"
#include "Map/Grids/GridRoom.h"
#include "Map/Grids/GridSafePath.h"
#include "Map/Grids/GridTileRoom.h"
#include "Map/Grids/GridZone.h"
#include "Map/MapPartitions.h"
#include "Map/MapTools.h"
#include "Combat/Micro/Micro.h"
#include "Combat/OpsBoss.h"
#include "Intelligence/PlayerSnapshot.h"
#include "Intelligence/Skills/SkillKit.h"
#include "Core/Tasks.h"

// Central access to many components.
#define the (The::Root())

namespace UAlbertaBot
{
    class Bases;
    class MapGrid;
    class InformationManager;
    class ProductionManager;
    class OpeningTiming;
	class OpponentModel;
    class Random;
    class StaticDefense;

    struct My
    {
        PlayerSnapshot completed;
        PlayerSnapshot all;
    };

    struct Your
    {
        PlayerSnapshot seen;
        PlayerSnapshot ever;
        PlayerSnapshot inferred;
    };

    class The
    {
    private:
        BWAPI::Race _selfRace;
		bool _airTasksStarted;

		void maybeStartAirTasks();

    public:
        The();
        // Initialize The. Call this once per game in onStart().
        void initialize();

        // Map information.

        GridRoom vWalkRoom;
        // How much room is there around this tile? A rough estimate of how much stuff fits there.
        GridTileRoom tileRoom;
        // How far is this walk tile from the nearest wall?
        GridInset inset;
        // What zone is this tile in?
        GridZone zone;
        // What map partition is this walk tile in? You can walk between places in the same partition.
        MapPartitions partitions;
        // Map information and calculations.
        MapTools map;

        // Managers.

        // Information about bases and resources.
        Bases & bases;
        // Combat sim does keep some game-duration info of its own.
        CombatSimulation combatSim;
        // Large cells laid over the map.
        MapGrid & grid;
        // Game state information, especially stored information about the enemy.
        InformationManager & info;
        // Perform unit control actions ("unit micro").
        Micro micro;
        // Compare openings by the times of events in them.
        OpeningTiming & openingTiming;
        // Place buildings. Find macro locations.
        BuildingPlacer placer;
        // Make stuff.
        ProductionManager & production;
        // Operations.
        OpsBoss ops;
        // Extensible set of skills using the opponent model.
        SkillKit skillkit;
        // Terran bunkers and missile turrets.
        StaticDefense & staticDefense;
        // Asynchronous tasks.
        Tasks tasks;

        // Varying during the game.

        // My current unit counts.
        My my;
        // Your current unit counts.
        Your your;
        // How many enemy units hit each tile on the ground?
        GroundAttacksFixed groundHitsFixed;
        GroundAttacksMobile groundHitsMobile;
        // How many enemy units hit each tile in the air?
        AirAttacksFixed airHitsFixed;
        AirAttacksMobile airHitsMobile;
        // How many immobile enemy units could hit this unit, at its actual or another location?
        int staticHits(BWAPI::Unit unit, const BWAPI::TilePosition & tile) const;
        int staticHits(BWAPI::Unit unit) const;
		// How many total enemy units could hit ground/air at this location?
		int groundHits(const BWAPI::TilePosition & tile) const;
		int airHits(const BWAPI::TilePosition & tile) const;
		int hits(bool air, const BWAPI::TilePosition & tile) const;
		int hits(bool air, const BWAPI::Position & pos) const;
		// How many total enemy units could hit this unit, at its actual or another location?
        int hits(BWAPI::Unit unit, const BWAPI::TilePosition & tile) const;
        int hits(BWAPI::Unit unit) const;

		// The safe path tasks are started on demand,
		// when the first call is made to find a safe path.
		void startGroundPathTask(Base * base);
		void startAirPathTask(Base * base);

        // Update the varying values.
        void update();

        // A static neutral blocker (mineral/building/etc.) disappeared. Rebuild
        // the dynamic terrain layers while keeping existing manager APIs stable.
        void onNeutralDestroyed(BWAPI::Unit unit);
        unsigned int mapTopologyRevision() const { return partitions.getRevision(); };

        // Utility.

        // The player (this bot).
        BWAPI::Player self()      const { return BWAPI::Broodwar->self(); };
        // The enemy player.
        BWAPI::Player enemy()     const { return BWAPI::Broodwar->enemy(); };
        // The neutral player, which owns things like resources and critters.
        BWAPI::Player neutral()   const { return BWAPI::Broodwar->neutral(); };

        // The bot's supported race is Terran.
        BWAPI::Race selfRace()    const { return _selfRace; };
        // The enemy's race, terran protoss zerg unknown.
        // It changes from unknown to another value when a random player is first scouted.
        BWAPI::Race enemyRace()   const { return BWAPI::Broodwar->enemy()->getRace(); };

        // Current frame count, same as Broodwar->getFrameCount().
        int now() const { return BWAPI::Broodwar->getFrameCount(); };

		OpponentModel & oppModel();

        // Generate random numbers.
        Random & random;

        static inline The & Root() {
            static The TheRoot;
            return TheRoot;
        };
    };
}
