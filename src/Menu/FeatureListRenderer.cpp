#include "FeatureListRenderer.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>
#include <numbers>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include "Feature.h"
#include "FeatureConstraints.h"
#include "FeatureIssues.h"
#include "Features/CSEditor.h"
#include "Features/CSUtility.h"
#include "Features/FeatureOverwrites.h"
#include "Features/SceneManagerUI.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/HomePageRenderer.h"
#include "Menu/PerformanceRenderer.h"
#include "Menu/ProfilingRenderer.h"
#include "Menu/ThemeManager.h"
#include "SceneSettingsManager.h"
#include "SceneSettingsUIHooks.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Util.h"
#include "Utils/UI.h"

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
			const float capStartAngle = lineAngle + std::numbers::pi_v<float> * 0.5f;
			drawList->PathArcTo(lineStart, capRadius,
				capStartAngle, capStartAngle + std::numbers::pi_v<float>);
			drawList->PathArcTo(lineEnd, capRadius,
				capStartAngle + std::numbers::pi_v<float>, capStartAngle + std::numbers::pi_v<float> * 2.0f);
			drawList->PathFillConvex(lineColor);
		}
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
	bool g_featurePreferenceSaveFailed = false;

	std::string GetMenuId(const FeatureListRenderer::MenuFuncInfo& item)
	{
		if (const auto* feature = std::get_if<Feature*>(&item))
			return (*feature)->GetShortName();
		if (const auto* menu = std::get_if<FeatureListRenderer::BuiltInMenu>(&item))
			return menu->canonicalId;
		return {};
	}
}

void FeatureListRenderer::RenderFeatureList(
	float footerHeight,
	Menu::SidebarState& sidebar,
	size_t& selectedMenu,
	std::string& featureSearch,
	std::string& pendingFeatureSelection,
	const std::function<void()>& drawGeneralSettings,
	const std::function<void()>& drawAdvancedSettings)
{
	if (!ImGui::BeginChild("Menus Table", ImVec2(0, -footerHeight))) {
		ImGui::EndChild();
		return;
	}

	auto menuList = BuildMenuList(drawGeneralSettings, drawAdvancedSettings);
	static std::string selectedMenuId = "Home";
	selectedMenu = 0;
	for (size_t i = 0; i < menuList.size(); ++i) {
		if (GetMenuId(menuList[i]) == selectedMenuId) {
			selectedMenu = i;
			break;
		}
	}

	HandlePendingFeatureSelection(pendingFeatureSelection, menuList, selectedMenu);

	const bool leftPanelVisible = ShouldShowLeftPanel() && sidebar.visible;
	const float step = ImGui::GetIO().DeltaTime / ThemeManager::Constants::SIDEBAR_SLIDE_DURATION;
	sidebar.progress = std::clamp(sidebar.progress + (leftPanelVisible ? step : -step), 0.0f, 1.0f);
	const float easedProgress = sidebar.progress * sidebar.progress * (3.0f - 2.0f * sidebar.progress);
	const ImVec2 available = ImGui::GetContentRegionAvail();
	const bool windowResized = sidebar.availableWidth > 0.0f && sidebar.availableWidth != available.x;
	const float contentWidth = windowResized ? sidebar.widthRatio * available.x : sidebar.contentWidth;
	const float slideWidth = sidebar.width + contentWidth - sidebar.contentWidth;
	const float slideOffset = std::floor(slideWidth * (1.0f - easedProgress));
	if (auto* savedLayout = ImGui::TableSettingsFindByID(ImGui::GetID("Menus Table"));
		savedLayout && savedLayout->ColumnsCount == 2 && savedLayout->GetColumnSettings()[0].IsStretch) {
		auto* columns = savedLayout->GetColumnSettings();
		const float totalWeight = columns[0].WidthOrWeight + columns[1].WidthOrWeight;
		const float widthRatio = totalWeight > 0.0f ? columns[0].WidthOrWeight / totalWeight : ThemeManager::Constants::AUTOHIDE_PANEL_WIDTH_RATIO;
		// Saved stretch weights must become pixel widths before restoring the fixed sidebar column.
		columns[0].WidthOrWeight = available.x * widthRatio;
		columns[0].IsStretch = false;
		savedLayout->RefScale = ImGui::GetFontSize();
	}
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 tableOrigin(origin.x - slideOffset, origin.y);
	ImGui::SetCursorScreenPos(tableOrigin);
	if (ImGui::BeginTable("Menus Table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable,
			ImVec2(available.x + slideOffset, 0.0f))) {
		ImGui::TableSetupColumn("##ListOfMenus", ImGuiTableColumnFlags_WidthFixed,
			available.x * ThemeManager::Constants::AUTOHIDE_PANEL_WIDTH_RATIO);
		ImGui::TableSetupColumn("##MenuConfig", ImGuiTableColumnFlags_WidthStretch);
		if (windowResized)
			ImGui::TableSetColumnWidth(0, contentWidth);
		if (sidebar.progress > 0.0f) {
			RenderLeftColumn(menuList, selectedMenu, featureSearch);
		} else {
			ImGui::TableNextColumn();
		}
		ImGui::TableNextColumn();
		const float columnWidth = ImGui::GetCurrentTable()->Columns[0].WidthRequest;
		if (!windowResized && columnWidth != sidebar.contentWidth)
			sidebar.widthRatio = columnWidth / available.x;
		sidebar.contentWidth = columnWidth;
		sidebar.availableWidth = available.x;
		sidebar.width = ImGui::GetCursorScreenPos().x - tableOrigin.x;
		RenderRightColumn(menuList, selectedMenu, pendingFeatureSelection);

		ImGui::EndTable();
	}

	if (selectedMenu < menuList.size())
		selectedMenuId = GetMenuId(menuList[selectedMenu]);

	ImGui::EndChild();
}

