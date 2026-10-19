#include "Combat/Squads/Squad.h"

#include "Map/Bases.h"
#include "Combat/Simulation/CombatSimulation.h"
#include "Map/MapTools.h"
#include "Combat/Squads/SquadOrder.h"
#include "Combat/CombatCommander.h"
#include "Strategy/StrategyManager.h"
#include "Strategy/Terran/AirTanksPolicy.h"
#include "Strategy/Terran/AirTanksFormation.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"
#include "Combat/Micro/Terran/MicroTanks.h"
#include "Combat/Micro/Terran/MicroMarines.h"
#include "Combat/Micro/Terran/BioInfantrySafety.h"
#include "Combat/Micro/Terran/TerranFormation.h"
#include <climits>
#include <cmath>

using namespace UAlbertaBot;

namespace
{
    int countGoliathsInUnits(const BWAPI::Unitset& units)
    {
        int count = 0;

        for (BWAPI::Unit unit : units)
        {
            if (unit &&
                unit->exists() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Goliath)
            {
                ++count;
            }
        }

        return count;
    }

    // At 180/200+ supply the army is deliberately in force-push mode.
    // BWAPI reports supply doubled, so 180 supply == 360 supplyUsed().
    // This is intentionally independent of current bank/worker count: the
    // point of the rule is to stop a nearly-maxed army from oscillating in
    // front of a small amount of static defense.
    bool maxSupplyForcePush()
    {
        return the.self() && the.self()->supplyUsed() >= 360;
    }

    BWAPI::Position goliathFormationDestination(
        BWAPI::Unit unit,
        int index,
        int total,
        const BWAPI::Position& clusterCenter,
        const BWAPI::Position& destination,
        const BWAPI::Position& formationAnchor)
    {
        if (!unit ||
            !unit->exists() ||
            total <= 0 ||
            !clusterCenter.isValid() ||
            !destination.isValid() ||
            !formationAnchor.isValid())
        {
            return destination;
        }

        const BWAPI::Position result = TerranFormation::goliathSlot(
            unit, index, total, formationAnchor,
            formationAnchor == destination ? destination + (destination - clusterCenter) : destination);
        return result.isValid() ? result : destination;
    }


}

Squad::Squad()
    : _name("Default")
    , _combatSquad(false)
    , _combatSimRadius(Config::Micro::CombatSimRadius)
    , _fightVisibleOnly(false)
    , _meatgrinder(false)
    , _hasAir(false)
    , _hasGround(false)
    , _canAttackAir(false)
    , _canAttackGround(false)
    , _attackAtMax(false)
    , _priority(0)
    , _timeMark(MAX_FRAME)
    , _lastAttack(the.now())
    , _lastRetreat(the.now())
    , _order(SquadOrder("None"))
    , _orderFrame(the.now())
    , _regroupPosition(BWAPI::Positions::Invalid)
{
    setOrderForMicroManagers();
}
// The usual work of workers is managed by WorkerManager. If we put workers into
// another squad, we have to notify WorkerManager.
Squad::Squad(const std::string & name, size_t priority)
    : _name(name)
    , _combatSquad(name != "Idle")
    , _combatSimRadius(Config::Micro::CombatSimRadius)
    , _fightVisibleOnly(false)
    , _meatgrinder(false)
    , _hasAir(false)
    , _hasGround(false)
    , _canAttackAir(false)
    , _canAttackGround(false)
    , _attackAtMax(false)
    , _priority(priority)
    , _timeMark(MAX_FRAME)
    , _lastAttack(the.now())
    , _lastRetreat(the.now())
    , _order(SquadOrder("None"))
    , _orderFrame(the.now())
    , _regroupPosition(BWAPI::Positions::Invalid)
{
    setOrderForMicroManagers();
}

Squad::~Squad()
{
    clear();
}

void Squad::update()
{
    _lastUpdateProfile = SquadUpdateProfile();
    _lastUpdateProfile.squad = _name;
    _lastUpdateProfile.measure(SquadUpdateProfile::Units, [&] { updateUnits(); });
    _lastUpdateProfile.units = int(_units.size());

    // The Irradiated squad.
    

    // The vanguard (squad unit farthest forward) will be updated below if appropriate.
    _vanguard = nullptr;

    if (_units.empty())
    {
        return;
    }
    
    // If this is a worker squad, there is nothing more to do.
    if (!_combatSquad)
    {
        return;
    }

    if (_order.getType() == SquadOrderTypes::Watch)
    {
        
        // Else fall through and act as usual.
    }

    // This is a non-empty combat squad, so it may have a meaningful vanguard unit.
    _lastUpdateProfile.measure(SquadUpdateProfile::Vanguard,
        [&] { _vanguard = unitClosestToTarget(_units); });

    if (_order.getType() == SquadOrderTypes::Load)
    {
        _lastUpdateProfile.measure(SquadUpdateProfile::Combat,
            [&] { loadTransport(); });
        return;
    }

    if (_order.getType() == SquadOrderTypes::Drop)
    {
        _lastUpdateProfile.measure(SquadUpdateProfile::Combat,
            [&] { _microTransports.update(); });
        // And fall through to let the rest of the drop squad fight.
    }

    // Maybe stim marines and firebats.
    _lastUpdateProfile.measure(SquadUpdateProfile::Stim,
        [&] { stimIfNeeded(); });

    // Detectors.
    // Build a cleaner escort set for detectors:
    // no detectors themselves, no transports, no unready units.
    _lastUpdateProfile.measure(SquadUpdateProfile::Detectors, [&]
    {
        BWAPI::Unitset detectorEscortUnits;

        for (BWAPI::Unit unit : _units)
        {
            if (!unit || !unit->exists() || !unit->getPosition().isValid())
            {
                continue;
            }

            if (unit->getType().isDetector())
            {
                continue;
            }

            if (unit->getType().spaceProvided() > 0)
            {
                continue;
            }





            detectorEscortUnits.insert(unit);
        }

        BWAPI::Unit detectorVanguard = nullptr;
        if (!detectorEscortUnits.empty())
        {
            detectorVanguard = unitClosestToTarget(detectorEscortUnits);
        }

        // Detectors.
        _microDetectors.setUnitClosestToTarget(detectorVanguard);
        _microDetectors.setSquadSize(_units.size());
        _microDetectors.go(detectorEscortUnits);
    });

    // Finish choosing the units.
    BWAPI::Unitset unitsToCluster;
    for (BWAPI::Unit unit : _units)
    {
        if (BioInfantrySafety::evade(unit)) continue;
        if (unit->getType().isDetector() ||
            unit->getType().spaceProvided() > 0)
        {
        }
        else 
        {
            unitsToCluster.insert(unit);
        }
    }

    // First pass to set cluster status.
    // It also takes non-combat actions.
    _lastUpdateProfile.measure(SquadUpdateProfile::Clustering,
        [&] { the.ops.cluster(the.self(), unitsToCluster, _clusters); });
    _lastUpdateProfile.clusters = int(_clusters.size());
    for (UnitCluster & cluster : _clusters)
    {
        // MicroVultures is the only Vulture command owner. It gets the first
        // chance to maintain/issue a strategic mine order; moveCluster() below
        // recognizes that lease and will not overwrite it.
        _lastUpdateProfile.measure(SquadUpdateProfile::Special,
            [&] { _microVultures.executeStrategicMines(cluster); });
        _lastUpdateProfile.measure(SquadUpdateProfile::Status,
            [&] { setClusterStatus(cluster); });
        _lastUpdateProfile.measure(SquadUpdateProfile::Special,
            [&] { microSpecialUnits(cluster); });
    }

    // Second pass to reconsider cluster status in light of the status of nearby clusters.
    _lastUpdateProfile.measure(SquadUpdateProfile::Status, [&]
    {
        bool statusReset;
        do
        {
            statusReset = false;
            for (UnitCluster & cluster : _clusters)
            {
                statusReset = statusReset || resetClusterStatus(cluster);
            }
        } while (statusReset);

        // Update _lastAttack and _lastRetreat based on the cluster status.
        setLastAttackRetreat();
    });

    for (const UnitCluster& cluster : _clusters)
    {
        _lastUpdateProfile.measure(SquadUpdateProfile::Tanks,
            [&] { _microTanks.execute(cluster); });
    }

    // Third pass to carry out combat or retreat actions for clusters that have them.
    // It can get slow in late game when there are many clusters, so cut down the update frequency.
    const int nPhases = std::max(1, std::min(4, int(_clusters.size() / 12)));
    int phase = BWAPI::Broodwar->getFrameCount() % nPhases;
    for (const UnitCluster & cluster : _clusters)
    {
        BWAPI::Position regroupPosition = BWAPI::Positions::Invalid;
        if (cluster.status == ClusterStatus::Regroup && (phase == 0 || the.now() % 6 == 0))
            _lastUpdateProfile.measure(SquadUpdateProfile::Regroup,
                [&] { regroupPosition = calcRegroupPosition(cluster); });
        if (the.now() % 6 == 0)
            _lastUpdateProfile.measure(SquadUpdateProfile::Medics,
                [&] { _microMedics.update(cluster, _units, regroupPosition); });
        if (phase == 0)
        {
            _lastUpdateProfile.measure(SquadUpdateProfile::Combat,
                [&] { clusterCombat(cluster, regroupPosition); });
        }
        phase = (phase + 1) % nPhases;
    }

}

