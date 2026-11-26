import datetime
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, 'build', 'host')
RELEASE_BUILD = os.path.join(BUILD, 'release')
LINUX_RELEASE_BUILD = os.path.join(BUILD, 'linux', 'release')
APP = os.path.join(ROOT, 'GTRacingFreemiumHD.app')
MSYS = r'C:\msys64\mingw32\bin'
STRIP = os.path.join(MSYS, 'strip.exe') if os.name == 'nt' else 'strip'
TEMPLATES = os.path.join(HERE, 'release')
RESERVE_START = 0x11000
RESERVE_END = 0x900000

PC_MENUS_WITH = ("The PC menus are in (patch\\): Options > Display changes the settings above in the game, Options >\n"
                 "Controls shows and changes the keys, the main menu has an Exit tile and the multiplayer page a\n"
                 "Name tile.")
PC_MENUS_WITHOUT = ("This release has the game's own menus: the PC menus (Options > Display, the keys page in Options >\n"
                    "Controls, the Exit and Name tiles) come in the optional patch\\ folder (see above), which is\n"
                    "empty here.  Without its files, change the settings in gtracing.ini.")
RELEASE_FILES = ('gtracing.exe', 'SDL3.dll', 'gtracing.ini', 'README.txt', 'IMPORTANT.TXT')
LINUX_FILES = ('gtracing', 'gtracing.ini', 'README.txt', 'IMPORTANT.TXT')
LINUX_INI = (('read by gtracing.exe at', 'read by gtracing at'),
             ("(scaled with Windows' display scale)", "(scaled with the desktop's display scale)"))
FOLDERS = os.path.join(TEMPLATES, 'folders')
NOTE_DIRS = ('GTRacingFreemiumHD.app', 'patch', 'add-on')


def fail(msg):
    print('FAILED: ' + msg)
    sys.exit(1)


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
        strtab = self.symtab + 18 * self.nsyms if self.symtab else 0
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
        return [x for x in self.sections if not x['name'].lstrip('.').startswith(('debug', 'zdebug')) and
                not x['flags'] & 0x02000000]

    def content(self, sec):
        n = min(sec['vsize'], sec['rsize'])
        return bytes(self.data[sec['rptr']:sec['rptr'] + n]) if sec['rptr'] else b''

    def set_subsystem(self, sub):
        struct.pack_into('<H', self.data, self.opt + 68, sub)
        self.subsystem = sub

    def fix_checksum(self):
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
    if stripped.nsyms:
        fail('the stripped exe still has %d symbols' % stripped.nsyms)
    left = [x['name'] for x in stripped.sections if x not in stripped.mapped()]
    if left:
        fail('the stripped exe still has debug sections: %s' % left)
    return len(b)


class ELF:
    def __init__(self, path):
        self.path = path
        d = self.data = open(path, 'rb').read()
        if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
            fail('%s: not a 32-bit little-endian ELF file' % path)
        (self.type, self.machine, _, self.entry, phoff, shoff, _, _, phsz, phnum, shsz, shnum,
         shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
        self.segments = [struct.unpack_from('<IIIIIIII', d, phoff + i * phsz) for i in range(phnum)]
        raw = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shsz) for i in range(shnum)]
        names = raw[shstrndx] if shnum else None
        self.sections = []
        for n, typ, flags, addr, off, size, _, _, _, _ in raw:
            o = names[4] + n
            name = d[o:d.index(b'\0', o)].decode('latin-1') if names else ''
            self.sections.append(dict(name=name, type=typ, flags=flags, va=addr, off=off, size=size))

    def mapped(self):
        return [x for x in self.sections if x['flags'] & 2]

    def content(self, sec):
        return b'' if sec['type'] == 8 else self.data[sec['off']:sec['off'] + sec['size']]


def check_strip_elf(orig, stripped):
    a, b = orig.mapped(), stripped.mapped()
    if [x['name'] for x in a] != [x['name'] for x in b]:
        fail('strip changed the sections: %s -> %s' % ([x['name'] for x in a], [x['name'] for x in b]))
    for x, y in zip(a, b):
        for k in ('va', 'size', 'flags', 'type'):
            if x[k] != y[k]:
                fail('strip changed %s %s: 0x%x -> 0x%x' % (x['name'], k, x[k], y[k]))
        if orig.content(x) != stripped.content(y):
            fail('strip changed the bytes of %s' % x['name'])
    load = [(t, va, fsz, msz, fl) for t, _, va, _, fsz, msz, fl, _ in orig.segments if t == 1]
    if load != [(t, va, fsz, msz, fl) for t, _, va, _, fsz, msz, fl, _ in stripped.segments if t == 1]:
        fail('strip changed the loaded segments')
    if (orig.entry, orig.type) != (stripped.entry, stripped.type) or stripped.type != 2:
        fail('strip changed the entry point, or the file is not a fixed-address executable')
    g = [x for x in b if x['name'] == '.gtimg']
    if not g or g[0]['va'] != RESERVE_START or g[0]['size'] != RESERVE_END - RESERVE_START or g[0]['type'] != 8:
        fail('the reserved game range is not an empty section at 0x%x..0x%x' % (RESERVE_START, RESERVE_END))
    left = [x['name'] for x in stripped.sections if x['name'] in ('.symtab', '.strtab') or
            x['name'].lstrip('.').startswith(('debug', 'zdebug'))]
    if left:
        fail('the stripped file still has %s' % left)
    return len(b)


