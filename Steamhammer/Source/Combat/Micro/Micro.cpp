
#include "Map/Bases.h"
#include "Intelligence/InformationManager.h"
#include "Map/MapGrid.h"
#include "Core/The.h"
#include "Utilities/UnitUtil.h"

using namespace UAlbertaBot;

size_t TotalCommands = 0;  // not all commands are counted

namespace
{
    BWAPI::Position g_lastComsatScanPosition = BWAPI::Positions::Invalid;
    int g_lastComsatScanFrame = -100000;
    int g_lastComsatScanComsatID = -1;
    int g_lastComsatScanEnergy = 0;

    std::map<int, int> g_lastTankSiegeCommandFrame;
    std::map<int, int> g_lastTankUnsiegeCommandFrame;
    std::map<int, int> g_tankSiegeHoldUntil;
    int g_lastTankTransitionStateFrame = -100000;

    // A yield point is at most four build tiles away. Search only a tiny local
    // window; the general A* path finder allocates full-map arrays even for a
    // two-tile query and caused avoidable frame spikes on blocked maps.
    bool shortTrafficYieldPath(
        const BWAPI::TilePosition& from,
        const BWAPI::TilePosition& to)
    {
        if (!from.isValid() || !to.isValid()) return false;
        constexpr int radius = 6;
        constexpr int side = radius * 2 + 1;
        constexpr int cells = side * side;
        bool visited[side][side] = {};
        BWAPI::TilePosition queue[cells];
        int steps[cells] = {};
        int head = 0;
        int tail = 1;
        queue[0] = from;
        visited[radius][radius] = true;

        const int offsets[8][2] = {
            { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
            { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }
        };
        while (head < tail)
        {
            const BWAPI::TilePosition current = queue[head];
            const int depth = steps[head++];
            if (current == to) return true;
            if (depth >= 10) continue;
            for (const auto& offset : offsets)
            {
                const BWAPI::TilePosition next(
                    current.x + offset[0], current.y + offset[1]);
                const int x = next.x - from.x + radius;
                const int y = next.y - from.y + radius;
                if (!next.isValid() || x < 0 || x >= side ||
                    y < 0 || y >= side || visited[x][y] ||
                    !the.map.isGroundTransitionRuntimeWalkable(current, next))
                {
                    continue;
                }
                visited[x][y] = true;
                queue[tail] = next;
                steps[tail++] = depth + 1;
            }
        }
        return false;
    }

    // BWAPI may report the previous order until command latency has elapsed.
    // Keep delayed Move/Attack/Hold updates from replacing an accepted mode
    // change during that window or during the actual transformation.
    bool tankTransitionPending(BWAPI::Unit unit)
    {
        if (!unit || !unit->exists() ||
            (unit->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
             unit->getType() != BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode))
        {
            return false;
        }
        if (unit->getOrder() == BWAPI::Orders::Sieging ||
            unit->getOrder() == BWAPI::Orders::Unsieging)
        {
            return true;
        }
        int lastTransition = -100000;
        const auto siege = g_lastTankSiegeCommandFrame.find(unit->getID());
        const auto unsiege = g_lastTankUnsiegeCommandFrame.find(unit->getID());
        if (siege != g_lastTankSiegeCommandFrame.end())
        {
            lastTransition = siege->second;
        }
        if (unsiege != g_lastTankUnsiegeCommandFrame.end())
        {
            lastTransition = std::max(lastTransition, unsiege->second);
        }
        const int now = the.now();
        return now >= lastTransition &&
            now - lastTransition <= std::max(1, BWAPI::Broodwar->getLatencyFrames());
    }

    void resetTankTransitionStateForNewGame(int now)
    {
        if (now < g_lastTankTransitionStateFrame)
        {
            g_lastTankSiegeCommandFrame.clear();
            g_lastTankUnsiegeCommandFrame.clear();
            g_tankSiegeHoldUntil.clear();
        }
        else if (now != g_lastTankTransitionStateFrame)
        {
            for (auto it = g_tankSiegeHoldUntil.begin(); it != g_tankSiegeHoldUntil.end(); )
            {
                if (it->second <= now)
                {
                    it = g_tankSiegeHoldUntil.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        g_lastTankTransitionStateFrame = now;
    }

    void rememberComsatScan(BWAPI::Unit comsat, const BWAPI::Position& targetPosition, int energyBeforeScan)
    {
        g_lastComsatScanPosition = targetPosition;
        g_lastComsatScanFrame = BWAPI::Broodwar->getFrameCount();
        g_lastComsatScanComsatID = comsat ? comsat->getID() : -1;
        g_lastComsatScanEnergy = energyBeforeScan;

        BWAPI::TilePosition tile(targetPosition);

        DebugTacticalPrintf(
            "COMSAT SCAN fired: comsat=%d energy=%d pos=(%d,%d) tile=(%d,%d)",
            g_lastComsatScanComsatID,
            g_lastComsatScanEnergy,
            targetPosition.x,
            targetPosition.y,
            tile.x,
            tile.y
        );
    }

    void drawLastComsatScan()
    {
        if (!Config::Debug::DrawUnitTargets)
        {
            return;
        }

        if (!g_lastComsatScanPosition.isValid())
        {
            return;
        }

        const int now = BWAPI::Broodwar->getFrameCount();
        const int age = now - g_lastComsatScanFrame;

        // Draw marker for about 12 seconds.
        if (age > 24 * 12)
        {
            return;
        }

        const BWAPI::Position p = g_lastComsatScanPosition;

        // Big visible marker on the map.
        BWAPI::Broodwar->drawCircleMap(p, 96, BWAPI::Colors::Cyan, false);
        BWAPI::Broodwar->drawCircleMap(p, 160, BWAPI::Colors::Cyan, false);

        // Cross mark.
        BWAPI::Broodwar->drawLineMap(
            BWAPI::Position(p.x - 160, p.y),
            BWAPI::Position(p.x + 160, p.y),
            BWAPI::Colors::Cyan
        );

        BWAPI::Broodwar->drawLineMap(
            BWAPI::Position(p.x, p.y - 160),
            BWAPI::Position(p.x, p.y + 160),
            BWAPI::Colors::Cyan
        );

        BWAPI::Broodwar->drawTextMap(
            p.x - 64,
            p.y - 24,
            "COMSAT SCAN"
        );

        BWAPI::Broodwar->drawTextScreen(
            10,
            165,
            "Last scan: comsat=%d energy=%d pos=(%d,%d) age=%d",
            g_lastComsatScanComsatID,
            g_lastComsatScanEnergy,
            p.x,
            p.y,
            age
        );
    }
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
// Utility routine.

// Is the distance inside the target distance?
bool distanceIs(int x, int y, const Grid * distances, int target)
{
	BWAPI::TilePosition xy(x, y);
	return xy.isValid() && distances->at(xy) < target && distances->at(xy) >= 0;
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// Complain if there is an "obvious" problem.
// So far, the only problem detected is issuing two orders during the same frame.
void MicroState::check(BWAPI::Unit u, BWAPI::Order o) const
{
    return;	// TODO only continue when debugging

    if (orderFrame == the.now())
    {
        BWAPI::Broodwar->printf(">1 order for %s %d frame %d, %s -> %s",
            UnitTypeName(u).c_str(), u->getID(), the.now(),
            order.getName().c_str(), o.getName().c_str());
    }
}

// Execute the unit's order.
void MicroState::execute(BWAPI::Unit u)
{
    if (order == BWAPI::Orders::Move)
    {
		followupTarget = nextDestination(u);

        if (u->getPosition() != followupTarget && the.now() >= lastActionFrame + FramesBetweenActions)
        {
			lastActionFrame = the.now();
			if (u->move(followupTarget))
            {
                lastCheckFrame = executeFrame = lastActionFrame;
                needsMonitoring = true;
            }
        }
    }
    else
    {
        // It's an order we don't support, so don't waste time trying.
        executeFrame = 0;
        needsMonitoring = false;
    }
}

// Follow up on the order: It is executed in parts over time.
void MicroState::followup(BWAPI::Unit u)
{
	const int FollowupInterval = 23;		// follow up after this long

	if (order == BWAPI::Orders::Move && targetBase)
	{
		if (the.now() >= lastActionFrame + FollowupInterval)
		{
			BWAPI::Position t = nextDestination(u);
			if (t != followupTarget)
			{
				followupTarget = t;
				lastActionFrame = the.now();

				if (u->move(t))
				{
					lastCheckFrame = lastActionFrame;
				}
			}
		}
		else if (the.now() > lastCheckFrame + BWAPI::Broodwar->getLatencyFrames())
		{
			// Dodge the enemy if necessary.
			BWAPI::Unit enemy = the.micro.inWeaponsDanger(u, DangerMargin);
			if (enemy)
			{
				lastActionFrame = the.now();
				lastCheckFrame = lastActionFrame;
				(void) u->move(dodge(u));
			}
		}
	}
	else
	{
		// Can't follow up on it, so don't waste time trying.
		needsFollowup = false;
	}
}

// Reset progress tracking whenever a materially new order is stored.
void MicroState::resetMoveProgress(BWAPI::Unit u)
{
    progressPosition = u && u->getPosition().isValid()
        ? u->getPosition()
        : BWAPI::Positions::Invalid;
    progressDistance =
        u && targetPosition.isValid()
        ? u->getDistance(targetPosition)
        : MAX_DISTANCE;
    lastProgressFrame = the.now();
    recoveryPosition = BWAPI::Positions::Invalid;
    recoveryUntilFrame = -1;
    recoveryAttempts = 0;
}

// Pick a short local move that can break friendly-unit congestion without
// throwing away the real destination. This is intentionally used only after a
// Move order has failed to make progress for a while, so the extra path checks
// do not sit on the normal hot path.
BWAPI::Position MicroState::findMoveRecoveryPosition(BWAPI::Unit u) const
{
    if (!u || !u->exists() || u->isFlying() ||
        !u->getPosition().isValid() || !targetPosition.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    const BWAPI::Position current = u->getPosition();
    const BWAPI::TilePosition currentTile(current);
    if (!currentTile.isValid())
    {
        return BWAPI::Positions::Invalid;
    }

    std::vector<BWAPI::Position> candidates;

    // First try the runtime A* route. This is the best answer when a Factory,
    // Bunker or other grounded building appeared after the cached route was
    // created.
    const BWAPI::Position route =
        the.map.getGroundPathWaypoint(current, targetPosition, 3, true);
    if (route.isValid() && current.getApproxDistance(route) >= 24)
    {
        candidates.push_back(route);
    }

    double dx = double(targetPosition.x - current.x);
    double dy = double(targetPosition.y - current.y);
    double length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0)
    {
        return candidates.empty() ? BWAPI::Positions::Invalid : candidates.front();
    }

    dx /= length;
    dy /= length;
    const double sx = -dy;
    const double sy = dx;

    // Small forward/side steps are enough to get around another tank/goliath.
    // Try both sides and two widths. A slight backwards-side option is kept as
    // a last resort for units wedged against a building corner.
    const int forwardSteps[] = { 48, 48, 32, 32, -16, -16, -48, -48 };
    const int sideSteps[]    = { 56, -56, 88, -88, 72, -72, 88, -88 };
    for (int i = 0; i < 8; ++i)
    {
        const BWAPI::Position candidate(
            int(current.x + dx * forwardSteps[i] + sx * sideSteps[i]),
            int(current.y + dy * forwardSteps[i] + sy * sideSteps[i]));
        if (candidate.isValid())
        {
            candidates.push_back(candidate);
        }
    }

    BWAPI::Position best = BWAPI::Positions::Invalid;
    int bestScore = MAX_DISTANCE;
    const int currentThreat = the.hits(u, currentTile);
    const int currentPartition = the.partitions.id(currentTile);

    for (const BWAPI::Position& candidate : candidates)
    {
        if (!candidate.isValid() || current.getApproxDistance(candidate) < 20)
        {
            continue;
        }

        const BWAPI::TilePosition tile(candidate);
        if (!tile.isValid() || !the.map.isRuntimeWalkable(tile))
        {
            continue;
        }

        if (currentPartition > 0 && the.partitions.id(tile) != currentPartition)
        {
            continue;
        }

        // Avoid repeatedly selecting the same failed nudge.
        int repeatPenalty = 0;
        if (recoveryPosition.isValid() &&
            recoveryPosition.getApproxDistance(candidate) < 32)
        {
            repeatPenalty = 4 * 32;
        }

        int crowd = 0;
        int tankInterference = 0;
        bool insideTank = false;
        for (BWAPI::Unit friendly : the.self()->getUnits())
        {
            if (!friendly || !friendly->exists() || friendly == u ||
                friendly->isFlying() || friendly->getType().isBuilding() ||
                !friendly->getPosition().isValid())
            {
                continue;
            }

            const int d = friendly->getDistance(candidate);
            const BWAPI::UnitType type = friendly->getType();
            if (type == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
                type == BWAPI::UnitTypes::Terran_Siege_Tank_Siege_Mode)
            {
                // A route waypoint can be walkable terrain while a Tank fills
                // the actual lane. Never choose a nudge inside its footprint,
                // and prefer one that goes around it rather than through it.
                const int clearance = u->getType().dimensionLeft() +
                    type.dimensionRight() + 8;
                if (d < clearance)
                {
                    insideTank = true;
                    break;
                }

                const double vx = double(candidate.x - current.x);
                const double vy = double(candidate.y - current.y);
                const double lengthSquared = vx * vx + vy * vy;
                if (lengthSquared > 1.0)
                {
                    const BWAPI::Position tankPosition = friendly->getPosition();
                    const double projection = std::max(0.0, std::min(1.0,
                        ((tankPosition.x - current.x) * vx +
                         (tankPosition.y - current.y) * vy) / lengthSquared));
                    const double gapX = tankPosition.x - (current.x + projection * vx);
                    const double gapY = tankPosition.y - (current.y + projection * vy);
                    if (gapX * gapX + gapY * gapY < double(clearance * clearance))
                        tankInterference += 1;
                }
            }
            if (d < 40)
            {
                crowd += 3;
            }
            else if (d < 72)
            {
                ++crowd;
            }
        }
        if (insideTank) continue;

        const int threat = the.hits(u, tile);
        const int extraThreat = std::max(0, threat - currentThreat);
        const int directness = candidate.getApproxDistance(targetPosition);
        const int score =
            directness +
            crowd * 48 +
            tankInterference * 8 * 32 +
            extraThreat * 6 * 32 +
            repeatPenalty;

        if (score < bestScore)
        {
            bestScore = score;
            best = candidate;
        }
    }

    return best;
}

// Monitor the order: Check for and try to correct failure to execute.
void MicroState::monitor(BWAPI::Unit u)
{
    if (order != BWAPI::Orders::Move)
    {
        // It's an order we don't support. Don't waste time trying.
        needsMonitoring = false;
        return;
    }

    if (u->isFlying())
    {
        // Units that don't benefit.
        needsMonitoring = false;
        return;
    }

    if (!targetPosition.isValid())
    {
        needsMonitoring = false;
        return;
    }

    const int now = the.now();
    const int currentDistance = u->getDistance(targetPosition);

    // Exact pixel equality is too strict for large ground units. If we're
    // essentially on the requested point, stop trying to micro-correct it.
    if (currentDistance <= 12)
    {
        needsMonitoring = false;
        recoveryUntilFrame = -1;
        return;
    }

    // During a recovery nudge, do not immediately interpret the intentionally
    // different last Move target as a missing command. Let the unit get clear,
    // then restore the original destination below.
    if (recoveryUntilFrame > now)
    {
        lastCheckFrame = now;
        return;
    }
    if (recoveryUntilFrame >= 0)
    {
        recoveryUntilFrame = -1;
        if (u->move(targetPosition))
        {
            lastCheckFrame = now;
            lastActionFrame = now;
            progressPosition = u->getPosition();
            progressDistance = currentDistance;
            lastProgressFrame = now;
        }
        return;
    }

    // Record real progress. Sideways movement around an obstacle also counts;
    // tiny collision jitter does not.
    const int moved = progressPosition.isValid()
        ? progressPosition.getApproxDistance(u->getPosition())
        : MAX_DISTANCE;
    const int distanceImprovement =
        progressDistance == MAX_DISTANCE
        ? 0
        : progressDistance - currentDistance;

    if (!progressPosition.isValid() || moved >= 24 || distanceImprovement >= 16)
    {
        progressPosition = u->getPosition();
        progressDistance = currentDistance;
        lastProgressFrame = now;
        if (distanceImprovement >= 16)
        {
            recoveryAttempts = std::max(0, recoveryAttempts - 1);
        }
    }

    // 1. The command might not have been accepted despite the return value.
    if (u->getOrder() != BWAPI::Orders::Move ||
        u->getLastCommand().getType() != BWAPI::UnitCommandTypes::Move ||
        u->getLastCommand().getTargetPosition() != targetPosition)
    {
        if (u->move(targetPosition))
        {
            lastCheckFrame = now;
            lastActionFrame = now;
        }
        return;
    }

    // 2/3/4. Stuck, frozen, or blocked by friendly traffic. BWAPI's isStuck()
    // may stay false while a unit is continuously trying to walk through a
    // packed formation, so progress time is the primary signal.
    int blockedFrames = 26;
    if (u->getType() == BWAPI::UnitTypes::Terran_Vulture)
    {
        blockedFrames = 18;
    }
    else if (u->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode ||
             u->getType() == BWAPI::UnitTypes::Terran_Goliath)
    {
        blockedFrames = 24;
    }
    else if (u->getType().isWorker())
    {
        blockedFrames = 30;
    }

    const int noProgressFrames = now - lastProgressFrame;
    const bool stopped =
        u->getVelocityX() == 0.0 && u->getVelocityY() == 0.0;
    const bool blocked =
        (u->isStuck() && noProgressFrames >= 10) ||
        (noProgressFrames >= blockedFrames && stopped) ||
        noProgressFrames >= blockedFrames + 12;

    if (!blocked)
    {
        lastCheckFrame = now;
        return;
    }

    // Moving the blocked unit sideways can leave the actual lane blocked.
    // Give an idle ally in the choke a chance to step clear first.
    if (the.micro.yieldBlockingUnit(u, targetPosition))
    {
        lastCheckFrame = now;
        return;
    }

    BWAPI::Position recovery = findMoveRecoveryPosition(u);
    if (recovery.isValid() && u->move(recovery))
    {
        recoveryPosition = recovery;
        // Long enough to clear one unit footprint, short enough that combat
        // code regains control quickly.
        recoveryUntilFrame = now + std::min(14, 7 + recoveryAttempts * 2);
        ++recoveryAttempts;
        lastCheckFrame = now;
        lastActionFrame = now;

        if (Config::Debug::DrawUnitTargets)
        {
            BWAPI::Broodwar->drawLineMap(
                u->getPosition(), recovery, BWAPI::Colors::Yellow);
            BWAPI::Broodwar->drawTextMap(
                u->getPosition() + BWAPI::Position(0, 8), "unstick");
        }
        return;
    }

    // No safe local escape was found. Stop once to clear StarCraft's internal
    // path state; the original Move will be reissued by the next monitor pass.
    u->stop();
    lastCheckFrame = now;
    lastActionFrame = now;
    lastProgressFrame = now;
}

// The two positions are so close together that we can treat them as the same
// for purposes of orders which take a position.
bool MicroState::positionsNearlyEqual(BWAPI::Unit u, const BWAPI::Position & pos1, const BWAPI::Position & pos2) const
{
    UAB_ASSERT(pos1.isValid() && pos2.isValid(), "bad position");

    int dist = u->getDistance(pos2);
    int tolerance =
        dist <= 6 * 32
          ? 3                                   // tightest tolerance
          : (dist <= 12 * 32                    // within tank range?
              ? 6                               // yes, close tolerance
              : std::min(4 * 32, dist / 32));   // no, be looser

    return
        abs(pos1.x - pos2.x) <= tolerance &&
        abs(pos1.y - pos2.y) <= tolerance;
}

// Return the position the unit should be ordered to,
// either the targetPosition, or an intermediate step toward it,
// or a position to dodge to, depending on the orders and situation.
// The distances grid gives distance to a base resouce depot, and the
// target position is expected to be near but not always exactly there.
// If we're trying to be safe, then the distances grid will avoid known
// enemies as of the time of last update, but there may be unexpected enemy
// fire. Try to dodge it reactively.
// If we're as close to the target position as the base position,
// go to the target position.
// The return value is valid if the targetPosition is valid.
BWAPI::Position MicroState::nextDestination(BWAPI::Unit u) const
{
    // Base distance fields are rooted at the depot and may pull a returning
    // unit into the Supply Depot side of a Terran wall. Cross at the Barracks
    // opening first; after the unit reaches the far side, follow the base grid.
    if (!u->isFlying() && targetPosition.isValid())
    {
        const BWAPI::Position gate =
            the.placer.terranMainWallTransitWaypoint(
                u->getPosition(), targetPosition, u->getID());
        if (gate.isValid()) return gate;
    }
	if (!targetBase || !u->getTilePosition().isValid())
	{
		return targetPosition;
	}

	const Grid * distances;
	if (safe)
	{
		distances = u->isFlying()
			? &dynamic_cast<Grid &>(targetBase->getAirSafePaths())
			: &dynamic_cast<Grid &>(targetBase->getGroundSafePaths());
	}
	else
	{
		distances = u->isFlying() ? nullptr : &dynamic_cast<Grid &>(targetBase->getDistances());
	}

	// A null distances grid means don't use distances. We're flying and not trying to be safe,
	// so just go straight to the target position.
	if (!distances)
	{
		return targetPosition;
	}

	const int StepSize = 8;
	int here = distances->at(u);		// tile distance from the target base

	// Cases where we go to the target position, but may dodge if trying to be safe.
	if (
		// We're either near, or we can't get there - same answer for both.
		here < StepSize ||

		// We're closer to the target than to the base.
		32 * here >= u->getDistance(targetPosition) ||

		// We're already at the base. Just go to the target.
		targetBase->isInBase(u))
	{
		if (safe && the.micro.inWeaponsDanger(u, DangerMargin))
		{
			return dodge(u);
		}
		return targetPosition;
	}

	// Use the distances map to find our next waypoint.
	// One tile of every StepSize tiles along the path is a waypoint.
	// When we are halfway between waypoints, we switch to the following one.
	// In other words, if we're going to X and we're halfway between B and C, we switch from waypoint C to D.
	const int phase = here % StepSize;
	const int target = std::max(0, here - phase - (phase > StepSize / 2 ? 0 : StepSize));

	int x = u->getTilePosition().x;
	int y = u->getTilePosition().y;
	while (here > target)
	{
		UAB_ASSERT(BWAPI::TilePosition(x, y).isValid(), "bad tile %d,%d", x, y);
		UAB_ASSERT(distances->at(BWAPI::TilePosition(x, y)) >= 0, "inaccessible tile %d,%d", x, y);

		// Choose among ALL lower-distance neighbors. The legacy code checked
		// diagonals in a fixed NW->NE->SE->SW order, which created a map-wide
		// directional bias: a unit moving east could choose NE over E even when
		// both were equally short, then correct back down later.
		const int actionX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
		const int actionY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
		BWAPI::TilePosition best = BWAPI::TilePositions::Invalid;
		int bestDistance = here;
		int bestDirectness = MAX_DISTANCE;

		for (int a = 0; a < 8; ++a)
		{
			const BWAPI::TilePosition next(x + actionX[a], y + actionY[a]);
			if (!next.isValid())
			{
				continue;
			}

			const int d = distances->at(next);
			if (d < 0 || d >= here)
			{
				continue;
			}

			if (!u->isFlying() && !the.map.isGroundTransitionRuntimeWalkable(
				BWAPI::TilePosition(x, y), next))
			{
				continue;
			}

			const int directness = TileCenter(next).getApproxDistance(targetPosition);
			if (d < bestDistance || (d == bestDistance && directness < bestDirectness))
			{
				best = next;
				bestDistance = d;
				bestDirectness = directness;
			}
		}

		if (!best.isValid())
		{
			if (!u->isFlying())
			{
				const BWAPI::Position detour =
					the.map.getGroundPathWaypoint(u->getPosition(), targetPosition, 6, true);
				if (detour.isValid())
				{
					return detour;
				}
			}
			return targetPosition;
		}

		x = best.x;
		y = best.y;

		if (safe && the.hits(u, BWAPI::TilePosition(x, y)))
		{
			return dodge(u);
		}
		here = distances->at(BWAPI::TilePosition(x, y));        // closer by 1 or 2 tiles
	}

	UAB_ASSERT(BWAPI::TilePosition(x, y).isValid(), "bad tile %d,%d", x, y);
	UAB_ASSERT(distances->at(BWAPI::TilePosition(x, y)) >= 0, "inaccessible tile %d,%d", x, y);

	return TileCenter(BWAPI::TilePosition(x, y));
}

// TODO Try to dodge intelligently.
// If that fails, try to flee crudely.
BWAPI::Position MicroState::dodge(BWAPI::Unit u) const
{
	return flee(u);		// OK, flee crudely
}

// Try to flee the enemy, using a simple but careless method.
BWAPI::Position MicroState::flee(BWAPI::Unit u) const
{
	BWAPI::Unit enemy = the.micro.inWeaponsDanger(u, DangerMargin);
	if (enemy)
	{
		if (!enemy->getType().isBuilding())
		{
			// Look for friendly nearby units that we can flee toward.
			const std::vector<UnitCluster> & defenders = the.ops.getDefenseClusters(enemy->isFlying());
			const UnitCluster * bestCluster = nullptr;
			int lowestScore = 5 * 24;
			for (const UnitCluster & cluster : defenders)
			{
				// Time in frames for each unit to reach the cluster.
				// The times may be negative if the unit is already inside the cluster's radius.
				int friendTime = int((u->getDistance(cluster.center) - cluster.radius) / (the.self()->topSpeed(u->getType())));
				int enemyTime = int((enemy->getDistance(cluster.center) - cluster.radius) / (the.enemy()->topSpeed(enemy->getType())));
				int enemyTimeClipped = std::max(0, enemyTime);  // OK if enemy is deeper inside

				int score = friendTime - enemyTimeClipped;
				if (score < lowestScore)
				{
					bestCluster = &cluster;
					lowestScore = score;
				}
			}
			if (bestCluster)
			{
				//if (u->getType() == BWAPI::UnitTypes::Zerg_Queen) BWAPI::Broodwar->printf("fleeing toward defenders");
				return bestCluster->center;
			}
		}

		// No nearby units to flee toward. Flee away from the enemy instead.
		//if (u->getType() == BWAPI::UnitTypes::Zerg_Queen) BWAPI::Broodwar->printf("fleeing directly away");
		return the.micro.fleeTo(u, enemy->getPosition(), DangerMargin);
	}

	//BWAPI::Broodwar->printf("fleeing home");
	return the.bases.myMain()->getPosition();		// flee home
}

// -- --

// Create a blank MicroState. Values will be filled in later.
MicroState::MicroState()
    : order(BWAPI::Orders::None)
    , targetUnit(nullptr)
    , targetPosition(BWAPI::Positions::None)
    , finalMovePosition(BWAPI::Positions::Invalid)
    , pendingGateRightClick(nullptr)
	, safe(false)
	, targetBase(nullptr)
    , attack(0.5)
    , attackUpdateFrame(-1)
    , orderFrame(-1)
    , executeFrame(-1)
	, needsFollowup(false)
    , needsMonitoring(false)
    , lastCheckFrame(-1)
    , lastActionFrame(-FramesBetweenActions)       // an action at time 0 must execute
    , progressPosition(BWAPI::Positions::Invalid)
    , progressDistance(MAX_DISTANCE)
    , lastProgressFrame(-1)
    , recoveryPosition(BWAPI::Positions::Invalid)
    , recoveryUntilFrame(-1)
    , recoveryAttempts(0)
	, followupTarget(BWAPI::Positions::None)
    , startPosition(BWAPI::Positions::None)
{
}

// No-argument order.
void MicroState::setOrder(BWAPI::Unit u, BWAPI::Order o)
{
    check(u, o);
    finalMovePosition = BWAPI::Positions::Invalid;
    pendingGateRightClick = nullptr;

	targetBase = nullptr;
	safe = false;
	needsFollowup = false;
	
	if (order != o)
    {
        order = o;
        targetPosition = BWAPI::Positions::None;
        targetUnit = nullptr;

        orderFrame = the.now();
        executeFrame = -1;

        startPosition = u->getPosition();
        resetMoveProgress(u);
    }
}

// Order that targets a unit.
void MicroState::setOrder(BWAPI::Unit u, BWAPI::Order o, BWAPI::Unit t)
{
    check(u, o);
    finalMovePosition = BWAPI::Positions::Invalid;
    pendingGateRightClick = nullptr;

	targetBase = nullptr;
	safe = false;
	needsFollowup = false;

	if (order != o || targetUnit != t)
    {
        order = o;
        targetPosition = BWAPI::Positions::None;
        targetUnit = t;

        orderFrame = the.now();
        executeFrame = -1;

        startPosition = u->getPosition();
        resetMoveProgress(u);
    }
}

// Order that targets a position.
void MicroState::setOrder(BWAPI::Unit u, BWAPI::Order o, BWAPI::Position p)
{
    check(u, o);
    finalMovePosition = BWAPI::Positions::Invalid;
    pendingGateRightClick = nullptr;

	targetBase = nullptr;
	safe = false;
	needsFollowup = false;

	// We check whether positions are "nearly equal" rather than equal
    // to reduce unnecessary orders. Don't spam Starcraft!
    if (order != o || !positionsNearlyEqual(u, targetPosition, p))
    {
        order = o;
        targetPosition = p;
        targetUnit = nullptr;

        orderFrame = the.now();
        executeFrame = -1;

        startPosition = u->getPosition();
        resetMoveProgress(u);
    }
}

// Order to move to a position safely, using a base's safe path grid.
// Attack move is also allowed.
void MicroState::setOrder(BWAPI::Unit u, BWAPI::Order o, BWAPI::Position p, Base * b, bool s)
{
    check(u, o);
    finalMovePosition = BWAPI::Positions::Invalid;
    pendingGateRightClick = nullptr;

	if (o != BWAPI::Orders::Move && o != BWAPI::Orders::AttackMove)
	{
		UAB_MESSAGE("rejected, not a move-to-base order");
		return;
	}

	// We check whether positions are "nearly equal" rather than equal
	// to reduce unnecessary orders. Don't spam Starcraft!
	if (order != o || !positionsNearlyEqual(u, targetPosition, p) ||
		b != targetBase || s != safe)
	{
		order = o;
		targetPosition = p;
		targetUnit = nullptr;

		orderFrame = the.now();
		executeFrame = -1;

		startPosition = u->getPosition();
        resetMoveProgress(u);

		if (b)
		{
			targetBase = b;
			safe = s;
			needsFollowup = true;
		}
	}
}

// Notionally, an order is executed once.
// If it is executed piece by piece, then it is followed up on.
// Otherwise, it may be monitored to ensure good execution.
void MicroState::update(BWAPI::Unit u)
{
    if (tankTransitionPending(u))
    {
        return;
    }

    // A one-shot command can still hold an old gate waypoint when the wall is
    // permanently released between updates. Resume its real goal immediately
    // once the Barracks footprint is clear, instead of driving back to it.
    if (order == BWAPI::Orders::Move && finalMovePosition.isValid() &&
        targetPosition != finalMovePosition &&
        the.placer.isTerranMainWallReleased())
    {
        const BWAPI::Position gate =
            the.placer.getTerranMainWallGateCenter();
        if (gate.isValid() &&
            the.map.isRuntimeWalkable(BWAPI::TilePosition(gate)))
        {
            BWAPI::Unit clickTarget = pendingGateRightClick;
            if (clickTarget && clickTarget->exists())
            {
                if (u->getLastCommandFrame() < the.now() &&
                    u->rightClick(clickTarget))
                {
                    BWAPI::Order clickOrder = BWAPI::Orders::AttackUnit;
                    if (clickTarget->getType() ==
                        BWAPI::UnitTypes::Resource_Mineral_Field)
                        clickOrder = BWAPI::Orders::MoveToMinerals;
                    else if (clickTarget->getType().isRefinery())
                        clickOrder = BWAPI::Orders::MoveToGas;
                    setOrder(u, clickOrder, clickTarget);
                    ++TotalCommands;
                }
                return;
            }
            setOrder(u, BWAPI::Orders::Move, finalMovePosition);
        }
    }

    // While the wall is still acting as a gate, advance a one-shot command
    // from the Barracks opening to the far side as each waypoint is reached.
    if (order == BWAPI::Orders::Move && finalMovePosition.isValid() &&
        targetPosition != finalMovePosition &&
        u->getDistance(targetPosition) <= 40)
    {
        const BWAPI::Position nextGate =
            the.placer.terranMainWallTransitWaypoint(
                u->getPosition(), finalMovePosition, u->getID());
        if (nextGate.isValid() && nextGate != targetPosition)
        {
            const BWAPI::Position goal = finalMovePosition;
            BWAPI::Unit clickTarget = pendingGateRightClick;
            setOrder(u, BWAPI::Orders::Move, nextGate);
            finalMovePosition = goal;
            pendingGateRightClick = clickTarget;
        }
    }

    if (order == BWAPI::Orders::Move && pendingGateRightClick)
    {
        BWAPI::Unit clickTarget = pendingGateRightClick;
        if (!clickTarget->exists())
        {
            pendingGateRightClick = nullptr;
        }
        else if (u->getDistance(targetPosition) <= 40 &&
            !the.placer.terranMainWallTransitWaypoint(
                u->getPosition(), clickTarget->getPosition(), u->getID()).isValid() &&
            u->getLastCommandFrame() < the.now() &&
            u->rightClick(clickTarget))
        {
            BWAPI::Order clickOrder = BWAPI::Orders::AttackUnit;
            if (clickTarget->getType() == BWAPI::UnitTypes::Resource_Mineral_Field)
                clickOrder = BWAPI::Orders::MoveToMinerals;
            else if (clickTarget->getType().isRefinery())
                clickOrder = BWAPI::Orders::MoveToGas;
            setOrder(u, clickOrder, clickTarget);
            ++TotalCommands;
            return;
        }
    }

    // Complete a one-shot Move after the Barracks opening, even if its caller
    // does not refresh the order on later frames.
    if (order == BWAPI::Orders::Move && !pendingGateRightClick &&
        finalMovePosition.isValid() &&
        targetPosition != finalMovePosition &&
        u->getDistance(targetPosition) <= 40 &&
        !the.placer.terranMainWallTransitWaypoint(
            u->getPosition(), finalMovePosition, u->getID()).isValid())
    {
        const BWAPI::Position goal = finalMovePosition;
        setOrder(u, BWAPI::Orders::Move, goal);
    }

    if (executeFrame < 0)
    {
        // Not executed yet. Do that.
        execute(u);
    }
	else if (needsFollowup)
	{
		followup(u);
	}
	else if (needsMonitoring && the.now() > lastCheckFrame + 2 * BWAPI::Broodwar->getLatencyFrames())
	{
		// Executed and should be working. Make sure it is.
		monitor(u);
	}
	else if (u->getType().isBuilding() &&
			 (order == BWAPI::Orders::ConstructingBuilding || order == BWAPI::Orders::ZergBuildingMorph) &&
			 u->isCompleted())
	{
		// Leaving an old morph order around can prevent future morphs from happening.
		// The only case is morphing a lair into a hive (but it's a critical one).
		order = BWAPI::Orders::None;
		targetPosition = BWAPI::Positions::None;
		targetUnit = nullptr;

		orderFrame = the.now();
		executeFrame = 0;
	}
}

// `a` is true if the combat sim tells the unit to participate in an attack this frame, false if to regroup.
// Maintain floating point `attack` which smooths the attack/retreat values over time using an
// exponential moving average.
void MicroState::setAttack(bool a)
{
    // Don't try to update twice per frame.
	// NOTE This happens on its own when the game is interrupted through the UI,
	//      so don't notify about the issue unless we're debugging.
    // UAB_ASSERT(attackUpdateFrame < the.now(), "double attack update");
	if (attackUpdateFrame >= the.now())
	{
		return;
	}

    // Decay the previous attack value depending on how much time has passed.
    if (attackUpdateFrame != -1)
    {
        attack *= std::pow(1.0 - attackDecay, the.now() - attackUpdateFrame);
    }
    attackUpdateFrame = the.now();

    // And add in the new value.
    attack += a ? attackDecay : 0.0;
}

// Return the smoothed attack/retreat value.
// It will be combined with values from other units to make decisions.
double MicroState::getSmoothedAttack() const
{
    return attack;
}

void MicroState::draw(BWAPI::Unit u) const
{
    int x = u->getRight() + 2;
    int y = u->getTop();

    if (order == BWAPI::Orders::Move)
    {
        BWAPI::Broodwar->drawTextMap(x, y, "move %d,%d", targetPosition.x, targetPosition.y);
    }
    else
    {
        BWAPI::Broodwar->drawTextMap(x, y, "%s", order.getName().c_str());
    }
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

// For drawing debug info on the screen.
const int dotRadius = 2;

// Execute any micro commands stored up this frame, and generally try to make sure
// that units do what they have been ordered to.
void Micro::update()
{
    const int now = the.now();
    for (auto it = trafficYields.begin(); it != trafficYields.end(); )
    {
        BWAPI::Unit unit = it->first;
        if (!unit || !unit->exists() || now >= it->second.untilFrame ||
            unit->isUnderAttack() || the.hits(unit) > 0 ||
            unit->isAttackFrame() || unit->isSieged())
        {
            it = trafficYields.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = orders.begin(); it != orders.end(); )
    {
        BWAPI::Unit u = (*it).first;
        MicroState & o = (*it).second;

        if (u->exists())
        {
            auto yielding = trafficYields.find(u);
            if (yielding != trafficYields.end() &&
                o.getOrder() == BWAPI::Orders::AttackUnit &&
                o.getTargetUnit() && o.getTargetUnit()->exists() &&
                u->getDistance(o.getTargetUnit()) <= 6 * 32)
            {
                trafficYields.erase(yielding);
                yielding = trafficYields.end();
            }
            if (yielding != trafficYields.end())
            {
                // Combat and formation managers may refresh their old hold or
                // rally target each frame. Keep this short side-step leased so
                // the blocker actually clears the passage before returning.
                if (u->getDistance(yielding->second.position) > 20)
                    o.setOrder(u, BWAPI::Orders::Move, yielding->second.position);
                else
                {
                    if (u->getLastCommandFrame() < now &&
                        u->getLastCommand().getType() !=
                            BWAPI::UnitCommandTypes::Hold_Position)
                    {
                        u->holdPosition();
                        ++TotalCommands;
                    }
                    o.setOrder(u, BWAPI::Orders::HoldPosition);
                }
            }
            o.update(u);

            // NOTE Execute orders before drawing the state.
            if (Config::Debug::DrawMicroState)
            {
                o.draw(u);
            }

            ++it;
        }
        else
        {
            // Delete records for units which are gone.
            it = orders.erase(it);
        }
    }
    drawLastComsatScan();
}

bool Micro::alwaysKite(BWAPI::UnitType type) const
{
    return type == BWAPI::UnitTypes::Terran_Vulture ||
        type == BWAPI::UnitTypes::Terran_Wraith;
}

// If the distances are given and we can get there from here, return a nearby position on a good path.
// In any other case, return the ultimate destination.
BWAPI::Position Micro::nextGroundDestination(BWAPI::Unit unit, const BWAPI::Position & destination, const GridDistances * distances) const
{
    if (!distances || !unit->getTilePosition().isValid())
    {
        return destination;
    }

    const int StepSize = 8;
    int here = distances->at(unit->getTilePosition());
    if (here < StepSize)
    {
        // Either we're already very near, or we can't get there. Same answer for both.
        // NOTE This lets distances be slightluy offset from the true distances without error.
        return destination;
    }

    // Use the distances map to find our next waypoint.
    // One tile of every StepSize tiles along the path is a waypoint.
    // When we are halfway between waypoints, we switch to the following one.
    // In other words, if we're going to X and we're halfway between B and C, we switch from waypoint C to D.
    const int phase = here % StepSize;
    const int target = std::max(0, here - phase - (phase > StepSize / 2 ? 0 : StepSize));

    int x = unit->getTilePosition().x;
    int y = unit->getTilePosition().y;
    while (here > target)
    {
        UAB_ASSERT(BWAPI::TilePosition(x, y).isValid(), "bad tile %d,%d", x, y);
        UAB_ASSERT(distances->at(BWAPI::TilePosition(x, y)) >= 0, "inaccessible tile %d,%d", x, y);

        const int actionX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
        const int actionY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
        BWAPI::TilePosition best = BWAPI::TilePositions::Invalid;
        int bestDistance = here;
        int bestDirectness = MAX_DISTANCE;

        for (int a = 0; a < 8; ++a)
        {
            const BWAPI::TilePosition next(x + actionX[a], y + actionY[a]);
            if (!next.isValid())
            {
                continue;
            }

            const int d = distances->at(next);
            if (d < 0 || d >= here)
            {
                continue;
            }

            if (!unit->isFlying() && !the.map.isGroundTransitionRuntimeWalkable(
                BWAPI::TilePosition(x, y), next))
            {
                continue;
            }

            const int directness = TileCenter(next).getApproxDistance(destination);
            if (d < bestDistance || (d == bestDistance && directness < bestDirectness))
            {
                best = next;
                bestDistance = d;
                bestDirectness = directness;
            }
        }

        if (!best.isValid())
        {
            if (!unit->isFlying())
            {
                const BWAPI::Position detour =
                    the.map.getGroundPathWaypoint(unit->getPosition(), destination, StepSize, true);
                if (detour.isValid())
                {
                    return detour;
                }
            }
            return destination;
        }

        x = best.x;
        y = best.y;

        here = distances->at(BWAPI::TilePosition(x, y));        // closer by 1 or 2 tiles
    }

    UAB_ASSERT(BWAPI::TilePosition(x, y).isValid(), "bad tile %d,%d", x, y);
    UAB_ASSERT(distances->at(BWAPI::TilePosition(x, y)) >= 0, "inaccessible tile %d,%d", x, y);

    return TileCenter(BWAPI::TilePosition(x, y));
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

Micro::Micro()
{
}

void Micro::resetForNewGame()
{
    orders.clear();
    trafficYields.clear();
    lastTrafficYieldFrame = -100000;

    g_lastComsatScanPosition = BWAPI::Positions::Invalid;
    g_lastComsatScanFrame = -100000;
    g_lastComsatScanComsatID = -1;
    g_lastComsatScanEnergy = 0;

    g_lastTankSiegeCommandFrame.clear();
    g_lastTankUnsiegeCommandFrame.clear();
    g_tankSiegeHoldUntil.clear();
    g_lastTankTransitionStateFrame = -100000;
}

bool Micro::trafficYieldActive(BWAPI::Unit unit) const
{
    const auto it = trafficYields.find(unit);
    return it != trafficYields.end() && unit && unit->exists() &&
        the.now() < it->second.untilFrame &&
        !unit->isUnderAttack() && !unit->isAttackFrame() &&
        the.hits(unit) <= 0;
}

bool Micro::yieldBlockingUnit(
    BWAPI::Unit mover, const BWAPI::Position& destination)
{
    const int now = the.now();
    if (!mover || !mover->exists() || mover->isFlying() ||
        !mover->getPosition().isValid() || !destination.isValid() ||
        now - lastTrafficYieldFrame < 8 || trafficYields.size() >= 3 ||
        mover->isUnderAttack() || the.hits(mover) > 0)
    {
        return false;
    }
    lastTrafficYieldFrame = now;

    const BWAPI::Position start = mover->getPosition();
    const BWAPI::TilePosition startTile(start);
    if (!startTile.isValid()) return false;

    const ChokePoint* choke = nullptr;
    int chokeDistance = 6 * 32;
    for (const ChokePoint& candidate : the.zone.chokes())
    {
        if (!candidate.center().isValid()) continue;
        const int distance = start.getApproxDistance(candidate.center());
        if (distance < chokeDistance)
        {
            choke = &candidate;
            chokeDistance = distance;
        }
    }
    const bool movingTank =
        mover->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode;
    const BWAPI::UnitType moverType = mover->getType();
    const bool movingInfantry =
        moverType == BWAPI::UnitTypes::Terran_Marine ||
        moverType == BWAPI::UnitTypes::Terran_Medic ||
        moverType == BWAPI::UnitTypes::Terran_Firebat;
    bool tankBlockingInfantry = false;
    if (movingInfantry)
    {
        for (BWAPI::Unit nearby : mover->getUnitsInRadius(3 * 32))
        {
            if (nearby && nearby->exists() &&
                nearby->getPlayer() == the.self() &&
                nearby->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode &&
                nearby->getPosition().isValid())
            {
                tankBlockingInfantry = true;
                break;
            }
        }
    }
    if (the.tileRoom.at(startTile) > 16 && !choke &&
        !movingTank && !tankBlockingInfantry)
        return false;

    double dx = double(destination.x - start.x);
    double dy = double(destination.y - start.y);
    const double length = std::sqrt(dx * dx + dy * dy);
    if (length < 2 * 32) return false;
    dx /= length;
    dy /= length;

    // Restrict the search to stopped friendly bodies directly in front of the
    // waiting unit. A defending Siege Tank or a unit fighting an enemy stays.
    for (BWAPI::Unit blocker : mover->getUnitsInRadius(4 * 32))
    {
        if (!blocker || !blocker->exists() || blocker == mover ||
            blocker->getPlayer() != the.self() || blocker->isFlying() ||
            blocker->getType().isBuilding() || !blocker->canMove() ||
            blocker->isSieged() || blocker->isLoaded() ||
            blocker->isUnderAttack() || blocker->isAttacking() ||
            blocker->isAttackFrame() || the.hits(blocker) > 0 ||
            blocker->getOrder() == BWAPI::Orders::PlaceMine ||
            trafficYields.find(blocker) != trafficYields.end() ||
            std::abs(blocker->getVelocityX()) +
                std::abs(blocker->getVelocityY()) > 0.5 ||
            !blocker->getPosition().isValid())
        {
            continue;
        }

        if (blocker->getType().isWorker() &&
            (!blocker->isIdle() || blocker->isCarryingMinerals() ||
             blocker->isCarryingGas()))
        {
            continue;
        }

        const BWAPI::Position here = blocker->getPosition();
        const BWAPI::TilePosition hereTile(here);
        if (!hereTile.isValid()) continue;
        const int room = the.tileRoom.at(hereTile);
        const bool nearChoke = choke &&
            here.getApproxDistance(choke->center()) <= 4 * 32;
        // A parked Vulture can pin a moving Tank even on open base terrain.
        // Let the normal short traffic lease clear that lane as well as chokes.
        const bool tankBehindVulture =
            movingTank &&
            blocker->getType() == BWAPI::UnitTypes::Terran_Vulture;
        const bool tankInInfantryLane =
            tankBlockingInfantry &&
            blocker->getType() == BWAPI::UnitTypes::Terran_Siege_Tank_Tank_Mode;
        if ((room <= 0 || room > 14) && !nearChoke &&
            !tankBehindVulture && !tankInInfantryLane) continue;

        const double forward =
            (here.x - start.x) * dx + (here.y - start.y) * dy;
        const double lateral = std::abs(
            -(here.x - start.x) * dy + (here.y - start.y) * dx);
        if (forward < -16 || forward > 4 * 32 || lateral > 76)
            continue;

        struct YieldCandidate
        {
            BWAPI::Position position;
            int score;
        };
        std::vector<YieldCandidate> candidates;
        const int offsets[8][2] = {
            { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
            { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }
        };
        for (int radius : { 2, 3, 4 })
        {
            for (const auto& offset : offsets)
            {
                const BWAPI::Position point(
                    here.x + offset[0] * radius * 32,
                    here.y + offset[1] * radius * 32);
                const BWAPI::TilePosition tile(point);
                if (!point.isValid() || !tile.isValid() ||
                    !the.map.isRuntimeWalkable(tile) ||
                    !the.partitions.connected(here, point) ||
                    the.groundHits(tile) > 0 ||
                    point.getApproxDistance(start) < 2 * 32)
                {
                    continue;
                }

                const int pointRoom = the.tileRoom.at(tile);
                const double pointLateral = std::abs(
                    -(point.x - start.x) * dy +
                    (point.y - start.y) * dx);
                const int chokeGain = nearChoke
                    ? point.getApproxDistance(choke->center()) -
                      here.getApproxDistance(choke->center())
                    : 0;
                if (pointRoom < room + 2 &&
                    (pointLateral < 2 * 32 || chokeGain < 32))
                {
                    continue;
                }

                bool occupied = false;
                for (BWAPI::Unit other :
                     BWAPI::Broodwar->getUnitsInRadius(point, 40))
                {
                    if (other && other->exists() && other != blocker &&
                        !other->isFlying() && !other->getType().isBuilding())
                    {
                        occupied = true;
                        break;
                    }
                }
                if (occupied) continue;

                const int score =
                    pointRoom * 32 + int(pointLateral) +
                    chokeGain - here.getApproxDistance(point);
                candidates.push_back({ point, score });
            }
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const YieldCandidate& a, const YieldCandidate& b)
            {
                return a.score > b.score;
            });
        for (size_t i = 0; i < std::min<size_t>(4, candidates.size()); ++i)
        {
            const BWAPI::Position point = candidates[i].position;
            if (!shortTrafficYieldPath(hereTile, BWAPI::TilePosition(point)))
            {
                continue;
            }

            Move(blocker, point);
            trafficYields[blocker] = { point, now + 48 };
            if (Config::Debug::DrawUnitTargets)
            {
                BWAPI::Broodwar->drawLineMap(
                    here, point, BWAPI::Colors::Yellow);
                BWAPI::Broodwar->drawTextMap(here, "yield choke");
            }
            return true;
        }
    }

    return false;
}

const MicroState & Micro::getMicroState(BWAPI::Unit unit) const
{
    auto it = orders.find(unit);

    if (it == orders.end())
    {
        return noOrder;
    }

    return (*it).second;
}

// Was the unit already given a command this frame?
bool Micro::alreadyCommanded(BWAPI::Unit unit) const
{
    auto it = orders.find(unit);

    if (it == orders.end())
    {
        return false;
    }

    return (*it).second.getOrderFrame() >= the.now();
}

// Is the given unit in range of enemy weapons fire, with the given safety margin (in pixels)?
// If so, return the nearest enemy that puts the unit in danger.
// NOTE The check is quick and dirty, not exact. It will make mistakes.
BWAPI::Unit Micro::inWeaponsDanger(BWAPI::Unit unit, int margin) const
{
    BWAPI::Unit enemy;
    if (unit->isFlying())
    {
        // Flying unit.
        enemy = BWAPI::Broodwar->getClosestUnit(
            unit->getPosition(),
            BWAPI::Filter::IsEnemy &&
            (BWAPI::Filter::AirWeapon != BWAPI::WeaponTypes::None || BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Bunker || BWAPI::Filter::GetType == BWAPI::UnitTypes::Protoss_Carrier) &&
            BWAPI::Filter::GetType != BWAPI::UnitTypes::Protoss_Interceptor &&
            BWAPI::Filter::IsCompleted,
            margin + 8 * 32
        );
    }
    else
    {
        // Ground unit.
        enemy = BWAPI::Broodwar->getClosestUnit(
            unit->getPosition(),
            BWAPI::Filter::IsEnemy &&
            !BWAPI::Filter::IsWorker &&
            (BWAPI::Filter::GroundWeapon != BWAPI::WeaponTypes::None || BWAPI::Filter::GetType == BWAPI::UnitTypes::Terran_Bunker || BWAPI::Filter::GetType == BWAPI::UnitTypes::Protoss_Reaver || BWAPI::Filter::GetType == BWAPI::UnitTypes::Protoss_Carrier) &&
            BWAPI::Filter::GetType != BWAPI::UnitTypes::Protoss_Interceptor &&
            BWAPI::Filter::IsCompleted,
            margin + (the.info.enemyHasSiegeMode() ? 12 * 32 : 8 * 32)
        );
    }

    if (enemy && unit->getDistance(enemy) < margin + UnitUtil::GetAttackRangeAssumingUpgrades(enemy->getType(), unit->getType()))
    {
        return enemy;
    }
    return nullptr;
}

// Is this a safe spot for the given unit to flee to? Useful for nearby spots only.
// "Safe" mostly means that we should not get stuck on terrain, etc.
bool Micro::canFleeTo(BWAPI::Unit unit, const BWAPI::Position & destination) const
{
    // Don't flee off the map.
    if (!destination.isValid())
    {
        return false;
    }

    // Don't flee into enemy static defense, unless we're already in its range.
    BWAPI::TilePosition tile(destination);
    if (the.staticHits(unit, tile) > the.staticHits(unit))
    {
        return false;
    }

    // For a flying unit, that's all we want to check.
    if (unit->isFlying())
    {
        return true;
    }

    // Don't flee somewhere we can't walk on the terrain.
    if (!the.map.isWalkable(tile))
    {
        return false;
    }

    if (unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
    {
        const BWAPI::UnitType body = unit->getType();
        const int left = destination.x - body.dimensionLeft(), right = destination.x + body.dimensionRight();
        const int top = destination.y - body.dimensionUp(), bottom = destination.y + body.dimensionDown();
        if (left < 0 || top < 0 || right >= BWAPI::Broodwar->mapWidth() * 32 ||
            bottom >= BWAPI::Broodwar->mapHeight() * 32 ||
            !the.partitions.connected(unit->getPosition(), destination)) return false;
        for (int x = left / 8; x <= right / 8; ++x)
            for (int y = top / 8; y <= bottom / 8; ++y)
                if (!BWAPI::Broodwar->isWalkable(x, y)) return false;
        // These are short kiting steps, not routed travel orders. A clear
        // endpoint across a thin cliff still cannot be reached directly.
        const BWAPI::Position start = unit->getPosition();
        const int steps = std::max(1, (std::max(std::abs(destination.x - start.x),
            std::abs(destination.y - start.y)) + 7) / 8);
        for (int step = 1; step < steps; ++step)
        {
            const BWAPI::Position p(start.x + (destination.x - start.x) * step / steps,
                start.y + (destination.y - start.y) * step / steps);
            for (int x = (p.x - body.dimensionLeft()) / 8; x <= (p.x + body.dimensionRight()) / 8; ++x)
                for (int y = (p.y - body.dimensionUp()) / 8; y <= (p.y + body.dimensionDown()) / 8; ++y)
                    if (!BWAPI::Broodwar->isWalkable(x, y)) return false;
        }
        for (BWAPI::Unit obstacle : BWAPI::Broodwar->getUnitsInRadius(destination, 96))
        {
            if (!obstacle || !obstacle->exists() || obstacle == unit || obstacle->isFlying() || obstacle->isBurrowed()) continue;
            const BWAPI::UnitType type = obstacle->getType();
            const BWAPI::Position p = obstacle->getPosition();
            if (right + 4 >= p.x - type.dimensionLeft() && left - 4 <= p.x + type.dimensionRight() &&
                bottom + 4 >= p.y - type.dimensionUp() && top - 4 <= p.y + type.dimensionDown()) return false;
        }
    }

    // Don't flee to a tile blocked by a building or unit.
    // The check conservatively assumes that any other ground unit may block us.
    BWAPI::Unitset blocks = BWAPI::Broodwar->getUnitsOnTile(tile, !BWAPI::Filter::IsFlying && !BWAPI::Filter::IsBurrowed);
    for (BWAPI::Unit u : blocks)
    {
        if (u != unit)      // no way to check this with a filter, apparently
        {
            return false;
        }
    }

    return true;
}

// Kite backward a short distance, in a direction away from the danger point.
// If that's impossible (something is in the way), return false instead.
bool Micro::kiteBack(BWAPI::Unit unit, BWAPI::Unit enemy)
{
    BWAPI::Position destination = RawDistanceAndDirection(unit->getPosition(), enemy->getPosition(), -48);

    //BWAPI::Broodwar->drawLineMap(unit->getPosition(), destination, BWAPI::Colors::Grey);

    if (canFleeTo(unit, destination))
    {
        Move(unit, destination);
        //BWAPI::Broodwar->drawLineMap(unit->getPosition(), destination, BWAPI::Colors::Green);
        return true;
    }
    else
    {
        // The direct retreat does not work. Let's see if we can sidestep.
        if (!unit->isFlying() && !enemy->isFlying())
        {
            int range = UnitUtil::GetAttackRange(enemy, unit);
            if ((range > 0 && range <= 32) || unit->getType() == BWAPI::UnitTypes::Terran_Vulture)
            {
                int enemyDist = unit->getDistance(enemy);
                for (int dx = -32; dx <= 32; dx += 32)
                {
                    for (int dy = -32; dy <= 32; dy += 32)
                    {
                        if (dx != 0 || dy != 0)
                        {
                            destination = unit->getPosition() + BWAPI::Position(dx, dy);
                            if (canFleeTo(unit, destination) && enemy->getDistance(destination) > enemyDist)
                            {
                                Move(unit, destination);
                                //BWAPI::Broodwar->drawLineMap(unit->getPosition(), destination, BWAPI::Colors::Blue);
                                return true;
                            }
                            else
                            {
                                //BWAPI::Broodwar->drawLineMap(unit->getPosition(), destination, BWAPI::Colors::Purple);
                            }
                        }
                    }
                }
            }
        }
        //BWAPI::Broodwar->drawCircleMap(unit->getPosition(), 3, BWAPI::Colors::Green);
    }

    return false;
}

BWAPI::Position Micro::fleeTo(BWAPI::Unit unit, const BWAPI::Position & danger, int distance) const
{
    return DistanceAndDirection(unit->getPosition(), danger, -distance);
}

void Micro::fleePosition(BWAPI::Unit unit, const BWAPI::Position & danger, int distance)
{
    BWAPI::Position destination = fleeTo(unit, danger, distance);
    the.micro.MoveNear(unit, destination);
    if (Config::Debug::DrawUnitTargets)
    {
        BWAPI::Broodwar->drawLineMap(unit->getPosition(), destination, BWAPI::Colors::Green);
    }
}

void Micro::fleeEnemy(BWAPI::Unit unit, BWAPI::Unit enemy, int distance)
{
    fleePosition(unit, enemy->getPosition(), distance);
}

// If our ground unit is next to an undetected dark templar, run it away and return true.
// Otherwise return false.
bool Micro::fleeDT(BWAPI::Unit unit)
{
    if (!unit || !unit->exists() || unit->getPlayer() != the.self() || !unit->getPosition().isValid())
    {
        UAB_ASSERT(false, "bad arg");
        return false;
    }

    if (!unit->isFlying() && unit->canMove())
    {
        BWAPI::Unit dt = BWAPI::Broodwar->getClosestUnit(
            unit->getPosition(),
            BWAPI::Filter::GetType == BWAPI::UnitTypes::Protoss_Dark_Templar &&
            BWAPI::Filter::IsEnemy &&
            !BWAPI::Filter::IsDetected,
            64);

        if (dt)
        {
            fleeEnemy(unit, dt);
            return true;
        }
    }

    return false;
}

void Micro::Stop(BWAPI::Unit unit)
{
    if (tankTransitionPending(unit))
    {
        return;
    }

    if (!unit || !unit->exists() || unit->getPlayer() != the.self())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }

    // if we have issued a command to this unit already this frame, ignore this one
    if (unit->getLastCommandFrame() >= the.now() || unit->isAttackFrame())
    {
        return;
    }

    // If we already gave this command, don't repeat it.
    BWAPI::UnitCommand currentCommand(unit->getLastCommand());
    if (currentCommand.getType() == BWAPI::UnitCommandTypes::Stop)
    {
        return;
    }

    orders[unit].setOrder(unit, BWAPI::Orders::Stop);

    unit->stop();
    TotalCommands++;
}

void Micro::HoldPosition(BWAPI::Unit unit)
{
    if (tankTransitionPending(unit) || trafficYieldActive(unit))
    {
        return;
    }

    if (!unit || !unit->exists() || unit->getPlayer() != the.self())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }

    // if we have issued a command to this unit already this frame, ignore this one
    if (unit->getLastCommandFrame() >= the.now() || unit->isAttackFrame())
    {
        return;
    }

    // If we already gave this command, don't repeat it.
    BWAPI::UnitCommand currentCommand(unit->getLastCommand());
    if (currentCommand.getType() == BWAPI::UnitCommandTypes::Hold_Position)
    {
        return;
    }

    orders[unit].setOrder(unit, BWAPI::Orders::HoldPosition);

    unit->holdPosition();
    TotalCommands++;
}

// If the target is moving, chase it.
// If it's not moving or we're in range and ready, attack it.
void Micro::CatchAndAttackUnit(BWAPI::Unit attacker, BWAPI::Unit target)
{
    if (!attacker || !attacker->exists() || attacker->getPlayer() != the.self() ||
        !target || !target->exists())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }

    if (!target->isMoving() || !attacker->canMove() || attacker->isInWeaponRange(target))
    {
        //BWAPI::Broodwar->drawLineMap(attacker->getPosition(), target->getPosition(), BWAPI::Colors::Orange);
        AttackUnit(attacker, target);
    }
    else
    {
        if (attacker->getDistance(target) > 8 * 32)
        {
            // The target is far away, so be lazy.
            BWAPI::Position destination = PredictMovement(target, 12);
            //BWAPI::Broodwar->drawLineMap(attacker->getPosition(), destination, BWAPI::Colors::Red);
            MoveNear(attacker, destination);
        }
        else if (attacker->getDistance(target) > 4 * 32)
        {
            BWAPI::Position destination = PredictMovement(target, 8);
            //BWAPI::Broodwar->drawLineMap(attacker->getPosition(), destination, BWAPI::Colors::Orange);
            Move(attacker, destination);
        }
        else
        {
            // The target is near. Aim for its predicted position.
            // NOTE The caller should have already decided that we can catch the target.
            int frames = UnitUtil::FramesToReachAttackRange(attacker, target);
            BWAPI::Position destination = PredictMovement(target, std::min(frames, 8));
            //BWAPI::Broodwar->drawLineMap(target->getPosition(), destination, BWAPI::Colors::Blue);
            //BWAPI::Broodwar->drawLineMap(attacker->getPosition(), destination, BWAPI::Colors::Yellow);
            Move(attacker, destination);
        }
    }
}

void Micro::AttackUnit(BWAPI::Unit attacker, BWAPI::Unit target)
{
    if (tankTransitionPending(attacker))
    {
        return;
    }

    if (!attacker || !attacker->exists() || attacker->getPlayer() != the.self() ||
        !target || !target->exists())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }
    if (trafficYieldActive(attacker) &&
        attacker->getDistance(target) > 6 * 32)
    {
        return;
    }

    // Do nothing if we've already issued a command this frame, or the unit is busy attacking.
    // NOTE A lurker attacking a fixed target is ALWAYS on an attack frame.
    //      According to Arrak, sunken colonies behave the same.
    if (attacker->getLastCommandFrame() >= the.now() ||
        attacker->isAttackFrame())
    {
        return;
    }

    // A ground attacker ordered after a unit beyond the wall must approach the
    // Barracks opening, not repeatedly path into the Supply Depot side.
    if (!attacker->isFlying() && !attacker->isSieged() &&
        !target->isFlying() &&
        !attacker->isInWeaponRange(target) &&
        the.placer.terranMainWallTransitWaypoint(
            attacker->getPosition(), target->getPosition(), attacker->getID()).isValid())
    {
        MoveNear(attacker, target->getPosition());
        return;
    }
    
    // get the unit's current command
    BWAPI::UnitCommand currentCommand(attacker->getLastCommand());

    // if we've already told this unit to attack this target, ignore this command
    if (currentCommand.getType() == BWAPI::UnitCommandTypes::Attack_Unit &&
        currentCommand.getTarget() == target &&
        (attacker->getOrderTarget() == target ||
         the.now() - attacker->getLastCommandFrame() < 12))
    {
        return;
    }
    
    orders[attacker].setOrder(attacker, BWAPI::Orders::AttackUnit, target);

    // if nothing prevents it, attack the target
    attacker->attack(target);
    TotalCommands++;

    if (Config::Debug::DrawUnitTargets) 
    {
        BWAPI::Broodwar->drawCircleMap(attacker->getPosition(), dotRadius, BWAPI::Colors::Red, true);
        BWAPI::Broodwar->drawCircleMap(target->getPosition(), dotRadius, BWAPI::Colors::Red, true);
        BWAPI::Broodwar->drawLineMap(attacker->getPosition(), target->getPosition(), BWAPI::Colors::Red);
    }
}

void Micro::AttackMove(BWAPI::Unit attacker, const BWAPI::Position & targetPosition)
{
    if (tankTransitionPending(attacker) || trafficYieldActive(attacker))
    {
        return;
    }

    if (!attacker || !attacker->exists() || attacker->getPlayer() != the.self() || !targetPosition.isValid())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }

    // if we have issued a command to this unit already this frame, ignore this one
    if (attacker->getLastCommandFrame() >= the.now() || attacker->isAttackFrame())
    {
        return;
    }

    const BWAPI::Position gate = !attacker->isFlying()
        ? the.placer.terranMainWallTransitWaypoint(
            attacker->getPosition(), targetPosition, attacker->getID())
        : BWAPI::Positions::Invalid;
    const BWAPI::Position destination = gate.isValid() ? gate : targetPosition;
    BWAPI::UnitCommand currentCommand(attacker->getLastCommand());
    if (currentCommand.getType() == BWAPI::UnitCommandTypes::Attack_Move &&
        currentCommand.getTargetPosition() == destination)
    {
        return;
    }
    orders[attacker].setOrder(attacker, BWAPI::Orders::AttackMove, destination);

    // Nothing prevents it. Attack toward the target position.
    attacker->attack(destination);
    TotalCommands++;

    if (Config::Debug::DrawUnitTargets) 
    {
        BWAPI::Broodwar->drawCircleMap(attacker->getPosition(), dotRadius, BWAPI::Colors::Orange, true);
        BWAPI::Broodwar->drawCircleMap(targetPosition, dotRadius, BWAPI::Colors::Orange, true);
        BWAPI::Broodwar->drawLineMap(attacker->getPosition(), targetPosition, BWAPI::Colors::Orange);
    }
}

void Micro::Move(BWAPI::Unit unit, const BWAPI::Position & targetPosition, const GridDistances * distances)
{
    if (tankTransitionPending(unit) || (unit && unit->exists() && unit->isSieged()))
    {
        return;
    }

    if (!unit || !unit->exists() || unit->getPlayer() != the.self() || !targetPosition.isValid())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }
    const auto yielding = trafficYields.find(unit);
    if (yielding != trafficYields.end() &&
        trafficYieldActive(unit) &&
        targetPosition.getApproxDistance(yielding->second.position) > 32)
    {
        return;
    }
    
    const BWAPI::Position gate = !unit->isFlying()
        ? the.placer.terranMainWallTransitWaypoint(
            unit->getPosition(), targetPosition, unit->getID())
        : BWAPI::Positions::Invalid;
    BWAPI::Position destination = gate.isValid()
        ? gate
        : nextGroundDestination(unit, targetPosition, distances);

    orders[unit].setOrder(unit, BWAPI::Orders::Move, destination);
    if (gate.isValid() && gate != targetPosition)
    {
        orders[unit].setFinalMovePosition(targetPosition);
    }

    TotalCommands++;

    if (Config::Debug::DrawUnitTargets)
    {
        BWAPI::Broodwar->drawCircleMap(unit->getPosition(), dotRadius, BWAPI::Colors::White, true);
        BWAPI::Broodwar->drawCircleMap(destination, dotRadius, BWAPI::Colors::White, true);
        BWAPI::Broodwar->drawLineMap(unit->getPosition(), destination, BWAPI::Colors::White);
        BWAPI::Broodwar->drawLineMap(destination, targetPosition, BWAPI::Colors::Orange);
   }
}

// Like Move, but lazier. If the new target position is close to the previous one,
// then don't be in a hurry to update the order.
// Suitable for approaching a moving target that is distant, or slow, or needs little accuracy.
// This reduces unnecessary orders. It's up to the caller to decide that they may be unnecessary.
// With fewer orders, units are less likely to get stuck.
void Micro::MoveNear(BWAPI::Unit unit, const BWAPI::Position & targetPosition, const GridDistances * distances)
{
    auto it = orders.find(unit);
    if (it == orders.end())
    {
        // The unit doesn't have an existing order.
        Move(unit, targetPosition, distances);
    }
    else
    {
        // The unit has an existing order. Check it.
        MicroState & state = it->second;

        if (state.getOrder() != BWAPI::Orders::Move ||
            the.now() - state.getOrderFrame() >= 18 ||
            state.getTargetPosition().getApproxDistance(targetPosition) > 2 * 32)
        {
            Move(unit, targetPosition, distances);
        }
        // Otherwise do nothing. It's close enough.
    }
}

// Move toward the given destination, unless danger is visible, in which case move
// away from the danger. Call this every frame (or every n frames) to attempt to
// follow a safe path toward the destination.
// Suitable for units which never want to come under fire, such as scouting units.
// Not too smart; units in danger often oscillate toward and away from it and get nowhere.
// Return true if we fled danger, otherwise false.
bool Micro::MoveSafely(BWAPI::Unit unit, const BWAPI::Position & targetPosition, const GridDistances * distances)
{
    if (!unit || !unit->exists() || unit->getPlayer() != the.self() || !unit->getPosition().isValid() ||
        !targetPosition.isValid())
    {
        UAB_ASSERT(false, "bad arg");
        return false;
    }

    int margin = 32;
    if (unit->getType().isWorker() || !UnitUtil::TypeCanAttack(unit->getType()))
    {
        margin = 2 * 32;
    }

    BWAPI::Unit enemy = inWeaponsDanger(unit, margin);
    if (enemy)
    {
        if (!enemy->getType().isBuilding())
        {
            // Look for friendly nearby units that we can flee toward.
            const std::vector<UnitCluster> & defenders =
                enemy->isFlying() ? the.ops.getAirDefenseClusters() : the.ops.getGroundDefenseClusters();
            const UnitCluster * bestCluster = nullptr;
            int lowestScore = 5 * 24;
            for (const UnitCluster & cluster : defenders)
            {
                // Time in frames for each unit to reach the cluster.
                // The times may be negative if the unit is already inside the cluster's radius.
                int friendTime = int((unit->getDistance(cluster.center) - cluster.radius) / (the.self()->topSpeed(unit->getType())));
                int enemyTime = int((enemy->getDistance(cluster.center) - cluster.radius) / (the.enemy()->topSpeed(enemy->getType())));
                int enemyTimeClipped = std::max(0, enemyTime);  // OK if enemy is deeper inside

                int score = friendTime - enemyTimeClipped;
                if (score < lowestScore)
                {
                    bestCluster = &cluster;
                    lowestScore = score;
                }
            }
            if (bestCluster)
            {
                MoveNear(unit, bestCluster->center);
                return true;
            }
        }

        // No nearby units to flee toward. Flee away from the enemy instead.
        fleeEnemy(unit, enemy);
        return true;
    }

    MoveNear(unit, targetPosition, distances);
    return false;
}

void Micro::RightClick(BWAPI::Unit unit, BWAPI::Unit target)
{
    if (!unit || !unit->exists() || unit->getPlayer() != the.self() ||
        !target || !target->exists())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }

    // if we have issued a command to this unit already this frame, ignore this one
    if (unit->getLastCommandFrame() >= the.now() || unit->isAttackFrame())
    {
        return;
    }

    if (!unit->isFlying() &&
        the.placer.terranMainWallTransitWaypoint(
            unit->getPosition(), target->getPosition(), unit->getID()).isValid())
    {
        Move(unit, target->getPosition());
        orders[unit].setPendingGateRightClick(target);
        return;
    }

    // get the unit's current command
    BWAPI::UnitCommand currentCommand(unit->getLastCommand());

    // if we've already told this unit to right-click this target, ignore this command
    if ((currentCommand.getType() == BWAPI::UnitCommandTypes::Right_Click_Unit) && (currentCommand.getTargetPosition() == target->getPosition()))
    {
        return;
    }

    // NOTE This treats a right-click on one of our own units as an order to attack it.
    BWAPI::Order order = BWAPI::Orders::AttackUnit;
    if (target->getType() == BWAPI::UnitTypes::Resource_Mineral_Field)
    {
        order = BWAPI::Orders::MoveToMinerals;
    }
    else if (target->getType().isRefinery())
    {
        order = BWAPI::Orders::MoveToGas;
    }
    orders[unit].setOrder(unit, order, target);

    // if nothing prevents it, attack the target
    unit->rightClick(target);
    TotalCommands++;

    if (Config::Debug::DrawUnitTargets) 
    {
        BWAPI::Broodwar->drawCircleMap(unit->getPosition(), dotRadius, BWAPI::Colors::Cyan, true);
        BWAPI::Broodwar->drawCircleMap(target->getPosition(), dotRadius, BWAPI::Colors::Cyan, true);
        BWAPI::Broodwar->drawLineMap(unit->getPosition(), target->getPosition(), BWAPI::Colors::Cyan);
    }
}

// This is part of the support for mineral locking. It works for gathering resources.
// To mine out blocking minerals with zero resources, or to mineral walk, use RightClick() instead.
void Micro::MineMinerals(BWAPI::Unit unit, BWAPI::Unit mineralPatch)
{
    if (!unit || !unit->exists() || unit->getPlayer() != the.self() ||
        !mineralPatch || !mineralPatch->exists() || !mineralPatch->getType().isMineralField())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }

    // if we have issued a command to this unit already this frame, ignore this one
    if (unit->getLastCommandFrame() >= the.now() || unit->isAttackFrame())
    {
        return;
    }

    orders[unit].setOrder(unit, BWAPI::Orders::MoveToMinerals, mineralPatch);

    unit->rightClick(mineralPatch);
    TotalCommands++;

    if (Config::Debug::DrawUnitTargets)
    {
        BWAPI::Broodwar->drawLineMap(unit->getPosition(), mineralPatch->getPosition(), BWAPI::Colors::Cyan);
    }
}

bool Micro::LaySpiderMine(BWAPI::Unit unit, const BWAPI::Position& pos)
{
    if (!unit || !unit->exists() || unit->getPlayer() != the.self() || !pos.isValid())
    {
        UAB_ASSERT(false, "bad arg");
        return false;
    }

    // Spider mines do friendly splash damage. Keep every placement path safe
    // in one place, including retreat, worker-harass and kamikaze mines.
    // Reserve build/traffic footprints, including the wall's lifted gate.
    const BWAPI::TilePosition tile(pos);
    if (the.placer.isReserved(tile)) return false;
    const int friendlyExclusionRadius = 3 * 32;
    for (BWAPI::Unit friendly : the.self()->getUnits())
    {
        if (friendly && friendly->exists() && friendly != unit)
        {
            if (friendly->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine &&
                friendly->getDistance(pos) < 3 * 32) return false;
            // Include pending placements: two Vultures can act in the same
            // frame before either new mine exists in BWAPI's unit list.
            if (friendly->getType() == BWAPI::UnitTypes::Terran_Vulture)
            {
                const BWAPI::UnitCommand command = friendly->getLastCommand();
                if (command.getType() == BWAPI::UnitCommandTypes::Use_Tech_Position &&
                    command.getTechType() == BWAPI::TechTypes::Spider_Mines &&
                    (friendly->getOrder() == BWAPI::Orders::PlaceMine ||
                     the.now() - friendly->getLastCommandFrame() <= BWAPI::Broodwar->getLatencyFrames() + 2) &&
                    command.getTargetPosition().getApproxDistance(pos) < 3 * 32) return false;
            }
        }
        if (!friendly ||
            !friendly->exists() ||
            friendly == unit ||
            friendly->isFlying() ||
            friendly->getType().isBuilding() ||
            friendly->getType() == BWAPI::UnitTypes::Terran_Vulture_Spider_Mine ||
            !friendly->getPosition().isValid())
        {
            continue;
        }

        if (friendly->getDistance(pos) <= friendlyExclusionRadius)
        {
            return false;
        }
    }

    if (!unit->canUseTech(BWAPI::TechTypes::Spider_Mines, pos))
    {
        return false;
    }

    if (unit->getLastCommandFrame() >= the.now())
    {
        return false;
    }

    BWAPI::UnitCommand currentCommand(unit->getLastCommand());

    if ((currentCommand.getType() == BWAPI::UnitCommandTypes::Use_Tech_Position) &&
        (currentCommand.getTechType() == BWAPI::TechTypes::Spider_Mines) &&
        (currentCommand.getTargetPosition() == pos) && unit->getOrder() == BWAPI::Orders::PlaceMine)
    {
        return true;
    }

    if (!unit->useTech(BWAPI::TechTypes::Spider_Mines, pos)) return false;
    orders[unit].setOrder(unit, BWAPI::Orders::PlaceMine, pos);
    TotalCommands++;
    return true;
}

void Micro::Repair(BWAPI::Unit unit, BWAPI::Unit target)
{
    if (!unit || !unit->exists() || unit->getPlayer() != the.self() ||
        !target || !target->exists())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }

    // if we have issued a command to this unit already this frame, ignore this one
    if (unit->getLastCommandFrame() >= the.now() || unit->isAttackFrame())
    {
        return;
    }

    // get the unit's current command
    BWAPI::UnitCommand currentCommand(unit->getLastCommand());

    // if we've already told this unit to move to this position, ignore this command
    if ((currentCommand.getType() == BWAPI::UnitCommandTypes::Repair) && (currentCommand.getTarget() == target))
    {
        return;
    }

    orders[unit].setOrder(unit, BWAPI::Orders::Repair, target);

    // Nothing prevents it, so attack the target.
    unit->repair(target);
    TotalCommands++;

    if (Config::Debug::DrawUnitTargets) 
    {
        BWAPI::Broodwar->drawCircleMap(unit->getPosition(), dotRadius, BWAPI::Colors::Cyan, true);
        BWAPI::Broodwar->drawCircleMap(target->getPosition(), dotRadius, BWAPI::Colors::Cyan, true);
        BWAPI::Broodwar->drawLineMap(unit->getPosition(), target->getPosition(), BWAPI::Colors::Cyan);
    }
}

// Use one of the given base's ground path grids to find a path to a location near the base.
// If safe is set, use the safe path grid; otherwise use the plain distances.
// One call handles the entire movement, but it can be updated every frame  without harm.
bool Micro::MoveToBase(BWAPI::Unit unit, Base * targetBase, const BWAPI::Position & targetPosition, bool safe)
{
    if (tankTransitionPending(unit) || (unit && unit->exists() && unit->isSieged()))
    {
        return false;
    }

    if (!unit || !unit->exists() || !targetBase || !targetPosition.isValid())
    {
        return false;
    }

    // The closed Barracks gate can make the base's safe grid report no route.
    // Keep the final base target so MicroState follows up after the crossing.
    if (!unit->isFlying() &&
        the.placer.terranMainWallTransitWaypoint(
            unit->getPosition(), targetPosition, unit->getID()).isValid())
    {
        orders[unit].setOrder(unit, BWAPI::Orders::Move,
            targetPosition, targetBase, safe);
        return true;
    }

	bool ok;		// can we get there, according to the grid?
	if (safe)
	{
		ok = targetBase->getSafeTileDistance(unit->isFlying(), unit->getTilePosition()) >= 0;
	}
	else
	{
		ok = targetBase->getDistance(unit->getTilePosition()) >= 0;
	}

	if (ok)
	{
		orders[unit].setOrder(unit, BWAPI::Orders::Move, targetPosition, targetBase, safe);

		if (Config::Debug::DrawUnitTargets)
		{
			BWAPI::Broodwar->drawCircleMap(unit->getPosition(), dotRadius, BWAPI::Colors::Cyan, true);
			BWAPI::Broodwar->drawCircleMap(targetPosition, dotRadius, BWAPI::Colors::Cyan, true);
			BWAPI::Broodwar->drawLineMap(unit->getPosition(), targetPosition, BWAPI::Colors::Cyan);
		}
	}

	return ok;
}

void Micro::ReturnCargo(BWAPI::Unit worker)
{
    if (!worker || !worker->exists() || worker->getPlayer() != the.self() ||
        !worker->getType().isWorker())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }

