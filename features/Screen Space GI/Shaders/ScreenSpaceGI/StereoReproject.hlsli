#ifndef SSGI_STEREO_REPROJECT
#define SSGI_STEREO_REPROJECT

// Shared view-independent reproject helpers for SSGI: one predicate gates both the
// eye-0-only march skip and the transfer, so the two hit/miss decisions cannot drift.

// Include our own dependencies so this is order-independent: the include sorter
// reorders headers alphabetically, which otherwise pulls common.hlsli after this
// file (SharedData/FP_Z/RES_MIP/samplers) and breaks the FRAMEBUFFER permutation.
// All three are include-guarded, so this is a no-op where already included.
#include "Common/VR.hlsli"
#include "Common/VRReproject.hlsli"
#include "ScreenSpaceGI/common.hlsli"
#include "VRStereoOptimizations/modes.hlsli"

// Inverse of ScreenToViewDepth: linear view-space Z back to raw NDC depth.
float LinearToRawDepth(float d)
{
	return (SharedData::CameraData.x - SharedData::CameraData.w / d) / SharedData::CameraData.z;
}
float4 LinearToRawDepth(float4 d)
{
	return (SharedData::CameraData.x - SharedData::CameraData.w / d) / SharedData::CameraData.z;
}

// FRAMEBUFFER (not just VR) because ReprojectToOtherEye lives in the FrameBuffer-gated
// half of VR.hlsli; the plain-compute gi.cs permutation (no FRAMEBUFFER) includes this
// header but never calls the helper, so keep it out of that compile.
#if defined(VR) && defined(FRAMEBUFFER)
static const float kGIReprojectDepthAgree = 0.05;  // NDC surface-match tolerance

// True when this eye's pixel can take the other eye's marched GI exactly: frustum-valid
// reproject, agreeing depths, and (when useModeTex is set) agreeing VRStereoOptimizations
// classification -- catches boundary pixels depth alone misses. Returns the other-eye
// pixel in otherPx on a hit; a miss is the disoccluded set the caller marches natively.
bool GIReprojectsCleanly(float2 uv, float linearDepth, uint eyeIndex, Texture2D<float> depthTex, float2 texScale, out int2 otherPx,
	bool useModeTex, Texture2D<uint> modeTex, float2 modeTexDim)
{
	otherPx = int2(0, 0);
	if (linearDepth < FP_Z)  // HMD mask / first-person hands: not view-independent geometry
		return false;

	if (useModeTex && !IsModeMain(modeTex, uint2(uv * modeTexDim)))
		return false;

	float rawDepth = LinearToRawDepth(linearDepth);
	Stereo::StereoBilateralResult r = Stereo::ReprojectToOtherEye(uv, rawDepth, eyeIndex, OUT_FRAME_DIM);
	if (!r.valid)  // frustum out-of-bounds: the disoccluded edge strip
		return false;

	float otherLinear = depthTex.SampleLevel(samplerPointClamp, r.otherStereoUV * texScale, RES_MIP);
	if (otherLinear < FP_Z)  // reprojected into mask / hands
		return false;

	otherPx = r.otherPx;
	return Stereo::IsReprojectionExact(r, rawDepth, LinearToRawDepth(otherLinear), kGIReprojectDepthAgree);
}
#endif  // VR && FRAMEBUFFER

#endif  // SSGI_STEREO_REPROJECT
