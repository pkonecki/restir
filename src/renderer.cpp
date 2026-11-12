#include "renderer.h"

#include "bvh.h"
#include "lights.h"

#include "bgfx_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace
{

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

// Destroys a bgfx handle if it is valid, and marks it invalid.
template<typename Handle>
void destroyIfValid(Handle& _handle)
{
	if (bgfx::isValid(_handle))
	{
		bgfx::destroy(_handle);
		_handle = BGFX_INVALID_HANDLE;
	}
}

// The radius of the scene the shader distances and light intensity were tuned
// for (the bundled Cornell box). Their values are scaled by the actual scene
// radius over this one.
constexpr float kReferenceSceneRadius = 1.75f;

constexpr uint16_t kShadowFaceBorder = 4; // texels kept for filter taps near face edges (see shaders/shadow.sh)

// World-space hash grid (see shaders/hashgrid.sh, which must agree on the
// table size and cell capacity).
constexpr uint32_t kGridTableSize = 65536;
constexpr uint32_t kGridCellCapacity = 32;
constexpr float kGridMinCellSize = 0.02f; // for the reference scene, scaled with the scene
constexpr float kGridCellPixels = 6.0f;   // cell size, in GI pixels, that the grid aims for

// Buffer stage of the BVH in the GI passes (BVH_STAGE in shaders/restir.sh).
constexpr uint8_t kBvhStage = 15;

// TAA: the jitter sequence's length, and the radical inverse it is made of.
constexpr uint32_t kTaaJitterCount = 8;

float halton(uint32_t _index, uint32_t _base)
{
	float result = 0.0f;
	float fraction = 1.0f;
	for (uint32_t value = _index; value > 0; value /= _base)
	{
		fraction /= float(_base);
		result += fraction * float(value % _base);
	}
	return result;
}

// Buffer stage of the light list in the DI passes (shaders/di.sh).
constexpr uint8_t kLightStage = 14;

// Cap on the M of every reused reservoir (kMaxHistory in shaders/restir.sh).
constexpr int kMaxReusedHistory = 20;

// Buffer stages of the grid, in the compute passes and in the GI passes.
constexpr uint8_t kGridComputeStage = 0;  // 0-2
constexpr uint8_t kGridFragmentStage = 11; // 11-13

} // namespace

void Renderer::init(const Settings& _settings)
{
	bgfx::setViewClear(kGeometryView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
	// Alpha 0 marks the background for the tone mapping pass.
	bgfx::setViewClear(kCompositeView, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);

	// The shadow faces must be rendered before the GI and composite passes.
	const bgfx::ViewId viewOrder[] =
	{
		kShadowViewBase + 0, kShadowViewBase + 1, kShadowViewBase + 2,
		kShadowViewBase + 3, kShadowViewBase + 4, kShadowViewBase + 5,
		kGeometryView, kDiTemporalView, kDiSpatialView,
		kGridClearView, kGridInsertView, kTemporalView, kSpatialView, kSpecularView,
		kDenoiseViewBase + 0, kDenoiseViewBase + 1, kDenoiseViewBase + 2,
		kDenoiseViewBase + 3, kDenoiseViewBase + 4, kDenoiseViewBase + 5,
		kCompositeView, kTaaView, kExposureView, kTonemapView, kUiView,
	};
	bgfx::setViewOrder(0, BX_COUNTOF(viewOrder), viewOrder);

	SceneVertex::init();
	FullscreenVertex::init();

	if (0 == (bgfx::getCaps()->supported & BGFX_CAPS_INDEX32))
	{
		throw std::runtime_error("The renderer does not support 32-bit index buffers.");
	}
	if (0 == (bgfx::getCaps()->supported & BGFX_CAPS_COMPUTE))
	{
		throw std::runtime_error("World-space ReSTIR GI requires compute shaders.");
	}

	// Materials: a white texture stands in for missing ones, so the
	// factors apply unchanged, and a flat one for missing normal maps.
	m_baseColorSampler = bgfx::createUniform("s_baseColor", bgfx::UniformType::Sampler);
	m_metallicRoughnessSampler = bgfx::createUniform("s_metallicRoughness", bgfx::UniformType::Sampler);
	m_normalMapSampler = bgfx::createUniform("s_normalMap", bgfx::UniformType::Sampler);
	m_occlusionSampler = bgfx::createUniform("s_occlusion", bgfx::UniformType::Sampler);
	m_emissiveMapSampler = bgfx::createUniform("s_emissiveMap", bgfx::UniformType::Sampler);
	m_materialInfoUniform = bgfx::createUniform("u_materialInfo", bgfx::UniformType::Vec4);
	m_materialEmissiveUniform = bgfx::createUniform("u_materialEmissive", bgfx::UniformType::Vec4);
	m_materialFactorsUniform = bgfx::createUniform("u_materialFactors", bgfx::UniformType::Vec4);
	const uint8_t white[] = { 255, 255, 255, 255 };
	m_whiteTexture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(white, sizeof(white)));
	const uint8_t black[] = { 0, 0, 0, 0 };
	m_blackTexture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(black, sizeof(black)));
	const uint8_t flatNormal[] = { 128, 128, 255, 255 };
	m_flatNormalTexture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(flatNormal, sizeof(flatNormal)));

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
	m_temporalProgram = loadProgram("vs_fullscreen", "fs_restir_temporal");
	m_spatialProgram = loadProgram("vs_fullscreen", "fs_restir_spatial");
	m_gridClearProgram = bgfx::createProgram(loadShader("cs_grid_clear"), true);
	m_gridInsertProgram = bgfx::createProgram(loadShader("cs_grid_insert"), true);
	m_compositeProgram = loadProgram("vs_fullscreen", "fs_composite");
	m_shadowProgram = loadProgram("vs_shadow", "fs_shadow");
	m_varianceProgram = loadProgram("vs_fullscreen", "fs_svgf_variance");
	m_atrousProgram = loadProgram("vs_fullscreen", "fs_svgf_atrous");
	m_exposureProgram = bgfx::createProgram(loadShader("cs_exposure"), true);
	m_tonemapProgram = loadProgram("vs_fullscreen", "fs_tonemap");
	m_taaProgram = loadProgram("vs_fullscreen", "fs_taa");
	m_diTemporalProgram = loadProgram("vs_fullscreen", "fs_di_temporal");
	m_diSpatialProgram = loadProgram("vs_fullscreen", "fs_di_spatial");
	m_specularProgram = loadProgram("vs_fullscreen", "fs_specular");
	m_debugProgram = loadProgram("vs_fullscreen", "fs_debug");
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
	m_reservoirSampleSampler = bgfx::createUniform("s_reservoirSample", bgfx::UniformType::Sampler);
	m_reservoirReceiverSampler = bgfx::createUniform("s_reservoirReceiver", bgfx::UniformType::Sampler);
	m_reservoirNormalSampler = bgfx::createUniform("s_reservoirNormal", bgfx::UniformType::Sampler);
	m_gridInfoUniform = bgfx::createUniform("u_gridInfo", bgfx::UniformType::Vec4);
	m_gridCameraUniform = bgfx::createUniform("u_gridCamera", bgfx::UniformType::Vec4);
	m_restirOptionsUniform = bgfx::createUniform("u_restirOptions", bgfx::UniformType::Vec4);
	m_hdrSampler = bgfx::createUniform("s_hdr", bgfx::UniformType::Sampler);
	m_exposureAdaptUniform = bgfx::createUniform("u_exposureAdapt", bgfx::UniformType::Vec4);
	m_tonemapUniform = bgfx::createUniform("u_tonemap", bgfx::UniformType::Vec4);
	m_taaHistorySampler = bgfx::createUniform("s_taaHistory", bgfx::UniformType::Sampler);
	m_taaPreviousViewProjUniform = bgfx::createUniform("u_taaPreviousViewProj", bgfx::UniformType::Mat4);
	m_taaInfoUniform = bgfx::createUniform("u_taaInfo", bgfx::UniformType::Vec4);
	m_diInfoUniform = bgfx::createUniform("u_diInfo", bgfx::UniformType::Vec4);
	m_diPreviousUniform = bgfx::createUniform("u_diPrevious", bgfx::UniformType::Vec4);
	m_screenSizeUniform = bgfx::createUniform("u_screenSize", bgfx::UniformType::Vec4);
	m_lightingInfoUniform = bgfx::createUniform("u_lightingInfo", bgfx::UniformType::Vec4);
	m_previousSampleSampler = bgfx::createUniform("s_previousSample", bgfx::UniformType::Sampler);
	m_previousReceiverSampler = bgfx::createUniform("s_previousReceiver", bgfx::UniformType::Sampler);
	m_previousNormalSampler = bgfx::createUniform("s_previousNormal", bgfx::UniformType::Sampler);
	m_surfaceLightSampler = bgfx::createUniform("s_surfaceLight", bgfx::UniformType::Sampler);
	m_diDiffuseSampler = bgfx::createUniform("s_diDiffuse", bgfx::UniformType::Sampler);
	m_diSpecularSampler = bgfx::createUniform("s_diSpecular", bgfx::UniformType::Sampler);
	m_specularSampler = bgfx::createUniform("s_specular", bgfx::UniformType::Sampler);
	m_specularHistorySampler = bgfx::createUniform("s_specularHistory", bgfx::UniformType::Sampler);
	m_previousHdrSampler = bgfx::createUniform("s_previousHdr", bgfx::UniformType::Sampler);
	m_compositeSpecularUniform = bgfx::createUniform("u_compositeSpecular", bgfx::UniformType::Vec4);
	m_debugViewUniform = bgfx::createUniform("u_debugView", bgfx::UniformType::Vec4);
	m_emissiveSampler = bgfx::createUniform("s_emissive", bgfx::UniformType::Sampler);
	m_shadingNormalSampler = bgfx::createUniform("s_shadingNormal", bgfx::UniformType::Sampler);
	m_bvhInfoUniform = bgfx::createUniform("u_bvhInfo", bgfx::UniformType::Vec4);
	m_skyZenithUniform = bgfx::createUniform("u_skyZenith", bgfx::UniformType::Vec4);
	m_skyHorizonUniform = bgfx::createUniform("u_skyHorizon", bgfx::UniformType::Vec4);
	m_skyGroundUniform = bgfx::createUniform("u_skyGround", bgfx::UniformType::Vec4);
	m_compositeOptionsUniform = bgfx::createUniform("u_compositeOptions", bgfx::UniformType::Vec4);

	// Two hash grids, this frame's and last frame's, like the reservoirs
	// they index. Their size does not depend on the resolution.
	for (uint8_t idx = 0; idx < 2; ++idx)
	{
		m_gridChecksums[idx] = bgfx::createDynamicIndexBuffer(
			kGridTableSize, BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_INDEX32);
		m_gridCounts[idx] = bgfx::createDynamicIndexBuffer(
			kGridTableSize, BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_INDEX32);
		m_gridEntries[idx] = bgfx::createDynamicIndexBuffer(
			kGridTableSize * kGridCellCapacity, BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_INDEX32);
		if (!bgfx::isValid(m_gridChecksums[idx]) || !bgfx::isValid(m_gridCounts[idx])
		||  !bgfx::isValid(m_gridEntries[idx]))
		{
			throw std::runtime_error("Failed to create the hash grid buffers.");
		}
	}

	// Auto exposure keeps its adapted luminance on the GPU, from frame to frame.
	m_exposureBuffer = bgfx::createDynamicIndexBuffer(4, BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_INDEX32);
	if (!bgfx::isValid(m_exposureBuffer))
	{
		throw std::runtime_error("Failed to create the exposure buffer.");
	}

	BX_ASSERT(bgfx::isValid(m_geometryProgram), "Failed to load G-buffer shaders.");
	BX_ASSERT(bgfx::isValid(m_temporalProgram), "Failed to load ReSTIR GI temporal shaders.");
	BX_ASSERT(bgfx::isValid(m_spatialProgram), "Failed to load ReSTIR GI spatial shaders.");
	BX_ASSERT(bgfx::isValid(m_gridClearProgram), "Failed to load the hash grid clear shader.");
	BX_ASSERT(bgfx::isValid(m_gridInsertProgram), "Failed to load the hash grid insert shader.");
	BX_ASSERT(bgfx::isValid(m_compositeProgram), "Failed to load composite shaders.");
	BX_ASSERT(bgfx::isValid(m_shadowProgram), "Failed to load shadow shaders.");
	BX_ASSERT(bgfx::isValid(m_varianceProgram), "Failed to load denoiser variance shaders.");
	BX_ASSERT(bgfx::isValid(m_atrousProgram), "Failed to load denoiser filter shaders.");
	BX_ASSERT(bgfx::isValid(m_exposureProgram), "Failed to load the auto exposure shader.");
	BX_ASSERT(bgfx::isValid(m_tonemapProgram), "Failed to load tone mapping shaders.");
	BX_ASSERT(bgfx::isValid(m_debugProgram), "Failed to load debug view shaders.");
	createShadowMap(_settings);
}

