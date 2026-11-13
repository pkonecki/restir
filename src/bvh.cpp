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
	bool m_leaf = false;
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
		m_nodes[_node].m_leaf = true;
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

// A texture's colour over a triangle: the texel at the centre of its
// texture coordinates, from the mip level whose texels are about the size of
// the triangle in the texture, so a large triangle gets the average of the
// area it covers. Colour images are decoded from the same 2.2 gamma the
// shaders use. White without a texture.
std::array<float, 4> triangleTexture(const SceneMesh& _mesh, int32_t _image, const float* _uv0, const float* _uv1, const float* _uv2, bool _gamma)
{
	if (_image < 0 || std::size_t(_image) >= _mesh.m_images.size() || _mesh.m_images[_image].m_data.empty())
	{
		return { 1.0f, 1.0f, 1.0f, 1.0f };
	}

	const SceneImage& image = _mesh.m_images[_image];
	const float uvArea = 0.5f * std::abs((_uv1[0] - _uv0[0]) * (_uv2[1] - _uv0[1]) - (_uv2[0] - _uv0[0]) * (_uv1[1] - _uv0[1]));
	const float texels = uvArea * float(image.m_width) * float(image.m_height);
	const int wantedLevel = texels > 1.0f ? int(0.5f * std::log2(texels)) : 0;

	// Walk the chain (each level halves, down to 1x1) to the wanted level.
	uint32_t width = image.m_width;
	uint32_t height = image.m_height;
	std::size_t offset = 0;
	for (int level = 0; level < wantedLevel && (width > 1 || height > 1); ++level)
	{
		offset += std::size_t(width) * height * 4;
		width = std::max(width / 2, 1u);
		height = std::max(height / 2, 1u);
	}

	// The centre, wrapped (glTF textures repeat by default).
	float u = (_uv0[0] + _uv1[0] + _uv2[0]) / 3.0f;
	float v = (_uv0[1] + _uv1[1] + _uv2[1]) / 3.0f;
	u -= std::floor(u);
	v -= std::floor(v);
	const uint32_t x = std::min(uint32_t(u * float(width)), width - 1);
	const uint32_t y = std::min(uint32_t(v * float(height)), height - 1);
	const uint8_t* texel = image.m_data.data() + offset + (std::size_t(y) * width + x) * 4;

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

	Builder builder(triangleBounds, centroids);
	builder.build();

	bvh.m_nodeCount = uint32_t(builder.m_nodes.size());
	bvh.m_triangleCount = uint32_t(triangleCount);
	bvh.m_triangleOffset = 2 * bvh.m_nodeCount;
	bvh.m_depth = builder.m_depth;
	bvh.m_data.resize(std::size_t(bvh.m_triangleOffset + 4 * bvh.m_triangleCount) * 4, 0.0f);

	// The nodes in depth-first order: a node's left child follows it, and the
	// node after its whole subtree is where a traversal goes once it is done
	// with (or misses) the node: its skip link. That lets the shaders
	// traverse without a stack (see shaders/bvh.sh).
	float* out = bvh.m_data.data();
	uint32_t written = 0;
	const auto emit = [&](const auto& _self, uint32_t _node) -> void
	{
		const BuildNode& node = builder.m_nodes[_node];
		float* values = out + std::size_t(written) * 8;
		++written;
		values[0] = node.m_bounds.m_min.x;
		values[1] = node.m_bounds.m_min.y;
		values[2] = node.m_bounds.m_min.z;
		values[4] = node.m_bounds.m_max.x;
		values[5] = node.m_bounds.m_max.y;
		values[6] = node.m_bounds.m_max.z;
		if (node.m_leaf)
		{
			values[3] = float(node.m_first);
			values[7] = float(node.m_count);
			return;
		}

		_self(_self, node.m_first);
		const uint32_t right = written;
		_self(_self, node.m_first + 1);
		values[3] = float(right);
		values[7] = -float(written + 1); // skip link, negative: an interior node
	};
	emit(emit, 0);
	out += std::size_t(written) * 8; // the triangles follow

	uint32_t bvhIndex = 0;
	for (const uint32_t triangle : builder.m_order)
	{
		const uint32_t material = triangleMaterial[triangle];
		const SceneMaterial& source = _mesh.m_materials[material];
		const SceneVertex* corners[3];
		float baseColor[3] = { 0.0f, 0.0f, 0.0f };
		float metallic = 0.0f;
		for (int corner = 0; corner < 3; ++corner)
		{
			corners[corner] = &_mesh.m_vertices[_mesh.m_indices[triangle * 3 + corner]];
			const bx::Vec3 position = vertex(_mesh.m_indices[triangle * 3 + corner]);
			out[corner * 4 + 0] = position.x;
			out[corner * 4 + 1] = position.y;
			out[corner * 4 + 2] = position.z;
			for (int channel = 0; channel < 3; ++channel)
			{
				baseColor[channel] += corners[corner]->m_color[channel] / 3.0f;
			}
			metallic += corners[corner]->m_material[1] / 3.0f;
		}
		out[3] = float(material);

		// The diffuse albedo, as diffuseAlbedo() in shaders/restir.sh derives
		// it from the G-buffer, and the emission, from the textures over this
		// triangle.
		const auto texture = [&](int32_t _image, bool _gamma)
		{
			return triangleTexture(_mesh, _image
				, corners[0]->m_texcoord, corners[1]->m_texcoord, corners[2]->m_texcoord, _gamma);
		};
		const std::array<float, 4> base = texture(source.m_baseColorImage, true);
		metallic *= source.m_metallicTextureMissing ? 0.0f : texture(source.m_metallicRoughnessImage, false)[2];
		const std::array<float, 4> emissive = texture(source.m_emissiveImage, true);
		float emission[3];
		for (int channel = 0; channel < 3; ++channel)
		{
			out[12 + channel] = baseColor[channel] * base[channel] * (1.0f - metallic);
			emission[channel] = source.m_emissive[channel] * emissive[channel];
		}
		out[7] = emission[0];
		out[11] = emission[1];
		out[15] = emission[2];

		if (emission[0] > 0.0f || emission[1] > 0.0f || emission[2] > 0.0f)
		{
			EmissiveTriangle emitter;
			emitter.m_triangle = bvhIndex;
			for (int corner = 0; corner < 3; ++corner)
			{
				std::memcpy(emitter.m_vertices[corner], corners[corner]->m_position, sizeof(emitter.m_vertices[corner]));
			}
			std::memcpy(emitter.m_emission, emission, sizeof(emitter.m_emission));
			bvh.m_emitters.push_back(emitter);
		}
		++bvhIndex;
		out += 16;
	}

	return bvh;
}
