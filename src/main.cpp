#include "common.h"
#include "bgfx_utils.h"
#include "entry/input.h"

#include <bimg/decode.h>
#include <bx/commandline.h>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

struct SceneVertex
{
	float m_position[3];
	float m_normal[3];
	float m_color[3];
	float m_material[2]; // roughness, metallic
	float m_texcoord[2];

	static void init()
	{
		ms_layout
			.begin()
			.add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0,    3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.end();
	}

	static bgfx::VertexLayout ms_layout;
};

bgfx::VertexLayout SceneVertex::ms_layout;

struct FullscreenVertex
{
	float m_position[3];
	float m_texcoord[2];

	static void init()
	{
		ms_layout
			.begin()
			.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.end();
	}

	static bgfx::VertexLayout ms_layout;
};

bgfx::VertexLayout FullscreenVertex::ms_layout;

// A decoded RGBA8 texture with its full mip chain, down to 1x1, mip 0 first.
struct SceneImage
{
	uint16_t m_width = 0;
	uint16_t m_height = 0;
	std::vector<uint8_t> m_data;
};

struct SceneMaterial
{
	int32_t m_baseColorImage = -1;         // index into SceneMesh::m_images, -1: none
	int32_t m_metallicRoughnessImage = -1; // index into SceneMesh::m_images, -1: none
	float m_alphaCutoff = 0.0f;            // alpha-masked materials discard texels below it
	bool m_twoSidedMask = false;           // alpha-masked and double-sided, like foliage cards
	bool m_metallicTextureMissing = false; // has a metallic-roughness texture that could not be loaded
};

// A range of the index buffer drawn with one material.
struct SceneDraw
{
	uint32_t m_firstIndex = 0;
	uint32_t m_indexCount = 0;
	uint32_t m_material = 0;
};

struct SceneMesh
{
	std::vector<SceneVertex> m_vertices;
	std::vector<uint32_t> m_indices; // grouped by material, see m_draws
	std::vector<SceneDraw> m_draws;
	std::vector<SceneMaterial> m_materials; // the glTF materials, then a default one
	std::vector<SceneImage> m_images;

	// Indices of each material while primitives are appended.
	std::vector<std::vector<uint32_t>> m_materialIndices;

	// World-space bounds of all vertices.
	bx::Vec3 m_boundsMin = {  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max() };
	bx::Vec3 m_boundsMax = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

	// Area-weighted sums over emissive triangles, used to place the light.
	bx::Vec3 m_emissiveCentroid = { 0.0f, 0.0f, 0.0f };
	bx::Vec3 m_emissiveNormal = { 0.0f, 0.0f, 0.0f };
	float m_emissiveArea = 0.0f;
};

// What the renderer needs to know about the loaded scene.
struct SceneInfo
{
	bx::Vec3 m_center = { 0.0f, 0.0f, 0.0f };
	float m_radius = 1.0f;
	bx::Vec3 m_lightPosition = { 0.0f, 0.0f, 0.0f };
};

template<typename IndexType>
void appendIndices(
	  const fastgltf::Asset& _asset
	, const fastgltf::Accessor& _accessor
	, std::vector<uint32_t>& _indices
	)
{
	for (const IndexType index : fastgltf::iterateAccessor<IndexType>(_asset, _accessor))
	{
		_indices.push_back(uint32_t(index));
	}
}

void appendPrimitive(
	  const fastgltf::Asset& _asset
	, const fastgltf::Primitive& _primitive
	, const fastgltf::math::fmat4x4& _transform
	, SceneMesh& _mesh
	)
{
	if (_primitive.type != fastgltf::PrimitiveType::Triangles)
	{
		// Points, lines and strips have no place in a G-buffer of surfaces.
		std::fprintf(stderr, "Skipping a glTF primitive that is not a triangle list.\n");
		return;
	}

	const auto positionAttribute = _primitive.findAttribute("POSITION");
	if (positionAttribute == _primitive.attributes.end())
	{
		throw std::runtime_error("A glTF primitive has no POSITION attribute.");
	}

	std::vector<fastgltf::math::fvec3> positions;
	for (const auto& position : fastgltf::iterateAccessor<fastgltf::math::fvec3>(
		_asset, _asset.accessors[positionAttribute->accessorIndex]))
	{
		positions.push_back(position);
	}

	std::vector<uint32_t> indices;
	if (_primitive.indicesAccessor.has_value())
	{
		const auto& accessor = _asset.accessors[_primitive.indicesAccessor.value()];
		switch (accessor.componentType)
		{
		case fastgltf::ComponentType::UnsignedByte:
			appendIndices<uint8_t>(_asset, accessor, indices);
			break;
		case fastgltf::ComponentType::UnsignedShort:
			appendIndices<uint16_t>(_asset, accessor, indices);
			break;
		case fastgltf::ComponentType::UnsignedInt:
			appendIndices<uint32_t>(_asset, accessor, indices);
			break;
		default:
			throw std::runtime_error("A glTF index accessor has an invalid component type.");
		}
	}
	else
	{
		indices.resize(positions.size());
		for (uint32_t idx = 0; idx < indices.size(); ++idx)
		{
			indices[idx] = idx;
		}
	}

	if (indices.empty() || indices.size() % 3 != 0)
	{
		throw std::runtime_error("A glTF primitive has an invalid triangle index count.");
	}

	std::vector<fastgltf::math::fvec3> normals;
	const auto normalAttribute = _primitive.findAttribute("NORMAL");
	if (normalAttribute != _primitive.attributes.end())
	{
		for (const auto& normal : fastgltf::iterateAccessor<fastgltf::math::fvec3>(
			_asset, _asset.accessors[normalAttribute->accessorIndex]))
		{
			normals.push_back(normal);
		}

		if (normals.size() != positions.size())
		{
			throw std::runtime_error("A glTF primitive has mismatched POSITION and NORMAL counts.");
		}
	}
	else
	{
		normals.resize(positions.size(), fastgltf::math::fvec3(0.0f));
		for (std::size_t idx = 0; idx < indices.size(); idx += 3)
		{
			const uint32_t a = indices[idx];
			const uint32_t b = indices[idx + 1];
			const uint32_t c = indices[idx + 2];
			if (a >= positions.size() || b >= positions.size() || c >= positions.size())
			{
				throw std::runtime_error("A glTF primitive contains an out-of-range index.");
			}

			const auto faceNormal = fastgltf::math::cross(positions[b] - positions[a], positions[c] - positions[a]);
			normals[a] += faceNormal;
			normals[b] += faceNormal;
			normals[c] += faceNormal;
		}
	}

	const std::size_t baseVertex = _mesh.m_vertices.size();
	if (positions.size() > std::size_t(std::numeric_limits<uint32_t>::max()) - baseVertex)
	{
		throw std::runtime_error("The glTF scene exceeds the 32-bit index-buffer limit.");
	}

	const fastgltf::math::fmat3x3 normalTransform = fastgltf::math::transpose(
		fastgltf::math::inverse(fastgltf::math::fmat3x3(_transform)));

	// Every material's textures are read with TEXCOORD_0.
	std::vector<fastgltf::math::fvec2> texcoords(positions.size(), fastgltf::math::fvec2(0.0f));
	const auto texcoordAttribute = _primitive.findAttribute("TEXCOORD_0");
	if (texcoordAttribute != _primitive.attributes.end())
	{
		const fastgltf::Accessor& accessor = _asset.accessors[texcoordAttribute->accessorIndex];
		if (accessor.count != positions.size())
		{
			throw std::runtime_error("A glTF primitive has mismatched POSITION and TEXCOORD_0 counts.");
		}

		std::size_t idx = 0;
		for (const auto& texcoord : fastgltf::iterateAccessor<fastgltf::math::fvec2>(_asset, accessor))
		{
			texcoords[idx++] = texcoord;
		}
	}

	// glTF material defaults: white, fully rough, fully metallic, not emissive.
	fastgltf::math::fvec4 baseColor(1.0f);
	float roughness = 1.0f;
	float metallic = 1.0f;
	bool isEmissive = false;
	if (_primitive.materialIndex.has_value())
	{
		const fastgltf::Material& material = _asset.materials[_primitive.materialIndex.value()];
		baseColor = material.pbrData.baseColorFactor;
		roughness = float(material.pbrData.roughnessFactor);
		metallic = float(material.pbrData.metallicFactor);
		const float emissiveStrength = float(material.emissiveStrength)
			* std::max({ float(material.emissiveFactor[0]), float(material.emissiveFactor[1]), float(material.emissiveFactor[2]) });
		isEmissive = emissiveStrength > 0.0f;
	}

	for (std::size_t idx = 0; idx < positions.size(); ++idx)
	{
		const auto position = _transform * fastgltf::math::fvec4(
			positions[idx].x(), positions[idx].y(), positions[idx].z(), 1.0f);
		const auto transformedNormal = normalTransform * normals[idx];
		const float normalLength = fastgltf::math::length(transformedNormal);
		const auto normal = normalLength > 1e-12f
			? transformedNormal / normalLength
			: fastgltf::math::fvec3(0.0f, 1.0f, 0.0f);

		SceneVertex vertex;
		vertex.m_position[0] = position.x();
		vertex.m_position[1] = position.y();
		vertex.m_position[2] = position.z();
		vertex.m_normal[0] = normal.x();
		vertex.m_normal[1] = normal.y();
		vertex.m_normal[2] = normal.z();
		vertex.m_color[0] = float(baseColor[0]);
		vertex.m_color[1] = float(baseColor[1]);
		vertex.m_color[2] = float(baseColor[2]);
		vertex.m_material[0] = roughness;
		vertex.m_material[1] = metallic;
		vertex.m_texcoord[0] = texcoords[idx].x();
		vertex.m_texcoord[1] = texcoords[idx].y();
		_mesh.m_vertices.push_back(vertex);

		const bx::Vec3 worldPosition = { position.x(), position.y(), position.z() };
		_mesh.m_boundsMin = bx::min(_mesh.m_boundsMin, worldPosition);
		_mesh.m_boundsMax = bx::max(_mesh.m_boundsMax, worldPosition);
	}

	// Primitives without a material use the default one, after the glTF ones.
	const std::size_t materialSlot = _primitive.materialIndex.has_value()
		? _primitive.materialIndex.value()
		: _asset.materials.size();
	std::vector<uint32_t>& materialIndices = _mesh.m_materialIndices[materialSlot];
	for (const uint32_t index : indices)
	{
		if (index >= positions.size())
		{
			throw std::runtime_error("A glTF primitive contains an out-of-range index.");
		}

		materialIndices.push_back(uint32_t(baseVertex + index));
	}

	if (isEmissive)
	{
		for (std::size_t idx = 0; idx < indices.size(); idx += 3)
		{
			const auto vertexPosition = [&](std::size_t _corner)
			{
				const float* p = _mesh.m_vertices[baseVertex + indices[idx + _corner]].m_position;
				return bx::Vec3{ p[0], p[1], p[2] };
			};
			const auto vertexNormal = [&](std::size_t _corner)
			{
				const float* n = _mesh.m_vertices[baseVertex + indices[idx + _corner]].m_normal;
				return bx::Vec3{ n[0], n[1], n[2] };
			};

			const bx::Vec3 a = vertexPosition(0);
			const bx::Vec3 b = vertexPosition(1);
			const bx::Vec3 c = vertexPosition(2);
			const float area = 0.5f * bx::length(bx::cross(bx::sub(b, a), bx::sub(c, a)));
			const bx::Vec3 centroid = bx::mul(bx::add(bx::add(a, b), c), 1.0f / 3.0f);
			const bx::Vec3 normal = bx::add(bx::add(vertexNormal(0), vertexNormal(1)), vertexNormal(2));

			_mesh.m_emissiveCentroid = bx::add(_mesh.m_emissiveCentroid, bx::mul(centroid, area));
			_mesh.m_emissiveNormal = bx::add(_mesh.m_emissiveNormal, bx::mul(normal, area));
			_mesh.m_emissiveArea += area;
		}
	}
}

