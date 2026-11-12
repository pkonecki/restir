#include "bvh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{

struct Bounds
{
	bx::Vec3 m_min = {  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max() };
	bx::Vec3 m_max = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

	void grow(const bx::Vec3& _point)
	{
		m_min = bx::min(m_min, _point);
		m_max = bx::max(m_max, _point);
	}

	void grow(const Bounds& _other)
	{
		m_min = bx::min(m_min, _other.m_min);
		m_max = bx::max(m_max, _other.m_max);
	}

	bool empty() const
	{
		return m_min.x > m_max.x;
	}

	float area() const
	{
		if (empty())
		{
			return 0.0f;
		}
		const bx::Vec3 size = bx::sub(m_max, m_min);
		return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
	}
};

float component(const bx::Vec3& _vector, int _axis)
{
	return _axis == 0 ? _vector.x : _axis == 1 ? _vector.y : _vector.z;
}

struct BuildNode
{
	Bounds m_bounds;
	uint32_t m_first = 0; // first triangle (leaf) or first child (interior)
	uint32_t m_count = 0; // triangles; 0 for an interior node
};

constexpr int kBins = 12;
constexpr uint32_t kMaxLeafSize = 4;     // leaves split above this, when the SAH allows
constexpr uint32_t kForcedLeafSize = 16; // and are always split above this (until the depth limit)

class Builder
{
public:
	Builder(const std::vector<Bounds>& _triangleBounds, const std::vector<bx::Vec3>& _centroids)
		: m_triangleBounds(_triangleBounds)
		, m_centroids(_centroids)
	{
		m_order.resize(_centroids.size());
		for (uint32_t idx = 0; idx < m_order.size(); ++idx)
		{
			m_order[idx] = idx;
		}
	}

	void build()
	{
		m_nodes.reserve(2 * m_order.size());
		m_nodes.emplace_back();
		split(0, 0, uint32_t(m_order.size()), 1);
	}

	std::vector<BuildNode> m_nodes;
	std::vector<uint32_t> m_order; // triangle indices in leaf order
	uint32_t m_depth = 0;

private:
	void makeLeaf(uint32_t _node, uint32_t _begin, uint32_t _end, uint32_t _depth)
	{
		m_nodes[_node].m_first = _begin;
		m_nodes[_node].m_count = _end - _begin;
		m_depth = std::max(m_depth, _depth);
	}

