"""make_icon.py - disegna l'icona dell'emulatore (una ghianda stilizzata,
omaggio ad Acorn) e la salva come .ico con immagini PNG a piu' dimensioni.

    python tools/make_icon.py res/armwin.ico [anteprima.png]

Solo libreria standard: le forme sono campionate con antialiasing 4x4.
Cappello scuro e frutto chiaro: le parti si distinguono per luminosita'.
"""
import math
import struct
import sys
import zlib

OUTLINE = (40, 24, 12)
CAP = (92, 58, 30)
CAP_LINE = (58, 36, 18)
NUT = (222, 170, 72)
NUT_DARK = (176, 122, 44)
HIGHLIGHT = (250, 226, 160)
STEM = (70, 46, 24)


def ellipse_r(x, y, cx, cy, rx, ry):
    return math.hypot((x - cx) / rx, (y - cy) / ry)


def sample(x, y):
    """colore RGB o None (trasparente) nel punto (x, y) in [0,1]."""
    # picciolo, leggermente inclinato
    sx = 0.52 + (0.20 - y) * 0.25
    if 0.05 <= y <= 0.24 and abs(x - sx) <= 0.045:
        return OUTLINE if abs(x - sx) > 0.025 or y < 0.07 else STEM
    # cappello: meta' superiore di un'ellisse schiacciata, con bordo inferiore
    rc = ellipse_r(x, y, 0.5, 0.42, 0.42, 0.24)
    if rc <= 1.0 and y <= 0.47:
        if rc > 0.90 or y > 0.44:
            return OUTLINE
        # trama a rombi della cupola
        u, v = (x + y) * 14, (x - y) * 14
        if min(u % 1, v % 1) < 0.16:
            return CAP_LINE
        return CAP
    # frutto: ellisse con punta in basso
    ry = 0.30 if y < 0.62 else 0.33
    rx = 0.31 * (1.0 - max(0.0, y - 0.62) * 0.9)
    rn = ellipse_r(x, y, 0.5, 0.62, rx, ry)
    tip = abs(x - 0.5) < 0.03 and 0.90 <= y <= 0.97
    if rn <= 1.0 or tip:
        if tip and rn > 1.0:
            return OUTLINE
        if rn > 0.91:
            return OUTLINE
        if ellipse_r(x, y, 0.40, 0.60, 0.07, 0.13) <= 1.0:
            return HIGHLIGHT
        # ombra sul lato destro
        return NUT_DARK if (x - 0.5) / rx + (y - 0.62) / ry * 0.3 > 0.45 else NUT
    return None


def render(size):
    n = 4
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            r = g = b = a = 0
            for sy in range(n):
                for sx in range(n):
                    c = sample((px + (sx + 0.5) / n) / size, (py + (sy + 0.5) / n) / size)
                    if c:
                        r += c[0]; g += c[1]; b += c[2]; a += 1
            if a:
                row += bytes((r // a, g // a, b // a, a * 255 // (n * n)))
            else:
                row += bytes(4)
        rows.append(bytes(row))
    return rows


def png(size, rows):
    raw = b"".join(b"\0" + r for r in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    sizes = [16, 24, 32, 48, 64, 256]
    images = [png(s, render(s)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(sys.argv[1], "wb") as f:
        f.write(header + entries + b"".join(images))
    if len(sys.argv) > 2:
        with open(sys.argv[2], "wb") as f:
            f.write(images[-1])
    print(f"{sys.argv[1]}: {len(sizes)} dimensioni")


if __name__ == "__main__":
    main()
