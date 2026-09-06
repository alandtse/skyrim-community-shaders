#ifndef __CHARACTER_RAIN_SPOTS_HLSLI__
#define __CHARACTER_RAIN_SPOTS_HLSLI__

#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

namespace CharacterRainSpots
{
	static const float BeadRadiusScale = 0.5f;
	static const float SettledBeadRadius = 0.27f;
	static const float ArrivingBeadRadius = 0.65f;
	static const float FlowBeadRadius = 0.6f;
	static const float FlowWidthInRadii = 0.22f;
	static const float MinimumSurfaceScale = 0.85f;
	static const float TwoPi = 6.28318530718f;
	static const float UintToUnit = 1.0f / 16777216.0f;

	/** @brief Measures posed surface distance while bounding expansion on compressed geometry. */
	float SurfaceDistanceSquared(float2 offset, float3 surfaceMetric)
	{
		float distanceSquared = dot(offset * offset, surfaceMetric.xz) + 2.0f * offset.x * offset.y * surfaceMetric.y;
		return max(distanceSquared, dot(offset, offset) * MinimumSurfaceScale * MinimumSurfaceScale);
	}

	/** @brief Returns a fixed-size asymmetric cap's filtered coverage and normalized physical height. */
	float2 BeadProfile(float2 offset, float radius, float baseRadius, float3 surfaceMetric, float footprint, float3 variation)
	{
		float2 unitOffset = offset / radius;
		float2 shapeAxis = variation.xy * 2.0f - 1.0f;
		shapeAxis *= rsqrt(max(dot(shapeAxis, shapeAxis), 1e-8f));
		float axisPosition = dot(unitOffset, shapeAxis);
		float2 pinchedOffset = unitOffset + shapeAxis * axisPosition * lerp(0.15f, 0.6f, variation.z);
		pinchedOffset += float2(shapeAxis.y, -shapeAxis.x) * saturate(axisPosition * axisPosition) * 0.15f;
		float radiusSquared = max(SurfaceDistanceSquared(unitOffset, surfaceMetric), SurfaceDistanceSquared(pinchedOffset, surfaceMetric));
		float edgeWidth = clamp(footprint * 0.5f / radius, 0.025f, 0.25f);
		float coverage = 1.0f - smoothstep(1.0f - edgeWidth, 1.0f + edgeWidth, sqrt(radiusSquared));
		float height = sqrt(saturate(1.0f - radiusSquared)) * radius / baseRadius;
		float detailFade = 1.0f - smoothstep(0.55f, 1.5f, footprint / radius);
		return float2(coverage, height) * detailFade;
	}

	/** @brief Adds small stable water beads whose placement has no time-dependent component. */
	float3 SettledBeads(float2 surfacePosition, float3 surfaceMetric, float footprint, float baseRadius, uint projectionIndex, float density)
	{
		float3 result = float3(0.0f, 0.0f, 0.0f);
		float cellSize = baseRadius * 2.0f;
		int2 cell = int2(floor(surfacePosition / cellSize));
		float3 variation = float3(Random::pcg3d(uint3(asuint(cell), projectionIndex + 191u)) >> 8u) * UintToUnit;
		float selection = smoothstep(variation.z * 0.9f, variation.z * 0.9f + 0.1f, density);
		[branch] if (selection > 0.0f)
		{
			float2 center = (float2(cell) + 0.5f + (variation.xy - 0.5f) * 0.4f) * cellSize;
			float radius = baseRadius * SettledBeadRadius * lerp(0.65f, 1.0f, variation.z);
			float2 bead = BeadProfile(surfacePosition - center, radius, baseRadius, surfaceMetric, footprint, variation);
			result = float3(bead, 0.0f) * selection;
		}
		return result;
	}

