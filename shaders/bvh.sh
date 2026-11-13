/*
 * Ray tracing against the scene's bounding volume hierarchy (src/bvh.cpp
 * describes the buffer layout), for the rays and visibility tests the depth
 * buffer cannot answer: geometry off screen, or hidden behind other geometry.
 *
 * Define BVH_STAGE as a plain number before including this file.
 */

#ifndef BVH_SH_HEADER_GUARD
#define BVH_SH_HEADER_GUARD

BUFFER_RO(b_scene, vec4, BVH_STAGE);

uniform vec4 u_bvhInfo; // x: first triangle (in vec4s), z: node count (0: no BVH), w: ray mode

// Keep in sync with RayMode in settings.h.
#define RAY_MODE_SCREEN 0 // depth buffer only
#define RAY_MODE_HYBRID 1 // depth buffer, then the BVH for rays it does not answer
#define RAY_MODE_BVH    2 // the BVH for every ray and visibility test

#define kRayMode int(u_bvhInfo.w)
#define kBvhShortStack 8 // entries per thread of the shared-memory traversal stack (a power of two)
#define kBvhMaxSteps 2048 // node visits, a guard against a broken tree

bool bvhAvailable()
{
	return u_bvhInfo.z > 0.5;
}

// Distance along the ray to the box, or a negative value if it misses
// within _maxDistance.
float intersectBox(vec3 _origin, vec3 _inverseDirection, vec3 _min, vec3 _max, float _maxDistance)
{
	vec3 t0 = (_min - _origin) * _inverseDirection;
	vec3 t1 = (_max - _origin) * _inverseDirection;
	vec3 near = min(t0, t1);
	vec3 far = max(t0, t1);
	float enter = max(max(near.x, near.y), max(near.z, 0.0));
	float leave = min(min(far.x, far.y), min(far.z, _maxDistance));
	return enter <= leave ? enter : -1.0;
}

// Moeller-Trumbore, both sides. The distance, or a negative value on a miss.
float intersectTriangle(vec3 _origin, vec3 _direction, vec3 _v0, vec3 _v1, vec3 _v2)
{
	vec3 edge1 = _v1 - _v0;
	vec3 edge2 = _v2 - _v0;
	vec3 p = cross(_direction, edge2);
	float determinant = dot(edge1, p);
	if (abs(determinant) < 1e-12)
	{
		return -1.0;
	}

	float inverse = 1.0 / determinant;
	vec3 s = _origin - _v0;
	float u = dot(s, p) * inverse;
	if (u < 0.0 || u > 1.0)
	{
		return -1.0;
	}

	vec3 q = cross(s, edge1);
	float v = dot(_direction, q) * inverse;
	if (v < 0.0 || u + v > 1.0)
	{
		return -1.0;
	}

	return dot(edge2, q) * inverse;
}

vec3 bvhVertex(int _triangle, int _corner)
{
	return b_scene[int(u_bvhInfo.x) + _triangle * 4 + _corner].xyz;
}

// The nearest triangle the ray hits within _maxDistance, or -1. With
// _anyHit, the first one found (for visibility tests).
//
// Traversals of the same depth-first node layout (src/bvh.h):
// - Stackless (the default): a node's left child is the next node, and its
//   skip link is where to go once the node is missed or done with. No array,
//   so nothing to spill to scratch memory, which a per-thread stack in a pixel
//   shader does (the dynamically indexed array cannot stay in registers). It
//   always visits the left child first, so closest-hit rays may visit more
//   nodes.
// - With BVH_SHARED_STACK (a compute shader whose groups have that many
//   threads): closest-hit rays visit the nearest child first, with a short
//   stack of the other children in shared memory, kBvhShortStack entries per
//   thread. Shared memory limits how many groups a GPU runs at once, so the
//   stack is kept short: when it overflows, its oldest entry is dropped, and
//   once it runs empty the traversal finishes stackless from the root with
//   the nearest hit found so far (exact, only sometimes redundant). The
//   shader sets g_bvhThread to gl_LocalInvocationIndex at the start of
//   main() (bgfx passes it to main() only). Visibility rays stay stackless:
//   any hit will do, so the order matters less.
// - With RESTIR_WAVE_OPS (Vulkan, see compute.sh), visibility rays
//   traverse in step across the wave: each step, the wave visits the
//   smallest node any of its lanes still needs, read once for all of them
//   at a wave-uniform index. Node indices only grow along a stackless
//   traversal, so every lane still visits its own nodes in order.
#ifdef BVH_SHARED_STACK
#	if BGFX_SHADER_LANGUAGE_GLSL
int g_bvhThread;
#	else
static int g_bvhThread;
#	endif
SHARED int s_bvhStack[BVH_SHARED_STACK * kBvhShortStack];
#endif

