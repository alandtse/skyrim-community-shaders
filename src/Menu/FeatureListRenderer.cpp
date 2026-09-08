#include "FeatureListRenderer.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <imgui.h>
#include <numbers>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include "Feature.h"
#include "FeatureConstraints.h"
#include "FeatureIssues.h"
#include "Features/CSEditor.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/HomePageRenderer.h"
#include "Menu/PerformanceRenderer.h"
#include "Menu/ProfilingRenderer.h"
#include "Menu/ThemeManager.h"
#include "SceneSettingsManager.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Util.h"
#include "Utils/UI.h"
#include "WeatherVariableRegistry.h"

namespace
{
	constexpr float FEATURE_ACTION_BUTTON_SCALE = 1.4f;
	constexpr float FEATURE_ACTION_CHECKMARK_LEFT_OFFSET = 2.0f;
	constexpr float FEATURE_PAGE_BOTTOM_TOLERANCE = 1.0f;
	constexpr float FEATURE_PAGE_LAYOUT_EPSILON = 0.5f;
	constexpr float FEATURE_ACTION_ICON_HALF_WIDTH_RATIO = 0.24f;
	constexpr float FEATURE_ACTION_ICON_LINE_SPACING_RATIO = 0.16f;
	constexpr float FEATURE_ACTION_ICON_STROKE_RATIO = 0.07f;
	constexpr float FEATURE_ACTION_ICON_FLIGHT_ARC_RATIO = 0.06f;
	constexpr float FEATURE_ACTION_ICON_FULL_TURN = std::numbers::pi_v<float> * 2.0f;
	constexpr float FEATURE_ACTION_ICON_SNAP_RATIO = 0.05f;
	constexpr float FEATURE_ACTION_ICON_RESPONSE_SPEED = 21.0f;
	constexpr float FEATURE_ACTION_ICON_START_BOOST = 13.0f;
	constexpr float FEATURE_ACTION_ICON_MAX_DELTA_TIME = 1.0f / 30.0f;
	constexpr float FEATURE_ACTION_ICON_FINISH_BIAS = 0.04f;

	struct FeaturePageLayoutState
	{
		bool measured = false;
		float materialHeight = 0.0f;
		float contentHeight = 0.0f;
		float scrollY = 0.0f;
		float scrollMaxY = 0.0f;
	};

	std::unordered_map<std::string, FeaturePageLayoutState> g_featurePageLayouts;
	float g_featureActionsIconProgress = 0.0f;

	void DrawFeatureActionsIcon(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, float progress)
	{
		IM_ASSERT(drawList != nullptr);
		IM_ASSERT(max.x >= min.x && max.y >= min.y);

		const float iconSize = std::min(max.x - min.x, max.y - min.y);
		const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
		const float halfWidth = iconSize * FEATURE_ACTION_ICON_HALF_WIDTH_RATIO;
		const float lineSpacing = iconSize * FEATURE_ACTION_ICON_LINE_SPACING_RATIO;
		const float triangleHalfHeight = halfWidth * std::numbers::sqrt3_v<float> * 0.5f;
		const float strokeWidth = std::max(1.0f, iconSize * FEATURE_ACTION_ICON_STROKE_RATIO);
		const float animationProgress = std::clamp(progress, 0.0f, 1.0f);
		const float snapPhase = std::sin(std::numbers::pi_v<float> * 2.0f * animationProgress) *
		                        std::sin(std::numbers::pi_v<float> * animationProgress);
		const float morphProgress = std::clamp(
			animationProgress + FEATURE_ACTION_ICON_SNAP_RATIO * snapPhase, 0.0f, 1.0f);
		const float flightPhase = std::sin(std::numbers::pi_v<float> * animationProgress);
		const auto interpolate = [morphProgress](float start, float end) {
			return std::lerp(start, end, morphProgress);
		};
		struct LineTransform
		{
			ImVec2 center;
			float angle;
		};

		const ImVec2 triangleLeft(center.x - halfWidth, center.y - triangleHalfHeight);
		const ImVec2 triangleRight(center.x + halfWidth, center.y - triangleHalfHeight);
		const ImVec2 triangleBottom(center.x, center.y + triangleHalfHeight);
		const std::array<LineTransform, 3> hamburgerLines = { { { ImVec2(center.x, center.y - lineSpacing), 0.0f },
			{ center, 0.0f },
			{ ImVec2(center.x, center.y + lineSpacing), 0.0f } } };
		const std::array<LineTransform, 3> triangleLines = { { { ImVec2((triangleBottom.x + triangleRight.x) * 0.5f, (triangleBottom.y + triangleRight.y) * 0.5f),
																   std::atan2(triangleRight.y - triangleBottom.y, triangleRight.x - triangleBottom.x) - FEATURE_ACTION_ICON_FULL_TURN },
			{ ImVec2((triangleLeft.x + triangleBottom.x) * 0.5f, (triangleLeft.y + triangleBottom.y) * 0.5f),
				std::atan2(triangleBottom.y - triangleLeft.y, triangleBottom.x - triangleLeft.x) + FEATURE_ACTION_ICON_FULL_TURN },
			{ ImVec2((triangleLeft.x + triangleRight.x) * 0.5f, triangleLeft.y), FEATURE_ACTION_ICON_FULL_TURN } } };
		const std::array<ImVec2, 3> flightOffsets = { { ImVec2(-iconSize * FEATURE_ACTION_ICON_FLIGHT_ARC_RATIO, lineSpacing / std::numbers::pi_v<float>),
			ImVec2(-iconSize * FEATURE_ACTION_ICON_FLIGHT_ARC_RATIO, 0.0f),
			ImVec2(iconSize * FEATURE_ACTION_ICON_FLIGHT_ARC_RATIO, -lineSpacing / std::numbers::pi_v<float>) } };
		const ImU32 lineColor = ImGui::GetColorU32(ImGuiCol_Text);
		const float capRadius = strokeWidth * 0.5f;

		for (std::size_t i = 0; i < hamburgerLines.size(); ++i) {
			const ImVec2 lineCenter(
				interpolate(hamburgerLines[i].center.x, triangleLines[i].center.x) + flightOffsets[i].x * flightPhase,
				interpolate(hamburgerLines[i].center.y, triangleLines[i].center.y) + flightOffsets[i].y * flightPhase);
			const float lineAngle = interpolate(hamburgerLines[i].angle, triangleLines[i].angle);
			const ImVec2 halfLine(std::cos(lineAngle) * halfWidth, std::sin(lineAngle) * halfWidth);
			const ImVec2 lineStart(lineCenter.x - halfLine.x, lineCenter.y - halfLine.y);
			const ImVec2 lineEnd(lineCenter.x + halfLine.x, lineCenter.y + halfLine.y);
			const ImVec2 rasterOffset(0.5f, 0.5f);
			const float capStartAngle = lineAngle + std::numbers::pi_v<float> * 0.5f;
			drawList->PathArcTo(ImVec2(lineStart.x + rasterOffset.x, lineStart.y + rasterOffset.y), capRadius,
				capStartAngle, capStartAngle + std::numbers::pi_v<float>);
			drawList->PathArcTo(ImVec2(lineEnd.x + rasterOffset.x, lineEnd.y + rasterOffset.y), capRadius,
				capStartAngle + std::numbers::pi_v<float>, capStartAngle + std::numbers::pi_v<float> * 2.0f);
			drawList->PathFillConvex(lineColor);
		}
	}

