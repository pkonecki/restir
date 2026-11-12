#include "ui.h"

#include "camera.h"
#include "imgui/imgui.h"

#include <bgfx/bgfx.h>
#include <bx/string.h>

#include <algorithm>
#include <cfloat>

namespace
{

constexpr uint32_t kStaleTimingFrames = 60; // a pass not timed for this long shows as "-"

} // namespace

void PerformanceStats::update()
{
	const bgfx::Stats* stats = bgfx::getStats();
	const auto smooth = [](float& _value, float _sample)
	{
		_value = _value <= 0.0f ? _sample : _value + 0.05f * (_sample - _value);
	};

	const float cpuFrameMs = float(double(stats->cpuTimeFrame) * 1000.0 / double(stats->cpuTimerFreq));
	smooth(m_cpuFrameMs, cpuFrameMs);
	m_frameTimes[m_frameTimeCursor] = cpuFrameMs;
	m_frameTimeCursor = (m_frameTimeCursor + 1) % kFrameTimeHistory;

	// Timer query results can skip frames, so a value is kept until it has
	// not been refreshed for kStaleTimingFrames (e.g. the denoiser is off).
	const double gpuTicksToMs = stats->gpuTimerFreq > 0 ? 1000.0 / double(stats->gpuTimerFreq) : 0.0;
	for (int group = 0; group < kPassGroupCount; ++group)
	{
		int64_t ticks = 0;
		bool found = false;
		for (uint16_t idx = 0; idx < stats->numViews; ++idx)
		{
			const bgfx::ViewStats& view = stats->viewStats[idx];
			if (view.view >= kPassGroups[group].m_firstView && view.view <= kPassGroups[group].m_lastView
			&&  view.gpuTimeEnd > view.gpuTimeBegin)
			{
				ticks += view.gpuTimeEnd - view.gpuTimeBegin;
				found = true;
			}
		}

		if (found && gpuTicksToMs > 0.0)
		{
			smooth(m_passGroupMs[group], float(double(ticks) * gpuTicksToMs));
			m_passGroupAge[group] = 0;
			m_gpuTimingsSeen = true;
		}
		else if (++m_passGroupAge[group] > kStaleTimingFrames)
		{
			m_passGroupMs[group] = 0.0f;
		}
	}

	if (m_gpuTimingsSeen && stats->gpuTimeEnd > stats->gpuTimeBegin)
	{
		smooth(m_gpuFrameMs, float(double(stats->gpuTimeEnd - stats->gpuTimeBegin) * gpuTicksToMs));
	}
}

