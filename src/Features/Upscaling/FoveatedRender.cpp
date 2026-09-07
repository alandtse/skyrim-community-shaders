#include "FoveatedRender.h"

#include "../../Globals.h"
#include "../../I18n/I18n.h"
#include "../../Utils/Subrect.h"
#include "../../Utils/UI.h"
#include "../FoveatedCommon.h"
#include "../Upscaling.h"
#include "FoveatedRender/Core.h"

#include <algorithm>

#define I18N_KEY_PREFIX "feature.upscaling."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FoveatedRender::Settings,
	enabled,
	dlssMode,
	stretchMode,
	peripheryBlurRadius,
	debugVisualize,
	peripheryAAMode,
	peripheryTemporalAlpha,
	subrectBlendMode,
	subrectFeatherWidth,
	subrectDitherStrength);

// ============================================================================
// Lifecycle
// ============================================================================

void FoveatedRender::PostPostLoad()
{
	bootSnapshot.LatchIfNeeded(settings);

	// Opt into the stereo extension so the controller tracks a separate
	// right-eye UV (HMD nose-side overlap symmetry).
	subrectController.SetStereoEnabled(true);

	// Seed sensible foveal presets. Empty-case only — user edits persist.
	// "Center N%" presets are symmetric per eye (no rightUV → auto-mirror, which
	// for centered UVs produces an identical right-eye UV). "Nasal Convergence"
	// is asymmetric: left eye biased toward its right edge, right eye biased
	// toward its left edge — both targeting the nose-side region where HMD
	// binocular fusion is strongest, so DLSS reconstruction lands in the actual
	// stereo overlap zone rather than diverging left/right fields.
	subrectController.SeedDefaultPresets({
		{ .name = kPresetFullEye, .uv = { 0.0f, 0.0f, 1.0f, 1.0f } },
		{ .name = kPresetCenter75, .uv = { 0.125f, 0.125f, 0.75f, 0.75f } },
		{ .name = kPresetCenter50, .uv = { 0.25f, 0.25f, 0.5f, 0.5f } },
		{ .name = kPresetNasalConvergence50,
			.uv = { 0.5f, 0.25f, 0.5f, 0.5f },
			.rightUV = Util::Subrect::UVRegion{ 0.0f, 0.25f, 0.5f, 0.5f } },
	});
	// PostPostLoad runs after settings load, so a user with an older, shorter
	// persisted preset list (from before these names existed) still sees every
	// current preset in the DrawEditor dropdown, not just whichever ones they
	// happened to click as buttons.
	subrectController.MaterializeNewDefaults();
}

void FoveatedRender::ClearShaderCache()
{
	FoveatedRenderImpl::Core::ClearShaderCache();
}

// ============================================================================
// Settings I/O — driven from Upscaling::Save/LoadSettings under a nested key
// ============================================================================

void FoveatedRender::SaveSettings(json& o_json)
{
	o_json = settings;
	subrectController.SaveSettings(o_json);
}

void FoveatedRender::LoadSettings(const json& o_json)
{
	settings = o_json;
	// Util::Subrect::Controller::LoadSettings takes `const json&` (Subrect.h:68)
	// so no const_cast is needed — keeping it would imply mutation that never
	// happens.
	subrectController.LoadSettings(o_json);
	ClampSettings();
}

void FoveatedRender::RestoreDefaultSettings()
{
	settings = {};
	ClampSettings();
}

