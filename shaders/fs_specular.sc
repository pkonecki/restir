$input v_texcoord0

/*
 * Indirect specular light (glossy reflections), at GI resolution.
 *
 * One ray per pixel, sampled from the GGX distribution of visible normals
 * (Heitz 2018), so its weight is just F * G1(light). It is traced like the
 * GI rays (restir.sh): through the depth buffer, then the BVH, then the sky.
 * The radiance it brings back:
 *
 *   - a surface that is visible: last frame's HDR image there, which holds
 *     everything (direct, diffuse and specular GI), so reflections show
 *     reflections, one frame late;
 *   - off screen or hidden: its triangle's direct light, bounce light and
 *     emission, as for GI samples;
 *   - nothing (confirmed by the BVH): the sky.
 *
 * The estimate is divided by the split-sum specular albedo (brdf.sh) so that
 * what is accumulated is smooth, and accumulated over up to 4 frames on a
 * mirror and 32 on a rough surface: reflections move with the view, and
 * long histories would smear them. The composite multiplies the specular
 * albedo back in.
 *
 * Output: rgb = demodulated specular radiance, a = history length.
 */

#include <common.sh>

SAMPLER2D(s_previousHdr, 3);      // last frame's composite (scene-referred)
SAMPLER2D(s_specularHistory, 4);  // last frame's output of this pass

uniform vec4 u_cameraPosition;

#include "restir.sh"
#include "brdf.sh"

#define kMinSpecularHistory 4.0
#define kMaxSpecularHistory 32.0

// The surface's tangent frame, for the visible-normal sampling.
void tangentFrame(vec3 _normal, out vec3 _tangent, out vec3 _bitangent)
{
	_tangent = normalize(abs(_normal.y) < 0.99
		? cross(_normal, vec3(0.0, 1.0, 0.0))
		: cross(_normal, vec3(1.0, 0.0, 0.0)));
	_bitangent = cross(_normal, _tangent);
}

// A microfacet normal from the GGX distribution of normals visible from
// _view (in the tangent frame, z up), Heitz 2018.
vec3 sampleVisibleNormal(vec3 _view, float _alpha, float _random0, float _random1)
{
	vec3 stretched = normalize(vec3(_alpha * _view.x, _alpha * _view.y, _view.z));
	float lengthSquared = stretched.x * stretched.x + stretched.y * stretched.y;
	vec3 t1 = lengthSquared > 0.0
		? vec3(-stretched.y, stretched.x, 0.0) * inversesqrt(lengthSquared)
		: vec3(1.0, 0.0, 0.0);
	vec3 t2 = cross(stretched, t1);
	float radius = sqrt(_random0);
	float angle = 2.0 * kPi * _random1;
	float p1 = radius * cos(angle);
	float p2 = radius * sin(angle);
	float s = 0.5 * (1.0 + stretched.z);
	p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
	vec3 normal = p1 * t1 + p2 * t2 + sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0)) * stretched;
	return normalize(vec3(_alpha * normal.x, _alpha * normal.y, max(normal.z, 0.0)));
}

// Smith's masking for GGX.
float smithG1(float _cosine, float _alpha)
{
	float alphaSquared = _alpha * _alpha;
	return 2.0 * _cosine / (_cosine + sqrt(alphaSquared + (1.0 - alphaSquared) * _cosine * _cosine));
}

// Radiance leaving a ray's hit point back along the ray.
vec3 hitRadiance(vec3 _hitPosition, float _hitId, vec3 _rayDirection)
{
	if (isSkySample(_hitId))
	{
		return skyRadiance(_rayDirection);
	}

	vec2 uv;
	float viewDepth;
	bool visibleSurface = false;
	if (projectToScreen(_hitPosition, uv, viewDepth))
	{
		float depth = texture2DLod(s_depth, uv, 0.0).x;
		visibleSurface = depth < kSkyDepth
			&& distance(reconstructWorldPosition(uv, depth), _hitPosition) <= kSampleTolerance;
	}

	if (visibleSurface)
	{
		// Last frame's image at that point, if it was on screen then.
		vec4 clip = mul(u_previousViewProj, vec4(_hitPosition, 1.0));
		if (u_frameInfo.y > 0.5 && clip.w > 0.0001)
		{
			vec2 previousUv = clipToUv(clip);
			vec4 previous = texture2DLod(s_previousHdr, previousUv, 0.0);
			if (isOnScreen(previousUv) && previous.a > 0.5)
			{
				return previous.rgb;
			}
		}

		vec3 normal = decodeNormal(uv);
		return evaluateSecondaryDirect(uv, _hitPosition, normal)
			+ previousIndirect(_hitPosition, normal, diffuseAlbedo(uv))
			+ texture2DLod(s_surfaceLight, uv, 0.0).rgb * u_lightingInfo.x;
	}

	int triangleIndex = int(abs(_hitId) + 0.5) - 1;
	if (triangleIndex < 0 || !bvhAvailable())
	{
		return vec3_splat(0.0);
	}

	vec3 normal = bvhTriangleNormal(triangleIndex, -_rayDirection);
	vec3 albedo = bvhTriangleAlbedo(triangleIndex);
	return evaluateWorldDirect(albedo, _hitPosition, normal)
		+ previousIndirect(_hitPosition, normal, albedo)
		+ bvhTriangleEmission(triangleIndex) * u_lightingInfo.y;
}

