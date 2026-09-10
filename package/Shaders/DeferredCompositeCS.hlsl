
#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "Common/VR.hlsli"
#include "Common/WindField.hlsli"

Texture2D<float3> SpecularTexture : register(t0);
Texture2D<unorm float3> AlbedoTexture : register(t1);
Texture2D<unorm float3> NormalRoughnessTexture : register(t2);
Texture2D<float3> MasksTexture : register(t3);
Texture2D<unorm float> Masks2Texture : register(t9);
Texture2D<float4> GrassWindSpringDebug : register(t18);

RWTexture2D<float4> MainRW : register(u0);
RWTexture2D<float4> NormalTAAMaskSpecularMaskRW : register(u1);
RWTexture2D<float2> MotionVectorsRW : register(u2);

// 24/32-bit depth: TerrainBlending ON -> R32_FLOAT (no unorm),
// OFF -> R24_UNORM_X8_TYPELESS game depth (unorm).
#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t4);
#else
Texture2D<unorm float> DepthTexture : register(t4);
#endif

#if defined(DYNAMIC_CUBEMAPS)
Texture2D<float3> ReflectanceTexture : register(t5);
TextureCube<float3> EnvTexture : register(t6);
TextureCube<float3> EnvReflectionsTexture : register(t7);

SamplerState LinearSampler : register(s0);
#endif

#if defined(SKYLIGHTING)
#	define SKYLIGHTING_PROBE_REGISTER t8
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(SSGI)
Texture2D<float4> SsgiAoTexture : register(t10);
#	if !defined(SSGI_AO_ONLY)
Texture2D<float4> SsgiYTexture : register(t11);
Texture2D<float4> SsgiCoCgTexture : register(t12);
Texture2D<float4> SsgiSpecularTexture : register(t13);
#	endif

void SampleSSGI(uint2 pixCoord, float3 normalWS, out float ao, out float3 il)
{
	ao = 1 - SsgiAoTexture[pixCoord].x;
#	if defined(SSGI_AO_ONLY)
	il = 0;
#	else
	float4 ssgiIlYSh = SsgiYTexture[pixCoord];
	// without ZH hallucination
	// float ssgiIlY = SphericalHarmonics::FuncProductIntegral(ssgiIlYSh, SphericalHarmonics::EvaluateCosineLobe(normalWS));
	float ssgiIlY = SphericalHarmonics::SHHallucinateZH3Irradiance(ssgiIlYSh, normalWS);
	float2 ssgiIlCoCg = SsgiCoCgTexture[pixCoord].xy;
	il = max(0, Color::YCoCgToRGB(float3(ssgiIlY, ssgiIlCoCg)));
#	endif
}

void SampleSSGISpecular(uint2 pixCoord, sh2 lobe, inout float ao, out float3 il, in float3 normal, in float3 view, in float roughness)
{
	float NdotV = dot(normal, view);
	float alpha = roughness * roughness;
	ao = SpecularOcclusion(saturate(NdotV), alpha, ao);

#	if defined(SSGI_AO_ONLY)
	il = 0;
#	else
	float4 ssgiIlYSh = SsgiYTexture[pixCoord];
	float ssgiIlY = SphericalHarmonics::FuncProductIntegral(ssgiIlYSh, lobe);
	float2 ssgiIlCoCg = SsgiCoCgTexture[pixCoord].xy;

	// pi to compensate for the /pi in specularLobe
	// i don't think there really should be a 1/PI but without it the specular is too strong
	// reflectance being ambient reflectance doesn't help either
	il = max(0, Color::YCoCgToRGB(float3(ssgiIlY, ssgiIlCoCg / Math::PI)));

	// HQ spec
	float4 hq_spec = SsgiSpecularTexture[pixCoord];
	ao *= 1 - hq_spec.a;
	il += hq_spec.rgb;
#	endif
}
#endif

