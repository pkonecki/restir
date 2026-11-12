#include "scene.h"

#include "entry/entry.h"

#include <bimg/decode.h>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

bgfx::VertexLayout SceneVertex::ms_layout;

namespace
{

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

} // namespace

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
