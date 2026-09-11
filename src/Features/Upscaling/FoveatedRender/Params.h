#pragma once

#include "../FoveatedRender.h"
#include "Utils/Subrect.h"
#include <d3d11.h>

namespace FoveatedRenderImpl
{
	/** @brief Engine-space inputs and output for pre-post-processing foveated reconstruction. */
	struct VRDlssParams
	{
		// Dimensions
		uint32_t renderW;       // SBS render width  (after DRS)
		uint32_t renderH;       // SBS render height (after DRS)
		uint32_t eyeWidthIn;    // per-eye input  (render) width
		uint32_t eyeHeightIn;   // per-eye input  (render) height
		uint32_t eyeWidthOut;   // per-eye output (display) width
		uint32_t eyeHeightOut;  // per-eye output (display) height

		// Textures
		ID3D11Resource* colorSrc;                // input color  (kMAIN)
		ID3D11Resource* colorDst;                // output color (kMAIN)
		ID3D11UnorderedAccessView* colorDstUAV;  // UAV for stretch output target
		ID3D11Resource* depthTexture;
		ID3D11Resource* reactiveMask;
		ID3D11Resource* transparencyMask;
		ID3D11Resource* motionVectors;

		// Mode & subrect (mode set: kDefault, kFaster).
		FoveatedRender::DlssMode mode;
		Util::Subrect::UVRegion leftUV;
		Util::Subrect::UVRegion rightUV;
		bool isFullEye;

		// Jitter (pixel-space, render resolution)
		float jitterX;
		float jitterY;

		// Build a complete parameter block from current global state.
		static VRDlssParams Resolve(
			ID3D11Resource* upscalingTexture,
			ID3D11Resource* depthTexture,
			ID3D11Resource* reactiveMask,
			ID3D11Resource* transparencyMask,
			ID3D11Resource* motionVectors);
	};
}
