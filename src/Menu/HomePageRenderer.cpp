#include "HomePageRenderer.h"
#include "PCH.h"

#include <imgui.h>

#include "Feature.h"
#include "FeatureConstraints.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Plugin.h"
#include "ShaderCache.h"
#include "State.h"

// Static member definitions
bool HomePageRenderer::isFirstTimeSetupShown = false;
uint32_t HomePageRenderer::keyThatClosedDialog = 0;

bool HomePageRenderer::ShouldSkipKeyRelease(uint32_t key)
{
	if (keyThatClosedDialog && key == keyThatClosedDialog) {
		keyThatClosedDialog = 0;
		return true;
	}
	return false;
}

void HomePageRenderer::RenderHomePage()
{
	ImGui::BeginChild("HomePage", ImVec2(0, 0), false);

	RenderWelcomeSection();
	ImGui::Spacing();

	RenderCacheMismatchSection();

	RenderActiveConstraintsSection();

	RenderQuickLinksSection();
	ImGui::Spacing();

	RenderFAQSection();

	ImGui::EndChild();
}

void HomePageRenderer::RenderWelcomeSection()
{
	float scale = Util::GetUIScale();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 8 * scale));

	// Main title - centered with safe font handling
	ImGuiIO& io = ImGui::GetIO();
	ImFont* titleFont = nullptr;

	// Safely check if we have multiple fonts and the second one is valid
	if (io.Fonts && io.Fonts->Fonts.Size > 1 && io.Fonts->Fonts[1] != nullptr) {
		titleFont = io.Fonts->Fonts[1];
	}

	// Scale the text to make it larger (2.0x size)
	ImGui::SetWindowFontScale(TITLE_FONT_SCALE);

	// Only push font if we have a valid one, otherwise use default scaled
	if (titleFont) {
		ImGui::PushFont(titleFont, titleFont->LegacySize);
	}

	ImVec2 windowSize = ImGui::GetWindowSize();
	auto versionStr = Util::GetFormattedVersion(Plugin::VERSION);
	auto expectedTag = std::format("v{}", versionStr);
	std::string titleWithVersion = Plugin::BUILD_DESCRIBE == expectedTag ?
	                                   I18n::GetSingleton()->Format("menu.home.welcome", { { "version", std::string(versionStr) } }, "Welcome to Open Shaders {version}") :
	                                   I18n::GetSingleton()->Format("menu.home.welcome_dev", { { "version", std::string(versionStr) }, { "build", std::string(Plugin::BUILD_DESCRIBE) } }, "Welcome to Open Shaders {version} [{build}]");
	ImVec2 titleSize = ImGui::CalcTextSize(titleWithVersion.c_str());
	ImGui::SetCursorPosX((windowSize.x - titleSize.x) * 0.5f);
	ImGui::Text("%s", titleWithVersion.c_str());

	// Only pop font if we pushed one
	if (titleFont) {
		ImGui::PopFont();
	}

	// Reset text scale back to normal
	ImGui::SetWindowFontScale(1.0f);

	ImGui::Spacing();

	// Intro text - centered
	const char* introText = T("menu.home.intro",
		"Open Shaders is a fork of Community Shaders providing advanced graphics enhancements for Skyrim.\n"
		"This comprehensive collection of features brings modern rendering techniques\n"
		"to enhance your visual experience.");
	ImVec2 introSize = ImGui::CalcTextSize(introText);
	ImGui::SetCursorPosX((windowSize.x - introSize.x) * 0.5f);
	ImGui::TextWrapped("%s", introText);

	ImGui::Spacing();

	// VR discoverability: name the location and offer a click straight into the VR feature
	// (Performance is its first tab).
	if (globals::game::isVR) {
		ImGui::TextWrapped("%s", T("menu.home.performance_tip",
									 "One-tap performance profiles (foveation, reprojection, render resolution) are on "
									 "the VR feature's Performance tab:"));
		if (ImGui::SmallButton(T("menu.home.performance_link", "Open Performance"))) {
			if (auto* menu = Menu::GetSingleton())
				menu->SelectFeatureMenu("VR");
		}
		ImGui::Spacing();
	}

	ImGui::PopStyleVar();
}

