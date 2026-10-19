#include "Production/ProductionManager.h"
#include "Strategy/BioProductionPolicy.h"
#include "Strategy/Terran/BioTankAssessment.h"
#include "Strategy/Terran/AirTanksPolicy.h"

#include "Map/Bases.h"
#include "Construction/BuildingPlacer.h"
#include "Combat/CombatCommander.h"
#include "Intelligence/InformationManager.h"
#include "Utilities/Random.h"
#include "Intelligence/ScoutManager.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <map>
#include <set>
#include <vector>

using namespace UAlbertaBot;


namespace
{
    std::map<int, int> factoryTrainingWatchStartFrame;
    std::map<int, int> factoryTrainingLastRemaining;
    std::map<int, BWAPI::UnitType> factoryTrainingWatchType;
    std::map<int, int> factoryIgnoreUntilFrame;
    int factoryRecoveryLastFrame = -1;
    std::map<int, int> factoryIdleSinceFrame;

    std::map<int, int> commandCenterTrainingWatchStartFrame;
    std::map<int, int> commandCenterTrainingLastRemaining;
    std::map<int, int> commandCenterIgnoreUntilFrame;
    int commandCenterRecoveryLastFrame = -1;

    int usefulTerranWorkerCap()
    {
        // WorkerManager applies AbsoluteMaxWorkers and derives the smaller
        // live-resource capacity. Keep at least one worker as its documented
        // recovery reserve when every current mineral line is exhausted.
        return std::max(
            1,
            WorkerManager::Instance().getMaxWorkers());
    }

    int terranSCVCountIncludingTraining()
    {
        int count =
            the.my.all.count(BWAPI::UnitTypes::Terran_SCV);

        // A unit in a Command Center's training slot is not yet present in
        // PlayerSnapshot::all. Command Centers can train only SCVs, so this
        // avoids the crash-prone getTrainingQueue() call while preventing many
        // simultaneous bases from overshooting the worker cap together.
        for (BWAPI::Unit unit : the.self()->getUnits())
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

    bool terranSCVCapReached()
    {
        return
            terranSCVCountIncludingTraining() >=
            usefulTerranWorkerCap();
    }

    // Only a visible ground attacker at an owned base can interrupt the JSON
    // opening. Predictions and enemies elsewhere must not spend its savings.
    int visibleGroundAttackersAtOwnedBase()
    {
        int attackers = 0;
        for (BWAPI::Unit enemy : the.enemy()->getUnits())
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                enemy->isFlying() || !enemy->getPosition().isValid() ||
                !UnitUtil::CanAttackGround(enemy) ||
                (enemy->getType().isWorker() && !enemy->isAttacking())) continue;

            for (Base* base : the.bases.getAll())
            {
                if (base && base->getOwner() == the.self() &&
                    base->getDepot() && base->getDepot()->exists() &&
                    enemy->getDistance(base->getPosition()) <= 12 * 32)
                {
                    ++attackers;
                    break;
                }
            }
        }
        return attackers;
    }
}

// NOTE Items that may call BWAPI must be initialized in initialize().
ProductionManager::ProductionManager()
    : _lastProductionFrame(0)
    , _assignedWorkerForThisBuilding     (nullptr)
    , _typeOfUpcomingBuilding			 (BWAPI::UnitTypes::None)
    , _locationOfUpcomingBuilding       (MacroLocation::Anywhere)
    , _tileOfUpcomingBuilding           (BWAPI::TilePositions::None)
    , _haveLocationForThisBuilding       (false)
    , _delayBuildingPredictionUntilFrame (0)
    , _unplaceableBunkerSinceFrame       (-1)
    , _unplaceableMacroBuildingSinceFrame (-1)
    , _nextTerranWallCheckFrame          (0)
    , _terranWallGateOpenUntilFrame      (0)
    , _terranWallLastGateCommandFrame    (-100000)
    , _terranWallReleasedForVision       (false)
    , _nextTerranBarracksVisionFrame     (0)
    , _outOfBook                         (false)
    , _targetGasAmount                   (0)
	, _combatMineralsSpent               (0)
	, _combatGasSpent                    (0)
    , _terranRulePlanFrame              (-1)
    , _terranRuleLastProgressFrame      (-1)
    , _terranRuleLastObservedFrame      (-1)
    , _terranRuleDecision               ("JSON opening queue")
{
}

// Reset the state of the Terran rule executor. The DLL can remain loaded
// between games, while the BWAPI frame counter starts again from zero.
void ProductionManager::resetTerranRuleProduction()
{
    _terranRulePlan.clear();
    _terranRuleBaseline.clear();
    _terranRuleIssued.clear();
    _terranRulePlanFrame = -1;
    _terranRuleLastProgressFrame = -1;
    _terranRuleDecision = "JSON opening queue";
    _terranStructuralTargets.clear();
    _terranDisplayFrame = -100000;
}

void ProductionManager::invalidateTerranRulePlan(
    const std::string & reason,
    bool clearPendingGoals)
{
    _terranRulePlan.clear();
    _terranRuleBaseline.clear();
    _terranRuleIssued.clear();
    _terranRulePlanFrame = -1;
    // Keep limits for already accepted buildings across plan invalidation.

    if (clearPendingGoals)
    {
        _goals.clear();
    }

    setTerranRuleDecision("REFRESH: " + reason);
}

// A stable key for one unit, technology or upgrade rule.
int ProductionManager::terranRuleKey(const MacroAct & act) const
{
    if (act.isUnit())
    {
        const BWAPI::UnitType type = act.getUnitType();

        if (type.isBuilding())
        {
            const int location = int(act.getMacroLocation()) & 0x0f;
            const BWAPI::TilePosition tile = act.getTileLocation();
            const int tileX =
                act.getMacroLocation() == MacroLocation::Tile && tile.isValid()
                ? tile.x & 0xff
                : 0;
            const int tileY =
                act.getMacroLocation() == MacroLocation::Tile && tile.isValid()
                ? tile.y & 0xff
                : 0;

            return
                0x10000000 |
                ((type.getID() & 0xff) << 20) |
                (location << 16) |
                (tileX << 8) |
                tileY;
        }

        return 0x10000 + type.getID();
    }
    if (act.isTech())
    {
        return 0x20000 + act.getTechType().getID();
    }
    if (act.isUpgrade())
    {
        return 0x30000 + act.getUpgradeType().getID();
    }

    return -1;
}

// Clamp goals to what Brood War can actually finish. This value is used by
// both execution and the on-screen explanation, so the displayed target and
// the target acted upon can never disagree.
size_t ProductionManager::terranRuleTarget(const MetaPair & rule) const
{
    size_t target = rule.second;

    if (rule.first.isTech())
    {
        target = std::min(target, size_t(1));
    }
    else if (rule.first.isUnit() &&
        rule.first.getUnitType() == BWAPI::UnitTypes::Terran_SCV)
    {
        // Strategy goals are normally generated with this cap already. Clamp
        // again here so a stale/malformed rule can never outlive the mineral
        // lines from which its original target was calculated.
        target = std::min(
            target,
            size_t(usefulTerranWorkerCap()));
    }
    else if (rule.first.isUnit() &&
        (rule.first.getUnitType() == BWAPI::UnitTypes::Terran_Engineering_Bay ||
         rule.first.getUnitType() == BWAPI::UnitTypes::Terran_Armory))
    {
        // Two upgrade buildings of each type are enough for parallel research.
        target = std::min(target, size_t(2));
    }
    else if (rule.first.isUpgrade())
    {
        target = std::min(
            target,
            size_t(the.self()->getMaxUpgradeLevel(
                rule.first.getUpgradeType())));
    }

    return target;
}

void ProductionManager::setTerranRuleDecision(
    const std::string & decision)
{
    // Do not print every frame. Keeping only the latest distinct explanation
    // gives useful live diagnostics without recreating the old log spam and
    // frame-time spikes.
    if (_terranRuleDecision != decision)
    {
        _terranRuleDecision = decision;
    }
}

bool ProductionManager::sameTerranRuleAction(
    const MacroAct & first,
    const MacroAct & second) const
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

// Count copies which have already left the strategic plan but have not yet
// appeared in the normal BWAPI unit/upgrade state.
size_t ProductionManager::queuedTerranRuleActionCount(const MacroAct & act)
{
    size_t count = 0;

    for (int i = 0; i < int(_queue.size()); ++i)
    {
        if (sameTerranRuleAction(_queue[i].macroAct, act))
        {
            ++count;
        }
    }

    for (const ProductionGoal & goal : _goals)
    {
        if (sameTerranRuleAction(goal.act, act))
        {
            ++count;
        }
    }

    return count;
}

// Current progress toward one StrategyManager target. PlayerSnapshot::all
// already includes the unit in each producer's training queue.
size_t ProductionManager::terranRuleProgress(const MacroAct & act)
{
    if (act.isUnit())
    {
        const BWAPI::UnitType type = act.getUnitType();
        size_t count = 0;

        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
        {
            count =
                the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
                the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
        }
        else if (type == BWAPI::UnitTypes::Terran_Armory)
        {
            // Armories serve one global upgrade target. Placement can put the
            // second one well outside the Main macro-location radius.
            count = the.my.all.count(type) +
                BuildingManager::Instance().getNumUnstarted(type) +
                _queue.numInQueue(type);
            for (const ProductionGoal & goal : _goals)
            {
                if (goal.act.isUnit() && goal.act.getUnitType() == type)
                {
                    ++count;
                }
            }
            return count;
        }
        else if (type.isBuilding() &&
            (act.getMacroLocation() == MacroLocation::Main ||
             act.getMacroLocation() == MacroLocation::Natural ||
             act.getMacroLocation() == MacroLocation::Front ||
             act.getMacroLocation() == MacroLocation::Center ||
             act.getMacroLocation() == MacroLocation::Proxy ||
             act.getMacroLocation() == MacroLocation::EnemyMain ||
             act.getMacroLocation() == MacroLocation::EnemyNatural ||
             act.getMacroLocation() == MacroLocation::Tile))
        {
            BWAPI::Position target =
                act.getMacroLocation() == MacroLocation::Tile
                ? BWAPI::Position(
                    act.getTileLocation().x * 32 + type.tileWidth() * 16,
                    act.getTileLocation().y * 32 + type.tileHeight() * 16)
                : the.placer.getMacroLocationPos(act.getMacroLocation());

            if (target.isValid())
            {
                const int radius =
                    act.getMacroLocation() == MacroLocation::Tile
                    // Positional Turret rules are individual ring slots. The
                    // old seven-tile radius let one Turret satisfy two or three
                    // neighbouring slots and silently stopped construction.
                    ? (type == BWAPI::UnitTypes::Terran_Missile_Turret
                        ? 3 * 32
                        : 7 * 32)
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

            if (type.isBuilding())
            {
                count += BuildingManager::Instance().getNumUnstarted(type);
            }
        }

        return count + queuedTerranRuleActionCount(act);
    }

    if (act.isTech())
    {
        const BWAPI::TechType type = act.getTechType();
        return
            the.self()->hasResearched(type) ||
            the.self()->isResearching(type) ||
            queuedTerranRuleActionCount(act) > 0
            ? 1
            : 0;
    }

    if (act.isUpgrade())
    {
        const BWAPI::UpgradeType type = act.getUpgradeType();
        const size_t currentLevel = the.self()->getUpgradeLevel(type);
        const size_t pending = std::max(
            size_t(the.self()->isUpgrading(type) ? 1 : 0),
            queuedTerranRuleActionCount(act));

        return std::min(
            size_t(the.self()->getMaxUpgradeLevel(type)),
            currentLevel + pending);
    }

    return 0;
}

int ProductionManager::plannedTerranTypeCount(BWAPI::UnitType type)
{
    if (type == BWAPI::UnitTypes::None ||
        type == BWAPI::UnitTypes::Unknown)
    {
        return 0;
    }

    int count = the.my.all.count(type);

    if (type.isBuilding())
    {
        count += BuildingManager::Instance().getNumUnstarted(type);
        count += int(_queue.numInQueue(type));

        for (const ProductionGoal & goal : _goals)
        {
            if (goal.act.isUnit() && goal.act.getUnitType() == type)
            {
                ++count;
            }
        }
    }

    return count;
}

// Return the deepest missing prerequisite needed before this Terran unit type
// can be requested. Returning the type itself means that its own prerequisites
// are ready and this is the next building to add.
BWAPI::UnitType ProductionManager::missingTerranPrerequisite(
    BWAPI::UnitType type,
    int depth)
{
    if (type == BWAPI::UnitTypes::None ||
        type == BWAPI::UnitTypes::Unknown ||
        type.getRace() != BWAPI::Races::Terran ||
        plannedTerranTypeCount(type) > 0)
    {
        return BWAPI::UnitTypes::None;
    }

    if (depth >= 8)
    {
        return type;
    }

    for (const std::pair<const BWAPI::UnitType, int> & requirement : type.requiredUnits())
    {
        const BWAPI::UnitType requiredType = requirement.first;

        if (requiredType.getRace() == BWAPI::Races::Terran &&
            plannedTerranTypeCount(requiredType) == 0)
        {
            const BWAPI::UnitType missing =
                missingTerranPrerequisite(requiredType, depth + 1);

            return missing == BWAPI::UnitTypes::None
                ? requiredType
                : missing;
        }
    }

    const BWAPI::UnitType producerType = type.whatBuilds().first;

    // SCVs are the normal builders. Resource depots and workers are handled by
    // the existing extreme-emergency path to avoid a circular CC/SCV request.
    if (producerType != BWAPI::UnitTypes::None &&
        producerType != BWAPI::UnitTypes::Terran_SCV &&
        producerType != BWAPI::UnitTypes::Terran_Command_Center &&
        producerType != type &&
        producerType.getRace() == BWAPI::Races::Terran &&
        plannedTerranTypeCount(producerType) == 0)
    {
        const BWAPI::UnitType missing =
            missingTerranPrerequisite(producerType, depth + 1);

        return missing == BWAPI::UnitTypes::None
            ? producerType
            : missing;
    }

    return type;
}

BWAPI::UnitType ProductionManager::missingTerranPrerequisite(
    const MacroAct & act)
{
    if (act.isUnit())
    {
        const BWAPI::UnitType type = act.getUnitType();

        for (const std::pair<const BWAPI::UnitType, int> & requirement : type.requiredUnits())
        {
            const BWAPI::UnitType requiredType = requirement.first;

            if (requiredType.getRace() == BWAPI::Races::Terran &&
                plannedTerranTypeCount(requiredType) == 0)
            {
                return missingTerranPrerequisite(requiredType);
            }
        }
    }
    else if (act.isTech())
    {
        const BWAPI::UnitType requiredType = act.getTechType().requiredUnit();

        if (requiredType.getRace() == BWAPI::Races::Terran &&
            plannedTerranTypeCount(requiredType) == 0)
        {
            return missingTerranPrerequisite(requiredType);
        }
    }
    else if (act.isUpgrade())
    {
        const BWAPI::UpgradeType upgrade = act.getUpgradeType();
        const int nextLevel = std::min(
            the.self()->getUpgradeLevel(upgrade) + 1,
            the.self()->getMaxUpgradeLevel(upgrade));
        const BWAPI::UnitType requiredType = upgrade.whatsRequired(nextLevel);

        if (requiredType.getRace() == BWAPI::Races::Terran &&
            plannedTerranTypeCount(requiredType) == 0)
        {
            return missingTerranPrerequisite(requiredType);
        }
    }

    const BWAPI::UnitType producerType = act.whatBuilds();

    if (producerType != BWAPI::UnitTypes::None &&
        producerType.getRace() == BWAPI::Races::Terran &&
        plannedTerranTypeCount(producerType) == 0)
    {
        return missingTerranPrerequisite(producerType);
    }

    return BWAPI::UnitTypes::None;
}

// Urgent reactions have a stricter requirement than the normal persistent
// planner: a prerequisite buried in the opening queue is not usable yet. Walk
// the same Terran prerequisite graph, but accept only completed structures.
// This lets StrategyManager accelerate the next reachable prerequisite without
// ever placing an unusable Bunker/unit/research action above the JSON opening.
BWAPI::UnitType ProductionManager::missingCompletedTerranPrerequisite(
    BWAPI::UnitType type,
    int depth) const
{
    if (type == BWAPI::UnitTypes::None ||
        type == BWAPI::UnitTypes::Unknown ||
        type.getRace() != BWAPI::Races::Terran ||
        the.my.completed.count(type) > 0)
    {
        return BWAPI::UnitTypes::None;
    }

    if (depth >= 8)
    {
        return type;
    }

    for (const std::pair<const BWAPI::UnitType, int> & requirement :
        type.requiredUnits())
    {
        const BWAPI::UnitType requiredType = requirement.first;

        if (requiredType.getRace() == BWAPI::Races::Terran &&
            the.my.completed.count(requiredType) < requirement.second)
        {
            const BWAPI::UnitType missing =
                missingCompletedTerranPrerequisite(requiredType, depth + 1);

            return missing == BWAPI::UnitTypes::None
                ? requiredType
                : missing;
        }
    }

    const BWAPI::UnitType producerType = type.whatBuilds().first;

    // SCVs construct ordinary buildings. Command Centers train SCVs and are
    // restored by the existing extreme-economy recovery path.
    if (producerType != BWAPI::UnitTypes::None &&
        producerType != BWAPI::UnitTypes::Terran_SCV &&
        producerType != BWAPI::UnitTypes::Terran_Command_Center &&
        producerType != type &&
        producerType.getRace() == BWAPI::Races::Terran &&
        the.my.completed.count(producerType) == 0)
    {
        const BWAPI::UnitType missing =
            missingCompletedTerranPrerequisite(producerType, depth + 1);

        return missing == BWAPI::UnitTypes::None
            ? producerType
            : missing;
    }

    return type;
}

BWAPI::UnitType ProductionManager::getMissingCompletedTerranPrerequisite(
    const MacroAct & act) const
{
    

    if (act.isUnit())
    {
        const BWAPI::UnitType type = act.getUnitType();

        for (const std::pair<const BWAPI::UnitType, int> & requirement :
            type.requiredUnits())
        {
            const BWAPI::UnitType requiredType = requirement.first;

            if (requiredType.getRace() == BWAPI::Races::Terran &&
                the.my.completed.count(requiredType) < requirement.second)
            {
                return missingCompletedTerranPrerequisite(requiredType);
            }
        }
    }
    else if (act.isTech())
    {
        const BWAPI::UnitType requiredType = act.getTechType().requiredUnit();

        if (requiredType.getRace() == BWAPI::Races::Terran &&
            the.my.completed.count(requiredType) == 0)
        {
            return missingCompletedTerranPrerequisite(requiredType);
        }
    }
    else if (act.isUpgrade())
    {
        const BWAPI::UpgradeType upgrade = act.getUpgradeType();
        const int nextLevel = std::min(
            the.self()->getUpgradeLevel(upgrade) + 1,
            the.self()->getMaxUpgradeLevel(upgrade));
        const BWAPI::UnitType requiredType = upgrade.whatsRequired(nextLevel);

        if (requiredType.getRace() == BWAPI::Races::Terran &&
            the.my.completed.count(requiredType) == 0)
        {
            return missingCompletedTerranPrerequisite(requiredType);
        }
    }

    const BWAPI::UnitType producerType = act.whatBuilds();

    if (producerType != BWAPI::UnitTypes::None &&
        producerType != BWAPI::UnitTypes::Terran_SCV &&
        producerType != BWAPI::UnitTypes::Terran_Command_Center &&
        producerType.getRace() == BWAPI::Races::Terran &&
        the.my.completed.count(producerType) == 0)
    {
        return missingCompletedTerranPrerequisite(producerType);
    }

    return BWAPI::UnitTypes::None;
}

bool ProductionManager::terranMacroProductionActive() const
{
    // Pending goals and reserved resources are intentions, not progress.
    // A blocked Comsat or expansion must not disable stall recovery forever.
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->getType().isBuilding())
        {
            continue;
        }

        if (unit->isBeingConstructed() ||
            unit->isResearching() ||
            unit->isUpgrading())
        {
            return true;
        }

        // Continuous SCV production is healthy economy, but it must not mask a
        // dead strategic plan forever. Previously one Command Center training
        // workers made an impossible Factory/tech rule look like "macro is
        // active", so the recovery path was never reached. Count only combat
        // production as evidence that the current strategic wave is moving.
        if (unit->isTraining() && !unit->getType().isResourceDepot())
        {
            return true;
        }
    }

    return false;
}

// Dynamic emergency insertions must not move a JSON scouting command to a
// different effective supply/timing. The opening owns its order until the
// scheduled scout command itself reaches the front of the queue.
bool ProductionManager::terranOpeningScoutPending()
{
    if (the.selfRace() != BWAPI::Races::Terran || _outOfBook)
    {
        return false;
    }

    for (int i = 0; i < int(_queue.size()); ++i)
    {
        const MacroAct & act = _queue[i].macroAct;

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

// A Machine Shop can perform only one research job at a time. If the next
// Spider Mines, Ion Thrusters, Siege Mode or Charon Boosters job has no free
// producer, prepare another Shop on a Factory which can accept an add-on.
// Keep the research item in place: it will start as soon as either Shop is
// free. This is intentionally producer-driven rather than strategy-specific,
// so JSON openings and later Terran rule plans follow the same safe behavior.
ProductionManager::ResearchProducerPreparation
ProductionManager::prepareMachineShopForResearch(
    const MacroAct& research,
    std::string& decision)
{
    decision.clear();

    const BWAPI::UnitType shopType =
        BWAPI::UnitTypes::Terran_Machine_Shop;

    if (the.selfRace() != BWAPI::Races::Terran ||
        (!research.isTech() && !research.isUpgrade()) ||
        research.whatBuilds() != shopType)
    {
        return ResearchProducerPreparation::NotApplicable;
    }

    // A free Shop already exists. The caller should use it normally; lack of
    // resources or another prerequisite is not a reason to add more Shops.
    if (getProducer(research))
    {
        return ResearchProducerPreparation::NotApplicable;
    }

    const std::string researchName =
        NiceMacroActName(research.getName());

    // Do not issue the same add-on repeatedly while BWAPI is acknowledging it,
    // while it is under construction, or while ProductionGoal is retrying a
    // blocked footprint. A later Machine Shop entry still sitting behind this
    // research in the JSON queue is deliberately not counted: waiting for that
    // entry would recreate the exact circular opening-book stall fixed here.
    const bool shopAlreadyPreparing =
        the.my.all.count(shopType) > the.my.completed.count(shopType) ||
        getPendingGoalCount(shopType) > 0;

    if (shopAlreadyPreparing)
    {
        decision =
            "WAIT Machine Shop being prepared for " + researchName;
        return ResearchProducerPreparation::Waiting;
    }

    const BWAPI::UnitType factoryType =
        BWAPI::UnitTypes::Terran_Factory;

    BWAPI::Unit readyFactory = nullptr;
    BWAPI::Unit busyFactory = nullptr;
    bool factoryOrAddonBeingConstructed =
        BuildingManager::Instance().isBeingBuilt(factoryType) ||
        BuildingManager::Instance().getNumUnstarted(factoryType) > 0;

    for (BWAPI::Unit factory : the.self()->getUnits())
    {
        if (!factory || !factory->exists() ||
            factory->getType() != factoryType ||
            factory->isLifted() || factory->getAddon())
        {
            continue;
        }

        if (!factory->isCompleted() || factory->isConstructing())
        {
            factoryOrAddonBeingConstructed = true;
            continue;
        }

        // Prefer an idle Factory so the add-on can start immediately. A busy
        // naked Factory is still a valid fallback; ProductionGoal waits for
        // its current Vulture/Goliath to finish and then attaches the Shop.
        if (!factory->isTraining() &&
            !factory->isResearching() &&
            !factory->isUpgrading() &&
            factory->getRemainingTrainTime() <= 0)
        {
            readyFactory = factory;
            break;
        }

        if (!busyFactory)
        {
            busyFactory = factory;
        }
    }

    BWAPI::Unit selectedFactory = readyFactory
        ? readyFactory
        : busyFactory;

    if (!selectedFactory && !factoryOrAddonBeingConstructed)
    {
        decision =
            "WAIT: no Factory can add Machine Shop for " + researchName;
        return ResearchProducerPreparation::Waiting;
    }

    if (selectedFactory)
    {
        _goals.push_front(ProductionGoal(
            MacroAct(shopType, selectedFactory)));

        decision =
            "PREPARE Machine Shop at Factory " +
            std::to_string(selectedFactory->getID()) +
            " for " + researchName;
    }
    else
    {
        // A Factory is already being built. The normal ProductionGoal logic
        // will acquire it after completion instead of dropping the research.
        _goals.push_front(ProductionGoal(MacroAct(shopType)));
        decision =
            "PREPARE Machine Shop after Factory completion for " +
            researchName;
    }

    _lastProductionFrame = the.now();
    _terranRuleLastProgressFrame = the.now();
    return ResearchProducerPreparation::Scheduled;
}

void ProductionManager::refreshTerranRulePlan()
{
    const auto planStarted = std::chrono::steady_clock::now();
    _terranRulePlan = StrategyManager::Instance().getBuildOrderGoal();
    _lastTerranRuleGoalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - planStarted).count();
    _terranRuleBaseline.clear();
    _terranRuleIssued.clear();
    _terranRulePlanFrame = the.now();

    for (const MetaPair & rule : _terranRulePlan)
    {
        const int key = terranRuleKey(rule.first);

        if (key >= 0)
        {
            _terranRuleBaseline[key] = terranRuleProgress(rule.first);
            _terranRuleIssued[key] = 0;
            if (rule.first.isBuilding())
            {
                const int typeID = rule.first.getUnitType().getID();
                _terranStructuralTargets[typeID] = std::max(
                    _terranStructuralTargets[typeID], terranRuleTarget(rule));
            }
        }
    }

    if (_terranRuleLastProgressFrame < 0)
    {
        _terranRuleLastProgressFrame = the.now();
    }
    _lastTerranRulePlanMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - planStarted).count();
}

