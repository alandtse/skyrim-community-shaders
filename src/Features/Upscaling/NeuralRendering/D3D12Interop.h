#pragma once

#include <array>
#include <cstdint>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace NeuralRendering
{
	struct SharedTexture
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> resource11;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv11;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav11;
		Microsoft::WRL::ComPtr<ID3D12Resource> resource12;
		D3D11_TEXTURE2D_DESC desc{};
	};

	class D3D12Interop
	{
	public:
		~D3D12Interop();

		D3D12Interop(const D3D12Interop&) = delete;
		D3D12Interop& operator=(const D3D12Interop&) = delete;

		bool Initialize(IDXGIAdapter* adapter, ID3D11Device* device, ID3D11DeviceContext* context);
		void Shutdown();
		bool CreateSharedTexture(const D3D11_TEXTURE2D_DESC& desc, SharedTexture& texture, const char* name);
		bool BeginD3D12(ID3D12GraphicsCommandList** commandList);
		bool EndD3D12();
		bool WaitForIdle();

		[[nodiscard]] bool IsInitialized() const { return initialized_; }
		[[nodiscard]] HRESULT LastError() const { return lastError_; }
		[[nodiscard]] const char* LastOperation() const { return lastOperation_; }
		[[nodiscard]] std::uint32_t LastResourceFlags() const { return lastResourceFlags_; }
		[[nodiscard]] ID3D12Device* Device() const { return device12_.Get(); }

	private:
		static constexpr std::size_t kCommandContextCount = 3;

		struct CommandContext
		{
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
			std::uint64_t fenceValue = 0;
		};

		D3D12Interop() = default;
		friend class Renderer;
		bool RecordFailure(HRESULT result);
		bool WaitForFence(std::uint64_t value);

		Microsoft::WRL::ComPtr<ID3D11Device5> device11_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;
		Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
		Microsoft::WRL::ComPtr<ID3D12Device> device12_;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue12_;
		std::array<CommandContext, kCommandContextCount> commandContexts_;
		Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
		HANDLE fenceEvent_ = nullptr;
		std::uint64_t fenceValue_ = 0;
		HRESULT lastError_ = S_OK;
		const char* lastOperation_ = "none";
		std::uint32_t lastResourceFlags_ = 0;
		std::size_t commandContextCursor_ = 0;
		std::size_t recordingContext_ = kCommandContextCount;
		bool backpressureLogged_ = false;
		bool initialized_ = false;
		bool recording_ = false;
	};
}
