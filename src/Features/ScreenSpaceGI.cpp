#include "ScreenSpaceGI.h"

#include "Util.h"

class FrameChecker
{
private:
	uint32_t last_frame = UINT32_MAX;

public:
	inline bool isNewFrame(uint32_t frame)
	{
		bool retval = last_frame != frame;
		last_frame = frame;
		return retval;
	}
	inline bool isNewFrame() { return isNewFrame(RE::BSGraphics::State::GetSingleton()->uiFrameCount); }
};

void ScreenSpaceGI::DrawSettings()
{
	ImGui::Checkbox("Enabled", &settings.Enabled);

	ImGui::SliderInt("Slices", (int*)&settings.SliceCount, 1, 1000);
	ImGui::SliderInt("Steps Per Slice", (int*)&settings.StepsPerSlice, 1, 1000);

	ImGui::InputFloat("Effect radius", &settings.EffectRadius, 10.f, 0.0f, "%.2f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("World (viewspace) effect radius\nExpected range: depends on the scene & requirements, anything from 0.01 to 1000+");

	if (ImGui::CollapsingHeader("Auto-tuned settings (heuristics)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::InputFloat("Falloff range", &settings.EffectFalloffRange, 0.05f, 0.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Gently reduce sample impact as it gets out of 'Effect radius' bounds\nExpected range: [0.0, 1.0].");

		ImGui::InputFloat("Sample distribution power", &settings.SampleDistributionPower, 0.05f, 0.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Make samples on a slice equally distributed (1.0) or focus more towards the center (>1.0)\nExpected range: [1.0, 3.0].");

		ImGui::InputFloat("Thin occluder compensation", &settings.ThinOccluderCompensation, 0.05f, 0.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Slightly reduce impact of samples further back to counter the bias from depth-based (incomplete) input scene geometry data\nExpected range: [0.0, 0.7]");

		ImGui::InputFloat("Final power", &settings.FinalValuePower, 0.05f, 0.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Applies power function to the final value: occlusion = pow( occlusion, finalPower )\nExpected range: [0.5, 5.0]");

		ImGui::InputFloat("Depth MIP sampling offset", &settings.DepthMIPSamplingOffset, 0.05f, 0.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Mainly performance (texture memory bandwidth) setting but as a side-effect reduces overshadowing by thin objects and increases temporal instability\nExpected range: [2.0, 6.0]");
	}

	if (ImGui::CollapsingHeader("Debug View")) {
		ImGui::BulletText("texColor");
		ImGui::Image(texColor->srv.get(), { texColor->desc.Width * .3f, texColor->desc.Height * .3f });

		ImGui::BulletText("texGI");
		ImGui::Image(texGI->srv.get(), { texGI->desc.Width * .3f, texGI->desc.Height * .3f });

		ImGui::BulletText("texColorMix");
		ImGui::Image(texColorMix->srv.get(), { texColorMix->desc.Width * .3f, texColorMix->desc.Height * .3f });
	}
}

void ScreenSpaceGI::ClearComputeShader()
{
	if (hilbertLutCompute)
		hilbertLutCompute->Release();
	hilbertLutCompute = nullptr;
	hilbertLUTGenFlag = true;

	if (prefilterDepthsCompute)
		prefilterDepthsCompute->Release();
	prefilterDepthsCompute = nullptr;

	if (ssgiCompute)
		ssgiCompute->Release();
	ssgiCompute = nullptr;

	if (mixCompute)
		mixCompute->Release();
	mixCompute = nullptr;
}

void ScreenSpaceGI::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto device = renderer->GetRuntimeData().forwarder;

	if (!ssgiCB)
		ssgiCB = new ConstantBuffer(ConstantBufferDesc<SSGICB>());

	constexpr auto shader_path = L"Data\\Shaders\\ScreenSpaceGI\\vaGTAO.hlsl";
	if (!hilbertLutCompute)
		hilbertLutCompute = (ID3D11ComputeShader*)Util::CompileShader(shader_path, { { "", "" } }, "cs_5_0", "CSGenerateHibertLUT");
	if (!prefilterDepthsCompute)
		prefilterDepthsCompute = (ID3D11ComputeShader*)Util::CompileShader(shader_path, { { "", "" } }, "cs_5_0", "CSPrefilterDepths16x16");
	if (!ssgiCompute)
		ssgiCompute = (ID3D11ComputeShader*)Util::CompileShader(shader_path, { { "", "" } }, "cs_5_0", "CSGTAO");
	if (!mixCompute)
		mixCompute = (ID3D11ComputeShader*)Util::CompileShader(shader_path, { { "", "" } }, "cs_5_0", "CSMix");

	if (!texGI) {
		{
			D3D11_TEXTURE2D_DESC texDesc{
				.Width = 64,
				.Height = 64,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R32_UINT,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
				.CPUAccessFlags = 0,
				.MiscFlags = 0
			};
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = {
					.MostDetailedMip = 0,
					.MipLevels = texDesc.MipLevels }
			};
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};

			{
				texHilbertLUT = new Texture2D(texDesc);
				texHilbertLUT->CreateSRV(srvDesc);
				texHilbertLUT->CreateUAV(uavDesc);
			}

			auto snowSwapTexture = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP];
			snowSwapTexture.texture->GetDesc(&texDesc);
			srvDesc.Format = uavDesc.Format = texDesc.Format;

			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			{
				texColorMix = new Texture2D(texDesc);
				texColorMix->CreateSRV(srvDesc);
				texColorMix->CreateUAV(uavDesc);
			}

			texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;

			{
				texColor = new Texture2D(texDesc);
				texColor->CreateSRV(srvDesc);
			}

			texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
			texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

			{
				texGI = new Texture2D(texDesc);
				texGI->CreateSRV(srvDesc);
				texGI->CreateUAV(uavDesc);
			}

			texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;

			{
				texWorkingDepth = new Texture2D(texDesc);
				texWorkingDepth->CreateSRV(srvDesc);
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth + 0));
				uavDesc.Texture2D.MipSlice = 1;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth + 1));
				uavDesc.Texture2D.MipSlice = 2;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth + 2));
				uavDesc.Texture2D.MipSlice = 3;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth + 3));
				uavDesc.Texture2D.MipSlice = 4;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth + 4));
			}
		}

		{
			D3D11_SAMPLER_DESC samplerDesc = {
				.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
				.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
				.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
				.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
				.MaxAnisotropy = 1,
				.MinLOD = 0,
				.MaxLOD = D3D11_FLOAT32_MAX
			};
			DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &linearSampler));

			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointSampler));
		}
	}
}

