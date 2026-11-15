
#include "stdafx.h"
#include "TJPEGWrapper.h"
#include "libjpeg-turbo\include\turbojpeg.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "GainMap.h"
#include "HdrToneMap.h"
#include <cstring>

namespace {

// Find the XMP packet in a JPEG's APP1 segment (identifier
// "http://ns.adobe.com/xap/1.0/\0"). Returns a pointer into `jpeg`
// (no copy) and the packet size, or false when there is no XMP APP1
// or the markers are malformed/truncated. The packet itself may
// contain trailing padding; the XMP parser only searches it.
bool FindXmpPacket(const unsigned char* jpeg, int sizebytes, const unsigned char*& xmp, size_t& xmpSize) {
	xmp = NULL;
	xmpSize = 0;
	if (jpeg == NULL || sizebytes < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
		return false;
	}
	static const char kXmpPrefix[] = "http://ns.adobe.com/xap/1.0/";
	const size_t kPrefixLen = 28;  // + NUL = 29 bytes before the packet
	const size_t size = (size_t)sizebytes;
	size_t pos = 2;
	while (pos + 1 < size) {
		if (jpeg[pos] != 0xFF) {
			return false;
		}
		const unsigned char marker = jpeg[pos + 1];
		if (marker == 0xFF) {
			++pos;  // fill byte before the marker code
			continue;
		}
		pos += 2;
		if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
			(marker >= 0xD0 && marker <= 0xD7) || marker == 0xDA) {
			return false;  // standalone marker or SOS: no more segments
		}
		if (pos + 2 > size) {
			return false;
		}
		const size_t len = (size_t)(((size_t)jpeg[pos] << 8) | (size_t)jpeg[pos + 1]);
		if (len < 2 || pos + len > size) {
			return false;  // truncated segment
		}
		if (marker == 0xE1 && len - 2 >= kPrefixLen + 1 &&
			std::memcmp(jpeg + pos + 2, kXmpPrefix, kPrefixLen) == 0 &&
			jpeg[pos + 2 + kPrefixLen] == 0) {
			xmp = jpeg + pos + 2 + kPrefixLen + 1;
			xmpSize = len - 2 - (kPrefixLen + 1);
			return true;
		}
		pos += len;
	}
	return false;
}

