#include "SettingsOverrideManager.h"

#include "Feature.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/SettingsPatch.h"

#include <fstream>

namespace
{
	constexpr size_t kMaxOverrideBytes = 1024 * 1024;
	constexpr int kJsonIndent = 2;

	bool ReadOverrideDocument(const std::filesystem::path& path, json& data)
	{
		std::error_code error;
		const auto size = std::filesystem::file_size(path, error);
		if (error || size == 0 || size > kMaxOverrideBytes)
			return false;
		try {
			std::ifstream input(path);
			input >> data;
			return data.is_object();
		} catch (const std::exception& e) {
			logger::error("Could not read overwrite '{}': {}", path.string(), e.what());
			return false;
		}
	}

	struct FileChange
	{
		std::filesystem::path path;
		json before;
		json after;
		bool remove = false;
	};

	bool ReadFileChange(const std::filesystem::path& path, FileChange& change)
	{
		std::error_code error;
		const bool exists = std::filesystem::exists(path, error);
		change.path = exists ? Util::FileHelpers::ResolveExistingFile(path) : path;
		return !error && !change.path.empty() && (!exists || ReadOverrideDocument(change.path, change.before));
	}

	bool CollectUserCleanup(SettingsOverrideManager& manager, std::vector<FileChange>& changes)
	{
		std::error_code error;
		const auto directory = manager.GetUserOverridesDirectory();
		if (!std::filesystem::exists(directory, error))
			return !error;
		for (std::filesystem::directory_iterator file(directory, error), end; !error && file != end; file.increment(error)) {
			const auto name = file->path().filename().string();
			constexpr std::string_view suffix = ".user.json";
			if (!name.ends_with(suffix) || !file->is_regular_file(error))
				continue;
			FileChange change;
			if (!ReadFileChange(file->path(), change))
				return false;
			change.after = Util::Settings::SelectSettings(change.before,
				manager.GetMergedOverrideSettings(name.substr(0, name.size() - suffix.size()), json::object()));
			change.remove = change.after.empty();
			if (change.after != change.before || change.remove)
				changes.push_back(std::move(change));
		}
		return !error;
	}

	bool CommitFileChanges(const std::vector<FileChange>& changes, const std::function<bool()>& saveUserSettings)
	{
		for (size_t i = 0; i <= changes.size(); ++i) {
			if (i == changes.size()) {
				if (!saveUserSettings || saveUserSettings())
					return true;
			} else {
				const auto& change = changes[i];
				std::error_code error;
				const bool saved = change.remove ? std::filesystem::remove(change.path, error) && !error :
				                                   Util::FileHelpers::WriteJsonAtomically(change.path, change.after, kJsonIndent, "feature overwrite");
				if (saved)
					continue;
				logger::error("Could not commit feature overwrite '{}'", change.path.string());
			}
			while (i-- > 0) {
				std::error_code error;
				const bool restored = changes[i].before.is_null() ?
				                          std::filesystem::remove(changes[i].path, error) && !error :
				                          Util::FileHelpers::WriteJsonAtomically(changes[i].path, changes[i].before, kJsonIndent, "overwrite rollback");
				if (!restored)
					logger::error("Could not restore overwrite '{}'", changes[i].path.string());
			}
			return false;
		}
		return true;
	}
}

bool SettingsOverrideManager::IsApplicable(const OverrideInfo& info) const
{
	if (info.isGlobal)
		return true;
	const auto* feature = Feature::FindFeatureByShortName(info.featureName);
	return feature && feature->UsesMainSettings();
}

json SettingsOverrideManager::GetLayerData(const OverrideInfo& info) const
{
	if (!info.isGlobal) {
		if (auto* feature = Feature::FindFeatureByShortName(info.featureName); feature && feature->UsesMainSettings())
			return json{ { feature->GetName(), info.overrideData } };
		return json::object();
	}
	auto layer = info.overrideData;
	for (auto* feature : Feature::GetFeatureList())
		if (!feature->loaded || !feature->UsesMainSettings())
			layer.erase(feature->GetName());
	return layer;
}

std::vector<size_t> SettingsOverrideManager::GetOrderedOverrides() const
{
	std::vector<size_t> ordered;
	if (enabled && discovered)
		for (bool global : { true, false })
			for (size_t i = 0; i < overrides.size(); ++i)
				if (overrides[i].enabled && overrides[i].isGlobal == global && IsApplicable(overrides[i]))
					ordered.push_back(i);
	return ordered;
}

void SettingsOverrideManager::RestoreBaselines(json& settings) const
{
	if (!userBaseline.is_object())
		return;
	for (auto i : GetOrderedOverrides())
		Util::Settings::RestoreSettings(settings, userBaseline, GetLayerData(overrides[i]));
}

