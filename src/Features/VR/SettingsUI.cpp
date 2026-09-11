#include "FeatureConstraints.h"
#include "Features/DynamicCubemaps.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/Upscaling.h"
#include "Features/VR.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/Fonts.h"
#include "RE/B/BSOpenVR.h"
#include "RE/P/PlayerCharacter.h"
#include "State.h"
#include "Utils/PerfUtils.h"
#include "Utils/UI.h"
#include "Utils/VRUtils.h"

#include <openvr.h>

#define I18N_KEY_PREFIX "feature.vr."

namespace
{
	bool BeginTabItemWithFont(const char* label, Menu::FontRole role, ImGuiTabItemFlags flags = ImGuiTabItemFlags_None)
	{
		return MenuFonts::BeginTabItemWithFont(label, role, flags);
	}
}

//=============================================================================
// OVERLAY (WELCOME MESSAGE)
//=============================================================================

// VR overlay UX (including the welcome banner) is owned by the standalone
// ImGuiVRHelper plugin. OverlayFeature::DrawOverlay is pure virtual, so VR
// keeps an empty override; no built-in overlay fallback.
void VR::DrawOverlay() {}

//=============================================================================
// ANONYMOUS NAMESPACE: SETTINGS PANEL DRAW FUNCTIONS
//=============================================================================

namespace
{
	void DrawStereoSettings()
	{
		auto& vr = globals::features::vr;
		VR::Settings& settings = vr.settings;

		if (ImGui::CollapsingHeader(T(TKEY("stereo_reprojection_header"), "Stereo Reprojection"), ImGuiTreeNodeFlags_DefaultOpen))
			vr.stereoOpt.DrawSettings();

		bool hasEffects = VR::AnyScreenSpaceEffectLoaded();
		bool isDev = globals::state && globals::state->IsDeveloperMode();

		// Developer-only: superseded by per-effect cross-eye reproject (SSS/SSGI) plus the
		// native eye-1 G-buffer fill; kept as a stereo-disparity inspector, not a user knob.
		if (isDev && ImGui::CollapsingHeader(T(TKEY("stereo_blend_header"), "Stereo Blend"), ImGuiTreeNodeFlags_DefaultOpen)) {
			if (!hasEffects)
				ImGui::TextColored(ImVec4(0.6f, 0.6f, 1.0f, 1.0f), "%s", T(TKEY("stereo_blend_dev_mode"), "Developer mode: no screen-space effects active."));

			ImGui::Checkbox(T(TKEY("stereo_blend_enable"), "Enable Stereo Blend"), &settings.EnableStereoBlend);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s",
					T(TKEY("stereo_blend_enable_tooltip"),
						"Post-composite depth-aware bilateral blend between eyes.\n"
						"Reduces stereo inconsistencies from screen-space effects (SSGI, SSR, etc.).\n"
						"Each pixel is reprojected to the other eye; blending is applied only where\n"
						"depth agrees (same surface). Full-screen pass in VR."));
			}

			ImGui::BeginDisabled(!settings.EnableStereoBlend);

