#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <tuple>

#include "SceneSettingsManager.h"
#include "Utils/UI.h"

struct Feature;

/// Shared UI utilities for scene-settings panels.
namespace SceneSettingsUI
{
	using SceneType = SceneSettingsManager::SceneType;
	using EntrySource = SceneSettingsManager::EntrySource;
	using Period = SceneSettingsManager::TimeOfDayPeriod;
	using SceneSettingControlType = SceneSettingsManager::SettingControlType;
	using SceneSettingDescriptor = SceneSettingsManager::SettingDescriptor;
	static constexpr int kPeriodCount = SceneSettingsManager::kPeriodCount;

	/// Unique setting identifier for TOD table ordering.
	struct SettingId
	{
		std::string feature;
		std::vector<std::string> path;
		std::string key;
		std::string displayName;
		std::string categoryName;
		std::vector<std::string> parentPath;
		std::int8_t componentStart = -1;
		bool operator<(const SettingId& o) const { return std::tie(feature, path, key, componentStart) < std::tie(o.feature, o.path, o.key, o.componentStart); }
	};

	/// One logical setting row with cached entry membership.
	struct SourceRow
	{
		SettingId setting;
		std::array<std::vector<size_t>, kPeriodCount> cells;
		std::vector<size_t> indices;
		std::vector<size_t> addPeriodSourceIndices;
	};

	/// Contiguous rows sharing one category header.
	struct SourceCategoryRange
	{
		size_t begin = 0;
		size_t end = 0;
	};

	/// Cached table model built from a set of entries.
	struct SourceGroup
	{
		std::vector<SourceRow> rows;
		std::vector<SourceCategoryRange> categories;
		std::array<std::vector<size_t>, kPeriodCount> perColumn;
	};

	/// Measured table columns shared by a section header and its settings table.
	struct SourceTableLayout
	{
		int numValueColumns = 1;
		std::array<float, kPeriodCount> valueColumnWidths{};
		float auxiliaryColumnWidth = 0.0f;
		float sectionWidth = 0.0f;
		bool checkboxOnlyValueColumn = false;
	};

	/// Build a SourceGroup from entries, optionally filtered to a single source.
	SourceGroup BuildSourceGroup(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		EntrySource sourceFilter, bool filterBySource = true, bool transitionOnly = false,
		bool multiColumn = false, std::optional<bool> periodMode = std::nullopt);

	/// Split entry indices by source (Overwrite vs User).
	void SplitBySource(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		std::vector<size_t>& overwriteOut, std::vector<size_t>& userOut, bool transitionOnly = false, std::optional<bool> periodMode = std::nullopt);

	/// Remove entries by indices in reverse order.
	void RemoveIndicesReversed(const std::vector<size_t>& indices,
		std::function<void(size_t)> removeFn);

	struct AddSettingNode
	{
		std::map<std::string, AddSettingNode> children;
		std::vector<size_t> settings;
	};

	/// Cached data for the standalone copy panel.
	struct CopySettingState
	{
		EntrySource sourceLayer = EntrySource::User;
		std::uint64_t revision = 0;
		std::string locale;
		std::vector<SceneSettingsManager::CopySource> availableSources;
		std::vector<SceneSettingsManager::CopySource> sources;
		std::vector<std::string> sourceLabels;
		std::optional<SceneSettingsManager::SceneContextType> sourceTypeFilter;
		std::optional<SceneSettingsManager::SceneContextId> sourceContextFilter;
		std::string sourceSearch;
		std::optional<SceneSettingsManager::SceneContextId> selectedSourceContext;
		int selectedSource = -1;
		std::vector<SceneSettingsManager::CopySourceSetting> sourceSettings;
		std::string settingSearch;
		std::vector<size_t> visibleSettingIndices;
		std::optional<SceneSettingsManager::SceneContextId> selectionSourceContext;
		std::set<SceneSettingsManager::SettingIdentity> selectedSettings;

