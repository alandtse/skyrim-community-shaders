#include "../Wind.h"

#include "Features/Wind/Trees/TreeWindPatcher.h"
#include "Features/Wind/Trees/TreeWindSettings.h"
#include "Features/Wind/WindMath.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/Format.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <random>
#include <string_view>

#define I18N_KEY_PREFIX "feature.wind."

using namespace WindSettingsLimits;
using WindMath::ClampFiniteOrDefault;

namespace
{
	constexpr uint32_t kGrassWindSpringTextureCount = 4;
	constexpr uint32_t kGrassWindSpringBytesPerPixel = 8;
	constexpr double kBytesPerMiB = 1024.0 * 1024.0;
	constexpr float kDebugWindSpawnRadius = 1000.0f;
	constexpr float kWorstCaseInnerRadius = 2000.0f;
	constexpr float kWorstCaseOuterRadius = 24000.0f;
	constexpr float kWorstCaseSourceDistance = 12000.0f;
	constexpr float kWorstCaseWaveHalfWidth = 600.0f;
	constexpr float kWorstCasePropagationSpeed = 1200.0f;
	constexpr float kWorstCaseDecayTime = 3.0f;
	constexpr float kWorstCaseConeCosine = 0.85f;
	constexpr float kGoldenAngleRadians = 2.39996323f;
	std::string CompactMeshPath(std::string_view a_path, float a_availableWidth)
	{
		const std::string fullPath(a_path);
		if (ImGui::CalcTextSize(fullPath.c_str()).x <= a_availableWidth)
			return fullPath;

		const auto fileSeparator = a_path.rfind('/');
		if (fileSeparator == std::string_view::npos)
			return fullPath;

		const std::string fileName(a_path.substr(fileSeparator + 1));
		if (ImGui::CalcTextSize(fileName.c_str()).x > a_availableWidth)
			return fileName;

		std::string compactPath = ".../" + fileName;
		auto suffixStart = fileSeparator;
		while (suffixStart > 0) {
			const auto previousSeparator = a_path.rfind('/', suffixStart - 1);
			if (previousSeparator == std::string_view::npos)
				break;
			const std::string candidate = "..." + std::string(a_path.substr(previousSeparator));
			if (ImGui::CalcTextSize(candidate.c_str()).x > a_availableWidth)
				break;
			compactPath = candidate;
			suffixStart = previousSeparator;
		}
		return compactPath;
	}
}

