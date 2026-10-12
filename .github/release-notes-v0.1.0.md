# ReCompiled Steel 0.1.0

The first public release: Real Steel (Xbox 360, 2011) as a native program for
**Windows**, **Linux / Steam Deck** and **Android** (arm64). It contains no
game data. The setup program copies your own copy of the game from files you
took off your Xbox 360.

## What's in it

- **The whole game, recompiled**: career, assembly, local multiplayer,
  add-ons, achievements and saves.
- **A native Vulkan renderer** in place of the Xbox GPU:
  - internal resolution up to 5K, MSAA, FXAA or both, and anisotropic
    filtering up to 16x;
  - v-sync on or off, and HDR output on HDR displays;
  - 60 fps on a Steam Deck and on a Retroid Pocket Nova.
- **Any screen shape: 16:9, 16:10, 21:9 and 4:3.** The 3D view widens instead
  of stretching, and menus are laid out for the shape. The help bar sits at
  the bottom and flashes cover the whole screen.
- **Settings in the game's own menus**: HELP AND OPTIONS > GAME OPTIONS, with
  tabs above the panel (LB / RB, or click or tap a tab). F1 or both stick
  clicks open the same settings as an overlay. F11 toggles fullscreen.
- **Online multiplayer through the game's own Xbox Live code**:
  - matches found through lobby servers anyone can run (LOBBY_SERVER.md), on
    the local network, or by typing an address;
  - match passwords and leaderboards.
- **Achievements** with unlock pop-ups, and the list in the game's menus.
- **Robot mods**: drop a robot folder into `mods/` (MODDING.md). Each one is
  built on any of the 24 stock skeletons, with its own models, textures,
  names, stats and prices. Removed mods are cleaned up once no saved robot
  uses them.
- **Controls**: keyboard controls you can rebind next to any controller. On
  Android, touch controls appear when no controller is connected, with the
  dodge buttons under the right thumb.
- **Android**: the APK keeps the game data in a `RealSteel` folder on the
  device and survives being suspended and resumed. See android/README.md in
  the repository.
- **Small comforts**:
  - F12 screenshots, and saves backed up on every launch;
  - English or Japanese, and mute while in the background;
  - a Cheats tab with game speed and a cash button;
  - QUIT GAME on the main menu, and no more DOWNLOAD CONTENT.
- Third-party licences are in the `licenses` folder of each download.

## Downloads

| File | For |
| --- | --- |
| `ReCompiledSteel-windows-x64-0.1.0.zip` | Windows 10 / 11, 64-bit: run RealSteelSetup.exe, then realsteel.exe |
| `ReCompiledSteel-linux-x64-0.1.0.tar.gz` | Linux and Steam Deck: run realsteel-setup, then start.sh |
| `ReCompiledSteel-android-arm64-0.1.0.apk` | Android, arm64, Vulkan 1.3: install, then copy your installed game files to a `RealSteel` folder on the device |

## Known issues

- Windows shows "Windows protected your PC" the first time, because the
  release is unsigned. Choose More info, then Run anyway.
- The MY SCORE page of the leaderboards stays empty.
- Hole punching through home routers has only been tested on a few networks.
  LOBBY_SERVER.md has the port-forwarding fallback.
- At 4:3 and 16:10, the full-screen fades in fights cover only the 16:9 part.
- On Android only the Retroid Pocket Nova has been tested.

## Checksums (SHA-256)

```
92637a3f52dfbe29be11ed861bb2de6e1724708f4899e8d6118ec6a5ccb5210f  ReCompiledSteel-windows-x64-0.1.0.zip
f0ff5af7d9bc4e20e041972acce54c41bb72da7db247a2b89c7525ede7f97046  ReCompiledSteel-linux-x64-0.1.0.tar.gz
dcd5f7d57127e2738b0733f80291a0f2f540e6a8420c8f660b9af25b1c5fd12f  ReCompiledSteel-android-arm64-0.1.0.apk
```

Only download from this page, and compare the checksum if a copy reached you
some other way (`certutil -hashfile <file> SHA256` on Windows, `sha256sum
<file>` on Linux).

## Thanks

ReXGlue, XenosRecomp, Xenia, XenonRecomp, Unleashed Recompiled, the Skate 3
Android port and the libraries listed in the README's credits.
