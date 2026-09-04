#pragma once

#include <cmath>
#include <limits>

namespace VRNearClipMath
{
	/** @brief Recover near from Skyrim's perspective Z/W coefficients in an explicit matrix layout. */
	inline float PerspectiveNear(float depthScale, float depthOffset, float wScale, float wOffset)
	{
		if (!std::isfinite(depthScale) || depthScale <= 0.0f || !std::isfinite(depthOffset) ||
			wScale != 1.0f || wOffset != 0.0f)
			return std::numeric_limits<float>::quiet_NaN();
		const float nearDistance = -depthOffset / depthScale;
		return std::isfinite(nearDistance) && nearDistance > 0.0f ? nearDistance : std::numeric_limits<float>::quiet_NaN();
	}
}
