#include "RainRendering.h"

#if defined(ENABLE_EFFECTS11)
#	include "Effects11/PresetManager.h"
#endif
#include "Globals.h"
#include "I18n/I18n.h"
#include "Skylighting.h"
#include "Utils/FileSystem.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>

#define I18N_KEY_PREFIX "feature.rain_rendering."

namespace
{
	bool DrawFlagCheckbox(const char* a_label, uint& a_value)
	{
		bool enabled = a_value != 0;
		if (!ImGui::Checkbox(a_label, &enabled))
			return false;
		a_value = enabled ? 1u : 0u;
		return true;
	}
}

void RainRendering::DrawPerformanceSettings()
{
	NormalizeSettings();
	int dropCount = static_cast<int>(settings.RainDropCount);
	if (ImGui::SliderInt(T(TKEY("drop_count"), "Maximum Drop Count"), &dropCount,
			static_cast<int>(kDropCountRange.minimum), static_cast<int>(kDropCountRange.maximum)))
		settings.RainDropCount = static_cast<uint>(dropCount);

	int overheadDropCount = static_cast<int>(settings.RainOverheadDropCount);
	if (ImGui::SliderInt(T(TKEY("overhead_drop_count"), "Overhead Drop Count"), &overheadDropCount,
			static_cast<int>(kOverheadDropCountRange.minimum), static_cast<int>(kOverheadDropCountRange.maximum)))
		settings.RainOverheadDropCount = static_cast<uint>(overheadDropCount);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("overhead_drop_count_tooltip"), "Reserves this many near-layer particles for a softly faded volume above the player. The total drop count does not increase."));

	ImGui::SliderFloat(T(TKEY("density"), "Rain Density"), &settings.RainDensity, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, "%.2f");
	if (ImGui::SliderFloat(T(TKEY("far_distance"), "Rain Far Distance"), &settings.RainFarDistance, kFarDistanceRange.minimum, kFarDistanceRange.maximum, "%.0f"))
		NormalizeSettings();

	if (ImGui::TreeNodeEx(T(TKEY("layer_budgets"), "Depth Layer Budgets"))) {
		const float nearLayerMaximum = GetNearLayerMaximum(settings.RainFarDistance);
		if (ImGui::SliderFloat(T(TKEY("near_layer_distance"), "Near Layer Distance"), &settings.RainNearLayerDistance, kMinimumNearLayerDistance, nearLayerMaximum, "%.0f units"))
			NormalizeSettings();
		const float midLayerMinimum = GetMidLayerMinimum(settings.RainNearLayerDistance);
		const float midLayerMaximum = GetMidLayerMaximum(settings.RainFarDistance);
		ImGui::SliderFloat(T(TKEY("mid_layer_distance"), "Mid Layer Distance"), &settings.RainMidLayerDistance, midLayerMinimum, midLayerMaximum, "%.0f units");
		ImGui::SliderFloat(T(TKEY("near_budget"), "Near Budget Weight"), &settings.RainNearBudgetWeight, kBudgetWeightRange.minimum, kBudgetWeightRange.maximum, "%.2f");
		ImGui::SliderFloat(T(TKEY("mid_budget"), "Mid Budget Weight"), &settings.RainMidBudgetWeight, kBudgetWeightRange.minimum, kBudgetWeightRange.maximum, "%.2f");
		ImGui::SliderFloat(T(TKEY("far_budget"), "Far Budget Weight"), &settings.RainFarBudgetWeight, kBudgetWeightRange.minimum, kBudgetWeightRange.maximum, "%.2f");

		const auto counts = GetLayerDropCounts();
		const auto radii = GetLayerRadii(settings.RainFarDistance);
		ImGui::Text(T(TKEY("allocated_drops"), "Reserved drops: %u near / %u mid / %u far"), counts[0], counts[1], counts[2]);
		ImGui::Text(T(TKEY("effective_layer_radii"), "Effective outer ranges: %.0f / %.0f / %.0f"), radii.x, radii.y, radii.z);
		ImGui::TextWrapped(T(TKEY("layer_budget_help"), "Weights split the existing maximum drop count. Adjacent layers overlap and fade smoothly; near/mid ranges are limited by the far range."));
		ImGui::TreePop();
	}
	NormalizeSettings();
}

