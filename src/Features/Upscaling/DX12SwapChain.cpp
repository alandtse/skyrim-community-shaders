#include "DX12SwapChain.h"

#include <FidelityFX/api/include/dx12/ffx_api_dx12.hpp>
#include <dxgi1_6.h>

#include "../HDRDisplay.h"
#include "../Upscaling.h"
#include "FidelityFX.h"
#include "Streamline.h"
#include "Utils/D3D.h"

void DX12SwapChain::CreateD3D12Device(IDXGIAdapter* a_adapter)
{
	// Idempotent: the DLSS-G probe binds Streamline to this device; a second call from
	// the FSR path must not replace it out from under that binding.
	if (d3d12Device)
		return;

	DX::ThrowIfFailed(D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Device)));

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queueDesc.NodeMask = 0;

	DX::ThrowIfFailed(d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)));

	for (int i = 0; i < 3; i++) {
		DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[i])));
		DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[i].get(), nullptr, IID_PPV_ARGS(&commandLists[i])));
		commandLists[i]->Close();
	}
}

void DX12SwapChain::CreateSwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	CreateD3D12Device(adapter);

	IDXGIFactory4* dxgiFactory;
	DX::ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)));

	// Runtime format negotiation for swap chain
	DXGI_FORMAT attemptedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	DXGI_FORMAT negotiatedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	bool isVR = globals::game::isVR;
	bool fallbackUsed = false;

	// Test R10G10B10A2 support (applies to both VR and non-VR for HDR capability)
	D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_FORMAT_SUPPORT1_RENDER_TARGET, D3D12_FORMAT_SUPPORT2_NONE };
	if (SUCCEEDED(d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)))) {
		if ((formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0) {
			logger::warn("[DX12SwapChain] R10G10B10A2_UNORM not supported as render target, falling back to R8G8B8A8_UNORM");
			negotiatedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			fallbackUsed = true;
		} else if (isVR) {
			logger::info("[DX12SwapChain] VR detected with R10G10B10A2_UNORM support, attempting HDR");
		}
	} else {
		logger::warn("[DX12SwapChain] CheckFeatureSupport failed for R10G10B10A2_UNORM, falling back to R8G8B8A8_UNORM");
		negotiatedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		fallbackUsed = true;
	}

	logger::info("[DX12SwapChain] Swap chain format negotiation: attempted={}, negotiated={}, VR={}, fallback={}",
		static_cast<uint32_t>(attemptedFormat),
		static_cast<uint32_t>(negotiatedFormat),
		isVR ? "true" : "false",
		fallbackUsed ? "true" : "false");

	swapChainDesc = {};
	swapChainDesc.Width = a_swapChainDesc.BufferDesc.Width;
	swapChainDesc.Height = a_swapChainDesc.BufferDesc.Height;
	swapChainDesc.Format = negotiatedFormat;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 2;
	swapChainDesc.SwapEffect = a_swapChainDesc.SwapEffect;
	swapChainDesc.Flags = a_swapChainDesc.Flags;

	ffx::CreateContextDescFrameGenerationSwapChainForHwndDX12 ffxSwapChainDesc{};

	ffxSwapChainDesc.desc = &swapChainDesc;
	ffxSwapChainDesc.dxgiFactory = dxgiFactory;
	ffxSwapChainDesc.fullscreenDesc = nullptr;
	ffxSwapChainDesc.gameQueue = commandQueue.get();
	ffxSwapChainDesc.hwnd = a_swapChainDesc.OutputWindow;
	ffxSwapChainDesc.swapchain = &swapChain;

	auto& fidelityFX = globals::features::upscaling.fidelityFX;

	if (ffx::CreateContext(fidelityFX.swapChainContext, nullptr, ffxSwapChainDesc) != ffx::ReturnCode::Ok) {
		logger::critical("[FidelityFX] Failed to create swap chain context!");
	}

	DX::ThrowIfFailed(swapChain->GetBuffer(0, IID_PPV_ARGS(&swapChainBuffers[0])));
	DX::ThrowIfFailed(swapChain->GetBuffer(1, IID_PPV_ARGS(&swapChainBuffers[1])));

	frameIndex = swapChain->GetCurrentBackBufferIndex();

	// Set color space based on HDR Display feature state and negotiated format
	auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
	bool enableHDR = hdr && hdr->settings.enableHDR;
	// Only set HDR color space if not falling back to SDR format
	SetColorSpace(enableHDR && !fallbackUsed);

	fidelityFX.SetupFrameGeneration();
}

