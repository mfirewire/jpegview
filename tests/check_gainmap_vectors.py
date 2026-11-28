#!/usr/bin/env python3
# check_gainmap_vectors.py - R-13: independent tie for the gain-map sample vectors.
#
# The C++ test (tests/gainmap_signed_weight_test.cpp, W4) never opens the
# four .avif files or mozilla-sdr-endpoint.png: it reads the pre-extracted
# vectors (*-base16.bgra, *-gain8.raw) and hardcodes the endpoint greys
# (137, 188). So an extraction error in those vectors would silently pass
# CI. This script re-extracts the vectors from the .avif sources with an
# INDEPENDENT decoder (ffmpeg, system libavif -- not the vendored libavif
# 1.4.2 C extractor that produced the committed vectors) and compares:
#
#   - base: stream 0 decoded to 16-bit BGRA, compared at 8-bit precision
#     (>>8 on both sides). The committed vectors use libavif's 8->16-bit
#     upscale (byte replication, v*257); ffmpeg shifts (v*256), so a
#     16-bit comparison would differ by up to 255 on identical pixels.
#     Tolerance: max diff <= 1 (observed 0 for the Mozilla pair, <= 1
#     for the Seine crops -- YUV->RGB rounding differs by <= 1 LSB
#     between two independent decoders on real photos).
#     EXCEPTION -- mozilla-hdr: its base is planar RGB (gbrp, 8-bit)
#     with a PQ transfer. Requesting a 16-bit format (gbrp16le) from
#     this 8-bit source makes ffmpeg insert a scaler, and on the CI
#     runner's ffmpeg 8.0.1 (Ubuntu 26.04 resolute, libavcodec62,
#     dav1d 1.5.3) that scaler applies a PQ->sRGB conversion (max
#     diff 69 vs the vector; 255 - 186 = 69). So for this one sample
#     the base is decoded to its NATIVE format (gbrp, no scaler in
#     the graph at all) and the G/B/R planes are compared directly
#     against the vector's channels at 8-bit. Native-format decode
#     returns the coded values, so it is immune to scaler behaviour
#     and version-stable.
#   - gain: stream 1 (the gain-map image) decoded to 8-bit gray.
#     Tolerance: max diff <= 1 (observed byte-exact on ffmpeg 8.1.2).
#   - Seine vectors are a 128x128 centre crop (x 136..264, y 86..214) of
#     the 400x300 image; the crop is applied here before comparing.
#   - endpoint PNG: decoded to RGB and checked directly -- left half
#     (x < 32) uniformly #898989, right half (x >= 32) uniformly
#     #bcbcbc. This is the value the W4(a1) test hardcodes (137, 188).
#
# The AVIF decodes are pinned to libdav1d (-c:v libdav1d), NOT the
# ffmpeg default AV1 decoder. Reason: ffmpeg's native 'av1' decoder
# fails outright on the mozilla-hdr base on ffmpeg 8.1.2, so it is
# excluded as a guard; the 69-level difference seen on the CI
# runner was the scaler's PQ->sRGB conversion, not the decoder
# (pinning libdav1d changed nothing about it -- before the pin the
# decoder already was libdav1d by default). libdav1d and libaom-av1
# both reproduce the committed vectors byte-exact locally; libdav1d
# is pinned because it is the decoder libavif itself uses. If
# libdav1d is missing, ffmpeg fails and this script fails loudly
# instead of silently falling back to another decoder.
#
# Fails (exit 1) on any mismatch, on a size mismatch, or when ffmpeg is
# missing -- never a silent skip. Run in CI by the gainmap-signed-weight
# job in .github/workflows/ymck-guard.yml. Stdlib only; needs ffmpeg.
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SAMPLES = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("tests/samples")
FAILURES = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond:
        FAILURES.append(what)


def ffmpeg_decode(src, stream, pix_fmt, codec=None):
    """Decode one stream of src to rawvideo bytes; returns bytes or None."""
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as tmp:
        out = tmp.name
    cmd = ["ffmpeg", "-v", "error"]
    if codec:
        cmd += ["-c:v", codec]
    cmd += ["-i", str(src), "-map", f"0:{stream}",
            "-pix_fmt", pix_fmt, "-f", "rawvideo", "-y", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"ffmpeg failed for {src} stream {stream}: {r.stderr.strip()}")
        Path(out).unlink(missing_ok=True)
        return None
    data = Path(out).read_bytes()
    Path(out).unlink(missing_ok=True)
    return data


def max_diff(a, b):
    return max((abs(x - y) for x, y in zip(a, b)), default=0)


def u16_to_8(data):
    vals = struct.unpack(f"<{len(data) // 2}H", data)
    return bytes(x >> 8 for x in vals)


def crop_bgra16(data, width, x0, y0, cw, ch):
    out = bytearray()
    for y in range(y0, y0 + ch):
        s = (y * width + x0) * 8
        out += data[s:s + cw * 8]
    return bytes(out)


def crop_gray(data, width, x0, y0, cw, ch):
    out = bytearray()
    for y in range(y0, y0 + ch):
        s = y * width + x0
        out += data[s:s + cw]
    return bytes(out)