// Execute the current Terran goal as persistent production rules. Buildings
// still pass through the normal queue/BuildingManager. Units, tech and
// upgrades start directly on an available producer, so a busy Barracks cannot
// serialize independent Factory production.
bool ProductionManager::manageTerranRuleProduction()
{
    if (the.selfRace() != BWAPI::Races::Terran || !_outOfBook)
    {
        return false;
    }

    const int now = the.now();

    if (_terranRuleLastObservedFrame > now)
    {
        resetTerranRuleProduction();
    }
    _terranRuleLastObservedFrame = now;

    // Strategy decisions are intentionally refreshed once per second, not on
    // every frame. Urgent production continues to react every frame.
    if (_terranRulePlanFrame < 0 ||
        now >= _terranRulePlanFrame + 24)
    {
        refreshTerranRulePlan();
    }

    if (_terranRulePlan.empty())
    {
        // An empty goal at maximum supply is a completed macro state, not a
        // planning failure. Avoid waking BOSS only to receive a large or
        // redundant recovery queue while the army is already maxed.
        if (the.self()->supplyUsed() > 400 - 12 ||
            terranMacroProductionActive())
        {
            setTerranRuleDecision(
                the.self()->supplyUsed() > 400 - 12
                ? "HOLD: army is at maximum supply"
                : "WAIT: current production is still running");
            return true;
        }

        setTerranRuleDecision("FALLBACK: no valid strategic goal");
        return false;
    }

    bool hasUnfinishedRule = false;
    std::string firstWaitingDecision;
    int reservedMinerals = 0;
    int reservedGas = 0;
    const bool queueAlreadyHasWork = !_queue.isEmpty();
    const MacroAct* queuedAct =
        queueAlreadyHasWork
        ? &_queue.getHighestPriorityItem().macroAct
        : nullptr;
    // Optional AirTanks research must not monopolize independent production
    // lanes while its Armory/other research building is occupied. The queued
    // item stays in place and starts normally as soon as its producer is free.
    bool blockedAirTanksResearch = false;
    if (StrategyManager::Instance().airTanksPlanActive() && queuedAct &&
        (queuedAct->isTech() || queuedAct->isUpgrade()) &&
        the.self()->minerals() >= queuedAct->mineralPrice() &&
        the.self()->gas() >= queuedAct->gasPrice())
    {
        BWAPI::Unit researchProducer = getProducer(*queuedAct);
        blockedAirTanksResearch = !researchProducer ||
            !canMakeNow(researchProducer, *queuedAct);
    }

    // A queued mobile unit must not prevent independent strategic
    // infrastructure forever. This is especially important at maximum supply:
    // a blocked Tank cannot consume supply, while a Factory or Machine Shop
    // could be started immediately. Structural/research/command entries remain
    // true barriers and keep their ordering.
    const bool queueBlocksStructuralRules =
        queuedAct &&
        !blockedAirTanksResearch &&
        (queuedAct->isBuilding() ||
            queuedAct->isAddon() ||
            queuedAct->isTech() ||
            queuedAct->isUpgrade() ||
            queuedAct->isCommand());

    // Protect the next executable queue item from rule spending. If its
    // producer is merely occupied, do not reserve its cost: that was the old
    // Ghost-blocks-Goliath failure mode which this executor is meant to avoid.
    if (queueAlreadyHasWork)
    {
        if (queuedAct->isBuilding() ||
            queuedAct->isAddon() ||
            queuedAct->isTech() ||
            queuedAct->isUpgrade() ||
            queuedAct->isCommand() ||
            getProducer(*queuedAct))
        {
            reservedMinerals = queuedAct->mineralPrice();
            reservedGas = queuedAct->gasPrice();
        }
    }

    // Add-ons are moved out of the visible queue into ProductionGoal. Keep
    // their small resource reservation visible to the parallel rule executor,
    // otherwise Goliaths/Tanks can repeatedly spend the last gas while a
    // Comsat or Machine Shop waits forever.
    for (const ProductionGoal & goal : _goals)
    {
        if (goal.act.isAddon())
        {
            reservedMinerals = std::max(
                reservedMinerals,
                goal.act.mineralPrice());
            reservedGas = std::max(
                reservedGas,
                goal.act.gasPrice());
        }
    }

    // Production lanes are independent. An expansion or an extra production
    // building may save in parallel, but it must not idle an already completed
    // Barracks/Factory/Starport which can make a combat unit now.
    auto reserveAppliesToAct =
        [&](const MacroAct & act) -> bool
        {
            const BWAPI::UnitType actType = act.isUnit()
                ? act.getUnitType()
                : BWAPI::UnitTypes::None;
            const bool pureAirFactoryArmyLane =
                StrategyManager::Instance().pureAirPlanActive() &&
                act.isUnit() &&
                the.my.completed.count(
                    BWAPI::UnitTypes::Terran_Factory) > 0 &&
                actType == BWAPI::UnitTypes::Terran_Goliath &&
                the.my.completed.count(
                    BWAPI::UnitTypes::Terran_Armory) > 0;

            // Once the relevant Factory prerequisites are operational, an
            // available Factory must build the PureAir Goliath army while the
            // second Armory, optional support buildings and upgrades proceed in
            // their own lanes. Existing Tanks/Vultures remain rear support, but
            // this generic lane must never recreate their old production floor.
            // With no visible queue item, the only reserve is an add-on goal and
            // it must not idle that Factory either.
            if (pureAirFactoryArmyLane)
            {
                if (!queuedAct)
                {
                    return false;
                }
                if (queuedAct->isTech() || queuedAct->isUpgrade())
                {
                    return false;
                }
                if (queuedAct->isUnit())
                {
                    const BWAPI::UnitType queuedType =
                        queuedAct->getUnitType();
                    if (queuedType == BWAPI::UnitTypes::Terran_Armory ||
                        queuedType == BWAPI::UnitTypes::Terran_Engineering_Bay ||
                        queuedType == BWAPI::UnitTypes::Terran_Academy ||
                        queuedType == BWAPI::UnitTypes::Terran_Starport ||
                        queuedType == BWAPI::UnitTypes::Terran_Science_Facility ||
                        queuedType == BWAPI::UnitTypes::Terran_Machine_Shop)
                    {
                        return false;
                    }
                }
            }

            if (!queuedAct || !act.isCombatUnit())
            {
                return true;
            }

            // A temporarily blocked upgrade has no use for idle Factory or
            // Starport time. Keep its resource reserve for structures, but
            // let combat units use their own production lanes.
            if (blockedAirTanksResearch)
            {
                return false;
            }

            if (!queuedAct->isUnit())
            {
                return true;
            }

            const BWAPI::UnitType queuedType = queuedAct->getUnitType();
            const BWAPI::UnitType producerType = act.whatBuilds();

            // In BioTank a Tank waiting in the visible queue must not
            // reserve all minerals against an independent Barracks. The
            // infantry lane owns 80% of army supply and can spend 50 now.
            if (StrategyManager::Instance().bioTankPlanActive() &&
                producerType == BWAPI::UnitTypes::Terran_Barracks &&
                queuedType == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
            {
                return false;
            }

            // Economy lane: do not freeze army production while saving 400 for
            // the next Command Center (or 50 for a queued SCV).
            if (queuedType == BWAPI::UnitTypes::Terran_Command_Center ||
                queuedType == BWAPI::UnitTypes::Terran_SCV)
            {
                return false;
            }

            // Infrastructure scaling lane: if at least one producer already
            // exists, it should keep working while another copy is being saved.
            if (queuedType == BWAPI::UnitTypes::Terran_Barracks &&
                producerType == BWAPI::UnitTypes::Terran_Barracks &&
                the.my.completed.count(BWAPI::UnitTypes::Terran_Barracks) > 0)
            {
                return false;
            }

            if (queuedType == BWAPI::UnitTypes::Terran_Factory &&
                producerType == BWAPI::UnitTypes::Terran_Factory &&
                the.my.completed.count(BWAPI::UnitTypes::Terran_Factory) > 0)
            {
                return false;
            }

            if (queuedType == BWAPI::UnitTypes::Terran_Starport &&
                producerType == BWAPI::UnitTypes::Terran_Starport &&
                the.my.completed.count(BWAPI::UnitTypes::Terran_Starport) > 0)
            {
                return false;
            }

            return true;
        };

    auto canSpendWithoutBreakingReserve =
        [&](const MacroAct & act) -> bool
        {
            int minerals = 0;
            int gas = 0;
            getAvailableResources(act, minerals, gas);

            if (act.mineralPrice() > minerals || act.gasPrice() > gas)
            {
                return false;
            }

            const bool respectReserve =
                reserveAppliesToAct(act);

            if (respectReserve &&
                act.mineralPrice() > 0 &&
                reservedMinerals > 0 &&
                minerals < reservedMinerals + act.mineralPrice())
            {
                return false;
            }

            if (respectReserve &&
                act.gasPrice() > 0 &&
                reservedGas > 0 &&
                gas < reservedGas + act.gasPrice())
            {
                return false;
            }

            return true;
        };

    auto rememberWaitingResources =
        [&](const MacroAct & act)
        {
            if (reservedMinerals == 0 && reservedGas == 0)
            {
                reservedMinerals = act.mineralPrice();
                reservedGas = act.gasPrice();
            }
        };

    auto rememberWaitingDecision =
        [&](const std::string & decision)
        {
            if (firstWaitingDecision.empty())
            {
                firstWaitingDecision = decision;
            }
        };

    for (const MetaPair & rule : _terranRulePlan)
    {
        const MacroAct & act = rule.first;
        const int key = terranRuleKey(act);
        const size_t target = terranRuleTarget(rule);
        const std::string actName = NiceMacroActName(act.getName());

        // Defensive clamping: malformed or duplicated strategic goals must
        // never wait for a fourth upgrade level or a second copy of a
        // one-time technology. That kind of impossible goal used to block the
        // global production queue indefinitely.
        if (key < 0 || target == 0)
        {
            continue;
        }

        if (act.isUnit() && act.getUnitType() == BWAPI::UnitTypes::Terran_Marine &&
            BioTankPolicy::reserveGoliathSupply())
        {
            rememberWaitingDecision("WAIT supply for BioTank anti-air counter");
            continue;
        }

        // A strategy transition may happen between the once-per-second rule
        // refreshes. Never execute a stale ground-production rule after the
        // Carrier/Mutalisk technology response has already become PureAir.
        if (StrategyManager::Instance().pureAirPlanActive())
        {
            bool conflictsWithPureAir = false;

            if (act.isUnit())
            {
                const BWAPI::UnitType type = act.getUnitType();
                conflictsWithPureAir =
                    type == BWAPI::UnitTypes::Terran_Firebat ||
                    type == BWAPI::UnitTypes::Terran_Vulture ||
                    type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                    type == BWAPI::UnitTypes::Terran_Valkyrie ||
                    type == BWAPI::UnitTypes::Terran_Dropship ||
                    type == BWAPI::UnitTypes::Terran_Battlecruiser ||
                    type == BWAPI::UnitTypes::Terran_Marine ||
                    type == BWAPI::UnitTypes::Terran_Medic;
            }
            else if (act.isTech())
            {
                conflictsWithPureAir =
                    act.getTechType() == BWAPI::TechTypes::Spider_Mines;
            }
            else if (act.isUpgrade())
            {
                conflictsWithPureAir =
                    act.getUpgradeType() == BWAPI::UpgradeTypes::Ion_Thrusters;
            }

            if (conflictsWithPureAir)
            {
                continue;
            }
        }

        const size_t baseline = _terranRuleBaseline[key];
        const size_t issued = _terranRuleIssued[key];
        const size_t effectiveProgress = std::max(
            terranRuleProgress(act),
            baseline + issued);

        if (effectiveProgress >= target)
        {
            continue;
        }

        hasUnfinishedRule = true;

        // Build the deepest missing prerequisite first. Only one building is
        // posted at a time, preserving the existing placement and worker code.
        const BWAPI::UnitType missing = missingTerranPrerequisite(act);

        if (missing != BWAPI::UnitTypes::None)
        {
            const MacroAct prerequisite(missing);

            if (!queueBlocksStructuralRules &&
                plannedTerranTypeCount(missing) == 0)
            {
                if (canSpendWithoutBreakingReserve(prerequisite))
                {
                    _queue.queueAsHighestPriority(prerequisite);
                    setTerranRuleDecision(
                        "BUILD " + NiceMacroActName(missing.getName()) +
                        ": prerequisite for " + actName);
                    _terranRuleLastProgressFrame = now;
                    _lastProductionFrame = now;
                    return true;
                }

                rememberWaitingResources(prerequisite);
                rememberWaitingDecision(
                    "SAVE " + std::to_string(prerequisite.mineralPrice()) +
                    "M/" + std::to_string(prerequisite.gasPrice()) +
                    "G for " + NiceMacroActName(missing.getName()) +
                    " -> " + actName);
            }
            else
            {
                rememberWaitingDecision(
                    "WAIT: " + NiceMacroActName(missing.getName()) +
                    " is needed before " + actName);
            }

            continue;
        }

        if (act.isBuilding())
        {
            if (queueBlocksStructuralRules)
            {
                // Keep economy and Tanks capacity independent of a blocked
                // structural/research entry. Protect its full resource reserve,
                // require a legal site, and post only one unstarted copy.
                // Opening books and other strategies retain their ordering.
                const BWAPI::UnitType economyType = act.getUnitType();
                const bool tankFactoryScaling =
                    economyType == BWAPI::UnitTypes::Terran_Factory &&
                    StrategyManager::Instance().getOpeningGroup() == "tanks" &&
                    the.my.completed.count(economyType) > 0;
                if ((economyType == BWAPI::UnitTypes::Terran_Command_Center ||
                     economyType == BWAPI::UnitTypes::Terran_Refinery ||
                     tankFactoryScaling) &&
                    !_queue.anyInQueue(economyType) &&
                    canSpendWithoutBreakingReserve(act) &&
                    BuildingManager::Instance().getNumUnstarted(economyType) == 0)
                {
                    Building economyBuilding = makeBuildingForPrediction(act, false);
                    if (economyBuilding.desiredPosition.isValid() &&
                        BuildingManager::Instance().getBuildingLocation(
                            economyBuilding).isValid())
                    {
                        BuildingManager::Instance().addBuildingTask(
                            act, economyBuilding.desiredPosition, nullptr, false);
                        setTerranRuleDecision("BUILD " + actName + " (parallel)");
                        ++_terranRuleIssued[key];
                        _terranRuleLastProgressFrame = now;
                        _lastProductionFrame = now;
                        return true;
                    }
                }
                rememberWaitingDecision(
                    "WAIT queue, then build " + actName);
                continue;
            }

            if (canSpendWithoutBreakingReserve(act))
            {
                _queue.queueAsHighestPriority(act);
                setTerranRuleDecision(
                    "BUILD " + actName + " " +
                    std::to_string(int(effectiveProgress + 1)) + "/" +
                    std::to_string(int(target)));
                ++_terranRuleIssued[key];
                _terranRuleLastProgressFrame = now;
                _lastProductionFrame = now;
                return true;
            }

            rememberWaitingResources(act);
            rememberWaitingDecision(
                "SAVE " + std::to_string(act.mineralPrice()) +
                "M/" + std::to_string(act.gasPrice()) +
                "G for " + actName);
            continue;
        }

        BWAPI::Unit producer = getProducer(act);

        if (!producer)
        {
            std::string machineShopDecision;
            const ResearchProducerPreparation preparation =
                prepareMachineShopForResearch(
                    act,
                    machineShopDecision);

            if (preparation == ResearchProducerPreparation::Scheduled)
            {
                setTerranRuleDecision(machineShopDecision);
                return true;
            }

            if (preparation == ResearchProducerPreparation::Waiting)
            {
                rememberWaitingDecision(machineShopDecision);
                continue;
            }

            bool liftedProducerExists = false;
            const BWAPI::UnitType producerType = act.whatBuilds();

            for (BWAPI::Unit unit : the.self()->getUnits())
            {
                if (unit &&
                    unit->exists() &&
                    unit->isCompleted() &&
                    unit->getType() == producerType &&
                    unit->isLifted())
                {
                    liftedProducerExists = true;
                    break;
                }
            }

            // Posting one job lets the existing landing repair identify which
            // lifted production building is required. Do not post jobs behind
            // a merely busy landed producer.
            if (!queueBlocksStructuralRules && liftedProducerExists)
            {
                _queue.queueAsHighestPriority(act);
                setTerranRuleDecision(
                    "LAND " + NiceMacroActName(producerType.getName()) +
                    " to make " + actName);
                ++_terranRuleIssued[key];
                _terranRuleLastProgressFrame = now;
                _lastProductionFrame = now;
                return true;
            }

            // A producer may simply be busy. Unlike the global queue, that is
            // not a reason to block rules using other producer types.
            rememberWaitingDecision(
                "WAIT free " + NiceMacroActName(producerType.getName()) +
                " for " + actName);
            continue;
        }

        if (!canSpendWithoutBreakingReserve(act))
        {
            rememberWaitingResources(act);
            rememberWaitingDecision(
                "SAVE " + std::to_string(act.mineralPrice()) +
                "M/" + std::to_string(act.gasPrice()) +
                "G for " + actName);
            continue;
        }

        if (act.isUnit() &&
            act.supplyRequired() > the.self()->supplyTotal() - the.self()->supplyUsed() &&
            the.self()->supplyTotal() < 400)
        {
            const BWAPI::UnitType depot = BWAPI::UnitTypes::Terran_Supply_Depot;
            bool depotPending =
                the.my.all.count(depot) > the.my.completed.count(depot) ||
                BuildingManager::Instance().getNumUnstarted(depot) > 0 ||
                _queue.numInQueue(depot) > 0;

            if (!depotPending)
            {
                for (const ProductionGoal & goal : _goals)
                {
                    if (goal.act.isUnit() && goal.act.getUnitType() == depot)
                    {
                        depotPending = true;
                        break;
                    }
                }
            }

            if (!depotPending)
            {
                const MacroAct supply(depot);

                if (!queueAlreadyHasWork &&
                    canSpendWithoutBreakingReserve(supply))
                {
                    _queue.queueAsHighestPriority(supply);
                    setTerranRuleDecision(
                        "BUILD Supply Depot: supply needed for " + actName);
                    _terranRuleLastProgressFrame = now;
                    _lastProductionFrame = now;
                    return true;
                }

                rememberWaitingResources(supply);
                rememberWaitingDecision(
                    "SAVE 100M for Supply Depot before " + actName);
            }
            else
            {
                rememberWaitingDecision(
                    "WAIT Supply Depot before " + actName);
            }

            continue;
        }

        if (canMakeNow(producer, act) && create(producer, BuildOrderItem(act)))
        {
            setTerranRuleDecision(
                "MAKE " + actName + " " +
                std::to_string(int(effectiveProgress + 1)) + "/" +
                std::to_string(int(target)) + " at " +
                NiceMacroActName(producer->getType().getName()));
            ++_terranRuleIssued[key];
            _terranRuleLastProgressFrame = now;
            _lastProductionFrame = now;

            // The Tanks strategy exposes one Factory composition decision at a
            // time. Re-evaluate immediately after a Factory starts a unit so
            // the next free Factory gets the unit with the largest new deficit
            // instead of following a stale one-second rolling target.
            if (producer->getType() == BWAPI::UnitTypes::Terran_Factory &&
                act.isUnit())
            {
                const BWAPI::UnitType made = act.getUnitType();

                if (made == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                    made == BWAPI::UnitTypes::Terran_Vulture ||
                    made == BWAPI::UnitTypes::Terran_Goliath)
                {
                    _terranRulePlanFrame = -1;
                }
            }

            return true;
        }

        rememberWaitingDecision(
            "WAIT: " + actName + " is not legal this frame");
    }

    if (!hasUnfinishedRule)
    {
        // Queued/in-progress work counts toward the target. Keep the normal
        // one-second refresh; a complete wave must not rebuild its plan every
        // frame while those same buildings are still being constructed.
        setTerranRuleDecision("WAIT: current strategic work is committed");
        return true;
    }

    if (!firstWaitingDecision.empty())
    {
        setTerranRuleDecision(firstWaitingDecision);
    }

    if (terranMacroProductionActive())
    {
        return true;
    }

    // Keep BOSS as a recovery path for a genuinely impossible rule set. It is
    // not used merely because one producer is occupied or resources are being
    // accumulated for a valid rule.
    if (_terranRuleLastProgressFrame >= 0 &&
        now < _terranRuleLastProgressFrame + 24 * 10)
    {
        return true;
    }

    resetTerranRuleProduction();
    setTerranRuleDecision("FALLBACK: rule plan stalled for 10 seconds");
    return false;
}

void ProductionManager::setBuildOrder(const BuildOrder & buildOrder)
{
    clearBuildingPrediction(true);
    _queue.clearAll();

    for (size_t i(0); i<buildOrder.size(); ++i)
    {
        _queue.queueAsLowestPriority(buildOrder[i]);
    }
    _queue.resetModified();
}

void ProductionManager::update() 
{
    _slowestProductionPhase = "";
    _slowestProductionPhaseMs = 0.0;
    _lastTerranRulePlanMs = _lastTerranRuleGoalMs = 0.0;
    auto phaseStart = std::chrono::steady_clock::now();
    const auto finishPhase = [this, &phaseStart](const char * name)
    {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(
            now - phaseStart).count();
        if (ms > _slowestProductionPhaseMs)
        {
            _slowestProductionPhase = name;
            _slowestProductionPhaseMs = ms;
        }
        phaseStart = now;
    };
    bool terranRulesCheckedThisFrame = false;

    // TODO make it more precise; it normally goes a little over
    // If we have reached a target amount of gas, take workers off gas.
    if (_targetGasAmount && the.self()->gatheredGas() >= _targetGasAmount)
    {
        WorkerManager::Instance().setCollectGas(false);
        _targetGasAmount = 0;           // clear the target
    }

    fixStuckCommandCenterTraining();
    fixStuckFactoryTraining();

    // If we're in trouble, adjust the production queue to help.
    // Includes scheduling supply as needed.
    _openingDefensiveMarineTarget = 0;
    StrategyManager::Instance().handleUrgentProductionIssues(_queue);
    StrategyManager::Instance().updateBioTankAttackState();
    StrategyManager::Instance().updateAirTanksAttackState();
    StrategyManager::Instance().updateTankAttackState();
    finishPhase("urgent");

    // A JSON opening may order the natural before it ever asks for a dynamic
    // StrategyManager goal. Open the verified wall for that builder here, so
    // the expansion cannot deadlock behind its own Barracks.
    manageTerranMainWall();
    finishPhase("wall");

    // BOSS and emergency queue edits can leave more copies of an upgrade than
    // there are levels left, or duplicate a technology which is already under
    // research. Remove only those provably redundant items before they can
    // become permanent queue barriers.
    dropRedundantQueueResearch();
    dropExcessQueuedCommandCenters();
    dropExcessQueuedUpgradeBuildings();

    // Drop malformed items from ordinary queues. A Terran JSON opening is
    // deliberately excluded: its next producer/prerequisite is often a
    // Factory, Machine Shop or Academy which is still under construction or
    // represented by a ProductionGoal. Treating that normal wait as
    // "unproducible" could consume the complete book in one frame and switch
    // every strategy to rule goals immediately.
    if (_outOfBook)
    {
        dropJammedItemsFromQueue();
    }

    // Carry out production goals, plus any other needed goal housekeeping.
    updateGoals();
    finishPhase("goals");

    // Goals and a freshly-finished BOSS search can edit the queue after the
    // early sanitation pass above. Enforce the structural hard caps again
    // before any item is executed or drawn this frame.
    dropExcessQueuedUpgradeBuildings();

    // If nothing is ready to go, use the Terran persistent rule executor. The
    // opening book is left only once; repeatedly calling the clearing version
    // here would erase freshly posted rule work and reset jam observation.
    if (_queue.isEmpty())
    {
        if (Config::Debug::DrawBuildOrderSearchInfo)
        {
            BWAPI::Broodwar->drawTextScreen(150, 10, "Nothing left to build, replanning.");
        }

        if (!_outOfBook)
        {
            //DebugQueuePrintf("build finished %d, minerals and gas %d %d",
            //	the.now(), the.self()->minerals(), the.self()->gas()
            //);
        }

        if (!_outOfBook)
        {
            goOutOfBookAndClearQueue();
        }

        const bool terranRulesActive =
            manageTerranRuleProduction();
        terranRulesCheckedThisFrame =
            the.selfRace() == BWAPI::Races::Terran && _outOfBook;

        if (!terranRulesActive)
        {
            StrategyManager::Instance().freshProductionPlan();
            dropExcessQueuedCommandCenters();
            dropExcessQueuedUpgradeBuildings();
        }
    }
    finishPhase("rules");

    // Build stuff from the production queue. A BOSS result may have become
    // available inside freshProductionPlan(), so keep the final guard here too.
    dropExcessQueuedUpgradeBuildings();
    moveLiftedBarracksForTankVision();
    finishPhase("vision");

    // A ground attack at a bio opening's base must not wait behind the next
    // expensive JSON item. Train at most one Marine per frame from a free
    // Barracks; the opening queue is otherwise left in its original order.
    const int openingBaseAttackers =
        !_outOfBook && the.selfRace() == BWAPI::Races::Terran &&
        StrategyManager::Instance().getOpeningGroup() == "bio"
            ? visibleGroundAttackersAtOwnedBase() : 0;
    bool emergencyMarineStarted = false;
    if (openingBaseAttackers > 0 && _lastProductionFrame != the.now())
    {
        _openingDefensiveMarineTarget = std::max(
            _openingDefensiveMarineTarget,
            std::min(12, std::max(6, 4 + 2 * openingBaseAttackers)));
        emergencyMarineStarted = manageOpeningDefensiveMarines(true);
    }
    if (!emergencyMarineStarted) manageBuildOrderQueue();
    finishPhase("queue");

    // During the JSON opening, turn the currently safe unit-only phase into
    // parallel goals. The ordered top item and every structural/command
    // barrier remain untouched.
    if (!_outOfBook &&
        the.selfRace() == BWAPI::Races::Terran &&
        _lastProductionFrame != the.now())
    {
        manageTerranOpeningGoals();
    }
    finishPhase("opening");

    // Loss replacement is a separate lane. It never inserts another Marine
    // ahead of JSON technology, workers or other producers.
    if (_lastProductionFrame != the.now()) manageOpeningDefensiveMarines();
    finishPhase("reinforce");

    if (_lastProductionFrame != the.now()) manageAirTanksOpeningVultures();

    // If an existing queue item could not start (for example a Ghost whose
    // only Barracks is busy), let an independent producer execute a rule. A
    // queue item which did start already owns this frame's production action.
    if (!terranRulesCheckedThisFrame &&
        _outOfBook &&
        the.selfRace() == BWAPI::Races::Terran &&
        _lastProductionFrame != the.now())
    {
        manageTerranRuleProduction();
    }
    finishPhase("fallback");

    prepareIdleFactoryMachineShop();
    finishPhase("idle");
}

// Use otherwise idle Factory time to prepare future Tank capacity. Normal
// units and opening/emergency priorities get their turn before this runs.
void ProductionManager::prepareIdleFactoryMachineShop()
{
    if (the.selfRace() != BWAPI::Races::Terran || !_outOfBook ||
        StrategyManager::Instance().pureAirPlanActive())
    {
        factoryIdleSinceFrame.clear();
        return;
    }

    const int now = the.now();
    if (now % 12 != 0)
    {
        return;
    }

    std::set<int> idleFactories;
    std::vector<BWAPI::Unit> candidates;
    const BWAPI::UnitType shopType = BWAPI::UnitTypes::Terran_Machine_Shop;
    for (BWAPI::Unit factory : the.self()->getUnits())
    {
        if (!factory || !factory->exists() ||
            factory->getType() != BWAPI::UnitTypes::Terran_Factory ||
            !factory->isCompleted() || factory->isLifted() ||
            factory->getAddon() || factory->isTraining() ||
            factory->isConstructing() || factory->isResearching() ||
            factory->isUpgrading() || factory->getRemainingTrainTime() > 0)
        {
            continue;
        }

        const int id = factory->getID();
        idleFactories.insert(id);
        auto inserted = factoryIdleSinceFrame.emplace(id, now);
        if (inserted.first->second > now)
        {
            inserted.first->second = now;
        }
        if (now >= inserted.first->second + 2 * 24)
        {
            candidates.push_back(factory);
        }
    }
    for (auto it = factoryIdleSinceFrame.begin(); it != factoryIdleSinceFrame.end(); )
    {
        if (idleFactories.count(it->first) == 0)
        {
            it = factoryIdleSinceFrame.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (candidates.empty() || _queue.anyInQueue(shopType) ||
        getPendingGoalCount(shopType) > 0)
    {
        return;
    }

    const int freeSupply = the.self()->supplyTotal() - the.self()->supplyUsed();
    const auto needsUnmodifiedFactory = [&](const MacroAct& act)
    {
        return act.isUnit() &&
            (act.getUnitType() == BWAPI::UnitTypes::Terran_Vulture ||
             act.getUnitType() == BWAPI::UnitTypes::Terran_Goliath) &&
            act.supplyRequired() <= freeSupply;
    };

    int protectedMinerals = 0;
    int protectedGas = 0;
    for (int i = int(_queue.size()) - 1; i >= std::max(0, int(_queue.size()) - 3); --i)
    {
        const MacroAct& act = _queue[i].macroAct;
        if (act.isCommand() || needsUnmodifiedFactory(act))
        {
            return;
        }
        protectedMinerals = std::max(protectedMinerals, act.mineralPrice());
        protectedGas = std::max(protectedGas, act.gasPrice());
    }
    for (const MetaPair& rule : _terranRulePlan)
    {
        if (terranRuleProgress(rule.first) >= terranRuleTarget(rule))
        {
            continue;
        }
        if (needsUnmodifiedFactory(rule.first))
        {
            return;
        }
        // Preserve the next real production job; blocked producers need no
        // second reservation in addition to BuildingManager's reservations.
        if (getProducer(rule.first))
        {
            protectedMinerals = std::max(protectedMinerals, rule.first.mineralPrice());
            protectedGas = std::max(protectedGas, rule.first.gasPrice());
        }
    }

    if (getFreeMinerals() < shopType.mineralPrice() + protectedMinerals ||
        getFreeGas() < shopType.gasPrice() + protectedGas)
    {
        return;
    }

    for (BWAPI::Unit factory : candidates)
    {
        MacroAct shop(shopType, factory);
        if (shop.isProducer(factory) && canMakeNow(factory, shop))
        {
            // The existing add-on goal owns retries and footprint clearing.
            // One request at a time avoids taking every Factory offline.
            _goals.push_front(ProductionGoal(shop));
            setTerranRuleDecision("PREPARE Machine Shop at idle Factory " +
                std::to_string(factory->getID()));
            return;
        }
    }
}

// BOSS occasionally returns dozens of duplicate Command Centers in one
// out-of-book plan. Keep opening-book expansion sequences intact, but in the
// repeating macro planner allow only one future expansion at a time. AirTanks
// can prepare one more gas expansion while its previous CC is constructing.
void ProductionManager::dropExcessQueuedCommandCenters()
{
    if (!_outOfBook || the.selfRace() != BWAPI::Races::Terran || _queue.isEmpty())
    {
        return;
    }

    const BWAPI::UnitType center = BWAPI::UnitTypes::Terran_Command_Center;
    const bool parallelAirTanksExpansion =
        StrategyManager::Instance().airTanksPlanActive() &&
        the.self()->minerals() >= 1600 && the.self()->gas() <= 250 &&
        BuildingManager::Instance().getNumUnstarted(center) == 0 &&
        the.my.all.count(center) == the.my.completed.count(center) + 1;
    const int allowedQueued =
        !BuildingManager::Instance().isBeingBuilt(center) ||
        parallelAirTanksExpansion ? 1 : 0;

    int kept = 0;
    for (int i = int(_queue.size()) - 1; i >= 0; --i)
    {
        const MacroAct act = _queue[i].macroAct;
        if (!act.isUnit() ||
            act.getUnitType() != BWAPI::UnitTypes::Terran_Command_Center)
        {
            continue;
        }

        if (kept < allowedQueued)
        {
            ++kept;
        }
        else
        {
            _queue.removeAt(size_t(i));
        }
    }
}

// Keep the highest-priority copies which can still produce a real result and
// remove only surplus research items. An upgrade already in progress consumes
// one of the remaining levels even though getUpgradeLevel() has not increased yet.
void ProductionManager::dropRedundantQueueResearch()
{
    if (_queue.isEmpty())
    {
        return;
    }

    BWAPI::Player self = the.self();

    for (BWAPI::UpgradeType type : BWAPI::UpgradeTypes::allUpgradeTypes())
    {
        const int currentLevel = self->getUpgradeLevel(type);
        const int maxLevel = self->getMaxUpgradeLevel(type);
        const int inProgress = self->isUpgrading(type) ? 1 : 0;
        const int levelsLeft = std::max(0, maxLevel - currentLevel - inProgress);

        int kept = 0;

        for (int i = int(_queue.size()) - 1; i >= 0; --i)
        {
            const MacroAct act = _queue[i].macroAct;

            if (!act.isUpgrade() || act.getUpgradeType() != type)
            {
                continue;
            }

            if (kept < levelsLeft)
            {
                ++kept;
                continue;
            }

            if (Config::Debug::DrawQueueFixInfo)
            {
                DebugQueuePrintf(
                    "queue: drop redundant %s (%d/%d%s)",
                    type.getName().c_str(),
                    currentLevel,
                    maxLevel,
                    inProgress ? ", active" : ""
                );
            }

            _queue.removeAt(size_t(i));
        }
    }

    for (BWAPI::TechType type : BWAPI::TechTypes::allTechTypes())
    {
        const int copiesAllowed =
            self->hasResearched(type) || self->isResearching(type) ? 0 : 1;

        int kept = 0;

        for (int i = int(_queue.size()) - 1; i >= 0; --i)
        {
            const MacroAct act = _queue[i].macroAct;

            if (!act.isTech() || act.getTechType() != type)
            {
                continue;
            }

            if (kept < copiesAllowed)
            {
                ++kept;
                continue;
            }

            if (Config::Debug::DrawQueueFixInfo)
            {
                DebugQueuePrintf(
                    "queue: drop redundant %s",
                    type.getName().c_str()
                );
            }

            _queue.removeAt(size_t(i));
        }
    }
}

// If something important was destroyed, we may want to react.
void ProductionManager::onUnitDestroy(BWAPI::Unit unit)
{
    // If it's not our unit, we don't care.
    if (!unit || unit->getPlayer() != the.self())
    {
        return;
    }

    if (_assignedWorkerForThisBuilding && unit == _assignedWorkerForThisBuilding)
    {
        DebugQueuePrintf(
            "production: assigned build worker died, clearing assignment"
        );

        clearBuildingPrediction(false);
    }
    
    
    

    // If it's a worker or a building, it affects the production plan.
    if (unit->getType().isWorker() && !_outOfBook)
    {
        if (the.selfRace() == BWAPI::Races::Terran &&
            terranOpeningScoutPending())
        {
            // Replacing a lost SCV at highest priority used to shift every
            // later book item, including the exact go-scout timing. The
            // StrategyManager economy rules replace the worker immediately
            // after the early scout command has executed.
            return;
        }

        // We lost a worker in the opening. Replace it.
        // This helps if a small number of workers are killed. If many are killed, you're toast anyway.
        // Still, it's better than breaking out of the opening altogether.
        _queue.queueAsHighestPriority(unit->getType());

        // If we have a barracks and no marines,
        // consider making a military unit first. To, you know, stay alive and stuff.
         
        {
            if (the.my.completed.count(BWAPI::UnitTypes::Terran_Barracks) > 0 &&
                the.my.all.count(BWAPI::UnitTypes::Terran_Marine) == 0 &&
                !_queue.anyInNextN(BWAPI::UnitTypes::Terran_Marine, 2) &&
                (the.self()->minerals() >= 100 || the.my.completed.count(BWAPI::UnitTypes::Terran_SCV) > 3))
            {
                _queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_Marine);
            }
        }
    }
    else if (unit->getType().isBuilding() &&
        !(UnitUtil::CanAttackAir(unit) || UnitUtil::CanAttackGround(unit)) &&
        unit->getType().supplyProvided() == 0)
    {
        // We lost a building other than static defense or supply. Terran can
        // re-evaluate its persistent targets without erasing an emergency
        // unit or detector which may already be at the front of the queue.
        
        {
            invalidateTerranRulePlan("critical building loss", true);
            _outOfBook = true;
            _lastProductionFrame = the.now();
        }
    }
}

// A prerequisite may already be under construction or may have been converted
// from the JSON queue into a ProductionGoal (Terran add-ons use that route).
// Treat it as planned so the next JSON item is not incorrectly discarded.
bool ProductionManager::terranOpeningTypeAvailableOrPlanned(
    BWAPI::UnitType type) const
{
    if (type == BWAPI::UnitTypes::None ||
        type == BWAPI::UnitTypes::Unknown)
    {
        return true;
    }

    if (the.my.all.count(type) > 0 ||
        BuildingManager::Instance().isBeingBuilt(type) ||
        BuildingManager::Instance().getNumUnstarted(type) > 0)
    {
        return true;
    }

    for (const ProductionGoal & goal : _goals)
    {
        if (goal.act.isUnit() && goal.act.getUnitType() == type)
        {
            return true;
        }
    }

    return false;
}

bool ProductionManager::terranOpeningRequirementsPlanned(
    const MacroAct & act) const
{
    if (act.isUnit())
    {
        const BWAPI::UnitType unitType = act.getUnitType();

        for (const std::pair<const BWAPI::UnitType, int> & requirement :
            unitType.requiredUnits())
        {
            if (requirement.first.getRace() == BWAPI::Races::Terran &&
                !terranOpeningTypeAvailableOrPlanned(requirement.first))
            {
                return false;
            }
        }
    }
    else if (act.isTech())
    {
        const BWAPI::UnitType required = act.getTechType().requiredUnit();

        if (required.getRace() == BWAPI::Races::Terran &&
            !terranOpeningTypeAvailableOrPlanned(required))
        {
            return false;
        }
    }
    else if (act.isUpgrade())
    {
        const BWAPI::UpgradeType upgrade = act.getUpgradeType();
        const int nextLevel = std::min(
            the.self()->getUpgradeLevel(upgrade) + 1,
            the.self()->getMaxUpgradeLevel(upgrade));
        const BWAPI::UnitType required = upgrade.whatsRequired(nextLevel);

        if (required.getRace() == BWAPI::Races::Terran &&
            !terranOpeningTypeAvailableOrPlanned(required))
        {
            return false;
        }
    }

    return terranOpeningTypeAvailableOrPlanned(act.whatBuilds());
}

// Drop any initial items from the queue that will demonstrably cause a production jam.
void ProductionManager::dropExcessQueuedUpgradeBuildings()
{
    

    const int hardCap = 2;
    const BWAPI::UnitType types[] =
    {
        BWAPI::UnitTypes::Terran_Engineering_Bay,
        BWAPI::UnitTypes::Terran_Armory
    };

    for (const BWAPI::UnitType type : types)
    {
        const int alreadyCommitted = the.my.all.count(type) +
            int(BuildingManager::Instance().getNumUnstarted(type));
        const int allowedQueued = std::max(0, hardCap - alreadyCommitted);
        int queued = int(_queue.numInQueue(type));

        // Index 0 is low priority. Keep urgent orders when replans overlap.
        for (int i = 0; i < int(_queue.size()) && queued > allowedQueued; )
        {
            const MacroAct act = _queue[i].macroAct;
            if (act.isUnit() && act.getUnitType() == type)
            {
                if (Config::Debug::DrawQueueFixInfo)
                {
                    DebugQueuePrintf("queue: drop excess %s (%d > %d)",
                        type.getName().c_str(), queued, allowedQueued);
                }
                _queue.removeAt(size_t(i));
                --queued;
                continue;
            }
            ++i;
        }
    }
}

void ProductionManager::dropJammedItemsFromQueue()
{
    while (!_queue.isEmpty() &&
        !_queue.getHighestPriorityItem().isGasSteal &&
        !itemCanBeProduced(_queue.getHighestPriorityItem().macroAct))
    {
        if (Config::Debug::DrawQueueFixInfo)
        {
            DebugQueuePrintf("queue: drop jammed %s", _queue.getHighestPriorityItem().macroAct.getName().c_str());
        }
        _queue.removeHighestPriorityItem();
    }
}

// Return false if the item definitely can't be made next.
// This doesn't yet try to handle all cases, so it can return false when it shouldn't.
bool ProductionManager::itemCanBeProduced(const MacroAct& act) const
{
    // A command can always be executed.
    // An addon is a goal that can always be posted (though it may fail).
    if (act.isCommand() || act.isAddon())
    {
        return true;
    }

    // A busy research building is temporary, not a reason to discard valid
    // future research. Completed/in-progress duplicates are removed centrally
    // by dropRedundantQueueResearch().
    if (act.isUpgrade())
    {
        const BWAPI::UpgradeType type = act.getUpgradeType();

        if (the.self()->getUpgradeLevel(type) >= the.self()->getMaxUpgradeLevel(type))
        {
            return false;
        }

        return
            act.hasEventualProducer() ||
            BuildingManager::Instance().isBeingBuilt(act.whatBuilds()) ||
            (the.selfRace() == BWAPI::Races::Terran &&
                !_outOfBook &&
                terranOpeningRequirementsPlanned(act));
    }

    if (act.isTech())
    {
        const BWAPI::TechType type = act.getTechType();

        if (the.self()->hasResearched(type) || the.self()->isResearching(type))
        {
            return false;
        }

        return
            act.hasEventualProducer() ||
            BuildingManager::Instance().isBeingBuilt(act.whatBuilds()) ||
            (the.selfRace() == BWAPI::Races::Terran &&
                !_outOfBook &&
                terranOpeningRequirementsPlanned(act));
    }

    // Barracks units can be temporarily impossible only because the Barracks is lifted.
    // Do not drop them as jammed if we have any Barracks and the required tech exists.
    if (act.isUnit())
    {
        const BWAPI::UnitType type = act.getUnitType();

        if (the.selfRace() == BWAPI::Races::Terran &&
            !_outOfBook &&
            terranOpeningRequirementsPlanned(act))
        {
            return true;
        }

        if (type == BWAPI::UnitTypes::Terran_Ghost ||
            type == BWAPI::UnitTypes::Terran_Marine ||
            type == BWAPI::UnitTypes::Terran_Medic ||
            type == BWAPI::UnitTypes::Terran_Firebat)
        {
            bool hasAnyBarracks = false;

            for (BWAPI::Unit unit : the.self()->getUnits())
            {
                if (!unit || !unit->exists())
                {
                    continue;
                }

                if (unit->getType() == BWAPI::UnitTypes::Terran_Barracks)
                {
                    hasAnyBarracks = true;
                    break;
                }
            }

            if (hasAnyBarracks && act.hasTech())
            {
                return true;
            }
        }
    }

    return act.hasPotentialProducer() && act.hasTech();
}

void ProductionManager::fixStuckCommandCenterTraining()
{
    

    const int now = the.now();

    // The bot DLL may remain loaded between matches, while unit ids and the
    // frame counter restart. Never reuse recovery state from the previous game.
    if (commandCenterRecoveryLastFrame > now)
    {
        commandCenterTrainingWatchStartFrame.clear();
        commandCenterTrainingLastRemaining.clear();
        commandCenterIgnoreUntilFrame.clear();
    }
    commandCenterRecoveryLastFrame = now;

    std::set<int> liveCommandCenterIDs;

    for (BWAPI::Unit commandCenter : the.self()->getUnits())
    {
        if (!commandCenter ||
            !commandCenter->exists() ||
            commandCenter->getType() != BWAPI::UnitTypes::Terran_Command_Center)
        {
            continue;
        }

        const int id = commandCenter->getID();
        liveCommandCenterIDs.insert(id);

        // Command Centers can only train SCVs. Avoid getTrainingQueue() here:
        // this BWAPI call has produced a reproducible access violation in the
        // Terran production path (BWAPI.dll + 0x247FD). isTraining() is enough
        // to identify an active Command Center training job.
        if (!commandCenter->isCompleted() ||
            commandCenter->isLifted() ||
            !commandCenter->isTraining())
        {
            commandCenterTrainingWatchStartFrame.erase(id);
            commandCenterTrainingLastRemaining.erase(id);
            continue;
        }

        const int remaining = commandCenter->getRemainingTrainTime();
        const bool firstSeen =
            commandCenterTrainingWatchStartFrame.find(id) ==
                commandCenterTrainingWatchStartFrame.end();

        if (firstSeen)
        {
            commandCenterTrainingWatchStartFrame[id] = now;
            commandCenterTrainingLastRemaining[id] = remaining;
            continue;
        }

        const int lastRemaining = commandCenterTrainingLastRemaining[id];

        if (remaining > 0 && remaining < lastRemaining)
        {
            commandCenterTrainingWatchStartFrame[id] = now;
            commandCenterTrainingLastRemaining[id] = remaining;
            continue;
        }

        commandCenterTrainingLastRemaining[id] = remaining;

        const int age = now - commandCenterTrainingWatchStartFrame[id];
        const bool stuckAtExit = remaining <= 0 && age >= 24 * 4;
        const bool noProgress =
            remaining >= lastRemaining && age >= 24 * 10;

        if (!stuckAtExit && !noProgress)
        {
            continue;
        }

        // Cancel the blocked first slot, replace exactly that SCV, and avoid
        // selecting this Command Center again for a short recovery window.
        if (commandCenter->cancelTrain(0))
        {
            commandCenterIgnoreUntilFrame[id] = now + 24 * 12;

            // Replace the canceled worker only if the economy still wants one.
            // Another Command Center may have completed enough SCVs while this
            // stuck job was being observed.
            if (terranSCVCountIncludingTraining() <
                usefulTerranWorkerCap())
            {
                _queue.queueAsHighestPriority(BWAPI::UnitTypes::Terran_SCV);
            }

            _lastProductionFrame = now;

            commandCenterTrainingWatchStartFrame.erase(id);
            commandCenterTrainingLastRemaining.erase(id);
        }
        else
        {
            // BWAPI can reject a command during latency. Keep the original SCV
            // and retry only after another full observation window.
            commandCenterTrainingWatchStartFrame[id] = now;
            commandCenterTrainingLastRemaining[id] = remaining;
        }
    }

    auto eraseDeadCommandCenters = [&liveCommandCenterIDs](auto& state)
    {
        for (auto it = state.begin(); it != state.end(); )
        {
            if (liveCommandCenterIDs.find(it->first) == liveCommandCenterIDs.end())
            {
                it = state.erase(it);
            }
            else
            {
                ++it;
            }
        }
    };

    eraseDeadCommandCenters(commandCenterTrainingWatchStartFrame);
    eraseDeadCommandCenters(commandCenterTrainingLastRemaining);
    eraseDeadCommandCenters(commandCenterIgnoreUntilFrame);
}

void ProductionManager::fixStuckFactoryTraining()
{
    

    const int now = the.now();

    // A DLL/process may survive between games. Unit ids can then be reused, so
    // never carry Factory recovery state across a frame-counter reset.
    if (factoryRecoveryLastFrame > now)
    {
        factoryTrainingWatchStartFrame.clear();
        factoryTrainingLastRemaining.clear();
        factoryTrainingWatchType.clear();
        factoryIgnoreUntilFrame.clear();
    }
    factoryRecoveryLastFrame = now;

    std::set<int> liveFactoryIDs;

    for (BWAPI::Unit factory : the.self()->getUnits())
    {
        if (!factory || !factory->exists())
        {
            continue;
        }

        if (factory->getType() != BWAPI::UnitTypes::Terran_Factory)
        {
            continue;
        }

        const int id = factory->getID();
        liveFactoryIDs.insert(id);

        if (!factory->isCompleted() || factory->isLifted())
        {
            factoryTrainingWatchStartFrame.erase(id);
            factoryTrainingLastRemaining.erase(id);
            factoryTrainingWatchType.erase(id);
            continue;
        }

        if (!factory->isTraining())
        {
            factoryTrainingWatchStartFrame.erase(id);
            factoryTrainingLastRemaining.erase(id);
            factoryTrainingWatchType.erase(id);
            continue;
        }

        // Do not query Unit::getTrainingQueue(). The crash logger proved that
        // BWAPI can fault inside that call even for an otherwise valid Factory.
        // UnitCommand::getUnitType() returns the trained type for a Train command
        // and None for commands which do not carry a unit type. If a later
        // command (for example a rally command) became the most recent one,
        // retain the type already observed for this Factory.
        BWAPI::UnitType trainingType = factory->getLastCommand().getUnitType();

        if (trainingType != BWAPI::UnitTypes::Terran_Vulture &&
            trainingType != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
            trainingType != BWAPI::UnitTypes::Terran_Goliath)
        {
            const auto watchedType = factoryTrainingWatchType.find(id);
            if (watchedType != factoryTrainingWatchType.end())
            {
                trainingType = watchedType->second;
            }
        }

        if (trainingType != BWAPI::UnitTypes::Terran_Vulture &&
            trainingType != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
            trainingType != BWAPI::UnitTypes::Terran_Goliath)
        {
            // We know that the Factory is training, but without a trustworthy
            // unit type it would be unsafe to cancel and invent a replacement.
            // Leave the real BW job alone and try again on a later frame.
            factoryTrainingWatchStartFrame.erase(id);
            factoryTrainingLastRemaining.erase(id);
            factoryTrainingWatchType.erase(id);
            continue;
        }

        const int remaining = factory->getRemainingTrainTime();

        const bool firstSeen =
            factoryTrainingWatchStartFrame.find(id) == factoryTrainingWatchStartFrame.end();

        const bool typeChanged =
            factoryTrainingWatchType.find(id) == factoryTrainingWatchType.end() ||
            factoryTrainingWatchType[id] != trainingType;

        if (firstSeen || typeChanged)
        {
            factoryTrainingWatchStartFrame[id] = now;
            factoryTrainingLastRemaining[id] = remaining;
            factoryTrainingWatchType[id] = trainingType;
            continue;
        }

        const int startFrame = factoryTrainingWatchStartFrame[id];
        const int lastRemaining = factoryTrainingLastRemaining[id];

        if (remaining > 0 && remaining < lastRemaining)
        {
            factoryTrainingWatchStartFrame[id] = now;
            factoryTrainingLastRemaining[id] = remaining;
            continue;
        }

        factoryTrainingLastRemaining[id] = remaining;

        const int age = now - startFrame;

        const bool stuckAtZero =
            remaining <= 0 && age >= 24 * 4;

        const bool notProgressingTooLong =
            remaining >= lastRemaining && age >= 24 * 10;

        if (!stuckAtZero && !notProgressingTooLong)
        {
            continue;
        }

        DebugQueuePrintf(
            "factory stuck training %s, id=%d, remaining=%d, age=%d -> cancel/requeue",
            trainingType.getName().c_str(),
            id,
            remaining,
            age
        );

        const bool canceled = factory->cancelTrain();
        bool stopped = false;

        if (!canceled)
        {
            stopped = factory->stop();
        }

        // Do not invent a successful recovery. If BWAPI rejected both
        // commands, keep watching the real training item and do not requeue a
        // duplicate.
        if (!canceled && !stopped)
        {
            DebugQueuePrintf(
                "factory recovery rejected, id=%d; leaving original order intact",
                id
            );

            factoryTrainingWatchStartFrame[id] = now;
            factoryTrainingLastRemaining[id] = remaining;
            continue;
        }

        factoryIgnoreUntilFrame[id] = now + 24 * 8;

        // cancelTrain() explicitly removed the paid training item, so replace
        // it once. stop() may merely free a blocked exit; in that case the
        // original unit can still complete and must not be duplicated.
        if (canceled && !_queue.anyInQueue(trainingType))
        {
            const bool staleGroundUnitAfterPureAirTransition =
                StrategyManager::Instance().pureAirPlanActive() &&
                (trainingType == BWAPI::UnitTypes::Terran_Vulture ||
                 trainingType == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode);

            if (!staleGroundUnitAfterPureAirTransition)
            {
                _queue.queueAsHighestPriority(trainingType);
            }
        }

        _lastProductionFrame = now;

        factoryTrainingWatchStartFrame.erase(id);
        factoryTrainingLastRemaining.erase(id);
        factoryTrainingWatchType.erase(id);
    }

    auto eraseDeadFactories = [&liveFactoryIDs](auto& state)
    {
        for (auto it = state.begin(); it != state.end(); )
        {
            if (liveFactoryIDs.find(it->first) == liveFactoryIDs.end())
            {
                it = state.erase(it);
            }
            else
            {
                ++it;
            }
        }
    };

    eraseDeadFactories(factoryTrainingWatchStartFrame);
    eraseDeadFactories(factoryTrainingLastRemaining);
    eraseDeadFactories(factoryTrainingWatchType);
    eraseDeadFactories(factoryIgnoreUntilFrame);
}

// Build the same temporary record that BuildingManager will ultimately
// receive. In particular, an emergency bunker with MacroLocation::Tile must
// pre-position its SCV at that exact tile, not at the generic main-base tile.
Building ProductionManager::makeBuildingForPrediction(
    const MacroAct & act,
    bool isGasSteal) const
{
    const MacroLocation location = act.getMacroLocation();
    const BWAPI::TilePosition requestedTile = act.getTileLocation();
    const BWAPI::TilePosition desiredTile =
        location == MacroLocation::Tile && requestedTile.isValid()
        ? requestedTile
        : the.placer.getMacroLocationTile(location);

    Building building(act.getUnitType(), desiredTile);
    building.macroLocation = location;
    building.isGasSteal = isGasSteal;

    if (location == MacroLocation::Tile && requestedTile.isValid())
    {
        building.finalPosition = requestedTile;
    }

    return building;
}

bool ProductionManager::buildingPredictionMatches(const MacroAct & act) const
{
    if (!act.isBuilding() ||
        _typeOfUpcomingBuilding != act.getUnitType() ||
        _locationOfUpcomingBuilding != act.getMacroLocation())
    {
        return false;
    }

    return
        act.getMacroLocation() != MacroLocation::Tile ||
        _tileOfUpcomingBuilding == act.getTileLocation();
}

void ProductionManager::clearBuildingPrediction(bool releaseWorker)
{
    if (releaseWorker &&
        _assignedWorkerForThisBuilding &&
        _assignedWorkerForThisBuilding->exists() &&
        _assignedWorkerForThisBuilding->getPlayer() == the.self())
    {
        WorkerManager::Instance().finishedWithWorker(
            _assignedWorkerForThisBuilding);
    }

    _assignedWorkerForThisBuilding = nullptr;
    _typeOfUpcomingBuilding = BWAPI::UnitTypes::None;
    _locationOfUpcomingBuilding = MacroLocation::Anywhere;
    _tileOfUpcomingBuilding = BWAPI::TilePositions::None;
    _predictedTilePosition = BWAPI::TilePositions::None;
    _haveLocationForThisBuilding = false;
    _delayBuildingPredictionUntilFrame = 0;
    _unplaceableMacroBuildingSinceFrame = -1;
}

// The late BioTank technology chain must not idle independent Barracks. Keep
// funding the next book action, but spend the surplus on Marines and Medics.
bool ProductionManager::manageBioTankOpeningProduction()
{
    if (_outOfBook || _queue.isEmpty() ||
        StrategyManager::Instance().getOpeningGroup() != "bio") return false;

    const MacroAct top = _queue.getHighestPriorityItem().macroAct;
    if (top.isCommand() || top.isSupply()) return false;
    const BWAPI::UnitType factory = BWAPI::UnitTypes::Terran_Factory;
    const BWAPI::UnitType shop = BWAPI::UnitTypes::Terran_Machine_Shop;
    const BWAPI::UnitType tank = BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode;
    const bool techPhase = the.my.all.count(factory) > 0 ||
        BuildingManager::Instance().getNumUnstarted(factory) > 0 ||
        (top.isUnit() && (top.getUnitType() == factory ||
            top.getUnitType() == shop || top.getUnitType() == tank));
    if (!techPhase || (top.isUnit() &&
        top.whatBuilds() == BWAPI::UnitTypes::Terran_Barracks)) return false;

    int reserveMinerals = top.mineralPrice();
    int reserveGas = top.gasPrice();
    bool tankPending = false;
    bool reservedTech = false;
    // Look past the add-on barrier only for reservations, never to reorder
    // technology. Once a building has started, BuildingManager owns its cost.
    for (int i = int(_queue.size()) - 1; i >= 0; --i)
    {
        const MacroAct& act = _queue[i].macroAct;
        if (act.isCommand()) break;
        if (act.isUnit() && act.getUnitType() == tank) tankPending = true;
        if (!reservedTech && ((act.isUnit() && (act.getUnitType() == factory ||
            act.getUnitType() == shop || act.getUnitType() == tank)) ||
            (act.isTech() && act.getTechType() == BWAPI::TechTypes::Tank_Siege_Mode)))
        {
            reserveMinerals = std::max(reserveMinerals, act.mineralPrice());
            reserveGas = std::max(reserveGas, act.gasPrice());
            reservedTech = true;
        }
    }
    // Add-ons can already have left the book for an unstarted ProductionGoal.
    for (const ProductionGoal& goal : _goals)
    {
        if (!goal.act.isUnit() || goal.act.getUnitType() != shop) continue;
        reserveMinerals = std::max(reserveMinerals, goal.act.mineralPrice());
        reserveGas = std::max(reserveGas, goal.act.gasPrice());
    }

    // The all snapshot already includes units being trained in Barracks.
    const int marines = the.my.all.count(BWAPI::UnitTypes::Terran_Marine);
    const int medics = the.my.all.count(BWAPI::UnitTypes::Terran_Medic);
    const bool needMedic =
        the.my.completed.count(BWAPI::UnitTypes::Terran_Academy) > 0 &&
        marines >= 5 && medics < BioProductionPolicy::medicTarget(marines,
            the.my.all.count(BWAPI::UnitTypes::Terran_Firebat),
            the.my.all.count(BWAPI::UnitTypes::Terran_Ghost));
    const MacroAct medic(BWAPI::UnitTypes::Terran_Medic);
    const bool medicAffordable =
        getFreeMinerals() >= reserveMinerals + medic.mineralPrice() &&
        getFreeGas() >= reserveGas + medic.gasPrice();
    const MacroAct infantry(needMedic && medicAffordable ? BWAPI::UnitTypes::Terran_Medic
        : BWAPI::UnitTypes::Terran_Marine);

    const int freeSupply = the.self()->supplyTotal() - the.self()->supplyUsed();
    // Leave room for the first Tank as well as the supplementary infantry.
    const int tankSupply = tankPending ? tank.supplyRequired() : 0;
    if (freeSupply < infantry.supplyRequired() + tankSupply)
    {
        const BWAPI::UnitType depot = BWAPI::UnitTypes::Terran_Supply_Depot;
        if (!BuildingManager::Instance().isBeingBuilt(depot) &&
            the.self()->supplyTotal() < 400)
        {
            for (int i = int(_queue.size()) - 1; i >= 0; --i)
            {
                if (_queue[i].macroAct.isUnit() &&
                    _queue[i].macroAct.getUnitType() == depot)
                {
                    _queue.pullToTop(size_t(i));
                    return true;
                }
            }
            _queue.queueAsHighestPriority(depot);
            setTerranRuleDecision("BOOK BIO: supply for infantry and first Tank");
            return true;
        }
        return false;
    }
    if (getFreeMinerals() < reserveMinerals + infantry.mineralPrice() ||
        (infantry.gasPrice() > 0 &&
         getFreeGas() < reserveGas + infantry.gasPrice())) return false;
    BWAPI::Unit producer = getProducer(infantry);
    if (!producer || !canMakeNow(producer, infantry) ||
        !create(producer, BuildOrderItem(infantry))) return false;

    // If this same infantry was already scheduled before the next book
    // command, consume its entry rather than producing it twice.
    for (int i = int(_queue.size()) - 1; i >= 0; --i)
    {
        const MacroAct& act = _queue[i].macroAct;
        if (act.isCommand()) break;
        if (act.isUnit() && act.getUnitType() == infantry.getUnitType())
        {
            _queue.removeAt(size_t(i));
            break;
        }
    }
    _lastProductionFrame = the.now();
    setTerranRuleDecision("BOOK BIO: MAKE " + NiceMacroActName(infantry.getName()) +
        " beside Tank technology");
    return true;
}

void ProductionManager::requestOpeningDefensiveMarines(int target)
{
    if (!_outOfBook && the.selfRace() == BWAPI::Races::Terran &&
        !terranOpeningScoutPending())
    {
        _openingDefensiveMarineTarget = std::max(_openingDefensiveMarineTarget, target);
    }
}

bool ProductionManager::manageOpeningDefensiveMarines(bool immediateBaseAttack)
{
    if (_outOfBook || _openingDefensiveMarineTarget <= 0 ||
        the.selfRace() != BWAPI::Races::Terran || terranOpeningScoutPending())
        return false;

    const BWAPI::UnitType marineType = BWAPI::UnitTypes::Terran_Marine;
    // PlayerSnapshot::all includes the active training slot. Count accepted
    // orders now, not survivors at the end of a four-Marine replacement batch.
    const int remaining = std::max(0, _openingDefensiveMarineTarget -
        the.my.all.count(marineType));
    if (remaining == 0) return false;

    const MacroAct marine(marineType);
    BWAPI::Unit barracks = getProducer(marine);
    if (!barracks || !canMakeNow(barracks, marine)) return false;

    // Keep the next real book investment funded while replacing one loss.
    // Existing Marine entries remain one-shot orders and are consumed by
    // manageBuildOrderQueue() when BWAPI accepts them, regardless of later deaths.
    int reserveMinerals = 0, reserveGas = 0;
    if (!immediateBaseAttack)
    {
        for (int i = int(_queue.size()) - 1; i >= 0; --i)
        {
            const MacroAct act = _queue[i].macroAct;
            if (act.isCommand()) break;
            if (act.isUnit() && !act.isBuilding() && act.whatBuilds() ==
                BWAPI::UnitTypes::Terran_Barracks) continue;
            reserveMinerals = act.mineralPrice();
            reserveGas = act.gasPrice();
            break;
        }
    }
    if (getFreeMinerals() < reserveMinerals + marine.mineralPrice() ||
        getFreeGas() < reserveGas) return false;
    if (!create(barracks, BuildOrderItem(marine))) return false;

    if (immediateBaseAttack)
    {
        // This Marine fulfils one existing opening order if there is one.
        for (int i = int(_queue.size()) - 1; i >= 0; --i)
        {
            const MacroAct& act = _queue[i].macroAct;
            if (act.isCommand()) break;
            if (act.isUnit() && act.getUnitType() == marineType)
            {
                _queue.removeAt(size_t(i));
                break;
            }
        }
    }

    _lastProductionFrame = the.now();
    setTerranRuleDecision(std::string(immediateBaseAttack ?
        "BASE DEFENSE: MAKE 1 Marine, remaining " :
        "DEFENSE: MAKE 1 Marine, remaining ") +
        std::to_string(remaining - 1) + " (parallel to book)");
    return true;
}

bool ProductionManager::manageAirTanksOpeningVultures()
{
    const AirTanks::Variant variant = AirTanks::variantFromName(
        StrategyManager::Instance().airTanksVariant());
    if (_outOfBook || !StrategyManager::Instance().airTanksPlanActive() ||
        (variant != AirTanks::Variant::TankWraith &&
         variant != AirTanks::Variant::TwoPortWraith) ||
        _queue.isEmpty() || the.self()->gas() >= 200 ||
        the.self()->minerals() < 700) return false;

    const MacroAct& next = _queue.getHighestPriorityItem().macroAct;
    if (next.isCommand() || next.isSupply()) return false;
    const int tanks = the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
        the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
    if (variant == AirTanks::Variant::TwoPortWraith &&
        tanks < AirTanks::FirstPushTanks) return false;
    const int vultures = the.my.all.count(BWAPI::UnitTypes::Terran_Vulture);
    const int target = AirTanks::tankWraithVultureTarget(
        tanks, the.self()->minerals(), the.self()->gas());
    if (vultures >= target) return false;

    const MacroAct vulture(BWAPI::UnitTypes::Terran_Vulture);
    const int reserveMinerals = next.mineralPrice();
    if (getFreeMinerals() < reserveMinerals + vulture.mineralPrice()) return false;
    BWAPI::Unit factory = getProducer(vulture);
    if (!factory || !canMakeNow(factory, vulture) ||
        !create(factory, BuildOrderItem(vulture))) return false;

    _lastProductionFrame = the.now();
    setTerranRuleDecision("BOOK AIR: MAKE Vulture from mineral surplus " +
        std::to_string(vultures + 1) + "/" + std::to_string(target));
    return true;
}

// If the next JSON entry cannot start, try up to three immediately following
// entries. A producer-independent building or research can run beside it, but
// commands, supply and add-ons keep their strict opening-book semantics.
bool ProductionManager::manageTerranOpeningGoals()
{
    if (the.selfRace() != BWAPI::Races::Terran ||
        _outOfBook ||
        _queue.isEmpty())
    {
        return false;
    }

    const MacroAct top = _queue.getHighestPriorityItem().macroAct;
    const std::string topName = NiceMacroActName(top.getName());

    if (manageBioTankOpeningProduction()) return true;

    // The JSON book lists only a few SCV batches. Its Factory/tech phase can
    // otherwise leave the Command Center idle for minutes, so keep filling the
    // current mineral line while the next ordered item is waiting. Do not
    // steal the last 50 minerals from an imminent building or expansion.
    const bool emergencyCommandCenter =
        top.isUnit() &&
        top.getUnitType() == BWAPI::UnitTypes::Terran_Command_Center &&
        StrategyManager::Instance().isTerranMineralExpansionEmergency();
    if (!emergencyCommandCenter &&
        !top.isCommand() && !top.isSupply() &&
        !(top.isUnit() &&
          top.getUnitType() == BWAPI::UnitTypes::Terran_SCV) &&
        !(terranWallOpeningTankCorePending() && top.isUnit() &&
          (top.getUnitType() == BWAPI::UnitTypes::Terran_Command_Center ||
           top.getUnitType() == BWAPI::UnitTypes::Terran_Vulture)) &&
        !terranSCVCapReached())
    {
        const int completedBases =
            the.my.completed.count(BWAPI::UnitTypes::Terran_Command_Center);
        const int openingWorkerTarget = std::min(
            usefulTerranWorkerCap(),
            completedBases >= 2 ? 42 : 24);
        const MacroAct scv(BWAPI::UnitTypes::Terran_SCV);
        const int freeMinerals = getFreeMinerals();
        const bool topBuildingNearlyFunded =
            top.isBuilding() &&
            getFreeGas() >= top.gasPrice() &&
            freeMinerals >= top.mineralPrice() - scv.mineralPrice();

        if (terranSCVCountIncludingTraining() < openingWorkerTarget &&
            !topBuildingNearlyFunded &&
            freeMinerals >= scv.mineralPrice() &&
            the.self()->supplyTotal() - the.self()->supplyUsed() >=
                scv.supplyRequired())
        {
            BWAPI::Unit commandCenter = getProducer(scv);
            if (commandCenter && canMakeNow(commandCenter, scv) &&
                create(commandCenter, BuildOrderItem(scv)))
            {
                _lastProductionFrame = the.now();
                setTerranRuleDecision(
                    "BOOK ECONOMY: make SCV while " + topName + " waits");
                return true;
            }
        }
    }

    // manageBuildOrderQueue() owns the four-Tank wall core. While it is
    // holding an early Vulture or natural, do not bypass that hold by pulling
    // another JSON unit forward in this parallel-opening helper.
    if (!emergencyCommandCenter &&
        terranWallOpeningTankCorePending() &&
        top.isUnit() &&
        (top.getUnitType() == BWAPI::UnitTypes::Terran_Command_Center ||
         top.getUnitType() == BWAPI::UnitTypes::Terran_Vulture))
    {
        setTerranRuleDecision(
            "WALL CORE: parallel JSON paused until 4 Tanks");
        return true;
    }

    // Commands have no producer, and add-ons already use ProductionGoal.
    // Never pass either kind through getProducer().
    if (top.isCommand() || top.isAddon())
    {
        return false;
    }

    // Supply is deliberately strict. Moving army production in front of it
    // only creates a larger supply block and hides the real opening problem.
    if (top.isSupply())
    {
        setTerranRuleDecision(
            "BOOK PRIORITY: finish Supply Depot before later goals");
        return false;
    }

    const bool topNeedsWorker =
        top.isBuilding() &&
        UnitUtil::NeedsWorkerBuildingType(top.getUnitType());

    // manageBuildOrderQueue() already owns worker selection and prediction for
    // the top building earlier in this frame. Calling getProducer() again here
    // repeated the complete SCV search while BOOK SAVE waited for a bunker.
    BWAPI::Unit topProducer = nullptr;
    if (!topNeedsWorker || top.isGasSteal() ||
        _assignedWorkerForThisBuilding)
    {
        topProducer = getProducer(top);
    }

    if (!topProducer && !topNeedsWorker)
    {
        std::string machineShopDecision;
        const ResearchProducerPreparation preparation =
            prepareMachineShopForResearch(
                top,
                machineShopDecision);

        if (preparation != ResearchProducerPreparation::NotApplicable)
        {
            setTerranRuleDecision(
                "BOOK " + machineShopDecision);
            return preparation == ResearchProducerPreparation::Scheduled;
        }
    }

    if (_queue.size() < 2)
    {
        return false;
    }

    if (topProducer && canMakeNow(topProducer, top))
    {
        // The normal queue owns this action. It should normally have started
        // earlier in the same frame; never duplicate it here.
        return false;
    }

    const int freeMinerals = getFreeMinerals();
    const int freeGas = getFreeGas();
    // Keep an imminent structure or long research funded. A blocked trainable
    // unit has no dedicated worker and should not idle another producer.
    const bool reserveTop = topNeedsWorker || top.isTech() || top.isUpgrade();
    const int reserveMinerals = reserveTop ? top.mineralPrice() : 0;
    const int reserveGas = reserveTop ? top.gasPrice() : 0;
    const bool wallCorePending = terranWallOpeningTankCorePending();

    // A busy Barracks may have several one-shot Marines in the book. Look
    // past that batch for an independent producer, keeping all phase barriers.
    const bool marineBatch = top.isUnit() &&
        top.getUnitType() == BWAPI::UnitTypes::Terran_Marine;
    const int lastIndex = std::max(0, int(_queue.size()) - (marineBatch ? 12 : 4));
    for (int i = int(_queue.size()) - 2; i >= lastIndex; --i)
    {
        const BuildOrderItem item = _queue[i];
        const MacroAct act = item.macroAct;

        // Do not cross an opening phase command, a supply requirement, or an
        // add-on whose parent Factory/Barracks can be reserved by a goal.
        if (act.isCommand() ||
            act.isSupply() ||
            act.isAddon())
        {
            break;
        }

        if (!act.isUnit() && !act.isTech() && !act.isUpgrade())
        {
            continue;
        }

        if (wallCorePending && act.isUnit() &&
            (act.getUnitType() == BWAPI::UnitTypes::Terran_Command_Center ||
             act.getUnitType() == BWAPI::UnitTypes::Terran_Vulture))
        {
            continue;
        }

        // getProducer() would otherwise reuse the worker assigned to the
        // first building and steal its predicted construction location.
        if (act.isBuilding() && _assignedWorkerForThisBuilding)
        {
            continue;
        }

        if (act.mineralPrice() + reserveMinerals > freeMinerals ||
            act.gasPrice() + reserveGas > freeGas)
        {
            continue;
        }

        BWAPI::Unit producer = getProducer(act);

        if (!producer || !canMakeNow(producer, act))
        {
            continue;
        }

        if (!create(producer, item))
        {
            continue;
        }
        _queue.removeAt(size_t(i));
        _lastProductionFrame = the.now();

        setTerranRuleDecision(
            "BOOK LOOKAHEAD: MAKE " + NiceMacroActName(act.getName()) +
            " while " + topName + " waits");
        return true;
    }

    if (top.mineralPrice() > freeMinerals || top.gasPrice() > freeGas)
    {
        setTerranRuleDecision(
            "BOOK SAVE " + std::to_string(top.mineralPrice()) +
            "M/" + std::to_string(top.gasPrice()) +
            "G for " + topName);
    }
    else if (top.isUnit() &&
        top.supplyRequired() >
            the.self()->supplyTotal() - the.self()->supplyUsed() &&
        the.self()->supplyTotal() < 400)
    {
        const BWAPI::UnitType depot =
            BWAPI::UnitTypes::Terran_Supply_Depot;
        const bool depotPending =
            BuildingManager::Instance().isBeingBuilt(depot) ||
            _queue.anyInQueue(depot);
        setTerranRuleDecision(
            std::string("BOOK WAIT Supply Depot ") +
            (depotPending ? "(pending) " : "(missing) ") +
            "before " + topName);
    }
    else if (!terranOpeningRequirementsPlanned(top))
    {
        setTerranRuleDecision(
            "BOOK WAIT: missing prerequisite for " + topName);
    }
    else if (topNeedsWorker && !_predictedTilePosition.isValid())
    {
        setTerranRuleDecision(
            "BOOK WAIT build location for " + topName);
    }
    else if (topNeedsWorker && _assignedWorkerForThisBuilding)
    {
        setTerranRuleDecision(
            "BOOK WAIT builder traveling for " + topName);
    }
    else if (!topProducer)
    {
        setTerranRuleDecision(
            "BOOK WAIT select/route " + NiceMacroActName(top.whatBuilds().getName()) +
            " for " + topName);
    }
    else
    {
        setTerranRuleDecision(
            "BOOK WAIT: " + topName + " is not legal this frame");
    }

    return false;
}

void ProductionManager::manageBuildOrderQueue()
{
    // If the extractor trick is in progress, do that.
    

    // If we were planning to build and assigned a worker, but the queue was then
    // changed behind our back: Release the worker and continue.
    if (_queue.isModified() &&
        _assignedWorkerForThisBuilding &&
        (_queue.isEmpty() ||
         !buildingPredictionMatches(
             _queue.getHighestPriorityItem().macroAct)))
    {
        clearBuildingPrediction(true);
    }

    // We do nothing if the queue is empty (obviously).
    while (!_queue.isEmpty()) 
    {
        if (_outOfBook && _queue.getHighestPriorityItem().macroAct.isUnit() &&
            _queue.getHighestPriorityItem().macroAct.getUnitType() == BWAPI::UnitTypes::Terran_Marine &&
            BioTankPolicy::reserveGoliathSupply())
        {
            setTerranRuleDecision("WAIT supply for BioTank anti-air counter");
            break;
        }
        // A bank-filler or urgent Marine can occupy the next free Barracks
        // ahead of the persistent Medic rule. Repair that queue ordering when
        // BioTank is short of its one-Medic-per-five-Marines screen.
        if (_outOfBook &&
            StrategyManager::Instance().bioTankPlanActive() &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Academy) > 0 &&
            the.my.all.count(BWAPI::UnitTypes::Terran_Marine) >= 10 &&
            _queue.getHighestPriorityItem().macroAct.isUnit() &&
            _queue.getHighestPriorityItem().macroAct.getUnitType() ==
                BWAPI::UnitTypes::Terran_Marine)
        {
            const int marines =
                the.my.all.count(BWAPI::UnitTypes::Terran_Marine) +
                int(_queue.numInQueue(BWAPI::UnitTypes::Terran_Marine));
            const int medics =
                the.my.all.count(BWAPI::UnitTypes::Terran_Medic) +
                int(_queue.numInQueue(BWAPI::UnitTypes::Terran_Medic));
            if (medics < BioProductionPolicy::medicTarget(marines,
                    the.my.all.count(BWAPI::UnitTypes::Terran_Firebat),
                    the.my.all.count(BWAPI::UnitTypes::Terran_Ghost)) &&
                the.self()->minerals() >= BWAPI::UnitTypes::Terran_Medic.mineralPrice() &&
                the.self()->gas() >= BWAPI::UnitTypes::Terran_Medic.gasPrice())
            {
                int queuedMedic = -1;
                for (int i = int(_queue.size()) - 1; i >= 0; --i)
                {
                    const MacroAct& act = _queue[size_t(i)].macroAct;
                    if (act.isUnit() &&
                        act.getUnitType() == BWAPI::UnitTypes::Terran_Medic)
                    {
                        queuedMedic = i;
                        break;
                    }
                }
                if (queuedMedic >= 0)
                    _queue.pullToTop(queuedMedic);
                else
                    _queue.queueAsHighestPriority(
                        BWAPI::UnitTypes::Terran_Medic);
            }
        }

        // We may be able to produce faster if we pull a later item to the front.
        maybeReorderQueue();

        const BuildOrderItem & currentItem = _queue.getHighestPriorityItem();

        // With a verified completed P/Z main wall, the opening defense is the
        // wall + 4 Marines in its Bunker + a four-Tank core. Do not let an
        // early Vulture or the natural consume the Factory/mineral window
        // before those Tanks exist. This does NOT inject Tank items into the
        // queue, so JSON/urgent orders cannot alternate Tank/Vulture every
        // frame; it simply uses a free Factory directly and leaves the current
        // book item in place until the core is ready.
        if (terranWallOpeningTankCorePending() &&
            currentItem.macroAct.isUnit() &&
            (currentItem.macroAct.getUnitType() ==
                 BWAPI::UnitTypes::Terran_Command_Center ||
             currentItem.macroAct.getUnitType() ==
                 BWAPI::UnitTypes::Terran_Vulture) &&
            !(currentItem.macroAct.getUnitType() ==
                  BWAPI::UnitTypes::Terran_Command_Center &&
              StrategyManager::Instance().isTerranMineralExpansionEmergency()))
        {
            const MacroAct tank(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode);
            BWAPI::Unit producer = getProducer(tank);

            if (producer && canMakeNow(producer, tank) &&
                create(producer, BuildOrderItem(tank)))
            {
                _lastProductionFrame = the.now();
                setTerranRuleDecision(
                    "WALL CORE: MAKE Tank before Vulture/natural");
            }
            else
            {
                setTerranRuleDecision(
                    "WALL CORE: save Factory/resources for 4 Tanks before natural");
            }
            return;
        }

        // Final queue-level safety for every SCV source: JSON, urgent base
        // bootstrap, jam recovery and persistent strategic rules. The urgent
        // base lane previously bypassed the normal target and could order one
        // SCV per second forever after a mined-out base's workers transferred
        // away. Drop capped requests and continue to the next army/structure
        // item in the same frame.
        if (currentItem.macroAct.isUnit() &&
            currentItem.macroAct.getUnitType() ==
                BWAPI::UnitTypes::Terran_SCV &&
            terranSCVCapReached())
        {
            setTerranRuleDecision(
                "SKIP SCV: worker cap " +
                std::to_string(usefulTerranWorkerCap()) +
                " reached");
            _queue.doneWithHighestPriorityItem();
            continue;
        }

        // WORKAROUND for BOSS bug of making too many gateways: Limit the count.
        // Idea borrowed from Locutus by Bruce Nielsen.
        

        // Legacy Steamhammer limited Terran to one Starport as a BOSS
        // workaround. That silently disables legitimate 2-port Wraith/drop
        // plans. Keep only a generous malformed-plan safety cap; normal
        // StrategyManager targets remain the real source of the desired count.
        if (currentItem.macroAct.isUnit() &&
            currentItem.macroAct.getUnitType() == BWAPI::UnitTypes::Terran_Starport)
        {
            const int ownedBases =
                std::max(1, the.bases.baseCount(the.self()));

            const int starportHardCap =
                std::min(6, std::max(2, 2 * ownedBases));

            if (the.my.all.count(BWAPI::UnitTypes::Terran_Starport) >=
                starportHardCap)
            {
                _queue.doneWithHighestPriorityItem();
                _lastProductionFrame = the.now();
                continue;
            }
        }

        // If this is a command, execute it and keep going.
        if (currentItem.macroAct.isCommand())
        {
            if (the.selfRace() == BWAPI::Races::Terran && !_outOfBook)
            {
                setTerranRuleDecision(
                    "BOOK COMMAND: " +
                    NiceMacroActName(currentItem.macroAct.getName()));
            }

            executeCommand(currentItem.macroAct);
            _queue.doneWithHighestPriorityItem();
            _lastProductionFrame = the.now();
            continue;
        }

        // Repair missing prerequisites before predicting a builder or moving
        // an add-on into goals. A lost Barracks must not leave a Bunker at the
        // head of the book waiting forever. Reuse a queued prerequisite and
        // let existing construction/goals finish without creating duplicates.
        if (the.selfRace() == BWAPI::Races::Terran &&
            the.my.all.count(BWAPI::UnitTypes::Terran_SCV) > 0)
        {
            BWAPI::UnitType prerequisite =
                getMissingCompletedTerranPrerequisite(currentItem.macroAct);
            auto prerequisiteCommitted = [&](BWAPI::UnitType type)
            {
                return the.my.all.count(type) > 0 ||
                    BuildingManager::Instance().getNumUnstarted(type) > 0 ||
                    getPendingGoalCount(type) > 0;
            };
            const bool missingPrerequisite =
                prerequisite != BWAPI::UnitTypes::None &&
                prerequisite != BWAPI::UnitTypes::Unknown;
            const int neededGas = missingPrerequisite
                ? prerequisite.gasPrice() : currentItem.macroAct.gasPrice();
            if ((!missingPrerequisite || !prerequisiteCommitted(prerequisite)) &&
                neededGas > the.self()->gas())
            {
                WorkerManager::Instance().setCollectGas(true);
                if (!prerequisiteCommitted(BWAPI::UnitTypes::Terran_Refinery))
                    prerequisite = BWAPI::UnitTypes::Terran_Refinery;
            }
            if (prerequisite != BWAPI::UnitTypes::None &&
                prerequisite != BWAPI::UnitTypes::Unknown &&
                prerequisite != BWAPI::UnitTypes::Terran_SCV &&
                prerequisite != BWAPI::UnitTypes::Terran_Command_Center &&
                !prerequisiteCommitted(prerequisite))
            {
                const std::string blockedName = currentItem.macroAct.getName();
                int queuedPrerequisite = -1;
                for (int i = int(_queue.size()) - 2; i >= 0; --i)
                {
                    const MacroAct& act = _queue[size_t(i)].macroAct;
                    if (act.isUnit() && act.getUnitType() == prerequisite)
                    {
                        queuedPrerequisite = i;
                        break;
                    }
                }
                if (queuedPrerequisite >= 0)
                    _queue.pullToTop(size_t(queuedPrerequisite));
                else
                    _queue.queueAsHighestPriority(MacroAct(prerequisite));
                clearBuildingPrediction(true);
                invalidateTerranRulePlan("restore prerequisite for " + blockedName);
                setTerranRuleDecision("RESTORE " + NiceMacroActName(prerequisite.getName()) +
                    " before " + NiceMacroActName(blockedName));
                break;
            }
        }

        // Turn some items into production goals.
        // if (currentItem.macroAct.isAddon() || currentItem.macroAct.isUpgrade() || currentItem.macroAct.isTech())
        if (currentItem.macroAct.isAddon())
        {
            const BWAPI::UnitType addonType = currentItem.macroAct.getUnitType();
            const BWAPI::UnitType parentType = currentItem.macroAct.whatBuilds();
            size_t availableParents =
                BuildingManager::Instance().getNumUnstarted(parentType) +
                _queue.numInQueue(parentType);
            for (BWAPI::Unit parent : the.self()->getUnits())
            {
                if (parent && parent->exists() && parent->getType() == parentType &&
                    !parent->getAddon() && !parent->isLifted())
                {
                    ++availableParents;
                }
            }
            if (getPendingGoalCount(addonType) >= availableParents)
            {
                // Moving the same emergency add-on into _goals every frame
                // used to grow that list and masquerade as production progress.
                _queue.doneWithHighestPriorityItem();
                continue;
            }

            if (the.selfRace() == BWAPI::Races::Terran && !_outOfBook)
            {
                setTerranRuleDecision(
                    "BOOK GOAL: " +
                    NiceMacroActName(currentItem.macroAct.getName()));
            }

            _goals.push_front(ProductionGoal(currentItem.macroAct));
            _queue.doneWithHighestPriorityItem();
            continue;
        }

        const bool workerBuiltBuilding =
            currentItem.macroAct.isBuilding() &&
            UnitUtil::NeedsWorkerBuildingType(
                currentItem.macroAct.getUnitType());

        if (workerBuiltBuilding &&
            !buildingPredictionMatches(currentItem.macroAct))
        {
            // An urgent bunker can replace another planned structure without
            // changing unit type later (for example bunker #1 -> bunker #2).
            // Never reuse the old tile or its posted SCV for the new request.
            clearBuildingPrediction(true);
            _typeOfUpcomingBuilding = currentItem.macroAct.getUnitType();
            _locationOfUpcomingBuilding =
                currentItem.macroAct.getMacroLocation();
            _tileOfUpcomingBuilding =
                _locationOfUpcomingBuilding == MacroLocation::Tile
                ? currentItem.macroAct.getTileLocation()
                : BWAPI::TilePositions::None;
        }

        const bool buildingTypeStalled =
            workerBuiltBuilding &&
            BuildingManager::Instance().typeIsStalled(
                currentItem.macroAct.getUnitType());

        // predictWorkerMovement() is the single owner of initial SCV
        // selection. Previously getProducer() selected the same worker first,
        // then prediction repeated the complete worker scan in the same frame.
        const bool waitForPredictiveBuilder =
            workerBuiltBuilding &&
            !currentItem.isGasSteal &&
            !_assignedWorkerForThisBuilding &&
            !buildingTypeStalled;

        // The unit which can produce the currentItem. May be null.
        BWAPI::Unit producer = waitForPredictiveBuilder
            ? nullptr
            : getProducer(currentItem.macroAct);

        if (!producer)
        {
            std::string machineShopDecision;
            const ResearchProducerPreparation preparation =
                prepareMachineShopForResearch(
                    currentItem.macroAct,
                    machineShopDecision);

            if (preparation != ResearchProducerPreparation::NotApplicable)
            {
                setTerranRuleDecision(
                    std::string(_outOfBook ? "QUEUE " : "BOOK ") +
                    machineShopDecision);
            }
        }
        
        // check to see if we can make it right now
        bool canMake = producer && canMakeNow(producer, currentItem.macroAct);

        // if the next item in the list is a building and we can't yet make it
        if (!canMake &&
            nextIsBuilding() &&
            the.now() >= _delayBuildingPredictionUntilFrame &&
            !buildingTypeStalled)
        {
            // construct a temporary building object
            Building b = makeBuildingForPrediction(
                currentItem.macroAct,
                currentItem.isGasSteal);

            // predict the worker movement to that building location
            // NOTE If the worker is set moving, this sets flag _movingToThisBuildingLocation = true
            //      so that we don't 
            predictWorkerMovement(b);
            break;
        }

        if (canMake && !create(producer, currentItem))
        {
            setTerranRuleDecision("RETRY: " +
                NiceMacroActName(currentItem.macroAct.getName()) +
                " (" + BWAPI::Broodwar->getLastError().getName() + ")");
            // Retain the item and still reach the jam watchdog below.
            canMake = false;
        }

        // The command was accepted; only now consume the queue entry.
        if (canMake)
        {
            
            {
                setTerranRuleDecision(
                    std::string(_outOfBook ? "QUEUE " : "BOOK ") +
                    (currentItem.macroAct.isBuilding() ? "BUILD " : "MAKE ") +
                    NiceMacroActName(currentItem.macroAct.getName()) +
                    " (ordered next)");
            }

            // BuildingManager now owns the worker supplied to create().
            clearBuildingPrediction(false);

            // and remove it from the _queue
            _queue.doneWithHighestPriorityItem();
            _lastProductionFrame = the.now();

            // don't actually loop around in here
            // TODO because we don't keep track of resources used,
            //      we wait until the next frame to build the next thing.
            //      Can cause delays, especially in late game!
            break;
        }

        // We didn't make anything. Check for a possible production jam.
        // Jams can happen due to bugs, or due to losing prerequisites for planned items.
        // If the queue is empty, we didn't intend to make anything--likely we can't.
        if (!_queue.isEmpty() &&
            the.now() > _lastProductionFrame + Config::Macro::ProductionJamFrameLimit &&
            (the.self()->minerals() >= 100 || the.bases.remainingMineralPatchCount() > 0))
        {
             if (the.self()->supplyUsed() > 400 - 12)
            {
                // We can't produce most likely because we're maxed. Great news!
            }
            else
            {
                // A single stalled item must never erase every independent
                // macro lane. Terran JSON gets an extended dependency grace
                // period and, if an item is genuinely impossible, skips only
                // that item while remaining in the opening book.
                if (the.selfRace() == BWAPI::Races::Terran && _outOfBook)
                {
                    const std::string stalledName =
                        _queue.getHighestPriorityItem().macroAct.getName();

                    _queue.removeHighestPriorityItem();
                    invalidateTerranRulePlan(
                        "stalled queue item: " + stalledName);
                    _lastProductionFrame = the.now();

                    setTerranRuleDecision(
                        "DROP STALLED: " + NiceMacroActName(stalledName));
                }
                else if (the.selfRace() == BWAPI::Races::Terran && !_outOfBook)
                {
                    const MacroAct stalledAct =
                        _queue.getHighestPriorityItem().macroAct;

                    const int stalledFrames =
                        the.now() - _lastProductionFrame;

                    const int dependencyGraceFrames =
                        std::max(
                            Config::Macro::ProductionJamFrameLimit * 4,
                            60 * 24);

                    // terranOpeningRequirementsPlanned() includes buildings
                    // under construction and add-ons already moved into
                    // _goals. They are active opening progress even though the
                    // top queue item cannot start on this exact frame.
                    if (terranOpeningRequirementsPlanned(stalledAct) &&
                        stalledFrames < dependencyGraceFrames)
                    {
                        setTerranRuleDecision(
                            "BOOK WAIT ACTIVE: " +
                            NiceMacroActName(stalledAct.getName()));
                    }
                    else
                    {
                        const std::string stalledName = stalledAct.getName();

                        _queue.removeHighestPriorityItem();
                        _lastProductionFrame = the.now();

                        setTerranRuleDecision(
                            "BOOK DROP ONE STALLED: " +
                            NiceMacroActName(stalledName));

                    DebugQueuePrintf(
                        "book: skipped one stalled item %s; %d items remain",
                        stalledName.c_str(),
                        int(_queue.size()));
                    }
                }
                else
                {
                    if (Config::Debug::DrawQueueFixInfo)
                    {
                        DebugQueuePrintf("queue: production jam timed out");
                    }
                    goOutOfBookAndClearQueue();
                }
            }
        }

        // TODO not much of a loop, eh? breaks on all branches
        //      only commands and bug workarounds continue to the next item
        break;
    }
}

// If we can't immediately produce the top item in the queue but we can produce a
// later item, we may want to move the later item to the front.
void ProductionManager::maybeReorderQueue()
{
    if (_queue.size() < 2)
    {
        return;
    }

    // If we're in a severe emergency situation, don't try to reorder the queue.
    // We need a resource depot and a few workers.
    if (the.bases.baseCount(the.self()) == 0 ||
        WorkerManager::Instance().getNumMineralWorkers() <= 3)
    {
        return;
    }

    MacroAct top = _queue.getHighestPriorityItem().macroAct;

    // Don't move anything in front of a command.
    if (top.isCommand())
    {
        return;
    }

    // If next up is supply, don't reorder it.
    // Supply is usually made automatically. If we move something above supply, code below
    // will sometimes have to check whether we have supply to make a unit.
    if (top.isUnit() && top.getUnitType() == the.selfRace().getSupplyProvider())
    {
        return;
    }

    // Terran JSON openings use manageTerranOpeningGoals() below the normal
    // queue execution. Its phase barriers are stricter and its decisions are
    // visible on screen, so do not let the older four-item heuristic reorder
    // the book first.
    if (the.selfRace() == BWAPI::Races::Terran && !_outOfBook)
    {
        return;
    }

    int minerals = getFreeMinerals();
    int gas = getFreeGas();

    // PureAir uses one Barracks for Ghosts and several Factories for Goliaths.
    // A second queued Ghost cannot start while that Barracks is training, but
    // it must not serialize the independent Factory production behind it.
    // This targeted bypass consumes only the resources of the Goliath which
    // can actually start now; reserving the blocked Ghost's cost as well was
    // the reason otherwise idle Factories stopped producing.
    if (StrategyManager::Instance().pureAirPlanActive() &&
        top.isUnit() &&
        top.getUnitType() == BWAPI::UnitTypes::Terran_Ghost)
    {
        BWAPI::Unit ghostProducer = getProducer(top);
        const bool ghostBlocked =
            !ghostProducer || !canMakeNow(ghostProducer, top);

        if (ghostBlocked)
        {
            for (int i = int(_queue.size()) - 2; i >= 0; --i)
            {
                const MacroAct& act = _queue[i].macroAct;

                // Do not jump a Factory unit over structural, add-on or
                // research priorities. This bypass only parallelizes the
                // contiguous combat-unit part of the queue.
                if (act.isCommand() || !act.isUnit())
                {
                    break;
                }

                if (act.getUnitType() != BWAPI::UnitTypes::Terran_Goliath ||
                    act.mineralPrice() > minerals ||
                    act.gasPrice() > gas)
                {
                    continue;
                }

                BWAPI::Unit factory = getProducer(act);

                if (factory && canMakeNow(factory, act))
                {
                    if (Config::Debug::DrawQueueFixInfo)
                    {
                        DebugQueuePrintf(
                            "queue: run Goliath beside busy Ghost @ %d",
                            int(_queue.size()) - i
                        );
                    }

                    _queue.pullToTop(size_t(i));
                    return;
                }
            }
        }
    }

    // Research uses a building for a long time. If the top research item has
    // the resources but its producer or prerequisite is temporarily occupied,
    // let any executable item before the next command barrier run. As soon as
    // the research can start, it remains highest priority and starts normally.
    if (top.isUpgrade() || top.isTech())
    {
        BWAPI::Unit researchProducer = getProducer(top);
        const bool hasResearchResources =
            top.mineralPrice() <= minerals &&
            top.gasPrice() <= gas;

        const bool noFreeResearchSlot =
            !researchProducer && top.hasEventualProducer();

        const bool blockedDespiteHavingResources =
            hasResearchResources &&
            (!researchProducer || !canMakeNow(researchProducer, top));

        if (noFreeResearchSlot || blockedDespiteHavingResources)
        {
            for (int i = int(_queue.size()) - 2; i >= 0; --i)
            {
                const MacroAct & act = _queue[i].macroAct;

                if (act.isCommand())
                {
                    break;
                }

                if (act.mineralPrice() > minerals || act.gasPrice() > gas)
                {
                    continue;
                }

                BWAPI::Unit producer = getProducer(act);

                if (producer && canMakeNow(producer, act))
                {
                    if (Config::Debug::DrawQueueFixInfo)
                    {
                        DebugQueuePrintf(
                            "queue: bypass blocked research with %s @ %d",
                            act.getName().c_str(),
                            int(_queue.size()) - i
                        );
                    }

                    _queue.pullToTop(size_t(i));
                    return;
                }
            }
        }
    }

    // We can reorder the queue if: Case 1:
    // We are waiting for gas and have excess minerals,
    // and we can move a later no-gas item to the front,
    // and we have the minerals to cover both
    // and the moved item doesn't require more supply.
    if (top.gasPrice() > 0 && top.gasPrice() > gas && top.mineralPrice() < minerals)
    {
        for (int i = _queue.size() - 2; i >= std::max(0, int(_queue.size()) - 5); --i)
        {
            const MacroAct & act = _queue[i].macroAct;
            // Don't reorder a command or anything after it.
            if (act.isCommand())
            {
                break;
            }
            BWAPI::Unit producer;
            if (act.isUnit() &&
                act.gasPrice() == 0 &&
                act.mineralPrice() + top.mineralPrice() <= minerals &&
                act.supplyRequired() <= top.supplyRequired() &&
                (producer = getProducer(act)) &&
                canMakeNow(producer, act))
            {
                if (Config::Debug::DrawQueueFixInfo)
                {
                    DebugQueuePrintf("queue: pull to front gas-free %s @ %d", act.getName().c_str(), _queue.size() - i);
                }
                _queue.pullToTop(i);
                return;
            }
        }
    }

    // We can reorder the queue if: Case 2:
    // We can't produce the next item
    // and a later item can be produced
    // and it does not require more supply than this item
    // and we have the resources for both.
    // This is where it starts to make a difference.
    BWAPI::Unit topProducer = getProducer(top);
    if (top.gasPrice() < gas &&
        top.mineralPrice() < minerals &&
        (!topProducer || !canMakeNow(topProducer,top)))
    {
        for (int i = _queue.size() - 2; i >= std::max(0, int(_queue.size()) - 5); --i)
        {
            const MacroAct & act = _queue[i].macroAct;
            // Don't reorder a command or anything after it.
            if (act.isCommand())
            {
                break;
            }
            BWAPI::Unit producer;
            if (act.supplyRequired() <= top.supplyRequired() &&
                act.gasPrice() + top.gasPrice() <= gas &&
                act.mineralPrice() + top.mineralPrice() <= minerals &&
                (producer = getProducer(act)) &&
                canMakeNow(producer, act))
            {
                if (Config::Debug::DrawQueueFixInfo)
                {
                    DebugQueuePrintf("queue: pull to front %s @ %d", act.getName().c_str(), _queue.size() - i);
                }
                _queue.pullToTop(i);
                return;
            }
        }
    }
}

// Return null if no producer is found.
BWAPI::Unit ProductionManager::getProducer(MacroAct act) const
{
    if (act.isBuilding() && UnitUtil::NeedsWorkerBuildingType(act.getUnitType()))
    {
        if (_assignedWorkerForThisBuilding)
        {
            return _assignedWorkerForThisBuilding;
        }
        Building b = makeBuildingForPrediction(act, act.isGasSteal());
        return WorkerManager::Instance().getBuilder(b);
    }

    std::vector<BWAPI::Unit> candidateProducers;
    act.getCandidateProducers(candidateProducers);
    // Once the BioTank army attacks, the wall Barracks must get an idle
    // moment to lift. Keep training at the other Barracks in the meantime.
    if (act.isUnit() &&
        act.whatBuilds() == BWAPI::UnitTypes::Terran_Barracks &&
        _terranWallReleasedForVision && the.placer.hasTerranMainWall())
    {
        candidateProducers.erase(
            std::remove_if(candidateProducers.begin(), candidateProducers.end(),
                [](BWAPI::Unit producer)
                {
                    return producer &&
                        the.placer.isTerranMainWallBarracks(producer) &&
                        producer->getTilePosition() ==
                            the.placer.getTerranMainWallBarracksTile();
                }),
            candidateProducers.end());
    }
    // Prefer an available interior Barracks over the exposed wall gate.
    // Keep the gate usable when it is the only producer.
    if (act.isUnit() && act.getUnitType() == BWAPI::UnitTypes::Terran_Marine &&
        the.placer.hasTerranMainWall() && !the.placer.isTerranMainWallReleased())
    {
        std::vector<BWAPI::Unit> interior;
        for (BWAPI::Unit barracks : candidateProducers)
        {
            if (barracks && !the.placer.isTerranMainWallBarracks(barracks) &&
                the.placer.isInsideTerranMainWall(barracks->getPosition()))
                interior.push_back(barracks);
        }
        if (!interior.empty()) candidateProducers.swap(interior);
    }
    if (candidateProducers.empty())
    {
        return nullptr;
    }

    // We may want to choose the producer by location.
    BWAPI::Position closestTo = BWAPI::Positions::None;     // "don't care where"
    if (act.getMacroLocation() != MacroLocation::Anywhere)
    {
        closestTo = the.placer.getMacroLocationPos(act.getMacroLocation());
    }
    else if (act.isUnit())
    {
        // We can produce from anywhere. But try the best places first when we can tell.

        // If we're producing from a larva, seek an appropriate one.
        

        // Trick: If we're producing a worker, choose the producer (command center or nexus)
        // which is farthest from the main base. That way expansions are preferentially
        // populated with less need to transfer workers.
        // Prefer expansion command centers when producing SCVs.
        if (act.isWorker())
        {
            if (act.getUnitType() == BWAPI::UnitTypes::Terran_SCV)
            {
                std::vector<BWAPI::Unit> recoveredCommandCenters;
                const int now = the.now();

                for (BWAPI::Unit producer : candidateProducers)
                {
                    if (!producer ||
                        !producer->exists() ||
                        producer->getType() != BWAPI::UnitTypes::Terran_Command_Center)
                    {
                        continue;
                    }

                    bool reservedForComsat = false;
                    for (const ProductionGoal& goal : _goals)
                    {
                        if (goal.reservesProducer(producer))
                        {
                            reservedForComsat = true;
                            break;
                        }
                    }

                    if (reservedForComsat)
                    {
                        continue;
                    }

                    const auto ignored =
                        commandCenterIgnoreUntilFrame.find(producer->getID());

                    if (ignored != commandCenterIgnoreUntilFrame.end() &&
                        now < ignored->second)
                    {
                        continue;
                    }

                    recoveredCommandCenters.push_back(producer);
                }

                if (recoveredCommandCenters.empty())
                {
                    return nullptr;
                }

                // Prefer the free Command Center whose base has the
                // largest worker deficit. "Farthest from main" was a useful
                // proxy when there were only two bases, but with 3-4 bases it
                // can keep feeding the newest expansion while another CC sits
                // almost empty.
                BWAPI::Unit bestProducer = nullptr;
                int bestDeficit = INT_MIN;
                int bestDistanceFromMain = -1;

                const BWAPI::Position mainPosition =
                    the.bases.myMain() &&
                    the.bases.myMain()->getPosition().isValid()
                    ? the.bases.myMain()->getPosition()
                    : BWAPI::Positions::None;

                for (BWAPI::Unit producer : recoveredCommandCenters)
                {
                    Base* producerBase = nullptr;

                    for (Base* base : the.bases.getAll())
                    {
                        if (base &&
                            base->getOwner() == the.self() &&
                            base->getDepot() == producer)
                        {
                            producerBase = base;
                            break;
                        }
                    }

                    int deficit = 0;

                    if (producerBase)
                    {
                        int desiredWorkers =
                            producerBase->getMaxMineralWorkers();

                        // Gas workers belong to this base's saturation target
                        // only when a completed Refinery actually exists.
                        for (BWAPI::Unit geyser : producerBase->getGeysers())
                        {
                            if (geyser &&
                                geyser->exists() &&
                                geyser->getPlayer() == the.self() &&
                                geyser->getType().isRefinery() &&
                                geyser->isCompleted())
                            {
                                desiredWorkers +=
                                    Config::Macro::WorkersPerRefinery;
                            }
                        }

                        deficit =
                            desiredWorkers - producerBase->getNumWorkers();
                    }

                    const int distanceFromMain =
                        mainPosition.isValid()
                        ? producer->getDistance(mainPosition)
                        : 0;

                    if (!bestProducer ||
                        deficit > bestDeficit ||
                        (deficit == bestDeficit &&
                         distanceFromMain > bestDistanceFromMain))
                    {
                        bestProducer = producer;
                        bestDeficit = deficit;
                        bestDistanceFromMain = distanceFromMain;
                    }
                }

                if (bestProducer)
                {
                    return bestProducer;
                }

                return getFarthestUnitFromPosition(
                    recoveredCommandCenters,
                    mainPosition);
            }

            return getFarthestUnitFromPosition(
                candidateProducers,
                the.bases.myMain()->getPosition()
            );
        }

        
        

        // If we're morphing a lair, do it out of enemy vision if possible.
        
    }
    // The no-internal-queue safeguard is specifically for the observed
    // Factory completion bug. Barracks, Starports, Command Centers, Nexuses,
    // Gateways and Zerg producers keep Steamhammer's normal queue behavior.
    if (act.isUnit() && act.whatBuilds() == BWAPI::UnitTypes::Terran_Factory)
    {
        std::vector<BWAPI::Unit> freeProducers;

        for (BWAPI::Unit producer : candidateProducers)
        {
            if (!producer || !producer->exists())
            {
                continue;
            }

            if (!producer->isCompleted())
            {
                continue;
            }

            if (producer->getType() == BWAPI::UnitTypes::Terran_Factory)
            {
                const int id = producer->getID();
                const int now = the.now();

                auto it = factoryIgnoreUntilFrame.find(id);
                if (it != factoryIgnoreUntilFrame.end())
                {
                    if (now < it->second)
                    {
                        continue;
                    }

                    factoryIgnoreUntilFrame.erase(it);
                }
            }

            if (producer->getType() == BWAPI::UnitTypes::Terran_Factory)
            {
                if (producer->isLifted())
                {
                    continue;
                }

                if (producer->isTraining())
                {
                    continue;
                }

                // isTraining() plus getRemainingTrainTime() are sufficient here.
                // Avoid getTrainingQueue(): BWAPI.dll + 0x247FD reproducibly
                // faults while copying/reading that queue on some Factories.

                if (producer->getRemainingTrainTime() > 0)
                {
                    continue;
                }
            }

            freeProducers.push_back(producer);
        }

        if (!freeProducers.empty())
        {
            return getClosestUnitToPosition(freeProducers, closestTo);
        }

        // If all producers are busy, do not pick a busy one.
        // Returning nullptr forces ProductionManager to wait.
        return nullptr;
    }

    return getClosestUnitToPosition(candidateProducers, closestTo);
}

// Comparison function for a sort done below.
bool compareByDronesNeeded(Base * left, Base * right)
{
    return
        left->getMaxWorkers() - left->getNumWorkers() > right->getMaxWorkers() - right->getNumWorkers();
}

// We're producing a zerg unit from a larva.
// The list of candidate producers is guaranteed not empty.


BWAPI::Unit ProductionManager::getClosestUnitToPosition(const std::vector<BWAPI::Unit> & units, BWAPI::Position closestTo) const
{
    if (units.empty())
    {
        return nullptr;
    }

    // if we don't care where the unit is return the first one we have
    if (closestTo == BWAPI::Positions::None)
    {
        return *(units.begin());
    }

    BWAPI::Unit closestUnit = nullptr;
    int minDist = MAX_DISTANCE;

    for (BWAPI::Unit unit : units)
    {
        UAB_ASSERT(unit, "Unit was null");

        int distance = unit->getDistance(closestTo);
        if (distance < minDist)
        {
            closestUnit = unit;
            minDist = distance;
        }
    }

    return closestUnit;
}

BWAPI::Unit ProductionManager::getFarthestUnitFromPosition(const std::vector<BWAPI::Unit> & units, BWAPI::Position farthest) const
{
    if (units.empty())
    {
        return nullptr;
    }

    // if we don't care where the unit is return the first one we have
    if (farthest == BWAPI::Positions::None)
    {
        return *(units.begin());
    }

    BWAPI::Unit farthestUnit = nullptr;
    int maxDist(-1);

    for (BWAPI::Unit unit : units)
    {
        UAB_ASSERT(unit != nullptr, "Unit was null");

        int distance = unit->getDistance(farthest);
        if (distance > maxDist)
        {
            farthestUnit = unit;
            maxDist = distance;
        }
    }

    return farthestUnit;
}

// Exclude larvas that have been ordered to morph into something else.
// NOTE This is part of a workaround for a BWAPI 4.4.0 latency compensation bug.


// We want to morph a lurker, guardian, or devourer. Try to choose a candidate that won't die quickly.


// We want to morph a lair. Try to do it in a safe place out of enemy vision.
BWAPI::Unit ProductionManager::getBestHatcheryForLair(const std::vector<BWAPI::Unit> & hatcheries) const
{
    int bestScore = INT_MIN;      // higher is better
    BWAPI::Unit bestHatch = nullptr;

    for (BWAPI::Unit hatchery : hatcheries)
    {
        BWAPI::Unit nearestEnemy = BWAPI::Broodwar->getClosestUnit(
            hatchery->getPosition(),
            BWAPI::Filter::IsEnemy,
            14 * 32);
        int score = 16 * 32;
        if (nearestEnemy)
        {
           score = hatchery->getDistance(nearestEnemy);
        }
        if (the.bases.myStart()->getOwner() == the.self() &&
            the.zone.sameArea(hatchery, the.bases.myStart()->getCenter()))
        {
            score += 8 * 32 + int(the.random.range(5.0));   // a little jitter for variety
        }
        if (score > bestScore)
        {
            bestScore = score;
            bestHatch = hatchery;
        }
    }

    return bestHatch;
}

// Create a unit, start research, etc.
bool ProductionManager::create(BWAPI::Unit producer, const BuildOrderItem & item)
{
    // Keep the invariant immediately beside the actual BWAPI production call.
    // Higher-level checks make this branch rare, but no alternate queue/rule
    // path may issue an SCV after useful live-resource capacity is reached.
    if (item.macroAct.isUnit() &&
        item.macroAct.getUnitType() == BWAPI::UnitTypes::Terran_SCV &&
        terranSCVCapReached())
    {
        return false;
    }

    if (!item.macroAct.produce(producer))
    {
        return false;
    }

    // Keep track of resources used to create combat units.
    if (item.macroAct.isCombatUnit())
    {
        _combatMineralsSpent += item.macroAct.mineralPrice();
        _combatGasSpent += item.macroAct.gasPrice();
    }
    return true;
}

bool ProductionManager::canMakeNow(BWAPI::Unit producer, MacroAct act)
{
    if (!producer || !producer->exists())
    {
        return false;
    }

    // Only Factory production uses the single-item safeguard. Applying it to
    // every Terran and Protoss producer globally changes unrelated strategies.
    if (act.isUnit() && producer->getType() == BWAPI::UnitTypes::Terran_Factory)
    {
        if (!producer->isCompleted())
        {
            return false;
        }

        if (producer->isLifted())
        {
            return false;
        }

        if (producer->isTraining())
        {
            return false;
        }

        // Do not call getTrainingQueue() here. The producer has already been
        // filtered by isTraining(), and getRemainingTrainTime() below provides
        // the additional stale/busy-state safeguard without the crashing BWAPI
        // queue accessor.

        if (producer->getRemainingTrainTime() > 0)
        {
            return false;
        }
    }

    return act.canProduce(producer);
}

// When the next item in the _queue is a building, this checks to see if we should move to
// its location in preparation for construction. If so, it takes ownership of the worker
// and orders the move.
// The function is here as it needs to access production manager's reserved resources info.
// TODO A better plan is to move the work to BuildingManager: Have ProductionManager create
//      a preliminary building and let all other steps be done in one place, and tied to
//      a specific building instance.
void ProductionManager::predictWorkerMovement(Building & b)
{
    if (b.isGasSteal)
    {
        return;
    }

    _typeOfUpcomingBuilding = b.type;

    // get a possible building location for the building
    if (!_haveLocationForThisBuilding)
    {
        _predictedTilePosition = b.finalPosition = BuildingManager::Instance().getBuildingLocation(b);
    }

    if (_predictedTilePosition.isValid())
    {
        _unplaceableMacroBuildingSinceFrame = -1;
        if (b.type == BWAPI::UnitTypes::Terran_Bunker)
            _unplaceableBunkerSinceFrame = -1;
        _haveLocationForThisBuilding = true;
        b.finalPosition = _predictedTilePosition;
    }
    else
    {
        // This function returns through the prediction branch before the
        // global jam watchdog. Army/SCV production also resets that watchdog.
        // Time an unplaceable macro building independently so it cannot hold
        // every later structure forever. The strategy may request it again.
        if (the.selfRace() == BWAPI::Races::Terran && _outOfBook &&
            !_queue.isEmpty())
        {
            if (_unplaceableMacroBuildingSinceFrame < 0)
                _unplaceableMacroBuildingSinceFrame = the.now();
            if (the.now() - _unplaceableMacroBuildingSinceFrame >= 20 * 24)
            {
                const std::string stalledName =
                    _queue.getHighestPriorityItem().macroAct.getName();
                _queue.removeHighestPriorityItem();
                clearBuildingPrediction(true);
                invalidateTerranRulePlan("unplaceable building: " + stalledName);
                _lastProductionFrame = the.now();
                setTerranRuleDecision("REPLAN unplaceable " +
                    NiceMacroActName(stalledName));
                return;
            }
        }
        // Prediction used to break out before reaching the jam watchdog.
        // A missing wall Bunker tile could therefore freeze the entire book
        // forever, even while supplementary Marines reset production time.
        if (the.selfRace() == BWAPI::Races::Terran && !_outOfBook &&
            b.type == BWAPI::UnitTypes::Terran_Bunker && !_queue.isEmpty())
        {
            if (_unplaceableBunkerSinceFrame < 0)
                _unplaceableBunkerSinceFrame = the.now();
            if (the.now() - _unplaceableBunkerSinceFrame >= 20 * 24)
            {
                _queue.removeHighestPriorityItem();
                clearBuildingPrediction(true);
                _unplaceableBunkerSinceFrame = -1;
                _lastProductionFrame = the.now();
                setTerranRuleDecision("BOOK SKIP unplaceable Bunker; continue technology");
                return;
            }
        }
        // DebugQueuePrintf("can't place building %s", UnitTypeName(b.type).c_str());

        // If we can't place the building now, we probably can't place it next frame either.
        // Delay for a while before trying again. We could overstep the time limit.
        _delayBuildingPredictionUntilFrame = 13 + the.now();
        return;
    }
    
    int x1 = _predictedTilePosition.x * 32;
    int y1 = _predictedTilePosition.y * 32;

    if (Config::Debug::DrawWorkerInfo)
    {
        int x2 = x1 + (b.type.tileWidth()) * 32;
        int y2 = y1 + (b.type.tileHeight()) * 32;
        BWAPI::Broodwar->drawBoxMap(x1, y1, x2, y2, BWAPI::Colors::Blue, false);
    }

    // If we assigned a worker and it's not available any more, forget the assignment.
    if (_assignedWorkerForThisBuilding)
    {
        if (!_assignedWorkerForThisBuilding->exists() ||					// it's dead
            _assignedWorkerForThisBuilding->getPlayer() != the.self() ||	// it was mind controlled
            _assignedWorkerForThisBuilding->isLockedDown() ||
            _assignedWorkerForThisBuilding->isStasised() ||
            _assignedWorkerForThisBuilding->isMaelstrommed() ||
            _assignedWorkerForThisBuilding->isBurrowed())
        {
            // DebugQueuePrintf("missing assigned worker cleared");
            _assignedWorkerForThisBuilding = nullptr;
        }
    }

    // Conditions under which to assign the worker: 
    //		- the build position is valid (verified above)
    //		- we haven't yet assigned a worker to move to this location
    //		- there's a valid worker to move
    //		- we expect to have the required resources by the time the worker gets there
    if (!_assignedWorkerForThisBuilding)
    {
        // Candidate worker to move into position to build. Don't assign the worker yet.
        BWAPI::Unit builder = WorkerManager::Instance().getBuilder(b);

        // Where we want position the worker.
        BWAPI::Position walkToPosition = BWAPI::Position(x1 + (b.type.tileWidth() / 2) * 32, y1 + (b.type.tileHeight() / 2) * 32);

        // What more do we need to construct this building?
        int mineralsRequired = std::max(0, b.type.mineralPrice() - getFreeMinerals());
        int gasRequired = std::max(0, b.type.gasPrice() - getFreeGas());

        if (builder &&
            WorkerManager::Instance().willHaveResources(mineralsRequired, gasRequired, builder->getDistance(walkToPosition)))
        {
            // Assign the worker.
            _assignedWorkerForThisBuilding = builder;
            WorkerManager::Instance().setBuildWorker(builder);

            // Forget about any queue modification that happened. We're beyond it.
            _queue.resetModified();

            // Move the worker.
			Base * base = the.bases.findBaseByPosition(walkToPosition);
			if (base)
			{
				the.micro.MoveToBase(builder, base, walkToPosition, true);
			}
			else
			{
				the.micro.MoveSafely(builder, walkToPosition);
			}

            _delayBuildingPredictionUntilFrame = 0;
        }
        else
        {
            // Worker availability and income do not change enough to justify
            // walking every SCV list on all 24 frames of a second. Retry four
            // times per second; an already assigned worker remains O(1), while
            // an unavailable worker adds at most a quarter-second delay.
            _delayBuildingPredictionUntilFrame = the.now() + 6;
        }
    }
}

int ProductionManager::getFreeMinerals() const
{
    return the.self()->minerals() - BuildingManager::Instance().getReservedMinerals();
}

int ProductionManager::getFreeGas() const
{
    return the.self()->gas() - BuildingManager::Instance().getReservedGas();
}

void ProductionManager::getAvailableResources(
    const MacroAct& act, int& minerals, int& gas) const
{
    minerals = getFreeMinerals();
    gas = getFreeGas();
    if (the.selfRace() == BWAPI::Races::Terran && act.isUnit() &&
        (!act.isBuilding() || act.getUnitType() == BWAPI::UnitTypes::Terran_Supply_Depot))
    {
        // A mine-blocked expansion or an unplaceable Factory remains planned,
        // but cannot freeze SCVs, supply and the army for its entire timeout.
        // New infrastructure/add-ons still honor the full reservation.
        int stalledMinerals = 0;
        int stalledGas = 0;
        BuildingManager::Instance().getStalledReservations(stalledMinerals, stalledGas);
        minerals += stalledMinerals;
        gas += stalledGas;
    }
}

void ProductionManager::executeCommand(const MacroAct & act)
{
    UAB_ASSERT(act.isCommand(), "not a command");

    MacroCommandType cmd = act.getCommandType().getType();

    if (cmd == MacroCommandType::Scout ||
        cmd == MacroCommandType::ScoutIfNeeded||
        cmd == MacroCommandType::ScoutLocation ||
        cmd == MacroCommandType::ScoutOnceOnly ||
        cmd == MacroCommandType::ScoutWhileSafe)
    {
        ScoutManager::Instance().setScoutCommand(cmd);
    }
    else if (cmd == MacroCommandType::StealGas)
    {
        ScoutManager::Instance().setGasSteal();
    }
    else if (cmd == MacroCommandType::StopGas)
    {
        WorkerManager::Instance().setCollectGas(false);
    }
    else if (cmd == MacroCommandType::StartGas)
    {
        WorkerManager::Instance().setCollectGas(true);
    }
    else if (cmd == MacroCommandType::GasUntil)
    {
        WorkerManager::Instance().setCollectGas(true);
        // NOTE This normally works correctly, but can be wrong if we turn gas on and off too quickly.
        //      It's wrong if e.g. we collect 100, then ask to collect 100 more before the first 100 is spent.
        _targetGasAmount = the.self()->gatheredGas()
            - the.self()->gas()
            + act.getCommandType().getAmount();
    }
    else   if (cmd == MacroCommandType::Aggressive)
    {
        CombatCommander::Instance().goAggressive();
    }
    else if (cmd == MacroCommandType::Defensive)
    {
        CombatCommander::Instance().goDefensive();
    }
    else if (cmd == MacroCommandType::PullWorkers)
    {
        CombatCommander::Instance().pullWorkers(act.getCommandType().getAmount());
    }
    else if (cmd == MacroCommandType::PullWorkersLeaving)
    {
        int nWorkers = WorkerManager::Instance().getNumMineralWorkers() + WorkerManager::Instance().getNumGasWorkers();
        CombatCommander::Instance().pullWorkers(nWorkers - act.getCommandType().getAmount());
    }
    else if (cmd == MacroCommandType::ReleaseWorkers)
    {
        CombatCommander::Instance().releaseWorkers();
    }
    else if (cmd == MacroCommandType::PostWorker)
    {
        WorkerManager::Instance().postWorker(act.getMacroLocation());
    }
    else if (cmd == MacroCommandType::UnpostWorkers)
    {
        WorkerManager::Instance().unpostWorkers(act.getMacroLocation());
    }
    else  if (cmd == MacroCommandType::Lift)
    {
        liftBuildings(act.getCommandType().getUnitType());
    }
    else if (cmd == MacroCommandType::QueueBarrier)
    {
        // It does nothing! Every command is a queue barrier.
    }
    else
    {
        UAB_ASSERT(false, "unknown MacroCommand");
    }
}

void ProductionManager::updateGoals()
{
    // 1. Drop any goals which have been achieved.
    _goals.remove_if([](ProductionGoal & g) { return g.done(); });

    // Urgent Vultures/Goliaths get the free Factory before an outstanding
    // Machine Shop goal. In particular, a newly recognized HeavyRush must
    // still be able to use the emergency Vulture production window.
    bool factoryUnitFirst = false;
    if (!_queue.isEmpty())
    {
        const MacroAct& next = _queue.getHighestPriorityItem().macroAct;
        factoryUnitFirst = next.isUnit() &&
            (next.getUnitType() == BWAPI::UnitTypes::Terran_Vulture ||
             next.getUnitType() == BWAPI::UnitTypes::Terran_Goliath);
    }

    // 2. Attempt to carry out goals.
    for (ProductionGoal & goal : _goals)
    {
        if (factoryUnitFirst && goal.act.isUnit() &&
            goal.act.getUnitType() == BWAPI::UnitTypes::Terran_Machine_Shop)
        {
            continue;
        }
        if (goal.update())
        {
            _lastProductionFrame = the.now();
            _terranRuleLastProgressFrame = the.now();
            setTerranRuleDecision("MAKE " + NiceMacroActName(goal.act.getName()) +
                " (production goal)");
        }
    }
}

size_t ProductionManager::getPendingGoalCount(BWAPI::UnitType type) const
{
    size_t count = 0;
    for (const ProductionGoal& goal : _goals)
    {
        if (goal.act.isUnit() && goal.act.getUnitType() == type)
        {
            ++count;
        }
    }
    return count;
}

// Can we afford it, taking into account reserved resources?
bool ProductionManager::meetsReservedResources(const MacroAct & act) const
{
    int minerals = 0;
    int gas = 0;
    getAvailableResources(act, minerals, gas);
    return
        act.mineralPrice() <= minerals &&
        act.gasPrice() <= gas;
}

// Explain the live Terran macro plan without writing a message every frame.
// Progress includes units/buildings already queued or in production, matching
// the numbers used by the executor itself.
void ProductionManager::drawTerranRuleInformation(int x, int & y)
{
    

    const int now = the.now();
    if (now < _terranDisplayFrame || now - _terranDisplayFrame >= 24)
    {
        _terranDisplayFrame = now;
        _terranDisplayDecision = _terranRuleDecision;
        _terranDisplayGoals.clear();

        size_t turretRingTarget = 0;
        size_t turretRingProgress = 0;
        for (const MetaPair & rule : _terranRulePlan)
        {
            if (!rule.first.isUnit() ||
                rule.first.getUnitType() !=
                    BWAPI::UnitTypes::Terran_Missile_Turret ||
                rule.first.getMacroLocation() != MacroLocation::Tile)
            {
                continue;
            }

            const size_t target = terranRuleTarget(rule);
            turretRingTarget += target;
            turretRingProgress += std::min(
                target, terranRuleProgress(rule.first));
        }
        if (turretRingTarget > 0 && turretRingProgress < turretRingTarget)
        {
            _terranDisplayGoals.push_back(
                "-> AA ring " + std::to_string(turretRingProgress) +
                "/" + std::to_string(turretRingTarget));
        }

        for (const MetaPair & rule : _terranRulePlan)
        {
            if (rule.first.isUnit() &&
                rule.first.getUnitType() ==
                    BWAPI::UnitTypes::Terran_Missile_Turret &&
                rule.first.getMacroLocation() == MacroLocation::Tile)
            {
                continue;
            }

            const size_t target = terranRuleTarget(rule);
            const size_t progress = terranRuleProgress(rule.first);
            if (target == 0 || progress >= target) continue;
            if (_terranDisplayGoals.size() == 5)
            {
                _terranDisplayGoals.push_back("... more goals follow");
                break;
            }
            _terranDisplayGoals.push_back("-> " + NiceMacroActName(rule.first.getName()) +
                " " + std::to_string(progress) + "/" + std::to_string(target));
        }
    }

    y += 12;

    if (!_outOfBook)
    {
        BWAPI::Broodwar->drawTextScreen(
            x, y,
            "%cMacro: %cJSON opening + parallel goals",
            cyan, white);

        y += 10;
        BWAPI::Broodwar->drawTextScreen(
            x, y,
            "%cDecision: %c%s",
            yellow,
            white,
            _terranDisplayDecision.empty()
                ? "waiting for next opening item"
                : _terranDisplayDecision.c_str());
        return;
    }

    const std::string openingGroup =
        StrategyManager::Instance().getOpeningGroup();
    const bool pureAir =
        StrategyManager::Instance().pureAirPlanActive();

    BWAPI::Broodwar->drawTextScreen(
        x, y,
        "%cMacro: %c%s%s",
        cyan,
        white,
        openingGroup.empty() ? "unknown" : openingGroup.c_str(),
        pureAir ? " + PureAir" : "");

    y += 10;
    BWAPI::Broodwar->drawTextScreen(
        x, y,
        "%cDecision: %c%s",
        yellow,
        white,
        _terranDisplayDecision.empty()
            ? "waiting for next rule evaluation"
            : _terranDisplayDecision.c_str());

    if (StrategyManager::Instance().airTanksPlanActive())
    {
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y, "%cAirTanks %c%s: %s",
            cyan, white, StrategyManager::Instance().airTanksVariant().c_str(),
            StrategyManager::Instance().airTanksAttackStatus().c_str());
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y, "%cAir fleet: %cWraith %d Valkyrie %d BC %d Vessel %d",
            cyan, white, the.my.all.count(BWAPI::UnitTypes::Terran_Wraith),
            the.my.all.count(BWAPI::UnitTypes::Terran_Valkyrie),
            the.my.all.count(BWAPI::UnitTypes::Terran_Battlecruiser),
            the.my.all.count(BWAPI::UnitTypes::Terran_Science_Vessel));
    }

    const int reservedMinerals = BuildingManager::Instance().getReservedMinerals();
    const int reservedGas = BuildingManager::Instance().getReservedGas();
    {
        int stalledMinerals = 0;
        int stalledGas = 0;
        BuildingManager::Instance().getStalledReservations(stalledMinerals, stalledGas);
        y += 10;
        BWAPI::Broodwar->drawTextScreen(
            x, y, "%cBuild reserve: %dM/%dG; stalled: %dM/%dG; addon goals: %d",
            orange, reservedMinerals, reservedGas, stalledMinerals, stalledGas,
            int(std::distance(_goals.begin(), _goals.end())));
    }

    // Macro-core diagnostics: show whether production capacity is actually
    // being used. This makes a screenshot immediately distinguish "need more
    // Factories" from "Factories exist but the scheduler/resources leave them
    // idle".
    auto producerCounts =
        [&](BWAPI::UnitType type, int& completed, int& idle)
        {
            completed = 0;
            idle = 0;

            for (BWAPI::Unit unit : the.self()->getUnits())
            {
                if (!unit ||
                    !unit->exists() ||
                    !unit->isCompleted() ||
                    unit->getType() != type ||
                    unit->isLifted())
                {
                    continue;
                }

                ++completed;

                if (!unit->isTraining() &&
                    !unit->isResearching() &&
                    !unit->isUpgrading() &&
                    unit->getRemainingTrainTime() <= 0)
                {
                    ++idle;
                }
            }
        };

    int barracksCompleted = 0;
    int barracksIdle = 0;
    int factoriesCompleted = 0;
    int factoriesIdle = 0;
    int commandCentersCompleted = 0;
    int commandCentersIdle = 0;

    producerCounts(
        BWAPI::UnitTypes::Terran_Barracks,
        barracksCompleted,
        barracksIdle);
    producerCounts(
        BWAPI::UnitTypes::Terran_Factory,
        factoriesCompleted,
        factoriesIdle);
    producerCounts(
        BWAPI::UnitTypes::Terran_Command_Center,
        commandCentersCompleted,
        commandCentersIdle);

    y += 10;
    BWAPI::Broodwar->drawTextScreen(
        x, y,
        "%cMacro core: %cSCV %d/%d  %cCC %d/%d  Rax busy %d/%d  Fac busy %d/%d",
        cyan,
        white,
        the.my.all.count(BWAPI::UnitTypes::Terran_SCV),
        WorkerManager::Instance().getMaxWorkers(),
        orange,
        commandCentersCompleted - commandCentersIdle,
        commandCentersCompleted,
        barracksCompleted - barracksIdle,
        barracksCompleted,
        factoriesCompleted - factoriesIdle,
        factoriesCompleted);

    int gasBaseTarget = 0;
    for (Base* base : the.bases.getAll())
    {
        BWAPI::Unit depot = base ? base->getDepot() : nullptr;
        if (depot && depot->exists() && depot->getPlayer() == the.self() &&
            depot->getType() == BWAPI::UnitTypes::Terran_Command_Center &&
            !depot->isLifted() && !base->getInitialGeysers().empty())
        {
            ++gasBaseTarget;
        }
    }
    y += 10;
    BWAPI::Broodwar->drawTextScreen(
        x, y, "%cGas: %cRefinery %d/%d  queued %d  unstarted %d",
        cyan, white, the.my.all.count(BWAPI::UnitTypes::Terran_Refinery),
        gasBaseTarget, int(_queue.numInQueue(BWAPI::UnitTypes::Terran_Refinery)),
        int(BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Refinery)));

    if (openingGroup == "airtanks")
    {
        const BWAPI::UnitType center = BWAPI::UnitTypes::Terran_Command_Center;
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y,
            "%cAir expo: %cCC ready %d all %d | queue %d unstarted %d",
            cyan, white, commandCentersCompleted, the.my.all.count(center),
            int(_queue.numInQueue(center)),
            int(BuildingManager::Instance().getNumUnstarted(center)));
    }

    // Factory lane diagnostics. The Tanks strategy now exposes only one
    // Tank/Vulture/Goliath production decision at a time, so this line tells
    // us exactly what the next Factory should make and the composition that
    // caused the scheduler to choose it.
    BWAPI::UnitType nextFactoryType = BWAPI::UnitTypes::None;

    for (const MetaPair & rule : _terranRulePlan)
    {
        if (!rule.first.isUnit())
        {
            continue;
        }

        const BWAPI::UnitType type = rule.first.getUnitType();

        if (type != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
            type != BWAPI::UnitTypes::Terran_Vulture &&
            type != BWAPI::UnitTypes::Terran_Goliath)
        {
            continue;
        }

        if (terranRuleProgress(rule.first) < terranRuleTarget(rule))
        {
            nextFactoryType = type;
            break;
        }
    }

    if (openingGroup == "tanks")
    {
        const BWAPI::UnitType factoryType = BWAPI::UnitTypes::Terran_Factory;
        const BWAPI::UnitType depotType = BWAPI::UnitTypes::Terran_Command_Center;
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y,
            "%cFactory growth: %cready %d all %d target %d | queue %d pending %d",
            cyan, white, factoriesCompleted, the.my.all.count(factoryType),
            terranBuildingQueueLimit(factoryType, the.my.all.count(factoryType)),
            int(_queue.numInQueue(factoryType)),
            int(BuildingManager::Instance().getNumUnstarted(factoryType)));
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y,
            "%cExpansion: %cCC ready %d all %d | queue %d pending %d",
            cyan, white, commandCentersCompleted, the.my.all.count(depotType),
            int(_queue.numInQueue(depotType)),
            int(BuildingManager::Instance().getNumUnstarted(depotType)));

        const int tankCount =
            the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
        const int vultureCount =
            the.my.all.count(BWAPI::UnitTypes::Terran_Vulture);
        const int goliathCount =
            the.my.all.count(BWAPI::UnitTypes::Terran_Goliath);

        y += 10;

        if (nextFactoryType != BWAPI::UnitTypes::None)
        {
            BWAPI::Broodwar->drawTextScreen(
                x, y,
                "%cFactory lane: %cNEXT %s  %cT%d V%d G%d",
                orange,
                white,
                NiceMacroActName(nextFactoryType.getName()).c_str(),
                cyan,
                tankCount,
                vultureCount,
                goliathCount);
        }
        else
        {
            BWAPI::Broodwar->drawTextScreen(
                x, y,
                "%cFactory lane: %cno unit deficit  %cT%d V%d G%d",
                orange,
                white,
                cyan,
                tankCount,
                vultureCount,
                goliathCount);
        }

        y += 10;
        BWAPI::Broodwar->drawTextScreen(
            x, y,
            "%cFactory reason: %ccomposition deficit / emergency minimum",
            orange,
            white);
    }
    else if (openingGroup == "bio")
    {
        const int marines = the.my.all.count(BWAPI::UnitTypes::Terran_Marine);
        const int medics = the.my.all.count(BWAPI::UnitTypes::Terran_Medic);
        const int tanks =
            the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
        int marineGoal = marines;
        int medicGoal = std::max(medics, BioProductionPolicy::medicTarget(marines,
            the.my.all.count(BWAPI::UnitTypes::Terran_Firebat),
            the.my.all.count(BWAPI::UnitTypes::Terran_Ghost)));
        int tankGoal = tanks;
        int barracksGoal = the.my.all.count(BWAPI::UnitTypes::Terran_Barracks) +
            int(BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Barracks)) +
            int(_queue.numInQueue(BWAPI::UnitTypes::Terran_Barracks));
        int factoryGoal = the.my.all.count(BWAPI::UnitTypes::Terran_Factory) +
            int(BuildingManager::Instance().getNumUnstarted(BWAPI::UnitTypes::Terran_Factory)) +
            int(_queue.numInQueue(BWAPI::UnitTypes::Terran_Factory));
        for (const MetaPair& rule : _terranRulePlan)
        {
            if (!rule.first.isUnit()) continue;
            const int target = int(terranRuleTarget(rule));
            const BWAPI::UnitType type = rule.first.getUnitType();
            if (type == BWAPI::UnitTypes::Terran_Marine)
                marineGoal = std::max(marineGoal, target);
            else if (type == BWAPI::UnitTypes::Terran_Medic)
                medicGoal = std::max(medicGoal, target);
            else if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode)
                tankGoal = std::max(tankGoal, target);
            else if (type == BWAPI::UnitTypes::Terran_Barracks)
                barracksGoal = std::max(barracksGoal, target);
            else if (type == BWAPI::UnitTypes::Terran_Factory)
                factoryGoal = std::max(factoryGoal, target);
        }
        y += 10;
        BWAPI::Broodwar->drawTextScreen(
            x, y,
            "%cBio army: %cMarine %d/%d  Medic %d/%d  Tank %d/%d  (1:5)",
            cyan, white, marines, marineGoal, medics, medicGoal,
            tanks, tankGoal);
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y,
            "%cBio capacity ready/plan: %cRax %d/%d  Fac %d/%d  Tanks %d%%",
            cyan, white, barracksCompleted, barracksGoal,
            factoriesCompleted, factoryGoal,
            StrategyManager::Instance().bioTankDesiredTankPercent());
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y,
            "%cBio push: %c%s | Stim %s Siege %s Range %d Armor %d",
            cyan, white, StrategyManager::Instance().bioTankAttackStatus().c_str(),
            the.self()->hasResearched(BWAPI::TechTypes::Stim_Packs) ? "yes" : "no",
            the.self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode) ? "yes" : "no",
            the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::U_238_Shells),
            the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::Terran_Infantry_Armor));
        int scanCharges = 0;
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (unit && unit->exists() && unit->isCompleted() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Comsat_Station)
            {
                scanCharges += unit->getEnergy() / 50;
            }
        }
        y += 10;
        BWAPI::Broodwar->drawTextScreen(x, y,
            "%cBio support: %cVessel %d | Comsat scans %d | Irradiate %s",
            cyan, white,
            the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Vessel),
            scanCharges, the.self()->hasResearched(BWAPI::TechTypes::Irradiate) ? "yes" : "no");
    }

    // Plans occupy fixed rows; changing deficits must not move the live
    // construction list up/down or look like cancelling and replacing orders.
    for (size_t row = 0; row < 6; ++row)
    {
        y += 10;
        if (row < _terranDisplayGoals.size())
            BWAPI::Broodwar->drawTextScreen(x, y, "%c%s", green,
                _terranDisplayGoals[row].c_str());
        else if (row == 0)
            BWAPI::Broodwar->drawTextScreen(x, y, "%c-> current work committed", green);
    }
}