void ScreenSpaceGI::Load(json& o_json)
{
	Feature::Load(o_json);
}

void ScreenSpaceGI::Save(json&)
{
}

void ScreenSpaceGI::GenerateHilbertLUT()
{
	if (hilbertLUTGenFlag) {
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

		context->CSSetUnorderedAccessViews(0, 1, texHilbertLUT->uav.put(), nullptr);
		context->CSSetShader(hilbertLutCompute, nullptr, 0);
		context->Dispatch(2, 2, 1);

		ID3D11UnorderedAccessView* uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}

void ScreenSpaceGI::UpdateBuffer()
{
	auto viewport = RE::BSGraphics::State::GetSingleton();

	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	auto projMat = (!REL::Module::IsVR()) ? state->GetRuntimeData().cameraData.getEye().projMat :
	                                        state->GetVRRuntimeData().cameraData.getEye().projMat;

	SSGICB ssgi_cb_contents = {
		.ViewportSize = { (int32_t)(texGI->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale),
			(int32_t)(texGI->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale) },
		.DepthUnpackConsts = { -projMat(3, 2), projMat(2, 2) },
		.CameraTanHalfFOV = { 1.0f / projMat(0, 0), -1.0f / projMat(1, 1) },

		.NDCToViewMul = { 2.0f / projMat(0, 0), -2.0f / projMat(1, 1) },
		.NDCToViewAdd = { -1.0f / projMat(0, 0), 1.0f / projMat(1, 1) },

		.SliceCount = settings.SliceCount,
		.StepsPerSlice = settings.StepsPerSlice,

		.EffectRadius = settings.EffectRadius,
		.EffectFalloffRange = settings.EffectFalloffRange,
		.RadiusMultiplier = 1.f,
		.FinalValuePower = settings.FinalValuePower,
		.SampleDistributionPower = settings.SampleDistributionPower,
		.ThinOccluderCompensation = settings.ThinOccluderCompensation,
		.DepthMIPSamplingOffset = settings.DepthMIPSamplingOffset,
		.NoiseIndex = (int32_t)viewport->uiFrameCount,
	};
	ssgi_cb_contents.ViewportPixelSize = { 1.f / ssgi_cb_contents.ViewportSize[0], 1.f / ssgi_cb_contents.ViewportSize[1] };
	ssgi_cb_contents.NDCToViewMul_x_PixelSize = { ssgi_cb_contents.NDCToViewMul.x * ssgi_cb_contents.ViewportPixelSize.x,
		ssgi_cb_contents.NDCToViewMul.y * ssgi_cb_contents.ViewportPixelSize.y };

	ssgiCB->Update(ssgi_cb_contents);
}

void ScreenSpaceGI::Draw(const RE::BSShader* shader, const uint32_t)
{
	if (shader->shaderType.get() != RE::BSShader::Type::Lighting)
		return;

	static FrameChecker frame_checker;
	if (frame_checker.isNewFrame()) {
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();

		ID3D11RenderTargetView* rtvs[2];
		context->OMGetRenderTargets(2, rtvs, nullptr);
		normalSwap = rtvs[2] == renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK_SWAP].RTV;
	}
}

