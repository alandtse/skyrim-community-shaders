#pragma once

#include <d3d11.h>
#include <nvapi.h>
#include <nvapi_lite_d3dext.h>
#include <vector>
#include <winrt/base.h>

namespace VRFeatures
{
	/** @brief Per-eye VRS centering data, sourced from the real optical axis when available. */
	struct FoveationProfile
	{
		bool usingRealLensCenter = false;
		// Signed delta from each eye's natural (0.5, 0.5) center, in that eye's
		// own normalized UV space -- not an absolute position.
		float2 centerOffsets[2] = {};
	};

	/** @brief Manages NVIDIA Variable Rate Shading via NVAPI for VR foveated rendering. */
	class VRVariableRateShading
	{
	public:
		static VRVariableRateShading* GetSingleton()
		{
			static VRVariableRateShading instance;
			return &instance;
		}

		/** @brief Initializes NVAPI and checks hardware VRS support; safe to call every frame (no-op after the first call). */
		bool Initialize();
		/** @brief Applies the coarse shading-rate pattern, sized to the current DLSS/FSR internal render resolution. */
		void ApplyForRenderTarget(ID3D11DeviceContext* a_context);
		/** @brief Turns VRS off (all tiles full rate) without releasing GPU resources. */
		void Disable(ID3D11DeviceContext* a_context);
		/** @brief Releases the shading-rate texture/view. */
		void Cleanup();
		/** @brief Enables/disables VRS, initializing NVAPI on first enable and releasing resources on disable. */
		void SetEnabled(bool a_enabled);
		/** @brief Whether VRS is currently turned on (independent of hardware availability). */
		bool IsEnabled() const { return enabled; }
		/** @brief Whether Initialize() found hardware VRS support. */
		bool IsAvailable() const { return nvapiAvailable; }

		/** @brief Replaces the active foveation profile used to derive VRS eye centers. */
		void SetFoveationProfile(const FoveationProfile& a_profile);

		/** @brief Tunes the coverage radius and inner/mid ring ratios; clamps to sane ranges. */
		void SetTuning(float a_radiusScale, float a_innerRadiusFactor, float a_midRadiusFactor);

		/** @brief Forces 1x1 shading rate for the current draw only; does not change IsEnabled(), so a caller can force full rate for one draw (e.g. grass) while VRS stays enabled overall. */
		void ForceFullRate(ID3D11DeviceContext* a_context);

		/** @brief Enables/disables the color-coded shading-rate debug overlay (green=1x1 ... red=4x4). */
		void SetDebugVisualize(bool a_enabled) { debugVisualize = a_enabled; }
		bool IsDebugVisualizeEnabled() const { return debugVisualize; }
		/** @brief Sets the periphery dither strength (0 disables); clamped to [0, 1]. */
		void SetDitherStrength(float a_strength);

		/** @brief Tints and/or dithers kMAIN by shading rate, run once per frame before tonemap consumes it; no-op if both effects are off. */
		void PostSceneProcess(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_mainResource, ID3D11UnorderedAccessView* a_mainUAV);

		/** @brief Snapshot of the active VRS region shape/center, for the settings UI. */
		struct RegionInfo
		{
			bool usingRealLensCenter = false;  // false: symmetric (0.5, 0.5) default, no per-eye lens data
			float outerWidthFraction = 1.0f;   // coverage-radius width, as a fraction of eye width
			float outerHeightFraction = 1.0f;  // coverage-radius height, as a fraction of eye height
			float innerRadiusFactor = 0.6f;    // fraction of the coverage radius rendered at full (1x1) rate
			float midRadiusFactor = 0.8f;      // fraction of the coverage radius rendered at half (1x2) rate
			float2 centerOffsets[2] = {};      // per-eye signed delta from (0.5, 0.5)
		};
		/** @brief Snapshot of the region currently in effect, for the settings UI. */
		RegionInfo GetRegionInfo() const;

		/** @brief Why IsAvailable() is currently false; meaningless when IsAvailable() is true. */
		enum class UnavailableReason
		{
			Unknown,             ///< Initialize() hasn't run yet; not a real diagnosis
			None,                ///< IsAvailable() is true
			NotVR,               ///< Not running under VR; VRS is VR-only
			NvApiInitFailed,     ///< NvAPI_Initialize failed (no NVIDIA driver, or non-NVIDIA GPU)
			HardwareUnsupported  ///< NVAPI initialized but this GPU/driver lacks hardware VRS support
		};
		/** @brief Why IsAvailable() is currently false. */
		UnavailableReason GetUnavailableReason() const { return unavailableReason; }

	private:
		VRVariableRateShading() = default;
		~VRVariableRateShading() = default;
		VRVariableRateShading(const VRVariableRateShading&) = delete;
		VRVariableRateShading(VRVariableRateShading&&) = delete;
		VRVariableRateShading& operator=(const VRVariableRateShading&) = delete;
		VRVariableRateShading& operator=(VRVariableRateShading&&) = delete;

		static constexpr uint32_t kVrsTileSize = 16;  // fixed by the NVAPI VRS hardware spec

		void CreateShadingRateResource(uint32_t width, uint32_t height);
		void UpdateShadingRatePattern();
		// Fills the viewport shading-rate table (index 0 always full rate) and submits it.
		void SubmitShadingRateTable(ID3D11DeviceContext* a_context, bool a_enable,
			NV_PIXEL_SHADING_RATE a_rate1, NV_PIXEL_SHADING_RATE a_rate2, NV_PIXEL_SHADING_RATE a_rate3);
		void CompilePostSceneShader();

		struct SizedResource
		{
			uint32_t width = 0;
			uint32_t height = 0;
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11NvShadingRateResourceView> view;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
		};
		// Small cache keyed by size, reused instead of destroying/recreating on every
		// switch -- the actual bound render target alternates between a fixed set of
		// sizes multiple times per frame (pre-/post-upscale passes), and tearing down
		// an NVAPI shading-rate resource while the driver may still reference it
		// crashes nvwgf2umx.dll.
		static constexpr size_t kMaxCachedResources = 4;
		std::vector<SizedResource> resourceCache;

		bool enabled = false;
		bool initialized = false;
		bool nvapiAvailable = false;
		bool debugVisualize = false;
		float ditherStrength = 0.0f;
		UnavailableReason unavailableReason = UnavailableReason::Unknown;
		float radiusScale = 1.0f;
		float innerRadiusFactor = 0.6f;
		float midRadiusFactor = 0.8f;
		FoveationProfile foveationProfile{};
		winrt::com_ptr<ID3D11NvShadingRateResourceView> shadingRateView;
		winrt::com_ptr<ID3D11Texture2D> srrTexture;
		winrt::com_ptr<ID3D11ShaderResourceView> srrSRV;
		winrt::com_ptr<ID3D11ComputeShader> postSceneCS;
		winrt::com_ptr<ID3D11Buffer> postSceneCB;
		uint32_t currentWidth = 0;
		uint32_t currentHeight = 0;
	};
}