void ProductionManager::drawProductionInformation(int x, int y)
{
    if (!Config::Debug::DrawProductionInfo)
    {
        return;
    }

    y += 10;
    
    
    
    
    y += 2;

    drawTerranRuleInformation(x - 10, y);

    // fill prod with each unit which is under construction
    std::vector<BWAPI::Unit> prod;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        UAB_ASSERT(unit != nullptr, "Unit was null");

        if (unit->isBeingConstructed())
        {
            prod.push_back(unit);
        }
    }
    
    // Actual construction keeps a stable row order even if an SCV pauses or
    // two build timers tie. The old elapsed-time comparator could swap them.
    std::set<int> visibleProduction;
    for (BWAPI::Unit unit : prod)
    {
        visibleProduction.insert(unit->getID());
        _productionDisplayFirstSeen.emplace(unit->getID(), the.now());
    }
    for (auto it = _productionDisplayFirstSeen.begin(); it != _productionDisplayFirstSeen.end(); )
    {
        if (!visibleProduction.count(it->first)) it = _productionDisplayFirstSeen.erase(it);
        else ++it;
    }
    std::sort(prod.begin(), prod.end(), [&](BWAPI::Unit a, BWAPI::Unit b)
    {
        const int firstA = _productionDisplayFirstSeen.at(a->getID());
        const int firstB = _productionDisplayFirstSeen.at(b->getID());
        return firstA != firstB ? firstA < firstB : a->getID() < b->getID();
    });

    for (BWAPI::Unit unit : prod)
    {
        y += 10;

        BWAPI::UnitType t = unit->getType();
        

        BWAPI::Broodwar->drawTextScreen(x, y, " %c%s", green, NiceMacroActName(t.getName()).c_str());
        BWAPI::Broodwar->drawTextScreen(x - 35, y, "%c%6d", green, unit->getRemainingBuildTime());
    }
    
    _queue.drawQueueInformation(x, y+10, _outOfBook);
}

