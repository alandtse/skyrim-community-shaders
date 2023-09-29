#include "ScreenSpaceGI.h"

#include "Util.h"

#include "SubsurfaceScattering.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceGI::Settings,
	Enabled,
	EnableGI,
	UseBitmask,
	CheckBackface,
	SliceCount,
	StepsPerSlice,
	BackfaceStrength,
	EffectRadius,
	EffectFalloffRange,
	SampleDistributionPower,
	ThinOccluderCompensation,
	DepthMIPSamplingOffset,
	Thickness,
	BackfaceStrength,
	GIBounceFade,
	GIDistanceCompensation,
	GICompensationMaxDist,
	AOClamp,
	AOPower,
	AORemap,
	GIStrength,
	DebugView)

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

class DisableIf
{
private:
	bool disable;

public:
	DisableIf(bool disable) :
		disable(disable)
	{
		if (disable)
			ImGui::BeginDisabled();
	}
	~DisableIf()
	{
		if (disable)
			ImGui::EndDisabled();
	}

	operator bool() { return true; }
};

bool percentageSlider(const char* label, float* data)
{
	float percentageData = (*data) * 1e2f;
	bool retval = ImGui::SliderFloat(label, &percentageData, 0.f, 100.f, "%.1f %%");
	(*data) = percentageData * 1e-2f;
	return retval;
}

