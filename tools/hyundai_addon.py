"""HYUNDAI_CARS add-on: the Hyundai edition's two cars as an add-on folder (as in the Gangstar port) that
the iPad game reads.

Builds build\\host\\add-on\\hyundai\\ from the Android GT Racing: Hyundai Edition OBB
(main.100.com.gameloft.android.ANMP.GloftGTHY.obb, version 1.0.1h): the Hyundai Genesis Coupe and the Hyundai
i-oniq, made into files the iPad game reads.  Gameloft's files: never shipped, never in the repository.

    python -I tools/hyundai_addon.py [path\\to\\main.100...GloftGTHY.obb] [out_dir]

The OBB is Gameloft's own archive: u32 count, then count x (name[256], u32 size, u32 offset).  The Hyundai
edition is the same engine as the iPad game a version earlier, so its files are nearly the iPad's:
- a car is <name>.ba01 (the iPad game makes its car table from every .ba01 in the bundle but Coin / Dollar /
  Star, 0x2b965c), <name>.ba7 / .ba8 (the cockpit SWF, HD / not), its model in the objects pack (.ba5) and its
  textures in the textures pack (.ba4).  Every .ba5 / .ba4 in the bundle is read (0x14ecdc, 0x153a70), the
  "hires" ones (name has HIK0TAEZUK) on an HD device, the others on the rest, so the add-on has its own packs.
- the car file: u16 n, n x (u32 offset, u16 index), the sections.  Section 0 the record (a version byte, then
  the fields: car id, brand, year, ..., model id at +4*5), 4 / 5 the brand logo (Sprite), 8.. one string pack
  per language (brand, model name), last 15 zero bytes.  The Hyundai edition's is version 'A' (260-byte record,
  7 languages, 16 sections); the iPad reads version 'C' (268 bytes: 0x2b9a28 reads 0x10c after the version
  byte, 11 languages, 20 sections).  'C' is 'A' with two words (1, -1) after the record's byte 148: so in 137
  of the iPad's 138 cars, which keep both versions (.ba0 'A', .ba01 'C').
- the models are objects 342 / 343, the same format as the iPad's cars, but the iPad's pack has other models
  under those ids, so here they are 503 / 504 (free); the car ids 342 / 343 are free in the iPad game.
- each model uses the iPad's shared car textures (20000, 20700, 20802, 20803, 20805) and two of its own
  (Genesis 36300 / 36350, i-oniq 36400 / 36450); the i-oniq's record also names 36452 (the Genesis's 36352
  is in the iPad pack already, the same bytes).  The paint (0x1a0b70) keeps the body texture N and N + 1 in
  memory (car +0xec / +0xf0) to paint them: 36301 / 36401 too, or the car's paint reads a NULL (seen
  as a fault in the texture maker 0x15148c from 0x1a15b4 when the main menu showed the i-oniq).
  Some are PVR v2 textures in a pack entry of format 0x3c (here RGBA4444; ETC1 elsewhere in that pack), a form
  the iPad game has no path for: they are made the iPad's uncompressed RGBA (format 0x0c) here.
  Pack entry: u32 zlib length, u32 (data size | format), zlib(header + data), padded to 32 bytes.
"""
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBB = os.path.join(ROOT, 'com.gameloft.android.ANMP.GloftGTHY', 'main.100.com.gameloft.android.ANMP.GloftGTHY.obb')
OUT = os.path.join(ROOT, 'build', 'host', 'add-on', 'hyundai')

# (Hyundai edition file name, model id there, model id here, textures of its own)
CARS = [
    ('HIZXWURADSBUK0GOEZRAEZUKK0UKC4REWUROEZADA1AAAF', 342, 503, (36300, 36301, 36350)),   # hyundaigenesiscoupe2013
    ('HIZXWURADSBUK0K0RERAK0SIADA1AAAF', 343, 504, (36400, 36401, 36450, 36452)),           # hyundaiioniq2013
]
PACK = 'HIZXWURADSBUK0'          # "hyundai" in the bundle's two-letter code; HIK0TAEZUK = "hires"
HIRES = 'HIK0TAEZUK'


