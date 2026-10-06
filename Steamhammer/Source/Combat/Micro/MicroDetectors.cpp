#include "Combat/Micro/MicroManager.h"
#include "Combat/Micro/MicroDetectors.h"

#include "Core/The.h"
#include "Map/Bases.h"
#include "Intelligence/Learning/GameRecord.h"
#include "Strategy/StrategyManager.h"
#include "Utilities/UnitUtil.h"

#include <algorithm>
#include <cmath>
#include <set>
#include "Combat/Micro/Terran/MicroTanks.h"

using namespace UAlbertaBot;

MicroDetectors::MicroDetectors()
    : squadSize(0)
    , unitClosestToTarget(nullptr)
    , _lastStateFrame(-1)
    , _detectionRequestPosition(BWAPI::Positions::Invalid)
    , _detectionRequestFrame(-100000)
    , _expansionDetectionPosition(BWAPI::Positions::Invalid)
    , _expansionDetectionFrame(-100000)
{
}

void MicroDetectors::executeMicro(const BWAPI::Unitset&, const UnitCluster&)
{
}

static bool isVesselGroupUnit(BWAPI::Unit unit)
{
    if (!unit ||
        !unit->exists() ||
        !unit->isCompleted() ||
        !unit->getPosition().isValid())
    {
        return false;
    }

    const BWAPI::UnitType type = unit->getType();

    if (type.isBuilding() || type.isWorker())
    {
        return false;
    }

    if (type == BWAPI::UnitTypes::Terran_Science_Vessel)
    {
        return false;
    }

    return
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
        type == BWAPI::UnitTypes::Terran_Goliath ||
        type == BWAPI::UnitTypes::Terran_Vulture ||
        type == BWAPI::UnitTypes::Terran_Marine ||
        UnitUtil::CanAttackGround(unit) ||
        UnitUtil::CanAttackAir(unit);
}

static bool getBiggestLocalSquadCenter(
    const BWAPI::Unitset& squadUnits,
    BWAPI::Position& center,
    int& size)
{
    std::vector<BWAPI::Unit> units;

    for (BWAPI::Unit unit : squadUnits)
    {
        if (isVesselGroupUnit(unit))
        {
            units.push_back(unit);
        }
    }

    if (units.empty())
    {
        center = BWAPI::Positions::Invalid;
        size = 0;
        return false;
    }

    const int GROUP_RADIUS = 10 * 32;

    int bestCount = 0;
    int bestSumX = 0;
    int bestSumY = 0;

    for (BWAPI::Unit anchor : units)
    {
        int count = 0;
        int sumX = 0;
        int sumY = 0;

        for (BWAPI::Unit other : units)
        {
            if (anchor->getDistance(other) <= GROUP_RADIUS)
            {
                ++count;
                sumX += other->getPosition().x;
                sumY += other->getPosition().y;
            }
        }

        if (count > bestCount)
        {
            bestCount = count;
            bestSumX = sumX;
            bestSumY = sumY;
        }
    }

    if (bestCount <= 0)
    {
        center = BWAPI::Positions::Invalid;
        size = 0;
        return false;
    }

    center = BWAPI::Position(bestSumX / bestCount, bestSumY / bestCount);
    size = bestCount;

    return center.isValid();
}

// Find the front edge of the main local army without locking onto one lone
// scout. Averaging the foremost quarter gives the Vessel a stable lead point
// as new reinforcements join from behind.
static BWAPI::Position getLocalArmyVanguardCenter(
    const BWAPI::Unitset& squadUnits,
    const BWAPI::Position& armyCenter,
    const BWAPI::Position& orderPosition)
{
    if (!armyCenter.isValid() || !orderPosition.isValid())
    {
        return armyCenter;
    }

    double dx = double(orderPosition.x - armyCenter.x);
    double dy = double(orderPosition.y - armyCenter.y);
    double length = std::sqrt(dx * dx + dy * dy);

    if (length < 1.0)
    {
        return armyCenter;
    }

    dx /= length;
    dy /= length;

    std::vector<std::pair<double, BWAPI::Unit>> ranked;
    for (BWAPI::Unit unit : squadUnits)
    {
        if (!isVesselGroupUnit(unit) ||
            unit->getDistance(armyCenter) > 10 * 32)
        {
            continue;
        }

        const BWAPI::Position p = unit->getPosition();
        const double projection =
            double(p.x - armyCenter.x) * dx +
            double(p.y - armyCenter.y) * dy;
        ranked.push_back(std::make_pair(projection, unit));
    }

    if (ranked.empty())
    {
        return armyCenter;
    }

    std::sort(
        ranked.begin(),
        ranked.end(),
        [](const std::pair<double, BWAPI::Unit>& a,
           const std::pair<double, BWAPI::Unit>& b)
        {
            if (a.first != b.first)
            {
                return a.first > b.first;
            }
            return a.second->getID() < b.second->getID();
        });

    const int frontCount =
        std::max(1, std::min(5, int((ranked.size() + 3) / 4)));

    int sumX = 0;
    int sumY = 0;
    for (int i = 0; i < frontCount; ++i)
    {
        sumX += ranked[i].second->getPosition().x;
        sumY += ranked[i].second->getPosition().y;
    }

    return BWAPI::Position(sumX / frontCount, sumY / frontCount);
}

bool MicroDetectors::canCastAnotherSpell(BWAPI::Unit vessel) const
{
    if (!vessel || !vessel->exists())
    {
        return false;
    }

    auto it = _lastSpellCastFrame.find(vessel->getID());
    if (it == _lastSpellCastFrame.end())
    {
        return true;
    }

    const int SPELL_COOLDOWN_FRAMES = 24;
    return BWAPI::Broodwar->getFrameCount() - it->second >= SPELL_COOLDOWN_FRAMES;
}

void MicroDetectors::rememberSpellCast(BWAPI::Unit vessel)
{
    if (!vessel || !vessel->exists())
    {
        return;
    }

    _lastSpellCastFrame[vessel->getID()] = BWAPI::Broodwar->getFrameCount();
}

