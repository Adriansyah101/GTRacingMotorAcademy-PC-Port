"""swfpatch_exit.py <menu.FAK> <out.FAK> [--png preview.png] -- EXIT_BUTTON: an Exit button on the main menu.

The original has no way to leave the game (on the iPad the Home button did that).  This adds btnPCExit to
NavigationMenu's bottom-left bar, right of the gear (btnSettings), where the original only ever shows
btnCashbonus when Gameloft's ad server offers free cash (0x33a0c8; never offline).  host/games/gtracing/features/exitbtn.c takes
its click: a YES / NO popup, then the game ends as when the window is closed.

The button is a copy of the gear button's sprite (its Idle / Hide / Show / Clicked / pressToClick frames,
its hitzone, its press animation and click sound), with:
  - the tile: a new bitmap (DefineBitsLossless2, drawn here) in place of the gear's atlas picture: the
    same yellow tile (colours sampled from the game's gear tile on screen) with a white power
    symbol instead of the gears;
  - the press's last action (PushMenu AllOptionsMenu, i.e. what the gear opens) replaced by
    gotoAndStop("Idle").
It is placed wherever btnSettings is (NavigationMenu frame 1 on, and the bar sprite NavigationMenu shows
on frame 0), 79 px to the right: the gap between the LIVE and gear tiles again, which ends the tile at the
bar's right edge (232 px).  Every later change of btnSettings' depth (its fade in Hide / Show) is done to
ours too.  Applied after swfpatch_controls.py, to the main menu files only."""
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from swfpatch_controls import BR, BW, Swf, tag, tags  # noqa: E402

PX = 20
DX = 79 * PX                       # btnSettings -> btnPCExit
NAME = 'btnPCExit'

# the picture
# The tile in the tile sprite's space (where the gear's 541 draws its 540), stage px: measured on a
# 1441 x 1079 screenshot of the gear tile (black border 111.5 .. 218.5, 968 .. 1066 screen px)
TILE = (0.8, 0.3, 76.8, 69.9)      # x0 y0 x1 y1
RES = 2                            # bitmap pixels per stage pixel
PAD = 1                            # transparent bitmap pixels round the tile
RADIUS, BORDER = 6.5, 2.1         # outer corner radius, black border (stage px)
GLOSS = 0.563                      # the light / deep yellow edge, of the inner height
UPPER = [(0.00, 0xfcf2c8), (0.04, 0xfde6b2), (0.10, 0xfdd696), (0.15, 0xfdc979), (0.20, 0xfdc979),
         (0.35, 0xfdcf83), (0.45, 0xfcd386), (0.53, 0xf6d288), (GLOSS, 0xf0cc7a)]
LOWER = [(GLOSS, 0xfcb60f), (0.64, 0xfcba0f), (0.75, 0xfcc40f), (0.84, 0xfcce0f), (0.90, 0xfcd40f),
         (1.00, 0xfcd70f)]


def ramp(stops, t):
    if t <= stops[0][0]:
        return stops[0][1]
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            f = (t - t0) / (t1 - t0) if t1 > t0 else 1
            return tuple(round(((c0 >> s) & 255) * (1 - f) + ((c1 >> s) & 255) * f) for s in (16, 8, 0))
    c = stops[-1][1]
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255)


def rgb(c):
    return c if isinstance(c, tuple) else ((c >> 16) & 255, (c >> 8) & 255, c & 255)


def sd_round_rect(x, y, cx, cy, hw, hh, r):
    qx, qy = abs(x - cx) - hw + r, abs(y - cy) - hh + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def sd_power(x, y):
    """the power symbol, centred on 0,0 (stage px): an open ring and a bar through its gap"""
    rmid, th, gap = 18.5, 7.0, math.radians(40)
    a = math.atan2(x, -y)                         # 0 straight up
    if abs(a) > gap:
        d_ring = abs(math.hypot(x, y) - rmid) - th / 2
    else:
        ex, ey = rmid * math.sin(gap), -rmid * math.cos(gap)
        d_ring = min(math.hypot(x - ex, y - ey), math.hypot(x + ex, y - ey)) - th / 2
    y0, y1 = -24.5, -4.5
    d_bar = math.hypot(x, y - min(max(y, y0), y1)) - th / 2
    return min(d_ring, d_bar)


def cover(d, px):
    return min(max(0.5 - d / px, 0.0), 1.0)


def blur(a, w, h, r):
    """three box passes each way (about a Gaussian)"""
    for _ in range(3):
        for horiz in (True, False):
            out = [0.0] * (w * h)
            n, m = (w, h) if horiz else (h, w)
            for j in range(m):
                idx = [(j * w + i) if horiz else (i * w + j) for i in range(n)]
                acc, row = 0.0, [a[k] for k in idx]
                pre = [0.0]
                for v in row:
                    acc += v
                    pre.append(acc)
                for i in range(n):
                    lo, hi = max(i - r, 0), min(i + r + 1, n)
                    out[idx[i]] = (pre[hi] - pre[lo]) / (2 * r + 1)
            a = out
    return a


