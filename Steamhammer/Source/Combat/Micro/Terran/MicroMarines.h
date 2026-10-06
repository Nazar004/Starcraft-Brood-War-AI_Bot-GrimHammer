#pragma once

#include "Utilities/UnitUtil.h"
#include "Combat/Micro/Terran/TerranSiegeSafety.h"
#include <algorithm>
#include <cmath>

namespace UAlbertaBot
{
// Marine decisions shared by squad Stim and ranged combat. Header-only so
// the existing Visual Studio project can use it without a new source entry.
class MicroMarines
{
public:
    // Keep Marines outside a building's splash zone while friendly Siege
    // Tanks are shelling it. Distance is measured from the building edge.
    static BWAPI::Position siegeBuildingStandoff(
        BWAPI::Unit marine, BWAPI::Unit building)
    {
        return TerranSiegeSafety::standoff(marine, building);
    }

    static bool shouldStim(BWAPI::Unit marine,
        const BWAPI::Unitset& medics,
        const BWAPI::Unitset& nearbyEnemies)
    {
        if (!marine || !marine->exists() || marine->isStimmed() ||
            marine->getHitPoints() <= 30 || !marine->getPosition().isValid())
        {
            return false;
        }

        bool medicNearby = false;
        for (BWAPI::Unit medic : medics)
        {
            if (medic && medic->exists() && !medic->isLoaded() &&
                medic->getEnergy() >= 5 && medic->getPosition().isValid() &&
                medic->getDistance(marine) <= 7 * 32)
            {
                medicNearby = true;
                break;
            }
        }
        if (!medicNearby && marine->getHitPoints() < 40)
        {
            return false;
        }

        for (BWAPI::Unit enemy : nearbyEnemies)
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                !enemy->isDetected() || enemy->isInvincible() ||
                !enemy->getPosition().isValid() ||
                !UnitUtil::CanAttack(marine, enemy) ||
                (enemy->isUnderDarkSwarm() && !enemy->isFlying()))
            {
                continue;
            }

            // Stim for a fight, not for an Overlord or an empty building.
            if (UnitUtil::CanAttack(enemy, marine) ||
                enemy->getType().isWorker() ||
                enemy->getType() == BWAPI::UnitTypes::Protoss_High_Templar ||
                enemy->getType() == BWAPI::UnitTypes::Zerg_Defiler)
            {
                return true;
            }
        }
        return false;
    }

    static bool shouldKite(BWAPI::Unit marine, BWAPI::Unit target)
    {
        // Stim speed is spent attacking; moving between every volley gives
        // Mutalisks free shots and pulls Marines away from their Medics.
        return marine && target && !marine->isStimmed() &&
            !target->isFlying();
    }
};
}
