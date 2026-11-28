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
#     The 8-bit check cannot see low-byte damage, so a normalized
#     16-bit check runs alongside it (P3-6): r8 = vec - ff - (ff>>8)
#     for 8-bit sources, r10 = vec - ff - (ff>>10) for 10-bit, BGR
#     only, tolerance 3 (r8) / 2 (r10). Saturated channels (ff or vec
#     == 65535) are carved out -- there v*257 predicts 65790 and both
#     sides truncate to 65535. The v*257-vs-v*256 story holds only
#     for the Mozilla pair; the Seine vectors are true 16-bit
#     YUV->RGB output, so the normalized form (not a raw 16-bit
#     comparison) is what makes the low byte meaningful.
#     Fragility (option A): the two Seine 8-bit diffs sit exactly at
#     tolerance 1, so a 1-LSB decoder rounding change reds CI loudly
#     (a loud failure, not a silent pass).
#     EXCEPTION -- mozilla-hdr base pixel tie is SKIPPED in CI
#     (documented manual step, P1-1 section 13(c) fallback). No ffmpeg
#     decode variant reproduces the committed vector on the CI
#     runner's ffmpeg 8.0.1 (Ubuntu 26.04 resolute, libavcodec62,
#     dav1d 1.5.3) -- three attempts, all max diff 69 (255 - 186):
#     (1) gbrp16le: the scaler inserted for the 8-bit -> 16-bit
#     conversion applies a PQ->sRGB conversion (first pixel 255);
#     (2) native gbrp (-pix_fmt gbrp): the auto-scale misinterprets
#     the RGB planes as YUV, and BT.601 limited-range YUV->RGB of
#     186 gives (255, 125, 255); (3) setparams colorspace=0
#     (AVCOL_SPC_RGB) + format=gbrp inside the filtergraph: the
#     same (255, 125, 255) -- the conversion is not tag-dependent
#     on 8.0.1. So the base pixel tie cannot run in CI. The vector
#     itself is verified by hand instead: tests/samples/
#     mozilla-hdr-base16.bgra (64x32, uniform B=G=R=47802,
#     A=65535) was extracted with libavif; locally, ffmpeg 8.1.2
#     native gbrp decode gives uniform 186 (max diff 0), and
#     libavif's avifdec gives the same. The normalized r8 check for
#     this base is skipped for the same reason (it needs the same
#     decode); its values are verified locally (r8 = 0 on ffmpeg
#     8.1.2 + libdav1d). The mozilla-hdr GAIN tie still runs in CI
#     (gray stream, unaffected by the RGB issue).
#     Manual step (copy-paste; run from the repo root):
#       ffmpeg -v error -c:v libdav1d -i tests/samples/mozilla-hdr-base-gainmap.avif -map 0:0 -pix_fmt gbrp -f rawvideo -y out.raw
#     gives 6144 bytes, uniform 186 (max diff 0 vs the vector >>8).
#       ffmpeg -v error -c:v libdav1d -i tests/samples/mozilla-hdr-base-gainmap.avif -map 0:0 -pix_fmt bgra64le -f rawvideo -y out.raw
#     gives 16384 bytes, BGR 47616 (>>8 = 186); r8 = 47802 - 47616 - 186 = 0.
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
# missing. One documented exception: the mozilla-hdr base pixel tie is
# skipped with a printed SKIP (see EXCEPTION above) -- never a silent
# skip. Run in CI by the gainmap-signed-weight job in
# .github/workflows/ymck-guard.yml. Stdlib only; needs ffmpeg.
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SAMPLES = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("tests/samples")
FAILURES = []
SKIPS = []


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


