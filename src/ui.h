#pragma once

// The ReSTIR GI settings window (Dear ImGui) and the timings it shows. Every
// toggle and setting belongs in this window.

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
};

// What the window asks the application to do.
struct SettingsWindowOutput
{
	std::optional<std::size_t> m_switchModel;
	bool m_moveLightToCamera = false;
};

// Draws the window. Settings change in place; the caller reacts to what
// changed (see main.cpp).
SettingsWindowOutput drawSettingsWindow(const SettingsWindowInput& _input, Settings& _settings, FlyCamera& _camera);
