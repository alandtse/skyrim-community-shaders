#include "SceneSelector.h"
#include "I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.weather_picker."

#include "Feature.h"
#include "Menu.h"
#include "State.h"
#include "Util.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include "CSEditor.h"
#include "CSEditor/EditorWindow.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <nlohmann/json.hpp>

namespace
{
	struct WeatherFlagInfo
	{
		RE::TESWeather::WeatherDataFlag flag;
		std::string_view canonicalName;
	};

	constexpr std::array<WeatherFlagInfo, 6> kClassifiedWeatherFlags = { {
		{ RE::TESWeather::WeatherDataFlag::kPleasant, "Pleasant" },
		{ RE::TESWeather::WeatherDataFlag::kCloudy, "Cloudy" },
		{ RE::TESWeather::WeatherDataFlag::kRainy, "Rainy" },
		{ RE::TESWeather::WeatherDataFlag::kSnow, "Snow" },
		{ RE::TESWeather::WeatherDataFlag::kPermAurora, "Aurora" },
		{ RE::TESWeather::WeatherDataFlag::kAuroraFollowsSun, "Aurora Sun" },
	} };

	const WeatherFlagInfo* FindWeatherFlagInfo(RE::TESWeather::WeatherDataFlag flag)
	{
		auto it = std::ranges::find(kClassifiedWeatherFlags, flag, &WeatherFlagInfo::flag);
		return it != kClassifiedWeatherFlags.end() ? &*it : nullptr;
	}

	const WeatherFlagInfo* FindWeatherFlagInfo(std::string_view canonicalName)
	{
		auto it = std::ranges::find(kClassifiedWeatherFlags, canonicalName, &WeatherFlagInfo::canonicalName);
		return it != kClassifiedWeatherFlags.end() ? &*it : nullptr;
	}

	const char* GetWeatherFilterLabel(const WeatherFlagInfo& info)
	{
		switch (info.flag) {
		case RE::TESWeather::WeatherDataFlag::kPleasant:
			return T(TKEY("pleasant"), "Pleasant");
		case RE::TESWeather::WeatherDataFlag::kCloudy:
			return T(TKEY("cloudy"), "Cloudy");
		case RE::TESWeather::WeatherDataFlag::kRainy:
			return T(TKEY("rainy"), "Rainy");
		case RE::TESWeather::WeatherDataFlag::kSnow:
			return T(TKEY("snow"), "Snow");
		case RE::TESWeather::WeatherDataFlag::kPermAurora:
			return T(TKEY("aurora"), "Aurora");
		case RE::TESWeather::WeatherDataFlag::kAuroraFollowsSun:
			return T(TKEY("aurora_sun"), "Aurora Sun");
		default:
			return info.canonicalName.data();
		}
	}

	bool DrawAnalysisSectionHeader(const char* label)
	{
		constexpr auto flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		return ImGui::TreeNodeEx(label, flags);
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SceneSelector::WeatherDetailsWindowSettings,
	Enabled,
	ShowInOverlay,
	Position,
	PositionSet)

void SceneSelector::LoadSettings(json& o_json)
{
	WeatherDetailsWindow = o_json;
}

void SceneSelector::SaveSettings(json& o_json)
{
	o_json = WeatherDetailsWindow;
}

void SceneSelector::PostPostLoad()
{
	// Before the game loop starts, so no call site can be executing while it is rewritten.
	EditorWindow::InstallWeatherLockHooks();
}

void SceneSelector::DataLoaded()
{
	s_dataAvailable = true;
}

void SceneSelector::Prepass()
{
	if (!globals::features::csEditor.loaded)
		CSEditor::UpdateWeatherLockAndTime();
}

void SceneSelector::EnsureWeatherListLoaded()
{
	if (!s_dataAvailable)
		return;

	LoadAllWeathers();
}

void SceneSelector::DrawSettings()
{
	EnsureWeatherListLoaded();

	DrawTimeControls();
	DrawSceneSelectorSection();

	ImGui::Spacing();
	DrawShowInOverlayToggle();
}

void SceneSelector::DrawShowInOverlayToggle()
{
	const auto& themeSettings = Menu::GetSingleton()->GetTheme();
	const auto& menuSettings = Menu::GetSingleton()->GetSettings();

	bool showInOverlay = WeatherDetailsWindow.ShowInOverlay;
	if (ImGui::Checkbox(T(TKEY("show_in_overlay"), "Show in Overlay"), &showInOverlay)) {
		WeatherDetailsWindow.ShowInOverlay = showInOverlay;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("show_in_overlay_tooltip"),
							  "Opens weather details in a separate window that stays open\neven when the main menu is closed. "));
		ImGui::Text(T(TKEY("toggle_with"), "Toggle with "));
		ImGui::SameLine();
		ImGui::TextColored(themeSettings.StatusPalette.CurrentHotkey, "%s", Util::Input::KeyIdToString(menuSettings.OverlayToggleKey).c_str());
	}
}

void SceneSelector::DrawSceneSelectorSection()
{
	ImGui::Spacing();

	RenderCoreWeatherDetails(true);

	RenderFeatureWeatherAnalysis();
}

void SceneSelector::DrawTimeControls()
{
	ImGui::SeparatorText(T(TKEY("time_control"), "Time Control"));
	EditorWindow::GetSingleton()->DrawTimeControls();
	ImGui::Spacing();
}

