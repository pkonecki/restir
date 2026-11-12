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
	float cosine = max(dot(_halfway, _viewDirection), 0.0);
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

#endif // BRDF_SH_HEADER_GUARD
