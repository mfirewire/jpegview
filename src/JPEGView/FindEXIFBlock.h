/*  FindEXIFBlock.h - the EXIF APP1 walk, in a form the Linux CI can compile.
 *
 *  Moved verbatim out of Helpers.cpp (FindEXIFBlock) so
 *  tests/loader_jpeg_exif_test.cpp can exercise it without MFC. The only
 *  change is uint8 -> unsigned char: uint8 is defined as unsigned char in
 *  ImageProcessingTypes.h:7, so the two spellings are the same type and the
 *  behaviour is unchanged. Helpers.h keeps the declaration; Helpers.cpp
 *  includes this header instead of defining the function itself.
 *
 *  The walk scans EVERY header segment instead of stopping at the first
 *  APP1: a JPEG may legitimately carry an XMP APP1 before the Exif APP1
 *  (exiftool -o, some phones and editors write them in that order), and the
 *  old "first 0xE1 or nothing" rule returned NULL there, so the file lost
 *  its orientation while every other viewer showed it correctly. An APP1
 *  segment only counts as Exif when its length field is >= 6 and its first
 *  4 payload bytes are "Exif", so a too-short "Exif" segment is rejected.
 *
 *  Inclusion rule: this header is included by Helpers.cpp only. It defines
 *  a non-inline function, so including it from a second .cpp in the app
 *  build would be a duplicate definition at link time. The test TU is a
 *  separate binary, which is the only other includer.
 */

#pragma once

#include <cstddef>
#include <cstring>

namespace Helpers {

void* FindEXIFBlock(void* pJPEGStream, int nStreamLength) {
	unsigned char* pStream = (unsigned char*)pJPEGStream;
	if (pStream == NULL || nStreamLength < 4 || pStream[0] != 0xFF || pStream[1] != 0xD8) {
		return NULL; // not a JPEG
	}
	// Walk every header segment instead of stopping at the first APP1.
	// A JPEG may legitimately carry an XMP APP1 before the Exif APP1 (exiftool
	// -o, some phones and editors write them in that order). The old rule -
	// "first 0xE1 or nothing" - returned NULL there, so the file lost its
	// orientation, camera data and timestamp while every other viewer showed
	// it correctly.
	int nIndex = 2;
	while (nIndex + 3 < nStreamLength) {
		if (pStream[nIndex] != 0xFF) {
			break; // entropy-coded data: no more header segments
		}
		while (nIndex < nStreamLength && pStream[nIndex] == 0xFF) nIndex++;
		if (nIndex + 2 >= nStreamLength) break;
		const unsigned char nSegmentMarker = pStream[nIndex];
		if (nSegmentMarker == 0x00) break;                    // FF 00 is byte stuffing
		if (nSegmentMarker == 0xDA || nSegmentMarker == 0xD9) break; // SOS / EOI
		const int nBlockLength = pStream[nIndex + 1] * 256 + pStream[nIndex + 2];
		if (nBlockLength < 2 || nIndex + 1 + nBlockLength > nStreamLength) {
			break; // malformed: a length below 2 would not advance the index
		}
		// The segment starts at the 0xFF that precedes the marker byte, and the
		// caller expects exactly that pointer (length at +2, "Exif" at +4).
		if (nSegmentMarker == 0xE1 && nBlockLength >= 6 && nIndex >= 1 &&
			nIndex + 7 <= nStreamLength &&
			memcmp(pStream + nIndex + 3, "Exif", 4) == 0) {
			return pStream + (nIndex - 1);
		}
		nIndex += 1 + nBlockLength;
	}
	return NULL;
}

} // namespace Helpers
