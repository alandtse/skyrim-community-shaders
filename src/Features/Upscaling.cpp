#include "Upscaling.h"
#include "GpuPass.h"

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "HDRDisplay.h"
#include "Hooks.h"
#include "State.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/FoveatedRender.h"
#include "Upscaling/FoveatedRender/Bridge.h"
#include "Upscaling/FoveatedRender/Core.h"
#include "Upscaling/FoveatedRender/Postprocess.h"
#include "Upscaling/FoveatedRender/Preprocess.h"
#include "Upscaling/PerfMode.h"
#include "Upscaling/Streamline.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Game.h"
#include "Utils/UI.h"
#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <directx/d3dx12.h>
#include <format>

#include "Features/PostProcessing.h"

#define I18N_KEY_PREFIX "feature.upscaling."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Upscaling::Settings,
	upscaleMethod,
	upscaleMethodNoDLSS,
	qualityMode,
	frameLimitMode,
	frameGenerationMode,
	frameGenerationForceEnable,
	frameGenerationAllowInMenus,
	preferFSRFrameGen,
	dlssgFramesToGenerate,
	streamlineLogLevel,
	sharpnessFSR,
	sharpnessEnabledDLSS,
	sharpnessDLSS,
	presetDLSS,
	reflexLowLatencyMode,
	reflexLowLatencyBoost,
	reflexUseMarkersToOptimize,
	reflexUseFPSLimit,
	reflexFPSLimit,
	renderAtUpscaleRes,
	vrRenderScale,
	fsr4RuntimeEnable,
	fsr4RuntimeSelectionSchemaVersion);

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChainUpscaling;

/**
 * @brief One-shot migration of fsr4RuntimeEnable for the detected adapter's FSR4 support class.
 *
 * RX 7000 (RDNA3 discrete) eligibility was added after fsr4RuntimeEnable already shipped
 * defaulting to off; auto-enable it once for those adapters so existing/new RX 7000 users
 * get the same experience as a fresh RX 9000 install. Leaves the user's own choice alone on
 * RX 9000 (already eligible pre-migration) and on Unsupported adapters, where the version is
 * deliberately left unstamped so the migration re-runs once a supported adapter is detected.
 */
void ApplyLegacyFsr4RuntimeSelectionMigration(Upscaling::Settings& a_settings, FidelityFX::Fsr4AdapterSupport a_adapterSupport)
{
	if (a_settings.fsr4RuntimeSelectionSchemaVersion >= Upscaling::kFsr4RuntimeSelectionSchemaVersion)
		return;

	if (a_adapterSupport == FidelityFX::Fsr4AdapterSupport::Unsupported)
		return;

	if (a_adapterSupport == FidelityFX::Fsr4AdapterSupport::RadeonRx7000 && !a_settings.fsr4RuntimeEnable) {
		a_settings.fsr4RuntimeEnable = true;
		logger::info("[Upscaling] Auto-enabled Runtime FSR4 for detected RX 7000-class adapter");
	}

	a_settings.fsr4RuntimeSelectionSchemaVersion = Upscaling::kFsr4RuntimeSelectionSchemaVersion;
}

/**
 * @brief Creates a Direct3D 11 device and swap chain, with support for advanced upscaling and frame generation features.
 *
 * This function intercepts the standard D3D11 device and swap chain creation process to enable integration with Streamline and FidelityFX technologies, as well as optional D3D12 proxying for frame generation. It adjusts swap chain flags for tearing support, manages feature checks, and conditionally routes device creation through Streamline or FidelityFX proxies based on runtime settings and hardware capabilities. If frame generation is enabled and supported, a D3D12 proxy is used; otherwise, the standard D3D11 creation path is followed.
 *
 * @return HRESULT indicating the success or failure of device and swap chain creation.
 */
HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChainUpscaling(
	IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	[[maybe_unused]] const D3D_FEATURE_LEVEL* pFeatureLevels,
	[[maybe_unused]] UINT FeatureLevels,
	UINT SDKVersion,
	DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	auto& upscaling = globals::features::upscaling;

	DXGI_ADAPTER_DESC adapterDesc{};
	if (pAdapter && SUCCEEDED(pAdapter->GetDesc(&adapterDesc))) {
		globals::state->SetAdapterDescription(adapterDesc.Description);
		ApplyLegacyFsr4RuntimeSelectionMigration(upscaling.settings, FidelityFX::GetFsr4AdapterSupport(adapterDesc));
	}

	upscaling.LoadUpscalingSDKs();

	// FLIP_DISCARD requires BufferCount >= 2 and a flip-model-compatible (non-sRGB) format.
	pSwapChainDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	if (pSwapChainDesc->BufferCount < 2)
		pSwapChainDesc->BufferCount = 2;

	if (globals::features::hdrDisplay.loaded) {
		logger::info("[Upscaling] Upgrading swap chain format from {} to R10G10B10A2_UNORM for HDR", static_cast<int>(pSwapChainDesc->BufferDesc.Format));
		pSwapChainDesc->BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	} else if (pSwapChainDesc->BufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
		pSwapChainDesc->BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	} else if (pSwapChainDesc->BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
		pSwapChainDesc->BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	}

	bool shouldProxy = !globals::game::isVR;
	if (shouldProxy)
		if (!pSwapChainDesc->Windowed)
			shouldProxy = false;

	auto refreshRate = Upscaling::GetRefreshRate(pSwapChainDesc->OutputWindow);
	upscaling.refreshRate = refreshRate;

	if (shouldProxy) {
		if (upscaling.settings.frameGenerationMode)
			if (refreshRate >= 120)
				shouldProxy = true;
			else if (upscaling.settings.frameGenerationForceEnable)
				shouldProxy = true;
			else
				shouldProxy = false;
		else
			shouldProxy = false;
	}

	upscaling.lowRefreshRate = refreshRate < 120;
	upscaling.isWindowed = pSwapChainDesc->Windowed;

	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;

	if (shouldProxy) {
		logger::info("[Frame Generation] Frame Generation enabled, using D3D12 proxy");

		// DLSS-G availability is only trustworthy once a real D3D12 device is bound to
		// the Streamline instance, so create and bind it before probing.
		bool dlssgAvailable = false;
		// NVIDIA only: on other vendors the probe would still SL-proxy the device and
		// queue that the FSR path then runs on.
		if (upscaling.streamlineDX12.initialized && adapterDesc.VendorId == Streamline::kNvidiaVendorId) {
			auto& sc = upscaling.dx12SwapChain;
			sc.CreateD3D12Device(pAdapter);

			// Detection needs the device bound, not SL-upgraded -- run raw so
			// dlssgAvailable reflects real availability, not just preference.
			upscaling.streamlineDX12.SetD3DDevice12(sc.d3d12Device.get());
			upscaling.streamlineDX12.CheckFeatures(pAdapter);
			upscaling.streamlineDX12.PostDevice();

			// Only suppress DLSS-G when FSR3 is actually reachable to fall back to.
			const bool userPrefersReachableFsr = upscaling.settings.preferFSRFrameGen && upscaling.fidelityFX.featureFSR3FG;
			dlssgAvailable = upscaling.streamlineDX12.featureDLSSG && !userPrefersReachableFsr;

			// Gating on dlssgAvailable (any cause, not just preference) keeps the FSR
			// path on a clean device -- upgrading unconditionally corrupted FSR3's
			// FrameGeneration DLL state and crashed on first Present.
			if (dlssgAvailable && upscaling.streamlineDX12.slUpgradeInterface) {
				// Upgrade in place -- a local copy leaves later queue/swap-chain
				// creation SL-invisible and silently breaks FG.
				upscaling.streamlineDX12.slUpgradeInterface((void**)&sc.d3d12Device);

				// CreateD3D12Device already populated commandQueue; release it first --
				// put() on an already-populated com_ptr leaks the prior reference.
				sc.commandQueue = nullptr;
				D3D12_COMMAND_QUEUE_DESC queueDesc = {};
				queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
				queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
				queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
				queueDesc.NodeMask = 0;
				DX::ThrowIfFailed(sc.d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(sc.commandQueue.put())));
			}
		}

		if (dlssgAvailable || upscaling.HasFrameGenModule()) {
			DX::ThrowIfFailed(D3D11CreateDevice(
				pAdapter,
				DriverType,
				Software,
				Flags,
				&featureLevel,
				1,
				SDKVersion,
				ppDevice,
				pFeatureLevel,
				ppImmediateContext));

			upscaling.SetProxyD3D11Device(*ppDevice);
			upscaling.SetProxyD3D11DeviceContext(*ppImmediateContext);

			if (dlssgAvailable) {
				logger::info("[Frame Generation] DLSS-G available, creating direct swap chain");
				upscaling.CreateProxySwapChainDirect(pAdapter, *pSwapChainDesc);
				if (upscaling.streamlineDX12.slUpgradeInterface)
					upscaling.streamlineDX12.slUpgradeInterface((void**)&upscaling.dx12SwapChain.swapChain);
			} else {
				upscaling.CreateProxySwapChain(pAdapter, *pSwapChainDesc);
			}

			upscaling.CreateProxyInterop();

			*ppSwapChain = upscaling.GetProxySwapChain();

			upscaling.d3d12SwapChainActive = true;

			if (upscaling.IsBackendInitialized()) {
				upscaling.UpgradeBackendInterface((void**)&(*ppDevice));
				// Never SL-wrap the swap chain here: the proxy's GetDevice() override (which
				// SkyrimPlatform relies on for IID_ID3D11Device) must stay outermost, or QI
				// through Streamline's wrapper fails with E_NOINTERFACE.
				upscaling.SetBackendD3DDevice(*ppDevice);
				// Feature availability (notably Reflex/PCL) is only reliable after device bind.
				upscaling.CheckBackendFeatures(pAdapter);
				upscaling.PostBackendDevice();
			}

			return S_OK;
		} else {
			logger::warn("[Frame Generation] No frame generation module available, skipping proxy");
			upscaling.fidelityFXMissing = true;
		}
	}

	auto ret = ptrD3D11CreateDeviceAndSwapChainUpscaling(pAdapter,
		DriverType,
		Software,
		Flags,
		&featureLevel,
		1,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);

	if (upscaling.IsBackendInitialized()) {
		upscaling.UpgradeBackendInterface((void**)&(*ppDevice));
		upscaling.UpgradeBackendInterface((void**)&(*ppSwapChain));
		upscaling.SetBackendD3DDevice(*ppDevice);
		// Feature availability (notably Reflex/PCL) is only reliable after device bind.
		upscaling.CheckBackendFeatures(pAdapter);
		upscaling.PostBackendDevice();
	}

	return ret;
}

// VR PerfMode: on-by-default performance feature. The setting persists across method
// switches (we don't auto-flip it when the user picks TAA/NONE), but the checkbox is
// disabled outside upscalers that can target a separate displayRes output (DLSS, FSR);
// kept visible-but-greyed so users see the option exists. Restart-gated: the BSOpenVR
// size hook reads this at world load and sizes every engine RT off the boot value.
void Upscaling::DrawPerfModeToggle()
{
	const auto upscaleMethod = GetUpscaleMethod();
	const bool methodSupportsPerf =
		upscaleMethod == UpscaleMethod::kDLSS ||
		upscaleMethod == UpscaleMethod::kFSR;
	if (!methodSupportsPerf)
		ImGui::BeginDisabled();
	ImGui::Checkbox(T(TKEY("render_at_upscale_res"), "Render engine at upscaled resolution"), &settings.renderAtUpscaleRes);
	if (!methodSupportsPerf)
		ImGui::EndDisabled();
	// Hover tooltip always renders (so users learn what the option does even when greyed out).
	// The pending-restart banner fires only when DLSS or FSR is the active upscaler; the
	// feature can't take effect otherwise, so a "pending restart" hint there would mislead.
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("render_at_upscale_res_tooltip"),
							  "On by default. The engine pipeline allocates render targets at the upscaled-render\n"
							  "resolution instead of the HMD display resolution; the upscaler (DLSS or FSR) writes\n"
							  "its output to a private DisplayRes texture. Substantial VRAM and bandwidth savings,\n"
							  "especially at high HMD resolutions.\n"
							  "\n"
							  "Locked to the Upscale Preset selected at launch: changing the preset (or this\n"
							  "toggle) takes effect after a game restart. At Native AA (1.0x) there is no\n"
							  "render-res reduction, so the lock stays off and preset changes apply live.\n"
							  "\n"
							  "Requires DLSS or FSR. Sharpness / model preset / Reflex remain live."));
	}
	if (!methodSupportsPerf && settings.renderAtUpscaleRes)
		Util::Text::Disabled(T(TKEY("render_at_upscale_res_requires"), "Render-at-upscaled-resolution requires DLSS or FSR. Switch upscaler Method to activate."));
	// At Native AA (1x) the size hook is a no-op unless an explicit scale engages
	// it; surface that rather than implying the toggle does something.
	if (methodSupportsPerf && settings.renderAtUpscaleRes &&
		GetQualityModeRatio(settings.qualityMode) <= 1.0f && settings.vrRenderScale <= 0.0f)
		Util::Text::Disabled(T(TKEY("render_at_upscale_res_native_noop"), "No effect at Native AA (1x): renders at full resolution; raise the Upscale Preset to engage."));
	if (methodSupportsPerf)
		Util::UI::DrawSettingDiff(bootSnapshot, settings, &Settings::renderAtUpscaleRes);

	const bool scaleEditable = methodSupportsPerf && settings.renderAtUpscaleRes;
	if (!scaleEditable)
		ImGui::BeginDisabled();
	// Leftmost slider stop is the Auto sentinel (stored as 0).
	constexpr float kAutoStop = kVRRenderScaleMin * 100.0f - 1.0f;
	float scalePercent = settings.vrRenderScale > 0.0f ? settings.vrRenderScale * 100.0f : kAutoStop;
	const char* scaleFormat = scalePercent <= kAutoStop + 0.5f ? T(TKEY("vr_render_scale_auto"), "Auto") : "%.0f%%";
	if (ImGui::SliderFloat(T(TKEY("vr_render_scale"), "VR Render Scale"), &scalePercent, kAutoStop, kVRRenderScaleMax * 100.0f, scaleFormat))
		settings.vrRenderScale = scalePercent <= kAutoStop + 0.5f ? 0.0f : scalePercent / 100.0f;
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("vr_render_scale_tooltip"),
							  "Renders the game at a percentage of your headset's resolution, then\n"
							  "upscales it with DLSS or FSR. Auto follows the Upscale Preset.\n"
							  "Takes effect after restarting the game."));
	}
	if (!scaleEditable)
		ImGui::EndDisabled();
	if (methodSupportsPerf)
		Util::UI::DrawSettingDiff(bootSnapshot, settings, &Settings::vrRenderScale);
}

// FoveatedRender: foveated subrect DLSS, VR-only, opt-in. Enable lives at the top level
// for discoverability; the body knobs are collapsed by default and greyed until opted in.
void Upscaling::DrawFoveationControls(bool showTuning)
{
	ImGui::Separator();
	foveatedRender.DrawEnable();
	// Hub view shows just the enable; the deep tuning tree lives in the Upscaling panel.
	if (!showTuning)
		return;
	const bool enabled = foveatedRender.settings.enabled != 0;
	if (!enabled)
		ImGui::BeginDisabled();
	if (ImGui::TreeNodeEx(T(TKEY("foveated_tuning"), "Foveated Upscaling Tuning"))) {
		foveatedRender.DrawSettings();
		ImGui::TreePop();
	}
	if (!enabled)
		ImGui::EndDisabled();
}

// Narrower than the feature name: the hub section only covers the VR perf knobs.
std::string Upscaling::GetPerformanceSectionLabel()
{
	return T(TKEY("vr_perf_upscaling_header"), "Upscaling & Foveation");
}

// Central Performance hub view: the restart-pending diff for qualityMode, the one
// thing the hub's own Performance/Balanced/Quality button row (which already shows
// which preset is active via highlighting) can't convey on its own.
void Upscaling::DrawPerformancePresets()
{
	// Only meaningful when an upscaler is active; match the same gate DrawSettings
	// uses so the hub doesn't show an inert diff for None/TAA.
	const auto upscaleMethod = GetUpscaleMethod();
	if (upscaleMethod == UpscaleMethod::kNONE || upscaleMethod == UpscaleMethod::kTAA)
		return;
	// No pending-restart diff while an explicit scale owns the render res:
	// the preset is inert and a restart wouldn't apply it.
	if (perfMode.IsHookActive() && !perfMode.IsExplicitScaleLatched())
		Util::UI::DrawSettingDiff(bootSnapshot, settings, &Settings::qualityMode);
}

// Central Performance hub view: the render-res PerfMode toggle and Foveated DLSS,
// the two upscaler-owned VR perf knobs, bound to the same settings the upscaler panel shows.
void Upscaling::DrawPerformanceSettings()
{
	DrawPerfModeToggle();
	// Foveated DLSS is VR-only; hide rather than disable so flat users never see a
	// control they can never activate (mirrors the gate at DrawSettings' own call site).
	if (globals::game::isVR)
		DrawFoveationControls(false);
}

namespace
{
	struct UpscalePreset
	{
		uint qualityMode;
		bool foveation;
		const char* cropPresetName;
		FoveatedRender::DlssMode dlssMode;
		FoveatedRender::StretchMode stretchMode;
		FoveatedRender::PeripheryAAMode peripheryAAMode;
		FoveatedRender::SubrectBlendMode subrectBlendMode;
	};

