# Source guide

GrimHammer's source code is organized by subsystem. Visual Studio project folders follow the same layout.

| Directory | Responsibility |
| --- | --- |
| `Core/` | DLL entry points, game loop, shared types, configuration, and tasks |
| `Strategy/` | Strategy selection, army composition, and production goals |
| `Production/` | Build orders, production queues, technology requirements, and BOSS integration |
| `Construction/` | Building placement, construction tasks, and static defense |
| `Combat/` | Army management, squad orders, combat simulation, and unit control |
| `Map/` | Bases, resources, pathfinding, and map grids |
| `Economy/` | Worker management, mining, and repairs |
| `Intelligence/` | Scouting, enemy information, opponent models, and game history |
| `Utilities/` | Logging, timers, JSON parsing, and shared helpers |
| `ThirdParty/` | Bundled RapidJSON headers |

## Entry points

- [UAlbertaBotModule.cpp](Core/UAlbertaBotModule.cpp) — BWAPI callbacks and bot lifecycle.
- [StrategyManager.cpp](Strategy/StrategyManager.cpp) — strategy selection and army goals.
- [ProductionManager.cpp](Production/ProductionManager.cpp) — production scheduling.
- [BuildingPlacer.cpp](Construction/BuildingPlacer.cpp) — building placement.
- [CombatCommander.cpp](Combat/CombatCommander.cpp) — army and defense coordination.
- [WorkerManager.cpp](Economy/WorkerManager.cpp) — worker control.
- [InformationManager.cpp](Intelligence/InformationManager.cpp) — enemy information.

## Organization

Keep implementation files and their headers together. Shared unit controllers belong in `Combat/Micro/`; Terran-specific controllers belong in `Combat/Micro/Terran/`. Squads are defined in `Combat/Squads/`.

Include bot headers using paths relative to `Source`, for example `#include "Core/Config.h"`. Register source files in [GrimHammer.vcxproj](../VisualStudio/GrimHammer.vcxproj) and [its filters](../VisualStudio/GrimHammer.vcxproj.filters).

Bot settings and opening build orders are stored in [config/GrimHammer.json](../../config/GrimHammer.json). BOSS and BWAPILIB are located at the repository root.
