#pragma once

#include "Strategy/Terran/BioTankPolicy.h"
#include "Strategy/BioProductionPolicy.h"
#include "Combat/CombatCommander.h"
#include "Intelligence/InformationManager.h"
#include "Map/Bases.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"

namespace UAlbertaBot { namespace BioTankPolicy
{
    struct EnemyMix
    {
        int zerglings = 0, heavyGround = 0, lightGround = 0, air = 0;
        int carriers = 0, battlecruisers = 0, guardians = 0, mutalisks = 0;
        int antiAir = 0;
    };

    inline const EnemyMix& enemyMix()
    {
        static int frame = -100000;
        static EnemyMix mix;
        if (the.now() >= frame && the.now() - frame < 12) return mix;
        frame = the.now();
        mix = EnemyMix();
        // UnitData removes confirmed deaths; "ever seen" would keep ordering
        // counters for a dead fleet. Refresh once per half second.
        for (const auto& entry :
            InformationManager::Instance().getUnitData(the.enemy()).getUnits())
        {
            const BWAPI::UnitType type = entry.second.type;
            if (type.isWorker() || type.isBuilding() || type.isSpell()) continue;
            if (type == BWAPI::UnitTypes::Zerg_Zergling) ++mix.zerglings;
            if (type == BWAPI::UnitTypes::Protoss_Carrier) ++mix.carriers;
            if (type == BWAPI::UnitTypes::Terran_Battlecruiser) ++mix.battlecruisers;
            if (type == BWAPI::UnitTypes::Zerg_Guardian) ++mix.guardians;
            if (type == BWAPI::UnitTypes::Zerg_Mutalisk) ++mix.mutalisks;
            if (UnitUtil::TypeCanAttackAir(type)) ++mix.antiAir;
            if (type.isFlyer())
            {
                if (UnitUtil::TypeCanAttackGround(type) || UnitUtil::TypeCanAttackAir(type))
                    ++mix.air;
            }
            else if (type == BWAPI::UnitTypes::Zerg_Hydralisk ||
                type == BWAPI::UnitTypes::Zerg_Lurker ||
                type == BWAPI::UnitTypes::Zerg_Ultralisk ||
                type == BWAPI::UnitTypes::Protoss_Dragoon ||
                type == BWAPI::UnitTypes::Protoss_Reaver ||
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
                type == BWAPI::UnitTypes::Terran_Goliath) ++mix.heavyGround;
            else if (UnitUtil::TypeCanAttackGround(type)) ++mix.lightGround;
        }
        return mix;
    }

    inline int goliathTarget(bool carrierPreparation = false)
    {
        const EnemyMix& mix = enemyMix();
        return std::max(carrierPreparation ? 4 : 0,
            goliaths(mix.carriers, mix.battlecruisers,
                guardianWraiths(mix.guardians, mix.antiAir) > 0 ? 0 : mix.guardians));
    }

    inline bool reserveGoliathSupply()
    {
        return StrategyManager::Instance().getTerranPlan() == TerranPlanId::BioTank &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Marine) >= 16 &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
                the.my.completed.count(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) >= 2 &&
            the.my.all.count(BWAPI::UnitTypes::Terran_Goliath) < goliathTarget() &&
            the.self()->supplyTotal() - the.self()->supplyUsed() <
                2 * BWAPI::UnitTypes::Terran_Goliath.supplyRequired();
    }