    // If the worker has no cargo, ignore this command.
    if (!worker->isCarryingMinerals() && !worker->isCarryingGas())
    {
        return;
    }

    // if we have issued a command to this unit already this frame, ignore this one
    if (worker->getLastCommandFrame() >= the.now() || worker->isAttackFrame())
    {
        return;
    }

    BWAPI::Unit depot = nullptr;
    if (the.placer.hasTerranMainWall())
    {
        int bestDistance = MAX_DISTANCE;
        for (BWAPI::Unit candidate : the.self()->getUnits())
        {
            if (!candidate || !UnitUtil::IsCompletedResourceDepot(candidate))
                continue;
            const int distance = worker->getDistance(candidate);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                depot = candidate;
            }
        }
    }
    if (depot && the.placer.terranMainWallTransitWaypoint(
        worker->getPosition(), depot->getPosition(), worker->getID()).isValid())
    {
        MoveNear(worker, depot->getPosition());
        return;
    }

    // If we've already issued this command, don't issue it again.
    BWAPI::UnitCommand currentCommand(worker->getLastCommand());
    if (currentCommand.getType() == BWAPI::UnitCommandTypes::Return_Cargo)
    {
        return;
    }

    orders[worker].setOrder(worker, worker->isCarryingMinerals() ? BWAPI::Orders::ReturnMinerals : BWAPI::Orders::ReturnGas);

    // Nothing prevents it, so return cargo.
    worker->returnCargo();
    TotalCommands++;
}

