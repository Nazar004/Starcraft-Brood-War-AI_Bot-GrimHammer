#include "Intelligence/OpponentPlan.h"

#include "Map/Bases.h"
#include "Intelligence/InformationManager.h"
#include "Intelligence/PlayerSnapshot.h"
#include "Core/The.h"

using namespace UAlbertaBot;

// Attempt to recognize what the opponent is doing, so we can cope with it.
// For now, only try to recognize a small number of opening situations that require
// different handling.

// This is part of the OpponentModel module. Access should normally be through the OpponentModel instance.

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// Assume that the unit started at the given base (at the location of the resource depot).
// About how long (in frames) did the unit need to reach its current position?
// If the path appears unwalkable, return -1.
int OpponentPlan::travelTime(BWAPI::UnitType unitType, const BWAPI::Position & pos, const Base * base) const
{
    int pixelDistance = base->getDistance(pos);
    if (pixelDistance < 0)
    {
        return -1;
    }

    // NOTE This is called only for units that are known to move!
    //      Otherwise there would be a risk of division by zero.
    UAB_ASSERT(unitType.topSpeed() > 0.0, "immobile unit");
    // NOTE No need to call round(), one frame off is more than good enough.
    return int(pixelDistance / unitType.topSpeed());
}

// The location of the enemy base is not known. Find the closest possibility.
// NOTE If the position is on an unwalkable or partially-walkable tile, this
//      will return null. It can't find the closest base.
Base * OpponentPlan::closestEnemyBase(const BWAPI::Position & pos) const
{
    int bestDist = MAX_DISTANCE;
    Base * bestBase = nullptr;

    for (Base * base : the.bases.getStarting())
    {
        // An explored base can't be the enemy's base, or we'd know it.
        if (base->getOwner() == the.neutral() && !base->isExplored())
        {
            int dist = base->getTileDistance(pos);
            if (dist >= 0 && dist < bestDist)
            {
                bestDist = dist;
                bestBase = base;
            }
        }
    }
    return bestBase;
}

// Does this enemy building imply that we are facing a fast rush?
// Check for an early completion time.
bool OpponentPlan::rushBuilding(const UnitInfo & ui) const
{
    return
        ui.type == BWAPI::UnitTypes::Terran_Barracks && ui.completeBy < 2375 ||  // probably 7 rax or earlier
        ui.type == BWAPI::UnitTypes::Protoss_Gateway && ui.completeBy < 2540 ||  // probably 7 gate or earlier
        ui.type == BWAPI::UnitTypes::Zerg_Spawning_Pool && ui.completeBy < 2675; // probably 8 pool or earlier
}

// Return whether the enemy has a building in our main or natural base.
// Returns the plan as Proxy, Contain for a more distant defense building, or Unknown for none.
OpeningPlan OpponentPlan::recognizeProxy() const
{
    int mainZone = the.zone.areaIDAt(the.bases.myStart()->getTilePosition());
    int naturalZone = the.bases.myNatural() ? the.zone.areaIDAt(the.bases.myNatural()->getTilePosition()) : 0;
    bool naturalTaken = the.bases.myNatural() && the.bases.myNatural()->getOwner() == the.self();

    for (const auto & kv : InformationManager::Instance().getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo & ui(kv.second);

        if (ui.type.isBuilding() &&
            !ui.goneFromLastPosition &&
            !ui.type.isRefinery() &&
            ui.type != BWAPI::UnitTypes::Terran_Engineering_Bay &&
            ui.type != BWAPI::UnitTypes::Terran_Supply_Depot &&
            ui.type != BWAPI::UnitTypes::Protoss_Pylon)
        {
            int zone = the.zone.areaIDAt(ui.lastPosition);
            if (zone > 0)
            {
                const int mainDist = ui.lastPosition.getApproxDistance(the.bases.myStart()->getCenter());
                if (zone == mainZone && mainDist <= 24 * 32 || mainDist <= 13 * 32)
                {
                    return OpeningPlan::Proxy;
                }

                // NOTE A forward forge can only mean cannons.
                const bool possibleContain =
                    ui.type == BWAPI::UnitTypes::Terran_Bunker ||
                    ui.type == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
                    ui.type == BWAPI::UnitTypes::Protoss_Forge ||
                    ui.type == BWAPI::UnitTypes::Zerg_Creep_Colony ||
                    ui.type == BWAPI::UnitTypes::Zerg_Sunken_Colony;
                const int natDist = the.bases.myNatural() ? ui.lastPosition.getApproxDistance(the.bases.myNatural()->getCenter()) : MAX_DISTANCE;
                if (naturalTaken || !possibleContain)
                {
                    if (zone == naturalZone && natDist <= 18 * 32 || natDist <= 12 * 32)
                    {
                        return OpeningPlan::Proxy;
                    }
                }

                // If it's static defense, the enemy may be trying to contain us with it.
                // Check the natural only, and don't worry about whether we've taken the natural.
                if (possibleContain)
                {
                    if (zone == naturalZone && natDist <= 24 * 32 || natDist <= 15 * 32)
                    {
                        return OpeningPlan::Contain;
                    }
                }
            }
        }
    }

    return OpeningPlan::Unknown;
}