void FoveatedRender::ClampSettings()
{
	settings.enabled = std::min(settings.enabled, 1u);
	settings.dlssMode = std::min(settings.dlssMode, 1u);
	settings.stretchMode = std::min(settings.stretchMode, 2u);
	settings.debugVisualize = std::min(settings.debugVisualize, 1u);
	settings.peripheryAAMode = std::min(settings.peripheryAAMode, 1u);
	settings.subrectBlendMode = std::min(settings.subrectBlendMode, 2u);
	settings.peripheryBlurRadius = std::clamp(settings.peripheryBlurRadius, 0.5f, 4.0f);
	settings.peripheryTemporalAlpha = std::clamp(settings.peripheryTemporalAlpha, 0.05f, 0.5f);
	settings.subrectFeatherWidth = std::clamp(settings.subrectFeatherWidth, 2.0f, 128.0f);
	settings.subrectDitherStrength = std::clamp(settings.subrectDitherStrength, 0.0f, 2.0f);
	// Preset clamping reads from Upscaling::Settings now.
	auto& sharedPreset = globals::features::upscaling.settings.presetDLSS;
	sharedPreset = std::min(sharedPreset, 5u);
	if (!IsPresetCompatibleWithMode(sharedPreset)) {
		sharedPreset = 3;  // Fall back to L
	}
}

// ============================================================================
// Activation + accessors
// ============================================================================

bool FoveatedRender::IsActive() const
{
	// Gate on DLSS/FSR being the *selected* method, not just available
	// (IsRuntimeSupported): otherwise foveation and its SSR consumer run under
	// TAA/None and the route derefs unallocated upscaler resources — the crash
	// seen under RenderDoc, whose DX12 swapchain disables DLSS.
	if (!enabledAtBoot || !IsRuntimeSupported())
		return false;
	const auto method = globals::features::upscaling.GetUpscaleMethod();
	if (method != Upscaling::UpscaleMethod::kDLSS && method != Upscaling::UpscaleMethod::kFSR)
		return false;

	// A Full Eye region pays the isolation/stretch overhead (snapshot copies, mask
	// clears, StretchDRS) for a subrect equal to the full frame -- no savings, real
	// cost. Skip the route so the standard full-frame path runs instead.
	const bool isFullEye = subrectController.GetUV().IsFullEye() && subrectController.GetRightEyeUV().IsFullEye();
	return !isFullEye;
}

bool FoveatedRender::ShouldForceVisualize() const
{
	using namespace std::chrono_literals;
	return std::chrono::steady_clock::now() - lastDragTime < 3s;
}

bool FoveatedRender::IsRuntimeSupported() const
{
	// FSR's host path has no adapter/runtime prerequisite, so VR alone is
	// sufficient to enable the option -- IsActive() is what actually gates
	// on the *selected* method (kDLSS/kFSR) being one the route supports.
	return globals::game::isVR;
}

FoveatedRender::DlssMode FoveatedRender::GetDlssMode() const
{
	if (globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kFSR)
		return DlssMode::kDefault;
	return (DlssMode)std::min(settings.dlssMode, 1u);
}

FoveatedRender::FoveationProfile FoveatedRender::GetFoveationProfile() const
{
	FoveationProfile profile;
	if (!IsActive())
		return profile;

	const auto& leftUV = subrectController.GetUV();
	const auto& rightUV = subrectController.GetRightEyeUV();

	// Map the rectangular subrect onto the centered superellipse the mask helper expects: vertical
	// extent drives coverageScale (radiusY = coverageScale/2), the rect aspect drives the horizontal
	// stretch (radiusX = coverageScale * hScale/2). The mask carries one scale for both eyes (only
	// the center offset is per-eye), so size comes from the less-foveated (larger) extent of the two:
	// the center is the superset enclosing both eyes' full-quality regions, so neither eye's sharp
	// zone is ever foveated (min would shrink it below an eye's sharp region and foveate it). A
	// full eye therefore yields full coverage and disables foveation (the gate below).
	const float coverageH = std::max(leftUV.h, rightUV.h);
	const float coverageW = std::max(leftUV.w, rightUV.w);
	const float coverageScale = FoveatedCommon::ClampCenterScale(coverageH);

	// Availability keys off the clamped scale: if the larger eye rounds up to full coverage there is
	// nothing to foveate, leave the default (available == false).
	if (!FoveatedCommon::IsActiveCoverage(coverageScale))
		return profile;

	profile.available = true;
	profile.coverageScale = coverageScale;
	profile.centerHorizontalScale = FoveatedCommon::ClampCenterHorizontalScale(
		coverageH > 1e-4f ? coverageW / coverageH : 1.0f);
	profile.centerOffsets[0] = float2{ (leftUV.x + leftUV.w * 0.5f) - 0.5f, (leftUV.y + leftUV.h * 0.5f) - 0.5f };
	profile.centerOffsets[1] = float2{ (rightUV.x + rightUV.w * 0.5f) - 0.5f, (rightUV.y + rightUV.h * 0.5f) - 0.5f };
	return profile;
}

