#!/usr/bin/env python3
"""Fetches and builds the Real Steel fork of XenosRecomp, the shader
translator the build needs (generated/shader_cache.cpp).

  setup_xenosrecomp.py [--dir thirdparty/XenosRecomp] [--no-build]

Steps: clone hedge-dev/XenosRecomp at the pinned commit (with its
submodules: DXC binaries, fmt, smol-v, xxHash, zstd - about 250 MB), apply
thirdparty/XenosRecomp-realsteel.patch (the Real Steel changes: COLOR2/3
interpolators, bool-conditioned clauses, vertex-fetch records, vertex shader
templates...), copy tools/replayer/xenos_vfetch_scan.h in, and build it with
CMake + Ninja + clang into <dir>/build. Re-running is safe: an existing
checkout is reused and only rebuilt.

Needs git, cmake, ninja and clang (the same tools the game needs).
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
UPSTREAM = 'https://github.com/hedge-dev/XenosRecomp'
COMMIT = '990d03b28a27b50277ee5d8d942e1c5f873869d1'   # 2025-08-03, "Fix half pixel..."
PATCH = os.path.join(ROOT, 'thirdparty', 'XenosRecomp-realsteel.patch')
MARKER = 'Real Steel (2026-10-02)'


def run(cmd, cwd=None):
    print('+ ' + ' '.join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=cwd)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dir', default=os.path.join(ROOT, 'thirdparty', 'XenosRecomp'))
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--compiler', default='', help='C++ compiler (default: clang++, clang++-20 on Linux if present)')
    args = ap.parse_args()
    d = os.path.abspath(args.dir)

    if not os.path.isdir(os.path.join(d, '.git')):
        run(['git', 'clone', UPSTREAM, d])
    run(['git', 'checkout', '--quiet', COMMIT], cwd=d)
    run(['git', 'submodule', 'update', '--init', '--recursive', '--depth', '1'], cwd=d)

    main_cpp = os.path.join(d, 'XenosRecomp', 'main.cpp')
    if MARKER not in open(main_cpp, encoding='utf-8', errors='replace').read():
        run(['git', 'apply', '--whitespace=nowarn', PATCH], cwd=d)
    shutil.copy(os.path.join(ROOT, 'tools', 'replayer', 'xenos_vfetch_scan.h'),
                os.path.join(d, 'XenosRecomp', 'xenos_vfetch_scan.h'))
    print('XenosRecomp source ready in %s' % d)
    if args.no_build:
        return 0

    cxx = args.compiler
    if not cxx:
        cxx = 'clang++'
        if sys.platform != 'win32' and shutil.which('clang++-20'):
            cxx = 'clang++-20'
    cc = cxx.replace('clang++', 'clang')
    build = os.path.join(d, 'build')
    run(['cmake', '-S', d, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_C_COMPILER=' + cc, '-DCMAKE_CXX_COMPILER=' + cxx])
    run(['cmake', '--build', build])
    exe = os.path.join(build, 'XenosRecomp', 'XenosRecomp' + ('.exe' if sys.platform == 'win32' else ''))
    if not os.path.isfile(exe):
        print('build finished but %s is missing' % exe, file=sys.stderr)
        return 1
    print('XenosRecomp built: %s' % exe)
    print('The game build finds it there by itself (or pass -DRS_XENOSRECOMP=%s).' % exe)
    return 0


if __name__ == '__main__':
    sys.exit(main())
