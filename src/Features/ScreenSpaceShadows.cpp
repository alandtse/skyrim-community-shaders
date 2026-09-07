#include "ScreenSpaceShadows.h"

#include "Features/TerrainBlending.h"
#include "Features/VR.h"
#include "FoveatedCommon.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Upscaling.h"
#include "Util.h"
#include "Utils/D3D.h"
#include <algorithm>
#include <array>
#include <cmath>

#define I18N_KEY_PREFIX "feature.screen_space_shadows."

#pragma warning(push)
#pragma warning(disable: 4838 4244)
#include "ScreenSpaceShadows/bend_sss_cpu.h"
#pragma warning(pop)

using RE::RENDER_TARGETS;

namespace
{
	struct FoveatedShadowState
	{
		bool available = false;
		bool active = false;
		float centerScale = FoveatedCommon::kCenterScaleMax;
		float centerHorizontalScale = 1.0f;
		std::array<float2, 2> centerOffsets{};
	};

	FoveatedShadowState ResolveFoveatedShadowState(const ScreenSpaceShadows::BendSettings& a_settings)
	{
		FoveatedShadowState state{};
		if (!globals::game::isVR)
			return state;

		const auto& upscaling = globals::features::upscaling;
		const auto profile = upscaling.foveatedRender.GetFoveationProfile();
		state.available = profile.available;
		if (!state.available || !a_settings.EnableFoveated)
			return state;

		state.centerScale = FoveatedCommon::ClampCenterScale(profile.coverageScale);
		state.centerHorizontalScale = FoveatedCommon::ClampCenterHorizontalScale(profile.centerHorizontalScale);
		state.centerOffsets[0] = profile.centerOffsets[0];
		state.centerOffsets[1] = profile.centerOffsets[1];
		state.active = FoveatedCommon::IsActiveCoverage(state.centerScale);
		return state;
	}

	// Only called when unavailable. Check hardware/mode blockers before opt-in/restart
	// hints, so a user on a method Foveated Render doesn't support is never told to
	// go toggle Foveated Render.
	const char* FoveatedUnavailableReason(const ScreenSpaceShadows::BendSettings& a_settings)
	{
		const auto& upscaling = globals::features::upscaling;
		if (!a_settings.Enable)
			return T(TKEY("fov_unavailable_sss_disabled"), "Requires Screen Space Shadows to be enabled.");
		const auto method = upscaling.GetUpscaleMethod();
		if (method != Upscaling::UpscaleMethod::kDLSS && method != Upscaling::UpscaleMethod::kFSR)
			return T(TKEY("fov_unavailable_not_dlss"), "Requires the DLSS or FSR upscaler: set Upscaling's method to one of them.");
		if (method == Upscaling::UpscaleMethod::kDLSS && !upscaling.streamline.featureDLSS)
			return T(TKEY("fov_unavailable_no_dlss"), "Requires DLSS, which this GPU or driver does not support.");
		if (!upscaling.foveatedRender.settings.enabled)
			return T(TKEY("fov_unavailable_foveation_off"), "Enable Foveated Render in the Upscaling settings; it takes effect after a restart.");
		if (!upscaling.foveatedRender.IsLoaded())
			return T(TKEY("fov_unavailable_restart"), "Foveated Render is enabled but needs a game restart to take effect.");
		return T(TKEY("fov_unavailable_full_coverage"), "Foveated Render currently covers the full frame, so there is nothing to foveate.");
	}

	struct SssPreset
	{
		bool foveated;
	};

	// foveated is Performance-only: SSS foveation depends on Upscaling's own boot-latched
	// foveation subrect, which Upscaling's own tiers only enable at Performance.
	constexpr SssPreset GetSssPreset(Feature::PerfProfile profile)
	{
		return { profile == Feature::PerfProfile::Performance };
	}

