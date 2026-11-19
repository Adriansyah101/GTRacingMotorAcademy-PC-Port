"""Stage 5: a clean release folder of the GT Racing PC port from the release build (build/host/release, made by
python -I host/build.py --release: GT_DEV=0, no diagnostic key or trace).

usage: python -I tools/package_release.py [--version V] [--out DIR] [--include-patch] [--console] [--no-run] [--force]
                                          [--zip]

  --version V        the folder's name: GTRacing-PC-Port-<V>-Windows-x86 (default: today's date)
  --out DIR          where the folder goes (default build/release)
  --include-patch    also copy build/host/patch: the menu files the build patched for the PC menus (Controls rows,
                     Display, Exit and Name tiles).  They are Gameloft's menu files, changed: the folder is then
                     named ...-PRIVATE and must not be shared.  Without it the release has the game's own menus.
  --console          keep the console window (the build's subsystem); by default the release exe is a window
                     program (no console behind the game)
  --no-run           skip the check run of the stripped exe (--selftest)
  --force            replace an existing release folder of that name (and its zip)
  --zip              also make <out>/<folder>.zip: the folder as its one top folder, read back and compared

What it does:
  1. gtracing.exe is copied and stripped (strip --strip-all: the debug info and the symbol table).  Then each
     section the loader maps is checked against the build's: same address, size, flags and bytes, the reserved
     .gtimg (host/core/reserve.S, gt.ld) first at 0x11000; entry point, image base and data directories the same.
  2. Only these go in: gtracing.exe, SDL3.dll, gtracing.ini, README.txt and IMPORTANT.TXT (tools/release templates:
     the player's defaults, not the developer's own ini; IMPORTANT.TXT holds the link to the notes and downloads of
     the optional patch/ and add-on/ folders), SHA256SUMS.txt; patch/ with --include-patch.
     The folders GTRacingFreemiumHD.app/, patch/ and add-on/ come along empty but for a note each
     (tools/release/folders), so a player sees where the game and the optional files go.  The exe skips a
     GTRacingFreemiumHD.app without the game's program in it (host/core/main.c find_app).  Nothing else of
     build/host (gtracing.map, err.txt, app.rc, logs, link.rsp, home folders, obj, shots) can come along.
  3. gtracing.map goes to <out>/symbols/<folder>.map, outside the release: crash logs name host addresses, and
     the map of the same build turns them into function names.
  4. Every file of the release is searched (tools/privacy_scan.py: 8-bit and UTF-16LE, any case) for the Windows
     user's name, the PC's name, the Windows user's folder, C:\\Users and the project's folder (each folder name in
     its path); any hit stops the script.
     The exe must also hold no debug section and no symbol table.
  5. The stripped exe runs --selftest against the project's GTRacingFreemiumHD.app with its home, log and ini in a
     temporary folder (nothing is written into the release), and its log is searched the same way.
"""
import datetime
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, 'build', 'host')                # SDL3.dll and patch/ (both builds share them)
RELEASE_BUILD = os.path.join(BUILD, 'release')               # gtracing.exe and gtracing.map of the release build
APP = os.path.join(ROOT, 'GTRacingFreemiumHD.app')
MSYS = r'C:\msys64\mingw32\bin'
STRIP = os.path.join(MSYS, 'strip.exe')
TEMPLATES = os.path.join(HERE, 'release')
RESERVE_START = 0x11000                     # host/games/gtracing/layout.h GT_RESERVE_START

PC_MENUS_WITH = ("The PC menus are in (patch\\): Options > Display changes the settings above in the game, Options >\n"
                 "Controls shows and changes the keys, the main menu has an Exit tile and the multiplayer page a\n"
                 "Name tile.")
PC_MENUS_WITHOUT = ("This release has the game's own menus: the PC menus (Options > Display, the keys page in Options >\n"
                    "Controls, the Exit and Name tiles) come in the optional patch\\ folder (see above), which is\n"
                    "empty here.  Without its files, change the settings in gtracing.ini.")
RELEASE_FILES = ('gtracing.exe', 'SDL3.dll', 'gtracing.ini', 'README.txt', 'IMPORTANT.TXT')   # SHA256SUMS.txt last
FOLDERS = os.path.join(TEMPLATES, 'folders')             # <folder>/<note>.txt, copied with CRLF line ends
NOTE_DIRS = ('GTRacingFreemiumHD.app', 'patch', 'add-on')


def fail(msg):
    print('FAILED: ' + msg)
    sys.exit(1)


# PE