// Order construction of a building.
bool Micro::Build(BWAPI::Unit builder, BWAPI::UnitType building, const BWAPI::TilePosition & location)
{
    if (!builder || !builder->exists() || !builder->getType().isWorker() ||
        !builder->getPosition().isValid() || builder->isBurrowed() ||
        builder->getPlayer() != the.self() ||
        !building.isBuilding() || !location.isValid())
    {
        UAB_ASSERT(false, "bad building");
        return false;
    }

    // NOTE This order does not set the type or location.
    //      The purpose is only to remind MicroState that the unit has this order.
    orders[builder].setOrder(builder, BWAPI::Orders::ConstructingBuilding);

    return builder->build(building, location);
}

// Order production of a unit. Includes terran addon and zerg morphed building or unit.
bool Micro::Make(BWAPI::Unit producer, BWAPI::UnitType type)
{
    if (!producer || !producer->exists() || !producer->getPosition().isValid() || producer->getType().getRace() != type.getRace())
    {
        UAB_ASSERT(false, "bad unit");
        return false;
    }

    // NOTE We do not set the type.
    //      The purpose is only to remind MicroState that the unit has this order.

    if (type.isAddon())
    {
        orders[producer].setOrder(producer, BWAPI::Orders::BuildAddon);
        return producer->buildAddon(type);
    }

    // Zerg morphs units, except for the infested terran, which is trained. Tricky!
    

    orders[producer].setOrder(producer, BWAPI::Orders::Train);
    return producer->train(type);
}

