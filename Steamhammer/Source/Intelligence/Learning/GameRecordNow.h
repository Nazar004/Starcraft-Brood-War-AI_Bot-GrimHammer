#pragma once

#include "Core/Common.h"
#include "Intelligence/Learning/GameRecord.h"

// The game record of the current game, updated as we go along.

namespace UAlbertaBot
{
// Enough tech that we can tell which units are possible.
enum class GameTech : int
    { Barracks
    , Academy
    , Factory
    , MachineShop
    , Armory
    , Starport
    , ControlTower
    , ScienceFac
    , PhysicsLab
    , CovertOps
    , Nuke

    , Zealot
    , Dragoon
    , Templar
    , Robo
    , Reaver
    , Observer
    , Stargate
    , Carrier
    , Arbiter

    , Zergling
    , Hydralisk
    , Lurker
    , Lair
    , Hive
    , Spire
    , GreaterSpire
    , Queen
    , Infested
    , Ultralisk
    , Defiler

    , Size
    };

class GameRecordNow : public GameRecord
{
private:
    std::map<GameTech, int> techHistory;

    void takeSnapshot();
    void updateEnemyStrategyClass(const PlayerSnapshot & enemySnapshot);
    void updateEnemyThreatProfile(const PlayerSnapshot & enemySnapshot);

    bool enemyScoutedUs() const;

protected:

    void writeSkills(std::ostream & output) const override;

public:
    GameRecordNow();

    void resetForNewGame();
    void setOpening(const std::string & opening) { openingName = opening; };
    void setInitialTerranPlan(const std::string & plan)
    {
        // Initial Plan is an arm chosen by learning. Never overwrite it after
        // an in-game transition, because UCB must receive credit for the arm
        // that actually started the game.
        if (initialTerranPlan.empty() || initialTerranPlan == "None")
        {
            initialTerranPlan = plan;
        }
    };
    void setFinalTerranPlan(const std::string & plan) { finalTerranPlan = plan; };
    void setWin(bool isWinner);

    void update();
};

}