def tar_info(t):
    t.uid = t.gid = 0
    t.uname = t.gname = ''
    t.mode = 0o755 if t.isdir() or t.name.endswith('/gtracing') else 0o644
    return t


sys.path.insert(0, HERE)
from privacy_scan import mask, private_words, scan_bytes, scan_paths  # noqa: E402


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
    linux = opt('--platform', 'windows') == 'linux'
    if opt('--platform', 'windows') not in ('windows', 'linux'):
        fail('--platform: windows or linux')
    if not version.replace('.', '').replace('-', '').replace('_', '').isalnum():
        fail('--version: letters, digits, dots and dashes only')
    name = 'GTRacing-PC-Port-%s-%s-x86%s' % (version, 'Linux' if linux else 'Windows', '-PRIVATE' if with_patch else '')
    dest = os.path.join(out, name)
    eol = '\n' if linux else '\r\n'
    sep = '/' if linux else '\\'

    exe = os.path.join(LINUX_RELEASE_BUILD, 'gtracing') if linux else os.path.join(RELEASE_BUILD, 'gtracing.exe')
    template = os.path.join(TEMPLATES, 'README-linux.txt' if linux else 'README.txt')
    needs = [exe, os.path.join(TEMPLATES, 'gtracing.ini'), template, os.path.join(TEMPLATES, 'IMPORTANT.TXT'),
             *[os.path.join(FOLDERS, d) for d in NOTE_DIRS]]
    if not linux:
        needs += [os.path.join(BUILD, 'SDL3.dll'), STRIP]
    for need in needs:
        if not os.path.exists(need):
            fail('missing %s' % need)
    zip_path = dest + ('.tar.gz' if linux else '.zip')
    make_zip = '--zip' in args or '--archive' in args
    if os.path.exists(dest) or (make_zip and os.path.exists(zip_path)):
        if '--force' not in args:
            fail('%s exists (--force replaces it)' % (dest if os.path.exists(dest) else zip_path))
        if os.path.exists(dest):
            shutil.rmtree(dest)
        if make_zip and os.path.exists(zip_path):
            os.remove(zip_path)
    os.makedirs(dest)
    print('release folder: %s' % dest)

    rel_exe = os.path.join(dest, os.path.basename(exe))
    strip = 'strip' if linux and os.name != 'nt' else STRIP
    orig = ELF(exe) if linux else PE(exe)
    p = subprocess.run([strip, '--strip-all', '-o', rel_exe, exe], capture_output=True, text=True)
    if p.returncode:
        fail('strip: %s' % (p.stdout + p.stderr).strip())
    if linux:
        stripped = ELF(rel_exe)
        n = check_strip_elf(orig, stripped)
        os.chmod(rel_exe, 0o755)
        print('gtracing: stripped %.1f -> %.1f MB; %d loaded sections, .gtimg at 0x%x, segments and entry '
              'unchanged' % (os.path.getsize(exe) / 1e6, os.path.getsize(rel_exe) / 1e6, n, RESERVE_START))
    else:
        stripped = PE(rel_exe)
        n = check_strip(orig, stripped)
        if '--console' not in args:
            stripped.set_subsystem(2)
        stripped.fix_checksum()
        stripped.save()
        print('gtracing.exe: stripped %.1f -> %.1f MB; %d mapped sections, .gtimg at 0x%x, entry and directories '
              'unchanged; %s subsystem' % (os.path.getsize(exe) / 1e6, os.path.getsize(rel_exe) / 1e6, n,
                                           RESERVE_START, 'console' if stripped.subsystem == 3 else 'window'))
        shutil.copy2(os.path.join(BUILD, 'SDL3.dll'), dest)

    ini = open(os.path.join(TEMPLATES, 'gtracing.ini'), encoding='utf-8').read()
    for a, b in LINUX_INI if linux else ():
        if a not in ini:
            fail('the gtracing.ini template no longer says "%s"' % a)
        ini = ini.replace(a, b)
    with open(os.path.join(dest, 'gtracing.ini'), 'w', encoding='utf-8', newline=eol) as f:
        f.write(ini.replace('\r\n', '\n'))
    shutil.copy2(os.path.join(TEMPLATES, 'IMPORTANT.TXT'), dest)
    readme = open(template, encoding='utf-8').read()
    menus = PC_MENUS_WITH if with_patch else PC_MENUS_WITHOUT
    readme = readme.replace('{PC_MENUS}', menus.replace('patch\\', 'patch' + sep))
    if with_patch:
        readme = readme.replace('the PC menus, see below; empty in the %s)' % ('tar.gz' if linux else 'zip'),
                                'the PC menus, in this copy)  ' + ('     ' if linux else ''))
    with open(os.path.join(dest, 'README.txt'), 'w', encoding='utf-8', newline=eol) as f:
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

    for d in NOTE_DIRS:
        os.makedirs(os.path.join(dest, d), exist_ok=True)
        if d == 'patch' and with_patch:
            continue
        for fn in sorted(os.listdir(os.path.join(FOLDERS, d))):
            text = open(os.path.join(FOLDERS, d, fn), encoding='utf-8').read()
            if linux:
                text = text.replace('gtracing.exe', 'gtracing').replace('\\', '/')
            with open(os.path.join(dest, d, fn), 'w', encoding='utf-8', newline=eol) as f:
                f.write(text)
    print('folders with a note: %s' % ', '.join(d + '/' for d in NOTE_DIRS))

    symdir = os.path.join(out, 'symbols')
    os.makedirs(symdir, exist_ok=True)
    mp = os.path.join(os.path.dirname(exe), 'gtracing.map')
    if os.path.exists(mp):
        shutil.copy2(mp, os.path.join(symdir, name + '.map'))
        with open(os.path.join(symdir, name + '.txt'), 'w') as f:
            f.write('%s as built (before strip) sha256 %s\nthe release %s sha256 %s\n' %
                    (os.path.basename(exe), sha256(exe), 'program' if linux else 'exe', sha256(rel_exe)))
        print('symbols: %s (not in the release)' % os.path.join(symdir, name + '.map'))

    words = private_words()
    bad = scan_paths([dest], words, base=dest)
    if bad:
        fail('private strings in the release:\n  ' + '\n  '.join(bad))
    print('privacy scan: %d files, %d private strings looked for (8-bit and UTF-16LE): none found' %
          (sum(len(f) for _, _, f in os.walk(dest)), len(words)))

    if '--no-run' not in args:
        if linux and os.name == 'nt':
            fail('the check run of the Linux build needs Linux or WSL (python3 tools/package_release.py there), '
                 'or --no-run')
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
            env.update(GT_HOME=os.path.join(tmp, 'home'), GT_LOG=os.path.join(tmp, 'gtracing.log'), GT_INI=ini,
                       GT_CLOUD_BOARD='0')
            p = subprocess.run([rel_exe, '--selftest', '--app', APP], cwd=tmp, env=env, timeout=300)
            log = open(os.path.join(tmp, 'gtracing.log'), 'rb').read() if os.path.exists(env['GT_LOG']) else b''
            line = [ln for ln in log.decode('utf-8', 'replace').splitlines() if 'checks failed' in ln]
            if p.returncode or not line or not line[-1].split('[selftest] ')[-1].startswith('0 of'):
                fail('the stripped exe\'s selftest: exit %d, %s' % (p.returncode, line[-1] if line else 'no result'))
            hits = scan_bytes(log, words)
            if hits:
                fail('the check run\'s log names: %s' % ', '.join('"%s" (%s)' % (mask(w), e) for w, e, _ in hits))
            print('check run: %s; its log names no private string' % line[-1].strip())
            left = [f for f in os.listdir(dest) if f not in (LINUX_FILES if linux else RELEASE_FILES) + NOTE_DIRS]
            if left:
                fail('the check run wrote into the release: %s' % left)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    found = sorted(os.listdir(dest))
    want = sorted((LINUX_FILES if linux else RELEASE_FILES) + NOTE_DIRS)
    if found != want:
        fail('the release folder holds %s, not %s' % (found, want))

    with open(os.path.join(dest, 'SHA256SUMS.txt'), 'w', newline='\n') as f:
        for d, _, files in sorted(os.walk(dest)):
            for fn in sorted(files):
                if fn == 'SHA256SUMS.txt':
                    continue
                p = os.path.join(d, fn)
                f.write('%s  %s\n' % (sha256(p), os.path.relpath(p, dest).replace('\\', '/')))

    if make_zip and linux:
        files = []
        for d, dirs, fns in sorted(os.walk(dest)):
            for x in sorted(dirs) + sorted(fns):
                files.append(os.path.relpath(os.path.join(d, x), dest).replace('\\', '/'))
        with tarfile.open(zip_path, 'w:gz', compresslevel=9, format=tarfile.PAX_FORMAT) as t:
            t.add(dest, name, recursive=False, filter=tar_info)
            for rel in files:
                t.add(os.path.join(dest, rel), name + '/' + rel, recursive=False, filter=tar_info)
        with tarfile.open(zip_path, 'r:gz') as t:
            for m in t.getmembers():
                if m.uid or m.gid or m.uname or m.gname:
                    fail('the tar.gz names an owner for %s' % m.name)
                if m.isfile() and t.extractfile(m).read() != open(os.path.join(out, m.name), 'rb').read():
                    fail('the tar.gz\'s %s differs from the folder\'s' % m.name)
            mode = t.getmember(name + '/gtracing').mode
            if mode != 0o755:
                fail('gtracing in the tar.gz is not executable (mode %o)' % mode)
        print('tar.gz: %s, %d entries, %d bytes, sha256 %s' % (zip_path, len(files) + 1, os.path.getsize(zip_path),
                                                              sha256(zip_path)))
    elif make_zip:
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