// Cancel a building under construction or a morphing unit.
bool Micro::Cancel(BWAPI::Unit unit)
{
    if (!unit || !unit->exists() || !unit->getPosition().isValid())
    {
        UAB_ASSERT(false, "bad unit");
        return false;
    }

    orders[unit].setOrder(unit, BWAPI::Orders::Stop);

    return unit->cancelMorph() || unit->cancelConstruction();
}

bool Micro::Lift(BWAPI::Unit terranBuilding)
{
    if (!terranBuilding || !terranBuilding->exists() ||
        !terranBuilding->getType().isBuilding() ||
        terranBuilding->getPlayer() != the.self())
    {
        UAB_ASSERT(false, "bad unit");
        return false;
    }

    orders[terranBuilding].setOrder(terranBuilding, BWAPI::Orders::LiftingOff);

    return terranBuilding->lift();
}

// Burrow a zerg unit.


// Unburrow a zerg unit.


// Load a unit into a bunker or transport.
bool Micro::Load(BWAPI::Unit container, BWAPI::Unit content)
{
    if (!container || !container->exists() || !content || !content->exists() || !content->getPosition().isValid())
    {
        UAB_ASSERT(false, "bad unit");
        return false;
    }

    if (container->getType() == BWAPI::UnitTypes::Terran_Bunker)
    {
        orders[container].setOrder(container, BWAPI::Orders::PickupBunker, content);
    }
    else
    {
        orders[container].setOrder(container, BWAPI::Orders::PickupTransport, content);
    }

    return container->load(content);
}