void SceneSelector::DrawWeatherStatusPanel()
{
	ImGui::SeparatorText(T(TKEY("weather_transition"), "Weather Transition"));

	auto* sky = globals::game::sky;
	auto* currentWeather = sky ? sky->currentWeather : nullptr;
	auto* previousWeather = sky ? sky->lastWeather : nullptr;
	const float weatherLerp = sky && std::isfinite(sky->currentWeatherPct) ?
	                              std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) :
	                              1.0f;
	const auto& theme = Menu::GetSingleton()->GetTheme();

	if (currentWeather) {
		ImGui::Text(T(TKEY("current_weather"), "Current Weather: %s"),
			currentWeather->GetFormEditorID() ?
				currentWeather->GetFormEditorID() :
				std::format("{:08X}", currentWeather->GetFormID()).c_str());

		const bool isTransitioning = previousWeather && weatherLerp < 1.0f;
		if (isTransitioning) {
			ImGui::Text(T(TKEY("transitioning_from"), "Transitioning From: %s"),
				previousWeather->GetFormEditorID() ?
					previousWeather->GetFormEditorID() :
					std::format("{:08X}", previousWeather->GetFormID()).c_str());

			float transitionPct = weatherLerp * 100.0f;
			const auto transitionOverlay = std::vformat(T(TKEY("transition_progress"), "Transition: {:.1f}%"), std::make_format_args(transitionPct));
			ImGui::ProgressBar(weatherLerp, ImVec2(-1, 0), transitionOverlay.c_str());
		} else {
			ImGui::TextDisabled("%s", T(TKEY("no_transition"), "No active weather transition"));
		}
	} else {
		ImGui::TextColored(theme.StatusPalette.Warning, "%s", T(TKEY("no_active_weather"), "No Active Weather"));
	}
}

// ================================================================================
// Scene Selector functionality
// ================================================================================

void SceneSelector::ResetWindowLayout()
{
	WeatherDetailsWindow.PositionSet = false;
	resetWindowSize = true;
}

