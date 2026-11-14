/*  PnmScale.h - the one place that knows PNM white is maxval, not 255.
 *
 *  WHY THIS FILE EXISTS
 *  --------------------
 *  PNM defines white as maxval, not as 255, and stb_image ignores the field
 *  and hands the raw samples back. A legal maxval=15 PGM therefore used to
 *  display as a near-black image (sample 15 out of 255) instead of pure
 *  white, and a 16-bit maxval=4095 file came out at 15/255 for the same
 *  reason (B-PNM1: stb already shifted it down by >>8, so its decoded range
 *  is [0, maxval>>8], not [0, maxval]).
 *
 *  The scale is the identity for maxval == 255 and maxval == 65535, and it
 *  maps both endpoints exactly (0 -> 0 and maxval -> 255). Interior 16-bit
 *  values stay approximate because stb already dropped the low byte (e.g.
 *  maxval=4095, sample 2048 decodes to 8 and scales to 136 instead of 128).
 *
 *  The formula below is the released one, moved verbatim out of
 *  PPMWrapper.cpp so the Linux CI can exercise it without MFC:
 *  tests/loader_pnm_test.cpp decodes hand-written PNM samples with the
 *  vendored stb_image and asserts the scaled pixels. PPMWrapper.cpp is the
 *  only other caller; tests/check_pnm_scale.py fails the build if the
 *  arithmetic shows up anywhere else.
 */

#pragma once

#include <cstddef>

namespace PnmScale {

// Scales stb-decoded PNM samples from [0, maxval] to [0, 255] in place.
// nMaxVal is the PNM header maxval. A maxval of 255 or 65535 is the
// identity (no-op), as is any out-of-range value (<= 0 or > 65535); a null
// buffer or a zero count is also a no-op.
inline void ScaleSamples(unsigned char* pSamples, size_t nSamples, int nMaxVal) noexcept {
	if (pSamples == nullptr || nSamples == 0 || nMaxVal <= 0 || nMaxVal > 65535 ||
		nMaxVal == 255 || nMaxVal == 65535) {
		return;
	}
	const unsigned nDivisor = (nMaxVal < 255) ? (unsigned)nMaxVal : ((unsigned)nMaxVal >> 8);
	for (size_t i = 0; i < nSamples; i++) {
		pSamples[i] = (unsigned char)(((unsigned)pSamples[i] * 255u + nDivisor / 2u) / nDivisor);
	}
}

} // namespace PnmScale
