#include "Common/FrameBuffer.hlsli"
#include "Common/Hash.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/StereoSampling.hlsli"
#include "RainRendering/RainConstants.hlsli"
#include "RainRendering/RainDistribution.hlsli"

#include "RainRendering/RainLighting.hlsli"
#if defined(RAIN_SKYLIGHTING_OCCLUSION) && defined(COMPUTESHADER)
#	include "RainRendering/RainRoofOcclusion.hlsli"
#endif

#define RAIN_COMPUTE_GROUP_SIZE 128
#define RAIN_PREFIX_GROUP_SIZE 1024

struct RainDrop
{
	float4 PositionLength;
	float4 VelocityWidth;
	float4 ColorOpacity;
	float4 LightDirection;
};

RWStructuredBuffer<RainDrop> RainDropsRW : register(u0);
StructuredBuffer<RainDrop> RainDrops : register(t1);
RWStructuredBuffer<uint> RainLocalOffsetsRW : register(u1);
RWStructuredBuffer<uint> RainGroupOffsetsRW : register(u2);
RWStructuredBuffer<uint> RainVisibleDropIndicesRW : register(u3);
RWByteAddressBuffer RainIndirectArgsRW : register(u4);
StructuredBuffer<uint> RainCompactionData : register(t39);
StructuredBuffer<uint> RainGroupOffsets : register(t40);
struct RainCachedLight
{
	float4 Position;
	float4 Irradiance;
	float4 Direction;
};
RWStructuredBuffer<uint> RainLightClaimsRW : register(u5);
RWStructuredBuffer<RainCachedLight> RainLightCacheRW : register(u6);
StructuredBuffer<uint> RainLightClaims : register(t41);
StructuredBuffer<RainCachedLight> RainLightCache : register(t42);

Texture2D<float> SceneDepth : register(t0);
Texture2D<float4> SceneColor : register(t2);
Texture2D<float> HalfResolutionSceneDepth : register(t5);
SamplerState RefractionSampler : register(s0);

struct RainSceneColorVertexOutput
{
	float4 Position: SV_Position;
};

