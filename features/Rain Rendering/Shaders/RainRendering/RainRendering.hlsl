#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "RainRendering/RainConstants.hlsli"

#include "RainRendering/RainLighting.hlsli"
#if defined(RAIN_SKYLIGHTING_OCCLUSION) && defined(COMPUTESHADER)
#	include "RainRendering/RainRoofOcclusion.hlsli"
#endif

#define RAIN_COMPUTE_GROUP_SIZE 128

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
						   RainCameraData.w / max(-depth00 * RainCameraData.z + RainCameraData.x, 1e-4f),
						   RainCameraData.w / max(-depth10 * RainCameraData.z + RainCameraData.x, 1e-4f)),
		min(
			RainCameraData.w / max(-depth01 * RainCameraData.z + RainCameraData.x, 1e-4f),
			RainCameraData.w / max(-depth11 * RainCameraData.z + RainCameraData.x, 1e-4f)));
	return output;
}

uint Hash(uint value)
{
	value ^= value >> 16u;
	value *= 0x7FEB352Du;
	value ^= value >> 15u;
	value *= 0x846CA68Bu;
	return value ^ (value >> 16u);
}

float Hash01(uint value)
{
	return float(Hash(value) >> 8u) * (1.0f / 16777216.0f);
}

uint HashLattice(int2 lattice, uint seed)
{
	return Hash(asuint(lattice.x) ^ Hash(asuint(lattice.y) ^ seed));
}

float ValueNoise(float2 position, uint seed)
{
	int2 lattice = int2(floor(position));
	float2 offset = frac(position);
	float2 blend = offset * offset * (3.0f - 2.0f * offset);
	float value00 = Hash01(HashLattice(lattice, seed));
	float value10 = Hash01(HashLattice(lattice + int2(1, 0), seed));
	float value01 = Hash01(HashLattice(lattice + int2(0, 1), seed));
	float value11 = Hash01(HashLattice(lattice + int2(1, 1), seed));
	return lerp(lerp(value00, value10, blend.x), lerp(value01, value11, blend.x), blend.y);
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
	int3 offset = int3(
		PositiveModulo(int(slot.x) - baseSlot.x, dimension.x),
		PositiveModulo(int(slot.y) - baseSlot.y, dimension.y),
		PositiveModulo(int(slot.z) - baseSlot.z, dimension.z));
	return baseCell + offset;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
	float lengthSquared = dot(value, value);
	return lengthSquared > 1e-6f ? value * rsqrt(lengthSquared) : fallback;
}

void RejectDrop(uint dropIndex, float3 position)
{
	RainDrop drop;
	drop.PositionLength = float4(position, 0.0f);
	drop.VelocityWidth = 0.0f;
	drop.ColorOpacity = 0.0f;
	drop.LightDirection = 0.0f;
	RainDropsRW[dropIndex] = drop;
}

static const float RainFrustumGuardBand = 0.05f;

bool RainCapsuleOutsidePlane(float4 plane, float3 startPosition, float3 endPosition, float radius)
{
	float projectedRadius = radius * length(plane.xyz);
	return dot(plane, float4(startPosition, 1.0f)) < -projectedRadius &&
	       dot(plane, float4(endPosition, 1.0f)) < -projectedRadius;
}

