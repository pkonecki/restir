#pragma once

// The light list that ReSTIR DI samples (shaders/di.sh): the scene's
// emissive triangles and, optionally, extra point lights scattered through
// the scene. The main point light is not in it: it keeps its shadow map.
//
// Each light is 4 vec4s:
//
//   (v0.xyz, type)       type 0: triangle, 1: point light (v0 is its position)
//   (v1.xyz, triangle)   the triangle's index in the BVH, -1 for a point light
//   (v2.xyz, cdf)        cumulative selection probability, up to this light
//   (emission.rgb, pmf)  radiance (triangle) or intensity (point light), and
//                        the probability of selecting this light
//
// Lights are selected in proportion to their power.

#include "bvh.h"

#include <bx/math.h>

#include <cstdint>
#include <vector>

struct ExtraLights
{
	int m_count = 0;
	float m_intensity = 1.0f;
	uint32_t m_seed = 1;
};

struct LightList
{
	std::vector<float> m_data; // 16 floats per light
	uint32_t m_count = 0;
	uint32_t m_triangles = 0;  // of which emissive triangles
};

// _intensityScale converts an intensity of 1 into the scene's units (the
// point light's scale, see Renderer::render).
LightList buildLightList(
	  const std::vector<EmissiveTriangle>& _emitters
	, const ExtraLights& _extra
	, const bx::Vec3& _boundsMin
	, const bx::Vec3& _boundsMax
	, float _intensityScale
	);
