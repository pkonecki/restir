#pragma once

// The ReSTIR GI settings window (Dear ImGui) and the timings it shows. Every
// toggle and setting belongs in this window.

#include "bookmarks.h"
#include "renderer.h"
#include "settings.h"

#include <bx/math.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class FlyCamera;

constexpr int kFrameTimeHistory = 120; // frames in the frame time plot

// Smoothed timings in milliseconds (0 while unavailable) and the recent frame
// times for the plot.
struct PerformanceStats
{
	// Reads bgfx's timings of the last completed frame. GPU timings need
	// timer queries, which some renderers and browsers do not offer.
	void update();

	float m_cpuFrameMs = 0.0f;
	float m_gpuFrameMs = 0.0f;
	float m_passGroupMs[kPassGroupCount] = {};
	uint32_t m_passGroupAge[kPassGroupCount] = {}; // frames since the group was last timed
	bool m_gpuTimingsSeen = false;
	float m_frameTimes[kFrameTimeHistory] = {};
	int m_frameTimeCursor = 0;
};

// What the window shows besides the settings themselves.
struct SettingsWindowInput
{
	const std::vector<std::string>& m_modelPaths;
	std::size_t m_modelIndex;
	const std::string& m_modelError;
	const PerformanceStats& m_stats;
	uint32_t m_giWidth;
	uint32_t m_giHeight;
	float m_sceneRadius;
	bx::Vec3 m_defaultLightPosition; // where describeScene put the light
	const std::vector<CameraPose>& m_bookmarks; // the current model's
	const std::string& m_bookmarkError;
	CameraPose m_cameraPose;
	const std::string& m_screenshotResult;
	Renderer::BvhStats m_bvhStats;
	uint32_t m_lightCount;
	uint32_t m_emissiveTriangles;
	bool m_referenceAccumulating;
	uint32_t m_referenceFrames;
	Renderer::ReferenceMetrics m_referenceMetrics;
	std::string m_shareString; // the settings and the view (share.h)
	bool m_waveIntrinsicsAvailable; // the renderer is Vulkan and has the wave variants
};

// What the window asks the application to do.
struct SettingsWindowOutput
{
	std::optional<std::size_t> m_switchModel;
	bool m_moveLightToCamera = false;
	bool m_screenshot = false;
	bool m_addBookmark = false;
	std::optional<std::size_t> m_goToBookmark;
	std::optional<std::size_t> m_removeBookmark;
	bool m_toggleReference = false;
	bool m_clearReference = false;
};

// Draws the window. Settings change in place; the caller reacts to what
// changed (see main.cpp).
SettingsWindowOutput drawSettingsWindow(const SettingsWindowInput& _input, Settings& _settings, FlyCamera& _camera);