	/** @brief Adds independently timed fresh drops without enlarging or sliding their footprint. */
	float3 ArrivingDrops(float2 surfacePosition, float3 surfaceMetric, float footprint, float baseRadius, uint projectionIndex, float density)
	{
		float3 result = float3(0.0f, 0.0f, 0.0f);
		const SharedData::WetnessEffectsSettings settings = SharedData::wetnessEffectsSettings;
		float cellSize = baseRadius * 3.0f;
		int2 cell = int2(floor(surfacePosition / cellSize));
		uint3 cellHash = Random::pcg3d(uint3(asuint(cell), projectionIndex + 719u));
		float activityRate = sqrt(max(settings.CharacterRainActivityMultiplier, 0.25f));
		float lifetime = max(settings.CharacterSpotLifetime, 0.5f);
		float interval = max(settings.CharacterSpotInterval, lifetime * 1.1f);
		float cycle = max(SharedData::Timer, 0.0f) * activityRate / interval + float(cellHash.x >> 8u) * UintToUnit;
		float age = frac(cycle) * interval / lifetime;
		float3 variation = float3(Random::pcg3d(cellHash ^ uint3(uint(floor(cycle)), 0u, 0u)) >> 8u) * UintToUnit;
		float eventFade = smoothstep(variation.z * 0.9f, variation.z * 0.9f + 0.1f, density) *
		                  smoothstep(0.0f, 0.035f, age) * (1.0f - smoothstep(0.25f, 1.0f, age));
		[branch] if (eventFade > 0.0f)
		{
			float2 center = (float2(cell) + 0.5f + (variation.xy - 0.5f) * 0.35f) * cellSize;
			float radius = baseRadius * ArrivingBeadRadius * lerp(0.65f, 1.0f, variation.y);
			float2 bead = BeadProfile(surfacePosition - center, radius, baseRadius, surfaceMetric, footprint, variation);
			result = float3(bead, 0.0f) * eventFade;
		}
		return result;
	}

	/** @brief Warps a rivulet sideways with smooth low-frequency advection instead of reseeding it. */
	float FlowWarp(float position, float time, float phase, float baseRadius)
	{
		float wave = sin(position / max(baseRadius * 7.0f, 0.01f) + time * 0.45f + phase);
		wave += sin(position / max(baseRadius * 3.0f, 0.01f) - time * 0.3f + phase * 1.7f) * 0.4f;
		return wave * baseRadius * 0.65f * SharedData::wetnessEffectsSettings.CharacterFlowDistortion;
	}

	/** @brief Advects separate tapered rivulets with rounded heads, uneven necks, and thin residual trails. */
	float3 FlowingRivulets(float2 surfacePosition, float3 surfaceMetric, float footprint, float baseRadius, uint projectionIndex, float density)
	{
		float3 result = float3(0.0f, 0.0f, 0.0f);
		const SharedData::WetnessEffectsSettings settings = SharedData::wetnessEffectsSettings;
		[branch] if (projectionIndex < 2u && settings.CharacterDropTravel > 0.0f && settings.CharacterDropTrailLength > 0.0f)
		{
			float activityRate = sqrt(max(settings.CharacterRainActivityMultiplier, 0.25f));
			float lifetime = max(settings.CharacterSpotLifetime, 0.5f);
			float streakLength = settings.CharacterDropTrailLength;
			float2 cellSize = float2(baseRadius * 8.0f, streakLength + baseRadius * 6.0f);
			int column = int(floor(surfacePosition.x / cellSize.x));
			uint3 columnHash = Random::pcg3d(uint3(asuint(column), projectionIndex + 137u, 0u));
			float flowCycle = max(SharedData::Timer, 0.0f) * activityRate / lifetime + float(columnHash.x >> 8u) * UintToUnit;
			float pause = clamp(settings.CharacterDropPause / lifetime, 0.0f, 0.5f);
			float progress = smoothstep(pause, 1.0f, frac(flowCycle));
			float2 flowPosition = surfacePosition;
			flowPosition.y += (floor(flowCycle) + progress) * settings.CharacterDropTravel;
			int2 cell = int2(floor(flowPosition / cellSize));
			float3 variation = float3(Random::pcg3d(uint3(asuint(cell), projectionIndex + 1319u)) >> 8u) * UintToUnit;
			float selection = smoothstep(variation.z * 0.9f, variation.z * 0.9f + 0.1f, density);
			[branch] if (selection > 0.0f)
			{
				float2 localPosition = flowPosition - (float2(cell) + 0.5f) * cellSize;
				float headY = -streakLength * 0.5f + (variation.y - 0.5f) * baseRadius * 1.5f;
				float centerX = (variation.x - 0.5f) * cellSize.x * 0.2f;
				float phase = variation.z * TwoPi;
				float time = max(SharedData::Timer, 0.0f);
				float headX = centerX + FlowWarp(headY, time, phase, baseRadius);
				float2 head = BeadProfile(localPosition - float2(headX, headY), baseRadius * FlowBeadRadius,
					baseRadius, surfaceMetric, footprint, variation);
				float behindHead = localPosition.y - headY;
				float trailAge = saturate(behindHead / streakLength);
				float pathX = centerX + FlowWarp(localPosition.y, time, phase, baseRadius);
				float neck = lerp(0.65f, 1.0f, sin(behindHead / max(baseRadius * 2.0f, 0.01f) + phase) * 0.5f + 0.5f);
				float width = baseRadius * FlowWidthInRadii * lerp(1.0f, 0.3f, trailAge) * neck;
				float lateralScale = sqrt(max(surfaceMetric.x - surfaceMetric.y * surfaceMetric.y / max(surfaceMetric.z, 1e-8f),
					MinimumSurfaceScale * MinimumSurfaceScale));
				float lateralDistance = abs(localPosition.x - pathX) * lateralScale;
				float edgeWidth = clamp(footprint * 0.5f, baseRadius * 0.025f, baseRadius * 0.15f);
				float trailFade = smoothstep(-edgeWidth, edgeWidth, behindHead) * (1.0f - smoothstep(0.5f, 1.0f, trailAge));
				trailFade *= saturate(width / max(footprint * 0.5f, width)) * settings.CharacterDropTrailStrength;
				float trailCoverage = (1.0f - smoothstep(max(width - edgeWidth, 0.0f), width + edgeWidth, lateralDistance)) * trailFade;
				float trailHeight = sqrt(saturate(1.0f - lateralDistance * lateralDistance / max(width * width, 1e-8f))) * width / baseRadius * trailFade;
				float detailFade = 1.0f - smoothstep(0.75f, 1.75f, footprint / baseRadius);
				result = float3(max(head.x, trailCoverage), max(head.y, trailHeight), trailCoverage * (1.0f - head.x)) * selection * detailFade;
			}
		}
		return result;
	}

