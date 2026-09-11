#include "SceneSettingsUI.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>

#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include "../Feature.h"
#include "../Globals.h"
#include "../I18n/I18n.h"
#include "../Menu.h"
#include "../Menu/ThemeManager.h"
#include "../SceneSettingsManager.h"
#include "../Utils/FileSystem.h"
#include "../Utils/Form.h"
#include "../Utils/Format.h"
#include "../Utils/Game.h"
#include "../Utils/StringUtils.h"

namespace SceneSettingsUI
{
	using C = ThemeManager::Constants;
	constexpr int kNoSubFeatureSelection = -1;
	constexpr int kPeriodlessEntrySlot = 0;
	constexpr float kSingleValueColumnScale = 1.25f;
	constexpr float kLabelOverflowTolerance = 0.5f;
	constexpr float kSceneFloatDragSpeed = 0.01f;
	constexpr float kSceneIntDragSpeed = 1.0f;
	constexpr float kActionsColumnMinWidthEm = 4.5f;
	constexpr float kActionControlSpacingCount = 2.0f;
	constexpr float kCompactToggleWidthScale = 1.6f;
	constexpr float kCompactToggleHeightScale = 0.8f;
	constexpr float kSceneFlyoutRounding = 4.0f;
	constexpr float kSceneFlyoutPaddingX = 6.0f;
	constexpr float kSceneFlyoutPaddingY = 2.0f;
	constexpr float kSceneFlyoutBackgroundAlpha = 0.95f;
	constexpr float kChannelSelectorWidthEm = 4.0f;
	constexpr float kStringEditorMaxWidthEm = 16.0f;
	constexpr float kLocationTransitionColumnWidthEm = 5.0f;
	constexpr float kCopyListHeightEm = 14.0f;
	constexpr size_t kCopySettingVisibleRows = 6;
	constexpr int kSceneTargetComboVisibleItems = 12;
	constexpr float kTableBorderWidth = 1.0f;
	constexpr const char* kEllipsis = "...";
	constexpr const char* kNumericWidthSample = "-000.000";
	constexpr std::string_view kDisplaySeparator = " / ";
	static int s_addDialogFrame = -1;
	static AddSettingState* s_activeAddDialog = nullptr;
	static std::uint64_t s_transientGeneration = 1;
	using SettingEntry = SceneSettingsManager::SettingEntry;

