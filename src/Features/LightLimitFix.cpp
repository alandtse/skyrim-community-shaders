#include "LightLimitFix.h"
#if defined(ENABLE_EFFECTS11)
#	include "Features/Effects11.h"
#endif
#include "Features/InverseSquareLighting/Common.h"
#include "Features/LightLimitFix/SettingsSanitize.h"
#include "Features/LightLimitFix/ShadowCasterMath.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "InverseSquareLighting.h"
#include "LinearLighting.h"
#include "Menu/PerformanceRenderer.h"
#include "Profiler.h"
#include "Utils/UI.h"
#include <bit>

#include "Deferred.h"
#include "Menu/ThemeManager.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/ExternalEmittance.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace
{
	constexpr float kParticleLightsSaturationMin = 1.0f;
	constexpr float kParticleLightsSaturationMax = 2.0f;
	constexpr float kParticleBrightnessMin = 0.0f;
	constexpr float kParticleBrightnessMax = 10.0f;
	constexpr float kParticleRadiusMin = 0.0f;
	constexpr float kParticleRadiusMax = 10.0f;
	constexpr float kBillboardBrightnessMin = 0.0f;
	constexpr float kBillboardBrightnessMax = 10.0f;
	constexpr float kBillboardRadiusMin = 0.0f;
	constexpr float kBillboardRadiusMax = 10.0f;
	constexpr float kParticleClusterThresholdMin = 8.0f;
	constexpr float kParticleClusterThresholdMax = 128.0f;
	constexpr int kMaxParticlesPerEmitterMin = 32;
	constexpr int kMaxParticlesPerEmitterMax = 2048;
	constexpr float kMaxParticleDistanceMin = 0.0f;
	constexpr float kMaxParticleDistanceMax = 20000.0f;
	constexpr float kJsonPlacedLightIntensityMin = 0.0f;
	constexpr float kJsonPlacedLightIntensityMax = 8.0f;

	float ClampFiniteOrDefault(float a_value, float a_min, float a_max, float a_default)
	{
		if (!std::isfinite(a_value))
			return a_default;
		return std::clamp(a_value, a_min, a_max);
	}

	void SanitizeSettings(LightLimitFix::Settings& a_settings)
	{
		a_settings.ParticleLightsSaturation =
			ClampFiniteOrDefault(a_settings.ParticleLightsSaturation, kParticleLightsSaturationMin, kParticleLightsSaturationMax, 1.0f);
		a_settings.ParticleBrightness =
			ClampFiniteOrDefault(a_settings.ParticleBrightness, kParticleBrightnessMin, kParticleBrightnessMax, 1.0f);
		a_settings.ParticleRadius =
			ClampFiniteOrDefault(a_settings.ParticleRadius, kParticleRadiusMin, kParticleRadiusMax, 1.0f);
		a_settings.BillboardBrightness =
			ClampFiniteOrDefault(a_settings.BillboardBrightness, kBillboardBrightnessMin, kBillboardBrightnessMax, 1.0f);
		a_settings.BillboardRadius =
			ClampFiniteOrDefault(a_settings.BillboardRadius, kBillboardRadiusMin, kBillboardRadiusMax, 1.0f);
		a_settings.ParticleClusterThreshold =
			ClampFiniteOrDefault(a_settings.ParticleClusterThreshold, kParticleClusterThresholdMin, kParticleClusterThresholdMax, 32.0f);
		a_settings.MaxParticlesPerEmitter = std::clamp(a_settings.MaxParticlesPerEmitter, kMaxParticlesPerEmitterMin, kMaxParticlesPerEmitterMax);
		a_settings.MaxParticleDistance =
			ClampFiniteOrDefault(a_settings.MaxParticleDistance, kMaxParticleDistanceMin, kMaxParticleDistanceMax, 6000.0f);
		a_settings.JsonPlacedLightIntensity =
			ClampFiniteOrDefault(a_settings.JsonPlacedLightIntensity, kJsonPlacedLightIntensityMin, kJsonPlacedLightIntensityMax, 1.0f);
	}

	void ClearStrictLightData(LightLimitFix::StrictLightDataCB& a_data, bool a_resetRoomIndex) noexcept
	{
		a_data.NumStrictLights = 0;
		a_data.ShadowBitMask = 0;
		a_data.FirstPerson = 0;
		a_data.WorldEyePosition = float4{};
		if (a_resetRoomIndex)
			a_data.RoomIndex = -1;
	}

	void SetPointLightTypeFlags(LightLimitFix::LightData& a_light, RE::BSLight* a_bsLight)
	{
		PointLightFlags::SetPointLightTypeFlags(a_light.lightFlags, a_bsLight);
	}

	// Tints a hovered/highlighted debug-table row's light magenta in-world so
	// the user can see which light a row corresponds to in 3D.
	void ApplyLightDebugOverrides(LightLimitFix::LightData& a_light, const void* a_lightPtr)
	{
		const auto key = reinterpret_cast<uintptr_t>(a_lightPtr);
		auto hoverKey = ShadowCasterManager::GetHoveredLight();
		if (hoverKey != 0 && key == hoverKey) {
			float t = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.2831853f);
			a_light.color = float3{ 1.0f, 0.0f, 1.0f };  // magenta
			a_light.fade = 4.0f + t * 4.0f;              // pulsed intensity
		} else if (ShadowCasterManager::IsHighlighted(key)) {
			// Steady magenta on every light in the selected highlight group
			// (populated by the table's group-button hover), distinct from
			// the single pulsing hover light.
			a_light.color = float3{ 1.0f, 0.0f, 1.0f };
		}
	}
}

// Debug visualisation state (EnableLightsVisualisation / LightsVisualisationMode)
// is intentionally NOT serialized -- it lives as instance members on the
// LightLimitFix class so it resets per session and can't accidentally end up in
// a shipped JSON config that would force every load to compile the heavier
// LLFDEBUG shader permutation.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LightLimitFix::Settings,
	EnableContactShadows,
	ContactShadowMaxSteps,
	ContactShadowMaxDistance,
	ContactShadowStride,
	ContactShadowThickness,
	ContactShadowDepthFade,
	ContactShadowMinIntensity,
	ShowShadowOverlay,
	ShadowSettings,
	EnableParticleContactShadows,
	EnableParticleLights,
	EnableParticleLightsCulling,
	EnableParticleLightsDetection,
	EnableParticleLightsOptimization,
	ParticleLightsSaturation,
	ParticleBrightness,
	ParticleRadius,
	BillboardBrightness,
	BillboardRadius,
	ParticleClusterThreshold,
	MaxParticlesPerEmitter,
	MaxParticleDistance,
	JsonPlacedLightIntensity,
	JsonPlacedLightsInteriorsOnly,
	JsonPlacedLightsPortalStrictOnly)

void LightLimitFix::DrawPerformanceSettings()
{
	if (!settings.ShadowSettings.Enabled)
		return;
	ShadowCasterManager::DrawImpactCullSliders(settings.ShadowSettings);
}

void LightLimitFix::DrawPerformancePresets()
{
	// Indented link, not a SeparatorText header, so this reads as nested under Light
	// Limit Fix rather than a sibling section; the hub's own row above (not here) applies presets.
	PerformanceRenderer::DrawSubsectionLink(T("feature.light_limit_fix.shadow_limit_fix_header", "Shadow Limit Fix"), this, "ShadowLimitFix");
	if (!settings.ShadowSettings.Enabled)
		ImGui::TextDisabled("%s", T("feature.light_limit_fix.shadow_limit_fix_disabled_hub",
									  "Shadow Limit Fix is disabled in this feature's settings."));
	ImGui::Unindent();
}

namespace
{
	struct ImpactCullPreset
	{
		float floor;
		float cull;
	};

	// Same three tiers DrawImpactCullControls' own preset buttons apply (Quality: no
	// culling; Balanced/Performance: progressively stronger impact floor + angular cull).
	// Single source of truth for Apply/MatchesPerformanceProfile below.
	constexpr ImpactCullPreset GetImpactCullPreset(Feature::PerfProfile profile)
	{
		switch (profile) {
		case Feature::PerfProfile::Performance:
			return { 0.025f, 0.012f };
		case Feature::PerfProfile::Balanced:
			return { 0.001f, 0.008f };
		default:
			return { 0.0f, 0.0f };
		}
	}
}

void LightLimitFix::ApplyPerformanceProfile(PerfProfile profile)
{
	const auto preset = GetImpactCullPreset(profile);
	settings.ShadowSettings.ShadowImpactFloor = preset.floor;
	settings.ShadowSettings.CasterCullAngularMin = preset.cull;
}

bool LightLimitFix::MatchesPerformanceProfile(PerfProfile profile) const
{
	// Shadow Limit Fix off: these values have no effect, so don't veto the hub's
	// active-profile detection for a knob the user can't currently apply.
	if (!settings.ShadowSettings.Enabled)
		return true;
	const auto preset = GetImpactCullPreset(profile);
	// Epsilon compare, not ==: a JSON save/load round-trip through float can
	// perturb the last bit, which would otherwise permanently read as Custom.
	constexpr float kEpsilon = 1e-4f;
	return std::abs(settings.ShadowSettings.ShadowImpactFloor - preset.floor) <= kEpsilon &&
	       std::abs(settings.ShadowSettings.CasterCullAngularMin - preset.cull) <= kEpsilon;
}

