#include "stdafx.h"

#include "QOIWrapper.h"
#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "qoi/qoi.h"
#include "MaxImageDef.h"
#include "Helpers.h"

#define LINEAR_TO_SRGB(rgb) ((rgb) > 0 ? ((rgb) < 255 ? (255.0 * (1.055 * pow((rgb)/255.0, 1.0/2.4) - 0.055)) : 255) : 0)

void* QoiReaderWriter::ReadImage(int& width,
	int& height,
	int& nchannels,
	bool& outOfMemory,
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;

	qoi_desc desc = { 0 };
	unsigned char* pDecodedPixels = (unsigned char*)qoi_decode(buffer, sizebytes, &desc, 0);
	if (pDecodedPixels == NULL)
		return NULL;
	width = desc.width;
	height = desc.height;
	nchannels = desc.channels;
	if (abs((double)width * height) > MAX_IMAGE_PIXELS)
		outOfMemory = true;
	if (outOfMemory || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
		QOI_FREE(pDecodedPixels);
		return NULL;
	}
	// QOI 的 width/height 也来自文件头，int 乘法会回绕（65535x65535x4）。
	size_t decoded_stride = (size_t)width * nchannels;
	size_t padded_stride = (size_t)Helpers::DoPadding((int)decoded_stride, 4);
	size_t allocSize = 0;
	if (!IsImageBufferSizeOk(padded_stride, (size_t)height, &allocSize)) {
		outOfMemory = true;
		QOI_FREE(pDecodedPixels);
		return NULL;
	}
	size_t size = decoded_stride * (size_t)height;
	unsigned char* pPixelData = new(std::nothrow) unsigned char[allocSize];
	if (pPixelData != NULL) {
		// Copy from RGB(A) to BGR(A)
		for (size_t i = 0; i < size; i += nchannels) {
			size_t j = i / decoded_stride * padded_stride + i % decoded_stride;
			if (desc.colorspace == QOI_LINEAR) {
				pPixelData[j  ] = (unsigned char)LINEAR_TO_SRGB(pDecodedPixels[i+2]);
				pPixelData[j+1] = (unsigned char)LINEAR_TO_SRGB(pDecodedPixels[i+1]);
				pPixelData[j+2] = (unsigned char)LINEAR_TO_SRGB(pDecodedPixels[i  ]);
			} else {
				pPixelData[j  ] = pDecodedPixels[i+2];
				pPixelData[j+1] = pDecodedPixels[i+1];
				pPixelData[j+2] = pDecodedPixels[i  ];
			}
			if (nchannels == 4)
				pPixelData[j+3] = pDecodedPixels[i+3];
		}
	} else {
		outOfMemory = true;
	}
	QOI_FREE(pDecodedPixels);
	return (void*)pPixelData;
}

void* QoiReaderWriter::Compress(const void* source,
	int width,
	int height,
	int& len) {

	int nchannels = 3;
	void* pOutput = NULL;

	qoi_desc desc;
	desc.width = width;
	desc.height = height;
	desc.channels = nchannels;
	desc.colorspace = QOI_SRGB;
	int input_stride = width * nchannels;
	int padded_stride = Helpers::DoPadding(input_stride, 4);
	int size = input_stride * height;
	unsigned char* pPixelData = new(std::nothrow) unsigned char[input_stride * height];
	unsigned char* pSourcePixels = (unsigned char*)source;
	if (pPixelData != NULL) {
		// Copy from BGR to RGB
		for (int i = 0; i < size; i += nchannels) {
			int j = i / input_stride * padded_stride + i % input_stride;
			pPixelData[i  ] = pSourcePixels[j+2];
			pPixelData[i+1] = pSourcePixels[j+1];
			pPixelData[i+2] = pSourcePixels[j  ];
		}
		pOutput = qoi_encode(pPixelData, &desc, &len);
		delete[] pPixelData;
	}
	return pOutput;
}

void QoiReaderWriter::FreeMemory(void* pointer) {
	QOI_FREE(pointer);
}