void Wind::DrawWindFieldSettings()
{
	ImGui::Checkbox(T(TKEY("visualize_wind_field"), "Visualize Wind Field"), &runtimeState.visualizeWindField);
	ImGui::TextWrapped("%s", T(TKEY("visualize_wind_field_tooltip"),
								 "Colors visible world geometry by GPU-sampled ambient gust pressure."));
	ImGui::Checkbox(T(TKEY("wind_field_use_real_speed"), "Wind Debug: Use Real Wind Speed"), &runtimeState.windFieldUseRealSpeed);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("wind_field_use_real_speed_tooltip"),
			"Use the current weather wind magnitude for air velocity and the base gust-front travel rate; otherwise use the override below."));
	}
	if (ImGui::SliderFloat(T(TKEY("wind_field_override_speed"), "Wind Debug: Override Speed"), &runtimeState.windFieldOverrideSpeed,
			0.0f, 2.0f, "%.3f"))
		runtimeState.windFieldUseRealSpeed = false;
	if (runtimeState.treeWindTest.enabled) {
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s",
			T(TKEY("tree_wind_test_active_notice"), "Tree Meshes runtime test conditions currently override wind speed and gust tuning."));
	}
	if (ImGui::Checkbox(T(TKEY("wind_field_use_real_direction"), "Wind Debug: Use Real Wind Direction"),
			&runtimeState.windFieldUseRealDirection) &&
		!runtimeState.windFieldUseRealDirection) {
		const auto* state = globals::state;
		const float currentDirectionDegrees = state ?
		                                          std::atan2(state->windFieldCurrent.direction.y, state->windFieldCurrent.direction.x) *
		                                              (180.0f / 3.14159265358979323846f) :
		                                          0.0f;
		runtimeState.windFieldPendingDirectionDegrees = currentDirectionDegrees;
		runtimeState.windFieldAppliedDirectionDegrees = currentDirectionDegrees;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("wind_field_use_real_direction_tooltip"),
			"Use the current weather wind direction. Disable this to stage and apply a manual field direction."));
	}
	ImGui::SliderFloat(T(TKEY("wind_field_target_direction"), "Target Direction"),
		&runtimeState.windFieldPendingDirectionDegrees, -180.0f, 180.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
	if (ImGui::Button(T(TKEY("wind_field_apply_direction"), "Apply Direction"))) {
		runtimeState.windFieldAppliedDirectionDegrees = runtimeState.windFieldPendingDirectionDegrees;
		runtimeState.windFieldUseRealDirection = false;
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("wind_field_random_direction"), "New Random Wind Direction"))) {
		static std::mt19937 randomGenerator{ std::random_device{}() };
		static std::uniform_real_distribution<float> randomDirectionDegrees(-180.0f, 180.0f);
		runtimeState.windFieldPendingDirectionDegrees = randomDirectionDegrees(randomGenerator);
		runtimeState.windFieldAppliedDirectionDegrees = runtimeState.windFieldPendingDirectionDegrees;
		runtimeState.windFieldUseRealDirection = false;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("wind_field_random_direction_tooltip"),
			"Choose and apply a new random fixed direction immediately."));
	ImGui::SliderFloat(T(TKEY("wind_field_transition_duration"), "Direction Blend Period"),
		&settings.windFieldDirectionTransitionDuration, kWindFieldDirectionTransitionDurationMin,
		kWindFieldDirectionTransitionDurationMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("wind_field_transition_duration_tooltip"),
			"A direction change creates a new, fixed-orientation noise field and lerps the old field into it over this period."));
	static constexpr const char* debugViewLabels[]{ "Blended Output", "Current Field", "Previous Field", "Both Fields", "Comparison", "Spring Field", "Transient Impulses" };
	int debugView = static_cast<int>(runtimeState.windFieldDebugView);
	if (ImGui::Combo(T(TKEY("wind_field_debug_view"), "Wind Debug View"), &debugView, debugViewLabels,
			static_cast<int>(std::size(debugViewLabels)))) {
		runtimeState.windFieldDebugView = static_cast<WindFieldDebugView>(debugView);
		runtimeState.visualizeWindField = true;
	}
	ImGui::SeparatorText(T(TKEY("wind_field_live_values"), "Wind Field Live Values"));
	auto* const state = globals::state;
	const auto* sky = globals::game::sky;
	const auto* weather = sky ? sky->currentWeather : nullptr;
	const float ambientDirectionLength = std::hypot(state->ambientWindVelocity.x, state->ambientWindVelocity.y);
	const float ambientDirectionDegrees = ambientDirectionLength > 0.0001f ?
	                                          std::atan2(state->ambientWindVelocity.y, state->ambientWindVelocity.x) * (180.0f / 3.14159265358979323846f) :
	                                          0.0f;
	ImGui::Text("%s: (%.5f, %.5f, %.5f)", T(TKEY("wind_field_ambient_velocity"), "Ambient velocity"),
		state->ambientWindVelocity.x, state->ambientWindVelocity.y, state->ambientWindVelocity.z);
	ImGui::Text("%s: %.5f", T(TKEY("wind_field_ambient_speed"), "Selected ambient speed"), state->windFieldAmbientSpeed);
	ImGui::Text("%s: %.2f deg", T(TKEY("wind_field_ambient_direction"), "Ambient direction"), ambientDirectionDegrees);
	ImGui::Text("%s: %.6f s", T(TKEY("wind_field_frame_time"), "Frame delta"), state->windFieldFrameTime);
	ImGui::Text("%s: %.6f", T(TKEY("wind_field_travel_delta"), "Travel delta"), state->windFieldTravelDelta);
	ImGui::Text("%s: %.5f", T(TKEY("wind_field_travel_distance"), "Accumulated travel distance"), state->windFieldGustTravelDistance);
	ImGui::Text("%s: %.3f units/s", T(TKEY("wind_field_advection_speed"), "Gust advection speed"), state->windFieldAdvectionSpeed);
	ImGui::Text("%s: %.5f", T(TKEY("wind_field_global_time"), "Global timer"), state->timer);
	ImGui::Text("Current field: direction (%.3f, %.3f), travel %.1f", state->windFieldCurrent.direction.x,
		state->windFieldCurrent.direction.y, state->windFieldCurrent.travelDistance);
	if (state->windFieldTransitionActive) {
		ImGui::Text("Previous field: direction (%.3f, %.3f), travel %.1f", state->windFieldTransition.direction.x,
			state->windFieldTransition.direction.y, state->windFieldTransition.travelDistance);
		ImGui::Text("Transition: %.0f%%", state->windFieldTransitionBlend * 100.0f);
	}
	if (sky)
		ImGui::Text("%s: speed %.5f, angle %.5f", T(TKEY("wind_field_sky_input"), "Sky wind input"), sky->windSpeed, sky->windAngle);
	if (weather)
		ImGui::Text("%s: %.3f (raw %u), direction raw %u", T(TKEY("wind_field_weather_input"), "Weather input"),
			static_cast<unsigned>(weather->data.windSpeed) / 255.0f, static_cast<unsigned>(weather->data.windSpeed),
			static_cast<unsigned>(weather->data.windDirection));
	const float selectedSpeed = state->windFieldSelectedSpeed;
	const float appliedDirectionRadians = DirectX::XMConvertToRadians(runtimeState.windFieldAppliedDirectionDegrees);
	const float selectedDirectionX = runtimeState.windFieldUseRealDirection && ambientDirectionLength > 0.0001f ?
	                                     state->ambientWindVelocity.x / ambientDirectionLength :
	                                     std::cos(appliedDirectionRadians);
	const float selectedDirectionY = runtimeState.windFieldUseRealDirection && ambientDirectionLength > 0.0001f ?
	                                     state->ambientWindVelocity.y / ambientDirectionLength :
	                                     std::sin(appliedDirectionRadians);
	const float weatherWindSpeed = std::sqrt(
		state->ambientWindVelocity.x * state->ambientWindVelocity.x +
		state->ambientWindVelocity.y * state->ambientWindVelocity.y +
		state->ambientWindVelocity.z * state->ambientWindVelocity.z);
	float localWindSpeed = state->windFieldSelectedSpeed;
	float ambientGust = 0.0f;
	ImGui::Text("%s: speed %.5f, direction (%.5f, %.5f, 0.00000)",
		T(TKEY("wind_field_selected_input"), "Selected sampler input"), selectedSpeed, selectedDirectionX, selectedDirectionY);
	ImGui::TextWrapped("%s", T(TKEY("wind_field_color_note"),
								 "The debug color represents normalized ambient gust pressure. Advection controls transport independently from gust amplitude."));
	if (globals::game::shadowState) {
		const auto eyePosition = Util::GetAverageEyePosition();
		const float3 samplePosition{ eyePosition.x, eyePosition.y, eyePosition.z };
		const float rawAmbientSpeed = std::sqrt(
			state->ambientWindVelocity.x * state->ambientWindVelocity.x +
			state->ambientWindVelocity.y * state->ambientWindVelocity.y +
			state->ambientWindVelocity.z * state->ambientWindVelocity.z);
		const auto eyeSample = state->SampleWind(samplePosition, state->ambientWindVelocity, rawAmbientSpeed);
		const auto selectedSample = state->SampleWind(samplePosition);
		localWindSpeed = std::sqrt(
			selectedSample.velocity.x * selectedSample.velocity.x +
			selectedSample.velocity.y * selectedSample.velocity.y +
			selectedSample.velocity.z * selectedSample.velocity.z);
		ambientGust = selectedSample.ambientGust;
		ImGui::Text("%s: (%.5f, %.5f, %.5f), gust %.5f",
			T(TKEY("wind_field_cpu_sample"), "CPU sample at camera"), eyeSample.velocity.x, eyeSample.velocity.y,
			eyeSample.velocity.z, eyeSample.ambientGust);
		ImGui::Text("%s: (%.5f, %.5f, %.5f), gust %.5f",
			T(TKEY("wind_field_selected_sample"), "Selected sample at camera"), selectedSample.velocity.x,
			selectedSample.velocity.y, selectedSample.velocity.z, selectedSample.ambientGust);
	}
	ImGui::SeparatorText(T(TKEY("wind_field_readout"), "Field Readout"));
	ImGui::Text("%s: %.2f", T(TKEY("wind_field_weather_wind"), "Weather Wind"), weatherWindSpeed);
	ImGui::Text("%s: %.2f", T(TKEY("wind_field_local_wind"), "Local Wind"), localWindSpeed);
	ImGui::Text("%s: %+.2f", T(TKEY("wind_field_ambient_gust"), "Ambient Gust"), ambientGust);
	ImGui::Text("%s: %.2f units/s", T(TKEY("wind_field_advection_speed_readout"), "Advection Speed"),
		state->windFieldAdvectionSpeed);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("wind_field_readout_tooltip"),
			"Weather Wind is the raw ambient input. Local Wind is the canonical sampled velocity magnitude at the camera. Ambient Gust is the normalized field value used to modulate it."));
	ImGui::SeparatorText(T(TKEY("wind_field_profile"), "Profile"));
	ImGui::Text("%s: %.2f", T(TKEY("wind_field_profile_amplitude"), "Amplitude"), settings.windFieldGustAmplitude);
	ImGui::Text("%s: %.0f / %.0f", T(TKEY("wind_field_profile_scale"), "Along-wind / crosswind scale"),
		settings.windFieldGustScale, settings.windFieldGustCrosswindScale);
	ImGui::Text("%s: %.2fx", T(TKEY("wind_field_profile_advection"), "Advection"), settings.windFieldGustAdvectionMultiplier);
	if (ImGui::TreeNodeEx(T(TKEY("wind_field_tuning"), "Sampler tuning"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("wind_field_gust_advection_multiplier"), "Gust Advection Multiplier"),
			&settings.windFieldGustAdvectionMultiplier, kWindFieldGustAdvectionMultiplierMin,
			kWindFieldGustAdvectionMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_field_gust_advection_multiplier_tooltip"),
				"Changes only how quickly gust structures move through world space; it does not increase local air velocity."));
		ImGui::SliderFloat(T(TKEY("wind_field_gust_scale"), "Gust Along-Wind Scale"), &settings.windFieldGustScale,
			kWindFieldGustScaleMin, kWindFieldGustScaleMax, "%.0f units",
			ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_field_gust_scale_tooltip"),
				"Controls gust thickness along the wind direction."));
		ImGui::SliderFloat(T(TKEY("wind_field_gust_crosswind_scale"), "Gust Crosswind Scale"),
			&settings.windFieldGustCrosswindScale, kWindFieldGustCrosswindScaleMin,
			kWindFieldGustCrosswindScaleMax, "%.0f units",
			ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_field_gust_crosswind_scale_tooltip"),
				"Controls gust length across the wind direction; lower values produce shorter, more compact blobs."));
		ImGui::SliderFloat(T(TKEY("wind_field_gust_amplitude"), "Gust Amplitude"), &settings.windFieldGustAmplitude,
			kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_field_gust_amplitude_tooltip"),
				"Fractional velocity deviation around the mean; 0.35 produces a 0.65x to 1.35x range."));
		const auto& tuning = state->windFieldTuning;
		ImGui::Text("Base advection %.2f units/s at wind speed 1.0, front aspect %.2f",
			tuning.gustAdvectionBaseSpeed, tuning.frontAspectRatio);
		ImGui::Text("Detail ratios %.3f / %.3f, turbulence %.3f, skew %.3f", tuning.detailScaleRatio,
			tuning.detailCrosswindScaleRatio, tuning.turbulenceStrength, tuning.turbulenceSkew);
		ImGui::Text("Contrast %.3f - %.3f", tuning.contrastLow, tuning.contrastHigh);
		ImGui::Text("Seeds: broad 0x%08X, detail 0x%08X, mix 0x%08X", tuning.broadGustSeed,
			tuning.turbulentGustSeed, tuning.gradientSeedMix);
		ImGui::Text("PCG: multiplier %u, increment %u", tuning.pcgMultiplier, tuning.pcgIncrement);
		ImGui::TreePop();
	}
	ImGui::Separator();
}

