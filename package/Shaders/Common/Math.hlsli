#ifndef __MATH_DEPENDENCY_HLSL__
#define __MATH_DEPENDENCY_HLSL__

#define EPSILON_SSS_ALBEDO 1e-3f               // For albedo clamping in SSS calculations
#define EPSILON_SKIN_ALBEDO 0.001f             // Minimum per-channel skin base color to prevent SSS division explosion
#define EPSILON_DOT_CLAMP 1e-5f                // For dot product clamping
#define EPSILON_DEPTH_SKY 1e-5f                // Depth threshold for sky/unrendered pixel detection (raw reversed-Z near zero)
#define EPSILON_DIVISION 1e-6f                 // For division to avoid division by zero
#define EPSILON_GLINTS 1e-8f                   // For glints calculations
#define EPSILON_WEIGHT_SUM 1e-10f              // For weight normalization
#define EPSILON_LENGTH_SQ 1e-20f               // Minimum dot(v,v) before rsqrt to avoid inf on degenerate vectors
#define EPSILON_WIND_GEOMETRY 1e-4f            // Minimum linear extent or magnitude for wind geometry
#define EPSILON_WIND_LENGTH_SQ 1e-6f           // Minimum squared length for wind direction checks
#define EPSILON_WIND_RESPONSE 1e-5f            // Minimum wind response magnitude for stable normalization
#define EPSILON_DAMPED_SPRING_FREQUENCY 1e-3f  // Minimum damped-spring frequency
#define EPSILON_DAMPED_SPRING_RADICAND 1e-5f   // Minimum damped-spring radicand

#define DEPTH_SKY_SENTINEL 999999.0f  // Linearized depth sentinel for sky/unmapped pixels (beyond any real geometry)

// GetWaterData returns .w = INT_MIN (~-2.147e9) when the tile is out of the 5x5 grid.
// Use this threshold to test for "no water body present": waterHeight > WATER_HEIGHT_NO_TILE_SENTINEL.
#define WATER_HEIGHT_NO_TILE_SENTINEL -1e9f

namespace Math
{
	static const float4x4 IdentityMatrix = {
		{ 1, 0, 0, 0 },
		{ 0, 1, 0, 0 },
		{ 0, 0, 1, 0 },
		{ 0, 0, 0, 1 }
	};

	static const float PI = 3.1415926535897932384626433832795f;  // PI
	static const float HALF_PI = PI * 0.5f;                      // PI / 2
	static const float TAU = PI * 2.0f;                          // PI * 2
	static const float INV_PI = 1.0f / PI;                       // 1 / PI

	float GetFinalDepth(float a_depth, float a_nearPlane, float a_farPlane)
	{
		return (2.0f * a_nearPlane * a_farPlane) / ((a_farPlane + a_nearPlane) - (a_depth * 2.0f - 1.0f) * (a_farPlane - a_nearPlane));
	}

	// pow() NaNs on a negative base; use when base is non-negative by construction
	// except for FP rounding noise.
	float SafePow(float base, float exponent)
	{
		return pow(abs(base), exponent);
	}
}

#endif  //__MATH_DEPENDENCY_HLSL__