void FoveatedRender::LatchQualityMode()
{
	qualityModeAtBoot = std::clamp(globals::features::upscaling.settings.qualityMode, 1u, 4u);
}

uint FoveatedRender::GetActiveQualityMode() const
{
	return std::clamp(globals::features::upscaling.settings.qualityMode, 1u, 4u);
}

uint FoveatedRender::GetActivePresetDLSS() const
{
	return std::min(globals::features::upscaling.settings.presetDLSS, 5u);
}

float FoveatedRender::GetActiveSharpnessDLSS() const
{
	return std::clamp(globals::features::upscaling.settings.sharpnessDLSS, 0.0f, 1.0f);
}

float FoveatedRender::GetRenderScaleForQuality(uint qualityMode)
{
	return Upscaling::GetQualityModeRatio(qualityMode);
}

bool FoveatedRender::IsPresetCompatibleWithMode(uint presetIndex) const
{
	// Preset indices: 0=Default, 1=J, 2=K, 3=L, 4=M, 5=F
	// Faster mode: J(1) and K(2) are incompatible.
	if (GetDlssMode() == DlssMode::kFaster) {
		return presetIndex != 1 && presetIndex != 2;
	}
	return true;
}

void FoveatedRender::ClampPresetToMode()
{
	auto& sharedPreset = globals::features::upscaling.settings.presetDLSS;
	if (!IsPresetCompatibleWithMode(sharedPreset)) {
		sharedPreset = 3;  // Fall back to L
	}
}

// ============================================================================
// UI — FoveatedRender-specific knobs only. Quality / sharpness / preset /
// Streamline log level live on Upscaling's panel and apply to both DLSS paths.
// Called from Upscaling::DrawSettings inside a TreeNode.
// ============================================================================

void FoveatedRender::DrawEnable()
{
	ClampSettings();

	ImGui::TextWrapped(T(TKEY("foveated_overview"),
		"Foveated subrect upscaling: only the user-selected region gets full DLSS/FSR "
		"upscaling, the periphery is cheaply stretched. Significant upscaler cost reduction "
		"at the cost of peripheral sharpness. VR only."));

	const bool runtimeSupported = IsRuntimeSupported();
	if (!runtimeSupported) {
		settings.enabled = 0;
	}

	if (!runtimeSupported)
		ImGui::BeginDisabled();
	bool enabledBool = settings.enabled != 0;
	if (ImGui::Checkbox(T(TKEY("foveated_enable"), "Enable Foveated Upscaling (region source)"), &enabledBool)) {
		settings.enabled = enabledBool ? 1u : 0u;
		// Full Eye is a deliberate no-op (see IsActive()) -- enabling straight from
		// it would silently do nothing until the user finds the region picker below.
		if (enabledBool && subrectController.GetUV().IsFullEye() && subrectController.GetRightEyeUV().IsFullEye())
			subrectController.ApplyPresetByName(kPresetCenter75);
	}
	if (!runtimeSupported)
		ImGui::EndDisabled();

	Util::UI::DrawSettingDiff(bootSnapshot, settings, &Settings::enabled);

	if (enabledAtBoot) {
		const auto method = globals::features::upscaling.GetUpscaleMethod();
		const bool methodOk = method == Upscaling::UpscaleMethod::kDLSS || method == Upscaling::UpscaleMethod::kFSR;
		if (IsActive())
			Util::Text::WrappedInfo(T(TKEY("foveated_active"), "Active: foveated subrect upscaling is enabled (skipped in menus / on preflight failure)."));
		else if (!methodOk)
			Util::Text::Warning(T(TKEY("foveated_standing_by"), "Standing by: only active while the Upscaling Method is DLSS or FSR. Inactive right now."));
		else
			Util::Text::Warning(T(TKEY("foveated_standing_by_full_eye"), "Standing by: region is Full Eye (no crop) -- shrink it in Subrect Region below to see savings."));
	}

	if (!globals::game::isVR) {
		Util::Text::Warning(T(TKEY("foveated_vr_only"), "VR only -- flat has no equivalent lens-driven periphery quality cliff to exploit."));
	}
}