void HomePageRenderer::RenderQuickLinksSection()
{
	// Quick Links title - centered
	ImVec2 windowSize = ImGui::GetWindowSize();
	const char* quickLinksTitle = T("menu.home.quick_links", "Quick Links");
	ImVec2 titleSize = ImGui::CalcTextSize(quickLinksTitle);
	ImGui::SetCursorPosX((windowSize.x - titleSize.x) * 0.5f);
	ImGui::Text("%s", quickLinksTitle);

	// Nexus button → the Open Shaders fork page (mod 180419).
	ImGui::Columns(4, nullptr, false);

	if (ImGui::Button(T("menu.home.nexus_mods", "Nexus Mods"), ImVec2(-1, 0))) {
		ShellExecuteA(NULL, "open", "https://www.nexusmods.com/skyrimspecialedition/mods/180419", NULL, NULL, SW_SHOWNORMAL);
	}

	// Discord button → the Open Shaders fork's own server invite.
	ImGui::NextColumn();
	if (ImGui::Button(T("menu.home.join_discord", "Join Discord"), ImVec2(-1, 0))) {
		ShellExecuteA(NULL, "open", "https://discord.gg/V6qa82Spxw", NULL, NULL, SW_SHOWNORMAL);
	}

	ImGui::NextColumn();
	if (ImGui::Button(T("menu.home.github", "GitHub"), ImVec2(-1, 0))) {
		ShellExecuteA(NULL, "open", "https://github.com/alandtse/open-shaders", NULL, NULL, SW_SHOWNORMAL);
	}

	ImGui::NextColumn();
	if (ImGui::Button(T("menu.home.dev_wiki", "Developer Wiki"), ImVec2(-1, 0))) {
		ShellExecuteA(NULL, "open", "https://github.com/alandtse/open-shaders/wiki", NULL, NULL, SW_SHOWNORMAL);
	}

	ImGui::Columns(1);
}

void HomePageRenderer::RenderFAQSection()
{
	// FAQ title - centered
	ImVec2 windowSize = ImGui::GetWindowSize();
	const char* faqTitle = T("menu.faq.title", "Frequently Asked Questions");
	ImVec2 titleSize = ImGui::CalcTextSize(faqTitle);
	ImGui::SetCursorPosX((windowSize.x - titleSize.x) * 0.5f);
	ImGui::Text("%s", faqTitle);
	ImGui::Separator();

	// FAQ items with collapsible headers
	if (ImGui::CollapsingHeader(T("menu.faq.q1", "What is Open Shaders?"))) {
		ImGui::TextWrapped("%s", T("menu.faq.a1",
									 "Open Shaders is a fork of Community Shaders that ships features the upstream project "
									 "has not yet released. Both projects are comprehensive graphics enhancement frameworks "
									 "for Skyrim that provide advanced lighting, materials, and visual effects. They're "
									 "designed to be modular, letting you enable only the features you want while "
									 "maintaining good performance. This fork preserves the upstream runtime layout so user "
									 "settings and themes are compatible."));
	}

	if (ImGui::CollapsingHeader(T("menu.faq.q2", "How do I configure features?"))) {
		ImGui::TextWrapped("%s", T("menu.faq.a2",
									 "Each feature can be found in the left sidebar menu. Click on any feature to access its "
									 "settings. Most features include presets and detailed tooltips to help you understand "
									 "what each setting does."));
	}

	if (ImGui::CollapsingHeader(T("menu.faq.q3", "Why are some features not loading?"))) {
		ImGui::TextWrapped("%s", T("menu.faq.a3",
									 "Features may fail to load due to hardware incompatibility, missing dependencies, or "
									 "conflicts with other mods. Check the 'Feature Issues' tab for detailed information "
									 "about any problematic features."));
	}

	if (ImGui::CollapsingHeader(T("menu.faq.q4", "I have \"Failed Shaders\" when compiling?"))) {
		ImGui::TextWrapped("%s", T("menu.faq.a4",
									 "Failed shaders are usually caused by mixed file versions. Ensure all features are up to date "
									 "and avoid mixing files from test builds or outdated versions. Please review the 'Feature Issues' tab "
									 "and/or Wiki for more information. Update your features and remove any obsolete features."));
	}

	if (ImGui::CollapsingHeader(T("menu.faq.q5", "How do I improve performance?"))) {
		ImGui::TextWrapped("%s", T("menu.faq.a5",
									 "Start by enabling the Performance Overlay to monitor your FPS. Consider disabling "
									 "expensive features like Screen Space GI or reducing quality settings. The 'Display' "
									 "tab also includes upscaling options that can improve performance."));
	}

	if (ImGui::CollapsingHeader(T("menu.faq.q6", "Is Open Shaders compatible with ENB?"))) {
		ImGui::TextWrapped("%s", T("menu.faq.a6",
									 "No, Open Shaders (like upstream Community Shaders) is not compatible with ENB. The "
									 "plugin will automatically disable itself if ENB is detected."));
	}

	if (ImGui::CollapsingHeader(T("menu.faq.q7", "The menu hotkey isn't working!"))) {
		ImGui::TextWrapped("%s", T("menu.faq.a7",
									 "By default, Open Shaders uses the END key to open this menu. If your keyboard "
									 "doesn't have an END key or it's not working, you can change it in the General > Keybindings tab. "
									 "You can also edit the hotkey in the JSON configuration files."));
	}

	if (ImGui::CollapsingHeader(T("menu.faq.q8", "I would like to help develop Open Shaders."))) {
		ImGui::TextWrapped("%s", T("menu.faq.a8",
									 "Open Shaders is open source. Check out the upstream GitHub wiki for contribution "
									 "guidelines on the shared architecture; open issues and PRs against this repository for "
									 "fork-specific work, or against upstream Community Shaders for changes that benefit both "
									 "projects. Whether you're interested in shader programming, C++ development, or "
									 "documentation, there's always something to contribute."));
	}

	if (ImGui::CollapsingHeader(T("menu.faq.q9", "Is Open Shaders open source?"))) {
		ImGui::TextWrapped("%s", T("menu.faq.a9",
									 "Yes! Open Shaders is completely open source and available on GitHub, as is upstream "
									 "Community Shaders. You can view the source code, report issues, suggest features, and "
									 "contribute to either project. Both are licensed under GPL, ensuring they remain free and "
									 "open for everyone. Branding materials and assets (icons, Nexus branding, typography, etc.) "
									 "are not covered by the GPL Licence. Any included assets may not be used without explicit "
									 "permission."));
	}
}

