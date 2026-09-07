#ifndef __VR_STEREO_OPT_MODES_HLSLI__
#define __VR_STEREO_OPT_MODES_HLSLI__

#define MODE_DISOCCLUDED 0
#define MODE_EDGE 1
#define MODE_MAIN 2
#define MODE_EDGE_NEIGHBOUR 3
#define MODE_FULL_BLEND 4

// True when the classified pixel is eligible for cross-eye reuse.
bool IsModeMain(Texture2D<uint> modeTex, uint2 px)
{
	return modeTex.Load(int3(px, 0)) == MODE_MAIN;
}

#endif
