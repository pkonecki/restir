/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

$input v_texcoord0

#include <common.sh>

/*
 * Screen-space ReSTIR GI.
 *
 * A reservoir holds one sample point (a visible surface that bounces light
 * towards the receiver). Every frame:
 *
 *   1. Fresh candidates are traced through the depth buffer and resampled
 *      together with the pixel's reprojected reservoir from the last frame.
 *      This temporal reservoir is what gets stored as history.
 *   2. That canonical reservoir is combined with the temporal reservoirs of
 *      a few nearby pixels using pairwise MIS (Bitterli 2022, Wyman et al.
 *      2023). Unlike plain 1/M weights, this stays unbiased when a neighbour
 *      cannot see the sample this pixel found (or vice versa), which is what
 *      used to darken areas next to occluders. The combined sample is tested
 *      for visibility and only used for shading. It is deliberately not fed
 *      back: storing spatially merged results would compound correlation and
 *      visibility errors frame after frame.
 *
 * Render targets:
 *
 *   0: rgb = temporally accumulated indirect estimate, demodulated (divided
 *            by the diffuse albedo), a = history length
 *   1: xyz = sample position,   w = M (number of candidates seen)
 *   2: xyz = receiver position, w = W (unbiased contribution weight)
 *   3: xyz = receiver normal
 *   4: x   = mean luminance, y = mean squared luminance, z = history length
 *
 * The denoiser (fs_svgf_*.sc) filters target 0 using targets 2-4, and feeds
 * its first filtered iteration back as s_historyColor for the next frame.
 * Demodulating keeps albedo detail out of the filter: the composite pass
 * multiplies the albedo back in.
 *
 * Multiple bounces: the history times the albedo is the indirect radiance
 * leaving each visible surface. When a sample point is shaded, last frame's
 * value at that point is added to its direct radiance, so each frame adds one more
 * bounce and the result converges to all bounces within a few frames. Every
 * reservoir sample is re-shaded each frame, so the target function picks up
 * the extra light as well.
 *
 * All densities are expressed in solid angle at the receiver. Reusing a
 * reservoir at a different receiver therefore needs the reconnection
 * Jacobian, which is why the receiver position is stored with the sample.
 */

SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_normal, 1);
SAMPLER2D(s_depth, 2);
SAMPLER2D(s_historySample, 3);
SAMPLER2D(s_historyReceiver, 4);
SAMPLER2D(s_historyColor, 5);
SAMPLER2D(s_historyNormal, 6);
SAMPLER2D(s_historyMoments, 8);

uniform mat4 u_sceneInvViewProj;
uniform mat4 u_sceneViewProj;
uniform mat4 u_previousViewProj;
uniform vec4 u_lightPosition;
uniform vec4 u_lightColor;
uniform vec4 u_frameInfo;  // x: frame index, y: history valid, z: origin bottom-left, w: multiple bounces
uniform vec4 u_screenInfo; // xy: GI target size, zw: GI texel size
uniform vec4 u_sceneInfo;  // x: scene scale (see main.cpp)

#define SHADOW_SAMPLER_STAGE 7
#include "shadow.sh"

#define INITIAL_CANDIDATES 2
#define SPATIAL_TAPS       4
#define TRACE_STEPS        32
#define REFINE_STEPS       5

