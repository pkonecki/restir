#pragma once

// The deferred renderer: shadow map, G-buffer, world-space hash grid, ReSTIR
// GI temporal and spatial passes, SVGF-style denoiser and composite. It owns
// every GPU resource except the settings window's.

#include "camera.h"
#include "scene.h"
#include "settings.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <cstdint>
#include <vector>

// Views, in the order they run (see the setViewOrder call in Renderer::init).
constexpr bgfx::ViewId kGeometryView = 0;
constexpr bgfx::ViewId kTemporalView = 1;
constexpr bgfx::ViewId kCompositeView = 2;
constexpr bgfx::ViewId kShadowViewBase = 3;  // views 3-8, one per cube face
constexpr bgfx::ViewId kDenoiseViewBase = 9; // views 9-14: variance, then the a-trous iterations
constexpr bgfx::ViewId kGridClearView = 15;
constexpr bgfx::ViewId kGridInsertView = 16;
constexpr bgfx::ViewId kSpatialView = 17;
constexpr bgfx::ViewId kExposureView = 18;   // auto exposure (compute)
constexpr bgfx::ViewId kTonemapView = 19;    // exposure and tone mapping, to the window
constexpr bgfx::ViewId kUiView = 20;         // the settings window, drawn over everything

// GPU timings in the performance section: the views of each group of passes.
struct PassGroup
{
	const char* m_name;
	bgfx::ViewId m_firstView;
	bgfx::ViewId m_lastView;
};

constexpr PassGroup kPassGroups[] =
{
	{ "Shadow map",      kShadowViewBase,  kShadowViewBase + 5 },
	{ "G-buffer",        kGeometryView,    kGeometryView },
	{ "Hash grid",       kGridClearView,   kGridInsertView },
	{ "ReSTIR temporal", kTemporalView,    kTemporalView },
	{ "ReSTIR spatial",  kSpatialView,     kSpatialView },
	{ "Denoiser",        kDenoiseViewBase, kDenoiseViewBase + 5 },
	{ "Composite",       kCompositeView,   kCompositeView },
	{ "Tone mapping",    kExposureView,    kTonemapView },
};
constexpr int kPassGroupCount = int(BX_COUNTOF(kPassGroups));

// What changes from frame to frame besides the settings.
struct FrameInput
{
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	CameraMatrices m_camera;
	bx::Vec3 m_eye = { 0.0f, 0.0f, 0.0f };
	float m_deltaTime = 0.0f; // seconds since the last frame, for the exposure adaptation
};

class Renderer
{
public:
	// Creates the programs, uniforms and buffers. Throws if the renderer
	// lacks a feature the passes need.
	void init(const Settings& _settings);
	void shutdown();

	// Replaces the scene's buffers and textures. Restarts the history.
	void setScene(const SceneMesh& _mesh, const SceneInfo& _info);

	// Submits every pass of a frame (but does not call bgfx::frame).
	void render(const FrameInput& _input, const Settings& _settings);

	// Last frame's reservoirs and accumulated GI no longer apply.
	void invalidateHistory() { m_historyValid = false; }

	// GI targets are the window size over this.
	uint32_t giDivisor() const { return m_targetGiDivisor; }

private:
	void destroyScene();
	void setMaterial(uint32_t _material);
	void setFullscreenGeometry();
	void destroyRenderTargets();
	void createRenderTargets(uint32_t _width, uint32_t _height, const Settings& _settings);
	void createShadowMap(const Settings& _settings);

	SceneInfo m_scene;

	// Scene geometry and materials.
	bgfx::VertexBufferHandle m_vbh = BGFX_INVALID_HANDLE;
	bgfx::IndexBufferHandle m_ibh = BGFX_INVALID_HANDLE;
	std::vector<bgfx::TextureHandle> m_modelTextures;
	std::vector<SceneMaterial> m_materials;
	std::vector<SceneDraw> m_draws;
	bgfx::TextureHandle m_whiteTexture = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_baseColorSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_metallicRoughnessSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_materialInfoUniform = BGFX_INVALID_HANDLE;

	bgfx::VertexBufferHandle m_fullscreenVbh = BGFX_INVALID_HANDLE;
	bgfx::IndexBufferHandle m_fullscreenIbh = BGFX_INVALID_HANDLE;

	// G-buffer and the per-frame-parity history of the GI passes.
	bgfx::FrameBufferHandle m_gbuffer = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_gbufferAlbedo = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_gbufferNormal = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_gbufferDepth = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_historyColor[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historySample[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historyReceiver[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historyNormal[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historyMoments[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::FrameBufferHandle m_reservoirFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE }; // sample, receiver, normal
	bgfx::FrameBufferHandle m_shadeFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };     // colour, moments
	uint8_t m_historyRead = 0;
	bool m_historyValid = false;
	uint32_t m_targetWidth = 0;
	uint32_t m_targetHeight = 0;
	uint32_t m_targetGiDivisor = 0;
	float m_previousViewProj[16] = {};
	bx::Vec3 m_previousEye = { 0.0f, 0.0f, 0.0f };
	uint32_t m_frameNumber = 0;

	// The composite's scene-referred output, tone mapped to the window, and
	// the adapted log2 luminance of auto exposure.
	bgfx::TextureHandle m_hdrTexture = BGFX_INVALID_HANDLE;
	bgfx::FrameBufferHandle m_hdrFrameBuffer = BGFX_INVALID_HANDLE;
	bgfx::DynamicIndexBufferHandle m_exposureBuffer = BGFX_INVALID_HANDLE;
	bool m_exposureValid = false; // false: jump to the next frame's luminance

	// Denoiser targets.
	bgfx::TextureHandle m_filteredHistory[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::FrameBufferHandle m_filteredHistoryFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_filterTextures[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::FrameBufferHandle m_filterFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };

	// Shadow atlas.
	bgfx::TextureHandle m_shadowMap = BGFX_INVALID_HANDLE;
	bgfx::FrameBufferHandle m_shadowFrameBuffer = BGFX_INVALID_HANDLE;
	uint16_t m_shadowFaceSize = 0; // of the current shadow map

	// World-space hash grid, one per frame parity like the reservoirs.
	bgfx::DynamicIndexBufferHandle m_gridChecksums[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::DynamicIndexBufferHandle m_gridCounts[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::DynamicIndexBufferHandle m_gridEntries[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };

	// Programs.
	bgfx::ProgramHandle m_geometryProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_temporalProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_spatialProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_compositeProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_shadowProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_varianceProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_atrousProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_gridClearProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_gridInsertProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_exposureProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_tonemapProgram = BGFX_INVALID_HANDLE;

	// Uniforms and samplers.
	bgfx::UniformHandle m_lightPosition = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_lightColor = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_cameraPosition = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_albedoSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_normalSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_depthSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_historySampleSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_historyReceiverSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_historyColorSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_historyNormalSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_giSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_inverseViewProjection = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_viewProjection = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_previousViewProjection = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_frameInfo = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_screenInfo = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_sceneInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_shadowSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_shadowViewProjUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_shadowInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_shadowOptionsUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_historyMomentsSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_giColorSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_giMomentsSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_giReceiverSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_giNormalSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_filterInputSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_atrousInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_reservoirSampleSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_reservoirReceiverSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_reservoirNormalSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_gridInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_gridCameraUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_restirOptionsUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_hdrSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_exposureAdaptUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_tonemapUniform = BGFX_INVALID_HANDLE;
};
