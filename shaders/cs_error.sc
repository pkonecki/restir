/*
 * The error of the displayed image against the reference (fs_reference.sc),
 * over the pixels that show geometry, for the settings window:
 *
 *   x: relative MSE, the mean of (L - L_ref)^2 / (L_ref^2 + 0.01) in luminance
 *   y: mean luminance of the image
 *   z: mean luminance of the reference
 *   w: pixels compared
 *
 * One group of 16x16 threads walks the whole image (each thread every 16th
 * pixel in both directions) and sums in shared memory. The result goes to a
 * small buffer that the CPU reads back.
 */

#include <bgfx_compute.sh>

SAMPLER2D(s_display, 0);   // the image before tone mapping (after TAA)
SAMPLER2D(s_reference, 1);
BUFFER_RW(b_error, uint, 2); // 4 floats, as bits

uniform vec4 u_errorInfo; // xy: image size

#define kThreads 256

SHARED float g_relative[kThreads];
SHARED float g_image[kThreads];
SHARED float g_reference[kThreads];
SHARED float g_count[kThreads];

float luminance(vec3 _color)
{
	return dot(_color, vec3(0.2126, 0.7152, 0.0722));
}

NUM_THREADS(16, 16, 1)
void main()
{
	int index = int(gl_LocalInvocationIndex);
	ivec2 thread = ivec2(gl_LocalInvocationID.xy);
	ivec2 size = ivec2(u_errorInfo.xy);
	vec2 texelSize = vec2_splat(1.0) / u_errorInfo.xy;

	float relative = 0.0;
	float image = 0.0;
	float reference = 0.0;
	float count = 0.0;
	for (int y = thread.y; y < size.y; y += 16)
	{
		for (int x = thread.x; x < size.x; x += 16)
		{
			vec2 uv = (vec2(float(x), float(y)) + 0.5) * texelSize;
			vec4 referenceColor = texture2DLod(s_reference, uv, 0.0);
			if (referenceColor.a < 0.5)
			{
				continue;
			}
			float imageLuminance = luminance(texture2DLod(s_display, uv, 0.0).rgb);
			float referenceLuminance = luminance(referenceColor.rgb);
			float difference = imageLuminance - referenceLuminance;
			relative += difference * difference / (referenceLuminance * referenceLuminance + 0.01);
			image += imageLuminance;
			reference += referenceLuminance;
			count += 1.0;
		}
	}

	g_relative[index] = relative;
	g_image[index] = image;
	g_reference[index] = reference;
	g_count[index] = count;
	barrier();

	for (int stride = kThreads / 2; stride > 0; stride >>= 1) // a shift: D3D warns about signed divides
	{
		if (index < stride)
		{
			g_relative[index] += g_relative[index + stride];
			g_image[index] += g_image[index + stride];
			g_reference[index] += g_reference[index + stride];
			g_count[index] += g_count[index + stride];
		}
		barrier();
	}

	if (index == 0)
	{
		float total = max(g_count[0], 1.0);
		b_error[0] = floatBitsToUint(g_relative[0] / total);
		b_error[1] = floatBitsToUint(g_image[0] / total);
		b_error[2] = floatBitsToUint(g_reference[0] / total);
		b_error[3] = floatBitsToUint(g_count[0]);
	}
}
