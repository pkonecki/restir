$input v_texcoord0

/*
 * Denoiser, step 2: one iteration of the edge-avoiding a-trous wavelet filter
 * (Dammertz et al. 2010, as used by SVGF, Schied et al. 2017).
 *
 * A 5x5 B3-spline kernel whose taps are u_atrousInfo.x texels apart; running
 * it with steps 1, 2, 4, 8, 16 covers a large footprint cheaply. Taps are
 * weighted down when they lie on another surface, or when their luminance
 * differs from this pixel's by more than its noise explains, so converged
 * pixels stay sharp and noisy ones are blurred more. The variance is filtered
 * along with the colour (with squared weights) for the next iteration.
 *
 * Input and output: rgb = demodulated GI, a = luminance variance.
 */

#include <common.sh>

SAMPLER2D(s_filterInput, 0);
SAMPLER2D(s_giReceiver, 1);
SAMPLER2D(s_giNormal, 2);

uniform vec4 u_screenInfo; // xy: GI target size, zw: GI texel size
uniform vec4 u_sceneInfo;  // x: scene scale
uniform vec4 u_atrousInfo; // x: tap spacing in texels

#include "svgf.sh"

float splineWeight(int _offset)
{
	int distance = _offset < 0 ? -_offset : _offset;
	return distance == 0 ? 0.375 : (distance == 1 ? 0.25 : 0.0625);
}

void main()
{
	vec3 normal = texture2DLod(s_giNormal, v_texcoord0, 0.0).xyz;
	if (dot(normal, normal) < 0.5)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	normal = normalize(normal);
	vec3 position = texture2DLod(s_giReceiver, v_texcoord0, 0.0).xyz;
	vec4 center = texture2DLod(s_filterInput, v_texcoord0, 0.0);
	float centerLuminance = luminance(center.rgb);

	// The variance of a single pixel is itself noisy: blur it over 3x3 first.
	float variance = 0.0;
	for (int blurY = -1; blurY <= 1; ++blurY)
	{
		for (int blurX = -1; blurX <= 1; ++blurX)
		{
			float weight = (blurX == 0 ? 0.5 : 0.25) * (blurY == 0 ? 0.5 : 0.25);
			variance += texture2DLod(s_filterInput, v_texcoord0 + vec2(float(blurX), float(blurY)) * u_screenInfo.zw, 0.0).a * weight;
		}
	}

	float luminanceScale = 1.0 / (kLuminanceSigma * sqrt(max(variance, 0.0)) + 0.0001);

	vec3 colorSum = vec3_splat(0.0);
	float varianceSum = 0.0;
	float weightSum = 0.0;
	for (int y = -2; y <= 2; ++y)
	{
		for (int x = -2; x <= 2; ++x)
		{
			vec2 uv = v_texcoord0 + vec2(float(x), float(y)) * (u_atrousInfo.x * u_screenInfo.zw);
			if (!isInside(uv))
			{
				continue;
			}

			vec4 tap = texture2DLod(s_filterInput, uv, 0.0);
			float weight = splineWeight(x) * splineWeight(y);
			if (x != 0 || y != 0)
			{
				weight *= geometryWeight(
					  position
					, normal
					, texture2DLod(s_giReceiver, uv, 0.0).xyz
					, texture2DLod(s_giNormal, uv, 0.0).xyz
					);
				weight *= exp(-abs(luminance(tap.rgb) - centerLuminance) * luminanceScale);
			}

			colorSum += tap.rgb * weight;
			varianceSum += tap.a * weight * weight;
			weightSum += weight;
		}
	}

	gl_FragColor = vec4(colorSum / weightSum, varianceSum / (weightSum * weightSum));
}