		void Reset()
		{
			sourceLayer = EntrySource::User;
			revision = 0;
			locale.clear();
			availableSources.clear();
			sources.clear();
			sourceLabels.clear();
			sourceTypeFilter.reset();
			sourceContextFilter.reset();
			sourceSearch.clear();
			selectedSourceContext.reset();
			selectedSource = -1;
			sourceSettings.clear();
			settingSearch.clear();
			visibleSettingIndices.clear();
			selectionSourceContext.reset();
			selectedSettings.clear();
		}
	};

	/// Persistent state for the "+" add-setting dialog.
	struct AddSettingState
	{
		bool dialogOpen = false;
		int selectedFeatureIdx = -1;
		std::vector<std::string> cachedFeatureNames;
		std::vector<SceneSettingDescriptor> cachedSettings;
		std::vector<int> selectedSubFeaturePath;
		AddSettingNode settingTree;
		std::vector<bool> selectedSettings;  // Checkbox state per setting key
		std::vector<int> selectedMembers;    // -1 selects every member of a logical control
		std::vector<std::vector<uint8_t>> cachedAddedMembers;
		bool addedMembersCached = false;
		std::uint64_t cachedAddedRevision = 0;
		bool shiftWasDown = false;
		bool measureInitialLayout = false;
		int lastDrawFrame = -1;
		void Reset()
		{
			dialogOpen = false;
			selectedFeatureIdx = -1;
			cachedFeatureNames.clear();
			cachedSettings.clear();
			selectedSubFeaturePath.clear();
			settingTree = {};
			selectedSettings.clear();
			selectedMembers.clear();
			cachedAddedMembers.clear();
			addedMembersCached = false;
			cachedAddedRevision = 0;
			shiftWasDown = false;
			measureInitialLayout = false;
			lastDrawFrame = -1;
		}
	};

	/// Shared confirmation popup state for a panel.
	struct PopupState
	{
		Util::ConfirmationPopup deleteAllOverwrites;
		Util::ConfirmationPopup deleteSingleOverwrite;
		Util::ConfirmationPopup deleteRowOverwrite;
		Util::ConfirmationPopup deleteAllUser;
		size_t pendingDeleteIndex = SIZE_MAX;
		std::vector<size_t> pendingDeleteRow;
	};

	/// Reset and open the add-setting dialog.
	void OpenAddDialog(SceneType type, AddSettingState& state);
	void OpenWeatherAddDialog(RE::FormID weatherId, AddSettingState& state);

	/// Draw the modal add-setting dialog. Call each frame for each active dialog state.
	void DrawAddSettingDialog(SceneType type, AddSettingState& state,
		Period period = Period::Count, bool addToAllPeriods = false);
	void DrawWeatherAddDialog(RE::FormID weatherId, AddSettingState& state,
		Period period = Period::Count, bool addToAllPeriods = false);

	/// Result from DrawFlyoutControls indicating which action the user triggered.
	struct FlyoutResult
	{
		bool toggled = false;
		bool reverted = false;
		bool deleted = false;
	};

	/// Flyout state for the shared table renderer (one set per table instance).
	struct TableFlyoutState
	{
		Util::FlyoutState cell;  // Per-value-cell flyout
		Util::FlyoutState row;   // Row-level flyout (setting name)
		Util::FlyoutState col;   // Column-header flyout (period names)
		size_t cellSourceRow = SIZE_MAX;
		size_t rowSourceRow = SIZE_MAX;
		std::uint64_t transientGeneration = 0;
	};

	/// Callbacks for the shared table renderer, abstracting manager operations.
	struct TableCallbacks
	{
		std::function<void(size_t idx, float width, bool readOnly)> drawEditor;
		std::function<void(const std::vector<size_t>& indices, float width, bool readOnly)> drawEditorMulti;
		std::function<void(size_t idx)> togglePause;
		std::function<void(size_t idx)> revert;
		std::function<void(size_t idx)> remove;
		// Optional: called when user clicks + in an empty period cell (multi-column only)
		std::function<void(const std::string& feature, const std::vector<std::string>& path, const std::string& key, int period)> onAddPeriod;
		const char* auxiliaryColumnLabel = nullptr;
		std::function<void(const std::vector<size_t>& indices, float width, bool readOnly)> drawAuxiliary;
	};

