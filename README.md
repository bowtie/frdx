# Forza Radio DX

![Forza Radio DX player bar in Forza Horizon 6](screenshots/in-game.webp)

Forza Radio DX replaces the radio in Forza Horizon 6 with a list of online
radio stations. It also adds a few quality-of-life features.

## What it does

### Radio

Pick a station from the player bar. It plays through the game's audio, so the
in-game volume applies. The radio HUD shows station logos and album art. In
the settings you can choose logos only, logos plus album art, or no artwork.

### Extras

- **Skip the intro wait.** The game goes straight past the startup screens.
- **DualSense triggers.** The brake and throttle triggers push back and pulse
  as you brake, hit ABS or reach the rev limiter. They are off until you
  turn them on.
- **Overlay.** Press **Insert** to show or hide the player bar. The station
  list and settings are on the bar.
- **Pixel or modern font.** Pixel is the default. Modern is for DPI scaling
  and higher resolutions, and might look slightly off.
- **No pause when you tab out.** Optional. The game keeps running and the
  radio keeps playing when you switch windows.

## Install

1. Close the game, then extract the download into the folder that contains
   `forzahorizon6.exe`.
2. In the game, go to **Settings → Audio** and turn **Streamer Mode** on.
3. Start playing. The player bar appears on its own. Pick a station.

To remove it, delete `version.dll` and the `mods/frdx` folder. The mod doesn't
change any other game files.

## Credits

- [g0ldyy/fh6-universal-radio](https://github.com/g0ldyy/fh6-universal-radio):
  this project started as a mod of it.
- [TigerHix/fh6_fastboot](https://github.com/TigerHix/fh6_fastboot):
  the fast boot.
- [HamzaYslmn/Forza-Horizon-DualSense-Python](https://github.com/HamzaYslmn/Forza-Horizon-DualSense-Python):
  the DualSense triggers, ported to C++.

Fonts:

- [Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font) by TakWolf
  (pixel font, SIL OFL).
- [Geist](https://github.com/vercel/geist-font) by Vercel (modern font, SIL OFL).
- [Source Han Sans](https://github.com/adobe-fonts/source-han-sans) by Adobe
  (Japanese and Korean text in the modern font, SIL OFL).
- [Kenney Game Icons](https://kenney.nl/assets/game-icons) by Kenney (button
  icons, CC0).

## License

GPLv3, see [LICENSE](LICENSE). This is an unofficial fan project and isn't
affiliated with Turn 10, Playground Games, Xbox Game Studios or Microsoft.