// Recognize a fast rush (e.g. 5 pool) or a worker rush.
OpeningPlan OpponentPlan::recognizeRush() const
{
    int frame = the.now();

    Base * myBase = the.bases.myStart();
    BWAPI::Position myOrigin = myBase->getPosition();
    Base * enemyBase = the.bases.enemyStart();

    int enemyWorkerRushCount = 0;
    int nBarracks = 0;              // barracks or gateway count
    int latestBarracks = 0;         // greatest completion time in frames
    int nMarines = 0;
    int nZealots = 0;
    int nDragoons = 0;
    int nZerglings = 0;
    int nHatcheries = 0;
    int earliestPoolCompletion = MAX_DISTANCE;

    // Count remembered scouting evidence even when the units are still at the
    // enemy base. The old recognizer waited until a rush crossed half the map,
    // which was too late to alter production.
    for (const auto& kv : InformationManager::Instance().getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo& ui(kv.second);

        if (!ui.lastPosition.isValid())
        {
            continue;
        }

        if (ui.type == BWAPI::UnitTypes::Terran_Marine) ++nMarines;
        else if (ui.type == BWAPI::UnitTypes::Protoss_Zealot) ++nZealots;
        else if (ui.type == BWAPI::UnitTypes::Protoss_Dragoon) ++nDragoons;
        else if (ui.type == BWAPI::UnitTypes::Zerg_Zergling) ++nZerglings;
        else if (ui.type == BWAPI::UnitTypes::Zerg_Hatchery) ++nHatcheries;
        else if (ui.type == BWAPI::UnitTypes::Zerg_Spawning_Pool)
        {
            earliestPoolCompletion =
                std::min(earliestPoolCompletion, ui.completeBy);
        }
    }

    if (enemyBase)
    {
        // We know where the enemy started, so we can calculate how far units have traveled.
        // Even in the case of a center proxy, a worker had to travel to build the proxy.
        for (const auto & kv : InformationManager::Instance().getUnitData(the.enemy()).getUnits())
        {
            const UnitInfo & ui(kv.second);

            if (ui.lastPosition.isValid())
            {
                // NOTE If the enemy tile distance == -1 then the comparison still gives the right answer.
                if (ui.type.isWorker() &&
                    myBase->getTileDistance(ui.lastPosition) >= 0 &&
                    myBase->getTileDistance(ui.lastPosition) <= enemyBase->getTileDistance(ui.lastPosition))
                {
                    // This enemy worker is at least half way to my base. Count it.
                    ++enemyWorkerRushCount;
                    if (enemyWorkerRushCount >= 3)
                    {
                        return OpeningPlan::WorkerRush;
                    }
                }
                else if (
                    ui.type == BWAPI::UnitTypes::Terran_Marine ||
                    ui.type == BWAPI::UnitTypes::Protoss_Zealot ||
                    ui.type == BWAPI::UnitTypes::Zerg_Zergling)
                {
                    if (3000 > frame - travelTime(ui.type, ui.lastPosition, enemyBase))
                    {
                        return OpeningPlan::FastRush;
                    }
                }
                else if (rushBuilding(ui))
                {
                    return OpeningPlan::FastRush;
                }
                else if (
                    ui.type == BWAPI::UnitTypes::Terran_Barracks ||
                    ui.type == BWAPI::UnitTypes::Protoss_Gateway)
                {
                    ++nBarracks;
                    latestBarracks = std::max(latestBarracks,  ui.completeBy);
                }
            }
        }
    }
    else
    {
        // Enemy base is not found.
        for (const auto & kv : InformationManager::Instance().getUnitData(the.enemy()).getUnits())
        {
            const UnitInfo & ui(kv.second);

            if (ui.lastPosition.isValid())
            {
                // NOTE If the enemy tile distance == -1 then the comparison still gives the right answer.
                if (ui.type.isWorker() &&
                    myBase->getTileDistance(ui.lastPosition) >= 0)
                {
                    Base * closestEnemy = closestEnemyBase(ui.lastPosition);
                    if (closestEnemy &&
                        myBase->getTileDistance(ui.lastPosition) <= closestEnemy->getTileDistance(ui.lastPosition))
                    {
                        // This enemy worker is at least half way to my base. Count it.
                        ++enemyWorkerRushCount;
                        if (enemyWorkerRushCount >= 3)
                        {
                            return OpeningPlan::WorkerRush;
                        }
                    }
                }
                else if (
                    ui.type == BWAPI::UnitTypes::Terran_Marine ||
                    ui.type == BWAPI::UnitTypes::Protoss_Zealot ||
                    ui.type == BWAPI::UnitTypes::Zerg_Zergling)
                {
                    Base * closestEnemy = closestEnemyBase(ui.lastPosition);
                    if (closestEnemy && closestEnemy->getTileDistance(ui.lastPosition) >= 0)
                    {
                        if (3000 > frame - travelTime(ui.type, ui.lastPosition, closestEnemy))
                        {
                            return OpeningPlan::FastRush;
                        }
                    }
                }
                else if (rushBuilding(ui))
                {
                    return OpeningPlan::FastRush;
                }
                else if (
                    ui.type == BWAPI::UnitTypes::Terran_Barracks ||
                    ui.type == BWAPI::UnitTypes::Protoss_Gateway)
                {
                    ++nBarracks;
                    latestBarracks = std::max(latestBarracks, ui.completeBy);
                }
            }
        }
    }

    if (nBarracks >= 2 && latestBarracks < 3200)
    {
        // This looks like BBS.
        return OpeningPlan::FastRush;
    }

    // Scouting signatures of common BW all-ins/pressures. The thresholds are
    // deliberately based on commitment (multiple units or very early tech),
    // not on seeing one normal opening unit.
    if (earliestPoolCompletion < 3000 && nHatcheries <= 1)
    {
        return OpeningPlan::FastRush;
    }

    if (frame <= 6 * 24 * 60 &&
        (nZerglings >= 6 || nZealots >= 2 || nMarines >= 5))
    {
        return OpeningPlan::HeavyRush;
    }

    if (frame <= 8 * 24 * 60 &&
        (nZerglings >= 10 || nZealots >= 4 || nDragoons >= 2 || nMarines >= 8))
    {
        return OpeningPlan::HeavyRush;
    }

    return OpeningPlan::Unknown;
}

