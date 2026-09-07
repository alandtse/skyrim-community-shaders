#ifndef __SHARED_DATA_DEPENDENCY_HLSL__
#define __SHARED_DATA_DEPENDENCY_HLSL__

#include "Common/FrameBuffer.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "Common/VR.hlsli"

namespace SharedData
{
	cbuffer SharedData : register(b5)
	{
		float4 WaterData[25];
		float4 DirLightDirection;
		float4 DirLightColor;
		float4 SunDirection;
		float4 SunColor;
		float4 MasserDirection;
		float4 MasserColor;
		float4 SecundaDirection;
		float4 SecundaColor;
		float4 CameraData;
		float4 BufferDim;
		float Timer;
		uint FrameCount;
		uint FrameCountAlwaysActive;
		bool InInterior;  // If the current cell is an interior
		bool HasDirectionalShadows;
		bool InMapMenu;           // If the world/local map is open (note that the renderer is still deferred here)
		bool HideSky;             // HideSky flag in WorldSpace, e.g. Blackreach
		float MipBias;            // Offset to mip level for TAA sharpness
		float WaterSystemHeight;  // TES::GetWaterHeight at eye-0 in camera-relative Z; -FLT_MAX when no water body found (VR only)
		float3 pad0;
		float4 AmbientSHR;
		float4 AmbientSHG;
		float4 AmbientSHB;
		float4 VRFoveationData0;          // x=center coverage scale, y=feather, z=horizontal scale, w=SSR raymarch mode (0 off, 1 feathered, 2 hard cutoff)
		float4 VRFoveationCenterOffsets;  // xy=left eye center offset, zw=right eye center offset
		float4 HDRData;
		float RefractionScale;
		float3 pad1;
	};

	struct GrassLightingSettings
	{
		float Glossiness;
		float SpecularStrength;
		float SubsurfaceScatteringAmount;
		bool OverrideComplexGrassSettings;

		float BasicGrassBrightness;
		bool EnableWrappedLighting;
		float ComplexGrassThreshold;
		float1 pad0;
	};

	struct CPMSettings
	{
		bool EnableComplexMaterial;
		bool EnableParallax;
		bool EnableTerrainParallax;
		bool EnableHeightBlending;
		bool EnableShadows;
		bool EnableParallaxWarpingFix;
		uint2 pad0;
	};

	struct CubemapCreatorSettings
	{
		uint Enabled;
		float3 pad0;

		float4 CubemapColor;
	};

	struct TerraOccSettings
	{
		bool EnableTerrainShadow;
		float3 Scale;
		float2 ZRange;
		float2 Offset;
	};

	struct LightLimitFixSettings
	{
		uint EnableContactShadows;
		uint ContactShadowMaxSteps;
		float ContactShadowMaxDistance;
		float ContactShadowStride;
		float ContactShadowThickness;
		float ContactShadowDepthFade;
		float ContactShadowMinIntensity;
		uint ShadowMapSlots;  // total shadow map texture-array capacity
		// Cluster config (computed)
		uint4 ClusterSize;
		// Debug (last)
		uint EnableLightsVisualisation;
		uint LightsVisualisationMode;
		uint EnableParticleContactShadows;
		uint pad0;
	};

	struct WetnessEffectsSettings
	{
		row_major float4x4 OcclusionViewProj;

		float Time;
		float Raining;
		float Wetness;
		float PuddleWetness;

		bool EnableWetnessEffects;
		float MaxRainWetness;
		float MaxPuddleWetness;
		float MaxShoreWetness;
		float GrassWetnessRoughness;

		uint ShoreRange;
		float PuddleRadius;
		float PuddleMaxAngle;
		float PuddleMinWetness;

		float MinRainWetness;
		float SkinWetness;
		float WeatherTransitionSpeed;
		bool EnableRaindropFx;

		bool EnableSplashes;
		bool EnableRipples;
		uint EnableVanillaRipples;
		float RaindropFxRange;

		float RaindropGridSizeRcp;
		float RaindropIntervalRcp;
		float RaindropChance;
		float SplashesLifetime;

		float SplashesStrength;
		float SplashesMinRadius;
		float SplashesMaxRadius;
		float RippleStrength;

