#pragma once
#include <map>
#include "Combat/Micro/MicroManager.h"

namespace UAlbertaBot
{
class MicroVultures : public MicroManager
{
private:

    std::map<int, int> _lastRetreatMineFrame;
    std::map<int, int> _mineOrderStartFrame;
    std::map<int, int> _mineOrderStartCount;
    std::map<int, int> _mineRetryBlockedUntil;
    std::map<int, int> _nextStrategicMineCheckFrame;
    int _lastStateFrame = -1;

    void cleanVultureState();
    bool monitorVultureMineOrder(BWAPI::Unit vulture, bool& retryBlocked);

    // Ranged ground weapon does splash damage, so it works under dark swarm.

    bool tryLaySpiderMineOnRetreat(BWAPI::Unit vulture, BWAPI::Unit target);
    bool tryLayStrategicMine(BWAPI::Unit vulture);
    BWAPI::Unit getBestWorkerHarassTarget(BWAPI::Unit rangedUnit) const;

public:

    MicroVultures();

    static bool HasPriorityMineCommand(BWAPI::Unit unit);
    void executeMicro(const BWAPI::Unitset & targets, const UnitCluster & cluster);
    void regroup(const BWAPI::Position & regroupPosition, const UnitCluster & cluster);
    void moveCluster(const UnitCluster & cluster,
        const BWAPI::Unitset & squadUnits,
        const BWAPI::Position & destination,
        bool lazy, bool baseDefenseSquad);
    void attackMove(BWAPI::Unit vulture, const BWAPI::Position & destination);
    void moveSafely(BWAPI::Unit vulture, const BWAPI::Position & destination);
    void assignTargets(
        const BWAPI::Unitset & rangedUnits,
        const BWAPI::Unitset & targets,
        const UnitCluster & cluster);
    void executeStrategicMines(const UnitCluster & cluster);

    int getAttackPriority(BWAPI::Unit rangedUnit, BWAPI::Unit target);
    BWAPI::Unit getTarget(BWAPI::Unit rangedUnit, const BWAPI::Unitset & targets, bool underThreat);

    bool stayHomeUntilReady(const BWAPI::Unit u) const;
};
}