// Set cluster status and take non-combat cluster actions.
void Squad::setClusterStatus(UnitCluster & cluster)
{
    // A neutral blocker is not an enemy, so the normal near-enemy test would
    // keep the squad in Advance forever and MicroManager would never issue the
    // attack. DestroyNeutral must execute its dedicated target logic.
    if (_order.getType() == SquadOrderTypes::DestroyNeutral)
    {
        cluster.status = ClusterStatus::Attack;
        return;
    }

    // A near-max army is normally forced forward, but a dense cannon/sunken
    // line can trade away a Goliath/Vulture ball before it fires enough shots.
    // Gather the siege support first unless our local force is overwhelming.
    if (shouldWaitForSiegeAgainstStaticDefense(cluster))
    {
        cluster.status = ClusterStatus::Regroup;
        _regroupStatus = yellow + std::string("Wait for siege vs static defense");
        drawCluster(cluster);
        return;
    }

    // 180+ supply means a real force push, not merely "don't retreat in the
    // combat simulator". Put combat-capable clusters into Attack directly so
    // Join up / Assemble cannot park a maxed army in front of one Sunken,
    // bunker or cannon. Each micro manager still chooses legal targets and
    // pathing; non-combat clusters keep their normal handling below.
    if (maxSupplyForcePush() && !noCombatUnits(cluster) && _order.isCombatOrder())
    {
        cluster.status = ClusterStatus::Attack;
        _regroupStatus = green + std::string("Banzai 180+");
        drawCluster(cluster);
        return;
    }

    // Cases where the cluster can't get into a fight.
    if (noCombatUnits(cluster))
    {
        if (joinUp(cluster))
        {
            cluster.status = ClusterStatus::Advance;
            _regroupStatus = yellow + std::string("Join up");
        }
        else
        {
            // Can't join another cluster. Move back to base.
            cluster.status = ClusterStatus::FallBack;
            moveCluster(cluster, finalRegroupPosition(), false);
            _regroupStatus = red + std::string("Fall back");
        }
    }
    else if (notNearEnemy(cluster))
    {
        // Neutral Eggs are absent from the enemy grid. Without this transition
        // Advance only issues movement and never calls the firing micro.
        BWAPI::Unitset eggs;
        if (_order.isCombatOrder()) MicroManager::addNearbyNeutralEggTargets(cluster, eggs);
        if (!eggs.empty())
        {
            // Cleanup is movement work, not a battle that should pull nearby
            // retreating clusters back into combat through resetClusterStatus().
            cluster.status = ClusterStatus::Advance;
            _regroupStatus = yellow + std::string("Clear nearby Eggs");
            drawCluster(cluster);
            return;
        }
        cluster.status = ClusterStatus::Advance;
        if (joinUp(cluster))
        {
            _regroupStatus = yellow + std::string("Join up");
        }
        else
        {
            bool waitForGoliathSupport = false;
            const UnitCluster* goliathSupportCluster = nullptr;
            int goliathSupportCount = 0;

            // In PureAir the foremost AntiAir cluster used to keep walking
            // while every smaller cluster chased it. The result was the long
            // one-unit-wide stream seen at ramps. Let a small vanguard wait
            // until the majority catches up; a real assembled army is never
            // held for a few newly produced reinforcements.
            if (_name == "AntiAir" &&
                StrategyManager::Instance().getTerranArmyPosture() ==
                    TerranArmyPosture::PureAir)
            {
                const int totalGoliaths = countGoliathsInUnits(_units);
                const int clusterGoliaths = countGoliathsInUnits(cluster.units);

                // A real main group (at least half of all Goliaths) keeps
                // advancing. Only a smaller group ahead of a meaningful body
                // of reinforcements closes the gap. The old 65% threshold and
                // "any Goliath behind" test allowed one newly produced unit to
                // stop the front indefinitely.
                if (totalGoliaths >= 8 &&
                    clusterGoliaths > 0 &&
                    clusterGoliaths * 100 < totalGoliaths * 50)
                {
                    const int clusterDistance = getDistance(cluster.center);

                    for (const UnitCluster& other : _clusters)
                    {
                        const int otherGoliaths =
                            countGoliathsInUnits(other.units);

                        if (other.center == cluster.center ||
                            otherGoliaths <= 0)
                        {
                            continue;
                        }

                        const bool otherIsBehind =
                            getDistance(other.center) >= clusterDistance;

                        if (otherIsBehind &&
                            otherGoliaths > goliathSupportCount)
                        {
                            goliathSupportCluster = &other;
                            goliathSupportCount = otherGoliaths;
                        }
                    }

                    waitForGoliathSupport =
                        goliathSupportCluster &&
                        goliathSupportCount >= std::max(2, totalGoliaths / 5);
                }
            }

            if (waitForGoliathSupport)
            {
                const int supportGap =
                    cluster.center.getApproxDistance(
                        goliathSupportCluster->center);

                if (supportGap > 5 * 32)
                {
                    // Do not leave a detached group parked in the middle of
                    // the map. Move it back toward the largest supporting
                    // Goliath body until both groups are close enough to fight
                    // as one formation.
                    moveCluster(cluster, goliathSupportCluster->center);
                }
                else
                {
                    for (BWAPI::Unit unit : cluster.units)
                    {
                        if (unit &&
                            unit->exists() &&
                            unit->getType() == BWAPI::UnitTypes::Terran_Goliath)
                        {
                            the.micro.HoldPosition(unit);
                        }
                    }
                }

                _regroupStatus = yellow + std::string("Assemble");
            }
            else
            {
                // Move toward the order position.
                moveCluster(cluster, _order.getPosition());
                _regroupStatus = yellow + std::string("Advance");
            }
        }
    }
	else if (canReachMineralLine(cluster))
	{
		safeMoveCluster(cluster, _order.getPosition());
		_regroupStatus = yellow + std::string("Safe Advance");
	}
    else
    {
        // Cases where the cluster might get into a fight.
        if (needsToRegroup(cluster))
        {
            cluster.status = ClusterStatus::Regroup;
        }
        else
        {
            cluster.status = ClusterStatus::Attack;
        }
    }

    drawCluster(cluster);
}

// Update _lastAttack and _lastRetreat based on the cluster status.
void Squad::setLastAttackRetreat()
{
    for (UnitCluster & cluster : _clusters)
    {
        if (cluster.status == ClusterStatus::Attack)
        {
            _lastAttack = the.now();
        }
        else if (cluster.status == ClusterStatus::Regroup)
        {
            _lastRetreat = the.now();
        }
    }
}

// Reconsider cluster status in light of the status of nearby clusters.
// Some clusters may be set to retreat, but they're near a fight they should join.
// Tell them to attack after all.
// Return true if a change was made.
bool Squad::resetClusterStatus(UnitCluster & cluster)
{
    if (cluster.status == ClusterStatus::Regroup)
    {
        if (shouldWaitForSiegeAgainstStaticDefense(cluster))
        {
            return false;
        }
        const bool bio = StrategyManager::Instance().bioTankPlanActive();
        for (const UnitCluster & cluster2 : _clusters)
        {
            // A fight twenty tiles away is not immediate support for a weak
            // bio reinforcement. Join only a substantial nearby fighting group.
            if (bio && (cluster2.units.size() < std::max(size_t(8), cluster.units.size()) ||
                !the.partitions.connected(cluster.center, cluster2.center))) continue;
            if (cluster2.status == ClusterStatus::Attack &&
                cluster.center.getApproxDistance(cluster2.center) < (bio ? 6 : 20) * 32)
            {
                cluster.status = ClusterStatus::Attack;
                return true;
            }
        }
    }

    return false;
}

bool Squad::shouldWaitForSiegeAgainstStaticDefense(const UnitCluster & cluster) const
{
    if (the.selfRace() != BWAPI::Races::Terran || cluster.air ||
        (_order.getType() != SquadOrderTypes::Attack &&
         _order.getType() != SquadOrderTypes::OmniAttack) ||
        !cluster.center.isValid() ||
        cluster.units.empty())
    {
        return false;
    }

    BWAPI::Unit closestDefense = nullptr;
    int closestDistance = MAX_DISTANCE;
    for (BWAPI::Unit enemy : the.enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            !enemy->isCompleted() || !enemy->getPosition().isValid() ||
            (enemy->getType() != BWAPI::UnitTypes::Protoss_Photon_Cannon &&
             enemy->getType() != BWAPI::UnitTypes::Zerg_Sunken_Colony))
        {
            continue;
        }

        const int distance = enemy->getDistance(cluster.center);
        if (distance < closestDistance)
        {
            closestDistance = distance;
            closestDefense = enemy;
        }
    }

    // Only evaluate the defense line immediately ahead of this cluster.
    if (!closestDefense || closestDistance > 12 * 32)
    {
        return false;
    }

    int defenses = 0;
    int mobileDefenders = 0;
    for (BWAPI::Unit enemy : the.enemy()->getUnits())
    {
        if (!enemy || !enemy->exists() || !enemy->isVisible() ||
            !enemy->getPosition().isValid() ||
            enemy->getDistance(closestDefense) > 8 * 32)
        {
            continue;
        }

        if (enemy->isCompleted() &&
            (enemy->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon ||
             enemy->getType() == BWAPI::UnitTypes::Zerg_Sunken_Colony))
        {
            ++defenses;
        }
        else if (!enemy->getType().isBuilding() && !enemy->isFlying() &&
                 UnitUtil::CanAttackGround(enemy))
        {
            ++mobileDefenders;
        }
    }

    if (defenses < 3)
    {
        return false;
    }

    const int requiredTanks = defenses >= 5 ? 3 : 2;
    int localTanks = 0;
    int groundPower = 0;
    for (BWAPI::Unit unit : cluster.units)
    {
        if (!unit || !unit->exists() || !unit->isCompleted() ||
            !unit->getPosition().isValid() ||
            unit->getDistance(closestDefense) > 16 * 32)
        {
            continue;
        }

        const BWAPI::UnitType type = unit->getType();
        if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
            type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
        {
            ++localTanks;
        }
        else if (type == BWAPI::UnitTypes::Terran_Goliath)
        {
            groundPower += 2;
        }
        else if (type == BWAPI::UnitTypes::Terran_Vulture ||
                 type == BWAPI::UnitTypes::Terran_Marine ||
                 type == BWAPI::UnitTypes::Terran_Firebat)
        {
            ++groundPower;
        }
    }

    // A huge mobile army can clear a smaller line without siege support.
    // Count nearby mobile defenders too so this escape hatch is conservative.
    const bool overwhelming =
        groundPower >= 10 * defenses + 4 * mobileDefenders;
    return localTanks < requiredTanks && !overwhelming;
}

