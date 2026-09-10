namespace GrassCollision
{
	Texture2D<float4> Deformation : register(t100);
	Texture2D<float4> PreviousDeformation : register(t101);
	SamplerState DeformationSampler : register(s15);

	const static uint TEXTURE_SIZE = 1024;
	const static float WORLD_SIZE = 8192.0;

	float2 GetFieldUV(float2 worldPosition, float2 positionOffset, uint2 arrayOrigin, out bool isValid)
	{
		float2 logicalUV = (worldPosition - positionOffset) / WORLD_SIZE + 0.5;
		isValid = all(logicalUV >= 0.0) && all(logicalUV <= 1.0);
		return frac(logicalUV + float2(arrayOrigin) / TEXTURE_SIZE);
	}

	float4 SampleCurrentDeformation(float2 worldPosition)
	{
		bool isValid;
		float2 uv = GetFieldUV(
			worldPosition, SharedData::grassCollisionData.PosOffset,
			SharedData::grassCollisionData.ArrayOrigin, isValid);
		return isValid ? Deformation.SampleLevel(DeformationSampler, uv, 0.0) : float4(0.0, 0.0, 0.0, 0.0);
	}

	float4 SamplePreviousDeformation(float2 worldPosition)
	{
		bool isValid;
		float2 uv = GetFieldUV(
			worldPosition, SharedData::grassCollisionData.PreviousPosOffset,
			SharedData::grassCollisionData.PreviousArrayOrigin, isValid);
		return isValid ? PreviousDeformation.SampleLevel(DeformationSampler, uv, 0.0) : float4(0.0, 0.0, 0.0, 0.0);
	}

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

	void ApplyDeformation(
		VS_INPUT input, float3 currentPosition, float3 previousPosition,
		out float3 displacement, out float3 previousDisplacement,
		out float3 bendAxis, out float bendAngle)
	{
		float3 currentRootWorld = mul(World[0], float4(input.InstanceData1.xyz, 1.0)).xyz;
		float3 previousRootWorld = mul(PreviousWorld[0], float4(input.InstanceData1.xyz, 1.0)).xyz;
		float currentNearFactor = smoothstep(4096.0, 0.0, length(currentRootWorld));
		float previousNearFactor = smoothstep(4096.0, 0.0, length(previousRootWorld));
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

		float4 currentField = SampleCurrentDeformation(currentRootWorld.xy);
		float4 previousField = SamplePreviousDeformation(previousRootWorld.xy);
		displacement = CalculateDisplacement(
			currentPosition, input.InstanceData1.xyz, currentField, bendWeight, compressionWeight,
			currentNearFactor, World[0], bendAxis, bendAngle);
		float3 previousBendAxis;
		float previousBendAngle;
		previousDisplacement = CalculateDisplacement(
			previousPosition, input.InstanceData1.xyz, previousField, bendWeight, compressionWeight,
			previousNearFactor, PreviousWorld[0], previousBendAxis, previousBendAngle);
	}
}