json SettingsOverrideManager::GetPendingEdits(const json& settings)
{
	return Util::Settings::BuildUserOverride(settings, appliedSettings.is_object() ? appliedSettings : ApplyLayers(userBaseline));
}

void SettingsOverrideManager::PrepareUserSettings(json& settings)
{
	const auto current = settings;
	json installed = json::object();
	for (auto i : GetOrderedOverrides())
		installed.update(GetLayerData(overrides[i]), true);
	const auto customizations = Util::Settings::BuildUserOverride(ApplyLayers(json::object()), installed);
	RestoreBaselines(settings);
	settings.update(Util::Settings::SelectSettings(current, customizations), true);
	settings.update(GetPendingEdits(current), true);
}

bool SettingsOverrideManager::SaveUserEdits(const json& settings, const std::function<bool()>& saveUserSettings)
try {
	const auto pending = GetPendingEdits(settings);
	json featureMask = json::object();
	std::map<std::string, json> groups;
	for (auto i : GetOrderedOverrides()) {
		const auto& info = overrides[i];
		const auto disk = LoadOverrideFile(info.filePath);
		if (!disk || disk->fileHash != info.fileHash) {
			logger::error("Overwrite '{}' changed on disk; reload settings before saving", info.filePath);
			return false;
		}
		const auto layer = GetLayerData(info);
		auto& group = groups[info.isGlobal ? "Global" : info.featureName];
		if (!group.is_object())
			group = json::object();
		group.update(layer, true);
		if (!info.isGlobal)
			featureMask.update(layer, true);
	}

	std::vector<FileChange> changes;
	for (const auto& [name, layer] : groups) {
		auto edited = Util::Settings::SelectSettings(pending, layer);
		if (name == "Global")
			Util::Settings::RestoreSettings(edited, json::object(), featureMask);
		auto* feature = name == "Global" ? nullptr : Feature::FindFeatureByShortName(name);
		const auto unwrap = [&](const json& values) {
			return feature ? values.value(feature->GetName(), json::object()) : values;
		};
		FileChange change;
		if (!ReadFileChange(GetUserOverridesDirectory() / (name + ".user.json"), change))
			return false;
		const auto mask = unwrap(layer);
		change.after = Util::Settings::SelectSettings(change.before, mask);
		Util::Settings::RestoreSettings(change.after, unwrap(Util::Settings::BuildUserOverride(settings, layer)), unwrap(edited));
		change.remove = change.after.empty();
		if (change.after != change.before && !(change.remove && change.before.is_null()))
			changes.push_back(std::move(change));
	}
	auto userSettings = settings;
	PrepareUserSettings(userSettings);
	if (!CommitFileChanges(changes, saveUserSettings))
		return false;
	if (!userBaseline.is_object())
		userBaseline = json::object();
	userBaseline.update(userSettings, true);
	if (!appliedSettings.is_object())
		appliedSettings = ApplyLayers(userBaseline);
	appliedSettings.update(settings, true);
	return true;
} catch (const std::exception& e) {
	logger::error("Could not save user overwrite settings: {}", e.what());
	return false;
}

bool SettingsOverrideManager::CleanupStaleUserOverrides()
{
	if (!enabled || !discovered)
		return true;
	std::vector<FileChange> changes;
	return CollectUserCleanup(*this, changes) && CommitFileChanges(changes, {});
}

json SettingsOverrideManager::ApplyLayers(json settings)
{
	ApplyGlobalOverrides(settings);
	LoadUserOverride("Global", settings);
	for (auto* feature : Feature::GetFeatureList()) {
		if (!feature->loaded || !feature->UsesMainSettings())
			continue;
		auto& values = settings[feature->GetName()];
		ApplyOverrides(feature->GetShortName(), values);
		LoadUserOverride(feature->GetShortName(), values);
	}
	return settings;
}

void SettingsOverrideManager::RebuildOverrideIndex()
{
	featureOverrideMap.clear();
	for (size_t i = 0; i < overrides.size(); ++i)
		if (!overrides[i].isGlobal)
			featureOverrideMap[overrides[i].featureName].push_back(i);
}

void SettingsOverrideManager::ReapplyChangedSettings(const json& current, const json& underlying, const json& mask, const json& pending)
{
	const auto previousApplied = appliedSettings.is_object() ? appliedSettings : ApplyLayers(underlying);
	auto updated = current;
	Util::Settings::RestoreSettings(updated, ApplyLayers(underlying), mask);
	updated.update(pending, true);
	if (updated != current)
		globals::state->LoadFromJson(updated);
	globals::state->SaveToJson(appliedSettings);
	Util::Settings::RestoreSettings(appliedSettings, previousApplied, pending);
}

