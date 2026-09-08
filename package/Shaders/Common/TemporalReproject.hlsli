#ifndef __TEMPORAL_REPROJECT_HLSLI__
#define __TEMPORAL_REPROJECT_HLSLI__

#include "Common/FrameBuffer.hlsli"

namespace Temporal
{
	/**
	* @brief Mono UV a world-space point occupied in the previous frame.
	*
	* @param positionWS Current-frame world position (camera-relative, this frame's PosAdjust)
	* @param eyeIndex Eye whose previous view-projection to use
	* @param[out] valid False when the point was behind the previous camera or off screen
	* @return Previous-frame mono UV [0,1], saturated; only meaningful when valid
	*/
	float2 PreviousFrameUV(float3 positionWS, uint eyeIndex, out bool valid)
	{
		float3 previousPositionWS = positionWS + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPreviousPosAdjust[eyeIndex].xyz;
		float4 previousClip = mul(FrameBuffer::CameraPreviousViewProjUnjittered[eyeIndex], float4(previousPositionWS, 1.0));
		float2 uv = previousClip.xy / previousClip.w * float2(0.5, -0.5) + 0.5;
		valid = previousClip.w > 0.0 && all(uv >= 0.0) && all(uv < 1.0);
		return saturate(uv);
	}
}

#endif