def picture():
    """(width, height, rgba bytes): the tile, straight alpha"""
    x0, y0, x1, y1 = TILE
    w = int(round((x1 - x0) * RES)) + 2 * PAD
    h = int(round((y1 - y0) * RES)) + 2 * PAD
    px = 1.0 / RES
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hw, hh = (x1 - x0) / 2, (y1 - y0) / 2
    in_top, in_h = y0 + BORDER, (y1 - y0) - 2 * BORDER
    sym_cy = cy + 3.0                             # the symbol's ink is centred on the tile
    # the symbol's shadow (dark brown, as round the gears), blurred
    sh = [0.0] * (w * h)
    for j in range(h):
        for i in range(w):
            x, y = x0 + (i - PAD + 0.5) * px, y0 + (j - PAD + 0.5) * px
            sh[j * w + i] = cover(sd_power(x - cx, y - sym_cy - 0.8) - 1.3, px)
    sh = blur(sh, w, h, max(1, int(1.2 * RES)))
    out = bytearray()
    for j in range(h):
        for i in range(w):
            x, y = x0 + (i - PAD + 0.5) * px, y0 + (j - PAD + 0.5) * px
            d_out = sd_round_rect(x, y, cx, cy, hw, hh, RADIUS)
            a_out = cover(d_out, px)
            if a_out <= 0:
                out += b'\0\0\0\0'
                continue
            t = (y - in_top) / in_h
            t = min(max(t, 0.0), 1.0)
            # the gloss edge, anti-aliased
            f = min(max((t - GLOSS) / (px / in_h) + 0.5, 0.0), 1.0)
            cu, cl = rgb(ramp(UPPER, min(t, GLOSS))), rgb(ramp(LOWER, max(t, GLOSS)))
            col = [cu[k] * (1 - f) + cl[k] * f for k in range(3)]
            # the symbol: shadow, grey rim, white face (a touch greyer at the bottom)
            s = sh[j * w + i] * 0.55
            col = [c * (1 - s) + v * s for c, v in zip(col, (70, 48, 18))]
            d_sym = sd_power(x - cx, y - sym_cy)
            rim = cover(d_sym - 0.9, px)
            col = [c * (1 - rim) + v * rim for c, v in zip(col, (160, 152, 140))]
            face = cover(d_sym, px)
            g = 255 - 22 * min(max((y - (sym_cy - 26)) / 50, 0), 1)
            col = [c * (1 - face) + g * face for c in col]
            # the black border
            b = cover(d_out + BORDER, px)             # 1 inside the border's inner edge
            col = [c * b for c in col]
            out += bytes([int(round(min(max(c, 0), 255))) for c in col] + [int(round(a_out * 255))])
    return w, h, bytes(out)


