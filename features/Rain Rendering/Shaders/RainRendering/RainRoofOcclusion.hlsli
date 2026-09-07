#ifndef __RAIN_ROOF_OCCLUSION_HLSL__
#define __RAIN_ROOF_OCCLUSION_HLSL__

#define SKYLIGHTING_PROBE_REGISTER t38
#include "Skylighting/Skylighting.hlsli"

namespace RainRoofOcclusion
{
	float Sample(float3 worldPosition, float fadeStart, float fadeEnd)
	{
		float3 positionMS = worldPosition - FrameBuffer::CameraPosAdjust[0].xyz;
		sh2 skyVisibility = Skylighting::SampleNoBiasIncludingInteriors(positionMS);
		float visibility = Skylighting::CosineLobeVisibility(
			skyVisibility, float3(0.0f, 0.0f, 1.0f), Skylighting::GetFadeOutFactor(positionMS));
		return smoothstep(min(fadeStart, fadeEnd), max(fadeStart, fadeEnd), visibility);
	}
}

#endif  // __RAIN_ROOF_OCCLUSION_HLSL__
