#ifndef COMMON_HASH_HLSLI
#define COMMON_HASH_HLSLI

namespace Hash
{
	/** @brief Mixes a 32-bit integer deterministically for procedural seeds. */
	uint LowBias32(uint value)
	{
		value ^= value >> 16u;
		value *= 0x7FEB352Du;
		value ^= value >> 15u;
		value *= 0x846CA68Bu;
		return value ^ (value >> 16u);
	}
}

#endif