void RainRendering::ApplyGlassyReferenceSettings()
{
	const auto& defaults = GetDefaultSettings();
	settings.EnableRainRendering = defaults.EnableRainRendering;
	settings.EnableGlassyRain = defaults.EnableGlassyRain;
	settings.EnableTexturedRain = 1u;
	settings.EnableRainRefraction = defaults.EnableRainRefraction;
	settings.RainStreakWidth = defaults.RainStreakWidth;
	settings.RainStreakLength = defaults.RainStreakLength;
	settings.RainVelocityStretch = defaults.RainVelocityStretch;
	settings.RainOpacity = defaults.RainOpacity;
	settings.RainBrightness = defaults.RainBrightness;
	settings.RainLightingResponse = defaults.RainLightingResponse;
	settings.RainMinimumVisibility = defaults.RainMinimumVisibility;
	settings.RainCoreDarkening = defaults.RainCoreDarkening;
	settings.RainEdgeHighlight = defaults.RainEdgeHighlight;
	settings.RainRefractionStrength = defaults.RainRefractionStrength;
	settings.RainRefractionDistance = defaults.RainRefractionDistance;
	settings.RainTextureNormalStrength = defaults.RainTextureNormalStrength;
	settings.RainTextureReflectionStrength = defaults.RainTextureReflectionStrength;
	settings.RainTextureUVWidth = defaults.RainTextureUVWidth;
	settings.RainEnvironmentTransmission = defaults.RainEnvironmentTransmission;
	settings.RainSceneRefractionMix = defaults.RainSceneRefractionMix;
	settings.RainHighlightRoughness = defaults.RainHighlightRoughness;
	settings.RainLightScattering = defaults.RainLightScattering;
	settings.RainLocalLightResponse = defaults.RainLocalLightResponse;
	settings.RainDebugMode = 0;
	NormalizeSettings();
}

void RainRendering::DrawGeneralSettings()
{
	DrawFlagCheckbox(T(TKEY("enable"), "Enable Airborne Rain"), settings.EnableRainRendering);
	DrawFlagCheckbox(T(TKEY("force_rain"), "Force Rain for Testing"), settings.ForceRainRendering);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("force_rain_tooltip"), "Renders at full intensity regardless of the current weather or location."));

	if (shaderCompileAttempted && !renderPathReady) {
		Util::Text::Error("%s", T(TKEY("shader_compile_error"), "Airborne Rain shaders failed to compile, so the effect is not rendering."));
		if (ImGui::Button(T(TKEY("retry_shaders"), "Retry Rain Shaders")))
			ClearShaderCache();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("retry_shaders_tooltip"), "Clears the failed compile state and retries when rain next renders."));
	}
}

void RainRendering::DrawVolumeSettings()
{
	if (!ImGui::TreeNodeEx(T(TKEY("volume_density"), "Volume & Density"), ImGuiTreeNodeFlags_DefaultOpen))
		return;
	DrawPerformanceSettings();
	ImGui::SliderFloat(T(TKEY("near_cutoff_distance"), "Near Cutoff Distance"), &settings.RainNearCutoffDistance, kNearCutoffDistanceRange.minimum, kNearCutoffDistanceRange.maximum, "%.1f units");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("near_cutoff_distance_tooltip"), "Hard-discards rain streaks that come within this distance of the head. There is no opacity fade; zero disables the cutoff."));
	ImGui::TreePop();
}

void RainRendering::DrawMotionSettings()
{
	if (!ImGui::TreeNodeEx(T(TKEY("motion"), "Motion"), ImGuiTreeNodeFlags_DefaultOpen))
		return;
	ImGui::SliderFloat(T(TKEY("fall_speed"), "Base Fall Speed"), &settings.RainFallSpeed, kFallSpeedRange.minimum, kFallSpeedRange.maximum, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("fall_speed_tooltip"), "Baseline fall speed scaled by the active weather's gravity. Force Rain uses this value directly."));
	DrawFlagCheckbox(T(TKEY("wind_enable"), "Follow Vanilla Rain Wind"), settings.EnableRainWind);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("wind_enable_tooltip"), "Uses the active vanilla rain emitter's wind-to-gravity ratio. It does not load the separate spatial wind system."));
	ImGui::BeginDisabled(!settings.EnableRainWind);
	ImGui::SliderFloat(T(TKEY("wind_influence"), "Wind Influence"), &settings.RainWindInfluence, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, "%.2f");
	ImGui::EndDisabled();
	ImGui::TreePop();
}

