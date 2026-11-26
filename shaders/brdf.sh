/*
 * The surface model shared by the composite and the direct lighting passes:
 * Lambertian diffuse plus GGX specular (Smith-Schlick geometry, Schlick
 * Fresnel), metallic-roughness parameterised as glTF has it.
 */

#ifndef BRDF_SH_HEADER_GUARD
#define BRDF_SH_HEADER_GUARD

#ifndef kPi
#	define kPi 3.14159265359
#endif

float distributionGGX(vec3 _normal, vec3 _halfway, float _roughness)
{
	float alpha = _roughness * _roughness;
	float alphaSquared = alpha * alpha;
	float normalHalfway = max(dot(_normal, _halfway), 0.0);
	float denominator = normalHalfway * normalHalfway * (alphaSquared - 1.0) + 1.0;
	return alphaSquared / max(kPi * denominator * denominator, 0.0001);
}

float geometrySchlickGGX(float _normalDirection, float _roughness)
{
	float k = (_roughness + 1.0) * (_roughness + 1.0) / 8.0;
	return _normalDirection / max(_normalDirection * (1.0 - k) + k, 0.0001);
}

vec3 fresnelSchlick(vec3 _halfway, vec3 _viewDirection, vec3 _f0)
{
	// Clamped to 1 as well: with the light right behind the viewer the
	// halfway vector equals the view direction, the dot product can round
	// above 1, and pow() of a negative number is NaN (which TAA would then
	// keep forever).
	float cosine = clamp(dot(_halfway, _viewDirection), 0.0, 1.0);
	return _f0 + (1.0 - _f0) * pow(1.0 - cosine, 5.0);
}

// The BRDF times the cosine at the surface, for light arriving from
// _lightDirection, split into its diffuse and specular parts.
void evaluateBrdf(
	  vec3 _normal
	, vec3 _viewDirection
	, vec3 _lightDirection
	, vec3 _albedo
	, float _roughness
	, float _metallic
	, out vec3 _diffuse
	, out vec3 _specular
	)
{
	float normalLight = max(dot(_normal, _lightDirection), 0.0);
	float normalView = max(dot(_normal, _viewDirection), 0.0);
	vec3 halfway = normalize(_viewDirection + _lightDirection);
	vec3 f0 = mix(vec3_splat(0.04), _albedo, _metallic);
	vec3 fresnel = fresnelSchlick(halfway, _viewDirection, f0);
	float roughness = max(_roughness, 0.05);
	float distribution = distributionGGX(_normal, halfway, roughness);
	float geometry = geometrySchlickGGX(normalLight, roughness) * geometrySchlickGGX(normalView, roughness);
	_specular = distribution * geometry * fresnel / max(4.0 * normalLight * normalView, 0.001) * normalLight;
	_diffuse = (vec3_splat(1.0) - fresnel) * (1.0 - _metallic) * _albedo / kPi * normalLight;
}

// The split-sum specular albedo: the fraction of light from all directions a
// GGX surface reflects towards the viewer, averaged over the hemisphere, as a
// scale and bias of F0. Karis's analytic fit ("Physically Based Shading on
// Mobile", 2014).
vec3 specularAlbedo(vec3 _f0, float _roughness, float _normalView)
{
	vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
	vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
	vec4 r = c0 * _roughness + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * _normalView)) * r.x + r.y;
	vec2 scaleBias = vec2(-1.04, 1.04) * a004 + r.zw;
	return _f0 * scaleBias.x + scaleBias.y;
}

#endif // BRDF_SH_HEADER_GUARD