RainSceneColorVertexOutput RainSceneColorVS(uint vertexID : SV_VertexID)
{
	RainSceneColorVertexOutput output;
	float2 corner = float2((vertexID << 1u) & 2u, vertexID & 2u);
	output.Position = float4(corner * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	return output;
}

struct RainSceneColorPixelOutput
{
	float4 Color: SV_Target0;
	float Depth: SV_Target1;
};

RainSceneColorPixelOutput RainSceneColorPS(RainSceneColorVertexOutput input)
{
	uint sourceWidth;
	uint sourceHeight;
	SceneColor.GetDimensions(sourceWidth, sourceHeight);
	float2 sourcePixel = input.Position.xy * 2.0f;
	float2 sourceMinimum = 0.5f.xx;
	float2 sourceMaximum = min(float2(sourceWidth, sourceHeight), ceil(ScreenSize.xy)) - 0.5f;
#ifdef VR
	float sourceEyeWidth = ceil(ScreenSize.x * 0.5f);
	float targetEyeWidth = ceil(sourceEyeWidth * 0.5f);
	float eyeIndex = input.Position.x >= targetEyeWidth ? 1.0f : 0.0f;
	sourcePixel.x = (input.Position.x - eyeIndex * targetEyeWidth) * 2.0f + eyeIndex * sourceEyeWidth;
	sourceMinimum.x = eyeIndex * sourceEyeWidth + 0.5f;
	sourceMaximum.x = min((eyeIndex + 1.0f) * sourceEyeWidth, float(sourceWidth)) - 0.5f;
#endif
	sourcePixel = clamp(sourcePixel, sourceMinimum, sourceMaximum);
	int2 sourceBase = int2(floor(sourcePixel - 0.5f));
	int2 minimumPixel = int2(ceil(sourceMinimum - 0.5f));
	int2 maximumPixel = int2(floor(sourceMaximum - 0.5f));
	float depth00 = SceneDepth.Load(int3(clamp(sourceBase, minimumPixel, maximumPixel), 0));
	float depth10 = SceneDepth.Load(int3(clamp(sourceBase + int2(1, 0), minimumPixel, maximumPixel), 0));
	float depth01 = SceneDepth.Load(int3(clamp(sourceBase + int2(0, 1), minimumPixel, maximumPixel), 0));
	float depth11 = SceneDepth.Load(int3(clamp(sourceBase + 1, minimumPixel, maximumPixel), 0));
	RainSceneColorPixelOutput output;
	output.Color = SceneColor.SampleLevel(RefractionSampler, sourcePixel / float2(sourceWidth, sourceHeight), 0.0f);
	output.Depth = min(min(
						   RainCameraData.w / max(-depth00 * RainCameraData.z + RainCameraData.x, EPSILON_DIVISION),
						   RainCameraData.w / max(-depth10 * RainCameraData.z + RainCameraData.x, EPSILON_DIVISION)),
		min(
			RainCameraData.w / max(-depth01 * RainCameraData.z + RainCameraData.x, EPSILON_DIVISION),
			RainCameraData.w / max(-depth11 * RainCameraData.z + RainCameraData.x, EPSILON_DIVISION)));
	return output;
}

int PositiveModulo(int value, int divisor)
{
	int result = value % divisor;
	return result < 0 ? result + divisor : result;
}

int3 SelectWorldCell(uint3 slot, int3 baseCell, uint3 dimensions)
{
	int3 dimension = int3(dimensions);
	int3 baseSlot = int3(
		PositiveModulo(baseCell.x, dimension.x),
		PositiveModulo(baseCell.y, dimension.y),
		PositiveModulo(baseCell.z, dimension.z));
	int3 offset = int3(slot) - baseSlot;
	offset += int3(offset.x < 0 ? dimension.x : 0, offset.y < 0 ? dimension.y : 0, offset.z < 0 ? dimension.z : 0);
	return baseCell + offset;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
	float lengthSquared = dot(value, value);
	return lengthSquared > EPSILON_DIVISION ? value * rsqrt(lengthSquared) : fallback;
}

void RejectDrop(uint dropIndex)
{
	// Compaction checks only opacity; other fields are valid only for visible drops.
	RainDropsRW[dropIndex].ColorOpacity.a = 0.0f;
}

static const float RainRefractionDepthFade = 12.0f;

bool RainCapsuleOutsidePlane(float4 plane, float3 startPosition, float3 endPosition, float projectedRadius)
{
	return dot(plane, float4(startPosition, 1.0f)) < -projectedRadius &&
	       dot(plane, float4(endPosition, 1.0f)) < -projectedRadius;
}

bool RainStreakIntersectsEyeFrustum(float3 position, float3 axis, float halfLength, float radius, uint eyeIndex)
{
	float3 cameraPosition = FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	float3 startPosition = position - axis * halfLength - cameraPosition;
	float3 endPosition = position + axis * halfLength - cameraPosition;
	[unroll] for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
	{
		uint index = eyeIndex * 6u + planeIndex;
		float projectedRadius = radius * RainFrustumPlaneLengths[index / 4u][index % 4u];
		if (RainCapsuleOutsidePlane(RainFrustumPlanes[index], startPosition, endPosition, projectedRadius))
			return false;
	}
	return true;
}

bool RainStreakIntersectsActiveFrustum(float3 position, float3 axis, float halfLength, float radius)
{
#ifdef VR
	// A shared visible list keeps peripheral drops stable when only one eye sees them.
	return RainStreakIntersectsEyeFrustum(position, axis, halfLength, radius, 0u) ||
	       RainStreakIntersectsEyeFrustum(position, axis, halfLength, radius, 1u);
#else
	return RainStreakIntersectsEyeFrustum(position, axis, halfLength, radius, 0u);
#endif
}

// Must match RainRendering::kRainLightCacheSize, which sizes both cache buffers.
static const uint RainLightCacheSize = 16384u;
static const float RainLightCellSize = 128.0f;
static const float RainIndividualLightDistance = 1024.0f;

int3 RainLightCell(float3 position)
{
	return int3(floor(position / RainLightCellSize));
}

uint RainLightBucket(int3 cell)
{
	return Hash::LowBias32(asuint(cell.x) ^ Hash::LowBias32(asuint(cell.y)) ^ Hash::LowBias32(asuint(cell.z) + 0x9E3779B9u)) & (RainLightCacheSize - 1u);
}

bool RainUsesSharedLighting(float headDistance)
{
	return MaterialLighting.w > 0.5f && LocalLighting.x > 0.0f && headDistance >= RainIndividualLightDistance && headDistance < LocalLighting.y;
}

[numthreads(RAIN_COMPUTE_GROUP_SIZE, 1, 1)] void RainUpdateCS(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint dropIndex = dispatchThreadID.x;
	if (dropIndex >= LayerCounts.w)
		return;
	uint layerIndex = dropIndex < LayerCounts.x ? 0u : (dropIndex < LayerCounts.x + LayerCounts.y ? 1u : 2u);
	uint layerStart = layerIndex == 0u ? 0u : (layerIndex == 1u ? LayerCounts.x : LayerCounts.x + LayerCounts.y);
	uint layerDropIndex = dropIndex - layerStart;
	uint overheadDropCount = min(uint(max(RoofOcclusion.w, 0.0f) + 0.5f), LayerCounts.x);
	bool isOverheadDrop = layerIndex == 0u && layerDropIndex < overheadDropCount;
	float layerRadius = LayerRadii[layerIndex];
	uint3 gridDimensions = max(GridAndDebug.xyz, uint3(1u, 1u, 1u));
	uint totalCellCount = gridDimensions.x * gridDimensions.y * gridDimensions.z;
	uint slotIndex = Hash::LowBias32(layerDropIndex ^ Hash::LowBias32(layerIndex + 0xD7A38E91u)) % totalCellCount;
	uint3 slot;
	slot.x = slotIndex % gridDimensions.x;
	slot.y = (slotIndex / gridDimensions.x) % gridDimensions.y;
	slot.z = slotIndex / (gridDimensions.x * gridDimensions.y);

	float3 volumeSize = max(VolumeSizeAndDensity.xyz * (layerRadius / max(LayerRadii.z, 1.0f)), 1.0f.xxx);
	// A spare cell keeps recycling outside the spherical layer when its world-cell anchor advances.
	float3 cellSize = volumeSize / max(float3(gridDimensions) - 1.0f, 1.0f.xxx);
	float3 volumeMinimum = HeadPositionAndTime.xyz - volumeSize * 0.5f;
	int3 baseCell = int3(floor(volumeMinimum / cellSize));
	int3 worldCell = SelectWorldCell(slot, baseCell, gridDimensions);
	uint cellSeed = Hash::LowBias32(layerDropIndex ^ Hash::LowBias32(layerIndex + 0xD7A38E91u) ^ Hash::LowBias32(asuint(worldCell.x)) ^
									Hash::LowBias32(asuint(worldCell.y) + 0x9E3779B9u) ^ Hash::LowBias32(asuint(worldCell.z) + 0x85EBCA6Bu));
	float overheadRadius = 0.0f;
	float overheadHeight = 0.0f;
	float overheadBottom = 0.0f;
	float fallCycleHeight = cellSize.z;
	if (isOverheadDrop) {
		uint3 overheadDimensions = uint3(8u, 8u, 1u);
		uint overheadSlotIndex = Hash::LowBias32(layerDropIndex ^ 0xA24BAED5u) % 64u;
		uint3 overheadSlot = uint3(overheadSlotIndex % 8u, overheadSlotIndex / 8u, 0u);
		overheadRadius = 450.0f;
		overheadHeight = 900.0f;
		overheadBottom = 100.0f;
		float overheadCellSize = overheadRadius * 0.25f;
		float3 overheadCellDimensions = float3(overheadCellSize, overheadCellSize, overheadHeight);
		float3 overheadMinimum = float3(HeadPositionAndTime.xy - overheadRadius, HeadPositionAndTime.z);
		int3 overheadBaseCell = int3(floor(overheadMinimum / overheadCellDimensions));
		worldCell = SelectWorldCell(overheadSlot, overheadBaseCell, overheadDimensions);
		cellSize = overheadCellDimensions;
		cellSeed = Hash::LowBias32(layerDropIndex ^ 0xA24BAED5u ^ Hash::LowBias32(asuint(worldCell.x)) ^
								   Hash::LowBias32(asuint(worldCell.y) + 0x9E3779B9u));
		fallCycleHeight = overheadHeight;
	}

	float fallSpeed = max(WeatherFallDepth.y, 1.0f);
	float2 lateralVelocity = VanillaWind.xy * fallSpeed * VanillaWind.z;
	float initialPhase = Hash::Float01(cellSeed ^ 0xA511E9B3u);
	float fallDistance = max(WeatherFallDepth.z, 0.0f) + initialPhase * fallCycleHeight;
	uint lifeCycle = uint(floor(fallDistance / fallCycleHeight));
	float lifeFraction = frac(fallDistance / fallCycleHeight);
	uint lifeSeed = Hash::LowBias32(cellSeed ^ Hash::LowBias32(lifeCycle + 0x63D83595u));

	float3 jitter = float3(
		Hash::Float01(lifeSeed ^ 0xB5297A4Du),
		Hash::Float01(lifeSeed ^ 0x68E31DA4u),
		0.0f);
	float3 alternateJitter = float3(
		Hash::Float01(lifeSeed ^ 0x1B56C4E9u),
		Hash::Float01(lifeSeed ^ 0x9E3779B9u),
		0.0f);
	RainDistribution::Field distribution;
	float meanDensityWeight;
	// Birth candidates stay fixed for the lifetime; evaluating moving positions would switch paths mid-fall.
	float3 dropPosition = RainDistribution::SelectSpawn(
		(float3(worldCell) + jitter) * cellSize, (float3(worldCell) + alternateJitter) * cellSize,
		isOverheadDrop ? overheadRadius : layerRadius, lifeSeed, distribution, meanDensityWeight);
	if (isOverheadDrop)
		dropPosition.z = HeadPositionAndTime.z + overheadBottom + (1.0f - lifeFraction) * overheadHeight;
	else
		dropPosition.z = (float(worldCell.z) + 1.0f - lifeFraction) * cellSize.z;
	dropPosition.xy += lateralVelocity * (lifeFraction * fallCycleHeight / fallSpeed);

	float3 velocity = float3(lateralVelocity, -fallSpeed);

	float distanceFromHead = length(dropPosition - HeadPositionAndTime.xyz);
	float farDistance = max(LayerRadii.z, 1.0f);
	float innerRadius = layerIndex == 0u ? 0.0f : LayerRadii[max(layerIndex, 1u) - 1u];
	float layerFade;
	if (isOverheadDrop) {
		float radialDistance = length(dropPosition.xy - HeadPositionAndTime.xy);
		float radialFade = 1.0f - smoothstep(overheadRadius * 0.78f, overheadRadius, radialDistance);
		float traveledDistance = lifeFraction * overheadHeight;
		float entryFade = smoothstep(0.0f, overheadHeight * 0.18f, traveledDistance);
		float exitFade = 1.0f - smoothstep(overheadHeight * 0.82f, overheadHeight, traveledDistance);
		layerFade = radialFade * entryFade * exitFade;
	} else {
		layerFade = 1.0f - smoothstep(layerRadius * (1.0f - LayerRadii.w), layerRadius, distanceFromHead);
		if (layerIndex > 0u)
			layerFade *= smoothstep(innerRadius * (1.0f - LayerRadii.w), innerRadius, distanceFromHead);
	}
	if (layerFade <= 0.0f) {
		RejectDrop(dropIndex);
		return;
	}

	// Appearance is continuous across overlapping layers even though their particle budgets are separate.
	float distanceRatio;
	if (distanceFromHead < LayerRadii.x)
		distanceRatio = 0.25f * distanceFromHead / LayerRadii.x;
	else if (distanceFromHead < LayerRadii.y)
		distanceRatio = lerp(0.25f, 0.67f, (distanceFromHead - LayerRadii.x) / (LayerRadii.y - LayerRadii.x));
	else
		distanceRatio = lerp(0.67f, 1.0f, saturate((distanceFromHead - LayerRadii.y) / (farDistance - LayerRadii.y)));
	float speed = length(velocity);
	float lodLength = lerp(1.35f, 0.48f, smoothstep(0.15f, 1.0f, distanceRatio));
	float lodWidth = lerp(1.30f, 0.52f, smoothstep(0.08f, 1.0f, distanceRatio));
	float streakLength = (Streak.x + speed * Streak.y) * lodLength;
	float streakWidth = Streak.z * lodWidth;
	float lengthSample = Hash::Float01(lifeSeed ^ 0x917AC53Du);
	float streakVariation = lerp(1.0f, lerp(0.25f, 1.60f, lengthSample * lengthSample), Refraction.z);
	streakLength *= streakVariation;
	streakWidth *= lerp(1.0f, streakVariation, 0.35f);
	if (GridAndDebug.w == 1u) {
		velocity = float3(0.0f, 0.0f, -1.0f);
		streakLength = 16.0f;
		streakWidth = 5.0f;
	} else if (GridAndDebug.w == 2u) {
		streakLength = max(48.0f, speed * 0.12f);
		streakWidth = max(streakWidth, 3.0f);
	}

	float3 streakAxis = SafeNormalize(velocity, float3(0.0f, 0.0f, -1.0f));
	if (!RainStreakIntersectsActiveFrustum(
			dropPosition, streakAxis, max(streakLength, 1.0f) * 0.5f, max(streakWidth, 0.05f) * 0.5f)) {
		RejectDrop(dropIndex);
		return;
	}
	if (Appearance.w > 0.0f) {
		float3 headOffset = HeadPositionAndTime.xyz - dropPosition;
		float closestAlongStreak = clamp(dot(headOffset, streakAxis), -streakLength * 0.5f, streakLength * 0.5f);
		float distanceToStreak = length(dropPosition + streakAxis * closestAlongStreak - HeadPositionAndTime.xyz);
		if (distanceToStreak <= Appearance.w + streakWidth * 0.5f) {
			RejectDrop(dropIndex);
			return;
		}
	}

	float lodDensity = lerp(0.42f, 1.38f, sqrt(distanceRatio));
	float density = isOverheadDrop ? VolumeSizeAndDensity.w : VolumeSizeAndDensity.w * WeatherFallDepth.x * lodDensity;
	// Vanilla density already includes the weather transition; do not multiply that fade twice.
	if (CurtainDensity.z >= 0.0f)
		density = CurtainDensity.z;
	float acceptance = Hash::Float01(lifeSeed ^ 0xC2B2AE35u);
	float acceptedDensity = RainDistribution::Acceptance(density, meanDensityWeight);
	if ((GridAndDebug.w == 0u || GridAndDebug.w >= 6u) && acceptance >= acceptedDensity) {
		RejectDrop(dropIndex);
		return;
	}

	float roofVisibility = 1.0f;
#if defined(RAIN_SKYLIGHTING_OCCLUSION) && defined(COMPUTESHADER)
	[branch] if (RoofOcclusion.x > 0.5f)
	{
		roofVisibility = RainRoofOcclusion::Sample(dropPosition, RoofOcclusion.y, RoofOcclusion.z);
		if (roofVisibility <= 0.001f) {
			RejectDrop(dropIndex);
			return;
		}
	}
#endif
	float opacity = roofVisibility * WeatherFallDepth.x * lerp(1.0f, 0.55f, distanceRatio);
	if (Glassy.x <= 0.5f)
		opacity *= Streak.w;

	float lightLuminance = dot(max(LightColor.rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
	float lighting = lerp(1.0f, 0.35f + sqrt(saturate(lightLuminance)), saturate(Appearance.y));
	float3 rainColor = float3(0.62f, 0.72f, 0.82f) * Appearance.x * lighting;
	RainLighting::Sample localLight = (RainLighting::Sample)0;
	if (Glassy.x > 0.5f) {
		if (RainUsesSharedLighting(distanceFromHead)) {
			uint previousClaim;
			InterlockedMin(RainLightClaimsRW[RainLightBucket(RainLightCell(dropPosition))], dropIndex, previousClaim);
		} else {
			localLight = RainLighting::Evaluate(dropPosition, distanceFromHead);
		}
		rainColor = localLight.Irradiance * Appearance.x;
	}

	if (GridAndDebug.w == 1u) {
		rainColor = float3(1.0f, 0.1f, 0.8f);
		opacity = 0.9f;
	} else if (GridAndDebug.w == 2u) {
		rainColor = float3(1.0f, 0.35f, 0.05f);
		opacity = 0.85f;
	} else if (GridAndDebug.w == 3u) {
		rainColor = lerp(float3(0.05f, 0.1f, 0.8f), float3(1.0f, 0.15f, 0.0f), saturate(distribution.SpatialDensity * 0.6f));
		opacity = 0.8f;
	} else if (GridAndDebug.w == 4u) {
		rainColor = lerp(float3(0.08f, 0.05f, 0.3f), float3(1.0f, 0.85f, 0.05f), distribution.CurtainShape);
		opacity = 0.8f;
	} else if (GridAndDebug.w == 5u) {
		rainColor = layerIndex == 0u ? float3(1.0f, 0.1f, 0.1f) :
		                               (layerIndex == 1u ? float3(0.1f, 1.0f, 0.1f) : float3(0.1f, 0.35f, 1.0f));
		opacity = 0.8f;
	}

	RainDrop drop;
	drop.PositionLength = float4(dropPosition, max(streakLength, 1.0f));
	drop.VelocityWidth = float4(velocity, max(streakWidth, 0.05f));
	drop.ColorOpacity = float4(rainColor, saturate(opacity * layerFade));
	drop.LightDirection = float4(localLight.Direction, 0.0f);
	RainDropsRW[dropIndex] = drop;
}

	[numthreads(RAIN_COMPUTE_GROUP_SIZE, 1, 1)] void RainLightCacheCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint bucket = dispatchThreadID.x;
	if (bucket >= RainLightCacheSize)
		return;
	uint dropIndex = RainLightClaims[bucket];
	if (dropIndex >= LayerCounts.w)
		return;
	float3 position = RainDrops[dropIndex].PositionLength.xyz;
	RainLighting::Sample light = RainLighting::Evaluate(position, distance(position, HeadPositionAndTime.xyz));
	RainCachedLight cached;
	cached.Position = float4(position, 0.0f);
	cached.Irradiance = float4(light.Irradiance, 0.0f);
	cached.Direction = float4(light.Direction, 0.0f);
	RainLightCacheRW[bucket] = cached;
}

[numthreads(RAIN_COMPUTE_GROUP_SIZE, 1, 1)] void RainApplyLightingCS(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint dropIndex = dispatchThreadID.x;
	if (dropIndex >= LayerCounts.w)
		return;
	if (RainDropsRW[dropIndex].ColorOpacity.a <= 0.0f)
		return;
	RainDrop drop = RainDropsRW[dropIndex];
	float3 position = drop.PositionLength.xyz;
	float headDistance = distance(position, HeadPositionAndTime.xyz);
	if (!RainUsesSharedLighting(headDistance))
		return;
	int3 cell = RainLightCell(position);
	RainCachedLight cached = RainLightCache[RainLightBucket(cell)];
	RainLighting::Sample light;
	// Hash collisions must not borrow lighting from an unrelated world-space cell.
	if (all(cell == RainLightCell(cached.Position.xyz))) {
		light.Irradiance = cached.Irradiance.rgb;
		light.Direction = cached.Direction.xyz;
	} else {
		light = RainLighting::Evaluate(position, headDistance);
	}
	RainDropsRW[dropIndex].ColorOpacity.rgb = light.Irradiance * Appearance.x;
	RainDropsRW[dropIndex].LightDirection = float4(light.Direction, 0.0f);
}

groupshared uint RainGroupScan[RAIN_COMPUTE_GROUP_SIZE];

[numthreads(RAIN_COMPUTE_GROUP_SIZE, 1, 1)] void RainCountCS(
	uint3 dispatchThreadID : SV_DispatchThreadID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint3 groupID : SV_GroupID) {
	uint dropIndex = dispatchThreadID.x;
	uint isVisible = dropIndex < LayerCounts.w && RainDrops[dropIndex].ColorOpacity.a > 0.0f ? 1u : 0u;
	RainGroupScan[groupThreadID.x] = isVisible;
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint offset = 1u; offset < RAIN_COMPUTE_GROUP_SIZE; offset <<= 1u)
	{
		uint precedingCount = groupThreadID.x >= offset ? RainGroupScan[groupThreadID.x - offset] : 0u;
		GroupMemoryBarrierWithGroupSync();
		RainGroupScan[groupThreadID.x] += precedingCount;
		GroupMemoryBarrierWithGroupSync();
	}

	if (dropIndex < LayerCounts.w)
		RainLocalOffsetsRW[dropIndex] = RainGroupScan[groupThreadID.x] - isVisible;
	if (groupThreadID.x == RAIN_COMPUTE_GROUP_SIZE - 1u)
		RainGroupOffsetsRW[groupID.x] = RainGroupScan[RAIN_COMPUTE_GROUP_SIZE - 1u];
}

groupshared uint RainGroupPrefixScan[RAIN_PREFIX_GROUP_SIZE];

[numthreads(RAIN_PREFIX_GROUP_SIZE, 1, 1)] void RainPrefixCS(uint3 groupThreadID : SV_GroupThreadID) {
	uint groupCount = (LayerCounts.w + RAIN_COMPUTE_GROUP_SIZE - 1u) / RAIN_COMPUTE_GROUP_SIZE;
	uint groupIndex = groupThreadID.x;
	uint groupVisibleCount = groupIndex < groupCount ? RainGroupOffsetsRW[groupIndex] : 0u;
	RainGroupPrefixScan[groupIndex] = groupVisibleCount;
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint offset = 1u; offset < RAIN_PREFIX_GROUP_SIZE; offset <<= 1u)
	{
		uint precedingCount = groupIndex >= offset ? RainGroupPrefixScan[groupIndex - offset] : 0u;
		GroupMemoryBarrierWithGroupSync();
		RainGroupPrefixScan[groupIndex] += precedingCount;
		GroupMemoryBarrierWithGroupSync();
	}

	if (groupIndex < groupCount)
		RainGroupOffsetsRW[groupIndex] = RainGroupPrefixScan[groupIndex] - groupVisibleCount;
	if (groupIndex == 0u) {
		uint visibleDropCount = RainGroupPrefixScan[RAIN_PREFIX_GROUP_SIZE - 1u];
		RainIndirectArgsRW.Store(0, 12u);
#ifdef VR
		RainIndirectArgsRW.Store(4, visibleDropCount * 2u);
#else
		RainIndirectArgsRW.Store(4, visibleDropCount);
#endif
		RainIndirectArgsRW.Store(8, 0u);
		RainIndirectArgsRW.Store(12, 0u);
	}
}

	[numthreads(RAIN_COMPUTE_GROUP_SIZE, 1, 1)] void RainScatterCS(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupID : SV_GroupID)
{
	uint dropIndex = dispatchThreadID.x;
	if (dropIndex >= LayerCounts.w || RainDrops[dropIndex].ColorOpacity.a <= 0.0f)
		return;
	uint visibleIndex = RainGroupOffsets[groupID.x] + RainCompactionData[dropIndex];
	RainVisibleDropIndicesRW[visibleIndex] = dropIndex;
}