void SceneSelector::RenderWeatherDetailsWindow(bool* open)
{
	if (!open || !*open)
		return;

	auto player = RE::PlayerCharacter::GetSingleton();
	if (!player || !player->parentCell)
		return;

	const bool allowInputs =
		Menu::GetSingleton()->ShouldSwallowInput() ||
		(globals::game::ui && globals::game::ui->IsMenuOpen(RE::CursorMenu::MENU_NAME));

	// Set initial position if not already set
	const float scale = Util::GetUIScale();
	if (!WeatherDetailsWindow.PositionSet) {
		const auto* viewport = ImGui::GetMainViewport();
		const float padding = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;
		const ImVec2 defaultPosition(
			viewport->WorkPos.x + viewport->WorkSize.x - padding,
			viewport->WorkPos.y + padding);
		ImGui::SetNextWindowPos(defaultPosition, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
		WeatherDetailsWindow.Position = defaultPosition;
		WeatherDetailsWindow.PositionSet = true;
	} else {
		ImGui::SetNextWindowPos(WeatherDetailsWindow.Position, ImGuiCond_FirstUseEver);
	}

	ImGui::SetNextWindowSize(
		ImVec2(600 * scale, 800 * scale),
		resetWindowSize ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
	const ImGuiWindowFlags windowFlags = allowInputs ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs;
	if (Util::BeginWithRoundedClose("Weather Details##Popup", open, windowFlags)) {
		// Remember window position for next frame
		ImVec2 currentPos = ImGui::GetWindowPos();
		if (currentPos.x != WeatherDetailsWindow.Position.x || currentPos.y != WeatherDetailsWindow.Position.y) {
			WeatherDetailsWindow.Position = currentPos;
		}

		// Keep scroll state separate when the weather controls appear or disappear.
		const char* contentId = allowInputs ? "##InteractiveWeatherDetails" : "##ReadOnlyWeatherDetails";
		if (ImGui::BeginChild(contentId, { 0, 0 })) {
			RenderCoreWeatherDetails(allowInputs);
			RenderFeatureWeatherAnalysis();
		}
		ImGui::EndChild();
	}
	ImGui::End();
	resetWindowSize = false;
}

ImVec4 SceneSelector::GetWeatherTypeColor(RE::TESWeather* weather)
{
	if (!weather) {
		return Menu::GetSingleton()->GetTheme().StatusPalette.InfoColor;
	}

	const auto& theme = Menu::GetSingleton()->GetTheme();

	// Priority order for weather classification colors (highest priority first)
	static const std::vector<RE::TESWeather::WeatherDataFlag> priorityOrder = {
		RE::TESWeather::WeatherDataFlag::kRainy,
		RE::TESWeather::WeatherDataFlag::kSnow,
		RE::TESWeather::WeatherDataFlag::kPermAurora,
		RE::TESWeather::WeatherDataFlag::kAuroraFollowsSun,
		RE::TESWeather::WeatherDataFlag::kCloudy,
		RE::TESWeather::WeatherDataFlag::kPleasant
	};

	// Check flags in priority order
	for (const auto& flag : priorityOrder) {
		if (weather->data.flags.any(flag)) {
			return GetWeatherFlagColor(flag);
		}
	}

	// Check for unclassified/unflagged weather
	if (weather->data.flags.underlying() == 0) {
		return Menu::GetSingleton()->GetTheme().StatusPalette.Warning;
	}

	return theme.StatusPalette.InfoColor;  // Default blue
}

// --- Helper: Display basic weather info (name, flags, percentage) ---
void SceneSelector::DisplayWeatherBasicInfo(RE::TESWeather* weather, float weatherPct)
{
	if (!weather) {
		ImGui::BulletText("%s", T(TKEY("no_weather_found"), "No Weather Found"));
		return;
	}
	std::string weatherText = Util::FormatWeather(weather);
	ImGui::Bullet();
	ImGui::SameLine();
	bool showTooltip = SceneSelector::RenderMultiColorWeatherName(weather, weatherText);
	if (showTooltip) {
		ImGui::BeginTooltip();
		ImGui::Text(T(TKEY("tooltip_name"), "Name: %s"), weather->GetName() ? weather->GetName() : T(TKEY("unnamed"), "Unnamed"));
		ImGui::Text(T(TKEY("tooltip_editor_id_2"), "Editor ID: %s"), weather->GetFormEditorID() ? weather->GetFormEditorID() : T(TKEY("none_value"), "None"));
		ImGui::Text(T(TKEY("tooltip_form_id_2"), "Form ID: 0x%08X"), weather->GetFormID());
		auto flagNames = SceneSelector::GetWeatherFlagNames(weather);
		if (!flagNames.empty()) {
			std::string joinedFlags = flagNames[0];
			for (size_t j = 1; j < flagNames.size(); ++j) {
				joinedFlags += ", " + flagNames[j];
			}
			ImGui::Text(T(TKEY("tooltip_flags"), "Flags: %s"), joinedFlags.c_str());
		} else {
			ImGui::Text("%s", T(TKEY("tooltip_flags_none"), "Flags: None"));
		}
		ImGui::EndTooltip();
	}
	if (weatherPct >= 0.0f) {
		ImGui::BulletText(T(TKEY("weather_percentage"), "Weather Percentage: %.1f%%"), weatherPct * 100.0f);
	}
}

void SceneSelector::DisplayPrecipitationInfo(RE::TESWeather* weather)
{
	if (!weather || !weather->precipitationData) {
		ImGui::BulletText("%s", T(TKEY("no_precipitation_data"), "Particle Density: No precipitation data"));
		return;
	}
	auto particleDensity = weather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
	ImGui::BulletText(T(TKEY("particle_density"), "Particle Density: %.3f"), particleDensity);
	GET_INSTANCE_MEMBER(particleTexture, weather->precipitationData)
	if (!particleTexture.textureName.empty()) {
		ImGui::BulletText(T(TKEY("particle_texture"), "Particle Texture: %s"), particleTexture.textureName.c_str());
	} else {
		ImGui::BulletText("%s", T(TKEY("particle_texture_none"), "Particle Texture: None"));
	}
	uint8_t precipBeginFadeIn = weather->data.precipitationBeginFadeIn;
	uint8_t precipEndFadeOut = weather->data.precipitationEndFadeOut;
	float precipBeginNormalized = precipBeginFadeIn / 255.0f;
	float precipEndNormalized = precipEndFadeOut / 255.0f;
	ImGui::BulletText(T(TKEY("precip_begin_fade_in"), "Precip Begin Fade-In: %.3f (raw %u)"), precipBeginNormalized, precipBeginFadeIn);
	ImGui::BulletText(T(TKEY("precip_end_fade_out"), "Precip End Fade-Out: %.3f (raw %u)"), precipEndNormalized, precipEndFadeOut);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		Util::DrawMultiLineTooltip({ T(TKEY("precip_fade_info_0"), "Precipitation fade transition parameters:"),
			T(TKEY("precip_fade_info_1"), "Begin Fade-In: Point where precipitation starts appearing"),
			T(TKEY("precip_fade_info_2"), "End Fade-Out: Point where precipitation fully disappears"),
			T(TKEY("precip_fade_info_3"), "Raw values: 0-255 (uint8), Normalized: 0.0-1.0") });
	}
}

void SceneSelector::DisplayLightningInfo(RE::TESWeather* weather, bool showInteractiveElements)
{
	if (!weather || (uint8_t)weather->data.thunderLightningFrequency == 0)
		return;
	const auto& theme = Menu::GetSingleton()->GetTheme();
	uint8_t lightningR = weather->data.lightningColor.red;
	uint8_t lightningG = weather->data.lightningColor.green;
	uint8_t lightningB = weather->data.lightningColor.blue;
	ImGui::Text("%s", T(TKEY("lightning_color"), "Lightning Color:"));
	ImGui::SameLine();
	float lightningColor[3] = { lightningR / 255.0f, lightningG / 255.0f, lightningB / 255.0f };
	ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
	if (!showInteractiveElements) {
		flags |= ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip;
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, theme.StatusPalette.Disable.w);
	}
	bool colorChanged = ImGui::ColorEdit3("##LightningColor", lightningColor, flags);
	if (!showInteractiveElements) {
		ImGui::PopStyleVar();
	}
	if (colorChanged && showInteractiveElements) {
		weather->data.lightningColor.red = static_cast<std::uint8_t>(lightningColor[0] * 255.0f + 0.5f);
		weather->data.lightningColor.green = static_cast<std::uint8_t>(lightningColor[1] * 255.0f + 0.5f);
		weather->data.lightningColor.blue = static_cast<std::uint8_t>(lightningColor[2] * 255.0f + 0.5f);
	}
	uint8_t thunderFreqRaw = (uint8_t)weather->data.thunderLightningFrequency;
	ImGui::BulletText(T(TKEY("thunder_frequency"), "Thunder Frequency: %u"), static_cast<unsigned>(thunderFreqRaw));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		Util::DrawMultiLineTooltip({ T(TKEY("thunder_freq_info_0"), "Thunder frequency raw value (0-255):"),
			"",
			T(TKEY("thunder_freq_info_1"), "Known data points from Creation Kit slider:"),
			T(TKEY("thunder_freq_info_2"), "- Raw 15 = ~100% frequency (highest thunder)"),
			T(TKEY("thunder_freq_info_3"), "- Raw 76 = ~75% frequency"),
			T(TKEY("thunder_freq_info_4"), "- Raw 203 = ~20% frequency"),
			T(TKEY("thunder_freq_info_5"), "- Raw 246 = ~5% frequency"),
			T(TKEY("thunder_freq_info_6"), "- Raw 255 = ~0% frequency (lowest thunder)"),
			"",
			T(TKEY("thunder_freq_info_7"), "Range: 0-255 (unsigned 8-bit integer)"),
			T(TKEY("thunder_freq_info_8"), "Note: Creation Kit interprets this value non-linearly") });
	}
	uint8_t lightningBeginFadeIn = weather->data.thunderLightningBeginFadeIn;
	uint8_t lightningEndFadeOut = weather->data.thunderLightningEndFadeOut;
	float lightningBeginNormalized = lightningBeginFadeIn / 255.0f;
	float lightningEndNormalized = lightningEndFadeOut / 255.0f;
	ImGui::BulletText(T(TKEY("lightning_begin_fade_in"), "Lightning Begin Fade-In: %.3f (raw %u)"), lightningBeginNormalized, lightningBeginFadeIn);
	ImGui::BulletText(T(TKEY("lightning_end_fade_out"), "Lightning End Fade-Out: %.3f (raw %u)"), lightningEndNormalized, lightningEndFadeOut);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		Util::DrawMultiLineTooltip({ T(TKEY("lightning_fade_info_0"), "Lightning fade transition parameters:"),
			T(TKEY("lightning_fade_info_1"), "Begin Fade-In: Point where lightning starts appearing"),
			T(TKEY("lightning_fade_info_2"), "End Fade-Out: Point where lightning fully disappears"),
			T(TKEY("lightning_fade_info_3"), "Raw values: 0-255 (uint8), Normalized: 0.0-1.0") });
	}
}

