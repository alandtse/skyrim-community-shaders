#include "Wind.h"

#include "Globals.h"
#include "GpuPass.h"
#include "State.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string_view>

using namespace WindSettingsLimits;

namespace
{
	constexpr UINT kGrassWindSpringVertexConstantBufferSlot = 3;
}

void Wind::RecreateGrassWindSpringTextures(uint32_t a_qualityIndex, uint32_t a_textureSize)
{
	if (a_qualityIndex >= kGrassWindSpringQualityRangeCount)
		return;

	ID3D11ShaderResourceView* nullSrvs[2]{};
	globals::d3d::context->VSSetShaderResources(105 + a_qualityIndex, 1, nullSrvs);
	globals::d3d::context->VSSetShaderResources(108 + a_qualityIndex, 1, nullSrvs);
	globals::d3d::context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
	ID3D11UnorderedAccessView* nullUavs[2]{};
	globals::d3d::context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
	globals::d3d::context->CSSetShader(nullptr, nullptr, 0);

	for (uint32_t textureIndex = 0; textureIndex < 2; ++textureIndex) {
		grassState.springResponseTextures[a_qualityIndex][textureIndex].reset();
		grassState.springVelocityTextures[a_qualityIndex][textureIndex].reset();
	}

	D3D11_TEXTURE2D_DESC textureDesc{
		.Width = a_textureSize,
		.Height = a_textureSize,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{
		.Format = textureDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{
		.Format = textureDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};
	const float clearValue[4]{};
	for (uint32_t textureIndex = 0; textureIndex < 2; ++textureIndex) {
		const std::string responseName = std::format(
			"Wind::GrassWindSpring{}Response{}", kGrassWindSpringQualityRangeNames[a_qualityIndex], textureIndex);
		grassState.springResponseTextures[a_qualityIndex][textureIndex] = std::make_unique<Texture2D>(textureDesc, responseName.c_str());
		grassState.springResponseTextures[a_qualityIndex][textureIndex]->CreateSRV(srvDesc);
		grassState.springResponseTextures[a_qualityIndex][textureIndex]->CreateUAV(uavDesc);
		globals::d3d::context->ClearUnorderedAccessViewFloat(
			grassState.springResponseTextures[a_qualityIndex][textureIndex]->uav.get(), clearValue);

		const std::string velocityName = std::format(
			"Wind::GrassWindSpring{}Velocity{}", kGrassWindSpringQualityRangeNames[a_qualityIndex], textureIndex);
		grassState.springVelocityTextures[a_qualityIndex][textureIndex] = std::make_unique<Texture2D>(textureDesc, velocityName.c_str());
		grassState.springVelocityTextures[a_qualityIndex][textureIndex]->CreateSRV(srvDesc);
		grassState.springVelocityTextures[a_qualityIndex][textureIndex]->CreateUAV(uavDesc);
		globals::d3d::context->ClearUnorderedAccessViewFloat(
			grassState.springVelocityTextures[a_qualityIndex][textureIndex]->uav.get(), clearValue);
	}
	grassState.springTextureSizes[a_qualityIndex] = a_textureSize;
	grassState.springTextureIndices[a_qualityIndex] = 0;
	grassState.springInitialized[a_qualityIndex] = false;
	grassState.springFieldAvailable[a_qualityIndex] = false;
}

void Wind::SetupGrassWindResources()
{
	grassState.springConstantBuffer = std::make_unique<ConstantBuffer>(
		ConstantBufferDesc<GrassWindSpringData>(), "Wind::GrassWindSpringData");
	Settings sanitizedSettings = settings;
	SanitizeSettings(sanitizedSettings);
	for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex)
		RecreateGrassWindSpringTextures(qualityIndex, sanitizedSettings.grassWindSpringQuality[qualityIndex].textureSize);

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, grassState.springSampler.put()));
	Util::SetResourceName(grassState.springSampler.get(), "Wind::GrassWindSpringSampler");
}

void Wind::ClearShaderCache()
{
	grassState.springComputeShader.Reset();
	treeState.springFieldComputeShader.Reset();
}

