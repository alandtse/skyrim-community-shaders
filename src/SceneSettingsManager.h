
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using json = nlohmann::json;

#include "Feature.h"
#include "Globals.h"
#include "Utils/Form.h"

/// Manages interior, time-of-day, weather, and location-specific setting overrides.
/// Applies catalog-backed settings in priority order and avoids redundant feature updates.
class SceneSettingsManager
{
public:
	static SceneSettingsManager* GetSingleton();

	// --- Scene Types ---

	enum class SceneType
	{
		InteriorOnly,
		TimeOfDay,
		Location
	};

	/// Kind of scene context used by copy and bulk operations.
	enum class SceneContextType : std::uint8_t
	{
		Interior,
		TimeOfDay,
		Weather,
		Location,
	};

	// --- Time of Day Periods ---

	enum class TimeOfDayPeriod
	{
		Dawn = 0,
		Sunrise,
		Day,
		Sunset,
		Dusk,
		Night,
		Count
	};

	/// Number of time-of-day periods (avoids repeated static_cast).
	static constexpr int kPeriodCount = static_cast<int>(TimeOfDayPeriod::Count);

	/// Display names for each period - must match TimeOfDayPeriod order.
	static constexpr std::array<const char*, kPeriodCount> kPeriodNames = {
		"Dawn", "Sunrise", "Day", "Sunset", "Dusk", "Night"
	};

	/// Hour boundaries for each period [start, end).  Night wraps around midnight (21-28 i.e. 21-4).
	static constexpr float kPeriodHours[kPeriodCount][2] = {
		{ 4.0f, 6.0f },    // Dawn
		{ 6.0f, 8.0f },    // Sunrise
		{ 8.0f, 17.0f },   // Day
		{ 17.0f, 19.0f },  // Sunset
		{ 19.0f, 21.0f },  // Dusk
		{ 21.0f, 28.0f }   // Night (wraps past midnight)
	};

	/// Transition blend zone in hours at each period boundary.
	static constexpr float kTransitionHours = 0.5f;

	// --- Event Handler ---

