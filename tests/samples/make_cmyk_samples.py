#!/usr/bin/env python3
"""Generate the CMYK end-to-end test samples (B-CMYK1).

Writes, next to this script:
  cmyk_chart_6x1.psd - 6x1 uncompressed 8-bit 4-channel CMYK PSD
  cmyk_chart_6x1.jpx - 6x1 lossless CMYK JP2 (JP2 container, .jpx name)

Both files share one strip of six patches; the ink table below is the
single source of truth (C, M, Y, K; 0 = no ink, 255 = full ink):

    white    (  0,   0,   0,   0) -> RGB (255,255,255)
    black    (  0,   0,   0, 255) -> RGB (  0,  0,  0)
    cyan     (255,   0,   0,   0) -> RGB (  0,255,255)
    magenta  (  0, 255,   0,   0) -> RGB (255,  0,255)
    yellow   (  0,   0, 255,   0) -> RGB (255,255,  0)
    mid grey (  0,   0,   0, 128) -> RGB (127,127,127)

PSD
---
Written by hand to the PSD specification (signature 8BPS, version 1,
4 channels, depth 8, colour mode 4 = CMYK, compression 0 = raw, image
data planar in C/M/Y/K order). PSD stores each plane INVERTED, so the
stored byte is 255 - ink. No colour-mode data, no image resources, no
embedded ICC profile - the product therefore takes its no-ICC naive
branch for this file. Verified after generation with Pillow
(Image.open -> mode CMYK, pixels equal to the ink table) and by
tests/cmyk_reference_check.py, which also re-parses the raw bytes.

JP2
---
Pillow 10.2 cannot write a CMYK JPEG 2000 (OSError: broken data stream),
and opj_compress rejects PAM TUPLTYPE CMYK, so the file is encoded with
the system OpenJPEG library (libopenjp2 2.5.0; the vendored copy is the
same 2.5 line, v2.5.4, but ships Windows binaries only) through the small
C encoder embedded below: lossless 5-3 DWT, 1 resolution level (a 6x1
image is too small for the default 6), tcp_mct = 0, colour space
OPJ_CLRSPC_CMYK, components in C/M/Y/K order holding ink amounts
(0 = no ink) directly.

One post-encode fix is applied, and it is applied honestly: OpenJPEG
2.5.0's JP2 encoder writes the colr box with enumCS = 0 (unspecified)
even when the image colour space is CMYK, so a decoder never learns
the file is CMYK. The script patches exactly those 4 bytes to 12
(CMYK, ISO/IEC 15444-1 enumerated colour space), asserting first that
the box is a 15-byte colr box with method 1 and the enumCS field
currently 0. The codestream (pixel data) is untouched. Verified after
the patch: openjpeg reports color_space CMYK after decode with the
component values intact, opj_decompress converts the file to the
expected RGB picture, and Pillow reads the raw component values back.

Requirements to regenerate: python3, a C compiler, and the system
OpenJPEG development files (pkg-config libopenjp2). The checked-in
samples are the deliverable; this script is their provenance record.
After regenerating, run tests/cmyk_reference_check.py and the C++
test (see tests/cmyk_pixel_test.cpp) before committing.

Usage:  python3 tests/samples/make_cmyk_samples.py
"""

import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# Ink (C, M, Y, K) per patch, left to right. 0 = no ink, 255 = full ink.
INKS = [
    (0, 0, 0, 0),      # white
    (0, 0, 0, 255),    # black
    (255, 0, 0, 0),    # cyan
    (0, 255, 0, 0),    # magenta
    (0, 0, 255, 0),    # yellow
    (0, 0, 0, 128),    # mid grey (half K)
]
WIDTH = len(INKS)
HEIGHT = 1

