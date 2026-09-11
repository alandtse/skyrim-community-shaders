Texture2D<float> SceneDepth : register(t0);
Texture2D<float> TerrainDepth : register(t1);
RWStructuredBuffer<uint4> EyeResults : register(u0);

static const float MaximumDistance = 3.402823466e+38;

cbuffer ProbeConstants : register(b0)
{
	float4 CameraData;
	uint2 RenderSize;
	uint HasTerrainDepth;
	float Padding;
};

groupshared float4 Closest[256];
groupshared uint Valid[256];

void InsertClosest(inout float4 values, float candidate)
{
	if (candidate >= values.w)
		return;
	values.w = candidate;
	if (values.w < values.z) {
		float swap = values.z;
		values.z = values.w;
		values.w = swap;
	}
	if (values.z < values.y) {
		float swap = values.y;
		values.y = values.z;
		values.z = swap;
	}
	if (values.y < values.x) {
		float swap = values.x;
		values.x = values.y;
		values.y = swap;
	}
}

[numthreads(16, 16, 1)] void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex) {
	// Cover the central 30% of each eye; each grid point also inspects its adjacent texels.
	float2 eyeUV = 0.35 + (float2(thread.xy) + 0.5) * (0.3 / 16.0);
	uint2 eyeSize = uint2(RenderSize.x / 2, RenderSize.y);
	uint2 pixel = min(uint2(eyeUV * eyeSize), eyeSize - 2);
	pixel.x += group.x * eyeSize.x;
	float nearest = MaximumDistance;
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
	Closest[index] = float4(nearest, MaximumDistance, MaximumDistance, MaximumDistance);
	Valid[index] = valid != 0;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 128; stride > 0; stride >>= 1)
	{
		if (index < stride) {
			float4 candidates = Closest[index + stride];
			InsertClosest(Closest[index], candidates.x);
			InsertClosest(Closest[index], candidates.y);
			InsertClosest(Closest[index], candidates.z);
			InsertClosest(Closest[index], candidates.w);
			Valid[index] += Valid[index + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (index == 0) {
		float relevant = Valid[0] >= 4 ? Closest[0].w : Closest[0].x;
		EyeResults[group.x] = uint4(asuint(Closest[0].x), asuint(relevant), Valid[0], 0);
	}
}
