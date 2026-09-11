#ifndef COMMON_VALUE_NOISE_HLSLI
#define COMMON_VALUE_NOISE_HLSLI

#include "Common/Hash.hlsli"

namespace ValueNoise
{
	/** @brief Samples seeded, cubic-interpolated 3D value noise in [0, 1). */
	float Sample(float3 position, uint seed)
	{
		int3 lattice = int3(floor(position));
		float3 offset = frac(position);
		float3 blend = offset * offset * (3.0f - 2.0f * offset);
		float value000 = Hash::Float01(Hash::Lattice3D(lattice, seed));
		float value100 = Hash::Float01(Hash::Lattice3D(lattice + int3(1, 0, 0), seed));
		float value010 = Hash::Float01(Hash::Lattice3D(lattice + int3(0, 1, 0), seed));
		float value110 = Hash::Float01(Hash::Lattice3D(lattice + int3(1, 1, 0), seed));
		float value001 = Hash::Float01(Hash::Lattice3D(lattice + int3(0, 0, 1), seed));
		float value101 = Hash::Float01(Hash::Lattice3D(lattice + int3(1, 0, 1), seed));
		float value011 = Hash::Float01(Hash::Lattice3D(lattice + int3(0, 1, 1), seed));
		float value111 = Hash::Float01(Hash::Lattice3D(lattice + int3(1, 1, 1), seed));
		return lerp(
			lerp(lerp(value000, value100, blend.x), lerp(value010, value110, blend.x), blend.y),
			lerp(lerp(value001, value101, blend.x), lerp(value011, value111, blend.x), blend.y), blend.z);
	}
}

#endif
