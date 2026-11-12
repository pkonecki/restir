// ReSTIR GI example: the application. It owns the window, input and the list
// of models, and hands each frame to the renderer (renderer.cpp). The glTF
// loader is in scene.cpp, the fly camera in camera.cpp and the settings
// window in ui.cpp.

#include "bookmarks.h"
#include "camera.h"
#include "capture.h"
#include "renderer.h"
#include "scene.h"
#include "settings.h"
#include "ui.h"

#include "common.h"
#include "bgfx_utils.h"
#include "entry/input.h"
#include "imgui/imgui.h"

#include <bx/commandline.h>
#include <bx/timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

// Camera start, in scene radii from the centre of the scene.
constexpr float kInitialCameraDistance = 3.17f; // outside the scene, for open models like the Cornell box
constexpr float kInitialCameraYaw = 0.64f;
constexpr float kInitialCameraPitch = 0.255f;

// Models offered by the M key when they are present, with where the camera
// starts. Sponza is closed all around, so the camera starts inside its
// courtyard, looking down its long (x) axis. It is not committed (see
// cmake/fetch_sponza.cmake), so it only shows up once fetched.
struct BundledModel
{
	const char* m_path;
	float m_cameraDistance; // in scene radii
	float m_cameraYaw;      // radians, 0 = camera on +z
	bool m_sky;             // whether the sky starts on (it does for other models)
};

constexpr BundledModel kBundledModels[] =
{
	// The Cornell box is a closed room lit from inside (its front is open, so
	// a sky would flood it); Sponza is an open courtyard.
	{ "assets/cornell_box/scene.gltf", kInitialCameraDistance, kInitialCameraYaw, false },
	{ "assets/sponza/Sponza.gltf",     0.45f,                  bx::kPiHalf,       true  },
};

class ExampleRestirGi : public entry::AppI
{
public:
	ExampleRestirGi(const char* _name, const char* _description, const char* _url)
		: entry::AppI(_name, _description, _url)
	{
	}

	void init(int32_t _argc, const char* const* _argv, uint32_t _width, uint32_t _height) override
	{
		Args args(_argc, _argv);
		bx::CommandLine commandLine(_argc, _argv);
		// M cycles through the models: the one passed with --model (if any),
		// then the bundled ones that are present.
		if (const char* modelOption = commandLine.findOption("model"))
		{
			m_modelPaths.push_back(modelOption);
		}
		for (const BundledModel& bundled : kBundledModels)
		{
			if (std::filesystem::exists(bundled.m_path)
			&&  std::find(m_modelPaths.begin(), m_modelPaths.end(), bundled.m_path) == m_modelPaths.end())
			{
				m_modelPaths.push_back(bundled.m_path);
			}
		}
		if (m_modelPaths.empty())
		{
			throw std::runtime_error("No model found: pass one with --model.");
		}

		// --camera x,y,z,yaw,pitch starts the first model from that view (the
		// settings window shows the current one in this form).
		std::optional<CameraPose> startPose;
		if (const char* cameraOption = commandLine.findOption("camera"))
		{
			startPose = parsePose(cameraOption);
			if (!startPose)
			{
				std::fprintf(stderr, "Ignoring --camera %s: expected x,y,z,yaw,pitch.\n", cameraOption);
			}
		}
		// --no-ui starts with the settings window hidden (F2 toggles it).
		m_showWindow = !commandLine.hasArg("no-ui");

		m_width  = _width;
		m_height = _height;
		// The profiler records the per-view GPU timings the settings window shows.
		m_debug  = BGFX_DEBUG_PROFILER;
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
		init.callback          = &m_capture;
		bgfx::init(init);

		bgfx::setDebug(m_debug);
		imguiCreate(15.0f);

		m_renderer.init(m_settings);
		m_appliedSettings = m_settings;

		// The first model must load; a failure while switching later only
		// keeps the current one.
		setModel(0, loadScene(m_modelPaths[0]));
		if (startPose)
		{
			setCameraPose(*startPose);
		}
	}

	int shutdown() override
	{
		m_renderer.shutdown();
		imguiDestroy();
		bgfx::shutdown();
		return 0;
	}

