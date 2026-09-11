#include "GrassCollision/GrassCollisionField.hlsli"

namespace GrassCollision
{
	float3 CalculateDisplacement(
		float3 modelPosition, float3 instanceRoot, float4 fieldSample, float bendWeight,
		float compressionWeight,
		float nearFactor, float4x4 worldMatrix, out float3 bendAxis, out float bendAngle)
	{
		float2 worldBend = fieldSample.xy;
		float worldBendMagnitude = length(worldBend);
		float3 relativePosition = modelPosition - instanceRoot;
		float3 deformedPosition = relativePosition;
		bendAxis = float3(0.0, 1.0, 0.0);
		bendAngle = 0.0;
		if (worldBendMagnitude > 1e-5 && bendWeight > 1e-5 && nearFactor > 1e-5) {
			float3 worldBendDirection = float3(worldBend / worldBendMagnitude, 0.0);
			float3 modelBendDirection = mul(transpose((float3x3)worldMatrix), worldBendDirection);
			modelBendDirection.z = 0.0;
			float modelDirectionLength = length(modelBendDirection);
			if (modelDirectionLength > 1e-5) {
				modelBendDirection /= modelDirectionLength;
				bendAxis = cross(float3(0.0, 0.0, 1.0), modelBendDirection);
				bendAngle = worldBendMagnitude * bendWeight * nearFactor;
				deformedPosition = GrassWind::RotateVector(relativePosition, bendAxis, bendAngle);
			}
		}
		float compression = saturate(fieldSample.z) * compressionWeight * nearFactor;
		deformedPosition.z *= 1.0 - compression;
		return deformedPosition - relativePosition;
	}

	void ApplySampledDeformation(
		VS_INPUT input, float3 currentPosition, float3 previousPosition, float3 instanceRoot,
		float4 currentField, float4 previousField, float4x4 worldMatrix, float4x4 previousWorldMatrix,
		out float3 displacement, out float3 previousDisplacement,
		out float3 bendAxis, out float bendAngle)
	{
		float normalizedHeight = saturate(input.Color.w);
		float bendWeight = normalizedHeight * normalizedHeight;
		float compressionReach = max(SharedData::grassCollisionData.CompressionHeight, 0.1);
		float compressionWeight = smoothstep(0.0, compressionReach, normalizedHeight);
		// Undo the vertex-height weight to estimate the scaled height of the whole grass blade.
		float instanceUpScale = length(float3(input.InstanceData2.z, input.InstanceData3.z, input.InstanceData3.w));
		float vertexHeight = abs(input.Position.z * (input.InstanceData4.y * ScaleMask.z + 1.0)) * instanceUpScale;
		float bladeHeight = vertexHeight / max(normalizedHeight, 1e-3);
		float maximumCompressibleHeight = max(
			SharedData::grassCollisionData.MaximumCompressibleGrassHeight, 1.0);
		float heightFade = min(maximumCompressibleHeight * 0.25, 16.0);
		compressionWeight *= 1.0 - smoothstep(
									   maximumCompressibleHeight - heightFade, maximumCompressibleHeight, bladeHeight);

		displacement = CalculateDisplacement(
			currentPosition, instanceRoot, currentField, bendWeight, compressionWeight,
			currentField.w, worldMatrix, bendAxis, bendAngle);
		float3 previousBendAxis;
		float previousBendAngle;
		previousDisplacement = CalculateDisplacement(
			previousPosition, instanceRoot, previousField, bendWeight, compressionWeight,
			previousField.w, previousWorldMatrix, previousBendAxis, previousBendAngle);
	}

#ifndef GRASS_OPTIMIZATIONS
	void ApplyDeformation(
		VS_INPUT input, float3 currentPosition, float3 previousPosition,
		out float3 displacement, out float3 previousDisplacement,
		out float3 bendAxis, out float bendAngle)
	{
		float3 currentRootWorld = mul(World[0], float4(input.InstanceData1.xyz, 1.0)).xyz;
		float3 previousRootWorld = mul(PreviousWorld[0], float4(input.InstanceData1.xyz, 1.0)).xyz;
		float4 currentField = SampleCurrentDeformation(currentRootWorld.xy);
		float4 previousField = SamplePreviousDeformation(previousRootWorld.xy);
		currentField.w = smoothstep(4096.0, 0.0, length(currentRootWorld));
		previousField.w = smoothstep(4096.0, 0.0, length(previousRootWorld));
		ApplySampledDeformation(input, currentPosition, previousPosition, input.InstanceData1.xyz,
			currentField, previousField, World[0], PreviousWorld[0],
			displacement, previousDisplacement, bendAxis, bendAngle);
	}
#endif
}