void Wind::UpdateGrassWindSpring(bool a_compute)
{
	if (!grassState.springConstantBuffer)
		return;
	for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex) {
		if (!grassState.springResponseTextures[qualityIndex][0] || !grassState.springResponseTextures[qualityIndex][1] ||
			!grassState.springVelocityTextures[qualityIndex][0] || !grassState.springVelocityTextures[qualityIndex][1])
			return;
	}

	static Util::FrameChecker frameChecker;
	auto* context = globals::d3d::context;
	if (frameChecker.IsNewFrame()) {
		Settings sanitizedSettings = settings;
		SanitizeSettings(sanitizedSettings);
		RE::NiPoint3 center{};
		if (globals::game::player)
			center = globals::game::player->GetPosition();
		const float anchorCellSize = sanitizedSettings.grassWindSpringQuality[0].maxDistance * 2.0f /
		                             static_cast<float>(sanitizedSettings.grassWindSpringQuality[0].textureSize);
		const float2 snappedCenter{
			std::floor(center.x / anchorCellSize) * anchorCellSize,
			std::floor(center.y / anchorCellSize) * anchorCellSize
		};

		GrassWindSpringData data{};
		data.transientFieldMask = GetTransientFieldMask();
		const float fieldHeight = center.z;
		const float frameTime = std::clamp(globals::state->windFieldFrameTime, 0.0f, 0.25f);
		const float responseRadians = DirectX::XMConvertToRadians(sanitizedSettings.grassWindResponse);
		const float maximumTiltRadians = DirectX::XMConvertToRadians(sanitizedSettings.grassWindMaximumTilt);
		const float sensitivity = sanitizedSettings.grassWindSensitivity;
		const float springFrequency = sanitizedSettings.grassWindSpringFrequency;
		const float springDamping = sanitizedSettings.grassWindSpringDamping;
		ID3D11ShaderResourceView* nullSrvs[2]{};
		ID3D11UnorderedAccessView* nullUavs[2]{};
		for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex) {
			const auto& quality = sanitizedSettings.grassWindSpringQuality[qualityIndex];
			const float fieldSize = quality.maxDistance * 2.0f;
			if (quality.textureSize != grassState.springTextureSizes[qualityIndex])
				RecreateGrassWindSpringTextures(qualityIndex, quality.textureSize);
			if (fieldSize != grassState.springWorldSizes[qualityIndex]) {
				grassState.springWorldSizes[qualityIndex] = fieldSize;
				grassState.springInitialized[qualityIndex] = false;
				grassState.springFieldAvailable[qualityIndex] = false;
			}
			const float fieldHalfSize = fieldSize * 0.5f;
			const float2 nextFieldMinimum{
				snappedCenter.x - fieldHalfSize,
				snappedCenter.y - fieldHalfSize
			};
			grassState.previousSpringFieldMinimum[qualityIndex] = grassState.springInitialized[qualityIndex] ?
			                                                          grassState.springFieldMinimum[qualityIndex] :
			                                                          nextFieldMinimum;
			grassState.springFieldMinimum[qualityIndex] = nextFieldMinimum;
			data.fields[qualityIndex] = {
				grassState.springFieldMinimum[qualityIndex],
				grassState.previousSpringFieldMinimum[qualityIndex],
				fieldHeight,
				frameTime,
				responseRadians,
				maximumTiltRadians,
				sensitivity,
				springFrequency,
				springDamping,
				grassState.springInitialized[qualityIndex] ? 0u : 1u,
				0u,
				fieldSize,
				quality.textureSize,
				quality.maxDistance
			};
		}

		ID3D11Buffer* constantBuffers[]{ grassState.springConstantBuffer->CB(), globals::state->sharedDataCB->CB() };
		context->CSSetConstantBuffers(0, 1, constantBuffers);
		context->CSSetConstantBuffers(5, 1, constantBuffers + 1);
		auto* shader = sanitizedSettings.enableAmbientGrassWind ?
		                   grassState.springComputeShader.Get(
							   L"Data\\Shaders\\GrassWindSpringCS.hlsl", {}, "cs_5_0", "main",
							   "Wind::GrassWindSpringCS") :
		                   nullptr;
		for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex) {
			data.activeField = qualityIndex;
			data.fields[qualityIndex].fieldAvailable = 0u;
			grassState.springConstantBuffer->Update(data);
			const uint32_t currentTextureIndex = grassState.springTextureIndices[qualityIndex];
			const uint32_t outputTextureIndex = currentTextureIndex ^ 1u;
			ID3D11ShaderResourceView* srvs[]{
				grassState.springResponseTextures[qualityIndex][currentTextureIndex]->srv.get(),
				grassState.springVelocityTextures[qualityIndex][currentTextureIndex]->srv.get()
			};
			context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
			ID3D11UnorderedAccessView* uavs[]{
				grassState.springResponseTextures[qualityIndex][outputTextureIndex]->uav.get(),
				grassState.springVelocityTextures[qualityIndex][outputTextureIndex]->uav.get()
			};
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			grassState.springFieldAvailable[qualityIndex] = false;
			if (shader) {
				context->CSSetShader(shader, nullptr, 0);
				CS_GPU_PASS("Wind::GrassWindSpringUpdate");
				context->Dispatch((data.fields[qualityIndex].textureSize + 7) / 8,
					(data.fields[qualityIndex].textureSize + 7) / 8, 1);
				grassState.springTextureIndices[qualityIndex] = outputTextureIndex;
				grassState.springFieldAvailable[qualityIndex] = true;
				if (!grassState.springInitialized[qualityIndex]) {
					context->CSSetShaderResources(0, ARRAYSIZE(srvs), nullSrvs);
					context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), nullUavs, nullptr);
					const uint32_t previousTextureIndex = grassState.springTextureIndices[qualityIndex] ^ 1u;
					context->CopyResource(
						grassState.springResponseTextures[qualityIndex][previousTextureIndex]->resource.get(),
						grassState.springResponseTextures[qualityIndex][grassState.springTextureIndices[qualityIndex]]->resource.get());
					context->CopyResource(
						grassState.springVelocityTextures[qualityIndex][previousTextureIndex]->resource.get(),
						grassState.springVelocityTextures[qualityIndex][grassState.springTextureIndices[qualityIndex]]->resource.get());
				}
				grassState.springInitialized[qualityIndex] = true;
			}
			if (!grassState.springFieldAvailable[qualityIndex])
				grassState.springInitialized[qualityIndex] = false;
		}

		context->CSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);
		context->CSSetConstantBuffers(5, 1, &nullBuffer);
		context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);

		data.activeField = 0u;
		for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex) {
			data.fields[qualityIndex].initialize = 0u;
			data.fields[qualityIndex].fieldAvailable = grassState.springFieldAvailable[qualityIndex] ? 1u : 0u;
		}
		grassState.springConstantBuffer->Update(data);
	}

	ID3D11Buffer* springBuffer = grassState.springConstantBuffer->CB();
	std::array<ID3D11ShaderResourceView*, kGrassWindSpringQualityRangeCount> currentSpringSrvs{};
	std::array<ID3D11ShaderResourceView*, kGrassWindSpringQualityRangeCount> previousSpringSrvs{};
	for (uint32_t qualityIndex = 0; qualityIndex < kGrassWindSpringQualityRangeCount; ++qualityIndex) {
		const uint32_t currentTextureIndex = grassState.springTextureIndices[qualityIndex];
		currentSpringSrvs[qualityIndex] = grassState.springResponseTextures[qualityIndex][currentTextureIndex]->srv.get();
		previousSpringSrvs[qualityIndex] = grassState.springResponseTextures[qualityIndex][currentTextureIndex ^ 1u]->srv.get();
	}
	ID3D11SamplerState* samplers[]{ grassState.springSampler.get() };
	if (a_compute) {
		context->CSSetConstantBuffers(kGrassWindSpringVertexConstantBufferSlot, 1, &springBuffer);
		context->CSSetShaderResources(105, static_cast<UINT>(currentSpringSrvs.size()), currentSpringSrvs.data());
		context->CSSetShaderResources(108, static_cast<UINT>(previousSpringSrvs.size()), previousSpringSrvs.data());
		context->CSSetSamplers(14, ARRAYSIZE(samplers), samplers);
	} else {
		context->VSSetConstantBuffers(kGrassWindSpringVertexConstantBufferSlot, 1, &springBuffer);
		context->VSSetShaderResources(105, static_cast<UINT>(currentSpringSrvs.size()), currentSpringSrvs.data());
		context->VSSetShaderResources(108, static_cast<UINT>(previousSpringSrvs.size()), previousSpringSrvs.data());
		context->VSSetSamplers(14, ARRAYSIZE(samplers), samplers);
	}
}

ID3D11ShaderResourceView* Wind::GetGrassWindSpringDebugSRV() const
{
	return grassState.springFieldAvailable[0] && grassState.springResponseTextures[0][grassState.springTextureIndices[0]] ?
	           grassState.springResponseTextures[0][grassState.springTextureIndices[0]]->srv.get() :
	           nullptr;
}