#if defined(IBL)
#	if !defined(DYNAMIC_CUBEMAPS)
#		undef IBL
#	else
#		define IBL_DEFERRED
#		include "IBL/IBL.hlsli"
#	endif
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside screen bounds
	if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;  // adjust for dynamic res

	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

	uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	float3 normalGlossiness = NormalRoughnessTexture[dispatchID.xy];
	float3 normalVS = GBuffer::DecodeNormal(normalGlossiness.xy);

	float3 diffuseColor = MainRW[dispatchID.xy].xyz;
	float3 specularColor = SpecularTexture[dispatchID.xy];
	float3 albedo = AlbedoTexture[dispatchID.xy];

	float depth = DepthTexture[dispatchID.xy];
	float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	positionWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionWS);
	positionWS.xyz = positionWS.xyz / positionWS.w;

	if (depth == 1.0)
		MotionVectorsRW[dispatchID.xy] = MotionBlur::GetSSMotionVector(positionWS, positionWS, eyeIndex);  // Apply sky motion vectors

	float glossiness = normalGlossiness.z;

	float3 linDiffuseColor = Color::IrradianceToLinear(diffuseColor);
	float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0)).xyz);

#if defined(SSGI)

	float ssgiAo;
	float3 ssgiIl;
	SampleSSGI(dispatchID.xy, normalWS, ssgiAo, ssgiIl);

	// Masks2.x stores 1 - vertexAO (Lighting.hlsl only); cleared to 0 for
	// pixels with no vertex AO contribution, so vertexAO defaults to 1.
	float vertexAO = 1.0 - Masks2Texture[dispatchID.xy].x;
	ssgiAo = saturate(ssgiAo / max(vertexAO, EPSILON_DIVISION));

	float3 linAlbedo = Color::IrradianceToLinear(albedo / Color::PBRLightingScale);
	float3 multiBounceSSGIAo = MultiBounceAO(linAlbedo, ssgiAo);

	float3 directionalAmbientColor = 0;

#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		float3 vanillaDALC = Color::Ambient(max(0, SharedData::GetAmbient(normalWS)));

#		if defined(SKYLIGHTING)
#			if defined(VR)
		float3 positionMS = positionWS.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#			else
		float3 positionMS = positionWS.xyz;
#			endif
		sh2 skylightingSH = Skylighting::Sample(positionMS.xyz, normalWS);
		float skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, normalWS);
		directionalAmbientColor = ImageBasedLighting::GetDiffuseIBLOccluded(vanillaDALC, -normalWS, skylightingDiffuse) * albedo;
#		else
		directionalAmbientColor = ImageBasedLighting::GetDiffuseIBL(vanillaDALC, -normalWS) * albedo;
#		endif

		directionalAmbientColor = Color::RGBToYCoCg(directionalAmbientColor);
		directionalAmbientColor.x = MasksTexture[dispatchID.xy].z;
		directionalAmbientColor = Color::YCoCgToRGB(directionalAmbientColor);
		directionalAmbientColor = max(0, directionalAmbientColor);
	} else
#	endif
	{
		directionalAmbientColor = Color::Ambient(max(0, SharedData::GetAmbient(normalWS)));
		directionalAmbientColor *= albedo;

		directionalAmbientColor = Color::RGBToYCoCg(directionalAmbientColor);
		directionalAmbientColor.x = MasksTexture[dispatchID.xy].z;
		directionalAmbientColor = Color::YCoCgToRGB(directionalAmbientColor);
		directionalAmbientColor = max(0, directionalAmbientColor);
	}

	{
		float maxScale = 1.0;
		if (directionalAmbientColor.x > 0.0)
			maxScale = min(maxScale, diffuseColor.x / directionalAmbientColor.x);
		if (directionalAmbientColor.y > 0.0)
			maxScale = min(maxScale, diffuseColor.y / directionalAmbientColor.y);
		if (directionalAmbientColor.z > 0.0)
			maxScale = min(maxScale, diffuseColor.z / directionalAmbientColor.z);
		directionalAmbientColor *= maxScale;

		diffuseColor = max(0.0, diffuseColor - directionalAmbientColor);
		linDiffuseColor = Color::IrradianceToLinear(diffuseColor);
		linDiffuseColor *= sqrt(multiBounceSSGIAo);
		diffuseColor = Color::IrradianceToGamma(linDiffuseColor);
		diffuseColor += Color::IrradianceToGamma(Color::IrradianceToLinear(directionalAmbientColor) * multiBounceSSGIAo);
		linDiffuseColor = Color::IrradianceToLinear(diffuseColor);
	}

	linDiffuseColor += ssgiIl * linAlbedo;
