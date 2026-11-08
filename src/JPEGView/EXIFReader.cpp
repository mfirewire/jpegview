#include "StdAfx.h"
#include "EXIFReader.h"
#include "ImageProcessingTypes.h"
#include "Helpers.h"

double CEXIFReader::UNKNOWN_DOUBLE_VALUE = 283740261.192864;

static uint32 ReadUInt(void * ptr, bool bLittleEndian) {
	uint32 nValue = *((uint32*)ptr);
	if (!bLittleEndian) {
		return _byteswap_ulong(nValue);
	} else {
		return nValue;
	}
}

static void WriteUInt(void * ptr, uint32 nValue, bool bLittleEndian) {
	uint32 nVal = nValue;
	if (!bLittleEndian) {
		nVal = _byteswap_ulong(nVal);
	}
	*((uint32*)ptr) = nVal;
}

static uint16 ReadUShort(void * ptr, bool bLittleEndian) {
	uint16 nValue = *((uint16*)ptr);
	if (!bLittleEndian) {
		return _byteswap_ushort(nValue);
	} else {
		return nValue;
	}
}

static void WriteUShort(void * ptr, uint16 nValue, bool bLittleEndian) {
	uint16 nVal = nValue;
	if (!bLittleEndian) {
		nVal = _byteswap_ushort(nVal);
	}
	*((uint16*)ptr) = nVal;
}

static uint8* FindTag(uint8* ptr, uint8* ptrLast, uint16 nTag, bool bLittleEndian) {
	while (ptr < ptrLast) {
		if (ReadUShort(ptr, bLittleEndian) == nTag) {
			return ptr;
		}
		ptr += 12;
	}
	return NULL;
}

// R-17: every offset read out of an EXIF tag is file-controlled, so it has
// to stay inside the APP1 block. pTIFFEnd is the first byte past the block
// (m_pApp1 + nApp1Size); the check runs in 64 bit so a huge offset cannot
// wrap around and pass. Callers pass the TIFF header, offsets are relative
// to it.
static bool IsTIFFRangeValid(uint8* pTIFFHeader, const uint8* pTIFFEnd, uint32 nOffset, unsigned __int64 nLength) {
	if (pTIFFEnd < pTIFFHeader) {
		return false;
	}
	unsigned __int64 nTIFFLen = (unsigned __int64)(pTIFFEnd - pTIFFHeader);
	return (unsigned __int64)nOffset <= nTIFFLen && nLength <= nTIFFLen - (unsigned __int64)nOffset;
}

static void ReadStringTag(CString & strOut, uint8* ptr, uint8* pTIFFHeader, const uint8* pTIFFEnd, bool bLittleEndian, bool bTryReadAsUTF8 = false, uint32 maxLength = 65536) {
	try {
		if (ptr != NULL && ReadUShort(ptr + 2, bLittleEndian) == 2) {
			uint32 nSize = ReadUInt(ptr + 4, bLittleEndian);
			if (nSize > maxLength) {
				strOut.Empty();
				return;
			}
			LPCSTR pString;
			if (nSize <= 4) {
				// Inline value: lives inside the 12-byte tag entry itself.
				pString = (LPCSTR)(ptr + 8);
			} else {
				uint32 nOffset = ReadUInt(ptr + 8, bLittleEndian);
				if (!IsTIFFRangeValid(pTIFFHeader, pTIFFEnd, nOffset, nSize)) {
					strOut.Empty();
					return;
				}
				pString = (LPCSTR)(pTIFFHeader + nOffset);
			}
			// Bounded scan: strlen would run past the block when the declared
			// bytes contain no NUL at all.
			int nMaxChars = 0;
			while (nMaxChars < (int)nSize && pString[nMaxChars] != 0) {
				nMaxChars++;
			}
			if (bTryReadAsUTF8) {
				CString strUTF8 = Helpers::TryConvertFromUTF8((uint8*)pString, nMaxChars);
				if (!strUTF8.IsEmpty()) {
					strOut = strUTF8;
					return;
				}
			}
			strOut = CString(pString, nMaxChars);
		} else {
			strOut.Empty();
		}
	} catch (...) {
		// EXIF corrupt?
		strOut.Empty();
	}
}