void ScreenSpaceGI::DrawDeferred()
{
	if (!loaded)  // need abstraction
		return;

	SetupResources();
	GenerateHilbertLUT();

	if (!settings.Enabled)
		return;

	UpdateBuffer();

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto viewport = RE::BSGraphics::State::GetSingleton();

	float dynamic_res[2] = { texGI->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale,
		texGI->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale };

	ID3D11ShaderResourceView* srvs[4] = {
		renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV,
		renderer->GetRuntimeData().renderTargets[normalSwap ? RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK_SWAP : RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK].SRV,
		texHilbertLUT->srv.get(),
		texColor->srv.get()
	};
	ID3D11Buffer* cbs[1] = { ssgiCB->CB() };
	ID3D11SamplerState* samplers[2] = { pointSampler, linearSampler };
	ID3D11UnorderedAccessView* uavs[5] = { nullptr };

	context->CSSetConstantBuffers(0, ARRAYSIZE(cbs), cbs);
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	// copy color
	context->CopySubresourceRegion(texColor->resource.get(), 0, 0, 0, 0,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP].texture, 0, nullptr);
	context->GenerateMips(texColor->srv.get());

	// prefilter depths
	{
		memcpy(uavs, uavWorkingDepth, sizeof(void*) * ARRAYSIZE(uavWorkingDepth));

		context->CSSetShaderResources(0, 1, srvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavWorkingDepth), uavs, nullptr);
		context->CSSetShader(prefilterDepthsCompute, nullptr, 0);
		context->Dispatch((uint32_t)std::ceil(dynamic_res[0] / 16.0f), (uint32_t)std::ceil(dynamic_res[1] / 16.0f), 1);
	}

	memset(uavs, 0, sizeof(void*) * ARRAYSIZE(uavs));
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	// main ao/gi pass
	{
		srvs[0] = texWorkingDepth->srv.get();
		uavs[0] = texGI->uav.get();

		context->CSSetShaderResources(0, 4, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(ssgiCompute, nullptr, 0);
		context->Dispatch((uint32_t)std::ceil(dynamic_res[0] / 32.0f), (uint32_t)std::ceil(dynamic_res[1] / 32.0f), 1);
	}

	memset(srvs, 0, sizeof(void*) * ARRAYSIZE(srvs));
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	memset(uavs, 0, sizeof(void*) * ARRAYSIZE(uavs));
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	// mix
	{
		srvs[0] = texColor->srv.get();
		srvs[1] = texGI->srv.get();
		uavs[0] = texColorMix->uav.get();

		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(mixCompute, nullptr, 0);
		context->Dispatch((uint32_t)std::ceil(dynamic_res[0] / 32.0f), (uint32_t)std::ceil(dynamic_res[1] / 32.0f), 1);
	}

	// cleanup
	memset(srvs, 0, sizeof(void*) * ARRAYSIZE(srvs));
	memset(uavs, 0, sizeof(void*) * ARRAYSIZE(uavs));
	memset(cbs, 0, sizeof(void*) * ARRAYSIZE(cbs));
	memset(samplers, 0, sizeof(void*) * ARRAYSIZE(samplers));

	context->CSSetConstantBuffers(0, ARRAYSIZE(cbs), cbs);
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	// copy back
	context->CopyResource(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP].texture, texColorMix->resource.get());
}