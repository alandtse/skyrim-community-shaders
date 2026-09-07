// Blends DLSS subrect output back onto the stretched background in kMAIN.
// Replaces the hard CopySubresourceRegion with a feathered transition at the
// subrect boundary.  Two blend modes and two mask shapes are supported:
//   0 = Feather  – smoothstep alpha ramp over FeatherWidth pixels
//   1 = Dither   – noise-perturbed gradient in feather band (DitherStrength controls noise)
//   0 = Rectangle – original rectangular edge
//   1 = Oval      – distance-corrected elliptical edge inside the same rectangle

cbuffer BlendCB : register(b0)
{
	uint DstOffsetX;       // SBS destination X for this eye (0 or eyeWidthOut)
	uint DstOffsetY;       // SBS destination Y (usually 0, non-zero if subrect offset)
	uint SubWidth;         // DLSS output width  (subrect)
	uint SubHeight;        // DLSS output height (subrect)
	uint BlendMode;        // 0 = Feather, 1 = Dither
	uint MaskMode;         // 0 = Rectangle, 1 = Oval
	uint FrameIndex;       // For dither noise animation
	uint SrcOffsetX;       // Source X offset (0 for most modes, non-zero for Extreme strip)
	float FeatherWidth;    // Feather band in pixels (default ~64)
	float DitherStrength;  // 0 = pure smooth gradient, 1 = natural noise, 2 = aggressive dither
	float FalloffCurve;    // 0.5 = earlier neural handoff, 1 = balanced, 2 = later handoff
	float MaskCenterX;     // Subrect-local oval center, in pixels
	float MaskCenterY;
	float MaskRadiusX;  // Subrect-local oval radii, in pixels
	float MaskRadiusY;
	float _pad0;
};

Texture2D<float4> SrcTex : register(t0);    // DLSS subrect output
RWTexture2D<float4> DstTex : register(u0);  // kMAIN (already has stretched background)

// Simple hash-based blue noise (no texture needed, near zero cost)
float BlueNoise(uint2 pos, uint frame)
{
	// Interleaved gradient noise (Jimenez 2014) — good spatial distribution
	float x = float(pos.x) + 5.588238 * float(frame);
	float y = float(pos.y) + 5.588238 * float(frame);
	return frac(52.9829189 * frac(0.06711056 * x + 0.00583715 * y));
}

// First-order signed-distance estimate for an ellipse.  The old normalized
// radius was scaled by the minor axis, so an elongated oval had a visibly
// different transition width near its long-axis and short-axis ends.  The
// implicit ellipse value divided by its gradient gives a pixel-space distance
// near the boundary, while remaining stable and cheap in this small pass.
float EllipseEdgeDistance(float2 offset, float2 radii)
{
	float2 safeRadii = max(radii, float2(0.5, 0.5));
	float2 inverseRadiiSquared = 1.0 / (safeRadii * safeRadii);
	float ellipseValue = 1.0 - dot(offset * offset, inverseRadiiSquared);
	float2 gradient = 2.0 * offset * inverseRadiiSquared;
	float gradientLength = length(gradient);
	if (gradientLength < 1e-5)
		return min(safeRadii.x, safeRadii.y);

	float distance = ellipseValue / gradientLength;
	return clamp(distance, -max(safeRadii.x, safeRadii.y), max(safeRadii.x, safeRadii.y));
}

float FalloffAlpha(float normalizedDistance, float curve)
{
	float t = saturate(normalizedDistance);
	float exponent = clamp(curve, 0.5, 2.0);
	t = pow(t, exponent);
	return t * t * (3.0 - 2.0 * t);
}

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	if (tid.x >= SubWidth || tid.y >= SubHeight)
		return;

	uint2 srcPos = uint2(tid.x + SrcOffsetX, tid.y);
	uint2 dstPos = uint2(tid.x + DstOffsetX, tid.y + DstOffsetY);

	float4 dlss = SrcTex.Load(int3(srcPos, 0));

	// Distance from the selected mask edge in subrect-local space.
	//
	// Bot-flagged Major bug (CodeRabbit + Copilot on the original PR): the
	// previous implementation used `srcPos.x` for distL/distR. In Extreme
	// strip mode, SrcOffsetX != 0 (each eye reads its half of a horizontally-
	// concatenated SBS strip), so srcPos.x = tid.x + SrcOffsetX could exceed
	// SubWidth, making distR negative and breaking the feather band entirely
	// — the strip would never blend correctly with the background. Use the
	// dispatch-local tid.xy so distances are in [0, SubWidth-1] regardless
	// of the source-side offset.
	float edgeDist;
	float featherDistance = FeatherWidth;
	if (MaskMode == 1) {
		// Measure the implicit ellipse in pixels rather than using a normalized
		// radial coordinate scaled by the minor axis. This keeps the transition
		// width more even around elongated regions.
		float2 localPos = float2(tid.xy) + 0.5;
		float2 center = float2(MaskCenterX, MaskCenterY);
		float2 radii = max(float2(MaskRadiusX, MaskRadiusY), float2(0.5, 0.5));
		edgeDist = EllipseEdgeDistance(localPos - center, radii);
	} else {
		float distL = (float)tid.x;
		float distR = (float)(SubWidth - 1 - tid.x);
		float distT = (float)tid.y;
		float distB = (float)(SubHeight - 1 - tid.y);
		edgeDist = min(min(distL, distR), min(distT, distB));
	}

	if (edgeDist >= featherDistance) {
		// Interior: pure DLSS (fast path, skips background read)
		DstTex[dstPos] = dlss;
		return;
	}

	// We're in the feather band — need background
	float4 bg = DstTex[dstPos];

	if (BlendMode == 1) {
		// Dither: noise-perturbed continuous gradient
		// Noise shifts the blend threshold per-pixel → natural irregular boundary
		float t = edgeDist / featherDistance;  // 0 at edge, 1 at band end
		float noise = BlueNoise(srcPos, FrameIndex);
		float alpha = saturate(FalloffAlpha(t, FalloffCurve) + (noise - 0.5) * DitherStrength);
		DstTex[dstPos] = lerp(bg, dlss, alpha);
	} else {
		// Feather (default): tunable smooth alpha ramp
		float alpha = FalloffAlpha(edgeDist / featherDistance, FalloffCurve);
		DstTex[dstPos] = lerp(bg, dlss, alpha);
	}
}
