#pragma once

#include "Feature.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/FoveatedRender.h"
#include "Upscaling/PerfMode.h"
#include "Upscaling/RCAS/RCAS.h"
#include "Upscaling/Streamline.h"
#include "Utils/BootSnapshot.h"
#include "Utils/LazyShader.h"
#include "VR/OpenVRDetection.h"
#include <algorithm>
#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

/**
 * @brief Provides upscaling functionality including DLSS, FSR and TAA.
 *
 * This feature handles various upscaling methods and frame generation technologies
 * to improve performance while maintaining visual quality.
 */
struct Upscaling : Feature
{
private:
	static constexpr std::string_view MOD_ID = "156952";

public:
	// Feature interface
	virtual inline std::string GetName() override { return "Upscaling"; }
	virtual std::string GetDisplayName() override { return T("feature.upscaling.name", "Upscaling"); }
	virtual inline std::string GetShortName() override { return "Upscaling"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline bool SupportsVR() override { return true; }
	virtual inline bool IsCore() const override { return false; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.upscaling.description", "Advanced upscaling and frame generation technologies for improved performance"),
			{ T("feature.upscaling.key_feature_1", "DLSS (Deep Learning Super Sampling) support"),
				T("feature.upscaling.key_feature_2", "FSR (FidelityFX Super Resolution) support"),
				T("feature.upscaling.key_feature_3", "TAA (Temporal Anti-Aliasing) support"),
				T("feature.upscaling.key_feature_4", "Frame generation for supported systems") } };
	};

	float2 jitter = { 0, 0 };

	enum class FrameGenMethod
	{
		kNone,
		kFSR,
		kDLSSG
	};

	enum class UpscaleMethod
	{
		kNONE,
		kTAA,
		kFSR,
		kDLSS
	};

	// Upscale preset indexed by numeric value: lower values (Quality) mean higher internal
	// render resolution; higher values (Ultra Performance) upscale more. Matches FfxFsr3QualityMode.
	enum class QualityMode : uint
	{
		kNativeAA = 0,
		kQuality = 1,
		kBalanced = 2,
		kPerformance = 3,
		kUltraPerformance = 4,
	};

	static constexpr uint32_t kFsr4RuntimeSelectionSchemaVersion = 1;

	struct Settings
	{
		uint upscaleMethod = (uint)UpscaleMethod::kDLSS;
		uint upscaleMethodNoDLSS = (uint)UpscaleMethod::kFSR;
		uint qualityMode = (uint)QualityMode::kQuality;
		uint frameLimitMode = 1;
		uint frameGenerationMode = 1;
		uint frameGenerationForceEnable = 0;
		bool frameGenerationAllowInMenus = false;
		// Workaround for adapters where DLSS-G initializes successfully but silently
		// produces no interpolated frames; forces the FSR3 FG backend instead.
		bool preferFSRFrameGen = false;
		// Generated frames per real frame (1=2x). Clamped at apply time to the
		// hardware-reported DLSSGState::numFramesToGenerateMax.
		uint dlssgFramesToGenerate = 1;
		uint streamlineLogLevel = 0;  // 0=Off, 1=Default, 2=Verbose
		// Both default to 0.8, matching AMD's own FSR3 sample default.
		float sharpnessFSR = 0.8f;
		bool sharpnessEnabledDLSS = false;
		float sharpnessDLSS = 0.8f;
		uint presetDLSS = 0;  // 0=Default, 1=J, 2=K, 3=L, 4=M
		bool reflexLowLatencyMode = false;
		bool reflexLowLatencyBoost = false;
		bool reflexUseMarkersToOptimize = false;
		bool reflexUseFPSLimit = false;
		float reflexFPSLimit = 60.0f;

		// VR PerfMode auto-lock (default on): allocate engine render targets at upscaled-render
		// resolution to bank VRAM/bandwidth. Boot-locked (changing it re-allocates RTs at world load).
		// Engages only when ShouldEngagePerfMode() holds; a no-op otherwise (see GetUpscaleMethod area).
		bool renderAtUpscaleRes = true;