	/** @brief Blends independent settled, arriving, and flowing water layers with bounded per-cell work. */
	float3 EvaluateProjection(float2 surfacePosition, float3 surfaceMetric, float footprint, uint projectionIndex,
		float rainDensity, float settledDensity, float flowSlope, bool stationaryOnly)
	{
		const SharedData::WetnessEffectsSettings settings = SharedData::wetnessEffectsSettings;
		float configuredRadius = stationaryOnly ? settings.WeaponSpotRadius : settings.CharacterSpotRadius;
		float baseRadius = max(configuredRadius, 0.2f) * BeadRadiusScale;
		float3 settled = float3(0.0f, 0.0f, 0.0f);
		float3 arriving = float3(0.0f, 0.0f, 0.0f);
		float3 flowing = float3(0.0f, 0.0f, 0.0f);
		// Every profile fits its owning cell, so no neighboring-cell search is needed for these layers.
		float settledStrength = stationaryOnly ? 1.0f : settings.CharacterStaticBeadStrength;
		[branch] if (settledStrength > 0.0f)
			settled = SettledBeads(surfacePosition, surfaceMetric, footprint, baseRadius, projectionIndex, settledDensity) * settledStrength;
		[branch] if (!stationaryOnly && settings.CharacterImpactStrength > 0.0f)
			arriving = ArrivingDrops(surfacePosition, surfaceMetric, footprint, baseRadius, projectionIndex, rainDensity) * settings.CharacterImpactStrength;
		[branch] if (!stationaryOnly && settings.CharacterFlowStrength > 0.0f && flowSlope > 0.0f)
			flowing = FlowingRivulets(surfacePosition, surfaceMetric, footprint, baseRadius, projectionIndex, settledDensity) * settings.CharacterFlowStrength * flowSlope;
		if (settings.CharacterSpotDebug == 2u)
			return settled;
		if (settings.CharacterSpotDebug == 3u)
			return arriving;
		if (settings.CharacterSpotDebug == 4u)
			return flowing;
		float coverage = 1.0f - (1.0f - settled.x) * (1.0f - arriving.x) * (1.0f - flowing.x);
		return float3(coverage, max(settled.y, max(arriving.y, flowing.y)), flowing.z);
	}

