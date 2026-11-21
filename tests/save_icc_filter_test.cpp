// save_icc_filter_test.cpp — unit test for the save-path ICC filter (N2).
//
// What this pins:
// ---------------
// * ClassifyIccProfile against the REAL profile fixtures in
//   tests/samples/: the genuine sRGB IEC 61966-2-1 profile (D65 wtpt,
//   D50 colourants) is sRGB; Adobe RGB 1998, Display P3 and ProPhoto
//   are not; a grey profile is unclassifiable. The D65-wtpt branch is
//   covered by the real sRGB fixture, the D50-wtpt branch by the same
//   fixture with wtpt patched to D50 (what lcms2-built profiles store).
// * The H-02-pattern trap: the stored colourants are D50 values, so a
//   rule comparing them against the D65-native values (0.4124, ...)
//   would reject the real sRGB profile on blue alone (distance 0.24
//   vs the 0.05 tolerance) and strip every real sRGB file. The fixture
//   test below IS the negative control for that wrong rule: if the
//   implementation ever compared against D65 colourants, the real
//   sRGB fixture would come back kNonSrgb and this test would fail.
// * The tolerance boundary (a +0.03 perturbation still sRGB, +0.06
//   not) and the all-three-primaries requirement (Adobe RGB's blue
//   alone is within tolerance of sRGB blue, so dropping any one
//   primary check would pass Adobe RGB).
// * Malformed profiles fail safe to kUnknown (keep): truncated,
//   tag table overrunning the bytes, missing/wrong-typed/out-of-range
//   tags, LUT-based RGB (no colourant tags), non-RGB colour space.
// * FilterNonRGBICCProfileSegments on synthetic segment blobs: an
//   Adobe RGB ICC is stripped when the gate is on and kept when it is
//   off; sRGB is kept either way; CMYK/grey are stripped regardless
//   of the gate (pre-existing F-H10 behaviour, unchanged); multi-
//   chunk profiles strip/keep as a whole from chunk 1's decision;
//   malformed chains and truncated chunk 1 keep the blob; the
//   nSegmentsBeforeEXIF accounting survives a strip on both sides of
//   the split; an all-ICC blob strips to nothing.
//
// What this does not verify: the SaveImage.cpp call-site wiring (the
// gate argument is CSettingsProvider::This().UseEmbeddedColorProfiles()
// there — verified by reading the call site, and the CI build proves
// it compiles), or any GUI behaviour. The device save-check (save a
// non-sRGB ICC JPEG, reload it, confirm the ICC is gone and the
// colours are right) remains the end-to-end supplement.
//
// Build and run (Linux, no Windows toolchain needed):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView
//       tests/save_icc_filter_test.cpp src/JPEGView/SaveIccFilter.cpp
//       -o /tmp/save_icc_filter_test
//   /tmp/save_icc_filter_test
// Run from the repository root: the fixture paths below are relative.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "SaveIccFilter.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("FAIL: %s\n", what);
    }
}

std::vector<unsigned char> ReadFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf((size_t)n);
    if (n > 0 && std::fread(buf.data(), 1, (size_t)n, f) != (size_t)n) buf.clear();
    std::fclose(f);
    return buf;
}

// --- ICC profile byte helpers (big-endian, ICC file layout) ---

void PutBe32(std::vector<unsigned char>& v, size_t off, uint32_t x) {
    v[off] = (unsigned char)((x >> 24) & 0xFF);
    v[off + 1] = (unsigned char)((x >> 16) & 0xFF);
    v[off + 2] = (unsigned char)((x >> 8) & 0xFF);
    v[off + 3] = (unsigned char)(x & 0xFF);
}

uint32_t GetBe32(const std::vector<unsigned char>& v, size_t off) {
    return ((uint32_t)v[off] << 24) | ((uint32_t)v[off + 1] << 16) |
           ((uint32_t)v[off + 2] << 8) | (uint32_t)v[off + 3];
}

