#include "Menu/BackgroundBlur.hlsli"

float4 PS_Main(VS_OUTPUT input) : SV_TARGET
{
	return BackgroundBlur::SampleGaussian(input.TexCoord, float2(0.0f, BlurTextureSize.w));
}