// The encoded bytes (PNG, JPEG, ...) of a glTF image: an external file next
// to the model, or data inside one of its buffers. False if unavailable.
bool readImageBytes(
	  const fastgltf::Asset& _asset
	, const fastgltf::Image& _image
	, const std::filesystem::path& _directory
	, std::vector<uint8_t>& _bytes
	)
{
	const auto copyBytes = [&_bytes](const std::byte* _data, std::size_t _size)
	{
		_bytes.assign(reinterpret_cast<const uint8_t*>(_data), reinterpret_cast<const uint8_t*>(_data) + _size);
		return _size > 0;
	};

	if (const auto* uri = std::get_if<fastgltf::sources::URI>(&_image.data))
	{
		if (!uri->uri.isLocalPath())
		{
			return false;
		}

		std::FILE* file = std::fopen((_directory / uri->uri.fspath()).string().c_str(), "rb");
		if (file == nullptr)
		{
			return false;
		}

		std::fseek(file, 0, SEEK_END);
		const long size = std::ftell(file);
		std::fseek(file, long(uri->fileByteOffset), SEEK_SET);
		_bytes.resize(size > long(uri->fileByteOffset) ? std::size_t(size - long(uri->fileByteOffset)) : 0);
		const std::size_t read = _bytes.empty() ? 0 : std::fread(_bytes.data(), 1, _bytes.size(), file);
		std::fclose(file);
		return read == _bytes.size() && !_bytes.empty();
	}

	if (const auto* array = std::get_if<fastgltf::sources::Array>(&_image.data))
	{
		return copyBytes(array->bytes.data(), array->bytes.size());
	}

	if (const auto* vector = std::get_if<fastgltf::sources::Vector>(&_image.data))
	{
		return copyBytes(vector->bytes.data(), vector->bytes.size());
	}

	if (const auto* view = std::get_if<fastgltf::sources::BufferView>(&_image.data))
	{
		const fastgltf::BufferView& bufferView = _asset.bufferViews[view->bufferViewIndex];
		const fastgltf::Buffer& buffer = _asset.buffers[bufferView.bufferIndex];
		const std::byte* data = nullptr;
		std::size_t size = 0;
		if (const auto* bufferArray = std::get_if<fastgltf::sources::Array>(&buffer.data))
		{
			data = bufferArray->bytes.data();
			size = bufferArray->bytes.size();
		}
		else if (const auto* bufferVector = std::get_if<fastgltf::sources::Vector>(&buffer.data))
		{
			data = bufferVector->bytes.data();
			size = bufferVector->bytes.size();
		}

		if (data == nullptr || bufferView.byteOffset + bufferView.byteLength > size)
		{
			return false;
		}

		return copyBytes(data + bufferView.byteOffset, bufferView.byteLength);
	}

	return false;
}

// Halves an RGBA8 image (rounding odd sizes down, but never below 1). Colour
// textures are averaged in linear space, so the mips do not darken; their
// shaders decode them with the same 2.2 gamma.
void downsampleRgba8(
	  const uint8_t* _src
	, uint32_t _srcWidth
	, uint32_t _srcHeight
	, uint8_t* _dst
	, uint32_t _dstWidth
	, uint32_t _dstHeight
	, bool _gamma
	)
{
	static float s_toLinear[256];
	static uint8_t s_toGamma[4096];
	static bool s_tablesReady = false;
	if (!s_tablesReady)
	{
		for (uint32_t idx = 0; idx < 256; ++idx)
		{
			s_toLinear[idx] = std::pow(float(idx) / 255.0f, 2.2f);
		}
		for (uint32_t idx = 0; idx < 4096; ++idx)
		{
			s_toGamma[idx] = uint8_t(std::lround(std::pow(float(idx) / 4095.0f, 1.0f / 2.2f) * 255.0f));
		}
		s_tablesReady = true;
	}

	for (uint32_t y = 0; y < _dstHeight; ++y)
	{
		const uint32_t y0 = std::min(2 * y, _srcHeight - 1);
		const uint32_t y1 = std::min(2 * y + 1, _srcHeight - 1);
		for (uint32_t x = 0; x < _dstWidth; ++x)
		{
			const uint32_t x0 = std::min(2 * x, _srcWidth - 1);
			const uint32_t x1 = std::min(2 * x + 1, _srcWidth - 1);
			const uint8_t* taps[4] =
			{
				_src + (y0 * _srcWidth + x0) * 4, _src + (y0 * _srcWidth + x1) * 4,
				_src + (y1 * _srcWidth + x0) * 4, _src + (y1 * _srcWidth + x1) * 4,
			};

			uint8_t* out = _dst + (y * _dstWidth + x) * 4;
			for (uint32_t channel = 0; channel < 4; ++channel)
			{
				if (_gamma && channel < 3)
				{
					float sum = 0.0f;
					for (const uint8_t* tap : taps)
					{
						sum += s_toLinear[tap[channel]];
					}
					out[channel] = s_toGamma[std::min(uint32_t(sum * 0.25f * 4095.0f + 0.5f), 4095u)];
				}
				else
				{
					uint32_t sum = 0;
					for (const uint8_t* tap : taps)
					{
						sum += tap[channel];
					}
					out[channel] = uint8_t((sum + 2) / 4);
				}
			}
		}
	}
}

// Decodes an image to RGBA8 and appends its mip chain. _gamma is true for
// colour textures (sRGB-encoded) and false for data such as metallic-roughness.
bool decodeImage(const std::vector<uint8_t>& _bytes, bool _gamma, SceneImage& _image)
{
	bimg::ImageContainer* container = bimg::imageParse(
		  entry::getAllocator()
		, _bytes.data()
		, uint32_t(_bytes.size())
		, bimg::TextureFormat::RGBA8
		);
	if (container == nullptr)
	{
		return false;
	}

	const bool valid = container->m_format == bimg::TextureFormat::RGBA8
		&& container->m_width > 0 && container->m_height > 0
		&& container->m_width <= 16384 && container->m_height <= 16384
		// bimg reports a depth of 0 for 2D images.
		&& container->m_depth <= 1 && container->m_numLayers == 1 && !container->m_cubeMap;
	if (!valid)
	{
		bimg::imageFree(container);
		return false;
	}

	uint32_t width = container->m_width;
	uint32_t height = container->m_height;
	_image.m_width = uint16_t(width);
	_image.m_height = uint16_t(height);

	// Size of the whole chain: every level halves, down to 1x1.
	std::size_t total = 0;
	for (uint32_t levelWidth = width, levelHeight = height; ; )
	{
		total += std::size_t(levelWidth) * levelHeight * 4;
		if (levelWidth == 1 && levelHeight == 1)
		{
			break;
		}
		levelWidth = std::max(levelWidth / 2, 1u);
		levelHeight = std::max(levelHeight / 2, 1u);
	}

	_image.m_data.resize(total);
	const std::size_t level0Size = std::size_t(width) * height * 4;
	std::memcpy(_image.m_data.data(), container->m_data, level0Size);
	bimg::imageFree(container);

	std::size_t offset = 0;
	while (width > 1 || height > 1)
	{
		const uint32_t nextWidth = std::max(width / 2, 1u);
		const uint32_t nextHeight = std::max(height / 2, 1u);
		const std::size_t levelSize = std::size_t(width) * height * 4;
		downsampleRgba8(
			  _image.m_data.data() + offset, width, height
			, _image.m_data.data() + offset + levelSize, nextWidth, nextHeight
			, _gamma
			);
		offset += levelSize;
		width = nextWidth;
		height = nextHeight;
	}

	return true;
}

// The glTF image a texture slot of a material refers to, if any.
std::optional<std::size_t> textureImage(const fastgltf::Asset& _asset, const fastgltf::Optional<fastgltf::TextureInfo>& _info)
{
	if (!_info.has_value() || _info->textureIndex >= _asset.textures.size())
	{
		return std::nullopt;
	}

	const fastgltf::Texture& texture = _asset.textures[_info->textureIndex];
	if (!texture.imageIndex.has_value() || texture.imageIndex.value() >= _asset.images.size())
	{
		return std::nullopt;
	}

	return texture.imageIndex.value();
}

