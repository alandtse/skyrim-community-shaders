#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

struct VS_INPUT
{
	float4 Position: POSITION0;

#if defined(TEX) || defined(HORIZFADE)
	float2 TexCoord: TEXCOORD0;
#endif

	float4 Color: COLOR0;
#if defined(VR)
	uint InstanceID: SV_INSTANCEID;
#endif  // VR
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;

#if defined(DITHER) && defined(TEX)
	float4 TexCoord0: TEXCOORD0;
#elif defined(DITHER)
	float2 TexCoord0: TEXCOORD3;
#elif defined(TEX) || defined(HORIZFADE)
	float2 TexCoord0: TEXCOORD0;
#endif

#if defined(TEXLERP)
	float2 TexCoord1: TEXCOORD1;
#endif

#if defined(HORIZFADE)
	float TexCoord2: TEXCOORD2;
#endif

#if defined(TEX) || defined(DITHER) || defined(HORIZFADE)
	float4 Color: COLOR0;
#endif

#if !defined(OCCLUSION) && !defined(MOONMASK) && !defined(HORIZFADE)
	float4 SkyBlendColor0: TEXCOORD5;
	float4 SkyBlendColor2: TEXCOORD6;
#endif

	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
	float3 FogPosition: TEXCOORD4;
#if defined(VR)
	float ClipDistance: SV_ClipDistance0;  // o11
	float CullDistance: SV_CullDistance0;  // p11
	uint EyeIndex: EYEIDX0;
#endif  // VR
};

#ifdef VSHADER
cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	row_major float4x4 WorldViewProj[1] : packoffset(c0);
	row_major float4x4 World[1] : packoffset(c4);
	row_major float4x4 PreviousWorld[1] : packoffset(c8);
	float3 EyePosition[1] : packoffset(c12);
	float VParams : packoffset(c12.w);
	float4 BlendColor[3] : packoffset(c13);
	float2 TexCoordOff : packoffset(c16);
#	else
	row_major float4x4 WorldViewProj[2] : packoffset(c0);
	row_major float4x4 World[2] : packoffset(c8);
	row_major float4x4 PreviousWorld[2] : packoffset(c16);
	float3 EyePosition[2] : packoffset(c24);
	float VParams : packoffset(c25.w);
	float4 BlendColor[3] : packoffset(c26);
	float2 TexCoordOff : packoffset(c29);
#	endif  // !VR
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;
	uint eyeIndex = Stereo::GetEyeIndexVS(
#	if defined(VR)
		input.InstanceID
#	endif
	);

	float4 inputPosition = float4(input.Position.xyz, 1.0);

#	if defined(OCCLUSION)

	// Intentionally left blank

#	elif defined(MOONMASK)

	vsout.TexCoord0 = input.TexCoord;
	vsout.Color = float4(VParams.xxx, 1.0);

#	elif defined(HORIZFADE)

	float worldHeight = mul(World[eyeIndex], inputPosition).z;
	float eyeHeightDelta = -EyePosition[eyeIndex].z + worldHeight;

	vsout.TexCoord0.xy = input.TexCoord;
	vsout.TexCoord2.x = saturate((1.0 / 17.0) * eyeHeightDelta);
	vsout.Color.xyz = BlendColor[0].xyz * VParams;
	vsout.Color.w = BlendColor[0].w;

#	else  // MOONMASK HORIZFADE

#		if defined(DITHER)

#			if defined(TEX)
	vsout.TexCoord0.xyzw = input.TexCoord.xyxy * float4(1.0, 1.0, 501.0, 501.0);
#			else
	float3 inputDirection = normalize(input.Position.xyz);
	inputDirection.y += inputDirection.z;

	vsout.TexCoord0.x = 501 * acos(inputDirection.x);
	vsout.TexCoord0.y = 501 * asin(inputDirection.y);
#			endif  // TEX

#		elif defined(CLOUDS)
	vsout.TexCoord0.xy = TexCoordOff + input.TexCoord;
#		else
	vsout.TexCoord0.xy = input.TexCoord;
#		endif  // DITHER CLOUDS