bool RainStreakIntersectsEyeFrustum(float3 position, float3 axis, float halfLength, float radius, uint eyeIndex)
{
	float3 cameraPosition = FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	float3 startPosition = position - axis * halfLength - cameraPosition;
	float3 endPosition = position + axis * halfLength - cameraPosition;
	row_major float4x4 viewProjection = FrameBuffer::CameraViewProjUnjittered[eyeIndex];
	float4 clipX = viewProjection[0];
	float4 clipY = viewProjection[1];
	float4 clipZ = viewProjection[2];
	float4 clipW = viewProjection[3];
	float4 guardedW = clipW * (1.0f + RainFrustumGuardBand);

	return !RainCapsuleOutsidePlane(guardedW + clipX, startPosition, endPosition, radius) &&
	       !RainCapsuleOutsidePlane(guardedW - clipX, startPosition, endPosition, radius) &&
	       !RainCapsuleOutsidePlane(guardedW + clipY, startPosition, endPosition, radius) &&
	       !RainCapsuleOutsidePlane(guardedW - clipY, startPosition, endPosition, radius) &&
	       !RainCapsuleOutsidePlane(clipZ, startPosition, endPosition, radius) &&
	       !RainCapsuleOutsidePlane(clipW - clipZ, startPosition, endPosition, radius);
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
	uint slotIndex = (layerDropIndex * 12347u + 5317u) % totalCellCount;
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
	uint cellSeed = Hash(layerDropIndex ^ Hash(layerIndex + 0xD7A38E91u) ^ Hash(asuint(worldCell.x)) ^
						 Hash(asuint(worldCell.y) + 0x9E3779B9u) ^ Hash(asuint(worldCell.z) + 0x85EBCA6Bu));
	float overheadRadius = 0.0f;
	float overheadHeight = 0.0f;
	float overheadBottom = 0.0f;
	float fallCycleHeight = cellSize.z;
	if (isOverheadDrop) {
		uint3 overheadDimensions = uint3(8u, 8u, 1u);
		uint overheadSlotIndex = (layerDropIndex * 37u + 17u) % 64u;
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
		cellSeed = Hash(layerDropIndex ^ 0xA24BAED5u ^ Hash(asuint(worldCell.x)) ^
						Hash(asuint(worldCell.y) + 0x9E3779B9u));
		fallCycleHeight = overheadHeight;
	}

	float fallSpeed = max(WeatherFallDepth.y, 1.0f);
	float2 lateralVelocity = VanillaWind.xy * fallSpeed * VanillaWind.z;
	float initialPhase = Hash01(cellSeed ^ 0xA511E9B3u);
	float fallDistance = max(HeadPositionAndTime.w, 0.0f) * fallSpeed + initialPhase * fallCycleHeight;
	uint lifeCycle = uint(floor(fallDistance / fallCycleHeight));
	float lifeFraction = frac(fallDistance / fallCycleHeight);
	uint lifeSeed = Hash(cellSeed ^ Hash(lifeCycle + 0x63D83595u));

	float3 jitter = float3(
		Hash01(lifeSeed ^ 0xB5297A4Du),
		Hash01(lifeSeed ^ 0x68E31DA4u),
		0.0f);
	float3 dropPosition = (float3(worldCell) + jitter) * cellSize;
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
		RejectDrop(dropIndex, dropPosition);
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
	float streakVariation = lerp(1.0f, lerp(0.55f, 1.45f, Hash01(lifeSeed ^ 0x917AC53Du)), Refraction.z);
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
		RejectDrop(dropIndex, dropPosition);
		return;
	}
	if (Appearance.w > 0.0f) {
		float3 headOffset = HeadPositionAndTime.xyz - dropPosition;
		float closestAlongStreak = clamp(dot(headOffset, streakAxis), -streakLength * 0.5f, streakLength * 0.5f);
		float distanceToStreak = length(dropPosition + streakAxis * closestAlongStreak - HeadPositionAndTime.xyz);
		if (distanceToStreak <= Appearance.w + streakWidth * 0.5f) {
			RejectDrop(dropIndex, dropPosition);
			return;
		}
	}

	float spatialNoise = ValueNoise(dropPosition.xy / max(DistanceNoise.y, 1.0f), 0xD1B54A35u);
	float spatialDensity = lerp(1.0f, lerp(0.32f, 1.68f, spatialNoise), saturate(DistanceNoise.z));

	const float2 curtainDirection = float2(0.8192319f, 0.5734624f);
	float2 curtainPerpendicular = float2(-curtainDirection.y, curtainDirection.x);
	float curtainScale = max(Curtain.x, 1.0f);
	float2 curtainCoordinate = float2(
		dot(dropPosition.xy, curtainDirection) / (curtainScale * 0.28f),
		dot(dropPosition.xy, curtainPerpendicular) / curtainScale);
	float curtainNoise = ValueNoise(curtainCoordinate, 0x94D049BBu);
	float curtainShape = pow(saturate(curtainNoise), max(Curtain.z, 0.1f));
	float curtainMinimum = min(CurtainDensity.x, CurtainDensity.y);
	float curtainMaximum = max(CurtainDensity.x, CurtainDensity.y);
	float curtainDensity = lerp(curtainMinimum, curtainMaximum, curtainShape);
	float curtainFactor = lerp(1.0f, curtainDensity, saturate(Curtain.y));

	float lodDensity = lerp(0.42f, 1.38f, sqrt(distanceRatio));
	float density = VolumeSizeAndDensity.w * WeatherFallDepth.x * spatialDensity * curtainFactor * lodDensity;
	float acceptance = Hash01(lifeSeed ^ 0xC2B2AE35u);
	float acceptedDensity = isOverheadDrop ? saturate(VolumeSizeAndDensity.w) : saturate(density);
	if ((GridAndDebug.w == 0u || GridAndDebug.w >= 6u) && acceptance > acceptedDensity) {
		RejectDrop(dropIndex, dropPosition);
		return;
	}

	float roofVisibility = 1.0f;
