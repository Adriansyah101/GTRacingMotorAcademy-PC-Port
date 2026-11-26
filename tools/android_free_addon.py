import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hyundai_addon as H  # noqa: E402

ROOT = H.ROOT
SRC = os.path.join(ROOT, 'GTRacingFreeAndroid1.4.3', 'com.gameloft.android.ANMP.GloftGTFM')
IPAD = os.path.join(ROOT, 'GTRacingFreemiumHD.app')
OUT = os.path.join(ROOT, 'build', 'host', 'add-on')

CARS = [
    ('dodge', 'DSREDSFAEZC4HIBUTAGOEZTAADA1AAAD', None,
     'DSREDSFAEZC4HIBUTAGOEZTAADA1AAAD', 'BdBQi_cLZEEiNQiX_FXw8_2012'),
    ('ferrari', 'FAEZTATABUTAK0ADBSBSGOVWRE', 'FAEZTATABUTAK0ADBSBSGOVWREAABTBSB0',
     'FAEZTATABUTAK0ADBSBSGOVWREAABTBSB0', 'KiXXZXl_458_2010'),
]
OBJECTS = 'REBWLOEZC4VWUK'
TEXTURES = 'VWEZY3VWWUTAEZUK'


def pack_ids(path, wide):
    with open(path, 'rb') as f:
        n, = struct.unpack('<H', f.read(2))
        fmt, size = ('<II', 8) if wide else ('<IH', 6)
        return {struct.unpack(fmt, f.read(size))[1] for _ in range(n)}


def sprite_tex(s):
    return struct.unpack_from('<H', s, 6)[0]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else SRC
    out = sys.argv[2] if len(sys.argv) > 2 else OUT
    if not os.path.isdir(src):
        print('no Android data folder at %s' % src)
        return 1
    rd = lambda d, n: open(os.path.join(d, n), 'rb').read()
    have = {}
    for v in ('', H.HIRES):
        have[v] = (pack_ids(os.path.join(IPAD, OBJECTS + v + '.ba5'), True),
                   pack_ids(os.path.join(IPAD, TEXTURES + v + '.ba4'), False))
    for brand, name, cockpit, here, donor in CARS:
        bdir = os.path.join(out, brand)
        os.makedirs(bdir, exist_ok=True)
        d = H.car_parse(rd(IPAD, donor + '.ba01'))

        def fix(secs, rec):
            for k in (4, 5, 7):
                secs[k] = d[k]
            w13 = struct.unpack_from('<i', rec, 1 + 13 * 4)[0]
            if w13 == 60:
                struct.pack_into('<i', rec, 1 + 13 * 4, 120)
            struct.pack_into('<i', rec, 1 + 35 * 4, 0)
            w = struct.unpack_from('<38i', rec, 1)
            if w[1] != struct.unpack_from('<i', d[0], 5)[0]:
                raise ValueError('%s: brand %d, the donor is another' % (name, w[1]))
            need_t = {w[21], w[21] + 1, sprite_tex(secs[6])}
            for v in ('', H.HIRES):
                o, t = have[v]
                if w[5] not in o or not need_t <= t:
                    raise ValueError('%s: the iPad pack%s lacks model %d or textures %s'
                                     % (name, v, w[5], sorted(need_t - t)))
        a = rd(src, name + '.ba0')
        model = struct.unpack_from('<i', H.car_parse(a)[0], 1 + 5 * 4)[0]
        c, f = H.car_to_c(a, model, fix)
        open(os.path.join(bdir, here + '.ba01'), 'wb').write(c)
        for ext in ('ba7', 'ba8'):
            data = rd(src, '%s.%s' % (cockpit, ext)) if cockpit else rd(IPAD, '%s.%s' % (donor, ext))
            open(os.path.join(bdir, '%s.%s' % (here, ext)), 'wb').write(data)
        print('%s\\%s: car %d, brand %d, year %d, model %d, cockpit %s'
              % (brand, here, f[0], f[1], f[2], f[5], 'Android' if cockpit else donor))
    return 0


if __name__ == '__main__':
    sys.exit(main())
