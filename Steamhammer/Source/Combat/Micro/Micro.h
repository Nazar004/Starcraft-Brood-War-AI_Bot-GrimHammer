#pragma once

#include "Core/Common.h"

namespace UAlbertaBot
{
class Base;
class Grid;
class GridDistances;

class MicroState
{
private:
    BWAPI::Order order;
    BWAPI::Unit targetUnit;				// nullptr if none
    BWAPI::Position targetPosition;		// None if none
    BWAPI::Position finalMovePosition; // final goal while crossing the main Barracks gate
    BWAPI::Unit pendingGateRightClick; // resume a one-shot unit action beyond the gate
	bool safe;							// try to stay out of enemy fire
	Base * targetBase;					// use this base's distance and/or safe path grids

	// Combat sim result smoothing.
    double attack;                      // <.5 to retreat, >= .5 to attack
    int attackUpdateFrame;
    inline static const double attackDecay = 0.15;  // for exponential moving average

    int orderFrame;						// when the order was given
    int executeFrame;					// -1 if not executed yet
	bool needsFollowup;					// if true, follow up to continue executing the order
    bool needsMonitoring;				// if true, monitor the result
    int lastCheckFrame;					// execute frame or latest monitored frame
    int lastActionFrame;                // time of issuing last order to BWAPI (even if failed), persists across setOrder()

    // Progress tracking for ground Move orders. BWAPI's isStuck() alone is not
    // enough: units can spend seconds pushing against friendly traffic while
    // still reporting a normal Move order. Keep the real destination and use
    // a short temporary recovery move when no progress is made.
    BWAPI::Position progressPosition;
    int progressDistance;
    int lastProgressFrame;
    BWAPI::Position recoveryPosition;
    int recoveryUntilFrame;
    int recoveryAttempts;

	const int DangerMargin = 2 * 32;	// safe distance beyond enemy firing range

    static const int FramesBetweenActions = 2;
	BWAPI::Position followupTarget;     // interim target that may not be all the way to the targetPosition

    // Debugging test: Complain if something looks bad.
    void check(BWAPI::Unit u, BWAPI::Order o) const;

    void execute(BWAPI::Unit u);		// carry out the order
	void followup(BWAPI::Unit u);		// the order is executed piece by piece over time
	void monitor(BWAPI::Unit u);		// check for and try to correct failures
    void resetMoveProgress(BWAPI::Unit u);
    BWAPI::Position findMoveRecoveryPosition(BWAPI::Unit u) const;

    bool positionsNearlyEqual(BWAPI::Unit u, const BWAPI::Position & pos1, const BWAPI::Position & pos2) const;
	BWAPI::Position nextDestination(BWAPI::Unit u) const;
	BWAPI::Position dodge(BWAPI::Unit u) const;
	BWAPI::Position flee(BWAPI::Unit u) const;

public:
    BWAPI::Position startPosition;

    MicroState();

    void setOrder(BWAPI::Unit u, BWAPI::Order o);
	void setOrder(BWAPI::Unit u, BWAPI::Order o, BWAPI::Unit t);
	void setOrder(BWAPI::Unit u, BWAPI::Order o, BWAPI::Position p);
    void setOrder(BWAPI::Unit u, BWAPI::Order o, BWAPI::Position p, Base * b, bool s);
    void setFinalMovePosition(const BWAPI::Position & p) { finalMovePosition = p; }
    void setPendingGateRightClick(BWAPI::Unit unit) { pendingGateRightClick = unit; }

    BWAPI::Order getOrder() const { return order; };
    BWAPI::Unit getTargetUnit() const { return targetUnit; };
    BWAPI::Position getTargetPosition() const { return targetPosition; };
    int getOrderFrame() const { return orderFrame; };

    void update(BWAPI::Unit u);

    void setAttack(bool a);
    double getSmoothedAttack() const;

    void draw(BWAPI::Unit u) const;
};

// Micro implements unit actions that the rest of the program can treat as primitive.

class Micro
{
    struct TrafficYield
    {
        BWAPI::Position position;
        int untilFrame;
    };

    std::map<BWAPI::Unit, MicroState> orders;
    std::map<BWAPI::Unit, TrafficYield> trafficYields;
    int lastTrafficYieldFrame = -100000;
    MicroState noOrder;

    bool alwaysKite(BWAPI::UnitType type) const;
    bool trafficYieldActive(BWAPI::Unit unit) const;

