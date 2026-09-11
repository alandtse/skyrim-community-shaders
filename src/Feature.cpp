#include "Feature.h"

#include "FeatureIssues.h"
#include "FeatureVersions.h"
#include "Features/CSEditor.h"
#include "Features/CSUtility.h"
#include "Features/CloudRelight.h"
#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#if defined(ENABLE_EFFECTS11)
#	include "Features/Effects11.h"
#endif
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/FeatureOverwrites.h"
#include "Features/FoliageLighting.h"
#include "Features/GrassCollision.h"
#include "Features/GrassLighting.h"
#include "Features/GrassOptimizations.h"
#include "Features/HDRDisplay.h"
#include "Features/HairSpecular.h"
#include "Features/HorizonFix.h"
#include "Features/IBL.h"
#include "Features/InteriorSun.h"
#include "Features/InverseSquareLighting.h"
#include "Features/LODBlending.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/PerformanceOverlay.h"
#include "Features/PostProcessing.h"
#include "Features/RemoteControl.h"
#include "Features/RenderDoc.h"
#include "Features/SceneManager.h"
#include "Features/SceneSelector.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/ScreenshotFeature.h"
#include "Features/Skin.h"
#include "Features/SkySync.h"
#include "Features/Skylighting.h"
#include "Features/SubsurfaceScattering.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainHelper.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainVariation.h"
#include "Features/UnifiedWater.h"
#include "Features/Upscaling.h"
#include "Features/VR.h"
#include "Features/VanillaFresnel.h"
#include "Features/VolumetricLighting.h"
#include "Features/VolumetricShadows.h"
#include "Features/WaterEffects.h"
#include "Features/WetnessEffects.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/Format.h"

void Feature::Load(json& o_json)
{
	// AIO ships every feature's ini to every runtime; loaded must stay false
	// here on VR unless dev mode's own test-all-features bypass is active.
	if (globals::game::isVR && !SupportsVR() && !globals::state->IsDeveloperMode()) {
		loaded = false;
		logger::info("{} does not support VR, feature disabled", GetShortName());
		return;
	}

	// Convert string to wstring
	auto ini_filename = std::format("{}.ini", GetShortName());
	std::wstring ini_filename_w;
	std::ranges::copy(ini_filename, std::back_inserter(ini_filename_w));
	auto ini_path = L"Data\\Shaders\\Features\\" + ini_filename_w;

	CSimpleIniA ini;
	ini.SetUnicode();
	SI_Error rc = ini.LoadFile(ini_path.c_str());

	if (rc < 0) {
		if (!FeatureIssues::IsObsoleteFeature(GetShortName()))
			logger::info("{} failed to load, feature disabled", ini_filename);
		loaded = false;
		return;
	}

	bool hasError = false;
	std::string errorVersion;
	FeatureIssues::FeatureIssueInfo::IssueType errorType = FeatureIssues::FeatureIssueInfo::IssueType::UNKNOWN;

	if (FeatureIssues::IsObsoleteFeature(GetShortName())) {
		hasError = true;
		errorVersion = "N/A";
		errorType = FeatureIssues::FeatureIssueInfo::IssueType::OBSOLETE;
		failedLoadedMessage = std::format("{} is an obsolete feature that has been removed", GetDisplayName());
	} else if (auto value = ini.GetValue("Info", "Version")) {
		try {
			REL::Version featureVersion(std::regex_replace(value, std::regex("-"), "."));

			// Check if feature exists in minimal versions
			REL::Version minimalFeatureVersion;
			if (!Feature::IsFeatureKnown(GetShortName(), &minimalFeatureVersion)) {
				hasError = true;
				errorVersion = value;
				errorType = FeatureIssues::FeatureIssueInfo::IssueType::UNKNOWN;
				failedLoadedMessage = std::format("{} {} is an unknown feature not supported by this Open Shaders version. This may be a feature from a development branch.", GetDisplayName(), value);
			} else {
				// Version compatibility check
				bool oldFeature = featureVersion.compare(minimalFeatureVersion) == std::strong_ordering::less;
				bool majorVersionMismatch = featureVersion.major() < minimalFeatureVersion.major();

				if (!oldFeature && !majorVersionMismatch) {
					loaded = true;
					logger::info("{} {} successfully loaded", ini_filename, value);
				} else {
					hasError = true;
					errorVersion = value;
					errorType = FeatureIssues::FeatureIssueInfo::IssueType::VERSION_MISMATCH;

					std::string minimalVersionString = Util::GetFormattedVersion(minimalFeatureVersion);

					if (IsCore()) {
						failedLoadedMessage = std::format("This feature is already included as part of the core Open Shaders installation. Uninstall this feature with your mod manager.");
					} else if (majorVersionMismatch) {
						failedLoadedMessage = std::format("{} {} is too old, major version incompatibility detected. Required: {}", GetDisplayName(), value, minimalVersionString);
					} else {
						failedLoadedMessage = std::format("{} {} is an old feature version, required: {}", GetDisplayName(), value, minimalVersionString);
					}
				}
			}

			version = value;
		} catch (const std::exception& e) {
			hasError = true;
			errorVersion = value;
			errorType = FeatureIssues::FeatureIssueInfo::IssueType::VERSION_MISMATCH;
			failedLoadedMessage = std::format("{} {} has invalid version format: {}", GetDisplayName(), value, e.what());
		}
	} else {
		hasError = true;
		errorVersion = "unknown";
		errorType = FeatureIssues::FeatureIssueInfo::IssueType::VERSION_MISMATCH;

		// Get the minimum required version to include in the error message
		std::string requiredVersion = Feature::GetFeatureRequiredVersion(GetShortName());

		failedLoadedMessage = std::format("The feature file for {} is missing. This feature is not installed! Version required: {}", GetDisplayName(), requiredVersion);
	}

	if (hasError) {
		loaded = false;
		logger::warn("{}", failedLoadedMessage);

		// Guard against empty shortName to prevent bogus filesystem access
		std::string shortName = GetShortName();
		if (!shortName.empty()) {
			FeatureIssues::FeatureFileInfo fileInfo = FeatureIssues::GetFeatureFileInfo(shortName);

			// For version mismatch, also pass the minimum required version
			std::string minimumVersion;
			if (errorType == FeatureIssues::FeatureIssueInfo::IssueType::VERSION_MISMATCH) {
				minimumVersion = Feature::GetFeatureRequiredVersion(shortName);
			}

			FeatureIssues::AddFeatureIssue(shortName, errorVersion, failedLoadedMessage, errorType, fileInfo, minimumVersion);

		} else {
			logger::error("Feature has empty short name, cannot add to feature issues list");
		}
	} else {
		if (!UsesMainSettings())
			return;

		// No errors, load settings now
		if (o_json[GetName()].is_structured()) {
			logger::info("Loading {} settings", GetName());
			try {
				LoadSettings(o_json[GetName()]);
			} catch (...) {
				logger::warn("Invalid settings for {}, using default.", GetName());
				RestoreDefaultSettings();
			}
		} else {
			logger::info("Loading default settings for {}", GetName());
			RestoreDefaultSettings();
		}
	}
}

