#include "PerformanceRenderer.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <imgui.h>
#include <vector>

#include "Feature.h"
#include "Features/PerformanceOverlay.h"
#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "ProfilingRenderer.h"
#include "SceneSettingsManager.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "menu.performance."

namespace
{
	constexpr float kProfileCardMinimumWidth = 240.0f;
	constexpr float kProfileCardHeight = 144.0f;
	constexpr float kProfileCardAccentHeight = 3.0f;
	constexpr float kProfileCardBorderThickness = 2.0f;
}

// Shared by DrawSectionHeader and DrawSubsectionLink for a consistent link + tooltip.
static void DrawFeatureLink(const char* label, Feature* feature, const char* sectionAnchor = "")
{
	if (ImGui::TextLink(label)) {
		if (auto* menu = Menu::GetSingleton())
			menu->SelectFeatureMenu(feature->GetShortName(), sectionAnchor);
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("open_feature_tooltip"), "Open this feature's full settings panel"));
}

// Section header: the feature name as a jump link to its panel, or a plain
// SeparatorText for the host (a link there would navigate to this very page).
// Draws no trailing divider of its own -- the caller closes out each section
// with a separator AFTER its content, so dividers read as "end of this
// feature's block" rather than appearing to split the link from its own body.
static void DrawSectionHeader(Feature* feature, bool linkable)
{
	const std::string label = feature->GetPerformanceSectionLabel();
	if (label.empty())
		return;
	if (!linkable) {
		ImGui::SeparatorText(label.c_str());
		return;
	}
	DrawFeatureLink(label.c_str(), feature);
}

void PerformanceRenderer::DrawSubsectionLink(const char* label, Feature* feature, const char* sectionAnchor)
{
	// Indent BEFORE drawing the link, not after -- otherwise only content following
	// the link (often nothing) ends up indented, and the link itself, the one thing
	// meant to visually nest, stays flush with the top-level section header above it.
	ImGui::Indent();
	DrawFeatureLink(label, feature, sectionAnchor);
}

// Draws a compact Performance/Balanced/Quality override row for one feature.
static void DrawProfileButtonRow(const Feature::PerfProfile (&profiles)[3], const char* const (&labels)[3],
	const char* const (&tooltips)[3], int activeIdx, const std::function<void(Feature::PerfProfile)>& apply)
{
	for (int i = 0; i < IM_ARRAYSIZE(profiles); ++i) {
		if (i > 0)
			ImGui::SameLine();
		const bool active = i == activeIdx;
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::Button(labels[i]))
			apply(profiles[i]);
		if (active)
			ImGui::PopStyleColor();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", tooltips[i]);
	}
	if (activeIdx < 0) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", T(TKEY("profile_custom"), "(Custom)"));
	}
}

