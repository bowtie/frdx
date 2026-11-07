# Forza Radio DX

![Forza Radio DX player bar in Forza Horizon 6](screenshots/in-game.webp)

Replaces the radio in Forza Horizon 6 with a curated set of online radio
stations.

## Features

- **Online radio.** Curated stations with logos and album art on the radio HUD.
- **Fast boot.** Skips the intro wait.
- **DualSense triggers.** Adaptive trigger feedback for braking and
  acceleration. Off by default.
- **Overlay.** Press **Insert** to show or hide the player bar.
- **Pixel or modern font.** Modern is for DPI scaling and might look slightly
  off.
- **No pause when you tab out.** Optional.

## Install

1. Close the game and extract the download into the folder that contains
   `forzahorizon6.exe`.
2. In the game, turn on **Settings → Audio → Streamer Mode**.
3. Start playing and pick a station.

To remove it, delete `version.dll` and the `mods/frdx` folder.

## Credits

- [g0ldyy/fh6-universal-radio](https://github.com/g0ldyy/fh6-universal-radio):
  this project started as a mod of it.
- [TigerHix/fh6_fastboot](https://github.com/TigerHix/fh6_fastboot):
  the fast boot.
- [HamzaYslmn/Forza-Horizon-DualSense-Python](https://github.com/HamzaYslmn/Forza-Horizon-DualSense-Python):
  the DualSense triggers, ported to C++.

Libraries:

- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT)
- [im-anim](https://github.com/soufianekhiat/im-anim) by Soufiane Khiat (MIT)
- [imgui-animated](https://github.com/interception-team/imgui-animated)
  (animation helpers)
- [FreeType](https://github.com/freetype/freetype) (FreeType License)
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD-2-Clause)
- [kiero](https://github.com/Rebzzel/kiero) by Rebzzel (MIT)
- [mpg123](https://www.mpg123.de) (LGPL-2.1)
- [hidapi](https://github.com/libusb/hidapi) (HIDAPI license)
- [bc7enc](https://github.com/richgel999/bc7enc) by Richard Geldreich (MIT or public domain)
- [stb](https://github.com/nothings/stb) by Sean Barrett (public domain)
- [nlohmann/json](https://github.com/nlohmann/json) (MIT)
- [toml11](https://github.com/ToruNiina/toml11) by Toru Niina (MIT)
- [Windows Implementation Libraries](https://github.com/microsoft/wil) (MIT)
- [IconFontCppHeaders](https://github.com/juliettef/IconFontCppHeaders) by Juliette Foucaut (zlib)

Fonts:

- [Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font) by TakWolf
  (pixel font, SIL OFL).
- [Geist](https://github.com/vercel/geist-font) by Vercel (modern font, SIL OFL).
- [Source Han Sans](https://github.com/adobe-fonts/source-han-sans) by Adobe
  (Japanese and Korean text in the modern font, SIL OFL).
- [Kenney Game Icons](https://kenney.nl/assets/game-icons) by Kenney (button
  icons, CC0).

## License

GPLv3, see [LICENSE](LICENSE).
