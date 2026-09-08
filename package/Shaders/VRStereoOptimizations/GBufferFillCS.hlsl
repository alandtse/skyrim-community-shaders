// VR Stereo Optimizations - G-Buffer Fill Compute Shader
//
// Stencil-culled Eye 1 pixels never get G-buffer data written during the deferred
// geometry pass. Instead of patching every downstream consumer (SSGI, composite,
// SSS, screen-space shadows...), this pass materializes valid Eye 1 data exactly
// once: each culled pixel reprojects to Eye 0 and copies all G-buffer channels.
// Downstream passes then run unmodified and light Eye 1 natively — view-dependent
// shading (specular, reflections) is computed for Eye 1's real view direction,
// only the material inputs are shared between eyes.
//
// View-space normals copy directly: HMD eye matrices differ by translation only,
// so the view-space rotation is identical between eyes.
//
// Reads Eye 0 texels and writes Eye 1 texels of the SAME textures via UAV — no
// copies needed; the halves never overlap so there is no intra-dispatch hazard.
// Requires typed UAV load support for the G-buffer formats (TypedUAVLoadAdditionalFormats).
//
// Dispatched over the Eye 1 half only (FrameDim.x/2 x FrameDim.y).

#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"
#include "Common/VRReproject.hlsli"
#include "VRStereoOptimizations/cbuffers.hlsli"
#include "VRStereoOptimizations/modes.hlsli"

Texture2D<float> DepthTexture : register(t0);  // scene depth after the depth-fill pass (full SBS)
Texture2D<uint> ModeTexture : register(t1);    // per-pixel classification (full SBS)

RWTexture2D<float4> MainRW : register(u0);                   // diffuse light accumulation (R16G16B16A16F)
RWTexture2D<float2> MotionRW : register(u1);                 // motion vectors (R16G16F)
RWTexture2D<unorm float4> NormalRoughnessRW : register(u2);  // R10G10B10A2: xy octahedral normal, z glossiness, w stochastic dither selector
RWTexture2D<unorm float4> AlbedoRW : register(u3);           // R10G10B10A2
RWTexture2D<float3> SpecularRW : register(u4);               // R11G11B10
RWTexture2D<float3> ReflectanceRW : register(u5);            // R11G11B10
RWTexture2D<float3> MasksRW : register(u6);                  // R11G11B10
RWTexture2D<unorm float> Masks2RW : register(u7);            // R16_UNORM