// A Factory by itself is normal TvT tech. Require actual early Vultures,
// preferably already committed across the map, before naming a rush.
bool OpponentPlan::recognizeVultureRush() const
{
    const int frame = the.now();
    if (the.enemyRace() != BWAPI::Races::Terran ||
        frame > 9 * 24 * 60)
    {
        return false;
    }

    const Base * main = the.bases.myStart();
    const Base * natural = the.bases.myNatural();
    const Base * enemyMain = the.bases.enemyStart();
    if (!main || !main->getCenter().isValid()) return false;

    int vulturesSeen = 0;
    int vulturesForward = 0;
    int vulturesNearBase = 0;
    bool attackingNearBase = false;
    for (const auto& kv : InformationManager::Instance().getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo& ui = kv.second;
        if (ui.type != BWAPI::UnitTypes::Terran_Vulture ||
            !ui.lastPosition.isValid()) continue;

        // A stale sighting does not prove that the Vulture is still pressing.
        if (ui.goneFromLastPosition || frame - ui.updateFrame > 15 * 24)
            continue;
        ++vulturesSeen;

        const bool nearBase =
            ui.lastPosition.getApproxDistance(main->getCenter()) <= 22 * 32 ||
            (natural && natural->getCenter().isValid() &&
             ui.lastPosition.getApproxDistance(natural->getCenter()) <= 18 * 32);
        if (nearBase)
        {
            ++vulturesNearBase;
            attackingNearBase = attackingNearBase ||
                (ui.unit && ui.unit->exists() && ui.unit->isVisible() &&
                 ui.unit->isAttacking());
        }

        if (enemyMain)
        {
            const int ourDistance = main->getTileDistance(ui.lastPosition);
            const int theirDistance = enemyMain->getTileDistance(ui.lastPosition);
            if (ourDistance >= 0 && theirDistance >= 0 &&
                ourDistance <= theirDistance)
            {
                ++vulturesForward;
            }
        }
    }

    return vulturesNearBase >= 2 ||
            (vulturesNearBase >= 1 && attackingNearBase) ||
            vulturesForward >= 2 ||
            (vulturesSeen >= 3 &&
             the.bases.baseCount(the.enemy()) <= 1);
}

