#pragma once
#include "I18n/I18n.h"
#include "Menu.h"
#include "OverlayFeature.h"
#include "Utils/Input.h"
#include "VR/OpenVRDetection.h"  // In Features/VR/
#include "VRStereoOptimizations.h"
#include <algorithm>
#include <d3d11.h>
#include <string>
#include <vector>
#include <winrt/base.h>

using namespace DirectX::SimpleMath;

// Backwards compatibility aliases
using ControllerDevice = InputDeviceType;
using ButtonCombo = InputCombo;

/**
 * @brief Main VR feature class providing VR-specific optimizations and the ImGuiVRHelper client
 *
 * This class extends OverlayFeature and provides:
 * - Performance optimizations (depth buffer culling, occlusion culling)
 * - Stereo reprojection/blend consistency passes
 * - The ImGuiVRHelper client glue: the standalone helper plugin owns the VR
 *   menu panel, overlay submission, controller input, combos, and the status
 *   HUD; this feature wires CS settings/menu state to it (see HelperClient.cpp).
 *
 * The VR class follows the singleton pattern and integrates with the OpenVR API
 * to provide a seamless VR experience within the Open Shaders framework.
 *
 * @example
 * ```cpp
 * VR* vr = VR::GetSingleton();
 * if (vr->SupportsVR()) {
 *     vr->settings.EnableDepthBufferCullingExterior = true;
 * }
 * ```
 */
struct VR : OverlayFeature
{
public:
	//=============================================================================
	// NESTED TYPES AND CONSTANTS
	//=============================================================================

	/**
	 * @brief Configuration constants for VR feature defaults and limits
	 *
	 * These constants define the default values and valid ranges for various
	 * VR settings to ensure consistent behavior and prevent invalid configurations.
	 */
	struct Config
	{
		static constexpr int kOverlayHeight = 1080;           ///< Reference panel height in pixels (used for font sizing)
		static constexpr float kDefaultMouseDeadzone = 0.1f;  ///< Default thumbstick deadzone for mouse input
	};

	//=============================================================================
	// FEATURE BASE CLASS OVERRIDES
	//=============================================================================