void RainRendering::DrawAppearanceSettings()
{
	if (!ImGui::TreeNodeEx(T(TKEY("streaks"), "Streak Appearance"), ImGuiTreeNodeFlags_DefaultOpen))
		return;
	ImGui::SliderFloat(T(TKEY("streak_length"), "Base Streak Length"), &settings.RainStreakLength, kStreakLengthRange.minimum, kStreakLengthRange.maximum, "%.1f");
	ImGui::SliderFloat(T(TKEY("velocity_stretch"), "Velocity Stretch"), &settings.RainVelocityStretch, kVelocityStretchRange.minimum, kVelocityStretchRange.maximum, "%.3f");
	ImGui::SliderFloat(T(TKEY("streak_width"), "Streak Width"), &settings.RainStreakWidth, kStreakWidthRange.minimum, kStreakWidthRange.maximum, "%.2f");
	ImGui::SliderFloat(T(TKEY("opacity"), "Rain Opacity"), &settings.RainOpacity, kUnitRange.minimum, kUnitRange.maximum, "%.2f");
	ImGui::SliderFloat(T(TKEY("brightness"), "Rain Brightness"), &settings.RainBrightness, kBrightnessRange.minimum, kBrightnessRange.maximum, "%.2f");
	ImGui::SliderFloat(T(TKEY("lighting_response"), "Rain Lighting Response"), &settings.RainLightingResponse, kUnitRange.minimum, kUnitRange.maximum, "%.2f");
	ImGui::SliderFloat(T(TKEY("minimum_visibility"), "Minimum Dark-Scene Visibility"), &settings.RainMinimumVisibility, kUnitRange.minimum, kUnitRange.maximum, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("minimum_visibility_tooltip"), "Keeps restrained water highlights visible when a weather or post-processing preset makes environmental lighting nearly black. This affects shaped highlights, not the whole streak."));
	ImGui::TreePop();
}

void RainRendering::DrawTextureSettings(bool a_usesWaterMaterial)
{
	if (!ImGui::TreeNodeEx(T(TKEY("texture_map"), "Drop Texture")))
		return;

	ImGui::BeginDisabled(!a_usesWaterMaterial);
	DrawFlagCheckbox(T(TKEY("textured_rain"), "Textured Water Drops"), settings.EnableTexturedRain);
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("textured_rain_tooltip"), "Uses an optional normal/opacity map on world-space streaks when one is available. Reuses Dynamic Cubemaps when available."));

	ImGui::TextWrapped("%s", T(TKEY("texture_install_help"), "Airborne Rain includes a default RGBA normal/opacity texture. To use your own without replacing it, install your texture here:"));
	ImGui::Indent();
	ImGui::TextUnformatted(kCustomRainTexturePath);
	ImGui::Unindent();
	ImGui::TextWrapped(T(TKEY("texture_source"), "Current texture source: %s"), settings.RainTexturePath.c_str());

	if (ImGui::Button(T(TKEY("use_default_texture"), "Use Default Texture")))
		SelectRainTexture(kDefaultRainTexturePath);
	ImGui::SameLine();
	const bool customTextureAvailable = Util::PathHelpers::SafeExists(kCustomRainTexturePath);
	ImGui::BeginDisabled(!customTextureAvailable);
	if (ImGui::Button(T(TKEY("use_custom_texture"), "Use Custom Texture")))
		SelectRainTexture(kCustomRainTexturePath);
	ImGui::EndDisabled();
	if (!customTextureAvailable)
		ImGui::TextDisabled("%s", T(TKEY("custom_texture_unavailable"), "No custom texture was found at the path above."));

	const auto effects11RainTexture = GetEffects11RainTexturePath();
	const bool effects11TextureAvailable = !effects11RainTexture.empty() && Util::PathHelpers::SafeExists(effects11RainTexture);
	ImGui::BeginDisabled(!effects11TextureAvailable);
	if (ImGui::Button(T(TKEY("load_effects11_texture"), "Load from Effects 11 Preset")))
		SelectRainTexture(effects11RainTexture);
	ImGui::EndDisabled();
	if (!effects11TextureAvailable)
		ImGui::TextDisabled("%s", T(TKEY("effects11_texture_unavailable"), "No active Effects 11 preset texture was found at enbseries\\enbraindrops.png."));

	if (rainTextureSRV)
		Util::Text::Success(T(TKEY("texture_loaded"), "Drop texture loaded (%.0f x %.0f)."), rainTextureSize.x, rainTextureSize.y);
	else if (rainTextureLoadAttempted)
		Util::Text::WrappedWarning(T(TKEY("rain_texture_unavailable"), "Drop texture not found at %s; using procedural rain."), settings.RainTexturePath.c_str());
	else
		ImGui::TextDisabled("%s", T(TKEY("texture_pending"), "The file is checked when textured rain next renders."));

	ImGui::BeginDisabled(!a_usesWaterMaterial || !settings.EnableTexturedRain);
	ImGui::SliderFloat(T(TKEY("texture_normals"), "Drop Curvature"), &settings.RainTextureNormalStrength, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, "%.2f");
	ImGui::SliderFloat(T(TKEY("texture_reflections"), "Water Reflection Strength"), &settings.RainTextureReflectionStrength, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, "%.2f");
	ImGui::SliderFloat(T(TKEY("texture_uv_width"), "Texture UV Width"), &settings.RainTextureUVWidth, kTextureUVWidthRange.minimum, kTextureUVWidthRange.maximum, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("texture_uv_width_tooltip"), "Samples the centered portion of the texture to trim transparent side padding. Use 1.0 for the round drop map; narrower values crop its curved edges."));
	ImGui::EndDisabled();
	if (ImGui::Button(T(TKEY("reload_rain_texture"), "Reload Drop Texture"))) {
		rainTextureSRV = nullptr;
		rainTextureLoadAttempted = false;
	}
	ImGui::TreePop();
}