int MicroDetectors::scoreDefensiveMatrixTarget(BWAPI::Unit vessel, BWAPI::Unit unit) const
{
    if (!vessel || !unit || !vessel->exists() || !unit->exists())
    {
        return -1;
    }

    if (!unit->getPosition().isValid())
    {
        return -1;
    }

    const BWAPI::UnitType type = unit->getType();
    const bool isTank =
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
        type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
    const bool isGoliath =
        type == BWAPI::UnitTypes::Terran_Goliath;
    const bool isVessel =
        type == BWAPI::UnitTypes::Terran_Science_Vessel;

    if (!isTank && !isGoliath && !isVessel)
    {
        return -1;
    }

    if (unit->isDefenseMatrixed())
    {
        return -1;
    }

    if (!vessel->canUseTech(BWAPI::TechTypes::Defensive_Matrix, unit))
    {
        return -1;
    }

    int nearbyEnemyAttackers = 0;
    int veryCloseEnemyAttackers = 0;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
        unit->getPosition(),
        8 * 32,
        BWAPI::Filter::IsEnemy))
    {
        if (!enemy || !enemy->exists() || !enemy->isCompleted())
        {
            continue;
        }

        if (!enemy->getType().canAttack())
        {
            continue;
        }

        ++nearbyEnemyAttackers;

        if (unit->getDistance(enemy) <= 4 * 32)
        {
            ++veryCloseEnemyAttackers;
        }
    }

    const int maxHP = std::max(1, unit->getType().maxHitPoints());
    const double hpRatio = double(unit->getHitPoints()) / double(maxHP);

    const bool underRealThreat =
        unit->isUnderAttack() ||
        nearbyEnemyAttackers >= 2 ||
        veryCloseEnemyAttackers >= 1 ||
        hpRatio <= 0.75;

    if (!underRealThreat)
    {
        return -1;
    }

    int score = 0;

    if (isTank)    score += 60;
    if (isGoliath) score += 35;
    if (isVessel)  score += 30;

    if (unit->isUnderAttack())       score += 40;
    score += nearbyEnemyAttackers * 12;
    score += veryCloseEnemyAttackers * 10;

    if (hpRatio <= 0.75) score += 20;
    if (hpRatio <= 0.50) score += 20;
    if (hpRatio <= 0.30) score += 20;

    return score;
}

BWAPI::Unit MicroDetectors::getBestDefensiveMatrixTarget(BWAPI::Unit vessel, const BWAPI::Unitset& squadUnits) const
{
    BWAPI::Unit bestTarget = nullptr;
    int bestScore = -1;

    for (BWAPI::Unit unit : squadUnits)
    {
        int score = scoreDefensiveMatrixTarget(vessel, unit);
        if (score > bestScore)
        {
            bestScore = score;
            bestTarget = unit;
        }
    }

    return bestTarget;
}

bool MicroDetectors::tryDefensiveMatrix(BWAPI::Unit vessel, const BWAPI::Unitset& squadUnits)
{
    if (!vessel || !vessel->exists())
    {
        return false;
    }

    if (!BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Defensive_Matrix))
    {
        return false;
    }

    if (vessel->getEnergy() < 100)
    {
        return false;
    }

    BWAPI::Unit bestTarget = getBestDefensiveMatrixTarget(vessel, squadUnits);
    if (!bestTarget)
    {
        return false;
    }

    if (vessel->useTech(BWAPI::TechTypes::Defensive_Matrix, bestTarget))
    {
        rememberSpellCast(vessel);
        return true;
    }

    return false;
}

int MicroDetectors::scoreIrradiateTarget(BWAPI::Unit vessel, BWAPI::Unit enemy) const
{
    if (!vessel || !enemy || !vessel->exists() || !enemy->exists())
    {
        return -1;
    }

    if (!enemy->isCompleted() || !enemy->getPosition().isValid())
    {
        return -1;
    }

    if (!enemy->isVisible() || !enemy->isDetected())
    {
        return -1;
    }

    if (enemy->getPlayer() != BWAPI::Broodwar->enemy())
    {
        return -1;
    }

    const BWAPI::UnitType type = enemy->getType();

    if (type.isBuilding() || type.isSpell() || type.isInvincible())
    {
        return -1;
    }

    if (!type.isOrganic())
    {
        return -1;
    }

    if (enemy->isIrradiated())
    {
        return -1;
    }

    if (type == BWAPI::UnitTypes::Zerg_Larva ||
        type == BWAPI::UnitTypes::Zerg_Egg ||
        type == BWAPI::UnitTypes::Zerg_Broodling ||
        type == BWAPI::UnitTypes::Zerg_Overlord ||
        type == BWAPI::UnitTypes::Zerg_Scourge ||
        type == BWAPI::UnitTypes::Zerg_Zergling ||
        type == BWAPI::UnitTypes::Zerg_Drone)
    {
        return -1;
    }

    if (!vessel->canUseTech(BWAPI::TechTypes::Irradiate, enemy))
    {
        return -1;
    }

    int nearbyEnemyOrganic = 0;
    int nearbyEnemyImportant = 0;

    for (BWAPI::Unit other : BWAPI::Broodwar->getUnitsInRadius(
        enemy->getPosition(),
        2 * 32,
        BWAPI::Filter::IsEnemy))
    {
        if (!other ||
            !other->exists() ||
            !other->isCompleted() ||
            !other->getType().isOrganic() ||
            other->getType().isBuilding())
        {
            continue;
        }

        ++nearbyEnemyOrganic;

        BWAPI::UnitType otherType = other->getType();

        if (otherType == BWAPI::UnitTypes::Zerg_Defiler ||
            otherType == BWAPI::UnitTypes::Zerg_Lurker ||
            otherType == BWAPI::UnitTypes::Zerg_Mutalisk ||
            otherType == BWAPI::UnitTypes::Zerg_Guardian ||
            otherType == BWAPI::UnitTypes::Zerg_Devourer ||
            otherType == BWAPI::UnitTypes::Zerg_Queen ||
            otherType == BWAPI::UnitTypes::Zerg_Ultralisk ||
            otherType == BWAPI::UnitTypes::Zerg_Hydralisk)
        {
            ++nearbyEnemyImportant;
        }
    }

    int nearbyFriendlyOrganic = 0;

    for (BWAPI::Unit other : BWAPI::Broodwar->getUnitsInRadius(
        enemy->getPosition(),
        2 * 32,
        BWAPI::Filter::IsOwned))
    {
        if (!other ||
            !other->exists() ||
            !other->isCompleted() ||
            !other->getType().isOrganic() ||
            other->getType().isBuilding())
        {
            continue;
        }

        ++nearbyFriendlyOrganic;
    }

    if (nearbyFriendlyOrganic > 0)
    {
        return -1;
    }

    int score = 0;

    if (type == BWAPI::UnitTypes::Zerg_Defiler)
    {
        score += 1000;
    }
    else if (type == BWAPI::UnitTypes::Zerg_Lurker)
    {
        score += 850;
    }
    else if (type == BWAPI::UnitTypes::Zerg_Guardian)
    {
        score += 800;
    }
    else if (type == BWAPI::UnitTypes::Zerg_Devourer)
    {
        score += 760;
    }
    else if (type == BWAPI::UnitTypes::Zerg_Mutalisk)
    {
        score += 720;
    }
    else if (type == BWAPI::UnitTypes::Zerg_Queen)
    {
        score += 650;
    }
    else if (type == BWAPI::UnitTypes::Zerg_Ultralisk)
    {
        score += 600;
    }
    else if (type == BWAPI::UnitTypes::Zerg_Hydralisk)
    {
        if (nearbyEnemyOrganic < 3 && nearbyEnemyImportant < 2)
        {
            return -1;
        }

        score += 350;
    }
    else
    {
        return -1;
    }

    score += nearbyEnemyOrganic * 35;
    score += nearbyEnemyImportant * 50;

    score -= vessel->getDistance(enemy) / 16;

    return score;
}

