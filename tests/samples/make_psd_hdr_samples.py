#!/usr/bin/env python3
"""Generate the PSD HDR test samples (16/32-bit PSD decode core).

Writes, next to this script, small synthetic PSD/PSB files with KNOWN
pixel values, one per (bit depth, colour, compression) combination:

    psd_hdr_16bit_rgb_raw.psd       5x2  16-bit RGB   raw (0)
    psd_hdr_16bit_rgb_rle.psd       5x2  16-bit RGB   RLE (1)
    psd_hdr_16bit_rgb_zip.psd       5x2  16-bit RGB   ZIP w/o prediction (2)
    psd_hdr_16bit_rgb_zippred.psd   5x2  16-bit RGB   ZIP with prediction (3)
    psd_hdr_16bit_gray_raw.psd      5x2  16-bit grey  raw (0)
    psd_hdr_16bit_rgba_raw.psd      5x2  16-bit RGBA  raw (0)
    psd_hdr_16bit_rgb_rle_psb.psd   5x2  16-bit RGB   RLE (1), PSB version 2
    psd_hdr_32bit_rgb_raw.psd       9x2  32-bit RGB   raw (0)
    psd_hdr_32bit_rgb_rle.psd       9x2  32-bit RGB   RLE (1)
    psd_hdr_32bit_rgb_zip.psd       9x2  32-bit RGB   ZIP w/o prediction (2)
    psd_hdr_32bit_rgb_zippred.psd   9x2  32-bit RGB   ZIP with prediction (3)
    psd_hdr_32bit_gray_raw.psd      9x2  32-bit grey  raw (0)
    psd_hdr_32bit_rgba_raw.psd      9x2  32-bit RGBA  raw (0)

Layout (row 0 = grey ramp, row 1 = colour ramp, left to right):
    16-bit levels (linear): 0.0, 0.18, 0.5, 0.75, 1.0
    32-bit levels (linear): 0.0, 0.18, 0.5, 0.75, 1.0, 2.0, 4.0, 8.0, 16.0
    colour row: R = level, G = 0.18, B = 0.5 (per-channel independence)
    RGBA alpha row: 1.0, 0.75, 0.5, 0.25, 0.0, ... (16-bit: round(a*65535))

16-bit samples are sRGB gamma-encoded (Photoshop 16-bit mode stores
samples in the document working space, normally sRGB): the stored
uint16 is round(sRGB_OETF(linear) * 65535). Linear levels above 1.0
cannot be represented in 16-bit sRGB, so the 16-bit files only cover
the SDR range; the 32-bit files store IEEE-754 float32 samples
linear as-is and cover the HDR range, so the BT.2390 highlight
roll-off can be verified (linear 1.0 -> ~211, 2.0 -> ~235, 4.0 -> ~249,
8.0 -> ~254, 16.0 -> 255 with a content peak of 16.0).

PSD structure (per the Adobe spec, same hand-rolled pattern as
make_cmyk_samples.py): 8BPS header, empty colour-mode data, empty
image resources, empty layer/mask section (length field 0), 2-byte
compression field, then planar channel data (R, G, B [, A] or grey;
for RLE the byte-count table precedes the PackBits data, 2 bytes per
row per channel for PSD v1, 4 bytes for PSB v2, big-endian).

ZIP with prediction (3): each row's samples are stored as horizontal
deltas — each sample minus the previous sample in the same row, same
channel, in the sample's native width (uint16 / uint32 bit pattern,
wraparound); the first sample of a row is stored as-is (delta from
zero). This is the Adobe spec's prediction, cross-checked against the
oov/psd decoder (decodeDelta accumulates uint16/uint32 differences
row-by-row, accumulator reset per row).

This script also prints the expected 8-bit BGR output of every file
after PsdHdr decode + HdrToneMap::ConvertLinearImage, computed by the
Python reference implementation of ConvertLinearPixel below (an
independent re-implementation of the PQ-domain BT.2390 EETF from
HdrToneMap.cpp, same formulas). tests/psd_hdr_test.cpp pins those
numbers as literals. The peak used is the file's own content peak
(max sRGB luma over all pixels): 1.0 for the 16-bit files, 16.0 for
the 32-bit files.

Verification after generation: the script re-parses every file's
bytes (header, sections, compression, image-data length) as a
structural self-check. Pillow is tried on every file but CANNOT read
any of them, honestly: Pillow's PsdImagePlugin MODES table lists only
8-bit depths (plus 1-bit bitmap), so 16-bit and 32-bit PSD raise
UnidentifiedImageError before any pixel is read — that is a Pillow
limitation, not a defect in these files (the 8-bit
cmyk_chart_6x1.psd opens fine in the same Pillow). The C++ unit test
tests/psd_hdr_test.cpp is therefore the real check for these files.
The ZIP-prediction direction (per-row delta, accumulator reset per
row) is corroborated outside this repo by the Adobe spec text and the
oov/psd decoder's decodeDelta, not only by the generator.

Requirements to regenerate: python3, Pillow (for the sanity check
only). The checked-in samples are the deliverable; this script is
their provenance record.

Usage:  python3 tests/samples/make_psd_hdr_samples.py
"""

