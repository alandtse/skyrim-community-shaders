#include "VRSubmitUpscaling.h"

#include "Features/Upscaling.h"
#include "FoveatedRender/Ops.h"
#include "GpuPass.h"
#include "Utils/D3D.h"

#include <cmath>
#include <sl_matrix_helpers.h>

namespace
{
	struct PeripheryConstants
	{
		uint32_t width, height;
		float alpha;
		uint32_t padding = 0;
	};
}

void VRSubmitUpscaling::ClearFoveationResources()
{
	for (auto& eye : foveatedEyes)
		eye = {};
	peripheryShader.Reset();
	peripheryBuffer.reset();
	peripherySampler = nullptr;
	foveationUnavailable = false;
}

bool VRSubmitUpscaling::PrepareFoveation()
{
	auto& upscaling = globals::features::upscaling;
	auto& foveation = upscaling.foveatedRender;
	if (foveationUnavailable || !foveation.IsActive())
		return false;
	CS_GPU_PASS("Upscaling::SubmitFoveationPrepare");
	try {
		const auto validUV = [](const Util::Subrect::UVRegion& uv) {
			return std::isfinite(uv.x) && std::isfinite(uv.y) && std::isfinite(uv.w) && std::isfinite(uv.h) &&
			       uv.x >= 0 && uv.y >= 0 && uv.w > 0 && uv.h > 0 && uv.x + uv.w <= 1 && uv.y + uv.h <= 1;
		};
		if (!validUV(foveation.subrectController.GetUV()) || !validUV(foveation.subrectController.GetRightEyeUV()))
			return false;
		const auto input = foveation.subrectController.GetStereoPixelRegions(plan.renderWidth * 2, plan.renderHeight);
		const auto output = foveation.subrectController.GetStereoPixelRegions(plan.outputWidth * 2, plan.outputHeight);
		const Util::Subrect::PixelRegion inputs[] = { input.leftEye, input.rightEye };
		const Util::Subrect::PixelRegion outputs[] = { output.leftEye, output.rightEye };
		bool changed = false;
		for (uint32_t eye = 0; eye < 2; ++eye) {
			auto in = inputs[eye];
			in.w &= ~1u;
			in.h &= ~1u;
			const auto& out = outputs[eye];
			if (!in.w || !in.h || !out.w || !out.h || in.x + in.w > plan.renderWidth || in.y + in.h > plan.renderHeight ||
				out.x + out.w > plan.outputWidth || out.y + out.h > plan.outputHeight)
				return false;
			if (capturedMethod == uint32_t(Upscaling::UpscaleMethod::kDLSS)) {
				auto width = in.w, height = in.h;
				upscaling.streamline.ClampToDLSSRenderRange(plan.qualityMode, out.w, out.h, width, height);
				if (width != in.w || height != in.h)
					return false;
			}
			auto& resources = foveatedEyes[eye];
			const sl::Extent inputExtent{ in.y, in.x, in.w, in.h };
			const sl::Extent outputExtent{ out.y, out.x, out.w, out.h };
			changed |= resources.input != inputExtent || resources.output != outputExtent;
			if (!resources.crop.color || !resources.input.isSameRes(inputExtent) || !resources.output.isSameRes(outputExtent)) {
				EyeResources created;
				const auto name = std::format("Upscaling::SubmitFovea{}", eye);
				created.color = MakeTexture(in.w, in.h, DXGI_FORMAT_R16G16B16A16_FLOAT, name + " Color");
				created.output = MakeTexture(out.w, out.h, DXGI_FORMAT_R16G16B16A16_FLOAT, name + " Output");
				created.depth = MakeTexture(in.w, in.h, DXGI_FORMAT_R32_FLOAT, name + " Depth");
				created.motion = MakeTexture(in.w, in.h, DXGI_FORMAT_R16G16_FLOAT, name + " Motion");
				created.reactive = MakeTexture(in.w, in.h, DXGI_FORMAT_R8_UNORM, name + " Reactive");
				created.transparency = MakeTexture(in.w, in.h, DXGI_FORMAT_R8_UNORM, name + " Transparency");
				resources.crop = std::move(created);
			}
			resources.input = inputExtent;
			resources.output = outputExtent;
		}
		if (changed || !foveatedPair) {
			resetHistory = true;
			if (capturedMethod == uint32_t(Upscaling::UpscaleMethod::kDLSS))
				upscaling.streamline.DestroyDLSSResources();
		}
		for (uint32_t eye = 0; eye < 2; ++eye) {
			auto& resources = foveatedEyes[eye];
			const auto& in = resources.input;
			const D3D11_BOX box{ in.left, in.top, 0, in.left + in.width, in.top + in.height, 1 };
			const auto copy = [&](Texture2D* source, Texture2D* destination) {
				context->CopySubresourceRegion(destination->resource.get(), 0, 0, 0, 0, source->resource.get(), 0, &box);
			};
			copy(eyes[eye].color.get(), resources.crop.color.get());
			copy(eyes[eye].depth.get(), resources.crop.depth.get());
			copy(eyes[eye].motion.get(), resources.crop.motion.get());
			copy(eyes[eye].reactive.get(), resources.crop.reactive.get());
			copy(eyes[eye].transparency.get(), resources.crop.transparency.get());
			if (!FoveatedRenderImpl::Ops::StretchDRSToFullEye(SmoothPeriphery(eye), eyes[eye].output->uav.get(),
					0, plan.outputWidth, plan.outputHeight, 0, plan.renderWidth, plan.renderHeight, plan.renderWidth, plan.renderHeight)) {
				foveationUnavailable = true;
				resetHistory = true;
				logger::warn("[VRSubmit] Foveated stretch unavailable; using full-eye reconstruction until resource reset");
				return false;
			}
		}
		return true;
	} catch (...) {
		context->ClearState();
		foveationUnavailable = true;
		resetHistory = true;
		logger::warn("[VRSubmit] Foveated resources unavailable; using full-eye reconstruction until resource reset");
		return false;
	}
}