BWAPI::Unit MicroDetectors::getBestIrradiateTarget(BWAPI::Unit vessel) const
{
    BWAPI::Unit bestTarget = nullptr;
    int bestScore = -1;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        int score = scoreIrradiateTarget(vessel, enemy);

        if (score > bestScore)
        {
            bestScore = score;
            bestTarget = enemy;
        }
    }

    if (bestScore < 400)
    {
        return nullptr;
    }

    return bestTarget;
}

bool MicroDetectors::tryIrradiate(BWAPI::Unit vessel)
{
    if (!vessel || !vessel->exists())
    {
        return false;
    }

    if (BWAPI::Broodwar->enemy()->getRace() != BWAPI::Races::Zerg)
    {
        return false;
    }

    if (!BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Irradiate))
    {
        return false;
    }

    if (vessel->getEnergy() < 75)
    {
        return false;
    }

    BWAPI::Unit bestTarget = getBestIrradiateTarget(vessel);
    if (!bestTarget)
    {
        return false;
    }

    if (vessel->useTech(BWAPI::TechTypes::Irradiate, bestTarget))
    {
        rememberSpellCast(vessel);

        BWAPI::Broodwar->drawLineMap(
            vessel->getPosition(),
            bestTarget->getPosition(),
            BWAPI::Colors::Green
        );

        DrawUnitTargetTextMap(
            bestTarget->getPosition() + BWAPI::Position(0, -16),
            "IRRADIATE"
        );

        return true;
    }

    return false;
}

int MicroDetectors::scoreEMPAtPosition(BWAPI::Unit vessel, BWAPI::Position pos) const
{
    if (!vessel || !vessel->exists() || !pos.isValid())
    {
        return -1;
    }

    if (!vessel->canUseTech(BWAPI::TechTypes::EMP_Shockwave, pos))
    {
        return -1;
    }

    int score = 0;
    int affectedUnits = 0;
    bool hasPriorityTarget = false;

    for (BWAPI::Unit other : BWAPI::Broodwar->getUnitsInRadius(
        pos,
        3 * 32,
        BWAPI::Filter::IsEnemy))
    {
        if (!other || !other->exists() || !other->isCompleted())
        {
            continue;
        }

        BWAPI::UnitType type = other->getType();

        if (type.maxShields() > 0)
        {
            score += other->getShields();
            ++affectedUnits;
        }

        bool priorityUnit = false;
        if (type == BWAPI::UnitTypes::Protoss_High_Templar) { score += 180; priorityUnit = true; }
        if (type == BWAPI::UnitTypes::Protoss_Dark_Archon) { score += 180; priorityUnit = true; }
        if (type == BWAPI::UnitTypes::Protoss_Arbiter) { score += 200; priorityUnit = true; }
        if (type == BWAPI::UnitTypes::Protoss_Archon) { score += 110; priorityUnit = true; }
        if (type == BWAPI::UnitTypes::Protoss_Carrier) { score += 100; priorityUnit = true; }
        // Terran units have no shields. Do not spend EMP only because an
        // empty caster/capital ship happens to be nearby; require enough
        // stored energy for the drain to have immediate tactical value.
        if (type == BWAPI::UnitTypes::Terran_Ghost && other->getEnergy() >= 50)
        {
            score += 160;
            priorityUnit = true;
        }
        if (type == BWAPI::UnitTypes::Terran_Science_Vessel && other->getEnergy() >= 50)
        {
            score += 140;
            priorityUnit = true;
        }
        if (type == BWAPI::UnitTypes::Terran_Battlecruiser && other->getEnergy() >= 100)
        {
            score += 120;
            priorityUnit = true;
        }

        if (priorityUnit)
        {
            // EMP should prefer a charged caster/Yamato over the same empty
            // unit, while shields still make clustered Protoss valuable.
            score += std::min(200, other->getEnergy());
            hasPriorityTarget = true;
        }
    }

    if (affectedUnits < 2 && !hasPriorityTarget)
    {
        return -1;
    }

    return score;
}

BWAPI::Position MicroDetectors::getBestEMPPosition(BWAPI::Unit vessel) const
{
    BWAPI::Position bestPos = BWAPI::Positions::None;
    int bestScore = -1;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isCompleted() || !enemy->getPosition().isValid())
        {
            continue;
        }

        int score = scoreEMPAtPosition(vessel, enemy->getPosition());
        if (score > bestScore)
        {
            bestScore = score;
            bestPos = enemy->getPosition();
        }
    }

    return bestPos;
}

