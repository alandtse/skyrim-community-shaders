#include "../Wind.h"
#include "../WindMath.h"

#include <algorithm>
#include <cmath>

using namespace WindSettingsLimits;
using WindMath::ClampFiniteOrDefault;

uint32_t Wind::SanitizeGrassWindSpringTextureSize(uint32_t a_textureSize)
{
	return *std::min_element(kGrassWindSpringTextureSizes.begin(), kGrassWindSpringTextureSizes.end(),
		[a_textureSize](uint32_t a_left, uint32_t a_right) {
			return std::abs(static_cast<int64_t>(a_left) - a_textureSize) <
		           std::abs(static_cast<int64_t>(a_right) - a_textureSize);
		});
}

void Wind::SanitizeGrassWindSettings(Settings& a_settings)
{
	const Settings defaults{};
	a_settings.trunkWindIntensityOverride = ClampFiniteOrDefault(a_settings.trunkWindIntensityOverride, kTrunkWindIntensityMin, kTrunkWindIntensityMax, defaults.trunkWindIntensityOverride);
	a_settings.grassWindResponse = ClampFiniteOrDefault(a_settings.grassWindResponse, kGrassWindResponseMin, kGrassWindResponseMax, defaults.grassWindResponse);
	a_settings.grassWindSensitivity = ClampFiniteOrDefault(a_settings.grassWindSensitivity, kGrassWindSensitivityMin, kGrassWindSensitivityMax, defaults.grassWindSensitivity);
	a_settings.grassWindMaximumTilt = ClampFiniteOrDefault(a_settings.grassWindMaximumTilt, kGrassWindMaximumTiltMin, kGrassWindMaximumTiltMax, defaults.grassWindMaximumTilt);
	a_settings.grassWindBendProfile = ClampFiniteOrDefault(a_settings.grassWindBendProfile, kGrassWindBendProfileMin, kGrassWindBendProfileMax, defaults.grassWindBendProfile);
	a_settings.grassWindCompressionToBend = ClampFiniteOrDefault(a_settings.grassWindCompressionToBend, kGrassWindCompressionToBendMin, kGrassWindCompressionToBendMax, defaults.grassWindCompressionToBend);
	a_settings.grassWindSpringFrequency = ClampFiniteOrDefault(a_settings.grassWindSpringFrequency, kGrassWindSpringFrequencyMin, kGrassWindSpringFrequencyMax, defaults.grassWindSpringFrequency);
	a_settings.grassWindSpringDamping = ClampFiniteOrDefault(a_settings.grassWindSpringDamping, kGrassWindSpringDampingMin, kGrassWindSpringDampingMax, defaults.grassWindSpringDamping);
	for (uint32_t index = 0; index < kGrassWindSpringQualityRangeCount; ++index) {
		a_settings.grassWindSpringQuality[index].textureSize =
			SanitizeGrassWindSpringTextureSize(a_settings.grassWindSpringQuality[index].textureSize);
		a_settings.grassWindSpringQuality[index].maxDistance = ClampFiniteOrDefault(
			a_settings.grassWindSpringQuality[index].maxDistance,
			kGrassWindSpringDistanceMin,
			kGrassWindSpringDistanceMax,
			defaults.grassWindSpringQuality[index].maxDistance);
		if (index > 0)
			a_settings.grassWindSpringQuality[index].maxDistance =
				std::max(a_settings.grassWindSpringQuality[index].maxDistance,
					a_settings.grassWindSpringQuality[index - 1].maxDistance);
	}
	a_settings.grassWindFlutterStrength = ClampFiniteOrDefault(a_settings.grassWindFlutterStrength, kGrassWindFlutterStrengthMin, kGrassWindFlutterStrengthMax, defaults.grassWindFlutterStrength);
	a_settings.grassWindFlutterFrequency = ClampFiniteOrDefault(a_settings.grassWindFlutterFrequency, kGrassWindFlutterFrequencyMin, kGrassWindFlutterFrequencyMax, defaults.grassWindFlutterFrequency);
}

void Wind::ResetGrassWindSettings()
{
	const Settings defaults{};
	settings.overrideTrunkWindIntensity = defaults.overrideTrunkWindIntensity;
	settings.trunkWindIntensityOverride = defaults.trunkWindIntensityOverride;
	settings.enableAmbientGrassWind = defaults.enableAmbientGrassWind;
	settings.grassWindResponse = defaults.grassWindResponse;
	settings.grassWindSensitivity = defaults.grassWindSensitivity;
	settings.grassWindMaximumTilt = defaults.grassWindMaximumTilt;
	settings.grassWindBendProfile = defaults.grassWindBendProfile;
	settings.grassWindCompressionToBend = defaults.grassWindCompressionToBend;
	settings.grassWindSpringFrequency = defaults.grassWindSpringFrequency;
	settings.grassWindSpringDamping = defaults.grassWindSpringDamping;
	settings.grassWindSpringQuality = defaults.grassWindSpringQuality;
	settings.grassWindFlutterStrength = defaults.grassWindFlutterStrength;
	settings.grassWindFlutterFrequency = defaults.grassWindFlutterFrequency;
}
