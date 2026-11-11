$input v_worldPosition

#include <common.sh>

uniform vec4 u_lightPosition;
uniform vec4 u_shadowInfo; // x: far plane (see shadow.sh)

// Distance from the light, normalised by the far plane. Every face is drawn,
// front or back facing: surfaces hidden from the camera still block light.
void main()
{
	gl_FragColor = vec4(length(v_worldPosition - u_lightPosition.xyz) / u_shadowInfo.x, 0.0, 0.0, 1.0);
}
