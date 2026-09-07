#include "Streamline.h"

#include <algorithm>
#include <cmath>
#include <dxgi.h>
#include <dxgi1_3.h>

#include "../../Deferred.h"
#include "../../Hooks.h"
#include "../../State.h"
#include "../../Util.h"
#include "../../Utils/NvApiDrs.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"
#include "FoveatedRender/Bridge.h"
#include "PerfMode.h"

void LoggingCallback(sl::LogType type, const char* msg)
{
	// Remove trailing newlines from the raw message
	std::string rawMsg(msg);
	while (!rawMsg.empty() && (rawMsg.back() == '\n' || rawMsg.back() == '\r'))
		rawMsg.pop_back();

	// Remove leading bracketed metadata
	const char* p = msg;
	while (*p == '[') {
		const char* close = strchr(p, ']');
		if (!close)
			break;
		p = close + 1;
		// Skip whitespace after each bracketed section
		while (*p == ' ' || *p == '\t') ++p;
	}
	// Now p points to the first non-bracketed section (file/line info or message)
	std::string cleanMsg(p);
	// Trim leading/trailing whitespace and newlines
	size_t start = cleanMsg.find_first_not_of(" \t\r\n");
	size_t end = cleanMsg.find_last_not_of(" \t\r\n");
	if (start != std::string::npos && end != std::string::npos)
		cleanMsg = cleanMsg.substr(start, end - start + 1);
	else
		cleanMsg.clear();

	// If the cleaned message is empty or only bracketed tokens, log the raw message
	bool onlyBrackets = true;
	for (char c : cleanMsg) {
		if (c != '[' && c != ']' && c != ' ' && c != '\t') {
			onlyBrackets = false;
			break;
		}
	}
	if (cleanMsg.empty() || onlyBrackets) {
		logger::info("[StreamlineSDK:RAW] {}", rawMsg);
		return;
	}

	// Use a clear prefix
	const char* prefix = "[StreamlineSDK]";
	switch (type) {
	case sl::LogType::eInfo:
		logger::info("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eWarn:
		logger::warn("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eError:
		logger::error("{} {}", prefix, cleanMsg);
		break;
	}
}

std::vector<std::pair<std::string, std::string>> Streamline::dllVersions = {};

void Streamline::LoadInterposer()
{
	triedInitialization = true;

	std::filesystem::path pluginDirPath = std::filesystem::path(pluginDir);

	std::wstring interposerPath = (pluginDirPath / interposerDllName).wstring();
	interposer = LoadLibraryW(interposerPath.c_str());
	if (interposer == nullptr) {
		DWORD errorCode = GetLastError();
		logger::info("[Streamline {}] Failed to load interposer: Error Code {:x}", instanceTag, errorCode);
		return;
	} else {
		logger::info("[Streamline {}] Interposer loaded at address: {:p}", instanceTag, static_cast<void*>(interposer));
	}

	// Only log DLL versions from the DX11 instance to avoid duplicate output.
	if (renderAPI == sl::RenderAPI::eD3D11) {
		Streamline::dllVersions = Util::EnumerateDllVersions(pluginDirPath);
		for (const auto& [name, versionStr] : Streamline::dllVersions)
			logger::info("[Streamline DX11] {} version: {}", name, versionStr);
	}

	logger::info("[Streamline {}] Initializing Streamline", instanceTag);

	sl::Preferences pref;

	// DX11 instance: DLSS upscaling + low-latency (Reflex/PCL).
	// DX12 instance: DLSS-G frame generation + Reflex/PCL (DLSS-G requires Reflex).
	sl::Feature featuresDX11[] = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL };
	sl::Feature featuresDX12[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
	if (renderAPI == sl::RenderAPI::eD3D11) {
		pref.featuresToLoad = featuresDX11;
		pref.numFeaturesToLoad = _countof(featuresDX11);
	} else {
		pref.featuresToLoad = featuresDX12;
		pref.numFeaturesToLoad = _countof(featuresDX12);
	}

	// Set log level from settings
	switch (globals::features::upscaling.settings.streamlineLogLevel) {
	case 2:
		pref.logLevel = sl::LogLevel::eVerbose;
		break;
	case 1:
		pref.logLevel = sl::LogLevel::eDefault;
		break;
	case 0:
	default:
		pref.logLevel = sl::LogLevel::eOff;
		break;
	}
	pref.logMessageCallback = LoggingCallback;
	pref.showConsole = false;
	std::error_code pluginPathError;
	auto pluginDirAbsolute = std::filesystem::absolute(pluginDirPath, pluginPathError);
	if (pluginPathError)
		pluginDirAbsolute = pluginDirPath;
	// Each instance needs its own persistent string so the pointer passed to slInit
	// remains valid for the duration of the call.
	static std::wstring pluginDirAbsoluteW_DX11;
	static std::wstring pluginDirAbsoluteW_DX12;
	std::wstring& pluginDirAbsoluteW = (renderAPI == sl::RenderAPI::eD3D11) ? pluginDirAbsoluteW_DX11 : pluginDirAbsoluteW_DX12;
	pluginDirAbsoluteW = pluginDirAbsolute.wstring();
	const wchar_t* pluginPaths[1] = { pluginDirAbsoluteW.c_str() };
	pref.pathsToPlugins = pluginPaths;
	pref.numPathsToPlugins = 1;
	logger::info("[Streamline {}] Plugin search path: {}", instanceTag, pluginDirAbsolute.string());

	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";

	pref.renderAPI = renderAPI;
	pref.flags = sl::PreferenceFlags::eUseManualHooking;
	if (renderAPI == sl::RenderAPI::eD3D12)
		pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;

	// Hook up all of the functions exported by the SL Interposer Library
	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(interposer, "slIsFeatureSupported");
	slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)GetProcAddress(interposer, "slIsFeatureLoaded");
	slSetFeatureLoaded = (PFun_slSetFeatureLoaded*)GetProcAddress(interposer, "slSetFeatureLoaded");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
	slAllocateResources = (PFun_slAllocateResources*)GetProcAddress(interposer, "slAllocateResources");
	slFreeResources = (PFun_slFreeResources*)GetProcAddress(interposer, "slFreeResources");
	slSetTag = (PFun_slSetTag*)GetProcAddress(interposer, "slSetTag");
	slSetTagForFrame = (PFun_slSetTagForFrame*)GetProcAddress(interposer, "slSetTagForFrame");
	slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GetProcAddress(interposer, "slGetFeatureRequirements");
	slGetFeatureVersion = (PFun_slGetFeatureVersion*)GetProcAddress(interposer, "slGetFeatureVersion");
	slUpgradeInterface = (PFun_slUpgradeInterface*)GetProcAddress(interposer, "slUpgradeInterface");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetNativeInterface = (PFun_slGetNativeInterface*)GetProcAddress(interposer, "slGetNativeInterface");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");

	if (SL_FAILED(res, slInit(pref, sl::kSDKVersion))) {
		logger::critical("[Streamline {}] Failed to initialize Streamline", instanceTag);
	} else {
		initialized = true;
		featureDLSS = false;
		featureDLSSG = false;
		featureReflex = false;
		featurePCL = false;
		reflexSupportedOnCurrentAdapter = false;
		reflexOptionsCache = {};
		lastReflexSleepFrame = UINT32_MAX;
		logger::info("[Streamline {}] Successfully initialized Streamline", instanceTag);
	}
}

void Streamline::CheckFeatures(IDXGIAdapter* a_adapter)
{
	logger::info("[Streamline {}] Checking features", instanceTag);
	DXGI_ADAPTER_DESC adapterDesc;
	a_adapter->GetDesc(&adapterDesc);
	reflexSupportedOnCurrentAdapter = adapterDesc.VendorId == kNvidiaVendorId;

	sl::AdapterInfo adapterInfo;
	adapterInfo.deviceLUID = (uint8_t*)&adapterDesc.AdapterLuid;
	adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

	auto checkFeatureAvailability = [&](sl::Feature feature, const char* featureName, bool& outAvailable) {
		outAvailable = false;
		bool loaded = false;
		if (SL_FAILED(result, slIsFeatureLoaded(feature, loaded))) {
			logger::warn("[Streamline {}] {} load-state query failed: {}", instanceTag, featureName, magic_enum::enum_name(result));
			return;
		}
		if (!loaded) {
			logger::info("[Streamline {}] {} feature is not loaded", instanceTag, featureName);
			sl::FeatureRequirements featureRequirements;
			sl::Result requirementsResult = slGetFeatureRequirements(feature, featureRequirements);
			if (requirementsResult != sl::Result::eOk) {
				logger::info("[Streamline {}] {} feature failed to load due to: {}", instanceTag, featureName, magic_enum::enum_name(requirementsResult));
			}
			return;
		}

		logger::info("[Streamline {}] {} feature is loaded", instanceTag, featureName);
		outAvailable = slIsFeatureSupported(feature, adapterInfo) == sl::Result::eOk;
	};

	if (renderAPI == sl::RenderAPI::eD3D11) {
		checkFeatureAvailability(sl::kFeatureDLSS, "DLSS", featureDLSS);
		logger::info("[Streamline DX11] DLSS {} available", featureDLSS ? "is" : "is not");
	} else {
		checkFeatureAvailability(sl::kFeatureDLSS_G, "DLSS-G", featureDLSSG);
		logger::info("[Streamline DX12] DLSS-G {} available", featureDLSSG ? "is" : "is not");
	}

	if (reflexSupportedOnCurrentAdapter) {
		checkFeatureAvailability(sl::kFeatureReflex, "Reflex", featureReflex);
		checkFeatureAvailability(sl::kFeaturePCL, "PCL", featurePCL);
		logger::info("[Streamline {}] Reflex {} available", instanceTag, featureReflex ? "is" : "is not");
		logger::info("[Streamline {}] PCL {} available", instanceTag, featurePCL ? "is" : "is not");
	} else {
		featureReflex = false;
		featurePCL = false;
		logger::info("[Streamline {}] Reflex/PCL disabled on non-NVIDIA adapter", instanceTag);
	}

	reflexOptionsCache = {};
	lastReflexSleepFrame = UINT32_MAX;
}

bool Streamline::BindFeatureFunction(sl::Feature a_feature, const char* a_functionName, void*& a_function)
{
	a_function = nullptr;
	const sl::Result bindResult = slGetFeatureFunction(a_feature, a_functionName, a_function);
	if (bindResult != sl::Result::eOk)
		logger::warn("[Streamline {}] {} bind failed with {}", instanceTag, a_functionName, magic_enum::enum_name(bindResult));
	return bindResult == sl::Result::eOk && a_function != nullptr;
}

void Streamline::RequestFeatureLoad(sl::Feature a_feature, const char* a_featureName)
{
	const sl::Result loadResult = slSetFeatureLoaded(a_feature, true);
	if (loadResult != sl::Result::eOk)
		logger::warn("[Streamline {}] Failed to request {} load: {}", instanceTag, a_featureName, magic_enum::enum_name(loadResult));
}

void Streamline::BindReflexAndPCL()
{
	if (!slGetFeatureFunction || !reflexSupportedOnCurrentAdapter)
		return;

	if (slSetFeatureLoaded) {
		// Reflex/PCL availability can change after device bind; request explicit load here.
		RequestFeatureLoad(sl::kFeatureReflex, "Reflex");
		RequestFeatureLoad(sl::kFeaturePCL, "PCL");
	}

	// Keep runtime controls strict: only advertise Reflex/PCL as available when required entry points bind.
	bool reflexFnsBound = true;
	reflexFnsBound &= BindFeatureFunction(sl::kFeatureReflex, "slReflexGetState", (void*&)slReflexGetState);
	reflexFnsBound &= BindFeatureFunction(sl::kFeatureReflex, "slReflexSleep", (void*&)slReflexSleep);
	reflexFnsBound &= BindFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", (void*&)slReflexSetOptions);
	featureReflex = reflexFnsBound && slReflexSetOptions && slReflexSleep;
	if (!featureReflex)
		logger::warn("[Streamline {}] Reflex functions are missing; Reflex runtime controls will be disabled", instanceTag);
	else
		logger::info("[Streamline {}] Reflex runtime controls are available", instanceTag);

	bool pclFnBound = BindFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", (void*&)slPCLSetMarker);
	featurePCL = pclFnBound && slPCLSetMarker;
	if (!featurePCL)
		logger::warn("[Streamline {}] PCL marker function is unavailable; marker optimization requests will be ignored", instanceTag);
	else
		logger::info("[Streamline {}] PCL marker interface is available", instanceTag);
}

void Streamline::PostDevice()
{
	// Hook up all of the feature functions using the sl function slGetFeatureFunction

	if (renderAPI == sl::RenderAPI::eD3D12) {
		slDLSSGGetState = nullptr;
		slDLSSGSetOptions = nullptr;
		featureDLSSG = false;
		slReflexGetState = nullptr;
		slReflexSleep = nullptr;
		slReflexSetOptions = nullptr;
		slPCLSetMarker = nullptr;

		if (slGetFeatureFunction) {
			bool dlssgFnsBound = true;
			dlssgFnsBound &= BindFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", (void*&)slDLSSGGetState);
			dlssgFnsBound &= BindFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)slDLSSGSetOptions);
			featureDLSSG = dlssgFnsBound && slDLSSGGetState && slDLSSGSetOptions;

			if (!featureDLSSG) {
				logger::warn("[Streamline DX12] DLSS-G functions missing; DLSS-G runtime controls will be disabled");
				dlssgMaxFramesToGenerate = 1;
			} else {
				logger::info("[Streamline DX12] DLSS-G runtime controls are available");

				sl::DLSSGState state{};
				if (SL_FAILED(result, slDLSSGGetState(viewport, state, nullptr))) {
					logger::warn("[Streamline DX12] slDLSSGGetState failed querying numFramesToGenerateMax: {}", magic_enum::enum_name(result));
					dlssgMaxFramesToGenerate = 1;
				} else {
					dlssgMaxFramesToGenerate = std::max<uint32_t>(1, state.numFramesToGenerateMax);
					logger::info("[Streamline DX12] DLSS-G supports up to {}x frame generation", dlssgMaxFramesToGenerate + 1);
				}
			}

			BindReflexAndPCL();
		}

		reflexOptionsCache = {};
		lastReflexSleepFrame = UINT32_MAX;
		return;
	}

	// DX11 instance: DLSS + Reflex + PCL
	if (featureDLSS) {
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", (void*&)slDLSSGetOptimalSettings);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetState", (void*&)slDLSSGetState);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", (void*&)slDLSSSetOptions);
	}

	slReflexGetState = nullptr;
	slReflexSleep = nullptr;
	slReflexSetOptions = nullptr;
	slPCLSetMarker = nullptr;
	featureReflex = false;
	featurePCL = false;

	BindReflexAndPCL();

	reflexOptionsCache = {};
	lastReflexSleepFrame = UINT32_MAX;
}

