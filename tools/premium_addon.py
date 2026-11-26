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
    return ''.join(CODE[c] for c in name)


def lic_name(n):
    return enc('licence%d' % n) + '.ba3'


CLASSES = 'ElciNFi.TlN'
CLASS_C = [0, 1, 2, 5, 4, 3]


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


def lic_parse(b):
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
    return t[:20], t[20:].decode('utf-16-le').split('\0')


def title(t):
    return strings(t)[1][0]


def renumber(t, num):
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
    _, _, typ, after = rec_fields(r)
    r = bytearray(r)
    struct.pack_into('<I', r, 0, tid)
    struct.pack_into('<H', r, 14, typ + 1)
    return bytes(r[:after]) + rewards + bytes(r[after:])


def learn_fixes(pairs):
    seen = {}
    for p, f in pairs:
        for lang in range(6):
            a, b = text(p[1 + lang]), text(f[1 + lang])
            if len(a) != len(b):
                continue
            for k in range(20, len(a) - 1, 2):
                u, v = a[k:k + 2], b[k:k + 2]
                if u != v and not (u[1] == 0 and chr(u[0]).isdigit()):
                    seen.setdefault(lang, {}).setdefault(u, set()).add(v)
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
    for d, n in [(prem, k) for k in P] + [(free, k) for k in F]:
        b = open(os.path.join(d, lic_name(n)), 'rb').read()
        if lic_build(lic_parse(b)) != b:
            raise ValueError('%s: the licence layout does not hold' % lic_name(n))
    p_by_id = {rec_fields(s[0])[0]: s for s in P.values()}
    twin = {}
    for n, s in F.items():
        tid = rec_fields(s[0])[0]
        twin[n] = p_by_id[5 if tid == 3 else tid]
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
    for n, s in sorted(F.items()):
        want = int(re.findall(r'\d+', title(text(twin[n][1])))[0])
        have = int(re.findall(r'\d+', title(text(s[1])))[0])
        if want == have:
            continue
        secs = [s[0]] + [text_sec(renumber(text(t), want)) for t in s[1:]]
        open(os.path.join(odir, lic_name(n)), 'wb').write(lic_build(secs))
        print('licence%d: %s -> %d' % (n, title(text(s[1])), want))
    return 0


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
        if a is None or b is None or a <= b:
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
