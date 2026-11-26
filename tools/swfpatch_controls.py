import struct
import sys

PX = 20


def hdr(d, i):
    v = struct.unpack_from('<H', d, i)[0]
    code, ln = v >> 6, v & 63
    i += 2
    if ln == 63:
        ln = struct.unpack_from('<I', d, i)[0]
        i += 4
    return code, ln, i


def tags(d, i, end):
    out = []
    while i < end:
        code, ln, b = hdr(d, i)
        out.append((code, d[b:b + ln], d[i:b + ln]))
        i = b + ln
        if code == 0:
            break
    return out


def tag(code, body):
    if len(body) < 63:
        return struct.pack('<H', code << 6 | len(body)) + body
    return struct.pack('<HI', code << 6 | 63, len(body)) + body


class BR:
    def __init__(self, d, k):
        self.d, self.pos = d, k * 8

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def sg(self, n):
        v = self.u(n)
        return v - (1 << n) if n and v >> (n - 1) else v

    def end(self):
        return (self.pos + 7) >> 3


class BW:
    def __init__(self):
        self.bits = []

    def u(self, v, n):
        self.bits += [(v >> (n - 1 - k)) & 1 for k in range(n)]

    def done(self):
        b = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int(''.join(map(str, b[k:k + 8])), 2) for k in range(0, len(b), 8))


def matrix_tx(d, k):
    r = BR(d, k)
    if r.u(1):
        n = r.u(5); r.sg(n); r.sg(n)
    if r.u(1):
        n = r.u(5); r.sg(n); r.sg(n)
    n = r.u(5)
    return r.sg(n), r.sg(n)


def placement(code, b):
    f = b[0]
    k = 2 if code == 70 else 1
    depth = struct.unpack_from('<H', b, k)[0]
    k += 2
    cid = None
    if f & 2:
        cid = struct.unpack_from('<H', b, k)[0]
        k += 2
    t = None
    if f & 4:
        t = matrix_tx(b, k)
        r = BR(b, k)
        if r.u(1):
            n = r.u(5); r.sg(n); r.sg(n)
        if r.u(1):
            n = r.u(5); r.sg(n); r.sg(n)
        n = r.u(5); r.sg(n); r.sg(n)
        k = r.end()
    if f & 8:
        r = BR(b, k)
        ha, hm, n = r.u(1), r.u(1), r.u(4)
        for _ in range((4 if hm else 0) + (4 if ha else 0)):
            r.sg(n)
        k = r.end()
    if f & 16:
        k += 2
    name = None
    if f & 32:
        e = b.index(b'\0', k)
        name = b[k:e].decode('latin-1')
    return f, depth, cid, t, name


def matrix(tx, ty):
    w = BW()
    w.u(0, 1)
    w.u(0, 1)
    n = max(abs(tx), abs(ty), 1).bit_length() + 1
    w.u(n, 5)
    w.u(tx & ((1 << n) - 1), n)
    w.u(ty & ((1 << n) - 1), n)
    return w.done()


def place(depth, cid, x, y, name):
    return tag(26, bytes([0x02 | 0x04 | 0x20]) + struct.pack('<HH', depth, cid) + matrix(x, y) +
               name.encode() + b'\0')


def bits(*vals):
    return max(max(abs(v) for v in vals).bit_length() + 1, 2)


def matrix_s(sx, sy, tx, ty):
    w = BW()
    fx, fy = int(round(sx * 65536)), int(round(sy * 65536))
    if (fx, fy) != (65536, 65536):
        w.u(1, 1)
        n = bits(fx, fy)
        w.u(n, 5)
        w.u(fx & ((1 << n) - 1), n)
        w.u(fy & ((1 << n) - 1), n)
    else:
        w.u(0, 1)
    w.u(0, 1)
    n = bits(tx, ty)
    w.u(n, 5)
    w.u(tx & ((1 << n) - 1), n)
    w.u(ty & ((1 << n) - 1), n)
    return w.done()


def place_s(depth, cid, sx, sy, tx, ty, name=None):
    return tag(26, bytes([0x02 | 0x04 | (0x20 if name else 0)]) + struct.pack('<HH', depth, cid) +
               matrix_s(sx, sy, int(round(tx)), int(round(ty))) + (name.encode() + b'\0' if name else b''))


