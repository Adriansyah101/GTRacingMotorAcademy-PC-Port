import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBB = os.path.join(ROOT, 'com.gameloft.android.ANMP.GloftGTHY', 'main.100.com.gameloft.android.ANMP.GloftGTHY.obb')
OUT = os.path.join(ROOT, 'build', 'host', 'add-on', 'hyundai')

CARS = [
    ('HIZXWURADSBUK0GOEZRAEZUKK0UKC4REWUROEZADA1AAAF', 342, 503, (36300, 36301, 36350)),
    ('HIZXWURADSBUK0K0RERAK0SIADA1AAAF', 343, 504, (36400, 36401, 36450, 36452)),
]
PACK = 'HIZXWURADSBUK0'
HIRES = 'HIK0TAEZUK'


def obb_files(path):
    f = open(path, 'rb')
    n, = struct.unpack('<I', f.read(4))
    ents = {}
    for _ in range(n):
        name = f.read(256).split(b'\0')[0].decode('latin1')
        size, off = struct.unpack('<II', f.read(8))
        ents[name] = (off, size)

    def read(name):
        off, size = ents[name]
        f.seek(off)
        return f.read(size)
    return ents, read


def pack_read(b, wide):
    n, = struct.unpack_from('<H', b, 0)
    es = [struct.unpack_from('<II' if wide else '<IH', b, 2 + (8 if wide else 6) * i) for i in range(n)]
    hdr = 2 + (8 if wide else 6) * n
    out = {}
    for i, (off, id_) in enumerate(es[:-1]):
        out[id_] = b[hdr + off:hdr + es[i + 1][0]]
    return out


def pack_write(items, wide):
    items = sorted(items)
    n = len(items) + 1
    out = bytearray(struct.pack('<H', n))
    off = 0
    for id_, d in items:
        out += struct.pack('<II' if wide else '<IH', off, id_)
        off += len(d)
    out += struct.pack('<II' if wide else '<IH', off, 0xffffffff if wide else 0xffff)
    for _, d in items:
        out += d
    return bytes(out)


ETC_TAB = ((2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183))


def clamp(v):
    return 0 if v < 0 else 255 if v > 255 else v


