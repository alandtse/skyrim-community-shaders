///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion",
// https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
//
// Implementation:  Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>         (\_/)
// Version:         (see XeGTAO.h)                                                                            (='.'=)
// Details:         https://github.com/GameTechDev/XeGTAO                                                     (")_(")
//
// Version history: see XeGTAO.h
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "../Common/Color.hlsl"

#ifndef __INTELLISENSE__  // avoids some pesky intellisense errors
#	include "XeGTAO.h"
#endif

cbuffer GTAOConstantBuffer : register(b0)
{
	GTAOConstants g_GTAOConsts;
}

#include "XeGTAO.hlsli"

RWTexture2D<uint> g_outHilbertLUT : register(u0);

// input output textures for XeGTAO_PrefilterDepths16x16
Texture2D<float> g_srcRawDepth : register(t0);              // source depth buffer data (in NDC space in DirectX)
RWTexture2D<lpfloat> g_outWorkingDepthMIP0 : register(u0);  // output viewspace depth MIP (these are views into g_srcWorkingDepth MIP levels)
RWTexture2D<lpfloat> g_outWorkingDepthMIP1 : register(u1);  // output viewspace depth MIP (these are views into g_srcWorkingDepth MIP levels)
RWTexture2D<lpfloat> g_outWorkingDepthMIP2 : register(u2);  // output viewspace depth MIP (these are views into g_srcWorkingDepth MIP levels)
RWTexture2D<lpfloat> g_outWorkingDepthMIP3 : register(u3);  // output viewspace depth MIP (these are views into g_srcWorkingDepth MIP levels)
RWTexture2D<lpfloat> g_outWorkingDepthMIP4 : register(u4);  // output viewspace depth MIP (these are views into g_srcWorkingDepth MIP levels)

// input output textures for XeGTAO_FetchRadiance
Texture2D<float4> g_srcDiffuse : register(t0);
Texture2D<float4> g_srcAmbient : register(t1);
Texture2D<float4> g_srcPrevRadiance : register(t2);
Texture2D<float4> g_srcMotionVec : register(t3);
RWTexture2D<float4> g_outRadiance : register(u0);

// input output textures for XeGTAO_MainPass
Texture2D<lpfloat> g_srcWorkingDepth : register(t0);  // viewspace depth with MIPs, output by XeGTAO_PrefilterDepths16x16 and consumed by XeGTAO_MainPass
Texture2D<float4> g_srcNormalmap : register(t1);      // source normal map (if used)
Texture2D<uint> g_srcHilbertLUT : register(t2);       // hilbert lookup table  (if any)
Texture2D<float4> g_srcAlbedo : register(t3);
Texture2D<float4> g_srcRadiance : register(t4);
RWTexture2D<float4> g_outWorkingAOTerm : register(u0);      // output AO term (includes bent normals if enabled - packed as R11G11B10 scaled by AO)
RWTexture2D<unorm float> g_outWorkingEdges : register(u1);  // output depth-based edges used by the denoiser

// input output textures for XeGTAO_Denoise
Texture2D<float4> g_srcWorkingAOTerm : register(t0);  // coming from previous pass
Texture2D<lpfloat> g_srcWorkingEdges : register(t1);  // coming from previous pass
RWTexture2D<float4> g_outFinalAOTerm : register(u0);  // final AO term - just 'visibility' or 'visibility + bent normals'

// input output textures for XeGTAO_Mix
Texture2D<float4> g_srcColor : register(t0);
Texture2D<float4> g_srcGI : register(t1);
Texture2D<float4> g_srcAmbientMix : register(t2);
RWTexture2D<float4> g_outColor : register(u0);

SamplerState g_samplerPointClamp : register(s0);
SamplerState g_samplerLinearClamp : register(s1);

// Engine-specific screen & temporal noise loader
lpfloat2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)  // without TAA, temporalIndex is always 0
{
	float2 noise;
	uint index = g_srcHilbertLUT.Load(uint3(pixCoord % 64, 0)).x;
	index += 288 * (temporalIndex % 64);  // why 288? tried out a few and that's the best so far (with XE_HILBERT_LEVEL 6U) - but there's probably better :)
	// R2 sequence - see http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
	return lpfloat2(frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114)));
}

