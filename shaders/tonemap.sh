/*
 * Tone mapping operators: scene-referred linear colour in, display-referred
 * linear colour (0-1) out. The caller encodes the result for the display.
 */

#ifndef TONEMAP_SH_HEADER_GUARD
#define TONEMAP_SH_HEADER_GUARD

// Keep in sync with the Tonemapper enum in settings.h.
#define TONEMAP_ACES     0
#define TONEMAP_AGX      1
#define TONEMAP_NEUTRAL  2
#define TONEMAP_REINHARD 3
#define TONEMAP_CLAMP    4

// ACES filmic, Stephen Hill's fit of the RRT and ODT (sRGB primaries).
vec3 tonemapAces(vec3 _color)
{
	// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
	mat3 inputMatrix = mtxFromRows(
		  vec3(0.59719, 0.35458, 0.04823)
		, vec3(0.07600, 0.90834, 0.01566)
		, vec3(0.02840, 0.13383, 0.83777)
		);
	// ODT_SAT => XYZ => D60_2_D65 => sRGB
	mat3 outputMatrix = mtxFromRows(
		  vec3( 1.60475, -0.53108, -0.07367)
		, vec3(-0.10208,  1.10813, -0.00605)
		, vec3(-0.00327, -0.07276,  1.07602)
		);

	vec3 color = mul(inputMatrix, _color);
	vec3 a = color * (color + 0.0245786) - 0.000090537;
	vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
	color = mul(outputMatrix, a / b);
	return clamp(color, 0.0, 1.0);
}

// AgX (Troy Sobotka), base look, with the sigmoid approximated by a
// polynomial (Benjamin Wrensch's minimal version).
vec3 agxContrast(vec3 _x)
{
	vec3 x2 = _x * _x;
	vec3 x4 = x2 * x2;
	return + 15.5     * x4 * x2
	       - 40.14    * x4 * _x
	       + 31.96    * x4
	       - 6.868    * x2 * _x
	       + 0.4298   * x2
	       + 0.1191   * _x
	       - 0.00232;
}

vec3 tonemapAgx(vec3 _color)
{
	mat3 inset = mtxFromRows(
		  vec3(0.842479062253094,  0.0784335999999992, 0.0792237451477643)
		, vec3(0.0423282422610123, 0.878468636469772,  0.0791661274605434)
		, vec3(0.0423756549057051, 0.0784336,          0.879142973793104)
		);
	mat3 outset = mtxFromRows(
		  vec3( 1.19687900512017,   -0.0980208811401368, -0.0990297440797205)
		, vec3(-0.0528968517574562,  1.15190312990417,   -0.0989611768448433)
		, vec3(-0.0529716355144438, -0.0980434501171241,  1.15107367264116)
		);
	float minEv = -12.47393;
	float maxEv = 4.026069;

	vec3 color = mul(inset, max(_color, vec3_splat(0.0)));
	color = clamp(log2(max(color, vec3_splat(1e-10))), minEv, maxEv);
	color = (color - minEv) / (maxEv - minEv);
	color = agxContrast(color);
	color = mul(outset, color);
	// The curve's output is display-encoded (2.2 gamma); return it linear.
	return pow(clamp(color, 0.0, 1.0), vec3_splat(2.2));
}

// Khronos PBR Neutral: keeps base colours as authored up to a highlight
// threshold, then compresses towards white.
vec3 tonemapNeutral(vec3 _color)
{
	float startCompression = 0.8 - 0.04;
	float desaturation = 0.15;

	float x = min(_color.r, min(_color.g, _color.b));
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	vec3 color = _color - offset;

	float peak = max(color.r, max(color.g, color.b));
	if (peak < startCompression)
	{
		return clamp(color, 0.0, 1.0);
	}

	float d = 1.0 - startCompression;
	float newPeak = 1.0 - d * d / (peak + d - startCompression);
	color *= newPeak / peak;

	float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
	return clamp(mix(color, vec3_splat(newPeak), g), 0.0, 1.0);
}

vec3 tonemap(vec3 _color, int _operator)
{
	if (_operator == TONEMAP_ACES)
	{
		return tonemapAces(_color);
	}
	if (_operator == TONEMAP_AGX)
	{
		return tonemapAgx(_color);
	}
	if (_operator == TONEMAP_NEUTRAL)
	{
		return tonemapNeutral(_color);
	}
	if (_operator == TONEMAP_REINHARD)
	{
		return _color / (_color + vec3_splat(1.0));
	}
	return clamp(_color, 0.0, 1.0);
}

#endif // TONEMAP_SH_HEADER_GUARD
