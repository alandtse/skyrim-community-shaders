#pragma once

#include "../../Buffer.h"
#include "../../State.h"

#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>

#define NV_WINDOWS

#pragma warning(push)
#pragma warning(disable: 4471)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_reflex.h>
#include <sl_version.h>
#pragma warning(pop)

/** @brief Manages NVIDIA Streamline integration for DLSS upscaling and Reflex latency reduction. */
class Streamline
{
public:
	Streamline() = default;

	/** @brief Returns the short identifier used for logging. */
	inline std::string GetShortName() { return "Streamline"; }

	static constexpr UINT kNvidiaVendorId = 0x10DE;

	// Configure before calling LoadInterposer(). DX12 instance uses a separate plugin
	// directory and interposer DLL so the two SDK states are fully independent per-process.
	sl::RenderAPI renderAPI = sl::RenderAPI::eD3D11;
	std::wstring pluginDir = L"Data\\Shaders\\Upscaling\\Streamline";
	std::wstring interposerDllName = L"sl.interposer.dll";
	std::string instanceTag = "DX11";

	bool initialized = false;
	bool triedInitialization = false;

	bool featureDLSS = false;
	bool featureDLSSG = false;
	// Upper bound for DLSSGOptions::numFramesToGenerate, queried once via slDLSSGGetState
	// after PostDevice binds the DLSS-G functions (DX12 instance only). 1 = 2x-only.
	uint32_t dlssgMaxFramesToGenerate = 1;
	// Last slDLSSGGetState results, cached for GetDiagnostics (querying there would
	// steal the since-last-query frame counter from the present path).
	sl::DLSSGStatus lastDLSSGStatus = sl::DLSSGStatus::eOk;
	uint32_t lastDLSSGFramesPresented = 0;
	bool featureReflex = false;
	bool featurePCL = false;
	bool reflexSupportedOnCurrentAdapter = false;

	sl::ViewportHandle viewport{ 0 };
	sl::ViewportHandle viewportRight{ 1 };
	static constexpr uint32_t MAX_RESOLUTION = 8192;
	HMODULE interposer = NULL;

	// SL Interposer Functions
	PFun_slInit* slInit{};
	PFun_slShutdown* slShutdown{};
	PFun_slIsFeatureSupported* slIsFeatureSupported{};
	PFun_slIsFeatureLoaded* slIsFeatureLoaded{};
	PFun_slSetFeatureLoaded* slSetFeatureLoaded{};
	PFun_slEvaluateFeature* slEvaluateFeature{};
	PFun_slAllocateResources* slAllocateResources{};
	PFun_slFreeResources* slFreeResources{};
	PFun_slSetTag* slSetTag{};
	PFun_slSetTagForFrame* slSetTagForFrame{};
	PFun_slGetFeatureRequirements* slGetFeatureRequirements{};
	PFun_slGetFeatureVersion* slGetFeatureVersion{};
	PFun_slUpgradeInterface* slUpgradeInterface{};
	PFun_slSetConstants* slSetConstants{};
	PFun_slGetNativeInterface* slGetNativeInterface{};
	PFun_slGetFeatureFunction* slGetFeatureFunction{};
	PFun_slGetNewFrameToken* slGetNewFrameToken{};
	PFun_slSetD3DDevice* slSetD3DDevice{};

	// DLSS specific functions (DX11 instance)
	PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings{};
	PFun_slDLSSGetState* slDLSSGetState{};
	PFun_slDLSSSetOptions* slDLSSSetOptions{};

	// DLSS-G specific functions (DX12 instance)
	PFun_slDLSSGGetState* slDLSSGGetState{};
	PFun_slDLSSGSetOptions* slDLSSGSetOptions{};

	// Reflex specific functions (DX11 instance)
	PFun_slReflexGetState* slReflexGetState{};
	PFun_slReflexSleep* slReflexSleep{};
	PFun_slReflexSetOptions* slReflexSetOptions{};
	PFun_slPCLSetMarker* slPCLSetMarker{};

	Util::FrameChecker frameChecker;
	sl::FrameToken* frameToken = nullptr;

	struct ReflexOptionsCache
	{
		bool valid = false;
		sl::ReflexMode mode = sl::ReflexMode::eOff;
		uint32_t frameLimitUs = 0;
		bool useMarkersToOptimize = false;
	};
	ReflexOptionsCache reflexOptionsCache{};
	uint32_t lastReflexSleepFrame = UINT32_MAX;