	// Core built-in menu names that always appear first in the menu list
	// These are canonical identifiers used for logic — NOT translated
	constexpr std::array<const char*, 6> CORE_MENU_NAMES = {
		"Home", "General", "Performance", "Advanced", "Profiling", "Display"
	};

	const char* GetCoreMenuDisplayName(const char* canonicalName)
	{
		if (std::strcmp(canonicalName, "Home") == 0)
			return T("menu.features.home", "Home");
		if (std::strcmp(canonicalName, "General") == 0)
			return T("menu.features.general", "General");
		if (std::strcmp(canonicalName, "Performance") == 0)
			return T("menu.features.performance", "Performance");
		if (std::strcmp(canonicalName, "Advanced") == 0)
			return T("menu.features.advanced", "Advanced");
		if (std::strcmp(canonicalName, "Profiling") == 0)
			return T("menu.features.profiling", "Profiling");
		if (std::strcmp(canonicalName, "Display") == 0)
			return T("menu.features.display", "Display");
		return canonicalName;
	}

	bool IsCoreMenu(const std::string& canonicalId)
	{
		return std::find(CORE_MENU_NAMES.begin(), CORE_MENU_NAMES.end(), canonicalId) != CORE_MENU_NAMES.end();
	}

	// Color for the [ALPHA]/[BETA] stage marker. Alpha (less stable) reads as an error,
	// Beta as a warning.
	ImVec4 StageTagColor(Feature::ReleaseStage stage)
	{
		const auto& statusPalette = globals::menu->GetTheme().StatusPalette;
		return stage == Feature::ReleaseStage::Alpha ? statusPalette.Error : statusPalette.Warning;
	}

	/**
	 * @brief Determines if the left feature panel should be visible based on auto-hide settings and mouse position
	 * @return true if panel should be visible, false if it should be hidden
	 */
	bool ShouldShowLeftPanel()
	{
		bool autoHideEnabled = globals::menu->GetSettings().AutoHideFeatureList;
		static bool leftPanelVisible = true;
		static float hoverStartTime = 0.0f;
		static bool wasHovering = false;

		if (!autoHideEnabled) {
			leftPanelVisible = true;
			return true;
		}

		// Get mouse position and window bounds
		ImVec2 mousePos = ImGui::GetMousePos();
		ImVec2 windowPos = ImGui::GetWindowPos();
		ImVec2 windowSize = ImGui::GetWindowSize();
		float currentTime = static_cast<float>(ImGui::GetTime());

		// Use constants for auto-hide behavior
		const float activationZoneWidth = ThemeManager::Constants::AUTOHIDE_ACTIVATION_ZONE_WIDTH;
		const float expandDelay = ThemeManager::Constants::AUTOHIDE_EXPAND_DELAY;
		const float panelWidth = windowSize.x * ThemeManager::Constants::AUTOHIDE_PANEL_WIDTH_RATIO;

		// Calculate relative X position
		const float relativeX = mousePos.x - windowPos.x;

		// For activation: only check if mouse is at left edge (allow any Y position for easier triggering)
		// Prevent negative X from triggering, but don't restrict Y-axis for activation
		bool mouseInActivationZone = relativeX >= 0.0f && relativeX < activationZoneWidth;

		// For staying visible: check both X and Y to ensure mouse is actually over the panel area
		const bool mouseOverPanelX = relativeX >= 0.0f && relativeX < panelWidth;
		const bool mouseOverPanelY = mousePos.y >= windowPos.y && mousePos.y <= (windowPos.y + windowSize.y);
		bool mouseOverPanel = leftPanelVisible && mouseOverPanelX && mouseOverPanelY;

		// Track hover start time
		if (mouseInActivationZone && !wasHovering) {
			hoverStartTime = currentTime;
			wasHovering = true;
		} else if (!mouseInActivationZone) {
			wasHovering = false;
		}

		// Expand only after delay has elapsed
		bool shouldExpand = mouseInActivationZone && (currentTime - hoverStartTime >= expandDelay);

		// Update visibility: expand with delay, or stay visible while mouse is over panel
		if (shouldExpand || mouseOverPanel) {
			leftPanelVisible = true;
		} else if (!mouseOverPanel && !mouseInActivationZone) {
			leftPanelVisible = false;
		}

		return leftPanelVisible;
	}

	void SeparatorTextWithFont(const char* text, Menu::FontRole role)
	{
		MenuFonts::FontRoleGuard guard(role);
		ImGui::SeparatorText(text);
	}

	void SeparatorTextWithFont(const std::string& text, Menu::FontRole role)
	{
		SeparatorTextWithFont(text.c_str(), role);
	}

	bool BeginTabItemWithFont(const char* label, Menu::FontRole role, ImGuiTabItemFlags flags = ImGuiTabItemFlags_None)
	{
		return MenuFonts::BeginTabItemWithFont(label, role, flags);
	}

	std::string TranslateFeatureCategory(std::string_view category)
	{
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
		if (category == "Post-Processing")
			return T("feature.category.post_processing", "Post-Processing");
		if (category == FeatureCategories::kOther)
			return T("feature.category.other", "Other");
		if (category == FeatureCategories::kSky)
			return T("feature.category.sky", "Sky");
		if (category == FeatureCategories::kUtility)
			return T("feature.category.utility", "Utility");
		if (category == FeatureCategories::kWater)
			return T("feature.category.water", "Water");

		return std::string(category);
	}

	/**
	 * @brief Draws a feature header with the feature name in large text and version in smaller text
	 * @param featureName The display name of the feature
	 * @param version The version string (can be empty)
	 * @param description Short description shown below the title (single line, truncated if too long)
	 * @param minimumTitleHeight Minimum height reserved for title-row controls
	 * @return The height of just the title line (for button alignment)
	 */
	float DrawFeatureHeader(const std::string& featureName, const std::string& version, const std::string& description = "", const std::string& stageTag = "", ImVec4 stageColor = {}, float minimumTitleHeight = 0.0f)
	{
		IM_ASSERT(minimumTitleHeight >= 0.0f);

		auto& themeSettings = globals::menu->GetTheme();
		auto& palette = themeSettings.Palette;
		auto& featureHeading = themeSettings.FeatureHeading;

		// Sanitize and clamp to UI slider range to prevent malformed theme JSON from destabilizing layout
		float titleScale = featureHeading.FeatureTitleScale;
		if (!std::isfinite(titleScale)) {
			titleScale = ThemeManager::Constants::DEFAULT_FEATURE_TITLE_SCALE;
		}
		titleScale = std::clamp(titleScale, 1.0f, 3.0f);

		ImVec2 startPos = ImGui::GetCursorScreenPos();

		// Calculate title size
		ImVec2 titleSize;
		{
			MenuFonts::FontRoleGuard titleGuard(Menu::FontRole::Title);
			titleSize = ImGui::CalcTextSize(featureName.c_str());
			titleSize.x *= titleScale;
			titleSize.y *= titleScale;
		}

		const float titleOnlyHeight = std::max(titleSize.y, minimumTitleHeight);
		const float titleY = startPos.y + (titleOnlyHeight - titleSize.y) * 0.5f;
		ImGui::SetCursorScreenPos(ImVec2(startPos.x, titleY));
		{
			MenuFonts::FontRoleGuard titleGuard(Menu::FontRole::Title);
			ImGui::SetWindowFontScale(titleScale);
			ImGui::TextUnformatted(featureName.c_str());
			ImGui::SetWindowFontScale(1.0f);
		}

		// Running x for bottom-aligned annotations (stage tag, then version) to the right of the title
		float annotationX = startPos.x + titleSize.x + ImGui::GetStyle().ItemSpacing.x;

		// Draw stage marker ([ALPHA]/[BETA]) on same line, bottom-aligned
		if (!stageTag.empty()) {
			ImVec2 tagSize;
			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				tagSize = ImGui::CalcTextSize(stageTag.c_str());
				tagSize.x *= titleScale;
				tagSize.y *= titleScale;
			}

			ImGui::SetCursorScreenPos(ImVec2(annotationX, titleY + titleSize.y - tagSize.y));
			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				ImGui::SetWindowFontScale(titleScale);
				ImGui::TextColored(stageColor, "%s", stageTag.c_str());
				ImGui::SetWindowFontScale(1.0f);
			}

