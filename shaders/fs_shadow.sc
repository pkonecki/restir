$input v_worldPosition, v_texcoord0

#include <common.sh>

SAMPLER2D(s_baseColor, 0); // a = coverage, for alpha-masked materials

uniform vec4 u_lightPosition;
uniform vec4 u_shadowInfo;   // x: far plane (see shadow.sh)
uniform vec4 u_materialInfo; // x: alpha cutoff (0: opaque)

// Distance from the light, normalised by the far plane. Every face is drawn,
// front or back facing: surfaces hidden from the camera still block light.
// The cut-out parts of alpha-masked surfaces let it through.
void main()
{
	float coverage = texture2D(s_baseColor, v_texcoord0).a;
	if (coverage < u_materialInfo.x)
	{
		discard;
	}

	gl_FragColor = vec4(length(v_worldPosition - u_lightPosition.xyz) / u_shadowInfo.x, 0.0, 0.0, 1.0);
}