	bool update() override
	{
		if (entry::processEvents(m_width, m_height, m_debug, m_reset, &m_mouseState))
		{
			return false;
		}
		// F7 toggles vsync too.
		m_settings.vsync = 0 != (m_reset & BGFX_RESET_VSYNC);
		m_appliedSettings.vsync = m_settings.vsync;

		const uint8_t mouseButtons = 0
			| (m_mouseState.m_buttons[entry::MouseButton::Left]   ? IMGUI_MBUT_LEFT   : 0)
			| (m_mouseState.m_buttons[entry::MouseButton::Right]  ? IMGUI_MBUT_RIGHT  : 0)
			| (m_mouseState.m_buttons[entry::MouseButton::Middle] ? IMGUI_MBUT_MIDDLE : 0)
			;
		imguiBeginFrame(m_mouseState.m_mx, m_mouseState.m_my, mouseButtons, m_mouseState.m_mz
			, uint16_t(m_width), uint16_t(m_height), -1, kUiView);

		const int64_t now = bx::getHPCounter();
		m_deltaTime = m_lastFrameTime == 0
			? 0.0f
			: std::min(float(double(now - m_lastFrameTime) / double(bx::getHPFrequency())), 0.1f);
		m_lastFrameTime = now;

		m_camera.update(m_mouseState, m_scene.m_radius);
		updateKeyboardShortcuts();
		m_stats.update();

		// A screenshot asked for last frame is taken this frame, without the
		// window if so chosen.
		const bool takeScreenshot = m_screenshotRequested;
		m_screenshotRequested = false;
		if (m_showWindow && !(takeScreenshot && m_settings.hideWindowInScreenshots))
		{
			drawWindow();
		}
		applySettingChanges();
		updateLight();

		imguiEndFrame();

		if (m_width == 0 || m_height == 0)
		{
			bgfx::frame();
			return true;
		}

		FrameInput frame;
		frame.m_width = m_width;
		frame.m_height = m_height;
		frame.m_camera = m_camera.matrices(float(m_width) / float(m_height), m_scene.m_center, m_scene.m_radius);
		frame.m_eye = m_camera.eye();
		frame.m_deltaTime = m_deltaTime;
		m_renderer.render(frame, m_settings);
		if (takeScreenshot)
		{
			bgfx::requestScreenShot(BGFX_INVALID_HANDLE, makeScreenshotPath().c_str());
		}

		bgfx::setMarker("Present");
		bgfx::frame();
		return true;
	}

private:
	void drawWindow()
	{
		const uint32_t giDivisor = std::max(m_renderer.giDivisor(), 1u);
		const std::string& model = m_modelPaths[m_modelIndex];
		const std::string screenshotResult = m_capture.lastResult();
		const SettingsWindowInput windowInput =
		{
			m_modelPaths, m_modelIndex, m_modelError, m_stats,
			std::max(1u, m_width / giDivisor), std::max(1u, m_height / giDivisor),
			m_scene.m_radius, m_scene.m_lightPosition,
			m_bookmarks.get(model), m_bookmarks.error(), cameraPose(), screenshotResult,
			m_renderer.bvhStats(), m_renderer.lightCount(), m_renderer.emissiveTriangleCount(),
		};
		const SettingsWindowOutput windowOutput = drawSettingsWindow(windowInput, m_settings, m_camera);
		if (windowOutput.m_switchModel)
		{
			switchModel(*windowOutput.m_switchModel);
		}
		if (windowOutput.m_moveLightToCamera)
		{
			moveLightToCamera();
		}
		if (windowOutput.m_screenshot)
		{
			m_screenshotRequested = true;
		}
		if (windowOutput.m_addBookmark)
		{
			m_bookmarks.add(model, cameraPose());
		}
		if (windowOutput.m_goToBookmark)
		{
			goToBookmark(*windowOutput.m_goToBookmark);
		}
		if (windowOutput.m_removeBookmark)
		{
			m_bookmarks.remove(model, *windowOutput.m_removeBookmark);
		}
	}

	CameraPose cameraPose() const
	{
		CameraPose pose;
		pose.m_eye = m_camera.eye();
		pose.m_yaw = m_camera.yaw();
		pose.m_pitch = m_camera.pitch();
		return pose;
	}