void DX12SwapChain::CreateSwapChainDirect(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	CreateD3D12Device(adapter);

	IDXGIFactory4* factoryRaw{};
	DX::ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&factoryRaw)));

	// CreateSwapChainForHwnd must go through the SL-upgraded factory (a mandatory manual
	// hook), or Streamline never recognizes the swap chain as its own.
	auto& streamlineDX12 = globals::features::upscaling.streamlineDX12;
	if (streamlineDX12.slUpgradeInterface)
		streamlineDX12.slUpgradeInterface((void**)&factoryRaw);
	winrt::com_ptr<IDXGIFactory4> dxgiFactory;
	dxgiFactory.attach(factoryRaw);

	DXGI_FORMAT attemptedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	DXGI_FORMAT negotiatedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	bool fallbackUsed = false;

	D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_FORMAT_SUPPORT1_RENDER_TARGET, D3D12_FORMAT_SUPPORT2_NONE };
	if (SUCCEEDED(d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)))) {
		if ((formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0) {
			logger::warn("[DX12SwapChain] R10G10B10A2_UNORM not supported as render target, falling back to R8G8B8A8_UNORM");
			negotiatedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			fallbackUsed = true;
		}
	} else {
		logger::warn("[DX12SwapChain] CheckFeatureSupport failed for R10G10B10A2_UNORM, falling back to R8G8B8A8_UNORM");
		negotiatedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		fallbackUsed = true;
	}

	logger::info("[DX12SwapChain] Direct swap chain format negotiation: attempted={}, negotiated={}, fallback={}",
		static_cast<uint32_t>(attemptedFormat),
		static_cast<uint32_t>(negotiatedFormat),
		fallbackUsed ? "true" : "false");

	swapChainDesc = {};
	swapChainDesc.Width = a_swapChainDesc.BufferDesc.Width;
	swapChainDesc.Height = a_swapChainDesc.BufferDesc.Height;
	swapChainDesc.Format = negotiatedFormat;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	// Three backbuffers: the SL pacer holds one for composition while flipping generated
	// frames; a two-buffer chain leaves it no slack.
	swapChainDesc.BufferCount = 3;
	swapChainDesc.SwapEffect = a_swapChainDesc.SwapEffect;
	// No FRAME_LATENCY_WAITABLE_OBJECT: waiting on it serializes presents to one in
	// flight, which makes the SL pacer drop every interpolated frame.
	swapChainDesc.Flags = a_swapChainDesc.Flags & ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

	winrt::com_ptr<IDXGISwapChain1> swapChain1;
	DX::ThrowIfFailed(dxgiFactory->CreateSwapChainForHwnd(
		commandQueue.get(),
		a_swapChainDesc.OutputWindow,
		&swapChainDesc,
		nullptr,
		nullptr,
		swapChain1.put()));

	DX::ThrowIfFailed(swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain)));

	for (UINT i = 0; i < 3; i++) {
		DX::ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&swapChainBuffers[i])));
		const std::wstring bufferName = L"DX12SwapChain::DirectBackBuffer[" + std::to_wstring(i) + L"]";
		swapChainBuffers[i]->SetName(bufferName.c_str());
	}

	frameIndex = swapChain->GetCurrentBackBufferIndex();

	auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
	bool enableHDR = hdr && hdr->settings.enableHDR;
	SetColorSpace(enableHDR && !fallbackUsed);

	useDLSSG = true;
	logger::info("[DX12SwapChain] Created direct swap chain for DLSS-G ({}x{})", swapChainDesc.Width, swapChainDesc.Height);
}

void DX12SwapChain::CreateInterop()
{
	HANDLE sharedFenceHandle;
	DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d12Fence)));
	DX::ThrowIfFailed(d3d12Device->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle));
	DX::ThrowIfFailed(d3d11Device->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(&d3d11Fence)));
	CloseHandle(sharedFenceHandle);

	swapChainProxy = new DXGISwapChainProxy(swapChain);

	RecreateWrappedResources(swapChainDesc);
}