void Streamline::SetD3DDevice12(ID3D12Device* a_device)
{
	if (!initialized || !slSetD3DDevice || !a_device)
		return;
	if (SL_FAILED(result, slSetD3DDevice(static_cast<void*>(a_device))))
		logger::error("[Streamline DX12] slSetD3DDevice(device) failed: {}", magic_enum::enum_name(result));
	else
		logger::info("[Streamline DX12] D3D12 device bound");
}

void Streamline::EnsureDriverProfileAllowsDLSSG()
{
	Util::NvApiDrs::Api drs{};
	if (!drs.Load())
		return;

	Util::NvApiDrs::SessionHandle session{};
	if (drs.CreateSession(&session) != 0)
		return;
	if (drs.LoadSettings(session) != 0) {
		drs.DestroySession(session);
		return;
	}

	uint16_t profileName[2048]{};
	Util::NvApiDrs::Api::CopyProfileName(Util::NvApiDrs::kSkyrimSEProfileName, profileName);

	Util::NvApiDrs::ProfileHandle profile{};
	// No profile means driver defaults, which allow DLSS-G.
	if (drs.FindProfileByName(session, profileName, &profile) == 0) {
		Util::NvApiDrs::Setting setting{};
		setting.version = Util::NvApiDrs::kSettingVersion;
		if (drs.GetSetting(session, profile, Util::NvApiDrs::kKeyDLSSGDisable, &setting) == 0 && setting.u32CurrentValue != 0) {
			// The NVIDIA App may re-assert the key later, so this runs every boot.
			logger::info("[Streamline DX12] Driver profile disables DLSS-G (DRS key {:#x}={}); resetting to driver default",
				Util::NvApiDrs::kKeyDLSSGDisable, setting.u32CurrentValue);
			Util::NvApiDrs::Setting newSetting{};
			newSetting.version = Util::NvApiDrs::kSettingVersion;
			newSetting.settingId = Util::NvApiDrs::kKeyDLSSGDisable;
			newSetting.settingType = 0;
			newSetting.u32CurrentValue = 0;
			if (drs.SetSetting(session, profile, &newSetting) != 0 || drs.SaveSettings(session) != 0)
				logger::warn(
					"[Streamline DX12] Failed to reset the DRS key; DLSS-G will report eOk but generate no frames. "
					"Disable the DLSS override for Skyrim in the NVIDIA App, or clear key {:#x} with NVIDIA Profile Inspector.",
					Util::NvApiDrs::kKeyDLSSGDisable);
			else
				logger::info("[Streamline DX12] DRS key reset; if frame generation does not engage this session, restart the game");
		}
	}

	drs.DestroySession(session);
}

