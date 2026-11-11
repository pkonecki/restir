/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

$input v_worldPosition, v_worldNormal, v_albedo, v_material

#include <common.sh>

void main()
{
	// Drop surfaces seen from behind. The lighting passes assume the stored
	// normal faces the camera, and removing the near walls lets the camera look
	// into the box from any side. The model's triangle winding is inconsistent,
	// so this is decided from the shading normal instead of rasterizer culling.
	vec3 worldNormal = normalize(v_worldNormal);
	vec3 cameraPosition = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	if (dot(worldNormal, cameraPosition - v_worldPosition) <= 0.0)
	{
		discard;
	}

	// Albedo target: rgb = base colour, a = roughness.
	// Normal target: rgb = encoded normal, a = metallic.
	gl_FragData[0] = vec4(v_albedo, v_material.x);
	gl_FragData[1] = vec4(worldNormal * 0.5 + 0.5, v_material.y);
}
