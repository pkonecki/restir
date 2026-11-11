/*
 * World-space hash grid of reservoirs, after Boissé, "World-Space
 * Spatiotemporal Reservoir Reuse for Ray-Traced Global Illumination"
 * (SIGGRAPH Asia 2021 Technical Communications).
 *
 * Every GI pixel's reservoir is listed in the cell of the hash grid that
 * holds its receiver (the visible surface point it shades). Reuse then picks
 * reservoirs from the receiver's cell instead of from a screen-space disk, so
 * neighbours are found by where they are in the scene, whatever their place
 * on screen. Last frame's grid indexes last frame's reservoirs, which gives
 * temporal reuse without reprojection.
 *
 * Cells:
 *   - The cell size grows with the distance to the camera, so that a cell
 *     covers roughly the same number of pixels anywhere on screen. Sizes are
 *     the smallest size times a power of two; the size is part of the key, so
 *     cells of different sizes never mix.
 *   - The receiver's dominant normal axis is part of the key as well, so the
 *     two walls meeting at a corner never share a cell.
 *   - Positions are jittered in the tangent plane by up to half a cell, both
 *     when a reservoir is inserted and on every lookup. Without that, every
 *     pixel of a cell would reuse the same set of reservoirs and the cell
 *     boundaries would show up as blocks the denoiser cannot remove.
 *
 * Storage: an open-addressing table of kGridTableSize slots. One hash of the
 * key picks the first slot, linear probing resolves collisions, and a second,
 * independent 32-bit hash of the key (the checksum) tells which cell a slot
 * holds. Each slot lists up to kGridCellCapacity pixel indices; reservoirs
 * beyond that are simply not listed, which does not bias anything since the
 * choice does not depend on what the reservoirs hold.
 *
 * Before including this file, define the stages of the three buffers as
 * plain numbers (GRID_CHECKSUM_STAGE, GRID_COUNT_STAGE, GRID_ENTRY_STAGE; the
 * register macros cannot take expressions) and, in the passes that only read
 * the grid, GRID_READ_ONLY.
 */

#ifndef HASHGRID_SH_HEADER_GUARD
#define HASHGRID_SH_HEADER_GUARD

#ifdef GRID_READ_ONLY
BUFFER_RO(b_gridChecksums, uint, GRID_CHECKSUM_STAGE);
BUFFER_RO(b_gridCounts,    uint, GRID_COUNT_STAGE);
BUFFER_RO(b_gridEntries,   uint, GRID_ENTRY_STAGE);
#else
BUFFER_RW(b_gridChecksums, uint, GRID_CHECKSUM_STAGE);
BUFFER_RW(b_gridCounts,    uint, GRID_COUNT_STAGE);
BUFFER_RW(b_gridEntries,   uint, GRID_ENTRY_STAGE);
#endif

uniform vec4 u_gridInfo;   // x: smallest cell size, y: world size of a GI pixel at unit distance, z: cell size in GI pixels, w: world-space reuse enabled
uniform vec4 u_gridCamera; // xyz: camera position the cell sizes of this grid are chosen from

// Keep in sync with main.cpp.
#define kGridTableSize    65536u
#define kGridTableMask    65535u
#define kGridCellCapacity 32u
#define kGridMaxProbes    8u
#define kGridInvalid      0xffffffffu