void LightLimitFix::DrawSettings()
{
	auto shaderCache = globals::shaderCache;

	ShadowCasterManager::DrawSettings(settings.ShadowSettings);

	if (ImGui::TreeNodeEx(T("feature.light_limit_fix.statistics", "Statistics"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text(std::vformat(T("feature.light_limit_fix.stat_clustered_light_count", "Clustered Light Count : {}"), std::make_format_args(lightCount)).c_str());
		auto particleLightCountValue = particleLightCount.load(std::memory_order_relaxed);
		ImGui::Text(std::vformat(T("feature.light_limit_fix.stat_particle_lights_count", "Particle Lights Count : {}"), std::make_format_args(particleLightCountValue)).c_str());
		ImGui::TreePop();
	}

	// ---- Active Shadow Casters --------------------------------------
	// One cohesive section: overlay toggle, then ALL the stats grouped
	// together (summary + scheduler stats + budget verdict), then the
	// table below. Same layout as the overlay so testers see the same
	// thing in both views with the stats above the (potentially long)
	// table -- no scrolling required to find the headline numbers.
	ImGui::SeparatorText(T("feature.light_limit_fix.shadow_limit_fix_active_casters", "Shadow Limit Fix -- Active Casters"));

	ImGui::Checkbox(T("feature.light_limit_fix.show_shadow_overlay", "Show Shadow Overlay"), &settings.ShowShadowOverlay);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s",
			T("feature.light_limit_fix.show_shadow_overlay_tooltip",
				"Pop out an always-visible overlay window with the shadow caster table.\n"
				"Without this, the overlay only appears when a light is suppressed\n"
				"or a visualisation mode is active. Enable to access the table's\n"
				"debug controls (cycle button, solo, hover pulse) any time."));
	}

	ShadowCasterManager::DrawShadowSummary(lightCount, MAX_LIGHTS, shadowUnshadowedLightCount);
	ShadowCasterManager::DrawShadowSchedulerStats();
	ImGui::Separator();
	ShadowCasterManager::DrawShadowLightTable(true, false);

	///////////////////////////////
	ImGui::SeparatorText(T("feature.light_limit_fix.contact_shadows_header", "Contact Shadows"));

	ImGui::Checkbox(T("feature.light_limit_fix.enable_contact_shadows", "Enable Contact Shadows"), &settings.EnableContactShadows);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.light_limit_fix.enable_contact_shadows_tooltip", "All point lights (strict and clustered, except simple lights) cast short screen-space shadows. Performance impact."));
	}

	if (settings.EnableContactShadows && ImGui::TreeNode(T("feature.light_limit_fix.contact_shadow_tuning", "Contact Shadow Tuning"))) {
		// SliderScalar with ImGuiDataType_U32 instead of `SliderInt + (int*)cast`:
		// the cast violates strict aliasing (UB) and would also misinterpret any
		// transient negative value inside ImGui before clamp. SliderScalar
		// reads/writes the uint storage directly with explicit min/max bounds.
		constexpr uint32_t kMinSteps = 1, kMaxSteps = 16;
		ImGui::SliderScalar(T("feature.light_limit_fix.contact_shadow_max_steps", "Max Steps"), ImGuiDataType_U32, &settings.ContactShadowMaxSteps,
			&kMinSteps, &kMaxSteps, "%u", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.contact_shadow_max_steps_tooltip", "Raymarch steps at zero depth. Higher = longer / more accurate contact shadows, linearly more cost.\nVR users should consider 2 to halve per-eye cost."));
		}

		// AlwaysClamp on every float slider too: without it, Ctrl+Click text entry can
		// land arbitrary out-of-range values in settings before GetCommonBufferData's
		// boundary clamp catches them at the GPU side.
		ImGui::SliderFloat(T("feature.light_limit_fix.contact_shadow_max_distance", "Max Distance"), &settings.ContactShadowMaxDistance, 64.0f, 4096.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.contact_shadow_max_distance_tooltip", "View-space depth at which contact shadows fade to zero steps. Avoids paying for shadows on distant surfaces where they don't read."));
		}

		ImGui::SliderFloat(T("feature.light_limit_fix.contact_shadow_stride", "Stride"), &settings.ContactShadowStride, 0.5f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.contact_shadow_stride_tooltip", "Per-step march length in view-space units at near depth (auto-scales linearly past ~100 units so far surfaces don't undersample). Larger = longer screen-space reach with coarser detail."));
		}

		ImGui::SliderFloat(T("feature.light_limit_fix.contact_shadow_thickness", "Thickness"), &settings.ContactShadowThickness, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.contact_shadow_thickness_tooltip", "Depth-delta multiplier for shadow onset. Larger = darker contact at occluder edges."));
		}

		ImGui::SliderFloat(T("feature.light_limit_fix.contact_shadow_depth_fade", "Depth Fade"), &settings.ContactShadowDepthFade, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.contact_shadow_depth_fade_tooltip", "Depth-delta multiplier for shadow falloff. Larger = shadows truncate sooner behind thick occluders."));
		}

		ImGui::SliderFloat(T("feature.light_limit_fix.contact_shadow_min_intensity", "Min Light Intensity"), &settings.ContactShadowMinIntensity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s",
				T("feature.light_limit_fix.contact_shadow_min_intensity_tooltip",
					"Skip contact shadows for CLUSTERED lights whose normalized distance falloff "
					"`1 - (lightDist/radius)^2` at the pixel is below this threshold. "
					"Strict lights are always raymarched regardless of this threshold. "
					"Higher = larger perf win, may drop subtle shadows from weak lights at their reach edge."));
		}

		ImGui::TreePop();
	}

	ImGui::BeginDisabled(!settings.EnableContactShadows);
	ImGui::Checkbox(T("feature.light_limit_fix.enable_particle_contact_shadows", "Enable Particle Contact Shadows"), &settings.EnableParticleContactShadows);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.light_limit_fix.enable_particle_contact_shadows_tooltip", "Also cast contact shadows from particle lights. Larger performance impact in fire/magic-heavy scenes."));
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText(T("feature.light_limit_fix.particle_lights_header", "Particle Lights"));

	ImGui::TextWrapped("%s",
		T("feature.light_limit_fix.particle_lights_intro",
			"Turns configured particle effects (candles, braziers, torches, magic) into dynamic lights. "
			"Requires a particle-light config pack shipping Data\\ParticleLights\\*.ini (e.g. Embers HD, "
			"Lanterns of Skyrim); with no pack installed this section has no effect."));
	ImGui::TextWrapped("%s",
		T("feature.light_limit_fix.particle_lights_additive_note",
			"Particle lights are additive emitters and do NOT cast shadow-map shadows, so they never appear "
			"in the shadow caster table above. Turn on \"Enable Particle Contact Shadows\" in the Contact "
			"Shadows section for short screen-space contact shadows."));
	ImGui::Spacing();

	ImGui::Checkbox(T("feature.light_limit_fix.enable_particle_lights", "Enable Particle Lights"), &settings.EnableParticleLights);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.light_limit_fix.enable_particle_lights_tooltip", "Master toggle for the particle-light feature."));
	}

	if (ImGui::TreeNode(T("feature.light_limit_fix.particle_performance", "Performance##particles"))) {
		ImGui::Checkbox(T("feature.light_limit_fix.enable_particle_culling", "Enable Culling"), &settings.EnableParticleLightsCulling);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.enable_particle_culling_tooltip", "Significantly improves performance by not rendering empty textures. Only disable if you are encountering issues."));
		}

		ImGui::Checkbox(T("feature.light_limit_fix.enable_particle_detection", "Enable Detection"), &settings.EnableParticleLightsDetection);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.enable_particle_detection_tooltip", "Adds particle lights to the player light level so that NPCs detect them for stealth and gameplay."));
		}

		ImGui::Checkbox(T("feature.light_limit_fix.enable_particle_optimization", "Enable Optimization"), &settings.EnableParticleLightsOptimization);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.enable_particle_optimization_tooltip", "Merges vertices which are close enough to each other to improve performance."));
		}

		ImGui::SliderFloat(T("feature.light_limit_fix.particle_cluster_threshold", "Cluster Threshold"), &settings.ParticleClusterThreshold, kParticleClusterThresholdMin, kParticleClusterThresholdMax, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s",
				T("feature.light_limit_fix.particle_cluster_threshold_tooltip",
					"Distance+radius similarity threshold for merging particles into one light.\n"
					"Higher = more merging, better performance, blurrier lights.\n"
					"Lower = less merging, more precise, more expensive."));
		}

		ImGui::SliderInt(T("feature.light_limit_fix.max_particles_per_emitter", "Max Particles per Emitter"), &settings.MaxParticlesPerEmitter, kMaxParticlesPerEmitterMin, kMaxParticlesPerEmitterMax);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s",
				T("feature.light_limit_fix.max_particles_per_emitter_tooltip",
					"Maximum number of particles sampled per emitter per frame.\n"
					"Higher = closer to the real particle system but more CPU work.\n"
					"Lower = faster, especially for very dense effects."));
		}

		ImGui::SliderFloat(T("feature.light_limit_fix.max_particle_distance", "Max Particle Distance"), &settings.MaxParticleDistance, 1000.0f, kMaxParticleDistanceMax, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s",
				T("feature.light_limit_fix.max_particle_distance_tooltip",
					"Particle lights beyond this distance from the camera are skipped entirely.\n"
					"Lower = better performance, but distant effects won't contribute light.\n"
					"Higher = more distant particle lighting, but more cost."));
		}

		ImGui::TreePop();
	}

	if (ImGui::TreeNode(T("feature.light_limit_fix.particle_appearance", "Appearance##particles"))) {
		ImGui::SliderFloat(T("feature.light_limit_fix.particle_saturation", "Saturation"), &settings.ParticleLightsSaturation, kParticleLightsSaturationMin, kParticleLightsSaturationMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.particle_saturation_tooltip", "Color saturation of particle/billboard lights. 1.0 = source color; higher = more vivid."));
		}
		ImGui::SliderFloat(T("feature.light_limit_fix.particle_brightness", "Particle Brightness"), &settings.ParticleBrightness, kParticleBrightnessMin, kParticleBrightnessMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.particle_brightness_tooltip", "Intensity multiplier for particle-system emitters (fire, sparks, magic)."));
		}
		ImGui::SliderFloat(T("feature.light_limit_fix.particle_radius", "Particle Radius"), &settings.ParticleRadius, kParticleRadiusMin, kParticleRadiusMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.particle_radius_tooltip", "Radius multiplier for particle-system emitters. Larger = light reaches further."));
		}
		ImGui::SliderFloat(T("feature.light_limit_fix.billboard_brightness", "Billboard Brightness"), &settings.BillboardBrightness, kBillboardBrightnessMin, kBillboardBrightnessMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.billboard_brightness_tooltip", "Intensity multiplier for billboard (single-quad) emitters such as candle flames."));
		}
		ImGui::SliderFloat(T("feature.light_limit_fix.billboard_radius", "Billboard Radius"), &settings.BillboardRadius, kBillboardRadiusMin, kBillboardRadiusMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.billboard_radius_tooltip", "Radius multiplier for billboard emitters. Larger = light reaches further."));
		}

		ImGui::TreePop();
	}

	ImGui::SeparatorText(T("feature.light_limit_fix.placed_lights_json_header", "Placed Lights (JSON)"));

	ImGui::TextWrapped("%s",
		T("feature.light_limit_fix.placed_lights_json_intro",
			"Scales the intensity of runtime lights attached from Light records by Light Placer-style mods. "
			"Separate from particle lights; requires Inverse Square Lighting for the runtime metadata."));
	ImGui::Spacing();

	{
		const bool jsonPlacedLightsSupported = globals::features::inverseSquareLighting.loaded;
		ImGui::BeginDisabled(!jsonPlacedLightsSupported);
		ImGui::SliderFloat(T("feature.light_limit_fix.json_intensity_scale", "Intensity Scale"), &settings.JsonPlacedLightIntensity, kJsonPlacedLightIntensityMin, kJsonPlacedLightIntensityMax, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s",
				T("feature.light_limit_fix.json_intensity_scale_tooltip",
					"Scales intensity for attached runtime lights generated from Light records.\n"
					"Primarily targets Light Placer-style JSON lights.\n"
					"Requires Inverse Square Lighting runtime metadata."));
		}

		ImGui::Checkbox(T("feature.light_limit_fix.json_interiors_only", "Interiors Only"), &settings.JsonPlacedLightsInteriorsOnly);
		ImGui::Checkbox(T("feature.light_limit_fix.json_portal_strict_only", "Portal Strict Only"), &settings.JsonPlacedLightsPortalStrictOnly);
		ImGui::EndDisabled();

		if (!jsonPlacedLightsSupported)
			ImGui::TextDisabled("%s", T("feature.light_limit_fix.json_requires_isl", "Requires Inverse Square Lighting to identify JSON-placed runtime lights."));
	}

	///////////////////////////////
	ImGui::SeparatorText(T("feature.light_limit_fix.debug", "Debug"));

	ImGui::Checkbox(T("feature.light_limit_fix.shadow_demand_instrumentation", "Shadow Demand Instrumentation"), &ShadowDemandInstrumentation);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.light_limit_fix.shadow_demand_instrumentation_tooltip",
							  "Diagnostic only: logs a per-slot shadow-demand distribution every ~300 frames.\n"
							  "The measurement itself always runs while \"Prioritize Redraws by Screen\n"
							  "Demand\" (Performance settings) is on, with or without this log.\n"));
	}

	if (ImGui::TreeNode(T("feature.light_limit_fix.light_limit_vis", "Light Limit Visualization"))) {
		ImGui::Checkbox(T("feature.light_limit_fix.enable_lights_vis", "Enable Lights Visualisation"), &EnableLightsVisualisation);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("feature.light_limit_fix.enable_lights_vis_tooltip", "Enables visualization of the light limit\n"));
		}

		{
			const char* comboOptions[] = {
				T("feature.light_limit_fix.lights_vis_mode_opt_light_limit", "Light Limit"),
				T("feature.light_limit_fix.lights_vis_mode_opt_strict_lights_count", "Strict Lights Count"),
				T("feature.light_limit_fix.lights_vis_mode_opt_clustered_lights_count", "Clustered Lights Count"),
				T("feature.light_limit_fix.lights_vis_mode_opt_shadow_mask", "Shadow Mask"),
				T("feature.light_limit_fix.lights_vis_mode_opt_shadow_light_count", "Shadow Light Count"),
				T("feature.light_limit_fix.lights_vis_mode_opt_point_light_shadow_factor", "Point Light Shadow Factor"),
				T("feature.light_limit_fix.lights_vis_mode_opt_unshadowed_point_lights", "Unshadowed Point Lights"),
				T("feature.light_limit_fix.lights_vis_mode_opt_shadow_caster_density", "Shadow Caster Density"),
				T("feature.light_limit_fix.lights_vis_mode_opt_shadow_slot_index_color", "Shadow Slot Index Color"),
				T("feature.light_limit_fix.lights_vis_mode_opt_light_type_visualization", "Light Type Visualization"),
			};
			// Round-trip through int instead of `(int*)&uint` to avoid strict-aliasing UB
			// (ImGui has no ComboScalar). Clamp on the way in defends against any stale
			// persisted value that might still exist from older builds.
			int visMode = std::clamp(static_cast<int>(LightsVisualisationMode),
				0, IM_ARRAYSIZE(comboOptions) - 1);
			ImGui::Combo(T("feature.light_limit_fix.lights_vis_mode", "Lights Visualisation Mode"), &visMode, comboOptions, IM_ARRAYSIZE(comboOptions));
			LightsVisualisationMode = static_cast<uint>(visMode);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"%s",
					T("feature.light_limit_fix.lights_vis_mode_tooltip",
						"Light Limit: Red when the strict light limit is reached (>=7 portal-strict lights).\n"
						"\n"
						"Strict Lights Count: Heatmap of portal-strict lights per pixel (blue=0, red=15).\n"
						"\n"
						"Clustered Lights Count: Heatmap of dynamic lights in each screen tile (blue=0, red=128)."));
				ShadowCasterManager::DrawVisualisationTooltipShadowModes();
			}
		}

		currentEnableLightsVisualisation = EnableLightsVisualisation;
		if (previousEnableLightsVisualisation != currentEnableLightsVisualisation) {
			globals::state->SetDefines(EnableLightsVisualisation ? "LLFDEBUG" : "");
			shaderCache->Clear(RE::BSShader::Type::Lighting);
			previousEnableLightsVisualisation = currentEnableLightsVisualisation;
		}

		ImGui::TreePop();
	}
}