ProductionManager & ProductionManager::Instance()
{
    static ProductionManager instance;
    return instance;
}

void ProductionManager::initialize()
{
    _goals.clear();
    _productionDisplayFirstSeen.clear();
    _terranDisplayGoals.clear();
    _terranDisplayDecision.clear();
    _outOfBook = false;
    _openingDefensiveMarineTarget = 0;
    _lastProductionFrame = the.now();
    clearBuildingPrediction(false);
    _unplaceableBunkerSinceFrame = -1;
    _nextTerranWallCheckFrame = 0;
    _terranWallGateOpenUntilFrame = 0;
    _terranWallLastGateCommandFrame = -100000;
    _terranWallGateOccupiedSince.clear();
    _terranWallGateClearing = false;
    _terranWallReleasedForVision = false;
    _nextTerranBarracksVisionFrame = 0;
    _targetGasAmount = 0;
    _combatMineralsSpent = 0;
    _combatGasSpent = 0;

    resetTerranRuleProduction();
    _terranRuleLastObservedFrame = -1;

    // These recovery watches live in the process-wide anonymous namespace.
    // The DLL can stay loaded between games, so stale unit ids / frame numbers
    // from the previous match must never influence the new macro state.
    factoryTrainingWatchStartFrame.clear();
    factoryTrainingLastRemaining.clear();
    factoryTrainingWatchType.clear();
    factoryIgnoreUntilFrame.clear();
    factoryRecoveryLastFrame = -1;

    factoryIdleSinceFrame.clear();

    commandCenterTrainingWatchStartFrame.clear();
    commandCenterTrainingLastRemaining.clear();
    commandCenterIgnoreUntilFrame.clear();
    commandCenterRecoveryLastFrame = -1;

    setBuildOrder(StrategyManager::Instance().getOpeningBookBuildOrder());
}