struct RainVertexOutput
{
	float4 Position: SV_Position;
	float2 StreakCoordinate: TEXCOORD0;
	float4 ColorOpacity: TEXCOORD1;
	float ViewDepth: TEXCOORD2;
	nointerpolation uint EyeIndex: TEXCOORD3;
	nointerpolation float3 ScreenSideAndWidth: TEXCOORD4;
	nointerpolation float DetailFade: TEXCOORD5;
	nointerpolation float SpawnReveal: TEXCOORD6;
	nointerpolation float4 ScreenAlongAndLength: TEXCOORD7;
	nointerpolation float3 StreakAxisWorld: TEXCOORD8;
	nointerpolation float3 StreakSideWorld: TEXCOORD9;
	nointerpolation float3 HeadViewDirection: TEXCOORD10;
	nointerpolation float4 LightDirection: TEXCOORD11;
	float2 EyeClip: SV_ClipDistance0;
};

float2 ProjectWorldVectorToPixels(float3 worldVector, float4 centerClip, uint eyeIndex)
{
	float4 vectorClip = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(worldVector, 0.0f));
	float2 projected =
		(vectorClip.xy * centerClip.w - centerClip.xy * vectorClip.w) /
		max(centerClip.w * centerClip.w, EPSILON_DIVISION);
