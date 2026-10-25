# No Pause When Losing Focus for FH2

Keeps **Forza Horizon 2** (Xbox One build on PC via the Azimuth launcher) running when its window loses focus, so you can alt-tab, click another monitor or use a second window without the game pausing. Built on the **FH2 Mod Platform**.

## How it works

The game could be checking focus in more than one way, so the mod covers both:

- It subclasses the game window and swallows the "you lost focus" messages (`WM_ACTIVATEAPP`, `WM_ACTIVATE`, `WM_KILLFOCUS`).
- It hooks `GetForegroundWindow`, `GetActiveWindow` and `GetFocus` so they answer with the game window while the setting is on.

Azimuth reads the controller through `Windows.Gaming.Input`, which stops delivering input to a window that isn't in the foreground. So the mod also hooks `IGamepad::GetCurrentReading` and, while the game is unfocused, fills the reading from XInput (which isn't gated). It installs once a controller is connected.

A background thread re-checks every 1.5 seconds for the game window (the largest visible top-level window of the process), so it keeps working through loading screens and mode switches.

## Settings

| Setting | Default | What it does |
|---|---|---|
| Keep Running When Unfocused | On | Stops the game pausing when you alt-tab or click another window. |

Change it on **Pause > Settings > MODS**, or in `fh2-mods\FH2NoPause.ini`:

```ini
[Settings]
KeepRunning=1
```

Set `KeepRunning=0` to get the normal pause-on-focus-loss behaviour back.

## Installation

Requires the [FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform) (`d3dcompiler_47.dll` in your Azimuth folder), which most FH2 mods already install.

1. Download `FH2NoPause-<version>.zip` from the **Releases** page and unzip it.
2. Close the game. Copy the zip's **`azimuth-windows\`** folder into your Azimuth folder (the one with `azimuth.exe`) and say yes to merging.
3. Start the game through Azimuth. `fh2-mods\platform.log` should show `loaded FH2NoPause.dll` and `NoPause ready (on)`.

## Uninstalling

Close the game and delete `fh2-mods\FH2NoPause.dll` (and `fh2-mods\FH2NoPause.ini` if present) from your Azimuth folder. To switch it off without uninstalling, turn the setting off as described above.

## Building from source

Requires **Visual Studio 2022** (MSVC C++20) and **CMake 3.20+**.

```powershell
git clone --recurse-submodules https://github.com/qiangqiang101/FH2NoPauseWhenLosingFocus.git
cd FH2NoPauseWhenLosingFocus
cmake -B build -A x64
cmake --build build --config Release --target FH2NoPause
```

The result is `build\Release\FH2NoPause.dll`. If you cloned without `--recurse-submodules`, run `git submodule update --init`.