// Factory, possibly with starport, and no sign of many marines intended.
OpeningPlan OpponentPlan::recognizeTerranTech() const
{
    if (the.enemyRace() != BWAPI::Races::Terran)
    {
        return OpeningPlan::Unknown;
    }

    int nMarines = 0;
    int nBarracks = 0;
    int nStarports = 0;
    int nTechProduction = 0;
    bool tech = false;

    for (const auto & kv : InformationManager::Instance().getUnitData(the.enemy()).getUnits())
    {
        const UnitInfo & ui(kv.second);

        if (ui.type == BWAPI::UnitTypes::Terran_Marine)
        {
            ++nMarines;
        }

        else if (ui.type.whatBuilds().first == BWAPI::UnitTypes::Terran_Barracks)
        {
            return OpeningPlan::Unknown;			// academy implied, marines seem to be intended
        }

        else if (ui.type == BWAPI::UnitTypes::Terran_Barracks)
        {
            ++nBarracks;
        }

        else if (ui.type == BWAPI::UnitTypes::Terran_Academy)
        {
            return OpeningPlan::Unknown;			// marines seem to be intended
        }

        else if (ui.type == BWAPI::UnitTypes::Terran_Wraith)
        {
            return OpeningPlan::Wraith;
        }

        else if (ui.type == BWAPI::UnitTypes::Terran_Starport)
        {
            if (ui.unit && ui.unit->isVisible() && ui.unit->isTraining() && !ui.unit->getAddon())
            {
                // The starport is flashing. Without an addon, it can only build a wraith.
                return OpeningPlan::Wraith;
            }
            ++nStarports;
        }

        else if (ui.type == BWAPI::UnitTypes::Terran_Factory)
        {
            ++nTechProduction;
        }

        else if (ui.type.whatBuilds().first == BWAPI::UnitTypes::Terran_Factory ||
            ui.type.whatBuilds().first == BWAPI::UnitTypes::Terran_Starport ||
            ui.type == BWAPI::UnitTypes::Terran_Armory)
        {
            tech = true;			// indicates intention to rely on tech units
        }
    }

    if (nStarports > 1)
    {
        return OpeningPlan::Wraith;
    }

    if ((nTechProduction >= 2 || tech) && nMarines <= 6 && nBarracks <= 1)
    {
        return OpeningPlan::Factory;
    }

    return OpeningPlan::Unknown;

}

