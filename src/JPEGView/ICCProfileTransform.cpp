#include "stdafx.h"

#include "ICCProfileTransform.h"
#include "SettingsProvider.h"
#include <string.h>
#include <math.h>
#include <mutex> // E3: std::call_once for the shared profiles


#ifndef WINXP

// This define is necessary for 32-bit builds to work, for some reason
#define CMS_DLL
#include "lcms2.h"
#define FLAGS (cmsFLAGS_BLACKPOINTCOMPENSATION|cmsFLAGS_COPY_ALPHA)
#define TYPE_LabA_8 (COLORSPACE_SH(PT_Lab)|EXTRA_SH(1)|CHANNELS_SH(3)|BYTES_SH(1))
#define TYPE_YMCK_8 (COLORSPACE_SH(PT_CMYK)|CHANNELS_SH(4)|BYTES_SH(1)|DOSWAP_SH(1)|SWAPFIRST_SH(1))
#define TYPE_AKYMC_8 (COLORSPACE_SH(PT_CMYK)|EXTRA_SH(1)|CHANNELS_SH(4)|BYTES_SH(1)|DOSWAP_SH(1))


void* ICCProfileTransform::sRGBProfile = NULL;
void* ICCProfileTransform::LabProfile = NULL;
void* ICCProfileTransform::CMYKProfile = NULL;
// E3 (S-L3): two decoder threads could enter the lazy initialisers at the
// same time and both create (and leak) a profile. Guard each one with
// call_once; the profiles are process-wide and never freed by design.
static std::once_flag s_sRGBProfileOnce, s_LabProfileOnce, s_CMYKProfileOnce;


void* ICCProfileTransform::GetsRGBProfile() {
	std::call_once(s_sRGBProfileOnce, []() {
		// Use try-catch to avoid crash if lcms2.dll not present
		try {
			sRGBProfile = cmsCreate_sRGBProfile();
		} catch (...) {}
	});
	return sRGBProfile;
}

void* ICCProfileTransform::GetLabProfile() {
	std::call_once(s_LabProfileOnce, []() {
		try {
			LabProfile = cmsCreateLab4Profile(cmsD50_xyY());
		} catch (...) {}
	});
	return LabProfile;
}

void* ICCProfileTransform::GetCMYKProfile() {
	std::call_once(s_CMYKProfileOnce, []() {
		try {
			CHAR path[MAX_PATH + 16];
		DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
		if (length > 0 && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			PathRemoveFileSpecA(path);
			lstrcatA(path, "\\cmyk.icm");
			CMYKProfile = cmsOpenProfileFromFile(path, "r");
		}
		} catch (...) {}
	});
	return CMYKProfile;
}

void* ICCProfileTransform::CreateTransform(const void* profile, unsigned int size, PixelFormat format)
{
	if (GetsRGBProfile() == NULL)
		return NULL; // lcms2.dll not found

	// Create transform from embedded profile to sRGB
	cmsUInt32Number inFormat, outFormat;
	switch (format) {
		case FORMAT_BGRA:
			inFormat = TYPE_BGRA_8;
			outFormat = TYPE_BGRA_8;
			break;
		case FORMAT_RGBA:
			inFormat = TYPE_RGBA_8;
			outFormat = TYPE_BGRA_8;
			break;
		case FORMAT_BGR:
			inFormat = TYPE_BGR_8;
			outFormat = TYPE_BGR_8;
			break;
		case FORMAT_RGB:
			inFormat = TYPE_RGB_8;
			outFormat = TYPE_BGR_8;
			break;
		case FORMAT_Lab:
			inFormat = TYPE_Lab_8;
			outFormat = TYPE_BGR_8;
			break;
		case FORMAT_LabA:
			inFormat = TYPE_LabA_8;
			outFormat = TYPE_BGRA_8;
			break;
		case FORMAT_YMCK:
			inFormat = TYPE_YMCK_8;
			outFormat = TYPE_BGR_8;
			break;
		case FORMAT_AKYMC:
			inFormat = TYPE_AKYMC_8;
			outFormat = TYPE_BGRA_8;
			break;
		default:
			return NULL;
	}
	cmsHPROFILE hEmbeddedProfile = NULL;
	if (profile != NULL && size > 0 && CSettingsProvider::This().UseEmbeddedColorProfiles()) {
		hEmbeddedProfile = cmsOpenProfileFromMem(profile, size);
	}
	cmsHPROFILE hInProfile = hEmbeddedProfile;
	if (hInProfile == NULL) {
		switch (T_COLORSPACE(inFormat)) {
			// Use our own profiles for CMYK/Lab PSDs
			case PT_CMYK:
				hInProfile = GetCMYKProfile();
				break;
			case PT_Lab:
				hInProfile = GetLabProfile();
				break;
			default:
				return NULL;
		}
	}

	cmsHTRANSFORM transform = cmsCreateTransform(hInProfile, inFormat, GetsRGBProfile(), outFormat, INTENT_RELATIVE_COLORIMETRIC, FLAGS);
	cmsCloseProfile(hEmbeddedProfile);
	return transform;
}

