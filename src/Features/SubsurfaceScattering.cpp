#include "SubsurfaceScattering.h"
#include <Util.h>

#include "State.h"

void SubsurfaceScattering::DrawSettings()
{
}

void SubsurfaceScattering::Draw(const RE::BSShader* shader, const uint32_t)
{
	if (shader->shaderType.get() != RE::BSShader::Type::Lighting)
		return;

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

	auto reflections = (!REL::Module::IsVR() ?
							   RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().cubeMapRenderTarget :
							   RE::BSGraphics::RendererShadowState::GetSingleton()->GetVRRuntimeData().cubeMapRenderTarget) == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS;

	if (!(reflections || accumulator->GetRuntimeData().activeShadowSceneNode != RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0])) {
		if (!deferredTexture) {
			auto renderer = RE::BSGraphics::Renderer::GetSingleton();

			auto snowSwapTexture = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP];

			D3D11_TEXTURE2D_DESC texDesc{};
			snowSwapTexture.texture->GetDesc(&texDesc);
			if ((texDesc.BindFlags | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET) != texDesc.BindFlags) {
				logger::info("missing access!");
			}
			colorTextureTemp = new Texture2D(texDesc);
			colorTextureTemp2 = new Texture2D(texDesc);

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			snowSwapTexture.SRV->GetDesc(&srvDesc);
			colorTextureTemp->CreateSRV(srvDesc);
			colorTextureTemp2->CreateSRV(srvDesc);

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			snowSwapTexture.RTV->GetDesc(&rtvDesc);
			colorTextureTemp->CreateRTV(rtvDesc);
			colorTextureTemp2->CreateRTV(rtvDesc);

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			snowSwapTexture.UAV->GetDesc(&uavDesc);
			colorTextureTemp->CreateUAV(uavDesc);
			colorTextureTemp2->CreateUAV(uavDesc);

			texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			rtvDesc.Format = texDesc.Format;
			srvDesc.Format = texDesc.Format;
			uavDesc.Format = texDesc.Format;

			specularTexture = new Texture2D(texDesc);
			specularTexture->CreateRTV(rtvDesc);
			specularTexture->CreateSRV(srvDesc);
			specularTexture->CreateUAV(uavDesc);

			albedoTexture = new Texture2D(texDesc);
			albedoTexture->CreateRTV(rtvDesc);
			albedoTexture->CreateSRV(srvDesc);
			albedoTexture->CreateUAV(uavDesc);

			deferredTexture = new Texture2D(texDesc);
			deferredTexture->CreateRTV(rtvDesc);
			deferredTexture->CreateSRV(srvDesc);
			deferredTexture->CreateUAV(uavDesc);

			ambientTexture = new Texture2D(texDesc);
			ambientTexture->CreateRTV(rtvDesc);
			ambientTexture->CreateSRV(srvDesc);
			ambientTexture->CreateUAV(uavDesc);

			auto depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
			depthTexture.texture->GetDesc(&texDesc);
			depthTexture.depthSRV->GetDesc(&srvDesc);
			depthTextureTemp = new Texture2D(texDesc);
			depthTextureTemp->CreateSRV(srvDesc);

			auto device = renderer->GetRuntimeData().forwarder;

			D3D11_SAMPLER_DESC samplerDesc = {};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.MaxAnisotropy = 1;
			samplerDesc.MinLOD = 0;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &linearSampler));

			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointSampler));
		}

		ID3D11RenderTargetView* views[8];
		ID3D11DepthStencilView* depthStencil;

		context->OMGetRenderTargets(4, views, &depthStencil);

		views[4] = specularTexture->rtv.get();
		views[5] = albedoTexture->rtv.get();
		views[6] = deferredTexture->rtv.get();
		views[7] = ambientTexture->rtv.get();
		context->OMSetRenderTargets(8, views, depthStencil);

		auto srv = deferredTexture->srv.get();
		context->PSSetShaderResources(40, 1, &srv);

		ID3D11BlendState* blendState;
		FLOAT blendFactor[4];
		UINT sampleMask;

		context->OMGetBlendState(&blendState, blendFactor, &sampleMask);

		if (!mappedBlendStates.contains(blendState)) {
			if (!modifiedBlendStates.contains(blendState)) {
				D3D11_BLEND_DESC blendDesc;
				blendState->GetDesc(&blendDesc);
				blendDesc.IndependentBlendEnable = true;
				for (int i = 1; i < 8; i++)
					blendDesc.RenderTarget[i] = blendDesc.RenderTarget[0];
				auto renderer = RE::BSGraphics::Renderer::GetSingleton();
				auto device = renderer->GetRuntimeData().forwarder;
				ID3D11BlendState* modifiedBlendState;
				DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &modifiedBlendState));
				mappedBlendStates.insert(modifiedBlendState);
				modifiedBlendStates.insert({ blendState, modifiedBlendState });
			}
			blendState = modifiedBlendStates[blendState];
			context->OMSetBlendState(blendState, blendFactor, sampleMask);
		}
	}

	{
		PerPass perPassData{};
		perPassData.SkinMode = skinMode;

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(PerPass);
		memcpy_s(mapped.pData, bytes, &perPassData, bytes);
		context->Unmap(perPass->resource.get(), 0);

		ID3D11ShaderResourceView* views[1]{};
		views[0] = perPass->srv.get();
		context->PSSetShaderResources(35, ARRAYSIZE(views), views);
	}
}

