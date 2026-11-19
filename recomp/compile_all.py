"""Compile the generated C in parallel (checks it builds; objects for linking).

Usage: python -I compile_all.py <gen_dir> <obj_dir> <x86_64|i686> [extra clang flags...]
"""
import glob
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(os.path.dirname(HERE), 'runtime')

FLAGS = ['-O2', '-c', '-fno-strict-aliasing', '-ffp-contract=off', '-fwrapv',
         '-Wall', '-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
         '-Wno-self-assign', '-Wno-unused-function']
ARCH = {'x86_64': ['--target=x86_64-w64-mingw32'],
        'i686': ['--target=i686-w64-mingw32', '-msse2', '-mfpmath=sse']}


def main():
    gen, obj, arch = sys.argv[1], sys.argv[2], sys.argv[3]
    extra = sys.argv[4:]
    os.makedirs(obj, exist_ok=True)
    srcs = sorted(glob.glob(os.path.join(gen, '*.c')))
    srcs = [s for s in srcs if not s.endswith('imports_stub.c')] + \
        [s for s in srcs if s.endswith('imports_stub.c')]

    def one(src):
        out = os.path.join(obj, os.path.basename(src)[:-2] + '.o')
        t = time.time()
        p = subprocess.run(['clang'] + ARCH[arch] + FLAGS + extra + ['-I', RUNTIME, '-I', gen, src, '-o', out],
                           capture_output=True, text=True)
        return src, p.returncode, p.stderr, time.time() - t

    t0 = time.time()
    bad = 0
    with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        for src, rc, err, dt in ex.map(one, srcs):
            if rc or err.strip():
                bad += rc != 0
                print('%s: rc=%d %.1fs\n%s' % (os.path.basename(src), rc, dt, err[:3000]))
    print('%d files, %d failed, %.1f s' % (len(srcs), bad, time.time() - t0))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