// Special-case units which are clustered, but arrange their own movement
// instead of accepting the cluster's movement commands.
void Squad::microSpecialUnits(const UnitCluster & cluster)
{
    // Ghost movement shares the fixed six-frame spell schedule with the squad.
    if (the.now() % 6 == 0)
    {
        BWAPI::Unit vanguard = unitClosestToTarget(cluster.units);
        if (!vanguard) vanguard = _vanguard;
        _microGhost.updateMovement(cluster, vanguard);
    }
}

// Take cluster combat actions.
// This handles cluster status of Attack and Regroup, plus Ghost spell
// opportunities while the rest of the cluster is advancing.
void Squad::clusterCombat(const UnitCluster & cluster,
    const BWAPI::Position& regroupPosition)
{
    BWAPI::Unitset eggs;
    if (cluster.status == ClusterStatus::Advance && _order.isCombatOrder())
        MicroManager::addNearbyNeutralEggTargets(cluster, eggs);
    if (cluster.status == ClusterStatus::Attack || !eggs.empty())
    {
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Air,
            [&] { _microAirToAir.execute(cluster); });
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Melee,
            [&] { _microMelee.execute(cluster); });
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Ghost,
            [&] { _microGhost.execute(cluster); });
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Ranged,
            [&] { _microRanged.execute(cluster); });
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Vultures,
            [&] { _microVultures.execute(cluster); });
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Goliaths,
            [&] { _microGoliath.execute(cluster); });
        //_microTanks.execute(cluster);

    }
    else if (cluster.status == ClusterStatus::Advance)
    {
        // A Ghost can see a Carrier through forward Goliaths before its own
        // cluster enters Attack status. Give Lockdown a chance on approach.
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Ghost,
            [&] { _microGhost.execute(cluster); });
    }
    else if (cluster.status == ClusterStatus::Regroup)
    {
        // Regroup, aka retreat. Only fighting units care about regrouping.
        _regroupPosition = regroupPosition.isValid()
            ? regroupPosition : calcRegroupPosition(cluster);

        if (Config::Debug::DrawClusters)
        {
            BWAPI::Broodwar->drawLineMap(cluster.center, _regroupPosition, BWAPI::Colors::Purple);
        }

        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Air,
            [&] { _microAirToAir.regroup(_regroupPosition, cluster); });
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Melee,
            [&] { _microMelee.regroup(_regroupPosition, cluster); });
        // Ghost movement is owned by MicroGhost even during a squad retreat.
        // Issuing generic regroup here could overwrite the cloak command sent
        // immediately after Lockdown on the same frame.
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Ranged,
            [&] { _microRanged.regroup(_regroupPosition, cluster); });
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Vultures,
            [&] { _microVultures.regroup(_regroupPosition, cluster); });

        // Large Goliath groups retreat along the same real shortest-ground
        // route used for forward movement. Do not route through Area/Choke
        // boundaries here: one ambiguous boundary used to make the whole army
        // take a triangular detour on Benzene-like open platforms.
        BWAPI::Position goliathRegroupPosition = _regroupPosition;
        if (_hasGround &&
            countGoliathsInUnits(cluster.units) >= 4 &&
            cluster.center.isValid() &&
            _regroupPosition.isValid() &&
            cluster.center.getApproxDistance(_regroupPosition) > 5 * 32)
        {
            const BWAPI::Position waypoint =
                the.map.getGroundPathWaypoint(cluster.center, _regroupPosition, 6, true);
            if (waypoint.isValid())
            {
                goliathRegroupPosition = waypoint;
            }
        }
        _lastUpdateProfile.measureMicro(SquadUpdateProfile::Goliaths,
            [&] { _microGoliath.regroup(goliathRegroupPosition, cluster); });
        //_microTanks.execute(cluster);

        // Aggressive lurkers do not regroup, but always execute their order.
        
        {
        }

        _regroupPosition = BWAPI::Positions::Invalid;       // don't reuse it by mistake
    }
}

// The cluster has no units which can fight.
// It should try to join another cluster, or else retreat to base.
bool Squad::noCombatUnits(const UnitCluster & cluster) const
{
    for (BWAPI::Unit unit : cluster.units)
    {
        if (UnitUtil::TypeCanAttack(unit->getType()))
        {
            return false;
        }
    }
    return true;
}

// The cluster has no enemies nearby.
// It will try to join another cluster, or to advance toward the goal.
bool Squad::notNearEnemy(const UnitCluster & cluster)
{
    // The generic near-enemy cache can miss air units which are already close
    // enough for Goliaths to intercept. In PureAir that left the cluster in
    // Advance, so MicroGoliath never received a combat update for one Wraith
    // or another small air contact.
    if (_name == "AntiAir" &&
        StrategyManager::Instance().getTerranArmyPosture() ==
            TerranArmyPosture::PureAir && cluster.center.isValid())
    {
        for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
            cluster.center, 16 * 32, BWAPI::Filter::IsEnemy))
        {
            if (!enemy || !enemy->exists() || !enemy->isVisible() ||
                !enemy->isDetected() || !enemy->isFlying())
            {
                continue;
            }
            for (BWAPI::Unit unit : cluster.units)
            {
                if (unit && unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Goliath &&
                    unit->getDistance(enemy) <= 12 * 32)
                {
                    return false;
                }
            }
        }
    }

    for (BWAPI::Unit unit : cluster.units)
    {
        if (_nearEnemy[unit])
        {
            return false;
        }
    }
    return true;
}