void Renderer::shutdown()
{
	destroyRenderTargets();
	destroyIfValid(m_shadowFrameBuffer);
	destroyScene();
	destroyIfValid(m_whiteTexture);
	destroyIfValid(m_flatNormalTexture);
	destroyIfValid(m_blackTexture);
	destroyIfValid(m_fullscreenVbh);
	destroyIfValid(m_fullscreenIbh);
	destroyIfValid(m_exposureBuffer);
	for (uint8_t idx = 0; idx < 2; ++idx)
	{
		destroyIfValid(m_gridChecksums[idx]);
		destroyIfValid(m_gridCounts[idx]);
		destroyIfValid(m_gridEntries[idx]);
	}

	bgfx::ProgramHandle programs[] =
	{
		m_geometryProgram, m_temporalProgram, m_spatialProgram, m_compositeProgram, m_shadowProgram,
		m_varianceProgram, m_atrousProgram, m_gridClearProgram, m_gridInsertProgram,
		m_exposureProgram, m_tonemapProgram, m_debugProgram, m_taaProgram,
		m_diTemporalProgram, m_diSpatialProgram, m_specularProgram,
	};
	for (bgfx::ProgramHandle& program : programs)
	{
		destroyIfValid(program);
	}

	bgfx::UniformHandle uniforms[] =
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
		m_reservoirSampleSampler, m_reservoirReceiverSampler, m_reservoirNormalSampler,
		m_gridInfoUniform, m_gridCameraUniform, m_restirOptionsUniform,
		m_baseColorSampler, m_metallicRoughnessSampler, m_materialInfoUniform,
		m_normalMapSampler, m_occlusionSampler, m_emissiveMapSampler,
		m_materialEmissiveUniform, m_materialFactorsUniform,
		m_emissiveSampler, m_shadingNormalSampler, m_bvhInfoUniform,
		m_skyZenithUniform, m_skyHorizonUniform, m_skyGroundUniform,
		m_taaHistorySampler, m_taaPreviousViewProjUniform, m_taaInfoUniform,
		m_diInfoUniform, m_diPreviousUniform, m_screenSizeUniform, m_lightingInfoUniform,
		m_previousSampleSampler, m_previousReceiverSampler, m_previousNormalSampler,
		m_surfaceLightSampler, m_diDiffuseSampler, m_diSpecularSampler,
		m_specularSampler, m_specularHistorySampler, m_previousHdrSampler, m_compositeSpecularUniform,
		m_hdrSampler, m_exposureAdaptUniform, m_tonemapUniform,
		m_debugViewUniform, m_compositeOptionsUniform,
	};
	for (bgfx::UniformHandle& uniform : uniforms)
	{
		destroyIfValid(uniform);
	}
}