LightLimitFix::PerFrame LightLimitFix::GetCommonBufferData()
{
	// Defensive sanitization before the values hit the constant buffer. The
	// sliders enforce ImGuiSliderFlags_AlwaysClamp at the UI, but Settings
	// can be mutated through other paths (JSON persistence, mod overrides,
	// remote-control / MCP server, or just an internal logic bug) -- a few
	// of these fields will produce divisions, infinite loops, or visual
	// corruption if they arrive non-finite or out-of-range, so we re-validate
	// at the shader boundary rather than trusting upstream callers.
	//
	// std::clamp passes NaN through unchanged (every NaN comparison is false),
	// so reject non-finite values explicitly first; fall back to the lower
	// bound on NaN/inf to produce degraded but stable behavior.
	auto sanitizeFloat = [](float v, float lo, float hi) {
		return LightLimitFixSanitize::SanitizeFloat(v, lo, hi);
	};

	PerFrame perFrame{};
	perFrame.EnableContactShadows = settings.EnableContactShadows;
	perFrame.ContactShadowMaxSteps = std::clamp<uint32_t>(settings.ContactShadowMaxSteps, 1u, 16u);
	perFrame.ContactShadowMaxDistance = sanitizeFloat(settings.ContactShadowMaxDistance, 64.0f, 4096.0f);
	perFrame.ContactShadowStride = sanitizeFloat(settings.ContactShadowStride, 0.5f, 8.0f);
	perFrame.ContactShadowThickness = sanitizeFloat(settings.ContactShadowThickness, 0.0f, 1.0f);
	perFrame.ContactShadowDepthFade = sanitizeFloat(settings.ContactShadowDepthFade, 0.0f, 1.0f);
	perFrame.ContactShadowMinIntensity = sanitizeFloat(settings.ContactShadowMinIntensity, 0.0f, 1.0f);
	perFrame.ShadowMapSlots = ShadowCasterManager::GetInstalledSlotCount();
	perFrame.EnableParticleContactShadows = settings.EnableContactShadows && settings.EnableParticleContactShadows;
	std::copy(clusterSize, clusterSize + 3, perFrame.ClusterSize);
	perFrame.EnableLightsVisualisation = EnableLightsVisualisation;
	perFrame.LightsVisualisationMode = LightsVisualisationMode;
	return perFrame;
}

void LightLimitFix::SetupResources()
{
	auto screenSize = globals::state->screenSize;
	if (globals::game::isVR)
		screenSize.x *= .5;
	clusterSize[0] = ((uint)screenSize.x + 63) / 64;
	clusterSize[1] = ((uint)screenSize.y + 63) / 64;
	clusterSize[2] = 32;
	uint clusterCount = clusterSize[0] * clusterSize[1] * clusterSize[2];

	{
		CompileComputeShaders();

		lightBuildingCB = new ConstantBuffer(ConstantBufferDesc<LightBuildingCB>());
		lightCullingCB = new ConstantBuffer(ConstantBufferDesc<LightCullingCB>());
		shadowDemandCB = new ConstantBuffer(ConstantBufferDesc<ShadowDemandCB>(), "LLF::ShadowDemandCB");
		shadowDepthPyramidCB = new ConstantBuffer(ConstantBufferDesc<ShadowDepthPyramidCB>(), "LLF::ShadowDepthPyramidCB");
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DEFAULT;
		sbDesc.CPUAccessFlags = 0;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.Flags = 0;

		std::uint32_t numElements = clusterCount;

		sbDesc.StructureByteStride = sizeof(ClusterAABB);
		sbDesc.ByteWidth = sizeof(ClusterAABB) * numElements;
		clusters = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::Clusters");
		srvDesc.Buffer.NumElements = numElements;
		clusters->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		clusters->CreateUAV(uavDesc);

		numElements = 1;
		sbDesc.StructureByteStride = sizeof(uint32_t);
		sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
		lightIndexCounter = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::LightIndexCounter");
		srvDesc.Buffer.NumElements = numElements;
		lightIndexCounter->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		lightIndexCounter->CreateUAV(uavDesc);

		numElements = clusterCount * CLUSTER_MAX_LIGHTS;
		sbDesc.StructureByteStride = sizeof(uint32_t);
		sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
		lightIndexList = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::LightIndexList");
		srvDesc.Buffer.NumElements = numElements;
		lightIndexList->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		lightIndexList->CreateUAV(uavDesc);

		numElements = clusterCount;
		sbDesc.StructureByteStride = sizeof(LightGrid);
		sbDesc.ByteWidth = sizeof(LightGrid) * numElements;
		lightGrid = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::LightGrid");
		srvDesc.Buffer.NumElements = numElements;
		lightGrid->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		lightGrid->CreateUAV(uavDesc);

		numElements = clusterSize[0] * clusterSize[1] * (globals::game::isVR ? 2u : 1u);
		sbDesc.StructureByteStride = sizeof(float) * 2;
		sbDesc.ByteWidth = sbDesc.StructureByteStride * numElements;
		tileDepthRange = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::TileDepthRange");
		srvDesc.Buffer.NumElements = numElements;
		tileDepthRange->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		tileDepthRange->CreateUAV(uavDesc);

		numElements = MAX_SHADOW_DEMAND_SLOTS;
		sbDesc.StructureByteStride = sizeof(uint32_t);
		sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
		shadowDemand = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::ShadowDemand");
		uavDesc.Buffer.NumElements = numElements;
		shadowDemand->CreateUAV(uavDesc);

		numElements = 1;
		sbDesc.StructureByteStride = sizeof(uint32_t);
		sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
		shadowDemandOverflow = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::ShadowDemandOverflow");
		uavDesc.Buffer.NumElements = numElements;
		shadowDemandOverflow->CreateUAV(uavDesc);

		numElements = kShadowDemandMaxElements;
		sbDesc.StructureByteStride = sizeof(uint32_t);
		sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
		shadowDemandMax = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::ShadowDemandMax");
		uavDesc.Buffer.NumElements = numElements;
		shadowDemandMax->CreateUAV(uavDesc);

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		stagingDesc.BindFlags = 0;
		// No SHADER_RESOURCE/UNORDERED_ACCESS bind flag here (staging has none),
		// so MISC_BUFFER_STRUCTURED would make CreateBuffer reject the desc.
		stagingDesc.MiscFlags = 0;
		stagingDesc.StructureByteStride = 0;
		stagingDesc.ByteWidth = sizeof(uint32_t) * MAX_SHADOW_DEMAND_SLOTS;
		D3D11_BUFFER_DESC maxStagingDesc = stagingDesc;
		maxStagingDesc.ByteWidth = sizeof(uint32_t) * kShadowDemandMaxElements;
		for (uint32_t i = 0; i < kShadowDemandRingSize; i++) {
			shadowDemandStaging[i] = eastl::make_unique<Buffer>(stagingDesc, nullptr,
				fmt::format("LLF::ShadowDemandStaging{}", i).c_str());
			shadowDemandMaxStaging[i] = eastl::make_unique<Buffer>(maxStagingDesc, nullptr,
				fmt::format("LLF::ShadowDemandMaxStaging{}", i).c_str());
			shadowDemandRingState[i] = ShadowDemandRingState::Idle;
			shadowDemandRingWriteFrame[i] = 0;
		}
		shadowDemandEMA.fill(0.0f);
		shadowDemandEMAInitialized = false;
		shadowDemandMaxLatest.fill(0);
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(LightData);
		sbDesc.ByteWidth = sizeof(LightData) * MAX_LIGHTS;
		lights = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::Lights");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_LIGHTS;
		lights->CreateSRV(srvDesc);
	}

	{
		strictLightDataCB = new ConstantBuffer(ConstantBufferDesc<StrictLightDataCB>());
	}
}

