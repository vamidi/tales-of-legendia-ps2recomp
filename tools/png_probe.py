#!/usr/bin/env python3
"""Coarse analysis of a captured frame: colour census + ASCII luminance map.

The runtime's writePng() emits RGBA8 with filter type 0 on every row, so the
IDAT stream is simply the concatenation of the raw rows and needs no filter
reconstruction beyond the leading filter byte.

Usage: python tools/png_probe.py logs/shots/frame_001200.png [cols] [rows]
"""
import sys
import zlib
import collections


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    width = height = 0
    idat = bytearray()
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
            depth = body[8]
            ctype = body[9]
            assert depth == 8 and ctype == 6, f"expected RGBA8, got depth={depth} ctype={ctype}"
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    stride = width * 4 + 1
    assert len(raw) == stride * height, f"unexpected raw size {len(raw)} != {stride * height}"
    pixels = bytearray(width * height * 4)
    for y in range(height):
        row = raw[y * stride:(y + 1) * stride]
        assert row[0] == 0, f"filter {row[0]} at row {y}, decoder only handles 0"
        pixels[y * width * 4:(y + 1) * width * 4] = row[1:]
    return width, height, pixels


def main():
    path = sys.argv[1]
    cols = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    rows = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    width, height, px = read_png(path)

    census = collections.Counter()
    nonblack = 0
    for i in range(0, len(px), 4):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r or g or b:
            nonblack += 1
        census[(r >> 4 << 4, g >> 4 << 4, b >> 4 << 4)] += 1

    total = width * height
    print(f"{path}: {width}x{height}  non-black {100.0 * nonblack / total:.2f}%")
    print("top colours (r,g,b @4bit) :", ", ".join(
        f"{c} {100.0 * n / total:.1f}%" for c, n in census.most_common(6)))

    ramp = " .:-=+*#%@"
    print(f"\nluminance map {cols}x{rows} (each cell = block mean):")
    for ry in range(rows):
        line = []
        for rx in range(cols):
            x0 = rx * width // cols
            x1 = max(x0 + 1, (rx + 1) * width // cols)
            y0 = ry * height // rows
            y1 = max(y0 + 1, (ry + 1) * height // rows)
            acc = 0
            count = 0
            for y in range(y0, y1):
                base = y * width * 4
                for x in range(x0, x1):
                    i = base + x * 4
                    acc += (px[i] * 77 + px[i + 1] * 150 + px[i + 2] * 29) >> 8
                    count += 1
            level = acc // max(1, count)
            line.append(ramp[min(len(ramp) - 1, level * len(ramp) // 256)])
        print("".join(line))


if __name__ == "__main__":
    main()
