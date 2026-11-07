# Forza Radio DX

![Forza Radio DX player bar in Forza Horizon 6](docs/images/in-game.webp)

A mod for **Forza Horizon 6** that replaces the in-game radio with a curated
set of online radio stations, plus a few quality-of-life extras.

## Features

- **Online radio** – curated stations streamed into the game's own audio
  engine, with station logos and album art on the HUD.

Quality of life:

- **Fast boot** – skip the wait on the intro screens at startup.
- **DualSense triggers** – responsive adaptive triggers for braking, throttle,
  ABS, rev limiter and more (off by default).
- **Overlay** – player bar, station selector and settings. Press **Insert**.
- **No pause on alt-tab** – optional.

## Install

1. Close the game and extract the release into the folder containing
   `forzahorizon6.exe`.
2. In-game, turn **Settings → Audio → Streamer Mode** on.
3. Start playing; the radio attaches automatically.

To uninstall, delete `version.dll` and `mods/frdx/`, then verify the game files.

## Credits

- [g0ldyy/fh6-universal-radio](https://github.com/g0ldyy/fh6-universal-radio) –
  this project started as a mod of it.
- [TigerHix/fh6_fastboot](https://github.com/TigerHix/fh6_fastboot) – fast boot.
- [HamzaYslmn/Forza-Horizon-DualSense-Python](https://github.com/HamzaYslmn/Forza-Horizon-DualSense-Python) –
  DualSense trigger logic, ported to C++.

## License

GPLv3, see [LICENSE](LICENSE). Unofficial fan project, not affiliated with
Turn 10, Playground Games, Xbox Game Studios or Microsoft.
