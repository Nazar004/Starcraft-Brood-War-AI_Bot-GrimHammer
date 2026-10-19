#pragma once

#include "Combat/Micro/Terran/BioInfantryFront.h"

namespace UAlbertaBot
{
class MicroManager;

class MicroMedics : public MicroManager
{
    BioInfantryFront _infantryFront;
    struct HealAssignment
    {
        int patientId;
        int frame;
        bool approaching;
    };
    std::map<int, HealAssignment> _healAssignments;
    bool safeHealApproach(BWAPI::Unit medic, BWAPI::Unit patient,
        BWAPI::Position& waypoint) const;
    std::map<int, std::pair<int, int>> _claimedPatients;
    int _lastFrame = -1;

public:

    MicroMedics();
    void executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster);
    void update(const UnitCluster & cluster, const BWAPI::Unitset & squadUnits,
        const BWAPI::Position& regroupPosition);
    int getTotalEnergy();
};
}
