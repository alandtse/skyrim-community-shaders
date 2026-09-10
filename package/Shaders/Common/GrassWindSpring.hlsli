#ifndef __GRASS_WIND_SPRING_DEPENDENCY_HLSL__
#define __GRASS_WIND_SPRING_DEPENDENCY_HLSL__

#include "Common/DampedSpring.hlsli"
#include "Common/Math.hlsli"

namespace GrassWindSpring
{
	static const uint QualityRangeCount = 3u;

	struct FieldData
	{
		float2 FieldMinimum;
		float2 PreviousFieldMinimum;
		float FieldHeight;
		float FrameTime;
		float ResponseRadians;
		float MaximumTiltRadians;
		float Sensitivity;
		float SpringFrequency;
		float SpringDamping;
		uint Initialize;
		uint FieldAvailable;
		float FieldSize;
		uint TextureSize;
		float MaxDistance;
	};

#if defined(GRASS_WIND_SPRING_COMPUTE)
	cbuffer SpringField : register(b0)
#else
	cbuffer SpringField : register(b3)
#endif
	{
		FieldData Fields[QualityRangeCount];
		uint ActiveField;
		uint TransientFieldMask;
		float2 SpringPadding;
	};

#if defined(GRASS_WIND_SPRING_COMPUTE)
	Texture2D<float4> PreviousResponse : register(t0);
	Texture2D<float4> PreviousVelocity : register(t1);
#else
	Texture2D<float4> ResponseFields[QualityRangeCount] : register(t105);
	Texture2D<float4> PreviousResponseFields[QualityRangeCount] : register(t108);
	SamplerState ResponseSampler : register(s14);
#endif

	float3 CalculateTarget(float3 windVelocity, FieldData field)
	{
		windVelocity *= max(field.Sensitivity, 0.0f);
		float lateralSpeed = length(windVelocity.xy);
		float targetAngle = field.MaximumTiltRadians > EPSILON_WIND_RESPONSE ?
		                        field.MaximumTiltRadians * tanh(lateralSpeed * max(field.ResponseRadians, 0.0f) / field.MaximumTiltRadians) :
		                        0.0f;
		float2 targetBend = lateralSpeed > EPSILON_WIND_RESPONSE ? windVelocity.xy * (targetAngle / lateralSpeed) : 0.0f.xx;
		float downwardSpeed = max(-windVelocity.z, 0.0f);
		float targetCompression = field.MaximumTiltRadians > EPSILON_WIND_RESPONSE ?
		                              saturate(tanh(downwardSpeed * max(field.ResponseRadians, 0.0f) / field.MaximumTiltRadians)) :
		                              0.0f;
		return float3(targetBend, targetCompression);
	}

#if !defined(GRASS_WIND_SPRING_COMPUTE)
	uint SelectField(float2 worldPosition)
	{
		float2 fieldCenter = Fields[0].FieldMinimum + Fields[0].FieldSize * 0.5f;
		float distance = length(worldPosition - fieldCenter);
		if (distance < Fields[0].MaxDistance)
			return 0u;
		if (distance < Fields[1].MaxDistance)
			return 1u;
		return 2u;
	}

	bool IsInQualityRange(uint fieldIndex, float2 worldPosition)
	{
		float2 fieldCenter = Fields[0].FieldMinimum + Fields[0].FieldSize * 0.5f;
		float distance = length(worldPosition - fieldCenter);
		float minimumDistance = fieldIndex == 0u ? 0.0f : Fields[fieldIndex - 1u].MaxDistance;
		return distance >= minimumDistance && distance < Fields[fieldIndex].MaxDistance;
	}

	bool Contains(float2 worldPosition, float2 fieldMinimum, float fieldSize)
	{
		float2 uv = (worldPosition - fieldMinimum) / fieldSize;
		return all(uv >= 0.0f) && all(uv <= 1.0f);
	}

	bool Contains(float2 worldPosition, FieldData field)
	{
		return Contains(worldPosition, field.FieldMinimum, field.FieldSize);
	}