// We're zerg and doing the extractor trick to get an extra drone or pair of zerglings,
// as specified in the argument.
// Set a flag to start the procedure, and handle various error cases.


// The extractor trick is in progress. Take the next step, when possible.
// At most one step occurs per frame.


BWAPI::Position ProductionManager::getTankVisionPosition(
    BWAPI::Unit barracks) const
{
    const int mapW = BWAPI::Broodwar->mapWidth() * 32;
    const int mapH = BWAPI::Broodwar->mapHeight() * 32;

    auto clampToMap = [mapW, mapH](const BWAPI::Position& p)
        {
            return BWAPI::Position(
                std::max(0, std::min(mapW - 1, p.x)),
                std::max(0, std::min(mapH - 1, p.y))
            );
        };

    if (!barracks || !barracks->exists() ||
        (barracks->getType() != BWAPI::UnitTypes::Terran_Barracks &&
         barracks->getType() != BWAPI::UnitTypes::Terran_Engineering_Bay))
    {
        return BWAPI::Positions::Invalid;
    }

    Base* frontBase = the.bases.myFront();
    if (!frontBase)
    {
        frontBase = the.bases.myMain();
    }

    if (!frontBase)
    {
        BWAPI::Position fallback =
            the.bases.myStart()
                ? the.bases.myStart()->getCenter()
                : BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());

        return clampToMap(fallback);
    }

    BWAPI::Position frontPos = frontBase->getFront();

    if (!frontPos.isValid() || frontPos == BWAPI::Positions::Unknown)
    {
        frontPos = frontBase->getCenter();
    }

    if (!frontPos.isValid())
    {
        return BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation());
    }

    BWAPI::Position baseCenter = frontBase->getCenter();

    if (!baseCenter.isValid())
    {
        baseCenter = frontPos;
    }

    const bool pureAir = StrategyManager::Instance().pureAirPlanActive();
    std::vector<BWAPI::Unit> tanks;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isCompleted() ||
            !unit->getPosition().isValid())
        {
            continue;
        }

        const BWAPI::UnitType type = unit->getType();
        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
            (pureAir && type == BWAPI::UnitTypes::Terran_Goliath))
        {
            tanks.push_back(unit);
        }
    }

    // Follow the largest local Tank group, not a lone defender or a Tank which
    // has not yet joined the army. This is the same stable-vanguard idea used
    // by Science Vessels, with a slightly longer lead for ordinary vision.
    std::vector<BWAPI::Unit> mainTankGroup;
    const int groupRadius = 11 * 32;
    int bestGroupSpread = MAX_DISTANCE;
    for (BWAPI::Unit anchor : tanks)
    {
        std::vector<BWAPI::Unit> local;
        int spread = 0;
        for (BWAPI::Unit other : tanks)
        {
            const int distance = anchor->getDistance(other);
            if (distance <= groupRadius)
            {
                local.push_back(other);
                spread += distance;
            }
        }

        if (local.size() > mainTankGroup.size() ||
            (local.size() == mainTankGroup.size() &&
             spread < bestGroupSpread))
        {
            mainTankGroup.swap(local);
            bestGroupSpread = spread;
        }
    }

    BWAPI::Position armyCenter = baseCenter;
    BWAPI::Position vanguard = frontPos;
    if (!mainTankGroup.empty())
    {
        int sumX = 0;
        int sumY = 0;
        for (BWAPI::Unit tank : mainTankGroup)
        {
            sumX += tank->getPosition().x;
            sumY += tank->getPosition().y;
        }
        armyCenter = BWAPI::Position(
            sumX / int(mainTankGroup.size()),
            sumY / int(mainTankGroup.size()));
    }

    BWAPI::Position directionTarget = BWAPI::Positions::Invalid;
    int nearestEnemyDistance = 18 * 32 + 1;
    for (BWAPI::Unit enemy : the.enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            !enemy->isCompleted() || (!pureAir && enemy->isFlying()) ||
            enemy->isInvincible() || !enemy->getPosition().isValid() ||
            enemy->getType().isSpell())
        {
            continue;
        }

        const int distance = enemy->getDistance(armyCenter);
        if (distance < nearestEnemyDistance)
        {
            nearestEnemyDistance = distance;
            directionTarget = enemy->getPosition();
        }
    }

    if (!directionTarget.isValid() &&
        the.bases.enemyStart() &&
        the.bases.enemyStart()->getCenter().isValid())
    {
        directionTarget = the.bases.enemyStart()->getCenter();
    }
    if (!directionTarget.isValid())
    {
        directionTarget = frontPos;
    }

    double forwardX = double(directionTarget.x - armyCenter.x);
    double forwardY = double(directionTarget.y - armyCenter.y);
    double len = std::sqrt(forwardX * forwardX + forwardY * forwardY);

    if (len < 1.0)
    {
        forwardX = double(frontPos.x - baseCenter.x);
        forwardY = double(frontPos.y - baseCenter.y);
        len = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (len < 1.0)
        {
            forwardX = 0.0;
            forwardY = -1.0;
            len = 1.0;
        }
    }

    forwardX /= len;
    forwardY /= len;

    if (!mainTankGroup.empty())
    {
        std::vector<std::pair<double, BWAPI::Unit> > ordered;
        for (BWAPI::Unit tank : mainTankGroup)
        {
            const BWAPI::Position position = tank->getPosition();
            const double projection =
                double(position.x - armyCenter.x) * forwardX +
                double(position.y - armyCenter.y) * forwardY;
            ordered.push_back(std::make_pair(projection, tank));
        }

        std::sort(
            ordered.begin(), ordered.end(),
            [](const std::pair<double, BWAPI::Unit> & a,
               const std::pair<double, BWAPI::Unit> & b)
            {
                if (a.first != b.first)
                {
                    return a.first > b.first;
                }
                return a.second->getID() < b.second->getID();
            });

        const int frontCount = std::max(
            1, std::min(5, int((ordered.size() + 3) / 4)));
        int sumX = 0;
        int sumY = 0;
        for (int i = 0; i < frontCount; ++i)
        {
            sumX += ordered[size_t(i)].second->getPosition().x;
            sumY += ordered[size_t(i)].second->getPosition().y;
        }
        vanguard = BWAPI::Position(sumX / frontCount, sumY / frontCount);
    }

    bool scanReady = false;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit && unit->exists() && unit->isCompleted() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Comsat_Station &&
            unit->getEnergy() >= 50)
        {
            scanReady = true;
            break;
        }
    }

    // The Bay is a dedicated, tougher vision screen: 7-8 tiles ahead of
    // the leading combat line versus a Vessel's 3. Damaged spotters pull back.
    const bool isBay = barracks->getType() == BWAPI::UnitTypes::Terran_Engineering_Bay;
    int wantedLeadTiles = (isBay ? 7 : 5) + (scanReady ? 0 : 1);
    if (barracks->getHitPoints() * 2 < barracks->getType().maxHitPoints())
        wantedLeadTiles = 3;
    if (barracks->getHitPoints() * 4 < barracks->getType().maxHitPoints())
        wantedLeadTiles = -2;
    auto safeForFlyingBarracks = [](const BWAPI::Position & position)
    {
        if (!position.isValid())
        {
            return false;
        }

        const BWAPI::TilePosition center(position);
        for (int dx = -2; dx <= 2; ++dx)
        {
            for (int dy = -2; dy <= 2; ++dy)
            {
                const BWAPI::TilePosition tile(center.x + dx, center.y + dy);
                if (tile.isValid() && the.airHits(tile) > 0)
                {
                    return false;
                }
            }
        }
        return true;
    };

    BWAPI::Position barracksVisionPosition = BWAPI::Positions::Invalid;
    for (int leadTiles = wantedLeadTiles; leadTiles >= -2; --leadTiles)
    {
        const BWAPI::Position candidate = clampToMap(BWAPI::Position(
            int(vanguard.x + forwardX * leadTiles * 32),
            int(vanguard.y + forwardY * leadTiles * 32)));
        if (safeForFlyingBarracks(candidate))
        {
            barracksVisionPosition = candidate;
            break;
        }
    }

    if (!barracksVisionPosition.isValid())
    {
        const BWAPI::Position current = barracks->getPosition();
        if (current.isValid() && safeForFlyingBarracks(current))
        {
            barracksVisionPosition = current;
        }
        else
        {
            barracksVisionPosition = clampToMap(baseCenter);
        }
    }

    if (Config::Debug::DrawTankPositions || Config::Debug::DrawUnitTargets)
    {
        BWAPI::Broodwar->drawCircleMap(
            barracksVisionPosition, 24, BWAPI::Colors::Green, false);
        BWAPI::Broodwar->drawLineMap(
            vanguard, barracksVisionPosition, BWAPI::Colors::Green);
        BWAPI::Broodwar->drawTextMap(
            barracksVisionPosition + BWAPI::Position(6, 0),
            "TANK SPOTTER");
    }

    return barracksVisionPosition;
}

