# ReCompiled Steel 0.1.2

## What's new

- **Phones: the game is landscape and fills the screen.** On phones (which
  are portrait by nature) the game could start sideways, and turning the
  phone stretched the picture instead of rotating it. It now always runs in
  landscape, either way up, and draws at the screen's real size.
- **Lower resolutions for slower devices.** Settings, Graphics, Internal
  resolution now also offers 540p, 480p and 360p. They are faster but softer
  than the original 720p, which stays the default.
- **Android: better bug reports.** **Save diagnostics** in the launcher now
  writes one zip with everything a report needs (the app's log, the game's
  logs, the renderer's log, any crash report and the settings), and a start
  that fails saves one by itself. Attach it to an issue: your online name,
  match password, lobby and IP addresses are left out of it. The logs also
  name the GPU and its driver.
- **Android: optional custom graphics driver** (Qualcomm/Adreno devices).
  **Graphics driver** in the launcher can load a Vulkan driver file you
  choose instead of the device's own, through libadrenotools. It is off
  unless you choose a file, entirely at your own risk and not supported by
  this project: check any problem with the system driver before reporting
  it. **Use system driver** undoes it, and after a failed start on a custom
  driver the launcher offers to switch back.

## Downloads

| File | For |
| --- | --- |
| `ReCompiledSteel-windows-x64-0.1.2.zip` | Windows 10 / 11, 64-bit: run RealSteelSetup.exe, then realsteel.exe |
| `ReCompiledSteel-linux-x64-0.1.2.tar.gz` | Linux and Steam Deck: run realsteel-setup, then start.sh |
| `ReCompiledSteel-android-arm64-0.1.2.apk` | Android, arm64, Vulkan 1.3: install, then **Install game files** in the app |

You need your own copy of Real Steel from an Xbox 360: the game
(`000D0000`) and its title update (`000B0000`), plus the add-ons
(`00000002`) if you have them. Third-party licences are in the `licenses`
folder of each download.

## Checksums (SHA-256)

```
dbb01dfacecdf7aa03a9e6276721df67733d1bbb1233b7ce8474c71c0203cbc7  ReCompiledSteel-windows-x64-0.1.2.zip
1b09ee16cf87579a9cf1b8fe0d811c0b1d288f328038d7d6875472f2a10236db  ReCompiledSteel-linux-x64-0.1.2.tar.gz
4875cf5d2bc850afae75b2aa24beea34bb312591b0653074a52451b1ff4144ae  ReCompiledSteel-android-arm64-0.1.2.apk
```

Only download from this page, and compare the checksum if a copy reached you
some other way (`certutil -hashfile <file> SHA256` on Windows, `sha256sum
<file>` on Linux).
