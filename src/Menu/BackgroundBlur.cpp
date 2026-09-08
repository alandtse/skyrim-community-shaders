// Inspired by Unrimp rendering engine's separable blur implementation
// Credits: Christian Ofenberg and the Unrimp project (https://github.com/cofenberg/unrimp)
// License: MIT License

#include "BackgroundBlur.h"
#include "../Features/HDRDisplay.h"
#include "../Features/Upscaling.h"
#include "../Globals.h"
#include "../GpuPass.h"
#include "../I18n/I18n.h"
#include "../State.h"
#include "../Util.h"

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_internal.h>

#include "RE/Skyrim.h"

using namespace std::literals;

// Downsampling factor (8 = eighth resolution for performance)
constexpr UINT DOWNSAMPLE_FACTOR = 8;

// Extra pixels added around scissor rect for anti-aliased rounded corner edges
constexpr float SCISSOR_AA_PADDING = 2.0f;

// Vertex count for a fullscreen triangle draw call
constexpr UINT FULLSCREEN_TRIANGLE_VERTICES = 3;

namespace BackgroundBlur
{
	// Module-local state
	namespace
	{
		std::mutex resourceMutex;
		bool enabled = false;
		bool csEditorActive = false;

		// DirectX resources (RAII managed)
		winrt::com_ptr<ID3D11VertexShader> vertexShader;
		winrt::com_ptr<ID3D11PixelShader> downsamplePixelShader;
		winrt::com_ptr<ID3D11PixelShader> copyPixelShader;
		winrt::com_ptr<ID3D11PixelShader> horizontalPixelShader;
		winrt::com_ptr<ID3D11PixelShader> verticalPixelShader;
		winrt::com_ptr<ID3D11PixelShader> compositePixelShader;  // For rounded corner compositing
		winrt::com_ptr<ID3D11PixelShader> clearPixelShader;      // For rounded corner UI buffer clearing
		winrt::com_ptr<ID3D11PixelShader> layerPixelShader;
		winrt::com_ptr<ID3D11Buffer> constantBuffer;
		winrt::com_ptr<ID3D11SamplerState> samplerState;
		winrt::com_ptr<ID3D11BlendState> blendState;
		winrt::com_ptr<ID3D11RasterizerState> scissorRasterizerState;

		// Blend state for compositing UI over game world (alpha blending)
		winrt::com_ptr<ID3D11BlendState> compositeBlendState;

		// Downsampled textures for blur (1/8 res for performance)
		winrt::com_ptr<ID3D11Texture2D> downsampleTexture;
		winrt::com_ptr<ID3D11RenderTargetView> downsampleRTV;
		winrt::com_ptr<ID3D11ShaderResourceView> downsampleSRV;

		// Intermediate blur textures (at downsampled resolution)
		winrt::com_ptr<ID3D11Texture2D> blurTexture1;
		winrt::com_ptr<ID3D11Texture2D> blurTexture2;
		winrt::com_ptr<ID3D11RenderTargetView> blurRTV1;
		winrt::com_ptr<ID3D11RenderTargetView> blurRTV2;
		winrt::com_ptr<ID3D11ShaderResourceView> blurSRV1;
		winrt::com_ptr<ID3D11ShaderResourceView> blurSRV2;

		// Cached SRV for non-upscaling path (avoids per-frame CreateShaderResourceView)
		winrt::com_ptr<ID3D11ShaderResourceView> cachedSourceSRV;
		ID3D11Texture2D* cachedSourceTexture = nullptr;  // raw pointer for cache invalidation check

		winrt::com_ptr<ID3D11Texture2D> editorUITexture;
		winrt::com_ptr<ID3D11RenderTargetView> editorUIRTV;
		winrt::com_ptr<ID3D11ShaderResourceView> editorUISRV;

		struct RetainedBuffer
		{
			winrt::com_ptr<ID3D11Texture2D> source;
			winrt::com_ptr<ID3D11Texture2D> clean;
			uint64_t generation = 0;
			bool dirty = false;
		};
		RetainedBuffer retainedScene;
		RetainedBuffer retainedUI;

		UINT textureWidth = 0;
		UINT textureHeight = 0;
		UINT downsampledWidth = 0;
		UINT downsampledHeight = 0;
		DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;

		bool initialized = false;
		bool initializationFailed = false;

		struct BlurConstants
		{
			float blurTextureSize[4];
			float windowRect[4];
			float windowParams[4];
		};
		static_assert(sizeof(BlurConstants) == 48);