import hashlib
import math
import struct
import zlib
from pathlib import Path

# ---------------------------------------------------------------- levels

LEVELS_16 = [0.0, 0.18, 0.5, 0.75, 1.0]
LEVELS_32 = [0.0, 0.18, 0.5, 0.75, 1.0, 2.0, 4.0, 8.0, 16.0]
HEIGHT = 2

# ---------------------------------------------------------------- sRGB

def srgb_oetf(linear):
    if linear <= 0.0031308:
        return 12.92 * linear
    return 1.055 * linear ** (1.0 / 2.4) - 0.055

def srgb_eotf(code):
    if code <= 0.04045:
        return code / 12.92
    return ((code + 0.055) / 1.055) ** 2.4

# ------------------------------------------------- HdrToneMap reference
# Independent Python re-implementation of HdrToneMap::ConvertLinearPixel
# (PQ-domain BT.2390 EETF, linear 1.0 = SDR white = 203 nits), copied
# from the formulas in src/JPEGView/HdrToneMap.cpp, not from its code.

K_SDR_WHITE_NITS = 203.0
K_PQ_M1 = 2610.0 / 16384.0
K_PQ_M2 = 2523.0 / 32.0
K_PQ_C1 = 3424.0 / 4096.0
K_PQ_C2 = 2413.0 / 128.0
K_PQ_C3 = 2392.0 / 128.0
K_PQ_PEAK_NITS = 10000.0
K_SRGB_LUMA = (0.2126, 0.7152, 0.0722)

def pq_signal_to_nits(signal):
    s = min(max(signal, 0.0), 1.0)
    p = s ** (1.0 / K_PQ_M2)
    frac = (p - K_PQ_C1) / (K_PQ_C2 - K_PQ_C3 * p)
    if not frac > 0.0:
        return 0.0
    return frac ** (1.0 / K_PQ_M1) * K_PQ_PEAK_NITS

def pq_nits_to_signal(nits):
    lm = nits / K_PQ_PEAK_NITS
    if not lm > 0.0:
        return 0.0
    lm_m1 = lm ** K_PQ_M1
    return ((K_PQ_C1 + K_PQ_C2 * lm_m1) / (1.0 + K_PQ_C3 * lm_m1)) ** K_PQ_M2

def bt2390_eetf_pq(signal, peak_nits):
    lw = max(peak_nits, K_SDR_WHITE_NITS)
    lmax = K_SDR_WHITE_NITS
    pq_lw = pq_nits_to_signal(lw)
    pq_lb = pq_nits_to_signal(0.0)
    pq_lmax = pq_nits_to_signal(lmax)
    pq_lmin = pq_nits_to_signal(0.0)
    denom = pq_lw - pq_lb
    if not denom > 0.0:
        return min(max(signal, 0.0), 1.0)
    max_lum = (pq_lmax - pq_lb) / denom
    min_lum = (pq_lmin - pq_lb) / denom
    ks = 1.5 * max_lum - 0.5
    e1 = min(max(signal, 0.0), 1.0) / pq_lw
    e1 = min(e1, 1.0)
    if e1 < ks or not ks < 1.0:
        e2 = e1
    else:
        t = min(max((e1 - ks) / (1.0 - ks), 0.0), 1.0)
        t2, t3 = t * t, t * t * t
        e2 = ((2.0 * t3 - 3.0 * t2 + 1.0) * ks
              + (t3 - 2.0 * t2 + t) * (1.0 - ks)
              + (-2.0 * t3 + 3.0 * t2) * max_lum)
    e3 = e2 + min_lum * (1.0 - e2) ** 4.0
    return pq_nits_to_signal(pq_signal_to_nits(e3 * denom + pq_lb))

