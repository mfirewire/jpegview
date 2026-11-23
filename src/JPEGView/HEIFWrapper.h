#pragma once

#include "libheif/heif_cxx.h"

// Gain-map retention (HDR toggle): the extracted gain map travels via
// an out-parameter on ReadImage; forward declaration avoids pulling
// GainMap.h into this header.
namespace gainmap { struct ExtractedGainMap; }

class HeifReader
{
public:
	// Returns data in the form 4-byte BGRA
	static void * ReadImage(int &width,   // width of the image loaded.
						 int &height,  // height of the image loaded.
						 int &bpp,     // BYTES (not bits) PER PIXEL.
						 int &frame_count, // number of top-level images
					     void* &exif_chunk, // Pointer to Exif data (must be freed by caller)
						 bool &outOfMemory, // set to true when no memory to read image
						 bool &isSizeLimitExceeded, // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
						 int frame_index, // index of requested frame
						 const void *buffer, // memory address containing heic compressed data.
						 int sizebytes, // size of heic compressed data.
						 gainmap::ExtractedGainMap* pGainOut = NULL); // optional: receives the extracted (not applied) gain map; NULL skips extraction.
};