// Reads the materials of a model and decodes the textures they use. Images
// that cannot be read or decoded are skipped with a warning; the material
// then uses its factors alone.
void loadMaterials(const fastgltf::Asset& _asset, const std::filesystem::path& _directory, SceneMesh& _mesh)
{
	_mesh.m_materials.resize(_asset.materials.size() + 1);
	_mesh.m_materialIndices.resize(_asset.materials.size() + 1);

	// Decode each image once, as colour if any material uses it as such.
	std::vector<int8_t> imageUse(_asset.images.size(), 0); // 0: unused, 1: data, 2: colour
	for (const fastgltf::Material& material : _asset.materials)
	{
		if (const auto image = textureImage(_asset, material.pbrData.baseColorTexture))
		{
			imageUse[*image] = 2;
		}
		if (const auto image = textureImage(_asset, material.pbrData.metallicRoughnessTexture))
		{
			imageUse[*image] = std::max<int8_t>(imageUse[*image], 1);
		}
	}

	std::vector<int32_t> imageSlots(_asset.images.size(), -1);
	uint32_t failed = 0;
	std::vector<uint8_t> bytes;
	for (std::size_t idx = 0; idx < _asset.images.size(); ++idx)
	{
		if (imageUse[idx] == 0)
		{
			continue;
		}

		SceneImage image;
		if (readImageBytes(_asset, _asset.images[idx], _directory, bytes)
		&&  decodeImage(bytes, imageUse[idx] == 2, image))
		{
			imageSlots[idx] = int32_t(_mesh.m_images.size());
			_mesh.m_images.push_back(std::move(image));
		}
		else
		{
			++failed;
		}
	}

	if (failed > 0)
	{
		std::fprintf(stderr, "%u of the model's textures could not be loaded; those materials use their factors alone.\n", failed);
	}

	for (std::size_t idx = 0; idx < _asset.materials.size(); ++idx)
	{
		const fastgltf::Material& source = _asset.materials[idx];
		SceneMaterial& material = _mesh.m_materials[idx];

		if (const auto image = textureImage(_asset, source.pbrData.baseColorTexture))
		{
			material.m_baseColorImage = imageSlots[*image];
		}
		if (const auto image = textureImage(_asset, source.pbrData.metallicRoughnessTexture))
		{
			material.m_metallicRoughnessImage = imageSlots[*image];
			// Models with a metallic-roughness texture usually leave the
			// metallic factor at its default of 1 and keep the metal mask in the
			// texture. Without the texture, the factor alone would make the
			// material bare metal (no diffuse light at all).
			material.m_metallicTextureMissing = material.m_metallicRoughnessImage < 0;
		}
		if (source.alphaMode == fastgltf::AlphaMode::Mask)
		{
			material.m_alphaCutoff = float(source.alphaCutoff);
			material.m_twoSidedMask = source.doubleSided;
		}
	}
}

SceneMesh loadScene(const std::filesystem::path& _path)
{
	auto data = fastgltf::GltfDataBuffer::FromPath(_path);
	if (data.error() != fastgltf::Error::None)
	{
		throw std::runtime_error(
			  "Failed to read glTF model '" + _path.string() + "': "
			+ std::string(fastgltf::getErrorMessage(data.error()))
			);
	}

	fastgltf::Parser parser;
	auto assetResult = parser.loadGltf(
		  data.get()
		, _path.parent_path()
		, fastgltf::Options::LoadExternalBuffers
		);
	if (assetResult.error() != fastgltf::Error::None)
	{
		throw std::runtime_error(
			  "Failed to load glTF model '" + _path.string() + "': "
			+ std::string(fastgltf::getErrorMessage(assetResult.error()))
			);
	}

	const auto& asset = assetResult.get();
	if (asset.scenes.empty())
	{
		throw std::runtime_error("The glTF file has no scenes.");
	}
	const std::size_t sceneIndex = asset.defaultScene.has_value() ? asset.defaultScene.value() : 0;
	if (sceneIndex >= asset.scenes.size())
	{
		throw std::runtime_error("The glTF file has an invalid default scene.");
	}

	SceneMesh mesh;
	loadMaterials(asset, _path.parent_path(), mesh);
	fastgltf::iterateSceneNodes(
		  asset
		, sceneIndex
		, fastgltf::math::fmat4x4()
		, [&asset, &mesh](const fastgltf::Node& _node, const fastgltf::math::fmat4x4& _transform)
		{
			if (!_node.meshIndex.has_value())
			{
				return;
			}

			const std::size_t meshIndex = _node.meshIndex.value();
			if (meshIndex >= asset.meshes.size())
			{
				throw std::runtime_error("The glTF scene references an invalid mesh.");
			}

			for (const auto& primitive : asset.meshes[meshIndex].primitives)
			{
				appendPrimitive(asset, primitive, _transform, mesh);
			}
		}
		);

	// One draw per material, over consecutive indices.
	for (uint32_t material = 0; material < mesh.m_materialIndices.size(); ++material)
	{
		std::vector<uint32_t>& indices = mesh.m_materialIndices[material];
		if (indices.empty())
		{
			continue;
		}

		SceneDraw draw;
		draw.m_firstIndex = uint32_t(mesh.m_indices.size());
		draw.m_indexCount = uint32_t(indices.size());
		draw.m_material = material;
		mesh.m_draws.push_back(draw);
		mesh.m_indices.insert(mesh.m_indices.end(), indices.begin(), indices.end());
		std::vector<uint32_t>().swap(indices);
	}

	if (mesh.m_vertices.empty() || mesh.m_indices.empty())
	{
		throw std::runtime_error("The glTF scene contains no drawable triangles.");
	}

	return mesh;
}

// Frames the scene and places the point light. The light goes in front of the
// scene's emissive surfaces, or near the top of the bounds if there are none.
SceneInfo describeScene(const SceneMesh& _mesh)
{
	SceneInfo info;
	info.m_center = bx::mul(bx::add(_mesh.m_boundsMin, _mesh.m_boundsMax), 0.5f);
	info.m_radius = std::max(0.5f * bx::length(bx::sub(_mesh.m_boundsMax, _mesh.m_boundsMin)), 1e-3f);

	// Kept away from the emitter so the inverse-square falloff does not blow
	// out the surfaces right next to it.
	const float lightOffset = 0.15f * info.m_radius;
	const float normalLength = bx::length(_mesh.m_emissiveNormal);
	if (_mesh.m_emissiveArea > 0.0f && normalLength > 0.0f)
	{
		const bx::Vec3 centroid = bx::mul(_mesh.m_emissiveCentroid, 1.0f / _mesh.m_emissiveArea);
		const bx::Vec3 normal = bx::mul(_mesh.m_emissiveNormal, 1.0f / normalLength);
		info.m_lightPosition = bx::add(centroid, bx::mul(normal, lightOffset));
	}
	else
	{
		info.m_lightPosition = { info.m_center.x, _mesh.m_boundsMax.y - lightOffset, info.m_center.z };
	}

	return info;
}

// Destroys a bgfx handle if it is valid, and marks it invalid.
template<typename Handle>
void destroyIfValid(Handle& _handle)
{
	if (bgfx::isValid(_handle))
	{
		bgfx::destroy(_handle);
		_handle = BGFX_INVALID_HANDLE;
	}
}

// The radius of the scene the shader distances and light intensity were tuned
// for (the bundled Cornell box). Their values are scaled by the actual scene
// radius over this one.
constexpr float kReferenceSceneRadius = 1.75f;

// Point light shadows: six cube faces in a 3x2 atlas (see shaders/shadow.sh).
constexpr uint16_t kShadowFaceSize = 512;
constexpr uint16_t kShadowFaceBorder = 4; // texels kept for filter taps near face edges
constexpr bgfx::ViewId kShadowViewBase = 3; // views 3-8, ordered before the others

// Denoiser (SVGF-style): a variance pass and a-trous iterations with tap
// spacings 1, 2, 4, 8, 16, at GI resolution, between the GI and composite passes.
constexpr bgfx::ViewId kDenoiseViewBase = 9; // views 9-14
constexpr uint8_t kAtrousIterations = 5;

// ReSTIR GI passes: the G-buffer (view 0), then the world-space hash grid is
// cleared and filled (compute), then the temporal pass (view 1) and the
// spatial pass.
constexpr bgfx::ViewId kGridClearView = 15;
constexpr bgfx::ViewId kGridInsertView = 16;
constexpr bgfx::ViewId kSpatialView = 17;

// World-space hash grid (see shaders/hashgrid.sh, which must agree on the
// table size and cell capacity).
constexpr uint32_t kGridTableSize = 65536;
constexpr uint32_t kGridCellCapacity = 32;
constexpr float kGridMinCellSize = 0.02f; // for the reference scene, scaled with the scene
constexpr float kGridCellPixels = 6.0f;   // cell size, in GI pixels, that the grid aims for

// Buffer stages of the grid, in the compute passes and in the GI passes.
constexpr uint8_t kGridComputeStage = 0;  // 0-2
constexpr uint8_t kGridFragmentStage = 11; // 11-13

constexpr float kCameraFovY = 48.0f;

// Camera start, in scene radii from the centre of the scene, and speed.
constexpr float kInitialCameraDistance = 3.17f; // outside the scene, for open models like the Cornell box
constexpr float kCameraSpeed = 0.4f;            // scene radii per second, before the wheel's scale

constexpr float kInitialCameraYaw = 0.64f;
constexpr float kInitialCameraPitch = 0.255f;

// Models offered by the M key when they are present, with where the camera
// starts. Sponza is closed all around, so the camera starts inside its
// courtyard, looking down its long (x) axis. It is not committed (see
// cmake/fetch_sponza.cmake), so it only shows up once fetched.
struct BundledModel
{
	const char* m_path;
	float m_cameraDistance; // in scene radii
	float m_cameraYaw;      // radians, 0 = camera on +z
};

constexpr BundledModel kBundledModels[] =
{
	{ "assets/cornell_box/scene.gltf", kInitialCameraDistance, kInitialCameraYaw },
	{ "assets/sponza/Sponza.gltf",     0.45f,                  bx::kPiHalf },
};

class ExampleRestirGi : public entry::AppI
{
public:
	ExampleRestirGi(const char* _name, const char* _description, const char* _url)
		: entry::AppI(_name, _description, _url)
	{
	}