uint gridHash(uint _value)
{
	// PCG
	uint state = _value * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint gridChecksumHash(uint _value)
{
	// xxHash32 avalanche
	_value ^= _value >> 15u;
	_value *= 2246822519u;
	_value ^= _value >> 13u;
	_value *= 3266489917u;
	_value ^= _value >> 16u;
	return _value;
}

// Size of the cells around _position: about u_gridInfo.z GI pixels across,
// rounded down to the smallest size times a power of two.
float gridCellSize(vec3 _position)
{
	float target = distance(_position, u_gridCamera.xyz) * u_gridInfo.y * u_gridInfo.z;
	float level = floor(log2(max(target / u_gridInfo.x, 1.0)));
	return u_gridInfo.x * exp2(level);
}

uint gridNormalBucket(vec3 _normal)
{
	vec3 absolute = abs(_normal);
	uint bucket = _normal.z >= 0.0 ? 4u : 5u;
	if (absolute.x >= absolute.y && absolute.x >= absolute.z)
	{
		bucket = _normal.x >= 0.0 ? 0u : 1u;
	}
	else if (absolute.y >= absolute.z)
	{
		bucket = _normal.y >= 0.0 ? 2u : 3u;
	}

	return bucket;
}

// Moves a position by up to half a cell in each direction of its tangent plane.
vec3 gridJitter(vec3 _position, vec3 _normal, float _cellSize, float _random0, float _random1)
{
	vec3 tangent = normalize(abs(_normal.y) < 0.99
		? cross(_normal, vec3(0.0, 1.0, 0.0))
		: cross(_normal, vec3(1.0, 0.0, 0.0)));
	vec3 bitangent = cross(_normal, tangent);
	return _position + (tangent * (_random0 - 0.5) + bitangent * (_random1 - 0.5)) * _cellSize;
}

// First table slot and checksum of the cell holding _position.
void gridKey(vec3 _position, vec3 _normal, float _cellSize, out uint _slot, out uint _checksum)
{
	ivec3 cell = ivec3(floor(_position / _cellSize));
	uint level = floatBitsToUint(_cellSize);
	uint bucket = gridNormalBucket(_normal);

	_slot = gridHash(level + gridHash(bucket + gridHash(uint(cell.x) + gridHash(uint(cell.y) + gridHash(uint(cell.z)))))) & kGridTableMask;
	uint checksum = gridChecksumHash(level ^ gridChecksumHash(bucket ^ gridChecksumHash(
		uint(cell.x) ^ gridChecksumHash(uint(cell.y) ^ gridChecksumHash(uint(cell.z) + 0x9e3779b9u)))));
	// Zero marks an empty slot.
	_checksum = max(checksum, 1u);
}

#ifdef GRID_READ_ONLY

// The slot holding the cell, or kGridInvalid if no reservoir was inserted there.
uint gridFindCell(uint _slot, uint _checksum)
{
	for (uint probe = 0u; probe < kGridMaxProbes; ++probe)
	{
		uint slot = (_slot + probe) & kGridTableMask;
		uint stored = b_gridChecksums[slot];
		if (stored == _checksum)
		{
			return slot;
		}

		if (stored == 0u)
		{
			break;
		}
	}

	return kGridInvalid;
}

// Picks one reservoir at random from the cell around a jittered copy of the
// receiver position. Returns its GI pixel index, or kGridInvalid if the cell
// is empty. Always uses three random numbers.
uint gridPickEntry(vec3 _position, vec3 _normal, float _random0, float _random1, float _random2)
{
	float cellSize = gridCellSize(_position);
	vec3 jittered = gridJitter(_position, _normal, cellSize, _random0, _random1);

	uint slot;
	uint checksum;
	gridKey(jittered, _normal, cellSize, slot, checksum);
	uint cell = gridFindCell(slot, checksum);
	if (cell == kGridInvalid)
	{
		return kGridInvalid;
	}

	uint count = min(b_gridCounts[cell], kGridCellCapacity);
	if (count == 0u)
	{
		return kGridInvalid;
	}

	uint index = min(uint(_random2 * float(count)), count - 1u);
	return b_gridEntries[cell * kGridCellCapacity + index];
}

#else

// Lists _entry in the cell, claiming a slot for the cell if it has none yet.
void gridInsert(uint _slot, uint _checksum, uint _entry)
{
	for (uint probe = 0u; probe < kGridMaxProbes; ++probe)
	{
		uint slot = (_slot + probe) & kGridTableMask;
		uint previous;
		atomicFetchCompareExchange(b_gridChecksums[slot], 0u, _checksum, previous);
		if (previous == 0u || previous == _checksum)
		{
			uint index;
			atomicFetchAndAdd(b_gridCounts[slot], 1u, index);
			if (index < kGridCellCapacity)
			{
				b_gridEntries[slot * kGridCellCapacity + index] = _entry;
			}
			return;
		}
	}
}

#endif // GRID_READ_ONLY

#endif // HASHGRID_SH_HEADER_GUARD