def place_matrix(code, b):
    k = 2 if code == 70 else 1
    k += 2
    if b[0] & 2:
        k += 2
    r = BR(b, k)
    sx = sy = 1.0
    if r.u(1):
        n = r.u(5)
        sx, sy = r.sg(n) / 65536, r.sg(n) / 65536
    if r.u(1):
        n = r.u(5)
        r.sg(n)
        r.sg(n)
    n = r.u(5)
    return sx, sy, r.sg(n), r.sg(n)


def rect_shape(sid, w, h, fill, line, lw):
    hw, hh, m = w // 2, h // 2, lw // 2
    b = BW()
    n = bits(hw + m, hh + m)
    b.u(n, 5)
    for v in (-hw - m, hw + m, -hh - m, hh + m):
        b.u(v & ((1 << n) - 1), n)
    body = struct.pack('<H', sid) + b.done() + bytes([1, 0]) + bytes(fill) + bytes([1]) + \
        struct.pack('<H', lw) + bytes(line)
    r = BW()
    r.u(1, 4)
    r.u(1, 4)
    for f in (0, 0, 1, 1, 0, 1):
        r.u(f, 1)
    n = bits(hw, hh)
    r.u(n, 5)
    r.u(-hw & ((1 << n) - 1), n)
    r.u(-hh & ((1 << n) - 1), n)
    r.u(1, 1)
    r.u(1, 1)
    for dx, dy in ((2 * hw, 0), (0, 2 * hh), (-2 * hw, 0), (0, -2 * hh)):
        r.u(1, 1)
        r.u(1, 1)
        n = bits(dx, dy)
        r.u(n - 2, 4)
        r.u(0, 1)
        if dx:
            r.u(0, 1)
            r.u(dx & ((1 << n) - 1), n)
        else:
            r.u(1, 1)
            r.u(dy & ((1 << n) - 1), n)
    r.u(0, 6)
    return tag(32, body + r.done())


def sprite(sid, *places):
    return tag(39, struct.pack('<HH', sid, 1) + b''.join(places) + tag(1, b'') + tag(0, b''))


class Swf:
    def __init__(self, d):
        if d[:3] != b'FWS':
            raise SystemExit('not an uncompressed SWF')
        self.d = d
        nbits = d[8] >> 3
        r = BR(d, 8)
        n = r.u(5)
        self.stage = [r.sg(n) for _ in range(4)]
        self.start = 8 + (5 + 4 * nbits + 7) // 8 + 4
        self.top = tags(d, self.start, len(d))
        self.sprites, self.texts, self.shapes = {}, {}, {}
        for c, b, _ in self.top:
            if c == 39:
                self.sprites[struct.unpack_from('<H', b)[0]] = b
            elif c in (37, 2, 22, 32, 83):
                cid = struct.unpack_from('<H', b)[0]
                r = BR(b, 2)
                n = r.u(5)
                box = [r.sg(n) for _ in range(4)]
                (self.texts if c == 37 else self.shapes)[cid] = box

    def places(self, cid=None):
        src = tags(self.sprites[cid], 4, len(self.sprites[cid])) if cid else self.top
        out, frame = [], 0
        for c, b, _ in src:
            if c == 1:
                frame += 1
            elif c in (26, 70):
                out.append((frame,) + placement(c, b) + (c, b))
        return out

    def child(self, cid, name):
        ps = self.places(cid)
        for p in ps:
            if p[5] == name:
                ts = [q[4] for q in ps if q[2] == p[2] and q[4]]
                return p[3], ts
        raise SystemExit('%s not found in %s' % (name, cid or 'root'))


def bounds(s, cid, depth=0):
    if cid in s.shapes:
        return s.shapes[cid]
    if cid in s.texts:
        return s.texts[cid]
    box = None
    if cid in s.sprites and depth < 6:
        for p in s.places(cid):
            if p[0] != 0 or p[3] is None or p[5] == 'hitzone':
                continue
            b = bounds(s, p[3], depth + 1)
            if not b:
                continue
            sx, sy, tx, ty = place_matrix(p[6], p[7])
            xs = sorted((sx * b[0] + tx, sx * b[1] + tx))
            ys = sorted((sy * b[2] + ty, sy * b[3] + ty))
            bb = [xs[0], xs[1], ys[0], ys[1]]
            box = bb if not box else [min(box[0], bb[0]), max(box[1], bb[1]), min(box[2], bb[2]), max(box[3], bb[3])]
    return box


