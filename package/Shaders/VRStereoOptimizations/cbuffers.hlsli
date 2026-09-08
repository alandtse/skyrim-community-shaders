// VR Stereo Optimizations - Shared constant buffer layout
// Must match VRStereoOptParams in VRStereoOptimizations.h exactly

#ifndef __VR_STEREO_OPT_CBUFFERS_HLSLI__
#define __VR_STEREO_OPT_CBUFFERS_HLSLI__

cbuffer VRStereoOptParams : register(b1)
{
	float2 FrameDim;     // Full stereo buffer dimensions (both eyes)
	float2 RcpFrameDim;  // 1.0 / FrameDim

	uint StereoModeValue;         // 0=Off, 1=Enable
	float DisocclusionThreshold;  // Depth difference threshold for disocclusion detection
	float EdgeDepthThreshold;     // Relative depth difference threshold for edge detection
	uint RepairFromEye0Depth;     // 1 = culled Eye 1 pixels take Eye 0's warped final depth when nearer than the prepass depth

	float2 _pad1;
	float FoveatedRadius;             // reserved for foveated reprojection — see alandtse/open-shaders#143
	float DirectionalOcclusionRatio;  // Eye 0 must be closer than this fraction of Eye 1's depth (0 = disabled)

	float2 FoveatedCenter;  // reserved for foveated reprojection — see alandtse/open-shaders#143
	float MinEdgeDistance;
	float FullBlendDistance;  // Linearized depth below which pixels get MODE_FULL_BLEND (game units)
};

#define DEPTH_UNRENDERED 1.0             // depth clear value: nothing was rasterised at the pixel
#define SCATTER_DEPTH_EMPTY 0xFFFFFFFFu  // ScatterDepth texel no Eye 0 texel landed on; must match kScatterDepthEmpty

#define STEREO_MODE_OFF 0
#define STEREO_MODE_ENABLE 1

#include "VRStereoOptimizations/modes.hlsli"

#endif