def convert_linear_pixel(r, g, b, peak_linear):
    peak_nits = peak_linear * K_SDR_WHITE_NITS
    if not peak_nits > 0.0:
        peak_nits = K_SDR_WHITE_NITS
    out = []
    for v in (r, g, b):
        if not v >= 0.0:
            v = 0.0
        sig = pq_nits_to_signal(v * K_SDR_WHITE_NITS)
        e = bt2390_eetf_pq(sig, peak_nits)
        lin = pq_signal_to_nits(e) / K_SDR_WHITE_NITS
        # GamutMap is the identity for in-gamut sRGB; clamp as safety.
        lin = min(max(lin, 0.0), 1.0)
        byte = int(min(max(srgb_oetf(lin), 0.0), 1.0) * 255.0 + 0.5)
        out.append(min(byte, 255))
    return tuple(out)  # (R, G, B)

# ---------------------------------------------------------------- pixels

def build_planes(levels, channels, color_mode):
    """planes[ch][y][x] = linear float value of that sample."""
    width = len(levels)
    planes = []
    if color_mode == 1:  # grayscale
        planes.append([[levels[x] for x in range(width)] for _ in range(HEIGHT)])
    else:  # RGB(A): row 0 grey ramp (R=G=B=level), row 1 colour ramp
        # (R=level, G=0.18, B=0.5); the alpha plane (if any) is added below.
        planes.append([list(levels), list(levels)])                    # R
        planes.append([list(levels), [0.18] * width])                  # G
        planes.append([list(levels), [0.5] * width])                   # B
    if channels == 4:
        # Alpha ramp across the columns, same on both rows.
        alphas = [1.0, 0.75, 0.5, 0.25, 0.0, 0.0, 0.0, 0.0, 0.0][:width]
        planes.append([[alphas[x] for x in range(width)] for _ in range(HEIGHT)])
    return planes

# ---------------------------------------------------------------- encoding

def pack_sample(value, bit_depth):
    if bit_depth == 16:
        return struct.pack(">H", round(srgb_oetf(value) * 65535))
    return struct.pack(">f", value)

def packbits_encode(row_bytes):
    """PackBits-encode one row. Runs of >=3 identical bytes become a
    repeat packet; anything else goes in literal packets (max 128)."""
    out = bytearray()
    i, n = 0, len(row_bytes)
    while i < n:
        # Run at i?
        j = i + 1
        while j < n and row_bytes[j] == row_bytes[i] and j - i < 128:
            j += 1
        run = j - i
        if run >= 3:
            out.append(257 - run)
            out.append(row_bytes[i])
            i = j
            continue
        # Literal up to the next run of >=3, or 128 bytes.
        start = i
        i += 1
        while i < n and i - start < 128:
            k = i + 1
            while k < n and row_bytes[k] == row_bytes[i] and k - i < 128:
                k += 1
            if k - i >= 3:
                break
            i += 1
        out.append(i - start - 1)
        out.extend(row_bytes[start:i])
    return bytes(out)

def delta_encode_row(samples, bit_depth):
    """Horizontal delta per row, native width, wraparound."""
    out = []
    prev = 0
    for s in samples:
        if bit_depth == 16:
            d = (s - prev) & 0xFFFF
        else:
            d = (s - prev) & 0xFFFFFFFF
        out.append(d)
        prev = s
    return out

