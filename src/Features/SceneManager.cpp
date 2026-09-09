#include "SceneManager.h"

#include "SceneManagerUI.h"
#include "SceneSettingsManager.h"
#include "SceneSettingsUIHooks.h"
#include "Utils/DevBenchUx.h"

std::pair<std::string, std::vector<std::string>> SceneManager::GetFeatureSummary()
{
	return {
		T("feature.scene_manager.description",
			"Applies selected Open Shaders settings by interior, time of day, weather, and location."),
		{
			T("feature.scene_manager.key_feature_1", "Blends exterior settings across time of day and weather transitions"),
			T("feature.scene_manager.key_feature_2", "Applies interior settings separately from exterior settings"),
			T("feature.scene_manager.key_feature_3", "Supports region, location, and cell overrides with per-setting precedence"),
		},
	};
}

void SceneManager::DrawSettings()
{
	SceneManagerUI::Draw();
}

void SceneManager::SetupResources()
{
	LoadAll();
}

void SceneManager::PostPostLoad()
{
	SceneSettingsUIHooks::Install();
}

void SceneManager::DataLoaded()
{
	SceneSettingsManager::OnDataLoaded();
	MenuOpenCloseEventHandler::Register();
}

void SceneManager::Update()
{
	SceneSettingsManager::Update();
}

namespace
{
	SceneSettingsManager::SceneContextId ParseSceneTarget(const json& args)
	{
		using Manager = SceneSettingsManager;
		const auto type = args.at("type").get<std::string>();
		const auto formKey = args.at("formKey").get<std::string>();
		if (type == "weather") {
			const auto id = Util::SpidToFormId(formKey);
			if (!RE::TESForm::LookupByID<RE::TESWeather>(id))
				throw std::invalid_argument("formKey must resolve to a weather");
			return { .type = Manager::SceneContextType::Weather, .weatherId = id };
		}
		if (type != "location")
			throw std::invalid_argument("type must be weather or location");
		const auto locationType = magic_enum::enum_cast<Manager::LocationTargetType>(args.at("locationType").get<std::string>());
		for (const auto& target : Manager::GetSingleton()->GetLocationManagementTargets())
			if (Util::SpidToFormId(target.formKey) == Util::SpidToFormId(formKey) &&
				locationType == target.type)
				return { .type = Manager::SceneContextType::Location, .locationType = target.type, .locationFormKey = target.formKey };
		throw std::invalid_argument("formKey and locationType must match an available location target");
	}

	SceneSettingsManager::SceneContextId ParseFeatureSceneSet(const json& args)
	{
		using Manager = SceneSettingsManager;
		const auto type = args.at("type").get<std::string>();
		auto context = type == "interior"  ? Manager::SceneContextId{ .type = Manager::SceneContextType::Interior } :
		               type == "timeOfDay" ? Manager::SceneContextId{ .type = Manager::SceneContextType::TimeOfDay } :
		                                     ParseSceneTarget(args);
		const auto period = args.value("period", std::string("Normal"));
		context.period = Manager::GetPeriodFromName(period);
		if ((period != "Normal" && context.period == Manager::TimeOfDayPeriod::Count) ||
			(type == "timeOfDay" && period == "Normal") || (type == "interior" && period != "Normal"))
			throw std::invalid_argument("period must match the scene type: Normal or Dawn|Sunrise|Day|Sunset|Dusk|Night");
		return context;
	}
}