bool MicroDetectors::tryEMP(BWAPI::Unit vessel)
{
    if (!vessel || !vessel->exists())
    {
        return false;
    }

    BWAPI::Race enemyRace = BWAPI::Broodwar->enemy()->getRace();
    if (enemyRace != BWAPI::Races::Protoss && enemyRace != BWAPI::Races::Terran)
    {
        return false;
    }

    if (!BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::EMP_Shockwave))
    {
        return false;
    }

    if (vessel->getEnergy() < 100)
    {
        return false;
    }

    BWAPI::Position bestPos = getBestEMPPosition(vessel);
    if (!bestPos.isValid())
    {
        return false;
    }

    if (vessel->useTech(BWAPI::TechTypes::EMP_Shockwave, bestPos))
    {
        rememberSpellCast(vessel);
        return true;
    }

    return false;
}

bool MicroDetectors::useScienceVesselAbilities(BWAPI::Unit vessel, const BWAPI::Unitset& squadUnits)
{
    if (!vessel || !vessel->exists())
    {
        return false;
    }

    if (vessel->getType() != BWAPI::UnitTypes::Terran_Science_Vessel)
    {
        return false;
    }

    if (!canCastAnotherSpell(vessel))
    {
        return false;
    }

    const bool pureAir =
        StrategyManager::Instance().pureAirPlanActive();
    const bool versusZerg =
        BWAPI::Broodwar->enemy()->getRace() == BWAPI::Races::Zerg;
    const EnemyThreatMask threats =
        StrategyManager::Instance().getEnemyThreatMask();
    const bool highValueEMPThreat =
        HasEnemyThreat(threats, EnemyThreat::Spellcaster) ||
        HasEnemyThreat(threats, EnemyThreat::CapitalAir);

    // Against Zerg, a ready Irradiate is the Vessel's strategic spell. Spend
    // 75 energy on a Defiler/Muta cluster before Matrix can consume 100.
    if (versusZerg && tryIrradiate(vessel))
    {
        return true;
    }

    // Against Carriers, Battlecruisers and dedicated spellcasters, EMP must
    // not lose all Vessel energy to a routine Matrix cast first.
    if ((pureAir || highValueEMPThreat) && tryEMP(vessel))
    {
        return true;
    }

    if (tryDefensiveMatrix(vessel, squadUnits))
    {
        return true;
    }

    if (!pureAir && !highValueEMPThreat && tryEMP(vessel))
    {
        return true;
    }

    return false;
}

static bool isBadScienceVesselTarget(BWAPI::Unit enemy)
{
    if (!enemy || !enemy->exists() || !enemy->getPosition().isValid())
    {
        return true;
    }

    const BWAPI::UnitType type = enemy->getType();

    return
        type == BWAPI::UnitTypes::Zerg_Larva ||
        type == BWAPI::UnitTypes::Zerg_Egg ||
        type == BWAPI::UnitTypes::Protoss_Interceptor ||
        type == BWAPI::UnitTypes::Protoss_Scarab ||
        type == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine;
}

static bool isDangerousToScienceVessel(BWAPI::Unit enemy)
{
    if (isBadScienceVesselTarget(enemy))
    {
        return false;
    }

    if (!enemy->isVisible())
    {
        return false;
    }

    const BWAPI::UnitType type = enemy->getType();

    if (type == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
        type == BWAPI::UnitTypes::Terran_Missile_Turret ||
        type == BWAPI::UnitTypes::Zerg_Spore_Colony)
    {
        return true;
    }

    return UnitUtil::CanAttackAir(enemy);
}

static int scienceVesselDangerRange(BWAPI::Unit enemy, BWAPI::Unit vessel)
{
    if (!enemy || !vessel)
    {
        return 0;
    }

    int range = UnitUtil::GetAttackRange(enemy, vessel);

    const BWAPI::UnitType type = enemy->getType();

    if (type == BWAPI::UnitTypes::Terran_Missile_Turret ||
        type == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
        type == BWAPI::UnitTypes::Zerg_Spore_Colony)
    {
        range = std::max(range, 7 * 32);
    }

    // safety buffer
    return range + 2 * 32;
}

static bool positionSafeForScienceVessel(
    BWAPI::Unit vessel,
    const BWAPI::Position& pos)
{
    if (!vessel || !vessel->exists() || !pos.isValid())
    {
        return false;
    }

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!isDangerousToScienceVessel(enemy))
        {
            continue;
        }

        const int dangerRange = scienceVesselDangerRange(enemy, vessel);

        if (enemy->getDistance(pos) <= dangerRange)
        {
            return false;
        }
    }

    return true;
}

static BWAPI::Position clampScienceVesselPositionToMap(
    const BWAPI::Position& position);

static BWAPI::Position getScienceVesselDetectionEdgePosition(
    BWAPI::Unit vessel,
    const BWAPI::Position& targetPosition)
{
    if (!vessel || !vessel->exists() || !targetPosition.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const int detectionTiles =
        std::max(2, UnitUtil::GetDetectionRange(vessel->getType()) - 1);
    const int radius = detectionTiles * 32;

    static const double directions[][2] =
    {
        { 1.0,  0.0 }, {-1.0,  0.0 },
        { 0.0,  1.0 }, { 0.0, -1.0 },
        { 0.707,  0.707 }, { 0.707, -0.707 },
        {-0.707,  0.707 }, {-0.707, -0.707 }
    };

    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestDistance = MAX_DISTANCE;

    for (const auto& direction : directions)
    {
        BWAPI::Position candidate(
            targetPosition.x + int(direction[0] * radius),
            targetPosition.y + int(direction[1] * radius));

        candidate = clampScienceVesselPositionToMap(candidate);

        if (!positionSafeForScienceVessel(vessel, candidate))
        {
            continue;
        }

        const int distance = vessel->getDistance(candidate);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = candidate;
        }
    }

    return best;
}