#		ifdef TEXLERP
	vsout.TexCoord1.xy = TexCoordOff + input.TexCoord;
#		endif  // TEXLERP

	float3 skyColor = BlendColor[0].xyz * input.Color.xxx + BlendColor[1].xyz * input.Color.yyy +
	                  BlendColor[2].xyz * input.Color.zzz;

	vsout.Color.xyz = VParams * skyColor;
	vsout.Color.w = BlendColor[0].w * input.Color.w;
	vsout.SkyBlendColor0 = float4(BlendColor[0].xyz * VParams, 0);
	vsout.SkyBlendColor2 = float4(BlendColor[2].xyz * VParams, 0);
#	endif      // OCCLUSION MOONMASK HORIZFADE

	vsout.Position = mul(WorldViewProj[eyeIndex], inputPosition).xyww;
	vsout.WorldPosition = mul(World[eyeIndex], inputPosition);
	vsout.FogPosition = vsout.WorldPosition.xyz - EyePosition[eyeIndex].xyz;
	vsout.PreviousWorldPosition = mul(PreviousWorld[eyeIndex], inputPosition);

#	ifdef VR
	vsout.EyeIndex = eyeIndex;
	Stereo::VR_OUTPUT VRout = Stereo::GetVRVSOutput(vsout.Position, eyeIndex);
	vsout.Position = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#	endif  // VR
	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
	float4 MotionVectors: SV_Target1;
	float4 Normal: SV_Target2;
#if defined(CLOUD_SHADOWS) && defined(CLOUDS) && !defined(DEFERRED)
	float4 CloudShadows: SV_Target3;
#endif
};

#ifdef PSHADER
SamplerState SampBaseSampler : register(s0);
SamplerState SampBlendSampler : register(s1);
SamplerState SampNoiseGradSampler : register(s2);

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexBlendSampler : register(t1);
Texture2D<float4> TexNoiseGradSampler : register(t2);

cbuffer PerGeometry : register(b2)
{
	float2 PParams : packoffset(c0);
};

#	if !defined(VR)
cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#	endif

#	include "Common/MotionBlur.hlsli"
#	include "Common/SharedData.hlsli"

#	if defined(CLOUD_SHADOWS)
#		include "CloudShadows/CloudShadows.hlsli"
#	endif

#	if defined(CLOUD_RELIGHT) && defined(CLOUD_SHADOWS) && defined(TEX) && defined(CLOUDS)
#		define CR_CLOUDS
#		include "CloudRelight/CloudRelight.hlsli"
#	endif

#	if defined(EXP_HEIGHT_FOG)
#		define SampColorSampler SampBaseSampler
#		include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#	endif

#	ifdef HDR_OUTPUT
#		include "HDRDisplay/HDRSun.hlsli"
#	endif

Texture2D<float> TexDepthSampler : register(t17);

#	if defined(EFFECTS11)
float ComputeProceduralSun(float2 uv)
{
	float2 p = uv * 2.0 - 1.0;
	float dist = dot(p, p) - SharedData::enbSettings.ProceduralSunDiskRadiusSq;

	float c = saturate(dist * SharedData::enbSettings.ProceduralSunCoronaScale);
	float corona = (1.0 - c) * rcp(SharedData::enbSettings.ProceduralSunCoronaFalloff * c + 1.0) * SharedData::enbSettings.ProceduralSunGlowIntensity;

	float disk = saturate(-dist * SharedData::enbSettings.ProceduralSunDiskEdgeScale);

	return corona + disk;
}
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	// Color::Sky is float3->float3 (per-channel sky gamma). PParams.yyy broadcasts the packed
	// scalar in PParams.y to RGB; float3 matches output .xyz where skyScale is added.
	float3 skyScale = Color::Sky(PParams.yyy);
#	if !defined(VR)
	uint eyeIndex = 0;
#	else
	uint eyeIndex = input.EyeIndex;
#	endif  // !VR

#	ifndef OCCLUSION
#		ifndef TEXLERP
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
	baseColor.xyz = Color::Sky(baseColor.xyz);
#			ifdef TEXFADE
	baseColor.w *= PParams.x;