void Renderer::setScene(const SceneMesh& _mesh, const SceneInfo& _info)
{
	destroyScene();

	// Repeat wrapping and trilinear, anisotropic filtering (glTF samplers
	// are not read; repeat is their default).
	const uint64_t textureFlags = BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC;
	for (const SceneImage& image : _mesh.m_images)
	{
		m_modelTextures.push_back(bgfx::createTexture2D(
			  image.m_width, image.m_height, true, 1, bgfx::TextureFormat::RGBA8, textureFlags
			, bgfx::copy(image.m_data.data(), uint32_t(image.m_data.size()))
			));
	}
	m_materials = _mesh.m_materials;
	m_draws = _mesh.m_draws;

	m_vbh = bgfx::createVertexBuffer(
		  bgfx::copy(_mesh.m_vertices.data(), uint32_t(_mesh.m_vertices.size() * sizeof(SceneVertex)))
		, SceneVertex::ms_layout
		);
	m_ibh = bgfx::createIndexBuffer(
		  bgfx::copy(_mesh.m_indices.data(), uint32_t(_mesh.m_indices.size() * sizeof(uint32_t)))
		, BGFX_BUFFER_INDEX32
		);

	// The BVH for rays the depth buffer cannot answer, in one buffer of vec4s.
	const auto buildStart = std::chrono::steady_clock::now();
	const SceneBvh bvh = buildBvh(_mesh);
	const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
	std::fprintf(stderr, "BVH: %u triangles, %u nodes, depth %u, %.1f MB, built in %.0f ms.\n"
		, bvh.m_triangleCount, bvh.m_nodeCount, bvh.m_depth
		, double(bvh.m_data.size() * sizeof(float)) / (1024.0 * 1024.0), buildMs);
	bgfx::VertexLayout vec4Layout;
	vec4Layout.begin().add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float).end();
	m_bvhBuffer = bgfx::createVertexBuffer(
		  bgfx::copy(bvh.m_data.data(), uint32_t(bvh.m_data.size() * sizeof(float)))
		, vec4Layout
		, BGFX_BUFFER_COMPUTE_READ
		);
	m_bvhInfo[0] = float(bvh.m_triangleOffset);
	m_bvhInfo[1] = 0.0f;
	m_bvhInfo[2] = bgfx::isValid(m_bvhBuffer) ? float(bvh.m_nodeCount) : 0.0f;
	m_bvhStats = { bvh.m_triangleCount, bvh.m_nodeCount, bvh.m_depth, float(buildMs) };
	m_emitters = bvh.m_emitters;
	m_lightListKey = {}; // rebuilt on the next frame

	m_scene = _info;
	m_historyValid = false;
	m_taaValid = false;
	m_exposureValid = false;
}

void Renderer::destroyScene()
{
	destroyIfValid(m_bvhBuffer);
	destroyIfValid(m_lightBuffer);
	m_lightCount = 0;
	destroyIfValid(m_vbh);
	destroyIfValid(m_ibh);
	for (bgfx::TextureHandle& texture : m_modelTextures)
	{
		destroyIfValid(texture);
	}
	m_modelTextures.clear();
}

