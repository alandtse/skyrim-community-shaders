#ifndef __RAIN_MATERIAL_HLSLI__
#define __RAIN_MATERIAL_HLSLI__

#ifdef PSHADER
#	include "Common/Math.hlsli"
#	include "Common/Color.hlsli"
#	include "Common/BRDF.hlsli"

Texture2D<float4> RainNormalOpacity : register(t3);
TextureCube<float3> RainEnvironment : register(t4);

namespace RainMaterial
{
	static const float WaterIndexOfRefraction = 1.333f;
	static const float EnvironmentTransmission = 0.8f;
	static const float WaterFresnelRatio = (1.0f - WaterIndexOfRefraction) / (1.0f + WaterIndexOfRefraction);
	static const float WaterFresnelF0 = WaterFresnelRatio * WaterFresnelRatio;
	static const float UnresolvedWaterFresnel = 0.08f;
	static const float LensDeflectionScale = 2.0f;
	static const float MaximumSurfaceFresnel = 0.35f;
	static const float HighlightAxialCurvature = 0.65f;
	static const float HighlightTailResponse = 0.08f;

	/** @brief Filtered water shape and light response in the shared world-space streak basis. */
	struct Surface
	{
		float Opacity;
		float Core;
		float Fresnel;
		float2 Distortion;
		float3 Reflection;
		float3 EnvironmentTransmission;
		float3 NormalWorld;
		float3 LocalLighting;
		float3 DirectLighting;
		float3 ScatteredLighting;
	};

	/** @brief Evaluates a filtered water highlight without tinting it by the environment reflection. */
	float3 DirectHighlight(float3 normal, float3 viewDirection, float3 lightDirection, float roughness)
	{
		float3 halfVector = SafeNormalize(viewDirection + lightDirection, viewDirection);
		float normalLight = saturate(dot(normal, lightDirection));
		float normalView = max(abs(dot(normal, viewDirection)), 0.01f);
		float normalHalf = saturate(dot(normal, halfVector));
		float viewHalf = saturate(dot(viewDirection, halfVector));
		return BRDF::D_GGX(roughness, normalHalf) * BRDF::Vis_SmithJointApprox(roughness, normalView, max(normalLight, 0.01f)) *
		       BRDF::F_Schlick(WaterFresnelF0.xxx, viewHalf) * normalLight;
	}

	/** @brief Reads the same directional light color convention used by scene materials. */
	float3 DirectionalIrradiance()
	{
		bool isLinear = SharedData::linearLightingSettings.isDirLightLinear;
		float multiplier = SharedData::linearLightingSettings.enableLinearLighting && !isLinear && !SharedData::InInterior ?
		                       SharedData::linearLightingSettings.dirLightMult :
		                       1.0f;
		float3 lightColor = Color::DirectionalLight(max(SharedData::DirLightColor.rgb, 0.0f) / max(multiplier, EPSILON_DIVISION), isLinear) * multiplier;
		return Color::IrradianceToLinear(lightColor) * Appearance.x * Appearance.y;
	}

	float TransmittedHighlight(float3 normal, float3 viewDirection, float3 lightDirection, float roughness)
	{
		float3 transmittedDirection = refract(-viewDirection, normal, 1.0f / WaterIndexOfRefraction);
		float alignment = saturate(dot(transmittedDirection, lightDirection));
		return pow(alignment, lerp(64.0f, 8.0f, roughness));
	}