		// Explicit per-eye fraction of native HMD size (0 = Auto: preset-derived).
		// Boot-locked; the preset is inert while set. DLSS clamps into NGX range.
		float vrRenderScale = 0.0f;

		// Opt in to AMD's runtime FSR4 upscaler DLL on eligible RDNA4 hardware instead of the
		// host-linked FSR3 SDK; falls back to FSR3 on any failure.
		bool fsr4RuntimeEnable = false;

		// Tracks whether fsr4RuntimeEnable has been auto-migrated for the detected adapter.
		// Defaults to current so a fresh config needs no migration; LoadSettings resets it to
		// 0 when absent from JSON so pre-existing configs run the migration once.
		uint32_t fsr4RuntimeSelectionSchemaVersion = kFsr4RuntimeSelectionSchemaVersion;
	};

	static constexpr float kVRRenderScaleMin = 0.33f;
	static constexpr float kVRRenderScaleMax = 0.95f;

	Settings settings;

	// Single source of truth for restart-gated fields. Order is not load-bearing
	// — the call-site `DrawSettingDiff` invocations in DrawSettings() handle any
	// per-field conditional gating (e.g., qualityMode/upscaleMethod banners only
	// render while PerfMode's render-target hook is active). MCP discovery
	// reports the full set; clients can check feature state themselves.
	// presetDLSS is deliberately NOT here: Streamline::SetDLSSOptions reads
	// settings.presetDLSS per-frame and applies it via slDLSSSetOptions, so
	// it's already runtime-effective.
	inline static constexpr Util::Settings::RestartTable<Settings, 9> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, frameGenerationMode, "Frame Generation"),
		UTIL_RESTART_FIELD(Settings, frameGenerationForceEnable, "Force Enable Frame Generation"),
		UTIL_RESTART_FIELD(Settings, preferFSRFrameGen, "Prefer AMD FSR Frame Generation"),
		UTIL_RESTART_FIELD(Settings, renderAtUpscaleRes, "Render at Upscaled Resolution"),
		UTIL_RESTART_FIELD(Settings, streamlineLogLevel, "Streamline Logging"),
		UTIL_RESTART_FIELD(Settings, upscaleMethod, "Upscaling Method"),
		UTIL_RESTART_FIELD(Settings, qualityMode, "Upscale Preset"),
		UTIL_RESTART_FIELD(Settings, vrRenderScale, "VR Render Scale"),
		// CreateUpscalingTextureResources (which allocates runtimeFsrDepthTexture)
		// only runs on an upscale-method change, not on this toggle -- restart-gate
		// it so a mid-session flip can't select the runtime provider without ever
		// allocating the texture Upscale() then dereferences.
		UTIL_RESTART_FIELD(Settings, fsr4RuntimeEnable, "Use Runtime FSR4"),
	} };
	Util::Settings::BootSnapshot<Settings> bootSnapshot{ kRestartFields };

	struct JitterCB
	{
		float2 jitter;
		float useWideKernel;
		float pad0;
	};

	struct UpscalingDataCB
	{
		float2 trueSamplingDim;  // per-eye render dim in VR, full render dim otherwise
		uint eyeOffsetX;         // X offset into stereo source buffers; 0 for non-VR / left eye
		uint pad0;
	};

	struct CameraMotionVectorsCB
	{
		float4x4 curViewProjUnjitteredInverse[2];  // index 1 unused in flat
		float4x4 prevViewProjUnjittered[2];
	};

	ConstantBuffer* jitterCB = nullptr;
	ConstantBuffer* upscalingDataCB = nullptr;
	ConstantBuffer* cameraMotionVectorsCB = nullptr;

	// True while the current menu frame's MV buffer holds camera-derived motion
	// (written by FillMenuCameraMotionVectors); gates the upscalers' menu reset.
	bool menuCameraMVsValid = false;

	// Runtime state
	bool isWindowed = false;
	bool lowRefreshRate = false;
	bool fidelityFXMissing = false;
	bool d3d12SwapChainActive = false;

	// Timing and scaling
	double refreshRate = 0.0f;
	float2 resolutionScale = { 1.0f, 1.0f };
	LARGE_INTEGER qpf;

	// FG FPS Measurement for Overlay
	bool IsFrameGenerationDx12PathActive() const;
	bool IsFrameGenerationActive() const;
	bool ShouldUseFrameGenerationThisFrame() const;
	bool IsUpscalingActive() const;

	// Feature interface overrides
	std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const override
	{
		// While PerfMode is engaged the engine RTs are sized for qualityMode/upscaleMethod, so both
		// are boot-locked; off, they switch live. renderAtUpscaleRes/vrRenderScale are boot-locked
		// only when the other prerequisites could make editing them engage PerfMode (else a no-op).
		// static thread_local: storage must outlive the returned span; callers consume it on the
		// same thread before the next call.
		static thread_local std::vector<Util::Settings::RestartFieldInfo> scratch;
		scratch.clear();
		if (perfMode.IsHookActive()) {
			if (!perfMode.IsExplicitScaleLatched())
				return { kRestartFields.data(), kRestartFields.size() };
			// An explicit scale owns the render res, so the preset is inert until
			// the scale is cleared; don't demand a restart for an inert field.
			for (const auto& f : kRestartFields) {
				if (f.offset != offsetof(Settings, qualityMode))
					scratch.push_back(f);
			}
			return { scratch.data(), scratch.size() };
		}
		const bool gatePerfFields = PerfModePrerequisitesMet();
		for (const auto& f : kRestartFields) {
			if (f.offset == offsetof(Settings, qualityMode) || f.offset == offsetof(Settings, upscaleMethod))
				continue;
			if ((f.offset == offsetof(Settings, renderAtUpscaleRes) || f.offset == offsetof(Settings, vrRenderScale)) && !gatePerfFields)
				continue;
			scratch.push_back(f);
		}
		return { scratch.data(), scratch.size() };
	}
	const void* GetBootValue(std::string_view jsonKey) const override { return bootSnapshot.RawBoot(jsonKey); }
	const void* GetSettingsBlob() const override { return &settings; }
	size_t GetSettingsBlobSize() const override { return sizeof(settings); }

	virtual void DrawSettings() override;
	virtual void DrawPerformanceSettings() override;
	void DrawPerformancePresets() override;
	std::string GetPerformanceSectionLabel() override;
	int GetPerformanceOrder() const override { return 10; }
	virtual void ApplyPerformanceProfile(PerfProfile profile) override;
	bool MatchesPerformanceProfile(PerfProfile profile) const override;
	/** @copydoc Feature::GetProfilePreviewText */
	std::string GetProfilePreviewText(PerfProfile profile) const override;
	/** @copydoc Feature::RegisterUxActions */
	void RegisterUxActions() override;
	/// @brief Renders the PerfMode (render-at-upscaled-res) toggle. Shared by the
	/// upscaler panel and the Performance hub. Meaningful on Flat and VR alike;
	/// gates on the active upscale method instead (DLSS/FSR only).
	void DrawPerfModeToggle();
	/// @brief Renders the Foveated DLSS enable + tuning tree. Shared by the upscaler
	/// panel and the Performance hub. VR-only; self-gates via IsRuntimeSupported()
	/// (shown disabled off-VR rather than hidden).
	void DrawFoveationControls(bool showTuning = true);
	const char* GetQualityModeName(uint qualityMode) const;
	virtual void SaveSettings(json& o_json) override;
	virtual void LoadSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;

	/**
	 * @brief Installs Direct3D-related hooks for device and factory creation.
	 *
	 * Loads FidelityFX support and patches the import address table (IAT) to redirect D3D11 device and DXGI factory creation functions to custom hook implementations.
	**/
	virtual void Load() override;
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;

	UpscaleMethod GetUpscaleMethod() const;
	FrameGenMethod GetFrameGenMethod() const;
	bool UsesDLSSGFrameGen() const;

	/** @brief Real:generated frame ratio for the active FG backend -- the single source
	 * of truth for pacing (FrameLimiter) and reporting (PerformanceOverlay). */
	uint GetFrameGenerationMultiplier() const;

	/** @brief Frame-generation runtime state for devbench (method, multiplier, DLSS-G status). */
	virtual json GetDiagnostics() override;

	// PerfMode can bank render resolution only in VR, with an upscale method that redirects its
	// output to a separate display-res target (DLSS/FSR — TAA/None can't), and a preset below 1.0x
	// (Native AA banks nothing). Prerequisites = everything except the renderAtUpscaleRes opt-in;
	// ShouldEngagePerfMode = prerequisites AND the opt-in. Read from persisted settings, so both are
	// valid at hook-install time (before perfMode.IsHookActive() flips). Single gate for Hooks.cpp,
	// Globals.cpp, and restart-field reporting — keep callers from re-deriving and drifting.
	bool PerfModePrerequisitesMet() const;
	bool ShouldEngagePerfMode() const;

	/// Render-to-display scale ratio for a quality mode index
	/// (1=Quality, 2=Balanced, 3=Performance, 4=UltraPerformance).
	/// Single source of truth across DLSS, FSR, and FoveatedRender paths:
	/// the four "quality preset" ratios (1.5/1.7/2.0/3.0) are aligned across
	/// DLSS and FSR3 by NVIDIA's DLSS Programming Guide and FFX's
	/// FfxFsr3QualityMode enum, so all upscalers in this plugin route their
	/// quality lookups through here rather than duplicating the table. Returns
	/// 3.0 (UltraPerformance) on out-of-range input.
	static float GetQualityModeRatio(uint qualityMode);

	void CheckResources(UpscaleMethod a_upscalemethod);
	void CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod);
	void DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod);

	Util::LazyShader<ID3D11ComputeShader> encodeTexturesCS[5];          // One for each UpscaleMethod
	Util::LazyShader<ID3D11ComputeShader> encodeTexturesCSDepthOutput;  // FSR: converts R24G8_TYPELESS depth to R32_FLOAT
	ID3D11ComputeShader* GetEncodeTexturesCS();

	Util::LazyShader<ID3D11PixelShader> depthRefractionUpscalePS;
	ID3D11PixelShader* GetDepthRefractionUpscalePS();

	Util::LazyShader<ID3D11PixelShader> underwaterMaskUpscalePS;
	ID3D11PixelShader* GetUnderwaterMaskUpscalePS();

	Util::LazyShader<ID3D11PixelShader> cameraMotionVectorsPS;
	ID3D11PixelShader* GetCameraMotionVectorsPS();

	/**
	 * @brief Writes camera-derived motion vectors into kMOTION_VECTOR from depth and the
	 *        unjittered view-proj delta. Valid only when nothing but the camera moves
	 *        (the main menu, where no geometry pass writes motion vectors).
	 *        Sets menuCameraMVsValid on success.
	 */
	void FillMenuCameraMotionVectors();

	Util::LazyShader<ID3D11VertexShader> upscaleVS;
	ID3D11VertexShader* GetUpscaleVS();

	winrt::com_ptr<ID3D11DepthStencilState> upscaleDepthStencilState;
	winrt::com_ptr<ID3D11BlendState> upscaleBlendState;
	winrt::com_ptr<ID3D11RasterizerState> upscaleRasterizerState;

	// Shared VR HMD Mask Clearing
	winrt::com_ptr<ID3D11ComputeShader> vrClearHMDMaskCS;
	winrt::com_ptr<ID3D11Buffer> vrClearHMDMaskCB;
	// Helper to dispatch mask clearing for a single eye region
	void ClearHMDMask(ID3D11UnorderedAccessView* colorUAV, ID3D11ShaderResourceView* depthSRV,
		uint32_t eyeWidth, uint32_t eyeHeight, uint32_t depthOffsetX, uint32_t colorOffsetX);

	// Shared VR Per-Eye Intermediate Buffers
	// Owned here so both Streamline (DLSS) and FidelityFX (FSR) can use them.
	eastl::unique_ptr<Texture2D> vrIntermediateColorIn[2];           // per-eye render resolution
	eastl::unique_ptr<Texture2D> vrIntermediateColorOut[2];          // per-eye output resolution
	eastl::unique_ptr<Texture2D> vrIntermediateDepth;                // right-eye render resolution (R24G8_TYPELESS, DLSS only)
	eastl::unique_ptr<Texture2D> vrIntermediateLinearDepth[2];       // per-eye render resolution (R32_FLOAT, for FSR)
	eastl::unique_ptr<Texture2D> vrIntermediateMotionVectors[2];     // per-eye render resolution
	eastl::unique_ptr<Texture2D> vrIntermediateReactiveMask[2];      // per-eye render resolution
	eastl::unique_ptr<Texture2D> vrIntermediateTransparencyMask[2];  // per-eye render resolution

	// Helper to create/resize per-eye buffers matching source formats
	void CreateVRIntermediateTextures(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
		ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc);

	// Helper: Create a Texture2D matching source format at a given size
	static eastl::unique_ptr<Texture2D> CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
		bool copyBindFlags = false, bool createSRV = false, bool createUAV = false, const char* name = nullptr);

	// Shared Pipeline Steps

	/// Ensures VR per-eye intermediate textures exist at the correct resolution.
	/// Must be called before any per-eye EncodeTexturesCS dispatch or PreparePerEyeInputs.
	void EnsureVRIntermediateTextures();

	/// Splits the combined stereo color buffer into per-eye intermediates, copies raw
	/// motion vectors, and clears the HMD hidden area. FSR-only.
	/// Reactive/transparency masks are written by EncodeTexturesCS.
	void PreparePerEyeInputs(ID3D11Resource* colorSrc);
	void FinalizePerEyeOutputs(ID3D11Resource* colorDst);

	void ConfigureTAA();
	void ConfigureUpscaling(RE::BSGraphics::State* a_state);
	void Upscale();

	// D3D11 textures
	Texture2D* reactiveMaskTexture = nullptr;
	Texture2D* transparencyCompositionMaskTexture = nullptr;
	Texture2D* motionVectorCopyTexture = nullptr;
	Texture2D* sharpenerTexture = nullptr;
	Texture2D* runtimeFsrDepthTexture = nullptr;

	virtual void ClearShaderCache() override;

	// Static instances instead of singletons
	static inline Streamline streamline;      ///< DX11 instance: DLSS + Reflex + PCL
	static inline Streamline streamlineDX12;  ///< DX12 instance: DLSS-G frame generation
	static inline FidelityFX fidelityFX;      ///< AMD FSR frame generation
	static inline DX12SwapChain dx12SwapChain;
	static inline RCAS rcas;                      ///< Standalone RCAS sharpening for DLSS
	static inline PerfMode perfMode;              ///< VR-only: render engine at upscaled-render res
	static inline FoveatedRender foveatedRender;  ///< VR-only: foveated subrect DLSS

	Util::LazyShader<ID3D11PixelShader> copyDepthToSharedBufferPS;

	float projectionPosScaleX = 0.0f;
	float projectionPosScaleY = 0.0f;

	float dynamicResolutionWidthRatio = 1.0f;
	float dynamicResolutionHeightRatio = 1.0f;

	bool previousUpscalingWasActive = false;
	bool depthUpscaleUseWideKernel = false;

	/// Set by MenuOpenCloseEventHandler when LoadingMenu closes (cell/worldspace transitions,
	/// initial load). Consumed at the start of Upscale() to force a one-frame DLSS feature
	/// rebuild - works around a VR-only persistent ~2-3ms GPU regression after worldspace
	/// loads that otherwise only clears when the user manually toggles DLSS/preset. VR+DLSS
	/// only; flat has no repro and per-eye extent asymmetry doesn't apply.
	std::atomic<bool> pendingDLSSReset{ false };

	void CopySharedD3D12Resources();
	void PostDisplay();
	void PerformUpscaling();
	void UpscaleDepth();

	/**
	 * @brief Standalone full-resolution underwater mask repair (VR).
	 *
	 * Same draw as UpscaleDepth's mask branch on the full-resolution path,
	 * extracted so callers that bypass the standard upscale flow (notably
	 * PerfMode::HandlePostProcessing, where engine RTs are pre-shrunk to
	 * renderRes and DLSS targets a private displayRes texture) can drive
	 * the repair without going through UpscaleDepth's wider envelope.
	 * Sets and leaves D3D11 pipeline state dirty on exit — wrap in your
	 * own save/restore (e.g. Util::FullscreenPassScope).
	 */
	void RunUnderwaterMaskRepair();

	/**
	 * @brief Applies RCAS sharpening to the main render target after DLSS upscaling.
	 *
	 * Runs in HDR space before tonemapping. Only called when DLSS is active and sharpness > 0.
	 */
	void ApplySharpening();

	/**
	 * @brief Whether PerfMode's zero-copy sharpening redirect (DLSS writes refraTempTex
	 * instead of testTexture) is active this frame. Shared by Streamline::Upscale (which
	 * picks colorOut) and PerfMode::MaybeBlitMenuBG (which must resolve the redirect).
	 */
	bool IsPerfModeSharpenRedirectActive() const
	{
		return perfMode.IsHookActive() && perfMode.GetTestTexture() && perfMode.GetTestTextureUAV() &&
		       perfMode.GetRefraTempTex() && perfMode.GetRefraTempSRV() && perfMode.GetRefraTempUAV() &&
		       settings.sharpnessEnabledDLSS && settings.sharpnessDLSS > 0.0f;
	}

	static void TimerSleepQPC(int64_t targetQPC);

	void FrameLimiter();

	static double GetRefreshRate(HWND a_window);

	// Unified interface methods - external code should use these instead of direct access
	void LoadUpscalingSDKs();  // Loads all SDKs at once
	HANDLE GetFrameLatencyWaitableObject() const;
	float GetFrameTime() const;

	// Backend interface methods
	bool IsBackendInitialized() const;
	void CheckBackendFeatures(IDXGIAdapter* adapter);
	void UpgradeBackendInterface(void** ppInterface);
	void SetBackendD3DDevice(ID3D11Device* device);
	void PostBackendDevice();

	// Module availability methods
	bool HasFrameGenModule() const;

	// Proxy interface methods
	void SetProxyD3D11Device(ID3D11Device* device);
	void SetProxyD3D11DeviceContext(ID3D11DeviceContext* context);
	void CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateProxySwapChainDirect(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateProxyInterop();
	IDXGISwapChain* GetProxySwapChain();

	using BlurResources = DX12SwapChain::BlurResources;

	// Get all D3D11 resources needed for background blur when D3D12 swap chain is active
	BlurResources GetBlurResources() const;

private:
	// OpenComposite conflict guard: when the OpenComposite VR shim runs its own
	// DLSS/FSR/DLAA upscaling, ours stands down to avoid double upscaling.
	// Detection lives in VRDetection; this class owns the force-to-None policy.
	// forceRefresh re-probes the config; otherwise the cached result is returned.
	const VRDetection::OpenCompositeUpscalingState& GetOpenCompositeUpscalingBlocker(bool a_forceRefresh = false) const;
	void ApplyOpenCompositeUpscalingBlocker(bool a_forceRefresh = false);

	mutable VRDetection::OpenCompositeUpscalingState openCompositeUpscalingBlocker;
	mutable bool openCompositeUpscalingBlockerCacheValid = false;
	bool openCompositeUpscalingBackendSkipLogged = false;

	struct Main_UpdateJitter
	{
		static void thunk(RE::BSGraphics::State* a_state);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_PostProcessing
	{
		static void thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetScissorRect
	{
		static void thunk(RE::BSGraphics::Renderer* This, int a_left, int a_top, int a_right, int a_bottom);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_RenderPrecipitation
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSFaceGenManager_UpdatePendingCustomizationTextures
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		static bool Register();
	};
};