// Move to the given position and unload all units from a transport.
bool Micro::UnloadAt(BWAPI::Unit container, const BWAPI::Position & targetPosition)
{
    if (!container || !container->exists() || !container->getPosition().isValid() ||
        !targetPosition.isValid())
    {
        UAB_ASSERT(false, "bad unit");
        return false;
    }

    orders[container].setOrder(container, BWAPI::Orders::MoveUnload);

    return container->unloadAll(targetPosition);
}

// Unload all units from a bunker or transport.
bool Micro::UnloadAll(BWAPI::Unit container)
{
    if (!container || !container->exists() || !container->getPosition().isValid())
    {
        UAB_ASSERT(false, "bad unit");
        return false;
    }

    orders[container].setOrder(container, BWAPI::Orders::Unload);

    return container->unloadAll();
}

// Siege a tank.
bool Micro::Siege(BWAPI::Unit tank)
{
    if (!tank || !tank->exists() || !tank->getPosition().isValid())
    {
        UAB_ASSERT(false, "bad unit");
        return false;
    }
    if (trafficYieldActive(tank)) return false;

    // Final invariant for every caller, including generic ImmobilizeUnit():
    // never deploy inside the unsafe ring around a resource depot. Formation
    // code normally keeps Tanks out already, but this prevents an emergency
    // branch from issuing the StarCraft-clipping Siege command in place.
    if (!UnitUtil::TankPositionHasSafeDepotClearance(tank->getPosition()))
    {
        return false;
    }

    if (tankTransitionPending(tank))
    {
        return false;
    }

    const int now = the.now();
    resetTankTransitionStateForNewGame(now);

    const auto lastUnsiege =
        g_lastTankUnsiegeCommandFrame.find(tank->getID());

    // One central cooldown prevents independent tank-micro branches from
    // immediately undoing each other's command.
    if (lastUnsiege != g_lastTankUnsiegeCommandFrame.end() &&
        now - lastUnsiege->second < 48)
    {
        return false;
    }

    const bool issued = tank->siege();
    if (issued)
    {
        orders[tank].setOrder(tank, BWAPI::Orders::Sieging);
        g_lastTankSiegeCommandFrame[tank->getID()] = now;
    }

    return issued;
}