ID3D11ShaderResourceView* VRSubmitUpscaling::SmoothPeriphery(uint32_t eye)
{
	auto& settings = globals::features::upscaling.foveatedRender;
	auto& resources = foveatedEyes[eye];
	auto* current = eyes[eye].color.get();
	if (settings.GetPeripheryAAMode() != FoveatedRender::PeripheryAAMode::kTemporalSmooth) {
		resources.historyValid = false;
		return current->srv.get();
	}
	CS_GPU_PASS("Upscaling::SubmitPeripheryTemporal");
	if (!resources.history[0] || !resources.history[1]) {
		for (uint32_t index = 0; index < 2; ++index)
			resources.history[index] = MakeTexture(plan.renderWidth, plan.renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
				std::format("Upscaling::SubmitEye{} PeripheryHistory{}", eye, index));
		resources.historyValid = false;
	}
	const auto writeIndex = resources.historyIndex ^ 1u;
	auto* destination = resources.history[writeIndex].get();
	if (resetHistory || !resources.historyValid) {
		context->CopyResource(destination->resource.get(), current->resource.get());
	} else {
		auto* shader = peripheryShader.Get(L"Data/Shaders/Upscaling/FoveatedRender/PeripheryTemporalSmoothCS.hlsl",
			{ { "SINGLE_EYE", "" } }, "cs_5_0", "main", "Upscaling::SubmitPeripheryTemporal CS");
		if (!shader) {
			resources.historyValid = false;
			return current->srv.get();
		}
		if (!peripheryBuffer)
			peripheryBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PeripheryConstants>(), "Upscaling::SubmitPeriphery CB");
		if (!peripherySampler) {
			D3D11_SAMPLER_DESC desc{};
			desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
			desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
			winrt::check_hresult(globals::d3d::device->CreateSamplerState(&desc, peripherySampler.put()));
			Util::SetResourceName(peripherySampler.get(), "Upscaling::SubmitPeriphery Sampler");
		}
		peripheryBuffer->Update(PeripheryConstants{ plan.renderWidth, plan.renderHeight, settings.settings.peripheryTemporalAlpha });
		auto buffer = peripheryBuffer->CB();
		auto sampler = peripherySampler.get();
		auto output = destination->uav.get();
		ID3D11ShaderResourceView* inputs[] = { current->srv.get(), resources.history[resources.historyIndex]->srv.get(), eyes[eye].motion->srv.get() };
		context->CSSetShader(shader, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &buffer);
		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetShaderResources(0, 3, inputs);
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		context->Dispatch((plan.renderWidth + 7) / 8, (plan.renderHeight + 7) / 8, 1);
		context->ClearState();
	}
	resources.historyIndex = writeIndex;
	resources.historyValid = true;
	return destination->srv.get();
}

sl::Constants VRSubmitUpscaling::GetEyeConstants(uint32_t eye) const
{
	auto constants = cameraConstants[eye];
	if (!foveatedPair)
		return constants;
	const auto& in = foveatedEyes[eye].input;
	const float scaleX = float(plan.renderWidth) / in.width;
	const float scaleY = float(plan.renderHeight) / in.height;
	sl::float4x4 crop{};
	crop[0].x = scaleX;
	crop[1].y = scaleY;
	crop[2].z = crop[3].w = 1;
	crop[3].x = (float(plan.renderWidth) - 2.0f * in.left - in.width) / in.width;
	crop[3].y = (2.0f * in.top + in.height - float(plan.renderHeight)) / in.height;
	sl::float4x4 inverseCrop, projection, previousClip, transformed;
	sl::matrixFullInvert(inverseCrop, crop);
	sl::matrixMul(projection, constants.cameraViewToClip, crop);
	sl::matrixMul(previousClip, inverseCrop, constants.clipToPrevClip);
	sl::matrixMul(transformed, previousClip, crop);
	constants.cameraViewToClip = projection;
	sl::matrixFullInvert(constants.clipToCameraView, projection);
	constants.clipToPrevClip = transformed;
	sl::matrixFullInvert(constants.prevClipToClip, transformed);
	constants.mvecScale = { constants.mvecScale.x * scaleX, constants.mvecScale.y * scaleY };
	constants.cameraAspectRatio = float(in.width) / in.height;
	constants.cameraFOV = 2.0f * std::atan(std::tan(constants.cameraFOV * 0.5f) / scaleY);
	return constants;
}

void VRSubmitUpscaling::ComposeFoveatedEye(uint32_t eye)
{
	CS_GPU_PASS("Upscaling::SubmitFoveationCompose");
	auto& resources = foveatedEyes[eye];
	const auto& out = resources.output;
	if (!FoveatedRenderImpl::Ops::BlendSubrectToOutput(resources.crop.output->resource.get(), eyes[eye].output->resource.get(),
			eyes[eye].output->uav.get(), out.left, out.top, out.width, out.height)) {
		context->ClearState();
		const D3D11_BOX box{ 0, 0, 0, out.width, out.height, 1 };
		context->CopySubresourceRegion(eyes[eye].output->resource.get(), 0, out.left, out.top, 0, resources.crop.output->resource.get(), 0, &box);
	}
}