/**
 * @brief Updates and sets camera and frame constants for the current Streamline frame.
 *
 * Populates and submits camera parameters, projection matrices, motion vector settings, and other per-frame constants to the Streamline SDK for the current frame. Uses cached framebuffer data and global state to ensure correct configuration for upscaling and frame generation features.
 */
bool Streamline::EnsureFrameToken()
{
	if (!initialized || !slGetNewFrameToken || !globals::state)
		return false;

	if (!frameChecker.IsNewFrame())
		return frameToken != nullptr;

	if (SL_FAILED(result, slGetNewFrameToken(frameToken, &globals::state->frameCount))) {
		logger::error("[Streamline {}] Could not get frame token: {}", instanceTag, magic_enum::enum_name(result));
		frameToken = nullptr;
		return false;
	}

	return frameToken != nullptr;
}

bool Streamline::CheckFrameConstants(sl::ViewportHandle p_viewport, uint32_t eyeIndex)
{
	if (!initialized)
		return false;

	if (!EnsureFrameToken())
		return false;

	// In VR, we need to set constants for each viewport/eye separately
	// In non-VR, this is called once per frame
	auto state = globals::state;

	sl::Constants slConstants = {};

	// Calculate aspect ratio for the SINGLE EYE
	float eyeWidth = state->screenSize.x * (globals::game::isVR ? 0.5f : 1.0f);
	slConstants.cameraAspectRatio = eyeWidth / state->screenSize.y;

	slConstants.cameraFOV = Util::GetVerticalFOVRad();
	slConstants.cameraNear = *globals::game::cameraNear;
	slConstants.cameraFar = *globals::game::cameraFar;

	auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse(eyeIndex).Transpose();
	auto cameraViewToClip = globals::game::frameBufferCached.GetCameraProjUnjittered(eyeIndex).Transpose();

	slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
	slConstants.cameraPinholeOffset = { 0.f, 0.f };
	slConstants.cameraRight = { viewMatrix._11, viewMatrix._12, viewMatrix._13 };
	slConstants.cameraUp = { viewMatrix._21, viewMatrix._22, viewMatrix._23 };
	slConstants.cameraFwd = { viewMatrix._31, viewMatrix._32, viewMatrix._33 };
	slConstants.cameraPos = *(sl::float3*)&globals::game::frameBufferCached.GetCameraPosAdjust(eyeIndex);
	slConstants.cameraViewToClip = *(sl::float4x4*)&cameraViewToClip;
	slConstants.depthInverted = sl::Boolean::eFalse;

	if (globals::game::isVR) {
		// VR: compute clipToCameraView / clipToPrevClip / prevClipToClip from Skyrim's per-eye matrices.
		// recalculateCameraMatrices() uses a single static prev-frame slot -- unusable for two viewports.
		sl::matrixFullInvert(slConstants.clipToCameraView, slConstants.cameraViewToClip);

		auto currViewProj = globals::game::frameBufferCached.GetCameraViewProjUnjittered(eyeIndex).Transpose();
		auto prevViewProj = globals::game::frameBufferCached.GetCameraPreviousViewProjUnjittered(eyeIndex).Transpose();

		sl::float4x4 currViewProjSL = *(sl::float4x4*)&currViewProj;
		sl::float4x4 prevViewProjSL = *(sl::float4x4*)&prevViewProj;

		sl::float4x4 invCurrViewProj;
		sl::matrixFullInvert(invCurrViewProj, currViewProjSL);
		sl::matrixMul(slConstants.clipToPrevClip, invCurrViewProj, prevViewProjSL);
		sl::matrixFullInvert(slConstants.prevClipToClip, slConstants.clipToPrevClip);
	} else {
		recalculateCameraMatrices(slConstants);
	}

	auto& upscaling = globals::features::upscaling;
	auto jitter = upscaling.jitter;
	slConstants.jitterOffset = { -jitter.x, -jitter.y };
	// Static menu backdrops render no reliable motion vectors; camera-derived MVs
	// restore valid reprojection there. Reset only when that fill could not run —
	// accumulating against zero MVs ghosts.
	slConstants.reset = (state->IsStaticMenuBackdropOpen(globals::game::ui) && !upscaling.menuCameraMVsValid) ?
	                        sl::Boolean::eTrue :
	                        sl::Boolean::eFalse;

	// Apply foveated mvec scale only when the subrect execute path is actually
	// running this frame (flag set by ExecuteFoveatedRoute). The standard full-frame
	// DLSS path — including menus and frames where foveated is skipped — must
	// keep mvecScale at identity or DLSS massively over-estimates motion.
	float mvecX = 1.0f, mvecY = 1.0f;
	if (FoveatedRenderImpl::Bridge::foveatedEvaluating)
		FoveatedRenderImpl::Bridge::ComputeMvecScale(eyeIndex, mvecX, mvecY);
	slConstants.mvecScale = { mvecX, mvecY };
	slConstants.motionVectors3D = sl::Boolean::eFalse;
	slConstants.motionVectorsInvalidValue = FLT_MIN;
	slConstants.orthographicProjection = sl::Boolean::eFalse;
	slConstants.motionVectorsDilated = sl::Boolean::eFalse;
	slConstants.motionVectorsJittered = sl::Boolean::eFalse;

	if (SL_FAILED(res, slSetConstants(slConstants, *frameToken, p_viewport))) {
		logger::error("[Streamline {}] Could not set constants for eye {}", instanceTag, eyeIndex);
		return false;
	}

	return true;
}

