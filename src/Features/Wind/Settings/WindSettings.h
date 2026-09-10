#pragma once

#include <array>
#include <cstdint>
#include <string_view>

/** Persistent settings owned by the Wind feature. */
struct WindSettings
{
	struct GrassWindSpringQualityRange
	{
		uint32_t textureSize;
		float maxDistance;
	};

	bool enableTrunkBend = true;
	bool overrideTrunkWindIntensity = false;
	float trunkWindIntensityOverride = 1.0f;
	float trunkWindBendSensitivity = 1.02f;
	float treeLeafBaseWindFlutterGain = 5.0f;
	float treeWindGustScale = 1.0f;
	float treeWindGustSoftLimit = 0.75f;
	float treeWindSpringFrequency = 0.6f;
	float treeWindSpringDamping = 0.7f;
	float treeTransientSpringFrequency = 2.0f;
	float treeTransientSpringDamping = 0.7f;
	float windFieldGustScale = 853.0f;
	float windFieldGustCrosswindScale = 824.0f;
	float windFieldGustAmplitude = 1.0f;
	float windFieldGustAdvectionMultiplier = 0.93f;
	float windFieldDirectionTransitionDuration = 15.0f;
	bool processMidRangeTransients = true;
	bool processFarRangeTransients = false;
	bool enableAmbientGrassWind = true;
	float grassWindResponse = 20.0f;
	float grassWindSensitivity = 2.59f;
	float grassWindMaximumTilt = 89.0f;
	float grassWindBendProfile = 0.5f;
	float grassWindCompressionToBend = 0.5f;
	float grassWindSpringFrequency = 2.0f;
	float grassWindSpringDamping = 0.82f;
	std::array<GrassWindSpringQualityRange, 3> grassWindSpringQuality{ { { 512, 3000.0f },
		{ 512, 6262.0f },
		{ 256, 12000.0f } } };
	float grassWindFlutterStrength = 2.0f;
	float grassWindFlutterFrequency = 1.0f;
};

namespace WindSettingsLimits
{
	inline constexpr float kTrunkWindSensitivityMin = 0.0f;
	inline constexpr float kTrunkWindSensitivityMax = 20.0f;
	inline constexpr float kTreeLeafBaseWindFlutterGainMin = 0.0f;
	inline constexpr float kTreeLeafBaseWindFlutterGainMax = 20.0f;
	inline constexpr float kTreeWindGustScaleMin = 0.0f;
	inline constexpr float kTreeWindGustScaleMax = 2.0f;
	inline constexpr float kTreeWindGustSoftLimitMin = 0.0f;
	inline constexpr float kTreeWindGustSoftLimitMax = 2.0f;
	inline constexpr float kTreeWindSpringFrequencyMin = 0.25f;
	inline constexpr float kTreeWindSpringFrequencyMax = 2.0f;
	inline constexpr float kTreeWindSpringDampingMin = 0.5f;
	inline constexpr float kTreeWindSpringDampingMax = 1.5f;
	inline constexpr float kTreeTransientSpringFrequencyMin = 0.5f;
	inline constexpr float kTreeTransientSpringFrequencyMax = 8.0f;
	inline constexpr float kTreeTransientSpringDampingMin = 0.5f;
	inline constexpr float kTreeTransientSpringDampingMax = 1.5f;
	inline constexpr float kWindFieldGustScaleMin = 128.0f;
	inline constexpr float kWindFieldGustScaleMax = 16384.0f;
	inline constexpr float kWindFieldGustCrosswindScaleMin = 128.0f;
	inline constexpr float kWindFieldGustCrosswindScaleMax = 65536.0f;
	inline constexpr float kWindFieldGustAmplitudeMin = 0.0f;
	inline constexpr float kWindFieldGustAmplitudeMax = 1.0f;
	inline constexpr float kWindFieldGustAdvectionMultiplierMin = 0.0f;
	inline constexpr float kWindFieldGustAdvectionMultiplierMax = 8.0f;
	inline constexpr float kWindFieldDirectionTransitionDurationMin = 0.0f;
	inline constexpr float kWindFieldDirectionTransitionDurationMax = 30.0f;
	inline constexpr float kTrunkWindIntensityMin = 0.0f;
	inline constexpr float kTrunkWindIntensityMax = 10.0f;
	inline constexpr float kGrassWindResponseMin = 0.0f;
	inline constexpr float kGrassWindResponseMax = 180.0f;
	inline constexpr float kGrassWindSensitivityMin = 0.0f;
	inline constexpr float kGrassWindSensitivityMax = 5.0f;
	inline constexpr float kGrassWindMaximumTiltMin = 0.0f;
	inline constexpr float kGrassWindMaximumTiltMax = 89.0f;
	inline constexpr float kGrassWindBendProfileMin = 0.0f;
	inline constexpr float kGrassWindBendProfileMax = 1.0f;
	inline constexpr float kGrassWindCompressionToBendMin = 0.0f;
	inline constexpr float kGrassWindCompressionToBendMax = 1.0f;
	inline constexpr float kGrassWindSpringFrequencyMin = 0.25f;
	inline constexpr float kGrassWindSpringFrequencyMax = 8.0f;
	inline constexpr float kGrassWindSpringDampingMin = 0.5f;
	inline constexpr float kGrassWindSpringDampingMax = 1.5f;
	inline constexpr float kGrassWindSpringDistanceMin = 1000.0f;
	inline constexpr float kGrassWindSpringDistanceMax = 32768.0f;
	inline constexpr float kGrassWindFlutterStrengthMin = 0.0f;
	inline constexpr float kGrassWindFlutterStrengthMax = 2.0f;
	inline constexpr float kGrassWindFlutterFrequencyMin = 0.0f;
	inline constexpr float kGrassWindFlutterFrequencyMax = 2.0f;
	inline constexpr uint32_t kGrassWindSpringQualityRangeCount = 3;
	inline constexpr std::array<std::string_view, kGrassWindSpringQualityRangeCount> kGrassWindSpringQualityRangeNames{
		"Near", "Mid", "Far"
	};
	inline constexpr std::array<uint32_t, 6> kGrassWindSpringTextureSizes{ 32, 64, 128, 256, 512, 1024 };
}
