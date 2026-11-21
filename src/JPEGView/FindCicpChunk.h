/*  FindCicpChunk.h - the PNG cICP chunk scan, in a form the Linux CI can compile.
 *
 *  Moved out of PNGWrapper.cpp (FindCicpChunk) so
 *  tests/loader_png_cicp_test.cpp can exercise it without MFC. The only
 *  changes from the PNGWrapper version are (a) the chunk size is read by
 *  manual big-endian byte assembly instead of _byteswap_ulong (an MSVC
 *  intrinsic, unavailable on Linux), and (b) the 31-bit chunk-size cap is
 *  the literal 0x7FFFFFFF instead of PNG_UINT_31_MAX (from png.h). Both
 *  spellings compute the same value, so the behaviour is unchanged.
 *  PNGWrapper.cpp includes this header instead of defining the function
 *  itself.
 *
 *  A cICP chunk (ISO/IEC 23001-17) is 4 bytes: colour primaries, transfer
 *  characteristics, matrix coefficients, full-range flag. JPEGView only
 *  needs the first two (primaries + transfer) to decide whether a PNG is
 *  HDR (PQ/HLG) and which tone-mapping path to take.
 *
 *  The function is inline (S3), so any translation unit may include
 *  this header without a duplicate-definition link error. It is
 *  currently included by PNGWrapper.cpp and by
 *  tests/loader_png_cicp_test.cpp (a separate binary).
 */

#pragma once

#include <cstddef>
#include <cstring>

#include "HdrToneMap.h"

namespace PngCicp {

// Scan PNG chunks for a cICP chunk. `buffer` is the PNG data;
// `startOffset` is 8 for a buffer with the PNG signature (as
// MustUseLibpng receives) and 0 for the signature-stripped buffer
// BeginReading receives. Returns true and fills `cicp` when a
// well-formed cICP chunk is found before the first IDAT chunk; false
// when there is none, it is malformed, or the scan runs out of buffer.
// A cICP chunk is only 4 bytes of data, so a truncated or over-long
// one is ignored (treated as no cICP).
inline bool FindCicpChunk(const void* bufferIn, size_t sizebytes, size_t startOffset, HdrToneMap::Cicp& cicp)
{
	const unsigned char* buffer = (const unsigned char*)bufferIn;
	size_t offset = startOffset;
	while (offset + 7 < sizebytes) {
		// PNG chunk sizes are big-endian.
		unsigned int chunksize = ((unsigned int)buffer[offset] << 24)
			| ((unsigned int)buffer[offset + 1] << 16)
			| ((unsigned int)buffer[offset + 2] << 8)
			| (unsigned int)buffer[offset + 3];
		if (memcmp(buffer + offset + 4, "cICP", 4) == 0) {
			if (chunksize == 4 && offset + 8 + 4 <= sizebytes) {
				cicp.primaries = buffer[offset + 8];
				cicp.transfer = buffer[offset + 9];
				cicp.matrix = buffer[offset + 10];
				cicp.fullRange = buffer[offset + 11];
				return true;
			}
			return false;
		}
		if (memcmp(buffer + offset + 4, "IDAT", 4) == 0)
			return false;
		// PNG chunk sizes are 31-bit; a larger value cannot advance.
		if (chunksize > 0x7FFFFFFF) return false;
		// 12 = 4 bytes chunk size + 4 bytes chunk name + 4 bytes CRC32.
		offset += chunksize + 12;
	}
	return false;
}

}  // namespace PngCicp
