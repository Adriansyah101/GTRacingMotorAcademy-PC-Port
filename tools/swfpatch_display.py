"""swfpatch_display.py <menu.FAK> <out.FAK> [--png preview.png] -- DISPLAY_MENU: Options' Customer Care tile
becomes a Display tile.

The Display page holds resolution, aspect ratio, anti-aliasing and anisotropic filtering.  Customer Care opens
Gameloft's support pages, which need
the network (offline in this port).

AllOptionsMenu's btnCustomSupport (its own 72-frame sprite) is placed with a copy of btnControls' sprite
instead, so it looks, moves and sounds like the other five tiles and its press runs Controls' action
(FSCommand PushMenu ControlsMenu); host/games/gtracing/features/display.c takes the press first (the game's own handler, 0x24b484,
would open Customer Care) and has ControlsMenu show only its PCDisplay page (swfpatch_controls.py).  The
instance keeps its name: AllOptionsMenu's scripts show and hide it by name.  The copy's placements are
moved so its tile sits where Customer Care's did.

The tile's picture is made here, at build time, from the player's own copy of the game (nothing of the
game's is stored in this repository): Customer Care's picture (the blue platform under a support agent,
cut from the menu's texture atlas, tag 200) with the agent painted out (the platform's top filled across
from its sides, above it transparent) and a monitor drawn standing on the platform.  Applied after
swfpatch_controls.py (and swfpatch_exit.py) to the main menu files and to the in-race ones
(LHBMiNHF*: the pause menu's Options, which showed Customer Care's tile too)."""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from swfpatch_controls import Swf, tag, tags  # noqa: E402
from swfpatch_exit import (bitmap_shape, build_place2, lossless2, parse_place2, png,  # noqa: E402
                           sd_round_rect, cover, blur)

PX = 20


def sprite_tags(body):
    return tags(body, 4, len(body))


def first_place(s, sid, depth):
    for c, b, _ in sprite_tags(s.sprites[sid]):
        if c == 26 and b[0] & 2 and parse_place2(b)['depth'] == depth:
            return parse_place2(b)
    raise SystemExit('no depth %d in sprite %d' % (depth, sid))


def shape_fill(s, sid):
    """(box twips (x0 x1 y0 y1), bitmap id, fill matrix) of a one-bitmap-fill DefineShape"""
    from swfpatch_controls import BR
    for c, b, _ in s.top:
        if c in (2, 22, 32) and struct.unpack_from('<H', b)[0] == sid:
            r = BR(b, 2)
            n = r.u(5)
            box = [r.sg(n) for _ in range(4)]
            k = r.end()
            if b[k] < 1 or b[k + 1] not in (0x40, 0x41, 0x42, 0x43):
                raise SystemExit('shape %d: not a bitmap fill' % sid)
            bid = struct.unpack_from('<H', b, k + 2)[0]
            from swfpatch_exit import read_matrix
            m, _ = read_matrix(b, k + 4)
            return box, bid, m
    raise SystemExit('no shape %d' % sid)


def atlas(s, bid):
    """(width, height, rgba) of a menu texture (tag 200: Gameloft's PVR, RGBA 8888 in the HD files,
    RGBA 4444 in the SD one)"""
    for c, b, _ in s.top:
        if c == 200 and struct.unpack_from('<H', b)[0] == bid:
            p = b.index(b'pvr\0') + 4
            hs, h, w, mips, flags = struct.unpack_from('<5I', b, p)
            kind = flags & 0xff
            lw = struct.unpack_from('<H', b, 2)[0]       # the size the SWF's fills count in (the SD file's
            f = w / float(lw) if lw else 1.0             # texture is half of it)
            if kind in (0x12, 0x10):
                return w, h, (b, p + hs, 4 if kind == 0x12 else 2), f
            raise SystemExit('texture %d: pixel type 0x%x, not RGBA 8888 / 4444' % (bid, kind))
    raise SystemExit('no texture %d' % bid)