#ifdef VR
	return projected * float2(ScreenSize.x * 0.25f, -ScreenSize.y * 0.5f);
#else
	return projected * float2(ScreenSize.x * 0.5f, -ScreenSize.y * 0.5f);
#endif
}

RainVertexOutput RainVS(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	RainVertexOutput output = (RainVertexOutput)0;
#ifdef VR
	uint eyeIndex = instanceID & 1u;
	uint visibleDropIndex = instanceID >> 1u;
#else
	uint eyeIndex = 0u;
	uint visibleDropIndex = instanceID;
#endif
	uint dropIndex = RainCompactionData[visibleDropIndex];
	RainDrop drop = RainDrops[dropIndex];
	uint layerIndex = dropIndex < LayerCounts.x ? 0u : (dropIndex < LayerCounts.x + LayerCounts.y ? 1u : 2u);
	uint overheadDropCount = min(uint(max(RoofOcclusion.w, 0.0f) + 0.5f), LayerCounts.x);
	bool isOverheadDrop = layerIndex == 0u && dropIndex < overheadDropCount;
	float fallCycleHeight;
	float lifeFraction;
	if (isOverheadDrop) {
		fallCycleHeight = 900.0f;
		float overheadTop = HeadPositionAndTime.z + 100.0f + fallCycleHeight;
		lifeFraction = saturate((overheadTop - drop.PositionLength.z) / fallCycleHeight);
	} else {
		float layerRadius = LayerRadii[layerIndex];
		float volumeHeight = max(VolumeSizeAndDensity.z * (layerRadius / max(LayerRadii.z, 1.0f)), 1.0f);
		fallCycleHeight = volumeHeight / max(float(GridAndDebug.z) - 1.0f, 1.0f);
		lifeFraction = frac(-drop.PositionLength.z / fallCycleHeight);
	}
	// Reveal recycled streaks from their leading edge instead of materializing the full drop silhouette.
	output.SpawnReveal = saturate(lifeFraction * fallCycleHeight / max(drop.PositionLength.w, 1.0f));

	static const float2 corners[6] = {
		float2(-1.0f, -1.0f), float2(-1.0f, 1.0f), float2(1.0f, 1.0f),
		float2(-1.0f, -1.0f), float2(1.0f, 1.0f), float2(1.0f, -1.0f)
	};
	uint planeIndex = vertexID / 6u;
	float2 corner = corners[vertexID % 6u];
	float3 streakAxis = SafeNormalize(drop.VelocityWidth.xyz, float3(0.0f, 0.0f, -1.0f));
	float3 stableReference = abs(streakAxis.z) < 0.90f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
	float3 firstSide = SafeNormalize(cross(streakAxis, stableReference), float3(1.0f, 0.0f, 0.0f));
	float3 secondSide = SafeNormalize(cross(streakAxis, firstSide), float3(0.0f, 1.0f, 0.0f));
	float3 sideAxis = planeIndex == 0u ? firstSide : secondSide;
	float ribbonCoverage = 1.0f;
	float3 worldPosition = drop.PositionLength.xyz +
	                       streakAxis * (corner.x * drop.PositionLength.w * 0.5f) +
	                       sideAxis * (corner.y * drop.VelocityWidth.w * 0.5f);
	float3 cameraRelativePosition = worldPosition - FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	float4 clipPosition = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(cameraRelativePosition, 1.0f));
	output.EyeClip = float2(clipPosition.w + clipPosition.x, clipPosition.w - clipPosition.x);
	if (Glassy.x > 0.5f) {
		float4 centerClip = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(drop.PositionLength.xyz - FrameBuffer::CameraPosAdjust[eyeIndex].xyz, 1.0f));
		float2 projectedSide = ProjectWorldVectorToPixels(
			sideAxis * drop.VelocityWidth.w * 0.5f, centerClip, eyeIndex);
		float halfWidth = length(projectedSide);
		output.ScreenSideAndWidth = float3(projectedSide / max(halfWidth, EPSILON_DIVISION), halfWidth);
		output.DetailFade = 1.0f - smoothstep(Refraction.x * 0.65f, Refraction.x, distance(drop.PositionLength.xyz, HeadPositionAndTime.xyz));
		output.StreakAxisWorld = streakAxis;
		output.StreakSideWorld = sideAxis;
		output.HeadViewDirection = SafeNormalize(HeadPositionAndTime.xyz - drop.PositionLength.xyz, float3(0.0f, 0.0f, 1.0f));
		float2 ribbonFacing = float2(dot(secondSide, output.HeadViewDirection), dot(firstSide, output.HeadViewDirection));
		ribbonFacing *= ribbonFacing;
		// The two world-space ribbons share one coverage budget and identical weights in both eyes.
		ribbonCoverage = ribbonFacing[planeIndex] / max(ribbonFacing.x + ribbonFacing.y, EPSILON_DOT_CLAMP);
		output.LightDirection = drop.LightDirection;
		if (TexturedRain.x > 0.5f) {
			float2 projectedAlong = ProjectWorldVectorToPixels(
				streakAxis * drop.PositionLength.w * 0.5f, centerClip, eyeIndex);
			float halfLength = length(projectedAlong);
			float2 projectedSize = max(float2(halfWidth, halfLength) * 2.0f, 0.25f);
			float2 texelsPerPixel = RainTextureShape.xy * float2(RainTextureShape.z, 1.0f) / projectedSize;
			float textureMip = max(log2(max(texelsPerPixel.x, texelsPerPixel.y)), 0.0f);
			output.ScreenAlongAndLength = float4(projectedAlong / max(halfLength, EPSILON_DIVISION), halfLength, textureMip);
		}
	}
