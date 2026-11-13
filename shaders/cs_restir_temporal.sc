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
 *
 * A compute shader, in groups of 8x8 pixels (compute.sh). The fresh rays
 * that need the BVH are queued in shared memory and traced by consecutive
 * threads of the group, so a wave traces only when it has rays to trace,
 * instead of every wave waiting for its few lanes that do. Closest-hit rays
 * keep their traversal stack in shared memory (bvh.sh).
 */

#include <bgfx_compute.sh>
#include "shaderlib.sh"

#define COMPUTE_GROUP_SUM
#define COMPUTE_GROUP_QUEUE
#define BVH_SHARED_STACK 64 // kGroupThreads
#include "compute.sh"

SAMPLER2D(s_historySample, 3); // last frame's sample position + M

#include "restir.sh"

// The reservoir targets (restir.sh).
IMAGE2D_WO(o_sample, rgba32f, 4);
IMAGE2D_WO(o_receiver, rgba32f, 8);
IMAGE2D_WO(o_normal, rgba32f, 10);

// The group's queue of BVH rays: origin and direction. The thread that
// traces a ray writes the result (triangle index or -1, distance) over its
// origin, which nothing reads after it.
SHARED vec4 s_queueOrigin[kGroupThreads];
SHARED vec4 s_queueDirection[kGroupThreads];

NUM_THREADS(kGroupSize, kGroupSize, 1)
void main()
{
	int thread = int(gl_LocalInvocationIndex);
	g_bvhThread = thread;
	ivec2 pixel = computePixel(gl_GlobalInvocationID);
	bool inside = insideImage(pixel, u_screenInfo.xy);
	vec2 uv = (vec2(pixel) + 0.5) * u_screenInfo.zw;

	// Pixels outside the image or over the sky do no work, but they still
	// take part in the group's barriers below.
	float depth = inside ? texture2DLod(s_depth, uv, 0.0).x : 1.0;
	bool working = inside && depth < kSkyDepth;

	vec3 position = reconstructWorldPosition(uv, depth);
	vec3 normal = decodeNormal(uv);
	vec3 albedo = diffuseAlbedo(uv);
	vec3 rayOrigin = position + normal * kRayBias;

	vec2 giPixelCoord = giPixel(uv);
	uint rng = pcg(uint(giPixelCoord.x) + pcg(uint(giPixelCoord.y) + pcg(uint(u_frameInfo.x))));

	if (thread == 0)
	{
		s_queueCount = 0u;
	}
	GROUP_SYNC();

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
		bool hit = false;
		if (working && kRayMode != RAY_MODE_BVH)
		{
			hit = traceScreenSpaceRay(rayOrigin, direction, kMaxRayDistance, hitPosition);
		}

		// The rays left for the BVH go to the group's queue, and the first
		// threads of the group trace them.
		bool needBvh = working && !hit && kRayMode != RAY_MODE_SCREEN;
		uint slot = queueSlot(needBvh);
		if (needBvh)
		{
			s_queueOrigin[slot] = vec4(rayOrigin, 0.0);
			s_queueDirection[slot] = vec4(direction, 0.0);
		}
		GROUP_SYNC();
		uint queued = s_queueCount;
		if (uint(thread) < queued)
		{
			float queueDistance;
			int queueTriangle = traceBvh(s_queueOrigin[thread].xyz, s_queueDirection[thread].xyz
				, kMaxBvhRayDistance, false, queueDistance);
			s_queueOrigin[thread] = vec4(float(queueTriangle), queueDistance, 0.0, 0.0);
		}
		GROUP_SYNC();
		if (thread == 0)
		{
			s_queueCount = 0u;
		}
		if (needBvh)
		{
			vec2 result = s_queueOrigin[slot].xy;
			int triangleIndex = int(result.x);
			if (triangleIndex >= 0)
			{
				hit = true;
				hitPosition = rayOrigin + direction * result.y;
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
		GROUP_SYNC(); // the queue is empty again before the next candidate

		// No early continue: the barriers above must not sit in a loop that
		// some threads leave early (the D3D compiler rejects that).
		vec3 contribution = vec3_splat(0.0);
		vec3 hitNormal = vec3_splat(0.0);
		bool evaluated = false;
		if (hit)
		{
			evaluated = evaluateSample(position, normal, albedo, hitPosition, hitId, contribution, hitNormal);
		}
		if (evaluated)
		{
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
	float combinedWeight = 0.0;
	if (working)
	{
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

		combinedWeight = finishPairwiseMis(mis, position, normal, canonical, canonicalWeight, nextRandom(rng));
	}

	// Boiling filter (as in RTXDI's ReSTIR GI). The reservoir's estimate
	// f * W may not exceed kBoilingFilter times a reference: what its fresh
	// candidates show now, or, if larger, what this pixel showed last frame
	// (its accumulated irradiance, times the albedo) or, without a history
	// there (just disoccluded), the mean estimate of the group's pixels. The
	// fresh estimate is unbiased and independent of the history, so the
	// lighting can still change. Contribution weights are heavy-tailed, and
	// with multiple bounces the integrand is made of last frame's estimates:
	// a reservoir with an outlying weight brightens the history its own
	// sample reads its light from, and temporal reuse through the hash grid
	// keeps it and spreads it. Without the filter, the light then slowly
	// spreads from corners over hundreds of frames (more readily with TAA's
	// jitter, which keeps moving the receivers). The filter only acts on
	// outliers, so it costs little light (README.md, "Bias").
	float estimate = working ? mis.combined.target * combinedWeight : 0.0;
	vec2 tileSum = groupSum(vec2(estimate, working ? 1.0 : 0.0), thread);
	float tileMean = tileSum.x / max(tileSum.y, 1.0);
	if (working && kBoilingFilter > 0.0 && combinedWeight > 0.0)
	{
		float reference = canonical.target * canonicalWeight;
		vec3 historyColor;
		if (historyValid && historyIrradiance(previousUv, position, normal, historyColor))
		{
			reference = max(reference, luminance(historyColor * albedo));
		}
		else
		{
			reference = max(reference, tileMean);
		}
		float limit = kBoilingFilter * reference;
		if (estimate > limit)
		{
			combinedWeight *= limit / estimate;
		}
	}

	if (inside)
	{
		imageStore(o_sample, pixel, working ? vec4(mis.combined.position, mis.totalCount) : vec4_splat(0.0));
		imageStore(o_receiver, pixel, working ? vec4(position, combinedWeight) : vec4_splat(0.0));
		imageStore(o_normal, pixel, working ? vec4(normal, mis.combined.id) : vec4_splat(0.0));
	}
}
