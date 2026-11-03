#include "stdafx.h"

#include "RAWWrapper.h"
#include "libraw/libraw.h"
#include "Helpers.h"
#include "ICCProfileTransform.h"
#include "TJPEGWrapper.h"
#include "RawMetadata.h"
#include "MaxImageDef.h"

CJPEGImage* RawReader::ReadImage(LPCTSTR strFileName, bool& bOutOfMemory, bool bGetThumb)
{
	unsigned char* pPixelData = NULL;

	LibRaw RawProcessor;
	if (RawProcessor.open_file(strFileName) != LIBRAW_SUCCESS) {
		return NULL;
	}

	// A RAW whose declared pixel count far exceeds the file size is corrupt.
	// libraw's unpack reads the image data without checking it against the file
	// size, so a truncated DNG declaring 65535x21846 while carrying only a few KB
	// reads past the buffer and crashes intermittently. Reject it before unpack:
	// every real RAW stores at least one bit per pixel, so a file smaller than
	// pixels/8 bytes cannot possibly hold the declared image.
	{
		const unsigned long long declaredPixels =
			(unsigned long long)RawProcessor.imgdata.sizes.width * RawProcessor.imgdata.sizes.height;
		HANDLE hFile = ::CreateFile(strFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile != INVALID_HANDLE_VALUE) {
			LARGE_INTEGER fileSize;
			if (::GetFileSizeEx(hFile, &fileSize) && fileSize.QuadPart < (LONGLONG)(declaredPixels / 8)) {
				::CloseHandle(hFile);
				return NULL;
			}
			::CloseHandle(hFile);
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

		if ((double)width * height > MAX_IMAGE_PIXELS) {
			bOutOfMemory = true;
			return NULL;
		}

		// stride * height is int arithmetic: 65535 columns by 21846 rows of 3
		// channels is 4.29 GB and wraps to ~256 KB, after which copy_mem_image
		// still writes the real size. MAX_IMAGE_PIXELS only bounds the pixel
		// count as a double, never the byte count, so it does not catch this.
		const size_t stride = (size_t)Helpers::DoPadding(width * colors, 4);
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
		pPixelData = (unsigned char*)TurboJpeg::ReadImage(width, height, colors, eChromoSubSampling, bOutOfMemory, thumb->data, thumb->data_size);
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
		RawProcessor.dcraw_clear_mem(thumb);
	}
	// RawProcessor.recycle();

	return Image;
}
