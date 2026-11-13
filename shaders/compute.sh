/*
 * Shared code of the ReSTIR compute passes (cs_restir_*.sc, cs_di_*.sc).
 *
 * Each runs in groups of 8x8 threads, one per pixel. Include this file before
 * restir.sh or di.sh: it gives the BVH traversal its stack in shared memory
 * (bvh.sh). Define, before including it:
 *
 *   COMPUTE_GROUP_SUM    for groupSum(), a sum over the group
 *   COMPUTE_GROUP_QUEUE  for queueSlot() and s_queueCount, slots in a queue
 *                        in shared memory
 *
 * Every thread of the group must call groupSum() and every GROUP_SYNC(),
 * including threads outside the image: they are barriers.
 *
 * Wave intrinsics: shaders compiled with RESTIR_WAVE_OPS (the *_wave
 * variants, SPIR-V 1.3 for Vulkan only; see CMakeLists.txt and the "Wave
 * intrinsics" setting) use the HLSL Shader Model 6 wave operations, which
 * bgfx's shader compiler (glslang's HLSL front end) turns into Vulkan subgroup
 * operations. They assume that a group's waves are made of consecutive
 * threads (gl_LocalInvocationIndex), as on every GPU we know of.
 */

#ifndef COMPUTE_SH_HEADER_GUARD
#define COMPUTE_SH_HEADER_GUARD

#define kGroupSize 8     // threads along x and y
#define kGroupThreads 64 // kGroupSize * kGroupSize
#define BVH_SHARED_STACK 64

#if BGFX_SHADER_LANGUAGE_GLSL
#	define GROUP_SYNC() memoryBarrierShared(); barrier()
#else
#	define GROUP_SYNC() GroupMemoryBarrierWithGroupSync()
#endif

// The pixel of this thread, and whether it lies in an image of _size pixels.
ivec2 computePixel(uvec3 _globalId)
{
	return ivec2(int(_globalId.x), int(_globalId.y));
}

bool insideImage(ivec2 _pixel, vec2 _size)
{
	return _pixel.x < int(_size.x) && _pixel.y < int(_size.y);
}

#ifdef COMPUTE_GROUP_SUM
SHARED vec2 s_groupSum[kGroupThreads];

// The sum of _value over the group's threads (_thread: gl_LocalInvocationIndex).
vec2 groupSum(vec2 _value, int _thread)
{
#	ifdef RESTIR_WAVE_OPS
	// One sum per wave, then the waves' sums.
	vec2 waveSum = WaveActiveSum(_value);
	int laneCount = int(WaveGetLaneCount());
	if (WaveIsFirstLane())
	{
		s_groupSum[_thread / laneCount] = waveSum;
	}
	GROUP_SYNC();
	vec2 total = vec2_splat(0.0);
	for (int wave = 0; wave < kGroupThreads / laneCount; ++wave)
	{
		total += s_groupSum[wave];
	}
#	else
	// A tree reduction in shared memory.
	s_groupSum[_thread] = _value;
	GROUP_SYNC();
	for (int stride = kGroupThreads / 2; stride > 0; stride /= 2)
	{
		if (_thread < stride)
		{
			s_groupSum[_thread] += s_groupSum[_thread + stride];
		}
		GROUP_SYNC();
	}
	vec2 total = s_groupSum[0];
#	endif
	GROUP_SYNC(); // before s_groupSum is written again
	return total;
}
#endif // COMPUTE_GROUP_SUM

#ifdef COMPUTE_GROUP_QUEUE
SHARED uint s_queueCount;

// A slot in the group's queue for each thread that pushes; meaningless for
// the others. Thread 0 resets s_queueCount, between GROUP_SYNC()s.
uint queueSlot(bool _push)
{
#	ifdef RESTIR_WAVE_OPS
	// One atomic per wave: the wave's pushing lanes take consecutive slots.
	uint offset = WavePrefixCountBits(_push);
	uint count = WaveActiveCountBits(_push);
	uint base = 0u;
	if (WaveIsFirstLane())
	{
		atomicFetchAndAdd(s_queueCount, count, base);
	}
	return WaveReadLaneFirst(base) + offset;
#	else
	uint slot = 0u;
	if (_push)
	{
		atomicFetchAndAdd(s_queueCount, 1u, slot);
	}
	return slot;
#	endif
}
#endif // COMPUTE_GROUP_QUEUE

#endif // COMPUTE_SH_HEADER_GUARD
