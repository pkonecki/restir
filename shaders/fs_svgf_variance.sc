$input v_texcoord0

/*
 * Denoiser, step 1: per-pixel noise estimate.
 *
 * Where a pixel has enough history, its variance comes from the temporally
 * accumulated luminance moments. Where it does not (just disoccluded, or the
 * first frames), it is estimated from the moments of nearby pixels on the same
 * surface, and boosted, so the a-trous filter blurs those pixels harder.
 *
 * Output: rgb = demodulated GI, a = luminance variance.
 */

#include <common.sh>

SAMPLER2D(s_giColor, 0);    // rgb: temporally accumulated demodulated GI
SAMPLER2D(s_giMoments, 1);  // x: mean luminance, y: mean squared luminance, z: history length
SAMPLER2D(s_giReceiver, 2); // xyz: world position
SAMPLER2D(s_giNormal, 3);   // xyz: world normal (zero for sky)

uniform vec4 u_screenInfo; // xy: GI target size, zw: GI texel size
uniform vec4 u_sceneInfo;  // x: scene scale

#include "svgf.sh"

#define kMinTemporalHistory 4.0
#define kSpatialRadius      3

void main()
{
	vec3 normal = texture2DLod(s_giNormal, v_texcoord0, 0.0).xyz;
	if (dot(normal, normal) < 0.5)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	normal = normalize(normal);
	vec3 color = texture2DLod(s_giColor, v_texcoord0, 0.0).rgb;
	vec3 moments = texture2DLod(s_giMoments, v_texcoord0, 0.0).xyz;
	float historyLength = moments.z;
	if (historyLength >= kMinTemporalHistory)
	{
		gl_FragColor = vec4(color, max(moments.y - moments.x * moments.x, 0.0));
		return;
	}

	vec3 position = texture2DLod(s_giReceiver, v_texcoord0, 0.0).xyz;
	vec2 momentSum = vec2_splat(0.0);
	float weightSum = 0.0;
	for (int y = -kSpatialRadius; y <= kSpatialRadius; ++y)
	{
		for (int x = -kSpatialRadius; x <= kSpatialRadius; ++x)
		{
			vec2 uv = v_texcoord0 + vec2(float(x), float(y)) * u_screenInfo.zw;
			if (!isInside(uv))
			{
				continue;
			}

			float weight = geometryWeight(
				  position
				, normal
				, texture2DLod(s_giReceiver, uv, 0.0).xyz
				, texture2DLod(s_giNormal, uv, 0.0).xyz
				);
			momentSum += texture2DLod(s_giMoments, uv, 0.0).xy * weight;
			weightSum += weight;
		}
	}

	vec2 spatialMoments = momentSum / max(weightSum, 0.0001);
	float variance = max(spatialMoments.y - spatialMoments.x * spatialMoments.x, 0.0)
		* (kMinTemporalHistory / max(historyLength, 1.0));
	gl_FragColor = vec4(color, variance);
}