    BWAPI::Position nextGroundDestination(BWAPI::Unit unit, const BWAPI::Position & destination, const GridDistances * distances) const;

public:
    Micro();
    void resetForNewGame();

    // Call this at the end of the frame to execute any orders stored in the orders map.
    void update();

    // Ask an idle ground unit in a narrow passage to make room for a stalled
    // allied mover. Only invoked by the slow-path Move progress monitor.
    bool yieldBlockingUnit(BWAPI::Unit mover, const BWAPI::Position& destination);

    const MicroState & getMicroState(BWAPI::Unit unit) const;
    bool alreadyCommanded(BWAPI::Unit unit) const;

    BWAPI::Unit inWeaponsDanger(BWAPI::Unit unit, int margin) const;
    bool canFleeTo(BWAPI::Unit unit, const BWAPI::Position & destination) const;
    bool kiteBack(BWAPI::Unit unit, BWAPI::Unit enemy);
    BWAPI::Position fleeTo(BWAPI::Unit unit, const BWAPI::Position & danger, int distance) const;
    void fleePosition(BWAPI::Unit unit, const BWAPI::Position & danger, int distance = 8 * 32);
    void fleeEnemy(BWAPI::Unit unit, BWAPI::Unit enemy, int distance = 8 * 32);
    bool fleeDT(BWAPI::Unit unit);

    void Stop(BWAPI::Unit unit);
    void HoldPosition(BWAPI::Unit unit);
    void CatchAndAttackUnit(BWAPI::Unit attacker, BWAPI::Unit target);
    void AttackUnit(BWAPI::Unit attacker, BWAPI::Unit target);
    void AttackMove(BWAPI::Unit attacker, const BWAPI::Position & targetPosition);
    void Move(BWAPI::Unit unit, const BWAPI::Position & targetPosition, const GridDistances * distances = nullptr);
    void MoveNear(BWAPI::Unit attacker, const BWAPI::Position & targetPosition, const GridDistances * distances = nullptr);
    bool MoveSafely(BWAPI::Unit unit, const BWAPI::Position & targetPosition, const GridDistances * distances = nullptr);
	void RightClick(BWAPI::Unit unit, BWAPI::Unit target);
    void MineMinerals(BWAPI::Unit unit, BWAPI::Unit mineralPatch);
    bool LaySpiderMine(BWAPI::Unit unit, const BWAPI::Position & pos);
    void Repair(BWAPI::Unit unit, BWAPI::Unit target);		// TODO UNIMPLEMENTED
    void ReturnCargo(BWAPI::Unit worker);

	bool MoveToBase(BWAPI::Unit unit, Base * targetBase, const BWAPI::Position & targetPosition, bool safe);
	
	bool Build(BWAPI::Unit builder, BWAPI::UnitType building, const BWAPI::TilePosition & location);
    bool Make(BWAPI::Unit producer, BWAPI::UnitType type);
    bool Cancel(BWAPI::Unit unit);
    bool Lift(BWAPI::Unit terranBuilding);


    bool Load(BWAPI::Unit container, BWAPI::Unit content);
    bool UnloadAt(BWAPI::Unit container, const BWAPI::Position & targetPosition);
    bool UnloadAll(BWAPI::Unit container);

    bool Siege(BWAPI::Unit tank);
    bool Unsiege(BWAPI::Unit tank, bool emergency = false);
    void setSiegeHoldUntil(BWAPI::Unit tank, int untilFrame);
    bool siegeHoldActive(BWAPI::Unit tank) const;

    bool Scan(const BWAPI::Position& targetPosition);
    bool ScanWithComsat(BWAPI::Unit comsat, const BWAPI::Position& targetPosition);
    bool ScanExceptComsat(BWAPI::Unit excludedComsat, const BWAPI::Position& targetPosition);
    bool Stim(BWAPI::Unit unit);


    bool UseTech(BWAPI::Unit unit, BWAPI::TechType tech, BWAPI::Unit target);

    void CooldownDanceTarget(BWAPI::Unit rangedUnit, BWAPI::Unit target);

    void KiteTarget(BWAPI::Unit rangedUnit, BWAPI::Unit target);

    void setAttack(const BWAPI::Unitset & units, bool a);
    double getSmoothedAttack(const BWAPI::Unitset & units);
};
}
