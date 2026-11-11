$input v_texcoord0

/*
 * ReSTIR GI, step 1: fresh candidates and temporal reuse.
 *
 * Fresh candidates are traced through the depth buffer and resampled into a
 * canonical reservoir. It is then combined, with pairwise MIS (restir.sh),
 * with reservoirs from the previous frame:
 *
 *   - World-space reuse (the default): TEMPORAL_TAPS reservoirs picked at
 *     random from last frame's hash grid cell around this receiver (see
 *     hashgrid.sh). The cell sizes of that grid were chosen from last frame's
 *     camera position, so the lookups use it too. No reprojection is
 *     involved: a surface that was visible last frame anywhere on screen
 *     contributes, as long as it lies in this receiver's cell.
 *   - Screen-space reuse: this pixel's reprojected reservoir.
 *
 * The result is written to the reservoir targets (see restir.sh). It is what
 * the hash grid built this frame points to, for the spatial pass and for the
 * next frame's temporal reuse. A sample taken over from another receiver is
 * tested for visibility from this one before it is stored, so every stored
 * sample is visible from its receiver (or carries W = 0), as the MIS weights
 * of later reuse assume.
 */

#include <common.sh>

SAMPLER2D(s_historySample, 3); // last frame's sample position + M

#include "restir.sh"

#define INITIAL_CANDIDATES 2
#define TEMPORAL_TAPS      2

void main()
{
	float depth = texture2DLod(s_depth, v_texcoord0, 0.0).x;
	if (depth >= kSkyDepth)
	{
		gl_FragData[0] = vec4_splat(0.0);
		gl_FragData[1] = vec4_splat(0.0);
		gl_FragData[2] = vec4_splat(0.0);
		return;
	}

	vec3 position = reconstructWorldPosition(v_texcoord0, depth);
	vec3 normal = decodeNormal(v_texcoord0);
	vec3 albedo = diffuseAlbedo(v_texcoord0);
	vec3 rayOrigin = position + normal * kRayBias;

	vec2 pixel = giPixel(v_texcoord0);
	uint rng = pcg(uint(pixel.x) + pcg(uint(pixel.y) + pcg(uint(u_frameInfo.x))));

	// Initial candidates: cosine-distributed rays, pdf = cos / pi.
	Reservoir canonical = emptyReservoir();
	for (int sampleIndex = 0; sampleIndex < INITIAL_CANDIDATES; ++sampleIndex)
	{
		float random0 = nextRandom(rng);
		float random1 = nextRandom(rng);
		float randomSelect = nextRandom(rng);
		vec3 direction = cosineHemisphere(normal, random0, random1);

		// Every candidate counts, including rays that found nothing.
		canonical.count += 1.0;

		vec3 hitPosition;
		if (!traceScreenSpaceRay(rayOrigin, direction, kMaxRayDistance, hitPosition))
		{
			continue;
		}

		vec3 contribution;
		vec3 hitNormal;
		if (!evaluateSample(position, normal, albedo, hitPosition, contribution, hitNormal))
		{
			continue;
		}

		float target = luminance(contribution);
		float sourcePdf = dot(normal, normalize(hitPosition - position)) / kPi;
		if (target > 0.0 && sourcePdf > 0.0001)
		{
			updateReservoir(canonical, contribution, hitPosition, hitNormal, target, target / sourcePdf, randomSelect);
		}
	}

	float canonicalWeight = canonical.target > 0.000001
		? canonical.weightSum / (canonical.count * canonical.target)
		: 0.0;

	bool historyValid = u_frameInfo.y > 0.5;
	vec2 previousUv = vec2_splat(0.0);
	vec4 clipPrevious = mul(u_previousViewProj, vec4(position, 1.0));
	if (clipPrevious.w > 0.0001)
	{
		previousUv = clipToUv(clipPrevious);
	}

	float radius = reuseRadius(position);
	PairwiseMis mis = beginPairwiseMis(position, normal, canonical, canonical.count, TEMPORAL_TAPS);
	for (int tapIndex = 0; tapIndex < TEMPORAL_TAPS; ++tapIndex)
	{
		float random0 = nextRandom(rng);
		float random1 = nextRandom(rng);
		float random2 = nextRandom(rng);
		float randomSelect = nextRandom(rng);

		vec2 historyUv = vec2_splat(-1.0);
		if (historyValid)
		{
			if (kWorldSpaceReuse)
			{
				uint entry = gridPickEntry(position, normal, random0, random1, random2);
				if (entry != kGridInvalid)
				{
					historyUv = giPixelUv(entry);
				}
			}
			else if (tapIndex == 0 && clipPrevious.w > 0.0001)
			{
				historyUv = previousUv;
			}
		}

		NeighbourReservoir neighbour;
		if (!isOnScreen(historyUv)
		||  !acceptNeighbour(
			  texture2DLod(s_historySample, historyUv, 0.0)
			, texture2DLod(s_historyReceiver, historyUv, 0.0)
			, texture2DLod(s_historyNormal, historyUv, 0.0).xyz
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

	gl_FragData[0] = vec4(mis.combined.position, mis.totalCount);
	gl_FragData[1] = vec4(position, combinedWeight);
	gl_FragData[2] = vec4(normal, 0.0);
}