// Record commitments that can coexist with the main opening label. Use
// remembered buildings, since the scout may leave before production starts.
// The time and base-count gates keep normal midgame macro from being treated
// as an all-in.
void OpponentPlan::recognizeThreats()
{
    const int frame = the.now();
    if (frame > 12 * 24 * 60)
    {
        return;
    }

    PlayerSnapshot snap;
    snap.takeEnemy();

    const int enemyBases = std::max(1, the.bases.baseCount(the.enemy()));
    const BWAPI::Race race = the.enemyRace();

    auto remember = [this](OpeningThreat threat)
    {
        _threatFlags |= static_cast<unsigned int>(threat);
    };

    if (_openingPlan == OpeningPlan::Factory)
    {
        remember(OpeningThreat::FactoryPressure);
    }
    else if (_openingPlan == OpeningPlan::Wraith)
    {
        remember(OpeningThreat::FastAirTech);
    }

    if (race == BWAPI::Races::Protoss)
    {
        const int gateways = snap.count(BWAPI::UnitTypes::Protoss_Gateway);
        const int nexus = snap.count(BWAPI::UnitTypes::Protoss_Nexus);
        const bool hasGas = snap.count(BWAPI::UnitTypes::Protoss_Assimilator) > 0;
        const bool hasCore = snap.count(BWAPI::UnitTypes::Protoss_Cybernetics_Core) > 0;
        const int robotics = snap.count(BWAPI::UnitTypes::Protoss_Robotics_Facility);
        const int stargates = snap.count(BWAPI::UnitTypes::Protoss_Stargate);
        const int citadels = snap.count(BWAPI::UnitTypes::Protoss_Citadel_of_Adun);
        const int archives = snap.count(BWAPI::UnitTypes::Protoss_Templar_Archives);
        const int protossBases = std::max(enemyBases, nexus);
        const int dragoons = snap.count(BWAPI::UnitTypes::Protoss_Dragoon);

        _maxProtossGatewaysSeen = std::max(_maxProtossGatewaysSeen, gateways);
        if (hasCore || hasGas || dragoons > 0)
        {
            _protossRangedTechSeen = true;
        }

        // HeavyRush is a temporary reaction label.  If the later scout finds a
        // real Core/gas transition behind multiple Gateways, stop presenting the
        // old rush label as if the recognizer were frozen.  The detailed threat
        // name below now carries the current plan.
        if (_openingPlan == OpeningPlan::HeavyRush && gateways >= 2 &&
            (hasCore || hasGas || dragoons > 0))
        {
            _openingPlan = OpeningPlan::Unknown;
            _planIsFixed = false;
        }

        // Standard 1 Gate Core is not an all-in, but it is early, reliable
        // evidence that the first ranged pressure will be Dragoons. It merits
        // one covered bunker and early siege, not the full 4 Gate response.
        if (frame <= 7 * 24 * 60 && gateways >= 1 && hasCore && protossBases <= 1)
        {
            remember(OpeningThreat::ProtossOneGateCore);
            remember(OpeningThreat::DragoonPressure);
        }

        // A second Gateway behind Core/gas is already meaningful ranged
        // commitment.  The old code had no 2 Gate Core state at all, so the UI
        // stayed on "1 Gate Core" until four Gateways were eventually seen.
        // Treat 2+ Gate + ranged tech as gateway pressure immediately.
        if (frame <= 10 * 24 * 60 && gateways >= 2 &&
            (hasCore || hasGas || dragoons > 0) && protossBases <= 2)
        {
            remember(OpeningThreat::GatewayPressure);
            remember(OpeningThreat::DragoonPressure);
        }

        if (frame <= 7 * 24 * 60 && gateways >= 2 &&
            !hasCore && !hasGas && protossBases <= 1)
        {
            remember(OpeningThreat::ProtossTwoGateZealot);
            remember(OpeningThreat::GatewayPressure);
            remember(OpeningThreat::ZealotPressure);
        }

        if (frame <= 10 * 24 * 60 && gateways >= 4 &&
            (hasCore || hasGas || dragoons > 0) &&
            protossBases <= 2)
        {
            remember(OpeningThreat::ProtossFourGateDragoon);
            remember(OpeningThreat::GatewayPressure);
            remember(OpeningThreat::DragoonPressure);
            remember(OpeningThreat::MassProduction);
        }

        if (frame <= 8 * 24 * 60 && robotics > 0 && protossBases <= 1)
        {
            remember(OpeningThreat::ProtossFastRobotics);
        }

        if (frame <= 9 * 24 * 60 && stargates > 0 && protossBases <= 1)
        {
            remember(OpeningThreat::ProtossFastStargate);
            remember(OpeningThreat::FastAirTech);
        }

        if (frame <= 9 * 24 * 60 &&
            (archives > 0 ||
             snap.count(BWAPI::UnitTypes::Protoss_Dark_Templar) > 0 ||
             (citadels > 0 && robotics == 0 && stargates == 0 && protossBases <= 1)))
        {
            remember(OpeningThreat::ProtossFastDarkTemplar);
            remember(OpeningThreat::CloakTech);
        }

        const bool gatewayCommitment =
            (frame <= 9 * 24 * 60 && gateways >= 3 && protossBases <= 1) ||
            (frame <= 10 * 24 * 60 && gateways >= 4 && protossBases <= 2);

        if (gatewayCommitment)
        {
            remember(OpeningThreat::GatewayPressure);

            if (hasGas || hasCore ||
                dragoons > 0)
            {
                remember(OpeningThreat::DragoonPressure);
            }
            else
            {
                remember(OpeningThreat::ZealotPressure);
            }
        }

        if (stargates > 0 ||
            snap.count(BWAPI::UnitTypes::Protoss_Fleet_Beacon) > 0)
        {
            remember(OpeningThreat::FastAirTech);
        }

        if (citadels > 0 ||
            archives > 0 ||
            snap.count(BWAPI::UnitTypes::Protoss_Dark_Templar) > 0)
        {
            remember(OpeningThreat::CloakTech);
        }
    }
    else if (race == BWAPI::Races::Terran)
    {
        if (recognizeVultureRush())
        {
            remember(OpeningThreat::TerranVultureRush);
        }
        const int factories = snap.count(BWAPI::UnitTypes::Terran_Factory);
        const int barracks = snap.count(BWAPI::UnitTypes::Terran_Barracks);
        const int commandCenters = snap.count(BWAPI::UnitTypes::Terran_Command_Center);
        const int starports = snap.count(BWAPI::UnitTypes::Terran_Starport);
        const int terranBases = std::max(enemyBases, commandCenters);

        if (_openingPlan == OpeningPlan::HeavyRush && factories > 0)
        {
            _openingPlan = OpeningPlan::Unknown;
            _planIsFixed = false;
        }

        if ((frame <= 10 * 24 * 60 && factories >= 2 && terranBases <= 2) ||
            snap.count(BWAPI::UnitTypes::Terran_Machine_Shop) >= 2)
        {
            remember(OpeningThreat::FactoryPressure);
            remember(OpeningThreat::TerranTwoFactory);
        }

        if (frame <= 8 * 24 * 60 && barracks >= 2 && terranBases <= 1)
        {
            remember(OpeningThreat::MassBarracks);
            remember(OpeningThreat::TerranTwoRax);
        }

        if (frame <= 10 * 24 * 60 && starports >= 2)
        {
            remember(OpeningThreat::TerranTwoStarport);
        }

        if (starports >= 2 ||
            snap.count(BWAPI::UnitTypes::Terran_Control_Tower) > 0)
        {
            remember(OpeningThreat::FastAirTech);
        }

        if (frame <= 8 * 24 * 60 && factories >= 1 && terranBases >= 2)
        {
            remember(OpeningThreat::TerranFactoryExpand);
        }

        if (snap.count(BWAPI::UnitTypes::Terran_Covert_Ops) > 0 ||
            snap.count(BWAPI::UnitTypes::Terran_Ghost) > 0 ||
            snap.count(BWAPI::UnitTypes::Terran_Wraith) > 0)
        {
            remember(OpeningThreat::CloakTech);
        }
    }
    else if (race == BWAPI::Races::Zerg)
    {
        const int hatcheries =
            snap.count(BWAPI::UnitTypes::Zerg_Hatchery) +
            snap.count(BWAPI::UnitTypes::Zerg_Lair) +
            snap.count(BWAPI::UnitTypes::Zerg_Hive);
        const int pools = snap.count(BWAPI::UnitTypes::Zerg_Spawning_Pool);
        const int extractors = snap.count(BWAPI::UnitTypes::Zerg_Extractor);
        const int lairs =
            snap.count(BWAPI::UnitTypes::Zerg_Lair) +
            snap.count(BWAPI::UnitTypes::Zerg_Hive);
        const int hydraDens = snap.count(BWAPI::UnitTypes::Zerg_Hydralisk_Den);
        const int spires =
            snap.count(BWAPI::UnitTypes::Zerg_Spire) +
            snap.count(BWAPI::UnitTypes::Zerg_Greater_Spire);
        const int zergBases = std::max(enemyBases, std::min(hatcheries, 3));

        if (_openingPlan == OpeningPlan::HeavyRush &&
            (lairs > 0 || hydraDens > 0 || spires > 0))
        {
            _openingPlan = OpeningPlan::Unknown;
            _planIsFixed = false;
        }

        if (frame <= 6 * 24 * 60 && pools > 0 && zergBases <= 1)
        {
            remember(OpeningThreat::ZergPoolFirst);
            remember(OpeningThreat::LingPressure);
        }

        if (frame <= 7 * 24 * 60 && zergBases >= 2 && pools > 0 &&
            extractors == 0 && lairs == 0 && hydraDens == 0)
        {
            remember(OpeningThreat::ZergTwoHatchLing);
            remember(OpeningThreat::LingPressure);
        }

        if (frame <= 9 * 24 * 60 && spires > 0 && enemyBases <= 2)
        {
            remember(OpeningThreat::ZergTwoHatchMuta);
            remember(OpeningThreat::FastAirTech);
        }

        if (frame <= 10 * 24 * 60 && hydraDens > 0 && lairs > 0 && enemyBases <= 2)
        {
            remember(OpeningThreat::ZergTwoHatchLurker);
            remember(OpeningThreat::HydraliskPressure);
            remember(OpeningThreat::CloakTech);
        }
        else if (frame <= 8 * 24 * 60 && hydraDens > 0)
        {
            remember(OpeningThreat::HydraliskPressure);
        }

        if (spires > 0 ||
            snap.count(BWAPI::UnitTypes::Zerg_Greater_Spire) > 0)
        {
            remember(OpeningThreat::FastAirTech);
            remember(OpeningThreat::ZergMutaliskTech);
        }

        if (snap.count(BWAPI::UnitTypes::Zerg_Lurker) > 0 ||
            snap.count(BWAPI::UnitTypes::Zerg_Lurker_Egg) > 0)
        {
            remember(OpeningThreat::CloakTech);
        }
    }

    int productionBuildings = 0;
    if (race == BWAPI::Races::Protoss)
    {
        productionBuildings =
            snap.count(BWAPI::UnitTypes::Protoss_Gateway) +
            snap.count(BWAPI::UnitTypes::Protoss_Robotics_Facility) +
            snap.count(BWAPI::UnitTypes::Protoss_Stargate);
    }
    else if (race == BWAPI::Races::Terran)
    {
        productionBuildings =
            snap.count(BWAPI::UnitTypes::Terran_Barracks) +
            snap.count(BWAPI::UnitTypes::Terran_Factory) +
            snap.count(BWAPI::UnitTypes::Terran_Starport);
    }
    else if (race == BWAPI::Races::Zerg)
    {
        // Hatcheries are both economy and production, so require a stronger
        // one-base commitment before raising the generic flag.
        productionBuildings = snap.count(BWAPI::UnitTypes::Zerg_Hatchery);
    }

    const int massThreshold = race == BWAPI::Races::Zerg ? 3 : 4;
    if (frame <= 10 * 24 * 60 && enemyBases <= 1 &&
        productionBuildings >= massThreshold)
    {
        remember(OpeningThreat::MassProduction);
    }
}