def png(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[j * w * 4:(j + 1) * w * 4] for j in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


# SWF records
def sbits(*vals):
    return max(max(abs(v) for v in vals).bit_length() + 1, 1)


def matrix_bytes(m):
    sx, sy, r0, r1, tx, ty = m
    w = BW()
    if (sx, sy) != (65536, 65536):
        w.u(1, 1)
        n = sbits(sx, sy)
        w.u(n, 5)
        w.u(sx & ((1 << n) - 1), n)
        w.u(sy & ((1 << n) - 1), n)
    else:
        w.u(0, 1)
    if r0 or r1:
        w.u(1, 1)
        n = sbits(r0, r1)
        w.u(n, 5)
        w.u(r0 & ((1 << n) - 1), n)
        w.u(r1 & ((1 << n) - 1), n)
    else:
        w.u(0, 1)
    n = sbits(tx, ty) if tx or ty else 0
    w.u(n, 5)
    if n:
        w.u(tx & ((1 << n) - 1), n)
        w.u(ty & ((1 << n) - 1), n)
    return w.done()


def read_matrix(b, k):
    r = BR(b, k)
    sx = sy = 65536
    r0 = r1 = 0
    if r.u(1):
        n = r.u(5)
        sx, sy = r.sg(n), r.sg(n)
    if r.u(1):
        n = r.u(5)
        r0, r1 = r.sg(n), r.sg(n)
    n = r.u(5)
    tx, ty = r.sg(n), r.sg(n)
    return (sx, sy, r0, r1, tx, ty), r.end()


def read_cxform(b, k):
    r = BR(b, k)
    ha, hm, n = r.u(1), r.u(1), r.u(4)
    for _ in range((4 if hm else 0) + (4 if ha else 0)):
        r.sg(n)
    return b[k:r.end()], r.end()


def parse_place2(b):
    """PlaceObject2 -> dict of its fields (cxform kept as bytes)"""
    p = {'flags': b[0], 'depth': struct.unpack_from('<H', b, 1)[0]}
    k = 3
    f = p['flags']
    if f & 2:
        p['char'] = struct.unpack_from('<H', b, k)[0]
        k += 2
    if f & 4:
        p['matrix'], k = read_matrix(b, k)
    if f & 8:
        p['cxform'], k = read_cxform(b, k)
    if f & 16:
        p['ratio'] = b[k:k + 2]
        k += 2
    if f & 32:
        e = b.index(b'\0', k)
        p['name'] = b[k:e]
        k = e + 1
    if f & 64:
        p['clip'] = b[k:k + 2]
        k += 2
    p['tail'] = b[k:]                             # clip actions, kept as they are
    return p


def build_place2(p):
    f = p['flags']
    out = bytes([f]) + struct.pack('<H', p['depth'])
    if f & 2:
        out += struct.pack('<H', p['char'])
    if f & 4:
        out += matrix_bytes(p['matrix'])
    if f & 8:
        out += p['cxform']
    if f & 16:
        out += p['ratio']
    if f & 32:
        out += p['name'] + b'\0'
    if f & 64:
        out += p['clip']
    return tag(26, out + p.get('tail', b''))


def rect_bytes(x0, x1, y0, y1):
    w = BW()
    n = sbits(x0, x1, y0, y1)
    w.u(n, 5)
    for v in (x0, x1, y0, y1):
        w.u(v & ((1 << n) - 1), n)
    return w.done()


def bitmap_shape(sid, bid, x0, y0, x1, y1, scale):
    """DefineShape: the rectangle x0..x1, y0..y1 (twips) filled with bitmap bid (clipped), scale twips per
    bitmap pixel, its top left at x0, y0"""
    body = struct.pack('<H', sid) + rect_bytes(x0, x1, y0, y1)
    body += bytes([1, 0x41]) + struct.pack('<H', bid) + matrix_bytes((scale * 65536, scale * 65536, 0, 0, x0, y0))
    body += bytes([0])                            # no line styles
    w = BW()
    w.u(1, 4)                                     # fill bits
    w.u(0, 4)                                     # line bits
    w.u(0, 1)                                     # style change: fill 1, move to x0, y0
    w.u(0, 1); w.u(0, 1); w.u(1, 1); w.u(0, 1); w.u(1, 1)
    n = sbits(x0, y0)
    w.u(n, 5)
    w.u(x0 & ((1 << n) - 1), n)
    w.u(y0 & ((1 << n) - 1), n)
    w.u(1, 1)                                     # fill style 1 = 1 (inside, drawn clockwise)
    for dx, dy in ((x1 - x0, 0), (0, y1 - y0), (x0 - x1, 0), (0, y0 - y1)):
        w.u(1, 1); w.u(1, 1)                      # edge, straight
        n = max(sbits(dx, dy), 2)
        w.u(n - 2, 4)
        w.u(0, 1)                                 # not general: vertical or horizontal
        if dx:
            w.u(0, 1)
            w.u(dx & ((1 << n) - 1), n)
        else:
            w.u(1, 1)
            w.u(dy & ((1 << n) - 1), n)
    w.u(0, 6)                                     # end
    return tag(2, body + w.done())


def lossless2(bid, w, h, rgba):
    """DefineBitsLossless2, 32-bit ARGB (premultiplied, as the SWF format has it)"""
    argb = bytearray()
    for k in range(0, len(rgba), 4):
        r, g, b, a = rgba[k:k + 4]
        argb += bytes([a, r * a // 255, g * a // 255, b * a // 255])
    return tag(36, struct.pack('<HBHH', bid, 5, w, h) + zlib.compress(bytes(argb), 9))


def sprite_tags(body):
    return tags(body, 4, len(body))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    s = Swf(open(src, 'rb').read())
    # the sprites that place btnSettings (NavigationMenu and its frame-0 bar), the gear's sprite
    users, gear, tile_sprite = [], None, None
    for sid, body in s.sprites.items():
        for c, b, _ in sprite_tags(body):
            if c == 26 and b[0] & 0x22 == 0x22 and parse_place2(b).get('name') == b'btnSettings':
                users.append(sid)
                gear = parse_place2(b)['char']
    if not users:
        raise SystemExit('no btnSettings in %s' % src)
    # the gear sprite's tile: the sprite at depth 1 of its first frame, and the shape that sprite shows
    for c, b, _ in sprite_tags(s.sprites[gear]):
        if c == 26 and b[0] & 2 and parse_place2(b)['depth'] == 1:
            tile_sprite = parse_place2(b)['char']
            break
    tile_shape = next(parse_place2(b)['char'] for c, b, _ in sprite_tags(s.sprites[tile_sprite])
                      if c == 26 and b[0] & 2)

    ids = set(s.sprites) | set(s.texts)
    for c, b, _ in s.top:
        if c in (2, 6, 10, 11, 14, 20, 21, 22, 32, 33, 34, 35, 36, 37, 39, 46, 48, 60, 75, 83, 84, 90, 200) \
                and len(b) >= 2:
            ids.add(struct.unpack_from('<H', b)[0])
    bid, shid, tid, xid = (max(ids) + k for k in range(1, 5))

    w, h, rgba = picture()
    if '--png' in sys.argv:
        png(sys.argv[sys.argv.index('--png') + 1], w, h, rgba)
    x0, y0 = int(round(TILE[0] * PX)) - PAD * PX // RES, int(round(TILE[1] * PX)) - PAD * PX // RES
    scale = PX // RES
    defs = [lossless2(bid, w, h, rgba),
            bitmap_shape(shid, bid, x0, y0, x0 + w * scale, y0 + h * scale, scale),
            tag(39, struct.pack('<HH', tid, 1) +
                build_place2({'flags': 0x06, 'depth': 1, 'char': shid, 'matrix': (65536, 65536, 0, 0, 0, 0)}) +
                tag(1, b'') + tag(0, b''))]
    # the button: the gear's frames, our tile, no PushMenu
    sid, frames = struct.unpack_from('<HH', s.sprites[gear])
    new, pushes = [], 0
    for c, b, r in sprite_tags(s.sprites[gear]):
        if c == 26 and b[0] & 2:
            p = parse_place2(b)
            if p['char'] in (tile_sprite, tile_shape):
                p['char'] = tid if p['char'] == tile_sprite else shid
                new.append(build_place2(p))
                continue
        if c == 12 and b'PushMenu' in b:
            pushes += 1
            new.append(tag(12, b'\x8c\x05\x00Idle\x00\x07\x00'))   # GotoLabel "Idle"; Stop
            continue
        new.append(r)
    if pushes != 1:
        raise SystemExit('the gear sprite %d: %d PushMenu actions (1 expected)' % (gear, pushes))
    defs.append(tag(39, struct.pack('<HH', xid, frames) + b''.join(new)))

    # btnPCExit wherever btnSettings is, at a free depth above it; btnSettings' later changes copied
    first_user = min(users)
    out = []
    for code, body, raw in s.top:
        if code == 39 and struct.unpack_from('<H', body)[0] == first_user:
            out += defs                                   # defined before their first user
        if code == 39 and struct.unpack_from('<H', body)[0] in users:
            usid, uframes = struct.unpack_from('<HH', body)
            ts = sprite_tags(body)
            used = {parse_place2(b)['depth'] for c, b, _ in ts if c == 26}
            used |= {struct.unpack_from('<H', b, 2 if c == 70 else 1)[0] for c, b, _ in ts if c == 70}
            gd = next(parse_place2(b)['depth'] for c, b, _ in ts
                      if c == 26 and b[0] & 0x22 == 0x22 and parse_place2(b).get('name') == b'btnSettings')
            od = gd + 1
            while od in used:
                od += 1
            new = []
            for c, b, r in ts:
                new.append(r)
                if c == 26 and parse_place2(b)['depth'] == gd:
                    p = parse_place2(b)
                    p['depth'] = od
                    if p['flags'] & 2:
                        p['char'] = xid
                    if p['flags'] & 4:
                        m = p['matrix']
                        p['matrix'] = m[:4] + (m[4] + DX, m[5])
                    if p['flags'] & 32:
                        p['name'] = NAME.encode()
                    new.append(build_place2(p))
                elif c == 28 and struct.unpack_from('<H', b)[0] == gd:
                    new.append(tag(28, struct.pack('<H', od)))
                elif c == 5 and struct.unpack_from('<H', b, 2)[0] == gd:
                    new.append(tag(5, struct.pack('<HH', xid, od)))
            out.append(tag(39, struct.pack('<HH', usid, uframes) + b''.join(new)))
            print('%s: %s in sprite %d at depth %d (btnSettings %d)' % (os.path.basename(dst), NAME, usid, od, gd))
        else:
            out.append(raw)
    body = s.d[8:s.start] + b''.join(out)
    open(dst, 'wb').write(b'FWS' + s.d[3:4] + struct.pack('<I', 8 + len(body)) + body)
    print('%s: exit tile %dx%d (bitmap %d, shape %d, tile %d, button %d from the gear %d)' %
          (dst, w, h, bid, shid, tid, xid, gear))


if __name__ == '__main__':
    main()