void LightLimitFix::Reset()
{
	effectLightValidationCache.clear();

	std::lock_guard<std::mutex> queueLock{ particleLightsQueueMutex };

	for (auto& particleLight : currentParticleLights) {
		if (!particleLight.node)
			continue;

		if (!particleLight.billboard) {
			if (const auto particleSystem = static_cast<RE::NiParticleSystem*>(particleLight.node)) {
				if (auto particleData = particleSystem->GetParticlesRuntimeData().particleData.get())
					particleData->DecRefCount();
			}
		}
		particleLight.node->DecRefCount();
	}
	currentParticleLights.clear();
	std::swap(currentParticleLights, queuedParticleLights);
	particleLightCount.store(static_cast<uint32_t>(currentParticleLights.size()), std::memory_order_relaxed);
	// References are keyed by transient pass geometry pointers; rebuild every frame to avoid stale entries.
	particleLightsReferences.clear();
	jsonPlacedLightCache.clear();
}

void LightLimitFix::OnSceneTransitionReset(bool opening)
{
	// LoadingMenu open: drop the shadow-caster session caches before the engine tears down the old
	// cell. Dispatched on the render thread (Feature::DrainSceneTransitions), so it serializes with
	// the settings-menu table iteration that reads the same caches instead of racing it.
	if (opening) {
		ShadowCasterManager::ResetSession();
		// Slots are reassigned to different lights across a cell transition;
		// an old occupant's decaying EMA must not read as a new light's demand.
		shadowDemandEMA.fill(0.0f);
		shadowDemandEMAInitialized = false;
		shadowDemandMaxLatest.fill(0);
		shadowDemandClusterSaturated = false;
		for (uint32_t i = 0; i < kShadowDemandRingSize; i++)
			shadowDemandRingState[i] = ShadowDemandRingState::Idle;
	}
}

void LightLimitFix::LoadSettings(json& o_json)
{
	settings = o_json;
	SanitizeSettings(settings);
	// iShadowMapResolution:Display is owned by Skyrim's INI, not our JSON.
	ShadowCasterManager::LoadINISettings();

	// Raise saved values below the current floor so older configs migrate.
	if (settings.ShadowSettings.MaxRedrawPerFrame < ShadowCasterManager::Settings::kMinMaxRedrawPerFrame)
		settings.ShadowSettings.MaxRedrawPerFrame = ShadowCasterManager::Settings::kMinMaxRedrawPerFrame;

	// Upgrade an untouched ScoreFormula to the current default; a customized
	// formula never matches a legacy default verbatim and is left alone.
	for (const char* legacy : ShadowCasterManager::kLegacyScoreFormulas)
		if (settings.ShadowSettings.ScoreFormula == legacy) {
			settings.ShadowSettings.ScoreFormula = ShadowCasterManager::Settings{}.ScoreFormula;
			break;
		}
}

void LightLimitFix::SaveSettings(json& o_json)
{
	SanitizeSettings(settings);
	o_json = settings;
	ShadowCasterManager::SaveINISettings();
}

void LightLimitFix::RestoreDefaultSettings()
{
	settings = {};
	SanitizeSettings(settings);
}

json LightLimitFix::GetDiagnostics()
{
	return json{
		{ "particleLightCount", particleLightCount.load(std::memory_order_relaxed) },
		{ "lightCount", clusteredLightCount.load(std::memory_order_relaxed) },
		{ "maxLights", MAX_LIGHTS },
	};
}

json LightLimitFix::GetRuntimeFlags()
{
	return json{
		{ "ShadowDemandInstrumentation", ShadowDemandInstrumentation },
	};
}

bool LightLimitFix::SetRuntimeFlag(std::string_view name, bool value)
{
	if (name == "ShadowDemandInstrumentation") {
		ShadowDemandInstrumentation = value;
		return true;
	}
	return false;
}

RE::NiNode* GetParentRoomNode(RE::NiAVObject* object)
{
	if (object == nullptr)
		return nullptr;

	static const auto* roomRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSMultiBoundRoom }.get();
	static const auto* portalRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSPortalSharedNode }.get();

	const auto* rtti = object->GetRTTI();
	if (rtti == roomRtti || rtti == portalRtti)
		return static_cast<RE::NiNode*>(object);

	return GetParentRoomNode(object->parent);
}

void LightLimitFix::BSLightingShader_SetupGeometry_Before(RE::BSRenderPass* a_pass)
{
	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	ClearStrictLightData(strictLightDataTemp, true);

	if (!a_pass || !a_pass->geometry)
		return;

	if (!roomNodes.empty()) {
		if (RE::NiNode* roomNode = GetParentRoomNode(a_pass->geometry)) {
			if (auto it = roomNodes.find(roomNode); it != roomNodes.cend())
				strictLightDataTemp.RoomIndex = it->second;
		}
	}
}

void LightLimitFix::BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights(RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->sceneLights) {
		ClearStrictLightData(strictLightDataTemp, false);
		return;
	}

	auto smState = globals::game::smState;
	if (!smState) {
		ClearStrictLightData(strictLightDataTemp, false);
		return;
	}

	auto& isl = globals::features::inverseSquareLighting;

	auto accumulator = *globals::game::currentAccumulator.get();
	if (!accumulator) {
		ClearStrictLightData(strictLightDataTemp, false);
		return;
	}

	bool inWorld = accumulator->GetRuntimeData().activeShadowSceneNode == smState->shadowSceneNode[0];
	const bool isInterior = Util::IsInterior();
	// The first-person pass rebases b12's posAdjust, so its draws can't index
	// the world-camera cluster grid. BSShaderAccumulator::firstPerson is never
	// written on SE, so detect the camera rebase directly; VR keeps the grid.
	const bool firstPerson = inWorld && !globals::game::isVR &&
	                         (Util::GetEyePosition(0) - eyePositionCached[0]).SqrLength() > 1.0f;

	constexpr uint32_t kStrictLightCapacity = 15;
	const uint32_t availableSceneLights = a_pass->numLights > 0 ? (a_pass->numLights - 1) : 0;
	const uint32_t requestedStrictLights = (inWorld && !firstPerson) ? 0u : availableSceneLights;
	const uint32_t strictLightCount = std::min(requestedStrictLights, kStrictLightCapacity);
	const uint32_t strictShadowLightCount = std::min(static_cast<uint32_t>(a_pass->numShadowLights), availableSceneLights);
	RefreshJsonPlacedLightCacheFrame();

	ClearStrictLightData(strictLightDataTemp, false);
	strictLightDataTemp.FirstPerson = firstPerson ? 1u : 0u;
	if (firstPerson) {
		// Shadow-space projections need the true world eye; b12's posAdjust is
		// viewmodel-local during this pass and cannot reconstruct it.
		strictLightDataTemp.WorldEyePosition =
			float4(eyePositionCached[0].x, eyePositionCached[0].y, eyePositionCached[0].z, 0.0f);
	}

	uint32_t outIndex = 0;
#if defined(_MSC_VER)
	__try
#endif
	{
		for (uint32_t i = 0; i < strictLightCount; i++) {
			auto bsLight = a_pass->sceneLights[i + 1];
			if (!bsLight)
				continue;
			auto niLight = bsLight->light.get();
			if (!niLight)
				continue;
			// IsSuppressed includes solo (every key except the soloed one is
			// implicitly suppressed). The cluster path already filters through
			// this; strict lights need the same so solo/hover debug tooling is
			// consistent between world and first-person surfaces.
			if (ShadowCasterManager::IsSuppressed(reinterpret_cast<uintptr_t>(bsLight)))
				continue;

			auto& runtimeData = niLight->GetLightRuntimeData();

			LightData light{};
			light.color = float3{ runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue };
			light.lightFlags = std::bit_cast<LightFlags>(runtimeData.ambient.red);

			if (isl.loaded) {
				isl.ProcessLight(light, bsLight, niLight);
			} else {
				light.radius = runtimeData.radius.x;
				light.fade = runtimeData.fade;
			}

			SetPointLightTypeFlags(light, bsLight);
			light.fade *= bsLight->lodDimmer;
			const bool isPortalStrict = !IsGlobalLight(bsLight);
			ApplyJsonPlacedLightIntensityScale(light, bsLight, niLight, isPortalStrict, isInterior);

#if defined(ENABLE_EFFECTS11)
			auto& effects11 = globals::features::effects11;
			if (inWorld && effects11.enableEffect)
				effects11.OverridePointLightColor(light.color);
#endif

			SetLightPosition(light, niLight->world.translate, inWorld);

			ApplyLightDebugOverrides(light, bsLight);

			if (i < strictShadowLightCount && bsLight->IsShadowLight()) {
				auto* shadowLight = static_cast<RE::BSShadowLight*>(bsLight);
				// Use SCM's stable slot: shadowmapDescriptors[0].shadowmapIndex can be
				// corrupted mid-frame by ReturnShadowmaps. -1 means sun/inactive, skip.
				const int32_t slot = ShadowCasterManager::GetShadowSlot(shadowLight);
				if (slot >= 0 && static_cast<uint32_t>(slot) < ShadowCasterManager::GetInstalledSlotCount()) {
					light.shadowMapIndex = static_cast<uint32_t>(slot);
					light.lightFlags.set(LightFlags::Shadow);
				}
			}

			strictLightDataTemp.StrictLights[outIndex++] = light;
		}
		strictLightDataTemp.NumStrictLights = outIndex;

		// Don't build strictLightDataTemp.ShadowBitMask: no shader reads it (the
		// IsLightIgnored bit-mask branch was replaced by per-light shadowMapIndex
		// sampling, set inline above). The field stays for cbuffer ABI stability
		// and is zero-initialised by ClearStrictLightData.
	}
