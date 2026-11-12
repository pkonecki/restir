#pragma once

// Camera bookmarks: saved views, per model, kept in camera_bookmarks.txt in
// the working directory so they survive restarts. Keys 1 to 9 recall them.
// A pose also reads and prints as "x,y,z,yaw,pitch", the form the --camera
// command line option takes.

#include <bx/math.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

struct CameraPose
{
	bx::Vec3 m_eye = { 0.0f, 0.0f, 0.0f };
	float m_yaw = 0.0f;
	float m_pitch = 0.0f;
};

constexpr std::size_t kMaxBookmarks = 9;

// "x,y,z,yaw,pitch", with enough digits to restore the view exactly.
std::string formatPose(const CameraPose& _pose);
std::optional<CameraPose> parsePose(const std::string& _text);

class CameraBookmarks
{
public:
	explicit CameraBookmarks(std::string _filePath = "camera_bookmarks.txt");

	// The model's bookmarks, in the order they were added.
	const std::vector<CameraPose>& get(const std::string& _model) const;

	// False when the model already has kMaxBookmarks.
	bool add(const std::string& _model, const CameraPose& _pose);
	void remove(const std::string& _model, std::size_t _index);

	// Why the file could not be written, if it could not.
	const std::string& error() const { return m_error; }

private:
	void load();
	void save();

	std::string m_filePath;
	std::map<std::string, std::vector<CameraPose>> m_bookmarks;
	std::string m_error;
};
