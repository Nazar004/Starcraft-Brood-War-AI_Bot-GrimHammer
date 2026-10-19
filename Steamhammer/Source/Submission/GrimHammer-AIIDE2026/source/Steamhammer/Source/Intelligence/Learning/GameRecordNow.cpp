#include "Intelligence/Learning/GameRecordNow.h"

#include "Map/Bases.h"
#include "Core/GameCommander.h"
#include "Intelligence/Learning/OpponentModel.h"
#include "Intelligence/ScoutManager.h"
#include "Core/The.h"

#include <algorithm>

using namespace UAlbertaBot;

namespace
{
    bool isCapitalCombatAir(BWAPI::UnitType type)
    {
        return
            type == BWAPI::UnitTypes::Protoss_Carrier ||
            type == BWAPI::UnitTypes::Terran_Battlecruiser ||
            type == BWAPI::UnitTypes::Zerg_Guardian;
    }

    bool isHeavyCombatGround(BWAPI::UnitType type)
    {
        return
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
            type == BWAPI::UnitTypes::Terran_Goliath ||
            type == BWAPI::UnitTypes::Protoss_Dragoon ||
            type == BWAPI::UnitTypes::Protoss_Reaver ||
            type == BWAPI::UnitTypes::Protoss_Archon ||
            type == BWAPI::UnitTypes::Zerg_Lurker ||
            type == BWAPI::UnitTypes::Zerg_Ultralisk;
    }

    bool isArmyUnitForStrategyClass(BWAPI::UnitType type)
    {
        if (type == BWAPI::UnitTypes::None ||
            type == BWAPI::UnitTypes::Unknown ||
            type.isWorker() ||
            type.isBuilding())
        {
            return false;
        }

        // Transports, detectors, larvae and spell-only entities must not turn
        // an otherwise ground army into PureAir/Mixed. Carrier/Reaver are
        // explicit because their damage is delivered by spawned ammunition.
        return
            type.canAttack() ||
            type == BWAPI::UnitTypes::Protoss_Carrier ||
            type == BWAPI::UnitTypes::Protoss_Reaver;
    }

    StrategyClass classifyObservedEnemyArmy(const PlayerSnapshot & snapshot)
    {
        int airUnits = 0;
        int groundUnits = 0;
        int airValue = 0;
        int groundValue = 0;
        int heavyGroundUnits = 0;
        bool capitalAir = false;

        for (const auto & entry : snapshot.getCounts())
        {
            const BWAPI::UnitType type = entry.first;
            const int count = std::max(0, entry.second);
            if (count <= 0 || !isArmyUnitForStrategyClass(type))
            {
                continue;
            }

            // Gas-heavy units represent a larger strategic commitment than
            // their mineral price alone suggests.
            const int value = count * (type.mineralPrice() + 2 * type.gasPrice());

            if (type.isFlyer())
            {
                // Valkyries, Corsairs, Devourers and Scourge can attack only
                // air. They are important threats to our support units, but
                // they cannot replace the enemy ground army and must never
                // make the saved composition PureAir/Mixed on their own.
                if (!HasEnemyThreat(
                        EnemyThreatForUnitType(type),
                        EnemyThreat::GroundAir))
                {
                    continue;
                }

                airUnits += count;
                airValue += value;
                capitalAir = capitalAir || isCapitalCombatAir(type);
            }
            else
            {
                groundUnits += count;
                groundValue += value;
                if (isHeavyCombatGround(type))
                {
                    heavyGroundUnits += count;
                }
            }
        }

        // Ignore token scouts and one or two cheap opening units. One costly
        // air attacker or Tank is already enough evidence; cheap armies need
        // several bodies before they define a strategy dimension.
        const bool committedAir =
            capitalAir || airUnits >= 3 || airValue >= 200;
        const bool committedGround =
            groundUnits >= 4 || heavyGroundUnits >= 2 || groundValue >= 300;

        if (committedAir && committedGround)
        {
            return StrategyClass::Mixed;
        }
        if (committedAir)
        {
            return StrategyClass::PureAir;
        }
        if (committedGround)
        {
            return StrategyClass::Ground;
        }
        return StrategyClass::Unknown;
    }

