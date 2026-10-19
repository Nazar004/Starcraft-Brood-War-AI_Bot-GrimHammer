# BioTank

Ordinary `TerranPlanId::BioTank` keeps Marine/Medic/Tank production as its
macro identity. Counter units do not switch it to the mech host.

## Files and scope

- `BioTankPolicy.h`: numerical composition, economy-capacity and support rules.
- `BioTankAssessment.h`: current enemy counts, first-push assembly, reachable
  base threats, local garrisons and counter supply reservations.
- `Combat/Micro/Terran/BioInfantrySafety.h`: immediate Storm/Scarab escape.
- `StrategyManager.cpp`: adapters to the existing production executor.

Sparks, Pre-Hive and SK retain their separate production targets and first-push
contracts. Deep Six retains 24 Marines / 5 Medics / 4 Tanks and its upgrades;
it shares the mobile assembly check and the corrected capital-air counter.
Bio infantry safety and combat-evaluation fixes apply to the bio family.
Mech retains its existing production and combat-readiness shortcuts.

## First push and defense

Ordinary BioTank requires 16 completed mobile Marines, 4 Medics with at least
5 energy, 2 Tanks (either mode), Stim and Siege. The field units must gather
within 10 tiles of one infantry member in the Ground squad and share a map
partition. Loaded Marines, scouting/drop units and base-defense units do not
count. Readiness checks refresh at most twice per second and latch after release.
A Science Vessel is not an additional first-push requirement.

Minor harassment is handled by base-defense squads. Whole-army recall compares
current enemy power with the local garrison separately for ground and air;
a critically damaged Command Center lowers the emergency threshold. The main
returning army is not counted as permanent garrison, avoiding recall/advance
oscillation. Spore Colonies and Interceptors do not create global ground danger.

Threats across cliffs/gates need a short actual ground route, unless their
weapon can already reach the town. Distance fields are cached per base.
An old rush label can no longer hold a safe, developed BioTank economy until
minute twelve. The plan's earliest-third timing still applies.

## Production

| Situation | Ordinary BioTank policy |
| --- | --- |
| Normal composition | 20% Tank supply; minimum 2 Tanks |
| Heavy ground army | 30% Tank supply |
| Air exceeds ground combat-unit count | 15% Tank supply |
| Zerglings observed | 0–4 Firebats, bounded by Marine and Zergling counts |
| Factory demand | 1 normally; 2 for a larger target; 3 only with sufficient demand, workers and bank |
| Vessel support against Zerg | roughly 1 per 16 Marines; maximum 1 / 4 / 6 on 1 / 2 / 3+ bases |
| Known Lurkers / Defilers | Vessel floor 2 / 3, subject to the base cap |
| Other races | 1 Vessel, or 2 against enemy casters/capital-air EMP threats |

The regular Vessel chain can start on two bases with a 16-Marine / 4-Medic /
2-Tank core. After the first Vessel begins training, Irradiate is funded before
additional Zerg-matchup Vessels. A second support Starport is staged after the
first two Vessels and sufficient economy; completed parallel lanes stay usable.

Known Carriers and Battlecruisers receive Goliath/Charon support. Fleet Beacon
evidence alone prepares four Goliaths. Guardians with little anti-air support
receive 2–6 Wraiths; otherwise the counter uses Goliaths. Against at least
eight known Mutalisks, Guardians or Devourers, a two-base ordinary BioTank
with 16 Marines prepares 2–4 Valkyries. Once Armory and Control Tower are
ready, the next Valkyrie is funded before routine army waves; the target does
not disappear when gas is spent or Mutalisks morph.
These are counter additions, not new selectable air strategies.

When a live capital fleet requires Goliaths, their production precedes surplus
Marines/Tanks. Near the supply cap, persistent rules, queued Marines and bank
fillers all preserve room for two Goliaths. Existing trained units are retained.

## Combat and verification

Bio clusters no longer bypass battle evaluation merely because the opponent
has an air composition or total friendly supply exceeds 180. A local simulation
result may be reused for six frames when army sizes, positions and HP remain
close; this avoids introducing a full FAP simulation every frame at high supply.

Stimmed Marines can kite nearby melee targets, with shooting windows and attack
animations preserved. Immediate Storm/Scarab escape runs before clustering, so
marching and regrouping units react too. Escape checks collision boxes and the
short movement segment rather than choosing an endpoint through a wall.

This change was inspected at source level. No build, game or replay execution
was performed. Actual timing, counter ratios, hazard avoidance and frame time
require in-game evaluation; these policies do not guarantee professional play.
