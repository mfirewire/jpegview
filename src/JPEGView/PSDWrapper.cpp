/*  This file uses code adapted from SAIL (https://github.com/HappySeaFox/sail/blob/master/src/sail-codecs/psd/psd.c)
	See the original copyright notice below:

	Copyright (c) 2022 Dmitry Baryshev

	The MIT License

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all
	copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	SOFTWARE.
*/

/* Documentation of the PSD file format can be found here: https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
	Tags can also be found here: https://exiftool.org/TagNames/Photoshop.html

	Useful image resources:
	0x0409 1033 (Photoshop 4.0) Thumbnail resource for Photoshop 4.0 only.See See Thumbnail resource format.
	0x040C 1036 (Photoshop 5.0) Thumbnail resource(supersedes resource 1033).See See Thumbnail resource format.
	0x040F 1039 (Photoshop 5.0) ICC Profile.The raw bytes of an ICC(International Color Consortium) format profile.See ICC1v42_2006 - 05.pdf in the Documentation folder and icProfileHeader.h in Sample Code\Common\Includes .
	0x0411 1041 (Photoshop 5.0) ICC Untagged Profile. 1 byte that disables any assumed profile handling when opening the file. 1 = intentionally untagged.
	0x0417 1047 (Photoshop 6.0) Transparency Index. 2 bytes for the index of transparent color, if any.
	0x0419 1049 (Photoshop 6.0) Global Altitude. 4 byte entry for altitude
	0x041D 1053 (Photoshop 6.0) Alpha Identifiers. 4 bytes of length, followed by 4 bytes each for every alpha identifier.
	Get alpha identifier and look at its index number, if not 0 abort
	0x0421 1057 (Photoshop 6.0) Version Info. 4 bytes version, 1 byte hasRealMergedData, Unicode string : writer name, Unicode string : reader name, 4 bytes file version.
	0x0422 1058 (Photoshop 7.0) EXIF data 1. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
	0x0423 1059 (Photoshop 7.0) EXIF data 3. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
	Not sure what 0x0423 is.
*/

#include "stdafx.h"
#include "PSDWrapper.h"
#include "MaxImageDef.h"
#include "Helpers.h"
#include "TJPEGWrapper.h"
#include "ICCProfileTransform.h"
#include "SettingsProvider.h"


#define PSD_HEADER_SIZE 26

// Throw exception if bShouldThrow is true. Setting a breakpoint in here is useful for debugging
static inline void ThrowIf(bool bShouldThrow) {
	if (bShouldThrow) {
		throw 1;
	}
}

