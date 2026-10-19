#pragma once

#include "Strategy/Terran/AirTanksPolicy.h"
#include "Strategy/Terran/AirTanksWraith.h"
#include "Strategy/Terran/AirTanksValkyrie.h"
#include "Strategy/Terran/AirTanksFastVessel.h"
#include "Strategy/Terran/AirTanksBattlecruiser.h"
#include "Strategy/StrategyManager.h"
#include "Combat/CombatCommander.h"
#include "Construction/BuildingManager.h"
#include "Core/The.h"
#include "Map/Bases.h"
#include "Utilities/UnitUtil.h"

namespace UAlbertaBot { namespace AirTanks
{
    inline int all(BWAPI::UnitType type) { return the.my.all.count(type); }
    inline int completed(BWAPI::UnitType type) { return the.my.completed.count(type); }
    inline int tanks()
    {
        return all(BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) +
            all(BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode);
    }

    inline bool isTank(BWAPI::Unit unit)
    {
        return unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode;
    }

    inline bool mobile(BWAPI::Unit unit)
    {
        return unit && unit->exists() && unit->isCompleted() &&
            unit->getPosition().isValid() && !unit->isLoaded() &&
            !unit->isStasised() && !unit->isLockedDown() && !unit->isMaelstrommed();
    }

    inline Variant variant()
    {
        return variantFromName(StrategyManager::Instance().airTanksVariant());
    }

    inline Push push()
    {
        switch (variant())
        {
        case Variant::Fantasy: return AirTanksValkyrie::push(false);
        case Variant::Valkonic: return AirTanksValkyrie::push(true);
        case Variant::FastVessel: return AirTanksFastVessel::push();
        case Variant::Battlecruiser: return AirTanksBattlecruiser::push();
        default: return AirTanksWraith::push(variant() == Variant::TwoPortWraith);
        }
    }

    inline bool coreTogether()
    {
        using namespace BWAPI::UnitTypes;
        const Push required = push();
        if (!the.self()->hasResearched(BWAPI::TechTypes::Tank_Siege_Mode) ||
            (required.stim && !the.self()->hasResearched(BWAPI::TechTypes::Stim_Packs)) ||
            (required.irradiate && !the.self()->hasResearched(BWAPI::TechTypes::Irradiate)) ||
            (variant() == Variant::Valkonic &&
                the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::U_238_Shells) == 0)) return false;
        const auto& squads = CombatCommander::Instance().squadDiagnostics().getSquads();
        const auto ground = squads.find("Ground"), flying = squads.find("Flying");
        if (ground == squads.end()) return false;
        for (BWAPI::Unit seed : ground->second.getUnits())
        {
            if (!mobile(seed) || !isTank(seed)) continue;
            int nearbyTanks = 0, marines = 0, medics = 0, goliaths = 0;
            for (BWAPI::Unit unit : ground->second.getUnits())
            {
                if (!mobile(unit) || unit->getDistance(seed) > 10 * 32 ||
                    !the.partitions.connected(seed->getPosition(), unit->getPosition())) continue;
                if (isTank(unit)) ++nearbyTanks;
                if (unit->getType() == Terran_Marine) ++marines;
                if (unit->getType() == Terran_Medic && unit->getEnergy() >= 5) ++medics;
                if (unit->getType() == Terran_Goliath) ++goliaths;
            }
            if (nearbyTanks < required.tanks || marines < required.marines ||
                medics < required.medics || goliaths < required.goliaths) continue;
            int wraiths = 0, cruisers = 0, vessels = 0;
            if (flying != squads.end())
                for (BWAPI::Unit unit : flying->second.getUnits())
                {
                    if (!mobile(unit) || unit->getDistance(seed) > 12 * 32) continue;
                    if (unit->getType() == Terran_Wraith) ++wraiths;
                    if (unit->getType() == Terran_Battlecruiser) ++cruisers;
                }
            if (required.vessels > 0)
                for (BWAPI::Unit unit : the.self()->getUnits())
                    if (mobile(unit) && unit->getType() == Terran_Science_Vessel &&
                        unit->getDistance(seed) <= 14 * 32) ++vessels;
            if (wraiths >= required.wraiths && cruisers >= required.battlecruisers &&
                vessels >= required.vessels) return true;
        }
        return false;
    }