// Try to merge this cluster with one ahead of it. Return true for success.
bool Squad::joinUp(const UnitCluster & cluster)
{
    if (_name == "Ground" && TerranFormation::isBaseDefenseOrder(_order))
        for (BWAPI::Unit unit : cluster.units)
            if (unit && unit->exists() && unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
                return false;

    if (_clusters.size() < 2)
    {
        // Nobody to join up with.
        return false;
    }

    // A detached Goliath group should reconnect with the Tank mass, not the
    // first lone Tank that happened to get closer to the attack objective.
    const bool mixedGroundGoliaths =
        _name == "Ground" &&
        !TerranFormation::isBaseDefenseOrder(_order) &&
        StrategyManager::Instance().getTerranArmyPosture() != TerranArmyPosture::PureAir &&
        countGoliathsInUnits(cluster.units) >= 3 && cluster.center.isValid();
    TerranFormation::TankCore tankCore;
    if (mixedGroundGoliaths)
    {
        tankCore = TerranFormation::dominantTankCore(
            _units, cluster.center, the.partitions.id(cluster.center));
        int localTanks = 0;
        for (BWAPI::Unit unit : cluster.units)
        {
            if (unit && unit->exists() &&
                (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                 unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode))
            {
                ++localTanks;
            }
        }
        const int clusterGoalDistance = getDistance(cluster.center);
        const int coreGoalDistance = tankCore.center.isValid()
            ? getDistance(tankCore.center) : -1;
        const int clusterGoliaths = countGoliathsInUnits(cluster.units);
        const int squadGoliaths = countGoliathsInUnits(_units);
        // Only pull a reinforcement group forward to the Tank line. A large
        // Goliath body already ahead of the Tanks must not turn around and
        // abandon the attack objective to escort a lagging or idle Tank core.
        if (tankCore.center.isValid() && localTanks * 2 < tankCore.count &&
            clusterGoliaths * 2 < squadGoliaths &&
            clusterGoalDistance >= 0 && coreGoalDistance >= 0 &&
            coreGoalDistance + 2 * 32 < clusterGoalDistance &&
            cluster.center.getApproxDistance(tankCore.center) > 8 * 32)
        {
            moveCluster(cluster, tankCore.center);
            return true;
        }
    }

    // Merge into a cluster that is demonstrably ahead, or into a much larger
    // peer at the same distance. The old >= comparison made equal-distance
    // clusters chase each other every frame and circle at one point.
    int bestDistance = MAX_DISTANCE;
    const UnitCluster * bestCluster = nullptr;
    const int currentGoalDistance = getDistance(cluster.center);
    const bool supportOnlyCluster = noCombatUnits(cluster);
    const bool pureAirGoliathCore =
        _name == "AntiAir" &&
        StrategyManager::Instance().getTerranArmyPosture() ==
            TerranArmyPosture::PureAir &&
        countGoliathsInUnits(cluster.units) >= 4;
    const int currentGoliaths = pureAirGoliathCore
        ? countGoliathsInUnits(cluster.units) : 0;

    for (const UnitCluster & otherCluster : _clusters)
    {
        if (cluster.center == otherCluster.center)
        {
            continue;
        }

        if (mixedGroundGoliaths && tankCore.center.isValid() &&
            otherCluster.units.size() <= 2)
        {
            bool loneTank = false;
            for (BWAPI::Unit unit : otherCluster.units)
            {
                loneTank = loneTank ||
                    (unit && unit->exists() &&
                     (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                      unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode));
            }
            if (loneTank)
            {
                continue;
            }
        }

        // Tanks escort the PureAir Goliath core. A single Tank or detached
        // Goliath at the ramp must not become the core's join-up objective.
        if (pureAirGoliathCore &&
            countGoliathsInUnits(otherCluster.units) * 2 < currentGoliaths)
        {
            continue;
        }

        const int otherGoalDistance = getDistance(otherCluster.center);
        const bool clearlyAhead =
            currentGoalDistance >= 0 && otherGoalDistance >= 0 &&
            otherGoalDistance + 32 < currentGoalDistance;
        const bool largerPeer =
            currentGoalDistance >= 0 && otherGoalDistance >= 0 &&
            std::abs(otherGoalDistance - currentGoalDistance) <= 32 &&
            otherCluster.units.size() > 2 * cluster.units.size();
        if (!clearlyAhead && !largerPeer)
        {
            continue;
        }

        const int dist = cluster.center.getApproxDistance(otherCluster.center);
        if ((supportOnlyCluster || dist > 3 * 32) && dist < bestDistance)
        {
            bestDistance = dist;
            bestCluster = &otherCluster;
        }
    }

    if (bestCluster)
    {
        moveCluster(cluster, bestCluster->center);
        return true;
    }

    return false;
}

// Can the cluster reach the mineral line of the target base (if any)
// without coming into enemy range?
bool Squad::canReachMineralLine(const UnitCluster & cluster) const
{
	// NOTE Disabled. Needs more smarts before it's useful.
	return false;

	Base * base = _order.getBase();

	if (!base)
	{
		// We're not attacking a base.
		return false;
	}

	if (base->getOwner() != the.enemy())
	{
		// We don't want to reach the mineral line: It's not there.
		return false;
	}

	if (cluster.center.getDistance(base->getPosition()) <= 5 * 32)		// sight range of a zergling
	{
		// We have already reached the mineral line.
		return false;
	}

	if (!_order.getRaid() &&
		!base->isInBase(BWAPI::TilePosition(cluster.center)))
	{
		// Raiding squads have the goal of raiding an enemy mineral line. Otherwise,
		// don't try to dodge enemies to reach the mineral line unless we're already in the base.
		// Regular squads should be happy to fight the enemy army, and not try to bypass it.
		return false;
	}

	// NOTE The hits are updated frequently, the safe distances less often. If we're
	//      just now seeing the defense, the safe distances may not indicate it yet.
	if (the.hits(cluster.air, base->getTilePosition()))
	{
		// The target position is in enemy range.
		return false;
	}

	return base->getSafeTileDistance(cluster.air, BWAPI::TilePosition(cluster.center)) >= 0;
}

bool Squad::shouldSkipTankForClusterOrders(BWAPI::Unit unit) const
{
    if (!unit || !unit->exists())
    {
        return false;
    }

    const BWAPI::UnitType type = unit->getType();

    if (type != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
        type != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
    {
        return false;
    }

    // MicroTanks executes for every Terran composition, including bio. Letting
    // the generic cluster layer move the same tank on the same frame created a
    // second gather point and repeated command churn.
    return true;
}

// Move the cluster toward the given position.
// Parameter lazy defaults to true. If true, use MoveNear() which reduces APM and accuracy.
void Squad::moveCluster(const UnitCluster & cluster, const BWAPI::Position & destination, bool lazy)
{
    // A join-up or fallback point has a different destination from the squad
    // order. Its distance field would route the unit back toward the attack
    // target and undo the join-up command.
    const GridDistances* routeDistances = destination == _order.getPosition()
        ? _order.distances() : nullptr;
    _microVultures.moveCluster(cluster, _units, destination, lazy,
        _name == "Ground");

    std::vector<BWAPI::Unit> goliaths;
    long long tankX = 0;
    long long tankY = 0;
    int formationTanks = 0;

    const TerranArmyPosture posture =
        StrategyManager::Instance().getTerranArmyPosture();

    for (BWAPI::Unit unit : cluster.units)
    {
        if (unit &&
            unit->exists() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Goliath)
        {
            goliaths.push_back(unit);
        }

        if (unit && unit->exists() && unit->getPosition().isValid() &&
            (unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
             unit->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode))
        {
            tankX += unit->getPosition().x;
            tankY += unit->getPosition().y;
            ++formationTanks;
        }


    }

    std::sort(
        goliaths.begin(),
        goliaths.end(),
        [](BWAPI::Unit a, BWAPI::Unit b)
        {
            return a->getID() < b->getID();
        }
    );

    // Use the main Tank group for a Goliath cluster with only one or two
    // Tanks. The nearest Tank may be a detached scout far from the actual line.
    bool dominantTankExists = false;
    if (!goliaths.empty() && cluster.center.isValid() &&
        posture != TerranArmyPosture::PureAir &&
        !TerranFormation::isBaseDefenseOrder(_order) &&
        formationTanks < 3)
    {
        const TerranFormation::TankCore core =
            TerranFormation::dominantTankCore(
                _units, cluster.center, the.partitions.id(cluster.center));
        dominantTankExists = core.center.isValid();
        if (dominantTankExists)
        {
            tankX = 0;
            tankY = 0;
            formationTanks = 0;
            if (cluster.center.getApproxDistance(core.center) <= 18 * 32)
            {
                tankX = core.center.x * core.count;
                tankY = core.center.y * core.count;
                formationTanks = core.count;
            }
        }
    }

    // With no assembled Tank group, a nearby lone Tank is still a useful
    // local anchor when it is the only support available.
    if (formationTanks == 0 && !dominantTankExists &&
        !goliaths.empty() && cluster.center.isValid())
    {
        BWAPI::Unit nearestTank = nullptr;
        int nearestDistance = MAX_DISTANCE;
        for (BWAPI::Unit unit : _units)
        {
            if (!unit || !unit->exists() || !unit->getPosition().isValid() ||
                (unit->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
                 unit->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode))
            {
                continue;
            }

            const int distance = unit->getDistance(cluster.center);
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestTank = unit;
            }
        }

        if (nearestTank && nearestDistance <= 18 * 32)
        {
            tankX = nearestTank->getPosition().x;
            tankY = nearestTank->getPosition().y;
            formationTanks = 1;
        }
    }

    const BWAPI::Position mixedTankCenter = formationTanks > 0
        ? BWAPI::Position(
            int(tankX / formationTanks),
            int(tankY / formationTanks))
        : BWAPI::Positions::Invalid;

    const bool mixedOffensiveFormation =
        posture != TerranArmyPosture::PureAir &&
        mixedTankCenter.isValid() &&
        destination == _order.getPosition() &&
        (_order.getType() == SquadOrderTypes::Attack ||
         _order.getType() == SquadOrderTypes::OmniAttack);

    BWAPI::Position mixedGoliathAnchor = BWAPI::Positions::Invalid;
    const bool heavyMixed =
        StrategyManager::Instance().isTerranHeavyMixed();
    const bool lightMixed =
        StrategyManager::Instance().isTerranLightMixed();

    if (mixedOffensiveFormation && !lightMixed)
    {
        const int rearGap = heavyMixed
            ? TerranFormation::HeavyMixedRearGap
            : posture == TerranArmyPosture::Mixed
            ? TerranFormation::MixedRearGap
            : TerranFormation::GroundRearGap;
        mixedGoliathAnchor = TerranFormation::tankRearAnchor(mixedTankCenter, destination,
            rearGap);
        for (BWAPI::Unit unit : _units)
            if (unit && unit->exists() && unit->isCompleted() &&
                unit->getType() == BWAPI::UnitTypes::Terran_Goliath &&
                unit->getDistance(mixedTankCenter) <= 18 * 32 &&
                std::find(goliaths.begin(), goliaths.end(), unit) == goliaths.end())
                goliaths.push_back(unit);
        std::sort(goliaths.begin(), goliaths.end(),
            [](BWAPI::Unit a, BWAPI::Unit b) { return a->getID() < b->getID(); });
    }

    for (BWAPI::Unit unit : cluster.units)
    {
        // The bunker issues a load command to the Marine. Do not replace its
        // EnterTransport order with this squad's rally move while it walks to
        // the bunker; that makes it shuttle between the Bunker and base.
        if (unit && unit->exists() &&
            unit->getType() == BWAPI::UnitTypes::Terran_Marine &&
            unit->getOrder() == BWAPI::Orders::EnterTransport &&
            unit->getOrderTarget() &&
            unit->getOrderTarget()->getType() == BWAPI::UnitTypes::Terran_Bunker &&
            !unit->isUnderAttack())
        {
            continue;
        }
        if (unit && unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        {
            continue;
        }

        if (shouldSkipTankForClusterOrders(unit))
        {
            continue;
        }

        if (unit->getType() != BWAPI::UnitTypes::Terran_Medic &&
            unit->getType() != BWAPI::UnitTypes::Terran_Ghost)
        {
            if (!UnitUtil::MobilizeUnit(unit))
            {
                BWAPI::Position unitDestination = destination;
                bool hasAssignedGoliathSlot = false;

                if (posture != TerranArmyPosture::PureAir &&
                    unit->getType() == BWAPI::UnitTypes::Terran_Goliath &&
                    (goliaths.size() >= 4 || mixedGoliathAnchor.isValid()))
                {
                    const auto it = std::lower_bound(
                        goliaths.begin(),
                        goliaths.end(),
                        unit,
                        [](BWAPI::Unit a, BWAPI::Unit b)
                        {
                            return a->getID() < b->getID();
                        }
                    );

                    const int index = int(std::distance(goliaths.begin(), it));

                    unitDestination = goliathFormationDestination(
                        unit,
                        index,
                        int(goliaths.size()),
                        cluster.center,
                        destination,
                        mixedGoliathAnchor.isValid()
                            ? mixedGoliathAnchor
                            : destination
                    );
                    hasAssignedGoliathSlot = unitDestination != destination;
                }
                if (hasAssignedGoliathSlot)
                {
                    // The order distance field leads to the shared objective,
                    // not this Goliath's formation slot. Using it here can
                    // repeatedly pull a flank member into the Tank line.
                    if (lazy) the.micro.MoveNear(unit, unitDestination);
                    else the.micro.Move(unit, unitDestination);
                }
                else if (lazy)
                {
                    the.micro.MoveNear(unit, unitDestination, routeDistances);
                }
                else
                {
                    the.micro.Move(unit, unitDestination, routeDistances);
                }
            }
        }
    }
}

// UNUSED
// Attack-move the cluster toward the given position.
void Squad::attackMoveCluster(const UnitCluster & cluster, const BWAPI::Position & destination)
{
    for (BWAPI::Unit unit : cluster.units)
    {
        if (unit && unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        {
            _microVultures.attackMove(unit, destination);
            continue;
        }

        if (shouldSkipTankForClusterOrders(unit))
        {
            continue;
        }

        if (unit->getType() != BWAPI::UnitTypes::Terran_Medic &&
            unit->getType() != BWAPI::UnitTypes::Terran_Ghost)
        {
            if (!UnitUtil::MobilizeUnit(unit))
            {
                the.micro.AttackMove(unit, destination);
            }
        }
    }
}

// Safely move the cluster toward the given position.
void Squad::safeMoveCluster(const UnitCluster & cluster, const BWAPI::Position & destination)
{
    for (BWAPI::Unit unit : cluster.units)
    {
        if (unit && unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        {
            _microVultures.moveSafely(unit, destination);
            continue;
        }

        if (shouldSkipTankForClusterOrders(unit))
        {
            continue;
        }

        if (unit->getType() != BWAPI::UnitTypes::Terran_Medic &&
            unit->getType() != BWAPI::UnitTypes::Terran_Ghost)
        {
            if (!UnitUtil::MobilizeUnit(unit))
            {
                if (_order.getBase())
                {
                    the.micro.MoveToBase(unit, _order.getBase(), destination, true);
                }
                else
                {
                    the.micro.MoveSafely(unit, destination, _order.distances());
                }
            }
        }
    }
}

// If the unit needs to do some preparatory work before it can be put into a cluster,
// do a step of the prep work and return true.


bool Squad::isEmpty() const
{
    return _units.empty();
}

size_t Squad::getPriority() const
{
    return _priority;
}

void Squad::setPriority(const size_t & priority)
{
    _priority = priority;
}

void Squad::updateUnits()
{
    setAllUnits();
    setNearEnemyUnits();
    addUnitsToMicroManagers();
}

// Clean up the _units vector.
// Also notice and remember a few facts about the members of the squad.
// NOTE Some units may be loaded in a bunker or transport and cannot accept orders.
//      Check unit->isLoaded() before issuing orders.
void Squad::setAllUnits()
{
    _hasAir = false;
    _hasGround = false;
    _canAttackAir = false;
    _canAttackGround = false;

    BWAPI::Unitset goodUnits;
    for (BWAPI::Unit unit : _units)
    {
        if (UnitUtil::IsValidUnit(unit))
        {
            goodUnits.insert(unit);

            if (unit->isFlying())
            {
                if (!unit->getType().isDetector())    // mobile detectors don't count
                {
                    _hasAir = true;
                }
            }
            else
            {
                _hasGround = true;
            }
            if (UnitUtil::CanAttackAir(unit))
            {
                _canAttackAir = true;
            }
            if (UnitUtil::CanAttackGround(unit))
            {
                _canAttackGround = true;
            }
        }
    }
    _units = goodUnits;
}

void Squad::setNearEnemyUnits()
{
    _nearEnemy.clear();

    for (BWAPI::Unit unit : _units)
    {
        if (!unit->getPosition().isValid())   // exclude loaded units
        {
            continue;
        }

        _nearEnemy[unit] = unitNearEnemy(unit);

        if (Config::Debug::DrawSquadInfo)
        {
            if (_nearEnemy[unit])
            {
                int left = unit->getType().dimensionLeft();
                int right = unit->getType().dimensionRight();
                int top = unit->getType().dimensionUp();
                int bottom = unit->getType().dimensionDown();

                int x = unit->getPosition().x;
                int y = unit->getPosition().y;

                BWAPI::Broodwar->drawBoxMap(x - left, y - top, x + right, y + bottom,
                    Config::Debug::ColorUnitNearEnemy);
            }
        }
    }
}

// Pass the order on to all micromanagers that care about the order. (Some don't.)
void Squad::setOrderForMicroManagers()
{
    _microAirToAir.setOrder(_order);
    _microMelee.setOrder(_order);
    _microRanged.setOrder(_order);
    _microVultures.setOrder(_order);
    _microGoliath.setOrder(_order);
    _microGhost.setOrder(_order);
    _microDetectors.setOrder(_order);
    _microMedics.setOrder(_order);
    _microTanks.setOrder(_order);
    _microTransports.setOrder(_order);
}

void Squad::addUnitsToMicroManagers()
{
    BWAPI::Unitset airToAirUnits, meleeUnits, rangedUnits, vultureUnits;
    BWAPI::Unitset goliathUnits, ghostUnits, detectorUnits, transportUnits;
    BWAPI::Unitset tankUnits, medicUnits;
    for (BWAPI::Unit unit : _units)
    {
        if (!unit->isCompleted() || !unit->exists() || unit->getHitPoints() <= 0 ||
            !unit->getPosition().isValid()) continue;
        const BWAPI::UnitType type = unit->getType();
        if (type == BWAPI::UnitTypes::Terran_Valkyrie) airToAirUnits.insert(unit);
        else if (type == BWAPI::UnitTypes::Terran_Goliath) goliathUnits.insert(unit);
        else if (type == BWAPI::UnitTypes::Terran_Vulture) vultureUnits.insert(unit);
        else if (type == BWAPI::UnitTypes::Terran_Ghost) ghostUnits.insert(unit);
        else if (type == BWAPI::UnitTypes::Terran_Medic) medicUnits.insert(unit);
        else if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode ||
                 type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode) tankUnits.insert(unit);
        else if (type.isDetector() && type.isFlyer()) detectorUnits.insert(unit);
        else if (type == BWAPI::UnitTypes::Terran_Dropship) transportUnits.insert(unit);
        else if (type.groundWeapon().maxRange() > 32) rangedUnits.insert(unit);
        else if (type.isWorker() && _combatSquad)
        {
            WorkerManager::Instance().setCombatWorker(unit);
            meleeUnits.insert(unit);
        }
        else if (type.groundWeapon().maxRange() > 0 && type.groundWeapon().maxRange() <= 32)
            meleeUnits.insert(unit);
    }
    _microAirToAir.setUnits(airToAirUnits);
    _microMelee.setUnits(meleeUnits);
    _microMelee.setFormationUnits(_units);
    _microRanged.setUnits(rangedUnits);
    _microVultures.setUnits(vultureUnits);
    _microGoliath.setUnits(goliathUnits);
    _microGoliath.setFormationUnits(_units);
    _microGhost.setUnits(ghostUnits);
    _microDetectors.setUnits(detectorUnits);
    _microMedics.setUnits(medicUnits);
    _microTanks.setUnits(tankUnits);
    _microTransports.setUnits(transportUnits);
}

// Calculates whether to regroup, aka retreat. Does combat sim if necessary.
// If we don't regroup, we attack.
bool Squad::needsToRegroup(UnitCluster & cluster)
{
	cluster.setExtraText("");

    // Our order may not allow us to regroup.
    if (!_order.isRegroupableOrder())
    {
        _regroupStatus = yellow + std::string("Never retreat!");
        return false;
    }

    // Carrier combat needs its own readiness check. The old unconditional
    // "four Goliaths never retreat" rule fed small groups into large fleets.
    // Bio always reaches the ordinary combat evaluation below. Only mech
    // receives this shortcut with completed range and enough local Goliaths.
    if ((_name == "AntiAir" || _name == "Ground") &&
        (StrategyManager::Instance().getTerranArmyPosture() ==
            TerranArmyPosture::PureAir ||
         StrategyManager::Instance().getTerranArmyPosture() ==
            TerranArmyPosture::Mixed) &&
        cluster.center.isValid())
    {
        const int clusterGoliaths = countGoliathsInUnits(cluster.units);
        const bool bioArmy = StrategyManager::Instance().bioTankPlanActive();
        int visibleGroundThreats = 0;
        int visibleCarriers = 0;
        int visibleSmallAirThreats = 0;
        int visibleNonCarrierAir = 0;

        for (BWAPI::Unit enemy : BWAPI::Broodwar->getUnitsInRadius(
            cluster.center,
            12 * 32,
            BWAPI::Filter::IsEnemy))
        {
            if (enemy &&
                enemy->exists() &&
                enemy->isVisible() &&
                enemy->getType() == BWAPI::UnitTypes::Protoss_Carrier)
            {
                ++visibleCarriers;
            }
            else if (enemy && enemy->exists() && enemy->isVisible() &&
                enemy->isDetected() && enemy->isFlying())
            {
                ++visibleNonCarrierAir;
                if (UnitUtil::CanAttackGround(enemy))
                {
                    ++visibleSmallAirThreats;
                }
            }
            else if (enemy &&
                enemy->exists() &&
                enemy->isVisible() &&
                !enemy->isFlying() &&
                UnitUtil::CanAttackGround(enemy))
            {
                ++visibleGroundThreats;
            }
        }

        const bool charonCompleted =
            the.self()->getUpgradeLevel(BWAPI::UpgradeTypes::Charon_Boosters) > 0;
        const int requiredGoliaths =
            visibleCarriers <= 0
            ? 4
            : std::max(8, std::min(30, 3 * visibleCarriers + 4));

        if (!bioArmy && visibleCarriers == 0 && visibleNonCarrierAir > 0 &&
            clusterGoliaths >= std::max(3, 2 * visibleSmallAirThreats + 1) &&
            visibleGroundThreats <= std::max(1, clusterGoliaths / 3))
        {
            _regroupStatus = green + std::string("Goliath air intercept");
            return false;
        }

        if (!bioArmy && charonCompleted && clusterGoliaths >= requiredGoliaths &&
            visibleGroundThreats <= std::max(2, clusterGoliaths / 2))
        {
            _regroupStatus = green + std::string("AntiAir advance");
            return false;
        }
    }

    // If we're nearly maxed and have good income or cash, don't retreat.
    // BWAPI supply is doubled. Waiting for 390 meant the no-retreat push only
    // activated at 195/200, after a ramp simulation could already make a
    // 180+ supply army bounce down from enemy high ground repeatedly.
    if (the.self()->supplyUsed() >= 360)
    {
        _attackAtMax = true;
    }

    // Global supply does not make a small detached bio cluster win a fight.
    if (StrategyManager::Instance().bioTankPlanActive()) _attackAtMax = false;
    if (_attackAtMax)
    {
        if (the.self()->supplyUsed() < 320)
        {
            _attackAtMax = false;
        }
        else
        {
            _regroupStatus = green + std::string("Banzai!");
            return false;
        }
    }

    BWAPI::Unit vanguard = unitClosestToTarget(cluster.units);  // cluster vanguard (not squad vanguard)

    if (!vanguard)
    {
        _regroupStatus = yellow + std::string("No vanguard");
        return true;
    }

    const BWAPI::Position lastStand = finalRegroupPosition();

    // Is there static defense nearby that we should take into account?
    // The vanguard is known to be set thanks to the test immediately above.
    BWAPI::Unit defense = nearbyStaticDefense(vanguard->getPosition());
    if (defense)
    {
        // Don't retreat if we are in range of defense that is attacking.
        if (defense->getOrder() == BWAPI::Orders::AttackUnit)
        {
            _regroupStatus = green + std::string("Go defense!");
            return false;
        }

        // If there is defense to retreat to, try to get behind it wrt the enemy.
        const UnitCluster * enemyCluster = the.ops.getNearestEnemyClusterVs(cluster.center, !cluster.air, cluster.air);
        if (!enemyCluster)
        {
            _regroupStatus = green + std::string("Nothing to fear");
            return false;
        }

        if (defense->getDistance(cluster.center) < 128 &&
            cluster.center.getApproxDistance(enemyCluster->center) - defense->getDistance(enemyCluster->center) >= 32)
        {
            _regroupStatus = green + std::string("Behind defense");
            return false;
        }
    }
    else
    {
        // There is no defense to retreat to.
        // Have we retreated as far as we can?
        if (vanguard->getDistance(lastStand) < 224)
        {
            _regroupStatus = green + std::string("Back to the wall");
            return false;
        }
    }

    // -- --
    // All other checks are done. Finally do the expensive combat simulation.

    CombatSimulation & sim = the.combatSim;

    double score = 0.0;
    BioCombatResult* cached = nullptr;
    bool reuse = false;
    const UnitCluster* enemy = nullptr;
    if (StrategyManager::Instance().bioTankPlanActive())
    {
        for (auto it = _bioCombatResults.begin(); it != _bioCombatResults.end(); )
            if (the.now() < it->second.frame || the.now() - it->second.frame > 24)
                it = _bioCombatResults.erase(it);
            else ++it;
        cached = &_bioCombatResults[vanguard->getID()];
        enemy = the.ops.getNearestEnemyClusterVs(cluster.center, !cluster.air, cluster.air);
        // Re-enabling retreat at high supply must not run a full FAP battle
        // every frame. Reuse for at most 1/4 second unless the local armies change.
        reuse = the.now() >= cached->frame && the.now() - cached->frame < 6 &&
            cached->units == cluster.units.size() &&
            std::abs(cached->hp - cluster.hp) <= std::max(40, cached->hp / 10) &&
            cached->center.isValid() && cached->center.getApproxDistance(cluster.center) <= 64 &&
            cached->enemyUnits == (enemy ? enemy->units.size() : 0) &&
            std::abs(cached->enemyHP - (enemy ? enemy->hp : 0)) <= std::max(40, cached->enemyHP / 10) &&
            (!enemy || (cached->enemyCenter.isValid() &&
                cached->enemyCenter.getApproxDistance(enemy->center) <= 64));
        if (reuse) score = cached->score;
    }
    if (!reuse)
    {
        ++_lastUpdateProfile.simulations;
        _lastUpdateProfile.measure(SquadUpdateProfile::Simulation, [&]
        {
            sim.setCombatUnits(cluster.units, vanguard->getPosition(), _combatSimRadius, _fightVisibleOnly);
            score = sim.simulateCombat(_meatgrinder);
        });
        if (cached)
        {
            cached->frame = the.now();
            cached->center = cluster.center;
            cached->units = cluster.units.size();
            cached->hp = cluster.hp;
            cached->enemyUnits = enemy ? enemy->units.size() : 0;
            cached->enemyHP = enemy ? enemy->hp : 0;
            cached->enemyCenter = enemy ? enemy->center : BWAPI::Positions::Invalid;
            cached->score = score;
        }
    }

    // Use the smoothing mechanism to average out recent results.
	the.micro.setAttack(cluster.units, score >= 0.0);
    double smoothedAttack = the.micro.getSmoothedAttack(cluster.units);
	bool attack = smoothedAttack > 0.0;

	std::stringstream clusterText;
	if (Config::Debug::DrawClusters)
	{
		clusterText << white << "sim: " << (attack ? green : red) << score << " -> " << smoothedAttack;
	}

	if (Config::Debug::DrawClusters)
    {
        clusterText << (attack ? green : red) << (attack ? " go" : " back");
        cluster.setExtraText(clusterText.str());
    }

    if (attack)
    {
        _regroupStatus = green + std::string("Attack");
    }
    else
    {
        _regroupStatus = red + std::string("Retreat");

        /* Disabled because it works poorly in some important situations.
        // The combat sim says to retreat, but... can we?
        if (cluster.size() < 40)
        {
            _regroupPosition = calcRegroupPosition(cluster);
            sim.setCombatUnits(cluster.units, vanguard->getPosition(), _combatSimRadius, true);
            double retreatScore = sim.simulateRetreat(_regroupPosition);
            attack = retreatScore > 0.50;   // if losing more than this proportion, fight after all
        }
        if (attack)
        {
            _regroupStatus = green + std::string("Last stand");
        }
        */
    }

    return !attack;
}

BWAPI::Position Squad::calcRegroupPosition(const UnitCluster & cluster) const
{
    if (_name == "Flying" && cluster.air &&
        StrategyManager::Instance().airTanksPlanActive() &&
        AirTanks::variantFromName(StrategyManager::Instance().airTanksVariant()) ==
            AirTanks::Variant::TankWraith)
    {
        const auto& squads = CombatCommander::Instance().squadDiagnostics().getSquads();
        const auto ground = squads.find("Ground");
        if (ground != squads.end())
        {
            BWAPI::Unit tank = AirTanksFormation::frontTank(ground->second.getUnits());
            if (tank)
            {
                // FAP may order a retreat from strong anti-air. Pull back
                // behind the Tank line rather than all the way to the main.
                BWAPI::Position safe = tank->getPosition();
                const Base* home = the.bases.myMain();
                if (home)
                {
                    const BWAPI::Position rear = home->getPosition();
                    const int distance = std::max(1, safe.getApproxDistance(rear));
                    const int step = std::min(4 * 32, distance);
                    safe = BWAPI::Position(
                        safe.x + (rear.x - safe.x) * step / distance,
                        safe.y + (rear.y - safe.y) * step / distance);
                    safe.makeValid();
                }
                return safe;
            }
        }
    }

    // 1. Retreat toward immobile defense, if any is near.
    BWAPI::Unit vanguard = unitClosestToTarget(cluster.units);  // cluster vanguard (not squad vanguard)

    if (vanguard)
    {
        BWAPI::Unit nearest = nearbyImmobileDefense(vanguard->getPosition());
        if (nearest)
        {
            BWAPI::Position behind = DistanceAndDirection(nearest->getPosition(), vanguard->getPosition(), -128);
            return behind;
        }
    }

    // 2. Regroup toward another cluster.
    // Look for a cluster nearby, and preferably closer to the enemy.
    const int riskRadius = (!cluster.air && the.info.enemyHasSiegeMode()) ? 12 * 32 : 8 * 32;
    BWAPI::Unit closestEnemy = BWAPI::Broodwar->getClosestUnit(cluster.center, BWAPI::Filter::IsEnemy && BWAPI::Filter::CanAttack, riskRadius);
    if (!closestEnemy)
    {
        const int distToOrder = getDistance(cluster.center);        // pixels by air or ground
        const UnitCluster * bestCluster = nullptr;
        int bestScore = INT_MIN;
        for (const UnitCluster & neighbor : _clusters)
        {
            int distToNeighbor = cluster.center.getApproxDistance(neighbor.center);
            // An air cluster may join a ground cluster, but not vice versa.
            if (distToNeighbor > 0 && cluster.air >= neighbor.air)
            {
                int score = distToOrder - getDistance(neighbor.center);
                if (neighbor.status == ClusterStatus::Attack)
                {
                    score += 3 * 32;
                }
                else if (neighbor.status == ClusterStatus::Regroup)
                {
                    score -= 32;
                }
                if (score > bestScore)
                {
                    bestCluster = &neighbor;
                    bestScore = score;
                }
            }
        }
        if (bestCluster)
        {
            return bestCluster->center;
        }
    }

    // 3. Retreat to the location of the cluster unit not near the enemy which is
    // closest to the order position. This tries to stay close while still out of enemy range.
    // Units in the cluster are all air or all ground and exclude mobile detectors.
    BWAPI::Position regroup(BWAPI::Positions::None);
    int minDist = MAX_DISTANCE;
    for (BWAPI::Unit unit : cluster.units)
    {
        // Count combat units only. Bug fix originally thanks to AIL, it's been rewritten since then.
        if (unit->exists() &&
            !_nearEnemy.at(unit) &&
            unit->getType() != BWAPI::UnitTypes::Terran_Medic &&
            unit->getPosition().isValid())      // excludes loaded units
        {
            int dist = unit->getDistance(_order.getPosition());
            if (dist < minDist)
            {
                // If the squad has any ground units, don't try to retreat to the position of a unit
                // which is in a place that we cannot reach.
                if (!_hasGround || -1 != the.map.getGroundTileDistance(unit->getPosition(), _order.getPosition()))
                {
                    minDist = dist;
                    regroup = unit->getPosition();
                }
            }
        }
    }
    if (regroup.isValid())
    {
        return regroup;
    }

    // 4. Retreat to a base we own.
    return finalRegroupPosition();
}

// Return the rearmost position we should retreat to, which puts our "back to the wall".
// It is our main base or natural. 
BWAPI::Position Squad::finalRegroupPosition() const
{
    // Retreat to the main base, unless we change our mind below.
    Base * base = the.bases.myMain();

    // If the natural has been taken, retreat there instead.
    Base * natural = the.bases.myNatural();
    if (natural && natural->getOwner() == the.self())
    {
        base = natural;
    }

    return base->getPosition();
}

bool Squad::containsUnit(BWAPI::Unit u) const
{
    return _units.contains(u);
}

BWAPI::Unit Squad::nearbyImmobileGroundDefense(const BWAPI::Position & pos) const
{
    return BWAPI::Broodwar->getClosestUnit(
        pos,
        (BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Bunker ||
         BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode) &&
        BWAPI::Filter::IsOwned && BWAPI::Filter::IsCompleted,
        ImmobileDefenseRadius);
}

// Immobile defense means static defense, sieged tank, or burrowed lurker.
BWAPI::Unit Squad::nearbyImmobileDefense(const BWAPI::Position & pos) const
{
    // NOTE What matters is whether the enemy has ground or air units.
    //      We are checking the wrong thing here. But it's usually correct anyway.

    if (hasGround())
    {
        return nearbyImmobileGroundDefense(pos);
    }

    BWAPI::Unit nearest = InformationManager::Instance().nearestAirStaticDefense(pos);
    if (nearest && nearest->getDistance(pos) < ImmobileDefenseRadius)
    {
        return nearest;
    }
    return nullptr;
}

BWAPI::Unit Squad::nearbyStaticDefense(const BWAPI::Position & pos) const
{
    BWAPI::Unit nearest = nullptr;

    // NOTE What matters is whether the enemy has ground or air units.
    //      We are checking the wrong thing here. But it's usually correct anyway.
    if (hasGround())
    {
        nearest = InformationManager::Instance().nearestGroundStaticDefense(pos);
    }
    else
    {
        nearest = InformationManager::Instance().nearestAirStaticDefense(pos);
    }
    if (nearest && nearest->getDistance(pos) < ImmobileDefenseRadius)
    {
        return nearest;
    }
    return nullptr;
}

bool Squad::containsUnitType(BWAPI::UnitType t) const
{
    for (BWAPI::Unit u : _units)
    {
        if (u->getType() == t)
        {
            return true;
        }
    }
    return false;
}

void Squad::clear()
{
    for (BWAPI::Unit unit : _units)
    {
        if (unit->getType().isWorker())
        {
            WorkerManager::Instance().finishedWithWorker(unit);
        }
    }

    _units.clear();
}

bool Squad::unitNearEnemy(BWAPI::Unit unit)
{
    UAB_ASSERT(unit, "missing unit");

    // A unit is near the enemy if it can be fired on.
    if (the.hits(unit) > 0)
    {
        return true;
    }

    // NOTE The numbers match with CombatSimulation::getClosestEnemyCombatUnit().
    int safeDistance = (!unit->isFlying() && InformationManager::Instance().enemyHasSiegeMode()) ? 15*32 : 11*32;

    // For each enemy unit, visible or not.
    for (const auto & kv : InformationManager::Instance().getUnitData(BWAPI::Broodwar->enemy()).getUnits())
    {
        const UnitInfo & ui(kv.second);

        if (ui.lastPosition.isValid() && !ui.goneFromLastPosition)
        {
            if (unit->getDistance(ui.lastPosition) <= safeDistance)
            {
                return true;
            }
        }
    }
    return false;
}

// What map partition is the squad on?
// It only matters for ground units.
// Not an easy question. The different units might be on different partitions.
// We simply pick a unit, any unit, and assume that that gives the partition.
int Squad::mapPartition() const
{
    // Default to our starting position.
    BWAPI::Position pos = the.bases.myStart()->getPosition();

    // Pick any unit with a position on the map (not, for example, in a bunker).
    for (BWAPI::Unit unit : _units)
    {
        if (unit->getPosition().isValid())
        {
            pos = unit->getPosition();
            break;
        }
    }

    return the.partitions.id(pos);
}

// NOTE The squad center is a geometric center. It ignores terrain.
// The center might be on unwalkable ground, or even on a different island.
BWAPI::Position Squad::calcCenter() const
{
    if (_units.empty())
    {
        return the.bases.myStart()->getPosition();
    }

    BWAPI::Position accum(0,0);
    for (BWAPI::Unit unit : _units)
    {
        if (unit->getPosition().isValid())
        {
            accum += unit->getPosition();
        }
    }
    return BWAPI::Position(accum.x / _units.size(), accum.y / _units.size());
}

BWAPI::Unit Squad::unitClosestToPosition(const BWAPI::Position & pos, const BWAPI::Unitset & units) const
{
    UAB_ASSERT(pos.isValid(), "bad order position");

    BWAPI::Unit closest = nullptr;
    int closestDist = MAX_DISTANCE;

    for (BWAPI::Unit unit : units)
    {
        // Non-combat units should be ignored for this calculation.
        // If the cluster contains only these units, we'll return null.
        if (unit->getType().isDetector() ||
            !unit->getPosition().isValid() ||       // includes units loaded into bunkers or transports
            unit->getType() == BWAPI::UnitTypes::Terran_Medic)
        {
            continue;
        }

        int dist;
        if (_hasGround)
        {
            // A ground or air-ground group. Use ground distance.
            // It is -1 if no ground path exists.
            dist = the.map.getGroundDistance(unit->getPosition(), pos);
        }
        else
        {
            // An all-air group. Use air distance (which is what unit->getDistance() gives).
            dist = unit->getDistance(pos);
        }

        if (dist < closestDist && dist != -1)
        {
            closest = unit;
            closestDist = dist;
        }
    }

    return closest;
}

// Return the unit closest to the order position.
// The order position might be an enemy position, or a defense position in our base.
BWAPI::Unit Squad::unitClosestToTarget(const BWAPI::Unitset & units) const
{
    return unitClosestToPosition(_order.getPosition(), units);
}

const BWAPI::Unitset & Squad::getUnits() const	
{ 
    return _units; 
} 

void Squad::setOrder(SquadOrder so)
{
    if (_order != so)
    {
        _orderFrame = the.now();
    }

    // Always move the order, to avoid memory leaks.
    _order = std::move(so);     // `so` should not be reused
    setOrderForMicroManagers();
}

SquadOrder & Squad::getOrder()
{ 
    return _order; 
}



const std::string Squad::getRegroupStatus() const
{
    return _regroupStatus;
}

void Squad::addUnit(BWAPI::Unit u)
{
    _units.insert(u);
}

void Squad::removeUnit(BWAPI::Unit u)
{
    if (_combatSquad && u->getType().isWorker())
    {
        WorkerManager::Instance().finishedWithWorker(u);
    }
    _units.erase(u);
}

// Remove all workers from the squad, releasing them back to WorkerManager.
void Squad::releaseWorkers()
{
    for (auto it = _units.begin(); it != _units.end(); )
    {
        if (_combatSquad && (*it)->getType().isWorker())
        {
            WorkerManager::Instance().finishedWithWorker(*it);
            it = _units.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

const std::string & Squad::getName() const
{
    return _name;
}

// This is a watch squad. Handle special cases.
// 1. If a unit is at its assigned watch position, burrow it if possible.
// 2. If a unit is burrowed and no detector is in view, leave it there.
// If there's an enemy detector, stay above ground to fight or flee.


// The drop squad has been given a Load order. Load up the transports for a drop.
// Unlike other code in the drop system, this supports any number of transports, including zero.
// Called once per frame while a Load order is in effect.
void Squad::loadTransport()
{
    for (BWAPI::Unit trooper : _units)
    {
        // If it's not the transport itself, send it toward the order location,
        // which is set to the transport's initial location.
        if (trooper->exists() && !trooper->isLoaded() && trooper->getType().spaceProvided() == 0)
        {
            the.micro.Move(trooper, _order.getPosition());
        }
    }

    for (const auto transport : _microTransports.getUnits())
    {
        if (!transport->exists())
        {
            continue;
        }
        
        for (BWAPI::Unit unit : _units)
        {
            if (transport->getSpaceRemaining() == 0)
            {
                break;
            }

            if (the.micro.Load(transport, unit))
            {
                break;
            }
        }
    }
}

// Reserve a conservative healing budget only from a nearby available Medic.
static BWAPI::Unit localStimMedic(BWAPI::Unit infantry,
    const BWAPI::Unitset& medics, const std::map<int, int>& committedEnergy)
{
    BWAPI::Unit closest = nullptr;
    int distance = 7 * 32 + 1;
    for (BWAPI::Unit medic : medics)
    {
        if (!medic || !medic->exists() || !medic->isCompleted() || medic->isLoaded() ||
            !medic->getPosition().isValid() || medic->isStasised() ||
            medic->isMaelstrommed() || medic->isLockedDown()) continue;
        const auto committed = committedEnergy.find(medic->getID());
        const int reserved = committed == committedEnergy.end() ? 0 : committed->second;
        if (medic->getEnergy() - reserved < 10 ||
            !the.partitions.connected(infantry->getPosition(), medic->getPosition())) continue;
        const int d = medic->getDistance(infantry);
        if (d < distance)
        {
            distance = d;
            closest = medic;
        }
    }
    return closest;
}

// Stim marines and firebats if possible and appropriate.
// This stims for combat. It doesn't consider stim to travel faster.
// We bypass the micro managers because it simplifies the bookkeeping, but a disadvantage
// is that we don't have access to the target list. Should refactor a little to get that,
// because it can help us figure out how important it is to stim.
void Squad::stimIfNeeded()
{
    // Do we have stim?
    if (BWAPI::Broodwar->self()->getRace() != BWAPI::Races::Terran ||
        !BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Stim_Packs))
    {
        return;
    }

    // Are there enemies nearby that we may want to fight?
    if (_nearEnemy.empty())
    {
        return;
    }

    // So far so good. Time to get into details.

    const int now = the.now();
    if (now < _lastStimReservationFrame) _stimHealingReservations.clear();
    _lastStimReservationFrame = now;
    std::map<int, int> committedMedicEnergy;
    for (auto it = _stimHealingReservations.begin(); it != _stimHealingReservations.end(); )
    {
        BWAPI::Unit medic = BWAPI::Broodwar->getUnit(it->first);
        if (!medic || !medic->exists() || now >= it->second.second)
            it = _stimHealingReservations.erase(it);
        else
        {
            committedMedicEnergy[it->first] = it->second.first;
            ++it;
        }
    }

    // Stim costs 10 HP, which requires 5 energy for a medic to heal.
    // To reduce overstim, pretend it costs more than that.
    const int stimEnergyCost = 10;

    // Firebats first, because they are likely to be right up against the enemy.
    for (auto firebat : _microMelee.getUnits())
    {
        // Invalid position means the firebat is probably in a bunker or transport.
        if (firebat->getType() != BWAPI::UnitTypes::Terran_Firebat || !firebat->getPosition().isValid())
        {
            continue;
        }
        // Don't overstim and lose too many HP.
        BWAPI::Unit medic = localStimMedic(firebat, _microMedics.getUnits(), committedMedicEnergy);
        if (firebat->isStimmed() || firebat->getHitPoints() < 35 ||
            (!medic && firebat->getHitPoints() < 45))
        {
            continue;
        }

        BWAPI::Unitset nearbyEnemies;
        MapGrid::Instance().getUnits(nearbyEnemies, firebat->getPosition(), 64, false, true);

        bool firebatCanFight = false;
        for (BWAPI::Unit enemy : nearbyEnemies)
        {
            if (enemy && enemy->exists() && enemy->isVisible() &&
                enemy->isDetected() && !enemy->isInvincible() &&
                UnitUtil::CanAttack(firebat, enemy) &&
                !enemy->isUnderDarkSwarm())
            {
                firebatCanFight = true;
                break;
            }
        }
        if (firebatCanFight)
        {
            if (the.micro.Stim(firebat))
            {
                if (medic)
                {
                    const int reserved = committedMedicEnergy[medic->getID()] += stimEnergyCost;
                    _stimHealingReservations[medic->getID()] =
                        { reserved, now + 3 * 24 + BWAPI::Broodwar->getLatencyFrames() };
                }
            }
        }
    }

    // Next marines, treated the same except for range and hit points.
    for (auto marine : _microRanged.getUnits())
    {
        // Invalid position means the marine is probably in a bunker or transport.
        if (marine->getType() != BWAPI::UnitTypes::Terran_Marine || !marine->getPosition().isValid())
        {
            continue;
        }
        // Don't overstim and lose too many HP.
        BWAPI::Unit medic = localStimMedic(marine, _microMedics.getUnits(), committedMedicEnergy);
        if (marine->isStimmed() || marine->getHitPoints() <= 30 ||
            (!medic && marine->getHitPoints() < 40))
        {
            continue;
        }

        BWAPI::Unitset nearbyEnemies;
        MapGrid::Instance().getUnits(nearbyEnemies, marine->getPosition(), 5 * 32, false, true);

        BWAPI::Unitset supportingMedics;
        if (medic) supportingMedics.insert(medic);
        if (MicroMarines::shouldStim(
                marine, supportingMedics, nearbyEnemies))
        {
            if (the.micro.Stim(marine))
            {
                if (medic)
                {
                    const int reserved = committedMedicEnergy[medic->getID()] += stimEnergyCost;
                    _stimHealingReservations[medic->getID()] =
                        { reserved, now + 3 * 24 + BWAPI::Broodwar->getLatencyFrames() };
                }
            }
        }
    }
}

bool Squad::hasCombatUnits() const
{
    // If the only units we have are detectors, then we have no combat units.
    return !(_units.empty() || _units.size() == _microDetectors.getUnits().size());
}

// Is every unit in the squad an overlord hunter (or a detector)?
// An overlord hunter is a fast air unit that is strong against overlords.
bool Squad::isOverlordHunterSquad() const
{
    if (!hasCombatUnits())
    {
        return false;
    }

    for (BWAPI::Unit unit : _units)
    {
        const BWAPI::UnitType type = unit->getType();
        if (!type.isFlyer())
        {
            return false;
        }
        if (!type.isDetector() &&
            type != BWAPI::UnitTypes::Terran_Wraith &&
            type != BWAPI::UnitTypes::Terran_Valkyrie)
        {
            return false;
        }
    }
    return true;
}

// Pixel distance from the given tile to the order position.
int Squad::getDistance(const BWAPI::TilePosition & tile) const
{
    if (_order.distances())
    {
        return 32 * _order.distances()->at(tile);
    }

    return _order.getPosition().getApproxDistance(TileCenter(tile));
}

// Pixel distance from the given position to the order position.
int Squad::getDistance(const BWAPI::Position & pos) const
{
    if (_order.distances())
    {
        return 32 * _order.distances()->at(pos);
    }

    return _order.getPosition().getApproxDistance(pos);
}

void Squad::drawCluster(const UnitCluster & cluster) const
{
    if (Config::Debug::DrawClusters)
    {
        cluster.draw(BWAPI::Colors::Grey, white + _name + ' ' + _regroupStatus);
    }
}