sl::DLSSMode Streamline::DLSSModeForQualityMode(uint32_t a_qualityMode)
{
	switch (a_qualityMode) {
	case 1:
		return sl::DLSSMode::eMaxQuality;
	case 2:
		return sl::DLSSMode::eBalanced;
	case 3:
		return sl::DLSSMode::eMaxPerformance;
	case 4:
		return sl::DLSSMode::eUltraPerformance;
	default:
		return sl::DLSSMode::eDLAA;
	}
}

void Streamline::ClampToDLSSRenderRange(uint32_t a_qualityMode, uint32_t a_outputWidth, uint32_t a_outputHeight, uint32_t& a_renderWidth, uint32_t& a_renderHeight)
{
	if (!featureDLSS || !slDLSSGetOptimalSettings)
		return;

	sl::DLSSOptions options{};
	options.mode = DLSSModeForQualityMode(a_qualityMode);
	options.outputWidth = a_outputWidth;
	options.outputHeight = a_outputHeight;
	sl::DLSSOptimalSettings optimal{};
	if (slDLSSGetOptimalSettings(options, optimal) != sl::Result::eOk)
		return;
	if (!optimal.renderWidthMin || !optimal.renderHeightMin || !optimal.renderWidthMax || !optimal.renderHeightMax)
		return;

	// Prefer even dims (half-res buffer alignment) but never leave the NGX range.
	auto clampEven = [](uint32_t v, uint32_t lo, uint32_t hi) {
		v = std::clamp(v, lo, hi);
		if ((v & 1u) && v + 1 <= hi)
			++v;
		else if ((v & 1u) && v > lo)
			--v;
		return v;
	};
	const uint32_t clampedW = clampEven(a_renderWidth, optimal.renderWidthMin, optimal.renderWidthMax);
	const uint32_t clampedH = clampEven(a_renderHeight, optimal.renderHeightMin, optimal.renderHeightMax);
	if (clampedW != a_renderWidth || clampedH != a_renderHeight)
		logger::info("[Streamline] Render extent {}x{} clamped to DLSS mode {} range [{}x{}..{}x{}] -> {}x{}",
			a_renderWidth, a_renderHeight, a_qualityMode,
			optimal.renderWidthMin, optimal.renderHeightMin, optimal.renderWidthMax, optimal.renderHeightMax,
			clampedW, clampedH);
	a_renderWidth = clampedW;
	a_renderHeight = clampedH;
}