void OpponentPlan::recognize()
{
    // Don't recognize island plans.
    // The regular plans and reactions do not make sense for island maps.
    if (the.bases.isIslandStart())
    {
        return;
    }

    // Recognize fast plans first, slow plans below.

    // Recognize in-base proxy buildings and slightly more distant Contain buildings.
    // NOTE It's not required, but still safest to check proxies before rushes, to prevent misrecognition.
    OpeningPlan maybeProxyPlan = recognizeProxy();
    if (maybeProxyPlan != OpeningPlan::Unknown)
    {
        _openingPlan = maybeProxyPlan;
        _planIsFixed = true;
        return;
    }

    // Recognize fast rush and worker rush.
    OpeningPlan maybeRushPlan = recognizeRush();
    if (recognizeVultureRush())
    {
        maybeRushPlan = OpeningPlan::VultureRush;
    }
    const bool heavyRushHasAlreadyTransitioned =
        maybeRushPlan == OpeningPlan::HeavyRush &&
        (hasThreat(OpeningThreat::FastAirTech) ||
         hasThreat(OpeningThreat::CloakTech) ||
         (hasThreat(OpeningThreat::GatewayPressure) &&
          hasThreat(OpeningThreat::DragoonPressure)) ||
         hasThreat(OpeningThreat::FactoryPressure) ||
         hasThreat(OpeningThreat::HydraliskPressure));

    if (maybeRushPlan != OpeningPlan::Unknown && !heavyRushHasAlreadyTransitioned)
    {
        _openingPlan = maybeRushPlan;
        // Worker/proxy-speed cheese is a lasting opening identity. HeavyRush is
        // a pressure state and must be allowed to transition when later scouting
        // reveals Core/Factory/Lair tech or an expansion.
        _planIsFixed = maybeRushPlan != OpeningPlan::HeavyRush;
        return;
    }

    int frame = the.now();

    PlayerSnapshot snap;
    snap.takeEnemy();

    // Recognize slower rushes.
    // TODO make sure we've seen the bare geyser in the enemy base!
    // TODO seeing an enemy worker carrying gas also means the enemy has gas
    if (snap.count(BWAPI::UnitTypes::Zerg_Hatchery) >= 2 &&
        snap.count(BWAPI::UnitTypes::Zerg_Spawning_Pool) > 0 &&
        snap.count(BWAPI::UnitTypes::Zerg_Extractor) == 0
        ||
        snap.count(BWAPI::UnitTypes::Terran_Barracks) >= 2 &&
        snap.count(BWAPI::UnitTypes::Terran_Refinery) == 0 &&
        snap.count(BWAPI::UnitTypes::Terran_Command_Center) <= 1
        ||
        snap.count(BWAPI::UnitTypes::Protoss_Gateway) >= 2 &&
        snap.count(BWAPI::UnitTypes::Protoss_Assimilator) == 0 &&
        snap.count(BWAPI::UnitTypes::Protoss_Nexus) <= 1)
    {
        _openingPlan = OpeningPlan::HeavyRush;
        _planIsFixed = false;
        return;
    }

    // Recognize terran factory or starport tech openings.
    OpeningPlan maybeTechPlan = recognizeTerranTech();
    if (maybeTechPlan != OpeningPlan::Unknown)
    {
        _openingPlan = maybeTechPlan;
        if (_openingPlan == OpeningPlan::Wraith)
        {
            // We're cautious about recognizing it, so don't downgrade Wraith to Factory later.
            _planIsFixed = true;
        }
        return;
    }

    // Recognize expansions with pre-placed static defense.
    // Zerg can't do this.
    // NOTE Incomplete test! We don't check the location of the static defense
    if (the.bases.baseCount(the.enemy()) >= 2)
    {
        if (snap.count(BWAPI::UnitTypes::Terran_Bunker) > 0 ||
            snap.count(BWAPI::UnitTypes::Protoss_Photon_Cannon) > 0)
        {
            _openingPlan = OpeningPlan::SafeExpand;
            return;
        }
    }

    // Recognize a naked expansion.
    // This has to run after the SafeExpand check, since it doesn't check for what's missing.
    if (the.bases.baseCount(the.enemy()) >= 2)
    {
        _openingPlan = OpeningPlan::NakedExpand;
        return;
    }

    // Recognize a turtling enemy.
    // NOTE Incomplete test! We don't check where the defenses are placed.
    if (the.bases.baseCount(the.enemy()) < 2)
    {
        if (snap.count(BWAPI::UnitTypes::Terran_Bunker) >= 2 ||
            snap.count(BWAPI::UnitTypes::Protoss_Photon_Cannon) >= 2 ||
            snap.count(BWAPI::UnitTypes::Zerg_Sunken_Colony) >= 2)
        {
            _openingPlan = OpeningPlan::Turtle;
            return;
        }
    }

    // Nothing recognized: Opening plan remains unchanged.
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

OpponentPlan::OpponentPlan()
    : _openingPlan(OpeningPlan::Unknown)
    , _planIsFixed(false)
    , _threatFlags(0)
    , _maxProtossGatewaysSeen(0)
    , _protossRangedTechSeen(false)
{
}

// Return the most useful specific opening name for the UI. The flags remain
// independent; this merely chooses the latest/highest-impact one to display.
std::string OpponentPlan::getThreatName() const
{
    if (hasThreat(OpeningThreat::ProtossFourGateDragoon) ||
        (_maxProtossGatewaysSeen >= 4 && _protossRangedTechSeen))
    {
        return "4 Gate Dragoon";
    }
    if (hasThreat(OpeningThreat::ProtossFastDarkTemplar)) return "Fast Dark Templar";
    if (hasThreat(OpeningThreat::ProtossFastRobotics)) return "Fast Robotics";
    if (hasThreat(OpeningThreat::ProtossFastStargate)) return "Fast Stargate";

    // Show the observed gateway progression instead of leaving a sticky
    // 1 Gate Core label on the screen.  Buildings are remembered, including
    // incomplete ones, so this normally changes within four frames of scouting
    // the additional Gateway.
    if (_maxProtossGatewaysSeen >= 3 && _protossRangedTechSeen) return "3 Gate Dragoon";
    if (_maxProtossGatewaysSeen >= 2 && _protossRangedTechSeen) return "2 Gate Core";
    if (_maxProtossGatewaysSeen >= 3 && hasThreat(OpeningThreat::ZealotPressure)) return "3 Gate Zealot";
    if (hasThreat(OpeningThreat::ProtossTwoGateZealot)) return "2 Gate Zealot";
    if (hasThreat(OpeningThreat::ProtossOneGateCore)) return "1 Gate Core";

    if (hasThreat(OpeningThreat::TerranTwoStarport)) return "2 Port Wraith";
    if (hasThreat(OpeningThreat::TerranVultureRush)) return "Vulture rush";
    if (hasThreat(OpeningThreat::TerranTwoFactory)) return "2 Factory";
    if (hasThreat(OpeningThreat::TerranTwoRax)) return "2 Rax Bio";
    if (hasThreat(OpeningThreat::TerranFactoryExpand)) return "Factory Expand";

    if (hasThreat(OpeningThreat::ZergTwoHatchLurker)) return "2 Hatch Lurker";
    if (hasThreat(OpeningThreat::ZergTwoHatchMuta)) return "2 Hatch Muta";
    if (hasThreat(OpeningThreat::ZergMutaliskTech)) return "Mutalisk Tech";
    if (hasThreat(OpeningThreat::ZergTwoHatchLing)) return "2 Hatch Ling";
    if (hasThreat(OpeningThreat::HydraliskPressure)) return "Fast Hydralisk Den";
    if (hasThreat(OpeningThreat::ZergPoolFirst)) return "Pool First";

    return std::string();
}

// Update the recognized plan.
// Call this every frame. It will take care of throttling itself down to avoid unnecessary work.
void OpponentPlan::update()
{
    if (!Config::Strategy::UsePlanRecognizer)
    {
        return;
    }

    int frame = BWAPI::Broodwar->getFrameCount();

    if (frame > 100 && frame < 12 * 24 * 60) // late scout must still reveal an opening/expand
    {
        // Threat flags drive emergency production directly. Refresh them more
        // often than the heavier opening-label recognizer so a newly visible
        // Spire / 2 Hatch Muta / DT / proxy signal reaches StrategyManager in
        // at most 4 frames instead of waiting up to half a second.
        if (frame % 4 == 3)
        {
            recognizeThreats();
        }

        // The coarse opening label is still throttled to the old 12-frame
        // cadence. Once fixed, independent threat flags continue updating.
        if (frame % 12 == 7 && !_planIsFixed)
        {
            recognize();
        }
    }
}
