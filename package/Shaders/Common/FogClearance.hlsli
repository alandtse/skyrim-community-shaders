#ifndef OPEN_SHADERS_FOG_CLEARANCE_HLSLI
#define OPEN_SHADERS_FOG_CLEARANCE_HLSLI

#include "Common/FrameBuffer.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

#if defined(PSHADER)
namespace FogClearance
{
	/** @brief Remove listed fog fragments inside one shared headset-centered sphere or an entire replacement draw. */
	void Apply(float3 positionRelativeToEye, uint eyeIndex)
	{
#	if defined(VR)
		[branch] if (Permutation::FogClearanceRadius < 0.0)
		{
			clip(-1.0);
		}
		else if (Permutation::FogClearanceRadius > 0.0)
		{
			float3 leftEye = FrameBuffer::ViewToWorld(0.0.xxx, true, 0);
			float3 rightEye = FrameBuffer::ViewToWorld(0.0.xxx, true, 1);
			// Convert each eye origin into the fragment's camera-relative coordinate frame.
			leftEye += FrameBuffer::CameraPosAdjust[0].xyz - FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
			rightEye += FrameBuffer::CameraPosAdjust[1].xyz - FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
			float3 fromHeadset = positionRelativeToEye - 0.5 * (leftEye + rightEye);
			clip(dot(fromHeadset, fromHeadset) - Permutation::FogClearanceRadius * Permutation::FogClearanceRadius);
		}
#	endif
	}

	/** @brief Apply the same sphere to a depth fragment without sampling scene depth. */
	void ApplyScreen(float4 screenPosition, uint eyeIndex)
	{
#	if defined(VR)
		[branch] if (Permutation::FogClearanceRadius < 0.0)
		{
			clip(-1.0);
		}
		else if (Permutation::FogClearanceRadius > 0.0)
		{
			float2 uv = screenPosition.xy * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
			uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);
			float4 positionCS = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), screenPosition.z, 1.0);
			float4 positionWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionCS);
			Apply(positionWS.xyz / positionWS.w, eyeIndex);
		}
#	endif
	}
}
#endif

#endif
