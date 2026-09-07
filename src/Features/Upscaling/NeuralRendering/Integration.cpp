#include "Integration.h"

#include "Features/HDRDisplay.h"
#include "Features/Upscaling.h"
#include "Features/Upscaling/FoveatedRender/Bridge.h"
#include "Features/Upscaling/FoveatedRender/Core.h"
#include "Features/Upscaling/PerfMode.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Renderer.h"
#include "State.h"

#include "RE/C/Console.h"

#include <algorithm>
#include <array>
#include <atomic>

namespace NeuralRendering
{
	namespace
	{
		eastl::unique_ptr<Texture2D> color[2];
		eastl::unique_ptr<Texture2D> stereoBlendTarget;
		std::uint32_t colorWidth = 0;
		std::uint32_t colorHeight = 0;
		DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
		std::uint32_t stereoBlendWidth = 0;
		std::uint32_t stereoBlendHeight = 0;
		DXGI_FORMAT stereoBlendFormat = DXGI_FORMAT_UNKNOWN;
		std::uint32_t lastAppliedFrame = UINT32_MAX;
		bool writebackLogged = false;
		bool flatRouteWasActive = false;
		bool flatFrameGenerationBlockLogged = false;
		bool flatHdrBlockLogged = false;
		bool blendFallbackLogged = false;
		std::atomic_bool historyResetRequested{ false };
		bool temporalSuppressed = false;
		std::uint32_t preUpscaleAppliedFrame = UINT32_MAX;
		bool preUpscaleModeObserved = false;
		bool preUpscaleMode = false;
		bool preUpscaleBlockLogged = false;
		bool preUpscaleSuccessLogged = false;
		bool preUpscaleExecutionFailed = false;

		bool IsTemporalOverlayOpen()
		{
			auto* state = globals::state;
			auto* ui = globals::game::ui;
			const bool consoleOpen = ui && ui->IsMenuOpen(RE::Console::MENU_NAME);
			return consoleOpen || (state && state->IsPausedOrMenuOpen(ui));
		}

		ID3D11Texture2D* ResolveRenderTargetTexture(
			const RE::BSGraphics::RenderTargetData& target,
			winrt::com_ptr<ID3D11Texture2D>& holder)
		{
			if (target.texture)
				return target.texture;
			auto resolveView = [&](ID3D11View* view) -> ID3D11Texture2D* {
				if (!view)
					return nullptr;
				winrt::com_ptr<ID3D11Resource> resource;
				view->GetResource(resource.put());
				if (!resource || FAILED(resource->QueryInterface(holder.put())))
					return nullptr;
				return holder.get();
			};
			if (auto* texture = resolveView(target.SRV))
				return texture;
			return resolveView(target.RTV);
		}

		bool EnsureColorResources(ID3D11Resource* source, std::uint32_t width, std::uint32_t height)
		{
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (!source || FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			if (color[0] && colorWidth == width && colorHeight == height && colorFormat == sourceDesc.Format)
				return true;
			const std::uint32_t resourceCount = globals::game::isVR ? 2u : 1u;
			for (std::uint32_t eye = 0; eye < resourceCount; ++eye) {
				color[eye] = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					eye == 0 ? "NeuralRendering::LdrColorLeft" : "NeuralRendering::LdrColorRight");
				if (!color[eye])
					return false;
			}
			if (!globals::game::isVR)
				color[1].reset();
			colorWidth = width;
			colorHeight = height;
			colorFormat = sourceDesc.Format;
			return true;
		}

		bool EnsureStereoBlendTarget(ID3D11Resource* source, std::uint32_t width, std::uint32_t height)
		{
			if (!source || width == 0 || height == 0)
				return false;

			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			if (stereoBlendTarget && stereoBlendWidth == width && stereoBlendHeight == height &&
				stereoBlendFormat == sourceDesc.Format && stereoBlendTarget->uav)
				return true;

			stereoBlendTarget = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
				"NeuralRendering::FoveatedBlendTarget");
			if (!stereoBlendTarget || !stereoBlendTarget->uav)
				return false;
			stereoBlendWidth = width;
			stereoBlendHeight = height;
			stereoBlendFormat = sourceDesc.Format;
			return true;
		}

