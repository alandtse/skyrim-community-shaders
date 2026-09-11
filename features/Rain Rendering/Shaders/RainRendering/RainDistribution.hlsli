#ifndef RAIN_DISTRIBUTION_HLSLI
#define RAIN_DISTRIBUTION_HLSLI

#include "Common/ValueNoise.hlsli"
#include "RainRendering/RainConstants.hlsli"

namespace RainDistribution
{
	static const float MinimumDetailScale = 64.0f;
	static const float LayerDetailScale = 0.5f;
	static const float3 CurtainDirection = float3(0.8192319f, 0.5734624f, 0.0f);

	struct Field
	{
		float SpatialDensity;
		float CurtainShape;
		float Weight;
	};

	/** @brief Samples world-anchored clusters and elongated precipitation bands at a spawn position. */
	Field Sample(float3 position, float layerRadius)
	{
		Field field;
		field.SpatialDensity = 1.0f;
		field.CurtainShape = 0.0f;
		float detailScale = max(layerRadius * LayerDetailScale, MinimumDetailScale);
		[branch] if (DistanceNoise.z > 0.0f || GridAndDebug.w == 3u)
		{
			float noiseScale = max(min(DistanceNoise.y, detailScale), MinimumDetailScale);
			float noise = ValueNoise::Sample(position / noiseScale, 0xD1B54A35u);
			field.SpatialDensity = lerp(1.0f, lerp(0.32f, 1.68f, noise), saturate(DistanceNoise.z));
		}
		float curtainFactor = 1.0f;
		[branch] if (Curtain.y > 0.0f || GridAndDebug.w == 4u)
		{
			float curtainScale = max(min(Curtain.x, detailScale * 2.0f), MinimumDetailScale);
			float3 curtainCoordinate = float3(
				dot(position, CurtainDirection) / (curtainScale * 0.28f),
				dot(position.xy, float2(-CurtainDirection.y, CurtainDirection.x)) / curtainScale,
				position.z / (curtainScale * 2.0f));
			float noise = ValueNoise::Sample(curtainCoordinate, 0x94D049BBu);
			field.CurtainShape = pow(saturate(noise), max(Curtain.z, 0.1f));
			float curtainDensity = lerp(min(CurtainDensity.x, CurtainDensity.y), max(CurtainDensity.x, CurtainDensity.y), field.CurtainShape);
			curtainFactor = lerp(1.0f, curtainDensity, saturate(Curtain.y));
		}
		field.Weight = field.SpatialDensity * curtainFactor;
		return field;
	}

	/** @brief Biases birth positions without requiring extra particles or changing their fall velocity. */
	float3 SelectSpawn(float3 first, float3 second, float layerRadius, uint lifeSeed, out Field field, out float meanWeight)
	{
		Field firstField = Sample(first, layerRadius);
		Field secondField = Sample(second, layerRadius);
		float totalWeight = firstField.Weight + secondField.Weight;
		bool chooseSecond = Hash::Float01(lifeSeed ^ 0x243F6A89u) * totalWeight < secondField.Weight;
		field = firstField;
		if (chooseSecond)
			field = secondField;
		meanWeight = totalWeight * 0.5f;
		return chooseSecond ? second : first;
	}

	/** @brief Keeps the full budget at maximum density; spawn redistribution supplies its variation. */
	float Acceptance(float density, float meanWeight)
	{
		if (meanWeight <= 0.0f)
			return 0.0f;
		float baseAcceptance = saturate(density);
		return saturate(baseAcceptance + min(baseAcceptance, 1.0f - baseAcceptance) * (meanWeight - 1.0f));
	}
}

#endif
