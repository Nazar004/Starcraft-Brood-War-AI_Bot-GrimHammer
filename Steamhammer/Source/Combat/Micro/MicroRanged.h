#pragma once
#include "Combat/Micro/MicroManager.h"

namespace UAlbertaBot
{
class MicroRanged : public MicroManager
{
private:

public:
    MicroRanged();

    void executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster) override;
    void assignTargets(const BWAPI::Unitset & rangedUnits,
        const BWAPI::Unitset & targets, const UnitCluster & cluster);
    static void defendTerranWall(const BWAPI::Unitset& availableUnits);

    int getAttackPriority(BWAPI::Unit rangedUnit, BWAPI::Unit target);
    BWAPI::Unit getTarget(BWAPI::Unit rangedUnit,
        const BWAPI::Unitset & targets, bool underThreat);
    bool stayHomeUntilReady(const BWAPI::Unit u) const;
};
}