void HomePageRenderer::RenderCacheMismatchSection()
{
	auto shaderCache = globals::shaderCache;
	if (!shaderCache || (!shaderCache->IsDiskCacheHeld() &&
							!shaderCache->HasFeatureSetChanges() &&
							!shaderCache->HasFeatureSetRevertPending() &&
							!shaderCache->HasPreviousDiskCache()))
		return;

	auto menu = Menu::GetSingleton();
	ImVec4 warningColor = menu ? menu->GetTheme().StatusPalette.Warning : ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
	const bool featureSetChanged = shaderCache->HasFeatureSetChanges();
	const bool revertPending = shaderCache->HasFeatureSetRevertPending();
	const bool featureSetCacheBackedUp = shaderCache->HasFeatureSetCacheBackup();
	const bool previousCacheAvailable = shaderCache->HasPreviousDiskCache();
	const bool cacheHeld = shaderCache->IsDiskCacheHeld() && !featureSetChanged && !revertPending;
	const bool featureChangeHeld = shaderCache->IsDiskCacheHeld() && featureSetChanged && !featureSetCacheBackedUp;

	ImGui::PushStyleColor(ImGuiCol_Text, warningColor);
	bool headerOpen = ImGui::CollapsingHeader(T("menu.home.cache_mismatch", "Shader Cache Mismatch Detected"), ImGuiTreeNodeFlags_DefaultOpen);
	ImGui::PopStyleColor();
	if (!headerOpen)
		return;

	if (revertPending) {
		ImGui::TextColored(menu ? menu->GetTheme().StatusPalette.RestartNeeded : ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
			"%s", T("menu.home.cache_mismatch_prev_restored", "Previous shader cache restored; restart to use it."));
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		return;
	}

	const char* descText = cacheHeld ?
	                           T("menu.home.cache_mismatch_desc", "A feature used by the saved shader cache is missing or failed to load. CS is keeping the saved disk cache unchanged until you choose what to do.") :
	                       featureChangeHeld ?
	                           T("menu.home.cache_mismatch_no_rollback", "Your feature setup changed, but CS could not save a rollback copy. CS is keeping the saved disk cache unchanged until it can rebuild safely.") :
	                       featureSetChanged ?
	                           T("menu.home.cache_mismatch_changed_desc", "Your feature setup changed. CS saved the previous shader cache and is building a new cache for the current setup. If you do nothing, future launches will use the new cache after compilation finishes.") :
	                           T("menu.home.cache_mismatch_prev_available", "A previous shader cache is available. Restore it if you want to go back to the earlier feature setup.");

	ImGui::TextWrapped("%s", descText);
	ImGui::Spacing();

	using MismatchKind = Util::CacheInvalidation::CacheMismatch::Kind;
	const auto& mismatches = (cacheHeld || featureChangeHeld) ? shaderCache->GetCacheMismatches() :
	                                                            shaderCache->GetPreviousCacheMismatches();
	for (const auto& mismatch : mismatches) {
		const char* detail = mismatch.detail.c_str();
		if (mismatch.kind == MismatchKind::EnabledFlip) {
			if (cacheHeld || featureChangeHeld) {
				detail = mismatch.nowPresent ?
				             T("menu.home.cache_mismatch_added", "in your setup now, but missing from the saved cache") :
				             T("menu.home.cache_mismatch_removed", "in the saved cache, but missing from your setup now");
			} else {
				detail = mismatch.nowPresent ?
				             T("menu.home.cache_mismatch_added_prev", "enabled now; previous cache had it disabled") :
				             T("menu.home.cache_mismatch_removed_prev", "disabled now; previous cache had it enabled");
			}
		}
		ImGui::BulletText("%s: %s", mismatch.feature.c_str(), detail);
	}
	ImGui::Spacing();

	const char* resolveText = cacheHeld ?
	                              T("menu.home.cache_mismatch_resolve", "Review the listed feature. If CS menu > Feature Issues is available, check it for details. Fix the feature install in your mod manager and restart to reuse the saved cache, or rebuild for the current setup if the feature was removed intentionally:") :
	                          featureChangeHeld ?
	                              T("menu.home.cache_mismatch_no_rollback_desc", "Restore is unavailable because CS could not save a rollback copy. Let compilation finish to rebuild the cache for the current setup.") :
	                              T("menu.home.cache_mismatch_restore_desc", "To go back, restore the previous cache and boot toggles, then restart:");

	ImGui::TextWrapped("%s", resolveText);
	ImGui::Spacing();

	// Every shown mismatch is an EnabledFlip (a Disable-at-Boot difference), so matching the
	// cache's enabled-state lets the next boot reuse it with no recompile. Disabled when a flip
	// is for a feature that failed to load: settings can't match a broken feature (the hover explains).
	const auto* blockingMismatch = Util::CacheInvalidation::FindMatchBlockingFeature(shaderCache->GetCacheMismatches());
	const char* blockingFeature = blockingMismatch ? blockingMismatch->feature.c_str() : nullptr;
	const bool canMatch = blockingFeature == nullptr;
	static bool s_matchApplied = false;
	static bool s_restoreFailed = false;
	// Clear the "restart to reuse" confirmation if the mismatch set changes underneath us.
	static size_t s_lastMismatchCount = 0;
	if (const size_t mismatchCount = shaderCache->GetCacheMismatches().size(); mismatchCount != s_lastMismatchCount) {
		s_matchApplied = false;
		s_restoreFailed = false;
		s_lastMismatchCount = mismatchCount;
	}

	if (!cacheHeld && !featureChangeHeld) {
		const bool isCompiling = shaderCache->IsCompiling();
		const bool restoreDisabled = !previousCacheAvailable || (featureSetChanged && !featureSetCacheBackedUp);
		if (restoreDisabled)
			ImGui::BeginDisabled();

		const bool restoreClicked = ImGui::Button(isCompiling ?
													  T("menu.home.cache_mismatch_cancel_restore", "Cancel Compile & Restore Previous Cache") :
													  T("menu.home.cache_mismatch_restore", "Restore Previous Cache & Restart"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", isCompiling ?
								  T("menu.home.cache_mismatch_cancel_restore_tooltip", "Stops the in-progress compile (discarding its progress) and swaps the previous shader cache back into use. Restart required.") :
								  T("menu.home.cache_mismatch_restore_tooltip", "Swaps the previous shader cache back into use and resets boot toggles to match it. Keeps a backup copy of compiled shaders; uses extra disk space. Restart required."));
		}
		static bool s_showCancelConfirm = false;
		if (restoreClicked) {
			if (isCompiling)
				s_showCancelConfirm = true;
			else
				s_restoreFailed = !shaderCache->RestorePreviousDiskCache();
		}

		if (restoreDisabled)
			ImGui::EndDisabled();

		if (isCompiling) {
			ImGui::TextDisabled("%s", shaderCache->GetShaderStatsString().c_str());
		} else if (s_restoreFailed) {
			ImGui::TextColored(menu ? menu->GetTheme().StatusPalette.Error : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
				"%s", T("menu.home.cache_mismatch_restore_failed", "Restore failed. Check CommunityShaders.log for details."));
		}

		if (s_showCancelConfirm) {
			constexpr const char* popupId = "###CancelCompileRestoreConfirm";
			const std::string popupTitle = fmt::format("{}{}", T("menu.home.cache_mismatch_cancel_restore_title", "Cancel Compile?"), popupId);
			ImGui::OpenPopup(popupId);
			ImVec2 center = ImGui::GetMainViewport()->GetCenter();
			ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			if (Util::BeginPopupModalWithRoundedClose(popupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::TextWrapped("%s", T("menu.home.cache_mismatch_cancel_restore_body", "The in-progress shader compile will be stopped and its progress discarded. You'll need to recompile later if you want the new feature set."));
				ImGui::Spacing();
				if (ImGui::Button(T("menu.home.cache_mismatch_cancel_restore_confirm", "Cancel Compile & Restore"))) {
					s_restoreFailed = !shaderCache->RestorePreviousDiskCache();
					s_showCancelConfirm = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(T("menu.home.cache_mismatch_cancel_restore_keep_compiling", "Keep Compiling"))) {
					s_showCancelConfirm = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		}
	}

	if (cacheHeld) {
		// After Match the choice is made (reuse on restart): show only the confirmation and drop
		// both buttons, so the resolved state doesn't leave Rebuild beside the reuse message.
		if (s_matchApplied) {
			ImGui::TextColored(menu ? menu->GetTheme().StatusPalette.RestartNeeded : ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
				"%s", T("menu.home.cache_mismatch_matched", "Boot settings updated; restart to reuse the saved cache."));
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			return;
		}

		if (!canMatch)
			ImGui::BeginDisabled();
		const bool matchClicked = ImGui::Button(T("menu.home.cache_mismatch_match", "Match Cache & Restart"));
		if (!canMatch)
			ImGui::EndDisabled();
		if (auto _ttm = Util::HoverTooltipWrapper()) {
			if (canMatch)
				ImGui::Text("%s", T("menu.home.cache_mismatch_match_tooltip", "Sets your Disable-at-Boot toggles to match the saved cache, so a restart reuses it with no recompile."));
			else
				ImGui::Text("%s", std::vformat(T("menu.home.cache_mismatch_match_blocked", "Unavailable: '{}' could not load, so settings can't match the cache. Check Feature Issues for details, fix the install, or rebuild."), std::make_format_args(blockingFeature)).c_str());
		}
		if (matchClicked) {
			if (auto* state = globals::state) {
				for (const auto& m : shaderCache->GetCacheMismatches()) {
					if (m.kind != MismatchKind::EnabledFlip)
						continue;
					state->SetFeatureDisabled(m.shortName, m.nowPresent);
				}
				state->Save();
				s_matchApplied = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(T("menu.home.cache_mismatch_rebuild", "Rebuild Cache for Current Features"))) {
			shaderCache->AcceptCacheRebuild();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("menu.home.cache_mismatch_rebuild_tooltip",
								  "Discards the saved cache and recompiles all shaders for your current features. "
								  "Takes a while, but stops the every-launch recompile."));
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
}

void HomePageRenderer::RenderActiveConstraintsSection()
{
	auto constraints = FeatureConstraints::GetAllActiveConstraints();
	if (constraints.empty()) {
		return;  // Don't show section if there are no active constraints
	}

	ImGui::Spacing();

	// Use warning color for the header to draw attention
	ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
	bool headerOpen = ImGui::CollapsingHeader(T("menu.home.active_constraints", "Active Setting Constraints"), ImGuiTreeNodeFlags_None);
	ImGui::PopStyleColor();

	if (headerOpen) {
		ImGui::TextWrapped("%s", T("menu.home.constraints_desc", "Some settings are constrained by other features. Hover over rows for details."));

		ImGui::Spacing();

		// Prepare data for table
		struct ConstraintRow
		{
			std::string setting;
			std::string forcedTo;
			std::string constrainedBy;
			std::string firstSourceShortName;  // For "navigate to feature" on click
			std::string tooltip;
		};

		std::vector<ConstraintRow> rows;
		for (const auto& [settingId, result] : constraints) {
			ConstraintRow row;
			row.setting = std::format("{}.{}", settingId.featureShortName, settingId.settingPath);
			row.forcedTo = FeatureConstraints::FormatConstraintValue(result.forcedValue);
			for (size_t i = 0; i < result.sources.size(); ++i) {
				if (i > 0)
					row.constrainedBy += ", ";
				row.constrainedBy += result.sources[i].featureName;
			}
			if (!result.sources.empty()) {
				row.firstSourceShortName = result.sources[0].featureShortName;
			}
			// Build tooltip
			for (const auto& src : result.sources) {
				if (!row.tooltip.empty())
					row.tooltip += "\n";
				row.tooltip += std::format("{}: {}", src.featureName, src.reason);
				if (src.recommendDisableAtBoot) {
					row.tooltip += std::string("\n") + T("menu.home.consider_disabling_at_boot", "Consider disabling at boot.");
				}
			}
			rows.push_back(row);
		}

		// Define headers
		std::vector<std::string> headers = { T("menu.home.constraint_header_setting", "Setting"), T("menu.home.constraint_header_forced_to", "Forced To"), T("menu.home.constraint_header_constrained_by", "Constrained By") };

		// Custom sorts (string comparators for each column)
		std::vector<std::function<bool(const ConstraintRow&, const ConstraintRow&, bool)>> customSorts = {
			[](const ConstraintRow& a, const ConstraintRow& b, bool asc) { return Util::StringSortComparator(a.setting, b.setting, asc); },
			[](const ConstraintRow& a, const ConstraintRow& b, bool asc) { return Util::StringSortComparator(a.forcedTo, b.forcedTo, asc); },
			[](const ConstraintRow& a, const ConstraintRow& b, bool asc) { return Util::StringSortComparator(a.constrainedBy, b.constrainedBy, asc); }
		};

		// Cell render -- column 2 ("Constrained By") is clickable to navigate
		// to the first source feature's settings page.
		auto cellRender = [](int rowIdx, int colIdx, const ConstraintRow& row) {
			if (colIdx == 0) {
				Util::RenderTableCell(row.setting, "", "", nullptr, ImVec4(1, 1, 1, 1), true, Util::Colors::GetWarning());
			} else if (colIdx == 1) {
				Util::RenderTableCell(row.forcedTo, "", "", nullptr, ImVec4(1, 1, 1, 1), true);
			} else if (colIdx == 2) {
				if (!row.firstSourceShortName.empty()) {
					if (ImGui::Selectable(std::format("{}##nav{}", row.constrainedBy, rowIdx).c_str())) {
						if (auto* menu = Menu::GetSingleton()) {
							menu->SelectFeatureMenu(row.firstSourceShortName);
						}
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", I18n::GetSingleton()->Format("menu.home.click_to_navigate",
																  { { "feature", row.constrainedBy } },
																  "Click to navigate to {feature}")
											  .c_str());
						if (!row.tooltip.empty()) {
							ImGui::Separator();
							ImGui::Text("%s", row.tooltip.c_str());
						}
					}
				} else {
					Util::RenderTableCell(row.constrainedBy, "", row.tooltip, nullptr, ImVec4(1, 1, 1, 1), true);
				}
			}
		};

		// Render table
		Util::ShowSortedStringTableCustom<ConstraintRow>(
			"ConstraintsTable",
			headers,
			rows,
			0,     // sortColumn
			true,  // ascending
			customSorts,
			cellRender);
	}

	ImGui::Spacing();
}

void HomePageRenderer::RenderFirstTimeSetupDialog()
{
	if (!ShouldShowFirstTimeSetup()) {
		return;
	}

	// Block input to the game and make cursor visible - input blocking is handled by ShouldSwallowInput()
	auto& io = ImGui::GetIO();
	io.WantCaptureMouse = true;
	io.WantCaptureKeyboard = true;
	io.MouseDrawCursor = true;  // Show ImGui cursor

	float uiScale = Util::GetUIScale();
	ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
	ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(DIALOG_MIN_WIDTH * uiScale, 0), ImVec2(DIALOG_MAX_WIDTH * uiScale, FLT_MAX));
	ImGui::SetNextWindowFocus();

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, DIALOG_CORNER_ROUNDING * uiScale);

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
	                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
	                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize;

	if (!ImGui::Begin("##FirstTimeSetup", nullptr, flags)) {
		ImGui::PopStyleVar();
		ImGui::End();
		return;
	}

	// Fullscreen fade on the dialog's draw list — covers all windows beneath at the dialog's z-position
	auto* drawList = ImGui::GetWindowDrawList();
	drawList->PushClipRectFullScreen();
	drawList->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, MODAL_OVERLAY_ALPHA));
	drawList->PopClipRect();

	auto menu = Menu::GetSingleton();

	// Render CS logo as background watermark with proper aspect ratio
	if (menu && menu->uiIcons.logo.texture) {
		ImVec2 windowPos = ImGui::GetWindowPos();
		ImVec2 windowSize = ImGui::GetWindowSize();

		// Get the original texture size to maintain aspect ratio
		ImVec2 textureSize = menu->uiIcons.logo.size;
		float aspectRatio = textureSize.x / textureSize.y;

		// Set desired height and calculate width to maintain aspect ratio
		float logoHeight = LOGO_WATERMARK_HEIGHT * uiScale;
		float logoWidth = logoHeight * aspectRatio;

		ImVec2 logoMin(windowPos.x + (windowSize.x - logoWidth) * 0.5f,
			windowPos.y + (windowSize.y - logoHeight) * 0.5f);
		ImVec2 logoMax(logoMin.x + logoWidth, logoMin.y + logoHeight);

		// Determine watermark color based on monochrome logo setting
		ImU32 watermarkColor;
		if (menu->GetSettings().Theme.UseMonochromeLogo) {
			ImVec4 textColor = menu->GetSettings().Theme.Palette.Text;
			textColor.w = 0.24f;  // Low alpha for watermark effect
			watermarkColor = ImGui::GetColorU32(textColor);
		} else {
			watermarkColor = IM_COL32(255, 255, 255, 60);
		}

		// Render as subtle watermark background
		ImGui::GetWindowDrawList()->AddImage(menu->uiIcons.logo.texture, logoMin, logoMax,
			ImVec2(0, 0), ImVec2(1, 1), watermarkColor);
	}

	// Center all content
	float windowWidth = ImGui::GetWindowWidth();
	auto centerText = [windowWidth](const char* text) {
		ImGui::SetCursorPosX((windowWidth - ImGui::CalcTextSize(text).x) * 0.5f);
	};
	auto centerWidth = [windowWidth](float width) {
		ImGui::SetCursorPosX((windowWidth - width) * 0.5f);
	};

	// Version text - two lines, both centered (reduced spacing between lines)
	const char* versionLine1 = T("menu.setup.new_install_line1", "This appears to be a new install, update, or");
	const char* versionLine2 = T("menu.setup.new_install_line2", "reinstallation of Open Shaders.");

	centerText(versionLine1);
	ImGui::Text("%s", versionLine1);
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() - DIALOG_LINE_TIGHTEN * uiScale);
	centerText(versionLine2);
	ImGui::Text("%s", versionLine2);

	ImGui::Spacing();

	// Description - centered
	const char* description = T("menu.setup.choose_hotkey", "Please choose a hotkey to access the menu:");
	centerText(description);
	ImGui::Text("%s", description);

	// Hotkey selection - clickable hotkey text
	// Show current toggle key and allow user to change it by clicking on it
	auto& themeSettings = menu->GetTheme();
	bool isCapturing = menu->settingToggleKey;

	// Increase font size for hotkey text - bigger when capturing
	ImGui::SetWindowFontScale(isCapturing ? HOTKEY_TEXT_SCALE_CAPTURING : HOTKEY_TEXT_SCALE);

	// Format hotkey with brackets to make it look like a button
	std::string hotkeyStr;
	if (isCapturing) {
		hotkeyStr = "[ ... ]";
	} else {
		auto& keys = menu->GetSettings().ToggleKey;
		hotkeyStr = std::string("[ ") + Util::Input::KeyIdToString(keys) + " ]";
	}

	ImVec2 hotkeyTextSize = ImGui::CalcTextSize(hotkeyStr.c_str());

	centerWidth(hotkeyTextSize.x);
	ImVec2 buttonPos = ImGui::GetCursorScreenPos();

	// Create invisible button for hover detection and clicking
	ImGui::PushID("HotkeyButton");
	bool clicked = ImGui::InvisibleButton("##HotkeyClick", hotkeyTextSize);
	bool hovered = ImGui::IsItemHovered();
	ImGui::PopID();

	// Set cursor position back for text rendering
	ImGui::SetCursorScreenPos(buttonPos);

	// Choose color based on state
	ImVec4 hotkeyColor;
	if (isCapturing) {
		// Pulsing effect using theme's hotkey color
		hotkeyColor = Util::GetPulsingColor(themeSettings.StatusPalette.CurrentHotkey);
	} else if (hovered) {
		hotkeyColor = ImVec4(themeSettings.StatusPalette.CurrentHotkey.x * HOTKEY_HOVER_DIM_FACTOR,
			themeSettings.StatusPalette.CurrentHotkey.y * HOTKEY_HOVER_DIM_FACTOR,
			themeSettings.StatusPalette.CurrentHotkey.z * HOTKEY_HOVER_DIM_FACTOR,
			themeSettings.StatusPalette.CurrentHotkey.w);
	} else {
		hotkeyColor = themeSettings.StatusPalette.CurrentHotkey;
	}

	ImGui::TextColored(hotkeyColor, "%s", hotkeyStr.c_str());

	ImGui::SetWindowFontScale(1.0f);

	// Handle click to start hotkey capture
	if (clicked && !isCapturing) {
		// Prevent starting capture if this click was caused by Enter key,
		// because we want Enter to close the dialog instead.
		if (!ImGui::IsKeyPressed(ImGuiKey_Enter))
			menu->settingToggleKey = true;
	}

	// Show hotkey capture message when in capture mode
	if (isCapturing) {
		const char* pressKeyText = T("menu.setup.press_any_key", "Press any key to set as toggle key...");
		centerText(pressKeyText);
		ImGui::TextDisabled("%s", pressKeyText);
	}

	// CS Editor hotkey status — updates live as user picks keys
	{
		auto& csEditorKey = menu->GetSettings().CSEditorToggleKey;
		if (csEditorKey.empty()) {
			const char* warnText = T("menu.setup.cs_editor_unbound", "OS Editor hotkey unbound - chosen key uses Shift");
			centerText(warnText);
			Util::Text::Warning("%s", warnText);
		} else {
			std::string infoStr = I18n::GetSingleton()->Format("menu.setup.cs_editor_will_be",
				{ { "key", Util::Input::KeyIdToString(csEditorKey) } },
				"OS Editor hotkey will be: {key}");
			centerText(infoStr.c_str());
			ImGui::TextDisabled("%s", infoStr.c_str());
		}
	}

	ImGui::Spacing();

	const char* laterText = T("menu.setup.change_later", "You can change this later in General > Keybindings.");
	centerText(laterText);
	ImGui::Text("%s", laterText);

	ImGui::Spacing();

	// Check for Enter or Escape key to close, but only if not capturing a hotkey
	bool escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape);
	if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || escapePressed) && !isCapturing) {
		MarkFirstTimeSetupComplete(escapePressed ? VK_ESCAPE : VK_RETURN);
	}

	// Help text with breathing animation
	const char* helpText = T("menu.setup.press_to_close", "Press Escape or Enter to continue");

	ImGui::SetWindowFontScale(HELP_TEXT_SCALE);
	centerText(helpText);
	Util::DrawBreathingText(helpText);

	ImGui::SetWindowFontScale(1.0f);

	ImGui::End();
	ImGui::PopStyleVar();
}

bool HomePageRenderer::ShouldShowFirstTimeSetup()
{
	// Never show first-time setup in VR mode
	if (globals::game::isVR) {
		return false;
	}

	// Check if already completed this session
	if (isFirstTimeSetupShown) {
		return false;
	}

	// Check if first-time setup has been completed using the Menu settings
	auto menu = Menu::GetSingleton();
	return !menu->GetSettings().FirstTimeSetupCompleted;
}

void HomePageRenderer::MarkFirstTimeSetupComplete(uint32_t closingKey)
{
	// Set the flag in the Menu settings
	auto menu = Menu::GetSingleton();
	menu->GetSettings().FirstTimeSetupCompleted = true;
	// Ensure we are not capturing a hotkey when closing the dialog
	menu->settingToggleKey = false;

	// Immediately save settings to ensure the flag is persisted
	// This prevents the welcome screen from showing again even if user doesn't manually save
	globals::state->Save();

	isFirstTimeSetupShown = true;
	keyThatClosedDialog = closingKey;
}
