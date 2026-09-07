#pragma once

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "Utils/BootSnapshot.h"
#include <d3d11.h>
#include <unordered_map>
#include <winrt/base.h>

/**
 * @brief VR Stereo Rendering Optimizations feature.
 *
 * Uses hardware stencil culling to skip Eye 1 pixel shading for pixels that can be
 * reprojected from Eye 0, then restores depth and the G-buffer for those pixels so the
 * deferred composite lights them natively. This avoids redundant pixel shading in the
 * overlapping stereo region without sharing shaded color between eyes.
 *
 * Pipeline:
 *   1. DispatchStencil()             - CS classifies per-pixel reprojection viability into a mode texture,
 *                                      then a fullscreen VS/PS pass writes that classification into the stencil buffer.
 *   2. (Game renders Eye 1)          - Hardware stencil test skips shading for marked pixels.
 *   3. RepairCulledEye1()            - Restores depth and reprojects the G-buffer from Eye 0 for the
 *                                      skipped pixels, so all downstream passes light Eye 1 natively.
 */
struct VRStereoOptimizations
{
	bool loaded = false;

	//=============================================================================
	// ENUMS
	//=============================================================================

	/// Operating mode for stereo reprojection
	enum class StereoMode : uint32_t
	{
		Off = 0,    ///< Feature disabled
		Enable = 1  ///< Stereo reprojection enabled
	};

	/// Per-pixel classification written by StencilCS
	enum PixelMode : uint8_t
	{
		MODE_DISOCCLUDED = 0,     ///< Fully shaded natively (no reprojection)
		MODE_EDGE = 1,            ///< Depth-edge band: fully shaded natively
		MODE_MAIN = 2,            ///< Eligible for culling: Eye 1 shading skipped, G-buffer reprojected from Eye 0
		MODE_EDGE_NEIGHBOUR = 3,  ///< Outer band near an edge: fully shaded natively
		MODE_FULL_BLEND = 4,      ///< Near-camera (within FullBlendDistance): excluded from culling, shaded in both eyes
	};

	//=============================================================================
	// CONSTANTS
	//=============================================================================

	//=============================================================================
	// PUBLIC METHODS
	//=============================================================================

	void SetupResources();
	void Reset();
	void DrawSettings();
	void SaveSettings(json& o_json);
	void LoadSettings(json& o_json);
	void RestoreDefaultSettings();
	void ClearShaderCache();

	/// True when the GPU supports typed UAV loads for every G-buffer format the
	/// fill CS touches. Stateless capability query (no setup ordering dependency),
	/// so Deferred can gate UAV-capable G-buffer allocation on the same condition.
	static bool SupportsGBufferFill();

	//=============================================================================
	// SETTINGS
	//=============================================================================

	struct Settings
	{
		StereoMode stereoMode = StereoMode::Off;
		float disocclusionDepthThreshold = 0.01f;
		float edgeDepthThreshold = 0.05f;
		float minEdgeDistance = 5000.0f;  ///< Minimum linearized depth for edge AA (game units)
		float fullBlendDistance = 0.0f;   ///< Linearized depth below which near-camera geometry is excluded from culling (game units)
		/// Eye 0 must be within this fraction of Eye 1's depth to count as a genuine occluding
		/// edge (isDisoccluded when eye0Depth < eye1Depth * ratio); 0 = disabled.
		float directionalOcclusionRatio = 0.9f;
		// reserved for foveated reprojection — see alandtse/open-shaders#143
		float foveatedRegionRadius = 0.3f;
		float foveatedRegionCenterX = 0.5f;
		float foveatedRegionCenterY = 0.5f;
		bool useEyeTracking = false;

		// Debug controls
		bool debugSkipMerge = false;
		bool debugDepthMap = false;

	} settings;

