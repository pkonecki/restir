/*
 * Shared code of the ReSTIR GI passes (fs_restir_temporal.sc and
 * fs_restir_spatial.sc).
 *
 * A reservoir holds one sample point: a visible surface that bounces light
 * towards the receiver. All densities are expressed in solid angle at the
 * receiver, so reusing a reservoir at a different receiver needs the
 * reconnection Jacobian, which is why the receiver position is stored with
 * the sample.
 *
 * Per-pixel reservoir targets (written by the temporal pass):
 *
 *   sample:   xyz = sample position,   w = M (number of candidates seen)
 *   receiver: xyz = receiver position, w = W (unbiased contribution weight)
 *   normal:   xyz = receiver normal, w = sample id (see below)
 *
 * Sample ids: 0 for a sample point found in the depth buffer; +-(t + 1) for
 * one found by tracing the BVH, where t is the triangle it lies on and the
 * sign says which side of the triangle's plane normal faces the receiver
 * that found it. A sample point that is a visible surface is shaded from the
 * G-buffer; otherwise, if it lies on a known triangle, from that triangle's
 * data (its average albedo, its plane normal, the point light through the
 * cube shadow map, its emission). So BVH samples survive going off screen or
 * behind other geometry, while depth buffer samples are dropped then.
 *
 * A ray that the BVH confirms leaves the scene samples the sky. Its sample is
 * a direction, not a point: the position holds the unit direction and the id
 * is kSkySampleId. Sky samples are compared between receivers in solid angle
 * (both see the same direction, so the shift's Jacobian is 1), where surface
 * samples are compared per unit area at the sample point.
 *
 * Emissive surfaces: a sample point on one adds its emitted radiance, so
 * emitters that are on screen light the scene through the same reservoirs.
 *
 * Multiple bounces: the denoised history times the albedo is the indirect
 * radiance leaving each visible surface. When a sample point is shaded, last
 * frame's value at that point is added to its direct radiance, so each frame
 * adds one more bounce and the result converges to all bounces within a few
 * frames. Every reservoir sample is re-shaded each frame, so the target
 * function picks up the extra light as well.
 */

#ifndef RESTIR_SH_HEADER_GUARD
#define RESTIR_SH_HEADER_GUARD

#include <bgfx_compute.sh>

SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_normal, 1);
SAMPLER2D(s_depth, 2);
// Last frame's outputs, for the bounce light and the temporal accumulation.
SAMPLER2D(s_historyColor, 5);    // denoised (or raw) demodulated GI
SAMPLER2D(s_historyNormal, 6);   // receiver normal
SAMPLER2D(s_historyReceiver, 9); // receiver position + W
SAMPLER2D(s_emissive, 14);       // rgb: emitted radiance (G-buffer)

uniform mat4 u_sceneInvViewProj;
uniform mat4 u_sceneViewProj;
uniform mat4 u_previousViewProj;
uniform vec4 u_lightPosition;
uniform vec4 u_lightColor;
uniform vec4 u_frameInfo;  // x: frame index, y: history valid, z: origin bottom-left, w: multiple bounces
uniform vec4 u_screenInfo; // xy: GI target size, zw: GI texel size
uniform vec4 u_sceneInfo;  // x: scene scale (see renderer.cpp), y: emissive surfaces light the scene, z: Jacobian clamp, w: outlier clamp
uniform vec4 u_restirOptions; // x: initial candidates, y: temporal taps, z: spatial taps, w: trace steps

#define SHADOW_SAMPLER_STAGE 7
#include "shadow.sh"

#define BVH_STAGE 15
#include "bvh.sh"
#include "sky.sh"

#define GRID_READ_ONLY
#define GRID_CHECKSUM_STAGE 11
#define GRID_COUNT_STAGE    12
#define GRID_ENTRY_STAGE    13
#include "hashgrid.sh"

// Sample counts, set from the settings window (see settings.h).
#define kInitialCandidates int(u_restirOptions.x)
#define kTemporalTaps      int(u_restirOptions.y)
#define kSpatialTaps       int(u_restirOptions.z)
#define kTraceSteps        int(u_restirOptions.w)
#define kReferenceTraceSteps 32.0 // the step spacing below was tuned for this many steps
#define REFINE_STEPS 5