void Streamline::SetDLSSOptions(sl::ViewportHandle p_viewport, uint32_t width, uint32_t height)
{
	sl::DLSSOptions dlssOptions{};

	// DLSS dispatch must match the renderRes the engine RTs were latched for,
	// not the live preset.
	auto& perfModeRef = globals::features::upscaling.perfMode;
	const uint32_t qualityMode = perfModeRef.IsHookActive() ? perfModeRef.GetLatchedQualityMode() : globals::features::upscaling.settings.qualityMode;
	dlssOptions.mode = DLSSModeForQualityMode(qualityMode);

	auto state = globals::state;

	// PerfMode bridge: state->screenSize.y is polluted to RenderRes by the
	// BSOpenVR size hook; use perfMode's snapshot of the real DisplayRes when
	// the hook is live so DLSS is created at the right scale. The width arg
	// is already display-correct (caller computes from displaySize).
	auto& perfMode = globals::features::upscaling.perfMode;
	const bool dlssperfActive = perfMode.IsHookActive() && perfMode.GetTestTexture();

	dlssOptions.outputWidth = width;
	// height==0 → caller is the standard upscale path; use full per-eye DisplayRes height.
	// Non-zero is the FoveatedRender subrect height — must match extentOut.height or NGX
	// produces zeroed output. See SetDLSSOptions decl in Streamline.h for the rationale.
	dlssOptions.outputHeight = height != 0 ? height : (dlssperfActive ? (uint)perfMode.GetDisplayScreenSize().y : (uint)state->screenSize.y);

	// Detect HDR from kMAIN format at runtime -- VR kMAIN may be 8-bit while SE is FP16
	{
		auto renderer = globals::game::renderer;
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		D3D11_TEXTURE2D_DESC mainDesc;
		static_cast<ID3D11Texture2D*>(main.texture)->GetDesc(&mainDesc);
		bool isHDR = mainDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM;
		dlssOptions.colorBuffersHDR = isHDR ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	}
	dlssOptions.useAutoExposure = sl::Boolean::eTrue;

	std::optional<sl::DLSSPreset> customPreset;
	switch (globals::features::upscaling.settings.presetDLSS) {
	case 1:
		customPreset = sl::DLSSPreset::ePresetJ;
		break;
	case 2:
		customPreset = sl::DLSSPreset::ePresetK;
		break;
	case 3:
		customPreset = sl::DLSSPreset::ePresetL;
		break;
	case 4:
		customPreset = sl::DLSSPreset::ePresetM;
		break;
	}

	// Keep eDefault for Auto so NVIDIA can update the mode-specific presets.
	if (customPreset.has_value()) {
		dlssOptions.dlaaPreset = customPreset.value();
		dlssOptions.ultraQualityPreset = customPreset.value();
		dlssOptions.qualityPreset = customPreset.value();
		dlssOptions.balancedPreset = customPreset.value();
		dlssOptions.performancePreset = customPreset.value();
		dlssOptions.ultraPerformancePreset = customPreset.value();
	}

	dlssOptions.preExposure = 1.0f;
	dlssOptions.sharpness = 0.0f;

	if (SL_FAILED(result, slDLSSSetOptions(p_viewport, dlssOptions))) {
		logger::critical("[Streamline DX11] Could not enable DLSS");
	}
}

bool Streamline::EvaluateDLSS(sl::ViewportHandle vp, uint32_t eyeIndex,
	ID3D11Resource* colorIn, ID3D11Resource* colorOut, ID3D11Resource* depth,
	ID3D11Resource* mvec, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask,
	const sl::Extent& extentIn, const sl::Extent& extentOut, uint32_t outputWidth,
	uint32_t outputHeight)
{
	auto context = globals::d3d::context;

	sl::Resource colorInRes = { sl::ResourceType::eTex2d, colorIn, 0 };
	sl::Resource colorOutRes = { sl::ResourceType::eTex2d, colorOut, 0 };
	sl::Resource depthRes = { sl::ResourceType::eTex2d, depth, 0 };
	sl::Resource mvecRes = { sl::ResourceType::eTex2d, mvec, 0 };
	sl::Resource reactiveMaskRes = { sl::ResourceType::eTex2d, reactiveMask, 0 };
	sl::Resource transparencyMaskRes = { sl::ResourceType::eTex2d, transparencyMask, 0 };

	if (!CheckFrameConstants(vp, eyeIndex))
		return false;

	const bool emitPCLMarkers =
		globals::features::upscaling.settings.reflexUseMarkersToOptimize &&
		reflexOptionsCache.useMarkersToOptimize &&
		featurePCL;
	const auto emitPCLMarker = [&](sl::PCLMarker marker, const char* stageName, uint32_t stageIndex) {
		if (!emitPCLMarkers || !slPCLSetMarker || !frameToken)
			return;
		const sl::Result markerResult = slPCLSetMarker(marker, *frameToken);
		if (markerResult != sl::Result::eOk) {
			static bool markerErrorLogged[2][2] = { { false, false }, { false, false } };
			const uint32_t logIdx = globals::game::isVR ? std::min(eyeIndex, 1u) : 0u;
			const uint32_t boundedStageIndex = std::min(stageIndex, 1u);
			if (markerErrorLogged[logIdx][boundedStageIndex])
				return;
			markerErrorLogged[logIdx][boundedStageIndex] = true;
			logger::warn(
				"[Streamline {}] slPCLSetMarker({}) failed{}: {}",
				instanceTag,
				stageName,
				globals::game::isVR ? std::format(" for eye {}", eyeIndex) : "",
				magic_enum::enum_name(markerResult));
		}
	};

	SetDLSSOptions(vp, outputWidth, outputHeight);

	sl::ResourceTag tags[] = {
		{ &colorInRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &extentIn },
		{ &colorOutRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &extentOut },
		{ &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &reactiveMaskRes, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &transparencyMaskRes, sl::kBufferTypeTransparencyHint, sl::ResourceLifecycle::eValidUntilPresent, &extentIn }
	};

	slSetTag(vp, tags, _countof(tags), context);

	sl::ViewportHandle view(vp);
	const sl::BaseStructure* inputs[] = { &view };

	auto state = globals::state;
	if (state->frameAnnotations) {
		if (globals::game::isVR) {
			char buf[32];
			snprintf(buf, sizeof(buf), "DLSS Evaluate Eye %u", eyeIndex);
			state->BeginPerfEvent(buf);
		} else {
			state->BeginPerfEvent("DLSS Evaluate");
		}
	}

	emitPCLMarker(sl::PCLMarker::eRenderSubmitStart, "DLSS-EvaluateStart", 0);
	sl::Result evalResult = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), context);
	emitPCLMarker(sl::PCLMarker::eRenderSubmitEnd, "DLSS-EvaluateEnd", 1);

	if (state->frameAnnotations)
		state->EndPerfEvent();

	if (evalResult != sl::Result::eOk) {
		static bool evalErrorLogged[2] = { false, false };
		uint32_t logIdx = globals::game::isVR ? eyeIndex : 0;
		if (!evalErrorLogged[logIdx]) {
			evalErrorLogged[logIdx] = true;
			logger::error("[Streamline {}] slEvaluateFeature failed{} result={}", instanceTag, globals::game::isVR ? std::format(" for eye {}", eyeIndex) : "", (int)evalResult);
		}
		return false;
	}

	return true;
}

