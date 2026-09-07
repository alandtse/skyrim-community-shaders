#include "ScreenSpaceGI.h"
#include "GpuPass.h"

#include <DirectXTex.h>

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "Features/VR.h"
#include "State.h"
#include "Util.h"
#include "Utils/Game.h"

#define I18N_KEY_PREFIX "feature.screen_space_gi."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceGI::Settings,
	Enabled,
	EnableGI,
	EnableExperimentalSpecularGI,
	EnableVanillaSSAO,
	NumSlices,
	NumSteps,
	ResolutionMode,
	ResourceProfile,
	EnableAdaptiveSampling,
	DebugUseUnjitteredCameraReconstruction,
	MinScreenRadius,
	AORadius,
	GIRadius,
	Thickness,
	DepthFadeRange,
	GISaturation,
	GIDistanceCompensation,
	AOPower,
	GIStrength,
	EnableTemporalDenoiser,
	EnableBlur,
	DepthDisocclusion,
	NormalDisocclusion,
	MaxAccumFrames,
	BlurRadius,
	DistanceNormalisation,
	UseStereoReproject)

////////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceGI::RestoreDefaultSettings()
{
	settings = {};
	recompileFlag = true;
}

void ScreenSpaceGI::DrawReprojectToggle()
{
	auto reprojectGuard = Util::DisableGuard(settings.EnableExperimentalSpecularGI);
	ImGui::Checkbox(T(TKEY("vr_stereo_reproject"), "Stereo Reprojection"), &settings.UseStereoReproject);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("vr_stereo_reproject_tooltip"),
							  "Reprojects Eye 0 (left)'s diffuse GI into Eye 1 (right) and skips the Eye 1 "
							  "march where it can, reducing GPU cost. Requires HQ Specular IL off (specular "
							  "is view-dependent). Disoccluded pixels are marched natively instead."));
}

// Hub view: the SSGI stereo reprojection toggle, bound to the same setting the SSGI panel shows.
void ScreenSpaceGI::DrawPerformanceSettings()
{
	DrawReprojectToggle();
}

// Reprojection is the GI perf win with a minor disocclusion artifact: on for
// Performance/Balanced, off for Quality (max fidelity). Ignored when specular GI is on.
void ScreenSpaceGI::ApplyPerformanceProfile(PerfProfile profile)
{
	settings.UseStereoReproject = ProfileEnablesReproject(profile);
}

bool ScreenSpaceGI::MatchesPerformanceProfile(PerfProfile profile) const
{
	// Specular GI forces bilateral sync (view-dependent), so the reproject setting is moot
	// then, so don't veto the hub's active-profile detection for a knob the user can't apply.
	return settings.EnableExperimentalSpecularGI || settings.UseStereoReproject == ProfileEnablesReproject(profile);
}

