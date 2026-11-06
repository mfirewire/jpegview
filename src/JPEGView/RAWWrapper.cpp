#include "stdafx.h"
#include <limits.h> // F-M12: INT_MAX

#include "RAWWrapper.h"
#include "libraw/libraw.h"
#include "Helpers.h"
#include "ICCProfileTransform.h"
#include "TJPEGWrapper.h"
#include "RawMetadata.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"

CJPEGImage* RawReader::ReadImage(LPCTSTR strFileName, bool& bOutOfMemory, bool& bSizeLimitExceeded, bool bGetThumb)
{
	unsigned char* pPixelData = NULL;

	LibRaw RawProcessor;
	if (RawProcessor.open_file(strFileName) != LIBRAW_SUCCESS) {
		return NULL;
	}

	// A RAW whose declared pixel count far exceeds the file size is corrupt and
	// crashes intermittently in unpack, which reads the image without checking it
	// against the file size. Reject it before unpack. The dimensions must come
	// from raw_width/raw_height: for an RGB DNG without an ActiveArea tag,
	// identify() leaves the effective width/height at zero and only fills the raw
	// dimensions from the TIFF ImageWidth/ImageLength tags, so using width*height
	// here would compute 0 and let the file through.
	{
		const unsigned long long declaredPixels =
			(unsigned long long)RawProcessor.imgdata.sizes.raw_width *
			(unsigned long long)RawProcessor.imgdata.sizes.raw_height;
		// Fail closed: libraw could not determine a sane effective or raw size
		// (the malformed RGB DNG leaves width/height at 0), so do not hand the
		// file to unpack, which would over-read.
		if (RawProcessor.imgdata.sizes.width == 0 ||
			RawProcessor.imgdata.sizes.height == 0 ||
			declaredPixels == 0) {
			return NULL;
		}
		HANDLE hFile = ::CreateFile(strFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return NULL;
		}
		LARGE_INTEGER fileSize;
		BOOL ok = ::GetFileSizeEx(hFile, &fileSize);
		::CloseHandle(hFile);
		if (!ok || fileSize.QuadPart < (LONGLONG)(declaredPixels / 8)) {
			return NULL;
		}
	}

	int width, height, colors, bps;
	
	CJPEGImage* Image = NULL;
	if (!bGetThumb) {
		RawProcessor.imgdata.params.output_bps = 8;

		// Must unpack and process first to get accurate info
		if (RawProcessor.unpack() != LIBRAW_SUCCESS || RawProcessor.dcraw_process() != LIBRAW_SUCCESS) {
			return NULL;
		}

		RawProcessor.get_mem_image_format(&width, &height, &colors, &bps);

		if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
			return NULL;
		}

		// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
		if ((double)width * height > (double)CSettingsProvider::This().MaxImagePixels()) {
			bSizeLimitExceeded = true;
			return NULL;
		}

		// stride * height is int arithmetic: 65535 columns by 21846 rows of 3
		// channels is 4.29 GB and wraps to ~256 KB, after which copy_mem_image
		// still writes the real size. The pixel cap only bounds the pixel
		// count as a double, never the byte count, so it does not catch this.
		// F-M12: width * colors used to be computed in 32-bit int before
		// DoPadding (which itself takes int), and stride was then cast
		// back to (int)/(unsigned int). Compute the row width in size_t,
		// pad in size_t, and refuse anything that does not fit in int
		// before either cast. The IsImageBufferSizeOk check below still
		// bounds stride * height.
		size_t rowBytes = (size_t)width * (size_t)colors;
		if (rowBytes > (size_t)INT_MAX - 3) {
			bOutOfMemory = true;
			return NULL;
		}
		const size_t stride = (rowBytes + 3) & ~(size_t)3;
		if (stride > (size_t)INT_MAX) {
			bOutOfMemory = true;
			return NULL;
		}
		size_t bytes = 0;
		if (!IsImageBufferSizeOk(stride, (size_t)height, &bytes)) {
			bOutOfMemory = true;
			return NULL;
		}

		pPixelData = new(std::nothrow) unsigned char[bytes];
		if (pPixelData == NULL) {
			bOutOfMemory = true;
			return NULL;
		}
		if (RawProcessor.copy_mem_image(pPixelData, (int)stride, 1) != LIBRAW_SUCCESS) {
			delete[] pPixelData;
			return NULL;
		}

		void* transform = ICCProfileTransform::CreateTransform(RawProcessor.imgdata.color.profile, RawProcessor.imgdata.color.profile_length, ICCProfileTransform::FORMAT_BGR);
		ICCProfileTransform::DoTransform(transform, pPixelData, pPixelData, width, height, (unsigned int)stride);
		ICCProfileTransform::DeleteTransform(transform);

		CRawMetadata* metadata = new CRawMetadata(RawProcessor.imgdata.idata.make, RawProcessor.imgdata.idata.model, RawProcessor.imgdata.other.timestamp,
			RawProcessor.imgdata.color.flash_used != 0.0f, RawProcessor.imgdata.other.iso_speed, RawProcessor.imgdata.other.shutter,
			RawProcessor.imgdata.other.focal_len, RawProcessor.imgdata.other.aperture, RawProcessor.imgdata.sizes.flip, width, height,
			RawProcessor.imgdata.other.parsed_gps.latitude, RawProcessor.imgdata.other.parsed_gps.latref, RawProcessor.imgdata.other.parsed_gps.longitude,
			RawProcessor.imgdata.other.parsed_gps.longref, RawProcessor.imgdata.other.parsed_gps.altitude, RawProcessor.imgdata.other.parsed_gps.altref);

		if (pPixelData)
			Image = new CJPEGImage(width, height, pPixelData, NULL, colors, 0, IF_CameraRAW, false, 0, 1, 0, NULL, false, metadata);
	} else if (RawProcessor.is_jpeg_thumb()) {
		TJSAMP eChromoSubSampling;
		if (RawProcessor.unpack_thumb() != LIBRAW_SUCCESS) {
			return NULL;
		}
		libraw_processed_image_t* thumb = RawProcessor.dcraw_make_mem_thumb();
		if (thumb == NULL) {
			return NULL;
		}
		pPixelData = (unsigned char*)TurboJpeg::ReadImage(width, height, colors, eChromoSubSampling, bOutOfMemory, bSizeLimitExceeded, thumb->data, thumb->data_size);
		if (pPixelData != NULL && (colors == 3 || colors == 1))
		{
			CRawMetadata* metadata = new CRawMetadata(RawProcessor.imgdata.idata.make, RawProcessor.imgdata.idata.model, RawProcessor.imgdata.other.timestamp,
				RawProcessor.imgdata.color.flash_used != 0.0f, RawProcessor.imgdata.other.iso_speed, RawProcessor.imgdata.other.shutter,
				RawProcessor.imgdata.other.focal_len, RawProcessor.imgdata.other.aperture, RawProcessor.imgdata.sizes.flip, width, height,
				RawProcessor.imgdata.other.parsed_gps.latitude, RawProcessor.imgdata.other.parsed_gps.latref, RawProcessor.imgdata.other.parsed_gps.longitude,
				RawProcessor.imgdata.other.parsed_gps.longref, RawProcessor.imgdata.other.parsed_gps.altitude, RawProcessor.imgdata.other.parsed_gps.altref);

			Image = new CJPEGImage(width, height, pPixelData, NULL /* Helpers::FindEXIFBlock(thumb->data, thumb->data_size) */, colors,
				Helpers::CalculateJPEGFileHash(thumb->data, thumb->data_size), IF_JPEG_Embedded, false, 0, 1, 0, NULL, false, metadata);

			Image->SetJPEGComment(Helpers::GetJPEGComment(thumb->data, thumb->data_size));
			Image->SetJPEGChromoSampling(eChromoSubSampling);
		}
		// F-L22: a CMYK thumbnail (colors == 4) reaches here with an
		// allocated pPixelData but no CJPEGImage to own it; free it
		// instead of leaking one buffer per CMYK thumbnail load.
		else {
			delete[] pPixelData;
			pPixelData = NULL;
		}
		RawProcessor.dcraw_clear_mem(thumb);
	}
	// RawProcessor.recycle();

	return Image;
}
