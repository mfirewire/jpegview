#!/usr/bin/env python3
"""Reference check for the CMYK test samples (B-CMYK1).

This is the INDEPENDENT cross-check for tests/samples/cmyk_chart_6x1.psd
and .jpx: it decodes both files with decoders that are not the product
and not the test's own parsers, and asserts the picture. It does not
import or reuse any code from tests/cmyk_pixel_test.cpp.

Checks:
  PSD - Pillow opens it as CMYK and returns exactly the ink table.
      - A raw byte-level parse (header fields, planar layout, stored
        bytes == 255 - ink) confirms the file really stores inverted
        planes, so the C++ test's ToInk step is tested against the
        file rather than against itself.
  JP2 - Pillow reads the four raw component values back exactly.
        (Pillow reports the mode as RGBA: it does not interpret the
        CMYK colr box, it just returns the four components. The values
        are what matter here.)
      - opj_decompress (system OpenJPEG CLI, when available) converts
        the file to RGB; the result must be the expected picture. The
        mid-grey patch is allowed +/-1: OpenJPEG's own CMYK->RGB
        conversion gives 126 where the naive ink formula gives 127.
        That 1-LSB difference is a property of the two conversions,
        not an error in either.
      - opj_dump (when available) must report 4 components, 6x1.

What this does NOT prove (same honest limit as the C++ test): the
polarity axiom itself. These samples were generated from the PSD/JP2
specifications, so they prove the files are well-formed and the
decoders agree on their contents - not that the specification's
polarity is the one real Photoshop files use. That proof remains the
one in src/JPEGView/YmckDecode.h (cmyk_chart_rswop.psd on build
19897de). The product wrappers still need a Windows device test.

Mutations exercised (2026-11-15, Pillow 10.2.0, libopenjp2-tools
2.5.0):
  * PSD with one plane byte wrong (C plane, white patch, stored 255
    -> 254): CAUGHT - the raw-planes check and the Pillow-pixels
    check both fail, exit 1.
  * JP2 with the colr patch reverted (enumCS 12 -> 0, as OpenJPEG
    2.5.0 wrote it): CAUGHT by opj_decompress - without the CMYK
    declaration it emits 4 components (24 bytes) instead of
    converting to 3-component RGB (18 bytes), so the length check
    fails, exit 1. (The Pillow check alone does not catch it:
    Pillow reports the same mode (RGBA) and the same component
    values either way; it does not interpret enumCS. opj_dump
    numcomps=4 still passes.)
  Count: the earlier "4 mutations" claim had 2 demonstrated (change
  an expected value, add -DYMCK_PLANE_IS_INVERTED=0) and these 2
  unrecorded; all 4 are now demonstrated caught.

Usage (from the repository root):
    python3 tests/cmyk_reference_check.py [tests/samples] [--allow-missing-tools]

libopenjp2-tools (opj_decompress, opj_dump) is installed in CI, so a
missing or failing tool is a failure there, not a skip. Locally,
pass --allow-missing-tools to skip the OpenJPEG checks explicitly.

Exit code 0 = all checks passed, 1 = at least one failed.
"""

import struct
import subprocess
import sys
from pathlib import Path

# Ink (C, M, Y, K) per patch, left to right. Must match the generator
# (tests/samples/make_cmyk_samples.py) and the C++ test.
INKS = [
    (0, 0, 0, 0),      # white
    (0, 0, 0, 255),    # black
    (255, 0, 0, 0),    # cyan
    (0, 255, 0, 0),    # magenta
    (0, 0, 255, 0),    # yellow
    (0, 0, 0, 128),    # mid grey (half K)
]
EXPECTED_RGB = [
    (255, 255, 255),
    (0, 0, 0),
    (0, 255, 255),
    (255, 0, 255),
    (255, 255, 0),
    (127, 127, 127),
]
WIDTH, HEIGHT = 6, 1

failures = 0
checks = 0

# Set from --allow-missing-tools (see the usage note above).
ALLOW_MISSING_TOOLS = "--allow-missing-tools" in sys.argv


def missing_tool(what, detail):
    # A missing OpenJPEG CLI is an environment failure in CI, where
    # libopenjp2-tools is installed; only an explicit local opt-out
    # turns it back into a skip.
    if ALLOW_MISSING_TOOLS:
        print("skip  %s not available: %s" % (what, detail))
    else:
        check(False, "%s available (libopenjp2-tools installed)" % what, detail)


def check(ok, what, detail=""):
    global failures, checks
    checks += 1
    if ok:
        print("ok    %s" % what)
    else:
        failures += 1
        print("FAIL  %s %s" % (what, detail))


