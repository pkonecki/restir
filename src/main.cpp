// ReSTIR GI example: the application. It owns the window, input and the list
// of models, and hands each frame to the renderer (renderer.cpp). The glTF
// loader is in scene.cpp, the fly camera in camera.cpp and the settings
// window in ui.cpp.

#include "camera.h"
#include "renderer.h"
#include "scene.h"
#include "settings.h"
#include "ui.h"

#include "common.h"
#include "bgfx_utils.h"
#include "entry/input.h"
#include "imgui/imgui.h"

#include <bx/commandline.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
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
};

constexpr BundledModel kBundledModels[] =
{
	{ "assets/cornell_box/scene.gltf", kInitialCameraDistance, kInitialCameraYaw },
	{ "assets/sponza/Sponza.gltf",     0.45f,                  bx::kPiHalf },
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
		bgfx::init(init);

		bgfx::setDebug(m_debug);
		imguiCreate(15.0f);

		m_renderer.init(m_settings);
		m_appliedSettings = m_settings;

		// The first model must load; a failure while switching later only
		// keeps the current one.
		setModel(0, loadScene(m_modelPaths[0]));
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

		m_camera.update(m_mouseState, m_scene.m_radius);
		updateKeyboardShortcuts();
		m_stats.update();

		const uint32_t giDivisor = std::max(m_renderer.giDivisor(), 1u);
		const SettingsWindowInput windowInput =
		{
			m_modelPaths, m_modelIndex, m_modelError, m_stats,
			std::max(1u, m_width / giDivisor), std::max(1u, m_height / giDivisor),
		};
		const SettingsWindowOutput windowOutput = drawSettingsWindow(windowInput, m_settings, m_camera);
		if (windowOutput.m_switchModel)
		{
			switchModel(*windowOutput.m_switchModel);
		}
		applySettingChanges();

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
		m_renderer.render(frame, m_settings);

		bgfx::setMarker("Present");
		bgfx::frame();
		return true;
	}

private:
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

		float cameraDistance = kInitialCameraDistance;
		float cameraYaw = kInitialCameraYaw;
		for (const BundledModel& bundled : kBundledModels)
		{
			if (m_modelPaths[_index] == bundled.m_path)
			{
				cameraDistance = bundled.m_cameraDistance;
				cameraYaw = bundled.m_cameraYaw;
			}
		}
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
};

} // namespace

ENTRY_IMPLEMENT_MAIN(
	  ExampleRestirGi
	, "restir-gi"
	, "ReSTIR global illumination starter."
	, "https://github.com/bkaradzic/bgfx"
	);