static BWAPI::Position getVisibleEnemyArmyCenterForVessel()
{
    int count = 0;
    int sumX = 0;
    int sumY = 0;
    int score = 0;

    for (BWAPI::Unit enemy : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (isBadScienceVesselTarget(enemy))
        {
            continue;
        }

        if (!enemy->isVisible() || !enemy->isCompleted())
        {
            continue;
        }

        if (enemy->getType().isWorker())
        {
            continue;
        }
        if (!enemy->getType().isBuilding() &&
            !UnitUtil::CanAttackGround(enemy) &&
            !UnitUtil::CanAttackAir(enemy) &&
            enemy->getType() != BWAPI::UnitTypes::Zerg_Lurker &&
            enemy->getType() != BWAPI::UnitTypes::Zerg_Defiler &&
            enemy->getType() != BWAPI::UnitTypes::Protoss_High_Templar)
        {
            continue;
        }

        sumX += enemy->getPosition().x;
        sumY += enemy->getPosition().y;
        ++count;

        if (UnitUtil::CanAttackGround(enemy) || UnitUtil::CanAttackAir(enemy))
        {
            score += 2;
        }
        else
        {
            score += 1;
        }
    }

    if (count <= 0 || score < 3)
    {
        return BWAPI::Positions::Invalid;
    }

    return BWAPI::Position(sumX / count, sumY / count);
}

static BWAPI::Position clampScienceVesselPositionToMap(const BWAPI::Position& p)
{
    const int maxX = BWAPI::Broodwar->mapWidth() * 32 - 1;
    const int maxY = BWAPI::Broodwar->mapHeight() * 32 - 1;

    return BWAPI::Position(
        std::max(0, std::min(maxX, p.x)),
        std::max(0, std::min(maxY, p.y))
    );
}

