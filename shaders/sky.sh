/*
 * The sky: a gradient from the ground colour (below the horizon) through the
 * horizon colour to the zenith colour, +y up. It lights the scene through the
 * GI rays that leave it (fs_restir_temporal.sc) and shows behind it
 * (fs_composite.sc). The colours come premultiplied by the intensity.
 */

#ifndef SKY_SH_HEADER_GUARD
#define SKY_SH_HEADER_GUARD

uniform vec4 u_skyZenith;  // rgb: radiance straight up, w: 1 when the sky is on
uniform vec4 u_skyHorizon; // rgb: radiance at the horizon
uniform vec4 u_skyGround;  // rgb: radiance straight down

bool skyEnabled()
{
	return u_skyZenith.w > 0.5;
}

vec3 skyRadiance(vec3 _direction)
{
	if (!skyEnabled())
	{
		return vec3_splat(0.0);
	}

	float height = clamp(_direction.y, -1.0, 1.0);
	return height >= 0.0
		? mix(u_skyHorizon.rgb, u_skyZenith.rgb, sqrt(height))
		: mix(u_skyHorizon.rgb, u_skyGround.rgb, sqrt(-height));
}

#endif // SKY_SH_HEADER_GUARD
