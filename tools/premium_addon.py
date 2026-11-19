"""PREMIUM_LICENCES + PREMIUM_AUDIO add-ons, built from the paid GT Racing Motor Academy iPad app
(GTRacingiPad.app, Version.txt 1.1.4, older than the free HD 1.3.4b this port runs): the three licence tests the
free build cut, and the paid build's 44 kHz audio.  Gameloft's files: never shipped, never in the repository.

    python -I tools/premium_addon.py [premium .app folder] [add-on folder]

Builds two add-on folders (host/shims/ns_misc.c reads every add-on\\<folder>\\ as bundle files):

add-on\\premium-licences\\ -- the three licence tests the free build cut.
  The game opens every licence<N>.ba3 the bundle lists (seen by GT_TRACE: in name order after the bundle's
  contentsOfDirectoryAtPath).  A file: u32 n, u16 0, (n - 1) x (u16 index, u32 end) relative to the table's
  end, 0xffff; section 0 the record, then one text pack per language (u32 zlib length, u32 text size, zlib,
  padded to 32 bytes), zero bytes to a 32-byte size.  Record: u32 test id, u32 track, u16, u32, u16 test type,
  u8 n + n x u32 medal times, the rest.
  - The premium files (n = 8): 6 languages (EN FR DE ES IT JA).  The free ones (n = 13): 11 (+ KO ZH RU and
    two Portuguese), the type one higher (a type was added before them), and 22 bytes after the times
    (09 05, then 5 words: the free build's rewards; their meaning is not known).
  - Premium has 24 tests (ids 0-23); free has 21: Cornering 2 (id 2), Overtaking 1 (id 3) and Braking 2 (id 4)
    are gone, the free id 3 is premium's Full Lap 1 (premium id 5) and the free build renumbered the titles.
  - Here: licence3 = Cornering 2 (id 2), licence5 = Braking 2 (id 4), licence6 = Overtaking 1 (id 5: the free
    id 3 stays Full Lap 1, so a save's medal stays on its test).  Their 5 new languages: Cornering 2 and Braking 2
    take the free build's Cornering 1 / Braking 1 text with the number changed (the same task), Overtaking 1
    the English (the free texts name other cars).  Rewards: the medal words of a free test of the same type,
    the fourth word (it rises with the id) between the free ids around it, the fifth the smaller of theirs.
    The medal times stay premium's (the free build eased only Braking 1 and Cornering 1's).
  - Which tests a class shows is License.bin (ElciNFi.TlN), not the files: loading the three alone left class C
    at three tests.  Class C here: ids 0 1 2 5 4 3, premium's order.  The save keeps
    medals by place, so host/games/gtracing/features/licences.c turns the places the game asks for into the free build's (a save's
    Full Lap 1 medal stays on Full Lap 1, the save keeps the free layout).
  - A class is passed when all its tests have a medal (0x2659cc: the smallest): with the three new tests not
    yet driven, class C is not passed and the game sends the player to the licence office before events.
  - The free tests whose numbers moved get premium's numbers back (Cornering 2-5 -> 3-6, Braking 2-4 -> 3-5,
    Overtaking 1-3 -> 2-4), files of the same name as the bundle's, read instead of them.
  - The premium texts' Spanish / French / Italian letters the free build corrected are corrected the same way
    (the replacements are learnt from the tests both builds have).

add-on\\premium-audio\\ -- the premium build's sounds (.ba6 WAV) where they are better than the free build's: the
  19 music tracks (IMA ADPCM stereo) 44100 Hz there, 32000 Hz in free, and 102 effects, mostly the engines
  (PCM mono), 22050 Hz there, 16000 Hz in free.  Sounds of the same rate in both (46) and the free build's own
  sounds stay.  host/shims/al.c plays every buffer at the rate the game gives it.
"""
import os
import re
import shutil
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREMIUM = os.path.join(ROOT, 'GTRacingiPad.app')
FREE = os.path.join(ROOT, 'GTRacingFreemiumHD.app')
OUT = os.path.join(ROOT, 'build', 'host', 'add-on')