		struct UIBufferViews
		{
			ID3D11ShaderResourceView* srv = nullptr;
			ID3D11RenderTargetView* rtv = nullptr;
		};

		struct BlurFrame
		{
			ID3D11ShaderResourceView* sourceSRV;
			ID3D11RenderTargetView* targetRTV;
			ID3D11RenderTargetView* imguiRTV;
			UIBufferViews uiBuffer;
			bool uiOnly = false;
		};

		struct WindowBlur
		{
			const BlurFrame* frame;
			ImDrawList* drawList;
			BlurConstants constants;
			int drawOrder;
			bool needed = true;
		};

		bool ShouldUseD3D12UIBufferForBlur()
		{
			return globals::features::hdrDisplay.ShouldUseD3D12UIBuffer();
		}

		UIBufferViews GetD3D12UIBufferViews(const DX12SwapChain::BlurResources& res)
		{
			return { res.uiBufferSRV, res.uiBufferRTV };
		}

		UIBufferViews GetHDRUIBufferViews(HDRDisplay& hdr, Upscaling& upscaling)
		{
			if (upscaling.d3d12SwapChainActive && ShouldUseD3D12UIBufferForBlur())
				return GetD3D12UIBufferViews(upscaling.GetBlurResources());

			if (hdr.uiTexture && hdr.uiTexture->srv && hdr.uiTexture->rtv)
				return { hdr.uiTexture->srv.get(), hdr.uiTexture->rtv.get() };

			return {};
		}

		bool IsStartupMenuBlurSourceReady()
		{
			return globals::state && globals::state->startupMenuBlurSourceReady;
		}

		winrt::com_ptr<ID3D11Texture2D> GetTexture(ID3D11View* view)
		{
			winrt::com_ptr<ID3D11Resource> resource;
			if (view)
				view->GetResource(resource.put());
			return resource ? resource.try_as<ID3D11Texture2D>() : nullptr;
		}

		bool CopyCompatible(ID3D11Texture2D* first, ID3D11Texture2D* second)
		{
			if (!first || !second)
				return false;
			D3D11_TEXTURE2D_DESC a{}, b{};
			first->GetDesc(&a);
			second->GetDesc(&b);
			return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format &&
			       a.MipLevels == b.MipLevels && a.ArraySize == b.ArraySize &&
			       a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality;
		}

		void RestoreBuffer(RetainedBuffer& buffer, ID3D11Texture2D* current, uint64_t generation)
		{
			if (buffer.source.get() != current || buffer.generation != generation) {
				buffer.source = nullptr;
				buffer.dirty = false;
				return;
			}
			if (buffer.dirty && CopyCompatible(current, buffer.clean.get())) {
				CS_GPU_PASS("BackgroundBlur::RestoreRetainedBuffer");
				globals::d3d::context->CopyResource(current, buffer.clean.get());
			}
			buffer.dirty = false;
		}

