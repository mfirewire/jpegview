#pragma once

class PPMReader
{
public:
	// Decodes a binary PNM file (P6 PPM / P5 PGM content, .ppm
	// routing only) from memory. Returns 3-byte BGR pixels padded to
	// a 4-byte boundary, like QOIWrapper, or NULL on failure. 16-bit
	// PNM input is shown as 8-bit (stb_image's stbi_load path, with
	// the raster byte-swapped to spec big-endian first, see
	// PPMWrapper.cpp); ASCII P2/P3, bitmap P1/P4 and PAM P7 are not
	// supported by stb_image and are rejected by the magic check.
	static void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		bool& outOfMemory, // set to true when no memory to read image
		const void* buffer, // memory address containing PNM data.
		int sizebytes); // size of PNM data.
};