static void ReadUserCommentTag(CString & strOut, uint8* ptr, uint8* pTIFFHeader, const uint8* pTIFFEnd, bool bLittleEndian) {
	try {
		if (ptr != NULL && ReadUShort(ptr + 2, bLittleEndian) == 7) {
			int nSize = ReadUInt(ptr + 4, bLittleEndian);
			if (nSize > 10 && nSize <= 4096) {
				uint32 nOffset = ReadUInt(ptr + 8, bLittleEndian);
				if (!IsTIFFRangeValid(pTIFFHeader, pTIFFEnd, nOffset, (uint32)nSize)) {
					strOut.Empty();
					return;
				}
				LPCSTR sCodeDesc = (LPCSTR)(pTIFFHeader + nOffset);
				// F-19: bounded compare. strcmp would read past the validated nSize bytes looking for a NUL; nSize > 10 above guarantees the 8 bytes strncmp can touch are inside the range, and the NUL inside each literal makes the match exact ("ASCII" + NUL, "UNICODE" + NUL).
				if (strncmp(sCodeDesc, "ASCII", 8) == 0) {
					strOut = CString((LPCSTR)(sCodeDesc + 8), nSize - 8);
				} else if (strncmp(sCodeDesc, "UNICODE", 8) == 0) {
					bool bLE = bLittleEndian;
					nSize -= 8;
					sCodeDesc += 8;
					if (sCodeDesc[0] == 0xFF && sCodeDesc[1] == 0xFE) {
						bLE = true;
						nSize -= 2; sCodeDesc += 2;
					} else if (sCodeDesc[0] == 0xFE && sCodeDesc[1] == 0xFF) {
						bLE = false;
						nSize -= 2; sCodeDesc += 2;
					}
					if (bLE) {
						strOut = CString((LPCWSTR)sCodeDesc, nSize / 2);
					} else {
						// swap 16 bit characters to little endian
						char* pString = new char[nSize];
						char* pSwap = pString;
						memcpy(pString, sCodeDesc, nSize);
						for (int i = 0; i < nSize / 2; i++) {
							char t = *pSwap;
							*pSwap = pSwap[1];
							pSwap[1] = t;
							pSwap += 2;
						}
						strOut = CString((LPCWSTR)pString, nSize / 2);
						delete[] pString;
					}
				}
			} // else comment is corrupt or too big
		}
	} catch (...) {
		// EXIF corrupt?
		strOut.Empty();
	}
}

static uint32 ReadLongTag(uint8* ptr, bool bLittleEndian) {
	if (ptr != NULL && ReadUShort(ptr + 2, bLittleEndian) == 4) {
		return ReadUInt(ptr + 8, bLittleEndian);
	} else {
		return 0;
	}
}

static uint16 ReadShortTag(uint8* ptr, bool bLittleEndian) {
	if (ptr != NULL && ReadUShort(ptr + 2, bLittleEndian) == 3) {
		return ReadUShort(ptr + 8, bLittleEndian);
	} else {
		return 0;
	}
}

static uint32 ReadShortOrLongTag(uint8* ptr, bool bLittleEndian) {
	if (ptr != NULL) {
		uint16 nType = ReadUShort(ptr + 2, bLittleEndian);
		if (nType == 3) {
			return ReadUShort(ptr + 8, bLittleEndian);
		} else if (nType == 4) {
			return ReadUInt(ptr + 8, bLittleEndian);
		}
	}
	return 0;
}

static void WriteShortTag(uint8* ptr, uint16 nValue, bool bLittleEndian) {
	if (ptr != NULL && ReadUShort(ptr + 2, bLittleEndian) == 3) {
		WriteUShort(ptr + 8, nValue, bLittleEndian);
	}
}

static void WriteLongTag(uint8* ptr, uint32 nValue, bool bLittleEndian) {
	if (ptr != NULL && ReadUShort(ptr + 2, bLittleEndian) == 4) {
		WriteUInt(ptr + 8, nValue, bLittleEndian);
	}
}

