# Building GrimHammer

## Requirements

- Windows and Visual Studio with **Desktop development with C++**, MSVC, and a Windows SDK.
- A compatible BWAPI SDK with `include/BWAPI.h`.
- PowerShell.

Build for **Win32 / x86**. The bot DLL runs inside StarCraft: Brood War.

BWAPILIB also requires `include/svnrev.h`, `include/Debug.h`, and `include/starcraftver.h`. The SDK revision must match the BWAPI installation used to run the bot. The supported SDK configuration uses `SVN_REV = 5016`.

## PowerShell

```powershell
git clone https://github.com/Nazar004/Starcraft-Brood-War-AI_Bot-GrimHammer.git
cd Starcraft-Brood-War-AI_Bot-GrimHammer
.\scripts\build.ps1 -BwapiDir "C:\BWAPI"
```

The script locates MSBuild, selects an installed toolset, and builds BOSS, BWAPILIB, and the bot. You can also specify the toolset or configuration:

```powershell
.\scripts\build.ps1 -BwapiDir "C:\BWAPI" -PlatformToolset v143
.\scripts\build.ps1 -BwapiDir "C:\BWAPI" -Configuration Debug
```

| Configuration | Output |
| --- | --- |
| Release / Win32 | `build/Release/Steamhammer.dll` |
| Debug / Win32 | `build/Debug/Steamhammer_d.dll` |

Build output is stored in `build/` and excluded from Git.

## Visual Studio

Set the `BWAPI_DIR` environment variable to your SDK directory, then open `Steamhammer/VisualStudio/GrimHammer.sln`. Select **Release / Win32** and an installed toolset for all three projects. The project files default to `v143`.

Restart Visual Studio after changing environment variables. See the [setup guide](RUNNING.md) to install the resulting DLL.