CODE = dict(a='BU', b='BW', c='C4', d='DS', e='EZ', f='FA', g='GO', h='HI', i='K0', j='LO', k='MP', l='NN', m='QY',
            n='RA', o='RE', p='RO', q='SI', r='TA', s='UK', t='VW', u='WU', v='WV', w='XA', x='Y3', y='ZX', z='ZZ',
            **{'0': 'A1', '1': 'AA', '2': 'AD', '3': 'AF', '4': 'AZ', '5': 'B0', '6': 'BB', '7': 'BR', '8': 'BS',
               '9': 'BT', '_': 'UT'})


def enc(name):
    """a bundle name in the two-letter code: "licence3" -> NNK0C4EZRAC4EZAF"""
    return ''.join(CODE[c] for c in name)


def lic_name(n):
    return enc('licence%d' % n) + '.ba3'


# License.bin (the bundle's one-letter code: ElciNFi.TlN), read by 0x2759ec into the licence manager +0x240:
# per class (C B A S) 6 words, u8 n, n test ids -- the tests the class shows, in that order (0x27481c: class,
# index -> id -> the test, found by id).  The free build's class C is ids 0 1 3.
CLASSES = 'ElciNFi.TlN'
# Premium's order (Full Lap 1 at the end).  The save keeps a test's medal by its place in the class, not its id
# (this order showed a save's Full Lap 1 gold on Cornering 2): host/games/gtracing/features/licences.c keeps the
# medals at the free build's places.
CLASS_C = [0, 1, 2, 5, 4, 3]     # Braking 1, Cornering 1, Cornering 2, Overtaking 1, Braking 2, Full Lap 1


def classes_parse(b):
    out, k = [], 0
    for _ in range(4):
        w = b[k:k + 24]
        n = b[k + 24]
        out.append((w, list(struct.unpack_from('<%di' % n, b, k + 25))))
        k += 25 + 4 * n
    return out, b[k:]


def classes_build(classes):
    b = b''.join(w + bytes([len(ids)]) + struct.pack('<%di' % len(ids), *ids) for w, ids in classes)
    return b + b'\0' * (-len(b) % 32 or 8)


# licence files

def lic_parse(b):
    """[record, text pack per language]"""
    n = struct.unpack_from('<I', b)[0]
    k, ends = 6, []
    for _ in range(n - 1):
        idx, end = struct.unpack_from('<HI', b, k)
        k += 6
        ends.append(end)
    if struct.unpack_from('<H', b, k)[0] != 0xffff:
        raise ValueError('licence table')
    k += 2
    secs, prev = [], 0
    for e in ends:
        secs.append(b[k + prev:k + e])
        prev = e
    return secs


def lic_build(secs):
    tab = struct.pack('<IH', len(secs) + 1, 0)
    end = 0
    for i, s in enumerate(secs):
        end += len(s)
        tab += struct.pack('<HI', i, end)
    b = tab + b'\xff\xff' + b''.join(secs)
    return b + b'\0' * (-len(b) % 32)


def text(sec):
    zl, size = struct.unpack_from('<II', sec)
    t = zlib.decompress(sec[8:8 + zl])
    if len(t) != size:
        raise ValueError('text pack size')
    return t


def text_sec(t):
    z = zlib.compress(t, 9)
    s = struct.pack('<II', len(z), len(t)) + z
    return s + b'\0' * (-len(s) % 32)


def strings(t):
    """the text pack's header and its strings (title, description): utf-16 after 20 bytes"""
    return t[:20], t[20:].decode('utf-16-le').split('\0')


def title(t):
    return strings(t)[1][0]


def renumber(t, num):
    """the title's number made num (same length: one digit for one digit)"""
    h, s = strings(t)
    m = re.findall(r'\d+', s[0])
    if len(m) != 1 or len(m[0]) != len(str(num)):
        raise ValueError('title %r: no single number' % s[0])
    s[0] = re.sub(r'\d+', str(num), s[0])
    return h + '\0'.join(s).encode('utf-16-le')


def rec_fields(r):
    tid, trk = struct.unpack_from('<II', r)
    typ = struct.unpack_from('<H', r, 14)[0]
    n = r[16]
    return tid, trk, typ, 17 + 4 * n