def check_rgb_close(got, want, what, tol=1):
    ok = all(abs(g - w) <= tol for g, w in zip(got, want))
    check(ok, what, "got %s, expected %s (+/-%d)" % (got, want, tol))


def check_psd(path):
    from PIL import Image

    data = path.read_bytes()
    # Raw byte-level parse: header fields, then planar data.
    ok = (
        len(data) == 26 + 12 + 2 + 4 * WIDTH * HEIGHT
        and data[0:4] == b"8BPS"
        and struct.unpack(">H", data[4:6])[0] == 1
        and struct.unpack(">H", data[12:14])[0] == 4
        and struct.unpack(">I", data[14:18])[0] == HEIGHT
        and struct.unpack(">I", data[18:22])[0] == WIDTH
        and struct.unpack(">H", data[22:24])[0] == 8
        and struct.unpack(">H", data[24:26])[0] == 4
        and struct.unpack(">H", data[38:40])[0] == 0  # compression: raw
    )
    check(ok, "PSD raw header: 8BPS v1, 4ch, 6x1, 8-bit, mode CMYK, raw")
    planes = data[40:]
    stored_ok = all(
        planes[ch * WIDTH + x] == 255 - INKS[x][ch]
        for x in range(WIDTH)
        for ch in range(4)
    )
    check(stored_ok, "PSD raw planes: C/M/Y/K planar, stored byte is 255 - ink")
    # Independent decoder: Pillow.
    im = Image.open(path)
    check(im.mode == "CMYK" and im.size == (WIDTH, HEIGHT),
          "PSD Pillow: mode CMYK, size 6x1",
          "got mode %s size %s" % (im.mode, im.size))
    im.load()
    pixels = list(im.getdata())
    check(pixels == INKS, "PSD Pillow pixels equal the ink table",
          "got %s" % (pixels,))


def check_jp2(path):
    from PIL import Image

    # Independent decoder 1: Pillow returns the four raw components
    # (it labels the mode RGBA because it does not interpret the CMYK
    # colr box; the component values are the check).
    im = Image.open(path)
    im.load()
    pixels = list(im.getdata())
    check(pixels == INKS, "JP2 Pillow component values equal the ink table",
          "got mode %s pixels %s" % (im.mode, pixels))
    # Independent decoder 2: system OpenJPEG CLI converts to RGB.
    try:
        out = subprocess.run(
            ["opj_decompress", "-i", str(path), "-o", "/tmp/cmyk_ref.raw"],
            capture_output=True, text=True, check=True,
        )
    except FileNotFoundError as e:
        missing_tool("opj_decompress", str(e))
        return
    except subprocess.CalledProcessError as e:
        check(False, "JP2 opj_decompress converts CMYK to RGB",
              "exit %d: %s" % (e.returncode, e.stderr.strip()))
        return
    raw = Path("/tmp/cmyk_ref.raw").read_bytes()
    check(len(raw) == 3 * WIDTH * HEIGHT,
          "JP2 opj_decompress: converts CMYK to 3-component RGB",
          "got %d bytes" % len(raw))
    if len(raw) == 3 * WIDTH * HEIGHT:
        # opj_decompress writes planar RGB.
        for x in range(WIDTH):
            got = (raw[x], raw[WIDTH + x], raw[2 * WIDTH + x])
            check_rgb_close(got, EXPECTED_RGB[x],
                            "JP2 opj_decompress pixel %d -> RGB" % x)
    # Independent structure check: opj_dump reports 4 components.
    try:
        dump = subprocess.run(
            ["opj_dump", "-i", str(path)],
            capture_output=True, text=True, check=True,
        ).stdout
    except FileNotFoundError as e:
        missing_tool("opj_dump", str(e))
        return
    except subprocess.CalledProcessError as e:
        check(False, "JP2 opj_dump: numcomps=4",
              "exit %d: %s" % (e.returncode, e.stderr.strip()))
        return
    check("numcomps=4" in dump, "JP2 opj_dump: numcomps=4")


def main():
    args = [a for a in sys.argv[1:] if a != "--allow-missing-tools"]
    samples = Path(args[0]) if args else Path("tests/samples")
    print("CMYK reference check (samples: %s)" % samples)
    check_psd(samples / "cmyk_chart_6x1.psd")
    check_jp2(samples / "cmyk_chart_6x1.jpx")
    if failures == 0:
        print("PASS  %d checks" % checks)
        return 0
    print("FAILED  %d of %d checks" % (failures, checks))
    return 1


if __name__ == "__main__":
    sys.exit(main())
