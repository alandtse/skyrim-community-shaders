#define TREE_WIND_SPRING_COMPUTE
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/TransientWindCulling.hlsli"
#include "Common/TreeWindSpring.hlsli"
#include "Common/WindField.hlsli"

Texture2D<float2> PreviousResponse : register(t0);
Texture2D<float2> PreviousVelocity : register(t1);
Texture2DArray<float4> PreviousTransientResponse : register(t2);
Texture2DArray<float2> PreviousTransientVelocity : register(t3);
RWTexture2D<float2> Response : register(u0);
RWTexture2D<float2> Velocity : register(u1);
RWTexture2DArray<float4> TransientField : register(u2);
RWTexture2DArray<float2> TransientVelocity : register(u3);

groupshared uint RelevantSourceCount;
groupshared uint RelevantSourceIndices[WindField::TransientImpulseCapacity];
static const float TRANSIENT_CANCELLATION_DAMPING_RATE = 12.0f;

WindField::TransientImpulseSample SampleCurrentTransientBand(
	float2 worldPosition, float bandCenter, float bandHalfExtent, uint sourceCount,
	out float directionalCoherence)
{
	WindField::TransientImpulseSample sample;
	sample.velocity = 0.0f.xxx;
	sample.intensity = 0.0f;
	float summedHorizontalMagnitude = 0.0f;
	[loop] for (uint relevantIndex = 0u; relevantIndex < sourceCount; ++relevantIndex)
	{
		uint index = RelevantSourceIndices[relevantIndex];
		WindField::TransientWindSource source = SharedData::WindFieldTransientImpulses[index];
		float influenceHeight = source.origin.z;
		float horizontalDirectionLengthSquared = dot(source.direction.xy, source.direction.xy);
		if (source.type != WindField::TransientWindSourceTypeRadialWave &&
			horizontalDirectionLengthSquared > EPSILON_WIND_LENGTH_SQ) {
			float centerlineDistance = dot(worldPosition - source.origin.xy, source.direction.xy) /
			                           horizontalDirectionLengthSquared;
			influenceHeight += source.direction.z * centerlineDistance;
		}
		float sampleHeight = clamp(
			influenceHeight, bandCenter - bandHalfExtent, bandCenter + bandHalfExtent);
		WindField::TransientImpulseSample sourceSample = WindField::SampleTransientImpulse(
			float3(worldPosition, sampleHeight), source);
		sample.velocity += sourceSample.velocity;
		summedHorizontalMagnitude += abs(sourceSample.velocity.x) + abs(sourceSample.velocity.y);
	}
	float netHorizontalMagnitude = abs(sample.velocity.x) + abs(sample.velocity.y);
	directionalCoherence = summedHorizontalMagnitude > EPSILON_WIND_GEOMETRY ?
	                           saturate(netHorizontalMagnitude / summedHorizontalMagnitude) :
	                           1.0f;
	return sample;
}

float2 ApplyGustResponse(float2 gustVelocity)
{
	float2 scaledGust = gustVelocity * max(TreeWindSpring::GustScale, 0.0f);
	float softLimit = max(TreeWindSpring::GustSoftLimit, 0.0f);
	if (softLimit <= EPSILON_WIND_GEOMETRY)
		return scaledGust;
	float speedSquared = dot(scaledGust, scaledGust);
	return scaledGust * rsqrt(1.0f + speedSquared / (softLimit * softLimit));
}

void AdvanceResponse(uint2 outputCell, int2 previousCell, float2 target, bool historyValid, float frameTime)
{
	float2 response = target;
	float2 velocity = 0.0f.xx;
	if (historyValid) {
		float2 nextResponse;
		float2 nextVelocity;
		DampedSpring::Advance(
			PreviousResponse.Load(int3(previousCell, 0)),
			PreviousVelocity.Load(int3(previousCell, 0)),
			target,
			frameTime,
			TreeWindSpring::SpringFrequency,
			TreeWindSpring::SpringDamping,
			nextResponse,
			nextVelocity);
		response = nextResponse;
		velocity = nextVelocity;
	}
	Response[outputCell] = response;
	Velocity[outputCell] = velocity;
}

