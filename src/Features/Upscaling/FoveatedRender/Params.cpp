#include "Params.h"

#include "../../../State.h"
#include "../../../Utils/Game.h"
#include "../../Upscaling.h"
#include "../FoveatedRender.h"

namespace FoveatedRenderImpl
{
	VRDlssParams VRDlssParams::Resolve(
		ID3D11Resource* upscalingTexture,
		ID3D11Resource* depth,
		ID3D11Resource* reactive,
		ID3D11Resource* transparency,
		ID3D11Resource* mvec)
	{
		VRDlssParams p{};

		const auto screenSize = globals::state->screenSize;
		const auto renderSize = Util::ConvertToDynamic(screenSize);
		const auto displaySize = screenSize;

		p.renderW = (uint32_t)renderSize.x;
		p.renderH = (uint32_t)renderSize.y;
		p.eyeWidthIn = (uint32_t)(renderSize.x / 2);
		p.eyeHeightIn = (uint32_t)renderSize.y;
		p.eyeWidthOut = (uint32_t)(displaySize.x / 2);
		p.eyeHeightOut = (uint32_t)displaySize.y;

		p.colorSrc = upscalingTexture;
		p.colorDst = upscalingTexture;
		p.colorDstUAV = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].UAV;

		p.depthTexture = depth;
		p.reactiveMask = reactive;
		p.transparencyMask = transparency;
		p.motionVectors = mvec;

		// Mode & subrect. Stereo Subrect API: GetUV() returns the primary
		// UV (= left-eye in stereo mode); GetRightEyeUV() returns the
		// mirrored right-eye UV.
		auto& enhancer = globals::features::upscaling.foveatedRender;
		p.mode = enhancer.GetDlssMode();
		p.leftUV = enhancer.subrectController.GetUV();
		p.rightUV = enhancer.subrectController.GetRightEyeUV();
		p.isFullEye = p.leftUV.IsFullEye() && p.rightUV.IsFullEye();

		// Jitter — ConfigureUpscaling already computed correct DLSS jitter.
		auto& upscaling = globals::features::upscaling;
		p.jitterX = upscaling.jitter.x;
		p.jitterY = upscaling.jitter.y;

		return p;
	}
}