	// Single source of truth for Apply/MatchesPerformanceProfile below.
	constexpr UpscalePreset GetUpscalePreset(Feature::PerfProfile profile)
	{
		switch (profile) {
		case Feature::PerfProfile::Performance:
			return { (uint)Upscaling::QualityMode::kPerformance, true, FoveatedRender::kPresetCenter50,
				FoveatedRender::DlssMode::kFaster, FoveatedRender::StretchMode::kBilinear,
				FoveatedRender::PeripheryAAMode::kNone, FoveatedRender::SubrectBlendMode::kDither };
		case Feature::PerfProfile::Balanced:
			return { (uint)Upscaling::QualityMode::kBalanced, true, FoveatedRender::kPresetCenter75,
				FoveatedRender::DlssMode::kDefault, FoveatedRender::StretchMode::kGaussianBlur,
				FoveatedRender::PeripheryAAMode::kTemporalSmooth, FoveatedRender::SubrectBlendMode::kFeather };
		default:
			return { (uint)Upscaling::QualityMode::kQuality, false, FoveatedRender::kPresetFullEye,
				FoveatedRender::DlssMode::kDefault, FoveatedRender::StretchMode::kGaussianBlur,
				FoveatedRender::PeripheryAAMode::kTemporalSmooth, FoveatedRender::SubrectBlendMode::kFeather };
		}
	}
}

// Single source for preset display names; the native tier reads DLAA under DLSS, Native AA otherwise.
const char* Upscaling::GetQualityModeName(uint qualityMode) const
{
	switch (std::min(qualityMode, (uint)QualityMode::kUltraPerformance)) {
	case (uint)QualityMode::kNativeAA:
		return GetUpscaleMethod() == UpscaleMethod::kDLSS ? T(TKEY("preset_dlaa"), "DLAA") : T(TKEY("preset_native_aa"), "Native AA");
	case (uint)QualityMode::kQuality:
		return T(TKEY("preset_quality"), "Quality");
	case (uint)QualityMode::kBalanced:
		return T(TKEY("preset_balanced"), "Balanced");
	case (uint)QualityMode::kPerformance:
		return T(TKEY("preset_performance"), "Performance");
	default:
		return T(TKEY("preset_ultra_performance"), "Ultra Performance");
	}
}

// PerfMode stays on for every profile; qualityMode and foveation latch at boot.
// Profiles are preset-driven, so scale overrides are cleared.
void Upscaling::ApplyPerformanceProfile(PerfProfile profile)
{
	const auto preset = GetUpscalePreset(profile);
	settings.renderAtUpscaleRes = true;
	settings.qualityMode = preset.qualityMode;
	settings.vrRenderScale = 0.0f;
	// Only fills the gap (never overrides an existing DLSS-or-FSR choice); Quality never
	// forces one. GetUpscaleMethod() reads upscaleMethod/upscaleMethodNoDLSS depending on
	// DLSS availability -- write whichever field it reads.
	if (profile != PerfProfile::Quality) {
		const auto currentMethod = GetUpscaleMethod();
		const bool needsUpscaler = currentMethod != UpscaleMethod::kDLSS && currentMethod != UpscaleMethod::kFSR;
		if (needsUpscaler) {
			if (streamline.featureDLSS)
				settings.upscaleMethod = (uint)UpscaleMethod::kDLSS;
			else
				settings.upscaleMethodNoDLSS = (uint)UpscaleMethod::kFSR;
		}
	}
	// Foveation is VR-only (DrawFoveationControls/IsRuntimeSupported); leave it alone on Flat.
	if (globals::game::isVR) {
		foveatedRender.settings.enabled = preset.foveation ? 1 : 0;
		if (preset.foveation) {
			foveatedRender.settings.dlssMode = (uint)preset.dlssMode;
			foveatedRender.settings.stretchMode = (uint)preset.stretchMode;
			foveatedRender.settings.peripheryAAMode = (uint)preset.peripheryAAMode;
			foveatedRender.settings.subrectBlendMode = (uint)preset.subrectBlendMode;
			// A user-dragged custom crop is sticky across profile switches -- never
			// silently overwrite it with a preset region.
			if (!foveatedRender.subrectController.HasCustomCrop())
				foveatedRender.subrectController.ApplyPresetByName(preset.cropPresetName);
		}
	}
}

bool Upscaling::MatchesPerformanceProfile(PerfProfile profile) const
{
	const auto preset = GetUpscalePreset(profile);
	if (!(settings.renderAtUpscaleRes &&
			settings.vrRenderScale == 0.0f &&
			settings.qualityMode == preset.qualityMode)) {
		return false;
	}
	// Match on "any redirecting method", not a hardcoded expected one -- ApplyPerformanceProfile
	// never overrides an existing DLSS-or-FSR choice.
	if (profile != PerfProfile::Quality) {
		const auto method = GetUpscaleMethod();
		if (method != UpscaleMethod::kDLSS && method != UpscaleMethod::kFSR) {
			return false;
		}
	}
	if (!globals::game::isVR) {
		return true;
	}
	if ((foveatedRender.settings.enabled != 0) != preset.foveation) {
		return false;
	}
	if (!preset.foveation) {
		return true;
	}
	if (foveatedRender.settings.dlssMode != (uint)preset.dlssMode ||
		foveatedRender.settings.stretchMode != (uint)preset.stretchMode ||
		foveatedRender.settings.peripheryAAMode != (uint)preset.peripheryAAMode ||
		foveatedRender.settings.subrectBlendMode != (uint)preset.subrectBlendMode) {
		return false;
	}
	// A sticky custom crop still counts as "matching" -- ApplyPerformanceProfile
	// deliberately leaves it alone rather than forcing the preset region.
	if (foveatedRender.subrectController.HasCustomCrop()) {
		return true;
	}
	const auto expectedUV = foveatedRender.subrectController.FindPresetUV(preset.cropPresetName);
	if (!expectedUV) {
		return false;
	}
	const auto& currentUV = foveatedRender.subrectController.GetUV();
	return currentUV.x == expectedUV->x && currentUV.y == expectedUV->y &&
	       currentUV.w == expectedUV->w && currentUV.h == expectedUV->h;
}

std::string Upscaling::GetProfilePreviewText(PerfProfile profile) const
{
	if (!globals::game::isVR)
		return "";
	const auto preset = GetUpscalePreset(profile);
	if (!preset.foveation)
		return T(TKEY("profile_preview_foveation_off"), "Foveation off (Full Eye)");
	// ApplyPerformanceProfile leaves a custom crop alone rather than switching to the
	// preset region -- the preview must match, or it promises a crop change that won't happen.
	const std::string cropLabel = foveatedRender.subrectController.HasCustomCrop() ?
	                                  T(TKEY("profile_preview_custom_crop"), "custom crop preserved") :
	                                  preset.cropPresetName;
	const char* dlssModeName = FoveatedRender::DlssModeName(preset.dlssMode);
	const char* stretchModeName = FoveatedRender::StretchModeName(preset.stretchMode);
	const char* peripheryAAName = FoveatedRender::PeripheryAAModeName(preset.peripheryAAMode);
	const char* blendModeName = FoveatedRender::SubrectBlendModeName(preset.subrectBlendMode);
	return std::vformat(T(TKEY("profile_preview_format"), "Foveation: {} / {} / {} / {} / {} blend"),
		std::make_format_args(cropLabel, dlssModeName, stretchModeName, peripheryAAName, blendModeName));
}

void Upscaling::RegisterUxActions()
{
	FEATURE_COMMAND("applyFoveationPreset",
		"Apply a named foveation crop preset (see openshaders.feature get shortName=Upscaling -> foveatedRender.CropPresets[].name, e.g. \"Center 75%\") -- the same code path as clicking the preset dropdown, including right-eye auto-mirror. Params: name (string).",
		[](Feature*, const json& args) {
			foveatedRender.subrectController.ApplyPresetByName(args.value("name", std::string{}));
		});
}

