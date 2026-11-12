$input v_texcoord0

/*
 * ReSTIR DI, step 1: fresh candidates and temporal reuse.
 *
 * Fresh candidates pick a light by power and a point on it, and are
 * resampled by their unshadowed contribution into a canonical reservoir.
 * It is combined with this pixel's reservoir from last frame (found by
 * reprojecting the surface) with the generalized balance heuristic over the
 * two (GRIS):
 *
 *   m_c(x) = M_c p_c(x) / (M_c p_c(x) + M_t p_t(x))
 *   m_t(x) = M_t p_t(x) / (M_t p_t(x) + M_c p_c(x))
 *
 * where p_c is the target at this pixel (unshadowed: its fresh candidates
 * were not tested) and p_t the target at last frame's receiver, seen from
 * last frame's camera, including visibility (stored samples are visible).
 * The selected sample is then tested for visibility and stored (di.sh).
 */

#include <common.sh>
#include "di.sh"

SAMPLER2D(s_previousSample, 4);
SAMPLER2D(s_previousReceiver, 5);
SAMPLER2D(s_previousNormal, 6);

uniform mat4 u_previousViewProj;
uniform vec4 u_diPrevious; // xyz: last frame's camera position

void main()
{
	Surface surface = loadSurface(v_texcoord0);
	if (!surface.valid || lightCount() == 0)
	{
		gl_FragData[0] = vec4(0.0, 0.0, 0.0, -1.0);
		gl_FragData[1] = vec4_splat(0.0);
		gl_FragData[2] = vec4_splat(0.0);
		return;
	}

	uvec2 pixel = uvec2(gl_FragCoord.xy);
	uint rng = pcg(pixel.x + pcg(pixel.y + pcg(uint(u_frameInfo.x) ^ 0x3c6ef372u)));

	// Fresh candidates.
	int candidates = int(u_diInfo.y + 0.5);
	float weightSum = 0.0;
	vec3 chosenPoint = vec3_splat(0.0);
	int chosenLight = -1;
	float chosenTarget = 0.0;
	for (int candidate = 0; candidate < candidates; ++candidate)
	{
		float pmf;
		int light = selectLight(nextRandom(rng), pmf);
		float density;
		vec3 lightPoint = sampleLightPoint(light, nextRandom(rng), nextRandom(rng), density);
		float target = lightTarget(surface, light, lightPoint);
		float sourcePdf = pmf * density;
		float weight = sourcePdf > 0.0 ? target / sourcePdf : 0.0;
		weightSum += weight;
		if (nextRandom(rng) * weightSum < weight)
		{
			chosenPoint = lightPoint;
			chosenLight = light;
			chosenTarget = target;
		}
	}
	float canonicalCount = float(candidates);
	float canonicalWeight = chosenTarget > 0.0 ? weightSum / (canonicalCount * chosenTarget) : 0.0;
	vec3 canonicalPoint = chosenPoint;
	int canonicalLight = chosenLight;
	float canonicalTarget = chosenTarget;

	// Last frame's reservoir of the same surface.
	float temporalCount = 0.0;
	float temporalWeight = 0.0;
	vec3 temporalPoint = vec3_splat(0.0);
	int temporalLight = -1;
	Surface previous = surface;
	vec4 clip = mul(u_previousViewProj, vec4(surface.position, 1.0));
	if (u_diInfo.w > 0.5 && clip.w > 0.0001)
	{
		vec2 uv = vec2(clip.x, u_frameInfo.z > 0.5 ? clip.y : -clip.y) / clip.w * 0.5 + 0.5;
		if (uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0)
		{
			vec4 sampleData = texture2DLod(s_previousSample, uv, 0.0);
			vec4 receiver = texture2DLod(s_previousReceiver, uv, 0.0);
			vec4 normal = texture2DLod(s_previousNormal, uv, 0.0);
			int light = int(sampleData.w + 0.5);
			if (sampleData.w >= 0.0 && light < lightCount()
			&&  sameSurface(surface.position, surface.normal, receiver.xyz, normal.xyz, kDiReceiverRadius))
			{
				temporalCount = min(normal.w, kDiMaxHistory);
				temporalWeight = receiver.w;
				temporalPoint = sampleData.xyz;
				temporalLight = light;
				previous.position = receiver.xyz;
				previous.normal = normalize(normal.xyz);
				previous.eye = u_diPrevious.xyz;
			}
		}
	}

	// Combine the two with the generalized balance heuristic.
	weightSum = 0.0;
	chosenLight = -1;
	chosenTarget = 0.0;
	if (canonicalTarget > 0.0)
	{
		float previousTarget = temporalCount > 0.0 && lightVisible(previous, canonicalPoint)
			? lightTarget(previous, canonicalLight, canonicalPoint)
			: 0.0;
		float denominator = canonicalCount * canonicalTarget + temporalCount * previousTarget;
		float weight = canonicalCount * canonicalTarget / denominator * canonicalTarget * canonicalWeight;
		weightSum += weight;
		chosenPoint = canonicalPoint;
		chosenLight = canonicalLight;
		chosenTarget = canonicalTarget;
	}
	if (temporalCount > 0.0 && temporalWeight > 0.0)
	{
		float target = lightTarget(surface, temporalLight, temporalPoint);
		float previousTarget = lightTarget(previous, temporalLight, temporalPoint);
		float denominator = temporalCount * previousTarget + canonicalCount * target;
		float weight = denominator > 0.0
			? temporalCount * previousTarget / denominator * target * temporalWeight
			: 0.0;
		weightSum += weight;
		if (weight > 0.0 && nextRandom(rng) * weightSum < weight)
		{
			chosenPoint = temporalPoint;
			chosenLight = temporalLight;
			chosenTarget = target;
		}
	}

	// The MIS weights sum to one, so W = sum of weights / target.
	float combinedWeight = chosenTarget > 0.0 ? weightSum / chosenTarget : 0.0;
	if (combinedWeight > 0.0 && !lightVisible(surface, chosenPoint))
	{
		combinedWeight = 0.0;
	}

	gl_FragData[0] = vec4(chosenPoint, float(chosenLight));
	gl_FragData[1] = vec4(surface.position, combinedWeight);
	gl_FragData[2] = vec4(surface.normal, canonicalCount + temporalCount);
}