#			endif
#		else
	float4 blendColor = TexBlendSampler.Sample(SampBlendSampler, input.TexCoord1.xy);
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
	blendColor.xyz = Color::Sky(blendColor.xyz);
	baseColor.xyz = Color::Sky(baseColor.xyz);
	baseColor = PParams.xxxx * (-baseColor + blendColor) + baseColor;
#		endif
#		if defined(CR_CLOUDS)
	if (SharedData::cloudRelightSettings.enabled) {
		float3 viewDir = normalize(input.WorldPosition.xyz);
		baseColor.rgb = CloudRelight::RelightCloud(baseColor, viewDir, SampBaseSampler);
	}
#		endif

#		if defined(HDR_OUTPUT)
	float hdrSunGain = HDRSun::GetHdrSunGain(input.TexCoord0.xy, baseColor);
	baseColor.xyz *= hdrSunGain;
	if (HDRSun::IsHdrSunActive()) {
		// Dither bright output to reduce banding in high-boost sun path.
		// Same baseColor/skyScale treatment for DITHER and non-DITHER; DITHER adds noiseGrad later.
		baseColor.xyz += (Random::InterleavedGradientNoise(Stereo::EyeStableNoiseCoord(input.Position.xy, SharedData::BufferDim.xy)) - 0.5f) *
		                 (saturate(hdrSunGain - 1.0f) / 255.0f);
		skyScale = 0.0f;
	}
#		endif

#		if defined(TEX) && defined(EFFECTS11)
	if (SharedData::enbSettings.EnableProceduralSun && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun)) {
		baseColor.xyz = ComputeProceduralSun(input.TexCoord0.xy);
		baseColor.w = input.Color.w;
		skyScale = 0.0;
	}
#		endif

	float skyBrightnessMultiplier = SharedData::csUtilitySettings.skyBrightness;

#		if defined(DITHER)
	float2 noiseGradUv = float2(0.125, 0.125) * input.Position.xy;
	// The 8x8 engine dither texture stores 0..63, so center it without shifting the weather color.
	const float noiseGradCenter = 31.5 / 255.0;
	float noiseGrad = (TexNoiseGradSampler.Sample(SampNoiseGradSampler, noiseGradUv).x - noiseGradCenter) * 0.03125;

#			ifdef TEX
	float3 skyVertColor = ENABLE_LL ? (input.Color.xyz + noiseGrad) : input.Color.xyz;
	float3 sunGlareColor = Color::Sky(skyVertColor) * baseColor.xyz;
	// Dither/noise term is the legacy sky path contribution for gradient smoothing.
	psout.Color.xyz = ((sunGlareColor + skyScale) * skyBrightnessMultiplier) + (ENABLE_LL ? 0.0 : noiseGrad);
	psout.Color.w = baseColor.w * input.Color.w;
#			else
	float3 skyGradientColor = input.Color.xyz;

#				if defined(EFFECTS11)
	float3 viewDirection = normalize(input.WorldPosition.xyz);
	if (SharedData::enbSettings.UseProceduralGradientWeights) {
		float gradientPosition = pow(1.0 - saturate(viewDirection.z), SharedData::enbSettings.ProceduralGradientWeightCurve);
		skyGradientColor = lerp(input.SkyBlendColor2.xyz, input.SkyBlendColor0.xyz, gradientPosition);
	}
#				endif
	psout.Color.xyz = (skyScale + Color::Sky(skyGradientColor + noiseGrad)) * skyBrightnessMultiplier;
	psout.Color.w = input.Color.w;
#			endif  // TEX

#		elif defined(MOONMASK)
	psout.Color.xyzw = baseColor;

	if (baseColor.w - AlphaTestRefRS.x < 0) {
		discard;
	}

#		elif defined(HORIZFADE)
	psout.Color.xyz = float3(1.5, 1.5, 1.5) * ((Color::Sky(input.Color.xyz) * baseColor.xyz + skyScale) * skyBrightnessMultiplier);
	psout.Color.w = input.TexCoord2.x * (baseColor.w * input.Color.w);
#		else