// Absolute value of a uint32 holding a two's-complement int32, computed in
// the unsigned domain: abs(INT_MIN) is UB, but 0u - 0x80000000u == 2^31,
// which is exactly |INT_MIN|.
static uint32 AbsInt32AsUInt32(uint32 u) {
	return (u & 0x80000000u) != 0 ? 0u - u : u;
}

static int ReadRationalTag(Rational & rational, uint8* ptr, uint8* pTIFFHeader, const uint8* pTIFFEnd, int index, bool bLittleEndian) {
	rational.Numerator = 0;
	rational.Denominator = 0;
	if (ptr != NULL) {
		uint16 nType = ReadUShort(ptr + 2, bLittleEndian);
		// Do not truncate the count to uint16: a count above 65535 used to
		// wrap and let index pass the check with a bogus offset.
		uint32 nCount = ReadUInt(ptr + 4, bLittleEndian);
		if (index >= 0 && (uint32)index < nCount) {
			if (nType == 5 || nType == 10) {
				uint32 nBase = ReadUInt(ptr + 8, bLittleEndian);
				if (!IsTIFFRangeValid(pTIFFHeader, pTIFFEnd, nBase, (unsigned __int64)index * 8 + 8)) {
					return nType;
				}
				int nOffset = (int)(nBase + (uint32)index * 8);
				rational.Numerator = ReadUInt(pTIFFHeader + nOffset, bLittleEndian);
				rational.Denominator = ReadUInt(pTIFFHeader + nOffset + 4, bLittleEndian);
				if (rational.Numerator != 0 && rational.Denominator != 0) {
					// Calculate the ggT
					uint32 nModulo;
					// A3-7: nType 10 is SRATIONAL; abs((int)...) is UB for
					// 0x80000000 (INT_MIN), so take the absolute value in
					// the unsigned domain instead (see AbsInt32AsUInt32).
					uint32 nA = (nType == 10) ? AbsInt32AsUInt32(rational.Numerator) : rational.Numerator;
					uint32 nB = (nType == 10) ? AbsInt32AsUInt32(rational.Denominator) : rational.Denominator;
					do {
						nModulo = nA % nB;
						nA = nB;
						nB = nModulo;
					} while (nB != 0);
					// normalize
					if (nType == 10) {
						rational.Numerator = (int)rational.Numerator / (int)nA;
						rational.Denominator = (int)rational.Denominator / (int)nA;
					}
					else {
						rational.Numerator /= nA;
						rational.Denominator /= nA;
					}
				}
			}
		}
		return nType;
	}
	return 0;
}

static int ReadRationalTag(Rational & rational, uint8* ptr, uint8* pTIFFHeader, const uint8* pTIFFEnd, bool bLittleEndian) {
	return ReadRationalTag(rational, ptr, pTIFFHeader, pTIFFEnd, 0, bLittleEndian);
}

static bool ReadSignedRationalTag(SignedRational & rational, uint8* ptr, uint8* pTIFFHeader, const uint8* pTIFFEnd, bool bLittleEndian) {
	ReadRationalTag((Rational &)rational, ptr, pTIFFHeader, pTIFFEnd, bLittleEndian);
}

static double ReadDoubleTag(uint8* ptr, uint8* pTIFFHeader, const uint8* pTIFFEnd, int index, bool bLittleEndian) {
	Rational rational(0, 0);
	int nType = ReadRationalTag(rational, ptr, pTIFFHeader, pTIFFEnd, index, bLittleEndian);
	if (rational.Denominator != 0) {
		if (nType == 5) {
			return (double)rational.Numerator / rational.Denominator;
		} else {
			int nNum = rational.Numerator;
			int nDenum = rational.Denominator;
			return (double)nNum/nDenum;
		}
	}
	return CEXIFReader::UNKNOWN_DOUBLE_VALUE;
}

static double ReadDoubleTag(uint8* ptr, uint8* pTIFFHeader, const uint8* pTIFFEnd, bool bLittleEndian) {
	return ReadDoubleTag(ptr, pTIFFHeader, pTIFFEnd, 0, bLittleEndian);
}

