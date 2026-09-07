#ifndef __VR_STEREO_EFFECTS_DEPENDENCY_HLSL__
#define __VR_STEREO_EFFECTS_DEPENDENCY_HLSL__

#include "Common/VR.hlsli"

#if defined(VR)
namespace VRStereoEffects
{
	/** Clamps a dynamic-resolution SBS UV to a horizontal texel center within the selected eye. */
	float2 ClampDynamicStereoUVToEyeTexel(float2 dynamicStereoUV, uint eyeIndex, uint2 textureDimensions, float2 resolutionScale)
	{
		uint2 activeDimensions = max(uint2(float2(textureDimensions) * resolutionScale), uint2(2, 1));
		float2 normalizedUV = dynamicStereoUV / resolutionScale;
		return Stereo::ClampToEyeUV(normalizedUV, eyeIndex, activeDimensions) * resolutionScale;
	}

	/** Clamps a dynamic-resolution SBS UV using the sampled texture's dimensions. */
	float2 ClampDynamicStereoUVToEyeTexel(float2 dynamicStereoUV, uint eyeIndex, Texture2D<float4> sourceTexture, float2 resolutionScale)
	{
		uint width;
		uint height;
		sourceTexture.GetDimensions(width, height);
		return ClampDynamicStereoUVToEyeTexel(dynamicStereoUV, eyeIndex, uint2(width, height), resolutionScale);
	}

	/** Clamps a packed SBS UV using the sampled 2D texture's dimensions. */
	float2 ClampStereoUVToEyeTexel(float2 stereoUV, uint eyeIndex, Texture2D<float4> sourceTexture)
	{
		uint width;
		uint height;
		sourceTexture.GetDimensions(width, height);
		return Stereo::ClampToEyeUV(stereoUV, eyeIndex, uint2(width, height));
	}

	/** Clamps a packed SBS UV using the sampled 3D texture's dimensions. */
	float2 ClampStereoUVToEyeTexel(float2 stereoUV, uint eyeIndex, Texture3D<float4> sourceTexture)
	{
		uint width;
		uint height;
		uint depth;
		sourceTexture.GetDimensions(width, height, depth);
		return Stereo::ClampToEyeUV(stereoUV, eyeIndex, uint2(width, height));
	}
}
#endif

#endif
