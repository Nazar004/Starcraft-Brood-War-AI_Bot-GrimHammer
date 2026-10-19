# Fortress: closed gates and ground movement

Inspected 2026-10-12. No project build, game run, or replay execution.

## Map evidence

Read `maps/BroodWar/AIIDE/(4)Fortress.scx` and the installed game's jungle
CV5/VF4 terrain data from `STARDAT.MPQ`, without modifying either file.
The map is 128x128 build tiles. The low three ERA bits select Jungle.

THG2 contains eight unit sprites of types 207/208 (neutral invincible pit
doors), at pixel positions:

- (3296, 832), (736, 3328)
- (672, 768), (3488, 3392)
- (864, 864), (3296, 3296)
- (3104, 928), (928, 3232)

All eight door centers have walkable underlying terrain. They are blocked by
the neutral units, which `MapPartitions` already includes in its occupancy.
The screenshot's `WALL: OPEN` describes the bot's Barracks wall, not these doors.

The old Tank gather check tested only one terrain walk cell. It accepted all
eight door centers. Tank parking checked the complete terrain footprint, but
its additional unit query only considered buildings/resources; doors are neither.
Thus both checks could accept a closed door or an adjacent narrow gap.

Separately, coarse distance cells are accepted when they contain any 16x16
walkable patch. Movement returned their geometric centers without validating
the mover's collision box. Reconstructing terrain plus immobile neutrals found
1,044 such cells whose center is invalid for a 32x32 Tank. This is a map-wide
count, not a count of observed collisions in the screenshot.

## Changes

- `MapTools`: full-body position validation against `MapPartitions`, optionally
  including current grounded building occupancy. A bounded nine-point search
  returns a valid point within a coarse cell, or Invalid.
- `Micro`: validate intermediate points for the actual mover. If a coarse
  waypoint cannot fit it, preserve the final destination for native routing.
  Move/AttackMove/base movement also project a goal inside a static obstacle
  onto nearby valid ground in the unit's existing partition.
- `MicroTanks`: gather and parking use the full static footprint, including
  neutral doors/eggs. Base-route intermediate points must fit a Tank; otherwise
  pass the actual final goal to normal movement.
- `getGroundPathWaypoint`: validate returned points, including runtime A*
  results. Do not return the original blocked goal after normalizing it to a
  different tile.

Ordinary valid movement uses a small footprint check. The fallback searches
at most a three-tile neighborhood. No new full-map distance fields, BWAPI
hasPath queries, or high-resolution per-unit searches were introduced.

## Offline verification

`audit_fortress.py` reconstructs terrain and neutral footprints and checks:

- all eight closed door centers fail full-body validation;
- the bounded projection neighborhood contains Tank-sized ground for each door;
- of the 1,044 invalid centers, 581 have a safe shifted Tank waypoint;
- the other 463 require the native final-goal fallback;
- every returned Tank waypoint fits its complete footprint;
- every previously accepted 16x16 coarse passage retains a minimum-size waypoint;
- 16px cracks are rejected for Tanks; bounds and Marine/Goliath/SCV/Medic body
  checks pass.

Also inspected the changed source/API call sites and checked diff whitespace.
These checks do not execute the C++ implementation. The screenshot alone does
not identify the precise live orders or prove that all possible movement
deadlocks are fixed; a subsequent Fortress match is still needed.

## Format references

- [THG2 reference from the scmJS parser project](https://docs.scmjs.dev/chk/thg2/)
- [PKWARE stream format documented in zlib's blast](https://github.com/madler/zlib/blob/master/contrib/blast/blast.c)

The MPQ/CHK inspector is a read-only Python utility stored in this Source tree.