def encode_image_data(planes, channels, bit_depth, compression, version):
    """Return the image-data section bytes after the compression field.
    planes[ch][y][x] holds stored values: uint16 ints (16-bit) or
    float32 bit patterns as uint32 (32-bit)."""
    width = len(planes[0][0])
    height = len(planes[0])
    # Per-plane, per-row raw bytes (big-endian samples).
    raw_rows = []  # raw_rows[ch][y] = bytes
    for ch in range(channels):
        rows = []
        for y in range(height):
            b = bytearray()
            for x in range(width):
                v = planes[ch][y][x]
                if bit_depth == 16:
                    b += struct.pack(">H", v)
                else:
                    b += struct.pack(">I", v)
            rows.append(bytes(b))
        raw_rows.append(rows)
    if compression == 0:
        return b"".join(b"".join(rows) for rows in raw_rows)
    if compression == 1:
        table = bytearray()
        data = bytearray()
        fmt = ">I" if version == 2 else ">H"
        for ch in range(channels):
            for y in range(height):
                enc = packbits_encode(raw_rows[ch][y])
                table += struct.pack(fmt, len(enc))
                data += enc
        return bytes(table) + bytes(data)
    if compression in (2, 3):
        if compression == 3:
            # Delta-encode each row in the sample's native width, then
            # pack the deltas as big-endian samples.
            buf = bytearray()
            for ch in range(channels):
                for y in range(height):
                    samples = [struct.unpack(">H", raw_rows[ch][y][i:i+2])[0]
                               if bit_depth == 16 else
                               struct.unpack(">I", raw_rows[ch][y][i:i+4])[0]
                               for i in range(0, len(raw_rows[ch][y]),
                                              2 if bit_depth == 16 else 4)]
                    deltas = delta_encode_row(samples, bit_depth)
                    for d in deltas:
                        buf += struct.pack(">H", d) if bit_depth == 16 \
                            else struct.pack(">I", d)
            return zlib.compress(bytes(buf), 9)
        return zlib.compress(b"".join(b"".join(rows) for rows in raw_rows), 9)
    raise ValueError("bad compression %d" % compression)

def make_psd(path, version, channels, height, width, bit_depth, color_mode,
             compression, planes):
    header = (
        b"8BPS"
        + struct.pack(">H", version)
        + b"\x00" * 6
        + struct.pack(">H", channels)
        + struct.pack(">I", height)
        + struct.pack(">I", width)
        + struct.pack(">H", bit_depth)
        + struct.pack(">H", color_mode)
    )
    sections = struct.pack(">I", 0) + struct.pack(">I", 0)
    # Layer/mask info: length field 0 (empty section). 4 bytes for PSD
    # v1, 8 bytes for PSB v2.
    sections += struct.pack(">Q", 0) if version == 2 else struct.pack(">I", 0)
    image_data = encode_image_data(planes, channels, bit_depth, compression, version)
    path.write_bytes(header + sections + struct.pack(">H", compression) + image_data)

def stored_planes(levels, channels, color_mode, bit_depth):
    """Linear planes -> stored sample values (uint16 / uint32 bits)."""
    linear = build_planes(levels, channels, color_mode)
    stored = []
    for ch in range(channels):
        rows = []
        for y in range(HEIGHT):
            row = []
            for x in range(len(levels)):
                v = linear[ch][y][x]
                if bit_depth == 16:
                    row.append(round(srgb_oetf(v) * 65535))
                else:
                    row.append(struct.unpack(">I", struct.pack(">f", v))[0])
            rows.append(row)
        stored.append(rows)
    return stored, linear

def expected_bgr(levels, channels, color_mode, bit_depth):
    """Expected 8-bit BGR output per pixel after PsdHdr + HdrToneMap,
    from the *stored* samples (so 16-bit rounding is accounted for)."""
    stored, linear = stored_planes(levels, channels, color_mode, bit_depth)
    width = len(levels)
    # Recover the linear values the decoder will see.
    lin = []
    for y in range(HEIGHT):
        row = []
        for x in range(width):
            if color_mode == 1:
                v = stored[0][y][x]
                l = srgb_eotf(v / 65535.0) if bit_depth == 16 \
                    else struct.unpack(">f", struct.pack(">I", v))[0]
                row.append((l, l, l))
            else:
                px = []
                for ch in range(3):
                    v = stored[ch][y][x]
                    px.append(srgb_eotf(v / 65535.0) if bit_depth == 16
                              else struct.unpack(">f", struct.pack(">I", v))[0])
                row.append(tuple(px))
        lin.append(row)
    peak = 0.0
    for y in range(HEIGHT):
        for x in range(width):
            r, g, b = lin[y][x]
            peak = max(peak, r * 0.2126 + g * 0.7152 + b * 0.0722)
    out = []
    for y in range(HEIGHT):
        for x in range(width):
            r, g, b = convert_linear_pixel(*lin[y][x], peak)
            out.append((b, g, r))  # BGR, JPEGView order
    return out, peak