void main()
{
	float depth = texture2DLod(s_depth, v_texcoord0, 0.0).x;
	if (depth >= kSkyDepth)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	vec3 position = reconstructWorldPosition(v_texcoord0, depth);
	vec3 normal = decodeNormal(v_texcoord0);
	vec4 albedoRoughness = texture2DLod(s_albedo, v_texcoord0, 0.0);
	float metallic = texture2DLod(s_normal, v_texcoord0, 0.0).a;
	float roughness = max(albedoRoughness.a, 0.02);
	float alpha = roughness * roughness;
	vec3 viewDirection = normalize(u_cameraPosition.xyz - position);
	float normalView = max(dot(normal, viewDirection), 0.001);
	vec3 f0 = mix(vec3_splat(0.04), albedoRoughness.rgb, metallic);

	vec2 pixel = giPixel(v_texcoord0);
	uint rng = pcg(uint(pixel.x) + pcg(uint(pixel.y) + pcg(uint(u_frameInfo.x) ^ 0x2545f491u)));

	// A reflected direction from the visible normals.
	vec3 tangent;
	vec3 bitangent;
	tangentFrame(normal, tangent, bitangent);
	vec3 localView = vec3(dot(viewDirection, tangent), dot(viewDirection, bitangent), normalView);
	vec3 localHalfway = sampleVisibleNormal(localView, alpha, nextRandom(rng), nextRandom(rng));
	vec3 halfway = tangent * localHalfway.x + bitangent * localHalfway.y + normal * localHalfway.z;
	vec3 direction = reflect(-viewDirection, halfway);
	float normalLight = dot(normal, direction);

	vec3 estimate = vec3_splat(0.0);
	if (normalLight > 0.0)
	{
		vec3 rayOrigin = position + normal * kRayBias;
		vec3 hitPosition = vec3_splat(0.0);
		float hitId = 0.0;
		bool hit = false;
		if (kRayMode != RAY_MODE_BVH)
		{
			hit = traceScreenSpaceRay(rayOrigin, direction, kMaxRayDistance, hitPosition);
		}
		if (!hit && kRayMode != RAY_MODE_SCREEN)
		{
			float hitDistance = 0.0;
			int triangleIndex = traceBvh(rayOrigin, direction, kMaxBvhRayDistance, false, hitDistance);
			if (triangleIndex >= 0)
			{
				hit = true;
				hitPosition = rayOrigin + direction * hitDistance;
				hitId = encodeSampleId(triangleIndex, direction);
			}
			else if (skyEnabled())
			{
				hit = true;
				hitId = kSkySampleId;
			}
		}

		if (hit)
		{
			vec3 fresnel = fresnelSchlick(halfway, viewDirection, f0);
			estimate = hitRadiance(hitPosition, hitId, direction) * fresnel * smithG1(normalLight, alpha);
		}
	}

	// Demodulated, and limited like the diffuse GI.
	vec3 demodulated = estimate / max(specularAlbedo(f0, roughness, normalView), vec3_splat(0.001));
	if (kOutlierClamp > 0.0)
	{
		demodulated = min(demodulated, vec3_splat(kOutlierClamp));
	}

	// Last frame's result at this surface (the nearest same-surface texel).
	vec3 history = vec3_splat(0.0);
	float historyLength = 0.0;
	vec4 clipPrevious = mul(u_previousViewProj, vec4(position, 1.0));
	if (u_frameInfo.y > 0.5 && clipPrevious.w > 0.0001)
	{
		vec2 previousUv = (floor(clipToUv(clipPrevious) * u_screenInfo.xy) + 0.5) * u_screenInfo.zw;
		vec3 receiverPosition = texture2DLod(s_historyReceiver, previousUv, 0.0).xyz;
		vec3 receiverNormal = texture2DLod(s_historyNormal, previousUv, 0.0).xyz;
		vec3 offset = receiverPosition - position;
		if (isOnScreen(previousUv)
		&&  dot(receiverNormal, receiverNormal) > 0.5
		&&  dot(offset, offset) <= kBounceRadius * kBounceRadius
		&&  abs(dot(offset, normal)) <= kReceiverPlane
		&&  dot(normalize(receiverNormal), normal) >= kReceiverNormal)
		{
			vec4 previous = texture2DLod(s_specularHistory, previousUv, 0.0);
			history = previous.rgb;
			historyLength = previous.a;
		}
	}

	float maxHistory = mix(kMinSpecularHistory, kMaxSpecularHistory, clamp(roughness * 1.5, 0.0, 1.0));
	historyLength = min(historyLength + 1.0, maxHistory);
	gl_FragColor = vec4(mix(history, demodulated, 1.0 / historyLength), historyLength);
}
