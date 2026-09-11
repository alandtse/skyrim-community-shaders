#include "Wind.h"

#include "Globals.h"
#include "GpuPass.h"
#include "State.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

namespace
{
	static_assert(kTreeWindSpringFieldCount == WindSettingsLimits::kGrassWindSpringQualityRangeCount);
	constexpr UINT kTreeWindSpringVertexConstantBufferSlot = 3;
	constexpr UINT kTreeWindSpringCurrentTextureSlot = 111;
	constexpr UINT kTreeWindSpringPreviousTextureSlot = 114;
	constexpr UINT kTreeWindTransientCurrentTextureSlot = 119;
	constexpr UINT kTreeWindTransientPreviousTextureSlot = 122;
	constexpr UINT kTreeWindSpringSamplerSlot = 14;
	static_assert(kTreeWindTransientPreviousTextureSlot + kTreeWindSpringFieldCount <=
				  D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
	enum class TreeWindSpringPass
	{
		Near,
		Mid,
		Far
	};
	constexpr std::array<TreeWindSpringPass, kTreeWindSpringFieldCount> kTreeWindSpringFieldPasses{
		TreeWindSpringPass::Near,
		TreeWindSpringPass::Mid,
		TreeWindSpringPass::Far
	};

	void DispatchTreeWindSpring(
		ID3D11DeviceContext* a_context, UINT a_width, UINT a_height, UINT a_depth, TreeWindSpringPass a_pass)
	{
		switch (a_pass) {
		case TreeWindSpringPass::Near:
			{
				CS_GPU_PASS("Wind::TreeWindSpringNearUpdate");
				a_context->Dispatch(a_width, a_height, a_depth);
				break;
			}
		case TreeWindSpringPass::Mid:
			{
				CS_GPU_PASS("Wind::TreeWindSpringMidUpdate");
				a_context->Dispatch(a_width, a_height, a_depth);
				break;
			}
		case TreeWindSpringPass::Far:
			{
				CS_GPU_PASS("Wind::TreeWindSpringFarUpdate");
				a_context->Dispatch(a_width, a_height, a_depth);
				break;
			}
		}
	}

	std::unique_ptr<Texture2D> CreateTreeWindSpringTexture(uint32_t a_textureSize, const std::string& a_name)
	{
		D3D11_TEXTURE2D_DESC textureDesc{
			.Width = a_textureSize,
			.Height = a_textureSize,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		auto texture = std::make_unique<Texture2D>(textureDesc, a_name.c_str());
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = textureDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		texture->CreateSRV(srvDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = textureDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		texture->CreateUAV(uavDesc);
		const float clearValue[4]{};
		globals::d3d::context->ClearUnorderedAccessViewFloat(texture->uav.get(), clearValue);
		return texture;
	}

	std::unique_ptr<Texture2D> CreateTreeWindTransientTexture(uint32_t a_textureSize, const std::string& a_name)
	{
		D3D11_TEXTURE2D_DESC textureDesc{
			.Width = a_textureSize,
			.Height = a_textureSize,
			.MipLevels = 1,
			.ArraySize = kTreeWindTransientHeightCount,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		auto texture = std::make_unique<Texture2D>(textureDesc, a_name.c_str());
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = textureDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MostDetailedMip = 0;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.FirstArraySlice = 0;
		srvDesc.Texture2DArray.ArraySize = kTreeWindTransientHeightCount;
		texture->CreateSRV(srvDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = textureDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.MipSlice = 0;
		uavDesc.Texture2DArray.FirstArraySlice = 0;
		uavDesc.Texture2DArray.ArraySize = kTreeWindTransientHeightCount;
		texture->CreateUAV(uavDesc);
		const float clearValue[4]{};
		globals::d3d::context->ClearUnorderedAccessViewFloat(texture->uav.get(), clearValue);
		return texture;
	}

	std::unique_ptr<Texture2D> CreateTreeWindTransientVelocityTexture(
		uint32_t a_textureSize, const std::string& a_name)
	{
		D3D11_TEXTURE2D_DESC textureDesc{
			.Width = a_textureSize,
			.Height = a_textureSize,
			.MipLevels = 1,
			.ArraySize = kTreeWindTransientHeightCount,
			.Format = DXGI_FORMAT_R16G16_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		auto texture = std::make_unique<Texture2D>(textureDesc, a_name.c_str());
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = textureDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MostDetailedMip = 0;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.FirstArraySlice = 0;
		srvDesc.Texture2DArray.ArraySize = kTreeWindTransientHeightCount;
		texture->CreateSRV(srvDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = textureDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.MipSlice = 0;
		uavDesc.Texture2DArray.FirstArraySlice = 0;
		uavDesc.Texture2DArray.ArraySize = kTreeWindTransientHeightCount;
		texture->CreateUAV(uavDesc);
		const float clearValue[4]{};
		globals::d3d::context->ClearUnorderedAccessViewFloat(texture->uav.get(), clearValue);
		return texture;
	}

	bool AdvanceTreeWindSpringTextures(
		ID3D11DeviceContext* a_context,
		ID3D11ComputeShader* a_shader,
		TreeWindState::TexturePair& a_responseTextures,
		TreeWindState::TexturePair& a_velocityTextures,
		TreeWindState::TexturePair& a_transientTextures,
		TreeWindState::TexturePair& a_transientVelocityTextures,
		uint32_t& a_textureIndex,
		bool& a_initialized,
		UINT a_dispatchWidth,
		UINT a_dispatchHeight,
		UINT a_dispatchDepth,
		TreeWindSpringPass a_pass)
	{
		const uint32_t currentTextureIndex = a_textureIndex;
		const uint32_t outputTextureIndex = currentTextureIndex ^ 1u;
		ID3D11ShaderResourceView* srvs[]{
			a_responseTextures[currentTextureIndex]->srv.get(),
			a_velocityTextures[currentTextureIndex]->srv.get(),
			a_transientTextures[currentTextureIndex]->srv.get(),
			a_transientVelocityTextures[currentTextureIndex]->srv.get()
		};
		ID3D11UnorderedAccessView* uavs[]{
			a_responseTextures[outputTextureIndex]->uav.get(),
			a_velocityTextures[outputTextureIndex]->uav.get(),
			a_transientTextures[outputTextureIndex]->uav.get(),
			a_transientVelocityTextures[outputTextureIndex]->uav.get()
		};
		a_context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		a_context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		if (!a_shader) {
			a_initialized = false;
			return false;
		}

		a_context->CSSetShader(a_shader, nullptr, 0);
		DispatchTreeWindSpring(a_context, a_dispatchWidth, a_dispatchHeight, a_dispatchDepth, a_pass);
		a_textureIndex = outputTextureIndex;
		if (!a_initialized) {
			ID3D11ShaderResourceView* nullSrvs[4]{};
			ID3D11UnorderedAccessView* nullUavs[4]{};
			a_context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
			a_context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
			const uint32_t previousTextureIndex = a_textureIndex ^ 1u;
			a_context->CopyResource(
				a_responseTextures[previousTextureIndex]->resource.get(),
				a_responseTextures[a_textureIndex]->resource.get());
			a_context->CopyResource(
				a_velocityTextures[previousTextureIndex]->resource.get(),
				a_velocityTextures[a_textureIndex]->resource.get());
			a_context->CopyResource(
				a_transientTextures[previousTextureIndex]->resource.get(),
				a_transientTextures[a_textureIndex]->resource.get());
			a_context->CopyResource(
				a_transientVelocityTextures[previousTextureIndex]->resource.get(),
				a_transientVelocityTextures[a_textureIndex]->resource.get());
		}
		a_initialized = true;
		return true;
	}
}

void Wind::RecreateTreeWindSpringTextures(uint32_t a_qualityIndex)
{
	if (a_qualityIndex >= kTreeWindSpringFieldCount)
		return;

	ID3D11ShaderResourceView* nullSrv = nullptr;
	globals::d3d::context->VSSetShaderResources(
		kTreeWindSpringCurrentTextureSlot + a_qualityIndex, 1, &nullSrv);
	globals::d3d::context->VSSetShaderResources(
		kTreeWindSpringPreviousTextureSlot + a_qualityIndex, 1, &nullSrv);
	globals::d3d::context->VSSetShaderResources(
		kTreeWindTransientCurrentTextureSlot + a_qualityIndex, 1, &nullSrv);
	globals::d3d::context->VSSetShaderResources(
		kTreeWindTransientPreviousTextureSlot + a_qualityIndex, 1, &nullSrv);
	ID3D11ShaderResourceView* nullSrvs[4]{};
	globals::d3d::context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
	ID3D11UnorderedAccessView* nullUavs[4]{};
	globals::d3d::context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
	globals::d3d::context->CSSetShader(nullptr, nullptr, 0);

	const uint32_t textureSize = kTreeWindSpringTextureSizes[a_qualityIndex];
	for (uint32_t textureIndex = 0; textureIndex < 2; ++textureIndex) {
		const std::string responseName = std::format(
			"Wind::TreeWindSpring{}Response{}", WindSettingsLimits::kGrassWindSpringQualityRangeNames[a_qualityIndex], textureIndex);
		treeState.springResponseTextures[a_qualityIndex][textureIndex] =
			CreateTreeWindSpringTexture(textureSize, responseName);
		const std::string velocityName = std::format(
			"Wind::TreeWindSpring{}Velocity{}", WindSettingsLimits::kGrassWindSpringQualityRangeNames[a_qualityIndex], textureIndex);
		treeState.springVelocityTextures[a_qualityIndex][textureIndex] =
			CreateTreeWindSpringTexture(textureSize, velocityName);
		const std::string transientName = std::format(
			"Wind::TreeWindTransient{}{}", WindSettingsLimits::kGrassWindSpringQualityRangeNames[a_qualityIndex], textureIndex);
		treeState.transientTextures[a_qualityIndex][textureIndex] =
			CreateTreeWindTransientTexture(textureSize, transientName);
		const std::string transientVelocityName = std::format(
			"Wind::TreeWindTransient{}Velocity{}",
			WindSettingsLimits::kGrassWindSpringQualityRangeNames[a_qualityIndex],
			textureIndex);
		treeState.transientVelocityTextures[a_qualityIndex][textureIndex] =
			CreateTreeWindTransientVelocityTexture(textureSize, transientVelocityName);
	}
	treeState.springTextureIndices[a_qualityIndex] = 0;
	treeState.springInitialized[a_qualityIndex] = false;
	treeState.springFieldAvailable[a_qualityIndex] = false;
}

void Wind::SetupTreeWindResources()
{
	treeState.hasUpdated = false;
	treeState.springConstantBuffer = std::make_unique<ConstantBuffer>(
		ConstantBufferDesc<TreeWindSpringData>(), "Wind::TreeWindSpringData");
	for (uint32_t qualityIndex = 0; qualityIndex < kTreeWindSpringFieldCount; ++qualityIndex)
		RecreateTreeWindSpringTextures(qualityIndex);
	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, treeState.springSampler.put()));
	Util::SetResourceName(treeState.springSampler.get(), "Wind::TreeWindSpringSampler");
}

void Wind::UpdateTreeWindSpring()
{
	if (!treeState.springConstantBuffer || !treeState.springSampler.get() || !globals::state)
		return;
	for (uint32_t qualityIndex = 0; qualityIndex < kTreeWindSpringFieldCount; ++qualityIndex) {
		if (!treeState.springResponseTextures[qualityIndex][0] ||
			!treeState.springResponseTextures[qualityIndex][1] ||
			!treeState.springVelocityTextures[qualityIndex][0] ||
			!treeState.springVelocityTextures[qualityIndex][1] ||
			!treeState.transientTextures[qualityIndex][0] ||
			!treeState.transientTextures[qualityIndex][1] ||
			!treeState.transientVelocityTextures[qualityIndex][0] ||
			!treeState.transientVelocityTextures[qualityIndex][1])
			return;
	}
	auto* context = globals::d3d::context;
	const uint32_t currentFrame = globals::state->frameCount;
	const bool newFrame = !treeState.hasUpdated || treeState.lastUpdateFrame != currentFrame;
	if (newFrame) {
		const bool historyContinuous = treeState.hasUpdated && currentFrame - treeState.lastUpdateFrame == 1u;
		if (!historyContinuous) {
			treeState.springInitialized.fill(false);
		}
		treeState.hasUpdated = true;
		treeState.lastUpdateFrame = currentFrame;

		std::array<ID3D11ShaderResourceView*, 14> nullVertexSrvs{};
		context->VSSetShaderResources(
			kTreeWindSpringCurrentTextureSlot, static_cast<UINT>(nullVertexSrvs.size()), nullVertexSrvs.data());

		RE::NiPoint3 center{};
		if (globals::game::player)
			center = globals::game::player->GetPosition();
		// The far-cell snap moves every cascade by whole texels with the configured resolutions.
		const uint32_t anchorFieldIndex = kTreeWindSpringFieldCount - 1;
		const float anchorCellSize = kTreeWindSpringMaximumDistances[anchorFieldIndex] * 2.0f /
		                             static_cast<float>(kTreeWindSpringTextureSizes[anchorFieldIndex]);
		const float2 snappedCenter{
			std::floor(center.x / anchorCellSize) * anchorCellSize,
			std::floor(center.y / anchorCellSize) * anchorCellSize
		};

		TreeWindSpringData data{};
		const float frameTime = std::clamp(globals::state->windFieldFrameTime, 0.0f, 0.25f);
		for (uint32_t qualityIndex = 0; qualityIndex < kTreeWindSpringFieldCount; ++qualityIndex) {
			const float fieldSize = kTreeWindSpringMaximumDistances[qualityIndex] * 2.0f;
			const float fieldHalfSize = fieldSize * 0.5f;
			const float2 nextFieldMinimum{
				snappedCenter.x - fieldHalfSize,
				snappedCenter.y - fieldHalfSize
			};
			treeState.previousSpringFieldMinimum[qualityIndex] = treeState.springInitialized[qualityIndex] ?
			                                                         treeState.springFieldMinimum[qualityIndex] :
			                                                         nextFieldMinimum;
			treeState.springFieldMinimum[qualityIndex] = nextFieldMinimum;
			treeState.previousSpringFieldHeight[qualityIndex] = treeState.springInitialized[qualityIndex] ?
			                                                        treeState.springFieldHeight[qualityIndex] :
			                                                        center.z;
			treeState.springFieldHeight[qualityIndex] = center.z;
			data.fields[qualityIndex] = {
				treeState.springFieldMinimum[qualityIndex],
				treeState.previousSpringFieldMinimum[qualityIndex],
				center.z,
				frameTime,
				fieldSize,
				kTreeWindSpringTextureSizes[qualityIndex],
				treeState.springInitialized[qualityIndex] ? 0u : 1u,
				0u,
				kTreeWindSpringMaximumDistances[qualityIndex],
				treeState.previousSpringFieldHeight[qualityIndex]
			};
		}
		data.springFrequency = settings.treeWindSpringFrequency;
		data.springDamping = settings.treeWindSpringDamping;
		data.gustScale = settings.treeWindGustScale;
		data.gustSoftLimit = settings.treeWindGustSoftLimit;
		data.transientFieldMask = GetTransientFieldMask();
		data.transientSpringFrequency = settings.treeTransientSpringFrequency;
		data.transientSpringDamping = settings.treeTransientSpringDamping;

		ID3D11Buffer* constantBuffers[]{ treeState.springConstantBuffer->CB(), globals::state->sharedDataCB->CB() };
		context->CSSetConstantBuffers(0, 1, constantBuffers);
		context->CSSetConstantBuffers(5, 1, constantBuffers + 1);
		auto* fieldShader = treeState.springFieldComputeShader.Get(
			L"Data\\Shaders\\TreeWindSpringCS.hlsl", {}, "cs_5_0", "UpdateField",
			"Wind::TreeWindSpringCS");
		ID3D11ShaderResourceView* nullSrvs[4]{};
		ID3D11UnorderedAccessView* nullUavs[4]{};

		for (uint32_t qualityIndex = 0; qualityIndex < kTreeWindSpringFieldCount; ++qualityIndex) {
			data.activeField = qualityIndex;
			data.fields[qualityIndex].fieldAvailable = 0u;
			treeState.springConstantBuffer->Update(data);
			treeState.springFieldAvailable[qualityIndex] = AdvanceTreeWindSpringTextures(
				context, fieldShader,
				treeState.springResponseTextures[qualityIndex],
				treeState.springVelocityTextures[qualityIndex],
				treeState.transientTextures[qualityIndex],
				treeState.transientVelocityTextures[qualityIndex],
				treeState.springTextureIndices[qualityIndex],
				treeState.springInitialized[qualityIndex],
				(data.fields[qualityIndex].textureSize + 7) / 8,
				(data.fields[qualityIndex].textureSize + 7) / 8,
				kTreeWindTransientHeightCount,
				kTreeWindSpringFieldPasses[qualityIndex]);
		}

		context->CSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);
		context->CSSetConstantBuffers(5, 1, &nullBuffer);
		context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);

		data.activeField = 0u;
		for (uint32_t qualityIndex = 0; qualityIndex < kTreeWindSpringFieldCount; ++qualityIndex) {
			data.fields[qualityIndex].initialize = 0u;
			data.fields[qualityIndex].fieldAvailable = treeState.springFieldAvailable[qualityIndex] ? 1u : 0u;
		}
		treeState.springConstantBuffer->Update(data);
	}

	ID3D11Buffer* springBuffer = treeState.springConstantBuffer->CB();
	context->VSSetConstantBuffers(kTreeWindSpringVertexConstantBufferSlot, 1, &springBuffer);
	std::array<ID3D11ShaderResourceView*, kTreeWindSpringFieldCount> currentSpringSrvs{};
	std::array<ID3D11ShaderResourceView*, kTreeWindSpringFieldCount> previousSpringSrvs{};
	std::array<ID3D11ShaderResourceView*, kTreeWindSpringFieldCount> currentTransientSrvs{};
	std::array<ID3D11ShaderResourceView*, kTreeWindSpringFieldCount> previousTransientSrvs{};
	for (uint32_t qualityIndex = 0; qualityIndex < kTreeWindSpringFieldCount; ++qualityIndex) {
		const uint32_t currentTextureIndex = treeState.springTextureIndices[qualityIndex];
		currentSpringSrvs[qualityIndex] = treeState.springResponseTextures[qualityIndex][currentTextureIndex]->srv.get();
		previousSpringSrvs[qualityIndex] = treeState.springResponseTextures[qualityIndex][currentTextureIndex ^ 1u]->srv.get();
		currentTransientSrvs[qualityIndex] = treeState.transientTextures[qualityIndex][currentTextureIndex]->srv.get();
		previousTransientSrvs[qualityIndex] = treeState.transientTextures[qualityIndex][currentTextureIndex ^ 1u]->srv.get();
	}
	context->VSSetShaderResources(kTreeWindSpringCurrentTextureSlot,
		static_cast<UINT>(currentSpringSrvs.size()), currentSpringSrvs.data());
	context->VSSetShaderResources(kTreeWindSpringPreviousTextureSlot,
		static_cast<UINT>(previousSpringSrvs.size()), previousSpringSrvs.data());
	context->VSSetShaderResources(kTreeWindTransientCurrentTextureSlot,
		static_cast<UINT>(currentTransientSrvs.size()), currentTransientSrvs.data());
	context->VSSetShaderResources(kTreeWindTransientPreviousTextureSlot,
		static_cast<UINT>(previousTransientSrvs.size()), previousTransientSrvs.data());
	ID3D11SamplerState* samplers[]{ treeState.springSampler.get() };
	context->VSSetSamplers(kTreeWindSpringSamplerSlot, ARRAYSIZE(samplers), samplers);
}