	virtual inline std::string GetName() override { return "VR"; }
	virtual std::string GetDisplayName() override { return T("feature.vr.name", "VR"); }
	virtual inline std::string GetShortName() override { return "VR"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.vr.description", "Provides VR-specific optimizations and enhancements for Open Shaders, improving performance and visual quality in virtual reality environments."),
			{ T("feature.vr.key_feature_1", "Depth buffer culling optimization for VR performance"),
				T("feature.vr.key_feature_2", "ImGuiVRHelper-driven in-headset menu, overlay, and controller input"),
				T("feature.vr.key_feature_3", "Customizable controller button mappings via the helper bindings table"),
				T("feature.vr.key_feature_4", "Stereo reprojection and depth-aware blend consistency"),
				T("feature.vr.key_feature_5", "Configurable occlusion culling parameters"),
				T("feature.vr.key_feature_6", "Enhanced VR compatibility with SteamVR and OpenComposite") }
		};
	}

	virtual void Reset() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual bool SupportsVR() override { return true; }
	virtual bool IsCore() const override { return true; }

	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void EarlyPrepass() override;

	void UpdateDepthBufferCulling();

	// Stereo bilateral blend pass - called from Deferred::DeferredPasses after composite
	void DrawStereoBlend();
	void CompileStereoBlendShaders();
	/// Gates whether DispatchStencil() should even be called this frame.
	bool IsStereoOptimizationDispatchReady() const
	{
		return globals::game::isVR && stereoOpt.CanClassify();
	}
	static bool AnyScreenSpaceEffectLoaded();

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual json GetDiagnostics() override;

	virtual void DrawSettings() override;
	virtual void DrawPerformanceSettings() override;
	/// @brief DrawPerformanceSettings() only draws the stereo reprojection toggle.
	bool PerformanceSectionRequiresVR() const override { return true; }
	std::string GetPerformanceSectionLabel() override { return GetDisplayName(); }
	int GetPerformanceOrder() const override { return 20; }
	virtual void ApplyPerformanceProfile(PerfProfile profile) override;
	bool MatchesPerformanceProfile(PerfProfile profile) const override;

	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }

	//=============================================================================
	// OVERLAY FEATURE OVERRIDES
	//=============================================================================

	// VR status/welcome overlays are owned by ImGuiVRHelper (see RenderStatusHud),
	// so the OverlayFeature draw path is a no-op and never reports itself visible.
	virtual void DrawOverlay() override;
	virtual bool IsOverlayVisible() const override { return false; }

	//=============================================================================
	// SETTINGS STRUCTURE
	//=============================================================================

	/**
	 * @brief Configuration settings for the VR feature
	 *
	 * This structure contains all user-configurable settings for VR functionality,
	 * including performance optimizations, overlay positioning, input mapping, and
	 * visual customization options. Settings are automatically validated and clamped
	 * to valid ranges when loaded or modified.
	 */
	struct Settings
	{
		// Performance optimization settings
		bool EnableDepthBufferCullingExterior = true;  ///< Enable depth buffer culling for VR performance
		bool EnableDepthBufferCullingInterior = true;
		float MinOccludeeBoxExtent = 10.0f;  ///< Minimum bounding box size for occlusion culling

		// Stereo consistency blend pass (post-composite safety net)
		bool EnableStereoBlend = false;           ///< Enable depth-aware bilateral blend between eyes
		float StereoBlendDepthSigma = 0.01f;      ///< Depth sensitivity for bilateral weight (lower = stricter)
		float StereoBlendMaxFactor = 0.1f;        ///< Maximum blend factor; keep low to preserve stereo parallax
		float StereoBlendColorThreshold = 0.02f;  ///< Minimum color difference to trigger blending (luminance)
		// Shared reprojection debug view: Coverage drives SSS's and SSGI's own reproject
		// shaders; Back-Check/Blend Weight/Edge Detection are StereoBlend-specific.
		int ReprojectDebugMode = 0;  ///< 0=off, 1=coverage, 2=back-check, 3=blend weight, 4=edge detection

		// VR foveated shader detail: render expensive screen-space effects at reduced detail in the
		// periphery, driven by the active Foveated DLSS region. Consumed by foveated SSR.
		bool EnableSSRFoveation = false;            ///< Foveate screen-space reflection raymarching in the periphery
		bool EnableSSRFoveationHardCutoff = false;  ///< Hard-skip SSR outside the center (vs feathered falloff)

		// Thumbstick deadzone for the wand-driven mouse cursor (0.0-1.0).
		float mouseDeadzone = Config::kDefaultMouseDeadzone;

		// Overlay open/close combos forwarded to ImGuiVRHelper; the helper owns the
		// menu open/close combos. Rebinds round-trip through the helper's bindings table.
		std::vector<ButtonCombo> VROverlayOpenKeys = {
			ButtonCombo::Secondary(static_cast<uint32_t>(RE::BSOpenVRControllerDevice::Keys::kJoystickTrigger))
		};
		std::vector<ButtonCombo> VROverlayCloseKeys = {
			ButtonCombo::Primary(static_cast<uint32_t>(RE::BSOpenVRControllerDevice::Keys::kJoystickTrigger))
		};

		/**
		 * @brief Clamps all settings to their valid ranges
		 *
		 * This method ensures all numeric settings are within acceptable bounds,
		 * automatically correcting any out-of-range values that might have been
		 * loaded from configuration files or set programmatically.
		 */
		void ClampToValidRanges()
		{
			mouseDeadzone = std::clamp(mouseDeadzone, 0.0f, 1.0f);
			StereoBlendDepthSigma = std::clamp(StereoBlendDepthSigma, 0.001f, 0.1f);
			StereoBlendMaxFactor = std::clamp(StereoBlendMaxFactor, 0.0f, 0.5f);
			StereoBlendColorThreshold = std::clamp(StereoBlendColorThreshold, 0.0f, 0.2f);
			ReprojectDebugMode = std::clamp(ReprojectDebugMode, 0, 4);
		}
	};

	Settings settings;  ///< Current VR configuration settings

	//=============================================================================
	// VR-SPECIFIC PUBLIC API
	//=============================================================================

	// ImGuiVRHelper client integration (impl in Features/VR/HelperClient.cpp).
	// The VR overlay/menu/input/HUD is provided by the standalone ImGuiVRHelper
	// plugin; the VR feature is its client. CS glue (keybind<->combo mapping,
	// rebind persistence, Menu::IsEnabled, status HUD) lives in that file.
	void ConnectHelper();        ///< Connect as a client; called from PostPostLoad.
	void UpdateHelper();         ///< Per-frame focus reconcile + wand input (from Menu).
	void RenderHelperToPanel();  ///< Blit the menu into the helper's panel (from OverlayRenderer).
	void RenderStatusHud();      ///< Always-on status overlays via a HUD-mode client (from Menu).
	void FeedHelperEvent(uint32_t device, uint32_t key, bool pressed, float stickX, float stickY);
	[[nodiscard]] bool IsHelperRegistered() const;    ///< True once the menu client is connected.
	[[nodiscard]] bool HelperRequestsRender() const;  ///< True when the helper routed focus to us this frame.
	void DrawHelperBindingsTable();                   ///< Controller bindings table (from DrawSettings).

	/// Pixel dimensions of the helper's menu panel RTV (from the client SDK's
	/// GetPanel). False if the helper isn't connected or the panel isn't
	/// allocated yet. Used to lay the menu out in the panel's logical space so
	/// font sizing is correct independent of the desktop-mirror window.
	[[nodiscard]] bool GetHelperPanelSize(uint32_t& width, uint32_t& height) const;

	//=============================================================================
	// PUBLIC MEMBER VARIABLES
	//=============================================================================

	// Stereo blend compute shader resources
	winrt::com_ptr<ID3D11ComputeShader> stereoBlendCS;
	winrt::com_ptr<ID3D11ComputeShader> stereoBlendDebugBackCheckCS;
	winrt::com_ptr<ID3D11ComputeShader> stereoBlendDebugBlendWeightCS;
	winrt::com_ptr<ID3D11ComputeShader> stereoBlendDebugEdgeDetectionCS;
	eastl::unique_ptr<Texture2D> stereoBlendCopyTex;
	eastl::unique_ptr<ConstantBuffer> stereoBlendCB;

	VRStereoOptimizations stereoOpt;

	struct alignas(16) StereoBlendCB
	{
		float FrameDim[2];
		float RcpFrameDim[2];
		float DepthSigma;
		float MaxBlendFactor;
		float ColorDiffThreshold;
		float _pad;
	};

	// Engine hook integration points
	bool* gDepthBufferCulling = nullptr;
	float* gMinOccludeeBoxExtent = nullptr;

	// OpenVR version and compatibility information
	struct OpenVRInfo
	{
		bool isAvailable = false;
		bool isCompatible = false;
		std::string dllPath;
		std::string version;
		uint64_t fileSize = 0;
		std::string modificationTime;

		// Interface probing results
		bool hasOverlayInterface = false;
		bool hasSystemInterface = false;
		bool hasCompositorInterface = false;

		// Detection metadata
		VRDetection::RuntimeType runtimeType = VRDetection::RuntimeType::Unknown;
		bool probingSucceeded = false;
	} openVRInfo;

	void DetectOpenVRInfo();
	bool IsOpenVRCompatible() const;

	/// Returns the HMD display refresh rate in Hz, or 0.0 if unavailable.
	/// Queries IVRSystem via the game's already-loaded OpenVR DLL — no extra linking required.
	float GetHMDRefreshRate() const;
};
