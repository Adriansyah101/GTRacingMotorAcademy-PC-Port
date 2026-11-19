"""swfpatch_cashnew.py <menu.FAK> <out.FAK> -- DYNAMIC_PRICE: a NEW label on the title bar's cash.

A NEW label marks the title bar's cash icon while a cash price the player has not seen is on offer.  Every
title bar of the main menu file (the sprites placing "cashBoard": the main menu's own and the one the other menus
share) gets "cashNew", a copy of the main menu's own "NEW!" label (the sprite NavigationMenu places as
NotificationNew: its text SP_NAVIGATION_MENU60, yellow, slanted), smaller, over the cash icon's top left corner.
Everything is found by name (the files number their characters differently).  host/games/gtracing/render/widescreen.c draws it only
while host/games/gtracing/features/shop.c's shop_cash_new() says the cash rate is one the player has not seen in the shop.
Applied after the other main menu patches, to the main menu files only."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from swfpatch_controls import Swf, tag  # noqa: E402
from swfpatch_exit import build_place2, parse_place2, sprite_tags  # noqa: E402

PX = 20
NAME = b'cashNew'
SCALE = 1.6                      # NotificationNew is 2.04 in the side menu
AT = (2, 0)                      # px from the cash board's origin (the cash icon's top left)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    s = Swf(open(src, 'rb').read())
    label, slant = None, 0.0
    bars = []
    for sid, body in s.sprites.items():
        for c, b, _ in sprite_tags(body):
            if c != 26 or not b[0] & 0x20:
                continue
            p = parse_place2(b)
            if p.get('name') == b'NotificationNew' and 'char' in p and label is None:
                label = p['char']
                sx, sy, r0, r1 = p['matrix'][:4]
                slant = r1 / sy
            if p.get('name') == b'cashBoard':
                bars.append(sid)
            if p.get('name') == NAME:
                raise SystemExit('%s: already has %s' % (src, NAME.decode()))
    if label is None or not bars:
        # the _EdAXiF layout (the iPhone's) has no NotificationNew: left as it is, without the label
        if src != dst:
            open(dst, 'wb').write(s.d)
        print('%s: no NotificationNew (%s) or no cashBoard (%s): no %s' %
              (os.path.basename(dst), label, bars, NAME.decode()))
        return
    sc = int(SCALE * 65536)
    out = []
    for code, body, raw in s.top:
        sid = struct.unpack_from('<H', body)[0] if code == 39 else None
        if sid not in bars:
            out.append(raw)
            continue
        ts = sprite_tags(body)
        places = [parse_place2(b) for c, b, _ in ts if c == 26]
        used = {p['depth'] for p in places}
        board = next(p for p in places if p.get('name') == b'cashBoard')
        depth = board['depth'] + 1
        while depth in used:
            depth += 1
        tx, ty = board['matrix'][4:6]
        new, placed = [], False
        for c, b, r in ts:
            if c == 1 and not placed:
                new.append(build_place2({'flags': 0x26, 'depth': depth, 'char': label,
                                         'matrix': (sc, sc, 0, int(slant * sc), tx + AT[0] * PX, ty + AT[1] * PX),
                                         'name': NAME}))
                placed = True
            new.append(r)
        out.append(tag(39, body[:4] + b''.join(new)))
        print('%s: %s (NEW label %d) in the title bar sprite %d at depth %d' %
              (os.path.basename(dst), NAME.decode(), label, sid, depth))
    body = s.d[8:s.start] + b''.join(out)
    open(dst, 'wb').write(b'FWS' + s.d[3:4] + struct.pack('<I', 8 + len(body)) + body)


if __name__ == '__main__':
    main()