void Renderer::render(const FrameInput& _input, const Settings& _settings)
{
	const uint32_t width = _input.m_width;
	const uint32_t height = _input.m_height;
	if (width != m_targetWidth || height != m_targetHeight
	||  kGiResolutions[_settings.giResolution].m_divisor != m_targetGiDivisor)
	{
		createRenderTargets(width, height, _settings);
	}
	if (kShadowFaceSizes[_settings.shadowFaceSize] != m_shadowFaceSize)
	{
		createShadowMap(_settings);
	}

	const DebugView debugView = DebugView(_settings.debugView);
	const bool dataView = debugView >= DebugView::Albedo;

	const float radius = m_scene.m_radius;
	const bx::Vec3 eye = _input.m_eye;
	const float* view = _input.m_camera.m_view;
	// With TAA, every frame is rendered with a different sub-pixel offset
	// (Halton 2, 3), which the TAA pass integrates. The scene passes all use
	// the jittered matrices; TAA reprojects with the unjittered ones.
	const bool taa = _settings.taa && !dataView;
	float projection[16];
	bx::memCopy(projection, _input.m_camera.m_projection, sizeof(projection));
	if (taa)
	{
		const uint32_t index = (m_frameNumber % kTaaJitterCount) + 1;
		const float jitterX = (halton(index, 2) - 0.5f) * 2.0f / float(width);
		const float jitterY = (halton(index, 3) - 0.5f) * 2.0f / float(height);
		// clip.xy += jitter * clip.w, and clip.w comes from column 3.
		projection[8] += jitterX * projection[11];
		projection[9] += jitterY * projection[11];
	}

	float viewProjection[16];
	float inverseViewProjection[16];
	float unjitteredViewProjection[16];
	bx::mtxMul(viewProjection, view, projection);
	bx::mtxInverse(inverseViewProjection, viewProjection);
	bx::mtxMul(unjitteredViewProjection, view, _input.m_camera.m_projection);

	const LightSettings& light = _settings.light;
	const float lightPosition[] = { light.position[0], light.position[1], light.position[2], 1.0f };
	// An intensity of 1 is tuned for the reference scene; it is scaled with
	// the square of the scene size so the scene is lit the same at any scale.
	const float sceneScale = radius / kReferenceSceneRadius;
	const float lightIntensityScale = light.intensity * sceneScale * sceneScale;
	const float lightColor[] =
	{
		light.color[0] * lightIntensityScale,
		light.color[1] * lightIntensityScale,
		light.color[2] * lightIntensityScale,
		1.0f,
	};
	const float sceneInfo[] =
	{
		sceneScale,
		_settings.emissiveLighting ? 1.0f : 0.0f,
		_settings.clampJacobian ? kJacobianClamp : 0.0f,
		_settings.clampOutliers ? kOutlierClamp : 0.0f,
	};
	const float cameraPosition[] = { eye.x, eye.y, eye.z, 1.0f };

	const uint16_t giWidth = uint16_t(std::max(1u, width / m_targetGiDivisor));
	const uint16_t giHeight = uint16_t(std::max(1u, height / m_targetGiDivisor));

	// Shadow pass: the scene from the light, once per cube face. Each face
	// covers slightly more than 90 degrees so that the border texels exist.
	const float shadowTanHalfFov = float(m_shadowFaceSize) / float(m_shadowFaceSize - 2 * kShadowFaceBorder);
	const float shadowFar = 2.5f * radius;
	const float shadowInfo[] =
	{
		shadowFar, shadowTanHalfFov, 1.0f / float(m_shadowFaceSize),
		bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f,
	};
	const float shadowOptions[] = { _settings.shadows ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
	float shadowViewProj[6 * 16];
	{
		const bx::Vec3 lightCenter = { light.position[0], light.position[1], light.position[2] };
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
			bx::mtxLookAt(faceView, lightCenter, bx::add(lightCenter, faceDirections[face]), up, bx::Handedness::Right);
			bx::mtxMul(&shadowViewProj[face * 16], faceView, shadowProjection);

			const bgfx::ViewId viewId = bgfx::ViewId(kShadowViewBase + face);
			bgfx::setViewName(viewId, "Shadow Face");
			bgfx::setViewRect(viewId
				, uint16_t((face % 3) * m_shadowFaceSize)
				, uint16_t((face / 3) * m_shadowFaceSize)
				, m_shadowFaceSize
				, m_shadowFaceSize
				);
			bgfx::setViewFrameBuffer(viewId, m_shadowFrameBuffer);
			bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xffffffff, 1.0f, 0);
			bgfx::setViewTransform(viewId, faceView, shadowProjection);
			bgfx::touch(viewId);

			if (_settings.shadows)
			{
				for (const SceneDraw& draw : m_draws)
				{
							bgfx::setVertexBuffer(0, m_vbh);
					bgfx::setIndexBuffer(m_ibh, draw.m_firstIndex, draw.m_indexCount);
					setMaterial(draw.m_material, false);
					bgfx::setUniform(m_lightPosition, lightPosition);
					bgfx::setUniform(m_shadowInfoUniform, shadowInfo);
					bgfx::setState(BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
					bgfx::submit(viewId, m_shadowProgram);
				}
			}
		}
	}

	bgfx::setMarker("Geometry Pass");
	bgfx::setViewName(kGeometryView, "G-Buffer");
	bgfx::setViewRect(kGeometryView, 0, 0, uint16_t(width), uint16_t(height));
	bgfx::setViewFrameBuffer(kGeometryView, m_gbuffer);
	bgfx::setViewTransform(kGeometryView, view, projection);
	for (const SceneDraw& draw : m_draws)
	{
		bgfx::setVertexBuffer(0, m_vbh);
		bgfx::setIndexBuffer(m_ibh, draw.m_firstIndex, draw.m_indexCount);
		setMaterial(draw.m_material, true);
		bgfx::setState(
			  BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_WRITE_Z
			| BGFX_STATE_DEPTH_TEST_LESS
			);
		bgfx::submit(kGeometryView, m_geometryProgram);
	}

	const float frameInfo[] =
	{
		float(m_frameNumber & 0xffffff)
		, m_historyValid ? 1.0f : 0.0f
		, bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f
		, _settings.multipleBounces ? 1.0f : 0.0f
	};
	const float bvhInfo[] = { m_bvhInfo[0], m_bvhInfo[1], m_bvhInfo[2], float(_settings.rayMode) };

	// ReSTIR DI: the light list (emitters and extra point lights), at full
	// resolution, before the GI passes, which take its diffuse light as the
	// light their sample points reflect.
	const bool restirDi = _settings.directLighting == int(DirectLighting::ReStirDi);
	updateLightList(_settings, sceneScale * sceneScale);
	const bool diActive = restirDi && m_lightCount > 0 && bvhInfo[2] > 0.0f;
	const uint8_t diWrite = uint8_t(1 - m_diRead);
	if (diActive)
	{
		const float diInfo[] =
		{
			float(m_lightCount),
			float(_settings.diCandidates),
			float(_settings.diSpatialTaps),
			m_diValid ? 1.0f : 0.0f,
		};
		const float diPrevious[] = { m_previousEye.x, m_previousEye.y, m_previousEye.z, 0.0f };
		const float screenSize[] = { float(width), float(height), 1.0f / float(width), 1.0f / float(height) };
		const auto setDiInputs = [&]()
		{
			bgfx::setTexture(0, m_albedoSampler, m_gbufferAlbedo);
			bgfx::setTexture(1, m_normalSampler, m_gbufferNormal);
			bgfx::setTexture(2, m_depthSampler, m_gbufferDepth);
			bgfx::setTexture(3, m_shadingNormalSampler, m_gbufferShadingNormal);
			bgfx::setBuffer(kLightStage, m_lightBuffer, bgfx::Access::Read);
			bgfx::setBuffer(kBvhStage, m_bvhBuffer, bgfx::Access::Read);
			bgfx::setUniform(m_bvhInfoUniform, bvhInfo);
			bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
			bgfx::setUniform(m_cameraPosition, cameraPosition);
			bgfx::setUniform(m_sceneInfoUniform, sceneInfo);
			bgfx::setUniform(m_frameInfo, frameInfo);
			bgfx::setUniform(m_diInfoUniform, diInfo);
			setFullscreenGeometry();
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		};

		bgfx::setMarker("ReSTIR DI Temporal");
		bgfx::setViewName(kDiTemporalView, "ReSTIR DI Temporal");
		bgfx::setViewRect(kDiTemporalView, 0, 0, uint16_t(width), uint16_t(height));
		bgfx::setViewFrameBuffer(kDiTemporalView, m_diReservoirFrameBuffers[diWrite]);
		bgfx::setViewTransform(kDiTemporalView, nullptr, nullptr);
		bgfx::setTexture(4, m_previousSampleSampler, m_diSample[m_diRead]);
		bgfx::setTexture(5, m_previousReceiverSampler, m_diReceiver[m_diRead]);
		bgfx::setTexture(6, m_previousNormalSampler, m_diNormal[m_diRead]);
		bgfx::setUniform(m_previousViewProjection, m_previousViewProj);
		bgfx::setUniform(m_diPreviousUniform, diPrevious);
		setDiInputs();
		bgfx::submit(kDiTemporalView, m_diTemporalProgram);

		bgfx::setMarker("ReSTIR DI Spatial");
		bgfx::setViewName(kDiSpatialView, "ReSTIR DI Spatial");
		bgfx::setViewRect(kDiSpatialView, 0, 0, uint16_t(width), uint16_t(height));
		bgfx::setViewFrameBuffer(kDiSpatialView, m_diShadeFrameBuffer);
		bgfx::setViewTransform(kDiSpatialView, nullptr, nullptr);
		bgfx::setTexture(4, m_reservoirSampleSampler, m_diSample[diWrite]);
		bgfx::setTexture(5, m_reservoirReceiverSampler, m_diReceiver[diWrite]);
		bgfx::setTexture(6, m_reservoirNormalSampler, m_diNormal[diWrite]);
		bgfx::setUniform(m_screenSizeUniform, screenSize);
		setDiInputs();
		bgfx::submit(kDiSpatialView, m_diSpatialProgram);

		m_diRead = diWrite;
		m_diValid = true;
	}
	else
	{
		m_diValid = false;
	}
	const bgfx::TextureHandle diDiffuse = diActive ? m_diDiffuse : m_blackTexture;
	const bgfx::TextureHandle diSpecular = diActive ? m_diSpecular : m_blackTexture;

	// What the GI sample points send out besides the point light's light.
	const bool emitters = _settings.emissiveLighting;
	const bgfx::TextureHandle surfaceLight = restirDi ? diDiffuse : m_gbufferEmissive;
	const float lightingInfo[] =
	{
		restirDi ? 1.0f : (emitters ? 1.0f : 0.0f),
		restirDi ? 0.0f : (emitters ? 1.0f : 0.0f),
		0.0f,
		0.0f,
	};

	// This frame's reservoirs, shaded GI and hash grid are written at
	// historyWrite; last frame's are read from m_historyRead.
	const uint8_t historyWrite = uint8_t(1 - m_historyRead);

	const float screenInfo[] =
	{
		float(giWidth), float(giHeight), 1.0f / float(giWidth), 1.0f / float(giHeight),
	};
	// The world size of a GI pixel grows linearly with the distance from
	// the camera; the grid sizes its cells from it.
	const float gridInfo[] =
	{
		kGridMinCellSize * sceneScale
		, 2.0f * std::tan(0.5f * bx::toRad(kCameraFovY)) / float(giHeight)
		, kGridCellPixels
		, _settings.worldSpaceReuse ? 1.0f : 0.0f
	};
	const float restirOptions[] =
	{
		  float(_settings.initialCandidates)
		, float(_settings.temporalTaps)
		, float(_settings.spatialTaps)
		, float(_settings.traceSteps)
	};
	const float previousCameraPosition[] =
	{
		m_previousEye.x, m_previousEye.y, m_previousEye.z, 1.0f,
	};

	// World-space hash grid: clear this frame's grid, then list every GI
	// pixel in the cell of its receiver.
	bgfx::setMarker("Hash Grid");
	bgfx::setViewName(kGridClearView, "Hash Grid Clear");
	bgfx::setBuffer(kGridComputeStage + 0, m_gridChecksums[historyWrite], bgfx::Access::Write);
	bgfx::setBuffer(kGridComputeStage + 1, m_gridCounts[historyWrite], bgfx::Access::Write);
	bgfx::setBuffer(kGridComputeStage + 2, m_gridEntries[historyWrite], bgfx::Access::Write);
	bgfx::dispatch(kGridClearView, m_gridClearProgram, kGridTableSize / 64, 1, 1);

	bgfx::setViewName(kGridInsertView, "Hash Grid Insert");
	bgfx::setBuffer(kGridComputeStage + 0, m_gridChecksums[historyWrite], bgfx::Access::ReadWrite);
	bgfx::setBuffer(kGridComputeStage + 1, m_gridCounts[historyWrite], bgfx::Access::ReadWrite);
	bgfx::setBuffer(kGridComputeStage + 2, m_gridEntries[historyWrite], bgfx::Access::Write);
	bgfx::setTexture(3, m_normalSampler, m_gbufferNormal);
	bgfx::setTexture(4, m_depthSampler, m_gbufferDepth);
	bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
	bgfx::setUniform(m_frameInfo, frameInfo);
	bgfx::setUniform(m_screenInfo, screenInfo);
	bgfx::setUniform(m_gridInfoUniform, gridInfo);
	bgfx::setUniform(m_gridCameraUniform, cameraPosition);
	bgfx::dispatch(kGridInsertView, m_gridInsertProgram
		, uint32_t(giWidth + 7) / 8, uint32_t(giHeight + 7) / 8, 1);

	// Uniforms and textures both GI passes use (see shaders/restir.sh).
	// The sky's colours, premultiplied by its intensity.
	const SkySettings& sky = _settings.sky;
	const auto skyColor = [&sky](const float* _color, float _w)
	{
		return std::array<float, 4>{ _color[0] * sky.intensity, _color[1] * sky.intensity, _color[2] * sky.intensity, _w };
	};
	const std::array<float, 4> skyZenith = skyColor(sky.zenith, sky.enabled ? 1.0f : 0.0f);
	const std::array<float, 4> skyHorizon = skyColor(sky.horizon, 0.0f);
	const std::array<float, 4> skyGround = skyColor(sky.ground, 0.0f);
	const auto setSkyUniforms = [&]()
	{
		bgfx::setUniform(m_skyZenithUniform, skyZenith.data());
		bgfx::setUniform(m_skyHorizonUniform, skyHorizon.data());
		bgfx::setUniform(m_skyGroundUniform, skyGround.data());
	};

	const auto setGiPassInputs = [&]()
	{
		bgfx::setTexture(0, m_albedoSampler, m_gbufferAlbedo);
		bgfx::setTexture(1, m_normalSampler, m_gbufferNormal);
		bgfx::setTexture(2, m_depthSampler, m_gbufferDepth);
		// With the denoiser, the history is its first filtered iteration.
		bgfx::setTexture(5, m_historyColorSampler
			, _settings.denoiser ? m_filteredHistory[m_historyRead] : m_historyColor[m_historyRead]);
		bgfx::setTexture(6, m_historyNormalSampler, m_historyNormal[m_historyRead]);
		bgfx::setTexture(7, m_shadowSampler, m_shadowMap);
		bgfx::setTexture(9, m_historyReceiverSampler, m_historyReceiver[m_historyRead]);
		bgfx::setTexture(14, m_surfaceLightSampler, surfaceLight);
		bgfx::setUniform(m_lightingInfoUniform, lightingInfo);
		bgfx::setUniform(m_shadowViewProjUniform, shadowViewProj, 6);
		bgfx::setUniform(m_shadowInfoUniform, shadowInfo);
		bgfx::setUniform(m_shadowOptionsUniform, shadowOptions);
		bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
		bgfx::setUniform(m_viewProjection, viewProjection);
		bgfx::setUniform(m_previousViewProjection, m_previousViewProj);
		bgfx::setUniform(m_lightPosition, lightPosition);
		bgfx::setUniform(m_lightColor, lightColor);
		bgfx::setUniform(m_frameInfo, frameInfo);
		bgfx::setUniform(m_screenInfo, screenInfo);
		bgfx::setUniform(m_sceneInfoUniform, sceneInfo);
		bgfx::setUniform(m_gridInfoUniform, gridInfo);
		bgfx::setUniform(m_restirOptionsUniform, restirOptions);
		bgfx::setBuffer(kBvhStage, m_bvhBuffer, bgfx::Access::Read);
		bgfx::setUniform(m_bvhInfoUniform, bvhInfo);
		setSkyUniforms();
	};
	const auto setGrid = [](const bgfx::DynamicIndexBufferHandle* _buffers)
	{
		bgfx::setBuffer(kGridFragmentStage + 0, _buffers[0], bgfx::Access::Read);
		bgfx::setBuffer(kGridFragmentStage + 1, _buffers[1], bgfx::Access::Read);
		bgfx::setBuffer(kGridFragmentStage + 2, _buffers[2], bgfx::Access::Read);
	};

	// Temporal pass: fresh candidates, combined with reservoirs from last
	// frame's grid, into this frame's reservoirs. Last frame's cell sizes
	// were chosen from last frame's camera position.
	bgfx::setMarker("ReSTIR GI Temporal");
	bgfx::setViewName(kTemporalView, "ReSTIR GI Temporal");
	bgfx::setViewRect(kTemporalView, 0, 0, giWidth, giHeight);
	bgfx::setViewFrameBuffer(kTemporalView, m_reservoirFrameBuffers[historyWrite]);
	bgfx::setViewTransform(kTemporalView, nullptr, nullptr);
	setGiPassInputs();
	bgfx::setTexture(3, m_historySampleSampler, m_historySample[m_historyRead]);
	const bgfx::DynamicIndexBufferHandle previousGrid[] =
	{
		m_gridChecksums[m_historyRead], m_gridCounts[m_historyRead], m_gridEntries[m_historyRead],
	};
	setGrid(previousGrid);
	bgfx::setUniform(m_gridCameraUniform, previousCameraPosition);
	setFullscreenGeometry();
	bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
	bgfx::submit(kTemporalView, m_temporalProgram);

	// Spatial pass: combined with other reservoirs from this frame's grid,
	// shaded and accumulated for the denoiser.
	bgfx::setMarker("ReSTIR GI Spatial");
	bgfx::setViewName(kSpatialView, "ReSTIR GI Spatial");
	bgfx::setViewRect(kSpatialView, 0, 0, giWidth, giHeight);
	bgfx::setViewFrameBuffer(kSpatialView, m_shadeFrameBuffers[historyWrite]);
	bgfx::setViewTransform(kSpatialView, nullptr, nullptr);
	setGiPassInputs();
	bgfx::setTexture(3, m_reservoirSampleSampler, m_historySample[historyWrite]);
	bgfx::setTexture(4, m_reservoirReceiverSampler, m_historyReceiver[historyWrite]);
	bgfx::setTexture(8, m_historyMomentsSampler, m_historyMoments[m_historyRead]);
	bgfx::setTexture(10, m_reservoirNormalSampler, m_historyNormal[historyWrite]);
	const bgfx::DynamicIndexBufferHandle currentGrid[] =
	{
		m_gridChecksums[historyWrite], m_gridCounts[historyWrite], m_gridEntries[historyWrite],
	};
	setGrid(currentGrid);
	bgfx::setUniform(m_gridCameraUniform, cameraPosition);
	setFullscreenGeometry();
	bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
	bgfx::submit(kSpatialView, m_spatialProgram);

	// Indirect specular: one GGX ray per GI pixel, accumulated over time.
	const bool specularActive = _settings.specularGi;
	if (specularActive)
	{
		const uint8_t specularWrite = uint8_t(1 - m_specularRead);
		bgfx::setMarker("Specular GI");
		bgfx::setViewName(kSpecularView, "Specular GI");
		bgfx::setViewRect(kSpecularView, 0, 0, giWidth, giHeight);
		bgfx::setViewFrameBuffer(kSpecularView, m_specularFrameBuffers[specularWrite]);
		bgfx::setViewTransform(kSpecularView, nullptr, nullptr);
		setGiPassInputs();
		bgfx::setTexture(3, m_previousHdrSampler, m_hdrTexture);
		bgfx::setTexture(4, m_specularHistorySampler, m_specularTextures[m_specularRead]);
		setGrid(currentGrid);
		bgfx::setUniform(m_gridCameraUniform, cameraPosition);
		bgfx::setUniform(m_cameraPosition, cameraPosition);
		setFullscreenGeometry();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		bgfx::submit(kSpecularView, m_specularProgram);
		m_specularRead = specularWrite;
	}

	// Denoiser: variance estimate into filter target 0, then the a-trous
	// iterations. The first writes the filtered history for next frame,
	// the rest ping-pong between the two filter targets.
	bgfx::TextureHandle denoised = m_historyColor[historyWrite];
	if (_settings.denoiser)
	{
		bgfx::setMarker("Denoiser");
		for (uint8_t pass = 0; pass <= uint8_t(_settings.atrousIterations); ++pass)
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
				denoised = bgfx::getTexture(output);
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
	bgfx::setViewName(kCompositeView, "Composite");
	bgfx::setViewRect(kCompositeView, 0, 0, uint16_t(width), uint16_t(height));
	bgfx::setViewFrameBuffer(kCompositeView, m_hdrFrameBuffer);
	bgfx::setViewTransform(kCompositeView, nullptr, nullptr);
	if (dataView)
	{
		// Data views replace the composite with display colours.
		const float debugInfo[] =
		{
			float(_settings.debugView),
			float(_settings.initialCandidates + _settings.temporalTaps * kMaxReusedHistory),
			bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f,
			sceneScale,
		};
		bgfx::setTexture(0, m_albedoSampler, m_gbufferAlbedo);
		bgfx::setTexture(1, m_normalSampler, m_gbufferNormal);
		bgfx::setTexture(2, m_depthSampler, m_gbufferDepth);
		bgfx::setTexture(3, m_reservoirSampleSampler, m_historySample[historyWrite]);
		bgfx::setTexture(4, m_reservoirReceiverSampler, m_historyReceiver[historyWrite]);
		bgfx::setTexture(5, m_giMomentsSampler, m_historyMoments[historyWrite]);
		bgfx::setTexture(6, m_emissiveSampler, m_gbufferEmissive);
		bgfx::setTexture(7, m_shadingNormalSampler, m_gbufferShadingNormal);
		bgfx::setTexture(8, m_reservoirNormalSampler, m_historyNormal[historyWrite]);
		setGrid(currentGrid);
		bgfx::setUniform(m_viewProjection, viewProjection);
		bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
		bgfx::setUniform(m_gridInfoUniform, gridInfo);
		bgfx::setUniform(m_gridCameraUniform, cameraPosition);
		bgfx::setUniform(m_debugViewUniform, debugInfo);
		setFullscreenGeometry();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		bgfx::submit(kCompositeView, m_debugProgram);
	}
	else
	{
		// The lighting views keep only part of the light.
		const float compositeOptions[] =
		{
			debugView == DebugView::Final || debugView == DebugView::DirectLight ? 1.0f : 0.0f,
			debugView == DebugView::DirectLight || debugView == DebugView::IndirectSpecular ? 0.0f : 1.0f,
			debugView == DebugView::Irradiance ? 0.0f : 1.0f,
			debugView == DebugView::Final || debugView == DebugView::DirectLight ? 1.0f : 0.0f,
		};
		bgfx::setUniform(m_compositeOptionsUniform, compositeOptions);
		const float compositeSpecular[] =
		{
			specularActive && (debugView == DebugView::Final || debugView == DebugView::IndirectLight
				|| debugView == DebugView::IndirectSpecular) ? 1.0f : 0.0f,
			0.0f, 0.0f, 0.0f,
		};
		bgfx::setUniform(m_compositeSpecularUniform, compositeSpecular);
		bgfx::setTexture(0, m_albedoSampler, m_gbufferAlbedo);
		bgfx::setTexture(1, m_normalSampler, m_gbufferNormal);
		bgfx::setTexture(2, m_depthSampler, m_gbufferDepth);
		bgfx::setTexture(3, m_giSampler, denoised);
		bgfx::setTexture(4, m_shadowSampler, m_shadowMap);
		bgfx::setTexture(5, m_emissiveSampler, m_gbufferEmissive);
		bgfx::setTexture(6, m_shadingNormalSampler, m_gbufferShadingNormal);
		bgfx::setTexture(7, m_diDiffuseSampler, diDiffuse);
		bgfx::setTexture(8, m_diSpecularSampler, diSpecular);
		bgfx::setTexture(9, m_specularSampler, specularActive ? m_specularTextures[m_specularRead] : m_blackTexture);
		bgfx::setUniform(m_shadowViewProjUniform, shadowViewProj, 6);
		bgfx::setUniform(m_shadowInfoUniform, shadowInfo);
		bgfx::setUniform(m_shadowOptionsUniform, shadowOptions);
		bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
		bgfx::setUniform(m_lightPosition, lightPosition);
		bgfx::setUniform(m_lightColor, lightColor);
		bgfx::setUniform(m_cameraPosition, cameraPosition);
		bgfx::setUniform(m_screenInfo, screenInfo);
		bgfx::setUniform(m_sceneInfoUniform, sceneInfo);
		setSkyUniforms();
		setFullscreenGeometry();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		bgfx::submit(kCompositeView, m_compositeProgram);
	}

	// Auto exposure: measure the composite's output and adapt to it. A data
	// view replaces that output, so the exposure holds still meanwhile.
	if (_settings.autoExposure && !dataView)
	{
		bgfx::setMarker("Auto Exposure");
		bgfx::setViewName(kExposureView, "Auto Exposure");
		const float exposureAdapt[] =
		{
			1.0f - std::exp(-_settings.adaptationSpeed * _input.m_deltaTime),
			m_exposureValid ? 0.0f : 1.0f,
			0.0f,
			0.0f,
		};
		bgfx::setTexture(0, m_hdrSampler, m_hdrTexture);
		bgfx::setBuffer(1, m_exposureBuffer, bgfx::Access::ReadWrite);
		bgfx::setUniform(m_exposureAdaptUniform, exposureAdapt);
		bgfx::dispatch(kExposureView, m_exposureProgram, 1, 1, 1);
		m_exposureValid = true;
	}
	else if (!_settings.autoExposure)
	{
		// Turning it back on starts from the scene as it is then.
		m_exposureValid = false;
	}

	// TAA: blend this frame into the reprojected history.
	bgfx::TextureHandle display = m_hdrTexture;
	if (taa)
	{
		const uint8_t taaWrite = uint8_t(1 - m_taaRead);
		bgfx::setMarker("TAA");
		bgfx::setViewName(kTaaView, "TAA");
		bgfx::setViewRect(kTaaView, 0, 0, uint16_t(width), uint16_t(height));
		bgfx::setViewFrameBuffer(kTaaView, m_taaFrameBuffers[taaWrite]);
		bgfx::setViewTransform(kTaaView, nullptr, nullptr);
		const float taaInfo[] =
		{
			m_taaValid ? 1.0f : 0.0f,
			bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f,
			1.0f / float(width),
			1.0f / float(height),
		};
		bgfx::setTexture(0, m_hdrSampler, m_hdrTexture);
		bgfx::setTexture(1, m_taaHistorySampler, m_taaTextures[m_taaRead]);
		bgfx::setTexture(2, m_depthSampler, m_gbufferDepth);
		bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
		bgfx::setUniform(m_taaPreviousViewProjUniform, m_previousUnjitteredViewProj);
		bgfx::setUniform(m_taaInfoUniform, taaInfo);
		setFullscreenGeometry();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		bgfx::submit(kTaaView, m_taaProgram);
		display = m_taaTextures[taaWrite];
		m_taaRead = taaWrite;
		m_taaValid = true;
	}
	else
	{
		// The history would be stale when TAA comes back.
		m_taaValid = false;
	}

	bgfx::setMarker("Tone Mapping");
	bgfx::setViewName(kTonemapView, "Tone Mapping");
	bgfx::setViewRect(kTonemapView, 0, 0, uint16_t(width), uint16_t(height));
	bgfx::setViewFrameBuffer(kTonemapView, BGFX_INVALID_HANDLE);
	bgfx::setViewTransform(kTonemapView, nullptr, nullptr);
	const float tonemap[] =
	{
		_settings.exposureEv,
		_settings.autoExposure ? 1.0f : 0.0f,
		float(_settings.tonemapper),
		dataView ? 1.0f : 0.0f,
	};
	bgfx::setTexture(0, m_hdrSampler, display);
	bgfx::setBuffer(1, m_exposureBuffer, bgfx::Access::Read);
	bgfx::setUniform(m_tonemapUniform, tonemap);
	setFullscreenGeometry();
	bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
	bgfx::submit(kTonemapView, m_tonemapProgram);

	bx::memCopy(m_previousViewProj, viewProjection, sizeof(m_previousViewProj));
	bx::memCopy(m_previousUnjitteredViewProj, unjitteredViewProjection, sizeof(m_previousUnjitteredViewProj));
	m_previousEye = eye;
	m_historyRead = historyWrite;
	m_historyValid = true;
	++m_frameNumber;
}

// Rebuilds the light list when what goes into it changed: the scene, the
// emitters setting, or the extra lights.
void Renderer::updateLightList(const Settings& _settings, float _intensityScale)
{
	LightListKey key;
	key.m_emitters = _settings.emissiveLighting;
	key.m_extraCount = _settings.light.extraLights;
	key.m_extraIntensity = _settings.light.extraIntensity;
	key.m_extraSeed = _settings.light.extraSeed;
	key.m_intensityScale = _intensityScale;
	key.m_valid = true;
	if (key == m_lightListKey)
	{
		return;
	}
	m_lightListKey = key;

	destroyIfValid(m_lightBuffer);
	ExtraLights extra;
	extra.m_count = key.m_extraCount;
	extra.m_intensity = key.m_extraIntensity;
	extra.m_seed = uint32_t(key.m_extraSeed);
	static const std::vector<EmissiveTriangle> s_noEmitters;
	const LightList list = buildLightList(key.m_emitters ? m_emitters : s_noEmitters
		, extra, m_scene.m_boundsMin, m_scene.m_boundsMax, _intensityScale);
	m_lightCount = list.m_count;
	m_lightTriangles = list.m_triangles;
	if (m_lightCount > 0)
	{
		bgfx::VertexLayout vec4Layout;
		vec4Layout.begin().add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float).end();
		m_lightBuffer = bgfx::createVertexBuffer(
			  bgfx::copy(list.m_data.data(), uint32_t(list.m_data.size() * sizeof(float)))
			, vec4Layout
			, BGFX_BUFFER_COMPUTE_READ
			);
	}
	// Stored light indices refer to the old list.
	m_diValid = false;
}

