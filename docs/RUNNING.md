# Running GrimHammer

Use StarCraft: Brood War **1.16.1** with a BWAPI installation compatible with the SDK used to build the bot.

1. [Build](BUILD.md) the Release DLL.
2. Copy `build/Release/Steamhammer.dll` to `bwapi-data/AI/Steamhammer.dll` in your game directory.
3. Copy [GrimHammer.json](../config/GrimHammer.json) to `bwapi-data/AI/GrimHammer.json`.
4. Set the `ai` option in `bwapi-data/bwapi.ini` to `bwapi-data/AI/Steamhammer.dll`.
5. Select **Terran** and launch the game through your BWAPI launcher or tournament runner.

## Configuration

`GrimHammer.json` contains bot settings and opening build orders. The `BWAPI` and `Debug` sections control game speed and on-screen information.

Paths in the `IO` section are relative to the game directory. Create the configured data directories, including `bwapi-data/read/`, `bwapi-data/write/`, and `bwapi-data/AI/om/`, and ensure they are writable.

For local experiments, keep configuration copies as `*.local.json`. Install your selected configuration as `bwapi-data/AI/GrimHammer.json` before running the bot.

GrimHammer plays Terran and can face opponents of any race. For general BWAPI setup information, see the [BWAPI documentation](https://bwapi.github.io/).