// These are #defines on purpose. HLSL turns a global "const float" into a
// uniform that nobody sets, so it would silently read as zero on Direct3D.
//
// Distances are multiplied by kSceneScale, the scene radius relative to the
// scene they were tuned for, so the passes behave the same at any model scale.
#define kSceneScale u_sceneInfo.x
#define kWorldSpaceReuse (u_gridInfo.w > 0.5)
#define kPi 3.14159265359
#define kSkyDepth 0.99999
#define kMaxHistory 20.0       // cap on the M of every reused reservoir
#define kSpatialRadius 12.0    // screen-space reuse only, in GI texels
#define kJacobianClamp u_sceneInfo.z // 0: unclamped (unbiased), else the largest reconnection Jacobian
#define kOutlierClamp u_sceneInfo.w  // 0: none, else the largest estimate the spatial pass hands on
#define kRayBias (0.02 * kSceneScale)          // origin offset along the normal
#define kSurfaceBias (0.01 * kSceneScale)      // depth a ray must sink below a surface to count as behind it
#define kMaxRayDistance (3.6 * kSceneScale)
#define kMaxBvhRayDistance (4.0 * kSceneScale) // past the far side of the scene (radius 1.75, scaled)
#define kSampleTolerance (0.1 * kSceneScale)   // sample point must still be this close to a visible surface
#define kReceiverRadius (0.4 * kSceneScale)    // max distance between receivers that share reservoirs
#define kReceiverPlane (0.03 * kSceneScale)    // max distance of a reused receiver from our tangent plane
#define kReceiverNormal 0.9    // min cosine between the normals of receivers that share reservoirs
#define kMinDistanceSquared (0.0001 * kSceneScale * kSceneScale)
#define kBounceRadius (0.1 * kSceneScale)      // max distance between a sample point and the history texel it reads bounce light from

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

// Independent uniform numbers in [0, 1). Every reservoir decision must use a
// fresh one, otherwise the selection is no longer proportional to the weights.
float nextRandom(inout uint _state)
{
	_state = pcg(_state);
	return float(_state >> 8u) * (1.0 / 16777216.0);
}

// Diffuse reflectance: base colour, minus the part metals reflect specularly.
vec3 diffuseAlbedo(vec2 _uv)
{
	vec3 albedo = texture2DLod(s_albedo, _uv, 0.0).rgb;
	float metallic = texture2DLod(s_normal, _uv, 0.0).a;
	return albedo * (1.0 - metallic);
}

vec3 decodeNormal(vec2 _uv)
{
	return normalize(texture2DLod(s_normal, _uv, 0.0).xyz * 2.0 - 1.0);
}

vec3 reconstructWorldPosition(vec2 _uv, float _depth)
{
	vec3 clipPosition = vec3(_uv * 2.0 - 1.0, toClipSpaceDepth(_depth));
	clipPosition.y = toClipSpaceY(clipPosition.y);
	return clipToWorld(u_sceneInvViewProj, clipPosition);
}

vec2 clipToUv(vec4 _clip)
{
	float originSign = u_frameInfo.z > 0.5 ? 1.0 : -1.0;
	return vec2(_clip.x, _clip.y * originSign) / _clip.w * 0.5 + 0.5;
}

bool isOnScreen(vec2 _uv)
{
	return _uv.x > 0.001 && _uv.x < 0.999 && _uv.y > 0.001 && _uv.y < 0.999;
}

// GI pixel of this fragment, and its index as the hash grid lists it.
vec2 giPixel(vec2 _uv)
{
	return floor(_uv * u_screenInfo.xy);
}

uint giPixelIndex(vec2 _pixel)
{
	return uint(_pixel.y) * uint(u_screenInfo.x) + uint(_pixel.x);
}

// Texture coordinate of the GI pixel with the given index.
vec2 giPixelUv(uint _index)
{
	uint width = uint(u_screenInfo.x);
	return (vec2(float(_index % width), float(_index / width)) + 0.5) * u_screenInfo.zw;
}

// Projects a world position. _viewDepth is the distance along the view axis.
bool projectToScreen(vec3 _position, out vec2 _uv, out float _viewDepth)
{
	vec4 clip = mul(u_sceneViewProj, vec4(_position, 1.0));
	_viewDepth = clip.w;
	_uv = vec2_splat(0.0);
	if (clip.w <= 0.0001)
	{
		return false;
	}

	_uv = clipToUv(clip);
	return isOnScreen(_uv);
}

