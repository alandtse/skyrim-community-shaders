#ifndef __RAIN_RENDERING_SHARED_HLSL__
#define __RAIN_RENDERING_SHARED_HLSL__

namespace SharedData
{
	struct RainRenderingSettings
	{
		uint DisableVanillaRain;
		uint3 pad;
	};
}

#endif  // __RAIN_RENDERING_SHARED_HLSL__
