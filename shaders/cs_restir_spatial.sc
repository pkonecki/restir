/*
 * ReSTIR GI, step 2: spatial reuse and shading.
 *
 * This pixel's reservoir from the temporal pass is combined, with pairwise
 * MIS (restir.sh), with this frame's reservoirs of kSpatialTaps other
 * receivers:
 *
 *   - World-space reuse (the default): picked at random from this frame's
 *     hash grid cell around this receiver (see hashgrid.sh), each tap through
 *     its own jittered lookup.
 *   - Screen-space reuse: pixels in a disk around this one.
 *
 * The combined sample is shaded and handed to the denoiser. It is
 * deliberately not stored: feeding spatially merged results back would
 * compound correlation and visibility errors frame after frame.
 *
 * Outputs (images at stages 12 and 13):
 *
 *   0: rgb = temporally accumulated indirect estimate, demodulated (divided
 *            by the diffuse albedo), a = history length
 *   1: x   = mean luminance, y = mean squared luminance, z = history length
 *
 * The denoiser (fs_svgf_*.sc) filters output 0 using output 1 and the
 * receiver position and normal targets, and feeds its first filtered
 * iteration back as s_historyColor for the next frame. Demodulating keeps
 * albedo detail out of the filter: the composite pass multiplies the albedo
 * back in.
 */

#include <bgfx_compute.sh>
#include "shaderlib.sh"
#include "compute.sh"

SAMPLER2D(s_reservoirSample, 3);   // this frame's sample position + M
SAMPLER2D(s_reservoirReceiver, 4); // this frame's receiver position + W
SAMPLER2D(s_historyMoments, 8);    // last frame's luminance moments
SAMPLER2D(s_reservoirNormal, 10);  // this frame's receiver normal

#include "restir.sh"

#define kMaxAccumulation 16.0 // frames blended into the displayed estimate

// Last frame's accumulated GI and luminance moments at this receiver, from
// the four history texels around its reprojected position. Only texels that
// held the same surface (close to this receiver's tangent plane, similar
// normal) contribute, with bilinear weights. Bilinear rather than nearest:
// snapping to the nearest texel shifts the history by up to half a texel
// every frame under camera motion, which adds noise instead of averaging it.
bool reprojectHistory(vec2 _previousUv, vec3 _position, vec3 _normal, out vec3 _color, out vec3 _moments)
{
	_color = vec3_splat(0.0);
	_moments = vec3_splat(0.0);

	vec2 texelCoord = _previousUv * u_screenInfo.xy - 0.5;
	vec2 baseCoord = floor(texelCoord);
	vec2 blend = texelCoord - baseCoord;
	float weightSum = 0.0;
	for (int tapIndex = 0; tapIndex < 4; ++tapIndex)
	{
		vec2 offset = vec2(float(tapIndex & 1), float(tapIndex >> 1));
		vec2 uv = (baseCoord + offset + 0.5) * u_screenInfo.zw;
		if (!isOnScreen(uv))
		{
			continue;
		}

		vec3 receiverPosition = texture2DLod(s_historyReceiver, uv, 0.0).xyz;
		vec3 receiverNormal = texture2DLod(s_historyNormal, uv, 0.0).xyz;
		vec3 receiverOffset = receiverPosition - _position;
		if (dot(receiverNormal, receiverNormal) < 0.5
		||  dot(receiverOffset, receiverOffset) > kBounceRadius * kBounceRadius
		||  abs(dot(receiverOffset, _normal)) > kReceiverPlane
		||  dot(normalize(receiverNormal), _normal) < kReceiverNormal)
		{
			continue;
		}

		vec2 bilinear = mix(vec2_splat(1.0) - blend, blend, offset);
		float weight = bilinear.x * bilinear.y;
		_color += texture2DLod(s_historyColor, uv, 0.0).rgb * weight;
		_moments += texture2DLod(s_historyMoments, uv, 0.0).xyz * weight;
		weightSum += weight;
	}

	if (weightSum < 0.01)
	{
		return false;
	}

	_color /= weightSum;
	_moments /= weightSum;
	return true;
}

