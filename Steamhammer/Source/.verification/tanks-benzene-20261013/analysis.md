# Benzene1.1, GrimHammer vs WillyT, 2026-10-13

The screenshot was taken at frame 31033, but its stored CPU peak is frame
24640: 149.1 ms total, 111.3 ms Combat, 104.8 ms Ground squad, and 91.1 ms
Tanks. These are measurements from one earlier frame, not the screenshot's
current army state. The replay is 33550 frames long.

The replay command stream around frames 24520–24760 contains 439 own Tank
Move orders across 30 Tank tags identified from Siege/Unsiege commands.
Frequently ordered destinations are tiles (15,56), (14,58), and (15,57),
near the left-side owned bases. These are orders, not a reliable count of
living Tanks. The replay does not contain the bot's call stack or CPU timers.
See `replay-evidence.json` for the compact counts.

Code paths explaining the stall:

- TvT_Anti2Factory specifies 10 Tanks for release, while urgent production
  restored a generic 15 on each call. The plan's intended release size could
  be lost before the attack decision.
- The ordinary Tanks attack decision sat below many urgent production early
  returns. A bunker/technology/queue wait could leave the Ground squad on an
  old defensive order even with a mature army.
- The wall Bunker hold returned before normal release evaluation. A large
  army could stay in opening defense until a separate time fallback fired.
- Tank base-defense classified passing enemies within 24 tiles of any owned
  depot as a local attack. Multiple Tanks or Vultures near a base could recall
  most of the army. The new detector uses 12 tiles plus ground reachability,
  while real fire at a base still qualifies from farther away.

Code paths capable of the Tank CPU spike:

- Defensive and gather formation caches invalidated their entire slot plans
  when the Tank set changed. In a 30-Tank army, one new Tank or loss could
  rerun many terrain, occupancy and path checks in the same frame.
- Base-threat discovery and responder selection repeated for each spatial
  Tank cluster; the screenshot recorded 32 Ground clusters at the peak.
- Occupancy checks queried BWAPI's nearby-unit index for every candidate
  Tank parking point.

The slot plans now preserve surviving assignments, base observations are
cached per frame, and Tank positions are sampled once per frame for parking
checks. The HUD now records defense/gather plan rebuild time and count on the
stored peak frame. Those numbers will distinguish formation cost from the
remaining Tank micro cost in the next game. No build or game run was made.
