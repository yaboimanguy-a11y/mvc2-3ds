#!/usr/bin/env python3
"""make_cia_banner.py OUT_DIR [LOGO.png]: the CIA's home-menu banner inputs:
banner.png (256x128), icon.png (48x48), banner.wav (a short two-note chime,
22050 Hz mono 16-bit).  With LOGO (an 8-bit RGBA PNG, the project's
port/platform/3ds/cia/logo.png) the banner is the logo on a dark background
and on the icon; without it, block letters drawn here.
bannertool turns them into the banner and the SMDH (port/CMakeLists.txt,
target mvc2_cia)."""
import math
import os
import struct
import sys
import wave
import zlib

# 5x7 block letters for the title
FONT = {
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "C": ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    "2": ["01110", "10001", "00001", "00110", "01000", "10000", "11111"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    " ": ["00000"] * 7,
}


def png(path, w, h, px):
    raw = b"".join(b"\x00" + bytes(c for p in px[y * w:(y + 1) * w] for c in p) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def canvas(w, h):
    px = []
    for y in range(h):
        for x in range(w):
            t = (x + y) / (w + h)
            r, g, b = int(120 * (1 - t) + 30 * t), int(36 * (1 - t) + 70 * t), int(160 * (1 - t) + 180 * t)
            if ((x - y) // 6) % 4 == 0:  # diagonal stripes
                r, g, b = min(255, r + 25), min(255, g + 25), min(255, b + 25)
            px.append((r, g, b, 255))
    return px


def text(px, w, s, x0, y0, scale, col):
    for i, ch in enumerate(s):
        for ry, row in enumerate(FONT[ch]):
            for rx, bit in enumerate(row):
                if bit == "1":
                    for dy in range(scale):
                        for dx in range(scale):
                            x, y = x0 + (i * 6 + rx) * scale + dx, y0 + ry * scale + dy
                            px[y * w + x] = col


def png_read(path):
    """8-bit RGBA / RGB, non-interlaced PNG -> (w, h, [(r, g, b, a)])"""
    d = open(path, "rb").read()
    pos, idat, w = 8, b"", 0
    while pos < len(d):
        n, t = struct.unpack(">I4s", d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, bd, ct, _, _, il = struct.unpack(">IIBBBBB", body)
            if bd != 8 or ct not in (2, 6) or il:
                sys.exit("logo: 8-bit RGB(A) non-interlaced PNG only")
            bpp = 4 if ct == 6 else 3
        elif t == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride = zlib.decompress(idat), w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[x] = (line[x] + pr) & 255
        rows.append(line)
        prev = line
    px = []
    for line in rows:
        for x in range(w):
            q = line[x * bpp:(x + 1) * bpp]
            px.append((q[0], q[1], q[2], q[3] if bpp == 4 else 255))
    return w, h, px


def paste(dst, dw, src, sw, sx0, sy0, sw_, sh_, ox, oy, tw, th):
    """the source box (sx0, sy0, sw_ x sh_) area-averaged to tw x th, over dst at (ox, oy)"""
    for ty in range(th):
        y0, y1 = sy0 + ty * sh_ / th, sy0 + (ty + 1) * sh_ / th
        for tx in range(tw):
            x0, x1 = sx0 + tx * sw_ / tw, sx0 + (tx + 1) * sw_ / tw
            r = g = b = a = n = 0.0
            for yy in range(int(y0), max(int(y0) + 1, int(math.ceil(y1)))):
                for xx in range(int(x0), max(int(x0) + 1, int(math.ceil(x1)))):
                    p = src[yy * sw + xx]
                    al = p[3] / 255.0
                    r += p[0] * al; g += p[1] * al; b += p[2] * al; a += al; n += 1
            if a <= 0:
                continue
            al = a / n
            q = dst[(oy + ty) * dw + ox + tx]
            dst[(oy + ty) * dw + ox + tx] = tuple(int(round(c / a * al + q[i] * (1 - al)))
                                                  for i, c in enumerate((r, g, b))) + (255,)


def plain(w, h):
    px = []
    for y in range(h):
        for x in range(w):
            t = y / h
            px.append((int(26 + 14 * t), int(12 + 6 * t), int(52 + 30 * t), 255))
    return px


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    if len(sys.argv) > 2 and os.path.exists(sys.argv[2]):
        lw, lh, logo = png_read(sys.argv[2])
        w, h = 256, 128
        px = plain(w, h)
        s = min((w - 8) / lw, (h - 6) / lh)
        tw, th = int(lw * s), int(lh * s)
        paste(px, w, logo, lw, 0, 0, lw, lh, (w - tw) // 2, (h - th) // 2, tw, th)
        png(os.path.join(out, "banner.png"), w, h, px)
        iw = 48
        ip = plain(iw, iw)
        s = (iw - 2) / max(lw, lh)  # the whole logo, as large as it fits
        tw, th = int(lw * s), int(lh * s)
        paste(ip, iw, logo, lw, 0, 0, lw, lh, (iw - tw) // 2, (iw - th) // 2, tw, th)
        png(os.path.join(out, "icon.png"), iw, iw, ip)
        chime(out)
        return
    w, h = 256, 128
    px = canvas(w, h)
    # title, shadowed, centred
    s, sc = "MVC2", 7
    tw = (len(s) * 6 - 1) * sc
    text(px, w, s, (w - tw) // 2 + 3, 20 + 3, sc, (20, 10, 40, 255))
    text(px, w, s, (w - tw) // 2, 20, sc, (255, 204, 64, 255))
    s2, sc2 = "3DS", 4
    tw2 = (len(s2) * 6 - 1) * sc2
    text(px, w, s2, (w - tw2) // 2, 84, sc2, (240, 240, 248, 255))
    png(os.path.join(out, "banner.png"), w, h, px)
    iw = 48
    ip = canvas(iw, iw)
    text(ip, iw, "M", 4, 10, 3, (255, 204, 64, 255))
    text(ip, iw, "2", 25, 10, 3, (240, 240, 248, 255))
    png(os.path.join(out, "icon.png"), iw, iw, ip)
    chime(out)


def chime(out):
    rate = 22050
    with wave.open(os.path.join(out, "banner.wav"), "wb") as wv:
        wv.setnchannels(1)
        wv.setsampwidth(2)
        wv.setframerate(rate)
        frames = bytearray()
        for f, dur in ((659.25, 0.18), (987.77, 0.42)):
            n = int(rate * dur)
            for i in range(n):
                env = math.exp(-4.0 * i / n)
                frames += struct.pack("<h", int(9000 * env * math.sin(2 * math.pi * f * i / rate)))
        wv.writeframes(bytes(frames))


if __name__ == "__main__":
    main()
