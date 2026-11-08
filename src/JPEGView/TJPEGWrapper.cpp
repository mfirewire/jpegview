
#include "stdafx.h"
#include "TJPEGWrapper.h"
#include "libjpeg-turbo\include\turbojpeg.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"

void * TurboJpeg::ReadImage(int &width,
					   int &height,
					   int &nchannels,
					   TJSAMP &chromoSubsampling,
					   bool &outOfMemory,
					   bool &isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
					   const void *buffer,
					   int sizebytes)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 3;
	chromoSubsampling = TJSAMP_420;

	tjhandle hDecoder = tj3Init(TJINIT_DECOMPRESS);
	if (hDecoder == NULL) {
		return NULL;
	}

	unsigned char* pPixelData = NULL;
	int nResult = tj3DecompressHeader(hDecoder, (unsigned char*)buffer, sizebytes);
	if (nResult == 0) {
		width = tj3Get(hDecoder, TJPARAM_JPEGWIDTH);
		height = tj3Get(hDecoder, TJPARAM_JPEGHEIGHT);
		chromoSubsampling = (TJSAMP)tj3Get(hDecoder, TJPARAM_SUBSAMP);
		// CMYK/YCCK JPEGs carry 4 components, which the 3-channel BGR output
		// cannot accept (jdcolor.c ERREXITs on it). Decode them to a packed
		// inverted-CMYK plane (TJPF_CMYK, 255-C/255-M/255-Y/255-K) and let the
		// caller flip and convert to RGB; this also stops them from falling back
		// to GDI+, whose colour management mis-renders them.
		const int colorSpace = tj3Get(hDecoder, TJPARAM_COLORSPACE);
		const bool bCMYK = (colorSpace == TJCS_CMYK || colorSpace == TJCS_YCCK);
		// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
		// No abs(): libjpeg-turbo only returns non-negative dimensions for
		// a parsed header; unqualified abs(double) resolves to ::abs(int).
		if ((double)width * (double)height > (double)CSettingsProvider::This().MaxImagePixels()) {
			isSizeLimitExceeded = true;
		} else if (width <= MAX_IMAGE_DIMENSION && height <= MAX_IMAGE_DIMENSION && (chromoSubsampling != TJSAMP_UNKNOWN || bCMYK)) {
			// TJPAD is int arithmetic, but width <= 65535 keeps width*bytesPerPixel
			// (max 262140) inside int. The real risk is stride*height: 65535 rows of
			// 21846 by 4 channels is 5.7 GB and wraps in 32 bits, so the byte count
			// must be checked through the helper rather than multiplied directly.
			const int bytesPerPixel = bCMYK ? 4 : 3;
			const int pixelFormat = bCMYK ? TJPF_CMYK : TJPF_BGR;
			nchannels = bytesPerPixel;
			const size_t stride = (size_t)TJPAD(width * bytesPerPixel);
			size_t bytes = 0;
			if (!IsImageBufferSizeOk(stride, (size_t)height, &bytes)) {
				outOfMemory = true;
			} else {
				pPixelData = new(std::nothrow) unsigned char[bytes];
				if (pPixelData != NULL) {
					nResult = tj3Decompress8(hDecoder, (unsigned char*)buffer, sizebytes, pPixelData, (int)stride, pixelFormat);
					if (nResult != 0) {
						delete[] pPixelData;
						pPixelData = NULL;
					}
				} else {
					outOfMemory = true;
				}
			}
		}
	}

	tj3Destroy(hDecoder);

	return pPixelData;
}

void * TurboJpeg::Compress(const void *source,
					  int width,
					  int height,
					  int &len,
					  bool &outOfMemory,
					  int quality,
					  TJSAMP subsampling)
{
	outOfMemory = false;
	len = 0;
	tjhandle hEncoder = tj3Init(TJINIT_COMPRESS);
	if (hEncoder == NULL) {
		return NULL;
	}

	unsigned char* pJPEGCompressed = NULL;
	size_t nCompressedLen = 0;
	// F-H09: was hardcoded TJSAMP_420; now from the JPEGSaveSubsampling
	// INI setting (444/422/420, default 420). The EXIF thumbnail below
	// keeps 420 via the default argument.
	tj3Set(hEncoder, TJPARAM_SUBSAMP, subsampling);
	tj3Set(hEncoder, TJPARAM_QUALITY, quality);
	int nResult = tj3Compress8(hEncoder, (unsigned char*)source, width, TJPAD(width * 3), height, TJPF_BGR,
		&pJPEGCompressed, &nCompressedLen);
	if (nResult != 0 || nCompressedLen > INT_MAX) {
		if (pJPEGCompressed == NULL) {
			outOfMemory = true;
		}
		Free(pJPEGCompressed);
		pJPEGCompressed = NULL;
	}

	len = nCompressedLen;

	tj3Destroy(hEncoder);

	return pJPEGCompressed;
}

void TurboJpeg::Free(void* buffer) {
	tj3Free(buffer);
}
