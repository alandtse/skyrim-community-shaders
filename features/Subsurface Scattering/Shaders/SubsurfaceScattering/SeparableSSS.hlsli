/**
 * Copyright (C) 2012 Jorge Jimenez (jorge@iryoku.com)
 * Copyright (C) 2012 Diego Gutierrez (diegog@unizar.es)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the following disclaimer
 *       in the documentation and/or other materials provided with the 
 *       distribution:
 *
 *       "Uses Separable SSS. Copyright (C) 2012 by Jorge Jimenez and Diego
 *        Gutierrez."
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS 
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR 
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDERS OR CONTRIBUTORS 
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are 
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the copyright holders.
 */


/**
 *                  _______      _______      _______      _______
 *                 /       |    /       |    /       |    /       |
 *                |   (----    |   (----    |   (----    |   (----
 *                 \   \        \   \        \   \        \   \
 *              ----)   |    ----)   |    ----)   |    ----)   |
 *             |_______/    |_______/    |_______/    |_______/
 *
 *        S E P A R A B L E   S U B S U R F A C E   S C A T T E R I N G
 *
 *                           http://www.iryoku.com/
 *
 * Hi, thanks for your interest in Separable SSS!
 *
 * It's a simple shader composed of two components:
 *
 * 1) A transmittance function, 'SSSSTransmittance', which allows to calculate
 *    light transmission in thin slabs, useful for ears and nostrils. It should
 *    be applied during the main rendering pass as follows:
 *
 *        float3 t = albedo.rgb * lights[i].color * attenuation * spot;
 *        color.rgb += t * SSSSTransmittance(...)
 *
 *    (See 'Main.fx' for more details).
 *
 * 2) A simple two-pass reflectance post-processing shader, 'SSSSBlur*', which
 *    softens the skin appearance. It should be applied as a regular
 *    post-processing effect like bloom (the usual framebuffer ping-ponging):
 *
 *    a) The first pass (horizontal) must be invoked by taking the final color
 *       framebuffer as input, and storing the results into a temporal
 *       framebuffer.
 *    b) The second pass (vertical) must be invoked by taking the temporal
 *       framebuffer as input, and storing the results into the original final
 *       color framebuffer.
 *
 *    Note that This SSS filter should be applied *before* tonemapping.
 *
 * Before including SeparableSSS.h you'll have to setup the target. The
 * following targets are available:
 *         SMAA_HLSL_3
 *         SMAA_HLSL_4
 *         SMAA_GLSL_3
 *
 * For more information of what's under the hood, you can check the following
 * URLs (but take into account that the shader has evolved a little bit since
 * these publications):
 *
 * 1) Reflectance: http://www.iryoku.com/sssss/
 * 2) Transmittance: http://www.iryoku.com/translucency/
 *
 * If you've got any doubts, just contact us!
 */

//-----------------------------------------------------------------------------
// Separable SSS Reflectance Pixel Shader
float3 sRGB2Lin(float3 Color)
{ return Color > 0.04045 ? pow(Color / 1.055 + 0.055 / 1.055, 2.4) : Color / 12.92; }

float3 Lin2sRGB(float3 Color)
{ return Color > 0.0031308 ? 1.055 * pow(Color, 1.0/2.4) - 0.055 : 12.92 * Color; }

float RGBToLuminance(float3 color)
{
	return dot(color, float3(0.2125, 0.7154, 0.0721));
}

float InterleavedGradientNoise(float2 uv)
{
    if (FrameCount == 0)
        return 1.0;
	// Temporal factor
	float frameStep = float(FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy))) * 1.5;
}

#define SSSS_N_SAMPLES 25

float3 gaussian(float variance, float r, float3 sss) {
    /**
     * We use a falloff to modulate the shape of the profile. Big falloffs
     * spreads the shape making it wider, while small falloffs make it
     * narrower.
     */
    float3 falloff = float3(1.0f, 0.37f, 0.3f);
    falloff = sss;
    
    float3 g;
    for (int i = 0; i < 3; i++) {
        float rr = r / (0.001f + falloff[i]);
        g[i] = exp((-(rr * rr)) / (2.0f * variance)) / (2.0f * 3.14f * variance);
    }
    return g;
}


