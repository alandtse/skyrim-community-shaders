#include "FeatureOverwrites.h"

#include "Menu/ThemeManager.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Utils/DevBenchUx.h"
#include "Utils/FileSystem.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

namespace
{
	using C = ThemeManager::Constants;
	constexpr size_t kVisibleExportRows = 12;

	struct ExportState
	{
		char modName[128]{};
		std::vector<std::string> features;
		std::vector<std::string> labels;
		int featureIndex = -1;
		std::vector<Util::Settings::ExportSetting> settings;
		std::vector<uint8_t> selected;
		ImGuiTextFilter filter;
		bool failed = false;
	};

	ExportState exportState;
	Util::ConfirmationPopup deletePopup;
	std::string deletePath;
	bool actionFailed = false;

	void SelectFeature(int index)
	{
		exportState.featureIndex = index;
		exportState.settings.clear();
		exportState.filter.Clear();
		exportState.failed = false;
		if (auto* feature = Feature::FindFeatureByShortName(exportState.features[index])) {
			json settings;
			globals::state->SaveToJson(settings);
			exportState.settings = Util::Settings::GetExportSettings(feature->GetShortName(), settings.at(feature->GetName()));
		}
		exportState.selected.assign(exportState.settings.size(), uint8_t{ 1 });
	}

	void BeginExport()
	{
		exportState.features.clear();
		exportState.labels.clear();
		exportState.settings.clear();
		exportState.selected.clear();
		exportState.featureIndex = -1;
		exportState.failed = false;
		exportState.filter.Clear();
		auto features = Feature::GetFeatureList();
		std::ranges::sort(features, [](Feature* a, Feature* b) { return a->GetDisplayName() < b->GetDisplayName(); });
		for (auto* feature : features) {
			if (!feature->loaded || !feature->UsesMainSettings())
				continue;
			json settings;
			feature->SaveSettings(settings);
			if (!settings.is_object() || settings.empty())
				continue;
			exportState.features.push_back(feature->GetShortName());
			exportState.labels.push_back(feature->GetDisplayName());
		}
		ImGui::OpenPopup("##ExportFeatureOverwrites");
	}