def etc1_decode(data, w, h):
    px = bytearray(w * h * 4)
    bw = (w + 3) // 4
    for bi in range(len(data) // 8):
        bx, by = bi % bw, bi // bw
        if by * 4 >= h:
            break
        hi, lo = struct.unpack_from('>II', data, bi * 8)
        flip, diff = hi & 1, (hi >> 1) & 1
        if diff:
            def ext5(v): return (v << 3) | (v >> 2)

            def sgn3(v): return v - 8 if v & 4 else v
            r, g, b = hi >> 27, (hi >> 19) & 31, (hi >> 11) & 31
            c1 = (ext5(r), ext5(g), ext5(b))
            c2 = (ext5((r + sgn3((hi >> 24) & 7)) & 31), ext5((g + sgn3((hi >> 16) & 7)) & 31),
                  ext5((b + sgn3((hi >> 8) & 7)) & 31))
        else:
            c1 = tuple(((hi >> s) & 15) * 17 for s in (28, 20, 12))
            c2 = tuple(((hi >> s) & 15) * 17 for s in (24, 16, 8))
        t1, t2 = ETC_TAB[(hi >> 5) & 7], ETC_TAB[(hi >> 2) & 7]
        for x in range(4):
            for y in range(4):
                i = x * 4 + y
                idx = ((lo >> (16 + i)) & 1) << 1 | ((lo >> i) & 1)
                second = (y >= 2) if flip else (x >= 2)
                c, t = (c2, t2) if second else (c1, t1)
                m = (t[0], t[1], -t[0], -t[1])[idx]
                X, Y = bx * 4 + x, by * 4 + y
                if X < w and Y < h:
                    o = (Y * w + X) * 4
                    px[o:o + 4] = bytes((clamp(c[0] + m), clamp(c[1] + m), clamp(c[2] + m), 255))
    return bytes(px)


def tex_entry(inner_header_and_data, fmt):
    z = zlib.compress(inner_header_and_data, 9)
    size = len(inner_header_and_data) - 12
    e = struct.pack('<II', len(z), size | fmt) + z
    return e + b'\0' * (-len(e) % 32)


def tex_for_ipad(entry):
    zl, x = struct.unpack_from('<II', entry, 0)
    fmt = x & 0xff
    if fmt == 0x0c:
        return entry, 'RGBA as is'
    if fmt != 0x3c:
        raise ValueError('texture format %#x' % fmt)
    inner = zlib.decompress(entry[8:8 + zl])
    hsz, h, w, mips, flags, dsz = struct.unpack_from('<6I', inner, 8)
    if hsz != 0x34 or inner[8 + 44:8 + 48] != b'PVR!':
        raise ValueError('not a PVR v2 texture (header %d)' % hsz)
    data = inner[8 + hsz:]
    kind = flags & 0xff
    if kind == 0x36:
        rgba, what = etc1_decode(data[:w * h // 2], w, h), 'ETC1'
    elif kind == 0x10:
        px = struct.unpack_from('<%dH' % (w * h), data, 0)
        rgba = bytes(c for v in px for c in ((v >> 12) * 17, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17))
        what = 'RGBA4444'
    elif kind == 0x12:
        rgba, what = data[:w * h * 4], 'RGBA8888'
    else:
        raise ValueError('PVR pixel type %#x' % kind)
    return tex_entry(struct.pack('<HHHHI', w, h, 8, 1, len(rgba)) + rgba, 0x0c), '%s %dx%d -> RGBA' % (what, w, h)


def car_parse(b):
    n, = struct.unpack_from('<H', b, 0)
    hdr = 2 + 6 * n
    offs = [struct.unpack_from('<I', b, 2 + 6 * k)[0] for k in range(n)]
    return [b[hdr + offs[k]:hdr + (offs[k + 1] if k + 1 < n else len(b) - hdr)] for k in range(n)]


def car_build(secs):
    out = bytearray(struct.pack('<H', len(secs)))
    off = 0
    for k, s in enumerate(secs):
        out += struct.pack('<IH', off, k)
        off += len(s)
    for s in secs:
        out += s
    return bytes(out)


IPAD_LANGS = 11

FLAG_KOREA = (0x2a, 0x40, 0x2a, 0x20)
SPRITE_RECTS = {4: (20003, (393, 127, 112, 57)), 5: (20004, (785, 10, 224, 114)), 7: (20002, FLAG_KOREA)}


def sprite_to_ipad(s, tex, rect):
    s = bytearray(s)
    if len(s) != 42 or s[:2] != b'\xdf\x03' or struct.unpack_from('<H', s, 6)[0] != tex:
        raise ValueError('sprite of an unknown form: %s' % s.hex())
    struct.pack_into('<4H', s, 12, *rect)
    s[36], s[37] = rect[2], rect[3]
    return bytes(s)


def car_to_c(a, model_id, fix=None):
    secs = car_parse(a)
    rec = secs[0]
    if rec[0] != 0x41 or len(rec) != 261 or len(secs) < 10:
        raise ValueError('not a version A car file (%#x, %d bytes, %d sections)' % (rec[0], len(rec), len(secs)))
    rec = bytearray(b'C' + rec[1:149] + struct.pack('<ii', 1, -1) + rec[149:])
    struct.pack_into('<I', rec, 1 + 5 * 4, model_id)
    langs = secs[8:-1]
    if fix is None:
        for k, (tex, rect) in SPRITE_RECTS.items():
            secs[k] = sprite_to_ipad(secs[k], tex, rect)
    else:
        fix(secs, rec)
    out = [bytes(rec)] + secs[1:8] + [langs[k] if k < len(langs) else langs[0] for k in range(IPAD_LANGS)] + [secs[-1]]
    return car_build(out), struct.unpack_from('<7i', rec, 1)


def main():
    obb = sys.argv[1] if len(sys.argv) > 1 else OBB
    out = sys.argv[2] if len(sys.argv) > 2 else OUT
    if not os.path.exists(obb):
        print('no OBB at %s' % obb)
        return 1
    ents, read = obb_files(obb)
    os.makedirs(out, exist_ok=True)
    objs = {}
    texs = {}
    for v in ('', HIRES):
        o = pack_read(read('REBWLOEZC4VWUK%s.ba5' % v), True)
        t = pack_read(read('VWEZY3VWWUTAEZUK%s.ba4' % v), False)
        objs[v] = [(new, o[old]) for _, old, new, _ in CARS]
        texs[v] = []
        for _, _, _, own in CARS:
            for tid in own:
                e, how = tex_for_ipad(t[tid])
                texs[v].append((tid, e))
                print('texture %d%s: %s' % (tid, ' (hires)' if v else '', how))
    for v in ('', HIRES):
        for ext, items, wide in (('ba5', objs[v], True), ('ba4', texs[v], False)):
            p = os.path.join(out, '%s%s.%s' % (PACK, v, ext))
            open(p, 'wb').write(pack_write(items, wide))
            print('%s: %d entries, %d bytes' % (os.path.basename(p), len(items), os.path.getsize(p)))
    for name, old, new, _ in CARS:
        c, f = car_to_c(read(name + '.ba0'), new)
        open(os.path.join(out, name + '.ba01'), 'wb').write(c)
        for ext in ('ba7', 'ba8'):
            open(os.path.join(out, '%s.%s' % (name, ext)), 'wb').write(read('%s.%s' % (name, ext)))
        print('%s: car %d, brand %d, year %d, model %d -> %d' % (name, f[0], f[1], f[2], old, f[5]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