	static size_t PreviousUtf8CodepointBoundary(std::string_view text, size_t offset)
	{
		if (offset == 0)
			return 0;

		--offset;
		while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80)
			--offset;
		return offset;
	}

	static std::string_view GetVisibleLabel(std::string_view label)
	{
		return label.substr(0, label.find("##"));
	}

	static void ContinueButtonRowIfFits(const char* nextLabel)
	{
		const float nextWidth = ImGui::CalcTextSize(nextLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		const float contentRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
		if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + nextWidth <= contentRight)
			ImGui::SameLine();
	}

	static ImVec2 GetCompactFeatureToggleSize()
	{
		const float frameHeight = ImGui::GetFrameHeight();
		return ImVec2(
			frameHeight * kCompactToggleWidthScale * C::FLYOUT_TOGGLE_SCALE,
			frameHeight * kCompactToggleHeightScale * C::FLYOUT_TOGGLE_SCALE);
	}

	static float GetSceneActionButtonSize()
	{
		return ImGui::GetFrameHeight() * C::FLYOUT_BUTTON_SCALE;
	}

	static bool DrawSceneAddButton(const char* id, float size)
	{
		const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
		const auto minimum = ImGui::GetItemRectMin();
		const auto maximum = ImGui::GetItemRectMax();
		const bool held = ImGui::IsItemActive() || pressed;
		const bool hovered = ImGui::IsItemHovered();
		const auto color = ImGui::GetColorU32(
			held ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered :
													 ImGuiCol_Button);
		ImGui::RenderFrame(minimum, maximum, color, true, ImGui::GetStyle().FrameRounding);

		const float halfExtent = std::max(2.0f, std::floor(size * 0.18f));
		const float stroke = std::max(1.0f, std::floor(size * 0.08f));
		const float halfStroke = stroke * 0.5f;
		const float outerHalfExtent = halfExtent + halfStroke;
		const ImVec2 rawCenter(
			(minimum.x + maximum.x) * 0.5f,
			(minimum.y + maximum.y) * 0.5f);
		const bool oddPixelStroke = static_cast<int>(stroke) % 2 != 0;
		const auto snapCenter = [oddPixelStroke](float value) {
			return oddPixelStroke ? std::floor(value) + 0.5f : std::round(value);
		};
		const ImVec2 center(snapCenter(rawCenter.x), snapCenter(rawCenter.y));
		auto* drawList = ImGui::GetWindowDrawList();
		const auto glyphColor = ImGui::GetColorU32(ImGuiCol_Text);
		drawList->AddRectFilled(
			ImVec2(center.x - outerHalfExtent, center.y - halfStroke),
			ImVec2(center.x + outerHalfExtent, center.y + halfStroke), glyphColor);
		drawList->AddRectFilled(
			ImVec2(center.x - halfStroke, center.y - outerHalfExtent),
			ImVec2(center.x + halfStroke, center.y + outerHalfExtent), glyphColor);
		return pressed;
	}

	static Util::FlyoutStyle GetSceneFlyoutStyle()
	{
		const float scale = Util::GetUIScale();
		return {
			{ kSceneFlyoutPaddingX * scale, kSceneFlyoutPaddingY * scale },
			kSceneFlyoutRounding * scale,
			kSceneFlyoutBackgroundAlpha,
			1.0f,
			0.0f,
			true,
			true
		};
	}

	static bool DrawSceneIconButton(const char* id, void* texture, const ImVec2& size, float padding)
	{
		if (!texture)
			return false;
		const ImVec2 imageSize(
			std::max(1.0f, size.x - padding * 2.0f),
			std::max(1.0f, size.y - padding * 2.0f));
		auto& colors = ImGui::GetStyle().Colors;
		ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_FrameBg]);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_FrameBgHovered]);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[ImGuiCol_FrameBgActive]);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding, padding));
		const bool clicked = ImGui::ImageButton(
			id, texture, imageSize, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), Util::GetIconTint());
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
		return clicked;
	}

	static bool DrawSceneDeleteButton(const char* label, float size)
	{
		auto& colors = ImGui::GetStyle().Colors;
		ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_FrameBg]);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_FrameBgHovered]);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[ImGuiCol_FrameBgActive]);
		const float paddingY = std::max(0.0f, (size - ImGui::GetFontSize()) * 0.5f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, size * 0.3f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(paddingY, paddingY));
		const bool clicked = ImGui::Button(label, ImVec2(size, size));
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(3);
		return clicked;
	}

	static bool IsTransitionEntry(const SettingEntry& entry)
	{
		return entry.value.is_number_float();
	}

	static const char* GetPeriodDisplayName(Period period)
	{
		switch (period) {
		case Period::Dawn:
			return T("feature.scene_manager.period.dawn", "Dawn");
		case Period::Sunrise:
			return T("feature.scene_manager.period.sunrise", "Sunrise");
		case Period::Day:
			return T("feature.scene_manager.period.day", "Day");
		case Period::Sunset:
			return T("feature.scene_manager.period.sunset", "Sunset");
		case Period::Dusk:
			return T("feature.scene_manager.period.dusk", "Dusk");
		case Period::Night:
			return T("feature.scene_manager.period.night", "Night");
		case Period::Count:
			return T("feature.scene_manager.channel.all", "All");
		default:
			return "";
		}
	}

	static std::string GetAddPeriodLabel(Period period)
	{
		const auto* periodName = GetPeriodDisplayName(period);
		return std::vformat(T("feature.scene_manager.action.add_period", "Add {0}"),
			std::make_format_args(periodName));
	}

	static std::string GetEntryDisplayName(const SettingEntry& entry)
	{
		return entry.displayName.empty() ? SceneSettingsManager::GetSettingDisplayName(entry.settingKey) : entry.displayName;
	}

	static std::vector<std::string> SplitDisplayName(std::string_view displayName)
	{
		std::vector<std::string> parts;
		if (displayName.empty())
			return parts;
		for (size_t start = 0; start <= displayName.size();) {
			size_t end = displayName.find(kDisplaySeparator, start);
			if (end == std::string_view::npos) {
				parts.emplace_back(displayName.substr(start));
				break;
			}
			parts.emplace_back(displayName.substr(start, end - start));
			start = end + kDisplaySeparator.size();
		}
		return parts;
	}

	static std::string JoinDisplayParts(const std::vector<std::string>& parts)
	{
		std::string result;
		for (const auto& part : parts) {
			if (!result.empty())
				result += kDisplaySeparator;
			result += part;
		}
		return result;
	}

	static std::vector<std::string> GetTableParentPath(const std::vector<std::string>& displayPath)
	{
		if (displayPath.empty())
			return {};

		auto first = std::prev(displayPath.end());
		const bool indexedChild = !first->empty() &&
		                          std::ranges::all_of(*first, [](const char ch) { return ch >= '0' && ch <= '9'; });
		if (indexedChild && first != displayPath.begin())
			--first;
		return std::vector<std::string>(first, displayPath.end());
	}

	static SettingId MakeSettingId(const SettingEntry& entry, const std::string& rootCategoryName)
	{
		SceneSettingsManager::SettingControlInfo info;
		if (SceneSettingsManager::GetSettingControlInfo(entry, info)) {
			auto settingName = info.displayName;
			if (info.controlType != SceneSettingControlType::Scalar &&
				!info.componentDisplayName.empty())
				settingName += std::format(" ({})", info.componentDisplayName);
			auto parentPath = GetTableParentPath(info.tableDisplayPath);
			return { entry.featureShortName, entry.settingPath, entry.settingKey,
				std::move(settingName), rootCategoryName, std::move(parentPath), -1 };
		}

		auto displayParts = SplitDisplayName(GetEntryDisplayName(entry));
		if (displayParts.empty())
			displayParts.push_back(SceneSettingsManager::GetSettingDisplayName(entry.settingKey));

		auto settingName = displayParts.back();
		displayParts.pop_back();

		auto parentPath = GetTableParentPath(displayParts);
		return { entry.featureShortName, entry.settingPath, entry.settingKey,
			std::move(settingName), rootCategoryName, std::move(parentPath), -1 };
	}

	static SettingId MakeControlSettingId(const SettingEntry& entry,
		const SceneSettingsManager::SettingControlInfo& info, const std::string& rootCategoryName)
	{
		auto parentPath = GetTableParentPath(info.tableDisplayPath);
		return {
			entry.featureShortName, info.settingPath, info.settingKey, info.displayName,
			rootCategoryName, std::move(parentPath), info.componentStart
		};
	}

	SourceGroup BuildSourceGroup(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		EntrySource sourceFilter, bool filterBySource, bool transitionOnly, bool multiColumn, std::optional<bool> periodMode)
	{
		SourceGroup group;
		std::vector<SettingId> order;
		std::map<SettingId, std::array<size_t, kPeriodCount>> entryMap;
		std::map<SettingId, std::array<std::vector<size_t>, kPeriodCount>> memberMap;
		std::map<std::string, std::string> featureDisplayNames;
		auto getFeatureDisplayName = [&](const std::string& feature) -> const std::string& {
			auto it = featureDisplayNames.find(feature);
			if (it == featureDisplayNames.end())
				it = featureDisplayNames.emplace(feature, SceneSettingsManager::GetFeatureDisplayName(feature)).first;
			return it->second;
		};

		using AggregateKey = std::tuple<
			std::string, std::vector<std::string>, std::string, std::int8_t, std::uint8_t,
			SceneSettingControlType, EntrySource, int>;
		struct AggregateCandidate
		{
			SceneSettingsManager::SettingControlInfo info;
			std::map<std::int8_t, std::vector<size_t>> components;
			bool valid = true;
		};
		std::map<AggregateKey, AggregateCandidate> candidates;
		for (size_t idx = 0; idx < entries.size(); ++idx) {
			const auto& entry = entries[idx];
			if (filterBySource && entry.source != sourceFilter)
				continue;
			if (periodMode && (entry.period != Period::Count) != *periodMode)
				continue;
			if (transitionOnly && !IsTransitionEntry(entry))
				continue;
			int period = static_cast<int>(entry.period);
			if (period < 0)
				continue;
			if (period >= kPeriodCount)
				period = kPeriodlessEntrySlot;
			SceneSettingsManager::SettingControlInfo info;
			if (!SceneSettingsManager::GetSettingControlInfo(entry, info) ||
				info.controlType == SceneSettingControlType::Scalar || info.componentCount < 2)
				continue;
			if (multiColumn &&
				info.aggregatePresentation == SceneSettingsManager::AggregatePresentation::Components &&
				info.unifiedEditMode == SceneSettingsManager::UnifiedEditMode::None)
				continue;
			auto [candidateIt, inserted] = candidates.try_emplace(AggregateKey{
				entry.featureShortName, info.settingPath, info.settingKey,
				info.componentStart, info.componentCount, info.controlType, entry.source, period });
			auto& candidate = candidateIt->second;
			if (inserted) {
				candidate.info = info;
			} else if (candidate.info.displayName != info.displayName ||
					   candidate.info.displayPath != info.displayPath ||
					   candidate.info.tableDisplayPath != info.tableDisplayPath ||
					   candidate.info.aggregatePresentation != info.aggregatePresentation ||
					   candidate.info.unifiedEditMode != info.unifiedEditMode) {
				candidate.valid = false;
			}
			candidate.components[info.componentIndex].push_back(idx);
		}

		std::map<size_t, SettingId> aggregateSettings;
		for (const auto& [candidateKey, candidate] : candidates) {
			(void)candidateKey;
			bool complete = candidate.valid && candidate.components.size() == candidate.info.componentCount;
			for (size_t component = 0; complete && component < candidate.info.componentCount; ++component)
				complete = candidate.components.contains(
					static_cast<std::int8_t>(candidate.info.componentStart + component));
			std::optional<bool> paused;
			for (const auto& [componentIndex, indices] : candidate.components) {
				(void)componentIndex;
				for (const auto index : indices) {
					if (!paused)
						paused = entries[index].paused;
					else if (*paused != entries[index].paused)
						complete = false;
				}
			}
			if (!complete)
				continue;
			const auto firstIndex = candidate.components.begin()->second.front();
			const auto setting = MakeControlSettingId(
				entries[firstIndex], candidate.info, getFeatureDisplayName(entries[firstIndex].featureShortName));
			for (const auto& [componentIndex, indices] : candidate.components) {
				(void)componentIndex;
				for (const auto index : indices)
					aggregateSettings.emplace(index, setting);
			}
		}

		for (size_t idx = 0; idx < entries.size(); ++idx) {
			const auto& e = entries[idx];
			if (filterBySource && e.source != sourceFilter)
				continue;
			if (periodMode && (e.period != Period::Count) != *periodMode)
				continue;
			if (transitionOnly && !IsTransitionEntry(e))
				continue;
			int p = static_cast<int>(e.period);
			if (p < 0)
				continue;
			if (p >= kPeriodCount)
				p = kPeriodlessEntrySlot;
			auto aggregate = aggregateSettings.find(idx);
			SettingId setting = aggregate != aggregateSettings.end() ?
			                        aggregate->second :
			                        MakeSettingId(e, getFeatureDisplayName(e.featureShortName));
			auto [it, inserted] = entryMap.try_emplace(setting);
			if (inserted) {
				it->second.fill(SIZE_MAX);
				order.push_back(setting);
			}
			if (it->second[p] == SIZE_MAX)
				it->second[p] = idx;
			memberMap[setting][p].push_back(idx);
		}
		std::sort(order.begin(), order.end());
		group.rows.reserve(order.size());
		for (auto& setting : order) {
			SourceRow row;
			row.setting = std::move(setting);
			const auto& perKey = entryMap.at(row.setting);
			auto members = memberMap.find(row.setting);
			for (int period = 0; period < kPeriodCount; ++period) {
				if (members != memberMap.end() && !members->second[period].empty())
					row.cells[period] = std::move(members->second[period]);
				else if (perKey[period] != SIZE_MAX)
					row.cells[period].push_back(perKey[period]);
				if (!row.cells[period].empty() && row.addPeriodSourceIndices.empty())
					row.addPeriodSourceIndices = row.cells[period];
				row.indices.insert(row.indices.end(), row.cells[period].begin(), row.cells[period].end());
				group.perColumn[period].insert(
					group.perColumn[period].end(), row.cells[period].begin(), row.cells[period].end());
			}
			if (group.categories.empty() ||
				group.rows[group.categories.back().begin].setting.feature != row.setting.feature ||
				group.rows[group.categories.back().begin].setting.categoryName != row.setting.categoryName) {
				if (!group.categories.empty())
					group.categories.back().end = group.rows.size();
				group.categories.push_back({ group.rows.size(), group.rows.size() });
			}
			group.rows.push_back(std::move(row));
		}
		if (!group.categories.empty())
			group.categories.back().end = group.rows.size();
		return group;
	}

	void SplitBySource(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		std::vector<size_t>& overwriteOut, std::vector<size_t>& userOut, bool transitionOnly, std::optional<bool> periodMode)
	{
		for (size_t i = 0; i < entries.size(); ++i) {
			if (periodMode && (entries[i].period != Period::Count) != *periodMode)
				continue;
			if (transitionOnly && !IsTransitionEntry(entries[i]))
				continue;
			(entries[i].source == EntrySource::Overwrite ? overwriteOut : userOut).push_back(i);
		}
	}

	void RemoveIndicesReversed(const std::vector<size_t>& indices, std::function<void(size_t)> removeFn)
	{
		auto sorted = indices;
		std::sort(sorted.begin(), sorted.end(), std::greater<>());
		for (auto idx : sorted)
			removeFn(idx);
	}

	using OverrideKey = std::tuple<std::string, std::vector<std::string>, std::string, int>;

	static OverrideKey MakeOverrideKey(const SettingEntry& entry)
	{
		return { entry.featureShortName, entry.settingPath, entry.settingKey, static_cast<int>(entry.period) };
	}

	static std::set<OverrideKey> BuildActiveOverrideSet(const std::vector<SettingEntry>& entries)
	{
		std::set<OverrideKey> overrides;
		for (const auto& entry : entries)
			if (entry.source == EntrySource::Overwrite && !entry.paused)
				overrides.insert(MakeOverrideKey(entry));
		return overrides;
	}

	static std::set<OverrideKey> BuildUserEntrySet(const std::vector<SettingEntry>& entries)
	{
		std::set<OverrideKey> userEntries;
		for (const auto& entry : entries)
			if (entry.source == EntrySource::User)
				userEntries.insert(MakeOverrideKey(entry));
		return userEntries;
	}

	static bool IsOverridden(const std::set<OverrideKey>& overrides, const SettingEntry& entry)
	{
		return !overrides.empty() && overrides.contains(MakeOverrideKey(entry));
	}

	static bool AreAllPaused(const std::vector<size_t>& indices, const std::vector<SettingEntry>& entries)
	{
		return std::all_of(indices.begin(), indices.end(),
			[&](size_t idx) { return idx < entries.size() && entries[idx].paused; });
	}

	/// Request a confirmation popup for deleting overwrite entries by indices.
	static void RequestOverwriteRowDelete(PopupState& popups,
		const std::vector<SceneSettingsManager::SettingEntry>& entries,
		const std::vector<size_t>& indices)
	{
		std::set<std::string> filenames;
		for (auto idx : indices)
			if (idx < entries.size())
				filenames.insert(entries[idx].sourceFilename);
		std::string fileList;
		for (const auto& f : filenames) {
			if (!fileList.empty())
				fileList += ", ";
			fileList += "'" + f + "'";
		}
		popups.pendingDeleteRow = indices;
		popups.deleteRowOverwrite.message = std::vformat(
			T("feature.scene_manager.confirm.delete_overwrite_entries",
				"Delete overwrite entries from {0}?\nEmpty overwrite files will be removed from disk."),
			std::make_format_args(fileList));
		popups.deleteRowOverwrite.Request();
	}

	struct FlyoutSource
	{
		ImGuiID id;
		bool pressed;
	};

	static FlyoutSource SubmitFlyoutSource(const char* id, const ImVec2& minimum, const ImVec2& maximum)
	{
		auto* window = ImGui::GetCurrentWindow();
		const ImGuiID itemId = window->GetID(id);
		const ImRect bounds(minimum,
			ImVec2(minimum.x + std::max(1.0f, maximum.x - minimum.x),
				minimum.y + std::max(1.0f, maximum.y - minimum.y)));
		bool pressed = false;
		if (ImGui::ItemAdd(bounds, itemId, nullptr, ImGuiItemFlags_NoNav)) {
			bool hovered = false;
			bool held = false;
			pressed = ImGui::ButtonBehavior(bounds, itemId, &hovered, &held);
		}
		return { itemId, pressed };
	}

	static void ApplyGroupControlResult(const FlyoutResult& result, const std::vector<size_t>& indices,
		bool allPaused, bool isOverwrite, PopupState* popups, const std::vector<SettingEntry>& entries,
		const TableCallbacks& cb, Util::FlyoutState* flyout)
	{
		if (result.toggled)
			for (auto idx : indices)
				if (idx < entries.size() && entries[idx].paused == allPaused)
					cb.togglePause(idx);
		if (result.reverted)
			for (auto idx : indices)
				cb.revert(idx);
		if (!result.deleted)
			return;

		if (popups && isOverwrite) {
			RequestOverwriteRowDelete(*popups, entries, indices);
			if (flyout)
				Util::RequestCloseFlyout(*flyout);
		} else {
			RemoveIndicesReversed(indices, cb.remove);
			if (flyout)
				Util::CloseFlyout(*flyout);
		}
	}

	static std::vector<std::string> GetFeatureNamesForType(SceneType type)
	{
		switch (type) {
		case SceneType::InteriorOnly:
			return SceneSettingsManager::GetInteriorRelevantFeatureNames();
		case SceneType::Location:
			return SceneSettingsManager::GetLocationRelevantFeatureNames();
		default:
			return SceneSettingsManager::GetExteriorRelevantFeatureNames();
		}
	}

	static std::string GetDescriptorDisplayName(const SceneSettingDescriptor& descriptor)
	{
		return descriptor.displayName.empty() ? SceneSettingsManager::GetSettingDisplayName(descriptor.key) : descriptor.displayName;
	}

	static void RebuildSettingTree(AddSettingState& state)
	{
		state.settingTree = {};
		state.selectedSubFeaturePath.clear();

		for (size_t i = 0; i < state.cachedSettings.size(); ++i) {
			auto* node = &state.settingTree;
			for (const auto& part : state.cachedSettings[i].displayPath)
				node = &node->children[part];
			node->settings.push_back(i);
		}
	}

	static int GetStoredAllMemberIndex(const SceneSettingDescriptor& descriptor)
	{
		for (size_t index = 0; index < descriptor.members.size(); ++index)
			if (descriptor.members[index].aggregateAll)
				return static_cast<int>(index);
		return -1;
	}

	static bool CanSelectSyntheticAll(const SceneSettingDescriptor& descriptor, bool shiftHeld)
	{
		return descriptor.aggregatePresentation == SceneSettingsManager::AggregatePresentation::ColorPicker ||
		       descriptor.unifiedEditMode == SceneSettingsManager::UnifiedEditMode::Always ||
		       (descriptor.unifiedEditMode == SceneSettingsManager::UnifiedEditMode::Shift && shiftHeld);
	}

	static void CacheSettings(AddSettingState& state, std::vector<SceneSettingDescriptor> settings)
	{
		state.cachedSettings = std::move(settings);
		state.selectedSettings.assign(state.cachedSettings.size(), false);
		state.selectedMembers.assign(state.cachedSettings.size(), -1);
		state.cachedAddedMembers.clear();
		state.addedMembersCached = false;
		state.cachedAddedRevision = 0;
		for (size_t index = 0; index < state.cachedSettings.size(); ++index) {
			const auto& descriptor = state.cachedSettings[index];
			const auto storedAll = GetStoredAllMemberIndex(descriptor);
			if (storedAll >= 0)
				state.selectedMembers[index] = storedAll;
			else if (!CanSelectSyntheticAll(descriptor, false) && !descriptor.members.empty())
				state.selectedMembers[index] = 0;
		}
		RebuildSettingTree(state);
	}

	static void RefreshCachedSettingValues(
		AddSettingState& state, const std::vector<SceneSettingDescriptor>& refreshedSettings)
	{
		for (auto& descriptor : state.cachedSettings) {
			auto refreshedDescriptor = std::ranges::find_if(refreshedSettings, [&](const auto& candidate) {
				return candidate.settingPath == descriptor.settingPath && candidate.key == descriptor.key &&
				       candidate.controlType == descriptor.controlType;
			});
			if (refreshedDescriptor == refreshedSettings.end())
				continue;

			descriptor.value = refreshedDescriptor->value;
			for (auto& member : descriptor.members) {
				auto refreshedMember = std::ranges::find_if(
					refreshedDescriptor->members, [&](const auto& candidate) {
						return candidate.settingPath == member.settingPath && candidate.key == member.key &&
					           candidate.componentIndex == member.componentIndex;
					});
				if (refreshedMember != refreshedDescriptor->members.end())
					member.value = refreshedMember->value;
			}
		}
	}

	static std::string GetDescriptorMemberName(const SceneSettingDescriptor& descriptor, size_t memberIndex)
	{
		if (memberIndex >= descriptor.members.size())
			return {};
		const auto& name = descriptor.members[memberIndex].componentDisplayName;
		return name.empty() ? std::to_string(memberIndex + 1) : name;
	}

	template <class Callback>
	static void ForEachSelectedDescriptorMember(
		const SceneSettingDescriptor& descriptor, int selectedMember, Callback&& callback)
	{
		if (selectedMember >= 0 && selectedMember < static_cast<int>(descriptor.members.size())) {
			callback(descriptor.members[selectedMember]);
			return;
		}
		for (const auto& member : descriptor.members)
			callback(member);
	}

	static bool IsValidChildIndex(const AddSettingNode& node, int index)
	{
		return index >= 0 && index < static_cast<int>(node.children.size());
	}

	static const AddSettingNode* GetChildByIndex(const AddSettingNode& node, int index)
	{
		if (!IsValidChildIndex(node, index))
			return nullptr;
		auto it = node.children.begin();
		std::advance(it, index);
		return &it->second;
	}

	static bool DrawSubFeatureBackButton(size_t level)
	{
		auto* menu = globals::menu;
		float buttonSize = ImGui::GetFrameHeight();
		float iconPadding = buttonSize * C::FLYOUT_REVERT_PAD_SCALE;
		ImGui::PushID(static_cast<int>(level));
		bool clicked = menu && menu->uiIcons.undo.texture ?
		                   DrawSceneIconButton("##SubFeatureBack", menu->uiIcons.undo.texture,
							   ImVec2(buttonSize, buttonSize), iconPadding) :
		                   ImGui::ArrowButton("##SubFeatureBack", ImGuiDir_Left);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("feature.scene_manager.action.show_parent_settings", "Show parent settings"));
		ImGui::PopID();
		return clicked;
	}

	static const AddSettingNode* DrawSubFeatureSelectors(AddSettingState& state)
	{
		const auto* node = &state.settingTree;
		for (size_t level = 0; !node->children.empty(); ++level) {
			int selectedIdx = level < state.selectedSubFeaturePath.size() ?
			                      state.selectedSubFeaturePath[level] :
			                      kNoSubFeatureSelection;
			auto selectedIt = node->children.begin();
			bool hasSelection = IsValidChildIndex(*node, selectedIdx);
			if (hasSelection)
				std::advance(selectedIt, selectedIdx);
			auto label = hasSelection ? selectedIt->first :
			                            std::string(T("feature.scene_manager.select_subfeature", "Select Sub Feature..."));

			ImGui::Spacing();
			bool canGoBack = hasSelection;
			float width = ImGui::GetContentRegionAvail().x;
			if (canGoBack)
				width = std::max(ImGui::GetFrameHeight(), width - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetNextItemWidth(width);
			ImGui::PushID(static_cast<int>(level));
			if (Util::BeginSearchableCombo("##SubFeatureSelect", label.c_str())) {
				int i = 0;
				for (const auto& [name, _] : node->children) {
					if (!Util::SearchableComboMatches(name)) {
						++i;
						continue;
					}
					if (ImGui::Selectable(name.c_str(), i == selectedIdx)) {
						if (state.selectedSubFeaturePath.size() <= level)
							state.selectedSubFeaturePath.resize(level + 1, kNoSubFeatureSelection);
						state.selectedSubFeaturePath[level] = i;
						state.selectedSubFeaturePath.resize(level + 1);
						selectedIdx = i;
						hasSelection = true;
					}
					if (i == selectedIdx)
						ImGui::SetItemDefaultFocus();
					++i;
				}
				Util::EndSearchableCombo();
			}
			ImGui::PopID();

			if (canGoBack) {
				ImGui::SameLine();
				if (DrawSubFeatureBackButton(level)) {
					state.selectedSubFeaturePath.resize(level);
					return node;
				}
			}

			if (!hasSelection)
				return node;

			node = GetChildByIndex(*node, selectedIdx);
			if (!node)
				return nullptr;
		}
		return node;
	}

	template <class Callback>
	static void ForEachSettingIndex(const AddSettingNode& node, Callback&& callback)
	{
		for (auto idx : node.settings)
			callback(idx);
	}

	static bool IsAlreadyAdded(SceneType type, const std::string& feature,
		const std::vector<std::string>& path, const std::string& key, Period period)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		return (type == SceneType::TimeOfDay) ? manager->HasEntryForPeriod(feature, path, key, period, EntrySource::User) : manager->HasEntryFromSource(type, feature, path, key, EntrySource::User);
	}

	static void OpenAddDialogWithFeatures(AddSettingState& state, std::vector<std::string> features)
	{
		if (s_activeAddDialog && s_activeAddDialog != &state)
			s_activeAddDialog->Reset();
		state.Reset();
		state.dialogOpen = true;
		state.measureInitialLayout = true;
		state.cachedFeatureNames = std::move(features);
		s_activeAddDialog = &state;
		++s_transientGeneration;
	}

	void OpenAddDialog(SceneType type, AddSettingState& state)
	{
		OpenAddDialogWithFeatures(state, GetFeatureNamesForType(type));
	}

	void OpenWeatherAddDialog(RE::FormID /*weatherId*/, AddSettingState& state)
	{
		OpenAddDialogWithFeatures(state, SceneSettingsManager::GetExteriorRelevantFeatureNames());
	}

	static std::string GetLocationTargetLabel(
		const SceneSettingsManager::LocationTarget& target);
	static std::string GetWeatherTargetLabel(const RE::TESWeather* weather);

	static std::string GetCopySourceLabel(const SceneSettingsManager::CopySource& source)
	{
		switch (source.context.type) {
		case SceneSettingsManager::SceneContextType::Interior:
			return T("feature.scene_manager.tab.interior", "Interior");
		case SceneSettingsManager::SceneContextType::TimeOfDay:
			return std::format("{} / {}",
				T("feature.scene_manager.tab.time_of_day", "Time of Day"), source.displayName);
		case SceneSettingsManager::SceneContextType::Weather:
			return std::format("{} / {}",
				T("feature.scene_manager.copy.weather", "Weather"), source.displayName);
		case SceneSettingsManager::SceneContextType::Location:
			for (const auto& target : SceneSettingsManager::GetSingleton()->GetLocationManagementTargets())
				if (target.type == source.context.locationType &&
					target.formKey == source.context.locationFormKey)
					return std::format("{} / {}",
						T("feature.scene_manager.tab.locations", "Locations"),
						GetLocationTargetLabel(target));
			return source.displayName;
		default:
			return source.displayName;
		}
	}

	static const char* GetCopySourceTypeLabel(
		std::optional<SceneSettingsManager::SceneContextType> type)
	{
		if (!type)
			return T("feature.scene_manager.channel.all", "All");
		switch (*type) {
		case SceneSettingsManager::SceneContextType::Interior:
			return T("feature.scene_manager.tab.interior", "Interior");
		case SceneSettingsManager::SceneContextType::TimeOfDay:
			return T("feature.scene_manager.tab.time_of_day", "Time of Day");
		case SceneSettingsManager::SceneContextType::Weather:
			return T("feature.scene_manager.copy.weather", "Weather");
		case SceneSettingsManager::SceneContextType::Location:
			return T("feature.scene_manager.location.target_location", "Location");
		default:
			return T("feature.scene_manager.channel.all", "All");
		}
	}

	static void ApplyCopySourceFilter(CopySettingState& state)
	{
		state.sources.clear();
		state.sourceLabels.clear();
		const auto normalizedSearch = state.sourceContextFilter ?
		                                  std::string{} :
		                                  Util::ToLowerAscii(state.sourceSearch);
		for (const auto& source : state.availableSources) {
			if (state.sourceTypeFilter && source.context.type != *state.sourceTypeFilter)
				continue;
			if (state.sourceContextFilter && source.context != *state.sourceContextFilter)
				continue;
			auto label = GetCopySourceLabel(source);
			if (!normalizedSearch.empty() &&
				!Util::ToLowerAscii(label).contains(normalizedSearch))
				continue;
			state.sources.push_back(source);
			state.sourceLabels.push_back(std::vformat(
				T("feature.scene_manager.copy.source_count", "{0} ({1})"),
				std::make_format_args(label, source.settingCount)));
		}

		state.selectedSource = -1;
		const auto preferredSource = state.sourceContextFilter ?
		                                 state.sourceContextFilter :
		                                 state.selectedSourceContext;
		if (preferredSource) {
			for (size_t index = 0; index < state.sources.size(); ++index)
				if (state.sources[index].context == *preferredSource) {
					state.selectedSource = static_cast<int>(index);
					state.selectedSourceContext = *preferredSource;
					break;
				}
		}
		if (state.selectedSource < 0) {
			state.selectedSourceContext.reset();
			state.sourceSettings.clear();
			state.visibleSettingIndices.clear();
			state.selectionSourceContext.reset();
			state.selectedSettings.clear();
		}
	}

	static void RefreshCopySettingFilter(CopySettingState& state)
	{
		const auto normalizedSearch = Util::ToLowerAscii(state.settingSearch);
		state.visibleSettingIndices.clear();
		state.visibleSettingIndices.reserve(state.sourceSettings.size());
		for (size_t index = 0; index < state.sourceSettings.size(); ++index)
			if (normalizedSearch.empty() ||
				Util::ToLowerAscii(state.sourceSettings[index].displayName).contains(normalizedSearch))
				state.visibleSettingIndices.push_back(index);
	}

	static void RefreshCopySourceSettings(CopySettingState& state)
	{
		state.sourceSettings.clear();
		if (state.selectedSource < 0 || state.selectedSource >= static_cast<int>(state.sources.size())) {
			state.visibleSettingIndices.clear();
			state.selectionSourceContext.reset();
			state.selectedSettings.clear();
			return;
		}

		const auto& source = state.sources[state.selectedSource].context;
		if (state.selectionSourceContext != source) {
			state.selectionSourceContext = source;
			state.selectedSettings.clear();
		}
		state.sourceSettings = SceneSettingsManager::GetSingleton()->GetCopySourceSettings(
			source, state.sourceLayer);
		RefreshCopySettingFilter(state);
		std::erase_if(state.selectedSettings, [&](const auto& selectedSetting) {
			return std::ranges::none_of(state.sourceSettings, [&](const auto& sourceSetting) {
				return sourceSetting.setting == selectedSetting;
			});
		});
	}

	static std::vector<SceneSettingsManager::SettingIdentity> GetSelectedCopySettings(
		const CopySettingState& state)
	{
		return { state.selectedSettings.begin(), state.selectedSettings.end() };
	}

	static void DrawCopySourceTypeFilter(CopySettingState& state)
	{
		const std::optional<SceneSettingsManager::SceneContextType> options[] = {
			std::nullopt,
			SceneSettingsManager::SceneContextType::Interior,
			SceneSettingsManager::SceneContextType::TimeOfDay,
			SceneSettingsManager::SceneContextType::Weather,
			SceneSettingsManager::SceneContextType::Location,
		};
		for (const auto option : options) {
			const bool selected = option == state.sourceTypeFilter;
			if (ImGui::RadioButton(GetCopySourceTypeLabel(option), selected)) {
				state.sourceTypeFilter = option;
				state.sourceContextFilter.reset();
				ApplyCopySourceFilter(state);
				RefreshCopySourceSettings(state);
			}
			if (option != options[std::size(options) - 1])
				ImGui::SameLine();
		}
	}

	static void RefreshCopySources(CopySettingState& state)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto revision = manager->GetEntryPresentationRevision();
		const auto locale = I18n::GetSingleton()->GetCurrentLocale();
		if (state.revision == revision && state.locale == locale)
			return;

		const auto sourceTypeFilter = state.sourceTypeFilter;
		const auto sourceContextFilter = state.sourceContextFilter;
		const auto sourceLayer = state.sourceLayer;
		auto sourceSearch = std::move(state.sourceSearch);
		const auto selectedSourceContext = sourceContextFilter ?
		                                       sourceContextFilter :
		                                       state.selectedSourceContext;
		auto settingSearch = std::move(state.settingSearch);
		const auto selectionSourceContext = state.selectionSourceContext;
		auto selectedSettings = std::move(state.selectedSettings);
		state.Reset();
		state.sourceLayer = sourceLayer;
		state.sourceTypeFilter = sourceTypeFilter;
		state.sourceContextFilter = sourceContextFilter;
		state.sourceSearch = std::move(sourceSearch);
		state.selectedSourceContext = selectedSourceContext;
		state.settingSearch = std::move(settingSearch);
		state.selectionSourceContext = selectionSourceContext;
		state.selectedSettings = std::move(selectedSettings);
		state.revision = revision;
		state.locale = locale;
		state.availableSources = manager->GetCopySources(state.sourceLayer);
		ApplyCopySourceFilter(state);
		RefreshCopySourceSettings(state);
	}

	struct FeatureSceneTargetState
	{
		SceneSettingsManager::SceneContextType type =
			SceneSettingsManager::SceneContextType::TimeOfDay;
		Period period = Period::Day;
		RE::FormID weatherId = 0;
		SceneSettingsManager::LocationTargetType locationType =
			SceneSettingsManager::LocationTargetType::Location;
		std::string locationFormKey;

		auto operator<=>(const FeatureSceneTargetState&) const = default;
	};

	struct FeatureLocationPickerEntry
	{
		size_t targetIndex = 0;
		std::string identity;
		std::string displayLabel;
		std::string selectableLabel;
		bool configured = false;
		bool paused = false;
	};

	struct FeatureLocationPickerCache
	{
		static constexpr size_t kInvalidTargetIndex = std::numeric_limits<size_t>::max();

		size_t targetCount = std::numeric_limits<size_t>::max();
		std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
		std::string locale;
		std::vector<FeatureLocationPickerEntry> entries;
		std::map<std::string, size_t> indicesByIdentity;
		std::vector<size_t> configuredIndices;
		std::vector<size_t> locationTypeIndices;
		std::vector<size_t> unconfiguredIndices;
		std::vector<size_t> currentIndices;
		std::vector<std::uint8_t> currentMembership;
		std::vector<size_t> visibleIndices;
		size_t selectedTargetIndex = kInvalidTargetIndex;
	};

	struct CopyDestinationState
	{
		FeatureSceneTargetState target;
		FeatureLocationPickerCache locationPicker;
		std::set<RE::FormID> weatherIds;
		std::string weatherSearch;
		std::set<std::pair<SceneSettingsManager::LocationTargetType, std::string>> locations;
		std::string locationSearch;
		bool timeOfDay = false;
		bool numericSettings = false;
		std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
		std::optional<SceneSettingsManager::SceneContextId> source;
		EntrySource sourceLayer = EntrySource::User;
		std::vector<SceneSettingsManager::SettingIdentity> settings;
		std::vector<SceneSettingsManager::SceneContextType> supportedTypes;
		std::vector<SceneSettingsManager::SceneContextId> destinations;
		size_t compatibleCount = 0;
		size_t maskedCount = 0;
		bool hasConflicts = false;
		SceneSettingsManager::CopyConflictPolicy policy = SceneSettingsManager::CopyConflictPolicy::SkipExisting;
	};

	static void DrawCopyDestinationPopup(const char* id, CopyDestinationState& state,
		const SceneSettingsManager::SceneContextId& source, CopySettingState& copy, bool selectSettings = false);

	struct CopyPanelState

	{
		Period sourcePeriod = Period::Day;
		RE::FormID sourceWeatherId = 0;
		bool sourceTimeOfDay = false;
		CopyDestinationState destination;

		CopySettingState copy;
	};

	static CopyPanelState s_copyPanelState;
	static ImVec2 s_featurePageCenter;

	static const std::vector<RE::TESWeather*>& GetSceneWeatherTargets()
	{
		static RE::TESDataHandler* cachedDataHandler = nullptr;
		static size_t cachedWeatherCount = std::numeric_limits<size_t>::max();
		static std::vector<RE::TESWeather*> targets;
		static std::unordered_set<RE::FormID> targetIds;
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		const auto weatherCount = dataHandler ?
		                              dataHandler->GetFormArray<RE::TESWeather>().size() :
		                              0;
		bool targetsChanged = dataHandler != cachedDataHandler || weatherCount != cachedWeatherCount;
		if (targetsChanged) {
			cachedDataHandler = dataHandler;
			cachedWeatherCount = weatherCount;
			targets.clear();
			targetIds.clear();
			if (dataHandler)
				for (auto* weather : dataHandler->GetFormArray<RE::TESWeather>())
					if (weather && weather->GetFormID() != 0 &&
						targetIds.insert(weather->GetFormID()).second)
						targets.push_back(weather);
		}
		const auto addWeather = [&](RE::TESWeather* weather) {
			if (weather && weather->GetFormID() != 0 &&
				targetIds.insert(weather->GetFormID()).second) {
				targets.push_back(weather);
				targetsChanged = true;
			}
		};
		if (auto* sky = globals::game::sky)
			for (auto* weather : { sky->currentWeather, sky->lastWeather })
				addWeather(weather);
		if (targetsChanged)
			std::ranges::sort(targets, [](const auto* lhs, const auto* rhs) {
				return std::tuple{ GetWeatherTargetLabel(lhs), lhs->GetFormID() } <
				       std::tuple{ GetWeatherTargetLabel(rhs), rhs->GetFormID() };
			});
		return targets;
	}

	static bool DrawSceneModePicker(const char* id, bool& timeOfDay,
		SceneSettingsManager::SceneContextType type, bool numeric = true)
	{
		ImGui::PushID(id);
		const SKSE::stl::scope_exit restoreId([]() noexcept { ImGui::PopID(); });
		bool changed = false;
		if (ImGui::RadioButton(GetCopySourceTypeLabel(type), !timeOfDay)) {
			timeOfDay = false;
			changed = true;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!numeric);
		if (ImGui::RadioButton(T("feature.scene_manager.tab.time_of_day", "Time of Day"), timeOfDay)) {
			timeOfDay = true;
			changed = true;
		}
		ImGui::EndDisabled();
		return changed;
	}

	static ImVec4 GetScenePickerTextColor(bool current, bool paused = false, bool inactive = false)
	{
		if (paused)
			return Util::Colors::GetWarning();
		if (current)
			return Util::Colors::GetSuccess();
		return ImGui::GetStyleColorVec4(inactive ? ImGuiCol_TextDisabled : ImGuiCol_Text);
	}

	static bool IsCurrentLocation(const SceneSettingsManager::LocationTarget& target)
	{
		return std::ranges::any_of(SceneSettingsManager::GetSingleton()->GetCurrentLocationTargets(),
			[&](const auto& current) { return current.type == target.type && current.formKey == target.formKey; });
	}

	enum class SceneSettingMarker
	{
		None,
		Settings,
		Overwrite
	};

	static SceneSettingMarker GetFeatureSceneMarker(std::string_view featureShortName,
		const SceneSettingsManager::SceneContextId& context, bool matchPeriod = true, bool wholeType = false)
	{
		if (featureShortName.empty())
			return SceneSettingMarker::None;
		auto* manager = SceneSettingsManager::GetSingleton();
		static std::string cachedFeature;
		static std::uint64_t cachedRevision = std::numeric_limits<std::uint64_t>::max();
		static std::map<std::tuple<SceneSettingsManager::SceneContextId, bool, bool>, SceneSettingMarker> savedSets;
		const bool featureChanged = cachedFeature != featureShortName;
		if (featureChanged)
			cachedFeature = featureShortName;
		if (featureChanged || cachedRevision != manager->GetEntryPresentationRevision()) {
			cachedRevision = manager->GetEntryPresentationRevision();
			savedSets.clear();
		}
		auto [it, inserted] = savedSets.try_emplace(std::tuple{ context, matchPeriod, wholeType });
		if (inserted) {
			const auto summary = wholeType ? manager->GetFeatureSceneSummary(featureShortName, context.type) :
			                                 manager->GetFeatureSceneSummary(featureShortName, context, matchPeriod);
			it->second = summary.activeOverwrites ? SceneSettingMarker::Overwrite :
			             !summary.Empty()         ? SceneSettingMarker::Settings :
			                                        SceneSettingMarker::None;
		}
		auto marker = it->second;
		if (manager->IsFeatureSceneEditing(featureShortName)) {
			if (manager->HasFeatureSceneEditOverwrites(&context, matchPeriod, wholeType))
				marker = SceneSettingMarker::Overwrite;
			if (marker == SceneSettingMarker::Overwrite && manager->AreFeatureSceneEditOverwritesPaused())
				marker = SceneSettingMarker::Settings;
		}
		return marker;
	}

	struct ScenePickerPresentation
	{
		std::string label;
		bool paused = false;
	};

	static ScenePickerPresentation GetScenePickerPresentation(std::string label,
		const std::vector<SceneSettingsManager::SettingEntry>& entries, bool timeOfDay)
	{
		if (timeOfDay)
			label = std::format("{} ({})", label, T("feature.scene_manager.mode.time_of_day_short", "TOD"));
		return { std::move(label), std::ranges::any_of(entries, [](const auto& entry) { return entry.paused; }) };
	}

	static std::string GetScenePickerLabel(std::string_view label, SceneSettingMarker marker)
	{
		return marker == SceneSettingMarker::Overwrite ? std::format("{} **", label) :
		       marker == SceneSettingMarker::Settings  ? std::format("{} *", label) :
		                                                 std::string(label);
	}

	static void DrawScenePickerText(const std::string& label, ImVec2 position, float right, const ImVec4& color, SceneSettingMarker marker = SceneSettingMarker::None)
	{
		const auto text = GetScenePickerLabel(label, marker);
		const char* end = ImGui::FindRenderedTextEnd(text.c_str());
		const ImVec4 clip(position.x, position.y, right, position.y + ImGui::GetTextLineHeight());
		ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position, ImGui::GetColorU32(color), text.c_str(), end, 0.0f, &clip);
	}

	static bool DrawScenePickerCheckbox(const std::string& label, bool* selected, const ImVec4& color)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		const SKSE::stl::scope_exit restoreColor([]() noexcept { ImGui::PopStyleColor(); });
		return ImGui::Checkbox(std::format("{}###SceneTarget", label).c_str(), selected);
	}

	static const ScenePickerPresentation& GetWeatherPickerPresentation(const RE::TESWeather* weather)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		static std::uint64_t cachedRevision = std::numeric_limits<std::uint64_t>::max();
		static std::string cachedLocale;
		static std::map<RE::FormID, ScenePickerPresentation> labels;
		const auto locale = I18n::GetSingleton()->GetCurrentLocale();
		if (cachedRevision != manager->GetEntryPresentationRevision() || cachedLocale != locale) {
			cachedRevision = manager->GetEntryPresentationRevision();
			cachedLocale = locale;
			labels.clear();
		}
		auto [it, inserted] = labels.try_emplace(weather->GetFormID());
		if (inserted)
			it->second = GetScenePickerPresentation(GetWeatherTargetLabel(weather),
				manager->GetWeatherConfig(weather->GetFormID()).entries, manager->IsWeatherShowTimeOfDay(weather->GetFormID()));
		return it->second;
	}

	static std::string GetWeatherPickerLabel(const RE::TESWeather* weather)
	{
		if (!weather)
			return {};
		return GetWeatherPickerPresentation(weather).label;
	}

	static bool DrawCopyPeriodPicker(const char* id, Period& period, bool allowAll = false, const char* normalOption = nullptr,
		std::string_view featureShortName = {}, const SceneSettingsManager::SceneContextId* sceneContext = nullptr)
	{
		bool changed = false;
		ImGui::SetNextItemWidth(-FLT_MIN);
		const auto current = SceneSettingsManager::GetCurrentPeriod();
		const auto preview = normalOption && period == Period::Count ? normalOption : GetPeriodDisplayName(period);
		const bool featurePicker = sceneContext && !featureShortName.empty();
		const auto hasPeriodSettings = [&](Period candidate) {
			if (featurePicker) {
				auto context = *sceneContext;
				context.period = candidate;
				return GetFeatureSceneMarker(featureShortName, context);
			}
			return SceneSettingMarker::None;
		};
		const auto previewLabel = GetScenePickerLabel(preview, hasPeriodSettings(period));
		const bool open = ImGui::BeginCombo(id, previewLabel.c_str());
		if (open) {
			auto* manager = SceneSettingsManager::GetSingleton();
			static std::uint64_t cachedRevision = std::numeric_limits<std::uint64_t>::max();
			static std::map<SceneSettingsManager::SceneContextId, bool> pausedPeriods;
			if (cachedRevision != manager->GetEntryPresentationRevision()) {
				cachedRevision = manager->GetEntryPresentationRevision();
				pausedPeriods.clear();
			}
			const auto drawPeriod = [&](Period candidate, const char* label) {
				bool paused = false;
				if (sceneContext) {
					auto context = *sceneContext;
					context.period = candidate;
					auto [it, inserted] = pausedPeriods.try_emplace(context);
					if (inserted)
						it->second = manager->GetFeatureSceneSummary({}, context).paused != 0;
					paused = it->second;
				}
				const auto start = ImGui::GetCursorScreenPos();
				const float right = start.x + ImGui::GetContentRegionAvail().x;
				const bool pressed = ImGui::Selectable(std::format("##Period{}", static_cast<int>(candidate)).c_str(), period == candidate);
				DrawScenePickerText(label, start, right, GetScenePickerTextColor(candidate == current, paused), hasPeriodSettings(candidate));
				return pressed;
			};
			if (allowAll) {
				const bool selected = period == Period::Count;
				if (drawPeriod(Period::Count, normalOption ? normalOption : GetPeriodDisplayName(Period::Count))) {
					period = Period::Count;
					changed = true;
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			for (int index = 0; index < kPeriodCount; ++index) {
				const auto candidate = static_cast<Period>(index);
				const bool selected = period == candidate;
				if (drawPeriod(candidate, GetPeriodDisplayName(candidate))) {
					period = candidate;
					changed = true;
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	static void DrawTargetManagementPopup(const SceneSettingsManager::SceneContextId& target, const std::string& label, bool open);

	static bool DrawSceneTargetSelectable(const char* id, const std::string& label, bool selected, const ImVec4& color,
		const SceneSettingsManager::SceneContextId* target = nullptr, SceneSettingMarker marker = SceneSettingMarker::None)
	{
		ImGui::PushID(id);
		const SKSE::stl::scope_exit restoreId([]() noexcept { ImGui::PopID(); });
		const auto start = ImGui::GetCursorScreenPos();
		const float right = start.x + ImGui::GetContentRegionAvail().x;
		const auto* manageLabel = target && target->type == SceneSettingsManager::SceneContextType::Weather ?
		                              T("feature.scene_manager.action.manage_weather", "Manage Weather") :
		                              T("feature.scene_manager.action.manage_location", "Manage Location");
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float buttonWidth = ImGui::CalcTextSize(manageLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		const float buttonX = std::max(start.x, right - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x);
		const bool manageHovered = target && ImGui::IsWindowHovered() &&
		                           ImGui::IsMouseHoveringRect(ImVec2(buttonX, start.y), ImVec2(buttonX + buttonWidth, start.y + ImGui::GetTextLineHeight()));
		const bool pressed = ImGui::Selectable("##SceneTarget", selected || manageHovered,
			ImGuiSelectableFlags_NoAutoClosePopups | (target ? ImGuiSelectableFlags_AllowOverlap : ImGuiSelectableFlags_None));
		if (selected)
			ImGui::SetItemDefaultFocus();
		DrawScenePickerText(label, start, target ? buttonX - spacing : right, color, marker);
		if (target) {
			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(buttonX, start.y));
			const bool manage = ImGui::SmallButton(manageLabel);
			DrawTargetManagementPopup(*target, label, manage);
		}
		if (pressed && !manageHovered)
			ImGui::CloseCurrentPopup();
		return pressed && !manageHovered;
	}

	static bool GetWeatherSelectionTimeOfDay(RE::FormID weatherId)
	{
		const auto& config = SceneSettingsManager::GetSingleton()->GetWeatherConfig(weatherId);
		return config.timeOfDayEnabled || (!config.entries.empty() &&
											  std::ranges::all_of(config.entries, [](const auto& entry) {
												  return entry.period >= Period::Dawn && entry.period < Period::Count;
											  }));
	}

	static bool DrawCopyWeatherPicker(const char* id,
		const std::vector<RE::TESWeather*>& weatherTargets, RE::FormID& weatherId, bool manageTargets = false,
		bool selectDefault = true, std::string_view featureShortName = {})
	{
		const auto previousWeatherId = weatherId;
		auto selectedWeather = std::ranges::find_if(weatherTargets, [&](const auto* weather) {
			return weather->GetFormID() == weatherId;
		});
		if (selectedWeather == weatherTargets.end()) {
			if (selectDefault) {
				if (auto* sky = globals::game::sky; sky && sky->currentWeather)
					selectedWeather = std::ranges::find_if(weatherTargets, [&](const auto* weather) {
						return weather->GetFormID() == sky->currentWeather->GetFormID();
					});
				if (selectedWeather == weatherTargets.end() && !weatherTargets.empty())
					selectedWeather = weatherTargets.begin();
			}
			weatherId = selectedWeather != weatherTargets.end() ?
			                (*selectedWeather)->GetFormID() :
			                0;
		}

		const auto preview = selectedWeather != weatherTargets.end() ?
		                         GetWeatherPickerLabel(*selectedWeather) :
		                         std::string(T("feature.scene_manager.weather.select_target",
									 "Select a weather..."));
		bool changed = weatherId != previousWeatherId;
		ImGui::SetNextItemWidth(-FLT_MIN);
		const auto previewLabel = GetScenePickerLabel(preview, GetFeatureSceneMarker(featureShortName,
																   { .type = SceneSettingsManager::SceneContextType::Weather, .weatherId = weatherId }, false));
		if (Util::BeginSearchableCombo(id, previewLabel.c_str(), ImGuiComboFlags_None,
				nullptr, kSceneTargetComboVisibleItems)) {
			auto* manager = SceneSettingsManager::GetSingleton();
			std::set<RE::FormID> classified;
			std::vector<RE::TESWeather*> currentGroup;
			std::vector<RE::TESWeather*> configuredGroup;
			std::vector<RE::TESWeather*> availableGroup;
			for (auto* weather : weatherTargets) {
				if (manager->GetWeatherConfig(weather->GetFormID()).entries.empty())
					continue;
				configuredGroup.push_back(weather);
				classified.insert(weather->GetFormID());
			}
			if (auto* sky = globals::game::sky) {
				for (auto* current : { sky->currentWeather, sky->lastWeather }) {
					if (!current || classified.contains(current->GetFormID()))
						continue;
					auto candidate = std::ranges::find_if(weatherTargets, [&](const auto* weather) {
						return weather->GetFormID() == current->GetFormID();
					});
					if (candidate == weatherTargets.end())
						continue;
					currentGroup.push_back(*candidate);
					classified.insert(current->GetFormID());
				}
			}
			for (auto* weather : weatherTargets)
				if (!classified.contains(weather->GetFormID()))
					availableGroup.push_back(weather);

			const bool filtering = !Util::GetSearchableComboFilter().empty();
			const bool hasManagementPopup = manageTargets && ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
			const auto drawGroup = [&](const char* groupLabel,
									   const std::vector<RE::TESWeather*>& candidates) {
				std::vector<size_t> visibleIndices;
				visibleIndices.reserve(candidates.size());
				int selectedVisibleIndex = -1;
				int managedVisibleIndex = -1;
				for (size_t index = 0; index < candidates.size(); ++index) {
					const auto* weather = candidates[index];
					const bool managing = hasManagementPopup && ImGui::IsPopupOpen(
																	ImHashStr("##ManageSceneTarget", 0, ImGui::GetID(std::format("CopyWeather{:08X}", weather->GetFormID()).c_str())),
																	ImGuiPopupFlags_AnyPopupLevel);
					if (!managing && filtering && !Util::SearchableComboMatches(GetWeatherPickerLabel(weather)))
						continue;
					if (weather->GetFormID() == weatherId)
						selectedVisibleIndex = static_cast<int>(visibleIndices.size());
					if (managing)
						managedVisibleIndex = static_cast<int>(visibleIndices.size());
					visibleIndices.push_back(index);
				}
				if (visibleIndices.empty())
					return;
				ImGui::SeparatorText(groupLabel);
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(visibleIndices.size()));
				if (selectedVisibleIndex >= 0)
					clipper.IncludeItemByIndex(selectedVisibleIndex);
				if (managedVisibleIndex >= 0)
					clipper.IncludeItemByIndex(managedVisibleIndex);
				while (clipper.Step()) {
					for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
						auto* weather = candidates[visibleIndices[visibleIndex]];
						const auto label = GetWeatherPickerLabel(weather);
						const bool selected = weather->GetFormID() == weatherId;
						const auto itemLabel = std::format("CopyWeather{:08X}", weather->GetFormID());
						const bool canManage = manageTargets && !manager->GetWeatherConfig(weather->GetFormID()).entries.empty();
						const SceneSettingsManager::SceneContextId context{
							.type = SceneSettingsManager::SceneContextType::Weather,
							.weatherId = weather->GetFormID(),
						};
						const bool current = globals::game::sky && globals::game::sky->currentWeather == weather;
						if (DrawSceneTargetSelectable(itemLabel.c_str(), label, selected,
								GetScenePickerTextColor(current, GetWeatherPickerPresentation(weather).paused),
								canManage ? &context : nullptr,
								GetFeatureSceneMarker(featureShortName, context, false))) {
							weatherId = weather->GetFormID();
							changed = true;
						}
					}
				}
			};
			drawGroup(T("feature.scene_manager.weather.group.current", "Current"), currentGroup);
			drawGroup(T("feature.scene_manager.weather.group.configured", "Configured"), configuredGroup);
			drawGroup(T("feature.scene_manager.weather.group.available", "Available"), availableGroup);
			Util::EndSearchableCombo();
		}
		return changed;
	}

	static std::vector<RE::TESWeather*> GetCopySourceWeatherTargets(
		const CopySettingState& state)
	{
		std::set<RE::FormID> sourceWeatherIds;
		for (const auto& source : state.availableSources)
			if (source.context.type == SceneSettingsManager::SceneContextType::Weather)
				sourceWeatherIds.insert(source.context.weatherId);

		auto weatherTargets = GetSceneWeatherTargets();
		std::erase_if(weatherTargets, [&](const auto* weather) {
			return !sourceWeatherIds.contains(weather->GetFormID());
		});
		return weatherTargets;
	}

	static void UpdateCopySourceContextFilter(CopyPanelState& panel)
	{
		auto& state = panel.copy;
		std::optional<SceneSettingsManager::SceneContextId> context;
		if (state.sourceTypeFilter) {
			switch (*state.sourceTypeFilter) {
			case SceneSettingsManager::SceneContextType::Interior:
				context = SceneSettingsManager::SceneContextId{
					.type = SceneSettingsManager::SceneContextType::Interior,
				};
				break;
			case SceneSettingsManager::SceneContextType::TimeOfDay:
				context = SceneSettingsManager::SceneContextId{
					.type = SceneSettingsManager::SceneContextType::TimeOfDay,
					.period = panel.sourcePeriod == Period::Count ? Period::Count : panel.sourcePeriod,
					.allPeriods = panel.sourcePeriod == Period::Count,
				};
				break;
			case SceneSettingsManager::SceneContextType::Weather:
				context = SceneSettingsManager::SceneContextId{
					.type = SceneSettingsManager::SceneContextType::Weather,
					.period = panel.sourceTimeOfDay ? panel.sourcePeriod : Period::Count,
					.allPeriods = panel.sourceTimeOfDay && panel.sourcePeriod == Period::Count,
					.weatherId = panel.sourceWeatherId,
				};
				break;
			case SceneSettingsManager::SceneContextType::Location:
				break;
			}
		}
		if (state.sourceContextFilter == context)
			return;
		state.sourceContextFilter = context;
		state.selectedSourceContext = context;
		ApplyCopySourceFilter(state);
		RefreshCopySourceSettings(state);
	}

	void DrawCopyPanel()
	{
		auto& state = s_copyPanelState;

		const auto selectSourceLayer = [&](EntrySource layer) {
			if (state.copy.sourceLayer == layer)
				return;
			state.copy.sourceLayer = layer;
			state.copy.revision = std::numeric_limits<std::uint64_t>::max();
			state.copy.selectedSourceContext.reset();
			state.copy.selectionSourceContext.reset();
			state.copy.selectedSettings.clear();
		};
		if (ImGui::RadioButton(T("feature.scene_manager.section.user_settings", "User Settings"),
				state.copy.sourceLayer == EntrySource::User))
			selectSourceLayer(EntrySource::User);
		ImGui::SameLine();
		if (ImGui::RadioButton(T("feature.scene_manager.copy.overwrite_settings", "Overwrite Settings"),
				state.copy.sourceLayer == EntrySource::Overwrite))
			selectSourceLayer(EntrySource::Overwrite);
		if (state.copy.sourceLayer == EntrySource::Overwrite)
			ImGui::TextDisabled("%s", T("feature.scene_manager.copy.overwrite_to_user_note",
										  "Copied values are saved as User Settings. Overwrite files are not modified."));

		RefreshCopySources(state.copy);
		const float footerHeight = ImGui::GetFrameHeightWithSpacing();
		const float bodyHeight = std::max(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().y - footerHeight);
		const bool bodyVisible = ImGui::BeginChild("##CopySourceBody", ImVec2(0.0f, bodyHeight));
		if (bodyVisible) {
			DrawCopySourceTypeFilter(state.copy);
			if (ImGui::BeginTable("##CopyFromLayout", 2,
					ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn(T("feature.scene_manager.copy.scene", "Scene"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupColumn(T("feature.scene_manager.column.setting", "Setting"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableHeadersRow();
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);

				if (state.copy.sourceTypeFilter == SceneSettingsManager::SceneContextType::Interior) {
					ImGui::TextUnformatted(T("feature.scene_manager.tab.interior", "Interior"));
				} else if (state.copy.sourceTypeFilter == SceneSettingsManager::SceneContextType::TimeOfDay) {
					ImGui::TextUnformatted(T("feature.scene_manager.tab.time_of_day", "Time of Day"));
					DrawCopyPeriodPicker("##CopySourcePeriod", state.sourcePeriod, true);
				} else if (state.copy.sourceTypeFilter == SceneSettingsManager::SceneContextType::Weather) {
					auto sourceWeatherTargets = GetCopySourceWeatherTargets(state.copy);
					ImGui::TextUnformatted(T("feature.scene_manager.copy.weather", "Weather"));
					if (DrawCopyWeatherPicker("##CopySourceWeather", sourceWeatherTargets, state.sourceWeatherId))
						state.sourceTimeOfDay = GetWeatherSelectionTimeOfDay(state.sourceWeatherId);
					DrawSceneModePicker("##CopySourceMode", state.sourceTimeOfDay, SceneSettingsManager::SceneContextType::Weather);
					if (state.sourceTimeOfDay) {
						ImGui::TextUnformatted(T("feature.scene_manager.tab.time_of_day", "Time of Day"));
						DrawCopyPeriodPicker("##CopySourceWeatherPeriod", state.sourcePeriod, true);
					}
				}
				UpdateCopySourceContextFilter(state);

				const bool sourceHasDedicatedPicker =
					state.copy.sourceTypeFilter == SceneSettingsManager::SceneContextType::Interior ||
					state.copy.sourceTypeFilter == SceneSettingsManager::SceneContextType::TimeOfDay ||
					state.copy.sourceTypeFilter == SceneSettingsManager::SceneContextType::Weather;
				if (sourceHasDedicatedPicker) {
					if (state.copy.sources.empty())
						ImGui::TextDisabled("%s", T("feature.scene_manager.copy.no_sources",
													  "No sources match this filter."));
				} else {
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::InputTextWithHint("##CopySourceSearch", T("ui.search", "Search..."),
							&state.copy.sourceSearch)) {
						ApplyCopySourceFilter(state.copy);
						RefreshCopySourceSettings(state.copy);
					}

					const float copySourceListHeight =
						C::Em(kCopyListHeightEm) + ImGui::GetTextLineHeightWithSpacing();
					if (ImGui::BeginChild("##CopySourceList", ImVec2(-FLT_MIN, copySourceListHeight),
							ImGuiChildFlags_Borders)) {
						if (state.copy.sources.empty())
							ImGui::TextDisabled("%s", T("feature.scene_manager.copy.no_sources",
														  "No sources match this filter."));
						for (size_t index = 0; index < state.copy.sources.size(); ++index) {
							const bool selected = static_cast<int>(index) == state.copy.selectedSource;
							const auto sourceLabel = std::format("  {}", state.copy.sourceLabels[index]);
							ImGui::PushID(static_cast<int>(index));
							if (ImGui::Selectable(sourceLabel.c_str(), selected)) {
								state.copy.selectedSource = static_cast<int>(index);
								state.copy.selectedSourceContext = state.copy.sources[index].context;
								RefreshCopySourceSettings(state.copy);
							}
							ImGui::PopID();
						}
					}
					ImGui::EndChild();
				}

				ImGui::TableSetColumnIndex(1);

				const bool hasSource = state.copy.selectedSource >= 0 &&
				                       state.copy.selectedSource < static_cast<int>(state.copy.sources.size());
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::InputTextWithHint("##CopySettingSearch", T("ui.search", "Search..."),
						&state.copy.settingSearch))
					RefreshCopySettingFilter(state.copy);
				ImGui::BeginDisabled(!hasSource || state.copy.sourceSettings.empty());
				if (ImGui::SmallButton(T("feature.scene_manager.action.select_all", "Select All")))
					for (const auto& sourceSetting : state.copy.sourceSettings)
						state.copy.selectedSettings.insert(sourceSetting.setting);
				ImGui::SameLine();
				if (ImGui::SmallButton(T("feature.scene_manager.action.select_none", "Select None")))
					state.copy.selectedSettings.clear();
				ImGui::EndDisabled();
				if (ImGui::BeginChild("##CopySettingList", ImVec2(-FLT_MIN, C::Em(kCopyListHeightEm)),
						ImGuiChildFlags_Borders)) {
					if (!hasSource)
						ImGui::TextDisabled("%s", T("feature.scene_manager.copy.no_sources",
													  "No sources match this filter."));
					else {
						const auto& visibleSettings = state.copy.visibleSettingIndices;
						if (visibleSettings.empty())
							ImGui::TextDisabled("%s", T("feature.scene_manager.copy.no_settings",
														  "No settings match this search."));
						ImGuiListClipper clipper;
						clipper.Begin(static_cast<int>(visibleSettings.size()), ImGui::GetFrameHeightWithSpacing());
						while (clipper.Step())
							for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
								const auto index = visibleSettings[static_cast<size_t>(visibleIndex)];
								const auto& sourceSetting = state.copy.sourceSettings[index];
								bool selected = state.copy.selectedSettings.contains(sourceSetting.setting);
								ImGui::PushID(static_cast<int>(index));
								if (ImGui::Checkbox(sourceSetting.displayName.c_str(), &selected)) {
									if (selected)
										state.copy.selectedSettings.insert(sourceSetting.setting);
									else
										state.copy.selectedSettings.erase(sourceSetting.setting);
								}
								ImGui::PopID();
							}
					}
				}
				ImGui::EndChild();
				ImGui::EndTable();
			}
		}
		ImGui::EndChild();
		const bool hasSource = state.copy.selectedSource >= 0 &&
		                       state.copy.selectedSource < static_cast<int>(state.copy.sources.size());
		ImGui::BeginDisabled(!hasSource || state.copy.selectedSettings.empty());
		if (ImGui::Button(T("feature.scene_manager.copy.copy_selected", "Copy Selected Settings"),
				ImVec2(-FLT_MIN, 0.0f))) {
			state.destination = {};
			ImGui::OpenPopup("##CopyDestinations");
		}
		ImGui::EndDisabled();
		if (hasSource)
			DrawCopyDestinationPopup("##CopyDestinations", state.destination,
				state.copy.sources[state.copy.selectedSource].context, state.copy);
	}

	struct FeaturePageEditorState
	{
		std::string featureShortName;
		bool toolbarOpen = false;
		std::string pendingFeatureShortName;
		Util::ConfirmationPopup replaceEditor;
		Util::ConfirmationPopup replaceScene;
		std::optional<FeatureSceneTargetState> pendingTarget;
		bool saveFailed = false;
		std::vector<SceneSettingsManager::SceneContextType> supportedTypes;
		FeatureSceneTargetState edit;
		CopyDestinationState destination;

		std::optional<SceneSettingsManager::SceneContextId> activeContext;
		std::optional<SceneSettingsManager::SceneContextId> copySource;
		CopySettingState copy;
		FeatureLocationPickerCache locationPicker;
		Util::ConfirmationPopup deleteSettings;
		std::optional<SceneSettingsManager::SceneContextId> savedContext;
		std::uint64_t savedRevision = std::numeric_limits<std::uint64_t>::max();
		bool hasSavedSettings = false;
		SceneSettingsManager::EntryLayerSummary savedSummary;
		bool overwritesPaused = false;
		bool copyOpenRequested = false;
	};

	static FeaturePageEditorState s_featurePageEditor;

	static bool FeatureSupportsSceneContext(const std::string& featureShortName,
		SceneSettingsManager::SceneContextType type)
	{
		switch (type) {
		case SceneSettingsManager::SceneContextType::Interior:
			return SceneSettingsManager::IsFeatureAllowedForType(
				SceneType::InteriorOnly, featureShortName);
		case SceneSettingsManager::SceneContextType::TimeOfDay:
		case SceneSettingsManager::SceneContextType::Weather:
			return SceneSettingsManager::IsFeatureAllowedForType(
				SceneType::TimeOfDay, featureShortName);
		case SceneSettingsManager::SceneContextType::Location:
			return SceneSettingsManager::IsFeatureAllowedForType(
				SceneType::Location, featureShortName);
		default:
			return false;
		}
	}

	static std::vector<SceneSettingsManager::SceneContextType> GetFeatureSceneContextTypes(
		const std::string& featureShortName)
	{
		std::vector<SceneSettingsManager::SceneContextType> types;
		for (const auto type : {
				 SceneSettingsManager::SceneContextType::Interior,
				 SceneSettingsManager::SceneContextType::TimeOfDay,
				 SceneSettingsManager::SceneContextType::Weather,
				 SceneSettingsManager::SceneContextType::Location })
			if (FeatureSupportsSceneContext(featureShortName, type))
				types.push_back(type);
		return types;
	}

	static void SetFeatureTargetFromContext(FeatureSceneTargetState& target,
		const SceneSettingsManager::SceneContextId& context)
	{
		target.type = context.type;
		target.period = context.period;
		target.weatherId = context.weatherId;
		target.locationType = context.locationType;
		target.locationFormKey = context.locationFormKey;
	}

	static std::optional<SceneSettingsManager::SceneContextId> GetFeatureSceneContext(
		const FeatureSceneTargetState& target)
	{
		switch (target.type) {
		case SceneSettingsManager::SceneContextType::Interior:
			return SceneSettingsManager::SceneContextId{
				.type = SceneSettingsManager::SceneContextType::Interior,
			};
		case SceneSettingsManager::SceneContextType::TimeOfDay:
			if (target.period == Period::Count)
				return std::nullopt;
			return SceneSettingsManager::SceneContextId{
				.type = SceneSettingsManager::SceneContextType::TimeOfDay,
				.period = target.period,
			};
		case SceneSettingsManager::SceneContextType::Weather:
			if (target.weatherId == 0)
				return std::nullopt;
			return SceneSettingsManager::SceneContextId{
				.type = SceneSettingsManager::SceneContextType::Weather,
				.period = SceneSettingsManager::GetSingleton()->IsWeatherShowTimeOfDay(
							  target.weatherId) ?
				              target.period :
				              Period::Count,
				.weatherId = target.weatherId,
			};
		case SceneSettingsManager::SceneContextType::Location:
			if (target.locationFormKey.empty())
				return std::nullopt;
			return SceneSettingsManager::SceneContextId{
				.type = SceneSettingsManager::SceneContextType::Location,
				.period = SceneSettingsManager::GetSingleton()->IsLocationShowTimeOfDay(target.locationType, target.locationFormKey) ? target.period : Period::Count,
				.locationType = target.locationType,
				.locationFormKey = target.locationFormKey,
			};
		default:
			return std::nullopt;
		}
	}

	static bool SelectDefaultLocationTarget(FeatureSceneTargetState& target,
		const std::vector<SceneSettingsManager::LocationTarget>& targets,
		const std::vector<SceneSettingsManager::LocationTarget>& currentTargets)
	{
		auto selected = std::ranges::find_if(targets, [&](const auto& candidate) {
			return candidate.type == target.locationType &&
			       candidate.formKey == target.locationFormKey;
		});
		if (selected != targets.end())
			return false;
		if (targets.empty()) {
			target.locationFormKey.clear();
			return false;
		}
		auto fallback = targets.end();
		for (auto current = currentTargets.rbegin(); current != currentTargets.rend(); ++current) {
			fallback = std::ranges::find_if(targets, [&](const auto& candidate) {
				return candidate.type == current->type && candidate.formKey == current->formKey;
			});
			if (fallback != targets.end())
				break;
		}
		if (fallback == targets.end())
			fallback = std::ranges::find_if(targets, [&](const auto& candidate) {
				return !SceneSettingsManager::GetSingleton()
				            ->GetLocationConfig(candidate.type, candidate.formKey)
				            .entries.empty();
			});
		if (fallback == targets.end())
			fallback = targets.begin();
		target.locationType = fallback->type;
		target.locationFormKey = fallback->formKey;
		return true;
	}

	static bool NormalizeFeatureSceneTarget(FeatureSceneTargetState& target)
	{
		const auto previous = target;
		if (target.type == SceneSettingsManager::SceneContextType::TimeOfDay &&
			target.period == Period::Count)
			target.period = SceneSettingsManager::GetCurrentPeriod();
		if (target.type == SceneSettingsManager::SceneContextType::Weather) {
			const auto& weatherTargets = GetSceneWeatherTargets();
			auto selected = std::ranges::find_if(weatherTargets, [&](const auto* weather) {
				return weather->GetFormID() == target.weatherId;
			});
			if (selected == weatherTargets.end()) {
				if (auto* sky = globals::game::sky; sky && sky->currentWeather)
					selected = std::ranges::find_if(weatherTargets, [&](const auto* weather) {
						return weather->GetFormID() == sky->currentWeather->GetFormID();
					});
				if (selected == weatherTargets.end() && !weatherTargets.empty())
					selected = weatherTargets.begin();
				target.weatherId = selected != weatherTargets.end() ?
				                       (*selected)->GetFormID() :
				                       0;
			}
			if (target.weatherId != 0) {
				if (GetWeatherSelectionTimeOfDay(target.weatherId)) {
					if (target.period == Period::Count)
						target.period = SceneSettingsManager::GetCurrentPeriod();
				} else {
					target.period = Period::Count;
				}
			}
		}
		if (target.type == SceneSettingsManager::SceneContextType::Location) {
			auto* manager = SceneSettingsManager::GetSingleton();
			SelectDefaultLocationTarget(target, manager->GetLocationManagementTargets(),
				manager->GetCurrentLocationTargets());
			if (manager->IsLocationShowTimeOfDay(target.locationType, target.locationFormKey)) {
				if (target.period == Period::Count)
					target.period = SceneSettingsManager::GetCurrentPeriod();
			} else {
				target.period = Period::Count;
			}
		}
		return target != previous;
	}

	static std::string GetFeatureLocationTargetIdentity(
		const SceneSettingsManager::LocationTarget& target)
	{
		return std::format("{}:{}", static_cast<int>(target.type), target.formKey);
	}

	static void RefreshFeatureLocationPickerCache(
		const std::vector<SceneSettingsManager::LocationTarget>& targets,
		FeatureLocationPickerCache& cache)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto revision = manager->GetEntryPresentationRevision();
		const auto locale = I18n::GetSingleton()->GetCurrentLocale();
		if (cache.targetCount == targets.size() && cache.revision == revision &&
			cache.locale == locale)
			return;

		cache.targetCount = targets.size();
		cache.revision = revision;
		cache.locale = locale;
		cache.entries.clear();
		cache.entries.reserve(targets.size());
		cache.indicesByIdentity.clear();
		cache.configuredIndices.clear();
		cache.locationTypeIndices.clear();
		cache.unconfiguredIndices.clear();
		for (size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
			const auto& target = targets[targetIndex];
			auto identity = GetFeatureLocationTargetIdentity(target);
			const auto& entries = manager->GetLocationConfig(target.type, target.formKey).entries;
			auto presentation = GetScenePickerPresentation(GetLocationTargetLabel(target), entries, manager->IsLocationShowTimeOfDay(target.type, target.formKey));
			const bool configured = !entries.empty();
			const size_t cacheIndex = cache.entries.size();
			cache.indicesByIdentity.emplace(identity, cacheIndex);
			cache.entries.push_back({
				.targetIndex = targetIndex,
				.identity = std::move(identity),
				.displayLabel = std::move(presentation.label),
				.configured = configured,
				.paused = presentation.paused,
			});
			auto& entry = cache.entries.back();
			entry.selectableLabel = std::format("{}##FeatureScene{}", entry.displayLabel, entry.identity);
			(target.type == SceneSettingsManager::LocationTargetType::LocationType ? cache.locationTypeIndices :
				configured                                                         ? cache.configuredIndices :
																					 cache.unconfiguredIndices)
				.push_back(cacheIndex);
		}
		cache.currentIndices.clear();
		cache.currentMembership.assign(cache.entries.size(), 0);
		cache.visibleIndices.clear();
		cache.selectedTargetIndex = FeatureLocationPickerCache::kInvalidTargetIndex;
	}

	static bool DrawLocationPickerGroupHeader(const char* label, bool locationTypes, bool filtering)
	{
		if (locationTypes && !filtering) {
			ImGui::SeparatorText("");
			return ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_NoTreePushOnOpen);
		}
		ImGui::SeparatorText(label);
		return true;
	}

	template <typename PickerCache>
	static void OrderLocationTypePickerEntries(PickerCache& cache,
		const std::vector<SceneSettingsManager::LocationTarget>& targets)
	{
		std::ranges::sort(cache.locationTypeIndices);
		const auto unconfigured = std::ranges::stable_partition(cache.locationTypeIndices,
			[&](size_t index) { return cache.entries[index].configured; });
		std::ranges::stable_partition(unconfigured, [&](size_t index) {
			return IsCurrentLocation(targets[cache.entries[index].targetIndex]);
		});
	}

	static bool DrawFeatureLocationPicker(const char* id, FeatureSceneTargetState& target,
		FeatureLocationPickerCache& cache, std::string_view featureShortName)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& targets = manager->GetLocationManagementTargets();
		const auto& currentTargets = manager->GetCurrentLocationTargets();
		RefreshFeatureLocationPickerCache(targets, cache);
		bool changed = false;
		auto selected = cache.indicesByIdentity.find(
			std::format("{}:{}", static_cast<int>(target.locationType), target.locationFormKey));
		if (selected == cache.indicesByIdentity.end()) {
			changed |= SelectDefaultLocationTarget(target, targets, currentTargets);
			selected = cache.indicesByIdentity.find(
				std::format("{}:{}", static_cast<int>(target.locationType), target.locationFormKey));
		}
		cache.selectedTargetIndex = selected != cache.indicesByIdentity.end() ?
		                                cache.entries[selected->second].targetIndex :
		                                FeatureLocationPickerCache::kInvalidTargetIndex;
		const auto preview = cache.selectedTargetIndex < targets.size() ?
		                         cache.entries[selected->second].displayLabel :
		                         std::string(T("feature.scene_manager.location.select_target",
									 "Select a worldspace, region, location type, location, or cell..."));
		ImGui::SetNextItemWidth(-FLT_MIN);
		const auto previewLabel = GetScenePickerLabel(preview, GetFeatureSceneMarker(featureShortName,
																   { .type = SceneSettingsManager::SceneContextType::Location, .locationType = target.locationType, .locationFormKey = target.locationFormKey }, false));
		if (Util::BeginSearchableCombo(id, previewLabel.c_str(), ImGuiComboFlags_None,
				nullptr, kSceneTargetComboVisibleItems)) {
			OrderLocationTypePickerEntries(cache, targets);
			cache.currentIndices.clear();
			std::fill(cache.currentMembership.begin(), cache.currentMembership.end(), 0);
			for (auto current = currentTargets.rbegin(); current != currentTargets.rend(); ++current) {
				if (current->type == SceneSettingsManager::LocationTargetType::LocationType)
					continue;
				const auto found = cache.indicesByIdentity.find(
					GetFeatureLocationTargetIdentity(*current));
				if (found == cache.indicesByIdentity.end())
					continue;
				const size_t cacheIndex = found->second;
				if (cache.entries[cacheIndex].configured || cache.currentMembership[cacheIndex])
					continue;
				cache.currentMembership[cacheIndex] = 1;
				cache.currentIndices.push_back(cacheIndex);
			}

			const auto drawGroup = [&](const char* groupLabel,
									   const std::vector<size_t>& candidates,
									   bool excludeCurrent = false) {
				cache.visibleIndices.clear();
				int selectedVisibleIndex = -1;
				for (const auto cacheIndex : candidates) {
					if (excludeCurrent && cache.currentMembership[cacheIndex])
						continue;
					const auto& entry = cache.entries[cacheIndex];
					if (!Util::SearchableComboMatches(entry.displayLabel))
						continue;
					const auto& candidate = targets[entry.targetIndex];
					if (candidate.type == target.locationType &&
						candidate.formKey == target.locationFormKey)
						selectedVisibleIndex = static_cast<int>(cache.visibleIndices.size());
					cache.visibleIndices.push_back(cacheIndex);
				}
				if (cache.visibleIndices.empty())
					return;
				if (!DrawLocationPickerGroupHeader(groupLabel, &candidates == &cache.locationTypeIndices,
						!Util::GetSearchableComboFilter().empty()))
					return;
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(cache.visibleIndices.size()));
				if (selectedVisibleIndex >= 0)
					clipper.IncludeItemByIndex(selectedVisibleIndex);
				while (clipper.Step()) {
					for (int visibleIndex = clipper.DisplayStart;
						visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
						const auto& entry = cache.entries[cache.visibleIndices[visibleIndex]];
						const auto& candidate = targets[entry.targetIndex];
						const bool isSelected = candidate.type == target.locationType &&
						                        candidate.formKey == target.locationFormKey;
						const SceneSettingsManager::SceneContextId context{
							.type = SceneSettingsManager::SceneContextType::Location,
							.locationType = candidate.type,
							.locationFormKey = candidate.formKey,
						};

						if (DrawSceneTargetSelectable(entry.identity.c_str(), entry.displayLabel, isSelected,
								GetScenePickerTextColor(IsCurrentLocation(candidate), entry.paused), nullptr,
								GetFeatureSceneMarker(featureShortName, context, false))) {
							target.locationType = candidate.type;
							target.locationFormKey = candidate.formKey;
							cache.selectedTargetIndex = entry.targetIndex;
							changed = true;
						}
						if (isSelected)
							ImGui::SetItemDefaultFocus();
					}
				}
			};
			drawGroup(T("feature.scene_manager.location.group.current", "Current"),
				cache.currentIndices);
			drawGroup(T("feature.scene_manager.location.group.configured", "Configured"),
				cache.configuredIndices);
			drawGroup(T("feature.scene_manager.location.group.types", "Location Types"),
				cache.locationTypeIndices);
			drawGroup(T("feature.scene_manager.location.group.available", "Available"),
				cache.unconfiguredIndices, true);
			Util::EndSearchableCombo();
		}
		return changed;
	}

	static bool DrawFeatureSceneTypePicker(const char* id,
		std::span<const SceneSettingsManager::SceneContextType> supportedTypes,
		SceneSettingsManager::SceneContextType& selectedType, std::string_view featureShortName = {})
	{
		bool changed = false;
		ImGui::SetNextItemWidth(-FLT_MIN);
		const auto* preview = GetCopySourceTypeLabel(selectedType);
		const auto hasTypeSettings = [&](SceneSettingsManager::SceneContextType type) {
			return GetFeatureSceneMarker(featureShortName, { .type = type }, false, true);
		};
		const auto previewLabel = GetScenePickerLabel(preview, hasTypeSettings(selectedType));
		const bool open = ImGui::BeginCombo(id, previewLabel.c_str());
		if (open) {
			for (const auto type : supportedTypes) {
				const bool selected = type == selectedType;
				const auto start = ImGui::GetCursorScreenPos();
				const float right = start.x + ImGui::GetContentRegionAvail().x;
				if (ImGui::Selectable(std::format("##SceneType{}", static_cast<int>(type)).c_str(), selected)) {
					selectedType = type;
					changed = true;
				}
				DrawScenePickerText(GetCopySourceTypeLabel(type), start, right, ImGui::GetStyleColorVec4(ImGuiCol_Text), hasTypeSettings(type));
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	static void InitializeFeatureSceneTarget(Feature* feature, FeatureSceneTargetState& target)
	{
		const auto featureShortName = feature->GetShortName();
		target = {};
		target.period = SceneSettingsManager::GetCurrentPeriod();
		if (auto* sky = globals::game::sky; sky && sky->currentWeather)
			target.weatherId = sky->currentWeather->GetFormID();
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& currentLocations = manager->GetCurrentLocationTargets();

		if (Util::IsInterior() && FeatureSupportsSceneContext(
									  featureShortName, SceneSettingsManager::SceneContextType::Interior)) {
			target.type = SceneSettingsManager::SceneContextType::Interior;
		} else if (FeatureSupportsSceneContext(
					   featureShortName, SceneSettingsManager::SceneContextType::TimeOfDay)) {
			target.type = SceneSettingsManager::SceneContextType::TimeOfDay;
		} else if (!currentLocations.empty() && FeatureSupportsSceneContext(
													featureShortName, SceneSettingsManager::SceneContextType::Location)) {
			target.type = SceneSettingsManager::SceneContextType::Location;
		} else {
			const auto types = GetFeatureSceneContextTypes(featureShortName);
			if (!types.empty())
				target.type = types.front();
		}
		if (target.type == SceneSettingsManager::SceneContextType::Location)
			SelectDefaultLocationTarget(
				target, manager->GetLocationManagementTargets(), currentLocations);
	}

	static bool DrawFeatureTargetPicker(const char* idPrefix, FeatureSceneTargetState& target,
		FeatureLocationPickerCache& locationPicker, std::string_view featureShortName)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		bool changed = target.type == SceneSettingsManager::SceneContextType::Weather ||
		                       target.type == SceneSettingsManager::SceneContextType::Location ?
		                   false :
		                   NormalizeFeatureSceneTarget(target);
		switch (target.type) {
		case SceneSettingsManager::SceneContextType::Interior:
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(T("feature.scene_manager.tab.interior", "Interior"));
			break;
		case SceneSettingsManager::SceneContextType::TimeOfDay:
			{
				const SceneSettingsManager::SceneContextId context{ .type = target.type };
				changed |= DrawCopyPeriodPicker(
					std::format("{}Period", idPrefix).c_str(), target.period, false, nullptr, featureShortName, &context);
			}
			break;
		case SceneSettingsManager::SceneContextType::Weather:
		case SceneSettingsManager::SceneContextType::Location:
			{
				const auto previousTarget = target;
				const auto& weatherTargets = GetSceneWeatherTargets();
				const auto drawWeather = [&]() {
					const bool selected = target.type == SceneSettingsManager::SceneContextType::Weather ?
					                          DrawCopyWeatherPicker(std::format("{}Weather", idPrefix).c_str(), weatherTargets, target.weatherId, false, true, featureShortName) :
					                          DrawFeatureLocationPicker(std::format("{}Location", idPrefix).c_str(), target, locationPicker, featureShortName);
					changed |= selected;
					const bool weatherChanged = selected || target != previousTarget;
					if (weatherChanged) {
						const bool showTimeOfDay = target.type == SceneSettingsManager::SceneContextType::Weather ?
						                               GetWeatherSelectionTimeOfDay(target.weatherId) :
						                               manager->IsLocationShowTimeOfDay(target.locationType, target.locationFormKey);
						if (showTimeOfDay) {
							if (target.period == Period::Count)
								target.period = SceneSettingsManager::GetCurrentPeriod();
						} else {
							target.period = Period::Count;
						}
					}
				};
				const auto layoutId = std::format("{}WeatherLayout", idPrefix);
				ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 0.0f));
				const SKSE::stl::scope_exit restorePadding([]() noexcept { ImGui::PopStyleVar(); });
				if (ImGui::BeginTable(layoutId.c_str(), 2,
						ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
					ImGui::TableSetupColumn("##WeatherTarget", ImGuiTableColumnFlags_WidthStretch, 2.0f);
					ImGui::TableSetupColumn("##WeatherPeriod", ImGuiTableColumnFlags_WidthStretch, 1.0f);
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					drawWeather();
					ImGui::TableSetColumnIndex(1);
					const SceneSettingsManager::SceneContextId context{
						.type = target.type,
						.weatherId = target.weatherId,
						.locationType = target.locationType,
						.locationFormKey = target.locationFormKey,
					};
					changed |= DrawCopyPeriodPicker(
						std::format("{}WeatherPeriod", idPrefix).c_str(), target.period, true, GetCopySourceTypeLabel(target.type), featureShortName, &context);
					ImGui::EndTable();
				}
				changed |= target != previousTarget;
			}
			break;
		}
		return changed;
	}

	static void RefreshCopyDestinationTypes(CopyDestinationState& state,
		const SceneSettingsManager::SceneContextId& source, const CopySettingState& copy)
	{
		state.supportedTypes.clear();
		state.numericSettings = std::ranges::all_of(copy.sourceSettings, [&](const auto& setting) {
			return !copy.selectedSettings.contains(setting.setting) || setting.numeric;
		});
		if (!state.numericSettings)
			state.timeOfDay = false;
		auto* manager = SceneSettingsManager::GetSingleton();
		for (const auto type : {
				 SceneSettingsManager::SceneContextType::Interior,
				 SceneSettingsManager::SceneContextType::TimeOfDay,
				 SceneSettingsManager::SceneContextType::Weather,
				 SceneSettingsManager::SceneContextType::Location }) {
			if (source.allPeriods && type != SceneSettingsManager::SceneContextType::TimeOfDay &&
				type != SceneSettingsManager::SceneContextType::Weather && type != SceneSettingsManager::SceneContextType::Location)
				continue;
			const auto compatible = manager->GetCopySourceSettings(source, copy.sourceLayer, type);
			if (!state.settings.empty() && std::ranges::all_of(state.settings, [&](const auto& setting) {
					return std::ranges::any_of(compatible, [&](const auto& candidate) {
						return candidate.setting == setting;
					});
				}))
				state.supportedTypes.push_back(type);
		}
		if (!state.supportedTypes.empty() &&
			std::ranges::find(state.supportedTypes, state.target.type) == state.supportedTypes.end())
			state.target.type = state.supportedTypes.front();
	}

	static bool IsInactiveCopySet(const SceneSettingsManager::SceneContextId& target, bool timeOfDay)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = target.type == SceneSettingsManager::SceneContextType::Weather ?
		                          manager->GetWeatherConfig(target.weatherId).entries :
		                          manager->GetLocationConfig(target.locationType, target.locationFormKey).entries;
		return !entries.empty() && manager->IsSceneTimeOfDayEnabled(target) != timeOfDay;
	}

	static void DrawCopyWeatherDestinations(CopyDestinationState& state)
	{
		ImGui::PushID("CopyWeatherDestinations");
		const SKSE::stl::scope_exit restoreId([]() noexcept { ImGui::PopID(); });
		auto* manager = SceneSettingsManager::GetSingleton();
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##CopyWeatherSearch", T("ui.search", "Search..."), &state.weatherSearch);
		const auto filter = Util::ToLowerAscii(state.weatherSearch);
		std::vector<RE::TESWeather*> current;
		std::vector<RE::TESWeather*> configured;
		std::vector<RE::TESWeather*> available;
		for (auto* weather : GetSceneWeatherTargets()) {
			const auto id = weather->GetFormID();
			if (!filter.empty() && !Util::ToLowerAscii(GetWeatherPickerLabel(weather)).contains(filter))
				continue;
			if (!manager->GetWeatherConfig(id).entries.empty())
				configured.push_back(weather);
			else if (globals::game::sky && (globals::game::sky->currentWeather == weather || globals::game::sky->lastWeather == weather))
				current.push_back(weather);
			else
				available.push_back(weather);
		}
		if (ImGui::SmallButton(T("feature.scene_manager.action.select_all", "Select All")))
			for (const auto* group : { &current, &configured, &available })
				for (const auto* weather : *group)
					state.weatherIds.insert(weather->GetFormID());
		ImGui::SameLine();
		if (ImGui::SmallButton(T("feature.scene_manager.action.select_none", "Select None")))
			state.weatherIds.clear();
		if (ImGui::BeginChild("##CopyWeatherList", ImVec2(0.0f, C::Em(kCopyListHeightEm)),
				ImGuiChildFlags_Borders)) {
			const auto drawGroup = [&](const char* label, const std::vector<RE::TESWeather*>& weathers) {
				if (weathers.empty())
					return;
				ImGui::SeparatorText(label);
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(weathers.size()), ImGui::GetFrameHeightWithSpacing());
				while (clipper.Step())
					for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
						auto* weather = weathers[index];
						const auto id = weather->GetFormID();
						bool selected = state.weatherIds.contains(id);

						ImGui::PushID(static_cast<int>(id));
						const auto color = GetScenePickerTextColor(globals::game::sky && globals::game::sky->currentWeather == weather,
							GetWeatherPickerPresentation(weather).paused,
							IsInactiveCopySet({ .type = SceneSettingsManager::SceneContextType::Weather, .weatherId = id }, state.timeOfDay));
						if (DrawScenePickerCheckbox(GetWeatherPickerLabel(weather), &selected, color)) {
							if (selected)
								state.weatherIds.insert(id);
							else
								state.weatherIds.erase(id);
						}
						ImGui::PopID();
					}
			};
			drawGroup(T("feature.scene_manager.weather.group.current", "Current"), current);
			drawGroup(T("feature.scene_manager.weather.group.configured", "Configured"), configured);
			drawGroup(T("feature.scene_manager.weather.group.available", "Available"), available);
		}
		ImGui::EndChild();
	}

	static void DrawCopyLocationDestinations(CopyDestinationState& state)
	{
		ImGui::PushID("CopyLocationDestinations");
		const SKSE::stl::scope_exit restoreId([]() noexcept { ImGui::PopID(); });
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& targets = manager->GetLocationManagementTargets();
		const auto& currentTargets = manager->GetCurrentLocationTargets();
		auto& cache = state.locationPicker;
		RefreshFeatureLocationPickerCache(targets, cache);
		OrderLocationTypePickerEntries(cache, targets);
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##CopyLocationSearch", T("ui.search", "Search..."), &state.locationSearch);
		const auto filter = Util::ToLowerAscii(state.locationSearch);
		cache.currentIndices.clear();
		std::fill(cache.currentMembership.begin(), cache.currentMembership.end(), 0);
		for (auto current = currentTargets.rbegin(); current != currentTargets.rend(); ++current) {
			if (current->type == SceneSettingsManager::LocationTargetType::LocationType)
				continue;
			const auto found = cache.indicesByIdentity.find(GetFeatureLocationTargetIdentity(*current));
			if (found == cache.indicesByIdentity.end() || cache.entries[found->second].configured || cache.currentMembership[found->second])
				continue;
			cache.currentMembership[found->second] = 1;
			cache.currentIndices.push_back(found->second);
		}
		cache.visibleIndices.clear();
		const std::array groups{ &cache.currentIndices, &cache.configuredIndices, &cache.locationTypeIndices, &cache.unconfiguredIndices };
		const std::array groupLabels{
			T("feature.scene_manager.location.group.current", "Current"),
			T("feature.scene_manager.location.group.configured", "Configured"),
			T("feature.scene_manager.location.group.types", "Location Types"),
			T("feature.scene_manager.location.group.available", "Available"),
		};
		std::array<size_t, groups.size() + 1> groupOffsets{};
		for (size_t group = 0; group < groups.size(); ++group) {
			groupOffsets[group] = cache.visibleIndices.size();
			for (const auto index : *groups[group]) {
				if (groups[group] != &cache.currentIndices && cache.currentMembership[index])
					continue;
				if (filter.empty() || Util::ToLowerAscii(cache.entries[index].displayLabel).contains(filter))
					cache.visibleIndices.push_back(index);
			}
		}
		groupOffsets.back() = cache.visibleIndices.size();
		if (ImGui::SmallButton(T("feature.scene_manager.action.select_all", "Select All")))
			for (const auto index : cache.visibleIndices) {
				const auto& target = targets[cache.entries[index].targetIndex];
				state.locations.insert({ target.type, target.formKey });
			}
		ImGui::SameLine();
		if (ImGui::SmallButton(T("feature.scene_manager.action.select_none", "Select None")))
			state.locations.clear();
		if (ImGui::BeginChild("##CopyLocationList", ImVec2(0.0f, C::Em(kCopyListHeightEm)), ImGuiChildFlags_Borders)) {
			for (size_t group = 0; group < groups.size(); ++group) {
				const auto start = groupOffsets[group];
				const auto count = groupOffsets[group + 1] - start;
				if (count == 0)
					continue;
				if (!DrawLocationPickerGroupHeader(groupLabels[group], groups[group] == &cache.locationTypeIndices,
						!filter.empty()))
					continue;
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(count), ImGui::GetFrameHeightWithSpacing());
				while (clipper.Step())
					for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
						const auto& entry = cache.entries[cache.visibleIndices[start + index]];
						const auto& target = targets[entry.targetIndex];
						const auto identity = std::pair{ target.type, target.formKey };
						bool selected = state.locations.contains(identity);
						ImGui::PushID(entry.identity.c_str());

						const auto color = GetScenePickerTextColor(IsCurrentLocation(target), entry.paused,
							IsInactiveCopySet({ .type = SceneSettingsManager::SceneContextType::Location, .locationType = target.type, .locationFormKey = target.formKey }, state.timeOfDay));
						if (DrawScenePickerCheckbox(entry.displayLabel, &selected, color)) {
							if (selected)
								state.locations.insert(identity);
							else
								state.locations.erase(identity);
						}
						ImGui::PopID();
					}
			}
		}
		ImGui::EndChild();
	}

	static std::vector<SceneSettingsManager::SceneContextId> GetCopyDestinations(
		const CopyDestinationState& state, bool sourceAllPeriods)
	{
		std::vector<SceneSettingsManager::SceneContextId> destinations;
		const auto addPeriods = [&](SceneSettingsManager::SceneContextId context, bool timeOfDay) {
			if (sourceAllPeriods) {
				context.allPeriods = true;
				destinations.push_back(context);
			} else if (timeOfDay && state.target.period == Period::Count) {
				for (int index = 0; index < kPeriodCount; ++index) {
					context.period = static_cast<Period>(index);
					destinations.push_back(context);
				}
			} else {
				destinations.push_back(context);
			}
		};
		if (state.target.type == SceneSettingsManager::SceneContextType::Weather) {
			for (const auto id : state.weatherIds) {
				const bool timeOfDay = state.timeOfDay;
				if (sourceAllPeriods && !timeOfDay)
					continue;
				addPeriods({
							   .type = SceneSettingsManager::SceneContextType::Weather,
							   .period = timeOfDay ? state.target.period : Period::Count,
							   .weatherId = id,
						   },
					timeOfDay);
			}
		} else if (state.target.type == SceneSettingsManager::SceneContextType::TimeOfDay) {
			addPeriods({ .type = state.target.type, .period = state.target.period }, true);
		} else if (state.target.type == SceneSettingsManager::SceneContextType::Location) {
			for (const auto& [type, formKey] : state.locations) {
				const bool timeOfDay = state.timeOfDay;
				if ((sourceAllPeriods && !timeOfDay) || (timeOfDay && !state.numericSettings))
					continue;
				addPeriods({
							   .type = SceneSettingsManager::SceneContextType::Location,
							   .period = timeOfDay ? state.target.period : Period::Count,
							   .locationType = type,
							   .locationFormKey = formKey,
						   },
					timeOfDay);
			}
		} else if (const auto context = GetFeatureSceneContext(state.target)) {
			destinations.push_back(*context);
		}
		return destinations;
	}

	static void DrawCopyDestinationPopup(const char* id, CopyDestinationState& state,
		const SceneSettingsManager::SceneContextId& source, CopySettingState& copy, bool selectSettings)
	{
		if (!ImGui::IsPopupOpen(id))
			return;
		ImGui::SetNextWindowPos(s_featurePageCenter, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(C::Em(36.0f), 0.0f),
			ImVec2(C::Em(36.0f), ImGui::GetMainViewport()->WorkSize.y - ImGui::GetStyle().WindowPadding.y * 2.0f));
		auto popup = Util::Popup(id, T("feature.scene_manager.copy.to", "Copy to"));
		if (!popup)
			return;
		if (selectSettings && ImGui::CollapsingHeader(T("feature.scene_manager.column.setting", "Setting"))) {
			ImGui::PushID("CopySettingSelection");
			const SKSE::stl::scope_exit restoreId([]() noexcept { ImGui::PopID(); });
			if (ImGui::SmallButton(T("feature.scene_manager.action.select_all", "Select All")))
				for (const auto& setting : copy.sourceSettings)
					copy.selectedSettings.insert(setting.setting);
			ImGui::SameLine();
			if (ImGui::SmallButton(T("feature.scene_manager.action.select_none", "Select None")))
				copy.selectedSettings.clear();
			const auto visibleRows = std::clamp(copy.sourceSettings.size(), size_t{ 1 }, kCopySettingVisibleRows);
			const float listHeight = ImGui::GetFrameHeightWithSpacing() * static_cast<float>(visibleRows) -
			                         ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y * 2.0f;
			if (ImGui::BeginChild("##FeatureCopySettings", ImVec2(0.0f, listHeight),
					ImGuiChildFlags_Borders)) {
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(copy.sourceSettings.size()), ImGui::GetFrameHeightWithSpacing());
				while (clipper.Step())
					for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
						const auto& setting = copy.sourceSettings[index];
						bool selected = copy.selectedSettings.contains(setting.setting);
						ImGui::PushID(index);
						if (ImGui::Checkbox(setting.displayName.c_str(), &selected)) {
							if (selected)
								copy.selectedSettings.insert(setting.setting);
							else
								copy.selectedSettings.erase(setting.setting);
						}
						ImGui::PopID();
					}
			}
			ImGui::EndChild();
		}

		auto* manager = SceneSettingsManager::GetSingleton();
		const auto selected = GetSelectedCopySettings(copy);
		const auto revision = manager->GetEntryPresentationRevision();
		const bool changed = state.source != source || state.sourceLayer != copy.sourceLayer ||
		                     state.settings != selected || state.revision != revision;
		if (changed) {
			state.source = source;
			state.sourceLayer = copy.sourceLayer;
			state.settings = selected;
			state.revision = revision;
			RefreshCopyDestinationTypes(state, source, copy);
		}
		if (state.supportedTypes.empty()) {
			ImGui::TextWrapped("%s", T("feature.scene_manager.copy.no_compatible_destinations",
										 "No destination supports all selected settings. Change the setting selection first."));
			if (ImGui::Button(T("feature.scene_manager.action.cancel", "Cancel"), ImVec2(-FLT_MIN, 0.0f)))
				ImGui::CloseCurrentPopup();
			return;
		}
		DrawFeatureSceneTypePicker("##CopyDestinationType", state.supportedTypes, state.target.type);
		if (source.allPeriods) {
			state.target.period = Period::Count;
			state.timeOfDay = true;
		}
		if (state.target.type == SceneSettingsManager::SceneContextType::Weather ||
			state.target.type == SceneSettingsManager::SceneContextType::Location) {
			ImGui::BeginDisabled(source.allPeriods);
			DrawSceneModePicker("##CopyDestinationMode", state.timeOfDay, state.target.type, state.numericSettings);
			if (state.timeOfDay)
				DrawCopyPeriodPicker("##CopyDestinationScenePeriod", state.target.period, true);
			ImGui::EndDisabled();
			ImGui::TextDisabled("%s", T("feature.scene_manager.copy.mode_unchanged", "Empty scenes use the copied mode. Configured scenes keep their active mode."));
		}
		if (state.target.type == SceneSettingsManager::SceneContextType::Weather) {
			DrawCopyWeatherDestinations(state);
		} else if (state.target.type == SceneSettingsManager::SceneContextType::TimeOfDay) {
			ImGui::BeginDisabled(source.allPeriods);
			DrawCopyPeriodPicker("##CopyDestinationPeriod", state.target.period, true);
			ImGui::EndDisabled();
		} else if (state.target.type == SceneSettingsManager::SceneContextType::Location) {
			DrawCopyLocationDestinations(state);
		}
		auto destinations = GetCopyDestinations(state, source.allPeriods);
		if (copy.sourceLayer == EntrySource::User)
			std::erase(destinations, source);
		if (changed || destinations != state.destinations) {
			state.destinations = std::move(destinations);
			state.compatibleCount = 0;
			state.maskedCount = 0;
			state.hasConflicts = false;
			for (const auto& destination : state.destinations)
				for (const auto& candidate : manager->GetCopyCandidates(source, destination, copy.sourceLayer, selected)) {
					state.compatibleCount += candidate.compatible;
					state.maskedCount += candidate.compatible && candidate.maskedByOverwrite;
					state.hasConflicts |= candidate.compatible && candidate.conflicts;
				}
		}
		ImGui::TextDisabled("%s", std::vformat(
									  T("feature.scene_manager.copy.selected_compatible", "Selected compatible settings ({0})"),
									  std::make_format_args(state.compatibleCount))
									  .c_str());
		if (state.maskedCount != 0)
			Util::Text::WrappedError("%s", std::vformat(
											   T("feature.scene_manager.copy.masked_destination",
												   "{0} copied User Settings will remain overridden until the destination overwrites are paused or removed."),
											   std::make_format_args(state.maskedCount))
											   .c_str());
		if (state.hasConflicts) {
			ImGui::TextWrapped("%s", T("feature.scene_manager.copy.conflicts_message",
										 "Some settings already exist here. Choose how to handle them."));
			if (ImGui::RadioButton(T("feature.scene_manager.copy.skip_existing", "Skip Existing"),
					state.policy == SceneSettingsManager::CopyConflictPolicy::SkipExisting))
				state.policy = SceneSettingsManager::CopyConflictPolicy::SkipExisting;
			if (ImGui::RadioButton(T("feature.scene_manager.copy.replace_existing_user", "Replace Existing User Settings"),
					state.policy == SceneSettingsManager::CopyConflictPolicy::OverwriteExisting))
				state.policy = SceneSettingsManager::CopyConflictPolicy::OverwriteExisting;
		}
		ImGui::BeginDisabled(state.compatibleCount == 0 || (selectSettings && manager->AreFeatureSceneEditActionsLocked()));
		if (ImGui::Button(T("feature.scene_manager.copy.copy_selected", "Copy Selected Settings"), ImVec2(-FLT_MIN, 0.0f))) {
			bool copied = false;
			for (const auto& destination : state.destinations)
				copied |= manager->CopySettings(source, destination, copy.sourceLayer, state.policy, selected, true).Changed();
			if (copied)
				manager->CommitSceneSettingChanges();
			copy.revision = std::numeric_limits<std::uint64_t>::max();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		if (ImGui::Button(T("feature.scene_manager.action.cancel", "Cancel"), ImVec2(-FLT_MIN, 0.0f)))
			ImGui::CloseCurrentPopup();
	}

	static bool IsConsoleIdentifier(std::string_view value)
	{
		return !value.empty() && std::ranges::all_of(value, [](unsigned char character) {
			return std::isalnum(character) != 0 || character == '_';
		});
	}

	static const SceneSettingsManager::LocationTarget* GetSelectedFeatureLocationTarget(
		const FeatureSceneTargetState& target, const FeatureLocationPickerCache& cache)
	{
		const auto& targets = SceneSettingsManager::GetSingleton()->GetLocationManagementTargets();
		if (cache.selectedTargetIndex >= targets.size())
			return nullptr;
		const auto& selected = targets[cache.selectedTargetIndex];
		return selected.type == target.locationType && selected.formKey == target.locationFormKey ?
		           &selected :
		           nullptr;
	}

	static std::optional<std::string> GetCellTravelCommand(RE::TESObjectCELL* cell)
	{
		if (!cell)
			return std::nullopt;
		const auto cocCode = Util::GetFormEditorID(cell);
		if (IsConsoleIdentifier(cocCode))
			return std::format("coc {}", cocCode);
		if (!cell->IsExteriorCell())
			return std::nullopt;
		auto* coordinates = cell->GetCoordinates();
		auto* worldSpace = cell->GetRuntimeData().worldSpace;
		const auto worldSpaceEditorId = Util::GetFormEditorID(worldSpace);
		if (!coordinates || !IsConsoleIdentifier(worldSpaceEditorId))
			return std::nullopt;
		return std::format(
			"cow {} {} {}", worldSpaceEditorId, coordinates->cellX, coordinates->cellY);
	}

	static std::optional<std::string> GetLocationTravelCommand(
		const SceneSettingsManager::LocationTarget& target)
	{
		if (IsConsoleIdentifier(target.cocCode))
			return std::format("coc {}", target.cocCode);

		RE::TESObjectCELL* cell = nullptr;
		if (target.type == SceneSettingsManager::LocationTargetType::Cell) {
			cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(target.formId);
		} else if (target.type == SceneSettingsManager::LocationTargetType::Location) {
			if (auto* location = RE::TESForm::LookupByID<RE::BGSLocation>(target.formId)) {
				auto marker = location->worldLocMarker ? location->worldLocMarker.get() : nullptr;
				if (marker)
					cell = marker->GetParentCell();
			}
		}
		return GetCellTravelCommand(cell);
	}

	static std::optional<SceneSettingsManager::SceneContextId> s_playingContext;

	static bool CanPreviewFeatureSceneContext(
		const SceneSettingsManager::SceneContextId& context,
		const std::optional<std::string>& locationTravelCommand)
	{
		if (context.type == SceneSettingsManager::SceneContextType::Interior || context.allPeriods)
			return false;
		if (context.type == SceneSettingsManager::SceneContextType::Location && !locationTravelCommand)
			return false;
		if ((context.type == SceneSettingsManager::SceneContextType::TimeOfDay ||
				context.period != Period::Count) &&
			(!globals::game::calendar || !globals::game::calendar->gameHour || !globals::game::calendar->timeScale ||
				static_cast<unsigned>(context.period) >= kPeriodCount))
			return false;
		return context.type != SceneSettingsManager::SceneContextType::Weather ||
		       (globals::game::sky && RE::TESForm::LookupByID<RE::TESWeather>(context.weatherId));
	}

	bool SetFeaturePagePreviewPlaying(bool playing)
	{
		if (!playing) {
			Util::EnvironmentControls::StopPreview();
			s_playingContext.reset();
			return true;
		}
		if (!SceneSettingsManager::GetSingleton()->IsSceneReady())
			return false;
		auto& state = s_featurePageEditor;
		const auto context = GetFeatureSceneContext(state.edit);
		std::optional<std::string> travel;
		if (context && context->type == SceneSettingsManager::SceneContextType::Location)
			if (const auto* target = GetSelectedFeatureLocationTarget(state.edit, state.locationPicker))
				travel = GetLocationTravelCommand(*target);
		if (!context || !CanPreviewFeatureSceneContext(*context, travel))
			return false;
		std::optional<float> hour;
		if (context->period != Period::Count) {
			const auto& hours = SceneSettingsManager::kPeriodHours[static_cast<int>(context->period)];
			hour = std::fmod((hours[0] + hours[1]) * 0.5f, Util::EnvironmentControls::kHoursPerDay);
		}
		auto* weather = context->type == SceneSettingsManager::SceneContextType::Weather ?
		                    RE::TESForm::LookupByID<RE::TESWeather>(context->weatherId) :
		                    nullptr;
		if (weather || hour) {
			if (!Util::EnvironmentControls::StartPreview(weather, hour))
				return false;
		} else {
			Util::EnvironmentControls::StopPreview();
		}
		s_playingContext = context;
		if (travel)
			RE::Console::ExecuteCommand(travel->c_str());
		return true;
	}

	static bool DrawPreviewButton(bool playing)
	{
		if (!playing)
			return ImGui::ArrowButton("##FeatureScenePreview", ImGuiDir_Right);
		const float size = ImGui::GetFrameHeight();
		const bool pressed = Util::ErrorTextButton("##FeatureScenePreview", ImVec2(size, size));
		const ImVec2 center = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()).GetCenter();
		const float halfExtent = ImGui::GetFontSize() * 0.25f;
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(center.x - halfExtent, center.y - halfExtent),
			ImVec2(center.x + halfExtent, center.y + halfExtent), ImGui::GetColorU32(ImGuiCol_Text));
		return pressed;
	}

	static void InitializeFeatureCopyDestination(FeaturePageEditorState& state)
	{
		state.destination = {};
		state.destination.target = state.edit;
	}

	static void RefreshFeatureCopySettings(FeaturePageEditorState& state)
	{
		if (!state.copySource)
			return;
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto revision = manager->GetEntryPresentationRevision();
		if (state.copy.revision == revision &&
			state.copy.selectionSourceContext == state.copySource)
			return;

		state.copy.sourceSettings = manager->GetCopySourceSettings(
			*state.copySource, EntrySource::User);
		std::erase_if(state.copy.sourceSettings, [&](const auto& setting) {
			return setting.setting.featureShortName != state.featureShortName;
		});
		state.copy.selectedSettings.clear();
		for (const auto& setting : state.copy.sourceSettings)
			state.copy.selectedSettings.insert(setting.setting);
		state.copy.selectionSourceContext = state.copySource;
		state.copy.revision = revision;
	}

	static void DrawFeatureCopyPopup(FeaturePageEditorState& state)
	{
		if (!ImGui::IsPopupOpen("##FeatureSceneCopy") || !state.copySource)
			return;
		RefreshFeatureCopySettings(state);
		DrawCopyDestinationPopup("##FeatureSceneCopy", state.destination, *state.copySource, state.copy, true);
	}

	bool CanEditFeaturePage(Feature* feature)
	{
		if (!feature)
			return false;
		const auto featureShortName = feature->GetShortName();
		return FeatureSupportsSceneContext(
				   featureShortName, SceneSettingsManager::SceneContextType::Interior) ||
		       FeatureSupportsSceneContext(
				   featureShortName, SceneSettingsManager::SceneContextType::TimeOfDay) ||
		       FeatureSupportsSceneContext(
				   featureShortName, SceneSettingsManager::SceneContextType::Location);
	}

	static bool StartFeaturePageEditing(Feature* feature)
	{
		SetFeaturePagePreviewPlaying(false);
		SceneSettingsManager::GetSingleton()->EndFeatureSceneEdit(false);
		auto& state = s_featurePageEditor;
		state = {};
		state.featureShortName = feature->GetShortName();
		state.toolbarOpen = true;
		state.supportedTypes = GetFeatureSceneContextTypes(state.featureShortName);
		InitializeFeatureSceneTarget(feature, state.edit);
		const auto context = GetFeatureSceneContext(state.edit);
		if (context && SceneSettingsManager::GetSingleton()->BeginFeatureSceneEdit(feature, *context)) {
			state.activeContext = context;
		}
		InitializeFeatureCopyDestination(state);
		return true;
	}

	bool BeginFeaturePageEditing(Feature* feature)
	{
		if (!CanEditFeaturePage(feature))
			return false;
		auto& state = s_featurePageEditor;
		if (state.featureShortName == feature->GetShortName()) {
			state.toolbarOpen = true;
			return true;
		}
		if (!state.featureShortName.empty() && SceneSettingsManager::GetSingleton()->HasPendingFeatureSceneEdits()) {
			state.pendingFeatureShortName = feature->GetShortName();
			state.replaceEditor.Request();
			return false;
		}
		return StartFeaturePageEditing(feature);
	}

	static void DrawFeaturePageEditConfirmation(Feature* feature)
	{
		auto& state = s_featurePageEditor;
		if (state.pendingFeatureShortName != feature->GetShortName())
			return;
		if (!SceneSettingsManager::GetSingleton()->HasPendingFeatureSceneEdits()) {
			StartFeaturePageEditing(feature);
			return;
		}
		state.replaceEditor.title = std::format("{}##FeatureSceneReplace",
			T("feature.scene_manager.edit.discard_title", "Discard Unsaved Scene Settings?"));
		state.replaceEditor.message = I18n::GetSingleton()->Format("feature.scene_manager.edit.discard_message",
			{ { "feature", SceneSettingsManager::GetFeatureDisplayName(state.featureShortName) }, { "target", feature->GetDisplayName() } },
			"Unsaved scene settings for {feature} will be discarded. Open Scene Manager for {target}?");
		state.replaceEditor.confirmLabel = T("feature.scene_manager.edit.discard_confirm", "Discard and Open");
		state.replaceEditor.cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");
		if (state.replaceEditor.Draw())
			StartFeaturePageEditing(feature);
		else if (!state.replaceEditor.IsOpen())
			state.pendingFeatureShortName.clear();
	}

	bool IsFeaturePageEditing(Feature* feature)
	{
		return feature && s_featurePageEditor.toolbarOpen &&
		       s_featurePageEditor.featureShortName == feature->GetShortName();
	}

	static bool FinishActiveFeatureSceneEdit(FeaturePageEditorState& state)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		if (!manager->IsFeatureSceneEditing(state.featureShortName)) {
			state.activeContext.reset();
			return true;
		}
		manager->EndFeatureSceneEdit(false);
		state.activeContext.reset();
		state.saveFailed = false;
		state.overwritesPaused = false;
		return !manager->IsFeatureSceneEditing(state.featureShortName);
	}

	static bool CanBeginFeatureSceneContext(
		const SceneSettingsManager::SceneContextId& context)
	{
		if (context.type != SceneSettingsManager::SceneContextType::Location)
			return true;
		const auto& currentTargets = SceneSettingsManager::GetSingleton()->GetCurrentLocationTargets();
		return std::ranges::any_of(currentTargets, [&](const auto& target) {
			return target.type == context.locationType &&
			       target.formKey == context.locationFormKey;
		});
	}

	void HideFeaturePageEditing()
	{
		s_featurePageEditor.toolbarOpen = false;
	}

	bool DrawFeaturePageControls(Feature* feature, bool enabled)
	{
		s_featurePageCenter = ImGui::GetCurrentWindow()->Rect().GetCenter();
		if (!feature)
			return false;
		if (enabled && SceneSettingsManager::GetSingleton()->IsSceneReady())
			DrawFeaturePageEditConfirmation(feature);
		if (!enabled || !IsFeaturePageEditing(feature))
			return false;
		auto* manager = SceneSettingsManager::GetSingleton();
		auto& state = s_featurePageEditor;
		const auto disableToolbar = Util::DisableGuard(!manager->IsSceneReady());
		const auto featureShortName = feature->GetShortName();
		if (!manager->IsFeatureSceneEditing(featureShortName)) {
			state.activeContext.reset();
			state.overwritesPaused = false;
		} else
			state.overwritesPaused = manager->AreFeatureSceneEditOverwritesPaused();

		if (state.pendingTarget) {
			state.replaceScene.title = std::format("{}##FeatureSceneChange",
				T("feature.scene_manager.edit.discard_title", "Discard Unsaved Scene Settings?"));
			state.replaceScene.message = T("feature.scene_manager.edit.discard_scene_message",
				"Unsaved scene settings will be discarded. Switch to the selected scene?");
			state.replaceScene.confirmLabel = T("feature.scene_manager.edit.discard_confirm", "Discard and Open");
			state.replaceScene.cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");
			if (state.replaceScene.Draw()) {
				FinishActiveFeatureSceneEdit(state);
				state.edit = *state.pendingTarget;
				state.pendingTarget.reset();
			} else if (!state.replaceScene.IsOpen())
				state.pendingTarget.reset();
		}

		const auto previousTarget = state.edit;
		bool closeEditor = false;
		std::optional<SceneSettingsManager::SceneContextId> requestedContext;
		const auto cellPadding = ImGui::GetStyle().CellPadding;
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
			ImVec2(std::max(1.0f, cellPadding.x * 0.5f), cellPadding.y));
		if (ImGui::BeginTable("##FeatureSceneEditorBar", 8,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
			ImGui::TableSetupColumn("##SceneManagerType", ImGuiTableColumnFlags_WidthStretch,
				0.65f);
			ImGui::TableSetupColumn("##SceneManagerTarget", ImGuiTableColumnFlags_WidthStretch,
				1.3f);
			ImGui::TableSetupColumn("##SceneManagerPreview", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("##SceneManagerSave", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("##SceneManagerCopy", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("##SceneManagerPause", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("##SceneManagerDelete", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("##SceneManagerClose", ImGuiTableColumnFlags_WidthFixed,
				ImGui::GetFrameHeight());
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			const bool typeChanged = DrawFeatureSceneTypePicker(
				"##FeatureSceneEditType", state.supportedTypes, state.edit.type, state.featureShortName);
			if (typeChanged)
				NormalizeFeatureSceneTarget(state.edit);

			ImGui::TableSetColumnIndex(1);
			DrawFeatureTargetPicker(
				"##FeatureSceneEdit", state.edit, state.locationPicker, state.featureShortName);

			bool canSwitchContext = manager->IsSceneReady();
			if (canSwitchContext && state.activeContext != GetFeatureSceneContext(state.edit) &&
				manager->HasPendingFeatureSceneEdits()) {
				state.pendingTarget = state.edit;
				state.replaceScene.Request();
				canSwitchContext = false;
			}
			if (canSwitchContext && state.edit.type == SceneSettingsManager::SceneContextType::Weather &&
				state.edit.weatherId != 0) {
				const bool requestedTimeOfDay = state.edit.period != Period::Count;
				if (manager->IsWeatherShowTimeOfDay(state.edit.weatherId) != requestedTimeOfDay) {
					canSwitchContext = FinishActiveFeatureSceneEdit(state);
					if (canSwitchContext)
						manager->SetWeatherShowTimeOfDay(
							state.edit.weatherId, requestedTimeOfDay);
				}
			}

			if (canSwitchContext && state.edit.type == SceneSettingsManager::SceneContextType::Location && !state.edit.locationFormKey.empty()) {
				const bool requestedTimeOfDay = state.edit.period != Period::Count;
				if (manager->IsLocationShowTimeOfDay(state.edit.locationType, state.edit.locationFormKey) != requestedTimeOfDay) {
					canSwitchContext = FinishActiveFeatureSceneEdit(state);
					if (canSwitchContext)
						manager->SetLocationShowTimeOfDay(state.edit.locationType, state.edit.locationFormKey, requestedTimeOfDay);
				}
			}

			requestedContext = GetFeatureSceneContext(state.edit);
			if (canSwitchContext && state.activeContext != requestedContext) {
				canSwitchContext = FinishActiveFeatureSceneEdit(state);
				if (canSwitchContext) {
					if (requestedContext && CanBeginFeatureSceneContext(*requestedContext) &&
						manager->BeginFeatureSceneEdit(feature, *requestedContext))
						state.activeContext = requestedContext;
					state.copySource.reset();
					state.copy.Reset();
				}
			}
			if (!canSwitchContext) {
				if (state.activeContext)
					SetFeatureTargetFromContext(state.edit, *state.activeContext);
				else
					state.edit = previousTarget;
			}
			requestedContext = GetFeatureSceneContext(state.edit);
			std::optional<std::string> locationTravelCommand;
			manager->SetFeatureSceneEditOverwritesPaused(state.overwritesPaused);
			if (requestedContext &&
				requestedContext->type == SceneSettingsManager::SceneContextType::Location)
				if (const auto* target = GetSelectedFeatureLocationTarget(
						state.edit, state.locationPicker))
					locationTravelCommand = GetLocationTravelCommand(*target);

			ImGui::TableSetColumnIndex(2);
			if (s_playingContext && s_playingContext != requestedContext)
				if (!Util::EnvironmentControls::IsPreviewActive() || !SetFeaturePagePreviewPlaying(true))
					SetFeaturePagePreviewPlaying(false);
			const bool playing = Util::EnvironmentControls::IsPreviewActive();
			const bool canPreview = requestedContext &&
			                        CanPreviewFeatureSceneContext(
										*requestedContext, locationTravelCommand);
			ImGui::BeginDisabled(!playing && !canPreview);
			const bool previewPressed = DrawPreviewButton(playing);
			ImGui::EndDisabled();
			Util::AddTooltip(playing ? T("feature.scene_manager.edit.stop_preview", "Stop preview and release weather/time locks") :
									   T("feature.scene_manager.edit.play_preview", "Preview the selected scene and lock its weather/time"));
			if (previewPressed)
				SetFeaturePagePreviewPlaying(!playing);

			ImGui::TableSetColumnIndex(3);
			ImGui::BeginDisabled(!state.activeContext || !manager->HasPendingFeatureSceneEdits());
			const auto saveLabel = GetScenePickerLabel(T("menu.save_settings", "Save Settings"), manager->HasPendingFeatureSceneEdits() ? SceneSettingMarker::Settings : SceneSettingMarker::None) + "###FeatureSceneSave";
			if (ImGui::Button(saveLabel.c_str()))
				state.saveFailed = !manager->StoreFeatureSceneEdit();
			ImGui::EndDisabled();

			if (state.savedContext != requestedContext || state.savedRevision != manager->GetEntryPresentationRevision()) {
				state.savedContext = requestedContext;
				state.savedRevision = manager->GetEntryPresentationRevision();
				state.savedSummary = requestedContext ? manager->GetFeatureSceneSummary(featureShortName, *requestedContext) : SceneSettingsManager::EntryLayerSummary{};
				state.hasSavedSettings = requestedContext && std::ranges::any_of(
																 manager->GetCopySourceSettings(*requestedContext, EntrySource::User),
																 [&](const auto& setting) { return setting.setting.featureShortName == featureShortName; });
			}
			ImGui::TableSetColumnIndex(4);
			ImGui::BeginDisabled(!state.hasSavedSettings || manager->HasPendingFeatureSceneEdits() || manager->AreFeatureSceneEditActionsLocked());
			if (ImGui::Button(T("feature.scene_manager.copy.to", "Copy to")) && requestedContext) {
				state.copy.Reset();
				state.copy.sourceLayer = EntrySource::User;
				state.copySource = requestedContext;
				InitializeFeatureCopyDestination(state);
				state.copyOpenRequested = true;
			}
			ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(5);
			ImGui::BeginDisabled(state.savedSummary.Empty() || manager->HasPendingFeatureSceneEdits() || manager->AreFeatureSceneEditActionsLocked());
			const auto* pauseLabel = state.savedSummary.paused == 0 ? T("feature.scene_manager.action.pause", "Pause") :
			                         state.savedSummary.AllPaused() ? T("feature.scene_manager.action.resume", "Resume") :
			                                                          T("feature.scene_manager.action.resume_all", "Resume All");
			if (ImGui::Button(std::format("{}###FeatureScenePause", pauseLabel).c_str()) && requestedContext)
				manager->SetFeatureSceneSettingsPaused(featureShortName, *requestedContext, state.savedSummary.paused == 0);
			ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(6);
			ImGui::BeginDisabled(!state.hasSavedSettings || manager->AreFeatureSceneEditActionsLocked());
			if (ImGui::Button(T("feature.scene_manager.action.delete", "Delete")))
				state.deleteSettings.Request();
			ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(7);
			const float closeButtonSize = ImGui::GetFrameHeight();
			closeEditor = Util::CloseButton("##FeatureSceneEditorClose", closeButtonSize);
			ImGui::EndTable();
		}
		ImGui::PopStyleVar();
		if (closeEditor) {
			HideFeaturePageEditing();
			return false;
		}
		if (state.saveFailed && manager->HasPendingFeatureSceneEdits())
			ImGui::TextColored(Util::Colors::GetError(), "%s", T("feature.scene_manager.edit.save_failed", "Could not save scene settings. Your unsaved changes are kept. Try Save Settings again."));
		if (manager->HasFeatureSceneEditOverwrites() || manager->AreFeatureSceneEditOverwritesPaused()) {
			const bool paused = manager->AreFeatureSceneEditOverwritesPaused();
			ImGui::TextColored(Util::Colors::GetError(), "%s", paused ? T("feature.scene_manager.edit.overwrites_paused", "Feature overwrites are temporarily paused") : T("feature.scene_manager.edit.overwritten_warning", "Feature settings are being overwritten"));
			ImGui::SameLine();
			const auto* label = paused ? T("feature.scene_manager.edit.resume_overwrites", "Resume Overwrites") : T("feature.scene_manager.edit.pause_overwrites", "Pause Overwrites");
			if (ImGui::SmallButton(std::format("{}###FeatureOverwritePause", label).c_str())) {
				state.overwritesPaused = !paused;
				manager->SetFeatureSceneEditOverwritesPaused(!paused);
			}
		}
		if (state.copyOpenRequested) {
			ImGui::OpenPopup("##FeatureSceneCopy");
			state.copyOpenRequested = false;
		}
		DrawFeatureCopyPopup(state);

		state.deleteSettings.title = std::format("{}##FeatureSceneDelete",
			T("feature.scene_manager.edit.delete_title", "Delete Scene Settings?"));
		state.deleteSettings.message = T("feature.scene_manager.edit.delete_message",
			"Remove this feature's User Settings from the selected scene?");
		state.deleteSettings.confirmLabel = T("feature.scene_manager.action.delete", "Delete");
		state.deleteSettings.cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");
		if (state.deleteSettings.Draw() && requestedContext && !manager->AreFeatureSceneEditActionsLocked()) {
			FinishActiveFeatureSceneEdit(state);
			manager->DeleteFeatureSceneSettings(featureShortName, *requestedContext);
			if (CanBeginFeatureSceneContext(*requestedContext) &&
				manager->BeginFeatureSceneEdit(feature, *requestedContext))
				state.activeContext = requestedContext;
			state.copySource.reset();
			state.copy.Reset();
		}
		ImGui::Separator();
		return true;
	}

	// Core add-setting dialog: renders UI and delegates data ops to callbacks.
	static void DrawAddDialogCore(AddSettingState& state, Period period, bool addToAllPeriods,
		bool selectAggregateMember,
		std::function<std::vector<SceneSettingDescriptor>(const std::string&)> settingsFn,
		std::function<std::set<OverrideKey>()> addedEntriesFn,
		std::function<bool(const std::string&, const std::vector<std::string>&, const std::string&, Period)> isAddedFn,
		std::function<bool(const std::string&, const std::vector<std::string>&, const std::string&, const json&, Period)> addFn,
		std::function<void()> commitFn)
	{
		if (!state.dialogOpen) {
			if (s_activeAddDialog == &state)
				s_activeAddDialog = nullptr;
			return;
		}
		const int currentFrame = ImGui::GetFrameCount();
		const bool revisited = state.lastDrawFrame >= 0 && state.lastDrawFrame + 1 < currentFrame;
		state.lastDrawFrame = currentFrame;
		s_addDialogFrame = currentFrame;
		if (revisited && state.selectedFeatureIdx >= 0 &&
			state.selectedFeatureIdx < static_cast<int>(state.cachedFeatureNames.size())) {
			RefreshCachedSettingValues(
				state, settingsFn(state.cachedFeatureNames[state.selectedFeatureIdx]));
		}

		ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
		const float dialogWidth = C::Em(C::SCENE_ADD_DIALOG_WIDTH_EM);
		ImGui::SetNextWindowSizeConstraints(ImVec2(dialogWidth, 0.0f), ImVec2(dialogWidth, FLT_MAX));
		auto windowTitle = std::format("{}##{:x}",
			T("feature.scene_manager.add_dialog.title", "Add Feature Settings"),
			reinterpret_cast<uintptr_t>(&state));
		const bool measureInitialLayout = state.measureInitialLayout;
		if (measureInitialLayout)
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
		const auto windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
		                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
		                         (measureInitialLayout ? ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNavFocus : 0);

		if (!Util::BeginWithRoundedClose(windowTitle.c_str(), &state.dialogOpen, windowFlags)) {
			ImGui::End();
			if (measureInitialLayout) {
				ImGui::PopStyleVar();
				state.measureInitialLayout = false;
			}
			return;
		}
		if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
			ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

		auto displayName = (state.selectedFeatureIdx >= 0 &&
							   state.selectedFeatureIdx < static_cast<int>(state.cachedFeatureNames.size())) ?
		                       SceneSettingsManager::GetFeatureDisplayName(state.cachedFeatureNames[state.selectedFeatureIdx]) :
		                       std::string(T("feature.scene_manager.select_feature", "Select Feature..."));

		ImGui::SetNextItemWidth(-FLT_MIN);
		if (Util::BeginSearchableCombo("##FeatureSelect", displayName.c_str())) {
			for (int i = 0; i < static_cast<int>(state.cachedFeatureNames.size()); ++i) {
				auto itemLabel = SceneSettingsManager::GetFeatureDisplayName(state.cachedFeatureNames[i]);
				if (!Util::SearchableComboMatches(itemLabel))
					continue;
				if (ImGui::Selectable(itemLabel.c_str(), i == state.selectedFeatureIdx)) {
					state.selectedFeatureIdx = i;
					CacheSettings(state, settingsFn(state.cachedFeatureNames[i]));
				}
				if (i == state.selectedFeatureIdx)
					ImGui::SetItemDefaultFocus();
			}
			Util::EndSearchableCombo();
		}

		const AddSettingNode* visibleNode = nullptr;
		if (!state.cachedSettings.empty())
			visibleNode = !state.settingTree.children.empty() ? DrawSubFeatureSelectors(state) : &state.settingTree;
		const auto presentationRevision =
			SceneSettingsManager::GetSingleton()->GetEntryPresentationRevision();
		if ((!state.addedMembersCached || state.cachedAddedRevision != presentationRevision) &&
			state.selectedFeatureIdx >= 0 &&
			state.selectedFeatureIdx < static_cast<int>(state.cachedFeatureNames.size())) {
			const auto& featureName = state.cachedFeatureNames[state.selectedFeatureIdx];
			const auto addedEntries = addedEntriesFn();
			const auto isAdded = [&](const auto& member, Period targetPeriod) {
				return addedEntries.contains(OverrideKey{
					featureName, member.settingPath, member.key, static_cast<int>(targetPeriod) });
			};
			state.cachedAddedMembers.assign(state.cachedSettings.size(), {});
			for (size_t settingIndex = 0; settingIndex < state.cachedSettings.size(); ++settingIndex) {
				const auto& descriptor = state.cachedSettings[settingIndex];
				auto& addedMembers = state.cachedAddedMembers[settingIndex];
				addedMembers.reserve(descriptor.members.size());
				for (const auto& member : descriptor.members) {
					bool added = isAdded(member, period);
					if (addToAllPeriods) {
						added = true;
						for (int targetPeriod = 0; targetPeriod < kPeriodCount; ++targetPeriod)
							added &= isAdded(member, static_cast<Period>(targetPeriod));
					}
					addedMembers.push_back(added ? 1 : 0);
				}
			}
			state.addedMembersCached = true;
			state.cachedAddedRevision = presentationRevision;
		}
		const auto selectionAlreadyAdded = [&](size_t settingIndex, int selectedMember) {
			if (settingIndex >= state.cachedAddedMembers.size())
				return true;
			const auto& addedMembers = state.cachedAddedMembers[settingIndex];
			if (selectedMember >= 0 && selectedMember < static_cast<int>(addedMembers.size()))
				return addedMembers[selectedMember] != 0;
			return std::all_of(addedMembers.begin(), addedMembers.end(), [](uint8_t added) { return added != 0; });
		};
		const bool shiftHeld = ImGui::GetIO().KeyShift;
		if (selectAggregateMember && state.shiftWasDown && !shiftHeld) {
			for (size_t index = 0; index < state.cachedSettings.size(); ++index) {
				const auto& descriptor = state.cachedSettings[index];
				if (descriptor.unifiedEditMode != SceneSettingsManager::UnifiedEditMode::Shift ||
					GetStoredAllMemberIndex(descriptor) >= 0 || state.selectedMembers[index] >= 0)
					continue;
				if (!descriptor.members.empty())
					state.selectedMembers[index] = 0;
				state.selectedSettings[index] = false;
			}
		}
		state.shiftWasDown = shiftHeld;

		bool hasVisibleSettings = state.selectedFeatureIdx >= 0 && visibleNode && !visibleNode->settings.empty();
		if (hasVisibleSettings) {
			ImGui::Spacing();
			ImGui::Separator();

			if (ImGui::SmallButton(T("feature.scene_manager.action.select_all", "Select All")))
				ForEachSettingIndex(*visibleNode, [&](size_t i) {
					const int selectedMember = selectAggregateMember ? state.selectedMembers[i] : -1;
					state.selectedSettings[i] = !selectionAlreadyAdded(i, selectedMember);
				});
			ImGui::SameLine();
			if (ImGui::SmallButton(T("feature.scene_manager.action.select_none", "Select None")))
				ForEachSettingIndex(*visibleNode, [&](size_t i) { state.selectedSettings[i] = false; });

			ImGui::Spacing();

			auto& featureName = state.cachedFeatureNames[state.selectedFeatureIdx];
			if (ImGui::BeginChild("##SettingList", ImVec2(-FLT_MIN, C::Em(C::SCENE_ADD_LIST_HEIGHT_EM)), ImGuiChildFlags_Borders)) {
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(visibleNode->settings.size()), ImGui::GetFrameHeightWithSpacing());
				while (clipper.Step()) {
					for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
						const auto i = visibleNode->settings[static_cast<size_t>(visibleIndex)];
						const auto& descriptor = state.cachedSettings[i];
						const bool hasStoredAll = GetStoredAllMemberIndex(descriptor) >= 0;
						const bool showSyntheticAll = !hasStoredAll &&
						                              CanSelectSyntheticAll(descriptor, shiftHeld);
						const int selectedMember = selectAggregateMember ? state.selectedMembers[i] : -1;
						const bool alreadyAdded = selectionAlreadyAdded(i, selectedMember);

						auto prettyKey = GetDescriptorDisplayName(descriptor);
						ImGui::PushID(static_cast<int>(i));
						if (alreadyAdded) {
							state.selectedSettings[i] = false;
							auto _ = Util::DisableGuard(true);
							bool checked = true;
							ImGui::Checkbox(prettyKey.c_str(), &checked);
						} else {
							bool sel = state.selectedSettings[i];
							if (ImGui::Checkbox(prettyKey.c_str(), &sel))
								state.selectedSettings[i] = sel;
						}
						if (selectAggregateMember && descriptor.controlType != SceneSettingControlType::Scalar &&
							!descriptor.members.empty()) {
							ImGui::SameLine();
							ImGui::SetNextItemWidth(C::Em(kChannelSelectorWidthEm));
							const auto preview = state.selectedMembers[i] < 0 ?
							                         std::string(T("feature.scene_manager.channel.all", "All")) :
							                         GetDescriptorMemberName(descriptor, state.selectedMembers[i]);
							if (Util::BeginSearchableCombo("##Channel", preview.c_str())) {
								const bool allSelected = state.selectedMembers[i] < 0;
								if (showSyntheticAll && Util::SearchableComboMatches(
															T("feature.scene_manager.channel.all", "All"))) {
									if (ImGui::Selectable(T("feature.scene_manager.channel.all", "All"), allSelected))
										state.selectedMembers[i] = -1;
									if (allSelected)
										ImGui::SetItemDefaultFocus();
								}
								for (size_t member = 0; member < descriptor.members.size(); ++member) {
									auto memberName = GetDescriptorMemberName(descriptor, member);
									if (!Util::SearchableComboMatches(memberName))
										continue;
									const bool selected = state.selectedMembers[i] == static_cast<int>(member);
									if (ImGui::Selectable(memberName.c_str(), selected))
										state.selectedMembers[i] = static_cast<int>(member);
									if (selected)
										ImGui::SetItemDefaultFocus();
								}
								Util::EndSearchableCombo();
							}
						}
						ImGui::PopID();
					}
				}
			}
			ImGui::EndChild();

			ImGui::Spacing();

			for (size_t i = 0; i < state.selectedSettings.size(); ++i) {
				const int selectedMember = selectAggregateMember ? state.selectedMembers[i] : -1;
				if (state.selectedSettings[i] && selectionAlreadyAdded(i, selectedMember))
					state.selectedSettings[i] = false;
			}

			int selectedCount = 0;
			for (size_t i = 0; i < state.selectedSettings.size(); ++i)
				if (state.selectedSettings[i])
					++selectedCount;

			{
				auto _ = Util::DisableGuard(selectedCount == 0);
				auto label = std::vformat(T("feature.scene_manager.action.add_count", "Add ({0})"),
					std::make_format_args(selectedCount));
				if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0))) {
					bool added = false;
					for (size_t i = 0; i < state.cachedSettings.size(); ++i) {
						if (!state.selectedSettings[i])
							continue;
						const auto& descriptor = state.cachedSettings[i];
						const int selectedMember = selectAggregateMember ? state.selectedMembers[i] : -1;
						ForEachSelectedDescriptorMember(descriptor, selectedMember, [&](const auto& member) {
							const auto& currentValue = member.value;
							if (addToAllPeriods) {
								for (int p = 0; p < kPeriodCount; ++p)
									if (!isAddedFn(featureName, member.settingPath, member.key, static_cast<Period>(p)))
										added |= addFn(featureName, member.settingPath, member.key, currentValue, static_cast<Period>(p));
							} else if (!isAddedFn(featureName, member.settingPath, member.key, period)) {
								added |= addFn(featureName, member.settingPath, member.key, currentValue, period);
							}
						});
					}
					if (added)
						commitFn();
					state.dialogOpen = false;
					if (s_activeAddDialog == &state)
						s_activeAddDialog = nullptr;
				}
			}
		}

		ImGui::End();
		if (measureInitialLayout) {
			ImGui::PopStyleVar();
			state.measureInitialLayout = false;
		}
		if (!state.dialogOpen && s_activeAddDialog == &state)
			s_activeAddDialog = nullptr;
	}

	void DrawAddSettingDialog(SceneType type, AddSettingState& state, Period period, bool addToAllPeriods)
	{
		if (!state.dialogOpen) {
			if (s_activeAddDialog == &state)
				s_activeAddDialog = nullptr;
			return;
		}
		auto* manager = SceneSettingsManager::GetSingleton();
		DrawAddDialogCore(state, period, addToAllPeriods, type == SceneType::TimeOfDay, [type](const std::string& feature) { return type == SceneType::TimeOfDay ?
			                                                                                                                            SceneSettingsManager::GetTransitionableSceneSettings(feature) :
			                                                                                                                            SceneSettingsManager::GetFeatureSceneSettings(type, feature); }, [=] { return BuildUserEntrySet(manager->GetEntries(type)); }, [type](const std::string& feature, const std::vector<std::string>& path, const std::string& key, Period targetPeriod) { return IsAlreadyAdded(type, feature, path, key, targetPeriod); }, [=](const std::string& feature, const std::vector<std::string>& path, const std::string& key, const json& value, Period targetPeriod) { return manager->AddSetting(
																																																																																																																																																																										type, feature, path, key, value, targetPeriod, true); }, [=] { manager->CommitSceneSettingChanges(); });
	}

	void DrawWeatherAddDialog(RE::FormID weatherId, AddSettingState& state, Period period, bool addToAllPeriods)
	{
		if (!state.dialogOpen) {
			if (s_activeAddDialog == &state)
				s_activeAddDialog = nullptr;
			return;
		}
		auto* manager = SceneSettingsManager::GetSingleton();
		DrawAddDialogCore(state, period, addToAllPeriods, true, [](const std::string& feature) { return SceneSettingsManager::GetTransitionableSceneSettings(feature); }, [=] { return BuildUserEntrySet(manager->GetWeatherConfig(weatherId).entries); }, [=](const std::string& feature, const std::vector<std::string>& path, const std::string& key, Period targetPeriod) { return manager->HasWeatherEntryForPeriod(
																																																																																													weatherId, feature, path, key, targetPeriod, EntrySource::User); }, [=](const std::string& feature, const std::vector<std::string>& path, const std::string& key, const json&, Period targetPeriod) { return manager->AddWeatherSetting(
																																																																																																																																																								   weatherId, feature, path, key, targetPeriod, true); }, [=] { manager->CommitSceneSettingChanges(); });
	}

	FlyoutResult DrawFlyoutControls(bool paused, bool isGroup, bool isOverwrite)
	{
		FlyoutResult result;
		float frameH = ImGui::GetFrameHeight();
		float buttonH = frameH * C::FLYOUT_BUTTON_SCALE;
		float toggleH = buttonH * C::FLYOUT_TOGGLE_SCALE;
		float toggleOffsetY = (buttonH - toggleH) * 0.5f;

		ImVec2 cursor = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + toggleOffsetY));
		bool active = !paused;
		if (Util::FeatureToggle(isGroup ? "##groupActive" : "##active", &active, GetCompactFeatureToggleSize()))
			result.toggled = true;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(paused ?
									   (isGroup ? T("feature.scene_manager.action.unpause_all", "Unpause All") : T("feature.scene_manager.status.paused", "Paused")) :
									   (isGroup ? T("feature.scene_manager.action.pause_all", "Pause All") : T("feature.scene_manager.status.active", "Active")));

		ImGui::SameLine();
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cursor.y));
		auto* menu = globals::menu;
		float iconH = frameH * C::FLYOUT_BUTTON_SCALE;
		float revertPad = iconH * C::FLYOUT_REVERT_PAD_SCALE;
		if (isOverwrite)
			ImGui::BeginDisabled();
		if (menu && DrawSceneIconButton(isGroup ? "##revertAll" : "##revert", menu->uiIcons.undo.texture, ImVec2(iconH, iconH), revertPad))
			result.reverted = true;
		if (!isOverwrite) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(isGroup ?
										   T("feature.scene_manager.action.revert_all", "Revert all to default") :
										   T("feature.scene_manager.action.revert", "Revert to default"));
		}
		if (isOverwrite)
			ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cursor.y));
		if (DrawSceneDeleteButton("X", iconH))
			result.deleted = true;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(isGroup ?
									   T("feature.scene_manager.action.remove_all", "Remove all") :
									   T("feature.scene_manager.action.remove", "Remove"));

		return result;
	}

	// Core value editor: renders the catalog-approved widget and calls updateFn on change.
	static void DrawValueEditorCore(const SettingEntry& entry, float inputWidth,
		bool useFullWidthSliders, bool readOnly,
		std::function<void(const json&)> updateFn, std::function<void()> commitFn)
	{
		const auto& value = entry.value;
		const auto choiceCount = SceneSettingsManager::GetSettingChoiceCount(entry);
		const bool booleanControl = SceneSettingsManager::IsBooleanControlSetting(entry);
		auto settingType = booleanControl ? SceneSettingsManager::SettingType::Boolean :
		                                    SceneSettingsManager::DetectSettingType(value);
		auto disabled = Util::DisableGuard(readOnly);

		if (choiceCount > 0) {
			const auto currentValue = value.get<std::int64_t>();
			std::string preview;
			for (size_t choiceIndex = 0; choiceIndex < choiceCount; ++choiceIndex) {
				std::int64_t choiceValue = 0;
				std::string choiceName;
				if (SceneSettingsManager::GetSettingChoice(entry, choiceIndex, choiceValue, choiceName) &&
					choiceValue == currentValue) {
					preview = std::move(choiceName);
					break;
				}
			}
			ImGui::SetNextItemWidth(inputWidth);
			const bool comboOpen = Util::BeginSearchableCombo("##val", preview.c_str(),
				ImGuiComboFlags_None, &entry.value);
			if (comboOpen) {
				for (size_t choiceIndex = 0; choiceIndex < choiceCount; ++choiceIndex) {
					std::int64_t choiceValue = 0;
					std::string choiceName;
					if (!SceneSettingsManager::GetSettingChoice(entry, choiceIndex, choiceValue, choiceName))
						continue;
					if (!Util::SearchableComboMatches(choiceName))
						continue;
					const bool selected = choiceValue == currentValue;
					if (ImGui::Selectable(choiceName.c_str(), selected) && !readOnly) {
						updateFn(json(choiceValue));
						commitFn();
					}
					if (selected)
						ImGui::SetItemDefaultFocus();
				}
				Util::EndSearchableCombo();
			}
		} else
			switch (settingType) {
			case SceneSettingsManager::SettingType::Boolean:
				{
					const bool invertedDisplay = SceneSettingsManager::IsInvertedDisplaySetting(entry);
					const bool storedValue = value.is_boolean() ? value.get<bool>() : value.get<int64_t>() != 0;
					bool val = invertedDisplay ? !storedValue : storedValue;
					if (ImGui::Checkbox("##val", &val) && !readOnly) {
						const bool updatedValue = invertedDisplay ? !val : val;
						updateFn(value.is_boolean() ? json(updatedValue) : json(updatedValue ? 1 : 0));
						commitFn();
					}
				}
				break;
			case SceneSettingsManager::SettingType::Float:
				{
					double minimum = 0.0;
					double maximum = 0.0;
					bool hasBounds = SceneSettingsManager::GetNumericBounds(entry, minimum, maximum);
					double minimumDisplayValue = 0.0;
					double maximumDisplayValue = 0.0;
					hasBounds = hasBounds &&
					            SceneSettingsManager::GetNumericDisplayValue(entry, minimum, minimumDisplayValue) &&
					            SceneSettingsManager::GetNumericDisplayValue(entry, maximum, maximumDisplayValue) &&
					            minimumDisplayValue <= maximumDisplayValue;
					double displayValue = 0.0;
					if (!value.is_number() ||
						!SceneSettingsManager::GetNumericDisplayValue(entry, value.get<double>(), displayValue))
						displayValue = hasBounds ? minimumDisplayValue : 0.0;
					float val = static_cast<float>(displayValue);
					if (!std::isfinite(val))
						val = hasBounds ? static_cast<float>(minimumDisplayValue) : 0.0f;
					const double displayScale = SceneSettingsManager::GetNumericDisplayScale(entry);
					const float minimumValue = hasBounds ? static_cast<float>(minimumDisplayValue) : 0.0f;
					const float maximumValue = hasBounds ? static_cast<float>(maximumDisplayValue) : 0.0f;
					const float dragSpeed = static_cast<float>(kSceneFloatDragSpeed * displayScale);
					const auto sliderFlags = SceneSettingsManager::IsNumericInputClamped(entry) ?
					                             ImGuiSliderFlags_AlwaysClamp :
					                             ImGuiSliderFlags_None;
					ImGui::SetNextItemWidth(inputWidth);
					const bool changed = useFullWidthSliders && hasBounds ?
					                         ImGui::SliderFloat("##val", &val, minimumValue, maximumValue,
												 displayScale == 1.0 ? "%.3f" : "%.1f", sliderFlags) :
					                         ImGui::DragFloat("##val", &val, dragSpeed, minimumValue, maximumValue,
												 displayScale == 1.0 ? "%.3f" : "%.1f", sliderFlags);
					if (changed && !readOnly) {
						double storedValue = 0.0;
						if (SceneSettingsManager::GetNumericStoredValue(entry, val, storedValue))
							updateFn(json(storedValue));
					}
					if (!readOnly && ImGui::IsItemDeactivatedAfterEdit())
						commitFn();
				}
				break;
			case SceneSettingsManager::SettingType::Integer:
				{
					double minimum = 0.0;
					double maximum = 0.0;
					const bool hasBounds = SceneSettingsManager::GetNumericBounds(entry, minimum, maximum);
					std::int64_t val = value.get<std::int64_t>();
					const auto minimumValue = static_cast<std::int64_t>(minimum);
					const auto maximumValue = static_cast<std::int64_t>(maximum);
					const auto sliderFlags = SceneSettingsManager::IsNumericInputClamped(entry) ?
					                             ImGuiSliderFlags_AlwaysClamp :
					                             ImGuiSliderFlags_None;
					ImGui::SetNextItemWidth(inputWidth);
					const bool changed = useFullWidthSliders && hasBounds ?
					                         ImGui::SliderScalar("##val", ImGuiDataType_S64, &val,
												 &minimumValue, &maximumValue, "%lld", sliderFlags) :
					                         ImGui::DragScalar("##val", ImGuiDataType_S64, &val, kSceneIntDragSpeed,
												 hasBounds ? &minimumValue : nullptr,
												 hasBounds ? &maximumValue : nullptr, "%lld", sliderFlags);
					if (changed && !readOnly)
						updateFn(json(val));
					if (!readOnly && ImGui::IsItemDeactivatedAfterEdit())
						commitFn();
				}
				break;
			case SceneSettingsManager::SettingType::String:
				{
					std::string val = value.is_string() ? value.get<std::string>() : std::string();
					ImGui::SetNextItemWidth(inputWidth);
					if (ImGui::InputText("##val", &val) && !readOnly)
						updateFn(json(val));
					if (!readOnly && ImGui::IsItemDeactivatedAfterEdit())
						commitFn();
				}
				break;
			default:
				ImGui::TextDisabled("%s", T("feature.scene_manager.unsupported_type", "(unsupported type)"));
				break;
			}
	}

	template <class Update, class Commit>
	static bool DrawAggregateValueEditorCore(const std::vector<SettingEntry>& entries,
		const std::vector<size_t>& indices, float inputWidth, bool showExpandedAggregateControls,
		bool readOnly, Update&& update, Commit&& commit)
	{
		if (indices.size() < 2)
			return false;

		struct Component
		{
			SceneSettingsManager::SettingControlInfo info;
			std::vector<size_t> entryIndices;
		};
		std::map<std::int8_t, Component> components;
		std::optional<bool> paused;
		for (const auto index : indices) {
			if (index >= entries.size() || !entries[index].value.is_number())
				return false;
			SceneSettingsManager::SettingControlInfo info;
			if (!SceneSettingsManager::GetSettingControlInfo(entries[index], info) ||
				info.controlType == SceneSettingControlType::Scalar)
				return false;
			if (!paused)
				paused = entries[index].paused;
			else if (*paused != entries[index].paused)
				return false;
			auto [componentIt, inserted] = components.try_emplace(info.componentIndex);
			if (inserted)
				componentIt->second.info = std::move(info);
			else {
				const auto& existing = componentIt->second.info;
				if (info.controlType != existing.controlType || info.settingPath != existing.settingPath ||
					info.settingKey != existing.settingKey || info.componentStart != existing.componentStart ||
					info.componentCount != existing.componentCount ||
					info.aggregatePresentation != existing.aggregatePresentation ||
					info.unifiedEditMode != existing.unifiedEditMode)
					return false;
			}
			if (std::find(componentIt->second.entryIndices.begin(), componentIt->second.entryIndices.end(), index) ==
				componentIt->second.entryIndices.end())
				componentIt->second.entryIndices.push_back(index);
		}
		if (components.size() < 2 || components.size() > 4)
			return false;

		const auto& first = components.begin()->second.info;
		if (components.size() != first.componentCount)
			return false;
		if (first.controlType == SceneSettingControlType::Color &&
			components.size() != 3 && components.size() != 4)
			return false;
		std::array<const Component*, 4> orderedComponents{};
		for (size_t component = 0; component < components.size(); ++component) {
			auto componentIt = components.find(static_cast<std::int8_t>(first.componentStart + component));
			if (componentIt == components.end())
				return false;
			const auto& info = componentIt->second.info;
			if (info.controlType != first.controlType || info.settingPath != first.settingPath ||
				info.settingKey != first.settingKey || info.componentStart != first.componentStart ||
				info.componentCount != first.componentCount ||
				info.componentIndex != first.componentStart + component ||
				info.aggregatePresentation != first.aggregatePresentation ||
				info.unifiedEditMode != first.unifiedEditMode)
				return false;
			orderedComponents[component] = &componentIt->second;
		}

		std::array<float, 4> values{};
		for (size_t component = 0; component < components.size(); ++component)
			values[component] = entries[orderedComponents[component]->entryIndices.back()].value.get<float>();

		if (readOnly)
			ImGui::BeginDisabled();
		bool changed = false;
		bool editDeactivated = false;
		const bool colorPicker =
			first.aggregatePresentation == SceneSettingsManager::AggregatePresentation::ColorPicker;
		const float colorPickerWidth =
			colorPicker && !showExpandedAggregateControls ? ImGui::GetFrameHeight() : inputWidth;
		if (colorPicker && !showExpandedAggregateControls)
			ImGui::SetCursorPosX(
				ImGui::GetCursorPosX() + std::max(0.0f, (inputWidth - colorPickerWidth) * 0.5f));
		ImGui::BeginGroup();
		if (colorPicker) {
			ImGui::SetNextItemWidth(colorPickerWidth);
			ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
			if (std::any_of(orderedComponents.begin(),
					orderedComponents.begin() + static_cast<std::ptrdiff_t>(components.size()),
					[&](const Component* component) {
						return SceneSettingsManager::IsHDRColorSetting(
							entries[component->entryIndices.back()]);
					}))
				flags |= ImGuiColorEditFlags_HDR;
			if (!showExpandedAggregateControls)
				flags |= ImGuiColorEditFlags_NoInputs;
			changed = components.size() == 3 ?
			              ImGui::ColorEdit3("##val", values.data(), flags) :
			              ImGui::ColorEdit4("##val", values.data(), flags);
			editDeactivated = ImGui::IsItemDeactivatedAfterEdit();
		} else {
			std::array<double, 4> minimums{};
			std::array<double, 4> maximums{};
			std::array<double, 4> displayScales{};
			std::array<bool, 4> hasBounds{};
			for (size_t component = 0; component < components.size(); ++component) {
				const auto entryIndex = orderedComponents[component]->entryIndices.back();
				const auto& entry = entries[entryIndex];
				hasBounds[component] = SceneSettingsManager::GetNumericBounds(
					entry, minimums[component], maximums[component]);
				displayScales[component] = SceneSettingsManager::GetNumericDisplayScale(entry);
				double minimumDisplayValue = 0.0;
				double maximumDisplayValue = 0.0;
				hasBounds[component] = hasBounds[component] &&
				                       SceneSettingsManager::GetNumericDisplayValue(
										   entry, minimums[component], minimumDisplayValue) &&
				                       SceneSettingsManager::GetNumericDisplayValue(
										   entry, maximums[component], maximumDisplayValue) &&
				                       minimumDisplayValue <= maximumDisplayValue;
				minimums[component] = minimumDisplayValue;
				maximums[component] = maximumDisplayValue;
				double displayValue = 0.0;
				if (!SceneSettingsManager::GetNumericDisplayValue(entry, values[component], displayValue))
					displayValue = hasBounds[component] ? minimumDisplayValue : 0.0;
				values[component] = static_cast<float>(displayValue);
			}
			float unifiedValue = values[0];
			if (first.unifiedEditMode == SceneSettingsManager::UnifiedEditMode::Always)
				unifiedValue = std::accumulate(values.begin(), values.begin() + components.size(), 0.0f) /
				               static_cast<float>(components.size());
			auto drawUnifiedEditor = [&](float width) {
				const auto entryIndex = orderedComponents[0]->entryIndices.back();
				const auto& entry = entries[entryIndex];
				const float displayScale = static_cast<float>(displayScales[0]);
				const float minimumValue = hasBounds[0] ? static_cast<float>(minimums[0]) : 0.0f;
				const float maximumValue = hasBounds[0] ? static_cast<float>(maximums[0]) : 0.0f;
				const float speed = static_cast<float>(kSceneFloatDragSpeed * displayScale);
				const auto sliderFlags = SceneSettingsManager::IsNumericInputClamped(entry) ?
				                             ImGuiSliderFlags_AlwaysClamp :
				                             ImGuiSliderFlags_None;
				ImGui::SetNextItemWidth(width);
				const bool unifiedChanged = showExpandedAggregateControls && hasBounds[0] ?
				                                ImGui::SliderFloat("##all", &unifiedValue, minimumValue, maximumValue,
													displayScale == 1.0 ? "%.3f" : "%.1f", sliderFlags) :
				                                ImGui::DragFloat("##all", &unifiedValue, speed, minimumValue, maximumValue,
													displayScale == 1.0 ? "%.3f" : "%.1f", sliderFlags);
				changed |= unifiedChanged;
				editDeactivated |= ImGui::IsItemDeactivatedAfterEdit();
				if (unifiedChanged)
					std::fill_n(values.begin(), components.size(), unifiedValue);
			};
			auto drawComponentEditors = [&](float width) {
				const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
				const float componentWidth = std::max(
					(width - spacing * static_cast<float>(components.size() - 1)) /
						static_cast<float>(components.size()),
					1.0f);
				for (size_t component = 0; component < components.size(); ++component) {
					const auto entryIndex = orderedComponents[component]->entryIndices.back();
					const auto& entry = entries[entryIndex];
					const auto displayScale = displayScales[component];
					const float minimumValue = hasBounds[component] ?
					                               static_cast<float>(minimums[component]) :
					                               0.0f;
					const float maximumValue = hasBounds[component] ?
					                               static_cast<float>(maximums[component]) :
					                               0.0f;
					const float speed = static_cast<float>(kSceneFloatDragSpeed * displayScale);
					const auto sliderFlags = SceneSettingsManager::IsNumericInputClamped(entry) ?
					                             ImGuiSliderFlags_AlwaysClamp :
					                             ImGuiSliderFlags_None;
					const char* valueFormat = displayScale == 1.0 ? "%.3f" : "%.1f";
					if (component > 0)
						ImGui::SameLine(0.0f, spacing);
					ImGui::PushID(static_cast<int>(component));
					ImGui::SetNextItemWidth(componentWidth);
					ImGuiSliderFlags componentSliderFlags = sliderFlags;
					const auto colorChannelIndex =
						orderedComponents[component]->info.colorChannelIndex;
					const bool colorMarked = colorChannelIndex >= 0;
					if (colorMarked) {
						constexpr std::array colorChannels{
							Util::ColorChannel::Red,
							Util::ColorChannel::Green,
							Util::ColorChannel::Blue,
						};
						Util::SetNextItemColorMarker(colorChannels[colorChannelIndex]);
						componentSliderFlags |= ImGuiSliderFlags_ColorMarkers;
					}
					std::string format(valueFormat);
					if (!colorMarked && !orderedComponents[component]->info.componentDisplayName.empty()) {
						format.push_back(' ');
						for (const char ch : orderedComponents[component]->info.componentDisplayName) {
							format.push_back(ch);
							if (ch == '%')
								format.push_back('%');
						}
					}
					changed |= showExpandedAggregateControls && hasBounds[component] ?
					               ImGui::SliderFloat("##val", &values[component], minimumValue,
									   maximumValue, format.c_str(), componentSliderFlags) :
					               ImGui::DragFloat("##val", &values[component], speed, minimumValue,
									   maximumValue, format.c_str(), componentSliderFlags);
					editDeactivated |= ImGui::IsItemDeactivatedAfterEdit();
					ImGui::PopID();
				}
			};

			const bool persistentUnified =
				first.unifiedEditMode == SceneSettingsManager::UnifiedEditMode::Always;
			const bool shiftUnified = first.unifiedEditMode == SceneSettingsManager::UnifiedEditMode::Shift &&
			                          ImGui::GetIO().KeyShift;
			if (persistentUnified && showExpandedAggregateControls) {
				const float spacing = ImGui::GetStyle().ItemSpacing.x;
				const float unifiedWidth = ImGui::CalcTextSize(kNumericWidthSample).x +
				                           ImGui::GetStyle().FramePadding.x * 2.0f;
				drawUnifiedEditor(unifiedWidth);
				ImGui::SameLine(0.0f, spacing);
				drawComponentEditors(std::max(1.0f, inputWidth - unifiedWidth - spacing));
			} else if (persistentUnified || shiftUnified) {
				drawUnifiedEditor(inputWidth);
			} else {
				drawComponentEditors(inputWidth);
			}
		}
		ImGui::EndGroup();

		if (changed && !readOnly) {
			std::vector<SceneSettingsManager::EntryValueUpdate> updates;
			updates.reserve(indices.size());
			for (size_t component = 0; component < components.size(); ++component) {
				const auto& componentEntries = orderedComponents[component]->entryIndices;
				const auto& entry = entries[componentEntries.back()];
				double storedValue = values[component];
				const bool validValue = SceneSettingsManager::GetNumericStoredValue(
					entry, values[component], storedValue);
				if (validValue)
					for (const auto entryIndex : orderedComponents[component]->entryIndices)
						updates.push_back({ entryIndex, json(storedValue) });
			}
			if (updates.size() == indices.size())
				update(updates);
		}
		if (!readOnly && editDeactivated)
			commit();
		if (readOnly)
			ImGui::EndDisabled();
		return true;
	}

	void DrawValueEditor(SceneType type, size_t index, float inputWidth, bool readOnly)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entry = manager->GetEntries(type)[index];
		DrawValueEditorCore(entry, inputWidth, type == SceneType::InteriorOnly, readOnly, [=](const json& v) { manager->UpdateEntryValue(type, index, v, true); }, [=]() { manager->SaveAllUserSettings(); });
	}

	static void DrawValueEditor(
		SceneType type, const std::vector<size_t>& indices, float inputWidth, bool readOnly)
	{
		if (indices.empty())
			return;
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetEntries(type);
		if (DrawAggregateValueEditorCore(entries, indices, inputWidth, type == SceneType::InteriorOnly, readOnly, [=](const auto& updates) { manager->UpdateEntryValues(type, updates, true); }, [=] { manager->CommitSceneSettingChanges(); }))
			return;
		DrawValueEditor(type, indices.back(), inputWidth, readOnly);
	}

	void DrawWeatherValueEditor(RE::FormID weatherId, size_t index, float inputWidth, bool readOnly)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entry = manager->GetWeatherConfig(weatherId).entries[index];
		DrawValueEditorCore(entry, inputWidth, false, readOnly, [=](const json& v) { manager->UpdateWeatherEntryValue(weatherId, index, v, true); }, [=]() { manager->SaveAllUserSettings(); });
	}

	void DrawWeatherValueEditor(RE::FormID weatherId, const std::vector<size_t>& indices, float inputWidth, bool readOnly)
	{
		if (indices.empty())
			return;
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetWeatherConfig(weatherId).entries;
		if (DrawAggregateValueEditorCore(entries, indices, inputWidth, false, readOnly, [=](const auto& updates) { manager->UpdateWeatherEntryValues(weatherId, updates, true); }, [=] { manager->SaveAllUserSettings(); }))
			return;
		const auto& entry = entries[indices.back()];
		DrawValueEditorCore(entry, inputWidth, false, readOnly, [=](const json& value) {
				std::vector<SceneSettingsManager::EntryValueUpdate> updates;
				updates.reserve(indices.size());
				for (const auto index : indices)
					updates.push_back({ index, value });
				manager->UpdateWeatherEntryValues(weatherId, updates, true); }, [=]() { manager->SaveAllUserSettings(); });
	}

	void DrawPopups(SceneType type, PopupState& popups)
	{
		auto* manager = SceneSettingsManager::GetSingleton();

		if (popups.deleteAllOverwrites.Draw())
			manager->DeleteAllOverwrites(type);

		if (popups.deleteSingleOverwrite.Draw()) {
			if (popups.pendingDeleteIndex < manager->GetEntries(type).size())
				manager->RemoveSetting(type, popups.pendingDeleteIndex);
			popups.pendingDeleteIndex = SIZE_MAX;
		}

		if (popups.deleteRowOverwrite.Draw()) {
			RemoveIndicesReversed(popups.pendingDeleteRow, [&](size_t idx) {
				if (idx < manager->GetEntries(type).size())
					manager->RemoveSetting(type, idx);
			});
			popups.pendingDeleteRow.clear();
		}

		if (popups.deleteAllUser.Draw())
			manager->DeleteAllUserSettings(type);
	}

	static void ConfigurePopups(PopupState& popups, const char* overwriteMessage, const char* userMessage)
	{
		const auto* deleteLabel = T("feature.scene_manager.action.delete", "Delete");
		const auto* deleteAllLabel = T("feature.scene_manager.action.delete_all", "Delete All");
		const auto* cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");

		popups.deleteAllOverwrites.title = T("feature.scene_manager.confirm.delete_all_overwrites_title", "Delete All Overwrites?");
		popups.deleteAllOverwrites.message = overwriteMessage;
		popups.deleteAllOverwrites.confirmLabel = deleteAllLabel;
		popups.deleteAllOverwrites.cancelLabel = cancelLabel;

		popups.deleteSingleOverwrite.title = T("feature.scene_manager.confirm.delete_overwrite_file_title", "Delete Overwrite File?");
		popups.deleteSingleOverwrite.confirmLabel = deleteLabel;
		popups.deleteSingleOverwrite.cancelLabel = cancelLabel;

		popups.deleteRowOverwrite.title = T("feature.scene_manager.confirm.delete_overwrite_row_title", "Delete Overwrite Row?");
		popups.deleteRowOverwrite.confirmLabel = deleteLabel;
		popups.deleteRowOverwrite.cancelLabel = cancelLabel;

		popups.deleteAllUser.title = T("feature.scene_manager.confirm.delete_all_user_title", "Delete All User Settings?");
		popups.deleteAllUser.message = userMessage;
		popups.deleteAllUser.confirmLabel = deleteAllLabel;
		popups.deleteAllUser.cancelLabel = cancelLabel;
	}

	static int GetSettingLabelMaxLines()
	{
		return static_cast<int>(C::SCENE_SETTING_MAX_LINES);
	}

	static std::string GetSettingLabel(const SettingId& setting)
	{
		return setting.displayName.empty() ? SceneSettingsManager::GetSettingDisplayName(setting.key) : setting.displayName;
	}

	static float GetSettingLabelVisualHeight()
	{
		return ImGui::GetTextLineHeight() * C::SCENE_SETTING_MAX_LINES;
	}

	static std::string TruncateTextToFitWidth(std::string text, float width)
	{
		if (text.empty() || width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= width)
			return text;

		size_t visibleLen = text.size();
		while (visibleLen > 0 &&
			   ImGui::CalcTextSize((text.substr(0, visibleLen) + kEllipsis).c_str()).x > width)
			visibleLen = PreviousUtf8CodepointBoundary(text, visibleLen);
		return text.substr(0, visibleLen) + kEllipsis;
	}

	static std::string TruncateTextMiddleToFitWidth(std::string text, float width)
	{
		if (text.empty() || width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= width)
			return text;

		std::vector<size_t> boundaries{ 0 };
		for (size_t offset = 0; offset < text.size();) {
			++offset;
			while (offset < text.size() &&
				   (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80)
				++offset;
			boundaries.push_back(offset);
		}
		for (size_t visible = boundaries.size() - 1; visible > 0; --visible) {
			const auto prefixCount = (visible + 1) / 2;
			const auto suffixCount = visible / 2;
			auto candidate = text.substr(0, boundaries[prefixCount]) + kEllipsis +
			                 text.substr(boundaries[boundaries.size() - 1 - suffixCount]);
			if (ImGui::CalcTextSize(candidate.c_str()).x <= width)
				return candidate;
		}
		return ImGui::CalcTextSize(kEllipsis).x <= width ? std::string(kEllipsis) : std::string{};
	}

	static std::string TruncateWrappedTextToLines(std::string text, float wrapWidth, int maxLines)
	{
		assert(maxLines > 0);
		if (text.empty() || wrapWidth <= 0.0f)
			return text;

		const float fixedH = ImGui::GetTextLineHeight() * maxLines;
		if (ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapWidth).y <= fixedH + kLabelOverflowTolerance)
			return text;

		auto* font = ImGui::GetFont();
		const float fontSize = ImGui::GetFontSize();
		const char* textBegin = text.c_str();
		const char* textEnd = textBegin + text.size();
		if (maxLines == 1)
			return TruncateTextMiddleToFitWidth(std::move(text), wrapWidth);

		const char* lineStart = textBegin;
		for (int line = 0; line < maxLines - 1 && lineStart < textEnd; ++line) {
			const char* nextLine = font->CalcWordWrapPosition(fontSize, lineStart, textEnd, wrapWidth);
			if (nextLine <= lineStart)
				break;
			lineStart = nextLine;
		}

		auto prefix = text.substr(0, static_cast<size_t>(lineStart - textBegin));
		while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back())))
			prefix.pop_back();
		auto suffix = text.substr(static_cast<size_t>(lineStart - textBegin));
		suffix.erase(0, suffix.find_first_not_of(" \t\r\n"));
		return std::format("{}\n{}", prefix,
			TruncateTextMiddleToFitWidth(std::move(suffix), wrapWidth));
	}

	static ImRect DrawSettingLabel(const SettingId& setting)
	{
		const float wrapWidth = ImGui::GetContentRegionAvail().x;
		const float fixedH = GetSettingLabelVisualHeight();

		if (setting.parentPath.empty()) {
			auto text = TruncateWrappedTextToLines(GetSettingLabel(setting), wrapWidth, GetSettingLabelMaxLines());
			ImGui::TextWrapped("%s", text.c_str());
		} else {
			auto parent = TruncateTextToFitWidth(JoinDisplayParts(setting.parentPath), wrapWidth);
			auto leaf = TruncateTextMiddleToFitWidth(GetSettingLabel(setting), wrapWidth);
			auto text = std::format("{}\n{}", parent, leaf);
			ImGui::TextUnformatted(text.c_str());
		}

		const ImRect labelRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		const float usedH = ImGui::GetItemRectSize().y;
		if (usedH < fixedH) {
			const float pad = fixedH - usedH - ImGui::GetStyle().ItemSpacing.y;
			if (pad > 0.0f)
				ImGui::Dummy(ImVec2(0, pad));
		}
		return labelRect;
	}

	static bool IsCheckboxOnlyRow(const SourceRow& row,
		const std::vector<SettingEntry>& entries)
	{
		if (row.indices.empty())
			return false;
		return std::all_of(row.indices.begin(), row.indices.end(), [&](const size_t index) {
			if (index >= entries.size() || SceneSettingsManager::GetSettingChoiceCount(entries[index]) > 0)
				return false;
			return SceneSettingsManager::IsBooleanControlSetting(entries[index]) ||
			       SceneSettingsManager::DetectSettingType(entries[index].value) ==
			           SceneSettingsManager::SettingType::Boolean;
		});
	}

	static bool IsCheckboxOnlyGroup(const SourceGroup& group,
		const std::vector<SettingEntry>& entries)
	{
		return !group.rows.empty() && std::ranges::all_of(group.rows,
										  [&](const auto& row) { return IsCheckboxOnlyRow(row, entries); });
	}

	static bool HasInlineActionColumn(int numValueColumns)
	{
		return numValueColumns == 1;
	}

	static float GetActionsColumnWidth()
	{
		const float controlWidth = GetCompactFeatureToggleSize().x +
		                           GetSceneActionButtonSize() * 2.0f +
		                           ImGui::GetStyle().ItemSpacing.x * kActionControlSpacingCount;
		return std::max(C::Em(kActionsColumnMinWidthEm), controlWidth);
	}

	static float GetMinimumValueColumnWidth(int numValueColumns)
	{
		const float baseWidth = C::Em(C::SCENE_TOD_PERIOD_COL_EM);
		return numValueColumns > 1 ? baseWidth : baseWidth * kSingleValueColumnScale;
	}

	static float GetExpandedValueColumnWidth()
	{
		float componentWidth = 0.0f;
		for (const char* component : { "R", "G", "B", "A" }) {
			const auto sample = std::format("{} {}", kNumericWidthSample, component);
			componentWidth = std::max(componentWidth,
				ImGui::CalcTextSize(sample.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f);
		}
		return componentWidth * 4.0f + ImGui::GetStyle().ItemInnerSpacing.x * 4.0f +
		       ImGui::GetFrameHeight();
	}

	static float MeasureScalarEditorWidth(const SettingEntry& entry, float minimumWidth)
	{
		float width = minimumWidth;
		const auto choiceCount = SceneSettingsManager::GetSettingChoiceCount(entry);
		for (size_t choiceIndex = 0; choiceIndex < choiceCount; ++choiceIndex) {
			std::int64_t choiceValue = 0;
			std::string choiceName;
			if (SceneSettingsManager::GetSettingChoice(entry, choiceIndex, choiceValue, choiceName))
				width = std::max(width, ImGui::CalcTextSize(choiceName.c_str()).x +
											ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight());
		}
		if (entry.value.is_string()) {
			width = std::max(width, C::Em(kStringEditorMaxWidthEm));
		}
		return width;
	}

	static float MeasureValueEditorWidth(const std::vector<SettingEntry>& entries,
		const std::vector<size_t>& indices, float minimumWidth, bool showExpandedAggregateControls)
	{
		float width = minimumWidth;
		std::map<std::int8_t, SceneSettingsManager::SettingControlInfo> components;
		for (const auto index : indices) {
			if (index >= entries.size())
				continue;
			width = std::max(width, MeasureScalarEditorWidth(entries[index], minimumWidth));
			SceneSettingsManager::SettingControlInfo info;
			if (!SceneSettingsManager::GetSettingControlInfo(entries[index], info) ||
				info.controlType == SceneSettingControlType::Scalar)
				continue;
			components.try_emplace(info.componentIndex, std::move(info));
		}
		if (components.size() < 2)
			return width;

		const auto& first = components.begin()->second;
		if (components.size() != first.componentCount)
			return width;
		for (size_t component = 0; component < components.size(); ++component) {
			auto componentIt = components.find(static_cast<std::int8_t>(first.componentStart + component));
			if (componentIt == components.end())
				return width;
			const auto& info = componentIt->second;
			if (info.settingPath != first.settingPath || info.settingKey != first.settingKey ||
				info.componentStart != first.componentStart || info.componentCount != first.componentCount ||
				info.controlType != first.controlType ||
				info.aggregatePresentation != first.aggregatePresentation ||
				info.unifiedEditMode != first.unifiedEditMode)
				return width;
		}
		const bool collapsedAggregate = !showExpandedAggregateControls &&
		                                (first.aggregatePresentation == SceneSettingsManager::AggregatePresentation::ColorPicker ||
											first.unifiedEditMode == SceneSettingsManager::UnifiedEditMode::Always);
		if (collapsedAggregate)
			return width;

		float componentWidth = ImGui::CalcTextSize(kNumericWidthSample).x +
		                       ImGui::GetStyle().FramePadding.x * 2.0f;
		for (const auto& [component, info] : components) {
			const bool colorMarked = info.colorChannelIndex >= 0;
			const auto sample = colorMarked || info.componentDisplayName.empty() ?
			                        std::string(kNumericWidthSample) :
			                        std::format("{} {}", kNumericWidthSample, info.componentDisplayName);
			componentWidth = std::max(componentWidth,
				ImGui::CalcTextSize(sample.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f);
		}
		float aggregateWidth = componentWidth * static_cast<float>(components.size()) +
		                       ImGui::GetStyle().ItemInnerSpacing.x * static_cast<float>(components.size() - 1);
		if (first.aggregatePresentation == SceneSettingsManager::AggregatePresentation::ColorPicker)
			aggregateWidth += ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
		if (first.unifiedEditMode == SceneSettingsManager::UnifiedEditMode::Always &&
			showExpandedAggregateControls)
			aggregateWidth += ImGui::CalcTextSize(kNumericWidthSample).x +
			                  ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
		return std::max(width, aggregateWidth);
	}

	static SourceTableLayout GetSourceTableLayout(const SourceGroup& group,
		const std::vector<SettingEntry>& entries, int numValueColumns,
		bool showExpandedAggregateControls = false, float auxiliaryColumnWidth = 0.0f)
	{
		SourceTableLayout layout;
		layout.numValueColumns = std::clamp(numValueColumns, 1, kPeriodCount);
		layout.auxiliaryColumnWidth = std::max(0.0f, auxiliaryColumnWidth);
		const bool multiColumn = layout.numValueColumns > 1;
		layout.checkboxOnlyValueColumn = !multiColumn && IsCheckboxOnlyGroup(group, entries);
		const float regularMinimumWidth = !multiColumn && showExpandedAggregateControls ?
		                                      std::max(GetMinimumValueColumnWidth(layout.numValueColumns),
												  GetExpandedValueColumnWidth()) :
		                                      GetMinimumValueColumnWidth(layout.numValueColumns);
		const float minimumWidth = layout.checkboxOnlyValueColumn ?
		                               ImGui::GetFrameHeight() :
		                               regularMinimumWidth;
		std::fill_n(layout.valueColumnWidths.begin(), layout.numValueColumns, minimumWidth);
		if (!multiColumn && !layout.checkboxOnlyValueColumn)
			for (const auto& row : group.rows)
				layout.valueColumnWidths[0] = std::max(layout.valueColumnWidths[0],
					MeasureValueEditorWidth(entries, row.indices, minimumWidth, showExpandedAggregateControls));

		auto& style = ImGui::GetStyle();
		const bool inlineActions = HasInlineActionColumn(layout.numValueColumns);
		const bool hasAuxiliaryColumn = layout.auxiliaryColumnWidth > 0.0f;
		const int totalColumns = 1 + layout.numValueColumns + (hasAuxiliaryColumn ? 1 : 0) +
		                         (inlineActions ? 1 : 0);
		float columnWidth = C::Em(C::SCENE_TOD_PARAM_COL_EM);
		for (int column = 0; column < layout.numValueColumns; ++column)
			columnWidth += layout.valueColumnWidths[column];
		if (hasAuxiliaryColumn)
			columnWidth += layout.auxiliaryColumnWidth;
		if (inlineActions)
			columnWidth += GetActionsColumnWidth();
		layout.sectionWidth = columnWidth + totalColumns * style.CellPadding.x * 2.0f +
		                      (totalColumns + 1) * kTableBorderWidth;
		return layout;
	}

	struct CachedSourceTable
	{
		SourceGroup group;
		SourceTableLayout layout;
		std::vector<size_t> indices;
		bool allPaused = false;
	};

	struct SourcePanelCache
	{
		std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
		const std::vector<SettingEntry>* entries = nullptr;
		std::string locale;
		const ImFont* font = nullptr;
		float fontSize = 0.0f;
		ImVec2 framePadding{};
		ImVec2 itemSpacing{};
		ImVec2 itemInnerSpacing{};
		ImVec2 cellPadding{};
		int numValueColumns = 0;
		bool showExpandedAggregateControls = false;
		bool transitionOnly = false;
		std::optional<bool> periodMode;
		float auxiliaryColumnWidth = 0.0f;
		CachedSourceTable overwrite;
		CachedSourceTable user;
		std::vector<uint8_t> overriddenEntries;
		bool hasActiveOverrides = false;
	};

	static bool EqualStyleMetric(const ImVec2& lhs, const ImVec2& rhs)
	{
		return lhs.x == rhs.x && lhs.y == rhs.y;
	}

	static void RefreshSourcePanelCache(SourcePanelCache& cache,
		const std::vector<SettingEntry>& entries, int numValueColumns,
		bool showExpandedAggregateControls = false, bool transitionOnly = false,
		float auxiliaryColumnWidth = 0.0f, std::optional<bool> periodMode = std::nullopt)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto revision = manager->GetEntryPresentationRevision();
		const auto locale = I18n::GetSingleton()->GetCurrentLocale();
		const auto* font = ImGui::GetFont();
		const auto fontSize = ImGui::GetFontSize();
		const auto& style = ImGui::GetStyle();
		const int clampedColumns = std::clamp(numValueColumns, 1, kPeriodCount);
		if (cache.revision == revision && cache.entries == &entries && cache.locale == locale &&
			cache.font == font && cache.fontSize == fontSize &&
			EqualStyleMetric(cache.framePadding, style.FramePadding) &&
			EqualStyleMetric(cache.itemSpacing, style.ItemSpacing) &&
			EqualStyleMetric(cache.itemInnerSpacing, style.ItemInnerSpacing) &&
			EqualStyleMetric(cache.cellPadding, style.CellPadding) &&
			cache.numValueColumns == clampedColumns &&
			cache.showExpandedAggregateControls == showExpandedAggregateControls &&
			cache.transitionOnly == transitionOnly && cache.periodMode == periodMode &&
			cache.auxiliaryColumnWidth == auxiliaryColumnWidth)
			return;

		const bool multiColumn = clampedColumns > 1;
		cache.overwrite.group = BuildSourceGroup(
			entries, EntrySource::Overwrite, true, transitionOnly, multiColumn, periodMode);
		cache.user.group = BuildSourceGroup(
			entries, EntrySource::User, true, transitionOnly, multiColumn, periodMode);
		cache.overwrite.layout = GetSourceTableLayout(
			cache.overwrite.group, entries, clampedColumns, showExpandedAggregateControls,
			auxiliaryColumnWidth);
		cache.user.layout = GetSourceTableLayout(
			cache.user.group, entries, clampedColumns, showExpandedAggregateControls,
			auxiliaryColumnWidth);
		if (!multiColumn &&
			cache.overwrite.layout.checkboxOnlyValueColumn ==
				cache.user.layout.checkboxOnlyValueColumn) {
			const float commonValueWidth = std::max(
				cache.overwrite.layout.valueColumnWidths[0], cache.user.layout.valueColumnWidths[0]);
			for (auto* layout : { &cache.overwrite.layout, &cache.user.layout }) {
				layout->sectionWidth += commonValueWidth - layout->valueColumnWidths[0];
				layout->valueColumnWidths[0] = commonValueWidth;
			}
		}
		cache.overwrite.indices.clear();
		cache.user.indices.clear();
		SplitBySource(entries, cache.overwrite.indices, cache.user.indices, transitionOnly, periodMode);
		cache.overwrite.allPaused = AreAllPaused(cache.overwrite.indices, entries);
		cache.user.allPaused = AreAllPaused(cache.user.indices, entries);

		const auto overrides = BuildActiveOverrideSet(entries);
		cache.overriddenEntries.assign(entries.size(), 0);
		cache.hasActiveOverrides = false;
		for (size_t index = 0; index < entries.size(); ++index) {
			if ((periodMode && (entries[index].period != Period::Count) != *periodMode) ||
				entries[index].source != EntrySource::User || !IsOverridden(overrides, entries[index]))
				continue;
			cache.overriddenEntries[index] = 1;
			cache.hasActiveOverrides = true;
		}

		cache.revision = revision;
		cache.entries = &entries;
		cache.locale = locale;
		cache.font = font;
		cache.fontSize = fontSize;
		cache.framePadding = style.FramePadding;
		cache.itemSpacing = style.ItemSpacing;
		cache.itemInnerSpacing = style.ItemInnerSpacing;
		cache.cellPadding = style.CellPadding;
		cache.numValueColumns = clampedColumns;
		cache.showExpandedAggregateControls = showExpandedAggregateControls;
		cache.transitionOnly = transitionOnly;
		cache.periodMode = periodMode;
		cache.auxiliaryColumnWidth = auxiliaryColumnWidth;
	}

	static void CenterCursorY(float rowContentHeight, float itemHeight)
	{
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (rowContentHeight - itemHeight) * 0.5f));
	}

	static void DrawInlineControls(const std::vector<size_t>& indices, bool isOverwrite, PopupState* popups,
		const std::vector<SettingEntry>& entries, const TableCallbacks& cb)
	{
		if (indices.empty())
			return;

		const bool allPaused = AreAllPaused(indices, entries);
		auto result = DrawFlyoutControls(allPaused, indices.size() > 1, isOverwrite);
		ApplyGroupControlResult(result, indices, allPaused, isOverwrite, popups, entries, cb, nullptr);
	}

	void DrawSourceTable(
		const SourceGroup& group,
		const std::vector<SceneSettingsManager::SettingEntry>& entries,
		const char* tableId,
		EntrySource source,
		const SourceTableLayout& layout,
		PopupState* popups,
		TableFlyoutState& flyout,
		const TableCallbacks& cb,
		std::span<const uint8_t> overriddenEntries)
	{
		const int numValueColumns = layout.numValueColumns;
		bool isOverwrite = source == EntrySource::Overwrite;
		bool multiColumn = numValueColumns > 1;
		bool inlineActions = HasInlineActionColumn(numValueColumns);
		const bool hasAuxiliaryColumn = layout.auxiliaryColumnWidth > 0.0f &&
		                                cb.auxiliaryColumnLabel && cb.drawAuxiliary;
		if (flyout.transientGeneration != s_transientGeneration) {
			Util::CloseFlyout(flyout.cell);
			Util::CloseFlyout(flyout.row);
			Util::CloseFlyout(flyout.col);
			flyout.cellSourceRow = SIZE_MAX;
			flyout.rowSourceRow = SIZE_MAX;
			flyout.transientGeneration = s_transientGeneration;
		}
		const bool suppressFlyouts = s_addDialogFrame == ImGui::GetFrameCount();
		if (suppressFlyouts) {
			Util::CloseFlyout(flyout.cell);
			Util::CloseFlyout(flyout.row);
			Util::CloseFlyout(flyout.col);
			flyout.cellSourceRow = SIZE_MAX;
			flyout.rowSourceRow = SIZE_MAX;
		}
		const auto flyoutStyle = GetSceneFlyoutStyle();
		constexpr int kSettingColumn = 0;
		constexpr int kFirstValueColumn = 1;
		const int auxiliaryColumn = kFirstValueColumn + numValueColumns;
		const int actionsColumn = auxiliaryColumn + (hasAuxiliaryColumn ? 1 : 0);
		int totalCols = actionsColumn + (inlineActions ? 1 : 0);
		struct PendingAddPeriod
		{
			std::string feature;
			std::vector<std::string> path;
			std::string key;
			int period;
		};
		std::vector<size_t> pendingRemoveIndices;
		std::vector<PendingAddPeriod> pendingAddPeriods;
		auto deferredCallbacks = cb;
		deferredCallbacks.remove = [&](size_t index) {
			if (std::find(pendingRemoveIndices.begin(), pendingRemoveIndices.end(), index) == pendingRemoveIndices.end())
				pendingRemoveIndices.push_back(index);
		};
		if (cb.onAddPeriod) {
			deferredCallbacks.onAddPeriod = [&](const std::string& feature, const std::vector<std::string>& path,
												const std::string& key, int period) {
				pendingAddPeriods.push_back({ feature, path, key, period });
			};
		}

		const ImGuiTableFlags tableFlags =
			ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
		if (!ImGui::BeginTable(tableId, totalCols, tableFlags))
			return;

		// Column setup
		ImGui::TableSetupColumn(T("feature.scene_manager.column.setting", "Setting"),
			ImGuiTableColumnFlags_WidthFixed, C::Em(C::SCENE_TOD_PARAM_COL_EM));
		if (multiColumn) {
			for (int i = 0; i < numValueColumns; ++i)
				ImGui::TableSetupColumn(GetPeriodDisplayName(static_cast<Period>(i)),
					ImGuiTableColumnFlags_WidthFixed, layout.valueColumnWidths[i]);
		} else {
			ImGui::TableSetupColumn(T("feature.scene_manager.column.value", "Value"), ImGuiTableColumnFlags_WidthFixed,
				layout.valueColumnWidths[0]);
		}
		if (hasAuxiliaryColumn)
			ImGui::TableSetupColumn(cb.auxiliaryColumnLabel,
				ImGuiTableColumnFlags_WidthFixed, layout.auxiliaryColumnWidth);
		if (inlineActions)
			ImGui::TableSetupColumn(T("feature.scene_manager.column.actions", "Actions"),
				ImGuiTableColumnFlags_WidthFixed, GetActionsColumnWidth());

		if (multiColumn || hasAuxiliaryColumn) {
			// Header row
			auto* table = GImGui->CurrentTable;
			if (!table->IsLayoutLocked)
				ImGui::TableUpdateLayout(table);
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers, ImGui::TableGetHeaderRowHeight());
			ImGui::TableSetColumnIndex(kSettingColumn);
			ImGui::TableHeader(T("feature.scene_manager.column.setting", "Setting"));

			if (!multiColumn) {
				ImGui::TableSetColumnIndex(kFirstValueColumn);
				ImGui::TableHeader(T("feature.scene_manager.column.value", "Value"));
			}
			for (int i = 0; multiColumn && i < numValueColumns; ++i) {
				const bool columnVisible = ImGui::TableSetColumnIndex(kFirstValueColumn + i);
				if (!columnVisible && !flyout.col.isOpen)
					continue;
				const ImVec2 contentMin = ImGui::GetCursorScreenPos();
				float colW = ImGui::GetContentRegionAvail().x;
				ImGui::TextUnformatted(GetPeriodDisplayName(static_cast<Period>(i)));
				const ImVec2 contentMax(contentMin.x + colW, ImGui::GetItemRectMax().y);
				const ImRect cellRect = ImGui::TableGetCellBgRect(table, kFirstValueColumn + i);

				const auto& indices = group.perColumn[i];
				if (!indices.empty() && !suppressFlyouts) {
					ImGui::PushID(i);
					const auto flyoutSource = SubmitFlyoutSource("##colFlyout", contentMin, contentMax);
					{
						Util::FlyoutScope flyoutScope(
							flyout.col, flyoutSource.id, flyoutSource.pressed,
							cellRect.Min, cellRect.Max, flyoutStyle);
						if (flyoutScope) {
							bool allPaused = AreAllPaused(indices, entries);
							auto result = DrawFlyoutControls(allPaused, true, isOverwrite);
							ApplyGroupControlResult(result, indices, allPaused, isOverwrite,
								popups, entries, deferredCallbacks, &flyout.col);
						}
					}
					ImGui::PopID();
				}
			}
			if (hasAuxiliaryColumn) {
				ImGui::TableSetColumnIndex(auxiliaryColumn);
				ImGui::TableHeader(cb.auxiliaryColumnLabel);
			}
			if (inlineActions) {
				ImGui::TableSetColumnIndex(actionsColumn);
				ImGui::TableHeader(T("feature.scene_manager.column.actions", "Actions"));
			}
		}

		auto& theme = globals::menu->GetSettings().Theme;
		const auto isOverridden = [&](size_t index) {
			return index < overriddenEntries.size() && overriddenEntries[index] != 0;
		};
		const auto drawRow = [&](const SourceRow& row, size_t rowIndex) {
			const auto& sid = row.setting;
			const auto& rowIndices = row.indices;

			const float controlHeight = ImGui::GetFrameHeight();
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(kSettingColumn);

			ImGui::PushID(sid.key.c_str());
			ImGui::PushID(static_cast<int>(sid.componentStart));
			for (const auto& part : sid.path)
				ImGui::PushID(part.c_str());
			ImGui::PushID(sid.feature.c_str());

			ImGui::Indent(C::Em(C::SCENE_ENTRY_INDENT_EM));
			ImGui::SetWindowFontScale(C::SCENE_TOD_FEATURE_TEXT_SCALE);
			const auto labelRect = DrawSettingLabel(sid);
			const float labelVisualH = GetSettingLabelVisualHeight();
			ImGui::SetWindowFontScale(1.0f);
			const float rowVisualH = std::max(labelVisualH, controlHeight);

			// Row-level flyout (only for multi-column to avoid duplicate controls)
			if (multiColumn && !suppressFlyouts) {
				const auto flyoutSource = SubmitFlyoutSource("##rowFlyout", labelRect.Min, labelRect.Max);
				{
					Util::FlyoutScope flyoutScope(
						flyout.row, flyoutSource.id, flyoutSource.pressed, flyoutStyle);
					if (flyout.row.isOpen && flyout.row.activeId == flyoutSource.id)
						flyout.rowSourceRow = rowIndex;
					if (flyoutScope) {
						bool allPaused = AreAllPaused(rowIndices, entries);
						auto result = DrawFlyoutControls(allPaused, true, isOverwrite);
						ApplyGroupControlResult(result, rowIndices, allPaused, isOverwrite,
							popups, entries, deferredCallbacks, &flyout.row);
					}
				}
			}

			ImGui::Unindent(C::Em(C::SCENE_ENTRY_INDENT_EM));

			// Value columns
			if (multiColumn) {
				for (int p = 0; p < numValueColumns; ++p) {
					const bool columnVisible = ImGui::TableSetColumnIndex(kFirstValueColumn + p);
					const auto& cellIndices = row.cells[p];
					if (!columnVisible) {
						bool ownsActiveFlyout = false;
						if (flyout.cell.isOpen && !cellIndices.empty()) {
							ImGui::PushID(p);
							ownsActiveFlyout = flyout.cell.activeId == ImGui::GetID("##cellFlyout");
							ImGui::PopID();
						}
						if (!ownsActiveFlyout)
							continue;
					}

					if (cellIndices.empty()) {
						if (source == EntrySource::User && deferredCallbacks.onAddPeriod) {
							ImGui::PushID(p);
							const float btnSz = C::Em(C::SCENE_ADD_PERIOD_BTN_EM);
							const float cellW = ImGui::GetContentRegionAvail().x;
							ImGui::SetCursorPos(ImVec2(
								ImGui::GetCursorPosX() + std::max(0.f, (cellW - btnSz) * 0.5f),
								ImGui::GetCursorPosY() + std::max(0.f, (rowVisualH - btnSz) * 0.5f)));
							if (DrawSceneAddButton("##addPeriod", btnSz)) {
								for (const auto sourceIndex : row.addPeriodSourceIndices) {
									if (sourceIndex >= entries.size())
										continue;
									const auto& sourceEntry = entries[sourceIndex];
									deferredCallbacks.onAddPeriod(
										sourceEntry.featureShortName, sourceEntry.settingPath,
										sourceEntry.settingKey, p);
								}
							}
							ImGui::PopID();
						} else {
							ImGui::TextDisabled("--");
						}
						continue;
					}

					const auto entryIndex = cellIndices.front();
					const auto& entry = entries[entryIndex];
					ImGui::PushID(p);
					const bool allPaused = AreAllPaused(cellIndices, entries);
					CenterCursorY(rowVisualH, ImGui::GetFrameHeight());

					if (allPaused)
						ImGui::BeginDisabled();

					const bool cellOverridden = std::any_of(
						cellIndices.begin(), cellIndices.end(),
						[&](const size_t index) { return isOverridden(index); });
					if (cellOverridden) {
						const auto& ec = theme.StatusPalette.Error;
						ImGui::PushStyleColor(ImGuiCol_Text, ec);
						ImGui::PushStyleColor(ImGuiCol_CheckMark, ec);
					}

					if (cellIndices.size() > 1 && deferredCallbacks.drawEditorMulti)
						deferredCallbacks.drawEditorMulti(
							cellIndices, ImGui::GetContentRegionAvail().x, entry.source == EntrySource::Overwrite);
					else
						deferredCallbacks.drawEditor(
							entryIndex, ImGui::GetContentRegionAvail().x, entry.source == EntrySource::Overwrite);

					if (cellOverridden)
						ImGui::PopStyleColor(2);

					if (allPaused)
						ImGui::EndDisabled();

					// Cell flyout
					const ImVec2 flyoutSourceMin = ImGui::GetItemRectMin();
					const ImVec2 flyoutSourceMax = ImGui::GetItemRectMax();
					const bool sourcePressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
					const ImGuiID cellId = ImGui::GetID("##cellFlyout");
					if (!suppressFlyouts) {
						Util::FlyoutScope flyoutScope(
							flyout.cell, cellId, sourcePressed, flyoutSourceMin, flyoutSourceMax, flyoutStyle);
						if (flyout.cell.isOpen && flyout.cell.activeId == cellId)
							flyout.cellSourceRow = rowIndex;
						if (flyoutScope) {
							auto result = DrawFlyoutControls(allPaused, cellIndices.size() > 1, isOverwrite);

							if (result.toggled) {
								for (const auto index : cellIndices)
									if (entries[index].paused == allPaused)
										deferredCallbacks.togglePause(index);
							}
							if (result.reverted)
								for (const auto index : cellIndices)
									deferredCallbacks.revert(index);
							if (result.deleted) {
								if (popups && entry.source == EntrySource::Overwrite) {
									if (cellIndices.size() > 1) {
										RequestOverwriteRowDelete(*popups, entries, cellIndices);
									} else {
										popups->pendingDeleteIndex = entryIndex;
										popups->deleteSingleOverwrite.message = std::vformat(
											T("feature.scene_manager.confirm.delete_overwrite_entry",
												"Delete overwrite entry from '{0}'?\nThe file will be removed if no settings remain."),
											std::make_format_args(entry.sourceFilename));
										popups->deleteSingleOverwrite.Request();
									}
									Util::RequestCloseFlyout(flyout.cell);
								} else {
									for (const auto index : cellIndices)
										deferredCallbacks.remove(index);
									Util::CloseFlyout(flyout.cell);
								}
							}
						}
					}

					ImGui::PopID();
				}
			} else {
				// Single-column: collapsed view of all entries for this key
				ImGui::TableSetColumnIndex(kFirstValueColumn);
				CenterCursorY(rowVisualH, ImGui::GetFrameHeight());

				if (rowIndices.empty()) {
					ImGui::TextDisabled("--");
					if (inlineActions) {
						ImGui::TableSetColumnIndex(actionsColumn);
						CenterCursorY(rowVisualH, ImGui::GetFrameHeight());
						ImGui::TextDisabled("--");
					}
				} else {
					size_t displayIndex = rowIndices[0];
					bool anyPaused = std::any_of(rowIndices.begin(), rowIndices.end(),
						[&](size_t i) { return i < entries.size() && entries[i].paused; });

					ImGui::PushID(static_cast<int>(displayIndex));

					bool rowOverridden = std::any_of(rowIndices.begin(), rowIndices.end(),
						[&](size_t i) { return isOverridden(i); });

					if (anyPaused)
						ImGui::BeginDisabled();

					if (rowOverridden) {
						const auto& ec = theme.StatusPalette.Error;
						ImGui::PushStyleColor(ImGuiCol_Text, ec);
						ImGui::PushStyleColor(ImGuiCol_CheckMark, ec);
					}

					if (deferredCallbacks.drawEditorMulti)
						deferredCallbacks.drawEditorMulti(rowIndices, ImGui::GetContentRegionAvail().x, isOverwrite);
					else
						deferredCallbacks.drawEditor(displayIndex, ImGui::GetContentRegionAvail().x, isOverwrite);

					if (rowOverridden)
						ImGui::PopStyleColor(2);

					if (anyPaused)
						ImGui::EndDisabled();

					if (inlineActions) {
						ImGui::TableSetColumnIndex(actionsColumn);
						CenterCursorY(rowVisualH, ImGui::GetFrameHeight());
						DrawInlineControls(rowIndices, isOverwrite, popups, entries, deferredCallbacks);
					}

					ImGui::PopID();
				}
			}

			if (hasAuxiliaryColumn && ImGui::TableSetColumnIndex(auxiliaryColumn)) {
				CenterCursorY(rowVisualH, ImGui::GetFrameHeight());
				auto disabled = Util::DisableGuard(!isOverwrite && AreAllPaused(rowIndices, entries));
				deferredCallbacks.drawAuxiliary(rowIndices, ImGui::GetContentRegionAvail().x, isOverwrite);
			}

			// Suppress row flyout when a cell flyout is active to prevent accidental whole-row deletion
			if (multiColumn && flyout.cell.isOpen && !flyout.cell.closing && flyout.row.isOpen && !flyout.row.flyoutHovered)
				flyout.row.closing = true;

			ImGui::PopID();
			for (size_t i = 0; i < sid.path.size(); ++i)
				ImGui::PopID();
			ImGui::PopID();
			ImGui::PopID();
		};

		constexpr auto kNoActiveFlyoutSource = std::numeric_limits<size_t>::max();
		if (!flyout.cell.isOpen)
			flyout.cellSourceRow = kNoActiveFlyoutSource;
		if (!flyout.row.isOpen)
			flyout.rowSourceRow = kNoActiveFlyoutSource;
		const size_t activeCellFlyoutSourceRow =
			flyout.cellSourceRow < group.rows.size() ? flyout.cellSourceRow : kNoActiveFlyoutSource;
		const size_t activeRowFlyoutSourceRow =
			flyout.rowSourceRow < group.rows.size() ? flyout.rowSourceRow : kNoActiveFlyoutSource;

		float measuredSettingRowHeight = -1.0f;
		for (const auto& category : group.categories) {
			if (category.begin >= category.end || category.end > group.rows.size())
				continue;
			const auto& categorySetting = group.rows[category.begin].setting;
			ImGui::TableNextRow();
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
			ImGui::TableSetColumnIndex(kSettingColumn);
			ImGui::SetWindowFontScale(C::SCENE_TOD_FEATURE_TEXT_SCALE);
			const auto visibleCategory = GetVisibleLabel(categorySetting.categoryName);
			ImGui::TextColored(theme.FeatureHeading.ColorDefault, "%.*s:",
				static_cast<int>(visibleCategory.size()), visibleCategory.data());
			ImGui::SetWindowFontScale(1.0f);

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(category.end - category.begin), measuredSettingRowHeight);
			if (activeCellFlyoutSourceRow >= category.begin && activeCellFlyoutSourceRow < category.end)
				clipper.IncludeItemByIndex(static_cast<int>(activeCellFlyoutSourceRow - category.begin));
			if (activeRowFlyoutSourceRow != activeCellFlyoutSourceRow &&
				activeRowFlyoutSourceRow >= category.begin && activeRowFlyoutSourceRow < category.end)
				clipper.IncludeItemByIndex(static_cast<int>(activeRowFlyoutSourceRow - category.begin));
			while (clipper.Step()) {
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
					const size_t rowIndex = category.begin + static_cast<size_t>(row);
					drawRow(group.rows[rowIndex], rowIndex);
				}
			}
			if (measuredSettingRowHeight < 0.0f && clipper.ItemsHeight > 0.0f)
				measuredSettingRowHeight = clipper.ItemsHeight;
		}

		ImGui::EndTable();
		RemoveIndicesReversed(pendingRemoveIndices, cb.remove);
		for (const auto& pending : pendingAddPeriods)
			cb.onAddPeriod(pending.feature, pending.path, pending.key, pending.period);
	}

	bool DrawSectionHeader(const char* label, const char* idSuffix,
		bool allPaused, std::function<void()> onTogglePause, std::function<void()> onDeleteAll,
		const SourceTableLayout& layout, std::function<void()> onExportAll, bool hasActiveOverrides)
	{
		ImGui::Spacing();
		const float sectionWidth = std::min(layout.sectionWidth, ImGui::GetContentRegionAvail().x);
		if (!ImGui::BeginChild(std::format("##sec{}", idSuffix).c_str(), ImVec2(sectionWidth, 0),
				ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_HorizontalScrollbar))
			return false;
		auto headerLabel = std::format("{}{}", label, idSuffix);
		bool open = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

		if (open) {
			if (hasActiveOverrides) {
				Util::Text::WrappedError(T("feature.scene_manager.overridden_warning",
					"Feature values are being overridden. Pause overwrites to see changes."));
			}
			if (onExportAll) {
				if (ImGui::SmallButton(std::format("{}{}",
						T("feature.scene_manager.action.export_all", "Export All"), idSuffix)
							.c_str()))
					onExportAll();
				ImGui::SameLine();
			}
			if (ImGui::SmallButton(std::format("{}{}", allPaused ? T("feature.scene_manager.action.unpause_all", "Unpause All") : T("feature.scene_manager.action.pause_all", "Pause All"), idSuffix).c_str()))
				onTogglePause();
			ImGui::SameLine();
			if (ImGui::SmallButton(std::format("{}{}",
					T("feature.scene_manager.action.delete_all", "Delete All"), idSuffix)
						.c_str()))
				onDeleteAll();
		}
		return open;
	}

	static bool DrawSelectedCheckbox(const std::string& label, uint8_t& selected)
	{
		bool checked = selected != 0;
		if (!ImGui::Checkbox(label.c_str(), &checked))
			return false;
		selected = checked ? 1 : 0;
		return true;
	}

	void EndSection()
	{
		ImGui::EndChild();
	}

	// Core export popup: list user entries with checkboxes (all on by default), then export on confirm.
	static void DrawExportPopupCore(
		const char* popupId,
		const std::vector<SceneSettingsManager::SettingEntry>& entries,
		ExportAllPopupState& state,
		std::function<SceneSettingsManager::OverwriteExportResult(
			const std::string&, const std::vector<size_t>&)>
			exportFn,
		bool showPeriod = true)
	{
		if (!state.dialogOpen)
			return;

		ImGui::OpenPopup(popupId);

		auto popup = Util::CenteredPopupModal(popupId, &state.dialogOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
		if (!popup)
			return;

		ImGui::InputText(T("feature.scene_manager.export.mod_name", "Mod Name"), state.modName, IM_ARRAYSIZE(state.modName));
		auto modName = Util::FileHelpers::SanitizeFileName(state.modName);
		if (modName.empty())
			Util::Text::WrappedDisabled(T("feature.scene_manager.export.enter_mod_name", "Enter a mod name to export."));
		ImGui::Spacing();

		ImGui::TextUnformatted(T("feature.scene_manager.export.select_settings", "Select settings to export as overwrite files:"));
		ImGui::Spacing();

		Util::DrawSelectionButtons(state.selected,
			T("feature.scene_manager.action.select_all", "Select All"),
			T("feature.scene_manager.action.select_none", "Select None"));

		ImGui::Spacing();
		if (ImGui::BeginChild("##ExportList", ImVec2(-FLT_MIN, C::Em(C::SCENE_ADD_LIST_HEIGHT_EM)), ImGuiChildFlags_Borders)) {
			if (showPeriod) {
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(state.userIndices.size()), ImGui::GetFrameHeightWithSpacing());
				while (clipper.Step()) {
					for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
						const auto i = static_cast<size_t>(visibleIndex);
						auto idx = state.userIndices[i];
						if (idx >= entries.size())
							continue;
						const auto& e = entries[idx];
						auto label = e.period != SceneSettingsManager::TimeOfDayPeriod::Count ? std::format("{} - {} ({})", SceneSettingsManager::GetFeatureDisplayName(e.featureShortName),
																									GetEntryDisplayName(e), GetPeriodDisplayName(e.period)) :
						                                                                        std::format("{} - {}", SceneSettingsManager::GetFeatureDisplayName(e.featureShortName),
																									GetEntryDisplayName(e));
						DrawSelectedCheckbox(std::format("{}##exp{}", label, i), state.selected[i]);
					}
				}
			} else {
				if (!state.flatGroupsCached) {
					using GroupKey = std::tuple<std::string, std::vector<std::string>, std::string>;
					std::map<GroupKey, std::vector<size_t>> groups;
					for (size_t i = 0; i < state.userIndices.size(); ++i) {
						const auto index = state.userIndices[i];
						if (index < entries.size())
							groups[{ entries[index].featureShortName, entries[index].settingPath,
									   entries[index].settingKey }]
								.push_back(i);
					}
					state.flatGroups.reserve(groups.size());
					for (auto& [key, indices] : groups) {
						(void)key;
						state.flatGroups.push_back(std::move(indices));
					}
					state.flatGroupsCached = true;
				}
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(state.flatGroups.size()), ImGui::GetFrameHeightWithSpacing());
				while (clipper.Step()) {
					for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
						auto& stateIs = state.flatGroups[static_cast<size_t>(visibleIndex)];
						if (stateIs.empty())
							continue;
						bool checked = std::all_of(stateIs.begin(), stateIs.end(), [&](size_t i) { return state.selected[i]; });
						const auto entryIndex = state.userIndices[stateIs.front()];
						if (entryIndex >= entries.size())
							continue;
						const auto& entry = entries[entryIndex];
						auto labelId = entry.settingKey;
						for (const auto& part : entry.settingPath)
							labelId += part;
						auto label = std::format("{} - {}##expg{}{}",
							SceneSettingsManager::GetFeatureDisplayName(entry.featureShortName), GetEntryDisplayName(entry),
							entry.featureShortName, labelId);
						if (ImGui::Checkbox(label.c_str(), &checked))
							for (auto i : stateIs)
								state.selected[i] = checked ? 1 : 0;
					}
				}
			}
		}
		ImGui::EndChild();

		ImGui::Spacing();

		int count = static_cast<int>(std::count_if(state.selected.begin(), state.selected.end(), [](uint8_t v) { return v != 0; }));
		if (state.exportResult && (state.exportResult->failedFiles != 0 ||
									  state.exportResult->writtenFiles == 0))
			Util::Text::WrappedError("%s", std::vformat(
											   T("feature.scene_manager.export.failed",
												   "Wrote {0} files, but {1} files could not be written. Check the log for details."),
											   std::make_format_args(state.exportResult->writtenFiles,
												   state.exportResult->failedFiles))
											   .c_str());
		{
			auto _ = Util::DisableGuard(count == 0 || modName.empty());
			auto exportLabel = std::vformat(T("feature.scene_manager.action.export_count", "Export ({0})"),
				std::make_format_args(count));
			if (ImGui::Button(exportLabel.c_str(), ImVec2(-FLT_MIN, 0))) {
				using ExportEntryKey = std::tuple<std::string, std::vector<std::string>,
					std::string, SceneSettingsManager::TimeOfDayPeriod>;
				std::map<ExportEntryKey, uint8_t> priorSelections;
				std::vector<size_t> toExport;
				for (size_t i = 0; i < state.userIndices.size(); ++i) {
					const auto entryIndex = state.userIndices[i];
					if (entryIndex >= entries.size())
						continue;
					const auto& entry = entries[entryIndex];
					priorSelections[{ entry.featureShortName, entry.settingPath,
						entry.settingKey, entry.period }] = state.selected[i];
					if (state.selected[i])
						toExport.push_back(state.userIndices[i]);
				}
				state.exportResult = exportFn(modName, toExport);
				if (state.exportResult->failedFiles == 0 && state.exportResult->writtenFiles != 0) {
					state.dialogOpen = false;
					ImGui::CloseCurrentPopup();
				} else if (state.exportResult->writtenFiles != 0) {
					state.userIndices.clear();
					state.selected.clear();
					for (size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
						const auto& entry = entries[entryIndex];
						if (entry.source != EntrySource::User)
							continue;
						auto selectionIt = priorSelections.find({ entry.featureShortName,
							entry.settingPath, entry.settingKey, entry.period });
						if (selectionIt == priorSelections.end())
							continue;
						state.userIndices.push_back(entryIndex);
						state.selected.push_back(selectionIt->second);
					}
					state.flatGroups.clear();
					state.flatGroupsCached = false;
				}
			}
		}
	}

	void DrawExportAllPopup(SceneType type, const std::vector<SceneSettingsManager::SettingEntry>& entries, ExportAllPopupState& state)
	{
		if (!state.dialogOpen)
			return;
		auto popupId = std::format("{}##scene", T("feature.scene_manager.export.title", "Export User Settings"));
		DrawExportPopupCore(popupId.c_str(), entries, state,
			[type](const std::string& modName, const std::vector<size_t>& indices) {
				return SceneSettingsManager::GetSingleton()->ExportUserSettingsToOverwrites(type, indices, modName);
			});
	}

	void DrawWeatherExportAllPopup(RE::FormID weatherId, const std::vector<SceneSettingsManager::SettingEntry>& entries, ExportAllPopupState& state, bool showTod)
	{
		if (!state.dialogOpen)
			return;
		auto popupId = std::format("{}##wx{:08X}", T("feature.scene_manager.export.title", "Export User Settings"), weatherId);
		DrawExportPopupCore(popupId.c_str(), entries, state, [weatherId](const std::string& modName, const std::vector<size_t>& indices) { return SceneSettingsManager::GetSingleton()->ExportWeatherUserSettingsToOverwrites(weatherId, indices, modName); }, showTod);
	}

	struct GlobalActionState
	{
		std::optional<SceneSettingsManager::SceneContextType> scope;
		std::optional<SceneSettingsManager::SceneContextId> target;
		std::string targetLabel;

		std::uint64_t summaryRevision = std::numeric_limits<std::uint64_t>::max();
		SceneSettingsManager::EntryLayerSummary userSummary;
		SceneSettingsManager::EntryLayerSummary overwriteSummary;
		bool exportOpen = false;
		EntrySource exportSource = EntrySource::User;
		char modName[ExportAllPopupState::kModNameBufferSize] = "";
		std::optional<SceneSettingsManager::OverwriteExportResult> exportResult;
		Util::ConfirmationPopup deleteUser;
		Util::ConfirmationPopup deleteOverwrites;
	};

	static void DrawSceneManagementActions(GlobalActionState& state)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto scope = state.scope;
		const auto* target = state.target ? &*state.target : nullptr;
		if (state.summaryRevision != manager->GetEntryPresentationRevision()) {
			state.userSummary = manager->GetEntryLayerSummary(EntrySource::User, scope, target);
			state.overwriteSummary = manager->GetEntryLayerSummary(EntrySource::Overwrite, scope, target);
			state.summaryRevision = manager->GetEntryPresentationRevision();
		}
		const auto& user = state.userSummary;
		const auto& overwrite = state.overwriteSummary;
		const auto drawLayerActions = [&](EntrySource source,
										  const SceneSettingsManager::EntryLayerSummary& summary, const char* label) {
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%s (%zu)", label, summary.count);
			ImGui::SameLine();
			{
				auto disabled = Util::DisableGuard(summary.Empty());
				if (ImGui::SmallButton(std::format("{}##Export{}", T("feature.scene_manager.action.export_all", "Export All"), static_cast<int>(source)).c_str())) {
					state.exportOpen = true;
					state.exportSource = source;
					state.exportResult.reset();
				}
				ImGui::SameLine();
			}
			{
				auto disabled = Util::DisableGuard(summary.Empty());
				const auto* pauseLabel = summary.AllPaused() ?
				                             T("feature.scene_manager.action.unpause_all", "Unpause All") :
				                             T("feature.scene_manager.action.pause_all", "Pause All");
				if (ImGui::SmallButton(std::format("{}##Global{}", pauseLabel,
						static_cast<int>(source))
							.c_str()))
					manager->SetEntryLayerPaused(source, !summary.AllPaused(), scope, target);
				ImGui::SameLine();
				const auto* deleteLabel = source == EntrySource::Overwrite ?
				                              T("feature.scene_manager.action.delete_loaded_overwrites", "Delete Loaded Overwrites") :
				                              T("feature.scene_manager.action.delete_all", "Delete All");
				if (ImGui::SmallButton(std::format("{}##Global{}", deleteLabel,
						static_cast<int>(source))
							.c_str())) {
					if (source == EntrySource::User)
						state.deleteUser.Request();
					else
						state.deleteOverwrites.Request();
				}
			}
		};

		drawLayerActions(EntrySource::User, user,
			T("feature.scene_manager.section.user_settings", "User Settings"));
		drawLayerActions(EntrySource::Overwrite, overwrite,
			T("feature.scene_manager.copy.overwrite_settings", "Overwrite Settings"));
		if (overwrite.count != 0)
			ImGui::TextDisabled("%s", T("feature.scene_manager.global.overwrite_pause_session",
										  "Overwrite pause state lasts for this game session."));
		state.deleteUser.title = std::format("{}##GlobalDeleteUser",
			T("feature.scene_manager.global.delete_user_title", "Delete All User Settings?"));
		state.deleteUser.message = T("feature.scene_manager.global.delete_user_message",
			"This deletes User Settings from every Interior, Time of Day, Weather, and Location scene. Weather and location modes are preserved.");
		if (scope) {
			const auto label = target ? state.targetLabel : std::string(GetCopySourceTypeLabel(scope));
			state.deleteUser.message = std::vformat(
				T("feature.scene_manager.bulk.delete_user_message",
					"Delete all User Settings in {0}? Other scene types and display preferences are preserved."),
				std::make_format_args(label));
		}
		state.deleteUser.confirmLabel = T("feature.scene_manager.action.delete_all", "Delete All");
		state.deleteUser.cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");
		if (state.deleteUser.Draw())
			manager->DeleteEntryLayer(EntrySource::User, scope, target);

		state.deleteOverwrites.title = T("feature.scene_manager.global.delete_overwrites_title",
			"Delete Loaded Overwrites?");
		state.deleteOverwrites.message = T("feature.scene_manager.global.delete_overwrites_message",
			"This deletes the backing files for every overwrite currently loaded by Scene Manager. Invalid or shadowed files that were not loaded are left on disk.");
		if (scope) {
			const auto label = target ? state.targetLabel : std::string(GetCopySourceTypeLabel(scope));
			state.deleteOverwrites.message = std::vformat(
				T("feature.scene_manager.bulk.delete_overwrites_message",
					"Delete every loaded overwrite file in {0}? Other scene types are preserved. This cannot be undone."),
				std::make_format_args(label));
		}
		state.deleteOverwrites.confirmLabel = T("feature.scene_manager.action.delete_loaded_overwrites",
			"Delete Loaded Overwrites");
		state.deleteOverwrites.cancelLabel = T("feature.scene_manager.action.cancel", "Cancel");
		if (state.deleteOverwrites.Draw())
			manager->DeleteEntryLayer(EntrySource::Overwrite, scope, target);

		if (!state.exportOpen)
			return;
		const auto popupId = std::format("{}##GlobalSceneExport",
			state.exportSource == EntrySource::User ?
				T("feature.scene_manager.global.export_title", "Export All User Settings") :
				T("feature.scene_manager.bulk.export_overwrites_title", "Export All Overwrite Settings"));
		ImGui::OpenPopup(popupId.c_str());
		auto popup = Util::CenteredPopupModal(popupId.c_str(), &state.exportOpen,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
		if (!popup)
			return;
		ImGui::InputText(T("feature.scene_manager.export.mod_name", "Mod Name"),
			state.modName, IM_ARRAYSIZE(state.modName));
		const auto modName = Util::FileHelpers::SanitizeFileName(state.modName);
		if (scope || state.exportSource == EntrySource::Overwrite) {
			const auto label = target ? state.targetLabel : std::string(GetCopySourceTypeLabel(scope));
			ImGui::TextWrapped("%s", std::vformat(
										 T("feature.scene_manager.bulk.export_message", "Exports the selected layer in {0} into grouped overwrite files."),
										 std::make_format_args(label))
										 .c_str());
		} else {
			ImGui::TextWrapped("%s", T("feature.scene_manager.global.export_message",
										 "Exports every loaded User Setting across all scenes into grouped overwrite files."));
		}
		if (state.exportResult && state.exportResult->failedFiles != 0)
			Util::Text::WrappedError("%s", std::vformat(
											   T("feature.scene_manager.global.export_failed",
												   "Wrote {0} files, but {1} files could not be written. Check the log for details."),
											   std::make_format_args(state.exportResult->writtenFiles,
												   state.exportResult->failedFiles))
											   .c_str());
		{
			auto disabled = Util::DisableGuard(modName.empty() ||
											   (state.exportSource == EntrySource::User ? user.Empty() : overwrite.Empty()));
			if (ImGui::Button(T("feature.scene_manager.action.export_all", "Export All"),
					ImVec2(-FLT_MIN, 0))) {
				state.exportResult = manager->ExportEntryLayerToOverwrites(modName, state.exportSource, scope, target);
				if (state.exportResult->failedFiles == 0 && state.exportResult->writtenFiles != 0) {
					state.exportOpen = false;
					ImGui::CloseCurrentPopup();
				}
			}
		}
	}

	static void DrawTargetManagementPopup(const SceneSettingsManager::SceneContextId& target, const std::string& label, bool open)
	{
		static GlobalActionState state;
		if (open) {
			state = {};
			state.target = target;
			state.scope = target.type;
			state.targetLabel = label;
			ImGui::OpenPopup("##ManageSceneTarget");
		}
		ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f), ImGuiCond_Appearing);
		auto popup = Util::Popup("##ManageSceneTarget", state.targetLabel.c_str());
		if (!popup)
			return;
		DrawSceneManagementActions(state);
	}

	void DrawGlobalActions(std::optional<SceneSettingsManager::SceneContextType> scope)
	{
		const int owner = scope ? static_cast<int>(*scope) + 1 : 0;
		static std::map<int, GlobalActionState> states;
		auto& state = states[owner];
		ImGui::PushID(owner);
		const SKSE::stl::scope_exit restoreId([]() noexcept { ImGui::PopID(); });
		const auto* label = scope == SceneSettingsManager::SceneContextType::Weather ?
		                        T("feature.scene_manager.action.manage_all_weathers", "Manage All Weathers") :
		                    scope == SceneSettingsManager::SceneContextType::Location ?
		                        T("feature.scene_manager.action.manage_all_locations", "Manage All Locations") :
		                        T("feature.scene_manager.action.manage_settings", "Manage Settings");
		if (ImGui::Button(label)) {
			state = {};
			state.scope = scope;
			ImGui::OpenPopup("##ManageSceneSettings");
		}
		ImGui::SetNextWindowSizeConstraints(ImVec2(C::Em(36.0f), 0.0f), ImVec2(C::Em(46.0f), FLT_MAX));
		auto popup = Util::Popup("##ManageSceneSettings", label);
		if (!popup)
			return;
		if (!scope) {
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::BeginCombo("##ManageSceneKind", GetCopySourceTypeLabel(state.scope))) {
				for (const auto candidate : {
						 std::optional<SceneSettingsManager::SceneContextType>{},
						 std::optional{ SceneSettingsManager::SceneContextType::Interior },
						 std::optional{ SceneSettingsManager::SceneContextType::TimeOfDay },
						 std::optional{ SceneSettingsManager::SceneContextType::Weather },
						 std::optional{ SceneSettingsManager::SceneContextType::Location } })
					if (ImGui::Selectable(GetCopySourceTypeLabel(candidate), state.scope == candidate)) {
						state.scope = candidate;
						state.summaryRevision = std::numeric_limits<std::uint64_t>::max();
					}
				ImGui::EndCombo();
			}
		}
		DrawSceneManagementActions(state);
	}

	static AddSettingState s_interiorAddState;
	static PopupState s_interiorPopups;
	static TableFlyoutState s_interiorTableFlyout;
	static ExportAllPopupState s_interiorExportState;
	static SourcePanelCache s_interiorTableCache;

	void DrawInteriorPanel()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetEntries(SceneType::InteriorOnly);
		auto& theme = globals::menu->GetSettings().Theme;
		ConfigurePopups(s_interiorPopups,
			T("feature.scene_manager.confirm.delete_all_interior_overwrites",
				"Are you sure you want to delete all interior overwrite files?\nThis cannot be undone."),
			T("feature.scene_manager.confirm.delete_all_interior_user",
				"Are you sure you want to remove all user-added interior settings?"));

		ImGui::TextUnformatted(T("feature.scene_manager.interior.title", "Interior Settings"));
		ImGui::Separator();

		if (ImGui::SmallButton(T("feature.scene_manager.action.add_setting", "Add Setting")))
			OpenAddDialog(SceneType::InteriorOnly, s_interiorAddState);

		DrawPopups(SceneType::InteriorOnly, s_interiorPopups);
		DrawAddSettingDialog(SceneType::InteriorOnly, s_interiorAddState);

		if (entries.empty()) {
			ImGui::Spacing();
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.interior.empty", "No interior settings configured."));
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.empty_add_hint", "Use the Add Setting button above to add overrides."));
			ImGui::Spacing();
			ImGui::TextWrapped("%s", T("feature.scene_manager.interior.description",
										 "Settings added here override feature values in interior cells and revert automatically outside."));
			return;
		}

		ImGui::Spacing();

		TableCallbacks cb{
			[](size_t idx, float w, bool ro) { DrawValueEditor(SceneType::InteriorOnly, idx, w, ro); },
			[](const std::vector<size_t>& indices, float w, bool ro) {
				DrawValueEditor(SceneType::InteriorOnly, indices, w, ro);
			},
			[](size_t idx) { SceneSettingsManager::GetSingleton()->TogglePauseEntry(SceneType::InteriorOnly, idx); },
			[](size_t idx) { SceneSettingsManager::GetSingleton()->RevertEntryToDefault(SceneType::InteriorOnly, idx); },
			[](size_t idx) { SceneSettingsManager::GetSingleton()->RemoveSetting(SceneType::InteriorOnly, idx); }
		};

		RefreshSourcePanelCache(s_interiorTableCache, entries, 1, true);
		auto& overwrite = s_interiorTableCache.overwrite;
		auto& user = s_interiorTableCache.user;

		if (!overwrite.group.rows.empty()) {
			if (DrawSectionHeader(T("feature.scene_manager.section.overwrite_files", "Overwrite Files"), "##iow", overwrite.allPaused, [&] { manager->SetAllOverwritesPaused(SceneType::InteriorOnly, !overwrite.allPaused); }, [&] { s_interiorPopups.deleteAllOverwrites.Request(); }, overwrite.layout))
				DrawSourceTable(overwrite.group, entries, "##InteriorOW", EntrySource::Overwrite,
					overwrite.layout, &s_interiorPopups, s_interiorTableFlyout, cb);
			EndSection();
		}

		if (!user.group.rows.empty()) {
			if (DrawSectionHeader(T("feature.scene_manager.section.user_settings", "User Settings"), "##iusr", user.allPaused, [&] { manager->SetAllUserPaused(SceneType::InteriorOnly, !user.allPaused); }, [&] { s_interiorPopups.deleteAllUser.Request(); }, user.layout, [&] { s_interiorExportState.Open(user.indices); }, s_interiorTableCache.hasActiveOverrides))
				DrawSourceTable(user.group, entries, "##InteriorUsr", EntrySource::User, user.layout,
					&s_interiorPopups, s_interiorTableFlyout, cb, s_interiorTableCache.overriddenEntries);
			EndSection();
		}
		DrawExportAllPopup(SceneType::InteriorOnly, entries, s_interiorExportState);
	}

	static AddSettingState s_todPeriodAddState[kPeriodCount];
	static AddSettingState s_todAllPeriodsAddState;
	static PopupState s_todPopups;
	static TableFlyoutState s_todTableFlyout;
	static ExportAllPopupState s_todExportState;
	static SourcePanelCache s_todTableCache;

	void DrawTimeOfDayPanel()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetEntries(SceneType::TimeOfDay);
		auto& theme = globals::menu->GetSettings().Theme;
		ConfigurePopups(s_todPopups,
			T("feature.scene_manager.confirm.delete_all_tod_overwrites",
				"Are you sure you want to delete all time-of-day overwrite files?\nThis cannot be undone."),
			T("feature.scene_manager.confirm.delete_all_tod_user",
				"Are you sure you want to remove all user-added time-of-day settings?"));

		ImGui::TextUnformatted(T("feature.scene_manager.time_of_day.title", "Time of Day Settings"));
		ImGui::SameLine();
		ImGui::TextDisabled("%s", T("feature.scene_manager.time_of_day.exterior_only", "(Exterior Only)"));

		auto currentPeriod = SceneSettingsManager::GetCurrentPeriod();
		const auto* currentPeriodName = GetPeriodDisplayName(currentPeriod);
		const float currentGameHour = SceneSettingsManager::GetCurrentGameHour();
		ImGui::SameLine();
		auto currentTimeLabel = std::vformat(
			T("feature.scene_manager.time_of_day.current_time", "[{0} {1:.1f} h]"),
			std::make_format_args(currentPeriodName, currentGameHour));
		ImGui::TextColored(theme.StatusPalette.InfoColor, "%s", currentTimeLabel.c_str());

		ImGui::Separator();

		for (int i = 0; i < kPeriodCount; ++i) {
			auto label = GetAddPeriodLabel(static_cast<Period>(i));
			if (i > 0)
				ContinueButtonRowIfFits(label.c_str());
			ImGui::PushID(i);
			if (ImGui::SmallButton(label.c_str()))
				OpenAddDialog(SceneType::TimeOfDay, s_todPeriodAddState[i]);
			ImGui::PopID();
		}
		const auto* addAllLabel = T("feature.scene_manager.action.add_all", "Add All");
		ContinueButtonRowIfFits(addAllLabel);
		if (ImGui::SmallButton(addAllLabel))
			OpenAddDialog(SceneType::TimeOfDay, s_todAllPeriodsAddState);

		for (int i = 0; i < kPeriodCount; ++i)
			DrawAddSettingDialog(SceneType::TimeOfDay, s_todPeriodAddState[i], static_cast<Period>(i));
		DrawAddSettingDialog(SceneType::TimeOfDay, s_todAllPeriodsAddState, Period::Count, true);

		ImGui::Separator();

		DrawPopups(SceneType::TimeOfDay, s_todPopups);
		RefreshSourcePanelCache(s_todTableCache, entries, kPeriodCount, false, true);

		if (s_todTableCache.overwrite.group.rows.empty() && s_todTableCache.user.group.rows.empty()) {
			ImGui::Spacing();
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.time_of_day.empty", "No time-of-day settings configured."));
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.time_of_day.empty_hint", "Use the Add buttons above to add overrides for each period."));
			return;
		}

		ImGui::Spacing();

		TableCallbacks cb{
			[](size_t idx, float w, bool ro) { DrawValueEditor(SceneType::TimeOfDay, idx, w, ro); },
			[](const std::vector<size_t>& indices, float w, bool ro) {
				DrawValueEditor(SceneType::TimeOfDay, indices, w, ro);
			},
			[](size_t idx) { SceneSettingsManager::GetSingleton()->TogglePauseEntry(SceneType::TimeOfDay, idx); },
			[](size_t idx) { SceneSettingsManager::GetSingleton()->RevertEntryToDefault(SceneType::TimeOfDay, idx); },
			[](size_t idx) { SceneSettingsManager::GetSingleton()->RemoveSetting(SceneType::TimeOfDay, idx); },
			[](const std::string& feat, const std::vector<std::string>& path, const std::string& key, int p) {
				SceneSettingsManager::GetSingleton()->AddSetting(SceneType::TimeOfDay, feat, path, key,
					SceneSettingsManager::GetFeatureSettingValue(feat, path, key), static_cast<Period>(p));
			}
		};

		auto& overwrite = s_todTableCache.overwrite;
		auto& user = s_todTableCache.user;

		if (!overwrite.group.rows.empty()) {
			if (DrawSectionHeader(T("feature.scene_manager.section.overwrite_files", "Overwrite Files"), "##tow", overwrite.allPaused, [&] { manager->SetAllOverwritesPaused(SceneType::TimeOfDay, !overwrite.allPaused); }, [&] { s_todPopups.deleteAllOverwrites.Request(); }, overwrite.layout))
				DrawSourceTable(overwrite.group, entries, "##TODOverwrite", EntrySource::Overwrite,
					overwrite.layout, &s_todPopups, s_todTableFlyout, cb);
			EndSection();
		}

		if (!user.group.rows.empty()) {
			if (DrawSectionHeader(T("feature.scene_manager.section.user_settings", "User Settings"), "##tusr", user.allPaused, [&] { manager->SetAllUserPaused(SceneType::TimeOfDay, !user.allPaused); }, [&] { s_todPopups.deleteAllUser.Request(); }, user.layout, [&] { s_todExportState.Open(user.indices); }, s_todTableCache.hasActiveOverrides))
				DrawSourceTable(user.group, entries, "##TODUser", EntrySource::User, user.layout,
					&s_todPopups, s_todTableFlyout, cb, s_todTableCache.overriddenEntries);
			EndSection();
		}
		DrawExportAllPopup(SceneType::TimeOfDay, entries, s_todExportState);
	}

	using LocationTarget = SceneSettingsManager::LocationTarget;
	using LocationTargetType = SceneSettingsManager::LocationTargetType;

	struct LocationTargetPickerEntry
	{
		size_t targetIndex = 0;
		std::string identity;
		std::string displayLabel;
		std::string selectableLabel;
		bool configured = false;
		bool paused = false;
	};

	struct LocationTargetPickerCache
	{
		size_t targetCount = std::numeric_limits<size_t>::max();
		std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
		std::string locale;
		std::vector<LocationTargetPickerEntry> entries;
		std::map<std::string, size_t> indicesByIdentity;
		std::vector<size_t> configuredIndices;
		std::vector<size_t> locationTypeIndices;
		std::vector<size_t> unconfiguredIndices;
		std::vector<size_t> currentIndices;
		std::vector<std::uint8_t> currentMembership;
		std::vector<size_t> visibleIndices;
	};

	struct LocationPanelState
	{
		LocationTargetType selectedType = LocationTargetType::Location;
		std::string selectedFormKey;
		size_t selectedTargetIndex = SIZE_MAX;
		LocationTarget targetSnapshot;
		bool hasTargetSnapshot = false;
		AddSettingState addState;
		Period addPeriod = Period::Count;
		PopupState popups;
		TableFlyoutState tableFlyout;
		ExportAllPopupState exportState;
		SourcePanelCache tableCache;
		LocationTargetPickerCache targetPicker;
	};

	static LocationPanelState s_locationState;

	static const char* GetLocationTargetTypeName(LocationTargetType type)
	{
		switch (type) {
		case LocationTargetType::Worldspace:
			return T("feature.scene_manager.location.target_worldspace", "Worldspace");
		case LocationTargetType::Region:
			return T("feature.scene_manager.location.target_region", "Region");
		case LocationTargetType::LocationType:
			return T("feature.scene_manager.location.target_location_type", "Location Type");
		case LocationTargetType::Location:
			return T("feature.scene_manager.location.target_location", "Location");
		case LocationTargetType::Cell:
			return T("feature.scene_manager.location.target_cell", "Cell");
		default:
			return "";
		}
	}

	static std::string GetLocationTargetLabel(const LocationTarget& target)
	{
		std::string targetTypeName;
		if (target.type == LocationTargetType::Location && !target.locationTypes.empty()) {
			targetTypeName = target.locationTypes.back();
		} else {
			targetTypeName = GetLocationTargetTypeName(target.type);
		}
		return std::vformat(T("feature.scene_manager.location.target_label", "{0} ({1})"),
			std::make_format_args(target.name.empty() ? target.formKey : target.name,
				targetTypeName));
	}

	static std::string GetLocationTargetIdentity(const LocationTarget& target)
	{
		return std::format("{}:{}", static_cast<int>(target.type), target.formKey);
	}

	static void RefreshLocationTargetPickerCache(const std::vector<LocationTarget>& targets,
		LocationPanelState& state)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		auto& cache = state.targetPicker;
		const auto revision = manager->GetEntryPresentationRevision();
		const auto locale = I18n::GetSingleton()->GetCurrentLocale();
		if (cache.targetCount == targets.size() && cache.revision == revision &&
			cache.locale == locale)
			return;

		cache.targetCount = targets.size();
		cache.revision = revision;
		cache.locale = locale;
		cache.entries.clear();
		cache.entries.reserve(targets.size());
		cache.indicesByIdentity.clear();
		cache.configuredIndices.clear();
		cache.locationTypeIndices.clear();
		cache.unconfiguredIndices.clear();
		state.selectedTargetIndex = SIZE_MAX;
		for (size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
			const auto& target = targets[targetIndex];
			auto identity = GetLocationTargetIdentity(target);
			const auto& entries = manager->GetLocationConfig(target.type, target.formKey).entries;
			auto presentation = GetScenePickerPresentation(GetLocationTargetLabel(target), entries, manager->IsLocationShowTimeOfDay(target.type, target.formKey));
			const bool configured = !entries.empty();
			const size_t cacheIndex = cache.entries.size();
			cache.indicesByIdentity.emplace(identity, cacheIndex);
			cache.entries.push_back({
				.targetIndex = targetIndex,
				.identity = std::move(identity),
				.displayLabel = std::move(presentation.label),
				.configured = configured,
				.paused = presentation.paused,
			});
			auto& entry = cache.entries.back();
			entry.selectableLabel = std::format("{}##{}", entry.displayLabel, entry.identity);
			(target.type == LocationTargetType::LocationType ? cache.locationTypeIndices :
				configured                                   ? cache.configuredIndices :
															   cache.unconfiguredIndices)
				.push_back(cacheIndex);
			if (target.type == state.selectedType && target.formKey == state.selectedFormKey)
				state.selectedTargetIndex = targetIndex;
		}
		cache.currentIndices.clear();
		cache.currentMembership.assign(cache.entries.size(), 0);
		cache.visibleIndices.clear();
	}

	static const LocationTarget* ResolveSelectedLocationTarget(
		const std::vector<LocationTarget>& targets, const std::vector<LocationTarget>& currentTargets,
		LocationPanelState& state)
	{
		auto& cache = state.targetPicker;
		if (state.selectedTargetIndex < targets.size()) {
			const auto& selected = targets[state.selectedTargetIndex];
			if (selected.type == state.selectedType && selected.formKey == state.selectedFormKey)
				return &selected;
		}

		size_t selectedIndex = SIZE_MAX;
		for (auto current = currentTargets.rbegin(); current != currentTargets.rend(); ++current) {
			const auto found = cache.indicesByIdentity.find(GetLocationTargetIdentity(*current));
			if (found != cache.indicesByIdentity.end()) {
				selectedIndex = cache.entries[found->second].targetIndex;
				break;
			}
		}
		if (selectedIndex == SIZE_MAX && !cache.configuredIndices.empty())
			selectedIndex = cache.entries[cache.configuredIndices.front()].targetIndex;
		if (selectedIndex >= targets.size())
			return nullptr;

		const auto& selected = targets[selectedIndex];
		state.selectedType = selected.type;
		state.selectedFormKey = selected.formKey;
		state.selectedTargetIndex = selectedIndex;
		return &selected;
	}

	static void UpdateLocationTargetSnapshot(LocationPanelState& state, const LocationTarget& target)
	{
		if (!state.hasTargetSnapshot || state.targetSnapshot.type != target.type ||
			state.targetSnapshot.formKey != target.formKey || state.targetSnapshot.name != target.name ||
			state.targetSnapshot.cocCode != target.cocCode ||
			state.targetSnapshot.locationTypes != target.locationTypes) {
			state.targetSnapshot = target;
			state.hasTargetSnapshot = true;
		}
	}

	static bool IsLocationPopupOpen(const PopupState& popups)
	{
		return popups.deleteAllOverwrites.IsOpen() || popups.deleteSingleOverwrite.IsOpen() ||
		       popups.deleteRowOverwrite.IsOpen() || popups.deleteAllUser.IsOpen();
	}

	static void DrawLocationAddDialog(const LocationTarget& target, AddSettingState& state, Period period, bool showTimeOfDay)
	{
		if (!state.dialogOpen) {
			if (s_activeAddDialog == &state)
				s_activeAddDialog = nullptr;
			return;
		}
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto getSettings = [showTimeOfDay](const std::string& feature) {
			auto settings = SceneSettingsManager::GetFeatureSceneSettings(SceneType::Location, feature);
			if (showTimeOfDay)
				std::erase_if(settings, [](const auto& setting) {
					return !std::ranges::all_of(setting.members, [](const auto& member) { return member.value.is_number_float(); });
				});
			return settings;
		};
		const auto getUserEntries = [&] {
			return BuildUserEntrySet(manager->GetLocationConfig(target.type, target.formKey).entries);
		};
		const auto hasEntry = [&](const std::string& feature, const std::vector<std::string>& path, const std::string& key, Period targetPeriod) {
			return manager->HasLocationEntry(target.type, target.formKey, feature, path, key, EntrySource::User, targetPeriod);
		};
		const auto addEntry = [&](const std::string& feature, const std::vector<std::string>& path,
								  const std::string& key, const json&, Period targetPeriod) {
			return manager->AddLocationSetting(target.type, target.formKey, target.name, target.cocCode,
				feature, path, key, true, targetPeriod);
		};
		const auto commit = [manager] { manager->CommitSceneSettingChanges(); };
		DrawAddDialogCore(state, period, showTimeOfDay && period == Period::Count, showTimeOfDay,
			getSettings, getUserEntries, hasEntry, addEntry, commit);
	}

	static void DrawLocationValueEditor(const LocationTarget& target, const std::vector<SettingEntry>& entries,
		size_t index, float inputWidth, bool readOnly)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entry = entries[index];
		DrawValueEditorCore(entry, inputWidth, entry.period == Period::Count, readOnly, [manager, &target, index](const json& value) { manager->UpdateLocationEntryValue(target.type, target.formKey, index, value, true); }, [manager] { manager->CommitSceneSettingChanges(); });
	}

	static void DrawLocationValueEditor(const LocationTarget& target, const std::vector<SettingEntry>& entries,
		const std::vector<size_t>& indices, float inputWidth, bool readOnly)
	{
		if (indices.empty())
			return;
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entry = entries[indices.back()];
		const bool useFullWidthSliders = entry.period == Period::Count;
		if (DrawAggregateValueEditorCore(entries, indices, inputWidth, useFullWidthSliders, readOnly, [manager, &target](const auto& updates) { manager->UpdateLocationEntryValues(target.type, target.formKey, updates, true); }, [manager] { manager->CommitSceneSettingChanges(); }))
			return;
		DrawValueEditorCore(entry, inputWidth, useFullWidthSliders, readOnly, [manager, &target, &indices](const json& value) {
			std::vector<SceneSettingsManager::EntryValueUpdate> updates;
			updates.reserve(indices.size());
			for (const auto index : indices)
				updates.push_back({ index, value });
			manager->UpdateLocationEntryValues(target.type, target.formKey, updates, true); }, [manager] { manager->CommitSceneSettingChanges(); });
	}

	static void DrawLocationTransitionEditor(const LocationTarget& target,
		const std::vector<SettingEntry>& entries, const std::vector<size_t>& indices,
		float inputWidth, bool readOnly)
	{
		if (indices.empty() || std::ranges::any_of(indices, [&](const size_t index) {
				return index >= entries.size() || !IsTransitionEntry(entries[index]);
			})) {
			ImGui::TextDisabled("--");
			return;
		}
		const auto duration = [&](size_t index) {
			return entries[index].transitionSeconds.value_or(SceneSettingsManager::kDefaultLocationTransitionSeconds);
		};
		float seconds = duration(indices.front());
		const bool mixed = std::ranges::any_of(indices, [&](size_t index) { return duration(index) != seconds; });
		auto disabled = Util::DisableGuard(readOnly);
		ImGui::SetNextItemWidth(inputWidth);
		if (ImGui::DragFloat("##locationTransitionSeconds", &seconds, kSceneFloatDragSpeed,
				0.0f, SceneSettingsManager::kMaxLocationTransitionSeconds, mixed ? "-- s" : "%.1f s",
				ImGuiSliderFlags_AlwaysClamp)) {
			SceneSettingsManager::GetSingleton()->SetLocationEntryTransitionSeconds(
				target.type, target.formKey, indices, seconds, true);
		}
		if (ImGui::IsItemDeactivatedAfterEdit())
			SceneSettingsManager::GetSingleton()->CommitSceneSettingChanges();
	}

	static void DrawLocationPopups(const LocationTarget& target, PopupState& popups)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		auto removeIndices = [&](const std::vector<size_t>& indices) {
			manager->RemoveSceneSettings({ .type = SceneSettingsManager::SceneContextType::Location,
											 .locationType = target.type,
											 .locationFormKey = target.formKey },
				indices);
		};
		auto getSourceIndices = [&](EntrySource source) {
			std::vector<size_t> indices;
			const auto& entries = manager->GetLocationConfig(target.type, target.formKey).entries;
			for (size_t i = 0; i < entries.size(); ++i)
				if (entries[i].source == source && manager->GetLocationConfig(target.type, target.formKey).IsPeriodActive(entries[i].period))
					indices.push_back(i);
			return indices;
		};

		if (popups.deleteAllOverwrites.Draw())
			removeIndices(getSourceIndices(EntrySource::Overwrite));

		if (popups.deleteSingleOverwrite.Draw()) {
			if (popups.pendingDeleteIndex < manager->GetLocationConfig(target.type, target.formKey).entries.size())
				manager->RemoveLocationSetting(target.type, target.formKey, popups.pendingDeleteIndex);
			popups.pendingDeleteIndex = SIZE_MAX;
		}

		if (popups.deleteRowOverwrite.Draw()) {
			removeIndices(popups.pendingDeleteRow);
			popups.pendingDeleteRow.clear();
		}

		if (popups.deleteAllUser.Draw())
			removeIndices(getSourceIndices(EntrySource::User));
	}

	static void DrawLocationExportPopup(const LocationTarget& target,
		const std::vector<SettingEntry>& entries, ExportAllPopupState& state)
	{
		if (!state.dialogOpen)
			return;
		auto popupId = std::format("{}##location{}:{}",
			T("feature.scene_manager.export.title", "Export User Settings"),
			static_cast<int>(target.type), target.formKey);
		DrawExportPopupCore(popupId.c_str(), entries, state, [=](const std::string& modName, const std::vector<size_t>& indices) { return SceneSettingsManager::GetSingleton()->ExportLocationUserSettingsToOverwrites(
																																	   target.type, target.formKey, indices, modName); }, SceneSettingsManager::GetSingleton()->IsLocationShowTimeOfDay(target.type, target.formKey));
	}

	void DrawLocationPanel()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		DrawGlobalActions(SceneSettingsManager::SceneContextType::Location);
		const auto& currentTargets = manager->GetCurrentLocationTargets();
		const auto& targets = manager->GetLocationManagementTargets();
		RefreshLocationTargetPickerCache(targets, s_locationState);
		auto& targetPicker = s_locationState.targetPicker;
		auto& theme = globals::menu->GetSettings().Theme;
		ConfigurePopups(s_locationState.popups,
			T("feature.scene_manager.confirm.delete_all_location_overwrites",
				"Are you sure you want to delete the overwrite files in this location's active mode?\nThis cannot be undone."),
			T("feature.scene_manager.confirm.delete_all_location_user",
				"Are you sure you want to remove all User Settings in this location's active mode?"));

		bool lockTargetSelection = s_locationState.addState.dialogOpen || s_locationState.exportState.dialogOpen ||
		                           IsLocationPopupOpen(s_locationState.popups);
		const LocationTarget* target = nullptr;
		if (lockTargetSelection && s_locationState.hasTargetSnapshot) {
			target = &s_locationState.targetSnapshot;
		} else {
			target = ResolveSelectedLocationTarget(targets, currentTargets, s_locationState);
			if (target)
				UpdateLocationTargetSnapshot(s_locationState, *target);
		}

		ImGui::BeginDisabled(lockTargetSelection);
		ImGui::SetNextItemWidth(-FLT_MIN);
		const auto* previewEntry = s_locationState.selectedTargetIndex < targetPicker.entries.size() ?
		                               &targetPicker.entries[s_locationState.selectedTargetIndex] :
		                               nullptr;
		auto preview = target && previewEntry ? previewEntry->displayLabel :
		               target                 ? GetLocationTargetLabel(*target) :
		                                        std::string(T("feature.scene_manager.location.select_target",
													"Select a worldspace, region, location type, location, or cell..."));
		bool targetSelectionChanged = false;
		if (Util::BeginSearchableCombo(T("feature.scene_manager.location.target", "Target"),
				preview.c_str(), ImGuiComboFlags_None, nullptr, kSceneTargetComboVisibleItems)) {
			OrderLocationTypePickerEntries(targetPicker, targets);
			targetPicker.currentIndices.clear();
			std::fill(targetPicker.currentMembership.begin(), targetPicker.currentMembership.end(), 0);
			for (auto current = currentTargets.rbegin(); current != currentTargets.rend(); ++current) {
				if (current->type == LocationTargetType::LocationType)
					continue;
				const auto found = targetPicker.indicesByIdentity.find(GetLocationTargetIdentity(*current));
				if (found == targetPicker.indicesByIdentity.end())
					continue;
				const size_t cacheIndex = found->second;
				if (targetPicker.entries[cacheIndex].configured || targetPicker.currentMembership[cacheIndex])
					continue;
				targetPicker.currentMembership[cacheIndex] = 1;
				targetPicker.currentIndices.push_back(cacheIndex);
			}

			const auto drawTargetGroup = [&](const char* label,
											 const std::vector<size_t>& candidates,
											 bool excludeCurrent = false) {
				targetPicker.visibleIndices.clear();
				int selectedVisibleIndex = -1;
				for (const auto cacheIndex : candidates) {
					if (excludeCurrent && targetPicker.currentMembership[cacheIndex])
						continue;
					const auto& entry = targetPicker.entries[cacheIndex];
					if (!Util::SearchableComboMatches(entry.displayLabel))
						continue;
					if (target && entry.targetIndex < targets.size()) {
						const auto& candidate = targets[entry.targetIndex];
						if (candidate.type == target->type && candidate.formKey == target->formKey)
							selectedVisibleIndex = static_cast<int>(targetPicker.visibleIndices.size());
					}
					targetPicker.visibleIndices.push_back(cacheIndex);
				}
				if (targetPicker.visibleIndices.empty())
					return;
				if (!DrawLocationPickerGroupHeader(label, &candidates == &targetPicker.locationTypeIndices,
						!Util::GetSearchableComboFilter().empty()))
					return;
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(targetPicker.visibleIndices.size()));
				if (selectedVisibleIndex >= 0)
					clipper.IncludeItemByIndex(selectedVisibleIndex);
				while (clipper.Step()) {
					for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
						const auto& entry = targetPicker.entries[targetPicker.visibleIndices[visibleIndex]];
						const auto& candidate = targets[entry.targetIndex];
						const bool selected = target && candidate.type == target->type &&
						                      candidate.formKey == target->formKey;
						const bool current = IsCurrentLocation(candidate);
						const SceneSettingsManager::SceneContextId context{
							.type = SceneSettingsManager::SceneContextType::Location,
							.locationType = candidate.type,
							.locationFormKey = candidate.formKey,
						};
						if (DrawSceneTargetSelectable(entry.identity.c_str(), entry.displayLabel, selected,
								GetScenePickerTextColor(current, entry.paused),
								entry.configured ? &context : nullptr)) {
							targetSelectionChanged |= !selected;
							s_locationState.selectedType = candidate.type;
							s_locationState.selectedFormKey = candidate.formKey;
							s_locationState.selectedTargetIndex = entry.targetIndex;
							s_locationState.addState.Reset();
						}
					}
				}
			};
			drawTargetGroup(T("feature.scene_manager.location.group.current", "Current"), targetPicker.currentIndices);
			drawTargetGroup(T("feature.scene_manager.location.group.configured", "Configured"), targetPicker.configuredIndices);
			drawTargetGroup(T("feature.scene_manager.location.group.types", "Location Types"), targetPicker.locationTypeIndices);
			drawTargetGroup(T("feature.scene_manager.location.group.available", "Available"), targetPicker.unconfiguredIndices, true);
			Util::EndSearchableCombo();
		}
		ImGui::EndDisabled();

		if (!lockTargetSelection && targetSelectionChanged) {
			target = ResolveSelectedLocationTarget(targets, currentTargets, s_locationState);
			if (target)
				UpdateLocationTargetSnapshot(s_locationState, *target);
		}
		if (!target) {
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.location.no_target", "No location target is selected."));
			ImGui::TextWrapped("%s", T("feature.scene_manager.location.no_target_hint",
										 "Choose any loaded worldspace, region, location type, location, or named cell to add or manage its settings."));
			return;
		}
		const auto& selectedTarget = *target;
		ImGui::TextDisabled("%s: %s", T("feature.scene_manager.location.spid_key", "SPID key"), selectedTarget.formKey.c_str());
		if (!selectedTarget.cocCode.empty())
			ImGui::TextDisabled("%s: %s", T("feature.scene_manager.location.coc_code", "COC code"), selectedTarget.cocCode.c_str());

		bool showTimeOfDay = manager->IsLocationShowTimeOfDay(selectedTarget.type, selectedTarget.formKey);
		if (ImGui::Checkbox(T("feature.scene_manager.tab.time_of_day", "Time of Day"), &showTimeOfDay)) {
			s_locationState.addState.Reset();
			manager->SetLocationShowTimeOfDay(selectedTarget.type, selectedTarget.formKey, showTimeOfDay);
		}

		if (showTimeOfDay)
			for (int p = 0; p < kPeriodCount; ++p) {
				const auto label = GetAddPeriodLabel(static_cast<Period>(p));
				if (p != 0)
					ContinueButtonRowIfFits(label.c_str());
				if (ImGui::SmallButton(label.c_str())) {
					s_locationState.addPeriod = static_cast<Period>(p);
					OpenAddDialog(SceneType::Location, s_locationState.addState);
				}
			}
		const auto* addLabel = showTimeOfDay ? T("feature.scene_manager.action.add_all", "Add All") :
		                                       T("feature.scene_manager.action.add_setting", "Add Setting");
		if (showTimeOfDay)
			ContinueButtonRowIfFits(addLabel);
		if (ImGui::SmallButton(addLabel)) {
			s_locationState.addPeriod = Period::Count;
			OpenAddDialog(SceneType::Location, s_locationState.addState);
		}
		DrawLocationAddDialog(selectedTarget, s_locationState.addState, s_locationState.addPeriod, showTimeOfDay);
		DrawLocationPopups(selectedTarget, s_locationState.popups);

		const auto& entries = manager->GetLocationConfig(
										 selectedTarget.type, selectedTarget.formKey)
		                          .entries;

		if (std::ranges::none_of(entries, [showTimeOfDay](const auto& entry) { return (entry.period != Period::Count) == showTimeOfDay; })) {
			ImGui::Spacing();
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.location.empty", "No settings are configured for this target."));

			return;
		}

		const bool hasTransitionEntries = std::ranges::any_of(entries, [showTimeOfDay](const auto& entry) {
			return IsTransitionEntry(entry) && (entry.period != Period::Count) == showTimeOfDay;
		});

		TableCallbacks callbacks{
			[&selectedTarget, &entries](size_t index, float width, bool readOnly) { DrawLocationValueEditor(selectedTarget, entries, index, width, readOnly); },
			[&selectedTarget, &entries](const std::vector<size_t>& indices, float width, bool readOnly) {
				DrawLocationValueEditor(selectedTarget, entries, indices, width, readOnly);
			},
			[&selectedTarget](size_t index) { SceneSettingsManager::GetSingleton()->TogglePauseLocationEntry(selectedTarget.type, selectedTarget.formKey, index); },
			[&selectedTarget](size_t index) { SceneSettingsManager::GetSingleton()->RevertLocationEntryToDefault(selectedTarget.type, selectedTarget.formKey, index); },
			[&selectedTarget](size_t index) { SceneSettingsManager::GetSingleton()->RemoveLocationSetting(selectedTarget.type, selectedTarget.formKey, index); },
			[&selectedTarget](const std::string& feature, const std::vector<std::string>& path, const std::string& key, int p) {
				SceneSettingsManager::GetSingleton()->AddLocationSetting(selectedTarget.type, selectedTarget.formKey, selectedTarget.name, selectedTarget.cocCode, feature, path, key, false, static_cast<Period>(p));
			}
		};
		if (hasTransitionEntries) {
			callbacks.auxiliaryColumnLabel = T("feature.scene_manager.location.transition.column", "Transition");
			callbacks.drawAuxiliary = [&selectedTarget, &entries](const std::vector<size_t>& indices,
										  float width, bool readOnly) {
				DrawLocationTransitionEditor(selectedTarget, entries, indices, width, readOnly);
			};
		}

		RefreshSourcePanelCache(s_locationState.tableCache, entries, showTimeOfDay ? kPeriodCount : 1, !showTimeOfDay, showTimeOfDay,
			hasTransitionEntries ? C::Em(kLocationTransitionColumnWidthEm) : 0.0f, showTimeOfDay);
		auto& overwrite = s_locationState.tableCache.overwrite;
		auto& user = s_locationState.tableCache.user;

		if (!overwrite.indices.empty()) {
			if (DrawSectionHeader(T("feature.scene_manager.section.overwrite_files", "Overwrite Files"), "##low", overwrite.allPaused, [&] { manager->SetLocationEntriesPaused(selectedTarget.type, selectedTarget.formKey, overwrite.indices, !overwrite.allPaused); }, [&] { s_locationState.popups.deleteAllOverwrites.Request(); }, overwrite.layout))
				DrawSourceTable(overwrite.group, entries, "##LocationOverwrite", EntrySource::Overwrite, overwrite.layout,
					&s_locationState.popups, s_locationState.tableFlyout, callbacks);
			EndSection();
		}

		if (!user.indices.empty()) {
			if (DrawSectionHeader(T("feature.scene_manager.section.user_settings", "User Settings"), "##lusr", user.allPaused, [&] { manager->SetLocationEntriesPaused(selectedTarget.type, selectedTarget.formKey, user.indices, !user.allPaused); }, [&] { s_locationState.popups.deleteAllUser.Request(); }, user.layout, [&] { s_locationState.exportState.Open(user.indices); }, s_locationState.tableCache.hasActiveOverrides))
				DrawSourceTable(user.group, entries, "##LocationUser", EntrySource::User, user.layout,
					&s_locationState.popups, s_locationState.tableFlyout, callbacks,
					s_locationState.tableCache.overriddenEntries);
			EndSection();
		}

		DrawLocationExportPopup(selectedTarget, entries, s_locationState.exportState);
	}

	struct WeatherPanelState
	{
		AddSettingState periodAddStates[kPeriodCount];
		AddSettingState allPeriodsAddState;
		PopupState popups;
		TableFlyoutState tableFlyout;
		ExportAllPopupState exportState;
		SourcePanelCache tableCache;
	};
	static WeatherPanelState s_weatherPanelState;
	static std::optional<RE::FormID> s_weatherPanelStateId;
	static RE::FormID s_selectedWeatherId = 0;

	static WeatherPanelState& GetWeatherState(RE::FormID id)
	{
		if (s_weatherPanelStateId && *s_weatherPanelStateId == id)
			return s_weatherPanelState;

		if (s_activeAddDialog == &s_weatherPanelState.allPeriodsAddState) {
			s_activeAddDialog->Reset();
			s_activeAddDialog = nullptr;
		}
		for (auto& addState : s_weatherPanelState.periodAddStates) {
			if (s_activeAddDialog != &addState)
				continue;
			s_activeAddDialog->Reset();
			s_activeAddDialog = nullptr;
			break;
		}

		s_weatherPanelState = {};
		s_weatherPanelStateId = id;
		++s_transientGeneration;
		return s_weatherPanelState;
	}

	static void ResetWeatherPeriodAddDialogs(WeatherPanelState& state)
	{
		for (auto& addState : state.periodAddStates) {
			if (s_activeAddDialog == &addState)
				s_activeAddDialog = nullptr;
			addState.Reset();
		}
	}

	static bool IsWeatherPanelInteractionOpen(const WeatherPanelState& state)
	{
		if (state.allPeriodsAddState.dialogOpen || state.exportState.dialogOpen ||
			state.popups.deleteAllOverwrites.IsOpen() || state.popups.deleteSingleOverwrite.IsOpen() ||
			state.popups.deleteRowOverwrite.IsOpen() || state.popups.deleteAllUser.IsOpen())
			return true;
		return std::any_of(std::begin(state.periodAddStates), std::end(state.periodAddStates),
			[](const auto& addState) { return addState.dialogOpen; });
	}

	static std::string GetWeatherTargetLabel(const RE::TESWeather* weather)
	{
		if (!weather)
			return {};
		auto label = Util::GetFormDisplayName(weather->GetFormID());
		return label.empty() ? Util::FormIdToSpid(weather->GetFormID()) : label;
	}

	void DrawWeatherPanel()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		DrawGlobalActions(SceneSettingsManager::SceneContextType::Weather);
		auto& theme = globals::menu->GetSettings().Theme;
		const auto& weatherTargets = GetSceneWeatherTargets();

		auto findWeather = [&](RE::FormID weatherId) {
			return std::ranges::find_if(weatherTargets,
				[weatherId](const auto* weather) { return weather->GetFormID() == weatherId; });
		};
		const auto previousWeatherId = s_selectedWeatherId;
		if (findWeather(s_selectedWeatherId) == weatherTargets.end()) {
			s_selectedWeatherId = 0;
			if (auto* sky = globals::game::sky; sky && sky->currentWeather &&
												findWeather(sky->currentWeather->GetFormID()) != weatherTargets.end())
				s_selectedWeatherId = sky->currentWeather->GetFormID();
		}

		WeatherPanelState* selectedState = s_selectedWeatherId != 0 ?
		                                       &GetWeatherState(s_selectedWeatherId) :
		                                       nullptr;
		const bool lockTargetSelection = selectedState && IsWeatherPanelInteractionOpen(*selectedState);
		ImGui::BeginDisabled(lockTargetSelection);
		if (DrawCopyWeatherPicker(T("feature.scene_manager.weather.target", "Weather"), weatherTargets, s_selectedWeatherId, true, false) ||
			s_selectedWeatherId != previousWeatherId)
			manager->SetWeatherShowTimeOfDay(s_selectedWeatherId, GetWeatherSelectionTimeOfDay(s_selectedWeatherId));
		ImGui::EndDisabled();

		if (s_selectedWeatherId == 0 || findWeather(s_selectedWeatherId) == weatherTargets.end()) {
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.weather.no_target", "No weather is selected."));
			ImGui::TextWrapped("%s", T("feature.scene_manager.weather.no_target_hint",
										 "Choose any loaded weather to add or manage its settings."));
			return;
		}
		ImGui::TextDisabled("%s: %s", T("feature.scene_manager.weather.spid_key", "SPID key"),
			Util::FormIdToSpid(s_selectedWeatherId).c_str());
		DrawWeatherScenePanel(s_selectedWeatherId);
	}

	static void DrawWeatherPopups(RE::FormID weatherId, PopupState& popups)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		auto removeIndices = [&](const std::vector<size_t>& indices) {
			manager->RemoveSceneSettings({ .type = SceneSettingsManager::SceneContextType::Weather, .weatherId = weatherId }, indices);
		};
		auto getSourceIndices = [&](EntrySource source) {
			std::vector<size_t> indices;
			const auto& entries = manager->GetWeatherConfig(weatherId).entries;
			for (size_t i = 0; i < entries.size(); ++i)
				if (entries[i].source == source && manager->GetWeatherConfig(weatherId).IsPeriodActive(entries[i].period))
					indices.push_back(i);
			return indices;
		};

		if (popups.deleteAllOverwrites.Draw())
			removeIndices(getSourceIndices(EntrySource::Overwrite));

		if (popups.deleteSingleOverwrite.Draw()) {
			if (popups.pendingDeleteIndex < manager->GetWeatherConfig(weatherId).entries.size())
				manager->RemoveWeatherSetting(weatherId, popups.pendingDeleteIndex);
			popups.pendingDeleteIndex = SIZE_MAX;
		}

		if (popups.deleteRowOverwrite.Draw()) {
			removeIndices(popups.pendingDeleteRow);
			popups.pendingDeleteRow.clear();
		}

		if (popups.deleteAllUser.Draw())
			removeIndices(getSourceIndices(EntrySource::User));
	}

	static TableCallbacks MakeWeatherCallbacks(RE::FormID weatherId)
	{
		return {
			[weatherId](size_t idx, float w, bool ro) { DrawWeatherValueEditor(weatherId, idx, w, ro); },
			[weatherId](const std::vector<size_t>& indices, float w, bool ro) {
				DrawWeatherValueEditor(weatherId, indices, w, ro);
			},
			[weatherId](size_t idx) { SceneSettingsManager::GetSingleton()->TogglePauseWeatherEntry(weatherId, idx); },
			[weatherId](size_t idx) { SceneSettingsManager::GetSingleton()->RevertWeatherEntryToDefault(weatherId, idx); },
			[weatherId](size_t idx) { SceneSettingsManager::GetSingleton()->RemoveWeatherSetting(weatherId, idx); },
			[weatherId](const std::string& feat, const std::vector<std::string>& path, const std::string& key, int p) {
				SceneSettingsManager::GetSingleton()->AddWeatherSetting(
					weatherId, feat, path, key, static_cast<Period>(p));
			}
		};
	}

	static void DrawWeatherSections(RE::FormID weatherId, WeatherPanelState& state, int numValueColumns)
	{
		bool showTod = numValueColumns > 1;
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& entries = manager->GetWeatherConfig(weatherId).entries;
		auto cb = MakeWeatherCallbacks(weatherId);
		auto& overwrite = state.tableCache.overwrite;
		auto& user = state.tableCache.user;

		if (!overwrite.indices.empty()) {
			if (DrawSectionHeader(T("feature.scene_manager.section.overwrite_files", "Overwrite Files"), "##wow", overwrite.allPaused, [&] { manager->SetWeatherEntriesPaused(weatherId, overwrite.indices, !overwrite.allPaused); }, [&] { state.popups.deleteAllOverwrites.Request(); }, overwrite.layout))
				DrawSourceTable(overwrite.group, entries, "##WxOverwrite", EntrySource::Overwrite,
					overwrite.layout, &state.popups, state.tableFlyout, cb);
			EndSection();
		}

		if (!user.indices.empty()) {
			if (DrawSectionHeader(T("feature.scene_manager.section.user_settings", "User Settings"), "##wusr", user.allPaused, [&] { manager->SetWeatherEntriesPaused(weatherId, user.indices, !user.allPaused); }, [&] { state.popups.deleteAllUser.Request(); }, user.layout, [&] { state.exportState.Open(user.indices); }, state.tableCache.hasActiveOverrides))
				DrawSourceTable(user.group, entries, "##WxUser", EntrySource::User, user.layout,
					nullptr, state.tableFlyout, cb, state.tableCache.overriddenEntries);
			EndSection();
		}
		DrawWeatherExportAllPopup(weatherId, entries, state.exportState, showTod);
	}

	void DrawWeatherScenePanel(RE::FormID weatherId)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		auto& state = GetWeatherState(weatherId);
		auto& theme = globals::menu->GetSettings().Theme;
		bool showTod = manager->IsWeatherShowTimeOfDay(weatherId);
		ConfigurePopups(state.popups,
			T("feature.scene_manager.confirm.delete_all_weather_overwrites",
				"Are you sure you want to delete the overwrite files in this weather's active mode?\nThis cannot be undone."),
			T("feature.scene_manager.confirm.delete_all_weather_user",
				"Are you sure you want to remove all User Settings in this weather's active mode?"));
		DrawWeatherPopups(weatherId, state.popups);
		const auto& config = manager->GetWeatherConfig(weatherId);

		{
			bool toggled = showTod;
			if (ImGui::Checkbox(T("feature.scene_manager.tab.time_of_day", "Time of Day"), &toggled)) {
				ResetWeatherPeriodAddDialogs(state);
				state.allPeriodsAddState.Reset();
				manager->SetWeatherShowTimeOfDay(weatherId, toggled);
			}
			showTod = toggled;
		}

		if (showTod) {
			for (int i = 0; i < kPeriodCount; ++i) {
				auto label = GetAddPeriodLabel(static_cast<Period>(i));
				if (i > 0)
					ContinueButtonRowIfFits(label.c_str());
				ImGui::PushID(i);
				if (ImGui::SmallButton(label.c_str()))
					OpenWeatherAddDialog(weatherId, state.periodAddStates[i]);
				ImGui::PopID();
			}
		}

		const auto* addLabel = showTod ?
		                           T("feature.scene_manager.action.add_all", "Add All") :
		                           T("feature.scene_manager.action.add_setting", "Add Setting");
		if (showTod)
			ContinueButtonRowIfFits(addLabel);
		if (ImGui::SmallButton(addLabel))
			OpenWeatherAddDialog(weatherId, state.allPeriodsAddState);

		DrawWeatherAddDialog(weatherId, state.allPeriodsAddState, Period::Count, showTod);
		if (showTod)
			for (int p = 0; p < kPeriodCount; ++p)
				DrawWeatherAddDialog(weatherId, state.periodAddStates[p], static_cast<Period>(p));

		RefreshSourcePanelCache(state.tableCache, config.entries, showTod ? kPeriodCount : 1, false, true, 0.0f, showTod);
		if (state.tableCache.overwrite.group.rows.empty() && state.tableCache.user.group.rows.empty()) {
			ImGui::Spacing();
			ImGui::TextColored(theme.StatusPalette.Disable, "%s",
				T("feature.scene_manager.weather.empty", "No scene settings for this weather."));

			return;
		}

		DrawWeatherSections(weatherId, state, showTod ? kPeriodCount : 1);
	}
}
