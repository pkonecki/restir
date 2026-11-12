#pragma once

// Fly camera: dragging with the left button turns it, W/S move along the
// view direction, A/D sideways, Shift/Ctrl straight up and down. The wheel
// scales the speed, which is otherwise relative to the scene size.

#include <bx/math.h>

#include <cstdint>

namespace entry { struct MouseState; }

constexpr float kCameraFovY = 48.0f;

struct CameraMatrices
{
	float m_view[16];
	float m_projection[16];
};

class FlyCamera
{
public:
	// Positive pitch looks down; yaw 0 looks along -z.
	void setPose(const bx::Vec3& _eye, float _yaw, float _pitch);

	// Places the camera _distance away from _target, at the given angles,
	// looking at it.
	void lookAt(const bx::Vec3& _target, float _distance, float _yaw, float _pitch);

	// Applies this frame's mouse and keyboard input. _sceneRadius sets the
	// speed. Input the settings window takes is ignored.
	void update(const entry::MouseState& _mouse, float _sceneRadius);

	// View and projection for a window of the given aspect. The far plane
	// reaches past the scene (centre and radius) from wherever the camera is.
	CameraMatrices matrices(float _aspect, const bx::Vec3& _sceneCenter, float _sceneRadius) const;

	bx::Vec3 forward() const;
	const bx::Vec3& eye() const { return m_eye; }
	float yaw() const { return m_yaw; }
	float pitch() const { return m_pitch; }

	float m_speedScale = 1.0f; // the wheel's factor, also set from the settings window

private:
	bx::Vec3 m_eye = { 0.0f, 0.0f, 0.0f };
	float m_yaw = 0.0f;
	float m_pitch = 0.0f;
	int64_t m_lastFrameTime = 0;
	int32_t m_lastMouseX = 0;
	int32_t m_lastMouseY = 0;
	int32_t m_lastMouseWheel = 0;
	bool m_isDragging = false;
};