void ScreenSpaceGI::DrawSettings()
{
	static bool showAdvanced;

	if (!ShadersOK())
		Util::Text::Error("%s", T(TKEY("shader_compile_error"), "Compute shaders failed to compile!"));

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("toggles"), "Toggles"));

	ImGui::Checkbox(T(TKEY("show_advanced"), "Show Advanced Options"), &showAdvanced);

	ImGui::SeparatorText(T(TKEY("effects_resources"), "Effects & Resources"));

	const int previousResourceProfile = settings.ResourceProfile;
	if (ImGui::BeginTable("ResourcesTable", 2)) {
		ImGui::TableNextColumn();
		if (ImGui::RadioButton(T(TKEY("profile_ao_only"), "AO-only Resources"), settings.ResourceProfile == kResourceProfileAOOnly)) {
			settings.ResourceProfile = kResourceProfileAOOnly;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("profile_ao_only_tooltip"), "Uses less video memory but disables indirect lighting. Requires a game restart to change."));
		}

		ImGui::TableNextColumn();
		if (ImGui::RadioButton(T(TKEY("profile_ao_gi"), "AO + GI Resources"), settings.ResourceProfile == kResourceProfileFullGI)) {
			settings.ResourceProfile = kResourceProfileFullGI;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("profile_ao_gi_tooltip"), "Enables full indirect lighting at the cost of more video memory. Requires a game restart to change."));
		}
		ImGui::EndTable();
	}

	if (settings.ResourceProfile != previousResourceProfile) {
		if (settings.ResourceProfile == kResourceProfileAOOnly) {
			settings.EnableGI = false;
			settings.EnableExperimentalSpecularGI = false;
		}
		recompileFlag = true;
	}

	Util::UI::DrawSettingDiff(bootSnapshot, settings, &Settings::ResourceProfile);

	if (ImGui::BeginTable("Toggles", 4)) {
		ImGui::TableNextColumn();
		ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enabled_tooltip"), "Enable Screen Space Global Illumination. When disabled, all other settings are ignored."));
		}

		ImGui::TableNextColumn();
		{
			auto ilToggleGuard = Util::DisableGuard(!settings.Enabled);
			recompileFlag |= ImGui::Checkbox(T(TKEY("indirect_lighting"), "Indirect Lighting (IL)"), &settings.EnableGI);
			// GI resources are boot-latched to the profile, so checking IL before a
			// restart is a no-op until the profile below is picked up.
			if (settings.EnableGI && !HasGIResources())
				Util::Text::RestartNeeded("%s", T(TKEY("indirect_lighting_pending"), "Pending restart: needs AO + GI Resources."));
		}
		ImGui::TableNextColumn();
		{
			auto vanillaSSAOGuard = Util::DisableGuard(globals::game::isVR);
			ImGui::Checkbox(T(TKEY("vanilla_ssao"), "Vanilla SSAO"), &settings.EnableVanillaSSAO);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				if (globals::game::isVR)
					ImGui::Text("%s", T(TKEY("vanilla_ssao_tooltip_vr"), "Vanilla SSAO is not supported in VR."));
				else
					ImGui::Text("%s", T(TKEY("vanilla_ssao_tooltip"), "Enable Skyrim's built-in SSAO. Usually disabled when using SSGI to avoid double-darkening."));
			}
		}
		ImGui::TableNextColumn();
		if (showAdvanced) {
			{
				auto hqSpecGuard = Util::DisableGuard(!settings.EnableGI);
				recompileFlag |= ImGui::Checkbox(T(TKEY("hq_specular_il"), "(Experimental) HQ Specular IL"), &settings.EnableExperimentalSpecularGI);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("hq_specular_il_tooltip"), "An experimental specular GI that is more accurate but requires more samples. Won't be blurred."));
			}

			if (globals::game::isVR)
				DrawReprojectToggle();
		}

		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("quality_performance"), "Quality/Performance"));

	{
		auto qualityGuard = Util::DisableGuard(!settings.Enabled);

		if (ImGui::BeginTable("Presets", 5)) {
			auto select = [](auto flatVal, auto vrVal) { return globals::game::isVR ? vrVal : flatVal; };

			ImGui::TableNextColumn();
			if (ImGui::Button(T(TKEY("ao_only"), "AO only"), { -1, 0 })) {
				settings.NumSlices = select(1, 3);
				settings.NumSteps = select(6, 8);
				settings.EnableBlur = true;
				settings.ResourceProfile = kResourceProfileAOOnly;
				settings.EnableGI = false;
				settings.EnableExperimentalSpecularGI = false;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(select("1 Slice, 6 Steps, blur enabled, no GI\n", "3 Slices, 8 Steps, blur enabled, no GI\n"));
			}

			ImGui::TableNextColumn();
			if (ImGui::Button(T(TKEY("low"), "Low"), { -1, 0 })) {
				settings.NumSlices = 10;
				settings.NumSteps = 12;
				settings.ResolutionMode = 2;
				settings.EnableBlur = true;
				settings.ResourceProfile = kResourceProfileFullGI;
				settings.EnableGI = true;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("low_tooltip"), "Quarter res and blurry."));

			ImGui::TableNextColumn();
			if (ImGui::Button(T(TKEY("standard"), "Standard"), { -1, 0 })) {
				settings.NumSlices = 4;
				settings.NumSteps = 8;
				settings.ResolutionMode = 1;
				settings.EnableBlur = true;
				settings.ResourceProfile = kResourceProfileFullGI;
				settings.EnableGI = true;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("standard_tooltip"), "Half res and somewhat stable."));

			ImGui::TableNextColumn();
			if (ImGui::Button(T(TKEY("extreme"), "Extreme"), { -1, 0 })) {
				settings.NumSlices = 4;
				settings.NumSteps = 8;
				settings.ResolutionMode = 0;
				settings.EnableBlur = true;
				settings.ResourceProfile = kResourceProfileFullGI;
				settings.EnableGI = true;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("extreme_tooltip"), "Full res and clean."));

			ImGui::TableNextColumn();
			if (ImGui::Button(T(TKEY("reference"), "Reference"), { -1, 0 })) {
				settings.NumSlices = 8;
				settings.NumSteps = 10;
				settings.ResolutionMode = 0;
				settings.EnableBlur = true;
				settings.ResourceProfile = kResourceProfileFullGI;
				settings.EnableGI = true;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("reference_tooltip"), "Reference mode."));

			ImGui::EndTable();
		}

		if (showAdvanced) {
			ImGui::SliderInt(T(TKEY("slices"), "Slices"), (int*)&settings.NumSlices, 1, 10);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("slices_tooltip"),
									  "How many directions do the samples take.\n"
									  "Controls noise."));

			ImGui::SliderInt(T(TKEY("steps_per_slice"), "Steps Per Slice"), (int*)&settings.NumSteps, 1, 20);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("steps_per_slice_tooltip"),
									  "How many samples does it take in one direction.\n"
									  "Controls accuracy of lighting, and noise when effect radius is large."));

			recompileFlag |= ImGui::Checkbox(T(TKEY("adaptive_sampling"), "Adaptive Sampling"), &settings.EnableAdaptiveSampling);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("adaptive_sampling_tooltip"), "Reduces steps on distant or flat surfaces. Adds a depth/normal variance pass; net cost depends on scene."));
		}

		if (ImGui::BeginTable("Less Work", 3)) {
			ImGui::TableNextColumn();
			recompileFlag |= ImGui::RadioButton(T(TKEY("full_res"), "Full Res"), &settings.ResolutionMode, 0);
			ImGui::TableNextColumn();
			recompileFlag |= ImGui::RadioButton(T(TKEY("half_res"), "Half Res"), &settings.ResolutionMode, 1);
			ImGui::TableNextColumn();
			recompileFlag |= ImGui::RadioButton(T(TKEY("quarter_res"), "Quarter Res"), &settings.ResolutionMode, 2);

			ImGui::EndTable();
		}
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("visual"), "Visual"));

	{
		auto visualGuard = Util::DisableGuard(!settings.Enabled);

		ImGui::SliderFloat(T(TKEY("ao_power"), "AO Power"), &settings.AOPower, 0.f, 6.f, "%.2f");

		{
			auto ilGuard = Util::DisableGuard(!settings.EnableGI);
			ImGui::SliderFloat(T(TKEY("il_source_brightness"), "IL Source Brightness"), &settings.GIStrength, 0.f, 6.f, "%.2f");
		}

		ImGui::Separator();

		ImGui::SliderFloat(T(TKEY("ao_radius"), "AO radius"), &settings.AORadius, 10.f, 1024.0f, "%.1f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			std::vector<std::string> tooltipLines = {
				T(TKEY("ao_radius_tooltip"), "A smaller radius produces tighter AO."),
				Util::Units::FormatDistance(settings.AORadius)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		{
			auto ilRadiusGuard = Util::DisableGuard(!settings.EnableGI);

			ImGui::SliderFloat(T(TKEY("il_radius"), "IL radius"), &settings.GIRadius, 10.f, 1024.0f, "%.1f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					T(TKEY("il_radius_tooltip"), "A larger radius produces wider IL."),
					Util::Units::FormatDistance(settings.GIRadius)
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
		}

		if (showAdvanced) {
			ImGui::SliderFloat(T(TKEY("min_screen_radius"), "Min Screen Radius"), &settings.MinScreenRadius, 0.f, 0.05f, "%.3f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("min_screen_radius_tooltip"),
									  "The minimum screen-space effect radius as proportion of display width, to prevent far field AO being too small."));
		}

		ImGui::SliderFloat2(T(TKEY("depth_fade_range"), "Depth Fade Range"), &settings.DepthFadeRange.x, 1e4, 5e4, "%.0f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			std::vector<std::string> tooltipLines = {
				T(TKEY("depth_fade_range_tooltip"), "Distance range where depth-based effects fade out."),
				"Near: " + Util::Units::FormatDistance(settings.DepthFadeRange.x),
				"Far: " + Util::Units::FormatDistance(settings.DepthFadeRange.y)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		if (showAdvanced) {
			ImGui::Separator();

			ImGui::SliderFloat(T(TKEY("thickness"), "Thickness"), &settings.Thickness, 0.f, 128.0f, "%.1f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					T(TKEY("thickness_tooltip"), "How thick the occluders are. Only affects AO."),
					Util::Units::FormatDistance(settings.Thickness)
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("visual_il"), "Visual - IL"));

	{
		auto visualILGuard = Util::DisableGuard(!settings.Enabled || !settings.EnableGI);

		if (showAdvanced) {
			ImGui::SliderFloat(T(TKEY("il_distance_compensation"), "IL Distance Compensation"), &settings.GIDistanceCompensation, -5.0f, 5.0f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("il_distance_compensation_tooltip"), "Brighten/Dimming further radiance samples."));

			ImGui::Separator();
		}

		Util::PercentageSlider(T(TKEY("il_saturation"), "IL Saturation"), &settings.GISaturation);
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("denoising"), "Denoising"));

	{
		auto denoiseGuard = Util::DisableGuard(!settings.Enabled);

		if (ImGui::BeginTable("denoisers", 2)) {
			ImGui::TableNextColumn();
			recompileFlag |= ImGui::Checkbox(T(TKEY("temporal_denoiser"), "Temporal Denoiser"), &settings.EnableTemporalDenoiser);

			ImGui::TableNextColumn();
			ImGui::Checkbox(T(TKEY("blur"), "Blur"), &settings.EnableBlur);

			ImGui::EndTable();
		}

		if (showAdvanced) {
			ImGui::Separator();

			{
				auto temporalGuard = Util::DisableGuard(!settings.EnableTemporalDenoiser);
				ImGui::SliderInt(T(TKEY("max_frame_accumulation"), "Max Frame Accumulation"), (int*)&settings.MaxAccumFrames, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("max_frame_accumulation_tooltip"), "How many past frames to accumulate results with. Higher values are less noisy but potentially cause ghosting."));
			}

			ImGui::Separator();

			{
				auto disocclusionGuard = Util::DisableGuard(!settings.EnableTemporalDenoiser && !settings.EnableGI);

				Util::PercentageSlider(T(TKEY("movement_disocclusion"), "Movement Disocclusion"), &settings.DepthDisocclusion, 0.f, 20.f);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("movement_disocclusion_tooltip"),
										  "If a pixel has moved too far from the last frame, its radiance will not be carried to this frame.\n"
										  "Lower values are stricter."));

				ImGui::Separator();
			}

			{
				auto blurGuard = Util::DisableGuard(!settings.EnableBlur);
				ImGui::SliderFloat(T(TKEY("blur_radius"), "Blur Radius"), &settings.BlurRadius, 0.f, 30.f, "%.1f px");

				if (showAdvanced) {
					ImGui::SliderFloat(T(TKEY("geometry_weight"), "Geometry Weight"), &settings.DistanceNormalisation, 0.f, 5.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("%s", T(TKEY("geometry_weight_tooltip"),
											  "Higher value makes the blur more sensitive to differences in geometry."));
				}
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText(T(TKEY("debug"), "Debug"));

	if (globals::game::isVR && showAdvanced) {
		ImGui::Checkbox(T(TKEY("debug_unjittered_vr"), "Use Unjittered VR Camera"), &settings.DebugUseUnjitteredCameraReconstruction);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("debug_unjittered_vr_tooltip"), "May reduce jitter-induced depth swim during head movement. Compare against the default before enabling."));
		}
	}

	if (ImGui::TreeNode(T(TKEY("buffer_viewer"), "Buffer Viewer"))) {
		static float debugRescale = .3f;
		ImGui::SliderFloat(T(TKEY("view_resize"), "View Resize"), &debugRescale, 0.f, 1.f);

		BUFFER_VIEWER_NODE(texNoise, debugRescale)
		BUFFER_VIEWER_NODE(texWorkingDepth, debugRescale)
		BUFFER_VIEWER_NODE(texPrevGeo, debugRescale)
		BUFFER_VIEWER_NODE(texRadiance, debugRescale)
		BUFFER_VIEWER_NODE(texAo[0], debugRescale)
		BUFFER_VIEWER_NODE(texAo[1], debugRescale)
		BUFFER_VIEWER_NODE(texIlY[0], debugRescale)
		BUFFER_VIEWER_NODE(texIlY[1], debugRescale)
		BUFFER_VIEWER_NODE(texIlCoCg[0], debugRescale)
		BUFFER_VIEWER_NODE(texIlCoCg[1], debugRescale)

		ImGui::TreePop();
	}
}