    inline bool coreTogether(int marineTarget, int tankTarget)
    {
        static int frame = -100000, lastMarines = 0, lastTanks = 0;
        static bool ready = false;
        if (the.now() >= frame && the.now() - frame < 12 &&
            marineTarget == lastMarines && tankTarget == lastTanks) return ready;
        frame = the.now();
        lastMarines = marineTarget;
        lastTanks = tankTarget;
        ready = false;
        const auto& squads = CombatCommander::Instance().squadDiagnostics().getSquads();
        const auto ground = squads.find("Ground");
        if (ground == squads.end()) return false;
        BWAPI::Unitset mobile;
        // Bunkers, local defense, scouts and drops cannot satisfy this gate.
        for (BWAPI::Unit unit : ground->second.getUnits())
            if (unit && unit->exists() && unit->isCompleted() && !unit->isLoaded() &&
                unit->getPosition().isValid() && !unit->isStasised() &&
                !unit->isMaelstrommed() && !unit->isLockedDown()) mobile.insert(unit);
        for (BWAPI::Unit seed : mobile)
        {
            if (seed->getType() != BWAPI::UnitTypes::Terran_Marine) continue;
            int marines = 0, medics = 0, tanks = 0;
            for (BWAPI::Unit unit : mobile)
            {
                if (unit->getDistance(seed) > 10 * 32 ||
                    !the.partitions.connected(seed->getPosition(), unit->getPosition())) continue;
                const BWAPI::UnitType type = unit->getType();
                if (type == BWAPI::UnitTypes::Terran_Marine) ++marines;
                else if (type == BWAPI::UnitTypes::Terran_Medic && unit->getEnergy() >= 5) ++medics;
                else if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                    type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) ++tanks;
            }
            if (marines >= marineTarget &&
                medics >= BioProductionPolicy::medicTarget(marineTarget) &&
                tanks >= tankTarget) return ready = true;
        }
        return false;
    }

    inline bool canThreatenBase(BWAPI::Unit enemy, Base* base, int maxGroundDistance)
    {
        if (!enemy || !enemy->exists() || !enemy->getPosition().isValid() ||
            !base || !base->getDepot() || !base->getDepot()->exists()) return false;
        if (enemy->isFlying()) return true;
        BWAPI::Unit depot = base->getDepot();
        // Guns can fire across cliffs even where walking is impossible.
        if (UnitUtil::CanAttackGround(enemy) && enemy->getDistance(depot) <=
            UnitUtil::GetAttackRange(enemy, depot) + 64) return true;
        if (!the.partitions.connected(enemy->getPosition(), base->getPosition())) return false;
        // All attackers share the cached field rooted at this base.
        const int distance = the.map.getGroundDistance(enemy->getPosition(), base->getPosition());
        return distance >= 0 && distance <= maxGroundDistance;
    }

    inline bool homeDefenseRequired()
    {
        static int frame = -100000;
        static bool required = false;
        if (the.now() >= frame && the.now() - frame < 8) return required;
        frame = the.now();
        required = false;
        BWAPI::Unitset garrison;
        for (const auto& squad : CombatCommander::Instance().squadDiagnostics().getSquads())
            if (squad.first.compare(0, 5, "Base ") == 0)
                for (BWAPI::Unit unit : squad.second.getUnits()) garrison.insert(unit);
        auto power = [](BWAPI::Unit unit)
        {
            const BWAPI::UnitType type = unit->getType();
            int value = std::max(1, type.supplyRequired());
            if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
                type == BWAPI::UnitTypes::Zerg_Lurker ||
                type == BWAPI::UnitTypes::Protoss_Reaver) value += 4;
            if (type.isBuilding()) value = 6;
            return std::max(1, value * unit->getHitPoints() /
                std::max(1, type.maxHitPoints()));
        };
        for (Base* base : the.bases.getAll())
        {
            if (!base || base->getOwner() != the.self() || !base->isCompleted()) continue;
            BWAPI::Unit depot = base->getDepot();
            if (!depot || !depot->exists() || depot->isFlying()) continue;
            int groundPower = 0, airPower = 0, groundDefense = 0, airDefense = 0;
            for (BWAPI::Unit enemy : the.enemy()->getUnits())
            {
                if (!enemy || !enemy->exists() || !enemy->isVisible() || enemy->isInvincible() ||
                    !enemy->getPosition().isValid() || enemy->getType().isWorker() ||
                    !UnitUtil::CanAttackGround(enemy) ||
                    enemy->getType() == BWAPI::UnitTypes::Protoss_Interceptor ||
                    enemy->getDistance(depot) > 12 * 32 ||
                    !canThreatenBase(enemy, base, 14 * 32)) continue;
                if (enemy->isFlying()) airPower += power(enemy);
                else groundPower += power(enemy);
            }
            if (groundPower == 0 && airPower == 0) continue;
            for (BWAPI::Unit unit : the.self()->getUnits())
            {
                if (!unit || !unit->exists() || !unit->isCompleted() || unit->isLoaded() ||
                    unit->isStasised() || unit->isLockedDown() || unit->isMaelstrommed() ||
                    !unit->getPosition().isValid() || unit->getType().isWorker() ||
                    unit->getDistance(depot) > 10 * 32 ||
                    (!unit->isFlying() && !the.partitions.connected(
                        unit->getPosition(), depot->getPosition()))) continue;
                if (unit->getType() == BWAPI::UnitTypes::Terran_Bunker)
                {
                    for (BWAPI::Unit passenger : unit->getLoadedUnits())
                    {
                        if (!passenger) continue;
                        if (UnitUtil::CanAttackGround(passenger)) groundDefense += power(passenger);
                        if (UnitUtil::CanAttackAir(passenger)) airDefense += power(passenger);
                    }
                }
                else if (unit->getType().isBuilding() || garrison.find(unit) != garrison.end())
                {
                    // The returning main army is temporary help, not a local
                    // garrison: counting it would toggle attack/recall forever.
                    if (UnitUtil::CanAttackGround(unit)) groundDefense += power(unit);
                    if (UnitUtil::CanAttackAir(unit)) airDefense += power(unit);
                }
            }
            const bool critical = depot->getHitPoints() < depot->getType().maxHitPoints() / 3;
            if (recallArmy(groundPower, groundDefense, critical) ||
                recallArmy(airPower, airDefense, critical))
            {
                required = true;
                break;
            }
        }
        return required;
    }
} }
