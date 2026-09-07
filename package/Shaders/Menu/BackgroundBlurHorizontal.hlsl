#include "Menu/BackgroundBlur.hlsli"

float4 PS_Copy(VS_OUTPUT input) : SV_TARGET
{
	return InputTexture.SampleLevel(LinearSampler, input.TexCoord, 0);
}

float4 PS_Downsample(VS_OUTPUT input) : SV_TARGET
{
	float2 offset = BlurTextureSize.zw * BackgroundBlur::kDownsampleOffset;
	return (InputTexture.SampleLevel(LinearSampler, input.TexCoord + offset, 0) +
			   InputTexture.SampleLevel(LinearSampler, input.TexCoord - offset, 0) +
			   InputTexture.SampleLevel(LinearSampler, input.TexCoord + float2(offset.x, -offset.y), 0) +
			   InputTexture.SampleLevel(LinearSampler, input.TexCoord + float2(-offset.x, offset.y), 0)) *
	       0.25f;
}

float4 PS_Main(VS_OUTPUT input) : SV_TARGET
{
	return BackgroundBlur::SampleGaussian(input.TexCoord, float2(BlurTextureSize.z, 0.0f));
}
