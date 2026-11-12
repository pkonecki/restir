/*
 * Shared code of the ReSTIR DI passes (fs_di_temporal.sc, fs_di_spatial.sc):
 * direct light from the light list (src/lights.h: emissive triangles and
 * extra point lights), with reservoirs at full resolution.
 *
 * A light sample is a light and a point on it: for a triangle, a point on
 * its surface (measured per unit area); for a point light, its position
 * (a discrete choice). Shifting a sample to another pixel keeps that point,
 * so the shift's Jacobian is 1 and target functions of different pixels
 * compare directly.
 *
 * Reservoir targets (written by the temporal pass):
 *
 *   sample:   xyz = point on the light,  w = light index
 *   receiver: xyz = receiver position,   w = W (unbiased contribution weight)
 *   normal:   xyz = receiver normal,     w = M
 *
 * Every stored sample is visible from its receiver, or carries W = 0.
 */

#ifndef DI_SH_HEADER_GUARD
#define DI_SH_HEADER_GUARD

#include <bgfx_compute.sh>

SAMPLER2D(s_albedo, 0);        // rgb: base colour, a: roughness
SAMPLER2D(s_normal, 1);        // rgb: surface normal, a: metallic
SAMPLER2D(s_depth, 2);
SAMPLER2D(s_shadingNormal, 3); // rgb: normal with the normal map

BUFFER_RO(b_lights, vec4, 14);
#define BVH_STAGE 15
#include "bvh.sh"

uniform mat4 u_sceneInvViewProj;
uniform vec4 u_cameraPosition;
uniform vec4 u_sceneInfo; // x: scene scale
uniform vec4 u_frameInfo; // x: frame index, z: origin bottom-left
uniform vec4 u_diInfo;    // x: light count, y: candidates, z: spatial taps, w: history valid

#define kPi 3.14159265359
#include "brdf.sh"

#define kSceneScale u_sceneInfo.x
#define kSkyDepth 0.99999
#define kDiRayBias (0.01 * kSceneScale)
#define kDiMaxHistory 20.0
#define kDiReceiverRadius (0.1 * kSceneScale) // a reused reservoir's receiver must lie this close
#define kDiReceiverPlane (0.03 * kSceneScale)
#define kDiReceiverNormal 0.9
#define kDiMinDistanceSquared (0.0001 * kSceneScale * kSceneScale)

float luminance(vec3 _color)
{
	return dot(_color, vec3(0.2126, 0.7152, 0.0722));
}

