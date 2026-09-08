// VR Stereo Optimizations - Depth Scatter Compute Shader
//
// Forward-warps Eye 0's final geometry depth into Eye 1, keeping the nearest depth per Eye 1
// pixel, so the depth-fill pass can restore geometry the z-prepass omitted (alpha-tested statics).
// Dispatched over the Eye 0 half only.

#include "Common/Math.hlsli"
#include "Common/VR.hlsli"
#include "Common/VRReproject.hlsli"
#include "VRStereoOptimizations/cbuffers.hlsli"

Texture2D<float> DepthTexture : register(t0);  // scene depth after the World pass (full SBS)
RWTexture2D<uint> ScatterRW : register(u0);    // asuint(nearest Eye 1 depth) per Eye 1 pixel (Eye 1 half width)

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	const uint eyeWidth = uint(FrameDim.x) / 2;
	if (dtid.x >= eyeWidth || dtid.y >= uint(FrameDim.y))
		return;

	float depth = DepthTexture[dtid];
	if (depth < EPSILON_DEPTH_SKY || depth >= DEPTH_UNRENDERED)
		return;

	float2 monoUV = Stereo::ConvertFromStereoUV((float2(dtid) + 0.5) / FrameDim, 0);
	float3 otherEyeUV = Stereo::ConvertMonoUVToOtherEye(float3(monoUV, depth), 0);
	if (FrameBuffer::IsOutsideFrame(otherEyeUV.xy, false))
		return;

	// Raw depth grows with distance, so the uint min of the float bits keeps the nearest surface.
	// Both columns around the sub-texel landing point are written so foreshortening leaves no holes.
	static const int kSplatColumns = 2;
	float x = otherEyeUV.x * eyeWidth;
	int x0 = int(floor(x - 0.5));
	uint y = clamp(uint(otherEyeUV.y * FrameDim.y), 0u, uint(FrameDim.y) - 1);
	uint bits = asuint(saturate(otherEyeUV.z));
	[unroll] for (int i = 0; i < kSplatColumns; i++)
	{
		int xi = x0 + i;
		if (xi >= 0 && xi < (int)eyeWidth)
			InterlockedMin(ScatterRW[uint2(xi, y)], bits);
	}
}
