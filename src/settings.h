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

// Values of the optional clamps in the reservoir reuse (see shaders/restir.sh).
constexpr float kJacobianClamp = 10.0f;
constexpr float kOutlierClamp = 8.0f;

// Denoiser a-trous iterations, with tap spacings 1, 2, 4, 8, 16.
constexpr int kMaxAtrousIterations = 5;

// Tone mapping operators, in the order the settings window lists them.
// Keep in sync with shaders/tonemap.sh.
enum class Tonemapper : int
{
	Aces,
	Agx,
	Neutral,
	Reinhard,
	Clamp,
	Count
};

constexpr const char* kTonemapperNames[] =
{
	"ACES (fitted)",
	"AgX",
	"Khronos PBR Neutral",
	"Reinhard",
	"None (clamp)",
};
static_assert(BX_COUNTOF(kTonemapperNames) == size_t(Tonemapper::Count));

// How the GI passes trace rays. Keep in sync with shaders/bvh.sh.
enum class RayMode : int
{
	ScreenSpace, // the depth buffer only
	Hybrid,      // the depth buffer, then the BVH for the rays it cannot answer
	Bvh,         // the BVH for every ray and visibility test
	Count
};

constexpr const char* kRayModeNames[] =
{
	"Depth buffer only",
	"Depth buffer, then BVH",
	"BVH only",
};
static_assert(BX_COUNTOF(kRayModeNames) == size_t(RayMode::Count));

// How emissive surfaces and the extra point lights light the scene.
enum class DirectLighting : int
{
	ReStirDi, // sampled by ReSTIR DI with shadow rays; GI adds their bounces
	GiOnly,   // emitters only, through the GI rays that happen to hit them
	Count
};

constexpr const char* kDirectLightingNames[] =
{
	"ReSTIR DI",
	"Through GI only",
};
static_assert(BX_COUNTOF(kDirectLightingNames) == size_t(DirectLighting::Count));

// What the window shows. The first five are lighting, tone mapped like the
// final image; the others are data, shown as display colours. Keep in sync
// with shaders/fs_debug.sc.
enum class DebugView : int
{
	Final,
	DirectLight,
	IndirectLight,
	Irradiance,
	IndirectSpecular,
	Albedo,
	Normals,
	RoughnessMetallic,
	Emission,
	Occlusion,
	ReservoirCount,
	ReservoirWeight,
	HistoryLength,
	HashGridCells,
	SampleSource,
	Count
};

constexpr const char* kDebugViewNames[] =
{
	"Final image",
	"Direct light only",
	"Indirect light only",
	"GI irradiance",
	"Indirect specular only",
	"Base colour",
	"Normals",
	"Roughness (R), metallic (G)",
	"Emission",
	"Ambient occlusion",
	"Reservoir M",
	"Reservoir W",
	"History length",
	"Hash grid cells",
	"Sample source",
};
static_assert(BX_COUNTOF(kDebugViewNames) == size_t(DebugView::Count));

constexpr const char* kDebugViewHelp[] =
{
	"The composited, tone mapped image.",
	"The point light's direct lighting (PBR, shadowed) and the\nmaterials' emission, without GI.",
	"The ReSTIR GI result alone, times the diffuse albedo.",
	"The denoised GI before the albedo is multiplied in (demodulated),\nat GI resolution, upsampled.",
	"Glossy reflections alone (fs_specular.sc): one GGX ray per GI pixel,\naccumulated over time, times the specular albedo.",
	"The G-buffer's base colour.",
	"The normals the direct light is shaded with (normal maps\nincluded), as colours. The GI passes use the surface's own normal.",
	"The G-buffer's roughness in red and metallic in green.",
	"The emitted radiance of emissive materials (factor times texture),\ncompressed with Reinhard so that strong emitters stay readable.",
	"The materials' ambient occlusion textures (white where none), which\ndim the indirect light.",
	"Candidates behind each pixel's reservoir after the temporal pass,\nfrom 0 (blue) to the most it can reach (red).",
	"Unbiased contribution weight of each pixel's reservoir, on a log\nscale from 2^-8 (blue) to 2^8 (red); black where it is zero.",
	"Frames accumulated in each pixel's GI estimate, from 1 (blue: just\ndisoccluded, history rejected) to 16 (red).",
	"A colour per world-space hash grid cell (without the lookup jitter),\nbrighter when it lists more reservoirs, grey when empty.",
	"Where each pixel's reservoir sample came from: blue the depth buffer,\ncyan the BVH and on a visible surface, orange the BVH and off screen\nor hidden (shaded from its triangle), white the sky; black where W is\nzero.",
};
static_assert(BX_COUNTOF(kDebugViewHelp) == size_t(DebugView::Count));