	void split(uint32_t _node, uint32_t _begin, uint32_t _end, uint32_t _depth)
	{
		Bounds bounds;
		Bounds centroidBounds;
		for (uint32_t idx = _begin; idx < _end; ++idx)
		{
			bounds.grow(m_triangleBounds[m_order[idx]]);
			centroidBounds.grow(m_centroids[m_order[idx]]);
		}
		m_nodes[_node].m_bounds = bounds;

		const uint32_t count = _end - _begin;
		if (count <= 1 || _depth >= kBvhMaxDepth)
		{
			makeLeaf(_node, _begin, _end, _depth);
			return;
		}

		// Binned SAH: the best of kBins - 1 planes along each axis.
		float bestCost = std::numeric_limits<float>::max();
		int bestAxis = -1;
		int bestPlane = 0;
		for (int axis = 0; axis < 3; ++axis)
		{
			const float low = component(centroidBounds.m_min, axis);
			const float extent = component(centroidBounds.m_max, axis) - low;
			if (extent <= 0.0f)
			{
				continue;
			}

			std::array<Bounds, kBins> binBounds;
			std::array<uint32_t, kBins> binCounts = {};
			const float scale = float(kBins) / extent;
			for (uint32_t idx = _begin; idx < _end; ++idx)
			{
				const uint32_t triangle = m_order[idx];
				const int bin = std::min(int((component(m_centroids[triangle], axis) - low) * scale), kBins - 1);
				binBounds[bin].grow(m_triangleBounds[triangle]);
				++binCounts[bin];
			}

			// Sweep from the right, then from the left.
			std::array<float, kBins> rightArea = {};
			std::array<uint32_t, kBins> rightCount = {};
			Bounds right;
			uint32_t rightTotal = 0;
			for (int bin = kBins - 1; bin > 0; --bin)
			{
				right.grow(binBounds[bin]);
				rightTotal += binCounts[bin];
				rightArea[bin] = right.area();
				rightCount[bin] = rightTotal;
			}
			Bounds left;
			uint32_t leftTotal = 0;
			for (int plane = 1; plane < kBins; ++plane)
			{
				left.grow(binBounds[plane - 1]);
				leftTotal += binCounts[plane - 1];
				if (leftTotal == 0 || rightCount[plane] == 0)
				{
					continue;
				}
				const float cost = left.area() * float(leftTotal) + rightArea[plane] * float(rightCount[plane]);
				if (cost < bestCost)
				{
					bestCost = cost;
					bestAxis = axis;
					bestPlane = plane;
				}
			}
		}

		const float leafCost = bounds.area() * float(count);
		uint32_t middle = _begin;
		if (bestAxis >= 0 && (bestCost < leafCost || count > kMaxLeafSize))
		{
			const float low = component(centroidBounds.m_min, bestAxis);
			const float scale = float(kBins) / (component(centroidBounds.m_max, bestAxis) - low);
			middle = uint32_t(std::partition(m_order.begin() + _begin, m_order.begin() + _end, [&](uint32_t _triangle)
			{
				return std::min(int((component(m_centroids[_triangle], bestAxis) - low) * scale), kBins - 1) < bestPlane;
			}) - m_order.begin());
		}
		else if (count > kForcedLeafSize)
		{
			// All centroids in one spot: split the list in half.
			middle = _begin + count / 2;
		}
		else
		{
			makeLeaf(_node, _begin, _end, _depth);
			return;
		}

		const uint32_t children = uint32_t(m_nodes.size());
		m_nodes.emplace_back();
		m_nodes.emplace_back();
		m_nodes[_node].m_first = children;
		m_nodes[_node].m_count = 0;
		split(children, _begin, middle, _depth + 1);
		split(children + 1, middle, _end, _depth + 1);
	}

	const std::vector<Bounds>& m_triangleBounds;
	const std::vector<bx::Vec3>& m_centroids;
};

// The average colour of an image: its 1x1 mip, the last texel of the chain.
// Colour images are decoded from the same 2.2 gamma the shaders use.
std::array<float, 4> averageColor(const SceneMesh& _mesh, int32_t _image, bool _gamma)
{
	if (_image < 0 || std::size_t(_image) >= _mesh.m_images.size() || _mesh.m_images[_image].m_data.size() < 4)
	{
		return { 1.0f, 1.0f, 1.0f, 1.0f };
	}

	const uint8_t* texel = _mesh.m_images[_image].m_data.data() + _mesh.m_images[_image].m_data.size() - 4;
	std::array<float, 4> color;
	for (int channel = 0; channel < 4; ++channel)
	{
		const float value = float(texel[channel]) / 255.0f;
		color[channel] = _gamma && channel < 3 ? std::pow(value, 2.2f) : value;
	}
	return color;
}

} // namespace

