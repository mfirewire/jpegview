#pragma once

class EXRReader
{
public:
	// Decodes an OpenEXR file (.exr) from memory using tinyexr (vendored
	// under tinyexr/: tinyexr.h + exr_reader.hh + streamreader.hh + miniz
	// for ZIP; statically compiled in: no new DLL). Returns 3-byte BGR
	// pixels padded to a 4-byte boundary, like QOIWrapper, or NULL on
	// failure. The dimensions are read from the EXR header
	// (ParseEXRHeaderFromMemory) and checked before any decode, so a header
	// claiming absurd dimensions fails before tinyexr allocates for it
	// (F-06/F-07 lesson). Half and float channels are converted to 8-bit
	// with a gamma-2.2 tone map, the same choice as the HDR wrapper's
	// stb_image path, so scene values above 1.0 clip to white; there is no
	// exposure control in this pass. Alpha, when present, is composited
	// over white in linear space before the gamma map. DWAA/DWAB
	// compression is NOT supported by tinyexr and is rejected before
	// decoding (ZFP likewise, it is compiled out by default); B44/B44A is
	// supported by the vendored tinyexr snapshot
	// (it ships a B44 decompressor, wired into DecodePixelData).
	// Multi-part EXR files are rejected (the simple single-part path only).
	static void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		bool& outOfMemory, // set to true when no memory to read image
		const void* buffer, // memory address containing EXR data.
		int sizebytes); // size of EXR data.
};
