#!/usr/bin/env python3
"""Checks that a screenshot from the example shows a rendered scene.

Fails if the image is mostly the background colour, or flat (a black or
uniform frame from a pipeline that silently failed). Standard library only:
decodes 8-bit RGB/RGBA PNGs, as bimg writes them.
"""

import struct
import sys
import zlib

BACKGROUND = (0x20, 0x24, 0x2B)  # the tone mapping pass's fixed background


def read_png(path):
    with open(path, "rb") as file:
        data = file.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")

    offset = 8
    width = height = channels = None
    compressed = b""
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        chunk = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", chunk[:10])
            if depth != 8 or color not in (2, 6):
                raise ValueError("unsupported PNG format")
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            compressed += chunk
        elif kind == b"IEND":
            break

    raw = zlib.decompress(compressed)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if kind == 1:
                row[x] = (row[x] + left) & 0xFF
            elif kind == 2:
                row[x] = (row[x] + up) & 0xFF
            elif kind == 3:
                row[x] = (row[x] + (left + up) // 2) & 0xFF
            elif kind == 4:
                estimate = left + up - upper_left
                distances = (abs(estimate - left), abs(estimate - up), abs(estimate - upper_left))
                predictor = (left, up, upper_left)[distances.index(min(distances))]
                row[x] = (row[x] + predictor) & 0xFF
        rows.append(row)
        previous = row
    return width, height, channels, rows


def main():
    width, height, channels, rows = read_png(sys.argv[1])
    scene = 0
    total = 0
    luminance_sum = 0.0
    luminance_squares = 0.0
    for row in rows[::4]:
        for x in range(0, width, 4):
            pixel = tuple(row[x * channels:x * channels + 3])
            total += 1
            if max(abs(a - b) for a, b in zip(pixel, BACKGROUND)) > 8:
                scene += 1
            luminance = 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]
            luminance_sum += luminance
            luminance_squares += luminance * luminance

    coverage = scene / total
    mean = luminance_sum / total
    deviation = max(luminance_squares / total - mean * mean, 0.0) ** 0.5
    print(f"{width}x{height}: scene covers {coverage:.1%}, mean luminance {mean:.1f}, deviation {deviation:.1f}")
    if coverage < 0.05 or deviation < 5.0:
        print("The frame looks blank.")
        sys.exit(1)


if __name__ == "__main__":
    main()