	FoveatedCommon::DispatchBounds BuildFoveatedBounds(
		const FoveatedShadowState& a_state,
		uint32_t a_eyeIndex,
		uint32_t a_eyeMinX,
		uint32_t a_eyeMaxX,
		uint32_t a_frameHeight)
	{
		const auto offset = a_state.centerOffsets[std::min<size_t>(a_eyeIndex, a_state.centerOffsets.size() - 1)];
		return FoveatedCommon::BuildCenteredDispatchBounds(
			a_eyeMinX,
			a_eyeMaxX,
			a_frameHeight,
			a_state.centerScale,
			offset.x,
			offset.y,
			FoveatedCommon::kCenterFeather,
			a_state.centerHorizontalScale);
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::BendSettings,
	Enable,
	SampleCount,
	SurfaceThickness,
	BilinearThreshold,
	ShadowContrast,
	EnableFoveated)

void ScreenSpaceShadows::DrawStereoToggles()
{
	ImGui::Checkbox(T(TKEY("vr_stereo_sync"), "Stereo Consistency"), &enableStereoSync);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("vr_stereo_sync_tooltip"),
							  "Off: each eye computes shadows independently (may mismatch between eyes).\n"
							  "On: both eyes compute, then reconcile (matches between eyes, highest cost)."));
}

void ScreenSpaceShadows::DrawFoveatedToggle()
{
	const FoveatedShadowState foveatedState = ResolveFoveatedShadowState(bendSettings);
	bool foveatedEnabled = bendSettings.EnableFoveated != 0;
	{
		auto foveatedGuard = Util::DisableGuard(!foveatedState.available);
		if (ImGui::Checkbox(T(TKEY("fov_screen_space_shadows"), "FOV Screen Space Shadows"), &foveatedEnabled))
			bendSettings.EnableFoveated = foveatedEnabled ? 1u : 0u;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("fov_screen_space_shadows_tooltip"),
			"Uses the active Upscaling FOV mask for Screen Space Shadows.\n"
			"When enabled, full-quality SSS is computed inside the FOV mask and fades to no SSS outside it.\n"
			"The mask center follows the active DLSS/upscaler foveation subrect.\n"
			"The mask area, horizontal scale, and per-eye offsets are taken from Upscaling; SSS has no separate FOV size."));
	}
	if (!foveatedState.available)
		ImGui::TextDisabled("%s", FoveatedUnavailableReason(bendSettings));
}

// Hub view: the SSS stereo sync toggle, bound to the same setting the SSS panel shows.
void ScreenSpaceShadows::DrawPerformanceSettings()
{
	DrawStereoToggles();
}

// Hub view: the FOV Screen Space Shadows toggle, bound to the same setting the SSS panel shows.
// PerformanceSectionRequiresVR() keeps this section out of the hub on flatrim.
void ScreenSpaceShadows::DrawPerformancePresets()
{
	ImGui::Indent();
	DrawFoveatedToggle();
	ImGui::Unindent();
}

// A profile drives the umbrella toggle, so enable it here (else it can't engage from Off).
// PerformanceSectionRequiresVR() keeps both functions off flatrim, so neither needs its own isVR check.
void ScreenSpaceShadows::ApplyPerformanceProfile(PerfProfile profile)
{
	const auto preset = GetSssPreset(profile);
	enableStereoSync = true;
	bendSettings.EnableFoveated = preset.foveated ? 1u : 0u;
}