void Upscaling::DrawSettings()
{
	// Force method to None up front so the picker reflects the locked state.
	ApplyOpenCompositeUpscalingBlocker();
	const auto& openCompositeBlocker = GetOpenCompositeUpscalingBlocker();
	const bool openCompositeBlocksUpscaling = openCompositeBlocker.active;

	// Display upscaling options in the UI
	std::vector<std::string> upscaleModes = {
		T(TKEY("method_none"), "None"),
		T(TKEY("method_taa"), "TAA")
	};

	std::string fsrLabel = "AMD FSR 3.1";
	upscaleModes.push_back(fsrLabel);

	std::string dlssLabel = "NVIDIA DLSS";
	upscaleModes.push_back(dlssLabel);

	// Determine available modes
	bool featureDLSS = streamline.featureDLSS;
	bool featureFSR = true;  // FSR is always available

	uint32_t* currentUpscaleMode = &settings.upscaleMethod;
	uint32_t availableModes = 1;  // Start with TAA
	if (featureFSR)
		availableModes = 2;  // Add FSR
	if (featureDLSS)
		availableModes = 3;  // Add DLSS if available
	else
		currentUpscaleMode = &settings.upscaleMethodNoDLSS;

	// Dropdown for method selection
	std::vector<const char*> modeLabels;
	for (uint32_t i = 0; i <= availableModes; ++i)
		modeLabels.push_back(upscaleModes[i].c_str());
	if (openCompositeBlocksUpscaling)
		ImGui::BeginDisabled();
	ImGui::Combo(T(TKEY("method"), "Method"), (int*)currentUpscaleMode, modeLabels.data(), (int)modeLabels.size());
	if (openCompositeBlocksUpscaling)
		ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		if (openCompositeBlocksUpscaling)
			ImGui::Text(T(TKEY("method_locked_opencomposite"), "Locked to None while OpenComposite has %s=true."), openCompositeBlocker.settingName.c_str());
		else
			ImGui::TextUnformatted(T(TKEY("method_tooltip"), "Selects the upscaling backend."));
	}

	*currentUpscaleMode = std::min(availableModes, *currentUpscaleMode);

	if (openCompositeBlocksUpscaling) {
		if (openCompositeBlocker.configPath.empty())
			Util::Text::WrappedWarning(
				"Upscaling is locked to None because OpenComposite has %s=true.",
				openCompositeBlocker.settingName.c_str());
		else
			Util::Text::WrappedWarning(
				"Upscaling is locked to None because OpenComposite has %s=true in %s.",
				openCompositeBlocker.settingName.c_str(),
				openCompositeBlocker.configPath.c_str());
	}

	// Check the current upscale method
	auto upscaleMethod = GetUpscaleMethod();

	// PerfMode: BSOpenVR size hook + RT::Create run once at world load, so
	// runtime reads of method/qualityMode route through the boot snapshot.
	// The always-present explanation is plain text — only the staged-change
	// diff uses the RestartNeeded color so users learn the cue means "you
	// changed something that won't apply yet."
	if (perfMode.IsHookActive()) {
		ImGui::TextWrapped(T(TKEY("perfmode_active_note"),
			"Render-at-upscaled-resolution is active: Method and Upscale Preset changes only take effect after a game restart. "
			"Sharpness / model preset / Reflex remain live."));

		// Method pending-diff. Only fires when the user is editing the DLSS-
		// path mode slot (upscaleMethod, not upscaleMethodNoDLSS), since
		// that's the one the boot snapshot locked.
		if (currentUpscaleMode == &settings.upscaleMethod &&
			bootSnapshot.HasPendingChange(settings, &Settings::upscaleMethod)) {
			const uint live = std::clamp<uint>(settings.upscaleMethod, 0u, availableModes);
			const uint boot = std::clamp<uint>(bootSnapshot.Boot(&Settings::upscaleMethod), 0u, availableModes);
			Util::Text::RestartNeeded(
				"Pending restart: currently active method = %s (selected = %s).",
				upscaleModes[boot].c_str(), upscaleModes[live].c_str());
		}
	}

	// Display warning for DLSS resolution limits (non-VR only; VR handles this automatically)
	if (!globals::game::isVR && upscaleMethod == UpscaleMethod::kDLSS) {
		auto screenSize = globals::state->screenSize;
		if (screenSize.x > streamline.MAX_RESOLUTION || screenSize.y > streamline.MAX_RESOLUTION) {
			Util::Text::Warning(T(TKEY("dlss_resolution_warning"), "Warning: Requested resolution %.0f x %.0f exceeds maximum supported resolution %d x %d for DLSS."),
				screenSize.x, screenSize.y, streamline.MAX_RESOLUTION, streamline.MAX_RESOLUTION);
			Util::Text::Warning(T(TKEY("dlss_will_not_function"), "DLSS will not function. Lower your resolution or select a different upscaling method."));
		}
	}

	// Display upscaling settings if applicable
	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		const char* baseLabel = GetQualityModeName(settings.qualityMode);

		if (baseLabel) {
			// Derive scale from live `settings.qualityMode` — `resolution-
			// Scale` is locked to the PerfMode boot snapshot, so reusing it
			// here would mismatch the slider position the user sees.
			const float displayScale = 1.0f / GetQualityModeRatio(settings.qualityMode);
			std::string labelWithScale = std::format("{} ( {:.2f}x )", baseLabel, displayScale);

			ImGui::SliderInt(T(TKEY("upscale_preset"), "Upscale Preset"), (int*)&settings.qualityMode, 0, 4, labelWithScale.c_str());

			// Pending-diff vs the boot snapshot the runtime upscaler actually
			// uses; while an explicit scale is latched the preset is inert.
			if (perfMode.IsExplicitScaleLatched()) {
				Util::Text::Disabled(T(TKEY("upscale_preset_ignored_render_scale"),
					"The Upscale Preset is ignored while VR Render Scale is set. Move it back to Auto to use the preset again (restart required)."));
			} else if (perfMode.IsHookActive() &&
					   bootSnapshot.HasPendingChange(settings, &Settings::qualityMode)) {
				const uint bm = std::clamp<uint>(bootSnapshot.Boot(&Settings::qualityMode), 0u, 4u);
				const char* bootLabel = GetQualityModeName(bm);
				Util::Text::RestartNeeded(
					"Pending restart: currently active = %s ( %.2fx ). Change applies after game restart.",
					bootLabel, 1.0f / GetQualityModeRatio(bm));
			}
		}

		if (upscaleMethod == UpscaleMethod::kFSR) {
			ImGui::SliderFloat(T(TKEY("sharpness"), "Sharpness"), &settings.sharpnessFSR, 0.0f, 1.0f, "%.1f");
			// Hidden entirely on ineligible GPUs so it can't be toggled somewhere it silently no-ops.
			if (fidelityFX.IsRuntimeFsr4AutoEligible()) {
				ImGui::Checkbox(T(TKEY("fsr4_runtime_enable"), "Use Runtime FSR4"), &settings.fsr4RuntimeEnable);
				if (settings.fsr4RuntimeEnable) {
					ImGui::TextDisabled("%s: %s", T(TKEY("fsr4_active_path"), "Active path"), fidelityFX.GetDisplayedFsrPathLabel().c_str());
					if (fidelityFX.IsRuntimeFsr4FailureLatched())
						Util::Text::Warning(T(TKEY("fsr4_failed_fallback"), "Runtime FSR4 failed this session -- using FSR3 fallback."));
					else if (fidelityFX.IsRuntimeUpscalerFailureLatched())
						Util::Text::Warning(T(TKEY("fsr4_runtime_failed_fallback"), "Runtime upscaler DLL failed this session -- using host FSR3 SDK."));
				}
			}
		} else if (upscaleMethod == UpscaleMethod::kDLSS) {
			ImGui::Checkbox(T(TKEY("enable_sharpening"), "Enable Sharpening"), &settings.sharpnessEnabledDLSS);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("enable_sharpening_tooltip"),
									  "Applies RCAS sharpening to the DLSS output.\n"
									  "Off by default; DLSS already resolves a sharp image."));
			}

			if (settings.sharpnessEnabledDLSS)
				ImGui::SliderFloat(T(TKEY("sharpness"), "Sharpness"), &settings.sharpnessDLSS, 0.0f, 1.0f, "%.1f");

			const char* presets[] = {
				T(TKEY("dlss_model_preset_default"), "Default"),
				T(TKEY("dlss_model_preset_j"), "Preset J"),
				T(TKEY("dlss_model_preset_k"), "Preset K"),
				T(TKEY("dlss_model_preset_l"), "Preset L"),
				T(TKEY("dlss_model_preset_m"), "Preset M")
			};
			ImGui::Combo(T(TKEY("dlss_model_preset"), "DLSS Model Preset"), (int*)&settings.presetDLSS, presets, 5);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("dlss_model_preset_tooltip"),
									  "Choose which DLSS AI model preset to use.\n"
									  "Default lets the NVIDIA runtime choose for each upscale mode.\n"
									  "Current defaults are K for DLAA, Quality, and Balanced; M for Performance; and L for Ultra Performance.\n"
									  "An explicit preset overrides every upscale mode."));
			}
		}

		// VR PerfMode toggle, discovered here alongside the upscaler controls,
		// and mirrored in the Performance hub.
		if (globals::game::isVR)
			DrawPerfModeToggle();
	}

	const bool frameGenerationDx12PathActive = IsFrameGenerationDx12PathActive();

	if (!globals::game::isVR) {
		if (ImGui::TreeNodeEx(T(TKEY("frame_generation"), "Frame Generation"), ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("%s", T(TKEY("frame_generation_desc"),
								  "Frame Generation interpolates real frames with generated ones for a smoother experience"));

			bool fgEnabled = settings.frameGenerationMode != 0;
			if (ImGui::Checkbox(T(TKEY("frame_generation"), "Frame Generation"), &fgEnabled))
				settings.frameGenerationMode = fgEnabled ? 1 : 0;
			Util::UI::RestartGatedAnnotate(bootSnapshot, settings, &Settings::frameGenerationMode,
				T(TKEY("frame_generation_tooltip"),
					"Interpolate real frames with generated ones for a smoother experience. Uses NVIDIA\n"
					"DLSS-G or AMD FSR Frame Generation depending on the adapter and preference below.\n"
					"Requires a D3D11-to-D3D12 proxy swapchain which can introduce compatibility issues;\n"
					"in particular, frame generation works only in windowed mode."));

			auto fgMethod = GetFrameGenMethod();
			if (fgMethod == FrameGenMethod::kDLSSG) {
				ImGui::TextColored(Util::Colors::GetSuccess(), "%s", T(TKEY("frame_generation_dlssg_active"), "Using NVIDIA DLSS Frame Generation (Auto)"));
			} else if (fgMethod == FrameGenMethod::kFSR) {
				if (streamlineDX12.featureDLSSG)
					ImGui::TextColored(Util::Colors::GetInfo(), "%s", T(TKEY("frame_generation_fsr_active_preferred"), "Using AMD FSR Frame Generation (Preferred)"));
				else
					ImGui::TextColored(Util::Colors::GetInfo(), "%s", T(TKEY("frame_generation_fsr_active"), "Using AMD FSR Frame Generation (Auto)"));
			} else {
				if (streamlineDX12.featureDLSSG)
					ImGui::Text("%s", T(TKEY("frame_generation_dlssg_available"),
										  "NVIDIA DLSS Frame Generation is available."));
				else if (fidelityFX.featureFSR3FG)
					ImGui::Text("%s", T(TKEY("frame_generation_fsr_available"),
										  "AMD FSR Frame Generation is available."));
			}

			if (streamlineDX12.featureDLSSG) {
				ImGui::Checkbox(T(TKEY("prefer_fsr_frame_gen"), "Prefer AMD FSR Frame Generation"), &settings.preferFSRFrameGen);
				Util::UI::RestartGatedAnnotate(bootSnapshot, settings, &Settings::preferFSRFrameGen,
					T(TKEY("prefer_fsr_frame_gen_tooltip"),
						"Uses AMD FSR3 Frame Generation instead of NVIDIA DLSS-G. This is a workaround for\n"
						"cases where DLSS-G initializes successfully but produces no interpolated frames.\n"
						"Restart required to apply."));
			}

			if (fgMethod == FrameGenMethod::kDLSSG) {
				int multiplier = static_cast<int>(settings.dlssgFramesToGenerate) + 1;
				int maxMultiplier = static_cast<int>(streamlineDX12.dlssgMaxFramesToGenerate) + 1;
				if (ImGui::SliderInt(T(TKEY("dlssg_frame_multiplier"), "DLSS-G Frame Multiplier"), &multiplier, 2, maxMultiplier))
					settings.dlssgFramesToGenerate = static_cast<uint>(multiplier - 1);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T(TKEY("dlssg_frame_multiplier_tooltip"), "How many total frames are shown per rendered frame. Higher values generate more frames."));
			} else if (fgMethod == FrameGenMethod::kFSR) {
				ImGui::Text("%s", T(TKEY("fsr_frame_gen_fixed_multiplier"), "AMD FSR Frame Generation: Fixed 2x"));
			}

			ImGui::Text("%s", T(TKEY("frame_generation_proxy_note"), "Requires a D3D11 to D3D12 proxy which can create compatibility issues"));

			if (!isWindowed) {
				Util::Text::Warning(T(TKEY("fg_warn_windowed"), "Warning: Requires windowed mode"));
			}

			if (lowRefreshRate && !settings.frameGenerationForceEnable) {
				Util::Text::Warning(T(TKEY("fg_warn_refresh_rate"), "Warning: Requires a high refresh rate monitor or Force Enable Frame Generation"));
			}

			if (fidelityFXMissing) {
				Util::Text::Warning(T(TKEY("fg_warn_fidelityfx_missing"), "Warning: FidelityFX DLLs are not loaded"));
			}

			if (!frameGenerationDx12PathActive)
				ImGui::BeginDisabled();

			bool flEnabled = settings.frameLimitMode != 0;
			if (ImGui::Checkbox(T(TKEY("frame_limit_vrr"), "Frame Limit (Variable Refresh Rate)"), &flEnabled))
				settings.frameLimitMode = flEnabled ? 1 : 0;

			if (!frameGenerationDx12PathActive)
				ImGui::EndDisabled();

			ImGui::TextWrapped(T(TKEY("frame_limit_refresh_rate"), "Allows frame generation to function on low refresh rate monitors. Detected: %.2f Hz"), refreshRate);
			bool fgForce = settings.frameGenerationForceEnable != 0;
			if (ImGui::Checkbox(T(TKEY("force_enable_frame_generation"), "Force Enable Frame Generation"), &fgForce))
				settings.frameGenerationForceEnable = fgForce ? 1 : 0;
			Util::UI::RestartGatedAnnotate(bootSnapshot, settings, &Settings::frameGenerationForceEnable,
				T(TKEY("force_enable_frame_generation_tooltip"),
					"Bypass the high-refresh-rate monitor check so Frame Generation can run on lower-Hz\n"
					"displays. Useful for laptops and older monitors at the cost of less headroom for the\n"
					"generated frames."));

			ImGui::Checkbox(T(TKEY("frame_generation_in_menus"), "Frame Generation in Menus"), &settings.frameGenerationAllowInMenus);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("frame_generation_in_menus_tooltip_1"), "Keeps frame generation active while game menus are open."));
				ImGui::TextUnformatted(T(TKEY("frame_generation_in_menus_tooltip_2"), "May feel smoother, but increases menu input latency."));
			}

			ImGui::TreePop();
		}
	}

	const bool reflexSupported = streamline.reflexSupportedOnCurrentAdapter || streamlineDX12.reflexSupportedOnCurrentAdapter;
	if (reflexSupported && ImGui::TreeNodeEx(T(TKEY("nvidia_reflex"), "NVIDIA Reflex"), ImGuiTreeNodeFlags_DefaultOpen)) {
		const bool usingDX12Reflex = UsesDLSSGFrameGen();
		auto& activeReflex = usingDX12Reflex ? streamlineDX12 : streamline;
		const bool reflexAvailable = activeReflex.initialized && activeReflex.featureReflex;
		const bool markerOptimizationAvailable = reflexAvailable && activeReflex.featurePCL;

		if (usingDX12Reflex) {
			ImGui::Text("%s", T(TKEY("reflex_via_dx12"), "Reflex is running via DX12 (DLSS Frame Generation active)."));
		}

		if (!reflexAvailable) {
			ImGui::TextDisabled("%s", T(TKEY("reflex_not_available"), "Reflex is not available. Ensure sl.reflex.dll is present and restart."));
		}

		if (!reflexAvailable)
			ImGui::BeginDisabled();

		ImGui::Checkbox(T(TKEY("low_latency_mode"), "Low Latency Mode"), &settings.reflexLowLatencyMode);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("low_latency_mode_tooltip_1"), "Cuts input delay by syncing CPU work closer to the GPU."));
			ImGui::TextUnformatted(T(TKEY("low_latency_mode_tooltip_2"), "Can reduce max FPS a little, but usually feels more responsive."));
		}

		if (!settings.reflexLowLatencyMode)
			ImGui::BeginDisabled();

		ImGui::Checkbox(T(TKEY("low_latency_boost"), "Low Latency Boost"), &settings.reflexLowLatencyBoost);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("low_latency_boost_tooltip_1"), "Keeps GPU clocks higher to avoid latency spikes at low GPU load."));
			ImGui::TextUnformatted(T(TKEY("low_latency_boost_tooltip_2"), "Useful if frametime jumps; costs extra power and heat."));
		}

		if (!markerOptimizationAvailable)
			ImGui::BeginDisabled();

		ImGui::Checkbox(T(TKEY("use_markers_to_optimize"), "Use Markers To Optimize"), &settings.reflexUseMarkersToOptimize);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("use_markers_to_optimize_tooltip_1"), "Uses frame markers for tighter Reflex timing."));
			ImGui::TextUnformatted(T(TKEY("use_markers_to_optimize_tooltip_2"), "Try On first; turn Off if it causes stutter on your setup."));
		}

		if (!markerOptimizationAvailable)
			ImGui::EndDisabled();

		if (!markerOptimizationAvailable) {
			ImGui::TextDisabled("%s", T(TKEY("marker_optimization_unavailable"), "Marker optimization unavailable (PCL not loaded)."));
		}

		ImGui::Checkbox(T(TKEY("use_fps_limit"), "Use FPS Limit"), &settings.reflexUseFPSLimit);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("use_fps_limit_tooltip_1"), "Uses Reflex's internal FPS cap for steadier frametimes."));
			ImGui::TextUnformatted(T(TKEY("use_fps_limit_tooltip_2"), "Can lower latency versus uncapped rendering."));
		}

		if (!settings.reflexLowLatencyMode)
			ImGui::EndDisabled();

		if (!settings.reflexUseFPSLimit)
			ImGui::BeginDisabled();

		if (!std::isfinite(settings.reflexFPSLimit))
			settings.reflexFPSLimit = 60.0f;
		settings.reflexFPSLimit = std::clamp(settings.reflexFPSLimit, 20.0f, 240.0f);
		ImGui::SliderFloat(T(TKEY("fps_limit"), "FPS Limit"), &settings.reflexFPSLimit, 20.0f, 240.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("fps_limit_tooltip_1"), "Set your frame cap target."));
			ImGui::TextUnformatted(T(TKEY("fps_limit_tooltip_2"), "Start about 2-3 FPS below refresh rate (e.g. 117 for 120 Hz)."));
		}

		if (!settings.reflexUseFPSLimit)
			ImGui::EndDisabled();

		if (!reflexAvailable)
			ImGui::EndDisabled();

		ImGui::TreePop();
	}

	// Foveated DLSS lives here rather than as a peer Feature so all DLSS surfaces share
	// one settings panel; also mirrored in the Performance hub.
	if (globals::game::isVR)
		DrawFoveationControls();

	if (ImGui::TreeNodeEx(T(TKEY("backend_diagnostics"), "Backend Diagnostics"))) {
		// Streamline log level selection
		const char* logLevels[] = {
			T(TKEY("streamline_log_level_off"), "Off"),
			T(TKEY("streamline_log_level_default"), "Default"),
			T(TKEY("streamline_log_level_verbose"), "Verbose")
		};
		// streamlineLogLevel is sanitized in LoadSettings (runs on every load,
		// not gated on this node being expanded), so the stored value is in range.
		int logLevelIdx = static_cast<int>(settings.streamlineLogLevel);
		if (ImGui::Combo(T(TKEY("streamline_logging"), "Streamline Logging"), &logLevelIdx, logLevels, IM_ARRAYSIZE(logLevels))) {
			settings.streamlineLogLevel = static_cast<uint>(logLevelIdx);
		}
		Util::UI::RestartGatedAnnotate(bootSnapshot, settings, &Settings::streamlineLogLevel,
			T(TKEY("streamline_logging_tooltip"),
				"Verbosity of the NVIDIA Streamline backend logs. Useful for debugging issues with DLSS / "
				"DLSS-G."));

		// VR Debug visualization -- per-eye buffers and native inputs
		if (globals::game::isVR) {
			ImGui::Separator();
			static float debugRescale = 0.15f;
			ImGui::SliderFloat(T(TKEY("view_resize"), "View Resize"), &debugRescale, 0.05f, 1.f);

			if (ImGui::TreeNode(T(TKEY("upscaling_intermediates"), "Upscaling Intermediates"))) {
				if (vrIntermediateMotionVectors[0]) {
					bool isDLSS = GetUpscaleMethod() == UpscaleMethod::kDLSS;
					if (vrIntermediateColorIn[0] && vrIntermediateColorOut[0]) {
						BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorIn[0], "Left Eye In", debugRescale)
						BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorIn[1], "Right Eye In", debugRescale)
						if (!isDLSS)
							BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorOut[0], "Left Eye Out", debugRescale)
						BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorOut[1], "Right Eye Out", debugRescale)
					}
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateMotionVectors[0], "Left Eye MVec", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateMotionVectors[1], "Right Eye MVec", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateReactiveMask[0], "Left Eye Reactive", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateReactiveMask[1], "Right Eye Reactive", debugRescale)
					if (vrIntermediateTransparencyMask[0]) {
						BUFFER_VIEWER_NODE_TITLE(vrIntermediateTransparencyMask[0], "Left Eye Transparency", debugRescale)
						BUFFER_VIEWER_NODE_TITLE(vrIntermediateTransparencyMask[1], "Right Eye Transparency", debugRescale)
					}
				} else {
					ImGui::TextDisabled("%s", T(TKEY("vr_intermediates_not_created"), "VR intermediates not yet created (enter game world)"));
				}
				ImGui::TreePop();
			}

			if (ImGui::TreeNode(T(TKEY("native_inputs"), "Native Inputs"))) {
				auto renderer = globals::game::renderer;
				auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
				auto& mvec = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
				auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

				auto DisplayRT = [&](const char* label, ID3D11Texture2D* tex, ID3D11ShaderResourceView* srv) {
					if (srv && tex) {
						D3D11_TEXTURE2D_DESC desc;
						tex->GetDesc(&desc);
						char buf[128];
						snprintf(buf, sizeof(buf), "%s (%ux%u)", label, desc.Width, desc.Height);
						if (ImGui::TreeNode(buf)) {
							ImGui::Image(srv, { desc.Width * debugRescale, desc.Height * debugRescale });
							ImGui::TreePop();
						}
					}
				};

				DisplayRT("kMAIN (Color Input)", (ID3D11Texture2D*)main.texture, (ID3D11ShaderResourceView*)main.SRV);
				DisplayRT("Motion Vectors", (ID3D11Texture2D*)mvec.texture, (ID3D11ShaderResourceView*)mvec.SRV);
				DisplayRT("Depth", depth.texture, depth.depthSRV);

				if (reactiveMaskTexture)
					BUFFER_VIEWER_NODE_TITLE(reactiveMaskTexture, "Reactive Mask", debugRescale)
				if (transparencyCompositionMaskTexture)
					BUFFER_VIEWER_NODE_TITLE(transparencyCompositionMaskTexture, "Transparency Mask", debugRescale)

				ImGui::TreePop();
			}
		}

		// VR Debug visualization -- per-eye buffers and native inputs
		if (globals::game::isVR) {
			ImGui::Separator();
			static float debugRescale = 0.15f;
			ImGui::SliderFloat(T(TKEY("view_resize"), "View Resize"), &debugRescale, 0.05f, 1.f);

			if (ImGui::TreeNode(T(TKEY("upscaling_intermediates"), "Upscaling Intermediates"))) {
				if (vrIntermediateMotionVectors[0]) {
					bool isDLSS = GetUpscaleMethod() == UpscaleMethod::kDLSS;
					if (vrIntermediateColorIn[0] && vrIntermediateColorOut[0]) {
						BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorIn[0], "Left Eye In", debugRescale)
						BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorIn[1], "Right Eye In", debugRescale)
						if (!isDLSS)
							BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorOut[0], "Left Eye Out", debugRescale)
						BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorOut[1], "Right Eye Out", debugRescale)
					}
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateMotionVectors[0], "Left Eye MVec", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateMotionVectors[1], "Right Eye MVec", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateReactiveMask[0], "Left Eye Reactive", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateReactiveMask[1], "Right Eye Reactive", debugRescale)
					if (vrIntermediateTransparencyMask[0]) {
						BUFFER_VIEWER_NODE_TITLE(vrIntermediateTransparencyMask[0], "Left Eye Transparency", debugRescale)
						BUFFER_VIEWER_NODE_TITLE(vrIntermediateTransparencyMask[1], "Right Eye Transparency", debugRescale)
					}
				} else {
					ImGui::TextDisabled("%s", T(TKEY("vr_intermediates_not_created"), "VR intermediates not yet created (enter game world)"));
				}
				ImGui::TreePop();
			}

			if (ImGui::TreeNode(T(TKEY("native_inputs"), "Native Inputs"))) {
				auto renderer = globals::game::renderer;
				auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
				auto& mvec = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
				auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

				auto DisplayRT = [&](const char* label, ID3D11Texture2D* tex, ID3D11ShaderResourceView* srv) {
					if (srv && tex) {
						D3D11_TEXTURE2D_DESC desc;
						tex->GetDesc(&desc);
						char buf[128];
						snprintf(buf, sizeof(buf), "%s (%ux%u)", label, desc.Width, desc.Height);
						if (ImGui::TreeNode(buf)) {
							ImGui::Image(srv, { desc.Width * debugRescale, desc.Height * debugRescale });
							ImGui::TreePop();
						}
					}
				};

				DisplayRT("kMAIN (Color Input)", (ID3D11Texture2D*)main.texture, (ID3D11ShaderResourceView*)main.SRV);
				DisplayRT("Motion Vectors", (ID3D11Texture2D*)mvec.texture, (ID3D11ShaderResourceView*)mvec.SRV);
				DisplayRT("Depth", depth.texture, depth.depthSRV);

				if (reactiveMaskTexture)
					BUFFER_VIEWER_NODE_TITLE(reactiveMaskTexture, "Reactive Mask", debugRescale)
				if (transparencyCompositionMaskTexture)
					BUFFER_VIEWER_NODE_TITLE(transparencyCompositionMaskTexture, "Transparency Mask", debugRescale)

				ImGui::TreePop();
			}
		}

		ImGui::Separator();
		Util::DrawDllVersionTable(T(TKEY("ffx_dll_table_title"), "AMD FidelityFX DLLs (click to open folder)"), FidelityFX::PluginDir, FidelityFX::dllVersions, "ffx_dll_versions");
		Util::DrawDllVersionTable(T(TKEY("sl_dll_table_title"), "NVIDIA Streamline DLLs (click to open folder)"), streamline.pluginDir.c_str(), Streamline::dllVersions, "sl_dll_versions");
		ImGui::TreePop();
	}
}