// These are #defines on purpose. HLSL turns a global "const float" into a
// uniform that nobody sets, so it would silently read as zero on Direct3D.
//
// Distances are multiplied by kSceneScale, the scene radius relative to the
// scene they were tuned for, so the pass behaves the same at any model scale.
#define kSceneScale u_sceneInfo.x
#define kPi 3.14159265359
#define kSkyDepth 0.99999
#define kMaxHistory 20.0       // cap on the M of every reused reservoir
#define kSpatialRadius 12.0    // in GI texels
#define kMaxJacobian 10.0
#define kMaxAccumulation 16.0  // frames blended into the displayed estimate
#define kRayBias (0.02 * kSceneScale)          // origin offset along the normal
#define kSurfaceBias (0.01 * kSceneScale)      // depth a ray must sink below a surface to count as behind it
#define kMaxRayDistance (3.6 * kSceneScale)
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

	for (int stepIndex = 1; stepIndex <= TRACE_STEPS; ++stepIndex)
	{
		if (previousDistance >= _maxDistance)
		{
			break;
		}

		float rayDistance = min((0.02 + float(stepIndex * stepIndex) * 0.0035) * kSceneScale, _maxDistance);
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
// for a sample point, re-shading it from the current G-buffer. Lo is the
// direct radiance plus, with multiple bounces, last frame's indirect radiance. Returns false
// when the point is no longer a visible surface, in which case the reservoir
// that carried it cannot be reused at all.
bool evaluateSample(
	  vec3 _receiverPosition
	, vec3 _receiverNormal
	, vec3 _receiverAlbedo
	, vec3 _samplePosition
	, out vec3 _contribution
	, out vec3 _sampleNormal
	)
{
	_contribution = vec3_splat(0.0);
	_sampleNormal = vec3(0.0, 0.0, 1.0);

	vec2 sampleUv;
	float sampleViewDepth;
	if (!projectToScreen(_samplePosition, sampleUv, sampleViewDepth))
	{
		return false;
	}

	float sampleDepth = texture2DLod(s_depth, sampleUv, 0.0).x;
	if (sampleDepth >= kSkyDepth)
	{
		return false;
	}

	vec3 surfacePosition = reconstructWorldPosition(sampleUv, sampleDepth);
	if (distance(surfacePosition, _samplePosition) > kSampleTolerance)
	{
		return false;
	}

	_sampleNormal = decodeNormal(sampleUv);
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

	vec3 radiance = evaluateSecondaryDirect(sampleUv, _samplePosition, _sampleNormal)
		+ previousIndirect(_samplePosition, _sampleNormal, diffuseAlbedo(sampleUv));
	_contribution = _receiverAlbedo * radiance * (receiverCosine / kPi);
	return true;
}

// Geometry factor cos(receiver) * cos(sample) / distance^2 between a receiver
// and a sample point. Multiplying a solid-angle density at the receiver by it
// gives the density per unit area at the sample point, a measure that every
// receiver shares. Zero when the two surfaces do not face each other.
float geometryTerm(vec3 _receiverPosition, vec3 _receiverNormal, vec3 _samplePosition, vec3 _sampleNormal)
{
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

// Screen-space visibility between a receiver and a sample point.
bool isVisible(vec3 _receiverPosition, vec3 _receiverNormal, vec3 _samplePosition)
{
	vec3 origin = _receiverPosition + _receiverNormal * kRayBias;
	vec3 toSample = _samplePosition - origin;
	float sampleDistance = length(toSample);
	vec3 occluderPosition;
	return !traceScreenSpaceRay(
		  origin
		, toSample / max(sampleDistance, 0.0001)
		, sampleDistance * 0.95 - 0.05 * kSceneScale
		, occluderPosition
		);
}

// Reconnection Jacobian |d(omega here) / d(omega there)| for moving a sample
// point from the receiver it was found at to this one. Zero if the sample does
// not face the original receiver.
float reconnectionJacobian(vec3 _here, vec3 _there, vec3 _samplePosition, vec3 _sampleNormal)
{
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

	return clamp((cosineHere / cosineThere) * (thereSquared / hereSquared), 0.0, kMaxJacobian);
}

struct Reservoir
{
	vec3 contribution; // integrand f of the selected sample at this receiver
	vec3 position;     // selected sample point
	vec3 normal;       // surface normal at the selected sample point
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
		_reservoir.target = _target;
		return true;
	}

	return false;
}

// Last frame's reservoir at _historyUv, if it belongs to (nearly) the same
// surface as this receiver. The receiver stored with the reservoir is the
// previous frame's, so this also rejects disoccluded history under motion.
struct HistoryReservoir
{
	vec3 samplePosition;
	vec3 receiverPosition;
	vec3 receiverNormal;
	float count;  // M, capped
	float weight; // W
};

bool loadHistory(vec2 _historyUv, vec3 _position, vec3 _normal, out HistoryReservoir _history)
{
	_history.samplePosition = vec3_splat(0.0);
	_history.receiverPosition = vec3_splat(0.0);
	_history.receiverNormal = vec3(0.0, 0.0, 1.0);
	_history.count = 0.0;
	_history.weight = 0.0;
	if (!isOnScreen(_historyUv))
	{
		return false;
	}

	vec4 historySample = texture2DLod(s_historySample, _historyUv, 0.0);
	vec4 historyReceiver = texture2DLod(s_historyReceiver, _historyUv, 0.0);
	vec3 historyNormal = texture2DLod(s_historyNormal, _historyUv, 0.0).xyz;
	if (historySample.w <= 0.0 || dot(historyNormal, historyNormal) < 0.5)
	{
		return false;
	}

	historyNormal = normalize(historyNormal);
	vec3 receiverOffset = historyReceiver.xyz - _position;
	if (dot(receiverOffset, receiverOffset) > kReceiverRadius * kReceiverRadius
	||  abs(dot(receiverOffset, _normal)) > kReceiverPlane
	||  dot(historyNormal, _normal) < kReceiverNormal)
	{
		return false;
	}

	_history.samplePosition = historySample.xyz;
	_history.receiverPosition = historyReceiver.xyz;
	_history.receiverNormal = historyNormal;
	_history.count = min(historySample.w, kMaxHistory);
	_history.weight = historyReceiver.w;
	return true;
}

// Last frame's accumulated GI and luminance moments at this receiver, from
// the four history texels around its reprojected position. Only texels that
// held the same surface (close to this receiver's tangent plane, similar
// normal) contribute, with bilinear weights. Bilinear rather than nearest:
// snapping to the nearest texel shifts the history by up to half a texel
// every frame under camera motion, which adds noise instead of averaging it.
bool reprojectHistory(vec2 _previousUv, vec3 _position, vec3 _normal, out vec3 _color, out vec3 _moments)
{
	_color = vec3_splat(0.0);
	_moments = vec3_splat(0.0);

	vec2 texelCoord = _previousUv * u_screenInfo.xy - 0.5;
	vec2 baseCoord = floor(texelCoord);
	vec2 blend = texelCoord - baseCoord;
	float weightSum = 0.0;
	for (int tapIndex = 0; tapIndex < 4; ++tapIndex)
	{
		vec2 offset = vec2(float(tapIndex & 1), float(tapIndex >> 1));
		vec2 uv = (baseCoord + offset + 0.5) * u_screenInfo.zw;
		if (!isOnScreen(uv))
		{
			continue;
		}

		vec3 receiverPosition = texture2DLod(s_historyReceiver, uv, 0.0).xyz;
		vec3 receiverNormal = texture2DLod(s_historyNormal, uv, 0.0).xyz;
		vec3 receiverOffset = receiverPosition - _position;
		if (dot(receiverNormal, receiverNormal) < 0.5
		||  dot(receiverOffset, receiverOffset) > kBounceRadius * kBounceRadius
		||  abs(dot(receiverOffset, _normal)) > kReceiverPlane
		||  dot(normalize(receiverNormal), _normal) < kReceiverNormal)
		{
			continue;
		}

		vec2 bilinear = mix(vec2_splat(1.0) - blend, blend, offset);
		float weight = bilinear.x * bilinear.y;
		_color += texture2DLod(s_historyColor, uv, 0.0).rgb * weight;
		_moments += texture2DLod(s_historyMoments, uv, 0.0).xyz * weight;
		weightSum += weight;
	}

	if (weightSum < 0.01)
	{
		return false;
	}

	_color /= weightSum;
	_moments /= weightSum;
	return true;
}

void main()
{
	float depth = texture2DLod(s_depth, v_texcoord0, 0.0).x;
	if (depth >= kSkyDepth)
	{
		gl_FragData[0] = vec4_splat(0.0);
		gl_FragData[1] = vec4_splat(0.0);
		gl_FragData[2] = vec4_splat(0.0);
		gl_FragData[3] = vec4_splat(0.0);
		gl_FragData[4] = vec4_splat(0.0);
		return;
	}

	vec3 position = reconstructWorldPosition(v_texcoord0, depth);
	vec3 normal = decodeNormal(v_texcoord0);
	vec3 albedo = diffuseAlbedo(v_texcoord0);
	vec3 rayOrigin = position + normal * kRayBias;

	vec2 pixel = floor(v_texcoord0 * u_screenInfo.xy);
	uint rng = pcg(uint(pixel.x) + pcg(uint(pixel.y) + pcg(uint(u_frameInfo.x))));

	// Initial candidates: cosine-distributed rays, pdf = cos / pi.
	Reservoir canonical = emptyReservoir();
	for (int sampleIndex = 0; sampleIndex < INITIAL_CANDIDATES; ++sampleIndex)
	{
		float random0 = nextRandom(rng);
		float random1 = nextRandom(rng);
		float randomSelect = nextRandom(rng);
		vec3 direction = cosineHemisphere(normal, random0, random1);

		// Every candidate counts, including rays that found nothing.
		canonical.count += 1.0;

		vec3 hitPosition;
		if (!traceScreenSpaceRay(rayOrigin, direction, kMaxRayDistance, hitPosition))
		{
			continue;
		}

		vec3 contribution;
		vec3 hitNormal;
		if (!evaluateSample(position, normal, albedo, hitPosition, contribution, hitNormal))
		{
			continue;
		}

		float target = luminance(contribution);
		float sourcePdf = dot(normal, normalize(hitPosition - position)) / kPi;
		if (target > 0.0 && sourcePdf > 0.0001)
		{
			updateReservoir(canonical, contribution, hitPosition, hitNormal, target, target / sourcePdf, randomSelect);
		}
	}

	bool historyValid = false;
	vec2 previousUv = vec2_splat(0.0);
	vec4 clipPrevious = mul(u_previousViewProj, vec4(position, 1.0));
	if (u_frameInfo.y > 0.5 && clipPrevious.w > 0.0001)
	{
		historyValid = true;
		previousUv = clipToUv(clipPrevious);
	}

	// Temporal reuse: merge this pixel's reprojected reservoir. Both describe
	// (almost) the same receiver, so plain 1/M weights are adequate here.
	HistoryReservoir temporal;
	float randomTemporal = nextRandom(rng);
	if (historyValid && loadHistory(previousUv, position, normal, temporal))
	{
		canonical.count += temporal.count;

		vec3 contribution;
		vec3 sampleNormal;
		if (temporal.weight > 0.0
		&&  evaluateSample(position, normal, albedo, temporal.samplePosition, contribution, sampleNormal))
		{
			float jacobian = reconnectionJacobian(
				position, temporal.receiverPosition, temporal.samplePosition, sampleNormal);
			float target = luminance(contribution);
			float weight = target * temporal.weight * temporal.count * jacobian;
			if (weight > 0.0)
			{
				updateReservoir(canonical, contribution, temporal.samplePosition, sampleNormal, target, weight, randomTemporal);
			}
		}
	}

	// Spatial reuse with pairwise MIS. Each neighbour forms a pair with the
	// canonical reservoir, which takes confidence M_c / k in every pair. With
	// lambda_i = (M_i + M_c/k) / M_total the MIS weights are
	//
	//   m_i(x) = lambda_i * M_i p_i(x) / (M_i p_i(x) + (M_c/k) p_c(x))
	//   m_c(x) = sum_i lambda_i * (M_c/k) p_c(x) / (M_i p_i(x) + (M_c/k) p_c(x))
	//
	// which sum to one for every x and reduce to M_i / M_total when all
	// target functions agree. p_r(x) is the area-measure density of the
	// samples receiver r can produce: its geometry term times its visibility.
	// The shared radiance and albedo factors cancel in the ratios, and only
	// the support matters for unbiasedness. A neighbour that cannot see x
	// thus hands its whole pair share to the canonical reservoir, instead of
	// still counting towards M as plain 1/M weighting does.
	//
	// Weights are accumulated without the 1 / M_total, which is applied when
	// computing W. A tap that lands on another surface is a technique with
	// M_i = 0, which hands its pair share to the canonical reservoir.
	float canonicalCount = canonical.count;
	float pairCanonical = canonicalCount / float(SPATIAL_TAPS);
	float totalCount = canonicalCount;
	bool hasCanonical = canonical.target > 0.000001;
	float canonicalGeometry = hasCanonical
		? geometryTerm(position, normal, canonical.position, canonical.normal)
		: 0.0;
	float canonicalShare = 0.0;

	Reservoir combined = emptyReservoir();
	bool neighbourSelected = false;

	for (int tapIndex = 0; tapIndex < SPATIAL_TAPS; ++tapIndex)
	{
		float radius = kSpatialRadius * sqrt(nextRandom(rng));
		float angle = 2.0 * kPi * nextRandom(rng);
		float randomSelect = nextRandom(rng);
		vec2 historyUv = previousUv + vec2(cos(angle), sin(angle)) * radius * u_screenInfo.zw;

		HistoryReservoir neighbour;
		if (!historyValid || !loadHistory(historyUv, position, normal, neighbour))
		{
			canonicalShare += pairCanonical;
			continue;
		}

		float neighbourCount = neighbour.count;
		float pairWeight = neighbourCount + pairCanonical;
		totalCount += neighbourCount;

		// Canonical sample: how likely could this neighbour have produced it?
		if (hasCanonical)
		{
			float neighbourGeometry = geometryTerm(
				neighbour.receiverPosition, neighbour.receiverNormal, canonical.position, canonical.normal);
			if (neighbourGeometry > 0.0
			&&  !isVisible(neighbour.receiverPosition, neighbour.receiverNormal, canonical.position))
			{
				neighbourGeometry = 0.0;
			}

			float canonicalDensity = pairCanonical * canonicalGeometry;
			float denominator = neighbourCount * neighbourGeometry + canonicalDensity;
			canonicalShare += denominator > 0.0
				? pairWeight * canonicalDensity / denominator
				: pairWeight;
		}

		// Neighbour sample, shifted to this receiver. Its own receiver found it,
		// and visibility from this receiver can be left out of p_c: wherever it
		// is occluded the integrand is zero, so the weights there do not matter.
		vec3 contribution;
		vec3 sampleNormal;
		if (neighbour.weight > 0.0
		&&  evaluateSample(position, normal, albedo, neighbour.samplePosition, contribution, sampleNormal))
		{
			float target = luminance(contribution);
			float jacobian = reconnectionJacobian(
				position, neighbour.receiverPosition, neighbour.samplePosition, sampleNormal);
			float hereGeometry = geometryTerm(position, normal, neighbour.samplePosition, sampleNormal);
			float thereGeometry = geometryTerm(
				neighbour.receiverPosition, neighbour.receiverNormal, neighbour.samplePosition, sampleNormal);
			float neighbourDensity = neighbourCount * thereGeometry;
			float denominator = neighbourDensity + pairCanonical * hereGeometry;
			float misWeight = denominator > 0.0 ? pairWeight * neighbourDensity / denominator : 0.0;
			float weight = misWeight * target * neighbour.weight * jacobian;
			if (weight > 0.0
			&&  updateReservoir(combined, contribution, neighbour.samplePosition, sampleNormal, target, weight, randomSelect))
			{
				neighbourSelected = true;
			}
		}
	}

	// The canonical reservoir enters with m_c(y_c) * p_c(y_c) * W_c, where
	// W_c = weightSum / (M_c * p_c(y_c)).
	if (hasCanonical)
	{
		float weight = canonicalShare * canonical.weightSum / canonicalCount;
		if (updateReservoir(combined, canonical.contribution, canonical.position, canonical.normal
			, canonical.target, weight, nextRandom(rng)))
		{
			neighbourSelected = false;
		}
	}

	float combinedWeight = combined.target > 0.000001
		? combined.weightSum / (totalCount * combined.target)
		: 0.0;

	// A neighbour's sample may be occluded from this receiver. The canonical
	// sample needs no test: it was found from this receiver in the first place.
	if (combinedWeight > 0.0 && neighbourSelected && !isVisible(position, normal, combined.position))
	{
		combinedWeight = 0.0;
	}

	vec3 estimate = min(combined.contribution * combinedWeight, vec3_splat(8.0));

	// Demodulate: divide out the diffuse albedo. Where it is zero (metals,
	// black) the estimate is zero too.
	vec3 irradiance = estimate / max(albedo, vec3_splat(0.001));
	float irradianceLuminance = luminance(irradiance);

	// The reservoirs converge on the right distribution, but each pixel still
	// shades a single sample. Average the result over time, along with the
	// luminance moments the denoiser derives its noise estimate from.
	vec3 previousColor = vec3_splat(0.0);
	vec3 previousMoments = vec3_splat(0.0);
	if (!historyValid || !reprojectHistory(previousUv, position, normal, previousColor, previousMoments))
	{
		previousMoments = vec3_splat(0.0);
	}

	float historyLength = min(previousMoments.z + 1.0, kMaxAccumulation);
	float blendWeight = 1.0 / historyLength;
	vec3 color = mix(previousColor, irradiance, blendWeight);
	vec2 moments = mix(
		  previousMoments.xy
		, vec2(irradianceLuminance, irradianceLuminance * irradianceLuminance)
		, blendWeight
		);

	float canonicalWeight = hasCanonical
		? canonical.weightSum / (canonicalCount * canonical.target)
		: 0.0;

	gl_FragData[0] = vec4(color, historyLength);
	gl_FragData[1] = vec4(canonical.position, canonicalCount);
	gl_FragData[2] = vec4(position, canonicalWeight);
	gl_FragData[3] = vec4(normal, 0.0);
	gl_FragData[4] = vec4(moments, historyLength, 0.0);
}