#if defined(_MSC_VER)
	__except (1) {
		ClearStrictLightData(strictLightDataTemp, false);
	}
#endif
}

void LightLimitFix::BSLightingShader_SetupGeometry_After(RE::BSRenderPass*)
{
	auto shaderCache = globals::shaderCache;
	auto context = globals::d3d::context;
	auto smState = globals::game::smState;

	if (!shaderCache->IsEnabled())
		return;

	if (!smState || !strictLightDataCB)
		return;

	auto accumulator = *globals::game::currentAccumulator.get();
	if (!accumulator)
		return;

	auto shadowSceneNode = smState->shadowSceneNode[0];

	const auto isEmpty = strictLightDataTemp.NumStrictLights == 0;
	const bool isWorld = accumulator->GetRuntimeData().activeShadowSceneNode == shadowSceneNode;
	const auto roomIndex = strictLightDataTemp.RoomIndex;
	const bool isFirstPerson = strictLightDataTemp.FirstPerson != 0;

	if (!isEmpty || (isEmpty && !wasEmpty) || isWorld != wasWorld || isFirstPerson != wasFirstPerson || previousRoomIndex != roomIndex) {
		strictLightDataCB->Update(strictLightDataTemp);
		wasEmpty = isEmpty;
		wasWorld = isWorld;
		wasFirstPerson = isFirstPerson;
		previousRoomIndex = roomIndex;
	}

	if (frameChecker.IsNewFrame()) {
		ID3D11Buffer* buffer = { strictLightDataCB->CB() };
		context->PSSetConstantBuffers(3, 1, &buffer);
	}
}

void LightLimitFix::SetLightPosition(LightLimitFix::LightData& a_light, RE::NiPoint3 a_initialPosition, bool a_cached)
{
	for (int eyeIndex = 0; eyeIndex < eyeCount; eyeIndex++) {
		RE::NiPoint3 eyePosition;

		if (a_cached)
			eyePosition = eyePositionCached[eyeIndex];
		else
			eyePosition = Util::GetEyePosition(eyeIndex);

		auto worldPos = a_initialPosition - eyePosition;
		a_light.positionWS[eyeIndex].data.x = worldPos.x;
		a_light.positionWS[eyeIndex].data.y = worldPos.y;
		a_light.positionWS[eyeIndex].data.z = worldPos.z;
	}
}

void LightLimitFix::RefreshJsonPlacedLightCacheFrame()
{
	if (jsonPlacedLightCacheFrameChecker.IsNewFrame())
		jsonPlacedLightCache.clear();
}

bool LightLimitFix::IsJsonPlacedLight(RE::BSLight* a_bsLight, RE::NiLight* a_niLight)
{
	if (!a_bsLight || !a_niLight || !a_bsLight->pointLight)
		return false;
	if (!globals::features::inverseSquareLighting.loaded)
		return false;
	if (const auto it = jsonPlacedLightCache.find(a_niLight); it != jsonPlacedLightCache.end())
		return it->second;

	bool isJsonPlacedLight = false;
	if (const auto ownerRef = a_niLight->GetUserData()) {
		if (const auto ownerBase = ownerRef->GetObjectReference(); ownerBase && ownerBase->GetFormType() != RE::FormType::Light) {
			const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(a_niLight);
			if (runtimeData && runtimeData->lighFormId != 0) {
				const auto lighForm = RE::TESForm::LookupByID(runtimeData->lighFormId);
				isJsonPlacedLight = lighForm && lighForm->GetFormType() == RE::FormType::Light;
			}
		}
	}

	jsonPlacedLightCache.insert_or_assign(a_niLight, isJsonPlacedLight);
	return isJsonPlacedLight;
}

void LightLimitFix::ApplyJsonPlacedLightIntensityScale(
	LightData& a_light,
	RE::BSLight* a_bsLight,
	RE::NiLight* a_niLight,
	bool a_isPortalStrict,
	bool a_isInterior)
{
	if (std::abs(settings.JsonPlacedLightIntensity - 1.0f) <= 1e-4f)
		return;
	if (settings.JsonPlacedLightsInteriorsOnly && !a_isInterior)
		return;
	if (settings.JsonPlacedLightsPortalStrictOnly && !a_isPortalStrict)
		return;
	if (!IsJsonPlacedLight(a_bsLight, a_niLight))
		return;

	a_light.fade *= settings.JsonPlacedLightIntensity;
}

void LightLimitFix::Prepass()
{
	CS_GPU_PASS("LightLimitFix::Prepass");

	auto context = globals::d3d::context;

	ShadowCasterManager::ShadowDemandSample demandSample;
	demandSample.ema = shadowDemandEMA;
	demandSample.maxLatest = shadowDemandMaxLatest;
	demandSample.initialized = shadowDemandEMAInitialized;
	demandSample.clusterSaturated = shadowDemandClusterSaturated;
	demandSample.instrumentation = ShadowDemandInstrumentation;
	demandSample.redrawDueGate = settings.ShadowSettings.RedrawDueGateEnabled;
	demandSample.sampleSerial = shadowDemandSampleSerial;
	demandSample.lastDrainFrame = shadowDemandLastDrainFrame;
	demandSample.frameCounter = shadowDemandFrameCounter;
	demandSample.tileCount = clusterSize[0] * clusterSize[1];
	ShadowCasterManager::SetShadowDemand(demandSample);
	ShadowCasterManager::Update(settings.ShadowSettings, globals::game::smState->shadowSceneNode[0], nullptr);
	UpdateLights();

	ID3D11ShaderResourceView* views[3]{};
	views[0] = lights->srv.get();
	views[1] = lightIndexList->srv.get();
	views[2] = lightGrid->srv.get();
	context->PSSetShaderResources(35, ARRAYSIZE(views), views);
}

bool LightLimitFix::IsValidLight(RE::BSLight* a_light)
{
	return a_light && a_light->light && a_light->light.get() && !a_light->light->GetFlags().any(RE::NiAVObject::Flag::kHidden);
}

bool LightLimitFix::IsGlobalLight(RE::BSLight* a_light)
{
	return a_light && !(a_light->portalStrict || !a_light->portalGraph);
}

void LightLimitFix::PostPostLoad()
{
	particleLights.GetConfigs();
	Hooks::Install();
	ShadowCasterManager::Init(settings.ShadowSettings);
	ShadowCasterManager::Install(settings.ShadowSettings);
}

void LightLimitFix::DataLoaded()
{
	if (auto gameSettings = globals::game::gameSettingCollection) {
		if (auto iMagicLightMaxCount = gameSettings->GetSetting("iMagicLightMaxCount")) {
			iMagicLightMaxCount->data.i = MAXINT32;
			logger::info("[LLF] Unlocked magic light limit");
		}
	}
}

void LightLimitFix::ClearShaderCache()
{
	clusterBuildingCS.Reset();
	clusterCullingCS.Reset();
	shadowDemandCS.Reset();
	shadowDepthPyramidCS.Reset();

	CompileComputeShaders();
}

void LightLimitFix::CompileComputeShaders()
{
	std::vector<std::pair<const char*, const char*>> clusterDefines;
	if (globals::game::isVR)
		clusterDefines = { { "VR", "" } };
	clusterBuildingCS.Get(L"Data\\Shaders\\LightLimitFix\\ClusterBuildingCS.hlsl", clusterDefines, "cs_5_0");
	clusterCullingCS.Get(L"Data\\Shaders\\LightLimitFix\\ClusterCullingCS.hlsl", clusterDefines, "cs_5_0");
	shadowDemandCS.Get(L"Data\\Shaders\\LightLimitFix\\ShadowDemandCS.hlsl", clusterDefines, "cs_5_0");
	shadowDepthPyramidCS.Get(L"Data\\Shaders\\LightLimitFix\\ShadowDepthPyramidCS.hlsl", clusterDefines, "cs_5_0");
}