static bool DrawProfileCard(const char* title, const char* description, bool selected, const ImVec4& accentColor)
{
	const float scale = Util::GetUIScale();
	const float padding = ThemeManager::Constants::BUTTON_PADDING * scale;
	const ImVec2 cardMin = ImGui::GetCursorScreenPos();
	const ImVec2 cardSize(ImGui::GetContentRegionAvail().x, kProfileCardHeight * scale);
	const bool pressed = ImGui::InvisibleButton("##ProfileCard", cardSize);
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();
	const ImVec2 cardMax = ImGui::GetItemRectMax();

	ImVec4 backgroundColor = ImGui::GetStyleColorVec4(selected ? ImGuiCol_Button : ImGuiCol_FrameBg);
	if (hovered)
		backgroundColor = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
	if (held)
		backgroundColor = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);

	auto* drawList = ImGui::GetWindowDrawList();
	const float rounding = ImGui::GetStyle().FrameRounding;
	drawList->AddRectFilled(cardMin, cardMax, ImGui::GetColorU32(backgroundColor), rounding);
	drawList->AddRect(cardMin, cardMax,
		ImGui::GetColorU32(selected ? accentColor : ImGui::GetStyleColorVec4(ImGuiCol_Border)),
		rounding, 0, selected ? kProfileCardBorderThickness * scale : 1.0f);
	if (selected) {
		drawList->AddRectFilled(cardMin, ImVec2(cardMax.x, cardMin.y + kProfileCardAccentHeight * scale),
			ImGui::GetColorU32(accentColor), rounding, ImDrawFlags_RoundCornersTop);
	}

	drawList->PushClipRect(cardMin, cardMax, true);
	float titleFontSize = ImGui::GetFontSize();
	{
		MenuFonts::FontRoleGuard titleFont(Menu::FontRole::Subheading);
		titleFontSize = ImGui::GetFontSize();
		drawList->AddText(ImGui::GetFont(), titleFontSize, ImVec2(cardMin.x + padding, cardMin.y + padding),
			ImGui::GetColorU32(ImGuiCol_Text), title);
	}
	{
		MenuFonts::FontRoleGuard descriptionFont(Menu::FontRole::Subtext);
		const ImVec2 descriptionPosition(
			cardMin.x + padding,
			cardMin.y + padding + titleFontSize + ThemeManager::Constants::BUTTON_SPACING * scale);
		drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), descriptionPosition,
			ImGui::GetColorU32(ImGuiCol_TextDisabled), description, nullptr, cardSize.x - padding * 2.0f);
	}

	if (selected) {
		MenuFonts::FontRoleGuard statusFont(Menu::FontRole::Subtext);
		drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
			ImVec2(cardMin.x + padding, cardMax.y - padding - ImGui::GetFontSize()),
			ImGui::GetColorU32(accentColor), T(TKEY("profile_selected"), "Selected"));
	}
	drawList->PopClipRect();

	if (hovered)
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return pressed;
}