void Feature::Save(json& o_json)
{
	if (UsesMainSettings())
		SaveSettings(o_json[GetName()]);
}

bool Feature::ValidateCache(CSimpleIniA& a_ini)
{
	auto name = GetName();
	auto ini_name = GetShortName();

	logger::info("Validating {}", name);

	auto enabledInCache = a_ini.GetBoolValue(ini_name.c_str(), "Enabled", false);
	if (enabledInCache && !loaded) {
		logger::info("Feature was uninstalled");
		return false;
	}
	if (!enabledInCache && loaded) {
		logger::info("Feature was installed");
		return false;
	}

	if (loaded) {
		auto versionInCache = a_ini.GetValue(ini_name.c_str(), "Version");
		if (strcmp(versionInCache, version.c_str()) != 0) {
			logger::info("Change in version detected. Installed {} but {} in Disk Cache", version, versionInCache);
			return false;
		} else {
			logger::info("Installed version and cached version match.");
		}
	}

	logger::info("Cached feature is valid");
	return true;
}

void Feature::WriteDiskCacheInfo(CSimpleIniA& a_ini)
{
	auto ini_name = GetShortName();
	a_ini.SetBoolValue(ini_name.c_str(), "Enabled", loaded);
	a_ini.SetValue(ini_name.c_str(), "Version", version.c_str());
}