	/** @brief Shades one textured drop using a head-centered view shared by both eyes. */
	Surface Evaluate(RainVertexOutput input, float silhouetteFade, float resolvedWidth, float intersectionFade)
	{
		Surface water = (Surface)0;
		float3 normalTS = float3(0.0f, 0.0f, 1.0f);
		float detailWeight = input.DetailFade * resolvedWidth;
		float mip = input.ScreenAlongAndLength.w;
		[branch] if (TexturedRain.x > 0.5f)
		{
			float2 textureUV = float2(0.5f + input.StreakCoordinate.y * RainTextureShape.z * 0.5f, input.StreakCoordinate.x);
			float4 normalOpacity = RainNormalOpacity.SampleLevel(RefractionSampler, textureUV, mip);
			water.Opacity = saturate(normalOpacity.a) * silhouetteFade;
			if (input.ColorOpacity.a * water.Opacity * intersectionFade <= RainMinimumOpticalCoverage)
				discard;
			float3 textureNormal = normalize(float3((normalOpacity.xy * 2.0f - 1.0f) * TexturedRain.y, max(normalOpacity.z * 2.0f - 1.0f, 0.05f)));
			normalTS = normalize(lerp(normalTS, textureNormal, detailWeight));
		}
		else discard;
		float3 planeNormal = cross(input.StreakSideWorld, input.StreakAxisWorld);
		planeNormal *= dot(planeNormal, input.HeadViewDirection) < 0.0f ? -1.0f : 1.0f;
		float3 normalWS = normalize(input.StreakSideWorld * normalTS.x + input.StreakAxisWorld * normalTS.y + planeNormal * normalTS.z);
		water.NormalWorld = normalWS;
		float facing = saturate(dot(normalWS, input.HeadViewDirection));
		water.Fresnel = WaterFresnelF0 + (1.0f - WaterFresnelF0) * pow(1.0f - facing, 5.0f);
		// Subpixel streaks use a stable average instead of aliasing between their center and rim.
		water.Fresnel = lerp(UnresolvedWaterFresnel, water.Fresnel, resolvedWidth);
		water.Core = saturate(normalTS.z);
		// Normalize the curved profile so the refraction control can reach its requested displacement.
		water.Distortion = -LensDeflectionScale * (input.ScreenSideAndWidth.xy * normalTS.x + input.ScreenAlongAndLength.xy * normalTS.y) * water.Core;

		float3 incident = -input.HeadViewDirection;
		float3 reflectedDirection = reflect(incident, normalWS);
		float3 reflectedColor = Color::IrradianceToLinear(Color::Light(max(LightColor.rgb, 0.0f))) * Appearance.x * 0.1f;
		float sceneMix = Refraction.y > 0.5f ? input.DetailFade * MaterialLighting.z : 0.0f;
		[branch] if (TexturedRain.w > 0.5f && Refraction.w * (1.0f - sceneMix) > 0.0f)
		{
			float3 transmittedDirection = refract(incident, normalWS, 1.0f / WaterIndexOfRefraction);
			transmittedDirection = SafeNormalize(lerp(incident, transmittedDirection, resolvedWidth), incident);
			float transmissionMip = lerp(5.0f, clamp(mip * 0.25f, 0.0f, 2.0f), detailWeight);
			float3 transmittedColor = RainEnvironment.SampleLevel(RefractionSampler, transmittedDirection, transmissionMip);
			water.EnvironmentTransmission = Color::IrradianceToLinear(max(transmittedColor, 0.0f)) * Appearance.x;
		}
		[branch] if (TexturedRain.w > 0.5f && detailWeight > 0.0f && TexturedRain.z > 0.0f && Glassy.z > 0.0f)
		{
			float environmentMip = clamp(mip * 0.25f, 0.75f, 2.0f);
			float3 environmentColor = Color::IrradianceToLinear(max(RainEnvironment.SampleLevel(RefractionSampler, reflectedDirection, environmentMip), 0.0f)) * Appearance.x;
			reflectedColor = lerp(reflectedColor, environmentColor, detailWeight);
		}
		water.Reflection = reflectedColor * water.Fresnel * TexturedRain.z * Glassy.z;
		float roughness = lerp(0.45f, MaterialLighting.x, detailWeight);
		// Broaden highlights on narrow ribbons so their curved normals do not create subpixel flashes.
		float pixelWidth = max(input.ScreenSideAndWidth.z * 2.0f, 1.0f);
		roughness = sqrt(saturate(roughness * roughness + 0.25f / (pixelWidth * pixelWidth)));
		float axialCoordinate = input.StreakCoordinate.x * 2.0f - 1.0f;
		float axialProfile = saturate(1.0f - axialCoordinate * axialCoordinate);
		float highlightProfile = lerp(HighlightTailResponse, 1.0f, axialProfile * axialProfile * axialProfile);
		float resolvedLength = saturate(input.ScreenAlongAndLength.z * 0.5f);
		highlightProfile = lerp(HighlightTailResponse + (1.0f - HighlightTailResponse) * (16.0f / 35.0f), highlightProfile, resolvedLength);
		float3 highlightNormal = normalize(normalWS + input.StreakAxisWorld * axialCoordinate * HighlightAxialCurvature * detailWeight);
		float3 localIrradiance = max(input.ColorOpacity.rgb, 0.0f);
		float directionConfidence = saturate(dot(input.LightDirection.xyz, input.LightDirection.xyz));
		float3 localDirectLighting = 0.0f;
		float3 localScatteredLighting = 0.0f;
		[branch] if (any(localIrradiance > 0.0f) && directionConfidence > 0.0f)
		{
			float3 localDirection = SafeNormalize(input.LightDirection.xyz, input.HeadViewDirection);
			float3 localHighlight = DirectHighlight(highlightNormal, input.HeadViewDirection, localDirection, roughness) * directionConfidence * highlightProfile;
			localDirectLighting = localIrradiance * localHighlight * TexturedRain.z;
			float localTransmission = TransmittedHighlight(highlightNormal, input.HeadViewDirection, localDirection, roughness) * directionConfidence * highlightProfile;
			localScatteredLighting =
				localIrradiance * MaterialLighting.y * localTransmission * (1.0f - water.Fresnel);
		}
		water.LocalLighting = localDirectLighting + localScatteredLighting;
		float3 sunDirection = SafeNormalize(SharedData::DirLightDirection.xyz, float3(0.0f, 0.0f, 1.0f));
		float3 sunHighlight = DirectHighlight(highlightNormal, input.HeadViewDirection, sunDirection, roughness) * highlightProfile;
		float sunScattering = TransmittedHighlight(highlightNormal, input.HeadViewDirection, sunDirection, roughness) * highlightProfile;
		float3 directionalIrradiance = DirectionalIrradiance();
		float3 directionalDirectLighting = directionalIrradiance * sunHighlight * TexturedRain.z;
		water.DirectLighting = localDirectLighting + directionalDirectLighting;
		water.ScatteredLighting = localScatteredLighting +
		                          directionalIrradiance * MaterialLighting.y * sunScattering * (1.0f - water.Fresnel);
		return water;
	}
}
#endif

#endif
