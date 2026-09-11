#include "SceneManager.h"

#include "CSEditor/SceneSettingsUI.h"
#include "SceneManagerUI.h"
#include "SceneSettingsManager.h"
#include "SceneSettingsUIHooks.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Game.h"

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
	FEATURE_QUERY("environmentPreviewState",
		"Read shared toolbar and OS Editor environment controls. Args: none. Returns playing (toolbar weather/time lock), weather FormID or 0, timePaused, hour and timeScale. Stop restores controls that preceded Play, but does not rewind the game hour or save settings. Loading/sleep/wait/map menus temporarily run time to avoid blocking engine transitions.",
		([](const Feature*, const json&) -> json {
			auto* weather = Util::EnvironmentControls::GetLockedWeather();
			auto* calendar = globals::game::calendar;
			return { { "playing", Util::EnvironmentControls::IsPreviewActive() },
				{ "weather", weather ? weather->GetFormID() : 0 }, { "timePaused", Util::EnvironmentControls::IsTimePaused() },
				{ "hour", calendar && calendar->gameHour ? json(calendar->gameHour->value) : json(nullptr) },
				{ "timeScale", calendar && calendar->timeScale ? json(calendar->timeScale->value) : json(nullptr) } };
		}));
	FEATURE_COMMAND("setEnvironmentPreviewPlaying",
		"Press the retained feature toolbar's Play/Stop control. Args: playing=boolean, feature=shortName (required for Play). Play uses the toolbar's selected weather/period/location, locks weather and/or time, and can travel to a location. Time-only playback also holds the current weather. Requires a loaded player cell and an open toolbar for the named feature. Stop is always allowed. No scene settings are saved. Verify with environmentPreviewState; OS menu closure and visiting another page retain the lock, changing the selected weather/period while playing retargets the existing lock without releasing it. Replacing the toolbar's owning feature or using explicit OS Editor environment controls ends the preview. Time sliders hold the dominant weather only while active, releasing their temporary lock when the interaction ends.",
		([](Feature*, const json& args) {
			const bool playing = args.at("playing").get<bool>();
			if (playing && !SceneManagerUI::IsFeaturePageEditing(Feature::FindFeatureByShortName(args.at("feature").get<std::string>())))
				throw std::invalid_argument("Open the feature's Scene Manager toolbar first");
			if (!SceneSettingsUI::SetFeaturePagePreviewPlaying(playing))
				throw std::invalid_argument("The selected scene cannot be previewed, or the game is not ready");
		}));
	FEATURE_QUERY("featureScenePauseState",
		"Read count, paused count, and activeOverwrites across both ownership layers for one feature and scene set, plus previewEditing, previewPendingEdits, toolbarOpen, previewOverwritesPaused and previewHasOverwrites for that feature's retained toolbar draft. sceneReady is false during main/loading menus or without a player cell; toolbarActionsLocked reports the loading/overwrite lock for Copy to, Pause and Delete. toolbarOpen means the toolbar is enabled on its owning feature, even while viewing another page or with the OS menu closed. Args: feature=shortName, type=interior|timeOfDay|weather|location, period=Normal|Dawn|Sunrise|Day|Sunset|Dusk|Night (default Normal; named period required for timeOfDay). Weather/location require formKey=SPID; location also requires locationType=Worldspace|Region|LocationType|Location|Cell. Entry pause is independent of feature-wide pause.",
		([](const Feature*, const json& args) -> json {
			auto* manager = SceneSettingsManager::GetSingleton();
			const auto feature = args.at("feature").get<std::string>();
			const auto summary = manager->GetFeatureSceneSummary(feature, ParseFeatureSceneSet(args));
			return { { "count", summary.count }, { "paused", summary.paused }, { "activeOverwrites", summary.activeOverwrites },
				{ "sceneReady", manager->IsSceneReady() },
				{ "toolbarActionsLocked", !manager->IsSceneReady() || (manager->IsFeatureSceneEditing(feature) && manager->AreFeatureSceneEditActionsLocked()) },
				{ "previewEditing", manager->IsFeatureSceneEditing(feature) },
				{ "previewPendingEdits", manager->IsFeatureSceneEditing(feature) && manager->HasPendingFeatureSceneEdits() },
				{ "toolbarOpen", SceneManagerUI::IsFeaturePageEditing(Feature::FindFeatureByShortName(feature)) },
				{ "previewOverwritesPaused", manager->IsFeatureSceneEditing(feature) && manager->AreFeatureSceneEditOverwritesPaused() },
				{ "previewHasOverwrites", manager->IsFeatureSceneEditing(feature) && manager->HasFeatureSceneEditOverwrites() } };
		}));
	FEATURE_COMMAND("openFeatureSceneEditor",
		"Open or reopen a feature's Scene Manager toolbar without saving. Args: feature=shortName. Reopening the same feature retains its draft. Requesting another feature shows a discard confirmation on that feature page only if the existing draft has unsaved edits; otherwise it switches immediately. Navigate to the target feature page with the Open Shaders menu action to see the toolbar or confirmation. Verify with featureScenePauseState previewEditing, previewPendingEdits and toolbarOpen.",
		([](Feature*, const json& args) {
			if (!SceneSettingsManager::GetSingleton()->IsSceneReady())
				throw std::invalid_argument("Wait until a player cell is loaded before opening the scene editor");
			auto* feature = Feature::FindFeatureByShortName(args.at("feature").get<std::string>());
			if (!feature || !feature->loaded || !SceneManagerUI::CanEditFeaturePage(feature))
				throw std::invalid_argument("feature must be loaded and support scene settings");
			SceneManagerUI::BeginFeaturePageEditing(feature);
		}));
	FEATURE_COMMAND("setFeaturePreviewOverwritesPaused",
		"Temporarily bypass or resume overwrites in a retained feature toolbar preview. Args: feature=shortName, paused=boolean. Does not save drafts or modify saved pause flags. The bypass survives closing the toolbar or OS menu and visiting other pages, along with the unsaved preview. Replacing the draft with another feature or exiting the game ends the bypass; entries already individually paused remain paused. Verify with featureScenePauseState previewOverwritesPaused and previewHasOverwrites.",
		([](Feature*, const json& args) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (!manager->IsSceneReady())
				throw std::invalid_argument("Wait until a player cell is loaded before changing the preview");
			if (!manager->IsFeatureSceneEditing(args.at("feature").get<std::string>()))
				throw std::invalid_argument("Open the feature's Scene Manager toolbar first");
			manager->SetFeatureSceneEditOverwritesPaused(args.at("paused").get<bool>());
		}));
	FEATURE_COMMAND("setFeatureScenePaused",
		"Set paused=boolean for both ownership layers in one feature's saved scene set, matching the toolbar Pause/Resume button. Uses the same feature/type/period/formKey/locationType args as featureScenePauseState, plus required paused. Refuses loading, pending toolbar edits, or active preview overwrites until Pause Overwrites is used; never saves a draft, creates entries, changes scene mode, or clears feature-wide pause. User pause persists; overwrite pause is session-only. Verify with featureScenePauseState.",
		([](Feature*, const json& args) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (manager->HasPendingFeatureSceneEdits())
				throw std::invalid_argument("Save or discard pending toolbar edits before pausing or resuming");
			if (!manager->IsSceneReady() || (manager->IsFeatureSceneEditing(args.at("feature").get<std::string>()) && manager->AreFeatureSceneEditActionsLocked()))
				throw std::invalid_argument("Wait until a player cell is loaded and pause preview overwrites before pausing or resuming");
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
		"Read saved entries and the active mode. Uses featureScenePauseState's type/period/formKey/locationType args, without feature. Returns every period and ownership layer with index, feature, path, setting, source, period, value, originalValue, transitionSeconds and paused. Indices refer to this snapshot and must be refreshed after structural changes. timeOfDayEnabled applies to weather/location only.",
		([](const Feature*, const json& args) -> json {
			const auto context = ParseFeatureSceneSet(args);
			auto* manager = SceneSettingsManager::GetSingleton();
			const auto& entries = context.type == SceneContextType::Interior  ? manager->GetEntries(SceneType::InteriorOnly) :
		                          context.type == SceneContextType::TimeOfDay ? manager->GetEntries(SceneType::TimeOfDay) :
		                          context.type == SceneContextType::Weather   ? manager->GetWeatherConfig(context.weatherId).entries :
		                                                                        manager->GetLocationConfig(context.locationType, context.locationFormKey).entries;
			json result{ { "timeOfDayEnabled", manager->IsSceneTimeOfDayEnabled(context) }, { "entries", json::array() } };
			for (size_t index = 0; index < entries.size(); ++index) {
				const auto& entry = entries[index];
				result["entries"].push_back({ { "index", index }, { "feature", entry.featureShortName }, { "path", entry.settingPath }, { "setting", entry.settingKey },
					{ "period", entry.period == TimeOfDayPeriod::Count ? "Normal" : GetPeriodName(entry.period) },
					{ "source", entry.source == EntrySource::User ? "User" : "Overwrite" },
					{ "value", entry.value }, { "originalValue", entry.originalValue },
					{ "transitionSeconds", entry.transitionSeconds ? json(*entry.transitionSeconds) : json(nullptr) }, { "paused", entry.paused } });
			}
			return result;
		}));
	FEATURE_COMMAND("addSceneSetting",
		"Add one User setting through the Scene Manager add-setting path. Args: feature, setting, path (string array, default empty), plus type/period/formKey/locationType as in featureScenePauseState. Initializes from the same lower-layer/default value as Add Setting, persists, and refuses incompatible types or duplicates. Does not change weather/location mode. Read sceneSets, then use updateSceneEntries to change the value.",
		([](Feature*, const json& args) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (!manager->IsSceneReady() || manager->HasPendingFeatureSceneEdits())
				throw std::invalid_argument("Load a player cell and finish pending toolbar edits first");
			const auto context = ParseFeatureSceneSet(args);
			const auto feature = args.at("feature").get<std::string>();
			const auto path = args.value("path", std::vector<std::string>{});
			const auto setting = args.at("setting").get<std::string>();
			bool added = false;
			if (context.type == SceneContextType::Weather)
				added = manager->AddWeatherSetting(context.weatherId, feature, path, setting, context.period);
			else if (context.type == SceneContextType::Location)
				added = manager->AddLocationSetting(context.locationType, context.locationFormKey, {}, {}, feature, path, setting, false, context.period);
			else
				added = manager->AddSetting(context.type == SceneContextType::Interior ? SceneType::InteriorOnly : SceneType::TimeOfDay,
					feature, path, setting, GetFeatureSettingValue(feature, path, setting), context.period);
			if (!added)
				throw std::invalid_argument("Setting is incompatible, unavailable or already added");
		}));
	FEATURE_COMMAND("updateSceneEntries",
		"Update entry values through the table's validated batch path. Args: type/period/formKey/locationType as in sceneSets, updates=[{index,value}]. Refresh indices with sceneSets first. User values persist once; Overwrite changes are session-only. Reapplies without creating entries or changing mode. Verify actual values with sceneSets.",
		([](Feature*, const json& args) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (!manager->IsSceneReady() || manager->HasPendingFeatureSceneEdits())
				throw std::invalid_argument("Load a player cell and finish pending toolbar edits first");
			const auto context = ParseFeatureSceneSet(args);
			std::vector<EntryValueUpdate> updates;
			for (const auto& update : args.at("updates"))
				updates.push_back({ update.at("index").get<size_t>(), update.at("value") });
			if (context.type == SceneContextType::Weather)
				manager->UpdateWeatherEntryValues(context.weatherId, updates);
			else if (context.type == SceneContextType::Location)
				manager->UpdateLocationEntryValues(context.locationType, context.locationFormKey, updates);
			else
				manager->UpdateEntryValues(context.type == SceneContextType::Interior ? SceneType::InteriorOnly : SceneType::TimeOfDay, updates);
		}));
	FEATURE_COMMAND("resetSceneEntry",
		"Reset one entry using the table's Reset action. Args: type/period/formKey/locationType as in sceneSets, index. User changes persist; Overwrite changes are session-only. Refresh indices with sceneSets first; verify afterward.",
		([](Feature*, const json& args) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (!manager->IsSceneReady() || manager->HasPendingFeatureSceneEdits())
				throw std::invalid_argument("Load a player cell and finish pending toolbar edits first");
			const auto context = ParseFeatureSceneSet(args);
			const auto index = args.at("index").get<size_t>();
			if (context.type == SceneContextType::Weather)
				manager->RevertWeatherEntryToDefault(context.weatherId, index);
			else if (context.type == SceneContextType::Location)
				manager->RevertLocationEntryToDefault(context.locationType, context.locationFormKey, index);
			else
				manager->RevertEntryToDefault(context.type == SceneContextType::Interior ? SceneType::InteriorOnly : SceneType::TimeOfDay, index);
		}));
	FEATURE_COMMAND("removeSceneEntries",
		"Delete selected weather/location entries through the batched table deletion path. Args: type/formKey/locationType as in sceneSets, indices=[index,...]. Refresh indices first. User entries are persisted once; Overwrites are removed from their backing files, one write per file. Deletion is permanent. Verify with sceneSets.",
		([](Feature*, const json& args) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (!manager->IsSceneReady() || manager->HasPendingFeatureSceneEdits())
				throw std::invalid_argument("Load a player cell and finish pending toolbar edits first");
			manager->RemoveSceneSettings(ParseSceneTarget(args), args.at("indices").get<std::vector<size_t>>());
		}));
	FEATURE_COMMAND("reloadSceneSettings",
		"Reload all Scene Manager user and overwrite files from disk and reapply. Args: none. Refuses pending toolbar edits. Ends any clean preview and preserves session-only overwrite pauses. Does not save or alter files. Use after externally restoring a SceneSettings backup, then verify sceneSets and live feature values.",
		([](Feature*, const json&) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (!manager->IsSceneReady() || manager->HasPendingFeatureSceneEdits())
				throw std::invalid_argument("Load a player cell and finish pending toolbar edits first");
			if (!manager->ReloadSceneSettings())
				throw std::runtime_error("Scene settings could not be reloaded");
		}));
	FEATURE_QUERY("blendState",
		"Read live resolver inputs: sceneReady, hour, periodFactors keyed by period name, and currentWeather/previousWeather FormIDs plus weatherBlend. Args: none. Combine with currentLocations and live openshaders.feature get values to verify simultaneous transitions. Does not advance time or change weather.",
		([](const Feature*, const json&) -> json {
			auto* manager = SceneSettingsManager::GetSingleton();
			std::array<float, kPeriodCount> factors;
			manager->GetTimeOfDayFactors(factors.data());
			json result{ { "sceneReady", manager->IsSceneReady() }, { "hour", manager->GetCurrentGameHour() }, { "periodFactors", json::object() } };
			for (size_t index = 0; index < factors.size(); ++index)
				result["periodFactors"][GetPeriodName(static_cast<TimeOfDayPeriod>(index))] = factors[index];
			if (auto* sky = globals::game::sky) {
				result["currentWeather"] = sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
				result["previousWeather"] = sky->lastWeather ? sky->lastWeather->GetFormID() : 0;
				result["weatherBlend"] = sky->currentWeatherPct;
			}
			return result;
		}));
	FEATURE_COMMAND("setLocationTransition",
		"Set per-setting location transition seconds using the table's grouped control. Args: type=location, formKey, locationType, indices=[index,...], seconds=number or null for default. Only User float settings are accepted, including grouped components. Persists and reapplies; refresh indices and verify with sceneSets.",
		([](Feature*, const json& args) {
			auto* manager = SceneSettingsManager::GetSingleton();
			if (!manager->IsSceneReady() || manager->HasPendingFeatureSceneEdits())
				throw std::invalid_argument("Load a player cell and finish pending toolbar edits first");
			const auto context = ParseSceneTarget(args);
			if (context.type != SceneContextType::Location)
				throw std::invalid_argument("type must be location");
			const std::optional<float> seconds = args.at("seconds").is_null() ? std::nullopt : std::optional(args.at("seconds").get<float>());
			manager->SetLocationEntryTransitionSeconds(context.locationType, context.locationFormKey, args.at("indices").get<std::vector<size_t>>(), seconds);
		}));
	FEATURE_COMMAND("setSceneMode",
		"Persist the active saved set without copying, converting or deleting entries. Args: type=weather|location, formKey=SPID, timeOfDayEnabled=boolean; locations also require locationType=Worldspace|Region|LocationType|Location|Cell. Verify with sceneSets.",
		([](Feature*, const json& args) {
			SceneSettingsManager::GetSingleton()->SetSceneTimeOfDayEnabled(ParseSceneTarget(args), args.at("timeOfDayEnabled").get<bool>());
		}));
}