def resample(src, w, h, f):
    """an rgba picture f times larger (bilinear, alpha-weighted)"""
    nw, nh = int(round(w * f)), int(round(h * f))
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        sy = min(max((y + 0.5) / f - 0.5, 0), h - 1)
        y0 = int(sy)
        y1, fy = min(y0 + 1, h - 1), sy - int(sy)
        for x in range(nw):
            sx = min(max((x + 0.5) / f - 0.5, 0), w - 1)
            x0 = int(sx)
            x1, fx = min(x0 + 1, w - 1), sx - int(sx)
            acc, a = [0.0, 0.0, 0.0], 0.0
            for xx, yy, wt in ((x0, y0, (1 - fx) * (1 - fy)), (x1, y0, fx * (1 - fy)), (x0, y1, (1 - fx) * fy),
                               (x1, y1, fx * fy)):
                o = (yy * w + xx) * 4
                pa = src[o + 3] * wt
                a += pa
                for k in range(3):
                    acc[k] += src[o + k] * pa
            o = (y * nw + x) * 4
            out[o:o + 4] = bytes([int(acc[k] / a + 0.5) if a else 0 for k in range(3)] + [int(a + 0.5)])
    return bytes(out), nw, nh


# the picture
def picture(src, w, h):
    """the Display tile from Customer Care's (straight alpha rgba, w x h, 1 pixel = 1 stage px)"""
    px = [list(src[k:k + 4]) for k in range(0, len(src), 4)]

    def at(x, y):
        return px[y * w + x]

    # the platform's back edge: per column, the first opaque row, read where nothing stands on it
    top = [next((y for y in range(h) if at(x, y)[3] > 128), h) for x in range(w)]
    sides = [x for x in range(w) if top[x] < h]
    xl, xr = sides[0], sides[-1]
    # the agent stands in the middle: columns whose top is far above the platform's flat back edge
    back = sorted(top[x] for x in range(xl, xr + 1))[len(range(xl, xr + 1)) // 2]   # the flat edge's row
    tall = [x for x in range(xl, xr + 1) if top[x] < back - 3]
    ax0, ax1 = tall[0] - 14, tall[-1] + 14                  # the agent's columns, with the shoulders
    ay1 = back + 62                                          # down to the waist on the platform
    out = [p[:] for p in px]
    for y in range(h):
        for x in range(max(ax0, 0), min(ax1, w - 1) + 1):
            if y < back:                                     # above the platform: nothing
                out[y * w + x] = [0, 0, 0, 0]
            elif y <= ay1:                                   # the platform's top, filled across
                l, r = at(ax0 - 1, y), at(ax1 + 1, y)
                f = (x - ax0 + 1) / (ax1 - ax0 + 2)
                c = [l[k] * (1 - f) + r[k] * f for k in range(4)]
                if y > ay1 - 8:                              # blended into what is below
                    g = (y - (ay1 - 8)) / 8
                    c = [c[k] * (1 - g) + at(x, y)[k] * g for k in range(4)]
                out[y * w + x] = c
    # the monitor, centred over the agent's place, standing on the platform
    cx = (ax0 + ax1) / 2
    sw, sh_ = 170.0, 112.0                                   # bezel
    sy0 = back - 92
    bez = (cx, sy0 + sh_ / 2, sw / 2, sh_ / 2)
    scr = (cx, sy0 + sh_ / 2 - 2, sw / 2 - 7, sh_ / 2 - 9)
    base_y = back + 40
    # its shadow on the platform
    shadow = [0.0] * (w * h)
    for y in range(h):
        for x in range(w):
            d = math.hypot((x - cx) / 62.0, (y - base_y - 4) / 11.0) - 1
            shadow[y * w + x] = cover(d * 10, 1.0)
    shadow = blur(shadow, w, h, 4)
    for y in range(h):
        for x in range(w):
            p = out[y * w + x]
            k = shadow[y * w + x] * 0.55
            out[y * w + x] = [p[0] * (1 - k), p[1] * (1 - k), p[2] * (1 - k), p[3]]

    def put(x, y, col, a):
        if a <= 0:
            return
        p = out[y * w + x]
        pa = p[3] / 255.0
        na = a + pa * (1 - a)
        if na <= 0:
            return
        out[y * w + x] = [(col[k] * a + p[k] * pa * (1 - a)) / na for k in range(3)] + [na * 255]

    for y in range(h):
        for x in range(w):
            fx, fy = x + 0.5, y + 0.5
            # the foot: a flat rounded slab
            d = sd_round_rect(fx, fy, cx, base_y, 44, 6, 6)
            a = cover(d, 1.0)
            if a > 0:
                t = (fy - (base_y - 6)) / 12
                g = 150 - 70 * t
                put(x, y, (g, g, g + 6), a)
            # the neck
            if sy0 + sh_ - 4 <= fy <= base_y:
                t = (fy - (sy0 + sh_)) / max(base_y - sy0 - sh_, 1)
                hwid = 11 + 5 * t
                d = abs(fx - cx) - hwid
                a = cover(d, 1.0)
                g = 120 + 50 * (1 - abs(fx - cx) / hwid) if hwid else 120
                put(x, y, (g, g, g + 6), a)
            # the bezel: dark, a lighter rim
            d = sd_round_rect(fx, fy, *bez, 8)
            a = cover(d, 1.0)
            if a > 0:
                inner = cover(d + 1.6, 1.0)              # 1 past the rim
                c = [28 * inner + 125 * (1 - inner)] * 3
                c[2] += 6
                put(x, y, c, a)
            # the screen: deep blue to cyan, a soft glow and a diagonal gloss
            d = sd_round_rect(fx, fy, *scr, 3)
            a = cover(d, 1.0)
            if a > 0:
                u = (fx - (scr[0] - scr[2])) / (2 * scr[2])
                v = (fy - (scr[1] - scr[3])) / (2 * scr[3])
                c = [10 + 30 * u, 60 + 90 * (1 - v) + 40 * u, 150 + 90 * (1 - v)]
                glow = math.exp(-((u - 0.35) ** 2 * 6 + (v - 0.3) ** 2 * 8))
                c = [c[k] + (255 - c[k]) * 0.35 * glow for k in range(3)]
                if u + v * 0.6 < 0.62:
                    c = [c[k] + (255 - c[k]) * 0.12 for k in range(3)]
                put(x, y, c, a)
    rgba = bytearray()
    for p in out:
        rgba += bytes(int(round(min(max(v, 0), 255))) for v in p)
    return bytes(rgba)


# the SWF
def main():
    src, dst = sys.argv[1], sys.argv[2]
    s = Swf(open(src, 'rb').read())
    aom, _ = s.child(None, 'AllOptionsMenu')
    cc, _ = s.child(aom, 'btnCustomSupport')
    ctl, _ = s.child(aom, 'btnControls')
    cc_tile, ctl_tile = first_place(s, cc, 1), first_place(s, ctl, 1)
    box, bid, fm = shape_fill(s, cc_tile['char'])
    aw, ah, tex, tf = atlas(s, bid)
    # the shape's box in the texture's pixels (the fill: scale 20 twips a pixel, origin at fm's tx, ty)
    sc = fm[0] / 65536.0
    bx0 = int(math.floor((box[0] - fm[4]) / sc))
    by0 = int(math.floor((box[2] - fm[5]) / sc))
    bx1 = int(math.ceil((box[1] - fm[4]) / sc))
    by1 = int(math.ceil((box[3] - fm[5]) / sc))
    w, h = bx1 - bx0, by1 - by0
    x0, y0 = int(round(fm[4] + bx0 * sc)), int(round(fm[5] + by0 * sc))
    # the texture's own pixels (the SD file's are half the fill's), then 1 pixel = 1 stage px
    rx0, ry0 = int(math.floor(bx0 * tf)), int(math.floor(by0 * tf))
    rw, rh = int(math.ceil(bx1 * tf)) - rx0, int(math.ceil(by1 * tf)) - ry0
    tb, to, bpp = tex
    crop = bytearray()
    for y in range(ry0, ry0 + rh):
        o = to + (y * aw + rx0) * bpp
        if bpp == 4:
            crop += tb[o:o + rw * 4]
        else:                                        # 16-bit RRRRGGGGBBBBAAAA
            for k in range(rw):
                v = tb[o + 2 * k] | tb[o + 2 * k + 1] << 8
                crop += bytes((((v >> 12) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17))
    crop = bytes(crop)
    if abs(tf - 1) > 0.01 or abs(sc - PX) > 0.01:
        crop, rw, rh = resample(crop, rw, rh, PX / (sc * tf))
        sc = PX
        w, h = rw, rh
    rgba = picture(crop, w, h)
    if '--png' in sys.argv:
        png(sys.argv[sys.argv.index('--png') + 1], w, h, rgba)

    ids = set(s.sprites) | set(s.texts)
    for c, b, _ in s.top:
        if c in (2, 6, 10, 11, 14, 20, 21, 22, 32, 33, 34, 35, 36, 37, 39, 46, 48, 60, 75, 83, 84, 90, 200) \
                and len(b) >= 2:
            ids.add(struct.unpack_from('<H', b)[0])
    nbid, nshid, nbtn = (max(ids) + k for k in range(1, 4))
    defs = [lossless2(nbid, w, h, rgba),
            bitmap_shape(nshid, nbid, x0, y0, x0 + int(w * sc), y0 + int(h * sc), int(sc))]
    # the button: Controls' frames, every placement moved by the tiles' difference, our tile at depth 1
    dx = cc_tile['matrix'][4] - ctl_tile['matrix'][4]
    dy = cc_tile['matrix'][5] - ctl_tile['matrix'][5]
    sid, frames = struct.unpack_from('<HH', s.sprites[ctl])
    new = []
    for c, b, r in sprite_tags(s.sprites[ctl]):
        if c == 26:
            p = parse_place2(b)
            if p['flags'] & 2 and p['char'] == ctl_tile['char']:
                p['char'] = nshid
            if p['flags'] & 4:
                m = p['matrix']
                p['matrix'] = m[:4] + (m[4] + dx, m[5] + dy)
            new.append(build_place2(p))
            continue
        if c == 70:
            raise SystemExit('btnControls: PlaceObject3 not handled')
        new.append(r)
    defs.append(tag(39, struct.pack('<HH', nbtn, frames) + b''.join(new)))

    out, placed = [], 0
    for code, body, raw in s.top:
        if code == 39 and struct.unpack_from('<H', body)[0] == aom:
            out += defs                                  # defined before their user
            usid, uframes = struct.unpack_from('<HH', body)
            nb = []
            for c, b, r in sprite_tags(body):
                if c == 26 and b[0] & 2 and parse_place2(b)['char'] == cc:
                    p = parse_place2(b)
                    p['char'] = nbtn
                    nb.append(build_place2(p))
                    placed += 1
                    continue
                nb.append(r)
            out.append(tag(39, struct.pack('<HH', usid, uframes) + b''.join(nb)))
        else:
            out.append(raw)
    if not placed:
        raise SystemExit('btnCustomSupport not placed in AllOptionsMenu')
    body = s.d[8:s.start] + b''.join(out)
    open(dst, 'wb').write(b'FWS' + s.d[3:4] + struct.pack('<I', 8 + len(body)) + body)
    print('%s: Display tile %dx%d (bitmap %d, shape %d) on a copy of btnControls %d as button %d, '
          'moved %d,%d twips' % (os.path.basename(dst), w, h, nbid, nshid, ctl, nbtn, dx, dy))


if __name__ == '__main__':
    main()