	/// Draw flyout controls (toggle + revert + delete). Works for both single and group.
	FlyoutResult DrawFlyoutControls(bool paused, bool isGroup, bool isOverwrite);

	void DrawValueEditor(SceneType type, size_t index, float inputWidth, bool readOnly = false);
	void DrawWeatherValueEditor(RE::FormID weatherId, size_t index, float inputWidth, bool readOnly = false);
	void DrawWeatherValueEditor(RE::FormID weatherId, const std::vector<size_t>& indices, float inputWidth, bool readOnly = false);
	void DrawPopups(SceneType type, PopupState& popups);

	bool DrawSectionHeader(const char* label, const char* idSuffix,
		bool allPaused, std::function<void()> onTogglePause, std::function<void()> onDeleteAll,
		const SourceTableLayout& layout, std::function<void()> onExportAll = nullptr,
		bool hasActiveOverrides = false);

	/// State for the export-to-overwrites selection popup.
	struct ExportAllPopupState
	{
		static constexpr size_t kModNameBufferSize = 128;

		bool dialogOpen = false;
		std::vector<size_t> userIndices;
		std::vector<uint8_t> selected;
		std::vector<std::vector<size_t>> flatGroups;
		bool flatGroupsCached = false;
		char modName[kModNameBufferSize] = "";
		std::optional<SceneSettingsManager::OverwriteExportResult> exportResult;

		void Open(const std::vector<size_t>& indices)
		{
			dialogOpen = true;
			userIndices = indices;
			selected.assign(indices.size(), 1);
			flatGroups.clear();
			flatGroupsCached = false;
			exportResult.reset();
		}
	};

	void DrawExportAllPopup(SceneType type, const std::vector<SceneSettingsManager::SettingEntry>& entries, ExportAllPopupState& state);
	void DrawWeatherExportAllPopup(RE::FormID weatherId, const std::vector<SceneSettingsManager::SettingEntry>& entries, ExportAllPopupState& state, bool showTod);

	/// Draw a source table with feature-grouped rows and per-cell value editing.
	/// When layout.numValueColumns is 1, row actions are drawn in a fixed right-side column.
	void DrawSourceTable(
		const SourceGroup& group,
		const std::vector<SceneSettingsManager::SettingEntry>& entries,
		const char* tableId,
		EntrySource source,
		const SourceTableLayout& layout,
		PopupState* popups,
		TableFlyoutState& flyout,
		const TableCallbacks& cb,
		std::span<const uint8_t> overriddenEntries = {});

	// --- Consolidated Panel Functions ---

	/// Draw page-wide Scene Manager actions before the tab bar.
	void DrawGlobalActions(std::optional<SceneSettingsManager::SceneContextType> scope = std::nullopt);

	/// Draw the full interior settings panel.
	void DrawInteriorPanel();

	/// Draw the full Time of Day settings panel.
	void DrawTimeOfDayPanel();

	/// Draw the weather settings panel and target browser.
	void DrawWeatherPanel();

	/// Draw scene settings for one weather record.
	void DrawWeatherScenePanel(RE::FormID weatherId);

	/// Draw settings for the current location hierarchy or cell.
	void DrawLocationPanel();

	/// Draw the standalone scene-context copy workflow.
	void DrawCopyPanel();

	/// Return whether a feature exposes at least one setting supported by Scene Manager.
	bool CanEditFeaturePage(Feature* feature);

	/// Start editing scene settings through a feature's native controls.
	bool BeginFeaturePageEditing(Feature* feature);

	/// Return whether the feature-page Scene Manager toolbar is open for this feature.
	bool IsFeaturePageEditing(Feature* feature);

	/// Draw the active feature-page scene toolbar and return whether the page is editing.
	bool DrawFeaturePageControls(Feature* feature, bool enabled);

	/// Play or stop the selected toolbar scene's shared weather/time lock without saving settings.
	bool SetFeaturePagePreviewPlaying(bool playing);

	/// Hide the toolbar without saving, discarding, or stopping its scene preview.
	void HideFeaturePageEditing();
}
