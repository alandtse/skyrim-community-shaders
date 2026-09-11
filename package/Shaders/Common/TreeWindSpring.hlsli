#ifndef __TREE_WIND_SPRING_DEPENDENCY_HLSL__
#define __TREE_WIND_SPRING_DEPENDENCY_HLSL__

#include "Common/DampedSpring.hlsli"

namespace TreeWindSpring
{
	static const uint FieldCount = 3u;
	static const uint TransientHeightCount = 3u;

	struct FieldData
	{
		float2 FieldMinimum;
		float2 PreviousFieldMinimum;
		float FieldHeight;
		float FrameTime;
		float FieldSize;
		uint TextureSize;
		uint Initialize;
		uint FieldAvailable;
		float MaxDistance;
		float PreviousFieldHeight;
	};

#if defined(TREE_WIND_SPRING_COMPUTE)
	cbuffer SpringField : register(b0)
#else
	cbuffer SpringField : register(b3)
#endif
	{
		FieldData Fields[FieldCount];
		uint ActiveField;
		float SpringFrequency;
		float SpringDamping;
		float GustScale;
		float GustSoftLimit;
		uint TransientFieldMask;
		float TransientSpringFrequency;
		float TransientSpringDamping;
	};

	float GetTransientHeightHalfRange(uint fieldIndex)
	{
		return clamp(Fields[fieldIndex].MaxDistance * 0.0625f, 1024.0f, 4096.0f);
	}

#if !defined(TREE_WIND_SPRING_COMPUTE)
	Texture2D<float2> ResponseFields[FieldCount] : register(t111);
	Texture2D<float2> PreviousResponseFields[FieldCount] : register(t114);
	Texture2DArray<float4> TransientFields[FieldCount] : register(t119);
	Texture2DArray<float4> PreviousTransientFields[FieldCount] : register(t122);
	SamplerState ResponseSampler : register(s14);

	bool Contains(float2 worldPosition, float2 fieldMinimum, float fieldSize)
	{
		float2 uv = (worldPosition - fieldMinimum) / fieldSize;
		return all(uv >= 0.0f) && all(uv <= 1.0f);
	}

	uint SelectCurrentField(float2 worldPosition)
	{
		float2 fieldCenter = Fields[0].FieldMinimum + Fields[0].FieldSize * 0.5f;
		float distance = length(worldPosition - fieldCenter);
		if (distance < Fields[0].MaxDistance)
			return 0u;
		if (distance < Fields[1].MaxDistance)
			return 1u;
		return 2u;
	}

	uint SelectPreviousField(float2 worldPosition)
	{
		float2 fieldCenter = Fields[0].PreviousFieldMinimum + Fields[0].FieldSize * 0.5f;
		float distance = length(worldPosition - fieldCenter);
		if (distance < Fields[0].MaxDistance)
			return 0u;
		if (distance < Fields[1].MaxDistance)
			return 1u;
		return 2u;
	}

	bool IsInQualityRange(uint fieldIndex, float2 worldPosition, bool previous)
	{
		float2 fieldMinimum = previous ? Fields[0].PreviousFieldMinimum : Fields[0].FieldMinimum;
		float2 fieldCenter = fieldMinimum + Fields[0].FieldSize * 0.5f;
		float distance = length(worldPosition - fieldCenter);
		float minimumDistance = fieldIndex == 0u ? 0.0f : Fields[fieldIndex - 1u].MaxDistance;
		return distance >= minimumDistance && distance < Fields[fieldIndex].MaxDistance;
	}

	float2 SampleCurrentResponseField(uint fieldIndex, float2 uv)
	{
		float2 response = 0.0f.xx;
		if (fieldIndex == 0u)
			response = ResponseFields[0].SampleLevel(ResponseSampler, uv, 0.0f);
		else if (fieldIndex == 1u)
			response = ResponseFields[1].SampleLevel(ResponseSampler, uv, 0.0f);
		else
			response = ResponseFields[2].SampleLevel(ResponseSampler, uv, 0.0f);
		return response;
	}

	float2 SamplePreviousResponseField(uint fieldIndex, float2 uv)
	{
		float2 response = 0.0f.xx;
		if (fieldIndex == 0u)
			response = PreviousResponseFields[0].SampleLevel(ResponseSampler, uv, 0.0f);
		else if (fieldIndex == 1u)
			response = PreviousResponseFields[1].SampleLevel(ResponseSampler, uv, 0.0f);
		else
			response = PreviousResponseFields[2].SampleLevel(ResponseSampler, uv, 0.0f);
		return response;
	}

	float4 SampleCurrentTransientSlice(uint fieldIndex, float2 uv, uint heightIndex)
	{
		float4 transientSample = 0.0f.xxxx;
		if (fieldIndex == 0u)
			transientSample = TransientFields[0].SampleLevel(ResponseSampler, float3(uv, heightIndex), 0.0f);
		else if (fieldIndex == 1u)
			transientSample = TransientFields[1].SampleLevel(ResponseSampler, float3(uv, heightIndex), 0.0f);
		else
			transientSample = TransientFields[2].SampleLevel(ResponseSampler, float3(uv, heightIndex), 0.0f);
		return transientSample;
	}