	void DrawExport()
	{
		if (!ImGui::IsPopupOpen("##ExportFeatureOverwrites"))
			return;
		ImGui::SetNextWindowSizeConstraints(ImVec2(C::Em(30.0f), 0.0f),
			ImVec2(C::Em(30.0f), ImGui::GetMainViewport()->WorkSize.y));
		auto popup = Util::Popup("##ExportFeatureOverwrites", T("feature.feature_overwrites.export.title", "Export Feature Settings"));
		if (!popup)
			return;
		ImGui::InputText(T("feature.feature_overwrites.export.mod_name", "Mod Name"), exportState.modName, IM_ARRAYSIZE(exportState.modName));
		const auto modName = Util::FileHelpers::SanitizeFileName(exportState.modName);
		ImGui::TextWrapped("%s", T("feature.feature_overwrites.export.description", "Choose one feature and the settings to export, without scene-specific values."));
		ImGui::TextWrapped("%s", T("feature.feature_overwrites.export.existing", "Existing files with the same name are updated. Other settings and metadata are preserved."));
		ImGui::SetNextItemWidth(-FLT_MIN);
		const auto* preview = exportState.featureIndex >= 0 ? exportState.labels[exportState.featureIndex].c_str() :
		                                                      T("feature.feature_overwrites.export.select_feature", "Select Feature...");
		if (Util::BeginSearchableCombo("##Feature", preview)) {
			for (size_t i = 0; i < exportState.features.size(); ++i) {
				ImGui::PushID(exportState.features[i].c_str());
				if (Util::SearchableComboMatches(exportState.labels[i]) &&
					ImGui::Selectable(exportState.labels[i].c_str(), static_cast<int>(i) == exportState.featureIndex))
					SelectFeature(static_cast<int>(i));
				ImGui::PopID();
			}
			Util::EndSearchableCombo();
		}
		if (exportState.featureIndex < 0)
			return;
		Util::DrawSelectionButtons(exportState.selected,
			T("feature.feature_overwrites.select_all", "Select All"),
			T("feature.feature_overwrites.select_none", "Select None"));
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::InputTextWithHint("##SettingsFilter", T("feature.feature_overwrites.export.search", "Search Settings"),
				exportState.filter.InputBuf, IM_ARRAYSIZE(exportState.filter.InputBuf)))
			exportState.filter.Build();
		std::vector<size_t> visible;
		for (size_t i = 0; i < exportState.settings.size(); ++i)
			if (exportState.filter.PassFilter(exportState.settings[i].label.c_str()))
				visible.push_back(i);
		const auto rows = std::clamp(visible.size(), size_t{ 1 }, kVisibleExportRows);
		const float rowHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
		const float height = rowHeight * static_cast<float>(rows) + ImGui::GetStyle().WindowPadding.y * 2.0f;
		if (ImGui::BeginChild("##Settings", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
			if (ImGui::BeginTable("##SettingList", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(visible.size()), rowHeight);
				while (clipper.Step())
					for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
						const auto i = visible[row];
						const auto& setting = exportState.settings[i];
						ImGui::PushID(setting.path.c_str());
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						bool selected = exportState.selected[i] != 0;
						if (ImGui::Checkbox(setting.label.c_str(), &selected))
							exportState.selected[i] = selected;
						Util::AddTooltip(setting.path.c_str());
						ImGui::PopID();
					}
				ImGui::EndTable();
			}
		}
		ImGui::EndChild();
		if (exportState.failed)
			Util::Text::WrappedError("%s", T("feature.feature_overwrites.export.failed", "Could not export the selected settings. Check the log and try again."));
		const auto count = std::ranges::count(exportState.selected, uint8_t{ 1 });
		auto disabled = Util::DisableGuard(modName.empty() || count == 0);
		const auto label = std::vformat(T("feature.feature_overwrites.export.count", "Export ({0})"), std::make_format_args(count));
		if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
			std::vector<std::string> selected;
			for (size_t i = 0; i < exportState.settings.size(); ++i)
				if (exportState.selected[i])
					selected.push_back(exportState.settings[i].path);
			json settings;
			globals::state->SaveToJson(settings);
			exportState.failed = !SettingsOverrideManager::GetSingleton()->ExportSettings(modName,
				exportState.features[exportState.featureIndex], selected, settings);
			if (!exportState.failed)
				ImGui::CloseCurrentPopup();
		}
	}
}

std::pair<std::string, std::vector<std::string>> FeatureOverwrites::GetFeatureSummary()
{
	return { T("feature.feature_overwrites.description", "Manage and export mod-provided feature overwrites."), {} };
}

