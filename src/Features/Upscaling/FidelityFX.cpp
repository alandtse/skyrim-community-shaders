#include "FidelityFX.h"

#include <directx/d3dx12.h>

#include "../../State.h"
#include "../../Utils/FileSystem.h"
#include "../HDRDisplay.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"

ffxFunctions ffxModule;

std::vector<std::pair<std::string, std::string>> FidelityFX::dllVersions = {};

namespace
{
	DLL_DIRECTORY_COOKIE s_fidelityFxDllDirectoryCookie = nullptr;

	std::string GetFidelityFxPathText(const std::filesystem::path& a_path)
	{
		return stl::utf16_to_utf8(a_path.wstring()).value_or("<unprintable path>");
	}

	void EnsureFidelityFxDllDirectory(const std::filesystem::path& a_pluginDir)
	{
		if (s_fidelityFxDllDirectoryCookie)
			return;

		auto kernel32 = GetModuleHandleW(L"kernel32.dll");
		if (!kernel32) {
			const auto error = GetLastError();
			logger::warn("[FidelityFX] Failed to access kernel32 while registering '{}' (Win32 error {})",
				GetFidelityFxPathText(a_pluginDir), error);
			return;
		}

		using AddDllDirectoryFn = DLL_DIRECTORY_COOKIE(WINAPI*)(PCWSTR);
		auto addDllDirectory = reinterpret_cast<AddDllDirectoryFn>(GetProcAddress(kernel32, "AddDllDirectory"));
		if (!addDllDirectory) {
			const auto error = GetLastError();
			logger::warn("[FidelityFX] AddDllDirectory is unavailable for '{}' (Win32 error {})",
				GetFidelityFxPathText(a_pluginDir), error);
			return;
		}

		s_fidelityFxDllDirectoryCookie = addDllDirectory(a_pluginDir.c_str());
		if (!s_fidelityFxDllDirectoryCookie) {
			const auto error = GetLastError();
			logger::warn("[FidelityFX] Failed to register DLL directory '{}' (Win32 error {})",
				GetFidelityFxPathText(a_pluginDir), error);
		}
	}

	HMODULE LoadFidelityFxDll(const std::filesystem::path& a_path, DWORD& a_error)
	{
		constexpr DWORD kLoadFlags =
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
			LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
			LOAD_LIBRARY_SEARCH_USER_DIRS;

		a_error = ERROR_SUCCESS;
		auto loadedModule = LoadLibraryExW(a_path.c_str(), nullptr, kLoadFlags);
		if (!loadedModule)
			a_error = GetLastError();
		return loadedModule;
	}

	bool FidelityFxDllExists(const std::filesystem::path& a_path, std::error_code& a_error)
	{
		a_error.clear();
		return std::filesystem::is_regular_file(a_path, a_error);
	}
}