[numthreads(32, 32, 1)] void CSGenerateHibertLUT(uint3 tid
												 : SV_DispatchThreadID) {
	g_outHilbertLUT[tid.xy] = HilbertIndex(tid.x, tid.y);
}

	// Engine-specific entry point for the first pass

	[numthreads(8, 8, 1)]  // <- hard coded to 8x8; each thread computes 2x2 blocks so processing 16x16 block: Dispatch needs to be called with (width + 16-1) / 16, (height + 16-1) / 16
	void CSPrefilterDepths16x16(uint2 dispatchThreadID
								: SV_DispatchThreadID, uint2 groupThreadID
								: SV_GroupThreadID)
{
	XeGTAO_PrefilterDepths16x16(dispatchThreadID, groupThreadID, g_GTAOConsts, g_srcRawDepth, g_samplerPointClamp, g_outWorkingDepthMIP0, g_outWorkingDepthMIP1, g_outWorkingDepthMIP2, g_outWorkingDepthMIP3, g_outWorkingDepthMIP4);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)] void CSFetchRadiance(const uint2 pixCoord
																				 : SV_DispatchThreadID) {
	float2 uv = (pixCoord + .5) * g_GTAOConsts.ViewportPixelSize;
	float2 prev_uv = uv + g_srcMotionVec[pixCoord].xy;

	float3 radiance = g_srcDiffuse[pixCoord].rgb;
	radiance -= g_srcAmbient[pixCoord].rgb * (1 - g_GTAOConsts.AmbientSource);
	radiance += g_srcPrevRadiance.SampleLevel(g_samplerLinearClamp, prev_uv, 0).rgb * g_GTAOConsts.GIBounceFade * g_GTAOConsts.GIStrength * 10.f;
	g_outRadiance[pixCoord] = float4(radiance, 1);
}

	// Engine-specific entry point for the second pass
	[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)] void CSGTAO(const uint2 pixCoord
																			: SV_DispatchThreadID)
{
	// g_samplerPointClamp is a sampler with D3D12_FILTER_MIN_MAG_MIP_POINT filter and D3D12_TEXTURE_ADDRESS_MODE_CLAMP addressing mode
	XeGTAO_MainPass(pixCoord,
		g_GTAOConsts.SliceCount, g_GTAOConsts.StepsPerSlice, SpatioTemporalNoise(pixCoord, g_GTAOConsts.NoiseIndex),
		UnpackNormal(g_srcNormalmap[pixCoord].xy), g_srcAlbedo[pixCoord].rgb,
		g_GTAOConsts,
		g_srcWorkingDepth, g_srcNormalmap, g_srcRadiance, g_srcAlbedo,
		g_samplerPointClamp, g_samplerPointClamp,
		g_outWorkingAOTerm, g_outWorkingEdges);
}

// Engine-specific entry point for the third pass
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)] void CSDenoisePass(const uint2 dispatchThreadID
																			   : SV_DispatchThreadID) {
	const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);  // we're computing 2 horizontal pixels at a time (performance optimization)
	// g_samplerPointClamp is a sampler with D3D12_FILTER_MIN_MAG_MIP_POINT filter and D3D12_TEXTURE_ADDRESS_MODE_CLAMP addressing mode
	XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_samplerPointClamp, g_outFinalAOTerm, false);
}

	[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)] void CSDenoiseLastPass(const uint2 dispatchThreadID
																					   : SV_DispatchThreadID)
{
	const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);  // we're computing 2 horizontal pixels at a time (performance optimization)
	// g_samplerPointClamp is a sampler with D3D12_FILTER_MIN_MAG_MIP_POINT filter and D3D12_TEXTURE_ADDRESS_MODE_CLAMP addressing mode
	XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges, g_samplerPointClamp, g_outFinalAOTerm, true);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)] void CSMix(const uint2 pixCoord
																	   : SV_DispatchThreadID) {
	float4 color = g_srcColor[pixCoord];
	float4 gi = g_srcGI[pixCoord];
	float3 ambient = g_srcAmbientMix[pixCoord].rgb;
	float3 direct = color.rgb - ambient;

	switch (g_GTAOConsts.DebugView) {
	case 1:
		ambient = 1;
		direct = 0;
		gi.rgb = 0;
		break;
	case 2:
		direct = 0;
		ambient = 0;
		gi.a = 0;
		break;
	case 3:
		direct = 0;
		ambient = 0.5;
		break;
	default:
		break;
	}

	gi.rgb = lerp(RGBToLuminance(gi.rgb).rrr, gi.rgb, g_GTAOConsts.GISaturation);

	gi.a = saturate((gi.a - g_GTAOConsts.AOClamp.x) / (g_GTAOConsts.AOClamp.y - g_GTAOConsts.AOClamp.x));
	gi.a = pow(gi.a, g_GTAOConsts.AOPower);
	gi.a = lerp(g_GTAOConsts.AORemap.x, g_GTAOConsts.AORemap.y, gi.a);

	float3 finalColor = max(0, lerp(1, gi.a, g_GTAOConsts.DirectLightAO) * direct + gi.a * ambient + gi.rgb * g_GTAOConsts.GIStrength * 10.f);

	g_outColor[pixCoord] = float4(finalColor, 1);

	// g_outColor[pixCoord] = float4(gi.rgb, 1);
}

///
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////