void SceneManager::RegisterUxActions()
{
	FEATURE_QUERY("featureScenePauseState",
		"Read count, paused count, and activeOverwrites across both ownership layers for one feature and scene set, plus previewEditing, previewPendingEdits, toolbarOpen, previewOverwritesPaused and previewHasOverwrites for that feature's retained toolbar draft. toolbarOpen means the toolbar is enabled on its owning feature, even while viewing another page or with the OS menu closed. Args: feature=shortName, type=interior|timeOfDay|weather|location, period=Normal|Dawn|Sunrise|Day|Sunset|Dusk|Night (default Normal; named period required for timeOfDay). Weather/location require formKey=SPID; location also requires locationType=Worldspace|Region|LocationType|Location|Cell. Entry pause is independent of feature-wide pause.",
		([](const Feature*, const json& args) -> json {
			auto* manager = SceneSettingsManager::GetSingleton();
			const auto feature = args.at("feature").get<std::string>();
			const auto summary = manager->GetFeatureSceneSummary(feature, ParseFeatureSceneSet(args));
			return { { "count", summary.count }, { "paused", summary.paused }, { "activeOverwrites", summary.activeOverwrites },
				{ "previewEditing", manager->IsFeatureSceneEditing(feature) },
				{ "previewPendingEdits", manager->IsFeatureSceneEditing(feature) && manager->HasPendingFeatureSceneEdits() },
				{ "toolbarOpen", SceneManagerUI::IsFeaturePageEditing(Feature::FindFeatureByShortName(feature)) },
				{ "previewOverwritesPaused", manager->IsFeatureSceneEditing(feature) && manager->AreFeatureSceneEditOverwritesPaused() },
				{ "previewHasOverwrites", manager->IsFeatureSceneEditing(feature) && manager->HasFeatureSceneEditOverwrites() } };
		}));
	FEATURE_COMMAND("openFeatureSceneEditor",
		"Open or reopen a feature's Scene Manager toolbar without saving. Args: feature=shortName. Reopening the same feature retains its draft. Requesting another feature shows a discard confirmation on that feature page only if the existing draft has unsaved edits; otherwise it switches immediately. Navigate to the target feature page with the Open Shaders menu action to see the toolbar or confirmation. Verify with featureScenePauseState previewEditing, previewPendingEdits and toolbarOpen.",
		([](Feature*, const json& args) {
			auto* feature = Feature::FindFeatureByShortName(args.at("feature").get<std::string>());
			if (!feature || !feature->loaded || !SceneManagerUI::CanEditFeaturePage(feature))
				throw std::invalid_argument("feature must be loaded and support scene settings");
			SceneManagerUI::BeginFeaturePageEditing(feature);
		}));
	FEATURE_COMMAND("setFeaturePreviewOverwritesPaused",
		"Temporarily bypass or resume overwrites in a retained feature toolbar preview. Args: feature=shortName, paused=boolean. Does not save drafts or modify saved pause flags. The bypass survives closing the toolbar or OS menu and visiting other pages, along with the unsaved preview. Replacing the draft with another feature or exiting the game ends the bypass; entries already individually paused remain paused. Verify with featureScenePauseState previewOverwritesPaused and previewHasOverwrites.",
		([](Feature*, const json& args) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (!manager->IsFeatureSceneEditing(args.at("feature").get<std::string>()))
				throw std::invalid_argument("Open the feature's Scene Manager toolbar first");
			manager->SetFeatureSceneEditOverwritesPaused(args.at("paused").get<bool>());
		}));
	FEATURE_COMMAND("setFeatureScenePaused",
		"Set paused=boolean for both ownership layers in one feature's saved scene set, matching the toolbar Pause/Resume button. Uses the same feature/type/period/formKey/locationType args as featureScenePauseState, plus required paused. Refuses pending toolbar edits; never saves a draft, creates entries, changes scene mode, or clears feature-wide pause. User pause persists; overwrite pause is session-only. Verify with featureScenePauseState.",
		([](Feature*, const json& args) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (manager->HasPendingFeatureSceneEdits())
				throw std::invalid_argument("Save or discard pending toolbar edits before pausing or resuming");
			manager->SetFeatureSceneSettingsPaused(args.at("feature").get<std::string>(), ParseFeatureSceneSet(args), args.at("paused").get<bool>());
		}));
	FEATURE_QUERY("currentLocations",
		"Read current location targets from general to specific: worldspace, direct location types, region, direct location, cell. Args: none. Returns type, formKey and name for each target. Parent locations and their inherited types are excluded; worldspace membership uses the exterior cell, not location boundaries.",
		([](const Feature*, const json&) -> json {
			json targets = json::array();
			for (const auto& target : SceneSettingsManager::GetSingleton()->GetCurrentLocationTargets())
				targets.push_back({ { "type", magic_enum::enum_name(target.type) },
					{ "formKey", target.formKey }, { "name", target.name } });
			return targets;
		}));
	FEATURE_QUERY("sceneSets",
		"Read both independent saved sets and the active mode. Args: type=weather|location, formKey=SPID; locations also require locationType=Worldspace|Region|LocationType|Location|Cell. Returns entries with source, period (Normal or named time period), value and pause state.",
		([](const Feature*, const json& args) -> json {
			const auto context = ParseSceneTarget(args);
			auto* manager = SceneSettingsManager::GetSingleton();
			const auto& entries = context.type == SceneContextType::Weather ? manager->GetWeatherConfig(context.weatherId).entries :
		                                                                      manager->GetLocationConfig(context.locationType, context.locationFormKey).entries;
			json result{ { "timeOfDayEnabled", manager->IsSceneTimeOfDayEnabled(context) }, { "entries", json::array() } };
			for (const auto& entry : entries)
				result["entries"].push_back({ { "feature", entry.featureShortName }, { "path", entry.settingPath }, { "setting", entry.settingKey },
					{ "period", entry.period == TimeOfDayPeriod::Count ? "Normal" : GetPeriodName(entry.period) },
					{ "source", entry.source == EntrySource::User ? "User" : "Overwrite" },
					{ "value", entry.value }, { "paused", entry.paused } });
			return result;
		}));
	FEATURE_COMMAND("setSceneMode",
		"Persist the active saved set without copying, converting or deleting entries. Args: type=weather|location, formKey=SPID, timeOfDayEnabled=boolean; locations also require locationType=Worldspace|Region|LocationType|Location|Cell. Verify with sceneSets.",
		([](Feature*, const json& args) {
			SceneSettingsManager::GetSingleton()->SetSceneTimeOfDayEnabled(ParseSceneTarget(args), args.at("timeOfDayEnabled").get<bool>());
		}));
}
