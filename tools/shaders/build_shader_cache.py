#!/usr/bin/env python3
"""Builds the native renderer's shader cache from the player's own game files.

The game's shaders (310 pixel + 96 vertex programs) sit inside the Yuke's
archives of the game and its title update. This script pulls every shader
container out of them (tools/pac/extract_shaders.py) and feeds them to the
Real Steel fork of XenosRecomp, which translates them to SPIR-V and writes
one C++ file holding the compressed cache that the game compiles in.

  build_shader_cache.py --game <game folder> [--tu <title update folder>]
                        --xenosrecomp <XenosRecomp executable>
                        --out generated/shader_cache.cpp

The game folder is the extracted Real Steel package (default.xex, pac/...),
the title update folder the extracted update package (pac/...). The build
(CMakeLists.txt) runs this when generated/shader_cache.cpp is missing or
older than its inputs; see docs/building.md for the XenosRecomp fork.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'pac'))
import extract_shaders  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', required=True, help='extracted game folder (holds pac/)')
    ap.add_argument('--tu', default='', help='extracted title update folder (holds pac/)')
    ap.add_argument('--xenosrecomp', required=True, help='XenosRecomp executable (the Real Steel fork)')
    ap.add_argument('--out', required=True, help='shader_cache.cpp to write')
    ap.add_argument('--prelude', default='',
                    help="HLSL prelude the translated shaders include (default: the fork's "
                         "own XenosRecomp/shader_common.h, found from the executable)")
    ap.add_argument('--keep', default='', help='keep the extracted containers in this folder')
    args = ap.parse_args()
    if not args.prelude:
        # <checkout>/build/XenosRecomp/XenosRecomp[.exe] -> <checkout>/XenosRecomp/shader_common.h
        d = os.path.dirname(os.path.abspath(args.xenosrecomp))
        for _ in range(4):
            candidate = os.path.join(d, 'XenosRecomp', 'shader_common.h')
            if os.path.isfile(candidate):
                args.prelude = candidate
                break
            d = os.path.dirname(d)
        else:
            args.prelude = os.path.join(HERE, '..', '..', 'thirdparty', 'XenosRecomp', 'XenosRecomp', 'shader_common.h')

    for path, what in ((os.path.join(args.game, 'pac'), 'game folder'),
                       (args.xenosrecomp, 'XenosRecomp executable'),
                       (args.prelude, 'shader prelude')):
        if not os.path.exists(path):
            print('build_shader_cache: %s not found: %s' % (what, path), file=sys.stderr)
            return 1
    if args.tu and not os.path.isdir(os.path.join(args.tu, 'pac')):
        print('build_shader_cache: title update folder has no pac/: %s' % args.tu, file=sys.stderr)
        return 1

    work = args.keep or tempfile.mkdtemp(prefix='rs_shaders_')
    containers = os.path.join(work, 'containers')
    if os.path.isdir(containers):
        shutil.rmtree(containers)
    roots = [args.game] + ([args.tu] if args.tu else [])
    count = extract_shaders.extract(roots, containers)
    if count == 0:
        print('build_shader_cache: no shader containers found under %s' % ', '.join(roots), file=sys.stderr)
        return 1

    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    tmp_out = args.out + '.tmp'
    cmd = [args.xenosrecomp, containers, tmp_out, args.prelude]
    print('build_shader_cache: translating %d containers...' % count)
    result = subprocess.run(cmd)
    if result.returncode != 0 or not os.path.isfile(tmp_out):
        print('build_shader_cache: XenosRecomp failed (%s)' % result.returncode, file=sys.stderr)
        return 1
    os.replace(tmp_out, args.out)
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    print('build_shader_cache: wrote %s' % args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