std::vector<FeatureListRenderer::MenuFuncInfo> FeatureListRenderer::BuildMenuList(
	const std::function<void()>& drawGeneralSettings,
	const std::function<void()>& drawAdvancedSettings)
{
	// Build the menu list
	auto& featureList = Feature::GetFeatureList();
	auto sortedFeatureList{ featureList };  // need a copy so the load order is not lost
	std::ranges::sort(sortedFeatureList, [](Feature* a, Feature* b) {
		return a->GetDisplayName() < b->GetDisplayName();
	});

	auto menuList = std::vector<MenuFuncInfo>{
		BuiltInMenu{ T("menu.features.home", "Home"), "Home", []() { HomePageRenderer::RenderHomePage(); } },
		BuiltInMenu{ T("menu.features.general", "General"), "General", drawGeneralSettings },
		BuiltInMenu{ T("menu.features.performance", "Performance"), "Performance", []() { PerformanceRenderer::Render(); } },
		BuiltInMenu{ T("menu.features.advanced", "Advanced"), "Advanced", drawAdvancedSettings }
	};

	const auto isFavorite = [](Feature* feature) {
		return feature != &globals::features::csEditor && globals::state->IsFeatureFavorite(feature->GetShortName());
	};

	menuList.push_back(CategoryHeader{ "Utility" });
	if (globals::features::csEditor.IsInMenu() && globals::features::csEditor.loaded)
		menuList.push_back(&globals::features::csEditor);
	for (Feature* feat : sortedFeatureList) {
		if (feat->IsInMenu() && feat->loaded && feat->GetCategory() == FeatureCategories::kUtility &&
			feat != &globals::features::csEditor && feat != &globals::features::featureOverwrites && !isFavorite(feat))
			menuList.push_back(feat);
	}
	if (globals::features::featureOverwrites.loaded && !isFavorite(&globals::features::featureOverwrites)) {
		const auto utility = std::ranges::find_if(menuList, [](const auto& item) {
			const auto* feature = std::get_if<Feature*>(&item);
			return feature && *feature == &globals::features::csUtility;
		});
		menuList.insert(utility, &globals::features::featureOverwrites);
	}

	auto favorites = sortedFeatureList | std::ranges::views::filter([&isFavorite](Feature* feat) {
		return feat->IsInMenu() && feat->loaded && isFavorite(feat);
	});
	const auto favoriteCount = std::ranges::distance(favorites);
	if (favoriteCount != 0) {
		menuList.push_back(CategoryHeader{ "Favorites", static_cast<int>(favoriteCount) });
		std::ranges::copy(favorites, std::back_inserter(menuList));
	}

	const auto featureCount = std::ranges::count_if(sortedFeatureList, [&isFavorite](Feature* feat) {
		return feat->IsInMenu() && feat->loaded && feat->GetCategory() != FeatureCategories::kUtility && !isFavorite(feat);
	});
	menuList.push_back(CategoryHeader{ "Features", static_cast<int>(featureCount) });
	for (Feature* feat : sortedFeatureList) {
		if (feat->IsInMenu() && feat->loaded && feat->GetCategory() != FeatureCategories::kUtility && !isFavorite(feat))
			menuList.push_back(feat);
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
		menuList.insert(menuList.begin(), BuiltInMenu{ T("menu.features.feature_issues", "Feature Issues"), "FeatureIssues", []() {
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
	std::string& featureSearch)
{
	ImGui::TableNextColumn();
	// Draw the feature list
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4());
	if (ImGui::BeginListBox("##MenusList", { -FLT_MIN, -FLT_MIN })) {
		bool filterFeatures = false;
		for (size_t i = 0; i < menuList.size(); ++i) {
			if (const auto* feature = std::get_if<Feature*>(&menuList[i]); feature && filterFeatures && !Util::FeatureMatchesSearch(*feature, featureSearch))
				continue;
			if (std::holds_alternative<std::string>(menuList[i]) && !featureSearch.empty() &&
				std::ranges::none_of(menuList, [&featureSearch](const auto& item) {
					const auto* feature = std::get_if<Feature*>(&item);
					return feature && !(*feature)->loaded && Util::FeatureMatchesSearch(*feature, featureSearch);
				}))
				continue;
			std::visit(ListMenuVisitor{ i, selectedMenu }, menuList[i]);
			if (const auto* header = std::get_if<CategoryHeader>(&menuList[i]); header && header->name == "Features") {
				filterFeatures = true;
				Util::DrawFeatureSearchBar(featureSearch);
				ImGui::Spacing();
			}
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
	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Heading);
	if (header.name == "Utility") {
		Util::DrawSectionHeader(T("feature.category.utility", "Utilities"), true, false);
	} else {
		const bool favorites = header.name == "Favorites";
		const auto label = std::format("{} ({})", favorites ? T("menu.features.favorites", "Favorites") : T("menu.features.features", "Features"), header.count);
		Util::DrawSectionHeader(label.c_str(), true, false, nullptr, favorites ? Util::DrawStarIcon : nullptr);
	}
}

void FeatureListRenderer::ListMenuVisitor::operator()(Feature* feat)
{
	if (feat == &globals::features::csEditor) {
		globals::features::csEditor.DrawLauncherButton();
		return;
	}

	MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);

	const auto featureName = feat->GetShortName();
	bool isDisabled = !feat->IsAlwaysEnabled() && globals::state->IsFeatureDisabled(featureName);
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

	auto* icon = feat->GetCategory() != FeatureCategories::kUtility ? Util::GetCategoryIcon(feat->GetCategory()) : nullptr;
	const ImVec2 iconMin = ImGui::GetCursorScreenPos();
	const float iconSize = ImGui::GetTextLineHeight();
	if (icon) {
		ImGui::Dummy(ImVec2(iconSize, iconSize));
		Util::AddTooltip(feat->GetDisplayCategory().c_str());
		ImGui::SameLine();
	}

	// Create selectable item with semantic color
	ImGui::PushStyleColor(ImGuiCol_Text, textColor);
	if (ImGui::Selectable(fmt::format(" {} ", feat->GetDisplayName()).c_str(), selectedMenuRef == listId, ImGuiSelectableFlags_SpanAllColumns)) {
		selectedMenuRef = listId;
	}
	ImGui::PopStyleColor();

	if (icon)
		ImGui::GetWindowDrawList()->AddImage(icon, iconMin, ImVec2(iconMin.x + iconSize, iconMin.y + iconSize),
			ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(textColor));

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
	bool isDisabled = !feat->IsAlwaysEnabled() && globals::state->IsFeatureDisabled(featureName);
	bool isLoaded = feat->loaded;
	bool hasFailedMessage = !feat->failedLoadedMessage.empty();
	const bool featureProfilingAvailable =
		!isDisabled && isLoaded && ProfilingRenderer::IsFeatureProfilingAvailable(featureName);

	ImGui::PushID(featureName.c_str());
	const float profilingHeight = ProfilingRenderer::PrepareFeatureTimers(featureName, featureProfilingAvailable);
	if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		auto* sceneManager = globals::sceneSettingsManager;
		const bool sceneControlled = sceneManager->HasActiveSettingsForFeature(featureName) &&
		                             !sceneManager->IsFeaturePaused(featureName);
		const bool canEditSceneSettings = SceneManagerUI::CanEditFeaturePage(feat);
		const auto featureActionsLayout = RenderFeatureHeader(
			feat, isDisabled, isLoaded, canEditSceneSettings);
		RenderFeatureActions(feat, isDisabled, isLoaded, sceneControlled,
			canEditSceneSettings, featureActionsLayout);
		SceneManagerUI::DrawFeaturePageControls(feat, !isDisabled && isLoaded);
		const bool sceneEditing = sceneManager->IsFeatureSceneEditing(featureName);
		const float pageViewportHeight = std::max(ImGui::GetContentRegionAvail().y, 0.0f);

		if (!featureProfilingAvailable) {
			g_featurePageLayouts.erase(featureName);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			const bool settingsVisible = ImGui::BeginChild("##FeatureSettingsScroll", { 0, 0 }, false);
			ImGui::PopStyleVar();
			if (settingsVisible)
				RenderFeatureMaterial(feat, isDisabled, isLoaded, hasFailedMessage,
					sceneControlled, sceneEditing);
			ImGui::EndChild();
		} else {
			auto& pageLayout = g_featurePageLayouts[featureName];
			const float predictedProfilingStart = std::max(
				pageLayout.materialHeight, pageViewportHeight - profilingHeight);
			const float predictedContentHeight = predictedProfilingStart + profilingHeight;
			const float contentHeightDelta = predictedContentHeight - pageLayout.contentHeight;
			const bool profilingWasAtBottom =
				pageLayout.scrollMaxY <= FEATURE_PAGE_BOTTOM_TOLERANCE ||
				pageLayout.scrollY >= pageLayout.scrollMaxY - FEATURE_PAGE_BOTTOM_TOLERANCE;

			ImGui::SetNextWindowContentSize(ImVec2(0.0f, predictedContentHeight));
			if (pageLayout.measured && profilingWasAtBottom &&
				std::abs(contentHeightDelta) >= FEATURE_PAGE_LAYOUT_EPSILON)
				ImGui::SetNextWindowScroll(
					ImVec2(-1.0f, std::max(pageLayout.scrollY + contentHeightDelta, 0.0f)));

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			const bool settingsVisible = ImGui::BeginChild("##FeatureSettingsScroll", { 0, 0 }, false);
			ImGui::PopStyleVar();
			if (settingsVisible) {
				const float materialStartY = ImGui::GetCursorPosY();
				const float materialHeight = RenderFeatureMaterial(feat, isDisabled, isLoaded,
					hasFailedMessage, sceneControlled, sceneEditing);
				const float profilingStart = std::max(
					materialHeight, pageViewportHeight - profilingHeight);
				ImGui::SetCursorPosY(materialStartY + profilingStart);
				ProfilingRenderer::RenderFeatureTimers(featureName);

				pageLayout.measured = true;
				pageLayout.materialHeight = materialHeight;
				pageLayout.contentHeight = profilingStart + profilingHeight;
				pageLayout.scrollY = ImGui::GetScrollY();
				pageLayout.scrollMaxY = ImGui::GetScrollMaxY();
			}
			ImGui::EndChild();
		}
	}
	ImGui::EndChild();
	ImGui::PopID();
	// Render reactive constraint warning outside the child window so it can appear as a top-level popup
	RenderReactiveConstraintWarningDialog();
}

float FeatureListRenderer::DrawMenuVisitor::RenderFeatureMaterial(Feature* feat,
	bool isDisabled, bool isLoaded, bool hasFailedMessage, bool sceneControlled,
	bool sceneEditing)
{
	const float materialStartY = ImGui::GetCursorPosY();
	RenderFeatureSettings(
		feat, isDisabled, isLoaded, hasFailedMessage, sceneControlled, sceneEditing);
	return std::max(ImGui::GetCursorPosY() - materialStartY, 0.0f);
}

FeatureListRenderer::DrawMenuVisitor::FeatureActionsLayout FeatureListRenderer::DrawMenuVisitor::RenderFeatureHeader(
	Feature* feat, bool isDisabled, bool isLoaded, bool canEditSceneSettings)
{
	const auto featureName = feat->GetShortName();
	auto* overrideManager = SettingsOverrideManager::GetSingleton();
	const bool hasOverrides = feat->UsesMainSettings() && overrideManager && overrideManager->HasFeatureOverrides(featureName);
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
	const bool canRestoreDefaults = !isDisabled && isLoaded && feat->HasRestoreDefaults();
	const bool canApplyOverrides = !isDisabled && isLoaded && hasOverrides;
	const bool hasFeatureActions = !feat->IsAlwaysEnabled() || canRestoreDefaults ||
	                               canApplyOverrides || canEditSceneSettings;
	const float actionsButtonSize = hasFeatureActions ? ImGui::GetFrameHeight() * FEATURE_ACTION_BUTTON_SCALE : 0.0f;
	const float titleOnlyHeight = DrawFeatureHeader(
		feat->GetDisplayName(), isLoaded ? feat->version : "", description, stageTag, StageTagColor(stage), actionsButtonSize);

	// Position the action button to the right of the header, middle-aligned with title only
	const float buttonY = titleStartPos.y + (titleOnlyHeight - actionsButtonSize) * 0.5f;
	const FeatureActionsLayout actionsLayout{
		titleStartPos.x + availableWidth - actionsButtonSize,
		buttonY,
		actionsButtonSize
	};

	return actionsLayout;
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureActions(
	Feature* feat,
	bool isDisabled,
	bool isLoaded,
	bool sceneControlled,
	bool canEditSceneSettings,
	const FeatureActionsLayout& layout)
{
	auto& themeSettings = globals::menu->GetSettings().Theme;
	const auto featureName = feat->GetShortName();
	auto* overrideManager = SettingsOverrideManager::GetSingleton();
	const bool hasOverrides = feat->UsesMainSettings() && overrideManager && overrideManager->HasFeatureOverrides(featureName);
	const bool canRestoreDefaults = !isDisabled && isLoaded && feat->HasRestoreDefaults();
	const bool canApplyOverrides = !isDisabled && isLoaded && hasOverrides;
	const bool sceneEditing = SceneManagerUI::IsFeaturePageEditing(feat);
	if (layout.size <= 0.0f) {
		if (g_featureActionsFlyoutFeature == featureName) {
			Util::CloseFlyout(g_featureActionsFlyout);
			g_featureActionsFlyoutFeature.clear();
		}
		return;
	}

	const ImVec2 cursorPosAfterSettings = ImGui::GetCursorScreenPos();
	const SKSE::stl::scope_exit restoreCursor([cursorPosAfterSettings]() noexcept {
		ImGui::SetCursorScreenPos(cursorPosAfterSettings);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
		const SKSE::stl::scope_exit restoreSpacing([]() noexcept { ImGui::PopStyleVar(); });
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
	});
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
		return;
	}

	bool bootEnabled = !isDisabled;
	if (g_featureActionsFlyoutFeature != featureName) {
		Util::CloseFlyout(g_featureActionsFlyout);
		g_featureActionsFlyoutFeature = featureName;
		g_featurePreferenceSaveFailed = false;
		g_featureActionsIconProgress = 0.0f;
	}

	ImGui::PushID(featureName.c_str());
	const bool actionsButtonPressed = ImGui::Button("##FeatureActions", ImVec2(layout.size, layout.size));
	const ImGuiID actionsButtonId = ImGui::GetItemID();
	const ImVec2 actionsButtonMin = ImGui::GetItemRectMin();
	const ImVec2 actionsButtonMax = ImGui::GetItemRectMax();
	auto* actionsButtonDrawList = ImGui::GetWindowDrawList();
	{
		const auto& style = ImGui::GetStyle();
		const float highlightGap = std::max(0.0f, style.WindowPadding.x - style.ItemSpacing.x * 0.5f);
		const ImVec2 flyoutPadding(style.WindowPadding.x, highlightGap + style.ItemSpacing.y * 0.5f);
		Util::FlyoutScope flyout(
			g_featureActionsFlyout, actionsButtonId, actionsButtonPressed,
			{ .windowPadding = flyoutPadding,
				.windowRounding = style.WindowRounding,
				.windowBackgroundAlpha = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).w,
				.contentAlpha = style.Alpha,
				.blurBackground = true });
		if (flyout) {
			bool closeFlyout = false;
			if (!feat->IsAlwaysEnabled()) {
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
					g_featurePreferenceSaveFailed = nowDisabled == isDisabled;
					bootEnabled = !nowDisabled;
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
			}

			const bool favorite = globals::state->IsFeatureFavorite(featureName);
			if (Util::FlyoutMenuItem(T("menu.features.add_to_favorites", "Add to Favorites"), favorite, isLoaded,
					FEATURE_ACTION_CHECKMARK_LEFT_OFFSET * Util::GetUIScale(), Util::DrawStarIcon))
				g_featurePreferenceSaveFailed = !globals::state->SetFeatureFavorite(featureName, !favorite);

			if (canEditSceneSettings) {
				if (Util::FlyoutMenuItem(
						T("feature.scene_manager.name", "Scene Manager"),
						sceneEditing,
						!isDisabled && isLoaded,
						FEATURE_ACTION_CHECKMARK_LEFT_OFFSET * Util::GetUIScale())) {
					if (sceneEditing)
						SceneManagerUI::HideFeaturePageEditing();
					else
						SceneManagerUI::BeginFeaturePageEditing(feat);
					closeFlyout = true;
				}
			}
			if (g_featurePreferenceSaveFailed)
				Util::Text::WrappedError("%s", T("menu.features.preference_save_failed", "Could not save this preference. Please try again."));

			if (canRestoreDefaults || canApplyOverrides) {
				ImGui::Separator();
				if (canRestoreDefaults) {
					ImGui::BeginDisabled(sceneEditing);
					if (Util::FlyoutMenuItem(
							feat->HasScopedDefaultSettings() ?
								T("menu.features.restore_page_defaults", "Restore Defaults (Page)") :
								T("menu.features.restore_defaults", "Restore Defaults"))) {
						SceneSettingsManager::SceneLayerGuard sceneLayerGuard(*SceneSettingsManager::GetSingleton());
						feat->RestoreCurrentPageDefaultSettings();
						closeFlyout = true;
					}
					ImGui::EndDisabled();

					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text(
							"%s",
							feat->HasScopedDefaultSettings() ?
								T("menu.features.restore_page_defaults_tooltip", "Restore default settings for this page") :
								T("menu.features.restore_defaults_tooltip", "Restore default settings for this feature"));
					}
				}

				if (canApplyOverrides) {
					if (Util::FlyoutMenuItem(
							feat->HasScopedOverrideSettings() ?
								T("menu.features.apply_page_override", "Apply Override (Page)") :
								T("menu.features.apply_override", "Apply Override"),
							std::nullopt,
							!sceneControlled && !sceneEditing)) {
						closeFlyout = true;
						SceneSettingsManager::SceneLayerGuard sceneLayerGuard(*SceneSettingsManager::GetSingleton());
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
}

void FeatureListRenderer::DrawMenuVisitor::RenderFeatureSettings(Feature* feat,
	bool isDisabled, bool isLoaded, bool hasFailedMessage, bool sceneControlled,
	bool sceneEditing)
{
	auto& themeSettings = globals::menu->GetSettings().Theme;

	if (isDisabled) {
		ImGui::TextColored(themeSettings.StatusPalette.Disable, "%s", T("menu.features.settings_hidden_disabled", "Feature settings are hidden because this feature is disabled at boot."));
		ImGui::Spacing();
		ImGui::Text("%s", T("menu.features.enable_to_access_config", "Enable the feature above to access its configuration options."));
	} else {
		if (isLoaded) {
			if (!sceneEditing) {
				const auto& featureShortName = feat->GetShortName();
				auto* sceneMgr = globals::sceneSettingsManager;
				bool scenePaused = sceneMgr->IsFeaturePaused(featureShortName);
				if (sceneMgr->HasCurrentSceneSettingsForFeature(featureShortName)) {
					const auto rowStart = ImGui::GetCursorScreenPos();
					const float rowHeight = ImGui::GetFrameHeight();
					const ImVec2 toggleSize(rowHeight * 1.6f, rowHeight * 0.8f);
					ImGui::SetCursorScreenPos(ImVec2(rowStart.x,
						rowStart.y + (rowHeight - toggleSize.y) * 0.5f));
					bool active = !scenePaused;
					if (Util::FeatureToggle("##PauseSceneSettings", &active, toggleSize)) {
						sceneMgr->SetFeaturePaused(featureShortName, !active);
						scenePaused = !active;
						sceneControlled = sceneMgr->HasActiveSettingsForFeature(featureShortName) && !scenePaused;
					}
					const auto toggleMaximum = ImGui::GetItemRectMax();
					const ImVec2 labelPosition(toggleMaximum.x + ImGui::GetStyle().ItemSpacing.x,
						rowStart.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
					ImGui::GetWindowDrawList()->AddText(labelPosition, ImGui::GetColorU32(ImGuiCol_Text),
						T("menu.features.scene_specific_settings", "Scene Specific Settings"));
					if (auto _tt = Util::HoverTooltipWrapper()) {
						const auto* tooltip = scenePaused ?
						                          T("menu.features.scene_paused_tooltip", "Paused - click to resume") :
						                          T("menu.features.scene_active_tooltip", "Active - click to pause");
						ImGui::Text("%s", tooltip);
					}
					ImGui::SetCursorScreenPos(ImVec2(rowStart.x,
						rowStart.y + rowHeight + ImGui::GetStyle().ItemSpacing.y));
					ImGui::Separator();
				}
			}

			ImVec2 cursorPosBefore = ImGui::GetCursorPos();
			{
				SceneSettingsUIHooks::FeatureDrawGuard featureDrawGuard(
					feat, sceneControlled, sceneEditing);
				feat->DrawSettings();
			}

			ImVec2 cursorPosAfter = ImGui::GetCursorPos();

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
