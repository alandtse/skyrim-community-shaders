#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"
#include "Common/VRReproject.hlsli"
#include "Common/VRStereoEffects.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
SamplerState NormalSampler : register(s0);
SamplerState ColorSampler : register(s1);
SamplerState DepthSampler : register(s2);
SamplerState AlphaSampler : register(s3);

Texture2D<float4> NormalTex : register(t0);
Texture2D<float4> ColorTex : register(t1);
Texture2D<float4> DepthTex : register(t2);
Texture2D<float4> AlphaTex : register(t3);

cbuffer PerGeometry : register(b2)
{
	float4 SSRParams : packoffset(c0);  // fReflectionRayThickness in x, fReflectionMarchingRadius in y, fAlphaWeight in z, 1 / fReflectionMarchingRadius in w
	float3 DefaultNormal : packoffset(c1);
};

static const int iterations = 64.0;
static const int binaryIterations = ceil(log2(iterations));

static const float rayLength = 1.0;

#	if defined(VR)
#		include "Common/FoveatedShaderDetail.hlsli"

static const int minFoveatedIterations = 16;

// Per-pixel SSR foveation weight from the active foveation mask (VRFoveationData0
// + per-eye center offset). 1 in the center, falling to 0 in the periphery.
float GetVRSSRFoveationWeight(float ssrFoveationMode, float2 eyeUv, uint eyeIndex)
{
	float2 centerOffset = eyeIndex == 0 ? SharedData::VRFoveationCenterOffsets.xy : SharedData::VRFoveationCenterOffsets.zw;
	return FoveatedEvaluateShaderDetailWeight(
		ssrFoveationMode,
		eyeUv,
		SharedData::VRFoveationData0.x,
		SharedData::VRFoveationData0.y,
		SharedData::VRFoveationData0.z,
		centerOffset);
}

// Scale the raymarch count by the foveation weight: full in the center, down to
// minFoveatedIterations toward the periphery.
int GetSSRRaymarchIterations(float foveationWeight)
{
	int iterationCount = (int)ceil(lerp((float)minFoveatedIterations, (float)iterations, saturate(foveationWeight)));
	return min(max(iterationCount, minFoveatedIterations), iterations);
}

int GetSSRBinaryIterations(int raymarchIterations)
{
	int iterationCount = (int)ceil(log2((float)raymarchIterations));
	return min(max(iterationCount, 1), binaryIterations);
}
#	endif

/** Maps an eye-local ray sample to current-frame dynamic-resolution SBS coordinates. */
float2 ConvertRaySample(float2 raySample, uint eyeIndex, uint2 textureDimensions)
{
	float2 stereoUV = Stereo::ConvertToStereoUV(raySample, eyeIndex);
	float2 screenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(stereoUV);
#	if defined(VR)
	screenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		screenPosition, eyeIndex, textureDimensions, FrameBuffer::DynamicResolutionParams1.xy);
#	endif
	return screenPosition;
}

/** Maps an eye-local ray sample to previous-frame dynamic-resolution SBS coordinates. */
float2 ConvertRaySamplePrevious(float2 raySample, uint eyeIndex)
{
	float2 stereoUV = Stereo::ConvertToStereoUV(raySample, eyeIndex);
	float2 screenPosition = FrameBuffer::GetPreviousDynamicResolutionAdjustedScreenPosition(stereoUV);
#	if defined(VR)
	screenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		screenPosition, eyeIndex, AlphaTex, FrameBuffer::DynamicResolutionParams1.zw);
#	endif
	return screenPosition;
}