// The ray's slabs: a huge but finite inverse keeps axis-parallel rays working.
vec3 bvhInverseDirection(vec3 _direction)
{
	vec3 safeDirection = vec3(
		  abs(_direction.x) > 1e-8 ? _direction.x : (_direction.x >= 0.0 ? 1e-8 : -1e-8)
		, abs(_direction.y) > 1e-8 ? _direction.y : (_direction.y >= 0.0 ? 1e-8 : -1e-8)
		, abs(_direction.z) > 1e-8 ? _direction.z : (_direction.z >= 0.0 ? 1e-8 : -1e-8)
		);
	return vec3_splat(1.0) / safeDirection;
}

// Tests a leaf's triangles, shortening _distance to the nearest hit.
int bvhIntersectLeaf(vec3 _origin, vec3 _direction, int _first, int _count, inout float _distance, int _hit)
{
	int hit = _hit;
	for (int triangleIndex = _first; triangleIndex < _first + _count; ++triangleIndex)
	{
		float t = intersectTriangle(_origin, _direction
			, bvhVertex(triangleIndex, 0), bvhVertex(triangleIndex, 1), bvhVertex(triangleIndex, 2));
		if (t > 0.0 && t < _distance)
		{
			_distance = t;
			hit = triangleIndex;
		}
	}
	return hit;
}

int traceBvhStackless(vec3 _origin, vec3 _direction, bool _anyHit, inout float _distance)
{
	vec3 inverseDirection = bvhInverseDirection(_direction);
	int nodeCount = int(u_bvhInfo.z);
	int hit = -1;
	int node = 0;
	for (int step = 0; step < kBvhMaxSteps; ++step)
	{
		if (node >= nodeCount)
		{
			break;
		}

		vec4 node0 = b_scene[node * 2];
		vec4 node1 = b_scene[node * 2 + 1];
		bool leaf = node1.w >= 0.0;
		if (intersectBox(_origin, inverseDirection, node0.xyz, node1.xyz, _distance) < 0.0)
		{
			// Missed: past its subtree (a leaf's is itself).
			node = leaf ? node + 1 : int(-node1.w) - 1;
			continue;
		}

		if (leaf)
		{
			hit = bvhIntersectLeaf(_origin, _direction, int(node0.w), int(node1.w), _distance, hit);
			if (_anyHit && hit >= 0)
			{
				return hit;
			}
		}
		node = node + 1;
	}
	return hit;
}

#ifdef RESTIR_WAVE_OPS
int traceBvhWave(vec3 _origin, vec3 _direction, bool _anyHit, inout float _distance)
{
	vec3 inverseDirection = bvhInverseDirection(_direction);
	int nodeCount = int(u_bvhInfo.z);
	int hit = -1;
	int node = 0;
	for (int step = 0; step < kBvhMaxSteps; ++step)
	{
		// Lanes that are done ask for no node (nodeCount and past).
		int waveNode = WaveActiveMin(node);
		if (waveNode >= nodeCount)
		{
			break;
		}

		if (node == waveNode)
		{
			vec4 node0 = b_scene[waveNode * 2];
			vec4 node1 = b_scene[waveNode * 2 + 1];
			bool leaf = node1.w >= 0.0;
			if (intersectBox(_origin, inverseDirection, node0.xyz, node1.xyz, _distance) < 0.0)
			{
				node = leaf ? node + 1 : int(-node1.w) - 1;
			}
			else
			{
				if (leaf)
				{
					hit = bvhIntersectLeaf(_origin, _direction, int(node0.w), int(node1.w), _distance, hit);
				}
				node = _anyHit && hit >= 0 ? nodeCount : node + 1;
			}
		}
	}
	return hit;
}
#endif

