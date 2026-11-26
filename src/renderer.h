#pragma once

// The deferred renderer: shadow map, G-buffer, world-space hash grid, ReSTIR
// GI temporal and spatial passes, SVGF-style denoiser and composite. It owns
// every GPU resource except the settings window's.

#include "bvh.h"
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
constexpr bgfx::ViewId kTaaView = 20;        // temporal anti-aliasing, before the exposure
constexpr bgfx::ViewId kUiView = 21;         // the settings window, drawn over everything
constexpr bgfx::ViewId kDiTemporalView = 22; // ReSTIR DI, after the G-buffer
constexpr bgfx::ViewId kDiSpatialView = 23;
constexpr bgfx::ViewId kSpecularView = 24;   // glossy reflections, after the GI passes
constexpr bgfx::ViewId kReferenceView = 25;  // reference accumulation, after TAA
constexpr bgfx::ViewId kErrorView = 26;      // error against the reference (compute)

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
	{ "DI temporal",     kDiTemporalView,  kDiTemporalView },
	{ "DI spatial",      kDiSpatialView,   kDiSpatialView },
	{ "Hash grid",       kGridClearView,   kGridInsertView },
	{ "GI temporal",     kTemporalView,    kTemporalView },
	{ "GI spatial",      kSpatialView,     kSpatialView },
	{ "Specular GI",     kSpecularView,    kSpecularView },
	{ "Denoiser",        kDenoiseViewBase, kDenoiseViewBase + 5 },
	{ "Composite",       kCompositeView,   kCompositeView },
	{ "TAA",             kTaaView,         kTaaView },
	{ "Reference",       kReferenceView,   kErrorView },
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

	// The current scene's BVH, for the settings window.
	struct BvhStats
	{
		uint32_t m_triangles = 0;
		uint32_t m_nodes = 0;
		uint32_t m_depth = 0;
		float m_buildMs = 0.0f;
	};
	const BvhStats& bvhStats() const { return m_bvhStats; }
	bool waveIntrinsicsAvailable() const { return m_waveProgramsValid; }

	// The reference: the unbiased image of the current view, accumulated
	// with every biased shortcut off, and the error of the live image
	// against it.
	struct ReferenceMetrics
	{
		bool m_valid = false;
		float m_relativeMse = 0.0f; // mean of (L - L_ref)^2 / (L_ref^2 + 0.01)
		float m_bias = 0.0f;        // mean luminance over the reference's, minus 1
	};
	void setReferenceAccumulation(bool _accumulate);
	void clearReference();
	bool referenceAccumulating() const { return m_referenceAccumulating; }
	uint32_t referenceFrames() const { return m_referenceFrames; }
	const ReferenceMetrics& referenceMetrics() const { return m_metrics; }

	// Call with what bgfx::frame returned, for the error read-back.
	void frameSubmitted(uint32_t _frame);

	// The light list ReSTIR DI samples, for the settings window.
	uint32_t lightCount() const { return m_lightCount; }
	uint32_t emissiveTriangleCount() const { return m_lightTriangles; }

