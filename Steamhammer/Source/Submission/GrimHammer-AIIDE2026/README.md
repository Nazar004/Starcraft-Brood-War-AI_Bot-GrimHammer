# GrimHammer — AIIDE 2026 submission

Bot: GrimHammer (Terran)  
Author: Nazar Biriukov  
Binary: `Steamhammer.dll` (Win32/x86, BWAPI 4.4.0)

## Install and run

Copy `Steamhammer.dll` and `GrimHammer.json` into the StarCraft 1.16.1
`bwapi-data/AI/` directory. Configure the tournament manager/BWAPI to load
`bwapi-data/AI/Steamhammer.dll`. The bot loads its JSON from the adjacent
`bwapi-data/AI/GrimHammer.json` path. The competition provides
`bwapi-data/read/` and `bwapi-data/write/` for cumulative per-opponent learning.
No local opponent records are included. The JSON disables complete map
information and user input, and sets local speed to 0.

## Build from source

The complete bot source and BOSS library source are in `source/`. No BWAPI
headers, DLLs, source, or libraries are bundled. Install the official
[BWAPI 4.4.0 release](https://github.com/bwapi/bwapi/releases/tag/v4.4.0)
with `include/BWAPI.h` and `BWAPILIB/BWAPILIB.vcxproj`. Set `BWAPI_DIR`
to that release's root directory. The official BWAPI 4.4.0 release does
not ship a prebuilt `BWAPILIB.lib`; the included build script first compiles
the official SDK project into `source/build/Release/BWAPILIB.lib`. Install Visual Studio with Desktop development
with C++, MSVC x86 tools and a Windows SDK. Run from `source/`:

```powershell
.\scripts\build.ps1 -BwapiDir "C:\BWAPI" -Configuration Release
```

The script detects the installed MSVC platform toolset, builds the
official SDK's BWAPILIB project, then BOSS and GrimHammer. Output:
`source/build/Release/Steamhammer.dll`. For a manual Release/Win32 build,
first build the official BWAPILIB project with its output set to
`source/build/Release/`, then open
`source/Steamhammer/VisualStudio/GrimHammer.sln`. `BWAPI_DIR` is the only external SDK path; there are no hard-coded
paths to the author's computer. BOSS and RapidJSON source needed for the bot
are included. No Boost, SDL or Java runtime is required for this DLL.

The submitted DLL was copied from the latest local release build. The
submission-specific build script compiles the official BWAPI 4.4.0
BWAPILIB project and the source solution links its resulting library. This source packaging has not been rebuilt or
run in a clean Windows 10 environment; verify before final submission.

## Notes

`GrimHammer.json` is the current ladder strategy configuration with all
matchup-appropriate openings enabled and per-opponent learning on. The
on-screen debug drawings are disabled only in this submission copy to
reduce per-frame time. Startup configuration is otherwise unchanged.
The bot is based on Steamhammer and UAlbertaBot. Licenses are in
`source/licenses/`; attribution is in `source/THIRD_PARTY_NOTICES.md`.