void ScreenSpaceGI::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.ResolutionMode = std::clamp(settings.ResolutionMode, 0, 2);
	if (!o_json.contains("ResourceProfile")) {
		// Existing VR configs keep full resources if GI was active, else use lean AO-only.
		settings.ResourceProfile = (REL::Module::IsVR() && !settings.EnableGI) ? kResourceProfileAOOnly : kResourceProfileFullGI;
	}
	recompileFlag = true;
}

void ScreenSpaceGI::SaveSettings(json& o_json)
{
	o_json = settings;
}

RE::BSEventNotifyControl ScreenSpaceGI::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening)
		globals::features::screenSpaceGI.queuedResetHistory = true;

	return RE::BSEventNotifyControl::kContinue;
}

bool ScreenSpaceGI::MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	auto ui = globals::game::ui;

	if (!ui) {
		logger::error("UI event source not found");
		return false;
	}

	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);
	return true;
}

void ScreenSpaceGI::PostPostLoad()
{
	MenuOpenCloseEventHandler::Register();
}

void ScreenSpaceGI::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	bootSnapshot.LatchIfNeeded(settings);

	activeResourceProfile = std::clamp(settings.ResourceProfile, kResourceProfileFullGI, kResourceProfileAOOnly);
	const bool allocateGIResources = HasGIResources();
	logger::info("SSGI resource profile: {}", allocateGIResources ? "Full GI resources" : "AO-only resources");

	logger::debug("Creating buffers...");
	{
		ssgiCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSGICB>(), "SSGI::CB");
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 64,
			.Height = 64,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32_UINT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		if (allocateGIResources) {
			srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
			texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;

			texRadiance = eastl::make_unique<Texture2D>(texDesc, "SSGI::Radiance");
			texRadiance->CreateSRV(srvDesc);
			// No default UAV needed: prefilterRadiance binds per-mip UAVs via uavRadiance[].

			// Create individual UAVs for each mip level for prefiltering
			for (uint i = 0; i < 5; ++i) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
					.Format = DXGI_FORMAT_R11G11B10_FLOAT,
					.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MipSlice = i }
				};
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texRadiance->resource.get(), &mipUavDesc, uavRadiance[i].put()));
				Util::SetResourceName(uavRadiance[i].get(), "SSGI::Radiance UAV mip%u", i);
			}

			// Staging texture for mip 0 radiance. radianceDisocc writes it directly,
			// prefilterRadiance reads it as SRV and writes the mip chain back to texRadiance.
			// Avoids a full-texture CopySubresourceRegion each frame.
			D3D11_TEXTURE2D_DESC tempTexDesc = texDesc;
			tempTexDesc.MipLevels = 1;
			tempTexDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			D3D11_SHADER_RESOURCE_VIEW_DESC tempSrvDesc = {
				.Format = DXGI_FORMAT_R11G11B10_FLOAT,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = {
					.MostDetailedMip = 0,
					.MipLevels = 1 }
			};

			D3D11_UNORDERED_ACCESS_VIEW_DESC tempUavDesc = {
				.Format = DXGI_FORMAT_R11G11B10_FLOAT,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};

			texRadianceTemp = eastl::make_unique<Texture2D>(tempTexDesc, "SSGI::RadianceTemp");
			texRadianceTemp->CreateSRV(tempSrvDesc);
			texRadianceTemp->CreateUAV(tempUavDesc);
		}

		texDesc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16_FLOAT;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;

		{
			texWorkingDepth = eastl::make_unique<Texture2D>(texDesc, "SSGI::WorkingDepth");
			texWorkingDepth->CreateSRV(srvDesc);
			for (int i = 0; i < 5; ++i) {
				uavDesc.Texture2D.MipSlice = i;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth[i].put()));
				Util::SetResourceName(uavWorkingDepth[i].get(), "SSGI::WorkingDepth UAV mip%d", i);
			}
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		{
			texNormal = eastl::make_unique<Texture2D>(texDesc, "SSGI::Normal");
			texNormal->CreateSRV(srvDesc);
			for (uint i = 0; i < 5; ++i) {
				uavDesc.Texture2D.MipSlice = i;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texNormal->resource.get(), &uavDesc, uavNormal[i].put()));
				Util::SetResourceName(uavNormal[i].get(), "SSGI::Normal UAV mip%u", i);
			}
		}

		uavDesc.Texture2D.MipSlice = 0;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		if (allocateGIResources) {
			srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			{
				texIlY[0] = eastl::make_unique<Texture2D>(texDesc, "SSGI::IlY[0]");
				texIlY[0]->CreateSRV(srvDesc);
				texIlY[0]->CreateUAV(uavDesc);

				texIlY[1] = eastl::make_unique<Texture2D>(texDesc, "SSGI::IlY[1]");
				texIlY[1]->CreateSRV(srvDesc);
				texIlY[1]->CreateUAV(uavDesc);

				texGiSpecular[0] = eastl::make_unique<Texture2D>(texDesc, "SSGI::GiSpecular[0]");
				texGiSpecular[0]->CreateSRV(srvDesc);
				texGiSpecular[0]->CreateUAV(uavDesc);

				texGiSpecular[1] = eastl::make_unique<Texture2D>(texDesc, "SSGI::GiSpecular[1]");
				texGiSpecular[1]->CreateSRV(srvDesc);
				texGiSpecular[1]->CreateUAV(uavDesc);
			}
			srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
			{
				texIlCoCg[0] = eastl::make_unique<Texture2D>(texDesc, "SSGI::IlCoCg[0]");
				texIlCoCg[0]->CreateSRV(srvDesc);
				texIlCoCg[0]->CreateUAV(uavDesc);

				texIlCoCg[1] = eastl::make_unique<Texture2D>(texDesc, "SSGI::IlCoCg[1]");
				texIlCoCg[1]->CreateSRV(srvDesc);
				texIlCoCg[1]->CreateUAV(uavDesc);
			}
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R8_UNORM;
		{
			texAo[0] = eastl::make_unique<Texture2D>(texDesc, "SSGI::AO[0]");
			texAo[0]->CreateSRV(srvDesc);
			texAo[0]->CreateUAV(uavDesc);

			texAo[1] = eastl::make_unique<Texture2D>(texDesc, "SSGI::AO[1]");
			texAo[1]->CreateSRV(srvDesc);
			texAo[1]->CreateUAV(uavDesc);

			texAccumFrames[0] = eastl::make_unique<Texture2D>(texDesc, "SSGI::AccumFrames[0]");
			texAccumFrames[0]->CreateSRV(srvDesc);
			texAccumFrames[0]->CreateUAV(uavDesc);

			texAccumFrames[1] = eastl::make_unique<Texture2D>(texDesc, "SSGI::AccumFrames[1]");
			texAccumFrames[1]->CreateSRV(srvDesc);
			texAccumFrames[1]->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		{
			texPrevGeo = eastl::make_unique<Texture2D>(texDesc, "SSGI::PrevGeo");
			texPrevGeo->CreateSRV(srvDesc);
			texPrevGeo->CreateUAV(uavDesc);
		}
	}

	logger::debug("Loading noise texture...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ "Data\\Shaders\\ScreenSpaceGI\\fast_2uges.dds" };

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texNoise = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource), "SSGI::Noise");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texNoise->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texNoise->CreateSRV(srvDesc);
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));
		Util::SetResourceName(linearClampSampler.get(), "SSGI::LinearClampSampler");

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
		Util::SetResourceName(pointClampSampler.get(), "SSGI::PointClampSampler");
	}

	CompileComputeShaders();
}

