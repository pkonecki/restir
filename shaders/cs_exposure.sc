/*
 * Auto exposure: the log-average luminance of the HDR image, adapted over
 * time.
 *
 * One group of 16x16 threads reads a 64x64 grid of samples spread over the
 * whole image (4x4 per thread), skipping the background, and sums the log2
 * luminances in shared memory. Thread 0 then moves the stored average
 * towards this frame's by the adaptation factor, so the exposure eases in
 * like an eye's instead of jumping with every new view.
 *
 * b_exposure[0] holds the adapted log2 luminance (float bits). The tone
 * mapping pass (fs_tonemap.sc) turns it into an exposure.
 */

#include <bgfx_compute.sh>

SAMPLER2D(s_hdr, 0); // rgb: scene-referred colour, a: 1 on geometry, 0 on the background
BUFFER_RW(b_exposure, uint, 1);

uniform vec4 u_exposureAdapt; // x: adaptation factor (0-1), y: 1 to jump straight to this frame's value

#define kGridSize   64.0
#define kThreads    256
#define kMinLog     -12.0
#define kMaxLog     12.0

SHARED float s_logSum[kThreads];
SHARED float s_count[kThreads];

NUM_THREADS(16, 16, 1)
void main()
{
	int index = int(gl_LocalInvocationIndex);
	vec2 cell = vec2(gl_LocalInvocationID.xy) * 4.0;

	float logSum = 0.0;
	float count = 0.0;
	for (int y = 0; y < 4; ++y)
	{
		for (int x = 0; x < 4; ++x)
		{
			vec2 uv = (cell + vec2(float(x), float(y)) + 0.5) / kGridSize;
			vec4 color = texture2DLod(s_hdr, uv, 0.0);
			if (color.a > 0.5)
			{
				float luminance = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
				logSum += clamp(log2(max(luminance, 1e-6)), kMinLog, kMaxLog);
				count += 1.0;
			}
		}
	}

	s_logSum[index] = logSum;
	s_count[index] = count;
	barrier();

	for (int stride = kThreads / 2; stride > 0; stride /= 2)
	{
		if (index < stride)
		{
			s_logSum[index] += s_logSum[index + stride];
			s_count[index] += s_count[index + stride];
		}
		barrier();
	}

	if (index == 0)
	{
		float totalCount = s_count[0];
		if (totalCount < 1.0)
		{
			// Only background: keep the last value.
			return;
		}

		float average = s_logSum[0] / totalCount;
		float previous = uintBitsToFloat(b_exposure[0]);
		bool reset = u_exposureAdapt.y > 0.5 || !(abs(previous) <= kMaxLog);
		float adapted = reset ? average : mix(previous, average, u_exposureAdapt.x);
		b_exposure[0] = floatBitsToUint(adapted);
	}
}
