#include "common.h"
#include "bgfx_utils.h"
#include "entry/input.h"

#include <bx/commandline.h>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

struct SceneVertex
{
	float m_position[3];
	float m_normal[3];
	float m_color[3];
	float m_material[2]; // roughness, metallic

	static void init()
	{
		ms_layout
			.begin()
			.add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0,    3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
			.end();
	}

	static bgfx::VertexLayout ms_layout;
};

bgfx::VertexLayout SceneVertex::ms_layout;

struct FullscreenVertex
{
	float m_position[3];
	float m_texcoord[2];

	static void init()
	{
		ms_layout
			.begin()
			.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.end();
	}

	static bgfx::VertexLayout ms_layout;
};

bgfx::VertexLayout FullscreenVertex::ms_layout;

struct SceneMesh
{
	std::vector<SceneVertex> m_vertices;
	std::vector<uint32_t> m_indices;

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

template<typename IndexType>
void appendIndices(
	  const fastgltf::Asset& _asset
	, const fastgltf::Accessor& _accessor
	, std::vector<uint32_t>& _indices
	)
{
	for (const IndexType index : fastgltf::iterateAccessor<IndexType>(_asset, _accessor))
	{
		_indices.push_back(uint32_t(index));
	}
}

void appendPrimitive(
	  const fastgltf::Asset& _asset
	, const fastgltf::Primitive& _primitive
	, const fastgltf::math::fmat4x4& _transform
	, SceneMesh& _mesh
	)
{
	if (_primitive.type != fastgltf::PrimitiveType::Triangles)
	{
		// Points, lines and strips have no place in a G-buffer of surfaces.
		std::fprintf(stderr, "Skipping a glTF primitive that is not a triangle list.\n");
		return;
	}

	const auto positionAttribute = _primitive.findAttribute("POSITION");
	if (positionAttribute == _primitive.attributes.end())
	{
		throw std::runtime_error("A glTF primitive has no POSITION attribute.");
	}

	std::vector<fastgltf::math::fvec3> positions;
	for (const auto& position : fastgltf::iterateAccessor<fastgltf::math::fvec3>(
		_asset, _asset.accessors[positionAttribute->accessorIndex]))
	{
		positions.push_back(position);
	}

	std::vector<uint32_t> indices;
	if (_primitive.indicesAccessor.has_value())
	{
		const auto& accessor = _asset.accessors[_primitive.indicesAccessor.value()];
		switch (accessor.componentType)
		{
		case fastgltf::ComponentType::UnsignedByte:
			appendIndices<uint8_t>(_asset, accessor, indices);
			break;
		case fastgltf::ComponentType::UnsignedShort:
			appendIndices<uint16_t>(_asset, accessor, indices);
			break;
		case fastgltf::ComponentType::UnsignedInt:
			appendIndices<uint32_t>(_asset, accessor, indices);
			break;
		default:
			throw std::runtime_error("A glTF index accessor has an invalid component type.");
		}
	}
	else
	{
		indices.resize(positions.size());
		for (uint32_t idx = 0; idx < indices.size(); ++idx)
		{
			indices[idx] = idx;
		}
	}

	if (indices.empty() || indices.size() % 3 != 0)
	{
		throw std::runtime_error("A glTF primitive has an invalid triangle index count.");
	}

	std::vector<fastgltf::math::fvec3> normals;
	const auto normalAttribute = _primitive.findAttribute("NORMAL");
	if (normalAttribute != _primitive.attributes.end())
	{
		for (const auto& normal : fastgltf::iterateAccessor<fastgltf::math::fvec3>(
			_asset, _asset.accessors[normalAttribute->accessorIndex]))
		{
			normals.push_back(normal);
		}

		if (normals.size() != positions.size())
		{
			throw std::runtime_error("A glTF primitive has mismatched POSITION and NORMAL counts.");
		}
	}
	else
	{
		normals.resize(positions.size(), fastgltf::math::fvec3(0.0f));
		for (std::size_t idx = 0; idx < indices.size(); idx += 3)
		{
			const uint32_t a = indices[idx];
			const uint32_t b = indices[idx + 1];
			const uint32_t c = indices[idx + 2];
			if (a >= positions.size() || b >= positions.size() || c >= positions.size())
			{
				throw std::runtime_error("A glTF primitive contains an out-of-range index.");
			}

			const auto faceNormal = fastgltf::math::cross(positions[b] - positions[a], positions[c] - positions[a]);
			normals[a] += faceNormal;
			normals[b] += faceNormal;
			normals[c] += faceNormal;
		}
	}

	const std::size_t baseVertex = _mesh.m_vertices.size();
	if (positions.size() > std::size_t(std::numeric_limits<uint32_t>::max()) - baseVertex)
	{
		throw std::runtime_error("The glTF scene exceeds the 32-bit index-buffer limit.");
	}

	const fastgltf::math::fmat3x3 normalTransform = fastgltf::math::transpose(
		fastgltf::math::inverse(fastgltf::math::fmat3x3(_transform)));

	// glTF material defaults: white, fully rough, fully metallic, not emissive.
	fastgltf::math::fvec4 baseColor(1.0f);
	float roughness = 1.0f;
	float metallic = 1.0f;
	bool isEmissive = false;
	if (_primitive.materialIndex.has_value())
	{
		const fastgltf::Material& material = _asset.materials[_primitive.materialIndex.value()];
		baseColor = material.pbrData.baseColorFactor;
		roughness = float(material.pbrData.roughnessFactor);
		metallic = float(material.pbrData.metallicFactor);
		const float emissiveStrength = float(material.emissiveStrength)
			* std::max({ float(material.emissiveFactor[0]), float(material.emissiveFactor[1]), float(material.emissiveFactor[2]) });
		isEmissive = emissiveStrength > 0.0f;
	}

	for (std::size_t idx = 0; idx < positions.size(); ++idx)
	{
		const auto position = _transform * fastgltf::math::fvec4(
			positions[idx].x(), positions[idx].y(), positions[idx].z(), 1.0f);
		const auto transformedNormal = normalTransform * normals[idx];
		const float normalLength = fastgltf::math::length(transformedNormal);
		const auto normal = normalLength > 1e-12f
			? transformedNormal / normalLength
			: fastgltf::math::fvec3(0.0f, 1.0f, 0.0f);

		SceneVertex vertex;
		vertex.m_position[0] = position.x();
		vertex.m_position[1] = position.y();
		vertex.m_position[2] = position.z();
		vertex.m_normal[0] = normal.x();
		vertex.m_normal[1] = normal.y();
		vertex.m_normal[2] = normal.z();
		vertex.m_color[0] = float(baseColor[0]);
		vertex.m_color[1] = float(baseColor[1]);
		vertex.m_color[2] = float(baseColor[2]);
		vertex.m_material[0] = roughness;
		vertex.m_material[1] = metallic;
		_mesh.m_vertices.push_back(vertex);

		const bx::Vec3 worldPosition = { position.x(), position.y(), position.z() };
		_mesh.m_boundsMin = bx::min(_mesh.m_boundsMin, worldPosition);
		_mesh.m_boundsMax = bx::max(_mesh.m_boundsMax, worldPosition);
	}

	for (const uint32_t index : indices)
	{
		if (index >= positions.size())
		{
			throw std::runtime_error("A glTF primitive contains an out-of-range index.");
		}

		_mesh.m_indices.push_back(uint32_t(baseVertex + index));
	}

	if (isEmissive)
	{
		for (std::size_t idx = 0; idx < indices.size(); idx += 3)
		{
			const auto vertexPosition = [&](std::size_t _corner)
			{
				const float* p = _mesh.m_vertices[baseVertex + indices[idx + _corner]].m_position;
				return bx::Vec3{ p[0], p[1], p[2] };
			};
			const auto vertexNormal = [&](std::size_t _corner)
			{
				const float* n = _mesh.m_vertices[baseVertex + indices[idx + _corner]].m_normal;
				return bx::Vec3{ n[0], n[1], n[2] };
			};

			const bx::Vec3 a = vertexPosition(0);
			const bx::Vec3 b = vertexPosition(1);
			const bx::Vec3 c = vertexPosition(2);
			const float area = 0.5f * bx::length(bx::cross(bx::sub(b, a), bx::sub(c, a)));
			const bx::Vec3 centroid = bx::mul(bx::add(bx::add(a, b), c), 1.0f / 3.0f);
			const bx::Vec3 normal = bx::add(bx::add(vertexNormal(0), vertexNormal(1)), vertexNormal(2));

			_mesh.m_emissiveCentroid = bx::add(_mesh.m_emissiveCentroid, bx::mul(centroid, area));
			_mesh.m_emissiveNormal = bx::add(_mesh.m_emissiveNormal, bx::mul(normal, area));
			_mesh.m_emissiveArea += area;
		}
	}
}

SceneMesh loadScene(const std::filesystem::path& _path)
{
	auto data = fastgltf::GltfDataBuffer::FromPath(_path);
	if (data.error() != fastgltf::Error::None)
	{
		throw std::runtime_error(
			  "Failed to read glTF model '" + _path.string() + "': "
			+ std::string(fastgltf::getErrorMessage(data.error()))
			);
	}

	fastgltf::Parser parser;
	auto assetResult = parser.loadGltf(
		  data.get()
		, _path.parent_path()
		, fastgltf::Options::LoadExternalBuffers
		);
	if (assetResult.error() != fastgltf::Error::None)
	{
		throw std::runtime_error(
			  "Failed to load glTF model '" + _path.string() + "': "
			+ std::string(fastgltf::getErrorMessage(assetResult.error()))
			);
	}

	const auto& asset = assetResult.get();
	if (asset.scenes.empty())
	{
		throw std::runtime_error("The glTF file has no scenes.");
	}
	const std::size_t sceneIndex = asset.defaultScene.has_value() ? asset.defaultScene.value() : 0;
	if (sceneIndex >= asset.scenes.size())
	{
		throw std::runtime_error("The glTF file has an invalid default scene.");
	}

	SceneMesh mesh;
	fastgltf::iterateSceneNodes(
		  asset
		, sceneIndex
		, fastgltf::math::fmat4x4()
		, [&asset, &mesh](const fastgltf::Node& _node, const fastgltf::math::fmat4x4& _transform)
		{
			if (!_node.meshIndex.has_value())
			{
				return;
			}

			const std::size_t meshIndex = _node.meshIndex.value();
			if (meshIndex >= asset.meshes.size())
			{
				throw std::runtime_error("The glTF scene references an invalid mesh.");
			}

			for (const auto& primitive : asset.meshes[meshIndex].primitives)
			{
				appendPrimitive(asset, primitive, _transform, mesh);
			}
		}
		);

	if (mesh.m_vertices.empty() || mesh.m_indices.empty())
	{
		throw std::runtime_error("The glTF scene contains no drawable triangles.");
	}

	return mesh;
}

// Frames the scene and places the point light. The light goes in front of the
// scene's emissive surfaces, or near the top of the bounds if there are none.
SceneInfo describeScene(const SceneMesh& _mesh)
{
	SceneInfo info;
	info.m_center = bx::mul(bx::add(_mesh.m_boundsMin, _mesh.m_boundsMax), 0.5f);
	info.m_radius = std::max(0.5f * bx::length(bx::sub(_mesh.m_boundsMax, _mesh.m_boundsMin)), 1e-3f);

	// Kept away from the emitter so the inverse-square falloff does not blow
	// out the surfaces right next to it.
	const float lightOffset = 0.15f * info.m_radius;
	const float normalLength = bx::length(_mesh.m_emissiveNormal);
	if (_mesh.m_emissiveArea > 0.0f && normalLength > 0.0f)
	{
		const bx::Vec3 centroid = bx::mul(_mesh.m_emissiveCentroid, 1.0f / _mesh.m_emissiveArea);
		const bx::Vec3 normal = bx::mul(_mesh.m_emissiveNormal, 1.0f / normalLength);
		info.m_lightPosition = bx::add(centroid, bx::mul(normal, lightOffset));
	}
	else
	{
		info.m_lightPosition = { info.m_center.x, _mesh.m_boundsMax.y - lightOffset, info.m_center.z };
	}

	return info;
}

// The radius of the scene the shader distances and light intensity were tuned
// for (the bundled Cornell box). Their values are scaled by the actual scene
// radius over this one.
constexpr float kReferenceSceneRadius = 1.75f;

// Point light shadows: six cube faces in a 3x2 atlas (see shaders/shadow.sh).
constexpr uint16_t kShadowFaceSize = 512;
constexpr uint16_t kShadowFaceBorder = 4; // texels kept for filter taps near face edges
constexpr bgfx::ViewId kShadowViewBase = 3; // views 3-8, ordered before the others

// Denoiser (SVGF-style): a variance pass and a-trous iterations with tap
// spacings 1, 2, 4, 8, 16, at GI resolution, between the GI and composite passes.
constexpr bgfx::ViewId kDenoiseViewBase = 9; // views 9-14
constexpr uint8_t kAtrousIterations = 5;

// Orbit camera distances, in scene radii.
constexpr float kInitialCameraDistance = 3.17f;
constexpr float kMinCameraDistance = 1.8f;
constexpr float kMaxCameraDistance = 5.2f;

class ExampleRestirGi : public entry::AppI
{
public:
	ExampleRestirGi(const char* _name, const char* _description, const char* _url)
		: entry::AppI(_name, _description, _url)
		, m_vbh(BGFX_INVALID_HANDLE)
		, m_ibh(BGFX_INVALID_HANDLE)
		, m_fullscreenVbh(BGFX_INVALID_HANDLE)
		, m_fullscreenIbh(BGFX_INVALID_HANDLE)
		, m_gbuffer(BGFX_INVALID_HANDLE)
		, m_gbufferAlbedo(BGFX_INVALID_HANDLE)
		, m_gbufferNormal(BGFX_INVALID_HANDLE)
		, m_gbufferDepth(BGFX_INVALID_HANDLE)
		, m_historyRead(0)
		, m_historyValid(false)
		, m_targetWidth(0)
		, m_targetHeight(0)
		, m_geometryProgram(BGFX_INVALID_HANDLE)
		, m_restirProgram(BGFX_INVALID_HANDLE)
		, m_compositeProgram(BGFX_INVALID_HANDLE)
		, m_lightPosition(BGFX_INVALID_HANDLE)
		, m_lightColor(BGFX_INVALID_HANDLE)
		, m_cameraPosition(BGFX_INVALID_HANDLE)
		, m_albedoSampler(BGFX_INVALID_HANDLE)
		, m_normalSampler(BGFX_INVALID_HANDLE)
		, m_depthSampler(BGFX_INVALID_HANDLE)
		, m_historySampleSampler(BGFX_INVALID_HANDLE)
		, m_historyReceiverSampler(BGFX_INVALID_HANDLE)
		, m_historyColorSampler(BGFX_INVALID_HANDLE)
		, m_historyNormalSampler(BGFX_INVALID_HANDLE)
		, m_giSampler(BGFX_INVALID_HANDLE)
		, m_inverseViewProjection(BGFX_INVALID_HANDLE)
		, m_viewProjection(BGFX_INVALID_HANDLE)
		, m_previousViewProjection(BGFX_INVALID_HANDLE)
		, m_frameInfo(BGFX_INVALID_HANDLE)
		, m_screenInfo(BGFX_INVALID_HANDLE)
		, m_sceneInfoUniform(BGFX_INVALID_HANDLE)
		, m_cameraYaw(0.64f)
		, m_cameraPitch(0.255f)
		, m_cameraDistance(1.0f)
		, m_frameNumber(0)
		, m_lastMouseX(0)
		, m_lastMouseY(0)
		, m_lastMouseWheel(0)
		, m_isDragging(false)
		, m_multipleBounces(true)
		, m_bounceKeyDown(false)
		, m_shadowMap(BGFX_INVALID_HANDLE)
		, m_shadowFrameBuffer(BGFX_INVALID_HANDLE)
		, m_shadowProgram(BGFX_INVALID_HANDLE)
		, m_shadowSampler(BGFX_INVALID_HANDLE)
		, m_shadowViewProjUniform(BGFX_INVALID_HANDLE)
		, m_shadowInfoUniform(BGFX_INVALID_HANDLE)
		, m_shadowOptionsUniform(BGFX_INVALID_HANDLE)
		, m_shadowsEnabled(true)
		, m_shadowKeyDown(false)
		, m_varianceProgram(BGFX_INVALID_HANDLE)
		, m_atrousProgram(BGFX_INVALID_HANDLE)
		, m_historyMomentsSampler(BGFX_INVALID_HANDLE)
		, m_giColorSampler(BGFX_INVALID_HANDLE)
		, m_giMomentsSampler(BGFX_INVALID_HANDLE)
		, m_giReceiverSampler(BGFX_INVALID_HANDLE)
		, m_giNormalSampler(BGFX_INVALID_HANDLE)
		, m_filterInputSampler(BGFX_INVALID_HANDLE)
		, m_atrousInfoUniform(BGFX_INVALID_HANDLE)
		, m_denoiseEnabled(true)
		, m_denoiseKeyDown(false)
	{
		for (uint8_t idx = 0; idx < 2; ++idx)
		{
			m_historyFrameBuffers[idx] = BGFX_INVALID_HANDLE;
			m_historyColor[idx] = BGFX_INVALID_HANDLE;
			m_historySample[idx] = BGFX_INVALID_HANDLE;
			m_historyReceiver[idx] = BGFX_INVALID_HANDLE;
			m_historyNormal[idx] = BGFX_INVALID_HANDLE;
			m_historyMoments[idx] = BGFX_INVALID_HANDLE;
			m_filteredHistory[idx] = BGFX_INVALID_HANDLE;
			m_filteredHistoryFrameBuffers[idx] = BGFX_INVALID_HANDLE;
			m_filterTextures[idx] = BGFX_INVALID_HANDLE;
			m_filterFrameBuffers[idx] = BGFX_INVALID_HANDLE;
		}
		bx::mtxIdentity(m_previousViewProj);
	}