// MicroTanks supplies the observation policy once per frame. Enforce its hold
// at the common command boundary so gathering, defense and regrouping cannot
// undo it through an early-return branch.
void Micro::setSiegeHoldUntil(BWAPI::Unit tank, int untilFrame)
{
    if (!tank || !tank->exists())
    {
        return;
    }
    const int now = the.now();
    resetTankTransitionStateForNewGame(now);
    if (untilFrame > now)
    {
        g_tankSiegeHoldUntil[tank->getID()] = untilFrame;
    }
    else
    {
        g_tankSiegeHoldUntil.erase(tank->getID());
    }
}

bool Micro::siegeHoldActive(BWAPI::Unit tank) const
{
    if (!tank || !tank->exists())
    {
        return false;
    }
    const auto hold = g_tankSiegeHoldUntil.find(tank->getID());
    return the.now() >= g_lastTankTransitionStateFrame &&
        hold != g_tankSiegeHoldUntil.end() && the.now() < hold->second;
}

// Unsiege a tank.
bool Micro::Unsiege(BWAPI::Unit tank, bool emergency)
{
    if (!tank || !tank->exists() || !tank->getPosition().isValid())
    {
        UAB_ASSERT(false, "bad unit");
        return false;
    }

    if (tankTransitionPending(tank))
    {
        return false;
    }

    const int now = the.now();
    resetTankTransitionStateForNewGame(now);

    const auto lastSiege =
        g_lastTankSiegeCommandFrame.find(tank->getID());

    if (!emergency &&
        (siegeHoldActive(tank) ||
         (lastSiege != g_lastTankSiegeCommandFrame.end() &&
          now - lastSiege->second < 4 * 24)))
    {
        return false;
    }

    const bool issued = tank->unsiege();
    if (issued)
    {
        orders[tank].setOrder(tank, BWAPI::Orders::Unsieging);
        g_lastTankUnsiegeCommandFrame[tank->getID()] = now;
    }

    return issued;
}

