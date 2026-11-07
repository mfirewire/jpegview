#include "stdafx.h"

#include "SVGWrapper.h"
// ThorVG 1.1.3, vendored under libthorvg/ (headers from the 1.1.3 release
// tarball; libthorvg-1.dll is built from that source with MinGW, see
// libthorvg/README-THORVG.txt). Only the C API header is used:
// thorvg_capi.h. That is deliberate, not incidental -- the C++ API
// (thorvg.h) cannot be linked from MSVC against a MinGW-built DLL
// (Itanium vs MSVC name mangling / ABI), while the C API's plain-C
// exports link fine through a dlltool-generated import library.
// libthorvg-1.dll is delay-loaded via the vcxproj DelayLoadDLLs entry, so the
// DLL is only loaded when an SVG file is actually opened.
#include "thorvg_capi.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "Helpers.h"
#include <new>
#include <float.h> // _finite: R-13, reject non-finite SVG sizes

namespace {

// The <svg> tag is normally near the start of the file, but an XML
// declaration, DOCTYPE or comments may precede it; scan the whole buffer
// for it. Everything here is bounds-checked: SVG data is not NUL
// terminated, so no strstr/strlen over the raw buffer.
const char* FindBytes(const char* buf, size_t len, const char* needle, size_t needleLen) {
	if (needleLen == 0 || len < needleLen)
		return NULL;
	for (size_t i = 0; i + needleLen <= len; i++) {
		if (memcmp(buf + i, needle, needleLen) == 0)
			return buf + i;
	}
	return NULL;
}

bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Parse one floating-point number at *pp (bounded by end). Returns false
// if there is no number there. Accepts an optional sign and decimal point;
// exponents are not expected in SVG lengths and are rejected.
bool ParseFloat(const char*& p, const char* end, float& value) {
	if (p >= end)
		return false;
	const char* start = p;
	if (*p == '+' || *p == '-')
		p++;
	bool anyDigit = false;
	while (p < end && *p >= '0' && *p <= '9') {
		p++;
		anyDigit = true;
	}
	if (p < end && *p == '.') {
		p++;
		while (p < end && *p >= '0' && *p <= '9') {
			p++;
			anyDigit = true;
		}
	}
	if (!anyDigit) {
		p = start;
		return false;
	}
	// strtof needs NUL termination; copy the token (at most 32 chars).
	char token[32];
	size_t n = (size_t)(p - start);
	if (n >= sizeof(token))
		return false;
	memcpy(token, start, n);
	token[n] = '\0';
	value = (float)strtod(token, NULL);
	return true;
}

// Parse one attribute (name="value" or name='value') inside the <svg ...>
// tag. The value must be a bare number or a number followed by "px";
// percentages and other units (pt/mm/cm/in/em/...) are NOT intrinsic pixel
// sizes and are reported as "not present". Returns false when the attribute
// is absent or has no usable pixel value.
bool ParseLengthAttr(const char* tag, size_t tagLen, const char* attrName, float& value) {
	size_t attrLen = strlen(attrName);
	const char* p = tag;
	const char* end = tag + tagLen;
	while (p < end) {
		const char* hit = FindBytes(p, (size_t)(end - p), attrName, attrLen);
		if (hit == NULL)
			return false;
		// The match must start an attribute name: the char before it is a
		// space (or the tag start), and the char after it is a space or '='.
		// This keeps "width" from matching inside e.g. "stroke-width".
		if (hit != tag && !IsSpace(hit[-1])) {
			p = hit + attrLen;
			continue;
		}
		const char* after = hit + attrLen;
		if (after < end && !IsSpace(*after) && *after != '=') {
			p = hit + attrLen;
			continue;
		}
		p = after;
		while (p < end && IsSpace(*p))
			p++;
		if (p >= end || *p != '=')
			continue;
		p++;
		while (p < end && IsSpace(*p))
			p++;
		if (p >= end || (*p != '"' && *p != '\''))
			continue;
		char quote = *p;
		p++;
		float v = 0.0f;
		if (!ParseFloat(p, end, v))
			return false;
		// Only a bare number or a "px" suffix is an intrinsic pixel size.
		if (p < end && *p == '%')
			return false;
		if (p + 1 < end && p[0] == 'p' && p[1] == 'x')
			p += 2;
		else if (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
			return false; // some other unit (pt/mm/cm/in/em/...): not pixels
		if (p >= end || (*p != quote && !IsSpace(*p)))
			return false;
		value = v;
		return true;
	}
	return false;
}

// Parse the viewBox="minx miny w h" attribute; returns the w/h pair.
// A1-3: like ParseLengthAttr, scan every "viewBox" hit in the tag: a hit
// only starts an attribute when the char before it is a space, quote,
// '<' or the tag start (so "data-viewBox" is skipped), and a hit that
// fails to parse (e.g. inside an attribute value, where '=' does not
// follow) resumes the search at hit+1 instead of failing outright.
bool ParseViewBox(const char* tag, size_t tagLen, float& w, float& h) {
	const char* p = tag;
	const char* end = tag + tagLen;
	while (p < end) {
		const char* hit = FindBytes(p, (size_t)(end - p), "viewBox", 7);
		if (hit == NULL)
			return false;
		if (hit != tag && !IsSpace(hit[-1]) && hit[-1] != '"' && hit[-1] != '\'' && hit[-1] != '<') {
			p = hit + 1;
			continue;
		}
		const char* q = hit + 7;
		while (q < end && IsSpace(*q))
			q++;
		if (q >= end || *q != '=') {
			p = hit + 1;
			continue;
		}
		q++;
		while (q < end && IsSpace(*q))
			q++;
		if (q >= end || (*q != '"' && *q != '\'')) {
			p = hit + 1;
			continue;
		}
		q++;
		float vals[4];
		bool ok = true;
		for (int i = 0; i < 4; i++) {
			while (q < end && (IsSpace(*q) || *q == ','))
				q++;
			if (!ParseFloat(q, end, vals[i])) {
				ok = false;
				break;
			}
		}
		if (!ok) {
			p = hit + 1;
			continue;
		}
		w = vals[2];
		h = vals[3];
		return true;
	}
	return false;
}

// Intrinsic size of the SVG, in pixels. width/height attributes win;
// otherwise the viewBox size; otherwise a 512x512 default. A size is only
// accepted when both dimensions are finite and positive.
bool ParseSvgSize(const char* buf, size_t len, float& width, float& height) {
	const char* svgTag = FindBytes(buf, len, "<svg", 4);
	if (svgTag == NULL)
		return false;
	// The tag ends at the first '>' (bounded: at most 64KB in, an
	// <svg> tag with more than that is not a tag we can trust).
	const char* tagEnd = NULL;
	size_t scanLen = len - (size_t)(svgTag - buf);
	if (scanLen > 64 * 1024)
		scanLen = 64 * 1024;
	for (size_t i = 0; i < scanLen; i++) {
		if (svgTag[i] == '>') {
			tagEnd = svgTag + i;
			break;
		}
	}
	if (tagEnd == NULL)
		return false;
	size_t tagLen = (size_t)(tagEnd - svgTag);
	float w = 0.0f, h = 0.0f;
	if (ParseLengthAttr(svgTag, tagLen, "width", w) && ParseLengthAttr(svgTag, tagLen, "height", h) &&
		w > 0.0f && h > 0.0f && w == w && h == h) { // w == w rejects NaN
		width = w;
		height = h;
		return true;
	}
	if (ParseViewBox(svgTag, tagLen, w, h) && w > 0.0f && h > 0.0f && w == w && h == h) {
		width = w;
		height = h;
		return true;
	}
	// No usable size: fall back to a default instead of failing. A file
	// that got this far IS an SVG (the sniff found <svg>); refusing it
	// for missing dimensions would be worse than rendering it at a
	// sensible default.
	width = 512.0f;
	height = 512.0f;
	return true;
}

// Scale (w, h) down, preserving aspect, so it fits the image limits.
// Unlike the raster wrappers, an oversized SVG is CAPPED (rendered
// smaller), not rejected: vector art has no "real" pixel size, so
// scaling down loses nothing but resolution the viewer cannot use.
// R-13: returns false when the size is not finite, or does not stay
// finite after scaling. ThorVG parses the full SVG number syntax, so
// <svg width="1e999" ...> makes tvg_picture_get_size return Inf, which
// passes the tvgW == tvgW check; the old code then computed
// scale = maxDim / Inf = 0, w = Inf * 0 = NaN, and the w < 1.0f floor is
// false for NaN, so (int)(NaN + 0.5f) was UB (MSVC yields INT_MIN, only
// caught by luck by the width <= 0 check). Returning false fails closed
// on the existing NULL path instead.
bool CapRenderSize(float& w, float& h) {
	if (!_finite((double)w) || !_finite((double)h))
		return false;
	double maxDim = (double)MAX_IMAGE_DIMENSION;
	// R-04/P2-5: the cap is the live MaxImagePixelsMP setting; the downscale-instead-of-reject semantics are unchanged.
	double maxPixels = (double)CSettingsProvider::This().MaxImagePixels();
	double scale = 1.0;
	if (w > maxDim)
		scale = maxDim / (double)w;
	if (h * scale > maxDim) {
		double s2 = maxDim / (double)h;
		if (s2 < scale)
			scale = s2;
	}
	if (w * scale * h * scale > maxPixels) {
		double s3 = sqrt(maxPixels / ((double)w * (double)h));
		if (s3 < scale)
			scale = s3;
	}
	w = (float)((double)w * scale);
	h = (float)((double)h * scale);
	if (w < 1.0f)
		w = 1.0f;
	if (h < 1.0f)
		h = 1.0f;
	// R-13: the arithmetic above cannot produce a non-finite result from
	// finite inputs, but check anyway so a future change cannot reopen
	// the float -> int UB at the width = (int)(fWidth + 0.5f) site.
	if (!_finite((double)w) || !_finite((double)h))
		return false;
	return true;
}

#ifdef _WIN32
// Load the system font for this engine session, so <text> renders.
// The font must be (re-)loaded after every tvg_engine_init(): ThorVG
// releases all loaded font data when the engine is fully terminated
// (tvg_engine_term() dropping the init refcount to zero -- see the
// ThorVG tutorial: "the global font data loaded into ThorVG is
// automatically released when the ThorVG engine is terminated").
// A process-lifetime "loaded once" flag would therefore leave every
// render after the first init/term cycle without any font, and <text>
// silently disappears when an SVG is revisited. tvg_font_load()
// caches by path, so re-loading while the engine is still up (nested
// init, font still loaded) is a cheap no-op, not a duplicate load.
// If the font file is missing (stripped Windows, Wine without fonts),
// render without text rather than failing the whole image.
bool LoadSystemFont() {
	WCHAR windir[MAX_PATH];
	if (::GetWindowsDirectoryW(windir, MAX_PATH) == 0)
		return false;
	CString fontPath(windir);
	fontPath += _T("\\Fonts\\arial.ttf");
	// tvg_font_load takes a narrow path; convert (system paths are ASCII
	// in practice, and a failed conversion just skips the font).
	CT2A narrowPath(fontPath);
	tvg_font_load(narrowPath);
	return true;
}
#endif

// R-14 (revised): the process-wide lock used to serialize only
// init/term (+ the font load), on the theory that the init/term
// refcount keeps the engine alive while any thread is inside its
// init/term pair, so rendering itself needed no lock. That theory was
// disproven on real hardware: rapid paging through a 3-SVG folder
// crashed 9/9 in libthorvg-1.dll with 0xC0000005, i.e. concurrent
// renders on different load threads (CJPEGProvider spawns one
// CImageLoadThread per worker) corrupt ThorVG-internal state -- the
// init/term refcount alone does not make picture load / canvas draw
// thread-safe, and tvg_picture_load_data itself touches shared
// loader/font state. The lock therefore now serializes the WHOLE
// ThorVG session: engine init, font load, picture load, canvas draw
// and sync, picture/canvas teardown, and engine term, via the RAII
// SvgEngineGuard in ReadImage below. SVG renders on different threads
// are strictly sequential; the cost is one SVG at a time, which is
// correct -- a crash is worse than serialization. The lock is only
// ever held by one guard at a time and never nested (the old
// Enter/Leave around tvg_engine_term in the cleanup block is gone,
// the guard covers it), so it cannot deadlock. tvg_engine_init(0)
// still requests no ThorVG worker threads; rendering happens on the
// calling (load) thread.
struct SvgEngineLock {
	CRITICAL_SECTION cs;
	SvgEngineLock() { ::InitializeCriticalSection(&cs); }
	~SvgEngineLock() { ::DeleteCriticalSection(&cs); }
};

CRITICAL_SECTION& GetSvgEngineLock() {
	static SvgEngineLock lock; // thread-safe magic static (VS2015+)
	return lock.cs;
}

// RAII holder for GetSvgEngineLock(): Enter on construction, Leave on
// destruction, so every return/break path out of the guarded region
// releases the lock. Non-copyable: copying would Leave twice.
class SvgEngineGuard {
public:
	SvgEngineGuard() { ::EnterCriticalSection(&GetSvgEngineLock()); }
	~SvgEngineGuard() { ::LeaveCriticalSection(&GetSvgEngineLock()); }
private:
	SvgEngineGuard(const SvgEngineGuard&);
	SvgEngineGuard& operator=(const SvgEngineGuard&);
};

} // namespace

void* SVGReader::ReadImage(int& width,
	int& height,
	bool& outOfMemory,
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;
	if (buffer == NULL || sizebytes < 4)
		return NULL;
	const char* pText = (const char*)buffer;
	// SVG-only gate, before ThorVG ever sees the buffer: the file must
	// contain an <svg tag. SVG has no magic bytes (it is XML text), so
	// this sniff plus the .svg extension routing is the gate. This rejects
	// any non-SVG content misnamed .svg.
	if (FindBytes(pText, (size_t)sizebytes, "<svg", 4) == NULL)
		return NULL;

	// Intrinsic size first, capped before any render buffer is allocated.
	float fWidth = 0.0f, fHeight = 0.0f;
	if (!ParseSvgSize(pText, (size_t)sizebytes, fWidth, fHeight))
		return NULL;
	// R-13: a non-finite size (e.g. a huge digit string that overflows
	// strtod to Inf) fails closed here, before the engine is started.
	if (!CapRenderSize(fWidth, fHeight))
		return NULL;

	// ThorVG engine: init(0) = no worker threads, rendering happens on
	// the calling (load) thread. init/term are reference-counted and must
	// pair on the same thread, so every exit path below goes through the
	// single cleanup block. R-14: the SvgEngineGuard below serializes
	// the whole ThorVG session (init, font load, picture load, canvas
	// draw/sync, teardown, term) across load threads; it is constructed
	// here, after the lock-free size checks, and lives until ReadImage
	// returns, so the early `return NULL` on init failure and both
	// returns at the end all release it via its destructor. The
	// BGR conversion below also runs under the guard -- it touches no
	// ThorVG state, but keeping one scope avoids an early unlock site.
	bool bEngineInit = false;
	SvgEngineGuard svgGuard;
	bEngineInit = (tvg_engine_init(0) == TVG_RESULT_SUCCESS);
	if (bEngineInit) {
#ifdef _WIN32
		LoadSystemFont();
#endif
	}
	if (!bEngineInit)
		return NULL;
	Tvg_Canvas canvas = NULL;
	Tvg_Paint picture = NULL;
	uint32_t* pArgb = NULL;
	void* pPixelData = NULL;
	bool ok = false;
	do {
		picture = tvg_picture_new();
		if (picture == NULL)
			break;
		// copy=true: ThorVG copies the SVG text, so the caller's buffer
		// may be freed while the picture lives. mimetype "svg" selects
		// the SVG loader explicitly; rpath NULL means external resources
		// (<image> hrefs) are not resolved -- they render blank, see the
		// header note.
		if (tvg_picture_load_data(picture, pText, (uint32_t)sizebytes, "svg", NULL, true) != TVG_RESULT_SUCCESS)
			break;
		// ThorVG's own size for the loaded picture (from width/height or
		// viewBox). If it disagrees with our parse, trust ThorVG -- but
		// cap it the same way before allocating.
		float tvgW = 0.0f, tvgH = 0.0f;
		if (tvg_picture_get_size(picture, &tvgW, &tvgH) == TVG_RESULT_SUCCESS &&
			tvgW > 0.0f && tvgH > 0.0f && tvgW == tvgW && tvgH == tvgH) {
			fWidth = tvgW;
			fHeight = tvgH;
			// R-13: ThorVG's own size can be Inf (e.g. width="1e999");
			// a non-finite size breaks out to the cleanup block, which
			// releases the engine and returns NULL.
			if (!CapRenderSize(fWidth, fHeight))
				break;
		}
		width = (int)(fWidth + 0.5f);
		height = (int)(fHeight + 0.5f);
		if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
			break;
		}
		// Scale the picture to the (possibly capped) render size.
		// set_size keeps the aspect ratio, fitting inside w x h.
		if (tvg_picture_set_size(picture, fWidth, fHeight) != TVG_RESULT_SUCCESS)
			break;
		// Render target: 32-bit ARGB, tightly packed (stride == width, in
		// uint32 units -- NOTE the argument order: buffer, stride, w, h,
		// colorspace; stride comes BEFORE w/h, a known footgun).
		size_t argbStrideBytes = (size_t)width * 4;
		size_t argbBytes = 0;
		if (!IsImageBufferSizeOk(argbStrideBytes, (size_t)height, &argbBytes)) {
			outOfMemory = true;
			break;
		}
		pArgb = new(std::nothrow) uint32_t[(size_t)width * (size_t)height];
		if (pArgb == NULL) {
			outOfMemory = true;
			break;
		}
		memset(pArgb, 0, argbBytes);
		canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
		if (canvas == NULL)
			break;
		if (tvg_swcanvas_set_target(canvas, pArgb, (uint32_t)width, (uint32_t)width, (uint32_t)height, TVG_COLORSPACE_ARGB8888S) != TVG_RESULT_SUCCESS)
			break;
		if (tvg_canvas_add(canvas, picture) != TVG_RESULT_SUCCESS)
			break;
		// The canvas now owns the picture; do not destroy it separately.
		picture = NULL;
		if (tvg_canvas_draw(canvas, true) != TVG_RESULT_SUCCESS)
			break;
		if (tvg_canvas_sync(canvas) != TVG_RESULT_SUCCESS)
			break;
		ok = true;
	} while (false);