	void init(int32_t _argc, const char* const* _argv, uint32_t _width, uint32_t _height) override
	{
		Args args(_argc, _argv);
		bx::CommandLine commandLine(_argc, _argv);
		const char* modelPath = commandLine.findOption("model", "assets/cornell_box/scene.gltf");

		m_width  = _width;
		m_height = _height;
		m_debug  = BGFX_DEBUG_TEXT;
		m_reset  = BGFX_RESET_VSYNC;

		bgfx::Init init;
		init.type     = args.m_type;
		init.vendorId = args.m_pciId;
		init.swapChain.nwh     = entry::getNativeWindowHandle(entry::kDefaultWindowHandle);
		init.swapChain.ndt     = entry::getNativeDisplayHandle();
		init.platformData.type = entry::getNativeWindowHandleType();
		init.swapChain.width   = m_width;
		init.swapChain.height  = m_height;
		init.reset             = m_reset;
		bgfx::init(init);

		bgfx::setDebug(m_debug);
		bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
		bgfx::setViewClear(2, BGFX_CLEAR_COLOR, 0x20242bff, 1.0f, 0);

		// The shadow faces must be rendered before the GI and composite passes.
		const bgfx::ViewId viewOrder[] =
		{
			kShadowViewBase + 0, kShadowViewBase + 1, kShadowViewBase + 2,
			kShadowViewBase + 3, kShadowViewBase + 4, kShadowViewBase + 5,
			0, 1,
			kDenoiseViewBase + 0, kDenoiseViewBase + 1, kDenoiseViewBase + 2,
			kDenoiseViewBase + 3, kDenoiseViewBase + 4, kDenoiseViewBase + 5,
			2,
		};
		bgfx::setViewOrder(0, BX_COUNTOF(viewOrder), viewOrder);

		SceneVertex::init();
		FullscreenVertex::init();

		if (0 == (bgfx::getCaps()->supported & BGFX_CAPS_INDEX32))
		{
			throw std::runtime_error("The renderer does not support 32-bit index buffers.");
		}

		const SceneMesh mesh = loadScene(modelPath);
		m_scene = describeScene(mesh);
		m_cameraDistance = kInitialCameraDistance * m_scene.m_radius;

		m_vbh = bgfx::createVertexBuffer(
			  bgfx::copy(mesh.m_vertices.data(), uint32_t(mesh.m_vertices.size() * sizeof(SceneVertex)))
			, SceneVertex::ms_layout
			);
		m_ibh = bgfx::createIndexBuffer(
			  bgfx::copy(mesh.m_indices.data(), uint32_t(mesh.m_indices.size() * sizeof(uint32_t)))
			, BGFX_BUFFER_INDEX32
			);
		const FullscreenVertex fullscreenVertices[] =
		{
			{ { -1.0f, -1.0f, 0.0f }, { 0.0f, bgfx::getCaps()->originBottomLeft ? 0.0f : 1.0f } },
			{ {  1.0f, -1.0f, 0.0f }, { 1.0f, bgfx::getCaps()->originBottomLeft ? 0.0f : 1.0f } },
			{ {  1.0f,  1.0f, 0.0f }, { 1.0f, bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f } },
			{ { -1.0f,  1.0f, 0.0f }, { 0.0f, bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f } },
		};
		const uint16_t fullscreenIndices[] = { 0, 1, 2, 0, 2, 3 };
		m_fullscreenVbh = bgfx::createVertexBuffer(
			  bgfx::copy(fullscreenVertices, sizeof(fullscreenVertices))
			, FullscreenVertex::ms_layout
			);
		m_fullscreenIbh = bgfx::createIndexBuffer(
			bgfx::copy(fullscreenIndices, sizeof(fullscreenIndices))
			);

		m_geometryProgram = loadProgram("vs_scene", "fs_gbuffer");
		m_restirProgram = loadProgram("vs_fullscreen", "fs_restir_gi");
		m_compositeProgram = loadProgram("vs_fullscreen", "fs_composite");
		m_shadowProgram = loadProgram("vs_shadow", "fs_shadow");
		m_varianceProgram = loadProgram("vs_fullscreen", "fs_svgf_variance");
		m_atrousProgram = loadProgram("vs_fullscreen", "fs_svgf_atrous");
		m_lightPosition = bgfx::createUniform("u_lightPosition", bgfx::UniformType::Vec4);
		m_lightColor = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4);
		m_cameraPosition = bgfx::createUniform("u_cameraPosition", bgfx::UniformType::Vec4);
		m_albedoSampler = bgfx::createUniform("s_albedo", bgfx::UniformType::Sampler);
		m_normalSampler = bgfx::createUniform("s_normal", bgfx::UniformType::Sampler);
		m_depthSampler = bgfx::createUniform("s_depth", bgfx::UniformType::Sampler);
		m_historySampleSampler = bgfx::createUniform("s_historySample", bgfx::UniformType::Sampler);
		m_historyReceiverSampler = bgfx::createUniform("s_historyReceiver", bgfx::UniformType::Sampler);
		m_historyColorSampler = bgfx::createUniform("s_historyColor", bgfx::UniformType::Sampler);
		m_historyNormalSampler = bgfx::createUniform("s_historyNormal", bgfx::UniformType::Sampler);
		m_giSampler = bgfx::createUniform("s_gi", bgfx::UniformType::Sampler);
		m_inverseViewProjection = bgfx::createUniform("u_sceneInvViewProj", bgfx::UniformType::Mat4);
		m_viewProjection = bgfx::createUniform("u_sceneViewProj", bgfx::UniformType::Mat4);
		m_previousViewProjection = bgfx::createUniform("u_previousViewProj", bgfx::UniformType::Mat4);
		m_frameInfo = bgfx::createUniform("u_frameInfo", bgfx::UniformType::Vec4);
		m_screenInfo = bgfx::createUniform("u_screenInfo", bgfx::UniformType::Vec4);
		m_sceneInfoUniform = bgfx::createUniform("u_sceneInfo", bgfx::UniformType::Vec4);
		m_shadowSampler = bgfx::createUniform("s_shadowMap", bgfx::UniformType::Sampler);
		m_shadowViewProjUniform = bgfx::createUniform("u_shadowViewProj", bgfx::UniformType::Mat4, 6);
		m_shadowInfoUniform = bgfx::createUniform("u_shadowInfo", bgfx::UniformType::Vec4);
		m_shadowOptionsUniform = bgfx::createUniform("u_shadowOptions", bgfx::UniformType::Vec4);
		m_historyMomentsSampler = bgfx::createUniform("s_historyMoments", bgfx::UniformType::Sampler);
		m_giColorSampler = bgfx::createUniform("s_giColor", bgfx::UniformType::Sampler);
		m_giMomentsSampler = bgfx::createUniform("s_giMoments", bgfx::UniformType::Sampler);
		m_giReceiverSampler = bgfx::createUniform("s_giReceiver", bgfx::UniformType::Sampler);
		m_giNormalSampler = bgfx::createUniform("s_giNormal", bgfx::UniformType::Sampler);
		m_filterInputSampler = bgfx::createUniform("s_filterInput", bgfx::UniformType::Sampler);
		m_atrousInfoUniform = bgfx::createUniform("u_atrousInfo", bgfx::UniformType::Vec4);

