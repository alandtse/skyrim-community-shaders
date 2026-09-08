// VR Stereo Optimizations - Stencil Classification Compute Shader
//
// Classifies BOTH eyes over the full SBS buffer. Each pixel is tagged as:
//   MODE_DISOCCLUDED    - Must be fully shaded (sky, HMD mask, parallax-occluded)
//   MODE_EDGE           - Depth edge boundary (dist 1) or inner/foreground band; fully shaded + bilateral blend
//   MODE_MAIN           - Standard pixel eligible for reprojection / bilateral blend
//   MODE_FULL_BLEND     - Near-camera geometry: both eyes fully shaded for 2x supersampling
//
// Dispatched over full SBS resolution (FrameDim.x x FrameDim.y).

#include "Common/SharedData.hlsli"
#include "Common/TemporalReproject.hlsli"
#include "Common/VR.hlsli"
#include "Common/VRReproject.hlsli"
#include "VRStereoOptimizations/cbuffers.hlsli"

Texture2D<float> DepthTexture : register(t0);

#ifdef CLASSIFY_WITH_HISTORY
Texture2D<float> DepthHistory : register(t1);     // previous frame's final depth (full SBS)
Texture2D<uint> UnrepairableMask : register(t2);  // 1 = Eye 1 half-width pixel no Eye 0 depth could repair
#endif

RWTexture2D<uint> ModeTextureRW : register(u0);

// Sentinel for the edge-detection search: means "no discontinuity found yet".
static const uint kEdgeDistNone = 0xFFFFFFFFu;

#ifdef CLASSIFY_WITH_HISTORY
static const uint kHistoryTapCount = 3;  // samples along the segment to the previous-frame position
static const int kMaskNeighborhood = 1;  // half-extent of the unrepairable-mask lookup window

/**
* @brief SBS pixel coordinate this surface point occupied in the previous frame.
*
* @param uv Stereo UV of the pixel [0,1]
* @param depth Raw depth at the pixel
* @param eyeIndex Eye the pixel belongs to (0 or 1)
* @param[out] valid False when the point was behind the previous camera or off screen
* @return Previous-frame SBS pixel coordinate
*/
float2 PreviousFramePixel(float2 uv, float depth, uint eyeIndex, out bool valid)
{
	float2 monoUV = Stereo::ConvertFromStereoUV(uv, eyeIndex);
	float4 clip = float4(monoUV * float2(2, -2) - float2(1, -1), depth, 1);
	float4 world = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], clip);
	world /= world.w;
	float2 prevMonoUV = Temporal::PreviousFrameUV(world.xyz, eyeIndex, valid);
	return Stereo::ConvertToStereoUV(prevMonoUV, eyeIndex) * FrameDim;
}

/**
* @brief Depth for the reprojection tests: the prepass depth, lowered to the nearest
* previous-frame final depth found along the motion segment.
*
* The z-prepass omits alpha-tested geometry, leaving a too-far depth there. Every classifier
* decision is monotone toward native shading in the nearer direction, so the min can only un-cull.
*
* @param px Pixel to classify
* @param eyeIndex Eye the pixel belongs to (0 or 1)
* @param[out] prevPx Previous-frame SBS pixel coordinate
* @param[out] prevValid True when prevPx is a usable on-screen position
* @return min(prepass depth, reprojected previous-frame final depth)
*/
float ClassifyDepth(uint2 px, uint eyeIndex, out float2 prevPx, out bool prevValid)
{
	float d = DepthTexture[px];
	prevPx = 0;
	prevValid = false;
	if (DepthHistoryValid == 0 || d < EPSILON_DEPTH_SKY || d >= DEPTH_UNRENDERED)
		return d;

	prevPx = PreviousFramePixel((float2(px) + 0.5) / FrameDim, d, eyeIndex, prevValid);
	if (!prevValid)
		return d;

	[unroll] for (uint k = 0; k < kHistoryTapCount; k++)
	{
		float2 tap = lerp(float2(px), prevPx, k / float(kHistoryTapCount - 1));
		int2 tapPx = Stereo::ClampToEyeBounds(int2(round(tap)), eyeIndex, FrameDim);
		float h = DepthHistory[tapPx];
		if (h >= EPSILON_DEPTH_SKY && h < DEPTH_UNRENDERED)
			d = min(d, h);
	}
	return d;
}
#endif

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	if (any(dtid >= uint2(FrameDim)))
		return;

	// Determine which eye this pixel belongs to
	float2 uv = (float2(dtid) + 0.5) / FrameDim;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

	// Read depth directly in SBS coords
	float centerDepth = DepthTexture[dtid];

