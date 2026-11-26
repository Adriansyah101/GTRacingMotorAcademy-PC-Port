"""Privacy scan: the files given must not name this PC, its user or the folders they are in.

usage: python -I tools/privacy_scan.py [--quiet] [--release] FILE_OR_FOLDER...

Each file (a folder: every file in it; a .zip or .tar.gz: every file in it too) is searched, any case, in UTF-8,
the ANSI code page and UTF-16LE, for:
  - the user's name (USERNAME, USER, LOGNAME), the PC's name (COMPUTERNAME and the network host name) and the
    user's folder (USERPROFILE, HOME);
  - the name and e-mail address Git commits with on this PC (git config user.name / user.email), when Git is there;
  - "C:\\Users" (either slash);
  - the project's folder, and each folder name on its path that is not a generic one (Users, Documents ...).
These are read when the scan runs and are never written anywhere.  The project's public repository name,
<its owner>/GTRacingMotorAcademy-PC-Port (in GitHub and badge addresses of the documents), the owner's profile
link ([<owner>](https://github.com/<owner>)) and the licence's "Copyright (c) 2026 <owner>" line are allowed.  A
hit prints the file, the encoding and the count, with the string masked (its first letter, then one * per
letter), so the output itself names nothing; the exit code is then 1.

--release adds what a release is made of: tools/release/ (the README.txt templates, IMPORTANT.TXT, the
gtracing.ini template, the empty folders' notes) and, in build/release/, each release folder, its zip or tar.gz
and the release notes.  Left out there: ...-PRIVATE folders (they hold Gameloft's menu files and are never
shared), symbols/ (the linker maps, which name the build's folders by design and stay out of the release) and
old/.

Used by tools/playtest.ps1 (both builds' exes, unstripped, the minidump and log of a forced crash, and --release) and
by tools/package_release.py (every file of the release, and the log of its check run).
"""
import os
import socket
import subprocess
import sys
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = 'GTRacingMotorAcademy-PC-Port'
GENERIC = {'users', 'documents', 'desktop', 'downloads', 'onedrive', 'appdata', 'program files', 'home', 'build'}


def git_config(key):
    try:
        r = subprocess.run(['git', 'config', '--get', key], capture_output=True, text=True, timeout=10)
        return r.stdout.strip() if r.returncode == 0 else ''
    except (OSError, subprocess.SubprocessError):
        return ''


def public_forms():
    owner = git_config('user.name')
    if len(owner) < 3:
        return []
    return ['%s/%s' % (owner, REPO), '[%s](https://github.com/%s)' % (owner, owner),
            'Copyright (c) 2026 %s' % owner]


def private_words():
    words = {'C:\\Users'}
    found = [os.environ.get(k, '') for k in ('USERNAME', 'COMPUTERNAME', 'USERPROFILE', 'USER', 'LOGNAME', 'HOME')]
    try:
        found.append(socket.gethostname())
    except OSError:
        pass
    found += [git_config('user.name'), git_config('user.email')]
    for v in found:
        if len(v) >= 3 and v.strip('/\\'):
            words.add(v)
    words.add(ROOT)
    for p in os.path.normpath(ROOT).split(os.sep)[1:]:
        if len(p) >= 3 and p.lower() not in GENERIC:
            words.add(p)
    for v in list(words):
        if '\\' in v:
            words.add(v.replace('\\', '/'))
    return sorted(words, key=str.lower)


def encodings(w):
    forms = {('utf-8', w.encode('utf-8').lower()), ('utf-16le', w.encode('utf-16-le').lower())}
    try:
        forms.add(('ansi', w.encode('mbcs').lower()))
    except (LookupError, UnicodeEncodeError):
        pass
    return forms


PUBLIC = None


def scan_bytes(data, words):
    global PUBLIC
    if PUBLIC is None:
        PUBLIC = [needle for f in public_forms() for _, needle in encodings(f)]
    low = data.lower()
    for needle in PUBLIC:
        low = low.replace(needle, b'')
    hits = []
    for w in words:
        for enc, needle in encodings(w):
            n = low.count(needle)
            if n:
                hits.append((w, enc, n))
    return hits


def mask(w):
    return w[:1] + '*' * (len(w) - 1)


def files_of(paths):
    for p in paths:
        if os.path.isdir(p):
            for d, _, names in os.walk(p):
                for f in names:
                    yield os.path.join(d, f)
        else:
            yield p


def contents(f):
    with open(f, 'rb') as fp:
        data = fp.read()
    out = [(f, data)]
    if f.lower().endswith('.zip'):
        with zipfile.ZipFile(f) as z:
            out += [(f + '!' + i.filename, z.read(i)) for i in z.infolist() if not i.is_dir()]
    elif f.lower().endswith(('.tar.gz', '.tgz')):
        with tarfile.open(f, 'r:gz') as t:
            for m in t.getmembers():
                out.append((f + '!' + m.name + ' (owner)', ('%s %s' % (m.uname, m.gname)).encode()))
                if m.isfile():
                    out.append((f + '!' + m.name, t.extractfile(m).read()))
    return out


def scan_paths(paths, words, base=None):
    bad = []
    for f in files_of(paths):
        for name, data in contents(f):
            for w, enc, n in scan_bytes(data, words):
                bad.append('%s: "%s" (%s) x%d' % (os.path.relpath(name, base) if base else name, mask(w), enc, n))
    return bad


def release_paths():
    paths = [os.path.join(HERE, 'release')]
    out = os.path.join(ROOT, 'build', 'release')
    if os.path.isdir(out):
        for n in sorted(os.listdir(out)):
            if n in ('symbols', 'old') or 'PRIVATE' in n.upper():
                continue
            paths.append(os.path.join(out, n))
    return paths


def main():
    args = [a for a in sys.argv[1:] if a not in ('--quiet', '--release')]
    quiet = '--quiet' in sys.argv[1:]
    if '--release' in sys.argv[1:]:
        args += release_paths()
    if not args:
        print(__doc__.strip())
        return 2
    missing = [p for p in args if not os.path.exists(p)]
    if missing:
        print('privacy scan: no such file: %s' % ', '.join(missing))
        return 2
    words = private_words()
    files = list(files_of(args))
    bad = scan_paths(args, words)
    for b in bad:
        print('privacy scan: ' + b)
    if not bad and not quiet:
        print('privacy scan: %d file(s), %d private strings looked for (UTF-8, ANSI, UTF-16LE): none found' %
              (len(files), len(words)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