bool ScreenSpaceShadows::MatchesPerformanceProfile(PerfProfile profile) const
{
	const auto preset = GetSssPreset(profile);
	return enableStereoSync &&
	       (bendSettings.EnableFoveated != 0) == preset.foveated;
}

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("general"), "General"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable"), "Enable"), (bool*)&bendSettings.Enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("enable_tooltip"), "Enable screen-space contact shadows from the sun/moon direction."));

		ImGui::SliderInt(T(TKEY("sample_count"), "Sample Count Multiplier"), (int*)&bendSettings.SampleCount, 1, 4);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("sample_count_tooltip"), "Multiplier for shadow ray sample count. Higher values increase shadow reach at the cost of performance. Adapts to render resolution."));

		ImGui::SliderFloat(T(TKEY("surface_thickness"), "Surface Thickness"), &bendSettings.SurfaceThickness, 0.005f, 0.05f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("surface_thickness_tooltip"), "Assumed thickness of surfaces for shadow detection. Lower values produce thinner, more precise shadows."));

		ImGui::SliderFloat(T(TKEY("bilinear_threshold"), "Bilinear Threshold"), &bendSettings.BilinearThreshold, 0.02f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("bilinear_threshold_tooltip"), "Depth threshold for edge detection during bilinear interpolation. Higher values smooth more aggressively across edges."));

		ImGui::SliderFloat(T(TKEY("shadow_contrast"), "Shadow Contrast"), &bendSettings.ShadowContrast, 0.0f, 4.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shadow_contrast_tooltip"), "Contrast boost for the shadow transition. Higher values produce harder shadow edges."));

		if (globals::game::isVR) {
			DrawStereoToggles();
			DrawFoveatedToggle();
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void ScreenSpaceShadows::InvalidateRaymarchShaders()
{
	raymarchCS.Reset();
	raymarchRightCS.Reset();
}

void ScreenSpaceShadows::ClearShaderCache()
{
	InvalidateRaymarchShaders();
	stereoSyncCS.Reset();
}

uint ScreenSpaceShadows::GetScaledSampleCount()
{
	if (globals::game::isVR) {
		// In VR, SAMPLE_COUNT is a pixel-space ray length that is FOV-driven, not resolution-driven.
		// Resolution-scaling produced 2-8x excess samples at VR resolutions with no quality benefit.
		// WAVE_SIZE (64) alignment is required for correct Bend READ_COUNT computation.
		return bendSettings.SampleCount * 64;
	}

	float2 renderSize = Util::ConvertToDynamic(globals::state->screenSize);

	// Scale sample count based on both dimensions relative to 1920x1080 reference
	float2 referenceRes = { 1920.0f, 1080.0f };
	float referenceArea = referenceRes.x * referenceRes.y;
	float currentArea = renderSize.x * renderSize.y;
	float areaScale = std::sqrt(currentArea / referenceArea);
	uint scaledSampleCount = static_cast<uint>(std::round(bendSettings.SampleCount * 60 * areaScale));

	// Quantize to steps of 8 to prevent frequent recompilation from small DRS oscillations
	scaledSampleCount = ((scaledSampleCount + 7u) / 8u) * 8u;
	scaledSampleCount = std::max(scaledSampleCount, 8u);

	return scaledSampleCount;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarch()
{
	uint scaledSampleCount = GetScaledSampleCount();

	if (scaledSampleCount != lastCompiledSampleCount) {
		lastCompiledSampleCount = scaledSampleCount;
		InvalidateRaymarchShaders();
	}

	auto sampleCount = std::format("{}", scaledSampleCount);
	std::vector<std::pair<const char*, const char*>> defines{ { "SAMPLE_COUNT", sampleCount.c_str() } };
	// TERRAIN_BLENDING flips DepthTexture's HLSL type from `Texture2D<unorm float>`
	// (R24_UNORM_X8_TYPELESS game depth) to `Texture2D<float>` (R32_FLOAT blendedDepth).
	if (globals::features::terrainBlending.loaded)
		defines.push_back({ "TERRAIN_BLENDING", "" });
	return raymarchCS.Get(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", defines, "cs_5_0");
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarchRight()
{
	uint scaledSampleCount = GetScaledSampleCount();
	auto sampleCount = std::format("{}", scaledSampleCount);
	std::vector<std::pair<const char*, const char*>> defines{ { "SAMPLE_COUNT", sampleCount.c_str() }, { "RIGHT", "" } };
	if (globals::features::terrainBlending.loaded)
		defines.push_back({ "TERRAIN_BLENDING", "" });
	return raymarchRightCS.Get(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", defines, "cs_5_0");
}

void ScreenSpaceShadows::DrawShadows()
{
	ZoneScopedS(8);
	CS_GPU_PASS("ScreenSpaceShadows::DrawShadows");

	auto context = globals::d3d::context;

	auto accumulator = *globals::game::currentAccumulator.get();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto& directionNi = dirLight->GetWorldDirection();
	float3 light = { directionNi.x, directionNi.y, directionNi.z };
	light.Normalize();
	float4 lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);

	// Helper lambda to calculate light projection for a given eye
	auto CalculateLightProjection = [&](uint32_t eyeIndex = 0) -> std::array<float, 4> {
		auto viewProjMat = globals::game::frameBufferCached.GetCameraViewProj(eyeIndex).Transpose();
		auto projectedLight = DirectX::SimpleMath::Vector4::Transform(lightProjection, viewProjMat);
		return { projectedLight.x, projectedLight.y, projectedLight.z, projectedLight.w };
	};

	auto lightProjectionF = CalculateLightProjection(0);

	float2 renderSize = Util::ConvertToDynamic(globals::state->screenSize);
	int viewportSize[2] = { (int)renderSize.x, (int)renderSize.y };

	if (globals::game::isVR)
		viewportSize[0] /= 2;

	const FoveatedShadowState foveatedState = ResolveFoveatedShadowState(bendSettings);

	// Setup common render state.
	// SSS always uses 24/32-bit depth, never the R16_UNORM half-precision path.
	// With TerrainBlending loaded the SRV is R32_FLOAT (blendedDepthTexture);
	// without it, the game's kPOST_ZPREPASS_COPY (R24_UNORM_X8_TYPELESS).
	// The shader's DepthTexture declaration is conditional on TERRAIN_BLENDING:
	// `<float>` for the R32_FLOAT path, `<unorm float>` for the R24_UNORM path.
	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	context->CSSetShaderResources(0, 1, &depthSRV);

	auto uav = screenSpaceShadowsTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetSamplers(0, 1, &pointBorderSampler);

	auto buffer = raymarchCB->CB();
	context->CSSetConstantBuffers(1, 1, &buffer);

	auto viewport = globals::game::graphicsState;

	float2 dynamicRes = { viewport->GetRuntimeData().dynamicResolutionWidthRatio, viewport->GetRuntimeData().dynamicResolutionHeightRatio };

	// Shared dispatch logic for both VR and non-VR
	auto DispatchEye = [&](const char* eyeName, ID3D11ComputeShader* shader, uint32_t eyeIndex, const float* lightProj,
						   float invTexSizeX, float invTexSizeY) {
		if (!shader)
			return;

		std::string timerName = eyeName ? std::format("ScreenSpaceShadows::RayMarch({})", eyeName) : "ScreenSpaceShadows::RayMarch";
		CS_GPU_PASS_DYNAMIC(timerName);

		context->CSSetShader(shader, nullptr, 0);

		int minRenderBounds[2] = { 0, 0 };
		int maxRenderBounds[2] = { viewportSize[0], viewportSize[1] };
		if (foveatedState.active) {
			const auto bounds = BuildFoveatedBounds(foveatedState, eyeIndex, 0u, static_cast<uint32_t>(viewportSize[0]), static_cast<uint32_t>(viewportSize[1]));
			if (bounds.maxX <= bounds.minX || bounds.maxY <= bounds.minY) {
				return;
			}

			minRenderBounds[0] = bounds.minX;
			minRenderBounds[1] = bounds.minY;
			maxRenderBounds[0] = bounds.maxX;
			maxRenderBounds[1] = bounds.maxY;
		}

		auto dispatchList = Bend::BuildDispatchList(const_cast<float*>(lightProj), viewportSize, minRenderBounds, maxRenderBounds);

		for (int i = 0; i < dispatchList.DispatchCount; i++) {
			auto dispatchData = dispatchList.Dispatch[i];

			{
				CS_GPU_PASS("SSS::RayMarch::DispatchEyeCB");

				RaymarchCB data{};
				data.LightCoordinate[0] = dispatchList.LightCoordinate_Shader[0];
				data.LightCoordinate[1] = dispatchList.LightCoordinate_Shader[1];
				data.LightCoordinate[2] = dispatchList.LightCoordinate_Shader[2];
				data.LightCoordinate[3] = dispatchList.LightCoordinate_Shader[3];

				data.WaveOffset[0] = dispatchData.WaveOffset_Shader[0];
				data.WaveOffset[1] = dispatchData.WaveOffset_Shader[1];

				data.FarDepthValue = 1.0f;
				data.NearDepthValue = 0.0f;

				data.DynamicRes = dynamicRes;
				data.FoveatedData0[0] = foveatedState.centerScale;
				data.FoveatedData0[1] = FoveatedCommon::kCenterFeather;
				data.FoveatedData0[2] = foveatedState.centerHorizontalScale;
				data.FoveatedData0[3] = foveatedState.active ? 1.0f : 0.0f;
				const auto centerOffset = foveatedState.centerOffsets[std::min<size_t>(eyeIndex, foveatedState.centerOffsets.size() - 1)];
				data.FoveatedCenterOffset[0] = centerOffset.x;
				data.FoveatedCenterOffset[1] = centerOffset.y;
				data.FoveatedCenterOffset[2] = 0.0f;
				data.FoveatedCenterOffset[3] = 0.0f;

				data.InvDepthTextureSize[0] = invTexSizeX;
				data.InvDepthTextureSize[1] = invTexSizeY;

				data.settings = bendSettings;

				raymarchCB->Update(data);
			}

			{
				CS_GPU_PASS("SSS::RayMarch::DispatchEyeSweep");
				context->Dispatch(dispatchData.WaveCount[0], dispatchData.WaveCount[1], dispatchData.WaveCount[2]);
			}
		}
	};

	float InvTexSizeX = 1.0f / (float)viewportSize[0];
	float InvTexSizeY = 1.0f / (float)viewportSize[1];

	if (!globals::game::isVR) {
		DispatchEye(nullptr, GetComputeRaymarch(), 0, lightProjectionF.data(), InvTexSizeX, InvTexSizeY);
	} else {
		{
			CS_GPU_PASS("SSS::LeftEye");
			DispatchEye("Left Eye", GetComputeRaymarch(), 0, lightProjectionF.data(), InvTexSizeX, InvTexSizeY);
		}

		auto lightProjectionRightF = CalculateLightProjection(1);
		{
			CS_GPU_PASS("SSS::RightEye");
			DispatchEye("Right Eye", GetComputeRaymarchRight(), 1, lightProjectionRightF.data(), InvTexSizeX, InvTexSizeY);
		}
	}

	ID3D11ShaderResourceView* views[1]{ nullptr };
	context->CSSetShaderResources(0, 1, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11SamplerState* sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);
}

void ScreenSpaceShadows::DrawStereoSync()
{
	if (!globals::game::isVR || !enableStereoSync || !stereoSyncCopyTex || !stereoSyncCB)
		return;

	std::vector<std::pair<const char*, const char*>> defines{ { "VR", "" }, { "FRAMEBUFFER", "" } };
	if (globals::features::terrainBlending.loaded)
		defines.push_back({ "TERRAIN_BLENDING", "" });
	ID3D11ComputeShader* stereoCS = stereoSyncCS.Get(L"Data\\Shaders\\ScreenSpaceShadows\\StereoSyncCS.hlsl", defines, "cs_5_0");
	if (!stereoCS)
		return;

	ZoneScoped;
	CS_GPU_PASS("ScreenSpaceShadows::StereoSync");

	auto context = globals::d3d::context;

	float2 resolution = Util::ConvertToDynamic(globals::state->screenSize);
	const uint32_t frameWidth = static_cast<uint32_t>(resolution.x);
	const uint32_t frameHeight = static_cast<uint32_t>(resolution.y);
	const FoveatedShadowState foveatedState = ResolveFoveatedShadowState(bendSettings);
	if (frameWidth == 0 || frameHeight == 0)
		return;

	const bool foveatedStereoSync = foveatedState.active && frameWidth > 1;
	std::array<FoveatedCommon::DispatchBounds, 2> syncBounds{};
	if (foveatedStereoSync) {
		const uint32_t eyeWidth = frameWidth >> 1;
		syncBounds[0] = BuildFoveatedBounds(foveatedState, 0, 0, eyeWidth, frameHeight);
		syncBounds[1] = BuildFoveatedBounds(foveatedState, 1, eyeWidth, frameWidth, frameHeight);
	} else {
		syncBounds[0].minX = 0;
		syncBounds[0].minY = 0;
		syncBounds[0].maxX = static_cast<int>(frameWidth);
		syncBounds[0].maxY = static_cast<int>(frameHeight);
	}

	auto ForEachSyncBounds = [&](auto&& a_fn) {
		a_fn(syncBounds[0], 0u);
		if (foveatedStereoSync)
			a_fn(syncBounds[1], 1u);
	};

	auto CopyStereoSyncSource = [&] {
		if (!foveatedStereoSync || !stereoSyncCopyTex->uav) {
			context->CopyResource(stereoSyncCopyTex->resource.get(), screenSpaceShadowsTexture->resource.get());
			return;
		}

		const FLOAT white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		context->ClearUnorderedAccessViewFloat(stereoSyncCopyTex->uav.get(), white);
		ForEachSyncBounds([&](const FoveatedCommon::DispatchBounds& bounds, uint32_t) {
			if (bounds.maxX <= bounds.minX || bounds.maxY <= bounds.minY)
				return;

			D3D11_BOX srcBox{
				static_cast<UINT>(bounds.minX),
				static_cast<UINT>(bounds.minY),
				0u,
				static_cast<UINT>(bounds.maxX),
				static_cast<UINT>(bounds.maxY),
				1u
			};
			context->CopySubresourceRegion(
				stereoSyncCopyTex->resource.get(),
				0,
				srcBox.left,
				srcBox.top,
				0,
				screenSpaceShadowsTexture->resource.get(),
				0,
				&srcBox);
		});
	};

	CopyStereoSyncSource();

	// Same 24/32-bit depth path as the raymarch — SrcDepthTexture's HLSL type is
	// conditional on TERRAIN_BLENDING via the define passed at compile time below.
	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	ID3D11ShaderResourceView* srvs[2]{ depthSRV, stereoSyncCopyTex->srv.get() };
	ID3D11UnorderedAccessView* uavs[1]{ screenSpaceShadowsTexture->uav.get() };

	auto* sharedDataBuf = globals::state->sharedDataCB->CB();
	context->CSSetConstantBuffers(5, 1, &sharedDataBuf);
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	context->CSSetShader(stereoCS, nullptr, 0);

	auto DispatchSyncBounds = [&](const FoveatedCommon::DispatchBounds& bounds, uint32_t eyeIndex) {
		if (bounds.maxX <= bounds.minX || bounds.maxY <= bounds.minY)
			return;

		const uint32_t dispatchWidth = static_cast<uint32_t>(bounds.maxX - bounds.minX);
		const uint32_t dispatchHeight = static_cast<uint32_t>(bounds.maxY - bounds.minY);
		if (dispatchWidth == 0 || dispatchHeight == 0)
			return;

		StereoSyncCB cbData{};
		cbData.FrameDim[0] = resolution.x;
		cbData.FrameDim[1] = resolution.y;
		cbData.RcpFrameDim[0] = 1.0f / resolution.x;
		cbData.RcpFrameDim[1] = 1.0f / resolution.y;
		cbData.DispatchBase[0] = static_cast<float>(bounds.minX);
		cbData.DispatchBase[1] = static_cast<float>(bounds.minY);
		cbData.DispatchExtent[0] = static_cast<float>(dispatchWidth);
		cbData.DispatchExtent[1] = static_cast<float>(dispatchHeight);
		cbData.FoveatedData0[0] = foveatedState.centerScale;
		cbData.FoveatedData0[1] = FoveatedCommon::kCenterFeather;
		cbData.FoveatedData0[2] = foveatedState.centerHorizontalScale;
		cbData.FoveatedData0[3] = foveatedState.active ? 1.0f : 0.0f;
		const auto centerOffset = foveatedState.centerOffsets[std::min<size_t>(eyeIndex, foveatedState.centerOffsets.size() - 1)];
		cbData.FoveatedCenterOffset[0] = centerOffset.x;
		cbData.FoveatedCenterOffset[1] = centerOffset.y;
		cbData.FoveatedCenterOffset[2] = 0.0f;
		cbData.FoveatedCenterOffset[3] = 0.0f;

		stereoSyncCB->Update(cbData);
		auto cbPtr = stereoSyncCB->CB();
		context->CSSetConstantBuffers(1, 1, &cbPtr);

		const uint32_t groupsX = (dispatchWidth + 7u) / 8u;
		const uint32_t groupsY = (dispatchHeight + 7u) / 8u;
		context->Dispatch(groupsX, groupsY, 1);
	};

	ForEachSyncBounds(DispatchSyncBounds);

	srvs[0] = nullptr;
	srvs[1] = nullptr;
	uavs[0] = nullptr;
	ID3D11Buffer* nullBuffer = nullptr;
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	context->CSSetConstantBuffers(1, 1, &nullBuffer);
	context->CSSetShader(nullptr, nullptr, 0);
}

void ScreenSpaceShadows::Prepass()
{
	auto context = globals::d3d::context;

	float white[4] = { 1, 1, 1, 1 };
	context->ClearUnorderedAccessViewFloat(screenSpaceShadowsTexture->uav.get(), white);

	if (auto sky = globals::game::sky)
		if (bendSettings.Enable && sky->mode.get() == RE::Sky::Mode::kFull) {
			DrawShadows();
			DrawStereoSync();
		}

	auto view = screenSpaceShadowsTexture->srv.get();
	context->PSSetShaderResources(45, 1, &view);
}

void ScreenSpaceShadows::LoadSettings(json& o_json)
{
	bendSettings = o_json;
	// Absent key resets to the default, so an older blob can't pin a stale state.
	enableStereoSync = o_json.value("EnableStereoSync", true);
}

void ScreenSpaceShadows::SaveSettings(json& o_json)
{
	o_json = bendSettings;
	o_json["EnableStereoSync"] = enableStereoSync;
}

void ScreenSpaceShadows::RestoreDefaultSettings()
{
	bendSettings = {};
	enableStereoSync = true;
}

bool ScreenSpaceShadows::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}

void ScreenSpaceShadows::SetupResources()
{
	raymarchCB = new ConstantBuffer(ConstantBufferDesc<RaymarchCB>(), "SSS::RaymarchCB");

	if (globals::game::isVR) {
		stereoSyncCB = new ConstantBuffer(ConstantBufferDesc<StereoSyncCB>(), "SSS::StereoSyncCB");
	}

	{
		auto device = globals::d3d::device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		samplerDesc.BorderColor[0] = 1.0f;
		samplerDesc.BorderColor[1] = 1.0f;
		samplerDesc.BorderColor[2] = 1.0f;
		samplerDesc.BorderColor[3] = 1.0f;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointBorderSampler));
		Util::SetResourceName(pointBorderSampler, "SSS::PointBorderSampler");
	}

	{
		auto renderer = globals::game::renderer;
		auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

		shadowMask.texture->GetDesc(&texDesc);
		shadowMask.SRV->GetDesc(&srvDesc);

		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		srvDesc.Format = texDesc.Format;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		screenSpaceShadowsTexture = new Texture2D(texDesc, "SSS::ShadowTexture");
		screenSpaceShadowsTexture->CreateSRV(srvDesc);
		screenSpaceShadowsTexture->CreateUAV(uavDesc);

		if (globals::game::isVR) {
			stereoSyncCopyTex = new Texture2D(texDesc, "SSS::StereoSyncCopy");
			stereoSyncCopyTex->CreateSRV(srvDesc);
			stereoSyncCopyTex->CreateUAV(uavDesc);
		}
	}
}
#undef I18N_KEY_PREFIX