void DX12SwapChain::RecreateWrappedResources(const DXGI_SWAP_CHAIN_DESC1& desc)
{
	D3D11_TEXTURE2D_DESC texDesc11{};
	texDesc11.Width = desc.Width;
	texDesc11.Height = desc.Height;
	texDesc11.MipLevels = 1;
	texDesc11.ArraySize = 1;
	texDesc11.Format = desc.Format;
	texDesc11.SampleDesc.Count = 1;
	texDesc11.SampleDesc.Quality = 0;
	texDesc11.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;

	// Build both replacements before releasing the active resources so a failed
	// allocation cannot leave the proxy with only half of its interop textures.
	auto newSwapChainBuffer = std::make_unique<WrappedResource>(texDesc11, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::SwapChainBuffer");

	// UI buffer uses R8G8B8A8_UNORM - vanilla UI is SDR and 8-bit precision
	texDesc11.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	auto newUiBuffer = std::make_unique<WrappedResource>(texDesc11, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::UIBuffer");

	delete swapChainBufferWrapped;
	delete uiBufferWrapped;
	swapChainBufferWrapped = newSwapChainBuffer.release();
	uiBufferWrapped = newUiBuffer.release();

	const float clearColor[4]{};
	d3d11Context->ClearRenderTargetView(swapChainBufferWrapped->rtv, clearColor);
	d3d11Context->ClearRenderTargetView(uiBufferWrapped->rtv, clearColor);
}

DXGISwapChainProxy* DX12SwapChain::GetSwapChainProxy()
{
	return swapChainProxy;
}

void DX12SwapChain::SetD3D11Device(ID3D11Device* a_d3d11Device)
{
	DX::ThrowIfFailed(a_d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device)));
}

void DX12SwapChain::SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context)
{
	DX::ThrowIfFailed(a_d3d11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context)));
}

HRESULT DX12SwapChain::GetBuffer(UINT buffer, REFIID riid, void** ppSurface)
{
	if (!ppSurface)
		return E_POINTER;

	*ppSurface = nullptr;
	if (buffer != 0 || !swapChainBufferWrapped || !swapChainBufferWrapped->resource11)
		return DXGI_ERROR_INVALID_CALL;

	// IDXGISwapChain::GetBuffer returns an owned COM reference. Returning the raw
	// pointer here let the caller's Release destroy the shared texture while the
	// D3D12 side still retained and submitted its corresponding resource.
	return swapChainBufferWrapped->resource11->QueryInterface(riid, ppSurface);
}

HRESULT DX12SwapChain::ResizeBuffers(UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
	if (!swapChain)
		return DXGI_ERROR_INVALID_CALL;

	// DXGI defines zero as "preserve the current buffer count". FidelityFX's
	// frame-generation swap-chain stores the supplied value verbatim and uses it
	// as its replacement-buffer count, so forwarding zero leaves it with no valid
	// source resource at the next Present.
	const UINT effectiveBufferCount = bufferCount ? bufferCount : swapChainDesc.BufferCount;
	if (!bufferCount)
		logger::warn("[FidelityFX] Normalized ResizeBuffers count from 0 to {} to preserve replacement buffers", effectiveBufferCount);
	if (effectiveBufferCount != 2) {
		logger::error("[DX12SwapChain] Rejected unsupported resize buffer count {} (CS requires 2)", effectiveBufferCount);
		return DXGI_ERROR_UNSUPPORTED;
	}

	// These references are to FidelityFX replacement buffers. They must not keep
	// the old generation alive across the provider's resize, and must be refreshed
	// before CS records another copy.
	swapChainBuffers[0] = nullptr;
	swapChainBuffers[1] = nullptr;
	const HRESULT result = swapChain->ResizeBuffers(effectiveBufferCount, width, height, format, flags);
	if (FAILED(result)) {
		// The resize didn't take effect, so the pre-resize buffers should still be
		// valid (unless the device itself is gone, in which case this also fails
		// and Present's null guard below is the last line of defense).
		swapChain->GetBuffer(0, IID_PPV_ARGS(swapChainBuffers[0].put()));
		swapChain->GetBuffer(1, IID_PPV_ARGS(swapChainBuffers[1].put()));
		return result;
	}

	DXGI_SWAP_CHAIN_DESC1 resizedDesc{};
	const HRESULT descResult = swapChain->GetDesc1(&resizedDesc);
	if (FAILED(descResult)) {
		// The resize itself succeeded; only the desc query failed. Re-fetch the
		// (already resized) buffers so Present isn't left with nulls.
		swapChain->GetBuffer(0, IID_PPV_ARGS(swapChainBuffers[0].put()));
		swapChain->GetBuffer(1, IID_PPV_ARGS(swapChainBuffers[1].put()));
		return descResult;
	}

	const bool wrappedResourcesChanged = resizedDesc.Width != swapChainDesc.Width ||
	                                     resizedDesc.Height != swapChainDesc.Height ||
	                                     resizedDesc.Format != swapChainDesc.Format;
	if (wrappedResourcesChanged)
		RecreateWrappedResources(resizedDesc);
	swapChainDesc = resizedDesc;

	DX::ThrowIfFailed(swapChain->GetBuffer(0, IID_PPV_ARGS(swapChainBuffers[0].put())));
	DX::ThrowIfFailed(swapChain->GetBuffer(1, IID_PPV_ARGS(swapChainBuffers[1].put())));
	frameIndex = swapChain->GetCurrentBackBufferIndex();
	return S_OK;
}