    inline bool homeDefenseRequired()
    {
        static int frame = -100000;
        static bool defend = false;
        if (the.now() >= frame && the.now() - frame < 8) return defend;
        frame = the.now();
        defend = false;
        BWAPI::Unitset garrison;
        for (const auto& squad : CombatCommander::Instance().squadDiagnostics().getSquads())
            if (squad.first.compare(0, 5, "Base ") == 0)
                for (BWAPI::Unit unit : squad.second.getUnits()) garrison.insert(unit);
        for (Base* base : the.bases.getAll())
        {
            if (!base || base->getOwner() != the.self()) continue;
            BWAPI::Unit depot = base->getDepot();
            if (!depot || !depot->exists() || !depot->isCompleted() || depot->isFlying()) continue;
            int ground = 0, air = 0, defenseGround = 0, defenseAir = 0;
            for (BWAPI::Unit enemy : the.enemy()->getUnits())
            {
                if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                    !enemy->getPosition().isValid() || enemy->getType().isWorker() ||
                    enemy->getType() == BWAPI::UnitTypes::Protoss_Interceptor ||
                    !UnitUtil::CanAttackGround(enemy) || enemy->getDistance(depot) > 11 * 32) continue;
                if (!enemy->isFlying() &&
                    !the.partitions.connected(enemy->getPosition(), depot->getPosition()) &&
                    !enemy->isInWeaponRange(depot)) continue;
                const int power = std::max(1, enemy->getType().supplyRequired());
                if (enemy->isFlying()) air += power;
                else ground += power;
            }
            for (BWAPI::Unit ally : the.self()->getUnits())
            {
                if (!ally || !ally->exists() || !ally->isCompleted() || ally->isLoaded() ||
                    ally->getDistance(depot) > 10 * 32 ||
                    (!ally->getType().isBuilding() && !garrison.count(ally))) continue;
                const int power = ally->getType() == BWAPI::UnitTypes::Terran_Bunker
                    ? int(ally->getLoadedUnits().size()) * 2
                    : ally->getType().isBuilding() ? 6 : std::max(1, ally->getType().supplyRequired());
                if (UnitUtil::CanAttackGround(ally)) defenseGround += power;
                if (UnitUtil::CanAttackAir(ally)) defenseAir += power;
            }
            const bool critical = 3 * depot->getHitPoints() < depot->getType().maxHitPoints();
            if ((ground >= 6 && ground > defenseGround + 4) ||
                (air >= 6 && air > defenseAir + 4) ||
                (critical && (ground > defenseGround || air > defenseAir))) return defend = true;
        }
        return defend;
    }

    // Independent economic, ground and air lanes. Targets are absolute counts,
    // with bounded army waves so losses never lock out tech or another producer.
    inline MetaPairVector productionGoals(bool expand, int refineries, bool ferry,
        bool mineralEmergency, bool rush)
    {
        using namespace BWAPI::UnitTypes;
        MetaPairVector goal;
        const auto add = [&goal](BWAPI::UnitType type, int count)
        {
            if (count > 0) goal.push_back(MetaPair(type, count));
        };
        const auto tech = [&goal](BWAPI::TechType type)
        {
            if (!the.self()->hasResearched(type) && !the.self()->isResearching(type))
                goal.push_back(MetaPair(type, 1));
        };
        const auto upgrade = [&goal](BWAPI::UpgradeType type, int target)
        {
            const int level = the.self()->getUpgradeLevel(type);
            if (level < target && !the.self()->isUpgrading(type) &&
                (level == 0 || completed(BWAPI::UnitTypes::Terran_Science_Facility) > 0 ||
                 type == BWAPI::UpgradeTypes::U_238_Shells || type == BWAPI::UpgradeTypes::Ion_Thrusters))
                goal.push_back(MetaPair(type, level + 1));
        };
        const Variant selected = variant();
        const bool bio = selected == Variant::Valkonic || selected == Variant::FastVessel;
        const bool fantasy = selected == Variant::Fantasy;
        const bool cruisers = selected == Variant::Battlecruiser;
        const bool fastVessel = selected == Variant::FastVessel;
        const bool wraithPlan = selected == Variant::TankWraith || selected == Variant::TwoPortWraith;
        const Push required = push();
        const int workers = all(Terran_SCV), bases = completed(Terran_Command_Center);
        const int tankCount = tanks(), wraiths = all(Terran_Wraith);
        const int factories = all(Terran_Factory), ports = all(Terran_Starport);
        const int shops = all(Terran_Machine_Shop), towers = all(Terran_Control_Tower);
        const int gas = the.self()->gas(), minerals = the.self()->minerals();
        const bool core = tankCount >= required.tanks && wraiths >= required.wraiths &&
            all(Terran_Marine) >= required.marines && all(Terran_Medic) >= required.medics;
        const bool cloakThreat = InformationManager::Instance().enemyHasCloakTech();
        int mutalisks = 0, antiAir = 0, carriers = 0, battlecruisers = 0, zerglings = 0, combatAir = 0;
        bool spire = false, defilers = false;
        for (const auto& entry : InformationManager::Instance().getUnitData(the.enemy()).getUnits())
        {
            const BWAPI::UnitType type = entry.second.type;
            if (type == Zerg_Spire || type == Zerg_Greater_Spire) spire = true;
            if (type == Zerg_Defiler || type == Zerg_Defiler_Mound) defilers = true;
            if (type.isFlyer() && !type.isBuilding() &&
                (UnitUtil::TypeCanAttackAir(type) || UnitUtil::TypeCanAttackGround(type))) ++combatAir;
            if (type == Zerg_Mutalisk) ++mutalisks;
            if (type == Protoss_Carrier) ++carriers;
            if (type == Terran_Battlecruiser) ++battlecruisers;
            if (type == Zerg_Zergling) ++zerglings;
            if (!type.isWorker() && !type.isBuilding() && UnitUtil::TypeCanAttackAir(type))
                ++antiAir;
        }
        const bool strongAntiAir = antiAir >= 10 && mutalisks < 8;
        const int valkyries = fantasy || selected == Variant::Valkonic
            ? AirTanksValkyrie::airTarget(mutalisks, combatAir, spire)
            : valkyrieTarget(mutalisks, bases);
        const int goliaths = std::min(16, std::max(fantasy ? 4 + 2 * bases : cruisers ? 4 : 0,
            std::max(3 * carriers, 2 * battlecruisers)));
        int tankGoal = cruisers ? AirTanksBattlecruiser::tankTarget(bases)
            : fastVessel ? AirTanksFastVessel::tankTarget(all(Terran_Marine))
            : bio || fantasy ? AirTanksValkyrie::tankTarget(all(Terran_Marine), bio, bases)
            : tankTarget(wraiths, bases, strongAntiAir);
        // A completed one-port composition must still spend late-game supply.
        // Its old 28-Tank ceiling could leave idle Factories at 180/200 while
        // the UI displayed an unrelated pending Ship Weapons upgrade.
        if (selected == Variant::TankWraith && bases >= 2 && workers >= 40 &&
            the.self()->supplyUsed() < 390)
            tankGoal = std::max(tankGoal, std::min(40, tankCount + 2));
        // A mature economy can spend a large surplus on ground reinforcements
        // even when a small defensive air wing has reached its composition cap.
        if (workers >= 36 && minerals >= 1200 && gas >= 600)
            tankGoal = std::min(cruisers ? 12 : selected == Variant::TankWraith ? 40 : 28,
                std::max(tankGoal, tankCount + 2));
        const bool bcTech = cruisers && AirTanksBattlecruiser::techReady(bases, workers,
            tankCount, minerals, gas, all(Terran_Physics_Lab) > 0);
        const bool airTech = fastVessel || fantasy || bio || cruisers ||
            selected == Variant::TwoPortWraith || tankCount >= 2 || ports > 0 || ferry;

        const int unstartedCenters = int(BuildingManager::Instance().getNumUnstarted(
            Terran_Command_Center));
        const int constructingCenters = std::max(0, all(Terran_Command_Center) - bases);
        // One expansion normally owns the economic lane. When a gas-starved
        // AirTanks economy has a large mineral bank, let it start the next
        // site while the first CC is already under construction. Do not post
        // another request while an unstarted task still owns its location.
        const bool ccPending = unstartedCenters > 0 ||
            constructingCenters > (minerals >= 1600 && gas <= 250 ? 1 : 0);
        bool expansionAdded = false;
        if ((mineralEmergency || (minerals >= 1600 && gas <= 250)) &&
            expand && !ccPending)
        {
            add(Terran_Command_Center, all(Terran_Command_Center) + 1);
            expansionAdded = true;
        }
        if (refineries > all(Terran_Refinery))
        {
            WorkerManager::Instance().setCollectGas(true);
            add(Terran_Refinery, refineries);
        }
        add(Terran_SCV, std::min(WorkerManager::Instance().getMaxWorkers(),
            workers + 2 * bases));
        add(Terran_Barracks, bio ? (fastVessel ? AirTanksFastVessel::barracks(workers)
            : AirTanksValkyrie::barracks(workers)) : 1);
        if (bio)
        {
            add(Terran_Academy, 1);
            // Medics precede the next Marine wave, with an independent bounded target.
            if (completed(Terran_Academy) > 0)
            {
                add(Terran_Medic, std::min((all(Terran_Marine) + 4) / 5,
                    all(Terran_Medic) + 2));
                tech(BWAPI::TechTypes::Stim_Packs);
                if (all(Terran_Marine) >= 12) upgrade(BWAPI::UpgradeTypes::U_238_Shells, 1);
            }
            const int marineGoal = fastVessel ? AirTanksFastVessel::marines(workers)
                : AirTanksValkyrie::marines(workers);
            add(Terran_Marine, std::min(marineGoal,
                all(Terran_Marine) + std::max(1, completed(Terran_Barracks))));
            add(Terran_Engineering_Bay, bases >= 2 && workers >= 32 && all(Terran_Marine) >= 24 ? 2 : 1);
        }
        else add(Terran_Marine, rush ? 8 : 4);
        if (rush) add(Terran_Bunker, 1);
        add(Terran_Factory, std::max(factories, factoryTarget(workers, bases)));
        if (completed(Terran_Factory) > 0)
            add(Terran_Machine_Shop, std::max(shops, std::max(1, completed(Terran_Factory))));
        if (completed(Terran_Machine_Shop) > 0) tech(BWAPI::TechTypes::Tank_Siege_Mode);

        if (airTech)
        {
            const int portGoal = bcTech ? AirTanksBattlecruiser::starports(bases)
                : selected == Variant::TwoPortWraith ? (bases >= 3 && workers >= 48 ? 3 : 2)
                : (fantasy || bio || cruisers) ? (bases >= 3 && workers >= 40 ? 2 : 1)
                : starportTarget(workers, bases, tankCount, wraiths, gas);
            add(Terran_Starport, std::max(ports, portGoal));
        }
        if (completed(Terran_Starport) > 0)
            add(Terran_Control_Tower, std::max(towers, completed(Terran_Starport)));

        // Fund urgent detection before optional air reinforcement/cloak.
        const int vessels = fastVessel ? AirTanksFastVessel::vessels(bases)
            : defilers ? 3 : cloakThreat ? 2 :
            (core && bases >= 2) || bio ? 1 : 0;
        if (vessels > 0)
        {
            add(Terran_Academy, 1);
            if (completed(Terran_Academy) > 0) add(Terran_Comsat_Station, std::max(1, bases));
            if (completed(Terran_Starport) > 0) add(Terran_Science_Facility, 1);
            if (completed(Terran_Science_Facility) > 0 && completed(Terran_Control_Tower) > 0)
            {
                // Fund the first Irradiate after one Vessel, before a fleet of casters.
                const bool irradiate = (fastVessel || defilers ||
                    (the.enemyRace() == BWAPI::Races::Zerg && all(Terran_Science_Vessel) > 0));
                if (irradiate && all(Terran_Science_Vessel) > 0) tech(BWAPI::TechTypes::Irradiate);
                add(Terran_Science_Vessel, irradiate && all(Terran_Science_Vessel) > 0 &&
                    !the.self()->hasResearched(BWAPI::TechTypes::Irradiate) ? 1
                    : std::min(vessels, all(Terran_Science_Vessel) + 1));
            }
        }
        if (ferry && completed(Terran_Control_Tower) > 0) add(Terran_Dropship, 1);

        if (valkyries > 0 || goliaths > 0 || core || bcTech) add(Terran_Armory, 1);
        if (bcTech)
        {
            if (completed(Terran_Starport) > 0) add(Terran_Science_Facility, 1);
            if (completed(Terran_Science_Facility) > 0) add(Terran_Physics_Lab, 1);
            if (completed(Terran_Physics_Lab) > 0 && completed(Terran_Control_Tower) > 0)
            {
                // Yamato is useful immediately; energy capacity can wait for a fleet.
                tech(BWAPI::TechTypes::Yamato_Gun);
                if (completed(Terran_Armory) > 0) upgrade(BWAPI::UpgradeTypes::Terran_Ship_Weapons, 1);
                add(Terran_Battlecruiser, std::min(AirTanksBattlecruiser::fleetTarget(bases),
                    all(Terran_Battlecruiser) + std::max(1, completed(Terran_Starport))));
                if (all(Terran_Battlecruiser) >= 4 && gas >= 450)
                    upgrade(BWAPI::UpgradeTypes::Colossus_Reactor, 1);
            }
        }
        if (completed(Terran_Armory) > 0)
        {
            add(Terran_Goliath, std::min(goliaths, all(Terran_Goliath) + 2));
            if (goliaths > 0 && completed(Terran_Machine_Shop) > 0 &&
                the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) == 0 &&
                !the.self()->isUpgrading(BWAPI::UpgradeTypes::Charon_Boosters))
                goal.push_back(MetaPair(BWAPI::UpgradeTypes::Charon_Boosters, 1));
            if (completed(Terran_Control_Tower) > 0)
                add(Terran_Valkyrie, std::min(valkyries, all(Terran_Valkyrie) + 2));
        }

        // Give an underfilled air core its first job before the Tank wave.
        const auto airWave = [&]()
        {
            if (wraithPlan && completed(Terran_Starport) > 0)
                add(Terran_Wraith, std::min(AirTanksWraith::airTarget(tankCount, strongAntiAir),
                    wraiths + std::max(1, completed(Terran_Starport))));
        };
        const bool twoPortVultureSurplus = selected == Variant::TwoPortWraith &&
            tankCount >= FirstPushTanks && minerals >= 700 && gas < 200;
        const auto vultureWave = [&]()
        {
            if (selected != Variant::TankWraith &&
                selected != Variant::TwoPortWraith) return;
            // The two-port opening keeps its first Tank core. After that,
            // mineral-only Vultures use Factory time while air and Tanks wait
            // for gas; the one-port variant keeps its existing timing.
            if (selected == Variant::TwoPortWraith &&
                tankCount < FirstPushTanks) return;
            const int vultureGoal = tankWraithVultureTarget(tankCount, minerals, gas);
            if (vultureGoal > all(Terran_Vulture))
                add(Terran_Vulture, std::min(vultureGoal,
                    all(Terran_Vulture) + std::max(1, completed(Terran_Factory))));
        };
        if (wraiths < FirstPushWraiths) airWave();
        if (twoPortVultureSurplus) vultureWave();
        if (completed(Terran_Machine_Shop) > 0)
            add(Terran_Siege_Tank_Tank_Mode, std::min(tankGoal,
                tankCount + std::max(1, completed(Terran_Machine_Shop))));
        if (wraiths >= FirstPushWraiths) airWave();

        if (wraithPlan)
        {
            if (!twoPortVultureSurplus) vultureWave();
            // Mines are valuable in TvT, but do not spend the gas needed to
            // establish the first Tank/Wraith core.
            if (all(Terran_Vulture) >= 4 && tankCount >= 4 && gas >= 250 &&
                completed(Terran_Machine_Shop) > 0)
                tech(BWAPI::TechTypes::Spider_Mines);
        }

        // Mines are a ground screen against mass Zerglings, not the default army.
        if ((fantasy || zerglings >= 8) && tankCount >= 2 && !bio)
        {
            add(Terran_Vulture, std::min(8, all(Terran_Vulture) + 2));
            if (completed(Terran_Machine_Shop) > 0)
            {
                tech(BWAPI::TechTypes::Spider_Mines);
                if (all(Terran_Vulture) >= 4) upgrade(BWAPI::UpgradeTypes::Ion_Thrusters, 1);
            }
        }
        if (!expansionAdded && expand && !ccPending && (bases < 2 ||
            the.now() >= ThirdMinute * 24 * 60 || mineralEmergency))
            add(Terran_Command_Center, all(Terran_Command_Center) + 1);

        // Cloak is optional; losing a Control Tower never blocks the first push.
        if (wraithPlan && wraiths >= 2 && gas >= 200 && completed(Terran_Control_Tower) > 0)
            tech(BWAPI::TechTypes::Cloaking_Field);
        // Upgrade by actual army investment; never research empty unit branches.
        if (minerals >= 250 && gas >= 250)
        {
            if (bio && all(Terran_Marine) >= 16 && completed(Terran_Engineering_Bay) > 0)
            {
                upgrade(BWAPI::UpgradeTypes::Terran_Infantry_Weapons, 3);
                if (the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::Terran_Infantry_Weapons) >= 1)
                    upgrade(BWAPI::UpgradeTypes::Terran_Infantry_Armor, 3);
            }
            if (completed(Terran_Armory) > 0)
            {
                if (tankCount >= 4) upgrade(BWAPI::UpgradeTypes::Terran_Vehicle_Weapons, 3);
                if (wraiths >= 4 || all(Terran_Valkyrie) >= 2 || all(Terran_Battlecruiser) > 0)
                    upgrade(BWAPI::UpgradeTypes::Terran_Ship_Weapons, 3);
                if (all(Terran_Battlecruiser) >= 2 || all(Terran_Valkyrie) >= 3)
                    upgrade(BWAPI::UpgradeTypes::Terran_Ship_Plating, 3);
                if (tankCount >= 8) upgrade(BWAPI::UpgradeTypes::Terran_Vehicle_Plating, 3);
            }
            if (tankCount >= 6 && bases >= 2 && completed(Terran_Starport) > 0)
                add(Terran_Science_Facility, 1);
            if (bases >= 3 && workers >= 44 && tankCount >= 8)
                add(Terran_Armory, 2);
        }
        return goal;
    }
} }
