#include "stdafx.h"

#include "ICCProfileTransform.h"
#include "SettingsProvider.h"
#include <string.h>
#include <math.h>


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


void* ICCProfileTransform::GetsRGBProfile() {
	if (sRGBProfile == NULL) {
		// Use try-catch to avoid crash if lcms2.dll not present
		try {
			sRGBProfile = cmsCreate_sRGBProfile();
		} catch (...) {}
	}
	return sRGBProfile;
}

void* ICCProfileTransform::GetLabProfile() {
	if (LabProfile == NULL) {
		LabProfile = cmsCreateLab4Profile(cmsD50_xyY());
	}
	return LabProfile;
}

void* ICCProfileTransform::GetCMYKProfile() {
	if (CMYKProfile == NULL) {
		CHAR path[MAX_PATH + 16];
		DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
		if (length > 0 && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			PathRemoveFileSpecA(path);
			lstrcatA(path, "\\cmyk.icm");
			CMYKProfile = cmsOpenProfileFromFile(path, "r");
		}
	}
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

bool ICCProfileTransform::IsNoTransformNeeded(const void* profile, unsigned int size)
{
	if (profile == NULL || size == 0)
		return true;
	cmsHPROFILE hProfile = cmsOpenProfileFromMem(profile, size);
	if (hProfile == NULL)
		return true; // unreadable profile: nothing to apply
	bool bSkip;
	if (cmsGetColorSpace(hProfile) != cmsSigRgbData) {
		// A gray or CMYK profile, while every caller here hands over RGB pixels.
		// lcms2 would refuse the transform, and refusing here is cheaper.
		bSkip = true;
	} else {
		// Only skip when the profile really is sRGB. The description alone was not
		// enough: a wide gamut profile whose description happens to contain "sRGB"
		// was skipped and its colors went unconverted (wrong colors). Verify the
		// colorant matrix as well. A genuinely sRGB profile with unusual numbers
		// merely gets a harmless identity transform, which is the safe direction.
		char text[128] = { 0 };
		cmsGetProfileInfoASCII(hProfile, cmsInfoDescription, "en", "US", text, (cmsUInt32Number) sizeof(text) - 1);
		bSkip = false;
		if ((strstr(text, "sRGB") != NULL) || (strstr(text, "srgb") != NULL)) {
			const cmsCIEXYZ* pRed = (const cmsCIEXYZ*)cmsReadTag(hProfile, cmsSigRedColorantTag);
			const cmsCIEXYZ* pGreen = (const cmsCIEXYZ*)cmsReadTag(hProfile, cmsSigGreenColorantTag);
			const cmsCIEXYZ* pBlue = (const cmsCIEXYZ*)cmsReadTag(hProfile, cmsSigBlueColorantTag);
			// sRGB colorants in the D50-adapted ICC PCS. The tolerance accepts vendor
			// variants but stays far from Adobe RGB and Display P3, whose red
			// primaries differ by more than 0.1.
			const double dTolerance = 0.05;
			bSkip = pRed != NULL && pGreen != NULL && pBlue != NULL
				&& fabs(pRed->X - 0.4361) < dTolerance && fabs(pRed->Y - 0.2225) < dTolerance && fabs(pRed->Z - 0.0139) < dTolerance
				&& fabs(pGreen->X - 0.3851) < dTolerance && fabs(pGreen->Y - 0.7169) < dTolerance && fabs(pGreen->Z - 0.0971) < dTolerance
				&& fabs(pBlue->X - 0.1431) < dTolerance && fabs(pBlue->Y - 0.0606) < dTolerance && fabs(pBlue->Z - 0.7141) < dTolerance;
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