C_ENCODER = r"""
/* Minimal lossless CMYK JP2 encoder (system libopenjp2; vendored is 2.5.4,
   same 2.5 line, Windows binaries only). Usage: make_jp2 <output.jp2> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openjpeg.h>
int main(int argc, char** argv) {
    static const unsigned char ink[6][4] = {
        {0,0,0,0},{0,0,0,255},{255,0,0,0},{0,255,0,0},{0,0,255,0},{0,0,0,128}
    };
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.tcp_numlayers = 1;
    params.tcp_rates[0] = 0; /* lossless */
    params.cp_disto_alloc = 1;
    params.tcp_mct = 0;      /* no multi-component transform for CMYK */
    params.numresolution = 1; /* 6x1 is too small for the default 6 */
    opj_codec_t* codec = opj_create_compress(OPJ_CODEC_JP2);
    opj_image_cmptparm_t cmpt[4];
    memset(cmpt, 0, sizeof cmpt);
    for (int c = 0; c < 4; c++) {
        cmpt[c].dx = 1; cmpt[c].dy = 1; cmpt[c].w = 6; cmpt[c].h = 1;
        cmpt[c].x0 = 0; cmpt[c].y0 = 0; cmpt[c].prec = 8; cmpt[c].sgnd = 0;
    }
    opj_image_t* image = opj_image_create(4, cmpt, OPJ_CLRSPC_CMYK);
    image->x0 = 0; image->y0 = 0; image->x1 = 6; image->y1 = 1;
    for (int c = 0; c < 4; c++)
        for (int x = 0; x < 6; x++)
            image->comps[c].data[x] = ink[x][c];
    opj_stream_t* stream = opj_stream_create_default_file_stream(argv[1], OPJ_FALSE);
    if (!stream) { fprintf(stderr, "cannot open output\n"); return 1; }
    if (!opj_setup_encoder(codec, &params, image)) { fprintf(stderr, "setup failed\n"); return 1; }
    if (!opj_start_compress(codec, image, stream)) { fprintf(stderr, "start failed\n"); return 1; }
    if (!opj_encode(codec, stream)) { fprintf(stderr, "encode failed\n"); return 1; }
    if (!opj_end_compress(codec, stream)) { fprintf(stderr, "end failed\n"); return 1; }
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    opj_image_destroy(image);
    printf("wrote %s\n", argv[1]);
    return 0;
}
"""


def make_psd(path):
    # Header: 8BPS, version 1, 6 reserved bytes, 4 channels, H, W,
    # depth 8, colour mode 4 (CMYK).
    header = (
        b"8BPS"
        + struct.pack(">H", 1)
        + b"\x00" * 6
        + struct.pack(">H", 4)
        + struct.pack(">I", HEIGHT)
        + struct.pack(">I", WIDTH)
        + struct.pack(">H", 8)
        + struct.pack(">H", 4)
    )
    # No colour-mode data, no image resources, no reserved data.
    sections = struct.pack(">I", 0) + struct.pack(">I", 0) + struct.pack(">I", 0)
    compression = struct.pack(">H", 0)  # 0 = raw, uncompressed
    # Planar image data, planes in file order C, M, Y, K; PSD stores
    # each plane inverted (255 = no ink), so the byte is 255 - ink.
    planes = b"".join(
        bytes(255 - INKS[x][ch] for x in range(WIDTH)) for ch in range(4)
    )
    path.write_bytes(header + sections + compression + planes)


def make_jp2(path):
    with tempfile.TemporaryDirectory(prefix="cmyk_jp2_") as tmp:
        src = Path(tmp) / "make_jp2.c"
        exe = Path(tmp) / "make_jp2"
        raw = Path(tmp) / "raw.jp2"
        src.write_text(C_ENCODER)
        cflags = subprocess.check_output(
            ["pkg-config", "--cflags", "libopenjp2"], text=True
        ).split()
        libs = subprocess.check_output(
            ["pkg-config", "--libs", "libopenjp2"], text=True
        ).split()
        subprocess.run(
            ["cc", "-O2", str(src), "-o", str(exe)] + cflags + libs,
            check=True,
        )
        subprocess.run([str(exe), str(raw)], check=True)
        data = bytearray(raw.read_bytes())
    # OpenJPEG 2.5.0 writes enumCS = 0 in the colr box for CMYK; patch
    # exactly that field to 12 (CMYK). Assert the box structure first
    # so this can never silently patch the wrong bytes.
    i = data.find(b"colr")
    assert i > 0, "colr box not found in encoded JP2"
    box_len = int.from_bytes(data[i - 4 : i], "big")
    assert box_len == 15, "unexpected colr box length %d" % box_len
    assert data[i + 4] == 1, "colr method is not enumerated"
    assert data[i + 5] == 0 and data[i + 6] == 0, "unexpected colr flags"
    enum_cs = int.from_bytes(data[i + 7 : i + 11], "big")
    assert enum_cs == 0, "encoder already wrote enumCS %d, not 0" % enum_cs
    data[i + 7 : i + 11] = (12).to_bytes(4, "big")
    path.write_bytes(bytes(data))


def main():
    here = Path(__file__).resolve().parent
    psd_path = here / "cmyk_chart_6x1.psd"
    jpx_path = here / "cmyk_chart_6x1.jpx"
    make_psd(psd_path)
    print("wrote %s (%d bytes, sha256 %s)" % (
        psd_path, psd_path.stat().st_size,
        hashlib.sha256(psd_path.read_bytes()).hexdigest()[:16]))
    make_jp2(jpx_path)
    print("wrote %s (%d bytes, sha256 %s)" % (
        jpx_path, jpx_path.stat().st_size,
        hashlib.sha256(jpx_path.read_bytes()).hexdigest()[:16]))
    print("Now run: python3 tests/cmyk_reference_check.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
