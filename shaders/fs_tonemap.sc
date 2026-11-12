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
SAMPLER2D(s_reference, 2); // the accumulated reference (fs_reference.sc)

uniform vec4 u_tonemap; // x: exposure value (EV), y: auto exposure, z: operator (tonemap.sh), w: pass through
uniform vec4 u_referenceView; // x: 0 the image, 1 the reference, 2 the relative error against it

// Turbo colour map, polynomial approximation (Google, Apache 2.0).
vec3 turbo(float _x)
{
	vec4 red4 = vec4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
	vec4 green4 = vec4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
	vec4 blue4 = vec4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
	vec2 red2 = vec2(-152.94239396, 59.28637943);
	vec2 green2 = vec2(4.27729857, 2.82956604);
	vec2 blue2 = vec2(-89.90310912, 27.34824973);
	float x = clamp(_x, 0.0, 1.0);
	vec4 v4 = vec4(1.0, x, x * x, x * x * x);
	vec2 v2 = v4.zw * v4.z;
	return vec3(dot(v4, red4) + dot(v2, red2), dot(v4, green4) + dot(v2, green2), dot(v4, blue4) + dot(v2, blue2));
}

#define kMiddleGrey 0.18

void main()
{
	vec4 hdr = texture2DLod(s_hdr, v_texcoord0, 0.0);
	if (u_referenceView.x > 1.5)
	{
		// Relative error in luminance: blue none, red 50% or more; signed
		// darker (left half of the map) or brighter (right half).
		vec4 reference = texture2DLod(s_reference, v_texcoord0, 0.0);
		float imageLuminance = dot(hdr.rgb, vec3(0.2126, 0.7152, 0.0722));
		float referenceLuminance = dot(reference.rgb, vec3(0.2126, 0.7152, 0.0722));
		float error = (imageLuminance - referenceLuminance) / max(referenceLuminance, 0.01);
		gl_FragColor = reference.a < 0.5
			? vec4(0.125, 0.141, 0.169, 1.0)
			: vec4(turbo(0.5 + clamp(error, -0.5, 0.5)), 1.0);
		return;
	}
	if (u_referenceView.x > 0.5)
	{
		hdr = texture2DLod(s_reference, v_texcoord0, 0.0);
	}
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
