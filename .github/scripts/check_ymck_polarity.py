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
     YmckDecode::InvertPlaneInPlace() in PSDWrapper.cpp, with the pixel
     count and stride arguments checked too: zero means the 9746da5
     regression is back, two means a double inversion (ec8032f), and a
     wrong count or stride inverts the wrong region.
  3. All three naive CMYK converters must route through a single YmckDecode
     function so the component permutation cannot be re-inlined incorrectly:
     YmckToRgb(p[0], p[1], p[2], p[3]) for the YMCK plane order (PSD,
     ImageLoadThread) and InkToRgb(v[0], v[1], v[2], v[3]) for JP2's
     C/M/Y/K component order.
  4. No raw `~` complement on a pixel buffer outside the header. This
     catches `~(*p)`, `~*p` and `~word`. Known-safe idioms are excluded:
     `& ~FLAG` flag clearing, `~x & 1` parity, `~c + 2` PackBits run length,
     and only the `~word` rule excludes dcraw_mod.cpp (vendored); a `~Name(` form is excluded so
     destructor definitions are not flagged.

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
# Raw complement of a pixel word: `~(*pImage32)` and `~*p`. The bare-`~word`
# form is a separate pattern below because it needs context exclusions;
# a single regex for it flags 21 legitimate uses (flag clearing, parity,
# PackBits run length, dcraw internals, `~` in a format string).
RAW_COMPLEMENT = re.compile(r"~\s*\(?\s*\*")

# Bare `~word`: `~` followed by an identifier. Two regex guards keep it from
# flagging destructors: `(?![A-Za-z0-9_])` forces the identifier to be maximal
# (without it the pattern backtracks to a one-letter prefix and `~CClass()`
# matches), and `(?!\s*\()` excludes a following `(` (destructor `~CClass()`
# and a complement of a function call `~func()` look the same; the former is
# far more common here, so `~func()` is a known blind spot). `~(size_t)3` and
# `~(nBlockWidth - 1)` are not caught: `~` is followed by `(`, not by an
# identifier, so bitmask alignment stays clean.
RAW_COMPLEMENT_WORD = re.compile(r"~\s*[A-Za-z_][A-Za-z0-9_]*(?![A-Za-z0-9_])(?!\s*\()")


def strip_strings(text):
    """Blank the contents of double-quoted strings and char literals, so a
    `~` inside a format string (e.g. "%s~jv%lu") is not mistaken for a
    complement. Only the delimiters are kept, so line structure is intact."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in ("\"", "'"):
            quote = c
            out.append(quote)
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    i += 2
                else:
                    i += 1
            if i < n:
                out.append(quote)
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _is_pixel_complement(line, m):
    """True if a `~word` match is a complement on a value, not a known-safe idiom."""
    before = line[:m.start()].rstrip()
    if before.endswith("&") or before.endswith("&="):
        return False  # flag/mask clearing: `& ~FLAG`, `&= ~FLAG`
    after = line[m.end():]
    rest = after.lstrip()
    if rest.startswith("->"):
        return True  # member access: `~p->field` is a pixel complement, not a safe idiom
    if rest[:1] in ("&", "|", "+", "-"):
        return False  # parity/bitmask/arithmetic: `~x & 1`, `~c + 2` (PackBits run length)
    return True

YMCK_TO_RGB_CALL = "YmckDecode::YmckToRgb(p[0],p[1],p[2],p[3])"
INVERT_CALL = "YmckDecode::InvertPlaneInPlace("
# Compacted form of the single InvertPlaneInPlace call: buffer, pixel
# count, stride (4 or 5 bytes per pixel). The count check alone cannot
# tell a wrong count or stride from the right one.
INVERT_CALL_ARGS = "InvertPlaneInPlace(pPixelData,(size_t)nWidth*nHeight,nChannels==5?5:4)"
JP2 = OWN_SRC / "JP2Wrapper.cpp"
INK_TO_RGB_CALL = "InkToRgb("
# Canonical InkToRgb call: same base variable indexed [0],[1],[2],[3] in
# order, so a permuted call (e.g. v[2],v[1],v[0],v[3]) does not match.
INK_TO_RGB_CANONICAL = re.compile(r"InkToRgb\(([A-Za-z_][A-Za-z0-9_]*)\[0\],\1\[1\],\1\[2\],\1\[3\]\)")
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
            if path.name != "dcraw_mod.cpp":
                code_nostr = strip_strings(code)
                for m in RAW_COMPLEMENT_WORD.finditer(code_nostr):
                    if _is_pixel_complement(code_nostr, m):
                        problems.append(
                            f"{path.relative_to(ROOT)}:{lineno}: raw '~' complement on a pixel "
                            f"buffer -> YmckDecode::InvertPlaneInPlace(). Line: {code.strip()}"
                        )
                        break

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
        if INVERT_CALL_ARGS not in psd:
            problems.append(
                "src/JPEGView/PSDWrapper.cpp: the single InvertPlaneInPlace call "
                "must pass the pixel count and stride "
                "(pPixelData, (size_t)nWidth*nHeight, nChannels==5?5:4); a wrong "
                "count or stride inverts the wrong region"
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

    if not JP2.exists():
        problems.append("src/JPEGView/JP2Wrapper.cpp not found")
    else:
        jp2 = compact(strip_comments(read(JP2)))
        ink_calls = jp2.count(INK_TO_RGB_CALL)
        canonical = INK_TO_RGB_CANONICAL.findall(jp2)
        if len(canonical) < 2 or ink_calls != len(canonical):
            problems.append(
                "src/JPEGView/JP2Wrapper.cpp: the naive CMYK converter must call "
                "YmckDecode::InkToRgb(v[0], v[1], v[2], v[3]) in component order "
                f"(found {len(canonical)} canonical call(s) out of {ink_calls} "
                "InkToRgb call(s)); a permuted or re-inlined conversion bypasses "
                "the polarity contract"
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