def rec_to_free(r, tid, rewards):
    """a premium record as the free build has it: id, the type + 1, the 22 reward bytes after the times"""
    _, _, typ, after = rec_fields(r)
    r = bytearray(r)
    struct.pack_into('<I', r, 0, tid)
    struct.pack_into('<H', r, 14, typ + 1)
    return bytes(r[:after]) + rewards + bytes(r[after:])


def learn_fixes(pairs):
    """per language: the utf-16 units the free build changed in the premium texts (same-length strings)"""
    seen = {}
    for p, f in pairs:
        for lang in range(6):
            a, b = text(p[1 + lang]), text(f[1 + lang])
            if len(a) != len(b):
                continue
            for k in range(20, len(a) - 1, 2):
                u, v = a[k:k + 2], b[k:k + 2]
                if u != v and not (u[1] == 0 and chr(u[0]).isdigit()):     # not the renumbered titles
                    seen.setdefault(lang, {}).setdefault(u, set()).add(v)
    # a unit made into one other unit everywhere it changed
    return {lang: {u: next(iter(v)) for u, v in m.items() if len(v) == 1} for lang, m in seen.items()}


def fix_text(t, fx):
    out = bytearray(t)
    for k in range(20, len(t) - 1, 2):
        u = bytes(t[k:k + 2])
        if u in fx:
            out[k:k + 2] = fx[u]
    return bytes(out)


def licences(prem, free, out):
    P = {}
    for n in range(1, 25):
        p = os.path.join(prem, lic_name(n))
        if os.path.exists(p):
            P[n] = lic_parse(open(p, 'rb').read())
    F = {}
    for n in range(1, 25):
        p = os.path.join(free, lic_name(n))
        if os.path.exists(p):
            F[n] = lic_parse(open(p, 'rb').read())
    if len(P) != 24 or sorted(F) != [1, 2, 4] + list(range(7, 25)):
        raise ValueError('licences: premium %d files, free %s' % (len(P), sorted(F)))
    for d, n in [(prem, k) for k in P] + [(free, k) for k in F]:     # the file layout as read here
        b = open(os.path.join(d, lic_name(n)), 'rb').read()
        if lic_build(lic_parse(b)) != b:
            raise ValueError('%s: the licence layout does not hold' % lic_name(n))
    p_by_id = {rec_fields(s[0])[0]: s for s in P.values()}
    # the free tests and their premium twin (same id; free id 3 = premium id 5, Full Lap 1)
    twin = {}
    for n, s in F.items():
        tid = rec_fields(s[0])[0]
        twin[n] = p_by_id[5 if tid == 3 else tid]
    # check the record rule on every test both builds have (the medal times may differ: the free build eased
    # some, Braking 1 and Cornering 1)
    eased = []
    for n, s in F.items():
        r = s[0]
        _, _, _, after = rec_fields(r)
        rew = r[after:after + 22]
        want = rec_to_free(twin[n][0], rec_fields(r)[0], rew)
        if want[:17] + want[after:] != r[:17] + r[after:] or len(s) != 12 or len(twin[n]) != 7:
            raise ValueError('licence%d: the record rule does not hold' % n)
        if want != r:
            eased.append(n)
    print('licences: the rule holds on all %d tests both builds have (other medal times: licence%s)'
          % (len(F), ', '.join(map(str, eased))))
    fixes = learn_fixes([(twin[n], F[n]) for n in F])
    by_type = {}
    for n, s in F.items():
        by_type.setdefault(rec_fields(s[0])[2], []).append(n)

    def reward_words(n):
        r = F[n][0]
        after = rec_fields(r)[3]
        if r[after:after + 2] != b'\x09\x05':
            raise ValueError('licence%d: rewards' % n)
        return struct.unpack_from('<5i', r, after + 2)

    by_id = {rec_fields(s[0])[0]: n for n, s in F.items()}

    def rewards(tid, like):
        """the three medal words of the free test of the same type; the fourth (rising with the id) between
        the free ids around it; the fifth the smaller of theirs"""
        lo = max(i for i in by_id if i < tid)
        hi = min(i for i in by_id if i > tid)
        wl, wh = reward_words(by_id[lo]), reward_words(by_id[hi])
        d = round(wl[3] + (wh[3] - wl[3]) * (tid - lo) / (hi - lo))
        w = reward_words(like)
        return b'\x09\x05' + struct.pack('<5i', w[0], w[1], w[2], d, min(wl[4], wh[4]))

    odir = os.path.join(out, 'premium-licences')
    os.makedirs(odir, exist_ok=True)
    for f in os.listdir(odir):
        os.remove(os.path.join(odir, f))
    # (file here, premium id, id here, free test whose other-language texts / rewards it takes, or None: English)
    new = [(3, 2, 2, 2, 2), (5, 4, 4, 1, 8), (6, 3, 5, None, 10)]
    for n, pid, tid, text_from, rew_from in new:
        p = p_by_id[pid]
        secs = [rec_to_free(p[0], tid, rewards(tid, rew_from))]
        for lang in range(6):
            secs.append(text_sec(fix_text(text(p[1 + lang]), fixes.get(lang, {}))))
        num = int(re.findall(r'\d+', title(text(p[1])))[0])
        for lang in range(6, 11):
            if text_from:
                secs.append(text_sec(renumber(text(F[text_from][1 + lang]), num)))
            else:
                secs.append(secs[1])
        open(os.path.join(odir, lic_name(n)), 'wb').write(lic_build(secs))
        print('licence%d: %s (id %d, track %d)' % (n, title(text(p[1])), tid, rec_fields(p[0])[1]))
    # the class lists: class C shows the six tests, in premium's order
    cls_path = os.path.join(free, CLASSES)
    cb = open(cls_path, 'rb').read()
    classes, rest = classes_parse(cb)
    if classes_build(classes) != cb:
        raise ValueError('%s: the layout does not hold' % CLASSES)
    if classes[0][1] != [0, 1, 3]:
        raise ValueError('%s: class C is %s' % (CLASSES, classes[0][1]))
    classes[0] = (classes[0][0], CLASS_C)
    open(os.path.join(odir, CLASSES), 'wb').write(classes_build(classes))
    print('%s: class C tests %s (ids)' % (CLASSES, CLASS_C))
    # the free tests back to premium's numbers
    for n, s in sorted(F.items()):
        want = int(re.findall(r'\d+', title(text(twin[n][1])))[0])
        have = int(re.findall(r'\d+', title(text(s[1])))[0])
        if want == have:
            continue
        secs = [s[0]] + [text_sec(renumber(text(t), want)) for t in s[1:]]
        open(os.path.join(odir, lic_name(n)), 'wb').write(lic_build(secs))
        print('licence%d: %s -> %d' % (n, title(text(s[1])), want))
    return 0


