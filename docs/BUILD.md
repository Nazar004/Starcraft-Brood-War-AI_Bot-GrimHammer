# Сборка GrimHammer

## Требования

- Windows; собирается **Win32 / x86**, поскольку DLL загружается в StarCraft: Brood War.
- Visual Studio с workload **Desktop development with C++**, MSVC x86/x64 и Windows SDK.
- Совместимый BWAPI SDK. Локальная сборка проекта использует SDK с `SVN_REV = 5016`; версия игрового BWAPI должна соответствовать SDK.
- PowerShell.

SDK не включён в репозиторий. Задайте путь к папке, содержащей `include/BWAPI.h`. Исходники BWAPILIB используют также `include/svnrev.h`, `include/Debug.h` и `include/starcraftver.h`: эти заголовки должны быть доступны в SDK. Не подставляйте произвольную ревизию при несовпадении SDK с игровым BWAPI.

## PowerShell

```powershell
git clone https://github.com/Nazar004/Starcraft-Brood-War-AI_Bot-GrimHammer.git
cd Starcraft-Brood-War-AI_Bot-GrimHammer
.\scripts\build.ps1 -BwapiDir "C:\BWAPI"
```

Скрипт находит MSBuild, выбирает установленный toolset и собирает BOSS, BWAPILIB и DLL бота. При необходимости задайте параметры явно:

```powershell
.\scripts\build.ps1 -BwapiDir "C:\BWAPI" -PlatformToolset v143
.\scripts\build.ps1 -BwapiDir "C:\BWAPI" -Configuration Debug
```

В проектах по умолчанию указан `v143`; скрипт может переопределить его, например на `v145`. Результаты и промежуточные файлы находятся в `build/` и исключены из Git.

| Конфигурация | Результат |
| --- | --- |
| Release / Win32 | `build/Release/Steamhammer.dll` |
| Debug / Win32 | `build/Debug/Steamhammer_d.dll` |

## Visual Studio

Задайте переменную среды `BWAPI_DIR` и откройте `Steamhammer/VisualStudio/GrimHammer.sln`. Выберите Win32, Release и установленный toolset для всех трёх проектов. После изменения переменной среды перезапустите Visual Studio.

При добавлении `.cpp` или `.h` обновляйте `GrimHammer.vcxproj` и `.vcxproj.filters`. Подключайте заголовки бота путём от `Steamhammer/Source`, например `Strategy/BioProductionPolicy.h`.

## Проверка

Сборка проверяет совместимость исходников и линковку. Для оценки микро, обороны и производства нужны игры и реплеи. Не считайте успешную сборку подтверждением победы или корректного поведения на любой карте.
