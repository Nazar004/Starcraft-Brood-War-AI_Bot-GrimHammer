#pragma once

#include "Combat/Micro/MicroManager.h"

namespace UAlbertaBot
{
    class MicroGoliath : public MicroManager
    {
    private:
        const BWAPI::Unitset* _formationUnits;
        static bool goliathShouldIgnoreInterceptor(BWAPI::Unit goliath, BWAPI::Unit interceptor);

    public:
        MicroGoliath();

        void setFormationUnits(const BWAPI::Unitset& units) { _formationUnits = &units; }

        void executeMicro(const BWAPI::Unitset& targets, const UnitCluster& cluster) override;
        void assignTargets(
            const BWAPI::Unitset& goliathUnits,
            const BWAPI::Unitset& targets,
            const UnitCluster& cluster);

        BWAPI::Unit getTarget(BWAPI::Unit goliath, const BWAPI::Unitset& targets, bool underThreat) const;
        int getAttackPriority(BWAPI::Unit goliath, BWAPI::Unit target) const;
    };
}