void SceneSelector::DisplayWindInfo(RE::TESWeather* weather)
{
	auto sky = globals::game::sky;
	if (!weather || (weather->data.windSpeed <= 0 && (!sky || sky->windSpeed <= 0.0f)))
		return;
	float windSpeedDisplay = weather->data.windSpeed / 255.0f;
	ImGui::BulletText(T(TKEY("weather_wind_speed"), "Weather Wind Speed: %.2f (raw %d)"), windSpeedDisplay, weather->data.windSpeed);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::string windStr = Util::Units::FormatWindSpeed(weather->data.windSpeed);
		Util::DrawMultiLineTooltip({ T(TKEY("wind_speed_tooltip_0"), "Wind speed from weather definition"),
			windStr.c_str() });
	}
	if (sky) {
		ImGui::BulletText(T(TKEY("sky_wind_speed"), "Sky Wind Speed: %.2f"), sky->windSpeed);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			Util::DrawMultiLineTooltip({ T(TKEY("sky_wind_tooltip_0"), "Current active wind speed from the sky system"),
				T(TKEY("sky_wind_tooltip_1"), "This affects particle behavior and wind-based effects") });
		}
	}
	float weatherWindDirDegrees = Util::Units::DirectionRawToDegrees(weather->data.windDirection);
	ImGui::BulletText(T(TKEY("wind_direction"), "Wind Direction: %.1f\xc2\xb0 (raw %d)"), weatherWindDirDegrees, weather->data.windDirection);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::string dirStr = Util::Units::FormatDirection(weather->data.windDirection);
		Util::DrawMultiLineTooltip({ T(TKEY("wind_direction_tooltip_0"), "Wind direction from weather definition"),
			dirStr.c_str() });
	}
	float weatherWindRangeDegrees = Util::Units::DirectionRangeToDegrees(weather->data.windDirectionRange);
	ImGui::BulletText(T(TKEY("wind_direction_range"), "Wind Direction Range: %.1f\xc2\xb0 (raw %d)"), weatherWindRangeDegrees, weather->data.windDirectionRange);

	if (auto player = RE::PlayerCharacter::GetSingleton()) {
		float playerAngleZ = player->GetAngleZ();
		float playerAngleDegrees = Util::Units::NormalizeDegrees0To360(Util::Units::RadiansToDegrees(playerAngleZ));
		ImGui::BulletText(T(TKEY("player_direction"), "Player Direction: %.1f\xc2\xb0"), playerAngleDegrees);
		float effectiveWindDirection = Util::Units::NormalizeDegrees0To360(weatherWindDirDegrees - WIND_DIRECTION_OFFSET);
		float rawDifference = Util::Units::NormalizeDegreesToSignedRange(effectiveWindDirection - playerAngleDegrees);
		ImGui::BulletText(T(TKEY("effective_wind_dir"), "Effective Wind Dir: %.1f\xc2\xb0 (raw - %.1f\xc2\xb0)"), effectiveWindDirection, WIND_DIRECTION_OFFSET);
		ImGui::BulletText(T(TKEY("wind_vs_player"), "Wind vs Player: %.1f\xc2\xb0"), rawDifference);
		const char* windRelation;
		if (std::abs(rawDifference) < 30.0f) {
			windRelation = T(TKEY("tailwind"), "Tailwind (wind behind player)");
		} else if (std::abs(rawDifference) > 150.0f) {
			windRelation = T(TKEY("headwind"), "Headwind (wind coming toward player)");
		} else if (rawDifference > 0) {
			windRelation = T(TKEY("right_crosswind"), "Right crosswind");
		} else {
			windRelation = T(TKEY("left_crosswind"), "Left crosswind");
		}
		ImGui::SameLine();
		Util::Text::RestartNeeded("(%s)", windRelation);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			Util::DrawMultiLineTooltip({
				T(TKEY("wind_vs_player_tooltip_0"), "Wind relative to player direction:"),
				T(TKEY("wind_vs_player_tooltip_1"), "- ~0\xc2\xb0 = Tailwind (wind behind player)"),
				T(TKEY("wind_vs_player_tooltip_2"),
					"- ~\xc2\xb1"
					"90\xc2\xb0 = Crosswind (left/right)"),
				T(TKEY("wind_vs_player_tooltip_3"),
					"- ~\xc2\xb1"
					"180\xc2\xb0 = Headwind (wind coming toward player)"),
			});
		}
	}
}

