#pragma once

#include "Combat/Micro/MicroManager.h"

namespace UAlbertaBot
{
class MicroGhost : public MicroManager
{
    std::map<int, int> _retreatUntilFrame;
    std::map<int, int> _lockdownReservationUntilFrame;
    std::map<int, int> _approachReservationUntilFrame;
    std::map<int, int> _approachTargetByGhost;
    std::map<int, int> _cloakRequestUntilFrame;

    BWAPI::Position getSupportPosition(BWAPI::Unit ghost, const UnitCluster& cluster) const;
    BWAPI::Position getLockdownApproachPosition(BWAPI::Unit ghost, BWAPI::Unit target, const UnitCluster& cluster) const;
    bool targetIsReserved(BWAPI::Unit target, BWAPI::Unit ghost) const;
    bool enemyDetectorNear(BWAPI::Position position, int radius) const;
    bool hasGoliathSupport(BWAPI::Unit ghost, BWAPI::Unit target) const;
    bool cloakForApproach(BWAPI::Unit ghost, BWAPI::Unit target);
    bool cloakForRetreat(BWAPI::Unit ghost);
    void cleanState();

public:
    MicroGhost();

    void executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster) override;
    void updateMovement(const UnitCluster& cluster, BWAPI::Unit vanguard);
    BWAPI::Unit getTarget(BWAPI::Unit ghost, const BWAPI::Unitset & targets) const;
    int getTargetScore(BWAPI::Unit target) const;
    bool canLockdownTarget(BWAPI::Unit ghost, BWAPI::Unit target) const;
};
}
