# Tanks: Aztec, 2026-10-08 17:49

Replay: `20261008_174925_917__GrimHammer__Aztec1.1__loss.rep`.
Opponent: dragon, Terran. Duration: 21908 frames. Times below use the bot's 24-frame second.

## Confirmed commands

| Command | Frame | Approximate time | Tile |
|---|---:|---:|---|
| Factory 1 | 4358 | 3:01 | 12,87 |
| Factory 2 | 5196 | 3:36 | 13,79 |
| Natural Command Center | 6600 | 4:35 | 6,56 |
| Third Command Center | 19404 | 13:28 | 6,30 |

No further Factory build commands occur in this replay. The two listed Command Center orders are in addition to the initial main. A build command proves an order was issued, not that construction completed. Training continued: 16 Tank, 12 Goliath, 7 Vulture, 16 Marine and 46 SCV training commands in total.

The reported 7000 minerals / 2000 gas are the user's observation; this command-only replay analysis does not independently measure the bank or reconstruct unit deaths.

## Source findings and fixes

1. `manageTerranRuleProduction()` prevented Factory infrastructure behind every structural/research queue entry. Economy had a parallel exception; Factory scaling did not. Tanks now receives the same independent construction path for an additional Factory once a completed Factory exists. The strategy still sets the target/cap. Full queued-item and construction reservations remain protected; an existing queued Factory or unstarted Factory prevents duplicate dispatch, and placement must succeed.
2. `manageBuildOrderQueue()` breaks immediately after `predictWorkerMovement()`. Consequently, failure to find a building tile never reaches the global jam watchdog on those prediction attempts. Supplementary army/SCV production also resets the global timer. Added a separate 20-second timer for unplaceable Terran macro buildings, reset on valid placement, changed prediction and initialization. On expiry only the blocked item is removed and strategic rules are refreshed; pending add-on goals and BuildingManager work remain intact. The JSON opening Bunker recovery is preserved.
3. `shouldExpandNow()` rejected a third through minute 11 for remembered opening threats before checking saturation or a mineral surplus. Tanks may now bypass that remembered-threat veto with >=1800 minerals, >=24 SCVs, >=2 completed Command Centers and no visible ground/air combat threat near owned bases. The plan's earliest-third timing and MapTools site/path/enemy checks remain required.
4. Tanks HUD now shows Factory ready/all/target/queued/unstarted and Command Center ready/all/queued/unstarted to distinguish missing goals from queue or construction blockage in subsequent games.

The replay does not contain the internal production queue or BuildingManager task state. These are verified source-level blocking paths consistent with the symptoms, not proof of which exact queue entry blocked this particular game. The delayed third also had a deliberate strategy timing restriction, unlike Factory growth.

## Validation

- Confirmed construction frames directly from the decoded command stream.
- Existing TvZ bio static suite passed: 5184 readiness states, 12 transition boundaries, opening dependencies/supply and preserved configuration.
- Existing wall clearance static suite passed: 5 occupancy scenarios, 17343 geometry cases, command ownership and reset/release guards.
- Checked field initialization/reset and preserved UTF-8/CRLF.
- `git diff --check` passed for the three edited source files.
- No compilation, build or game simulation was run, as requested.

The temporary decoder executable and full decoded replay JSON were removed after analysis; compact command evidence remains beside this report.