const char* FoveatedRender::DlssModeName(DlssMode mode)
{
	return mode == DlssMode::kFaster ?
	           T(TKEY("foveated_dlss_mode_faster"), "Faster") :
	           T(TKEY("foveated_dlss_mode_default"), "Default");
}

const char* FoveatedRender::StretchModeName(StretchMode mode)
{
	switch (mode) {
	case StretchMode::kPoint:
		return T(TKEY("foveated_stretch_point"), "Point");
	case StretchMode::kGaussianBlur:
		return T(TKEY("foveated_stretch_gaussian"), "Gaussian Blur");
	default:
		return T(TKEY("foveated_stretch_bilinear"), "Bilinear");
	}
}

const char* FoveatedRender::PeripheryAAModeName(PeripheryAAMode mode)
{
	return mode == PeripheryAAMode::kTemporalSmooth ?
	           T(TKEY("foveated_periphery_aa_temporal"), "Temporal Smooth") :
	           T(TKEY("foveated_periphery_aa_none"), "None");
}

const char* FoveatedRender::SubrectBlendModeName(SubrectBlendMode mode)
{
	switch (mode) {
	case SubrectBlendMode::kFeather:
		return T(TKEY("foveated_blend_feather"), "Feather");
	case SubrectBlendMode::kDither:
		return T(TKEY("foveated_blend_dither"), "Dither");
	default:
		return T(TKEY("foveated_blend_hard_copy"), "Hard Copy");
	}
}