// Perform a comsat scan at the given position if possible and necessary.
// If it's not possible, or we already scanned there, do nothing.
// Return whether the scan occurred.
// NOTE Comsat scan does not use the orders[] mechanism.
bool Micro::ScanWithComsat(BWAPI::Unit comsat, const BWAPI::Position& targetPosition)
{
    UAB_ASSERT(targetPosition.isValid(), "bad position");

    if (!comsat ||
        !comsat->exists() ||
        comsat->getType() != BWAPI::UnitTypes::Terran_Comsat_Station)
    {
        return false;
    }

    // If a scan of this position is still active, don't scan it again.
    if (MapGrid::Instance().scanIsActiveAt(targetPosition))
    {
        return false;
    }

    if (comsat->getEnergy() <= 49 ||
        !comsat->canUseTech(BWAPI::TechTypes::Scanner_Sweep, targetPosition))
    {
        return false;
    }

    const int energyBeforeScan = comsat->getEnergy();

    const bool scanStarted =
        comsat->useTech(BWAPI::TechTypes::Scanner_Sweep, targetPosition);

    if (scanStarted)
    {
        MapGrid::Instance().scanAtPosition(targetPosition);

        rememberComsatScan(comsat, targetPosition, energyBeforeScan);
    }

    return scanStarted;
}

