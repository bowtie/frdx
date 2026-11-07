# Forza Radio DX

![Forza Radio DX player bar in Forza Horizon 6](docs/images/in-game.webp)

Forza Radio DX swaps the radio in Forza Horizon 6 for a hand-picked list of
online radio stations. It also comes with a few small extras that make the
game nicer to play.

## What it does

**The radio**

Pick a station from the player bar and it plays in the game like any other
radio. Station logos and album art show up on the radio HUD.

**The extras**

- **Skip the intro wait.** The game no longer makes you sit through the
  startup screens.
- **DualSense triggers.** Your brake and throttle triggers push back and
  pulse with the car: braking, ABS, hitting the rev limiter and so on. This
  is off until you turn it on.
- **Overlay.** Press **Insert** to open the player bar, station list and
  settings.
- **Keep playing when you tab out.** Optionally stops the game from pausing
  when you switch windows.

## Install

1. Close the game, then extract the download into the folder that contains
   `forzahorizon6.exe`.
2. In the game, go to **Settings → Audio** and turn **Streamer Mode** on.
3. Start playing. The radio turns up on its own.

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