vec3 cosineHemisphere(vec3 _normal, float _random0, float _random1)
{
	float radius = sqrt(_random0);
	float angle = 2.0 * kPi * _random1;
	vec3 localDirection = vec3(radius * cos(angle), radius * sin(angle), sqrt(max(1.0 - _random0, 0.0)));

	vec3 tangent = normalize(abs(_normal.y) < 0.99
		? cross(_normal, vec3(0.0, 1.0, 0.0))
		: cross(_normal, vec3(1.0, 0.0, 0.0)));
	vec3 bitangent = cross(_normal, tangent);
	return normalize(tangent * localDirection.x + bitangent * localDirection.y + _normal * localDirection.z);
}

// Radiance leaving a visible diffuse surface due to the point light. One
// unfiltered shadow sample is enough: the result is accumulated over frames.
vec3 evaluateSecondaryDirect(vec2 _uv, vec3 _position, vec3 _normal)
{
	vec3 albedo = diffuseAlbedo(_uv);
	vec3 toLight = u_lightPosition.xyz - _position;
	float distanceSquared = max(dot(toLight, toLight), 0.01 * kSceneScale * kSceneScale);
	float normalLight = max(dot(_normal, toLight * inversesqrt(distanceSquared)), 0.0);
	return albedo * (u_lightColor.rgb / distanceSquared) * (normalLight / kPi)
		* (normalLight > 0.0 ? lightVisibility(_position, _normal, 1) : 0.0);
}

// The same for a point that is not a visible surface, from its triangle.
vec3 evaluateWorldDirect(vec3 _albedo, vec3 _position, vec3 _normal)
{
	vec3 toLight = u_lightPosition.xyz - _position;
	float distanceSquared = max(dot(toLight, toLight), 0.01 * kSceneScale * kSceneScale);
	float normalLight = max(dot(_normal, toLight * inversesqrt(distanceSquared)), 0.0);
	return _albedo * (u_lightColor.rgb / distanceSquared) * (normalLight / kPi)
		* (normalLight > 0.0 ? lightVisibility(_position, _normal, 1) : 0.0);
}

// Larger than any triangle id (+-(t + 1), t < 2^24).
#define kSkySampleId 30000000.0

bool isSkySample(float _sampleId)
{
	return _sampleId > kSkySampleId * 0.5;
}

// Encodes the triangle a BVH ray hit, and the side the ray came from.
float encodeSampleId(int _triangle, vec3 _rayDirection)
{
	vec3 v0 = bvhVertex(_triangle, 0);
	vec3 planeNormal = cross(bvhVertex(_triangle, 1) - v0, bvhVertex(_triangle, 2) - v0);
	return dot(planeNormal, _rayDirection) <= 0.0 ? float(_triangle + 1) : -float(_triangle + 1);
}

// Indirect radiance that left a surface point last frame: the (denoised,
// demodulated) history times the point's diffuse albedo. Zero when the point
// was not visible then, or when multiple bounces are disabled.
vec3 previousIndirect(vec3 _position, vec3 _normal, vec3 _albedo)
{
	if (u_frameInfo.y < 0.5 || u_frameInfo.w < 0.5)
	{
		return vec3_splat(0.0);
	}

	vec4 clip = mul(u_previousViewProj, vec4(_position, 1.0));
	if (clip.w <= 0.0001)
	{
		return vec3_splat(0.0);
	}

	vec2 uv = clipToUv(clip);
	if (!isOnScreen(uv))
	{
		return vec3_splat(0.0);
	}

	// The history texel must hold the same surface: last frame's receiver
	// there has to lie close to the point, on the same plane.
	vec3 receiverPosition = texture2DLod(s_historyReceiver, uv, 0.0).xyz;
	vec3 receiverNormal = texture2DLod(s_historyNormal, uv, 0.0).xyz;
	vec3 offset = receiverPosition - _position;
	if (dot(receiverNormal, receiverNormal) < 0.5
	||  dot(offset, offset) > kBounceRadius * kBounceRadius
	||  abs(dot(offset, _normal)) > kReceiverPlane
	||  dot(normalize(receiverNormal), _normal) < kReceiverNormal)
	{
		return vec3_splat(0.0);
	}

	return texture2DLod(s_historyColor, uv, 0.0).rgb * _albedo;
}