void ScreenSpaceGI::ClearShaderCache()
{
	Util::ClearShaders<ID3D11ComputeShader>({ prefilterDepthsCompute, prefilterRadianceCompute, prefilterNormalCompute, radianceDisoccCompute, giCompute, giEye0OnlyCompute, blurCompute, stereoSyncCompute, reprojectCompute, reprojectDebugCompute, upsampleCompute });

	CompileComputeShaders();
}

void ScreenSpaceGI::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	std::vector<ShaderCompileInfo> shaderInfos;
	shaderInfos.push_back({ &prefilterDepthsCompute, "prefilterDepths.cs.hlsl", { { "LINEAR_FILTER", "" } } });
	shaderInfos.push_back({ &prefilterNormalCompute, "prefilterNormal.cs.hlsl", {} });
	shaderInfos.push_back({ &radianceDisoccCompute, "radianceDisocc.cs.hlsl", {} });
	shaderInfos.push_back({ &giCompute, "gi.cs.hlsl", {} });
	shaderInfos.push_back({ &upsampleCompute, "upsample.cs.hlsl", {} });

	// The GI-only passes (radiance prefilter, IL blur) never dispatch on the AO-only profile.
	if (HasGIResources()) {
		shaderInfos.push_back({ &prefilterRadianceCompute, "prefilterRadiance.cs.hlsl", {} });
		shaderInfos.push_back({ &blurCompute, "blur.cs.hlsl", {} });
	}

	if (globals::game::isVR) {
		shaderInfos.push_back({ &stereoSyncCompute, "stereoSync.cs.hlsl", { { "FRAMEBUFFER", "" } } });
		shaderInfos.push_back({ &reprojectCompute, "reproject.cs.hlsl", { { "FRAMEBUFFER", "" } } });
		shaderInfos.push_back({ &reprojectDebugCompute, "reproject.cs.hlsl", { { "FRAMEBUFFER", "" }, { "DEBUG_DISOCCLUSION", "" } } });
		// Eye-0-only GI permutation for the reproject path. FRAMEBUFFER exposes the
		// Stereo:: reprojection helpers (gated out of VR.hlsli for plain compute). Only
		// meaningful with specular off (the reproject transfers diffuse GI); skip the
		// unused specular combination.
		if (!settings.EnableExperimentalSpecularGI)
			shaderInfos.push_back({ &giEye0OnlyCompute, "gi.cs.hlsl", { { "STEREO_EYE0_ONLY", "" }, { "FRAMEBUFFER", "" } } });
	}
	for (auto& info : shaderInfos) {
		if (globals::game::isVR)
			info.defines.push_back({ "VR", "" });
		if (settings.ResolutionMode == 1)
			info.defines.push_back({ "HALF_RES", "" });
		if (settings.ResolutionMode == 2)
			info.defines.push_back({ "QUARTER_RES", "" });
		if (settings.EnableTemporalDenoiser)
			info.defines.push_back({ "TEMPORAL_DENOISER", "" });
		// Key on the active profile, not the raw toggles: a hand-edited config can
		// pair GI-on with AO-only resources, and compiling GI paths would pay the
		// full march against null views for silently discarded output.
		if (IsGIActive())
			info.defines.push_back({ "GI", "" });
		if (IsSpecularGIActive())
			info.defines.push_back({ "GI_SPECULAR", "" });
		if (settings.EnableAdaptiveSampling && info.filename == "gi.cs.hlsl")
			info.defines.push_back({ "ADAPTIVE_SAMPLING", "" });
	}

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceGI") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
}