			annotationX += tagSize.x + ImGui::GetStyle().ItemSpacing.x;
		}

		// Draw version on same line with Body font, bottom-aligned if version exists
		if (!version.empty()) {
			// Format version: replace dashes with dots for consistency
			std::string formattedVersion = version;
			std::replace(formattedVersion.begin(), formattedVersion.end(), '-', '.');

			// Calculate version text size at scaled size
			ImVec2 versionSize;
			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				versionSize = ImGui::CalcTextSize(("v" + formattedVersion).c_str());
				versionSize.x *= titleScale;
				versionSize.y *= titleScale;
			}

			// Position version text: right of the stage tag (or title), bottom-aligned
			float versionX = annotationX;
			float versionY = titleY + titleSize.y - versionSize.y;

			ImGui::SetCursorScreenPos(ImVec2(versionX, versionY));

			// Use dimmed text color for version
			ImVec4 versionColor = palette.Text;
			versionColor.w *= ThemeManager::Constants::VERSION_TEXT_OPACITY;

			{
				MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
				ImGui::SetWindowFontScale(titleScale);
				ImGui::TextColored(versionColor, "v%s", formattedVersion.c_str());
				ImGui::SetWindowFontScale(1.0f);
			}
		}

		ImGui::SetCursorScreenPos(
			ImVec2(startPos.x, startPos.y + titleOnlyHeight + ImGui::GetStyle().ItemSpacing.y * 0.25f));

		// Draw description if provided (wrapped to content width)
		if (!description.empty()) {
			MenuFonts::FontRoleGuard subtextGuard(Menu::FontRole::Subtext);
			ImVec4 descColor = palette.Text;
			descColor.w *= 0.7f;  // Slightly dimmed
			ImGui::PushStyleColor(ImGuiCol_Text, descColor);
			ImGui::TextWrapped("%s", description.c_str());
			ImGui::PopStyleColor();
		}

		// Draw plain separator below
		ImGui::Separator();

		return titleOnlyHeight;
	}

	// ---------------------------------------------------------------------------
	// Persistent state for the reactive constraint warning popup.
	// DrawMenuVisitor is reconstructed every frame (it's a temporary passed to
	// std::visit), so member state is lost immediately.  These file-scope
	// variables survive across frames so the popup can actually render.
	// ---------------------------------------------------------------------------

	// Set of constraint keys we have already "seen" (and therefore warned about
	// or suppressed).  Keyed as "featureShortName|settingPath".
	std::unordered_set<std::string> g_knownConstraintKeys;
	bool g_knownConstraintKeysInitialised = false;

	// Pending popup state: non-empty when we have new constraints to show.
	bool g_reactiveWarningShow = false;
	std::vector<std::pair<FeatureConstraints::SettingId, FeatureConstraints::ConstraintResult>> g_reactiveWarningConstraints;

	// "Don't show again" checkbox state inside the modal (reset each time popup opens).
	bool g_dontShowAgainCheckbox = false;

	Util::FlyoutState g_featureActionsFlyout;
	std::string g_featureActionsFlyoutFeature;
}

void FeatureListRenderer::RenderFeatureList(
	float footerHeight,
	size_t& selectedMenu,
	std::string& featureSearch,
	std::string& pendingFeatureSelection,
	std::map<std::string, bool>& categoryExpansionStates,
	const std::function<void()>& drawGeneralSettings,
	const std::function<void()>& drawAdvancedSettings)
{
	ImGui::BeginChild("Menus Table", ImVec2(0, -footerHeight));

	auto menuList = BuildMenuList(featureSearch, categoryExpansionStates, drawGeneralSettings, drawAdvancedSettings);

	HandlePendingFeatureSelection(pendingFeatureSelection, menuList, selectedMenu);

	// Determine if left panel should be visible based on auto-hide settings
	bool leftPanelVisible = ShouldShowLeftPanel();

	// Create the table with appropriate number of columns based on visibility
	int numColumns = leftPanelVisible ? 2 : 1;
	if (ImGui::BeginTable("Menus Table", numColumns, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
		if (leftPanelVisible) {
			ImGui::TableSetupColumn("##ListOfMenus", 0, 2);
			ImGui::TableSetupColumn("##MenuConfig", 0, 8);
			RenderLeftColumn(menuList, selectedMenu, featureSearch, categoryExpansionStates);
			RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection);
		} else {
			// When left panel is hidden, right column takes full width
			ImGui::TableSetupColumn("##MenuConfig", 0, 1);
			RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection);
		}

		ImGui::EndTable();
	}

	ImGui::EndChild();
}