		float RippleRadius;
		float RippleBreadth;
		float RippleLifetimeRcp;
	};

	struct SkylightingSettings
	{
		row_major float4x4 OcclusionViewProj;
		float4 OcclusionDir;

		float4 PosOffset;   // xyz: cell origin in camera model space
		uint4 ArrayOrigin;  // xyz: array origin
		int4 ValidMargin;

		float MinDiffuseVisibility;
		float MinSpecularVisibility;
		uint2 pad0;
	};

	struct CloudShadowsSettings
	{
		float Opacity;
		float3 pad0;
	};

	struct CloudRelightSettings
	{
		uint enabled;
		float cloudRelightMix;
		float cloudOriginalMix;
		float silverLiningMix;

		float silverLiningSpread;
		float3 pad;
	};

	struct LODBlendingSettings
	{
		float LODTerrainBrightness;
		float LODObjectBrightness;
		float LODObjectSnowBrightness;
		bool DisableTerrainVertexColors;
		float LODTerrainGamma;
		float LODObjectGamma;
		float LODObjectSnowGamma;
		float pad0;
	};

	struct HairSpecularSettings
	{
		uint Enabled;
		float HairGlossiness;
		float SpecularMult;
		float DiffuseMult;
		uint EnableTangentShift;
		float PrimaryTangentShift;
		float SecondaryTangentShift;
		float HairSaturation;
		float SpecularIndirectMult;
		float DiffuseIndirectMult;
		float BaseColorMult;
		float Transmission;
		uint EnableSelfShadow;
		float SelfShadowStrength;
		float SelfShadowExponent;
		float SelfShadowScale;
		uint HairMode;  // 0: Kajiya-Kay, 1: Marschner
		uint3 pad;
	};

	/** @brief Terrain Variation feature settings. */
	struct TerrainVariationSettings
	{
		uint enableLODTerrainTilingFix;  ///< 1 = apply variation to LOD terrain.
		uint3 pad;
	};

	struct IBLSettings
	{
		uint EnableIBL;
		uint PreserveFogLuminance;
		uint UseStaticIBL;
		float DALCAmount;
		float EnvIBLScale;
		float SkyIBLScale;
		float EnvIBLSaturation;
		float SkyIBLSaturation;
		float FogAmount;
		uint DALCMode;  // 0: Luminance Ratio, 1: Color Ratio, 2: DALC + Sky, 3: DALC + Sky (Directional)
		float pad0;
		float pad1;
	};

	struct ExtendedTranslucencySettings
	{
		uint MaterialModel;  // [0,1,2,3] The MaterialModel
		float Reduction;     // [0, 1.0] The factor to reduce the transparency to matain the average transparency [0,1]
		float Softness;      // [0, 2.0] The soft remap upper limit [0,2]
		float Strength;      // [0, 1.0] The inverse blend weight of the effect
	};

	struct CSUtilitySettings
	{
		float skyBrightness;
		float directionalLightMult;
		float pointLightMult;
		float linearPointLightMult;
		float spotlightMult;
		float linearSpotlightMult;
		float omnidirectionalBulbMult;
		float linearOmnidirectionalBulbMult;
		float waterBrightness;
		float waterReflectionAmount;
		float waterRefractionAmount;
		float waterSunSpecularMultiplier;
		float waterWaveAmplitude;
		float waterFresnelMin;
		float waterFresnelMax;
		float waterMuddiness;
	};

	struct LinearLightingSettings
	{
		uint enableLinearLighting;
		uint enableACEScg;
		uint isDirLightLinear;
		float dirLightMult;
		float lightGamma;
		float colorGamma;
		float emitColorGamma;
		float glowmapGamma;
		float ambientGamma;
		float fogGamma;
		float fogAlphaGamma;
		float effectGamma;
		float effectAlphaGamma;
		float skyGamma;
		float waterGamma;
		float vlGamma;
		float ambientMult;
		float vanillaDiffuseColorMult;
		float emitColorMult;
		float glowmapMult;
		float effectLightingMult;
		float membraneEffectMult;
		float bloodEffectMult;
		float projectedEffectMult;
		float deferredEffectMult;
		float otherEffectMult;
		float2 pad0;
	};

