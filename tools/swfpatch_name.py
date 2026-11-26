import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from swfpatch_controls import BR, Swf, tag  # noqa: E402
from swfpatch_exit import (blur, build_place2, bitmap_shape, cover, lossless2, parse_place2, png,  # noqa: E402
                           rect_bytes, sd_round_rect, sprite_tags)

PX = 20
NAME = 'btnPCName'
W, H = 250.0, 56.0
RES = 2
PAD = 1
RADIUS, EDGE = 9.0, 2.6
AT = (162, 640)
TEXT_SCALE = 0.72
TEXT_AT = (-12, -12)


def sd_person(x, y):
    head = math.hypot(x, y + 7.5) - 6.2
    qx, qy = abs(x) - 11.0 + 6.0, abs(y - 10.0) - 6.5 + 6.0
    body = math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - 6.0
    body = max(body, -(y - 3.0) - 0.0) if y < 3.0 else body
    return min(head, max(body, 3.0 - y))


def picture():
    w = int(round(W * RES)) + 2 * PAD
    h = int(round(H * RES)) + 2 * PAD
    px = 1.0 / RES
    hw, hh = W / 2, H / 2
    icx, icy = -hw + 27.0, 1.0
    sh = [0.0] * (w * h)
    for j in range(h):
        for i in range(w):
            x, y = -hw + (i - PAD + 0.5) * px, -hh + (j - PAD + 0.5) * px
            sh[j * w + i] = cover(sd_person(x - icx, y - icy - 0.8) - 1.2, px)
    sh = blur(sh, w, h, max(1, int(1.2 * RES)))
    out = bytearray()
    for j in range(h):
        for i in range(w):
            x, y = -hw + (i - PAD + 0.5) * px, -hh + (j - PAD + 0.5) * px
            d_out = sd_round_rect(x, y, 0, 0, hw, hh, RADIUS)
            a = cover(d_out, px)
            if a <= 0:
                out += b'\0\0\0\0'
                continue
            t = min(max((y + hh) / H, 0.0), 1.0)
            g = 58 - 40 * t if t > 0.45 else 74 - 30 * t
            col = [g, g, g + 2]
            s = sh[j * w + i] * 0.7
            col = [c * (1 - s) for c in col]
            d_p = sd_person(x - icx, y - icy)
            face = cover(d_p, px)
            gl = 250 - 30 * min(max((y - icy + 14) / 28, 0), 1)
            col = [c * (1 - face) + gl * face for c in col]
            e = cover(d_out + EDGE, px)
            yel = (252, 205 - 30 * t, 20)
            col = [c * e + v * (1 - e) for c, v in zip(col, yel)]
            dark = cover(abs(d_out + EDGE) - 0.5, px) * 0.6
            col = [c * (1 - dark) for c in col]
            out += bytes([int(round(min(max(c, 0), 255))) for c in col] + [int(round(a * 255))])
    return w, h, bytes(out)


NAME_BOX = (426, 56, 4)
CODE_BOX = (500, 52, -4)
TEXT_H = 480
INK = (0x20, 0x20, 0x20, 0xff)


def edit_rect(b):
    r = BR(b, 2)
    n = r.u(5)
    rect = [r.sg(n) for _ in range(4)]
    return rect, r.end()


def place_of(s, sid, name):
    for c, b, _ in sprite_tags(s.sprites[sid]):
        if c == 26:
            p = parse_place2(b)
            if p.get('name') == name.encode() and p['flags'] & 2:
                return p
    raise SystemExit('no %s in sprite %d' % (name, sid))


def input_sprite(sprite_id, edit_id, shape_id, shape_w, shape_h, font, w, h, gap):
    fx = int(w * 0.03 + 4)
    fw = (w - 2 * fx) * PX
    edit = struct.pack('<H', edit_id) + rect_bytes(-40, fw, -40, 619)
    edit += bytes([0x0d, 0x31])
    edit += struct.pack('<HH', font, TEXT_H) + bytes(INK)
    edit += bytes([2]) + struct.pack('<HHHh', 0, 0, 0, 40) + b'\0'
    stop = tag(12, b'\x07\x00')
    body = struct.pack('<HH', sprite_id, 2)
    body += tag(43, b'Off\0') + stop + tag(1, b'')
    body += tag(43, b'On\0')
    body += build_place2({'flags': 0x06, 'depth': 1, 'char': shape_id,
                          'matrix': (int(w / shape_w * 65536), int(h / shape_h * 65536), 0, 0, 0, 0)})
    body += build_place2({'flags': 0x26, 'depth': 2, 'char': edit_id,
                          'matrix': (65536, 65536, 0, 0, fx * PX, int(h * 14 / 56 * PX)), 'name': b'field'})
    body += stop + tag(1, b'') + tag(0, b'')
    return [tag(37, edit), tag(39, body)]