// UltraHDR gain-map reconstruction (ISO 21496-1, Option D Phase 2).
// `pBaseBgr` is the already-decoded 3-channel base image (TJPAD
// stride). On success returns a NEW tone-mapped BGR buffer with the
// same stride and deletes `pBaseBgr`; the caller must use the
// returned buffer instead. On ANY failure returns NULL and leaves
// `pBaseBgr` untouched, so the caller keeps the SDR base: a broken
// gain map must never fail the load.
unsigned char* TryApplyUltraHdrGainMap(const unsigned char* jpegBuf, int sizebytes,
	unsigned char* pBaseBgr, int width, int height) {
	size_t gainOffset = 0, gainSize = 0;
	if (!gainmap::FindGainMapJpeg(jpegBuf, (size_t)sizebytes, gainOffset, gainSize)) {
		return NULL;  // no MPF gain map: SDR path, untouched
	}
	const unsigned char* pGainJpeg = jpegBuf + gainOffset;

	// Decode the gain-map JPEG to grayscale (Y/luma, 1 byte/pixel).
	// An RGB decode would be wrong here: the gain-map JPEG is YCbCr
	// with Cb/Cr != 128, so R != G != B.
	tjhandle hGain = tj3Init(TJINIT_DECOMPRESS);
	if (hGain == NULL) {
		return NULL;
	}
	unsigned char* pGainGray = NULL;
	int gainW = 0, gainH = 0;
	if (tj3DecompressHeader(hGain, (unsigned char*)pGainJpeg, (int)gainSize) == 0) {
		gainW = tj3Get(hGain, TJPARAM_JPEGWIDTH);
		gainH = tj3Get(hGain, TJPARAM_JPEGHEIGHT);
		size_t gainBytes = 0;
		if (gainW > 0 && gainH > 0 &&
			gainW <= MAX_IMAGE_DIMENSION && gainH <= MAX_IMAGE_DIMENSION &&
			IsImageBufferSizeOk((size_t)gainW, (size_t)gainH, &gainBytes)) {
			pGainGray = new(std::nothrow) unsigned char[gainBytes];
			if (pGainGray != NULL) {
				if (tj3Decompress8(hGain, (unsigned char*)pGainJpeg, (int)gainSize,
						pGainGray, gainW, TJPF_GRAY) != 0) {
					delete[] pGainGray;
					pGainGray = NULL;
				}
			}
		}
	}
	tj3Destroy(hGain);
	if (pGainGray == NULL) {
		return NULL;
	}

	// Metadata: ISO 21496-1 binary APP2 in the gain-map JPEG first,
	// XMP (hdrgm) in the primary JPEG as fallback.
	gainmap::Metadata meta;
	bool bHaveMeta = false;
	const uint8_t* pIsoPayload = NULL;
	size_t isoSize = 0;
	if (gainmap::FindIso21496Payload(pGainJpeg, gainSize, pIsoPayload, isoSize)) {
		bHaveMeta = gainmap::ParseIso21496Metadata(pIsoPayload, isoSize, meta);
	}
	if (!bHaveMeta) {
		const unsigned char* pXmp = NULL;
		size_t xmpSize = 0;
		if (FindXmpPacket(jpegBuf, sizebytes, pXmp, xmpSize)) {
			bHaveMeta = gainmap::ParseXmpMetadata(
				std::string((const char*)pXmp, xmpSize), meta);
		}
	}
	if (!bHaveMeta) {
		delete[] pGainGray;
		return NULL;
	}

	// Base BGR -> RGB temp, then gain-map reconstruction to linear
	// float, then BT.2390 tone mapping back to BGR (same stride).
	// Every allocation is size-checked; any failure returns NULL and
	// the caller keeps the SDR base.
	size_t rgbBytes = 0, linearBytes = 0;
	if (!IsImageBufferSizeOk((size_t)width * 3, (size_t)height, &rgbBytes) ||
		!IsImageBufferSizeOk((size_t)width * 3 * sizeof(float), (size_t)height, &linearBytes)) {
		delete[] pGainGray;
		return NULL;
	}
	unsigned char* pBaseRgb = new(std::nothrow) unsigned char[rgbBytes];
	float* pLinear = new(std::nothrow) float[linearBytes / sizeof(float)];
	unsigned char* pOutBgr = NULL;
	if (pBaseRgb != NULL && pLinear != NULL) {
		const size_t baseStride = (size_t)TJPAD(width * 3);
		for (int y = 0; y < height; ++y) {
			const unsigned char* pSrcRow = pBaseBgr + (size_t)y * baseStride;
			unsigned char* pDstRow = pBaseRgb + (size_t)y * (size_t)width * 3;
			for (int x = 0; x < width; ++x) {
				pDstRow[x * 3 + 0] = pSrcRow[x * 3 + 2];
				pDstRow[x * 3 + 1] = pSrcRow[x * 3 + 1];
				pDstRow[x * 3 + 2] = pSrcRow[x * 3 + 0];
			}
		}
		if (gainmap::ApplyGainMap(pBaseRgb, width, height, pGainGray, gainW, gainH, meta, 1.0, pLinear)) {
			const size_t outStride = (size_t)TJPAD(width * 3);
			size_t outBytes = 0;
			if (IsImageBufferSizeOk(outStride, (size_t)height, &outBytes)) {
				pOutBgr = new(std::nothrow) unsigned char[outBytes];
				if (pOutBgr != NULL) {
					HdrToneMap::ConvertLinearImage(pLinear, width, height,
						(size_t)width * 3, pOutBgr, outStride);
				}
			}
		}
	}
	delete[] pBaseRgb;
	delete[] pLinear;
	delete[] pGainGray;
	if (pOutBgr == NULL) {
		return NULL;
	}
	delete[] pBaseBgr;
	return pOutBgr;
}

}  // namespace

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
	// UltraHDR gain map (ISO 21496-1): if this JPEG carries an MPF
	// gain-map image, reconstruct HDR and tone-map it; on any
	// failure the SDR base is returned unchanged. CMYK/YCCK
	// (4-channel) images never take this path.
	if (pPixelData != NULL && !bCMYK) {
		unsigned char* pGainMapped = TryApplyUltraHdrGainMap(
			(const unsigned char*)buffer, sizebytes, pPixelData, width, height);
		if (pGainMapped != NULL) {
			pPixelData = pGainMapped;
		}
	}
	}

	tj3Destroy(hDecoder);

	return pPixelData;
}

TJSAMP TurboJpeg::GetSubsampling(const void *buffer, int sizebytes) {
	TJSAMP eSampling = TJSAMP_420;
	if (buffer == NULL || sizebytes <= 0) {
		return eSampling;
	}
	tjhandle hDecoder = tj3Init(TJINIT_DECOMPRESS);
	if (hDecoder == NULL) {
		return eSampling;
	}
	if (tj3DecompressHeader(hDecoder, (unsigned char*)buffer, sizebytes) == 0) {
		eSampling = (TJSAMP)tj3Get(hDecoder, TJPARAM_SUBSAMP);
		// UNKNOWN would index tjMCUWidth[-1] in CJPEGImage (R-06).
		if (eSampling == TJSAMP_UNKNOWN) {
			eSampling = TJSAMP_444;
		}
	}
	tj3Destroy(hDecoder);
	return eSampling;
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