void LightLimitFix::UpdateLights()
{
	ZoneScopedN("LLF::UpdateLights");

	auto context = globals::d3d::context;
	if (!context || !lights || !lights->resource) {
		// Drop last frame's particle lights so AddParticleLightLuminance (gameplay
		// thread) can't keep feeding stale lights into NPC detection on this early-out.
		std::lock_guard<std::shared_mutex> lk{ cachedParticleLightsMutex };
		cachedParticleLights.clear();
		return;
	}

	auto smState = globals::game::smState;
	auto& isl = globals::features::inverseSquareLighting;
	auto clearAndUpdate = [&]() {
		lightCount = 0;
		clusteredLightCount.store(0, std::memory_order_relaxed);
		// Drop last frame's particle lights too: AddParticleLightLuminance reads
		// cachedParticleLights on the gameplay thread, so a bare early-return here
		// would leave stale lights contributing to NPC light-level detection.
		{
			std::lock_guard<std::shared_mutex> lk{ cachedParticleLightsMutex };
			cachedParticleLights.clear();
		}
		UpdateStructure();
	};

	if (!smState) {
		clearAndUpdate();
		return;
	}

	auto shadowSceneNode = smState->shadowSceneNode[0];
	if (!shadowSceneNode) {
		clearAndUpdate();
		return;
	}

	// Cache data since cameraData can become invalid in first-person
	for (int eyeIndex = 0; eyeIndex < eyeCount; eyeIndex++) {
		auto eyePosition = globals::game::frameBufferCached.GetCameraPosAdjust(eyeIndex);
		eyePositionCached[eyeIndex] = { eyePosition.x, eyePosition.y, eyePosition.z };
	}

	eastl::vector<LightData> lightsData{};
	lightsData.reserve(MAX_LIGHTS);
	const bool isInterior = Util::IsInterior();
	RefreshJsonPlacedLightCacheFrame();

	{
		CS_PROFILE_CPU_SCOPE(globals::profiler, "LightLimitFix::SceneLightsCPU");

		// Process point lights

		roomNodes.clear();

		auto addRoom = [&](RE::NiNode* node, LightData& light) {
			if (!node)
				return;

			constexpr std::size_t kMaxRoomFlags = 128;
			uint8_t roomIndex = 0;
			if (auto it = roomNodes.find(node); it == roomNodes.cend()) {
				if (roomNodes.size() >= kMaxRoomFlags)
					return;
				roomIndex = static_cast<uint8_t>(roomNodes.size());
				roomNodes.insert_or_assign(node, roomIndex);
			} else {
				roomIndex = it->second;
			}
			light.roomFlags.SetBit(roomIndex, 1);
		};

		auto addLight = [&](const RE::NiPointer<RE::BSLight>& e) {
			if (auto bsLight = e.get()) {
				if (auto niLight = bsLight->light.get()) {
					// IsSuppressed includes solo (every key except the soloed one is
					// implicitly suppressed). This filters every non-shadow cluster
					// light through the user's debug overrides.
					if (ShadowCasterManager::IsSuppressed(reinterpret_cast<uintptr_t>(bsLight)))
						return;
					if (IsValidLight(bsLight)) {
						auto& runtimeData = niLight->GetLightRuntimeData();

						LightData light{};
						light.color = float3{ runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue };
						light.lightFlags = std::bit_cast<LightFlags>(runtimeData.ambient.red);

						if (isl.loaded) {
							isl.ProcessLight(light, bsLight, niLight);
						} else {
							light.radius = runtimeData.radius.x;
							light.fade = runtimeData.fade;
						}

#if defined(ENABLE_EFFECTS11)
						auto& effects11 = globals::features::effects11;
						if (effects11.enableEffect)
							effects11.OverridePointLightColor(light.color);
#endif

						SetPointLightTypeFlags(light, bsLight);
						light.fade *= bsLight->lodDimmer;
						const bool isPortalStrict = !IsGlobalLight(bsLight);

						if (isPortalStrict) {
							for (const auto& roomPtr : bsLight->rooms) {
								if (roomPtr)
									addRoom(static_cast<RE::NiNode*>(roomPtr), light);
							}
							for (const auto& portalPtr : bsLight->portals) {
								if (portalPtr && portalPtr->portalSharedNode)
									addRoom(static_cast<RE::NiNode*>(portalPtr->portalSharedNode.get()), light);
							}
							light.lightFlags.set(LightFlags::PortalStrict);
						}
						ApplyJsonPlacedLightIntensityScale(light, bsLight, niLight, isPortalStrict, isInterior);

						SetLightPosition(light, niLight->world.translate);

						ApplyLightDebugOverrides(light, bsLight);

						if ((light.color.x + light.color.y + light.color.z) * light.fade > 1e-4 && light.radius > 1e-4) {
							lightsData.push_back(light);
						}
					}
				}
			}
		};

		auto addShadowLight = [&](RE::BSShadowLight* shadowLight, bool castsShadow, uint32_t shadowSlot = 0) {
			if (IsValidLight(shadowLight)) {
				if (auto niLight = shadowLight->light.get()) {
					auto& runtimeData = niLight->GetLightRuntimeData();

					LightData light{};
					light.color = float3{ runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue };
					light.lightFlags = std::bit_cast<LightFlags>(runtimeData.ambient.red);

					if (isl.loaded) {
						isl.ProcessLight(light, shadowLight, niLight);
					} else {
						light.radius = runtimeData.radius.x;
						// light.color *= runtimeData.fade;
						light.fade = runtimeData.fade;
					}

					SetPointLightTypeFlags(light, shadowLight);
					light.fade *= shadowLight->lodDimmer;

					if (!IsGlobalLight(shadowLight)) {
						// List of BSMultiBoundRooms affected by a light
						for (const auto& roomPtr : shadowLight->rooms) {
							addRoom(roomPtr, light);
						}
						// List of BSPortals affected by a light
						for (const auto& portalPtr : shadowLight->portals) {
							addRoom(portalPtr->portalSharedNode.get(), light);
						}
						light.lightFlags.set(LightFlags::PortalStrict);
					}

					if (castsShadow) {
						// Use the caller-provided stable slot index from s_lights
						// rather than shadowmapDescriptors[0].shadowmapIndex, which
						// can drift relative to our scheduler-assigned slot when
						// ReturnShadowmaps fires between scheduling and lighting.
						light.shadowMapIndex = shadowSlot;
						light.lightFlags.set(LightFlags::Shadow);
					}

					SetLightPosition(light, niLight->world.translate);

					ApplyLightDebugOverrides(light, shadowLight);

					if ((light.color.x + light.color.y + light.color.z) * light.fade > 1e-4 && light.radius > 1e-4) {
						lightsData.push_back(light);
					}
				}
			}
		};

		// shadowLightPtrs lets activeLights below skip lights added here: EnableLight
		// calls both GameEnableLight and GameSetShadowCasterSlot for redrawn lights.
		static ankerl::unordered_dense::set<RE::BSLight*> shadowLightPtrs;
		shadowLightPtrs.clear();
		shadowLightPtrs.reserve(ShadowCasterManager::GetInstalledSlotCount() + 1);
		ShadowCasterManager::ForEachShadowLight(shadowSceneNode->GetRuntimeData().shadowLightsAccum,
			[&](RE::BSShadowLight* light) {
				shadowLightPtrs.insert(light);
				// -1 = sun, sampled via the cascade path; skip injection but keep it in
				// shadowLightPtrs so it isn't re-added. >=0 = kSHADOWMAPS slice index.
				int32_t stableSlot = ShadowCasterManager::GetShadowSlot(light);
				if (stableSlot < 0)
					return;
				bool castsShadow = static_cast<uint32_t>(stableSlot) < ShadowCasterManager::GetInstalledSlotCount();
				addShadowLight(light, castsShadow, castsShadow ? static_cast<uint32_t>(stableSlot) : 0u);
			});

		// Backstop for the same accum-walk silent-drop as
		// ShadowRenderer.cpp::CopyShadowLightData: a light this walk misses gets zero
		// illumination this frame, not just no shadow. Re-visit slots it didn't see.
		{
			const auto& pool = ShadowCasterManager::GetLights();
			const int32_t first = pool.PointLightFirst();
			const int32_t end = std::min(static_cast<int32_t>(ShadowCasterManager::GetInstalledSlotCount()), pool.Size);
			for (int32_t i = first; i < end; i++) {
				auto* light = pool.Lights[i].Light;
				if (!light || shadowLightPtrs.count(light))
					continue;
				shadowLightPtrs.insert(light);
				addShadowLight(light, true, static_cast<uint32_t>(i));
			}
		}

		for (auto& e : shadowSceneNode->GetRuntimeData().activeLights) {
			if (auto bsLight = e.get(); bsLight && shadowLightPtrs.count(bsLight))
				continue;  // shadow light: already added above with correct Shadow flag
			addLight(e);
		}

		// Converted shadow lights stay in activeShadowLights, not activeLights, so
		// iterate s_normalConvert directly or these lights get no cluster entry.
		ShadowCasterManager::ForEachConvertedLight([&](RE::BSShadowLight* light) {
			auto* asBs = static_cast<RE::BSLight*>(light);
			if (shadowLightPtrs.count(asBs))
				return;  // simultaneously a shadow caster this frame; already added
			// Honour the user's suppression toggle in the shadow caster table:
			// converted lights share the same lightKey suppression set as shadow
			// lights, so suppressing one in the table hides it whether it's
			// rendering as a shadow caster or demoted to non-shadow.
			if (ShadowCasterManager::IsSuppressed(reinterpret_cast<uintptr_t>(light)))
				return;
			ShadowCasterManager::RestoreZeroedLodDimmer(light);
			addLight(RE::NiPointer<RE::BSLight>(asBs));
		});
	}

	{
		CS_PROFILE_CPU_SCOPE(globals::profiler, "LightLimitFix::ParticleLightsCPU");
		ProcessQueuedParticleLights(lightsData);
	}

	lightCount = std::min((uint)lightsData.size(), MAX_LIGHTS);
	clusteredLightCount.store(lightCount, std::memory_order_relaxed);

	{
		CS_PROFILE_CPU_SCOPE(globals::profiler, "LightLimitFix::UploadLightsCPU");
		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(lights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(LightData) * lightCount;
		if (bytes > 0)
			memcpy_s(mapped.pData, bytes, lightsData.data(), bytes);
		context->Unmap(lights->resource.get(), 0);
	}

	UpdateStructure();
}

void LightLimitFix::UpdateStructure()
{
	std::vector<std::pair<const char*, const char*>> clusterDefines;
	if (globals::game::isVR)
		clusterDefines = { { "VR", "" } };
	auto* clusterBuilding = clusterBuildingCS.Get(L"Data\\Shaders\\LightLimitFix\\ClusterBuildingCS.hlsl", clusterDefines, "cs_5_0");
	auto* clusterCulling = clusterCullingCS.Get(L"Data\\Shaders\\LightLimitFix\\ClusterCullingCS.hlsl", clusterDefines, "cs_5_0");
	if (!clusterBuilding || !clusterCulling) {
		// The shading shader reads lightGrid every frame regardless of whether this
		// dispatch ran -- zero its light counts so a skipped build/cull doesn't leave
		// stale per-cluster light indices from a prior camera position bound as valid.
		const UINT zero[4]{ 0, 0, 0, 0 };
		globals::d3d::context->ClearUnorderedAccessViewUint(lightGrid->uav.get(), zero);
		UpdateShadowDemand();
		return;
	}

	auto context = globals::d3d::context;

	lightsNear = *globals::game::cameraNear;
	lightsFar = *globals::game::cameraFar;

	auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
	if (globals::game::isVR)
		renderSize.x *= .5;
	clusterSize[0] = ((uint)renderSize.x + 63) / 64;
	clusterSize[1] = ((uint)renderSize.y + 63) / 64;
	clusterSize[2] = 32;

	{
		CS_GPU_PASS("LightLimitFix::ClusterBuild");
		LightBuildingCB updateData{};
		updateData.LightsNear = lightsNear;
		updateData.LightsFar = lightsFar;
		std::copy(clusterSize, clusterSize + 3, updateData.ClusterSize);

		lightBuildingCB->Update(updateData);

		ID3D11Buffer* buffer = lightBuildingCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);

		ID3D11UnorderedAccessView* clusters_uav = clusters->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &clusters_uav, nullptr);

		context->CSSetShader(clusterBuilding, nullptr, 0);
		context->Dispatch((clusterSize[0] + 15) / 16, (clusterSize[1] + 15) / 16, (clusterSize[2] + 3) / 4);

		ID3D11UnorderedAccessView* null_uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
	}

	{
		CS_GPU_PASS("LightLimitFix::ClusterCull");
		LightCullingCB updateData{};
		updateData.LightCount = lightCount;
		std::copy(clusterSize, clusterSize + 3, updateData.ClusterSize);

		lightCullingCB->Update(updateData);

		UINT counterReset[4] = { 0, 0, 0, 0 };
		context->ClearUnorderedAccessViewUint(lightIndexCounter->uav.get(), counterReset);

		ID3D11Buffer* buffer = lightCullingCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);

		ID3D11ShaderResourceView* srvs[] = { clusters->srv.get(), lights->srv.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[] = { lightIndexCounter->uav.get(), lightIndexList->uav.get(), lightGrid->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(clusterCulling, nullptr, 0);
		context->Dispatch((clusterSize[0] + 15) / 16, (clusterSize[1] + 15) / 16, (clusterSize[2] + 3) / 4);
	}

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11Buffer* null_buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &null_buffer);

	ID3D11ShaderResourceView* null_srvs[2] = { nullptr };
	context->CSSetShaderResources(0, 2, null_srvs);

	ID3D11UnorderedAccessView* null_uavs[3] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 3, null_uavs, nullptr);

	UpdateShadowDemand();
}