private:
	void destroyScene();
	void setMaterial(uint32_t _material, bool _gbuffer);
	void setFullscreenGeometry();
	void destroyRenderTargets();
	void createRenderTargets(uint32_t _width, uint32_t _height, const Settings& _settings);
	void createShadowMap(const Settings& _settings);
	void updateLightList(const Settings& _settings, float _intensityScale);

	SceneInfo m_scene;

	// Scene geometry and materials.
	bgfx::VertexBufferHandle m_vbh = BGFX_INVALID_HANDLE;
	bgfx::IndexBufferHandle m_ibh = BGFX_INVALID_HANDLE;
	bgfx::VertexBufferHandle m_bvhBuffer = BGFX_INVALID_HANDLE; // see bvh.h
	float m_bvhInfo[4] = {};                                     // u_bvhInfo, without the ray mode
	BvhStats m_bvhStats;

	// The light list ReSTIR DI samples (see lights.h), rebuilt when its key
	// changes.
	struct LightListKey
	{
		bool m_valid = false;
		bool m_emitters = false;
		int m_extraCount = 0;
		float m_extraIntensity = 0.0f;
		int m_extraSeed = 0;
		float m_intensityScale = 0.0f;
		bool operator==(const LightListKey&) const = default;
	};
	std::vector<EmissiveTriangle> m_emitters;
	LightListKey m_lightListKey;
	bgfx::VertexBufferHandle m_lightBuffer = BGFX_INVALID_HANDLE;
	uint32_t m_lightCount = 0;
	uint32_t m_lightTriangles = 0;

	// ReSTIR DI reservoirs (per frame parity) and shaded output.
	bgfx::TextureHandle m_diSample[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_diReceiver[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_diNormal[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_diDiffuse = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_diSpecular = BGFX_INVALID_HANDLE;
	uint8_t m_diRead = 0;
	bool m_diValid = false;
	bgfx::TextureHandle m_blackTexture = BGFX_INVALID_HANDLE;

	// Indirect specular (fs_specular.sc), per frame parity.
	bgfx::TextureHandle m_specularTextures[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::FrameBufferHandle m_specularFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	uint8_t m_specularRead = 0;

	// Reference accumulation and the error read-back.
	bgfx::TextureHandle m_referenceTextures[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::FrameBufferHandle m_referenceFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	uint8_t m_referenceRead = 0;
	uint32_t m_referenceFrames = 0;
	bool m_referenceAccumulating = false;
	float m_referenceViewProj[16] = {};
	float m_referenceLight[3] = {};
	bgfx::DynamicIndexBufferHandle m_errorBuffer = BGFX_INVALID_HANDLE;
	uint32_t m_errorData[4] = {}; // float bits
	uint32_t m_errorReadFrame = 0; // the frame the pending read-back is ready, 0 for none
	ReferenceMetrics m_metrics;
	std::vector<bgfx::TextureHandle> m_modelTextures;
	std::vector<SceneMaterial> m_materials;
	std::vector<SceneDraw> m_draws;
	bgfx::TextureHandle m_whiteTexture = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_flatNormalTexture = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_baseColorSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_metallicRoughnessSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_normalMapSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_occlusionSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_emissiveMapSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_materialInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_materialEmissiveUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_materialFactorsUniform = BGFX_INVALID_HANDLE;

	bgfx::VertexBufferHandle m_fullscreenVbh = BGFX_INVALID_HANDLE;
	bgfx::IndexBufferHandle m_fullscreenIbh = BGFX_INVALID_HANDLE;

	// G-buffer and the per-frame-parity history of the GI passes.
	bgfx::FrameBufferHandle m_gbuffer = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_gbufferAlbedo = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_gbufferNormal = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_gbufferEmissive = BGFX_INVALID_HANDLE;      // rgb: emission, a: occlusion
	bgfx::TextureHandle m_gbufferShadingNormal = BGFX_INVALID_HANDLE; // with the normal map
	bgfx::TextureHandle m_gbufferDepth = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_historyColor[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historySample[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historyReceiver[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historyNormal[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historyMoments[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
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

	// TAA history (ping-pong) and last frame's unjittered view-projection.
	bgfx::TextureHandle m_taaTextures[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::FrameBufferHandle m_taaFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	uint8_t m_taaRead = 0;
	bool m_taaValid = false;
	float m_previousUnjitteredViewProj[16] = {};

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
	bgfx::DynamicIndexBufferHandle m_grid[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };

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
	bgfx::ProgramHandle m_debugProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_taaProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_diTemporalProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_diSpatialProgram = BGFX_INVALID_HANDLE;
	// The same four passes with wave intrinsics (Vulkan only, see compute.sh).
	bgfx::ProgramHandle m_temporalWaveProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_spatialWaveProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_diTemporalWaveProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_diSpatialWaveProgram = BGFX_INVALID_HANDLE;
	bool m_waveProgramsValid = false;
	bgfx::ProgramHandle m_specularProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_referenceProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_errorProgram = BGFX_INVALID_HANDLE;

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
	bgfx::UniformHandle m_debugViewUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_compositeOptionsUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_emissiveSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_shadingNormalSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_bvhInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_skyZenithUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_skyHorizonUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_skyGroundUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_taaHistorySampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_taaPreviousViewProjUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_taaInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_diInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_diPreviousUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_screenSizeUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_lightingInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_previousSampleSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_previousReceiverSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_previousNormalSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_surfaceLightSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_diDiffuseSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_diSpecularSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_specularSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_specularHistorySampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_previousHdrSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_compositeSpecularUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_referenceHistorySampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_referenceSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_displaySampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_referenceInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_errorInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_referenceViewUniform = BGFX_INVALID_HANDLE;
};
