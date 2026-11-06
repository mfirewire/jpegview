#pragma once

class PAMReader
{
public:
	// Decodes a binary PAM file (P7, .pam) from memory. Returns
	// 3-byte BGR pixels padded to a 4-byte boundary, like PBMWrapper,
	// or NULL on failure. Only the combinations proven by the sample
	// (sample_1920x1280.pam: DEPTH 3, MAXVAL 255, TUPLTYPE RGB) and
	// their direct sibling (DEPTH 4, MAXVAL 255, TUPLTYPE RGB_ALPHA,
	// alpha is dropped) are supported. 16-bit MAXVAL, DEPTH 1/2 and
	// any other TUPLTYPE are rejected cleanly (NULL, no OOM flag).
	// The dimensions are read from the header and checked before any
	// allocation.
	static void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		bool& outOfMemory, // set to true when no memory to read image
		bool& isSizeLimitExceeded, // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
		const void* buffer, // memory address containing PAM data.
		int sizebytes); // size of PAM data.
};
