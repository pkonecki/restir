#include "capture.h"

#include <bimg/bimg.h>
#include <bx/allocator.h>
#include <bx/debug.h>
#include <bx/file.h>
#include <bx/string.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

#if BX_PLATFORM_EMSCRIPTEN
#	include <emscripten.h>
#endif

namespace
{

// Hands a file to the browser as a download (web build only).
void offerDownload(const std::string& _name, const std::vector<uint8_t>& _bytes)
{
#if BX_PLATFORM_EMSCRIPTEN
	EM_ASM({
		const bytes = HEAPU8.slice($0, $0 + $1);
		const link = document.createElement('a');
		link.href = URL.createObjectURL(new Blob([bytes], { type: 'image/png' }));
		link.download = UTF8ToString($2);
		document.body.appendChild(link);
		link.click();
		setTimeout(() => { URL.revokeObjectURL(link.href); link.remove(); }, 1000);
	}, _bytes.data(), int(_bytes.size()), _name.c_str());
#else
	BX_UNUSED(_name, _bytes);
#endif
}

} // namespace

std::string CaptureCallback::lastResult() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_lastResult;
}

void CaptureCallback::fatal(const char* _filePath, uint16_t _line, bgfx::Fatal::Enum _code, const char* _str)
{
	std::fprintf(stderr, "%s (%d): BGFX FATAL 0x%08x: %s\n", _filePath, _line, unsigned(_code), _str);
	std::fflush(stderr); // before abort() discards it
#if BX_PLATFORM_EMSCRIPTEN
	// stderr does not always reach the page when bgfx fails inside a WebGPU
	// callback, so tell the console directly.
	EM_ASM({ console.error('BGFX FATAL: ' + UTF8ToString($0)); }, _str);
#endif
	if (bgfx::Fatal::DebugCheck == _code)
	{
		bx::debugBreak();
	}
	else
	{
		std::abort();
	}
}

void CaptureCallback::traceVargs(const char* _filePath, uint16_t _line, const char* _format, va_list _argList)
{
	char message[2048];
	const int32_t length = bx::snprintf(message, sizeof(message), "%s (%d): ", _filePath, _line);
	if (length >= 0 && length < int32_t(sizeof(message)))
	{
		bx::vsnprintf(message + length, sizeof(message) - length, _format, _argList);
	}
	bx::debugOutput(message);
}

void CaptureCallback::screenShot(
	  const char* _filePath
	, uint32_t _width
	, uint32_t _height
	, uint32_t _pitch
	, bgfx::TextureFormat::Enum _format
	, const void* _data
	, uint32_t _size
	, bool _yflip
	)
{
	BX_UNUSED(_size);
	std::filesystem::path path(_filePath);
	if (path.extension() != ".png")
	{
		path += ".png";
	}

	// Tightly packed RGBA8, opaque: the back buffer's alpha means nothing.
	std::vector<uint8_t> pixels(std::size_t(_width) * _height * 4);
	const uint8_t* source = static_cast<const uint8_t*>(_data);
	bool converted = true;
	if (_format == bgfx::TextureFormat::RGBA8 || _format == bgfx::TextureFormat::BGRA8)
	{
		const bool bgra = _format == bgfx::TextureFormat::BGRA8;
		for (uint32_t y = 0; y < _height; ++y)
		{
			const uint8_t* row = source + std::size_t(y) * _pitch;
			uint8_t* out = pixels.data() + std::size_t(y) * _width * 4;
			for (uint32_t x = 0; x < _width; ++x)
			{
				out[x * 4 + 0] = row[x * 4 + (bgra ? 2 : 0)];
				out[x * 4 + 1] = row[x * 4 + 1];
				out[x * 4 + 2] = row[x * 4 + (bgra ? 0 : 2)];
				out[x * 4 + 3] = 255;
			}
		}
	}
	else
	{
		bx::DefaultAllocator allocator;
		converted = bimg::imageConvert(&allocator, pixels.data(), bimg::TextureFormat::RGBA8
			, _data, bimg::TextureFormat::Enum(_format), _width, _height, 1);
		for (std::size_t idx = 3; idx < pixels.size(); idx += 4)
		{
			pixels[idx] = 255;
		}
	}

	std::string result;
	if (!converted)
	{
		result = "Screenshot failed: unsupported back buffer format.";
	}
	else
	{
		bx::DefaultAllocator allocator;
		bx::MemoryBlock block(&allocator);
		bx::MemoryWriter writer(&block);
		bx::Error error;
		const int32_t written = bimg::imageWritePng(&writer, _width, _height, _width * 4, pixels.data()
			, bimg::TextureFormat::RGBA8, _yflip, &error);
		const std::vector<uint8_t> png(
			  static_cast<const uint8_t*>(block.more(0))
			, static_cast<const uint8_t*>(block.more(0)) + (written > 0 ? written : 0));

		std::error_code directoryError;
		if (path.has_parent_path())
		{
			std::filesystem::create_directories(path.parent_path(), directoryError);
		}
		std::ofstream file(path, std::ios::binary);
		if (written > 0 && error.isOk() && file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size())))
		{
			result = "Saved " + path.generic_string();
			offerDownload(path.filename().string(), png);
		}
		else
		{
			result = "Could not write " + path.generic_string();
		}
	}

	std::fprintf(stderr, "%s\n", result.c_str());
	std::lock_guard<std::mutex> lock(m_mutex);
	m_lastResult = result;
}

std::string makeScreenshotPath()
{
	const std::time_t now = std::time(nullptr);
	char name[64];
	std::strftime(name, sizeof(name), "screenshots/restir-%Y%m%d-%H%M%S", std::localtime(&now));

	// Several in the same second get a suffix.
	std::string path = std::string(name) + ".png";
	for (int suffix = 2; std::filesystem::exists(path); ++suffix)
	{
		path = std::string(name) + "-" + std::to_string(suffix) + ".png";
	}
	return path;
}
