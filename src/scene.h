#pragma once

// glTF scene loading: the triangles, materials and textures of a model,
// flattened into one vertex and index buffer with one draw per material.

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

struct SceneVertex
{
	float m_position[3];
	float m_normal[3];
	float m_color[3];
	float m_material[2]; // roughness, metallic
	float m_texcoord[2];

	static void init()
	{
		ms_layout
			.begin()
			.add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0,    3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.end();
	}

	static bgfx::VertexLayout ms_layout;
};

// A decoded RGBA8 texture with its full mip chain, down to 1x1, mip 0 first.
struct SceneImage
{
	uint16_t m_width = 0;
	uint16_t m_height = 0;
	std::vector<uint8_t> m_data;
};

struct SceneMaterial
{
	int32_t m_baseColorImage = -1;         // index into SceneMesh::m_images, -1: none
	int32_t m_metallicRoughnessImage = -1; // index into SceneMesh::m_images, -1: none
	float m_alphaCutoff = 0.0f;            // alpha-masked materials discard texels below it
	bool m_twoSidedMask = false;           // alpha-masked and double-sided, like foliage cards
	bool m_metallicTextureMissing = false; // has a metallic-roughness texture that could not be loaded
};

// A range of the index buffer drawn with one material.
struct SceneDraw
{
	uint32_t m_firstIndex = 0;
	uint32_t m_indexCount = 0;
	uint32_t m_material = 0;
};

struct SceneMesh
{
	std::vector<SceneVertex> m_vertices;
	std::vector<uint32_t> m_indices; // grouped by material, see m_draws
	std::vector<SceneDraw> m_draws;
	std::vector<SceneMaterial> m_materials; // the glTF materials, then a default one
	std::vector<SceneImage> m_images;

	// Indices of each material while primitives are appended.
	std::vector<std::vector<uint32_t>> m_materialIndices;

	// World-space bounds of all vertices.
	bx::Vec3 m_boundsMin = {  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max() };
	bx::Vec3 m_boundsMax = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

	// Area-weighted sums over emissive triangles, used to place the light.
	bx::Vec3 m_emissiveCentroid = { 0.0f, 0.0f, 0.0f };
	bx::Vec3 m_emissiveNormal = { 0.0f, 0.0f, 0.0f };
	float m_emissiveArea = 0.0f;
};

// What the renderer needs to know about the loaded scene.
struct SceneInfo
{
	bx::Vec3 m_center = { 0.0f, 0.0f, 0.0f };
	float m_radius = 1.0f;
	bx::Vec3 m_lightPosition = { 0.0f, 0.0f, 0.0f };
};

// Loads the default scene of a glTF file. Throws std::runtime_error with a
// readable message if the file cannot be used.
SceneMesh loadScene(const std::filesystem::path& _path);

// Frames the scene and places the point light. The light goes in front of the
// scene's emissive surfaces, or near the top of the bounds if there are none.
SceneInfo describeScene(const SceneMesh& _mesh);