float3 profile(float r, float3 sss) {
    /**
     * We used the red channel of the original skin profile defined in
     * [d'Eon07] for all three channels. We noticed it can be used for green
     * and blue channels (scaled using the falloff parameter) without
     * introducing noticeable differences and allowing for total control over
     * the profile. For example, it allows to create blue SSS gradients, which
     * could be useful in case of rendering blue creatures.
     */
    return  // 0.233f * gaussian(0.0064f, r) + /* We consider this one to be directly bounced light, accounted by the strength parameter (see @STRENGTH) */
               0.100f * gaussian(0.0484f, r, sss) +
               0.118f * gaussian( 0.187f, r, sss) +
               0.113f * gaussian( 0.567f, r, sss) +
               0.358f * gaussian(  1.99f, r, sss) +
               0.078f * gaussian(  7.41f, r, sss);
} 

void CalculateKernel(inout float4 kernel[SSSS_N_SAMPLES], in float3 sss) {
    const float3 strength = float3(0.48f, 0.41f, 0.28f);
    const float RANGE = SSSS_N_SAMPLES > 20? 3.0f : 2.0f;
    const float EXPONENT = 2.0f;

    // Calculate the offsets:
    float step = 2.0f * RANGE / (SSSS_N_SAMPLES - 1);
    
    for (int i = 0; i < SSSS_N_SAMPLES; i++) {
        float o = -RANGE + float(i) * step;
        float sign = o < 0.0f? -1.0f : 1.0f;
        kernel[i].w = RANGE * sign * abs(pow(o, EXPONENT)) / pow(RANGE, EXPONENT);
    }
    
    // Calculate the weights:  
    for (int i = 0; i < SSSS_N_SAMPLES; i++) {
        float w0 = 0.0f;
        if (i > 0)
            w0 = abs(kernel[i].w - kernel[i - 1].w);

        float w1 = 0.0f;
        if (i < SSSS_N_SAMPLES - 1)
            w1 = abs(kernel[i].w - kernel[i + 1].w);

        float area = (w0 + w1) / 2.0f;
        float3 t = area * profile(kernel[i].w, sss);
        kernel[i].xyz = t.xyz;
    }
    
    // We want the offset 0.0 to come first:
    float4 t = kernel[SSSS_N_SAMPLES / 2];
    for (int i = SSSS_N_SAMPLES / 2; i > 0; i--)
        kernel[i] = kernel[i - 1];
    kernel[0] = t;
    
    // Calculate the sum of the weights, we will need to normalize them below:
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < SSSS_N_SAMPLES; i++)
        sum += kernel[i].xyz;

    // Normalize the weights:
    for (int i = 0; i < SSSS_N_SAMPLES; i++)
        kernel[i].xyz /= sum.xyz;

    // Tweak them using the desired strength. The first one is:
    //     lerp(1.0, kernel[0].rgb, strength)
    kernel[0].xyz = (1.0f - strength.xyz) * 1.0f + strength.xyz * kernel[0].xyz;

    // The others:
    //     lerp(0.0, kernel[0].rgb, strength)
    for (int i = 1; i < SSSS_N_SAMPLES; i++)
        kernel[i].xyz *= strength.xyz;
}

