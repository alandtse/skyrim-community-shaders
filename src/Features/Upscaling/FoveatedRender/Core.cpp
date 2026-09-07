#include "Core.h"
#include "Ops.h"

#include "../../../GpuPass.h"
#include "../../../State.h"
#include "../../../Util.h"
#include "../../../Utils/D3D.h"
#include "../../Upscaling.h"
#include "../FoveatedRender.h"
#include "../NeuralRendering/Integration.h"

#include <cstring>

namespace FoveatedRenderImpl::Ops
{
	namespace
	{
		struct DepthCopyConstants
		{
			uint32_t sourceOffsetX = 0;
			uint32_t sourceOffsetY = 0;
			uint32_t width = 0;
			uint32_t height = 0;
		};
		static_assert(sizeof(DepthCopyConstants) == 16);

		ID3D11ShaderResourceView* ResolveDepthSRV(
			ID3D11Resource* source,
			ID3D11ShaderResourceView* sourceSRV,
			winrt::com_ptr<ID3D11ShaderResourceView>& ownedSRV)
		{
			if (sourceSRV)
				return sourceSRV;
			if (!source || !globals::d3d::device)
				return nullptr;

			// The engine already exposes the correctly typed view for its depth
			// resource. Reuse it instead of guessing the view format.
			if (auto* renderer = globals::game::renderer) {
				auto& depth = renderer->GetDepthStencilData()
				                  .depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
				if (depth.texture == source && depth.depthSRV)
					return depth.depthSRV;
			}

			winrt::com_ptr<ID3D11Texture2D> texture;
			if (FAILED(source->QueryInterface(IID_PPV_ARGS(texture.put()))))
				return nullptr;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);

			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			viewDesc.Texture2D.MostDetailedMip = 0;
			viewDesc.Texture2D.MipLevels = 1;
			switch (desc.Format) {
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
				viewDesc.Format = DXGI_FORMAT_R32_FLOAT;
				break;
			case DXGI_FORMAT_R24G8_TYPELESS:
				viewDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
				break;
			case DXGI_FORMAT_R32G8X24_TYPELESS:
				viewDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
				break;
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
				viewDesc.Format = DXGI_FORMAT_R16_UNORM;
				break;
			default:
				viewDesc.Format = desc.Format;
				break;
			}
			if (FAILED(globals::d3d::device->CreateShaderResourceView(source, &viewDesc, ownedSRV.put())))
				return nullptr;
			return ownedSRV.get();
		}

