# ReCompiled Steel 0.1.1

## What's new

- **Android installs the game by itself.** Press **Install game files** in
  the launcher, pick the folder holding your Xbox 360 files (the game, its
  title update and any add-ons, for example the `Content` folder from your
  console or USB stick), and the installer lays everything out, the same way
  the PC and Steam Deck setup programs do. No more copying folders across
  from a PC install.
- **A new Android launcher:** the banner, what is installed at a glance, and
  a progress bar while installing.
- **The title update is required, and the game now says so.** The port is
  built for the updated game, so the original executable alone could not
  start: on Android it crashed (#1). The setup programs and the Android
  installer now refuse to install without the title update, and the game
  shows a clear message instead of crashing if it is missing.
- A start that failed could crash on the way out (a GPU thread outliving its
  service); it now shuts down cleanly.

## Downloads

| File | For |
| --- | --- |
| `ReCompiledSteel-windows-x64-0.1.1.zip` | Windows 10 / 11, 64-bit: run RealSteelSetup.exe, then realsteel.exe |
| `ReCompiledSteel-linux-x64-0.1.1.tar.gz` | Linux and Steam Deck: run realsteel-setup, then start.sh |
| `ReCompiledSteel-android-arm64-0.1.1.apk` | Android, arm64, Vulkan 1.3: install, then **Install game files** in the app |

You need your own copy of Real Steel from an Xbox 360: the game
(`000D0000`) and its title update (`000B0000`), plus the add-ons
(`00000002`) if you have them. Third-party licences are in the `licenses`
folder of each download.

## Checksums (SHA-256)

```
f8ee6d39567447ee39b38e07248b69ac337e66cb7c274845ab97da4960ed5268  ReCompiledSteel-windows-x64-0.1.1.zip
8dd1dc6654f36b31e0eb63cde10a3276f0d373fe2537549cb80bbdcb7e0cbaab  ReCompiledSteel-linux-x64-0.1.1.tar.gz
1cb7596c8f9570043abed14e31d680af7a085593e159a23413d9522fb873d53b  ReCompiledSteel-android-arm64-0.1.1.apk
```

Only download from this page, and compare the checksum if a copy reached you
some other way (`certutil -hashfile <file> SHA256` on Windows, `sha256sum
<file>` on Linux).