class PE:
    def __init__(self, path):
        self.path = path
        self.data = bytearray(open(path, 'rb').read())
        d = self.data
        if d[:2] != b'MZ':
            fail('%s: not an exe' % path)
        self.pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[self.pe:self.pe + 4] != b'PE\0\0':
            fail('%s: no PE header' % path)
        coff = self.pe + 4
        (self.machine, nsec, _, self.symtab, self.nsyms, opt_size, _) = struct.unpack_from('<HHIIIHH', d, coff)
        self.opt = coff + 20
        if struct.unpack_from('<H', d, self.opt)[0] != 0x10b:
            fail('%s: not a 32-bit exe' % path)
        self.entry = struct.unpack_from('<I', d, self.opt + 16)[0]
        self.image_base = struct.unpack_from('<I', d, self.opt + 28)[0]
        self.size_of_image = struct.unpack_from('<I', d, self.opt + 56)[0]
        self.subsystem = struct.unpack_from('<H', d, self.opt + 68)[0]
        ndirs = struct.unpack_from('<I', d, self.opt + 92)[0]
        self.dirs = [struct.unpack_from('<II', d, self.opt + 96 + 8 * i) for i in range(min(ndirs, 16))]
        self.sections = []
        s = self.opt + opt_size
        strtab = self.symtab + 18 * self.nsyms if self.symtab else 0   # long section names ("/4" = offset 4)
        for i in range(nsec):
            name = bytes(d[s:s + 8]).rstrip(b'\0').decode('latin-1')
            if name.startswith('/') and name[1:].isdigit() and strtab:
                o = strtab + int(name[1:])
                name = bytes(d[o:d.index(b'\0', o)]).decode('latin-1')
            vsize, va, rsize, rptr = struct.unpack_from('<IIII', d, s + 8)
            flags = struct.unpack_from('<I', d, s + 36)[0]
            self.sections.append(dict(name=name, vsize=vsize, va=va, rsize=rsize, rptr=rptr, flags=flags))
            s += 40

    def mapped(self):
        """the sections the loader maps (not the debug ones: .debug* / .zdebug*, or discardable)"""
        return [x for x in self.sections if not x['name'].lstrip('.').startswith(('debug', 'zdebug')) and
                not x['flags'] & 0x02000000]

    def content(self, sec):
        n = min(sec['vsize'], sec['rsize'])
        return bytes(self.data[sec['rptr']:sec['rptr'] + n]) if sec['rptr'] else b''

    def set_subsystem(self, sub):
        struct.pack_into('<H', self.data, self.opt + 68, sub)
        self.subsystem = sub

    def fix_checksum(self):
        """the PE checksum (imagehlp's CheckSumMappedFile): 16-bit sums with carry, the field itself left out"""
        off = self.opt + 64
        struct.pack_into('<I', self.data, off, 0)
        d = bytes(self.data) + (b'\0' if len(self.data) % 2 else b'')
        total = 0
        for (w,) in struct.iter_unpack('<H', d):
            total += w
            total = (total & 0xffff) + (total >> 16)
        total = (total & 0xffff) + (total >> 16)
        struct.pack_into('<I', self.data, off, (total + len(self.data)) & 0xffffffff)

    def save(self):
        open(self.path, 'wb').write(self.data)


def check_strip(orig, stripped):
    """every mapped section, the entry, the base and the data directories as the build's"""
    a, b = orig.mapped(), stripped.mapped()
    if [x['name'] for x in a] != [x['name'] for x in b]:
        fail('strip changed the sections: %s -> %s' % ([x['name'] for x in a], [x['name'] for x in b]))
    for x, y in zip(a, b):
        for k in ('va', 'vsize', 'flags'):
            if x[k] != y[k]:
                fail('strip changed %s %s: 0x%x -> 0x%x' % (x['name'], k, x[k], y[k]))
        if orig.content(x) != stripped.content(y):
            fail('strip changed the bytes of %s' % x['name'])
    if (orig.entry, orig.image_base) != (stripped.entry, stripped.image_base):
        fail('strip changed the entry point or the image base')
    if orig.dirs != stripped.dirs:
        changed = [i for i in range(len(orig.dirs)) if orig.dirs[i] != stripped.dirs[i]]
        fail('strip changed the data directories %s' % changed)
    first = b[0]
    if first['name'] != '.gtimg' or stripped.image_base + first['va'] != RESERVE_START:
        fail('the reserved game range is not the first section at 0x%x' % RESERVE_START)
    if stripped.nsyms:                    # a string table may stay: it spells long section names (.eh_frame)
        fail('the stripped exe still has %d symbols' % stripped.nsyms)
    left = [x['name'] for x in stripped.sections if x not in stripped.mapped()]
    if left:
        fail('the stripped exe still has debug sections: %s' % left)
    return len(b)


