"""ANDROID_FREE_CARS add-on: the two cars only the Android GT Racing Free+ has, built on the models and
textures the iPad packs already hold.

Builds build\\host\\add-on\\dodge\\ and build\\host\\add-on\\ferrari\\ (one folder per brand) from the Android
GT Racing Free+ 1.4.3 data folder
(com.gameloft.android.ANMP.GloftGTFM, loose files like the iPad bundle). It holds the two cars that only the
Android build has: car 340, the Dodge Charger 2012, and car 341, the Ferrari 288 GTO. These are Gameloft's
files, so they are never shipped and never put in the repository.

    python -I tools/android_free_addon.py [android data folder] [add-on folder]

The iPad's own packs already hold these two cars' models (objects 340 / 341) and textures (23800-23803,
23850, 23900-23903, 23950), but no iPad car file uses them. So the add-on is only the car files and the
cockpits; the models and textures are the iPad's. The Android car file is version 'A', made 'C' as in
tools/hyundai_addon.py (car_to_c).

Checked against the 133 cars that both builds have:
- the sections 1-6 match, so the model, the picture sprite and the logos are the same;
- the record differs in two fields the same way on every car. Word 13 is 60 on Android and 120 on the
  iPad (a 96 stays 96). Word 35 is a time limit for a sale (432000 or 604800 seconds) on Android and 0 on
  every iPad car. Both are set as the iPad has them;
- the flag (section 7) is on another atlas (the iPad's flags are 42 x 32, see hyundai_addon.py), so the
  logo and flag sprites are taken from an iPad car of the same brand.

The cockpit is found from the car file's name (<name>.ba7 / .ba8, HD / not). The Ferrari's Android cockpit
has "1984" after the car's name, so its car file gets that name too. The Charger has no cockpit of its own
on Android, so it uses the iPad's Dodge Challenger SRT8's cockpit.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hyundai_addon as H  # noqa: E402

ROOT = H.ROOT
SRC = os.path.join(ROOT, 'GTRacingFreeAndroid1.4.3', 'com.gameloft.android.ANMP.GloftGTFM')
IPAD = os.path.join(ROOT, 'GTRacingFreemiumHD.app')
OUT = os.path.join(ROOT, 'build', 'host', 'add-on')

# (brand folder, Android car file, Android cockpit or None, name here, iPad car of the same brand)
CARS = [
    ('dodge', 'DSREDSFAEZC4HIBUTAGOEZTAADA1AAAD', None,                         # dodgecharger2012
     'DSREDSFAEZC4HIBUTAGOEZTAADA1AAAD', 'BdBQi_cLZEEiNQiX_FXw8_2012'),         # Dodge Challenger SRT8 2012
    ('ferrari', 'FAEZTATABUTAK0ADBSBSGOVWRE', 'FAEZTATABUTAK0ADBSBSGOVWREAABTBSB0',  # ferrari288gto(1984)
     'FAEZTATABUTAK0ADBSBSGOVWREAABTBSB0', 'KiXXZXl_458_2010'),
]
OBJECTS = 'REBWLOEZC4VWUK'        # "objects" (.ba5)
TEXTURES = 'VWEZY3VWWUTAEZUK'     # "textures" (.ba4)


def pack_ids(path, wide):
    """the ids in a pack's index (the index only; the packs are 100+ MB)"""
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
            for k in (4, 5, 7):                       # small / large logo, flag: the iPad atlases
                secs[k] = d[k]
            w13 = struct.unpack_from('<i', rec, 1 + 13 * 4)[0]
            if w13 == 60:
                struct.pack_into('<i', rec, 1 + 13 * 4, 120)
            struct.pack_into('<i', rec, 1 + 35 * 4, 0)
            w = struct.unpack_from('<38i', rec, 1)
            if w[1] != struct.unpack_from('<i', d[0], 5)[0]:
                raise ValueError('%s: brand %d, the donor is another' % (name, w[1]))
            need_t = {w[21], w[21] + 1, sprite_tex(secs[6])}   # body + its paint pair (0x1a0b70), picture
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