void SubsurfaceScattering::SetupResources()
{
	blurCB = new ConstantBuffer(ConstantBufferDesc<BlurCB>());
	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(PerPass);
		sbDesc.ByteWidth = sizeof(PerPass);
		perPass = std::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;
		perPass->CreateSRV(srvDesc);
	}
}

void SubsurfaceScattering::Load(json& o_json)
{
	Feature::Load(o_json);
}

void SubsurfaceScattering::Save(json&)
{
}

void SubsurfaceScattering::DrawDeferred()
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

	auto reflections = (!REL::Module::IsVR() ?
							   RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().cubeMapRenderTarget :
							   RE::BSGraphics::RendererShadowState::GetSingleton()->GetVRRuntimeData().cubeMapRenderTarget) == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS;

	if (!(reflections || accumulator->GetRuntimeData().activeShadowSceneNode != RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0])) {
		auto viewport = RE::BSGraphics::State::GetSingleton();

		float resolutionX = colorTextureTemp->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		float resolutionY = colorTextureTemp->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

		{
			BlurCB data{};

			data.BufferDim.x = (float)deferredTexture->desc.Width;
			data.BufferDim.y = (float)deferredTexture->desc.Height;

			data.RcpBufferDim.x = 1.0f / (float)deferredTexture->desc.Width;
			data.RcpBufferDim.y = 1.0f / (float)deferredTexture->desc.Height;
			const auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
			auto bTAA = !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled :
			                                   imageSpaceManager->GetVRRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled;
			data.FrameCount = viewport->uiFrameCount * (bTAA || State::GetSingleton()->upscalerLoaded);

			data.DynamicRes.x = viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
			data.DynamicRes.y = viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

			data.DynamicRes.z = 1.0f / data.DynamicRes.x;
			data.DynamicRes.w = 1.0f / data.DynamicRes.y;

			auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			auto cameraData = !REL::Module::IsVR() ? shadowState->GetRuntimeData().cameraData.getEye() :
			                                         shadowState->GetVRRuntimeData().cameraData.getEye();
			data.SSSS_FOVY = atan(1.0f / cameraData.projMat.m[0][0]) * 2.0f * (180.0f / 3.14159265359f);

			data.CameraData.x = accumulator->kCamera->GetRuntimeData2().viewFrustum.fFar;
			data.CameraData.y = accumulator->kCamera->GetRuntimeData2().viewFrustum.fNear;
			data.CameraData.z = accumulator->kCamera->GetRuntimeData2().viewFrustum.fFar - accumulator->kCamera->GetRuntimeData2().viewFrustum.fNear;
			data.CameraData.w = accumulator->kCamera->GetRuntimeData2().viewFrustum.fFar * accumulator->kCamera->GetRuntimeData2().viewFrustum.fNear;

			data.SSSWidth = 10.0f;

			blurCB->Update(data);
		}

		auto renderer = RE::BSGraphics::Renderer::GetSingleton();

		{
			//ID3D11ShaderResourceView* srv[8];
			//context->PSGetShaderResources(1, 8, srv);

			ID3D11Buffer* buffer[1] = { blurCB->CB() };
			context->CSSetConstantBuffers(0, 1, buffer);

			ID3D11SamplerState* samplers[2];
			samplers[0] = linearSampler;
			samplers[1] = pointSampler;
			context->CSSetSamplers(0, 2, samplers);

			context->CopyResource(depthTextureTemp->resource.get(), renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].texture);
			context->CopyResource(colorTextureTemp->resource.get(), renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP].texture);

			ID3D11ShaderResourceView* views[6];
			views[0] = colorTextureTemp->srv.get();
			views[1] = depthTextureTemp->srv.get();
			views[2] = specularTexture->srv.get();
			views[3] = albedoTexture->srv.get();
			views[4] = deferredTexture->srv.get();
			views[5] = ambientTexture->srv.get();
			context->CSSetShaderResources(0, 6, views);

			ID3D11UnorderedAccessView* uav = nullptr;

			// Horizontal pass to temporary texture
			{
				uav = colorTextureTemp2->uav.get();
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

				auto shader = GetComputeShaderHorizontalBlur();
				context->CSSetShader(shader, nullptr, 0);

				context->Dispatch((uint32_t)std::ceil(resolutionX / 32.0f), (uint32_t)std::ceil(resolutionY / 32.0f), 1);
			}

			uav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

			// Vertical pass to main texture
			{
				views[0] = colorTextureTemp2->srv.get();
				context->CSSetShaderResources(0, 1, views);

				uav = colorTextureTemp->uav.get();
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

				auto shader = GetComputeShaderVerticalBlur();
				context->CSSetShader(shader, nullptr, 0);

				context->Dispatch((uint32_t)std::ceil(resolutionX / 32.0f), (uint32_t)std::ceil(resolutionY / 32.0f), 1);
			}

			context->CopyResource(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP].texture, colorTextureTemp->resource.get());
		}

		ID3D11Buffer* buffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &buffer);

		ID3D11SamplerState* samplers[2]{ nullptr, nullptr };
		context->CSSetSamplers(0, 2, samplers);

		ID3D11ShaderResourceView* views[5]{ nullptr, nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, 5, views);

		ID3D11UnorderedAccessView* uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);

		FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearRenderTargetView(deferredTexture->rtv.get(), clear);
		context->ClearRenderTargetView(specularTexture->rtv.get(), clear);
		context->ClearRenderTargetView(ambientTexture->rtv.get(), clear);
	}
}