std::vector<FeatureListRenderer::MenuFuncInfo> FeatureListRenderer::BuildMenuList(
	const std::string& featureSearch,
	std::map<std::string, bool>& categoryExpansionStates,
	const std::function<void()>& drawGeneralSettings,
	const std::function<void()>& drawAdvancedSettings)
{
	// Build the menu list
	auto& featureList = Feature::GetFeatureList();
	auto sortedFeatureList{ featureList };  // need a copy so the load order is not lost
	std::ranges::sort(sortedFeatureList, [](Feature* a, Feature* b) {
		return a->GetDisplayName() < b->GetDisplayName();
	});

	// Filter features by search string
	if (!featureSearch.empty()) {
		auto it = std::remove_if(sortedFeatureList.begin(), sortedFeatureList.end(),
			[&featureSearch](Feature* feat) { return !Util::FeatureMatchesSearch(feat, featureSearch); });
		sortedFeatureList.erase(it, sortedFeatureList.end());
	}

	auto menuList = std::vector<MenuFuncInfo>{
		BuiltInMenu{ T("menu.features.home", "Home"), "Home", []() { HomePageRenderer::RenderHomePage(); } },
		BuiltInMenu{ T("menu.features.general", "General"), "General", drawGeneralSettings },
		BuiltInMenu{ T("menu.features.performance", "Performance"), "Performance", []() { PerformanceRenderer::Render(); } },
		BuiltInMenu{ T("menu.features.advanced", "Advanced"), "Advanced", drawAdvancedSettings },
		BuiltInMenu{ T("menu.features.profiling", "Profiling"), "Profiling", []() { ProfilingRenderer::RenderStatistics(); } }
	};  // NOTE: The menu list is rebuilt every frame, so category expansion states
	// persist correctly. This is acceptable since the list is small and built
	// infrequently, but could be optimized if performance becomes an issue.

	// Group features by category
	std::map<std::string, std::vector<Feature*>> categorizedFeatures;
	for (Feature* feat : sortedFeatureList) {
		if (feat->IsInMenu() && feat->loaded) {
			std::string category(feat->GetCategory());
			categorizedFeatures[category].push_back(feat);
		}
	}

	// Sort features within each category
	for (auto& [category, features] : categorizedFeatures) {
		std::ranges::sort(features, [](Feature* a, Feature* b) {
			return a->GetDisplayName() < b->GetDisplayName();
		});
	}

	// Define category order
	std::vector<std::string> categoryOrder = { "Display", "Utility", "Characters", "Foliage", "Lighting", "Materials", "Post-Processing", "Sky", "Landscape & Textures", "Water", "Other" };
	// Add categorized features to menu with collapsible headers
	for (const std::string& category : categoryOrder) {
		if (categorizedFeatures.find(category) != categorizedFeatures.end() && !categorizedFeatures[category].empty()) {
			// Initialize expansion state if not exists
			if (categoryExpansionStates.find(category) == categoryExpansionStates.end()) {
				categoryExpansionStates[category] = true;  // Default to expanded
			}

			// Add category header
			menuList.push_back(CategoryHeader{ category });

			// Add features only if category is expanded
			if (categoryExpansionStates[category]) {
				std::ranges::copy(categorizedFeatures[category], std::back_inserter(menuList));
			}
		}
	}

	// Add any categories not in the predefined order
	for (const auto& [category, features] : categorizedFeatures) {
		if (std::find(categoryOrder.begin(), categoryOrder.end(), category) == categoryOrder.end() && !features.empty()) {
			// Initialize expansion state if not exists
			if (categoryExpansionStates.find(category) == categoryExpansionStates.end()) {
				categoryExpansionStates[category] = true;  // Default to expanded
			}

			// Add category header
			menuList.push_back(CategoryHeader{ category });

			// Add features only if category is expanded
			if (categoryExpansionStates[category]) {
				std::ranges::copy(features, std::back_inserter(menuList));
			}
		}
	}

	auto unloadedFeatures = sortedFeatureList | std::ranges::views::filter([](Feature* feat) {
		return !feat->loaded && feat->IsInMenu() && !feat->IsHiddenUnreleased() && (!FeatureIssues::IsObsoleteFeature(feat->GetShortName()) || globals::state->IsDeveloperMode());
	});
	if (std::ranges::distance(unloadedFeatures) != 0) {
		menuList.push_back(T("menu.features.unloaded_features", "Unloaded Features"));
		std::ranges::copy(unloadedFeatures, std::back_inserter(menuList));
	}
	// Add top section for feature issues (rejected features, obsolete info, etc.)
	if (FeatureIssues::HasFeatureIssues()) {
		menuList.insert(menuList.begin(), BuiltInMenu{ T("menu.features.feature_issues", "Feature Issues"), "", []() {
														  FeatureIssues::DrawFeatureIssuesUI();
													  } });
	}

	return menuList;
}

void FeatureListRenderer::HandlePendingFeatureSelection(
	std::string& pendingFeatureSelection,
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu)
{
	if (!pendingFeatureSelection.empty()) {
		for (size_t i = 0; i < menuList.size(); ++i) {
			if (std::holds_alternative<Feature*>(menuList[i])) {
				Feature* feature = std::get<Feature*>(menuList[i]);
				if (feature->GetShortName() == pendingFeatureSelection) {
					if (feature == &globals::features::csEditor) {
						if (feature->loaded)
							CSEditor::OpenEditorWindow();
						break;
					}
					selectedMenu = i;
					logger::info("Navigated to {} feature menu", pendingFeatureSelection);
					break;
				}
			} else if (std::holds_alternative<BuiltInMenu>(menuList[i])) {
				// Built-in pages (e.g. "Performance", "Home") aren't Features and have no
				// GetShortName(); match on the same canonicalId used by IsCoreMenu() instead.
				const auto& builtIn = std::get<BuiltInMenu>(menuList[i]);
				if (!builtIn.canonicalId.empty() && builtIn.canonicalId == pendingFeatureSelection) {
					selectedMenu = i;
					logger::info("Navigated to {} built-in menu", pendingFeatureSelection);
					break;
				}
			}
		}
		pendingFeatureSelection.clear();  // Clear after processing
	}
}

void FeatureListRenderer::RenderLeftColumn(
	const std::vector<MenuFuncInfo>& menuList,
	size_t& selectedMenu,
	std::string& featureSearch,
	std::map<std::string, bool>& categoryExpansionStates)
{
	ImGui::TableNextColumn();
	// Draw the feature list
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4());
	if (ImGui::BeginListBox("##MenusList", { -FLT_MIN, -FLT_MIN })) {
		// Find where core built-in menus end (Home, General, Advanced, Display)
		size_t coreMenuCount = 0;
		for (size_t i = 0; i < menuList.size(); i++) {
			if (std::holds_alternative<BuiltInMenu>(menuList[i])) {
				const BuiltInMenu& menu = std::get<BuiltInMenu>(menuList[i]);
				if (IsCoreMenu(menu.canonicalId)) {
					coreMenuCount++;
				}
			}
		}

		// First render the core built-in menus (Home, General, Advanced, Display)
		size_t renderedCoreMenus = 0;
		for (size_t i = 0; i < menuList.size() && renderedCoreMenus < CORE_MENU_NAMES.size(); i++) {
			if (std::holds_alternative<BuiltInMenu>(menuList[i])) {
				const BuiltInMenu& menu = std::get<BuiltInMenu>(menuList[i]);
				if (IsCoreMenu(menu.canonicalId)) {
					std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates }, menuList[i]);
					renderedCoreMenus++;
				}
			}
		}

		// Add Features header and search bar after built-in settings
		Util::DrawSectionHeader(T("menu.features.features", "Features"), true);
		Util::DrawFeatureSearchBar(featureSearch);

		// Then render the rest (features and categories, but skip already rendered core menus)
		for (size_t i = 0; i < menuList.size(); i++) {
			if (std::holds_alternative<BuiltInMenu>(menuList[i])) {
				const BuiltInMenu& menu = std::get<BuiltInMenu>(menuList[i]);
				if (IsCoreMenu(menu.canonicalId)) {
					continue;  // Skip, already rendered
				}
			}
			std::visit(ListMenuVisitor{ i, selectedMenu, categoryExpansionStates }, menuList[i]);
		}

		ImGui::EndListBox();
	}
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
}

void FeatureListRenderer::RenderRightColumn(
	const std::vector<MenuFuncInfo>& menuList,
	size_t selectedMenu,
	std::string& pendingFeatureSelection)
{
	ImGui::TableNextColumn();

	if (selectedMenu < menuList.size()) {
		std::visit(DrawMenuVisitor{ pendingFeatureSelection }, menuList[selectedMenu]);
	} else {
		ImGui::TextDisabled("%s", T("menu.features.select_item_left", "Please select an item on the left."));
	}
}