			ImGui::SliderFloat(T(TKEY("stereo_blend_depth_sigma"), "Depth Sigma"), &settings.StereoBlendDepthSigma, 0.001f, 0.1f, "%.4f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s",
					T(TKEY("stereo_blend_depth_sigma_tooltip"),
						"Depth sensitivity for the bilateral weight.\n"
						"Lower values are stricter -- only blend when depths match very closely.\n"
						"Higher values allow blending across slight depth differences.\n"
						"Default: 0.01"));
			}

			ImGui::SliderFloat(T(TKEY("stereo_blend_max_factor"), "Max Blend Factor"), &settings.StereoBlendMaxFactor, 0.0f, 0.5f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s",
					T(TKEY("stereo_blend_max_factor_tooltip"),
						"Maximum blend strength between the two eyes.\n"
						"Higher values reduce screen-space effect flicker but destroy stereo depth.\n"
						"Keep below ~0.15 to preserve 3D parallax.\n"
						"Default: 0.1"));
			}

			ImGui::SliderFloat(T(TKEY("stereo_blend_color_threshold"), "Color Difference Threshold"), &settings.StereoBlendColorThreshold, 0.0f, 0.2f, "%.3f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s",
					T(TKEY("stereo_blend_color_threshold_tooltip"),
						"Minimum luminance difference between eyes to trigger blending.\n"
						"Set to 0 to blend everywhere. Higher = more selective.\n"
						"Default: 0.02"));
			}

			ImGui::EndDisabled();

			ImGui::Separator();

			// Auto-enable required feature when a debug mode is selected; restore on Off.
			// Tracks what we toggled so user-initiated changes aren't clobbered.
			static bool s_weEnabledStereoBlend = false;

			const char* debugModes[] = {
				T(TKEY("reproject_debug_off"), "Off"),
				T(TKEY("reproject_debug_coverage"), "Coverage"),
				T(TKEY("reproject_debug_back_check"), "Back-Check"),
				T(TKEY("reproject_debug_blend_weight"), "Blend Weight"),
				T(TKEY("reproject_debug_edge_detection"), "Edge Detection")
			};
			if (ImGui::Combo(T(TKEY("reproject_debug_view"), "Reprojection Debug View"), &settings.ReprojectDebugMode, debugModes, IM_ARRAYSIZE(debugModes))) {
				int newMode = settings.ReprojectDebugMode;
				bool needsBlend = (newMode >= 2 && newMode <= 4);

				// Auto-enable Stereo Blend for its own debug modes (2-4); Coverage (1) is
				// SSS's/SSGI's own reproject-shader output and needs no Stereo Blend.
				if (needsBlend && !settings.EnableStereoBlend) {
					settings.EnableStereoBlend = true;
					s_weEnabledStereoBlend = true;
				} else if (!needsBlend && s_weEnabledStereoBlend) {
					settings.EnableStereoBlend = false;
					s_weEnabledStereoBlend = false;
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s",
					T(TKEY("reproject_debug_view_tooltip"),
						"Back-Check/Blend Weight/Edge Detection auto-enable Stereo Blend; setting back "
						"to Off restores it.\n\n"
						"Off: Normal rendering\n"
						"Coverage: SSS/SSGI reprojection coverage (black = disoccluded, marched natively)\n"
						"Back-Check: Round-trip reprojection validation\n"
						"Blend Weight: Heatmap of bilateral blend intensity\n"
						"Edge Detection: Highlights depth discontinuities"));
			}
		}

		if (ImGui::CollapsingHeader(T(TKEY("foveated_effects_header"), "Foveation-Following Effects"))) {
			auto& upscaling = globals::features::upscaling;
			auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			const bool foveatedDLSSActive = upscaling.foveatedRender.IsActive();
			const bool ssrEnabled = dynamicCubemaps.loaded && dynamicCubemaps.settings.EnabledSSR;

			if (!foveatedDLSSActive) {
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", T(TKEY("foveated_requires_dlss"), "Requires Foveated DLSS to be active:"));
				ImGui::SameLine();
				if (ImGui::TextLink(T(TKEY("foveated_requires_dlss_link"), "Upscaling settings"))) {
					if (auto* menu = Menu::GetSingleton())
						menu->SelectFeatureMenu(upscaling.GetShortName());
				}
			}
			if (!ssrEnabled) {
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", T(TKEY("foveated_requires_ssr"), "Requires Screen Space Reflections:"));
				ImGui::SameLine();
				if (ImGui::TextLink(T(TKEY("foveated_requires_ssr_link"), "Dynamic Cubemaps settings"))) {
					if (auto* menu = Menu::GetSingleton())
						menu->SelectFeatureMenu(dynamicCubemaps.GetShortName());
				}
			}

			ImGui::BeginDisabled(!foveatedDLSSActive || !ssrEnabled);
			ImGui::Checkbox(T(TKEY("foveated_ssr_raymarching"), "Foveate SSR (follows DLSS region)"), &settings.EnableSSRFoveation);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s",
					T(TKEY("foveated_ssr_raymarching_tooltip"),
						"Reduces screen-space reflection raymarching toward the periphery, using the\n"
						"active Foveated DLSS region. Central reflections stay full quality; peripheral\n"
						"pixels fall back to the cubemap / water reflection. VR only."));
			}

			ImGui::BeginDisabled(!settings.EnableSSRFoveation);
			ImGui::Checkbox(T(TKEY("foveated_ssr_hard_cutoff"), "Hard Cutoff Outside Center##SSRFoveation"), &settings.EnableSSRFoveationHardCutoff);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s",
					T(TKEY("foveated_hard_cutoff_tooltip"),
						"Hard-skip SSR outside the center region instead of a feathered falloff.\n"
						"Cheaper, but the transition edge may be visible. Default off (feathered)."));
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
		}
	}

	void DrawGeneralVRSettings()
	{
		auto& vr = globals::features::vr;
		VR::Settings& settings = vr.settings;
		if (ImGui::CollapsingHeader(T(TKEY("general_settings_header"), "General Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
			bool exteriorChanged = ImGui::Checkbox(T(TKEY("depth_culling_exteriors"), "Enable Depth Buffer Culling in Exteriors"), &settings.EnableDepthBufferCullingExterior);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("depth_culling_exteriors_tooltip"), "Improves performance in exteriors, recommended ON."));
			}

			bool interiorChanged = ImGui::Checkbox(T(TKEY("depth_culling_interiors"), "Enable Depth Buffer Culling in Interiors"), &settings.EnableDepthBufferCullingInterior);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("depth_culling_interiors_tooltip"), "Improves performance in interiors, recommended ON."));
			}

			if (exteriorChanged || interiorChanged) {
				vr.UpdateDepthBufferCulling();
			}

			if (ImGui::SliderFloat(T(TKEY("min_occludee_box_extent"), "Min Occludee Box Extent"), &settings.MinOccludeeBoxExtent, 0.0f, 1000.0f, "%.1f")) {
				if (vr.gMinOccludeeBoxExtent) {
					*vr.gMinOccludeeBoxExtent = settings.MinOccludeeBoxExtent;
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("min_occludee_box_extent_tooltip"), "Minimum bounding box dimensions for object occlusion culling. Lower values improve performance but may result in visual artifacts."));
			}
		}
	}

}  // namespace

