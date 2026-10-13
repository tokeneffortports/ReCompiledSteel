# Building ReCompiled Steel from source

Nothing from the game is in this repository. The build takes **your own copy
of Real Steel** and produces the recompiled code and the shader cache from it
on your machine. You need the game's Xbox Live Arcade package and its title
update (the 2015 one: the recompiled code is that version, so it is required).

Two things are generated and never checked in:

| Generated | From | By |
| --- | --- | --- |
| `generated/default_final/` (the game's code as C++) | `work/game/default.xex` + `default.xexp` | `rexglue codegen`, run by the build |
| `generated/shader_cache.cpp` (the game's shaders as SPIR-V) | `work/game/pac/_startup.pac` | `tools/shaders/build_shader_cache.py` + XenosRecomp, run by the build |

## 1. Tools

**Windows**

* Visual Studio 2022 Build Tools with the Windows SDK (headers only; the
  compiler is clang)
* LLVM / clang 18 or newer (`winget install LLVM.LLVM`)
* CMake 3.25+ and Ninja (`winget install Kitware.CMake Ninja-build.Ninja`)
* Python 3.8+ (`winget install Python.Python.3.12`), with `numpy` only if you
  build mods
* git

**Linux (Ubuntu 24.04 / the Steam Deck build)**

```bash
sudo apt-get install -y cmake ninja-build git python3 libgtk-3-dev libvulkan-dev
# clang 20 from https://apt.llvm.org:  wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 20
```

## 2. The ReXGlue SDK

The recompiler and runtime are the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
(open source). Download a release zip for your platform from its GitHub
Releases page (`rexglue-sdk-<version>-win-amd64.zip` /
`-linux-amd64.zip`) and unpack it, for example to `C:\rexglue\win-amd64` or
`~/rexglue/linux-amd64`. This project was made with **0.10.0** (nightly
`0.10.0-dev.g509ed5b`); a newer 0.10.x nightly should work, anything else may
need `generated/rexglue.cmake` regenerated with `rexglue init`.

## 3. Your game files

Copy the Real Steel packages from your Xbox 360 to the PC (FTP from a console
running Aurora or FSD, or a PC tool that reads Xbox 360 storage such as Party
Buffalo, Xplorer360 or USBXTAFGUI; they are under
`Content\0000000000000000\584111E0\`):

| Folder on the console | What | Needed |
| --- | --- | --- |
| `000D0000\<40 hex characters>` (about 200 MB) | the game | yes |
| `000B0000\<40 hex characters>` (about 6 MB) | the title update | yes |
| `00000002\<40 hex characters>` (18 files) | the add-ons | only to play them |

Then lay them out in `work/`:

```bash
python tools/pac/stfs_extract.py --work work <path to the game package> <path to the title update package>
```

which gives `work/game/` (with `default.xex`, the update's `default.xexp`
beside it, `pac/`, ...) and `work/tu/` (the update's `pac/`). The setup
program does the same with a window once it is built:
`RealSteelSetup.exe --scan <folder with your files> --dest work --yes`.

## 4. XenosRecomp (shader translator)

```bash
python tools/shaders/setup_xenosrecomp.py
```

clones [hedge-dev/XenosRecomp](https://github.com/hedge-dev/XenosRecomp) at
the pinned commit into `thirdparty/XenosRecomp` (about 250 MB with its
submodules), applies `thirdparty/XenosRecomp-realsteel.patch` (the Real
Steel changes) and builds it with clang. The game build finds the executable
there by itself; `-DRS_XENOSRECOMP=<path>` points it elsewhere.

Windows Defender's heuristics sometimes flag the freshly built, unsigned
`XenosRecomp.exe` (seen as "Trojan:Win32/Bearfoos.B!ml"). It is a false
positive on a command-line tool you just compiled from public source; restore
it from quarantine or add the folder to the exclusions, then build again.

## 5. Build

**Windows** (a "x64 Native Tools" prompt is not needed, clang finds the SDK):

```bat
cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=C:/rexglue/win-amd64
cmake --build --preset win-amd64-release
```

The first configure runs `rexglue codegen` (a few seconds; the SDK's
CMake only lists recompiled sources that already exist, so the project runs
it before including the SDK). The first build then translates the shaders
(about a minute) and compiles the 18,000 recompiled functions (several
minutes). If a build ever stops on a stale precompiled header right after
codegen re-ran, run the build command again.

Outputs: `out/build/win-amd64-release/realsteel.exe` (with `rexruntime.dll`)
and `tools/setup/RealSteelSetup.exe` under the same folder.

**Linux / Steam Deck**: `bash tools/build_linux.sh` does the equivalent with the
`linux-amd64-release` preset (set `REXSDK_LINUX_DIR` to the SDK folder; the
script mirrors the tree to `~/realsteel-deck-build` because NTFS mounts
under WSL are slow) and stages a ready-to-run folder in
`release/ReCompiledSteel-linux-x64/`.

## 6. Run

A built `realsteel.exe` looks for `game/` and `tu/` beside itself; during
development point it at `work/` instead:

```bat
tools
un_dev.bat
```

(`tools/run_dev.sh` on Linux), which passes `--game_data_root=work/game
--update_data_root=work/tu --realsteel_mods_dir=mods`. Settings are on the
in-game page (F1, or click both sticks); logs land in the working folder.

## 7. Release packages

```bash
python tools/release/package.py
```

zips the Windows build with the setup program, the player readme and the
third-party licences into
`release/ReCompiledSteel-windows-x64.zip`; on Linux `tools/build_linux.sh` leaves
`release/ReCompiledSteel-linux-x64.tar.gz`. Neither contains any game data: the
setup program copies the player's own files in.

**Android** (arm64, built from WSL or Linux): see [android/README.md](../android/README.md).

## Layout

| Folder | What |
| --- | --- |
| `src/` | the game's host side: hooks, native renderer glue, settings, keyboard and touch input, online play, mod loader |
| `tools/replayer/` | the Vulkan renderer, compiled into the game |
| `tools/setup/` | the setup program (STFS package reader, Windows window / Linux dialogs) |
| `tools/shaders/` | the shader cache build and the XenosRecomp fetch script |
| `tools/pac/` | the STFS extractor and the readers for Yuke's archives (EPAC / PACH / BPE) |
| `tools/mods/` | robot mod tools (`make_mod.py`, see [modding.md](modding.md)) |
| `tools/lobby/` | the lobby and leaderboard server ([lobby-server.md](lobby-server.md)) |
| `tools/release/` | release packaging and the icon |
| `android/` | the Android app (Java launcher, native glue, build scripts) |
| `packaging/` | what goes into the release folders: the player readme, the Linux start script, the third-party licences |
| `thirdparty/` | our patches for XenosRecomp and (Android) the ReXGlue SDK |

## Third-party components

None of them are kept in this repository. You download what the list says;
the rest is fetched for you, at pinned versions:

| Component | What for | How you get it |
| --- | --- | --- |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) 0.10.0 | the recompiler and the runtime the game runs on | download a release (step 2) |
| LLVM / clang, CMake, Ninja, Python 3, git | building | install them (step 1) |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) at `990d03b` | translating the game's shaders | `tools/shaders/setup_xenosrecomp.py` clones it and applies `thirdparty/XenosRecomp-realsteel.patch` (step 4) |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) v1.3.296, [volk](https://github.com/zeux/volk) 1.3.295 | the renderer's Vulkan access | downloaded by CMake on the first configure |
| [Zstandard](https://github.com/facebook/zstd) v1.5.7, [smol-v](https://github.com/aras-p/smol-v) `9dd54c3` | reading the shader cache | downloaded by CMake on the first configure |
| numpy | the mod tools only | `pip install numpy` |
| Android NDK, SDK, JDK 17 | the Android build only | `android/dev/setup_toolchain.sh` (you accept Google's licences there) |
| ReXGlue SDK source at `509ed5b` | the Android build only | `android/build_sdk.sh` clones it and applies `thirdparty/rexglue-android.patch` |
| [libadrenotools](https://github.com/bylaws/libadrenotools) `8fae8ce` | the Android launcher's optional custom GPU driver | downloaded by CMake on the first Android configure |

The first configure needs an internet connection for the CMake downloads. The
release packages carry every licence these components require
(`packaging/licenses/`).
