$input v_texcoord0

/*
 * ReSTIR GI, step 1: fresh candidates and temporal reuse.
 *
 * Fresh candidates are traced through the depth buffer and resampled into a
 * canonical reservoir. It is then combined, with pairwise MIS (restir.sh),
 * with reservoirs from the previous frame:
 *
 *   - World-space reuse (the default): kTemporalTaps reservoirs picked at
 *     random from last frame's hash grid cell around this receiver (see
 *     hashgrid.sh). The cell sizes of that grid were chosen from last frame's
 *     camera position, so the lookups use it too. No reprojection is
 *     involved: a surface that was visible last frame anywhere on screen
 *     contributes, as long as it lies in this receiver's cell.
 *   - Screen-space reuse: this pixel's reprojected reservoir.
 *
 * Fresh rays march the depth buffer; those it cannot answer (they leave the
 * screen, or pass behind what it shows) are traced through the scene's BVH
 * instead, or every ray is, depending on the ray mode (bvh.sh).
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
	for (int sampleIndex = 0; sampleIndex < kInitialCandidates; ++sampleIndex)
	{
		float random0 = nextRandom(rng);
		float random1 = nextRandom(rng);
		float randomSelect = nextRandom(rng);
		vec3 direction = cosineHemisphere(normal, random0, random1);

		// Every candidate counts, including rays that found nothing.
		canonical.count += 1.0;

		// The depth buffer first (unless every ray uses the BVH), then the
		// BVH for the rays it cannot answer: those that leave the screen or
		// pass behind what it shows.
		vec3 hitPosition = vec3_splat(0.0);
		float hitId = 0.0;
		bool hit = kRayMode != RAY_MODE_BVH
			&& traceScreenSpaceRay(rayOrigin, direction, kMaxRayDistance, hitPosition);
		if (!hit && kRayMode != RAY_MODE_SCREEN)
		{
			float hitDistance;
			int triangleIndex = traceBvh(rayOrigin, direction, kMaxBvhRayDistance, false, hitDistance);
			if (triangleIndex >= 0)
			{
				hit = true;
				hitPosition = rayOrigin + direction * hitDistance;
				hitId = encodeSampleId(triangleIndex, direction);
			}
			else if (skyEnabled())
			{
				// Nothing in the scene: the sky, in this direction.
				hit = true;
				hitPosition = direction;
				hitId = kSkySampleId;
			}
		}
		if (!hit)
		{
			continue;
		}

		vec3 contribution;
		vec3 hitNormal;
		if (!evaluateSample(position, normal, albedo, hitPosition, hitId, contribution, hitNormal))
		{
			continue;
		}

		float target = luminance(contribution);
		// The density of the hit point, in solid angle at the receiver (the
		// measure of the target function). The ray left from rayOrigin, a
		// little above the receiver, so its cosine density there is converted
		// through the density per unit area at the hit point:
		//   pdf(position) = pdf(rayOrigin) * (cos(hit, origin) / d(origin)^2)
		//                                  / (cos(hit, position) / d(position)^2)
		// The cosines at the hit point only agree for distant hits; for hits
		// next to a receiver close to a wall they differ a lot.
		// A direction to the sky is the same from both points.
		float sourcePdf = dot(normal, direction) / kPi;
		if (!isSkySample(hitId))
		{
			vec3 fromReceiver = hitPosition - position;
			vec3 fromOrigin = hitPosition - rayOrigin;
			float receiverSquared = max(dot(fromReceiver, fromReceiver), kMinDistanceSquared * 0.01);
			float originSquared = max(dot(fromOrigin, fromOrigin), kMinDistanceSquared * 0.01);
			float hitCosineOrigin = max(-dot(hitNormal, fromOrigin), 0.0) * inversesqrt(originSquared);
			float hitCosineReceiver = -dot(hitNormal, fromReceiver) * inversesqrt(receiverSquared);
			sourcePdf = hitCosineReceiver > 0.0001
				? sourcePdf * (hitCosineOrigin / originSquared) * (receiverSquared / hitCosineReceiver)
				: 0.0;
		}
		if (target > 0.0 && sourcePdf > 0.0001)
		{
			updateReservoir(canonical, contribution, hitPosition, hitNormal, hitId, target, target / sourcePdf, randomSelect);
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
	PairwiseMis mis = beginPairwiseMis(position, normal, canonical, canonical.count, kTemporalTaps);
	for (int tapIndex = 0; tapIndex < kTemporalTaps; ++tapIndex)
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
			, texture2DLod(s_historyNormal, historyUv, 0.0)
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

	// Boiling filter (as in RTXDI's ReSTIR GI). The reservoir's estimate
	// f * W may not exceed kBoilingFilter times what this pixel showed last
	// frame (its accumulated irradiance, times the albedo), or what its fresh
	// candidates show now, whichever is larger: the fresh estimate is
	// unbiased and independent of the history, so the lighting can still
	// change. Contribution weights are heavy-tailed, and with multiple bounces
	// the integrand is made of last frame's estimates: a reservoir with an
	// outlying weight brightens the history its own sample reads its light
	// from, and temporal reuse through the hash grid keeps it and spreads it.
	// Without the filter, the light then slowly spreads from corners over
	// hundreds of frames (more readily with TAA's jitter, which keeps moving
	// the receivers). The filter only acts on outliers, so it costs little
	// light (README.md, "Bias").
	vec3 historyColor;
	if (kBoilingFilter > 0.0 && historyValid && combinedWeight > 0.0
	&&  historyIrradiance(previousUv, position, normal, historyColor))
	{
		float reference = max(luminance(historyColor * albedo), canonical.target * canonicalWeight);
		float estimate = mis.combined.target * combinedWeight;
		float limit = kBoilingFilter * reference;
		if (estimate > limit)
		{
			combinedWeight *= limit / estimate;
		}
	}

	gl_FragData[0] = vec4(mis.combined.position, mis.totalCount);
	gl_FragData[1] = vec4(position, combinedWeight);
	gl_FragData[2] = vec4(normal, mis.combined.id);
}