bool CEXIFReader::ParseDateString(SYSTEMTIME & date, const CString& str) {
	int nYear, nMonth, nDay, nHour, nMin, nSec;
	if (_stscanf(str, _T("%d:%d:%d %d:%d:%d"), &nYear, &nMonth, &nDay, &nHour, &nMin, &nSec) == 6) {
		date.wYear = nYear;
		date.wMonth = nMonth;
		date.wDay = nDay;
		date.wHour = nHour;
		date.wMinute = nMin;
		date.wSecond = nSec;
		date.wMilliseconds = 0;
		date.wDayOfWeek = 0;
		return true;
	}
	return false;
}

CEXIFReader::CEXIFReader(void* pApp1Block, EImageFormat eImageFormat)
	: m_exposureTime{ 0, 0 },
	m_acqDate{ 0 },
	m_dateTime{ 0 }
{
	m_bFlashFired = false;
	m_bFlashFlagPresent = false;
	m_dFocalLength = m_dExposureBias = m_dFNumber = UNKNOWN_DOUBLE_VALUE;
	m_nISOSpeed = 0;
	m_nImageOrientation = 0;
	m_pTagOrientation = NULL;
	m_bLittleEndian = true;
	m_nThumbWidth = -1;
	m_nThumbHeight = -1;
	m_pLastIFD0 = NULL;
	m_pIFD1 = NULL;
	m_pLastIFD1 = NULL;
	m_bHasJPEGCompressedThumbnail = false;
	m_nJPEGThumbStreamLen = 0;
	m_pLatitude = NULL;
	m_pLongitude = NULL;
	m_dAltitude = UNKNOWN_DOUBLE_VALUE;

	m_pApp1 = (uint8*)pApp1Block;
	m_nApp1Size = 0;
	// APP1 marker
	if (m_pApp1[0] != 0xFF || m_pApp1[1] != 0xE1) {
		return;
	}
	int nApp1Size = m_pApp1[2]*256 + m_pApp1[3] + 2;
	m_nApp1Size = nApp1Size;
	// The TIFF header sits at offset 10 and is 8 bytes, so anything shorter is
	// not a real EXIF block. Without this a truncated APP1 read past the end of
	// the block below.
	if (nApp1Size < 18) {
		return;
	}

	// Read TIFF header
	uint8* pTIFFHeader = m_pApp1 + 10;
	// R-17: end of the APP1 block; all tag-relative reads below are bounded
	// against it (pTIFFHeader + (nApp1Size - 10)).
	const uint8* pTIFFEnd = m_pApp1 + nApp1Size;
	bool bLittleEndian;
	if (*(short*)pTIFFHeader == 0x4949) {
		bLittleEndian = true;
	} else if (*(short*)pTIFFHeader == 0x4D4D) {
		bLittleEndian = false;
	} else {
		return;
	}
	m_bLittleEndian = bLittleEndian;

	uint8* pIFD0 = pTIFFHeader + ReadUInt(pTIFFHeader + 4, bLittleEndian);
	if (pIFD0 - m_pApp1 >= nApp1Size - 1) {
		return;
	}
	// Read IFD0
	uint16 nNumTags = ReadUShort(pIFD0, bLittleEndian);
	pIFD0 += 2;
	uint8* pLastIFD0 = pIFD0 + nNumTags*12;
	if (pLastIFD0 - m_pApp1 + 4 >= nApp1Size) {
		return;
	}
	uint32 nOffsetIFD1 = ReadUInt(pLastIFD0, bLittleEndian);
	m_pLastIFD0 = pLastIFD0;

	// image orientation
	uint8* pTagOrientation = NULL;
	// orientation tags must be ignored for JXL and HEIF/AVIF
	if (eImageFormat != IF_JXL && eImageFormat != IF_HEIF && eImageFormat != IF_AVIF) {
		pTagOrientation = FindTag(pIFD0, pLastIFD0, 0x112, bLittleEndian);
	}
	if (pTagOrientation != NULL) {
		m_nImageOrientation = ReadShortTag(pTagOrientation, bLittleEndian);
	}
	m_pTagOrientation = pTagOrientation;

	uint8* pTagModel = FindTag(pIFD0, pLastIFD0, 0x110, bLittleEndian);
	ReadStringTag(m_sModel, pTagModel, pTIFFHeader, pTIFFEnd, bLittleEndian);

	uint8* pTagImageDesc = FindTag(pIFD0, pLastIFD0, 0x10E, bLittleEndian);
	ReadStringTag(m_sImageDescription, pTagImageDesc, pTIFFHeader, pTIFFEnd, bLittleEndian, true);

	// Add the manufacturer name if not contained in model name
	if (!m_sModel.IsEmpty()) {
		CString sMake;
		uint8* pTagMake = FindTag(pIFD0, pLastIFD0, 0x10F, bLittleEndian);
		ReadStringTag(sMake, pTagMake, pTIFFHeader, pTIFFEnd, bLittleEndian);
		if (!sMake.IsEmpty()) {
			int nSpace = sMake.Find(_T(' '));
			CString sMakeL(nSpace > 0 ? sMake.Left(nSpace) : sMake);
			if (m_sModel.Find(sMakeL) == -1) {
				m_sModel = sMakeL + _T(" ") + m_sModel;
			}
		}
	}

	uint8* pTagSoftware = FindTag(pIFD0, pLastIFD0, 0x0131, bLittleEndian);
	ReadStringTag(m_sSoftware, pTagSoftware, pTIFFHeader, pTIFFEnd, bLittleEndian);

	uint8* pTagModDate = FindTag(pIFD0, pLastIFD0, 0x0132, bLittleEndian);
	CString sModDate;
	ReadStringTag(sModDate, pTagModDate, pTIFFHeader, pTIFFEnd, bLittleEndian);
	ParseDateString(m_dateTime, sModDate);

	uint8* pTagEXIFIFD = FindTag(pIFD0, pLastIFD0, 0x8769, bLittleEndian);
	if (pTagEXIFIFD == NULL) {
		return;
	}

	uint8* pTagGPSIFD = FindTag(pIFD0, pLastIFD0, 0x8825, bLittleEndian);
	if (pTagGPSIFD != NULL) {
		ReadGPSData(pTIFFHeader, pTagGPSIFD, nApp1Size, bLittleEndian);
	}

	// Read EXIF IFD
	uint32 nOffsetEXIF = ReadLongTag(pTagEXIFIFD, bLittleEndian);
	if (nOffsetEXIF == 0) {
		return;
	}
	uint8* pEXIFIFD = pTIFFHeader + nOffsetEXIF;
	if (pEXIFIFD - m_pApp1 >= nApp1Size - 1) {
		return;
	}
	nNumTags = ReadUShort(pEXIFIFD, bLittleEndian);
	pEXIFIFD += 2;
	uint8* pLastEXIF = pEXIFIFD + nNumTags*12;
	if (pLastEXIF - m_pApp1 >= nApp1Size) {
		return;
	}
	uint8* pTagAcquisitionDate = FindTag(pEXIFIFD, pLastEXIF, 0x9003, bLittleEndian);
	CString sAcqDate;
	ReadStringTag(sAcqDate, pTagAcquisitionDate, pTIFFHeader, pTIFFEnd, bLittleEndian);
	ParseDateString(m_acqDate, sAcqDate);

	uint8* pTagExposureTime = FindTag(pEXIFIFD, pLastEXIF, 0x829A, bLittleEndian);
	ReadRationalTag(m_exposureTime, pTagExposureTime, pTIFFHeader, pTIFFEnd, bLittleEndian);

	uint8* pTagExposureBias = FindTag(pEXIFIFD, pLastEXIF, 0x9204, bLittleEndian);
	m_dExposureBias = ReadDoubleTag(pTagExposureBias, pTIFFHeader, pTIFFEnd, bLittleEndian);

	uint8* pTagFlash = FindTag(pEXIFIFD, pLastEXIF, 0x9209, bLittleEndian);
	uint16 nFlash = ReadShortTag(pTagFlash, bLittleEndian);
	m_bFlashFired = (nFlash & 1) != 0;
	m_bFlashFlagPresent = pTagFlash != NULL;

	uint8* pTagFocalLength = FindTag(pEXIFIFD, pLastEXIF, 0x920A, bLittleEndian);
	m_dFocalLength = ReadDoubleTag(pTagFocalLength, pTIFFHeader, pTIFFEnd, bLittleEndian);

	uint8* pTagFNumber = FindTag(pEXIFIFD, pLastEXIF, 0x829D, bLittleEndian);
	m_dFNumber = ReadDoubleTag(pTagFNumber, pTIFFHeader, pTIFFEnd, bLittleEndian);

	uint8* pTagISOSpeed = FindTag(pEXIFIFD, pLastEXIF, 0x8827, bLittleEndian);

	uint8* pTagISOSpeed2 = FindTag(pEXIFIFD, pLastEXIF, 0x8833, bLittleEndian);

	m_nISOSpeed = (pTagISOSpeed != NULL) ? ReadShortTag(pTagISOSpeed, bLittleEndian) :
		ReadLongTag(pTagISOSpeed2, bLittleEndian);

	uint8* pTagUserComment = FindTag(pEXIFIFD, pLastEXIF, 0x9286, bLittleEndian);
	ReadUserCommentTag(m_sUserComment, pTagUserComment, pTIFFHeader, pTIFFEnd, bLittleEndian);
	// Samsung Galaxy puts this useless comment into each JPEG, just ignore
	if (m_sUserComment == "User comments") {	
		m_sUserComment = "";
	}

	// https://exiv2.org/tags.html
	// uint8* pTagXPComment = FindTag(pIFD0, pLastIFD0, 0x9c9c, bLittleEndian);  // this is the XPComment tag to resolve this issue https://github.com/sylikc/jpegview/issues/72 , but I'm not sure how to decode it


	if (nOffsetIFD1 != 0) {
		m_pIFD1 = pTIFFHeader + nOffsetIFD1;
		if (m_pIFD1 - m_pApp1 >= nApp1Size - 1 || m_pIFD1 - m_pApp1 < 0) {
			return;
		}
		nNumTags = ReadUShort(m_pIFD1, bLittleEndian);
		m_pIFD1 += 2;
		uint8* pLastIFD1 = m_pIFD1 + nNumTags*12;
		if (pLastIFD1 - m_pApp1 >= nApp1Size) {
			return;
		}
		m_pLastIFD1 = pLastIFD1;
		uint8* pTagCompression = FindTag(m_pIFD1, pLastIFD1, 0x103, bLittleEndian);
		if (pTagCompression == NULL) {
			return;
		}
		if (ReadShortTag(pTagCompression, bLittleEndian) == 6) {
			// compressed
			uint8* pTagOffsetSOI = FindTag(m_pIFD1, pLastIFD1, 0x0201, bLittleEndian);
			uint8* pTagJPEGLen = FindTag(m_pIFD1, pLastIFD1, 0x0202, bLittleEndian);
			if (pTagOffsetSOI != NULL && pTagJPEGLen != NULL) {
				uint32 nOffsetSOI = ReadLongTag(pTagOffsetSOI, bLittleEndian);
				uint32 nJPEGBytes = ReadLongTag(pTagJPEGLen, bLittleEndian);
				// Both values come straight from the file and neither was bounded
				// against the block. A crafted offset made pTIFFHeader + nOffsetSOI
				// point outside the EXIF block and FindJPEGMarker then read from
				// it; a length larger than the block did the same on the far side.
				if (nOffsetSOI > (uint32)(m_nApp1Size - 10) ||
					nJPEGBytes > (uint32)(m_nApp1Size - 10) - nOffsetSOI) {
					return;
				}
				uint8* pSOI = pTIFFHeader + nOffsetSOI;
				uint8* pSOF = (uint8*) Helpers::FindJPEGMarker(pSOI, nJPEGBytes, 0xC0);
				// FindJPEGMarker only guarantees the marker bytes themselves. The
				// SOF body is read at fixed offsets 5..8, so the segment has to be
				// long enough: with a short or truncated thumbnail stream those
				// reads went past the end of the EXIF block.
				if (pSOF != NULL && (int)(pSOF - pSOI) + 9 <= (int)nJPEGBytes) {
					m_nThumbWidth = pSOF[7]*256 + pSOF[8];
					m_nThumbHeight = pSOF[5]*256 + pSOF[6];
					m_nJPEGThumbStreamLen = nJPEGBytes;
					m_bHasJPEGCompressedThumbnail = true;
				}
			}
		} else {
			// uncompressed
			uint8* pTagThumbWidth = FindTag(m_pIFD1, pLastIFD1, 0x0001, bLittleEndian);
			uint8* pTagThumbHeight = FindTag(m_pIFD1, pLastIFD1, 0x0101, bLittleEndian);
			if (pTagThumbWidth != NULL && pTagThumbHeight != NULL) {
				m_nThumbWidth = ReadShortOrLongTag(pTagThumbWidth, bLittleEndian);
				m_nThumbHeight = ReadShortOrLongTag(pTagThumbHeight, bLittleEndian);
			}
		}
	}
}