//////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceGI::DrawSettings()
{
	///////////////////////////////
	ImGui::SeparatorText("Toggles");

	if (ImGui::BeginTable("Toggles", 3)) {
		ImGui::TableNextColumn();
		ImGui::Checkbox("Enabled", &settings.Enabled);
		ImGui::TableNextColumn();
		ImGui::Checkbox("GI", &settings.EnableGI);
		ImGui::TableNextColumn();
		ImGui::Checkbox("Bitmask", &settings.UseBitmask);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("An alternative way to calculate AO/GI");

		ImGui::TableNextColumn();
		ImGui::Checkbox("Backface Checks", &settings.CheckBackface);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("You don't care if light is as bright at the back of objects as the front, then uncheck this to get some frames.");
		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText("Quality");

	ImGui::SliderInt("Slices", (int*)&settings.SliceCount, 1, 20);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("How many directions do the samples take. A greater value reduces noise but is more expensive.");

	ImGui::SliderInt("Steps Per Slice", (int*)&settings.StepsPerSlice, 1, 10);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("How many samples does it take in one direction. A greater value enhances the effects but is more expensive.");

	// ImGui::SliderInt("Denoise Passes", (int*)&settings.DenoisePasses, 0, 3);

	///////////////////////////////
	ImGui::SeparatorText("Composition");

	ImGui::SliderFloat2("AO Clamp", &settings.AOClamp.x, 0.f, 1.f, "%.2f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Clamps Raw AO visibility. Usually don't need change");

	ImGui::SliderFloat("AO Power", &settings.AOPower, 0.5f, 5.0f, "%.2f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Applies power function within the clamped range.");

	ImGui::SliderFloat2("AO Remap", &settings.AORemap.x, 0.f, 1.f, "%.2f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Remaps the clamped value to this range. The first parameter is basically inverted AO strength.");

	percentageSlider("Direct Light AO", &settings.DirectLightAO);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("AO usually influences ambient lights only, but you can make it affect direct lights too if you wish.");

	if (auto _ = DisableIf(!settings.EnableGI))
		ImGui::SliderFloat("GI Strength", &settings.GIStrength, 0.f, 5.f, "%.2f");

	///////////////////////////////
	ImGui::SeparatorText("Visual");

	ImGui::SliderFloat("Effect radius", &settings.EffectRadius, 10.f, 500.0f, "%.1f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("World (viewspace) effect radius. Depends on the scene & requirements");

	ImGui::SliderFloat("Sample Distribution Power", &settings.SampleDistributionPower, 1.f, 3.f, "%.2f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Make samples on a slice equally distributed (1.0) or focus more towards the center (>1.0)");

	ImGui::SliderFloat("MIP Sampling Offset", &settings.DepthMIPSamplingOffset, 2.f, 6.f, "%.2f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Mainly performance (texture memory bandwidth) setting but as a side-effect reduces overshadowing by thin objects and increases temporal instability");

	if (auto _ = DisableIf(!settings.EnableGI)) {
		percentageSlider("Ambient Light Source", &settings.AmbientSource);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How much ambient light is added as light source for GI calculation.");

		percentageSlider("GI Bounce", &settings.GIBounceFade);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How much of this frame's GI gets carried to the next frame. Simulates multiple light bounces.");

		ImGui::SliderFloat("GI Distance Compensation", &settings.GIDistanceCompensation, 0.0f, 9.0f, "%.1f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Brighten up further radiance samples that are otherwise too weak.");

		ImGui::SliderFloat("GI Compensation Distance", &settings.GICompensationMaxDist, 10.0f, 1000.0f, "%.1f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("The distance of maximal compensation/brightening.");
	}
	if (auto _ = DisableIf(!settings.EnableGI || !settings.CheckBackface)) {
		ImGui::SliderFloat("Backface Lighting Mix", &settings.BackfaceStrength, 0.f, 1.f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How bright at the back of objects is compared to the front. A small value to make up for foliage translucency.");
	}

	if (settings.UseBitmask) {
		ImGui::SliderFloat("Thickness", &settings.Thickness, 0.f, 100.0f, "%.1f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How thick the occluders are. 20 to 30 percent of effect radius is recommended.");
	} else {
		percentageSlider("Falloff Range", &settings.EffectFalloffRange);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Gently reduce sample impact as it gets out of 'Effect radius' bounds");

		ImGui::SliderFloat("Thin Occluder Compensation", &settings.ThinOccluderCompensation, 0.f, 0.7f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Slightly reduce impact of samples further back to counter the bias from depth-based (incomplete) input scene geometry data");
	}

	///////////////////////////////
	ImGui::SeparatorText("Debug");

	if (ImGui::BeginTable("Debug Views", 4)) {
		ImGui::TableNextColumn();
		ImGui::RadioButton("None", (int*)&settings.DebugView, 0);
		ImGui::TableNextColumn();
		ImGui::RadioButton("AO", (int*)&settings.DebugView, 1);
		ImGui::TableNextColumn();
		ImGui::RadioButton("GI", (int*)&settings.DebugView, 2);
		ImGui::TableNextColumn();
		ImGui::RadioButton("AO + GI", (int*)&settings.DebugView, 3);
		ImGui::EndTable();
	}

	if (ImGui::TreeNode("Buffer Viewer")) {
		if (ImGui::TreeNode("texColor0")) {
			ImGui::Image(texColor0->srv.get(), { texColor0->desc.Width * .3f, texColor0->desc.Height * .3f });
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("texColor1")) {
			ImGui::Image(texColor1->srv.get(), { texColor1->desc.Width * .3f, texColor1->desc.Height * .3f });
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("texRadiance")) {
			ImGui::Image(texRadiance->srv.get(), { texRadiance->desc.Width * .3f, texRadiance->desc.Height * .3f });
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("texGI0")) {
			ImGui::Image(texGI0->srv.get(), { texGI0->desc.Width * .3f, texGI0->desc.Height * .3f });
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("texGI1")) {
			ImGui::Image(texGI1->srv.get(), { texGI1->desc.Width * .3f, texGI1->desc.Height * .3f });
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("texEdge")) {
			ImGui::Image(texEdge->srv.get(), { texEdge->desc.Width * .3f, texEdge->desc.Height * .3f });
			ImGui::TreePop();
		}

		ImGui::TreePop();
	}
}

void ScreenSpaceGI::ClearComputeShader()
{
#define CLEARCOMP(shader)  \
	if (shader)            \
		shader->Release(); \
	shader = nullptr;

	CLEARCOMP(hilbertLutCompute)
	hilbertLUTGenFlag = true;

	CLEARCOMP(prefilterDepthsCompute)
	CLEARCOMP(fetchRadianceCompute)
	CLEARCOMP(ssgiCompute)
	CLEARCOMP(ssgiBitmaskCompute)
	CLEARCOMP(denoiseCompute)
	CLEARCOMP(denoiseFinalCompute)
	CLEARCOMP(mixCompute)
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
	if (!fetchRadianceCompute)
		fetchRadianceCompute = (ID3D11ComputeShader*)Util::CompileShader(shader_path, { { "", "" } }, "cs_5_0", "CSFetchRadiance");
	if (!ssgiCompute)
		ssgiCompute = (ID3D11ComputeShader*)Util::CompileShader(shader_path, { { "", "" } }, "cs_5_0", "CSGTAO");
	if (!ssgiBitmaskCompute)
		ssgiBitmaskCompute = (ID3D11ComputeShader*)Util::CompileShader(shader_path, { { "SSGI_USE_BITMASK", "" } }, "cs_5_0", "CSGTAO");
	if (!denoiseCompute)
		denoiseCompute = (ID3D11ComputeShader*)Util::CompileShader(shader_path, { { "", "" } }, "cs_5_0", "CSDenoisePass");
	if (!denoiseFinalCompute)
		denoiseFinalCompute = (ID3D11ComputeShader*)Util::CompileShader(shader_path, { { "", "" } }, "cs_5_0", "CSDenoiseLastPass");
	if (!mixCompute)
		mixCompute = (ID3D11ComputeShader*)Util::CompileShader(shader_path, { { "", "" } }, "cs_5_0", "CSMix");

	if (!texGI0) {
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

			texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			{
				texColor0 = new Texture2D(texDesc);
				texColor0->CreateSRV(srvDesc);

				texColor1 = new Texture2D(texDesc);
				texColor1->CreateSRV(srvDesc);
				texColor1->CreateUAV(uavDesc);
			}

			texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;
			texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;

			{
				texRadiance = new Texture2D(texDesc);
				texRadiance->CreateSRV(srvDesc);
				texRadiance->CreateUAV(uavDesc);
			}

			texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
			texDesc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
			texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

			{
				texGI0 = new Texture2D(texDesc);
				texGI0->CreateSRV(srvDesc);
				texGI0->CreateUAV(uavDesc);

				texGI1 = new Texture2D(texDesc);
				texGI1->CreateSRV(srvDesc);
				texGI1->CreateUAV(uavDesc);
			}

			texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32_FLOAT;

			{
				texEdge = new Texture2D(texDesc);
				texEdge->CreateSRV(srvDesc);
				texEdge->CreateUAV(uavDesc);
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
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void ScreenSpaceGI::Save(json& o_json)
{
	o_json[GetName()] = settings;
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

		hilbertLUTGenFlag = false;
	}
}

void ScreenSpaceGI::UpdateBuffer()
{
	auto viewport = RE::BSGraphics::State::GetSingleton();

	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	auto projMat = (!REL::Module::IsVR()) ? state->GetRuntimeData().cameraData.getEye().projMat :
	                                        state->GetVRRuntimeData().cameraData.getEye().projMat;

	SSGICB ssgi_cb_contents = {
		.ViewportSize = { (int32_t)(texGI0->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale),
			(int32_t)(texGI0->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale) },
		.ViewportPixelSize = { 1.f / texGI0->desc.Width, 1.f / texGI0->desc.Height },
		.DepthUnpackConsts = { -projMat(3, 2), projMat(2, 2) },
		.CameraTanHalfFOV = { 1.0f / projMat(0, 0), -1.0f / projMat(1, 1) },

		.NDCToViewMul = { 2.0f / projMat(0, 0), -2.0f / projMat(1, 1) },
		.NDCToViewAdd = { -1.0f / projMat(0, 0), 1.0f / projMat(1, 1) },

		.SliceCount = settings.SliceCount,
		.StepsPerSlice = settings.StepsPerSlice,

		.EffectRadius = settings.EffectRadius,
		.EffectFalloffRange = settings.EffectFalloffRange,
		.RadiusMultiplier = 1.f,
		.DenoiseBlurBeta = 1.2f,
		.SampleDistributionPower = settings.SampleDistributionPower,
		.ThinOccluderCompensation = settings.ThinOccluderCompensation,
		.DepthMIPSamplingOffset = settings.DepthMIPSamplingOffset,
		.NoiseIndex = (int32_t)viewport->uiFrameCount,

		.Thickness = settings.Thickness,

		.EnableGI = settings.EnableGI,
		.CheckBackface = settings.CheckBackface,
		.BackfaceStrength = settings.BackfaceStrength,
		.GIBounceFade = settings.GIBounceFade,
		.GIDistanceCompensation = settings.GIDistanceCompensation,

		.AOClamp = settings.AOClamp,
		.AOPower = settings.AOPower,
		.AORemap = settings.AORemap,
		.GIStrength = settings.GIStrength,

		.DebugView = settings.DebugView,

		.GICompensationMaxDist = settings.GICompensationMaxDist,
		.AmbientSource = settings.AmbientSource,
		.DirectLightAO = settings.DirectLightAO
	};
	ssgi_cb_contents.NDCToViewMul_x_PixelSize = {
		ssgi_cb_contents.NDCToViewMul.x * ssgi_cb_contents.ViewportPixelSize.x,
		ssgi_cb_contents.NDCToViewMul.y * ssgi_cb_contents.ViewportPixelSize.y
	};

	ssgiCB->Update(ssgi_cb_contents);
}

void ScreenSpaceGI::Draw(const RE::BSShader* shader, const uint32_t)
{
	if (shader->shaderType.get() != RE::BSShader::Type::Lighting)
		return;

	static FrameChecker frame_checker;
	static bool hasIdea = false;
	if (frame_checker.isNewFrame())
		hasIdea = false;
	if (!hasIdea) {
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();

		ID3D11RenderTargetView* rtvs[3];
		context->OMGetRenderTargets(3, rtvs, nullptr);
		if (rtvs[2] == renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK_SWAP].RTV) {
			normalSwap = true;
			hasIdea = true;
		} else if (rtvs[2] == renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK].RTV) {
			normalSwap = false;
			hasIdea = true;
		}
	}
}

void ScreenSpaceGI::DrawDeferred()
{
	if (!loaded)  // need abstraction
		return;

	if (!settings.Enabled)
		return;

	SetupResources();
	GenerateHilbertLUT();

	UpdateBuffer();

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto viewport = RE::BSGraphics::State::GetSingleton();

	float dynamic_res[2] = { texGI0->desc.Width * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale,
		texGI0->desc.Height * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale };

	ID3D11ShaderResourceView* srvs[5] = {
		renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
	};
	ID3D11UnorderedAccessView* uavs[5] = { nullptr };
	ID3D11Buffer* cbs[1] = { ssgiCB->CB() };
	ID3D11SamplerState* samplers[2] = { pointSampler, linearSampler };

	auto resetVs = [&]() {
		memset(srvs, 0, sizeof(void*) * ARRAYSIZE(srvs));
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		memset(uavs, 0, sizeof(void*) * ARRAYSIZE(uavs));
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
	};

	context->CSSetConstantBuffers(0, ARRAYSIZE(cbs), cbs);
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	// copy color
	context->CopySubresourceRegion(texColor0->resource.get(), 0, 0, 0, 0,
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP].texture, 0, nullptr);

	// prefilter depths
	{
		memcpy(uavs, uavWorkingDepth, sizeof(void*) * ARRAYSIZE(uavWorkingDepth));

		context->CSSetShaderResources(0, 1, srvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		context->CSSetShader(prefilterDepthsCompute, nullptr, 0);
		context->Dispatch((uint32_t)std::ceil(dynamic_res[0] / 16.0f), (uint32_t)std::ceil(dynamic_res[1] / 16.0f), 1);
	}

	resetVs();

	// fetch radiance
	{
		srvs[0] = texColor0->srv.get();
		srvs[1] = SubsurfaceScattering::GetSingleton()->ambientTexture->srv.get();
		srvs[2] = texGI0->srv.get();
		srvs[3] = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].SRV;
		uavs[0] = texRadiance->uav.get();

		context->CSSetShaderResources(0, 4, srvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		context->CSSetShader(fetchRadianceCompute, nullptr, 0);
		context->Dispatch((uint32_t)std::ceil(dynamic_res[0] / 32.0f), (uint32_t)std::ceil(dynamic_res[1] / 32.0f), 1);

		context->GenerateMips(texRadiance->srv.get());
	}

	resetVs();

	// main ao/gi pass
	{
		srvs[0] = texWorkingDepth->srv.get();
		srvs[1] = renderer->GetRuntimeData().renderTargets[normalSwap ? RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK_SWAP : RE::RENDER_TARGETS::kNORMAL_TAAMASK_SSRMASK].SRV;
		srvs[2] = texHilbertLUT->srv.get();
		srvs[3] = SubsurfaceScattering::GetSingleton()->albedoTexture->srv.get();
		srvs[4] = texRadiance->srv.get();
		uavs[0] = texGI0->uav.get();
		uavs[1] = texEdge->uav.get();

		context->CSSetShaderResources(0, 5, srvs);
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
		context->CSSetShader(settings.UseBitmask ? ssgiBitmaskCompute : ssgiCompute, nullptr, 0);
		context->Dispatch((uint32_t)std::ceil(dynamic_res[0] / 32.0f), (uint32_t)std::ceil(dynamic_res[1] / 32.0f), 1);
	}

	resetVs();

	// denoise
	bool isFinal0 = true;
	for (uint32_t i = 0; i < settings.DenoisePasses; ++i) {
		if (isFinal0) {
			srvs[0] = texGI0->srv.get();
			uavs[0] = texGI1->uav.get();
		} else {
			srvs[0] = texGI1->srv.get();
			uavs[0] = texGI0->uav.get();
		}
		srvs[1] = texEdge->srv.get();

		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(i + 1 == settings.DenoisePasses ? denoiseFinalCompute : denoiseCompute, nullptr, 0);
		context->Dispatch((uint32_t)std::ceil(dynamic_res[0] / 16.0f), (uint32_t)std::ceil(dynamic_res[1] / 16.0f), 1);

		resetVs();

		isFinal0 = !isFinal0;
	}

	// mix
	{
		srvs[0] = texColor0->srv.get();
		srvs[1] = isFinal0 ? texGI0->srv.get() : texGI1->srv.get();
		srvs[2] = SubsurfaceScattering::GetSingleton()->ambientTexture->srv.get();
		uavs[0] = texColor1->uav.get();

		context->CSSetShaderResources(0, 3, srvs);
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
	context->CopyResource(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSNOW_SWAP].texture, texColor1->resource.get());
}