void PutS15(std::vector<unsigned char>& v, size_t off, double x) {
    int32_t fixed = (int32_t)std::lround(x * 65536.0);
    PutBe32(v, off, (uint32_t)fixed);
}

// Find a tag-table entry: returns the entry offset, or -1 when absent.
long FindTagEntry(const std::vector<unsigned char>& p, const char* sig) {
    if (p.size() < 132) return -1;
    uint32_t nTags = GetBe32(p, 128);
    if (nTags > (p.size() - 132) / 12) return -1;
    for (uint32_t i = 0; i < nTags; i++) {
        size_t entry = 132 + 12 * (size_t)i;
        if (std::memcmp(p.data() + entry, sig, 4) == 0) return (long)entry;
    }
    return -1;
}

struct XyzTag {
    const char* sig;
    double x, y, z;
};

// Build a minimal matrix profile: 128-byte header + tag table + XYZ
// tag data laid out in order right after the table. `colorSpace` is
// the 4-byte signature at profile bytes 16..19.
std::vector<unsigned char> BuildProfile(const char* colorSpace, const std::vector<XyzTag>& tags) {
    size_t dataStart = 132 + 12 * tags.size();
    std::vector<unsigned char> p(dataStart + 20 * tags.size(), 0);
    PutBe32(p, 0, (uint32_t)p.size());
    std::memcpy(p.data() + 16, colorSpace, 4);
    std::memcpy(p.data() + 20, "XYZ ", 4);
    PutBe32(p, 128, (uint32_t)tags.size());
    for (size_t i = 0; i < tags.size(); i++) {
        size_t entry = 132 + 12 * i;
        std::memcpy(p.data() + entry, tags[i].sig, 4);
        uint32_t off = (uint32_t)(dataStart + 20 * i);
        PutBe32(p, entry + 4, off);
        PutBe32(p, entry + 8, 20);
        std::memcpy(p.data() + off, "XYZ ", 4);
        PutS15(p, off + 8, tags[i].x);
        PutS15(p, off + 12, tags[i].y);
        PutS15(p, off + 16, tags[i].z);
    }
    return p;
}

const XyzTag kSrgbWtptD65 = { "wtpt", 0.9505, 1.0, 1.0890 };
const XyzTag kSrgbRed = { "rXYZ", 0.4361, 0.2225, 0.0139 };
const XyzTag kSrgbGreen = { "gXYZ", 0.3851, 0.7169, 0.0971 };
const XyzTag kSrgbBlue = { "bXYZ", 0.1431, 0.0606, 0.7141 };
const XyzTag kAdobeRed = { "rXYZ", 0.6097, 0.3111, 0.0195 };
const XyzTag kAdobeGreen = { "gXYZ", 0.2053, 0.6257, 0.0609 };
const XyzTag kAdobeBlue = { "bXYZ", 0.1492, 0.0632, 0.7446 };

std::vector<unsigned char> BuildSrgbProfile() {
    return BuildProfile("RGB ", { kSrgbWtptD65, kSrgbRed, kSrgbGreen, kSrgbBlue });
}

std::vector<unsigned char> BuildAdobeProfile() {
    return BuildProfile("RGB ", { kSrgbWtptD65, kAdobeRed, kAdobeGreen, kAdobeBlue });
}

