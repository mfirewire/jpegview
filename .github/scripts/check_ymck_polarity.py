#!/usr/bin/env python3
"""Guard the CMYK/YMCK plane polarity contract declared in YmckDecode.h.

Run from the repository root:  python3 .github/scripts/check_ymck_polarity.py

Why this exists
---------------
Three consecutive "fixes" (F-M23, ec8032f, 9746da5) each re-derived the CMYK
polarity from the previous patch instead of from a fact, and the last one
turned every real Photoshop file black. Reading the code cannot settle the
question, so the assumption is pinned in exactly one header and this script
keeps it there.

Rules (project sources: src/JPEGView/*.cpp, *.h - vendored trees are skipped)
  1. Per-channel inversion `255 - x[i]` may appear only in YmckDecode.h.
     Everywhere else use YmckDecode::ToInk(). A line that is genuinely doing
     alpha compositing opts out with a `ymck-ok:` comment.
  2. A whole-plane inversion may appear only as exactly one call to
     YmckDecode::InvertPlaneInPlace() in PSDWrapper.cpp: zero means the
     9746da5 regression is back, two means a double inversion (ec8032f).
  3. Both naive CMYK converters must route through YmckDecode::YmckToRgb()
     so the Y/M/C/K permutation cannot be re-inlined incorrectly.
  4. No raw `~` complement on a pixel buffer outside the header.

Exit code 0 = clean, 1 = at least one violation.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OWN_SRC = ROOT / "src" / "JPEGView"
HEADER = OWN_SRC / "YmckDecode.h"
PSD = OWN_SRC / "PSDWrapper.cpp"
JPEG = OWN_SRC / "ImageLoadThread.cpp"

SOURCE_EXT = (".c", ".cc", ".cpp", ".h", ".hh", ".hpp", ".inl")

# `255 - x[0]`, `(255 - sp[3])`, `255-p[2]`. Requires an indexed operand so
# ordinary scalars (alpha compositing) are not caught; those still need the
# `ymck-ok:` marker only when they look like a plane access.
INVERT_LITERAL = re.compile(r"255\s*-\s*[A-Za-z_][A-Za-z0-9_]*\s*\[")

# Raw complement of a pixel word: `~(*pImage32)`, `= ~word`.
RAW_COMPLEMENT = re.compile(r"~\s*\(?\s*\*")

YMCK_TO_RGB_CALL = "YmckDecode::YmckToRgb(p[0],p[1],p[2],p[3])"
INVERT_CALL = "YmckDecode::InvertPlaneInPlace("
OPTOUT = "ymck-ok:"


def read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def strip_comments(text):
    """Drop // and /* */ comments so the rules only see compiled code.

    Comments legitimately write `255 - ink` in prose; the contract is about
    expressions. Also keeps the `ymck-ok:` marker out of the scanned text,
    which is why the opt-out is checked against the raw line instead.
    """
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


def compact(text):
    """Whitespace-insensitive view, for matching a call that was reformatted."""
    return re.sub(r"\s+", "", text)


def main():
    problems = []

    if not HEADER.exists():
        print(f"::error::missing {HEADER.relative_to(ROOT)} - the polarity contract has no home")
        return 1
    if "kPlaneIsInverted" not in strip_comments(read(HEADER)):
        problems.append(
            f"{HEADER.relative_to(ROOT)}: kPlaneIsInverted is gone; plane polarity must "
            "stay a single named constant"
        )

    for path in sorted(OWN_SRC.glob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_EXT:
            continue
        if path.resolve() == HEADER.resolve():
            continue
        raw_lines = read(path).splitlines()
        code_lines = strip_comments(read(path)).splitlines()
        for lineno, code in enumerate(code_lines, 1):
            raw = raw_lines[lineno - 1] if lineno - 1 < len(raw_lines) else ""
            if OPTOUT in raw:
                continue
            if INVERT_LITERAL.search(code):
                problems.append(
                    f"{path.relative_to(ROOT)}:{lineno}: per-channel inversion outside "
                    f"YmckDecode.h -> YmckDecode::ToInk(). Line: {code.strip()}"
                )
            if RAW_COMPLEMENT.search(code):
                problems.append(
                    f"{path.relative_to(ROOT)}:{lineno}: raw '~' complement on a pixel "
                    f"buffer -> YmckDecode::InvertPlaneInPlace(). Line: {code.strip()}"
                )

    if not PSD.exists():
        problems.append("src/JPEGView/PSDWrapper.cpp not found")
    else:
        psd = compact(strip_comments(read(PSD)))
        calls = psd.count(INVERT_CALL)
        if calls != 1:
            problems.append(
                f"src/JPEGView/PSDWrapper.cpp: expected exactly 1 call to "
                f"InvertPlaneInPlace (shared by the ICC and fallback paths), found {calls}. "
                "0 = the 9746da5 black-PSD regression, 2 = a double inversion (ec8032f)."
            )
        if YMCK_TO_RGB_CALL not in psd:
            problems.append(
                "src/JPEGView/PSDWrapper.cpp: the naive fallback must call "
                "YmckDecode::YmckToRgb(p[0], p[1], p[2], p[3]) so the Y/M/C/K order is "
                "not re-inlined"
            )

    if not JPEG.exists():
        problems.append("src/JPEGView/ImageLoadThread.cpp not found")
    else:
        jpeg = compact(strip_comments(read(JPEG)))
        if YMCK_TO_RGB_CALL not in jpeg:
            problems.append(
                "src/JPEGView/ImageLoadThread.cpp: ConvertYmckInkToRgbNaive must call "
                "YmckDecode::YmckToRgb(p[0], p[1], p[2], p[3]) - one formula for every "
                "CMYK decoder"
            )

    if problems:
        for p in problems:
            print(f"::error::{p}")
        print(f"\n{len(problems)} polarity violation(s). See src/JPEGView/YmckDecode.h.")
        return 1

    print("YMCK polarity contract: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