static_assert(LightLimitFix::MAX_SHADOW_DEMAND_SLOTS == ShadowCasterManager::kMaxShadowDemandSlots,
	"LightLimitFix::MAX_SHADOW_DEMAND_SLOTS and ShadowCasterManager::kMaxShadowDemandSlots must match -- "
	"SetShadowDemand copies between arrays of these sizes.");

void LightLimitFix::UpdateShadowDemand()
{
	// No-op if either compute shader failed to build. Clear the demand sample so
	// SetShadowDemand's consumer can't reuse a stale reading from before the
	// cluster layout changed or the shader cache failed.
	if (!shadowDemandCS || !shadowDepthPyramidCS) {
		shadowDemandEMA.fill(0.0f);
		shadowDemandEMAInitialized = false;
		shadowDemandMaxLatest.fill(0);
		shadowDemandClusterSaturated = false;
		// A Pending slot's GPU readback may still land after shaders recover;
		// drop it too, or its pre-failure sample would drain straight back in.
		for (uint32_t i = 0; i < kShadowDemandRingSize; i++)
			shadowDemandRingState[i] = ShadowDemandRingState::Idle;
		return;
	}

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	shadowDemandFrameCounter++;

	// Only the instrumentation log consumes the lag window; keep it empty while
	// off, or the first report after enabling it averages an arbitrarily long idle.
	if (!ShadowDemandInstrumentation) {
		shadowDemandDrainLagMin = UINT32_MAX;
		shadowDemandDrainLagMax = 0;
		shadowDemandDrainLagSum = 0;
		shadowDemandDrainCount = 0;
	}

	// Set inside the CS_GPU_PASS block below, but must outlive it: the ring
	// copy after the block needs the TapCount actually dispatched this frame.
	uint32_t dispatchedTapCount = 1;
	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	{
		CS_GPU_PASS("LightLimitFix::ShadowDepthPyramid");

		ShadowDepthPyramidCB pyramidCB{};
		std::copy(clusterSize, clusterSize + 3, pyramidCB.ClusterSize);
		shadowDepthPyramidCB->Update(pyramidCB);

		ID3D11Buffer* cb = shadowDepthPyramidCB->CB();
		context->CSSetConstantBuffers(0, 1, &cb);

		ID3D11ShaderResourceView* srvs[] = { depth.depthSRV };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[] = { tileDepthRange->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(shadowDepthPyramidCS.get(), nullptr, 0);
		context->Dispatch(clusterSize[0], clusterSize[1], globals::game::isVR ? 2 : 1);

		context->CSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* null_cb = nullptr;
		context->CSSetConstantBuffers(0, 1, &null_cb);
		ID3D11ShaderResourceView* null_srvs[ARRAYSIZE(srvs)] = { nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), null_srvs);
		ID3D11UnorderedAccessView* null_uavs[ARRAYSIZE(uavs)] = { nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), null_uavs, nullptr);
	}
	{
		CS_GPU_PASS("LightLimitFix::ShadowDemand");

		UINT zero[4] = { 0, 0, 0, 0 };
		context->ClearUnorderedAccessViewUint(shadowDemand->uav.get(), zero);
		context->ClearUnorderedAccessViewUint(shadowDemandOverflow->uav.get(), zero);
		// Mandatory: InterlockedMax persists across frames, so without this clear
		// every slot ratchets to its lifetime peak and never reads zero again.
		context->ClearUnorderedAccessViewUint(shadowDemandMax->uav.get(), zero);

		ShadowDemandCB cbData{};
		cbData.LightsNear = lightsNear;
		cbData.LightsFar = lightsFar;
		cbData.InvLogFarOverNear = 1.0f / std::log(lightsFar / lightsNear);
		// Jitter is mandatory: a fixed centre tap makes an unsampled lit region a
		// PERMANENT blind spot. kZeroDemandSkipStreak's calibration assumes a
		// jittered tap; unjittered is outside its validated domain.
		cbData.FrameIndex = static_cast<uint32_t>(shadowDemandFrameCounter) + 1u;
		std::copy(clusterSize, clusterSize + 3, cbData.ClusterSize);
		// Clamp to the jitter hash cycle's powers-of-two (kDemandTapCount). FrameIndex
		// is never 0, so HLSL's FrameIndex==0 centre-tap branch is dead but kept for
		// re-enabling that debug path without a shader edit.
		cbData.TapCount = std::clamp(std::bit_ceil(static_cast<uint32_t>(kDemandTapCount)), 1u, 8u);
		dispatchedTapCount = cbData.TapCount;
		shadowDemandCB->Update(cbData);

		ID3D11Buffer* cb = shadowDemandCB->CB();
		context->CSSetConstantBuffers(0, 1, &cb);

		ID3D11ShaderResourceView* srvs[] = { depth.depthSRV, lightGrid->srv.get(), lightIndexList->srv.get(), lights->srv.get(),
			tileDepthRange->srv.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[] = { shadowDemand->uav.get(), shadowDemandOverflow->uav.get(),
			shadowDemandMax->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(shadowDemandCS.get(), nullptr, 0);
		context->Dispatch((clusterSize[0] + 15) / 16, (clusterSize[1] + 15) / 16, 1);

		context->CSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* null_cb = nullptr;
		context->CSSetConstantBuffers(0, 1, &null_cb);
		ID3D11ShaderResourceView* null_srvs[ARRAYSIZE(srvs)] = { nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), null_srvs);
		ID3D11UnorderedAccessView* null_uavs2[ARRAYSIZE(uavs)] = { nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), null_uavs2, nullptr);
	}

	// Copy only if the ring slot isn't still awaiting an earlier Map --
	// overwriting a Pending slot's buffer while a Map is outstanding corrupts
	// the read. If Pending, skip; the cursor doesn't advance until it's free.
	uint32_t ring = shadowDemandRingCursor;
	if (shadowDemandRingState[ring] == ShadowDemandRingState::Idle) {
		context->CopyResource(shadowDemandStaging[ring]->resource.get(), shadowDemand->resource.get());
		context->CopyResource(shadowDemandMaxStaging[ring]->resource.get(), shadowDemandMax->resource.get());
		shadowDemandRingState[ring] = ShadowDemandRingState::Pending;
		shadowDemandRingWriteFrame[ring] = shadowDemandFrameCounter;
		shadowDemandRingTapCount[ring] = dispatchedTapCount;
		shadowDemandRingCursor = (ring + 1) % kShadowDemandRingSize;
	}

	// One non-blocking Map per eligible ring per frame; retry next frame on
	// DXGI_ERROR_WAS_STILL_DRAWING. Maxima combine with max(), never bitwise OR
	// (5|6==7 is corruption); the serial advances once regardless of drain count.
	std::array<uint32_t, kShadowDemandMaxElements> maxCombined{};
	bool drainedThisFrame = false;
	for (uint32_t i = 0; i < kShadowDemandRingSize; i++) {
		if (shadowDemandRingState[i] != ShadowDemandRingState::Pending)
			continue;
		if (shadowDemandFrameCounter - shadowDemandRingWriteFrame[i] < kShadowDemandRingSize)
			continue;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		HRESULT hr = context->Map(shadowDemandStaging[i]->resource.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
			continue;
		if (FAILED(hr)) {
			shadowDemandRingState[i] = ShadowDemandRingState::Idle;
			continue;
		}

		D3D11_MAPPED_SUBRESOURCE mappedMax{};
		HRESULT hrMax = context->Map(shadowDemandMaxStaging[i]->resource.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mappedMax);
		if (FAILED(hrMax)) {
			// The pair must stay in lockstep -- a max sample from a different
			// dispatch than its saturation flag is what this same-buffer placement prevents.
			context->Unmap(shadowDemandStaging[i]->resource.get(), 0);
			if (hrMax != DXGI_ERROR_WAS_STILL_DRAWING)
				shadowDemandRingState[i] = ShadowDemandRingState::Idle;
			continue;
		}

		// VR eyes and taps sum into one raw slot; divide by both here using THIS
		// ring slot's dispatched TapCount so a mid-flight override can't desync it.
		const float demandSumDivisor = (globals::game::isVR ? 2.0f : 1.0f) *
		                               static_cast<float>(std::max<uint32_t>(shadowDemandRingTapCount[i], 1u)) * 1024.0f;
		const uint32_t* raw = static_cast<const uint32_t*>(mapped.pData);
		for (uint32_t slot = 0; slot < MAX_SHADOW_DEMAND_SLOTS; slot++) {
			float sample = static_cast<float>(raw[slot]) / demandSumDivisor;  // matches kDemandScale in ShadowDemandCS.hlsl
			float& ema = shadowDemandEMA[slot];
			if (!shadowDemandEMAInitialized || sample > ema)
				ema = sample;  // instant attack
			else
				ema = std::lerp(ema, sample, 0.1f);  // slow decay
		}
		const uint32_t* rawMax = static_cast<const uint32_t*>(mappedMax.pData);
		for (uint32_t e = 0; e < kShadowDemandMaxElements; e++)
			maxCombined[e] = std::max(maxCombined[e], rawMax[e]);

		const auto lag = static_cast<uint32_t>(shadowDemandFrameCounter - shadowDemandRingWriteFrame[i]);
		shadowDemandDrainLagMin = std::min(shadowDemandDrainLagMin, lag);
		shadowDemandDrainLagMax = std::max(shadowDemandDrainLagMax, lag);
		shadowDemandDrainLagSum += lag;
		shadowDemandDrainCount++;

		shadowDemandEMAInitialized = true;
		drainedThisFrame = true;
		context->Unmap(shadowDemandMaxStaging[i]->resource.get(), 0);
		context->Unmap(shadowDemandStaging[i]->resource.get(), 0);
		shadowDemandRingState[i] = ShadowDemandRingState::Idle;
	}

	if (drainedThisFrame) {
		std::copy_n(maxCombined.begin(), MAX_SHADOW_DEMAND_SLOTS, shadowDemandMaxLatest.begin());
		shadowDemandClusterSaturated = maxCombined[MAX_SHADOW_DEMAND_SLOTS] != 0;
		shadowDemandSampleSerial++;
		shadowDemandLastDrainFrame = shadowDemandFrameCounter;
	}

	// Debug-only distribution dump; SetShadowDemand (called every frame from
	// Prepass) is the actual demand-skip consumption path and doesn't need this log.
	if (ShadowDemandInstrumentation && shadowDemandEMAInitialized && shadowDemandFrameCounter - shadowDemandLastLogFrame >= 300) {
		shadowDemandLastLogFrame = shadowDemandFrameCounter;
		auto installedSlotCount = ShadowCasterManager::GetInstalledSlotCount();
		auto count = std::min<uint32_t>(installedSlotCount, MAX_SHADOW_DEMAND_SLOTS);
		if (count > 0) {
			float minV = shadowDemandEMA[0], maxV = shadowDemandEMA[0], sum = 0.0f;
			for (uint32_t i = 0; i < count; i++) {
				minV = std::min(minV, shadowDemandEMA[i]);
				maxV = std::max(maxV, shadowDemandEMA[i]);
				sum += shadowDemandEMA[i];
			}
			logger::info("[SCM] ShadowDemand instrumentation: {} slots, min={:.2f} max={:.2f} mean={:.2f}",
				count, minV, maxV, sum / count);
		}
		// See shadowDemandDrainLagMin's declaration for the frame-rate caveat.
		if (shadowDemandDrainCount > 0) {
			logger::info("[SCM] ShadowDemand drain lag: min={} mean={:.1f} max={} frames over {} drains (saturated={})",
				shadowDemandDrainLagMin,
				static_cast<double>(shadowDemandDrainLagSum) / static_cast<double>(shadowDemandDrainCount),
				shadowDemandDrainLagMax, shadowDemandDrainCount, shadowDemandClusterSaturated);
			shadowDemandDrainLagMin = UINT32_MAX;
			shadowDemandDrainLagMax = 0;
			shadowDemandDrainLagSum = 0;
			shadowDemandDrainCount = 0;
		}
	}
}