void FeatureListRenderer::ListMenuVisitor::operator()(const BuiltInMenu& menu)
{
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);

	// Use error color for Feature Issues menu item
	bool isFeatureIssues = (menu.name == T("menu.features.feature_issues", "Feature Issues"));
	if (isFeatureIssues) {
		auto& themeSettings = globals::menu->GetSettings().Theme;
		ImGui::PushStyleColor(ImGuiCol_Text, themeSettings.StatusPalette.Error);

		if (ImGui::Selectable(fmt::format(" {} ", menu.name).c_str(), selectedMenuRef == listId, ImGuiSelectableFlags_SpanAllColumns))
			selectedMenuRef = listId;

		ImGui::PopStyleColor();
	} else {
		if (ImGui::Selectable(fmt::format(" {} ", menu.name).c_str(), selectedMenuRef == listId, ImGuiSelectableFlags_SpanAllColumns))
			selectedMenuRef = listId;
	}
}

void FeatureListRenderer::ListMenuVisitor::operator()(const std::string& label)
{
	// Style "Unloaded Features" to match category headers
	if (label == T("menu.features.unloaded_features", "Unloaded Features")) {
		Util::DrawSectionHeader(label.c_str(), true);
	} else {
		// Use default separator text for other labels - should be themed via ImGuiCol_Separator
		SeparatorTextWithFont(label, Menu::FontRole::Subheading);
	}
}

void FeatureListRenderer::ListMenuVisitor::operator()(const CategoryHeader& header)
{
	// Get expansion state from static map
	bool isExpanded = categoryExpansionStates[header.name];

	// Draw category header with custom styling using util:UI function
	// Use Heading font for category headers
	{
		MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Heading);
		int count = Menu::categoryCounts[std::string(header.name)];
		const auto categoryLabel = TranslateFeatureCategory(header.name);
		Util::DrawCategoryHeader(header.name.c_str(), categoryLabel.c_str(), isExpanded, count);
	}

	// Update expansion state
	categoryExpansionStates[header.name] = isExpanded;
}

void FeatureListRenderer::ListMenuVisitor::operator()(Feature* feat)
{
	if (feat == &globals::features::csEditor) {
		globals::features::csEditor.DrawLauncherButton();
		return;
	}

	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);

	const auto featureName = feat->GetShortName();
	bool isDisabled = globals::state->IsFeatureDisabled(featureName);
	bool isLoaded = feat->loaded;
	bool hasFailedMessage = !feat->failedLoadedMessage.empty();
	auto& themeSettings = globals::menu->GetSettings().Theme;

	ImVec4 textColor;

	// Determine the text color based on the state
	if (isDisabled) {
		textColor = themeSettings.StatusPalette.Disable;
	} else if (isLoaded) {
		// Loaded feature with staged but-not-yet-applied restart-gated
		// settings tints the same green as a feature pending re-enable.
		// Same semantic from the user's POV: "this feature has unmade
		// changes that take effect on restart."
		textColor = feat->HasAnyPendingRestart() ? themeSettings.StatusPalette.RestartNeeded : ImGui::GetStyleColorVec4(ImGuiCol_Text);
	} else if (hasFailedMessage) {
		textColor = feat->version.empty() ? themeSettings.StatusPalette.Disable : themeSettings.StatusPalette.Error;
	} else {
		// Installed but not loaded means the feature is only pending a restart (green),
		// otherwise it is simply missing (grey).
		textColor = feat->installed ? themeSettings.StatusPalette.RestartNeeded : themeSettings.StatusPalette.Disable;
	}

	// Create selectable item with semantic color
	ImGui::PushStyleColor(ImGuiCol_Text, textColor);
	if (ImGui::Selectable(fmt::format(" {} ", feat->GetDisplayName()).c_str(), selectedMenuRef == listId, ImGuiSelectableFlags_SpanAllColumns)) {
		selectedMenuRef = listId;
	}
	ImGui::PopStyleColor();

	// Display the stage marker behind the name, regardless of loaded state
	if (const auto stage = feat->GetReleaseStage(); stage != Feature::ReleaseStage::Release) {
		ImGui::SameLine();
		ImGui::TextColored(StageTagColor(stage), "%s", Feature::GetReleaseStageTag(stage).c_str());
	}

	// Display version if loaded
	if (isLoaded) {
		ImGui::SameLine();
		std::string formattedVersion = feat->version;
		std::replace(formattedVersion.begin(), formattedVersion.end(), '-', '.');
		ImGui::TextDisabled(fmt::format("({})", formattedVersion).c_str());
	}
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const BuiltInMenu& menu)
{
	ProfilingRenderer::DeactivateFeatureTimers();
	ImGui::PushID(menu.name.c_str());
	if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true)) {
		// Add spacing only for Home menu
		if (menu.name == T("menu.features.home", "Home")) {
			ImGui::Dummy(ImVec2(0, ThemeManager::Constants::BUTTON_SPACING));
		}
		menu.func();
	}
	ImGui::EndChild();
	ImGui::PopID();
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const std::string&)
{
	// std::unreachable() from c++23
	// you are not supposed to have selected a label!
}

void FeatureListRenderer::DrawMenuVisitor::operator()(const CategoryHeader&)
{
	// Category headers are not selectable in the right panel
	ImGui::TextDisabled("%s", T("menu.features.select_feature_left", "Please select a feature from the left."));
}

void FeatureListRenderer::DrawMenuVisitor::operator()(Feature* feat)
{
	if (feat == &globals::features::csEditor) {
		ProfilingRenderer::DeactivateFeatureTimers();
		return;
	}

	const auto featureName = feat->GetShortName();
	bool isDisabled = globals::state->IsFeatureDisabled(featureName);
	bool isLoaded = feat->loaded;
	bool hasFailedMessage = !feat->failedLoadedMessage.empty();
	const bool featureProfilingAvailable = !isDisabled && isLoaded && ProfilingRenderer::IsFeatureProfilingAvailable();

	ImGui::PushID(featureName.c_str());
	const float profilingHeight = ProfilingRenderer::PrepareFeatureTimers(featureName, featureProfilingAvailable);
	if (!featureProfilingAvailable) {
		g_featurePageLayouts.erase(featureName);
		if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true))
			RenderFeatureMaterial(feat, isDisabled, isLoaded, hasFailedMessage);
		ImGui::EndChild();
		ImGui::PopID();
		RenderReactiveConstraintWarningDialog();
		return;
	}

	auto& pageLayout = g_featurePageLayouts[featureName];
	const float pageViewportHeight = std::max(
		ImGui::GetContentRegionAvail().y - ImGui::GetStyle().WindowPadding.y * 2.0f,
		0.0f);
	const float predictedProfilingStart = std::max(pageLayout.materialHeight, pageViewportHeight - profilingHeight);
	const float predictedContentHeight = predictedProfilingStart + profilingHeight;
	const float contentHeightDelta = predictedContentHeight - pageLayout.contentHeight;
	const bool profilingWasAtBottom = pageLayout.scrollMaxY <= FEATURE_PAGE_BOTTOM_TOLERANCE ||
	                                  pageLayout.scrollY >= pageLayout.scrollMaxY - FEATURE_PAGE_BOTTOM_TOLERANCE;

	ImGui::SetNextWindowContentSize(ImVec2(0.0f, predictedContentHeight));
	if (pageLayout.measured && profilingWasAtBottom && std::abs(contentHeightDelta) >= FEATURE_PAGE_LAYOUT_EPSILON)
		ImGui::SetNextWindowScroll(ImVec2(-1.0f, std::max(pageLayout.scrollY + contentHeightDelta, 0.0f)));

	if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true)) {
		const float materialStartY = ImGui::GetCursorPosY();
		const float materialHeight = RenderFeatureMaterial(feat, isDisabled, isLoaded, hasFailedMessage);
		const float profilingStart = std::max(materialHeight, pageViewportHeight - profilingHeight);
		ImGui::SetCursorPosY(materialStartY + profilingStart);
		ProfilingRenderer::RenderFeatureTimers(featureName);

		pageLayout.measured = true;
		pageLayout.materialHeight = materialHeight;
		pageLayout.contentHeight = profilingStart + profilingHeight;
		pageLayout.scrollY = ImGui::GetScrollY();
		pageLayout.scrollMaxY = ImGui::GetScrollMaxY();
	}
	ImGui::EndChild();
	ImGui::PopID();
	// Render reactive constraint warning outside the child window so it can appear as a top-level popup
	RenderReactiveConstraintWarningDialog();
}