bool ProductionManager::terranWallOpeningTankCorePending() const
{
    if (the.selfRace() != BWAPI::Races::Terran ||
        StrategyManager::Instance().getOpeningGroup() != "tanks" ||
        StrategyManager::Instance().pureAirPlanActive() ||
        !the.placer.hasTerranMainWall())
    {
        return false;
    }

    // Only hold the natural once Tank production is actually available. This
    // avoids deadlocking an unusual opening whose Machine Shop is scheduled
    // after the Command Center.
    if (the.my.all.count(BWAPI::UnitTypes::Terran_Factory) <= 0 ||
        the.my.all.count(BWAPI::UnitTypes::Terran_Machine_Shop) <= 0)
    {
        return false;
    }

    if (the.my.all.count(BWAPI::UnitTypes::Terran_Command_Center) >= 2 ||
        BuildingManager::Instance().isBeingBuilt(
            BWAPI::UnitTypes::Terran_Command_Center))
    {
        return false;
    }

    const int tanks =
        the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
        the.my.all.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);

    return tanks < 4;
}

bool ProductionManager::terranMainWallGateActive() const
{
    return the.selfRace() == BWAPI::Races::Terran &&
        the.placer.hasTerranMainWall() && !_terranWallReleasedForVision;
}

// The external natural is a staged operation when the verified wall remains at
// the main choke. Once that wall is released, the natural needs defenders as
// soon as its Command Center starts. Otherwise the mobile screen stays at the
// old main front while the new base is exposed. With a closed main wall, keep
// the completed-depot and combat-core staging requirement.
bool ProductionManager::terranNaturalArmyStagingReady() const
{
    if (the.selfRace() != BWAPI::Races::Terran ||
        !the.placer.hasTerranMainWall() ||
        the.placer.terranMainWallCoversNatural())
    {
        return true;
    }

    Base * natural = the.bases.myNatural();
    if (!natural || natural->getOwner() != the.self())
    {
        return false;
    }

    BWAPI::Unit depot = natural->getDepot();
    if (!depot || !depot->exists() || depot->isFlying())
    {
        return false;
    }

    // BioTank's Marines must cover the natural while its Command Center is
    // being built. The ordinary mech rule waits for a completed depot and a
    // durable Tank/Goliath core, leaving this expansion unguarded.
    if (StrategyManager::Instance().bioTankPlanActive())
    {
        return true;
    }

    if (the.placer.isTerranMainWallReleased())
    {
        return true;
    }

    if (!UnitUtil::IsCompletedResourceDepot(depot))
    {
        return false;
    }

    Base * main = the.bases.myMain();
    if (main)
    {
        for (BWAPI::Unit enemy : the.enemy()->getUnits())
        {
            if (enemy && enemy->exists() && enemy->isVisible() &&
                !enemy->isFlying() && !enemy->getType().isSpell() &&
                enemy->getPosition().isValid() &&
                enemy->getDistance(main->getCenter()) <= 16 * 32)
            {
                return false;
            }
        }
    }

    int tanks = 0;
    int goliaths = 0;
    int marines = 0;
    int vultures = 0;
    int combatSupply = 0;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isCompleted() ||
            unit->isFlying() || unit->getType().isWorker() ||
            unit->getType().isBuilding() ||
            !UnitUtil::IsCombatUnit(unit))
        {
            continue;
        }

        const BWAPI::UnitType type = unit->getType();
        combatSupply += std::max(1, type.supplyRequired() / 2);
        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            ++tanks;
        else if (type == BWAPI::UnitTypes::Terran_Goliath)
            ++goliaths;
        else if (type == BWAPI::UnitTypes::Terran_Marine)
            ++marines;
        else if (type == BWAPI::UnitTypes::Terran_Vulture)
            ++vultures;
    }

    const bool durableCore = tanks >= 2 || goliaths >= 4;
    const bool mobileCore = vultures >= 4 && (marines + goliaths) >= 4;
    return combatSupply >= 8 && (durableCore || mobileCore);
}