	// stereoMode is restart-gated: the stencil/CS resources are only set up
	// when `loaded` is true at boot, and toggling mid-session can't install
	// them. Latched from VR::PostPostLoad.
	inline static constexpr Util::Settings::RestartTable<Settings, 1> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, stereoMode, "VR Stereo Reprojection"),
	} };
	Util::Settings::BootSnapshot<Settings> bootSnapshot{ kRestartFields };

	void LatchBootSnapshot() { bootSnapshot.LatchIfNeeded(settings); }

	//=============================================================================
	// GPU CONSTANT BUFFER (must match HLSL cbuffer layout exactly)
	//=============================================================================

	struct alignas(16) VRStereoOptParams
	{
		float FrameDim[2];     // Full stereo buffer dimensions
		float RcpFrameDim[2];  // 1.0 / FrameDim

		uint32_t StereoModeValue;  // Cast of StereoMode enum (0=Off, 1=Enable)
		float DisocclusionThreshold;
		float EdgeDepthThreshold;
		uint32_t _pad0;

		float _pad1[2];
		float FoveatedRadius;  // reserved for foveated reprojection — see alandtse/open-shaders#143
		float DirectionalOcclusionRatio;

		float FoveatedCenter[2];  // reserved for foveated reprojection — see alandtse/open-shaders#143
		float MinEdgeDistance;
		float FullBlendDistance;  // Linearized depth for full blend zone
	};
	static_assert(sizeof(VRStereoOptParams) % 16 == 0, "VRStereoOptParams must be 16-byte aligned for HLSL cbuffer.");

	//=============================================================================
	// PUBLIC API
	//=============================================================================

	/**
	 * @brief Classify Eye 1 pixels and write stencil marks.
	 *
	 * Dispatches the stencil classification CS, then performs a fullscreen triangle pass
	 * to write the classification into the hardware stencil buffer.
	 * Called from Deferred::StartDeferred() after OverrideBlendStates().
	 */
	void DispatchStencil();

	/// @brief Live sizes and per-frame flags for devbench diagnostics.
	json GetDiagnostics() const;

	/// @brief True when the classification (mode texture) pass is ready, independent of stereoMode.
	bool CanClassify() const
	{
		return loaded && stencilCS && texPerPixelMode && paramsCB;
	}

	/// @brief True when the mode texture holds real classification values, not debugDepthMap's
	/// unrelated visualization overload -- the gate external consumers (SSGI, Shadows) must use.
	/// Also requires this frame's classify dispatch to have actually written the texture: a null
	/// depth SRV (see DispatchStencil) skips the write and leaves stale/previous-frame data.
	bool CanExternallyConsumeClassification() const
	{
		return CanClassify() && !settings.debugDepthMap && classifiedThisFrame;
	}

	/**
	 * @brief Returns true when stencil classification/write resources are ready.
	 *
	 * This mirrors DispatchStencil prerequisites except transient per-frame inputs
	 * like depth SRV availability.
	 */
	bool CanDispatchStencil() const
	{
		// Cull Eye 1 geometry only when every pass that repairs it is ready: the stencil
		// classify/write pass AND the depth-fill + G-buffer-fill passes. Without this, a
		// fill-shader compile failure would cull Eye 1 and never restore it (full corruption).
		return loaded &&
		       settings.stereoMode != StereoMode::Off &&
		       !settings.debugSkipMerge &&
		       gBufferFillSupported &&
		       stencilCS &&
		       stencilWriteVS &&
		       stencilWritePS &&
		       depthFillPS &&
		       gBufferFillCS &&
		       texPerPixelMode &&
		       paramsCB &&
		       stencilWriteDSS &&
		       stencilWriteRS &&
		       depthFillDSS;
	}

	/**
	 * @brief Creates or retrieves a modified DSS with stencil NOT_EQUAL test.
	 *
	 * Clones the given DSS with read-only stencil (WriteMask=0x00, Func=NOT_EQUAL, ref=1)
	 * so that pixels marked by our stencil write pass are skipped during normal rendering.
	 * Cached per unique input DSS pointer.
	 *
	 * @param originalDSS The original depth-stencil state to modify.
	 * @return Modified DSS with stencil test, or original if creation fails.
	 */
	ID3D11DepthStencilState* GetOrCreateModifiedDSS(ID3D11DepthStencilState* originalDSS);

	/// Whether the stencil pass is currently active this frame
	bool IsStencilActive() const { return stencilActive; }
	void NoteStencilSwap() { ++stencilSwapCount; }

	/**
	 * @brief Repair the stencil-culled Eye 1 pixels after geometry rendering.
	 *
	 * Single deferred-pipeline entry point: deactivates stencil culling, restores depth,
	 * then reprojects the G-buffer for the culled pixels so all downstream consumers
	 * (SSGI, composite, water, sky) run unmodified and light Eye 1 natively. No-op when
	 * stencil culling did not engage this frame. Must run after geometry, before any
	 * pass that reads depth or the G-buffer; the sub-steps are ordering-sensitive and
	 * encapsulated here so callers cannot interleave work between them.
	 */
	void RepairCulledEye1();

	/// Get mode texture SRV for external consumers (GBufferFill, StencilWrite)
	ID3D11ShaderResourceView* GetModeTextureSRV() const { return texPerPixelMode ? texPerPixelMode->srv.get() : nullptr; }

