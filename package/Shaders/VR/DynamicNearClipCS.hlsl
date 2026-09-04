Texture2D<float> SceneDepth : register(t0);
Texture2D<float> TerrainDepth : register(t1);
RWStructuredBuffer<uint2> EyeResults : register(u0);

cbuffer ProbeConstants : register(b0)
{
	float4 CameraData;
	uint2 RenderSize;
	uint HasTerrainDepth;
	float Padding;
};

groupshared float Nearest[256];
groupshared uint Valid[256];

[numthreads(16, 16, 1)] void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex) {
	// Cover the central 30% of each eye; each grid point also inspects its adjacent texels.
	float2 eyeUV = 0.35 + (float2(thread.xy) + 0.5) * (0.3 / 16.0);
	uint2 eyeSize = uint2(RenderSize.x / 2, RenderSize.y);
	uint2 pixel = min(uint2(eyeUV * eyeSize), eyeSize - 2);
	pixel.x += group.x * eyeSize.x;
	float nearest = 3.402823466e+38;
	uint valid = 0;
	[unroll] for (uint y = 0; y < 2; ++y)
	{
		[unroll] for (uint x = 0; x < 2; ++x)
		{
			float depth = SceneDepth.Load(int3(pixel + uint2(x, y), 0));
			if (HasTerrainDepth)
				depth = min(depth, TerrainDepth.Load(int3(pixel + uint2(x, y), 0)));
			if (isfinite(depth) && depth > 0.0 && depth < 1.0) {
				float distance = CameraData.w / (CameraData.x - depth * CameraData.z);
				if (isfinite(distance) && distance > 0.0 && distance < CameraData.x) {
					nearest = min(nearest, distance);
					++valid;
				}
			}
		}
	}
	Nearest[index] = nearest;
	Valid[index] = valid;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 128; stride > 0; stride >>= 1)
	{
		if (index < stride) {
			Nearest[index] = min(Nearest[index], Nearest[index + stride]);
			Valid[index] += Valid[index + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (index == 0)
		EyeResults[group.x] = uint2(asuint(Nearest[0]), Valid[0]);
}