// How far the ray point lies behind the visible surface at its pixel.
// Negative when the ray is in front of the depth buffer (or over the sky).
float depthBehindSurface(vec3 _rayPosition, out vec2 _uv, out vec3 _surfacePosition, out bool _onScreen)
{
	float rayDepth;
	_surfacePosition = vec3_splat(0.0);
	_onScreen = projectToScreen(_rayPosition, _uv, rayDepth);
	if (!_onScreen)
	{
		return -1.0;
	}

	float depth = texture2DLod(s_depth, _uv, 0.0).x;
	if (depth >= kSkyDepth)
	{
		return -1.0;
	}

	_surfacePosition = reconstructWorldPosition(_uv, depth);
	float behind = rayDepth - mul(u_sceneViewProj, vec4(_surfacePosition, 1.0)).w;
	if (behind > 0.0)
	{
		// The depth buffer only knows the surface at the pixel centre. On
		// surfaces seen at a grazing angle that is not accurate enough, so
		// also require the point to be below the surface's tangent plane.
		vec3 surfaceNormal = decodeNormal(_uv);
		if (dot(_rayPosition - _surfacePosition, surfaceNormal) > -kSurfaceBias)
		{
			return -1.0;
		}
	}

	return behind;
}

// Marches the depth buffer and returns the first surface the ray crosses
// within _maxDistance. A crossing is bisected so the hit sits on the surface;
// if the ray merely went behind a silhouette (deep behind the depth buffer) it
// is treated as passing behind the occluder and the march continues.
bool traceScreenSpaceRay(
	  vec3 _origin
	, vec3 _direction
	, float _maxDistance
	, out vec3 _hitPosition
	)
{
	_hitPosition = vec3_splat(0.0);
	float previousDistance = 0.0;
	bool previousInFront = true;

	// Steps grow quadratically and reach kMaxRayDistance after the reference
	// count; other counts keep that reach with coarser or finer steps.
	int traceSteps = kTraceSteps;
	float stepScale = kReferenceTraceSteps / float(traceSteps);
	for (int stepIndex = 1; stepIndex <= traceSteps; ++stepIndex)
	{
		if (previousDistance >= _maxDistance)
		{
			break;
		}

		float stepPosition = float(stepIndex) * stepScale;
		float rayDistance = min((0.02 + stepPosition * stepPosition * 0.0035) * kSceneScale, _maxDistance);
		vec2 uv;
		vec3 surfacePosition;
		bool onScreen;
		float behind = depthBehindSurface(_origin + _direction * rayDistance, uv, surfacePosition, onScreen);
		if (!onScreen)
		{
			// A ray that left the view frustum never re-enters it.
			return false;
		}

		bool inFront = behind <= 0.0;
		if (!inFront && previousInFront)
		{
			float nearDistance = previousDistance;
			float farDistance = rayDistance;
			for (int refineIndex = 0; refineIndex < REFINE_STEPS; ++refineIndex)
			{
				float middle = 0.5 * (nearDistance + farDistance);
				vec2 middleUv;
				vec3 middleSurface;
				bool middleOnScreen;
				float middleBehind = depthBehindSurface(
					_origin + _direction * middle, middleUv, middleSurface, middleOnScreen);
				if (middleOnScreen && middleBehind > 0.0)
				{
					farDistance = middle;
					behind = middleBehind;
					surfacePosition = middleSurface;
				}
				else
				{
					nearDistance = middle;
				}
			}

			float thickness = 0.03 * kSceneScale + 0.03 * farDistance;
			if (behind < thickness)
			{
				_hitPosition = surfacePosition;
				return true;
			}
		}

		previousInFront = inFront;
		previousDistance = rayDistance;
	}

	return false;
}

