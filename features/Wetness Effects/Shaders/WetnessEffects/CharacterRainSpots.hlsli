#ifndef __CHARACTER_RAIN_SPOTS_HLSLI__
#define __CHARACTER_RAIN_SPOTS_HLSLI__

#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
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
	static const float MinimumBeadRadius = 0.2f;
	static const float SpotLifetime = 3.0f;
	static const float SpotInterval = 4.0f;
	static const float SpotDrawDistance = 900.0f;
	static const float VerticalSurfaceCoverage = 0.45f;
	static const float DropTravel = 12.0f;
	static const float DropTrailLength = 12.0f;
	static const float DropTrailStrength = 0.8f;
	static const float DropPause = 0.35f;
	static const float FlowDistortion = 0.4225f;
	static const float WeaponRadiusScale = 0.8f;
	static const float MinimumActivityMultiplier = 0.25f;
	static const float ShapePinchMinimum = 0.15f;
	static const float ShapePinchMaximum = 0.6f;
	static const float ShapeBend = 0.15f;
	static const float ProfileEdgeMinimum = 0.025f;
	static const float ProfileEdgeMaximum = 0.25f;
	static const float ProfileDetailFadeStart = 0.55f;
	static const float ProfileDetailFadeEnd = 1.5f;
	static const float MinimumRadiusVariation = 0.65f;
	static const float DensitySelectionScale = 0.9f;
	static const float DensitySelectionWidth = 0.1f;
	static const float ImpactFadeInEnd = 0.035f;
	static const float ImpactFadeOutStart = 0.25f;
	static const float MinimumFlowWaveScale = 0.01f;
	static const float SettledCellScale = 2.0f;
	static const float FlowNeckWavelength = 2.0f;
	static const float ImpactCellScale = 3.0f;
	static const float SettledCenterJitter = 0.4f;
	static const float ImpactCenterJitter = 0.35f;
	static const float PrimaryFlowWavelength = 7.0f;
	static const float SecondaryFlowWavelength = 3.0f;
	static const float PrimaryFlowSpeed = 0.45f;
	static const float SecondaryFlowSpeed = 0.3f;
	static const float SecondaryFlowPhaseScale = 1.7f;
	static const float SecondaryFlowAmplitude = 0.4f;
	static const float FlowCellWidth = 8.0f;
	static const float FlowCellPadding = 6.0f;
	static const float FlowHeadJitter = 1.5f;
	static const float FlowCenterJitter = 0.2f;
	static const float FlowNeckMinimum = 0.65f;
	static const float FlowTrailEndWidth = 0.3f;
	static const float TrailEdgeMinimumInRadii = 0.025f;
	static const float TrailEdgeMaximumInRadii = 0.15f;
	static const float TrailFadeOutStart = 0.5f;
	static const float TrailDetailFadeStart = 0.75f;
	static const float TrailDetailFadeEnd = 1.75f;
	static const float DrawDistanceFadeStart = 0.7f;
	static const float SkyVisibilityFadeStart = 0.25f;
	static const float SkyVisibilityFadeEnd = 0.65f;
	static const float SurfaceFacingFadeStart = -0.35f;
	static const float SurfaceFacingFadeEnd = 0.1f;
	static const float UpwardFacingEnd = 0.75f;
	static const float MinimumSettledFacingDensity = 0.35f;
	static const float FlowSlopeFadeStart = 0.15f;
	static const float FlowSlopeFadeEnd = 0.75f;
	static const float MinimumProjectionWeight = 0.001f;
	static const uint SlidingProjectionCount = 2u;
	static const uint SurfaceProjectionCount = 3u;
	static const uint SettledDebugMode = 2u;
	static const uint ImpactDebugMode = 3u;
	static const uint FlowDebugMode = 4u;
	static const uint SurfaceDebugMode = 5u;
	static const uint WeaponDebugMode = 6u;
	static const uint SettledHashSeed = 191u;
	static const uint ImpactHashSeed = 719u;
	static const uint FlowColumnHashSeed = 137u;
	static const uint FlowCellHashSeed = 1319u;
	static const uint HashFractionShift = 8u;
	static const float UintToUnit = 1.0f / 16777216.0f;

	float3 HashToUnitFloat3(uint3 seed)
	{
		return float3(Random::pcg3d(seed) >> HashFractionShift) * UintToUnit;
	}

	/** @brief Maps the activity control to the temporal rate used by drop animation. */
	float GetActivityRate(float activityMultiplier)
	{
		float activity = max(activityMultiplier, MinimumActivityMultiplier);
		return sqrt(activity);
	}

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
		shapeAxis *= rsqrt(max(dot(shapeAxis, shapeAxis), EPSILON_LENGTH_SQ));
		float axisPosition = dot(unitOffset, shapeAxis);
		float2 pinchedOffset = unitOffset + shapeAxis * axisPosition * lerp(ShapePinchMinimum, ShapePinchMaximum, variation.z);
		pinchedOffset += float2(shapeAxis.y, -shapeAxis.x) * saturate(axisPosition * axisPosition) * ShapeBend;
		float radiusSquared = max(SurfaceDistanceSquared(unitOffset, surfaceMetric), SurfaceDistanceSquared(pinchedOffset, surfaceMetric));
		float edgeWidth = clamp(footprint * 0.5f / radius, ProfileEdgeMinimum, ProfileEdgeMaximum);
		float coverage = 1.0f - smoothstep(1.0f - edgeWidth, 1.0f + edgeWidth, sqrt(radiusSquared));
		float height = sqrt(saturate(1.0f - radiusSquared)) * radius / baseRadius;
		float detailFade = 1.0f - smoothstep(ProfileDetailFadeStart, ProfileDetailFadeEnd, footprint / radius);
		return float2(coverage, height) * detailFade;
	}

	/** @brief Evaluates a bead layer from its caller-specific placement and intensity. */
	float3 EvaluateBead(float2 surfacePosition, float3 surfaceMetric, float footprint, float baseRadius,
		int2 cell, float cellSize, float centerJitter, float radiusScale, float radiusVariation,
		float3 variation, float intensity)
	{
		float3 result = float3(0.0f, 0.0f, 0.0f);
		[branch] if (intensity > 0.0f)
		{
			float2 center = (float2(cell) + 0.5f + (variation.xy - 0.5f) * centerJitter) * cellSize;
			float radius = baseRadius * radiusScale * lerp(MinimumRadiusVariation, 1.0f, radiusVariation);
			float2 bead = BeadProfile(surfacePosition - center, radius, baseRadius, surfaceMetric, footprint, variation);
			result = float3(bead, 0.0f) * intensity;
		}
		return result;
	}

	/** @brief Adds small stable water beads whose placement has no time-dependent component. */
	float3 SettledBeads(float2 surfacePosition, float3 surfaceMetric, float footprint, float baseRadius, uint projectionIndex, float density)
	{
		float cellSize = baseRadius * SettledCellScale;
		int2 cell = int2(floor(surfacePosition / cellSize));
		float3 variation = HashToUnitFloat3(uint3(asuint(cell), projectionIndex + SettledHashSeed));
		float selection = smoothstep(variation.z * DensitySelectionScale,
			variation.z * DensitySelectionScale + DensitySelectionWidth, density);
		return EvaluateBead(surfacePosition, surfaceMetric, footprint, baseRadius,
			cell, cellSize, SettledCenterJitter, SettledBeadRadius, variation.z, variation, selection);
	}

	/** @brief Adds independently timed fresh drops without enlarging or sliding their footprint. */
	float3 ArrivingDrops(float2 surfacePosition, float3 surfaceMetric, float footprint, float baseRadius, uint projectionIndex, float density)
	{
		const SharedData::WetnessEffectsSettings settings = SharedData::wetnessEffectsSettings;
		float cellSize = baseRadius * ImpactCellScale;
		int2 cell = int2(floor(surfacePosition / cellSize));
		uint3 cellHash = Random::pcg3d(uint3(asuint(cell), projectionIndex + ImpactHashSeed));
		float activityRate = GetActivityRate(settings.CharacterRainActivityMultiplier);
		float cycle = max(SharedData::Timer, 0.0f) * activityRate / SpotInterval + float(cellHash.x >> HashFractionShift) * UintToUnit;
		float age = frac(cycle) * SpotInterval / SpotLifetime;
		float3 variation = HashToUnitFloat3(cellHash ^ uint3(uint(floor(cycle)), 0u, 0u));
		float eventFade = smoothstep(variation.z * DensitySelectionScale,
							  variation.z * DensitySelectionScale + DensitySelectionWidth, density) *
		                  smoothstep(0.0f, ImpactFadeInEnd, age) * (1.0f - smoothstep(ImpactFadeOutStart, 1.0f, age));
		return EvaluateBead(surfacePosition, surfaceMetric, footprint, baseRadius,
			cell, cellSize, ImpactCenterJitter, ArrivingBeadRadius, variation.y, variation, eventFade);
	}

	/** @brief Warps a rivulet sideways with smooth low-frequency advection instead of reseeding it. */
	float FlowWarp(float position, float time, float phase, float baseRadius)
	{
		float wave = sin(position / max(baseRadius * PrimaryFlowWavelength, MinimumFlowWaveScale) + time * PrimaryFlowSpeed + phase);
		wave += sin(position / max(baseRadius * SecondaryFlowWavelength, MinimumFlowWaveScale) - time * SecondaryFlowSpeed + phase * SecondaryFlowPhaseScale) * SecondaryFlowAmplitude;
		return wave * baseRadius * FlowDistortion;
	}

	/** @brief Advects separate tapered rivulets with rounded heads, uneven necks, and thin residual trails. */
	float3 FlowingRivulets(float2 surfacePosition, float3 surfaceMetric, float footprint, float baseRadius, uint projectionIndex, float density)
	{
		float3 result = float3(0.0f, 0.0f, 0.0f);
		const SharedData::WetnessEffectsSettings settings = SharedData::wetnessEffectsSettings;
		[branch] if (projectionIndex < SlidingProjectionCount)
		{
			float activityRate = GetActivityRate(settings.CharacterRainActivityMultiplier);
			float streakLength = DropTrailLength;
			float2 cellSize = float2(baseRadius * FlowCellWidth, streakLength + baseRadius * FlowCellPadding);
			int column = int(floor(surfacePosition.x / cellSize.x));
			uint3 columnHash = Random::pcg3d(uint3(asuint(column), projectionIndex + FlowColumnHashSeed, 0u));
			float flowCycle = max(SharedData::Timer, 0.0f) * activityRate / SpotLifetime + float(columnHash.x >> HashFractionShift) * UintToUnit;
			float pause = DropPause / SpotLifetime;
			float progress = smoothstep(pause, 1.0f, frac(flowCycle));
			float2 flowPosition = surfacePosition;
			flowPosition.y += (floor(flowCycle) + progress) * DropTravel;
			int2 cell = int2(floor(flowPosition / cellSize));
			float3 variation = HashToUnitFloat3(uint3(asuint(cell), projectionIndex + FlowCellHashSeed));
			float selection = smoothstep(variation.z * DensitySelectionScale,
				variation.z * DensitySelectionScale + DensitySelectionWidth, density);
			[branch] if (selection > 0.0f)
			{
				float2 localPosition = flowPosition - (float2(cell) + 0.5f) * cellSize;
				float headY = -streakLength * 0.5f + (variation.y - 0.5f) * baseRadius * FlowHeadJitter;
				float centerX = (variation.x - 0.5f) * cellSize.x * FlowCenterJitter;
				float phase = variation.z * Math::TAU;
				float time = max(SharedData::Timer, 0.0f);
				float headX = centerX + FlowWarp(headY, time, phase, baseRadius);
				float2 head = BeadProfile(localPosition - float2(headX, headY), baseRadius * FlowBeadRadius,
					baseRadius, surfaceMetric, footprint, variation);
				float behindHead = localPosition.y - headY;
				float trailAge = saturate(behindHead / streakLength);
				float pathX = centerX + FlowWarp(localPosition.y, time, phase, baseRadius);
				float neck = lerp(FlowNeckMinimum, 1.0f,
					sin(behindHead / max(baseRadius * FlowNeckWavelength, MinimumFlowWaveScale) + phase) * 0.5f + 0.5f);
				float width = baseRadius * FlowWidthInRadii * lerp(1.0f, FlowTrailEndWidth, trailAge) * neck;
				float lateralScale = sqrt(max(surfaceMetric.x - surfaceMetric.y * surfaceMetric.y / max(surfaceMetric.z, EPSILON_DIVISION),
					MinimumSurfaceScale * MinimumSurfaceScale));
				float lateralDistance = abs(localPosition.x - pathX) * lateralScale;
				float edgeWidth = clamp(footprint * 0.5f,
					baseRadius * TrailEdgeMinimumInRadii, baseRadius * TrailEdgeMaximumInRadii);
				float trailFade = smoothstep(-edgeWidth, edgeWidth, behindHead) *
				                  (1.0f - smoothstep(TrailFadeOutStart, 1.0f, trailAge));
				trailFade *= saturate(width / max(footprint * 0.5f, width)) * DropTrailStrength;
				float trailCoverage = (1.0f - smoothstep(max(width - edgeWidth, 0.0f), width + edgeWidth, lateralDistance)) * trailFade;
				float trailHeight = sqrt(saturate(1.0f - lateralDistance * lateralDistance / max(width * width, EPSILON_DIVISION))) * width / baseRadius * trailFade;
				float detailFade = 1.0f - smoothstep(TrailDetailFadeStart, TrailDetailFadeEnd, footprint / baseRadius);
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
		float configuredRadius = settings.CharacterSpotRadius * (stationaryOnly ? WeaponRadiusScale : 1.0f);
		float baseRadius = max(configuredRadius, MinimumBeadRadius) * BeadRadiusScale;
		float3 settled = float3(0.0f, 0.0f, 0.0f);
		float3 arriving = float3(0.0f, 0.0f, 0.0f);
		float3 flowing = float3(0.0f, 0.0f, 0.0f);
		// Every profile fits its owning cell, so no neighboring-cell search is needed for these layers.
		settled = SettledBeads(surfacePosition, surfaceMetric, footprint, baseRadius, projectionIndex, settledDensity);
		[branch] if (!stationaryOnly)
			arriving = ArrivingDrops(surfacePosition, surfaceMetric, footprint, baseRadius, projectionIndex, rainDensity);
		[branch] if (!stationaryOnly && flowSlope > 0.0f)
			flowing = FlowingRivulets(surfacePosition, surfaceMetric, footprint, baseRadius, projectionIndex, settledDensity) * flowSlope;
		if (settings.CharacterSpotDebug == SettledDebugMode)
			return settled;
		if (settings.CharacterSpotDebug == ImpactDebugMode)
			return arriving;
		if (settings.CharacterSpotDebug == FlowDebugMode)
			return flowing;
		float coverage = 1.0f - (1.0f - settled.x) * (1.0f - arriving.x) * (1.0f - flowing.x);
		return float3(coverage, max(settled.y, max(arriving.y, flowing.y)), flowing.z);
	}

	/** @brief Returns surface-attached coverage, water height, and trail absorption shared by both eyes. */
	float3 Evaluate(float3 modelPosition, float3 relativeWorldPosition, float3 worldNormal, float skyVisibility,
		bool heldWeapon, uint eyeIndex)
	{
		const SharedData::WetnessEffectsSettings settings = SharedData::wetnessEffectsSettings;
		float retainedWetness = settings.CharacterRetainedWetness;
		float3 water = float3(0.0f, 0.0f, 0.0f);
		{
			// Derivatives filter edges and select surface projections, but never seed or animate drops.
			float3 modelDx = ddx(modelPosition);
			float3 modelDy = ddy(modelPosition);
			float3 worldDx = ddx(relativeWorldPosition);
			float3 worldDy = ddy(relativeWorldPosition);
			float footprint = max(length(worldDx), length(worldDy));
			float3 projectionWeights = abs(cross(modelDx, modelDy));
			projectionWeights /= max(max(projectionWeights.x, max(projectionWeights.y, projectionWeights.z)), EPSILON_WEIGHT_SUM);
			projectionWeights *= projectionWeights;
			projectionWeights *= projectionWeights;
			projectionWeights /= max(dot(projectionWeights, 1.0f.xxx), EPSILON_WEIGHT_SUM);

			float3 headPosition = FrameBuffer::CameraPosAdjust[0].xyz;
#ifdef VR
			headPosition = (headPosition + FrameBuffer::CameraPosAdjust[1].xyz) * 0.5f;
#endif
			float3 headRelativePosition = relativeWorldPosition + (FrameBuffer::CameraPosAdjust[eyeIndex].xyz - headPosition);
			float distanceFade = 1.0f - smoothstep(SpotDrawDistance * DrawDistanceFadeStart,
											SpotDrawDistance, length(headRelativePosition));
			float rainExposure = smoothstep(SkyVisibilityFadeStart, SkyVisibilityFadeEnd, skyVisibility);
			const float3 rainArrival = float3(0.0f, 0.0f, 1.0f);
			float facing = dot(worldNormal, rainArrival);
			float facingCoverage = smoothstep(SurfaceFacingFadeStart, SurfaceFacingFadeEnd, facing) *
			                       lerp(VerticalSurfaceCoverage, 1.0f, smoothstep(0.0f, UpwardFacingEnd, facing));
			float visibility = distanceFade;
			float configuredDensity = settings.CharacterSpotDensity;
			[branch] if (visibility > 0.0f && rainExposure > 0.0f && configuredDensity > 0.0f)
			{
				float activity = max(settings.CharacterRainActivityMultiplier, MinimumActivityMultiplier);
				float baseRainDensity = saturate(configuredDensity * settings.CharacterImpactIntensity * rainExposure * facingCoverage);
				float baseSettledDensity = saturate(configuredDensity * retainedWetness * lerp(MinimumSettledFacingDensity, 1.0f, facingCoverage));
				float rainDensity = 1.0f - pow(1.0f - baseRainDensity, activity);
				float settledDensity = 1.0f - pow(1.0f - baseSettledDensity, activity);
				float flowSlope = smoothstep(FlowSlopeFadeStart, FlowSlopeFadeEnd, length(worldNormal.xy));
				[unroll] for (uint projectionIndex = 0u; projectionIndex < SurfaceProjectionCount; ++projectionIndex)
				{
					[branch] if (projectionWeights[projectionIndex] > MinimumProjectionWeight)
					{
						float2 surfacePosition = projectionIndex == 0u ? modelPosition.yz : (projectionIndex == 1u ? modelPosition.xz : modelPosition.xy);
						float2 surfaceDx = projectionIndex == 0u ? modelDx.yz : (projectionIndex == 1u ? modelDx.xz : modelDx.xy);
						float2 surfaceDy = projectionIndex == 0u ? modelDy.yz : (projectionIndex == 1u ? modelDy.xz : modelDy.xy);
						float determinant = surfaceDx.x * surfaceDy.y - surfaceDx.y * surfaceDy.x;
						float inverseDeterminant = abs(determinant) > EPSILON_DIVISION ? rcp(determinant) : 0.0f;
						float3 surfaceU = (worldDx * surfaceDy.y - worldDy * surfaceDx.y) * inverseDeterminant;
						float3 surfaceV = (worldDy * surfaceDx.x - worldDx * surfaceDy.x) * inverseDeterminant;
						float3 surfaceMetric = abs(determinant) > EPSILON_DIVISION ? float3(dot(surfaceU, surfaceU), dot(surfaceU, surfaceV), dot(surfaceV, surfaceV)) : float3(1, 0, 1);
						water += EvaluateProjection(surfacePosition, surfaceMetric, footprint, projectionIndex,
									 rainDensity, settledDensity, flowSlope, heldWeapon) *
						         projectionWeights[projectionIndex];
					}
				}
				water.xz *= visibility * rainExposure * settings.CharacterSpotStrength;
			}
			// Shelter suppresses localized water here; the broad retained sheen is applied separately in Lighting.hlsl.
			// Coverage fades the coat contribution; flattening its height as well would attenuate normals twice.
		}
		return saturate(water);
	}
}

#endif
