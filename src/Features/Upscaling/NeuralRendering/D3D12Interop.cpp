#include "D3D12Interop.h"

#include "Utils/D3D.h"

#include <utility>

#include <Windows.h>

namespace NeuralRendering
{
	D3D12Interop::~D3D12Interop() { Shutdown(); }

	bool D3D12Interop::RecordFailure(HRESULT result)
	{
		lastError_ = result;
		return false;
	}

	bool D3D12Interop::Initialize(IDXGIAdapter* adapter, ID3D11Device* device, ID3D11DeviceContext* context)
	{
		Shutdown();
		if (!adapter || !device || !context)
			return RecordFailure(E_INVALIDARG);

		HRESULT result = device->QueryInterface(IID_PPV_ARGS(&device11_));
		if (FAILED(result))
			return RecordFailure(result);
		result = context->QueryInterface(IID_PPV_ARGS(&context11_));
		if (FAILED(result))
			return RecordFailure(result);
		result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12_));
		if (FAILED(result))
			return RecordFailure(result);

		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		result = device12_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue12_));
		if (FAILED(result))
			return RecordFailure(result);
		for (auto& commandContext : commandContexts_) {
			result = device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(&commandContext.allocator));
			if (FAILED(result))
				return RecordFailure(result);
			result = device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				commandContext.allocator.Get(), nullptr, IID_PPV_ARGS(&commandContext.commandList));
			if (FAILED(result))
				return RecordFailure(result);
			result = commandContext.commandList->Close();
			if (FAILED(result))
				return RecordFailure(result);
		}

		result = device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12_));
		if (FAILED(result))
			return RecordFailure(result);
		HANDLE sharedFence = nullptr;
		result = device12_->CreateSharedHandle(fence12_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence);
		if (FAILED(result))
			return RecordFailure(result);
		result = device11_->OpenSharedFence(sharedFence, IID_PPV_ARGS(&fence11_));
		CloseHandle(sharedFence);
		if (FAILED(result))
			return RecordFailure(result);

		fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!fenceEvent_)
			return RecordFailure(HRESULT_FROM_WIN32(GetLastError()));
		initialized_ = true;
		lastError_ = S_OK;
		logger::info("[DLSSNR] D3D12 interop initialized commandContexts={} cpuFenceWait=backpressure-only",
			kCommandContextCount);
		return true;
	}

	void D3D12Interop::Shutdown()
	{
		if (fenceEvent_)
			CloseHandle(fenceEvent_);
		fenceEvent_ = nullptr;
		recording_ = false;
		recordingContext_ = kCommandContextCount;
		commandContextCursor_ = 0;
		backpressureLogged_ = false;
		initialized_ = false;
		fenceValue_ = 0;
		fence11_.Reset();
		fence12_.Reset();
		commandContexts_ = {};
		queue12_.Reset();
		device12_.Reset();
		context11_.Reset();
		device11_.Reset();
	}

	bool D3D12Interop::CreateSharedTexture(const D3D11_TEXTURE2D_DESC& sourceDesc, SharedTexture& texture, const char* name)
	{
		if (!initialized_ || sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
			sourceDesc.ArraySize == 0 || sourceDesc.ArraySize > UINT16_MAX || sourceDesc.MipLevels > UINT16_MAX)
			return RecordFailure(E_INVALIDARG);

		D3D11_TEXTURE2D_DESC desc = sourceDesc;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0) {
			lastOperation_ = "CreateSharedTextureRequiresUAV";
			return RecordFailure(E_INVALIDARG);
		}

		SharedTexture replacement;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		HRESULT result = device11_->CreateTexture2D(&desc, nullptr, &replacement.resource11);
		if (FAILED(result)) {
			lastOperation_ = "D3D11CreateTexture2D";
			return RecordFailure(result);
		}
		Util::SetResourceName(replacement.resource11.Get(), name);
		result = device11_->CreateShaderResourceView(replacement.resource11.Get(), nullptr, &replacement.srv11);
		if (FAILED(result)) {
			lastOperation_ = "D3D11CreateShaderResourceView";
			return RecordFailure(result);
		}
		Util::SetResourceName(replacement.srv11.Get(), "%s SRV", name);
		result = device11_->CreateUnorderedAccessView(replacement.resource11.Get(), nullptr, &replacement.uav11);
		if (FAILED(result)) {
			lastOperation_ = "D3D11CreateUnorderedAccessView";
			return RecordFailure(result);
		}
		Util::SetResourceName(replacement.uav11.Get(), "%s UAV", name);

		Microsoft::WRL::ComPtr<IDXGIResource1> dxgiResource;
		result = replacement.resource11.As(&dxgiResource);
		if (FAILED(result))
			return RecordFailure(result);
		HANDLE sharedTexture = nullptr;
		result = dxgiResource->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedTexture);
		if (FAILED(result))
			return RecordFailure(result);
		result = device12_->OpenSharedHandle(sharedTexture, IID_PPV_ARGS(&replacement.resource12));
		CloseHandle(sharedTexture);
		if (FAILED(result))
			return RecordFailure(result);

		lastResourceFlags_ = static_cast<std::uint32_t>(replacement.resource12->GetDesc().Flags);
		replacement.desc = desc;
		texture = std::move(replacement);
		lastOperation_ = "CreateSharedTexture(D3D11First)";
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::BeginD3D12(ID3D12GraphicsCommandList** commandList)
	{
		if (!initialized_ || recording_ || !commandList)
			return RecordFailure(E_UNEXPECTED);

		const std::uint64_t completedValue = fence12_->GetCompletedValue();
		std::size_t contextIndex = kCommandContextCount;
		for (std::size_t offset = 0; offset < kCommandContextCount; ++offset) {
			const std::size_t candidate = (commandContextCursor_ + offset) % kCommandContextCount;
			const auto pendingValue = commandContexts_[candidate].fenceValue;
			if (pendingValue == 0 || completedValue >= pendingValue) {
				contextIndex = candidate;
				break;
			}
		}
		if (contextIndex == kCommandContextCount) {
			contextIndex = commandContextCursor_;
			if (!backpressureLogged_) {
				logger::warn("[DLSSNR] D3D12 command contexts saturated; applying CPU backpressure");
				backpressureLogged_ = true;
			}
			if (!WaitForFence(commandContexts_[contextIndex].fenceValue))
				return false;
		}

		auto& commandContext = commandContexts_[contextIndex];
		commandContext.fenceValue = 0;
		const std::uint64_t readyValue = ++fenceValue_;
		HRESULT result = context11_->Signal(fence11_.Get(), readyValue);
		if (FAILED(result))
			return RecordFailure(result);
		result = queue12_->Wait(fence12_.Get(), readyValue);
		if (FAILED(result))
			return RecordFailure(result);
		result = commandContext.allocator->Reset();
		if (FAILED(result))
			return RecordFailure(result);
		result = commandContext.commandList->Reset(commandContext.allocator.Get(), nullptr);
		if (FAILED(result))
			return RecordFailure(result);
		recording_ = true;
		recordingContext_ = contextIndex;
		commandContextCursor_ = (contextIndex + 1) % kCommandContextCount;
		*commandList = commandContext.commandList.Get();
		return true;
	}

	bool D3D12Interop::EndD3D12()
	{
		if (!initialized_ || !recording_ || recordingContext_ >= kCommandContextCount)
			return RecordFailure(E_UNEXPECTED);
		recording_ = false;
		auto& commandContext = commandContexts_[recordingContext_];
		recordingContext_ = kCommandContextCount;
		HRESULT result = commandContext.commandList->Close();
		if (FAILED(result))
			return RecordFailure(result);
		ID3D12CommandList* lists[] = { commandContext.commandList.Get() };
		queue12_->ExecuteCommandLists(1, lists);
		const std::uint64_t completeValue = ++fenceValue_;
		result = queue12_->Signal(fence12_.Get(), completeValue);
		if (FAILED(result))
			return RecordFailure(result);
		commandContext.fenceValue = completeValue;

		// This queues a GPU-side dependency. Subsequent D3D11 output copies wait
		// for Feature 18 without stalling the render thread on the CPU.
		result = context11_->Wait(fence11_.Get(), completeValue);
		if (FAILED(result))
			return RecordFailure(result);
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::WaitForFence(std::uint64_t value)
	{
		if (!value || fence12_->GetCompletedValue() >= value)
			return true;
		const HRESULT result = fence12_->SetEventOnCompletion(value, fenceEvent_);
		if (FAILED(result))
			return RecordFailure(result);
		const DWORD waitResult = WaitForSingleObject(fenceEvent_, 250);
		if (waitResult != WAIT_OBJECT_0)
			return RecordFailure(waitResult == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) :
															  HRESULT_FROM_WIN32(GetLastError()));
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::WaitForIdle()
	{
		if (!initialized_)
			return true;
		std::uint64_t lastSubmittedValue = 0;
		for (const auto& commandContext : commandContexts_)
			lastSubmittedValue = std::max(lastSubmittedValue, commandContext.fenceValue);
		return WaitForFence(lastSubmittedValue);
	}
}