void Wind::DrawTreeSettings()
{
	if (!ImGui::BeginTabItem(T(TKEY("tab_trees"), "Trees")))
		return;

	uiState.activeSettingsPage = SettingsPage::Trees;
	ImGui::Checkbox(T(TKEY("enable_trunk_bend"), "Enable Trunk Bend"), &settings.enableTrunkBend);
	ImGui::SeparatorText(T(TKEY("trunk_wind_response"), "Tree Response"));
	ImGui::SliderFloat(T(TKEY("trunk_wind_bend_sensitivity"), "Trunk Wind Sensitivity"), &settings.trunkWindBendSensitivity,
		kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::TextWrapped("%s", T(TKEY("tree_per_mesh_response_note"),
								 "Transient, displacement, and model response values are configured per tree in WindSettings JSONs."));
	ImGui::SliderFloat(T(TKEY("tree_wind_gust_scale"), "Tree Gust Scale"), &settings.treeWindGustScale,
		kTreeWindGustScaleMin, kTreeWindGustScaleMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("tree_wind_gust_scale_tooltip"),
			"Scales gust variation only for trees. Grass keeps the full shared gust amplitude."));
	ImGui::SliderFloat(T(TKEY("tree_wind_gust_soft_limit"), "Tree Gust Soft Limit"), &settings.treeWindGustSoftLimit,
		kTreeWindGustSoftLimitMin, kTreeWindGustSoftLimitMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("tree_wind_gust_soft_limit_tooltip"),
			"Smoothly compresses only the strongest tree gust peaks instead of clipping them. Set to 0 to disable."));
	ImGui::SliderFloat(T(TKEY("tree_wind_spring_frequency"), "Tree Spring Frequency"), &settings.treeWindSpringFrequency,
		kTreeWindSpringFrequencyMin, kTreeWindSpringFrequencyMax, "%.2f Hz",
		ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat(T(TKEY("tree_wind_spring_damping"), "Tree Spring Damping"), &settings.treeWindSpringDamping,
		kTreeWindSpringDampingMin, kTreeWindSpringDampingMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("tree_transient_spring_frequency"), "Transient Rebound Frequency"),
		&settings.treeTransientSpringFrequency, kTreeTransientSpringFrequencyMin,
		kTreeTransientSpringFrequencyMax, "%.2f Hz",
		ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat(T(TKEY("tree_transient_spring_damping"), "Transient Rebound Damping"),
		&settings.treeTransientSpringDamping, kTreeTransientSpringDampingMin,
		kTreeTransientSpringDampingMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SeparatorText(T(TKEY("tree_leaf_flutter"), "Leaf Flutter"));
	ImGui::SliderFloat(T(TKEY("tree_leaf_base_wind_flutter_gain"), "Base Wind Flutter Gain"),
		&settings.treeLeafBaseWindFlutterGain, kTreeLeafBaseWindFlutterGainMin,
		kTreeLeafBaseWindFlutterGainMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("tree_leaf_base_wind_flutter_gain_tooltip"),
			"Controls the direct leaf animation strength driven by shared base wind. Gust influence adds only local amplitude variation."));
	}
	ImGui::EndTabItem();
}

