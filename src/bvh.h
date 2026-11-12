#pragma once

// A bounding volume hierarchy over every triangle of the scene, for the GI
// passes to trace rays that the depth buffer cannot answer (off-screen or
// hidden geometry). See shaders/bvh.sh for the traversal.
//
// Everything goes into one array of vec4s, uploaded as a single GPU buffer:
//
//   nodes      2 vec4 each: (min.xyz, first), (max.xyz, count)
//              count > 0: a leaf with triangles [first, first + count)
//              count = 0: children at nodes first and first + 1
//   triangles  4 vec4 each: (v0.xyz, material), (v1.xyz, emission.r),
//              (v2.xyz, emission.g), (diffuse albedo.rgb, emission.b), in BVH
//              order; albedo and emission come from the material's textures
//              over the triangle (its UV centre, at the mip level that
//              matches its size in the texture)
//
// Indices are stored as float values (exact below 2^24), not as bits, so no
// GPU can flush them as denormals.

#include "scene.h"

#include <cstdint>
#include <vector>

// A triangle whose material emits light, for the direct lighting (see
// lights.h). Its index is its place in the BVH's triangle order.
struct EmissiveTriangle
{
	uint32_t m_triangle = 0;
	float m_vertices[3][3] = {};
	float m_emission[3] = {};
};

struct SceneBvh
{
	std::vector<EmissiveTriangle> m_emitters;
	std::vector<float> m_data; // 4 floats per vec4
	uint32_t m_nodeCount = 0;
	uint32_t m_triangleCount = 0;
	uint32_t m_triangleOffset = 0; // in vec4s
	uint32_t m_depth = 0;          // of the deepest leaf, root = 1
};

// The deepest the builder goes; leaves below it hold more triangles. The
// traversal stack in shaders/bvh.sh must be at least this deep.
constexpr uint32_t kBvhMaxDepth = 40;

SceneBvh buildBvh(const SceneMesh& _mesh);