	void init(int32_t _argc, const char* const* _argv, uint32_t _width, uint32_t _height) override
	{
		Args args(_argc, _argv);
		bx::CommandLine commandLine(_argc, _argv);
		// M cycles through the models: the one passed with --model (if any),
		// then the bundled ones that are present.
		if (const char* modelOption = commandLine.findOption("model"))
		{
			m_modelPaths.push_back(modelOption);
		}
		for (const BundledModel& bundled : kBundledModels)
		{
			if (std::filesystem::exists(bundled.m_path)
			&&  std::find(m_modelPaths.begin(), m_modelPaths.end(), bundled.m_path) == m_modelPaths.end())
			{
				m_modelPaths.push_back(bundled.m_path);
			}
		}
		if (m_modelPaths.empty())
		{
			throw std::runtime_error("No model found: pass one with --model.");
		}

		m_width  = _width;
		m_height = _height;
		m_debug  = BGFX_DEBUG_TEXT;
		m_reset  = BGFX_RESET_VSYNC;

		bgfx::Init init;
		init.type     = args.m_type;
		init.vendorId = args.m_pciId;
		init.swapChain.nwh     = entry::getNativeWindowHandle(entry::kDefaultWindowHandle);
		init.swapChain.ndt     = entry::getNativeDisplayHandle();
		init.platformData.type = entry::getNativeWindowHandleType();
		init.swapChain.width   = m_width;
		init.swapChain.height  = m_height;
		init.reset             = m_reset;
		bgfx::init(init);

		bgfx::setDebug(m_debug);
		bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
		bgfx::setViewClear(2, BGFX_CLEAR_COLOR, 0x20242bff, 1.0f, 0);

		// The shadow faces must be rendered before the GI and composite passes.
		const bgfx::ViewId viewOrder[] =
		{
			kShadowViewBase + 0, kShadowViewBase + 1, kShadowViewBase + 2,
			kShadowViewBase + 3, kShadowViewBase + 4, kShadowViewBase + 5,
			0, kGridClearView, kGridInsertView, 1, kSpatialView,
			kDenoiseViewBase + 0, kDenoiseViewBase + 1, kDenoiseViewBase + 2,
			kDenoiseViewBase + 3, kDenoiseViewBase + 4, kDenoiseViewBase + 5,
			2,
		};
		bgfx::setViewOrder(0, BX_COUNTOF(viewOrder), viewOrder);

		SceneVertex::init();
		FullscreenVertex::init();

		if (0 == (bgfx::getCaps()->supported & BGFX_CAPS_INDEX32))
		{
			throw std::runtime_error("The renderer does not support 32-bit index buffers.");
		}
		if (0 == (bgfx::getCaps()->supported & BGFX_CAPS_COMPUTE))
		{
			throw std::runtime_error("World-space ReSTIR GI requires compute shaders.");
		}

		// Materials: a white texture stands in for missing ones, so the
		// factors apply unchanged.
		m_baseColorSampler = bgfx::createUniform("s_baseColor", bgfx::UniformType::Sampler);
		m_metallicRoughnessSampler = bgfx::createUniform("s_metallicRoughness", bgfx::UniformType::Sampler);
		m_materialInfoUniform = bgfx::createUniform("u_materialInfo", bgfx::UniformType::Vec4);
		const uint32_t white = 0xffffffff;
		m_whiteTexture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(&white, sizeof(white)));

		// The first model must load; a failure while switching later only
		// keeps the current one.
		setModel(0, loadScene(m_modelPaths[0]));

		const FullscreenVertex fullscreenVertices[] =
		{
			{ { -1.0f, -1.0f, 0.0f }, { 0.0f, bgfx::getCaps()->originBottomLeft ? 0.0f : 1.0f } },
			{ {  1.0f, -1.0f, 0.0f }, { 1.0f, bgfx::getCaps()->originBottomLeft ? 0.0f : 1.0f } },
			{ {  1.0f,  1.0f, 0.0f }, { 1.0f, bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f } },
			{ { -1.0f,  1.0f, 0.0f }, { 0.0f, bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f } },
		};
		const uint16_t fullscreenIndices[] = { 0, 1, 2, 0, 2, 3 };
		m_fullscreenVbh = bgfx::createVertexBuffer(
			  bgfx::copy(fullscreenVertices, sizeof(fullscreenVertices))
			, FullscreenVertex::ms_layout
			);
		m_fullscreenIbh = bgfx::createIndexBuffer(
			bgfx::copy(fullscreenIndices, sizeof(fullscreenIndices))
			);

