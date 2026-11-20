# Halo and Hangar

Halo is part of the larosa023 suite. [Hangar](https://github.com/larosa023/Hangar) installs it, updates it, themes it and backs it up. Halo also works without Hangar.

## The manifest entry

This entry in Hangar's `manifest.json` puts Halo in Hangar's library, under Gaming:

```json
{
  "id": "halo",
  "name": "Halo",
  "repo": "larosa023/Halo",
  "description": "Battery, DPI and polling rate for the Finalmouse UltralightX.",
  "category": "gaming",
  "icon": "https://raw.githubusercontent.com/larosa023/Halo/main/res/halo.png",
  "exe": "HaloMouse.exe",
  "install": {
    "type": "portable",
    "asset": "HaloMouse.exe",
    "checksums": "SHA256SUMS.txt",
    "dir": "%LOCALAPPDATA%\\Programs\\Halo"
  },
  "args": { "quit": "--quit", "uninstall": "--uninstall" },
  "liveUpdate": true
}
```

Every release publishes `HaloMouse.exe`, `Halo-win-x64.zip` and `SHA256SUMS.txt`. The tag is `v<version>`, and versions with a `-` are pre-releases (Hangar's Beta channel).

## Installing the same way

Hangar copies `HaloMouse.exe` to `%LOCALAPPDATA%\Programs\Halo` and adds a Start menu shortcut and an Apps & features entry. **Settings → Hangar → Install** in Halo does exactly the same. So a copy installed either way is the same install, and Hangar finds it.

When Halo runs from that folder, it keeps the Apps & features entry's version current after it updates itself.

## Command line

| | |
|---|---|
| `--quit` | Asks the running Halo to close the normal way and waits up to 8 s for it. Hangar runs this before an update |
| `--uninstall` | Asks first, closes Halo, and removes the shortcut, the Apps & features entry, the startup entry and the exe. It can also remove the settings |
| `--restore-state <file>` | Starts where the previous version left off: the same page and scroll position, or hidden in the tray |
| `--tray` | Starts hidden in the tray. This is the "Start with Windows" entry |

## Live updates (Hangar IPC protocol v1)

Halo implements the app side of Hangar's [`docs/IPC.md`](https://github.com/larosa023/Hangar/blob/main/docs/IPC.md):

- **Pipe:** `\\.\pipe\larosa023.halo.<sessionId>`, one JSON object per line, current user only, remote clients rejected
- **Discovery file:** `%LOCALAPPDATA%\Hangar\run\halo.json` holds `{pid, pipe, version, protocol: 1, exe}`. Halo removes it when it exits
- **Health:** about 2 s after it starts, Halo sends `{"type":"health","app":"halo","version":…,"pid":…}` to `\\.\pipe\larosa023.hangar.<sessionId>`. After a `--restore-state` start it also sends `restored` with a summary such as "opened on Performance"

| Request | Halo's reply |
|---|---|
| `hello` | `{protocol:1, app:"halo", version, capabilities:["state","theme","settings"]}` |
| `status` | `{state:"idle"}` |
| `prepare-update` | saves the page, scroll and window state to `%LOCALAPPDATA%\Halo\state\hangar-restore.json` and replies `{ok:true, stateFile}`. Refuses while you're typing in a field |
| `restart` | closes the pipe and exits |
| `cancel-update` | `{ok:true}` |
| `apply-theme` | applies a theme in the shared format right away when Halo is set to **Match Hangar**. Replies `{ok:true, applied}` |
| `apply-settings` | reloads `settings.json`: `{ok:true, restartRequired:false}` |
| `export-settings` | writes `%LOCALAPPDATA%\Halo\export\halo-settings.json` and replies `{file}` |

`tests/ipc_probe.cpp` is a small client that sends these requests the way Hangar will.

## Themes

- **Match Hangar** is the default theme. Until Hangar pushes a theme over IPC, Halo reads Hangar's choice from `%LOCALAPPDATA%\Hangar\settings.json` and checks it again whenever you switch back to Halo
- Theme files in Hangar's `themes` folder show up in Halo too, marked "Suite", next to Halo's own
- The built-in themes, the accent colors and the theme-file format are the same as Hangar's (`src/themes.cpp`)

## Updates without Hangar

Halo updates itself with the same design as Hangar's own updater:

- It reads Halo's releases from Hangar's release index (`releases.json` on Hangar's `index` branch), which avoids GitHub's API limit. It falls back to the GitHub API
- It checks the download's SHA-256 against both GitHub's digest and `SHA256SUMS.txt`
- It swaps the exe and waits for a "healthy" signal. If the new version doesn't send one, Halo restores the previous version and skips the new one
- It keeps the last 2 versions for **Settings → Updates → Roll back**