		BX_ASSERT(bgfx::isValid(m_vbh), "Failed to create the scene vertex buffer.");
		BX_ASSERT(bgfx::isValid(m_ibh), "Failed to create the scene index buffer.");
		BX_ASSERT(bgfx::isValid(m_geometryProgram), "Failed to load G-buffer shaders.");
		BX_ASSERT(bgfx::isValid(m_restirProgram), "Failed to load ReSTIR GI shaders.");
		BX_ASSERT(bgfx::isValid(m_compositeProgram), "Failed to load composite shaders.");
		BX_ASSERT(bgfx::isValid(m_shadowProgram), "Failed to load shadow shaders.");
		BX_ASSERT(bgfx::isValid(m_varianceProgram), "Failed to load denoiser variance shaders.");
		BX_ASSERT(bgfx::isValid(m_atrousProgram), "Failed to load denoiser filter shaders.");
		createRenderTargets(_width, _height);
		createShadowMap();
	}

	int shutdown() override
	{
		destroyRenderTargets();
		if (bgfx::isValid(m_shadowFrameBuffer))
		{
			bgfx::destroy(m_shadowFrameBuffer);
		}
		const bgfx::UniformHandle uniforms[] =
		{
			m_lightPosition, m_lightColor, m_cameraPosition,
			m_albedoSampler, m_normalSampler, m_depthSampler,
			m_historySampleSampler, m_historyReceiverSampler, m_historyColorSampler,
			m_historyNormalSampler, m_giSampler,
			m_inverseViewProjection, m_viewProjection, m_previousViewProjection,
			m_frameInfo, m_screenInfo, m_sceneInfoUniform,
			m_shadowSampler, m_shadowViewProjUniform, m_shadowInfoUniform, m_shadowOptionsUniform,
			m_historyMomentsSampler, m_giColorSampler, m_giMomentsSampler, m_giReceiverSampler,
			m_giNormalSampler, m_filterInputSampler, m_atrousInfoUniform,
		};
		for (const bgfx::UniformHandle uniform : uniforms)
		{
			if (bgfx::isValid(uniform))
			{
				bgfx::destroy(uniform);
			}
		}
		if (bgfx::isValid(m_ibh))
		{
			bgfx::destroy(m_ibh);
		}
		if (bgfx::isValid(m_vbh))
		{
			bgfx::destroy(m_vbh);
		}
		if (bgfx::isValid(m_fullscreenIbh))
		{
			bgfx::destroy(m_fullscreenIbh);
		}
		if (bgfx::isValid(m_fullscreenVbh))
		{
			bgfx::destroy(m_fullscreenVbh);
		}
		const bgfx::ProgramHandle programs[] =
		{
			m_geometryProgram, m_restirProgram, m_compositeProgram, m_shadowProgram,
			m_varianceProgram, m_atrousProgram,
		};
		for (const bgfx::ProgramHandle program : programs)
		{
			if (bgfx::isValid(program))
			{
				bgfx::destroy(program);
			}
		}

		bgfx::shutdown();
		return 0;
	}