//=============================================================================
// DRAW SETTINGS (main entry point)
//=============================================================================

void VR::DrawSettings()
{
	auto menu = globals::menu;
	if (!menu)
		return;
	if (ImGui::BeginTabBar("##VRTabs", ImGuiTabBarFlags_None)) {
		if (BeginTabItemWithFont(T(TKEY("tab_general"), "General"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##VRGeneralFrame", { 0, 0 }, true)) {
				DrawGeneralVRSettings();
				dynamicNearClip.DrawSettings();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (BeginTabItemWithFont(T(TKEY("tab_stereo"), "Stereo"), Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##VRStereoFrame", { 0, 0 }, true)) {
				DrawStereoSettings();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		// Controller bindings — only meaningful when the VR overlay helper owns
		// input. Restores the sortable/filterable bindings table (with rebind)
		// via the helper's client SDK.
		if (IsHelperRegistered()) {
			if (BeginTabItemWithFont(T(TKEY("tab_controls"), "Controls"), Menu::FontRole::Subheading)) {
				if (ImGui::BeginChild("##VRControlsFrame", { 0, 0 }, true)) {
					ImGui::TextWrapped("%s", T(TKEY("controls_help"),
												 "VR controller bindings. Click Rebind, then hold the new button combo."));
					ImGui::Spacing();
					DrawHelperBindingsTable();
				}
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
		}

		ImGui::EndTabBar();
	}
}

// Central Performance hub view: the same stereo + culling controls the Stereo/General
// tabs render, bound to the same settings. Skips the tab bar so the hub can stack this
// feature's sections alongside other features' perf controls.
void VR::DrawPerformanceSettings()
{
	using StereoMode = VRStereoOptimizations::StereoMode;

	// The profile-controlled lever only; depth culling and detailed tuning live in the
	// General/Stereo tabs (no duplicate here).
	bool reproject = stereoOpt.settings.stereoMode != StereoMode::Off;
	if (ImGui::Checkbox(T(TKEY("vr_perf_reproject"), "Stereo Reprojection"), &reproject))
		stereoOpt.settings.stereoMode = reproject ? StereoMode::Enable : StereoMode::Off;
	// stereoMode latches at boot; the shared helper attaches the tooltip and the pending-restart cue.
	Util::UI::RestartGatedAnnotate(stereoOpt.bootSnapshot, stereoOpt.settings, &VRStereoOptimizations::Settings::stereoMode,
		T(TKEY("vr_perf_reproject_tooltip"),
			"Shares eye 0's shading with eye 1 where valid, cutting VR GPU cost. "
			"Detailed tuning is in the Stereo tab."));
}

// Stereo reprojection is the big VR GPU-cost saver with a minor disocclusion artifact,
// so Performance/Balanced enable it and Quality turns it off for maximum fidelity.
// stereoMode is restart-gated (surfaces its pending banner in the reprojection panel).
void VR::ApplyPerformanceProfile(PerfProfile profile)
{
	using StereoMode = VRStereoOptimizations::StereoMode;
	stereoOpt.settings.stereoMode = ProfileEnablesReproject(profile) ? StereoMode::Enable : StereoMode::Off;
}

bool VR::MatchesPerformanceProfile(PerfProfile profile) const
{
	using StereoMode = VRStereoOptimizations::StereoMode;
	return stereoOpt.settings.stereoMode == (ProfileEnablesReproject(profile) ? StereoMode::Enable : StereoMode::Off);
}

#undef I18N_KEY_PREFIX