		m_geometryProgram = loadProgram("vs_scene", "fs_gbuffer");
		m_temporalProgram = loadProgram("vs_fullscreen", "fs_restir_temporal");
		m_spatialProgram = loadProgram("vs_fullscreen", "fs_restir_spatial");
		m_gridClearProgram = bgfx::createProgram(loadShader("cs_grid_clear"), true);
		m_gridInsertProgram = bgfx::createProgram(loadShader("cs_grid_insert"), true);
		m_compositeProgram = loadProgram("vs_fullscreen", "fs_composite");
		m_shadowProgram = loadProgram("vs_shadow", "fs_shadow");
		m_varianceProgram = loadProgram("vs_fullscreen", "fs_svgf_variance");
		m_atrousProgram = loadProgram("vs_fullscreen", "fs_svgf_atrous");
		m_lightPosition = bgfx::createUniform("u_lightPosition", bgfx::UniformType::Vec4);
		m_lightColor = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4);
		m_cameraPosition = bgfx::createUniform("u_cameraPosition", bgfx::UniformType::Vec4);
		m_albedoSampler = bgfx::createUniform("s_albedo", bgfx::UniformType::Sampler);
		m_normalSampler = bgfx::createUniform("s_normal", bgfx::UniformType::Sampler);
		m_depthSampler = bgfx::createUniform("s_depth", bgfx::UniformType::Sampler);
		m_historySampleSampler = bgfx::createUniform("s_historySample", bgfx::UniformType::Sampler);
		m_historyReceiverSampler = bgfx::createUniform("s_historyReceiver", bgfx::UniformType::Sampler);
		m_historyColorSampler = bgfx::createUniform("s_historyColor", bgfx::UniformType::Sampler);
		m_historyNormalSampler = bgfx::createUniform("s_historyNormal", bgfx::UniformType::Sampler);
		m_giSampler = bgfx::createUniform("s_gi", bgfx::UniformType::Sampler);
		m_inverseViewProjection = bgfx::createUniform("u_sceneInvViewProj", bgfx::UniformType::Mat4);
		m_viewProjection = bgfx::createUniform("u_sceneViewProj", bgfx::UniformType::Mat4);
		m_previousViewProjection = bgfx::createUniform("u_previousViewProj", bgfx::UniformType::Mat4);
		m_frameInfo = bgfx::createUniform("u_frameInfo", bgfx::UniformType::Vec4);
		m_screenInfo = bgfx::createUniform("u_screenInfo", bgfx::UniformType::Vec4);
		m_sceneInfoUniform = bgfx::createUniform("u_sceneInfo", bgfx::UniformType::Vec4);
		m_shadowSampler = bgfx::createUniform("s_shadowMap", bgfx::UniformType::Sampler);
		m_shadowViewProjUniform = bgfx::createUniform("u_shadowViewProj", bgfx::UniformType::Mat4, 6);
		m_shadowInfoUniform = bgfx::createUniform("u_shadowInfo", bgfx::UniformType::Vec4);
		m_shadowOptionsUniform = bgfx::createUniform("u_shadowOptions", bgfx::UniformType::Vec4);
		m_historyMomentsSampler = bgfx::createUniform("s_historyMoments", bgfx::UniformType::Sampler);
		m_giColorSampler = bgfx::createUniform("s_giColor", bgfx::UniformType::Sampler);
		m_giMomentsSampler = bgfx::createUniform("s_giMoments", bgfx::UniformType::Sampler);
		m_giReceiverSampler = bgfx::createUniform("s_giReceiver", bgfx::UniformType::Sampler);
		m_giNormalSampler = bgfx::createUniform("s_giNormal", bgfx::UniformType::Sampler);
		m_filterInputSampler = bgfx::createUniform("s_filterInput", bgfx::UniformType::Sampler);
		m_atrousInfoUniform = bgfx::createUniform("u_atrousInfo", bgfx::UniformType::Vec4);
		m_reservoirSampleSampler = bgfx::createUniform("s_reservoirSample", bgfx::UniformType::Sampler);
		m_reservoirReceiverSampler = bgfx::createUniform("s_reservoirReceiver", bgfx::UniformType::Sampler);
		m_reservoirNormalSampler = bgfx::createUniform("s_reservoirNormal", bgfx::UniformType::Sampler);
		m_gridInfoUniform = bgfx::createUniform("u_gridInfo", bgfx::UniformType::Vec4);
		m_gridCameraUniform = bgfx::createUniform("u_gridCamera", bgfx::UniformType::Vec4);

		// Two hash grids, this frame's and last frame's, like the reservoirs
		// they index. Their size does not depend on the resolution.
		for (uint8_t idx = 0; idx < 2; ++idx)
		{
			m_gridChecksums[idx] = bgfx::createDynamicIndexBuffer(
				kGridTableSize, BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_INDEX32);
			m_gridCounts[idx] = bgfx::createDynamicIndexBuffer(
				kGridTableSize, BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_INDEX32);
			m_gridEntries[idx] = bgfx::createDynamicIndexBuffer(
				kGridTableSize * kGridCellCapacity, BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_INDEX32);
			if (!bgfx::isValid(m_gridChecksums[idx]) || !bgfx::isValid(m_gridCounts[idx])
			||  !bgfx::isValid(m_gridEntries[idx]))
			{
				throw std::runtime_error("Failed to create the hash grid buffers.");
			}
		}

		BX_ASSERT(bgfx::isValid(m_vbh), "Failed to create the scene vertex buffer.");
		BX_ASSERT(bgfx::isValid(m_ibh), "Failed to create the scene index buffer.");
		BX_ASSERT(bgfx::isValid(m_geometryProgram), "Failed to load G-buffer shaders.");
		BX_ASSERT(bgfx::isValid(m_temporalProgram), "Failed to load ReSTIR GI temporal shaders.");
		BX_ASSERT(bgfx::isValid(m_spatialProgram), "Failed to load ReSTIR GI spatial shaders.");
		BX_ASSERT(bgfx::isValid(m_gridClearProgram), "Failed to load the hash grid clear shader.");
		BX_ASSERT(bgfx::isValid(m_gridInsertProgram), "Failed to load the hash grid insert shader.");
		BX_ASSERT(bgfx::isValid(m_compositeProgram), "Failed to load composite shaders.");
		BX_ASSERT(bgfx::isValid(m_shadowProgram), "Failed to load shadow shaders.");
		BX_ASSERT(bgfx::isValid(m_varianceProgram), "Failed to load denoiser variance shaders.");
		BX_ASSERT(bgfx::isValid(m_atrousProgram), "Failed to load denoiser filter shaders.");
		createRenderTargets(_width, _height);
		createShadowMap();
	}

	int shutdown() override
	{
		destroyRenderTargets();
		destroyIfValid(m_shadowFrameBuffer);
		destroyModel();
		destroyIfValid(m_whiteTexture);
		destroyIfValid(m_fullscreenVbh);
		destroyIfValid(m_fullscreenIbh);
		for (uint8_t idx = 0; idx < 2; ++idx)
		{
			destroyIfValid(m_gridChecksums[idx]);
			destroyIfValid(m_gridCounts[idx]);
			destroyIfValid(m_gridEntries[idx]);
		}

		bgfx::ProgramHandle programs[] =
		{
			m_geometryProgram, m_temporalProgram, m_spatialProgram, m_compositeProgram, m_shadowProgram,
			m_varianceProgram, m_atrousProgram, m_gridClearProgram, m_gridInsertProgram,
		};
		for (bgfx::ProgramHandle& program : programs)
		{
			destroyIfValid(program);
		}

		bgfx::UniformHandle uniforms[] =
		{
			m_lightPosition, m_lightColor, m_cameraPosition,
			m_albedoSampler, m_normalSampler, m_depthSampler,
			m_historySampleSampler, m_historyReceiverSampler, m_historyColorSampler,
			m_historyNormalSampler, m_giSampler,
			m_inverseViewProjection, m_viewProjection, m_previousViewProjection,
			m_frameInfo, m_screenInfo, m_sceneInfoUniform,
			m_shadowSampler, m_shadowViewProjUniform, m_shadowInfoUniform, m_shadowOptionsUniform,
			m_historyMomentsSampler, m_giColorSampler, m_giMomentsSampler, m_giReceiverSampler,
			m_giNormalSampler, m_filterInputSampler, m_atrousInfoUniform,
			m_reservoirSampleSampler, m_reservoirReceiverSampler, m_reservoirNormalSampler,
			m_gridInfoUniform, m_gridCameraUniform,
			m_baseColorSampler, m_metallicRoughnessSampler, m_materialInfoUniform,
		};
		for (bgfx::UniformHandle& uniform : uniforms)
		{
			destroyIfValid(uniform);
		}

		bgfx::shutdown();
		return 0;
	}

	bool update() override
	{
		if (entry::processEvents(m_width, m_height, m_debug, m_reset, &m_mouseState))
		{
			return false;
		}

		updateCameraInput();

		// B toggles feeding last frame's indirect light back into the samples.
		if (keyPressed(entry::Key::KeyB, m_bounceKeyDown))
		{
			m_multipleBounces = !m_multipleBounces;
		}

		// H toggles shadows (WASD move the camera).
		if (keyPressed(entry::Key::KeyH, m_shadowKeyDown))
		{
			m_shadowsEnabled = !m_shadowsEnabled;
		}

		// N toggles the denoiser. The history means something different with
		// and without it (filtered or not), so restart accumulation.
		if (keyPressed(entry::Key::KeyN, m_denoiseKeyDown))
		{
			m_denoiseEnabled = !m_denoiseEnabled;
			m_historyValid = false;
		}

		// G switches where reservoirs are reused from: world-space hash grid
		// cells, or screen-space neighbourhoods. The reservoirs mean the same
		// in both, so accumulation carries on.
		if (keyPressed(entry::Key::KeyG, m_gridKeyDown))
		{
			m_worldSpaceReuse = !m_worldSpaceReuse;
		}

		if (keyPressed(entry::Key::KeyM, m_modelKeyDown) && m_modelPaths.size() > 1)
		{
			switchModel((m_modelIndex + 1) % m_modelPaths.size());
		}

		bgfx::dbgTextClear();
		bgfx::dbgTextPrintf(0, 1, 0x0f, "Indirect light: %s (B to toggle)"
			, m_multipleBounces ? "multiple bounces" : "single bounce");
		bgfx::dbgTextPrintf(0, 2, 0x0f, "Shadows: %s (H to toggle)"
			, m_shadowsEnabled ? "on" : "off");
		bgfx::dbgTextPrintf(0, 3, 0x0f, "Denoiser: %s (N to toggle)"
			, m_denoiseEnabled ? "on" : "off");
		bgfx::dbgTextPrintf(0, 4, 0x0f, "Reservoir reuse: %s (G to toggle)"
			, m_worldSpaceReuse ? "world-space hash grid" : "screen-space");
		if (m_modelPaths.size() > 1)
		{
			bgfx::dbgTextPrintf(0, 5, 0x0f, "Model %u/%u: %s (M to switch)"
				, uint32_t(m_modelIndex + 1), uint32_t(m_modelPaths.size()), m_modelPaths[m_modelIndex].c_str());
		}
		else
		{
			bgfx::dbgTextPrintf(0, 5, 0x0f, "Model: %s", m_modelPaths[m_modelIndex].c_str());
		}
		bgfx::dbgTextPrintf(0, 6, 0x0f
			, "Camera: WASD move, Shift/Ctrl up/down, drag to look, wheel for speed (x%.2f)", m_moveSpeedScale);
		if (!m_modelError.empty())
		{
			bgfx::dbgTextPrintf(0, 7, 0x0c, "%s", m_modelError.c_str());
		}

		if (m_width == 0 || m_height == 0)
		{
			bgfx::frame();
			return true;
		}
		if (m_width != m_targetWidth || m_height != m_targetHeight)
		{
			createRenderTargets(m_width, m_height);
		}

		const float radius = m_scene.m_radius;
		const bx::Vec3 eye = m_eye;
		const bx::Vec3 at = bx::add(eye, cameraForward());
		float view[16];
		float projection[16];
		// glTF is right-handed; a left-handed camera would mirror the scene.
		bx::mtxLookAt(view, eye, at, { 0.0f, 1.0f, 0.0f }, bx::Handedness::Right);
		// The far plane reaches past the scene from wherever the camera is.
		const float farPlane = bx::length(bx::sub(eye, m_scene.m_center)) + 2.0f * radius;
		bx::mtxProj(
			  projection
			, kCameraFovY
			, float(m_width) / float(m_height)
			, 0.05f * radius
			, std::max(farPlane, 2.5f * radius)
			, bgfx::getCaps()->homogeneousDepth
			, bx::Handedness::Right
			);

		float viewProjection[16];
		float inverseViewProjection[16];
		bx::mtxMul(viewProjection, view, projection);
		bx::mtxInverse(inverseViewProjection, viewProjection);

		const float lightPosition[] =
		{
			m_scene.m_lightPosition.x, m_scene.m_lightPosition.y, m_scene.m_lightPosition.z, 1.0f,
		};
		// Intensity for the reference scene; scaled with the square of the
		// scene size so the scene is lit the same at any scale.
		const float sceneScale = radius / kReferenceSceneRadius;
		const float lightIntensityScale = sceneScale * sceneScale;
		const float lightColor[] = { lightIntensityScale, lightIntensityScale, lightIntensityScale, 1.0f };
		const float sceneInfo[] = { sceneScale, 0.0f, 0.0f, 0.0f };
		const float cameraPosition[] = { eye.x, eye.y, eye.z, 1.0f };

		const uint16_t giWidth = uint16_t(std::max(1u, m_width / 2));
		const uint16_t giHeight = uint16_t(std::max(1u, m_height / 2));

		// Shadow pass: the scene from the light, once per cube face. Each face
		// covers slightly more than 90 degrees so that the border texels exist.
		const float shadowTanHalfFov = float(kShadowFaceSize) / float(kShadowFaceSize - 2 * kShadowFaceBorder);
		const float shadowFar = 2.5f * radius;
		const float shadowInfo[] =
		{
			shadowFar, shadowTanHalfFov, 1.0f / float(kShadowFaceSize),
			bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f,
		};
		const float shadowOptions[] = { m_shadowsEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
		float shadowViewProj[6 * 16];
		{
			const bx::Vec3 light = m_scene.m_lightPosition;
			const bx::Vec3 faceDirections[6] =
			{
				{ 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
				{ 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
				{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
			};
			float shadowProjection[16];
			bx::mtxProj(
				  shadowProjection
				, bx::toDeg(2.0f * std::atan(shadowTanHalfFov))
				, 1.0f
				, 0.01f * radius
				, shadowFar
				, bgfx::getCaps()->homogeneousDepth
				, bx::Handedness::Right
				);

			for (uint8_t face = 0; face < 6; ++face)
			{
				const bx::Vec3 up = face == 2 || face == 3
					? bx::Vec3{ 0.0f, 0.0f, 1.0f }
					: bx::Vec3{ 0.0f, 1.0f, 0.0f };
				float faceView[16];
				bx::mtxLookAt(faceView, light, bx::add(light, faceDirections[face]), up, bx::Handedness::Right);
				bx::mtxMul(&shadowViewProj[face * 16], faceView, shadowProjection);

				const bgfx::ViewId viewId = bgfx::ViewId(kShadowViewBase + face);
				bgfx::setViewName(viewId, "Shadow Face");
				bgfx::setViewRect(viewId
					, uint16_t((face % 3) * kShadowFaceSize)
					, uint16_t((face / 3) * kShadowFaceSize)
					, kShadowFaceSize
					, kShadowFaceSize
					);
				bgfx::setViewFrameBuffer(viewId, m_shadowFrameBuffer);
				bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xffffffff, 1.0f, 0);
				bgfx::setViewTransform(viewId, faceView, shadowProjection);
				bgfx::touch(viewId);

				if (m_shadowsEnabled)
				{
					for (const SceneDraw& draw : m_draws)
					{
								bgfx::setVertexBuffer(0, m_vbh);
						bgfx::setIndexBuffer(m_ibh, draw.m_firstIndex, draw.m_indexCount);
						setMaterial(draw.m_material);
						bgfx::setUniform(m_lightPosition, lightPosition);
						bgfx::setUniform(m_shadowInfoUniform, shadowInfo);
						bgfx::setState(BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
						bgfx::submit(viewId, m_shadowProgram);
					}
				}
			}
		}

		bgfx::setMarker("Geometry Pass");
		bgfx::setViewRect(0, 0, 0, uint16_t(m_width), uint16_t(m_height));
		bgfx::setViewFrameBuffer(0, m_gbuffer);
		bgfx::setViewTransform(0, view, projection);
		bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
		for (const SceneDraw& draw : m_draws)
		{
			bgfx::setVertexBuffer(0, m_vbh);
			bgfx::setIndexBuffer(m_ibh, draw.m_firstIndex, draw.m_indexCount);
			setMaterial(draw.m_material);
			bgfx::setState(
				  BGFX_STATE_WRITE_RGB
				| BGFX_STATE_WRITE_A
				| BGFX_STATE_WRITE_Z
				| BGFX_STATE_DEPTH_TEST_LESS
				);
			bgfx::submit(0, m_geometryProgram);
		}

		// This frame's reservoirs, shaded GI and hash grid are written at
		// historyWrite; last frame's are read from m_historyRead.
		const uint8_t historyWrite = uint8_t(1 - m_historyRead);

		const float frameInfo[] =
		{
			float(m_frameNumber & 0xffffff)
			, m_historyValid ? 1.0f : 0.0f
			, bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f
			, m_multipleBounces ? 1.0f : 0.0f
		};
		const float screenInfo[] =
		{
			float(giWidth), float(giHeight), 1.0f / float(giWidth), 1.0f / float(giHeight),
		};
		// The world size of a GI pixel grows linearly with the distance from
		// the camera; the grid sizes its cells from it.
		const float gridInfo[] =
		{
			kGridMinCellSize * sceneScale
			, 2.0f * std::tan(0.5f * bx::toRad(kCameraFovY)) / float(giHeight)
			, kGridCellPixels
			, m_worldSpaceReuse ? 1.0f : 0.0f
		};
		const float previousCameraPosition[] =
		{
			m_previousEye.x, m_previousEye.y, m_previousEye.z, 1.0f,
		};

		// World-space hash grid: clear this frame's grid, then list every GI
		// pixel in the cell of its receiver.
		bgfx::setMarker("Hash Grid");
		bgfx::setViewName(kGridClearView, "Hash Grid Clear");
		bgfx::setBuffer(kGridComputeStage + 0, m_gridChecksums[historyWrite], bgfx::Access::Write);
		bgfx::setBuffer(kGridComputeStage + 1, m_gridCounts[historyWrite], bgfx::Access::Write);
		bgfx::setBuffer(kGridComputeStage + 2, m_gridEntries[historyWrite], bgfx::Access::Write);
		bgfx::dispatch(kGridClearView, m_gridClearProgram, kGridTableSize / 64, 1, 1);

		bgfx::setViewName(kGridInsertView, "Hash Grid Insert");
		bgfx::setBuffer(kGridComputeStage + 0, m_gridChecksums[historyWrite], bgfx::Access::ReadWrite);
		bgfx::setBuffer(kGridComputeStage + 1, m_gridCounts[historyWrite], bgfx::Access::ReadWrite);
		bgfx::setBuffer(kGridComputeStage + 2, m_gridEntries[historyWrite], bgfx::Access::Write);
		bgfx::setTexture(3, m_normalSampler, m_gbufferNormal);
		bgfx::setTexture(4, m_depthSampler, m_gbufferDepth);
		bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
		bgfx::setUniform(m_frameInfo, frameInfo);
		bgfx::setUniform(m_screenInfo, screenInfo);
		bgfx::setUniform(m_gridInfoUniform, gridInfo);
		bgfx::setUniform(m_gridCameraUniform, cameraPosition);
		bgfx::dispatch(kGridInsertView, m_gridInsertProgram
			, uint32_t(giWidth + 7) / 8, uint32_t(giHeight + 7) / 8, 1);

		// Uniforms and textures both GI passes use (see shaders/restir.sh).
		const auto setGiPassInputs = [&]()
		{
			bgfx::setTexture(0, m_albedoSampler, m_gbufferAlbedo);
			bgfx::setTexture(1, m_normalSampler, m_gbufferNormal);
			bgfx::setTexture(2, m_depthSampler, m_gbufferDepth);
			// With the denoiser, the history is its first filtered iteration.
			bgfx::setTexture(5, m_historyColorSampler
				, m_denoiseEnabled ? m_filteredHistory[m_historyRead] : m_historyColor[m_historyRead]);
			bgfx::setTexture(6, m_historyNormalSampler, m_historyNormal[m_historyRead]);
			bgfx::setTexture(7, m_shadowSampler, m_shadowMap);
			bgfx::setTexture(9, m_historyReceiverSampler, m_historyReceiver[m_historyRead]);
			bgfx::setUniform(m_shadowViewProjUniform, shadowViewProj, 6);
			bgfx::setUniform(m_shadowInfoUniform, shadowInfo);
			bgfx::setUniform(m_shadowOptionsUniform, shadowOptions);
			bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
			bgfx::setUniform(m_viewProjection, viewProjection);
			bgfx::setUniform(m_previousViewProjection, m_previousViewProj);
			bgfx::setUniform(m_lightPosition, lightPosition);
			bgfx::setUniform(m_lightColor, lightColor);
			bgfx::setUniform(m_frameInfo, frameInfo);
			bgfx::setUniform(m_screenInfo, screenInfo);
			bgfx::setUniform(m_sceneInfoUniform, sceneInfo);
			bgfx::setUniform(m_gridInfoUniform, gridInfo);
		};
		const auto setGrid = [](const bgfx::DynamicIndexBufferHandle* _buffers)
		{
			bgfx::setBuffer(kGridFragmentStage + 0, _buffers[0], bgfx::Access::Read);
			bgfx::setBuffer(kGridFragmentStage + 1, _buffers[1], bgfx::Access::Read);
			bgfx::setBuffer(kGridFragmentStage + 2, _buffers[2], bgfx::Access::Read);
		};

		// Temporal pass: fresh candidates, combined with reservoirs from last
		// frame's grid, into this frame's reservoirs. Last frame's cell sizes
		// were chosen from last frame's camera position.
		bgfx::setMarker("ReSTIR GI Temporal");
		bgfx::setViewName(1, "ReSTIR GI Temporal");
		bgfx::setViewRect(1, 0, 0, giWidth, giHeight);
		bgfx::setViewFrameBuffer(1, m_reservoirFrameBuffers[historyWrite]);
		bgfx::setViewTransform(1, nullptr, nullptr);
		setGiPassInputs();
		bgfx::setTexture(3, m_historySampleSampler, m_historySample[m_historyRead]);
		const bgfx::DynamicIndexBufferHandle previousGrid[] =
		{
			m_gridChecksums[m_historyRead], m_gridCounts[m_historyRead], m_gridEntries[m_historyRead],
		};
		setGrid(previousGrid);
		bgfx::setUniform(m_gridCameraUniform, previousCameraPosition);
		setFullscreenGeometry();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		bgfx::submit(1, m_temporalProgram);

		// Spatial pass: combined with other reservoirs from this frame's grid,
		// shaded and accumulated for the denoiser.
		bgfx::setMarker("ReSTIR GI Spatial");
		bgfx::setViewName(kSpatialView, "ReSTIR GI Spatial");
		bgfx::setViewRect(kSpatialView, 0, 0, giWidth, giHeight);
		bgfx::setViewFrameBuffer(kSpatialView, m_shadeFrameBuffers[historyWrite]);
		bgfx::setViewTransform(kSpatialView, nullptr, nullptr);
		setGiPassInputs();
		bgfx::setTexture(3, m_reservoirSampleSampler, m_historySample[historyWrite]);
		bgfx::setTexture(4, m_reservoirReceiverSampler, m_historyReceiver[historyWrite]);
		bgfx::setTexture(8, m_historyMomentsSampler, m_historyMoments[m_historyRead]);
		bgfx::setTexture(10, m_reservoirNormalSampler, m_historyNormal[historyWrite]);
		const bgfx::DynamicIndexBufferHandle currentGrid[] =
		{
			m_gridChecksums[historyWrite], m_gridCounts[historyWrite], m_gridEntries[historyWrite],
		};
		setGrid(currentGrid);
		bgfx::setUniform(m_gridCameraUniform, cameraPosition);
		setFullscreenGeometry();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		bgfx::submit(kSpatialView, m_spatialProgram);

		// Denoiser: variance estimate into filter target 0, then the a-trous
		// iterations. The first writes the filtered history for next frame,
		// the rest ping-pong between the two filter targets and end in target 0.
		if (m_denoiseEnabled)
		{
			bgfx::setMarker("Denoiser");
			for (uint8_t pass = 0; pass <= kAtrousIterations; ++pass)
			{
				const bgfx::ViewId viewId = bgfx::ViewId(kDenoiseViewBase + pass);
				bgfx::setViewName(viewId, pass == 0 ? "Denoise Variance" : "Denoise A-Trous");
				bgfx::setViewRect(viewId, 0, 0, giWidth, giHeight);
				bgfx::setViewTransform(viewId, nullptr, nullptr);

				bgfx::FrameBufferHandle output;
				if (pass == 0)
				{
					output = m_filterFrameBuffers[0];
					bgfx::setTexture(0, m_giColorSampler, m_historyColor[historyWrite]);
					bgfx::setTexture(1, m_giMomentsSampler, m_historyMoments[historyWrite]);
					bgfx::setTexture(2, m_giReceiverSampler, m_historyReceiver[historyWrite]);
					bgfx::setTexture(3, m_giNormalSampler, m_historyNormal[historyWrite]);
				}
				else
				{
					// Iteration 1 reads target 0 and writes the history; 2 reads the
					// history and writes target 1; then 1 -> 0, 0 -> 1, 1 -> 0.
					const uint8_t iteration = uint8_t(pass - 1);
					const bgfx::TextureHandle input = iteration == 0
						? m_filterTextures[0]
						: iteration == 1
						? m_filteredHistory[historyWrite]
						: m_filterTextures[(iteration - 1) & 1];
					output = iteration == 0
						? m_filteredHistoryFrameBuffers[historyWrite]
						: m_filterFrameBuffers[iteration & 1];
					bgfx::setTexture(0, m_filterInputSampler, input);
					bgfx::setTexture(1, m_giReceiverSampler, m_historyReceiver[historyWrite]);
					bgfx::setTexture(2, m_giNormalSampler, m_historyNormal[historyWrite]);
					const float atrousInfo[] = { float(1 << iteration), 0.0f, 0.0f, 0.0f };
					bgfx::setUniform(m_atrousInfoUniform, atrousInfo);
				}

				bgfx::setViewFrameBuffer(viewId, output);
				bgfx::setUniform(m_screenInfo, screenInfo);
				bgfx::setUniform(m_sceneInfoUniform, sceneInfo);
				setFullscreenGeometry();
				bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
				bgfx::submit(viewId, pass == 0 ? m_varianceProgram : m_atrousProgram);
			}
		}

		bgfx::setMarker("Composite Pass");
		bgfx::setViewRect(2, 0, 0, uint16_t(m_width), uint16_t(m_height));
		bgfx::setViewFrameBuffer(2, BGFX_INVALID_HANDLE);
		bgfx::setViewTransform(2, nullptr, nullptr);
		bgfx::setTexture(0, m_albedoSampler, m_gbufferAlbedo);
		bgfx::setTexture(1, m_normalSampler, m_gbufferNormal);
		bgfx::setTexture(2, m_depthSampler, m_gbufferDepth);
		bgfx::setTexture(3, m_giSampler
			, m_denoiseEnabled ? m_filterTextures[0] : m_historyColor[historyWrite]);
		bgfx::setTexture(4, m_shadowSampler, m_shadowMap);
		bgfx::setUniform(m_shadowViewProjUniform, shadowViewProj, 6);
		bgfx::setUniform(m_shadowInfoUniform, shadowInfo);
		bgfx::setUniform(m_shadowOptionsUniform, shadowOptions);
		bgfx::setUniform(m_inverseViewProjection, inverseViewProjection);
		bgfx::setUniform(m_lightPosition, lightPosition);
		bgfx::setUniform(m_lightColor, lightColor);
		bgfx::setUniform(m_cameraPosition, cameraPosition);
		bgfx::setUniform(m_screenInfo, screenInfo);
		bgfx::setUniform(m_sceneInfoUniform, sceneInfo);
		setFullscreenGeometry();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		bgfx::submit(2, m_compositeProgram);

		bx::memCopy(m_previousViewProj, viewProjection, sizeof(m_previousViewProj));
		m_previousEye = eye;
		m_historyRead = historyWrite;
		m_historyValid = true;
		++m_frameNumber;

		bgfx::setMarker("Present");
		bgfx::frame();
		return true;
	}

private:
	// True on the frame _key goes down; _wasDown remembers its state.
	static bool keyPressed(entry::Key::Enum _key, bool& _wasDown)
	{
		const bool down = inputGetKeyState(_key);
		const bool pressed = down && !_wasDown;
		_wasDown = down;
		return pressed;
	}

	void destroyModel()
	{
		destroyIfValid(m_vbh);
		destroyIfValid(m_ibh);
		for (bgfx::TextureHandle& texture : m_modelTextures)
		{
			destroyIfValid(texture);
		}
		m_modelTextures.clear();
	}

	// Binds a material's textures (white where it has none) and parameters
	// for the next draw, in the G-buffer or shadow pass.
	void setMaterial(uint32_t _material)
	{
		const SceneMaterial& material = m_materials[_material];
		const auto texture = [this](int32_t _image)
		{
			return _image >= 0 ? m_modelTextures[_image] : m_whiteTexture;
		};

		bgfx::setTexture(0, m_baseColorSampler, texture(material.m_baseColorImage));
		bgfx::setTexture(1, m_metallicRoughnessSampler, texture(material.m_metallicRoughnessImage));
		const float materialInfo[] =
		{
			material.m_alphaCutoff,
			material.m_metallicTextureMissing ? 0.0f : 1.0f,
			material.m_twoSidedMask ? 1.0f : 0.0f,
			0.0f,
		};
		bgfx::setUniform(m_materialInfoUniform, materialInfo);
	}

	// Makes a loaded mesh the current model: replaces the scene buffers and
	// textures, places the camera and the light. Last frame's reservoirs and
	// hash grid describe the old scene, so the history restarts.
	void setModel(std::size_t _index, const SceneMesh& _mesh)
	{
		destroyModel();

		// Repeat wrapping and trilinear, anisotropic filtering (glTF samplers
		// are not read; repeat is their default).
		const uint64_t textureFlags = BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC;
		for (const SceneImage& image : _mesh.m_images)
		{
			m_modelTextures.push_back(bgfx::createTexture2D(
				  image.m_width, image.m_height, true, 1, bgfx::TextureFormat::RGBA8, textureFlags
				, bgfx::copy(image.m_data.data(), uint32_t(image.m_data.size()))
				));
		}
		m_materials = _mesh.m_materials;
		m_draws = _mesh.m_draws;

		m_vbh = bgfx::createVertexBuffer(
			  bgfx::copy(_mesh.m_vertices.data(), uint32_t(_mesh.m_vertices.size() * sizeof(SceneVertex)))
			, SceneVertex::ms_layout
			);
		m_ibh = bgfx::createIndexBuffer(
			  bgfx::copy(_mesh.m_indices.data(), uint32_t(_mesh.m_indices.size() * sizeof(uint32_t)))
			, BGFX_BUFFER_INDEX32
			);

		m_scene = describeScene(_mesh);
		float cameraDistance = kInitialCameraDistance;
		float cameraYaw = kInitialCameraYaw;
		for (const BundledModel& bundled : kBundledModels)
		{
			if (m_modelPaths[_index] == bundled.m_path)
			{
				cameraDistance = bundled.m_cameraDistance;
				cameraYaw = bundled.m_cameraYaw;
			}
		}
		// Start at that distance and angle from the centre of the scene,
		// looking at it.
		m_cameraYaw = cameraYaw;
		m_cameraPitch = kInitialCameraPitch;
		m_eye = bx::sub(m_scene.m_center, bx::mul(cameraForward(), cameraDistance * m_scene.m_radius));
		m_moveSpeedScale = 1.0f;
		m_modelIndex = _index;
		m_historyValid = false;
	}

	void switchModel(std::size_t _index)
	{
		try
		{
			setModel(_index, loadScene(m_modelPaths[_index]));
			m_modelError.clear();
		}
		catch (const std::exception& _error)
		{
			std::fprintf(stderr, "%s\n", _error.what());
			m_modelError = "Could not load " + m_modelPaths[_index] + " (see the log)";
		}
	}

	void setFullscreenGeometry()
	{
		bgfx::setVertexBuffer(0, m_fullscreenVbh);
		bgfx::setIndexBuffer(m_fullscreenIbh);
	}

	// The frame buffers own their textures; createRenderTargets makes new ones.
	void destroyRenderTargets()
	{
		destroyIfValid(m_gbuffer);
		for (uint8_t idx = 0; idx < 2; ++idx)
		{
			destroyIfValid(m_reservoirFrameBuffers[idx]);
			destroyIfValid(m_shadeFrameBuffers[idx]);
			destroyIfValid(m_filteredHistoryFrameBuffers[idx]);
			destroyIfValid(m_filterFrameBuffers[idx]);
		}
	}

	void createShadowMap()
	{
		const uint64_t samplerFlags = BGFX_SAMPLER_MIN_POINT
			| BGFX_SAMPLER_MAG_POINT
			| BGFX_SAMPLER_MIP_POINT
			| BGFX_SAMPLER_U_CLAMP
			| BGFX_SAMPLER_V_CLAMP;
		if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::R32F, BGFX_TEXTURE_RT | samplerFlags))
		{
			throw std::runtime_error("Shadows require R32F render targets.");
		}

		const uint16_t atlasWidth = 3 * kShadowFaceSize;
		const uint16_t atlasHeight = 2 * kShadowFaceSize;
		m_shadowMap = bgfx::createTexture2D(
			  atlasWidth, atlasHeight, false, 1, bgfx::TextureFormat::R32F
			, BGFX_TEXTURE_RT | samplerFlags
			);
		const bgfx::TextureFormat::Enum depthFormat =
			  bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT_WRITE_ONLY)
			? bgfx::TextureFormat::D32F
			: bgfx::TextureFormat::D24
			;
		const bgfx::TextureHandle shadowTextures[] =
		{
			m_shadowMap,
			bgfx::createTexture2D(atlasWidth, atlasHeight, false, 1, depthFormat, BGFX_TEXTURE_RT_WRITE_ONLY),
		};
		m_shadowFrameBuffer = bgfx::createFrameBuffer(BX_COUNTOF(shadowTextures), shadowTextures, true);
		if (!bgfx::isValid(m_shadowFrameBuffer))
		{
			throw std::runtime_error("Failed to create the shadow map.");
		}
	}

	void createRenderTargets(uint32_t _width, uint32_t _height)
	{
		destroyRenderTargets();

		const uint64_t samplerFlags = BGFX_SAMPLER_MIN_POINT
			| BGFX_SAMPLER_MAG_POINT
			| BGFX_SAMPLER_MIP_POINT
			| BGFX_SAMPLER_U_CLAMP
			| BGFX_SAMPLER_V_CLAMP;
		const uint16_t giWidth = uint16_t(std::max(1u, _width / 2));
		const uint16_t giHeight = uint16_t(std::max(1u, _height / 2));

		if (bgfx::getCaps()->limits.maxFBAttachments < 3
		||  !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT)
		||  !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA32F, BGFX_TEXTURE_RT))
		{
			throw std::runtime_error(
				"ReSTIR GI requires three framebuffer attachments and RGBA16F/RGBA32F render targets.");
		}

		const auto createTarget = [&](uint32_t _targetWidth, uint32_t _targetHeight, bgfx::TextureFormat::Enum _format)
		{
			return bgfx::createTexture2D(
				  uint16_t(_targetWidth), uint16_t(_targetHeight), false, 1, _format
				, BGFX_TEXTURE_RT | samplerFlags
				);
		};

		m_gbufferAlbedo = createTarget(_width, _height, bgfx::TextureFormat::BGRA8);
		m_gbufferNormal = createTarget(_width, _height, bgfx::TextureFormat::BGRA8);
		m_gbufferDepth = createTarget(_width, _height
			, bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT | samplerFlags)
			? bgfx::TextureFormat::D32F
			: bgfx::TextureFormat::D24
			);
		const bgfx::TextureHandle gbufferTextures[] =
		{
			m_gbufferAlbedo, m_gbufferNormal, m_gbufferDepth,
		};
		m_gbuffer = bgfx::createFrameBuffer(BX_COUNTOF(gbufferTextures), gbufferTextures, true);

		for (uint8_t idx = 0; idx < 2; ++idx)
		{
			// Per frame parity, the temporal pass writes the reservoirs (sample
			// position + M, receiver position + W, receiver normal, which the
			// pairwise MIS weights need), and the spatial pass writes the
			// accumulated (demodulated) estimate and the luminance moments +
			// history length (needed by the denoiser). Positions need full
			// float precision, and all of them must be point sampled.
			m_historyColor[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
			m_historySample[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA32F);
			m_historyReceiver[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA32F);
			m_historyNormal[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
			m_historyMoments[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
			const bgfx::TextureHandle reservoirTextures[] =
			{
				m_historySample[idx], m_historyReceiver[idx], m_historyNormal[idx],
			};
			m_reservoirFrameBuffers[idx] = bgfx::createFrameBuffer(
				BX_COUNTOF(reservoirTextures), reservoirTextures, true);
			const bgfx::TextureHandle shadeTextures[] =
			{
				m_historyColor[idx], m_historyMoments[idx],
			};
			m_shadeFrameBuffers[idx] = bgfx::createFrameBuffer(
				BX_COUNTOF(shadeTextures), shadeTextures, true);

			// Denoiser: the filtered history (one per frame parity, like the
			// rest of the history) and two ping-pong targets.
			m_filteredHistory[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
			m_filteredHistoryFrameBuffers[idx] = bgfx::createFrameBuffer(1, &m_filteredHistory[idx], true);
			m_filterTextures[idx] = createTarget(giWidth, giHeight, bgfx::TextureFormat::RGBA16F);
			m_filterFrameBuffers[idx] = bgfx::createFrameBuffer(1, &m_filterTextures[idx], true);
		}

		if (!bgfx::isValid(m_gbuffer)
		||  !bgfx::isValid(m_reservoirFrameBuffers[0]) || !bgfx::isValid(m_reservoirFrameBuffers[1])
		||  !bgfx::isValid(m_shadeFrameBuffers[0]) || !bgfx::isValid(m_shadeFrameBuffers[1])
		||  !bgfx::isValid(m_filteredHistoryFrameBuffers[0]) || !bgfx::isValid(m_filteredHistoryFrameBuffers[1])
		||  !bgfx::isValid(m_filterFrameBuffers[0]) || !bgfx::isValid(m_filterFrameBuffers[1]))
		{
			throw std::runtime_error("Failed to create the G-buffer or ReSTIR GI history targets.");
		}

		m_targetWidth = _width;
		m_targetHeight = _height;
		m_historyRead = 0;
		m_historyValid = false;
		m_frameNumber = 0;
		bx::mtxIdentity(m_previousViewProj);
	}

	// Direction the camera looks in. Positive pitch looks down; yaw 0 looks
	// along -z.
	bx::Vec3 cameraForward() const
	{
		const float cosPitch = std::cos(m_cameraPitch);
		return
		{
			-cosPitch * std::sin(m_cameraYaw),
			-std::sin(m_cameraPitch),
			-cosPitch * std::cos(m_cameraYaw),
		};
	}

	// Fly camera: dragging with the left button turns it, W/S move along the
	// view direction, A/D sideways, Shift/Ctrl straight up and down. The
	// wheel scales the speed, which is otherwise relative to the scene size.
	void updateCameraInput()
	{
		const int64_t now = bx::getHPCounter();
		const float deltaTime = m_lastFrameTime == 0
			? 0.0f
			: std::min(float(double(now - m_lastFrameTime) / double(bx::getHPFrequency())), 0.1f);
		m_lastFrameTime = now;

		const bool isDragging = m_mouseState.m_buttons[entry::MouseButton::Left] != 0;
		if (isDragging && m_isDragging)
		{
			const int32_t deltaX = m_mouseState.m_mx - m_lastMouseX;
			const int32_t deltaY = m_mouseState.m_my - m_lastMouseY;
			const float sensitivity = 0.004f;

			m_cameraYaw -= float(deltaX) * sensitivity;
			m_cameraPitch = std::clamp(m_cameraPitch + float(deltaY) * sensitivity, -1.5f, 1.5f);
		}

		m_isDragging = isDragging;
		m_lastMouseX = m_mouseState.m_mx;
		m_lastMouseY = m_mouseState.m_my;

		const int32_t wheelDelta = m_mouseState.m_mz - m_lastMouseWheel;
		m_lastMouseWheel = m_mouseState.m_mz;
		if (wheelDelta != 0)
		{
			m_moveSpeedScale = std::clamp(m_moveSpeedScale * std::pow(1.25f, float(wheelDelta)), 0.05f, 20.0f);
		}

		const bx::Vec3 forward = cameraForward();
		const bx::Vec3 right = bx::normalize(bx::cross(forward, { 0.0f, 1.0f, 0.0f }));
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

		bx::Vec3 move = bx::add(bx::mul(forward, forwardAmount), bx::mul(right, rightAmount));
		move = bx::add(move, { 0.0f, upAmount, 0.0f });
		const float moveLength = bx::length(move);
		if (moveLength > 0.0f)
		{
			const float speed = kCameraSpeed * m_scene.m_radius * m_moveSpeedScale;
			m_eye = bx::add(m_eye, bx::mul(move, speed * deltaTime / moveLength));
		}
	}

	entry::MouseState m_mouseState;
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_debug = 0;
	uint32_t m_reset = 0;
	bgfx::VertexBufferHandle m_vbh = BGFX_INVALID_HANDLE;
	bgfx::IndexBufferHandle m_ibh = BGFX_INVALID_HANDLE;
	bgfx::VertexBufferHandle m_fullscreenVbh = BGFX_INVALID_HANDLE;
	bgfx::IndexBufferHandle m_fullscreenIbh = BGFX_INVALID_HANDLE;
	bgfx::FrameBufferHandle m_gbuffer = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_gbufferAlbedo = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_gbufferNormal = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_gbufferDepth = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle m_historyColor[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historySample[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historyReceiver[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_historyNormal[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::FrameBufferHandle m_reservoirFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE }; // sample, receiver, normal
	bgfx::FrameBufferHandle m_shadeFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };     // colour, moments
	uint8_t m_historyRead = 0;
	bool m_historyValid = false;
	uint32_t m_targetWidth = 0;
	uint32_t m_targetHeight = 0;
	bgfx::ProgramHandle m_geometryProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_temporalProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_spatialProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_compositeProgram = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_lightPosition = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_lightColor = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_cameraPosition = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_albedoSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_normalSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_depthSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_historySampleSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_historyReceiverSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_historyColorSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_historyNormalSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_giSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_inverseViewProjection = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_viewProjection = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_previousViewProjection = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_frameInfo = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_screenInfo = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_sceneInfoUniform = BGFX_INVALID_HANDLE;
	SceneInfo m_scene;
	float m_cameraYaw = kInitialCameraYaw;
	float m_cameraPitch = kInitialCameraPitch;
	float m_previousViewProj[16] = {};
	uint32_t m_frameNumber = 0;
	int32_t m_lastMouseX = 0;
	int32_t m_lastMouseY = 0;
	int32_t m_lastMouseWheel = 0;
	bool m_isDragging = false;
	bool m_multipleBounces = true;
	bool m_bounceKeyDown = false;
	bgfx::TextureHandle m_shadowMap = BGFX_INVALID_HANDLE;
	bgfx::FrameBufferHandle m_shadowFrameBuffer = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_shadowProgram = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_shadowSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_shadowViewProjUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_shadowInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_shadowOptionsUniform = BGFX_INVALID_HANDLE;
	bool m_shadowsEnabled = true;
	bool m_shadowKeyDown = false;
	bgfx::TextureHandle m_historyMoments[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_filteredHistory[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::FrameBufferHandle m_filteredHistoryFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::TextureHandle m_filterTextures[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::FrameBufferHandle m_filterFrameBuffers[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::ProgramHandle m_varianceProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_atrousProgram = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_historyMomentsSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_giColorSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_giMomentsSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_giReceiverSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_giNormalSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_filterInputSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_atrousInfoUniform = BGFX_INVALID_HANDLE;
	bool m_denoiseEnabled = true;
	bool m_denoiseKeyDown = false;

	// World-space hash grid, one per frame parity like the reservoirs.
	bgfx::DynamicIndexBufferHandle m_gridChecksums[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::DynamicIndexBufferHandle m_gridCounts[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::DynamicIndexBufferHandle m_gridEntries[2] = { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
	bgfx::ProgramHandle m_gridClearProgram = BGFX_INVALID_HANDLE;
	bgfx::ProgramHandle m_gridInsertProgram = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_reservoirSampleSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_reservoirReceiverSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_reservoirNormalSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_gridInfoUniform = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_gridCameraUniform = BGFX_INVALID_HANDLE;
	bx::Vec3 m_previousEye = { 0.0f, 0.0f, 0.0f };
	bool m_worldSpaceReuse = true;
	bool m_gridKeyDown = false;

	// Models M cycles through, and the one shown.
	std::vector<std::string> m_modelPaths;
	std::size_t m_modelIndex = 0;
	std::string m_modelError;
	bool m_modelKeyDown = false;

	// The current model's textures, materials and per-material draws.
	std::vector<bgfx::TextureHandle> m_modelTextures;
	std::vector<SceneMaterial> m_materials;
	std::vector<SceneDraw> m_draws;
	bgfx::TextureHandle m_whiteTexture = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_baseColorSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_metallicRoughnessSampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle m_materialInfoUniform = BGFX_INVALID_HANDLE;

	// Fly camera.
	bx::Vec3 m_eye = { 0.0f, 0.0f, 0.0f };
	float m_moveSpeedScale = 1.0f;
	int64_t m_lastFrameTime = 0;
};

} // namespace

ENTRY_IMPLEMENT_MAIN(
	  ExampleRestirGi
	, "restir-gi"
	, "ReSTIR global illumination starter."
	, "https://github.com/bkaradzic/bgfx"
	);