float4 GetReflectionColor(
	float3 projReflectionDirection,
	float3 projPosition,
	uint eyeIndex,
	uint2 depthTextureDimensions
#	if defined(VR)
	,
	int raymarchIterations,
	int binaryIterationsCount,
	float foveationWeight
#	endif
)
{
	float3 prevRaySample;
	float3 raySample = projPosition;

	// VR scales the raymarch/binary counts by the foveation weight and fades the result; non-VR uses
	// full counts. Bounds are runtime in VR, so [loop] is required (cannot unroll).
#	if defined(VR)
	int rayCount = raymarchIterations;
	int binCount = binaryIterationsCount;
	float fovWeight = foveationWeight;
	[loop] for (int i = 0; i < rayCount; i++)
	{
#	else
	int rayCount = iterations;
	int binCount = binaryIterations;
	float fovWeight = 1.0;
	for (int i = 0; i < rayCount; i++) {
#	endif
		prevRaySample = raySample;
		raySample = projPosition + (float(i) / float(rayCount)) * projReflectionDirection;

		float2 sampleUV;
		uint sampleEyeIndex;
		Stereo::ResolveMonoUVForEye(raySample, eyeIndex, sampleUV, sampleEyeIndex);

		if (FrameBuffer::IsOutsideFrame(sampleUV))
			return 0.0;

		float iterationDepth = DepthTex.SampleLevel(DepthSampler, ConvertRaySample(sampleUV, sampleEyeIndex, depthTextureDimensions), 0).x;

		if (saturate((raySample.z - iterationDepth) / SSRParams.y) > 0.0) {
			float3 binaryMinRaySample = prevRaySample;
			float3 binaryMaxRaySample = raySample;
			float3 binaryRaySample = raySample;
			float depthThicknessFactor;
			uint hitEyeIndex = sampleEyeIndex;

#	if defined(VR)
			[loop] for (int k = 0; k < binCount; k++)
			{
#	else
			for (int k = 0; k < binCount; k++) {
#	endif
				binaryRaySample = lerp(binaryMinRaySample, binaryMaxRaySample, 0.5);

				Stereo::ResolveMonoUVForEye(binaryRaySample, eyeIndex, sampleUV, hitEyeIndex);
				iterationDepth = DepthTex.SampleLevel(DepthSampler, ConvertRaySample(sampleUV, hitEyeIndex, depthTextureDimensions), 0).x;

				// Compute expected depth vs actual depth
				depthThicknessFactor = 1.0 - saturate(abs(binaryRaySample.z - iterationDepth) / SSRParams.y);

				if (iterationDepth < binaryRaySample.z)
					binaryMaxRaySample = binaryRaySample;
				else
					binaryMinRaySample = binaryRaySample;
			}

			// Fade based on ray length
			float ssrMarchingRadiusFadeFactor = 1.0 - saturate(length(binaryRaySample - projPosition) / rayLength);

			float2 uvResultScreenCenterOffset = binaryRaySample.xy - 0.5;

#	ifdef VR
			float2 centerDistance = abs(uvResultScreenCenterOffset.xy * 2.0);

			// Make VR fades consistent by taking the closer of the two eyes
			// Based on concepts from https://cuteloong.github.io/publications/scssr24/
			float2 otherEyeUvResultScreenCenterOffset = Stereo::ConvertMonoUVToOtherEye(float3(binaryRaySample.xy, iterationDepth), eyeIndex).xy - 0.5;
			centerDistance = min(centerDistance, abs(otherEyeUvResultScreenCenterOffset * 2.0));
#	else
			float2 centerDistance = abs(uvResultScreenCenterOffset.xy * 2.0);
#	endif

			// Fade out around screen edges
			float centerDistanceFadeFactorX = smoothstep(0.0, 0.1, saturate(1.0 - centerDistance.x));
			float centerDistanceFadeFactorY = smoothstep(0.0, 0.5, saturate(1.0 - centerDistance.y));

			float fadeFactor = depthThicknessFactor * ssrMarchingRadiusFadeFactor * centerDistanceFadeFactorX * centerDistanceFadeFactorY;

			if (fadeFactor > 0.0) {
				// Resolve final UV in the eye that owns the hit
				float2 finalSampleUV;
				uint finalEyeIndex;
				Stereo::ResolveMonoUVForEye(float3(binaryRaySample.xy, iterationDepth), eyeIndex, finalSampleUV, finalEyeIndex);

				uint2 colorTextureDimensions = uint2(1, 1);
#	if defined(VR)
				ColorTex.GetDimensions(colorTextureDimensions.x, colorTextureDimensions.y);
#	endif
				float2 colorScreenPosition = ConvertRaySample(finalSampleUV, finalEyeIndex, colorTextureDimensions);
				float3 color = ColorTex.SampleLevel(ColorSampler, colorScreenPosition, 0).xyz;

				// Final sample to world-space
				float4 positionWS = float4(float2(finalSampleUV.x, 1.0 - finalSampleUV.y) * 2.0 - 1.0, iterationDepth, 1.0);
				positionWS = mul(FrameBuffer::CameraViewProjInverse[finalEyeIndex], positionWS);
				positionWS.xyz = positionWS.xyz / positionWS.w;
				positionWS.w = 1.0;

				// Compute camera motion vector
				float2 cameraMotionVector = MotionBlur::GetSSMotionVector(positionWS, positionWS, finalEyeIndex);

				// Reproject alpha from previous frame
				float2 reprojectedRaySample = finalSampleUV + cameraMotionVector;
				float4 alpha = 0.0;

				// Check that the reprojected data is within the frame
				if (!FrameBuffer::IsOutsideFrame(reprojectedRaySample.xy))
					alpha = float4(AlphaTex.SampleLevel(AlphaSampler, ConvertRaySamplePrevious(reprojectedRaySample.xy, finalEyeIndex), 0).xyz, 1.0);

				float3 reflectionColor = color + SSRParams.z * alpha.xyz * alpha.w;
				return float4(reflectionColor, fadeFactor * fovWeight);
			}

			return 0.0;
		}
	}

	return 0.0;
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	psout.Color = 0;

#	ifndef ENABLESSR
	// Disable SSR raymarch
	return psout;
#	endif

	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(input.TexCoord);
	float2 uv = input.TexCoord;
	float2 screenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
	float2 normalScreenPosition = screenPosition;
	float2 depthScreenPosition = screenPosition;

	uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);

#	if defined(VR)
	uint2 depthTextureDimensions;
	DepthTex.GetDimensions(depthTextureDimensions.x, depthTextureDimensions.y);
	normalScreenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		screenPosition, eyeIndex, NormalTex, FrameBuffer::DynamicResolutionParams1.xy);
	depthScreenPosition = VRStereoEffects::ClampDynamicStereoUVToEyeTexel(
		screenPosition, eyeIndex, depthTextureDimensions, FrameBuffer::DynamicResolutionParams1.xy);

	float ssrFoveationWeight = 1.0;
	float ssrFoveationMode = SharedData::VRFoveationData0.w;
	[branch] if (ssrFoveationMode >= FOVEATED_SHADER_DETAIL_MODE_FEATHERED)
	{
		ssrFoveationWeight = GetVRSSRFoveationWeight(ssrFoveationMode, uv, eyeIndex);
		// Outside the foveation mask: skip SSR entirely. The cubemap/water
		// reflection fallback already covers these pixels.
		[branch] if (!FoveatedIsShaderDetailActive(ssrFoveationWeight)) return psout;
	}
#	endif

	[branch] if (NormalTex.Sample(NormalSampler, normalScreenPosition).z <= 0)
	{
		return psout;
	}

	float3 viewNormal = DefaultNormal;

	float depth = DepthTex.SampleLevel(DepthSampler, depthScreenPosition, 0).x;

	float4 positionVS = float4(float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
	positionVS = mul(FrameBuffer::CameraProjInverse[eyeIndex], positionVS);
	positionVS.xyz = positionVS.xyz / positionVS.w;

	float3 viewPosition = positionVS.xyz;
	float3 viewDirection = normalize(viewPosition);

	float3 reflectionDirection = reflect(viewDirection, viewNormal);
	float viewAttenuation = saturate(dot(viewDirection, reflectionDirection));
	[branch] if (viewAttenuation < 0)
	{
		return psout;
	}

	float4 reflectionPosition = float4(viewPosition + reflectionDirection, 1.0);
	float4 projReflectionPosition = mul(FrameBuffer::CameraProj[eyeIndex], reflectionPosition);
	projReflectionPosition /= projReflectionPosition.w;
	projReflectionPosition.xy = projReflectionPosition.xy * float2(0.5, -0.5) + float2(0.5, 0.5);

	float3 projPosition = float3(uv, depth);
	float3 projReflectionDirection = normalize(projReflectionPosition.xyz - projPosition) * rayLength;

#	if defined(VR)
	int raymarchIterations = iterations;
	int binaryIterationsCount = binaryIterations;
	[branch] if (ssrFoveationWeight < 0.9999)
	{
		raymarchIterations = GetSSRRaymarchIterations(ssrFoveationWeight);
		binaryIterationsCount = GetSSRBinaryIterations(raymarchIterations);
	}
	psout.Color = GetReflectionColor(
		projReflectionDirection, projPosition, eyeIndex, depthTextureDimensions,
		raymarchIterations, binaryIterationsCount, ssrFoveationWeight);
#	else
	psout.Color = GetReflectionColor(projReflectionDirection, projPosition, eyeIndex, uint2(1, 1));
#	endif

	return psout;
}
#endif