bool ScreenSpaceGI::ShadersOK()
{
	const bool baseShadersOK = texNoise &&
	                           prefilterDepthsCompute &&
	                           prefilterNormalCompute &&
	                           radianceDisoccCompute &&
	                           giCompute &&
	                           upsampleCompute;

	const bool giShadersOK = !HasGIResources() || (prefilterRadianceCompute && blurCompute);

	const bool vrShadersOK = !globals::game::isVR || (stereoSyncCompute && reprojectCompute);

	return baseShadersOK && giShadersOK && vrShadersOK;
}

void ScreenSpaceGI::UpdateSB()
{
	float2 res = HasGIResources() ?
	                 float2{ (float)texRadiance->desc.Width, (float)texRadiance->desc.Height } :
	                 float2{ (float)texWorkingDepth->desc.Width, (float)texWorkingDepth->desc.Height };
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = float2{ floor(dynres.x), floor(dynres.y) };

	static float4x4 prevInvView[2] = {};

	SSGICB data;
	{
		// The game's jittered TAA matrices make the depth reconstruction swim in VR.
		const bool useUnjitteredCamera = globals::game::isVR && settings.DebugUseUnjitteredCameraReconstruction;

		for (int eyeIndex = 0; eyeIndex < (1 + globals::game::isVR); ++eyeIndex) {
			const auto eye = Util::GetCameraData(eyeIndex);
			float proj11 = eye.projMat(0, 0);
			float proj22 = eye.projMat(1, 1);
			float4x4 currentInvView = eye.viewMat.Invert();

			if (useUnjitteredCamera) {
				const auto& projUnjittered = globals::game::frameBufferCached.GetCameraProjUnjittered(eyeIndex);
				proj11 = projUnjittered._11;
				proj22 = projUnjittered._22;
				currentInvView = globals::game::frameBufferCached.GetCameraViewInverse(eyeIndex);
			}

			data.PrevInvViewMat[eyeIndex] = prevInvView[eyeIndex];
			data.NDCToViewMul[eyeIndex] = float2{ 2.0f / proj11, -2.0f / proj22 };
			data.NDCToViewAdd[eyeIndex] = float2{ -1.0f / proj11, 1.0f / proj22 };
			if (globals::game::isVR)
				data.NDCToViewMul[eyeIndex].x *= 2;

			prevInvView[eyeIndex] = currentInvView;
		}

		data.TexDim = res;
		data.RcpTexDim = float2(1.0f) / res;
		data.FrameDim = dynres;
		data.RcpFrameDim = float2(1.0f) / dynres;
		data.FrameIndex = globals::state->frameCount;

		data.NumSlices = settings.NumSlices;
		data.NumSteps = settings.NumSteps;
		data.MinScreenRadius = settings.MinScreenRadius * dynres.x;

		data.EffectRadius = std::max(settings.AORadius, settings.GIRadius);
		data.AORadius = settings.AORadius / data.EffectRadius;
		data.GIRadius = settings.GIRadius / data.EffectRadius;
		data.Thickness = settings.Thickness;
		data.DepthFadeRange = settings.DepthFadeRange;
		data.DepthFadeScaleConst = 1 / (settings.DepthFadeRange.y - settings.DepthFadeRange.x);

		data.GISaturation = settings.GISaturation;
		data.GIDistanceCompensation = settings.GIDistanceCompensation;
		data.GICompensationMaxDist = settings.AORadius;

		data.AOPower = settings.AOPower;
		data.GIStrength = settings.GIStrength;

		data.DepthDisocclusion = settings.DepthDisocclusion;
		data.NormalDisocclusion = settings.NormalDisocclusion;
		data.MaxAccumFrames = settings.MaxAccumFrames;
		data.BlurRadius = settings.BlurRadius;
		data.DistanceNormalisation = settings.DistanceNormalisation;
		useModeTextureThisFrame = settings.UseStereoReproject && globals::features::vr.stereoOpt.CanExternallyConsumeClassification();
		data.UseModeTexture = useModeTextureThisFrame;
	}

	ssgiCB->Update(data);
}