#endif

	float3 color = linDiffuseColor + specularColor;

#if defined(DYNAMIC_CUBEMAPS)

	float3 reflectance = ReflectanceTexture[dispatchID.xy];

	if (any(reflectance > 0.0)) {
		float3 V = -normalize(positionWS.xyz);
		float3 R = reflect(-V, normalWS);

		float roughness = 1.0 - glossiness;
		float level = roughness * 8.0;

		sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normalWS, V, roughness);

		float3 finalIrradiance = 0;

		float directionalAmbientColorSpecular = Color::RGBToLuminance(Color::Ambient(max(0, SharedData::GetAmbient(R)))) * Color::ReflectionNormalisationScale;
#	if defined(IBL) || defined(SKYLIGHTING)
		float skylightingSpecular = 1.0;
		float skylightingVisibility = 1.0;
#	endif

#	if defined(SKYLIGHTING)
#		if defined(VR)
		float3 positionMS = positionWS.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#		else
		float3 positionMS = positionWS.xyz;
#		endif

		sh2 skylightingSH = Skylighting::Sample(positionMS.xyz, R);
		skylightingSpecular = Skylighting::EvaluateSpecular(skylightingSH, specularLobe);
		skylightingVisibility = Skylighting::EvaluateVisibility(skylightingSH);
#	endif

#	if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
			float3 envSpecular;
			float3 skySpecular;
			ImageBasedLighting::ComputeSpecularIBL(
				EnvTexture,
				EnvReflectionsTexture,
				LinearSampler,
				R,
				level,
				directionalAmbientColorSpecular,
				skylightingSpecular,
				skylightingVisibility,
				envSpecular,
				skySpecular);

			finalIrradiance = envSpecular + skySpecular;
		} else
#	endif
		{
			// Fallback without IBL: normalize-by-luminance with DALC
#	if defined(INTERIOR)
			float3 specularIrradiance = EnvTexture.SampleLevel(LinearSampler, R, level);
			float specularIrradianceLuminance = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, R, 15));
			specularIrradiance = (specularIrradiance / max(specularIrradianceLuminance, 0.001)) * directionalAmbientColorSpecular;
			finalIrradiance = Color::IrradianceToLinear(specularIrradiance);
#	elif defined(SKYLIGHTING)
			float3 specularIrradianceReflections = 0.0;
			if (skylightingSpecular > 0.0) {
				specularIrradianceReflections = EnvReflectionsTexture.SampleLevel(LinearSampler, R, level);
				float lum = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, R, 15));
				specularIrradianceReflections = (specularIrradianceReflections / max(lum, 0.001)) * directionalAmbientColorSpecular;
				specularIrradianceReflections = Color::IrradianceToLinear(specularIrradianceReflections);
			}
			float3 specularIrradiance = 0.0;
			if (skylightingSpecular < 1.0) {
				specularIrradiance = EnvTexture.SampleLevel(LinearSampler, R, level);
				float lum = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, R, 15));
				float dalcScaled = Color::IrradianceToGamma(Color::IrradianceToLinear(directionalAmbientColorSpecular) * skylightingSpecular);
				specularIrradiance = (specularIrradiance / max(lum, 0.001)) * dalcScaled;
				specularIrradiance = Color::IrradianceToLinear(specularIrradiance);
			}
			finalIrradiance = lerp(specularIrradiance, specularIrradianceReflections, skylightingSpecular);
#	else
			float3 specularIrradiance = EnvReflectionsTexture.SampleLevel(LinearSampler, R, level);
			float specularIrradianceLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, R, 15));
			specularIrradiance = (specularIrradiance / max(specularIrradianceLuminance, 0.001)) * directionalAmbientColorSpecular;
			finalIrradiance = Color::IrradianceToLinear(specularIrradiance);
#	endif
		}