	if (ok) {
		// Convert ARGB8888S (straight alpha) to 3-byte BGR padded to a
		// 4-byte boundary, like QOIWrapper. The uint32 value is
		// 0xAARRGGBB, so on little-endian byte 0 is B, byte 1 is G, byte
		// 2 is R, byte 3 is A. Alpha is composited over white, so a
		// transparent pixel shows as white, not black.
		size_t padded_stride = (size_t)Helpers::DoPadding(width * 3, 4);
		size_t allocSize = 0;
		if (!IsImageBufferSizeOk(padded_stride, (size_t)height, &allocSize)) {
			outOfMemory = true;
			ok = false;
		} else {
			pPixelData = new(std::nothrow) unsigned char[allocSize];
			if (pPixelData == NULL) {
				outOfMemory = true;
				ok = false;
			}
		}
		if (ok) {
			unsigned char* pDst = (unsigned char*)pPixelData;
			memset(pDst, 0, allocSize);
			for (int y = 0; y < height; y++) {
				for (int x = 0; x < width; x++) {
					uint32_t v = pArgb[(size_t)y * (size_t)width + (size_t)x];
					unsigned char b = (unsigned char)(v & 0xFF);
					unsigned char g = (unsigned char)((v >> 8) & 0xFF);
					unsigned char r = (unsigned char)((v >> 16) & 0xFF);
					unsigned char a = (unsigned char)((v >> 24) & 0xFF);
					size_t dstIdx = (size_t)y * padded_stride + (size_t)x * 3;
					if (a == 255) {
						pDst[dstIdx] = b;
						pDst[dstIdx + 1] = g;
						pDst[dstIdx + 2] = r;
					} else if (a == 0) {
						pDst[dstIdx] = 255;
						pDst[dstIdx + 1] = 255;
						pDst[dstIdx + 2] = 255;
					} else {
						// Straight alpha over white, rounded.
						pDst[dstIdx] = (unsigned char)((b * a + 255 * (255 - a) + 127) / 255);
						pDst[dstIdx + 1] = (unsigned char)((g * a + 255 * (255 - a) + 127) / 255);
						pDst[dstIdx + 2] = (unsigned char)((r * a + 255 * (255 - a) + 127) / 255);
					}
				}
			}
		}
	}

	// Single cleanup: ownership of the picture transfers to the canvas on
	// a successful tvg_canvas_add (picture was nulled then), and the
	// canvas frees it on destroy. If add never happened, release the
	// picture here with tvg_paint_rel.
	if (picture != NULL)
		tvg_paint_rel(picture);
	if (canvas != NULL)
		tvg_canvas_destroy(canvas);
	delete[] pArgb;
	// R-14: paired with the init above; the SvgEngineGuard constructed
	// at the top of ReadImage already holds the lock here, so no
	// Enter/Leave is needed (and nesting one would be pointless --
	// CRITICAL_SECTION is re-entrant, but the guard is the single owner).
	tvg_engine_term();
	if (!ok) {
		delete[] (unsigned char*)pPixelData;
		return NULL;
	}
	return pPixelData;
}
