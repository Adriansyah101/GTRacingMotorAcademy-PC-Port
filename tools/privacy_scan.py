"""Privacy scan: the files given must not name this PC, its Windows user or the folders they are in.

usage: python -I tools/privacy_scan.py [--quiet] [--release] FILE_OR_FOLDER...

Each file (a folder: every file in it; a .zip: every file in it too) is searched, any case, in UTF-8, the ANSI code
page and UTF-16LE, for:
  - the Windows user's name (USERNAME), the PC's name (COMPUTERNAME and the network host name) and the Windows
    user's folder (USERPROFILE);
  - the name and e-mail address Git commits with on this PC (git config user.name / user.email), when Git is there;
  - "C:\\Users" (either slash);
  - the project's folder, and each folder name on its path that is not a generic one (Users, Documents ...).
These are read when the scan runs and are never written anywhere.  The project's public repository name,
<its owner>/GTRacingMotorAcademyRecomp (in GitHub and badge addresses of the README and the release notes), is
allowed.  A hit prints the file, the encoding and the count, with the string masked (its first letter, then one * per letter), so the output itself names nothing; the
exit code is then 1.

--release adds what a release is made of: tools/release/ (README.txt, IMPORTANT.TXT, the gtracing.ini template, the
empty folders' notes) and, in build/release/, each release folder, its zip and the release notes.  Left out there:
...-PRIVATE folders (they hold Gameloft's menu files and are never shared), symbols/ (the linker maps, which name
the build's folders by design and stay out of the release) and old/.

Used by tools/playtest.ps1 (both builds' exes, unstripped, the minidump and log of a forced crash, and --release) and
by tools/package_release.py (every file of the release, and the log of its check run).
"""
import os
import socket
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = 'GTRacingMotorAcademyRecomp'     # the project's public repository: <Git user.name>/<REPO> on GitHub
GENERIC = {'users', 'documents', 'desktop', 'downloads', 'onedrive', 'appdata', 'program files', 'home', 'build'}


def git_config(key):
    try:
        r = subprocess.run(['git', 'config', '--get', key], capture_output=True, text=True, timeout=10)
        return r.stdout.strip() if r.returncode == 0 else ''
    except (OSError, subprocess.SubprocessError):
        return ''


def public_forms():
    """what may hold a private word and is public by design: the project's repository name (its owner's name)"""
    owner = git_config('user.name')
    return ['%s/%s' % (owner, REPO)] if len(owner) >= 3 else []


def private_words():
    """the strings no shipped file, log or dump may hold"""
    words = {'C:\\Users'}
    found = [os.environ.get(k, '') for k in ('USERNAME', 'COMPUTERNAME', 'USERPROFILE')]
    try:
        found.append(socket.gethostname())
    except OSError:
        pass
    found += [git_config('user.name'), git_config('user.email')]
    for v in found:
        if len(v) >= 3:
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
    """[(word, encoding, count)] of every word found, any case, 8-bit (UTF-8 and the ANSI code page) and UTF-16LE;
    the public forms (public_forms) are taken out first"""
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
    """a private word as printed: its first letter and its length"""
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
    """[(name, bytes)]: the file itself, and each member of a .zip (stored compressed, so not seen in its bytes)"""
    with open(f, 'rb') as fp:
        data = fp.read()
    out = [(f, data)]
    if f.lower().endswith('.zip'):
        with zipfile.ZipFile(f) as z:
            out += [(f + '!' + i.filename, z.read(i)) for i in z.infolist() if not i.is_dir()]
    return out


def scan_paths(paths, words, base=None):
    """['<file>: "<masked>" (<encoding>) x<count>', ...] for every hit in the files of paths"""
    bad = []
    for f in files_of(paths):
        for name, data in contents(f):
            for w, enc, n in scan_bytes(data, words):
                bad.append('%s: "%s" (%s) x%d' % (os.path.relpath(name, base) if base else name, mask(w), enc, n))
    return bad


def release_paths():
    """tools/release/ and the shippable part of build/release/ (see the module's text)"""
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
