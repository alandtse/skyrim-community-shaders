#pragma once

#include "Core.h"
#include "Utils/Subrect.h"

#include <EASTL/unique_ptr.h>
#include <d3d11_4.h>

class Texture2D;

// Primitive operations for the FoveatedRender VR DLSS pipeline.
//
// Each function is a self-contained building block. Mode pipelines in
// Modes.cpp compose these in different orders to form the Default and
// Faster strategies.
namespace FoveatedRenderImpl::Ops
{
	// Texture creation helper.
	eastl::unique_ptr<Texture2D> CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
		bool copyBindFlags = false, bool createSRV = false, bool createUAV = false, const char* name = nullptr);

	// Depth-stencil resources cannot be copied directly into the typed R32_FLOAT
	// guide textures used by Feature 18. Convert through the native depth SRV and
	// crop the requested eye/region explicitly.
	bool CopyDepthRegionToTexture(
		ID3D11Resource* source,
		ID3D11ShaderResourceView* sourceSRV,
		ID3D11UnorderedAccessView* destinationUAV,
		uint32_t sourceOffsetX,
		uint32_t sourceOffsetY,
		uint32_t width,
		uint32_t height);

	// Lazy/idempotent resource ensure helpers.
	void EnsureVRIntermediateTextures(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH,
		ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc);

	void EnsureVRSubrectTextures(uint32_t subInW, uint32_t subInH, uint32_t subOutW, uint32_t subOutH,
		ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc);

	void EnsureFasterOutputTextures(uint32_t subOutW, uint32_t subOutH, ID3D11Resource* colorSrc);

	void EnsureVRRenderSBS(uint32_t renderW, uint32_t renderH, ID3D11Resource* colorSrc);

	// Copy full-eye slices from SBS textures into per-eye intermediates.
	bool PreparePerEyeInputs(ID3D11Resource* colorSrc, ID3D11Resource* depthSrc, ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc,
		uint32_t eyeWidthIn, uint32_t eyeHeightIn, uint32_t eyeWidthOut, uint32_t eyeHeightOut);

	// Copy per-eye output intermediates back into the SBS output texture.
	bool FinalizePerEyeOutputs(ID3D11Resource* colorDst, uint32_t eyeWidthOut, uint32_t eyeHeightOut);

	// Snapshot kMAIN DRS data into vrRenderSBS.
	void SnapshotSBS(ID3D11Resource* src, uint32_t renderW, uint32_t renderH);

	// Compute-shader stretch of a single eye region from renderSBS → kMAIN.
	void StretchDRSToFullEye(ID3D11ShaderResourceView* renderSBSSRV, ID3D11UnorderedAccessView* kMainUAV,
		uint32_t dstOffsetX, uint32_t dstWidth, uint32_t dstHeight,
		uint32_t srcOffsetX, uint32_t srcWidth, uint32_t srcHeight,
		uint32_t srcEyeWidth, uint32_t srcEyeHeight);

	// StretchDRS for both eyes (snapshot must already exist in vrRenderSBS).
	void StretchDRSBothEyes(ID3D11UnorderedAccessView* dstUAV, uint32_t eyeWidthOut, uint32_t eyeHeightOut,
		uint32_t eyeWidthIn, uint32_t eyeHeightIn, uint32_t renderW, uint32_t renderH,
		ID3D11ShaderResourceView* srcOverride = nullptr);

	// Periphery temporal smooth: ensure ping-pong history textures and mvec SRV
	// cache for the render-res SBS smoothing pass.
	void EnsureTemporalResources(uint32_t renderW, uint32_t renderH, ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc);

	// Apply temporal smoothing on vrRenderSBS. Returns SRV of the smoothed result
	// (pass as srcOverride to StretchDRSBothEyes). SnapshotSBS must be called first.
	ID3D11ShaderResourceView* TemporalSmoothSBS(uint32_t renderW, uint32_t renderH);

	// If PeripheryAAMode is kTemporalSmooth, ensures resources and returns the
	// smoothed SRV; otherwise returns nullptr (StretchDRSBothEyes uses vrRenderSBS).
	// SnapshotSBS must have been called on this frame before invoking this.
	ID3D11ShaderResourceView* MaybeTemporalSmooth(const VRDlssParams& p);

	// Clear HMD hidden-area mask on both eye halves of vrRenderSBS.
	// Must be called after SnapshotSBS. Prevents sky-blue temporal bleed into
	// pixels the HMD lens never shows, which DLSS would otherwise accumulate.
	void ClearHMDMaskOnSnapshot(const VRDlssParams& p);

	// Blend a DLSS subrect output onto the destination at (offsetX, offsetY).
	// kHardCopy fast-paths to CopySubresourceRegion; Feather/Dither dispatch
	// SubrectBlendCS into dstUAV.
	void BlendSubrectToOutput(ID3D11Resource* dlssSrc, ID3D11Resource* dst, ID3D11UnorderedAccessView* dstUAV,
		uint32_t dstOffsetX, uint32_t dstOffsetY, uint32_t subWidth, uint32_t subHeight, uint32_t srcOffsetX = 0);

	// Hash of per-eye UVs + mode for change detection (forces SL DLSS resource
	// recreation). Both eyes are mixed in so asymmetric presets — e.g. Nasal
	// Convergence, where rightUV differs from leftUV — don't collide on a
	// left-eye-only hash and skip SL recreation.
	uint64_t ComputeSubrectUVHash(const Util::Subrect::UVRegion& leftUV,
		const Util::Subrect::UVRegion& rightUV, uint32_t mode);
}