void restirSpatial(vec2 v_texcoord0, out vec4 o_color, out vec4 o_moments)
{
	float depth = texture2DLod(s_depth, v_texcoord0, 0.0).x;
	if (depth >= kSkyDepth)
	{
		o_color = vec4_splat(0.0);
		o_moments = vec4_splat(0.0);
		return;
	}

	vec3 position = reconstructWorldPosition(v_texcoord0, depth);
	vec3 normal = decodeNormal(v_texcoord0);
	vec3 albedo = diffuseAlbedo(v_texcoord0);

	vec2 pixel = giPixel(v_texcoord0);
	uint pixelIndex = giPixelIndex(pixel);
	// A different stream from the temporal pass's (salted).
	uint rng = pcg(uint(pixel.x) + pcg(uint(pixel.y) + pcg(uint(u_frameInfo.x) ^ 0x68e31da4u)));

	// Canonical reservoir: this pixel's, from the temporal pass. Its sample is
	// visible from here, or its W is zero.
	vec4 ownSample = texture2DLod(s_reservoirSample, v_texcoord0, 0.0);
	float canonicalWeight = texture2DLod(s_reservoirReceiver, v_texcoord0, 0.0).w;
	float canonicalCount = max(min(ownSample.w, kMaxHistory), 1.0);
	Reservoir canonical = emptyReservoir();
	vec3 canonicalContribution = vec3_splat(0.0);
	vec3 canonicalNormal = vec3(0.0, 0.0, 1.0);
	bool canonicalValid = false;
	if (canonicalWeight > 0.0)
	{
		canonicalValid = evaluateSample(position, normal, albedo, ownSample.xyz
			, texture2DLod(s_reservoirNormal, v_texcoord0, 0.0).w, canonicalContribution, canonicalNormal);
	}
	if (canonicalValid)
	{
		canonical.contribution = canonicalContribution;
		canonical.position = ownSample.xyz;
		canonical.normal = canonicalNormal;
		canonical.id = texture2DLod(s_reservoirNormal, v_texcoord0, 0.0).w;
		canonical.target = luminance(canonicalContribution);
	}

	float radius = reuseRadius(position);
	PairwiseMis mis = beginPairwiseMis(position, normal, canonical, canonicalCount, kSpatialTaps);
	for (int tapIndex = 0; tapIndex < kSpatialTaps; ++tapIndex)
	{
		float random0 = nextRandom(rng);
		float random1 = nextRandom(rng);
		float random2 = nextRandom(rng);
		float randomSelect = nextRandom(rng);

		vec2 neighbourUv = vec2_splat(-1.0);
		if (kWorldSpaceReuse)
		{
			uint entry = gridPickEntry(position, normal, random0, random1, random2);
			// Our own reservoir is the canonical one already.
			if (entry != kGridInvalid && entry != pixelIndex)
			{
				neighbourUv = giPixelUv(entry);
			}
		}
		else
		{
			float diskRadius = kSpatialRadius * sqrt(random0);
			float angle = 2.0 * kPi * random1;
			vec2 neighbourPixel = floor(pixel + vec2(cos(angle), sin(angle)) * diskRadius);
			if (neighbourPixel.x != pixel.x || neighbourPixel.y != pixel.y)
			{
				neighbourUv = (neighbourPixel + 0.5) * u_screenInfo.zw;
			}
		}

		NeighbourReservoir neighbour;
		if (!isOnScreen(neighbourUv)
		||  !acceptNeighbour(
			  texture2DLod(s_reservoirSample, neighbourUv, 0.0)
			, texture2DLod(s_reservoirReceiver, neighbourUv, 0.0)
			, texture2DLod(s_reservoirNormal, neighbourUv, 0.0)
			, position
			, normal
			, radius
			, neighbour
			))
		{
			skipNeighbour(mis);
			continue;
		}

		addNeighbour(mis, position, normal, albedo, neighbour, randomSelect);
	}

	float combinedWeight = finishPairwiseMis(mis, position, normal, canonical, canonicalWeight, nextRandom(rng));
	vec3 estimate = mis.combined.contribution * combinedWeight;
	if (kOutlierClamp > 0.0)
	{
		// Optional: rare bright samples are cut instead of averaged.
		estimate = min(estimate, vec3_splat(kOutlierClamp));
	}

	// Demodulate: divide out the diffuse albedo. Where it is zero (metals,
	// black) the estimate is zero too.
	vec3 irradiance = estimate / max(albedo, vec3_splat(0.001));
	float irradianceLuminance = luminance(irradiance);

	// The reservoirs converge on the right distribution, but each pixel still
	// shades a single sample. Average the result over time, along with the
	// luminance moments the denoiser derives its noise estimate from.
	vec3 previousColor = vec3_splat(0.0);
	vec3 previousMoments = vec3_splat(0.0);
	vec4 clipPrevious = mul(u_previousViewProj, vec4(position, 1.0));
	if (u_frameInfo.y > 0.5 && clipPrevious.w > 0.0001)
	{
		if (!reprojectHistory(clipToUv(clipPrevious), position, normal, previousColor, previousMoments))
		{
			previousColor = vec3_splat(0.0);
			previousMoments = vec3_splat(0.0);
		}
	}

	float historyLength = min(previousMoments.z + 1.0, kMaxAccumulation);
	float blendWeight = 1.0 / historyLength;
	vec3 color = mix(previousColor, irradiance, blendWeight);
	vec2 moments = mix(
		  previousMoments.xy
		, vec2(irradianceLuminance, irradianceLuminance * irradianceLuminance)
		, blendWeight
		);

	o_color = vec4(color, historyLength);
	o_moments = vec4(moments, historyLength, 0.0);
}

// A compute shader, in groups of 8x8 pixels (compute.sh). Its BVH rays are
// visibility rays, which need no traversal stack (bvh.sh).
IMAGE2D_WO(o_colorImage, rgba16f, 12);
IMAGE2D_WO(o_momentsImage, rgba16f, 13);

NUM_THREADS(kGroupSize, kGroupSize, 1)
void main()
{
	ivec2 pixel = computePixel(gl_GlobalInvocationID);
	if (!insideImage(pixel, u_screenInfo.xy))
	{
		return;
	}

	vec4 color;
	vec4 moments;
	restirSpatial((vec2(pixel) + 0.5) * u_screenInfo.zw, color, moments);
	imageStore(o_colorImage, pixel, color);
	imageStore(o_momentsImage, pixel, moments);
}