	/// Listens for LoadingMenu close so loaded locations apply without an in-world fade.
	/// Defers reset work until the menu closes.
	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		static bool Register()
		{
			static bool registered = false;
			if (registered)
				return true;

			static MenuOpenCloseEventHandler singleton;
			auto ui = globals::game::ui;
			if (!ui) {
				logger::error("[SceneSettings] UI event source not found");
				return false;
			}
			auto eventSource = ui->GetEventSource<RE::MenuOpenCloseEvent>();
			if (!eventSource) {
				logger::error("[SceneSettings] MenuOpenCloseEvent source not found");
				return false;
			}
			eventSource->AddEventSink(&singleton);
			registered = true;
			logger::info("[SceneSettings] Registered MenuOpenCloseEventHandler");
			return true;
		}
	};

	// --- Setting Entry ---

	enum class EntrySource
	{
		User,      // User-added via UI
		Overwrite  // Loaded from overwrite file
	};

	struct SettingEntry
	{
		std::string featureShortName;              // Feature's GetShortName()
		std::vector<std::string> settingPath;      // Feature-owned subfeature/object path
		std::string settingKey;                    // Feature-owned scene setting key
		std::string displayName;                   // Cached UI label
		json value;                                // Override value (bool, float, int, etc.)
		json originalValue;                        // Value at time of creation, for revert
		json serializedTemplate = json::object();  // Preserved forward-compatible fields
		bool paused = false;                       // Temporarily disabled
		EntrySource source = EntrySource::User;
		std::string sourceFilename;                       // For overwrites: the filename it came from
		std::filesystem::path sourcePath;                 // For overwrites: exact file path
		TimeOfDayPeriod period = TimeOfDayPeriod::Count;  // Which period this entry belongs to (TimeOfDay only)
		std::optional<float> transitionSeconds;           // Location float transition duration
	};

	/// One indexed value in an atomic scene-setting update.
	struct EntryValueUpdate
	{
		size_t index;
		json value;
	};

	/// Summary for one ownership layer across every Scene Manager context.
	struct EntryLayerSummary
	{
		size_t count = 0;
		size_t paused = 0;
		size_t activeOverwrites = 0;

		bool Empty() const { return count == 0; }
		bool AllPaused() const { return count != 0 && paused == count; }
	};

	/// Result of exporting user settings into overwrite files.
	struct OverwriteExportResult
	{
		size_t writtenFiles = 0;
		size_t failedFiles = 0;

		bool Succeeded() const { return writtenFiles != 0 && failedFiles == 0; }
	};

	// --- Generic Entry Management (scene-type agnostic) ---

	const std::vector<SettingEntry>& GetEntries(SceneType type) const;
	/// Monotonic revision for entry structure and pause state used by presentation caches.
	std::uint64_t GetEntryPresentationRevision() const { return entryPresentationRevision; }
	bool HasEntryFromSource(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source) const;
	/// Add a setting.  For TimeOfDay entries, specify the target period.
	bool AddSetting(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value,
		TimeOfDayPeriod period = TimeOfDayPeriod::Count, bool deferCommit = false);
	void RemoveSetting(SceneType type, size_t index);
	void TogglePauseEntry(SceneType type, size_t index);
	void UpdateEntryValue(SceneType type, size_t index, const json& newValue, bool deferSave = false);
	/// Validate and update a group of entries before applying any of them.
	void UpdateEntryValues(SceneType type, std::span<const EntryValueUpdate> updates, bool deferSave = false);
	void CommitSceneSettingChanges();

	/// Revert an entry's value to its originalValue (captured at creation).
	void RevertEntryToDefault(SceneType type, size_t index);

	/// Check if an entry already exists for a specific period (TimeOfDay)
	bool HasEntryForPeriod(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey,
		TimeOfDayPeriod period, EntrySource source) const;

	void SetAllOverwritesPaused(SceneType type, bool paused);
	bool AreAllOverwritesPaused(SceneType type) const;
	void DeleteAllOverwrites(SceneType type);

	void SetAllUserPaused(SceneType type, bool paused);
	bool AreAllUserPaused(SceneType type) const;
	void DeleteAllUserSettings(SceneType type);

	/// Export selected user entries to grouped per-feature overwrite JSON files.
	OverwriteExportResult ExportUserSettingsToOverwrites(
		SceneType type, const std::vector<size_t>& indices, const std::string& modName);
	OverwriteExportResult ExportWeatherUserSettingsToOverwrites(
		RE::FormID weatherId, const std::vector<size_t>& indices, const std::string& modName);
	void DeleteAllWeatherUserSettings(RE::FormID weatherId);

	struct SceneContextId;

	/// Return counts for one ownership layer, optionally limited to a scene kind or target.
	EntryLayerSummary GetEntryLayerSummary(EntrySource source, std::optional<SceneContextType> scope = std::nullopt,
		const SceneContextId* target = nullptr);
	/// Pause or unpause one ownership layer as one save and resolver transaction.
	void SetEntryLayerPaused(EntrySource source, bool paused, std::optional<SceneContextType> scope = std::nullopt,
		const SceneContextId* target = nullptr);
	/// Delete every user entry or every loaded overwrite as one resolver transaction.
	void DeleteEntryLayer(EntrySource source, std::optional<SceneContextType> scope = std::nullopt,
		const SceneContextId* target = nullptr);
	/// Export a loaded ownership layer, optionally restricted to one scene kind.
	OverwriteExportResult ExportEntryLayerToOverwrites(const std::string& modName,
		EntrySource source, std::optional<SceneContextType> scope = std::nullopt,
		const SceneContextId* target = nullptr);

	// --- Scene Application ---

	/// Called every frame from State::Update().
	void Update();
	/** @brief Return whether a loaded player cell is available outside loading and main menus. */
	bool IsSceneReady() const;

	/// Applies the new location immediately after a loading-screen transition.
	void OnLoadingTransition();

	/// Check if any scene settings are active for a given feature
	bool HasActiveSettingsForFeature(const std::string& featureShortName) const;
	/// Applied settings, or settings in the current scene that can resume from a feature-wide pause.
	bool HasCurrentSceneSettingsForFeature(const std::string& featureShortName) const;
	bool IsActiveSceneSetting(std::string_view featureShortName,
		std::string_view settingPath, std::string_view settingKey) const;
	bool IsActiveSceneSetting(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey) const;
	void CaptureExternalFeatureChanges(Feature* feature);
	/// Replace active scene values in a serialized global settings document with their baselines.
	void RestoreBaselinesInSerializedSettings(json& settings) const;

	/// Per-feature pause: temporarily disable all scene-specific settings for a feature
	bool IsFeaturePaused(const std::string& featureShortName) const;
	void SetFeaturePaused(const std::string& featureShortName, bool paused);

	class SceneLayerGuard
	{
	public:
		explicit SceneLayerGuard(SceneSettingsManager& manager);
		~SceneLayerGuard();

		SceneLayerGuard(const SceneLayerGuard&) = delete;
		SceneLayerGuard& operator=(const SceneLayerGuard&) = delete;

	private:
		SceneSettingsManager& manager;
	};

	// --- Persistence ---

	/// Save all user data to SceneManager.json, returning false if the write fails.
	bool SaveAllUserSettings();
	/** @brief Reload saved scene files without writing or discarding a pending feature draft. */
	bool ReloadSceneSettings();

	void DiscoverOverwrites(SceneType type);

	/// Discover weather-specific overwrite files from Weather/{SPID}/ folders.
	void DiscoverWeatherOverwrites();

	/// Load non-weather scene types (overwrites + user settings). Called early from Setup().
	void LoadAll();

	// --- Path Resolution ---

	static std::string GetSceneTypeName(SceneType type);
	static std::filesystem::path GetUserSettingsFilePath();
	static std::filesystem::path GetOverwritesPath(SceneType type);

	// --- Time of Day Helpers (public for UI) ---

	static const char* GetPeriodName(TimeOfDayPeriod period);
	static TimeOfDayPeriod GetPeriodFromName(const std::string& name);
	static float GetCurrentGameHour();
	void GetTimeOfDayFactors(float outFactors[static_cast<int>(TimeOfDayPeriod::Count)]);

	/// Returns the period whose hour range contains the current game hour.
	static TimeOfDayPeriod GetCurrentPeriod();

	// --- Feature Metadata ---

	/// Get loaded feature short names with scene-visible settings.
	static std::vector<std::string> GetInteriorRelevantFeatureNames();

	/// Get loaded feature short names with transitionable settings.
	static std::vector<std::string> GetExteriorRelevantFeatureNames();
	static std::vector<std::string> GetLocationRelevantFeatureNames();

	/// Check whether the feature exposes settings supported by the scene type.
	static bool IsFeatureAllowedForType(SceneType type, const std::string& featureShortName);
	static bool IsSettingAllowedForType(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey);

	/// Check the shared catalog and settings blacklist policy.
	static bool IsSceneSettingAllowed(
		std::string_view featureShortName, std::string_view settingPath, std::string_view settingKey);

	/// Get the localized display name for a feature.
	static std::string GetFeatureDisplayName(const std::string& featureShortName);

	/// Logical Scene Manager editor represented by one or more persisted values.
	enum class SettingControlType : std::uint8_t
	{
		Scalar,
		Numeric,
		Color,
	};

	/// Visual treatment used for a logical aggregate setting.
	enum class AggregatePresentation : std::uint8_t
	{
		Components,
		ColorPicker,
	};

	/// Interaction used to edit all components of a logical aggregate.
	enum class UnifiedEditMode : std::uint8_t
	{
		None,
		Always,
		Shift,
	};

	/// One persisted primitive belonging to a logical Scene Manager control.
	struct SettingDescriptorMember
	{
		std::vector<std::string> settingPath;
		std::string key;
		std::string componentDisplayName;
		json value;
		std::int8_t componentIndex = -1;
		bool aggregateAll = false;
	};

	/// Catalog-backed setting presented in the add-setting dialog.
	struct SettingDescriptor
	{
		std::vector<std::string> settingPath;
		std::string key;
		std::string displayName;
		std::vector<std::string> displayPath;
		json value;
		SettingControlType controlType = SettingControlType::Scalar;
		AggregatePresentation aggregatePresentation = AggregatePresentation::Components;
		UnifiedEditMode unifiedEditMode = UnifiedEditMode::None;
		std::vector<SettingDescriptorMember> members;
	};

	/// Get scene-safe setting descriptors for a feature.
	static std::vector<SettingDescriptor> GetFeatureSceneSettings(
		SceneType type, const std::string& featureShortName);

	/// Get scene-safe float setting descriptors for time/weather blending.
	static std::vector<SettingDescriptor> GetTransitionableSceneSettings(const std::string& featureShortName);

	/// Logical-control metadata for one stored scene-setting entry.
	struct SettingControlInfo
	{
		std::vector<std::string> settingPath;
		std::string settingKey;
		std::string displayName;
		std::string componentDisplayName;
		std::vector<std::string> displayPath;
		std::vector<std::string> tableDisplayPath;
		SettingControlType controlType = SettingControlType::Scalar;
		std::int8_t componentIndex = -1;
		std::int8_t colorChannelIndex = -1;
		std::int8_t componentStart = -1;
		std::uint8_t componentCount = 0;
		bool aggregateAll = false;
		AggregatePresentation aggregatePresentation = AggregatePresentation::Components;
		UnifiedEditMode unifiedEditMode = UnifiedEditMode::None;
	};

	/// Get the logical ImGui control represented by a scalar scene setting.
	static bool GetSettingControlInfo(const SettingEntry& entry, SettingControlInfo& info);

	/// Get a UI-friendly display label for a setting key.
	static std::string GetSettingDisplayName(const std::string& settingKey);

	/// Get current value of a specific setting from a feature
	static json GetFeatureSettingValue(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey);

	/// Detect the JSON type of a setting value for UI rendering
	enum class SettingType
	{
		Boolean,
		Integer,
		Float,
		String,
		Unknown
	};
	static SettingType DetectSettingType(const json& value);
	static bool IsBooleanControlSetting(const SettingEntry& entry);
	static bool IsInvertedDisplaySetting(const SettingEntry& entry);
	static bool GetNumericBounds(const SettingEntry& entry, double& minimum, double& maximum);
	static double GetNumericDisplayScale(const SettingEntry& entry);
	/// Convert a raw stored numeric setting value to its Scene Manager display value.
	static bool GetNumericDisplayValue(const SettingEntry& entry, double storedValue, double& displayValue);
	/// Convert a Scene Manager display value to its raw stored numeric setting value.
	static bool GetNumericStoredValue(const SettingEntry& entry, double displayValue, double& storedValue);
	/// Return whether direct numeric input must remain within the source widget bounds.
	static bool IsNumericInputClamped(const SettingEntry& entry);
	/// Return whether the source color editor accepts high-dynamic-range component values.
	static bool IsHDRColorSetting(const SettingEntry& entry);
	static size_t GetSettingChoiceCount(const SettingEntry& entry);
	static bool GetSettingChoice(const SettingEntry& entry, size_t index, std::int64_t& value, std::string& displayName);

	// --- Per-Weather Scene Settings ---

	/// Normal entries and time-period entries are independent saved sets.
	struct PeriodicSceneConfig
	{
		bool timeOfDayEnabled = false;
		std::optional<bool> userTimeOfDayEnabled;
		std::optional<bool> overwriteTimeOfDayEnabled;
		std::vector<SettingEntry> entries;

		/// Resolve the local preference before imported defaults or an unambiguous saved set.
		void RefreshTimeOfDayMode()
		{
			if (userTimeOfDayEnabled)
				timeOfDayEnabled = *userTimeOfDayEnabled;
			else if (overwriteTimeOfDayEnabled)
				timeOfDayEnabled = *overwriteTimeOfDayEnabled;
			else
				timeOfDayEnabled = !entries.empty() && std::ranges::all_of(entries, [](const auto& entry) {
					return entry.period >= TimeOfDayPeriod::Dawn && entry.period < TimeOfDayPeriod::Count;
				});
		}

		/// Select the saved set without converting or copying its values.
		bool IsPeriodActive(TimeOfDayPeriod period) const
		{
			return timeOfDayEnabled ? period >= TimeOfDayPeriod::Dawn && period < TimeOfDayPeriod::Count :
			                          period == TimeOfDayPeriod::Count;
		}
	};
	using WeatherSceneConfig = PeriodicSceneConfig;

	/// Return the active mode of a weather or location.
	bool IsSceneTimeOfDayEnabled(const SceneContextId& context) const;
	/// Persist a mode change without modifying either saved set.
	void SetSceneTimeOfDayEnabled(const SceneContextId& context, bool enabled);

	const WeatherSceneConfig& GetWeatherConfig(RE::FormID weatherId);
	bool HasWeatherConfig(RE::FormID weatherId);

	/// Add a float to Normal (Count) or one time period.
	bool AddWeatherSetting(RE::FormID weatherId, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period,
		bool deferSave = false);
	void RemoveWeatherSetting(RE::FormID weatherId, size_t index);
	/** @brief Remove selected entries with one write per backing file and one catalogue refresh. */
	void RemoveSceneSettings(const SceneContextId& context, std::span<const size_t> indices);
	void TogglePauseWeatherEntry(RE::FormID weatherId, size_t index);
	/// Set weather entries to one pause state with a single persistence and resolver update.
	void SetWeatherEntriesPaused(RE::FormID weatherId, std::span<const size_t> indices, bool paused);
	void UpdateWeatherEntryValue(RE::FormID weatherId, size_t index, const json& newValue, bool deferSave = false);
	/// Validate and update weather entries as one mutation.
	void UpdateWeatherEntryValues(
		RE::FormID weatherId, std::span<const EntryValueUpdate> updates, bool deferSave = false);
	void RevertWeatherEntryToDefault(RE::FormID weatherId, size_t index);
	bool HasWeatherEntryForPeriod(RE::FormID weatherId, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period,
		std::optional<EntrySource> source = std::nullopt);

	/// Return or change the weather's active saved set.
	bool IsWeatherShowTimeOfDay(RE::FormID weatherId);
	void SetWeatherShowTimeOfDay(RE::FormID weatherId, bool show);

	static std::filesystem::path GetWeatherOverwritesDir();

	// --- Per-Location Scene Settings ---

	enum class LocationTargetType
	{
		Region,
		LocationType,
		Location,
		Cell,
		Worldspace
	};

	struct LocationTarget
	{
		LocationTargetType type = LocationTargetType::Location;
		std::string formKey;
		std::string name;
		std::string cocCode;
		std::vector<std::string> locationTypes;
		RE::FormID formId = 0;
	};

	struct LocationSceneConfig : PeriodicSceneConfig
	{
		LocationTargetType type = LocationTargetType::Location;
		std::string formKey;
		std::string name;
		std::string cocCode;
	};

	/// Return cached targets from worldspace through location type, region, parent location, and cell.
	const std::vector<LocationTarget>& GetCurrentLocationTargets() const;
	/// Return worldspace, location-type, region, location, and named cell targets for persistent management.
	const std::vector<LocationTarget>& GetLocationManagementTargets() const;
	const LocationSceneConfig& GetLocationConfig(LocationTargetType type, std::string_view formKey) const;
	bool HasLocationConfig(LocationTargetType type, std::string_view formKey) const;
	/// Return whether this location uses its float-only Time of Day set.
	bool IsLocationShowTimeOfDay(LocationTargetType type, std::string_view formKey) const;
	/// Select the location's saved set and refresh its active settings.
	void SetLocationShowTimeOfDay(LocationTargetType type, const std::string& formKey, bool show);
	bool AddLocationSetting(LocationTargetType type, const std::string& formKey, const std::string& name,
		const std::string& cocCode,
		const std::string& featureShortName, const std::vector<std::string>& settingPath,
		const std::string& settingKey, bool deferSave = false, TimeOfDayPeriod period = TimeOfDayPeriod::Count);
	void RemoveLocationSetting(LocationTargetType type, const std::string& formKey, size_t index);
	void TogglePauseLocationEntry(LocationTargetType type, const std::string& formKey, size_t index);
	/// Set location entries to one pause state with a single persistence and resolver update.
	void SetLocationEntriesPaused(LocationTargetType type, const std::string& formKey,
		std::span<const size_t> indices, bool paused);
	void UpdateLocationEntryValue(LocationTargetType type, const std::string& formKey, size_t index,
		const json& newValue, bool deferSave = false);
	/// Validate and update location entries as one mutation.
	void UpdateLocationEntryValues(LocationTargetType type, const std::string& formKey,
		std::span<const EntryValueUpdate> updates, bool deferSave = false);
	void RevertLocationEntryToDefault(LocationTargetType type, const std::string& formKey, size_t index);
	bool HasLocationEntry(LocationTargetType type, std::string_view formKey,
		const std::string& featureShortName, const std::vector<std::string>& settingPath,
		const std::string& settingKey, std::optional<EntrySource> source = std::nullopt,
		TimeOfDayPeriod period = TimeOfDayPeriod::Count) const;
	OverwriteExportResult ExportLocationUserSettingsToOverwrites(LocationTargetType type, const std::string& formKey,
		const std::vector<size_t>& indices, const std::string& modName);
	void DeleteAllLocationUserSettings(LocationTargetType type, const std::string& formKey);
	static std::filesystem::path GetLocationOverwritesDir(LocationTargetType type);

	/// Default duration used by location float transitions.
	static constexpr float kDefaultLocationTransitionSeconds = 5.0f;
	/// Largest accepted typed location transition duration.
	static constexpr float kMaxLocationTransitionSeconds = 300.0f;

	/// Return an entry-specific location transition duration, or null for the default.
	std::optional<float> GetLocationEntryTransitionSeconds(
		LocationTargetType type, std::string_view formKey, size_t index) const;
	/// Set one or more location entries to the same transition duration as one atomic edit.
	void SetLocationEntryTransitionSeconds(LocationTargetType type, const std::string& formKey,
		std::span<const size_t> indices, std::optional<float> seconds, bool deferSave = false);

	// --- Generic Scene Copy ---

	/// Identifies one physical persisted setting.
	struct SettingIdentity
	{
		std::string featureShortName;
		std::vector<std::string> settingPath;
		std::string settingKey;

		auto operator<=>(const SettingIdentity&) const = default;
	};

	/// Stable identity for an interior, time period, weather period, or location target.
	struct SceneContextId
	{
		SceneContextType type = SceneContextType::TimeOfDay;
		TimeOfDayPeriod period = TimeOfDayPeriod::Count;
		bool allPeriods = false;
		RE::FormID weatherId = 0;
		LocationTargetType locationType = LocationTargetType::Location;
		std::string locationFormKey;

		auto operator<=>(const SceneContextId&) const = default;
	};

	/// Summarize saved entries and entry pauses in both layers; an empty feature includes every feature.
	EntryLayerSummary GetFeatureSceneSummary(std::string_view featureShortName, const SceneContextId& context, bool matchPeriod = true) const;
	/// Summarize every saved target and mode of one scene kind for a feature.
	EntryLayerSummary GetFeatureSceneSummary(std::string_view featureShortName, SceneContextType type) const;
	/// Pause or resume both ownership layers for this feature in the selected saved scene set.
	void SetFeatureSceneSettingsPaused(std::string_view featureShortName, const SceneContextId& context, bool paused);

	/// How an existing destination user setting is handled.
	enum class CopyConflictPolicy : std::uint8_t
	{
		SkipExisting,
		OverwriteExisting,
		Cancel,
	};

	/// One source context with active scene settings.
	struct CopySource
	{
		SceneContextId context;
		std::string displayName;
		size_t settingCount = 0;
	};

	/// One logical setting exposed by a source context.
	struct CopySourceSetting
	{
		/// Representative physical identity; aggregate members resolve to the same logical control.
		SettingIdentity setting;
		std::string displayName;
		bool numeric = false;
	};

	/// One logical setting available to copy.
	struct CopyCandidate
	{
		/// Representative physical identity; aggregate members resolve to the same logical control.
		SettingIdentity setting;
		std::string displayName;
		json value;
		bool compatible = false;
		bool conflicts = false;
		bool maskedByOverwrite = false;
		TimeOfDayPeriod sourcePeriod = TimeOfDayPeriod::Count;
		TimeOfDayPeriod destinationPeriod = TimeOfDayPeriod::Count;
	};

	/// Aggregate result of one transactional copy.
	struct CopyResult
	{
		size_t copied = 0;
		size_t skipped = 0;
		size_t overwritten = 0;
		size_t incompatible = 0;
		bool hadConflicts = false;
		bool cancelled = false;

		/// Return whether the operation changed the destination.
		bool Changed() const { return copied != 0 || overwritten != 0; }
	};

	/// Return non-empty contexts that can participate as copy sources.
	std::vector<CopySource> GetCopySources(EntrySource sourceLayer) const;
	/// Return logical source settings, optionally restricted to a compatible destination type.
	std::vector<CopySourceSetting> GetCopySourceSettings(
		const SceneContextId& source, EntrySource sourceLayer,
		std::optional<SceneContextType> destinationType = std::nullopt) const;
	/// Inspect the settings and conflicts in a proposed copy without mutating state.
	std::vector<CopyCandidate> GetCopyCandidates(const SceneContextId& source,
		const SceneContextId& destination, EntrySource sourceLayer,
		std::span<const SettingIdentity> settings) const;
	/// Copy settings as one validated mutation and one save/reapply operation.
	CopyResult CopySettings(const SceneContextId& source, const SceneContextId& destination,
		EntrySource sourceLayer, CopyConflictPolicy conflictPolicy,
		std::span<const SettingIdentity> settings, bool deferCommit = false);
	/** @brief Delete every User setting owned by one feature in exactly one scene context.
	 * @return True when at least one entry was removed. */
	bool DeleteFeatureSceneSettings(
		std::string_view featureShortName, const SceneContextId& context);

	/** @brief Begin editing one feature against an isolated scene-context preview. */
	bool BeginFeatureSceneEdit(Feature* feature, const SceneContextId& context);
	/** @brief Capture live feature controls into the current edit preview without persisting them. */
	bool CaptureFeatureSceneEditChanges(Feature* feature);
	/** @brief Persist pending feature-page edits as User Settings in the selected scene. */
	bool StoreFeatureSceneEdit();
	/** @brief Finish feature-page editing, optionally storing pending changes first. */
	void EndFeatureSceneEdit(bool storeChanges = true);
	/** @brief Return whether the named feature owns the active feature-page edit session. */
	bool IsFeatureSceneEditing(std::string_view featureShortName) const;
	/** @brief Return whether the feature's native controls currently show its edit preview. */
	bool CanCaptureFeatureSceneEdit(std::string_view featureShortName) const;
	/** @brief Return whether one catalog setting belongs to the active feature-page edit context. */
	bool IsFeatureSceneEditSetting(std::string_view featureShortName,
		std::string_view settingPath, std::string_view settingKey) const;
	/** @brief Return whether the active feature-page edit session has unstored changes. */
	bool HasPendingFeatureSceneEdits() const;
	/// Temporarily bypass overwrites in the current feature preview without changing saved pause flags.
	void SetFeatureSceneEditOverwritesPaused(bool paused);
	bool AreFeatureSceneEditOverwritesPaused() const;
	/** @brief Return whether loading or active overwrites block toolbar scene actions. */
	bool AreFeatureSceneEditActionsLocked() const;
	bool HasFeatureSceneEditOverwrites(const SceneContextId* context = nullptr,
		bool matchPeriod = true, bool wholeType = false) const;
	/// Return whether an active overwrite locks this control in the current preview.
	bool IsFeatureSceneEditSettingOverwritten(std::string_view featureShortName,
		std::string_view settingPath, std::string_view settingKey) const;
	bool IsFeatureSceneEditSettingAltered(std::string_view featureShortName,
		std::string_view settingPath, std::string_view settingKey) const;
	/** @brief Return the generation of the active feature-page edit context. */
	std::uint64_t GetFeatureSceneEditRevision() const { return featureSceneEditRevision; }

	/// Enables location discovery once Skyrim form data is guaranteed to be available.
	void OnDataLoaded();