// Evaluates the integrand f = (albedo / pi) * Lo(sample -> receiver) * cos
// for a sample point, re-shading it from the current G-buffer, or from its
// triangle when it is not a visible surface (see the sample ids above). Lo
// is the direct radiance plus its emission and, with multiple bounces, last
// frame's indirect radiance.
// Returns false when the point can no longer be shaded, in which case the
// reservoir that carried it cannot be reused at all.
bool evaluateSample(
	  vec3 _receiverPosition
	, vec3 _receiverNormal
	, vec3 _receiverAlbedo
	, vec3 _samplePosition
	, float _sampleId
	, out vec3 _contribution
	, out vec3 _sampleNormal
	)
{
	_contribution = vec3_splat(0.0);
	_sampleNormal = vec3(0.0, 0.0, 1.0);

	// The sky, in the stored direction. Its visibility is tested separately.
	if (isSkySample(_sampleId))
	{
		_sampleNormal = -_samplePosition;
		_contribution = _receiverAlbedo * skyRadiance(_samplePosition)
			* (max(dot(_receiverNormal, _samplePosition), 0.0) / kPi);
		return skyEnabled();
	}

	// A visible surface: shade it from the G-buffer.
	bool visibleSurface = false;
	vec2 sampleUv;
	float sampleViewDepth;
	if (projectToScreen(_samplePosition, sampleUv, sampleViewDepth))
	{
		float sampleDepth = texture2DLod(s_depth, sampleUv, 0.0).x;
		visibleSurface = sampleDepth < kSkyDepth
			&& distance(reconstructWorldPosition(sampleUv, sampleDepth), _samplePosition) <= kSampleTolerance;
	}

	// Otherwise only a point on a known triangle can still be shaded.
	int triangleIndex = int(abs(_sampleId) + 0.5) - 1;
	if (!visibleSurface && (triangleIndex < 0 || !bvhAvailable()))
	{
		return false;
	}

	if (visibleSurface)
	{
		_sampleNormal = decodeNormal(sampleUv);
	}
	else
	{
		vec3 v0 = bvhVertex(triangleIndex, 0);
		_sampleNormal = normalize(cross(bvhVertex(triangleIndex, 1) - v0, bvhVertex(triangleIndex, 2) - v0))
			* (_sampleId < 0.0 ? -1.0 : 1.0);
	}

	vec3 toReceiver = _receiverPosition - _samplePosition;
	float distanceSquared = dot(toReceiver, toReceiver);
	if (distanceSquared < kMinDistanceSquared * 0.01)
	{
		return true;
	}

	vec3 directionToReceiver = toReceiver * inversesqrt(distanceSquared);
	float sampleCosine = dot(_sampleNormal, directionToReceiver);
	float receiverCosine = -dot(_receiverNormal, directionToReceiver);
	if (sampleCosine <= 0.0 || receiverCosine <= 0.0)
	{
		// Facing away from each other: a valid sample that carries no light.
		return true;
	}

	// Off-screen points have no history to take bounce light from.
	vec3 radiance = visibleSurface
		? evaluateSecondaryDirect(sampleUv, _samplePosition, _sampleNormal)
			+ previousIndirect(_samplePosition, _sampleNormal, diffuseAlbedo(sampleUv))
			+ texture2DLod(s_emissive, sampleUv, 0.0).rgb * u_sceneInfo.y
		: evaluateWorldDirect(bvhTriangleAlbedo(triangleIndex), _samplePosition, _sampleNormal)
			+ previousIndirect(_samplePosition, _sampleNormal, bvhTriangleAlbedo(triangleIndex))
			+ bvhTriangleEmission(triangleIndex) * u_sceneInfo.y;
	_contribution = _receiverAlbedo * radiance * (receiverCosine / kPi);
	return true;
}

// Geometry factor cos(receiver) * cos(sample) / distance^2 between a receiver
// and a sample point. Multiplying a solid-angle density at the receiver by it
// gives the density per unit area at the sample point, a measure that every
// receiver shares. Zero when the two surfaces do not face each other.
// For a sky sample, just the cosine at the receiver: the density in solid
// angle, which receivers share for a direction.
float geometryTerm(vec3 _receiverPosition, vec3 _receiverNormal, vec3 _samplePosition, vec3 _sampleNormal, float _sampleId)
{
	if (isSkySample(_sampleId))
	{
		return max(dot(_receiverNormal, _samplePosition), 0.0);
	}

	vec3 offset = _samplePosition - _receiverPosition;
	float distanceSquared = dot(offset, offset);
	if (distanceSquared < kMinDistanceSquared * 0.01)
	{
		return 0.0;
	}

	vec3 direction = offset * inversesqrt(distanceSquared);
	float receiverCosine = dot(_receiverNormal, direction);
	float sampleCosine = -dot(_sampleNormal, direction);
	return receiverCosine > 0.0 && sampleCosine > 0.0
		? receiverCosine * sampleCosine / distanceSquared
		: 0.0;
}

