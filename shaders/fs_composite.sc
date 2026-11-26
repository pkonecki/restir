/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

$input v_texcoord0

#include <common.sh>

SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_normal, 1);
SAMPLER2D(s_depth, 2);
SAMPLER2D(s_gi, 3);
SAMPLER2D(s_emissive, 5);      // rgb: emitted radiance, a: ambient occlusion
SAMPLER2D(s_shadingNormal, 6); // rgb: encoded normal, with the normal map
SAMPLER2D(s_diDiffuse, 7);     // direct light from the light list (fs_di_spatial.sc), diffuse part
SAMPLER2D(s_diSpecular, 8);    // and specular part
SAMPLER2D(s_specular, 9);      // demodulated indirect specular (fs_specular.sc), GI resolution

uniform mat4 u_sceneInvViewProj;
uniform vec4 u_lightPosition;
uniform vec4 u_lightColor;
uniform vec4 u_cameraPosition;
uniform vec4 u_screenInfo; // xy: GI target size, zw: GI texel size
uniform vec4 u_sceneInfo;  // x: scene scale (see renderer.cpp)
uniform vec4 u_compositeOptions; // x: direct weight, y: indirect weight, z: 1 to multiply the albedo into the indirect light, w: emission weight
uniform vec4 u_compositeSpecular; // x: indirect specular weight

#define SHADOW_SAMPLER_STAGE 4
#include "shadow.sh"
#include "sky.sh"

#define kPi 3.14159265359
#define kSceneScale u_sceneInfo.x

#include "brdf.sh"

vec3 reconstructWorldPosition(vec2 _uv, float _depth)
{
	vec3 clipPosition = vec3(_uv * 2.0 - 1.0, toClipSpaceDepth(_depth));
	clipPosition.y = toClipSpaceY(clipPosition.y);
	return clipToWorld(u_sceneInvViewProj, clipPosition);
}

// The GI target is half resolution. Blend the four nearest GI texels, but
// only those that lie on the same surface as this pixel, so indirect light
// does not bleed across silhouettes.
void upsampleIndirect(vec2 _uv, vec3 _position, vec3 _normal, out vec3 _diffuse, out vec3 _specular)
{
	vec2 texelCoord = _uv * u_screenInfo.xy - 0.5;
	vec2 baseCoord = floor(texelCoord);
	vec2 blend = texelCoord - baseCoord;

	vec3 sum = vec3_splat(0.0);
	vec3 specularSum = vec3_splat(0.0);
	float weightSum = 0.0;
	for (int tapIndex = 0; tapIndex < 4; ++tapIndex)
	{
		vec2 offset = vec2(float(tapIndex & 1), float(tapIndex >> 1));
		vec2 tapUv = (baseCoord + offset + 0.5) * u_screenInfo.zw;
		float tapDepth = texture2DLod(s_depth, tapUv, 0.0).x;
		if (tapDepth >= 0.99999)
		{
			continue;
		}

		vec3 tapPosition = reconstructWorldPosition(tapUv, tapDepth);
		vec3 tapNormal = normalize(texture2DLod(s_normal, tapUv, 0.0).xyz * 2.0 - 1.0);
		vec2 bilinear = mix(vec2_splat(1.0) - blend, blend, offset);
		float weight = (bilinear.x * bilinear.y + 0.01)
			* step(abs(dot(tapPosition - _position, _normal)), 0.03 * kSceneScale)
			* step(0.9, dot(tapNormal, _normal));
		sum += texture2DLod(s_gi, tapUv, 0.0).rgb * weight;
		specularSum += texture2DLod(s_specular, tapUv, 0.0).rgb * weight;
		weightSum += weight;
	}

	_diffuse = weightSum > 0.0 ? sum / weightSum : texture2DLod(s_gi, _uv, 0.0).rgb;
	_specular = weightSum > 0.0 ? specularSum / weightSum : texture2DLod(s_specular, _uv, 0.0).rgb;
}

void main()
{
	float depth = texture2D(s_depth, v_texcoord0).x;
	if (depth >= 0.99999)
	{
		// The background: the sky, or (alpha 0) a fixed colour that the tone
		// mapping pass fills in.
		vec3 farPosition = reconstructWorldPosition(v_texcoord0, 1.0);
		vec3 direction = normalize(farPosition - u_cameraPosition.xyz);
		gl_FragColor = skyEnabled() ? vec4(skyRadiance(direction), 1.0) : vec4_splat(0.0);
		return;
	}

	vec4 albedoRoughness = texture2D(s_albedo, v_texcoord0);
	vec4 normalMetallic = texture2D(s_normal, v_texcoord0);
	vec3 albedo = albedoRoughness.rgb;
	float roughness = max(albedoRoughness.a, 0.05);
	float metallic = normalMetallic.a;
	// The surface normal finds the GI texels of the same surface and offsets
	// the shadow lookup; the normal-mapped one shades the direct light.
	vec3 surfaceNormal = normalize(normalMetallic.xyz * 2.0 - 1.0);
	vec3 normal = normalize(texture2D(s_shadingNormal, v_texcoord0).xyz * 2.0 - 1.0);
	vec4 emissiveOcclusion = texture2D(s_emissive, v_texcoord0);
	vec3 position = reconstructWorldPosition(v_texcoord0, depth);
	vec3 viewDirection = normalize(u_cameraPosition.xyz - position);
	vec3 toLight = u_lightPosition.xyz - position;
	float distanceSquared = max(dot(toLight, toLight), 0.01 * kSceneScale * kSceneScale);
	vec3 lightDirection = toLight * inversesqrt(distanceSquared);

	// The point light (shadow map), plus every other light through ReSTIR DI
	// (zero when it is off).
	vec3 diffuse;
	vec3 specular;
	evaluateBrdf(normal, viewDirection, lightDirection, albedo, roughness, metallic, diffuse, specular);
	// Not a ?: (HLSL evaluates both sides, see restir.sh).
	float visibility = 0.0;
	if (dot(normal, lightDirection) > 0.0)
	{
		visibility = lightVisibilitySoft(position, surfaceNormal);
	}
	vec3 direct = (diffuse + specular) * (u_lightColor.rgb / distanceSquared) * visibility
		+ texture2D(s_diDiffuse, v_texcoord0).rgb + texture2D(s_diSpecular, v_texcoord0).rgb;
	// The GI target holds demodulated irradiance: multiply the diffuse albedo
	// back in (the debug view of irradiance leaves it out), and the
	// material's ambient occlusion, which glTF applies to indirect light only.
	vec3 irradiance;
	vec3 specularRadiance;
	upsampleIndirect(v_texcoord0, position, surfaceNormal, irradiance, specularRadiance);
	vec3 indirect = irradiance
		* (u_compositeOptions.z > 0.5 ? albedo * (1.0 - metallic) * emissiveOcclusion.a : vec3_splat(1.0));
	// Indirect specular: the demodulated radiance times the specular albedo
	// (fs_specular.sc), dimmed by the ambient occlusion like the diffuse.
	vec3 f0 = mix(vec3_splat(0.04), albedo, metallic);
	indirect += specularRadiance * specularAlbedo(f0, roughness, max(dot(normal, viewDirection), 0.001))
		* emissiveOcclusion.a * u_compositeSpecular.x;
	// Scene-referred: exposure and tone mapping follow in fs_tonemap.sc.
	gl_FragColor = vec4(
		  direct * u_compositeOptions.x
		+ indirect * u_compositeOptions.y
		+ emissiveOcclusion.rgb * u_compositeOptions.w
		, 1.0);
}