void Wind::DrawSettings()
{
	if (ImGui::BeginTabBar("##WindTabs", ImGuiTabBarFlags_None)) {
		if (globals::state->IsDeveloperMode() && ImGui::BeginTabItem(T(TKEY("tab_wind_field"), "Wind Field"))) {
			uiState.activeSettingsPage = SettingsPage::WindField;
			DrawWindFieldSettings();
			ImGui::EndTabItem();
		}

		DrawWindEffectsSettings();
		DrawTreeSettings();
		DrawTreeMeshSettings();
		DrawGrassWindSettings();

		ImGui::EndTabBar();
	}
}

void Wind::DrawWindEffectsSettings()
{
	if (!ImGui::BeginTabItem(T(TKEY("tab_wind_effects"), "Wind Effects")))
		return;

	uiState.activeSettingsPage = SettingsPage::WindEffects;
	ImGui::SeparatorText(T(TKEY("transient_field_coverage"), "Transient Field Coverage"));
	ImGui::Checkbox(T(TKEY("process_mid_range_transients"), "Process Mid Transients"),
		&settings.processMidRangeTransients);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("process_mid_range_transients_tooltip"),
			"Processes transient sources in the Mid tree and grass spring tiers. Ambient wind and gusts are unaffected."));
	ImGui::Checkbox(T(TKEY("process_far_range_transients"), "Process Far Transients"),
		&settings.processFarRangeTransients);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("process_far_range_transients_tooltip"),
			"Processes transient sources in the Far tree and grass spring tiers. Ambient wind and gusts are unaffected."));
	if (globals::state->IsDeveloperMode()) {
		ImGui::SeparatorText(T(TKEY("debug_wind_effects"), "Performance Test"));
		ImGui::Checkbox(T(TKEY("debug_wind_effect_worst_case"), "Worst-case coverage"),
			&uiState.debugWindEffectWorstCase);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("debug_wind_effect_worst_case_tooltip"),
				"Distributes broad, long-lived directional waves across the Near, Mid, and Far fields. This can be much harsher than normal gameplay."));
		ImGui::SliderInt(T(TKEY("debug_wind_effect_spawn_count"), "How many wind effects to spawn"),
			&uiState.debugWindEffectSpawnCount, 1, static_cast<int>(WindField::kTransientImpulseCapacity), "%d",
			ImGuiSliderFlags_AlwaysClamp);
		if (ImGui::Button(T(TKEY("debug_wind_effect_spawn"), "Spawn")))
			SpawnDebugWindEffects();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("debug_wind_effect_spawn_tooltip"),
				"Replaces active impulses with the selected performance-test pattern and enables transient visualization."));
	}
	ImGui::SeparatorText(T(TKEY("configured_wind_effects"), "Configured Wind Effects"));
	if (windEffects.empty()) {
		ImGui::TextUnformatted(T(TKEY("no_wind_effects"), "No wind effects are registered."));
	} else if (ImGui::BeginTabBar("##WindEffectTabs", ImGuiTabBarFlags_None)) {
		for (std::size_t index = 0; index < windEffects.size(); ++index) {
			auto& effect = windEffects[index];
			const auto effectId = effect->GetId();
			const auto displayName = effect->GetDisplayName();
			ImGui::PushID(effectId.data(), effectId.data() + effectId.size());
			if (ImGui::BeginTabItem(displayName.c_str())) {
				uiState.activeWindEffectIndex = index;
				effect->DrawSettings();
				ImGui::EndTabItem();
			}
			ImGui::PopID();
		}
		ImGui::EndTabBar();
	}

	ImGui::EndTabItem();
}

void Wind::SpawnDebugWindEffects()
{
	auto* const state = globals::state;
	auto* const player = globals::game::player;
	if (!state || !player)
		return;

	static std::mt19937 randomGenerator{ std::random_device{}() };
	static std::uniform_real_distribution<float> unitDistribution(-1.0f, 1.0f);
	static std::uniform_real_distribution<float> strengthDistribution(1.25f, 2.5f);
	static std::uniform_real_distribution<float> distanceDistribution(450.0f, 800.0f);
	static std::uniform_real_distribution<float> widthDistribution(90.0f, 180.0f);
	static std::uniform_real_distribution<float> speedDistribution(650.0f, 1100.0f);
	static std::uniform_real_distribution<float> decayDistribution(0.35f, 0.75f);

	const auto playerPosition = player->GetPosition();
	const int spawnCount = std::clamp(uiState.debugWindEffectSpawnCount, 1,
		static_cast<int>(WindField::kTransientImpulseCapacity));
	state->ClearTransientWindImpulses();
	if (uiState.debugWindEffectWorstCase) {
		const float innerRadiusSquared = kWorstCaseInnerRadius * kWorstCaseInnerRadius;
		const float outerRadiusSquared = kWorstCaseOuterRadius * kWorstCaseOuterRadius;
		for (int index = 0; index < spawnCount; ++index) {
			const float distribution = (static_cast<float>(index) + 0.5f) / static_cast<float>(spawnCount);
			const float radius = std::sqrt(std::lerp(innerRadiusSquared, outerRadiusSquared, distribution));
			const float angle = static_cast<float>(index) * kGoldenAngleRadians;
			const float cosine = std::cos(angle);
			const float sine = std::sin(angle);
			const float heightOffset = static_cast<float>(index % 5 - 2) * 350.0f;
			const float3 origin{ playerPosition.x + cosine * radius, playerPosition.y + sine * radius,
				playerPosition.z + heightOffset };
			const float3 direction{ -cosine, -sine, -heightOffset / kWorstCaseSourceDistance };
			state->QueueTransientWindImpulse(WindField::MakeDirectionalWave(origin, direction, 2.5f,
				kWorstCaseSourceDistance, kWorstCaseWaveHalfWidth, kWorstCasePropagationSpeed,
				kWorstCaseConeCosine, kWorstCaseDecayTime));
		}
	} else {
		for (int index = 0; index < spawnCount; ++index) {
			float3 offset;
			do {
				offset = { unitDistribution(randomGenerator), unitDistribution(randomGenerator),
					unitDistribution(randomGenerator) };
			} while (offset.x * offset.x + offset.y * offset.y + offset.z * offset.z > 1.0f);
			offset *= kDebugWindSpawnRadius;
			const float3 origin{ playerPosition.x + offset.x, playerPosition.y + offset.y,
				playerPosition.z + offset.z };
			state->QueueTransientWindImpulse({ origin,
				0.0f,
				{},
				strengthDistribution(randomGenerator),
				distanceDistribution(randomGenerator),
				widthDistribution(randomGenerator),
				speedDistribution(randomGenerator),
				0.0f,
				decayDistribution(randomGenerator),
				0.0f,
				0.0f,
				0.0f });
		}
	}
	runtimeState.windFieldDebugView = WindFieldDebugView::TransientImpulses;
	runtimeState.visualizeWindField = true;
}