def input_boxes(s, menu, first_id):
    gl, _ = s.child(None, 'GLLiveLoginMenu')
    allbox, _ = s.child(gl, 'AllBox')
    pn = place_of(s, allbox, 'Profile_name')['char']
    shape = edit = None
    for c, b, _ in sprite_tags(s.sprites[pn]):
        if c == 26:
            p = parse_place2(b)
            if p['flags'] & 2 and p['depth'] == 1:
                shape = p['char']
            if p['flags'] & 2 and p.get('name') == b'field':
                edit = p['char']
    shape_tag = edit_tag = None
    for c, b, _ in s.top:
        if len(b) >= 2 and struct.unpack_from('<H', b)[0] == shape and c in (2, 22, 32, 83):
            shape_tag = (c, b)
        if c == 37 and struct.unpack_from('<H', b)[0] == edit:
            edit_tag = b
    if not shape_tag or not edit_tag:
        raise SystemExit('no Gameloft LIVE input box (shape %s, field %s)' % (shape, edit))
    srect, _ = edit_rect(shape_tag[1])
    shape_w, shape_h = (srect[1] - srect[0]) / PX, (srect[3] - srect[2]) / PX
    erect, k = edit_rect(edit_tag)
    if not edit_tag[k] & 1:
        raise SystemExit('the Gameloft LIVE field has no font')
    font = struct.unpack_from('<H', edit_tag, k + 2)[0]
    box_shape, name_edit, name_spr, code_edit, code_spr, first_edit, first_spr = (first_id + i for i in range(7))
    defs = [tag(shape_tag[0], struct.pack('<H', box_shape) + shape_tag[1][2:])]
    defs += input_sprite(name_spr, name_edit, box_shape, shape_w, shape_h, font, *NAME_BOX)
    defs += input_sprite(code_spr, code_edit, box_shape, shape_w, shape_h, font, *CODE_BOX)
    defs += input_sprite(first_spr, first_edit, box_shape, shape_w, shape_h, font, *NAME_BOX)

    def field_geom(text_char):
        for c, b, _ in s.top:
            if c == 37 and struct.unpack_from('<H', b)[0] == text_char:
                r, _ = edit_rect(b)
                return 0.0, 0.0, r
        p = place_of(s, text_char, 'field')
        fx, fy, r = field_geom(p['char'])
        return p['matrix'][4] + fx, p['matrix'][5] + fy, r

    def follow_texts(popup, box_spr, what):
        texts = place_of(s, popup, 'Texts')
        fx, fy, r = field_geom(texts['char'])
        w, h, gap = NAME_BOX
        dx = int(fx + (r[0] + r[1]) / 2 - w * PX / 2)
        dy = int(fy + r[2] + (31 + gap) * PX)
        ts = sprite_tags(s.sprites[popup])
        used = {parse_place2(b)['depth'] for c, b, _ in ts if c == 26}
        depth = texts['depth'] + 1
        while depth in used:
            depth += 1
        out, n = [], 0
        for c, b, raw in ts:
            out.append(raw)
            if c == 28 and struct.unpack_from('<H', b)[0] == texts['depth']:
                out.append(tag(28, struct.pack('<H', depth)))
            if c != 26:
                continue
            p = parse_place2(b)
            if p['depth'] != texts['depth'] or not p['flags'] & 4:
                continue
            m = p['matrix']
            q = {'flags': p['flags'] & 0x0d, 'depth': depth,
                 'matrix': (m[0], m[1], m[2], m[3], m[4] + dx, m[5] + dy)}
            if p['flags'] & 8:
                q['cxform'] = p['cxform']
            if p['flags'] & 2:
                q['flags'] |= 0x22
                q['char'], q['name'] = box_spr, b'Input'
            out.append(build_place2(q))
            n += 1
        new[popup] = struct.pack('<HH', *struct.unpack_from('<HH', s.sprites[popup])) + b''.join(out)
        print('%s box: sprite %d depth %d, %d placements, at Texts %+d,%+d px'
              % (what, popup, depth, n, dx // PX, dy // PX))

    new = {}
    ntu = place_of(s, menu, 'needToUpdate')['char']
    follow_texts(ntu, name_spr, 'name form (GameTypeMenu.needToUpdate)')
    nav, _ = s.child(None, 'NavigationMenu')
    fp = place_of(s, nav, 'FlurryPopup')['char']
    follow_texts(fp, first_spr, 'first start name form (NavigationMenu.FlurryPopup)')

    jm, _ = s.child(None, 'JoinMenu')
    dp = place_of(s, jm, 'DisconnectedPopup')['char']
    pc = place_of(s, dp, 'PopupCongrats')['char']
    tc = place_of(s, pc, 'TxtCongrats')
    _, _, r = field_geom(tc['char'])
    w, h, gap = CODE_BOX
    x = int(tc['matrix'][4] + (r[0] + r[1]) / 2 - w * PX / 2)
    y = int(tc['matrix'][5] + r[3] + gap * PX)
    ts = sprite_tags(s.sprites[pc])
    used = {parse_place2(b)['depth'] for c, b, _ in ts if c == 26}
    depth = max(used) + 1
    out, placed = [], False
    for c, b, raw in ts:
        if c == 1 and not placed:
            out.append(build_place2({'flags': 0x26, 'depth': depth, 'char': code_spr,
                                     'matrix': (65536, 65536, 0, 0, x, y), 'name': b'Input'}))
            placed = True
        out.append(raw)
    new[pc] = struct.pack('<HH', *struct.unpack_from('<HH', s.sprites[pc])) + b''.join(out)
    print('room code form box: PopupCongrats (sprite %d) depth %d at %d,%d px; Gameloft LIVE box shape %d '
          '(%.0fx%.0f px), font %d' % (pc, depth, x // PX, y // PX, shape, shape_w, shape_h, font))
    order = [struct.unpack_from('<H', b)[0] for c, b, _ in s.top if c == 39]
    before = min((ntu, pc, fp), key=order.index)
    return new, defs, before, 7


def main():
    src, dst = sys.argv[1], sys.argv[2]
    s = Swf(open(src, 'rb').read())
    menu = tile = None
    for sid, body in s.sprites.items():
        names = {parse_place2(b).get('name'): parse_place2(b).get('char') for c, b, _ in sprite_tags(body)
                 if c == 26 and b[0] & 0x22 == 0x22}
        if b'btnBluetooth' in names and b'btnWireless' in names:
            menu, tile = sid, names[b'btnBluetooth']
    if not menu:
        raise SystemExit('no GameTypeMenu in %s' % src)
    art = title = None
    for c, b, _ in sprite_tags(s.sprites[tile]):
        if c == 26 and b[0] & 2:
            p = parse_place2(b)
            if p['depth'] == 2:
                art = p['char']
            if p.get('name') == b'txtTitle':
                title = p
    ids = set(s.sprites) | set(s.texts)
    for c, b, _ in s.top:
        if c in (2, 6, 10, 11, 14, 20, 21, 22, 32, 33, 34, 35, 36, 37, 39, 46, 48, 60, 75, 83, 84, 90, 200) \
                and len(b) >= 2:
            ids.add(struct.unpack_from('<H', b)[0])
    bid, shid, tid, xid = (max(ids) + k for k in range(1, 5))
    boxes, box_defs, box_before, _ = input_boxes(s, menu, max(ids) + 5)

    w, h, rgba = picture()
    if '--png' in sys.argv:
        png(sys.argv[sys.argv.index('--png') + 1], w, h, rgba)
    scale = PX // RES
    x0, y0 = -w * scale // 2, -h * scale // 2
    defs = [lossless2(bid, w, h, rgba),
            bitmap_shape(shid, bid, x0, y0, x0 + w * scale, y0 + h * scale, scale),
            tag(39, struct.pack('<HH', tid, 1) +
                build_place2({'flags': 0x06, 'depth': 1, 'char': shid, 'matrix': (65536, 65536, 0, 0, 0, 0)}) +
                tag(1, b'') + tag(0, b''))]

    k = int(TEXT_SCALE * 65536)
    t0 = title['matrix']
    ttx, tty = TEXT_AT[0] * PX, TEXT_AT[1] * PX
    hz = (int(W * PX / 4953 * 65536), int(H * PX / 4040 * 65536), 0, 0,
          int(-W * PX / 2 + 1393 * W * PX / 4953), int(-H * PX / 2 + 1400 * H * PX / 4040))
    frames = struct.unpack_from('<HH', s.sprites[tile])[1]
    new, frame, cut = [], 0, 0
    for c, b, r in sprite_tags(s.sprites[tile]):
        if c == 1:
            frame += 1
        if c == 26:
            p = parse_place2(b)
            if p['depth'] == 2 and p['flags'] & 2:
                p['char'] = tid
                p['matrix'] = (65536, 65536, 0, 0, 0, 0)
                new.append(build_place2(p))
                continue
            if p['depth'] == 2 and p['flags'] & 4:
                m = p['matrix']
                p['matrix'] = (m[0], m[1], 0, 0, 0, 0)
                new.append(build_place2(p))
                continue
            if p['depth'] == 3 and p['flags'] & 4:
                m = p['matrix']
                f = m[0] / t0[0]
                p['matrix'] = (int(k * f), int(k * f), 0, 0, int(ttx * f), int(tty * f))
                new.append(build_place2(p))
                continue
            if p['depth'] == 7:
                if p['flags'] & 2:
                    p['matrix'] = hz
                    new.append(build_place2(p))
                elif p['flags'] & ~0x05 & 0xff:
                    p['flags'] &= ~4
                    p.pop('matrix', None)
                    new.append(build_place2(p))
                continue
        if c == 12 and (b'_root' in b or b'PushMenu' in b):
            cut += 1
            new.append(tag(12, b'\x8c\x05\x00Idle\x00\x07\x00' if b'PushMenu' in b else b'\x00'))
            continue
        new.append(r)
    if cut != 3:
        raise SystemExit('the Bluetooth tile %d: %d menu actions (3 expected)' % (tile, cut))
    defs.append(tag(39, struct.pack('<HH', xid, frames) + b''.join(new)))

    out = []
    for code, body, raw in s.top:
        if code == 39 and struct.unpack_from('<H', body)[0] == box_before:
            out += box_defs
        if code == 39 and struct.unpack_from('<H', body)[0] in boxes:
            out.append(tag(39, boxes[struct.unpack_from('<H', body)[0]]))
            continue
        if code == 39 and struct.unpack_from('<H', body)[0] == menu:
            out += defs
            msid, mframes = struct.unpack_from('<HH', body)
            ts = sprite_tags(body)
            used = {parse_place2(b)['depth'] for c, b, _ in ts if c == 26}
            depth = 13
            while depth in used:
                depth += 1
            new, placed = [], False
            for c, b, r in ts:
                if c == 1 and not placed:
                    new.append(build_place2({'flags': 0x26, 'depth': depth, 'char': xid,
                                             'matrix': (65536, 65536, 0, 0, AT[0] * PX, AT[1] * PX),
                                             'name': NAME.encode()}))
                    placed = True
                new.append(r)
            out.append(tag(39, struct.pack('<HH', msid, mframes) + b''.join(new)))
            print('%s: %s in GameTypeMenu (sprite %d) at depth %d' % (os.path.basename(dst), NAME, msid, depth))
        else:
            out.append(raw)
    body = s.d[8:s.start] + b''.join(out)
    open(dst, 'wb').write(b'FWS' + s.d[3:4] + struct.pack('<I', 8 + len(body)) + body)
    print('%s: name bar %dx%d (bitmap %d, shape %d, bar %d, button %d from the Bluetooth tile %d, art %s)' %
          (dst, w, h, bid, shid, tid, xid, tile, art))


if __name__ == '__main__':
    main()
