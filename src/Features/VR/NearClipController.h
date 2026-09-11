#pragma once

#include <algorithm>
#include <cmath>

/** @brief Settings in Skyrim world units; RestoreSpeed is an exponential rate per second. */
struct VRNearClipSettings
{
	bool DynamicNearClip = true;
	float NormalNearClip = 5.0f;
	float MinimumNearClip = 0.1f;
	float NearDistanceScale = 0.25f;
	float RestoreSpeed = 0.3f;
	bool DynamicNearClipReadout = false;

	/** @brief Replace non-finite values and constrain the controller's operating range. */
	void ClampNearClipSettings()
	{
		const auto clamp = [](float value, float fallback, float low, float high) {
			return std::clamp(std::isfinite(value) ? value : fallback, low, high);
		};
		NormalNearClip = clamp(NormalNearClip, 5.0f, 0.1f, 30.0f);
		MinimumNearClip = clamp(MinimumNearClip, 0.1f, 0.01f, NormalNearClip);
		NearDistanceScale = clamp(NearDistanceScale, 0.25f, 0.05f, 1.0f);
		RestoreSpeed = clamp(RestoreSpeed, 0.3f, 0.01f, 10.0f);
	}
};

/** @brief Immediate attack, delayed exponential release, and a relative release deadband. */
struct VRNearClipController
{
	static constexpr float kReleaseDelay = 0.25f;
	static constexpr float kHysteresis = 0.05f;
	float current = 0.1f;
	float releaseDelay = kReleaseDelay;

	/** @brief Start at the normal plane; the first valid depth sample can lower it immediately. */
	void Reset(const VRNearClipSettings& settings)
	{
		current = settings.NormalNearClip;
		releaseDelay = kReleaseDelay;
	}

	/** @brief Advance with a target from a recent probe; missing readback never permits release. */
	float Update(float target, float elapsed, bool fresh, const VRNearClipSettings& settings)
	{
		current = std::clamp(current, settings.MinimumNearClip, settings.NormalNearClip);
		if (!fresh || !std::isfinite(target) || !std::isfinite(elapsed))
			return current;
		target = std::clamp(target, settings.MinimumNearClip, settings.NormalNearClip);
		elapsed = std::clamp(elapsed, 0.0f, 0.1f);
		if (target < current) {
			current = target;
			releaseDelay = kReleaseDelay;
		} else if (target <= current * (1.0f + kHysteresis) && target != settings.NormalNearClip) {
			releaseDelay = kReleaseDelay;
		} else {
			const float releaseTime = std::max(0.0f, elapsed - releaseDelay);
			releaseDelay = std::max(0.0f, releaseDelay - elapsed);
			current = std::lerp(current, target, -std::expm1(-settings.RestoreSpeed * releaseTime));
			if (target == settings.NormalNearClip && target - current < settings.NormalNearClip * 0.001f)
				current = target;
		}
		return current;
	}
};
