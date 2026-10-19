#pragma once

#include "Strategy/StrategyManager.h"
#include "Strategy/Terran/TvZBioPlan.h"
#include "Construction/BuildingManager.h"
#include "Core/The.h"
#include "Production/ProductionManager.h"

namespace UAlbertaBot { namespace TvZBioPlan
{
    inline void appendInfantryTechGoals(MetaPairVector& goal, const Policy& policy,
        const BuildOrderQueue& queue)
    {
        if (the.my.completed.count(BWAPI::UnitTypes::Terran_Academy) == 0) return;
        if (!the.self()->hasResearched(BWAPI::TechTypes::Stim_Packs))
        {
            if (!the.self()->isResearching(BWAPI::TechTypes::Stim_Packs) &&
                !queue.anyInNextN(BWAPI::TechTypes::Stim_Packs, int(queue.size())))
                goal.push_back(MetaPair(BWAPI::TechTypes::Stim_Packs, 1));
        }
        else if (policy.push.range &&
            the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::U_238_Shells) == 0 &&
            !the.self()->isUpgrading(BWAPI::UpgradeTypes::U_238_Shells) &&
            !queue.anyInQueue(BWAPI::UpgradeTypes::U_238_Shells))
        {
            goal.push_back(MetaPair(BWAPI::UpgradeTypes::U_238_Shells, 1));
        }
    }

    // Used exclusively by Pre-Hive and SK. Legacy bio/mech retain their own
    // Vessel chain. No Tank count or enemy tech is needed to fund this lane.
    inline void appendSupportGoals(MetaPairVector& goal, const Policy& policy,
        const BuildOrderQueue& queue, int numMarines, int completedBases,
        int numVessels, int trainingVessels)
    {
        auto planned = [&](BWAPI::UnitType type)
        {
            return the.my.all.count(type) +
                int(BuildingManager::Instance().getNumUnstarted(type)) +
                int(queue.numInQueue(type)) +
                ProductionManager::Instance().getPendingGoalCount(type);
        };
        const int ports = policy.starports(numMarines, completedBases);
        if (planned(BWAPI::UnitTypes::Terran_Factory) == 0)
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Factory, 1));
        if (the.my.completed.count(BWAPI::UnitTypes::Terran_Factory) > 0 &&
            planned(BWAPI::UnitTypes::Terran_Starport) < ports)
            goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Starport, ports));
        const int completedPorts = the.my.completed.count(BWAPI::UnitTypes::Terran_Starport);
        if (completedPorts > 0)
        {
            if (planned(BWAPI::UnitTypes::Terran_Control_Tower) < std::min(ports, completedPorts))
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Control_Tower, std::min(ports, completedPorts)));
            if (planned(BWAPI::UnitTypes::Terran_Science_Facility) == 0)
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Science_Facility, 1));
        }
        if (the.my.completed.count(BWAPI::UnitTypes::Terran_Control_Tower) > 0 &&
            the.my.completed.count(BWAPI::UnitTypes::Terran_Science_Facility) > 0)
        {
            // Fund the first Vessel before Irradiate, then research before
            // replenishing the rest of the fleet can consume its gas.
            if (numVessels > 0 || trainingVessels > 0)
            {
                if (!the.self()->hasResearched(BWAPI::TechTypes::Irradiate) &&
                    !the.self()->isResearching(BWAPI::TechTypes::Irradiate) &&
                    !queue.anyInNextN(BWAPI::TechTypes::Irradiate, int(queue.size())))
                    goal.push_back(MetaPair(BWAPI::TechTypes::Irradiate, 1));
            }
            const int target = policy.vessels(numMarines, completedBases);
            if (numVessels < target && trainingVessels < ports)
                goal.push_back(MetaPair(BWAPI::UnitTypes::Terran_Science_Vessel,
                    std::min(target, numVessels + ports - trainingVessels)));
        }
    }
} }