// Visibility between a receiver and a sample point: through the BVH when
// every ray uses it, through the depth buffer otherwise. The sky is tested
// through the BVH whenever there is one: the depth buffer cannot tell that
// nothing lies off screen in that direction.
bool isVisible(vec3 _receiverPosition, vec3 _receiverNormal, vec3 _samplePosition, float _sampleId)
{
	vec3 origin = _receiverPosition + _receiverNormal * kRayBias;
	bool sky = isSkySample(_sampleId);
	vec3 toSample = sky ? _samplePosition * kMaxBvhRayDistance : _samplePosition - origin;
	float sampleDistance = length(toSample);
	vec3 direction = toSample / max(sampleDistance, 0.0001);

	// A single return: the D3D compiler reports a potentially uninitialised
	// return value when an early return follows a loop-heavy call.
	bool visible = true;
	if (sky)
	{
		float occluderDistance = 0.0;
		visible = bvhAvailable() && traceBvh(origin, direction, kMaxBvhRayDistance, true, occluderDistance) < 0;
	}
	else if (kRayMode == RAY_MODE_BVH && bvhAvailable())
	{
		float occluderDistance = 0.0;
		visible = traceBvh(origin, direction, sampleDistance - kRayBias, true, occluderDistance) < 0;
	}
	else
	{
		vec3 occluderPosition = vec3_splat(0.0);
		visible = !traceScreenSpaceRay(
			  origin
			, direction
			, sampleDistance * 0.95 - 0.05 * kSceneScale
			, occluderPosition
			);
	}

	return visible;
}

// Reconnection Jacobian |d(omega here) / d(omega there)| for moving a sample
// point from the receiver it was found at to this one. Zero if the sample does
// not face the original receiver.
//
// It needs no clamp to stay stable: the pairwise MIS weight of a shifted
// sample (below) falls with the geometry term at the receiver it came from
// as fast as the Jacobian grows, so their product stays bounded. Clamping it
// (optional, for comparison) darkens wherever samples are reused across
// large changes in distance or angle, such as near corners.
float reconnectionJacobian(vec3 _here, vec3 _there, vec3 _samplePosition, vec3 _sampleNormal, float _sampleId)
{
	if (isSkySample(_sampleId))
	{
		// The same direction from every receiver.
		return 1.0;
	}

	vec3 toHere = _here - _samplePosition;
	vec3 toThere = _there - _samplePosition;
	float hereSquared = max(dot(toHere, toHere), kMinDistanceSquared);
	float thereSquared = max(dot(toThere, toThere), kMinDistanceSquared);
	float cosineHere = dot(_sampleNormal, toHere) * inversesqrt(hereSquared);
	float cosineThere = dot(_sampleNormal, toThere) * inversesqrt(thereSquared);
	if (cosineThere <= 0.001)
	{
		return 0.0;
	}

	float jacobian = max((cosineHere / cosineThere) * (thereSquared / hereSquared), 0.0);
	return kJacobianClamp > 0.0 ? min(jacobian, kJacobianClamp) : jacobian;
}

struct Reservoir
{
	vec3 contribution; // integrand f of the selected sample at this receiver
	vec3 position;     // selected sample point
	vec3 normal;       // surface normal at the selected sample point
	float id;          // sample id (see above)
	float target;      // target function p-hat = luminance(f)
	float weightSum;   // sum of resampling weights
	float count;       // M
};

Reservoir emptyReservoir()
{
	Reservoir reservoir;
	reservoir.contribution = vec3_splat(0.0);
	reservoir.position = vec3_splat(0.0);
	reservoir.normal = vec3(0.0, 0.0, 1.0);
	reservoir.id = 0.0;
	reservoir.target = 0.0;
	reservoir.weightSum = 0.0;
	reservoir.count = 0.0;
	return reservoir;
}

// Streams one candidate into a reservoir. Returns true if it was selected.
bool updateReservoir(
	  inout Reservoir _reservoir
	, vec3 _contribution
	, vec3 _position
	, vec3 _normal
	, float _id
	, float _target
	, float _weight
	, float _random
	)
{
	_reservoir.weightSum += _weight;
	if (_random * _reservoir.weightSum < _weight)
	{
		_reservoir.contribution = _contribution;
		_reservoir.position = _position;
		_reservoir.normal = _normal;
		_reservoir.id = _id;
		_reservoir.target = _target;
		return true;
	}

	return false;
}

// A stored reservoir of another receiver.
struct NeighbourReservoir
{
	vec3 samplePosition;
	float sampleId;
	vec3 receiverPosition;
	vec3 receiverNormal;
	float count;  // M, capped
	float weight; // W
};