		Tuning GetTuning(const FoveatedRender::Settings& settings)
		{
			return {
				settings.neuralRenderingIntensity,
				settings.neuralRenderingLocalTone,
				settings.neuralRenderingLocalStructure,
				settings.neuralRenderingSkinStructure,
				settings.neuralRenderingStyle,
				settings.neuralRenderingAutoMask,
				settings.neuralRenderingUICorrection,
				settings.neuralRenderingModelResolution,
				settings.neuralRenderingResolveMode,
				// Keep the two experimental stage-order features mutually exclusive:
				// pre-upscale already adds a second NR route before the normal DLSS pass.
				settings.neuralRenderingPreUpscale == 0 ? std::min(settings.neuralRenderingMultiPass, 2u) : 0u,
			};
		}

		void LogPreUpscaleBlocked(const char* reason)
		{
			if (!preUpscaleBlockLogged) {
				logger::warn("[DLSSNR] experimental pre-upscale route unavailable ({}); falling back to post-upscale NR", reason);
				preUpscaleBlockLogged = true;
			}
		}

		bool IsFullEyeStereo(const FoveatedRender& foveated)
		{
			return foveated.subrectController.GetUV().IsFullEye() &&
			       foveated.subrectController.GetRightEyeUV().IsFullEye();
		}

		void RestoreRenderTargets(ID3D11DeviceContext* context,
			ID3D11RenderTargetView* (&savedRTVs)[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT],
			ID3D11DepthStencilView* savedDSV)
		{
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
			for (auto*& rtv : savedRTVs)
				if (rtv)
					rtv->Release();
			if (savedDSV)
				savedDSV->Release();
		}

