#pragma once

class SVGReader
{
public:
	// Renders an SVG file (.svg) from memory using ThorVG 1.1.3
	// (libthorvg-1.dll, delay-loaded: no cost when SVG is never opened).
	// Returns 3-byte BGR pixels padded to a 4-byte boundary, like QOIWrapper,
	// or NULL on failure. The intrinsic size is parsed from the <svg> tag
	// (width/height attributes, else the viewBox, else a 512x512 default)
	// and CAPPED to the image limits before any render buffer is allocated,
	// so an SVG declaring absurd dimensions renders scaled down instead of
	// allocating for them. ThorVG's own size for the loaded picture is
	// cross-checked and capped the same way. Text is rendered with the
	// system font (%WINDIR%\Fonts\arial.ttf, loaded at runtime); if the
	// font file is missing, text is skipped but the image still renders.
	// <image> external rasters and <foreignObject> are NOT supported in
	// this pass (ThorVG has no loader for them here; they render blank)
	// and are documented, not silently worked around.
	static void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		bool& outOfMemory, // set to true when no memory to read image
		const void* buffer, // memory address containing SVG data.
		int sizebytes); // size of SVG data.
};
