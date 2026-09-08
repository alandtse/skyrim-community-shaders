// Composite Blur Pass Shader with Rounded Rectangle Mask
// Part of the BackgroundBlur system - applies blurred texture with rounded corners

#include "Menu/BackgroundBlur.hlsli"

static const float CLIP_EPSILON = 0.001f;

float4 SampleBicubic(float2 uv)
{
	float2 pixel = uv * BlurTextureSize.xy - 0.5f;
	float2 base = floor(pixel);
	float2 f = pixel - base;
	float2 f2 = f * f;
	float2 f3 = f2 * f;
	float2 w0 = (1.0f - 3.0f * f + 3.0f * f2 - f3) / 6.0f;
	float2 w1 = (4.0f - 6.0f * f2 + 3.0f * f3) / 6.0f;
	float2 w2 = (1.0f + 3.0f * f + 3.0f * f2 - 3.0f * f3) / 6.0f;
	float2 w3 = f3 / 6.0f;
	float2 g0 = w0 + w1;
	float2 g1 = w2 + w3;
	float2 p0 = (base - 0.5f + w1 / g0) * BlurTextureSize.zw;
	float2 p1 = (base + 1.5f + w3 / g1) * BlurTextureSize.zw;
	return lerp(
		lerp(InputTexture.SampleLevel(LinearSampler, p0, 0), InputTexture.SampleLevel(LinearSampler, float2(p1.x, p0.y), 0), g1.x),
		lerp(InputTexture.SampleLevel(LinearSampler, float2(p0.x, p1.y), 0), InputTexture.SampleLevel(LinearSampler, p1, 0), g1.x), g1.y);
}

// Compute signed distance to a rounded rectangle
// Returns negative inside, positive outside
float RoundedRectSDF(float2 pixelPos, float2 rectMin, float2 rectMax, float radius)
{
	// Center of the rectangle
	float2 rectCenter = (rectMin + rectMax) * 0.5f;
	float2 rectHalfSize = (rectMax - rectMin) * 0.5f;

	// Clamp radius to not exceed half the smallest dimension
	radius = min(radius, min(rectHalfSize.x, rectHalfSize.y));

	// Distance from center
	float2 p = abs(pixelPos - rectCenter) - rectHalfSize + radius;

	// SDF for rounded rectangle
	return length(max(p, 0.0f)) + min(max(p.x, p.y), 0.0f) - radius;
}

float4 PS_Main(VS_OUTPUT input) :
	SV_TARGET
{
	// Convert UV to pixel coordinates
	float2 pixelPos = input.TexCoord * float2(WindowParams.y, WindowParams.z);

	// Get window bounds and corner radius
	float2 rectMin = WindowRect.xy;
	float2 rectMax = WindowRect.zw;
	float cornerRadius = WindowParams.x;

	float alpha = 1.0f;
	if (WindowParams.w < 0.5f) {
		// Calculate signed distance to rounded rectangle
		float sdf = RoundedRectSDF(pixelPos, rectMin, rectMax, cornerRadius);

		// Create smooth edge (anti-aliased)
		// Negative = inside, positive outside
		// Use 1.0 pixel transition for smooth edge
		alpha = saturate(-sdf);

		// Early out if completely outside
		if (alpha <= 0.0f) {
			discard;
		}
	}

	float4 blurColor = SampleBicubic(input.TexCoord);

	blurColor.a = alpha;

	return blurColor;
}

float4 PS_Layer(VS_OUTPUT input) : SV_TARGET
{
	float2 pixelPos = input.TexCoord * WindowParams.yz;
	clip(-RoundedRectSDF(pixelPos, WindowRect.xy, WindowRect.zw, WindowParams.x) - CLIP_EPSILON);
	return SampleBicubic(input.TexCoord);
}

// Clear shader entry point - outputs transparent black inside rounded rect only
// Used to clear UI buffer (HUD) in the exact same shape as the blur
float4 PS_Clear(VS_OUTPUT input) :
	SV_TARGET
{
	float2 pixelPos = input.TexCoord * float2(WindowParams.y, WindowParams.z);
	float sdf = RoundedRectSDF(pixelPos, WindowRect.xy, WindowRect.zw, WindowParams.x);

	// Discard pixels outside rounded rect to preserve HUD in corners
	clip(-sdf - CLIP_EPSILON);

	return float4(0.0f, 0.0f, 0.0f, 0.0f);
}
