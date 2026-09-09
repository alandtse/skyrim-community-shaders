#include "SceneSettingsUIHooks.h"

#include "Feature.h"
#include "I18n/I18n.h"
#include "SceneSettingsCatalog.generated.h"
#include "SceneSettingsManager.h"
#include "Utils/UI.h"

#include <imgui_internal.h>

#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	using CheckboxFn = bool (*)(const char*, bool*);
	using ButtonFn = bool (*)(const char*, const ImVec2&);
	using CheckboxFlagsIntFn = bool (*)(const char*, int*, int);
	using CheckboxFlagsUIntFn = bool (*)(const char*, unsigned int*, unsigned int);
	using RadioButtonIntFn = bool (*)(const char*, int*, int);
	using RadioButtonBoolFn = bool (*)(const char*, bool);
	using ComboItemsFn = bool (*)(const char*, int*, const char* const*, int, int);
	using ComboStringFn = bool (*)(const char*, int*, const char*, int);
	using ComboGetterFn = bool (*)(const char*, int*, const char* (*)(void*, int), void*, int, int);
	using DragFloatFn = bool (*)(const char*, float*, float, float, float, const char*, ImGuiSliderFlags);
	using DragFloatNFn = bool (*)(const char*, float*, float, float, float, const char*, ImGuiSliderFlags);
	using DragIntFn = bool (*)(const char*, int*, float, int, int, const char*, ImGuiSliderFlags);
	using DragIntNFn = bool (*)(const char*, int*, float, int, int, const char*, ImGuiSliderFlags);
	using DragScalarFn = bool (*)(const char*, ImGuiDataType, void*, float, const void*, const void*, const char*, ImGuiSliderFlags);
	using DragScalarNFn = bool (*)(const char*, ImGuiDataType, void*, int, float, const void*, const void*, const char*, ImGuiSliderFlags);
	using SliderFloatFn = bool (*)(const char*, float*, float, float, const char*, ImGuiSliderFlags);
	using SliderFloatNFn = bool (*)(const char*, float*, float, float, const char*, ImGuiSliderFlags);
	using SliderIntFn = bool (*)(const char*, int*, int, int, const char*, ImGuiSliderFlags);
	using SliderIntNFn = bool (*)(const char*, int*, int, int, const char*, ImGuiSliderFlags);
	using SliderScalarFn = bool (*)(const char*, ImGuiDataType, void*, const void*, const void*, const char*, ImGuiSliderFlags);
	using SliderScalarNFn = bool (*)(const char*, ImGuiDataType, void*, int, const void*, const void*, const char*, ImGuiSliderFlags);
	using SliderAngleFn = bool (*)(const char*, float*, float, float, const char*, ImGuiSliderFlags);
	using PercentageSliderFn = bool (*)(const char*, float*, float, float, const char*);
	using InputFloatFn = bool (*)(const char*, float*, float, float, const char*, ImGuiInputTextFlags);
	using InputFloatNFn = bool (*)(const char*, float*, const char*, ImGuiInputTextFlags);
	using InputIntFn = bool (*)(const char*, int*, int, int, ImGuiInputTextFlags);
	using InputIntNFn = bool (*)(const char*, int*, ImGuiInputTextFlags);
	using InputScalarFn = bool (*)(const char*, ImGuiDataType, void*, const void*, const void*, const char*, ImGuiInputTextFlags);
	using InputScalarNFn = bool (*)(const char*, ImGuiDataType, void*, int, const void*, const void*, const char*, ImGuiInputTextFlags);
	using ColorEdit3Fn = bool (*)(const char*, float*, ImGuiColorEditFlags);
	using ColorEdit4Fn = bool (*)(const char*, float*, ImGuiColorEditFlags);
	using BeginComboFn = bool (*)(const char*, const char*, ImGuiComboFlags);
	using BeginGroupFn = void (*)();
	using EndGroupFn = void (*)();
	using IsItemHoveredFn = bool (*)(ImGuiHoveredFlags);
	using MarkItemEditedFn = void (*)(ImGuiID);

	CheckboxFn g_checkbox = static_cast<CheckboxFn>(&ImGui::Checkbox);
	ButtonFn g_button = static_cast<ButtonFn>(&ImGui::Button);
	CheckboxFlagsIntFn g_checkboxFlagsInt = static_cast<CheckboxFlagsIntFn>(&ImGui::CheckboxFlags);
	CheckboxFlagsUIntFn g_checkboxFlagsUInt = static_cast<CheckboxFlagsUIntFn>(&ImGui::CheckboxFlags);
	RadioButtonIntFn g_radioButtonInt = static_cast<RadioButtonIntFn>(&ImGui::RadioButton);
	RadioButtonBoolFn g_radioButtonBool = static_cast<RadioButtonBoolFn>(&ImGui::RadioButton);
	ComboItemsFn g_comboItems = static_cast<ComboItemsFn>(&ImGui::Combo);
	ComboStringFn g_comboString = static_cast<ComboStringFn>(&ImGui::Combo);
	ComboGetterFn g_comboGetter = static_cast<ComboGetterFn>(&ImGui::Combo);
	DragFloatFn g_dragFloat = static_cast<DragFloatFn>(&ImGui::DragFloat);
	DragFloatNFn g_dragFloat2 = &ImGui::DragFloat2;
	DragFloatNFn g_dragFloat3 = &ImGui::DragFloat3;
	DragFloatNFn g_dragFloat4 = &ImGui::DragFloat4;
	DragIntFn g_dragInt = static_cast<DragIntFn>(&ImGui::DragInt);
	DragIntNFn g_dragInt2 = &ImGui::DragInt2;
	DragIntNFn g_dragInt3 = &ImGui::DragInt3;
	DragIntNFn g_dragInt4 = &ImGui::DragInt4;
	DragScalarFn g_dragScalar = &ImGui::DragScalar;
	DragScalarNFn g_dragScalarN = &ImGui::DragScalarN;
	SliderFloatFn g_sliderFloat = static_cast<SliderFloatFn>(&ImGui::SliderFloat);
	SliderFloatNFn g_sliderFloat2 = &ImGui::SliderFloat2;
	SliderFloatNFn g_sliderFloat3 = &ImGui::SliderFloat3;
	SliderFloatNFn g_sliderFloat4 = &ImGui::SliderFloat4;
	SliderIntFn g_sliderInt = static_cast<SliderIntFn>(&ImGui::SliderInt);
	SliderIntNFn g_sliderInt2 = &ImGui::SliderInt2;
	SliderIntNFn g_sliderInt3 = &ImGui::SliderInt3;
	SliderIntNFn g_sliderInt4 = &ImGui::SliderInt4;
	SliderScalarFn g_sliderScalar = &ImGui::SliderScalar;
	SliderScalarNFn g_sliderScalarN = &ImGui::SliderScalarN;
	SliderAngleFn g_sliderAngle = static_cast<SliderAngleFn>(&ImGui::SliderAngle);
	PercentageSliderFn g_percentageSlider = &Util::PercentageSlider;
	InputFloatFn g_inputFloat = static_cast<InputFloatFn>(&ImGui::InputFloat);
	InputFloatNFn g_inputFloat2 = &ImGui::InputFloat2;
	InputFloatNFn g_inputFloat3 = &ImGui::InputFloat3;
	InputFloatNFn g_inputFloat4 = &ImGui::InputFloat4;
	InputIntFn g_inputInt = static_cast<InputIntFn>(&ImGui::InputInt);
	InputIntNFn g_inputInt2 = &ImGui::InputInt2;
	InputIntNFn g_inputInt3 = &ImGui::InputInt3;
	InputIntNFn g_inputInt4 = &ImGui::InputInt4;
	InputScalarFn g_inputScalar = &ImGui::InputScalar;
	InputScalarNFn g_inputScalarN = &ImGui::InputScalarN;
	ColorEdit3Fn g_colorEdit3 = static_cast<ColorEdit3Fn>(&ImGui::ColorEdit3);
	ColorEdit4Fn g_colorEdit4 = static_cast<ColorEdit4Fn>(&ImGui::ColorEdit4);
	BeginComboFn g_beginCombo = static_cast<BeginComboFn>(&ImGui::BeginCombo);
	BeginGroupFn g_beginGroup = &ImGui::BeginGroup;
	EndGroupFn g_endGroup = &ImGui::EndGroup;
	IsItemHoveredFn g_isItemHovered = &ImGui::IsItemHovered;
	MarkItemEditedFn g_markItemEdited = &ImGui::MarkItemEdited;

	thread_local Feature* g_currentFeature = nullptr;
	thread_local bool g_sceneSettingsActive = false;
	thread_local bool g_featureSceneEditing = false;
	using BlockedFeatureSceneEditSettings =
		std::unordered_set<const SceneSettingsCatalog::SettingMetadata*>;
	using LogicalControlKey = std::tuple<std::string_view, std::string_view, std::string_view,
		SceneSettingsCatalog::AggregateSemantic, std::int8_t, std::uint8_t>;
	thread_local const BlockedFeatureSceneEditSettings* g_blockedFeatureSceneEditSettings = nullptr;
	thread_local BlockedFeatureSceneEditSettings g_cachedBlockedFeatureSceneEditSettings;
	thread_local BlockedFeatureSceneEditSettings g_cachedAlteredFeatureSceneEditSettings;
	thread_local std::map<LogicalControlKey, const SceneSettingsCatalog::SettingMetadata*> g_cachedAlteredFeatureSceneEditAggregates;
	thread_local std::map<LogicalControlKey, const SceneSettingsCatalog::SettingMetadata*>
		g_cachedBlockedFeatureSceneEditAggregates;
	thread_local std::string g_cachedFeatureSceneEditFeature;
	thread_local std::uint64_t g_cachedFeatureSceneEditRevision =
		std::numeric_limits<std::uint64_t>::max();
	thread_local bool g_cachedFeatureSceneEditCaptureAllowed = false;
	thread_local bool g_allowSceneSettingTooltip = false;
	thread_local unsigned int g_controlDetourDepth = 0;
	thread_local bool g_featureSettingMutation = false;
	struct ControlDetourScope
	{
		ControlDetourScope() { ++g_controlDetourDepth; }
		~ControlDetourScope() { --g_controlDetourDepth; }
	};
	struct ControlledItem
	{
		ImGuiWindow* window = nullptr;
		ImGuiID id = 0;
		ImRect rect{};
		bool valid = false;
	};
	thread_local ControlledItem g_sceneControlledItem;
	thread_local std::vector<bool> g_sceneControlledGroupStack;
	struct SceneControlFrame
	{
		ControlledItem item;
		std::vector<bool> groups;
		const BlockedFeatureSceneEditSettings* blockedEditSettings = nullptr;
		bool featureSettingMutation = false;
	};
	thread_local std::vector<SceneControlFrame> g_sceneControlFrames;
	bool g_installed = false;

	void ClearControlledItem()
	{
		g_sceneControlledItem = {};
	}

	void CaptureControlledItem()
	{
		g_sceneControlledItem = {
			ImGui::GetCurrentWindowRead(), ImGui::GetItemID(), GImGui->LastItemData.Rect, true
		};
	}

	bool IsControlledItem()
	{
		if (!g_sceneControlledItem.valid || ImGui::GetCurrentWindowRead() != g_sceneControlledItem.window)
			return false;
		const auto& item = GImGui->LastItemData;
		const auto& rect = g_sceneControlledItem.rect;
		return item.ID == g_sceneControlledItem.id && item.Rect.Min.x == rect.Min.x &&
		       item.Rect.Min.y == rect.Min.y && item.Rect.Max.x == rect.Max.x &&
		       item.Rect.Max.y == rect.Max.y;
	}

	bool IsActiveSceneSetting(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return SceneSettingsManager::IsSceneSettingAllowed(
				   setting.featureShortName, setting.settingPath, setting.settingKey) &&
		       SceneSettingsManager::GetSingleton()->IsActiveSceneSetting(
				   setting.featureShortName, setting.settingPath, setting.settingKey);
	}

	bool IsFeatureSceneEditSetting(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return !g_blockedFeatureSceneEditSettings ||
		       !g_blockedFeatureSceneEditSettings->contains(&setting);
	}

	bool IsSceneControlGuardActive()
	{
		return g_sceneSettingsActive || g_featureSceneEditing;
	}

	std::string_view GetVisibleLabel(std::string_view label)
	{
		return label.substr(0, label.find("##"));
	}

	bool MatchesLocalizedLabel(std::string_view label, std::string_view displayName,
		std::string_view displayNameKey)
	{
		if (displayNameKey.empty())
			return GetVisibleLabel(label) == GetVisibleLabel(displayName);
		std::string fallback(displayName);
		const auto* translated = T(displayNameKey, fallback.c_str());
		return translated && GetVisibleLabel(label) == GetVisibleLabel(translated);
	}

	bool IsSameLogicalControl(const SceneSettingsCatalog::SettingMetadata& lhs,
		const SceneSettingsCatalog::SettingMetadata& rhs)
	{
		return lhs.featureShortName == rhs.featureShortName &&
		       lhs.serializedPath == rhs.serializedPath &&
		       lhs.serializedKey == rhs.serializedKey &&
		       lhs.aggregateSemantic == rhs.aggregateSemantic &&
		       lhs.aggregateStart == rhs.aggregateStart &&
		       lhs.aggregateCount == rhs.aggregateCount;
	}

	template <class Aggregate>
	LogicalControlKey GetLogicalControlKey(const Aggregate& setting)
	{
		return { setting.featureShortName, setting.serializedPath, setting.serializedKey,
			setting.aggregateSemantic, setting.aggregateStart, setting.aggregateCount };
	}

	void RefreshBlockedFeatureSceneEditSettings(Feature& feature)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto revision = manager->GetFeatureSceneEditRevision();
		const auto featureShortName = feature.GetShortName();
		const bool captureAllowed = manager->CanCaptureFeatureSceneEdit(featureShortName);
		if (revision == g_cachedFeatureSceneEditRevision &&
			featureShortName == g_cachedFeatureSceneEditFeature &&
			captureAllowed == g_cachedFeatureSceneEditCaptureAllowed)
			return;

		g_cachedBlockedFeatureSceneEditSettings.clear();
		g_cachedAlteredFeatureSceneEditSettings.clear();
		g_cachedAlteredFeatureSceneEditAggregates.clear();
		g_cachedBlockedFeatureSceneEditAggregates.clear();
		g_cachedFeatureSceneEditFeature = featureShortName;
		g_cachedFeatureSceneEditRevision = revision;
		g_cachedFeatureSceneEditCaptureAllowed = captureAllowed;
		std::set<LogicalControlKey> blockedAggregates;
		for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
			if (captureAllowed && setting.featureShortName == featureShortName &&
				manager->IsFeatureSceneEditSettingAltered(setting.featureShortName, setting.settingPath, setting.settingKey)) {
				g_cachedAlteredFeatureSceneEditSettings.insert(&setting);
				if (setting.aggregateSemantic != SceneSettingsCatalog::AggregateSemantic::None)
					g_cachedAlteredFeatureSceneEditAggregates.try_emplace(GetLogicalControlKey(setting), &setting);
			}
			if (setting.featureShortName != featureShortName ||
				(captureAllowed && manager->IsFeatureSceneEditSetting(setting.featureShortName, setting.settingPath, setting.settingKey) &&
					!manager->IsFeatureSceneEditSettingOverwritten(setting.featureShortName, setting.settingPath, setting.settingKey)))
				continue;
			g_cachedBlockedFeatureSceneEditSettings.insert(&setting);
			if (setting.aggregateSemantic != SceneSettingsCatalog::AggregateSemantic::None) {
				blockedAggregates.insert(GetLogicalControlKey(setting));
				g_cachedBlockedFeatureSceneEditAggregates.try_emplace(
					GetLogicalControlKey(setting), &setting);
			}
		}
		if (blockedAggregates.empty())
			return;
		for (const auto& setting : SceneSettingsCatalog::GetSettings())
			if (setting.featureShortName == featureShortName &&
				blockedAggregates.contains(GetLogicalControlKey(setting)))
				g_cachedBlockedFeatureSceneEditSettings.insert(&setting);
	}

	bool MatchesSettingLabel(const SceneSettingsCatalog::SettingMetadata& setting,
		std::string_view label, bool choiceLabelsOnly)
	{
		if (!choiceLabelsOnly) {
			const auto displayName = setting.displayName.empty() ? setting.settingKey : setting.displayName;
			if (MatchesLocalizedLabel(label, displayName, setting.displayNameKey))
				return true;
		}
		for (std::size_t index = 0; index < setting.choiceCount; ++index) {
			const auto& choice = setting.choices[index];
			if (MatchesLocalizedLabel(label, choice.displayName, choice.displayNameKey))
				return true;
		}
		return false;
	}

	bool ShouldBlockSetting(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		if (!g_featureSceneEditing)
			return IsActiveSceneSetting(setting);
		return !IsFeatureSceneEditSetting(setting);
	}

	bool ShouldOutlineSetting(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return g_featureSceneEditing && g_cachedAlteredFeatureSceneEditSettings.contains(&setting);
	}

	struct SettingOutlineGuard
	{
		bool outlined;
		explicit SettingOutlineGuard(const SceneSettingsCatalog::SettingMetadata* setting) :
			outlined(setting && ShouldOutlineSetting(*setting))
		{
			if (!outlined)
				return;
			ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(ImGui::GetStyle().FrameBorderSize, Util::GetUIScale()));
		}
		~SettingOutlineGuard()
		{
			if (outlined) {
				ImGui::PopStyleVar();
				ImGui::PopStyleColor();
			}
		}
	};

	const SceneSettingsCatalog::SettingMetadata* FindUniqueBlockedSettingForLabel(
		const char* label, bool choiceLabelsOnly)
	{
		if (!label || GetVisibleLabel(label).empty())
			return nullptr;

		const auto featureShortName = g_currentFeature->GetShortName();
		const SceneSettingsCatalog::SettingMetadata* match = nullptr;
		for (const auto& candidate : SceneSettingsCatalog::GetSettings()) {
			if (candidate.featureShortName != featureShortName ||
				!SceneSettingsCatalog::IsSceneControllable(candidate) ||
				!MatchesSettingLabel(candidate, label, choiceLabelsOnly) ||
				(!ShouldBlockSetting(candidate) && !ShouldOutlineSetting(candidate)))
				continue;
			if (match && !IsSameLogicalControl(*match, candidate))
				return nullptr;
			match = &candidate;
		}
		return match;
	}

	template <class Aggregate>
	const SceneSettingsCatalog::SettingMetadata* FindBlockedAggregateSetting(const Aggregate& aggregate)
	{
		if (g_featureSceneEditing) {
			const auto match = g_cachedBlockedFeatureSceneEditAggregates.find(
				GetLogicalControlKey(aggregate));
			if (match != g_cachedBlockedFeatureSceneEditAggregates.end())
				return match->second;
			const auto altered = g_cachedAlteredFeatureSceneEditAggregates.find(GetLogicalControlKey(aggregate));
			return altered != g_cachedAlteredFeatureSceneEditAggregates.end() ? altered->second : nullptr;
		}
		for (const auto& candidate : SceneSettingsCatalog::GetSettings()) {
			if (candidate.featureShortName == aggregate.featureShortName &&
				candidate.serializedPath == aggregate.serializedPath &&
				candidate.serializedKey == aggregate.serializedKey &&
				candidate.aggregateSemantic == aggregate.aggregateSemantic &&
				candidate.aggregateStart == aggregate.aggregateStart &&
				candidate.aggregateCount == aggregate.aggregateCount && ShouldBlockSetting(candidate))
				return &candidate;
		}
		return nullptr;
	}

	struct RegisteredVirtualControlMatch
	{
		const SceneSettingsCatalog::SettingMetadata* setting = nullptr;
		bool metadataMatched = false;
	};

	RegisteredVirtualControlMatch FindRegisteredVirtualControlSetting(const char* label)
	{
		if (!label)
			return {};
		auto* window = ImGui::GetCurrentWindowRead();
		if (!window || window->IDStack.Size < 2)
			return {};

		const auto featureShortName = g_currentFeature->GetShortName();
		const auto parentSeed = window->IDStack[window->IDStack.Size - 2];
		const auto pushedId = window->IDStack.back();
		const auto itemId = ImHashStr(label, 0, pushedId);
		const SceneSettingsCatalog::VirtualAggregateControlMetadata* match = nullptr;
		for (const auto& control : SceneSettingsCatalog::GetVirtualAggregateControls()) {
			if (control.featureShortName != featureShortName ||
				ImHashStr(control.pushId.data(), control.pushId.size(), parentSeed) != pushedId ||
				ImHashStr(control.itemLabel.data(), control.itemLabel.size(), pushedId) != itemId)
				continue;
			if (match)
				return { nullptr, true };
			match = &control;
		}
		return { match ? FindBlockedAggregateSetting(*match) : nullptr, match != nullptr };
	}

	const SceneSettingsCatalog::SettingMetadata* FindVirtualControlSetting(
		const char* label, bool choiceLabelsOnly)
	{
		const auto registered = FindRegisteredVirtualControlSetting(label);
		if (registered.metadataMatched)
			return registered.setting;
		return FindUniqueBlockedSettingForLabel(label, choiceLabelsOnly);
	}

	const SceneSettingsCatalog::SettingMetadata* FindControlSetting(
		const char* label, const void* valueAddress, bool choiceLabelsOnly = false)
	{
		if (!IsSceneControlGuardActive() || !g_currentFeature)
			return nullptr;

		const void* settingAddress = Util::GetActiveControlStorageAddress();
		if (!settingAddress)
			settingAddress = valueAddress;
		auto* setting = SceneSettingsCatalog::FindSettingForControl(g_currentFeature, settingAddress);
		if (!setting)
			return FindVirtualControlSetting(label, choiceLabelsOnly);
		if (!SceneSettingsManager::IsSceneSettingAllowed(
				setting->featureShortName, setting->settingPath, setting->settingKey))
			return g_featureSceneEditing ? setting : nullptr;
		if (setting->aggregateSemantic == SceneSettingsCatalog::AggregateSemantic::None)
			return ShouldBlockSetting(*setting) || ShouldOutlineSetting(*setting) ? setting : nullptr;
		return FindBlockedAggregateSetting(*setting);
	}

	void DrawSceneSettingTooltip()
	{
		const ImGuiLastItemData lastItem = GImGui->LastItemData;
		const bool previousAllow = g_allowSceneSettingTooltip;
		g_allowSceneSettingTooltip = true;
		{
			if (auto tooltip = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"%s",
					g_featureSceneEditing ?
						T("feature.scene_manager.edit.unavailable_tooltip",
							"This setting is not available for the selected scene.") :
						T("feature.scene_manager.controlled_tooltip",
							"Disable scene-specific settings to edit this setting."));
			}
		}
		g_allowSceneSettingTooltip = previousAllow;
		GImGui->LastItemData = lastItem;
	}

	void FinishControlledItem()
	{
		CaptureControlledItem();
		if (!g_sceneControlledGroupStack.empty()) {
			g_sceneControlledGroupStack.back() = true;
			return;
		}
		DrawSceneSettingTooltip();
	}

	bool TrackFeatureSettingMutation(bool changed)
	{
		if (changed && g_currentFeature)
			g_featureSettingMutation = true;
		return changed;
	}

	template <class Draw>
	bool DrawControl(const char* label, const void* valueAddress, Draw draw)
	{
		if (g_controlDetourDepth > 0)
			return draw();
		ClearControlledItem();
		const auto* setting = FindControlSetting(label, valueAddress);
		SettingOutlineGuard outline(setting);
		if (!setting || !ShouldBlockSetting(*setting))
			return TrackFeatureSettingMutation(draw());

		ImGui::BeginDisabled();
		{
			ControlDetourScope detourScope;
			draw();
		}
		ImGui::EndDisabled();
		FinishControlledItem();
		return false;
	}

	void BeginGroupDetour()
	{
		g_beginGroup();
		if (IsSceneControlGuardActive())
			g_sceneControlledGroupStack.push_back(false);
	}

	void EndGroupDetour()
	{
		g_endGroup();
		if (!IsSceneControlGuardActive() || g_sceneControlledGroupStack.empty())
			return;
		const bool controlled = g_sceneControlledGroupStack.back();
		g_sceneControlledGroupStack.pop_back();
		if (!controlled) {
			ClearControlledItem();
			return;
		}
		FinishControlledItem();
	}

	bool IsItemHoveredDetour(ImGuiHoveredFlags flags)
	{
		if (!g_allowSceneSettingTooltip && IsSceneControlGuardActive() && IsControlledItem())
			return false;
		return g_isItemHovered(flags);
	}

	void MarkItemEditedDetour(ImGuiID id)
	{
		g_markItemEdited(id);
		if (g_controlDetourDepth == 0 && g_currentFeature)
			g_featureSettingMutation = true;
	}

	template <class Target>
	bool Attach(Target& target, Target detour)
	{
		return DetourAttach(reinterpret_cast<PVOID*>(&target), reinterpret_cast<PVOID>(detour)) == NO_ERROR;
	}

	bool CheckboxDetour(const char* label, bool* value)
	{
		return DrawControl(label, value, [&] { return g_checkbox(label, value); });
	}

	bool ButtonDetour(const char* label, const ImVec2& size)
	{
		if (g_controlDetourDepth > 0)
			return g_button(label, size);
		ClearControlledItem();
		const auto* setting = FindControlSetting(label, nullptr, true);
		SettingOutlineGuard outline(setting);
		if (!setting || !ShouldBlockSetting(*setting))
			return TrackFeatureSettingMutation(g_button(label, size));

		ImGui::BeginDisabled();
		{
			ControlDetourScope detourScope;
			g_button(label, size);
		}
		ImGui::EndDisabled();
		FinishControlledItem();
		return false;
	}

	bool CheckboxFlagsIntDetour(const char* label, int* flags, int flagsValue)
	{
		return DrawControl(label, flags, [&] { return g_checkboxFlagsInt(label, flags, flagsValue); });
	}

	bool CheckboxFlagsUIntDetour(const char* label, unsigned int* flags, unsigned int flagsValue)
	{
		return DrawControl(label, flags, [&] { return g_checkboxFlagsUInt(label, flags, flagsValue); });
	}

	bool RadioButtonIntDetour(const char* label, int* value, int buttonValue)
	{
		return DrawControl(label, value, [&] { return g_radioButtonInt(label, value, buttonValue); });
	}

	bool RadioButtonBoolDetour(const char* label, bool active)
	{
		return DrawControl(label, nullptr, [&] { return g_radioButtonBool(label, active); });
	}

	bool ComboItemsDetour(const char* label, int* currentItem, const char* const* items, int itemsCount, int popupMaxHeightInItems)
	{
		return DrawControl(label, currentItem, [&] { return g_comboItems(label, currentItem, items, itemsCount, popupMaxHeightInItems); });
	}

	bool ComboStringDetour(const char* label, int* currentItem, const char* itemsSeparatedByZeros, int popupMaxHeightInItems)
	{
		return DrawControl(label, currentItem, [&] { return g_comboString(label, currentItem, itemsSeparatedByZeros, popupMaxHeightInItems); });
	}

	bool ComboGetterDetour(const char* label, int* currentItem, const char* (*getter)(void*, int), void* userData, int itemsCount, int popupMaxHeightInItems)
	{
		return DrawControl(label, currentItem, [&] { return g_comboGetter(label, currentItem, getter, userData, itemsCount, popupMaxHeightInItems); });
	}

	bool DragFloatDetour(const char* label, float* value, float speed, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_dragFloat(label, value, speed, min, max, format, flags); });
	}

	bool DragFloat2Detour(const char* label, float* value, float speed, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_dragFloat2(label, value, speed, min, max, format, flags); });
	}

	bool DragFloat3Detour(const char* label, float* value, float speed, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_dragFloat3(label, value, speed, min, max, format, flags); });
	}

	bool DragFloat4Detour(const char* label, float* value, float speed, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_dragFloat4(label, value, speed, min, max, format, flags); });
	}

	bool DragIntDetour(const char* label, int* value, float speed, int min, int max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_dragInt(label, value, speed, min, max, format, flags); });
	}

	bool DragInt2Detour(const char* label, int* value, float speed, int min, int max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_dragInt2(label, value, speed, min, max, format, flags); });
	}

	bool DragInt3Detour(const char* label, int* value, float speed, int min, int max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_dragInt3(label, value, speed, min, max, format, flags); });
	}

	bool DragInt4Detour(const char* label, int* value, float speed, int min, int max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_dragInt4(label, value, speed, min, max, format, flags); });
	}

	bool DragScalarDetour(const char* label, ImGuiDataType dataType, void* value, float speed,
		const void* min, const void* max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value,
			[&] { return g_dragScalar(label, dataType, value, speed, min, max, format, flags); });
	}

	bool DragScalarNDetour(const char* label, ImGuiDataType dataType, void* value, int components,
		float speed, const void* min, const void* max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value,
			[&] { return g_dragScalarN(label, dataType, value, components, speed, min, max, format, flags); });
	}

	bool SliderFloatDetour(const char* label, float* value, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_sliderFloat(label, value, min, max, format, flags); });
	}

	bool SliderFloat2Detour(const char* label, float* value, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_sliderFloat2(label, value, min, max, format, flags); });
	}

	bool SliderFloat3Detour(const char* label, float* value, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_sliderFloat3(label, value, min, max, format, flags); });
	}

	bool SliderFloat4Detour(const char* label, float* value, float min, float max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_sliderFloat4(label, value, min, max, format, flags); });
	}

	bool SliderIntDetour(const char* label, int* value, int min, int max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_sliderInt(label, value, min, max, format, flags); });
	}

	bool SliderInt2Detour(const char* label, int* value, int min, int max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_sliderInt2(label, value, min, max, format, flags); });
	}

	bool SliderInt3Detour(const char* label, int* value, int min, int max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_sliderInt3(label, value, min, max, format, flags); });
	}

	bool SliderInt4Detour(const char* label, int* value, int min, int max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value, [&] { return g_sliderInt4(label, value, min, max, format, flags); });
	}

	bool SliderScalarDetour(const char* label, ImGuiDataType dataType, void* value,
		const void* min, const void* max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value,
			[&] { return g_sliderScalar(label, dataType, value, min, max, format, flags); });
	}

	bool SliderScalarNDetour(const char* label, ImGuiDataType dataType, void* value, int components,
		const void* min, const void* max, const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value,
			[&] { return g_sliderScalarN(label, dataType, value, components, min, max, format, flags); });
	}

	bool SliderAngleDetour(const char* label, float* value, float minDegrees, float maxDegrees,
		const char* format, ImGuiSliderFlags flags)
	{
		return DrawControl(label, value,
			[&] { return g_sliderAngle(label, value, minDegrees, maxDegrees, format, flags); });
	}

	bool PercentageSliderDetour(const char* label, float* value, float minPercent, float maxPercent,
		const char* format)
	{
		return DrawControl(label, value,
			[&] { return g_percentageSlider(label, value, minPercent, maxPercent, format); });
	}

	bool InputFloatDetour(const char* label, float* value, float step, float stepFast, const char* format, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value, [&] { return g_inputFloat(label, value, step, stepFast, format, flags); });
	}

	bool InputFloat2Detour(const char* label, float* value, const char* format, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value, [&] { return g_inputFloat2(label, value, format, flags); });
	}

	bool InputFloat3Detour(const char* label, float* value, const char* format, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value, [&] { return g_inputFloat3(label, value, format, flags); });
	}

	bool InputFloat4Detour(const char* label, float* value, const char* format, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value, [&] { return g_inputFloat4(label, value, format, flags); });
	}

	bool InputIntDetour(const char* label, int* value, int step, int stepFast, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value, [&] { return g_inputInt(label, value, step, stepFast, flags); });
	}

	bool InputInt2Detour(const char* label, int* value, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value, [&] { return g_inputInt2(label, value, flags); });
	}

	bool InputInt3Detour(const char* label, int* value, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value, [&] { return g_inputInt3(label, value, flags); });
	}

	bool InputInt4Detour(const char* label, int* value, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value, [&] { return g_inputInt4(label, value, flags); });
	}

	bool InputScalarDetour(const char* label, ImGuiDataType dataType, void* value,
		const void* step, const void* stepFast, const char* format, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value,
			[&] { return g_inputScalar(label, dataType, value, step, stepFast, format, flags); });
	}

	bool InputScalarNDetour(const char* label, ImGuiDataType dataType, void* value, int components,
		const void* step, const void* stepFast, const char* format, ImGuiInputTextFlags flags)
	{
		return DrawControl(label, value,
			[&] { return g_inputScalarN(label, dataType, value, components, step, stepFast, format, flags); });
	}

	bool ColorEdit3Detour(const char* label, float* color, ImGuiColorEditFlags flags)
	{
		return DrawControl(label, color, [&] { return g_colorEdit3(label, color, flags); });
	}

	bool ColorEdit4Detour(const char* label, float* color, ImGuiColorEditFlags flags)
	{
		return DrawControl(label, color, [&] { return g_colorEdit4(label, color, flags); });
	}

	bool BeginComboDetour(const char* label, const char* previewValue, ImGuiComboFlags flags)
	{
		if (g_controlDetourDepth > 0)
			return g_beginCombo(label, previewValue, flags);
		ClearControlledItem();
		const auto* setting = FindControlSetting(label, nullptr);
		SettingOutlineGuard outline(setting);
		if (!setting || !ShouldBlockSetting(*setting))
			return g_beginCombo(label, previewValue, flags);

		ImGui::BeginDisabled();
		bool opened = false;
		{
			ControlDetourScope detourScope;
			opened = g_beginCombo(label, previewValue, flags);
		}
		if (opened)
			ImGui::EndCombo();
		ImGui::EndDisabled();
		FinishControlledItem();
		return false;
	}
}