// Binds a material's textures (white where it has none) and parameters
// for the next draw, in the G-buffer pass or (base colour only, for alpha
// masks) the shadow pass.
void Renderer::setMaterial(uint32_t _material, bool _gbuffer)
{
	const SceneMaterial& material = m_materials[_material];
	const auto texture = [this](int32_t _image, bgfx::TextureHandle _fallback)
	{
		return _image >= 0 ? m_modelTextures[_image] : _fallback;
	};

	bgfx::setTexture(0, m_baseColorSampler, texture(material.m_baseColorImage, m_whiteTexture));
	const float materialInfo[] =
	{
		material.m_alphaCutoff,
		material.m_metallicTextureMissing ? 0.0f : 1.0f,
		material.m_twoSidedMask ? 1.0f : 0.0f,
		0.0f,
	};
	bgfx::setUniform(m_materialInfoUniform, materialInfo);
	if (!_gbuffer)
	{
		return;
	}

	bgfx::setTexture(1, m_metallicRoughnessSampler, texture(material.m_metallicRoughnessImage, m_whiteTexture));
	bgfx::setTexture(2, m_normalMapSampler, texture(material.m_normalImage, m_flatNormalTexture));
	bgfx::setTexture(3, m_occlusionSampler, texture(material.m_occlusionImage, m_whiteTexture));
	bgfx::setTexture(4, m_emissiveMapSampler, texture(material.m_emissiveImage, m_whiteTexture));
	const float emissive[] = { material.m_emissive[0], material.m_emissive[1], material.m_emissive[2], 0.0f };
	const float factors[] = { material.m_normalScale, material.m_occlusionStrength, 0.0f, 0.0f };
	bgfx::setUniform(m_materialEmissiveUniform, emissive);
	bgfx::setUniform(m_materialFactorsUniform, factors);
}