def main():
    import shutil
    if shutil.which("ffmpeg") is None:
        print("FAIL ffmpeg not found on PATH -- cannot verify the vectors")
        return 1

    cases = [
        # (stem, width, height, crop or None, skip base pixel tie?, source bit depth)
        ("mozilla-hdr", 64, 32, None, True, 8),
        ("mozilla-sdr", 64, 32, None, False, 8),
        ("seine-hdr", 400, 300, (136, 86, 128, 128), False, 10),
        ("seine-sdr", 400, 300, (136, 86, 128, 128), False, 8),
    ]
    for stem, w, h, crop, skip_base, depth in cases:
        avif = SAMPLES / f"{stem}-base-gainmap.avif"
        suffix = "-crop128" if crop else ""
        base_vec = (SAMPLES / f"{stem}-base16{suffix}.bgra").read_bytes()
        gain_vec = (SAMPLES / f"{stem}-gain8{suffix}.raw").read_bytes()

        if skip_base:
            # Section 13(c) fallback: no ffmpeg decode variant
            # reproduces this vector on CI's ffmpeg 8.0.1 (see header
            # EXCEPTION). The base pixel tie is a documented manual
            # step; the gain tie below still runs in CI.
            print(f"SKIP {stem} base16 @8bit: no ffmpeg decode variant "
                  f"reproduces the vector on CI ffmpeg 8.0.1 -- verify "
                  f"by hand (see header EXCEPTION)")
            SKIPS.append(f"{stem} base16 @8bit")
            # The normalized r8 check needs the same ffmpeg decode of
            # this base, so it rides the same manual step.
            print(f"SKIP {stem} base16 normalized r8: same decode as the "
                  f"8-bit tie -- verify by hand (see header EXCEPTION)")
            SKIPS.append(f"{stem} base16 normalized r8")
        else:
            raw_base = ffmpeg_decode(avif, 0, "bgra64le", "libdav1d")
            if raw_base is None:
                check(False, f"{stem}: ffmpeg decode failed")
                continue
            if crop:
                x0, y0, cw, ch = crop
                raw_base = crop_bgra16(raw_base, w, x0, y0, cw, ch)
            # Base: compare at 8-bit (see header for why 16-bit differs).
            raw8 = u16_to_8(raw_base)
            vec8 = u16_to_8(base_vec)
            d_base = max_diff(raw8, vec8)
            check(len(raw_base) == len(base_vec) and d_base <= 1,
                  f"{stem} base16 @8bit: max diff {d_base} (tolerance 1)")
            if d_base > 1:
                print(f"  first decoded pixel (BGRA): {list(raw8[0:4])}, "
                      f"first vector pixel (BGRA): {list(vec8[0:4])}")
            # P3-6 (A+C): normalized 16-bit check on the same decode
            # (no extra ffmpeg call). r8/r10 subtracts the upscale
            # model (vec is v*257, ffmpeg is v*256), so the low byte
            # is pinned; without it a damaged low byte is invisible
            # to the 8-bit check. Saturated channels (ff or vec ==
            # 65535) are carved out: there v*257 predicts 65790,
            # unrepresentable, and both sides truncate to 65535 (a
            # structural artifact, not rounding). Alpha is excluded
            # (both sides 65535, residual -255).
            if len(raw_base) == len(base_vec):
                vec16 = struct.unpack(f"<{len(base_vec) // 2}H", base_vec)
                ff16 = struct.unpack(f"<{len(raw_base) // 2}H", raw_base)
                shift = 8 if depth == 8 else 10
                tol = 3 if depth == 8 else 2
                rname = f"r{depth}"
                worst = 0
                for i in range(len(vec16)):
                    if i % 4 == 3:
                        continue
                    v, f = vec16[i], ff16[i]
                    if f == 65535 or v == 65535:
                        continue
                    r = v - f - (f >> shift)
                    if abs(r) > abs(worst):
                        worst = r
                check(abs(worst) <= tol,
                      f"{stem} base16 normalized {rname}: max |{rname}| {abs(worst)} (tolerance {tol})")
            else:
                check(False, f"{stem} base16 normalized: size mismatch")

        raw_gain = ffmpeg_decode(avif, 1, "gray", "libdav1d")
        if raw_gain is None:
            check(False, f"{stem}: ffmpeg gain decode failed")
            continue
        if crop:
            x0, y0, cw, ch = crop
            raw_gain = crop_gray(raw_gain, w, x0, y0, cw, ch)
        d_gain = max_diff(raw_gain, gain_vec)
        check(len(raw_gain) == len(gain_vec) and d_gain <= 1,
              f"{stem} gain8: max diff {d_gain} (tolerance 1)")
        if d_gain > 1:
            print(f"  first decoded gain byte: {raw_gain[0]}, "
                  f"first vector gain byte: {gain_vec[0]}")

    # P3-3: the 61-byte ISO body W2 hardcodes (1 version byte + 60 body)
    # must be the mozilla-hdr file's own bytes at offset 512, and absent
    # from the SDR twin. The expected bytes are parsed out of the C++
    # test itself, so this ties the test array to the file (a literal
    # copied into this script would only check the script's own copy).
    test_src = (Path(__file__).parent / "gainmap_signed_weight_test.cpp").read_text()
    body = bytes(int(x, 16) for x in re.findall(
        r"0x([0-9a-fA-F]{2})", test_src.split("kMozillaIsoBody[] = {")[1].split("};")[0]))
    hdr = (SAMPLES / "mozilla-hdr-base-gainmap.avif").read_bytes()
    sdr = (SAMPLES / "mozilla-sdr-base-gainmap.avif").read_bytes()
    check(len(body) == 61 and hdr[512:573] == body,
          "mozilla-hdr ISO body: test array matches file bytes at offset 512")
    check(body not in sdr, "mozilla-hdr ISO body: absent from SDR twin")

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
    skipped = f", {len(SKIPS)} skipped: {', '.join(SKIPS)}" if SKIPS else ""
    print(f"check_gainmap_vectors: all checked vectors tie to their AVIF/PNG sources{skipped}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