// The sky: a gradient from the ground through the horizon to the zenith.
// It lights the scene through GI rays that the BVH confirms leave it, so
// the ray mode must use the BVH.
struct SkySettings
{
	bool enabled = true;    // set per model when one is loaded (see main.cpp)
	float zenith[3] = { 0.30f, 0.50f, 0.95f };
	float horizon[3] = { 0.85f, 0.88f, 0.92f };
	float ground[3] = { 0.32f, 0.29f, 0.26f };
	float intensity = 1.0f; // radiance scale of the colours above
};

// The point light. Its position is reset to the scene's default (see
// describeScene) whenever a model is loaded.
struct LightSettings
{
	float position[3] = { 0.0f, 0.0f, 0.0f };
	float color[3] = { 1.0f, 1.0f, 1.0f };
	float intensity = 1.0f; // times the default, which scales with the square of the scene size
	bool orbit = false;       // circle around the vertical axis through the scene's centre, at the light's height
	float orbitRadius = 0.35f; // in scene radii
	float orbitSpeed = 0.5f;   // radians per second

	// Extra point lights, scattered through the scene (ReSTIR DI only).
	int extraLights = 0;
	float extraIntensity = 0.3f; // each, relative to the main light's default
	int extraSeed = 1;
};

// The defaults are what the example was tuned with.
struct Settings
{
	LightSettings light;
	SkySettings sky;

	bool multipleBounces = true;
	bool shadows = true;
	bool denoiser = true;
	bool worldSpaceReuse = true;
	bool vsync = true;
	bool hideWindowInScreenshots = true;
	bool taa = true; // temporal anti-aliasing
	bool specularGi = true; // glossy reflections
	int referenceView = 0;  // with a reference: 0 the image, 1 the reference, 2 the error against it
	bool emissiveLighting = true; // emissive surfaces on screen light the scene through GI
	int rayMode = int(RayMode::Hybrid);
	int directLighting = int(DirectLighting::ReStirDi);

	// Biased shortcuts in the reservoir reuse (README.md, "Bias").
	bool clampJacobian = true;  // cap the reconnection Jacobian at kJacobianClamp (off: runaway, see restir.sh)
	bool clampOutliers = true;  // cap each pixel's estimate at kOutlierClamp (as the example always did)

	int debugView = int(DebugView::Final);

	// Exposure and tone mapping.
	int tonemapper = int(Tonemapper::Agx);
	float exposureEv = 0.0f;      // manual exposure, or compensation on top of auto exposure
	bool autoExposure = false;
	float adaptationSpeed = 1.5f; // per second: how fast auto exposure follows the scene

	// Performance levers.
	int giResolution = 1;     // index into kGiResolutions
	int shadowFaceSize = 1;   // index into kShadowFaceSizes
	int atrousIterations = kMaxAtrousIterations;
	int initialCandidates = 2; // rays traced per pixel per frame
	int temporalTaps = 2;      // reservoirs reused from last frame
	int spatialTaps = 4;       // reservoirs reused from this frame, one visibility ray each
	int traceSteps = 32;       // depth buffer steps per ray, over the same distance
	int diCandidates = 8;      // ReSTIR DI: fresh light samples per pixel per frame
	int diSpatialTaps = 3;     // ReSTIR DI: reservoirs reused from neighbouring pixels
};