namespace
{
	// Defined below with the effect-shader guard.
	bool ProbeReadable(const void* a_ptr, std::size_t a_size);

	// Every unguarded engine read on these types, VR layout: NiLight fade +0x15C,
	// diffuse +0x144, direction through +0x173; BSLight through its NiPointer slot.
	constexpr std::size_t kNiLightEngineReadSize = 0x174;
	constexpr std::size_t kBSLightEngineReadSize = 0x50;

	// Plausibility + committed-readability in one check: the value test rejects
	// null/garbage cheaply, the VirtualQuery probe rejects freed-but-canonical
	// pointers the value test cannot (a recycled light block stays mapped-looking).
	bool IsSafeLightRange(const void* a_ptr, std::size_t a_size)
	{
		return ShadowCasterManager::IsPlausibleShadowLightPtr(reinterpret_cast<std::uintptr_t>(a_ptr)) &&
		       ProbeReadable(a_ptr, a_size);
	}

	// The directional slot is almost always the same sun NiLight for every pass in a
	// frame; revalidating it per pass would put a VirtualQuery syscall on the hottest
	// draw path. Render-thread-only state, revalidated once per pointer per frame.
	bool IsSafeDirectionalNiLight(const RE::NiLight* a_light)
	{
		static const RE::NiLight* validated = nullptr;
		static uint32_t validatedFrame = ~0u;
		const uint32_t frame = globals::state ? globals::state->frameCount : 0;
		if (a_light == validated && frame == validatedFrame)
			return true;
		if (!IsSafeLightRange(a_light, kNiLightEngineReadSize))
			return false;
		validated = a_light;
		validatedFrame = frame;
		return true;
	}

	// Pointer plausibility cannot distinguish a live BSLight from stale mapped memory.
	// Keep the SEH read free of C++ unwinding objects for MSVC's __try restriction.
	RE::NiLight* SafeReadNiLight(RE::BSLight* a_light)
	{
#if defined(_MSC_VER)
		__try {
			return a_light->light.get();
		} __except (1) {
			return nullptr;
		}
#else
		return a_light->light.get();
#endif
	}
}

void LightLimitFix::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	// Engine derefs sceneLights[0]->light with no null check -> CTD on a null/
	// stale directional slot (#92). Skip the engine call when it isn't safe;
	// clamping numLights (sibling guard) can't help -- slot 0 is always read.
	bool directionalSlotSafe = true;
	if (Pass) {
		using ShadowCasterManager::IsPlausibleShadowLightPtr;
		RE::BSLight* dirLight = (Pass->numLights > 0 && Pass->sceneLights) ? Pass->sceneLights[0] : nullptr;
		// A stale-but-canonical dirLight passes the pointer-value check yet still AVs on
		// dirLight->light, so capture the NiLight under SEH and reuse it below (no second deref).
		RE::NiLight* niLight = IsPlausibleShadowLightPtr(reinterpret_cast<std::uintptr_t>(dirLight)) ? SafeReadNiLight(dirLight) : nullptr;
		if (Pass->numLights == 0 || !IsSafeDirectionalNiLight(niLight)) {
			directionalSlotSafe = false;
			// One stale light is hit by many passes per frame; dedupe on the NiLight value
			// so a single bad light logs once, not once per draw. Bounded total either way.
			// A separate first-log flag avoids suppressing an initial value that happens to
			// equal any chosen sentinel (garbage slots can be small integers like 0x1).
			static bool everLogged = false;
			static std::uintptr_t lastLoggedNiLight = 0;
			static int distinctLogged = 0;
			const auto niLightVal = reinterpret_cast<std::uintptr_t>(niLight);
			if ((!everLogged || niLightVal != lastLoggedNiLight) && distinctLogged++ < 20) {
				everLogged = true;
				lastLoggedNiLight = niLightVal;
				logger::warn(
					"[LLF] BSLightingShader_SetupGeometry: directional sceneLights[0] unsafe "
					"(numLights={} BSLight=0x{:x} NiLight=0x{:x}); skipping engine SetupGeometry "
					"to avoid GeometrySetupConstantDirectionalLight null-deref (#92)",
					Pass->numLights,
					reinterpret_cast<std::uintptr_t>(dirLight),
					niLightVal);
			}
		}
	}

	// Run before/after even on skip so the strict-light CB is reset, not stale.
	auto& singleton = globals::features::lightLimitFix;
	singleton.BSLightingShader_SetupGeometry_Before(Pass);
	if (directionalSlotSafe)
		func(This, Pass, RenderFlags);
	singleton.BSLightingShader_SetupGeometry_After(Pass);
}

namespace
{
	// VirtualQuery readability probe -- the environment-independent check the address-floor
	// heuristic can't be: asks the OS whether [ptr, ptr+size) is committed + readable, rejecting
	// a freed light at ANY address without guessing the heap base. Tracy zone LLF::EffectLightProbe.
	bool IsReadableRange(const void* a_ptr, std::size_t a_size) noexcept
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (::VirtualQuery(a_ptr, &mbi, sizeof(mbi)) == 0)
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
		                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if ((mbi.Protect & kReadable) == 0 || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
			return false;
		const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
		const auto p = reinterpret_cast<std::uintptr_t>(a_ptr);
		return (p + a_size) <= (base + mbi.RegionSize);  // whole range in this committed region
	}

	bool ProbeReadable(const void* a_ptr, std::size_t a_size)
	{
		ZoneScopedN("LLF::EffectLightProbe");
		return IsReadableRange(a_ptr, a_size);
	}
}

void LightLimitFix::Hooks::BSEffectShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	// Validate each raw scene-light pair on first use per frame; cache hits still SEH-read the
	// BSLight field so unmapped or changed entries fall back to the full guard.
	if (Pass && Pass->sceneLights && Pass->numLights > 0) {
		std::uint8_t validCount = 0;
		auto& validationCache = globals::features::lightLimitFix.effectLightValidationCache;
		for (std::uint8_t i = 0; i < Pass->numLights; ++i) {
			RE::BSLight* bsLight = Pass->sceneLights[i];
			if (const auto cached = validationCache.find(bsLight); cached != validationCache.end()) {
				const auto currentNiLight = SafeReadNiLight(bsLight);
				if (currentNiLight == cached->second) {
					++validCount;
					continue;
				}
				validationCache.erase(cached);
			}

			if (!IsSafeLightRange(bsLight, kBSLightEngineReadSize)) {
				static int loggedBsLight = 0;
				if (loggedBsLight++ < 10) {
					logger::warn(
						"[LLF] BSEffectShader_SetupGeometry: bad BSLight* at "
						"sceneLights[{}]=0x{:x} numLights={}; clamping to {}",
						i, reinterpret_cast<std::uintptr_t>(bsLight), Pass->numLights, validCount);
				}
				break;
			}
			RE::NiLight* niLight = SafeReadNiLight(bsLight);
			if (!IsSafeLightRange(niLight, kNiLightEngineReadSize)) {
				// Catches both NULL (engine cleared the NiPointer) and
				// garbage (BSLight memory recycled). NULL is the more common
				// observed failure -- the engine's loop has no null check
				// before reading [+0x134].
				static int loggedNiLight = 0;
				if (loggedNiLight++ < 10) {
					logger::warn(
						"[LLF] BSEffectShader_SetupGeometry: bad NiLight at "
						"sceneLights[{}] (BSLight=0x{:x} NiLight=0x{:x}); clamping to {}",
						i,
						reinterpret_cast<std::uintptr_t>(bsLight),
						reinterpret_cast<std::uintptr_t>(niLight),
						validCount);
				}
				break;
			}

			validationCache.insert_or_assign(bsLight, niLight);
			++validCount;
		}
		if (validCount < Pass->numLights)
			Pass->numLights = validCount;
	}

	func(This, Pass, RenderFlags);
	ExternalEmittance::UpdatePermutation(Pass);
	auto& singleton = globals::features::lightLimitFix;
	singleton.BSLightingShader_SetupGeometry_Before(Pass);
	singleton.BSLightingShader_SetupGeometry_After(Pass);
}

void LightLimitFix::Hooks::BSWaterShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	// CloudShadows leaves cubemap depth in t17 without restoring it; rebind the 16-bit
	// scene depth the contact-shadow raymarch expects.
	auto* srv = Util::GetCurrentSceneDepthSRV(true);
	globals::d3d::context->PSSetShaderResources(17, 1, &srv);
	auto& singleton = globals::features::lightLimitFix;
	singleton.BSLightingShader_SetupGeometry_Before(Pass);
	singleton.BSLightingShader_SetupGeometry_After(Pass);
}