SceneBvh buildBvh(const SceneMesh& _mesh)
{
	SceneBvh bvh;
	const std::size_t triangleCount = _mesh.m_indices.size() / 3;
	if (triangleCount == 0)
	{
		return bvh;
	}

	const auto vertex = [&_mesh](uint32_t _index)
	{
		const float* p = _mesh.m_vertices[_index].m_position;
		return bx::Vec3{ p[0], p[1], p[2] };
	};

	std::vector<Bounds> triangleBounds(triangleCount);
	std::vector<bx::Vec3> centroids(triangleCount, bx::Vec3{ 0.0f, 0.0f, 0.0f });
	for (std::size_t triangle = 0; triangle < triangleCount; ++triangle)
	{
		Bounds bounds;
		for (int corner = 0; corner < 3; ++corner)
		{
			bounds.grow(vertex(_mesh.m_indices[triangle * 3 + corner]));
		}
		triangleBounds[triangle] = bounds;
		centroids[triangle] = bx::mul(bx::add(bounds.m_min, bounds.m_max), 0.5f);
	}

	// The material of each triangle, from the per-material draw ranges.
	std::vector<uint32_t> triangleMaterial(triangleCount, 0);
	for (const SceneDraw& draw : _mesh.m_draws)
	{
		for (uint32_t index = draw.m_firstIndex; index < draw.m_firstIndex + draw.m_indexCount; index += 3)
		{
			triangleMaterial[index / 3] = draw.m_material;
		}
	}

	// Per material: the average base colour and metallic of its textures, and
	// its emission (factor times the average emissive texture).
	const std::size_t materialCount = _mesh.m_materials.size();
	std::vector<std::array<float, 4>> materialBase(materialCount);
	std::vector<float> materialMetallic(materialCount);
	std::vector<std::array<float, 3>> materialEmission(materialCount);
	for (std::size_t idx = 0; idx < materialCount; ++idx)
	{
		const SceneMaterial& material = _mesh.m_materials[idx];
		materialBase[idx] = averageColor(_mesh, material.m_baseColorImage, true);
		materialMetallic[idx] = material.m_metallicTextureMissing
			? 0.0f
			: averageColor(_mesh, material.m_metallicRoughnessImage, false)[2];
		const std::array<float, 4> emissive = averageColor(_mesh, material.m_emissiveImage, true);
		for (int channel = 0; channel < 3; ++channel)
		{
			materialEmission[idx][channel] = material.m_emissive[channel] * emissive[channel];
		}
	}

	Builder builder(triangleBounds, centroids);
	builder.build();

	bvh.m_nodeCount = uint32_t(builder.m_nodes.size());
	bvh.m_triangleCount = uint32_t(triangleCount);
	bvh.m_triangleOffset = 2 * bvh.m_nodeCount;
	bvh.m_materialOffset = bvh.m_triangleOffset + 4 * bvh.m_triangleCount;
	bvh.m_depth = builder.m_depth;
	bvh.m_data.resize(std::size_t(bvh.m_materialOffset + materialCount) * 4, 0.0f);

	float* out = bvh.m_data.data();
	for (const BuildNode& node : builder.m_nodes)
	{
		const float values[8] =
		{
			node.m_bounds.m_min.x, node.m_bounds.m_min.y, node.m_bounds.m_min.z, float(node.m_first),
			node.m_bounds.m_max.x, node.m_bounds.m_max.y, node.m_bounds.m_max.z, float(node.m_count),
		};
		std::memcpy(out, values, sizeof(values));
		out += 8;
	}

	for (const uint32_t triangle : builder.m_order)
	{
		const uint32_t material = triangleMaterial[triangle];
		float baseColor[3] = { 0.0f, 0.0f, 0.0f };
		float metallic = 0.0f;
		for (int corner = 0; corner < 3; ++corner)
		{
			const SceneVertex& source = _mesh.m_vertices[_mesh.m_indices[triangle * 3 + corner]];
			const bx::Vec3 position = vertex(_mesh.m_indices[triangle * 3 + corner]);
			out[corner * 4 + 0] = position.x;
			out[corner * 4 + 1] = position.y;
			out[corner * 4 + 2] = position.z;
			for (int channel = 0; channel < 3; ++channel)
			{
				baseColor[channel] += source.m_color[channel] / 3.0f;
			}
			metallic += source.m_material[1] / 3.0f;
		}
		out[3] = float(material);

		// The diffuse albedo, as diffuseAlbedo() in shaders/restir.sh derives
		// it from the G-buffer.
		metallic *= materialMetallic[material];
		for (int channel = 0; channel < 3; ++channel)
		{
			out[12 + channel] = baseColor[channel] * materialBase[material][channel] * (1.0f - metallic);
		}
		out += 16;
	}

	for (std::size_t idx = 0; idx < materialCount; ++idx)
	{
		out[0] = materialEmission[idx][0];
		out[1] = materialEmission[idx][1];
		out[2] = materialEmission[idx][2];
		out += 4;
	}

	return bvh;
}
