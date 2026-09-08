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

#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"
#include "Common/VRReproject.hlsli"
#include "VRStereoOptimizations/cbuffers.hlsli"
#include "VRStereoOptimizations/modes.hlsli"

Texture2D<float> DepthTexture : register(t0);  // scene depth after the depth-fill pass (full SBS)
Texture2D<uint> ModeTexture : register(t1);    // per-pixel classification (full SBS)

RWTexture2D<float4> MainRW : register(u0);                   // diffuse light accumulation (R16G16B16A16F)
RWTexture2D<float2> MotionRW : register(u1);                 // motion vectors (R16G16F)
RWTexture2D<unorm float4> NormalRoughnessRW : register(u2);  // R10G10B10A2
RWTexture2D<unorm float4> AlbedoRW : register(u3);           // R10G10B10A2
RWTexture2D<float3> SpecularRW : register(u4);               // R11G11B10
RWTexture2D<float3> ReflectanceRW : register(u5);            // R11G11B10
RWTexture2D<float3> MasksRW : register(u6);                  // R11G11B10
RWTexture2D<unorm float> Masks2RW : register(u7);            // R16_UNORM

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

	uint2 src = uint2(r.otherPx);

	MainRW[px] = MainRW[src];
	// Eye 1 motion is culled too, so Eye 0's is the only value available — a close
	// approximation, as the inter-eye delta is just small per-frame disparity change.
	MotionRW[px] = MotionRW[src];
	NormalRoughnessRW[px] = NormalRoughnessRW[src];
	AlbedoRW[px] = AlbedoRW[src];
	SpecularRW[px] = SpecularRW[src];
	ReflectanceRW[px] = ReflectanceRW[src];
	MasksRW[px] = MasksRW[src];
	Masks2RW[px] = Masks2RW[src];
}
