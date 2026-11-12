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

uniform mat4 u_sceneInvViewProj;
uniform vec4 u_lightPosition;
uniform vec4 u_lightColor;
uniform vec4 u_cameraPosition;
uniform vec4 u_screenInfo; // xy: GI target size, zw: GI texel size
uniform vec4 u_sceneInfo;  // x: scene scale (see renderer.cpp)
uniform vec4 u_compositeOptions; // x: direct weight, y: indirect weight, z: 1 to multiply the albedo into the indirect light

#define SHADOW_SAMPLER_STAGE 4
#include "shadow.sh"

#define kPi 3.14159265359
#define kSceneScale u_sceneInfo.x

float distributionGGX(vec3 _normal, vec3 _halfway, float _roughness)
{
	float alpha = _roughness * _roughness;
	float alphaSquared = alpha * alpha;
	float normalHalfway = max(dot(_normal, _halfway), 0.0);
	float denominator = normalHalfway * normalHalfway * (alphaSquared - 1.0) + 1.0;
	return alphaSquared / max(kPi * denominator * denominator, 0.0001);
}

float geometrySchlickGGX(float _normalDirection, float _roughness)
{
	float k = (_roughness + 1.0) * (_roughness + 1.0) / 8.0;
	return _normalDirection / max(_normalDirection * (1.0 - k) + k, 0.0001);
}

vec3 fresnelSchlick(vec3 _halfway, vec3 _viewDirection, vec3 _f0)
{
	float cosine = max(dot(_halfway, _viewDirection), 0.0);
	return _f0 + (1.0 - _f0) * pow(1.0 - cosine, 5.0);
}

vec3 reconstructWorldPosition(vec2 _uv, float _depth)
{
	vec3 clipPosition = vec3(_uv * 2.0 - 1.0, toClipSpaceDepth(_depth));
	clipPosition.y = toClipSpaceY(clipPosition.y);
	return clipToWorld(u_sceneInvViewProj, clipPosition);
}

// The GI target is half resolution. Blend the four nearest GI texels, but
// only those that lie on the same surface as this pixel, so indirect light
// does not bleed across silhouettes.
vec3 upsampleIndirect(vec2 _uv, vec3 _position, vec3 _normal)
{
	vec2 texelCoord = _uv * u_screenInfo.xy - 0.5;
	vec2 baseCoord = floor(texelCoord);
	vec2 blend = texelCoord - baseCoord;

	vec3 sum = vec3_splat(0.0);
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
		weightSum += weight;
	}

	return weightSum > 0.0
		? sum / weightSum
		: texture2DLod(s_gi, _uv, 0.0).rgb;
}

void main()
{
	float depth = texture2D(s_depth, v_texcoord0).x;
	if (depth >= 0.99999)
	{
		// The background: the tone mapping pass gives it a fixed colour.
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	vec4 albedoRoughness = texture2D(s_albedo, v_texcoord0);
	vec4 normalMetallic = texture2D(s_normal, v_texcoord0);
	vec3 albedo = albedoRoughness.rgb;
	float roughness = max(albedoRoughness.a, 0.05);
	float metallic = normalMetallic.a;
	vec3 normal = normalize(normalMetallic.xyz * 2.0 - 1.0);
	vec3 position = reconstructWorldPosition(v_texcoord0, depth);
	vec3 viewDirection = normalize(u_cameraPosition.xyz - position);
	vec3 toLight = u_lightPosition.xyz - position;
	float distanceSquared = max(dot(toLight, toLight), 0.01 * kSceneScale * kSceneScale);
	vec3 lightDirection = toLight * inversesqrt(distanceSquared);
	vec3 halfwayDirection = normalize(viewDirection + lightDirection);

	float normalLight = max(dot(normal, lightDirection), 0.0);
	float normalView = max(dot(normal, viewDirection), 0.0);
	vec3 f0 = mix(vec3_splat(0.04), albedo, metallic);
	vec3 fresnel = fresnelSchlick(halfwayDirection, viewDirection, f0);
	float distribution = distributionGGX(normal, halfwayDirection, roughness);
	float geometry = geometrySchlickGGX(normalLight, roughness)
		* geometrySchlickGGX(normalView, roughness);
	vec3 specular = distribution * geometry * fresnel
		/ max(4.0 * normalLight * normalView, 0.001);
	vec3 diffuseWeight = (vec3_splat(1.0) - fresnel) * (1.0 - metallic);
	vec3 radiance = u_lightColor.rgb / distanceSquared;
	float visibility = normalLight > 0.0 ? lightVisibility(position, normal, 4) : 0.0;
	vec3 direct = (diffuseWeight * albedo / kPi + specular) * radiance * normalLight * visibility;
	// The GI target holds demodulated irradiance: multiply the diffuse albedo
	// back in.
	// (The debug view of irradiance leaves it out.)
	vec3 indirect = upsampleIndirect(v_texcoord0, position, normal)
		* (u_compositeOptions.z > 0.5 ? albedo * (1.0 - metallic) : vec3_splat(1.0));
	// Scene-referred: exposure and tone mapping follow in fs_tonemap.sc.
	gl_FragColor = vec4(direct * u_compositeOptions.x + indirect * u_compositeOptions.y, 1.0);
}
