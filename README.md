# Forza Radio DX

![Forza Radio DX player bar in Forza Horizon 6](docs/images/in-game.webp)

Forza Radio DX replaces the radio in Forza Horizon 6 with a list of online
radio stations. It also adds a few quality-of-life features.

## What it does

### Radio

Pick a station from the player bar and it plays in the game like any other
radio. Station logos and album art appear on the radio HUD.

### Extras

- **Skip the intro wait.** The game goes straight past the startup screens.
- **DualSense triggers.** The brake and throttle triggers push back and pulse
  as you brake, hit ABS or reach the rev limiter. They are off until you
  turn them on.
- **Overlay.** Press **Insert** to open the player bar, station list and
  settings.
- **No pause when you tab out.** Optional. The game keeps running and the
  radio keeps playing when you switch windows.

## Install

1. Close the game, then extract the download into the folder that contains
   `forzahorizon6.exe`.
2. In the game, go to **Settings → Audio** and turn **Streamer Mode** on.
3. Start playing. The player bar shows up on its own. Pick a station and it plays.

To remove it, delete `version.dll` and the `mods/frdx` folder, then verify
the game files.

## Credits

- [g0ldyy/fh6-universal-radio](https://github.com/g0ldyy/fh6-universal-radio):
  this project started as a mod of it.
- [TigerHix/fh6_fastboot](https://github.com/TigerHix/fh6_fastboot):
  the fast boot.
- [HamzaYslmn/Forza-Horizon-DualSense-Python](https://github.com/HamzaYslmn/Forza-Horizon-DualSense-Python):
  the DualSense triggers, ported to C++.

## License

GPLv3, see [LICENSE](LICENSE). This is an unofficial fan project and isn't
affiliated with Turn 10, Playground Games, Xbox Game Studios or Microsoft.