static BWAPI::Position getSafePositionAroundEnemyArmyForVessel(
    BWAPI::Unit vessel,
    const BWAPI::Position& enemyCenter)
{
    if (!vessel || !vessel->exists() || !enemyCenter.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    if (positionSafeForScienceVessel(vessel, enemyCenter))
    {
        return enemyCenter;
    }

    static const int radii[] =
    {
        5 * 32,
        7 * 32,
        9 * 32,
        11 * 32,
        13 * 32
    };

    static const double dirs[][2] =
    {
        { 1.0,  0.0 },
        {-1.0,  0.0 },
        { 0.0,  1.0 },
        { 0.0, -1.0 },
        { 0.7,  0.7 },
        { 0.7, -0.7 },
        {-0.7,  0.7 },
        {-0.7, -0.7 }
    };

    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestDistanceToEnemy = MAX_DISTANCE;
    int bestDistanceToVessel = MAX_DISTANCE;

    for (int r : radii)
    {
        for (const auto& dir : dirs)
        {
            BWAPI::Position candidate(
                enemyCenter.x + int(dir[0] * r),
                enemyCenter.y + int(dir[1] * r)
            );

            candidate = clampScienceVesselPositionToMap(candidate);

            if (!candidate.isValid())
            {
                continue;
            }

            if (!positionSafeForScienceVessel(vessel, candidate))
            {
                continue;
            }

            const int distToEnemy = candidate.getApproxDistance(enemyCenter);
            const int distToVessel = vessel->getDistance(candidate);

            if (distToEnemy < bestDistanceToEnemy ||
                (distToEnemy == bestDistanceToEnemy &&
                    distToVessel < bestDistanceToVessel))
            {
                bestDistanceToEnemy = distToEnemy;
                bestDistanceToVessel = distToVessel;
                best = candidate;
            }
        }

        if (best.isValid())
        {
            return best;
        }
    }

    return BWAPI::Positions::Invalid;
}

static BWAPI::Position getScienceVesselDefaultScoutPosition(
    BWAPI::Unit vessel,
    const BWAPI::Position& orderPosition)
{
    (void)orderPosition;

    if (the.bases.myNatural() && the.bases.myNatural()->getPosition().isValid())
    {
        return the.bases.myNatural()->getPosition();
    }

    if (the.bases.myMain() && the.bases.myMain()->getPosition().isValid())
    {
        return the.bases.myMain()->getPosition();
    }

    return vessel && vessel->exists()
        ? vessel->getPosition()
        : BWAPI::Positions::Invalid;
}

static BWAPI::Position getScienceVesselEscortPosition(
    BWAPI::Unit vessel,
    const BWAPI::Position& armyCenter,
    const BWAPI::Position& orderPosition,
    int vesselIndex)
{
    if (!vessel || !vessel->exists())
    {
        return BWAPI::Positions::Invalid;
    }

    if (!armyCenter.isValid())
    {
        return getScienceVesselDefaultScoutPosition(vessel, orderPosition);
    }

    BWAPI::Position directionTarget = orderPosition;
    const BWAPI::Position visibleEnemyCenter =
        getVisibleEnemyArmyCenterForVessel();

    if (visibleEnemyCenter.isValid() &&
        visibleEnemyCenter.getApproxDistance(armyCenter) <= 14 * 32)
    {
        directionTarget = visibleEnemyCenter;
    }

    double dx = directionTarget.isValid()
        ? double(directionTarget.x - armyCenter.x)
        : 0.0;
    double dy = directionTarget.isValid()
        ? double(directionTarget.y - armyCenter.y)
        : -1.0;
    double length = std::sqrt(dx * dx + dy * dy);

    if (length < 1.0)
    {
        dx = 0.0;
        dy = -1.0;
        length = 1.0;
    }

    dx /= length;
    dy /= length;
    const double sideX = -dy;
    const double sideY = dx;
    const int sideSign = vesselIndex % 2 == 0 ? -1 : 1;
    const int sideOffset = sideSign * (vesselIndex / 2) * 48;

    // Escort behind the local vanguard when scouting would expose the Vessel.
    for (int forwardTiles = -2; forwardTiles >= -5; --forwardTiles)
    {
        BWAPI::Position candidate(
            int(armyCenter.x + dx * forwardTiles * 32 + sideX * sideOffset),
            int(armyCenter.y + dy * forwardTiles * 32 + sideY * sideOffset));

        candidate = clampScienceVesselPositionToMap(candidate);
        if (positionSafeForScienceVessel(vessel, candidate))
        {
            return candidate;
        }
    }

    return vessel->getPosition();
}

static bool safeScienceVesselScoutLeg(
    BWAPI::Unit vessel,
    const BWAPI::Position& destination)
{
    if (!vessel || !destination.isValid())
    {
        return false;
    }

    const BWAPI::Position start = vessel->getPosition();
    const int distance = start.getApproxDistance(destination);
    const int steps = std::max(1, (distance + 31) / 32);
    for (int step = 1; step <= steps; ++step)
    {
        const BWAPI::Position sample(
            start.x + (destination.x - start.x) * step / steps,
            start.y + (destination.y - start.y) * step / steps);
        if (!positionSafeForScienceVessel(vessel, sample) ||
            the.airHitsFixed.at(BWAPI::TilePosition(sample)) > 0)
        {
            return false;
        }
    }
    return true;
}

static BWAPI::Position getScienceVesselScoutDestination(
    BWAPI::Unit vessel,
    const BWAPI::Position& orderPosition,
    const BWAPI::Position& armyCenter,
    const BWAPI::Position& escortAnchor,
    int vesselIndex,
    bool allowForwardScout)
{
    if (!vessel || !vessel->exists())
    {
        return BWAPI::Positions::Invalid;
    }

    BWAPI::Position fallback =
        getScienceVesselEscortPosition(
            vessel,
            escortAnchor.isValid() ? escortAnchor : armyCenter,
            orderPosition,
            vesselIndex);

    if (!fallback.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    // One Vessel can reveal terrain just ahead of the army. Keep its lead
    // short so the tanks can use the vision and the Vessel can fall back.
    if (allowForwardScout && armyCenter.isValid() &&
        escortAnchor.isValid() && orderPosition.isValid() &&
        escortAnchor.getApproxDistance(orderPosition) > 4 * 32)
    {
        const double dx = double(orderPosition.x - escortAnchor.x);
        const double dy = double(orderPosition.y - escortAnchor.y);
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length >= 1.0)
        {
            for (int leadTiles = 4; leadTiles >= 2; --leadTiles)
            {
                const BWAPI::Position scout = clampScienceVesselPositionToMap(
                    BWAPI::Position(
                        int(escortAnchor.x + dx * leadTiles * 32 / length),
                        int(escortAnchor.y + dy * leadTiles * 32 / length)));
                if (scout.getApproxDistance(armyCenter) <= 9 * 32 &&
                    safeScienceVesselScoutLeg(vessel, scout))
                {
                    return scout;
                }
            }
        }
    }

    if (positionSafeForScienceVessel(vessel, fallback) &&
        (!armyCenter.isValid() ||
            fallback.getApproxDistance(armyCenter) <= 9 * 32))
    {
        return fallback;
    }

    const BWAPI::Position compactFallback =
        getScienceVesselEscortPosition(
            vessel,
            armyCenter,
            orderPosition,
            vesselIndex);

    if (compactFallback.isValid() &&
        positionSafeForScienceVessel(vessel, compactFallback))
    {
        return compactFallback;
    }

    return vessel->getPosition();
}

void MicroDetectors::go(const BWAPI::Unitset& squadUnits)
{
    const BWAPI::Unitset& allDetectorUnits = getUnits();

    if (allDetectorUnits.empty())
    {
        return;
    }

    BWAPI::Unitset detectorUnits;
    BWAPI::Unitset genericDetectorUnits;
    std::set<int> liveVesselIDs;

    for (BWAPI::Unit detector : allDetectorUnits)
    {
        if (!detector || !detector->exists())
        {
            continue;
        }

        if (detector->getType() == BWAPI::UnitTypes::Terran_Science_Vessel)
        {
            detectorUnits.insert(detector);
            liveVesselIDs.insert(detector->getID());
        }
        else
        {
            // Observer and Overlord retain Steamhammer's race-neutral escort
            // behavior. Science Vessel scouting and spells must never be
            // applied to them.
            genericDetectorUnits.insert(detector);
        }
    }

    const int now = BWAPI::Broodwar->getFrameCount();
    if (_lastStateFrame > now)
    {
        _lastSpellCastFrame.clear();
    }
    _lastStateFrame = now;

    for (auto it = _lastSpellCastFrame.begin(); it != _lastSpellCastFrame.end(); )
    {
        if (liveVesselIDs.find(it->first) == liveVesselIDs.end())
        {
            it = _lastSpellCastFrame.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (!genericDetectorUnits.empty())
    {
        BWAPI::Unitset genericCloakedTargets;
        BWAPI::Unitset genericEnemies;
        BWAPI::Unitset genericGroundDefenders;
        BWAPI::Unitset genericAirDefenders;

        for (BWAPI::Unit target : BWAPI::Broodwar->enemy()->getUnits())
        {
            if (!target ||
                !target->exists() ||
                !target->getPosition().isValid() ||
                !target->isCompleted())
            {
                continue;
            }

            if (target->getType().hasPermanentCloak() ||
                target->getType().isCloakable() ||
                target->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
                target->getType() == BWAPI::UnitTypes::Zerg_Lurker ||
                target->isBurrowed() ||
                target->getOrder() == BWAPI::Orders::Burrowing)
            {
                genericCloakedTargets.insert(target);
            }

            if (UnitUtil::CanAttackAir(target))
            {
                genericEnemies.insert(target);
            }
        }

        for (BWAPI::Unit unit : squadUnits)
        {
            if (!unit || !unit->exists() || !unit->getPosition().isValid())
            {
                continue;
            }

            if (UnitUtil::CanAttackGround(unit) && !unit->getType().isWorker())
            {
                genericGroundDefenders.insert(unit);
            }
            if (UnitUtil::CanAttackAir(unit))
            {
                genericAirDefenders.insert(unit);
            }
        }

        for (BWAPI::Unit detector : genericDetectorUnits)
        {
            if (squadSize == 1)
            {
                const BWAPI::Position destination =
                    order && order->getPosition().isValid()
                    ? order->getPosition()
                    : BWAPI::Positions::Origin;
                the.micro.MoveSafely(detector, destination);
                continue;
            }

            BWAPI::Position destination;
            BWAPI::Unit nearestEnemy = NearestOf(detector->getPosition(), genericEnemies);
            BWAPI::Unit nearestGroundDefender = NearestOf(detector->getPosition(), genericGroundDefenders);
            BWAPI::Unit nearestAirDefender = NearestOf(detector->getPosition(), genericAirDefenders);
            BWAPI::Unit nearestCloaked = NearestOf(detector->getPosition(), genericCloakedTargets);

            if (nearestEnemy &&
                detector->getDistance(nearestEnemy) <=
                    2 * 32 + UnitUtil::GetAttackRange(nearestEnemy, detector))
            {
                if (nearestEnemy->isFlying() &&
                    nearestAirDefender &&
                    detector->getDistance(nearestAirDefender) <= 12 * 32)
                {
                    destination = nearestAirDefender->getPosition();
                }
                else if (!nearestEnemy->isFlying() &&
                    nearestGroundDefender &&
                    detector->getDistance(nearestGroundDefender) <= 8 * 32)
                {
                    destination = nearestGroundDefender->getPosition();
                }
                else
                {
                    destination = DistanceAndDirection(
                        detector->getPosition(),
                        nearestEnemy->getPosition(),
                        -8 * 32
                    );
                }
            }
            else if (nearestCloaked && detector->getDistance(nearestCloaked) > 9 * 32)
            {
                destination = nearestCloaked->getPosition();
            }
            else if (unitClosestToTarget &&
                unitClosestToTarget->getPosition().isValid() &&
                !the.airHitsFixed.at(unitClosestToTarget->getTilePosition()))
            {
                destination = unitClosestToTarget->getPosition();
            }
            else if (the.bases.myMain() && the.bases.myMain()->getPosition().isValid())
            {
                destination = the.bases.myMain()->getPosition();
            }
            else
            {
                destination = BWAPI::Positions::Origin;
            }

            the.micro.MoveNear(detector, destination);
        }
    }

    if (detectorUnits.empty())
    {
        return;
    }

    BWAPI::Position biggestSquadCenter = BWAPI::Positions::Invalid;
    int biggestSquadSize = 0;

    getBiggestLocalSquadCenter(
        squadUnits,
        biggestSquadCenter,
        biggestSquadSize
    );

    const BWAPI::Position vesselEscortAnchor =
        getLocalArmyVanguardCenter(
            squadUnits,
            biggestSquadCenter,
            order ? order->getPosition() : BWAPI::Positions::Invalid);

    BWAPI::Unitset cloakedTargets;
    BWAPI::Unitset enemies;

    for (const BWAPI::Unit target : BWAPI::Broodwar->enemy()->getUnits())
    {
        if (!target || !target->exists() || !target->getPosition().isValid() || !target->isCompleted())
        {
            continue;
        }

        if (target->isVisible() &&
            (target->isCloaked() ||
            target->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
            target->isBurrowed() ||
            target->getOrder() == BWAPI::Orders::Burrowing))
        {
            cloakedTargets.insert(target);
        }

        if (target->isVisible() && UnitUtil::CanAttackAir(target))
        {
            enemies.insert(target);
        }
    }

    BWAPI::Unitset groundDefenders;
    BWAPI::Unitset airDefenders;
    for (BWAPI::Unit unit : squadUnits)
    {
        if (unit && unit->exists() && unit->getPosition().isValid())
        {
            if (UnitUtil::CanAttackGround(unit) && !unit->getType().isWorker())
            {
                groundDefenders.insert(unit);
            }
            if (UnitUtil::CanAttackAir(unit))
            {
                airDefenders.insert(unit);
            }
        }
    }

    std::vector<BWAPI::Unit> orderedDetectors;
    orderedDetectors.reserve(detectorUnits.size());

    for (const BWAPI::Unit detectorUnit : detectorUnits)
    {
        if (detectorUnit && detectorUnit->exists())
        {
            orderedDetectors.push_back(detectorUnit);
        }
    }

    std::sort(
        orderedDetectors.begin(),
        orderedDetectors.end(),
        [](BWAPI::Unit a, BWAPI::Unit b)
        {
            return a->getID() < b->getID();
        }
    );

    const bool detectionRequestActive =
        _detectionRequestPosition.isValid() &&
        _detectionRequestFrame >= 0 &&
        now >= _detectionRequestFrame &&
        now <= _detectionRequestFrame + 20 * 24 &&
        biggestSquadCenter.isValid() &&
        _detectionRequestPosition.getApproxDistance(
            biggestSquadCenter) <= 12 * 32;

    const bool expansionRequestActive =
        orderedDetectors.size() >= 2 &&
        _expansionDetectionPosition.isValid() &&
        _expansionDetectionFrame >= 0 &&
        now >= _expansionDetectionFrame &&
        now <= _expansionDetectionFrame + 24;

    int expansionDetectorIndex = -1;
    if (expansionRequestActive)
    {
        int bestDistance = MAX_DISTANCE;
        const int firstIndex = detectionRequestActive ? 1 : 0;
        for (int i = firstIndex; i < int(orderedDetectors.size()); ++i)
        {
            const int distance = orderedDetectors[i]->getDistance(
                _expansionDetectionPosition);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                expansionDetectorIndex = i;
            }
        }
    }

    BWAPI::Position detectionTargetPosition =
        _detectionRequestPosition;

    if (detectionRequestActive && !cloakedTargets.empty())
    {
        BWAPI::Unit currentTarget =
            NearestOf(_detectionRequestPosition, cloakedTargets);

        if (currentTarget &&
            currentTarget->getDistance(_detectionRequestPosition) <= 12 * 32)
        {
            detectionTargetPosition = currentTarget->getPosition();
        }
    }

    for (int detectorIndex = 0; detectorIndex < int(orderedDetectors.size()); ++detectorIndex)
    {
        BWAPI::Unit detectorUnit = orderedDetectors[detectorIndex];

        if (!detectorUnit || !detectorUnit->exists())
        {
            continue;
        }

        if (useScienceVesselAbilities(detectorUnit, squadUnits))
        {
            DrawUnitTargetTextMap(
                detectorUnit->getPosition() + BWAPI::Position(0, 8),
                "vessel cast"
            );
            continue;
        }

        if (squadSize == 1 && !detectionRequestActive && cloakedTargets.empty())
        {
            const BWAPI::Position safeHome =
                getScienceVesselDefaultScoutPosition(
                    detectorUnit,
                    order ? order->getPosition() : BWAPI::Positions::Invalid);
            the.micro.MoveSafely(detectorUnit, safeHome);
            continue;
        }

        BWAPI::Position destination;
        BWAPI::Unit nearestEnemy = NearestOf(detectorUnit->getPosition(), enemies);
        BWAPI::Unit nearestGroundDefender = NearestOf(detectorUnit->getPosition(), groundDefenders);
        BWAPI::Unit nearestAirDefender = NearestOf(detectorUnit->getPosition(), airDefenders);
        BWAPI::Unit nearestCloaked = NearestOf(detectorUnit->getPosition(), cloakedTargets);
        const BWAPI::Position cloakedDetectionEdge = nearestCloaked
            ? getScienceVesselDetectionEdgePosition(
                detectorUnit, nearestCloaked->getPosition())
            : BWAPI::Positions::Invalid;

        if (nearestEnemy &&
            detectorUnit->getDistance(nearestEnemy) <= 2 * 32 + UnitUtil::GetAttackRange(nearestEnemy, detectorUnit))
        {
            if (nearestEnemy->isFlying() &&
                nearestAirDefender &&
                detectorUnit->getDistance(nearestAirDefender) <= 12 * 32)
            {
                destination = nearestAirDefender->getPosition();
            }
            else if (!nearestEnemy->isFlying() &&
                nearestGroundDefender &&
                detectorUnit->getDistance(nearestGroundDefender) <= 8 * 32)
            {
                destination = nearestGroundDefender->getPosition();
            }
            else
            {
                destination = DistanceAndDirection(detectorUnit->getPosition(), nearestEnemy->getPosition(), -8 * 32);
            }
        }
        else if (detectorIndex == 0 && detectionRequestActive)
        {
            const int detectionRange =
                UnitUtil::GetDetectionRange(detectorUnit->getType()) * 32;

            if (detectorUnit->getDistance(detectionTargetPosition) <= detectionRange)
            {
                destination = detectorUnit->getPosition();
            }
            else
            {
                const BWAPI::Position detectionEdge =
                    getScienceVesselDetectionEdgePosition(
                        detectorUnit,
                        detectionTargetPosition);

                if (detectionEdge.isValid())
                {
                    destination = detectionEdge;
                }
                else
                {
                    destination = getScienceVesselEscortPosition(
                        detectorUnit,
                        vesselEscortAnchor.isValid()
                            ? vesselEscortAnchor
                            : biggestSquadCenter,
                        order ? order->getPosition() : BWAPI::Positions::Invalid,
                        detectorIndex);
                }
            }
        }
        else if (detectorIndex == expansionDetectorIndex)
        {
            const int detectionRange =
                (UnitUtil::GetDetectionRange(detectorUnit->getType()) - 1) * 32;
            if (detectorUnit->getDistance(_expansionDetectionPosition) <=
                detectionRange)
            {
                destination = detectorUnit->getPosition();
            }
            else
            {
                destination = getScienceVesselDetectionEdgePosition(
                    detectorUnit, _expansionDetectionPosition);
                if (!destination.isValid())
                {
                    destination = getScienceVesselEscortPosition(
                        detectorUnit, vesselEscortAnchor, 
                        order ? order->getPosition() : BWAPI::Positions::Invalid,
                        detectorIndex);
                }
            }
        }
        else if (nearestCloaked &&
            biggestSquadCenter.isValid() &&
            nearestCloaked->getDistance(biggestSquadCenter) <= 10 * 32)
        {
            const int detectionRange =
                (UnitUtil::GetDetectionRange(detectorUnit->getType()) - 1) * 32;
            const BWAPI::Position escort = getScienceVesselEscortPosition(
                detectorUnit, vesselEscortAnchor, 
                order ? order->getPosition() : BWAPI::Positions::Invalid,
                detectorIndex);
            if (escort.isValid() &&
                escort.getApproxDistance(nearestCloaked->getPosition()) <=
                    detectionRange &&
                positionSafeForScienceVessel(detectorUnit, escort))
            {
                destination = escort;
            }
            else
            {
                destination = cloakedDetectionEdge.isValid()
                    ? cloakedDetectionEdge : detectorUnit->getPosition();
            }
        }
        else
        {
            bool cloakedNearArmy = false;
            for (BWAPI::Unit cloaked : cloakedTargets)
            {
                if (biggestSquadCenter.isValid() &&
                    cloaked->getDistance(biggestSquadCenter) <= 12 * 32)
                {
                    cloakedNearArmy = true;
                    break;
                }
            }

            bool airThreatNearFront = false;
            for (BWAPI::Unit enemy : enemies)
            {
                if (vesselEscortAnchor.isValid() &&
                    enemy->getDistance(vesselEscortAnchor) <= 12 * 32)
                {
                    airThreatNearFront = true;
                    break;
                }
            }

            const bool attacking = order &&
                (order->getType() == SquadOrderTypes::Attack ||
                 order->getType() == SquadOrderTypes::OmniAttack);
            const bool allowForwardScout = detectorIndex == 0 && attacking &&
                !detectionRequestActive && !cloakedNearArmy &&
                !airThreatNearFront &&
                detectorUnit->getHitPoints() >= 100;
            destination =
                getScienceVesselScoutDestination(
                    detectorUnit,
                    order ? order->getPosition() : BWAPI::Positions::Invalid,
                    biggestSquadCenter,
                    vesselEscortAnchor,
                    detectorIndex,
                    allowForwardScout
                );

            if (!destination.isValid())
            {
                destination = detectorUnit->getPosition();
            }
        }

        the.micro.MoveNear(detectorUnit, destination);
    }
}
