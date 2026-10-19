#pragma once

#include "Core/Common.h"
#include "Intelligence/InformationManager.h"
#include "Economy/WorkerManager.h"
#include "Production/BuildOrder.h"
#include "Production/BuildOrderQueue.h"

namespace UAlbertaBot
{
typedef std::pair<MacroAct, size_t> MetaPair;
typedef std::vector<MetaPair> MetaPairVector;

// Long-lived Terran macro identity. Opening groups still describe how the
// JSON book starts; a TerranPlan describes what the game is trying to become
// after (or while reacting during) that opening. Tactical EnemyArmyMode remains
// separate so one visible unit cannot rewrite the whole macro identity.
enum class TerranPlanId
{
    None,
    BioTank,
    TvP_DeepSix,
    TvP_Siege,
    TvP_AntiZealot,
    TvP_AntiDragoon,
    TvP_AntiCarrier,
    TvZ_Sparks,
    TvZ_PreHive,
    TvZ_SK,
    TvZ_Mech,
    TvZ_AntiLing,
    TvZ_VulturePressure,
    TvZ_AntiHydra,
    TvZ_AntiMuta,
    TvT_Mech,
    TvT_Anti2Factory,
    TvT_AntiAir
};

// Formation intent for the mobile Terran army. It is not a second enemy
// classifier: StrategyManager maps the current threat and durable Plan to one
// of these three stable micro contracts.
enum class TerranArmyPosture
{
    Ground,
    Mixed,
    PureAir
};

struct Strategy
{
    std::string _name;
    BWAPI::Race _race;
    std::string _openingGroup;
    std::string _terranPlan;
    BuildOrder  _buildOrder;

    Strategy()
        : _name("None")
        , _race(BWAPI::Races::None)
        , _openingGroup("")
        , _terranPlan("")
    {
    }

    Strategy(
        const std::string & name,
        const BWAPI::Race & race,
        const std::string & openingGroup,
        const std::string & terranPlan,
        const BuildOrder & buildOrder)
        : _name(name)
        , _race(race)
        , _openingGroup(openingGroup)
        , _terranPlan(terranPlan)
        , _buildOrder(buildOrder)
    {
    }
};

class StrategyManager 
{
    StrategyManager();

    BWAPI::Race					    _selfRace;
    BWAPI::Race					    _enemyRace;
    std::map<std::string, Strategy> _strategies;
    int                             _totalGamesPlayed;
    const BuildOrder                _emptyBuildOrder;
    std::string						_openingGroup;
    bool _pureAirPlan;
    bool							_hasDropTech;
    int								_highWaterBases;				// most bases we've ever had, terran and protoss only
    bool							_openingStaticDefenseDropped;	// make sure we do this at most once ever
    bool _defaultTankGather;
    bool _bioTankFirstPushReleased;
    int _bioFirstAttackFrame;
    std::string _bioTankAttackStatus;
    std::string _openingTerranPlanName;
    TerranPlanId _terranBaselinePlan;
    TerranPlanId _terranPlan;
    int _terranPlanEnteredFrame;
    unsigned int _terranPlanRevision;
    std::string _terranPlanReason;

    void                            initializeTerranPlan();
    void                            updateTerranPlan();
    bool                            switchTerranPlan(TerranPlanId plan, const std::string & reason, bool invalidateMacro);

    const	bool				    shouldExpandNow() const;
    const	MetaPairVector		    getTerranBuildOrderGoal();
    int _tankMassReleaseTanks;
    int _tankMassReleaseGoliaths;
    const char * _slowestUrgentStage = "";
    double _slowestUrgentStageMs = 0.0;
    const char * _slowestGoalStage = "";
    double _slowestGoalStageMs = 0.0;


    bool							detectSupplyBlock(BuildOrderQueue & queue) const;
    bool							handleExtremeEmergency(BuildOrderQueue & queue);
    bool                            adaptTerranStrategyToEnemyPlan();

    bool							canPlanBuildOrderNow() const;
    void							performBuildOrderSearch();

public:
    const char * slowestGoalStage() const { return _slowestGoalStage; }
    double slowestGoalStageMilliseconds() const { return _slowestGoalStageMs; }
    const char * slowestUrgentStage() const { return _slowestUrgentStage; }
    double slowestUrgentStageMilliseconds() const { return _slowestUrgentStageMs; }
    
    static	StrategyManager &	    Instance();
    bool defaultTankGatherEnabled() const;
    int tankMassReleaseTankCount() const;
    bool pureAirPlanActive() const;
    bool bioTankPlanActive() const;
    bool deepSixPlanActive() const;
    bool bioTankUsesTanks() const;
    bool bioTankPushReady();
    void updateBioTankAttackState();
    const std::string& bioTankAttackStatus() const { return _bioTankAttackStatus; }
    TerranArmyPosture getTerranArmyPosture() const;
    bool isTerranHeavyMixed() const;
    bool isTerranLightMixed() const;
    const char * getTerranArmyPostureName() const;
    TerranPlanId getTerranPlan() const;
    const char * getTerranPlanName() const;
    const std::string & getTerranPlanReason() const;
    const char * getEnemyArmyModeName() const;
    unsigned int getEnemyThreatMask() const;
    unsigned int getTerranPlanRevision() const;
    int tankMassReleaseGoliathCount() const;
    bool isTerranMineralExpansionEmergency() const;
    BWAPI::TilePosition getTerranDefensiveBunkerTile(int bunkerIndex) const;
    bool canBuildTerranDefensiveBunkerAt(BWAPI::TilePosition tile) const;

            void                    addStrategy(const std::string & name, const Strategy & strategy);
            void					setOpeningGroup();
    const	std::string &			getOpeningGroup() const;
    const	MetaPairVector		    getBuildOrderGoal();
    const	BuildOrder &            getOpeningBookBuildOrder() const;

            void					handleUrgentProductionIssues(BuildOrderQueue & queue);
            void					freshProductionPlan();

            bool					dropIsPlanned() const;
            bool					hasDropTech();
};

}