	bool update() override
	{
		if (entry::processEvents(m_width, m_height, m_debug, m_reset, &m_mouseState))
		{
			return false;
		}

		updateCameraInput();

		// B toggles feeding last frame's indirect light back into the samples.
		const bool bounceKeyDown = inputGetKeyState(entry::Key::KeyB);
		if (bounceKeyDown && !m_bounceKeyDown)
		{
			m_multipleBounces = !m_multipleBounces;
		}
		m_bounceKeyDown = bounceKeyDown;

		const bool shadowKeyDown = inputGetKeyState(entry::Key::KeyS);
		if (shadowKeyDown && !m_shadowKeyDown)
		{
			m_shadowsEnabled = !m_shadowsEnabled;
		}
		m_shadowKeyDown = shadowKeyDown;

		// D toggles the denoiser. The history means something different with
		// and without it (filtered or not), so restart accumulation.
		const bool denoiseKeyDown = inputGetKeyState(entry::Key::KeyD);
		if (denoiseKeyDown && !m_denoiseKeyDown)
		{
			m_denoiseEnabled = !m_denoiseEnabled;
			m_historyValid = false;
		}
		m_denoiseKeyDown = denoiseKeyDown;

		bgfx::dbgTextClear();
		bgfx::dbgTextPrintf(0, 1, 0x0f, "Indirect light: %s (B to toggle)"
			, m_multipleBounces ? "multiple bounces" : "single bounce");
		bgfx::dbgTextPrintf(0, 2, 0x0f, "Shadows: %s (S to toggle)"
			, m_shadowsEnabled ? "on" : "off");
		bgfx::dbgTextPrintf(0, 3, 0x0f, "Denoiser: %s (D to toggle)"
			, m_denoiseEnabled ? "on" : "off");

		if (m_width == 0 || m_height == 0)
		{
			bgfx::frame();
			return true;
		}
		if (m_width != m_targetWidth || m_height != m_targetHeight)
		{
			createRenderTargets(m_width, m_height);
		}

		const bx::Vec3 at = m_scene.m_center;
		const float radius = m_scene.m_radius;
		const float horizontalDistance = m_cameraDistance * std::cos(m_cameraPitch);
		const bx::Vec3 eye =
		{
			at.x + horizontalDistance * std::sin(m_cameraYaw),
			at.y + m_cameraDistance * std::sin(m_cameraPitch),
			at.z + horizontalDistance * std::cos(m_cameraYaw),
		};
		float view[16];
		float projection[16];
		// glTF is right-handed; a left-handed camera would mirror the scene.
		bx::mtxLookAt(view, eye, at, { 0.0f, 1.0f, 0.0f }, bx::Handedness::Right);
		bx::mtxProj(
			  projection
			, 48.0f
			, float(m_width) / float(m_height)
			, 0.05f * radius
			, (kMaxCameraDistance + 2.0f) * radius
			, bgfx::getCaps()->homogeneousDepth
			, bx::Handedness::Right
			);

		float viewProjection[16];
		float inverseViewProjection[16];
		bx::mtxMul(viewProjection, view, projection);
		bx::mtxInverse(inverseViewProjection, viewProjection);

		const float lightPosition[] =
		{
			m_scene.m_lightPosition.x, m_scene.m_lightPosition.y, m_scene.m_lightPosition.z, 1.0f,
		};
		// Intensity for the reference scene; scaled with the square of the
		// scene size so the scene is lit the same at any scale.
		const float sceneScale = radius / kReferenceSceneRadius;
		const float lightIntensityScale = sceneScale * sceneScale;
		// const float lightColor[] = { 18.0f, 16.5f, 14.0f, 1.0f };
		const float lightColor[] =
		{
			1.0f * lightIntensityScale, 1.0f * lightIntensityScale, 1.0f * lightIntensityScale, 1.0f,
		};
		const float sceneInfo[] = { sceneScale, 0.0f, 0.0f, 0.0f };
		const float cameraPosition[] = { eye.x, eye.y, eye.z, 1.0f };

		float model[16];
		bx::mtxIdentity(model);

		const uint16_t giWidth = uint16_t(std::max(1u, m_width / 2));
		const uint16_t giHeight = uint16_t(std::max(1u, m_height / 2));

		// Shadow pass: the scene from the light, once per cube face. Each face
		// covers slightly more than 90 degrees so that the border texels exist.
		const float shadowTanHalfFov = float(kShadowFaceSize) / float(kShadowFaceSize - 2 * kShadowFaceBorder);
		const float shadowFar = 2.5f * radius;
		const float shadowInfo[] =
		{
			shadowFar, shadowTanHalfFov, 1.0f / float(kShadowFaceSize),
			bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f,
		};
		const float shadowOptions[] = { m_shadowsEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
		float shadowViewProj[6 * 16];
		{
			const bx::Vec3 light = m_scene.m_lightPosition;
			const bx::Vec3 faceDirections[6] =
			{
				{ 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
				{ 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
				{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
			};
			float shadowProjection[16];
			bx::mtxProj(
				  shadowProjection
				, bx::toDeg(2.0f * std::atan(shadowTanHalfFov))
				, 1.0f
				, 0.01f * radius
				, shadowFar
				, bgfx::getCaps()->homogeneousDepth
				, bx::Handedness::Right
				);

			for (uint8_t face = 0; face < 6; ++face)
			{
				const bx::Vec3 up = face == 2 || face == 3
					? bx::Vec3{ 0.0f, 0.0f, 1.0f }
					: bx::Vec3{ 0.0f, 1.0f, 0.0f };
				float faceView[16];
				bx::mtxLookAt(faceView, light, bx::add(light, faceDirections[face]), up, bx::Handedness::Right);
				bx::mtxMul(&shadowViewProj[face * 16], faceView, shadowProjection);

				const bgfx::ViewId viewId = bgfx::ViewId(kShadowViewBase + face);
				bgfx::setViewName(viewId, "Shadow Face");
				bgfx::setViewRect(viewId
					, uint16_t((face % 3) * kShadowFaceSize)
					, uint16_t((face / 3) * kShadowFaceSize)
					, kShadowFaceSize
					, kShadowFaceSize
					);
				bgfx::setViewFrameBuffer(viewId, m_shadowFrameBuffer);
				bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xffffffff, 1.0f, 0);
				bgfx::setViewTransform(viewId, faceView, shadowProjection);
				bgfx::touch(viewId);

				if (m_shadowsEnabled)
				{
					bgfx::setTransform(model);
					bgfx::setVertexBuffer(0, m_vbh);
					bgfx::setIndexBuffer(m_ibh);
					bgfx::setUniform(m_lightPosition, lightPosition);
					bgfx::setUniform(m_shadowInfoUniform, shadowInfo);
					bgfx::setState(BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
					bgfx::submit(viewId, m_shadowProgram);
				}
			}
		}

		bgfx::setMarker("Geometry Pass");
		bgfx::setViewRect(0, 0, 0, uint16_t(m_width), uint16_t(m_height));
		bgfx::setViewFrameBuffer(0, m_gbuffer);
		bgfx::setViewTransform(0, view, projection);
		bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
		bgfx::setTransform(model);
		bgfx::setVertexBuffer(0, m_vbh);
		bgfx::setIndexBuffer(m_ibh);
		bgfx::setState(
			  BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_WRITE_Z
			| BGFX_STATE_DEPTH_TEST_LESS
			);
		bgfx::submit(0, m_geometryProgram);

		const uint8_t historyWrite = uint8_t(1 - m_historyRead);

		bgfx::setMarker("ReSTIR GI Pass");
		bgfx::setViewRect(1, 0, 0, giWidth, giHeight);
		bgfx::setViewFrameBuffer(1, m_historyFrameBuffers[historyWrite]);
		bgfx::setViewTransform(1, nullptr, nullptr);
		bgfx::setTexture(0, m_albedoSampler, m_gbufferAlbedo);
		bgfx::setTexture(1, m_normalSampler, m_gbufferNormal);
		bgfx::setTexture(2, m_depthSampler, m_gbufferDepth);
		bgfx::setTexture(3, m_historySampleSampler, m_historySample[m_historyRead]);
		bgfx::setTexture(4, m_historyReceiverSampler, m_historyReceiver[m_historyRead]);
		// With the denoiser, the history is its first filtered iteration.
		bgfx::setTexture(5, m_historyColorSampler
			, m_denoiseEnabled ? m_filteredHistory[m_historyRead] : m_historyColor[m_historyRead]);
		bgfx::setTexture(8, m_historyMomentsSampler, m_historyMoments[m_historyRead]);
		bgfx::setTexture(6, m_historyNormalSampler, m_historyNormal[m_historyRead]);
		bgfx::setTexture(7, m_shadowSampler, m_shadowMap);
		bgfx::setUniform(m_shadowViewProjUniform, shadowViewProj, 6);
		bgfx::setUniform(m_shadowInfoUniform, shadowInfo);
		bgfx::setUniform(m_shadowOptionsUniform, shadowOptions);
		bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
		bgfx::setUniform(m_viewProjection, viewProjection);
		bgfx::setUniform(m_previousViewProjection, m_previousViewProj);
		bgfx::setUniform(m_lightPosition, lightPosition);
		bgfx::setUniform(m_lightColor, lightColor);
		const float frameInfo[] =
		{
			float(m_frameNumber & 0xffffff)
			, m_historyValid ? 1.0f : 0.0f
			, bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f
			, m_multipleBounces ? 1.0f : 0.0f
		};
		const float screenInfo[] =
		{
			float(giWidth), float(giHeight), 1.0f / float(giWidth), 1.0f / float(giHeight),
		};
		bgfx::setUniform(m_frameInfo, frameInfo);
		bgfx::setUniform(m_screenInfo, screenInfo);
		bgfx::setUniform(m_sceneInfoUniform, sceneInfo);
		setFullscreenGeometry();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		bgfx::submit(1, m_restirProgram);

		// Denoiser: variance estimate into filter target 0, then the a-trous
		// iterations. The first writes the filtered history for next frame,
		// the rest ping-pong between the two filter targets and end in target 0.
		if (m_denoiseEnabled)
		{
			bgfx::setMarker("Denoiser");
			for (uint8_t pass = 0; pass <= kAtrousIterations; ++pass)
			{
				const bgfx::ViewId viewId = bgfx::ViewId(kDenoiseViewBase + pass);
				bgfx::setViewName(viewId, pass == 0 ? "Denoise Variance" : "Denoise A-Trous");
				bgfx::setViewRect(viewId, 0, 0, giWidth, giHeight);
				bgfx::setViewTransform(viewId, nullptr, nullptr);

				bgfx::FrameBufferHandle output;
				if (pass == 0)
				{
					output = m_filterFrameBuffers[0];
					bgfx::setTexture(0, m_giColorSampler, m_historyColor[historyWrite]);
					bgfx::setTexture(1, m_giMomentsSampler, m_historyMoments[historyWrite]);
					bgfx::setTexture(2, m_giReceiverSampler, m_historyReceiver[historyWrite]);
					bgfx::setTexture(3, m_giNormalSampler, m_historyNormal[historyWrite]);
				}
				else
				{
					// Iteration 1 reads target 0 and writes the history; 2 reads the
					// history and writes target 1; then 1 -> 0, 0 -> 1, 1 -> 0.
					const uint8_t iteration = uint8_t(pass - 1);
					const bgfx::TextureHandle input = iteration == 0
						? m_filterTextures[0]
						: iteration == 1
						? m_filteredHistory[historyWrite]
						: m_filterTextures[(iteration - 1) & 1];
					output = iteration == 0
						? m_filteredHistoryFrameBuffers[historyWrite]
						: m_filterFrameBuffers[iteration & 1];
					bgfx::setTexture(0, m_filterInputSampler, input);
					bgfx::setTexture(1, m_giReceiverSampler, m_historyReceiver[historyWrite]);
					bgfx::setTexture(2, m_giNormalSampler, m_historyNormal[historyWrite]);
					const float atrousInfo[] = { float(1 << iteration), 0.0f, 0.0f, 0.0f };
					bgfx::setUniform(m_atrousInfoUniform, atrousInfo);
				}

				bgfx::setViewFrameBuffer(viewId, output);
				bgfx::setUniform(m_screenInfo, screenInfo);
				bgfx::setUniform(m_sceneInfoUniform, sceneInfo);
				setFullscreenGeometry();
				bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
				bgfx::submit(viewId, pass == 0 ? m_varianceProgram : m_atrousProgram);
			}
		}

		bgfx::setMarker("Composite Pass");
		bgfx::setViewRect(2, 0, 0, uint16_t(m_width), uint16_t(m_height));
		bgfx::setViewFrameBuffer(2, BGFX_INVALID_HANDLE);
		bgfx::setViewTransform(2, nullptr, nullptr);
		bgfx::setTexture(0, m_albedoSampler, m_gbufferAlbedo);
		bgfx::setTexture(1, m_normalSampler, m_gbufferNormal);
		bgfx::setTexture(2, m_depthSampler, m_gbufferDepth);
		bgfx::setTexture(3, m_giSampler
			, m_denoiseEnabled ? m_filterTextures[0] : m_historyColor[historyWrite]);
		bgfx::setTexture(4, m_shadowSampler, m_shadowMap);
		bgfx::setUniform(m_shadowViewProjUniform, shadowViewProj, 6);
		bgfx::setUniform(m_shadowInfoUniform, shadowInfo);
		bgfx::setUniform(m_shadowOptionsUniform, shadowOptions);
		bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
		bgfx::setUniform(m_lightPosition, lightPosition);
		bgfx::setUniform(m_lightColor, lightColor);
		bgfx::setUniform(m_cameraPosition, cameraPosition);
		bgfx::setUniform(m_screenInfo, screenInfo);
		bgfx::setUniform(m_sceneInfoUniform, sceneInfo);
		setFullscreenGeometry();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		bgfx::submit(2, m_compositeProgram);

		bx::memCopy(m_previousViewProj, viewProjection, sizeof(m_previousViewProj));
		m_historyRead = historyWrite;
		m_historyValid = true;
		++m_frameNumber;

		bgfx::setMarker("Present");
		bgfx::frame();
		return true;
	}

private:
	void setFullscreenGeometry()
	{
		bgfx::setTransform(nullptr);
		bgfx::setVertexBuffer(0, m_fullscreenVbh);
		bgfx::setIndexBuffer(m_fullscreenIbh);
	}

	void destroyRenderTargets()
	{
		if (bgfx::isValid(m_gbuffer))
		{
			bgfx::destroy(m_gbuffer);
			m_gbuffer = BGFX_INVALID_HANDLE;
			m_gbufferAlbedo = BGFX_INVALID_HANDLE;
			m_gbufferNormal = BGFX_INVALID_HANDLE;
			m_gbufferDepth = BGFX_INVALID_HANDLE;
		}
		for (uint8_t idx = 0; idx < 2; ++idx)
		{
			if (bgfx::isValid(m_historyFrameBuffers[idx]))
			{
				bgfx::destroy(m_historyFrameBuffers[idx]);
				m_historyFrameBuffers[idx] = BGFX_INVALID_HANDLE;
				m_historyColor[idx] = BGFX_INVALID_HANDLE;
				m_historySample[idx] = BGFX_INVALID_HANDLE;
				m_historyReceiver[idx] = BGFX_INVALID_HANDLE;
				m_historyNormal[idx] = BGFX_INVALID_HANDLE;
				m_historyMoments[idx] = BGFX_INVALID_HANDLE;
			}

			if (bgfx::isValid(m_filteredHistoryFrameBuffers[idx]))
			{
				bgfx::destroy(m_filteredHistoryFrameBuffers[idx]);
				m_filteredHistoryFrameBuffers[idx] = BGFX_INVALID_HANDLE;
				m_filteredHistory[idx] = BGFX_INVALID_HANDLE;
			}

			if (bgfx::isValid(m_filterFrameBuffers[idx]))
			{
				bgfx::destroy(m_filterFrameBuffers[idx]);
				m_filterFrameBuffers[idx] = BGFX_INVALID_HANDLE;
				m_filterTextures[idx] = BGFX_INVALID_HANDLE;
			}
		}
	}

	void createShadowMap()
	{
		const uint64_t samplerFlags = BGFX_SAMPLER_MIN_POINT
			| BGFX_SAMPLER_MAG_POINT
			| BGFX_SAMPLER_MIP_POINT
			| BGFX_SAMPLER_U_CLAMP
			| BGFX_SAMPLER_V_CLAMP;
		if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::R32F, BGFX_TEXTURE_RT | samplerFlags))
		{
			throw std::runtime_error("Shadows require R32F render targets.");
		}

		const uint16_t atlasWidth = 3 * kShadowFaceSize;
		const uint16_t atlasHeight = 2 * kShadowFaceSize;
		m_shadowMap = bgfx::createTexture2D(
			  atlasWidth, atlasHeight, false, 1, bgfx::TextureFormat::R32F
			, BGFX_TEXTURE_RT | samplerFlags
			);
		const bgfx::TextureFormat::Enum depthFormat =
			  bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT_WRITE_ONLY)
			? bgfx::TextureFormat::D32F
			: bgfx::TextureFormat::D24
			;
		const bgfx::TextureHandle shadowTextures[] =
		{
			m_shadowMap,
			bgfx::createTexture2D(atlasWidth, atlasHeight, false, 1, depthFormat, BGFX_TEXTURE_RT_WRITE_ONLY),
		};
		m_shadowFrameBuffer = bgfx::createFrameBuffer(BX_COUNTOF(shadowTextures), shadowTextures, true);
		if (!bgfx::isValid(m_shadowFrameBuffer))
		{
			throw std::runtime_error("Failed to create the shadow map.");
		}
	}

	void createRenderTargets(uint32_t _width, uint32_t _height)
	{
		destroyRenderTargets();

		const uint64_t samplerFlags = BGFX_SAMPLER_MIN_POINT
			| BGFX_SAMPLER_MAG_POINT
			| BGFX_SAMPLER_MIP_POINT
			| BGFX_SAMPLER_U_CLAMP
			| BGFX_SAMPLER_V_CLAMP;
		const uint16_t giWidth = uint16_t(std::max(1u, _width / 2));
		const uint16_t giHeight = uint16_t(std::max(1u, _height / 2));

		if (bgfx::getCaps()->limits.maxFBAttachments < 5
		||  !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT)
		||  !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA32F, BGFX_TEXTURE_RT))
		{
			throw std::runtime_error(
				"ReSTIR GI requires five framebuffer attachments and RGBA16F/RGBA32F render targets.");
		}

