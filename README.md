<p align="center"><img src="docs/assets/banner.svg" alt="GrimHammer — Terran command system" width="100%"></p>

<p align="center">
<img src="https://img.shields.io/badge/language-C%2B%2B17-4c8bbf" alt="C++17">
<img src="https://img.shields.io/badge/race-Terran-e3a446" alt="Terran">
<img src="https://img.shields.io/badge/platform-Windows%20x86-596879" alt="Windows x86">
<img src="https://img.shields.io/badge/status-in%20development-727b87" alt="In development">
</p>

# GrimHammer

**Terran AI для StarCraft: Brood War на C++ и BWAPI.** Проект развивает Steamhammer / UAlbertaBot: выбирает стратегию, управляет экономикой и производством, строит оборону и контролирует армию.

Основное направление — BioTank: марины и медики при поддержке осадных танков. Бот играет за Terran и сохраняет анализ противников всех рас.

## Что умеет бот

- **BioTank и мех:** открытия из JSON, выбор состава армии и адаптация к плану противника.
- **Производство:** параллельный выпуск армии во время технологического развития, расширение производственных мощностей при избытке ресурсов.
- **Микроконтроль:** фронт био для сопровождения танками и медиками, лечение раненых, управление осадой и кайтинговое движение.
- **Оборона:** стены, защита маринами, бункеры у входа с проверкой свободного прохода.
- **Воздушная поддержка:** валькирии против воздушных угроз и масштабирование старпортов.
- **Разведка и история:** модель противника, распознавание плана и сохранение данных игр.

Проект активно меняется. Наличие этих механизмов не означает, что их поведение уже идеально на всех картах.

## Начать работу

1. Установить Visual Studio с компонентами C++ для x86 и Windows SDK, а также совместимый BWAPI SDK.
2. Собрать DLL по [инструкции](docs/BUILD.md).
3. Скопировать `config/GrimHammer.json` в `bwapi-data/AI/GrimHammer.json` установленной игры.
4. Подключить DLL в BWAPI и выбрать расу **Terran** — [подробности запуска](docs/RUNNING.md).

```powershell
.\scripts\build.ps1 -BwapiDir "C:\BWAPI"
```

Результат Release-сборки: `build/Release/Steamhammer.dll`. Имя DLL сохранено для совместимости с текущим подключением бота.

## Навигация по проекту

| Путь | Назначение |
| --- | --- |
| [`Steamhammer/Source/`](Steamhammer/Source/README.md) | Исходники бота, разложенные по подсистемам |
| [`config/GrimHammer.json`](config/GrimHammer.json) | Настройки, стратегии и build orders |
| [`Steamhammer/VisualStudio/`](Steamhammer/VisualStudio/) | Solution и проект бота |
| [`BOSS/`](BOSS/) | Библиотека поиска build order |
| [`BWAPILIB/`](BWAPILIB/) | Исходники библиотеки интерфейса BWAPI |
| [`scripts/`](scripts/) | Сборка из PowerShell |
| [`docs/`](docs/) | Сборка, запуск и правила разработки |
| [`licenses/`](licenses/) | Лицензии и уведомления зависимостей |

Для изменения поведения начните с [карты исходников](Steamhammer/Source/README.md). Для нового бага полезны реплей, карта, игровой кадр и название выбранной стратегии.

## Разработка

Перед отправкой изменений выполните Release-сборку. Изменения микро и стратегии проверяйте также в игре: компиляция не подтверждает качество боя. [Правила разработки](CONTRIBUTING.md).

Исходники подготовлены без локальных архивов, результатов сборки, реплеев и накопленной истории противников. Игра и установленный BWAPI SDK поставляются отдельно.

## Авторы и зависимости

GrimHammer — **Nazar Biriukov**. Основа: **Steamhammer — Jay J. P. Scott**, **UAlbertaBot / BOSS — David Churchill**. Симуляция боя использует FAP; JSON — RapidJSON; игровой интерфейс — BWAPI.

Лицензии исходной основы сохранены. У зависимостей собственные условия: см. [уведомления](THIRD_PARTY_NOTICES.md) и [LICENSE](LICENSE).
