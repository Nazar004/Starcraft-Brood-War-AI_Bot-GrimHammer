#pragma once

#include "Core/Common.h"
#include "Production/MacroAct.h"

namespace UAlbertaBot
{
    class ProductionGoal
    {
        BWAPI::Unit parent;		// aka producer
        bool attempted;
        int lastAttemptFrame;
        int lastAddonClearFrame;
        int addonBlockedSinceFrame;
        int addonRetryAfterFrame;
        int nextParentSearchFrame;
        int avoidedAddonProducerId;

        bool alreadyAchieved() const;
        bool failure() const;

    public:
        MacroAct act;

        ProductionGoal(const MacroAct & macroAct);

        // True only when BWAPI accepted a production command this frame.
        bool update();

        bool done();

        // A pending Comsat goal owns its Command Center between SCV cycles.
        // Otherwise normal worker production can occupy the center again on
        // every frame and the add-on never receives an idle producer.
        bool reservesProducer(BWAPI::Unit producer) const;
    };

};
