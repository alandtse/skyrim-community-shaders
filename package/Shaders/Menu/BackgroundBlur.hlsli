#ifndef MENU_BACKGROUND_BLUR_HLSLI
#define MENU_BACKGROUND_BLUR_HLSLI

cbuffer BlurBuffer : register(b1)
{
	float4 BlurTextureSize;  // xy = size, zw = inverse size
	float4 WindowRect;
	float4 WindowParams;  // x = corner radius, yz = screen size, w = fullscreen
};

SamplerState LinearSampler : register(s0);
Texture2D<float4> InputTexture : register(t0);

struct VS_OUTPUT
{
	float4 Position: SV_POSITION;
	float2 TexCoord: TEXCOORD0;
};

VS_OUTPUT VS_Main(uint vertexID : SV_VertexID)
{
	VS_OUTPUT output;
	output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
	output.Position = float4(output.TexCoord * 2.0f - 1.0f, 0.0f, 1.0f);
	output.Position.y = -output.Position.y;
	return output;
}

namespace BackgroundBlur
{
	static const float kDownsampleOffset = 0.25f;
	static const float kWeightSum = 0.1760327f + 2.0f * (0.1658591f + 0.1403215f + 0.1069852f + 0.0732894f);
	static const float kCenterWeight = 0.1760327f / kWeightSum;
	static const float2 kPairWeights = float2(0.1658591f + 0.1403215f, 0.1069852f + 0.0732894f) / kWeightSum;
	static const float2 kPairOffsets = float2(
		(0.1658591f + 2.0f * 0.1403215f) / (0.1658591f + 0.1403215f),
		(3.0f * 0.1069852f + 4.0f * 0.0732894f) / (0.1069852f + 0.0732894f));

	float4 SampleGaussian(float2 uv, float2 direction)
	{
		float4 result = InputTexture.SampleLevel(LinearSampler, uv, 0) * kCenterWeight;
		[unroll] for (int i = 0; i < 2; ++i)
		{
			float2 offset = direction * kPairOffsets[i];
			result += (InputTexture.SampleLevel(LinearSampler, uv + offset, 0) +
						  InputTexture.SampleLevel(LinearSampler, uv - offset, 0)) *
			          kPairWeights[i];
		}
		return result;
	}
}

#endif
