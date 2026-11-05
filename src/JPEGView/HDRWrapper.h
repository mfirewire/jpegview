#pragma once

class HDRReader
{
public:
	// Decodes a Radiance HDR file (.hdr/.rgbe, "#?" magic) from memory.
	// Returns 3-byte BGR pixels padded to a 4-byte boundary, like QOIWrapper,
	// or NULL on failure. HDR is floating-point; stb_image's 8-bit load
	// path converts it to 8-bit with its default gamma-2.2 tone mapping,
	// so bright highlights are compressed (documented, not worked around).
	// The dimensions are read with stbi_info_from_memory and checked
	// before any decode, so a header claiming absurd dimensions fails
	// before stb_image allocates for it (F-06/F-07 lesson).
	static void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		bool& outOfMemory, // set to true when no memory to read image
		const void* buffer, // memory address containing HDR data.
		int sizebytes); // size of HDR data.
};
