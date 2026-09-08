// VR Stereo Optimizations - Unrepairable Mask Compute Shader
//
// Marks culled Eye 1 pixels that received no Eye 0 depth, so next frame's classification
// shades that strip natively. Dispatched over the Eye 1 half width like DepthScatterCS.

#include "VRStereoOptimizations/cbuffers.hlsli"

Texture2D<uint> ModeTexture : register(t0);   // per-pixel classification (full SBS)
Texture2D<uint> ScatterDepth : register(t1);  // warped Eye 0 depth (Eye 1 half width)
RWTexture2D<uint> MaskRW : register(u0);      // 1 = culled with nothing to repair (Eye 1 half width)

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	const uint eyeWidth = uint(FrameDim.x) / 2;
	if (dtid.x >= eyeWidth || dtid.y >= uint(FrameDim.y))
		return;

	uint2 sbs = dtid + uint2(eyeWidth, 0);
	MaskRW[dtid] = (ModeTexture[sbs] == MODE_MAIN && ScatterDepth[dtid] == SCATTER_DEPTH_EMPTY) ? 1u : 0u;
}
