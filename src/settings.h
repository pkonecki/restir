#pragma once

// Everything the settings window changes, and the choices it offers. The
// renderer reads them each frame.

#include <bx/bx.h>

#include <cstdint>

// GI render target sizes offered in the settings window, as divisors of the
// window size.
struct GiResolution
{
	const char* m_name;
	uint32_t m_divisor;
};

constexpr GiResolution kGiResolutions[] =
{
	{ "Full",    1 },
	{ "Half",    2 },
	{ "Quarter", 4 },
};

// Point light shadows: texels per side of each of the six cube faces.
constexpr uint16_t kShadowFaceSizes[] = { 256, 512, 1024, 2048 };

// Denoiser a-trous iterations, with tap spacings 1, 2, 4, 8, 16.
constexpr int kMaxAtrousIterations = 5;

// The defaults are what the example was tuned with.
struct Settings
{
	bool multipleBounces = true;
	bool shadows = true;
	bool denoiser = true;
	bool worldSpaceReuse = true;
	bool vsync = true;

	// Performance levers.
	int giResolution = 1;     // index into kGiResolutions
	int shadowFaceSize = 1;   // index into kShadowFaceSizes
	int atrousIterations = kMaxAtrousIterations;
	int initialCandidates = 2; // rays traced per pixel per frame
	int temporalTaps = 2;      // reservoirs reused from last frame
	int spatialTaps = 4;       // reservoirs reused from this frame, one visibility ray each
	int traceSteps = 32;       // depth buffer steps per ray, over the same distance
};
