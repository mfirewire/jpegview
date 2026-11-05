// Exported methods of the WICLoader DLL

#include "stdafx.h"
#include "MaxImageDef.h"

#pragma comment(lib, "WindowsCodecs.lib")


#define IFS(fn)                \
{                              \
	if (SUCCEEDED(hr)) {       \
		hr = (fn);             \
	}                          \
}

#define RELEASE_INTERFACE(pi)  \
{                              \
	if (pi) {                  \
		pi->Release();         \
		pi = NULL;             \
	}                          \
}


// forward declaration
static HRESULT CopyWICBitmapToBuffer(IWICBitmapSource *piBitmapSource, byte* pBuffer);

typedef byte* Allocator(size_t sizeInBytes);
typedef void Deallocator(byte* buffer);

// Checks if WIC is installed by asking for the WIC imaging factory.
// WIC is integral part of Windows Vista and Windows 7 and can be installed for XP
__declspec(dllexport) bool __stdcall WICPresent(void) {
	bool wicPresent = false;
	IWICImagingFactory *piImagingFactory = NULL;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (LPVOID*) &piImagingFactory);
	if (SUCCEEDED(hr)) {
		wicPresent = true;
	}
	RELEASE_INTERFACE(piImagingFactory);

	return wicPresent;
}

// Loads the given image file with WIC. Returns NULL if loading fails. Memory is allocated and deallocated by the
// given function pointers be decouple from CRT version
__declspec(dllexport) byte* __stdcall LoadImageWithWIC(LPCWSTR fileName, Allocator* allocator, Deallocator* deallocator,
	unsigned int* width, unsigned int* height) {

	IWICImagingFactory *piImagingFactory = NULL;
	IWICBitmapDecoder *piDecoder = NULL;
	IWICBitmapFrameDecode *piBitmapFrame = NULL;
	UINT frameCount = 0;
	HRESULT hr = S_OK;

	IFS(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (LPVOID*) &piImagingFactory));
	IFS(piImagingFactory->CreateDecoderFromFilename(fileName, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &piDecoder));
	IFS(piDecoder->GetFrameCount(&frameCount));

	byte* bitmapBuffer = NULL;

	if (frameCount > 0) {
		IFS(piDecoder->GetFrame(0, &piBitmapFrame));
		if (SUCCEEDED(hr)) {
			piBitmapFrame->GetSize(width, height);
			if (*width <= MAX_IMAGE_DIMENSION && *height <= MAX_IMAGE_DIMENSION) {
				// F-15: byte cap before allocating. The pixel cap alone does not
				// bound the buffer: e.g. 40000x30000 passes it, but the 32-bit
				// CopyPixels size below would wrap. IsImageBufferSizeOk also
				// guarantees stride*height fits in a UINT, so the cast in
				// CopyWICBitmapToBuffer is safe.
				if ((double)*width * *height <= MAX_IMAGE_PIXELS &&
					IsImageBufferSizeOk((size_t)*width * 4, *height)) {
					bitmapBuffer = allocator((size_t)*width * *height * 4);
					if (bitmapBuffer != NULL) {
						hr = CopyWICBitmapToBuffer(piBitmapFrame, bitmapBuffer);
						if (!SUCCEEDED(hr)) {
							deallocator(bitmapBuffer);
							bitmapBuffer = NULL;
						}
					}
				}
			}
			piBitmapFrame->Release();
			piBitmapFrame = NULL;
		}
	}

	RELEASE_INTERFACE(piDecoder);
	RELEASE_INTERFACE(piImagingFactory);

	return bitmapBuffer;
}

static HRESULT CopyWICBitmapToBuffer(IWICBitmapSource *piBitmapSource, byte* targetBuffer) {
	HRESULT hr = S_OK;
	UINT width = 0;
	UINT height = 0;
	IWICImagingFactory *piImagingFactory = NULL;
	IWICFormatConverter *piFormatConverter = NULL;

	IFS(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (LPVOID*) &piImagingFactory));
	IFS(piImagingFactory->CreateFormatConverter(&piFormatConverter));
	// Colour management here is WIC's, not ours. IWICFormatConverter::Initialize
	// applies the source bitmap's embedded colour context when it converts to
	// 32bppBGR, even though no destination context is passed. That is deliberate
	// and verified: a wide-gamut image forced through this path renders identically
	// with and without an ICC profile under UseEmbeddedColorProfiles. Do not swap
	// this for a conversion path that drops the source colour context.
	// S-M5 (decided): the WIC fallback does no color management of its own and
	// is NOT governed by the UseEmbeddedColorProfiles switch. JPEGView applies
	// no embedded profile and no cmyk.icm on this path; WIC's own implicit use
	// of the source colour context (see above) is WIC's behaviour, and it is
	// likewise outside the switch's control. Do not describe this path as
	// color-managed by JPEGView.
	IFS(piFormatConverter->Initialize(piBitmapSource, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom));
	IFS(piFormatConverter->GetSize(&width, &height));

	if (SUCCEEDED(hr)) {
		UINT stride = width * 4;
		WICRect rc = { 0, 0, (int)width, (int)height };
		// F-15: compute the buffer size in size_t first. stride * height in
		// 32-bit UINT wraps for large images (e.g. 40000x30000 -> 4.8 GB wraps
		// to ~0.5 GB) and CopyPixels would then write past the buffer. The
		// IsImageBufferSizeOk gate in LoadImageWithWIC guarantees the size
		// fits in a UINT, so this cast is safe.
		size_t cbBufferSize = (size_t)stride * height;
		IFS(piFormatConverter->CopyPixels(&rc, stride, (UINT)cbBufferSize, targetBuffer));
	}

	RELEASE_INTERFACE(piFormatConverter);
	RELEASE_INTERFACE(piImagingFactory);

	return hr;
}


 
