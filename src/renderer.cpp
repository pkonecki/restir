#include "renderer.h"

#include "bgfx_utils.h"

#include <algorithm>
#include <cmath>
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

// Buffer stages of the grid, in the compute passes and in the GI passes.
constexpr uint8_t kGridComputeStage = 0;  // 0-2
constexpr uint8_t kGridFragmentStage = 11; // 11-13

} // namespace

void Renderer::init(const Settings& _settings)
{
	bgfx::setViewClear(kGeometryView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
	bgfx::setViewClear(kCompositeView, BGFX_CLEAR_COLOR, 0x20242bff, 1.0f, 0);

	// The shadow faces must be rendered before the GI and composite passes.
	const bgfx::ViewId viewOrder[] =
	{
		kShadowViewBase + 0, kShadowViewBase + 1, kShadowViewBase + 2,
		kShadowViewBase + 3, kShadowViewBase + 4, kShadowViewBase + 5,
		kGeometryView, kGridClearView, kGridInsertView, kTemporalView, kSpatialView,
		kDenoiseViewBase + 0, kDenoiseViewBase + 1, kDenoiseViewBase + 2,
		kDenoiseViewBase + 3, kDenoiseViewBase + 4, kDenoiseViewBase + 5,
		kCompositeView, kUiView,
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
	// factors apply unchanged.
	m_baseColorSampler = bgfx::createUniform("s_baseColor", bgfx::UniformType::Sampler);
	m_metallicRoughnessSampler = bgfx::createUniform("s_metallicRoughness", bgfx::UniformType::Sampler);
	m_materialInfoUniform = bgfx::createUniform("u_materialInfo", bgfx::UniformType::Vec4);
	const uint32_t white = 0xffffffff;
	m_whiteTexture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(&white, sizeof(white)));

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

	BX_ASSERT(bgfx::isValid(m_geometryProgram), "Failed to load G-buffer shaders.");
	BX_ASSERT(bgfx::isValid(m_temporalProgram), "Failed to load ReSTIR GI temporal shaders.");
	BX_ASSERT(bgfx::isValid(m_spatialProgram), "Failed to load ReSTIR GI spatial shaders.");
	BX_ASSERT(bgfx::isValid(m_gridClearProgram), "Failed to load the hash grid clear shader.");
	BX_ASSERT(bgfx::isValid(m_gridInsertProgram), "Failed to load the hash grid insert shader.");
	BX_ASSERT(bgfx::isValid(m_compositeProgram), "Failed to load composite shaders.");
	BX_ASSERT(bgfx::isValid(m_shadowProgram), "Failed to load shadow shaders.");
	BX_ASSERT(bgfx::isValid(m_varianceProgram), "Failed to load denoiser variance shaders.");
	BX_ASSERT(bgfx::isValid(m_atrousProgram), "Failed to load denoiser filter shaders.");
	createShadowMap(_settings);
}

void Renderer::shutdown()
{
	destroyRenderTargets();
	destroyIfValid(m_shadowFrameBuffer);
	destroyScene();
	destroyIfValid(m_whiteTexture);
	destroyIfValid(m_fullscreenVbh);
	destroyIfValid(m_fullscreenIbh);
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

	m_scene = _info;
	m_historyValid = false;
}

void Renderer::destroyScene()
{
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

	const float radius = m_scene.m_radius;
	const bx::Vec3 eye = _input.m_eye;
	const float* view = _input.m_camera.m_view;
	const float* projection = _input.m_camera.m_projection;

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
	const float lightColor[] = { lightIntensityScale, lightIntensityScale, lightIntensityScale, 1.0f };
	const float sceneInfo[] = { sceneScale, 0.0f, 0.0f, 0.0f };
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
					setMaterial(draw.m_material);
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
		setMaterial(draw.m_material);
		bgfx::setState(
			  BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_WRITE_Z
			| BGFX_STATE_DEPTH_TEST_LESS
			);
		bgfx::submit(kGeometryView, m_geometryProgram);
	}

	// This frame's reservoirs, shaded GI and hash grid are written at
	// historyWrite; last frame's are read from m_historyRead.
	const uint8_t historyWrite = uint8_t(1 - m_historyRead);

	const float frameInfo[] =
	{
		float(m_frameNumber & 0xffffff)
		, m_historyValid ? 1.0f : 0.0f
		, bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f
		, _settings.multipleBounces ? 1.0f : 0.0f
	};
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
	bgfx::setViewFrameBuffer(kCompositeView, BGFX_INVALID_HANDLE);
	bgfx::setViewTransform(kCompositeView, nullptr, nullptr);
	bgfx::setTexture(0, m_albedoSampler, m_gbufferAlbedo);
	bgfx::setTexture(1, m_normalSampler, m_gbufferNormal);
	bgfx::setTexture(2, m_depthSampler, m_gbufferDepth);
	bgfx::setTexture(3, m_giSampler, denoised);
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
	bgfx::submit(kCompositeView, m_compositeProgram);

	bx::memCopy(m_previousViewProj, viewProjection, sizeof(m_previousViewProj));
	m_previousEye = eye;
	m_historyRead = historyWrite;
	m_historyValid = true;
	++m_frameNumber;
}

// Binds a material's textures (white where it has none) and parameters
// for the next draw, in the G-buffer or shadow pass.
void Renderer::setMaterial(uint32_t _material)
{
	const SceneMaterial& material = m_materials[_material];
	const auto texture = [this](int32_t _image)
	{
		return _image >= 0 ? m_modelTextures[_image] : m_whiteTexture;
	};

	bgfx::setTexture(0, m_baseColorSampler, texture(material.m_baseColorImage));
	bgfx::setTexture(1, m_metallicRoughnessSampler, texture(material.m_metallicRoughnessImage));
	const float materialInfo[] =
	{
		material.m_alphaCutoff,
		material.m_metallicTextureMissing ? 0.0f : 1.0f,
		material.m_twoSidedMask ? 1.0f : 0.0f,
		0.0f,
	};
	bgfx::setUniform(m_materialInfoUniform, materialInfo);
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
	for (uint8_t idx = 0; idx < 2; ++idx)
	{
		destroyIfValid(m_reservoirFrameBuffers[idx]);
		destroyIfValid(m_shadeFrameBuffers[idx]);
		destroyIfValid(m_filteredHistoryFrameBuffers[idx]);
		destroyIfValid(m_filterFrameBuffers[idx]);
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

	if (bgfx::getCaps()->limits.maxFBAttachments < 3
	||  !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT)
	||  !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA32F, BGFX_TEXTURE_RT))
	{
		throw std::runtime_error(
			"ReSTIR GI requires three framebuffer attachments and RGBA16F/RGBA32F render targets.");
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
	m_gbufferDepth = createTarget(_width, _height
		, bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT | samplerFlags)
		? bgfx::TextureFormat::D32F
		: bgfx::TextureFormat::D24
		);
	const bgfx::TextureHandle gbufferTextures[] =
	{
		m_gbufferAlbedo, m_gbufferNormal, m_gbufferDepth,
	};
	m_gbuffer = bgfx::createFrameBuffer(BX_COUNTOF(gbufferTextures), gbufferTextures, true);

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
		m_historyNormal[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
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
	}

	if (!bgfx::isValid(m_gbuffer)
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
	m_frameNumber = 0;
	bx::mtxIdentity(m_previousViewProj);
}
