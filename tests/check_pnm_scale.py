#!/usr/bin/env python3
"""Guard the PNM maxval scaling contract declared in PnmScale.h.

Run from the repository root:  python3 tests/check_pnm_scale.py

Why this exists
---------------
PNM white is maxval, not 255, and stb_image ignores the field: a maxval=15
PGM used to display near-black and a 16-bit maxval=4095 file came out at
15/255 (B-PNM1). The scaling arithmetic now lives in exactly one place,
src/JPEGView/PnmScale.h, and this script keeps it there - a second copy in
PPMWrapper.cpp would drift from the tested one exactly the way the YMCK
polarity copies did (see .github/scripts/check_ymck_polarity.py).

Rules (comments are stripped before scanning, so prose does not count)
  1. PnmScale.h must define ScaleSamples and hold the scaling arithmetic
     (`* 255u`); it is the single home.
  2. PPMWrapper.cpp must include "PnmScale.h" and call
     PnmScale::ScaleSamples exactly once.
  3. PPMWrapper.cpp must not contain a second copy of the arithmetic: no
     `* 255` multiply and no `nDivisor` identifier in its code.

Exit code 0 = clean, 1 = at least one violation.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "JPEGView" / "PnmScale.h"
WRAPPER = ROOT / "src" / "JPEGView" / "PPMWrapper.cpp"


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def strip_comments(text):
    """Drop // and /* */ comments so the rules only see compiled code."""
    out = []
    i = 0
    n = len(text)
    state = None  # None, "line", "block", "string", "char"
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/":
                state = "line"
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                i += 2
                continue
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            out.append(c)
        elif state == "line":
            if c == "\n":
                state = None
                out.append(c)
        elif state == "block":
            if c == "*" and nxt == "/":
                state = None
                i += 2
                continue
            if c == "\n":
                out.append(c)
        elif state in ("string", "char"):
            out.append(c)
            if c == "\\":
                if i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                continue
            if (state == "string" and c == '"') or (state == "char" and c == "'"):
                state = None
        i += 1
    return "".join(out)


def main():
    problems = []

    if not HEADER.exists():
        print(f"::error::missing {HEADER.relative_to(ROOT)} - the PNM scale has no home")
        return 1
    header_code = strip_comments(read(HEADER))
    if "ScaleSamples" not in header_code:
        problems.append(
            "src/JPEGView/PnmScale.h: ScaleSamples is gone; the PNM maxval scale "
            "must stay a single named function"
        )
    if not re.search(r"\*\s*255u", header_code):
        problems.append(
            "src/JPEGView/PnmScale.h: the scaling multiply (* 255u) is gone from "
            "the header; the scale must live here"
        )

    if not WRAPPER.exists():
        print(f"::error::missing {WRAPPER.relative_to(ROOT)}")
        return 1
    wrapper_raw = read(WRAPPER)
    wrapper_code = strip_comments(wrapper_raw)
    if '#include "PnmScale.h"' not in wrapper_raw:
        problems.append(
            "src/JPEGView/PPMWrapper.cpp: missing #include \"PnmScale.h\""
        )
    calls = wrapper_code.count("PnmScale::ScaleSamples(")
    if calls != 1:
        problems.append(
            "src/JPEGView/PPMWrapper.cpp: expected exactly 1 call to "
            f"PnmScale::ScaleSamples, found {calls}. 0 = the maxval scaling is "
            "not applied, 2+ = a second scaling site"
        )
    if re.search(r"\*\s*255u?", wrapper_code):
        problems.append(
            "src/JPEGView/PPMWrapper.cpp: scaling multiply (* 255) outside "
            "PnmScale.h -> PnmScale::ScaleSamples(). A second copy of the "
            "arithmetic drifts from the tested one"
        )
    if "nDivisor" in wrapper_code:
        problems.append(
            "src/JPEGView/PPMWrapper.cpp: nDivisor outside PnmScale.h -> "
            "PnmScale::ScaleSamples(). The divisor rule lives in the header only"
        )

    if problems:
        for p in problems:
            print(f"::error::{p}")
        print(f"\n{len(problems)} PNM scale violation(s). See src/JPEGView/PnmScale.h.")
        return 1

    print("PNM scale contract: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
