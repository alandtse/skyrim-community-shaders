#ifndef COMMON_HASH_HLSLI
#define COMMON_HASH_HLSLI

namespace Hash
{
	static const float InverseUint24Range = 1.0f / 16777216.0f;

	/** @brief Mixes a 32-bit integer deterministically for procedural seeds. */
	uint LowBias32(uint value)
	{
		value ^= value >> 16u;
		value *= 0x7FEB352Du;
		value ^= value >> 15u;
		value *= 0x846CA68Bu;
		return value ^ (value >> 16u);
	}

	/** @brief Mixes a signed lattice coordinate and seed without floating-point conversion. */
	uint Lattice3D(int3 lattice, uint seed)
	{
		return LowBias32(asuint(lattice.x) ^ LowBias32(asuint(lattice.y) ^ LowBias32(asuint(lattice.z) ^ seed)));
	}

	/** @brief Hashes an integer into the half-open range [0, 1). */
	float Float01(uint value)
	{
		return float(LowBias32(value) >> 8u) * InverseUint24Range;
	}
}

#endif
