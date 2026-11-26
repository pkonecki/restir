/*
 * Omnidirectional shadow map lookup for the point light.
 *
 * The six cube faces are rendered into a 3x2 atlas of a plain 2D texture
 * (face f at column f % 3, row f / 3) instead of a cube texture, so looking a
 * point up uses exactly the matrices that rendered it and needs none of the
 * per-backend cube face conventions. Each texel holds the distance from the
 * light to the nearest surface, divided by the shadow far plane.
 *
 * Every face is rendered with a slightly wider field of view than 90 degrees,
 * leaving a border of texels around the region a face is selected for, so
 * filter taps near a face edge stay inside that face's part of the atlas.
 *
 * Define SHADOW_SAMPLER_STAGE before including this file, after declaring
 * u_lightPosition.
 */

#ifndef SHADOW_SH_HEADER_GUARD
#define SHADOW_SH_HEADER_GUARD

SAMPLER2D(s_shadowMap, SHADOW_SAMPLER_STAGE);

uniform mat4 u_shadowViewProj[6]; // +X, -X, +Y, -Y, +Z, -Z
uniform vec4 u_shadowInfo;        // x: far plane, y: tan(half field of view), z: 1 / face size, w: origin bottom-left
uniform vec4 u_shadowOptions;     // x: shadows enabled

#define kShadowNormalOffset 1.5   // in shadow texels
#define kShadowDepthBias    1.0   // in shadow texels
#define kShadowMaxSlope     8.0   // cap on the slope-scaled bias, so grazing light does not detach shadows

int shadowFace(vec3 _direction)
{
	vec3 absolute = abs(_direction);
	if (absolute.x >= absolute.y && absolute.x >= absolute.z)
	{
		return _direction.x >= 0.0 ? 0 : 1;
	}

	if (absolute.y >= absolute.z)
	{
		return _direction.y >= 0.0 ? 2 : 3;
	}

	return _direction.z >= 0.0 ? 4 : 5;
}

// Atlas texture coordinate of a world position on the given face.
vec2 shadowAtlasUv(vec3 _position, int _face)
{
	vec4 clip = mul(u_shadowViewProj[_face], vec4(_position, 1.0));
	vec2 ndc = clip.xy / clip.w;
	bool originBottomLeft = u_shadowInfo.w > 0.5;
	vec2 local = vec2(ndc.x * 0.5 + 0.5, originBottomLeft ? ndc.y * 0.5 + 0.5 : 0.5 - ndc.y * 0.5);

	float row = _face >= 3 ? 1.0 : 0.0;
	float column = float(_face) - 3.0 * row;
	if (originBottomLeft)
	{
		// View rects are placed from the top, texture rows count from the bottom.
		row = 1.0 - row;
	}

	return (vec2(column, row) + local) * vec2(1.0 / 3.0, 0.5);
}

// Where a surface point falls in the shadow atlas, and the depth it is
// compared against there. _filterRadius is how far, in texels, filter taps
// reach from the centre: on a surface lit at a grazing angle the distance to
// the light changes by tan(angle) texels per texel across the surface, so taps
// compare against nearer parts of the same surface and the bias grows with
// both. Returns false when shadows are off.
bool shadowLookup(vec3 _position, vec3 _normal, float _filterRadius, out vec2 _uv, out float _reference)
{
	_uv = vec2_splat(0.0);
	_reference = 0.0;
	if (u_shadowOptions.x < 0.5)
	{
		return false;
	}

	float lightDistance = length(u_lightPosition.xyz - _position);

	// World-space size of one shadow texel at this distance.
	float texelSize = 2.0 * lightDistance * u_shadowInfo.y * u_shadowInfo.z;

	vec3 position = _position + _normal * (texelSize * kShadowNormalOffset);
	vec3 fromLight = position - u_lightPosition.xyz;
	_uv = shadowAtlasUv(position, shadowFace(fromLight));

	float cosine = clamp(dot(_normal, -normalize(fromLight)), 0.0, 1.0);
	float slope = min(sqrt(1.0 - cosine * cosine) / max(cosine, 0.0001), kShadowMaxSlope);
	float bias = texelSize * (kShadowDepthBias + _filterRadius * slope);
	_reference = (length(fromLight) - bias) / u_shadowInfo.x;
	return true;
}

// Fraction of the point light that reaches _position, from one shadow
// sample (the GI passes, which accumulate over frames anyway).
float lightVisibilityHard(vec3 _position, vec3 _normal)
{
	vec2 uv;
	float reference;
	if (!shadowLookup(_position, _normal, 0.5, uv, reference))
	{
		return 1.0;
	}
	return reference <= texture2DLod(s_shadowMap, uv, 0.0).x ? 1.0 : 0.0;
}

// The same from a 4x4 grid of samples, for soft edges (the composite pass).
// Two separate functions rather than one with a tap count: inlined with a
// count of 1, the loop would run once, which fxc rejects (X3557).
float lightVisibilitySoft(vec3 _position, vec3 _normal)
{
	vec2 uv;
	float reference;
	if (!shadowLookup(_position, _normal, 2.0, uv, reference))
	{
		return 1.0;
	}

	vec2 atlasTexel = vec2(u_shadowInfo.z / 3.0, u_shadowInfo.z * 0.5);
	float lit = 0.0;
	for (int y = 0; y < 4; ++y)
	{
		for (int x = 0; x < 4; ++x)
		{
			vec2 offset = vec2(float(x) - 1.5, float(y) - 1.5) * atlasTexel;
			lit += reference <= texture2DLod(s_shadowMap, uv + offset, 0.0).x ? 1.0 : 0.0;
		}
	}
	return lit / 16.0;
}

#endif // SHADOW_SH_HEADER_GUARD