HRESULT DX12SwapChain::Present(UINT SyncInterval, UINT Flags)
{
	auto& upscaling = globals::features::upscaling;

	// Scale UI brightness BEFORE fence sync so the D3D11 UIBrightnessCS dispatch
	// is covered by the D3D11→D3D12 fence. Without this, FidelityFX may read
	// uiBufferWrapped on D3D12 before the PQ encoding completes on D3D11.
	// Only runs when HDR Display feature is loaded (UIBrightnessCS may not exist otherwise)
	auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
	if (hdr)
		hdr->ScaleUIBrightnessForFG();

	bool isHDR = hdr && hdr->settings.enableHDR;

	// Wait for D3D11 to finish (includes ApplyHDR scene encoding AND UIBrightnessCS)
	fenceValue++;
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));

	// New frame, reset
	if (frameFenceValues[frameIndex])
		DX::ThrowIfFailed(d3d12Fence->SetEventOnCompletion(frameFenceValues[frameIndex], nullptr));
	DX::ThrowIfFailed(commandAllocators[frameIndex]->Reset());
	DX::ThrowIfFailed(commandLists[frameIndex]->Reset(commandAllocators[frameIndex].get(), nullptr));

	// Copy shared texture to swap chain buffer
	{
		auto fakeSwapChain = swapChainBufferWrapped->resource.get();
		auto realSwapChain = swapChainBuffers[frameIndex].get();
		// Null only after a resize failure severe enough that even the pre-resize
		// buffers couldn't be re-fetched (e.g. device removed) -- skip this frame's
		// copy/present rather than pass a null resource to D3D12.
		if (!realSwapChain)
			return DXGI_ERROR_DEVICE_REMOVED;
		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
			commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}

		commandLists[frameIndex]->CopyResource(realSwapChain, fakeSwapChain);

		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
			commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}
	}

	if (useDLSSG) {
		auto& streamlineDX12 = upscaling.streamlineDX12;
		streamlineDX12.EnsureFrameToken();
		// The full per-frame PCL marker sequence is structural for interpolation
		// (eSimulationStart is emitted at the Reflex sleep site).
		streamlineDX12.EmitPCLMarker(sl::PCLMarker::eSimulationEnd);
		streamlineDX12.EmitPCLMarker(sl::PCLMarker::eRenderSubmitStart);
		// Skip tagging/interpolation without valid per-frame constants -- otherwise
		// DLSS-G interpolates against stale or default camera data.
		if (streamlineDX12.CheckFrameConstants(streamlineDX12.viewport)) {
			streamlineDX12.TagDX12Resources(commandLists[frameIndex].get(),
				depthBufferShared12 ? depthBufferShared12->resource.get() : nullptr,
				motionVectorBufferShared12 ? motionVectorBufferShared12->resource.get() : nullptr,
				swapChainBufferWrapped ? swapChainBufferWrapped->resource.get() : nullptr,
				uiBufferWrapped ? uiBufferWrapped->resource.get() : nullptr,
				swapChainDesc.Width, swapChainDesc.Height);
			streamlineDX12.ConfigureDLSSG(upscaling.ShouldUseFrameGenerationThisFrame());
		} else {
			streamlineDX12.ConfigureDLSSG(false);
		}
	} else {
		upscaling.fidelityFX.Present(upscaling.ShouldUseFrameGenerationThisFrame(), isHDR);
	}

	DX::ThrowIfFailed(commandLists[frameIndex]->Close());

	ID3D12CommandList* commandListsToExecute[] = { commandLists[frameIndex].get() };
	commandQueue->ExecuteCommandLists(1, commandListsToExecute);

	if (useDLSSG) {
		upscaling.streamlineDX12.EmitPCLMarker(sl::PCLMarker::eRenderSubmitEnd);
		upscaling.streamlineDX12.EmitPCLMarker(sl::PCLMarker::ePresentStart);
	}

	// Present the frame
	DX::ThrowIfFailed(swapChain->Present(SyncInterval, Flags));

	if (useDLSSG)
		upscaling.streamlineDX12.EmitPCLMarker(sl::PCLMarker::ePresentEnd);

	// Wait for D3D12 to finish
	fenceValue++;
	DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), fenceValue));
	frameFenceValues[frameIndex] = fenceValue;
	DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), fenceValue));

	// Update the frame index
	frameIndex = swapChain->GetCurrentBackBufferIndex();

	float clearColor[4]{ 0, 0, 0, 0 };
	d3d11Context->ClearRenderTargetView(uiBufferWrapped->rtv, clearColor);

	// If VSync is disabled, use frame limiter to prevent tearing and optimise pacing
	if (SyncInterval == 0)
		upscaling.FrameLimiter();

	return S_OK;
}

