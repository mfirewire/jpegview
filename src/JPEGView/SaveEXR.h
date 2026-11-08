#pragma once

// Saves a processed image as OpenEXR (.exr) using the vendored tinyexr
// (tinyexr/tinyexr.h; implementation lives in EXRWrapper.cpp, the decode
// side -- do NOT define TINYEXR_IMPLEMENTATION here).
// The input is the same 24-bit BGR DIB that SaveWebP/SaveJXL/SaveQOI
// receive: 3 bytes per pixel, rows padded to a 4-byte boundary, row 0
// first (top-down, the JPEGView DIB convention; Convert32bppTo24bppDIB
// with bFlip=false copies row j to row j, and the WebP/JXL/QOI
// Compress implementations all consume row 0 first with no flip).
// The pipeline is 8-bit throughout, so the EXR carries no real dynamic
// range: each 8-bit value v is stored as the fp16 nearest to its
// linear-light value (v/255)^2.2 in [0, 1]. The stored values are
// scene-referred linear light (the writer linearises the 8-bit sRGB
// input with Helpers::LinearFromSRGB, as SaveHDR does), and the decode
// side (EXRWrapper) applies the inverse gamma-2.2 map when reading EXR
// back, so a save -> reload round-trip IS pixel-identical (see the note
// in SaveEXR.cpp).
bool SaveEXRFile(LPCTSTR sFileName, void* pData24bppBGR, int nWidth, int nHeight);
