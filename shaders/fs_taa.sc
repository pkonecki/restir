$input v_texcoord0

/*
 * Temporal anti-aliasing, on the HDR image before tone mapping.
 *
 * Every frame renders with a different sub-pixel jitter (renderer.cpp), so
 * averaging frames over time integrates each pixel's footprint. This pass
 * blends the current frame into the history:
 *
 *   - Reprojection: the surface at this pixel (the nearest depth of the 3x3
 *     block, so edges follow the foreground) is projected with last frame's
 *     unjittered view-projection to find it in the history. The scene is
 *     static, so the camera is the only motion.
 *   - The history is read with a 5-tap Catmull-Rom filter, which keeps it
 *     sharp through repeated resampling.
 *   - Clipping: the history is pulled into the box spanned by the mean plus
 *     or minus the standard deviation of the 3x3 neighbourhood (in YCoCg),
 *     so disocclusions and changed lighting do not ghost.
 *   - Blending in a luminance-weighted space (1 / (1 + luminance)), so a
 *     single bright sample cannot dominate (Karis 2014).
 *
 * Alpha marks the background for the tone mapping pass and is passed on.
 */

#include <common.sh>

SAMPLER2D(s_hdr, 0);        // this frame's composite (point sampled)
SAMPLER2D(s_taaHistory, 1); // last frame's output (bilinear)
SAMPLER2D(s_depth, 2);

uniform mat4 u_sceneInvViewProj;   // this frame's, jittered
uniform mat4 u_taaPreviousViewProj; // last frame's, unjittered
uniform vec4 u_taaInfo; // x: history valid, y: origin bottom-left, zw: texel size

#define kBlend 0.1       // weight of the current frame
#define kClipGamma 1.25  // width of the clip box, in standard deviations

float luminance(vec3 _color)
{
	return dot(_color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 toYCoCg(vec3 _color)
{
	return vec3(
		  dot(_color, vec3( 0.25, 0.5,  0.25))
		, dot(_color, vec3( 0.5,  0.0, -0.5))
		, dot(_color, vec3(-0.25, 0.5, -0.25))
		);
}

vec3 fromYCoCg(vec3 _color)
{
	return vec3(
		  _color.x + _color.y - _color.z
		, _color.x + _color.z
		, _color.x - _color.y - _color.z
		);
}

// Compresses HDR values for the neighbourhood statistics and the blend.
vec3 compress(vec3 _color)
{
	return _color / (1.0 + luminance(_color));
}

vec3 decompress(vec3 _color)
{
	return _color / max(1.0 - luminance(_color), 0.0001);
}

// Catmull-Rom filtering with 5 bilinear taps (the corners dropped), after
// Jimenez, "Filmic SMAA" (2016).
vec3 sampleHistory(vec2 _uv)
{
	vec2 texelSize = u_taaInfo.zw;
	vec2 position = _uv / texelSize;
	vec2 center = floor(position - 0.5) + 0.5;
	vec2 f = position - center;
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);
	vec2 w12 = w1 + w2;
	vec2 offset12 = w2 / w12;

	vec2 uv0 = (center - 1.0) * texelSize;
	vec2 uv3 = (center + 2.0) * texelSize;
	vec2 uv12 = (center + offset12) * texelSize;

	vec3 color =
		  texture2DLod(s_taaHistory, vec2(uv12.x, uv0.y), 0.0).rgb * (w12.x * w0.y)
		+ texture2DLod(s_taaHistory, vec2(uv0.x, uv12.y), 0.0).rgb * (w0.x * w12.y)
		+ texture2DLod(s_taaHistory, vec2(uv12.x, uv12.y), 0.0).rgb * (w12.x * w12.y)
		+ texture2DLod(s_taaHistory, vec2(uv3.x, uv12.y), 0.0).rgb * (w3.x * w12.y)
		+ texture2DLod(s_taaHistory, vec2(uv12.x, uv3.y), 0.0).rgb * (w12.x * w3.y);
	float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
	return max(color / weight, vec3_splat(0.0));
}

// Pulls a colour towards the centre of an axis-aligned box until it is
// inside (clipping rather than clamping keeps the hue).
vec3 clipToBox(vec3 _color, vec3 _center, vec3 _extent)
{
	vec3 offset = _color - _center;
	vec3 units = abs(offset / max(_extent, vec3_splat(0.00001)));
	float largest = max(units.x, max(units.y, units.z));
	return largest > 1.0 ? _center + offset / largest : _color;
}

void main()
{
	vec4 current = texture2DLod(s_hdr, v_texcoord0, 0.0);

	// Neighbourhood statistics, and the nearest depth around the pixel.
	vec3 sum = vec3_splat(0.0);
	vec3 sumSquares = vec3_splat(0.0);
	float nearestDepth = 1.0;
	vec2 nearestUv = v_texcoord0;
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			vec2 uv = v_texcoord0 + vec2(float(x), float(y)) * u_taaInfo.zw;
			vec3 color = toYCoCg(compress(texture2DLod(s_hdr, uv, 0.0).rgb));
			sum += color;
			sumSquares += color * color;
			float depth = texture2DLod(s_depth, uv, 0.0).x;
			if (depth < nearestDepth)
			{
				nearestDepth = depth;
				nearestUv = uv;
			}
		}
	}

	if (u_taaInfo.x < 0.5)
	{
		gl_FragColor = current;
		return;
	}

	// Where the surface was last frame.
	vec3 clipPosition = vec3(nearestUv * 2.0 - 1.0, toClipSpaceDepth(nearestDepth));
	clipPosition.y = toClipSpaceY(clipPosition.y);
	vec3 worldPosition = clipToWorld(u_sceneInvViewProj, clipPosition);
	vec4 previousClip = mul(u_taaPreviousViewProj, vec4(worldPosition, 1.0));
	vec2 previousUv = previousClip.xy / previousClip.w;
	previousUv = vec2(previousUv.x, u_taaInfo.y > 0.5 ? previousUv.y : -previousUv.y) * 0.5 + 0.5;
	// The pixel's own offset within the nearest-depth texel.
	previousUv += v_texcoord0 - nearestUv;
	if (previousClip.w <= 0.0001
	||  previousUv.x < 0.0 || previousUv.x > 1.0 || previousUv.y < 0.0 || previousUv.y > 1.0)
	{
		gl_FragColor = current;
		return;
	}

	vec3 mean = sum / 9.0;
	vec3 deviation = sqrt(max(sumSquares / 9.0 - mean * mean, vec3_splat(0.0)));
	vec3 history = toYCoCg(compress(sampleHistory(previousUv)));
	history = clipToBox(history, mean, deviation * kClipGamma);

	vec3 blended = mix(fromYCoCg(history), compress(current.rgb), kBlend);
	vec3 result = decompress(blended);
	// A NaN anywhere upstream would otherwise stay in the history for good.
	if (any(notEqual(result, result)))
	{
		result = any(notEqual(current.rgb, current.rgb)) ? vec3_splat(0.0) : current.rgb;
	}
	gl_FragColor = vec4(result, current.a);
}
