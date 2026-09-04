#ifndef OPEN_SHADERS_VR_LOCAL_FOG_HLSLI
#define OPEN_SHADERS_VR_LOCAL_FOG_HLSLI

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

namespace ExponentialHeightFog
{
	bool ShouldApplyVRLocalFog()
	{
#if defined(VR)
		return SharedData::exponentialHeightFogSettings.localFogDensityRadiusHeightClearance.x > 0.0f;
#else
		return false;
#endif
	}

	float EvaluateVRLocalFogNoise(float3 absolutePositionWS)
	{
		float noiseScale = max(SharedData::exponentialHeightFogSettings.localFogNoise.x, 0.000001f);
		float driftSpeed = SharedData::exponentialHeightFogSettings.localFogNoise.z;
		float3 drift = float3(1.0f, 0.37f, 0.08f) * SharedData::Timer * driftSpeed;
		float3 noisePosition = (absolutePositionWS + drift) * noiseScale;

		float wave0 = sin(dot(noisePosition, float3(0.7549f, 1.0f, 0.5698f)));
		float wave1 = sin(dot(noisePosition * 1.91f + 2.17f, float3(-0.438f, 0.881f, 1.237f)));
		float wave2 = sin(dot(noisePosition * 0.47f - 1.37f, float3(1.131f, -0.579f, 0.733f)));
		return saturate(0.5f + 0.5f * (wave0 + 0.5f * wave1 + 0.25f * wave2) / 1.75f);
	}

	float EvaluateVRLocalFogExtinction(float3 absolutePositionWS)
	{
		float4 densityRadiusHeightClearance = SharedData::exponentialHeightFogSettings.localFogDensityRadiusHeightClearance;
		float3 fromHeadset = absolutePositionWS - SharedData::exponentialHeightFogSettings.localFogCenterWS.xyz;
		float radius = max(densityRadiusHeightClearance.y, 1.0f);
		float heightScale = max(densityRadiusHeightClearance.z, 0.01f);
		float normalizedDistance = length(fromHeadset / float3(radius, radius, radius * heightScale));
		float radialFade = 1.0f - smoothstep(0.65f, 1.0f, normalizedDistance);

		float clearanceRadius = max(densityRadiusHeightClearance.w, 0.0f);
		float headsetDistance = length(fromHeadset);
		float headsetFade = clearanceRadius > 0.0f ? smoothstep(clearanceRadius, clearanceRadius + 16.0f, headsetDistance) : 1.0f;

		float noise = smoothstep(0.2f, 0.85f, EvaluateVRLocalFogNoise(absolutePositionWS));
		float noiseAmount = saturate(SharedData::exponentialHeightFogSettings.localFogNoise.y);
		return densityRadiusHeightClearance.x * radialFade * headsetFade * lerp(1.0f, noise, noiseAmount);
	}

	float4 SampleVRLocalWorldFog(float3 positionRelativeToEye, uint eyeIndex)
	{
#if defined(VR)
		if (!ShouldApplyVRLocalFog())
			return float4(0.0f, 0.0f, 0.0f, 1.0f);

		float3 eyeOriginRelative = FrameBuffer::ViewToWorld(0.0f.xxx, true, eyeIndex);
		float3 ray = positionRelativeToEye - eyeOriginRelative;
		float surfaceDistance = length(ray);
		if (surfaceDistance <= 1e-4f)
			return float4(0.0f, 0.0f, 0.0f, 1.0f);

		float3 rayDirection = ray / surfaceDistance;
		float3 eyeOriginAbsolute = eyeOriginRelative + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
		float3 centerWS = SharedData::exponentialHeightFogSettings.localFogCenterWS.xyz;
		float radius = max(SharedData::exponentialHeightFogSettings.localFogDensityRadiusHeightClearance.y, 1.0f);
		float heightScale = max(SharedData::exponentialHeightFogSettings.localFogDensityRadiusHeightClearance.z, 0.01f);
		float3 radii = float3(radius, radius, radius * heightScale);

		float3 scaledOrigin = (eyeOriginAbsolute - centerWS) / radii;
		float3 scaledDirection = rayDirection / radii;
		float quadraticA = dot(scaledDirection, scaledDirection);
		float quadraticB = dot(scaledOrigin, scaledDirection);
		float quadraticC = dot(scaledOrigin, scaledOrigin) - 1.0f;
		float discriminant = quadraticB * quadraticB - quadraticA * quadraticC;
		if (discriminant <= 0.0f || quadraticA <= 1e-8f)
			return float4(0.0f, 0.0f, 0.0f, 1.0f);

		float root = sqrt(discriminant);
		float segmentStart = max(0.0f, (-quadraticB - root) / quadraticA);
		float segmentEnd = min(surfaceDistance, (-quadraticB + root) / quadraticA);
		if (segmentEnd <= segmentStart)
			return float4(0.0f, 0.0f, 0.0f, 1.0f);

		static const uint kRaySampleCount = 4u;
		float stepLength = (segmentEnd - segmentStart) / float(kRaySampleCount);
		float opticalDepth = 0.0f;
		[unroll] for (uint sampleIndex = 0u; sampleIndex < kRaySampleCount; sampleIndex++)
		{
			float sampleDistance = segmentStart + (float(sampleIndex) + 0.5f) * stepLength;
			float3 samplePositionWS = eyeOriginAbsolute + rayDirection * sampleDistance;
			opticalDepth += EvaluateVRLocalFogExtinction(samplePositionWS) * stepLength;
		}

		float transmittance = exp(-max(opticalDepth, 0.0f));
		float opacity = saturate(1.0f - transmittance);
		float3 fogColor = saturate(SharedData::exponentialHeightFogSettings.localFogColor.rgb);
		float ambient = max(SharedData::exponentialHeightFogSettings.localFogNoise.w, 0.0f);
		float phaseG = SharedData::exponentialHeightFogSettings.volumetricFogScatteringDistribution;
		float phaseDenominator = 1.0f + phaseG * phaseG - 2.0f * phaseG * dot(normalize(SharedData::DirLightDirection.xyz), rayDirection);
		float phase = (1.0f - phaseG * phaseG) / (12.5663706f * pow(max(phaseDenominator, 1e-5f), 1.5f));
		float3 illumination = fogColor * (ambient + max(SharedData::DirLightColor.rgb, 0.0f.xxx) * phase);
		return float4(illumination * opacity, transmittance);
#else
		return float4(0.0f, 0.0f, 0.0f, 1.0f);
#endif
	}
}

#endif
