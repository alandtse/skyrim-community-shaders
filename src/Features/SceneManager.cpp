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
}

void SceneManager::RegisterUxActions()
{
	FEATURE_QUERY("currentLocations",
		"Read current location targets from general to specific: worldspace, location types, region, parent locations, cell. Args: none. Returns type, formKey and name for each target; worldspace membership uses the exterior cell, not location boundaries.",
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