uint pcg(uint _value)
{
	uint state = _value * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float nextRandom(inout uint _state)
{
	_state = pcg(_state);
	return float(_state >> 8u) * (1.0 / 16777216.0);
}

vec3 reconstructWorldPosition(vec2 _uv, float _depth)
{
	vec3 clipPosition = vec3(_uv * 2.0 - 1.0, toClipSpaceDepth(_depth));
	clipPosition.y = toClipSpaceY(clipPosition.y);
	return clipToWorld(u_sceneInvViewProj, clipPosition);
}

struct Surface
{
	vec3 position;
	vec3 normal;        // shading normal
	vec3 surfaceNormal; // without the normal map, for ray offsets
	vec3 albedo;
	float roughness;
	float metallic;
	vec3 eye;           // the camera it is seen from
	bool valid;
};

Surface loadSurface(vec2 _uv)
{
	Surface surface;
	float depth = texture2DLod(s_depth, _uv, 0.0).x;
	vec4 albedoRoughness = texture2DLod(s_albedo, _uv, 0.0);
	vec4 normalMetallic = texture2DLod(s_normal, _uv, 0.0);
	surface.valid = depth < kSkyDepth;
	surface.position = reconstructWorldPosition(_uv, min(depth, kSkyDepth));
	surface.surfaceNormal = normalize(normalMetallic.xyz * 2.0 - 1.0);
	surface.normal = normalize(texture2DLod(s_shadingNormal, _uv, 0.0).xyz * 2.0 - 1.0);
	surface.albedo = albedoRoughness.rgb;
	surface.roughness = albedoRoughness.a;
	surface.metallic = normalMetallic.a;
	surface.eye = u_cameraPosition.xyz;
	return surface;
}

int lightCount()
{
	return int(u_diInfo.x + 0.5);
}

vec4 lightData(int _light, int _row)
{
	return b_lights[_light * 4 + _row];
}

bool isPointLight(int _light)
{
	return lightData(_light, 0).w > 0.5;
}

// Picks a light in proportion to its power: a binary search of the CDF.
int selectLight(float _random, out float _pmf)
{
	int low = 0;
	int high = lightCount() - 1;
	for (int step = 0; step < 24 && low < high; ++step)
	{
		int middle = (low + high) >> 1; // a shift: D3D warns about signed divides
		if (lightData(middle, 2).w < _random)
		{
			low = middle + 1;
		}
		else
		{
			high = middle;
		}
	}

	_pmf = lightData(low, 3).w;
	return low;
}

float triangleArea(int _light)
{
	vec3 v0 = lightData(_light, 0).xyz;
	return 0.5 * length(cross(lightData(_light, 1).xyz - v0, lightData(_light, 2).xyz - v0));
}

// A point on the light: uniform over a triangle's area. _density is the
// probability density of the point given the light (per unit area), 1 for a
// point light.
vec3 sampleLightPoint(int _light, float _random0, float _random1, out float _density)
{
	vec3 v0 = lightData(_light, 0).xyz;
	if (isPointLight(_light))
	{
		_density = 1.0;
		return v0;
	}

	float root = sqrt(_random0);
	float u = 1.0 - root;
	float v = _random1 * root;
	_density = 1.0 / max(triangleArea(_light), 1e-12);
	return v0 + (lightData(_light, 1).xyz - v0) * u + (lightData(_light, 2).xyz - v0) * v;
}

// The probability of a light sample, as selectLight and sampleLightPoint
// produce it.
float lightSamplePdf(int _light)
{
	float pmf = lightData(_light, 3).w;
	return isPointLight(_light) ? pmf : pmf / max(triangleArea(_light), 1e-12);
}

// The unshadowed light a surface reflects towards its eye from a light
// sample, split into diffuse and specular parts. Triangles emit from both
// sides (models wind them inconsistently).
void evaluateLightSample(Surface _surface, int _light, vec3 _point, out vec3 _diffuse, out vec3 _specular)
{
	_diffuse = vec3_splat(0.0);
	_specular = vec3_splat(0.0);
	if (_light < 0 || _light >= lightCount())
	{
		return;
	}

	vec3 toLight = _point - _surface.position;
	float distanceSquared = max(dot(toLight, toLight), kDiMinDistanceSquared);
	vec3 direction = toLight * inversesqrt(distanceSquared);
	if (dot(_surface.surfaceNormal, direction) <= 0.0)
	{
		// Below the surface itself, whatever the normal map says.
		return;
	}

	vec3 radiance = lightData(_light, 3).rgb / distanceSquared;
	if (!isPointLight(_light))
	{
		vec3 v0 = lightData(_light, 0).xyz;
		vec3 lightNormal = normalize(cross(lightData(_light, 1).xyz - v0, lightData(_light, 2).xyz - v0));
		radiance *= abs(dot(lightNormal, direction));
	}

	vec3 diffuse;
	vec3 specular;
	evaluateBrdf(_surface.normal, normalize(_surface.eye - _surface.position), direction
		, _surface.albedo, _surface.roughness, _surface.metallic, diffuse, specular);
	_diffuse = diffuse * radiance;
	_specular = specular * radiance;
}

float lightTarget(Surface _surface, int _light, vec3 _point)
{
	vec3 diffuse;
	vec3 specular;
	evaluateLightSample(_surface, _light, _point, diffuse, specular);
	return luminance(diffuse + specular);
}

// Whether the light point is visible from the surface, through the BVH. The
// ray stops short of the point, so it does not hit the light's own triangle.
bool lightVisible(Surface _surface, vec3 _point)
{
	vec3 origin = _surface.position + _surface.surfaceNormal * kDiRayBias;
	vec3 toPoint = _point - origin;
	float pointDistance = length(toPoint);
	float occluderDistance = 0.0;
	return traceBvh(origin, toPoint / max(pointDistance, 0.0001)
		, pointDistance * 0.999 - kDiRayBias, true, occluderDistance) < 0;
}

// Whether a stored reservoir's receiver is (nearly) this surface.
bool sameSurface(vec3 _position, vec3 _normal, vec3 _otherPosition, vec3 _otherNormal, float _radius)
{
	vec3 offset = _otherPosition - _position;
	return dot(_otherNormal, _otherNormal) > 0.5
		&& dot(offset, offset) <= _radius * _radius
		&& abs(dot(offset, _normal)) <= kDiReceiverPlane
		&& dot(normalize(_otherNormal), _normal) >= kDiReceiverNormal;
}

#endif // DI_SH_HEADER_GUARD