// --- Main function: now just delegates to helpers ---
void SceneSelector::DisplayWeatherInfo(RE::TESWeather* weather, float weatherPct, bool showInteractiveElements)
{
	ImGui::SeparatorText(T(TKEY("overview"), "Overview"));
	SceneSelector::DisplayWeatherBasicInfo(weather, weatherPct);
	if (!weather)
		return;

	ImGui::SeparatorText(T(TKEY("precipitation"), "Precipitation"));
	SceneSelector::DisplayPrecipitationInfo(weather);

	if ((uint8_t)weather->data.thunderLightningFrequency != 0) {
		ImGui::SeparatorText(T(TKEY("lightning"), "Lightning"));
		SceneSelector::DisplayLightningInfo(weather, showInteractiveElements);
	}

	auto sky = globals::game::sky;
	if (weather->data.windSpeed > 0 || (sky && sky->windSpeed > 0.0f)) {
		ImGui::SeparatorText(T(TKEY("wind"), "Wind"));
		SceneSelector::DisplayWindInfo(weather);
	}
}

void SceneSelector::RenderWeatherControls(RE::Sky* sky)
{
	ImGui::SeparatorText(T(TKEY("weather_selection"), "Weather Selection"));

	ImGui::Text("%s", T(TKEY("filter_by_weather_type"), "Filter by Weather Type:"));
	if (ImGui::Button(T(TKEY("select_all"), "Select All"))) {
		s_weatherFlagFilter = ALL_WEATHER_FLAGS;  // All weather flags (bits 0-6, including unclassified)
	}
	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("clear_all"), "Clear All"))) {
		s_weatherFlagFilter = 0x00;  // No flags
	}
	// Dynamic checkbox layout - calculate how many fit per row
	float availableWidth = ImGui::GetContentRegionAvail().x;
	float checkboxWidth = 110.0f;  // Fits "Aurora Sun" label
	const auto checkboxesPerRow = static_cast<size_t>(std::max(1, static_cast<int>(availableWidth / checkboxWidth)));

	// Classified weather filters share the canonical flag metadata used by analysis displays.
	size_t filterIndex = 0;
	for (const auto& filter : kClassifiedWeatherFlags) {
		if (filterIndex > 0 && filterIndex % checkboxesPerRow != 0) {
			ImGui::SameLine();
		}

		ImGui::PushStyleColor(ImGuiCol_Text, GetWeatherFlagColor(filter.flag));
		ImGui::CheckboxFlags(GetWeatherFilterLabel(filter), &s_weatherFlagFilter, static_cast<uint32_t>(filter.flag));
		ImGui::PopStyleColor();
		++filterIndex;
	}

	// Unclassified weather remains a separate synthetic filter bit.
	if (filterIndex % checkboxesPerRow != 0) {
		ImGui::SameLine();
	}
	ImGui::PushStyleColor(ImGuiCol_Text, Menu::GetSingleton()->GetTheme().StatusPalette.Warning);
	ImGui::CheckboxFlags(T(TKEY("none_filter"), "None"), &s_weatherFlagFilter, UNCLASSIFIED_FLAG);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		Util::DrawMultiLineTooltip({ T(TKEY("none_filter_tooltip_0"), "Shows weathers that are not classified under any specific category."),
			T(TKEY("none_filter_tooltip_1"), "Includes weathers with no flags or only untracked flags."),
			T(TKEY("none_filter_tooltip_2"), "Categories tracked: Pleasant, Cloudy, Rainy, Snow, Aurora, Aurora Sun") });
	}
	ImGui::PopStyleColor();

	// Update filtered weathers when filter changes
	if (s_lastWeatherFlagFilter != s_weatherFlagFilter) {
		UpdateFilteredWeathers();
		s_selectedWeatherIdx = -1;
		s_lastWeatherFlagFilter = s_weatherFlagFilter;
	}

	if (ImGui::Button(T(TKEY("reset_weather"), "Reset Weather"))) {
		Util::EnvironmentControls::ResetWeather();
		// Update the selection box to reflect the reset weather without double-applying
		s_selectedWeatherIdx = FindWeatherIndex(sky->defaultWeather);
		logger::info("[SceneSelector] Reset weather to default");
	}

	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("reset_weather_tooltip"), "Resets weather to default"));
	}

	// Lock Weather toggle
	ImGui::SameLine();
	auto editorWindow = EditorWindow::GetSingleton();
	bool isLocked = editorWindow->IsWeatherLocked();
	bool hooksInstalled = EditorWindow::AreWeatherLockHooksInstalled();
	const char* lockLabel = isLocked ? T(TKEY("unlock_weather"), "Unlock Weather") : T(TKEY("lock_weather"), "Lock Weather");

	if (isLocked) {
		const auto& theme = Menu::GetSingleton()->GetTheme();
		ImGui::PushStyleColor(ImGuiCol_Button, theme.StatusPalette.SuccessColor);
	}
	if (ImGui::Button(lockLabel)) {
		if (isLocked) {
			editorWindow->UnlockWeather();
		} else if (sky->currentWeather) {
			editorWindow->LockWeather(sky->currentWeather);
		}
	}
	if (isLocked) {
		ImGui::PopStyleColor();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		if (hooksInstalled) {
			ImGui::Text("%s", T(TKEY("lock_weather_tooltip"), isLocked ? "Unlock weather to allow natural changes" : "Lock current weather to prevent changes"));
		} else {
			// Hooks failed to install, but MaintainWeatherLock still re-applies the lock every
			// frame, so the lock still works -- just without redirecting the engine's own
			// SetWeather/ForceWeather call sites, so weather may visibly flash before correcting.
			ImGui::Text("%s", T(TKEY("lock_weather_unavailable_tooltip"), "Weather-lock hooks failed to install; the lock still works but weather may briefly flash before correcting"));
		}
	}

	ImGui::SameLine();
	ImGui::Checkbox(T(TKEY("accelerate_weather_change"), "Accelerate Weather Change"), &s_accelerateWeatherChange);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("accelerate_weather_change_tooltip"), "When enabled, weather changes instantly"));
	}

	// Weather Selection - now with colored text
	std::vector<std::string> weatherLabels;
	weatherLabels.reserve(s_filteredWeathers.size());
	for (const auto& weather : s_filteredWeathers) {
		weatherLabels.push_back(Util::FormatWeather(weather));
	}

	// Custom combo with colored text
	const char* comboPreview = (s_selectedWeatherIdx >= 0 && s_selectedWeatherIdx < static_cast<int>(weatherLabels.size())) ?
	                               weatherLabels[s_selectedWeatherIdx].c_str() :
	                               T(TKEY("select_weather"), "Select Weather");

	static constexpr const char* kWeatherSearchId = "SceneSelector";

	if (ImGui::BeginCombo(T(TKEY("weather"), "Weather"), comboPreview)) {
		auto searchText = Util::DrawComboSearchInput(kWeatherSearchId);

		for (int i = 0; i < static_cast<int>(s_filteredWeathers.size()); ++i) {
			const bool isSelected = (s_selectedWeatherIdx == i);
			auto weather = s_filteredWeathers[i];

			// Filter by EditorID, Name, and FormID only (not classification tags)
			if (!searchText.empty()) {
				auto editorId = weather->GetFormEditorID() ? std::string(weather->GetFormEditorID()) : "";
				auto name = weather->GetName() ? std::string(weather->GetName()) : "";
				auto formId = std::format("{:08X}", weather->GetFormID());

				if (!Util::StringMatchesSearch(editorId, searchText) &&
					!Util::StringMatchesSearch(name, searchText) &&
					!Util::StringMatchesSearch(formId, searchText))
					continue;
			}

			ImGui::PushStyleColor(ImGuiCol_Text, GetWeatherTypeColor(weather));
			bool didSelect = ImGui::Selectable(weatherLabels[i].c_str(), isSelected);
			ImGui::PopStyleColor();

			if (didSelect) {
				s_selectedWeatherIdx = i;
				auto selectedWeather = s_filteredWeathers[i];

				Util::EnvironmentControls::ChangeWeather(selectedWeather, s_accelerateWeatherChange);

				Util::ClearComboSearch(kWeatherSearchId);
				logger::info("[SceneSelector] Changed weather to: {}", Util::FormatWeather(selectedWeather));
				break;
			}

			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::Text(T(TKEY("tooltip_weather_name"), "Weather: %s"), weather->GetName() ? weather->GetName() : T(TKEY("unnamed"), "Unnamed"));
				ImGui::Text(T(TKEY("tooltip_editor_id"), "Editor ID: %s"), weather->GetFormEditorID() ? weather->GetFormEditorID() : T(TKEY("none_value"), "None"));
				ImGui::Text(T(TKEY("tooltip_form_id"), "Form ID: 0x%08X"), weather->GetFormID());
				ImGui::EndTooltip();
			}

			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	} else {
		Util::ClearComboSearch(kWeatherSearchId);
	}
}

