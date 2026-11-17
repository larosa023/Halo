# Halo

A small, fast control panel for the **Finalmouse UltralightX**. It replaces XPANEL: a native Windows app instead of a browser tab, around 750 KB, with nothing to install.

**Status: v0.1.0.** Part of the larosa023 suite, and works with [Hangar](https://github.com/larosa023/Hangar).

## What it does

| | |
|---|---|
| **Battery** | Level, voltage and charging state. A battery ring in the tray (or the number), and a Windows alert when it's low |
| **DPI** | Any value from 50 to 26,000. Click the number to type one, use −/+ for steps of 50 (Shift for 10), or keep your own presets |
| **Tracking** | Polling rate (500 to 8000 Hz), Motion Sync, lift-off distance (1 or 2 mm) |
| **Scroll & dongle** | Tournament scroll (direction and timeout) and the dongle light (off, battery color, solid white) |
| **Details** | Signal strength, link state, mouse and dongle firmware, serial number |
| **Profiles** | Save your settings, rename them, and switch from the window or the tray menu |
| **Themes** | The suite's 14 themes, accent colors and theme files, plus **Match Hangar** |
| **Updates** | Updates itself with checksum-verified downloads, automatic rollback and a Stable or Beta channel |

Settings are saved on the mouse, so they stay in effect even when Halo isn't running.

## Run it

Download `Halo.exe` from [Releases](https://github.com/larosa023/Halo/releases) and double-click it, or install it from Hangar. It needs no admin rights. Windows may show SmartScreen the first time because the exe isn't signed yet: **More info → Run anyway**.

Plug in the UltralightX's 2.4 GHz dongle and turn the mouse on. Halo finds it by itself, including when you plug it in later. Like XPANEL, Halo talks to the mouse through the dongle. If the mouse is on its cable, the dongle can sit on the end of the cable.

- **Tray:** closing the window hides Halo in the notification area. Left-click the icon to open Halo. Right-click it for DPI, polling rate, profiles and Quit
- **Keyboard:** Ctrl+1 to Ctrl+6 switch pages, F5 reads the mouse again, +/− step the DPI, and Esc hides the window
- **Portable mode:** put an empty file named `portable` next to `Halo.exe`, and Halo keeps its settings in a `data` folder beside it
- **Try it without a mouse:** set `HALO_DEMO=1` before starting Halo, and it shows a pretend mouse

Close XPANEL before using Halo, so they don't both send commands to the mouse at once.

## Hangar

Halo is part of the suite. Hangar installs it, updates it in place while you keep your page, and themes it. See [docs/HANGAR.md](docs/HANGAR.md) for the manifest entry, the IPC protocol and the command line (`--quit`, `--uninstall`, `--restore-state`, `--tray`).

## Build

```bash
./build.sh                         # Linux/macOS, needs Zig: pip install ziglang
cmake -S . -B build -A x64 && cmake --build build --config Release   # Windows, VS 2022
g++ -std=c++17 -Isrc tests/protocol_test.cpp src/protocol.cpp -o protocol_test && ./protocol_test
```

A release is published automatically when `HALO_VERSION` in `src/common.h` changes on `main`. Versions containing a `-` are published as pre-releases (the Beta channel).

## How it works

Native C++17 with Win32 and Direct2D, the same stack as Hangar, Foyer, Strata and Breaker. A background thread owns the dongle's HID handle. It keeps a read pending at all times, sends commands in order, and polls the battery every 10 s while the window is open (every 60 s from the tray). The wire protocol is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

Halo is not made by or affiliated with Finalmouse. It never flashes firmware.
