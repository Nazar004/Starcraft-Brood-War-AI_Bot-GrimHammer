#pragma once

#include "Combat/Micro/Terran/BioInfantryFront.h"

namespace UAlbertaBot
{
class MicroManager;

class MicroMedics : public MicroManager
{
    BioInfantryFront _infantryFront;
    std::map<int, std::pair<int, int>> _healAssignments;
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
