#include "MenuManager.h"

#include "EffectManager.h"
#include "Features/Effects11.h"
#include "Features/PostProcessing.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "PresetManager.h"
#include "SettingManager.h"
#include "State.h"
#include "TextureManager.h"
#include "Utils/UI.h"

static const char* const timeOfDayNames[] = { "Dawn", "Sunrise", "Day", "Sunset", "Dusk", "Night", "InteriorDay", "InteriorNight" };

namespace
{
	// Finds a_location's own state for a_flagName, or nullptr if it has no entry for it.
	const PresetEffectStatus* FindStatus(const PresetLocation& a_location, const std::string& a_flagName)
	{
		for (const auto& status : a_location.effectStatus) {
			if (status.flagName == a_flagName)
				return &status;
		}
		return nullptr;
	}

	// Hover tooltip for a preset-location picker entry: header comment, then a captioned
	// diff against a_active (or plain state if a_active is nullptr or is a_location).
	void RenderPresetTooltip(const PresetLocation& a_location, const PresetLocation* a_active)
	{
		if (!ImGui::IsItemHovered())
			return;

		ImGui::BeginTooltip();
		if (!a_location.headerComment.empty())
			ImGui::TextUnformatted(a_location.headerComment.c_str());

		const bool isActiveLocation = a_active && a_active->root == a_location.root;
		const auto& palette = globals::menu->GetSettings().Theme.StatusPalette;

		if (isActiveLocation) {
			// Its real settings are already live in the panel below -- repeating them
			// here (with no diff to show against itself) would just be noise.
			ImGui::TextColored(palette.InfoColor, "%s", T("feature.effects11.preset_tooltip_is_active", "(currently selected -- see settings below)"));
		} else if (a_active) {
			ImGui::TextColored(palette.InfoColor, T("feature.effects11.preset_tooltip_diff_vs", "Compared to selected preset (%s):"), a_active->label.c_str());
			for (const auto& status : a_location.effectStatus) {
				const PresetEffectStatus* activeStatus = FindStatus(*a_active, status.flagName);
				const bool activeEnabled = activeStatus && activeStatus->enabled;

				if (status.enabled && !status.fileExists) {
					ImGui::TextColored(palette.Error, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_broken", "on, file missing"));
				} else if (status.enabled && !activeEnabled) {
					ImGui::TextColored(palette.SuccessColor, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_added", "on (added)"));
				} else if (!status.enabled && activeEnabled) {
					ImGui::TextColored(palette.Error, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_removed", "off (active has it on)"));
				} else if (status.enabled) {
					ImGui::Text("%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_on", "on"));
				} else {
					ImGui::TextColored(palette.Disable, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_off", "off"));
				}
			}
		} else {
			// No active preset to diff against (nothing loaded yet), so nothing below
			// duplicates this -- show plain state instead of a diff.
			for (const auto& status : a_location.effectStatus) {
				if (status.enabled && !status.fileExists)
					ImGui::TextColored(palette.Error, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_broken", "on, file missing"));
				else if (status.enabled)
					ImGui::Text("%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_on", "on"));
				else
					ImGui::TextColored(palette.Disable, "%s: %s", status.flagName.c_str(), T("feature.effects11.preset_flag_off", "off"));
			}
		}
		ImGui::EndTooltip();
	}
}

MenuManager& MenuManager::GetSingleton()
{
	static MenuManager instance;
	return instance;
}

void MenuManager::RenderImGui()
{
	if (!ImGui::BeginTable("Effects11", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV, ImVec2(0, 0))) {
		return;
	}

	ImGui::TableSetupColumn("Settings", ImGuiTableColumnFlags_WidthFixed, 400.0f);
	ImGui::TableSetupColumn("Effects", ImGuiTableColumnFlags_WidthStretch);

	ImGui::TableNextRow();

	// Left side - Settings
	ImGui::TableSetColumnIndex(0);
	if (ImGui::BeginChild("Settings", ImVec2(0, 0), false)) {
		RenderSettingsPanel();
	}
	ImGui::EndChild();

	// Right side - Effects
	ImGui::TableSetColumnIndex(1);
	if (ImGui::BeginChild("Effects", ImVec2(0, 0), false)) {
		EffectManager::GetSingleton().RenderEffectsList();
	}
	ImGui::EndChild();

	ImGui::EndTable();
}

void MenuManager::RenderSettingsPanel()
{
	auto& settingManager = SettingManager::GetSingleton();
	auto& effectManager = EffectManager::GetSingleton();
	auto& effects11 = globals::features::effects11;
	auto& presetManager = PresetManager::GetSingleton();

	const auto& locations = presetManager.GetDiscoveredLocations();
	const auto* active = presetManager.GetActiveLocation();

	if (locations.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, globals::menu->GetSettings().Theme.StatusPalette.Warning);
		ImGui::TextWrapped("%s", T("feature.effects11.no_preset_found",
									 "No ENB preset found (checked game root, Data, and Data subfolders)."));
		ImGui::PopStyleColor();
	} else {
		const std::string previewLabel = active ? active->label : (effects11.settings.presetLocation.empty() ? T("feature.effects11.preset_none_selected", "(none selected)") : T("feature.effects11.preset_missing", "(missing) ") + effects11.settings.presetLocation);
		if (ImGui::BeginCombo(T("feature.effects11.preset_location", "Preset location"), previewLabel.c_str())) {
			for (const auto& loc : locations) {
				const bool isSelected = active && active->root == loc.root;
				if (ImGui::Selectable(loc.label.c_str(), isSelected)) {
					presetManager.SetActiveLocation(loc.root);
					effects11.settings.presetLocation = presetManager.ToRelativeKey(loc.root);
					// After Load()/Apply(), not before: Save() piggybacks an ENB save for
					// whatever preset is currently loaded, which would still be the old one.
					settingManager.Load();
					effectManager.Apply();
					globals::state->Save();
				}
				RenderPresetTooltip(loc, active);
			}
			ImGui::EndCombo();
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(T("feature.effects11.rescan_presets", "Rescan"))) {
		presetManager.Rescan();
	}

	// Without a resolved location, GetENBSeriesPath() returns empty and Save/Load
	// silently read/write a bare relative filename instead of the preset's ini.
	const bool hasActiveLocation = presetManager.GetActiveLocation() != nullptr;
	// Without a preset there is no ini to write back to, so saving would only create stubs
	const bool presetLoaded = effectManager.IsPresetLoaded();
	const bool canSave = presetLoaded && hasActiveLocation;

	ImGui::BeginDisabled(!canSave);
	if (ImGui::Button("Save & Apply")) {
		settingManager.Save();
		effectManager.Save();
		settingManager.Load();
		effectManager.Apply();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(hasActiveLocation ? "Save all settings, then reload and recompile shaders" : "Select a preset location first");
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!hasActiveLocation);
	if (ImGui::Button("Load & Apply")) {
		settingManager.Load();
		effectManager.Apply();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(hasActiveLocation ? "Load all settings from enbseries.ini, weather files, and effect configurations, reload shaders" : "Select a preset location first");
	}

	ImGui::SameLine();

	if (ImGui::Button("Load")) {
		settingManager.Load();
		effectManager.Load();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(hasActiveLocation ? "Load all settings from enbseries.ini, weather files, and effect configurations" : "Select a preset location first");
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!canSave);
	if (ImGui::Button("Save")) {
		settingManager.Save();
		effectManager.Save();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(hasActiveLocation ? "Save all settings to enbseries.ini, weather files, and effect configurations" : "Select a preset location first");
	}
	ImGui::EndDisabled();

	// Static caveat, not a preset check: third-party .fx content still assumes a single flat eye.
	if (globals::game::isVR) {
		ImGui::PushStyleColor(ImGuiCol_Text, globals::menu->GetSettings().Theme.StatusPalette.Warning);
		ImGui::TextWrapped("%s", T("feature.effects11.vr_flat_screen_warning", "VR: arbitrary flat-screen ENB presets are not guaranteed correct here -- screen-centered effects (vignette, lens flare, radial blur) may appear off-center or seam-darkened. Prefer a preset built or tested for VR."));
		ImGui::PopStyleColor();
	}

	ImGui::Separator();

	if (globals::state->GetTonemapOwner() == State::TonemapOwner::kEffects11 &&
		globals::features::postProcessing.loaded &&
		globals::features::postProcessing.WantsTonemapOwnership()) {
		ImGui::TextColored(
			Menu::GetSingleton()->GetTheme().StatusPalette.Warning,
			"%s",
			T("feature.effects11.post_processing_tonemap_override",
				"Effects 11 is overriding Post Processing's tonemapping. Enable \"UseOriginalPostProcessing\" below to hand it back."));
		ImGui::Separator();
	}

	if (ImGui::BeginChild("SettingsScroll", ImVec2(0, 0), false)) {
		RenderAllSettings();
	}
	ImGui::EndChild();
}

void MenuManager::RenderWeatherControl()
{
	auto& effectManager = EffectManager::GetSingleton();

	// Current weather status
	uint32_t currentWeatherID = static_cast<uint32_t>(effectManager.commonData.weather[0]);
	uint32_t lastWeatherID = static_cast<uint32_t>(effectManager.commonData.weather[1]);
	float blendFactor = effectManager.commonData.weather[2];

	ImGui::Text("Current Weather: 0x%X, Outgoing Weather: 0x%X", currentWeatherID, lastWeatherID);
	ImGui::Text("Weather Blend Factor: %.2f", blendFactor);
}

void MenuManager::RenderDebugControl()
{
	auto& effectManager = EffectManager::GetSingleton();
	auto& weatherManager = WeatherManager::GetSingleton();

	// Current time of day values
	if (ImGui::BeginTable("TimeOfDayValues", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
		ImGui::TableSetupColumn("Period", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();

		const char* tod1Names[] = { "Dawn", "Sunrise", "Day", "Sunset" };
		for (int i = 0; i < 4; ++i) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%s", tod1Names[i]);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", effectManager.commonData.timeOfDay1[i]);
		}

		const char* tod2Names[] = { "Dusk", "Night", "InteriorDay", "InteriorNight" };
		for (int i = 0; i < 4; ++i) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%s", tod2Names[i]);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", effectManager.commonData.timeOfDay2[i]);
		}

		ImGui::EndTable();
	}

	ImGui::Separator();

	// Weather file list
	if (ImGui::TreeNodeEx("Loaded Weather Files", ImGuiTreeNodeFlags_DefaultOpen)) {
		const auto& weatherEntries = weatherManager.GetWeatherEntries();

		if (!weatherEntries.empty()) {
			if (ImGui::BeginChild("WeatherList", ImVec2(0, 300), true)) {
				// Sort weather entries by name for consistent display
				std::vector<std::pair<std::string, const WeatherManager::WeatherEntry*>> sortedWeathers;
				for (const auto& [key, entry] : weatherEntries) {
					sortedWeathers.emplace_back(key, &entry);
				}
				std::sort(sortedWeathers.begin(), sortedWeathers.end());

				for (const auto& [key, entry] : sortedWeathers) {
					ImGui::PushID(key.c_str());

					// Show weather file name and IDs
					ImGui::Text("%s", entry->fileName.c_str());
					ImGui::SameLine();
					ImGui::Text("(%s)", key.c_str());

					// Show weather IDs on same line
					ImGui::SameLine();
					std::string idsText = "IDs: ";
					for (size_t i = 0; i < entry->weatherIDs.size() && i < 3; ++i) {
						if (i > 0)
							idsText += ", ";
						idsText += std::format("0x{:X}", entry->weatherIDs[i]);
					}
					if (entry->weatherIDs.size() > 3) {
						idsText += "...";
					}
					ImGui::Text("%s", idsText.c_str());

					ImGui::PopID();
				}
			}
			ImGui::EndChild();
		} else {
			ImGui::Text("No weather files loaded");
			ImGui::Text("Make sure _weatherlist.ini exists in enbseries folder");
		}

		ImGui::TreePop();
	}
}

float MenuManager::GetTimeOfDayBlendFactor(int timeIndex) const
{
	auto& effectManager = EffectManager::GetSingleton();
	const auto& commonData = effectManager.commonData;

	// Return the actual blend factor for each time period
	switch (timeIndex) {
	case 0:
		return commonData.timeOfDay1[0];  // Dawn
	case 1:
		return commonData.timeOfDay1[1];  // Sunrise
	case 2:
		return commonData.timeOfDay1[2];  // Day
	case 3:
		return commonData.timeOfDay1[3];  // Sunset
	case 4:
		return commonData.timeOfDay2[0];  // Dusk
	case 5:
		return commonData.timeOfDay2[1];  // Night
	case 6:
		return commonData.timeOfDay2[2];  // InteriorDay
	case 7:
		return commonData.timeOfDay2[3];  // InteriorNight
	default:
		return 0.0f;
	}
}

void MenuManager::RenderAllSettings()
{
	auto& settingManager = SettingManager::GetSingleton();
	auto categorizedSettings = SettingManager::GetSingleton().GetCategorizedSettings();

	// Define explicit order for tabs
	const std::vector<std::string> tabOrder = { "Main", "Weather", "Debug" };

	if (ImGui::BeginTabBar("SettingsTabBar", ImGuiTabBarFlags_None)) {
		for (const auto& tabName : tabOrder) {
			if (categorizedSettings.find(tabName) == categorizedSettings.end())
				continue;

			const auto& categories = categorizedSettings[tabName];

			if (ImGui::BeginTabItem(tabName.c_str())) {
				// Add weather control to the Weather tab
				if (tabName == "Weather") {
					RenderWeatherControl();
					ImGui::Separator();
				}

				if (tabName == "Debug") {
					RenderDebugControl();
				}

				for (const auto& category : categories) {
					if (!settingManager.IsCategoryEnabled(category))
						continue;

					ImGuiTreeNodeFlags flags = (tabName == "Weather") ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen;

					if (ImGui::TreeNodeEx(category.c_str(), flags)) {
						bool categoryDisabled = false;
						if (category == "RAIN") {
							if (!globals::features::effects11.raindropStatus.empty()) {
								categoryDisabled = true;
								ImGui::PushStyleColor(ImGuiCol_Text, globals::menu->GetSettings().Theme.StatusPalette.Warning);
								ImGui::TextWrapped("Rain disabled: %s", globals::features::effects11.raindropStatus.c_str());
								ImGui::PopStyleColor();
							}
						}

						auto settings = settingManager.GetSettingsByCategory(category);

						if (!categoryDisabled && ImGui::BeginTable((category + "_table").c_str(), 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
							ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthFixed);
							ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

							// Add weather ignore controls for categories with weather support (Weather tab only)
							if (tabName == "Weather" && settingManager.CategoryHasWeatherSupport(category)) {
								auto& effectManager = EffectManager::GetSingleton();
								bool isInterior = effectManager.commonData.eInteriorFactor > 0.5f;

								if (!isInterior) {
									// Show exterior ignore setting when outside
									ImGui::TableNextRow();
									ImGui::TableSetColumnIndex(0);
									ImGui::Text("IgnoreWeatherSystem");
									ImGui::TableSetColumnIndex(1);
									bool ignoreWeather = settingManager.GetIgnoreWeatherSystem(category);
									if (ImGui::Checkbox(("##IgnoreWeatherSystem_" + category).c_str(), &ignoreWeather)) {
										settingManager.SetIgnoreWeatherSystem(category, ignoreWeather);
									}
									if (ImGui::IsItemHovered()) {
										ImGui::SetTooltip("When enabled, uses enbseries.ini values instead of weather-specific values for exterior areas");
									}
								} else if (!settingManager.IsCategoryExteriorOnly(category)) {
									// Show interior ignore setting when inside (skip for exterior-only categories)
									ImGui::TableNextRow();
									ImGui::TableSetColumnIndex(0);
									ImGui::Text("IgnoreWeatherSystemInterior");
									ImGui::TableSetColumnIndex(1);
									bool ignoreWeatherInterior = settingManager.GetIgnoreWeatherSystemInterior(category);
									if (ImGui::Checkbox(("##IgnoreWeatherSystemInterior_" + category).c_str(), &ignoreWeatherInterior)) {
										settingManager.SetIgnoreWeatherSystemInterior(category, ignoreWeatherInterior);
									}
									if (ImGui::IsItemHovered()) {
										ImGui::SetTooltip("When enabled, uses enbseries.ini values instead of weather-specific values for interior areas");
									}
								}

								// Add separator
								ImGui::TableNextRow();
								ImGui::TableSetColumnIndex(0);
								ImGui::Separator();
								ImGui::TableSetColumnIndex(1);
								ImGui::Separator();
							}

							for (const auto& settingKey : settings) {
								auto settingInfo = settingManager.GetSettingInfo(settingKey, category);
								if (!settingInfo)
									continue;

								bool isTod = (settingInfo->type == SettingType::TimeOfDay || settingInfo->type == SettingType::ColorTimeOfDay);
								if (tabName == "Main" && isTod)
									continue;
								if (tabName == "Weather" && !isTod)
									continue;

								if (!settingManager.IsSettingEnabled(settingKey, category))
									continue;

								uint32_t settingID = settingInfo->id;

								ImGui::TableNextRow();
								ImGui::TableSetColumnIndex(0);
								ImGui::Text("%s", settingKey.c_str());
								ImGui::TableSetColumnIndex(1);

								switch (settingInfo->type) {
								case SettingType::Bool:
									{
										// Covers both a missing preset and one whose enbeffect.fx failed to compile
										const bool noPreset = category == "GLOBAL" && settingKey == "UseEffect" && !EffectManager::GetSingleton().IsPresetLoaded();

										bool v = !noPreset && settingManager.GetValue<bool>(settingID, true);
										ImGui::BeginDisabled(noPreset);
										if (ImGui::Checkbox(("##" + settingKey).c_str(), &v)) {
											settingManager.SetValue<bool>(settingID, v);
										}
										ImGui::EndDisabled();

										if (noPreset) {
											ImGui::SameLine();
											ImGui::PushStyleColor(ImGuiCol_Text, globals::menu->GetSettings().Theme.StatusPalette.Warning);
											ImGui::TextUnformatted(T("feature.effects11.no_valid_preset", "No valid preset loaded"));
											ImGui::PopStyleColor();
										}
										break;
									}
								case SettingType::Float:
									{
										float v = settingManager.GetValue<float>(settingID, true);
										if (ImGui::InputFloat(("##" + settingKey).c_str(), &v, settingInfo->step, settingInfo->step * 10.0f, "%.2f")) {
											// Clamp value between min and max after input
											v = std::clamp(v, settingInfo->minValue, settingInfo->maxValue);
											settingManager.SetValue<float>(settingID, v);
										}
										break;
									}
								case SettingType::TimeOfDay:
									{
										auto v = settingManager.GetValue<TimeOfDayValue>(settingID, true);
										bool exteriorOnly = settingManager.IsCategoryExteriorOnly(category);

										bool changed = false;
										bool firstRow = true;

										for (int i = 0; i < 8; ++i) {
											if (exteriorOnly && i >= 6)
												continue;

											if (!firstRow) {
												ImGui::TableNextRow();
												ImGui::TableSetColumnIndex(0);
												ImGui::TableSetColumnIndex(1);
											}
											firstRow = false;

											// Style the input based on activity
											float blendFactor = GetTimeOfDayBlendFactor(i);
											bool isActive = blendFactor > 0.0f;

											if (!isActive) {
												// Inactive inputs: dim the appearance
												ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
											}

											std::string label = std::string(timeOfDayNames[i]) + "##" + settingKey + std::to_string(i);
											if (ImGui::InputFloat(label.c_str(), &v.values[i], settingInfo->step, settingInfo->step * 10.0f, "%.2f")) {
												// Clamp value between min and max after input
												v.values[i] = std::clamp(v.values[i], settingInfo->minValue, settingInfo->maxValue);
												changed = true;
											}

											if (ImGui::IsItemHovered()) {
												ImGui::SetTooltip("%.0f%%", blendFactor * 100.0f);
											}

											if (!isActive) {
												ImGui::PopStyleVar();
											}
										}

										if (changed) {
											settingManager.SetValue<TimeOfDayValue>(settingID, v);
										}
										break;
									}
								case SettingType::ColorTimeOfDay:
									{
										auto v = settingManager.GetValue<ColorTimeOfDayValue>(settingID, true);
										bool exteriorOnly = settingManager.IsCategoryExteriorOnly(category);

										bool changed = false;
										bool firstRow = true;

										for (int i = 0; i < 8; ++i) {
											if (exteriorOnly && i >= 6)
												continue;

											if (!firstRow) {
												ImGui::TableNextRow();
												ImGui::TableSetColumnIndex(0);
												ImGui::TableSetColumnIndex(1);
											}
											firstRow = false;

											// Style the color picker based on activity
											float blendFactor = GetTimeOfDayBlendFactor(i);
											bool isActive = blendFactor > 0.0f;

											if (!isActive) {
												// Inactive sliders: dim the appearance
												ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
											}

											std::string label = std::string(timeOfDayNames[i]) + "##" + settingKey + std::to_string(i);
											float color[3] = { v.values[i].x, v.values[i].y, v.values[i].z };

											if (ImGui::ColorEdit3(label.c_str(), color, ImGuiColorEditFlags_NoInputs)) {
												v.values[i].x = color[0];
												v.values[i].y = color[1];
												v.values[i].z = color[2];
												changed = true;
											}

											if (ImGui::IsItemHovered()) {
												ImGui::SetTooltip("%.0f%%", blendFactor * 100.0f);
											}

											if (!isActive) {
												ImGui::PopStyleVar();
											}
										}

										if (changed) {
											settingManager.SetValue<ColorTimeOfDayValue>(settingID, v);
										}
										break;
									}
								}
							}

							ImGui::EndTable();
						}
						ImGui::TreePop();
					}
				}
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}
}
