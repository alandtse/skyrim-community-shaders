#pragma once

#include <string_view>
#include <vector>

namespace SceneSettingsPolicy
{
	using SettingPolicyPath = std::vector<std::string_view>;

	inline const std::vector<SettingPolicyPath> kSettingBlacklist = {
		{ "CSUtility", "Scene Dof", "locked" },
		{ "CSUtility", "Scene Dof", "baseline" },
		{ "CSUtility", "Underwater Dof", "locked" },
		{ "CSUtility", "Underwater Dof", "baseline" },
		{ "CSUtility", "Underwater Dof", "values", "autoFocus" },
		{ "CSUtility", "Underwater Dof", "values", "autoFocusSettings" },
		{ "CSUtility", "Underwater Dof", "values", "mode" },
		{ "ExponentialHeightFog", "volumetricGridPixelSize" },
		{ "ExponentialHeightFog", "volumetricGridSizeZ" },
		{ "ExponentialHeightFog", "volumetricShadowBias" },
		{ "ExponentialHeightFog", "volumetricDepthDistributionScale" },
		{ "ExponentialHeightFog", "volumetricHistoryWeight" },
		{ "ExponentialHeightFog", "volumetricHistoryMissSampleCount" },
		{ "ExponentialHeightFog", "volumetricSampleJitterMultiplier" },
		{ "ExponentialHeightFog", "volumetricUpsampleJitterMultiplier" },
		{ "ImageBasedLighting", "DisableInInteriors" },
		{ "ImageBasedLighting", "DisableInWorldMap" },
		{ "ImageBasedLighting", "DisableInLoadingScreen" },
		{ "LightLimitFix", "ShowShadowOverlay" },
		{ "PostProcessing", "Border" },
		{ "PostProcessing", "Depth of Field", "HighlightShape" },
		{ "PostProcessing", "LUT" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "enableTonemap" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "useOpenDrt" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "currentTonemapper" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "tonemapParams" },
		{ "PostProcessing", "Motion Blur", "VelocityScale" },
		{ "ScreenSpaceGI", "DebugUseUnjitteredCameraReconstruction" },
		{ "ScreenSpaceGI", "ResourceProfile" },
	};

	inline const std::vector<SettingPolicyPath> kLocationFeatureWhitelist = {
		{ "CSUtility" },
		{ "ExponentialHeightFog" },
		{ "LightLimitFix", "EnableContactShadows" },
		{ "LightLimitFix", "EnableParticleContactShadows" },
		{ "LightLimitFix", "ContactShadowMaxSteps" },
		{ "LightLimitFix", "ContactShadowMaxDistance" },
		{ "LightLimitFix", "ContactShadowStride" },
		{ "LightLimitFix", "ContactShadowThickness" },
		{ "LightLimitFix", "ContactShadowDepthFade" },
		{ "LightLimitFix", "ContactShadowMinIntensity" },
		{ "PostProcessing" },
		{ "ScreenSpaceGI" },
		{ "ScreenSpaceShadows" },
		{ "SubsurfaceScattering" },
		{ "ImageBasedLighting" },
		{ "VanillaFresnel" },
	};

	inline const std::vector<SettingPolicyPath> kTimeOfDayFeatureWhitelist = {
		{ "CSUtility" },
		{ "CloudRelight" },
		{ "CloudShadows" },
		{ "ExponentialHeightFog" },
		{ "GrassLighting" },
		{ "ImageBasedLighting" },
		{ "PostProcessing" },
		{ "Skylighting" },
		{ "SubsurfaceScattering" },
		{ "WetnessEffects" },
	};
}