#ifdef BVH_SHARED_STACK
int traceBvhSharedStack(vec3 _origin, vec3 _direction, inout float _distance)
{
	vec3 inverseDirection = bvhInverseDirection(_direction);
	if (intersectBox(_origin, inverseDirection, b_scene[0].xyz, b_scene[1].xyz, _distance) < 0.0)
	{
		return -1;
	}

	// A ring of kBvhShortStack entries: pushing onto a full one overwrites
	// its oldest entry, and the subtree it held is then visited by the
	// stackless pass below.
	int stackBase = g_bvhThread * kBvhShortStack;
	int stackTop = 0;   // grows without bound; entries live at stackTop % kBvhShortStack
	int stackCount = 0; // entries held, at most kBvhShortStack
	bool dropped = false;
	int hit = -1;
	int node = 0;
	for (int step = 0; step < kBvhMaxSteps; ++step)
	{
		vec4 node0 = b_scene[node * 2];
		vec4 node1 = b_scene[node * 2 + 1];
		if (node1.w >= 0.0)
		{
			hit = bvhIntersectLeaf(_origin, _direction, int(node0.w), int(node1.w), _distance, hit);
		}
		else
		{
			// Visit the nearer child first; keep the other for later.
			int left = node + 1;
			int right = int(node0.w);
			float leftDistance = intersectBox(_origin, inverseDirection
				, b_scene[left * 2].xyz, b_scene[left * 2 + 1].xyz, _distance);
			float rightDistance = intersectBox(_origin, inverseDirection
				, b_scene[right * 2].xyz, b_scene[right * 2 + 1].xyz, _distance);
			bool visitLeft = leftDistance >= 0.0;
			bool visitRight = rightDistance >= 0.0;
			if (visitLeft && visitRight)
			{
				bool leftFirst = leftDistance <= rightDistance;
				s_bvhStack[stackBase + (stackTop & (kBvhShortStack - 1))] = leftFirst ? right : left;
				++stackTop;
				if (stackCount == kBvhShortStack)
				{
					dropped = true;
				}
				else
				{
					++stackCount;
				}
				node = leftFirst ? left : right;
				continue;
			}
			if (visitLeft || visitRight)
			{
				node = visitLeft ? left : right;
				continue;
			}
		}

		if (stackCount == 0)
		{
			break;
		}
		--stackTop;
		--stackCount;
		node = s_bvhStack[stackBase + (stackTop & (kBvhShortStack - 1))];
	}

	if (dropped)
	{
		// Some subtrees were not visited: go over the whole tree again,
		// skipping everything beyond the nearest hit so far.
		int stacklessHit = traceBvhStackless(_origin, _direction, false, _distance);
		if (stacklessHit >= 0)
		{
			hit = stacklessHit;
		}
	}
	return hit;
}
#endif

int traceBvh(vec3 _origin, vec3 _direction, float _maxDistance, bool _anyHit, out float _distance)
{
	_distance = _maxDistance;
	if (!bvhAvailable())
	{
		return -1;
	}

#ifdef BVH_SHARED_STACK
	if (!_anyHit)
	{
		return traceBvhSharedStack(_origin, _direction, _distance);
	}
#endif
#ifdef RESTIR_WAVE_OPS
	if (_anyHit)
	{
		return traceBvhWave(_origin, _direction, true, _distance);
	}
#endif
	return traceBvhStackless(_origin, _direction, _anyHit, _distance);
}

// The triangle's plane normal, turned to face _towards (a direction from
// the triangle, e.g. back along the ray that hit it).
vec3 bvhTriangleNormal(int _triangle, vec3 _towards)
{
	vec3 v0 = bvhVertex(_triangle, 0);
	vec3 normal = normalize(cross(bvhVertex(_triangle, 1) - v0, bvhVertex(_triangle, 2) - v0));
	return dot(normal, _towards) < 0.0 ? -normal : normal;
}

vec3 bvhTriangleAlbedo(int _triangle)
{
	return b_scene[int(u_bvhInfo.x) + _triangle * 4 + 3].rgb;
}

vec3 bvhTriangleEmission(int _triangle)
{
	int base = int(u_bvhInfo.x) + _triangle * 4;
	return vec3(b_scene[base + 1].w, b_scene[base + 2].w, b_scene[base + 3].w);
}

#endif // BVH_SH_HEADER_GUARD
