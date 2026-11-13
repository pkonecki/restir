/*
 * Empties the hash grid (see hashgrid.sh) before this frame's reservoirs are
 * inserted. Only the checksums and counts need clearing: entries past a
 * cell's count are never read.
 */

#include <bgfx_compute.sh>

#define GRID_STAGE 0
#include "hashgrid.sh"

NUM_THREADS(64, 1, 1)
void main()
{
	uint slot = gl_GlobalInvocationID.x;
	if (slot < kGridTableSize)
	{
		b_grid[slot] = 0u;
		b_grid[kGridCountBase + slot] = 0u;
	}
}
