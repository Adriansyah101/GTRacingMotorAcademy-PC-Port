"""APP_ICON: the game's icon for gtracing.exe, made from iTunesArtwork.

Makes gtracing.exe's icon from an iTunesArtwork (the App Store picture):
    python -I tools/make_icon.py <iTunesArtwork> <out.ico> [corner radius / width]
host/build.py runs it into build\\host\\app.ico and links it in as icon 1 (windres), the icon Explorer and the
taskbar show; SDL takes the window's icon from the exe's first icon.  It is Gameloft's picture: made on the
builder's PC from their own files, never in the repository.

Which artwork: the iTunesArtwork at the IPA's top level (outside the .app), copied into the project folder:
a 600 x 600 JPEG of the Red Bull F1 car, as the game's home-screen icons (Icon.png, Icon-72@2x.png) are.
The one inside the bundle is an older 512 x 512 PNG of a yellow Camaro that Gameloft never updated, so it does
not match the icon players know.

The artwork is a full square drawn for iOS's rounded-corner mask, so outside a rounded rectangle it is made
transparent, with antialiasing: radius 0.16 of the width (iOS's own: 23 of 144) for the Red Bull picture,
whose grey frame runs into the corners; 0.203 for the Camaro one, where its frame's outer edge meets the
diagonal.  Sizes 256 (PNG in the .ico), 128, 64, 48, 32, 24, 16 (32-bit bitmaps), each an area average of
the full one.  No Pillow: a PNG is decoded here; a JPEG is turned into a PNG by Windows (System.Drawing,
through PowerShell), as the host is built on Windows anyway.
"""
import os
import subprocess
import struct
import sys
import tempfile
import zlib

SIZES = (256, 128, 64, 48, 32, 24, 16)
RADIUS = 0.16


def jpeg_to_png(src, dst):
    # the paths go in the environment: -Command joins its arguments into one line, which splits paths with spaces
    ps = ('$ErrorActionPreference = "Stop"; Add-Type -AssemblyName System.Drawing; '
          '$i = [System.Drawing.Image]::FromFile($env:ICON_SRC); '
          '$i.Save($env:ICON_DST, [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose()')
    env = dict(os.environ, ICON_SRC=os.path.abspath(src), ICON_DST=os.path.abspath(dst))
    p = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', ps],
                       env=env, capture_output=True, text=True)
    if p.returncode or os.path.getsize(dst) == 0:
        raise RuntimeError('JPEG -> PNG through PowerShell failed: %s' % (p.stdout + p.stderr).strip())


def picture_read(path):
    with open(path, 'rb') as f:
        head = f.read(3)
    if head != b'\xff\xd8\xff':
        return png_read(path)
    fd, tmp = tempfile.mkstemp(suffix='.png')
    os.close(fd)
    try:
        jpeg_to_png(path, tmp)
        return png_read(tmp)
    finally:
        os.remove(tmp)