void FidelityFX::LoadFFX()
{
	featureFSR3FG = false;
	featureRuntimeUpscaler = false;

	const auto pluginDir = (Util::PathHelpers::GetDataPath() / "Shaders" / "Upscaling" / "FidelityFX").lexically_normal();
	if (!pluginDir.is_absolute()) {
		logger::error("[FidelityFX] Refusing non-absolute DLL directory '{}'", GetFidelityFxPathText(pluginDir));
		return;
	}

	EnsureFidelityFxDllDirectory(pluginDir);

	const auto loaderPath = pluginDir / "amd_fidelityfx_loader_dx12.dll";
	const auto frameGenerationPath = pluginDir / "amd_fidelityfx_framegeneration_dx12.dll";
	const auto runtimeUpscalerPath = pluginDir / RuntimeUpscalerDllName;

	FidelityFX::dllVersions = Util::EnumerateDllVersions(pluginDir);
	for (const auto& [name, versionStr] : FidelityFX::dllVersions)
		logger::info("[FidelityFX] {} version: {}", name, versionStr);

	const auto loadDll = [](const char* a_label, const std::filesystem::path& a_path, HMODULE& a_module) {
		if (a_module) {
			logger::info("[FidelityFX] {} loaded from '{}'", a_label, GetFidelityFxPathText(a_path));
			return true;
		}

		std::error_code fileError;
		if (!FidelityFxDllExists(a_path, fileError)) {
			if (fileError) {
				logger::error("[FidelityFX] Failed to inspect {} at '{}': {}", a_label,
					GetFidelityFxPathText(a_path), fileError.message());
			} else {
				logger::warn("[FidelityFX] {} is missing at '{}'", a_label, GetFidelityFxPathText(a_path));
			}
			return false;
		}

		DWORD loadError = ERROR_SUCCESS;
		a_module = LoadFidelityFxDll(a_path, loadError);
		if (!a_module) {
			logger::error("[FidelityFX] {} exists but failed to load from '{}' (Win32 error {})",
				a_label, GetFidelityFxPathText(a_path), loadError);
			return false;
		}

		logger::info("[FidelityFX] {} loaded from '{}'", a_label, GetFidelityFxPathText(a_path));
		return true;
	};

	const bool loaderLoaded = loadDll("Loader DLL", loaderPath, module);
	if (loaderLoaded) {
		ffxModule = {};
		ffxLoadFunctions(&ffxModule, module);
	}

	const bool loaderReady = loaderLoaded &&
	                         ffxModule.CreateContext &&
	                         ffxModule.DestroyContext &&
	                         ffxModule.Configure &&
	                         ffxModule.Query &&
	                         ffxModule.Dispatch;
	if (loaderLoaded && !loaderReady)
		logger::error("[FidelityFX] Loader DLL is missing one or more required API exports at '{}'", GetFidelityFxPathText(loaderPath));

	if (!loaderReady) {
		const auto reportSkippedDll = [](const char* a_label, const std::filesystem::path& a_path) {
			std::error_code fileError;
			if (FidelityFxDllExists(a_path, fileError)) {
				logger::warn("[FidelityFX] {} exists at '{}' but was not loaded because the loader is unavailable",
					a_label, GetFidelityFxPathText(a_path));
			} else if (fileError) {
				logger::error("[FidelityFX] Failed to inspect {} at '{}': {}", a_label,
					GetFidelityFxPathText(a_path), fileError.message());
			} else {
				logger::warn("[FidelityFX] {} is missing at '{}'", a_label, GetFidelityFxPathText(a_path));
			}
		};

		reportSkippedDll("Frame generation DLL", frameGenerationPath);
		reportSkippedDll("Runtime upscaler DLL", runtimeUpscalerPath);
		return;
	}

	featureFSR3FG = loadDll("Frame generation DLL", frameGenerationPath, frameGenerationModule);
	featureRuntimeUpscaler = loadDll("Runtime upscaler DLL", runtimeUpscalerPath, runtimeUpscalerModule);
}

void FidelityFX::SetupFrameGeneration()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	ffx::CreateContextDescFrameGeneration createFg{};
	createFg.displaySize = { swapChain.swapChainDesc.Width, swapChain.swapChainDesc.Height };
	createFg.maxRenderSize = createFg.displaySize;
	createFg.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
	createFg.backBufferFormat = ffxApiGetSurfaceFormatDX12(swapChain.swapChainDesc.Format);

	ffx::CreateBackendDX12Desc backendDesc{};
	backendDesc.device = swapChain.d3d12Device.get();

	if (ffx::CreateContext(frameGenContext, nullptr, createFg, backendDesc) != ffx::ReturnCode::Ok)
		logger::critical("[FidelityFX] Failed to create frame generation context!");
}

/**
 * @brief Presents the current frame, optionally performing frame generation using FidelityFX.
 *
 * Configures and dispatches FidelityFX frame generation for the current swap chain frame if enabled. Sets up frame pacing, prepares resources, and issues dispatches for both frame generation parameters and camera information. Increments the internal frame ID after each call.
 *
 * @param a_useFrameGeneration If true, enables frame generation and dispatches the necessary workloads; otherwise, presents without frame generation.
 */