def gbrp_to_bgra8(data, width, height):
    """Native gbrp planes (G, B, R, 8-bit) -> interleaved BGRA (A=255)."""
    n = width * height
    g = data[0:n]
    b = data[n:2 * n]
    r = data[2 * n:3 * n]
    out = bytearray()
    for i in range(n):
        out += bytes((b[i], g[i], r[i], 255))
    return bytes(out)


def main():
    import shutil
    if shutil.which("ffmpeg") is None:
        print("FAIL ffmpeg not found on PATH -- cannot verify the vectors")
        return 1

    cases = [
        # (stem, width, height, crop or None, native gbrp base?)
        ("mozilla-hdr", 64, 32, None, True),
        ("mozilla-sdr", 64, 32, None, False),
        ("seine-hdr", 400, 300, (136, 86, 128, 128), False),
        ("seine-sdr", 400, 300, (136, 86, 128, 128), False),
    ]
    for stem, w, h, crop, native_gbr in cases:
        avif = SAMPLES / f"{stem}-base-gainmap.avif"
        suffix = "-crop128" if crop else ""
        base_vec = (SAMPLES / f"{stem}-base16{suffix}.bgra").read_bytes()
        gain_vec = (SAMPLES / f"{stem}-gain8{suffix}.raw").read_bytes()

        if native_gbr:
            raw_base = ffmpeg_decode(avif, 0, "gbrp", "libdav1d")
            if raw_base is None:
                check(False, f"{stem}: ffmpeg decode failed")
                continue
            base8 = gbrp_to_bgra8(raw_base, w, h)
            vec8 = u16_to_8(base_vec)
            # Compare only the B/G/R bytes (alpha is 255 on both sides).
            d_base = max(max_diff(base8[i * 4:i * 4 + 3], vec8[i * 4:i * 4 + 3])
                         for i in range(w * h))
            check(len(raw_base) == w * h * 3 and d_base <= 1,
                  f"{stem} base16 native gbrp @8bit: max diff {d_base} (tolerance 1)")
            if d_base > 1:
                print(f"  first decoded pixel (BGR): {list(base8[0:3])}, "
                      f"first vector pixel (BGR): {list(vec8[0:3])}")
            raw_gain = ffmpeg_decode(avif, 1, "gray", "libdav1d")
            if raw_gain is None:
                check(False, f"{stem}: ffmpeg gain decode failed")
                continue
            d_gain = max_diff(raw_gain, gain_vec)
            check(len(raw_gain) == len(gain_vec) and d_gain <= 1,
                  f"{stem} gain8: max diff {d_gain} (tolerance 1)")
            if d_gain > 1:
                print(f"  first decoded gain byte: {raw_gain[0]}, "
                      f"first vector gain byte: {gain_vec[0]}")
            continue

        raw_base = ffmpeg_decode(avif, 0, "bgra64le", "libdav1d")
        raw_gain = ffmpeg_decode(avif, 1, "gray", "libdav1d")
        if raw_base is None or raw_gain is None:
            check(False, f"{stem}: ffmpeg decode failed")
            continue
        if crop:
            x0, y0, cw, ch = crop
            raw_base = crop_bgra16(raw_base, w, x0, y0, cw, ch)
            raw_gain = crop_gray(raw_gain, w, x0, y0, cw, ch)
        # Base: compare at 8-bit (see header for why 16-bit differs).
        raw8 = u16_to_8(raw_base)
        vec8 = u16_to_8(base_vec)
        d_base = max_diff(raw8, vec8)
        check(len(raw_base) == len(base_vec) and d_base <= 1,
              f"{stem} base16 @8bit: max diff {d_base} (tolerance 1)")
        if d_base > 1:
            print(f"  first decoded pixel (BGRA): {list(raw8[0:4])}, "
                  f"first vector pixel (BGRA): {list(vec8[0:4])}")
        d_gain = max_diff(raw_gain, gain_vec)
        check(len(raw_gain) == len(gain_vec) and d_gain <= 1,
              f"{stem} gain8: max diff {d_gain} (tolerance 1)")
        if d_gain > 1:
            print(f"  first decoded gain byte: {raw_gain[0]}, "
                  f"first vector gain byte: {gain_vec[0]}")

    # Endpoint PNG: the greys W4(a1) hardcodes, read from the file itself.
    png = SAMPLES / "mozilla-sdr-endpoint.png"
    raw = ffmpeg_decode(png, 0, "rgb24")
    if raw is None:
        check(False, "endpoint PNG: ffmpeg decode failed")
    else:
        W, H = 64, 32
        ok_size = len(raw) == W * H * 3
        left = all(raw[(y * W + x) * 3:(y * W + x) * 3 + 3] == b"\x89\x89\x89"
                   for y in range(H) for x in range(32))
        right = all(raw[(y * W + x) * 3:(y * W + x) * 3 + 3] == b"\xbc\xbc\xbc"
                    for y in range(H) for x in range(32, 64))
        check(ok_size and left, "endpoint PNG: left half is #898989 (137)")
        check(ok_size and right, "endpoint PNG: right half is #bcbcbc (188)")

    if FAILURES:
        print(f"check_gainmap_vectors: {len(FAILURES)} failure(s)")
        return 1
    print("check_gainmap_vectors: all vectors tie to their AVIF/PNG sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