HRESULT DX12SwapChain::GetDevice(REFIID uuid, void** ppDevice)
{
	if (uuid == __uuidof(ID3D11Device) || uuid == __uuidof(ID3D11Device1) || uuid == __uuidof(ID3D11Device2) || uuid == __uuidof(ID3D11Device3) || uuid == __uuidof(ID3D11Device4) || uuid == __uuidof(ID3D11Device5)) {
		*ppDevice = d3d11Device.get();
		return S_OK;
	}

	return swapChain->GetDevice(uuid, ppDevice);
}

HANDLE DX12SwapChain::GetFrameLatencyWaitableObject()
{
	return swapChain->GetFrameLatencyWaitableObject();
}

float DX12SwapChain::GetFrameTime() const
{
	// Calculate frame time based on swap chain presentation
	static float lastPresentTime = 0.0f;
	static float frameTime = 1.0f / 60.0f;  // Default to 60 fps
	static LARGE_INTEGER frequency = {};
	static LARGE_INTEGER currentTime = {};

	if (frequency.QuadPart == 0) {
		QueryPerformanceFrequency(&frequency);
	}

	QueryPerformanceCounter(&currentTime);
	float time = static_cast<float>(currentTime.QuadPart) / static_cast<float>(frequency.QuadPart);

	if (lastPresentTime > 0.0f) {
		frameTime = time - lastPresentTime;
	}
	lastPresentTime = time;

	return frameTime;
}

WrappedResource::WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device, const std::string& a_name)
{
	// Create D3D11 shared texture directly instead of wrapping D3D12 resource
	a_texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	auto throwIfFailed = [&](HRESULT a_result, const char* a_operation) {
		if (FAILED(a_result)) {
			logger::error(
				"[DX12SwapChain] Wrapped resource '{}' {} failed: HRESULT 0x{:08X}, dimensions {}x{}, format {}, bind flags 0x{:X}, misc flags 0x{:X}",
				a_name.empty() ? "<unnamed>" : a_name.c_str(),
				a_operation,
				static_cast<uint32_t>(a_result),
				a_texDesc.Width,
				a_texDesc.Height,
				static_cast<uint32_t>(a_texDesc.Format),
				a_texDesc.BindFlags,
				a_texDesc.MiscFlags);
		}
		DX::ThrowIfFailed(a_result);
	};

	throwIfFailed(a_d3d11Device->CreateTexture2D(&a_texDesc, nullptr, &resource11), "CreateTexture2D");
	if (!a_name.empty())
		Util::SetResourceName(resource11, "%s", a_name.c_str());

	// Get shared handle from D3D11 texture to enable D3D12 access
	winrt::com_ptr<IDXGIResource1> dxgiResource;
	throwIfFailed(resource11->QueryInterface(IID_PPV_ARGS(dxgiResource.put())), "QueryInterface(IDXGIResource1)");
	HANDLE sharedHandle = nullptr;
	throwIfFailed(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle), "CreateSharedHandle");

	// Open the shared D3D11 texture as D3D12 resource. Close the NT handle
	// unconditionally before checking the result -- a thrown failure must
	// not leak it.
	const HRESULT openResult = a_d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(resource.put()));
	CloseHandle(sharedHandle);
	throwIfFailed(openResult, "OpenSharedHandle");

	if (a_texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = a_texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		throwIfFailed(a_d3d11Device->CreateShaderResourceView(resource11, &srvDesc, &srv), "CreateShaderResourceView");
		if (!a_name.empty())
			Util::SetResourceName(srv, "%s SRV", a_name.c_str());
	}

	if (a_texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
		if (a_texDesc.ArraySize > 1) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = a_texDesc.ArraySize;

			throwIfFailed(a_d3d11Device->CreateUnorderedAccessView(resource11, &uavDesc, &uav), "CreateUnorderedAccessView");
		} else {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;

			throwIfFailed(a_d3d11Device->CreateUnorderedAccessView(resource11, &uavDesc, &uav), "CreateUnorderedAccessView");
		}
		if (!a_name.empty())
			Util::SetResourceName(uav, "%s UAV", a_name.c_str());
	}

	if (a_texDesc.BindFlags & D3D11_BIND_RENDER_TARGET) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = a_texDesc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		throwIfFailed(a_d3d11Device->CreateRenderTargetView(resource11, &rtvDesc, &rtv), "CreateRenderTargetView");
		if (!a_name.empty())
			Util::SetResourceName(rtv, "%s RTV", a_name.c_str());
	}
}