void Renderer::setFullscreenGeometry()
{
	bgfx::setVertexBuffer(0, m_fullscreenVbh);
	bgfx::setIndexBuffer(m_fullscreenIbh);
}

// The frame buffers own their textures; createRenderTargets makes new ones.
void Renderer::destroyRenderTargets()
{
	destroyIfValid(m_gbuffer);
	destroyIfValid(m_hdrFrameBuffer);
	destroyIfValid(m_diReservoirFrameBuffers[0]);
	destroyIfValid(m_diReservoirFrameBuffers[1]);
	destroyIfValid(m_diShadeFrameBuffer);
	destroyIfValid(m_taaFrameBuffers[0]);
	destroyIfValid(m_taaFrameBuffers[1]);
	for (uint8_t idx = 0; idx < 2; ++idx)
	{
		destroyIfValid(m_reservoirFrameBuffers[idx]);
		destroyIfValid(m_shadeFrameBuffers[idx]);
		destroyIfValid(m_filteredHistoryFrameBuffers[idx]);
		destroyIfValid(m_filterFrameBuffers[idx]);
		destroyIfValid(m_specularFrameBuffers[idx]);
	}
}

// (Re)creates the shadow atlas at the face size chosen in the settings.
void Renderer::createShadowMap(const Settings& _settings)
{
	destroyIfValid(m_shadowFrameBuffer);
	m_shadowFaceSize = kShadowFaceSizes[_settings.shadowFaceSize];

	const uint64_t samplerFlags = BGFX_SAMPLER_MIN_POINT
		| BGFX_SAMPLER_MAG_POINT
		| BGFX_SAMPLER_MIP_POINT
		| BGFX_SAMPLER_U_CLAMP
		| BGFX_SAMPLER_V_CLAMP;
	if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::R32F, BGFX_TEXTURE_RT | samplerFlags))
	{
		throw std::runtime_error("Shadows require R32F render targets.");
	}

	const uint16_t atlasWidth = uint16_t(3 * m_shadowFaceSize);
	const uint16_t atlasHeight = uint16_t(2 * m_shadowFaceSize);
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

