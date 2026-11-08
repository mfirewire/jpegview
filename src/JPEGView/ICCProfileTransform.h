#pragma once

class ICCProfileTransform
{
public:
	enum PixelFormat {
		FORMAT_BGRA,
		FORMAT_RGBA,
		FORMAT_BGR,
		FORMAT_RGB,
		FORMAT_Lab,
		FORMAT_LabA,
		FORMAT_YMCK,
		FORMAT_AKYMC,
	};

	// Create a transform from given ICC Profile to standard sRGB color space.
	static void* CreateTransform(
		const void* profile, // pointer to ICC profile
		unsigned int size, // size of ICC profile in bytes
		PixelFormat format // format of input pixels
	);

	// Apply color transform to image. Returns true on success, false otherwise.
	static bool DoTransform(
		void* transform, // ICCP transform
		const void* inputBuffer, // 4-byte BGRA or RGBA input depending on transform pixel format
		void* outputBuffer, // 4-byte BGRA output
		unsigned int width, // width of image in pixels
		unsigned int height, // height of image in pixels
		unsigned int stride=0, // number of bytes per row of pixels in the input, only needed if not equal to width * 4
		unsigned int strideOut=0 // S-L2: bytes per output row. 0 derives it from the transform's output format (the historical behaviour); callers with padded output rows must pass it explicitly
	);

	// Free memory associated with the given transform
	static void DeleteTransform(void* transform);

	// True when handing this profile to CreateTransform would change nothing:
	// it already describes sRGB, it cannot be parsed, or it describes something
	// other than RGB data while the caller's pixels are RGB. Cameras and phones
	// mostly embed sRGB, and skipping those keeps color management free for the
	// common case.
	static bool IsNoTransformNeeded(const void* profile, unsigned int size);

private:
	static const int FLAGS;

	static void* sRGBProfile;
	static void* LabProfile;
	static void* CMYKProfile;

	static void* GetsRGBProfile();
	static void* GetLabProfile();
	static void* GetCMYKProfile();

};
