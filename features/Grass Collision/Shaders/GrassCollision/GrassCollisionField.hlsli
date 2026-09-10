#ifndef __GRASS_COLLISION_FIELD_HLSLI__
#define __GRASS_COLLISION_FIELD_HLSLI__

#include "Common/SharedData.hlsli"

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

}

#endif
