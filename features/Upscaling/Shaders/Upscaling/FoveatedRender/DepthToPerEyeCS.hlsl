Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> DestinationDepth : register(u0);

cbuffer DepthCopyConstants : register(b0)
{
	uint SourceOffsetX;
	uint SourceOffsetY;
	uint Width;
	uint Height;
};

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint destinationWidth;
	uint destinationHeight;
	DestinationDepth.GetDimensions(destinationWidth, destinationHeight);

	uint sourceWidth;
	uint sourceHeight;
	SourceDepth.GetDimensions(sourceWidth, sourceHeight);

	if (dispatchThreadID.x >= Width || dispatchThreadID.y >= Height ||
		dispatchThreadID.x >= destinationWidth || dispatchThreadID.y >= destinationHeight)
		return;

	uint sourceX = SourceOffsetX + dispatchThreadID.x;
	uint sourceY = SourceOffsetY + dispatchThreadID.y;
	if (sourceX < sourceWidth && sourceY < sourceHeight)
		DestinationDepth[dispatchThreadID.xy] = SourceDepth.Load(int3(sourceX, sourceY, 0));
}