const VRDetection::OpenCompositeUpscalingState& Upscaling::GetOpenCompositeUpscalingBlocker(bool a_forceRefresh) const
{
	// Cached after first probe; lifecycle entry points force a refresh.
	if (!a_forceRefresh && openCompositeUpscalingBlockerCacheValid)
		return openCompositeUpscalingBlocker;

	// VR-only; non-VR never probes the filesystem.
	openCompositeUpscalingBlocker = globals::game::isVR ?
	                                    VRDetection::DetectOpenCompositeUpscaling() :
	                                    VRDetection::OpenCompositeUpscalingState{};
	openCompositeUpscalingBlockerCacheValid = true;

	return openCompositeUpscalingBlocker;
}

void Upscaling::ApplyOpenCompositeUpscalingBlocker(bool a_forceRefresh)
{
	const auto& blocker = GetOpenCompositeUpscalingBlocker(a_forceRefresh);
	if (!blocker.active)
		return;

	const bool wasOverriding =
		settings.upscaleMethod != static_cast<uint>(UpscaleMethod::kNONE) ||
		settings.upscaleMethodNoDLSS != static_cast<uint>(UpscaleMethod::kNONE);
	if (wasOverriding) {
		if (blocker.configPath.empty())
			logger::warn("[Upscaling] Forcing upscaling to None because OpenComposite has {}=true.", blocker.settingName);
		else
			logger::warn("[Upscaling] Forcing upscaling to None because OpenComposite has {}=true in {}.", blocker.settingName, blocker.configPath);
	}

	settings.upscaleMethod = static_cast<uint>(UpscaleMethod::kNONE);
	settings.upscaleMethodNoDLSS = static_cast<uint>(UpscaleMethod::kNONE);
}

void Upscaling::SaveSettings(json& o_json)
{
	// Persist None, not the user's prior method, while OpenComposite owns upscaling.
	ApplyOpenCompositeUpscalingBlocker(true);
	o_json = settings;
	// Nest FoveatedRender's settings under a sub-key so they round-trip alongside
	// Upscaling's own. Subrect controller persistence is owned by FoveatedRender.
	json foveatedRenderJson;
	foveatedRender.SaveSettings(foveatedRenderJson);
	o_json["foveatedRender"] = foveatedRenderJson;
	auto iniSettingCollection = globals::game::iniPrefSettingCollection;
	if (iniSettingCollection) {
		auto setting = iniSettingCollection->GetSetting("bUseTAA:Display");
		if (setting) {
			iniSettingCollection->WriteSetting(setting);
		}
	}
}

void Upscaling::LoadSettings(json& o_json)
{
	// Pull FoveatedRender's nested block first so its absence doesn't fail the
	// outer settings deserialize. FoveatedRender::ClampSettings touches sibling
	// presetDLSS (cross-feature compat), so re-run it after `settings = o_json`
	// below — otherwise the JSON re-assign overwrites the clamp and an
	// incompatible preset slips through.
	if (o_json.contains("foveatedRender")) {
		foveatedRender.LoadSettings(o_json["foveatedRender"]);
		o_json.erase("foveatedRender");
	}
	// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT fills an absent key with
	// Settings' default member initializer (the CURRENT schema version), not 0 --
	// detect absence explicitly so a pre-existing config still runs the migration.
	const bool hadFsr4SchemaVersion = o_json.contains("fsr4RuntimeSelectionSchemaVersion");
	settings = o_json;
	if (!hadFsr4SchemaVersion)
		settings.fsr4RuntimeSelectionSchemaVersion = 0;
	ApplyLegacyFsr4RuntimeSelectionMigration(settings, fidelityFX.GetFsr4AdapterSupport());

	// Sanitize loaded settings to ensure enum indices are valid
	constexpr auto enumCount = 4;  // UpscaleMethod has 4 values: kNONE, kTAA, kFSR, kDLSS
	if (settings.upscaleMethod >= static_cast<uint>(enumCount)) {
		logger::warn("[Upscaling] Loaded upscaleMethod {} out of range, clamping to {}", settings.upscaleMethod, enumCount ? enumCount - 1 : 0);
		settings.upscaleMethod = enumCount ? enumCount - 1 : 0;
	}
	if (settings.upscaleMethodNoDLSS >= static_cast<uint>(enumCount)) {
		logger::warn("[Upscaling] Loaded upscaleMethodNoDLSS {} out of range, clamping to {}", settings.upscaleMethodNoDLSS, enumCount ? enumCount - 1 : 0);
		settings.upscaleMethodNoDLSS = enumCount ? enumCount - 1 : 0;
	}
	if (settings.vrRenderScale != 0.0f &&
		(!std::isfinite(settings.vrRenderScale) ||
			settings.vrRenderScale < kVRRenderScaleMin || settings.vrRenderScale > kVRRenderScaleMax)) {
		const float clamped = std::isfinite(settings.vrRenderScale) && settings.vrRenderScale > 0.0f ?
		                          std::clamp(settings.vrRenderScale, kVRRenderScaleMin, kVRRenderScaleMax) :
		                          0.0f;
		logger::warn("[Upscaling] Loaded vrRenderScale {} out of range, clamping to {}", settings.vrRenderScale, clamped);
		settings.vrRenderScale = clamped;
	}
	if (settings.qualityMode > 4) {
		logger::warn("[Upscaling] Loaded qualityMode {} out of range, clamping to 4", settings.qualityMode);
		settings.qualityMode = 4;
	}
	if (settings.presetDLSS > 4) {
		logger::warn("[Upscaling] Loaded presetDLSS {} out of range, resetting to 0 (Default)", settings.presetDLSS);
		settings.presetDLSS = 0;
	}
	if (settings.streamlineLogLevel > 2) {  // Off, Default, Verbose
		logger::warn("[Upscaling] Loaded streamlineLogLevel {} out of range, clamping to 2", settings.streamlineLogLevel);
		settings.streamlineLogLevel = 2;
	}
	// Re-apply FoveatedRender's cross-feature clamp now that the JSON
	// re-assign above has overwritten anything it set during its own
	// LoadSettings (which fired before this block ran). Idempotent — no-op
	// if FoveatedRender is inactive or the preset is already compatible.
	foveatedRender.ClampSettings();
	// Override a loaded method with None when OpenComposite owns upscaling.
	ApplyOpenCompositeUpscalingBlocker(true);
	const float originalReflexFPSLimit = settings.reflexFPSLimit;
	if (!std::isfinite(settings.reflexFPSLimit)) {
		settings.reflexFPSLimit = 60.0f;
		logger::warn(
			"[Upscaling] Loaded reflexFPSLimit {} is not finite, resetting to {}",
			originalReflexFPSLimit,
			settings.reflexFPSLimit);
	}
	const float clampedReflexFPSLimit = std::clamp(settings.reflexFPSLimit, 20.0f, 240.0f);
	if (clampedReflexFPSLimit != settings.reflexFPSLimit) {
		logger::warn(
			"[Upscaling] Loaded reflexFPSLimit {} out of range, clamping to {}",
			settings.reflexFPSLimit,
			clampedReflexFPSLimit);
	}
	settings.reflexFPSLimit = clampedReflexFPSLimit;
	auto iniSettingCollection = globals::game::iniPrefSettingCollection;
	if (iniSettingCollection) {
		auto setting = iniSettingCollection->GetSetting("bUseTAA:Display");
		if (setting) {
			iniSettingCollection->ReadSetting(setting);
		}
	}
}

void Upscaling::RestoreDefaultSettings()
{
	settings = {};
	foveatedRender.RestoreDefaultSettings();
	ApplyOpenCompositeUpscalingBlocker(true);
}

void Upscaling::DataLoaded()
{
	ApplyOpenCompositeUpscalingBlocker(true);
	if (const auto& blocker = GetOpenCompositeUpscalingBlocker(); blocker.active) {
		logger::warn("[Upscaling] Skipping data-loaded upscaling adjustments because OpenComposite has {}=true.", blocker.settingName);
		return;
	}

	// Fix screenshots fix from Engine Fixes
	Util::DisableVanillaTAA();

	// The game defaults this to a non-zero value
	static auto fDRClampOffset = RE::GetINISetting("fDRClampOffset:Display");
	fDRClampOffset->data.f = 0.0f;

	// VR + DLSS workaround: rebuild the DLSS feature on cell/worldspace transitions to
	// clear a persistent post-load GPU-time regression (see pendingDLSSReset comment).
	if (globals::game::isVR)
		MenuOpenCloseEventHandler::Register();
}

RE::BSEventNotifyControl Upscaling::MenuOpenCloseEventHandler::ProcessEvent(
	const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening)
		globals::features::upscaling.pendingDLSSReset.store(true, std::memory_order_relaxed);
	return RE::BSEventNotifyControl::kContinue;
}

bool Upscaling::MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	auto ui = globals::game::ui;
	if (!ui) {
		logger::error("[Upscaling] UI event source not found; DLSS reset-on-load disabled");
		return false;
	}
	ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);
	logger::info("[Upscaling] Registered MenuOpenCloseEventHandler for DLSS reset-on-load");
	return true;
}

void Upscaling::Load()
{
	ApplyOpenCompositeUpscalingBlocker(true);
	if (const auto& blocker = GetOpenCompositeUpscalingBlocker(); blocker.active) {
		logger::warn("[Upscaling] Skipping D3D11 device hook because OpenComposite has {}=true.", blocker.settingName);
		return;
	}

	*(uintptr_t*)&ptrD3D11CreateDeviceAndSwapChainUpscaling = SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChainUpscaling, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
}

struct BSImageSpace_Init_FXAA
{
	static void thunk()
	{
		func();

		// Force FXAA off safely
		auto fxaaEnabled = reinterpret_cast<bool*>(REL::RelocationID(513281, 391028).address());
		*fxaaEnabled = false;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

#ifdef TRACY_ENABLE
#	include <optional>
// Diagnostic GPU zone around the engine SSR raymarch DRAW, bracketed by the shader's PreRender
// (0x0A) / PostRender (0x0B) vfuncs — the correct hook points, stable on SE/AE/VR (see commit message).
static constexpr tracy::SourceLocationData kSSRZoneSrcLoc{ "SSR ReflectionsRayTracing", "ISReflectionsRayTracing::Render", __FILE__, (uint32_t)__LINE__, 0 };
// In-place holder (no per-frame heap): opened on PreRender, closed on PostRender, render-thread only
// and non-nested for this shader.
static std::optional<tracy::D3D11ZoneScope> g_ssrGpuZone;

struct SSRPreRender_Hook
{
	static void thunk(void* a_this)
	{
		func(a_this);  // original PreRender (engine SSR render-state setup)
		if (globals::state->tracyCtx) {
			g_ssrGpuZone.reset();  // defensive: close a prior zone if PostRender was ever skipped
			g_ssrGpuZone.emplace(globals::state->tracyCtx, &kSSRZoneSrcLoc, true);
		}
	}
	static inline REL::Relocation<decltype(thunk)> func;
};
struct SSRPostRender_Hook
{
	static void thunk(void* a_this)
	{
		g_ssrGpuZone.reset();  // close the GPU zone right after the draw
		func(a_this);          // original PostRender
	}
	static inline REL::Relocation<decltype(thunk)> func;
};
#endif

void Upscaling::PostPostLoad()
{
	// Guard before foveatedRender.PostPostLoad() so its hooks also stand down.
	ApplyOpenCompositeUpscalingBlocker(true);
	if (const auto& blocker = GetOpenCompositeUpscalingBlocker(); blocker.active) {
		logger::warn("[Upscaling] Skipping upscaling render hooks because OpenComposite has {}=true.", blocker.settingName);
		return;
	}

	// Subrect controller defaults + stereo flag (FoveatedRender is no longer a
	// Feature subclass so we drive its lifecycle from here).
	foveatedRender.PostPostLoad();

	bool isGOG = !GetModuleHandle(L"steam_api64.dll");
	stl::detour_thunk<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084));

	// Calculates resolution and jitter
	const std::uintptr_t steamJitterOffset = REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0)) ? 0x133 : 0xE2;
	stl::write_thunk_call<Main_UpdateJitter>(REL::RelocationID(75460, 77245).address() + REL::Relocate<std::uintptr_t>(0xE5, isGOG ? 0x133 : steamJitterOffset, 0x104));

	// Disables the original dynamic resolution system
	REL::safe_write(REL::RelocationID(35556, 36555).address() + REL::Relocate(0x2D, 0x2D, 0x25), REL::NOP5, sizeof(REL::NOP5));

	// Performs upscaling in between volumetric lighting and post processing
	stl::write_thunk_call<Main_PostProcessing>(REL::RelocationID(100430, 107148).address() + REL::Relocate(0x1F0, 0x1E7, 0x206));

	// Patches RSSetScissorRect calls to use dynamic resolution
	// This is a PC-specific function hence it was missing
	if (!globals::game::isVR)
		stl::detour_thunk<SetScissorRect>(REL::RelocationID(75564, 77365));

	// Patches facegen texture generation to not use dynamic resolution
	stl::detour_thunk<BSFaceGenManager_UpdatePendingCustomizationTextures>(REL::RelocationID(26455, 27041));

	// Patches precipitation camera to not use dynamic resolution
	stl::write_thunk_call<Main_RenderPrecipitation>(REL::RelocationID(35560, 36559).address() + REL::Relocate<std::uintptr_t>(0x3A1, REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0)) ? 0x3BF : 0x3A1, 0x2FA));

	// Forces FXAA off
	stl::detour_thunk<BSImageSpace_Init_FXAA>(REL::RelocationID(98974, 105626));

#ifdef TRACY_ENABLE
	// SSR raymarch GPU zone — see the SSRPreRender_Hook / SSRPostRender_Hook definitions above.
	stl::write_vfunc<0x0A, SSRPreRender_Hook>(RE::VTABLE_BSImagespaceShaderReflectionsRayTracing[0]);
	stl::write_vfunc<0x0B, SSRPostRender_Hook>(RE::VTABLE_BSImagespaceShaderReflectionsRayTracing[0]);
	logger::info("[Upscaling] Installed SSR ReflectionsRayTracing Tracy zone (PreRender/PostRender 0x0A/0x0B, TRACY_ENABLE)");
#endif

	logger::info("[Upscaling] Installed hooks");
}

float Upscaling::GetQualityModeRatio(uint qualityMode)
{
	// Lower bound is 0, not 1: qualityMode=0 is DLAA / NATIVEAA (1.0x —
	// render at display resolution). The FfxFsr3QualityMode enum header
	// doesn't *declare* a 0 value, but the implementation delegates to
	// FfxFsr3UpscalerQualityMode which has NATIVEAA=0 → 1.0f. Clamping to
	// 1 would force DLAA into Quality (1.5x) and shrink the rendered
	// region of kMAIN to 67%.
	const float ratio = ffxFsr3GetUpscaleRatioFromQualityMode(
		static_cast<FfxFsr3QualityMode>(std::clamp<uint>(qualityMode, 0u, 4u)));
	return std::isfinite(ratio) && ratio > 0.0f ? ratio : 3.0f;
}

Upscaling::UpscaleMethod Upscaling::GetUpscaleMethod() const
{
	// OpenComposite owning upscaling wins over everything (incl. PerfMode); VR-only.
	if (globals::game::isVR && GetOpenCompositeUpscalingBlocker().active)
		return UpscaleMethod::kNONE;

	// PerfMode sizes the engine RTs at boot for a testTexture-redirecting upscaler. Keep the
	// boot-latched method while DLSS is available; if DLSS drops out (RenderDoc / no-DLSS GPU)
	// fall to FSR — the only other redirecting method — never a live or non-redirecting choice
	// that would desync the fixed RT sizing.
	if (globals::features::upscaling.perfMode.IsHookActive())
		return streamline.featureDLSS ?
		           static_cast<UpscaleMethod>(bootSnapshot.Boot(&Settings::upscaleMethod)) :
		           UpscaleMethod::kFSR;

	// No PerfMode: the DLSS-capable preference, or the no-DLSS preference when DLSS is unavailable —
	// coerced off DLSS so an out-of-range config can't re-select an unresolved DLSS path.
	if (!streamline.featureDLSS) {
		const auto noDlss = static_cast<UpscaleMethod>(settings.upscaleMethodNoDLSS);
		return noDlss == UpscaleMethod::kDLSS ? UpscaleMethod::kFSR : noDlss;
	}
	return static_cast<UpscaleMethod>(settings.upscaleMethod);
}

bool Upscaling::PerfModePrerequisitesMet() const
{
	if (!loaded)
		return false;
	const auto method = GetUpscaleMethod();
	const bool methodRedirectsOutput = method == UpscaleMethod::kDLSS || method == UpscaleMethod::kFSR;
	// >1.0x: a sub-display render res to bank. Native AA (1.0x) banks nothing
	// unless an explicit render scale supplies the sub-display res itself.
	return globals::game::isVR && methodRedirectsOutput &&
	       (GetQualityModeRatio(settings.qualityMode) > 1.0f || settings.vrRenderScale > 0.0f);
}

bool Upscaling::ShouldEngagePerfMode() const
{
	return settings.renderAtUpscaleRes && PerfModePrerequisitesMet();
}

