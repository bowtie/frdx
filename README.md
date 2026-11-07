# Forza Radio DX

![Forza Radio DX player bar in Forza Horizon 6](docs/images/in-game.webp)

A free, open-source mod for **Forza Horizon 6** that adds a new radio station
streaming live internet radio into the game's own audio engine, skips the
startup hold, and drives DualSense adaptive triggers.

## Features

- **Fast boot** – skips the ~25 s black-screen hold at startup (and can mute
  the intro so the cut isn't abrupt).
- **Online radio** – streams Shoutcast/Icecast stations straight into the
  game's audio engine, so it ducks for menus, follows the in-game volume
  slider and fades on loading screens. Station logos and per-track album art
  appear on the radio HUD, with a station selector in the overlay.
- **DualSense triggers** – adaptive trigger feedback driven by Forza's Data
  Out telemetry: brake and throttle resistance, ABS pulses, rev-limiter
  buzz, wheelspin, gear-shift kicks and more. Off by default.
- **In-game overlay** – a player bar, station selector and settings panel
  with a frosted-glass backdrop blur, themes and pixel or modern fonts.
  Opens with **Insert**.
- **No pause on alt-tab** – optionally stops the game from pausing when you
  tab out, so the radio keeps playing.

## Install

1. Close Forza Horizon 6.
2. Extract the release archive into the folder that contains
   `forzahorizon6.exe`:
   - Steam: `...\steamapps\common\ForzaHorizon6`
   - Xbox app: `...\XboxGames\Forza Horizon 6\Content`
3. In the game, go to **Settings → Audio** and turn **Streamer Mode: ON**.
4. Start playing. The mod attaches the radio automatically, defaulting to
   Box Radio – Japan City Pop.

Some antivirus tools flag the bundled `version.dll` because of how the mod
hooks the game. If yours removes it, add the game folder to its exclusions
and extract again.

## Configure

Everything is editable from the overlay. Advanced options live in
`<game>/mods/frdx/config.toml` (see [`config.example.toml`](config.example.toml));
stations are listed in `mods/frdx/stations.json`
(see [`stations.example.json`](stations.example.json)).

For DualSense triggers, enable them in the overlay and point Forza's
**Data Out** at `127.0.0.1`, UDP port `5300`.

## Uninstall

Delete `version.dll` and the `mods/frdx/` folder from the game folder, then
verify/repair the game files (Steam: *Verify integrity of game files*; Xbox
app: *Repair*).

## Build from source

Windows, clang-cl, CMake and Ninja are required.

```powershell
.\scripts\get-deps.ps1   # fetch third-party dependencies
.\scripts\build.ps1      # build and stage into dist\
.\scripts\install.ps1 -GameDir "C:\path\to\Forza Horizon 6"
```

## Credits

- [g0ldyy/fh6-universal-radio](https://github.com/g0ldyy/fh6-universal-radio) –
  this project started as a mod of it.
- [TigerHix/fh6_fastboot](https://github.com/TigerHix/fh6_fastboot) – the
  original fast boot idea and implementation.
- [HamzaYslmn/Forza-Horizon-DualSense-Python](https://github.com/HamzaYslmn/Forza-Horizon-DualSense-Python) –
  the DualSense trigger logic was ported to C++ from it.

## License

GPLv3, see [LICENSE](LICENSE). Unofficial fan project, not affiliated with or
endorsed by Turn 10 Studios, Playground Games, Xbox Game Studios or Microsoft.
Provided as-is, with no warranty.