void Renderer::createRenderTargets(uint32_t _width, uint32_t _height, const Settings& _settings)
{
	destroyRenderTargets();

	const uint64_t samplerFlags = BGFX_SAMPLER_MIN_POINT
		| BGFX_SAMPLER_MAG_POINT
		| BGFX_SAMPLER_MIP_POINT
		| BGFX_SAMPLER_U_CLAMP
		| BGFX_SAMPLER_V_CLAMP;
	const uint32_t giDivisor = kGiResolutions[_settings.giResolution].m_divisor;
	const uint16_t giWidth = uint16_t(std::max(1u, _width / giDivisor));
	const uint16_t giHeight = uint16_t(std::max(1u, _height / giDivisor));

	if (bgfx::getCaps()->limits.maxFBAttachments < 4
	||  !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT)
	||  !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA32F, BGFX_TEXTURE_RT))
	{
		throw std::runtime_error(
			"ReSTIR GI requires four colour attachments and RGBA16F/RGBA32F render targets.");
	}

	const auto createTarget = [&](uint32_t _targetWidth, uint32_t _targetHeight, bgfx::TextureFormat::Enum _format)
	{
		return bgfx::createTexture2D(
			  uint16_t(_targetWidth), uint16_t(_targetHeight), false, 1, _format
			, BGFX_TEXTURE_RT | samplerFlags
			);
	};

	m_gbufferAlbedo = createTarget(_width, _height, bgfx::TextureFormat::BGRA8);
	m_gbufferNormal = createTarget(_width, _height, bgfx::TextureFormat::BGRA8);
	m_gbufferEmissive = createTarget(_width, _height, bgfx::TextureFormat::RGBA16F);
	m_gbufferShadingNormal = createTarget(_width, _height, bgfx::TextureFormat::BGRA8);
	m_gbufferDepth = createTarget(_width, _height
		, bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT | samplerFlags)
		? bgfx::TextureFormat::D32F
		: bgfx::TextureFormat::D24
		);
	const bgfx::TextureHandle gbufferTextures[] =
	{
		m_gbufferAlbedo, m_gbufferNormal, m_gbufferEmissive, m_gbufferShadingNormal, m_gbufferDepth,
	};
	m_gbuffer = bgfx::createFrameBuffer(BX_COUNTOF(gbufferTextures), gbufferTextures, true);

	m_hdrTexture = createTarget(_width, _height, bgfx::TextureFormat::RGBA16F);
	m_hdrFrameBuffer = bgfx::createFrameBuffer(1, &m_hdrTexture, true);

	// ReSTIR DI reservoirs (per frame parity) and its shaded output, at full
	// resolution.
	for (uint8_t idx = 0; idx < 2; ++idx)
	{
		m_diSample[idx] = createTarget(_width, _height, bgfx::TextureFormat::RGBA32F);
		m_diReceiver[idx] = createTarget(_width, _height, bgfx::TextureFormat::RGBA32F);
		m_diNormal[idx] = createTarget(_width, _height, bgfx::TextureFormat::RGBA16F);
		const bgfx::TextureHandle reservoirTextures[] = { m_diSample[idx], m_diReceiver[idx], m_diNormal[idx] };
		m_diReservoirFrameBuffers[idx] = bgfx::createFrameBuffer(BX_COUNTOF(reservoirTextures), reservoirTextures, true);
	}
	m_diDiffuse = createTarget(_width, _height, bgfx::TextureFormat::RGBA16F);
	m_diSpecular = createTarget(_width, _height, bgfx::TextureFormat::RGBA16F);
	const bgfx::TextureHandle diShadeTextures[] = { m_diDiffuse, m_diSpecular };
	m_diShadeFrameBuffer = bgfx::createFrameBuffer(BX_COUNTOF(diShadeTextures), diShadeTextures, true);
	m_diValid = false;

	// TAA history, read with bilinear filtering.
	for (uint8_t idx = 0; idx < 2; ++idx)
	{
		m_taaTextures[idx] = bgfx::createTexture2D(uint16_t(_width), uint16_t(_height), false, 1
			, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
		m_taaFrameBuffers[idx] = bgfx::createFrameBuffer(1, &m_taaTextures[idx], true);
	}

	for (uint8_t idx = 0; idx < 2; ++idx)
	{
		// Per frame parity, the temporal pass writes the reservoirs (sample
		// position + M, receiver position + W, receiver normal, which the
		// pairwise MIS weights need), and the spatial pass writes the
		// accumulated (demodulated) estimate and the luminance moments +
		// history length (needed by the denoiser). Positions need full
		// float precision, and all of them must be point sampled.
		m_historyColor[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
		m_historySample[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA32F);
		m_historyReceiver[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA32F);
		m_historyNormal[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA32F); // w: exact triangle ids
		m_historyMoments[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
		const bgfx::TextureHandle reservoirTextures[] =
		{
			m_historySample[idx], m_historyReceiver[idx], m_historyNormal[idx],
		};
		m_reservoirFrameBuffers[idx] = bgfx::createFrameBuffer(
			BX_COUNTOF(reservoirTextures), reservoirTextures, true);
		const bgfx::TextureHandle shadeTextures[] =
		{
			m_historyColor[idx], m_historyMoments[idx],
		};
		m_shadeFrameBuffers[idx] = bgfx::createFrameBuffer(
			BX_COUNTOF(shadeTextures), shadeTextures, true);

		// Denoiser: the filtered history (one per frame parity, like the
		// rest of the history) and two ping-pong targets.
		m_filteredHistory[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
		m_filteredHistoryFrameBuffers[idx] = bgfx::createFrameBuffer(1, &m_filteredHistory[idx], true);
		m_filterTextures[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
		m_filterFrameBuffers[idx] = bgfx::createFrameBuffer(1, &m_filterTextures[idx], true);

		// Indirect specular, accumulated per frame parity.
		m_specularTextures[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
		m_specularFrameBuffers[idx] = bgfx::createFrameBuffer(1, &m_specularTextures[idx], true);
	}

	if (!bgfx::isValid(m_gbuffer) || !bgfx::isValid(m_hdrFrameBuffer)
	||  !bgfx::isValid(m_taaFrameBuffers[0]) || !bgfx::isValid(m_taaFrameBuffers[1])
	||  !bgfx::isValid(m_diReservoirFrameBuffers[0]) || !bgfx::isValid(m_diReservoirFrameBuffers[1])
	||  !bgfx::isValid(m_diShadeFrameBuffer)
	||  !bgfx::isValid(m_reservoirFrameBuffers[0]) || !bgfx::isValid(m_reservoirFrameBuffers[1])
	||  !bgfx::isValid(m_shadeFrameBuffers[0]) || !bgfx::isValid(m_shadeFrameBuffers[1])
	||  !bgfx::isValid(m_filteredHistoryFrameBuffers[0]) || !bgfx::isValid(m_filteredHistoryFrameBuffers[1])
	||  !bgfx::isValid(m_filterFrameBuffers[0]) || !bgfx::isValid(m_filterFrameBuffers[1]))
	{
		throw std::runtime_error("Failed to create the G-buffer or ReSTIR GI history targets.");
	}

	m_targetWidth = _width;
	m_targetHeight = _height;
	m_targetGiDivisor = giDivisor;
	m_historyRead = 0;
	m_historyValid = false;
	m_taaValid = false;
	m_frameNumber = 0;
	bx::mtxIdentity(m_previousViewProj);
}
