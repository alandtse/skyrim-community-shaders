// VR Stereo Optimizations - Depth Fill Pixel Shader
//
// Stencil-culled Eye 1 pixels never get geometry depth written during the deferred
// pass (the stencil test kills the whole fragment, depth write included), so later
// depth-tested passes (water, sky, transparency) see holes and draw through geometry.
// This pass restores depth for exactly those pixels: the DSS stencil test (EQUAL,
// ref=1) hardware-masks the fullscreen triangle to culled pixels, and SV_Depth
// writes the same depth source the classification CS used when it marked the pixel
// reprojectable (TerrainBlending blended depth or the post-z-prepass copy).

Texture2D<float> SceneDepthTexture : register(t0);

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
	return SceneDepthTexture[int2(input.Position.xy)];
}