	/** @brief Returns surface-attached coverage, water height, and trail absorption shared by both eyes. */
	float3 Evaluate(float3 modelPosition, float3 relativeWorldPosition, float3 worldNormal, float skyVisibility,
		bool inWorld, bool heldWeapon, uint eyeIndex)
	{
		const SharedData::WetnessEffectsSettings settings = SharedData::wetnessEffectsSettings;
		float retainedWetness = settings.CharacterRetainedWetness;
		float3 water = float3(0.0f, 0.0f, 0.0f);
		[branch] if (settings.EnableCharacterRainSpots && inWorld &&
					 max(settings.CharacterImpactIntensity, retainedWetness) > 0.0f &&
					 (!heldWeapon || settings.EnableWeaponRainDrops))
		{
			// Derivatives filter edges and select surface projections, but never seed or animate drops.
			float3 modelDx = ddx(modelPosition);
			float3 modelDy = ddy(modelPosition);
			float3 worldDx = ddx(relativeWorldPosition);
			float3 worldDy = ddy(relativeWorldPosition);
			float footprint = max(length(worldDx), length(worldDy));
			float3 projectionWeights = abs(cross(modelDx, modelDy));
			projectionWeights /= max(max(projectionWeights.x, max(projectionWeights.y, projectionWeights.z)), 1e-10f);
			projectionWeights *= projectionWeights;
			projectionWeights *= projectionWeights;
			projectionWeights /= max(dot(projectionWeights, 1.0f.xxx), 1e-10f);

			float3 headPosition = FrameBuffer::CameraPosAdjust[0].xyz;
#ifdef VR
			headPosition = (headPosition + FrameBuffer::CameraPosAdjust[1].xyz) * 0.5f;
#endif
			float3 headRelativePosition = relativeWorldPosition + (FrameBuffer::CameraPosAdjust[eyeIndex].xyz - headPosition);
			float distanceFade = 1.0f - smoothstep(settings.CharacterSpotRange * 0.7f, settings.CharacterSpotRange, length(headRelativePosition));
			float rainExposure = SharedData::HideSky ? 0.0f : smoothstep(0.25f, 0.65f, skyVisibility);
			const float3 rainArrival = float3(0.0f, 0.0f, 1.0f);
			float facing = dot(worldNormal, rainArrival);
			float facingCoverage = smoothstep(-0.35f, 0.1f, facing) *
			                       lerp(settings.CharacterSpotVerticalCoverage, 1.0f, smoothstep(0.0f, 0.75f, facing));
			facingCoverage = lerp(facingCoverage, 1.0f, saturate(settings.CharacterShowcaseCoverage));
			float visibility = distanceFade;
			float configuredDensity = heldWeapon ? settings.WeaponSpotDensity : settings.CharacterSpotDensity;
			[branch] if (visibility > 0.0f && rainExposure > 0.0f && configuredDensity > 0.0f)
			{
				float activity = max(settings.CharacterRainActivityMultiplier, 0.25f);
				float baseRainDensity = saturate(configuredDensity * settings.CharacterImpactIntensity * rainExposure * facingCoverage);
				float baseSettledDensity = saturate(configuredDensity * retainedWetness * lerp(0.35f, 1.0f, facingCoverage));
				float rainDensity = 1.0f - pow(1.0f - baseRainDensity, activity);
				float settledDensity = 1.0f - pow(1.0f - baseSettledDensity, activity);
				float flowSlope = smoothstep(0.15f, 0.75f, length(worldNormal.xy));
				[unroll] for (uint projectionIndex = 0u; projectionIndex < 3u; ++projectionIndex)
				{
					[branch] if (projectionWeights[projectionIndex] > 0.001f)
					{
						float2 surfacePosition = projectionIndex == 0u ? modelPosition.yz : (projectionIndex == 1u ? modelPosition.xz : modelPosition.xy);
						float2 surfaceDx = projectionIndex == 0u ? modelDx.yz : (projectionIndex == 1u ? modelDx.xz : modelDx.xy);
						float2 surfaceDy = projectionIndex == 0u ? modelDy.yz : (projectionIndex == 1u ? modelDy.xz : modelDy.xy);
						float determinant = surfaceDx.x * surfaceDy.y - surfaceDx.y * surfaceDy.x;
						float inverseDeterminant = abs(determinant) > 1e-8f ? rcp(determinant) : 0.0f;
						float3 surfaceU = (worldDx * surfaceDy.y - worldDy * surfaceDx.y) * inverseDeterminant;
						float3 surfaceV = (worldDy * surfaceDx.x - worldDx * surfaceDy.x) * inverseDeterminant;
						float3 surfaceMetric = abs(determinant) > 1e-8f ? float3(dot(surfaceU, surfaceU), dot(surfaceU, surfaceV), dot(surfaceV, surfaceV)) : float3(1, 0, 1);
						water += EvaluateProjection(surfacePosition, surfaceMetric, footprint, projectionIndex,
									 rainDensity, settledDensity, flowSlope, heldWeapon) *
						         projectionWeights[projectionIndex];
					}
				}
				water.xz *= visibility * rainExposure * (heldWeapon ? settings.WeaponSpotStrength : settings.CharacterSpotStrength);
			}
			// Shelter suppresses localized water here; the broad retained sheen is applied separately in Lighting.hlsl.
			// Coverage fades the coat contribution; flattening its height as well would attenuate normals twice.
		}
		return saturate(water);
	}
}

#endif
