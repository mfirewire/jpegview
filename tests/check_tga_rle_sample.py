#!/usr/bin/env python3
"""Reference-decode the hand-written 15-bit RLE TGA samples in tests/samples/.

Run from the repository root:  python3 tests/check_tga_rle_sample.py

Why this exists
---------------
ReaderTGA.cpp's RLE loop used channels = bits/8, which is 1 byte for a
15-bit pixel; a 15-bit pixel is a two-byte 5-5-5 word, so every RLE packet
consumed one byte too few and the stream drifted from the second pixel on.
The fix is channels = (bits + 7) / 8 (ReaderTGA.cpp:302).

C++ side note: the RLE decoder has since been extracted into
src/JPEGView/TgaRleDecode.h, and tests/loader_tga_rle_test.cpp drives
it over these same samples. This script stays as sample + reference
validation, NOT a test of the C++ decoder: it proves, per the TGA
specification, that the sample files themselves are well-formed and
that their expected pixels are the ones stated here. Decoding the
samples through the real loader (file-level header checks, flip,
alpha) still needs a Windows device test.

Expected pixels (TGA 15-bit 5-5-5, little-endian word; the reader unpacks
b = (v & 0x1f) << 3, g = ((v >> 5) & 0x1f) << 3, r = ((v >> 10) & 0x1f) << 3):
  tga15_rle_run.tga  4x1, one run packet (0x83) of pixel 0x7C00
                     -> (248, 0, 0) x 4
  tga15_rle_raw.tga  2x1, one raw packet (0x01) of 0x7C00, 0x03E0
                     -> (248, 0, 0), (0, 248, 0)

The run packet is exactly 3 bytes on disk (1 header + 2 pixel bytes), so a
decoder consuming 1 byte per pixel - the old bug - cannot land on the
right stream position; the exact-length assertion below pins that.

Exit code 0 = all samples decode as expected, 1 = any mismatch.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SAMPLES = ROOT / "tests" / "samples"

EXPECTED = {
    "tga15_rle_run.tga": [(248, 0, 0)] * 4,
    "tga15_rle_raw.tga": [(248, 0, 0), (0, 248, 0)],
}


def unpack_15bit(word):
    """One 15-bit TGA pixel word -> (R, G, B), same formula as ReaderTGA.cpp."""
    return ((word >> 10 & 0x1F) << 3, (word >> 5 & 0x1F) << 3, (word & 0x1F) << 3)


def decode_tga15_rle(data):
    """Decode a type-10 (RLE true-color) 15-bit TGA per the TGA spec.

    Returns (pixels, bytes_consumed). Packet header < 128: raw packet,
    header + 1 pixels follow, 2 bytes each. Header >= 128: run packet,
    (header & 0x7F) + 1 copies of the single following 2-byte pixel.
    """
    if len(data) < 18:
        raise ValueError("file shorter than the 18-byte TGA header")
    (id_len, cmap_type, image_type, _cmap_start, _cmap_len, _cmap_bits,
     _x, _y, width, height, bits, _desc) = struct.unpack("<BBBHHBHHHHBB", data[:18])
    if image_type != 10:
        raise ValueError(f"image type {image_type}, expected 10 (RLE true-color)")
    if bits != 15:
        raise ValueError(f"bits per pixel {bits}, expected 15")
    if cmap_type != 0 or id_len != 0:
        raise ValueError("this reference decoder expects no ID field and no colormap")
    pos = 18
    pixels = []
    total = width * height
    while len(pixels) < total:
        if pos >= len(data):
            raise ValueError("packet stream ends before all pixels are read")
        header = data[pos]
        pos += 1
        if header < 128:
            count = header + 1
            for _ in range(count):
                if pos + 2 > len(data):
                    raise ValueError("raw packet runs past end of file")
                word = data[pos] | (data[pos + 1] << 8)
                pos += 2
                pixels.append(unpack_15bit(word))
        else:
            count = (header & 0x7F) + 1
            if pos + 2 > len(data):
                raise ValueError("run packet runs past end of file")
            word = data[pos] | (data[pos + 1] << 8)
            pos += 2
            pixels.extend([unpack_15bit(word)] * count)
    if len(pixels) != total:
        raise ValueError(f"decoded {len(pixels)} pixels, header says {total}")
    return pixels, pos


def main():
    failures = 0
    for name, want in EXPECTED.items():
        path = SAMPLES / name
        if not path.is_file():
            print(f"FAIL  {name}: sample file missing")
            failures += 1
            continue
        try:
            got, consumed = decode_tga15_rle(path.read_bytes())
        except ValueError as e:
            print(f"FAIL  {name}: {e}")
            failures += 1
            continue
        size = path.stat().st_size
        if consumed != size:
            print(f"FAIL  {name}: decoded {consumed} bytes but file is {size} "
                  "(trailing or missing bytes)")
            failures += 1
            continue
        if got != want:
            print(f"FAIL  {name}: got {got}, expected {want}")
            failures += 1
            continue
        print(f"PASS  {name}: {len(got)} pixels {got}, {consumed} bytes consumed exactly")
    if failures:
        print(f"FAILED  {failures} TGA sample check(s)")
        return 1
    print(f"PASS  {len(EXPECTED)} TGA 15-bit RLE samples decode as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
