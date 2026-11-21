#pragma once

class HDRReader
{
public:
	// Decodes a Radiance HDR file (.hdr/.rgbe, "#?" magic) from memory.
	// Returns 3-byte BGR pixels padded to a 4-byte boundary, like QOIWrapper,
	// or NULL on failure. HDR is floating-point: floats are loaded with
	// stbi_loadf_from_memory, linearized with the file's GAMMA header,
	// and tone-mapped by HdrToneMap::ConvertLinearImage (BT.2390 roll-off,
	// linear 1.0 = SDR white), so highlights roll off instead of clipping.
	// The dimensions are read with stbi_info_from_memory and checked
	// before any decode, so a header claiming absurd dimensions fails
	// before stb_image allocates for it (F-06/F-07 lesson).
	static void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		bool& outOfMemory, // set to true when no memory to read image
		bool& isSizeLimitExceeded, // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
		const void* buffer, // memory address containing HDR data.
		int sizebytes); // size of HDR data.
};