void FoveatedRender::DrawSettings()
{
	ClampSettings();

	Util::Text::WrappedInfo(T(TKEY("foveated_shared_panel_note"), "Quality and Sharpness are on the main Upscaling panel — changes there apply to foveated rendering too. DLSS Preset also applies there when DLSS is the selected upscaler."));

	// ── VR-only knobs ──
	if (globals::game::isVR) {
		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_dlss_mode_header"), "VR DLSS Mode"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_dlss_mode_tooltip"),
								  "Default — highest quality. Each eye gets its own isolated copy of color/depth/motion\n"
								  "vectors so DLSS can't sample across the stereo midline. 5 copies per eye per frame.\n"
								  "All DLSS presets supported. Best for screenshots or when Faster shows edge artifacts.\n"
								  "\n"
								  "Faster — lower overhead. DLSS reads directly from the frame buffer using a viewport\n"
								  "offset instead of isolating each eye. 1 snapshot + 2 mask clears per frame.\n"
								  "DLSS may sample 1-2 pixels from the neighboring eye near the stereo center — usually\n"
								  "invisible in motion. Presets J and K are incompatible and auto-clamp to L."));
		}

		const bool isFSR = globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kFSR;
		if (isFSR)
			ImGui::BeginDisabled();
		uint prevMode = settings.dlssMode;
		ImGui::SliderInt(T(TKEY("foveated_dlss_mode_label"), "DLSS Mode"), reinterpret_cast<int*>(&settings.dlssMode), 0, 1, DlssModeName((DlssMode)std::min(settings.dlssMode, 1u)));
		if (settings.dlssMode != prevMode) {
			const uint prevPreset = globals::features::upscaling.settings.presetDLSS;
			ClampPresetToMode();
			if (globals::features::upscaling.settings.presetDLSS != prevPreset) {
				logger::info("[FOVEATED] DLSS preset clamped from {} to {} after mode switch (J/K incompatible with Faster)",
					prevPreset, globals::features::upscaling.settings.presetDLSS);
			}
		}
		if (isFSR) {
			ImGui::EndDisabled();
			ImGui::TextWrapped(T(TKEY("foveated_dlss_mode_fsr_desc"), "Not used by FSR -- applies only when DLSS is the selected upscaler."));
		} else {
			switch (GetDlssMode()) {
			case DlssMode::kDefault:
				ImGui::TextWrapped(T(TKEY("foveated_dlss_mode_default_desc"), "Per-eye isolation: 5 copies per frame, 2 DLSS evaluates. All presets."));
				break;
			case DlssMode::kFaster:
				ImGui::TextWrapped(T(TKEY("foveated_dlss_mode_faster_desc"), "Viewport offset: 1 snapshot, 2 mask clears, 2 DLSS evaluates. Presets J/K unavailable."));
				break;
			default:
				break;
			}
		}

		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_periphery_header"), "Periphery Rendering"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_periphery_tooltip"),
								  "The area outside your selected subrect is filled cheaply rather than running\n"
								  "the selected upscaler. These settings control how that cheap fill looks and\n"
								  "whether it flickers.\n"
								  "\n"
								  "Stretch method: how pixels outside the subrect are reconstructed from the lower-res\n"
								  "render buffer. Does not affect the upscaled subrect region at all.\n"
								  "\n"
								  "Periphery AA: reduces temporal flicker in the stretched area using motion-compensated\n"
								  "history blending. Independent of the upscaled subrect.\n"
								  "\n"
								  "Edge Blend: controls how the upscaled subrect edge meets the stretched periphery.\n"
								  "Hard Copy leaves a sharp seam; Feather/Dither soften it. Only affects the boundary."));
		}

		ImGui::SliderInt(T(TKEY("foveated_stretch_label"), "Stretch"), reinterpret_cast<int*>(&settings.stretchMode), 0, 2, StretchModeName((StretchMode)settings.stretchMode));
		switch (GetStretchMode()) {
		case StretchMode::kBilinear:
			ImGui::TextWrapped(T(TKEY("foveated_stretch_bilinear_desc"), "Bilinear: smooth upscale of the render buffer. Looks soft but clean."));
			break;
		case StretchMode::kPoint:
			ImGui::TextWrapped(T(TKEY("foveated_stretch_point_desc"), "Point: cheapest, visibly pixelated. Good for benchmarking foveated savings."));
			break;
		case StretchMode::kGaussianBlur:
			ImGui::TextWrapped(T(TKEY("foveated_stretch_gaussian_desc"), "Gaussian: blurs the periphery further into soft focus. Good default for foveated use."));
			ImGui::SliderFloat(T(TKEY("foveated_blur_radius"), "Blur Radius"), &settings.peripheryBlurRadius, 0.5f, 4.0f, "%.1f px");
			break;
		}

		ImGui::SliderInt(T(TKEY("foveated_periphery_aa_label"), "Periphery AA"), reinterpret_cast<int*>(&settings.peripheryAAMode), 0, 1, PeripheryAAModeName((PeripheryAAMode)settings.peripheryAAMode));
		if (GetPeripheryAAMode() == PeripheryAAMode::kTemporalSmooth) {
			ImGui::TextWrapped(T(TKEY("foveated_periphery_aa_temporal_desc"), "Blends the stretched periphery with motion-reprojected history to reduce flicker."));
			ImGui::SliderFloat(T(TKEY("foveated_smoothing"), "Smoothing"), &settings.peripheryTemporalAlpha, 0.05f, 0.5f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("foveated_smoothing_tooltip"), "Lower = more temporal history (smoother but may ghost). Higher = more responsive."));
			}
		}

		ImGui::SliderInt(T(TKEY("foveated_edge_blend_label"), "Edge Blend"), reinterpret_cast<int*>(&settings.subrectBlendMode), 0, 2, SubrectBlendModeName((SubrectBlendMode)std::min(settings.subrectBlendMode, 2u)));
		switch (GetSubrectBlendMode()) {
		case SubrectBlendMode::kHardCopy:
			ImGui::TextWrapped(T(TKEY("foveated_blend_hard_copy_desc"), "Sharp seam at the subrect boundary. Lowest cost."));
			break;
		case SubrectBlendMode::kFeather:
			ImGui::TextWrapped(T(TKEY("foveated_blend_feather_desc"), "Smoothstep fade over N pixels at the boundary. Hides the seam."));
			ImGui::SliderFloat(T(TKEY("foveated_feather_width"), "Feather Width"), &settings.subrectFeatherWidth, 2.0f, 128.0f, "%.0f px");
			break;
		case SubrectBlendMode::kDither:
			ImGui::TextWrapped(T(TKEY("foveated_blend_dither_desc"), "Noise-dithered fade — more natural-looking than feather at large subrects."));
			ImGui::SliderFloat(T(TKEY("foveated_band_width"), "Band Width"), &settings.subrectFeatherWidth, 2.0f, 128.0f, "%.0f px");
			ImGui::SliderFloat(T(TKEY("foveated_noise_amount"), "Noise Amount"), &settings.subrectDitherStrength, 0.0f, 2.0f, "%.2f");
			break;
		}

		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_subrect_region_header"), "Subrect Region"));
		ImGui::TextWrapped(T(TKEY("foveated_subrect_region_desc"),
			"Drag in the preview below to select the region that gets full upscaling. "
			"The rest is cheaply stretched — saves significant upscaling cost."));
		Util::Text::WrappedInfo(T(TKEY("foveated_screenshot_subrect_note"), "Screenshot has its own subrect; align them only if you want pixel-matched captures."));

		bool debugBool = settings.debugVisualize != 0;
		if (ImGui::Checkbox(T(TKEY("foveated_visualize_regions"), "Visualize regions"), &debugBool))
			settings.debugVisualize = debugBool ? 1u : 0u;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_visualize_regions_tooltip"),
								  "Diagnostic: tint the cheap-stretched periphery red so the upscaled\n"
								  "subrect (un-tinted) pops visually in-game. Lets you confirm at a glance where\n"
								  "the selected upscaler is actually running vs where the cheap stretch is filling.\n"
								  "No perf impact; runtime toggle, no restart needed. Also shows briefly whenever\n"
								  "you drag-resize the region below, even with this off."));
		}

		// Preview off kVR_FRAMEBUFFER (the final composed SBS image the headset
		// sees) rather than kMAIN. kMAIN is mid-pipeline and carries non-1
		// alpha where Skyrim composited UI plates, so even with the opaque
		// blend callback you see the menu mask outline instead of the rendered
		// world. ScreenshotFeature picks the same RT for the same reason
		// (ScreenshotFeature.cpp:243). Foveated is VR-only so kVR_FRAMEBUFFER
		// is always populated when we get here.
		auto renderer = globals::game::renderer;
		if (renderer) {
			auto& fb = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kVR_FRAMEBUFFER];
			auto* tex = static_cast<ID3D11Texture2D*>(fb.texture);
			subrectController.DrawEditor(fb.SRV, tex, 0.5f, 0.0f, Util::Subrect::OpaquePreviewBlendCallback);
		} else {
			subrectController.DrawEditor(nullptr, nullptr, 0.5f);
		}

		if (subrectController.IsDragging())
			lastDragTime = std::chrono::steady_clock::now();
	}
}

#undef I18N_KEY_PREFIX
