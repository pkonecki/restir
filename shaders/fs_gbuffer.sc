/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

$input v_worldPosition, v_worldNormal, v_albedo, v_material, v_texcoord0

#include <common.sh>

// Materials without a texture get a 1x1 white one, so the factors (passed
// per vertex) apply unchanged.
SAMPLER2D(s_baseColor, 0);         // sRGB-encoded colour, a = coverage
SAMPLER2D(s_metallicRoughness, 1); // g = roughness, b = metallic (glTF layout)

uniform vec4 u_materialInfo; // x: alpha cutoff (0: opaque), y: metallic scale, z: two-sided alpha-masked

void main()
{
	// Textures are sampled before anything is discarded, so their derivatives
	// come from uniform control flow (which WGSL requires).
	vec4 baseColor = texture2D(s_baseColor, v_texcoord0);
	vec4 metallicRoughness = texture2D(s_metallicRoughness, v_texcoord0);

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
	if (dot(worldNormal, cameraPosition - v_worldPosition) <= 0.0)
	{
		if (u_materialInfo.z < 0.5)
		{
			discard;
		}

		worldNormal = -worldNormal;
	}

	vec3 albedo = v_albedo * toLinear(baseColor.rgb);
	float roughness = v_material.x * metallicRoughness.g;
	float metallic = v_material.y * metallicRoughness.b * u_materialInfo.y;

	// Albedo target: rgb = base colour, a = roughness.
	// Normal target: rgb = encoded normal, a = metallic.
	gl_FragData[0] = vec4(albedo, roughness);
	gl_FragData[1] = vec4(worldNormal * 0.5 + 0.5, metallic);
}