void FidelityFX::Present(bool a_useFrameGeneration, bool a_isHDR)
{
	auto& upscaling = globals::features::upscaling;
	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	// Cache peak nits first since we need HDR feature access
	auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
	float peakNits = hdr ? static_cast<float>(hdr->settings.hdrPeakNits) : 1000.0f;

	// Clamp peak nits to safe range [1.0f, 10000.0f] to prevent invalid values
	peakNits = std::clamp(peakNits, 1.0f, 10000.0f);

	// Detect if HDR parameters changed - if so, we need to reset FG history
	// because frames in the history were encoded with different parameters
	bool hdrParamsChanged = (a_isHDR != prevHDRActive) ||
	                        (a_isHDR && std::abs(peakNits - prevPeakNits) > 1.0f);

	// Update tracking for next frame
	prevHDRActive = a_isHDR;
	prevPeakNits = peakNits;

	// Store HDR state atomically for the callback to access (may be read from async thread)
	// Use seq_cst for both to ensure the callback sees both values consistently
	hdrPeakNits.store(peakNits, std::memory_order_seq_cst);
	isHDRActive.store(a_isHDR, std::memory_order_seq_cst);
	needsReset.store(hdrParamsChanged, std::memory_order_seq_cst);

	ffx::ConfigureDescFrameGeneration configParameters{};

	if (a_useFrameGeneration) {
		configParameters.frameGenerationEnabled = true;

		configParameters.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* pUserCtx) -> ffxReturnCode_t {
			// Tell FidelityFX the color space so it can properly interpolate. Fixes pixel smearing that occured with HDR on.
			// PQ requires decoding to linear for correct motion interpolation
			// Read atomically with seq_cst since this callback may run on async thread
			bool hdrActive = FidelityFX::isHDRActive.load(std::memory_order_seq_cst);
			if (hdrActive) {
				params->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
				// Set luminance range for PQ decoding (0 to peak nits)
				params->minMaxLuminance[0] = 0.0f;
				params->minMaxLuminance[1] = FidelityFX::hdrPeakNits.load(std::memory_order_seq_cst);
			} else {
				params->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
			}
			// Force reset when HDR parameters changed to clear internal buffers
			if (FidelityFX::needsReset.exchange(false, std::memory_order_seq_cst)) {
				params->reset = true;
			}
			return ffxModule.Dispatch(reinterpret_cast<ffxContext*>(pUserCtx), &params->header);
		};

		configParameters.frameGenerationCallbackUserContext = &frameGenContext;

	} else {
		configParameters.frameGenerationEnabled = false;

		configParameters.frameGenerationCallbackUserContext = nullptr;
		configParameters.frameGenerationCallback = nullptr;
	}

	configParameters.HUDLessColor = FfxApiResource({});

	configParameters.presentCallback = nullptr;
	configParameters.presentCallbackUserContext = nullptr;

	static uint64_t frameID = 0;

	// If HDR parameters changed, skip a frame ID to force FidelityFX to reset its history
	// This prevents interpolation artifacts when frames were encoded with different parameters
	// Per FidelityFX docs: "Any non-exactly-one difference will reset the frame generation logic"
	if (hdrParamsChanged && a_useFrameGeneration) {
		frameID += 2;  // Skip one ID to trigger reset
	}

	configParameters.frameID = frameID;
	configParameters.swapChain = swapChain.swapChain;
	configParameters.onlyPresentGenerated = false;
	configParameters.flags = 0;
	configParameters.allowAsyncWorkloads = true;

	auto state = globals::state;

	auto renderSize = state->screenSize * upscaling.resolutionScale;

	configParameters.generationRect.left = (swapChain.swapChainDesc.Width - swapChain.swapChainDesc.Width) / 2;
	configParameters.generationRect.top = (swapChain.swapChainDesc.Height - swapChain.swapChainDesc.Height) / 2;
	configParameters.generationRect.width = swapChain.swapChainDesc.Width;
	configParameters.generationRect.height = swapChain.swapChainDesc.Height;

	if (ffx::Configure(frameGenContext, configParameters) != ffx::ReturnCode::Ok) {
		logger::critical("[FidelityFX] Failed to configure frame generation!");
	}

	// Register UI buffer with FidelityFX only when FG is active
	// When paused, UI is composited in HDROutputCS to avoid flickering from inconsistent FidelityFX compositing
	ffx::ConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiConfig{};
	if (a_useFrameGeneration) {
		uiConfig.uiResource = ffxApiGetResourceDX12(swapChain.uiBufferWrapped->resource.get());
		// Use both premultiplied alpha and double buffering for consistent blending
		uiConfig.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
		                 FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
	} else {
		// No UI resource when FG is disabled - backbuffer already has UI composited
		uiConfig.uiResource = FfxApiResource({});
		uiConfig.flags = 0;
	}

	if (ffx::Configure(swapChainContext, uiConfig) != ffx::ReturnCode::Ok) {
		logger::critical("[FidelityFX] Failed to configure UI composition!");
	}

	if (a_useFrameGeneration) {
		ffx::DispatchDescFrameGenerationPrepare dispatchParameters{};

		dispatchParameters.commandList = swapChain.commandLists[swapChain.frameIndex].get();

		dispatchParameters.motionVectorScale.x = renderSize.x;
		dispatchParameters.motionVectorScale.y = renderSize.y;
		dispatchParameters.renderSize.width = static_cast<uint32_t>(renderSize.x);
		dispatchParameters.renderSize.height = static_cast<uint32_t>(renderSize.y);

		dispatchParameters.jitterOffset.x = -upscaling.jitter.x;
		dispatchParameters.jitterOffset.y = -upscaling.jitter.y;

		dispatchParameters.frameTimeDelta = RE::GetSecondsSinceLastFrame() * 1000.f;

		dispatchParameters.cameraFar = *globals::game::cameraFar;
		dispatchParameters.cameraNear = *globals::game::cameraNear;

		dispatchParameters.cameraFovAngleVertical = Util::GetVerticalFOVRad();
		dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;

		dispatchParameters.frameID = frameID;

		dispatchParameters.depth = ffxApiGetResourceDX12(swapChain.depthBufferShared12->resource.get());
		dispatchParameters.motionVectors = ffxApiGetResourceDX12(swapChain.motionVectorBufferShared12->resource.get());

		ffx::DispatchDescFrameGenerationPrepareCameraInfo cameraConfig{};

		auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse().Transpose();
		auto cameraViewToClip = globals::game::frameBufferCached.GetCameraProjUnjittered().Transpose();

		cameraConfig.cameraRight[0] = viewMatrix._11;
		cameraConfig.cameraRight[1] = viewMatrix._12;
		cameraConfig.cameraRight[2] = viewMatrix._13;

		cameraConfig.cameraUp[0] = viewMatrix._21;
		cameraConfig.cameraUp[1] = viewMatrix._22;
		cameraConfig.cameraUp[2] = viewMatrix._23;

		cameraConfig.cameraForward[0] = viewMatrix._31;
		cameraConfig.cameraForward[1] = viewMatrix._32;
		cameraConfig.cameraForward[2] = viewMatrix._33;

		cameraConfig.cameraPosition[0] = globals::game::frameBufferCached.GetCameraPosAdjust().x;
		cameraConfig.cameraPosition[1] = globals::game::frameBufferCached.GetCameraPosAdjust().y;
		cameraConfig.cameraPosition[2] = globals::game::frameBufferCached.GetCameraPosAdjust().z;

		if (ffx::Dispatch(frameGenContext, dispatchParameters, cameraConfig) != ffx::ReturnCode::Ok) {
			logger::critical("[FidelityFX] Failed to dispatch frame generation!");
		}
	}

	frameID++;

	// Set isFrameGenActive based on whether FSR3 frame generation is enabled
	isFrameGenActive = a_useFrameGeneration;
}