namespace
{
	/** @brief Every Feature instance Open Shaders knows about, independent of VR filtering. */
	const std::vector<Feature*>& GetAllFeatures()
	{
		static std::vector<Feature*> features = {
			&globals::features::truePBR,
			&globals::features::foliageLighting,
			&globals::features::volumetricShadows,
			&globals::features::grassLighting,
			&globals::features::grassCollision,
			&globals::features::grassOptimizations,
			&globals::features::screenSpaceShadows,
			&globals::features::extendedMaterials,
			&globals::features::wetnessEffects,
			&globals::features::lightLimitFix,
			&globals::features::dynamicCubemaps,
			&globals::features::cloudShadows,
			&globals::features::cloudRelight,
			&globals::features::waterEffects,
			&globals::features::performanceOverlay,
			&globals::features::subsurfaceScattering,
			&globals::features::terrainShadows,
			&globals::features::screenSpaceGI,
			&globals::features::skylighting,
			&globals::features::skySync,
			&globals::features::terrainBlending,
			&globals::features::terrainHelper,
			&globals::features::vanillaFresnel,
			&globals::features::volumetricLighting,
			&globals::features::lodBlending,
			&globals::features::inverseSquareLighting,
			&globals::features::hairSpecular,
			&globals::features::interiorSun,
			&globals::features::terrainVariation,
			&globals::features::ibl,
			&globals::features::extendedTranslucency,
			&globals::features::upscaling,
			&globals::features::renderDoc,
			&globals::features::remoteControl,
			&globals::features::csEditor,
			&globals::features::sceneSelector,
			&globals::features::csUtility,
			&globals::features::featureOverwrites,
			&globals::features::sceneManager,
			&globals::features::screenshotFeature,
			&globals::features::linearLighting,
#if defined(ENABLE_EFFECTS11)
			&globals::features::effects11,
#endif
			&globals::features::unifiedWater,
			&globals::features::horizonFix,
			&globals::features::exponentialHeightFog,
			&globals::features::hdrDisplay,
			&globals::features::skin,
			&globals::features::postProcessing
		};
		return features;
	}
}

/**
 * @brief Provides access to the registry of all known features.
 * @return A constant reference to the vector of all known feature instances.
 */
const std::vector<Feature*>& Feature::GetFeatureList()
{
	if (globals::game::isVR) {
		// Helper function to build VR feature list
		static auto BuildVRList = []() -> std::vector<Feature*> {
			auto v = GetAllFeatures();
			v.push_back(&globals::features::vr);

			// In developer mode, keep all features for testing
			// In production mode, filter to VR-compatible only
			if (!globals::state->IsDeveloperMode()) {
				std::erase_if(v, [](Feature* a) { return !a->SupportsVR(); });
			}
			return v;
		};

		// Cache the VR feature list but invalidate when developer mode changes
		static std::vector<Feature*> featuresVR;
		static bool cachedDevMode = false;

		bool currentDevMode = globals::state->IsDeveloperMode();
		if (featuresVR.empty() || currentDevMode != cachedDevMode) {
			featuresVR = BuildVRList();
			cachedDevMode = currentDevMode;
		}

		return featuresVR;
	} else {
		return GetAllFeatures();
	}
}

Feature* Feature::FindRegisteredFeatureByShortName(const std::string& shortName)
{
	for (auto* feature : GetAllFeatures()) {
		if (feature->GetShortName() == shortName)
			return feature;
	}
	// The VR feature is added to GetFeatureList() dynamically rather than living
	// in the base registry, since it only exists at all when running on VR.
	if (shortName == "VR")
		return &globals::features::vr;
	return nullptr;
}

void Feature::ApplyPerformanceProfileToAll(PerfProfile profile)
{
	for (auto* feature : GetFeatureList()) {
		if (!feature->loaded)
			continue;
		if (feature->PerformanceSectionRequiresVR() && !globals::game::isVR)
			continue;
		// One feature's failure must not abort the broadcast to the rest.
		try {
			feature->ApplyPerformanceProfile(profile);
		} catch (const std::exception& e) {
			logger::error("ApplyPerformanceProfileToAll: {} threw: {}", feature->GetShortName(), e.what());
		} catch (...) {
			logger::error("ApplyPerformanceProfileToAll: {} threw (unknown)", feature->GetShortName());
		}
	}
}

namespace
{
	// LoadingMenu fires on the main thread; latch the transition here and let the render thread run
	// the resets, so feature caches the menu/Prepass iterates aren't cleared mid-iteration.
	std::atomic<bool> g_loadingMenuOpenPending{ false };
	std::atomic<bool> g_loadingMenuClosePending{ false };

	class SceneTransitionSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
				if (a_event->opening)
					g_loadingMenuOpenPending.store(true, std::memory_order_release);
				else
					g_loadingMenuClosePending.store(true, std::memory_order_release);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
		static SceneTransitionSink* GetSingleton()
		{
			static SceneTransitionSink singleton;
			return &singleton;
		}
	};
}