# privacy scan (tools/privacy_scan.py)

sys.path.insert(0, HERE)                    # python -I leaves the script's folder off the path
from privacy_scan import mask, private_words, scan_bytes, scan_paths  # noqa: E402


# main

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def main():
    args = sys.argv[1:]

    def opt(name, default=None):
        if name in args:
            i = args.index(name)
            if i + 1 >= len(args):
                fail('%s needs a value' % name)
            return args[i + 1]
        return default
    version = opt('--version', datetime.date.today().isoformat())
    out = os.path.abspath(opt('--out', os.path.join(ROOT, 'build', 'release')))
    with_patch = '--include-patch' in args
    if not version.replace('.', '').replace('-', '').replace('_', '').isalnum():
        fail('--version: letters, digits, dots and dashes only')
    name = 'GTRacing-PC-Port-%s-Windows-x86%s' % (version, '-PRIVATE' if with_patch else '')
    dest = os.path.join(out, name)

    exe = os.path.join(RELEASE_BUILD, 'gtracing.exe')
    for need in (exe, os.path.join(BUILD, 'SDL3.dll'), STRIP, os.path.join(TEMPLATES, 'gtracing.ini'),
                 os.path.join(TEMPLATES, 'README.txt'), os.path.join(TEMPLATES, 'IMPORTANT.TXT'),
                 *[os.path.join(FOLDERS, d) for d in NOTE_DIRS]):
        if not os.path.exists(need):
            fail('missing %s' % need)
    zip_path = dest + '.zip'
    make_zip = '--zip' in args
    if os.path.exists(dest) or (make_zip and os.path.exists(zip_path)):
        if '--force' not in args:
            fail('%s exists (--force replaces it)' % (dest if os.path.exists(dest) else zip_path))
        if os.path.exists(dest):
            shutil.rmtree(dest)
        if make_zip and os.path.exists(zip_path):
            os.remove(zip_path)
    os.makedirs(dest)
    print('release folder: %s' % dest)

    # 1. the exe, stripped and checked
    orig = PE(exe)
    rel_exe = os.path.join(dest, 'gtracing.exe')
    p = subprocess.run([STRIP, '--strip-all', '-o', rel_exe, exe], capture_output=True, text=True)
    if p.returncode:
        fail('strip: %s' % (p.stdout + p.stderr).strip())
    stripped = PE(rel_exe)
    n = check_strip(orig, stripped)
    if '--console' not in args:
        stripped.set_subsystem(2)               # IMAGE_SUBSYSTEM_WINDOWS_GUI: no console window behind the game
    stripped.fix_checksum()
    stripped.save()
    print('gtracing.exe: stripped %.1f -> %.1f MB; %d mapped sections, .gtimg at 0x%x, entry and directories '
          'unchanged; %s subsystem' % (os.path.getsize(exe) / 1e6, os.path.getsize(rel_exe) / 1e6, n, RESERVE_START,
                                       'console' if stripped.subsystem == 3 else 'window'))

    # 2. the rest, by name only
    shutil.copy2(os.path.join(BUILD, 'SDL3.dll'), dest)
    shutil.copy2(os.path.join(TEMPLATES, 'gtracing.ini'), dest)
    shutil.copy2(os.path.join(TEMPLATES, 'IMPORTANT.TXT'), dest)
    readme = open(os.path.join(TEMPLATES, 'README.txt'), encoding='utf-8').read()
    readme = readme.replace('{PC_MENUS}', PC_MENUS_WITH if with_patch else PC_MENUS_WITHOUT)
    if with_patch:
        readme = readme.replace('the PC menus, see below; empty in the zip)', 'the PC menus, in this copy)  ')
    with open(os.path.join(dest, 'README.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(readme)
    if with_patch:
        src = os.path.join(BUILD, 'patch')
        if not os.path.isdir(src):
            fail('--include-patch: no build/host/patch (run host/build.py first)')
        shutil.copytree(src, os.path.join(dest, 'patch'))
        print('patch/: %d files (Gameloft menu files, changed: this folder is PRIVATE, do not share it)' %
              len(os.listdir(src)))
    else:
        print('patch/: left out (Gameloft menu files; --include-patch for a private copy)')

    # the three folders with their notes (a note only: no game file, no menu file, no add-on)
    for d in NOTE_DIRS:
        os.makedirs(os.path.join(dest, d), exist_ok=True)
        if d == 'patch' and with_patch:
            continue                    # filled: its note says the folder is empty
        for fn in sorted(os.listdir(os.path.join(FOLDERS, d))):
            text = open(os.path.join(FOLDERS, d, fn), encoding='utf-8').read()
            with open(os.path.join(dest, d, fn), 'w', encoding='utf-8', newline='\r\n') as f:
                f.write(text)
    print('folders with a note: %s' % ', '.join(d + '/' for d in NOTE_DIRS))

    # 3. the symbols, outside the release
    symdir = os.path.join(out, 'symbols')
    os.makedirs(symdir, exist_ok=True)
    mp = os.path.join(RELEASE_BUILD, 'gtracing.map')
    if os.path.exists(mp):
        shutil.copy2(mp, os.path.join(symdir, name + '.map'))
        with open(os.path.join(symdir, name + '.txt'), 'w') as f:
            f.write('gtracing.exe as built (before strip) sha256 %s\nthe release exe sha256 %s\n' %
                    (sha256(exe), sha256(rel_exe)))
        print('symbols: %s (not in the release)' % os.path.join(symdir, name + '.map'))

    # 4. the privacy scan
    words = private_words()
    bad = scan_paths([dest], words, base=dest)
    if bad:
        fail('private strings in the release:\n  ' + '\n  '.join(bad))
    print('privacy scan: %d files, %d private strings looked for (8-bit and UTF-16LE): none found' %
          (sum(len(f) for _, _, f in os.walk(dest)), len(words)))

    # 5. a check run of the stripped exe
    if '--no-run' not in args:
        if not os.path.isdir(APP):
            fail('no %s for the check run (or --no-run)' % APP)
        tmp = tempfile.mkdtemp(prefix='gtrel_')
        try:
            ini = os.path.join(tmp, 'check.ini')
            open(ini, 'w').write('GT_FULLSCREEN=0\n')
            env = dict(os.environ)
            for k in list(env):
                if k.upper().startswith('GT_'):
                    del env[k]
            env.update(GT_HOME=os.path.join(tmp, 'home'), GT_LOG=os.path.join(tmp, 'gtracing.log'), GT_INI=ini)
            p = subprocess.run([rel_exe, '--selftest', '--app', APP], cwd=tmp, env=env, timeout=300)
            log = open(os.path.join(tmp, 'gtracing.log'), 'rb').read() if os.path.exists(env['GT_LOG']) else b''
            line = [ln for ln in log.decode('utf-8', 'replace').splitlines() if 'checks failed' in ln]
            if p.returncode or not line or not line[-1].split('[selftest] ')[-1].startswith('0 of'):
                fail('the stripped exe\'s selftest: exit %d, %s' % (p.returncode, line[-1] if line else 'no result'))
            hits = scan_bytes(log, words)
            if hits:
                fail('the check run\'s log names: %s' % ', '.join('"%s" (%s)' % (mask(w), e) for w, e, _ in hits))
            print('check run: %s; its log names no private string' % line[-1].strip())
            left = [f for f in os.listdir(dest) if f not in RELEASE_FILES + NOTE_DIRS]
            if left:
                fail('the check run wrote into the release: %s' % left)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # the folder holds the release files and nothing else (patch/ only with --include-patch)
    found = sorted(os.listdir(dest))
    want = sorted(RELEASE_FILES + NOTE_DIRS)
    if found != want:
        fail('the release folder holds %s, not %s' % (found, want))

    # the checksums last, of what is in the folder now
    with open(os.path.join(dest, 'SHA256SUMS.txt'), 'w', newline='\n') as f:
        for d, _, files in sorted(os.walk(dest)):
            for fn in sorted(files):
                if fn == 'SHA256SUMS.txt':
                    continue
                p = os.path.join(d, fn)
                f.write('%s  %s\n' % (sha256(p), os.path.relpath(p, dest).replace('\\', '/')))

    # the zip: every file under one top folder of the release's name, read back and compared
    if make_zip:
        files = []
        for d, _, fns in sorted(os.walk(dest)):
            for fn in sorted(fns):
                files.append(os.path.relpath(os.path.join(d, fn), dest).replace('\\', '/'))
        with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for rel in files:
                z.write(os.path.join(dest, rel), name + '/' + rel)
        with zipfile.ZipFile(zip_path) as z:
            if z.testzip() is not None:
                fail('the zip does not read back')
            for rel in files:
                if z.read(name + '/' + rel) != open(os.path.join(dest, rel), 'rb').read():
                    fail('the zip\'s %s differs from the folder\'s' % rel)
        print('zip: %s, %d files, %d bytes, sha256 %s' % (zip_path, len(files), os.path.getsize(zip_path),
                                                          sha256(zip_path)))
    print('done: %s' % dest)


if __name__ == '__main__':
    main()