[numthreads(8, 8, 1)] void UpdateField(
	uint3 dispatchThreadId : SV_DispatchThreadID,
	uint3 groupId : SV_GroupID,
	uint3 groupThreadId : SV_GroupThreadID) {
	TreeWindSpring::FieldData field = TreeWindSpring::Fields[TreeWindSpring::ActiveField];
	uint2 fieldDimensions = uint2(field.TextureSize, field.TextureSize);
	float cellSize = field.FieldSize / field.TextureSize;
	bool processTransients = (TreeWindSpring::TransientFieldMask &
								 (1u << TreeWindSpring::ActiveField)) != 0u;
	uint activeSourceCount = processTransients ?
	                             min(SharedData::WindFieldActiveCounts.x, WindField::TransientImpulseCapacity) :
	                             0u;
	uint relevantSourceCount = 0u;
	if (activeSourceCount > 0u) {
		if (groupThreadId.x == 0u && groupThreadId.y == 0u)
			RelevantSourceCount = 0u;
		GroupMemoryBarrierWithGroupSync();

		uint groupThreadIndex = groupThreadId.y * 8u + groupThreadId.x;
		float2 tileCenter = field.FieldMinimum +
		                    (float2(groupId.xy * 8u) + 4.0f) * cellSize;
		float tileRadius = cellSize * 4.94974747f;
		for (uint sourceIndex = groupThreadIndex; sourceIndex < activeSourceCount; sourceIndex += 64u) {
			WindField::TransientWindSource source = SharedData::WindFieldTransientImpulses[sourceIndex];
			if (TransientWindCulling::SourceMayAffectTile(source, tileCenter, tileRadius)) {
				uint relevantIndex;
				InterlockedAdd(RelevantSourceCount, 1u, relevantIndex);
				RelevantSourceIndices[relevantIndex] = sourceIndex;
			}
		}
		GroupMemoryBarrierWithGroupSync();
		relevantSourceCount = RelevantSourceCount;
	}

	if (any(dispatchThreadId.xy >= fieldDimensions) ||
		dispatchThreadId.z >= TreeWindSpring::TransientHeightCount)
		return;
	float2 worldPosition = field.FieldMinimum +
	                       (float2(dispatchThreadId.xy) + 0.5f) * cellSize;
	float heightHalfRange = TreeWindSpring::GetTransientHeightHalfRange(TreeWindSpring::ActiveField);
	float heightBlend = dispatchThreadId.z / float(TreeWindSpring::TransientHeightCount - 1u);
	float worldHeight = field.FieldHeight + lerp(-heightHalfRange, heightHalfRange, heightBlend);
	float directionalCoherence;
	WindField::TransientImpulseSample transientSample =
		SampleCurrentTransientBand(
			worldPosition, worldHeight, heightHalfRange * 0.5f, relevantSourceCount,
			directionalCoherence);

	float2 previousCoordinate = (worldPosition - field.PreviousFieldMinimum) / cellSize;
	int2 previousCell = int2(floor(previousCoordinate));
	bool horizontalHistoryValid = field.Initialize == 0u &&
	                              all(previousCell >= 0) &&
	                              all(previousCell < int2(field.TextureSize, field.TextureSize));
	float previousHeightCoordinate =
		(worldHeight - (field.PreviousFieldHeight - heightHalfRange)) /
		(2.0f * heightHalfRange) * (TreeWindSpring::TransientHeightCount - 1u);
	bool transientHistoryValid = horizontalHistoryValid &&
	                             previousHeightCoordinate >= 0.0f &&
	                             previousHeightCoordinate <=
	                                 float(TreeWindSpring::TransientHeightCount - 1u);
	float2 transientResponse = 0.0f.xx;
	float2 transientVelocity = 0.0f.xx;
	if (transientHistoryValid) {
		uint lowerHeight = min(
			(uint)floor(previousHeightCoordinate), TreeWindSpring::TransientHeightCount - 1u);
		uint upperHeight = min(lowerHeight + 1u, TreeWindSpring::TransientHeightCount - 1u);
		float heightFraction = frac(previousHeightCoordinate);
		float4 previousTransientState = lerp(
			PreviousTransientResponse.Load(int4(previousCell, lowerHeight, 0)),
			PreviousTransientResponse.Load(int4(previousCell, upperHeight, 0)),
			heightFraction);
		transientResponse = previousTransientState.xy;
		transientVelocity = lerp(
			PreviousTransientVelocity.Load(int4(previousCell, lowerHeight, 0)),
			PreviousTransientVelocity.Load(int4(previousCell, upperHeight, 0)),
			heightFraction);
	}
	float2 immediateTransient = transientSample.velocity.xy;
	float cancellationStrength = 1.0f - directionalCoherence;
	if (transientHistoryValid && cancellationStrength > EPSILON_WIND_GEOMETRY) {
		float retainedMomentum = rcp(
			1.0f + TRANSIENT_CANCELLATION_DAMPING_RATE * cancellationStrength * field.FrameTime);
		transientResponse = lerp(immediateTransient, transientResponse, retainedMomentum);
		transientVelocity *= retainedMomentum;
	}
	float stateMagnitude = max(
		max(max(abs(transientResponse.x), abs(transientResponse.y)),
			max(abs(immediateTransient.x), abs(immediateTransient.y))),
		max(abs(transientVelocity.x), abs(transientVelocity.y)));
	if (stateMagnitude > EPSILON_WIND_GEOMETRY) {
		DampedSpring::Advance(
			transientResponse,
			transientVelocity,
			immediateTransient,
			field.FrameTime,
			TreeWindSpring::TransientSpringFrequency,
			TreeWindSpring::TransientSpringDamping,
			transientResponse,
			transientVelocity);
	} else {
		transientResponse = 0.0f.xx;
		transientVelocity = 0.0f.xx;
	}
	// XY retains structural spring momentum; ZW keeps immediate leaf response.
	TransientField[dispatchThreadId] = float4(transientResponse, immediateTransient);
	TransientVelocity[dispatchThreadId] = transientVelocity;

	if (dispatchThreadId.z != 0u)
		return;
	WindField::Components components = WindField::SampleCurrentComponents(
		float3(worldPosition, field.FieldHeight));
	float2 target = components.baseAmbientVelocity.xy + ApplyGustResponse(components.gustVelocity.xy);
	AdvanceResponse(
		dispatchThreadId.xy, previousCell, target, horizontalHistoryValid, field.FrameTime);
}