void ScreenSpaceGI::DrawSSGI()
{
	auto context = globals::d3d::context;

	if (auto* setting = RE::GetINISetting("bSAOEnable:Display"))
		setting->data.b = settings.EnableVanillaSSAO;

	// Also write the live SAO params object so the toggle applies this frame instead
	// of only at the next ImageSpaceManager reinit.
	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER_VRPTR(BSImagespaceShaderISSAOBlurH, imageSpaceManager);
	if (auto* sao = BSImagespaceShaderISSAOBlurH)
		sao->enableSAO = settings.EnableVanillaSSAO;

	if (!(settings.Enabled && ShadersOK())) {
		FLOAT clr[4] = { 0.f, 0.f, 0.f, 0.f };
		if (texAo[outputAoIdx])
			context->ClearUnorderedAccessViewFloat(texAo[outputAoIdx]->uav.get(), clr);
		if (texIlY[outputIlIdx])
			context->ClearUnorderedAccessViewFloat(texIlY[outputIlIdx]->uav.get(), clr);
		if (texIlCoCg[outputIlIdx])
			context->ClearUnorderedAccessViewFloat(texIlCoCg[outputIlIdx]->uav.get(), clr);
		return;
	}

	const bool runILPath = IsGIActive();

	// Full-profile resources stay allocated with GI off, so the composite keeps
	// sampling these UAVs even though DrawSSGI no longer writes them -- without
	// this they freeze on the last lit frame instead of reading as AO-only.
	if (HasGIResources() && !runILPath) {
		FLOAT clr[4] = { 0.f, 0.f, 0.f, 0.f };
		context->ClearUnorderedAccessViewFloat(texIlY[outputIlIdx]->uav.get(), clr);
		context->ClearUnorderedAccessViewFloat(texIlCoCg[outputIlIdx]->uav.get(), clr);
		context->ClearUnorderedAccessViewFloat(texGiSpecular[outputAoIdx]->uav.get(), clr);
	}

	CS_GPU_PASS("ScreenSpaceGI::SSGI");

	static uint lastFrameAoTexIdx = 0;
	static uint lastFrameGITexIdx = 0;
	static uint lastFrameAccumTexIdx = 0;
	uint inputAoTexIdx = lastFrameAoTexIdx;
	uint inputGITexIdx = lastFrameGITexIdx;

	// Zeroing the accumulation count drives the denoiser's lerp factor to 1, dropping stale
	// history in one frame instead of fading it over MaxAccumFrames.
	if (queuedResetHistory.exchange(false)) {
		FLOAT clr[4] = { 0.f, 0.f, 0.f, 0.f };
		for (auto& tex : texAccumFrames)
			context->ClearUnorderedAccessViewFloat(tex->uav.get(), clr);
	}

	//////////////////////////////////////////////////////

	if (recompileFlag)
		ClearShaderCache();

	UpdateSB();

	//////////////////////////////////////////////////////

	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;
	auto deferred = globals::deferred;

	float2 size = Util::ConvertToDynamic(globals::state->screenSize);
	auto resolution = std::array{ (uint)size.x, (uint)size.y };
	auto resChoices = std::array{
		resolution, std::array{ resolution[0] >> 1, resolution[1] >> 1 }, std::array{ resolution[0] >> 2, resolution[1] >> 2 }
	};
	auto internalRes = resChoices[settings.ResolutionMode];

	std::array<ID3D11ShaderResourceView*, 11> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 6> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 2> samplers = { pointClampSampler.get(), linearClampSampler.get() };
	auto cb = ssgiCB->CB();

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	//////////////////////////////////////////////////////

	context->CSSetConstantBuffers(1, 1, &cb);
	auto* sharedDataBuf = globals::state->sharedDataCB->CB();
	context->CSSetConstantBuffers(5, 1, &sharedDataBuf);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	// prefilter depths
	{
		CS_GPU_PASS("ScreenSpaceGI::PrefilterDepths");

		srvs.at(0) = Util::GetCurrentSceneDepthSRV();
		for (int i = 0; i < 5; ++i)
			uavs.at(i) = uavWorkingDepth[i].get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(prefilterDepthsCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 15) >> 4, (resolution[1] + 15) >> 4, 1);
	}

	// fetch radiance and disocclusion; without GI or the temporal denoiser the pass has no outputs
	if (runILPath || settings.EnableTemporalDenoiser) {
		CS_GPU_PASS("ScreenSpaceGI::RadianceDisocc");

		resetViews();
		srvs.at(0) = runILPath ? rts[deferred->forwardRenderTargets[0]].SRV : nullptr;
		srvs.at(1) = texWorkingDepth->srv.get();
		srvs.at(2) = rts[NORMALROUGHNESS].SRV;
		srvs.at(3) = texPrevGeo->srv.get();
		srvs.at(4) = rts[RE::RENDER_TARGET::kMOTION_VECTOR].SRV;
		srvs.at(5) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
		srvs.at(6) = texAo[inputAoTexIdx]->srv.get();
		if (runILPath) {
			srvs.at(7) = texIlY[inputGITexIdx]->srv.get();
			srvs.at(8) = texIlCoCg[inputGITexIdx]->srv.get();
			srvs.at(9) = texGiSpecular[inputAoTexIdx]->srv.get();
		}
		srvs.at(10) = nullptr;

		uavs.at(0) = runILPath ? texRadianceTemp->uav.get() : nullptr;
		uavs.at(1) = texAccumFrames[!lastFrameAccumTexIdx]->uav.get();
		uavs.at(2) = texAo[!inputAoTexIdx]->uav.get();
		if (runILPath) {
			uavs.at(3) = texIlY[!inputGITexIdx]->uav.get();
			uavs.at(4) = texIlCoCg[!inputGITexIdx]->uav.get();
			uavs.at(5) = texGiSpecular[!inputAoTexIdx]->uav.get();
		}

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(radianceDisoccCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		// Prefilter radiance texture instead of using GenerateMips for proper dynamic resolution handling.
		// radianceDisocc wrote mip 0 directly to texRadianceTemp above, so we can bind it
		// as SRV input here without an intermediate CopySubresourceRegion.
		if (runILPath) {
			CS_GPU_PASS("ScreenSpaceGI::PrefilterRadiance");

			resetViews();
			srvs.at(0) = texRadianceTemp->srv.get();
			uavs.at(0) = uavRadiance[0].get();  // Mip 0
			uavs.at(1) = uavRadiance[1].get();  // Mip 1
			uavs.at(2) = uavRadiance[2].get();  // Mip 2
			uavs.at(3) = uavRadiance[3].get();  // Mip 3
			uavs.at(4) = uavRadiance[4].get();  // Mip 4

			context->CSSetShaderResources(0, 1, srvs.data());
			context->CSSetUnorderedAccessViews(0, 5, uavs.data(), nullptr);
			context->CSSetShader(prefilterRadianceCompute.get(), nullptr, 0);
			context->Dispatch((internalRes[0] + 15u) >> 4, (internalRes[1] + 15u) >> 4, 1);
		}

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
		lastFrameAccumTexIdx = !lastFrameAccumTexIdx;
	}

	// Prefilter normals
	{
		CS_GPU_PASS("ScreenSpaceGI::PrefilterNormals");

		resetViews();
		srvs.at(0) = rts[NORMALROUGHNESS].SRV;
		uavs.at(0) = uavNormal[0].get();
		uavs.at(1) = uavNormal[1].get();
		uavs.at(2) = uavNormal[2].get();
		uavs.at(3) = uavNormal[3].get();
		uavs.at(4) = uavNormal[4].get();

		context->CSSetShaderResources(0, 1, srvs.data());
		context->CSSetUnorderedAccessViews(0, 5, uavs.data(), nullptr);
		context->CSSetShader(prefilterNormalCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 15u) >> 4, (internalRes[1] + 15u) >> 4, 1);
	}

	// Reproject eye 0's view-independent diffuse GI into eye 1; requires specular GI off
	// (view-dependent) and both the eye-0-only march and reproject shaders compiled.
	const bool useReproject = globals::game::isVR && settings.UseStereoReproject &&
	                          !settings.EnableExperimentalSpecularGI && giEye0OnlyCompute && reprojectCompute;

	// GI
	{
		CS_GPU_PASS("ScreenSpaceGI::GI");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = rts[NORMALROUGHNESS].SRV;
		srvs.at(2) = runILPath ? texRadiance->srv.get() : nullptr;
		srvs.at(3) = texNoise->srv.get();
		srvs.at(4) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
		if (runILPath) {
			srvs.at(5) = texIlY[inputGITexIdx]->srv.get();
			srvs.at(6) = texIlCoCg[inputGITexIdx]->srv.get();
			srvs.at(7) = texGiSpecular[inputAoTexIdx]->srv.get();
		}
		srvs.at(8) = texNormal->srv.get();
		if (useModeTextureThisFrame)
			srvs.at(9) = globals::features::vr.stereoOpt.GetModeTextureSRV();

		uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
		if (runILPath) {
			uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
			uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
			uavs.at(3) = texGiSpecular[!inputAoTexIdx]->uav.get();
		}
		uavs.at(4) = texPrevGeo->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(useReproject ? giEye0OnlyCompute.get() : giCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
		lastFrameGITexIdx = inputGITexIdx;
		lastFrameAoTexIdx = inputAoTexIdx;
	}

	// View-independent reproject: fill eye 1's diffuse GI from eye 0 before the blur, whose
	// seam taps read across eyes. Replaces the tail bilateral stereo sync below.
	if (useReproject) {
		CS_GPU_PASS("ScreenSpaceGI::Reproject");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = texAo[inputAoTexIdx]->srv.get();
		if (runILPath) {
			srvs.at(2) = texIlY[inputGITexIdx]->srv.get();
			srvs.at(3) = texIlCoCg[inputGITexIdx]->srv.get();
		}
		if (useModeTextureThisFrame)
			srvs.at(4) = globals::features::vr.stereoOpt.GetModeTextureSRV();

		uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
		if (runILPath) {
			uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
			uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
		}

		const bool useReprojectDebug = globals::features::vr.settings.ReprojectDebugMode == 1 && globals::state->IsDeveloperMode() && reprojectDebugCompute;
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(useReprojectDebug ? reprojectDebugCompute.get() : reprojectCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
		lastFrameGITexIdx = inputGITexIdx;
		lastFrameAoTexIdx = inputAoTexIdx;
	}

	// blur
	if (settings.EnableBlur && runILPath) {
		CS_GPU_PASS("ScreenSpaceGI::Blur");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = rts[NORMALROUGHNESS].SRV;
		srvs.at(2) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
		srvs.at(3) = texIlY[inputGITexIdx]->srv.get();
		srvs.at(4) = texIlCoCg[inputGITexIdx]->srv.get();

		uavs.at(0) = texAccumFrames[!lastFrameAccumTexIdx]->uav.get();
		uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(blurCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputGITexIdx = !inputGITexIdx;
		lastFrameGITexIdx = inputGITexIdx;
		lastFrameAccumTexIdx = !lastFrameAccumTexIdx;
	}

	// VR stereo sync: bilateral blend of SSGI buffers between eyes (skipped when the
	// view-independent reproject above already unified them).
	// Shi, Billeter, Eisemann 2022, "Stereo-consistent screen-space ambient occlusion"
	if (globals::game::isVR && stereoSyncCompute && !useReproject) {
		CS_GPU_PASS("ScreenSpaceGI::StereoSync");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = texAo[inputAoTexIdx]->srv.get();
		if (runILPath) {
			srvs.at(2) = texIlY[inputGITexIdx]->srv.get();
			srvs.at(3) = texIlCoCg[inputGITexIdx]->srv.get();
		}

		uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
		if (runILPath) {
			uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
			uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
		}

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(stereoSyncCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
	}

	// upsample
	if (settings.ResolutionMode != 0) {
		CS_GPU_PASS("ScreenSpaceGI::Upsample");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = texAo[inputAoTexIdx]->srv.get();
		if (runILPath) {
			srvs.at(2) = texIlY[inputGITexIdx]->srv.get();
			srvs.at(3) = texIlCoCg[inputGITexIdx]->srv.get();
			srvs.at(4) = texGiSpecular[inputAoTexIdx]->srv.get();
		}

		uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
		if (runILPath) {
			uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
			uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
			uavs.at(3) = texGiSpecular[!inputAoTexIdx]->uav.get();
		}

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(upsampleCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
	}

	outputAoIdx = inputAoTexIdx;
	outputIlIdx = inputGITexIdx;

	// cleanup
	resetViews();

	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);
}

#undef I18N_KEY_PREFIX