// Read exactly sz bytes of the file into p
static inline void ReadFromFile(void* dst, HANDLE file, DWORD sz) {
	unsigned int nNumBytesRead;
	ThrowIf(!(::ReadFile(file, dst, sz, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == sz));
}

// Read and return an unsigned 64-bit int from file
static inline unsigned long long ReadUInt64FromFile(HANDLE file) {
	unsigned long long val;
	ReadFromFile(&val, file, 8);
	return _byteswap_uint64(val);
}

// Read and return an unsigned int from file
static inline unsigned int ReadUIntFromFile(HANDLE file) {
	unsigned int val;
	ReadFromFile(&val, file, 4);
	return _byteswap_ulong(val);
}

// Read and return an unsigned short from file
static inline unsigned short ReadUShortFromFile(HANDLE file) {
	unsigned short val;
	ReadFromFile(&val, file, 2);
	return _byteswap_ushort(val);
}

// Read and return an unsigned char from file
static inline unsigned char ReadUCharFromFile(HANDLE file) {
	unsigned char val;
	ReadFromFile(&val, file, 1);
	return val;
}

// Move file pointer by offset from current position
static inline void SeekFile(HANDLE file, LONG offset) {
	ThrowIf(::SetFilePointer(file, offset, NULL, FILE_CURRENT) == INVALID_SET_FILE_POINTER);
}

// Move file pointer to offset from beginning of file
static inline void SeekFileFromStart(HANDLE file, LONG offset) {
	ThrowIf(::SetFilePointer(file, offset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER);
}

// Get current position in the file
static inline unsigned int TellFile(HANDLE file) {
	unsigned int ret = ::SetFilePointer(file, 0, NULL, FILE_CURRENT);
	ThrowIf(ret == INVALID_SET_FILE_POINTER);
	return ret;
}

CJPEGImage* PsdReader::ReadImage(LPCTSTR strFileName, bool& bOutOfMemory, bool& bSizeLimitExceeded)
{
	HANDLE hFile;
	hFile = ::CreateFile(strFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	char* pBuffer = NULL;
	void* pPixelData = NULL;
	void* pEXIFData = NULL;
	char* pICCProfile = NULL;
	unsigned int nICCProfileSize = 0;
	void* transform = NULL;
	CJPEGImage* Image = NULL;
	try {
		long long nFileSize = Helpers::GetFileSize(hFile);
		ThrowIf(nFileSize > MAX_PSD_FILE_SIZE);

		// Skip file signature
		SeekFile(hFile, 4);

		// Read version: 1 for PSD, 2 for PSB
		unsigned short nVersion = ReadUShortFromFile(hFile);
		ThrowIf(nVersion != 1 && nVersion != 2);

		// Check reserved bytes
		char pReserved[6];
		ReadFromFile(pReserved, hFile, 6);
		ThrowIf(memcmp(pReserved, "\0\0\0\0\0\0", 6));

		// Read number of channels
		unsigned short nRealChannels = ReadUShortFromFile(hFile);
		// PSD/PSB carry at most 56 channels; a larger value can only come
		// from a malformed header, and it feeds the RLE table skip below.
		ThrowIf(nRealChannels > 56);

		// Read width and height
		unsigned int nHeight = ReadUIntFromFile(hFile);
		unsigned int nWidth = ReadUIntFromFile(hFile);
		// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
		if ((double)nHeight * nWidth > (double)CSettingsProvider::This().MaxImagePixels()) {
			bSizeLimitExceeded = true;
		}
		ThrowIf(bSizeLimitExceeded || max(nHeight, nWidth) > MAX_IMAGE_DIMENSION || !min(nHeight, nWidth));

		// PSD can have bit depths of 1, 2, 4, 8, 16, 32
		unsigned short nBitDepth = ReadUShortFromFile(hFile);
		// Only 8-bit is supported for now
		// F-03 (known limitation, documented not implemented): 16-bit, 32-bit, ZIP-compressed, indexed-color and bitmap PSDs are rejected here; supporting them needs a 16-bit/ZIP/palette path.
		ThrowIf(nBitDepth != 8);

		
		// Read color mode
		// Bitmap = 0; Grayscale = 1; Indexed = 2; RGB = 3; CMYK = 4; Multichannel = 7; Duotone = 8; Lab = 9.
		// F-03 (known limitation): Bitmap (0) and Indexed (2) have no case below, so nChannels stays 0 and the file is rejected; no palette/bitmap path is implemented.
		// TODO: NegateCMYK
		unsigned short nChannels = 0;
		unsigned short nColorMode = ReadUShortFromFile(hFile);
		switch (nColorMode) {
			case MODE_Grayscale:
			case MODE_Duotone:
				nChannels = min(nRealChannels, 1);
				break;
			case MODE_Multichannel:
				nChannels = min(nRealChannels, 3);
				break;
			case MODE_Lab:
			case MODE_RGB:
				nChannels = min(nRealChannels, 4);
				break;
			case MODE_CMYK:
				nChannels = min(nRealChannels, 5);
				break;
		}
		if (nChannels == 2) {
			nChannels = 1;
		}
		ThrowIf(!nChannels);

		// Skip color mode data
		unsigned int nColorDataSize = ReadUIntFromFile(hFile);
		SeekFile(hFile, nColorDataSize);

		// Read resource section size
		unsigned int nResourceSectionSize = ReadUIntFromFile(hFile);

		// This default value should detect alpha channels for PSDs created by programs which don't save alpha identifiers (e.g. Krita, GIMP)
		bool bUseAlpha = nChannels >= 4;

		for (;;) {
			// Resource block signature
			try {
				if (ReadUIntFromFile(hFile) != 0x3842494D) { // "8BIM"
					break;
				}
			} catch (...) {
				break;
			}

			// Resource ID
			unsigned short nResourceID = ReadUShortFromFile(hFile);

			// Skip Pascal string (padded to be even length)
			unsigned char nStringSize = ReadUCharFromFile(hFile);
			SeekFile(hFile, nStringSize | 1);

			// Resource size
			unsigned int nResourceSize = ReadUIntFromFile(hFile);

			// Parse image resources
			switch (nResourceID) {
				case 0x040F: // ICC Profile
					pICCProfile = new(std::nothrow) char[nResourceSize];
					if (pICCProfile != NULL) {
						ReadFromFile(pICCProfile, hFile, nResourceSize);
						SeekFile(hFile, -nResourceSize);
						nICCProfileSize = nResourceSize;
					}
					break;
				case 0x041D: // 0x041D 1053 (Photoshop 6.0) Alpha Identifiers. 4 bytes of length, followed by 4 bytes each for every alpha identifier.
					if (bUseAlpha) {
						bUseAlpha = false;
						int i = 0;
						while (i < nResourceSize / 4) {
							i++;
							if (ReadUIntFromFile(hFile) == 0) {
								bUseAlpha = true;
								break;
							}
						}
						SeekFile(hFile, -i * 4);
					}
					break;
				case 0x0421: // 0x0421 1057 (Photoshop 6.0) Version Info. 4 bytes version, 1 byte hasRealMergedData, Unicode string : writer name, Unicode string : reader name, 4 bytes file version.
					if (nResourceSize >= 5) {
						ReadUIntFromFile(hFile);
						// See https://exiftool.org/forum/index.php?topic=12897.0
						ThrowIf(!ReadUCharFromFile(hFile));
						SeekFile(hFile, -5);
					}
					break;
				case 0x0422: // 0x0422 1058 (Photoshop 7.0) EXIF data 1. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
				case 0x0423: // 0x0423 1059 (Photoshop 7.0) EXIF data 3. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
					if (pEXIFData == NULL && nResourceSize < 65526) {
						pEXIFData = new(std::nothrow) char[nResourceSize + 10];
						if (pEXIFData != NULL) {
							memcpy(pEXIFData, "\xFF\xE1\0\0Exif\0\0", 10);
							*((unsigned short*)pEXIFData + 1) = _byteswap_ushort(nResourceSize + 8);
							ReadFromFile((char*)pEXIFData + 10, hFile, nResourceSize);
							SeekFile(hFile, -nResourceSize);
						}
					}
					break;
			}

			// Skip resource data (padded to be even length)
			SeekFile(hFile, (nResourceSize + 1) & -2);
		}
		
		// Go back to start of file
		SeekFileFromStart(hFile, PSD_HEADER_SIZE + 4 + nColorDataSize + 4 + nResourceSectionSize);

		// Skip Layer and Mask Info section
		unsigned long long nLayerSize;
		if (nVersion == 2) {
			nLayerSize = ReadUInt64FromFile(hFile);
		} else {
			nLayerSize = ReadUIntFromFile(hFile);
		}
		unsigned char nLayerSizeBytes = 4 * nVersion;
		SeekFile(hFile, nLayerSizeBytes);
		short nLayerCount = ReadUShortFromFile(hFile);
		bUseAlpha = bUseAlpha && (nLayerCount <= 0);
		SeekFile(hFile, nLayerSize - 2 - nLayerSizeBytes);

		// Compression. 0 = Raw Data, 1 = RLE compressed, 2 = ZIP without prediction, 3 = ZIP with prediction.
		unsigned short nCompressionMethod = ReadUShortFromFile(hFile);
		// F-03 (known limitation): ZIP without/with prediction (2/3) is rejected here; only raw and RLE are implemented.
		ThrowIf(nCompressionMethod != COMPRESSION_RLE && nCompressionMethod != COMPRESSION_None);

		unsigned int nImageDataSize = nFileSize - TellFile(hFile);
		pBuffer = new(std::nothrow) char[nImageDataSize];
		if (pBuffer == NULL) {
			bOutOfMemory = true;
			ThrowIf(true);
		}
		ReadFromFile(pBuffer, hFile, nImageDataSize);

		if (!bUseAlpha && nColorMode != MODE_CMYK) {
			nChannels = min(nChannels, 3);
		}

		// Apply ICC Profile
		if (nChannels >= 3) {
			if (nColorMode == MODE_Lab) {
				transform = ICCProfileTransform::CreateTransform(pICCProfile, nICCProfileSize, nChannels == 4 ? ICCProfileTransform::FORMAT_LabA : ICCProfileTransform::FORMAT_Lab);
				if (transform == NULL) {
					// F-M22 (reviewed): fall back to the Lightness plane as
					// grayscale. This is set BEFORE nRowSize and the decompress
					// loops below, so only channel 0 is read and CJPEGImage's
					// Convert1To4Channels uses the same DoPadding(nWidth,4)
					// stride: the row layouts do match. The old report's
					// "channel count changed without repacking pixels" was
					// incorrect. The only real limitation is a silent loss of
					// the a/b channels, with no user-visible warning.
					nChannels = 1;
				}
			} else if (nColorMode == MODE_RGB) {
				transform = ICCProfileTransform::CreateTransform(pICCProfile, nICCProfileSize, nChannels == 4 ? ICCProfileTransform::FORMAT_BGRA : ICCProfileTransform::FORMAT_BGR);
			} else if (nChannels >= 4 && nColorMode == MODE_CMYK) {
				// Alpha channel in CMYK is disabled; always treat as 4-channel YMCK
				// so the transform format matches the data layout used below.
				nChannels = 4;
				transform = ICCProfileTransform::CreateTransform(pICCProfile, nICCProfileSize, ICCProfileTransform::FORMAT_YMCK);
			}
		}

		int nRowSize = Helpers::DoPadding(nWidth * nChannels, 4);
		// nRowSize * nHeight is int arithmetic: 262144 x 65535 is 17GB and wraps.
		size_t allocSize = 0;
		if (!IsImageBufferSizeOk((size_t)nRowSize, (size_t)nHeight, &allocSize)) {
			bOutOfMemory = true;
			ThrowIf(true);
		}
		pPixelData = new(std::nothrow) char[allocSize];
		if (pPixelData == NULL) {
			bOutOfMemory = true;
			ThrowIf(true);
		}
		// TODO: non-8bit, better non-RGB support
		// non-8bit must first be decompressed as arbitrary data
		// TODO: continue next row at end of row bytes (to support corrupt images)
		unsigned char* p = (unsigned char*)pBuffer;
		if (nCompressionMethod == COMPRESSION_RLE) {
			// Skip byte counts for scanlines. The table size is computed in
			// 64 bit: nHeight * nRealChannels * 2 * nVersion overflows 32 bit
			// for large headers (65535 x 65535 x 2 x 2 wraps), and the table
			// must fit inside the image data actually read.
			unsigned __int64 nTableSize = (unsigned __int64)nHeight * nRealChannels * (nVersion == 2 ? 4 : 2);
			ThrowIf(nTableSize > nImageDataSize);
			p += (size_t)nTableSize;
			unsigned char* pOffset = p;
			for (unsigned channel = 0; channel < nChannels; channel++) {
				unsigned rchannel;
				if (nColorMode == MODE_Lab) {
					rchannel = channel;
				} else {
					rchannel = (-channel - 2) % nChannels;
				}
				for (unsigned row = 0; row < nHeight; row++) {
					p = pOffset;

					for (unsigned count = 0; count < nWidth; ) {
						unsigned char c;
						ThrowIf(p >= (unsigned char*)pBuffer + nImageDataSize);
						c = *p;
						p += 1;

						if (c > 128) {
							c = ~c + 2;

							ThrowIf(p >= (unsigned char*)pBuffer + nImageDataSize);
							unsigned char value = *p;
							p += 1;

							// ST-05: a PackBits run belongs to one row; a run reaching past the row edge would write into the next row (cross-row corruption). Reject it instead.
							ThrowIf((unsigned)count + c > nWidth);
							for (unsigned i = count; i < count + c; i++) {
								unsigned char* scan = (unsigned char*)pPixelData + row * nRowSize + i * nChannels;
								unsigned char* pixel = scan + rchannel;
								ThrowIf(pixel >= (unsigned char*)pPixelData + allocSize);
								*pixel = value;
							}
						} else if (c < 128) {
							c++;

							// ST-05: see the run-length branch above; a literal run must also end at the row edge.
							ThrowIf((unsigned)count + c > nWidth);
							for (unsigned i = count; i < count + c; i++) {
								ThrowIf(p >= (unsigned char*)pBuffer + nImageDataSize);
								unsigned char value = *p;
								p += 1;

								unsigned char* scan = (unsigned char*)pPixelData + row * nRowSize + i * nChannels;
								unsigned char* pixel = scan + rchannel;
								ThrowIf(pixel >= (unsigned char*)pPixelData + allocSize);
								*pixel = value;
							}
						}

						count += c;
					}

					// The row byte-count entry itself must lie inside the image
					// data; the loop's p checks do not cover this read (pOffset
					// advances from the table, not from p).
					unsigned int nElemSize = (nVersion == 2) ? 4 : 2;
					ThrowIf(((unsigned __int64)channel * nHeight + row) * nElemSize + nElemSize > nImageDataSize);
					if (nVersion == 2) {
						pOffset += _byteswap_ulong(*(unsigned int*)(pBuffer + (channel * nHeight + row) * 4));
					} else {
						pOffset += _byteswap_ushort(*(unsigned short*)(pBuffer + (channel * nHeight + row) * 2));
					}
#ifdef DEBUG
					if (p != pOffset) {
						WCHAR buf[100];
						swprintf(buf, _T("Misaligned scan line bytes (%+d) for channel %d row %d\n"), p - pOffset, channel, row);
						::OutputDebugString(buf);
					}
#endif
				}
			}
		} else { // No compression
			for (unsigned channel = 0; channel < nChannels; channel++) {
				unsigned rchannel;
				if (nColorMode == MODE_Lab) {
					rchannel = channel;
				} else {
					rchannel = (-channel - 2) % nChannels;
				}
				for (unsigned row = 0; row < nHeight; row++) {
					for (unsigned count = 0; count < nWidth; count++) {
						ThrowIf(p >= (unsigned char*)pBuffer + nImageDataSize);
						unsigned char value = *p;
						p += 1;

						unsigned char* scan = (unsigned char*)pPixelData + row * nRowSize + count * nChannels;
						unsigned char* pixel = scan + rchannel;
						ThrowIf(pixel >= (unsigned char*)pPixelData + allocSize);
						*pixel = value;
					}
				}
			}
		}

		COLORREF backgroundColor = CSettingsProvider::This().ColorTransparency();

		if (nColorMode == MODE_CMYK) {
			if (transform != NULL) {
				uint32* pImage32 = (uint32*)pPixelData;

				// S-L5: the nChannels == 5 cases below are dead branches,
				// annotated rather than deleted. CMYK starts with up to 5
				// channels, but the ICC block above forces nChannels = 4
				// for CMYK (alpha is disabled there), so a 5-channel CMYK
				// image never reaches this loop. The branches are kept so
				// a future alpha-capable CMYK path can revive the layout
				// deliberately instead of rediscovering it.
				for (size_t i = 0; i < (size_t)nWidth * nHeight; i++) {
					if (nChannels == 5)
						pImage32 = (uint32*)((uint8*)pImage32 + 1);
					*pImage32++ = ~(*pImage32);
				}

				if (nChannels == 5) {
					void* tempBuffer = new(std::nothrow) char[allocSize];
					if (ICCProfileTransform::DoTransform(transform, pPixelData, tempBuffer, nWidth, nHeight, nRowSize)) {
						delete[] pPixelData;
						pPixelData = tempBuffer;
					} else {
						delete[] tempBuffer;
					}
				} else {
					ICCProfileTransform::DoTransform(transform, pPixelData, pPixelData, nWidth, nHeight, nRowSize);
				}
				nChannels -= 1;
			} else if (nChannels == 4) {
				// F-M23: no ICC transform available. Convert the inverted YMCK
				// plane with the same naive ink formula as the JPEG CMYK fallback
				// (ConvertYmckInkToRgbNaive in ImageLoadThread.cpp), instead of
				// treating the K channel as alpha over a black background.
				uint32* pImage32 = (uint32*)pPixelData;
				for (size_t i = 0; i < (size_t)nWidth * nHeight; i++) {
					*pImage32++ = ~(*pImage32);
				}
				const size_t outStride = (size_t)Helpers::DoPadding((int)nWidth * 3, 4);
				size_t outBytes = 0;
				unsigned char* pRgb = NULL;
				if (IsImageBufferSizeOk(outStride, (size_t)nHeight, &outBytes))
					pRgb = new(std::nothrow) unsigned char[outBytes];
				if (pRgb != NULL) {
					const unsigned char* p = (const unsigned char*)pPixelData;
					for (size_t y = 0; y < (size_t)nHeight; y++) {
						unsigned char* q = pRgb + y * outStride;
						for (size_t x = 0; x < (size_t)nWidth; x++, p += 4) {
							const unsigned int inkK = 255 - p[3];
							q[x*3+0] = (unsigned char)((255 - p[0]) * inkK / 255); // B from Y
							q[x*3+1] = (unsigned char)((255 - p[1]) * inkK / 255); // G from M
							q[x*3+2] = (unsigned char)((255 - p[2]) * inkK / 255); // R from C
						}
					}
					delete[] (char*)pPixelData;
					pPixelData = pRgb;
					// nChannels MUST become 3: the 4-channel alpha blend below
					// would otherwise read BGR triples as BGRA quads.
					nChannels = 3;
				} else {
					backgroundColor = 0; // keep the old fallback on allocation failure
				}
			} else {
				// P0-1 guard: a malformed CMYK PSD can carry 1 or 3 channels
				// (the header only caps the count at 56 and ThrowIf(!nChannels)
				// rejects zero). There is no 4-byte YMCK plane to invert, so
				// keep the historical K-as-alpha fallback instead of reading
				// past the buffer allocated for nChannels bytes per pixel.
				backgroundColor = 0;
			}
		} else {
			ICCProfileTransform::DoTransform(transform, pPixelData, pPixelData, nWidth, nHeight, nRowSize);
		}

		if (nChannels == 4) {
			uint32* pImage32 = (uint32*)pPixelData;
			// Multiply alpha value into each AABBGGRR pixel
			for (size_t i = 0; i < (size_t)nWidth * nHeight; i++)
				*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, backgroundColor);
		}

		Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nChannels, 0, IF_PSD, false, 0, 1, 0);
	} catch (...) {
		delete Image;
		Image = NULL;
	}
	::CloseHandle(hFile);
	if (Image == NULL) {
		delete[] pPixelData;
	}
	delete[] pBuffer;
	delete[] pEXIFData;
	delete[] pICCProfile;
	ICCProfileTransform::DeleteTransform(transform);
	return Image;
};


CJPEGImage* PsdReader::ReadThumb(LPCTSTR strFileName, bool& bOutOfMemory, bool& bSizeLimitExceeded)
{
	HANDLE hFile;
	hFile = ::CreateFile(strFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	char* pBuffer = NULL;
	void* pPixelData = NULL;
	void* pEXIFData = NULL;
	CJPEGImage* Image = NULL;
	int nWidth, nHeight, nChannels;
	int nJpegSize;
	TJSAMP eChromoSubSampling;

	try {
		// Skip file header
		SeekFile(hFile, PSD_HEADER_SIZE);

		// Skip color mode data
		unsigned int nColorDataSize = ReadUIntFromFile(hFile);
		SeekFile(hFile, nColorDataSize);

		// Skip resource section size
		ReadUIntFromFile(hFile);

		for (;;) {
			// Resource block signature
			try {
				if (ReadUIntFromFile(hFile) != 0x3842494D) { // "8BIM"
					break;
				}
			} catch (...) {
				break;
			}


			unsigned short nResourceID = ReadUShortFromFile(hFile);

			// Skip Pascal string (padded to be even length)
			unsigned char nStringSize = ReadUCharFromFile(hFile);
			SeekFile(hFile, nStringSize | 1);

			// Resource size
			unsigned int nResourceSize = ReadUIntFromFile(hFile);

			// Parse image resources
			switch (nResourceID) {
				case 0x0409: // 0x0409 1033 (Photoshop 4.0) Thumbnail resource for Photoshop 4.0 only. See See Thumbnail resource format.
				case 0x040C: // 0x040C 1036 (Photoshop 5.0) Thumbnail resource (supersedes resource 1033). See See Thumbnail resource format.
					// Skip thumbnail resource header
					SeekFile(hFile, 28);

					// Read embedded JPEG thumbnail
					nJpegSize = nResourceSize - 28;
					if (nJpegSize > MAX_JPEG_FILE_SIZE) {
						bOutOfMemory = true;
						ThrowIf(true);
					}

					pBuffer = new(std::nothrow) char[nJpegSize];
					if (pBuffer == NULL) {
						bOutOfMemory = true;
						ThrowIf(true);
					}

					ReadFromFile(pBuffer, hFile, nJpegSize);
					SeekFile(hFile, -nResourceSize);


					pPixelData = TurboJpeg::ReadImage(nWidth, nHeight, nChannels, eChromoSubSampling, bOutOfMemory, bSizeLimitExceeded, pBuffer, nJpegSize);
					break;

				case 0x0422: // 0x0422 1058 (Photoshop 7.0) EXIF data 1. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
				case 0x0423: // 0x0423 1059 (Photoshop 7.0) EXIF data 3. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
					if (pEXIFData == NULL && nResourceSize < 65526) {
						pEXIFData = new(std::nothrow) char[nResourceSize + 10];
						if (pEXIFData != NULL) {
							memcpy(pEXIFData, "\xFF\xE1\0\0Exif\0\0", 10);
							*((unsigned short*)pEXIFData + 1) = _byteswap_ushort(nResourceSize + 8);
							ReadFromFile((char*)pEXIFData + 10, hFile, nResourceSize);
							SeekFile(hFile, -nResourceSize);
						}
					}
					break;
			}

			// Skip resource data (padded to be even length)
			SeekFile(hFile, (nResourceSize + 1) & -2);
		}

		if (pPixelData != NULL) {
			Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData,
				nChannels, Helpers::CalculateJPEGFileHash(pBuffer, nJpegSize), IF_JPEG_Embedded, false, 0, 1, 0);
			Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nJpegSize));
			Image->SetJPEGChromoSampling(eChromoSubSampling);
		}

	} catch(...) {
		delete Image;
		Image = NULL;
	}
	::CloseHandle(hFile);
	if (Image == NULL) {
		delete[] pPixelData;
	}
	delete[] pEXIFData;
	delete[] pBuffer;
	return Image;
}
