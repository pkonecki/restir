/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

$input v_worldPosition, v_worldNormal, v_albedo, v_material, v_texcoord0, v_worldTangent

#include <common.sh>

// Materials without a texture get a 1x1 stand-in (white, or a flat normal),
// so the factors (passed per vertex or in uniforms) apply unchanged.
SAMPLER2D(s_baseColor, 0);         // sRGB-encoded colour, a = coverage
SAMPLER2D(s_metallicRoughness, 1); // g = roughness, b = metallic (glTF layout)
SAMPLER2D(s_normalMap, 2);         // tangent-space normal
SAMPLER2D(s_occlusion, 3);         // r = ambient occlusion
SAMPLER2D(s_emissiveMap, 4);       // sRGB-encoded emitted colour

uniform vec4 u_materialInfo;     // x: alpha cutoff (0: opaque), y: metallic scale, z: two-sided alpha-masked
uniform vec4 u_materialEmissive; // rgb: emissive factor times strength
uniform vec4 u_materialFactors;  // x: normal scale, y: occlusion strength

void main()
{
	// Textures are sampled before anything is discarded, so their derivatives
	// come from uniform control flow (which WGSL requires).
	vec4 baseColor = texture2D(s_baseColor, v_texcoord0);
	vec4 metallicRoughness = texture2D(s_metallicRoughness, v_texcoord0);
	vec3 normalSample = texture2D(s_normalMap, v_texcoord0).xyz;
	float occlusionSample = texture2D(s_occlusion, v_texcoord0).r;
	vec3 emissiveSample = texture2D(s_emissiveMap, v_texcoord0).rgb;

	if (baseColor.a < u_materialInfo.x)
	{
		discard;
	}

	// Drop surfaces seen from behind. The lighting passes assume the stored
	// normal faces the camera, and removing the near walls lets the camera look
	// into the box from any side. The model's triangle winding is inconsistent,
	// so this is decided from the shading normal instead of rasterizer culling.
	// Alpha-masked double-sided cards (foliage) are shown from both sides,
	// with the normal turned towards the camera.
	vec3 worldNormal = normalize(v_worldNormal);
	vec3 cameraPosition = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	vec3 toCamera = normalize(cameraPosition - v_worldPosition);
	float facing = 1.0;
	if (dot(worldNormal, toCamera) <= 0.0)
	{
		if (u_materialInfo.z < 0.5)
		{
			discard;
		}

		facing = -1.0;
	}

	// Normal map, in the tangent frame of the interpolated vertex normal.
	// A zero tangent means the material has none.
	vec3 normal = worldNormal;
	if (dot(v_worldTangent.xyz, v_worldTangent.xyz) > 1e-8)
	{
		vec3 tangent = normalize(v_worldTangent.xyz - worldNormal * dot(worldNormal, v_worldTangent.xyz));
		vec3 bitangent = cross(worldNormal, tangent) * (v_worldTangent.w < 0.0 ? -1.0 : 1.0);
		vec3 tangentNormal = normalSample * 2.0 - 1.0;
		tangentNormal.xy *= u_materialFactors.x;
		normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y + worldNormal * tangentNormal.z);
	}
	normal *= facing;

	// A normal map can tilt the normal past the silhouette. Pull it back so
	// it still faces the camera, as the lighting passes assume.
	float normalView = dot(normal, toCamera);
	if (normalView < 0.05)
	{
		normal = normalize(normal + toCamera * (0.05 - normalView));
	}

	vec3 albedo = v_albedo * toLinear(baseColor.rgb);
	float roughness = v_material.x * metallicRoughness.g;
	float metallic = v_material.y * metallicRoughness.b * u_materialInfo.y;
	float occlusion = 1.0 + u_materialFactors.y * (occlusionSample - 1.0);
	vec3 emissive = u_materialEmissive.rgb * toLinear(emissiveSample);

	// Albedo target: rgb = base colour, a = roughness.
	// Normal target: rgb = encoded surface normal (without the normal map),
	//   a = metallic. The GI passes compare receivers by it, which bumpy
	//   normals would defeat.
	// Emissive target: rgb = emitted radiance, a = ambient occlusion.
	// Shading normal target: rgb = encoded normal with the normal map, for
	//   the direct light in the composite pass.
	gl_FragData[0] = vec4(albedo, roughness);
	gl_FragData[1] = vec4(worldNormal * facing * 0.5 + 0.5, metallic);
	gl_FragData[2] = vec4(emissive, occlusion);
	gl_FragData[3] = vec4(normal * 0.5 + 0.5, 1.0);
}