	// Helper: Execute DLSS for a single viewport with given resources.
	// outputHeight defaults to 0 -> SetDLSSOptions uses full per-eye DisplayRes height
	// (matches the standard upscale path where every eval is full eye). FoveatedRender's
	// subrect path must pass the actual subrect height so DLSS isn't configured for
	// `subOutW x eyeHeightOut` while extentOut says `subOutW x subOutH` - that mismatch
	// makes NGX return zeroed output and the subrect region renders black.
	// Returns false if CheckFrameConstants or slEvaluateFeature failed -- callers
	// on the foveated route use this to fall back instead of reporting success.
	bool EvaluateDLSS(sl::ViewportHandle vp, uint32_t eyeIndex,
		ID3D11Resource* colorIn, ID3D11Resource* colorOut, ID3D11Resource* depth,
		ID3D11Resource* mvec, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask,
		const sl::Extent& extentIn, const sl::Extent& extentOut, uint32_t outputWidth,
		uint32_t outputHeight = 0, const sl::Constants* frameConstants = nullptr);

	// Cached DLL version info for Streamline plugin directory
	static std::vector<std::pair<std::string, std::string>> dllVersions;

	/** @brief Loads the Streamline interposer DLL and initializes the SDK with feature preferences. */
	void LoadInterposer();

	/**
	 * @brief Queries available Streamline features (DLSS, Reflex, PCL) on the given adapter.
	 * @param a_adapter The DXGI adapter to check feature support against.
	 */
	void CheckFeatures(IDXGIAdapter* a_adapter);

	// Bind a DX12 device to this Streamline instance (DX12 instance only).
	void SetD3DDevice12(ID3D12Device* a_device);

	/**
	 * @brief Resets the driver-profile DRS key that silently disables DLSS-G for this
	 * executable (status stays eOk with zero interpolated frames when it is set). Must
	 * run at plugin load: the driver latches the profile early in process life, so a
	 * reset after device creation only takes effect on the next launch.
	 */
	static void EnsureDriverProfileAllowsDLSSG();

	/** @brief Binds DLSS and Reflex feature functions after the D3D device is created. */
	void PostDevice();

	/** @brief Resolves one feature function pointer, logging on failure. */
	bool BindFeatureFunction(sl::Feature a_feature, const char* a_functionName, void*& a_function);
	/** @brief Requests a feature be marked loaded, logging on failure. */
	void RequestFeatureLoad(sl::Feature a_feature, const char* a_featureName);
	/** @brief Binds Reflex/PCL functions and updates their availability flags. */
	void BindReflexAndPCL();

	// DLSS-G frame generation methods (DX12 instance only)
	void ConfigureDLSSG(bool enabled);
	/**
	 * @brief Emits a PCL latency marker for the current frame token. The marker's frame
	 * index is how DLSS-G's pacer matches presents to constants -- structural for FG.
	 */
	void EmitPCLMarker(sl::PCLMarker a_marker);
	void TagDX12Resources(ID3D12GraphicsCommandList* cmdList,
		ID3D12Resource* depth, ID3D12Resource* mvec, ID3D12Resource* hudLessColor,
		ID3D12Resource* uiColorAndAlpha, uint32_t width, uint32_t height);

	/** @brief Acquires a new frame token from Streamline for the current frame. */
	bool EnsureFrameToken();
	/** @brief Captures camera constants into snapshot, or publishes them when snapshot is null. */
	bool CheckFrameConstants(sl::ViewportHandle p_viewport, uint32_t eyeIndex = 0, sl::Constants* snapshot = nullptr);

	// height = 0 -> use full per-eye DisplayRes height (default for the standard
	// upscale path). Non-zero is the subrect height the FoveatedRender route needs.
	void SetDLSSOptions(sl::ViewportHandle p_viewport, uint32_t width, uint32_t height = 0);

	/** @brief Maps an Upscaling quality mode (0-4) to the corresponding sl::DLSSMode. */
	static sl::DLSSMode DLSSModeForQualityMode(uint32_t a_qualityMode);

	/**
	 * @brief Clamps a per-eye render extent into the NGX-supported range for the
	 * given quality mode and output size. No-op when DLSS or the optimal-settings
	 * query is unavailable.
	 */
	void ClampToDLSSRenderRange(uint32_t a_qualityMode, uint32_t a_outputWidth, uint32_t a_outputHeight, uint32_t& a_renderWidth, uint32_t& a_renderHeight);

	/**
	 * @brief Dispatches DLSS upscaling for the current frame.
	 * @param a_upscalingTexture The input color texture to upscale.
	 * @param a_reactiveMask Reactive mask for temporal stability hints.
	 * @param a_transparencyCompositionMask Mask for transparency handling.
	 * @param a_motionVectors Per-pixel motion vectors for temporal reprojection.
	 */
	void Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors);
	/** @brief Updates Reflex latency reduction state and performs the Reflex sleep call. */
	void UpdateReflex();

	/** @brief Frees DLSS viewport resources through the Streamline SDK. */
	void DestroyDLSSResources();
};