void FidelityFX::CreateFSRResources()
{
	auto state = globals::state;

	// Tear down any live runtime-upscaler contexts/resources so a resolution/mode change
	// doesn't leave them sized for the previous configuration (see RuntimeUpscaler.cpp).
	WaitForRuntimeUpscalerIdle();
	DestroyRuntimeUpscalerContexts(false);
	DestroyRuntimeUpscalerResources(false);
	ResetRuntimeUpscalerTracking(true);

	// Prevent multiple allocations
	if (fsrScratchBuffer) {
		logger::warn("[FidelityFX] FSR resources already created, skipping allocation");
		return;
	}

	auto fsrDevice = ffxGetDeviceDX11_Fsr31(globals::d3d::device);

	uint32_t numContexts = globals::game::isVR ? 2 : 1;
	size_t scratchBufferSize = ffxGetScratchMemorySizeDX11(numContexts);
	fsrScratchBuffer = calloc(scratchBufferSize, 1);
	if (!fsrScratchBuffer) {
		logger::critical("[FidelityFX] Failed to allocate FSR3 scratch buffer memory!");
		return;
	}
	memset(fsrScratchBuffer, 0, scratchBufferSize);

	FfxInterface fsrInterface;
	if (ffxGetInterfaceDX11(&fsrInterface, fsrDevice, fsrScratchBuffer, scratchBufferSize, numContexts) != FFX_OK) {
		logger::critical("[FidelityFX] Failed to initialize FSR3 backend interface!");
		free(fsrScratchBuffer);
		fsrScratchBuffer = nullptr;
		return;
	}

	auto screenSize = state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	// Submit-stage output dimensions are independent of the engine render targets.
	auto& vrSubmit = globals::features::upscaling.vrSubmit;
	const bool dlssperfActive = vrSubmit.IsHookActive();
	const auto displaySize = dlssperfActive ? vrSubmit.GetDisplayScreenSize() : screenSize;

	uint32_t displayWidth = (uint32_t)(globals::game::isVR ? displaySize.x / 2 : displaySize.x);
	uint32_t displayHeight = (uint32_t)displaySize.y;
	uint32_t renderWidth = (uint32_t)(globals::game::isVR ? renderSize.x / 2 : renderSize.x);
	uint32_t renderHeight = (uint32_t)renderSize.y;

	for (uint32_t i = 0; i < numContexts; ++i) {
		FfxFsr3ContextDescription contextDescription;
		contextDescription.maxRenderSize.width = renderWidth;
		contextDescription.maxRenderSize.height = renderHeight;
		contextDescription.maxUpscaleSize.width = displayWidth;
		contextDescription.maxUpscaleSize.height = displayHeight;
		contextDescription.displaySize.width = displayWidth;
		contextDescription.displaySize.height = displayHeight;
		contextDescription.flags = FFX_FSR3_ENABLE_UPSCALING_ONLY | FFX_FSR3_ENABLE_AUTO_EXPOSURE;
		if (globals::features::hdrDisplay.loaded || globals::features::upscaling.vrSubmit.IsHookActive()) {
			contextDescription.flags |= FFX_FSR3_ENABLE_HIGH_DYNAMIC_RANGE;
			contextDescription.backBufferFormat = FFX_SURFACE_FORMAT_R10G10B10A2_UNORM;
		} else {
			contextDescription.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
		}
		contextDescription.backendInterfaceUpscaling = fsrInterface;

		if (ffxFsr3ContextCreate(&fsrContext[i], &contextDescription) != FFX_OK) {
			logger::critical("[FidelityFX] Failed to initialize FSR3 context for eye {}!", i);
			for (uint32_t j = 0; j < i; ++j)
				ffxFsr3ContextDestroy(&fsrContext[j]);
			free(fsrScratchBuffer);
			fsrScratchBuffer = nullptr;
			return;
		}
	}
	logger::info("[FidelityFX] Created {} FSR3 contexts (Display: {}x{}, Render: {}x{})",
		numContexts, displayWidth, displayHeight, renderWidth, renderHeight);
}

