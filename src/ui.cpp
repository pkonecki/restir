#include "ui.h"

#include "camera.h"
#include "imgui/imgui.h"

#include <bgfx/bgfx.h>
#include <bx/string.h>

#include <algorithm>
#include <cfloat>
#include <vector>

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

		ImGui::SeparatorText("Views");
		for (std::size_t idx = 0; idx < _input.m_bookmarks.size(); ++idx)
		{
			const CameraPose& pose = _input.m_bookmarks[idx];
			ImGui::PushID(int(idx));
			char label[16];
			bx::snprintf(label, sizeof(label), "%d", int(idx + 1));
			if (ImGui::Button(label, ImVec2(ImGui::GetFrameHeight(), 0.0f)))
			{
				output.m_goToBookmark = idx;
			}
			ImGui::SetItemTooltip("Go to this view (%d).", int(idx + 1));
			ImGui::SameLine();
			ImGui::Text("(%.2f, %.2f, %.2f)  yaw %.0f  pitch %.0f"
				, pose.m_eye.x, pose.m_eye.y, pose.m_eye.z, bx::toDeg(pose.m_yaw), bx::toDeg(pose.m_pitch));
			ImGui::SameLine();
			if (ImGui::SmallButton("x"))
			{
				output.m_removeBookmark = idx;
			}
			ImGui::SetItemTooltip("Forget this view.");
			ImGui::PopID();
		}
		ImGui::BeginDisabled(_input.m_bookmarks.size() >= kMaxBookmarks);
		if (ImGui::Button("Save this view"))
		{
			output.m_addBookmark = true;
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Kept per model in camera_bookmarks.txt; keys 1-9 recall them.");
		if (!_input.m_bookmarkError.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", _input.m_bookmarkError.c_str());
		}

		char pose[160];
		bx::strCopy(pose, sizeof(pose), formatPose(_input.m_cameraPose).c_str());
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##pose", pose, sizeof(pose), ImGuiInputTextFlags_ReadOnly);
		ImGui::SetItemTooltip("This view, as --camera takes it (x,y,z,yaw,pitch).");
		std::vector<char> share(_input.m_shareString.begin(), _input.m_shareString.end());
		share.push_back('\0');
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##share", share.data(), share.size(), ImGuiInputTextFlags_ReadOnly);
		ImGui::SetItemTooltip("This view and every setting, as --settings takes it (the web\nbuild keeps it in the page address, so the link reproduces it).");

		ImGui::SeparatorText("Screenshot");
		if (ImGui::Button("Save screenshot (P)"))
		{
			output.m_screenshot = true;
		}
		ImGui::SetItemTooltip("A PNG in screenshots/, named after the time.");
		ImGui::SameLine();
		ImGui::Checkbox("Hide this window", &_settings.hideWindowInScreenshots);
		if (!_input.m_screenshotResult.empty())
		{
			ImGui::TextDisabled("%s", _input.m_screenshotResult.c_str());
		}
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

		ImGui::SeparatorText("Extra lights");
		ImGui::BeginDisabled(_settings.directLighting != int(DirectLighting::ReStirDi));
		ImGui::SliderInt("Count", &light.extraLights, 0, 1024, "%d", sliderFlags | ImGuiSliderFlags_Logarithmic);
		ImGui::SetItemTooltip("Coloured point lights scattered through the scene, sampled with\nReSTIR DI and shadowed through the BVH (the main light keeps its\nshadow map). The light list holds %u lights now, %u of them\nemissive triangles.", _input.m_lightCount, _input.m_emissiveTriangles);
		if (light.extraLights > 0)
		{
			ImGui::SliderFloat("Each", &light.extraIntensity, 0.01f, 10.0f, "x%.2f", sliderFlags | ImGuiSliderFlags_Logarithmic);
			ImGui::SetItemTooltip("Intensity of each extra light, relative to the main light's default.");
			ImGui::SameLine();
			if (ImGui::Button("Scatter again"))
			{
				++light.extraSeed;
			}
		}
		ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader("Sky", ImGuiTreeNodeFlags_DefaultOpen))
	{
		SkySettings& sky = _settings.sky;
		ImGui::Checkbox("Sky (K)", &sky.enabled);
		ImGui::SetItemTooltip("Light from a sky gradient, through GI rays the BVH confirms leave\nthe scene (so not with depth buffer rays only), and shown behind it.");
		if (sky.enabled)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##skyIntensity", &sky.intensity, 0.01f, 20.0f, "intensity %.2f"
				, sliderFlags | ImGuiSliderFlags_Logarithmic);
			ImGui::ColorEdit3("Zenith", sky.zenith, ImGuiColorEditFlags_Float);
			ImGui::ColorEdit3("Horizon", sky.horizon, ImGuiColorEditFlags_Float);
			ImGui::ColorEdit3("Ground", sky.ground, ImGuiColorEditFlags_Float);
			if (_settings.rayMode == int(RayMode::ScreenSpace))
			{
				ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Depth buffer rays cannot reach the sky.");
			}
		}
	}

	if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Checkbox("Multiple bounces (B)", &_settings.multipleBounces);
		ImGui::SetItemTooltip("Feed last frame's indirect light back into the samples.");
		ImGui::Checkbox("Shadows (H)", &_settings.shadows);
		ImGui::Checkbox("Denoiser (N)", &_settings.denoiser);
		ImGui::SameLine();
		ImGui::Checkbox("TAA", &_settings.taa);
		ImGui::SetItemTooltip("Temporal anti-aliasing: each frame is rendered with a different\nsub-pixel offset, and the frames are blended over time.");
		ImGui::Checkbox("Specular GI", &_settings.specularGi);
		ImGui::SetItemTooltip("Glossy reflections: one GGX ray per GI pixel, traced like the GI\nrays and shaded from last frame's image where it hits a visible\nsurface, accumulated over a few frames.");
		ImGui::Checkbox("Emissive lighting (E)", &_settings.emissiveLighting);
		ImGui::SetItemTooltip("Emissive surfaces light the scene.");
		ImGui::Combo("Direct lighting", &_settings.directLighting, kDirectLightingNames, int(DirectLighting::Count));
		ImGui::SetItemTooltip("How emitters and the extra lights light the scene.\nReSTIR DI: their light is sampled with reservoirs at full resolution\nand shadowed through the BVH; GI adds the light they bounce.\nThrough GI only: emitters light the scene only through the GI rays\nthat happen to hit them (noisier, and only emitters the rays reach).");
		ImGui::Combo("Rays (R)", &_settings.rayMode, kRayModeNames, int(RayMode::Count));
		ImGui::SetItemTooltip("Depth buffer: rays only find what is on screen.\nThen BVH: rays the depth buffer cannot answer (off screen, or\nbehind what it shows) are traced through the scene's BVH.\nBVH only: every ray and visibility test is.\n\nBVH: %u triangles, %u nodes, depth %u, built in %.0f ms."
			, _input.m_bvhStats.m_triangles, _input.m_bvhStats.m_nodes, _input.m_bvhStats.m_depth, _input.m_bvhStats.m_buildMs);
		int reuse = _settings.worldSpaceReuse ? 0 : 1;
		if (ImGui::Combo("Reuse (G)", &reuse, "World-space hash grid\0Screen-space\0"))
		{
			_settings.worldSpaceReuse = reuse == 0;
		}
		ImGui::SetItemTooltip("Where reservoirs are reused from.");
		ImGui::Checkbox("Clamp Jacobian", &_settings.clampJacobian);
		ImGui::SetItemTooltip("Cap the reconnection Jacobian at %.0f, as the example used to.\nNot needed for stability (the pairwise MIS weights bound it);\nit darkens where samples are reused across large changes in\ndistance or angle, such as near corners.", kJacobianClamp);
		ImGui::SameLine();
		ImGui::Checkbox("Clamp outliers", &_settings.clampOutliers);
		ImGui::SetItemTooltip("Cap each pixel's estimate at %.0f before accumulation, as the\nexample always did: fewer fireflies, at the cost of losing some\nlight (biased).", kOutlierClamp);

		ImGui::Combo("View (V)", &_settings.debugView, kDebugViewNames, int(DebugView::Count));
		ImGui::SetItemTooltip("%s", kDebugViewHelp[_settings.debugView]);

		ImGui::SeparatorText("Exposure");
		ImGui::Combo("Tone mapper (T)", &_settings.tonemapper, kTonemapperNames, int(Tonemapper::Count));
		ImGui::SetItemTooltip("How scene radiance is compressed into the display's range.");
		ImGui::SliderFloat("Exposure", &_settings.exposureEv, -8.0f, 8.0f, "%+.1f EV", sliderFlags);
		ImGui::SetItemTooltip(_settings.autoExposure
			? "Compensation on top of auto exposure, in stops."
			: "In stops: each one doubles the brightness.");
		ImGui::Checkbox("Auto exposure", &_settings.autoExposure);
		ImGui::SetItemTooltip("Bring the log-average luminance of the image to middle grey,\neasing in over time.");
		if (_settings.autoExposure)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##adaptationSpeed", &_settings.adaptationSpeed, 0.1f, 10.0f, "adapt %.1f/s"
				, sliderFlags | ImGuiSliderFlags_Logarithmic);
			ImGui::SetItemTooltip("How fast the exposure follows the scene.");
		}
	}

	if (ImGui::CollapsingHeader("Reference"))
	{
		if (ImGui::Button(_input.m_referenceAccumulating ? "Stop accumulating" : "Accumulate reference"))
		{
			output.m_toggleReference = true;
		}
		ImGui::SetItemTooltip("Average frames of this view with every biased shortcut off (no\nreuse, denoiser, TAA or clamps; the camera still jitters): the\nunbiased image of the same rays. Moving the camera or the light\nstarts it again. Stop it to compare the live image against it.");
		ImGui::SameLine();
		ImGui::BeginDisabled(_input.m_referenceFrames == 0);
		if (ImGui::Button("Clear"))
		{
			output.m_clearReference = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Text("%u frames", _input.m_referenceFrames);

		ImGui::BeginDisabled(_input.m_referenceFrames == 0);
		ImGui::RadioButton("Image", &_settings.referenceView, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Reference", &_settings.referenceView, 1);
		ImGui::SameLine();
		ImGui::RadioButton("Error", &_settings.referenceView, 2);
		ImGui::SetItemTooltip("Relative error in luminance against the reference: green none,\nblue darker and red brighter, saturating at 50%%.");
		ImGui::EndDisabled();

		const Renderer::ReferenceMetrics& metrics = _input.m_referenceMetrics;
		if (_input.m_referenceAccumulating)
		{
			ImGui::TextDisabled("Accumulating; stop to compare.");
		}
		else if (metrics.m_valid)
		{
			ImGui::Text("Relative MSE %.5f   bias %+.2f%%", metrics.m_relativeMse, 100.0f * metrics.m_bias);
			ImGui::SetItemTooltip("Of the displayed image (after TAA, before tone mapping) against\nthe reference, in luminance, over the pixels that show the scene.\nBias: mean brightness relative to the reference's.");
		}
		else if (_input.m_referenceFrames > 0)
		{
			ImGui::TextDisabled("Measuring...");
		}
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
		ImGui::SliderInt("Temporal taps", &_settings.temporalTaps, 0, 4, "%d", sliderFlags);
		ImGui::SetItemTooltip("Reservoirs reused from last frame (0: no temporal reuse).");
		ImGui::SliderInt("Spatial taps", &_settings.spatialTaps, 0, 8, "%d", sliderFlags);
		ImGui::SetItemTooltip("Reservoirs reused from this frame, each with a visibility ray\n(0: no spatial reuse; 0 and 0 give plain, unbiased Monte Carlo).");
		ImGui::SliderInt("DI candidates", &_settings.diCandidates, 1, 32, "%d", sliderFlags);
		ImGui::SetItemTooltip("Fresh light samples per pixel per frame in ReSTIR DI.");
		ImGui::SliderInt("DI spatial taps", &_settings.diSpatialTaps, 0, 8, "%d", sliderFlags);
		ImGui::SetItemTooltip("Neighbouring reservoirs reused in ReSTIR DI, each with a shadow ray.");
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
			_settings.diCandidates = defaults.diCandidates;
			_settings.diSpatialTaps = defaults.diSpatialTaps;
			_settings.vsync = defaults.vsync;
		}
	}

	ImGui::End();
	return output;
}