float4 SSSSBlurPS(
        float2 texcoord,
        float2 dir
    ) {

    // Fetch color of current pixel:
    float4 colorM = ColorTexture.SampleLevel(PointSampler, texcoord, 0);

#if defined(HORIZONTAL) && defined(LINEAR)
    colorM.rgb = sRGB2Lin(colorM.rgb);
#endif

    float3 sss = DeferredTexture.SampleLevel(PointSampler, texcoord, 0).xyz;

    colorM.a = sss.x;
    if (colorM.a == 0)
        return colorM;

    // Fetch linear depth of current pixel:
    float depthM =  DepthTexture.SampleLevel(PointSampler, texcoord, 0).r;
    depthM = GetScreenDepth(depthM);

    float4 kernel[SSSS_N_SAMPLES] = {
        float4(0.530605, 0.613514, 0.739601, 0),
        float4(0.000973794, 1.11862e-005, 9.43437e-007, -3),
        float4(0.00333804, 7.85443e-005, 1.2945e-005, -2.52083),
        float4(0.00500364, 0.00020094, 5.28848e-005, -2.08333),
        float4(0.00700976, 0.00049366, 0.000151938, -1.6875),
        float4(0.0094389, 0.00139119, 0.000416598, -1.33333),
        float4(0.0128496, 0.00356329, 0.00132016, -1.02083),
        float4(0.017924, 0.00711691, 0.00347194, -0.75),
        float4(0.0263642, 0.0119715, 0.00684598, -0.520833),
        float4(0.0410172, 0.0199899, 0.0118481, -0.333333),
        float4(0.0493588, 0.0367726, 0.0219485, -0.1875),
        float4(0.0402784, 0.0657244, 0.04631, -0.0833333),
        float4(0.0211412, 0.0459286, 0.0378196, -0.0208333),
        float4(0.0211412, 0.0459286, 0.0378196, 0.0208333),
        float4(0.0402784, 0.0657244, 0.04631, 0.0833333),
        float4(0.0493588, 0.0367726, 0.0219485, 0.1875),
        float4(0.0410172, 0.0199899, 0.0118481, 0.333333),
        float4(0.0263642, 0.0119715, 0.00684598, 0.520833),
        float4(0.017924, 0.00711691, 0.00347194, 0.75),
        float4(0.0128496, 0.00356329, 0.00132016, 1.02083),
        float4(0.0094389, 0.00139119, 0.000416598, 1.33333),
        float4(0.00700976, 0.00049366, 0.000151938, 1.6875),
        float4(0.00500364, 0.00020094, 5.28848e-005, 2.08333),
        float4(0.00333804, 7.85443e-005, 1.2945e-005, 2.52083),
        float4(0.000973794, 1.11862e-005, 9.43437e-007, 3),
    };

    // Accumulate center sample, multiplying it with its gaussian weight:
    float4 colorBlurred = colorM;
    colorBlurred.rgb *= kernel[0].rgb;
    
    // World-space width
    float distanceToProjectionWindow = 1.0 / tan(0.5 * radians(SSSS_FOVY));
    float scale = distanceToProjectionWindow / depthM;

    // Calculate the final step to fetch the surrounding pixels:
    float2 finalStep = scale * dir / 3.0;
    finalStep *= 1; // Modulate it using the alpha channel.
    finalStep *= InterleavedGradientNoise(texcoord * BufferDim); // Randomise width to fix banding

    // Accumulate the other samples:
    [unroll]
    for (int i = 1; i < SSSS_N_SAMPLES; i++) {
        // Fetch color and depth for current sample:
        float2 offset = texcoord + kernel[i].a * finalStep;
        float3 color = ColorTexture.SampleLevel(LinearSampler, offset, 0).rgb;

#if defined(HORIZONTAL) && defined(LINEAR)
    color.rgb = sRGB2Lin(color.rgb);
#endif

        float depth = DepthTexture.SampleLevel(LinearSampler, offset, 0).r;
        depth = GetScreenDepth(depth);

        // If the difference in depth is huge, we lerp color back to "colorM":
        float s = min(0.125 * abs(depthM - depth), 1.0);
        color = lerp(color, colorM.rgb, s);

        // Accumulate:
        colorBlurred.rgb += kernel[i].rgb * color.rgb;
    }

    colorBlurred = lerp(colorM, colorBlurred, colorM.a);
   
    return colorBlurred;
}

//-----------------------------------------------------------------------------
