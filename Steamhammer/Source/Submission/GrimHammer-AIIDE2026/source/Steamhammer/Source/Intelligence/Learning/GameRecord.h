#pragma once

#include "Core/Common.h"
#include "Intelligence/Learning/OpeningTimingRecord.h"
#include "Intelligence/OpponentPlan.h"
#include "Intelligence/PlayerSnapshot.h"
#include "Intelligence/Skills/Skill.h"

#include <exception>

// NOTE
// This class does only a little checking of its input file format. Feed it no bad files.

class game_record_read_error : public std::exception {};

namespace UAlbertaBot
{
enum class StrategyClass : unsigned char
{
    Unknown,
    Ground,
    Mixed,
    PureAir
};

const char * StrategyClassString(StrategyClass strategyClass);
StrategyClass StrategyClassFromString(const std::string & text);

// Independent enemy capabilities seen during a game. StrategyClass answers
// "where can the army fight?"; this mask answers "what special preparation
// does it require?". In particular, anti-air-only flyers must never turn a
// ground army into PureAir.
enum class EnemyThreat : unsigned int
{
    None        = 0,
    GroundAir   = 1u << 0,   // flying unit can damage ground targets
    AntiAirOnly = 1u << 1,   // Valkyrie/Corsair/Devourer/Scourge
    Spellcaster = 1u << 2,
    Cloak       = 1u << 3,
    CapitalAir  = 1u << 4,   // Carrier/Battlecruiser
    Splash      = 1u << 5
};

typedef unsigned int EnemyThreatMask;

inline EnemyThreatMask EnemyThreatBit(EnemyThreat threat)
{
    return static_cast<EnemyThreatMask>(threat);
}

inline bool HasEnemyThreat(EnemyThreatMask mask, EnemyThreat threat)
{
    return (mask & EnemyThreatBit(threat)) != 0;
}

const char * EnemyThreatString(EnemyThreat threat);
EnemyThreatMask EnemyThreatForUnitType(BWAPI::UnitType type);
std::string EnemyThreatMaskString(EnemyThreatMask mask);
EnemyThreatMask EnemyThreatMaskFromString(const std::string & text);

struct GameSnapshot
{
    const int frame;
    const PlayerSnapshot us;
    const PlayerSnapshot them;

    // For reading a snapshot from a file.
    GameSnapshot(int t, const PlayerSnapshot & me, const PlayerSnapshot & you)
        : frame(t)
        , us(me)
        , them(you)
    {
    }

    // For taking a snapshot during the game.
    GameSnapshot(const PlayerSnapshot & me, const PlayerSnapshot & you)
        : frame(BWAPI::Broodwar->getFrameCount())
        , us(me)
        , them(you)
    {
    }
};

class GameRecord
{
protected:
    const int firstSnapshotTime = 2 * 60 * 24;
    const int snapshotInterval = 30 * 24;

    const std::string latestRecordFormat = "3.4";
    const std::string gameEndMark = "END GAME";

    // Each game record is labeled with a version number, to allow backward compatibility
    // when the file format changes.
    // Plan: Set the version number to the last Steamhammer release which changed the file format.
    std::string recordFormat;

    // Is this a valid record, or broken in reading?
    bool valid;

    // Is this the record of the current game, or a saved record?
    bool savedRecord;

    // About the game.
    BWAPI::Race ourRace;
    BWAPI::Race enemyRace;
    bool enemyIsRandom;

    std::string mapName;
    std::string openingName;
    int myStartingBaseID;
    int enemyStartingBaseID;
    // The enemy army dimensions observed during this game. This is deliberately
    // independent of the counter composition which our bot happened to choose.
    StrategyClass strategyClass;
    // Capabilities actually revealed by enemy units during this game. The
    // mask is cumulative even though strategyClass tracks the latest army:
    // once an opponent has shown Cloak/Spellcaster/CapitalAir, next-game
    // preparation may safely remember that fact.
    EnemyThreatMask enemyThreats;
    bool enemyThreatProfileKnown;
    // Batch 7: record the Terran Plan selected before play and the final Plan
    // after any hard in-game reactions. UCB learns from the initial Plan; the
    // final Plan is evidence about what this opponent tends to force.
    std::string initialTerranPlan;
    std::string finalTerranPlan;
    OpeningPlan expectedEnemyPlan;
    OpeningPlan enemyPlan;
    bool win;