		m_gbufferAlbedo = bgfx::createTexture2D(
			  uint16_t(_width), uint16_t(_height), false, 1, bgfx::TextureFormat::BGRA8
			, BGFX_TEXTURE_RT | samplerFlags
			);
		m_gbufferNormal = bgfx::createTexture2D(
			  uint16_t(_width), uint16_t(_height), false, 1, bgfx::TextureFormat::BGRA8
			, BGFX_TEXTURE_RT | samplerFlags
			);
		const bgfx::TextureFormat::Enum depthFormat =
			  bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT | samplerFlags)
			? bgfx::TextureFormat::D32F
			: bgfx::TextureFormat::D24
			;
		m_gbufferDepth = bgfx::createTexture2D(
			  uint16_t(_width), uint16_t(_height), false, 1, depthFormat
			, BGFX_TEXTURE_RT | samplerFlags
			);
		const bgfx::TextureHandle gbufferTextures[] =
		{
			m_gbufferAlbedo, m_gbufferNormal, m_gbufferDepth,
		};
		m_gbuffer = bgfx::createFrameBuffer(BX_COUNTOF(gbufferTextures), gbufferTextures, true);

		for (uint8_t idx = 0; idx < 2; ++idx)
		{
			// History spans five targets: the accumulated (demodulated) estimate,
			// the sample position + M, the receiver position + W, the receiver
			// normal (needed by the pairwise MIS weights), and the luminance
			// moments + history length (needed by the denoiser). Positions need
			// full float precision, and all of them must be point sampled.
			m_historyColor[idx] = bgfx::createTexture2D(
				  giWidth, giHeight, false, 1, bgfx::TextureFormat::RGBA16F
				, BGFX_TEXTURE_RT | samplerFlags
				);
			m_historySample[idx] = bgfx::createTexture2D(
				  giWidth, giHeight, false, 1, bgfx::TextureFormat::RGBA32F
				, BGFX_TEXTURE_RT | samplerFlags
				);
			m_historyReceiver[idx] = bgfx::createTexture2D(
				  giWidth, giHeight, false, 1, bgfx::TextureFormat::RGBA32F
				, BGFX_TEXTURE_RT | samplerFlags
				);
			m_historyNormal[idx] = bgfx::createTexture2D(
				  giWidth, giHeight, false, 1, bgfx::TextureFormat::RGBA16F
				, BGFX_TEXTURE_RT | samplerFlags
				);
			m_historyMoments[idx] = bgfx::createTexture2D(
				  giWidth, giHeight, false, 1, bgfx::TextureFormat::RGBA16F
				, BGFX_TEXTURE_RT | samplerFlags
				);
			const bgfx::TextureHandle historyTextures[] =
			{
				m_historyColor[idx], m_historySample[idx], m_historyReceiver[idx], m_historyNormal[idx],
				m_historyMoments[idx],
			};
			m_historyFrameBuffers[idx] = bgfx::createFrameBuffer(
				BX_COUNTOF(historyTextures), historyTextures, true);

			// Denoiser: the filtered history (one per frame parity, like the
			// rest of the history) and two ping-pong targets.
			m_filteredHistory[idx] = bgfx::createTexture2D(
				  giWidth, giHeight, false, 1, bgfx::TextureFormat::RGBA16F
				, BGFX_TEXTURE_RT | samplerFlags
				);
			m_filteredHistoryFrameBuffers[idx] = bgfx::createFrameBuffer(1, &m_filteredHistory[idx], true);
			m_filterTextures[idx] = bgfx::createTexture2D(
				  giWidth, giHeight, false, 1, bgfx::TextureFormat::RGBA16F
				, BGFX_TEXTURE_RT | samplerFlags
				);
			m_filterFrameBuffers[idx] = bgfx::createFrameBuffer(1, &m_filterTextures[idx], true);
		}

		if (!bgfx::isValid(m_gbuffer) || !bgfx::isValid(m_historyFrameBuffers[0])
		||  !bgfx::isValid(m_historyFrameBuffers[1])
		||  !bgfx::isValid(m_filteredHistoryFrameBuffers[0]) || !bgfx::isValid(m_filteredHistoryFrameBuffers[1])
		||  !bgfx::isValid(m_filterFrameBuffers[0]) || !bgfx::isValid(m_filterFrameBuffers[1]))
		{
			throw std::runtime_error("Failed to create the G-buffer or ReSTIR GI history targets.");
		}

		m_targetWidth = _width;
		m_targetHeight = _height;
		m_historyRead = 0;
		m_historyValid = false;
		m_frameNumber = 0;
		bx::mtxIdentity(m_previousViewProj);
	}

	void updateCameraInput()
	{
		const bool isDragging = m_mouseState.m_buttons[entry::MouseButton::Left] != 0;
		if (isDragging && m_isDragging)
		{
			const int32_t deltaX = m_mouseState.m_mx - m_lastMouseX;
			const int32_t deltaY = m_mouseState.m_my - m_lastMouseY;
			const float sensitivity = 0.005f;

			m_cameraYaw -= float(deltaX) * sensitivity;
			m_cameraPitch = std::clamp(
				  m_cameraPitch + float(deltaY) * sensitivity
				, -0.15f
				, 1.35f
				);
		}

		m_isDragging = isDragging;
		m_lastMouseX = m_mouseState.m_mx;
		m_lastMouseY = m_mouseState.m_my;

		const int32_t wheelDelta = m_mouseState.m_mz - m_lastMouseWheel;
		m_lastMouseWheel = m_mouseState.m_mz;
		if (wheelDelta != 0)
		{
			m_cameraDistance = std::clamp(
				  m_cameraDistance * std::exp(-float(wheelDelta) * 0.12f)
				, kMinCameraDistance * m_scene.m_radius
				, kMaxCameraDistance * m_scene.m_radius
				);
		}
	}

	entry::MouseState m_mouseState;
	uint32_t m_width;
	uint32_t m_height;
	uint32_t m_debug;
	uint32_t m_reset;
	bgfx::VertexBufferHandle m_vbh;
	bgfx::IndexBufferHandle m_ibh;
	bgfx::VertexBufferHandle m_fullscreenVbh;
	bgfx::IndexBufferHandle m_fullscreenIbh;
	bgfx::FrameBufferHandle m_gbuffer;
	bgfx::TextureHandle m_gbufferAlbedo;
	bgfx::TextureHandle m_gbufferNormal;
	bgfx::TextureHandle m_gbufferDepth;
	bgfx::TextureHandle m_historyColor[2];
	bgfx::TextureHandle m_historySample[2];
	bgfx::TextureHandle m_historyReceiver[2];
	bgfx::TextureHandle m_historyNormal[2];
	bgfx::FrameBufferHandle m_historyFrameBuffers[2];
	uint8_t m_historyRead;
	bool m_historyValid;
	uint32_t m_targetWidth;
	uint32_t m_targetHeight;
	bgfx::ProgramHandle m_geometryProgram;
	bgfx::ProgramHandle m_restirProgram;
	bgfx::ProgramHandle m_compositeProgram;
	bgfx::UniformHandle m_lightPosition;
	bgfx::UniformHandle m_lightColor;
	bgfx::UniformHandle m_cameraPosition;
	bgfx::UniformHandle m_albedoSampler;
	bgfx::UniformHandle m_normalSampler;
	bgfx::UniformHandle m_depthSampler;
	bgfx::UniformHandle m_historySampleSampler;
	bgfx::UniformHandle m_historyReceiverSampler;
	bgfx::UniformHandle m_historyColorSampler;
	bgfx::UniformHandle m_historyNormalSampler;
	bgfx::UniformHandle m_giSampler;
	bgfx::UniformHandle m_inverseViewProjection;
	bgfx::UniformHandle m_viewProjection;
	bgfx::UniformHandle m_previousViewProjection;
	bgfx::UniformHandle m_frameInfo;
	bgfx::UniformHandle m_screenInfo;
	bgfx::UniformHandle m_sceneInfoUniform;
	SceneInfo m_scene;
	float m_cameraYaw;
	float m_cameraPitch;
	float m_cameraDistance;
	float m_previousViewProj[16];
	uint32_t m_frameNumber;
	int32_t m_lastMouseX;
	int32_t m_lastMouseY;
	int32_t m_lastMouseWheel;
	bool m_isDragging;
	bool m_multipleBounces;
	bool m_bounceKeyDown;
	bgfx::TextureHandle m_shadowMap;
	bgfx::FrameBufferHandle m_shadowFrameBuffer;
	bgfx::ProgramHandle m_shadowProgram;
	bgfx::UniformHandle m_shadowSampler;
	bgfx::UniformHandle m_shadowViewProjUniform;
	bgfx::UniformHandle m_shadowInfoUniform;
	bgfx::UniformHandle m_shadowOptionsUniform;
	bool m_shadowsEnabled;
	bool m_shadowKeyDown;
	bgfx::TextureHandle m_historyMoments[2];
	bgfx::TextureHandle m_filteredHistory[2];
	bgfx::FrameBufferHandle m_filteredHistoryFrameBuffers[2];
	bgfx::TextureHandle m_filterTextures[2];
	bgfx::FrameBufferHandle m_filterFrameBuffers[2];
	bgfx::ProgramHandle m_varianceProgram;
	bgfx::ProgramHandle m_atrousProgram;
	bgfx::UniformHandle m_historyMomentsSampler;
	bgfx::UniformHandle m_giColorSampler;
	bgfx::UniformHandle m_giMomentsSampler;
	bgfx::UniformHandle m_giReceiverSampler;
	bgfx::UniformHandle m_giNormalSampler;
	bgfx::UniformHandle m_filterInputSampler;
	bgfx::UniformHandle m_atrousInfoUniform;
	bool m_denoiseEnabled;
	bool m_denoiseKeyDown;
};

} // namespace

ENTRY_IMPLEMENT_MAIN(
	  ExampleRestirGi
	, "restir-gi"
	, "ReSTIR global illumination starter."
	, "https://github.com/bkaradzic/bgfx"
	);
