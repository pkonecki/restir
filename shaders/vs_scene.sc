/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

$input a_position, a_normal, a_color0, a_texcoord0, a_texcoord1, a_tangent
$output v_worldPosition, v_worldNormal, v_albedo, v_material, v_texcoord0, v_worldTangent

#include <common.sh>

void main()
{
	vec4 worldPosition = mul(u_model[0], vec4(a_position, 1.0));
	vec3 worldNormal = mul(u_model[0], vec4(a_normal, 0.0)).xyz;

	gl_Position = mul(u_viewProj, worldPosition);
	v_worldPosition = worldPosition.xyz;
	v_worldNormal = normalize(worldNormal);
	v_albedo = a_color0;
	v_material = a_texcoord1;
	v_texcoord0 = a_texcoord0;
	// Zero when the material has no normal map.
	v_worldTangent = vec4(mul(u_model[0], vec4(a_tangent.xyz, 0.0)).xyz, a_tangent.w);
}