void FidelityFX::DestroyFSRResources()
{
	WaitForHostFsrIdle();
	ResetFSRIdleFence();

	uint32_t numContexts = globals::game::isVR ? 2 : 1;
	for (uint32_t i = 0; i < numContexts; ++i) {
		if (ffxFsr3ContextDestroy(&fsrContext[i]) != FFX_OK)
			logger::critical("[FidelityFX] Failed to destroy FSR3 context for eye {}!", i);
	}

	// Free the scratch buffer to prevent memory leak
	if (fsrScratchBuffer) {
		free(fsrScratchBuffer);
		fsrScratchBuffer = nullptr;
	}

	// Reset crash logging flag when resources are destroyed
	fsrDispatchCrashLogged = false;

	WaitForRuntimeUpscalerIdle();
	DestroyRuntimeUpscalerContexts(false);
	DestroyRuntimeUpscalerResources(false);
	ResetRuntimeCommandContexts();
	ResetRuntimeUpscalerTracking(true);
}

FfxResource ffxGetResource(ID3D11Resource* dx11Resource,
	[[maybe_unused]] wchar_t const* ffxResName,
	FfxResourceStates state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ)
{
	FfxResource resource = {};
	resource.resource = reinterpret_cast<void*>(const_cast<ID3D11Resource*>(dx11Resource));
	resource.state = state;
	resource.description = GetFfxResourceDescriptionDX11(dx11Resource);

#ifdef _DEBUG
	if (ffxResName) {
		wcscpy_s(resource.name, ffxResName);
	}
#endif

	return resource;
}

