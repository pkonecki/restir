#include "camera.h"

#include "entry/entry.h"
#include "entry/input.h"
#include "imgui/imgui.h"

#include <bgfx/bgfx.h>
#include <bx/timer.h>

#include <algorithm>
#include <cmath>

namespace
{

constexpr float kCameraSpeed = 0.4f; // scene radii per second, before the wheel's scale
constexpr float kMouseSensitivity = 0.004f;

} // namespace

void FlyCamera::setPose(const bx::Vec3& _eye, float _yaw, float _pitch)
{
	m_eye = _eye;
	m_yaw = _yaw;
	m_pitch = std::clamp(_pitch, -1.5f, 1.5f);
}

void FlyCamera::lookAt(const bx::Vec3& _target, float _distance, float _yaw, float _pitch)
{
	m_yaw = _yaw;
	m_pitch = _pitch;
	m_eye = bx::sub(_target, bx::mul(forward(), _distance));
}

bx::Vec3 FlyCamera::forward() const
{
	const float cosPitch = std::cos(m_pitch);
	return
	{
		-cosPitch * std::sin(m_yaw),
		-std::sin(m_pitch),
		-cosPitch * std::cos(m_yaw),
	};
}

void FlyCamera::update(const entry::MouseState& _mouse, float _sceneRadius)
{
	const int64_t now = bx::getHPCounter();
	const float deltaTime = m_lastFrameTime == 0
		? 0.0f
		: std::min(float(double(now - m_lastFrameTime) / double(bx::getHPFrequency())), 0.1f);
	m_lastFrameTime = now;

	// The settings window keeps the mouse it is over, and the keyboard
	// while one of its widgets takes text.
	const ImGuiIO& io = ImGui::GetIO();
	const bool isDragging = _mouse.m_buttons[entry::MouseButton::Left] != 0 && !io.WantCaptureMouse;
	if (isDragging && m_isDragging)
	{
		const int32_t deltaX = _mouse.m_mx - m_lastMouseX;
		const int32_t deltaY = _mouse.m_my - m_lastMouseY;
		m_yaw -= float(deltaX) * kMouseSensitivity;
		m_pitch = std::clamp(m_pitch + float(deltaY) * kMouseSensitivity, -1.5f, 1.5f);
	}

	m_isDragging = isDragging;
	m_lastMouseX = _mouse.m_mx;
	m_lastMouseY = _mouse.m_my;

	const int32_t wheelDelta = _mouse.m_mz - m_lastMouseWheel;
	m_lastMouseWheel = _mouse.m_mz;
	if (wheelDelta != 0 && !io.WantCaptureMouse)
	{
		m_speedScale = std::clamp(m_speedScale * std::pow(1.25f, float(wheelDelta)), 0.05f, 20.0f);
	}

	if (io.WantCaptureKeyboard)
	{
		return;
	}

	const bx::Vec3 forwardDirection = forward();
	const bx::Vec3 right = bx::normalize(bx::cross(forwardDirection, { 0.0f, 1.0f, 0.0f }));
	const uint8_t modifiers = inputGetModifiersState();
	const auto axis = [](bool _positive, bool _negative)
	{
		return (_positive ? 1.0f : 0.0f) - (_negative ? 1.0f : 0.0f);
	};

	const float forwardAmount = axis(inputGetKeyState(entry::Key::KeyW), inputGetKeyState(entry::Key::KeyS));
	const float rightAmount = axis(inputGetKeyState(entry::Key::KeyD), inputGetKeyState(entry::Key::KeyA));
	const float upAmount = axis(
		  0 != (modifiers & (entry::Modifier::LeftShift | entry::Modifier::RightShift))
		, 0 != (modifiers & (entry::Modifier::LeftCtrl | entry::Modifier::RightCtrl))
		);

	bx::Vec3 move = bx::add(bx::mul(forwardDirection, forwardAmount), bx::mul(right, rightAmount));
	move = bx::add(move, { 0.0f, upAmount, 0.0f });
	const float moveLength = bx::length(move);
	if (moveLength > 0.0f)
	{
		const float speed = kCameraSpeed * _sceneRadius * m_speedScale;
		m_eye = bx::add(m_eye, bx::mul(move, speed * deltaTime / moveLength));
	}
}

CameraMatrices FlyCamera::matrices(float _aspect, const bx::Vec3& _sceneCenter, float _sceneRadius) const
{
	CameraMatrices matrices;
	const bx::Vec3 at = bx::add(m_eye, forward());
	// glTF is right-handed; a left-handed camera would mirror the scene.
	bx::mtxLookAt(matrices.m_view, m_eye, at, { 0.0f, 1.0f, 0.0f }, bx::Handedness::Right);
	const float farPlane = bx::length(bx::sub(m_eye, _sceneCenter)) + 2.0f * _sceneRadius;
	bx::mtxProj(
		  matrices.m_projection
		, kCameraFovY
		, _aspect
		, 0.05f * _sceneRadius
		, std::max(farPlane, 2.5f * _sceneRadius)
		, bgfx::getCaps()->homogeneousDepth
		, bx::Handedness::Right
		);
	return matrices;
}
