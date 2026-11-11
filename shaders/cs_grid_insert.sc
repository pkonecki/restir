/*
 * Lists every GI pixel in the hash grid cell of its receiver (see
 * hashgrid.sh), at a position jittered within its tangent plane. The
 * reservoirs themselves stay in the per-pixel reservoir targets; the grid only
 * holds pixel indices.
 *
 * This only needs the G-buffer, so it runs before the temporal pass, and the
 * temporal pass's reservoirs (written at the same pixels) are what the cells
 * point to by the time the spatial pass and the next frame read them.
 */

#include <bgfx_compute.sh>
#include "shaderlib.sh"

#define GRID_CHECKSUM_STAGE 0
#define GRID_COUNT_STAGE    1
#define GRID_ENTRY_STAGE    2
#include "hashgrid.sh"

SAMPLER2D(s_normal, 3);
SAMPLER2D(s_depth, 4);

uniform mat4 u_sceneInvViewProj;
uniform vec4 u_frameInfo;  // x: frame index
uniform vec4 u_screenInfo; // xy: GI target size, zw: GI texel size

#define kSkyDepth 0.99999

uint pcg(uint _value)
{
	uint state = _value * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float nextRandom(inout uint _state)
{
	_state = pcg(_state);
	return float(_state >> 8u) * (1.0 / 16777216.0);
}

NUM_THREADS(8, 8, 1)
void main()
{
	uvec2 pixel = gl_GlobalInvocationID.xy;
	uint width = uint(u_screenInfo.x);
	if (pixel.x >= width || pixel.y >= uint(u_screenInfo.y))
	{
		return;
	}

	// The same texel centres the GI fragment passes run at.
	vec2 uv = (vec2(pixel) + 0.5) * u_screenInfo.zw;
	float depth = texture2DLod(s_depth, uv, 0.0).x;
	if (depth >= kSkyDepth)
	{
		return;
	}

	vec3 clipPosition = vec3(uv * 2.0 - 1.0, toClipSpaceDepth(depth));
	clipPosition.y = toClipSpaceY(clipPosition.y);
	vec3 position = clipToWorld(u_sceneInvViewProj, clipPosition);
	vec3 normal = normalize(texture2DLod(s_normal, uv, 0.0).xyz * 2.0 - 1.0);

	// A different stream from the GI passes' (salted), so the jitter does not
	// correlate with the rays traced from the same pixel.
	uint rng = pcg(pixel.x + pcg(pixel.y + pcg(uint(u_frameInfo.x) ^ 0x5bd1e995u)));
	float cellSize = gridCellSize(position);
	vec3 jittered = gridJitter(position, normal, cellSize, nextRandom(rng), nextRandom(rng));

	uint slot;
	uint checksum;
	gridKey(jittered, normal, cellSize, slot, checksum);
	gridInsert(slot, checksum, pixel.y * width + pixel.x);
}
