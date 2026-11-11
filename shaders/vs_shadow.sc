$input a_position, a_texcoord0
$output v_worldPosition, v_texcoord0

#include <common.sh>

void main()
{
	vec4 worldPosition = mul(u_model[0], vec4(a_position, 1.0));
	gl_Position = mul(u_viewProj, worldPosition);
	v_worldPosition = worldPosition.xyz;
	v_texcoord0 = a_texcoord0;
}