void SubsurfaceScattering::ClearComputeShader()
{
	if (horizontalSSBlur) {
		horizontalSSBlur->Release();
		horizontalSSBlur = nullptr;
	}
	if (verticalSSBlur) {
		verticalSSBlur->Release();
		verticalSSBlur = nullptr;
	}
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderHorizontalBlur()
{
	if (!horizontalSSBlur) {
		logger::debug("Compiling horizontalSSBlur");
		horizontalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", { { "HORIZONTAL", "" } }, "cs_5_0");
	}
	return horizontalSSBlur;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderVerticalBlur()
{
	if (!verticalSSBlur) {
		logger::debug("Compiling verticalSSBlur");
		verticalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", {}, "cs_5_0");
	}
	return verticalSSBlur;
}

void SubsurfaceScattering::BSLightingShader_SetupGeometry_Before(RE::BSRenderPass* Pass)
{
	skinMode = 0;

	auto geometry = Pass->geometry;
	if (auto userData = geometry->GetUserData()) {
		if (auto actor = userData->As<RE::Actor>()) {
			if (auto race = actor->GetRace()) {
				if (Pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kFace, RE::BSShaderProperty::EShaderPropertyFlag::kFaceGenRGBTint)) {
					static auto isBeastRace = RE::TESForm::LookupByEditorID("IsBeastRace")->As<RE::BGSKeyword>();
					skinMode = race->HasKeyword(isBeastRace) ? 1 : 2;
				} else if (Pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kCharacterLighting, RE::BSShaderProperty::EShaderPropertyFlag::kSoftLighting)) {
					auto lightingMaterial = (RE::BSLightingShaderMaterialBase*)(Pass->shaderProperty->material);
					if (auto diffuse = lightingMaterial->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
						if (diffuse != nullptr) {
							std::string diffuseStr = diffuse;
							if (diffuseStr.contains("MaleChild") || diffuseStr.contains("FemaleChild")) {
								static auto isBeastRace = RE::TESForm::LookupByEditorID("IsBeastRace")->As<RE::BGSKeyword>();
								skinMode = race->HasKeyword(isBeastRace) ? 1 : 2;
							}
						}
					}
				}
			}
		}
	}
}