void Wind::DrawTreeWindTestSettings()
{
	if (!runtimeState.treeWindTest.enabled) {
		if (const auto* state = globals::state)
			runtimeState.treeWindTest.speed = ClampFiniteOrDefault(state->windFieldSelectedSpeed, 0.0f, 2.0f, 1.0f);
		runtimeState.treeWindTest.gustScale = settings.windFieldGustScale;
		runtimeState.treeWindTest.gustAmplitude = settings.windFieldGustAmplitude;
		runtimeState.treeWindTest.gustAdvectionMultiplier = settings.windFieldGustAdvectionMultiplier;
	}

	if (!ImGui::TreeNodeEx(T(TKEY("tree_wind_test_conditions"), "Test Wind Conditions"), ImGuiTreeNodeFlags_DefaultOpen))
		return;

	bool testEnabled = runtimeState.treeWindTest.enabled;
	if (ImGui::Checkbox(T(TKEY("tree_wind_test_enable"), "Override Wind for Testing"), &testEnabled))
		SetTreeWindTestEnabled(testEnabled);
	ImGui::TextWrapped("%s", T(TKEY("tree_wind_test_runtime_note"),
								 "Runtime only. Disable this override to immediately return control to weather and the Wind Field settings."));

	ImGui::BeginDisabled(!runtimeState.treeWindTest.enabled);
	ImGui::SliderFloat(T(TKEY("tree_wind_test_speed"), "Wind Speed"), &runtimeState.treeWindTest.speed, 0.0f, 2.0f, "%.3f",
		ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("tree_wind_test_advection"), "Gust Advection Multiplier"),
		&runtimeState.treeWindTest.gustAdvectionMultiplier, kWindFieldGustAdvectionMultiplierMin,
		kWindFieldGustAdvectionMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("tree_wind_test_scale"), "Gust Spatial Scale"), &runtimeState.treeWindTest.gustScale,
		kWindFieldGustScaleMin, kWindFieldGustScaleMax, "%.0f units",
		ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat(T(TKEY("tree_wind_test_amplitude"), "Gust Amplitude"), &runtimeState.treeWindTest.gustAmplitude,
		kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::TreePop();
}

void Wind::DrawTreeMeshSettings()
{
	if (!ImGui::BeginTabItem(T(TKEY("tab_tree_meshes"), "Tree Meshes")))
		return;

	uiState.activeSettingsPage = SettingsPage::TreeMeshes;
	if (globals::state->IsDeveloperMode())
		DrawTreeWindTestSettings();
	DrawTreeMeshConflictWarning();
	if (globals::state->IsDeveloperMode())
		DrawTreeGlobalOverrideSettings();
	DrawTreeMeshRuleControls();
	DrawTreeMeshRulesTable();
	ImGui::EndTabItem();
}

void Wind::DrawTreeMeshConflictWarning()
{
	const auto conflicts = TreeWindPatcher::GetConflictingFiles();
	if (!conflicts.empty()) {
		std::string fileList;
		for (const auto& file : conflicts) {
			if (!fileList.empty())
				fileList += ", ";
			fileList += file;
		}
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f), "%s: %s",
			T(TKEY("tree_response_conflict_warning"),
				"Duplicate tree responses found, trees may not behave as intended"),
			fileList.c_str());
	}
}

