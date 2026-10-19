# Rush defense and target handling — 2026-10-12

## Findings and changes

- The shared Bunker placer deliberately rejected positions close to the CC. A no-wall early ground-rush policy now selects the first Bunker within 3–6 tiles of main CC center, favors the Barracks/front side, avoids minerals, checks elevation and preserves the exit. The second uses the existing high-ground entrance shoulder search. It waits for a completed close Bunker, visible footprint, safe existing base route, attack grids and no nearby visible ground attacker. Natural/emergency/fallback requests use this contract. Assigned but unstarted forward construction rechecks safety and releases its SCV if danger arrives. An already queued forward first Bunker can be repositioned to the close shelter. Existing planned wall slots retain their placement contract.
- Base defense selected only units with weapons, leaving Medics in Ground. Defense squads now borrow one Medic per five unloaded Marines/Firebats (rounded up), selected near their actual infantry front. Support survives air/ground-only cleanup, returns when no infantry remains, and is released when defense ends. It does not count toward combat defender quotas.
- Normal MicroManager queries see enemy units only, while map Eggs can be Neutral. Explicit Zerg Egg exclusions also existed in unit-specific target filters. Nearby visible destructible neutral Eggs/Lurker Eggs are now added from a shared per-frame cache. Marching clusters execute targeting for this cleanup without marking it as an enemy battle; they do not pull nearby retreating clusters back into combat. Ground target filters accept Eggs; neutral Eggs receive low cleanup priority. Larva exclusions and enemy threat classification remain intact.
- Structure focus could outrank a nearby mobile attacker, and DestroyNeutral never collected enemies. Ranged, melee, Vulture, Goliath and Tank selectors now yield structure/neutral-egg focus to a visible, detected, attackable nearby mobile threat. The helper honors Dark Swarm restrictions and siege minimum range, skips disabled enemies and keeps existing unit-versus-unit focus rules. Neutral-building clearance uses the same response. Attack-frame guards and retreat logic remain intact.

## Verification

Source inspection only: 12 edited C++/header files passed delimiter/conflict checks; shared helper definitions/call sites and newly used local API declarations were inspected. git diff --check passed. No compilation, game execution or replay simulation was performed, as requested.

## Game scenarios still to verify

1. FastRush/HeavyRush without wall: close first Bunker; second waits through unsafe entrance, then starts on high ground after the entrance clears. Check threats arriving after an SCV has been assigned.
2. Marines defending natural in a Bio army: Medics travel with their defense squad and rejoin normal army when the raid ends. A Goliath-only defense should not retain Medics.
3. Benzene neutral Lurker Eggs: Marines, Firebats, Vultures, Goliaths and Tanks can target nearby Eggs during a march; live attacking enemies take precedence over cleanup.
4. A unit firing on an enemy/neutral building switches to a nearby shooter before low-HP retreat. A sieged Tank must not select an attacker inside its dead zone.