bool ICCProfileTransform::DoTransform(void* transform, const void* inputBuffer, void* outputBuffer, unsigned int width, unsigned int height, unsigned int stride) {
	unsigned int numPixels = width * height;
	if (transform == NULL || inputBuffer == NULL || outputBuffer == NULL || numPixels == 0)
		return false;

	cmsUInt32Number inFormat = cmsGetTransformInputFormat(transform);
	cmsUInt32Number bppIn = T_CHANNELS(inFormat) + T_EXTRA(inFormat);
	cmsUInt32Number outFormat = cmsGetTransformOutputFormat(transform);
	cmsUInt32Number bppOut = T_CHANNELS(outFormat) + T_EXTRA(outFormat);
	cmsUInt32Number bytesPerLineOut = Helpers::DoPadding(width * bppOut, 4);
	if (stride == 0)
		stride = width * bppIn;
	cmsDoTransformLineStride(transform, inputBuffer, outputBuffer, width, height, stride, bytesPerLineOut, stride * height, bytesPerLineOut * height);
	return true;
}

void ICCProfileTransform::DeleteTransform(void* transform)
{
	if (transform != NULL)
		cmsDeleteTransform(transform);
}

static double XYZDistance(const cmsCIEXYZ* p, double x, double y, double z) {
	const double dx = p->X - x;
	const double dy = p->Y - y;
	const double dz = p->Z - z;
	return sqrt(dx * dx + dy * dy + dz * dz);
}

bool ICCProfileTransform::IsNoTransformNeeded(const void* profile, unsigned int size)
{
	if (profile == NULL || size == 0)
		return true; // no profile at all: nothing to apply
	cmsHPROFILE hProfile = cmsOpenProfileFromMem(profile, size);
	if (hProfile == NULL) {
		// M-ICC1: an unreadable profile used to be silently skipped, exactly
		// like a verified sRGB profile, so a corrupt embedded profile
		// displayed unconverted with no trace. Do not skip it: return
		// false so the caller attempts the conversion (CreateTransform
		// will fail cleanly on the same bytes), and log the failure.
		OutputDebugString(_T("JPEGView: embedded ICC profile could not be opened; conversion attempted anyway.\n"));
		return false;
	}
	bool bSkip;
	if (cmsGetColorSpace(hProfile) != cmsSigRgbData) {
		// A gray or CMYK profile, while every caller here hands over RGB pixels.
		// lcms2 would refuse the transform, and refusing here is cheaper.
		bSkip = true;
	} else {
		// Only skip when the profile really is sRGB. The description alone was not
		// enough: a wide gamut profile whose description happens to contain "sRGB"
		// was skipped and its colors went unconverted (wrong colors). Verify the
		// colorant matrix and the white point as well. A genuinely sRGB profile
		// with unusual numbers merely gets a harmless identity transform, which
		// is the safe direction.
		char text[128] = { 0 };
		cmsGetProfileInfoASCII(hProfile, cmsInfoDescription, "en", "US", text, (cmsUInt32Number) sizeof(text) - 1);
		bSkip = false;
		if ((strstr(text, "sRGB") != NULL) || (strstr(text, "srgb") != NULL)) {
			const cmsCIEXYZ* pRed = (const cmsCIEXYZ*)cmsReadTag(hProfile, cmsSigRedColorantTag);
			const cmsCIEXYZ* pGreen = (const cmsCIEXYZ*)cmsReadTag(hProfile, cmsSigGreenColorantTag);
			const cmsCIEXYZ* pBlue = (const cmsCIEXYZ*)cmsReadTag(hProfile, cmsSigBlueColorantTag);
			const cmsCIEXYZ* pWhite = (const cmsCIEXYZ*)cmsReadTag(hProfile, cmsSigMediaWhitePointTag);
			// M-ICC1: reference values are sRGB in the D50-adapted ICC PCS;
			// the white point is D65 expressed in that PCS (0.9642, 1.0,
			// 0.8249). The tolerance is a Euclidean distance in XYZ per
			// primary (and for the white point), not nine independent
			// per-channel bounds: a profile passes only if each primary
			// and the white point lie within 0.05 of sRGB. That still
			// accepts vendor variants but stays far from Adobe RGB and
			// Display P3, whose red primaries differ by about 0.19.
			// Note the tolerance also admits a native-D50 white point,
			// which differs from the D65 value only in Z by 0.0002;
			// the check exists to catch a materially different white,
			// not to split D50 from D65.
			const double dTolerance = 0.05;
			bSkip = pRed != NULL && pGreen != NULL && pBlue != NULL && pWhite != NULL
				&& XYZDistance(pRed, 0.4361, 0.2225, 0.0139) < dTolerance
				&& XYZDistance(pGreen, 0.3851, 0.7169, 0.0971) < dTolerance
				&& XYZDistance(pBlue, 0.1431, 0.0606, 0.7141) < dTolerance
				&& XYZDistance(pWhite, 0.9642, 1.0, 0.8249) < dTolerance;
		}
	}
	cmsCloseProfile(hProfile);
	return bSkip;
}

#else

// stub out lcms2 methods in an elegant way in XP build, as per suggestion https://github.com/sylikc/jpegview/commit/4b62f07e2a147a04a5014a5711d159670162e799#commitcomment-102738193

void* ICCProfileTransform::CreateTransform(const void* /* profile */, unsigned int /* size */, PixelFormat /* format */) {
	return NULL;
}

bool ICCProfileTransform::DoTransform(void* /* transform */, const void* /* inputBuffer */, void* /* outputBuffer */, unsigned int /* width */, unsigned int /* height */, unsigned int /* stride */) {
	return false;
}

void ICCProfileTransform::DeleteTransform(void* /* transform */) { }

bool ICCProfileTransform::IsNoTransformNeeded(const void* /* profile */, unsigned int /* size */) {
	return true; // no lcms2 in the XP build, nothing to convert with
}

#endif
