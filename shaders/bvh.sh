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

uniform vec4 u_bvhInfo; // x: first triangle (in vec4s), y: first material, z: node count (0: no BVH), w: ray mode

// Keep in sync with RayMode in settings.h.
#define RAY_MODE_SCREEN 0 // depth buffer only
#define RAY_MODE_HYBRID 1 // depth buffer, then the BVH for rays it does not answer
#define RAY_MODE_BVH    2 // the BVH for every ray and visibility test

#define kRayMode int(u_bvhInfo.w)
#define kBvhStackSize 40 // at least kBvhMaxDepth in src/bvh.h
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
int traceBvh(vec3 _origin, vec3 _direction, float _maxDistance, bool _anyHit, out float _distance)
{
	_distance = _maxDistance;
	if (!bvhAvailable())
	{
		return -1;
	}

	// Axis-parallel rays: a huge but finite inverse keeps the slabs working.
	vec3 safeDirection = vec3(
		  abs(_direction.x) > 1e-8 ? _direction.x : (_direction.x >= 0.0 ? 1e-8 : -1e-8)
		, abs(_direction.y) > 1e-8 ? _direction.y : (_direction.y >= 0.0 ? 1e-8 : -1e-8)
		, abs(_direction.z) > 1e-8 ? _direction.z : (_direction.z >= 0.0 ? 1e-8 : -1e-8)
		);
	vec3 inverseDirection = vec3_splat(1.0) / safeDirection;

	int stack[kBvhStackSize];
	int stackSize = 0;
	int hit = -1;
	int node = 0;
	if (intersectBox(_origin, inverseDirection, b_scene[0].xyz, b_scene[1].xyz, _distance) < 0.0)
	{
		return -1;
	}

	for (int step = 0; step < kBvhMaxSteps; ++step)
	{
		vec4 node0 = b_scene[node * 2];
		vec4 node1 = b_scene[node * 2 + 1];
		int first = int(node0.w);
		int count = int(node1.w);
		if (count > 0)
		{
			for (int triangleIndex = first; triangleIndex < first + count; ++triangleIndex)
			{
				float t = intersectTriangle(_origin, _direction
					, bvhVertex(triangleIndex, 0), bvhVertex(triangleIndex, 1), bvhVertex(triangleIndex, 2));
				if (t > 0.0 && t < _distance)
				{
					_distance = t;
					hit = triangleIndex;
				}
			}

			if (_anyHit && hit >= 0)
			{
				return hit;
			}
		}
		else
		{
			// Visit the nearer child first; keep the other for later.
			int left = first;
			int right = first + 1;
			float leftDistance = intersectBox(_origin, inverseDirection
				, b_scene[left * 2].xyz, b_scene[left * 2 + 1].xyz, _distance);
			float rightDistance = intersectBox(_origin, inverseDirection
				, b_scene[right * 2].xyz, b_scene[right * 2 + 1].xyz, _distance);
			bool visitLeft = leftDistance >= 0.0;
			bool visitRight = rightDistance >= 0.0;
			if (visitLeft && visitRight)
			{
				bool leftFirst = leftDistance <= rightDistance;
				if (stackSize < kBvhStackSize)
				{
					stack[stackSize] = leftFirst ? right : left;
					++stackSize;
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

		if (stackSize == 0)
		{
			break;
		}
		--stackSize;
		node = stack[stackSize];
	}

	return hit;
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
	int material = int(b_scene[int(u_bvhInfo.x) + _triangle * 4].w);
	return b_scene[int(u_bvhInfo.y) + material].rgb;
}

#endif // BVH_SH_HEADER_GUARD