    StrategyClass selectRecordedStrategyClass(
        StrategyClass previous,
        StrategyClass observed)
    {
        // Record the latest meaningful army composition, not the union of
        // every phase of the game. A Ling opening followed by mass Mutalisks
        // is strategically PureAir for the next-game preparation; likewise a
        // Wraith opening which has genuinely transitioned to Tanks is Ground.
        // Mixed is reserved for air and ground which coexist meaningfully in
        // one observed composition. If the enemy army is wiped out just before
        // onEnd(), keep the last useful observation instead of writing Unknown.
        if (observed == StrategyClass::Unknown)
        {
            return previous;
        }
        return observed;
    }
}

void GameRecordNow::writeSkills(std::ostream & output) const
{
    the.skillkit.write(output);
}

// Take a digest snapshot of the game situation.
void GameRecordNow::takeSnapshot()
{
    snapshots.push_back(new GameSnapshot(PlayerSnapshot (BWAPI::Broodwar->self()), PlayerSnapshot (BWAPI::Broodwar->enemy())));
}

void GameRecordNow::updateEnemyStrategyClass(const PlayerSnapshot & enemySnapshot)
{
    strategyClass = selectRecordedStrategyClass(
        strategyClass,
        classifyObservedEnemyArmy(enemySnapshot));
}

void GameRecordNow::updateEnemyThreatProfile(const PlayerSnapshot & enemySnapshot)
{
    // Threat capabilities are cumulative for the game. A late transition to
    // Tanks should update StrategyClass to Ground, but it must not erase that
    // the same opponent already demonstrated Cloak or capital air.
    for (const auto & entry : enemySnapshot.getCounts())
    {
        if (entry.second > 0)
        {
            enemyThreats |= EnemyThreatForUnitType(entry.first);
        }
    }
}