# the OBB
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


# packs (.ba5 objects: u32 offset, u32 id; .ba4 textures: u32 offset, u16 id)
def pack_read(b, wide):
    n, = struct.unpack_from('<H', b, 0)
    es = [struct.unpack_from('<II' if wide else '<IH', b, 2 + (8 if wide else 6) * i) for i in range(n)]
    hdr = 2 + (8 if wide else 6) * n
    out = {}
    for i, (off, id_) in enumerate(es[:-1]):           # the last is the end (id -1, no data)
        out[id_] = b[hdr + off:hdr + es[i + 1][0]]
    return out


def pack_write(items, wide):
    """items: [(id, bytes)] in id order"""
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


# ETC1 -> RGBA
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
    """a Hyundai edition texture entry -> one the iPad game reads (ETC1 decoded to RGBA)"""
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
    if kind == 0x36:                                     # OGL_ETC
        rgba, what = etc1_decode(data[:w * h // 2], w, h), 'ETC1'
    elif kind == 0x10:                                   # OGL_RGBA_4444
        px = struct.unpack_from('<%dH' % (w * h), data, 0)
        rgba = bytes(c for v in px for c in ((v >> 12) * 17, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17))
        what = 'RGBA4444'
    elif kind == 0x12:                                   # OGL_RGBA_8888
        rgba, what = data[:w * h * 4], 'RGBA8888'
    else:
        raise ValueError('PVR pixel type %#x' % kind)
    return tex_entry(struct.pack('<HHHHI', w, h, 8, 1, len(rgba)) + rgba, 0x0c), '%s %dx%d -> RGBA' % (what, w, h)


# the car file
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

# Section 7 is the country flag (a Sprite, drawn by AbstractMenu::RenderCarFlag from the car table +0x11c in the
# Android build): one module of the flag atlas, texture 20002.  The Hyundai edition's atlas is 64 x 64 (flags
# 21 x 15); the iPad's is 128 x 128 (42 x 32 cells, rows from the bottom: Japan Italy France / USA UK Germany /
# Spain Korea Austria), so the Hyundai rectangle (21, 33) fell on the iPad's US flag (seen in the
# dealer).  The iPad atlas has the Korean flag
# already (at 42, 64, unused by the iPad's cars): point the module there.
# Sections 4 / 5 are the brand logo, small and large, on the logo atlases 20003 (512 x 512) and 20004
# (1024 x 1024).  The iPad's atlases have the Hyundai logo too, larger and a little elsewhere, so the Hyundai
# rectangles cut it (the logo was clipped in the dealer).  The rectangles below are the
# logo's bounds in the iPad atlases (the same in the hires pack), found from its alpha.
FLAG_KOREA = (0x2a, 0x40, 0x2a, 0x20)
SPRITE_RECTS = {4: (20003, (393, 127, 112, 57)), 5: (20004, (785, 10, 224, 114)), 7: (20002, FLAG_KOREA)}


def sprite_to_ipad(s, tex, rect):
    """a one-module Sprite section with its module moved to rect (x, y, w, h) of texture tex"""
    s = bytearray(s)
    if len(s) != 42 or s[:2] != b'\xdf\x03' or struct.unpack_from('<H', s, 6)[0] != tex:
        raise ValueError('sprite of an unknown form: %s' % s.hex())
    struct.pack_into('<4H', s, 12, *rect)
    s[36], s[37] = rect[2], rect[3]        # the frame's size, a byte each
    return bytes(s)


def car_to_c(a, model_id, fix=None):
    """version A car file -> version C; fix(secs, rec) may change the sections / record first (default: the
    Hyundai sprite rectangles; tools/android_free_addon.py passes its own)"""
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
