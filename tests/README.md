# Tests that pin a contract the compiler cannot see

`ymck_decode_test.cpp` is the reference example for a whole class of bugs in
this project: a decoder's output is only correct relative to a convention that
lives **outside the repository** (a file format, a library, a device). Reading
the source cannot settle it - every wrong variant is internally consistent.

The CMYK plane polarity is the case that cost us three releases-in-a-row:
F-M23, ec8032f and 9746da5 each re-derived the assumption from the previous
patch, and the last one turned every real Photoshop file black while all of our
own samples still "passed". The samples were generated from the same assumption
as the code, so they could only prove self-consistency.

## The pattern

When a value's meaning cannot be derived from the code, do not document it -
**pin it and make the pin fail loudly**:

1. **One named constant.** `YmckDecode::kPlaneIsInverted` is the only place
   that knows whether a channel byte is ink or light. Two flags in two files
   is how the branches drifted apart.
2. **Endpoint assertions, not sample comparisons.** `no ink => white`,
   `full ink => black`, `more K => darker`, `full C => (0,255,255)`. These are
   statements about the picture, so they stay true regardless of how the code
   is written. A comparison against a generated sample only proves the
   generator agrees with the decoder.
3. **One conversion site, enforced.** `InvertPlaneInPlace()` is called exactly
   once, and `check_ymck_polarity.py` fails the build if the subtraction shows
   up anywhere else, or if the call disappears or doubles.
4. **A switch that makes the fallback reachable.** A branch that never executes
   cannot be tested - F-M23 "fixed" one and verified nothing.
   `JPEGVIEW_DISABLE_CMYK_ICC=1` forces `CreateTransform()` to fail for CMYK so
   the no-profile paths really run. The switch only exists in debug builds
   (`#ifndef NDEBUG`); a Release build ignores the variable.

## Running it

No Windows toolchain needed - the header is plain C++17:

```sh
g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView tests/ymck_decode_test.cpp -o /tmp/ymck_test
/tmp/ymck_test
python3 .github/scripts/check_ymck_polarity.py
```

CI runs exactly these three commands plus a fourth step that proves the guard
can fail.

## Proving the guard has teeth

A test that has never failed is not evidence. Two historical regressions are
reproducible in one line each, and both must be rejected at compile time:

```sh
# 9746da5: the plane is never inverted
g++ -std=c++17 -DYMCK_PLANE_IS_INVERTED=0 -I src/JPEGView tests/ymck_decode_test.cpp -o /tmp/m1
#   -> error: static assertion failed: A1: a plane byte of 255 must be no ink...

# ec8032f: the naive formula inverts a second time (edit a copy of the header,
# replace (255u - inkA) * (255u - inkB) with inkA * inkB)
#   -> error: static assertion failed: A1 ... A2 ... A3 ... A4
```

The static gate has the same property: deleting the `InvertPlaneInPlace` call
reports `found 0`, duplicating it reports `found 2`, and re-inlining
`255 - p[0]` reports a per-channel inversion. All three were exercised before
this file was committed.

## Adding the next one

If you are about to rely on a convention that is not visible in the source
(a TIFF orientation flag, a JPEG restart interval, a device gamma, an EXIF
unit), do this instead of writing a comment:

- put the convention in one named constant with a comment naming the **proof**,
  not the belief ("renders correctly on build 19897de" is a proof; "PSD stores
  ink" is a belief);
- write four assertions that a wrong answer cannot satisfy, and check them
  against a real artefact produced by the original tool, never against your own
  generator;
- add a static gate if a second copy of the arithmetic would break it;
- add a switch if the code path is otherwise unreachable.