void RainRendering::DrawWaterMaterialSettings()
{
	if (!ImGui::TreeNodeEx(T(TKEY("water_material"), "Water Material"), ImGuiTreeNodeFlags_DefaultOpen))
		return;

	if (ImGui::Button(T(TKEY("glassy_reference"), "Apply Glassy Reference Look")))
		ApplyGlassyReferenceSettings();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("glassy_reference_tooltip"), "Enables textured/refraction rain and tunes width, stretch, opacity, brightness, curvature and reflection for clearer water bodies. Keeps density, drop count, weather forcing and volume unchanged."));

	DrawFlagCheckbox(T(TKEY("glassy_rain"), "Glassy Rain"), settings.EnableGlassyRain);
	const bool usesWaterMaterial = UsesWaterMaterial();
	if (!settings.EnableGlassyRain)
		ImGui::TextDisabled("%s", T(TKEY("glassy_inactive"), "Material: standard streaks (Glassy Rain is off)"));
	else if (!usesWaterMaterial)
		ImGui::TextDisabled("%s", T(TKEY("glassy_debug_suspended"), "Material: diagnostic output replaces the water material in this debug mode"));
	else
		Util::Text::Success("%s", T(TKEY("glassy_active"), "Material: transparent water"));

	DrawTextureSettings(usesWaterMaterial);

	ImGui::BeginDisabled(!usesWaterMaterial);
	DrawFlagCheckbox(T(TKEY("refraction"), "Nearby Rain Refraction"), settings.EnableRainRefraction);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("refraction_tooltip"), "Adds a scene-color copy and depth-checked background distortion inside nearby world-space streaks. Each eye samples only its own view."));
	ImGui::BeginDisabled(!settings.EnableRainRefraction);
	ImGui::SliderFloat(T(TKEY("refraction_strength"), "Refraction Strength"), &settings.RainRefractionStrength, kUnitRange.minimum,
		kMaximumRefractionPixels, "%.2f pixels");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("refraction_strength_tooltip"), "Maximum background displacement inside a drop, also bounded by its projected width. Foreground depth and eye boundaries are always respected."));
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	if (usesWaterMaterial && settings.EnableRainRefraction && sceneColorCopyFailed)
		Util::Text::WrappedWarning("%s", T(TKEY("refraction_unavailable"), "Scene-color copy is unavailable; rain refraction is disabled."));

	if (ImGui::TreeNodeEx(T(TKEY("advanced_water_material"), "Advanced Water Material"))) {
		ImGui::BeginDisabled(!usesWaterMaterial);
		ImGui::SliderFloat(T(TKEY("environment_transmission"), "Environment Transmission"), &settings.RainEnvironmentTransmission, kUnitRange.minimum, kUnitRange.maximum, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("environment_transmission_tooltip"), "Cubemap transmission fills the portion not using nearby scene distortion. It does not reduce Scene Distortion Mix. Zero leaves that portion as clear background transmission."));
		if (settings.RainEnvironmentTransmission > 0.0f && !GetRainEnvironment())
			ImGui::TextDisabled("%s", T(TKEY("rain_environment_unavailable"), "Environment cubemap unavailable; using background transmission."));
		ImGui::SliderFloat(T(TKEY("core_darkening"), "Translucent Core Darkening"), &settings.RainCoreDarkening, kCoreDarkeningRange.minimum, kCoreDarkeningRange.maximum, "%.2f");
		ImGui::SliderFloat(T(TKEY("edge_highlight"), "Edge Highlight"), &settings.RainEdgeHighlight, kEdgeHighlightRange.minimum, kEdgeHighlightRange.maximum, "%.2f");
		ImGui::SliderFloat(T(TKEY("streak_variation"), "Streak Variation"), &settings.RainStreakVariation, kUnitRange.minimum, kUnitRange.maximum, "%.2f");
		ImGui::SliderFloat(T(TKEY("local_light_response"), "Local Light Response"), &settings.RainLocalLightResponse, kDoubleUnitRange.minimum, kDoubleUnitRange.maximum, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("local_light_response_tooltip"), "Reuses the light grid once per drop for colored scattering and an intensity-weighted highlight direction. Separate from cubemap reflections. Unshadowed approximation; spot and portal-restricted lights are excluded."));
		ImGui::SliderFloat(T(TKEY("highlight_roughness"), "Water Highlight Roughness"), &settings.RainHighlightRoughness, kHighlightRoughnessRange.minimum, kHighlightRoughnessRange.maximum, "%.2f");
		ImGui::SliderFloat(T(TKEY("light_scattering"), "Light Scattering"), &settings.RainLightScattering, kUnitRange.minimum, kUnitRange.maximum, "%.2f");
		ImGui::BeginDisabled(!settings.EnableRainRefraction);
		ImGui::SliderFloat(T(TKEY("scene_distortion_mix"), "Scene Distortion Mix"), &settings.RainSceneRefractionMix, kUnitRange.minimum, kUnitRange.maximum, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("scene_distortion_mix_tooltip"), "Transmission taken from the distorted scene inside nearby drops. One gives scene distortion priority over cubemap transmission; Rain Opacity controls the drop's coverage."));
		ImGui::SliderFloat(T(TKEY("refraction_distance"), "Glassy Detail Distance"), &settings.RainRefractionDistance, kRefractionDistanceRange.minimum, kRefractionDistanceRange.maximum, "%.0f units");
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::TreePop();
	}
	ImGui::TreePop();
}

