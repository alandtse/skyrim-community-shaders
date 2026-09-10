#ifndef __GRASS_WIND_RESPONSE_HLSLI__
#define __GRASS_WIND_RESPONSE_HLSLI__

#include "Common/GrassWind.hlsli"
#include "Common/GrassWindSpring.hlsli"
#include "Common/Random.hlsli"

namespace GrassWindResponse
{
	void Sample(float2 instanceCoordinates, float2 rootWorldPosition, float2 previousRootWorldPosition,
		float4x4 worldMatrix, float4x4 previousWorldMatrix, float windTimer, float previousWindTimer,
		out float4 currentResponse, out float4 previousResponse, out float2 flutter)
	{
		currentResponse = previousResponse = float4(0.0, 1.0, 0.0, 0.0);
		float intensityScale = GrassWind::GetWindIntensityOverrideScale();
		float currentFrequency = 1.0;
		float previousFrequency = 1.0;
		if (Permutation::EnableAmbientGrassWind != 0) {
			uint currentField = GrassWindSpring::SelectField(rootWorldPosition);
			uint previousField = GrassWindSpring::SelectField(previousRootWorldPosition);
			if (currentField == previousField && GrassWindSpring::HasTemporalCoverage(
													 currentField, previousField, rootWorldPosition, previousRootWorldPosition)) {
				float4 currentSample = GrassWindSpring::SampleCurrent(currentField, rootWorldPosition);
				float4 previousSample = GrassWindSpring::SamplePrevious(previousField, previousRootWorldPosition);
				float responseScale = lerp(0.9, 1.1, Random::InterleavedGradientNoise(instanceCoordinates));
				float3 currentAxis, previousAxis;
				GrassWindSpring::ResolveModelBend(currentSample, responseScale, worldMatrix,
					GrassWindSpring::Fields[currentField].MaximumTiltRadians,
					Permutation::GrassWindCompressionToBend, currentAxis, currentResponse.z, currentResponse.w);
				GrassWindSpring::ResolveModelBend(previousSample, responseScale, previousWorldMatrix,
					GrassWindSpring::Fields[previousField].MaximumTiltRadians,
					Permutation::GrassWindCompressionToBend, previousAxis, previousResponse.z, previousResponse.w);
				currentResponse.xy = currentAxis.xy;
				previousResponse.xy = previousAxis.xy;
				float inverseMaximumTilt = rcp(max(
					GrassWindSpring::Fields[currentField].MaximumTiltRadians, EPSILON_WIND_GEOMETRY));
				float currentWindResponse = saturate(max(length(currentSample.xy) * inverseMaximumTilt, currentSample.z));
				float previousWindResponse = saturate(max(length(previousSample.xy) * inverseMaximumTilt, previousSample.z));
				currentFrequency = lerp(1.0, max(Permutation::GrassWindFlutterFrequency, 1.0), currentWindResponse);
				previousFrequency = lerp(1.0, max(Permutation::GrassWindFlutterFrequency, 1.0), previousWindResponse);
				intensityScale *= max(Permutation::GrassWindFlutterStrength, 0.0) * max(Permutation::GrassWindSensitivity, 0.0);
			}
		}
		flutter = float2(
					  GrassWind::CalculateFlutterWave(instanceCoordinates, windTimer * currentFrequency),
					  GrassWind::CalculateFlutterWave(instanceCoordinates, previousWindTimer * previousFrequency)) *
		          intensityScale;
	}
}

#endif
