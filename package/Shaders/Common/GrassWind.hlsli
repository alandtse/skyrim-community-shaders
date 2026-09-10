#ifndef __GRASS_WIND_DEPENDENCY_HLSL__
#define __GRASS_WIND_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Permutation.hlsli"

namespace GrassWind
{
	float CalculateFlutterWave(float2 instanceCoordinates, float windTimer)
	{
		float windAngle = 0.4 * ((instanceCoordinates.x + instanceCoordinates.y) * -0.0078125 + windTimer);
		float windAngleSin, windAngleCos;
		sincos(windAngle, windAngleSin, windAngleCos);

		float windTmp3 = 0.2 * cos(Math::PI * windAngleCos);
		float windTmp1 = sin(Math::PI * windAngleSin);
		float windTmp2 = sin(Math::TAU * windAngleSin);
		return (windTmp1 + windTmp2) * 0.3 + windTmp3;
	}

	float GetWindIntensityOverrideScale()
	{
		return Permutation::OverrideWindIntensity != 0 ? Permutation::WindIntensityOverride : 1.0;
	}

	float3 RotateVector(float3 inputVector, float3 axis, float angle)
	{
		float angleSin, angleCos;
		sincos(angle, angleSin, angleCos);
		return inputVector * angleCos + cross(axis, inputVector) * angleSin +
		       axis * dot(axis, inputVector) * (1.0 - angleCos);
	}

	float3 CalculateVanillaDisplacement(
		float2 instanceCoordinates, float tipWeight, float3 windVector, float windTimer, float windIntensityScale)
	{
		float windPower = windVector.z * windIntensityScale *
		                  (CalculateFlutterWave(instanceCoordinates, windTimer) * (0.5 * (tipWeight * tipWeight)));

		return float3(windVector.xy, 0) * windPower;
	}

	/** @brief Deforms a grass vertex from an already-resolved rigid bend. */
	float3 CalculateAmbientDisplacement(
		float tipWeight, float modelHeight, float instanceBaseHeight, float3 bendAxis,
		float rigidBendAngle, float rigidCompression, out float bendAngle)
	{
		float squaredTipWeight = saturate(tipWeight);
		squaredTipWeight *= squaredTipWeight;
		float deformationWeight = lerp(
			1.0, squaredTipWeight, saturate(Permutation::GrassWindBendProfile));
		bendAngle = rigidBendAngle * deformationWeight;
		float compression = saturate(rigidCompression * deformationWeight);

		float3 relativePosition = float3(0.0, 0.0, max(modelHeight - instanceBaseHeight, 0.0));
		float3 deformedPosition = RotateVector(relativePosition, bendAxis, bendAngle);
		deformedPosition.z *= 1.0 - compression;
		return deformedPosition - relativePosition;
	}
}

#endif  // __GRASS_WIND_DEPENDENCY_HLSL__