		bool PreserveUIBuffer(const UIBufferViews& uiBuffer)
		{
			if (!uiBuffer.rtv || !globals::state->IsPausedOrMenuOpen(globals::game::ui)) {
				retainedUI = {};
				return true;
			}
			auto source = GetTexture(uiBuffer.rtv);
			if (!source)
				return false;
			const auto generation = globals::features::hdrDisplay.uiGeneration;
			if (retainedUI.source == source && retainedUI.generation == generation && retainedUI.clean)
				return true;

			retainedUI.source = source;
			retainedUI.generation = generation;
			if (!CopyCompatible(source.get(), retainedUI.clean.get())) {
				retainedUI.clean = nullptr;
				D3D11_TEXTURE2D_DESC desc{};
				source->GetDesc(&desc);
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = 0;
				desc.CPUAccessFlags = 0;
				desc.MiscFlags = 0;
				if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, retainedUI.clean.put())))
					return false;
				Util::SetResourceName(retainedUI.clean.get(), "BackgroundBlur::RetainedUI");
			}
			CS_GPU_PASS("BackgroundBlur::PreserveUI");
			globals::d3d::context->CopyResource(retainedUI.clean.get(), source.get());
			return true;
		}

		void ReleaseEditorLayer()
		{
			editorUISRV = nullptr;
			editorUIRTV = nullptr;
			editorUITexture = nullptr;
		}

		// Release all blur texture resources; caller must hold resourceMutex
		void ReleaseBlurTextures()
		{
			downsampleTexture = nullptr;
			downsampleRTV = nullptr;
			downsampleSRV = nullptr;
			blurTexture1 = nullptr;
			blurTexture2 = nullptr;
			blurRTV1 = nullptr;
			blurRTV2 = nullptr;
			blurSRV1 = nullptr;
			blurSRV2 = nullptr;
			textureWidth = 0;
			textureHeight = 0;
			downsampledWidth = 0;
			downsampledHeight = 0;
			textureFormat = DXGI_FORMAT_UNKNOWN;
		}

		// Create a Texture2D with associated RTV and SRV
		bool CreateTextureSet(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc,
			winrt::com_ptr<ID3D11Texture2D>& tex,
			winrt::com_ptr<ID3D11RenderTargetView>& rtv,
			winrt::com_ptr<ID3D11ShaderResourceView>& srv,
			const char* name)
		{
			if (FAILED(device->CreateTexture2D(&desc, nullptr, tex.put()))) {
				logger::error("Failed to create {} texture", name);
				return false;
			}
			Util::SetResourceName(tex.get(), "BackgroundBlur::%s", name);
			if (FAILED(device->CreateRenderTargetView(tex.get(), nullptr, rtv.put()))) {
				logger::error("Failed to create {} RTV", name);
				return false;
			}
			Util::SetResourceName(rtv.get(), "BackgroundBlur::%s RTV", name);
			if (FAILED(device->CreateShaderResourceView(tex.get(), nullptr, srv.put()))) {
				logger::error("Failed to create {} SRV", name);
				return false;
			}
			Util::SetResourceName(srv.get(), "BackgroundBlur::%s SRV", name);
			return true;
		}

		bool CreateEditorLayer(UINT width, UINT height)
		{
			D3D11_TEXTURE2D_DESC desc{};
			if (editorUITexture) {
				editorUITexture->GetDesc(&desc);
				if (desc.Width == width && desc.Height == height)
					return true;
			}
			ReleaseEditorLayer();
			desc = {};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			if (!CreateTextureSet(globals::d3d::device, desc, editorUITexture, editorUIRTV, editorUISRV, "EditorUI")) {
				ReleaseEditorLayer();
				return false;
			}
			return true;
		}

		std::vector<WindowBlur> GatherWindows(ImDrawData* drawData, const BlurFrame& frame, BlurConstants constants)
		{
			std::vector<WindowBlur> windows;
			auto* ctx = ImGui::GetCurrentContext();
			const auto* performanceOverlay = ImGui::FindWindowByName(T("feature.perf_overlay.overlay_title", "Performance Overlay"));
			windows.reserve(ctx->Windows.Size);
			constants.windowParams[3] = 0.0f;
			for (auto* window : ctx->Windows) {
				if (!window->Active || window->Hidden || window->SkipItems)
					continue;
				if ((window->Flags & ImGuiWindowFlags_ChildWindow) && !(window->Flags & ImGuiWindowFlags_Popup) && !window->DockIsActive)
					continue;
				if (window->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_DockNodeHost))
					continue;
				if (window == performanceOverlay || Util::IsUnblurredFlyoutWindowName(window->Name))
					continue;

				// Docked backgrounds and tabs are drawn by the host before the window's content.
				auto* drawList = window->DockIsActive && window->DockNode && window->DockNode->HostWindow ?
				                     window->DockNode->HostWindow->DrawList :
				                     window->DrawList;
				const auto it = std::ranges::find(drawData->CmdLists, drawList);
				if (it == drawData->CmdLists.end())
					continue;

				const ImRect rect = window->Rect();
				const auto scale = drawData->FramebufferScale;
				constants.windowRect[0] = (rect.Min.x - drawData->DisplayPos.x) * scale.x;
				constants.windowRect[1] = (rect.Min.y - drawData->DisplayPos.y) * scale.y;
				constants.windowRect[2] = (rect.Max.x - drawData->DisplayPos.x) * scale.x;
				constants.windowRect[3] = (rect.Max.y - drawData->DisplayPos.y) * scale.y;
				constants.windowParams[0] = window->WindowRounding * (std::min)(scale.x, scale.y);
				windows.push_back({ &frame, drawList, constants, static_cast<int>(it - drawData->CmdLists.begin()) });
			}
			if (csEditorActive) {
				for (auto& window : windows) {
					const auto* rect = window.constants.windowRect;
					window.needed = std::ranges::any_of(windows, [&](const WindowBlur& lower) {
						const auto* other = lower.constants.windowRect;
						return lower.drawOrder < window.drawOrder && rect[0] < other[2] && rect[2] > other[0] &&
						       rect[1] < other[3] && rect[3] > other[1];
					});
				}
				std::erase_if(windows, [](const WindowBlur& window) { return !window.needed; });
			}
			return windows;
		}

	}  // anonymous namespace

	void RestoreRetainedBuffers()
	{
		if (globals::game::isVR || !globals::d3d::context)
			return;
		std::lock_guard<std::mutex> lock(resourceMutex);
		auto& hdr = globals::features::hdrDisplay;
		auto& upscaling = globals::features::upscaling;
		const bool hdrActive = hdr.loaded && hdr.settings.enableHDR && hdr.hdrTexture;
		RestoreBuffer(retainedScene, hdrActive ? hdr.hdrTexture->resource.get() : nullptr, hdr.sceneGeneration);
		retainedScene = {};
		if (retainedUI.source) {
			const auto uiBuffer = hdrActive ? GetHDRUIBufferViews(hdr, upscaling) :
			                      upscaling.d3d12SwapChainActive && ShouldUseD3D12UIBufferForBlur() ?
			                                  GetD3D12UIBufferViews(upscaling.GetBlurResources()) :
			                                  UIBufferViews{};
			RestoreBuffer(retainedUI, GetTexture(uiBuffer.rtv).get(), hdr.uiGeneration);
		}
		if (!enabled || !globals::state->IsPausedOrMenuOpen(globals::game::ui))
			retainedUI = {};
		if (!enabled || !csEditorActive)
			ReleaseEditorLayer();
	}

	bool Initialize()
	{
		std::lock_guard<std::mutex> lock(resourceMutex);

		if (initialized || initializationFailed) {
			return initialized;
		}

		auto device = globals::d3d::device;
		if (!device) {
			initializationFailed = true;
			return false;
		}

		// Compile shaders
		auto compileShader = [&](auto& shader, const wchar_t* path, const char* target, const char* entry, const char* name) -> bool {
			shader.attach(static_cast<decltype(shader.get())>(Util::CompileShader(path, {}, target, entry)));
			if (!shader) {
				logger::error("Failed to compile {}", name);
				initializationFailed = true;
				return false;
			}
			return true;
		};

		if (!compileShader(vertexShader, L"Data\\Shaders\\Menu\\BackgroundBlurHorizontal.hlsl", "vs_5_0", "VS_Main", "blur vertex shader") ||
			!compileShader(copyPixelShader, L"Data\\Shaders\\Menu\\BackgroundBlurHorizontal.hlsl", "ps_5_0", "PS_Copy", "UI composite pixel shader") ||
			!compileShader(downsamplePixelShader, L"Data\\Shaders\\Menu\\BackgroundBlurHorizontal.hlsl", "ps_5_0", "PS_Downsample", "blur downsample pixel shader") ||
			!compileShader(horizontalPixelShader, L"Data\\Shaders\\Menu\\BackgroundBlurHorizontal.hlsl", "ps_5_0", "PS_Main", "horizontal blur pixel shader") ||
			!compileShader(verticalPixelShader, L"Data\\Shaders\\Menu\\BackgroundBlurVertical.hlsl", "ps_5_0", "PS_Main", "vertical blur pixel shader") ||
			!compileShader(compositePixelShader, L"Data\\Shaders\\Menu\\BackgroundBlurComposite.hlsl", "ps_5_0", "PS_Main", "composite blur pixel shader") ||
			!compileShader(layerPixelShader, L"Data\\Shaders\\Menu\\BackgroundBlurComposite.hlsl", "ps_5_0", "PS_Layer", "UI blur pixel shader") ||
			!compileShader(clearPixelShader, L"Data\\Shaders\\Menu\\BackgroundBlurComposite.hlsl", "ps_5_0", "PS_Clear", "clear pixel shader"))
			return false;

		auto checkCreate = [&](HRESULT hr, const char* name) -> bool {
			if (FAILED(hr)) {
				logger::error("Failed to create {}", name);
				initializationFailed = true;
				return false;
			}
			return true;
		};

		// Create constant buffers
		D3D11_BUFFER_DESC cbDesc = {};
		cbDesc.Usage = D3D11_USAGE_DEFAULT;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

		cbDesc.ByteWidth = sizeof(BlurConstants);
		if (!checkCreate(device->CreateBuffer(&cbDesc, nullptr, constantBuffer.put()), "blur constant buffer"))
			return false;
		Util::SetResourceName(constantBuffer.get(), "BackgroundBlur::BlurCB");

		// Create sampler state
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (!checkCreate(device->CreateSamplerState(&samplerDesc, samplerState.put()), "blur sampler state"))
			return false;
		Util::SetResourceName(samplerState.get(), "BackgroundBlur::Sampler");

		// Create blend states
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.RenderTarget[0].BlendEnable = TRUE;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (!checkCreate(device->CreateBlendState(&blendDesc, blendState.put()), "blur blend state"))
			return false;
		Util::SetResourceName(blendState.get(), "BackgroundBlur::BlendState");

		// Composite: pre-multiplied alpha (SrcBlend=ONE, DestBlendAlpha=INV_SRC_ALPHA)
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		if (!checkCreate(device->CreateBlendState(&blendDesc, compositeBlendState.put()), "composite blend state"))
			return false;
		Util::SetResourceName(compositeBlendState.get(), "BackgroundBlur::CompositeBlendState");

		// Create scissor-enabled rasterizer state
		D3D11_RASTERIZER_DESC rsDesc = {};
		rsDesc.FillMode = D3D11_FILL_SOLID;
		rsDesc.CullMode = D3D11_CULL_BACK;
		rsDesc.FrontCounterClockwise = FALSE;
		rsDesc.DepthClipEnable = TRUE;
		rsDesc.ScissorEnable = TRUE;
		if (!checkCreate(device->CreateRasterizerState(&rsDesc, scissorRasterizerState.put()), "scissor rasterizer state"))
			return false;
		Util::SetResourceName(scissorRasterizerState.get(), "BackgroundBlur::ScissorRasterizerState");

		initialized = true;
		return true;
	}

	void CreateBlurTextures(UINT width, UINT height, DXGI_FORMAT format)
	{
		std::lock_guard<std::mutex> lock(resourceMutex);

		if (width == textureWidth && height == textureHeight && format == textureFormat && blurTexture1 && blurTexture2) {
			return;
		}

		auto device = globals::d3d::device;
		if (!device) {
			return;
		}

		ReleaseBlurTextures();

		UINT dsWidth = (std::max)(1u, width / DOWNSAMPLE_FACTOR);
		UINT dsHeight = (std::max)(1u, height / DOWNSAMPLE_FACTOR);

		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = dsWidth;
		texDesc.Height = dsHeight;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = format;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		if (!CreateTextureSet(device, texDesc, downsampleTexture, downsampleRTV, downsampleSRV, "downsample") ||
			!CreateTextureSet(device, texDesc, blurTexture1, blurRTV1, blurSRV1, "blur 1") ||
			!CreateTextureSet(device, texDesc, blurTexture2, blurRTV2, blurSRV2, "blur 2")) {
			ReleaseBlurTextures();
			return;
		}

		textureWidth = width;
		textureHeight = height;
		downsampledWidth = dsWidth;
		downsampledHeight = dsHeight;
		textureFormat = format;
	}

	void PerformBlur(const BlurFrame& frame, const BlurConstants& constants)
	{
		auto context = globals::d3d::context;
		D3D11_RECT scissorRect{
			static_cast<LONG>((std::max)(0.0f, std::floor(constants.windowRect[0] - SCISSOR_AA_PADDING))),
			static_cast<LONG>((std::max)(0.0f, std::floor(constants.windowRect[1] - SCISSOR_AA_PADDING))),
			static_cast<LONG>((std::min)(constants.windowParams[1], std::ceil(constants.windowRect[2] + SCISSOR_AA_PADDING))),
			static_cast<LONG>((std::min)(constants.windowParams[2], std::ceil(constants.windowRect[3] + SCISSOR_AA_PADDING)))
		};
		if (scissorRect.left >= scissorRect.right || scissorRect.top >= scissorRect.bottom)
			return;

		CS_GPU_PASS_SELECT(constants.windowParams[3] != 0.0f, "BackgroundBlur::Fullscreen", "BackgroundBlur::Window");

		auto constantBufferPtr = constantBuffer.get();
		auto samplerStatePtr = samplerState.get();
		ID3D11ShaderResourceView* nullSRV = nullptr;
		context->UpdateSubresource(constantBufferPtr, 0, nullptr, &constants, 0, 0);
		context->PSSetConstantBuffers(1, 1, &constantBufferPtr);
		context->PSSetSamplers(0, 1, &samplerStatePtr);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vertexShader.get(), nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		context->RSSetState(scissorRasterizerState.get());

		D3D11_VIEWPORT blurViewport{};
		blurViewport.Width = static_cast<FLOAT>(downsampledWidth);
		blurViewport.Height = static_cast<FLOAT>(downsampledHeight);
		blurViewport.MaxDepth = 1.0f;
		D3D11_RECT blurScissor{ 0, 0, static_cast<LONG>(downsampledWidth), static_cast<LONG>(downsampledHeight) };
		context->RSSetViewports(1, &blurViewport);
		context->RSSetScissorRects(1, &blurScissor);

		auto downsampleRTVPtr = downsampleRTV.get();
		context->OMSetRenderTargets(1, &downsampleRTVPtr, nullptr);
		context->PSSetShader(downsamplePixelShader.get(), nullptr, 0);
		context->PSSetShaderResources(0, 1, &frame.sourceSRV);
		context->Draw(FULLSCREEN_TRIANGLE_VERTICES, 0);
		context->PSSetShaderResources(0, 1, &nullSRV);

		if (frame.uiBuffer.srv) {
			context->OMSetBlendState(compositeBlendState.get(), nullptr, 0xFFFFFFFF);
			context->PSSetShaderResources(0, 1, &frame.uiBuffer.srv);
			context->Draw(FULLSCREEN_TRIANGLE_VERTICES, 0);
			context->PSSetShaderResources(0, 1, &nullSRV);
			context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		}

		auto rtv1Ptr = blurRTV1.get();
		auto downsampleSRVPtr = downsampleSRV.get();
		context->OMSetRenderTargets(1, &rtv1Ptr, nullptr);
		context->PSSetShader(horizontalPixelShader.get(), nullptr, 0);
		context->PSSetShaderResources(0, 1, &downsampleSRVPtr);
		context->Draw(FULLSCREEN_TRIANGLE_VERTICES, 0);
		context->PSSetShaderResources(0, 1, &nullSRV);

		auto rtv2Ptr = blurRTV2.get();
		auto srv1Ptr = blurSRV1.get();
		context->OMSetRenderTargets(1, &rtv2Ptr, nullptr);
		context->PSSetShader(verticalPixelShader.get(), nullptr, 0);
		context->PSSetShaderResources(0, 1, &srv1Ptr);
		context->Draw(FULLSCREEN_TRIANGLE_VERTICES, 0);
		context->PSSetShaderResources(0, 1, &nullSRV);

		D3D11_VIEWPORT compositeViewport{};
		compositeViewport.Width = constants.windowParams[1];
		compositeViewport.Height = constants.windowParams[2];
		compositeViewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &compositeViewport);
		context->RSSetScissorRects(1, &scissorRect);
		context->OMSetRenderTargets(1, &frame.targetRTV, nullptr);
		context->OMSetBlendState(frame.uiOnly ? nullptr : blendState.get(), nullptr, 0xFFFFFFFF);
		context->PSSetShader(frame.uiOnly ? layerPixelShader.get() : compositePixelShader.get(), nullptr, 0);
		auto srv2Ptr = blurSRV2.get();
		context->PSSetShaderResources(0, 1, &srv2Ptr);
		context->Draw(FULLSCREEN_TRIANGLE_VERTICES, 0);
		context->PSSetShaderResources(0, 1, &nullSRV);

		if (frame.uiBuffer.rtv) {
			context->OMSetRenderTargets(1, &frame.uiBuffer.rtv, nullptr);
			context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
			context->PSSetShader(clearPixelShader.get(), nullptr, 0);
			context->Draw(FULLSCREEN_TRIANGLE_VERTICES, 0);
		}

		context->OMSetRenderTargets(1, &frame.imguiRTV, nullptr);
	}

	void RenderWindowBlur(const ImDrawList*, const ImDrawCmd* command)
	{
		const auto& window = *static_cast<const WindowBlur*>(command->UserCallbackData);
		PerformBlur(*window.frame, window.constants);
	}

	void CompositeEditorLayer(ID3D11RenderTargetView* target)
	{
		CS_GPU_PASS("BackgroundBlur::CompositeEditorUI");
		auto context = globals::d3d::context;
		auto srv = editorUISRV.get();
		auto sampler = samplerState.get();
		D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(textureWidth), static_cast<float>(textureHeight), 0.0f, 1.0f };
		D3D11_RECT scissor{ 0, 0, static_cast<LONG>(textureWidth), static_cast<LONG>(textureHeight) };
		context->RSSetState(scissorRasterizerState.get());
		context->RSSetViewports(1, &viewport);
		context->RSSetScissorRects(1, &scissor);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vertexShader.get(), nullptr, 0);
		context->PSSetShader(copyPixelShader.get(), nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->OMSetRenderTargets(1, &target, nullptr);
		context->OMSetBlendState(compositeBlendState.get(), nullptr, 0xFFFFFFFF);
		context->PSSetSamplers(0, 1, &sampler);
		context->PSSetShaderResources(0, 1, &srv);
		context->Draw(FULLSCREEN_TRIANGLE_VERTICES, 0);
		srv = nullptr;
		context->PSSetShaderResources(0, 1, &srv);
	}

	void Cleanup()
	{
		std::lock_guard<std::mutex> lock(resourceMutex);

		vertexShader = nullptr;
		copyPixelShader = nullptr;
		downsamplePixelShader = nullptr;
		horizontalPixelShader = nullptr;
		verticalPixelShader = nullptr;
		compositePixelShader = nullptr;
		clearPixelShader = nullptr;
		layerPixelShader = nullptr;
		constantBuffer = nullptr;
		samplerState = nullptr;
		blendState = nullptr;
		compositeBlendState = nullptr;
		scissorRasterizerState = nullptr;

		ReleaseBlurTextures();
		ReleaseEditorLayer();
		retainedScene = {};
		retainedUI = {};

		cachedSourceSRV = nullptr;
		cachedSourceTexture = nullptr;

		enabled = false;
		initialized = false;
		initializationFailed = false;
	}

	void SetEnabled(bool enable)
	{
		enabled = enable;
	}

	void SetCSEditorActive(bool active)
	{
		csEditorActive = active;
	}

	bool IsCSEditorActive()
	{
		return csEditorActive;
	}

	bool RenderDrawData(ImDrawData* drawData)
	{
		if (!enabled || globals::game::isVR || !drawData || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
			return false;
		}

		if (!initialized || initializationFailed) {
			return false;
		}

		auto device = globals::d3d::device;
		auto context = globals::d3d::context;
		if (!device || !context) {
			return false;
		}

		// Check if upscaling with D3D12 swap chain is active
		auto& upscaling = globals::features::upscaling;
		bool useUpscalingBackbuffer = upscaling.d3d12SwapChainActive;

		auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
		bool hdrActive = hdr &&
		                 hdr->settings.enableHDR && hdr->hdrDataCB && hdr->outputTexture &&
		                 hdr->hdrTexture && hdr->hdrTexture->resource && hdr->hdrTexture->srv && hdr->hdrTexture->rtv;

		if (!IsStartupMenuBlurSourceReady())
			return false;

		winrt::com_ptr<ID3D11Texture2D> currentTexture;
		winrt::com_ptr<ID3D11RenderTargetView> currentRTV;
		ID3D11ShaderResourceView* sourceSRV = nullptr;  // Non-owning; lifetime managed elsewhere
		UIBufferViews uiBuffer;

		if (hdrActive) {
			// ApplyHDR composites the remaining UI over the blurred scene.
			currentTexture = hdr->hdrTexture->resource;
			sourceSRV = hdr->hdrTexture->srv.get();
			currentRTV = hdr->hdrTexture->rtv;

			uiBuffer = GetHDRUIBufferViews(*hdr, upscaling);
		} else if (useUpscalingBackbuffer) {
			// When D3D12 swap chain is active, get all resources in one call
			auto res = upscaling.GetBlurResources();
			if (!res.backbufferTex || !res.backbufferRTV || !res.backbufferSRV) {
				return false;
			}
			currentTexture.copy_from(res.backbufferTex);
			currentRTV.copy_from(res.backbufferRTV);
			sourceSRV = res.backbufferSRV;

			// D3D12 HDR/FG can route vanilla UI into a separate buffer.
			if (ShouldUseD3D12UIBufferForBlur())
				uiBuffer = GetD3D12UIBufferViews(res);
		} else {
			// Normal path: get current render target
			ID3D11RenderTargetView* rawRTV = nullptr;
			context->OMGetRenderTargets(1, &rawRTV, nullptr);
			if (!rawRTV) {
				return false;
			}
			currentRTV.attach(rawRTV);  // Takes ownership of the AddRef from OMGetRenderTargets

			// Get render target texture
			winrt::com_ptr<ID3D11Resource> currentRT;
			currentRTV->GetResource(currentRT.put());

			winrt::com_ptr<ID3D11Texture2D> tex;
			if (FAILED(currentRT->QueryInterface(IID_PPV_ARGS(tex.put()))) || !tex) {
				return false;
			}
			currentTexture = tex;

			// Cache SRV for non-upscaling path (avoids CreateShaderResourceView every frame)
			if (cachedSourceTexture != currentTexture.get()) {
				cachedSourceSRV = nullptr;
				HRESULT hr = device->CreateShaderResourceView(currentTexture.get(), nullptr, cachedSourceSRV.put());
				if (FAILED(hr)) {
					logger::error("Failed to create cached source SRV for blur");
					return false;
				}
				Util::SetResourceName(cachedSourceSRV.get(), "BackgroundBlur::Source SRV");
				cachedSourceTexture = currentTexture.get();
			}
			sourceSRV = cachedSourceSRV.get();
		}

		D3D11_TEXTURE2D_DESC texDesc;
		currentTexture->GetDesc(&texDesc);

		// Create blur textures if needed (check format too for HDR toggle)
		const auto blurFormat = csEditorActive ? DXGI_FORMAT_R16G16B16A16_FLOAT : texDesc.Format;
		if (textureWidth != texDesc.Width || textureHeight != texDesc.Height || textureFormat != blurFormat) {
			CreateBlurTextures(texDesc.Width, texDesc.Height, blurFormat);
		}

		std::lock_guard<std::mutex> lock(resourceMutex);
		if (!downsampleRTV || !blurRTV1 || !blurRTV2)
			return false;

		winrt::com_ptr<ID3D11RenderTargetView> imguiRTV;
		context->OMGetRenderTargets(1, imguiRTV.put(), nullptr);
		if (!imguiRTV)
			return false;

		const BlurFrame frame{ sourceSRV, currentRTV.get(), imguiRTV.get(), uiBuffer };
		BlurConstants constants{
			{ static_cast<float>(downsampledWidth), static_cast<float>(downsampledHeight), 1.0f / downsampledWidth, 1.0f / downsampledHeight },
			{ 0.0f, 0.0f, static_cast<float>(texDesc.Width), static_cast<float>(texDesc.Height) },
			{ 0.0f, static_cast<float>(texDesc.Width), static_cast<float>(texDesc.Height), csEditorActive ? 1.0f : 0.0f }
		};

		auto windows = GatherWindows(drawData, frame, constants);
		if (windows.empty() && !csEditorActive)
			return false;
		const bool useEditorLayer = csEditorActive && !windows.empty();
		if (useEditorLayer && !CreateEditorLayer(texDesc.Width, texDesc.Height))
			return false;
		if (!csEditorActive)
			ReleaseEditorLayer();
		const BlurFrame editorFrame{ editorUISRV.get(), editorUIRTV.get(), editorUIRTV.get(), {}, true };
		if (!PreserveUIBuffer(uiBuffer))
			return false;

		Util::FullscreenPassScope restoreState(context);
		UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_RECT savedScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		context->RSGetScissorRects(&scissorCount, savedScissors);
		const SKSE::stl::scope_exit restoreScissors([&]() noexcept { context->RSSetScissorRects(scissorCount, savedScissors); });

		if (hdrActive) {
			hdr->SnapshotCleanScene();
			if (!hdr->IsCleanSceneCaptureFresh() || !CopyCompatible(currentTexture.get(), hdr->cleanSceneCapture->resource.get()))
				return false;
			retainedScene = { currentTexture, hdr->cleanSceneCapture->resource, hdr->sceneGeneration, true };
		}
		retainedUI.dirty = retainedUI.source != nullptr;

		if (csEditorActive)
			PerformBlur(frame, constants);
		if (useEditorLayer) {
			const float clear[4]{};
			context->ClearRenderTargetView(editorUIRTV.get(), clear);
			auto target = editorUIRTV.get();
			context->OMSetRenderTargets(1, &target, nullptr);
		}
		for (auto& window : windows) {
			if (useEditorLayer)
				window.frame = &editorFrame;
			ImDrawCmd reset;
			reset.UserCallback = ImDrawCallback_ResetRenderState;
			ImDrawCmd blur;
			blur.UserCallback = RenderWindowBlur;
			blur.UserCallbackData = &window;
			auto& commands = window.drawList->CmdBuffer;
			commands.insert(commands.begin(), reset);
			commands.insert(commands.begin(), blur);
		}

		const SKSE::stl::scope_exit removeCallbacks([&]() noexcept {
			for (auto& window : windows) {
				auto& commands = window.drawList->CmdBuffer;
				commands.erase(commands.begin(), commands.begin() + 2);
			}
		});
		ImGui_ImplDX11_RenderDrawData(drawData);
		if (useEditorLayer)
			CompositeEditorLayer(imguiRTV.get());
		return true;
	}

}  // namespace BackgroundBlur