void FeatureOverwrites::DrawSettings()
{
	auto* manager = SettingsOverrideManager::GetSingleton();
	if (ImGui::Button(T("feature.feature_overwrites.export.button", "Export Settings")))
		BeginExport();
	if (actionFailed)
		Util::Text::WrappedError("%s", T("feature.feature_overwrites.action_failed", "Could not update the overwrite. Check the log and try again."));

	bool any = false;
	if (ImGui::BeginTable("##OverwriteFiles", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn(T("feature.feature_overwrites.feature", "Feature"));
		ImGui::TableSetupColumn(T("feature.feature_overwrites.mod", "Mod"));
		ImGui::TableSetupColumn(T("feature.feature_overwrites.file", "File"));
		ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();
		for (const auto& info : manager->GetOverrides()) {
			if (!info.enabled || !manager->IsApplicable(info))
				continue;
			any = true;
			ImGui::PushID(info.filePath.c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			auto* feature = Feature::FindFeatureByShortName(info.featureName);
			const auto name = feature ? feature->GetDisplayName() : T("feature.feature_overwrites.global", "Global");
			ImGui::TextUnformatted(name.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(info.modName.c_str());
			ImGui::TableNextColumn();
			const auto filename = std::filesystem::path(info.filePath).filename().string();
			ImGui::TextUnformatted(filename.c_str());
			Util::AddTooltip(info.filePath.c_str());
			ImGui::TableNextColumn();
			if (ImGui::SmallButton(T("feature.feature_overwrites.delete", "Delete"))) {
				deletePath = info.filePath;
				deletePopup.title = T("feature.feature_overwrites.delete_title", "Delete Feature Overwrite?");
				deletePopup.message = std::vformat(T("feature.feature_overwrites.delete_message", "Delete '{0}' from disk and remove orphaned user overwrite entries? Saved personal settings are kept."), std::make_format_args(filename));
				deletePopup.confirmLabel = T("feature.feature_overwrites.delete", "Delete");
				deletePopup.cancelLabel = T("feature.feature_overwrites.cancel", "Cancel");
				deletePopup.Request();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (!any)
		ImGui::TextDisabled("%s", T("feature.feature_overwrites.empty", "No feature overwrites are currently applied."));
	if (deletePopup.Draw())
		actionFailed = !manager->DeleteFile(deletePath);
	DrawExport();
}

void FeatureOverwrites::RegisterUxActions()
{
	FEATURE_QUERY("files", "List discovered overwrite files with filePath, feature, modName, enabled and applicable. enabled comes from file metadata; applicable means the target is loaded or Global. Args: none.",
		([](const Feature*, const json&) {
			auto* manager = SettingsOverrideManager::GetSingleton();
			json result = json::array();
			for (const auto& info : manager->GetOverrides())
				result.push_back({ { "filePath", info.filePath }, { "feature", info.featureName }, { "modName", info.modName },
					{ "enabled", info.enabled }, { "applicable", manager->IsApplicable(info) } });
			return result;
		}));
	FEATURE_COMMAND("deleteFile", "Delete one discovered, applicable overwrite file and reapply affected settings, preserving saved personal settings, unsaved normal feature edits and Scene Manager drafts. Args: filePath from files, confirm=true. Removes companion user entries no remaining overwrite owns, deleting empty companion files. Permanent disk deletion; restore installed files by reinstalling their providing mod. Boot preferences update for the next restart.",
		([](Feature*, const json& args) {
			if (!args.value("confirm", false) || !SettingsOverrideManager::GetSingleton()->DeleteFile(args.at("filePath").get<std::string>()))
				throw std::runtime_error("Deletion requires a discovered file and confirm=true");
		}));
	FEATURE_QUERY("exportSettings", "List exportable settings for one loaded feature. Args: feature=shortName. Returns path (JSON pointer) and display label for each setting, including catalogue-independent fallback entries. Arrays are one setting.",
		([](const Feature*, const json& args) {
			const auto name = args.at("feature").get<std::string>();
			auto* feature = Feature::FindFeatureByShortName(name);
			if (!feature || !feature->UsesMainSettings())
				throw std::runtime_error("Unknown or unavailable feature");
			json settings;
			globals::state->SaveToJson(settings);
			json result = json::array();
			for (const auto& setting : Util::Settings::GetExportSettings(name, settings.at(feature->GetName())))
				result.push_back({ { "path", setting.path }, { "label", setting.label } });
			return result;
		}));
	FEATURE_COMMAND("exportSettings", "Export selected settings from one loaded feature, including unsaved normal feature edits but excluding applied Scene Manager values and toolbar drafts. Args: modName=nonempty prefix, feature=shortName, settings=[JSON pointer paths from exportSettings query]. Writes Overrides/ModName_Feature.json, updating selected keys while preserving all other existing keys and metadata. Rejects empty/unknown paths, partial array components and external changes to a previously discovered target until settings are reloaded. Updates only the exported file in the live inventory, preserving conflict checks for other files. Does not save the main config.",
		([](Feature*, const json& args) {
			json settings;
			globals::state->SaveToJson(settings);
			const auto paths = args.at("settings").get<std::vector<std::string>>();
			if (!SettingsOverrideManager::GetSingleton()->ExportSettings(args.at("modName").get<std::string>(),
					args.at("feature").get<std::string>(), paths, settings))
				throw std::runtime_error("Feature setting export failed");
		}));
}
