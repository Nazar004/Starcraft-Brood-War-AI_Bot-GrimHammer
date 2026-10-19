#pragma once

#include "Combat/Micro/Terran/BioInfantryFront.h"

namespace UAlbertaBot
{
class MicroManager;

class MicroMelee : public MicroManager
{
    BioInfantryFront _infantryFront;
    const BWAPI::Unitset* _formationUnits = nullptr;

public:

    MicroMelee();

    void executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster);
    void assignTargets(const BWAPI::Unitset & meleeUnits, const BWAPI::Unitset & targets);
    void setFormationUnits(const BWAPI::Unitset & units) { _formationUnits = &units; }

    int getAttackPriority(BWAPI::Unit attacker, BWAPI::Unit unit) const;
    BWAPI::Unit getTarget(BWAPI::Unit meleeUnit, const BWAPI::Unitset & targets, bool underThreat);
    bool meleeUnitShouldRetreat(BWAPI::Unit meleeUnit, const BWAPI::Unitset & targets);
};
}
