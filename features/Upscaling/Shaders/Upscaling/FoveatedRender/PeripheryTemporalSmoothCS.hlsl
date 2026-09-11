// Temporal accumulation filter for the periphery background.
// Runs on render-resolution SBS layout. Blends current frame with reprojected
// history using motion vectors. EMA with motion-adaptive alpha to suppress
// ghosting during movement while keeping strong flicker reduction in the
// non-focal periphery when stationary.

#include "Common/Color.hlsli"

cbuffer TemporalSmoothCB : register(b0)
{
	uint TexWidth;     // SBS width (render-res)
	uint TexHeight;    // SBS height (render-res)
	float BlendAlpha;  // Current-frame weight (0.05 = very smooth, 0.5 = less smooth)
	uint _pad;
};

Texture2D<float4> CurrentTex : register(t0);   // vrRenderSBS snapshot (current frame)
Texture2D<float4> HistoryTex : register(t1);   // Previous frame smoothed (ping-pong read)
Texture2D<float4> MvecTex : register(t2);      // Motion vectors (per-eye UV delta, current→previous)
SamplerState BilinearSampler : register(s0);   // For history reprojection
RWTexture2D<float4> OutputTex : register(u0);  // New history (ping-pong write)

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	if (tid.x >= TexWidth || tid.y >= TexHeight)
		return;

	uint2 pos = tid.xy;
	float4 current = CurrentTex.Load(int3(pos, 0));

	// Motion vector is per-eye UV delta (current → previous), range ≈ ±small.
	// In SBS layout the x axis spans two eyes, so x-component must be halved.
	float2 mv = MvecTex.Load(int3(pos, 0)).xy;
	float2 currentUV = (float2(pos) + 0.5) / float2(TexWidth, TexHeight);
#if defined(SINGLE_EYE)
	float2 reprojUV = currentUV + mv;
	float eyeMinX = 0.0;
	float eyeMaxX = 1.0;
#else
	float2 reprojUV = currentUV + mv * float2(0.5, 1.0);

	// ── SBS eye-boundary clamp ──
	// Prevent reprojection from crossing into the other eye's half.
	// Left eye: x ∈ [0, 0.5), right eye: x ∈ [0.5, 1.0).
	float halfW = 0.5;
	float eyeMinX = (currentUV.x < halfW) ? 0.0 : halfW;
	float eyeMaxX = eyeMinX + halfW;
#endif
	float texelHalfX = 0.5 / (float)TexWidth;
	float texelHalfY = 0.5 / (float)TexHeight;
	reprojUV.x = clamp(reprojUV.x, eyeMinX + texelHalfX, eyeMaxX - texelHalfX);
	reprojUV.y = clamp(reprojUV.y, texelHalfY, 1.0 - texelHalfY);

	// Bilinear sample history at reprojected position
	float4 history = HistoryTex.SampleLevel(BilinearSampler, reprojUV, 0);

	// ── Disocclusion rejection: clamp history into the current-frame neighborhood ──
	// Local motion alone can't detect disocclusion: a moving NPC leaves near-zero-MV
	// background behind it, so reprojected history can be a stale, unrelated surface.
	// Clamp it into a 5-tap YCoCg box from the current frame before blending.
#if defined(SINGLE_EYE)
	uint eyeMinPx = 0;
	uint eyeMaxPx = TexWidth - 1;
#else
	uint halfWpx = TexWidth / 2;
	uint eyeMinPx = (pos.x < halfWpx) ? 0 : halfWpx;
	uint eyeMaxPx = eyeMinPx + halfWpx - 1;
#endif
	int2 posN = int2(pos.x, clamp((int)pos.y - 1, 0, (int)TexHeight - 1));
	int2 posS = int2(pos.x, clamp((int)pos.y + 1, 0, (int)TexHeight - 1));
	int2 posE = int2(clamp((int)pos.x + 1, (int)eyeMinPx, (int)eyeMaxPx), pos.y);
	int2 posW = int2(clamp((int)pos.x - 1, (int)eyeMinPx, (int)eyeMaxPx), pos.y);

	int2 taps[4] = { posN, posS, posE, posW };
	float3 centerYCoCg = Color::RGBToYCoCg(current.rgb);
	float3 boxMin = centerYCoCg;
	float3 boxMax = centerYCoCg;
	[unroll] for (int i = 0; i < 4; i++)
	{
		float3 tapYCoCg = Color::RGBToYCoCg(CurrentTex.Load(int3(taps[i], 0)).rgb);
		boxMin = min(boxMin, tapYCoCg);
		boxMax = max(boxMax, tapYCoCg);
	}

	float3 historyYCoCg = clamp(Color::RGBToYCoCg(history.rgb), boxMin, boxMax);
	history.rgb = Color::YCoCgToRGB(historyYCoCg);

	// ── Anti-ghosting: motion-adaptive alpha (squared for soft ramp) ──
	// Increased sensitivity (*10): VR head sway still low enough to keep smoothing,
	// but moderate movement now ramps up faster to reduce ghosting.
	float motionRaw = saturate(length(mv) * 10.0);
	float finalAlpha = max(BlendAlpha, motionRaw * motionRaw);

	// Exponential moving average with adaptive weight
	OutputTex[pos] = lerp(history, current, finalAlpha);
}