void Feature::DrainSceneTransitions()
{
	static std::atomic<bool> registered{ false };
	if (!registered.load(std::memory_order_acquire)) {
		if (auto* ui = globals::game::ui) {
			// GetEventSource can be null before the UI is fully up; leave registered=false and retry
			// next frame rather than dereferencing it.
			if (auto* source = ui->GetEventSource<RE::MenuOpenCloseEvent>()) {
				source->AddEventSink(SceneTransitionSink::GetSingleton());
				registered.store(true, std::memory_order_release);
			}
		}
	}

	if (g_loadingMenuOpenPending.exchange(false, std::memory_order_acq_rel))
		ForEachLoadedFeature("OnSceneTransitionReset(open)", [](Feature* f) { f->OnSceneTransitionReset(true); });
	if (g_loadingMenuClosePending.exchange(false, std::memory_order_acq_rel))
		ForEachLoadedFeature("OnSceneTransitionReset(close)", [](Feature* f) { f->OnSceneTransitionReset(false); });
}

Feature* Feature::FindFeatureByShortName(const std::string& shortName)
{
	for (auto* feature : GetFeatureList()) {
		if (feature->loaded && feature->GetShortName() == shortName)
			return feature;
	}
	return nullptr;
}

std::vector<std::string> Feature::GetLoadedFeatureNames()
{
	std::vector<std::string> names;
	for (auto* feature : GetFeatureList()) {
		if (feature->loaded && feature->IsInMenu())
			names.push_back(feature->GetShortName());
	}
	std::sort(names.begin(), names.end());
	return names;
}

bool Feature::ToggleAtBootSetting()
{
	if (IsAlwaysEnabled())
		return false;
	auto state = globals::state;
	const std::string featureName = GetShortName();
	auto disabled = state->IsFeatureDisabled(featureName);
	state->SetFeatureBootEnabled(featureName, disabled);

	return state->IsFeatureDisabled(featureName);  // Return the new state
}

bool Feature::ReapplyOverrideSettings()
{
	auto overrideManager = SettingsOverrideManager::GetSingleton();
	std::string featureName = GetShortName();

	if (!overrideManager || !overrideManager->HasFeatureOverrides(featureName)) {
		return false;
	}

	// Delete user override file to restore original override behavior
	overrideManager->DeleteUserOverride(featureName);

	// Get base settings and apply overrides fresh
	json featureJson;
	SaveSettings(featureJson);

	// Apply overrides to the settings (without user customizations)
	size_t appliedCount = overrideManager->ReapplyFeatureOverrides(featureName, featureJson);

	if (appliedCount > 0) {
		// Load the override settings back into the feature
		LoadSettings(featureJson);
		return true;
	}

	return false;
}

bool Feature::ReapplyOverrideSettingsForKeys(std::span<const std::string_view> a_settingKeys)
{
	auto* overrideManager = SettingsOverrideManager::GetSingleton();
	const std::string featureName = GetShortName();
	if (!overrideManager || !overrideManager->HasFeatureOverrides(featureName))
		return false;

	const auto featureOverrides = overrideManager->GetFeatureOverrides(featureName);
	const auto isOverridden = [&](std::string_view a_key) {
		const std::string key{ a_key };
		for (const auto* featureOverride : featureOverrides) {
			if (featureOverride->enabled && featureOverride->overrideData.contains(key))
				return true;
		}
		return false;
	};

	json originalSettings;
	SaveSettings(originalSettings);
	json currentSettings = originalSettings;
	json mergedSettings = originalSettings;
	if (overrideManager->ReapplyFeatureOverrides(featureName, mergedSettings) == 0)
		return false;

	bool applied = false;
	for (const auto keyView : a_settingKeys) {
		const std::string key{ keyView };
		if (!isOverridden(keyView) || !mergedSettings.contains(key))
			continue;
		currentSettings[key] = mergedSettings[key];
		applied = true;
	}
	if (!applied)
		return false;

	json overrideSettings;
	bool persistenceAttempted = false;
	try {
		LoadSettings(currentSettings);
		json appliedSettings;
		SaveSettings(appliedSettings);
		overrideSettings = overrideManager->GetMergedOverrideSettings(featureName, json::object());
		persistenceAttempted = true;
		if (overrideManager->PersistUserOverride(featureName, appliedSettings, overrideSettings))
			return true;
		logger::warn("Failed to persist scoped override settings for {}", featureName);
	} catch (const std::exception& e) {
		logger::warn("Failed to apply scoped override settings for {}. Error: {}", featureName, e.what());
	}

	try {
		LoadSettings(originalSettings);
	} catch (const std::exception& e) {
		logger::error("Failed to roll back scoped override settings for {}. Error: {}", featureName, e.what());
	}
	if (persistenceAttempted) {
		try {
			if (!overrideManager->PersistUserOverride(featureName, originalSettings, overrideSettings))
				logger::error("Failed to roll back persisted override settings for {}", featureName);
		} catch (const std::exception& e) {
			logger::error("Failed to roll back persisted override settings for {}. Error: {}", featureName, e.what());
		}
	}
	return false;
}

