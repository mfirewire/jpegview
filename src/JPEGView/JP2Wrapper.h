#pragma once

class JP2Reader
{
public:
	// Decodes a JPEG 2000 file (.jp2/.j2k/.j2c) from memory using OpenJPEG
	// 2.5 (openjp2.dll, delay-loaded: no cost when JP2 is never opened).
	// Returns 3-byte BGR pixels padded to a 4-byte boundary, like QOIWrapper,
	// or NULL on failure. The dimensions are read from the codestream header
	// (opj_read_header) and checked before any decode, so a header claiming
	// absurd dimensions fails before OpenJPEG allocates for it (F-06/F-07
	// lesson). 8-bit and 16-bit (and lower-precision) components are
	// supported, signed or unsigned; 1 (gray), 2 (gray+alpha, alpha dropped),
	// 3 (RGB) and 4 (RGBA, alpha dropped) components are supported.
	// YCbCr (SYCC/EYCC) codestreams are converted to RGB with BT.601;
	// CMYK images are rejected (clean failure, not wrong colors); palette
	// images are left to OpenJPEG's default PCLR/CMAP/CDEF handling,
	// unverified - see JP2Wrapper.cpp (R-21).
	// Subsampled components (dx/dy > 1) are upsampled by nearest neighbour.
	static void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		bool& outOfMemory, // set to true when no memory to read image
		bool& isSizeLimitExceeded, // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
		const void* buffer, // memory address containing JP2 data.
		int sizebytes); // size of JP2 data.
};