def main():
    src, dst = sys.argv[1], sys.argv[2]
    s = Swf(open(src, 'rb').read())
    ctl, ctl_ts = s.child(None, 'ControlsMenu')
    dam, dam_ts = s.child(None, 'DriveAidsMenu')
    tabs, tabs_ts = s.child(dam, 'AllTabs')
    ctl_t, dam_t, tabs_t = ctl_ts[0], dam_ts[0], tabs_ts[0]
    cam, cam_ts = s.child(tabs, 'CameraChange')
    cam_t = min(cam_ts, key=lambda t: abs(t[0]))
    row, row_ts = s.child(cam, 'OnOffHorizon')
    _, row2_ts = s.child(cam, 'OnOffScreen')
    sel, sel_ts = s.child(cam, 'Selector2')
    selt, selt_ts = s.child(cam, 'txtSelectorsTitle')
    title, title_ts = s.child(row, 'txtTitle')
    switch, switch_ts = s.child(row, 'btnOnOff')

    bx = dam_t[0] + tabs_t[0] + cam_t[0] - ctl_t[0]
    by = dam_t[1] + tabs_t[1] + cam_t[1] - ctl_t[1] - 60 * PX

    xmin, xmax, ymin, ymax = s.texts[title]
    tcx, tcy = (xmin + xmax) // 2, (ymin + ymax) // 2
    scx = title_ts[0][0] + tcx - switch_ts[0][0]
    scy = title_ts[0][1] + tcy + 54 * PX - switch_ts[0][1]
    stage_w = s.stage[1] - s.stage[0]
    cx, cy = stage_w // 2, 714 * PX
    text_w, gap, switch_w = 210 * PX, 16 * PX, 220 * PX
    left = cx - (text_w + gap + switch_w) // 2
    ids = set(s.sprites) | set(s.texts)
    for c, b, _ in s.top:
        if c in (2, 6, 10, 11, 12, 14, 20, 21, 22, 32, 33, 34, 35, 36, 37, 39, 46, 48, 60, 75, 83, 84, 90) \
                and len(b) >= 2:
            ids.add(struct.unpack_from('<H', b)[0])
    row_id = max(ids) + 1
    touch = struct.pack('<HH', row_id, 1) + \
        place(1, title, left + text_w // 2 - tcx - ctl_t[0], cy - tcy - ctl_t[1], 'txtTitle') + \
        place(2, switch, left + text_w + gap + switch_w // 2 - scx - ctl_t[0], cy - scy - ctl_t[1],
              'btnOnOff') + tag(1, b'') + tag(0, b'')

    defs = []
    nid = [row_id]

    def new_id():
        nid[0] += 1
        return nid[0]

    arrow, _ = s.child(sel, 'btnRightArrow')
    hz, _ = s.child(arrow, 'hitzone')
    hz_place = next(p for p in s.places(hz) if p[3] is not None)
    hsx, hsy, htx, hty = place_matrix(hz_place[6], hz_place[7])
    sx0, sx1, sy0, sy1 = s.shapes[hz_place[3]]
    hbox = (hsx * sx0 + htx, hsx * sx1 + htx, hsy * sy0 + hty, hsy * sy1 + hty)
    txt_sel, _ = s.child(sel, 'txtSelectorType')
    txt_head, _ = s.child(selt, 'typefield')
    bar_sel = next(p for p in s.places(sel) if p[3] is not None and p[5] is None)
    sel_txt_t = next(p[4] for p in s.places(sel) if p[5] == 'txtSelectorType')
    bx0, bx1, by0, by1 = s.shapes[bar_sel[3]]
    tx0, tx1, ty0, ty1 = s.texts[txt_sel]
    text_dy = sel_txt_t[1] + (ty0 + ty1) / 2 - (by0 + by1) / 2

    empty = new_id()
    defs.append(sprite(empty))

    def button(w, h):
        bid = new_id()
        sid_, frames = struct.unpack_from('<HH', s.sprites[arrow])
        body, frame = [], 0
        for c, b, r in tags(s.sprites[arrow], 4, len(s.sprites[arrow])):
            if c == 1:
                frame += 1
            if c == 26 and frame == 0 and b[0] & 2:
                cid = struct.unpack_from('<H', b, 3)[0]
                depth = struct.unpack_from('<H', b, 1)[0]
                if cid == hz:
                    ksx = w / (hbox[1] - hbox[0])
                    ksy = h / (hbox[3] - hbox[2])
                    body.append(place_s(depth, hz, ksx, ksy, -w / 2 - ksx * hbox[0], -h / 2 - ksy * hbox[2],
                                        'hitzone'))
                    continue
                bb = bytearray(b)
                struct.pack_into('<H', bb, 3, empty)
                body.append(tag(26, bytes(bb)))
                continue
            body.append(r)
        defs.append(tag(39, struct.pack('<HH', bid, frames) + b''.join(body)))
        return bid

    def cell(w, h, k):
        w, h = w * PX, h * PX
        bar, hi_shape, hi, cid = new_id(), new_id(), new_id(), None
        defs.append(rect_shape(bar, w, h, (44, 44, 44, 255), (96, 96, 96, 255), 2 * PX))
        defs.append(rect_shape(hi_shape, w, h, (252, 182, 15, 48), (252, 182, 15, 255), 3 * PX))
        defs.append(sprite(hi, place_s(1, hi_shape, 1, 1, 0, 0)))
        btn = button(w, h)
        cid = new_id()
        defs.append(sprite(cid, place_s(1, bar, 1, 1, 0, 0), place_s(2, hi, 1, 1, 0, 0, 'Hi'),
                           place_s(3, txt_sel, k, k, -k * (tx0 + tx1) / 2, k * (text_dy - (ty0 + ty1) / 2), 'txtKey'),
                           place_s(4, btn, 1, 1, 0, 0, 'btnKey')))
        return cid

    small, large = cell(180, 34, 0.72), cell(300, 40, 0.8)
    cols, title_x, row_y, pitch, head_y = (420, 615, 820), 205, 192, 40, 152
    page = []
    hx0, hx1, hy0, hy1 = s.texts[txt_head]
    for i, x in enumerate(cols):
        page.append(place_s(len(page) + 1, txt_head, 1, 1, x * PX - (hx0 + hx1) / 2 - ctl_t[0],
                            head_y * PX - (hy0 + hy1) / 2 - ctl_t[1], 'Head%d' % (i + 1)))
    ex0, ex1, ey0, ey1 = s.texts[title]
    for r_ in range(9):
        y = (row_y + r_ * pitch) * PX
        page.append(place_s(len(page) + 1, title, 0.85, 0.85, title_x * PX - 0.85 * (ex0 + ex1) / 2 - ctl_t[0],
                            y - 0.85 * (ey0 + ey1) / 2 - ctl_t[1], 'Title%d' % (r_ + 1)))
    for r_ in range(9):
        y = (row_y + r_ * pitch) * PX
        for c_, x in enumerate(cols):
            page.append(place_s(len(page) + 1, small, 1, 1, x * PX - ctl_t[0], y - ctl_t[1],
                                'Cell%d%d' % (r_ + 1, c_ + 1)))
    page.append(place_s(len(page) + 1, large, 1, 1, 340 * PX - ctl_t[0], 562 * PX - ctl_t[1], 'CellDefault'))
    page.append(place_s(len(page) + 1, large, 1, 1, 684 * PX - ctl_t[0], 562 * PX - ctl_t[1], 'CellBack'))
    keys_page = new_id()
    defs.append(sprite(keys_page, *page))

    NROWS = 7
    FIT = (5 * 70 + 44 + 20 + 37 + 12) / ((NROWS - 1) * 70 + 44 + 20 + 37 + 12)
    SEL_W, ROW_PITCH, TITLE_UP, TITLE_TOP, NOTE_BOTTOM = 610 * FIT, 70 * FIT, 37 * FIT, 12 * FIT, 20 * FIT
    NOTE_DOWN = (NROWS - 1) * ROW_PITCH + 44 * FIT
    ctabs, ctabs_ts = s.child(ctl, 'AllTabs')
    pp = next(p for p in s.places(ctabs) if p[0] == 0 and p[2] == 1 and p[3] is not None)
    pb = bounds(s, pp[3])
    psx, psy, ptx, pty = place_matrix(pp[6], pp[7])
    pcx = (psx * (pb[0] + pb[1]) / 2 + ptx + ctabs_ts[0][0])
    pcy = (psy * (pb[2] + pb[3]) / 2 + pty + ctabs_ts[0][1])
    sb = bounds(s, sel)
    k = SEL_W * PX / (sb[1] - sb[0])
    bar_t = bar_sel[4] or (0, 0)
    scx, scy = (sb[0] + sb[1]) / 2, bar_t[1] + (by0 + by1) / 2
    tf, tf_ts = s.child(selt, 'typefield')
    tb = s.texts[tf]
    tcx, tcy = tf_ts[0][0] + (tb[0] + tb[1]) / 2, tf_ts[0][1] + (tb[2] + tb[3]) / 2
    c1 = pcy - (NOTE_DOWN + NOTE_BOTTOM - TITLE_UP - TITLE_TOP) / 2 * PX
    disp = []
    for i in range(NROWS):
        c = c1 + i * ROW_PITCH * PX
        disp.append(place_s(len(disp) + 1, selt, FIT, FIT, pcx - FIT * tcx, c - TITLE_UP * PX - FIT * tcy,
                            'Title%d' % (i + 1)))
        disp.append(place_s(len(disp) + 1, sel, k, k, pcx - k * scx, c - k * scy, 'Sel%d' % (i + 1)))
    note_c = c1 + NOTE_DOWN * PX
    disp.append(place_s(len(disp) + 1, title, 0.75, 0.75, pcx - 0.75 * (ex0 + ex1) / 2,
                        note_c - 0.75 * (ey0 + ey1) / 2, 'Note'))
    disp.append(place_s(len(disp) + 1, large, 1, 1, pcx, note_c, 'CellRestart'))
    print('%s: Display page: panel centre %.0f,%.0f px, selector x %.3f, rows from %.0f px' % (
        src.split('\\')[-1].split('/')[-1], (pcx + ctl_t[0]) / PX, (pcy + ctl_t[1]) / PX, k, (c1 + ctl_t[1]) / PX))
    disp_page = new_id()
    defs.append(sprite(disp_page, *disp))

    ps = s.places(ctl)
    used = {p[2] for p in ps}
    named = {p[5]: p[2] for p in ps if p[5]}
    alltabs = named['AllTabs']
    d0 = max(named.get(n, 0) for n in ('AllTabs', 'btnCustom', 'btnPresets')) + 1
    while any(d in used for d in range(d0, d0 + 8)):
        d0 += 1
    ours = [(d0, row_id, 0, 0, 'PCTouch'),
            (d0 + 1, row, bx + row_ts[0][0], by + row_ts[0][1], 'PCTilt'),
            (d0 + 2, row, bx + row2_ts[0][0], by + row2_ts[0][1], 'PCGas'),
            (d0 + 3, selt, bx + selt_ts[0][0], by + selt_ts[0][1], 'PCGearTitle'),
            (d0 + 4, sel, bx + sel_ts[0][0], by + sel_ts[0][1], 'PCGear'),
            (d0 + 5, large, 512 * PX - ctl_t[0], 556 * PX - ctl_t[1], 'PCKeysOpen'),
            (d0 + 6, keys_page, 0, 0, 'PCKeys'),
            (d0 + 7, disp_page, 0, 0, 'PCDisplay')]

    out = []
    for code, body, raw in s.top:
        if code == 39 and struct.unpack_from('<H', body)[0] == ctl:
            sid, frames = struct.unpack_from('<HH', body)
            new, first = [], True
            for c, b, r in tags(body, 4, len(body)):
                if c == 1 and first:
                    first = False
                    new += [place(*o) for o in ours]
                new.append(r)
                if c in (26, 70):
                    f, depth, cid, t, name = placement(c, b)
                    if depth == alltabs and f & 8 and not f & 2:
                        for o in ours:
                            bb = bytearray(b)
                            struct.pack_into('<H', bb, 2 if c == 70 else 1, o[0])
                            new.append(tag(c, bytes(bb)))
            out.append(tag(39, touch))
            out += defs
            out.append(tag(39, struct.pack('<HH', sid, frames) + b''.join(new)))
        else:
            out.append(raw)
    body = s.d[8:s.start] + b''.join(out)
    res = b'FWS' + s.d[3:4] + struct.pack('<I', 8 + len(body)) + body
    open(dst, 'wb').write(res)
    print('%s: ControlsMenu (sprite %d) rows at depths %d..%d, touch row sprite %d, camera rows from %d,%d, '
          'keys page %d, display page %d (ids %d..%d)' % (dst, ctl, d0, d0 + 7, row_id, bx, by, keys_page,
                                                          disp_page, row_id, nid[0]))


if __name__ == '__main__':
    main()