// Accepts a stored reservoir for reuse at this receiver if its receiver lies
// on (nearly) the same surface: within _radius, close to our tangent plane,
// with a similar normal.
bool acceptNeighbour(
	  vec4 _sample
	, vec4 _receiver
	, vec4 _receiverNormal // w: sample id
	, vec3 _position
	, vec3 _normal
	, float _radius
	, out NeighbourReservoir _neighbour
	)
{
	_neighbour.samplePosition = _sample.xyz;
	_neighbour.sampleId = _receiverNormal.w;
	_neighbour.receiverPosition = _receiver.xyz;
	_neighbour.receiverNormal = vec3(0.0, 0.0, 1.0);
	_neighbour.count = min(_sample.w, kMaxHistory);
	_neighbour.weight = _receiver.w;
	if (_sample.w <= 0.0 || dot(_receiverNormal.xyz, _receiverNormal.xyz) < 0.5)
	{
		return false;
	}

	_neighbour.receiverNormal = normalize(_receiverNormal.xyz);
	vec3 receiverOffset = _receiver.xyz - _position;
	return dot(receiverOffset, receiverOffset) <= _radius * _radius
		&& abs(dot(receiverOffset, _normal)) <= kReceiverPlane
		&& dot(_neighbour.receiverNormal, _normal) >= kReceiverNormal;
}

// How far apart receivers that share reservoirs may be. Jittered hash grid
// lookups reach up to about one and a half cells away, and far from the
// camera a cell is larger than kReceiverRadius.
float reuseRadius(vec3 _position)
{
	return kWorldSpaceReuse
		? max(kReceiverRadius, 2.0 * gridCellSize(_position))
		: kReceiverRadius;
}

/*
 * Pairwise MIS (Bitterli 2022, Wyman et al. 2023), the pairwise form of the
 * generalized balance heuristic of GRIS (Lin et al. 2022, "Generalized
 * Resampled Importance Sampling"), between a canonical reservoir, found at
 * this receiver, and k reservoirs of other receivers.
 * Each neighbour forms a pair with the canonical reservoir, which takes
 * confidence M_c / k in every pair. With lambda_i = (M_i + M_c/k) / M_total
 * the MIS weights are
 *
 *   m_i(x) = lambda_i * M_i p_i(x) / (M_i p_i(x) + (M_c/k) p_c(x))
 *   m_c(x) = sum_i lambda_i * (M_c/k) p_c(x) / (M_i p_i(x) + (M_c/k) p_c(x))
 *
 * which sum to one for every x and reduce to M_i / M_total when all target
 * functions agree. p_r(x) is the area-measure density of the samples receiver
 * r can produce: its geometry term times its visibility. The shared radiance
 * and albedo factors cancel in the ratios, and only the support matters for
 * unbiasedness. A neighbour that cannot see x thus hands its whole pair share
 * to the canonical reservoir, instead of still counting towards M as plain
 * 1/M weighting does.
 *
 * Weights are accumulated without the 1 / M_total, which is applied when
 * computing W. A tap that found no usable reservoir is a technique with
 * M_i = 0, which hands its pair share to the canonical reservoir. With no
 * taps at all, the canonical reservoir is used as it is.
 *
 * The confidences M are capped at kMaxHistory. GRIS allows any confidence
 * weights, as long as the MIS weights sum to one, so the cap only trades how
 * much the history counts against how fast it adapts; it adds no bias.
 * What does: the optional Jacobian and outlier clamps, screen-space
 * visibility, and samples that can no longer be shaded (README.md).
 *
 * Neighbour samples are shifted to this receiver without a visibility test:
 * wherever they are occluded the integrand is zero, so the weights there do
 * not matter. The selected sample is tested once at the end instead, unless
 * it is the canonical one, which is visible from this receiver already.
 */
struct PairwiseMis
{
	Reservoir combined;
	vec3 canonicalPosition;
	vec3 canonicalNormal;
	float canonicalId;
	float pairCanonical;     // M_c / k
	float canonicalGeometry; // p_c of the canonical sample
	float canonicalShare;    // sum over pairs of the canonical sample's MIS weight, times M_total
	float totalCount;        // M_total
	bool hasCanonical;
	bool neighbourSelected;
};

PairwiseMis beginPairwiseMis(
	  vec3 _position
	, vec3 _normal
	, Reservoir _canonical
	, float _canonicalCount
	, int _taps
	)
{
	PairwiseMis mis;
	mis.combined = emptyReservoir();
	mis.canonicalPosition = _canonical.position;
	mis.canonicalNormal = _canonical.normal;
	mis.canonicalId = _canonical.id;
	mis.pairCanonical = _canonicalCount / float(max(_taps, 1));
	mis.totalCount = _canonicalCount;
	mis.hasCanonical = _canonical.target > 0.000001;
	mis.canonicalGeometry = mis.hasCanonical
		? geometryTerm(_position, _normal, _canonical.position, _canonical.normal, _canonical.id)
		: 0.0;
	// Without neighbours, the canonical sample takes the whole weight.
	mis.canonicalShare = _taps > 0 ? 0.0 : _canonicalCount;
	mis.neighbourSelected = false;
	return mis;
}