void FidelityFX::Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_depth, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors, float a_sharpness, ID3D11Resource* a_colorOut)
{
	auto state = globals::state;

	auto screenSize = state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	auto& upscaling = globals::features::upscaling;
	auto jitter = upscaling.jitter;

	// Submit output dimensions must match the display size used to create FSR contexts.
	auto& vrSubmit = upscaling.vrSubmit;
	const bool dlssperfActive = vrSubmit.IsHookActive();
	const auto displaySize = dlssperfActive ? vrSubmit.GetDisplayScreenSize() : screenSize;

	// Default to in-place output when caller didn't supply a separate destination.
	if (!a_colorOut)
		a_colorOut = a_upscalingTexture;

	auto DispatchFSR = [&](uint32_t contextIndex, ID3D11Resource* r_color, ID3D11Resource* r_depth, ID3D11Resource* r_mvec,
						   ID3D11Resource* r_reactive, ID3D11Resource* r_trans, ID3D11Resource* r_output,
						   uint32_t r_width, float mv_scale_x) {
		if (state->frameAnnotations) {
			if (globals::game::isVR) {
				char buf[32];
				snprintf(buf, sizeof(buf), "FSR Dispatch Eye %u", contextIndex);
				state->BeginPerfEvent(buf);
			} else {
				state->BeginPerfEvent("FSR Dispatch");
			}
		}

		const uint32_t displayWidth = (uint32_t)(globals::game::isVR ? displaySize.x / 2 : displaySize.x);
		const uint32_t displayHeight = (uint32_t)displaySize.y;

		const bool dispatched = UpscaleRegion(contextIndex, r_color, r_depth, r_mvec, r_reactive, r_trans, r_output,
			r_width, (uint32_t)renderSize.y, displayWidth, displayHeight,
			mv_scale_x, renderSize.y, a_sharpness);
		if (!dispatched)
			logger::critical("[FidelityFX] Failed to dispatch upscaling for eye {}!", contextIndex);

		if (state->frameAnnotations)
			state->EndPerfEvent();

		return dispatched;
	};

	if (globals::game::isVR) {
		// Prepare per-eye inputs and clear mask
		upscaling.PreparePerEyeInputs(a_upscalingTexture);

		uint32_t numViews = 2;
		uint32_t eyeWidth = (uint32_t)(renderSize.x / 2);
		bool allEvaluated = true;
		for (uint32_t i = 0; i < numViews; ++i) {
			if (!DispatchFSR(i,
					upscaling.vrIntermediateColorIn[i]->resource.get(),
					upscaling.vrIntermediateLinearDepth[i]->resource.get(),
					upscaling.vrIntermediateMotionVectors[i]->resource.get(),
					upscaling.vrIntermediateReactiveMask[i]->resource.get(),
					upscaling.vrIntermediateTransparencyMask[i]->resource.get(),
					upscaling.vrIntermediateColorOut[i]->resource.get(),
					eyeWidth,
					renderSize.x / 2.0f))
				allEvaluated = false;
		}

		// A failed eye must not publish unwritten or stale stereo output.
		if (allEvaluated)
			upscaling.FinalizePerEyeOutputs(a_colorOut);
	} else {
		DispatchFSR(0,
			a_upscalingTexture,
			a_depth,
			a_motionVectors,
			a_reactiveMask,
			a_transparencyCompositionMask,
			a_colorOut,
			(uint)renderSize.x,
			renderSize.x);
	}
}