static const int kEpipolarSearchRadius = 4;

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	const uint eyeWidth = uint(FrameDim.x) / 2;
	if (dtid.x >= eyeWidth || dtid.y >= uint(FrameDim.y))
		return;

	// This thread covers one Eye 1 pixel
	uint2 px = uint2(dtid.x + eyeWidth, dtid.y);

	if (ModeTexture[px] != MODE_MAIN)
		return;

	float depth = DepthTexture[px];
	float2 uv = (float2(px) + 0.5) / FrameDim;

	Stereo::StereoBilateralResult r = Stereo::ReprojectToOtherEye(uv, depth, 1, FrameDim);
	if (!r.valid)
		return;  // classification marks invalid reprojection MODE_DISOCCLUDED, so this is rare

	float2 samplePos = r.otherStereoUV * FrameDim - 0.5;
	int2 base = int2(floor(samplePos));
	float2 frac2 = frac(samplePos);
	const int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
	const float bilinear[4] = { (1 - frac2.x) * (1 - frac2.y), frac2.x * (1 - frac2.y), (1 - frac2.x) * frac2.y, frac2.x * frac2.y };

	uint2 idx[4];
	float weight[4];
	float weightSum = 0.0;
	uint2 nearest = uint2(r.otherPx);
	float bestBilinear = -1.0;
	[unroll] for (uint i = 0; i < 4; i++)
	{
		idx[i] = Stereo::ClampToEyeBounds(base + offsets[i], 0, FrameDim);

		float otherDepth = DepthTexture[idx[i]];
		float maxRaw = max(max(otherDepth, depth), EPSILON_DIVISION);
		float relDiff = abs(otherDepth - depth) / maxRaw;
		// Must stay the same relative-depth test StencilCS.hlsl uses to classify this
		// pixel as reprojectable; a divergent threshold desyncs fill from classification.
		weight[i] = (relDiff <= DisocclusionThreshold) ? bilinear[i] : 0.0;
		weightSum += weight[i];

		if (weight[i] > 0.0 && bilinear[i] > bestBilinear) {
			bestBilinear = bilinear[i];
			nearest = idx[i];
		}
	}

	if (weightSum <= EPSILON_DIVISION) {
		// The eyes differ only by a horizontal baseline, so a missed match can only lie on this row.
		uint2 fallback = nearest;
		float fallbackDepth = DepthTexture[nearest];
		bool fallbackAgrees = false;
		[unroll] for (int dx = -kEpipolarSearchRadius; dx <= kEpipolarSearchRadius; dx++)
		{
			uint2 candidate = Stereo::ClampToEyeBounds(int2(base.x + dx, r.otherPx.y), 0, FrameDim);
			float candidateDepth = DepthTexture[candidate];
			float maxRaw = max(max(candidateDepth, depth), EPSILON_DIVISION);
			bool agrees = (abs(candidateDepth - depth) / maxRaw) <= DisocclusionThreshold;
			bool better = agrees && (!fallbackAgrees || candidateDepth > fallbackDepth);
			if (better) {
				fallback = candidate;
				fallbackDepth = candidateDepth;
				fallbackAgrees = agrees;
			}
		}
		nearest = fallback;
	}

	// Motion and the dither selector have no meaningful average; both take the single
	// accepted tap.
	MotionRW[px] = MotionRW[nearest];
	float stochasticSelector = NormalRoughnessRW[nearest].w;

	if (weightSum <= EPSILON_DIVISION) {
		MainRW[px] = MainRW[nearest];
		NormalRoughnessRW[px] = NormalRoughnessRW[nearest];
		AlbedoRW[px] = AlbedoRW[nearest];
		SpecularRW[px] = SpecularRW[nearest];
		ReflectanceRW[px] = ReflectanceRW[nearest];
		MasksRW[px] = MasksRW[nearest];
		Masks2RW[px] = Masks2RW[nearest];
		return;
	}

	float4 mainSum = 0;
	float4 albedoSum = 0;
	float3 normalSum = 0;
	float3 specularSum = 0;
	float3 reflectanceSum = 0;
	float3 masksSum = 0;
	float glossSum = 0;
	float masks2Sum = 0;

	[unroll] for (uint j = 0; j < 4; j++)
	{
		if (weight[j] <= 0.0)
			continue;

		mainSum += MainRW[idx[j]] * weight[j];
		albedoSum += AlbedoRW[idx[j]] * weight[j];
		specularSum += SpecularRW[idx[j]] * weight[j];
		reflectanceSum += ReflectanceRW[idx[j]] * weight[j];
		masksSum += MasksRW[idx[j]] * weight[j];
		masks2Sum += Masks2RW[idx[j]] * weight[j];

		float4 normalRoughness = NormalRoughnessRW[idx[j]];
		normalSum += GBuffer::DecodeNormal(normalRoughness.xy) * weight[j];
		glossSum += normalRoughness.z * weight[j];
	}

	MainRW[px] = mainSum / weightSum;
	AlbedoRW[px] = albedoSum / weightSum;
	SpecularRW[px] = specularSum / weightSum;
	ReflectanceRW[px] = reflectanceSum / weightSum;
	MasksRW[px] = masksSum / weightSum;
	Masks2RW[px] = masks2Sum / weightSum;
	float3 averagedNormal = normalSum / weightSum;
	float2 encodedNormal = length(averagedNormal) > EPSILON_DIVISION ? GBuffer::EncodeNormal(normalize(averagedNormal)) : NormalRoughnessRW[nearest].xy;
	NormalRoughnessRW[px] = float4(encodedNormal, glossSum / weightSum, stochasticSelector);
}
