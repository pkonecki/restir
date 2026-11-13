#include "share.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <vector>

namespace
{

// One shared value: how to write it and how to read it back.
struct Field
{
	const char* m_key;
	std::function<std::string()> m_write;
	std::function<bool(const std::string&)> m_read;
};

std::string formatFloat(float _value)
{
	char text[32];
	std::snprintf(text, sizeof(text), "%.6g", _value);
	return text;
}

bool parseFloats(const std::string& _text, float* _values, int _count)
{
	std::stringstream stream(_text);
	std::string part;
	for (int idx = 0; idx < _count; ++idx)
	{
		if (!std::getline(stream, part, ','))
		{
			return false;
		}
		char* end = nullptr;
		_values[idx] = std::strtof(part.c_str(), &end);
		if (end == part.c_str())
		{
			return false;
		}
	}
	return true;
}

Field boolField(const char* _key, bool& _value)
{
	return { _key
		, [&_value]() { return std::string(_value ? "1" : "0"); }
		, [&_value](const std::string& _text) { _value = _text != "0"; return true; } };
}

Field intField(const char* _key, int& _value, int _min, int _max)
{
	return { _key
		, [&_value]() { return std::to_string(_value); }
		, [&_value, _min, _max](const std::string& _text)
		{
			char* end = nullptr;
			const long value = std::strtol(_text.c_str(), &end, 10);
			if (end == _text.c_str() || value < _min || value > _max)
			{
				return false;
			}
			_value = int(value);
			return true;
		} };
}

Field floatField(const char* _key, float& _value)
{
	return { _key
		, [&_value]() { return formatFloat(_value); }
		, [&_value](const std::string& _text) { return parseFloats(_text, &_value, 1); } };
}

Field colorField(const char* _key, float* _value)
{
	return { _key
		, [_value]() { return formatFloat(_value[0]) + "," + formatFloat(_value[1]) + "," + formatFloat(_value[2]); }
		, [_value](const std::string& _text) { return parseFloats(_text, _value, 3); } };
}

std::vector<Field> fields(Settings& _settings)
{
	LightSettings& light = _settings.light;
	SkySettings& sky = _settings.sky;
	return
	{
		colorField("lp", light.position),
		colorField("lc", light.color),
		floatField("li", light.intensity),
		boolField("lo", light.orbit),
		floatField("lr", light.orbitRadius),
		floatField("ls", light.orbitSpeed),
		intField("xn", light.extraLights, 0, 1024),
		floatField("xi", light.extraIntensity),
		intField("xs", light.extraSeed, 0, 1 << 30),
		boolField("sky", sky.enabled),
		colorField("sz", sky.zenith),
		colorField("sh", sky.horizon),
		colorField("sg", sky.ground),
		floatField("si", sky.intensity),
		boolField("mb", _settings.multipleBounces),
		boolField("shadows", _settings.shadows),
		boolField("dn", _settings.denoiser),
		boolField("taa", _settings.taa),
		boolField("spec", _settings.specularGi),
		boolField("em", _settings.emissiveLighting),
		intField("dl", _settings.directLighting, 0, int(DirectLighting::Count) - 1),
		intField("rays", _settings.rayMode, 0, int(RayMode::Count) - 1),
		boolField("grid", _settings.worldSpaceReuse),
		boolField("cj", _settings.clampJacobian),
		boolField("co", _settings.clampOutliers),
		boolField("bf", _settings.boilingFilter),
		intField("view", _settings.debugView, 0, int(DebugView::Count) - 1),
		intField("tm", _settings.tonemapper, 0, int(Tonemapper::Count) - 1),
		floatField("ev", _settings.exposureEv),
		boolField("ae", _settings.autoExposure),
		floatField("as", _settings.adaptationSpeed),
		intField("gi", _settings.giResolution, 0, int(BX_COUNTOF(kGiResolutions)) - 1),
		intField("sm", _settings.shadowFaceSize, 0, int(BX_COUNTOF(kShadowFaceSizes)) - 1),
		intField("at", _settings.atrousIterations, 1, kMaxAtrousIterations),
		intField("rpp", _settings.initialCandidates, 1, 8),
		intField("tt", _settings.temporalTaps, 0, 4),
		intField("st", _settings.spatialTaps, 0, 8),
		intField("steps", _settings.traceSteps, 8, 64),
		intField("dic", _settings.diCandidates, 1, 32),
		intField("dis", _settings.diSpatialTaps, 0, 8),
	};
}

} // namespace

std::string encodeShareString(const Settings& _settings, const CameraPose& _pose)
{
	// The fields only read through these references when writing.
	Settings copy = _settings;
	std::string text = "cam=" + formatPose(_pose);
	for (const Field& field : fields(copy))
	{
		text += "&";
		text += field.m_key;
		text += "=";
		text += field.m_write();
	}
	return text;
}

bool decodeShareString(const std::string& _text, Settings& _settings, CameraPose& _pose, bool& _hasPose)
{
	std::string text = _text;
	if (!text.empty() && (text[0] == '#' || text[0] == '?'))
	{
		text.erase(0, 1);
	}

	std::vector<Field> known = fields(_settings);
	bool usable = false;
	_hasPose = false;
	std::stringstream stream(text);
	std::string pair;
	while (std::getline(stream, pair, '&'))
	{
		const std::size_t equals = pair.find('=');
		if (equals == std::string::npos)
		{
			continue;
		}
		const std::string key = pair.substr(0, equals);
		const std::string value = pair.substr(equals + 1);
		if (key == "cam")
		{
			if (const std::optional<CameraPose> pose = parsePose(value))
			{
				_pose = *pose;
				_hasPose = true;
				usable = true;
			}
			continue;
		}
		for (const Field& field : known)
		{
			if (key == field.m_key && field.m_read(value))
			{
				usable = true;
			}
		}
	}
	return usable;
}
