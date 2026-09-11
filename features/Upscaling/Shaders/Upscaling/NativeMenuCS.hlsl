Texture2D<float4> Submitted : register(t0);
Texture2D<float4> MenuReference : register(t1);
Texture2D<float4> NativeMenu : register(t2);

#if defined(VALIDATE_MENU)
RWTexture2D<uint> Mismatch : register(u0);
groupshared uint GroupMismatch;

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
	if (groupIndex == 0)
		GroupMismatch = 0;
	GroupMemoryBarrierWithGroupSync();
	uint width, height;
	Submitted.GetDimensions(width, height);
	// Preserve any composition applied after the captured menu draw.
	if (id.x < width && id.y < height && any(Submitted[id.xy] != MenuReference[id.xy]))
		InterlockedOr(GroupMismatch, 1);
	GroupMemoryBarrierWithGroupSync();
	if (groupIndex == 0 && GroupMismatch != 0)
		InterlockedOr(Mismatch[uint2(0, 0)], 1);
}
#else
Texture2D<uint> Mismatch : register(t3);
Texture2D<float4> ReconstructedScene : register(t4);
Texture2D<uint> SceneMismatch : register(t5);
Texture2D<float4> MenuUILayer : register(t6);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint width, height;
	Output.GetDimensions(width, height);
	if (id.x >= width || id.y >= height)
		return;
	float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
	uint sourceWidth, sourceHeight;
	Submitted.GetDimensions(sourceWidth, sourceHeight);
	float eyeOffset = id.x < width / 2 ? 0.0 : 0.5;
	uv.x = clamp(uv.x, eyeOffset + 0.5 / sourceWidth, eyeOffset + 0.5 - 0.5 / sourceWidth);
	float4 background = SceneMismatch[uint2(0, 0)] == 0 ? ReconstructedScene[id.xy] :
	                    Mismatch[uint2(0, 0)] == 0      ? NativeMenu[id.xy] :
	                                                      Submitted.SampleLevel(LinearClamp, uv, 0);
	float4 ui = MenuUILayer[id.xy];
	Output[id.xy] = ui + background * (1.0 - saturate(ui.a));
}
#endif
