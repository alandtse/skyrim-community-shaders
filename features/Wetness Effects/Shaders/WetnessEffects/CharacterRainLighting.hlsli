#ifndef __CHARACTER_RAIN_LIGHTING_HLSLI__
#define __CHARACTER_RAIN_LIGHTING_HLSLI__

#include "Common/LightingEval.hlsli"
#include "Common/SharedData.hlsli"

namespace CharacterRainSpots
{
	static const float InverseLn2 = 1.44269504089f;

	/** @brief Converts artistic coat intensity into bounded optical coverage. */
	float GetCoatWeight(float coverage, float intensity)
	{
		return 1.0f - exp2(-InverseLn2 * saturate(coverage) * max(intensity, 0.0f));
	}

	/** @brief Gives covered water cores their own deferred roughness without strengthening filtered borders. */
	float GetCoatDominance(float coverage, float intensity)
	{
		return GetCoatWeight(coverage, intensity);
	}

	/** @brief Returns a water highlight independent of the underlying material's specular masks. */
	float3 EvaluateLighting(float3 normal, DirectContext context, float roughness, float coverage, float intensity)
	{
		float3 result = 0.0f;
		[branch] if (coverage > 0.0f && intensity > 0.0f)
		{
#if defined(TRUE_PBR)
			context.viewDir = context.coatViewDir;
			context.lightDir = context.coatLightDir;
			context.halfVector = context.coatHalfVector;
#endif
			DirectLightingOutput coated = (DirectLightingOutput)0;
			EvaluateWetnessLighting(normal, context, roughness, coated);
			result = coated.specular * GetCoatWeight(coverage, intensity);
		}
		return result;
	}

	/** @brief Returns coverage-weighted water reflectance without darkening the underlying material. */
	float3 EvaluateIndirect(float3 normal, IndirectContext context, float roughness, float coverage, float intensity)
	{
		float3 result = 0.0f;
		[branch] if (coverage > 0.0f && intensity > 0.0f)
		{
			IndirectLobeWeights coated = (IndirectLobeWeights)0;
			float3 reflection = GetWetnessIndirectLobeWeights(coated, normal, roughness, context);
			result = reflection * GetCoatWeight(coverage, intensity);
		}
		return result;
	}

	/** @brief Builds the bead normal in world units independently of mesh UV scale or seams. */
	float3 GetSurfaceNormal(float dropHeight, float3 surfaceWorldPosition, float3 baseSurfaceNormal)
	{
		float3 surfaceDx = ddx(surfaceWorldPosition);
		float3 surfaceDy = ddy(surfaceWorldPosition);
		float2 dropHeightGradient = float2(ddx(dropHeight), ddy(dropHeight));
		float3 tangentBasisX = cross(surfaceDy, baseSurfaceNormal);
		float3 tangentBasisY = cross(baseSurfaceNormal, surfaceDx);
		float determinant = dot(surfaceDx, tangentBasisX);
		if (abs(determinant) <= 1e-8f)
			return baseSurfaceNormal;
		float3 normalGradient = (dropHeightGradient.x * tangentBasisX + dropHeightGradient.y * tangentBasisY) / determinant;
		normalGradient *= min(1.0f, 2.0f * rsqrt(max(dot(normalGradient, normalGradient), 1e-8f)));
		return normalize(baseSurfaceNormal - normalGradient);
	}
}

#endif