void RainRendering::DrawSpatialVariationSettings()
{
	if (!ImGui::TreeNodeEx(T(TKEY("spatial_variation"), "Spatial Variation")))
		return;
	ImGui::SliderFloat(T(TKEY("density_noise_scale"), "Density Noise Scale"), &settings.RainDensityNoiseScale, kDensityNoiseScaleRange.minimum, kDensityNoiseScaleRange.maximum, "%.0f");
	ImGui::SliderFloat(T(TKEY("density_noise_strength"), "Density Noise Strength"), &settings.RainDensityNoiseStrength, kUnitRange.minimum, kUnitRange.maximum, "%.2f");
	ImGui::SliderFloat(T(TKEY("curtain_scale"), "Curtain Scale"), &settings.RainCurtainScale, kCurtainScaleRange.minimum, kCurtainScaleRange.maximum, "%.0f");
	ImGui::SliderFloat(T(TKEY("curtain_strength"), "Curtain Strength"), &settings.RainCurtainStrength, kUnitRange.minimum, kUnitRange.maximum, "%.2f");
	ImGui::SliderFloat(T(TKEY("curtain_contrast"), "Curtain Contrast"), &settings.RainCurtainContrast, kCurtainContrastRange.minimum, kCurtainContrastRange.maximum, "%.2f");
	ImGui::SliderFloat(T(TKEY("curtain_min_density"), "Curtain Minimum Density"), &settings.RainCurtainMinDensity, kCurtainMinimumDensityRange.minimum, kCurtainMinimumDensityRange.maximum, "%.2f");
	ImGui::SliderFloat(T(TKEY("curtain_max_density"), "Curtain Maximum Density"), &settings.RainCurtainMaxDensity, kCurtainMaximumDensityRange.minimum, kCurtainMaximumDensityRange.maximum, "%.2f");
	ImGui::TreePop();
}

