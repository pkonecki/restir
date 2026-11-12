#pragma once

// bgfx callback that saves screenshots as PNG files (bgfx's default one
// writes TGA). Every screenshot request goes through it: the P key, bgfx's
// own Ctrl+P / Print Screen, and the --screenshot command line option. In the
// web build, the file is also handed to the browser as a download.

#include <bgfx/bgfx.h>

#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <string>

class CaptureCallback : public bgfx::CallbackI
{
public:
	~CaptureCallback() override = default;

	// The last file saved (or the error), for the settings window. bgfx calls
	// screenShot on its render thread, hence the lock.
	std::string lastResult() const;

	void fatal(const char* _filePath, uint16_t _line, bgfx::Fatal::Enum _code, const char* _str) override;
	void traceVargs(const char* _filePath, uint16_t _line, const char* _format, va_list _argList) override;
	void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
	void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
	void profilerEnd() override {}
	uint32_t cacheReadSize(uint64_t) override { return 0; }
	bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
	void cacheWrite(uint64_t, const void*, uint32_t) override {}
	void screenShot(const char* _filePath, uint32_t _width, uint32_t _height, uint32_t _pitch
		, bgfx::TextureFormat::Enum _format, const void* _data, uint32_t _size, bool _yflip) override;
	void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
	void captureEnd() override {}
	void captureFrame(const void*, uint32_t) override {}

private:
	mutable std::mutex m_mutex;
	std::string m_lastResult;
};

// A new screenshot path in screenshots/, named after the local time.
std::string makeScreenshotPath();
