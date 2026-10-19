#pragma once

#include "Strategy/StrategyManager.h"
#include "Strategy/Terran/AirTanksPolicy.h"
#include "Strategy/Terran/AirTanksFormation.h"
#include "Combat/CombatCommander.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"
#include <map>
#include <algorithm>

namespace UAlbertaBot
{
class AirTanksMicro
{
public:
    static bool act(BWAPI::Unit unit, const BWAPI::Unitset& targets)
    {
        if (!StrategyManager::Instance().airTanksPlanActive() || !unit || !unit->exists())
            return false;
        const auto type = unit->getType();
        if (type != BWAPI::UnitTypes::Terran_Wraith && type != BWAPI::UnitTypes::Terran_Valkyrie &&
            type != BWAPI::UnitTypes::Terran_Battlecruiser) return false;

        if (type == BWAPI::UnitTypes::Terran_Battlecruiser &&
            (unit->getOrder() == BWAPI::Orders::FireYamatoGun || !unit->isInterruptible() ||
             (unit->getLastCommand().getTechType() == BWAPI::TechTypes::Yamato_Gun &&
              the.now() - unit->getLastCommandFrame() <= BWAPI::Broodwar->getRemainingLatencyFrames() + 6)))
            return true;

        // Scourge can be hidden in a Mutalisk stack. Keep fragile air near a screen.
        if (type != BWAPI::UnitTypes::Terran_Battlecruiser)
        {
            BWAPI::Unit scourge = nullptr;
            int distance = 5 * 32;
            for (BWAPI::Unit enemy : targets)
                if (enemy && enemy->exists() && enemy->isVisible() &&
                    enemy->getType() == BWAPI::UnitTypes::Zerg_Scourge &&
                    unit->getDistance(enemy) < distance)
                {
                    scourge = enemy;
                    distance = unit->getDistance(enemy);
                }
            if (scourge)
            {
                BWAPI::Unit screen = nullptr;
                int nearest = 10 * 32;
                for (BWAPI::Unit ally : the.self()->getUnits())
                {
                    if (!ally || !ally->exists() || !ally->isCompleted() || ally->isLoaded() ||
                        ally->isFlying() || !UnitUtil::CanAttackAir(ally)) continue;
                    const int d = unit->getDistance(ally);
                    if (d < nearest && ally->getDistance(scourge) > distance + 48)
                    {
                        nearest = d;
                        screen = ally;
                    }
                }
                const BWAPI::Position origin = unit->getPosition();
                BWAPI::Position escape = screen ? screen->getPosition()
                    : BWAPI::Position(origin.x + 2 * (origin.x - scourge->getPosition().x),
                        origin.y + 2 * (origin.y - scourge->getPosition().y));
                escape.makeValid();
                if (escape != origin)
                {
                    the.micro.MoveSafely(unit, escape);
                    return true;
                }
            }
        }

        if (type == BWAPI::UnitTypes::Terran_Wraith && !unit->isCloaked() &&
            unit->getEnergy() >= 50 && the.self()->hasResearched(BWAPI::TechTypes::Cloaking_Field))
        {
            bool threatened = false, detector = false;
            for (BWAPI::Unit enemy : the.enemy()->getUnits())
            {
                if (!enemy || !enemy->exists() || !enemy->isVisible()) continue;
                if ((enemy->getType().isDetector() ||
                    enemy->getType() == BWAPI::UnitTypes::Spell_Scanner_Sweep) &&
                    unit->getDistance(enemy) <= enemy->getType().sightRange() + 64) detector = true;
                if (UnitUtil::CanAttackAir(enemy) && unit->getDistance(enemy) <= 7 * 32)
                    threatened = true;
            }
            if (threatened && !detector && unit->canCloak() && unit->cloak()) return true;
        }

        if (type == BWAPI::UnitTypes::Terran_Wraith &&
            AirTanks::variantFromName(StrategyManager::Instance().airTanksVariant()) ==
                AirTanks::Variant::TankWraith &&
            !CombatCommander::Instance().airTanksWraithRaidActive())
        {
            const SquadData& squads = CombatCommander::Instance().squadDiagnostics();
            const Squad* owner = squads.getUnitSquad(unit);
            const auto ground = squads.getSquads().find("Ground");
            if (owner && owner->getName() == "Flying" &&
                ground != squads.getSquads().end())
            {
                BWAPI::Unit leadTank =
                    AirTanksFormation::frontTank(ground->second.getUnits());
                // The Flying order leads the Tank line. This leash stops a
                // target chase or kiting step from becoming an independent raid.
                if (leadTank && unit->getDistance(leadTank) > 8 * 32)
                {
                    if (!unit->isAttackFrame() && !unit->isStartingAttack() &&
                        the.now() - unit->getLastCommandFrame() >= 8)
                        the.micro.MoveSafely(unit, leadTank->getPosition());
                    return true;
                }
            }
        }

        if (type != BWAPI::UnitTypes::Terran_Battlecruiser || unit->getEnergy() < 150 ||
            !the.self()->hasResearched(BWAPI::TechTypes::Yamato_Gun)) return false;
        // Include ongoing shots so different frames do not all spend energy on one victim.
        static int reservationFrame = -1;
        static std::map<int, int> reservedDamage;
        if (reservationFrame != the.now())
        {
            reservationFrame = the.now();
            reservedDamage.clear();
            for (BWAPI::Unit ally : the.self()->getUnits())
            {
                if (!ally || !ally->exists() || ally->getType() != type) continue;
                BWAPI::Unit victim = nullptr;
                if (ally->getOrder() == BWAPI::Orders::FireYamatoGun) victim = ally->getOrderTarget();
                else if (ally->getLastCommand().getTechType() == BWAPI::TechTypes::Yamato_Gun &&
                    the.now() - ally->getLastCommandFrame() <= BWAPI::Broodwar->getRemainingLatencyFrames() + 6)
                    victim = ally->getLastCommand().getTarget();
                if (victim && victim->exists()) reservedDamage[victim->getID()] += 260;
            }
        }
        BWAPI::Unit best = nullptr;
        int bestScore = 0;
        for (BWAPI::Unit target : targets)
        {
            if (!target || !target->exists() || target->getPlayer() != the.enemy() ||
                !target->isVisible() || !target->isDetected() ||
                target->isInvincible() || target->isStasised() || target->getType().isWorker() ||
                target->getHitPoints() + target->getShields() < 160 ||
                unit->getDistance(target) > 10 * 32 ||
                reservedDamage[target->getID()] >= target->getHitPoints() + target->getShields() ||
                !unit->canUseTech(BWAPI::TechTypes::Yamato_Gun, target)) continue;
            const auto targetType = target->getType();
            const int score = (targetType == BWAPI::UnitTypes::Protoss_Carrier ||
                targetType == BWAPI::UnitTypes::Terran_Battlecruiser ? 500 :
                UnitUtil::CanAttackAir(target) ? 250 : 100) +
                std::min(260, target->getHitPoints() + target->getShields());
            if (score > bestScore) { bestScore = score; best = target; }
        }
        if (best && the.micro.UseTech(unit, BWAPI::TechTypes::Yamato_Gun, best))
        {
            reservedDamage[best->getID()] += 260;
            return true;
        }
        return false;
    }
};
}