std::string Feature::GetDisplayCategory() const
{
	const auto category = GetCategory();
	if (category == FeatureCategories::kCharacters)
		return T("feature.category.characters", "Characters");
	if (category == FeatureCategories::kDisplay)
		return T("feature.category.display", "Display");
	if (category == FeatureCategories::kFoliage)
		return T("feature.category.grass", "Foliage");
	if (category == FeatureCategories::kLandscapeAndTextures)
		return T("feature.category.landscape_and_textures", "Landscape & Textures");
	if (category == FeatureCategories::kLighting)
		return T("feature.category.lighting", "Lighting");
	if (category == FeatureCategories::kMaterials)
		return T("feature.category.materials", "Materials");
	if (category == FeatureCategories::kOther)
		return T("feature.category.other", "Other");
	if (category == FeatureCategories::kPostProcessing)
		return T("feature.category.post_processing", "Post-Processing");
	if (category == FeatureCategories::kSky)
		return T("feature.category.sky", "Sky");
	if (category == FeatureCategories::kUtility)
		return T("feature.category.utility", "Utilities");
	if (category == FeatureCategories::kWater)
		return T("feature.category.water", "Water");

	return std::string(category);
}

std::string Feature::GetReleaseStageTag(ReleaseStage stage)
{
	switch (stage) {
	case ReleaseStage::Alpha:
		return T("menu.features.tag_alpha", "[ALPHA]");
	case ReleaseStage::Beta:
		return T("menu.features.tag_beta", "[BETA]");
	default:
		return {};
	}
}

void Feature::DrawUnloadedUI()
{
	// Prioritize detailed failure message if available
	if (!failedLoadedMessage.empty()) {
		// Use error color for all failure messages
		auto& themeSettings = Menu::GetSingleton()->GetTheme();
		ImGui::TextColored(themeSettings.StatusPalette.Error, failedLoadedMessage.c_str());
		return;
	}

	// Fallback: Always show missing file message when no specific failure message exists
	auto& themeSettings = Menu::GetSingleton()->GetTheme();
	// Get the minimum required version to include in the error message
	std::string requiredVersion = Feature::GetFeatureRequiredVersion(GetShortName());

	auto missingFileMessage = std::format("The feature file for {} is missing. This feature is not installed! Version required: {}", GetDisplayName(), requiredVersion);
	ImGui::TextColored(themeSettings.StatusPalette.Error, missingFileMessage.c_str());

	// Also show feature summary if available
	auto [description, keyFeatures] = GetFeatureSummary();
	if (!description.empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", description.c_str());
	}

	if (!keyFeatures.empty()) {
		if (description.empty()) {
			ImGui::Spacing();
		}
		ImGui::TextWrapped("%s", T("feature.key_features", "Key features:"));
		for (const auto& feature : keyFeatures) {
			ImGui::BulletText("%s", feature.c_str());
		}
	}
}

std::string Feature::GetFeatureRequiredVersion(const std::string& shortName)
{
	if (shortName.empty()) {
		return "unknown";
	}
	auto iter = FeatureVersions::FEATURE_MINIMAL_VERSIONS.find(shortName);
	if (iter != FeatureVersions::FEATURE_MINIMAL_VERSIONS.end()) {
		return Util::GetFormattedVersion(iter->second);
	}

	return "unknown";
}

bool Feature::IsFeatureKnown(const std::string& shortName, REL::Version* outVersion)
{
	if (shortName.empty()) {
		return false;
	}

	auto iter = FeatureVersions::FEATURE_MINIMAL_VERSIONS.find(shortName);
	if (iter != FeatureVersions::FEATURE_MINIMAL_VERSIONS.end()) {
		if (outVersion) {
			*outVersion = iter->second;
		}
		return true;
	}

	return false;
}