# audio

def wav_rate(path):
    with open(path, 'rb') as f:
        h = f.read(64)
    if h[:4] != b'RIFF' or h[8:16] != b'WAVEfmt ':
        return None
    return struct.unpack_from('<I', h, 24)[0]


def audio(prem, free, out):
    odir = os.path.join(out, 'premium-audio')
    os.makedirs(odir, exist_ok=True)
    keep, size, rates = set(), 0, {}
    for f in sorted(os.listdir(prem)):
        if not f.endswith('.ba6') or not os.path.exists(os.path.join(free, f)):
            continue
        src = os.path.join(prem, f)
        a, b = wav_rate(src), wav_rate(os.path.join(free, f))
        if a is None or b is None or a <= b:          # the same rate in both builds: the free one stays
            continue
        dst = os.path.join(odir, f)
        if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src):
            shutil.copyfile(src, dst)
        keep.add(f)
        size += os.path.getsize(src)
        rates['%d -> %d Hz' % (b, a)] = rates.get('%d -> %d Hz' % (b, a), 0) + 1
    for f in os.listdir(odir):
        if f not in keep:
            os.remove(os.path.join(odir, f))
    print('premium-audio: %d sounds, %.1f MB (%s)' % (len(keep), size / 1e6,
                                                       ', '.join('%d x %s' % (v, k) for k, v in rates.items())))
    return 0


def main():
    prem = sys.argv[1] if len(sys.argv) > 1 else PREMIUM
    out = sys.argv[2] if len(sys.argv) > 2 else OUT
    if not os.path.isdir(prem):
        print('no premium GT Racing at %s' % prem)
        return 1
    return licences(prem, FREE, out) or audio(prem, FREE, out)


if __name__ == '__main__':
    sys.exit(main())