void RainRendering::DrawOcclusionSettings()
{
	if (!ImGui::TreeNodeEx(T(TKEY("occlusion"), "Occlusion")))
		return;
	ImGui::SliderFloat(T(TKEY("intersection_fade"), "Geometry Intersection Fade"), &settings.RainIntersectionFadeDistance, kIntersectionFadeRange.minimum, kIntersectionFadeRange.maximum, "%.0f");
	DrawFlagCheckbox(T(TKEY("roof_occlusion"), "Skylighting Roof Occlusion"), settings.EnableRainRoofOcclusion);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("roof_occlusion_tooltip"), "Uses Skylighting's existing sky-visibility field to block rain beneath enclosing geometry."));
	if (!globals::features::skylighting.loaded)
		ImGui::TextDisabled("%s", T(TKEY("roof_occlusion_unavailable"), "Skylighting is unavailable; roof occlusion falls back to scene depth only."));

	ImGui::BeginDisabled(!settings.EnableRainRoofOcclusion);
	if (ImGui::SliderFloat(T(TKEY("roof_occlusion_fade_start"), "Roof Fade Start"), &settings.RainRoofOcclusionFadeStart, kRoofFadeStartRange.minimum, kRoofFadeStartRange.maximum, "%.2f"))
		NormalizeSettings();
	ImGui::SliderFloat(T(TKEY("roof_occlusion_fade_end"), "Roof Fade End"), &settings.RainRoofOcclusionFadeEnd,
		settings.RainRoofOcclusionFadeStart + 0.01f, kUnitRange.maximum, "%.2f");
	ImGui::EndDisabled();
	ImGui::TreePop();
}

void RainRendering::DrawDiagnosticsSettings()
{
	if (!ImGui::TreeNodeEx(T(TKEY("diagnostics"), "Diagnostics")))
		return;
	const char* debugModes[] = {
		T(TKEY("debug_off"), "Off"),
		T(TKEY("debug_positions"), "Drop Positions"),
		T(TKEY("debug_velocity"), "Drop Velocity"),
		T(TKEY("debug_density"), "Density Field"),
		T(TKEY("debug_curtains"), "Curtain Field"),
		T(TKEY("debug_lod"), "Distance LOD"),
		T(TKEY("debug_refraction"), "Actual Scene Distortion"),
		T(TKEY("debug_lighting"), "Local Light Contribution"),
		T(TKEY("debug_water_normals"), "Water Surface Normals")
	};
	int debugMode = static_cast<int>(settings.RainDebugMode);
	if (ImGui::Combo(T(TKEY("debug_mode"), "Debug Visualization"), &debugMode, debugModes, static_cast<int>(std::size(debugModes))))
		settings.RainDebugMode = static_cast<uint>(debugMode);
	if (settings.RainDebugMode >= 6)
		ImGui::TextWrapped("%s", T(TKEY("water_debug_help"), "Water diagnostics require Glassy Rain. Distortion: red = horizontal displacement, green = vertical; black = no effective distortion. Local Light Contribution excludes sunlight and cubemap reflections."));
	ImGui::TreePop();
}

void RainRendering::DrawSettings()
{
	NormalizeSettings();
	DrawGeneralSettings();
	DrawVolumeSettings();
	DrawMotionSettings();
	DrawAppearanceSettings();
	DrawWaterMaterialSettings();
	DrawSpatialVariationSettings();
	DrawOcclusionSettings();
	DrawDiagnosticsSettings();
	NormalizeSettings();
}

std::filesystem::path RainRendering::GetEffects11RainTexturePath() const
{
#if defined(ENABLE_EFFECTS11)
	const auto enbseriesPath = PresetManager::GetSingleton().GetENBSeriesPath();
	return enbseriesPath.empty() ? std::filesystem::path{} : enbseriesPath / "enbraindrops.png";
#else
	return {};
#endif
}

void RainRendering::SelectRainTexture(const std::filesystem::path& a_path)
{
	auto selectedPath = a_path;
	if (selectedPath.is_absolute()) {
		selectedPath = Util::PathHelpers::SafeRelative(selectedPath, Util::PathHelpers::GetDataPath().parent_path());
		if (selectedPath.is_absolute()) {
			logger::warn("[RainRendering] Refusing to persist texture path outside the game root: {}", a_path.string());
			return;
		}
	}
	settings.RainTexturePath = selectedPath.lexically_normal().string();
	rainTextureSRV = nullptr;
	rainTextureLoadAttempted = false;
	EnsureRainTexture();
}