void Streamline::Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors)
{
	auto state = globals::state;

	auto renderer = globals::game::renderer;
	auto& depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	auto screenSize = state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	// PerfMode bridge: when the BSOpenVR size hook is live, state->screenSize
	// is polluted to RenderRes (the spoofed HMD recommended size). DLSS must
	// be told the TRUE DisplayRes for its output extent, otherwise NGX rejects
	// the evaluate as InvalidParameter (0xbad00005) because the configured
	// quality-scale doesn't match the actual extent ratio. The upscale also
	// has to write into perfMode's private DisplayRes testTexture instead of
	// the now-RenderRes kMAIN.
	// DLSS input and output must not alias. Always write to the intermediate texture,
	// then either sharpen or copy the result back to kMAIN.
	auto& upscaling = globals::features::upscaling;
	auto& perfMode = globals::features::upscaling.perfMode;
	const bool dlssperfActive = perfMode.IsHookActive() && perfMode.GetTestTexture();
	const auto displaySize = dlssperfActive ? perfMode.GetDisplayScreenSize() : screenSize;

	// Sharpening active -> write to the RCAS read source (sharpenerTexture, or PerfMode's
	// refraTempTex) directly so RCAS can sharpen with no CopyResource round-trip.
	const bool dlssperfSharpen = upscaling.IsPerfModeSharpenRedirectActive();
	ID3D11Resource* colorOut =
		dlssperfActive ? (dlssperfSharpen ? static_cast<ID3D11Resource*>(perfMode.GetRefraTempTex()) : static_cast<ID3D11Resource*>(perfMode.GetTestTexture())) :
						 ((upscaling.settings.sharpnessEnabledDLSS && upscaling.settings.sharpnessDLSS > 0.0f && upscaling.sharpenerTexture) ? upscaling.sharpenerTexture->resource.get() : a_upscalingTexture);

	// VR stereo DLSS: NGX D3D11 only accepts zero-offset subrects. Non-zero offsets return
	// FAIL_InvalidParameter because Streamline's dlssEntry.cpp never sets
	// NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects during context creation.
	//
	// Both eyes copy their color slice into per-eye intermediates so ClearHMDMask can zero
	// outside-mask regions before DLSS sees them (prevents temporal bleed into visible pixels).
	// Eye 0 outputs directly to colorOut (zero-offset) — no intermediate output buffer needed.
	// Eye 1 outputs to vrIntermediateColorOut[1] then copies back to kMAIN at eyeWidthOut.
	//
	// Eye 1 is pre-copied before eye 0 runs: at non-DLAA scales eye 0's upscaled output
	// extends past eyeWidthIn into eye 1's input region of kMAIN.
	if (globals::game::isVR) {
		auto context = globals::d3d::context;

		uint32_t eyeWidthOut = (uint32_t)(displaySize.x / 2);
		uint32_t eyeHeightOut = (uint32_t)displaySize.y;
		uint32_t eyeWidthIn = (uint32_t)(renderSize.x / 2);
		uint32_t eyeHeightIn = (uint32_t)renderSize.y;

		sl::Extent perEyeIn{ 0, 0, eyeWidthIn, eyeHeightIn };
		sl::Extent perEyeOut{ 0, 0, eyeWidthOut, eyeHeightOut };

		// Both flags track the same creation pool (EnsureVRIntermediateTextures creates all
		// intermediates atomically), so in practice eye0Ready == eye1Ready. The separate checks
		// are kept for null-safety and to document which resources each eye path actually uses.
		bool eye0Ready = upscaling.vrIntermediateColorIn[0] &&
		                 upscaling.vrIntermediateMotionVectors[0] && upscaling.vrIntermediateReactiveMask[0] && upscaling.vrIntermediateTransparencyMask[0];
		bool eye1Ready = upscaling.vrIntermediateColorIn[1] && upscaling.vrIntermediateColorOut[1] &&
		                 upscaling.vrIntermediateDepth && upscaling.vrIntermediateMotionVectors[1] &&
		                 upscaling.vrIntermediateReactiveMask[1] && upscaling.vrIntermediateTransparencyMask[1];

		// Pre-copy eye 1 before eye 0 runs (overlap hazard), then clear HMD mask.
		if (eye1Ready) {
			D3D11_BOX rightIn = { eyeWidthIn, 0, 0, eyeWidthIn * 2, eyeHeightIn, 1 };
			context->CopySubresourceRegion(upscaling.vrIntermediateColorIn[1]->resource.get(), 0, 0, 0, 0, a_upscalingTexture, 0, &rightIn);
			context->CopySubresourceRegion(upscaling.vrIntermediateDepth->resource.get(), 0, 0, 0, 0, depthTexture.texture, 0, &rightIn);
			upscaling.ClearHMDMask(upscaling.vrIntermediateColorIn[1]->uav.get(), depthTexture.depthSRV,
				eyeWidthIn, eyeHeightIn, eyeWidthIn, 0);
		}

		// Eye 0: copy left-eye slice, clear HMD mask, output directly to colorOut at offset 0.
		if (eye0Ready) {
			D3D11_BOX leftIn = { 0, 0, 0, eyeWidthIn, eyeHeightIn, 1 };
			context->CopySubresourceRegion(upscaling.vrIntermediateColorIn[0]->resource.get(), 0, 0, 0, 0, a_upscalingTexture, 0, &leftIn);
			upscaling.ClearHMDMask(upscaling.vrIntermediateColorIn[0]->uav.get(), depthTexture.depthSRV,
				eyeWidthIn, eyeHeightIn, 0, 0);

			EvaluateDLSS(viewport, 0,
				upscaling.vrIntermediateColorIn[0]->resource.get(), colorOut,
				depthTexture.texture,
				upscaling.vrIntermediateMotionVectors[0]->resource.get(),
				upscaling.vrIntermediateReactiveMask[0]->resource.get(),
				upscaling.vrIntermediateTransparencyMask[0]->resource.get(),
				perEyeIn, perEyeOut, eyeWidthOut);
		}

		// Eye 1: evaluate into intermediate, then copy upscaled result to kMAIN right-eye position.
		if (eye1Ready) {
			EvaluateDLSS(viewportRight, 1,
				upscaling.vrIntermediateColorIn[1]->resource.get(),
				upscaling.vrIntermediateColorOut[1]->resource.get(),
				upscaling.vrIntermediateDepth->resource.get(),
				upscaling.vrIntermediateMotionVectors[1]->resource.get(),
				upscaling.vrIntermediateReactiveMask[1]->resource.get(),
				upscaling.vrIntermediateTransparencyMask[1]->resource.get(),
				perEyeIn, perEyeOut, eyeWidthOut);

			D3D11_BOX rightOut = { 0, 0, 0, eyeWidthOut, eyeHeightOut, 1 };
			context->CopySubresourceRegion(colorOut, 0, eyeWidthOut, 0, 0, upscaling.vrIntermediateColorOut[1]->resource.get(), 0, &rightOut);
		}
	} else {
		// Non-VR: Simple full-texture upscale.
		sl::Extent extentIn{ 0, 0, (uint)renderSize.x, (uint)renderSize.y };
		sl::Extent extentOut{ 0, 0, (uint)displaySize.x, (uint)displaySize.y };

		EvaluateDLSS(viewport, 0,
			a_upscalingTexture, colorOut,
			depthTexture.texture, a_motionVectors, a_reactiveMask, a_transparencyCompositionMask,
			extentIn, extentOut, (uint)displaySize.x);
	}
}