void Upscaling::CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod)
{
	logger::debug("[Upscaling] Creating texture resources for method {} ({})", static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod));

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	if (a_upscalemethod == UpscaleMethod::kDLSS || a_upscalemethod == UpscaleMethod::kFSR) {
		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		if (!reactiveMaskTexture) {
			reactiveMaskTexture = new Texture2D(texDesc);
			reactiveMaskTexture->CreateSRV(srvDesc);
			reactiveMaskTexture->CreateUAV(uavDesc);
		}

		if (!transparencyCompositionMaskTexture) {
			transparencyCompositionMaskTexture = new Texture2D(texDesc);
			transparencyCompositionMaskTexture->CreateSRV(srvDesc);
			transparencyCompositionMaskTexture->CreateUAV(uavDesc);
		}

		// Cross-API runtime sharing does not support the game's R24G8 depth allocation.
		if (a_upscalemethod == UpscaleMethod::kFSR && !globals::game::isVR && fidelityFX.ShouldUseRuntimeUpscalerForFSR() && !runtimeFsrDepthTexture) {
			D3D11_TEXTURE2D_DESC depthDesc{};
			depthDesc.Width = texDesc.Width;
			depthDesc.Height = texDesc.Height;
			depthDesc.MipLevels = 1;
			depthDesc.ArraySize = 1;
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			depthDesc.SampleDesc.Count = 1;
			depthDesc.Usage = D3D11_USAGE_DEFAULT;
			depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
			depthSrvDesc.Format = depthDesc.Format;
			depthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			depthSrvDesc.Texture2D.MipLevels = 1;

			D3D11_UNORDERED_ACCESS_VIEW_DESC depthUavDesc{};
			depthUavDesc.Format = depthDesc.Format;
			depthUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

			runtimeFsrDepthTexture = new Texture2D(depthDesc, "Upscaling::RuntimeFsrDepth");
			runtimeFsrDepthTexture->CreateSRV(depthSrvDesc);
			runtimeFsrDepthTexture->CreateUAV(depthUavDesc);
		}
	}

	// Motion vector copy texture: DLSS's standard path always needs a per-frame snapshot
	// to dilate; FSR's standard path reads the engine's motion vectors directly and only
	// needs the copy when the foveated route (per-eye crop) is actually reachable.
	if (a_upscalemethod == UpscaleMethod::kDLSS ||
		(a_upscalemethod == UpscaleMethod::kFSR && foveatedRender.IsLoaded())) {
		if (!motionVectorCopyTexture) {
			auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

			D3D11_TEXTURE2D_DESC motionTexDesc{};
			motionVector.texture->GetDesc(&motionTexDesc);

			texDesc.Format = motionTexDesc.Format;
			srvDesc.Format = texDesc.Format;
			uavDesc.Format = texDesc.Format;

			motionVectorCopyTexture = new Texture2D(motionTexDesc);
			motionVectorCopyTexture->CreateSRV(srvDesc);
			motionVectorCopyTexture->CreateUAV(uavDesc);
		}
	}

	if (a_upscalemethod == UpscaleMethod::kDLSS) {
		// RCAS sharpener texture - matches kMAIN format for HDR sharpening
		if (!sharpenerTexture) {
			main.texture->GetDesc(&texDesc);
			main.SRV->GetDesc(&srvDesc);

			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			srvDesc.Format = texDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;

			uavDesc.Format = texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;

			sharpenerTexture = new Texture2D(texDesc);
			sharpenerTexture->CreateSRV(srvDesc);
			sharpenerTexture->CreateUAV(uavDesc);
		}
	}
}

void Upscaling::DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod)
{
	logger::debug("[Upscaling] Destroying texture resources for method {} ({})", static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod));

	// Clean up D3D11 textures that are no longer needed
	// Only destroy textures when switching away from methods that use them
	if (a_upscalemethod != UpscaleMethod::kDLSS && a_upscalemethod != UpscaleMethod::kFSR) {
		if (reactiveMaskTexture) {
			reactiveMaskTexture->srv = nullptr;
			reactiveMaskTexture->uav = nullptr;
			reactiveMaskTexture->resource = nullptr;

			delete reactiveMaskTexture;
			reactiveMaskTexture = nullptr;
		}

		if (transparencyCompositionMaskTexture) {
			transparencyCompositionMaskTexture->srv = nullptr;
			transparencyCompositionMaskTexture->uav = nullptr;
			transparencyCompositionMaskTexture->resource = nullptr;

			delete transparencyCompositionMaskTexture;
			transparencyCompositionMaskTexture = nullptr;
		}
	}

	if (a_upscalemethod != UpscaleMethod::kFSR && runtimeFsrDepthTexture) {
		runtimeFsrDepthTexture->srv = nullptr;
		runtimeFsrDepthTexture->uav = nullptr;
		runtimeFsrDepthTexture->resource = nullptr;

		delete runtimeFsrDepthTexture;
		runtimeFsrDepthTexture = nullptr;
	}

	// Motion vector copy texture is needed for DLSS and FSR's foveated route - mirror
	// CreateUpscalingTextureResources' allocation condition exactly, or a DLSS->FSR
	// switch with foveation not loaded leaks the DLSS-allocated texture (nothing
	// destroys it, and CreateUpscalingTextureResources also skips reallocating it).
	if (a_upscalemethod != UpscaleMethod::kDLSS &&
		!(a_upscalemethod == UpscaleMethod::kFSR && foveatedRender.IsLoaded())) {
		if (motionVectorCopyTexture) {
			motionVectorCopyTexture->srv = nullptr;
			motionVectorCopyTexture->uav = nullptr;
			motionVectorCopyTexture->resource = nullptr;

			delete motionVectorCopyTexture;
			motionVectorCopyTexture = nullptr;
		}
	}

	// RCAS sharpener texture is DLSS-only - destroy when switching away from DLSS
	if (a_upscalemethod != UpscaleMethod::kDLSS) {
		if (sharpenerTexture) {
			sharpenerTexture->srv = nullptr;
			sharpenerTexture->uav = nullptr;
			sharpenerTexture->resource = nullptr;

			delete sharpenerTexture;
			sharpenerTexture = nullptr;
		}
	}
}

void Upscaling::CheckResources(UpscaleMethod a_upscalemethod)
{
	static auto previousUpscaleMode = UpscaleMethod::kTAA;
	static bool previousFrameGenMode = false;

	bool frameGenModeCurrent = (settings.frameGenerationMode && d3d12SwapChainActive);
	bool frameGenModeChanged = frameGenModeCurrent != previousFrameGenMode;
	bool upscaleModeChanged = (previousUpscaleMode != a_upscalemethod);

	if (upscaleModeChanged || frameGenModeChanged) {
		logger::debug("[Upscaling] Resource change detected - Upscale: {} ({}) -> {} ({}), FrameGen: {} -> {} (d3d12Active={})",
			static_cast<int>(previousUpscaleMode), magic_enum::enum_name(previousUpscaleMode), static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod), previousFrameGenMode, frameGenModeCurrent, d3d12SwapChainActive);

		// Destroy previous upscaling method resources (only if they were actually active)
		if (upscaleModeChanged) {
			DestroyUpscalingTextureResources(a_upscalemethod);

			// Only destroy SDK resources if the previous method was actually performing upscaling
			if (previousUpscalingWasActive) {
				if (previousUpscaleMode == UpscaleMethod::kDLSS)
					streamline.DestroyDLSSResources();
				else if (previousUpscaleMode == UpscaleMethod::kFSR)
					fidelityFX.DestroyFSRResources();

				if (globals::game::isVR) {
					for (int i = 0; i < 2; i++) {
						vrIntermediateColorIn[i].reset();
						vrIntermediateColorOut[i].reset();
						vrIntermediateLinearDepth[i].reset();
						vrIntermediateMotionVectors[i].reset();
						vrIntermediateReactiveMask[i].reset();
						vrIntermediateTransparencyMask[i].reset();
					}
					vrIntermediateDepth.reset();
				}
			}
			if (a_upscalemethod == UpscaleMethod::kFSR)
				fidelityFX.CreateFSRResources();
		}

		// Create new upscaling method resources
		if (upscaleModeChanged) {
			CreateUpscalingTextureResources(a_upscalemethod);
		}

		// Update tracking for next call
		previousUpscaleMode = a_upscalemethod;
		previousFrameGenMode = (settings.frameGenerationMode && d3d12SwapChainActive);
		previousUpscalingWasActive = IsUpscalingActive();
	}
}

ID3D11ComputeShader* Upscaling::GetEncodeTexturesCS()
{
	auto upscaleMethod = GetUpscaleMethod();
	uint methodIndex = (uint)upscaleMethod;

	// Runtime FSR and VR require typed R32_FLOAT depth instead of the game's R24G8 resource.
	if (upscaleMethod == UpscaleMethod::kFSR && (globals::game::isVR || runtimeFsrDepthTexture)) {
		std::vector<std::pair<const char*, const char*>> defines = {
			{ "FSR", "" },
			{ "DEPTH_OUTPUT", "" }
		};
		return encodeTexturesCSDepthOutput.Get(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl", defines, "cs_5_0");
	}

	std::vector<std::pair<const char*, const char*>> defines;

	// Add upscale method define
	switch (upscaleMethod) {
	case UpscaleMethod::kDLSS:
		defines.push_back({ "DLSS", "" });
		break;
	case UpscaleMethod::kFSR:
		defines.push_back({ "FSR", "" });
		break;
	default:
		// No define for NONE or TAA
		break;
	}

	return encodeTexturesCS[methodIndex].Get(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl", defines, "cs_5_0");
}

ID3D11PixelShader* Upscaling::GetDepthRefractionUpscalePS()
{
	return depthRefractionUpscalePS.Get(L"Data/Shaders/Upscaling/DepthRefractionUpscalePS.hlsl", { { "PSHADER", "" } }, "ps_5_0");
}

ID3D11PixelShader* Upscaling::GetUnderwaterMaskUpscalePS()
{
	std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
	if (globals::game::isVR)
		defines.push_back({ "VR", "" });
	return underwaterMaskUpscalePS.Get(L"Data/Shaders/Upscaling/UnderwaterMaskUpscalePS.hlsl", defines, "ps_5_0");
}

ID3D11PixelShader* Upscaling::GetCameraMotionVectorsPS()
{
	std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
	if (globals::game::isVR)
		defines.push_back({ "VR", "" });
	return cameraMotionVectorsPS.Get(L"Data/Shaders/Upscaling/CameraMotionVectorsPS.hlsl", defines, "ps_5_0");
}

ID3D11VertexShader* Upscaling::GetUpscaleVS()
{
	return upscaleVS.Get(L"Data/Shaders/Upscaling/UpscaleVS.hlsl", { { "VSHADER", "" } }, "vs_5_0");
}

eastl::unique_ptr<Texture2D> Upscaling::CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
	bool copyBindFlags, bool createSRV, bool createUAV, const char* name)
{
	D3D11_TEXTURE2D_DESC srcDesc;
	static_cast<ID3D11Texture2D*>(src)->GetDesc(&srcDesc);

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = srcDesc.Format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = copyBindFlags ? srcDesc.BindFlags : (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);

	auto tex = eastl::make_unique<Texture2D>(desc);

	if (name) {
		Util::SetResourceName(tex->resource.get(), name);
	}

	if (createSRV) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = srcDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		tex->CreateSRV(srvDesc);
	}
	if (createUAV) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = srcDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		tex->CreateUAV(uavDesc);
	}
	return tex;
}

void Upscaling::CreateVRIntermediateTextures(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
	ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc)
{
	// Right-eye-only depth intermediate for DLSS. Streamline.Upscale copies the right-eye depth
	// slice here before evaluating DLSS eye 1; eye 0 reads the combined stereo depth directly at
	// zero offset. R24G8_TYPELESS matches the game's D24S8_TYPELESS cast group — R32_TYPELESS is
	// a different cast group and produces silent zero-copy failures.
	{
		D3D11_TEXTURE2D_DESC depthDesc = {};
		depthDesc.Width = inWidth;
		depthDesc.Height = inHeight;
		depthDesc.MipLevels = 1;
		depthDesc.ArraySize = 1;
		depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
		depthDesc.SampleDesc.Count = 1;
		depthDesc.Usage = D3D11_USAGE_DEFAULT;
		depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		vrIntermediateDepth = eastl::make_unique<Texture2D>(depthDesc);

		Util::SetResourceName(vrIntermediateDepth->resource.get(), "Upscale_Depth_Right");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		vrIntermediateDepth->CreateSRV(srvDesc);
	}

	// All buffers are per-eye: Streamline validates all extents against the input color texture
	// dimensions, so every tagged resource must be isolated per-eye at {0,0}.
	for (int i = 0; i < 2; i++) {
		std::string suffix = (i == 0) ? "Left" : "Right";

		vrIntermediateColorIn[i] = CreateTextureFromSource(colorSrc, inWidth, inHeight, false, true, true, ("Upscale_ColorIn_" + suffix).c_str());
		vrIntermediateColorOut[i] = CreateTextureFromSource(colorSrc, outWidth, outHeight, false, true, false, ("Upscale_ColorOut_" + suffix).c_str());

		// Linear depth: R32_FLOAT so FSR's GetFfxResourceDescriptionDX11() returns a valid format.
		// EncodeTexturesCS writes the non-linear depth as R32_FLOAT for FSR. Kept separate from
		// vrIntermediateDepth (R24G8_TYPELESS) which Streamline copies into for DLSS right eye.
		{
			D3D11_TEXTURE2D_DESC ldDesc = {};
			ldDesc.Width = inWidth;
			ldDesc.Height = inHeight;
			ldDesc.MipLevels = 1;
			ldDesc.ArraySize = 1;
			ldDesc.Format = DXGI_FORMAT_R32_FLOAT;
			ldDesc.SampleDesc.Count = 1;
			ldDesc.Usage = D3D11_USAGE_DEFAULT;
			ldDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			vrIntermediateLinearDepth[i] = eastl::make_unique<Texture2D>(ldDesc);

			Util::SetResourceName(vrIntermediateLinearDepth[i]->resource.get(), ("Upscale_LinearDepth_" + suffix).c_str());

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc2 = {};
			srvDesc2.Format = DXGI_FORMAT_R32_FLOAT;
			srvDesc2.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc2.Texture2D.MipLevels = 1;
			vrIntermediateLinearDepth[i]->CreateSRV(srvDesc2);

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc2 = {};
			uavDesc2.Format = DXGI_FORMAT_R32_FLOAT;
			uavDesc2.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc2.Texture2D.MipSlice = 0;
			vrIntermediateLinearDepth[i]->CreateUAV(uavDesc2);
		}

		// UAV required: EncodeTexturesCS writes directly into these per-eye buffers
		vrIntermediateMotionVectors[i] = CreateTextureFromSource(mvecSrc, inWidth, inHeight, false, true, true, ("Upscale_MVec_" + suffix).c_str());
		vrIntermediateReactiveMask[i] = CreateTextureFromSource(reactiveSrc, inWidth, inHeight, false, true, true, ("Upscale_Reactive_" + suffix).c_str());
		vrIntermediateTransparencyMask[i] = CreateTextureFromSource(transparencySrc, inWidth, inHeight, false, true, true, ("Upscale_Transparency_" + suffix).c_str());
	}

	logger::info("[Upscaling] Created VR intermediate textures: per-eye in {}x{}, out {}x{}",
		inWidth, inHeight, outWidth, outHeight);
}

void Upscaling::EnsureVRIntermediateTextures()
{
	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& motionVectorRT = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	auto screenSize = globals::state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	// PerfMode: state->screenSize is polluted to RenderRes (the BSOpenVR size
	// hook spoofs HMD-recommended size). DLSS output needs to land at real
	// DisplayRes, so size the OUTPUT intermediates from perfMode's snapshot
	// of the true HMD resolution. Input intermediates stay at renderSize.
	const bool dlssperfActive = perfMode.IsHookActive() && perfMode.GetTestTexture();
	const float2 outputSize = dlssperfActive ? perfMode.GetDisplayScreenSize() : screenSize;

	uint32_t eyeWidthOut = (uint32_t)(outputSize.x / 2);
	uint32_t eyeHeightOut = (uint32_t)outputSize.y;
	uint32_t eyeWidthIn = (uint32_t)(renderSize.x / 2);
	uint32_t eyeHeightIn = (uint32_t)renderSize.y;

	bool needsRecreate = !vrIntermediateColorIn[0] || !vrIntermediateColorOut[0] || !vrIntermediateLinearDepth[0];
	if (!needsRecreate) {
		needsRecreate = (vrIntermediateColorIn[0]->desc.Width != eyeWidthIn ||
						 vrIntermediateColorIn[0]->desc.Height != eyeHeightIn ||
						 vrIntermediateColorOut[0]->desc.Width != eyeWidthOut ||
						 vrIntermediateColorOut[0]->desc.Height != eyeHeightOut);
	}
	if (needsRecreate) {
		logger::info("[Upscaling] (Re)creating VR intermediates: per-eye in {}x{}, out {}x{}",
			eyeWidthIn, eyeHeightIn, eyeWidthOut, eyeHeightOut);
		CreateVRIntermediateTextures(eyeWidthIn, eyeHeightIn, eyeWidthOut, eyeHeightOut,
			main.texture, motionVectorRT.texture,
			reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get());
	}
}

void Upscaling::PreparePerEyeInputs(ID3D11Resource* colorSrc)
{
	if (!globals::game::isVR)
		return;

	CS_GPU_PASS("Upscaling::PreparePerEyeInputs");

	auto context = globals::d3d::context;
	auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);

	uint32_t eyeWidthIn = (uint32_t)(renderSize.x / 2);
	uint32_t eyeHeightIn = (uint32_t)renderSize.y;

	// Textures guaranteed to exist: EnsureVRIntermediateTextures() was called in Upscale()
	// Read the original game depth SRV for ClearHMDMask — the combined stereo buffer is
	// definitively valid here, whereas the per-eye copy may silently produce zeros on some
	// depth-stencil format / driver combinations.
	auto& depthTexture = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& motionVectorRT = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	for (uint32_t i = 0; i < 2; ++i) {
		uint32_t offsetXIn = (i == 1) ? eyeWidthIn : 0;
		D3D11_BOX srcBox = { offsetXIn, 0, 0, offsetXIn + eyeWidthIn, eyeHeightIn, 1 };

		context->CopySubresourceRegion(vrIntermediateColorIn[i]->resource.get(), 0, 0, 0, 0, colorSrc, 0, &srcBox);
		context->CopySubresourceRegion(vrIntermediateMotionVectors[i]->resource.get(), 0, 0, 0, 0, motionVectorRT.texture, 0, &srcBox);

		uint32_t depthOffset = (i == 1) ? eyeWidthIn : 0;
		ClearHMDMask(vrIntermediateColorIn[i]->uav.get(), depthTexture.depthSRV,
			eyeWidthIn, eyeHeightIn, depthOffset, 0);
	}
}