	struct ENBSettings
	{
		uint Enable;
		float ColorPow;
		float LightSpriteIntensity;
		float FireIntensity;

		float FireCurve;
		uint EnableRain;
		float RainMotionStretch;
		float RainMotionTransparency;

		float CloudsCurve;
		float CloudsDesaturation;
		float CloudsEdgeIntensity;
		float CloudsEdgeMoonMultiplier;

		uint EnableProceduralSun;
		float ProceduralSunDiskRadiusSq;
		float ProceduralSunDiskEdgeScale;
		float ProceduralSunGlowIntensity;

		float ProceduralSunCoronaFalloff;
		float ProceduralSunCoronaScale;
		uint UseProceduralGradientWeights;
		float ProceduralGradientWeightCurve;

		float ParticleIntensity;
		float ParticleLightingInfluence;
		float ParticleAmbientInfluence;
		float ParticlePointLightingInfluence;

		uint EnableVolumetricRays;
		float VolumetricRaysIntensity;
		float VolumetricRaysExtinction;
		float VolumetricRaysSkyColorAmount;

		float VolumetricRaysDesaturation;
		float3 VolumetricRaysColorFilter;
	};
	struct TerrainBlendingSettings
	{
		uint Enabled;
		uint3 _padding;
	};

	struct ExponentialHeightFogSettings
	{
		uint enabled;
		uint useDynamicCubemaps;
		float startDistance;
		float fogHeight;
		float fogHeightFalloff;
		float fogDensity;
		float directionalInscatteringMultiplier;
		float directionalInscatteringAnisotropy;
		float4 inscatteringTint;
		float cubemapMipLevel;
		float sunlightAttenuationAmount;
		uint respectVanillaFogFade;
		uint disableVanillaFog;
		float4 fogInscatteringColor;
		float originalFogColorAmount;
		uint volumetricFogEnabled;
		uint volumetricGridPixelSize;
		uint volumetricGridSizeZ;
		float volumetricFogDistance;
		float volumetricFogStartDistance;
		float volumetricFogNearFadeInDistance;
		float volumetricFogExtinctionScale;
		float4 volumetricFogAlbedo;
		float4 volumetricFogEmissive;
		float volumetricDirectionalScatteringIntensity;
		float volumetricShadowBias;
		float volumetricDepthDistributionScale;
		float volumetricSkyLightingIntensity;
		float volumetricFogScatteringDistribution;
		float volumetricHistoryWeight;
		uint volumetricHistoryMissSampleCount;
		float volumetricSampleJitterMultiplier;
		float volumetricUpsampleJitterMultiplier;
		float volumetricLocalLightScatteringIntensity;
		float2 pad0;
	};

	struct TruePBRSettings
	{
		float VertexAOStrength;
		uint3 pad;
	};

	struct FoliageLightingSettings
	{
		uint EnableFoliageScattering;
		uint EnableFoliageAmbientBoost;
		uint EnableFoliageAmbientFlip;
		float FoliageAmbientAmount;
		uint EnableGrassScattering;
		uint3 pad;
	};

	struct SkinData
	{
		float4 skinParams;
		float4 skinParams2;
		float4 skinDetailParams;
		float4 sssParams;
		float4 fuzzParams;
		float4 physicalParams;
		float4 wetParams;
	};

	struct VanillaFresnelSettings
	{
		uint Enable;
		uint EnableGGX;
		uint EnableGGXOnGrass;
		uint EnableDynamicCubemapsConversion;
		uint EnableEyeSpecialHandling;
		float RoughnessMultiplier;
		float SpecularRoughnessBlend;
		float BaseF0Multiplier;
		float MinF0;
		float CubemapToF0Multiplier;
		float ComplexMaterialF0Multiplier;
		float pad;
	};

	struct BloomSettings
	{
		uint Enabled;
		float EnhancementIntensity;
		float HaloRadius;
		float HaloSpread;

		float BloomSaturation;
		float3 BloomTint;
		float CompressionThreshold;
		float CompressionCeiling;
		float2 pad;
	};

	struct PostProcessingSettings
	{
		uint DisableVanillaTonemapping;
		uint3 pad0;
	};