void Wind::DrawTreeGlobalOverrideSettings()
{
	auto [universalOverrideEnabled, universalValues] = TreeWindPatcher::GetUniversalOverride();
	if (ImGui::TreeNodeEx(T(TKEY("tree_global_override"), "Global Override"), ImGuiTreeNodeFlags_DefaultOpen)) {
		bool overrideChanged = ImGui::Checkbox(
			T(TKEY("tree_global_override_enabled"), "Enable Global Override"),
			&universalOverrideEnabled);
		ImGui::TextWrapped("%s", T(TKEY("tree_global_override_note"),
									 "Runtime only. While enabled, every value below overrides the matching response for all trees, including all custom responses loaded from WindSettings JSONs. Disable it to immediately restore each tree's custom or coded-default response."));
		ImGui::BeginDisabled(!universalOverrideEnabled);
		overrideChanged |= ImGui::SliderFloat(T(TKEY("tree_override_bend"), "Bend Sensitivity"),
			&universalValues.bend, TreeWindSettings::kBend.minimum, TreeWindSettings::kBend.maximum,
			"%.2f", ImGuiSliderFlags_AlwaysClamp);
		overrideChanged |= ImGui::SliderFloat(T(TKEY("tree_override_leaf"), "Leaf Ambient Sensitivity"),
			&universalValues.leafAmbient, TreeWindSettings::kLeafAmbient.minimum,
			TreeWindSettings::kLeafAmbient.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		overrideChanged |= ImGui::SliderFloat(T(TKEY("tree_override_upper_bend"), "Upper Bend Range (%)"),
			&universalValues.upperBendRange, TreeWindSettings::kUpperBendPercent.minimum,
			TreeWindSettings::kUpperBendPercent.maximum, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
		overrideChanged |= ImGui::SliderFloat(T(TKEY("tree_override_displacement"), "Maximum Displacement (%)"),
			&universalValues.maximumDisplacementPercent, TreeWindSettings::kMaximumDisplacementPercent.minimum,
			TreeWindSettings::kMaximumDisplacementPercent.maximum, "%.2f%%", ImGuiSliderFlags_AlwaysClamp);
		overrideChanged |= ImGui::SliderFloat(T(TKEY("tree_override_transient_influence"), "Trunk Transient Wind Influence"),
			&universalValues.transientWindInfluence, TreeWindSettings::kTransientInfluence.minimum,
			TreeWindSettings::kTransientInfluence.maximum, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		overrideChanged |= ImGui::SliderFloat(T(TKEY("tree_override_leaf_transient_influence"), "Leaf Transient Wind Influence"),
			&universalValues.leafTransientWindInfluence, TreeWindSettings::kTransientInfluence.minimum,
			TreeWindSettings::kTransientInfluence.maximum, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		overrideChanged |= ImGui::SliderFloat(T(TKEY("tree_override_transient_flutter_cap"), "Transient Flutter Cap"),
			&universalValues.leafTransientFlutterMaximum, TreeWindSettings::kLeafTransientFlutterMaximum.minimum,
			TreeWindSettings::kLeafTransientFlutterMaximum.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		overrideChanged |= ImGui::SliderFloat(T(TKEY("tree_override_transient_bend"), "Transient Maximum Bend Multiplier"),
			&universalValues.transientMaximumBendMultiplier,
			TreeWindSettings::kTransientMaximumBendMultiplier.minimum,
			TreeWindSettings::kTransientMaximumBendMultiplier.maximum, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndDisabled();
		if (overrideChanged)
			TreeWindPatcher::SetUniversalOverride(universalOverrideEnabled, universalValues);

		if (!uiState.treeWindSaveStatus.empty()) {
			const ImVec4 statusColor = uiState.treeWindSaveSucceeded ?
			                               ImVec4(0.45f, 0.85f, 0.45f, 1.0f) :
			                               ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
			ImGui::TextColored(statusColor, "%s", uiState.treeWindSaveStatus.c_str());
		}
		ImGui::TreePop();
	}
}

void Wind::DrawTreeMeshRuleControls()
{
	const std::size_t ruleCount = TreeWindPatcher::GetRuleCount();
	const std::size_t unsavedCount = TreeWindPatcher::GetUnsavedRuleCount();
	ImGui::TextWrapped("%s", T(TKEY("tree_mesh_live_note"),
								 "Changes apply immediately to every loaded instance of the selected mesh. Save writes only changed values to the last-scanned JSON containing that tree."));
	ImGui::Text("%s: %zu    %s: %zu", T(TKEY("tree_mesh_rule_count"), "Meshes"), ruleCount,
		T(TKEY("tree_mesh_unsaved_count"), "Unsaved"), unsavedCount);

	ImGui::SetNextItemWidth(-1.0f);
	const bool searchChanged = ImGui::InputTextWithHint("##TreeMeshSearch",
		T(TKEY("tree_mesh_search_hint"), "Search mesh paths..."), uiState.treeMeshSearch.data(), uiState.treeMeshSearch.size());
	const std::string normalizedSearch = Util::FixFilePath(std::string(uiState.treeMeshSearch.data()));
	if (searchChanged || uiState.filteredTreeRuleCount != ruleCount || uiState.appliedTreeMeshSearch != normalizedSearch) {
		uiState.filteredTreeRuleIndices.clear();
		uiState.filteredTreeRuleIndices.reserve(ruleCount);
		for (std::size_t index = 0; index < ruleCount; ++index) {
			const auto rule = TreeWindPatcher::GetRule(index);
			if (normalizedSearch.empty() || rule.mesh.find(normalizedSearch) != std::string_view::npos)
				uiState.filteredTreeRuleIndices.push_back(index);
		}
		uiState.filteredTreeRuleCount = ruleCount;
		uiState.appliedTreeMeshSearch = normalizedSearch;
	}

	ImGui::BeginDisabled(unsavedCount == 0);
	if (ImGui::Button(T(TKEY("tree_mesh_save"), "Save JSON"))) {
		const auto result = TreeWindPatcher::SaveRules();
		uiState.treeWindSaveSucceeded = result.success;
		uiState.treeWindSaveStatus = result.success ?
		                                 std::format("Saved {} meshes to {}", result.savedRuleCount, result.path) :
		                                 std::format("Save failed: {}", result.error);
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("tree_mesh_revert"), "Revert Unsaved"))) {
		TreeWindPatcher::RevertUnsavedChanges();
		uiState.treeWindSaveStatus.clear();
	}
	ImGui::EndDisabled();

	if (!uiState.treeWindSaveStatus.empty()) {
		const ImVec4 statusColor = uiState.treeWindSaveSucceeded ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
		ImGui::TextColored(statusColor, "%s", uiState.treeWindSaveStatus.c_str());
	}
}

void Wind::DrawTreeMeshRulesTable()
{
	ImGui::Text("%s: %zu", T(TKEY("tree_mesh_search_results"), "Matches"), uiState.filteredTreeRuleIndices.size());
	const auto& style = ImGui::GetStyle();
	const float tableBottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - style.WindowPadding.y - style.ItemSpacing.y;
	const float visibleTableHeight = tableBottom - ImGui::GetCursorScreenPos().y;
	const float minimumTableHeight = ImGui::GetTextLineHeightWithSpacing() * 5.0f;
	const float tableHeight = std::max(visibleTableHeight, minimumTableHeight);
	const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
	                                   ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("##TreeMeshRules", 9, tableFlags, ImVec2(0.0f, tableHeight), ImGui::GetFontSize() * 80.0f)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_path"), "Mesh"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_bend"), "Bend"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_leaf"), "Leaf Flutter"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_upper_bend_range"), "Upper Bend"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_maximum_displacement"), "Top Displacement"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_transient_influence"), "Trunk Transient"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_leaf_transient_influence"), "Leaf Transient"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_transient_flutter_cap"), "Flutter Cap"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableSetupColumn(T(TKEY("tree_mesh_transient_bend"), "Transient Bend"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0f);
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(uiState.filteredTreeRuleIndices.size()));
		while (clipper.Step()) {
			for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
				const auto ruleIndex = uiState.filteredTreeRuleIndices[static_cast<std::size_t>(visibleIndex)];
				const auto rule = TreeWindPatcher::GetRule(ruleIndex);
				float bend = rule.bend;
				float leafAmbient = rule.leafAmbient;
				float upperBendRange = rule.upperBendRange;
				float maximumDisplacementPercent = rule.maximumDisplacementPercent;
				float transientWindInfluence = rule.transientWindInfluence;
				float leafTransientWindInfluence = rule.leafTransientWindInfluence;
				float leafTransientFlutterMaximum = rule.leafTransientFlutterMaximum;
				float transientMaximumBendMultiplier = rule.transientMaximumBendMultiplier;
				bool changed = false;

				ImGui::PushID(static_cast<int>(rule.id));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const std::string compactPath = CompactMeshPath(rule.mesh, ImGui::GetContentRegionAvail().x);
				ImGui::TextUnformatted(compactPath.c_str());
				if (compactPath != rule.mesh && ImGui::IsItemHovered())
					ImGui::SetTooltip("%.*s", static_cast<int>(rule.mesh.size()), rule.mesh.data());
				ImGui::TableSetColumnIndex(1);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##Bend", &bend, TreeWindSettings::kBend.minimum,
					TreeWindSettings::kBend.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(2);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##Leaf", &leafAmbient, TreeWindSettings::kLeafAmbient.minimum,
					TreeWindSettings::kLeafAmbient.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(3);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##UpperBend", &upperBendRange,
					TreeWindSettings::kUpperBendPercent.minimum, TreeWindSettings::kUpperBendPercent.maximum,
					"%.0f%%", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(4);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##MaximumDisplacement", &maximumDisplacementPercent,
					TreeWindSettings::kMaximumDisplacementPercent.minimum,
					TreeWindSettings::kMaximumDisplacementPercent.maximum, "%.2f%%",
					ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(5);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##TrunkTransientInfluence", &transientWindInfluence,
					TreeWindSettings::kTransientInfluence.minimum, TreeWindSettings::kTransientInfluence.maximum,
					"%.2f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(6);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##LeafTransientInfluence", &leafTransientWindInfluence,
					TreeWindSettings::kTransientInfluence.minimum, TreeWindSettings::kTransientInfluence.maximum,
					"%.2f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(7);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##TransientFlutterCap", &leafTransientFlutterMaximum,
					TreeWindSettings::kLeafTransientFlutterMaximum.minimum,
					TreeWindSettings::kLeafTransientFlutterMaximum.maximum,
					"%.2f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableSetColumnIndex(8);
				ImGui::SetNextItemWidth(-1.0f);
				changed |= ImGui::SliderFloat("##TransientBend", &transientMaximumBendMultiplier,
					TreeWindSettings::kTransientMaximumBendMultiplier.minimum,
					TreeWindSettings::kTransientMaximumBendMultiplier.maximum,
					"%.2f", ImGuiSliderFlags_AlwaysClamp);
				if (changed) {
					(void)TreeWindPatcher::SetRule(ruleIndex, bend, leafAmbient, upperBendRange,
						maximumDisplacementPercent, rule.trunkGustInfluence, rule.leafGustInfluence,
						rule.springStrength, rule.springDamping, transientWindInfluence, leafTransientWindInfluence,
						leafTransientFlutterMaximum,
						transientMaximumBendMultiplier);
					uiState.treeWindSaveStatus.clear();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
}

void Wind::DrawGrassWindSettings()
{
	if (!ImGui::BeginTabItem(T(TKEY("tab_grass"), "Grass")))
		return;
	uiState.activeSettingsPage = SettingsPage::Grass;

	ImGui::Checkbox(T(TKEY("enable_ambient_grass_wind"), "Enable Ambient Grass Wind"), &settings.enableAmbientGrassWind);
	ImGui::Checkbox(T(TKEY("override_trunk_wind_intensity"), "Override Vanilla Wind Intensity"), &settings.overrideTrunkWindIntensity);
	ImGui::BeginDisabled(!settings.overrideTrunkWindIntensity);
	ImGui::SliderFloat(T(TKEY("trunk_wind_intensity"), "Vanilla Wind Intensity"), &settings.trunkWindIntensityOverride,
		kTrunkWindIntensityMin, kTrunkWindIntensityMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("trunk_wind_intensity_tooltip"), "Scales Skyrim's vanilla grass motion; ambient tree bending always uses the shared wind field."));
	if (ImGui::Button(T(TKEY("reset_grass_wind_settings"), "Reset Grass Settings")))
		ResetGrassWindSettings();

	ImGui::BeginDisabled(!settings.enableAmbientGrassWind);
	ImGui::SliderFloat(T(TKEY("grass_wind_response"), "Bend Strength"), &settings.grassWindResponse,
		kGrassWindResponseMin, kGrassWindResponseMax, "%.0f deg/unit", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_response_tooltip"), "Controls how strongly sampled ambient wind velocity bends the grass."));
	ImGui::SliderFloat(T(TKEY("grass_wind_sensitivity"), "Wind Sensitivity"), &settings.grassWindSensitivity,
		kGrassWindSensitivityMin, kGrassWindSensitivityMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_sensitivity_tooltip"), "Scales grass-only wind speed. At 2x, wind at speed 1 is treated as speed 2 for grass bending and flutter."));
	ImGui::SliderFloat(T(TKEY("grass_wind_maximum_tilt"), "Maximum Bend Angle"), &settings.grassWindMaximumTilt,
		kGrassWindMaximumTiltMin, kGrassWindMaximumTiltMax, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_maximum_tilt_tooltip"), "Limits how far a grass blade can lean from upright."));
	ImGui::SliderFloat(T(TKEY("grass_wind_bend_profile"), "Tip Flexibility"), &settings.grassWindBendProfile,
		kGrassWindBendProfileMin, kGrassWindBendProfileMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_bend_profile_tooltip"), "Zero leans the whole blade uniformly; one concentrates bending toward the tip."));
	float compressionToBendPercent = settings.grassWindCompressionToBend * 100.0f;
	if (ImGui::SliderFloat(T(TKEY("grass_wind_compression_to_bend"), "Downward Wind Bend Share"),
			&compressionToBendPercent, kGrassWindCompressionToBendMin * 100.0f,
			kGrassWindCompressionToBendMax * 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
		settings.grassWindCompressionToBend = compressionToBendPercent * 0.01f;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_compression_to_bend_tooltip"),
			"Zero uses vertical compression; 100% converts the same downward-wind response into additional directional bending, limited by Maximum Bend Angle."));

	ImGui::SeparatorText(T(TKEY("grass_wind_spring"), "Physical Spring"));
	ImGui::SliderFloat(T(TKEY("grass_wind_spring_frequency"), "Natural Frequency"),
		&settings.grassWindSpringFrequency, kGrassWindSpringFrequencyMin,
		kGrassWindSpringFrequencyMax, "%.2f Hz", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_spring_frequency_tooltip"),
			"Controls how quickly blades react and rebound. Lower values feel heavier; higher values feel stiffer."));
	ImGui::SliderFloat(T(TKEY("grass_wind_spring_damping"), "Damping Ratio"),
		&settings.grassWindSpringDamping, kGrassWindSpringDampingMin,
		kGrassWindSpringDampingMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_spring_damping_tooltip"),
			"Below one allows a natural rebound; one is critically damped; above one settles without overshoot."));

	static constexpr const char* qualityNames[] = { "Near", "Mid", "Far" };
	static constexpr const char* textureSizeLabels[] = { "32", "64", "128", "256", "512", "1024" };
	static constexpr const char* textureSizeKeys[] = {
		"grass_wind_spring_near_texture_size",
		"grass_wind_spring_mid_texture_size",
		"grass_wind_spring_far_texture_size"
	};
	static constexpr const char* textureSizeDefaults[] = {
		"Near Field Texture Size",
		"Mid Field Texture Size",
		"Far Field Texture Size"
	};
	static constexpr const char* distanceKeys[] = {
		"grass_wind_spring_near_distance",
		"grass_wind_spring_mid_distance",
		"grass_wind_spring_far_distance"
	};
	static constexpr const char* distanceDefaults[] = {
		"Near Range End",
		"Mid Range End",
		"Far Range End"
	};
	for (uint32_t index = 0; index < kGrassWindSpringQualityRangeCount; ++index) {
		ImGui::SeparatorText(qualityNames[index]);
		int textureSizeIndex = 0;
		for (std::size_t option = 0; option < kGrassWindSpringTextureSizes.size(); ++option) {
			if (settings.grassWindSpringQuality[index].textureSize == kGrassWindSpringTextureSizes[option]) {
				textureSizeIndex = static_cast<int>(option);
				break;
			}
		}
		if (ImGui::Combo(T(textureSizeKeys[index], textureSizeDefaults[index]), &textureSizeIndex,
				textureSizeLabels, static_cast<int>(std::size(textureSizeLabels))))
			settings.grassWindSpringQuality[index].textureSize = kGrassWindSpringTextureSizes[textureSizeIndex];
		ImGui::SliderFloat(T(distanceKeys[index], distanceDefaults[index]),
			&settings.grassWindSpringQuality[index].maxDistance, kGrassWindSpringDistanceMin,
			kGrassWindSpringDistanceMax, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
	}
	for (uint32_t index = 1; index < kGrassWindSpringQualityRangeCount; ++index)
		settings.grassWindSpringQuality[index].maxDistance =
			std::max(settings.grassWindSpringQuality[index].maxDistance,
				settings.grassWindSpringQuality[index - 1].maxDistance);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_spring_quality_tooltip"),
			"Each field covers a radial quality range. The next range begins where the previous range ends; grass beyond the far range uses Skyrim's vanilla wind."));

	ImGui::SeparatorText(T(TKEY("grass_wind_flutter"), "Flutter"));
	ImGui::SliderFloat(T(TKEY("grass_wind_flutter_strength"), "Flutter Strength"), &settings.grassWindFlutterStrength,
		kGrassWindFlutterStrengthMin, kGrassWindFlutterStrengthMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_flutter_strength_tooltip"), "Scales Skyrim-style per-blade flutter."));
	ImGui::SliderFloat(T(TKEY("grass_wind_flutter_frequency"), "Flutter Frequency"), &settings.grassWindFlutterFrequency,
		kGrassWindFlutterFrequencyMin, kGrassWindFlutterFrequencyMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_flutter_frequency_tooltip"), "Controls how quickly Skyrim-style flutter oscillates for each grass blade."));
	std::array<double, kGrassWindSpringQualityRangeCount> springMemoryMiB{};
	double totalSpringMemoryMiB = 0.0;
	for (uint32_t index = 0; index < kGrassWindSpringQualityRangeCount; ++index) {
		const uint32_t textureSize = SanitizeGrassWindSpringTextureSize(
			settings.grassWindSpringQuality[index].textureSize);
		springMemoryMiB[index] = static_cast<double>(textureSize) * textureSize *
		                         kGrassWindSpringTextureCount * kGrassWindSpringBytesPerPixel / kBytesPerMiB;
		totalSpringMemoryMiB += springMemoryMiB[index];
	}
	ImGui::SeparatorText(T(TKEY("grass_wind_spring_memory"), "Spring Memory"));
	ImGui::Text("%s: %.2f MiB", T(TKEY("grass_wind_spring_memory_total"), "Estimated GPU memory reserved"), totalSpringMemoryMiB);
	ImGui::Text("%s: %.2f MiB | %s: %.2f MiB | %s: %.2f MiB",
		qualityNames[0], springMemoryMiB[0], qualityNames[1], springMemoryMiB[1], qualityNames[2], springMemoryMiB[2]);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("grass_wind_spring_memory_tooltip"),
			"Estimate for four RGBA16F textures per field: two response textures and two velocity textures."));

	ImGui::EndDisabled();
	ImGui::EndTabItem();
}

#undef I18N_KEY_PREFIX
