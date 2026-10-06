# Карта исходников Steamhammer / GrimHammer

Файлы физически разложены по назначению. Папки в проекте Visual Studio повторяют эту структуру.

| Папка | Что находится внутри |
| --- | --- |
| `Core/` | Запуск DLL, игровой цикл, общие типы, настройки и фоновые задачи |
| `Strategy/` | Выбор стратегии, состав армии, цели BioTank и правила масштабирования производства |
| `Production/` | Очередь строительства, JSON opening, параллельное производство, технологические зависимости и BOSS |
| `Construction/` | Места строительства, исполнение стройки, стены, бункеры и статическая оборона |
| `Combat/` | Управление армией, приказы, отряды и симуляция боя |
| `Map/` | Базы, ресурсы, проходимость, маршруты и сетки карты |
| `Economy/` | Рабочие, добыча, ремонт и распределение SCV |
| `Intelligence/` | Разведка, сведения о юнитах и план противника; `Learning/` — история игр; `Skills/` — обучаемые навыки |
| `Utilities/` | Логи, таймеры, JSON, разбор текста и общие функции для юнитов |
| `ThirdParty/` | Сторонняя библиотека RapidJSON, отделённая от кода бота |

## Где менять конкретное поведение

- **Медики:** [MicroMedics.cpp](Combat/Micro/Terran/MicroMedics.cpp), [команды лечения](Combat/Micro/Micro.cpp).
- **Марины и защита стены:** [MicroMarines.h](Combat/Micro/Terran/MicroMarines.h), [MicroRanged.cpp](Combat/Micro/MicroRanged.cpp), [CombatCommander.cpp](Combat/CombatCommander.cpp).
- **Танки, фронт био и строй армии:** [MicroTanks.cpp](Combat/Micro/Terran/MicroTanks.cpp), [BioInfantryFront.h](Combat/Micro/Terran/BioInfantryFront.h), [TerranFormation.h](Combat/Micro/Terran/TerranFormation.h).
- **Бункеры и проход у чока:** [BuildingPlacer.cpp](Construction/BuildingPlacer.cpp), [BunkerPlacementPolicy.h](Construction/BunkerPlacementPolicy.h), [StaticDefense.cpp](Construction/StaticDefense.cpp).
- **Избыток ресурсов и состав BioTank:** [StrategyManager.cpp](Strategy/StrategyManager.cpp), [BioProductionPolicy.h](Strategy/BioProductionPolicy.h), [ProductionManager.cpp](Production/ProductionManager.cpp).
- **Отряды и отступление:** [Squad.cpp](Combat/Squads/Squad.cpp), [OpsBoss.cpp](Combat/OpsBoss.cpp).
- **Воздушный бой, включая валькирий:** [MicroAirToAir.cpp](Combat/Micro/MicroAirToAir.cpp); цели старпортов — в `Strategy/StrategyManager.cpp`.

## Поддерживаемая раса

Бот играет только за **Terran**. Собственные стратегии и контроллеры протоссов/зергов удалены. При выборе другой расы менеджеры и игровые callbacks не запускаются; бот выводит сообщение о необходимости выбрать Terran.
Разведка, модель противника, симуляция боя и защита стены продолжают учитывать все расы противников. Из общих функций кайтинга сохранена `CooldownDanceTarget` для вультур и врайтов.
JSON-профили с `Race` не Terran пропускаются до разбора их build order; каталоги действий содержат только юниты и улучшения террана.

## Правила структуры

Держать `.cpp` и соответствующий `.h` рядом. Общие контроллеры микро находятся в `Combat/Micro/`, террановские — в `Combat/Micro/Terran/`.
Собственные заголовки подключать путями от `Source`, например `#include "Strategy/BioProductionPolicy.h"`. Корень `Source` уже входит в пути поиска заголовков проекта.
Внутреннюю структуру и подключения RapidJSON не менять. Заголовки BOSS подключаются относительно файла; сама библиотека остаётся за пределами `Source`.

При добавлении файла обновлять `../VisualStudio/GrimHammer.vcxproj` и `.vcxproj.filters`. Существующий `Map/Grids/GridAirDistances.cpp` сохранён как некомпилируемый элемент: он не входил в сборку до переноса.
JSON со стратегиями находится вне `Source`, в `bwapi-data/AI/GrimHammer.json` установленной игры.
