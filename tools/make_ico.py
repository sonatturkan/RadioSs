#!/usr/bin/env python3
"""Pack square RGBA PNG images into a Windows .ico file (standard library only).

    python tools/make_ico.py assets/icons/radios.ico 16.png 20.png ... 256.png

Entries smaller than 256 px are stored as classic 32-bit DIBs (BGRA plus the 1-bit
AND mask), which every Windows icon consumer reads; the 256 px entry is stored as
PNG (Windows Vista and later). The output is deterministic: the same PNG files
always give the same bytes. The PNGs come from assets/icons/*.svg rendered with
MSYS2 rsvg-convert (see assets/icons/README.md); the build only uses the committed
.ico and never runs this script.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class IconError(Exception):
    pass


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_rgba_png(path: Path) -> tuple[int, bytes]:
    """Decode an 8-bit RGBA, non-interlaced, square PNG. Returns (size, top-down RGBA rows)."""
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise IconError(f"{path}: PNG değil")
    offset, header, compressed = len(PNG_SIGNATURE), None, bytearray()
    while offset + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if len(body) != length or offset + 12 + length > len(data):
            raise IconError(f"{path}: kesik PNG parçası {kind!r}")
        (crc,) = struct.unpack(">I", data[offset + 8 + length:offset + 12 + length])
        if zlib.crc32(kind + body) & 0xFFFFFFFF != crc:
            raise IconError(f"{path}: {kind!r} parçasının CRC'si bozuk")
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
        offset += 12 + length
    if header is None:
        raise IconError(f"{path}: IHDR yok")
    width, height, depth, color, _compression, _filter, interlace = header
    if (depth, color, interlace) != (8, 6, 0):
        raise IconError(f"{path}: yalnız 8 bit RGBA, taramasız PNG desteklenir")
    if width != height or not 1 <= width <= 256:
        raise IconError(f"{path}: {width}x{height}; kare ve en çok 256 piksel olmalı")
    raw = zlib.decompress(bytes(compressed))
    stride = width * 4
    if len(raw) != height * (stride + 1):
        raise IconError(f"{path}: piksel verisi boyutu yanlış")
    pixels = bytearray(height * stride)
    previous = bytearray(stride)
    for row in range(height):
        kind = raw[row * (stride + 1)]
        line = bytearray(raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - 4] if i >= 4 else 0
            up = previous[i]
            corner = previous[i - 4] if i >= 4 else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                line[i] = (line[i] + _paeth(left, up, corner)) & 0xFF
            elif kind != 0:
                raise IconError(f"{path}: bilinmeyen PNG süzgeci {kind}")
        pixels[row * stride:(row + 1) * stride] = line
        previous = line
    return width, bytes(pixels)


def dib_entry(size: int, rgba: bytes) -> bytes:
    """BITMAPINFOHEADER + bottom-up BGRA + 1-bit AND mask (1 = transparent)."""
    stride = size * 4
    mask_stride = ((size + 31) // 32) * 4
    xor = bytearray()
    mask = bytearray()
    for row in reversed(range(size)):
        line = rgba[row * stride:(row + 1) * stride]
        bits = bytearray(mask_stride)
        for x in range(size):
            r, g, b, a = line[x * 4:x * 4 + 4]
            xor += bytes((b, g, r, a))
            if a < 128:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
    return header + bytes(xor) + bytes(mask)


def png_entry(path: Path) -> bytes:
    return path.read_bytes()


def build_icon(inputs: list[Path], png_from: int) -> bytes:
    entries = {}
    for path in inputs:
        size, rgba = read_rgba_png(path)
        if size in entries:
            raise IconError(f"{path}: {size} piksel iki kez verildi")
        entries[size] = png_entry(path) if size >= png_from else dib_entry(size, rgba)
    sizes = sorted(entries)
    directory = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    images = bytearray()
    for size in sizes:
        image = entries[size]
        dimension = 0 if size == 256 else size
        directory += struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(image), offset + len(images))
        images += image
    return directory + bytes(images)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output", type=Path)
    parser.add_argument("inputs", type=Path, nargs="+")
    parser.add_argument("--png-from", type=int, default=256,
                        help="bu boyuttan büyük/eşit girdiler PNG olarak saklanır (varsayılan 256)")
    args = parser.parse_args(argv)
    try:
        icon = build_icon(args.inputs, args.png_from)
    except (OSError, IconError, zlib.error) as error:
        print(f"make_ico: {error}", file=sys.stderr)
        return 1
    temporary = args.output.with_name(args.output.name + ".tmp")
    temporary.write_bytes(icon)
    temporary.replace(args.output)
    print(f"{args.output}: {len(args.inputs)} boyut, {len(icon)} bayt")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