float FeatureListRenderer::DrawMenuVisitor::RenderFeatureMaterial(Feature* feat, bool isDisabled, bool isLoaded, bool hasFailedMessage)
{
	const float materialStartY = ImGui::GetCursorPosY();
	auto* sceneManager = globals::sceneSettingsManager;
	const auto featureName = feat->GetShortName();
	const bool sceneControlled = sceneManager->HasActiveSettingsForFeature(featureName) && !sceneManager->IsFeaturePaused(featureName);
	const auto featureActionsLayout = RenderFeatureHeader(feat, isLoaded);
	RenderFeatureSettings(feat, isDisabled, isLoaded, hasFailedMessage, sceneControlled);
	RenderFeatureActions(feat, isDisabled, isLoaded, sceneControlled, featureActionsLayout);
	return std::max(ImGui::GetCursorPosY() - materialStartY, 0.0f);
}

FeatureListRenderer::DrawMenuVisitor::FeatureActionsLayout FeatureListRenderer::DrawMenuVisitor::RenderFeatureHeader(Feature* feat, bool isLoaded)
{
	// Get available content width for positioning
	float availableWidth = ImGui::GetContentRegionAvail().x;

	// Save position before drawing title
	ImVec2 titleStartPos = ImGui::GetCursorScreenPos();

	// Get feature description for subtitle
	auto [description, keyFeatures] = feat->GetFeatureSummary();
	(void)keyFeatures;  // Not used for subtitle display

	// Draw feature title, version, and description on the left
	// Returns title-only height for button alignment
	const auto stage = feat->GetReleaseStage();
	const std::string stageTag = Feature::GetReleaseStageTag(stage);  // empty for Release; color unused when tag is empty
	const float actionsButtonSize = ImGui::GetFrameHeight() * FEATURE_ACTION_BUTTON_SCALE;
	const float titleOnlyHeight = DrawFeatureHeader(
		feat->GetDisplayName(), isLoaded ? feat->version : "", description, stageTag, StageTagColor(stage), actionsButtonSize);

	// Position the action button to the right of the header, middle-aligned with title only
	// Calculate Y position to middle-align the button with title text only (not description)
	const float buttonY = titleStartPos.y + (titleOnlyHeight - actionsButtonSize) * 0.5f;
	const FeatureActionsLayout actionsLayout{
		titleStartPos.x + availableWidth - actionsButtonSize,
		buttonY + ImGui::GetScrollY(),
		actionsButtonSize
	};

	return actionsLayout;
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureActions(
	Feature* feat,
	bool isDisabled,
	bool isLoaded,
	bool sceneControlled,
	const FeatureActionsLayout& layout)
{
	auto& themeSettings = globals::menu->GetSettings().Theme;
	const auto featureName = feat->GetShortName();
	auto overrideManager = SettingsOverrideManager::GetSingleton();
	bool hasOverrides = overrideManager && overrideManager->HasFeatureOverrides(featureName);

	const ImVec2 cursorPosAfterSettings = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(ImVec2(layout.x, layout.y));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	const bool overlayVisible = ImGui::BeginChild(
		"##FeatureActionsOverlay",
		ImVec2(layout.size, layout.size),
		false,
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	if (!overlayVisible) {
		ImGui::EndChild();
		ImGui::SetCursorScreenPos(cursorPosAfterSettings);
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
		return;
	}

	// Feature actions dropdown
	bool bootEnabled = !isDisabled;
	if (g_featureActionsFlyoutFeature != featureName) {
		Util::CloseFlyout(g_featureActionsFlyout);
		g_featureActionsFlyoutFeature = featureName;
		g_featureActionsIconProgress = 0.0f;
	}

	ImGui::PushID(featureName.c_str());
	const bool actionsButtonPressed = ImGui::Button("##FeatureActions", ImVec2(layout.size, layout.size));
	const ImGuiID actionsButtonId = ImGui::GetItemID();
	const ImVec2 actionsButtonMin = ImGui::GetItemRectMin();
	const ImVec2 actionsButtonMax = ImGui::GetItemRectMax();
	auto* actionsButtonDrawList = ImGui::GetWindowDrawList();
	{
		Util::FlyoutScope flyout(g_featureActionsFlyout, actionsButtonId, actionsButtonPressed);
		if (flyout) {
			bool closeFlyout = false;
			{
				const bool failedToLoad = !feat->failedLoadedMessage.empty();
				if (failedToLoad)
					ImGui::PushStyleColor(ImGuiCol_Text, themeSettings.StatusPalette.Error);
				const SKSE::stl::scope_exit restoreTextColor([failedToLoad]() noexcept {
					if (failedToLoad)
						ImGui::PopStyleColor();
				});

				if (Util::FlyoutMenuItem(
						T("menu.features.enable_at_boot", "Enable at Boot"),
						bootEnabled,
						true,
						FEATURE_ACTION_CHECKMARK_LEFT_OFFSET * Util::GetUIScale())) {
					const bool nowDisabled = feat->ToggleAtBootSetting();
					bootEnabled = !nowDisabled;
					logger::info("{}: {} at boot.", featureName, nowDisabled ? "Disabled" : "Enabled");
				}
			}

			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					T("menu.features.boot_toggle_tooltip",
						"Toggle feature loading at boot.\n"
						"Current state: %s\n"
						"Restart required for changes to take effect.\n"
						"Disabling removes performance impact."),
					bootEnabled ? T("menu.features.enabled", "Enabled") : T("menu.features.disabled", "Disabled"));
			}

			if (!isDisabled && isLoaded) {
				ImGui::Separator();
				if (Util::FlyoutMenuItem(
						feat->HasScopedDefaultSettings() ?
							T("menu.features.restore_page_defaults", "Restore Defaults (Page)") :
							T("menu.features.restore_defaults", "Restore Defaults"))) {
					feat->RestoreCurrentPageDefaultSettings();
					closeFlyout = true;
				}

				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text(
						"%s",
						feat->HasScopedDefaultSettings() ?
							T("menu.features.restore_page_defaults_tooltip", "Restore default settings for this page") :
							T("menu.features.restore_defaults_tooltip", "Restore default settings for this feature"));
				}

				if (hasOverrides) {
					if (Util::FlyoutMenuItem(
							feat->HasScopedOverrideSettings() ?
								T("menu.features.apply_page_override", "Apply Override (Page)") :
								T("menu.features.apply_override", "Apply Override"),
							false,
							!sceneControlled)) {
						closeFlyout = true;
						if (feat->ReapplyCurrentPageOverrideSettings()) {
							logger::info("Successfully reapplied override settings for {}", featureName);
						} else {
							logger::warn("Failed to reapply override settings for {}", featureName);
						}
					}

					if (auto _tt = Util::HoverTooltipWrapper()) {
						if (sceneControlled) {
							ImGui::Text(
								"%s",
								T("menu.features.cannot_apply_overrides_scene",
									"Cannot apply overrides while scene-specific settings are active.\n"
									"Pause scene settings for this feature first."));
						} else {
							ImGui::Text(
								"%s",
								feat->HasScopedOverrideSettings() ?
									T("menu.features.restore_page_override_tooltip",
										"Restores override settings for this page from mod files.\n"
										"This will discard your customizations on this page and revert to\n"
										"the mod author's recommended settings.") :
									T("menu.features.restore_override_tooltip",
										"Restores original override settings from mod files.\n"
										"This will discard your customizations and revert to\n"
										"the mod author's recommended settings."));
						}
					}
				}
			}

			if (closeFlyout)
				Util::RequestCloseFlyout(g_featureActionsFlyout);
		}
	}

	const bool actionsIconActive = g_featureActionsFlyout.activeId == actionsButtonId &&
	                               g_featureActionsFlyout.isOpen && !g_featureActionsFlyout.closing;
	const float iconTargetProgress = actionsIconActive ? 1.0f : 0.0f;
	const float iconDeltaTime = std::min(ImGui::GetIO().DeltaTime, FEATURE_ACTION_ICON_MAX_DELTA_TIME);
	const float iconDistance = iconTargetProgress - g_featureActionsIconProgress;
	const float iconSpeed = FEATURE_ACTION_ICON_RESPONSE_SPEED + FEATURE_ACTION_ICON_START_BOOST * iconDistance * iconDistance;
	const float iconResponse = 1.0f - std::exp(-iconSpeed * iconDeltaTime);
	const float iconStep = (std::abs(iconDistance) + FEATURE_ACTION_ICON_FINISH_BIAS) * iconResponse;
	g_featureActionsIconProgress += std::clamp(iconDistance, -iconStep, iconStep);
	DrawFeatureActionsIcon(actionsButtonDrawList, actionsButtonMin, actionsButtonMax, g_featureActionsIconProgress);
	ImGui::PopID();
	ImGui::EndChild();
	ImGui::SetCursorScreenPos(cursorPosAfterSettings);
	ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureSettings(Feature* feat, bool isDisabled, bool isLoaded, bool hasFailedMessage, bool sceneControlled)
{
	auto& themeSettings = globals::menu->GetSettings().Theme;

	if (isDisabled) {
		ImGui::TextColored(themeSettings.StatusPalette.Disable, "%s", T("menu.features.settings_hidden_disabled", "Feature settings are hidden because this feature is disabled at boot."));
		ImGui::Spacing();
		ImGui::Text("%s", T("menu.features.enable_to_access_config", "Enable the feature above to access its configuration options."));
	} else {
		if (isLoaded) {
			auto weatherRegistry = WeatherVariables::GlobalWeatherRegistry::GetSingleton();
			if (weatherRegistry->HasWeatherSupport(feat->GetShortName())) {
				bool paused = weatherRegistry->IsFeaturePaused(feat->GetShortName());
				if (ImGui::Checkbox(T("menu.features.pause_weather_overrides", "Pause Weather Overrides"), &paused)) {
					weatherRegistry->SetFeaturePaused(feat->GetShortName(), paused);
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text(
						"%s",
						T("menu.features.pause_weather_tooltip",
							"Temporarily disable weather-based setting adjustments for this feature.\n"
							"This state is not saved."));
				}
				ImGui::Separator();
			}

			// Scene-specific settings toggle (Interior Only / TimeOfDay / Weather-Specific)
			// Show toggle whenever scene entries exist for this feature, even if feature-paused
			{
				const auto& featureShortName = feat->GetShortName();
				auto* sceneMgr = globals::sceneSettingsManager;
				bool scenePaused = sceneMgr->IsFeaturePaused(featureShortName);
				if (sceneControlled || scenePaused) {
					bool active = !scenePaused;
					if (Util::FeatureToggle("##PauseSceneSettings", &active))
						sceneMgr->SetFeaturePaused(featureShortName, !active);
					ImGui::SameLine();
					ImGui::Text("%s", T("menu.features.scene_specific_settings", "Scene Specific Settings"));
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(scenePaused ? "menu.features.scene_paused_tooltip" : "menu.features.scene_active_tooltip",
											  scenePaused ? "Paused - click to resume" : "Active - click to pause"));
					}
					ImGui::Separator();
				}
			}

			// Disable feature settings while scene overrides are actively applied (not paused)
			if (sceneControlled)
				ImGui::BeginDisabled();

			ImVec2 cursorPosBefore = ImGui::GetCursorPos();
			feat->DrawSettings();

			ImVec2 cursorPosAfter = ImGui::GetCursorPos();

			if (sceneControlled)
				ImGui::EndDisabled();

			// --- Reactive constraint detection ---
			// Compare the current full constraint set against g_knownConstraintKeys.
			// On the very first frame we just seed the set (no popup); after that
			// any key that wasn't previously known triggers the warning.
			// This catches both same-frame changes (e.g. TerrainBlending toggle)
			// and next-frame changes (e.g. Upscaling, whose resolutionScale is
			// updated in the render loop, not in DrawSettings).
			if (!g_reactiveWarningShow) {  // don't overwrite a pending popup
				auto currentConstraints = FeatureConstraints::GetAllActiveConstraints();

				if (!g_knownConstraintKeysInitialised) {
					// First time: seed known set, no popup
					for (const auto& [settingId, result] : currentConstraints) {
						g_knownConstraintKeys.insert(settingId.featureShortName + "|" + settingId.settingPath);
					}
					g_knownConstraintKeysInitialised = true;
				} else {
					// Diff: find keys present now but not previously known
					std::vector<std::pair<FeatureConstraints::SettingId, FeatureConstraints::ConstraintResult>> newConstraints;
					std::unordered_set<std::string> currentKeys;
					for (const auto& [settingId, result] : currentConstraints) {
						std::string key = settingId.featureShortName + "|" + settingId.settingPath;
						currentKeys.insert(key);
						if (g_knownConstraintKeys.find(key) == g_knownConstraintKeys.end()) {
							newConstraints.emplace_back(settingId, result);
						}
					}
					// Update known set to current (removes keys for constraints that went away)
					g_knownConstraintKeys = std::move(currentKeys);

					if (!newConstraints.empty() && !globals::menu->GetSettings().SkipConstraintWarning) {
						logger::info("Reactive constraint detection: {} new constraints", newConstraints.size());
						for (const auto& [settingId, result] : newConstraints) {
							logger::info("  - {}.{} forced to {} by {}", settingId.featureShortName, settingId.settingPath, FeatureConstraints::FormatConstraintValue(result.forcedValue), result.sources.empty() ? "?" : result.sources[0].featureName);
						}
						g_reactiveWarningShow = true;
						g_reactiveWarningConstraints = std::move(newConstraints);
						g_dontShowAgainCheckbox = false;
					}
				}
			}

			const float cursorEpsilon = 0.1f;
			bool cursorMoved = (std::abs(cursorPosAfter.x - cursorPosBefore.x) > cursorEpsilon ||
								std::abs(cursorPosAfter.y - cursorPosBefore.y) > cursorEpsilon);
			if (!cursorMoved) {
				ImGui::TextColored(themeSettings.StatusPalette.Disable, "%s", T("menu.features.no_settings_available", "There are no settings available for this feature."));
			}
		} else {
			if (FeatureIssues::IsObsoleteFeature(feat->GetShortName())) {
				feat->DrawUnloadedUI();
			} else if (feat->installed) {
				ImGui::Text("%s", T("menu.features.available_after_restart", "This feature will be available after restart."));
			} else {
				feat->DrawUnloadedUI();
				if (!feat->GetFeatureModLink().empty()) {
					ImGui::Spacing();
					auto featureModLink = feat->GetFeatureModLink();
					const auto downloadText = std::vformat(
						T("menu.features.download_link", "Click here to download this feature ({})"), std::make_format_args(featureModLink));
					if (ImGui::Selectable(downloadText.c_str())) {
						ShellExecuteA(NULL, "open", featureModLink.c_str(), NULL, NULL, SW_SHOWNORMAL);
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T("menu.features.download_tooltip", "Download the feature from the mod page."));
					}
				}
			}
		}
	}

	if (hasFailedMessage && feat->DrawFailLoadMessage() && !FeatureIssues::IsObsoleteFeature(feat->GetShortName())) {
		ImGui::Spacing();
		SeparatorTextWithFont(T("menu.features.error_header", "Error"), Menu::FontRole::Subheading);
		ImGui::TextColored(themeSettings.StatusPalette.Error, feat->failedLoadedMessage.c_str());
	}
}

