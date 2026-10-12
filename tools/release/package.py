#!/usr/bin/env python3
"""Assembles the release packages (no game data inside):

  release/ReCompiledSteel-windows-x64/   + .zip      from out/build/win-amd64-release
  release/ReCompiledSteel-linux-x64/     + .tar.gz   from a Linux build folder

  package.py [--windows <build dir>] [--linux <build dir> --linux-sdk <sdk dir>]
             [--out release] [--version 1.0]

Each package: the game, its runtime libraries, the setup program, the player
readme, the modding and lobby guides, the lobby server script and an empty
mods/ folder. tools/build_linux.sh calls this for the Linux side.
"""
import argparse
import os
import shutil
import stat
import sys
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))

# (source in the repository, name in the release folder)
DOCS = [('docs/modding.md', 'MODDING.md'), ('docs/lobby-server.md', 'LOBBY_SERVER.md'), ('LICENSE', 'LICENSE')]


def common(dest):
    os.makedirs(dest, exist_ok=True)
    shutil.copy(os.path.join(ROOT, 'packaging', 'README.txt'), os.path.join(dest, 'README.txt'))
    for src, name in DOCS:
        shutil.copy(os.path.join(ROOT, src), os.path.join(dest, name))
    licenses = os.path.join(dest, 'licenses')  # the third-party notices the binaries need
    if os.path.isdir(licenses):
        shutil.rmtree(licenses)
    shutil.copytree(os.path.join(ROOT, 'packaging', 'licenses'), licenses)
    skel = os.path.join(dest, 'skeletons')  # MODDING.md links skeletons/<SET>.png
    if os.path.isdir(skel):
        shutil.rmtree(skel)
    shutil.copytree(os.path.join(ROOT, 'docs', 'skeletons'), skel)
    os.makedirs(os.path.join(dest, 'tools', 'lobby'), exist_ok=True)
    shutil.copy(os.path.join(ROOT, 'tools', 'lobby', 'realsteel_lobby.py'), os.path.join(dest, 'tools', 'lobby'))
    os.makedirs(os.path.join(dest, 'mods'), exist_ok=True)
    with open(os.path.join(dest, 'mods', 'README.txt'), 'w', newline='\n') as f:
        f.write('Robot mod folders go here - see MODDING.md in the game folder.\n')


def vcredist_dir(given):
    """The x64 Microsoft.VC14x.CRT folder of a Visual Studio / Build Tools install."""
    if given:
        return given
    import glob
    candidates = []
    for root in (os.environ.get('ProgramFiles', r'C:\Program Files'),
                 os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')):
        candidates += glob.glob(os.path.join(root, 'Microsoft Visual Studio', '*', '*', 'VC', 'Redist', 'MSVC', '*', 'x64', 'Microsoft.VC*.CRT'))
    candidates = [c for c in candidates if os.path.isfile(os.path.join(c, 'msvcp140.dll'))]
    return sorted(candidates)[-1] if candidates else ''


args_vcredist = ''


def windows(build, out, version):
    dest = os.path.join(out, 'ReCompiledSteel-windows-x64')
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    common(dest)
    for name in ('realsteel.exe', 'rexruntime.dll'):  # (the SDK's TracyClient is not used)
        src = os.path.join(build, name)
        if not os.path.isfile(src):
            print('missing %s' % src, file=sys.stderr)
            return None
        shutil.copy(src, dest)
    # The Visual C++ runtime the binaries link (the SDK's too); shipped so a
    # plain Windows works without the redistributable installer.
    crt = vcredist_dir(args_vcredist)
    for name in ('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
        src = os.path.join(crt, name) if crt else ''
        if not os.path.isfile(src):
            print('missing Visual C++ runtime file %s (pass --vcredist <Microsoft.VC14x.CRT folder>)' % name, file=sys.stderr)
            return None
        shutil.copy(src, dest)
    setup = os.path.join(build, 'tools', 'setup', 'RealSteelSetup.exe')
    if not os.path.isfile(setup):
        print('missing %s' % setup, file=sys.stderr)
        return None
    shutil.copy(setup, dest)
    archive = dest + ('-%s' % version if version else '') + '.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for dirpath, _, files in os.walk(dest):
            for f in files:
                p = os.path.join(dirpath, f)
                z.write(p, os.path.join('ReCompiledSteel', os.path.relpath(p, dest)))
    print('wrote %s' % archive)
    return archive


def linux(build, sdk, out, version):
    dest = os.path.join(out, 'ReCompiledSteel-linux-x64')
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    common(dest)
    for src in (os.path.join(build, 'realsteel'),
                os.path.join(build, 'tools', 'setup', 'realsteel-setup'),
                os.path.join(sdk, 'lib', 'librexruntime.so')):
        if not os.path.isfile(src):
            print('missing %s' % src, file=sys.stderr)
            return None
        shutil.copy(src, dest)
    shutil.copy(os.path.join(ROOT, 'packaging', 'linux', 'start.sh'), os.path.join(dest, 'start.sh'))
    for name in ('realsteel', 'realsteel-setup', 'start.sh'):
        p = os.path.join(dest, name)
        os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    archive = dest + ('-%s' % version if version else '') + '.tar.gz'
    def anonymous(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ''
        return info
    with tarfile.open(archive, 'w:gz') as t:
        t.add(dest, arcname='ReCompiledSteel', filter=anonymous)
    print('wrote %s' % archive)
    return archive


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--windows', default=os.path.join(ROOT, 'out', 'build', 'win-amd64-release'))
    ap.add_argument('--no-windows', action='store_true')
    ap.add_argument('--linux', default='')
    ap.add_argument('--linux-sdk', default='')
    ap.add_argument('--out', default=os.path.join(ROOT, 'release'))
    ap.add_argument('--version', default='', help='default: the number in src/realsteel_version.h')
    ap.add_argument('--vcredist', default='', help='x64 Microsoft.VC14x.CRT folder (found automatically in Visual Studio)')
    args = ap.parse_args()
    global args_vcredist
    args_vcredist = args.vcredist
    os.makedirs(args.out, exist_ok=True)
    if not args.version:
        import re
        header = open(os.path.join(ROOT, 'src', 'realsteel_version.h'), encoding='utf-8').read()
        found = re.search(r'kVersion\s*=\s*"([^"]+)"', header)
        args.version = found.group(1) if found else ''
    ok = True
    if not args.no_windows and sys.platform == 'win32':
        ok = windows(args.windows, args.out, args.version) is not None and ok
    if args.linux:
        ok = linux(args.linux, args.linux_sdk, args.out, args.version) is not None and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