WrappedResource::~WrappedResource()
{
	if (resource11) {
		resource11->Release();
		resource11 = nullptr;
	}
	if (srv) {
		srv->Release();
		srv = nullptr;
	}
	if (uav) {
		uav->Release();
		uav = nullptr;
	}
	if (rtv) {
		rtv->Release();
		rtv = nullptr;
	}
	// resource (winrt::com_ptr) will be automatically released
}

DXGISwapChainProxy::DXGISwapChainProxy(IDXGISwapChain4* a_swapChain)
{
	swapChain = a_swapChain;
}

/****IUknown****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::QueryInterface(REFIID riid, void** ppvObj)
{
	auto ret = swapChain->QueryInterface(riid, ppvObj);
	if (*ppvObj)
		*ppvObj = this;
	return ret;
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::AddRef()
{
	return swapChain->AddRef();
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::Release()
{
	return swapChain->Release();
}

/****IDXGIObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData)
{
	return swapChain->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown)
{
	return swapChain->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData)
{
	return swapChain->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent)
{
	return swapChain->GetParent(riid, ppParent);
}

/****IDXGIDeviceSubObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice)
{
	return globals::features::upscaling.dx12SwapChain.GetDevice(riid, ppDevice);
}

/****IDXGISwapChain****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present(UINT SyncInterval, UINT Flags)
{
	return globals::features::upscaling.dx12SwapChain.Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBuffer(UINT buffer, _In_ REFIID riid, _COM_Outptr_ void** ppSurface)
{
	return globals::features::upscaling.dx12SwapChain.GetBuffer(buffer, riid, ppSurface);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget)
{
	return swapChain->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget)
{
	return swapChain->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc)
{
	return swapChain->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	return globals::features::upscaling.dx12SwapChain.ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters)
{
	return swapChain->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput)
{
	return swapChain->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats)
{
	return swapChain->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetLastPresentCount(_Out_ UINT* pLastPresentCount)
{
	return swapChain->GetLastPresentCount(pLastPresentCount);
}

void DX12SwapChain::SetColorSpace(bool enableHDR)
{
	if (!swapChain)
		return;

	if (enableHDR) {
		swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
		logger::info("[DX12SwapChain] Set color space to HDR10 (PQ/BT.2020)");
	} else {
		swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		logger::info("[DX12SwapChain] Set color space to SDR (sRGB)");
	}
}

DX12SwapChain::BlurResources DX12SwapChain::GetBlurResources() const
{
	BlurResources res;
	if (swapChainBufferWrapped) {
		res.backbufferTex = swapChainBufferWrapped->resource11;
		res.backbufferRTV = swapChainBufferWrapped->rtv;
		res.backbufferSRV = swapChainBufferWrapped->srv;
	}
	if (uiBufferWrapped) {
		res.uiBufferSRV = uiBufferWrapped->srv;
		res.uiBufferRTV = uiBufferWrapped->rtv;
	}
	return res;
}

void DX12SwapChain::CreateSharedResources()
{
	auto renderer = globals::game::renderer;

	// Create depth buffer
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC texDesc{};
	main.texture->GetDesc(&texDesc);
	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	depthBufferShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::DepthBufferShared");

	// Create motion vector buffer
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	motionVector.texture->GetDesc(&texDesc);
	motionVectorBufferShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::MotionVectorBufferShared");
}