#			if defined(CLOUDS) && defined(EFFECTS11)
	if (SharedData::enbSettings.Enable)
		baseColor.xyz = pow(abs(baseColor.xyz), SharedData::enbSettings.CloudsCurve);
#			endif

	psout.Color.w = input.Color.w * baseColor.w;
	psout.Color.xyz = (Color::Sky(input.Color.xyz) * baseColor.xyz + skyScale) * skyBrightnessMultiplier;

#			if defined(CLOUDS) && defined(EFFECTS11)
	if (SharedData::enbSettings.Enable) {
		float3 cloudColor = psout.Color.xyz;
		float3 viewDirection = normalize(input.WorldPosition.xyz);

		cloudColor.xyz = lerp(abs(cloudColor.xyz), dot(cloudColor.xyz, 1.0 / 3.0), SharedData::enbSettings.CloudsDesaturation);

		float cloudLuminance = dot(cloudColor.xyz, 1.0 / 3.0);

		float sunLighting = saturate(dot(viewDirection, SharedData::SunDirection.xyz) * 0.5 + 0.5);
		float masserLighting = saturate(dot(viewDirection, SharedData::MasserDirection.xyz) * 0.5 + 0.5);
		float secundaLighting = saturate(dot(viewDirection, SharedData::SecundaDirection.xyz) * 0.5 + 0.5);

		if (SharedData::enbSettings.CloudsEdgeIntensity > 0.0) {
			float cloudsEdgeAlpha = saturate(1.0 - baseColor.w);

			float3 sunPhase = pow(sunLighting, 32.0) * SharedData::SunColor.xyz * cloudsEdgeAlpha;
			float3 masserPhase = pow(masserLighting, 32.0) * SharedData::MasserColor.xyz * SharedData::enbSettings.CloudsEdgeMoonMultiplier * cloudsEdgeAlpha;
			float3 secundaPhase = pow(secundaLighting, 32.0) * SharedData::SecundaColor.xyz * SharedData::enbSettings.CloudsEdgeMoonMultiplier * cloudsEdgeAlpha;

			float3 cloudsScatter = (sunPhase + masserPhase + secundaPhase) * SharedData::enbSettings.CloudsEdgeIntensity;

			cloudColor += cloudLuminance * cloudsScatter;
		}

		psout.Color.xyz = cloudColor;
		psout.Color.w = saturate(psout.Color.w);
	}
#			endif
#		endif

#	else
	psout.Color = float4(0, 0, 0, 1.0);
#	endif  // OCCLUSION

#	if defined(EXP_HEIGHT_FOG)
	const bool inReflection = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection) != 0;
	// Reflected skies follow the same weather-fog exclusion as the main-view composite.
	if (inReflection && SharedData::exponentialHeightFogSettings.enabled && SharedData::exponentialHeightFogSettings.useVanillaFogSettings == 0) {
		float3 skyFogPosition = normalize(input.FogPosition.xyz) * SharedData::CameraData.x;
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFogNoVolumetric(skyFogPosition, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, psout.Color.xyz, float4(input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy, input.Position.z, 1));
		psout.Color.xyz = lerp(psout.Color.xyz, exponentialHeightFog.xyz, exponentialHeightFog.w);
	}
#	endif

	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, eyeIndex);

	psout.MotionVectors = float4(screenMotionVector, 0, psout.Color.w);
	psout.Normal = float4(0.5, 0.5, 0, psout.Color.w);

#	if defined(CLOUD_SHADOWS) && defined(CLOUDS) && !defined(DEFERRED)
	psout.CloudShadows = float4(1.0, 1.0, 1.0, psout.Color.w);

	// Keep sun behind scene depth to prevent halo leaks through geometry.
	float depth = TexDepthSampler.Load(int3(input.Position.xy, 0));
	if (depth < input.Position.z)
		psout.Color.w = 0;

#	else
	// Even without cloud shadows enabled, sun disc should be occluded by scene depth (clouds, terrain, etc.)
	if ((Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun)) {
		float depth = TexDepthSampler.Load(int3(input.Position.xy, 0));
		if (depth < input.Position.z)
			psout.Color.w = 0;
	}
#	endif

	return psout;
}
#endif
