# ReCompiled Steel 0.2.0

## What's new

- **Two new robots: ANGLER and QUILLBACK.** Made for this project and free
  for anyone to use (public domain). ANGLER is a deep-sea salvage diving suit
  with an anglerfish lure over its helmet; QUILLBACK is a spiked speedster
  with seven glowing gems caged in its waist. They are a separate, optional
  download: unzip **ReCompiledSteel-mods-0.2.0.zip** into the game's folder
  (on Android, copy the robot folders into `RealSteel/mods`). The official
  add-ons must be installed. Tested on Windows so far.
- **The game builds mods itself.** A robot mod is now just its models and
  pictures: the game turns them into game files when it starts, from your own
  copy of the game, so no mod has to carry game data and nobody runs tools.
  Mods with a `built` folder still load. A robot that can't be loaded (no
  add-ons, an unknown base set, a model with a problem) is left out, and the
  game says which and why at the bottom of the screen.
- **Options for slower devices**, all off by default so nothing changes
  unless you turn them on: **Crowd** off, **Texture quality: Half** and
  **Keep game speed** (fewer frames instead of slow motion on devices that
  can't hold 60 fps), next to the lower resolutions from 0.1.2.
  **Show memory use** and **Frame rate position** help to see what they
  change. Large textures also load faster, and memory use on the GPU is much
  lower.
- **Android: older drivers.** Drivers that report Vulkan 1.1 or 1.2 can now
  work through the extensions they offer; if something is still missing, the
  game says what instead of closing.
- **Android: the right way up.** The picture is drawn in the screen's own
  orientation, so it is correct whichever way round you hold the device,
  also on handhelds with a sideways-mounted screen.
- **Android: touch controls your way.** Settings, Controls: size and
  see-through for all controls, and **Arrange touch controls** to drag each
  button where you want it and change its size, shape (circle, rounded,
  square or a curved kidney) and angle. Settings lists scroll by dragging.

## Downloads

| File | For |
| --- | --- |
| `ReCompiledSteel-windows-x64-0.2.0.zip` | Windows 10 / 11, 64-bit: run RealSteelSetup.exe, then realsteel.exe |
| `ReCompiledSteel-linux-x64-0.2.0.tar.gz` | Linux and Steam Deck: run realsteel-setup, then start.sh |
| `ReCompiledSteel-android-arm64-0.2.0.apk` | Android, arm64: install, then **Install game files** in the app |
| `ReCompiledSteel-mods-0.2.0.zip` | Optional: the ANGLER and QUILLBACK robots (public domain, CC0) |

You need your own copy of Real Steel from an Xbox 360: the game
(`000D0000`) and its title update (`000B0000`), plus the add-ons
(`00000002`) if you have them. Third-party licences are in the `licenses`
folder of each download.

## Checksums (SHA-256)

```
dd714abe1174265fb8ce5cd4edff57d8d9fb7ba3bbcdaf70c3c91d6caf69ee2d  ReCompiledSteel-windows-x64-0.2.0.zip
b4fe70ae82be945dfb0fa297af05288377b8e1db3059197925788041a94a3261  ReCompiledSteel-linux-x64-0.2.0.tar.gz
4249f28755b12594ad0a99b22dda726d82903932bfc08a52a28ddac45a9398b4  ReCompiledSteel-android-arm64-0.2.0.apk
77e38cdf60063d3dca84c42fa339b0bc312ef197fe58600be92cd7b5cd071b30  ReCompiledSteel-mods-0.2.0.zip
```

Only download from this page, and compare the checksum if a copy reached you
some other way (`certutil -hashfile <file> SHA256` on Windows, `sha256sum
<file>` on Linux).