#ifdef DEBUG_DEPTH_MAP
	// DIAGNOSTIC: Visualize what depth values StencilCS sees.
	// Green (MODE_EDGE) = depth >= 1.0 (HMD mask threshold)
	// Magenta (MODE_EDGE_NEIGHBOUR) = depth < EPSILON_DEPTH_SKY (sky threshold)
	// No tint (MODE_MAIN) = normal geometry with valid depth
	if (centerDepth >= 1.0) {
		ModeTextureRW[dtid] = MODE_EDGE;
		return;
	}
	if (centerDepth < EPSILON_DEPTH_SKY) {
		ModeTextureRW[dtid] = MODE_EDGE_NEIGHBOUR;
		return;
	}
	ModeTextureRW[dtid] = MODE_MAIN;
	return;
#endif

	// Sky/unrendered pixels (depth >= 1.0 at z-prepass time = depth buffer clear value)
	// and HMD mask pixels both have depth >= 1.0 here. Treat them the same as sky:
	// let edge detection run so geometry-vs-sky boundaries get classified.
	// HMD mask pixels are in lens corners with no nearby geometry, so they'll
	// fall through to MODE_DISOCCLUDED at the end.
	bool isSky = (centerDepth < EPSILON_DEPTH_SKY) || (centerDepth >= 1.0);
	float linCenter = isSky ? DEPTH_SKY_SENTINEL : SharedData::GetScreenDepth(centerDepth);

	// Near-camera supersampling: geometry closer than FullBlendDistance gets full
	// shading in both eyes for bilateral blend (2x supersampling in VRPostProcess).
	if (!isSky && linCenter < FullBlendDistance) {
		ModeTextureRW[dtid] = MODE_FULL_BLEND;
		return;
	}

	// --- Disocclusion detection via reprojection (runs for all non-sky pixels) ---
	// Early return: disoccluded pixels are always MODE_DISOCCLUDED regardless of edge proximity.
	// This ensures MinEdgeDistance never affects disocclusion classification.
	if (!isSky) {
#ifdef CLASSIFY_WITH_HISTORY
		float2 prevPx = 0;
		bool prevValid = false;
		float reprojDepth = ClassifyDepth(dtid, eyeIndex, prevPx, prevValid);

		// Eye 1 strip that was culled last frame with no Eye 0 depth to repair it:
		// nothing can reconstruct it, so shade it natively this frame.
		if (eyeIndex == 1 && prevValid && UseUnrepairableMask != 0) {
			float2 maskPx = prevPx - float2(FrameDim.x * 0.5, 0);
			int2 maskMax = int2(int(FrameDim.x / 2) - 1, int(FrameDim.y) - 1);
			int2 maskCoord = clamp(int2(round(maskPx)), int2(0, 0), maskMax);
			bool unrepairable = false;
			[unroll] for (int dy = -kMaskNeighborhood; dy <= kMaskNeighborhood; dy++)
			{
				[unroll] for (int dx = -kMaskNeighborhood; dx <= kMaskNeighborhood; dx++)
				{
					int2 tap = clamp(maskCoord + int2(dx, dy), int2(0, 0), maskMax);
					unrepairable = unrepairable || (UnrepairableMask[tap] != 0);
				}
			}
			if (unrepairable) {
				ModeTextureRW[dtid] = MODE_DISOCCLUDED;
				return;
			}
		}
#else
		float reprojDepth = centerDepth;
#endif

		Stereo::StereoBilateralResult reproj = Stereo::ReprojectToOtherEye(
			uv,
			reprojDepth,
			eyeIndex,
			FrameDim);

		bool isDisoccluded = false;
		if (!reproj.valid) {
			isDisoccluded = true;
		} else {
#ifdef CLASSIFY_WITH_HISTORY
			float2 ignoredPrevPx = 0;
			bool ignoredPrevValid = false;
			float otherDepth = ClassifyDepth(uint2(reproj.otherPx), 1 - eyeIndex, ignoredPrevPx, ignoredPrevValid);
#else
			float otherDepth = DepthTexture[reproj.otherPx];
#endif
			// Raw reversed-Z depth comparison for disocclusion detection.
			// Using raw depth avoids concentric semicircle artifacts that occur
			// with linearized depth due to precision band boundaries in the
			// hyperbolic depth-to-linear conversion.
			float maxRaw = max(max(reprojDepth, otherDepth), EPSILON_DIVISION);
			float rawRelDiff = abs(reprojDepth - otherDepth) / maxRaw;
			isDisoccluded = (rawRelDiff > DisocclusionThreshold);

			// Directional disocclusion: catches silhouette edges the symmetric rawRelDiff check
			// above misses -- Eye 0 sees a real occluder meaningfully closer than Eye 1's.
			if (!isDisoccluded && eyeIndex == 1 && DirectionalOcclusionRatio > 0.0) {
				bool otherIsSky = (otherDepth < EPSILON_DEPTH_SKY) || (otherDepth >= 1.0);
				if (!otherIsSky) {
					float linOther = SharedData::GetScreenDepth(otherDepth);
					float linReproj = SharedData::GetScreenDepth(reprojDepth);
					isDisoccluded = (linOther < linReproj * DirectionalOcclusionRatio);
				}
			}
		}

		if (isDisoccluded) {
			ModeTextureRW[dtid] = MODE_DISOCCLUDED;
			return;
		}
	}

	// Depth gate: skip edge detection for nearby geometry (saves perf, distant AA matters more)
	// Sky pixels always run edge detection — they need to expand the edge band outward.
	// Disocclusion detection (above) is independent of this gate and always runs.
	bool skipEdgeDetection = !isSky && (linCenter < MinEdgeDistance);

	// --- Edge detection with two-tier classification ---
	// MODE_EDGE:           immediate neighbor (distance 1) has depth discontinuity, OR
	//                      inner/foreground band (distance <= kInnerWidth).
	// kInnerWidth=4 provides enough margin at high VR resolutions (~8k wide) to catch
	// disocclusion boundary pixels that are just outside the immediate-neighbor band.
	static const uint kInnerWidth = 4;
	int2 offsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };

	uint nearestEdgeDist = kEdgeDistNone;  // nearest distance at which a discontinuity was found
	bool nearestWeAreOuter = false;        // whether we are on the background side at that nearest hit

	// Use the larger of inner/outer widths for the search
	uint maxWidth = kInnerWidth;

	if (!skipEdgeDetection) {
		[loop] for (uint d = 1; d <= maxWidth; d++)
		{
			[unroll] for (int i = 0; i < 4; i++)
			{
				int2 rawNeighbor = int2(dtid) + offsets[i] * (int)d;
				uint2 neighborCoord = Stereo::ClampToEyeBounds(rawNeighbor, eyeIndex, FrameDim);

				float neighborDepth = DepthTexture[neighborCoord];
				bool neighborIsSky = (neighborDepth < EPSILON_DEPTH_SKY) || (neighborDepth >= 1.0);
				float linNeighbor = neighborIsSky ? DEPTH_SKY_SENTINEL : SharedData::GetScreenDepth(neighborDepth);
				float maxLin = max(max(linCenter, linNeighbor), EPSILON_DEPTH_SKY);
				float relDepthDiff = abs(linCenter - linNeighbor) / maxLin;

				if (relDepthDiff > EdgeDepthThreshold && d < nearestEdgeDist) {
					nearestEdgeDist = d;
					nearestWeAreOuter = (linNeighbor < linCenter);  // neighbor closer to camera = we are background
				}
			}
		}

	}  // !skipEdgeDetection

	if (nearestEdgeDist != kEdgeDistNone) {
		// Classify based on distance and side
		if (nearestEdgeDist == 1) {
			// Immediate neighbor discontinuity: always MODE_EDGE regardless of side
			ModeTextureRW[dtid] = MODE_EDGE;
			return;
		} else if (!nearestWeAreOuter && nearestEdgeDist <= kInnerWidth) {
			// Inner/foreground band beyond distance 1
			ModeTextureRW[dtid] = MODE_EDGE;
			return;
		}
	}

	// Sky pixels that aren't near edges -> disoccluded (reprojection is meaningless for sky)
	if (isSky) {
		ModeTextureRW[dtid] = MODE_DISOCCLUDED;
		return;
	}

	// Standard pixel
	ModeTextureRW[dtid] = MODE_MAIN;
}
