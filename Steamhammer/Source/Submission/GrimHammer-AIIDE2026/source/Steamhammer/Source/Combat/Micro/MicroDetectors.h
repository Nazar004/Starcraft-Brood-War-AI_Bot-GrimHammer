#pragma once

#include "Combat/Micro/MicroManager.h"
#include <map>


namespace UAlbertaBot
{
    class MicroDetectors : public MicroManager
    {
        int squadSize;
        BWAPI::Unit unitClosestToTarget;

        struct PendingSpell
        {
            BWAPI::TechType tech;
            int targetId;
            int energyBefore;
            int deadline;
            int completedFrame;
        };
        std::map<int, PendingSpell> _pendingSpells;
        std::map<int, int> _lastSpellCastFrame;
        int _lastStateFrame;
        BWAPI::Position _detectionRequestPosition;
        int _detectionRequestFrame;
        BWAPI::Position _expansionDetectionPosition;
        int _expansionDetectionFrame;

        bool canCastAnotherSpell(BWAPI::Unit vessel) const;
        bool spellPending(BWAPI::Unit vessel);
        void rememberSpellCast(BWAPI::Unit vessel, BWAPI::TechType tech,
            BWAPI::Unit target, const BWAPI::Position& position, int energyBefore);

        bool useScienceVesselAbilities(BWAPI::Unit vessel, const BWAPI::Unitset& squadUnits);

        bool tryDefensiveMatrix(BWAPI::Unit vessel, const BWAPI::Unitset& squadUnits);
        bool tryIrradiate(BWAPI::Unit vessel);
        bool tryEMP(BWAPI::Unit vessel);

        BWAPI::Unit getBestDefensiveMatrixTarget(BWAPI::Unit vessel, const BWAPI::Unitset& squadUnits) const;
        BWAPI::Unit getBestIrradiateTarget(BWAPI::Unit vessel) const;
        BWAPI::Position getBestEMPPosition(BWAPI::Unit vessel) const;

        int scoreDefensiveMatrixTarget(BWAPI::Unit vessel, BWAPI::Unit unit) const;
        int scoreIrradiateTarget(BWAPI::Unit vessel, BWAPI::Unit enemy) const;
        int scoreEMPAtPosition(BWAPI::Unit vessel, BWAPI::Position pos) const;

    public:
        MicroDetectors();

        void executeMicro(const BWAPI::Unitset& targets, const UnitCluster& cluster);
        void go(const BWAPI::Unitset& squadUnits);

        void setSquadSize(int size) { squadSize = size; }
        void setUnitClosestToTarget(BWAPI::Unit unit) { unitClosestToTarget = unit; }
        void setDetectionRequest(const BWAPI::Position& position, int frame)
        {
            _detectionRequestPosition = position;
            _detectionRequestFrame = frame;
        }
        void setExpansionDetectionRequest(const BWAPI::Position& position, int frame)
        {
            _expansionDetectionPosition = position;
            _expansionDetectionFrame = frame;
        }
    };
}
