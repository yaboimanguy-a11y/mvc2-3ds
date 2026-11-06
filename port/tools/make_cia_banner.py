#!/usr/bin/env python3
"""make_cia_banner.py OUT_DIR [LOGO.png]: the CIA's home-menu banner inputs:
banner.png (256x128), icon.png (48x48), banner.wav (an original fighting-game
sting, 2.4 s, 22050 Hz mono 16-bit).  With LOGO (an 8-bit RGBA PNG, the project's
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
    art(out)
    chime(out)


# ---- the original art: a raised, lit title over a stormy "VS" backdrop ----
def _rng(seed):
    x = seed & 0xFFFFFFFF
    while True:
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        yield (x & 0xFFFFFF) / float(0x1000000)


def _mask_text(W, H, s, x0, y0, c):
    m = bytearray(W * H)
    for i, ch in enumerate(s):
        for ry, row in enumerate(FONT[ch]):
            for rx, bit in enumerate(row):
                if bit != "1":
                    continue
                bx, by = x0 + (i * 6 + rx) * c, y0 + ry * c
                for y in range(by, min(H, by + c)):
                    o = y * W
                    for x in range(max(0, bx), min(W, bx + c)):
                        m[o + x] = 1
    return m


def _dilate(m, W, H, r):
    t = bytearray(W * H)
    for y in range(H):
        o = y * W
        run = [i for i in range(W) if m[o + i]]
        for i in run:
            for x in range(max(0, i - r), min(W, i + r + 1)):
                t[o + x] = 1
    u = bytearray(W * H)
    for x in range(W):
        for y in range(H):
            if t[y * W + x]:
                for yy in range(max(0, y - r), min(H, y + r + 1)):
                    u[yy * W + x] = 1
    return u


def _backdrop(W, H, seed):
    px = [[0.0, 0.0, 0.0] for _ in range(W * H)]
    cx, cy = W * 0.5, H * 0.42
    for y in range(H):
        for x in range(W):
            t = y / H
            r, g, b = 26 + 50 * (1 - t), 10 + 8 * (1 - t), 48 + 70 * (1 - t)
            d = (((x - cx) / W) ** 2 * 2.2 + ((y - cy) / H) ** 2) ** 0.5
            glow = max(0.0, 1.0 - d * 1.9) ** 2
            r += 230 * glow; g += 120 * glow; b += 30 * glow
            if ((x + 2 * y) // max(2, W // 64)) % 9 == 0:   # speed lines
                r += 10; g += 8; b += 18
            px[y * W + x] = [r, g, b]
    rnd = _rng(seed)
    for k in range(3):   # lightning
        x = W * (0.12 + 0.38 * k) + W * 0.1 * next(rnd)
        y = -2.0
        pts = [(x, y)]
        while y < H:
            y += H * (0.08 + 0.08 * next(rnd))
            x += W * 0.07 * (next(rnd) - 0.5)
            pts.append((x, y))
        rad = W / 40.0
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            lx, ly = x2 - x1, y2 - y1
            ll = lx * lx + ly * ly or 1.0
            for yy in range(int(max(0, min(y1, y2) - rad)), int(min(H, max(y1, y2) + rad + 1))):
                for xx in range(int(max(0, min(x1, x2) - rad)), int(min(W, max(x1, x2) + rad + 1))):
                    u = max(0.0, min(1.0, ((xx - x1) * lx + (yy - y1) * ly) / ll))
                    dx, dy = xx - (x1 + u * lx), yy - (y1 + u * ly)
                    d = (dx * dx + dy * dy) ** 0.5
                    if d < rad:
                        f = (1 - d / rad) ** 3
                        p = px[yy * W + xx]
                        core = 1.0 if d < rad * 0.12 else 0.0
                        p[0] += 90 * f + 160 * core
                        p[1] += 140 * f + 160 * core
                        p[2] += 255 * f + 160 * core
    return px


def _title(px, W, H, s, x0, y0, c, depth, face, side, outline, ss):
    m = _mask_text(W, H, s, x0, y0, c)
    pts = [i for i in range(W * H) if m[i]]
    ext = bytearray(W * H)
    for i in pts:
        x, y = i % W, i // W
        for d in range(1, depth + 1):
            if x + d < W and y + d < H:
                ext[(y + d) * W + x + d] = 1
    u = bytearray(a | b for a, b in zip(m, ext))
    ring = _dilate(u, W, H, 2 * ss)
    for i in range(W * H):           # the outline (and a soft drop shadow)
        if ring[i] and not u[i]:
            px[i] = list(outline)
    for d in range(depth, 0, -1):    # the extruded sides, darker further back
        k = 0.45 + 0.4 * (1 - d / depth)
        col = [side[0] * k, side[1] * k, side[2] * k]
        for i in pts:
            x, y = i % W, i // W
            if x + d < W and y + d < H and not m[(y + d) * W + x + d]:
                px[(y + d) * W + x + d] = col
    top = min(i // W for i in pts)
    bot = max(i // W for i in pts)
    e = max(1, int(1.5 * ss))
    for i in pts:                    # the lit face: gradient + bevel
        x, y = i % W, i // W
        t = (y - top) / max(1, bot - top)
        f0, f1, f2 = face
        col = [f0[j] + (f1[j] - f0[j]) * min(1, t * 2) if t < 0.5 else f1[j] + (f2[j] - f1[j]) * (t - 0.5) * 2
               for j in range(3)]
        lit = x - e >= 0 and y - e >= 0 and not m[(y - e) * W + x - e]
        dark = x + e < W and y + e < H and not m[(y + e) * W + x + e]
        if lit:
            col = [v + (255 - v) * 0.7 for v in col]
        elif dark:
            col = [v * 0.7 for v in col]
        if 0.30 < t < 0.36:              # a glossy band
            col = [v + (255 - v) * 0.35 for v in col]
        px[i] = col


def _down(px, W, H, ss):
    w, h = W // ss, H // ss
    out = []
    for y in range(h):
        for x in range(w):
            r = g = b = 0.0
            for yy in range(ss):
                o = (y * ss + yy) * W + x * ss
                for xx in range(ss):
                    p = px[o + xx]
                    r += p[0]; g += p[1]; b += p[2]
            n = ss * ss
            out.append((min(255, int(r / n)), min(255, int(g / n)), min(255, int(b / n)), 255))
    return out


GOLD = ((255, 246, 170), (255, 196, 40), (230, 90, 16))
CHROME = ((255, 255, 255), (170, 190, 220), (80, 100, 150))


def art(out):
    ss = 3
    W, H = 256 * ss, 128 * ss
    px = _backdrop(W, H, 0x4D564332)
    c = 8 * ss
    tw = (4 * 6 - 1) * c
    _title(px, W, H, "MVC2", (W - tw) // 2 - 3 * ss, 12 * ss, c, 7 * ss, GOLD, (150, 40, 10), (8, 4, 18), ss)
    c2 = 4 * ss
    tw2 = (3 * 6 - 1) * c2
    _title(px, W, H, "3DS", (W - tw2) // 2 - ss, 84 * ss, c2, 3 * ss, CHROME, (40, 60, 120), (8, 4, 18), ss)
    png(os.path.join(out, "banner.png"), 256, 128, _down(px, W, H, ss))
    ss = 4
    W = H = 48 * ss
    px = _backdrop(W, H, 0x49434F4E)
    c = 4 * ss
    tw = (2 * 6 - 1) * c
    _title(px, W, H, "M2", (W - tw) // 2 - ss, 9 * ss, c, 3 * ss, GOLD, (150, 40, 10), (8, 4, 18), ss)
    png(os.path.join(out, "icon.png"), 48, 48, _down(px, W, H, ss))


def chime(out):
    """An original fighting-game sting (2.4 s): a swoosh, a heavy hit, three
    power-chord stabs and a held "hyper combo" chord with sparkle."""
    rate = 22050
    n = int(rate * 2.4)
    buf = [0.0] * n
    rnd = _rng(0x534F554E)

    def add(t0, dur, fn):
        i0 = int(t0 * rate)
        for i in range(int(dur * rate)):
            if i0 + i < n:
                buf[i0 + i] += fn(i / rate, i / (dur * rate))

    def saw(f, t):
        x = f * t
        return 2.0 * (x - int(x)) - 1.0

    def chord(freqs, t, detune=0.004):
        return sum(saw(f, t) + saw(f * (1 + detune), t) for f in freqs) / (2 * len(freqs))

    # swoosh: noise, brighter and louder towards the hit
    lp = [0.0]
    def swoosh(t, u):
        lp[0] += (next(rnd) * 2 - 1 - lp[0]) * (0.08 + 0.6 * u)
        return lp[0] * 0.5 * u * u
    add(0.0, 0.2, swoosh)
    # the hit: a falling thump and a crack of noise
    add(0.2, 0.35, lambda t, u: 0.9 * math.sin(2 * math.pi * (110 * t - 120 * t * t)) * math.exp(-9 * t))
    add(0.2, 0.08, lambda t, u: (next(rnd) * 2 - 1) * 0.7 * (1 - u))
    # three stabs (E5, G5, A5 power chords)
    for k, root in enumerate((164.81, 196.0, 220.0)):
        add(0.55 + 0.17 * k, 0.15, lambda t, u, r=root: 0.55 * chord((r, r * 1.5, r * 2), t) * math.exp(-14 * t))
    # the held chord (B5) with vibrato, and a rising sparkle over it
    add(1.08, 1.3, lambda t, u: 0.5 * chord((246.94, 370.0, 493.88), t * (1 + 0.004 * math.sin(2 * math.pi * 6 * t)))
        * min(1.0, t * 30) * (1 - u) ** 1.5)
    for k in range(8):
        f = 987.77 * 2 ** (k / 12.0 * 2)
        add(1.1 + 0.06 * k, 0.5, lambda t, u, f=f: 0.12 * math.sin(2 * math.pi * f * t) * math.exp(-6 * t))
    peak = max(abs(v) for v in buf) or 1.0
    with wave.open(os.path.join(out, "banner.wav"), "wb") as wv:
        wv.setnchannels(1)
        wv.setsampwidth(2)
        wv.setframerate(rate)
        frames = bytearray()
        for v in buf:
            v = math.tanh(1.6 * v / peak) / math.tanh(1.6)   # a little punch
            frames += struct.pack("<h", int(26000 * v))
        wv.writeframes(bytes(frames))


if __name__ == "__main__":
    main()
