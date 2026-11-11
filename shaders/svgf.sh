/*
 * Shared helpers for the SVGF-style denoiser passes (variance estimation and
 * the a-trous wavelet filter). They run at GI resolution and use the GI
 * pass's receiver position and normal targets as their G-buffer.
 *
 * Declare u_sceneInfo before including this file.
 */

#ifndef SVGF_SH_HEADER_GUARD
#define SVGF_SH_HEADER_GUARD

#define kSceneScale      u_sceneInfo.x
#define kPlaneSigma      0.02   // world units (scaled), distance from the tangent plane
#define kNormalPower     64.0
#define kLuminanceSigma  4.0    // in standard deviations of the noise

float luminance(vec3 _color)
{
	return dot(_color, vec3(0.2126, 0.7152, 0.0722));
}

// Edge-stopping weight between a pixel (position, normal) and a neighbour.
// Neighbours off this pixel's tangent plane or with a different normal get
// little weight, so the filter does not blur across geometric edges. Zero for
// sky (no stored normal).
float geometryWeight(vec3 _position, vec3 _normal, vec3 _neighbourPosition, vec3 _neighbourNormal)
{
	if (dot(_neighbourNormal, _neighbourNormal) < 0.5)
	{
		return 0.0;
	}

	float planeDistance = abs(dot(_normal, _neighbourPosition - _position)) / (kPlaneSigma * kSceneScale);
	float normalWeight = pow(max(dot(_normal, normalize(_neighbourNormal)), 0.0), kNormalPower);
	return exp(-planeDistance) * normalWeight;
}

bool isInside(vec2 _uv)
{
	return _uv.x >= 0.0 && _uv.x <= 1.0 && _uv.y >= 0.0 && _uv.y <= 1.0;
}

#endif // SVGF_SH_HEADER_GUARD
