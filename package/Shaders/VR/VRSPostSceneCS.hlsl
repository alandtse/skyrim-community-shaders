// Runs on kMAIN right before tonemap consumes it -- the same pipeline point
// FoveatedRender's own debug tint uses, so both survive the compositor/preview
// step that the swap-chain's desktop mirror buffer does not.
//
// Two independent effects, both driven by the shading-rate tile texture:
// - DebugVisualize: tints by rate (green=1x1, yellow=1x2, orange=2x2, red=4x4).
// - DitherStrength: perturbs color in coarse tiles with a spatial hash to break
//   up the hard-edged block quantization VRS produces, the same way dithering
//   hides color-depth banding.

#include "Common/BlurDither.hlsli"

cbuffer VRSPostSceneCB : register(b0)
{
	uint TileWidth;
	uint TileHeight;
	uint OutputWidth;
	uint OutputHeight;
	uint DebugVisualize;
	float DitherStrength;
	uint Pad0;
	uint Pad1;
};

Texture2D<uint> RateTex : register(t0);
RWTexture2D<float4> MainTex : register(u0);

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	if (tid.x >= OutputWidth || tid.y >= OutputHeight)
		return;

	uint2 tileCoord = uint2(
		min(tid.x * TileWidth / OutputWidth, TileWidth - 1),
		min(tid.y * TileHeight / OutputHeight, TileHeight - 1));
	uint rate = RateTex.Load(int3(tileCoord, 0));

	if (rate == 0 && DebugVisualize == 0)
		return;

	float4 color = MainTex[tid.xy];

	if (DitherStrength > 0.0f && rate > 0) {
		// Multiplicative (relative to the pixel's own brightness) and capped small --
		// this hides block-edge banding, it isn't meant to be visible as grain.
		// At DitherStrength=1 and the coarsest rate, max perturbation is +/-3%.
		const float kMaxRelativeDither = 0.06f;
		float noise = BlurDither::Hash22(float2(tid.xy)).x - 0.5f;
		float ditherAmount = DitherStrength * (float(rate) / 3.0f) * kMaxRelativeDither;
		color.rgb *= (1.0f + noise * ditherAmount);
	}

	if (DebugVisualize != 0) {
		float3 tint;
		if (rate == 0)
			tint = float3(0.2, 1.0, 0.2);
		else if (rate == 1)
			tint = float3(1.0, 1.0, 0.2);
		else if (rate == 2)
			tint = float3(1.0, 0.6, 0.1);
		else
			tint = float3(1.0, 0.2, 0.2);
		color.rgb = lerp(color.rgb, color.rgb * tint, 0.5);
	}

	MainTex[tid.xy] = color;
}