void skipNeighbour(inout PairwiseMis _mis)
{
	_mis.canonicalShare += _mis.pairCanonical;
}

void addNeighbour(
	  inout PairwiseMis _mis
	, vec3 _position
	, vec3 _normal
	, vec3 _albedo
	, NeighbourReservoir _neighbour
	, float _random
	)
{
	float neighbourCount = _neighbour.count;
	float pairWeight = neighbourCount + _mis.pairCanonical;
	_mis.totalCount += neighbourCount;

	// Canonical sample: how likely could this neighbour have produced it?
	if (_mis.hasCanonical)
	{
		float neighbourGeometry = geometryTerm(
			_neighbour.receiverPosition, _neighbour.receiverNormal, _mis.canonicalPosition, _mis.canonicalNormal, _mis.canonicalId);
		if (neighbourGeometry > 0.0
		&&  !isVisible(_neighbour.receiverPosition, _neighbour.receiverNormal, _mis.canonicalPosition, _mis.canonicalId))
		{
			neighbourGeometry = 0.0;
		}

		float canonicalDensity = _mis.pairCanonical * _mis.canonicalGeometry;
		float denominator = neighbourCount * neighbourGeometry + canonicalDensity;
		_mis.canonicalShare += denominator > 0.0
			? pairWeight * canonicalDensity / denominator
			: pairWeight;
	}

	// Neighbour sample, shifted to this receiver.
	vec3 contribution;
	vec3 sampleNormal;
	if (_neighbour.weight > 0.0
	&&  evaluateSample(_position, _normal, _albedo, _neighbour.samplePosition, _neighbour.sampleId
		, contribution, sampleNormal))
	{
		float target = luminance(contribution);
		float jacobian = reconnectionJacobian(
			_position, _neighbour.receiverPosition, _neighbour.samplePosition, sampleNormal, _neighbour.sampleId);
		float hereGeometry = geometryTerm(_position, _normal, _neighbour.samplePosition, sampleNormal, _neighbour.sampleId);
		float thereGeometry = geometryTerm(
			_neighbour.receiverPosition, _neighbour.receiverNormal, _neighbour.samplePosition, sampleNormal, _neighbour.sampleId);
		float neighbourDensity = neighbourCount * thereGeometry;
		float denominator = neighbourDensity + _mis.pairCanonical * hereGeometry;
		float misWeight = denominator > 0.0 ? pairWeight * neighbourDensity / denominator : 0.0;
		float weight = misWeight * target * _neighbour.weight * jacobian;
		if (weight > 0.0
		&&  updateReservoir(_mis.combined, contribution, _neighbour.samplePosition, sampleNormal
			, _neighbour.sampleId, target, weight, _random))
		{
			_mis.neighbourSelected = true;
		}
	}
}

// Adds the canonical reservoir, with weight m_c(y_c) * p_c(y_c) * W_c, and
// returns the unbiased contribution weight W of the combined reservoir,
// including the final visibility test.
float finishPairwiseMis(
	  inout PairwiseMis _mis
	, vec3 _position
	, vec3 _normal
	, Reservoir _canonical
	, float _canonicalWeight
	, float _random
	)
{
	if (_mis.hasCanonical)
	{
		float weight = _mis.canonicalShare * _canonical.target * _canonicalWeight;
		if (updateReservoir(_mis.combined, _canonical.contribution, _canonical.position, _canonical.normal, _canonical.id
			, _canonical.target, weight, _random))
		{
			_mis.neighbourSelected = false;
		}
	}

	float combinedWeight = _mis.combined.target > 0.000001
		? _mis.combined.weightSum / (_mis.totalCount * _mis.combined.target)
		: 0.0;

	if (combinedWeight > 0.0 && _mis.neighbourSelected && !isVisible(_position, _normal, _mis.combined.position, _mis.combined.id))
	{
		combinedWeight = 0.0;
	}

	return combinedWeight;
}

#endif // RESTIR_SH_HEADER_GUARD
