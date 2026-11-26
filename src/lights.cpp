#include "lights.h"

#include <cmath>
#include <cstring>

namespace
{

float luminance(const float* _color)
{
	return 0.2126f * _color[0] + 0.7152f * _color[1] + 0.0722f * _color[2];
}

// A small deterministic generator, so the extra lights stay put for a seed.
struct Random
{
	uint32_t m_state;

	float next()
	{
		m_state = m_state * 747796405u + 2891336453u;
		uint32_t word = ((m_state >> ((m_state >> 28u) + 4u)) ^ m_state) * 277803737u;
		word = (word >> 22u) ^ word;
		return float(word >> 8) * (1.0f / 16777216.0f);
	}
};

// A saturated colour of the given hue (0-1).
void hueToRgb(float _hue, float* _rgb)
{
	for (int channel = 0; channel < 3; ++channel)
	{
		const float k = std::fmod(_hue * 6.0f + float(4 - 2 * channel) + 6.0f, 6.0f);
		_rgb[channel] = 1.0f - std::fmax(0.0f, std::fmin(std::fmin(k, 4.0f - k), 1.0f)) * 0.8f;
	}
}

} // namespace

LightList buildLightList(
	  const std::vector<EmissiveTriangle>& _emitters
	, const ExtraLights& _extra
	, const bx::Vec3& _boundsMin
	, const bx::Vec3& _boundsMax
	, float _intensityScale
	)
{
	LightList list;
	std::vector<float> power;

	const auto append = [&](const float* _v0, const float* _v1, const float* _v2, float _type, float _triangle
		, const float* _emission, float _power)
	{
		const float values[16] =
		{
			_v0[0], _v0[1], _v0[2], _type,
			_v1[0], _v1[1], _v1[2], _triangle,
			_v2[0], _v2[1], _v2[2], 0.0f,
			_emission[0], _emission[1], _emission[2], 0.0f,
		};
		list.m_data.insert(list.m_data.end(), values, values + 16);
		power.push_back(_power);
	};

	for (const EmissiveTriangle& emitter : _emitters)
	{
		const bx::Vec3 a = { emitter.m_vertices[0][0], emitter.m_vertices[0][1], emitter.m_vertices[0][2] };
		const bx::Vec3 b = { emitter.m_vertices[1][0], emitter.m_vertices[1][1], emitter.m_vertices[1][2] };
		const bx::Vec3 c = { emitter.m_vertices[2][0], emitter.m_vertices[2][1], emitter.m_vertices[2][2] };
		const float area = 0.5f * bx::length(bx::cross(bx::sub(b, a), bx::sub(c, a)));
		if (area <= 0.0f)
		{
			continue;
		}
		// Power of a Lambertian emitter: pi * radiance * area.
		append(emitter.m_vertices[0], emitter.m_vertices[1], emitter.m_vertices[2], 0.0f, float(emitter.m_triangle)
			, emitter.m_emission, bx::kPi * luminance(emitter.m_emission) * area);
	}
	list.m_triangles = uint32_t(power.size());

	// Extra point lights, scattered through the middle 80% of the bounds,
	// in saturated colours.
	Random random = { _extra.m_seed * 2654435761u + 1u };
	for (int idx = 0; idx < _extra.m_count; ++idx)
	{
		float position[3];
		const float low[3] = { _boundsMin.x, _boundsMin.y, _boundsMin.z };
		const float high[3] = { _boundsMax.x, _boundsMax.y, _boundsMax.z };
		for (int axis = 0; axis < 3; ++axis)
		{
			position[axis] = low[axis] + (high[axis] - low[axis]) * (0.1f + 0.8f * random.next());
		}
		float intensity[3];
		hueToRgb(random.next(), intensity);
		for (float& channel : intensity)
		{
			channel *= _extra.m_intensity * _intensityScale;
		}
		const float none[3] = { 0.0f, 0.0f, 0.0f };
		// Power of an isotropic point light: 4 pi * intensity.
		append(position, none, none, 1.0f, -1.0f, intensity, 4.0f * bx::kPi * luminance(intensity));
	}

	list.m_count = uint32_t(power.size());
	double total = 0.0;
	for (const float value : power)
	{
		total += value;
	}
	double cumulative = 0.0;
	for (uint32_t idx = 0; idx < list.m_count; ++idx)
	{
		const double pmf = total > 0.0 ? power[idx] / total : 1.0 / double(list.m_count);
		cumulative += pmf;
		list.m_data[idx * 16 + 11] = idx + 1 == list.m_count ? 1.0f : float(cumulative);
		list.m_data[idx * 16 + 15] = float(pmf);
	}
	return list;
}