SettingsWindowOutput drawSettingsWindow(const SettingsWindowInput& _input, Settings& _settings, FlyCamera& _camera)
{
	SettingsWindowOutput output;
	const PerformanceStats& stats = _input.m_stats;
	// Sized to its content, but no taller than the screen; it scrolls then.
	ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, 0.0f)
		, ImVec2(FLT_MAX, std::max(ImGui::GetIO().DisplaySize.y - 20.0f, 100.0f)));
	ImGui::Begin("ReSTIR GI", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
	const ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

	if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::BeginCombo("Model (M)", _input.m_modelPaths[_input.m_modelIndex].c_str()))
		{
			for (std::size_t idx = 0; idx < _input.m_modelPaths.size(); ++idx)
			{
				const bool selected = idx == _input.m_modelIndex;
				if (ImGui::Selectable(_input.m_modelPaths[idx].c_str(), selected) && !selected)
				{
					output.m_switchModel = idx;
				}
			}
			ImGui::EndCombo();
		}
		if (!_input.m_modelError.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", _input.m_modelError.c_str());
		}
		ImGui::SliderFloat("Camera speed", &_camera.m_speedScale, 0.05f, 20.0f, "x%.2f"
			, sliderFlags | ImGuiSliderFlags_Logarithmic);
		ImGui::SetItemTooltip("WASD move, Shift/Ctrl up/down, drag to look, wheel for speed.");
	}

	if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
	{
		LightSettings& light = _settings.light;
		ImGui::DragFloat3("Position", light.position, 0.005f * _input.m_sceneRadius, 0.0f, 0.0f, "%.3f");
		ImGui::SetItemTooltip("Drag to move, or Ctrl+click to type.");
		if (ImGui::Button("Move to camera (L)"))
		{
			output.m_moveLightToCamera = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset light"))
		{
			const LightSettings defaults;
			light = defaults;
			light.position[0] = _input.m_defaultLightPosition.x;
			light.position[1] = _input.m_defaultLightPosition.y;
			light.position[2] = _input.m_defaultLightPosition.z;
		}
		ImGui::ColorEdit3("Colour", light.color, ImGuiColorEditFlags_Float);
		ImGui::SliderFloat("Intensity", &light.intensity, 0.01f, 100.0f, "x%.2f"
			, sliderFlags | ImGuiSliderFlags_Logarithmic);
		ImGui::SetItemTooltip("Relative to the default, which scales with the scene size.");
		ImGui::Checkbox("Orbit", &light.orbit);
		ImGui::SetItemTooltip("Circle around the vertical axis through the scene's centre, at\nthe light's height. ReSTIR re-shades every reservoir's sample each\nframe, so the bounced light follows; the denoised history lags a little.");
		if (light.orbit)
		{
			ImGui::SliderFloat("Orbit radius", &light.orbitRadius, 0.0f, 1.0f, "%.2f scene radii", sliderFlags);
			ImGui::SliderFloat("Orbit speed", &light.orbitSpeed, -2.0f, 2.0f, "%.2f rad/s", sliderFlags);
		}
	}

	if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Checkbox("Multiple bounces (B)", &_settings.multipleBounces);
		ImGui::SetItemTooltip("Feed last frame's indirect light back into the samples.");
		ImGui::Checkbox("Shadows (H)", &_settings.shadows);
		ImGui::Checkbox("Denoiser (N)", &_settings.denoiser);
		int reuse = _settings.worldSpaceReuse ? 0 : 1;
		if (ImGui::Combo("Reuse (G)", &reuse, "World-space hash grid\0Screen-space\0"))
		{
			_settings.worldSpaceReuse = reuse == 0;
		}
		ImGui::SetItemTooltip("Where reservoirs are reused from.");
	}

	if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text("%.1f FPS  (%.2f ms per frame)", stats.m_cpuFrameMs > 0.0f ? 1000.0f / stats.m_cpuFrameMs : 0.0f, stats.m_cpuFrameMs);
		ImGui::PlotLines("##frameTimes", stats.m_frameTimes, kFrameTimeHistory, stats.m_frameTimeCursor
			, "frame time", 0.0f, std::max(2.0f * stats.m_cpuFrameMs, 1.0f), ImVec2(-1.0f, 40.0f));

		if (!stats.m_gpuTimingsSeen)
		{
			ImGui::TextDisabled("GPU timings need timer queries, which this\nrenderer or browser does not offer.");
		}
		else
		{
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 0.0f));
			if (ImGui::BeginTable("gpuTimings", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
			{
				const auto row = [](const char* _name, float _ms)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(_name);
					ImGui::TableNextColumn();
					if (_ms > 0.0f)
					{
						ImGui::Text("%.2f ms", _ms);
					}
					else
					{
						ImGui::TextDisabled("-");
					}
				};
				for (int group = 0; group < kPassGroupCount; ++group)
				{
					row(kPassGroups[group].m_name, stats.m_passGroupMs[group]);
				}
				row("GPU frame", stats.m_gpuFrameMs);
				ImGui::EndTable();
			}
			ImGui::PopStyleVar();
		}

		ImGui::SeparatorText("Quality / speed");
		ImGui::Checkbox("VSync", &_settings.vsync);
		ImGui::SetItemTooltip("Off to measure how fast frames can go.");

		const char* giResolutions[BX_COUNTOF(kGiResolutions)];
		for (std::size_t idx = 0; idx < BX_COUNTOF(kGiResolutions); ++idx)
		{
			giResolutions[idx] = kGiResolutions[idx].m_name;
		}
		ImGui::Combo("GI resolution", &_settings.giResolution, giResolutions, int(BX_COUNTOF(giResolutions)));
		ImGui::SetItemTooltip("Size of the ReSTIR and denoiser targets, relative to the window\n(%u x %u now). Everything in GI scales with the pixel count."
			, _input.m_giWidth, _input.m_giHeight);

		char shadowSizes[BX_COUNTOF(kShadowFaceSizes)][16];
		const char* shadowSizeNames[BX_COUNTOF(kShadowFaceSizes)];
		for (std::size_t idx = 0; idx < BX_COUNTOF(kShadowFaceSizes); ++idx)
		{
			bx::snprintf(shadowSizes[idx], sizeof(shadowSizes[idx]), "%u", unsigned(kShadowFaceSizes[idx]));
			shadowSizeNames[idx] = shadowSizes[idx];
		}
		ImGui::Combo("Shadow map face", &_settings.shadowFaceSize, shadowSizeNames, int(BX_COUNTOF(shadowSizeNames)));
		ImGui::SetItemTooltip("Texels per side of each of the six cube faces.");

		ImGui::SliderInt("Denoiser iterations", &_settings.atrousIterations, 1, kMaxAtrousIterations, "%d", sliderFlags);
		ImGui::SetItemTooltip("A-trous passes; each one doubles the filter radius.");
		ImGui::SliderInt("Rays per pixel", &_settings.initialCandidates, 1, 8, "%d", sliderFlags);
		ImGui::SetItemTooltip("Fresh candidates traced each frame in the temporal pass.");
		ImGui::SliderInt("Temporal taps", &_settings.temporalTaps, 1, 4, "%d", sliderFlags);
		ImGui::SetItemTooltip("Reservoirs reused from last frame.");
		ImGui::SliderInt("Spatial taps", &_settings.spatialTaps, 1, 8, "%d", sliderFlags);
		ImGui::SetItemTooltip("Reservoirs reused from this frame, each with a visibility ray.");
		ImGui::SliderInt("Ray march steps", &_settings.traceSteps, 8, 64, "%d", sliderFlags);
		ImGui::SetItemTooltip("Depth buffer steps per ray. Rays reach as far with fewer,\ncoarser steps, but miss thin geometry.");

		if (ImGui::Button("Reset to defaults"))
		{
			const Settings defaults;
			_settings.giResolution = defaults.giResolution;
			_settings.shadowFaceSize = defaults.shadowFaceSize;
			_settings.atrousIterations = defaults.atrousIterations;
			_settings.initialCandidates = defaults.initialCandidates;
			_settings.temporalTaps = defaults.temporalTaps;
			_settings.spatialTaps = defaults.spatialTaps;
			_settings.traceSteps = defaults.traceSteps;
			_settings.vsync = defaults.vsync;
		}
	}

	ImGui::End();
	return output;
}