		bool EnsureDepthCopyResources()
		{
			if (!globals::d3d::device)
				return false;
			if (!Core::vrDepthCopyCS) {
				Core::vrDepthCopyCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
					L"Data\\Shaders\\Upscaling\\FoveatedRender\\DepthToPerEyeCS.hlsl", {}, "cs_5_0")));
				if (!Core::vrDepthCopyCS)
					return false;
				Util::SetResourceName(Core::vrDepthCopyCS.get(), "FoveatedRender::DepthToPerEyeCS");
			}
			if (!Core::vrDepthCopyCB) {
				D3D11_BUFFER_DESC cbDesc{};
				cbDesc.ByteWidth = sizeof(DepthCopyConstants);
				cbDesc.Usage = D3D11_USAGE_DYNAMIC;
				cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (FAILED(globals::d3d::device->CreateBuffer(&cbDesc, nullptr, Core::vrDepthCopyCB.put()))) {
					logger::error("[FOVEATED] Failed to create DepthToPerEye constant buffer");
					return false;
				}
				Util::SetResourceName(Core::vrDepthCopyCB.get(), "FoveatedRender::DepthToPerEyeCB");
			}
			return true;
		}
	}

	bool CopyDepthRegionToTexture(
		ID3D11Resource* source,
		ID3D11ShaderResourceView* sourceSRV,
		ID3D11UnorderedAccessView* destinationUAV,
		uint32_t sourceOffsetX,
		uint32_t sourceOffsetY,
		uint32_t width,
		uint32_t height)
	{
		auto* context = globals::d3d::context;
		if (!source || !context || !destinationUAV || width == 0 || height == 0 || !EnsureDepthCopyResources())
			return false;

		winrt::com_ptr<ID3D11Texture2D> sourceTexture;
		if (FAILED(source->QueryInterface(IID_PPV_ARGS(sourceTexture.put()))))
			return false;
		D3D11_TEXTURE2D_DESC sourceDesc{};
		sourceTexture->GetDesc(&sourceDesc);
		if (sourceDesc.ArraySize != 1 || sourceDesc.MipLevels != 1 || sourceDesc.SampleDesc.Count != 1 ||
			sourceOffsetX > sourceDesc.Width || width > sourceDesc.Width - sourceOffsetX ||
			sourceOffsetY > sourceDesc.Height || height > sourceDesc.Height - sourceOffsetY)
			return false;

		winrt::com_ptr<ID3D11ShaderResourceView> ownedSRV;
		auto* resolvedSRV = ResolveDepthSRV(source, sourceSRV, ownedSRV);
		if (!resolvedSRV)
			return false;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(Core::vrDepthCopyCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
			return false;
		const DepthCopyConstants constants{ sourceOffsetX, sourceOffsetY, width, height };
		std::memcpy(mapped.pData, &constants, sizeof(constants));
		context->Unmap(Core::vrDepthCopyCB.get(), 0);

		CS_GPU_PASS("FoveatedRender::DepthToPerEye");
		context->CSSetShader(Core::vrDepthCopyCS.get(), nullptr, 0);
		ID3D11Buffer* cbs[1]{ Core::vrDepthCopyCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);
		ID3D11ShaderResourceView* srvs[1]{ resolvedSRV };
		context->CSSetShaderResources(0, 1, srvs);
		ID3D11UnorderedAccessView* uavs[1]{ destinationUAV };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRV[1]{};
		ID3D11UnorderedAccessView* nullUAV[1]{};
		ID3D11Buffer* nullCB[1]{};
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
		return true;
	}

	// Mirrors the StretchCB layout in SubrectStretchCS.hlsl — 8 dims + mode +
	// blur radius + debug flag + pad. Kept at namespace scope so the create-CB
	// path can size against sizeof(StretchCB) instead of a magic number.
	struct StretchCB
	{
		uint32_t data[8];
		uint32_t stretchMode;
		float blurRadius;
		uint32_t debugVisualize;
		uint32_t pad;
	};

	eastl::unique_ptr<Texture2D> CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
		bool copyBindFlags, bool createSRV, bool createUAV, const char* name)
	{
		if (!src) {
			logger::error("[FOVEATED] CreateTextureFromSource called with null src ({})", name ? name : "<unnamed>");
			return nullptr;
		}

		// QueryInterface for ID3D11Texture2D rather than blind static_cast — a
		// non-texture resource passed here would crash GetDesc otherwise.
		winrt::com_ptr<ID3D11Texture2D> srcTex;
		if (FAILED(src->QueryInterface(IID_PPV_ARGS(srcTex.put())))) {
			logger::error("[FOVEATED] CreateTextureFromSource src is not an ID3D11Texture2D ({})", name ? name : "<unnamed>");
			return nullptr;
		}

		D3D11_TEXTURE2D_DESC srcDesc;
		srcTex->GetDesc(&srcDesc);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = srcDesc.Format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = copyBindFlags ? srcDesc.BindFlags : (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);

		auto tex = eastl::make_unique<Texture2D>(desc);

		if (name) {
			Util::SetResourceName(tex->resource.get(), name);
		}

		if (createSRV) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = srcDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;
			tex->CreateSRV(srvDesc);
		}

		if (createUAV) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = srcDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			tex->CreateUAV(uavDesc);
		}

		return tex;
	}

	void EnsureVRIntermediateTextures(
		uint32_t inWidth,
		uint32_t inHeight,
		uint32_t outWidth,
		uint32_t outHeight,
		ID3D11Resource* colorSrc,
		ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc,
		ID3D11Resource* transparencySrc)
	{
		bool needsRecreate = !Core::vrIntermediateColorIn[0] || !Core::vrIntermediateColorOut[0];
		if (!needsRecreate) {
			needsRecreate = (Core::vrIntermediateColorIn[0]->desc.Width != inWidth ||
							 Core::vrIntermediateColorIn[0]->desc.Height != inHeight ||
							 Core::vrIntermediateColorOut[0]->desc.Width != outWidth ||
							 Core::vrIntermediateColorOut[0]->desc.Height != outHeight);
		}
		// Recreate if reactive/transparency source appeared but intermediate is missing
		if (!needsRecreate) {
			needsRecreate = (reactiveSrc && !Core::vrIntermediateReactiveMask[0]) ||
			                (transparencySrc && !Core::vrIntermediateTransparencyMask[0]);
		}

		// Also reset stale intermediates when a source DISAPPEARED. Otherwise
		// the per-eye reactive/transparency intermediates keep their last-known
		// data and PreparePerEyeInputs's null-source branch skips the copy —
		// DLSS then samples stale masks. Drop the intermediate so subsequent
		// frames don't read it. Independent of the recreate path: shrinking is
		// cheap and the next non-null source will trigger recreate above.
		if (!reactiveSrc && Core::vrIntermediateReactiveMask[0]) {
			Core::vrIntermediateReactiveMask[0].reset();
			Core::vrIntermediateReactiveMask[1].reset();
		}
		if (!transparencySrc && Core::vrIntermediateTransparencyMask[0]) {
			Core::vrIntermediateTransparencyMask[0].reset();
			Core::vrIntermediateTransparencyMask[1].reset();
		}

		if (!needsRecreate) {
			return;
		}

		for (int i = 0; i < 2; i++) {
			std::string suffix = (i == 0) ? "Left" : "Right";

			Core::vrIntermediateColorIn[i] = CreateTextureFromSource(colorSrc, inWidth, inHeight, false, true, true, ("FoveatedRender_ColorIn_" + suffix).c_str());
			Core::vrIntermediateColorOut[i] = CreateTextureFromSource(colorSrc, outWidth, outHeight, false, true, false, ("FoveatedRender_ColorOut_" + suffix).c_str());

			D3D11_TEXTURE2D_DESC depthDesc = {};
			depthDesc.Width = inWidth;
			depthDesc.Height = inHeight;
			depthDesc.MipLevels = 1;
			depthDesc.ArraySize = 1;
			depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			depthDesc.SampleDesc.Count = 1;
			depthDesc.Usage = D3D11_USAGE_DEFAULT;
			depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			Core::vrIntermediateDepth[i] = eastl::make_unique<Texture2D>(depthDesc);
			Util::SetResourceName(Core::vrIntermediateDepth[i]->resource.get(), ("FoveatedRender_Depth_" + suffix).c_str());

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			Core::vrIntermediateDepth[i]->CreateSRV(srvDesc);

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			Core::vrIntermediateDepth[i]->CreateUAV(uavDesc);

			Core::vrIntermediateMotionVectors[i] = CreateTextureFromSource(mvecSrc, inWidth, inHeight, false, true, false, ("FoveatedRender_MVec_" + suffix).c_str());
			if (reactiveSrc)
				Core::vrIntermediateReactiveMask[i] = CreateTextureFromSource(reactiveSrc, inWidth, inHeight, false, true, false, ("FoveatedRender_Reactive_" + suffix).c_str());
			else
				Core::vrIntermediateReactiveMask[i].reset();
			if (transparencySrc)
				Core::vrIntermediateTransparencyMask[i] = CreateTextureFromSource(transparencySrc, inWidth, inHeight, false, true, false, ("FoveatedRender_Transparency_" + suffix).c_str());
			else
				Core::vrIntermediateTransparencyMask[i].reset();
		}
	}

	void EnsureVRSubrectTextures(
		uint32_t subInW,
		uint32_t subInH,
		uint32_t subOutW,
		uint32_t subOutH,
		ID3D11Resource* colorSrc,
		ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc,
		ID3D11Resource* transparencySrc)
	{
		bool needsRecreate = !Core::vrSubrectColorIn[0] ||
		                     Core::vrSubrectInW != subInW || Core::vrSubrectInH != subInH ||
		                     Core::vrSubrectOutW != subOutW || Core::vrSubrectOutH != subOutH;
		// Recreate if reactive/transparency source appeared but intermediate is missing
		if (!needsRecreate) {
			needsRecreate = (reactiveSrc && !Core::vrSubrectReactiveMask[0]) ||
			                (transparencySrc && !Core::vrSubrectTransparencyMask[0]);
		}

		if (needsRecreate) {
			for (int i = 0; i < 2; i++) {
				std::string suffix = (i == 0) ? "Left" : "Right";
				Core::vrSubrectColorIn[i] = CreateTextureFromSource(colorSrc, subInW, subInH, false, true, true, ("FoveatedRender_Subrect_ColorIn_" + suffix).c_str());
				Core::vrSubrectColorOut[i] = CreateTextureFromSource(colorSrc, subOutW, subOutH, false, true, false, ("FoveatedRender_Subrect_ColorOut_" + suffix).c_str());

				D3D11_TEXTURE2D_DESC depthDesc = {};
				depthDesc.Width = subInW;
				depthDesc.Height = subInH;
				depthDesc.MipLevels = 1;
				depthDesc.ArraySize = 1;
				depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
				depthDesc.SampleDesc.Count = 1;
				depthDesc.Usage = D3D11_USAGE_DEFAULT;
				depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				Core::vrSubrectDepth[i] = eastl::make_unique<Texture2D>(depthDesc);
				Util::SetResourceName(Core::vrSubrectDepth[i]->resource.get(), ("FoveatedRender_Subrect_Depth_" + suffix).c_str());

				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = 1;
				Core::vrSubrectDepth[i]->CreateSRV(srvDesc);

				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
				uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				uavDesc.Texture2D.MipSlice = 0;
				Core::vrSubrectDepth[i]->CreateUAV(uavDesc);

				Core::vrSubrectMotionVectors[i] = CreateTextureFromSource(mvecSrc, subInW, subInH, false, true, false, ("FoveatedRender_Subrect_MVec_" + suffix).c_str());
				if (reactiveSrc)
					Core::vrSubrectReactiveMask[i] = CreateTextureFromSource(reactiveSrc, subInW, subInH, false, true, false, ("FoveatedRender_Subrect_Reactive_" + suffix).c_str());
				else
					Core::vrSubrectReactiveMask[i].reset();
				if (transparencySrc)
					Core::vrSubrectTransparencyMask[i] = CreateTextureFromSource(transparencySrc, subInW, subInH, false, true, false, ("FoveatedRender_Subrect_Transparency_" + suffix).c_str());
				else
					Core::vrSubrectTransparencyMask[i].reset();
			}

			Core::vrSubrectInW = subInW;
			Core::vrSubrectInH = subInH;
			Core::vrSubrectOutW = subOutW;
			Core::vrSubrectOutH = subOutH;
		}
	}

	bool PreparePerEyeInputs(
		ID3D11Resource* colorSrc,
		ID3D11Resource* depthSrc,
		ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc,
		ID3D11Resource* transparencySrc,
		uint32_t eyeWidthIn,
		uint32_t eyeHeightIn,
		uint32_t eyeWidthOut,
		uint32_t eyeHeightOut)
	{
		// Required sources are dereferenced unconditionally below; bail
		// rather than null-deref CopySubresourceRegion. Reactive/transparency
		// are optional and already conditionally copied.
		if (!colorSrc || !depthSrc || !mvecSrc) {
			logger::error("[FOVEATED] PreparePerEyeInputs missing required source textures");
			return false;
		}

		EnsureVRIntermediateTextures(
			eyeWidthIn,
			eyeHeightIn,
			eyeWidthOut,
			eyeHeightOut,
			colorSrc,
			mvecSrc,
			reactiveSrc,
			transparencySrc);

		for (uint32_t i = 0; i < 2; ++i) {
			if (!Core::vrIntermediateColorIn[i] || !Core::vrIntermediateColorOut[i] ||
				!Core::vrIntermediateDepth[i] || !Core::vrIntermediateMotionVectors[i] ||
				(reactiveSrc && !Core::vrIntermediateReactiveMask[i]) ||
				(transparencySrc && !Core::vrIntermediateTransparencyMask[i])) {
				logger::error("[FOVEATED] Missing per-eye intermediate resources for eye {}", i);
				return false;
			}
		}

		auto context = globals::d3d::context;
		auto* depthSRV = globals::game::renderer->GetDepthStencilData()
		                     .depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN]
		                     .depthSRV;
		if (!depthSRV)
			return false;
		for (uint32_t i = 0; i < 2; ++i) {
			uint32_t offsetXIn = (i == 1) ? eyeWidthIn : 0;
			D3D11_BOX srcBox = { offsetXIn, 0, 0, offsetXIn + eyeWidthIn, eyeHeightIn, 1 };

			context->CopySubresourceRegion(Core::vrIntermediateColorIn[i]->resource.get(), 0, 0, 0, 0, colorSrc, 0, &srcBox);
			if (!CopyDepthRegionToTexture(depthSrc, nullptr, Core::vrIntermediateDepth[i]->uav.get(),
					offsetXIn, 0, eyeWidthIn, eyeHeightIn)) {
				logger::error("[FOVEATED] Failed to convert native depth for eye {}", i);
				return false;
			}
			context->CopySubresourceRegion(Core::vrIntermediateMotionVectors[i]->resource.get(), 0, 0, 0, 0, mvecSrc, 0, &srcBox);
			if (transparencySrc)
				context->CopySubresourceRegion(Core::vrIntermediateTransparencyMask[i]->resource.get(), 0, 0, 0, 0, transparencySrc, 0, &srcBox);
			if (reactiveSrc)
				context->CopySubresourceRegion(Core::vrIntermediateReactiveMask[i]->resource.get(), 0, 0, 0, 0, reactiveSrc, 0, &srcBox);

			// Reapply HMD hidden-area mask clear into the per-eye intermediate so DLSS
			// history doesn't accumulate garbage from the masked-out region.
			// Depth source is full SBS (read at per-eye offset); color destination is per-eye
			// sized (write at offset 0).
			globals::features::upscaling.ClearHMDMask(
				Core::vrIntermediateColorIn[i]->uav.get(),
				depthSRV,
				eyeWidthIn,
				eyeHeightIn,
				i * eyeWidthIn,
				0);
		}

		return true;
	}

	bool FinalizePerEyeOutputs(ID3D11Resource* colorDst, uint32_t eyeWidthOut, uint32_t eyeHeightOut)
	{
		if (!colorDst) {
			logger::error("[FOVEATED] FinalizePerEyeOutputs received null destination color resource");
			return false;
		}

		for (uint32_t i = 0; i < 2; ++i) {
			if (!Core::vrIntermediateColorOut[i]) {
				logger::error("[FOVEATED] Missing per-eye output resource for eye {}", i);
				return false;
			}
		}

		auto context = globals::d3d::context;
		for (uint32_t i = 0; i < 2; ++i) {
			uint32_t offsetXOut = (i == 1) ? eyeWidthOut : 0;
			D3D11_BOX outBox = { 0, 0, 0, eyeWidthOut, eyeHeightOut, 1 };
			context->CopySubresourceRegion(colorDst, 0, offsetXOut, 0, 0, Core::vrIntermediateColorOut[i]->resource.get(), 0, &outBox);
		}

		return true;
	}

	void StretchDRSToFullEye(
		ID3D11ShaderResourceView* renderSBSSRV,
		ID3D11UnorderedAccessView* kMainUAV,
		uint32_t dstOffsetX,
		uint32_t dstWidth,
		uint32_t dstHeight,
		uint32_t srcOffsetX,
		uint32_t srcWidth,
		uint32_t srcHeight,
		uint32_t srcEyeWidth,
		uint32_t srcEyeHeight)
	{
		auto context = globals::d3d::context;

		if (!Core::vrSubrectStretchCS) {
			Core::vrSubrectStretchCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/FoveatedRender/SubrectStretchCS.hlsl", {}, "cs_5_0"));
			Util::SetResourceName(Core::vrSubrectStretchCS.get(), "FoveatedRender::SubrectStretchCS");

			D3D11_BUFFER_DESC cbDesc = {};
			cbDesc.ByteWidth = sizeof(StretchCB);
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(globals::d3d::device->CreateBuffer(&cbDesc, nullptr, Core::vrSubrectStretchCB.put()))) {
				logger::error("[FOVEATED] Failed to create SubrectStretch constant buffer");
				// Drop the partially-attached CS so the next frame retries the
				// whole init block — otherwise the outer !vrSubrectStretchCS
				// guard above stays false forever and Faster mode is dead for
				// the rest of the session.
				Core::vrSubrectStretchCS = nullptr;
				return;
			}
			Util::SetResourceName(Core::vrSubrectStretchCB.get(), "FoveatedRender::SubrectStretchCB");

			D3D11_SAMPLER_DESC sampDesc = {};
			sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
			sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			if (FAILED(globals::d3d::device->CreateSamplerState(&sampDesc, Core::vrSubrectStretchSampler.put()))) {
				logger::error("[FOVEATED] Failed to create SubrectStretch sampler");
				Core::vrSubrectStretchCS = nullptr;
				Core::vrSubrectStretchCB = nullptr;
				return;
			}
			Util::SetResourceName(Core::vrSubrectStretchSampler.get(), "FoveatedRender::SubrectStretchSampler");
		}

		if (!Core::vrSubrectStretchCS || !Core::vrSubrectStretchCB || !Core::vrSubrectStretchSampler) {
			return;
		}

		// Guard against a null destination UAV — CSSetUnorderedAccessViews +
		// Dispatch with nullptr would either no-op silently or assert in
		// debug builds. Returning lets the route's `routeHandled=false` path
		// fall back to standard DLSS so users still see output.
		if (!kMainUAV) {
			logger::error("[FOVEATED] StretchDRSToFullEye called with null kMainUAV");
			return;
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};
		// If Map fails the constant buffer keeps stale data from a prior
		// dispatch. CSSetConstantBuffers + Dispatch would then run the
		// shader against stale geometry/scale parameters, producing wrong
		// pixels rather than no pixels. Early-return preserves the prior
		// frame's output.
		if (FAILED(context->Map(Core::vrSubrectStretchCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			logger::error("[FOVEATED] StretchDRSToFullEye Map(vrSubrectStretchCB) failed; skipping dispatch");
			return;
		}
		{
			auto& enhSettings = globals::features::upscaling.foveatedRender.settings;
			StretchCB cb = {};
			cb.data[0] = dstOffsetX;
			cb.data[1] = dstWidth;
			cb.data[2] = dstHeight;
			cb.data[3] = srcOffsetX;
			cb.data[4] = srcWidth;
			cb.data[5] = srcHeight;
			cb.data[6] = srcEyeWidth;
			cb.data[7] = srcEyeHeight;
			cb.stretchMode = enhSettings.stretchMode;
			// BlurRadius is in SOURCE-texel units, scaled onto screen pixels by
			// r = dstWidth/srcEyeWidth. Floor r at kMinPeripheryBlurRatio so the
			// periphery is never blurred less on-screen than at that reference
			// scale -- only boosts the radius near-native, never reduces real DRS blur.
			constexpr float kMinPeripheryBlurRatio = 2.0f;  // reference: never softer than a 50%-render-scale stretch
			const float r = srcEyeWidth > 0 ? (float)dstWidth / (float)srcEyeWidth : 1.0f;
			cb.blurRadius = enhSettings.peripheryBlurRadius * (std::max(r, kMinPeripheryBlurRatio) / r);
			cb.debugVisualize = (enhSettings.debugVisualize != 0 || globals::features::upscaling.foveatedRender.ShouldForceVisualize()) ? 1u : 0u;
			std::memcpy(mapped.pData, &cb, sizeof(cb));
			context->Unmap(Core::vrSubrectStretchCB.get(), 0);
		}

		context->CSSetShader(Core::vrSubrectStretchCS.get(), nullptr, 0);
		ID3D11Buffer* cbs[1] = { Core::vrSubrectStretchCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);
		ID3D11ShaderResourceView* srvs[1] = { renderSBSSRV };
		context->CSSetShaderResources(0, 1, srvs);
		ID3D11SamplerState* samplers[1] = { Core::vrSubrectStretchSampler.get() };
		context->CSSetSamplers(0, 1, samplers);
		ID3D11UnorderedAccessView* uavs[1] = { kMainUAV };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		context->Dispatch((dstWidth + 7) / 8, (dstHeight + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		ID3D11SamplerState* nullSampler[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetSamplers(0, 1, nullSampler);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	void EnsureVRRenderSBS(uint32_t renderW, uint32_t renderH, ID3D11Resource* colorSrc)
	{
		if (!Core::vrRenderSBS || Core::vrRenderSBSW != renderW || Core::vrRenderSBSH != renderH) {
			// UAV is required for the Faster-mode HMD mask clear pass (ClearHMDMask
			// writes through the UAV before DLSS reads the SBS via extent offsets).
			Core::vrRenderSBS = CreateTextureFromSource(colorSrc, renderW, renderH, false, true, true, "FoveatedRender_RenderSBS");
			Core::vrRenderSBSW = renderW;
			Core::vrRenderSBSH = renderH;
		}
	}

	void EnsureFasterOutputTextures(uint32_t subOutW, uint32_t subOutH, ID3D11Resource* colorSrc)
	{
		bool needsRecreate = !Core::vrFasterColorOut[0] ||
		                     Core::vrFasterOutW != subOutW || Core::vrFasterOutH != subOutH;
		if (!needsRecreate)
			return;
		for (int i = 0; i < 2; i++) {
			std::string suffix = (i == 0) ? "Left" : "Right";
			Core::vrFasterColorOut[i] = CreateTextureFromSource(colorSrc, subOutW, subOutH, false, true, false, ("FoveatedRender_Faster_ColorOut_" + suffix).c_str());
		}
		Core::vrFasterOutW = subOutW;
		Core::vrFasterOutH = subOutH;
	}

	// ── Periphery Temporal Smooth ──
	// Ping-pong between two history buffers at render-res SBS.
	// Blends current frame with motion-reprojected history to reduce flicker
	// in the stretched periphery region.
	void EnsureTemporalResources(uint32_t renderW, uint32_t renderH, ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc)
	{
		if (!Core::vrTemporalHistory[0] || Core::vrTemporalHistoryW != renderW || Core::vrTemporalHistoryH != renderH) {
			for (int i = 0; i < 2; i++) {
				Core::vrTemporalHistory[i] = CreateTextureFromSource(colorSrc, renderW, renderH, false, true, true,
					i == 0 ? "FoveatedRender::TemporalHistA" : "FoveatedRender::TemporalHistB");
			}
			Core::vrTemporalHistoryW = renderW;
			Core::vrTemporalHistoryH = renderH;
			Core::vrTemporalHistoryValid = false;
			Core::vrTemporalFrameIdx = 0;
			Core::vrMvecSRV = nullptr;
			Core::vrMvecSRVOwner = nullptr;
		}

		// Cache an SRV on the game's mvec resource (no copy needed)
		if (Core::vrMvecSRVOwner != mvecSrc) {
			Core::vrMvecSRV = nullptr;
			winrt::com_ptr<ID3D11Texture2D> mvecTex;
			if (FAILED(mvecSrc->QueryInterface(IID_PPV_ARGS(mvecTex.put())))) {
				logger::error("[FOVEATED] EnsureTemporalResources: mvecSrc is not an ID3D11Texture2D");
				return;
			}
			D3D11_TEXTURE2D_DESC desc;
			mvecTex->GetDesc(&desc);
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;
			if (FAILED(globals::d3d::device->CreateShaderResourceView(mvecSrc, &srvDesc, Core::vrMvecSRV.put()))) {
				logger::error("[FOVEATED] Failed to create SRV on mvec resource for temporal smooth");
				return;
			}
			Util::SetResourceName(Core::vrMvecSRV.get(), "FoveatedRender::MVecSRV");
			Core::vrMvecSRVOwner = mvecSrc;
		}
	}

	ID3D11ShaderResourceView* TemporalSmoothSBS(uint32_t renderW, uint32_t renderH)
	{
		if (!Core::vrRenderSBS || !Core::vrRenderSBS->srv)
			return nullptr;

		auto context = globals::d3d::context;

		uint32_t readIdx = Core::vrTemporalFrameIdx & 1;
		uint32_t writeIdx = readIdx ^ 1;

		if (!Core::vrTemporalHistoryValid) {
			// First frame: copy snapshot as seed history
			context->CopyResource(Core::vrTemporalHistory[writeIdx]->resource.get(), Core::vrRenderSBS->resource.get());
			Core::vrTemporalHistoryValid = true;
			Core::vrTemporalFrameIdx++;
			return Core::vrTemporalHistory[writeIdx]->srv.get();
		}

		// Lazy-create CS resources
		if (!Core::vrTemporalSmoothCS) {
			Core::vrTemporalSmoothCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
				L"Data/Shaders/Upscaling/FoveatedRender/PeripheryTemporalSmoothCS.hlsl", {}, "cs_5_0")));
			Util::SetResourceName(Core::vrTemporalSmoothCS.get(), "FoveatedRender::TemporalSmoothCS");

			D3D11_BUFFER_DESC cbDesc = {};
			cbDesc.ByteWidth = 16;  // 2 uint + 1 float + 1 uint pad
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(globals::d3d::device->CreateBuffer(&cbDesc, nullptr, Core::vrTemporalSmoothCB.put()))) {
				logger::error("[FOVEATED] Failed to create TemporalSmooth constant buffer");
				return Core::vrRenderSBS->srv.get();
			}
			Util::SetResourceName(Core::vrTemporalSmoothCB.get(), "FoveatedRender::TemporalSmoothCB");

			D3D11_SAMPLER_DESC sampDesc = {};
			sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
			sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			if (FAILED(globals::d3d::device->CreateSamplerState(&sampDesc, Core::vrTemporalSmoothSampler.put()))) {
				logger::error("[FOVEATED] Failed to create TemporalSmooth sampler");
				return Core::vrRenderSBS->srv.get();
			}
			Util::SetResourceName(Core::vrTemporalSmoothSampler.get(), "FoveatedRender::TemporalSmoothSampler");
		}

		if (!Core::vrTemporalSmoothCS || !Core::vrTemporalSmoothCB || !Core::vrTemporalSmoothSampler)
			return Core::vrRenderSBS->srv.get();

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(Core::vrTemporalSmoothCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			struct
			{
				uint32_t w;
				uint32_t h;
				float alpha;
				uint32_t pad;
			} cb;
			cb.w = renderW;
			cb.h = renderH;
			cb.alpha = globals::features::upscaling.foveatedRender.settings.peripheryTemporalAlpha;
			cb.pad = 0;
			std::memcpy(mapped.pData, &cb, sizeof(cb));
			context->Unmap(Core::vrTemporalSmoothCB.get(), 0);
		}

		context->CSSetShader(Core::vrTemporalSmoothCS.get(), nullptr, 0);
		ID3D11Buffer* cbs[] = { Core::vrTemporalSmoothCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);
		ID3D11ShaderResourceView* srvs[] = {
			Core::vrRenderSBS->srv.get(),
			Core::vrTemporalHistory[readIdx]->srv.get(),
			Core::vrMvecSRV.get()
		};
		context->CSSetShaderResources(0, 3, srvs);
		ID3D11SamplerState* samplers[] = { Core::vrTemporalSmoothSampler.get() };
		context->CSSetSamplers(0, 1, samplers);
		ID3D11UnorderedAccessView* uavs[] = { Core::vrTemporalHistory[writeIdx]->uav.get() };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		context->Dispatch((renderW + 7) / 8, (renderH + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRVs[3] = {};
		ID3D11UnorderedAccessView* nullUAV[1] = {};
		ID3D11Buffer* nullCB[1] = {};
		ID3D11SamplerState* nullSampler[1] = {};
		context->CSSetShaderResources(0, 3, nullSRVs);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetSamplers(0, 1, nullSampler);
		context->CSSetShader(nullptr, nullptr, 0);

		Core::vrTemporalFrameIdx++;
		return Core::vrTemporalHistory[writeIdx]->srv.get();
	}

	ID3D11ShaderResourceView* MaybeTemporalSmooth(const VRDlssParams& p)
	{
		if (globals::features::upscaling.foveatedRender.GetPeripheryAAMode() == FoveatedRender::PeripheryAAMode::kTemporalSmooth) {
			EnsureTemporalResources(p.renderW, p.renderH, p.colorSrc, p.motionVectors);
			// Bail if the mvec SRV couldn't be created (EnsureTemporalResources logs
			// the failure). TemporalSmoothSBS would otherwise dispatch the CS with a
			// null SRV bound at t2, reading undefined data.
			if (!Core::vrMvecSRV)
				return nullptr;
			return TemporalSmoothSBS(p.renderW, p.renderH);
		}
		return nullptr;
	}

	void ClearHMDMaskOnSnapshot(const VRDlssParams& p)
	{
		auto* depthSRV = globals::game::renderer->GetDepthStencilData()
		                     .depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN]
		                     .depthSRV;
		if (!Core::vrRenderSBS || !Core::vrRenderSBS->uav || !depthSRV)
			return;
		auto& upscaling = globals::features::upscaling;
		for (uint32_t i = 0; i < 2; ++i) {
			const uint32_t eyeOffsetX = i * p.eyeWidthIn;
			upscaling.ClearHMDMask(Core::vrRenderSBS->uav.get(), depthSRV,
				p.eyeWidthIn, p.eyeHeightIn, eyeOffsetX, eyeOffsetX);
		}
	}

	// ── Subrect Blend (feathered / dithered copy-back) ──

	struct alignas(16) BlendCB
	{
		uint32_t DstOffsetX;
		uint32_t DstOffsetY;
		uint32_t SubWidth;
		uint32_t SubHeight;
		uint32_t BlendMode;
		uint32_t MaskMode;
		uint32_t FrameIndex;
		uint32_t SrcOffsetX;
		float FeatherWidth;
		float DitherStrength;
		float FalloffCurve;
		float MaskCenterX;
		float MaskCenterY;
		float MaskRadiusX;
		float MaskRadiusY;
		float _pad0;
	};
	static_assert(sizeof(BlendCB) == 64);

	uint64_t ComputeSubrectUVHash(const Util::Subrect::UVRegion& leftUV,
		const Util::Subrect::UVRegion& rightUV, uint32_t mode)
	{
		uint64_t h = 0;
		auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 12) + (h >> 4); };
		auto mixUV = [&](const Util::Subrect::UVRegion& uv) {
			mix(std::hash<float>{}(uv.x));
			mix(std::hash<float>{}(uv.y));
			mix(std::hash<float>{}(uv.w));
			mix(std::hash<float>{}(uv.h));
		};
		mixUV(leftUV);
		mixUV(rightUV);
		mix(std::hash<uint32_t>{}(mode));
		return h;
	}

	void SnapshotSBS(ID3D11Resource* src, uint32_t renderW, uint32_t renderH)
	{
		EnsureVRRenderSBS(renderW, renderH, src);
		auto context = globals::d3d::context;
		D3D11_BOX drsBox = { 0, 0, 0, renderW, renderH, 1 };
		context->CopySubresourceRegion(Core::vrRenderSBS->resource.get(), 0, 0, 0, 0, src, 0, &drsBox);
	}

	void StretchDRSBothEyes(ID3D11UnorderedAccessView* dstUAV, uint32_t eyeWidthOut, uint32_t eyeHeightOut,
		uint32_t eyeWidthIn, uint32_t eyeHeightIn, uint32_t renderW, uint32_t renderH,
		ID3D11ShaderResourceView* srcOverride)
	{
		// Snapshot creation can fail or be skipped on a fresh frame; degrade
		// rather than dereference vrRenderSBS->srv on the null path.
		auto* src = srcOverride ? srcOverride :
		                          (Core::vrRenderSBS ? Core::vrRenderSBS->srv.get() : nullptr);
		if (!src) {
			logger::error("[FOVEATED] StretchDRSBothEyes missing source SRV");
			return;
		}
		for (uint32_t i = 0; i < 2; ++i) {
			uint32_t dstX = (i == 1) ? eyeWidthOut : 0;
			uint32_t srcX = (i == 1) ? eyeWidthIn : 0;
			StretchDRSToFullEye(
				src, dstUAV,
				dstX, eyeWidthOut, eyeHeightOut,
				srcX, renderW, renderH,
				eyeWidthIn, eyeHeightIn);
		}
	}

	void BlendSubrectToOutput(ID3D11Resource* dlssSrc, ID3D11Resource* dst, ID3D11UnorderedAccessView* dstUAV,
		uint32_t dstOffsetX, uint32_t dstOffsetY, uint32_t subWidth, uint32_t subHeight, uint32_t srcOffsetX)
	{
		auto context = globals::d3d::context;
		auto& foveated = globals::features::upscaling.foveatedRender;
		auto blendMode = foveated.GetSubrectBlendMode();

		// Fast path: hard copy (original behaviour)
		if (blendMode == FoveatedRender::SubrectBlendMode::kHardCopy) {
			D3D11_BOX srcBox = { srcOffsetX, 0, 0, srcOffsetX + subWidth, subHeight, 1 };
			context->CopySubresourceRegion(dst, 0, dstOffsetX, dstOffsetY, 0, dlssSrc, 0, &srcBox);
			return;
		}

		if (!dstUAV) {
			// No UAV available — fall back to hard copy
			D3D11_BOX srcBox = { srcOffsetX, 0, 0, srcOffsetX + subWidth, subHeight, 1 };
			context->CopySubresourceRegion(dst, 0, dstOffsetX, dstOffsetY, 0, dlssSrc, 0, &srcBox);
			return;
		}

		auto device = globals::d3d::device;

		// Lazy-create CS resources
		if (!Core::vrSubrectBlendCS) {
			Core::vrSubrectBlendCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
				L"Data/Shaders/Upscaling/FoveatedRender/SubrectBlendCS.hlsl", {}, "cs_5_0")));
			Util::SetResourceName(Core::vrSubrectBlendCS.get(), "FoveatedRender::SubrectBlendCS");

			D3D11_BUFFER_DESC cbDesc = {};
			cbDesc.ByteWidth = sizeof(BlendCB);
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr, Core::vrSubrectBlendCB.put()))) {
				logger::error("[FOVEATED] Failed to create SubrectBlend constant buffer");
				// Drop the CS so the next frame retries the full init block.
				Core::vrSubrectBlendCS = nullptr;
				D3D11_BOX srcBox = { srcOffsetX, 0, 0, srcOffsetX + subWidth, subHeight, 1 };
				context->CopySubresourceRegion(dst, 0, dstOffsetX, dstOffsetY, 0, dlssSrc, 0, &srcBox);
				return;
			}
			Util::SetResourceName(Core::vrSubrectBlendCB.get(), "FoveatedRender::SubrectBlendCB");
		}

		if (!Core::vrSubrectBlendCS || !Core::vrSubrectBlendCB)
			return;

		// Get or create cached SRV on the DLSS output
		if (Core::vrBlendSrcSRVOwner != dlssSrc) {
			Core::vrBlendSrcSRV = nullptr;
			winrt::com_ptr<ID3D11Texture2D> dlssTex;
			if (FAILED(dlssSrc->QueryInterface(IID_PPV_ARGS(dlssTex.put())))) {
				D3D11_BOX srcBox = { srcOffsetX, 0, 0, srcOffsetX + subWidth, subHeight, 1 };
				context->CopySubresourceRegion(dst, 0, dstOffsetX, dstOffsetY, 0, dlssSrc, 0, &srcBox);
				return;
			}
			D3D11_TEXTURE2D_DESC desc;
			dlssTex->GetDesc(&desc);
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;
			if (FAILED(device->CreateShaderResourceView(dlssSrc, &srvDesc, Core::vrBlendSrcSRV.put()))) {
				D3D11_BOX srcBox = { srcOffsetX, 0, 0, srcOffsetX + subWidth, subHeight, 1 };
				context->CopySubresourceRegion(dst, 0, dstOffsetX, dstOffsetY, 0, dlssSrc, 0, &srcBox);
				return;
			}
			Util::SetResourceName(Core::vrBlendSrcSRV.get(), "FoveatedRender::BlendSrcSRV");
			Core::vrBlendSrcSRVOwner = dlssSrc;
		}

		{
			D3D11_MAPPED_SUBRESOURCE mapped;
			if (FAILED(context->Map(Core::vrSubrectBlendCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				logger::error("[FOVEATED] BlendSubrectToOutput Map(vrSubrectBlendCB) failed; skipping dispatch");
				return;
			}
			BlendCB* cb = reinterpret_cast<BlendCB*>(mapped.pData);
			cb->DstOffsetX = dstOffsetX;
			cb->DstOffsetY = dstOffsetY;
			cb->SubWidth = subWidth;
			cb->SubHeight = subHeight;
			cb->BlendMode = (blendMode == FoveatedRender::SubrectBlendMode::kDither) ? 1 : 0;
			cb->MaskMode = (foveated.GetSubrectMaskMode() == FoveatedRender::SubrectMaskMode::kOval) ? 1 : 0;
			cb->FrameIndex = globals::state->frameCount;
			cb->SrcOffsetX = srcOffsetX;
			cb->FeatherWidth = foveated.settings.subrectFeatherWidth;
			cb->DitherStrength = foveated.settings.subrectDitherStrength;
			cb->FalloffCurve = foveated.settings.subrectFalloffCurve;
			const float width = static_cast<float>(std::max(subWidth, 1u));
			const float height = static_cast<float>(std::max(subHeight, 1u));
			// Use the subrect's geometric center and half-extent so the ellipse is
			// tangent to the bounding rectangle at the midpoint of each edge. The
			// shader evaluates pixel centers, which keeps the first/last columns
			// symmetric instead of trimming one pixel from the oval.
			cb->MaskCenterX = 0.5f * width;
			cb->MaskCenterY = 0.5f * height;
			cb->MaskRadiusX = std::max(0.5f, cb->MaskCenterX);
			cb->MaskRadiusY = std::max(0.5f, cb->MaskCenterY);
			cb->_pad0 = 0.0f;
			context->Unmap(Core::vrSubrectBlendCB.get(), 0);
		}

		context->CSSetShader(Core::vrSubrectBlendCS.get(), nullptr, 0);
		ID3D11Buffer* cbs[] = { Core::vrSubrectBlendCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);
		ID3D11ShaderResourceView* srvs[] = { Core::vrBlendSrcSRV.get() };
		context->CSSetShaderResources(0, 1, srvs);
		ID3D11UnorderedAccessView* uavs[] = { dstUAV };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		context->Dispatch((subWidth + 7) / 8, (subHeight + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRV[1] = {};
		ID3D11UnorderedAccessView* nullUAV[1] = {};
		ID3D11Buffer* nullCB[1] = {};
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}

}  // namespace FoveatedRenderImpl::Ops

namespace FoveatedRenderImpl
{
	bool Core::PrepareVRPerEyeInputs(
		ID3D11Resource* colorSrc,
		ID3D11Resource* depthSrc,
		ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc,
		ID3D11Resource* transparencySrc,
		uint32_t eyeWidthIn,
		uint32_t eyeHeightIn,
		uint32_t eyeWidthOut,
		uint32_t eyeHeightOut)
	{
		return Ops::PreparePerEyeInputs(
			colorSrc,
			depthSrc,
			mvecSrc,
			reactiveSrc,
			transparencySrc,
			eyeWidthIn,
			eyeHeightIn,
			eyeWidthOut,
			eyeHeightOut);
	}

	bool Core::FinalizeVRPerEyeOutputs(
		ID3D11Resource* colorDst,
		uint32_t eyeWidthOut,
		uint32_t eyeHeightOut)
	{
		return Ops::FinalizePerEyeOutputs(colorDst, eyeWidthOut, eyeHeightOut);
	}

	void Core::ClearResources()
	{
		NeuralRendering::Reset();
		for (int i = 0; i < 2; ++i) {
			vrIntermediateColorIn[i].reset();
			vrIntermediateColorOut[i].reset();
			vrIntermediateDepth[i].reset();
			vrIntermediateMotionVectors[i].reset();
			vrIntermediateReactiveMask[i].reset();
			vrIntermediateTransparencyMask[i].reset();

			vrSubrectColorIn[i].reset();
			vrSubrectColorOut[i].reset();
			vrSubrectDepth[i].reset();
			vrSubrectMotionVectors[i].reset();
			vrSubrectReactiveMask[i].reset();
			vrSubrectTransparencyMask[i].reset();

			vrTemporalHistory[i].reset();
			vrFasterColorOut[i].reset();
		}
		vrSubrectInW = vrSubrectInH = vrSubrectOutW = vrSubrectOutH = 0;

		vrRenderSBS.reset();
		vrRenderSBSW = vrRenderSBSH = 0;

		vrDepthCopyCS = nullptr;
		vrDepthCopyCB = nullptr;

		vrFasterOutW = vrFasterOutH = 0;

		vrMvecSRV = nullptr;
		vrMvecSRVOwner = nullptr;
		vrTemporalHistoryW = vrTemporalHistoryH = 0;
		vrTemporalFrameIdx = 0;
		vrTemporalHistoryValid = false;

		vrBlendSrcSRV = nullptr;
		vrBlendSrcSRVOwner = nullptr;

		activeSubrectUVHash = 0;
		neuralGuidesFrame = UINT32_MAX;
	}

	void Core::InvalidateTemporalState()
	{
		// A crop can move without changing the intermediate dimensions. In that
		// case resource recreation alone is insufficient: the old guide snapshot,
		// periphery history, and DLSSNR history all describe the previous region.
		vrTemporalFrameIdx = 0;
		vrTemporalHistoryValid = false;
		neuralGuidesFrame = UINT32_MAX;
		NeuralRendering::ResetHistory();
	}

	void Core::ClearShaderCache()
	{
		vrSubrectStretchCS = nullptr;
		vrSubrectStretchCB = nullptr;
		vrSubrectStretchSampler = nullptr;
		vrDepthCopyCS = nullptr;
		vrDepthCopyCB = nullptr;

		vrTemporalSmoothCS = nullptr;
		vrTemporalSmoothCB = nullptr;
		vrTemporalSmoothSampler = nullptr;

		vrSubrectBlendCS = nullptr;
		vrSubrectBlendCB = nullptr;
	}
}