CEXIFReader::~CEXIFReader(void) {
	delete m_pLatitude;
	delete m_pLongitude;
}

void CEXIFReader::WriteImageOrientation(int nOrientation) {
	if (m_pTagOrientation != NULL && ImageOrientationPresent()) {
		WriteShortTag(m_pTagOrientation, nOrientation, m_bLittleEndian);
	}
}

void CEXIFReader::DeleteThumbnail() {
	if (m_pLastIFD0 != NULL) {
		WriteUInt(m_pLastIFD0, 0, m_bLittleEndian);
	}
}

void CEXIFReader::UpdateJPEGThumbnail(unsigned char* pJPEGStream, int nStreamLen, int nEXIFBlockLenCorrection, CSize sizeThumb) {
	if (!m_bHasJPEGCompressedThumbnail) {
		return;
	}
	// F-M13: the old code memcpy'd with no bound on nStreamLen, no
	// NULL check on the stream, and no check that the SOI target lies
	// inside the APP1 block. A missing SOI tag also made ReadLongTag
	// return 0, pointing the write at the TIFF header itself.
	if (pJPEGStream == NULL || nStreamLen < 0) {
		return;
	}
	uint8* pTagOffsetSOI = FindTag(m_pIFD1, m_pLastIFD1, 0x0201, m_bLittleEndian);
	uint8* pTagJPEGBytesEarly = FindTag(m_pIFD1, m_pLastIFD1, 0x0202, m_bLittleEndian);
	if (pTagOffsetSOI == NULL || pTagJPEGBytesEarly == NULL || m_pApp1 == NULL || m_nApp1Size <= 10) {
		return;
	}
	uint32 nOffsetSOI = ReadLongTag(pTagOffsetSOI, m_bLittleEndian);
	uint8* pTIFFHeader = m_pApp1 + 10;
	uint8* pSOI = pTIFFHeader + nOffsetSOI;
	// The write spans pSOI + 2 .. pSOI + 2 + nStreamLen, i.e. offsets
	// nOffsetSOI + 2 + nStreamLen inside the TIFF area. The caller
	// deliberately over-allocates the block (see the header comment:
	// SaveImage reserves 32000 extra bytes), and nEXIFBlockLenCorrection
	// is exactly that growth, so the bound is the original TIFF area
	// plus a positive correction. Computed in 64 bit so a huge
	// nStreamLen cannot wrap around and pass.
	unsigned __int64 nLimit = (unsigned __int64)(m_nApp1Size - 10) +
		(nEXIFBlockLenCorrection > 0 ? (unsigned __int64)nEXIFBlockLenCorrection : 0);
	if ((unsigned __int64)nOffsetSOI + 2 + (unsigned __int64)nStreamLen > nLimit) {
		return;
	}
	memcpy(pSOI + 2, pJPEGStream, nStreamLen);

	uint8* pTagJPEGBytes = pTagJPEGBytesEarly; // already NULL-checked above
	WriteLongTag(pTagJPEGBytes, nStreamLen + 2, m_bLittleEndian);
	int nNewApp1Size = m_pApp1[2]*256 + m_pApp1[3] + nEXIFBlockLenCorrection;
	m_pApp1[2] = nNewApp1Size >> 8;
	m_pApp1[3] = nNewApp1Size & 0xFF;
}

