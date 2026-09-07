#include "Renderer.h"

#include "D3D12Interop.h"
#include "Features/Upscaling/FoveatedRender/Ops.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/LazyShader.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace NeuralRendering
{
	namespace
	{
		constexpr std::uint32_t kEyeCount = 2;
		constexpr std::uint32_t kCascadePassCount = 3;

		std::uint32_t FeatureSlot(std::uint32_t eyeIndex, std::uint32_t passIndex)
		{
			return eyeIndex + passIndex * kEyeCount;
		}

		std::uint32_t GetPassCount(const Tuning& tuning)
		{
			return tuning.multiPass == 0 ? 1 : std::min(tuning.multiPass + 1, kCascadePassCount);
		}

		void TransitionEvaluationResources(ID3D12GraphicsCommandList* commandList,
			ID3D12Resource* input, ID3D12Resource* depth, ID3D12Resource* motionVectors,
			ID3D12Resource* output, bool entering)
		{
			D3D12_RESOURCE_BARRIER barriers[4]{};
			ID3D12Resource* resources[4]{ input, depth, motionVectors, output };
			for (std::size_t index = 0; index < std::size(barriers); ++index) {
				barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[index].Transition.pResource = resources[index];
				barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barriers[index].Transition.StateBefore = entering ? D3D12_RESOURCE_STATE_COMMON :
				                                                    (index == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				barriers[index].Transition.StateAfter = entering ?
				                                            (index == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) :
				                                            D3D12_RESOURCE_STATE_COMMON;
			}
			commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
		}

		bool GetTextureDesc(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& desc)
		{
			Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
			if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
				return false;
			texture->GetDesc(&desc);
			return true;
		}

		bool Matches(const SharedTexture& texture, const D3D11_TEXTURE2D_DESC& desc)
		{
			return texture.resource11 && texture.desc.Width == desc.Width && texture.desc.Height == desc.Height &&
			       texture.desc.Format == desc.Format && texture.desc.ArraySize == desc.ArraySize &&
			       texture.desc.MipLevels == desc.MipLevels && texture.desc.SampleDesc.Count == desc.SampleDesc.Count;
		}

		bool MatchesResolved(const Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
			const D3D11_TEXTURE2D_DESC& desc)
		{
			if (!texture)
				return false;
			D3D11_TEXTURE2D_DESC actual{};
			texture->GetDesc(&actual);
			return actual.Width == desc.Width && actual.Height == desc.Height &&
			       actual.Format == desc.Format && actual.ArraySize == desc.ArraySize &&
			       actual.MipLevels == desc.MipLevels && actual.SampleDesc.Count == desc.SampleDesc.Count;
		}

		std::uint32_t NormalizeModelResolution(std::uint32_t percent)
		{
			return percent == 33 || percent == 50 || percent == 75 || percent == 85 || percent == 90 ? percent : 100;
		}

		struct ModelResolveSettings
		{
			float transferStrength;
			float colourStrength;
			float maxRatio;
			float residualStrength;
		};

		ModelResolveSettings GetModelResolveSettings(std::uint32_t modelResolution)
		{
			// Reduced Feature 18 output is temporally less reliable around fine
			// shadow/light transitions. Near-native model resolutions have enough
			// spatial support to carry a substantially larger contribution, while
			// the aggressive 50%/33% modes stay conservative for artifact control.
			switch (modelResolution) {
			case 90:
				return { 1.00f, 0.80f, 1.60f, 0.90f };
			case 85:
				return { 0.98f, 0.70f, 1.55f, 0.84f };
			case 75:
				return { 0.90f, 0.52f, 1.50f, 0.76f };
			case 50:
				return { 0.70f, 0.22f, 1.35f, 0.65f };
			case 33:
				return { 0.60f, 0.18f, 1.20f, 0.55f };
			default:
				// Full resolution bypasses this resolve stage. Keep a safe default
				// here in case the helper is called independently in the future.
				return { 1.00f, 1.00f, 4.00f, 1.00f };
			}
		}

		std::uint32_t ScaleDimension(std::uint32_t dimension, std::uint32_t percent)
		{
			return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(
												  (static_cast<std::uint64_t>(dimension) * percent + 50) / 100));
		}

		D3D11_TEXTURE2D_DESC MakeSharedDesc(const D3D11_TEXTURE2D_DESC& source, std::uint32_t width,
			std::uint32_t height, UINT bindFlags)
		{
			auto desc = source;
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = bindFlags;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;
			return desc;
		}
	}

	class Renderer::State
	{
	public:
		struct EyeResources
		{
			SharedTexture color;
			SharedTexture modelInput;
			SharedTexture depth;
			SharedTexture motionVectors;
			std::array<SharedTexture, kCascadePassCount - 1> cascadeIntermediates;
			SharedTexture output;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> resolved;
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resolvedSRV;
			Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> resolvedUAV;
			bool reducedResolution = false;
			std::uint32_t passCount = 1;
		};

		bool Apply(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
			ID3D11Resource* color, ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
			ID3D11Resource* motionVectors,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning)
		{
			if (failureLatched || !device || !context || eyeIndex >= eyes.size() || !color || !depth || !depthSRV || !motionVectors)
				return false;
			CS_GPU_PASS("NeuralRendering::Evaluate");

			if (!interop.IsInitialized() && !InitializeInterop(device, context))
				return false;
			if (Runtime::Instance().Status() != RuntimeStatus::Initialized && !InitializeRuntime())
				return false;
			const std::uint32_t modelResolution = NormalizeModelResolution(tuning.modelResolutionPercent);
			const std::uint32_t modelWidth = ScaleDimension(colorWidth, modelResolution);
			const std::uint32_t modelHeight = ScaleDimension(colorHeight, modelResolution);
			const std::uint32_t passCount = GetPassCount(tuning);
			const auto resolveSettings = GetModelResolveSettings(modelResolution);
			if (!EnsureResources(device, eyeIndex, color, depth, motionVectors, guideWidth, guideHeight,
					colorWidth, colorHeight, modelWidth, modelHeight, passCount, modelResolution))
				return LatchFailure("shared resource creation", interop.LastError());

			auto& eye = eyes[eyeIndex];
			context->CopyResource(eye.color.resource11.Get(), color);
			if (eye.reducedResolution && !DispatchModelInput(device, context, eye, colorWidth, colorHeight, modelWidth, modelHeight,
											 tuning.modelResolveMode == 1))
				return LatchFailure("model input downsample", E_FAIL);
			if (!CopyDepthGuide(context, depthSRV, eye.depth.uav11.Get(), guideWidth, guideHeight))
				return LatchFailure("depth guide conversion", E_FAIL);
			context->CopyResource(eye.motionVectors.resource11.Get(), motionVectors);

			ID3D12GraphicsCommandList* commandList = nullptr;
			if (!interop.BeginD3D12(&commandList)) {
				return LatchFailure("BeginD3D12", interop.LastError());
			}
			const bool succeeded = ExecuteCascade(commandList, eyeIndex,
				eye.reducedResolution ? eye.modelInput.resource12.Get() : eye.color.resource12.Get(),
				eye.depth.resource12.Get(), eye.motionVectors.resource12.Get(),
				std::array<ID3D12Resource*, kCascadePassCount - 1>{
					eye.cascadeIntermediates[0].resource12.Get(), eye.cascadeIntermediates[1].resource12.Get() },
				eye.output.resource12.Get(),
				guideWidth, guideHeight, guideWidth, guideHeight, modelWidth, modelHeight,
				motionVectorScaleX * static_cast<float>(modelWidth) / colorWidth,
				motionVectorScaleY * static_cast<float>(modelHeight) / colorHeight,
				tuning, passCount, resetPending[eyeIndex]);
			if (!interop.EndD3D12()) {
				return LatchFailure("EndD3D12", interop.LastError());
			}
			if (!succeeded) {
				return LatchFailure("Feature 18", static_cast<HRESULT>(Runtime::Instance().NgxResult()));
			}

			if (eye.reducedResolution && !DispatchModelResolve(device, context, eye, colorWidth, colorHeight, resolveSettings,
											 tuning.modelResolveMode == 1)) {
				return LatchFailure("model output resolve", E_FAIL);
			}
			context->CopyResource(color, eye.reducedResolution ? eye.resolved.Get() : eye.output.resource11.Get());
			resetPending[eyeIndex] = false;
			return true;
		}

		bool ApplyStereo(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* color,
			const std::array<StereoEyeInput, 2>& inputs,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight, const Tuning& tuning,
			ID3D11Resource* destination, ID3D11UnorderedAccessView* destinationUAV,
			bool blendSubrect)
		{
			if (failureLatched || !device || !context || !color)
				return false;
			ID3D11Resource* writeback = destination ? destination : color;
			if (!writeback)
				return false;
			CS_GPU_PASS("NeuralRendering::EvaluateStereo");

			if (!interop.IsInitialized() && !InitializeInterop(device, context))
				return false;
			if (Runtime::Instance().Status() != RuntimeStatus::Initialized && !InitializeRuntime())
				return false;

			D3D11_TEXTURE2D_DESC colorDesc{};
			if (!GetTextureDesc(color, colorDesc))
				return false;
			const std::uint32_t modelResolution = NormalizeModelResolution(tuning.modelResolutionPercent);
			const std::uint32_t modelWidth = ScaleDimension(colorWidth, modelResolution);
			const std::uint32_t modelHeight = ScaleDimension(colorHeight, modelResolution);
			const std::uint32_t passCount = GetPassCount(tuning);
			const auto resolveSettings = GetModelResolveSettings(modelResolution);
			for (std::uint32_t eyeIndex = 0; eyeIndex < inputs.size(); ++eyeIndex) {
				const auto& input = inputs[eyeIndex];
				if (!input.depth || !input.depthSRV || !input.motionVectors)
					return false;
				if (!EnsureResources(device, eyeIndex, color, input.depth, input.motionVectors,
						guideWidth, guideHeight, colorWidth, colorHeight, modelWidth, modelHeight, passCount, modelResolution))
					return LatchFailure("shared resource creation", interop.LastError());

				D3D11_BOX sourceBox{
					input.sourceX, input.sourceY, 0,
					input.sourceX + colorWidth, input.sourceY + colorHeight, 1
				};
				auto& eye = eyes[eyeIndex];
				context->CopySubresourceRegion(eye.color.resource11.Get(), 0, 0, 0, 0, color, 0, &sourceBox);
				if (eye.reducedResolution && !DispatchModelInput(device, context, eye, colorWidth, colorHeight, modelWidth, modelHeight,
												 tuning.modelResolveMode == 1))
					return LatchFailure("model input downsample stereo", E_FAIL);
				if (!CopyDepthGuide(context, input.depthSRV, eye.depth.uav11.Get(), guideWidth, guideHeight))
					return LatchFailure("depth guide conversion", E_FAIL);
				context->CopyResource(eye.motionVectors.resource11.Get(), input.motionVectors);
			}

			ID3D12GraphicsCommandList* commandList = nullptr;
			if (!interop.BeginD3D12(&commandList)) {
				return LatchFailure("BeginD3D12 stereo", interop.LastError());
			}

			bool succeeded = true;
			for (std::uint32_t eyeIndex = 0; eyeIndex < inputs.size(); ++eyeIndex) {
				auto& eye = eyes[eyeIndex];
				const auto& input = inputs[eyeIndex];
				const bool eyeSucceeded = ExecuteCascade(commandList, eyeIndex,
					eye.reducedResolution ? eye.modelInput.resource12.Get() : eye.color.resource12.Get(),
					eye.depth.resource12.Get(), eye.motionVectors.resource12.Get(),
					std::array<ID3D12Resource*, kCascadePassCount - 1>{
						eye.cascadeIntermediates[0].resource12.Get(), eye.cascadeIntermediates[1].resource12.Get() },
					eye.output.resource12.Get(),
					guideWidth, guideHeight, guideWidth, guideHeight, modelWidth, modelHeight,
					input.motionVectorScaleX * static_cast<float>(modelWidth) / colorWidth,
					input.motionVectorScaleY * static_cast<float>(modelHeight) / colorHeight,
					tuning, passCount, resetPending[eyeIndex]);
				if (!eyeSucceeded) {
					succeeded = false;
					break;
				}
			}

			if (!interop.EndD3D12()) {
				return LatchFailure("EndD3D12 stereo", interop.LastError());
			}
			if (!succeeded) {
				return LatchFailure("Feature 18 stereo", static_cast<HRESULT>(Runtime::Instance().NgxResult()));
			}

			D3D11_BOX outputBox{ 0, 0, 0, colorWidth, colorHeight, 1 };
			for (std::uint32_t eyeIndex = 0; eyeIndex < inputs.size(); ++eyeIndex) {
				const auto& input = inputs[eyeIndex];
				auto& eye = eyes[eyeIndex];
				if (eye.reducedResolution && !DispatchModelResolve(device, context, eye, colorWidth, colorHeight, resolveSettings,
												 tuning.modelResolveMode == 1)) {
					return LatchFailure("model output resolve stereo", E_FAIL);
				}
				ID3D11Resource* neuralOutput = eye.reducedResolution ? eye.resolved.Get() : eye.output.resource11.Get();
				if (blendSubrect && destinationUAV) {
					// Keep the original background in `writeback` and composite the NR
					// crop over it with the same edge treatment as standard foveated DLSS.
					FoveatedRenderImpl::Ops::BlendSubrectToOutput(neuralOutput, writeback, destinationUAV,
						input.sourceX, input.sourceY, colorWidth, colorHeight);
				} else {
					context->CopySubresourceRegion(writeback, 0, input.sourceX, input.sourceY, 0,
						neuralOutput, 0, &outputBox);
				}
				resetPending[eyeIndex] = false;
			}
			return true;
		}

		void Reset()
		{
			interop.WaitForIdle();
			Runtime::Instance().Shutdown();
			interop.Shutdown();
			eyes = {};
			resetPending = { true, true };
			failureLatched = false;
			copyDepthGuideCS.Reset();
			modelResolutionCS.Reset();
			modelResolutionCB.Reset();
			modelResolutionSampler.Reset();
		}

		void ResetHistory()
		{
			interop.WaitForIdle();
			Runtime::Instance().ResetFeatures();
			resetPending = { true, true };
		}

		void ClearShaderCache()
		{
			copyDepthGuideCS.Reset();
			modelResolutionCS.Reset();
		}

		[[nodiscard]] bool IsFailureLatched() const { return failureLatched; }

	private:
		bool ExecuteCascade(ID3D12GraphicsCommandList* commandList, std::uint32_t eyeIndex,
			ID3D12Resource* initialInput, ID3D12Resource* depth, ID3D12Resource* motionVectors,
			const std::array<ID3D12Resource*, kCascadePassCount - 1>& intermediates,
			ID3D12Resource* finalOutput,
			std::uint32_t firstInputWidth, std::uint32_t firstInputHeight,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight,
			float motionVectorScaleX, float motionVectorScaleY,
			const Tuning& tuning, std::uint32_t passCount, bool reset)
		{
			if (!commandList || !initialInput || !depth || !motionVectors || !finalOutput ||
				passCount == 0 || passCount > kCascadePassCount)
				return false;
			for (std::uint32_t intermediateIndex = 0; intermediateIndex + 1 < passCount; ++intermediateIndex) {
				if (!intermediates[intermediateIndex])
					return false;
			}

			for (std::uint32_t passIndex = 0; passIndex < passCount; ++passIndex) {
				ID3D12Resource* input = passIndex == 0 ? initialInput : intermediates[passIndex - 1];
				ID3D12Resource* output = (passIndex + 1 == passCount) ? finalOutput : intermediates[passIndex];
				if (!input || !output || input == output)
					return false;

				CS_GPU_PASS_SELECT3(passIndex,
					"NeuralRendering::EvaluatePass0", "NeuralRendering::EvaluatePass1", "NeuralRendering::EvaluatePass2");
				TransitionEvaluationResources(commandList, input, depth, motionVectors, output, true);
				const bool succeeded = Runtime::Instance().Execute(commandList, FeatureSlot(eyeIndex, passIndex),
					input, depth, motionVectors, output,
					passIndex == 0 ? firstInputWidth : outputWidth,
					passIndex == 0 ? firstInputHeight : outputHeight,
					outputWidth, outputHeight, guideWidth, guideHeight,
					motionVectorScaleX, motionVectorScaleY, tuning, reset);
				TransitionEvaluationResources(commandList, input, depth, motionVectors, output, false);
				if (!succeeded) {
					logger::warn("[DLSSNR] cascade pass failed eye={} pass={} of {}", eyeIndex, passIndex + 1, passCount);
					return false;
				}
			}
			return true;
		}

		struct ModelResolutionConstants
		{
			std::uint32_t mode = 0;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			std::uint32_t sourceWidth = 0;
			std::uint32_t sourceHeight = 0;
			float transferStrength = 1.0f;
			float colourStrength = 1.0f;
			float maxRatio = 4.0f;
			float residualStrength = 1.0f;
			float padding0 = 0.0f;
			float padding1 = 0.0f;
			float padding2 = 0.0f;
		};
		static_assert(sizeof(ModelResolutionConstants) % 16 == 0);

		bool EnsureModelResolutionResources(ID3D11Device* device)
		{
			if (!device)
				return false;
			if (!modelResolutionCS.Get(
					L"Data\\Shaders\\Upscaling\\NeuralRendering\\ModelResolutionCS.hlsl", {}, "cs_5_0",
					"main", "NeuralRendering::ModelResolutionCS"))
				return false;

			if (!modelResolutionCB) {
				D3D11_BUFFER_DESC bufferDesc{};
				bufferDesc.ByteWidth = sizeof(ModelResolutionConstants);
				bufferDesc.Usage = D3D11_USAGE_DEFAULT;
				bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, modelResolutionCB.GetAddressOf())))
					return false;
				Util::SetResourceName(modelResolutionCB.Get(), "NeuralRendering::ModelResolutionCB");
			}

			if (!modelResolutionSampler) {
				D3D11_SAMPLER_DESC samplerDesc{};
				samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
				samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
				samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
				samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
				samplerDesc.MinLOD = 0.0f;
				samplerDesc.MaxLOD = std::numeric_limits<float>::max();
				if (FAILED(device->CreateSamplerState(&samplerDesc, modelResolutionSampler.GetAddressOf())))
					return false;
				Util::SetResourceName(modelResolutionSampler.Get(), "NeuralRendering::ModelResolutionSampler");
			}
			return true;
		}

		bool DispatchModelInput(ID3D11Device* device, ID3D11DeviceContext* context, const EyeResources& eye,
			std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t modelWidth, std::uint32_t modelHeight, bool exactArea)
		{
			if (!EnsureModelResolutionResources(device) || !context || !eye.color.srv11 || !eye.modelInput.uav11)
				return false;
			CS_GPU_PASS("NeuralRendering::ModelDownsample");
			ModelResolutionConstants constants{
				.mode = exactArea ? 2u : 0u,
				.width = modelWidth,
				.height = modelHeight,
				.sourceWidth = sourceWidth,
				.sourceHeight = sourceHeight,
			};
			context->UpdateSubresource(modelResolutionCB.Get(), 0, nullptr, &constants, 0, 0);
			ID3D11ShaderResourceView* source = eye.color.srv11.Get();
			ID3D11UnorderedAccessView* target = eye.modelInput.uav11.Get();
			ID3D11Buffer* constantBuffer = modelResolutionCB.Get();
			ID3D11SamplerState* sampler = modelResolutionSampler.Get();
			context->CSSetShader(modelResolutionCS.get(), nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &constantBuffer);
			context->CSSetShaderResources(0, 1, &source);
			context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
			context->CSSetSamplers(0, 1, &sampler);
			context->Dispatch((modelWidth + 7) / 8, (modelHeight + 7) / 8, 1);
			ClearModelResolutionBindings(context, 1);
			return true;
		}

		bool DispatchModelResolve(ID3D11Device* device, ID3D11DeviceContext* context, const EyeResources& eye,
			std::uint32_t width, std::uint32_t height, const ModelResolveSettings& resolveSettings,
			bool matchedResidual)
		{
			if (!EnsureModelResolutionResources(device) || !context || !eye.color.srv11 || !eye.modelInput.srv11 ||
				!eye.output.srv11 || !eye.resolvedUAV)
				return false;
			CS_GPU_PASS("NeuralRendering::ModelResolve");
			ModelResolutionConstants constants{
				.mode = matchedResidual ? 3u : 1u,
				.width = width,
				.height = height,
				.sourceWidth = width,
				.sourceHeight = height,
				.transferStrength = resolveSettings.transferStrength,
				.colourStrength = resolveSettings.colourStrength,
				.maxRatio = resolveSettings.maxRatio,
				.residualStrength = resolveSettings.residualStrength,
			};
			context->UpdateSubresource(modelResolutionCB.Get(), 0, nullptr, &constants, 0, 0);
			ID3D11ShaderResourceView* sources[3]{
				eye.modelInput.srv11.Get(), eye.output.srv11.Get(), eye.color.srv11.Get()
			};
			ID3D11UnorderedAccessView* target = eye.resolvedUAV.Get();
			ID3D11Buffer* constantBuffer = modelResolutionCB.Get();
			ID3D11SamplerState* sampler = modelResolutionSampler.Get();
			context->CSSetShader(modelResolutionCS.get(), nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &constantBuffer);
			context->CSSetShaderResources(0, 3, sources);
			context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
			context->CSSetSamplers(0, 1, &sampler);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			ClearModelResolutionBindings(context, 3);
			return true;
		}

		void ClearModelResolutionBindings(ID3D11DeviceContext* context, UINT sourceCount)
		{
			std::array<ID3D11ShaderResourceView*, 3> nullSources{};
			ID3D11UnorderedAccessView* nullTarget = nullptr;
			ID3D11Buffer* nullBuffer = nullptr;
			ID3D11SamplerState* nullSampler = nullptr;
			context->CSSetShaderResources(0, sourceCount, nullSources.data());
			context->CSSetUnorderedAccessViews(0, 1, &nullTarget, nullptr);
			context->CSSetConstantBuffers(0, 1, &nullBuffer);
			context->CSSetSamplers(0, 1, &nullSampler);
			context->CSSetShader(nullptr, nullptr, 0);
		}

		bool CopyDepthGuide(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
			ID3D11UnorderedAccessView* destination, std::uint32_t width, std::uint32_t height)
		{
			auto* shader = copyDepthGuideCS.Get(
				L"Data\\Shaders\\Upscaling\\NeuralRendering\\CopyDepthGuideCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::CopyDepthGuideCS");
			if (!shader || !destination)
				return false;
			context->CSSetShader(shader, nullptr, 0);
			context->CSSetShaderResources(0, 1, &source);
			context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			ID3D11ShaderResourceView* nullSRV = nullptr;
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			return true;
		}

		bool InitializeInterop(ID3D11Device* device, ID3D11DeviceContext* context)
		{
			Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
			Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
			HRESULT result = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
			if (SUCCEEDED(result))
				result = dxgiDevice->GetAdapter(&adapter);
			if (FAILED(result) || !interop.Initialize(adapter.Get(), device, context))
				return LatchFailure("D3D12 interop initialization", FAILED(result) ? result : interop.LastError());
			return true;
		}

		bool InitializeRuntime()
		{
			auto& runtime = Runtime::Instance();
			if (!runtime.Probe() || !runtime.Initialize(interop.Device()))
				return LatchFailure("runtime initialization", static_cast<HRESULT>(runtime.NgxResult()));
			logger::info("[DLSSNR] initialized version={} appId=0x{:08X} api=0x{:X}",
				runtime.Version(), runtime.ApplicationId(), runtime.ApiVersion());
			return true;
		}

		bool EnsureResources(ID3D11Device* device, std::uint32_t eyeIndex, ID3D11Resource* color, ID3D11Resource* depth,
			ID3D11Resource* motionVectors, std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			std::uint32_t modelWidth, std::uint32_t modelHeight, std::uint32_t passCount,
			std::uint32_t modelResolution)
		{
			if (!device || eyeIndex >= eyes.size() || colorWidth == 0 || colorHeight == 0 ||
				modelWidth == 0 || modelHeight == 0 || passCount == 0 || passCount > kCascadePassCount)
				return false;
			D3D11_TEXTURE2D_DESC colorSource{}, depthSource{}, motionSource{};
			if (!GetTextureDesc(color, colorSource) || !GetTextureDesc(depth, depthSource) ||
				!GetTextureDesc(motionVectors, motionSource))
				return false;
			const bool reducedResolution = modelWidth != colorWidth || modelHeight != colorHeight;
			const UINT sharedFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			auto colorDesc = MakeSharedDesc(colorSource, colorWidth, colorHeight, sharedFlags);
			auto modelInputDesc = MakeSharedDesc(colorSource, modelWidth, modelHeight, sharedFlags);
			auto outputDesc = MakeSharedDesc(colorSource, modelWidth, modelHeight, sharedFlags);
			auto depthDesc = MakeSharedDesc(depthSource, guideWidth, guideHeight, sharedFlags);
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			auto motionDesc = MakeSharedDesc(motionSource, guideWidth, guideHeight, sharedFlags);
			auto resolvedDesc = MakeSharedDesc(colorSource, colorWidth, colorHeight, sharedFlags);
			auto& eye = eyes[eyeIndex];
			const bool multiPass = passCount > 1;
			const bool intermediatesMatch = passCount <= 1 ||
			                                (Matches(eye.cascadeIntermediates[0], outputDesc) &&
												(passCount < 3 || Matches(eye.cascadeIntermediates[1], outputDesc)));
			if (eye.passCount == passCount && eye.reducedResolution == reducedResolution && Matches(eye.color, colorDesc) &&
				(!reducedResolution || Matches(eye.modelInput, modelInputDesc)) && Matches(eye.output, outputDesc) &&
				Matches(eye.depth, depthDesc) && Matches(eye.motionVectors, motionDesc) &&
				intermediatesMatch &&
				(!reducedResolution || (MatchesResolved(eye.resolved, resolvedDesc) && eye.resolvedSRV && eye.resolvedUAV)))
				return true;

			if (!interop.WaitForIdle())
				return false;
			for (std::uint32_t passIndex = 0; passIndex < kCascadePassCount; ++passIndex)
				Runtime::Instance().ResetFeature(FeatureSlot(eyeIndex, passIndex));
			eye = {};
			const std::string suffix = eyeIndex == 0 ? "Left" : "Right";
			if (!interop.CreateSharedTexture(colorDesc, eye.color, ("NeuralRendering::Color" + suffix).c_str()) ||
				(reducedResolution && !interop.CreateSharedTexture(modelInputDesc, eye.modelInput, ("NeuralRendering::ModelInput" + suffix).c_str())) ||
				(multiPass && !interop.CreateSharedTexture(outputDesc, eye.cascadeIntermediates[0], ("NeuralRendering::CascadeIntermediate0" + suffix).c_str())) ||
				(passCount > 2 && !interop.CreateSharedTexture(outputDesc, eye.cascadeIntermediates[1], ("NeuralRendering::CascadeIntermediate1" + suffix).c_str())) ||
				!interop.CreateSharedTexture(outputDesc, eye.output, ("NeuralRendering::Output" + suffix).c_str()) ||
				!interop.CreateSharedTexture(depthDesc, eye.depth, ("NeuralRendering::Depth" + suffix).c_str()) ||
				!interop.CreateSharedTexture(motionDesc, eye.motionVectors, ("NeuralRendering::Motion" + suffix).c_str()))
				return false;
			if (reducedResolution) {
				if (FAILED(device->CreateTexture2D(&resolvedDesc, nullptr, eye.resolved.GetAddressOf())) ||
					FAILED(device->CreateShaderResourceView(eye.resolved.Get(), nullptr, eye.resolvedSRV.GetAddressOf())) ||
					FAILED(device->CreateUnorderedAccessView(eye.resolved.Get(), nullptr, eye.resolvedUAV.GetAddressOf())))
					return false;
				Util::SetResourceName(eye.resolved.Get(), ("NeuralRendering::Resolved" + suffix).c_str());
				Util::SetResourceName(eye.resolvedSRV.Get(), ("NeuralRendering::Resolved" + suffix + " SRV").c_str());
				Util::SetResourceName(eye.resolvedUAV.Get(), ("NeuralRendering::Resolved" + suffix + " UAV").c_str());
			}
			eye.reducedResolution = reducedResolution;
			eye.passCount = passCount;
			resetPending = { true, true };
			const float modelAreaPercent = 100.0f *
			                               (static_cast<float>(modelWidth) / static_cast<float>(colorWidth)) *
			                               (static_cast<float>(modelHeight) / static_cast<float>(colorHeight));
			const auto resolveSettings = GetModelResolveSettings(modelResolution);
			logger::info("[DLSSNR] resources eye={} guides={}x{} color={}x{} model={}x{} modelPercent={}% modelArea={:.1f}% passes={} resolve=transfer:{:.2f},colour:{:.2f},maxRatio:{:.2f},residual:{:.2f}",
				eyeIndex, guideWidth, guideHeight, colorWidth, colorHeight, modelWidth, modelHeight,
				modelResolution, modelAreaPercent, passCount, resolveSettings.transferStrength,
				resolveSettings.colourStrength, resolveSettings.maxRatio, resolveSettings.residualStrength);
			return true;
		}

		bool LatchFailure(const char* operation, HRESULT error)
		{
			failureLatched = true;
			logger::error("[DLSSNR] {} failed hr/ngx=0x{:08X} status={} detail={}",
				operation, static_cast<std::uint32_t>(error), ToString(Runtime::Instance().Status()), Runtime::Instance().Detail());
			return false;
		}

		D3D12Interop interop;
		Util::LazyShader<ID3D11ComputeShader> copyDepthGuideCS;
		Util::LazyShader<ID3D11ComputeShader> modelResolutionCS;
		Microsoft::WRL::ComPtr<ID3D11Buffer> modelResolutionCB;
		Microsoft::WRL::ComPtr<ID3D11SamplerState> modelResolutionSampler;
		std::array<EyeResources, 2> eyes;
		std::array<bool, 2> resetPending{ true, true };
		bool failureLatched = false;
	};

	Renderer::Renderer() : state_(new State()) {}
	Renderer::~Renderer() { delete state_; }
	Renderer& Renderer::Instance()
	{
		static Renderer instance;
		return instance;
	}

	bool Renderer::Apply(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
		ID3D11Resource* color, ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
		ID3D11Resource* motionVectors,
		std::uint32_t guideWidth, std::uint32_t guideHeight, std::uint32_t colorWidth, std::uint32_t colorHeight,
		float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning)
	{
		return state_->Apply(device, context, eyeIndex, color, depth, depthSRV, motionVectors,
			guideWidth, guideHeight, colorWidth, colorHeight, motionVectorScaleX, motionVectorScaleY, tuning);
	}

	bool Renderer::ApplyStereo(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* color,
		const std::array<StereoEyeInput, 2>& eyes,
		std::uint32_t guideWidth, std::uint32_t guideHeight,
		std::uint32_t colorWidth, std::uint32_t colorHeight, const Tuning& tuning,
		ID3D11Resource* destination, ID3D11UnorderedAccessView* destinationUAV, bool blendSubrect)
	{
		return state_->ApplyStereo(device, context, color, eyes,
			guideWidth, guideHeight, colorWidth, colorHeight, tuning,
			destination, destinationUAV, blendSubrect);
	}

	void Renderer::Reset() { state_->Reset(); }
	void Renderer::ClearShaderCache() { state_->ClearShaderCache(); }
	void Renderer::ResetHistory() { state_->ResetHistory(); }
	bool Renderer::IsFailureLatched() const { return state_->IsFailureLatched(); }
	std::uint32_t Renderer::NgxResult() const { return Runtime::Instance().NgxResult(); }
	std::uint64_t Renderer::SuccessfulFrames() const { return Runtime::Instance().SuccessfulFrames(); }
	const char* Renderer::StatusText() const { return ToString(Runtime::Instance().Status()); }
}