	void setCameraPose(const CameraPose& _pose)
	{
		m_camera.setPose(_pose.m_eye, _pose.m_yaw, _pose.m_pitch);
	}

	void goToBookmark(std::size_t _index)
	{
		const std::vector<CameraPose>& bookmarks = m_bookmarks.get(m_modelPaths[m_modelIndex]);
		if (_index < bookmarks.size())
		{
			setCameraPose(bookmarks[_index]);
		}
	}

	// True on the frame _key goes down; _wasDown remembers its state.
	static bool keyPressed(entry::Key::Enum _key, bool& _wasDown)
	{
		const bool down = inputGetKeyState(_key);
		const bool pressed = down && !_wasDown;
		_wasDown = down;
		return pressed;
	}

	// The keyboard shortcuts for the toggles in the settings window. They are
	// ignored while a widget takes text (Ctrl+click on a slider).
	void updateKeyboardShortcuts()
	{
		if (ImGui::GetIO().WantCaptureKeyboard)
		{
			return;
		}

		if (keyPressed(entry::Key::KeyB, m_bounceKeyDown))
		{
			m_settings.multipleBounces = !m_settings.multipleBounces;
		}
		if (keyPressed(entry::Key::KeyH, m_shadowKeyDown))
		{
			m_settings.shadows = !m_settings.shadows;
		}
		if (keyPressed(entry::Key::KeyN, m_denoiseKeyDown))
		{
			m_settings.denoiser = !m_settings.denoiser;
		}
		if (keyPressed(entry::Key::KeyG, m_gridKeyDown))
		{
			m_settings.worldSpaceReuse = !m_settings.worldSpaceReuse;
		}
		if (keyPressed(entry::Key::KeyM, m_modelKeyDown) && m_modelPaths.size() > 1)
		{
			switchModel((m_modelIndex + 1) % m_modelPaths.size());
		}
		if (keyPressed(entry::Key::KeyK, m_skyKeyDown))
		{
			m_settings.sky.enabled = !m_settings.sky.enabled;
		}
		if (keyPressed(entry::Key::KeyR, m_rayKeyDown))
		{
			m_settings.rayMode = (m_settings.rayMode + 1) % int(RayMode::Count);
		}
		if (keyPressed(entry::Key::KeyE, m_emissiveKeyDown))
		{
			m_settings.emissiveLighting = !m_settings.emissiveLighting;
		}
		if (keyPressed(entry::Key::KeyV, m_viewKeyDown))
		{
			m_settings.debugView = (m_settings.debugView + 1) % int(DebugView::Count);
		}
		if (keyPressed(entry::Key::KeyT, m_tonemapKeyDown))
		{
			m_settings.tonemapper = (m_settings.tonemapper + 1) % int(Tonemapper::Count);
		}
		if (keyPressed(entry::Key::KeyL, m_lightKeyDown))
		{
			moveLightToCamera();
		}
		if (keyPressed(entry::Key::KeyP, m_screenshotKeyDown)
		&&  0 == (inputGetModifiersState() & (entry::Modifier::LeftCtrl | entry::Modifier::RightCtrl)))
		{
			// (Ctrl+P is bgfx's own screenshot, taken with the window.)
			m_screenshotRequested = true;
		}
		if (keyPressed(entry::Key::F2, m_windowKeyDown))
		{
			m_showWindow = !m_showWindow;
		}
		for (std::size_t idx = 0; idx < kMaxBookmarks; ++idx)
		{
			if (keyPressed(entry::Key::Enum(entry::Key::Key1 + idx), m_bookmarkKeyDown[idx]))
			{
				goToBookmark(idx);
			}
		}
	}

	void moveLightToCamera()
	{
		const bx::Vec3 eye = m_camera.eye();
		m_settings.light.position[0] = eye.x;
		m_settings.light.position[1] = eye.y;
		m_settings.light.position[2] = eye.z;
	}

	// Moves the light along its orbit: a horizontal circle around the
	// vertical axis through the scene's centre, at the light's height.
	void updateLight()
	{
		LightSettings& light = m_settings.light;
		if (!light.orbit)
		{
			return;
		}

		m_orbitAngle = std::fmod(m_orbitAngle + light.orbitSpeed * m_deltaTime, 2.0f * bx::kPi);
		const float radius = light.orbitRadius * m_scene.m_radius;
		light.position[0] = m_scene.m_center.x + radius * std::sin(m_orbitAngle);
		light.position[2] = m_scene.m_center.z + radius * std::cos(m_orbitAngle);
	}