#if defined(RAIN_SKYLIGHTING_OCCLUSION) && defined(COMPUTESHADER)
	[branch] if (RoofOcclusion.x > 0.5f)
	{
		roofVisibility = RainRoofOcclusion::Sample(dropPosition, RoofOcclusion.y, RoofOcclusion.z);
		if (roofVisibility <= 0.001f) {
			RejectDrop(dropIndex, dropPosition);
			return;
		}
	}
#endif
	float opacity = Streak.w * roofVisibility * WeatherFallDepth.x * lerp(1.0f, 0.55f, distanceRatio);

	float lightLuminance = dot(max(LightColor.rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
	float lighting = lerp(1.0f, 0.35f + sqrt(saturate(lightLuminance)), saturate(Appearance.y));
	float3 rainColor = float3(0.62f, 0.72f, 0.82f) * Appearance.x * lighting;
	RainLighting::Sample localLight = (RainLighting::Sample)0;
	if (Glassy.x > 0.5f) {
		localLight = RainLighting::Evaluate(dropPosition, HeadPositionAndTime.xyz, distanceFromHead);
		rainColor = localLight.Irradiance * Appearance.x;
	}

	if (GridAndDebug.w == 1u) {
		rainColor = float3(1.0f, 0.1f, 0.8f);
		opacity = 0.9f;
	} else if (GridAndDebug.w == 2u) {
		rainColor = float3(1.0f, 0.35f, 0.05f);
		opacity = 0.85f;
	} else if (GridAndDebug.w == 3u) {
		rainColor = lerp(float3(0.05f, 0.1f, 0.8f), float3(1.0f, 0.15f, 0.0f), saturate(spatialDensity * 0.6f));
		opacity = 0.8f;
	} else if (GridAndDebug.w == 4u) {
		rainColor = lerp(float3(0.08f, 0.05f, 0.3f), float3(1.0f, 0.85f, 0.05f), curtainShape);
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
	drop.LightDirection = float4(localLight.Direction, localLight.Scattering);
	RainDropsRW[dropIndex] = drop;
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

groupshared uint RainGroupPrefixScan[512];

[numthreads(512, 1, 1)] void RainPrefixCS(uint3 groupThreadID : SV_GroupThreadID) {
	uint groupCount = (LayerCounts.w + RAIN_COMPUTE_GROUP_SIZE - 1u) / RAIN_COMPUTE_GROUP_SIZE;
	uint groupIndex = groupThreadID.x;
	uint groupVisibleCount = groupIndex < groupCount ? RainGroupOffsetsRW[groupIndex] : 0u;
	RainGroupPrefixScan[groupIndex] = groupVisibleCount;
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint offset = 1u; offset < 512u; offset <<= 1u)
	{
		uint precedingCount = groupIndex >= offset ? RainGroupPrefixScan[groupIndex - offset] : 0u;
		GroupMemoryBarrierWithGroupSync();
		RainGroupPrefixScan[groupIndex] += precedingCount;
		GroupMemoryBarrierWithGroupSync();
	}

	if (groupIndex < groupCount)
		RainGroupOffsetsRW[groupIndex] = RainGroupPrefixScan[groupIndex] - groupVisibleCount;
	if (groupIndex == 0u) {
		uint visibleDropCount = RainGroupPrefixScan[511];
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
	nointerpolation float3 ScreenAlongAndLength: TEXCOORD7;
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
		max(centerClip.w * centerClip.w, 1e-4f);
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
		output.ScreenSideAndWidth = float3(projectedSide / max(halfWidth, 1e-4f), halfWidth);
		output.DetailFade = 1.0f - smoothstep(Refraction.x * 0.65f, Refraction.x, distance(drop.PositionLength.xyz, HeadPositionAndTime.xyz));
		output.StreakAxisWorld = streakAxis;
		output.StreakSideWorld = sideAxis;
		output.HeadViewDirection = SafeNormalize(HeadPositionAndTime.xyz - drop.PositionLength.xyz, float3(0.0f, 0.0f, 1.0f));
		float2 ribbonFacing = float2(dot(secondSide, output.HeadViewDirection), dot(firstSide, output.HeadViewDirection));
		ribbonFacing *= ribbonFacing;
		// The two world-space ribbons share one coverage budget and identical weights in both eyes.
		ribbonCoverage = ribbonFacing[planeIndex] / max(ribbonFacing.x + ribbonFacing.y, 1e-5f);
		output.LightDirection = drop.LightDirection;
		if (TexturedRain.x > 0.5f && output.DetailFade > 0.0f) {
			float2 projectedAlong = ProjectWorldVectorToPixels(
				streakAxis * drop.PositionLength.w * 0.5f, centerClip, eyeIndex);
			float halfLength = length(projectedAlong);
			output.ScreenAlongAndLength = float3(projectedAlong / max(halfLength, 1e-4f), halfLength);
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
	return RainCameraData.w / max(-rawSceneDepth * RainCameraData.z + RainCameraData.x, 1e-4f);
}

float2 HalfResolutionScenePixel(float2 pixel, uint eyeIndex, float eyeWidth)
{
#	ifdef VR
	float targetEyeWidth = ceil(eyeWidth * 0.5f);
	return float2((pixel.x - eyeIndex * eyeWidth) * 0.5f + eyeIndex * targetEyeWidth, pixel.y * 0.5f);
#	else
	return pixel * 0.5f;
#	endif
}

float4 RainPS(RainVertexOutput input) : SV_Target0
{
	float sceneDepth = LinearSceneDepth(input.Position.xy);
	float intersectionFade = saturate((sceneDepth - input.ViewDepth) / max(WeatherFallDepth.w, 1.0f));
	if (intersectionFade <= 0.0f)
		discard;
	float widthFade = saturate(1.0f - abs(input.StreakCoordinate.y));
	float endFade = smoothstep(0.0f, 0.10f, input.StreakCoordinate.x) *
	                smoothstep(0.0f, 0.10f, 1.0f - input.StreakCoordinate.x);
	float revealStart = saturate(1.0f - input.SpawnReveal);
	float spawnReveal = smoothstep(revealStart, revealStart + 0.08f, input.StreakCoordinate.x);
	float alpha = input.ColorOpacity.a * widthFade * endFade * spawnReveal * intersectionFade * 0.55f;
	float3 color = input.ColorOpacity.rgb;
	[branch] if (Glassy.x > 0.5f)
	{
		float resolvedWidth = smoothstep(0.35f, 1.25f, input.ScreenSideAndWidth.z);
		RainMaterial::Surface water = RainMaterial::Evaluate(input, widthFade * endFade * spawnReveal, resolvedWidth);
		float coverage = input.ColorOpacity.a * water.Opacity * intersectionFade;
		if (coverage <= 1e-4f)
			discard;
		float transmission = (1.0f - water.Fresnel) * (1.0f - Glassy.y * water.Core);
		float refractionAmount = Refraction.y > 0.5f ? input.DetailFade * MaterialLighting.z : 0.0f;
		float environmentAmount = TexturedRain.w > 0.5f ? Refraction.w * (1.0f - refractionAmount) : 0.0f;
		float3 waterRadiance = water.Reflection + water.LocalLighting + water.DirectionalLighting +
		                       water.EnvironmentTransmission * transmission * environmentAmount;
		// Preserve a restrained glass rim when environment grading drives every sampled light source to black.
		float visibilityShape = saturate(water.Fresnel * 0.85f + water.Core * 0.15f);
		float3 minimumVisibility = Color::IrradianceToLinear(float3(0.62f, 0.72f, 0.82f)) *
		                           Appearance.x * Appearance.z * visibilityShape;
		waterRadiance += minimumVisibility;
		float2 actualDisplacement = 0.0f;
		[branch] if (refractionAmount > 0.0f && coverage > 0.0f)
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
			float2 halfPixel = clamp(HalfResolutionScenePixel(pixel, input.EyeIndex, eyeWidth), halfMinimum, halfMaximum);
			float2 refractedHalfPixel = clamp(
				HalfResolutionScenePixel(refractedPixel, input.EyeIndex, eyeWidth), halfMinimum, halfMaximum);
			// Each stored depth is the nearest source sample; test all bilinear contributors conservatively.
			float2 sampleBase = floor(refractedHalfPixel - 0.5f) + 0.5f;
			float refractedDepth = min(min(
										   HalfResolutionSceneDepth.Load(int3(clamp(sampleBase, halfMinimum, halfMaximum), 0)),
										   HalfResolutionSceneDepth.Load(int3(clamp(sampleBase + float2(1, 0), halfMinimum, halfMaximum), 0))),
				min(
					HalfResolutionSceneDepth.Load(int3(clamp(sampleBase + float2(0, 1), halfMinimum, halfMaximum), 0)),
					HalfResolutionSceneDepth.Load(int3(clamp(sampleBase + 1.0f, halfMinimum, halfMaximum), 0))));
			float safeRefraction = saturate((refractedDepth - input.ViewDepth) / max(WeatherFallDepth.w, 1.0f));
			actualDisplacement = (refractedPixel - pixel) * safeRefraction * refractionAmount;
			float2 halfResolution = float2(halfWidth, halfHeight);
			float3 background = SceneColor.SampleLevel(RefractionSampler, halfPixel / halfResolution, 0).rgb;
			float3 refracted = SceneColor.SampleLevel(RefractionSampler, refractedHalfPixel / halfResolution, 0).rgb;
			waterRadiance += Color::IrradianceToLinear(lerp(background, refracted, safeRefraction)) * transmission * refractionAmount;
		}
		// Unsampled transmission stays in the destination blend, keeping the water body clear at every LOD.
		float extinction = 1.0f - transmission * (1.0f - environmentAmount - refractionAmount);
		color = Color::IrradianceToGamma(waterRadiance / max(extinction, 1e-4f));
		alpha = coverage * extinction;
		if (GridAndDebug.w == 6u) {
			color = float3(abs(actualDisplacement) / max(Glassy.w, 1.0f), 0.0f);
			alpha = coverage;
		} else if (GridAndDebug.w == 7u) {
			color = Color::IrradianceToGamma(water.LocalLighting);
			alpha = coverage;
		} else if (GridAndDebug.w == 8u) {
			color = water.NormalWorld * 0.5f + 0.5f;
			alpha = coverage;
		}
	}
	return float4(color, alpha);
}
#endif
