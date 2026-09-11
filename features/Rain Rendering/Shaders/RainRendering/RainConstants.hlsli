#ifndef RAIN_CONSTANTS_HLSLI
#define RAIN_CONSTANTS_HLSLI

static const float RainMinimumOpticalCoverage = 1e-4f;

cbuffer RainConstants : register(b0)
{
	float4 HeadPositionAndTime;
	float4 VolumeSizeAndDensity;
	float4 WeatherFallDepth;
	float4 Streak;
	float4 Appearance;
	float4 DistanceNoise;
	float4 Curtain;
	float4 CurtainDensity;
	float4 LightColor;
	float4 RainCameraData;
	uint4 GridAndDebug;
	float4 Glassy;
	float4 Refraction;
	float4 ScreenSize;
	float4 LocalLighting;
	uint4 RainLightGrid;
	float4 TexturedRain;
	float4 RainTextureShape;
	float4 LayerRadii;
	uint4 LayerCounts;
	float4 MaterialLighting;
	float4 RoofOcclusion;
	float4 VanillaWind;
	float4 RainFrustumPlanes[12];
	float4 RainFrustumPlaneLengths[3];
}

#endif