static void DrawGlobalProfileChooser(const Feature::PerfProfile (&profiles)[3], const char* const (&labels)[3],
	const char* const (&descriptions)[3], int activeIdx)
{
	const auto& theme = Menu::GetSingleton()->GetTheme();
	const float minimumCardWidth = kProfileCardMinimumWidth * Util::GetUIScale();
	const float minimumColumnWidth = minimumCardWidth + ImGui::GetStyle().CellPadding.x * 2.0f;
	const int columnCount = std::clamp(
		static_cast<int>(ImGui::GetContentRegionAvail().x / minimumColumnWidth),
		1,
		IM_ARRAYSIZE(profiles));
	if (ImGui::BeginTable("##GlobalPerformanceProfiles", columnCount,
			ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings)) {
		for (int i = 0; i < IM_ARRAYSIZE(profiles); ++i) {
			ImGui::TableNextColumn();
			ImGui::PushID(i);
			if (DrawProfileCard(labels[i], descriptions[i], i == activeIdx, theme.StatusPalette.InfoColor)) {
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard(*SceneSettingsManager::GetSingleton());
				Feature::ApplyPerformanceProfileToAll(profiles[i]);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (activeIdx < 0) {
		ImGui::TextColored(theme.StatusPalette.Warning, "%s %s",
			T(TKEY("profiles_label"), "Profile:"), T(TKEY("profile_custom"), "(Custom)"));
	}
}

static void RenderPresets(Feature* host)
{
	{
		MenuFonts::FontRoleGuard headingFont(Menu::FontRole::Heading);
		ImGui::TextUnformatted(T(TKEY("choose_preset"), "Choose a Performance Preset"));
	}
	ImGui::Spacing();
	ImGui::TextWrapped("%s", T(TKEY("intro"),
								 "Performance settings from across all features, gathered in one place. "
								 "Each section is the same control shown in that feature's own panel; "
								 "changes here take effect there too. Settings that need a restart take "
								 "effect at the next game launch."));
	ImGui::Spacing();

	// VR-only sections are skipped everywhere below (PerformanceSectionRequiresVR), and a
	// feature needs a non-empty section label to opt in at all -- else it'd get an empty node.
	std::vector<Feature*> ordered;
	for (Feature* feature : Feature::GetFeatureList())
		if (feature->loaded && (globals::game::isVR || !feature->PerformanceSectionRequiresVR()) &&
			!feature->GetPerformanceSectionLabel().empty())
			ordered.push_back(feature);
	// stable_sort keeps registration order among equal ranks (e.g. the default 1000) deterministic.
	std::stable_sort(ordered.begin(), ordered.end(),
		[](const Feature* a, const Feature* b) { return a->GetPerformanceOrder() < b->GetPerformanceOrder(); });

	// The active profile is the one every feature's settings currently match (else Custom).
	const Feature::PerfProfile profiles[3] = {
		Feature::PerfProfile::Performance, Feature::PerfProfile::Balanced, Feature::PerfProfile::Quality
	};
	int activeIdx = -1;
	for (int i = 0; i < IM_ARRAYSIZE(profiles) && activeIdx < 0; ++i) {
		bool all = true;
		for (Feature* f : ordered)
			if (!f->MatchesPerformanceProfile(profiles[i])) {
				all = false;
				break;
			}
		if (all)
			activeIdx = i;
	}

	const char* labels[3] = {
		T(TKEY("profile_performance"), "Performance"),
		T(TKEY("profile_balanced"), "Balanced"),
		T(TKEY("profile_quality"), "Quality")
	};
	// Foveation/reprojection tooltips only apply in VR. All three set Upscaling's
	// restart-gated qualityMode, so every tooltip carries that caveat, not just Quality's.
	const char* tooltips[3] = {
		globals::game::isVR ? T(TKEY("profile_performance_tooltip"), "Lowest render resolution; foveation and reprojection on. Fastest. Some changes apply on restart.") : T(TKEY("profile_performance_tooltip_flat"), "Lowest render resolution. Fastest. Some changes apply on restart."),
		globals::game::isVR ? T(TKEY("profile_balanced_tooltip"), "Mid render resolution; reprojection on. Some changes apply on restart.") : T(TKEY("profile_balanced_tooltip_flat"), "Mid render resolution. Some changes apply on restart."),
		globals::game::isVR ? T(TKEY("profile_quality_tooltip"), "Higher render resolution; reprojection off for max fidelity. Some changes apply on restart.") : T(TKEY("profile_quality_tooltip_flat"), "Higher render resolution for max fidelity. Some changes apply on restart.")
	};
	DrawGlobalProfileChooser(profiles, labels, tooltips, activeIdx);

	// Generic per-section tooltips: unlike the global row above, a section's row can't
	// claim specifics (render resolution, foveation) that only apply to SOME features.
	const char* sectionTooltips[3] = {
		T(TKEY("profile_performance_section_tooltip"), "Apply this section's Performance-tier settings only."),
		T(TKEY("profile_balanced_section_tooltip"), "Apply this section's Balanced-tier settings only."),
		T(TKEY("profile_quality_section_tooltip"), "Apply this section's Quality-tier settings only.")
	};

	// Surface the restart need here, not just inside each feature's collapsed section --
	// otherwise clicking a preset with a restart-gated field looks like it did nothing.
	bool anyPendingRestart = false;
	for (Feature* f : ordered)
		if (f->HasAnyPendingRestart()) {
			anyPendingRestart = true;
			break;
		}
	if (anyPendingRestart)
		Util::Text::RestartNeeded("%s", T(TKEY("pending_restart_banner"), "Some changes below need a restart to take effect."));

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	{
		MenuFonts::FontRoleGuard headingFont(Menu::FontRole::Heading);
		ImGui::TextUnformatted(T(TKEY("fine_tune_features"), "Fine-Tune Individual Features"));
	}
	ImGui::TextWrapped("%s", T(TKEY("fine_tune_features_description"),
								 "Override the selected preset for one feature without changing the others."));
	ImGui::Spacing();

	// Drawn in perf-impact order (GetPerformanceOrder), set up in `ordered` above.
	for (Feature* feature : ordered) {
		ImGui::PushID(feature);
		DrawSectionHeader(feature, feature != host);
		// Uniform, actionable preset row for every section: applies the profile to just
		// THIS feature, unlike the global broadcast row above.
		int featureActiveIdx = -1;
		for (int i = 0; i < IM_ARRAYSIZE(profiles) && featureActiveIdx < 0; ++i)
			if (feature->MatchesPerformanceProfile(profiles[i]))
				featureActiveIdx = i;
		// A feature can override GetProfilePreviewText to show what its click would actually
		// change (e.g. multiple interacting foveation levers); falls back to the generic text.
		std::string previewText[3];
		const char* featureTooltips[3];
		for (int i = 0; i < IM_ARRAYSIZE(profiles); ++i) {
			previewText[i] = feature->GetProfilePreviewText(profiles[i]);
			featureTooltips[i] = previewText[i].empty() ? sectionTooltips[i] : previewText[i].c_str();
		}
		DrawProfileButtonRow(profiles, labels, featureTooltips, featureActiveIdx,
			[feature](Feature::PerfProfile p) {
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard(*SceneSettingsManager::GetSingleton());
				feature->ApplyPerformanceProfile(p);
			});
		// Feature overrides stay visible beneath the global chooser.
		// Only raw sliders/knobs collapse into Advanced below.
		try {
			feature->DrawPerformancePresets();
		} catch (const std::exception& e) {
			logger::error("PerformanceRenderer: {} presets threw: {}", feature->GetShortName(), e.what());
			Util::Text::WrappedError("%s: draw error (%s)", feature->GetDisplayName().c_str(), e.what());
		} catch (...) {
			logger::error("PerformanceRenderer: {} presets threw (unknown)", feature->GetShortName());
			Util::Text::WrappedError("%s: draw error (unknown)", feature->GetDisplayName().c_str());
		}
		// Collapsed by default: for verifying what a preset changed or fine-tuning past it.
		if (ImGui::TreeNodeEx(T(TKEY("section_advanced"), "Advanced"), ImGuiTreeNodeFlags_None)) {
			// Isolate each feature's draw so one throwing hook can't blank the rest of the page.
			// Logged, not just shown inline, so a reported red box is diagnosable from the log.
			try {
				feature->DrawPerformanceSettings();
			} catch (const std::exception& e) {
				logger::error("PerformanceRenderer: {} threw: {}", feature->GetShortName(), e.what());
				Util::Text::WrappedError("%s: draw error (%s)", feature->GetDisplayName().c_str(), e.what());
			} catch (...) {
				logger::error("PerformanceRenderer: {} threw (unknown)", feature->GetShortName());
				Util::Text::WrappedError("%s: draw error (unknown)", feature->GetDisplayName().c_str());
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
		// Closes out this feature's block, not the header above -- keeps the divider
		// meaning consistent ("end of a section") no matter how much content it drew.
		ImGui::Separator();
		ImGui::Spacing();
	}
}

void PerformanceRenderer::Render(Feature* host)
{
	if (ImGui::BeginTabBar("##PerformanceTabs", ImGuiTabBarFlags_None)) {
		if (MenuFonts::BeginTabItemWithFont(T(TKEY("tab_presets"), "Presets"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##PerformancePresetsContent", ImVec2(0, 0), false))
				RenderPresets(host);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (MenuFonts::BeginTabItemWithFont(T(TKEY("tab_overlay"), "Overlay"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##PerformanceOverlayContent", ImVec2(0, 0), false))
				globals::features::performanceOverlay.DrawSettings();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (MenuFonts::BeginTabItemWithFont(T("menu.features.profiling", "Profiling"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##PerformanceProfilingContent", ImVec2(0, 0), false))
				ProfilingRenderer::RenderStatistics();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

#undef I18N_KEY_PREFIX