void TestClassifyRealFixtures() {
    std::vector<unsigned char> srgb = ReadFile("tests/samples/srgb_iec61966-2-1.icc");
    Check(!srgb.empty(), "sRGB fixture loads");
    // The trap: this real profile stores D65 wtpt with D50 colourants.
    // A rule comparing colourants against D65-native values would
    // reject it on blue alone (0.24 vs the 0.05 tolerance).
    Check(ClassifyIccProfile(srgb.data(), srgb.size()) == IccSrgbVerdict::kSrgb,
          "real sRGB profile (D65 wtpt, D50 colourants) is sRGB");
    std::vector<unsigned char> adobe = ReadFile("tests/samples/adobe_rgb_1998.icc");
    Check(!adobe.empty(), "Adobe RGB fixture loads");
    Check(ClassifyIccProfile(adobe.data(), adobe.size()) == IccSrgbVerdict::kNonSrgb,
          "real Adobe RGB 1998 profile is non-sRGB");
    std::vector<unsigned char> p3 = ReadFile("tests/samples/display_p3.icc");
    Check(!p3.empty(), "Display P3 fixture loads");
    Check(ClassifyIccProfile(p3.data(), p3.size()) == IccSrgbVerdict::kNonSrgb,
          "real Display P3 profile (D50 wtpt) is non-sRGB");
    std::vector<unsigned char> prophoto = ReadFile("tests/samples/prophoto_rgb.icc");
    Check(!prophoto.empty(), "ProPhoto fixture loads");
    Check(ClassifyIccProfile(prophoto.data(), prophoto.size()) == IccSrgbVerdict::kNonSrgb,
          "real ProPhoto profile is non-sRGB");
    std::vector<unsigned char> gray = ReadFile("tests/samples/adobe_rgb_gray.icc");
    Check(!gray.empty(), "grey fixture loads");
    Check(ClassifyIccProfile(gray.data(), gray.size()) == IccSrgbVerdict::kUnknown,
          "grey profile (no colourants) is unclassifiable, kept");
}

void TestClassifyWtptBranches() {
    // D50-wtpt branch: the same real sRGB bytes with wtpt re-stored
    // as D50 (0.9642, 1.0, 0.8249) — what lcms2-built profiles carry.
    std::vector<unsigned char> srgb = ReadFile("tests/samples/srgb_iec61966-2-1.icc");
    long entry = FindTagEntry(srgb, "wtpt");
    Check(entry >= 0, "wtpt entry found in sRGB fixture");
    size_t dataOff = GetBe32(srgb, (size_t)entry + 4);
    PutS15(srgb, dataOff + 8, 0.9642);
    PutS15(srgb, dataOff + 12, 1.0);
    PutS15(srgb, dataOff + 16, 0.8249);
    Check(ClassifyIccProfile(srgb.data(), srgb.size()) == IccSrgbVerdict::kSrgb,
          "sRGB profile with D50 wtpt is sRGB");
    // A white point that is neither D65 nor D50 is not sRGB, even
    // with sRGB colourants.
    std::vector<unsigned char> srgb2 = ReadFile("tests/samples/srgb_iec61966-2-1.icc");
    entry = FindTagEntry(srgb2, "wtpt");
    dataOff = GetBe32(srgb2, (size_t)entry + 4);
    PutS15(srgb2, dataOff + 8, 0.5);
    PutS15(srgb2, dataOff + 12, 1.0);
    PutS15(srgb2, dataOff + 16, 0.5);
    Check(ClassifyIccProfile(srgb2.data(), srgb2.size()) == IccSrgbVerdict::kNonSrgb,
          "sRGB colourants with an unrelated white point are non-sRGB");
}

