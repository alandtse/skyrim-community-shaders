cbuffer SubmitColor : register(b0)
{
	uint Width;
	uint Height;
	uint SourceOffsetX;
	uint Conversion;
};

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	float3 color = max(0.0, Source[uint2(id.x + SourceOffsetX, id.y)].rgb);
	if (Conversion == 1)
		color = (color <= 0.04045) ? color / 12.92 : pow((color + 0.055) / 1.055, 2.4);
	else if (Conversion == 2)
		color = (color <= 0.0031308) ? color * 12.92 : 1.055 * pow(color, 1.0 / 2.4) - 0.055;
	Output[id.xy] = float4(color, 1.0);
}