void CEXIFReader::ReadGPSData(uint8* pTIFFHeader, uint8* pTagGPSIFD, int nApp1Size, bool bLittleEndian) {
	if (nApp1Size < 10) {
		return;
	}
	const uint8* pTIFFEnd = pTIFFHeader + (nApp1Size - 10);
	uint32 nOffsetGPS = ReadLongTag(pTagGPSIFD, bLittleEndian);
	if (nOffsetGPS == 0) {
		return;
	}
	uint8* pGPSIFD = pTIFFHeader + nOffsetGPS;
	if (pGPSIFD - m_pApp1 >= nApp1Size - 1) {
		return;
	}
	int nNumTags = ReadUShort(pGPSIFD, bLittleEndian);
	pGPSIFD += 2;
	uint8* pLastGPS = pGPSIFD + nNumTags * 12;
	if (pLastGPS - m_pApp1 >= nApp1Size) {
		return;
	}

	uint8* pTagLatitudeRef = FindTag(pGPSIFD, pLastGPS, 0x1, bLittleEndian);
	if (pTagLatitudeRef == NULL)
		return;
	CString latitudeRef;
	ReadStringTag(latitudeRef, pTagLatitudeRef, pTIFFHeader, pTIFFEnd, bLittleEndian, false, 2);

	uint8* pTagLatitude = FindTag(pGPSIFD, pLastGPS, 0x2, bLittleEndian);
	m_pLatitude = ReadGPSCoordinate(pTIFFHeader, pTIFFEnd, pTagLatitude, latitudeRef, bLittleEndian);

	uint8* pTagLongitudeRef = FindTag(pGPSIFD, pLastGPS, 0x3, bLittleEndian);
	if (pTagLongitudeRef == NULL)
		return;
	CString longitudeRef;
	ReadStringTag(longitudeRef, pTagLongitudeRef, pTIFFHeader, pTIFFEnd, bLittleEndian, false, 2);

	uint8* pTagLongitude = FindTag(pGPSIFD, pLastGPS, 0x4, bLittleEndian);
	m_pLongitude = ReadGPSCoordinate(pTIFFHeader, pTIFFEnd, pTagLongitude, longitudeRef, bLittleEndian);

	uint8* pTagAltitude = FindTag(pGPSIFD, pLastGPS, 0x6, bLittleEndian);
	if (pTagAltitude != NULL) {
		m_dAltitude = ReadDoubleTag(pTagAltitude, pTIFFHeader, pTIFFEnd, bLittleEndian);
		uint8* pTagAltitudeRef = FindTag(pGPSIFD, pLastGPS, 0x5, bLittleEndian);
		if (pTagAltitudeRef != NULL && *(pTagAltitudeRef + 8) == 1) {
			m_dAltitude *= -1;
		}
	}
}

GPSCoordinate* CEXIFReader::ReadGPSCoordinate(uint8* pTIFFHeader, const uint8* pTIFFEnd, uint8* pTagLatOrLong, LPCTSTR reference, bool bLittleEndian) {
	if (pTagLatOrLong == NULL)
		return NULL;
	double dDeg = ReadDoubleTag(pTagLatOrLong, pTIFFHeader, pTIFFEnd, 0, bLittleEndian);
	double dMin = ReadDoubleTag(pTagLatOrLong, pTIFFHeader, pTIFFEnd, 1, bLittleEndian);
	double dSec = ReadDoubleTag(pTagLatOrLong, pTIFFHeader, pTIFFEnd, 2, bLittleEndian);
	if (dDeg == UNKNOWN_DOUBLE_VALUE || dMin == UNKNOWN_DOUBLE_VALUE || dSec == UNKNOWN_DOUBLE_VALUE)
		return NULL;

	return new GPSCoordinate(reference, dDeg, dMin, dSec);
}