#ifdef VR
	clipPosition.x = clipPosition.x * 0.5f + (eyeIndex == 0u ? -0.5f : 0.5f) * clipPosition.w;
#endif

	output.Position = clipPosition;
	output.StreakCoordinate = float2(corner.x * 0.5f + 0.5f, corner.y);
	output.ColorOpacity = drop.ColorOpacity;
	output.ColorOpacity.a *= ribbonCoverage;
	output.ViewDepth = max(abs(clipPosition.w), 0.0f);
	output.EyeIndex = eyeIndex;
	return output;
}

#ifdef PSHADER
#	include "RainRendering/RainMaterial.hlsli"

float LinearSceneDepth(float2 pixel)
{
	float rawSceneDepth = SceneDepth.Load(int3(pixel, 0));
	return RainCameraData.w / max(-rawSceneDepth * RainCameraData.z + RainCameraData.x, EPSILON_DIVISION);
}

[earlydepthstencil] float4 RainPS(RainVertexOutput input) : SV_Target0
{
	float widthFade = saturate(1.0f - abs(input.StreakCoordinate.y));
	float endFade = smoothstep(0.0f, 0.10f, input.StreakCoordinate.x) *
	                smoothstep(0.0f, 0.10f, 1.0f - input.StreakCoordinate.x);
	float revealStart = saturate(1.0f - input.SpawnReveal);
	float spawnReveal = smoothstep(revealStart, revealStart + 0.08f, input.StreakCoordinate.x);
	float silhouetteFade = widthFade * endFade * spawnReveal;
	// Texture opacity and depth fading can only reduce this coverage bound.
	if (Glassy.x > 0.5f && input.ColorOpacity.a * silhouetteFade <= RainMinimumOpticalCoverage)
		discard;
	float sceneDepth = LinearSceneDepth(input.Position.xy);
	float intersectionFade = saturate((sceneDepth - input.ViewDepth) / max(WeatherFallDepth.w, 1.0f));
	if (intersectionFade <= 0.0f)
		discard;
	float alpha = input.ColorOpacity.a * widthFade * endFade * spawnReveal * intersectionFade * 0.55f;
	float3 color = input.ColorOpacity.rgb;
	[branch] if (Glassy.x > 0.5f)
	{
		float resolvedWidth = smoothstep(0.35f, 1.25f, input.ScreenSideAndWidth.z);
		RainMaterial::Surface water = RainMaterial::Evaluate(input, silhouetteFade, resolvedWidth, intersectionFade);
		float opticalCoverage = input.ColorOpacity.a * water.Opacity * intersectionFade;
		float surfaceOpacity = saturate(Streak.w);
		float surfaceFresnel = min(water.Fresnel * surfaceOpacity, RainMaterial::MaximumSurfaceFresnel);
		float transmission = (1.0f - surfaceFresnel) * (1.0f - Glassy.y * water.Core);
		float refractionAmount = Refraction.y > 0.5f ? input.DetailFade : 0.0f;
		float environmentAmount = TexturedRain.w > 0.5f ? RainMaterial::EnvironmentTransmission * (1.0f - refractionAmount) : 0.0f;
		float reflectionScale = surfaceFresnel / max(water.Fresnel, EPSILON_DIVISION);
		float3 waterRadiance = water.Reflection * reflectionScale + water.DirectLighting + water.ScatteredLighting * surfaceOpacity +
		                       water.EnvironmentTransmission * transmission * environmentAmount;
		// Preserve a restrained glass rim when environment grading drives every sampled light source to black.
		float visibilityShape = saturate(water.Fresnel * 0.85f + water.Core * 0.15f);
		float3 minimumVisibility = Color::IrradianceToLinear(float3(0.62f, 0.72f, 0.82f)) *
		                           Appearance.x * Appearance.z * visibilityShape;
		waterRadiance += minimumVisibility * surfaceOpacity;
		float2 actualDisplacement = 0.0f;
		[branch] if (refractionAmount > 0.0f && opticalCoverage > 0.0f)
		{
			float eyeWidth = ScreenSize.x;
#	ifdef VR
			eyeWidth *= 0.5f;
#	endif
			float2 pixelMinimum = float2(ceil(input.EyeIndex * eyeWidth) + 0.5f, 0.5f);
			float2 pixelMaximum = float2(floor((input.EyeIndex + 1u) * eyeWidth) - 0.5f, floor(ScreenSize.y) - 0.5f);
			float2 pixel = clamp(input.Position.xy, pixelMinimum, pixelMaximum);
			// Bound the lens by the full projected width; distance fades its blend instead of shrinking it twice.
			float displacement = min(Glassy.w, input.ScreenSideAndWidth.z * 2.0f);
			float2 displacementDirection = water.Distortion;
			displacementDirection /= max(length(displacementDirection), 1.0f);
			float2 refractedPixel = clamp(pixel + displacementDirection * displacement, pixelMinimum, pixelMaximum);
			uint halfWidth;
			uint halfHeight;
			HalfResolutionSceneDepth.GetDimensions(halfWidth, halfHeight);
			float targetEyeWidth = ceil(eyeWidth * 0.5f);
			float2 halfMinimum = float2(input.EyeIndex * targetEyeWidth + 0.5f, 0.5f);
			float2 halfMaximum = float2(
				min((input.EyeIndex + 1u) * targetEyeWidth, float(halfWidth)) - 0.5f,
				min(ceil(ScreenSize.y * 0.5f), float(halfHeight)) - 0.5f);
			float2 halfPixel = clamp(StereoSampling::MapPixelToHalfResolution(pixel, input.EyeIndex, eyeWidth), halfMinimum, halfMaximum);
			float2 refractedHalfPixel = clamp(
				StereoSampling::MapPixelToHalfResolution(refractedPixel, input.EyeIndex, eyeWidth), halfMinimum, halfMaximum);
			// Each stored depth is the nearest source sample; test all bilinear contributors conservatively.
			float2 sampleBase = floor(refractedHalfPixel - 0.5f) + 0.5f;
			float refractedDepth = min(min(
										   HalfResolutionSceneDepth.Load(int3(clamp(sampleBase, halfMinimum, halfMaximum), 0)),
										   HalfResolutionSceneDepth.Load(int3(clamp(sampleBase + float2(1, 0), halfMinimum, halfMaximum), 0))),
				min(
					HalfResolutionSceneDepth.Load(int3(clamp(sampleBase + float2(0, 1), halfMinimum, halfMaximum), 0)),
					HalfResolutionSceneDepth.Load(int3(clamp(sampleBase + 1.0f, halfMinimum, halfMaximum), 0))));
			float safeRefraction = saturate((refractedDepth - input.ViewDepth) / RainRefractionDepthFade);
			actualDisplacement = (refractedPixel - pixel) * safeRefraction * refractionAmount;
			float2 halfResolution = float2(halfWidth, halfHeight);
			float2 samplePixel = safeRefraction > 0.0f ? refractedHalfPixel : halfPixel;
			float3 transmittedScene = SceneColor.SampleLevel(RefractionSampler, samplePixel / halfResolution, 0).rgb;
			[branch] if (safeRefraction > 0.0f && safeRefraction < 1.0f)
			{
				float3 background = SceneColor.SampleLevel(RefractionSampler, halfPixel / halfResolution, 0).rgb;
				transmittedScene = lerp(background, transmittedScene, safeRefraction);
			}
			waterRadiance += Color::IrradianceToLinear(transmittedScene) * transmission * refractionAmount;
		}
		// Unsampled transmission stays in the destination blend, keeping the water body clear at every LOD.
		float extinction = 1.0f - transmission * (1.0f - environmentAmount - refractionAmount);
		color = Color::IrradianceToGamma(waterRadiance / max(extinction, EPSILON_DIVISION));
		alpha = opticalCoverage * extinction;
		if (GridAndDebug.w == 6u) {
			color = float3(abs(actualDisplacement) / max(Glassy.w, 1.0f), 0.0f);
			alpha = opticalCoverage;
		} else if (GridAndDebug.w == 7u) {
			color = Color::IrradianceToGamma(water.LocalLighting);
			alpha = opticalCoverage;
		} else if (GridAndDebug.w == 8u) {
			color = water.NormalWorld * 0.5f + 0.5f;
			alpha = opticalCoverage;
		}
	}
	return float4(color, alpha);
}
#endif
