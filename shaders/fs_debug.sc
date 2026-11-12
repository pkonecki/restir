$input v_texcoord0

/*
 * Debug views of the renderer's intermediate data, drawn instead of the
 * composite. The output is already a display colour: the tone mapping pass
 * passes it through.
 *
 * G-buffer views are at full resolution; reservoir, history and hash grid
 * views read this frame's GI targets (GI resolution, point sampled).
 * Heat maps use the Turbo colour map: dark blue is low, dark red is high.
 */

#include <common.sh>
#include <bgfx_compute.sh>

SAMPLER2D(s_albedo, 0);            // rgb: base colour, a: roughness
SAMPLER2D(s_normal, 1);            // rgb: encoded normal, a: metallic
SAMPLER2D(s_depth, 2);
SAMPLER2D(s_reservoirSample, 3);   // xyz: sample position, w: M
SAMPLER2D(s_reservoirReceiver, 4); // xyz: receiver position, w: W
SAMPLER2D(s_giMoments, 5);         // z: history length

uniform mat4 u_sceneInvViewProj;
uniform vec4 u_debugView; // x: view (DebugView in settings.h), y: largest M

#define GRID_READ_ONLY
#define GRID_CHECKSUM_STAGE 11
#define GRID_COUNT_STAGE    12
#define GRID_ENTRY_STAGE    13
#include "hashgrid.sh"

// Keep in sync with DebugView in settings.h (the composite handles the
// lighting views before these).
#define VIEW_ALBEDO     4
#define VIEW_NORMALS    5
#define VIEW_MATERIAL   6
#define VIEW_M          7
#define VIEW_W          8
#define VIEW_HISTORY    9
#define VIEW_GRID       10

#define kSkyDepth 0.99999
#define kMaxAccumulation 16.0 // see fs_restir_spatial.sc

// Turbo colour map, polynomial approximation (Google, Apache 2.0).
vec3 turbo(float _x)
{
	vec4 red4 = vec4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
	vec4 green4 = vec4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
	vec4 blue4 = vec4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
	vec2 red2 = vec2(-152.94239396, 59.28637943);
	vec2 green2 = vec2(4.27729857, 2.82956604);
	vec2 blue2 = vec2(-89.90310912, 27.34824973);

	float x = clamp(_x, 0.0, 1.0);
	vec4 v4 = vec4(1.0, x, x * x, x * x * x);
	vec2 v2 = v4.zw * v4.z;
	return vec3(
		  dot(v4, red4) + dot(v2, red2)
		, dot(v4, green4) + dot(v2, green2)
		, dot(v4, blue4) + dot(v2, blue2)
		);
}

vec3 reconstructWorldPosition(vec2 _uv, float _depth)
{
	vec3 clipPosition = vec3(_uv * 2.0 - 1.0, toClipSpaceDepth(_depth));
	clipPosition.y = toClipSpaceY(clipPosition.y);
	return clipToWorld(u_sceneInvViewProj, clipPosition);
}

// A colour per hash grid cell (as seen from this frame's camera, without
// the jitter), dimmed when the cell lists few reservoirs and grey when no
// reservoir was inserted in it.
vec3 gridCellColor(vec3 _position, vec3 _normal)
{
	uint slot;
	uint checksum;
	gridKey(_position, _normal, gridCellSize(_position), slot, checksum);
	uint cell = gridFindCell(slot, checksum);
	if (cell == kGridInvalid)
	{
		return vec3_splat(0.25);
	}

	uint hash = gridHash(checksum);
	vec3 color = vec3(float(hash & 255u), float((hash >> 8u) & 255u), float((hash >> 16u) & 255u)) / 255.0;
	float fill = min(float(b_gridCounts[cell]) / float(kGridCellCapacity), 1.0);
	return mix(vec3_splat(0.15), color, 0.35 + 0.65 * fill);
}

void main()
{
	int view = int(u_debugView.x);
	float depth = texture2DLod(s_depth, v_texcoord0, 0.0).x;
	if (depth >= kSkyDepth)
	{
		gl_FragColor = vec4(0.125, 0.141, 0.169, 1.0);
		return;
	}

	vec4 albedoRoughness = texture2DLod(s_albedo, v_texcoord0, 0.0);
	vec4 normalMetallic = texture2DLod(s_normal, v_texcoord0, 0.0);
	vec3 color = vec3_splat(0.0);
	if (view == VIEW_ALBEDO)
	{
		color = toGamma(vec4(albedoRoughness.rgb, 1.0)).rgb;
	}
	else if (view == VIEW_NORMALS)
	{
		color = normalMetallic.rgb;
	}
	else if (view == VIEW_MATERIAL)
	{
		color = vec3(albedoRoughness.a, normalMetallic.a, 0.0);
	}
	else if (view == VIEW_M)
	{
		float count = texture2DLod(s_reservoirSample, v_texcoord0, 0.0).w;
		color = turbo(count / max(u_debugView.y, 1.0));
	}
	else if (view == VIEW_W)
	{
		float weight = texture2DLod(s_reservoirReceiver, v_texcoord0, 0.0).w;
		color = weight > 0.0 ? turbo((log2(weight) + 8.0) / 16.0) : vec3_splat(0.0);
	}
	else if (view == VIEW_HISTORY)
	{
		float historyLength = texture2DLod(s_giMoments, v_texcoord0, 0.0).z;
		color = turbo((historyLength - 1.0) / (kMaxAccumulation - 1.0));
	}
	else if (view == VIEW_GRID)
	{
		vec3 position = reconstructWorldPosition(v_texcoord0, depth);
		vec3 normal = normalize(normalMetallic.xyz * 2.0 - 1.0);
		color = gridCellColor(position, normal);
	}

	gl_FragColor = vec4(color, 1.0);
}