    // Gas steal data only for v 1.4 game records.
    int frameScoutSentForGasSteal;         // 0 if we didn't try to steal gas
    bool gasStealHappened;                 // true if a refinery building was started

    int frameWeMadeFirstCombatUnit;
    int frameWeGatheredGas;

    int frameEnemyScoutsOurBase;
    int frameEnemyGetsCombatUnits;
    int frameEnemyUsesGas;
    int frameEnemyGetsAirUnits;
    int frameEnemyGetsStaticAntiAir;
    int frameEnemyGetsMobileAntiAir;
    int frameEnemyGetsCloakedUnits;
    int frameEnemyGetsStaticDetection;     // includes spider mines
    int frameEnemyGetsMobileDetection;
    int frameGameEnds;

    // Record skill kit data as raw text so that individual skills don't have to remember it.
    std::vector <std::string> skillKitText;

    // A skill can also store data here for analysis.
    std::map< Skill *, std::map< int, std::vector<int> > > skillData;

    // We allocate the snapshots and never release them.
    std::vector<GameSnapshot *> snapshots;

    BWAPI::Race charRace(char ch);

    int readNumber(std::istream & input);
    int readNumber(std::string & s);

    void parseMatchup(const std::string & s);

    OpeningPlan readOpeningPlan(std::istream & input);

    bool readPlayerSnapshot(std::istream & input, PlayerSnapshot & snap);
    GameSnapshot * readGameSnapshot(std::istream & input);
    void skipToEnd(std::istream & input);

    void read_v3(
        std::istream & input,
        bool hasStrategyClass,
        bool hasEnemyThreatProfile,
        bool hasTerranPlans,
        bool strategyClassIsEnemyObservation);
    void read_v3_4(std::istream & input);
    void read_v3_3(std::istream & input);
    void read_v3_2(std::istream & input);
    void read_v3_1(std::istream & input);
    void read_v3_0(std::istream & input);
    void read_v1_4(std::istream & input);
    void read(std::istream & input);

    void writePlayerSnapshot(std::ostream & output, const PlayerSnapshot & snap);
    void writeGameSnapshot(std::ostream & output, const GameSnapshot * snap);
    virtual void writeSkills(std::ostream & output) const;

    int snapDistance(const PlayerSnapshot & a, const PlayerSnapshot & b) const;

public:
    // Store opening timing skill data specially.
    OpeningTimingRecord openingTimingRecord;

    GameRecord();
    GameRecord(std::istream & input);
    virtual ~GameRecord();

    void write(std::ostream & output);

    bool isValid() { return valid; };

    int distance(const GameRecord & record) const;    // similarity distance UNUSED

    bool findClosestSnapshot(int t, PlayerSnapshot & snap) const;


    bool sameMatchup(const GameRecord & record) const;

    bool getEnemyIsRandom() const { return enemyIsRandom; };
    const std::string & getMapName() const { return mapName; };
    const std::string & getOpeningName() const { return openingName; };
    StrategyClass getStrategyClass() const { return strategyClass; };
    EnemyThreatMask getEnemyThreats() const { return enemyThreats; };
    bool hasEnemyThreatProfile() const { return enemyThreatProfileKnown; };
    const std::string & getInitialTerranPlan() const { return initialTerranPlan; };
    const std::string & getFinalTerranPlan() const { return finalTerranPlan; };
    OpeningPlan getExpectedEnemyPlan() const { return expectedEnemyPlan; };
    OpeningPlan getEnemyPlan() const { return enemyPlan; };
    bool getWin() const { return win; };
    int getFrameScoutSentForGasSteal() const { return frameScoutSentForGasSteal; };
    bool getGasStealHappened() const { return gasStealHappened; };
    int getFrameEnemyUsesGas() const { return frameEnemyUsesGas; };
	int getFrameEnemyGetsAirUnits() const { return frameEnemyGetsAirUnits; };

    const std::vector<int> * getSkillInfo(Skill * skill, int i) const;
    void setSkillInfo(Skill * skill, int i, const std::vector<int> & info);

    void debugLog();
};

}
