# ReCompiled Steel 0.3.0

## What's new

- **Fighters from Pacific Rim.** If you also own Yuke's Pacific Rim (Xbox
  360), **Import fighters** (in the setup program, or the Android launcher)
  brings its fighters into the game from your own copy. Nothing from Pacific
  Rim comes with this project.
- **Frame rate: 30, 60, 90 or 120** (Settings > Display). The game keeps its
  normal speed in every mode. 90 and 120 are offered when the display
  refreshes that fast; online matches play at 60, like the original. 60 stays
  the default.
- **Texture packs** (new **Enhancements** tab, everything off by default).
  **Export textures** saves every texture the game shows as a PNG;
  **Texture pack** loads replacements for them, at any size, from a
  `texture_pack` folder next to the game. On Windows, `make_texture_pack.ps1`
  (in the download) makes a pack from your exported textures with
  Real-ESRGAN, which you download yourself; it leaves out normal maps and
  other textures that don't upscale well. 2x suits handhelds, 4x a strong PC
  graphics card. No packs come with the project: they are made from your own
  copy of the game.
- **Android: touch controls in colour.** Arrange touch controls now also
  sets each button's colour and how see-through it is.
- **Android: custom graphics drivers that come with extra libraries** (some
  Qualcomm driver packages) now load: the launcher keeps the other libraries
  of the zip beside the driver.

## Fixed

- The thin line under the buttons of the game's dialogs.
- The pre-fight title of add-on stages 4 to 9 (it showed stage 3's).
- Off the known-issues list: the leaderboards' **MY SCORE** page works, and
  the full-screen fades at 4:3 and 16:10 cover the whole picture.

## Downloads

| File | For |
| --- | --- |
| `ReCompiledSteel-windows-x64-0.3.0.zip` | Windows 10 / 11, 64-bit: run RealSteelSetup.exe, then realsteel.exe |
| `ReCompiledSteel-linux-x64-0.3.0.tar.gz` | Linux and Steam Deck: run realsteel-setup, then start.sh |
| `ReCompiledSteel-android-arm64-0.3.0.apk` | Android, arm64: install, then **Install game files** in the app |
| `ReCompiledSteel-mods-0.3.0.zip` | Optional: the ANGLER and QUILLBACK robots (public domain, CC0) |

You need your own copy of Real Steel from an Xbox 360: the game and its
title update, plus the add-ons if you have them. Third-party licences are in the `licenses`
folder of each download.

## Checksums (SHA-256)

```
9b50e522fb5d751c53a73fb141b7c042479b9d020298cff3e9387070c96d5417  ReCompiledSteel-windows-x64-0.3.0.zip
1099c244e6b7ec8924ce3021999edbb067320203edd7ab9ac00da1de3f5ba729  ReCompiledSteel-linux-x64-0.3.0.tar.gz
d6f63ef838ee5a58c0409aff533722ee50601e6e9aaf6bd950c17b2119322c8b  ReCompiledSteel-android-arm64-0.3.0.apk
d835039ac94b611e048e95d40e0db7755a5bf26d442db83f468559c4a9ac7f66  ReCompiledSteel-mods-0.3.0.zip
```

Only download from this page, and compare the checksum if a copy reached you
some other way (`certutil -hashfile <file> SHA256` on Windows, `sha256sum
<file>` on Linux).