	float4 SampleField(Texture2D<float4> field, float2 worldPosition, float2 fieldMinimum, float fieldSize)
	{
		float2 uv = (worldPosition - fieldMinimum) / fieldSize;
		return Contains(worldPosition, fieldMinimum, fieldSize) ?
		           field.SampleLevel(ResponseSampler, saturate(uv), 0.0f) :
		           0.0f.xxxx;
	}

	float4 SampleCurrent(uint fieldIndex, float2 worldPosition)
	{
		float4 sample = 0.0f.xxxx;
		if (fieldIndex == 0u && Fields[0].FieldAvailable != 0u)
			sample = SampleField(ResponseFields[0], worldPosition, Fields[0].FieldMinimum, Fields[0].FieldSize);
		else if (fieldIndex == 1u && Fields[1].FieldAvailable != 0u)
			sample = SampleField(ResponseFields[1], worldPosition, Fields[1].FieldMinimum, Fields[1].FieldSize);
		else if (fieldIndex == 2u && Fields[2].FieldAvailable != 0u)
			sample = SampleField(ResponseFields[2], worldPosition, Fields[2].FieldMinimum, Fields[2].FieldSize);
		return sample;
	}

	float4 SamplePrevious(uint fieldIndex, float2 worldPosition)
	{
		float4 sample = 0.0f.xxxx;
		if (fieldIndex == 0u && Fields[0].FieldAvailable != 0u)
			sample = SampleField(PreviousResponseFields[0], worldPosition, Fields[0].PreviousFieldMinimum, Fields[0].FieldSize);
		else if (fieldIndex == 1u && Fields[1].FieldAvailable != 0u)
			sample = SampleField(PreviousResponseFields[1], worldPosition, Fields[1].PreviousFieldMinimum, Fields[1].FieldSize);
		else if (fieldIndex == 2u && Fields[2].FieldAvailable != 0u)
			sample = SampleField(PreviousResponseFields[2], worldPosition, Fields[2].PreviousFieldMinimum, Fields[2].FieldSize);
		return sample;
	}

	bool HasTemporalCoverage(uint currentFieldIndex, uint previousFieldIndex,
		float2 worldPosition, float2 previousWorldPosition)
	{
		return Fields[currentFieldIndex].FieldAvailable != 0u &&
		       Fields[previousFieldIndex].FieldAvailable != 0u &&
		       IsInQualityRange(currentFieldIndex, worldPosition) &&
		       IsInQualityRange(previousFieldIndex, previousWorldPosition) &&
		       Contains(worldPosition, Fields[currentFieldIndex]) &&
		       Contains(previousWorldPosition, Fields[previousFieldIndex].PreviousFieldMinimum,
				   Fields[previousFieldIndex].FieldSize);
	}

	void ResolveModelBend(float4 fieldSample, float responseScale, float4x4 worldMatrix,
		float maximumTiltRadians, float compressionToBend,
		out float3 bendAxis, out float bendAngle, out float compression)
	{
		float3 modelBend = mul(transpose((float3x3)worldMatrix), float3(fieldSample.xy, 0.0f));
		modelBend.z = 0.0f;
		float modelBendMagnitude = length(modelBend);
		float3 bendDirection = modelBendMagnitude > EPSILON_WIND_RESPONSE ?
		                           modelBend / modelBendMagnitude :
		                           float3(1.0f, 0.0f, 0.0f);
		bendAxis = cross(float3(0.0f, 0.0f, 1.0f), bendDirection);
		float maximumBend = max(maximumTiltRadians, 0.0f);
		float downwardResponse = saturate(fieldSample.z * responseScale);
		float bendShare = saturate(compressionToBend);
		bendAngle = min(modelBendMagnitude * responseScale + downwardResponse * bendShare * maximumBend, maximumBend);
		compression = downwardResponse * (1.0f - bendShare);
	}
#endif
}

#endif  // __GRASS_WIND_SPRING_DEPENDENCY_HLSL__
