$input v_texcoord0

/*
 * Exposure and tone mapping, from the composite's HDR target to the window.
 *
 * The exposure is 2^EV, times, with auto exposure, the factor that brings
 * the adapted log-average luminance (cs_exposure.sc) to middle grey. The
 * background (alpha 0) gets a fixed display colour. Debug views that are
 * already display colours pass through unchanged.
 */

#include <common.sh>
#include <bgfx_compute.sh>
#include "tonemap.sh"

SAMPLER2D(s_hdr, 0);
BUFFER_RO(b_exposure, uint, 1);

uniform vec4 u_tonemap; // x: exposure value (EV), y: auto exposure, z: operator (tonemap.sh), w: pass through

#define kMiddleGrey 0.18

void main()
{
	vec4 hdr = texture2DLod(s_hdr, v_texcoord0, 0.0);
	if (u_tonemap.w > 0.5)
	{
		gl_FragColor = vec4(hdr.rgb, 1.0);
		return;
	}

	if (hdr.a < 0.5)
	{
		gl_FragColor = vec4(0.125, 0.141, 0.169, 1.0);
		return;
	}

	float exposure = exp2(u_tonemap.x);
	if (u_tonemap.y > 0.5)
	{
		exposure *= kMiddleGrey / exp2(uintBitsToFloat(b_exposure[0]));
	}

	vec3 color = tonemap(hdr.rgb * exposure, int(u_tonemap.z));
	gl_FragColor = vec4(toGamma(vec4(color, 1.0)).rgb, 1.0);
}