		bool ApplyFlatLdr(Upscaling& upscaling, FoveatedRender& foveated)
		{
			const bool frameGenerationConfigured = upscaling.IsFrameGenerationConfiguredForSession();
			const bool hdrConfigured = globals::features::hdrDisplay.loaded &&
			                           globals::features::hdrDisplay.settings.enableHDR;
			auto* renderer = globals::game::renderer;
			winrt::com_ptr<ID3D11Texture2D> framebufferHolder;
			ID3D11Texture2D* framebuffer = nullptr;
			if (renderer) {
				auto& target = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
				framebuffer = ResolveRenderTargetTexture(target, framebufferHolder);
			}
			const bool routeActive = upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
			                         foveated.settings.neuralRenderingEnabled && !frameGenerationConfigured && !hdrConfigured;
			if (!routeActive) {
				if (foveated.settings.neuralRenderingEnabled && frameGenerationConfigured && !flatFrameGenerationBlockLogged) {
					logger::warn("[DLSSNR] Flat route blocked: disable Frame Generation and restart the game");
					flatFrameGenerationBlockLogged = true;
				}
				if (foveated.settings.neuralRenderingEnabled && hdrConfigured && !flatHdrBlockLogged) {
					logger::warn("[DLSSNR] Flat route blocked: HDR Display is not supported by the LDR integration");
					flatHdrBlockLogged = true;
				}
				if (flatRouteWasActive)
					Reset();
				return false;
			}
			flatRouteWasActive = true;

			const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
			if (preUpscaleAppliedFrame == frame)
				return false;
			if (lastAppliedFrame == frame)
				return true;
			auto* context = globals::d3d::context;
			if (!renderer || !context || !globals::d3d::device || !upscaling.motionVectorCopyTexture)
				return false;

			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			if (!framebuffer || !depth.texture || !depth.depthSRV || !upscaling.motionVectorCopyTexture->resource)
				return false;

			D3D11_TEXTURE2D_DESC totalDesc{};
			D3D11_TEXTURE2D_DESC motionDesc{};
			framebuffer->GetDesc(&totalDesc);
			upscaling.motionVectorCopyTexture->resource->GetDesc(&motionDesc);
			if (!EnsureColorResources(framebuffer, totalDesc.Width, totalDesc.Height))
				return false;

			CS_GPU_PASS("NeuralRendering::FlatLdrBeforeUI");
			ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* savedDSV = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			context->CopyResource(color[0]->resource.get(), framebuffer);

			const bool succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
				color[0]->resource.get(), depth.texture, depth.depthSRV,
				upscaling.motionVectorCopyTexture->resource.get(), motionDesc.Width, motionDesc.Height,
				totalDesc.Width, totalDesc.Height, static_cast<float>(motionDesc.Width),
				static_cast<float>(motionDesc.Height), GetTuning(foveated.settings));
			if (succeeded) {
				context->CopyResource(framebuffer, color[0]->resource.get());
				lastAppliedFrame = frame;
				if (!writebackLogged) {
					logger::info("[DLSSNR] Flat LDR kFRAMEBUFFER output written before UI guides={}x{} color={}x{}",
						motionDesc.Width, motionDesc.Height, totalDesc.Width, totalDesc.Height);
					writebackLogged = true;
				}
			}

			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
			for (auto*& rtv : savedRTVs)
				if (rtv)
					rtv->Release();
			if (savedDSV)
				savedDSV->Release();
			return succeeded;
		}
	}

	void ResetHistory()
	{
		Renderer::Instance().ResetHistory();
		lastAppliedFrame = UINT32_MAX;
		preUpscaleAppliedFrame = UINT32_MAX;
	}

	void RequestHistoryReset()
	{
		historyResetRequested.store(true, std::memory_order_release);
	}

	void UpdateFrameState()
	{
		const bool overlayOpen = IsTemporalOverlayOpen();
		const bool requested = historyResetRequested.exchange(false, std::memory_order_acq_rel);
		const bool requestedPreUpscale = globals::features::upscaling.foveatedRender.settings.neuralRenderingPreUpscale != 0;
		bool stageChanged = false;
		if (!preUpscaleModeObserved) {
			preUpscaleModeObserved = true;
			preUpscaleMode = requestedPreUpscale;
		} else if (preUpscaleMode != requestedPreUpscale) {
			preUpscaleMode = requestedPreUpscale;
			stageChanged = true;
		}
		if (stageChanged) {
			preUpscaleBlockLogged = false;
			preUpscaleExecutionFailed = false;
		}
		if (!requested && overlayOpen == temporalSuppressed && !stageChanged)
			return;

		temporalSuppressed = overlayOpen;
		ResetHistory();
		logger::debug("[DLSSNR] Temporal history reset ({})",
			stageChanged ? "NR stage changed" : (requested ? "event" : (overlayOpen ? "overlay open" : "overlay closed")));
	}

	bool ApplyPreUpscale()
	{
		UpdateFrameState();
		if (temporalSuppressed)
			return false;

		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		if (!foveated.settings.neuralRenderingEnabled || foveated.settings.neuralRenderingPreUpscale == 0)
			return false;
		if (preUpscaleExecutionFailed) {
			LogPreUpscaleBlocked("the previous pre-NR execution failed; toggle the option to retry");
			return false;
		}

		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		if (preUpscaleAppliedFrame == frame)
			return true;

		const bool frameGenerationConfigured = upscaling.IsFrameGenerationConfiguredForSession();
		const bool hdrConfigured = globals::features::hdrDisplay.loaded &&
		                           globals::features::hdrDisplay.settings.enableHDR;
		if (upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS) {
			LogPreUpscaleBlocked("DLSS is not the selected upscaler");
			return false;
		}
		if (frameGenerationConfigured || upscaling.IsFrameGenerationActive()) {
			LogPreUpscaleBlocked("Frame Generation is enabled");
			return false;
		}
		if (hdrConfigured) {
			LogPreUpscaleBlocked("HDR Display uses the unsupported HDR integration");
			return false;
		}

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context || !globals::d3d::device) {
			LogPreUpscaleBlocked("D3D or renderer resources are not ready");
			return false;
		}

		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		if (!main.texture || !motionVector.texture || !depth.texture || !depth.depthSRV) {
			LogPreUpscaleBlocked("native color/depth/motion guide resources are missing");
			return false;
		}

		D3D11_TEXTURE2D_DESC colorDesc{};
		D3D11_TEXTURE2D_DESC motionDesc{};
		main.texture->GetDesc(&colorDesc);
		motionVector.texture->GetDesc(&motionDesc);
		if (colorDesc.Width == 0 || colorDesc.Height == 0 ||
			(globals::game::isVR && (colorDesc.Width < 2 || (colorDesc.Width & 1u) != 0)) ||
			motionDesc.Width == 0 || motionDesc.Height == 0) {
			LogPreUpscaleBlocked("native stereo dimensions are invalid");
			return false;
		}
		if (Renderer::Instance().IsFailureLatched()) {
			LogPreUpscaleBlocked("the shared NR renderer has an existing failure latch");
			return false;
		}

		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		bool succeeded = false;
		if (!globals::game::isVR) {
			CS_GPU_PASS("NeuralRendering::FlatPreUpscale");
			succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
				main.texture, depth.texture, depth.depthSRV, motionVector.texture,
				motionDesc.Width, motionDesc.Height, colorDesc.Width, colorDesc.Height,
				static_cast<float>(motionDesc.Width), static_cast<float>(motionDesc.Height),
				GetTuning(foveated.settings));
		} else {
			if (!FoveatedRenderImpl::Bridge::IsRouteActive()) {
				LogPreUpscaleBlocked("VR foveated route is not active");
			} else if (foveated.GetDlssMode() != FoveatedRender::DlssMode::kDefault) {
				LogPreUpscaleBlocked("VR Faster mode does not provide the isolated pre-NR guide contract");
			} else if (!IsFullEyeStereo(foveated)) {
				LogPreUpscaleBlocked("VR pre-NR is currently limited to Full Eye");
			} else {
				const std::uint32_t eyeWidth = colorDesc.Width / 2;
				const std::uint32_t eyeHeight = colorDesc.Height;
				std::uint32_t outputEyeWidth = eyeWidth;
				std::uint32_t outputEyeHeight = eyeHeight;
				if (upscaling.perfMode.IsHookActive() && upscaling.perfMode.GetTestTexture()) {
					outputEyeWidth = upscaling.perfMode.GetDisplayEyeWidth();
					outputEyeHeight = upscaling.perfMode.GetDisplayEyeHeight();
				} else {
					auto& total = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
					if (total.texture) {
						D3D11_TEXTURE2D_DESC totalDesc{};
						total.texture->GetDesc(&totalDesc);
						if (totalDesc.Width >= 2 && totalDesc.Height > 0) {
							outputEyeWidth = totalDesc.Width / 2;
							outputEyeHeight = totalDesc.Height;
						}
					}
				}

				if (outputEyeWidth == 0 || outputEyeHeight == 0) {
					LogPreUpscaleBlocked("display-resolution stereo dimensions are invalid");
				} else if (!FoveatedRenderImpl::Core::PrepareVRPerEyeInputs(
							   main.texture, depth.texture, motionVector.texture, nullptr, nullptr,
							   eyeWidth, eyeHeight, outputEyeWidth, outputEyeHeight)) {
					LogPreUpscaleBlocked("per-eye pre-NR guide preparation failed");
				} else {
					std::array<Renderer::StereoEyeInput, 2> inputs{};
					for (std::uint32_t eye = 0; eye < 2; ++eye) {
						auto& depthGuide = FoveatedRenderImpl::Core::vrIntermediateDepth[eye];
						auto& motionGuide = FoveatedRenderImpl::Core::vrIntermediateMotionVectors[eye];
						if (!depthGuide || !motionGuide)
							continue;
						inputs[eye] = {
							.depth = depthGuide->resource.get(),
							.depthSRV = depthGuide->srv.get(),
							.motionVectors = motionGuide->resource.get(),
							.sourceX = eye * eyeWidth,
							.sourceY = 0,
							.motionVectorScaleX = 1.0f,
							.motionVectorScaleY = 1.0f,
						};
					}
					if (!inputs[0].depth || !inputs[1].depth || !inputs[0].motionVectors || !inputs[1].motionVectors) {
						LogPreUpscaleBlocked("per-eye pre-NR guides were not created");
					} else {
						CS_GPU_PASS("NeuralRendering::StereoPreUpscale");
						succeeded = Renderer::Instance().ApplyStereo(globals::d3d::device, context,
							main.texture, inputs, eyeWidth, eyeHeight, eyeWidth, eyeHeight,
							GetTuning(foveated.settings));
					}
				}
			}
		}

		RestoreRenderTargets(context, savedRTVs, savedDSV);
		if (!succeeded) {
			// The experimental stage must not poison the established post-upscale
			// route. Reset the shared renderer after a pre-stage execution failure;
			// the later UI-composite hook can initialize it again for fallback.
			Renderer::Instance().Reset();
			preUpscaleExecutionFailed = true;
			LogPreUpscaleBlocked("pre-NR execution failed; renderer reset for fallback");
			return false;
		}

		preUpscaleAppliedFrame = frame;
		preUpscaleBlockLogged = false;
		preUpscaleExecutionFailed = false;
		if (!preUpscaleSuccessLogged) {
			logger::info("[DLSSNR] experimental pre-upscale route active; stage=before-dlss vr={} model={} resolve={}",
				globals::game::isVR, foveated.settings.neuralRenderingModelResolution,
				foveated.settings.neuralRenderingResolveMode == 1 ? "matched-residual" : "classic");
			preUpscaleSuccessLogged = true;
		}
		return true;
	}

	bool ApplyFoveatedLdr()
	{
		UpdateFrameState();
		if (temporalSuppressed)
			return false;

		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		if (!globals::game::isVR)
			return ApplyFlatLdr(upscaling, foveated);
		if (!globals::game::isVR || !FoveatedRenderImpl::Bridge::IsRouteActive() ||
			upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS ||
			foveated.GetDlssMode() != FoveatedRender::DlssMode::kDefault ||
			!foveated.settings.neuralRenderingEnabled || upscaling.IsFrameGenerationActive())
			return false;

		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		if (preUpscaleAppliedFrame == frame)
			return false;
		const std::uint32_t guideFrame = FoveatedRenderImpl::Core::neuralGuidesFrame;
		if (lastAppliedFrame == frame || (guideFrame != frame && !(frame > 0 && guideFrame == frame - 1)))
			return false;

		const auto& leftUV = foveated.subrectController.GetUV();
		const auto& rightUV = foveated.subrectController.GetRightEyeUV();
		const bool fullEye = leftUV.IsFullEye() && rightUV.IsFullEye();
		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		const auto* depthLeft = fullEye ? FoveatedRenderImpl::Core::vrIntermediateDepth[0].get() : FoveatedRenderImpl::Core::vrSubrectDepth[0].get();
		const auto* depthRight = fullEye ? FoveatedRenderImpl::Core::vrIntermediateDepth[1].get() : FoveatedRenderImpl::Core::vrSubrectDepth[1].get();
		const auto* motionLeft = fullEye ? FoveatedRenderImpl::Core::vrIntermediateMotionVectors[0].get() : FoveatedRenderImpl::Core::vrSubrectMotionVectors[0].get();
		const auto* motionRight = fullEye ? FoveatedRenderImpl::Core::vrIntermediateMotionVectors[1].get() : FoveatedRenderImpl::Core::vrSubrectMotionVectors[1].get();
		if (!renderer || !context || !globals::d3d::device || !depthLeft || !depthRight || !motionLeft || !motionRight)
			return false;
		auto& total = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
		if (!total.texture)
			return false;

		D3D11_TEXTURE2D_DESC totalDesc{};
		total.texture->GetDesc(&totalDesc);
		if (leftUV.w != rightUV.w || leftUV.h != rightUV.h)
			return false;
		const std::uint32_t eyeWidth = totalDesc.Width / 2;
		const std::uint32_t outWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(eyeWidth * leftUV.w));
		const std::uint32_t outHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(totalDesc.Height * leftUV.h));
		const std::uint32_t guideWidth = fullEye ? FoveatedRenderImpl::Core::vrIntermediateDepth[0]->desc.Width : FoveatedRenderImpl::Core::vrSubrectInW;
		const std::uint32_t guideHeight = fullEye ? FoveatedRenderImpl::Core::vrIntermediateDepth[0]->desc.Height : FoveatedRenderImpl::Core::vrSubrectInH;
		if (guideWidth == 0 || guideHeight == 0)
			return false;

		// The normal foveated route blends the cropped upscaler result over the
		// stretched/background image. NR used to bypass that step and hard-copy
		// the crop, which made its rectangle visible even when Edge Blend was set
		// to Feather or Dither.
		const auto blendMode = foveated.GetSubrectBlendMode();
		const bool wantsEdgeBlend = !fullEye && blendMode != FoveatedRender::SubrectBlendMode::kHardCopy;
		ID3D11Resource* destination = total.texture;
		ID3D11UnorderedAccessView* destinationUAV = total.UAV;
		bool stagedBlendTarget = false;
		if (wantsEdgeBlend && !destinationUAV) {
			// kTOTAL is not guaranteed to have D3D11_BIND_UNORDERED_ACCESS. Keep
			// its original background in a private UAV-capable copy, blend there,
			// then copy the finished SBS image back. This costs two full-frame
			// copies only on this fallback path; targets exposing a UAV stay direct.
			if (EnsureStereoBlendTarget(total.texture, totalDesc.Width, totalDesc.Height)) {
				context->CopyResource(stereoBlendTarget->resource.get(), total.texture);
				destination = stereoBlendTarget->resource.get();
				destinationUAV = stereoBlendTarget->uav.get();
				stagedBlendTarget = true;
			} else if (!blendFallbackLogged) {
				logger::warn("[DLSSNR] Edge Blend requested but kTOTAL has no usable UAV and staging allocation failed; using hard copy");
				blendFallbackLogged = true;
			}
		}

		CS_GPU_PASS("NeuralRendering::FoveatedLdrBeforeUI");
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		const Util::Subrect::UVRegion* eyeUVs[2]{ &leftUV, &rightUV };
		std::array<Renderer::StereoEyeInput, 2> inputs{};
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			const auto& uv = *eyeUVs[eye];
			const std::uint32_t x = (eye ? eyeWidth : 0) + static_cast<std::uint32_t>(eyeWidth * uv.x);
			const std::uint32_t y = static_cast<std::uint32_t>(totalDesc.Height * uv.y);
			float motionScaleX = 1.0f;
			float motionScaleY = 1.0f;
			FoveatedRenderImpl::Bridge::ComputeMvecScale(eye, motionScaleX, motionScaleY);
			inputs[eye] = {
				.depth = (fullEye ? FoveatedRenderImpl::Core::vrIntermediateDepth[eye] : FoveatedRenderImpl::Core::vrSubrectDepth[eye])->resource.get(),
				.depthSRV = (fullEye ? FoveatedRenderImpl::Core::vrIntermediateDepth[eye] : FoveatedRenderImpl::Core::vrSubrectDepth[eye])->srv.get(),
				.motionVectors = (fullEye ? FoveatedRenderImpl::Core::vrIntermediateMotionVectors[eye] : FoveatedRenderImpl::Core::vrSubrectMotionVectors[eye])->resource.get(),
				.sourceX = x,
				.sourceY = y,
				.motionVectorScaleX = motionScaleX * guideWidth,
				.motionVectorScaleY = motionScaleY * guideHeight,
			};
		}
		Tuning tuning = GetTuning(foveated.settings);
		if (!fullEye)
			// The cascade relies on full-eye dimensions and isolated stage history;
			// keep cropped/foveated regions on the established single-pass route.
			tuning.multiPass = 0;
		const bool succeeded = Renderer::Instance().ApplyStereo(globals::d3d::device, context,
			total.texture, inputs, guideWidth, guideHeight,
			outWidth, outHeight, tuning, destination, destinationUAV, wantsEdgeBlend && destinationUAV != nullptr);
		if (succeeded) {
			if (stagedBlendTarget)
				context->CopyResource(total.texture, destination);
			lastAppliedFrame = frame;
			if (!writebackLogged) {
				const char* path = stagedBlendTarget ? "staged-uav" :
				                                       (destinationUAV ? "direct-uav" : "direct-copy");
				logger::info("[DLSSNR] LDR output written before UI composite size={}x{} edgeBlend={} mode={} path={} batchedAsync=true",
					outWidth, outHeight, wantsEdgeBlend && destinationUAV != nullptr,
					FoveatedRender::SubrectBlendModeName(blendMode), path);
				writebackLogged = true;
			}
		}

		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		for (auto*& rtv : savedRTVs)
			if (rtv)
				rtv->Release();
		if (savedDSV)
			savedDSV->Release();
		return succeeded;
	}

	void Reset()
	{
		Renderer::Instance().Reset();
		historyResetRequested.store(false, std::memory_order_release);
		temporalSuppressed = false;
		color[0].reset();
		color[1].reset();
		stereoBlendTarget.reset();
		colorWidth = colorHeight = 0;
		colorFormat = DXGI_FORMAT_UNKNOWN;
		stereoBlendWidth = stereoBlendHeight = 0;
		stereoBlendFormat = DXGI_FORMAT_UNKNOWN;
		lastAppliedFrame = UINT32_MAX;
		preUpscaleAppliedFrame = UINT32_MAX;
		preUpscaleModeObserved = false;
		preUpscaleMode = false;
		preUpscaleBlockLogged = false;
		preUpscaleSuccessLogged = false;
		preUpscaleExecutionFailed = false;
		writebackLogged = false;
		flatRouteWasActive = false;
		flatFrameGenerationBlockLogged = false;
		flatHdrBlockLogged = false;
		blendFallbackLogged = false;
	}
}