void Streamline::UpdateReflex()
{
	if (!initialized || !reflexSupportedOnCurrentAdapter || !featureReflex || !slReflexSetOptions)
		return;

	const auto applyReflexOptionsIfChanged = [&](const sl::ReflexOptions& options, const char* onFailMessage) {
		if (reflexOptionsCache.valid &&
			reflexOptionsCache.mode == options.mode &&
			reflexOptionsCache.frameLimitUs == options.frameLimitUs &&
			reflexOptionsCache.useMarkersToOptimize == options.useMarkersToOptimize) {
			return;
		}

		if (SL_FAILED(result, slReflexSetOptions(options))) {
			logger::error("[Streamline {}] {}: {}", instanceTag, onFailMessage, magic_enum::enum_name(result));
			return;
		}

		reflexOptionsCache.valid = true;
		reflexOptionsCache.mode = options.mode;
		reflexOptionsCache.frameLimitUs = options.frameLimitUs;
		reflexOptionsCache.useMarkersToOptimize = options.useMarkersToOptimize;
	};

	const auto& upscaling = globals::features::upscaling;

	// Disable DX11 Reflex only when DLSS-G actually drives it via DX12 -- FSR3 FG
	// shares the D3D12 proxy but never emits DX12 Reflex markers.
	if (renderAPI == sl::RenderAPI::eD3D11 && upscaling.UsesDLSSGFrameGen()) {
		sl::ReflexOptions disabledOptions{};
		disabledOptions.mode = sl::ReflexMode::eOff;
		disabledOptions.frameLimitUs = 0u;
		disabledOptions.useMarkersToOptimize = false;
		applyReflexOptionsIfChanged(disabledOptions, "Failed to disable Reflex while DLSS-G frame-generation is active");
		return;
	}

	auto& settings = globals::features::upscaling.settings;

	sl::ReflexOptions options{};
	if (renderAPI == sl::RenderAPI::eD3D12) {
		// DX12 Reflex: DLSS-G requires at least eLowLatency when FG is active
		bool needReflex = upscaling.ShouldUseFrameGenerationThisFrame() || settings.reflexLowLatencyMode;
		if (needReflex)
			options.mode = settings.reflexLowLatencyBoost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
		else
			options.mode = sl::ReflexMode::eOff;
	} else {
		if (!settings.reflexLowLatencyMode)
			options.mode = sl::ReflexMode::eOff;
		else
			options.mode = settings.reflexLowLatencyBoost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
	}

	const float originalReflexFPSLimit = settings.reflexFPSLimit;
	float reflexFPSLimit = originalReflexFPSLimit;
	if (!std::isfinite(reflexFPSLimit)) {
		reflexFPSLimit = 60.0f;
		settings.reflexFPSLimit = reflexFPSLimit;
		logger::warn("[Streamline {}] reflexFPSLimit is not finite ({}), using {}", instanceTag, originalReflexFPSLimit, reflexFPSLimit);
	}
	const float fpsLimit = std::clamp(reflexFPSLimit, 20.0f, 240.0f);
	options.frameLimitUs = settings.reflexUseFPSLimit ? static_cast<uint32_t>(std::lround(1000000.0 / static_cast<double>(fpsLimit))) : 0u;
	options.useMarkersToOptimize = settings.reflexUseMarkersToOptimize && featurePCL;

	applyReflexOptionsIfChanged(options, "Failed to apply Reflex options");

	if (!slReflexSleep)
		return;

	if (options.mode == sl::ReflexMode::eOff && options.frameLimitUs == 0)
		return;

	const uint32_t currentFrame = globals::state ? globals::state->frameCount : 0;
	if (lastReflexSleepFrame == currentFrame)
		return;

	if (!EnsureFrameToken())
		return;

	lastReflexSleepFrame = currentFrame;
	if (SL_FAILED(result, slReflexSleep(*frameToken))) {
		logger::warn("[Streamline {}] Reflex sleep call failed: {}", instanceTag, magic_enum::enum_name(result));
	}

	// The frame's simulation begins right after the Reflex sleep returns; the matching
	// eSimulationEnd (and the render/present markers) are emitted on the present path.
	if (renderAPI == sl::RenderAPI::eD3D12)
		EmitPCLMarker(sl::PCLMarker::eSimulationStart);
}