namespace SceneSettingsUIHooks
{
	FeatureDrawGuard::FeatureDrawGuard(Feature* feature, bool sceneControlled, bool sceneEditing) :
		previousFeature(g_currentFeature),
		previousSceneControlled(g_sceneSettingsActive),
		previousSceneEditing(g_featureSceneEditing)
	{
		g_sceneControlFrames.push_back({ g_sceneControlledItem,
			std::move(g_sceneControlledGroupStack),
			g_blockedFeatureSceneEditSettings,
			g_featureSettingMutation });
		g_sceneControlledGroupStack.clear();
		g_blockedFeatureSceneEditSettings = nullptr;
		g_featureSettingMutation = false;
		g_currentFeature = feature;
		g_featureSceneEditing = sceneEditing && feature != nullptr;
		g_sceneSettingsActive = sceneControlled && !g_featureSceneEditing && feature != nullptr;
		if (g_featureSceneEditing) {
			RefreshBlockedFeatureSceneEditSettings(*feature);
			g_blockedFeatureSceneEditSettings = &g_cachedBlockedFeatureSceneEditSettings;
		}
		ClearControlledItem();
	}

	FeatureDrawGuard::~FeatureDrawGuard()
	{
		if (g_currentFeature && g_featureSettingMutation) {
			if (g_featureSceneEditing)
				SceneSettingsManager::GetSingleton()->CaptureFeatureSceneEditChanges(g_currentFeature);
			else
				SceneSettingsManager::GetSingleton()->CaptureExternalFeatureChanges(g_currentFeature);
		}
		if (!g_sceneControlFrames.empty()) {
			g_sceneControlledItem = g_sceneControlFrames.back().item;
			g_sceneControlledGroupStack = std::move(g_sceneControlFrames.back().groups);
			g_blockedFeatureSceneEditSettings = g_sceneControlFrames.back().blockedEditSettings;
			g_featureSettingMutation = g_sceneControlFrames.back().featureSettingMutation;
			g_sceneControlFrames.pop_back();
		} else {
			ClearControlledItem();
			g_sceneControlledGroupStack.clear();
			g_blockedFeatureSceneEditSettings = nullptr;
			g_featureSettingMutation = false;
		}
		g_currentFeature = previousFeature;
		g_sceneSettingsActive = previousSceneControlled;
		g_featureSceneEditing = previousSceneEditing;
	}

