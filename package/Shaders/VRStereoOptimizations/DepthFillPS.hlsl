// VR Stereo Optimizations - Depth Fill Pixel Shader
//
// Stencil-culled Eye 1 pixels never get geometry depth written during the deferred
// pass (the stencil test kills the whole fragment, depth write included), so later
// depth-tested passes (water, sky, transparency) see holes and draw through geometry.
// This pass restores depth for exactly those pixels: the DSS stencil test (EQUAL,
// ref=1) hardware-masks the fullscreen triangle to culled pixels, and SV_Depth
// writes the depth the classification CS used when it marked the pixel reprojectable,
// or the nearer depth DepthScatterCS warped over from Eye 0's final geometry when the
// z-prepass the classification used had omitted that geometry.

#include "VRStereoOptimizations/cbuffers.hlsli"

Texture2D<float> SceneDepthTexture : register(t0);  // classification depth source (full SBS)
Texture2D<uint> ScatterTexture : register(t1);      // asuint(nearest Eye 1 depth) per Eye 1 pixel (Eye 1 half width)

struct PS_INPUT
{
	float4 Position: SV_Position;
	float2 TexCoord: TEXCOORD0;
};

// Never add [earlydepthstencil] here: forced early-Z writes the rasterized triangle depth
// (0 = unrendered) instead of SV_Depth, wiping the culled pixels' depth.
float main(PS_INPUT input) : SV_Depth
{
	// Depth source is full SBS resolution - SV_Position maps directly
	// (viewport is the Eye 1 half, so Position.x starts at eyeWidth).
	int2 px = int2(input.Position.xy);
	float depth = SceneDepthTexture[px];

	if (!RepairFromEye0Depth)
		return depth;

	const uint eyeWidth = uint(FrameDim.x) / 2;
	uint bits = ScatterTexture[uint2(px.x - eyeWidth, px.y)];
	if (bits == SCATTER_DEPTH_EMPTY)
		return depth;

	// Below the classifier's own threshold the difference is the scatter's texel sampling, not
	// missing geometry; taking it would put a per-texel sawtooth on surfaces the prepass has right.
	float warped = asfloat(bits);
	return (depth - warped) > DisocclusionThreshold * depth ? warped : depth;
}