void Streamline::EmitPCLMarker(sl::PCLMarker a_marker)
{
	if (!initialized || !featurePCL || !slPCLSetMarker)
		return;
	if (!EnsureFrameToken())
		return;

	if (SL_FAILED(result, slPCLSetMarker(a_marker, *frameToken))) {
		static bool errorLogged = false;
		if (!errorLogged) {
			errorLogged = true;
			logger::warn("[Streamline {}] slPCLSetMarker({}) failed: {}", instanceTag,
				magic_enum::enum_name(a_marker), magic_enum::enum_name(result));
		}
	}
}

void Streamline::ConfigureDLSSG(bool enabled)
{
	if (!initialized || !slDLSSGSetOptions)
		return;

	sl::DLSSGOptions options{};
	options.mode = enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
	options.numFramesToGenerate = std::clamp<uint32_t>(
		globals::features::upscaling.settings.dlssgFramesToGenerate, 1, dlssgMaxFramesToGenerate);

	if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
		static bool errorLogged = false;
		if (!errorLogged) {
			errorLogged = true;
			logger::error("[Streamline DX12] slDLSSGSetOptions failed: {}", magic_enum::enum_name(result));
		}
	}

	if (slDLSSGGetState && enabled) {
		sl::DLSSGState state{};
		if (SL_FAILED(stateResult, slDLSSGGetState(viewport, state, &options))) {
			static uint32_t stateFailCount = 0;
			if (++stateFailCount % 60 == 0) {
				logger::warn("[Streamline DX12] slDLSSGGetState has failed {} times: {}", stateFailCount, magic_enum::enum_name(stateResult));
			}
		} else {
			lastDLSSGStatus = state.status;
			lastDLSSGFramesPresented = state.numFramesActuallyPresented;

			static sl::DLSSGStatus lastLoggedStatus = sl::DLSSGStatus::eOk;
			if (state.status != sl::DLSSGStatus::eOk && state.status != lastLoggedStatus) {
				lastLoggedStatus = state.status;
				logger::warn("[Streamline DX12] DLSS-G not generating frames this session: status={}",
					magic_enum::enum_name(state.status));
			} else if (state.status == sl::DLSSGStatus::eOk && lastLoggedStatus != sl::DLSSGStatus::eOk) {
				lastLoggedStatus = sl::DLSSGStatus::eOk;
				logger::info("[Streamline DX12] DLSS-G status recovered to eOk, now generating frames");
			}

			// status eOk does not confirm interpolation; numFramesActuallyPresented does.
			if (globals::features::upscaling.settings.streamlineLogLevel >= 2) {
				static uint32_t callCount = 0;
				if (++callCount % 120 == 0) {
					logger::info("[Streamline DX12] DLSS-G presented {} frames since last query (requested {}x)",
						state.numFramesActuallyPresented, options.numFramesToGenerate + 1);
				}
			}
		}
	}
}

void Streamline::TagDX12Resources(ID3D12GraphicsCommandList* cmdList,
	ID3D12Resource* depth, ID3D12Resource* mvec, ID3D12Resource* hudLessColor,
	ID3D12Resource* uiColorAndAlpha, uint32_t width, uint32_t height)
{
	if (!initialized || !slSetTagForFrame || !frameToken || !cmdList)
		return;

	sl::Extent extent{ 0, 0, width, height };

	// Depth/mvec carry valid data only in the render-resolution subrect of their
	// display-sized targets; declare that extent, matching the FSR FG dispatch.
	auto renderSizeF = globals::state->screenSize * globals::features::upscaling.resolutionScale;
	sl::Extent renderExtent{ 0, 0, static_cast<uint32_t>(renderSizeF.x), static_cast<uint32_t>(renderSizeF.y) };

	sl::Resource depthRes = { sl::ResourceType::eTex2d, depth, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource mvecRes = { sl::ResourceType::eTex2d, mvec, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource hudLessRes = { sl::ResourceType::eTex2d, hudLessColor, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource uiRes = { sl::ResourceType::eTex2d, uiColorAndAlpha, D3D12_RESOURCE_STATE_COMMON };

	// The UI alpha tag lets DLSS-G composite the HUD onto generated frames instead of
	// interpolating HUD pixels as scene motion. It must stay the LAST array entry so the
	// tag count below can drop it when no UI buffer exists this frame.
	sl::ResourceTag tags[] = {
		{ &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
		{ &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
		{ &hudLessRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extent },
		{ &uiRes, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &extent },
	};
	const uint32_t numTags = uiColorAndAlpha ? _countof(tags) : _countof(tags) - 1;

	slSetTagForFrame(*frameToken, viewport, tags, numTags, cmdList);
}

/**
 * @brief Releases DLSS resources and disables DLSS for the current viewport.
 *
 * Sets the DLSS mode to off and frees all DLSS-related resources associated with the viewport.
 */
void Streamline::DestroyDLSSResources()
{
	// DLSS entry points are only resolved when DLSS is available; calling them otherwise faults.
	if (!featureDLSS)
		return;

	sl::DLSSOptions dlssOptions{};
	dlssOptions.mode = sl::DLSSMode::eOff;

	slDLSSSetOptions(viewport, dlssOptions);
	slFreeResources(sl::kFeatureDLSS, viewport);

	if (globals::game::isVR) {
		slDLSSSetOptions(viewportRight, dlssOptions);
		slFreeResources(sl::kFeatureDLSS, viewportRight);
	}
}