bool Micro::Scan(const BWAPI::Position& targetPosition)
{
    UAB_ASSERT(targetPosition.isValid(), "bad position");

    int maxEnergy = 49;
    BWAPI::Unit bestComsat = nullptr;

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit->getType() == BWAPI::UnitTypes::Terran_Comsat_Station &&
            unit->getEnergy() > maxEnergy &&
            unit->canUseTech(BWAPI::TechTypes::Scanner_Sweep, targetPosition))
        {
            maxEnergy = unit->getEnergy();
            bestComsat = unit;
        }
    }

    return ScanWithComsat(bestComsat, targetPosition);
}

bool Micro::ScanExceptComsat(BWAPI::Unit excludedComsat, const BWAPI::Position& targetPosition)
{
    UAB_ASSERT(targetPosition.isValid(), "bad position");

    int maxEnergy = 49;
    BWAPI::Unit bestComsat = nullptr;

    for (BWAPI::Unit unit : the.self()->getUnits())
    {
        if (unit == excludedComsat)
        {
            continue;
        }

        if (unit->getType() == BWAPI::UnitTypes::Terran_Comsat_Station &&
            unit->getEnergy() > maxEnergy &&
            unit->canUseTech(BWAPI::TechTypes::Scanner_Sweep, targetPosition))
        {
            maxEnergy = unit->getEnergy();
            bestComsat = unit;
        }
    }

    return ScanWithComsat(bestComsat, targetPosition);
}
bool Micro::Stim(BWAPI::Unit unit)
{
    if (!unit ||
        unit->getType() != BWAPI::UnitTypes::Terran_Marine && unit->getType() != BWAPI::UnitTypes::Terran_Firebat ||
        unit->getPlayer() != the.self())
    {
        UAB_ASSERT(false, "bad unit");
        return false;
    }

    if (unit->isStimmed())
    {
        return false;
    }

    // if we have issued a command to this unit already this frame, ignore this one
    if (unit->getLastCommandFrame() >= the.now())
    {
        return false;
    }

    // Allow a small latency for a previous stim command to take effect.
    // Marines and firebats have only 1 tech to use, so we don't need to check which.
    if (unit->getLastCommand().getType() == BWAPI::UnitCommandTypes::Use_Tech &&
        the.now() - unit->getLastCommandFrame() < 8)
    {
        return false;
    }

    // useTech() checks whether stim is researched and any other conditions.
    return unit->useTech(BWAPI::TechTypes::Stim_Packs);
}







// Use a tech on a target unit.
// NOTE The order is set correctly only for techs that Steamhammer already implements.
bool Micro::UseTech(BWAPI::Unit unit, BWAPI::TechType tech, BWAPI::Unit target)
{
    if (!unit || !unit->exists() || !unit->getPosition().isValid() || unit->getPlayer() != the.self() ||
        !target || !target->exists() || !target->getPosition().isValid())
    {
        UAB_ASSERT(false, "bad unit");
        return false;
    }

    BWAPI::Order o;
       if (tech == BWAPI::TechTypes::Lockdown)
    {
        o = BWAPI::Orders::CastLockdown;
    }
    else if (tech == BWAPI::TechTypes::Healing)
    {
        o = BWAPI::Orders::MedicHeal;
    }
    else
    {
        o = BWAPI::Orders::UnusedNothing;
        UAB_ASSERT(false, "unsupported tech");
    }
    orders[unit].setOrder(unit, o);

    return unit->useTech(tech, target);
}




void Micro::KiteTarget(BWAPI::Unit rangedUnit, BWAPI::Unit target)
{
    if (!rangedUnit || !rangedUnit->exists() || rangedUnit->getPlayer() != the.self() ||
        !target || !target->exists())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }

    // The always-kite units have their own micro. Validate the unit first;
    // callers are allowed to lose a target/unit between assignment and micro.
    if (alwaysKite(rangedUnit->getType()))
    {
        CooldownDanceTarget(rangedUnit, target);
        return;
    }

    BWAPI::WeaponType weapon = UnitUtil::GetWeapon(rangedUnit, target);
    double range = rangedUnit->getPlayer()->weaponMaxRange(weapon);

    bool kite = true;

    // Only kite if somebody wants to shoot us.
    if (InformationManager::Instance().getEnemyFireteam(rangedUnit).empty())
    {
        kite = false;
    }

    // Kite if we're not ready yet: Wait for the weapon.
    double dist(rangedUnit->getDistance(target));
    double speed(rangedUnit->getPlayer()->topSpeed(rangedUnit->getType()));
    double timeToEnter = 0.0;                      // time to reach firing range
    if (speed > .00001)                            // don't even visit the same city as division by zero
    {
        timeToEnter = std::max(0.0, dist - range) / speed;
    }
    if (timeToEnter >= weapon.damageCooldown() + BWAPI::Broodwar->getRemainingLatencyFrames() ||
        target->getType().isBuilding())
    {
        kite = false;
    }

    if (kite)
    {
        // Run away if we can. If we can't, then kite becomes false.
        kite = kiteBack(rangedUnit, target);
    }
    
    if (!kite)
    {
        // Shoot.
        Micro::CatchAndAttackUnit(rangedUnit, target);
        //BWAPI::Broodwar->drawLineMap(rangedUnit->getPosition(), target->getPosition(), BWAPI::Colors::Orange);
    }
}

// Used for fast Terran units with no delay in making turns.
// See alwaysKite() for the set of units this is used for.


void Micro::setAttack(const BWAPI::Unitset & units, bool a)
{
    for (BWAPI::Unit u : units)
    {
        orders[u].setAttack(a);
    }
}

// The smoothed attack is in the range [0,1]. Values >= 0.5 mean attack.
// We subtract 0.5 to make it less confusing for the caller, which
// expects values >= 0 to mean attack.
double Micro::getSmoothedAttack(const BWAPI::Unitset & units)
{
    if (units.size() == 0)
    {
        return false;
    }

    double total = 0.0;
    for (BWAPI::Unit u : units)
    {
        total += orders[u].getSmoothedAttack();
    }
    return total / units.size() - 0.5;
}

// Cooldown kiting shared by fast Terran ground and air units.
void Micro::CooldownDanceTarget(BWAPI::Unit rangedUnit, BWAPI::Unit target)
{
    if (!rangedUnit || !rangedUnit->exists() || rangedUnit->getPlayer() != the.self() ||
        !target || !target->exists())
    {
        UAB_ASSERT(false, "bad arg");
        return;
    }

    if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture &&
        (rangedUnit->isAttackFrame() || rangedUnit->isStartingAttack())) return;

    const int latency					= BWAPI::Broodwar->getRemainingLatencyFrames();

    const int framesToEnterFiringRange	= UnitUtil::FramesToReachAttackRange(rangedUnit, target);
    const int framesToAttack			= framesToEnterFiringRange + 2 * latency;

    // How many frames are left before we should order the attack?
    const int cooldownNow =
        target->isFlying() ? rangedUnit->getAirWeaponCooldown() : rangedUnit->getGroundWeaponCooldown();
    const int staticCooldown =
        target->isFlying() ? rangedUnit->getType().airWeapon().damageCooldown() : rangedUnit->getType().groundWeapon().damageCooldown();
    const int cooldown =
        rangedUnit->isStartingAttack() ? staticCooldown : cooldownNow;

    if (cooldown <= framesToAttack)
    {
        // Attack.
        // This is functionally equivalent to Micro::CatchAndAttackUnit(rangedUnit, target) .

        if (!target->isMoving() || rangedUnit->isInWeaponRange(target))
        {
            //BWAPI::Broodwar->drawLineMap(rangedUnit->getPosition(), target->getPosition(), BWAPI::Colors::Orange);
            AttackUnit(rangedUnit, target);
        }
        else
        {
            BWAPI::Position destination = PredictMovement(target, std::min(framesToEnterFiringRange, 12));
            //BWAPI::Broodwar->drawLineMap(target->getPosition(), destination, BWAPI::Colors::Grey);
            //BWAPI::Broodwar->drawLineMap(rangedUnit->getPosition(), destination, BWAPI::Colors::Yellow);
            MoveNear(rangedUnit, destination);
        }
    }
    else
    {
        if (rangedUnit->getType() == BWAPI::UnitTypes::Terran_Vulture)
        {
            // Flying-unit retreat ignored cliffs, Tanks and buildings. Use
            // checked ground steps, and don't retreat from a harmless target
            // merely because the grenade is reloading.
            const int reach = UnitUtil::GetAttackRange(target, rangedUnit) + 48 +
                int(target->getPlayer()->topSpeed(target->getType()) * std::min(12, cooldown));
            if (UnitUtil::CanAttack(target, rangedUnit) && rangedUnit->getDistance(target) <= reach)
            {
                if (!kiteBack(rangedUnit, target)) AttackUnit(rangedUnit, target);
            }
            else if (rangedUnit->isInWeaponRange(target)) AttackUnit(rangedUnit, target);
            // Already at a safe firing distance: wait for the shot instead
            // of repeatedly running behind our own Tank line.
        }
        else fleeEnemy(rangedUnit, target, 64);
    }
}
