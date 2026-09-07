#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/VR.hlsli"
#include "Common/VRStereoEffects.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float3 Color: SV_Target0;
};

#if defined(PSHADER)
SamplerState SSRSourceSampler : register(s0);
SamplerState WaterMaskSampler : register(s1);
SamplerState MainBufferSampler : register(s2);

Texture2D<float4> SSRSourceTex : register(t0);
Texture2D<float4> WaterMaskTex : register(t1);
Texture2D<float4> MainBufferTex : register(t2);

cbuffer PerGeometry : register(b2)
{
	float4 SSRParams : packoffset(c0);
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 adjustedScreenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord.xy);
	float2 ssrSourceScreenPosition = adjustedScreenPosition;
	float2 waterMaskScreenPosition = adjustedScreenPosition;
	float2 mainBufferScreenPosition = adjustedScreenPosition;
#	if defined(VR)
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(input.TexCoord);
	ssrSourceScreenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		adjustedScreenPosition, eyeIndex, SSRSourceTex, FrameBuffer::DynamicResolutionParams1.xy);
	waterMaskScreenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		adjustedScreenPosition, eyeIndex, WaterMaskTex, FrameBuffer::DynamicResolutionParams1.xy);
	mainBufferScreenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		adjustedScreenPosition, eyeIndex, MainBufferTex, FrameBuffer::DynamicResolutionParams1.xy);
#	endif

	float2 waterMask = WaterMaskTex.SampleLevel(WaterMaskSampler, waterMaskScreenPosition, 0).zw;
	float3 mainColor = MainBufferTex.Sample(MainBufferSampler, mainBufferScreenPosition).xyz;

	float3 colorOffset = 0.0.xxx;
	if (1e-5 >= waterMask.x && waterMask.y > 1e-5) {
		float4 ssrSourceColor = SSRSourceTex.Sample(SSRSourceSampler, ssrSourceScreenPosition);
		colorOffset = clamp(SSRParams.x * (ssrSourceColor.xyz * ssrSourceColor.w),
			0, SSRParams.y * mainColor);
	}

	psout.Color = colorOffset + mainColor;

	return psout;
}
#endif
