#pragma once

// Saves a 24-bit BGR DIB as JPEG 2000 (.jp2/.j2k/.j2c) with OpenJPEG 2.5
// (openjp2.dll, delay-loaded: no cost when JP2 is never saved).
// The file format is derived from the target file name: .jp2 writes the
// JP2 container (OPJ_CODEC_JP2, signature box + codestream), .j2k/.j2c
// write a raw codestream (OPJ_CODEC_J2K). The dispatch in SaveImage.cpp
// only calls this for IF_JP2, which Helpers::GetImageFormat already maps
// from all three suffixes, so no new IF_ value is needed.
//
// bLossless selects the encoder setup:
//   - lossless (true): the defaults from opj_set_default_encoder_parameters
//     are already lossless (reversible 5-3 wavelet, irreversible == 0),
//     so no parameter is changed.
//   - lossy (false): irreversible 9-7 wavelet, one layer, rate allocation
//     (cp_disto_alloc == 1) with a target rate of 10.0, i.e. roughly 10:1
//     compression. That is the opj_compress lossy recipe, and 10:1 sits in
//     the same ballpark as the application's default JPEG/WebP save quality
//     of 85 for photographic content. There is no JP2SaveQuality INI
//     setting (unlike WEBPSaveQuality/JXLSaveQuality); the rate is a fixed
//     default. Adding one is a coordinator decision, not done here.
bool SaveJP2File(LPCTSTR sFileName, void* pData24bppBGR, int nWidth, int nHeight, bool bLossless);