private:
	//=============================================================================
	// INTERNAL METHODS
	//=============================================================================

	/// Fullscreen triangle pass: reads mode texture, writes stencil ref=1 for MODE_MAIN pixels
	void ExecuteStencilWritePass();

	/// Deactivate stencil culling once geometry rendering completes (RepairCulledEye1 step 1).
	void DeactivateStencil();

	/// Fullscreen pass (stencil EQUAL ref=1) writing SV_Depth from the classification depth
	/// source, restoring depth for culled Eye 1 pixels (RepairCulledEye1 step 2).
	void ExecuteDepthFillPass();

	/// Reproject the G-buffer from Eye 0 into the culled Eye 1 pixels so downstream passes
	/// light Eye 1 natively (RepairCulledEye1 step 3).
	void DispatchGBufferFill();

	/// Sets the rasterizer viewport to the Eye 1 (right) half of the classified SBS area (frameDim).
	void SetEye1Viewport();

	/// Compiles all shaders used by this feature
	void CompileShaders();

	/// Updates the constant buffer with current settings and frame dimensions
	void UpdateConstantBuffer();

	//=============================================================================
	// GPU RESOURCES
	//=============================================================================

	eastl::unique_ptr<ConstantBuffer> paramsCB;
	eastl::unique_ptr<Texture2D> texPerPixelMode;  ///< R8_UINT classification texture (full SBS resolution)

	winrt::com_ptr<ID3D11DepthStencilState> stencilWriteDSS;
	winrt::com_ptr<ID3D11DepthStencilState> depthFillDSS;
	winrt::com_ptr<ID3D11RasterizerState> stencilWriteRS;

	winrt::com_ptr<ID3D11ComputeShader> stencilCS;
	winrt::com_ptr<ID3D11ComputeShader> gBufferFillCS;
	winrt::com_ptr<ID3D11ComputeShader> stencilDebugDepthMapCS;
	winrt::com_ptr<ID3D11VertexShader> stencilWriteVS;
	winrt::com_ptr<ID3D11PixelShader> stencilWritePS;
	winrt::com_ptr<ID3D11PixelShader> depthFillPS;

	/// Cache of original DSS -> modified DSS with stencil NOT_EQUAL enforcement
	std::unordered_map<ID3D11DepthStencilState*, winrt::com_ptr<ID3D11DepthStencilState>> dssCache;

	bool stencilActive = false;
	uint32_t stencilSwapCount = 0;
	/// Classified SBS size this frame (dynamic-resolution scaled).
	float2 frameDim{};
	/// True once DispatchStencil() has written texPerPixelMode for the current frame.
	bool classifiedThisFrame = false;

	// GBufferFillCS does typed UAV loads on the G-buffer formats (R10G10B10A2,
	// R11G11B10, R16_UNORM, fp16); without TypedUAVLoadAdditionalFormats those reads
	// return undefined data, so the feature stays off rather than corrupt Eye 1.
	bool gBufferFillSupported = false;
};