	void Install()
	{
		if (g_installed)
			return;

		auto result = DetourTransactionBegin();
		if (result != NO_ERROR) {
			logger::warn("[SceneSettings] Failed to begin ImGui hook transaction: {}", result);
			return;
		}

		result = DetourUpdateThread(GetCurrentThread());
		if (result != NO_ERROR) {
			DetourTransactionAbort();
			logger::warn("[SceneSettings] Failed to enlist the current thread for ImGui hooks: {}", result);
			return;
		}

		bool attached =
			Attach(g_checkbox, CheckboxDetour) &&
			Attach(g_button, ButtonDetour) &&
			Attach(g_checkboxFlagsInt, CheckboxFlagsIntDetour) &&
			Attach(g_checkboxFlagsUInt, CheckboxFlagsUIntDetour) &&
			Attach(g_radioButtonInt, RadioButtonIntDetour) &&
			Attach(g_radioButtonBool, RadioButtonBoolDetour) &&
			Attach(g_comboItems, ComboItemsDetour) &&
			Attach(g_comboString, ComboStringDetour) &&
			Attach(g_comboGetter, ComboGetterDetour) &&
			Attach(g_dragFloat, DragFloatDetour) &&
			Attach(g_dragFloat2, DragFloat2Detour) &&
			Attach(g_dragFloat3, DragFloat3Detour) &&
			Attach(g_dragFloat4, DragFloat4Detour) &&
			Attach(g_dragInt, DragIntDetour) &&
			Attach(g_dragInt2, DragInt2Detour) &&
			Attach(g_dragInt3, DragInt3Detour) &&
			Attach(g_dragInt4, DragInt4Detour) &&
			Attach(g_dragScalar, DragScalarDetour) &&
			Attach(g_dragScalarN, DragScalarNDetour) &&
			Attach(g_sliderFloat, SliderFloatDetour) &&
			Attach(g_sliderFloat2, SliderFloat2Detour) &&
			Attach(g_sliderFloat3, SliderFloat3Detour) &&
			Attach(g_sliderFloat4, SliderFloat4Detour) &&
			Attach(g_sliderInt, SliderIntDetour) &&
			Attach(g_sliderInt2, SliderInt2Detour) &&
			Attach(g_sliderInt3, SliderInt3Detour) &&
			Attach(g_sliderInt4, SliderInt4Detour) &&
			Attach(g_sliderScalar, SliderScalarDetour) &&
			Attach(g_sliderScalarN, SliderScalarNDetour) &&
			Attach(g_sliderAngle, SliderAngleDetour) &&
			Attach(g_percentageSlider, PercentageSliderDetour) &&
			Attach(g_inputFloat, InputFloatDetour) &&
			Attach(g_inputFloat2, InputFloat2Detour) &&
			Attach(g_inputFloat3, InputFloat3Detour) &&
			Attach(g_inputFloat4, InputFloat4Detour) &&
			Attach(g_inputInt, InputIntDetour) &&
			Attach(g_inputInt2, InputInt2Detour) &&
			Attach(g_inputInt3, InputInt3Detour) &&
			Attach(g_inputInt4, InputInt4Detour) &&
			Attach(g_inputScalar, InputScalarDetour) &&
			Attach(g_inputScalarN, InputScalarNDetour) &&
			Attach(g_colorEdit3, ColorEdit3Detour) &&
			Attach(g_colorEdit4, ColorEdit4Detour) &&
			Attach(g_beginCombo, BeginComboDetour) &&
			Attach(g_beginGroup, BeginGroupDetour) &&
			Attach(g_endGroup, EndGroupDetour) &&
			Attach(g_isItemHovered, IsItemHoveredDetour) &&
			Attach(g_markItemEdited, MarkItemEditedDetour);

		if (!attached) {
			DetourTransactionAbort();
			logger::warn("[SceneSettings] Failed to attach ImGui scene setting hooks");
			return;
		}

		result = DetourTransactionCommit();
		if (result != NO_ERROR) {
			logger::warn("[SceneSettings] Failed to install ImGui scene setting hooks: {}", result);
			return;
		}

		g_installed = true;
		logger::info("[SceneSettings] Installed ImGui scene setting hooks");
	}
}
