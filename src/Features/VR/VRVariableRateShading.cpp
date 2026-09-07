#include "VRVariableRateShading.h"

#include "Deferred.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <vector>

namespace
{
	// Mirrors VRSPostSceneCB in VRSPostSceneCS.hlsl.
	struct PostSceneCB
	{
		uint32_t tileWidth;
		uint32_t tileHeight;
		uint32_t outputWidth;
		uint32_t outputHeight;
		uint32_t debugVisualize;
		float ditherStrength;
		uint32_t pad0;
		uint32_t pad1;
	};
}

namespace VRFeatures
{
	bool VRVariableRateShading::Initialize()
	{
		if (initialized) {
			return nvapiAvailable;
		}

		if (!globals::game::isVR) {
			nvapiAvailable = false;
			unavailableReason = UnavailableReason::NotVR;
			initialized = true;
			return false;
		}

		NvAPI_Status status = NvAPI_Initialize();
		if (status != NVAPI_OK) {
			nvapiAvailable = false;
			unavailableReason = UnavailableReason::NvApiInitFailed;
			initialized = true;
			logger::error("VRVariableRateShading: NvAPI_Initialize failed ({})", static_cast<int>(status));
			return false;
		}

		NV_D3D1x_GRAPHICS_CAPS caps{};
		status = NvAPI_D3D1x_GetGraphicsCapabilities(globals::d3d::device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
		if (status != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
			nvapiAvailable = false;
			unavailableReason = UnavailableReason::HardwareUnsupported;
			initialized = true;
			logger::info("VRVariableRateShading: Variable Rate Shading not supported ({})", static_cast<int>(status));
			return false;
		}

		nvapiAvailable = true;
		unavailableReason = UnavailableReason::None;
		initialized = true;
		logger::info("VRVariableRateShading: NVAPI VRS initialized successfully");
		return true;
	}

	void VRVariableRateShading::CreateShadingRateResource(uint32_t width, uint32_t height)
	{
		if (!nvapiAvailable) {
			return;
		}

		if (width == currentWidth && height == currentHeight && shadingRateView) {
			return;
		}

		auto cached = std::find_if(resourceCache.begin(), resourceCache.end(), [&](const SizedResource& a_resource) {
			return a_resource.width == width && a_resource.height == height;
		});
		if (cached != resourceCache.end()) {
			srrTexture = cached->texture;
			shadingRateView = cached->view;
			srrSRV = cached->srv;
			currentWidth = width;
			currentHeight = height;
			UpdateShadingRatePattern();
			return;
		}

		const uint32_t tileWidth = (width + kVrsTileSize - 1) / kVrsTileSize;
		const uint32_t tileHeight = (height + kVrsTileSize - 1) / kVrsTileSize;

		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = tileWidth;
		texDesc.Height = tileHeight;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R8_UINT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		winrt::com_ptr<ID3D11Texture2D> newTexture;
		HRESULT hr = globals::d3d::device->CreateTexture2D(&texDesc, nullptr, newTexture.put());
		if (FAILED(hr)) {
			logger::error("VRVariableRateShading: Failed to create SRR texture ({}x{}), hr={:#010x}", tileWidth, tileHeight, static_cast<unsigned long>(hr));
			shadingRateView = nullptr;
			srrTexture = nullptr;
			srrSRV = nullptr;
			return;
		}
		Util::SetResourceName(newTexture.get(), "VRVariableRateShading::ShadingRateTexture");

		NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC desc{};
		desc.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
		desc.Format = DXGI_FORMAT_R8_UINT;
		desc.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MipSlice = 0;

		winrt::com_ptr<ID3D11NvShadingRateResourceView> newView;
		NvAPI_Status status = NvAPI_D3D11_CreateShadingRateResourceView(globals::d3d::device, newTexture.get(), &desc, newView.put());
		if (status != NVAPI_OK) {
			logger::error("VRVariableRateShading: NvAPI_D3D11_CreateShadingRateResourceView failed ({})", static_cast<int>(status));
			shadingRateView = nullptr;
			srrTexture = nullptr;
			srrSRV = nullptr;
			return;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R8_UINT;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		winrt::com_ptr<ID3D11ShaderResourceView> newSRV;
		hr = globals::d3d::device->CreateShaderResourceView(newTexture.get(), &srvDesc, newSRV.put());
		if (FAILED(hr)) {
			logger::error("VRVariableRateShading: Failed to create SRR SRV, hr={:#010x}", static_cast<unsigned long>(hr));
			shadingRateView = nullptr;
			srrTexture = nullptr;
			srrSRV = nullptr;
			return;
		}
		Util::SetResourceName(newSRV.get(), "VRVariableRateShading::ShadingRateTexture SRV");

		if (resourceCache.size() >= kMaxCachedResources) {
			resourceCache.erase(resourceCache.begin());
		}
		resourceCache.push_back({ width, height, newTexture, newView, newSRV });

		srrTexture = newTexture;
		shadingRateView = newView;
		srrSRV = newSRV;
		currentWidth = width;
		currentHeight = height;
		UpdateShadingRatePattern();
	}

	void VRVariableRateShading::UpdateShadingRatePattern()
	{
		CS_GPU_PASS("VRVariableRateShading::UpdatePattern");

		if (!shadingRateView || !srrTexture) {
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		srrTexture->GetDesc(&desc);

		const uint32_t width = desc.Width;
		const uint32_t height = desc.Height;
		const float halfWidth = width * 0.5f;
		const uint32_t rowPitch = width * sizeof(uint8_t);

		std::vector<uint8_t> buffer(static_cast<size_t>(width) * height);
		uint32_t rateCounts[4] = { 0, 0, 0, 0 };

		const float2 leftCenter = { (0.5f + foveationProfile.centerOffsets[0].x) * halfWidth, (0.5f + foveationProfile.centerOffsets[0].y) * height };
		const float2 rightCenter = { halfWidth + (0.5f + foveationProfile.centerOffsets[1].x) * halfWidth, (0.5f + foveationProfile.centerOffsets[1].y) * height };

		// Isotropic radius (matches vrperfkit's fixed-foveated model) -- the ellipse
		// shape on screen still follows each eye's own aspect ratio because ndx/ndy
		// below are normalized against that eye's own half-width/height, not because
		// the radius itself is anisotropic.
		const float radius = 0.5f * radiusScale;

		auto ellipseTest = [](float ndx, float ndy, float rx, float ry) {
			const float ex = ndx / rx;
			const float ey = ndy / ry;
			return ex * ex + ey * ey;
		};

		for (uint32_t y = 0; y < height; ++y) {
			for (uint32_t x = 0; x < width; ++x) {
				const bool leftEye = x < static_cast<uint32_t>(halfWidth);
				const float cx = leftEye ? leftCenter.x : rightCenter.x;
				const float cy = leftEye ? leftCenter.y : rightCenter.y;

				const float ndx = (static_cast<float>(x) - cx) / halfWidth;
				const float ndy = (static_cast<float>(y) - cy) / height;

				const float innerVal = ellipseTest(ndx, ndy, radius * innerRadiusFactor, radius * innerRadiusFactor);
				const float middleVal = ellipseTest(ndx, ndy, radius * midRadiusFactor, radius * midRadiusFactor);
				const float outerVal = ellipseTest(ndx, ndy, radius, radius);

				uint8_t rate;
				if (innerVal <= 1.0f) {
					rate = 0;
				} else if (middleVal <= 1.0f) {
					rate = 1;
				} else if (outerVal <= 1.0f) {
					rate = 2;
				} else {
					rate = 3;
				}

				if (rate == 0) {
					const float innerValY = ellipseTest(ndx, ndy * 2.0f, radius * innerRadiusFactor, radius * innerRadiusFactor);
					const float innerValX = ellipseTest(ndx * 2.0f, ndy, radius * innerRadiusFactor, radius * innerRadiusFactor);
					if (innerValY > 1.0f && innerValX > 1.0f) {
						rate = 1;
					}
				}

				buffer[static_cast<size_t>(y) * width + x] = rate;
				++rateCounts[rate];
			}
		}

		globals::d3d::context->UpdateSubresource(srrTexture.get(), 0, nullptr, buffer.data(), rowPitch, 0);

		const uint32_t totalTiles = width * height;
		logger::info(
			"VRVariableRateShading: pattern updated ({}x{}px render, radius {:.0f}x{:.0f}px) -- 1x1={:.0f}% 1x2={:.0f}% 2x2={:.0f}% 4x4={:.0f}%",
			width * kVrsTileSize, height * kVrsTileSize, radius * halfWidth * kVrsTileSize, radius * height * kVrsTileSize,
			100.0f * rateCounts[0] / totalTiles, 100.0f * rateCounts[1] / totalTiles,
			100.0f * rateCounts[2] / totalTiles, 100.0f * rateCounts[3] / totalTiles);
	}

	void VRVariableRateShading::SubmitShadingRateTable(ID3D11DeviceContext* a_context, bool a_enable,
		NV_PIXEL_SHADING_RATE a_rate1, NV_PIXEL_SHADING_RATE a_rate2, NV_PIXEL_SHADING_RATE a_rate3)
	{
		NV_D3D11_VIEWPORT_SHADING_RATE_DESC viewportDesc{};
		viewportDesc.enableVariablePixelShadingRate = a_enable;
		for (auto& r : viewportDesc.shadingRateTable) {
			r = NV_PIXEL_X1_PER_RASTER_PIXEL;
		}
		viewportDesc.shadingRateTable[1] = a_rate1;
		viewportDesc.shadingRateTable[2] = a_rate2;
		viewportDesc.shadingRateTable[3] = a_rate3;

		NV_D3D11_VIEWPORTS_SHADING_RATE_DESC viewportsDesc{};
		viewportsDesc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
		viewportsDesc.numViewports = 1;
		viewportsDesc.pViewports = &viewportDesc;

		NvAPI_D3D11_RSSetViewportsPixelShadingRates(a_context, &viewportsDesc);
	}

	void VRVariableRateShading::ApplyForRenderTarget(ID3D11DeviceContext* a_context)
	{
		if (!enabled || !nvapiAvailable) {
			return;
		}

		// Size from whatever's actually bound, not screenSize -- post-upscale
		// passes (grass, sky, particles, effects) render at a different
		// resolution than the pre-upscale internal buffer screenSize describes.
		// Every early return below forces full rate rather than bare-returning,
		// so a target this pass doesn't recognize doesn't silently inherit
		// whatever coarse pattern was left bound from the previous draw.
		winrt::com_ptr<ID3D11RenderTargetView> boundRTV;
		a_context->OMGetRenderTargets(1, boundRTV.put(), nullptr);
		if (!boundRTV) {
			ForceFullRate(a_context);
			return;
		}
		winrt::com_ptr<ID3D11Resource> resource;
		boundRTV->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture) {
			ForceFullRate(a_context);
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);

		// Below this, it's not a stereo eye view (e.g. an icon/reflection-probe
		// render reusing the same shader class) -- foveation has no meaning there.
		constexpr uint32_t kMinStereoDimension = 256;
		if (desc.Width < kMinStereoDimension || desc.Height < kMinStereoDimension) {
			ForceFullRate(a_context);
			return;
		}

		if (desc.Width != currentWidth || desc.Height != currentHeight) {
			CreateShadingRateResource(desc.Width, desc.Height);
		}

		if (!shadingRateView) {
			ForceFullRate(a_context);
			return;
		}

		NvAPI_Status status = NvAPI_D3D11_RSSetShadingRateResourceView(a_context, shadingRateView.get());
		if (status != NVAPI_OK) {
			logger::error("VRVariableRateShading: RSSetShadingRateResourceView failed ({})", static_cast<int>(status));
		}

		SubmitShadingRateTable(a_context, true, NV_PIXEL_X1_PER_1X2_RASTER_PIXELS, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS, NV_PIXEL_X1_PER_4X4_RASTER_PIXELS);
	}

	void VRVariableRateShading::ForceFullRate(ID3D11DeviceContext* a_context)
	{
		if (!nvapiAvailable) {
			return;
		}
		SubmitShadingRateTable(a_context, true, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL);
	}

	void VRVariableRateShading::Disable(ID3D11DeviceContext* a_context)
	{
		if (!nvapiAvailable) {
			return;
		}
		SubmitShadingRateTable(a_context, false, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL);
	}

	void VRVariableRateShading::Cleanup()
	{
		if (nvapiAvailable && globals::d3d::context) {
			Disable(globals::d3d::context);
			NvAPI_D3D11_RSSetShadingRateResourceView(globals::d3d::context, nullptr);
		}
		shadingRateView = nullptr;
		srrTexture = nullptr;
		srrSRV = nullptr;
		resourceCache.clear();
		currentWidth = 0;
		currentHeight = 0;
	}

	void VRVariableRateShading::SetEnabled(bool a_enabled)
	{
		if (enabled != a_enabled) {
			enabled = a_enabled;
			if (enabled) {
				Initialize();
			} else {
				Cleanup();
			}
		}
	}

	VRVariableRateShading::RegionInfo VRVariableRateShading::GetRegionInfo() const
	{
		RegionInfo info{};
		info.usingRealLensCenter = foveationProfile.usingRealLensCenter;
		info.outerWidthFraction = radiusScale;
		info.outerHeightFraction = radiusScale;
		info.innerRadiusFactor = innerRadiusFactor;
		info.midRadiusFactor = midRadiusFactor;
		info.centerOffsets[0] = foveationProfile.centerOffsets[0];
		info.centerOffsets[1] = foveationProfile.centerOffsets[1];
		return info;
	}

	void VRVariableRateShading::SetFoveationProfile(const FoveationProfile& a_profile)
	{
		foveationProfile = a_profile;
		if (enabled && shadingRateView) {
			UpdateShadingRatePattern();
		}
	}

	void VRVariableRateShading::SetTuning(float a_radiusScale, float a_innerRadiusFactor, float a_midRadiusFactor)
	{
		radiusScale = std::clamp(a_radiusScale, 0.2f, 3.0f);
		innerRadiusFactor = std::clamp(a_innerRadiusFactor, 0.05f, 0.95f);
		midRadiusFactor = std::clamp(a_midRadiusFactor, innerRadiusFactor + 0.01f, 1.0f);
		if (enabled && shadingRateView) {
			UpdateShadingRatePattern();
		}
	}

	void VRVariableRateShading::SetDitherStrength(float a_strength)
	{
		ditherStrength = std::clamp(a_strength, 0.0f, 1.0f);
	}

	void VRVariableRateShading::CompilePostSceneShader()
	{
		if (postSceneCS && postSceneCB) {
			return;
		}

		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\VR\\VRSPostSceneCS.hlsl", {}, "cs_5_0"))) {
			postSceneCS.attach(rawPtr);
			Util::SetResourceName(postSceneCS.get(), "VRVariableRateShading::PostSceneCS");
		}

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(PostSceneCB);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		globals::d3d::device->CreateBuffer(&cbDesc, nullptr, postSceneCB.put());
		if (postSceneCB) {
			Util::SetResourceName(postSceneCB.get(), "VRVariableRateShading::PostSceneCB");
		}
	}

	void VRVariableRateShading::PostSceneProcess(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_mainResource, ID3D11UnorderedAccessView* a_mainUAV)
	{
		if (!enabled || !nvapiAvailable || !srrSRV || !a_mainResource || !a_mainUAV) {
			return;
		}
		if (!debugVisualize && ditherStrength <= 0.0f) {
			return;
		}

		CompilePostSceneShader();
		if (!postSceneCS || !postSceneCB) {
			return;
		}

		D3D11_TEXTURE2D_DESC mainDesc{};
		a_mainResource->GetDesc(&mainDesc);

		const uint32_t tileWidth = (currentWidth + kVrsTileSize - 1) / kVrsTileSize;
		const uint32_t tileHeight = (currentHeight + kVrsTileSize - 1) / kVrsTileSize;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(postSceneCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		PostSceneCB cbData{ tileWidth, tileHeight, mainDesc.Width, mainDesc.Height, debugVisualize ? 1u : 0u, ditherStrength, 0, 0 };
		std::memcpy(mapped.pData, &cbData, sizeof(cbData));
		a_context->Unmap(postSceneCB.get(), 0);

		CS_GPU_PASS("VRVariableRateShading::PostSceneProcess");

		a_context->CSSetShader(postSceneCS.get(), nullptr, 0);
		ID3D11Buffer* cbs[1] = { postSceneCB.get() };
		a_context->CSSetConstantBuffers(0, 1, cbs);
		ID3D11ShaderResourceView* srvs[1] = { srrSRV.get() };
		a_context->CSSetShaderResources(0, 1, srvs);
		ID3D11UnorderedAccessView* uavs[1] = { a_mainUAV };
		a_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		a_context->Dispatch((mainDesc.Width + 7) / 8, (mainDesc.Height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		a_context->CSSetShaderResources(0, 1, nullSRV);
		a_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		a_context->CSSetConstantBuffers(0, 1, nullCB);
		a_context->CSSetShader(nullptr, nullptr, 0);
	}
}
