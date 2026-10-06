#pragma once

#include "Combat/Micro/MicroManager.h"
#include "Combat/Micro/Terran/BioInfantryFront.h"
#include <map>
#include <set>
#include <vector>

namespace UAlbertaBot
{
    class MicroTanks : public MicroManager
    {
    private:

        struct TankDecisionState
        {
            int lastSiegeFrame = -100000;
            int lastUnsiegeFrame = -100000;
            int holdSiegeUntil = -100000;
            int holdTankUntil = -100000;

            int lastSawEnemyFrame = -100000;
            bool wasSiegedLastFrame = false;
            int holdRecentEnemyUntil = -100000;
            int lastCombatStateFrame = -100000;
            int lastDecisionFrame = -100000;

            // Finish a real retreat before allowing a formation/memory branch
            // to deploy again. The destination is fixed for this short step.
            BWAPI::Position retreatPosition = BWAPI::Positions::Invalid;
            int retreatUntil = -100000;

            // Keep a useful target briefly instead of switching every frame.
            // The target is still discarded immediately when it disappears,
            // leaves the available target set or becomes tactically invalid.
            BWAPI::Unit preferredTarget = nullptr;
            int preferredTargetUntil = -100000;

            // Stable mobile-gather slot. Assigning slots by unit id made tanks
            // cross through each other on the way to the rally. Keep an old
            // slot while the gather anchor is stable, and reassign only when
            // the formation actually moves.
            int gatherSlotIndex = -1;
            BWAPI::Position gatherSlotPosition = BWAPI::Positions::Invalid;
            BWAPI::Position gatherSlotAnchor = BWAPI::Positions::Invalid;
            BWAPI::Position gatherSlotForward = BWAPI::Positions::Invalid;

            // A Tank that has reached its gather slot gets at most one tiny
            // forward nudge for this anchor so its final facing points toward
            // the enemy. This is deliberately one-shot: repeating the command
            // was able to create command churn and interrupt Siege deployment.
            BWAPI::Position gatherOrientationAnchor = BWAPI::Positions::Invalid;

            // Rare recovery for a Tank that is physically unable to Siege on
            // its assigned gather slot. The alternate slot is persistent so we
            // do not run a terrain search every frame while the Tank retries.
            BWAPI::Position gatherRecoveryAnchor = BWAPI::Positions::Invalid;
            BWAPI::Position gatherRecoveryPosition = BWAPI::Positions::Invalid;
            int gatherSiegeBlockedSince = -100000;
            int lastGatherRecoveryFrame = -100000;

            int combatSiegeBlockedSince = -100000;
            int combatRecoveryFrame = -100000;
            int combatRecoveryTargetID = -1;
            BWAPI::Position combatRecoveryPosition = BWAPI::Positions::Invalid;

            // Progress watchdog shared by gather and defensive parking. Static
            // connectivity is not enough when another Tank/building occupies
            // the ordered footprint. If distance does not improve, the micro
            // chooses another reachable Siege-capable slot instead of issuing
            // the same impossible Move forever.
            BWAPI::Position parkingProgressTarget = BWAPI::Positions::Invalid;
            int parkingBestDistance = 1000000;
            int parkingLastProgressFrame = -100000;

            // While infantry crosses a tight passage, keep a stable open-terrain
            // pocket on this Tank's own side instead of parking in the lane.
            BWAPI::Position passageYieldPosition = BWAPI::Positions::Invalid;
            BWAPI::Position passageYieldCenter = BWAPI::Positions::Invalid;
            int passageYieldRetryFrame = -100000;
        };

        std::map<BWAPI::Unit, TankDecisionState> _tankState;
        BioInfantryFront _bioInfantryFront;

        // executeMicro() is invoked once for every spatial cluster. Global tank
        // discovery and state cleanup therefore have to be cached per frame.
        int _frameContextFrame = -100000;
        BWAPI::Unitset _cachedActiveTanks;
        BWAPI::Unitset _cachedAttackTanks;
        int _sharedTargetFrame = -100000;
        BWAPI::Unitset _cachedSharedGroundTargets;

        int nThreats(const BWAPI::Unitset& targets) const;
        bool anySiegeUnits(const BWAPI::Unitset& targets) const;
        bool allMeleeAndSameHeight(const BWAPI::Unitset& targets, BWAPI::Unit tank) const;
        TankDecisionState& stateFor(BWAPI::Unit tank);
        void cleanState(const BWAPI::Unitset& activeTanks);
        void refreshCombatState(const BWAPI::Unitset& tanks, const BWAPI::Unitset& targets);

        BWAPI::Position getHoldPosition() const;

        BWAPI::Position getDefensiveFrontTankPosition(
            int index,
            int totalTanks,
            const BWAPI::Position& center,
            const BWAPI::Position& enemyPosition
        ) const;

        void doDefensiveFrontForAttackTanks(
            const BWAPI::Unitset& tanks,
            const BWAPI::Unitset& targets
        );



    public:

        MicroTanks();
        void executeMicro(const BWAPI::Unitset& targets, const UnitCluster& cluster);

        int getAttackPriority(BWAPI::Unit rangedUnit, BWAPI::Unit target);
        BWAPI::Unit getTarget(BWAPI::Unit rangedUnit, const BWAPI::Unitset& targets);
        static void ResetForNewGame();
        static void DrawTacticalInformation();
    };
}
