cbuffer ModelResolutionParams : register(b0)
{
	uint gMode;
	uint gWidth;
	uint gHeight;
	uint gSourceWidth;
	uint gSourceHeight;
	float gTransferStrength;
	float gColourStrength;
	float gMaxRatio;
	float gResidualStrength;
	float gPadding0;
	float gPadding1;
	float gPadding2;
};

Texture2D<float4> gProxy : register(t0);
Texture2D<float4> gModel : register(t1);
Texture2D<float4> gOriginal : register(t2);
RWTexture2D<float4> gTarget : register(u0);
SamplerState gLinear : register(s0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float3 ClampAp1(float3 color)
{
	const float3x3 bt709ToAp1 = { 0.613097, 0.339523, 0.047379,
		0.070194, 0.916354, 0.013452,
		0.020616, 0.109570, 0.869815 };
	const float3x3 ap1ToBt709 = { 1.705051, -0.621792, -0.083259,
		-0.130256, 1.140805, -0.010548,
		-0.024003, -0.128969, 1.152972 };
	return mul(ap1ToBt709, max(mul(bt709ToAp1, color), float3(0.0, 0.0, 0.0)));
}

float3 CbrtSigned(float3 value)
{
	return sign(value) * pow(abs(value), 1.0 / 3.0);
}

float3 ToOkLab(float3 color)
{
	const float3x3 rgbToLms = { 0.4122214708, 0.5363325363, 0.0514459929,
		0.2119034982, 0.6806995451, 0.1073969566,
		0.0883024619, 0.2817188376, 0.6299787005 };
	const float3x3 lmsToLab = { 0.2104542553, 0.7936177850, -0.0040720468,
		1.9779984951, -2.4285922050, 0.4505937099,
		0.0259040371, 0.7827717662, -0.8086757660 };
	return mul(lmsToLab, CbrtSigned(mul(rgbToLms, color)));
}

float3 FromOkLab(float3 lab)
{
	const float3x3 labToLms = { 1.0, 0.3963377774, 0.2158037573,
		1.0, -0.1055613458, -0.0638541728,
		1.0, -0.0894841775, -1.2914855480 };
	const float3x3 lmsToRgb = { 4.0767416621, -3.3077115913, 0.2309699292,
		-1.2684380046, 2.6097574011, -0.3413193965,
		-0.0041960863, -0.7034186147, 1.7076147010 };
	float3 lms = mul(labToLms, lab);
	return mul(lmsToRgb, lms * lms * lms);
}

float3 PreserveModelHue(float3 scaledModel, float3 model)
{
	float3 scaledLab = ToOkLab(scaledModel);
	const float3 modelLab = ToOkLab(model);
	const float scaledChroma = length(scaledLab.yz);
	const float modelChroma = length(modelLab.yz);
	scaledLab.yz = modelLab.yz * (modelChroma == 0.0 ? 1.0 : scaledChroma / modelChroma);
	return ClampAp1(FromOkLab(scaledLab));
}

// Integrate the source pixel footprint covered by one model pixel. Bilinear
// filtering samples a point and becomes visibly biased at 50%/33%; this
// exact-area filter keeps exposure and thin geometry coverage closer to the
// full-resolution source before Feature 18 sees it.
float4 SampleExactArea(uint2 targetPixel)
{
	const float2 sourceSize = float2(max(gSourceWidth, 1u), max(gSourceHeight, 1u));
	const float2 targetSize = float2(max(gWidth, 1u), max(gHeight, 1u));
	const float2 begin = float2(targetPixel) * sourceSize / targetSize;
	const float2 end = float2(targetPixel + 1u) * sourceSize / targetSize;
	const uint2 sourceLast = uint2(max(gSourceWidth, 1u) - 1u, max(gSourceHeight, 1u) - 1u);
	const uint2 first = min(uint2(floor(begin)), sourceLast);
	const uint2 last = min(uint2(max(ceil(end) - 1.0, 0.0)), sourceLast);

	float4 sum = 0.0;
	[loop] for (uint y = first.y; y <= last.y; ++y)
	{
		const float yWeight = max(0.0, min(end.y, float(y + 1u)) - max(begin.y, float(y)));
		[loop] for (uint x = first.x; x <= last.x; ++x)
		{
			const float xWeight = max(0.0, min(end.x, float(x + 1u)) - max(begin.x, float(x)));
			sum += gProxy.Load(int3(x, y, 0)) * (xWeight * yWeight);
		}
	}

	return sum / max((end.x - begin.x) * (end.y - begin.y), 1e-6);
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= gWidth || id.y >= gHeight)
		return;

	const float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
	if (gMode == 0) {
		gTarget[id.xy] = gProxy.SampleLevel(gLinear, uv, 0);
		return;
	}
	if (gMode == 2) {
		gTarget[id.xy] = SampleExactArea(id.xy);
		return;
	}

	const float3 proxy = gProxy.SampleLevel(gLinear, uv, 0).rgb;
	const float3 model = gModel.SampleLevel(gLinear, uv, 0).rgb;
	const float4 originalSample = gOriginal.Load(int3(id.xy, 0));
	const float3 original = max(originalSample.rgb, float3(0.0, 0.0, 0.0));
	if (gMode == 3) {
		// Matched residual: keep the source's full-resolution texture and add
		// only the model-vs-input difference. The luminance bound limits halo and
		// dark-scene excursions without throwing away the model's local detail.
		float3 residual = model - proxy;
		const float originalLuma = dot(original, kLuma);
		const float residualLuma = dot(residual, kLuma);
		const float maxDelta = max(originalLuma * max(gMaxRatio - 1.0, 0.05), 0.01);
		if (abs(residualLuma) > maxDelta)
			residual *= maxDelta / max(abs(residualLuma), 1e-6);
		const float3 result = max(original + residual * saturate(gResidualStrength), float3(0.0, 0.0, 0.0));
		gTarget[id.xy] = float4(result, originalSample.a);
		return;
	}

	const float originalLuma = dot(original, kLuma);
	const float proxyLuma = dot(proxy, kLuma);
	const float modelLuma = dot(model, kLuma);

	if (modelLuma <= 1e-5) {
		gTarget[id.xy] = float4(original, originalSample.a);
		return;
	}

	float ratio;
	if (originalLuma < proxyLuma)
		ratio = originalLuma / max(proxyLuma, 1e-6);
	else
		ratio = (modelLuma + max(0.0, originalLuma - proxyLuma)) / modelLuma;

	const float3 upgraded = lerp(original,
		PreserveModelHue(model * ratio, model), saturate(gTransferStrength));
	const float upgradedLuma = dot(upgraded, kLuma);
	const float maxRatio = max(gMaxRatio, 1.0);
	float3 boundedUpgraded = upgraded;
	if (originalLuma > 1e-6 && upgradedLuma > 1e-6) {
		// Bound the candidate itself. The old code computed this clamp but then
		// discarded it when gColourStrength was 1, allowing unstable dark-scene
		// model output to replace the original pixel wholesale.
		const float candidateRatio = upgradedLuma / originalLuma;
		const float boundedRatio = clamp(candidateRatio, 1.0 / maxRatio, maxRatio);
		boundedUpgraded *= boundedRatio / max(candidateRatio, 1e-6);
	}
	// Keep the full-resolution DLSS source dominant. This is deliberately a
	// bounded enhancement, not a replacement of the source image.
	const float3 result = lerp(original, boundedUpgraded, saturate(gColourStrength));
	gTarget[id.xy] = float4(max(result, float3(0.0, 0.0, 0.0)), originalSample.a);
}
