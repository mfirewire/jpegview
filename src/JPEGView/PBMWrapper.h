#pragma once

class PBMReader
{
public:
	// Decodes a binary PBM file (P4 bitmap, .pbm) from memory. Returns
	// 3-byte BGR pixels padded to a 4-byte boundary, like QOIWrapper,
	// or NULL on failure. P4 stores 1 bit per pixel (MSB first, rows
	// padded to a byte boundary); 1 is white, 0 is black. ASCII P1/P2/P3
	// and PAM P7 are not supported (stb_image rejects them too, and the
	// PPM gate rejects them). The dimensions are read from the header
	// and checked before any allocation.
	static void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		bool& outOfMemory, // set to true when no memory to read image
		bool& isSizeLimitExceeded, // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
		const void* buffer, // memory address containing PBM data.
		int sizebytes); // size of PBM data.
};