def png_read(path):
    b = open(path, 'rb').read()
    if b[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    p, idat, w = 8, b'', 0
    while p < len(b):
        n, = struct.unpack('>I', b[p:p + 4])
        t, d = b[p + 4:p + 8], b[p + 8:p + 8 + n]
        p += 12 + n
        if t == b'CgBI':
            raise ValueError('an iOS-optimised PNG (CgBI)')
        if t == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', d)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError('PNG depth %d type %d interlace %d' % (depth, ctype, interlace))
            bpp = 3 if ctype == 2 else 4
        elif t == b'IDAT':
            idat += d
    raw = zlib.decompress(idat)
    st = w * bpp
    prev = bytearray(st)
    rgba = bytearray()
    o = 0
    for _ in range(h):
        f = raw[o]
        line = bytearray(raw[o + 1:o + 1 + st])
        o += 1 + st
        if f == 1:
            for i in range(bpp, st):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(st):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(st):
                line[i] = (line[i] + (((line[i - bpp] if i >= bpp else 0) + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(st):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                u = prev[i]
                pp = a + u - c
                pa, pb, pc = abs(pp - a), abs(pp - u), abs(pp - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (u if pb <= pc else c))) & 255
        prev = line
        if bpp == 4:
            rgba += line
        else:
            for x in range(w):
                rgba += line[x * 3:x * 3 + 3] + b'\xff'
    return w, h, rgba


def round_corners(w, h, px, r):
    """alpha 0 outside a rounded rectangle of radius r, antialiased over a pixel (4 x 4 samples)"""
    for y in range(h):
        for x in range(w):
            cx = r if x < r else (w - r if x >= w - r else None)
            cy = r if y < r else (h - r if y >= h - r else None)
            if cx is None or cy is None:
                continue
            inside = 0
            for sy in range(4):
                for sx in range(4):
                    dx, dy = x + (sx + .5) / 4 - cx, y + (sy + .5) / 4 - cy
                    inside += dx * dx + dy * dy <= r * r
            i = (y * w + x) * 4 + 3
            px[i] = px[i] * inside // 16
    return px


def shrink(w, h, px, n):
    """area average to n x n (alpha-weighted colours)"""
    out = bytearray()
    for ty in range(n):
        y0, y1 = ty * h / n, (ty + 1) * h / n
        for tx in range(n):
            x0, x1 = tx * w / n, (tx + 1) * w / n
            acc = [0.0] * 4
            tot = 0.0
            for y in range(int(y0), min(h, int(y1 + .999999))):
                wy = min(y + 1, y1) - max(y, y0)
                if wy <= 0:
                    continue
                for x in range(int(x0), min(w, int(x1 + .999999))):
                    wx = min(x + 1, x1) - max(x, x0)
                    if wx <= 0:
                        continue
                    k = wx * wy
                    i = (y * w + x) * 4
                    a = px[i + 3] * k
                    acc[0] += px[i] * a
                    acc[1] += px[i + 1] * a
                    acc[2] += px[i + 2] * a
                    acc[3] += a
                    tot += k
            a = acc[3]
            out += bytes((int(acc[0] / a + .5), int(acc[1] / a + .5), int(acc[2] / a + .5), int(a / tot + .5))
                         if a else (0, 0, 0, 0))
    return out


def png_write(n, px):
    rows = b''.join(b'\0' + bytes(px[y * n * 4:(y + 1) * n * 4]) for y in range(n))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def bmp_entry(n, px):
    """the .ico's bitmap form: BITMAPINFOHEADER (height doubled for the mask), BGRA bottom row first, AND mask"""
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    body = bytearray()
    for y in range(n - 1, -1, -1):
        for x in range(n):
            i = (y * n + x) * 4
            body += bytes((px[i + 2], px[i + 1], px[i], px[i + 3]))
    stride = ((n + 31) // 32) * 4
    mask = bytearray()
    for y in range(n - 1, -1, -1):
        row = bytearray(stride)
        for x in range(n):
            if px[(y * n + x) * 4 + 3] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    return hdr + bytes(body) + bytes(mask)


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        return 2
    radius = float(sys.argv[3]) if len(sys.argv) > 3 else RADIUS
    w, h, px = picture_read(sys.argv[1])
    if w != h:
        raise ValueError('artwork %dx%d is not square' % (w, h))
    px = round_corners(w, h, px, radius * w)
    entries = []
    for n in SIZES:
        small = shrink(w, h, px, n)
        entries.append((n, png_write(n, small) if n >= 256 else bmp_entry(n, small)))
    out = bytearray(struct.pack('<HHH', 0, 1, len(entries)))
    off = 6 + 16 * len(entries)
    for n, data in entries:
        out += struct.pack('<BBBBHHII', n & 255, n & 255, 0, 0, 1, 32, len(data), off)
        off += len(data)
    for _, data in entries:
        out += data
    open(sys.argv[2], 'wb').write(out)
    print('%s: %d sizes (%s), %d bytes' % (sys.argv[2], len(entries), ', '.join(str(n) for n in SIZES), len(out)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