#	if defined(SSGI)
		float3 ssgiIlSpecular;
		SampleSSGISpecular(dispatchID.xy, specularLobe, ssgiAo, ssgiIlSpecular, normalWS, V, roughness);

		finalIrradiance = (finalIrradiance * ssgiAo);

		ssgiIlSpecular = Color::RGBToYCoCg(ssgiIlSpecular);
		if (ssgiIlSpecular.x > 0.0) {
			ssgiIlSpecular = max(0, Color::YCoCgToRGB(float3(ssgiIlSpecular.x, lerp(ssgiIlSpecular.yz, Color::RGBToYCoCg(finalIrradiance).yz, 0.5))));
		} else {
			ssgiIlSpecular = 0;
		}

		finalIrradiance += ssgiIlSpecular;
#	endif

		color += reflectance * finalIrradiance;
	}

#endif

	color = Color::IrradianceToGamma(color);

#if defined(DEBUG)

#	if defined(VR)
	uv.x += (eyeIndex ? 0.1 : -0.1);
#	endif  // VR

	if (uv.x < 0.5 && uv.y < 0.5) {
		color = color;
	} else if (uv.x < 0.5) {
		color = albedo;
	} else if (uv.y < 0.5) {
		color = normalVS;
	} else {
		color = glossiness;
	}

#endif

	[branch] if (SharedData::windSettings.windFieldDebugEnabled != 0u && depth < 1.0f)
	{
		float3 worldPosition = positionWS.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
		uint debugView = SharedData::windSettings.windFieldDebugView;
		if (debugView == 5u) {
			color = 0.0f;
			if (SharedData::WindFieldSpringDebug.z > 0.0f) {
				float2 springUV = (worldPosition.xy - SharedData::WindFieldSpringDebug.xy) /
				                  SharedData::WindFieldSpringDebug.z;
				if (all(springUV >= 0.0f) && all(springUV <= 1.0f)) {
					uint springWidth, springHeight;
					GrassWindSpringDebug.GetDimensions(springWidth, springHeight);
					uint2 springCell = min(uint2(springUV * float2(springWidth, springHeight)),
						uint2(springWidth - 1u, springHeight - 1u));
					float3 springResponse = GrassWindSpringDebug.Load(int3(springCell, 0)).xyz;
					float springMagnitude = saturate(length(springResponse.xy) /
													 max(SharedData::WindFieldSpringDebug.w, EPSILON_WIND_GEOMETRY));
					color = lerp(Color::TurboColormap(springMagnitude), float3(1.0f, 1.0f, 1.0f), springResponse.z * 0.25f);
				}
			}
		} else if (debugView == 6u) {
			WindField::TransientImpulseSample impulseSample = WindField::SampleCurrentTransientImpulses(worldPosition);
			float intensity = saturate(impulseSample.intensity);
			color = Color::TurboColormap(intensity) * intensity;
		} else {
			WindField::WindSample currentSample = WindField::SampleField(
				worldPosition, SharedData::WindFieldCurrent, SharedData::WindFieldTuning);
			WindField::WindSample previousSample = currentSample;
			if (debugView >= 2u && SharedData::WindFieldTransitionData.x < 1.0f)
				previousSample = WindField::SampleField(
					worldPosition, SharedData::WindFieldTransition, SharedData::WindFieldTuning);
			WindField::WindSample displayedSample = WindField::SampleCurrent(worldPosition, 1.0f, 1.0f);
			if (debugView == 1u)
				displayedSample = currentSample;
			else if (debugView == 2u)
				displayedSample = previousSample;
			else if (debugView == 3u) {
				if (((dispatchID.x / 16u + dispatchID.y / 16u) & 1u) != 0u)
					displayedSample = previousSample;
			} else if (debugView == 4u) {
				if (dispatchID.x * 2u < uint(SharedData::BufferDim.x))
					displayedSample = previousSample;
			}
			float ambientPressure = saturate((displayedSample.ambientGust - 0.5f) * 2.0f);
			color = Color::TurboColormap(max(ambientPressure, saturate(displayedSample.transientImpulse)));
		}
	}

	MainRW[dispatchID.xy] = float4(color, 1.0);
	NormalTAAMaskSpecularMaskRW[dispatchID.xy] = float4(GBuffer::EncodeNormalVanilla(normalVS), 0.0, 0.0);
}
