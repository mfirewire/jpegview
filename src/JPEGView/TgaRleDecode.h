/*  TgaRleDecode.h - the TGA RLE packet decoder, in a form the Linux CI
 *  can compile (B-TEST1, option (a)).
 *
 *  Moved verbatim out of CReaderTGA::ReadTgaImage (ReaderTGA.cpp), with
 *  only the input adapted: the FILE* reads became reads through a
 *  TgaByteReader, and the Windows spellings (byte, uint8, uint32) became
 *  <cstdint> types. The product feeds it from the open FILE* (streaming,
 *  no extra allocation); the Linux test feeds it from a memory buffer.
 *  No packet semantics changed.
 *
 *  What this pins: the 15-bit regression (F-04). A 15-bit pixel is a
 *  two-byte 5-5-5 word, so channels must be (bits + 7) / 8, not bits / 8
 *  (== 1): with the old value every RLE packet consumed one byte too few
 *  and the stream drifted from the second pixel on. The run packet of
 *  tests/samples/tga15_rle_run.tga is exactly 3 bytes on disk (1 header
 *  + 2 pixel bytes), so a decoder consuming 1 byte per pixel cannot land
 *  on the right stream position; tests/loader_tga_rle_test.cpp asserts
 *  the reader is exactly exhausted, which pins that.
 *
 *  Short reads fail the decode (return false) instead of spinning on EOF
 *  with a stale rleID, exactly like the fread(...) != 1 checks this was
 *  extracted from (F-13). The caller frees the image buffer on false.
 *
 *  Inclusion rule: like PnmScale.h / FindEXIFBlock.h, this header is
 *  included by ReaderTGA.cpp and by tests/loader_tga_rle_test.cpp (a
 *  separate binary). The function is inline, so including it from a
 *  second .cpp in the app build would still be safe.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace TgaRle {

// Minimal byte source for the packet decoder. The product feeds it from
// the open FILE* (streaming); the Linux test feeds it from a memory
// buffer. A short read fails the decode.
struct ByteReader {
    virtual ~ByteReader() = default;
    virtual bool Read(void* dst, size_t n) = 0;
};

// ByteReader over a memory buffer; Read fails instead of returning
// partial data. bytesConsumed() is what the test asserts for exact
// stream consumption.
struct MemoryReader : public ByteReader {
    MemoryReader(const uint8_t* data, size_t size)
        : start(data), p(data), remaining(size) {}
    bool Read(void* dst, size_t n) override {
        if (n > remaining) return false;
        std::memcpy(dst, p, n);
        p += n;
        remaining -= n;
        return true;
    }
    size_t bytesConsumed() const { return static_cast<size_t>(p - start); }
    const uint8_t* start = nullptr;
    const uint8_t* p = nullptr;
    size_t remaining = 0;
};

// Decodes the RLE packet stream of one TGA image into pImageData (already
// allocated: targetStride bytes per row, targetChannels per pixel).
//   bits           bits per pixel (8, 15, 16, 24, 32; validated by caller)
//   isIndexed      indexed (palette) image
//   palette        768-byte palette (256 RGB entries); only read for
//                  8-bit indexed pixels, may be null otherwise
// Returns false on a truncated stream.
inline bool DecodeRlePackets(ByteReader& reader, int bits, bool isIndexed,
                             int width, int height, int targetChannels,
                             size_t targetStride, const uint8_t* palette,
                             uint8_t* pImageData) {
    // First, let me explain real quickly what RLE is.
    // For further information, check out Paul Bourke's intro article at:
    // http://astronomy.swin.edu.au/~pbourke/dataformats/rle/
    //
    // Anyway, we know that RLE is a basic type compression.  It takes
    // colors that are next to each other and then shrinks that info down
    // into the color and a integer that tells how much of that color is used.
    // For instance:
    // aaaaabbcccccccc would turn into a5b2c8
    // Well, that's fine and dandy and all, but how is it down with RGB colors?
    // Simple, you read in an color count (rleID), and if that number is less than 128,
    // it does NOT have any optimization for those colors, so we just read the next
    // pixels normally.  Say, the color count was 28, we read in 28 colors like normal.
    // If the color count is over 128, that means that the next color is optimized and
    // we want to read in the same pixel color for a count of (colorCount - 127).
    // It's 127 because we add 1 to the color count, as you'll notice in the code.

    // Create some variables to hold the rleID, current colors read, channels, & stride.
    uint8_t rleID = 0;
    // P0-5: size_t, not int. The buffer may be up to MAX_IMAGE_BYTES
    // (4 GB - 1 on x64), so a signed 32-bit accumulator overflows past
    // 2 GB and pImage[colorsRead + n] would write before the buffer.
    size_t colorsRead = 0;
    // Bytes actually stored per pixel. 15bpp is a two-byte 5-5-5 word, so
    // bits/8 (== 1) made every RLE packet consume one byte too few and the
    // stream drifted from the second pixel on: the image decoded to noise
    // instead of the solid colour the file holds.
    const int channels = (bits + 7) / 8;
    int x = 0;
    size_t padding = targetStride - (size_t)targetChannels * width; // P0-5: no int round-trip

    uint8_t colors[4];
    uint8_t* pColors = colors;
    uint8_t* pImage = pImageData;

    // Load in all the pixel data
    // width*height as int goes negative near the pixel limit, which made the RLE
    // loop read nothing and return an uninitialised buffer.
    size_t numPixels = (size_t)width * height;
    size_t i = 0;
    while (i < numPixels) {
        // Read in the current color count + 1
        // F-13: short read -> truncated file, fail cleanly (and do
        // not spin on EOF with a stale rleID).
        if (!reader.Read(&rleID, sizeof(uint8_t))) {
            return false;
        }

        // Check if we don't have an encoded string of colors
        bool useSameColor;
        if (rleID < 128) {
            // Increase the count by 1
            rleID++;
            useSameColor = false;
        }
        // Else, let's read in a string of the same character
        else {
            // Minus the 128 ID + 1 (127) to get the color count that needs to be read
            rleID -= 127;
            useSameColor = true;

            // Read in the current color, which is the same for a while
            // F-13: short read -> truncated file, fail cleanly.
            if (!reader.Read(pColors, sizeof(uint8_t) * channels)) {
                return false;
            }
        }

        // Go through and read all the unique colors found
        while (rleID && i < numPixels) {
            if (!useSameColor) {
                // Read in the current color
                // F-13: short read -> truncated file, fail cleanly.
                if (!reader.Read(pColors, sizeof(uint8_t) * channels)) {
                    return false;
                }
            }

            if (bits == 32) {
                *reinterpret_cast<uint32_t*>(pImage + colorsRead) =
                    *reinterpret_cast<const uint32_t*>(pColors);
            } else {
                if (bits == 8) {
                    if (isIndexed) {
                        int index = pColors[0] * 3;
                        pImage[colorsRead + 0] = palette[index];
                        pImage[colorsRead + 1] = palette[index + 1];
                        pImage[colorsRead + 2] = palette[index + 2];
                    } else {
                        pImage[colorsRead + 0] = pColors[0];
                        pImage[colorsRead + 1] = pColors[0];
                        pImage[colorsRead + 2] = pColors[0];
                    }
                } else if (bits == 16 || bits == 15) {
                    // 16/15-bit pixel is a 5-5-5 packed short; the RLE
                    // branch only read 2 bytes, so unpack it the same
                    // way as the non-RLE path (F-04).
                    uint16_t pixel = (uint16_t)(pColors[0] | (pColors[1] << 8));
                    pImage[colorsRead + 0] = (pixel & 0x1f) << 3;
                    pImage[colorsRead + 1] = ((pixel >> 5) & 0x1f) << 3;
                    pImage[colorsRead + 2] = ((pixel >> 10) & 0x1f) << 3;
                } else {
                    pImage[colorsRead + 0] = pColors[0];
                    pImage[colorsRead + 1] = pColors[1];
                    pImage[colorsRead + 2] = pColors[2];
                }

                // do padding target lines when 24 bpp
                x++;
                if (x == width) {
                    x = 0;
                    colorsRead += padding;
                }
            }

            // Increase the current pixels read, decrease the amount
            // of pixels left, and increase the starting index for the next pixel.
            i++;
            rleID--;
            colorsRead += targetChannels;
        }

    } // end of RLE pixel loop
    return true;
}

} // namespace TgaRle