void TestClassifyToleranceAndPrimaries() {
    // Tolerance boundary on the real fixture: +0.03 on blue Z stays
    // sRGB, +0.06 does not.
    std::vector<unsigned char> srgb = ReadFile("tests/samples/srgb_iec61966-2-1.icc");
    long entry = FindTagEntry(srgb, "bXYZ");
    size_t dataOff = GetBe32(srgb, (size_t)entry + 4);
    PutS15(srgb, dataOff + 16, 0.7141 + 0.03);
    Check(ClassifyIccProfile(srgb.data(), srgb.size()) == IccSrgbVerdict::kSrgb,
          "blue Z +0.03 stays sRGB");
    PutS15(srgb, dataOff + 16, 0.7141 + 0.06);
    Check(ClassifyIccProfile(srgb.data(), srgb.size()) == IccSrgbVerdict::kNonSrgb,
          "blue Z +0.06 is non-sRGB");
    // One primary out is enough: red X +0.06.
    std::vector<unsigned char> srgb2 = ReadFile("tests/samples/srgb_iec61966-2-1.icc");
    entry = FindTagEntry(srgb2, "rXYZ");
    dataOff = GetBe32(srgb2, (size_t)entry + 4);
    PutS15(srgb2, dataOff + 8, 0.4361 + 0.06);
    Check(ClassifyIccProfile(srgb2.data(), srgb2.size()) == IccSrgbVerdict::kNonSrgb,
          "red X +0.06 is non-sRGB");
    // Synthetic minimal profiles, independent of the fixtures.
    std::vector<unsigned char> synthSrgb = BuildSrgbProfile();
    Check(ClassifyIccProfile(synthSrgb.data(), synthSrgb.size()) == IccSrgbVerdict::kSrgb,
          "synthetic sRGB-valued profile is sRGB");
    std::vector<unsigned char> adobe = BuildAdobeProfile();
    Check(ClassifyIccProfile(adobe.data(), adobe.size()) == IccSrgbVerdict::kNonSrgb,
          "synthetic Adobe-valued profile is non-sRGB");
    // All three primaries are required: Adobe blue alone is within
    // tolerance of sRGB blue (0.031), so a profile with Adobe red and
    // green but sRGB blue must still be non-sRGB — it is, on red/green.
    // (The converse — a profile differing only in blue — is covered
    // by the +0.06 blue-Z case above.)
}

void TestClassifyMalformed() {
    Check(ClassifyIccProfile(NULL, 0) == IccSrgbVerdict::kUnknown, "NULL profile is unknown");
    std::vector<unsigned char> empty;
    Check(ClassifyIccProfile(empty.data(), empty.size()) == IccSrgbVerdict::kUnknown,
          "empty profile is unknown");
    std::vector<unsigned char> srgb = ReadFile("tests/samples/srgb_iec61966-2-1.icc");
    std::vector<unsigned char> shortProfile(srgb.begin(), srgb.begin() + 100);
    Check(ClassifyIccProfile(shortProfile.data(), shortProfile.size()) == IccSrgbVerdict::kUnknown,
          "100-byte truncated profile is unknown");
    std::vector<unsigned char> badCount = srgb;
    PutBe32(badCount, 128, 1000);  // tag table cannot fit
    Check(ClassifyIccProfile(badCount.data(), badCount.size()) == IccSrgbVerdict::kUnknown,
          "tag count overrunning the bytes is unknown");
    std::vector<unsigned char> noRed = srgb;
    long entry = FindTagEntry(noRed, "rXYZ");
    std::memcpy(noRed.data() + entry, "XXXX", 4);
    Check(ClassifyIccProfile(noRed.data(), noRed.size()) == IccSrgbVerdict::kUnknown,
          "missing rXYZ is unknown");
    std::vector<unsigned char> noWhite = srgb;
    entry = FindTagEntry(noWhite, "wtpt");
    std::memcpy(noWhite.data() + entry, "XXXX", 4);
    Check(ClassifyIccProfile(noWhite.data(), noWhite.size()) == IccSrgbVerdict::kUnknown,
          "missing wtpt is unknown");
    std::vector<unsigned char> wrongType = srgb;
    entry = FindTagEntry(wrongType, "rXYZ");
    size_t dataOff = GetBe32(wrongType, (size_t)entry + 4);
    std::memcpy(wrongType.data() + dataOff, "curv", 4);
    Check(ClassifyIccProfile(wrongType.data(), wrongType.size()) == IccSrgbVerdict::kUnknown,
          "rXYZ with a non-XYZ type is unknown");
    std::vector<unsigned char> badOffset = srgb;
    entry = FindTagEntry(badOffset, "rXYZ");
    PutBe32(badOffset, (size_t)entry + 4, 99999);
    Check(ClassifyIccProfile(badOffset.data(), badOffset.size()) == IccSrgbVerdict::kUnknown,
          "rXYZ data offset past the end is unknown");
    // LUT-based RGB: white point only, no colourant tags — cannot
    // prove sRGB, so it is kept (residual mis-tag risk, documented).
    std::vector<unsigned char> lut = BuildProfile("RGB ", { kSrgbWtptD65 });
    Check(ClassifyIccProfile(lut.data(), lut.size()) == IccSrgbVerdict::kUnknown,
          "LUT-based RGB profile (wtpt only) is unknown, kept");
    // Non-RGB colour space is the caller's strip decision, not this
    // classifier's.
    std::vector<unsigned char> cmyk = BuildProfile("CMYK", { kSrgbWtptD65, kSrgbRed, kSrgbGreen, kSrgbBlue });
    Check(ClassifyIccProfile(cmyk.data(), cmyk.size()) == IccSrgbVerdict::kUnknown,
          "CMYK colour space is unknown to the classifier");
}

