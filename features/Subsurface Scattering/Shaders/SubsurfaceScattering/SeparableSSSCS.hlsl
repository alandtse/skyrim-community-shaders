RWTexture2D<float4> SSSRW : register(u0);

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

Texture2D<float4> DepthTexture : register(t0);
Texture2D<float4> ColorTexture : register(t1);
Texture2D<float4> DeferredTexture : register(t2);
Texture2D<float4> SpecularTexture : register(t3);

#define LINEAR

cbuffer PerFrame : register(b0)
{
    float4 DynamicRes;
	float4 CameraData;
	float2 BufferDim;
    float2 RcpBufferDim;
	uint FrameCount;
    float SSSS_FOVY;
    float SSSWidth;
};

float GetScreenDepth(float depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

#include "SeparableSSS.hlsli"

[numthreads(32, 32, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	float2 texCoord = (DTid.xy + 0.5) * RcpBufferDim;
 #if defined(HORIZONTAL)
	    SSSRW[DTid.xy] = SSSSBlurPS(texCoord, float2(1.0, 0.0));
#else
	    float4 color = SSSSBlurPS(texCoord, float2(0.0, 1.0));
#if defined(LINEAR)
		color.rgb = Lin2sRGB(color.rgb);
#endif
		color.rgb += SpecularTexture.SampleLevel(PointSampler, texCoord, 0).rgb;
		
		color.a = 1.0;
		SSSRW[DTid.xy] = color;
#endif
}