void FeatureListRenderer::DrawMenuVisitor::RenderReactiveConstraintWarningDialog()
{
	if (!g_reactiveWarningShow) {
		return;
	}

	constexpr const char* popupId = "###SettingChangeWarning";
	const std::string popupTitle = fmt::format("{}{}", T("menu.features.setting_change_warning_title", "Setting Change Warning"), popupId);

	// OpenPopup is idempotent while the popup is already open, so calling it
	// every frame while the flag is set is safe and ensures we don't miss the
	// one-frame window where ImGui expects it.
	ImGui::OpenPopup(popupId);

	// Center the popup (ImGuiCond_Always matches the Clear Cache dialog pattern)
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

	if (Util::BeginPopupModalWithRoundedClose(popupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", T("menu.features.settings_adjusted_warning", "Some of your settings have been automatically adjusted due to feature incompatibilities."));
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// Table columns: Impacted Feature | Setting | Constrained By | Forced To
		if (ImGui::BeginTable("##ReactiveConstraintTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn(T("menu.features.col_impacted_feature", "Impacted Feature"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_setting", "Setting"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_constrained_by", "Constrained By"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("menu.features.col_forced_to", "Forced To"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			size_t rowIndex = 0;
			for (const auto& [settingId, result] : g_reactiveWarningConstraints) {
				ImGui::TableNextRow();

				// --- Column 0: Impacted Feature (clickable -> navigate to that feature) ---
				ImGui::TableSetColumnIndex(0);
				{
					// Look up the display name of the target feature from its short name
					std::string targetDisplayName = settingId.featureShortName;
					for (auto* f : Feature::GetFeatureList()) {
						if (f->GetShortName() == settingId.featureShortName) {
							targetDisplayName = f->GetDisplayName();
							break;
						}
					}
					if (ImGui::Selectable(fmt::format("{}##imp{}", targetDisplayName, rowIndex).c_str())) {
						pendingFeatureSelection = settingId.featureShortName;
						ImGui::CloseCurrentPopup();
						g_reactiveWarningShow = false;
						g_reactiveWarningConstraints.clear();
						return;
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text(T("menu.features.click_to_navigate", "Click to navigate to %s"), targetDisplayName.c_str());
					}
				}

				// --- Column 1: Setting name ---
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", settingId.settingPath.c_str());

				// --- Column 2: Constrained By (source features, clickable) ---
				ImGui::TableSetColumnIndex(2);
				if (!result.sources.empty()) {
					if (ImGui::Selectable(fmt::format("{}##src{}", result.sources[0].featureName, rowIndex).c_str())) {
						pendingFeatureSelection = result.sources[0].featureShortName;
						ImGui::CloseCurrentPopup();
						g_reactiveWarningShow = false;
						g_reactiveWarningConstraints.clear();
						return;
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text(T("menu.features.click_to_navigate", "Click to navigate to %s"), result.sources[0].featureName.c_str());
						if (result.sources.size() > 1) {
							ImGui::Separator();
							for (size_t i = 1; i < result.sources.size(); ++i) {
								ImGui::Text(T("menu.features.also_feature", "Also: %s"), result.sources[i].featureName.c_str());
							}
						}
						ImGui::Separator();
						ImGui::Text("%s", result.sources[0].reason.c_str());
					}
				}

				// --- Column 3: Forced value ---
				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%s", FeatureConstraints::FormatConstraintValue(result.forcedValue).c_str());

				rowIndex++;
			}

			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextWrapped(
			"%s",
			T("menu.features.constraints_explanation",
				"These settings are disabled in their respective feature menus while the constraints are active. "
				"Adjust the constraining features to remove them."));

		ImGui::Spacing();

		// "Don't show again" checkbox -- same pattern as Clear Cache dialog
		ImGui::Checkbox(T("menu.features.dont_show_warning", "Don't show this warning again"), &g_dontShowAgainCheckbox);

		ImGui::Spacing();

		// Centered OK button
		constexpr float buttonWidth = ThemeManager::Constants::POPUP_BUTTON_WIDTH;
		const float windowWidth = ImGui::GetWindowWidth();
		const float offset = (windowWidth - buttonWidth) * 0.5f;
		if (offset > 0)
			ImGui::SetCursorPosX(offset);

		if (ImGui::Button(T("menu.features.ok_button", "OK"), ImVec2(buttonWidth, 0))) {
			if (g_dontShowAgainCheckbox) {
				if (auto* menu = globals::menu) {
					menu->GetSettings().SkipConstraintWarning = true;
				}
			}
			g_reactiveWarningShow = false;
			g_reactiveWarningConstraints.clear();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else {
		// Popup was closed externally (e.g. clicked outside), reset state
		g_reactiveWarningShow = false;
		g_reactiveWarningConstraints.clear();
	}
}
