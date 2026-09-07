Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> DestinationDepth : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint width;
	uint height;
	uint sourceWidth;
	uint sourceHeight;
	DestinationDepth.GetDimensions(width, height);
	SourceDepth.GetDimensions(sourceWidth, sourceHeight);
	if (dispatchThreadID.x < min(width, sourceWidth) && dispatchThreadID.y < min(height, sourceHeight))
		DestinationDepth[dispatchThreadID.xy] = SourceDepth[dispatchThreadID.xy];
}