// Figure out whether the enemy has seen our base yet.
bool GameRecordNow::enemyScoutedUs() const
{
    Base * base = Bases::Instance().myStart();

    for (const auto & kv : InformationManager::Instance().getUnitData(BWAPI::Broodwar->enemy()).getUnits())
    {
        const UnitInfo & ui(kv.second);

        // If a unit was last spotted close to us, assume we've been seen.
        if (ui.lastPosition.getDistance(base->getCenter()) < 800)
        {
            return true;
        }
    }

    return false;
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// Constructor for the record of the current game.
// When this object is initialized, the opening and some other items are not yet known.
GameRecordNow::GameRecordNow()
{
}

// OpponentModel is process-wide. Tournament runners are allowed to keep the
// DLL loaded for another game, so explicitly rebuild all live-record state.
void GameRecordNow::resetForNewGame()
{
    for (GameSnapshot * snapshot : snapshots)
    {
        delete snapshot;
    }
    snapshots.clear();
    skillKitText.clear();
    skillData.clear();
    techHistory.clear();
    openingTimingRecord = OpeningTimingRecord();

    valid = true;
    savedRecord = false;
    recordFormat = latestRecordFormat;
    ourRace = BWAPI::Broodwar->self()->getRace();
    enemyRace = BWAPI::Broodwar->enemy()->getRace();
    enemyIsRandom = enemyRace == BWAPI::Races::Unknown;
    mapName = BWAPI::Broodwar->mapFileName();
    openingName.clear();
    myStartingBaseID = the.bases.myStart() ? the.bases.myStart()->getID() : 0;
    enemyStartingBaseID = the.bases.enemyStart() ? the.bases.enemyStart()->getID() : 0;
    strategyClass = StrategyClass::Unknown;
    enemyThreats = 0;
    enemyThreatProfileKnown = true;
    initialTerranPlan = "None";
    finalTerranPlan = "None";
    expectedEnemyPlan = OpeningPlan::Unknown;
    enemyPlan = OpeningPlan::Unknown;
    win = false;

    frameScoutSentForGasSteal = 0;
    gasStealHappened = false;
    frameWeMadeFirstCombatUnit = 0;
    frameWeGatheredGas = 0;
    frameEnemyScoutsOurBase = 0;
    frameEnemyGetsCombatUnits = 0;
    frameEnemyUsesGas = 0;
    frameEnemyGetsAirUnits = 0;
    frameEnemyGetsStaticAntiAir = 0;
    frameEnemyGetsMobileAntiAir = 0;
    frameEnemyGetsCloakedUnits = 0;
    frameEnemyGetsStaticDetection = 0;
    frameEnemyGetsMobileDetection = 0;
    frameGameEnds = 0;
}

// Called when the game is over.
void GameRecordNow::setWin(bool isWinner)
{
    // Capture the last battle even when onEnd falls between periodic updates.
    const PlayerSnapshot enemySnapshot(BWAPI::Broodwar->enemy());
    updateEnemyStrategyClass(enemySnapshot);
    updateEnemyThreatProfile(the.your.ever);
    win = isWinner;
    frameGameEnds = BWAPI::Broodwar->getFrameCount();
}

void GameRecordNow::update()
{
    int now = BWAPI::Broodwar->getFrameCount();

    // Update the when-it-happens frame counters. We don't actually need to check often.
    if (now % 32 == 30)
    {
        const PlayerSnapshot enemySnapshot(BWAPI::Broodwar->enemy());
        updateEnemyStrategyClass(enemySnapshot);
        updateEnemyThreatProfile(the.your.ever);

        if (enemyRace == BWAPI::Races::Unknown)
        {
            enemyRace = BWAPI::Broodwar->enemy()->getRace();
        }
        if (!enemyStartingBaseID && the.bases.enemyStart())
        {
            enemyStartingBaseID = the.bases.enemyStart()->getID();
        }
        enemyPlan = OpponentModel::Instance().getEnemyPlan();
        if (!frameWeMadeFirstCombatUnit && InformationManager::Instance().weHaveCombatUnits())
        {
            frameWeMadeFirstCombatUnit = now;
        }
        if (!frameWeGatheredGas && BWAPI::Broodwar->self()->gatheredGas() > 0)
        {
            frameWeGatheredGas = now;
        }
        if (!frameEnemyScoutsOurBase && enemyScoutedUs())
        {
            frameEnemyScoutsOurBase = now;
        }
        if (!frameEnemyGetsCombatUnits && InformationManager::Instance().enemyHasCombatUnits())
        {
            frameEnemyGetsCombatUnits = now;
        }
        if (!frameEnemyUsesGas && InformationManager::Instance().enemyGasTiming() > 0)
        {
            frameEnemyUsesGas = InformationManager::Instance().enemyGasTiming();
        }
        if (!frameEnemyGetsAirUnits && InformationManager::Instance().enemyHasAirTech())
        {
            frameEnemyGetsAirUnits = now;
        }
        if (!frameEnemyGetsStaticAntiAir && InformationManager::Instance().enemyHasStaticAntiAir())
        {
            frameEnemyGetsStaticAntiAir = now;
        }
        if (!frameEnemyGetsMobileAntiAir && InformationManager::Instance().enemyHasAntiAir())
        {
            frameEnemyGetsMobileAntiAir = now;
        }
        if (!frameEnemyGetsCloakedUnits && InformationManager::Instance().enemyHasCloakTech())
        {
            frameEnemyGetsCloakedUnits = now;
        }
        if (!frameEnemyGetsStaticDetection && InformationManager::Instance().enemyHasStaticDetection())
        {
            frameEnemyGetsStaticDetection = now;
        }
        if (!frameEnemyGetsMobileDetection && InformationManager::Instance().enemyHasMobileDetection())
        {
            frameEnemyGetsMobileDetection = now;
        }
    }

    /* disabled
    // If it's time, take a snapshot.
    int sinceFirst = now - firstSnapshotTime;
    if (sinceFirst >= 0 && sinceFirst % snapshotInterval == 0)
    {
        takeSnapshot();
    }
    */
}