# ---------------------------------------------------------------- self-check

def check_psd(path):
    data = path.read_bytes()
    assert data[:4] == b"8BPS", "bad signature"
    version = struct.unpack(">H", data[4:6])[0]
    assert version in (1, 2), "bad version"
    assert data[6:12] == b"\x00" * 6, "bad reserved"
    channels = struct.unpack(">H", data[12:14])[0]
    height = struct.unpack(">I", data[14:18])[0]
    width = struct.unpack(">I", data[18:22])[0]
    bit_depth = struct.unpack(">H", data[22:24])[0]
    color_mode = struct.unpack(">H", data[24:26])[0]
    pos = 26
    for _ in range(2):
        n = struct.unpack(">I", data[pos:pos+4])[0]
        pos += 4 + n
    layer_len_size = 8 if version == 2 else 4
    layer_len = int.from_bytes(data[pos:pos+layer_len_size], "big")
    pos += layer_len_size + layer_len
    compression = struct.unpack(">H", data[pos:pos+2])[0]
    assert compression in (0, 1, 2, 3), "bad compression"
    return {"version": version, "channels": channels, "width": width,
            "height": height, "bit_depth": bit_depth,
            "color_mode": color_mode, "compression": compression,
            "image_data_bytes": len(data) - pos - 2}

def try_pillow(path):
    try:
        from PIL import Image
        with Image.open(path) as im:
            im.load()
            return "mode=%s size=%s" % (im.mode, im.size)
    except Exception as exc:  # noqa: BLE001 - report, do not fail
        return "Pillow cannot read: %s: %s" % (type(exc).__name__, exc)

# ---------------------------------------------------------------- main

SPECS = [
    # (filename, version, bit_depth, color_mode, channels, compression)
    ("psd_hdr_16bit_rgb_raw.psd", 1, 16, 3, 3, 0),
    ("psd_hdr_16bit_rgb_rle.psd", 1, 16, 3, 3, 1),
    ("psd_hdr_16bit_rgb_zip.psd", 1, 16, 3, 3, 2),
    ("psd_hdr_16bit_rgb_zippred.psd", 1, 16, 3, 3, 3),
    ("psd_hdr_16bit_gray_raw.psd", 1, 16, 1, 1, 0),
    ("psd_hdr_16bit_rgba_raw.psd", 1, 16, 3, 4, 0),
    ("psd_hdr_16bit_rgb_rle_psb.psd", 2, 16, 3, 3, 1),
    ("psd_hdr_32bit_rgb_raw.psd", 1, 32, 3, 3, 0),
    ("psd_hdr_32bit_rgb_rle.psd", 1, 32, 3, 3, 1),
    ("psd_hdr_32bit_rgb_zip.psd", 1, 32, 3, 3, 2),
    ("psd_hdr_32bit_rgb_zippred.psd", 1, 32, 3, 3, 3),
    ("psd_hdr_32bit_gray_raw.psd", 1, 32, 1, 1, 0),
    ("psd_hdr_32bit_rgba_raw.psd", 1, 32, 3, 4, 0),
]

def main():
    here = Path(__file__).resolve().parent
    for name, version, bit_depth, color_mode, channels, compression in SPECS:
        levels = LEVELS_16 if bit_depth == 16 else LEVELS_32
        width = len(levels)
        stored, _linear = stored_planes(levels, channels, color_mode, bit_depth)
        path = here / name
        make_psd(path, version, channels, HEIGHT, width, bit_depth,
                 color_mode, compression, stored)
        info = check_psd(path)
        bgr, peak = expected_bgr(levels, channels, color_mode, bit_depth)
        print("wrote %s (%d bytes, sha256 %s)" % (
            name, path.stat().st_size,
            hashlib.sha256(path.read_bytes()).hexdigest()[:16]))
        print("  %s peak=%.4f Pillow: %s" % (info, peak, try_pillow(path)))
        print("  expected BGR (row-major): %s" % (bgr,))
    print("Now run: g++ tests/psd_hdr_test.cpp (see tests/psd_hdr_test.cpp)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