// --- JPEG segment blobs ---

std::vector<unsigned char> AppSegment(unsigned char marker, const std::vector<unsigned char>& payload) {
    size_t len = 2 + payload.size();
    std::vector<unsigned char> s = { 0xFF, marker,
        (unsigned char)((len >> 8) & 0xFF), (unsigned char)(len & 0xFF) };
    s.insert(s.end(), payload.begin(), payload.end());
    return s;
}

// One ICC_PROFILE APP2 chunk holding profile[begin..end).
std::vector<unsigned char> IccChunk(const std::vector<unsigned char>& profile, size_t begin, size_t end,
                                    int chunkNo, int chunkCount) {
    std::vector<unsigned char> payload = { 'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0,
        (unsigned char)chunkNo, (unsigned char)chunkCount };
    payload.insert(payload.end(), profile.begin() + (long)begin, profile.begin() + (long)end);
    return AppSegment(0xE2, payload);
}

std::vector<unsigned char> Join(const std::vector<std::vector<unsigned char>>& parts) {
    std::vector<unsigned char> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

bool BlobHasIcc(const std::vector<unsigned char>& blob) {
    size_t i = 0;
    while (i + 4 <= blob.size()) {
        if (blob[i] != 0xFF) return false;
        int len = blob[i + 2] * 256 + blob[i + 3];
        if (len < 2 || i + 2 + (size_t)len > blob.size()) return false;
        if (blob[i + 1] == 0xE2 && len >= 16 &&
            std::memcmp(blob.data() + i + 4, "ICC_PROFILE\0", 12) == 0) {
            return true;
        }
        i += 2 + (size_t)len;
    }
    return false;
}

struct FilterResult {
    bool stripped = false;
    std::vector<unsigned char> out;
    int outLen = 0;
    int outBefore = 0;
};

FilterResult RunFilter(const std::vector<unsigned char>& blob, int beforeExif, bool stripNonSrgb) {
    unsigned char* pFiltered = NULL;
    int nLen = 0, nBefore = 0;
    bool ok = FilterNonRGBICCProfileSegments(blob.data(), (int)blob.size(), beforeExif,
                                             &pFiltered, nLen, nBefore, stripNonSrgb);
    FilterResult r;
    r.stripped = ok;
    r.outLen = nLen;
    r.outBefore = nBefore;
    if (ok && pFiltered != NULL) {
        r.out.assign(pFiltered, pFiltered + nLen);
        delete[] pFiltered;
    }
    return r;
}

void TestFilterGateAndProfiles() {
    std::vector<unsigned char> adobe = ReadFile("tests/samples/adobe_rgb_1998.icc");
    std::vector<unsigned char> srgb = ReadFile("tests/samples/srgb_iec61966-2-1.icc");
    std::vector<unsigned char> app0 = AppSegment(0xE0, { 'J', 'F', 'I', 'F', 0, 1, 1, 0 });
    std::vector<unsigned char> com = AppSegment(0xFE, { 'h', 'i' });

    // Adobe RGB, gate on: stripped, neighbours preserved byte-exact.
    std::vector<unsigned char> blob = Join({ app0, IccChunk(adobe, 0, adobe.size(), 1, 1), com });
    FilterResult r = RunFilter(blob, (int)app0.size(), true);
    Check(r.stripped, "Adobe RGB ICC stripped with gate on");
    Check(!BlobHasIcc(r.out), "stripped blob has no ICC chunk left");
    Check(r.out == Join({ app0, com }), "stripped blob is APP0 + COM byte-exact");
    Check(r.outLen == (int)r.out.size(), "stripped length matches");
    Check(r.outBefore == (int)app0.size(), "BeforeEXIF kept when ICC was after the split");
    // Adobe RGB, gate off: kept — the pixels were not converted, so
    // the source profile still describes them.
    r = RunFilter(blob, (int)app0.size(), false);
    Check(!r.stripped, "Adobe RGB ICC kept with gate off");
    // sRGB is kept either way.
    std::vector<unsigned char> blobSrgb = Join({ app0, IccChunk(srgb, 0, srgb.size(), 1, 1), com });
    Check(!RunFilter(blobSrgb, (int)app0.size(), true).stripped, "sRGB ICC kept with gate on");
    Check(!RunFilter(blobSrgb, (int)app0.size(), false).stripped, "sRGB ICC kept with gate off");
    // No ICC at all: untouched.
    std::vector<unsigned char> blobPlain = Join({ app0, com });
    Check(!RunFilter(blobPlain, (int)app0.size(), true).stripped, "no-ICC blob untouched");
    // CMYK / grey are stripped regardless of the gate (F-H10, unchanged).
    std::vector<unsigned char> cmyk = BuildProfile("CMYK", { kSrgbWtptD65, kSrgbRed, kSrgbGreen, kSrgbBlue });
    std::vector<unsigned char> blobCmyk = Join({ app0, IccChunk(cmyk, 0, cmyk.size(), 1, 1), com });
    Check(RunFilter(blobCmyk, (int)app0.size(), true).stripped, "CMYK ICC stripped with gate on");
    Check(RunFilter(blobCmyk, (int)app0.size(), false).stripped, "CMYK ICC stripped with gate off");
    std::vector<unsigned char> gray = ReadFile("tests/samples/adobe_rgb_gray.icc");
    std::vector<unsigned char> blobGray = Join({ app0, IccChunk(gray, 0, gray.size(), 1, 1), com });
    Check(RunFilter(blobGray, (int)app0.size(), true).stripped, "grey ICC stripped with gate on");
    Check(RunFilter(blobGray, (int)app0.size(), false).stripped, "grey ICC stripped with gate off");
    // LUT-based RGB is kept even with the gate on (residual risk).
    std::vector<unsigned char> lut = BuildProfile("RGB ", { kSrgbWtptD65 });
    std::vector<unsigned char> blobLut = Join({ app0, IccChunk(lut, 0, lut.size(), 1, 1), com });
    Check(!RunFilter(blobLut, (int)app0.size(), true).stripped, "LUT-based RGB kept with gate on");
}

void TestFilterMultiChunkAndMalformed() {
    std::vector<unsigned char> app0 = AppSegment(0xE0, { 'J', 'F', 'I', 'F', 0, 1, 1, 0 });
    std::vector<unsigned char> com = AppSegment(0xFE, { 'h', 'i' });
    // Synthetic Adobe-valued profile with 140 trailing bytes (total
    // 400): all four XYZ tags end at 260, so chunk 1 (first 300
    // bytes) classifies it and both chunks are dropped.
    std::vector<unsigned char> adobe = BuildAdobeProfile();
    adobe.resize(400, 0);
    std::vector<unsigned char> blob = Join({ app0, IccChunk(adobe, 0, 300, 1, 2),
                                             IccChunk(adobe, 300, 400, 2, 2), com });
    FilterResult r = RunFilter(blob, (int)app0.size(), true);
    Check(r.stripped, "multi-chunk Adobe stripped from chunk 1's decision");
    Check(!BlobHasIcc(r.out), "multi-chunk strip drops every ICC chunk");
    Check(r.out == Join({ app0, com }), "multi-chunk strip keeps the rest byte-exact");
    // Same profile, chunk 1 cut to 200 bytes: the rXYZ data (at
    // 200..220) does not fit, so the profile is unclassifiable and
    // the blob is kept.
    std::vector<unsigned char> blobShort = Join({ app0, IccChunk(adobe, 0, 200, 1, 2),
                                                  IccChunk(adobe, 200, 400, 2, 2), com });
    Check(!RunFilter(blobShort, (int)app0.size(), true).stripped,
          "chunk 1 too short for the colourant tags keeps the blob");
    // Multi-chunk sRGB is kept as a whole.
    std::vector<unsigned char> srgb = ReadFile("tests/samples/srgb_iec61966-2-1.icc");
    std::vector<unsigned char> blobSrgb = Join({ app0, IccChunk(srgb, 0, 2000, 1, 2),
                                                 IccChunk(srgb, 2000, srgb.size(), 2, 2), com });
    Check(!RunFilter(blobSrgb, (int)app0.size(), true).stripped, "multi-chunk sRGB kept");
    // Malformed chain: a segment length overrunning the blob.
    std::vector<unsigned char> overrun = { 0xFF, 0xE0, 0x00, 0x64, 'J', 'F', 'I', 'F', 0 };
    Check(!RunFilter(overrun, 0, true).stripped, "length-overrunning chain keeps the blob");
    // Truncated chunk 1: ICC header present but under 36 bytes.
    std::vector<unsigned char> tiny = IccChunk(adobe, 0, 4, 1, 1);
    Check(!RunFilter(Join({ tiny, com }), 0, true).stripped, "truncated chunk 1 keeps the blob");
    // Segment not starting with FF.
    std::vector<unsigned char> noff = Join({ app0, com });
    noff[0] = 0x00;
    Check(!RunFilter(noff, 0, true).stripped, "chain not starting with FF keeps the blob");
    // Empty / tiny input.
    std::vector<unsigned char> empty;
    Check(!RunFilter(empty, 0, true).stripped, "empty blob keeps");
}

void TestFilterBeforeExifAndAllIcc() {
    std::vector<unsigned char> adobe = ReadFile("tests/samples/adobe_rgb_1998.icc");
    std::vector<unsigned char> app0 = AppSegment(0xE0, { 'J', 'F', 'I', 'F', 0, 1, 1, 0 });
    std::vector<unsigned char> com = AppSegment(0xFE, { 'h', 'i' });
    std::vector<unsigned char> icc = IccChunk(adobe, 0, adobe.size(), 1, 1);
    // ICC after the EXIF split: BeforeEXIF is unchanged by the strip.
    FilterResult r = RunFilter(Join({ app0, icc, com }), (int)app0.size(), true);
    Check(r.stripped && r.outBefore == (int)app0.size(), "BeforeEXIF kept when ICC was after the split");
    // ICC before the EXIF split: the kept segment started at the split
    // offset, so BeforeEXIF drops to zero (strict < in pass 3).
    r = RunFilter(Join({ icc, app0, com }), (int)icc.size(), true);
    Check(r.stripped && r.out == Join({ app0, com }), "ICC before the split strips to APP0 + COM");
    Check(r.outBefore == 0, "BeforeEXIF is zero when the stripped chunk was the split point");
    // All segments ICC: strips to nothing, filtered buffer is NULL.
    r = RunFilter(icc, 0, true);
    Check(r.stripped && r.outLen == 0 && r.outBefore == 0 && r.out.empty(),
          "all-ICC blob strips to nothing");
}

}  // namespace

int main() {
    TestClassifyRealFixtures();
    TestClassifyWtptBranches();
    TestClassifyToleranceAndPrimaries();
    TestClassifyMalformed();
    TestFilterGateAndProfiles();
    TestFilterMultiChunkAndMalformed();
    TestFilterBeforeExifAndAllIcc();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