bool SettingsOverrideManager::DeleteFile(const std::string& filePath)
{
	const auto found = std::ranges::find(overrides, filePath, &OverrideInfo::filePath);
	if (found == overrides.end() || !IsApplicable(*found) || !userBaseline.is_object())
		return false;
	json current;
	globals::state->SaveToJson(current);
	auto underlying = current;
	RestoreBaselines(underlying);
	const auto pendingEdits = GetPendingEdits(current);
	const auto mask = GetLayerData(*found);
	FileChange removal;
	if (!ReadFileChange(filePath, removal) || removal.before.is_null())
		return false;
	removal.remove = true;
	const auto index = static_cast<size_t>(found - overrides.begin());
	const auto removed = *found;
	overrides.erase(found);
	RebuildOverrideIndex();
	std::vector<FileChange> changes;
	const bool prepared = CollectUserCleanup(*this, changes);
	changes.push_back(std::move(removal));
	if (!prepared || !CommitFileChanges(changes, {})) {
		overrides.insert(overrides.begin() + index, removed);
		RebuildOverrideIndex();
		return false;
	}
	ReapplyChangedSettings(current, underlying, mask, pendingEdits);
	return true;
}

bool SettingsOverrideManager::ExportSettings(const std::string& modName, const std::string& featureName,
	std::span<const std::string> settingPaths, const json& settings)
{
	const auto safeName = Util::FileHelpers::SanitizeFileName(modName);
	auto* feature = Feature::FindFeatureByShortName(featureName);
	if (safeName.empty() || settingPaths.empty() || !feature || !feature->UsesMainSettings() ||
		!settings.contains(feature->GetName()))
		return false;
	const auto& values = settings.at(feature->GetName());
	const auto available = Util::Settings::GetExportSettings(featureName, values);
	for (const auto& selected : settingPaths)
		if (std::ranges::find(available, selected, &Util::Settings::ExportSetting::path) == available.end())
			return false;
	const auto selected = Util::Settings::SelectSettingPaths(values, { settingPaths.begin(), settingPaths.end() });
	const auto path = GetOverridesDirectory() / std::format("{}_{}.json", safeName, featureName);
	if (selected.empty() || !Util::PathHelpers::IsPathLexicallyWithinDirectory(GetOverridesDirectory(), path))
		return false;
	json document = json::object();
	std::error_code error;
	const bool exists = std::filesystem::exists(path, error);
	const auto destination = exists ? Util::FileHelpers::ResolveExistingFile(path) : path;
	if (error || destination.empty() || (exists && !ReadOverrideDocument(destination, document)))
		return false;
	const auto found = std::ranges::find_if(overrides, [&](const auto& info) {
		return Util::IEquals(std::filesystem::path(info.filePath).filename().string(), path.filename().string());
	});
	if (found != overrides.end()) {
		const auto current = LoadOverrideFile(found->filePath);
		if (!current || current->fileHash != found->fileHash) {
			logger::error("Overwrite '{}' changed on disk; reload settings before exporting", found->filePath);
			return false;
		}
	}
	auto underlying = settings;
	RestoreBaselines(underlying);
	const auto pendingEdits = GetPendingEdits(settings);
	document.update(selected, true);
	if (!ValidateOverrideFormat(document, path.string()) || !ValidateJsonDataTypes(document, "", path.string()) ||
		!Util::FileHelpers::WriteJsonAtomically(destination, document, kJsonIndent, "exported feature overwrite"))
		return false;
	auto refreshed = LoadOverrideFile(found != overrides.end() ? found->filePath : path.string());
	if (!refreshed)
		return false;
	const auto mask = GetLayerData(*refreshed);
	if (found != overrides.end()) {
		*found = std::move(*refreshed);
	} else {
		auto position = overrides.end();
		bool afterExport = false;
		for (std::filesystem::directory_iterator file(GetOverridesDirectory(), error), end;
			!error && file != end; file.increment(error)) {
			const auto name = file->path().filename().string();
			if (Util::IEquals(name, path.filename().string()))
				afterExport = true;
			else if (afterExport) {
				position = std::ranges::find_if(overrides, [&](const auto& info) {
					return Util::IEquals(std::filesystem::path(info.filePath).filename().string(), name);
				});
				if (position != overrides.end())
					break;
			}
		}
		overrides.insert(position, std::move(*refreshed));
		RebuildOverrideIndex();
	}
	discovered = true;
	ReapplyChangedSettings(settings, underlying, mask, pendingEdits);
	return true;
}
