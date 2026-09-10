#define GRASS_WIND_SPRING_COMPUTE
#include "Common/GrassWindSpring.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/TransientWindCulling.hlsli"
#include "Common/WindField.hlsli"

RWTexture2D<float4> Response : register(u0);
RWTexture2D<float4> Velocity : register(u1);

groupshared uint RelevantSourceCount;
groupshared uint RelevantSourceIndices[WindField::TransientImpulseCapacity];

float3 SampleRelevantTransientVelocity(float3 worldPosition, uint sourceCount)
{
	float3 velocity = 0.0f.xxx;
	[loop] for (uint relevantIndex = 0u; relevantIndex < sourceCount; ++relevantIndex)
	{
		uint sourceIndex = RelevantSourceIndices[relevantIndex];
		WindField::TransientImpulseSample sourceSample = WindField::SampleTransientImpulse(
			worldPosition, SharedData::WindFieldTransientImpulses[sourceIndex]);
		velocity += sourceSample.velocity;
	}
	return velocity;
}

[numthreads(8, 8, 1)] void main(
	uint3 dispatchThreadId : SV_DispatchThreadID,
	uint3 groupId : SV_GroupID,
	uint3 groupThreadId : SV_GroupThreadID) {
	GrassWindSpring::FieldData field = GrassWindSpring::Fields[GrassWindSpring::ActiveField];
	uint2 fieldDimensions = uint2(field.TextureSize, field.TextureSize);
	float cellSize = field.FieldSize / field.TextureSize;
	bool processTransients = (GrassWindSpring::TransientFieldMask &
								 (1u << GrassWindSpring::ActiveField)) != 0u;
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

	if (any(dispatchThreadId.xy >= fieldDimensions))
		return;
	float2 worldPosition = field.FieldMinimum +
	                       (float2(dispatchThreadId.xy) + 0.5f) * cellSize;
	float3 samplePosition = float3(worldPosition, field.FieldHeight);
	WindField::Components components = WindField::SampleCurrentComponents(samplePosition);
	float3 windVelocity = components.baseAmbientVelocity + components.gustVelocity +
	                      SampleRelevantTransientVelocity(samplePosition, relevantSourceCount);
	float3 target = GrassWindSpring::CalculateTarget(
		windVelocity, field);

	float3 response = target;
	float3 velocity = 0.0f.xxx;
	float2 previousCoordinate =
		(worldPosition - field.PreviousFieldMinimum) / cellSize;
	int2 previousCell = int2(floor(previousCoordinate));
	bool historyValid = field.Initialize == 0u &&
	                    all(previousCell >= 0) && all(previousCell < int2(field.TextureSize, field.TextureSize));
	if (historyValid) {
		response = GrassWindSpring::PreviousResponse.Load(int3(previousCell, 0)).xyz;
		velocity = GrassWindSpring::PreviousVelocity.Load(int3(previousCell, 0)).xyz;
		float3 nextResponse;
		float3 nextVelocity;
		DampedSpring::Advance(response, velocity, target, field.FrameTime,
			field.SpringFrequency, field.SpringDamping,
			nextResponse, nextVelocity);
		response = nextResponse;
		velocity = nextVelocity;
	}
	float bendMagnitude = length(response.xy);
	if (bendMagnitude > field.MaximumTiltRadians && bendMagnitude > EPSILON_WIND_RESPONSE) {
		float2 bendDirection = response.xy / bendMagnitude;
		response.xy = bendDirection * field.MaximumTiltRadians;
		velocity.xy -= bendDirection * max(dot(velocity.xy, bendDirection), 0.0f);
	}
	if (response.z <= 0.0f) {
		response.z = 0.0f;
		velocity.z = max(velocity.z, 0.0f);
	} else if (response.z >= 1.0f) {
		response.z = 1.0f;
		velocity.z = min(velocity.z, 0.0f);
	}

	Response[dispatchThreadId.xy] = float4(response, 0.0f);
	Velocity[dispatchThreadId.xy] = float4(velocity, 0.0f);
}