void Upscaling::FinalizePerEyeOutputs(ID3D11Resource* colorDst)
{
	if (!globals::game::isVR)
		return;

	CS_GPU_PASS("Upscaling::FinalizePerEyeOutputs");

	auto context = globals::d3d::context;

	// Drive output dims from the per-eye intermediate desc, not state->screenSize.
	// Under PerfMode the state value is polluted to renderRes while the intermediates
	// were allocated at displayRes via EnsureVRIntermediateTextures' size bridge.
	if (!vrIntermediateColorOut[0]) {
		return;
	}

	uint32_t eyeWidthOut = vrIntermediateColorOut[0]->desc.Width;
	uint32_t eyeHeightOut = vrIntermediateColorOut[0]->desc.Height;

	// Write upscaled outputs back
	for (uint32_t i = 0; i < 2; ++i) {
		uint32_t offsetXOut = (i == 1) ? eyeWidthOut : 0;
		D3D11_BOX outBox = { 0, 0, 0, eyeWidthOut, eyeHeightOut, 1 };
		context->CopySubresourceRegion(colorDst, 0, offsetXOut, 0, 0, vrIntermediateColorOut[i]->resource.get(), 0, &outBox);
	}
}

void Upscaling::ClearHMDMask(ID3D11UnorderedAccessView* colorUAV, ID3D11ShaderResourceView* depthSRV,
	uint32_t eyeWidth, uint32_t eyeHeight, uint32_t depthOffsetX, uint32_t colorOffsetX)
{
	if (!globals::game::isVR)
		return;

	auto context = globals::d3d::context;

	if (!vrClearHMDMaskCS) {
		vrClearHMDMaskCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/ClearHMDMaskCS.hlsl", {}, "cs_5_0"));

		D3D11_BUFFER_DESC cbDesc = {};
		cbDesc.ByteWidth = 16;  // 4 uints
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&cbDesc, nullptr, vrClearHMDMaskCB.put()));
	}

	if (vrClearHMDMaskCS) {
		auto dispatchX = (eyeWidth + 7) / 8;
		auto dispatchY = (eyeHeight + 7) / 8;

		context->CSSetShader(vrClearHMDMaskCS.get(), nullptr, 0);

		ID3D11ShaderResourceView* srvs[1] = { depthSRV };
		context->CSSetShaderResources(0, 1, srvs);

		ID3D11UnorderedAccessView* uavs[1] = { colorUAV };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		D3D11_MAPPED_SUBRESOURCE mapped{};
		context->Map(vrClearHMDMaskCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

		uint32_t offsets[4] = { depthOffsetX, colorOffsetX, 0, 0 };

		memcpy(mapped.pData, offsets, sizeof(offsets));
		context->Unmap(vrClearHMDMaskCB.get(), 0);

		ID3D11Buffer* cbs[1] = { vrClearHMDMaskCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);

		context->Dispatch(dispatchX, dispatchY, 1);

		// Unbind
		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}

int32_t GetJitterPhaseCount(int32_t renderWidth, int32_t displayWidth)
{
	const float basePhaseCount = 8.0f;
	const int32_t jitterPhaseCount = int32_t(basePhaseCount * pow((float(displayWidth) / renderWidth), 2.0f));
	return jitterPhaseCount;
}

// Calculate halton number for index and base.
static float Halton(int32_t index, int32_t base)
{
	float f = 1.0f, result = 0.0f;

	for (int32_t currentIndex = index; currentIndex > 0;) {
		f /= (float)base;
		result = result + f * (float)(currentIndex % base);
		currentIndex = (uint32_t)(floorf((float)(currentIndex) / (float)(base)));
	}

	return result;
}

void GetJitterOffset(float* outX, float* outY, int32_t index, int32_t phaseCount)
{
	const float x = Halton((index % phaseCount) + 1, 2) - 0.5f;
	const float y = Halton((index % phaseCount) + 1, 3) - 0.5f;

	*outX = x;
	*outY = y;
}

void Upscaling::ConfigureTAA()
{
	auto upscaleMethod = GetUpscaleMethod();

	// Force enable TAA if needed
	Util::SetTemporal(upscaleMethod != UpscaleMethod::kNONE);
}

void Upscaling::ConfigureUpscaling(RE::BSGraphics::State* a_viewport)
{
	auto upscaleMethod = GetUpscaleMethod();

	// Delete or create resources as necessary
	CheckResources(upscaleMethod);

	// Cache original TAA values for UI
	projectionPosScaleX = a_viewport->projectionPosScaleX;
	projectionPosScaleY = a_viewport->projectionPosScaleY;

	// Get full screen size
	auto state = globals::state;
	auto screenSize = state->screenSize;

	auto screenWidth = static_cast<int>(screenSize.x);
	auto screenHeight = static_cast<int>(screenSize.y);

	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		// PerfMode: when the BSOpenVR size hook is live, every engine RT was
		// already allocated at RenderRes — so the DRS-style scale is identity.
		// Jitter is still computed at the real DisplayRes phase ratio so the
		// upscaler has enough sub-pixel diversity for reconstruction.
		//
		// The upscaleMethod here comes from GetUpscaleMethod(), which under
		// PerfMode+hookActive is locked to the boot snapshot — so this gate
		// reads the value the user had selected at game start, not what they
		// later moved the slider to. Engine RTs were sized off that boot
		// choice (irreversible — the size hook can't un-allocate them); the
		// boot-snapshot lock keeps the runtime evaluate consistent with those
		// allocations. UI staged-change banners explain the restart
		// requirement for method/quality edits. Branch fires for both DLSS
		// and FSR since both consume the renderRes engine RTs and write to
		// perfMode.testTexture.
		const bool dlssperfRenderResPath =
			perfMode.IsHookActive() &&
			(upscaleMethod == UpscaleMethod::kDLSS || upscaleMethod == UpscaleMethod::kFSR);
		if (dlssperfRenderResPath) {
			resolutionScale = float2{ 1.0f, 1.0f };

			auto renderWidth = static_cast<int>(perfMode.GetRenderEyeWidth());
			auto displayWidth = static_cast<int>(perfMode.GetDisplayEyeWidth());

			auto phaseCount = GetJitterPhaseCount(renderWidth, displayWidth);
			GetJitterOffset(&jitter.x, &jitter.y, state->frameCount, phaseCount);
			// Loading screens reset the upscaler every frame; unintegrated jitter only wobbles the image.
			if (globals::state->isLoadingMenuOpen)
				jitter = float2{ 0.0f, 0.0f };

			if (globals::game::isVR)
				a_viewport->projectionPosScaleX = -jitter.x / renderWidth;
			else
				a_viewport->projectionPosScaleX = -2.0f * jitter.x / renderWidth;

			a_viewport->projectionPosScaleY = 2.0f * jitter.y / static_cast<int>(perfMode.GetRenderEyeHeight());
		} else {
			// Boot qualityMode under PerfMode so projection stays coherent
			// with the engine RTs sized at install.
			const uint32_t qm = globals::features::upscaling.perfMode.IsHookActive() ? bootSnapshot.Boot(&Settings::qualityMode) : settings.qualityMode;
			float resolutionScaleBase = 1.0f / GetQualityModeRatio(qm);

			auto renderWidth = static_cast<int>(screenWidth * resolutionScaleBase);
			auto renderHeight = static_cast<int>(screenHeight * resolutionScaleBase);

			resolutionScale.x = static_cast<float>(renderWidth) / static_cast<float>(screenWidth);
			resolutionScale.y = static_cast<float>(renderHeight) / static_cast<float>(screenHeight);

			auto phaseCount = GetJitterPhaseCount(renderWidth, screenWidth);

			GetJitterOffset(&jitter.x, &jitter.y, state->frameCount, phaseCount);
			// Loading screens reset the upscaler every frame; unintegrated jitter only wobbles the image.
			if (globals::state->isLoadingMenuOpen)
				jitter = float2{ 0.0f, 0.0f };

			if (globals::game::isVR)
				a_viewport->projectionPosScaleX = -jitter.x / renderWidth;
			else
				a_viewport->projectionPosScaleX = -2.0f * jitter.x / renderWidth;

			a_viewport->projectionPosScaleY = 2.0f * jitter.y / renderHeight;
		}
	} else {
		resolutionScale = float2{ 1.0f, 1.0f };

		if (globals::game::isVR)
			jitter.x = -a_viewport->projectionPosScaleX * screenWidth;
		else
			jitter.x = -a_viewport->projectionPosScaleX * screenWidth / 2.0f;

		jitter.y = a_viewport->projectionPosScaleY * screenHeight / 2.0f;
	}

	auto& runtimeData = a_viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = dynamicResolutionWidthRatio;
	runtimeData.dynamicResolutionPreviousHeightRatio = dynamicResolutionHeightRatio;
	runtimeData.dynamicResolutionWidthRatio = resolutionScale.x;
	runtimeData.dynamicResolutionHeightRatio = resolutionScale.y;

	dynamicResolutionWidthRatio = resolutionScale.x;
	dynamicResolutionHeightRatio = resolutionScale.y;

	// Disable dynamic resolution unless the game explicitly enables it
	if (!globals::game::isVR)
		runtimeData.dynamicResolutionLock = 1;
}

void Upscaling::SetupResources()
{
	ApplyOpenCompositeUpscalingBlocker(true);
	if (const auto& blocker = GetOpenCompositeUpscalingBlocker(); blocker.active) {
		logger::warn("[Upscaling] Skipping upscaling resource setup because OpenComposite has {}=true.", blocker.settingName);
		return;
	}

	QueryPerformanceFrequency(&qpf);

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
	depthStencilDesc.DepthEnable = true;                           // Enable depth testing
	depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;  // Write to all depth bits
	depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;          // Always pass depth test (write all depths)

	if (globals::game::isVR) {
		depthStencilDesc.StencilEnable = true;     // Enable stencil testing
		depthStencilDesc.StencilReadMask = 0xFF;   // Read all stencil bits
		depthStencilDesc.StencilWriteMask = 0xFF;  // Write to all stencil bits

		// Configure front-facing stencil operations
		depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;       // Replace on stencil fail
		depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;  // Replace on depth fail
		depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;    // Replace on pass
		depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;       // Always pass stencil test

		// Configure back-facing stencil operations (same as front)
		depthStencilDesc.BackFace.StencilFailOp = depthStencilDesc.FrontFace.StencilFailOp;
		depthStencilDesc.BackFace.StencilDepthFailOp = depthStencilDesc.FrontFace.StencilDepthFailOp;
		depthStencilDesc.BackFace.StencilPassOp = depthStencilDesc.FrontFace.StencilPassOp;
		depthStencilDesc.BackFace.StencilFunc = depthStencilDesc.FrontFace.StencilFunc;
	} else {
		depthStencilDesc.StencilEnable = false;  // Disable stencil testing
	}

	DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depthStencilDesc, upscaleDepthStencilState.put()));

	// Create jitter offset constant buffer for depth upscaling
	jitterCB = new ConstantBuffer(ConstantBufferDesc<JitterCB>());

	// Create upscaling data constant buffer for encode textures compute shader
	upscalingDataCB = new ConstantBuffer(ConstantBufferDesc<UpscalingDataCB>());

	// Create camera reprojection matrices constant buffer for menu motion vectors
	cameraMotionVectorsCB = new ConstantBuffer(ConstantBufferDesc<CameraMotionVectorsCB>(), "Upscaling::CameraMotionVectorsCB");

	// Create blend state for depth upscaling
	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.AlphaToCoverageEnable = false;
	blendDesc.IndependentBlendEnable = false;
	blendDesc.RenderTarget[0].BlendEnable = false;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&blendDesc, upscaleBlendState.put()));

	// Create rasterizer state for fullscreen rendering
	D3D11_RASTERIZER_DESC rasterizerDesc = {};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.FrontCounterClockwise = false;
	rasterizerDesc.DepthBias = 0;
	rasterizerDesc.DepthBiasClamp = 0.0f;
	rasterizerDesc.SlopeScaledDepthBias = 0.0f;
	rasterizerDesc.DepthClipEnable = false;
	rasterizerDesc.ScissorEnable = false;
	rasterizerDesc.MultisampleEnable = false;
	rasterizerDesc.AntialiasedLineEnable = false;
	DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&rasterizerDesc, upscaleRasterizerState.put()));

	CheckResources(GetUpscaleMethod());

	rcas.Initialize();

	if (d3d12SwapChainActive)
		dx12SwapChain.CreateSharedResources();
}

void Upscaling::ClearShaderCache()
{
	foveatedRender.ClearShaderCache();
	for (int i = 0; i < 5; ++i) {
		encodeTexturesCS[i].Reset();
	}
	encodeTexturesCSDepthOutput.Reset();
	copyDepthToSharedBufferPS.Reset();

	depthRefractionUpscalePS.Reset();
	underwaterMaskUpscalePS.Reset();
	cameraMotionVectorsPS.Reset();
	upscaleVS.Reset();
}

void Upscaling::CopySharedD3D12Resources()
{
	CS_GPU_PASS("Upscaling::CopySharedD3D12Resources");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	context->CopyResource(dx12SwapChain.motionVectorBufferShared12->resource11, motionVector.texture);

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	auto* vs = GetUpscaleVS();
	if (!vs)
		return;

	{
		// Set up viewport for fullscreen rendering
		auto screenSize = globals::state->screenSize;

		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = screenSize.x;
		viewport.Height = screenSize.y;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);

		// Set up Input Assembler for fullscreen triangle
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// Set up vertex shader
		context->VSSetShader(vs, nullptr, 0);

		// Set up rasterizer and blend states
		context->RSSetState(upscaleRasterizerState.get());
		context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

		// Set up pixel shader resources
		ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(views), views);

		// Set render target view for pixel shader output
		ID3D11RenderTargetView* rtvs[1] = { dx12SwapChain.depthBufferShared12->rtv };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

		if (auto* ps = copyDepthToSharedBufferPS.Get(L"Data\\Shaders\\Upscaling\\CopyDepthToSharedBufferPS.hlsl", { { "PSHADER", "" } }, "ps_5_0")) {
			context->PSSetShader(ps, nullptr, 0);
			context->Draw(3, 0);
		}
	}

	// Clean up
	ID3D11ShaderResourceView* views[1] = { nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(views), views);

	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->PSSetShader(nullptr, nullptr, 0);
	context->VSSetShader(nullptr, nullptr, 0);
}

void UpdateCameraData()
{
	using func_t = decltype(&UpdateCameraData);
	static REL::Relocation<func_t> func{ RELOCATION_ID(75472, 77258) };
	func();
}

void Upscaling::PostDisplay()
{
	auto viewport = globals::game::graphicsState;

	viewport->projectionPosScaleX = projectionPosScaleX;
	viewport->projectionPosScaleY = projectionPosScaleY;

	auto& runtimeData = viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = 1;
	runtimeData.dynamicResolutionPreviousHeightRatio = 1;
	runtimeData.dynamicResolutionWidthRatio = 1;
	runtimeData.dynamicResolutionHeightRatio = 1;
	runtimeData.dynamicResolutionLock = 1;

	globals::game::renderer->UpdateViewPort(0, 0, 1);
	UpdateCameraData();

	if (d3d12SwapChainActive)
		globals::features::hdrDisplay.SetUIBuffer();

	globals::state->UpdateSharedData(false, false);
}

void Upscaling::TimerSleepQPC(int64_t targetQPC)
{
	LARGE_INTEGER currentQPC;
	do {
		QueryPerformanceCounter(&currentQPC);
	} while (currentQPC.QuadPart < targetQPC);
}

void Upscaling::FrameLimiter()
{
	// The SL pacer owns presentation when DLSS-G is active; host-side blocking here
	// starves its flip queue and every interpolated frame gets dropped.
	if (UsesDLSSGFrameGen())
		return;

	if (d3d12SwapChainActive) {
		// Use frame latency waitable object if available for better frame pacing
		HANDLE waitableObject = GetFrameLatencyWaitableObject();

		// Wait for the next frame presentation slot
		WaitForSingleObject(waitableObject, INFINITE);

		if (settings.frameLimitMode) {
			static constexpr int64_t kNanosecondsPerSecond = 1000000000LL;
			// The real-frame target must scale with the active multiplier or every
			// configuration paces identically to 2x.
			const double frameRateScale = ShouldUseFrameGenerationThisFrame() ?
			                                  1.0 / static_cast<double>(GetFrameGenerationMultiplier()) :
			                                  1.0;
			int64_t targetFrameTimeNS = int64_t(static_cast<double>(kNanosecondsPerSecond) / (refreshRate * frameRateScale));
			int64_t targetFrameTicks = (targetFrameTimeNS * qpf.QuadPart) / kNanosecondsPerSecond;

			static LARGE_INTEGER lastFrame = {};
			LARGE_INTEGER timeNow;
			QueryPerformanceCounter(&timeNow);

			int64_t delta = timeNow.QuadPart - lastFrame.QuadPart;
			if (delta < targetFrameTicks) {
				TimerSleepQPC(lastFrame.QuadPart + targetFrameTicks);
			}
			QueryPerformanceCounter(&lastFrame);
		}
	}
}

/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