	// Reacts to settings that need more than the renderer reading them.
	void applySettingChanges()
	{
		// The history means something different with and without the
		// denoiser (filtered or not), so toggling it restarts accumulation.
		if (m_settings.denoiser != m_appliedSettings.denoiser)
		{
			m_renderer.invalidateHistory();
		}
		// entry::processEvents applies the changed reset flags on the next frame.
		if (m_settings.vsync != m_appliedSettings.vsync)
		{
			m_reset = m_settings.vsync ? (m_reset | BGFX_RESET_VSYNC) : (m_reset & ~BGFX_RESET_VSYNC);
		}
		m_appliedSettings = m_settings;
	}

	// Makes a loaded mesh the current model and places the camera.
	void setModel(std::size_t _index, const SceneMesh& _mesh)
	{
		m_scene = describeScene(_mesh);
		m_renderer.setScene(_mesh, m_scene);
		m_settings.light.position[0] = m_scene.m_lightPosition.x;
		m_settings.light.position[1] = m_scene.m_lightPosition.y;
		m_settings.light.position[2] = m_scene.m_lightPosition.z;

		float cameraDistance = kInitialCameraDistance;
		float cameraYaw = kInitialCameraYaw;
		bool sky = true;
		for (const BundledModel& bundled : kBundledModels)
		{
			if (m_modelPaths[_index] == bundled.m_path)
			{
				cameraDistance = bundled.m_cameraDistance;
				cameraYaw = bundled.m_cameraYaw;
				sky = bundled.m_sky;
			}
		}
		m_settings.sky.enabled = sky;
		// Start at that distance and angle from the centre of the scene,
		// looking at it.
		m_camera.lookAt(m_scene.m_center, cameraDistance * m_scene.m_radius, cameraYaw, kInitialCameraPitch);
		m_camera.m_speedScale = 1.0f;
		m_modelIndex = _index;
	}

	void switchModel(std::size_t _index)
	{
		try
		{
			setModel(_index, loadScene(m_modelPaths[_index]));
			m_modelError.clear();
		}
		catch (const std::exception& _error)
		{
			std::fprintf(stderr, "%s\n", _error.what());
			m_modelError = "Could not load " + m_modelPaths[_index] + " (see the log)";
		}
	}

	entry::MouseState m_mouseState;
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_debug = 0;
	uint32_t m_reset = 0;

	CaptureCallback m_capture; // must outlive bgfx
	Renderer m_renderer;
	Settings m_settings;
	Settings m_appliedSettings; // as of the last applySettingChanges
	PerformanceStats m_stats;
	FlyCamera m_camera;
	SceneInfo m_scene;

	// Models M cycles through, and the one shown.
	std::vector<std::string> m_modelPaths;
	std::size_t m_modelIndex = 0;
	std::string m_modelError;

	bool m_bounceKeyDown = false;
	bool m_shadowKeyDown = false;
	bool m_denoiseKeyDown = false;
	bool m_gridKeyDown = false;
	bool m_modelKeyDown = false;
	bool m_lightKeyDown = false;
	bool m_tonemapKeyDown = false;
	bool m_viewKeyDown = false;
	bool m_emissiveKeyDown = false;
	bool m_rayKeyDown = false;
	bool m_skyKeyDown = false;
	bool m_screenshotKeyDown = false;
	bool m_windowKeyDown = false;
	bool m_bookmarkKeyDown[kMaxBookmarks] = {};

	CameraBookmarks m_bookmarks;
	bool m_screenshotRequested = false;
	bool m_showWindow = true;

	int64_t m_lastFrameTime = 0;
	float m_deltaTime = 0.0f; // seconds since the last frame, at most 0.1
	float m_orbitAngle = 0.0f; // of the light, around the scene's vertical axis
};

} // namespace

ENTRY_IMPLEMENT_MAIN(
	  ExampleRestirGi
	, "restir-gi"
	, "ReSTIR global illumination starter."
	, "https://github.com/bkaradzic/bgfx"
	);
