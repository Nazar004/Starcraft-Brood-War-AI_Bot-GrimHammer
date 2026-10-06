#pragma once

#include "Core/Common.h"
#include "Map/Grids/GridDistances.h"
#include "Combat/Squads/SquadOrder.h"
#include "Core/The.h"

#include <climits>

namespace UAlbertaBot
{
// Support follows the leading infantry band, independently of where a new
// Marine or the supporting unit happens to be. Keep separate island fronts.
class BioInfantryFront
{
    struct Front
    {
        BWAPI::Position position = BWAPI::Positions::Invalid;
        BWAPI::Position goal = BWAPI::Positions::Invalid;
        int updated = -100000;
        int observed = -100000;
    };
    std::map<int, Front> _fronts;
    int _lastFrame = -100000;
    unsigned int _revision = 0;

public:
    void reset() { _fronts.clear(); }

    BWAPI::Position get(const BWAPI::Unitset& units,
        const BWAPI::Position& reference, const SquadOrder& order)
    {
        const BWAPI::Position goal = order.getPosition();
        const int now = the.now();
        if (now < _lastFrame || _revision != the.partitions.getRevision()) reset();
        _lastFrame = now;
        _revision = the.partitions.getRevision();
        if (!reference.isValid() || !goal.isValid()) return BWAPI::Positions::Invalid;
        const int partition = the.partitions.id(reference);
        if (partition <= 0) return BWAPI::Positions::Invalid;

        Front& front = _fronts[partition];
        if (!front.goal.isValid() || front.goal.getApproxDistance(goal) > 8 * 32)
            front = Front();
        front.goal = goal;
        if (now - front.updated < 12) return front.position;
        front.updated = now;

        const GridDistances* distances = order.distances();
        auto distanceToGoal = [distances, &goal](const BWAPI::Position& position)
        {
            if (distances)
            {
                const int tiles = distances->at(position);
                return tiles >= 0 ? 32 * tiles : INT_MAX;
            }
            return the.partitions.connected(position, goal)
                ? position.getApproxDistance(goal) : INT_MAX;
        };

        std::vector<std::pair<int, BWAPI::Unit>> infantry;
        for (BWAPI::Unit unit : units)
        {
            if (!unit || !unit->exists() || !unit->isCompleted() ||
                unit->isLoaded() || !unit->getPosition().isValid() ||
                (unit->getType() != BWAPI::UnitTypes::Terran_Marine &&
                 unit->getType() != BWAPI::UnitTypes::Terran_Firebat &&
                 unit->getType() != BWAPI::UnitTypes::Terran_Ghost) ||
                the.partitions.id(unit->getPosition()) != partition) continue;
            const int distance = distanceToGoal(unit->getPosition());
            if (distance != INT_MAX) infantry.emplace_back(distance, unit);
        }
        std::sort(infantry.begin(), infantry.end(),
            [](const std::pair<int, BWAPI::Unit>& a,
               const std::pair<int, BWAPI::Unit>& b)
            {
                return a.first != b.first ? a.first < b.first
                    : a.second->getID() < b.second->getID();
            });

        BWAPI::Position candidate = BWAPI::Positions::Invalid;
        for (const auto& seed : infantry)
        {
            int nearby = 0;
            for (const auto& member : infantry)
                if (member.second->getDistance(seed.second) <= 6 * 32) ++nearby;
            const bool survivor = front.position.isValid() &&
                seed.second->getDistance(front.position) <= 8 * 32;
            // A lone scout must not pull the army forward. Surviving members
            // of an established front still own that front after casualties.
            if (nearby < 3 && !survivor) continue;

            int x = 0, y = 0, count = 0;
            for (const auto& member : infantry)
            {
                if (member.first > seed.first + 4 * 32 ||
                    member.second->getDistance(seed.second) > 6 * 32) continue;
                x += member.second->getPosition().x;
                y += member.second->getPosition().y;
                if (++count >= 8) break;
            }
            const BWAPI::Position center(x / count, y / count);
            // Use an actual infantry tile: an average can lie in a cliff.
            BWAPI::Unit closest = seed.second;
            for (const auto& member : infantry)
                if (member.first <= seed.first + 4 * 32 &&
                    member.second->getDistance(seed.second) <= 6 * 32 &&
                    member.second->getDistance(center) < closest->getDistance(center))
                    closest = member.second;
            candidate = closest->getPosition();
            break;
        }

        const bool recentFront = front.position.isValid() &&
            now - front.observed < 4 * 24;
        if (recentFront && (!candidate.isValid() ||
            distanceToGoal(candidate) > distanceToGoal(front.position) + 6 * 32))
        {
            // A rear reinforcement group cannot instantly replace a depleted
            // front. Explicit defensive/retreat orders reset this memory.
            return front.position;
        }
        front.position = candidate;
        if (candidate.isValid()) front.observed = now;
        return front.position;
    }
};
}
