#ifndef COMMON_STEREO_SAMPLING_HLSLI
#define COMMON_STEREO_SAMPLING_HLSLI

namespace StereoSampling
{
	/** @brief Maps a full-resolution pixel into a half-resolution packed-stereo texture. */
	float2 MapPixelToHalfResolution(float2 pixel, uint eyeIndex, float eyeWidth)
	{
#ifdef VR
		float targetEyeWidth = ceil(eyeWidth * 0.5f);
		return float2((pixel.x - eyeIndex * eyeWidth) * 0.5f + eyeIndex * targetEyeWidth, pixel.y * 0.5f);
#else
		return pixel * 0.5f;
#endif
	}
}

#endif