	cbuffer FeatureData : register(b6)
	{
		GrassLightingSettings grassLightingSettings;
		CPMSettings extendedMaterialSettings;
		CubemapCreatorSettings cubemapCreatorSettings;
		TerraOccSettings terraOccSettings;
		LightLimitFixSettings lightLimitFixSettings;
		WetnessEffectsSettings wetnessEffectsSettings;
		SkylightingSettings skylightingSettings;
		CloudShadowsSettings cloudShadowsSettings;
		CloudRelightSettings cloudRelightSettings;
		LODBlendingSettings lodBlendingSettings;
		HairSpecularSettings hairSpecularSettings;
		TerrainVariationSettings terrainVariationSettings;
		IBLSettings iblSettings;
		ExtendedTranslucencySettings extendedTranslucencySettings;
		CSUtilitySettings csUtilitySettings;
		LinearLightingSettings linearLightingSettings;
		ENBSettings enbSettings;
		TerrainBlendingSettings terrainBlendingSettings;
		ExponentialHeightFogSettings exponentialHeightFogSettings;
		TruePBRSettings truePBRSettings;
		FoliageLightingSettings foliageLightingSettings;
		SkinData skinData;
		VanillaFresnelSettings vanillaFresnelSettings;
		BloomSettings bloomSettings;
		PostProcessingSettings postProcessingSettings;
	};

	Texture2D<float4> DepthTexture : register(t17);

	// Get a int3 to be used as texture sample coord. [0,1] in uv space
	int3 ConvertUVToSampleCoord(float2 uv, uint a_eyeIndex)
	{
		uv = Stereo::ConvertToStereoUV(uv, a_eyeIndex);
		uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
		return int3(uv * BufferDim.xy, 0);
	}

	// Get a raw depth from the depth buffer. [0,1] in uv space
	float GetDepth(float2 uv, uint a_eyeIndex = 0)
	{
		return DepthTexture.Load(ConvertUVToSampleCoord(uv, a_eyeIndex)).x;
	}

	float GetScreenDepth(float depth)
	{
		return (CameraData.w / (-depth * CameraData.z + CameraData.x));
	}

	float4 GetScreenDepths(float4 depths)
	{
		return (CameraData.w / (-depths * CameraData.z + CameraData.x));
	}

	float GetScreenDepth(float2 uv, uint a_eyeIndex = 0)
	{
		float depth = GetDepth(uv, a_eyeIndex);
		return GetScreenDepth(depth);
	}

	// Returns water data for the tile containing worldPosition (camera-relative XY).
	// The .w component (water surface height) is stored in C++ as camera-relative Z of
	// eye 0 (left eye).  Pass eyeIndex to have .w corrected into the current eye's
	// camera-relative frame; defaults to 0 (no correction, backwards-compatible).
	float4 GetWaterData(float3 worldPosition, uint eyeIndex = 0)
	{
		float2 cellF = (((worldPosition.xy + FrameBuffer::CameraPosAdjust[0].xy)) / 4096.0) + 64.0;  // always positive
		int2 cellInt;
		float2 cellFrac = modf(cellF, cellInt);

		cellF = worldPosition.xy / float2(4096.0, 4096.0);  // remap to cell scale
		cellF += 2.5;                                       // 5x5 cell grid
		cellF -= cellFrac;                                  // align to cell borders
		cellInt = round(cellF);

		uint waterTile = (uint)clamp(cellInt.x + (cellInt.y * 5), 0, 24);  // remap xy to 0-24

		float4 waterData = float4(1.0, 1.0, 1.0, -2147483648);

		[flatten] if (cellInt.x < 5 && cellInt.x >= 0 && cellInt.y < 5 && cellInt.y >= 0)
			waterData = WaterData[waterTile];

#if defined(VR)
		// Correct .w from eye-0 camera-relative Z to the current eye's camera-relative Z.
		// No-op when eyeIndex == 0 (both terms are identical).
		waterData.w += FrameBuffer::CameraPosAdjust[0].z - FrameBuffer::CameraPosAdjust[eyeIndex].z;
#endif

		return waterData;
	}

	float3 GetAmbient(float3 normal)
	{
		return SphericalHarmonics::Unproject(AmbientSHR, AmbientSHG, AmbientSHB, normal);
	}
}
#endif  // __SHARED_DATA_DEPENDENCY_HLSL__