// Workers may follow only after the staged combat group has actually reached
// the natural. This is intentionally stricter than ArmyStagingReady(): the
// first predicate starts one organized squad move, this one unlocks the gentle
// one-SCV-per-second mineral rebalance after defenders are in position.
bool ProductionManager::terranNaturalTransferReady() const
{
    if (!terranNaturalArmyStagingReady())
    {
        return false;
    }
    if (the.selfRace() != BWAPI::Races::Terran ||
        !the.placer.hasTerranMainWall() ||
        the.placer.terranMainWallCoversNatural())
    {
        return true;
    }

    Base * natural = the.bases.myNatural();
    if (!natural || !natural->getCenter().isValid())
    {
        return false;
    }

    const BWAPI::Position rally = natural->getFront().isValid()
        ? natural->getFront() : natural->getCenter();
    int tanks = 0;
    int goliaths = 0;
    int marines = 0;
    int vultures = 0;
    int nearbySupply = 0;
    bool bunkerReady = false;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->getPosition().isValid() ||
            unit->getDistance(rally) > 15 * 32)
        {
            continue;
        }

        const BWAPI::UnitType type = unit->getType();
        if (type == BWAPI::UnitTypes::Terran_Bunker && !unit->isLifted())
        {
            bunkerReady = unit->isCompleted() ||
                unit->getRemainingBuildTime() <= 15 * 24;
            continue;
        }
        if (!unit->isCompleted() || unit->isFlying() ||
            type.isWorker() || type.isBuilding() ||
            !UnitUtil::IsCombatUnit(unit))
        {
            continue;
        }

        nearbySupply += std::max(1, type.supplyRequired() / 2);
        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            ++tanks;
        else if (type == BWAPI::UnitTypes::Terran_Goliath)
            ++goliaths;
        else if (type == BWAPI::UnitTypes::Terran_Marine)
            ++marines;
        else if (type == BWAPI::UnitTypes::Terran_Vulture)
            ++vultures;
    }

    const bool hardAnchor = bunkerReady || tanks >= 3 || goliaths >= 5;
    const bool combinedScreen = tanks >= 2 &&
        (goliaths + marines + vultures) >= 3;
    return nearbySupply >= 8 && (hardAnchor || combinedScreen);
}