double Upscaling::GetRefreshRate(HWND a_window)
{
	HMONITOR monitor = MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info;
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(monitor, &info) != 0) {
		// using the CCD get the associated path and display configuration
		UINT32 requiredPaths, requiredModes;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) == ERROR_SUCCESS) {
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(requiredPaths);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes2(requiredModes);
			if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, paths.data(), &requiredModes, modes2.data(), nullptr) == ERROR_SUCCESS) {
				// iterate through all the paths until find the exact source to match
				for (auto& p : paths) {
					DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;
					sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
					sourceName.header.size = sizeof(sourceName);
					sourceName.header.adapterId = p.sourceInfo.adapterId;
					sourceName.header.id = p.sourceInfo.id;
					if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS && wcscmp(info.szDevice, sourceName.viewGdiDeviceName) == 0) {
						// find the matched device which is associated with current device
						// there may be the possibility that display may be duplicated and windows may be one of them in such scenario
						// there may be two callback because source is same target will be different
						// as window is on both the display so either selecting either one is ok
						// get the refresh rate
						UINT numerator = p.targetInfo.refreshRate.Numerator;
						UINT denominator = p.targetInfo.refreshRate.Denominator;
						return (double)numerator / (double)denominator;
					}
				}
			}
		}
	}
	logger::error("Failed to retrieve refresh rate from swap chain");
	return 60;
}

bool Upscaling::IsFrameGenerationDx12PathActive() const
{
	return d3d12SwapChainActive && !globals::game::isVR;
}

bool Upscaling::IsFrameGenerationActive() const
{
	if (!IsFrameGenerationDx12PathActive() || !settings.frameGenerationMode)
		return false;
	if (dx12SwapChain.useDLSSG)
		// featureDLSSG is a one-time boot flag; lastDLSSGStatus is live per-session.
		return streamlineDX12.featureDLSSG && streamlineDX12.lastDLSSGStatus == sl::DLSSGStatus::eOk;
	return fidelityFX.isFrameGenActive;
}

bool Upscaling::ShouldUseFrameGenerationThisFrame() const
{
	auto* state = globals::state;
	const bool menuOpen = state && state->IsPausedOrMenuOpen(globals::game::ui);
	return IsFrameGenerationDx12PathActive() && settings.frameGenerationMode && (settings.frameGenerationAllowInMenus || !menuOpen);
}

bool Upscaling::IsUpscalingActive() const
{
	auto method = GetUpscaleMethod();

	// Only consider vendor upscalers (FSR/DLSS) as "active" when the
	// selected method actually produces a downscale. If the renderer is
	// currently running at 1:1 (no downscale), treat upscaling as inactive.
	if (!(method == UpscaleMethod::kFSR || method == UpscaleMethod::kDLSS)) {
		return false;
	}

	// resolutionScale.x represents renderWidth / displayWidth.
	return resolutionScale.x < .99f;
}

// Unified interface methods
void Upscaling::LoadUpscalingSDKs()
{
	// Hooked into device creation, so this can fire repeatedly — log the skip once.
	ApplyOpenCompositeUpscalingBlocker(true);
	const auto& blocker = GetOpenCompositeUpscalingBlocker();
	if (blocker.active) {
		if (!openCompositeUpscalingBackendSkipLogged) {
			if (blocker.configPath.empty())
				logger::warn("[Upscaling] Skipping Streamline/FidelityFX backend init because OpenComposite has {}=true.", blocker.settingName);
			else
				logger::warn("[Upscaling] Skipping Streamline/FidelityFX backend init because OpenComposite has {}=true in {}.", blocker.settingName, blocker.configPath);
			openCompositeUpscalingBackendSkipLogged = true;
		}
		return;
	}

	// Initialize upscaling SDK components during plugin startup
	// This ensures all SDKs are available before any D3D device creation
	streamline.LoadInterposer();  // DX11: DLSS + Reflex + PCL

	streamlineDX12.renderAPI = sl::RenderAPI::eD3D12;
	streamlineDX12.pluginDir = L"Data\\Shaders\\Upscaling\\StreamlineDX12";
	streamlineDX12.interposerDllName = L"sl.interposer.dll";
	streamlineDX12.instanceTag = "DX12";
	streamlineDX12.LoadInterposer();

	fidelityFX.LoadFFX();  // AMD FSR frame generation
}

HANDLE Upscaling::GetFrameLatencyWaitableObject() const
{
	return dx12SwapChain.GetFrameLatencyWaitableObject();
}

float Upscaling::GetFrameTime() const
{
	return dx12SwapChain.GetFrameTime();
}

// Backend interface methods
bool Upscaling::IsBackendInitialized() const
{
	return streamline.initialized;
}

void Upscaling::CheckBackendFeatures(IDXGIAdapter* adapter)
{
	streamline.CheckFeatures(adapter);
}

void Upscaling::UpgradeBackendInterface(void** ppInterface)
{
	streamline.slUpgradeInterface(ppInterface);
}

void Upscaling::SetBackendD3DDevice(ID3D11Device* device)
{
	streamline.slSetD3DDevice(device);
}

void Upscaling::PostBackendDevice()
{
	streamline.PostDevice();
}

// Module availability methods
bool Upscaling::HasFrameGenModule() const
{
	// Only suppress DLSS-G when FSR3 is actually reachable to fall back to.
	const bool userPrefersReachableFsr = settings.preferFSRFrameGen && fidelityFX.featureFSR3FG;
	return fidelityFX.featureFSR3FG || (streamlineDX12.featureDLSSG && !userPrefersReachableFsr);
}

Upscaling::FrameGenMethod Upscaling::GetFrameGenMethod() const
{
	if (!d3d12SwapChainActive)
		return FrameGenMethod::kNone;
	if (dx12SwapChain.useDLSSG)
		return FrameGenMethod::kDLSSG;
	return FrameGenMethod::kFSR;
}

bool Upscaling::UsesDLSSGFrameGen() const
{
	return d3d12SwapChainActive && dx12SwapChain.useDLSSG;
}

uint Upscaling::GetFrameGenerationMultiplier() const
{
	if (!UsesDLSSGFrameGen())
		return 2;
	// Clamp to the hardware max like Streamline::ConfigureDLSSG does, or a stale
	// settings value would desync FrameLimiter's pacing from the real multiplier.
	const uint32_t clamped = std::clamp<uint32_t>(settings.dlssgFramesToGenerate, 0, streamlineDX12.dlssgMaxFramesToGenerate);
	return clamped + 1;
}

json Upscaling::GetDiagnostics()
{
	json diagnostics = json::object();
	const auto method = GetFrameGenMethod();
	diagnostics["frameGenMethod"] = method == FrameGenMethod::kDLSSG ? "DLSSG" :
	                                method == FrameGenMethod::kFSR   ? "FSR" :
	                                                                   "None";
	diagnostics["frameGenActive"] = IsFrameGenerationActive();
	diagnostics["frameGenMultiplier"] = GetFrameGenerationMultiplier();
	if (method == FrameGenMethod::kDLSSG) {
		diagnostics["dlssgStatus"] = std::string(magic_enum::enum_name(streamlineDX12.lastDLSSGStatus));
		diagnostics["dlssgFramesPresentedLastQuery"] = streamlineDX12.lastDLSSGFramesPresented;
	}
	return diagnostics;
}

// Proxy interface methods
void Upscaling::SetProxyD3D11Device(ID3D11Device* device)
{
	dx12SwapChain.SetD3D11Device(device);
}

void Upscaling::SetProxyD3D11DeviceContext(ID3D11DeviceContext* context)
{
	dx12SwapChain.SetD3D11DeviceContext(context);
}

void Upscaling::CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc)
{
	dx12SwapChain.CreateSwapChain(adapter, swapChainDesc);
}

void Upscaling::CreateProxySwapChainDirect(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc)
{
	dx12SwapChain.CreateSwapChainDirect(adapter, swapChainDesc);
}

void Upscaling::CreateProxyInterop()
{
	dx12SwapChain.CreateInterop();
}

IDXGISwapChain* Upscaling::GetProxySwapChain()
{
	return dx12SwapChain.GetSwapChainProxy();
}

Upscaling::BlurResources Upscaling::GetBlurResources() const
{
	if (d3d12SwapChainActive) {
		return dx12SwapChain.GetBlurResources();
	}
	return {};
}

void Upscaling::FillMenuCameraMotionVectors()
{
	menuCameraMVsValid = false;

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	auto* pixelShader = GetCameraMotionVectorsPS();
	auto* vertexShader = GetUpscaleVS();
	if (!pixelShader || !vertexShader || !cameraMotionVectorsCB ||
		!motionVector.RTV || !motionVector.texture || !depth.depthSRV)
		return;

	CS_GPU_PASS("Upscaling::MenuCameraMotionVectors");

	CameraMotionVectorsCB cbData{};
	const uint32_t numEyes = globals::game::isVR ? 2 : 1;
	for (uint32_t eyeIndex = 0; eyeIndex < numEyes; ++eyeIndex) {
		// Inversion is convention-safe on the raw cb12 bytes; composition with the previous
		// view-proj stays in the shader so the mul() convention matches FrameBuffer usage.
		cbData.curViewProjUnjitteredInverse[eyeIndex] =
			globals::game::frameBufferCached.GetCameraViewProjUnjittered(eyeIndex).Invert();
		cbData.prevViewProjUnjittered[eyeIndex] =
			globals::game::frameBufferCached.GetCameraPreviousViewProjUnjittered(eyeIndex);
	}
	cameraMotionVectorsCB->Update(cbData);

	{
		Util::FullscreenPassScope stateScope(context);

		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		context->VSSetShader(vertexShader, nullptr, 0);
		context->PSSetShader(pixelShader, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);

		ID3D11ShaderResourceView* srvs[] = { depth.depthSRV };
		context->PSSetShaderResources(0, 1, srvs);
		// b1: the slot FullscreenPassScope saves and restores.
		auto* constantBuffer = cameraMotionVectorsCB->CB();
		context->PSSetConstantBuffers(1, 1, &constantBuffer);

		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		context->OMSetDepthStencilState(nullptr, 0);
		context->RSSetState(nullptr);

		D3D11_TEXTURE2D_DESC mvDesc{};
		static_cast<ID3D11Texture2D*>(motionVector.texture)->GetDesc(&mvDesc);
		D3D11_VIEWPORT viewport = {};
		viewport.Width = static_cast<float>(mvDesc.Width);
		viewport.Height = static_cast<float>(mvDesc.Height);
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);

		ID3D11RenderTargetView* rtv = motionVector.RTV;
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->Draw(3, 0);
	}

	menuCameraMVsValid = true;
}

void Upscaling::Upscale()
{
	ZoneScoped;
	auto upscaleMethod = GetUpscaleMethod();

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	// Camera-only synthesized MVs are only valid for a static flat menu background.
	// Loading screens and any real scene rendered behind a menu (VR Playroom) have
	// their own motion the camera-derived MVs can't represent, so keep the reset.
	if (globals::state->isMainMenuOpen && !globals::state->isLoadingMenuOpen && !globals::state->worldRenderedThisFrame)
		FillMenuCameraMotionVectors();
	else
		menuCameraMVsValid = false;

	{
		CS_GPU_PASS("Upscaling::EncodeTextures");

		auto& temporalAAMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
		auto& normals = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[2]];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		// VR: ensure per-eye intermediate textures exist before the dispatch writes into them
		if (globals::game::isVR)
			EnsureVRIntermediateTextures();

		auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
		uint32_t numEyes = globals::game::isVR ? 2 : 1;
		uint32_t eyeRenderWidth = (uint32_t)(renderSize.x / numEyes);
		uint32_t eyeRenderHeight = (uint32_t)renderSize.y;

		// Sources are the same combined stereo buffers for both VR and non-VR.
		// The shader applies EyeOffsetX to sample the correct half.
		ID3D11ShaderResourceView* views[4] = { temporalAAMask.SRV, normals.SRV, motionVector.SRV, depth.depthSRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		if (auto* encodeCS = GetEncodeTexturesCS()) {
			context->CSSetShader(encodeCS, nullptr, 0);

			for (uint32_t i = 0; i < numEyes; ++i) {
				uint32_t offsetX = i * eyeRenderWidth;

				UpscalingDataCB upscalingData;
				upscalingData.trueSamplingDim = float2((float)eyeRenderWidth, (float)eyeRenderHeight);
				upscalingData.eyeOffsetX = offsetX;
				upscalingDataCB->Update(upscalingData);
				auto upscalingBuffer = upscalingDataCB->CB();
				context->CSSetConstantBuffers(0, 1, &upscalingBuffer);

				// u2 is DLSS-only; u3 provides typed depth for VR FSR and flat runtime FSR.
				ID3D11UnorderedAccessView* depthOutput = nullptr;
				if (upscaleMethod == UpscaleMethod::kFSR) {
					depthOutput = globals::game::isVR ? vrIntermediateLinearDepth[i]->uav.get() :
					                                    (runtimeFsrDepthTexture ? runtimeFsrDepthTexture->uav.get() : nullptr);
				}
				ID3D11UnorderedAccessView* uavs[4] = {
					globals::game::isVR ? vrIntermediateReactiveMask[i]->uav.get() : reactiveMaskTexture->uav.get(),
					globals::game::isVR ? vrIntermediateTransparencyMask[i]->uav.get() : transparencyCompositionMaskTexture->uav.get(),
					(upscaleMethod == UpscaleMethod::kDLSS) ? (globals::game::isVR ? vrIntermediateMotionVectors[i]->uav.get() : motionVectorCopyTexture->uav.get()) : nullptr,
					depthOutput
				};
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

				context->Dispatch((eyeRenderWidth + 7) / 8, (eyeRenderHeight + 7) / 8, 1);
			}
		}

		ID3D11ShaderResourceView* nullViews[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);

		ID3D11UnorderedAccessView* nullUAVs[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAVs), nullUAVs, nullptr);

		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);
	}

	{
		CS_GPU_PASS("Upscaling::Upscale");

		// Opt-in FoveatedRender route, shared by kDLSS/kFSR; falls through to the
		// standard path on failure. Menu-skip is required: in menus the world stops
		// producing fresh motion vectors/depth while kMAIN keeps changing (UI
		// composites), so the subrect route would accumulate history against stale data.
		auto tryFoveatedRoute = [&](ID3D11Resource* a_depth, const char* a_methodLabel) -> bool {
			auto* ui = globals::game::ui;
			auto* st = globals::state;
			const bool menuOpen = st && st->IsPausedOrMenuOpen(ui);
			if (!(FoveatedRenderImpl::Bridge::IsRouteActive() && globals::game::isVR && !menuOpen))
				return false;
			if (!FoveatedRenderImpl::Preprocess::EncodeUpscalingTextures(*this))
				return false;
			const bool routeHandled = FoveatedRenderImpl::Core::ExecuteFoveatedRoute(streamline,
				main.texture, a_depth,
				reactiveMaskTexture->resource.get(),
				transparencyCompositionMaskTexture->resource.get(),
				motionVectorCopyTexture->resource.get());
			if (!routeHandled) {
				logger::warn("[FOVEATED] route preflight failed — falling through to standard {} path", a_methodLabel);
			}
			return routeHandled;
		};

		if (upscaleMethod == UpscaleMethod::kDLSS) {
			// VR-only workaround: a worldspace/cell transition causes ~2-3ms persistent GPU-time
			// regression in the DLSS feature that only clears on a manual mode/preset toggle.
			// Mirror that toggle by tearing down the DLSS feature on LoadingMenu close — the next
			// SetDLSSOptions/slEvaluateFeature call below recreates it with current per-eye extents.
			if (globals::game::isVR && pendingDLSSReset.exchange(false, std::memory_order_relaxed)) {
				logger::debug("[Upscaling] LoadingMenu close detected — rebuilding DLSS feature");
				streamline.DestroyDLSSResources();
			}

			const bool routeHandled = tryFoveatedRoute(
				globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture, "DLSS");
			if (!routeHandled) {
				streamline.Upscale(main.texture, reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get(), motionVectorCopyTexture->resource.get());
			}
		} else if (upscaleMethod == UpscaleMethod::kFSR) {
			// PerfMode bridge: when the engine RTs are shrunk to renderRes, FSR's displayRes
			// output must land in perfMode.testTexture (the private displayRes target used for
			// OpenVR submit), not back in the now-small kMAIN. Mirrors Streamline's colorOut
			// routing for DLSS.
			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			ID3D11Resource* fsrDepth = runtimeFsrDepthTexture ? runtimeFsrDepthTexture->resource.get() : depth.texture;
			ID3D11Resource* fsrColorOut = (perfMode.IsHookActive() && perfMode.GetTestTexture()) ?
			                                  static_cast<ID3D11Resource*>(perfMode.GetTestTexture()) :
			                                  nullptr;

			const bool routeHandled = tryFoveatedRoute(fsrDepth, "FSR");
			if (!routeHandled) {
				fidelityFX.Upscale(main.texture, fsrDepth, reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get(), motionVector.texture, settings.sharpnessFSR, fsrColorOut);
			}
		}
	}
}

void Upscaling::PerformUpscaling()
{
	CS_GPU_PASS("Upscaling::PerformUpscaling");
	Upscale();
	UpscaleDepth();

	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();

	// Disable dynamic resolution past this point
	runtimeData.dynamicResolutionLock = 1;

	// Updates the PerFrame constant buffer so that dynamic resolution settings are disabled
	UpdateCameraData();
}