protected:
	SceneSettingsManager();
	~SceneSettingsManager();

private:
	SceneSettingsManager(const SceneSettingsManager&) = delete;
	SceneSettingsManager& operator=(const SceneSettingsManager&) = delete;

	// --- Per scene-type storage ---
	std::map<SceneType, std::vector<SettingEntry>> entries;
	std::map<SceneType, std::vector<json>> unresolvedUserEntries;
	std::uint64_t entryPresentationRevision = 0;
	json preservedUserSettingsRoot = json::object();
	bool userSettingsDocumentLoaded = false;
	bool userSettingsDocumentWritable = true;
	bool userSettingsWriteBlockedWarning = false;
	bool interiorUserSettingsModified = false;
	bool timeOfDayUserSettingsModified = false;
	bool weatherUserSettingsModified = false;
	bool locationUserSettingsModified = false;
	bool dataLoaded = false;
	bool deferredSceneChangesPending = false;
	std::chrono::steady_clock::time_point deferredSceneChangesDeadline{};
	static constexpr auto kDeferredSaveDelay = std::chrono::milliseconds(250);
	static constexpr auto kDeferredSaveRetryDelay = std::chrono::seconds(2);

	std::atomic<bool> queuedLoadingTransition = false;

	/// Minimum blend-progress or real-time change before advancing the resolver.
	static constexpr float kBlendEpsilon = 1e-3f;

	/// Minimum game-hour delta before re-running the blend. At the default
	/// timescale (20x), this equals about 0.18 real seconds.
	static constexpr float kHourUpdateThreshold = 1e-3f;

	// --- Pause states ---
	std::map<std::string, bool> featurePauseStates;
	int sceneLayerSuspendDepth = 0;

	// --- Per-Weather Scene storage ---
	std::map<RE::FormID, WeatherSceneConfig> weatherSceneConfigs;
	static const WeatherSceneConfig kEmptyWeatherConfig;
	std::map<SceneContextId, std::set<std::filesystem::path>> overwriteBackingFiles;

	/// UI preference per weather: show TOD table vs flat view (keyed by FormID for fast access).

	json unresolvedWeatherUserSettings = json::object();
	bool weatherDataLoaded = false;

	// --- Per-Location Scene storage ---
	std::map<std::string, LocationSceneConfig> locationSceneConfigs;
	static const LocationSceneConfig kEmptyLocationConfig;
	json unresolvedLocationUserSettings = json::object();
	bool locationDataLoaded = false;
	bool gameDataReady = false;

	struct SettingAddress
	{
		std::string featureShortName;
		std::vector<std::string> settingPath;
		std::string settingKey;

		auto operator<=>(const SettingAddress&) const = default;
	};
	struct CatalogSceneSettingUpdate
	{
		std::vector<std::string> settingPath;
		std::string key;
		json value;
	};

	using ResolvedSettingMap = std::map<SettingAddress, json>;
	using PeriodValues = std::array<std::optional<float>, kPeriodCount>;
	using PeriodSettingMap = std::map<SettingAddress, PeriodValues>;
	struct FeatureSceneEditState
	{
		std::string featureShortName;
		SceneContextId context;
		std::vector<SettingAddress> editableAddresses;
		json originalSettings = json::object();
		json workingSettings = json::object();
		ResolvedSettingMap workingOverrides;
		bool dirty = false;
		bool overwritesPaused = false;
		std::set<SettingAddress> overwriteAddresses;
		std::set<SettingAddress> alteredAddresses;
	};
	ResolvedSettingMap baselineSettings;
	ResolvedSettingMap appliedSettings;
	ResolvedSettingMap resolvedSettingsScratch;
	std::set<std::string> restoreFailureWarnings;
	std::map<std::string, std::chrono::steady_clock::time_point> restoreRetryAfter;
	struct ApplyFailureState
	{
		size_t signature = 0;
		std::chrono::steady_clock::time_point retryAfter{};
		bool warningLogged = false;
	};
	std::map<std::string, ApplyFailureState> applyFailures;
	std::map<std::string, ApplyFailureState> transitionApplyFailures;
	struct PendingApplyVerification
	{
		std::uint32_t appliedFrame = 0;
		std::chrono::steady_clock::time_point deadline;
		std::vector<CatalogSceneSettingUpdate> updates;
		std::vector<SettingAddress> restorationAddresses;
		size_t signature = 0;
		bool transition = false;
	};
	std::map<std::string, PendingApplyVerification> pendingApplyVerifications;
	std::map<std::string, json> featureApplyDocuments;
	static constexpr auto kApplyVerificationMaxDeferral = std::chrono::seconds(1);
	static constexpr auto kApplyRetryDelay = std::chrono::seconds(2);
	bool resolverDirty = true;
	bool resolverSuspended = false;
	bool activeEntryCacheDirty = true;
	bool hasActiveSceneEntries = false;
	std::uint32_t lastUpdateFrame = std::numeric_limits<std::uint32_t>::max();
	bool lastResolvedInterior = false;
	RE::FormID lastResolvedLocationId = 0;
	RE::FormID lastResolvedCellId = 0;
	RE::FormID lastResolvedRegionId = 0;
	RE::FormID lastResolvedWorldspaceId = 0;
	float lastResolvedHour = -1.0f;
	RE::FormID lastResolvedCurrentWeatherId = 0;
	RE::FormID lastResolvedPreviousWeatherId = 0;
	float lastResolvedWeatherLerp = -1.0f;
	bool suppressLocationTransitionUntilContextResolved = false;
	mutable RE::FormID cachedPreviousWeatherId = 0;
	mutable RE::FormID cachedTargetLocationId = 0;
	mutable RE::FormID cachedTargetCellId = 0;
	mutable RE::FormID cachedTargetRegionId = 0;
	mutable RE::FormID cachedTargetWorldspaceId = 0;
	mutable bool locationTargetsCached = false;
	mutable std::vector<LocationTarget> cachedLocationTargets;
	mutable bool locationManagementTargetsCached = false;
	mutable std::vector<LocationTarget> cachedLocationManagementTargets;

	struct LocationTransitionLayer
	{
		float noLocationWeight = 1.0f;
		float userWeight = 0.0f;
		float userWeightedValue = 0.0f;
		float overwriteWeight = 0.0f;
		float overwriteWeightedValue = 0.0f;
		bool operator==(const LocationTransitionLayer&) const = default;
	};
	using LocationTransitionLayers = std::array<LocationTransitionLayer, kPeriodCount>;

	struct LocationTransition
	{
		float startValue = 0.0f;
		float targetValue = 0.0f;
		float startTime = 0.0f;
		float duration = 0.0f;
		bool restoreAtEnd = false;
		bool exitsLocationLayer = false;
		LocationTransitionLayers startLayers;
		LocationTransitionLayers targetLayers;
	};
	struct LocationTransitionBatch
	{
		std::vector<SettingAddress> addresses;
		std::vector<LocationTransition*> transitions;
		std::vector<CatalogSceneSettingUpdate> updates;
		size_t signature = 0;
	};
	std::map<SettingAddress, LocationTransition> activeLocationTransitions;
	std::map<std::string, LocationTransitionBatch> locationTransitionBatches;
	bool locationTransitionBatchesDirty = true;
	std::optional<float> lastLocationTransitionApplyTime;
	std::map<SettingAddress, LocationTransitionLayers> lastLocationLayers;
	std::map<SettingAddress, float> lastLocationTransitionDurations;
	std::map<SettingAddress, float> pendingLocationTransitionDurations;
	ResolvedSettingMap cachedLocationUserSettings;
	ResolvedSettingMap cachedLocationOverwriteSettings;
	PeriodSettingMap cachedLocationUserPeriods;
	PeriodSettingMap cachedLocationOverwritePeriods;
	bool cachedLocationOverridesValid = false;
	bool locationOverridesDirty = true;
	std::optional<FeatureSceneEditState> featureSceneEdit;
	std::uint64_t featureSceneEditRevision = 0;

	// --- Per-Weather helpers ---
	/// Load weather overwrites/user settings once game data is available for SPID resolution.
	bool TryEnsureWeatherDataLoaded();
	bool TryEnsureLocationDataLoaded();
	void LoadWeatherData();
	WeatherSceneConfig& GetWeatherConfigMut(RE::FormID weatherId);
	const PeriodicSceneConfig* GetPeriodicSceneConfig(const SceneContextId& context) const;
	RE::FormID GetEffectivePreviousWeatherId(const RE::Sky* sky, float weatherLerp) const;
	struct CachedPeriodSettingMap
	{
		std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
		PeriodSettingMap values;
	};
	static constexpr size_t kEntrySourceCount = static_cast<size_t>(EntrySource::Overwrite) + 1;
	static constexpr size_t kCombinedPeriodValueCacheIndex = kEntrySourceCount;
	static constexpr size_t GetPeriodValueCacheIndex(std::optional<EntrySource> source)
	{
		return source ? static_cast<size_t>(*source) : kCombinedPeriodValueCacheIndex;
	}
	std::uint64_t sceneValueRevision = 0;
	mutable std::array<CachedPeriodSettingMap, kEntrySourceCount + 1> timeOfDayValueGroups;
	mutable std::map<RE::FormID, std::array<CachedPeriodSettingMap, kEntrySourceCount + 1>> weatherValueGroups;
	const PeriodSettingMap& BuildTimeOfDayValueGroups(std::optional<EntrySource> source = std::nullopt) const;
	const PeriodSettingMap& BuildWeatherValueGroups(
		RE::FormID weatherId, std::optional<EntrySource> source = std::nullopt) const;

	// --- Central runtime resolver ---
	void ResolveAndApply(bool force = false, bool allowLocationTransitions = true);
	bool HasActiveSceneEntriesCached();
	ResolvedSettingMap& BuildResolvedSettings(bool collectLocationTransitionDurations);
	void ApplyResolvedSettings(const ResolvedSettingMap& resolved, bool forceRetry);
	void StartLocationTransitions(const ResolvedSettingMap& resolved, float now, bool animateChanges);
	void RefreshLocationTransitionEndpoints(const ResolvedSettingMap& resolved);
	bool AdvanceLocationTransitions(float now);
	void RebuildLocationTransitionBatches();
	void ClearLocationTransitions();
	void RestoreAppliedSettings();
	void ResolveInteriorSettings(ResolvedSettingMap& resolved, EntrySource source) const;
	void ResolveExteriorSettings(ResolvedSettingMap& resolved,
		const std::array<float, kPeriodCount>& factors,
		const ResolvedSettingMap* userLocationValues,
		std::span<const SettingAddress> addressFilter = {},
		const PeriodSettingMap* userLocationPeriods = nullptr,
		const PeriodSettingMap* overwriteLocationPeriods = nullptr, bool transitionStart = false,
		bool applyOverwrites = true, std::set<SettingAddress>* appliedOverwrites = nullptr,
		std::set<SettingAddress>* appliedSceneSettings = nullptr) const;
	void ResolveTimeOfDaySettings(ResolvedSettingMap& resolved, const PeriodSettingMap& values,
		const std::array<float, kPeriodCount>& factors) const;
	void ResolveWeatherSettings(ResolvedSettingMap& resolved, const PeriodSettingMap& timeOfDayValues,
		const std::array<float, kPeriodCount>& factors,
		std::optional<EntrySource> valueSource = std::nullopt) const;
	void ResolveWeatherValueGroups(ResolvedSettingMap& resolved,
		const PeriodSettingMap& timeOfDayValues,
		const std::array<float, kPeriodCount>& factors,
		const PeriodSettingMap& currentValues, const PeriodSettingMap& previousValues,
		float weatherLerp) const;
	void ResolveLocationSettings(ResolvedSettingMap& resolved,
		std::span<const LocationTarget> locationTargets, EntrySource source,
		bool collectTransitionDurations, PeriodSettingMap* periodValues = nullptr,
		TimeOfDayPeriod period = TimeOfDayPeriod::Count);
	void OverlayEntries(ResolvedSettingMap& resolved, const std::vector<SettingEntry>& sourceEntries,
		SceneType type, std::optional<EntrySource> source = std::nullopt,
		std::map<SettingAddress, float>* transitionDurations = nullptr) const;
	std::optional<float> ResolveWeatherLowerValue(RE::FormID weatherId, const SettingAddress& address,
		TimeOfDayPeriod period, EntrySource selectedSource);
	json GetBaselineValue(const SettingAddress& address);
	const json* GetFeatureBaseSnapshot(const std::string& featureShortName);
	void EnsureBaselines(std::span<const SettingAddress> addresses);
	std::optional<json> ResolveLocationLowerValue(LocationTargetType type, std::string_view formKey,
		const SettingAddress& address, EntrySource selectedSource,
		TimeOfDayPeriod period = TimeOfDayPeriod::Count);
	std::optional<ResolvedSettingMap> BuildLocationLowerLayers(LocationTargetType type,
		std::string_view formKey, std::optional<EntrySource> selectedSource = std::nullopt,
		TimeOfDayPeriod period = TimeOfDayPeriod::Count);
	static bool ResolvedValuesEqual(const json& lhs, const json& rhs);
	static bool AppliedValuesEqual(const json& actual, const json& expected);
	static size_t GetCatalogUpdateSignature(std::string_view featureShortName,
		std::span<const CatalogSceneSettingUpdate> updates);
	bool ApplyCatalogSceneSettings(
		Feature& feature, const std::vector<CatalogSceneSettingUpdate>& updates);
	void ScheduleApplyVerification(std::string_view featureShortName,
		const std::vector<CatalogSceneSettingUpdate>& updates, size_t signature, bool transition,
		std::span<const SettingAddress> restorationAddresses = {});
	void VerifyPendingApplies(bool overdueOnly = false);
	bool SnapshotFeatureSceneEdit(Feature& feature, json& snapshot) const;
	void RefreshFeatureSceneEditOverrides(json liveSettings);
	void RebaseFeatureSceneEditPreview();
	void ApplyFeatureSceneEditPreview(ResolvedSettingMap& resolved);
	bool IsFeatureSceneEditPreviewActive() const;
	bool IsFeatureSceneEditContextValid(
		std::string_view featureShortName, const SceneContextId& context);

	// --- Per-Location helpers ---
	static std::string GetLocationConfigKey(LocationTargetType type, std::string_view formKey);
	LocationSceneConfig& GetLocationConfigMut(LocationTargetType type, const std::string& formKey,
		const std::string& name = {});
	void DiscoverLocationOverwrites();
	void DiscoverLocationOverwritesForTarget(LocationTargetType type, const std::filesystem::path& targetDir);
	void LoadLocationUserSettings(const json& data);
	void PrepareLocationUserSettingsMutation(LocationTargetType type, std::string_view formKey,
		bool replaceMalformedEntries);
	static const char* GetLocationSectionName(LocationTargetType type);
	static const char* GetLocationTargetTypeName(LocationTargetType type);

	// --- Helpers ---
	std::vector<SettingEntry>& GetEntriesMut(SceneType type);
	void BumpEntryPresentationRevision();
	bool IsEntryActive(const SettingEntry& entry) const;
	bool HasDuplicateEntry(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey,
		EntrySource source, TimeOfDayPeriod period = TimeOfDayPeriod::Count) const;

	void ReapplyIfActive(bool activeSetMayHaveChanged = true);
	void MarkEntryListUserSettingsModified(SceneType type);
	void PrepareWeatherUserSettingsMutation(RE::FormID weatherId, bool replaceMalformedEntries);
	void MarkDeferredSceneChanges();
	void FlushDeferredSceneChanges();
	void SuspendSceneLayer();
	void ResumeSceneLayer();
	float GetPauseAwareTime() const;

	struct CachedFeaturePresentation
	{
		std::vector<SettingDescriptor> interiorSettings;
		std::vector<SettingDescriptor> timeOfDaySettings;
		std::vector<SettingDescriptor> locationSettings;
	};
	mutable std::map<std::string, CachedFeaturePresentation> featurePresentationCache;
	mutable std::map<std::string, json> featureBaseSnapshots;
	std::set<std::string> appliedFeatureNames;
	const std::vector<SettingDescriptor>& GetCachedFeatureSceneSettings(
		SceneType type, const std::string& featureShortName);
	void InvalidateFeatureSnapshot(std::string_view featureShortName = {});

	const std::vector<SettingEntry>* GetCopyContextEntries(const SceneContextId& context) const;
	std::vector<SettingEntry>* GetCopyContextEntriesMut(const SceneContextId& context);
	bool IsCopyEntryCompatible(const SettingEntry& entry, SceneContextType destinationType) const;
	std::vector<CopyCandidate> BuildCopyCandidates(const SceneContextId& source,
		const SceneContextId& destination, EntrySource sourceLayer,
		std::span<const SettingIdentity> settings) const;
	static bool IsValidSceneContext(const SceneContextId& context);
	void ReloadOverwriteEntries();

	// --- Overwrite discovery helper ---
	void DiscoverOverwritesInDir(SceneType type, const std::filesystem::path& dir,
		TimeOfDayPeriod period = TimeOfDayPeriod::Count);

	/// Discover overwrite files for a single weather SPID folder.
	void DiscoverWeatherOverwritesForSpid(RE::FormID weatherId, const std::filesystem::path& weatherDir);

	/// Load non-weather user settings from unified SceneManager.json.
	void LoadAllUserSettings();

	/// Load weather user settings from SceneManager.json. Requires TESDataHandler.
	void LoadWeatherUserSettings();
};