int ProductionManager::terranBuildingQueueLimit(
    BWAPI::UnitType type, int fallback) const
{
    const auto target = _terranStructuralTargets.find(type.getID());
    return target == _terranStructuralTargets.end()
        ? fallback : std::max(fallback, int(target->second));
}

void ProductionManager::manageTerranMainWall()
{
    if (the.selfRace() != BWAPI::Races::Terran ||
        !the.placer.hasTerranMainWall())
    {
        return;
    }

    if (the.now() < _nextTerranWallCheckFrame)
    {
        return;
    }
    _nextTerranWallCheckFrame = the.now() + 6;

    BWAPI::Unit wallBarracks = nullptr;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit && unit->exists() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Barracks &&
            the.placer.isTerranMainWallBarracks(unit))
        {
            wallBarracks = unit;
            break;
        }
    }

    if (!wallBarracks || !wallBarracks->isCompleted())
    {
        return;
    }

    const BWAPI::TilePosition wallTile =
        the.placer.getTerranMainWallBarracksTile();
    const BWAPI::Position wallCenter =
        the.placer.getTerranMainWallGateCenter();
    Base * main = the.bases.myMain();

    if (!wallTile.isValid() || !wallCenter.isValid() || !main ||
        !main->getCenter().isValid())
    {
        return;
    }

    const BWAPI::Position mainCenter = main->getCenter();
    const int wallToMain = wallCenter.getApproxDistance(mainCenter);

    // The first real BioTank attack is an irreversible transition out of
    // opening defense. Marine production must never close this route again.
    if (!_terranWallReleasedForVision &&
        ((StrategyManager::Instance().bioTankPlanActive() &&
          StrategyManager::Instance().bioTankPushReady()) ||
         (StrategyManager::Instance().airTanksPlanActive() &&
          StrategyManager::Instance().airTanksPushReady())) &&
        CombatCommander::Instance().groundSquadAttackOrderActive())
    {
        _terranWallReleasedForVision = true;
        the.placer.markTerranMainWallReleasedForVision();
    }

    // A reserved Command Center task is not an expansion on the map: its SCV
    // can still be inside, saving money or waiting for the gate. Only actual
    // construction outside the main ends the opening gate's ownership.
    if (!_terranWallReleasedForVision &&
        StrategyManager::Instance().getOpeningGroup() == "tanks")
    {
        for (BWAPI::Unit unit : the.self()->getUnits())
        {
            if (unit && unit->exists() && !unit->isLifted() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Command_Center &&
                unit->getPosition().isValid() &&
                unit->getDistance(mainCenter) > 10 * 32 &&
                !the.placer.isInsideTerranMainWall(unit->getPosition()) &&
                terranNaturalTransferReady())
            {
                _terranWallReleasedForVision = true;
                the.placer.markTerranMainWallReleasedForVision();
                break;
            }
        }
    }

    bool enemyGroundNearGate = false;
    for (BWAPI::Unit enemy : the.enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            enemy->isFlying() || !enemy->getPosition().isValid())
        {
            continue;
        }

        // Any real enemy ground body can use an opened gate; it need not have
        // a conventional weapon (High Templar/Defiler are obvious examples).
        if (enemy->getType().isSpell())
        {
            continue;
        }

        if (enemy->getDistance(wallCenter) <= 11 * 32)
        {
            enemyGroundNearGate = true;
            break;
        }
    }

    // Do not open the gate merely because a nearby threat stepped into fog.
    // Mobile memories use a short window; grounded buildings stay authoritative
    // until their tile is actually checked empty.
    if (!enemyGroundNearGate)
    {
        for (const auto & entry : the.info.getUnitInfo(the.enemy()))
        {
            const UnitInfo & enemy = entry.second;
            if (enemy.goneFromLastPosition || !enemy.lastPosition.isValid() ||
                enemy.lifted || enemy.type.isFlyer() ||
                enemy.type.isSpell())
            {
                continue;
            }

            // Mobile memories expire quickly. A known grounded building cannot
            // move away, so keep it until the tile is actually checked empty.
            if (!enemy.type.isBuilding() &&
                the.now() - enemy.updateFrame > 8 * 24)
            {
                continue;
            }

            if (enemy.lastPosition.getApproxDistance(wallCenter) <= 10 * 32)
            {
                enemyGroundNearGate = true;
                break;
            }
        }
    }

    // A wall at the natural contains the second Command Center, so the
    // outside-expansion release above can never fire for it. Once a tank
    // opening has committed its ground squad to an attack, retire this gate
    // too: repeatedly landing the Barracks would otherwise funnel the whole
    // army through brief temporary openings. Keep the gate while an enemy is
    // close enough to enter it.
    if (!_terranWallReleasedForVision &&
        the.placer.terranMainWallCoversNatural() &&
        StrategyManager::Instance().getOpeningGroup() == "tanks" &&
        CombatCommander::Instance().groundSquadAttackOrderActive() &&
        !enemyGroundNearGate)
    {
        _terranWallReleasedForVision = true;
        the.placer.markTerranMainWallReleasedForVision();
    }

    const bool barracksProductionQueued =
        _queue.anyInQueue(BWAPI::UnitTypes::Terran_Ghost) ||
        _queue.anyInQueue(BWAPI::UnitTypes::Terran_Marine) ||
        _queue.anyInQueue(BWAPI::UnitTypes::Terran_Medic) ||
        _queue.anyInQueue(BWAPI::UnitTypes::Terran_Firebat);
    bool hasOtherLandedBarracks = false;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit && unit->exists() && unit->isCompleted() &&
            unit != wallBarracks && !unit->isLifted() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Barracks)
        {
            hasOtherLandedBarracks = true;
            break;
        }
    }
    const bool releasedWallNeededAsProducer =
        !StrategyManager::Instance().bioTankPlanActive() &&
        !hasOtherLandedBarracks &&
        (barracksProductionQueued || wallBarracks->isTraining());

    if (_terranWallReleasedForVision)
    {
        the.placer.markTerranMainWallReleasedForVision();
        // Once the former gate has landed at a safe production tile, it is a
        // Barracks again. Repeatedly lifting it here fought the landing
        // recovery and left BioTank with one permanently idle producer.
        if (!wallBarracks->isLifted() &&
            wallBarracks->getTilePosition() != wallTile)
        {
            return;
        }
        // Production takes priority for the only Barracks. When a Bay can
        // provide vision, leave the relocated Barracks at its useful base tile.
        // The original wall tile must still be vacated for expansion traffic.
        if (releasedWallNeededAsProducer ||
            (getEngineeringBayTankSpotter() && wallBarracks->getTilePosition() != wallTile))
        {
            return;
        }

        if (wallBarracks->isLifted())
        {
            _terranWallGateOpenUntilFrame = 0;
            the.placer.markTerranMainWallReleasedForVision();
            return;
        }

        // Do not permanently release the wall into an enemy standing at the
        // door. The expansion builder waits until the gate is clear; after the
        // safe lift this manager will never order the Barracks to land again.
        if (!enemyGroundNearGate &&
            wallBarracks->canLift() &&
            the.now() - _terranWallLastGateCommandFrame >= 12 &&
            the.micro.Lift(wallBarracks))
        {
            _terranWallLastGateCommandFrame = the.now();
            _terranWallGateOpenUntilFrame = 0;
            the.placer.markTerranMainWallReleasedForVision();
        }
        return;
    }

    bool friendlyWantsThroughGate = false;
    bool friendlyStillInGate = false;
    bool gateBlockerStalled = false;
    std::set<int> currentGateBlockers;

    const int gateLeft = wallTile.x * 32;
    const int gateTop = wallTile.y * 32;
    const int gateRight =
        (wallTile.x + BWAPI::UnitTypes::Terran_Barracks.tileWidth()) * 32 - 1;
    const int gateBottom =
        (wallTile.y + BWAPI::UnitTypes::Terran_Barracks.tileHeight()) * 32 - 1;

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isCompleted() ||
            unit->isFlying() || unit->isLoaded() ||
            unit->getType().isBuilding() ||
            !unit->getType().canMove() ||
            !unit->getPosition().isValid())
        {
            continue;
        }

        const int distanceToGate = unit->getDistance(wallCenter);
        if (distanceToGate > 8 * 32)
        {
            continue;
        }

        const int unitToMain = unit->getDistance(mainCenter);
        const bool outside = unitToMain > wallToMain + 48;
        const bool inside = unitToMain + 48 < wallToMain;
        const BWAPI::Position target = unit->getTargetPosition();
        const int targetToMain = target.isValid()
            ? target.getApproxDistance(mainCenter)
            : unitToMain;
        const bool targetHeadsInside =
            target.isValid() && targetToMain + 64 < wallToMain;
        const bool targetHeadsOutside =
            target.isValid() && targetToMain > wallToMain + 64;

        const bool crossesVerifiedGate =
            target.isValid() &&
            the.placer.crossesTerranMainWall(
                unit->getPosition(), target, 16);

        if (crossesVerifiedGate ||
            (outside && targetHeadsInside) ||
            (inside && targetHeadsOutside))
        {
            friendlyWantsThroughGate = true;
        }

        // Keep the gate open only while the unit's collision box overlaps the
        // real Barracks landing footprint. The old four-tile radius allowed a
        // nearby idle SCV/Tank to hold the wall open forever.
        const BWAPI::Position position = unit->getPosition();
        const int unitLeft = position.x - unit->getType().dimensionLeft();
        const int unitRight = position.x + unit->getType().dimensionRight();
        const int unitTop = position.y - unit->getType().dimensionUp();
        const int unitBottom = position.y + unit->getType().dimensionDown();
        const int doorwayMargin = 12;
        const bool overlapsDoorway =
            unitRight >= gateLeft - doorwayMargin &&
            unitLeft <= gateRight + doorwayMargin &&
            unitBottom >= gateTop - doorwayMargin &&
            unitTop <= gateBottom + doorwayMargin;

        if (wallBarracks->isLifted() && overlapsDoorway)
        {
            friendlyStillInGate = true;
            currentGateBlockers.insert(unit->getID());
            const auto entry = _terranWallGateOccupiedSince.emplace(
                unit->getID(), the.now());
            // Walking in circles is movement, but it is not a completed
            // crossing. Only leaving the footprint resets this deadline.
            if (the.now() - entry.first->second >= 4 * 24)
                gateBlockerStalled = true;
        }
    }
    for (auto it = _terranWallGateOccupiedSince.begin();
         it != _terranWallGateOccupiedSince.end(); )
    {
        if (currentGateBlockers.count(it->first) == 0)
            it = _terranWallGateOccupiedSince.erase(it);
        else
            ++it;
    }

    if (!wallBarracks->isLifted())
    {
        _terranWallGateOccupiedSince.clear();
        _terranWallGateClearing = false;
        if (wallBarracks->getTilePosition() != wallTile)
        {
            // Recover a gate displaced by an earlier producer/landing order.
            // Finish any paid training first, then return to the verified slot.
            if (!wallBarracks->isTraining() && wallBarracks->canLift() &&
                the.now() - _terranWallLastGateCommandFrame >= 12 &&
                the.micro.Lift(wallBarracks))
            {
                _terranWallLastGateCommandFrame = the.now();
                _terranWallGateOpenUntilFrame = the.now() + 24;
            }
            return;
        }
        if (wallBarracks->getTilePosition() == wallTile)
        {
            the.placer.markTerranMainWallClosed();
        }

        // The wall is a gate, not a permanently released opening. Lift only
        // for an own ground unit whose target is genuinely on the other side
        // (including the natural builder), and never while an enemy ground
        // threat is close enough to use the opening.
        if (the.placer.isTerranMainWallComplete() &&
            friendlyWantsThroughGate &&
            !enemyGroundNearGate &&
            wallBarracks->canLift() &&
            the.now() - _terranWallLastGateCommandFrame >= 12)
        {
            if (the.micro.Lift(wallBarracks))
            {
                _terranWallLastGateCommandFrame = the.now();
                _terranWallGateOpenUntilFrame = the.now() + 3 * 24;
                the.placer.markTerranMainWallOpened();
            }
        }
        return;
    }

    the.placer.markTerranMainWallOpened();

    if (enemyGroundNearGate || gateBlockerStalled)
        _terranWallGateClearing = true;

    // Finish one close operation before accepting more crossing requests.
    // Keep the same evacuation targets while the Barracks is descending.
    if (_terranWallGateClearing)
        the.micro.clearLandingArea(wallBarracks, wallTile, !enemyGroundNearGate);

    if (!_terranWallGateClearing &&
        (friendlyWantsThroughGate || friendlyStillInGate))
    {
        _terranWallGateOpenUntilFrame =
            std::max(_terranWallGateOpenUntilFrame, the.now() + 24);
    }

    // If some other system lifted the wall Barracks, recover it as a temporary
    // gate too instead of letting the Tank-vision code fly it away forever.
    if (_terranWallGateOpenUntilFrame <= 0)
    {
        _terranWallGateOpenUntilFrame = the.now() + 2 * 24;
    }

    const bool shouldCloseNow =
        _terranWallGateClearing ||
        (!friendlyStillInGate && the.now() >= _terranWallGateOpenUntilFrame);

    if (!shouldCloseNow ||
        wallBarracks->getOrder() == BWAPI::Orders::BuildingLand ||
        the.now() - _terranWallLastGateCommandFrame < 12)
    {
        return;
    }

    if (wallBarracks->canLand(wallTile, false, false) &&
        wallBarracks->land(wallTile))
    {
        _terranWallLastGateCommandFrame = the.now();
        _terranWallGateOpenUntilFrame = 0;
    }
}

bool ProductionManager::engineeringBayNeededForUpgrades() const
{
    for (BWAPI::UpgradeType upgrade : {
        BWAPI::UpgradeTypes::Terran_Infantry_Weapons,
        BWAPI::UpgradeTypes::Terran_Infantry_Armor })
    {
        if (the.self()->isUpgrading(upgrade) || _queue.anyInQueue(upgrade))
            return true;
        for (const auto & goal : _goals)
            if (goal.act.isUpgrade() && goal.act.getUpgradeType() == upgrade)
                return true;
        for (const auto & rule : _terranRulePlan)
            if (rule.first.isUpgrade() && rule.first.getUpgradeType() == upgrade &&
                size_t(the.self()->getUpgradeLevel(upgrade)) < terranRuleTarget(rule))
                return true;
    }
    return false;
}

BWAPI::Unit ProductionManager::getEngineeringBayTankSpotter() const
{
    if (StrategyManager::Instance().getOpeningGroup() != "tanks" ||
        engineeringBayNeededForUpgrades())
        return nullptr;

    bool hasTank = false;
    BWAPI::Unit best = nullptr;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isCompleted()) continue;
        const auto type = unit->getType();
        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            hasTank = true;
        if (type != BWAPI::UnitTypes::Terran_Engineering_Bay ||
            unit->isUpgrading() || unit->getOrder() == BWAPI::Orders::BuildingLand)
            continue;
        if (!unit->isLifted() && !unit->canLift()) continue;
        // Prefer an existing flyer, then a stable ID: never lift every Bay.
        if (!best || (unit->isLifted() && !best->isLifted()) ||
            (unit->isLifted() == best->isLifted() && unit->getID() < best->getID()))
            best = unit;
    }
    return hasTank ? best : nullptr;
}

void ProductionManager::moveLiftedBarracksForTankVision()
{
    if (StrategyManager::Instance().getOpeningGroup() != "tanks")
    {
        return;
    }

    if (the.now() < _nextTerranBarracksVisionFrame)
    {
        return;
    }
    _nextTerranBarracksVisionFrame = the.now() + 12;

    const bool barracksProductionQueued =
        _queue.anyInQueue(BWAPI::UnitTypes::Terran_Ghost) ||
        _queue.anyInQueue(BWAPI::UnitTypes::Terran_Marine) ||
        _queue.anyInQueue(BWAPI::UnitTypes::Terran_Medic) ||
        _queue.anyInQueue(BWAPI::UnitTypes::Terran_Firebat);

    BWAPI::Unit releasedWallBarracks = nullptr;
    BWAPI::Unit ordinaryLiftedBarracks = nullptr;
    for (BWAPI::Unit u : the.self()->getUnits())
    {
        if (!u || !u->exists() || !u->isCompleted() ||
            u->getType() != BWAPI::UnitTypes::Terran_Barracks ||
            !u->isLifted())
        {
            continue;
        }

        if (the.placer.isTerranMainWallBarracks(u))
        {
            if (_terranWallReleasedForVision)
            {
                releasedWallBarracks = u;
            }
            continue;
        }

        if (!ordinaryLiftedBarracks)
        {
            ordinaryLiftedBarracks = u;
        }
    }

    BWAPI::Unit spotter = getEngineeringBayTankSpotter();
    if (spotter && !spotter->isLifted())
    {
        the.micro.Lift(spotter);
        return;
    }
    if (!spotter && !barracksProductionQueued)
        spotter = releasedWallBarracks ? releasedWallBarracks : ordinaryLiftedBarracks;

    if (!spotter ||
        spotter->getOrder() == BWAPI::Orders::LiftingOff ||
        spotter->getOrder() == BWAPI::Orders::BuildingLand)
    {
        return;
    }

    const BWAPI::Position target = getTankVisionPosition(spotter);
    if (!target.isValid())
    {
        return;
    }

    if (spotter->getDistance(target) > 64)
    {
        the.micro.Move(spotter, target);
    }
}

void ProductionManager::liftExcessBarracks(int keepLanded) const
{
    keepLanded = std::max(getEngineeringBayTankSpotter() ? 1 : 0, keepLanded);

    std::vector<BWAPI::Unit> landed;
    std::vector<BWAPI::Unit> liftable;

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit ||
            !unit->exists() ||
            !unit->isCompleted() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Barracks ||
            unit->isLifted())
        {
            continue;
        }

        // The wall Barracks is either the opening gate or, after the natural
        // is committed, the dedicated Tank spotter. Generic excess-production
        // cleanup must never fight either lifecycle.
        if (the.placer.isTerranMainWallBarracks(unit))
        {
            continue;
        }

        landed.push_back(unit);

        if (unit->canLift())
        {
            liftable.push_back(unit);
        }
    }

    int toLift = std::max(0, int(landed.size()) - keepLanded);
    if (toLift <= 0 || liftable.empty())
    {
        return;
    }

    // Keep the Barracks nearest our main as the stable producer. Prefer
    // lifting forward/outlying idle Barracks, which are more likely to sit in
    // the Tank/Vulture traffic lane. Most importantly, never bounce the LAST
    // required Barracks between lift and land every macro wave.
    const BWAPI::Position mainPosition =
        the.bases.myMain()
        ? the.bases.myMain()->getPosition()
        : BWAPI::Position(the.self()->getStartLocation());

    std::sort(
        liftable.begin(),
        liftable.end(),
        [mainPosition](BWAPI::Unit a, BWAPI::Unit b)
        {
            return a->getDistance(mainPosition) > b->getDistance(mainPosition);
        });

    for (BWAPI::Unit unit : liftable)
    {
        if (toLift <= 0)
        {
            break;
        }

        if (the.micro.Lift(unit))
        {
            --toLift;
        }
    }
}

bool ProductionManager::liftTerranMainWallBarracks(
    bool requireInteriorBarracks) const
{
    if (!the.placer.hasTerranMainWall())
    {
        return false;
    }

    // Normally keep an interior replacement producer before opening.  The one
    // exception is an expansion SCV already waiting behind the wall: its
    // caller may open immediately to avoid a Command Center deadlock.  The
    // wall slot stays reserved, so any replacement is built inside the base.
    bool hasInteriorBarracks = false;
    BWAPI::Unit wallBarracks = nullptr;
    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (!unit || !unit->exists() || !unit->isCompleted() ||
            unit->getType() != BWAPI::UnitTypes::Terran_Barracks)
        {
            continue;
        }

        if (the.placer.isTerranMainWallBarracks(unit))
        {
            wallBarracks = unit;
        }
        else if (!unit->isLifted())
        {
            hasInteriorBarracks = true;
        }
    }

    if (!wallBarracks)
    {
        return false;
    }
    if (wallBarracks->isLifted())
    {
        the.placer.markTerranMainWallOpened();
        return true;
    }
    if ((requireInteriorBarracks && !hasInteriorBarracks) ||
        !wallBarracks->canLift())
    {
        return false;
    }

    if (!the.micro.Lift(wallBarracks))
    {
        return false;
    }

    the.placer.markTerranMainWallOpened();
    return true;
}

void ProductionManager::liftBuildings(BWAPI::UnitType type) const
{
    for (BWAPI::Unit u : the.self()->getUnits())
    {
        if (u->getType() == type && u->canLift())
        {
            // Generic JSON "lift Barracks" commands are for clearing an idle
            // production building out of the army lane. The verified wall
            // Barracks is a managed gate and must be opened only for an actual
            // crossing, otherwise two systems repeatedly lift/reland it.
            if (type == BWAPI::UnitTypes::Terran_Barracks &&
                the.placer.isTerranMainWallBarracks(u))
            {
                continue;
            }
            (void)the.micro.Lift(u);
        }
    }
}

void ProductionManager::queueGasSteal()
{
    _queue.queueAsHighestPriority(MacroAct(the.self()->getRace().getRefinery(), MacroLocation::GasSteal), true);
}

bool ProductionManager::isGasStealInQueue() const
{
    return _queue.isGasStealInQueue() || BuildingManager::Instance().isGasStealInQueue();
}

bool ProductionManager::nextIsBuilding() const
{
    if (_queue.isEmpty())
    {
        return false;
    }

    const MacroAct & next = _queue.getHighestPriorityItem().macroAct;

    return
        next.isBuilding() &&
        UnitUtil::NeedsWorkerBuildingType(next.getUnitType());
}

// We have finished our book line, or are breaking out of it early.
// Clear the queue, set _outOfBook, go aggressive.
// NOTE This clears the queue even if we are already out of book.
void ProductionManager::goOutOfBookAndClearQueue()
{
    const bool enteringOutOfBook = !_outOfBook;

    if (Config::Debug::DrawQueueFixInfo && !_queue.isEmpty())
    {
        DebugQueuePrintf("queue: go out of book and clear queue");
    }
    _queue.clearAll();
    clearBuildingPrediction(true);

    if (enteringOutOfBook && the.selfRace() == BWAPI::Races::Terran)
    {
        // Add-on/research goals created by the abandoned opening must not
        // survive into a different strategic plan.
        _goals.clear();
    }

    _outOfBook = true;
    _lastProductionFrame = the.now();       // don't immediately clear the "jam" again

    // Terran combat posture belongs to StrategyManager. Finishing a JSON book
    // or recovering production is not evidence that the army should attack.
    
}

// If we're in book, leave it and clear the queue.
// Otherwise do nothing.
void ProductionManager::goOutOfBook()
{
    if (!_outOfBook)
    {
        if (Config::Debug::DrawQueueFixInfo && _queue.isEmpty())
        {
            DebugQueuePrintf("queue: go out of book");
        }
        goOutOfBookAndClearQueue();
    }
}