	float4 SamplePreviousTransientSlice(uint fieldIndex, float2 uv, uint heightIndex)
	{
		float4 transientSample = 0.0f.xxxx;
		if (fieldIndex == 0u)
			transientSample = PreviousTransientFields[0].SampleLevel(ResponseSampler, float3(uv, heightIndex), 0.0f);
		else if (fieldIndex == 1u)
			transientSample = PreviousTransientFields[1].SampleLevel(ResponseSampler, float3(uv, heightIndex), 0.0f);
		else
			transientSample = PreviousTransientFields[2].SampleLevel(ResponseSampler, float3(uv, heightIndex), 0.0f);
		return transientSample;
	}

	float4 InterpolateCurrentTransient(uint fieldIndex, float2 uv, float worldHeight)
	{
		float heightHalfRange = GetTransientHeightHalfRange(fieldIndex);
		float heightCoordinate = saturate(
									 (worldHeight - (Fields[fieldIndex].FieldHeight - heightHalfRange)) /
									 (2.0f * heightHalfRange)) *
		                         (TransientHeightCount - 1u);
		uint lowerHeight = min((uint)floor(heightCoordinate), TransientHeightCount - 1u);
		uint upperHeight = min(lowerHeight + 1u, TransientHeightCount - 1u);
		return lerp(
			SampleCurrentTransientSlice(fieldIndex, uv, lowerHeight),
			SampleCurrentTransientSlice(fieldIndex, uv, upperHeight),
			frac(heightCoordinate));
	}

	float4 InterpolatePreviousTransient(uint fieldIndex, float2 uv, float worldHeight)
	{
		float heightHalfRange = GetTransientHeightHalfRange(fieldIndex);
		float heightCoordinate = saturate(
									 (worldHeight - (Fields[fieldIndex].PreviousFieldHeight - heightHalfRange)) /
									 (2.0f * heightHalfRange)) *
		                         (TransientHeightCount - 1u);
		uint lowerHeight = min((uint)floor(heightCoordinate), TransientHeightCount - 1u);
		uint upperHeight = min(lowerHeight + 1u, TransientHeightCount - 1u);
		return lerp(
			SamplePreviousTransientSlice(fieldIndex, uv, lowerHeight),
			SamplePreviousTransientSlice(fieldIndex, uv, upperHeight),
			frac(heightCoordinate));
	}

	/** @brief Samples packed current structural response and immediate leaf velocity. */
	bool TrySampleCurrentTransient(float3 worldPosition, out float4 transientSample)
	{
		transientSample = 0.0f.xxxx;
		uint fieldIndex = SelectCurrentField(worldPosition.xy);
		FieldData field = Fields[fieldIndex];
		if (field.FieldAvailable == 0u)
			return false;
		if (!IsInQualityRange(fieldIndex, worldPosition.xy, false) ||
			!Contains(worldPosition.xy, field.FieldMinimum, field.FieldSize))
			return true;
		float2 uv = saturate((worldPosition.xy - field.FieldMinimum) / field.FieldSize);
		transientSample = InterpolateCurrentTransient(fieldIndex, uv, worldPosition.z);
		return true;
	}

	/** @brief Samples packed previous structural response and immediate leaf velocity. */
	bool TrySamplePreviousTransient(float3 worldPosition, out float4 transientSample)
	{
		transientSample = 0.0f.xxxx;
		uint fieldIndex = SelectPreviousField(worldPosition.xy);
		FieldData field = Fields[fieldIndex];
		if (field.FieldAvailable == 0u)
			return false;
		if (!IsInQualityRange(fieldIndex, worldPosition.xy, true) ||
			!Contains(worldPosition.xy, field.PreviousFieldMinimum, field.FieldSize))
			return true;
		float2 uv = saturate((worldPosition.xy - field.PreviousFieldMinimum) / field.FieldSize);
		transientSample = InterpolatePreviousTransient(fieldIndex, uv, worldPosition.z);
		return true;
	}

	/** @brief Returns the combined current tree response or the mean wind outside the cache. */
	float2 SampleCurrent(float2 worldPosition)
	{
		uint fieldIndex = SelectCurrentField(worldPosition);
		FieldData field = Fields[fieldIndex];
		if (field.FieldAvailable == 0u ||
			!IsInQualityRange(fieldIndex, worldPosition, false) ||
			!Contains(worldPosition, field.FieldMinimum, field.FieldSize))
			return SharedData::WindFieldAmbient.xy;
		float2 uv = saturate((worldPosition - field.FieldMinimum) / field.FieldSize);
		return SampleCurrentResponseField(fieldIndex, uv);
	}

	/** @brief Returns the combined previous tree response or the prior mean wind outside the cache. */
	float2 SamplePrevious(float2 worldPosition)
	{
		uint fieldIndex = SelectPreviousField(worldPosition);
		FieldData field = Fields[fieldIndex];
		if (field.FieldAvailable == 0u ||
			!IsInQualityRange(fieldIndex, worldPosition, true) ||
			!Contains(worldPosition, field.PreviousFieldMinimum, field.FieldSize))
			return SharedData::WindFieldPreviousAmbient.xy;
		float2 uv = saturate((worldPosition - field.PreviousFieldMinimum) / field.FieldSize);
		return SamplePreviousResponseField(fieldIndex, uv);
	}
#endif
}

#endif  // __TREE_WIND_SPRING_DEPENDENCY_HLSL__
