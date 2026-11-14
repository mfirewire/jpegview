/*  loader_jpeg_exif_test.cpp - regression cases for FindEXIFBlock.
 *
 *  Two regressions hid in the EXIF APP1 walk (Helpers.cpp, now
 *  src/JPEGView/FindEXIFBlock.h, plain C++17, no MFC):
 *
 *    (1) A JPEG may carry an XMP APP1 before the Exif APP1. The old rule -
 *        "first 0xE1 or nothing" - returned NULL there, so the file lost
 *        its orientation, camera data and timestamp while every other
 *        viewer showed it correctly. The walk must scan every header
 *        segment and return the Exif one wherever it stands.
 *    (2) A too-short "Exif" APP1 (length field < 6) must be rejected, not
 *        treated as an Exif block.
 *
 *  The caller contract being pinned: the returned pointer sits on the 0xFF
 *  that starts the Exif APP1 segment, so the length is at +2 and "Exif"
 *  is at +4 (see Helpers.cpp, FindPreservedJPEGSegments).
 *
 *  Samples (all hand-built minimal JPEG marker chains in tests/samples/,
 *  SOI + APPn segments + EOI, no scan data):
 *    jpeg_exif_first.jpg      Exif APP1 first; expect offset 2
 *    jpeg_xmp_then_exif.jpg   XMP APP1 first, Exif second; expect offset 35
 *    jpeg_short_exif.jpg      APP1 length 5, payload "Exi"; expect NULL
 *    jpeg_truncated_exif.jpg  APP1 length 20 but the file ends after
 *                             "Exif\0\0"; expect NULL
 *    jpeg_no_exif.jpg         JFIF APP0 + XMP APP1 only; expect NULL
 *
 *  Build & run (from the repository root, Linux; no Windows/MFC needed):
 *      g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView tests/loader_jpeg_exif_test.cpp -o /tmp/loader_jpeg_exif_test
 *      /tmp/loader_jpeg_exif_test [tests/samples]
 *  Exit code 0 = all checks passed, 1 = at least one failed.
 */

#include "FindEXIFBlock.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

bool ReadFile(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t read = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return read == out.size();
}

void CheckFound(const std::string& path, long wantOffset) {
    std::vector<unsigned char> buf;
    char what[256];
    std::snprintf(what, sizeof(what), "read %s", path.c_str());
    if (!ReadFile(path, buf)) {
        Check(false, what);
        return;
    }
    const unsigned char* found = static_cast<const unsigned char*>(
        Helpers::FindEXIFBlock(buf.data(), static_cast<int>(buf.size())));
    std::snprintf(what, sizeof(what), "%s: Exif APP1 found at offset %ld",
                  path.c_str(), wantOffset);
    Check(found != nullptr && found - buf.data() == wantOffset, what);
    if (found != nullptr) {
        // Caller contract: 0xFF 0xE1 at the pointer, length at +2, "Exif" at +4.
        const bool layout = (found[0] == 0xFF && found[1] == 0xE1 &&
                             std::memcmp(found + 4, "Exif", 4) == 0 &&
                             (found[2] * 256 + found[3]) == 20);
        std::snprintf(what, sizeof(what), "%s: returned pointer layout (FF E1, len 20, Exif)",
                      path.c_str());
        Check(layout, what);
    }
}

void CheckNull(const std::string& path) {
    std::vector<unsigned char> buf;
    char what[256];
    std::snprintf(what, sizeof(what), "read %s", path.c_str());
    if (!ReadFile(path, buf)) {
        Check(false, what);
        return;
    }
    const void* found = Helpers::FindEXIFBlock(buf.data(), static_cast<int>(buf.size()));
    std::snprintf(what, sizeof(what), "%s: no Exif block -> NULL", path.c_str());
    Check(found == nullptr, what);
}

void TestInMemory() {
    // Not a JPEG at all.
    const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Check(Helpers::FindEXIFBlock(const_cast<unsigned char*>(png), 8) == nullptr,
          "non-JPEG stream -> NULL");
    // SOI only: too short for any segment.
    const unsigned char soi[] = {0xFF, 0xD8};
    Check(Helpers::FindEXIFBlock(const_cast<unsigned char*>(soi), 2) == nullptr,
          "SOI-only stream -> NULL");
    Check(Helpers::FindEXIFBlock(nullptr, 0) == nullptr, "null stream -> NULL");

    // Exif APP1 after a JFIF APP0 (not just after XMP): SOI + APP0 (16) +
    // Exif APP1. The Exif segment starts at offset 2 + 2 + 16 = 20.
    std::vector<unsigned char> buf = {
        0xFF, 0xD8,                         // SOI
        0xFF, 0xE0, 0x00, 0x10,             // APP0, length 16
        'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00, // 14 payload bytes
        0xFF, 0xE1, 0x00, 0x14,             // APP1 Exif, length 20
        'E', 'x', 'i', 'f', 0x00, 0x00,     // "Exif\0\0"
        'I', 'I', '*', 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,             // 8 more payload bytes
        0xFF, 0xD9};                        // EOI
    const unsigned char* found = static_cast<const unsigned char*>(
        Helpers::FindEXIFBlock(buf.data(), static_cast<int>(buf.size())));
    Check(found != nullptr && found - buf.data() == 20,
          "in-memory: Exif APP1 after JFIF APP0 found at offset 20");
}

void TestSamples(const std::string& dir) {
    CheckFound(dir + "/jpeg_exif_first.jpg", 2);
    CheckFound(dir + "/jpeg_xmp_then_exif.jpg", 35);
    CheckNull(dir + "/jpeg_short_exif.jpg");
    CheckNull(dir + "/jpeg_truncated_exif.jpg");
    CheckNull(dir + "/jpeg_no_exif.jpg");
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/samples";
    std::printf("loader_jpeg_exif_test (samples: %s)\n", dir.c_str());
    TestInMemory();
    TestSamples(dir);

    if (g_failures == 0) {
        std::printf("PASS  %d checks\n", g_checks);
        return 0;
    }
    std::printf("FAILED  %d of %d checks\n", g_failures, g_checks);
    return 1;
}
