/*
 * ReSTIR DI, step 2: spatial reuse and shading.
 *
 * This pixel's reservoir from the temporal pass is combined with those of
 * a few pixels in a disk around it that see (nearly) the same surface, with
 * pairwise MIS as in restir.sh, but on the full target functions: a light
 * sample is the same point for every pixel, so their targets compare
 * directly. The canonical sample's pair weights include its visibility from
 * each neighbour (their stored samples are all visible from them); a
 * neighbour's sample is tested from here once, if it is selected.
 *
 * The result is shaded and written as diffuse and specular radiance for the
 * composite pass. The diffuse part also feeds the GI passes, as the light
 * the sample points they hit reflect. It is not stored for reuse.
 */

#include <bgfx_compute.sh>
#include "shaderlib.sh"
#include "compute.sh"
#include "di.sh"

SAMPLER2D(s_reservoirSample, 4);
SAMPLER2D(s_reservoirReceiver, 5);
SAMPLER2D(s_reservoirNormal, 6);

uniform vec4 u_screenSize; // xy: size, zw: texel size

#define kSpatialRadius 16.0 // pixels

void diSpatial(vec2 v_texcoord0, ivec2 _pixel, out vec4 o_diffuse, out vec4 o_specular)
{
	Surface surface = loadSurface(v_texcoord0);
	vec4 ownSample = texture2DLod(s_reservoirSample, v_texcoord0, 0.0);
	vec4 ownReceiver = texture2DLod(s_reservoirReceiver, v_texcoord0, 0.0);
	vec4 ownNormal = texture2DLod(s_reservoirNormal, v_texcoord0, 0.0);
	if (!surface.valid || lightCount() == 0)
	{
		o_diffuse = vec4_splat(0.0);
		o_specular = vec4_splat(0.0);
		return;
	}

	uvec2 pixel = uvec2(_pixel);
	uint rng = pcg(pixel.x + pcg(pixel.y + pcg(uint(u_frameInfo.x) ^ 0x9e3779b9u)));

	int taps = int(u_diInfo.z + 0.5);
	float canonicalCount = max(min(ownNormal.w, kDiMaxHistory), 1.0);
	int canonicalLight = ownSample.w >= 0.0 ? int(ownSample.w + 0.5) : -1;
	vec3 canonicalPoint = ownSample.xyz;
	float canonicalTarget = canonicalLight >= 0 && ownReceiver.w > 0.0
		? lightTarget(surface, canonicalLight, canonicalPoint)
		: 0.0;

	// Pairwise MIS, accumulated as in restir.sh (without the 1 / M_total,
	// applied at the end).
	float pairCanonical = canonicalCount / float(max(taps, 1));
	float totalCount = canonicalCount;
	float canonicalShare = taps > 0 ? 0.0 : canonicalCount;
	float weightSum = 0.0;
	vec3 chosenPoint = canonicalPoint;
	int chosenLight = -1;
	float chosenTarget = 0.0;
	bool neighbourSelected = false;

	for (int tap = 0; tap < taps; ++tap)
	{
		float radius = kSpatialRadius * sqrt(nextRandom(rng));
		float angle = 2.0 * kPi * nextRandom(rng);
		float randomSelect = nextRandom(rng);
		vec2 uv = v_texcoord0 + vec2(cos(angle), sin(angle)) * radius * u_screenSize.zw;
		Surface neighbour = loadSurface(uv);
		vec4 sampleData = texture2DLod(s_reservoirSample, uv, 0.0);
		vec4 receiver = texture2DLod(s_reservoirReceiver, uv, 0.0);
		vec4 normal = texture2DLod(s_reservoirNormal, uv, 0.0);
		int light = sampleData.w >= 0.0 ? int(sampleData.w + 0.5) : -1;
		if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || !neighbour.valid
		||  !sameSurface(surface.position, surface.normal, neighbour.position, neighbour.normal, 4.0 * kDiReceiverRadius))
		{
			canonicalShare += pairCanonical;
			continue;
		}

		float neighbourCount = min(normal.w, kDiMaxHistory);
		float pairWeight = neighbourCount + pairCanonical;
		totalCount += neighbourCount;

		// The canonical sample: how likely could the neighbour have made it?
		if (canonicalTarget > 0.0)
		{
			float neighbourTarget = 0.0;
			if (lightVisible(neighbour, canonicalPoint))
			{
				neighbourTarget = lightTarget(neighbour, canonicalLight, canonicalPoint);
			}
			float canonicalDensity = pairCanonical * canonicalTarget;
			float denominator = neighbourCount * neighbourTarget + canonicalDensity;
			canonicalShare += pairWeight * canonicalDensity / denominator;
		}

		// The neighbour's sample, as seen from here.
		if (light >= 0 && light < lightCount() && receiver.w > 0.0)
		{
			float target = lightTarget(surface, light, sampleData.xyz);
			float neighbourTarget = lightTarget(neighbour, light, sampleData.xyz);
			float neighbourDensity = neighbourCount * neighbourTarget;
			float denominator = neighbourDensity + pairCanonical * target;
			float weight = denominator > 0.0
				? pairWeight * neighbourDensity / denominator * target * receiver.w
				: 0.0;
			weightSum += weight;
			if (weight > 0.0 && randomSelect * weightSum < weight)
			{
				chosenPoint = sampleData.xyz;
				chosenLight = light;
				chosenTarget = target;
				neighbourSelected = true;
			}
		}
	}

	if (canonicalTarget > 0.0)
	{
		float weight = canonicalShare * canonicalTarget * ownReceiver.w;
		weightSum += weight;
		if (nextRandom(rng) * weightSum < weight)
		{
			chosenPoint = canonicalPoint;
			chosenLight = canonicalLight;
			chosenTarget = canonicalTarget;
			neighbourSelected = false;
		}
	}

	float combinedWeight = chosenTarget > 0.0 ? weightSum / (totalCount * chosenTarget) : 0.0;
	if (combinedWeight > 0.0 && neighbourSelected)
	{
		if (!lightVisible(surface, chosenPoint))
		{
			combinedWeight = 0.0;
		}
	}

	vec3 diffuse;
	vec3 specular;
	evaluateLightSample(surface, chosenLight, chosenPoint, diffuse, specular);
	o_diffuse = vec4(diffuse * combinedWeight, 1.0);
	o_specular = vec4(specular * combinedWeight, 1.0);
}

// A compute shader, in groups of 8x8 pixels (compute.sh), so that the BVH
// traversal keeps its stack in shared memory (bvh.sh).
IMAGE2D_WO(o_diffuseImage, rgba16f, 7);
IMAGE2D_WO(o_specularImage, rgba16f, 8);

NUM_THREADS(kGroupSize, kGroupSize, 1)
void main()
{
	g_bvhThread = int(gl_LocalInvocationIndex);
	ivec2 pixel = computePixel(gl_GlobalInvocationID);
	if (!insideImage(pixel, u_screenSize.xy))
	{
		return;
	}

	vec4 diffuse;
	vec4 specular;
	diSpatial((vec2(pixel) + 0.5) * u_screenSize.zw, pixel, diffuse, specular);
	imageStore(o_diffuseImage, pixel, diffuse);
	imageStore(o_specularImage, pixel, specular);
}