void Upscaling::UpscaleDepth()
{
	CS_GPU_PASS("Upscaling::UpscaleDepth");
	// Optimization overview:
	// 1) Early validation exits before issuing GPU work.
	// 2) Wide-kernel depth mode uses hysteresis to avoid frequent toggles.
	// 3) Resource copies are skipped for aliased src/dst to reduce copy churn.

	// (1) Early validation exits
	const bool depthUpscaleActive = IsUpscalingActive();
	const auto upscaleMethod = GetUpscaleMethod();
	const bool isVR = globals::game::isVR;
	const bool vendorUpscaler = upscaleMethod == UpscaleMethod::kDLSS || upscaleMethod == UpscaleMethod::kFSR;
	const bool fullResolutionMaskPath =
		upscaleMethod == UpscaleMethod::kNONE ||
		upscaleMethod == UpscaleMethod::kTAA ||
		(vendorUpscaler && settings.qualityMode == 0);
	const bool repairVRFullResolutionMask =
		isVR &&
		fullResolutionMaskPath &&
		!depthUpscaleActive;

	if (!depthUpscaleActive && !repairVRFullResolutionMask) {
		return;
	}

	auto state = globals::state;
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!state || !renderer || !context || !deferred || !deferred->linearSampler || !jitterCB || !upscaleRasterizerState || !upscaleBlendState ||
		(depthUpscaleActive && !upscaleDepthStencilState)) {
		return;
	}

	auto screenSize = state->screenSize;
	if (screenSize.x <= 0.0f || screenSize.y <= 0.0f) {
		return;
	}

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
	auto& refractionNormals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kREFRACTION_NORMALS];
	auto& saoCameraZ = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSAO_CAMERAZ];
	auto& underwaterMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kUNDERWATER_MASK];

	if (!depth.texture || !depthCopy.texture || !depthCopy.depthSRV ||
		!underwaterMask.texture || !underwaterMask.textureCopy || !underwaterMask.SRVCopy || !underwaterMask.RTV) {
		return;
	}
	if (depthUpscaleActive &&
		(!depth.views[0] || !refractionNormals.texture || !refractionNormals.textureCopy || !refractionNormals.SRVCopy || !refractionNormals.RTV || !saoCameraZ.RTV)) {
		return;
	}
	// stencilSRV + views[0] are both upscale-path-only: the depth-upscale
	// draw binds depthCopy as a stencil SRV input and depth.views[0] as DSV.
	// The full-resolution mask repair never touches either, so don't disable
	// the VR fix on setups where stencil SRV creation is unavailable.
	if (depthUpscaleActive && isVR && (!depthCopy.stencilSRV || !depthCopy.views[0])) {
		return;
	}

	auto* fullscreenVS = GetUpscaleVS();
	auto* depthUpscalePS = depthUpscaleActive ? GetDepthRefractionUpscalePS() : nullptr;
	auto* underwaterMaskPS = GetUnderwaterMaskUpscalePS();
	if (!fullscreenVS || !underwaterMaskPS || (depthUpscaleActive && !depthUpscalePS)) {
		return;
	}

	// Unbind any prior render targets before issuing CopyResource on depth/
	// depthCopy. Upscale() does this for the standard upscale path, but
	// UpscaleDepth() can now be invoked standalone from Main_PostProcessing
	// (kNONE/kTAA VR path) without going through Upscale() first — match the
	// same precondition here to avoid a debug-layer hazard when depth happens
	// to still be bound as a DSV from a prior pass.
	context->OMSetRenderTargets(0, nullptr, nullptr);

	// Set up Input Assembler for fullscreen triangle (no vertex/index buffers needed)
	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Set up vertex shader that generates fullscreen triangle using SV_VertexID
	context->VSSetShader(fullscreenVS, nullptr, 0);

	// Set up viewport for fullscreen rendering
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = screenSize.x;
	viewport.Height = screenSize.y;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	// Set rasterizer and blend state
	context->RSSetState(upscaleRasterizerState.get());
	context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

	ID3D11SamplerState* samplers[] = { deferred->linearSampler };
	context->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	// Set up jitter/depth-kernel constant buffer for upscaling
	JitterCB jitterData{};
	jitterData.jitter = jitter;
	// (2) Wide-kernel hysteresis
	if (depthUpscaleActive) {
		constexpr float kEnterWideKernelRatio = 1.55f;
		constexpr float kExitWideKernelRatio = 1.45f;
		const float minScale = std::max(std::min(resolutionScale.x, resolutionScale.y), FLT_EPSILON);
		const float upscaleRatio = 1.0f / minScale;

		if (depthUpscaleUseWideKernel) {
			if (upscaleRatio < kExitWideKernelRatio) {
				depthUpscaleUseWideKernel = false;
			}
		} else {
			if (upscaleRatio > kEnterWideKernelRatio) {
				depthUpscaleUseWideKernel = true;
			}
		}

		jitterData.useWideKernel = depthUpscaleUseWideKernel ? 1.0f : 0.0f;
	}

	jitterCB->Update(jitterData);
	auto bufferArray = jitterCB->CB();
	context->PSSetConstantBuffers(0, 1, &bufferArray);

	// (3) Skip aliased copies
	const auto copyIfNonAliased = [&](ID3D11Resource* dst, ID3D11Resource* src) {
		if (dst && src && dst != src) {
			context->CopyResource(dst, src);
		}
	};

	if (depthUpscaleActive) {
		CS_GPU_PASS("Upscaling::DepthUpscale");

		// Sometimes this is not already copied e.g. map menu.
		// Skip alias copies to reduce unnecessary copy churn.
		copyIfNonAliased(depthCopy.texture, depth.texture);

		// Clear stencil to be 0xFF
		if (isVR) {
			context->ClearDepthStencilView(depthCopy.views[0], D3D11_CLEAR_STENCIL, 1.0f, 0xFF);
		}

		// Set depth stencil state to write 0x00
		context->OMSetDepthStencilState(upscaleDepthStencilState.get(), 0x00);

		copyIfNonAliased(refractionNormals.textureCopy, refractionNormals.texture);

		ID3D11ShaderResourceView* srvs[] = { refractionNormals.SRVCopy, depthCopy.depthSRV, depthCopy.stencilSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		// kSAO_CAMERAZ is at quarter-stereo resolution in VR; the full-stereo viewport would
		// corrupt only the top-left quarter. The engine's ISSAOCameraZ pass populates it correctly.
		ID3D11RenderTargetView* rtvs[] = { refractionNormals.RTV,
			isVR ? nullptr : saoCameraZ.RTV };
		context->OMSetRenderTargets(2, rtvs, depth.views[0]);

		context->PSSetShader(depthUpscalePS, nullptr, 0);
		context->Draw(3, 0);
	} else {
		CS_GPU_PASS("Upscaling::FullResolutionUnderwaterMaskDepthCopy");

		// Full-resolution paths only need to refresh the underwater mask depth source.
		copyIfNonAliased(depthCopy.texture, depth.texture);
	}

	{
		CS_GPU_PASS("Upscaling::UnderwaterMaskUpscale");

		viewport.Width = screenSize.x * 0.5f;
		viewport.Height = screenSize.y * 0.5f;
		context->RSSetViewports(1, &viewport);

		copyIfNonAliased(underwaterMask.textureCopy, underwaterMask.texture);

		context->OMSetDepthStencilState(nullptr, 0x00);

		// t0: vanilla mask copy, t1: original depth (for VR per-eye analytical mask).
		// depthCopy still holds the original pre-upscale depth here (VR re-copy deferred).
		ID3D11ShaderResourceView* srvs[] = { underwaterMask.SRVCopy, depthCopy.depthSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11RenderTargetView* rtvs[] = { underwaterMask.RTV };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

		context->PSSetShader(underwaterMaskPS, nullptr, 0);
		context->Draw(3, 0);
	}

	// Propagate the upscaled depth to kMAIN_COPY so downstream VR passes see
	// it. Skipped on the full-resolution path because the else branch above
	// already refreshed depthCopy from depth and nothing has touched it since.
	if (isVR && depthUpscaleActive) {
		CS_GPU_PASS("Upscaling::DepthVRPropagate");
		copyIfNonAliased(depthCopy.texture, depth.texture);
	}

	ID3D11ShaderResourceView* nullPSResources[3] = { nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(nullPSResources), nullPSResources);
}

void Upscaling::RunUnderwaterMaskRepair()
{
	if (!globals::game::isVR)
		return;

	auto state = globals::state;
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!state || !renderer || !context || !deferred || !deferred->linearSampler || !jitterCB) {
		return;
	}

	auto screenSize = state->screenSize;
	if (screenSize.x <= 0.0f || screenSize.y <= 0.0f) {
		return;
	}

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
	auto& underwaterMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kUNDERWATER_MASK];
	if (!depth.texture || !depthCopy.texture || !depthCopy.depthSRV ||
		!underwaterMask.texture || !underwaterMask.textureCopy || !underwaterMask.SRVCopy || !underwaterMask.RTV) {
		return;
	}

	auto* fullscreenVS = GetUpscaleVS();
	auto* underwaterMaskPS = GetUnderwaterMaskUpscalePS();
	if (!fullscreenVS || !underwaterMaskPS) {
		return;
	}

	CS_GPU_PASS("Upscaling::UnderwaterMaskRepairStandalone");

	// Unbind RTs/DSV before the CopyResource calls below — if the caller
	// still has depth bound as a DSV the copy is a debug-layer hazard.
	// Mirrors UpscaleDepth's entry-time precondition. The caller's save/
	// restore (FullscreenPassScope) restores the original binding on exit.
	context->OMSetRenderTargets(0, nullptr, nullptr);

	// Fullscreen triangle setup — pipeline state is the caller's
	// responsibility to save/restore; we do not touch the existing OM
	// bindings beyond the explicit binds below.
	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(fullscreenVS, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);

	context->RSSetState(nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	context->OMSetDepthStencilState(nullptr, 0x00);

	ID3D11SamplerState* samplers[] = { deferred->linearSampler };
	context->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	// jitterCB is shared with the depth-upscale path; the mask shader only
	// reads .jitter (de-jitter sampling). useWideKernel is depth-only.
	JitterCB jitterData{};
	jitterData.jitter = jitter;
	jitterCB->Update(jitterData);
	auto bufferArray = jitterCB->CB();
	context->PSSetConstantBuffers(0, 1, &bufferArray);

	// Refresh depthCopy + underwater mask copy before sampling.
	if (depthCopy.texture != depth.texture)
		context->CopyResource(depthCopy.texture, depth.texture);
	if (underwaterMask.textureCopy != underwaterMask.texture)
		context->CopyResource(underwaterMask.textureCopy, underwaterMask.texture);

	D3D11_VIEWPORT viewport = {};
	viewport.Width = screenSize.x * 0.5f;
	viewport.Height = screenSize.y * 0.5f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	ID3D11ShaderResourceView* srvs[] = { underwaterMask.SRVCopy, depthCopy.depthSRV };
	context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	ID3D11RenderTargetView* rtvs[] = { underwaterMask.RTV };
	context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);
	context->PSSetShader(underwaterMaskPS, nullptr, 0);
	context->Draw(3, 0);

	ID3D11ShaderResourceView* nullPSResources[2] = { nullptr, nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(nullPSResources), nullPSResources);
}

void Upscaling::ApplySharpening()
{
	if (!settings.sharpnessEnabledDLSS || settings.sharpnessDLSS <= 0.0f)
		return;

	// Streamline::Upscale already redirected DLSS to write into refraTempTex when
	// sharpening is active, so RCAS reads it directly here -- no CopyResource needed.
	if (perfMode.IsHookActive() && perfMode.GetTestTexture()) {
		if (!IsPerfModeSharpenRedirectActive())
			return;

		CS_GPU_PASS("Upscaling::Sharpening");

		float currentSharpness = (-2.0f * settings.sharpnessDLSS) + 2.0f;
		currentSharpness = exp2(-currentSharpness);
		rcas.ApplySharpen(perfMode.GetRefraTempSRV(), perfMode.GetTestTextureUAV(), currentSharpness);
		return;
	}

	if (!sharpenerTexture)
		return;

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	if (!main.texture)
		return;

	CS_GPU_PASS("Upscaling::Sharpening");

	auto context = globals::d3d::context;

	context->OMSetRenderTargets(0, nullptr, nullptr);

	if (settings.sharpnessEnabledDLSS && settings.sharpnessDLSS > 0.0f && main.UAV) {
		// Match FSR3's slider->RCAS conversion exactly (ffx_fsr3upscaler.cpp + FsrRcasCon):
		//   sharpenessRemapped = -2*slider + 2   (sharpness in stops)
		//   rcasAttenuation    = exp2(-sharpenessRemapped) = exp2(2*slider - 2)
		float currentSharpness = (-2.0f * settings.sharpnessDLSS) + 2.0f;
		currentSharpness = exp2(-currentSharpness);

		// DLSS has already written to sharpenerTexture; sharpen directly into kMAIN.UAV.
		rcas.ApplySharpen(sharpenerTexture->srv.get(), main.UAV, currentSharpness);
	} else {
		// Sharpening is disabled: resolve the DLSS output without altering it.
		context->CopyResource(main.texture, sharpenerTexture->resource.get());
	}

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void Upscaling::Main_UpdateJitter::thunk(RE::BSGraphics::State* a_state)
{
	globals::features::upscaling.ConfigureTAA();
	func(a_state);
	globals::features::upscaling.ConfigureUpscaling(a_state);
}

void Upscaling::MenuManagerDrawInterfaceStartHook::thunk(int64_t a1)
{
	globals::features::upscaling.PostDisplay();

	// For non-Frame Gen HDR: redirect kFRAMEBUFFER.RTV to UI texture before vanilla UI renders
	// When FG is active, its SetUIBuffer redirects to uiBufferWrapped instead
	// When HDR Display is not loaded, skip entirely so vanilla UI renders to kFRAMEBUFFER
	auto& upscaling = globals::features::upscaling;
	if (!upscaling.d3d12SwapChainActive && globals::features::hdrDisplay.loaded) {
		globals::features::hdrDisplay.SetUIBuffer();
	}

	func(a1);
}

void Upscaling::Main_PostProcessing::thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5)
{
	auto& postProcessing = globals::features::postProcessing;
	if (postProcessing.loaded) {
		postProcessing.DrawBeforeUpscaling();
	}

	auto& upscaling = globals::features::upscaling;
	auto upscaleMethod = upscaling.GetUpscaleMethod();

	if (upscaling.ShouldUseFrameGenerationThisFrame()) {
		if (postProcessing.loaded)
			postProcessing.ClearBorderMotionVectorsForFrameGen();
		upscaling.CopySharedD3D12Resources();
	}

	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		upscaling.PerformUpscaling();
	} else if (globals::game::isVR) {
		upscaling.UpscaleDepth();
	}

	if (upscaleMethod == UpscaleMethod::kDLSS) {
		// FoveatedRender's DLSS output doesn't land in sharpenerTexture the
		// way dev's path does (the route writes to its own per-eye intermediates
		// and copies back to kMAIN/testTexture), so dev's zero-copy
		// ApplySharpening can't read sharpenerTexture. Route through
		// Postprocess::ApplyDlssSharpening which does the kMAIN → sharpener →
		// kMAIN round-trip. Both paths honor sharpnessDLSS=0 to disable RCAS.
		if (FoveatedRenderImpl::Bridge::IsRouteActive()) {
			FoveatedRenderImpl::Postprocess::ApplyDlssSharpening(upscaling);
		} else {
			upscaling.ApplySharpening();
		}
	}

	Util::SetTemporal(upscaleMethod == UpscaleMethod::kTAA);

	// Redirect kFRAMEBUFFER to float texture before ISHDR runs so HDR values >1.0 survive
	// When HDR Display is not loaded, ISHDR writes to vanilla kFRAMEBUFFER (SDR path)
	bool hdrLoaded = globals::features::hdrDisplay.loaded;
	if (hdrLoaded)
		globals::features::hdrDisplay.RedirectFramebuffer();

	// PerfMode: hybrid Post — HandlePostProcessing performs a two-layer
	// struct swap around the engine's func() so tonemap + refraction read
	// the DisplayRes testTexture instead of the small kMAIN. The supplied
	// lambda is the engine call we'd normally make directly.
	//
	// Upscaler gate: testTexture is populated by whichever upscaler ran
	// (Streamline routes DLSS colorOut there, FidelityFX routes FSR
	// colorOut there). Under PerfMode+hookActive, GetUpscaleMethod() returns
	// the boot snapshot so this check evaluates against the install-time
	// choice — staged UI method changes don't reach here until restart.
	// ShouldHandlePost() covers the partial-init case (post resources
	// missing).
	const bool upscalerWritesTestTexture =
		upscaleMethod == UpscaleMethod::kDLSS ||
		upscaleMethod == UpscaleMethod::kFSR;
	if (upscalerWritesTestTexture && globals::features::upscaling.perfMode.ShouldHandlePost()) {
		globals::features::upscaling.perfMode.HandlePostProcessing([&]() {
			func(a_this, a3, a_target, a_4, a_5);
		});
	} else {
		func(a_this, a3, a_target, a_4, a_5);
	}

	// Restore kFRAMEBUFFER after ISHDR — hdrTexture now has the HDR scene
	if (hdrLoaded)
		globals::features::hdrDisplay.RestoreFramebuffer();

	Util::SetTemporal(false);
}

void Upscaling::SetScissorRect::thunk(RE::BSGraphics::Renderer* This, int a_left, int a_top, int a_right, int a_bottom)
{
	auto viewport = globals::game::graphicsState;
	auto& runtimeData = viewport->GetRuntimeData();

	if (!runtimeData.dynamicResolutionLock) {
		a_left = static_cast<int>(a_left * runtimeData.dynamicResolutionWidthRatio);
		a_right = static_cast<int>(a_right * runtimeData.dynamicResolutionWidthRatio);

		a_top = static_cast<int>(a_top * runtimeData.dynamicResolutionHeightRatio);
		a_bottom = static_cast<int>(a_bottom * runtimeData.dynamicResolutionHeightRatio);
	}

	func(This, a_left, a_top, a_right, a_bottom);
}

void Upscaling::Main_RenderPrecipitation::thunk()
{
	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}

void Upscaling::BSFaceGenManager_UpdatePendingCustomizationTextures::thunk()
{
	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}
