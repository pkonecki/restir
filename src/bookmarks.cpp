#include "bookmarks.h"

#include <cstdio>
#include <fstream>
#include <sstream>

std::string formatPose(const CameraPose& _pose)
{
	char text[160];
	std::snprintf(text, sizeof(text), "%.9g,%.9g,%.9g,%.9g,%.9g"
		, _pose.m_eye.x, _pose.m_eye.y, _pose.m_eye.z, _pose.m_yaw, _pose.m_pitch);
	return text;
}

std::optional<CameraPose> parsePose(const std::string& _text)
{
	CameraPose pose;
	char trailing = 0;
	if (std::sscanf(_text.c_str(), " %f , %f , %f , %f , %f %c"
		, &pose.m_eye.x, &pose.m_eye.y, &pose.m_eye.z, &pose.m_yaw, &pose.m_pitch, &trailing) != 5)
	{
		return std::nullopt;
	}
	return pose;
}

CameraBookmarks::CameraBookmarks(std::string _filePath)
	: m_filePath(std::move(_filePath))
{
	load();
}

const std::vector<CameraPose>& CameraBookmarks::get(const std::string& _model) const
{
	static const std::vector<CameraPose> s_none;
	const auto found = m_bookmarks.find(_model);
	return found != m_bookmarks.end() ? found->second : s_none;
}

bool CameraBookmarks::add(const std::string& _model, const CameraPose& _pose)
{
	std::vector<CameraPose>& poses = m_bookmarks[_model];
	if (poses.size() >= kMaxBookmarks)
	{
		return false;
	}
	poses.push_back(_pose);
	save();
	return true;
}

void CameraBookmarks::remove(const std::string& _model, std::size_t _index)
{
	const auto found = m_bookmarks.find(_model);
	if (found == m_bookmarks.end() || _index >= found->second.size())
	{
		return;
	}
	found->second.erase(found->second.begin() + std::ptrdiff_t(_index));
	if (found->second.empty())
	{
		m_bookmarks.erase(found);
	}
	save();
}

// One bookmark per line: the model path, a tab, then the pose.
void CameraBookmarks::load()
{
	std::ifstream file(m_filePath);
	std::string line;
	while (std::getline(file, line))
	{
		const std::size_t tab = line.find('\t');
		if (tab == std::string::npos)
		{
			continue;
		}
		const std::optional<CameraPose> pose = parsePose(line.substr(tab + 1));
		std::vector<CameraPose>& poses = m_bookmarks[line.substr(0, tab)];
		if (pose && poses.size() < kMaxBookmarks)
		{
			poses.push_back(*pose);
		}
	}
}

void CameraBookmarks::save()
{
	std::ostringstream text;
	for (const auto& [model, poses] : m_bookmarks)
	{
		for (const CameraPose& pose : poses)
		{
			text << model << '\t' << formatPose(pose) << '\n';
		}
	}

	std::ofstream file(m_filePath, std::ios::trunc);
	m_error = file.write(text.str().data(), std::streamsize(text.str().size()))
		? std::string()
		: "Could not write " + m_filePath;
}
