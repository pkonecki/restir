$input v_texcoord0

/*
 * Reference accumulation: the running mean of every frame since the
 * reference started. While it accumulates, the renderer turns off every
 * biased shortcut (reuse, denoiser, TAA, clamps) and keeps jittering the
 * camera, so the mean converges to the unbiased, anti-aliased image of the
 * same rays (renderer.cpp).
 */

#include <common.sh>

SAMPLER2D(s_hdr, 0);               // this frame's composite
SAMPLER2D(s_referenceHistory, 1);  // the mean so far

uniform vec4 u_referenceInfo; // x: weight of this frame, 1 / (frames + 1)

void main()
{
	vec4 current = texture2DLod(s_hdr, v_texcoord0, 0.0);
	vec4 history = texture2DLod(s_referenceHistory, v_texcoord0, 0.0);
	// NaNs would never leave the mean.
	vec3 color = any(notEqual(current.rgb, current.rgb)) ? history.rgb : current.rgb;
	gl_FragColor = vec4(mix(history.rgb, color, u_referenceInfo.x), current.a);
}