void SceneSelector::RenderWeatherInformationDisplay(RE::Sky* sky, bool showInteractiveElements)
{
	// Update cache: store current lastWeather if it exists, otherwise keep the cached one
	if (sky->lastWeather) {
		s_cachedLastWeather = sky->lastWeather;
	}

	if (!DrawAnalysisSectionHeader(T(TKEY("current_last_weather_analysis"), "Current & Last Weather Analysis")))
		return;

	// Use cached last weather for display if sky->lastWeather is null
	RE::TESWeather* displayLastWeather = sky->lastWeather ? sky->lastWeather : s_cachedLastWeather;

	// Create resizable 2-column table for current and last weather
	if (ImGui::BeginTable("WeatherComparison", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
		// Set up columns
		ImGui::TableSetupColumn(T(TKEY("current_weather_column"), "Current Weather"), ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableSetupColumn(T(TKEY("last_weather_column"), "Last Weather"), ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableHeadersRow();

		ImGui::TableNextRow();

		// Current Weather Column
		ImGui::TableNextColumn();
		ImGui::PushID("CurrentWeather");
		DisplayWeatherInfo(sky->currentWeather, sky->currentWeatherPct, showInteractiveElements);
		ImGui::PopID();

		// Last Weather Column
		ImGui::TableNextColumn();
		ImGui::PushID("LastWeather");
		DisplayWeatherInfo(displayLastWeather, std::abs(sky->currentWeatherPct - 1.0f), false);
		ImGui::PopID();

		ImGui::EndTable();
	}
}

void SceneSelector::RenderCoreWeatherDetails(bool showInteractiveElements)
{
	const auto showError = [](const char* msg) {
		auto menu = Menu::GetSingleton();
		const auto& theme = menu->GetTheme();
		ImGui::TextColored(theme.StatusPalette.Error, "%s", msg);
	};

	if (auto sky = globals::game::sky) {
		if (sky->mode.get() == RE::Sky::Mode::kFull) {
			if (showInteractiveElements) {
				RenderWeatherControls(sky);
			}
			DrawWeatherStatusPanel();
			ImGui::Spacing();
			ImGui::SeparatorText(T(TKEY("analysis"), "Analysis"));
			RenderWeatherInformationDisplay(sky, showInteractiveElements);
			ImGui::Spacing();
		} else {
			showError(T(TKEY("sky_not_full"), "Sky not in full mode"));
		}
	} else {
		showError(T(TKEY("sky_not_available"), "Sky not available"));
	}
}

void SceneSelector::LoadAllWeathers()
{
	if (s_weathersLoaded)
		return;

	auto dataHandler = RE::TESDataHandler::GetSingleton();
	if (dataHandler) {
		auto& weatherArray = dataHandler->GetFormArray<RE::TESWeather>();
		s_allWeathers.clear();
		s_allWeathers.reserve(weatherArray.size());
		for (auto weather : weatherArray) {
			if (weather) {
				s_allWeathers.push_back(weather);
			}
		}

		// Sort by name, then editorID, then formID for consistent ordering
		std::sort(s_allWeathers.begin(), s_allWeathers.end(), WeatherNameComparator{});
		s_weathersLoaded = true;
		// Initial population of filtered weathers
		UpdateFilteredWeathers();
	}
}

void SceneSelector::UpdateFilteredWeathers()
{
	s_filteredWeathers.clear();
	for (auto weather : s_allWeathers) {
		bool shouldInclude = false;

		// Check if all filters are selected (0x7F = all 7 bits)
		if (s_weatherFlagFilter == ALL_WEATHER_FLAGS) {
			shouldInclude = true;
		} else {
			// Check regular weather flags
			uint32_t weatherFlags = weather->data.flags.underlying();
			if ((weatherFlags & (s_weatherFlagFilter & 0x3F)) != 0) {
				shouldInclude = true;
			}

			// Check for None filter (bit 6) - includes weathers that don't match any of our tracked flags
			if (s_weatherFlagFilter & UNCLASSIFIED_FLAG) {
				// Define the mask for all the specific weather flags we track
				uint32_t trackedFlags = static_cast<uint32_t>(RE::TESWeather::WeatherDataFlag::kPleasant) |
				                        static_cast<uint32_t>(RE::TESWeather::WeatherDataFlag::kCloudy) |
				                        static_cast<uint32_t>(RE::TESWeather::WeatherDataFlag::kRainy) |
				                        static_cast<uint32_t>(RE::TESWeather::WeatherDataFlag::kSnow) |
				                        static_cast<uint32_t>(RE::TESWeather::WeatherDataFlag::kPermAurora) |
				                        static_cast<uint32_t>(RE::TESWeather::WeatherDataFlag::kAuroraFollowsSun);

				// Include if weather has no flags or only has flags we don't track
				if ((weatherFlags & trackedFlags) == 0) {
					shouldInclude = true;
				}
			}
		}

		if (shouldInclude) {
			s_filteredWeathers.push_back(weather);
		}
	}
}

int SceneSelector::FindWeatherIndex(RE::TESWeather* targetWeather)
{
	if (!targetWeather)
		return -1;
	for (size_t i = 0; i < s_filteredWeathers.size(); ++i) {
		if (s_filteredWeathers[i] == targetWeather) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

void SceneSelector::RenderFeatureWeatherAnalysis()
{
	auto sky = globals::game::sky;
	if (!sky || sky->mode.get() != RE::Sky::Mode::kFull)
		return;

	// Iterate through all loaded features to show their weather analysis
	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->loaded) {
			// Skip SceneSelector itself to avoid recursion
			if (feature == &globals::features::sceneSelector) {
				continue;
			}

			// Check if this feature provides weather analysis
			auto weatherConfig = feature->GetWeatherAnalysisConfig();
			if (weatherConfig.sectionName.empty()) {
				continue;  // Skip features that don't provide weather analysis
			}

			auto featureName = feature->GetShortName();
			ImGui::PushID(featureName.c_str());

			bool isExpanded = DrawAnalysisSectionHeader(weatherConfig.sectionName.c_str());
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(T(TKEY("feature_weather_analysis_tooltip"), "%s weather analysis"), feature->GetDisplayName().c_str());
			}

			if (isExpanded) {
				if (weatherConfig.drawFunction) {
					weatherConfig.drawFunction();
				}
			}

			ImGui::PopID();
		}
	}
}

std::vector<std::string> SceneSelector::GetWeatherFlagNames(RE::TESWeather* weather)
{
	std::vector<std::string> flagNames;
	if (!weather) {
		return flagNames;
	}

	uint32_t flags = weather->data.flags.underlying();
	if (flags == 0) {
		flagNames.push_back("None");
		return flagNames;
	}

	// Use magic_enum to iterate through all weather flags
	for (auto flagValue : magic_enum::enum_values<RE::TESWeather::WeatherDataFlag>()) {
		if (flagValue != RE::TESWeather::WeatherDataFlag::kNone &&
			weather->data.flags.any(flagValue)) {
			if (const auto* info = FindWeatherFlagInfo(flagValue)) {
				flagNames.emplace_back(info->canonicalName);
			} else {
				std::string flagName = std::string(magic_enum::enum_name(flagValue));
				flagNames.push_back(flagName.starts_with("k") ? flagName.substr(1) : flagName);
			}
		}
	}

	// Check for any unknown flags (flags not covered by the enum)
	uint32_t knownFlags = 0;
	for (auto flagValue : magic_enum::enum_values<RE::TESWeather::WeatherDataFlag>()) {
		if (flagValue != RE::TESWeather::WeatherDataFlag::kNone) {
			knownFlags |= static_cast<uint32_t>(flagValue);
		}
	}

	uint32_t unknownFlags = flags & ~knownFlags;
	if (unknownFlags != 0) {
		flagNames.push_back(std::format("{}({})", T(TKEY("unknown"), "Unknown"), unknownFlags));
	}

	return flagNames;
}

bool SceneSelector::RenderMultiColorWeatherName(RE::TESWeather* weather, const std::string& weatherName)
{
	if (!weather) {
		ImGui::Text("%s", weatherName.c_str());
		return false;
	}

	// Get all flags present in this weather
	std::vector<std::string> flagNames = GetWeatherFlagNames(weather);

	// If no flags or only one flag, use simple single-color display
	if (flagNames.size() <= 1) {
		ImVec4 weatherColor = GetWeatherTypeColor(weather);
		ImGui::PushStyleColor(ImGuiCol_Text, weatherColor);
		ImGui::Text("%s", weatherName.c_str());
		ImGui::PopStyleColor();
		return ImGui::IsItemHovered();
	}
	// For multiple flags, create a color-coded display
	// We'll show the weather name in segments, each with its own color

	// Create a visual representation with colored segments
	// Format: "WeatherName [Flag1][Flag2][Flag3]"

	// Display the main weather name in the primary color (highest priority flag)
	ImVec4 primaryColor = GetWeatherTypeColor(weather);
	ImGui::PushStyleColor(ImGuiCol_Text, primaryColor);

	// Extract base weather name (without the flag suffix)
	std::string baseName = weatherName;
	size_t bracketPos = baseName.find(" [");
	if (bracketPos != std::string::npos) {
		baseName = baseName.substr(0, bracketPos);
	}

	ImGui::Text("%s", baseName.c_str());
	ImGui::PopStyleColor();

	// Check if the main weather name (the most important part) was hovered
	bool baseNameHovered = ImGui::IsItemHovered();

	// Display flags as colored chips on the same line
	ImGui::SameLine();
	ImGui::Text(" ");

	for (size_t i = 0; i < flagNames.size(); ++i) {
		if (flagNames[i] == "None" || flagNames[i].find("Unknown") == 0) {
			continue;  // Skip "None" and "Unknown" flags for cleaner display
		}

		ImGui::SameLine();
		ImVec4 flagColor = GetWeatherFlagColorByName(flagNames[i]);
		ImGui::PushStyleColor(ImGuiCol_Text, flagColor);
		// Translate canonical flag name for display
		std::string flagKey = std::string(TKEY("flag_")) + flagNames[i];
		std::transform(flagKey.begin(), flagKey.end(), flagKey.begin(), ::tolower);
		const char* displayFlag = T(flagKey.c_str(), flagNames[i].c_str());
		ImGui::Text("[%s]", displayFlag);
		ImGui::PopStyleColor();
	}

	// Return true if the base name (largest, most visible part) was hovered
	return baseNameHovered;
}

// Helper function to get color for a specific weather flag
ImVec4 SceneSelector::GetWeatherFlagColor(RE::TESWeather::WeatherDataFlag flag)
{
	const auto& theme = Menu::GetSingleton()->GetTheme();

	switch (flag) {
	case RE::TESWeather::WeatherDataFlag::kRainy:
		return ImVec4(0.4f, 0.7f, 1.0f, 1.0f);  // Light blue for rain
	case RE::TESWeather::WeatherDataFlag::kSnow:
		return ImVec4(0.9f, 0.9f, 1.0f, 1.0f);  // Light blue-white for snow
	case RE::TESWeather::WeatherDataFlag::kPermAurora:
		return ImVec4(0.8f, 0.4f, 1.0f, 1.0f);  // Purple for aurora
	case RE::TESWeather::WeatherDataFlag::kAuroraFollowsSun:
		return ImVec4(0.9f, 0.6f, 1.0f, 1.0f);  // Light purple for aurora follows sun
	case RE::TESWeather::WeatherDataFlag::kCloudy:
		return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);  // Gray for cloudy
	case RE::TESWeather::WeatherDataFlag::kPleasant:
		return theme.StatusPalette.SuccessColor;  // Green for pleasant
	default:
		return theme.StatusPalette.InfoColor;  // Default blue
	}
}

// Helper function to get color for a specific flag name
ImVec4 SceneSelector::GetWeatherFlagColorByName(const std::string& flagName)
{
	if (const auto* info = FindWeatherFlagInfo(flagName)) {
		return GetWeatherFlagColor(info->flag);
	}

	// Default for unclassified or unknown flags
	return Menu::GetSingleton()->GetTheme().StatusPalette.Warning;
}

std::string SceneSelector::GetDisplayName(const RE::TESWeather* weather)
{
	if (!weather) {
		return "Unknown";
	}
	const char* name = weather->GetName();
	if (name && strlen(name) > 0) {
		return std::string(name);
	}
	const char* editorID = weather->GetFormEditorID();
	if (editorID && strlen(editorID) > 0) {
		return std::string(editorID);
	}
	return std::to_string(weather->GetFormID());
}

#undef I18N_KEY_PREFIX

void SceneSelector::DrawOverlay()
{
	auto player = RE::PlayerCharacter::GetSingleton();
	if (!player || !player->parentCell)
		return;

	bool overlayVisible = Menu::GetSingleton()->overlayVisible;
	static bool s_prevOverlayVisible = false;
	// If ShowInOverlay is true and overlay is visible, auto-enable the window if not already enabled
	if (WeatherDetailsWindow.ShowInOverlay && overlayVisible) {
		if (!s_prevOverlayVisible && !WeatherDetailsWindow.Enabled) {
			WeatherDetailsWindow.Enabled = true;
		}
		bool* p_open = &WeatherDetailsWindow.Enabled;
		RenderWeatherDetailsWindow(p_open);
	}
	s_prevOverlayVisible = overlayVisible;
}

bool SceneSelector::IsOverlayVisible() const
{
	return WeatherDetailsWindow.ShowInOverlay;
}
