#include "VRSubmitUpscaling.h"

#include "Deferred.h"
#include "Features/HDRDisplay.h"
#include "Features/Upscaling.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <detours/detours.h>

namespace
{
	struct ContextScope
	{
		ID3D11DeviceContext1* context;
		winrt::com_ptr<ID3DDeviceContextState> previous;
		ContextScope(ID3D11DeviceContext1* ctx, ID3DDeviceContextState* isolated) : context(ctx)
		{
			context->SwapDeviceContextState(isolated, previous.put());
			context->ClearState();
		}
		~ContextScope()
		{
			context->ClearState();
			context->SwapDeviceContextState(previous.get(), nullptr);
		}
		ContextScope(const ContextScope&) = delete;
		ContextScope& operator=(const ContextScope&) = delete;
	};

	struct SubmitScope
	{
		bool& entered;
		explicit SubmitScope(bool& value) : entered(value) { entered = true; }
		~SubmitScope() { entered = false; }
	};

	struct ColorConstants
	{
		uint32_t width, height, offset, conversion;
	};
}

std::unique_ptr<Texture2D> VRSubmitUpscaling::MakeTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& name)
{
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
	desc.Format = format;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	auto texture = std::make_unique<Texture2D>(desc, name.c_str());
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = format;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	texture->CreateSRV(srv);
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = format;
	uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	texture->CreateUAV(uav);
	return texture;
}

void VRSubmitUpscaling::InstallRenderTargetSizeHook()
{
	if (active || !globals::game::isVR)
		return;
	auto& upscaling = globals::features::upscaling;
	if (!upscaling.ShouldEngagePerfMode())
		return;
	auto* openVR = RE::BSOpenVR::GetSingleton();
	auto* compositor = openVR ? RE::BSOpenVR::GetIVRCompositor() : nullptr;
	if (!openVR || !openVR->vrSystem || !compositor || !globals::d3d::device || !globals::d3d::context)
		return;

	ResolutionPlan candidate;
	candidate.method = uint32_t(upscaling.GetUpscaleMethod());
	openVR->vrSystem->GetRecommendedRenderTargetSize(&candidate.outputWidth, &candidate.outputHeight);
	constexpr uint32_t maxDimension = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	if (!candidate.outputWidth || !candidate.outputHeight || candidate.outputWidth > maxDimension / 2 || candidate.outputHeight > maxDimension)
		return;
	const float explicitScale = upscaling.settings.vrRenderScale;
	if (!std::isfinite(explicitScale) || explicitScale < 0 || upscaling.settings.qualityMode > 4)
		return;
	candidate.explicitScale = explicitScale > 0;
	candidate.qualityMode = upscaling.settings.qualityMode;
	float ratio = Upscaling::GetQualityModeRatio(candidate.qualityMode);
	if (candidate.explicitScale) {
		ratio = 1.0f / std::clamp(explicitScale, Upscaling::kVRRenderScaleMin, Upscaling::kVRRenderScaleMax);
		float difference = FLT_MAX;
		for (uint32_t quality = 1; quality <= 4; ++quality) {
			const float delta = std::abs(Upscaling::GetQualityModeRatio(quality) - ratio);
			if (delta < difference) {
				difference = delta;
				candidate.qualityMode = quality;
			}
		}
	}
	candidate.renderWidth = std::max(2u, uint32_t(candidate.outputWidth / ratio)) & ~1u;
	candidate.renderHeight = std::max(2u, uint32_t(candidate.outputHeight / ratio)) & ~1u;
	if (candidate.explicitScale && upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS)
		upscaling.streamline.ClampToDLSSRenderRange(candidate.qualityMode, candidate.outputWidth, candidate.outputHeight,
			candidate.renderWidth, candidate.renderHeight);
	if (!candidate.renderWidth || !candidate.renderHeight || candidate.renderWidth >= candidate.outputWidth ||
		candidate.renderHeight >= candidate.outputHeight || (candidate.renderWidth & 1) || (candidate.renderHeight & 1))
		return;

	winrt::com_ptr<ID3D11Device1> device;
	if (FAILED(globals::d3d::device->QueryInterface(device.put())) ||
		FAILED(globals::d3d::context->QueryInterface(context.put())))
		return;
	const auto level = globals::d3d::device->GetFeatureLevel();
	if (FAILED(device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
			__uuidof(ID3D11Device), nullptr, isolatedState.put())))
		return;
	Util::SetResourceName(isolatedState.get(), "Upscaling::SubmitContextState");

	auto** vtable = *reinterpret_cast<void***>(compositor);
	WaitGetPosesHook::func = reinterpret_cast<decltype(WaitGetPosesHook::func)>(vtable[2]);
	SubmitHook::func = reinterpret_cast<decltype(SubmitHook::func)>(vtable[5]);
	if (DetourTransactionBegin() != NO_ERROR)
		return;
	if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
		DetourAttach(reinterpret_cast<PVOID*>(&WaitGetPosesHook::func), reinterpret_cast<PVOID>(WaitGetPosesHook::thunk)) != NO_ERROR ||
		DetourAttach(reinterpret_cast<PVOID*>(&SubmitHook::func), reinterpret_cast<PVOID>(SubmitHook::thunk)) != NO_ERROR) {
		DetourTransactionAbort();
		return;
	}
	if (DetourTransactionCommit() != NO_ERROR)
		return;
	plan = candidate;
	upscaling.bootSnapshot.LatchIfNeeded(upscaling.settings);
	stl::write_vfunc<0x12, RenderTargetSizeHook>(RE::VTABLE_BSOpenVR[0]);
	active = true;
	stl::write_vfunc<0x2A, MenuUIHook>(RE::VTABLE_BSShaderAccumulator[0]);
	stl::detour_thunk<MenuViewportHook>(REL::RelocationID(75455, 77240));
	stl::detour_vfunc<12, MenuDrawIndexed>(globals::d3d::context);
	stl::detour_vfunc<13, MenuDraw>(globals::d3d::context);
	stl::detour_vfunc<20, MenuDrawIndexedInstanced>(globals::d3d::context);
	stl::detour_vfunc<21, MenuDrawInstanced>(globals::d3d::context);

	logger::info("[VRSubmit] Latched per-eye render {}x{} -> output {}x{}", plan.renderWidth, plan.renderHeight, plan.outputWidth, plan.outputHeight);
}

void VRSubmitUpscaling::RenderTargetSizeHook::thunk(RE::BSOpenVR* self, uint32_t* width, uint32_t* height)
{
	func(self, width, height);
	const auto& owner = globals::features::upscaling.vrSubmit;
	*width = owner.plan.renderWidth;
	*height = owner.plan.renderHeight;
}

vr::EVRCompositorError VRSubmitUpscaling::WaitGetPosesHook::thunk(vr::IVRCompositor* self,
	vr::TrackedDevicePose_t* renderPoses, uint32_t renderCount, vr::TrackedDevicePose_t* gamePoses, uint32_t gameCount)
{
	auto result = func(self, renderPoses, renderCount, gamePoses, gameCount);
	globals::features::upscaling.vrSubmit.cycle.fetch_add(1);
	return result;
}

void VRSubmitUpscaling::Invalidate()
{
	captured = attempted = pairReady = false;
	lastSuccessCycle = UINT64_MAX;
	submittedSource = nullptr;
}

void VRSubmitUpscaling::SetupResources()
{
	if (!active)
		return;
	std::lock_guard lock(mutex);
	Invalidate();
	ClearFoveationResources();
	for (auto& eye : eyes)
		eye = {};
	sourceCopy = nullptr;
	sourceView = nullptr;
	encodeBuffer.reset();
	colorBuffer.reset();
	renderThread = 0;
	menuReady = false;
	menuUnavailable = false;
	menuSceneUnavailable = capturedMenuScene = lastSuccessMenuScene = false;
	menuSceneReconstructed = false;
	menuSceneCycle = UINT64_MAX;
	capturedMapScene = lastSuccessMapScene = false;
	lastCaptureTime = {};
	menuCycle = UINT64_MAX;
	menuResolvedCycle = UINT64_MAX;
	menuResolvedSource = nullptr;
	menuColor.reset();
	menuDepth.reset();
	menuReference.reset();
	menuResolved.reset();
	menuMismatch.reset();
	menuSceneReference.reset();
	menuSceneMismatch.reset();
	menuUILayer.reset();
	menuUILayerCycle = UINT64_MAX;
	menuLayerDraws = menuLayerRejected = 0;
	menuLayerRejection.clear();
	menuScene.reset();
	SetupMenuResources();
	failed = false;
	SetStatus("Waiting for world inputs");
}

void VRSubmitUpscaling::ClearShaderCache()
{
	shaderResetPending = true;
}

std::string VRSubmitUpscaling::GetStatus() const
{
	std::lock_guard lock(statusMutex);
	return std::format("{} ({} stereo pairs reconstructed)", status, reconstructedPairs.load());
}

void VRSubmitUpscaling::SetStatus(std::string_view message)
{
	std::lock_guard lock(statusMutex);
	if (status != message)
		status = message;
}

void VRSubmitUpscaling::Fail(std::string_view reason)
{
	Invalidate();
	if (!failed.exchange(true)) {
		SetStatus(std::format("Upscaling unavailable: {}", reason));
		logger::warn("[VRSubmit] Using original submissions until resource reset: {}", reason);
	}
}

bool VRSubmitUpscaling::CanJitter() const
{
	return active && !failed && globals::state && !IsMenuFrame();
}

bool VRSubmitUpscaling::IsMenuFrame() const
{
	return active && globals::state &&
	       (globals::state->IsPausedOrMenuOpen(globals::game::ui) || globals::state->IsFullScreenMenuOpen());
}

bool VRSubmitUpscaling::WantsNativeMenuUI() const
{
	auto* ui = globals::game::ui;
	return IsMenuFrame() && ui &&
	       (ui->IsMenuOpen(RE::JournalMenu::MENU_NAME) || ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) ||
			   ui->IsMenuOpen(RE::MapMenu::MENU_NAME) || ui->IsMenuOpen(RE::MagicMenu::MENU_NAME));
}

bool VRSubmitUpscaling::CanCaptureMenuScene()
{
	if (menuSceneUnavailable)
		return false;
	if (globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.settings.enableHDR) {
		menuSceneFallback = "HDR output is enabled";
		return false;
	}
	if (menuUnavailable || !WantsNativeMenuUI() ||
		globals::state->IsMainOrLoadingMenuOpen(globals::game::ui) || globals::state->isStatsMenuOpen) {
		menuSceneFallback = "unsupported menu state";
		return false;
	}
	if (!menuReady || menuCycle != cycle || menuUILayerCycle != cycle || !menuLayerDraws || menuLayerRejected) {
		menuSceneFallback = menuLayerRejected ? "UI capture incomplete: " + menuLayerRejection : "no separate UI layer this frame";
		return false;
	}
	if (
		!menuColor || !menuDepth || !menuReference || !menuResolved || !menuMismatch || !menuScene || !menuSceneReference || !menuSceneMismatch ||
		!menuCopyVS || !menuCopyPS || !menuSampler || !menuValidateCS || !menuResolveCS) {
		menuSceneFallback = "menu resources unavailable";
		return false;
	}
	const auto& source = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
	D3D11_TEXTURE2D_DESC desc{};
	if (!source.texture || !source.SRV || !source.RTV) {
		menuSceneFallback = "world framebuffer unavailable";
		return false;
	}
	source.texture->GetDesc(&desc);
	if (desc.Width != plan.renderWidth * 2 || desc.Height != plan.renderHeight ||
		desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
		menuSceneFallback = std::format("unsupported framebuffer: {}x{}, format {}", desc.Width, desc.Height, uint32_t(desc.Format));
		return false;
	}
	menuSceneFallback = "waiting for post-processed world";
	return true;
}

bool VRSubmitUpscaling::ShouldApplyMenuTAA() const
{
	return IsMenuFrame() && !(captured && capturedMenuScene && captureCycle == cycle && !failed);
}

bool VRSubmitUpscaling::ReconstructMenuScene(ID3D11Texture2D* source)
{
	if (!captured || !capturedMenuScene || captureCycle != cycle || attempted || menuSceneUnavailable)
		return false;
	attempted = true;
	try {
		CS_GPU_PASS("Upscaling::MenuSceneReconstruct");
		if (ReconstructPair(source, vr::ColorSpace_Gamma)) {
			ContextScope scope(context.get(), isolatedState.get());
			for (uint32_t eye = 0; eye < 2; ++eye)
				context->CopySubresourceRegion(menuScene->resource.get(), 0, eye * plan.outputWidth, 0, 0,
					eyes[eye].submit->resource.get(), 0, nullptr);
			menuSceneReconstructed = true;
			menuSceneCycle = captureCycle;
			return true;
		}
	} catch (const std::exception& error) {
		logger::warn("[VRSubmit] Menu scene reconstruction unavailable: {}", error.what());
	} catch (...) {
		logger::warn("[VRSubmit] Menu scene reconstruction failed");
	}
	dispatching = false;
	menuSceneUnavailable = true;
	menuSceneFallback = "DLSS/FSR dispatch failed; reset required";
	lastSuccessCycle = UINT64_MAX;
	return false;
}

void VRSubmitUpscaling::LogMenuDiagnostics(uint32_t stage, uint32_t postProcessingTarget)
{
	if (globals::features::upscaling.bootSnapshot.Boot(&Upscaling::Settings::streamlineLogLevel) != 2 ||
		stage >= std::size(menuDiagnosticTimes) || !WantsNativeMenuUI())
		return;
	const auto now = std::chrono::steady_clock::now();
	if (now - menuDiagnosticTimes[stage] < std::chrono::seconds(10))
		return;
	menuDiagnosticTimes[stage] = now;
	static constexpr const char* stages[] = { "capture", "post-processing", "UI", "submit" };
	logger::info("[VRSubmitMenu] stage={} cycle={} captureCycle={} worldRendered={} captured={} menuInputs={} attempted={} reconstructed={} menuReady={} fallback='{}' postTarget={} uiLayerDraws={} uiLayerRejected={}",
		stages[stage], cycle.load(), captureCycle, globals::state->worldRenderedThisFrame, captured, capturedMenuScene,
		attempted, menuSceneReconstructed, menuReady, menuSceneFallback, postProcessingTarget, menuLayerDraws, menuLayerRejected);
	const auto describeTexture = [](ID3D11Texture2D* texture) {
		if (!texture)
			return std::string("none");
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		return std::format("{} {}x{} fmt={} bind={} samples={} slices={} mips={}", static_cast<void*>(texture),
			desc.Width, desc.Height, uint32_t(desc.Format), desc.BindFlags, desc.SampleDesc.Count, desc.ArraySize, desc.MipLevels);
	};
	const auto describeView = [&](ID3D11View* view) {
		if (!view)
			return std::string("none");
		winrt::com_ptr<ID3D11Resource> resource;
		winrt::com_ptr<ID3D11Texture2D> texture;
		view->GetResource(resource.put());
		if (resource)
			resource->QueryInterface(texture.put());
		return describeTexture(texture.get());
	};
	auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	for (const auto index : { RE::RENDER_TARGETS::kMAIN, RE::RENDER_TARGETS::kFRAMEBUFFER,
			 RE::RENDER_TARGETS::kMENUBG, RE::RENDER_TARGETS::kTOTAL }) {
		const auto& target = targets[index];
		logger::info("[VRSubmitMenu] stage={} target={} texture=[{}] SRV=[{}] RTV=[{}]",
			stages[stage], uint32_t(index), describeTexture(target.texture), describeView(target.SRV), describeView(target.RTV));
	}
	if (globals::game::shadowState) {
		const auto& shadow = globals::game::shadowState->GetVRRuntimeData();
		logger::info("[VRSubmitMenu] stage={} shadowTarget={} depth={} clearMode={} viewport={}x{}",
			stages[stage], uint32_t(shadow.renderTargets[0]), shadow.depthStencil, uint32_t(shadow.setRenderTargetMode[0]),
			shadow.viewPort.Width, shadow.viewPort.Height);
	}
	winrt::com_ptr<ID3D11RenderTargetView> boundTarget;
	context->OMGetRenderTargets(1, boundTarget.put(), nullptr);
	logger::info("[VRSubmitMenu] stage={} boundRTV=[{}]", stages[stage], describeView(boundTarget.get()));
	logger::info("[VRSubmitMenu] stage={} lastUIRejection='{}'", stages[stage], menuLayerRejection);
	spdlog::default_logger()->flush();
}

void VRSubmitUpscaling::ReconstructMenuBackground(uint32_t postProcessingTarget)
{
	if (!active || GetCurrentThreadId() != renderThread)
		return;
	std::unique_lock lock(mutex, std::try_to_lock);
	if (!lock)
		return;
	LogMenuDiagnostics(1, postProcessingTarget);
	if (failed || shaderResetPending || !captured || !capturedMenuScene || captureCycle != cycle || attempted)
		return;
	if (postProcessingTarget != uint32_t(RE::RENDER_TARGETS::kTOTAL)) {
		menuSceneFallback = "unexpected post-processing output target";
		return;
	}
	auto& source = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
	D3D11_TEXTURE2D_DESC desc{};
	if (!source.texture || !source.SRV || !source.RTV)
		return;
	source.texture->GetDesc(&desc);
	if (desc.Width != plan.renderWidth * 2 || desc.Height != plan.renderHeight ||
		desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
		return;
	if (ReconstructMenuScene(source.texture)) {
		CopyMenuColor(menuScene->srv.get(), source.RTV, plan.renderWidth * 2, plan.renderHeight);
		ContextScope scope(context.get(), isolatedState.get());
		context->CopyResource(menuSceneReference->resource.get(), source.texture);
	}
}

void VRSubmitUpscaling::SetupMenuResources()
{
	try {
		if (!menuColor || !menuDepth) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = plan.outputWidth * 2;
			desc.Height = plan.outputHeight;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			auto color = std::make_unique<Texture2D>(desc, "Upscaling::NativeMenuColor");
			D3D11_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = desc.Format;
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			color->CreateRTV(rtv);
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			color->CreateSRV(srv);
			desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			auto depth = std::make_unique<Texture2D>(desc, "Upscaling::NativeMenuDepth");
			D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
			dsv.Format = desc.Format;
			dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			depth->CreateDSV(dsv);
			menuColor = std::move(color);
			menuDepth = std::move(depth);
		}
		if (!menuSampler) {
			D3D11_SAMPLER_DESC desc{};
			desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.MaxLOD = D3D11_FLOAT32_MAX;
			winrt::check_hresult(globals::d3d::device->CreateSamplerState(&desc, menuSampler.put()));
			Util::SetResourceName(menuSampler.get(), "Upscaling::NativeMenuSampler");
		}
		if (!menuUILayer) {
			D3D11_TEXTURE2D_DESC desc{};
			menuColor->resource->GetDesc(&desc);
			menuUILayer = std::make_unique<Texture2D>(desc, "Upscaling::MenuUILayer");
			D3D11_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = desc.Format;
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			menuUILayer->CreateRTV(rtv);
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			menuUILayer->CreateSRV(srv);
		}
		for (uint32_t premultiplied = 0; premultiplied < 2; ++premultiplied) {
			if (menuLayerBlend[premultiplied])
				continue;
			D3D11_BLEND_DESC desc{};
			auto& blend = desc.RenderTarget[0];
			blend.BlendEnable = true;
			blend.SrcBlend = premultiplied ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA;
			blend.DestBlend = blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			blend.BlendOp = blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blend.SrcBlendAlpha = D3D11_BLEND_ONE;
			blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			winrt::check_hresult(globals::d3d::device->CreateBlendState(&desc, menuLayerBlend[premultiplied].put()));
			Util::SetResourceName(menuLayerBlend[premultiplied].get(), "Upscaling::MenuLayerBlend%u", premultiplied);
		}
		menuCopyVS.Get(L"Data/Shaders/Upscaling/UpscaleVS.hlsl", { { "VSHADER", "" } }, "vs_5_0", "main", "Upscaling::NativeMenuCopy VS");
		menuCopyPS.Get(L"Data/Shaders/Upscaling/PerfMode/MenuBGBlitPS.hlsl", { { "PSHADER", "" } }, "ps_5_0", "main", "Upscaling::NativeMenuCopy PS");
		if (!menuReference)
			menuReference = MakeTexture(plan.renderWidth * 2, plan.renderHeight, DXGI_FORMAT_R8G8B8A8_UNORM, "Upscaling::MenuReference");
		if (!menuResolved)
			menuResolved = MakeTexture(plan.outputWidth * 2, plan.outputHeight, DXGI_FORMAT_R8G8B8A8_UNORM, "Upscaling::MenuResolved");
		if (!menuMismatch)
			menuMismatch = MakeTexture(1, 1, DXGI_FORMAT_R32_UINT, "Upscaling::MenuMismatch");
		if (!menuSceneReference)
			menuSceneReference = MakeTexture(plan.renderWidth * 2, plan.renderHeight, DXGI_FORMAT_R8G8B8A8_UNORM, "Upscaling::MenuSceneReference");
		if (!menuSceneMismatch)
			menuSceneMismatch = MakeTexture(1, 1, DXGI_FORMAT_R32_UINT, "Upscaling::MenuSceneMismatch");
		if (!menuScene)
			menuScene = MakeTexture(plan.outputWidth * 2, plan.outputHeight, DXGI_FORMAT_R8G8B8A8_UNORM, "Upscaling::MenuScene");
		menuValidateCS.Get(L"Data/Shaders/Upscaling/NativeMenuCS.hlsl", { { "VALIDATE_MENU", "" } }, "cs_5_0", "main", "Upscaling::MenuValidate CS");
		menuResolveCS.Get(L"Data/Shaders/Upscaling/NativeMenuCS.hlsl", {}, "cs_5_0", "main", "Upscaling::MenuResolve CS");
	} catch (const std::exception& error) {
		menuColor.reset();
		menuDepth.reset();
		logger::warn("[VRSubmit] Native menu resources unavailable: {}", error.what());
	}
}

void VRSubmitUpscaling::CopyMenuColor(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* destination,
	uint32_t width, uint32_t height)
{
	CS_GPU_PASS("Upscaling::NativeMenuCopy");
	ContextScope scope(context.get(), isolatedState.get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(menuCopyVS.get(), nullptr, 0);
	context->PSSetShader(menuCopyPS.get(), nullptr, 0);
	context->PSSetShaderResources(0, 1, &source);
	auto sampler = menuSampler.get();
	context->PSSetSamplers(0, 1, &sampler);
	context->OMSetRenderTargets(1, &destination, nullptr);
	const D3D11_VIEWPORT viewport{ 0, 0, float(width), float(height), 0, 1 };
	context->RSSetViewports(1, &viewport);
	context->Draw(3, 0);
}

bool VRSubmitUpscaling::DrawNativeMenuUI(RE::BSGraphics::BSShaderAccumulator* accumulator, uint32_t flags)
{
	if (GetCurrentThreadId() != renderThread || menuDrawing || !WantsNativeMenuUI() || accumulator->GetRuntimeData().renderMode != 24)
		return false;
	std::unique_lock lock(mutex, std::try_to_lock);
	if (!lock || failed || menuUnavailable || shaderResetPending || !globals::game::shadowState ||
		!menuColor || !menuDepth || !menuCopyVS || !menuCopyPS || !menuSampler ||
		!menuReference || !menuResolved || !menuMismatch || !menuValidateCS || !menuResolveCS ||
		!menuSceneReference || !menuSceneMismatch)
		return false;
	auto& shadow = globals::game::shadowState->GetVRRuntimeData();
	LogMenuDiagnostics(2);
	if (shadow.renderTargets[0] != RE::RENDER_TARGETS::kMENUBG ||
		shadow.depthStencil != uint32_t(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN))
		return false;
	for (uint32_t index = 1; index < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
		if (shadow.renderTargets[index] != RE::RENDER_TARGETS::kNONE)
			return false;
	}
	auto& target = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMENUBG];
	auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	D3D11_TEXTURE2D_DESC desc{};
	if (!target.texture || !target.SRV || !target.RTV)
		return false;
	target.texture->GetDesc(&desc);
	if (desc.Width != plan.renderWidth * 2 || desc.Height != plan.renderHeight ||
		desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
		return false;
	CS_GPU_PASS("Upscaling::NativeMenuUI");
	menuReady = false;
	if (menuUILayer && menuUILayerCycle != cycle) {
		const float transparent[4]{};
		context->ClearRenderTargetView(menuUILayer->rtv.get(), transparent);
		menuUILayerCycle = cycle.load();
		menuLayerDraws = menuLayerRejected = 0;
		menuLayerRejection.clear();
	}
	// A pending color clear must not seed the native target with the previous menu image.
	if (shadow.setRenderTargetMode[0] == RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR) {
		const float transparent[4]{};
		context->ClearRenderTargetView(menuColor->rtv.get(), transparent);
	} else {
		CopyMenuColor(target.SRV, menuColor->rtv.get(), plan.outputWidth * 2, plan.outputHeight);
	}
	{
		ContextScope scope(context.get(), isolatedState.get());
		context->CopyResource(menuResolved->resource.get(), menuColor->resource.get());
	}
	context->ClearDepthStencilView(menuDepth->dsv.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
	const auto savedTarget = target;
	const auto savedDepth = depth;
	const auto savedViewport = shadow.viewPort;
	D3D11_RECT savedScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	context->RSGetScissorRects(&scissorCount, savedScissors);
	const auto restore = [&]() {
		menuDrawing = false;
		target = savedTarget;
		depth = savedDepth;
		shadow.viewPort = savedViewport;
		context->RSSetScissorRects(scissorCount, savedScissors);
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET,
			RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
	};
	target.texture = menuColor->resource.get();
	target.textureCopy = savedTarget.textureCopy ? menuResolved->resource.get() : nullptr;
	target.SRVCopy = savedTarget.SRVCopy ? menuResolved->srv.get() : nullptr;
	target.RTV = menuColor->rtv.get();
	target.SRV = menuColor->srv.get();
	target.UAV = nullptr;
	depth.texture = menuDepth->resource.get();
	for (auto& view : depth.views)
		if (view)
			view = menuDepth->dsv.get();
	for (auto& view : depth.readOnlyViews)
		if (view)
			view = menuDepth->dsv.get();
	shadow.viewPort = { 0, 0, float(plan.outputWidth * 2), float(plan.outputHeight), 0, 1 };
	D3D11_RECT scaledScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	for (UINT index = 0; index < scissorCount; ++index) {
		const auto& rect = savedScissors[index];
		scaledScissors[index] = { LONG(double(rect.left) * plan.outputWidth / plan.renderWidth),
			LONG(double(rect.top) * plan.outputHeight / plan.renderHeight),
			LONG(double(rect.right) * plan.outputWidth / plan.renderWidth),
			LONG(double(rect.bottom) * plan.outputHeight / plan.renderHeight) };
	}
	context->RSSetScissorRects(scissorCount, scaledScissors);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET,
		RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
	try {
		menuDrawing = true;
		MenuUIHook::func(accumulator, flags);
	} catch (...) {
		restore();
		throw;
	}
	restore();
	CopyMenuColor(menuColor->srv.get(), savedTarget.RTV, plan.renderWidth * 2, plan.renderHeight);
	{
		ContextScope scope(context.get(), isolatedState.get());
		context->CopyResource(menuReference->resource.get(), savedTarget.texture);
	}
	menuCycle = cycle.load();
	menuResolvedCycle = UINT64_MAX;
	menuReady = true;
	return true;
}

bool VRSubmitUpscaling::CaptureMenuDraw(ID3D11DeviceContext* drawContext, const std::function<void()>& draw)
{
	if (GetCurrentThreadId() != renderThread || drawContext != globals::d3d::context ||
		!menuDrawing || !menuUILayer || !menuLayerBlend[0] || !menuLayerBlend[1] || menuUILayerCycle != cycle)
		return false;
	winrt::com_ptr<ID3D11ShaderResourceView> source;
	drawContext->PSGetShaderResources(0, 1, source.put());
	if (!source)
		return false;
	winrt::com_ptr<ID3D11Resource> sourceResource;
	source->GetResource(sourceResource.put());
	auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	bool menuSource = false;
	for (const auto index : { RE::RENDER_TARGETS::kPROJECTEDMENU, RE::RENDER_TARGETS::kHUDMENU,
			 RE::RENDER_TARGETS::kWORLDUI0, RE::RENDER_TARGETS::kWORLDUI1, RE::RENDER_TARGETS::kWORLDUI2,
			 RE::RENDER_TARGETS::kWORLDUI3, RE::RENDER_TARGETS::kWORLDUI4, RE::RENDER_TARGETS::kWORLDUI5, RE::RENDER_TARGETS::kWORLDUI6 }) {
		const auto& target = targets[index];
		menuSource |= source.get() == target.SRV || source.get() == target.SRVCopy ||
		              sourceResource.get() == target.texture || sourceResource.get() == target.textureCopy;
	}
	if (!menuSource)
		return false;
	winrt::com_ptr<ID3D11RenderTargetView> target;
	winrt::com_ptr<ID3D11DepthStencilView> depth;
	ID3D11RenderTargetView* boundTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
	drawContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, boundTargets, depth.put());
	target.attach(boundTargets[0]);
	bool additionalTarget = false;
	for (UINT index = 1; index < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
		if (boundTargets[index]) {
			additionalTarget = true;
			boundTargets[index]->Release();
		}
	}
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	UINT stencilRef = 0;
	drawContext->OMGetDepthStencilState(depthState.put(), &stencilRef);
	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	if (depthState)
		depthState->GetDesc(&depthDesc);
	winrt::com_ptr<ID3D11BlendState> blendState;
	float blendFactor[4]{};
	UINT sampleMask = 0;
	drawContext->OMGetBlendState(blendState.put(), blendFactor, &sampleMask);
	D3D11_BLEND_DESC blendDesc{};
	if (blendState)
		blendState->GetDesc(&blendDesc);
	const auto& blend = blendDesc.RenderTarget[0];
	if (additionalTarget || target.get() != menuColor->rtv.get() || !blendState || blendDesc.AlphaToCoverageEnable ||
		!blend.BlendEnable || blend.BlendOp != D3D11_BLEND_OP_ADD || blend.DestBlend != D3D11_BLEND_INV_SRC_ALPHA ||
		(blend.SrcBlend != D3D11_BLEND_SRC_ALPHA && blend.SrcBlend != D3D11_BLEND_ONE) ||
		(blend.RenderTargetWriteMask & 7) != 7 ||
		(depth && (!depthState || (depthDesc.DepthEnable && depthDesc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO) ||
					  (depthDesc.StencilEnable && depthDesc.StencilWriteMask != 0)))) {
		++menuLayerRejected;
		if (menuLayerRejected == 1)
			menuLayerRejection = std::format("target={} MRT={} blend={} op={} src={} dst={} mask={} depth={} write={} stencil={} stencilMask={}",
				target.get() == menuColor->rtv.get(), additionalTarget, bool(blend.BlendEnable), uint32_t(blend.BlendOp),
				uint32_t(blend.SrcBlend), uint32_t(blend.DestBlend), uint32_t(blend.RenderTargetWriteMask), bool(depthDesc.DepthEnable),
				uint32_t(depthDesc.DepthWriteMask), bool(depthDesc.StencilEnable), uint32_t(depthDesc.StencilWriteMask));
		return false;
	}
	CS_GPU_PASS("Upscaling::MenuLayerCapture");
	auto* layer = menuUILayer->rtv.get();
	drawContext->OMSetRenderTargets(1, &layer, depth.get());
	drawContext->OMSetBlendState(menuLayerBlend[blend.SrcBlend == D3D11_BLEND_ONE ? 1 : 0].get(), blendFactor, sampleMask);
	const auto restore = [&]() {
		auto* previous = target.get();
		drawContext->OMSetRenderTargets(1, &previous, depth.get());
		drawContext->OMSetBlendState(blendState.get(), blendFactor, sampleMask);
	};
	try {
		draw();
	} catch (...) {
		restore();
		throw;
	}
	restore();
	++menuLayerDraws;
	return true;
}

void VRSubmitUpscaling::MenuDrawIndexed::thunk(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
{
	auto& owner = globals::features::upscaling.vrSubmit;
	if (!owner.menuDrawing || !owner.CaptureMenuDraw(ctx, [&]() { func(ctx, count, start, base); }))
		func(ctx, count, start, base);
}

void VRSubmitUpscaling::MenuDraw::thunk(ID3D11DeviceContext* ctx, UINT count, UINT start)
{
	auto& owner = globals::features::upscaling.vrSubmit;
	if (!owner.menuDrawing || !owner.CaptureMenuDraw(ctx, [&]() { func(ctx, count, start); }))
		func(ctx, count, start);
}

void VRSubmitUpscaling::MenuDrawIndexedInstanced::thunk(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, INT base, UINT firstInstance)
{
	auto& owner = globals::features::upscaling.vrSubmit;
	if (!owner.menuDrawing || !owner.CaptureMenuDraw(ctx, [&]() { func(ctx, count, instances, start, base, firstInstance); }))
		func(ctx, count, instances, start, base, firstInstance);
}

void VRSubmitUpscaling::MenuDrawInstanced::thunk(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, UINT firstInstance)
{
	auto& owner = globals::features::upscaling.vrSubmit;
	if (!owner.menuDrawing || !owner.CaptureMenuDraw(ctx, [&]() { func(ctx, count, instances, start, firstInstance); }))
		func(ctx, count, instances, start, firstInstance);
}

void VRSubmitUpscaling::MenuUIHook::thunk(RE::BSGraphics::BSShaderAccumulator* accumulator, uint32_t flags)
{
	auto& owner = globals::features::upscaling.vrSubmit;
	if (!owner.DrawNativeMenuUI(accumulator, flags))
		func(accumulator, flags);
}

void VRSubmitUpscaling::MenuViewportHook::thunk(RE::BSGraphics::Renderer* renderer, uint32_t width, uint32_t height, bool matchTarget)
{
	auto& owner = globals::features::upscaling.vrSubmit;
	if (GetCurrentThreadId() == owner.renderThread && owner.menuDrawing &&
		globals::game::shadowState->GetVRRuntimeData().renderTargets[0] == RE::RENDER_TARGETS::kMENUBG) {
		width = owner.plan.outputWidth * 2;
		height = owner.plan.outputHeight;
		matchTarget = true;
	}
	func(renderer, width, height, matchTarget);
}

bool VRSubmitUpscaling::ResolveNativeMenu(ID3D11Texture2D* source)
{
	if (!menuReady || menuCycle != cycle || !WantsNativeMenuUI() || shaderResetPending || !menuSceneReference || !menuSceneMismatch)
		return false;
	if (menuResolvedCycle == menuCycle)
		return menuResolvedSource == source;
	auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	ID3D11ShaderResourceView* sourceSRV = nullptr;
	for (const auto index : { RE::RENDER_TARGETS::kMENUBG, RE::RENDER_TARGETS::kTOTAL, RE::RENDER_TARGETS::kFRAMEBUFFER }) {
		if (targets[index].texture == source)
			sourceSRV = targets[index].SRV;
	}
	if (!sourceSRV)
		return false;
	D3D11_SHADER_RESOURCE_VIEW_DESC sourceDesc{};
	sourceSRV->GetDesc(&sourceDesc);
	if (sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
		return false;
	CS_GPU_PASS("Upscaling::NativeMenuResolve");
	ContextScope scope(context.get(), isolatedState.get());
	const UINT zero[4]{};
	context->ClearUnorderedAccessViewUint(menuMismatch->uav.get(), zero);
	ID3D11ShaderResourceView* inputs[] = { sourceSRV, menuReference->srv.get(), menuColor->srv.get() };
	context->CSSetShaderResources(0, 3, inputs);
	auto mismatch = menuMismatch->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &mismatch, nullptr);
	context->CSSetShader(menuValidateCS.get(), nullptr, 0);
	context->Dispatch((plan.renderWidth * 2 + 7) / 8, (plan.renderHeight + 7) / 8, 1);
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	const bool reconstructedScene = menuSceneReconstructed && menuSceneCycle == cycle;
	auto sceneReference = reconstructedScene ? menuSceneReference->srv.get() : sourceSRV;
	const UINT unavailable[4]{ 1, 1, 1, 1 };
	context->ClearUnorderedAccessViewUint(menuSceneMismatch->uav.get(), reconstructedScene ? zero : unavailable);
	context->CSSetShaderResources(1, 1, &sceneReference);
	auto sceneMismatch = menuSceneMismatch->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &sceneMismatch, nullptr);
	context->Dispatch((plan.renderWidth * 2 + 7) / 8, (plan.renderHeight + 7) / 8, 1);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	ID3D11ShaderResourceView* sceneInputs[] = { reconstructedScene ? menuScene->srv.get() : sourceSRV, menuSceneMismatch->srv.get() };
	context->CSSetShaderResources(4, 2, sceneInputs);
	auto* uiLayer = menuUILayerCycle == cycle && menuLayerDraws ? menuUILayer->srv.get() : nullptr;
	context->CSSetShaderResources(6, 1, &uiLayer);
	auto mismatchSRV = menuMismatch->srv.get();
	context->CSSetShaderResources(3, 1, &mismatchSRV);
	auto destination = menuResolved->uav.get();
	auto sampler = menuSampler.get();
	context->CSSetSamplers(0, 1, &sampler);
	context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
	context->CSSetShader(menuResolveCS.get(), nullptr, 0);
	context->Dispatch((plan.outputWidth * 2 + 7) / 8, (plan.outputHeight + 7) / 8, 1);
	menuResolvedCycle = menuCycle;
	menuResolvedSource = source;
	return SUCCEEDED(globals::d3d::device->GetDeviceRemovedReason());
}

bool VRSubmitUpscaling::EnsureResources()
{
	if (eyes[0].color)
		return true;
	EyeResources created[2];
	for (uint32_t eye = 0; eye < 2; ++eye) {
		auto& resources = created[eye];
		const auto name = std::format("Upscaling::SubmitEye{}", eye);
		resources.color = MakeTexture(plan.renderWidth, plan.renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, name + " Color");
		resources.output = MakeTexture(plan.outputWidth, plan.outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, name + " Output");
		resources.sharpened = MakeTexture(plan.outputWidth, plan.outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, name + " Sharpened");
		resources.submit = MakeTexture(plan.outputWidth, plan.outputHeight, DXGI_FORMAT_R8G8B8A8_UNORM, name + " Presentation");
		resources.depth = MakeTexture(plan.renderWidth, plan.renderHeight, DXGI_FORMAT_R32_FLOAT, name + " Depth");
		resources.motion = MakeTexture(plan.renderWidth, plan.renderHeight, DXGI_FORMAT_R16G16_FLOAT, name + " Motion");
		resources.reactive = MakeTexture(plan.renderWidth, plan.renderHeight, DXGI_FORMAT_R8_UNORM, name + " Reactive");
		resources.transparency = MakeTexture(plan.renderWidth, plan.renderHeight, DXGI_FORMAT_R8_UNORM, name + " Transparency");
	}
	auto encode = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Upscaling::UpscalingDataCB>(), "Upscaling::SubmitEncode CB");
	auto color = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ColorConstants>(), "Upscaling::SubmitColor CB");
	for (uint32_t eye = 0; eye < 2; ++eye)
		eyes[eye] = std::move(created[eye]);
	encodeBuffer = std::move(encode);
	colorBuffer = std::move(color);
	return true;
}

void VRSubmitUpscaling::CaptureInputs()
{
	if (!active)
		return;
	// Startup resource allocation does not establish the thread that renders world frames.
	DWORD unassignedThread = 0;
	renderThread.compare_exchange_strong(unassignedThread, GetCurrentThreadId());
	if (GetCurrentThreadId() != renderThread) {
		SetStatus("Upscaling skipped: post-processing thread changed");
		return;
	}
	std::unique_lock lock(mutex, std::try_to_lock);
	if (!lock)
		return;
	if (shaderResetPending.exchange(false)) {
		Invalidate();
		sourceCopy = nullptr;
		sourceView = nullptr;
		for (auto& shader : encodeShaders)
			shader.Reset();
		colorShader.Reset();
		menuCopyVS.Reset();
		menuCopyPS.Reset();
		menuValidateCS.Reset();
		menuResolveCS.Reset();
		menuReady = false;
		menuSceneReconstructed = false;
		menuSceneCycle = UINT64_MAX;
		menuUnavailable = false;
		menuSceneUnavailable = false;
		SetupMenuResources();
		ClearFoveationResources();
		failed = false;
	}
	if (failed)
		return;
	auto* state = globals::state;
	const auto currentCycle = cycle.load();
	if (captured && captureCycle == currentCycle)
		return;
	captured = attempted = pairReady = false;
	menuSceneReconstructed = false;
	menuSceneCycle = UINT64_MAX;
	submittedSource = nullptr;
	capturedMenuScene = CanCaptureMenuScene();
	capturedMapScene = capturedMenuScene && state->isMapMenuOpen;
	LogMenuDiagnostics(0);
	if (!state->worldRenderedThisFrame || (IsMenuFrame() && !capturedMenuScene)) {
		if (!state->worldRenderedThisFrame)
			menuSceneFallback = "no fresh world frame";
		Invalidate();
		SetStatus(IsMenuFrame() ? "Menu: engine TAA at render resolution" : "Upscaling paused: no active world frame");
		return;
	}
	auto& upscaling = globals::features::upscaling;
	auto method = upscaling.GetUpscaleMethod();
	if (method != Upscaling::UpscaleMethod::kDLSS && method != Upscaling::UpscaleMethod::kFSR) {
		SetStatus("Upscaling skipped: no DLSS or FSR backend");
		return;
	}
	try {
		winrt::com_ptr<ID3D11DeviceContext1> currentContext;
		if (FAILED(globals::d3d::context->QueryInterface(currentContext.put())) || currentContext.get() != context.get()) {
			Fail("render context changed");
			return;
		}
		if (FAILED(globals::d3d::device->GetDeviceRemovedReason())) {
			Fail("device removed");
			return;
		}
		CS_GPU_PASS("Upscaling::SubmitCapture");
		ContextScope scope(context.get(), isolatedState.get());
		EnsureResources();
		auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
		auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		ID3D11ShaderResourceView* views[] = { targets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK].SRV,
			targets[globals::deferred->forwardRenderTargets[2]].SRV, targets[RE::RENDER_TARGETS::kMOTION_VECTOR].SRV, depth.depthSRV };
		for (auto* view : views) {
			D3D11_TEXTURE2D_DESC desc{};
			if (!view || !Util::GetTexture2DDesc(view, desc) || desc.Width != plan.renderWidth * 2 ||
				desc.Height != plan.renderHeight || desc.SampleDesc.Count != 1 || desc.ArraySize != 1) {
				Fail("auxiliary texture layout does not match the resolution plan");
				return;
			}
		}
		const bool dlss = method == Upscaling::UpscaleMethod::kDLSS;
		if (capturedMapScene) {
			upscaling.FillMenuCameraMotionVectors();
			if (!upscaling.menuCameraMVsValid) {
				menuSceneUnavailable = true;
				menuSceneFallback = "map camera motion unavailable";
				SetStatus("Map: camera motion unavailable; retaining TAA");
				return;
			}
		}
		auto* shader = encodeShaders[dlss ? 0 : 1].Get(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl",
			{ { dlss ? "DLSS" : "FSR", "" }, { "DEPTH_OUTPUT", "" } }, "cs_5_0", "main", "Upscaling::SubmitEncode CS");
		if (!shader || !colorShader.Get(L"Data/Shaders/Upscaling/SubmitColorCS.hlsl", {}, "cs_5_0", "main", "Upscaling::SubmitColor CS")) {
			Fail("required shader unavailable");
			return;
		}
		context->CSSetShader(shader, nullptr, 0);
		context->CSSetShaderResources(0, 4, views);
		auto shared = state->sharedDataCB->CB();
		context->CSSetConstantBuffers(5, 1, &shared);
		for (uint32_t eye = 0; eye < 2; ++eye) {
			Upscaling::UpscalingDataCB data{ { float(plan.renderWidth), float(plan.renderHeight) }, eye * plan.renderWidth, 0 };
			encodeBuffer->Update(data);
			auto buffer = encodeBuffer->CB();
			context->CSSetConstantBuffers(0, 1, &buffer);
			auto& resources = eyes[eye];
			ID3D11UnorderedAccessView* outputs[] = { resources.reactive->uav.get(), resources.transparency->uav.get(),
				resources.motion->uav.get(), resources.depth->uav.get() };
			context->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
			context->Dispatch((plan.renderWidth + 7) / 8, (plan.renderHeight + 7) / 8, 1);
		}
		captureCycle = currentCycle;
		capturedJitter = upscaling.jitter;
		temporal = { capturedJitter, *globals::game::cameraNear, *globals::game::cameraFar,
			Util::GetVerticalFOVRad(), *globals::game::deltaTime * 1000.0f };
		const auto captureTime = std::chrono::steady_clock::now();
		if (capturedMenuScene)
			temporal.frameTime = std::clamp(std::chrono::duration<float, std::milli>(captureTime - lastCaptureTime).count(), 1.0f, 100.0f);
		lastCaptureTime = captureTime;
		if (!std::isfinite(temporal.cameraNear) || !std::isfinite(temporal.cameraFar) || !std::isfinite(temporal.verticalFov) ||
			!std::isfinite(temporal.frameTime) || temporal.cameraNear <= 0 || temporal.cameraFar <= temporal.cameraNear ||
			temporal.verticalFov <= 0 || temporal.verticalFov >= DirectX::XM_PI || temporal.frameTime < 0) {
			SetStatus("Upscaling skipped: invalid camera parameters");
			return;
		}
		if (dlss) {
			for (uint32_t eye = 0; eye < 2; ++eye) {
				if (!upscaling.streamline.CheckFrameConstants(eye == 0 ? upscaling.streamline.viewport : upscaling.streamline.viewportRight,
						eye, &cameraConstants[eye])) {
					Fail("camera constants unavailable");
					return;
				}
			}
		}
		if (capturedMethod != uint32_t(method))
			lastSuccessCycle = UINT64_MAX;
		capturedMethod = uint32_t(method);
		captured = true;
	} catch (const std::exception& error) {
		Fail(error.what());
	} catch (...) {
		Fail("input capture failed");
	}
}

bool VRSubmitUpscaling::ValidateSource(ID3D11Texture2D* source, vr::EVREye eye, const vr::VRTextureBounds_t* bounds) const
{
	if (!bounds || (eye != vr::Eye_Left && eye != vr::Eye_Right))
		return false;
	const float offset = eye == vr::Eye_Left ? 0.0f : 0.5f;
	if (!std::isfinite(bounds->uMin) || !std::isfinite(bounds->uMax) || !std::isfinite(bounds->vMin) || !std::isfinite(bounds->vMax) ||
		std::min(bounds->uMin, bounds->uMax) != offset || std::max(bounds->uMin, bounds->uMax) != offset + 0.5f ||
		std::min(bounds->vMin, bounds->vMax) != 0 || std::max(bounds->vMin, bounds->vMax) != 1)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	source->GetDesc(&desc);
	winrt::com_ptr<ID3D11Device> device;
	source->GetDevice(device.put());
	winrt::com_ptr<IUnknown> sourceDevice, renderDevice;
	if (FAILED(device->QueryInterface(sourceDevice.put())) || FAILED(globals::d3d::device->QueryInterface(renderDevice.put())))
		return false;
	return sourceDevice.get() == renderDevice.get() && desc.Width == plan.renderWidth * 2 && desc.Height == plan.renderHeight &&
	       desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM && desc.ArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1;
}

void VRSubmitUpscaling::ConvertColor(ID3D11ShaderResourceView* source, ID3D11UnorderedAccessView* output,
	uint32_t width, uint32_t height, uint32_t offset, uint32_t conversion)
{
	CS_GPU_PASS("Upscaling::SubmitColor");
	ColorConstants data{ width, height, offset, conversion };
	colorBuffer->Update(data);
	auto buffer = colorBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->CSSetShader(colorShader.get(), nullptr, 0);
	context->CSSetShaderResources(0, 1, &source);
	context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullView = nullptr;
	ID3D11UnorderedAccessView* nullOutput = nullptr;
	context->CSSetShaderResources(0, 1, &nullView);
	context->CSSetUnorderedAccessViews(0, 1, &nullOutput, nullptr);
}

bool VRSubmitUpscaling::ReconstructPair(ID3D11Texture2D* source, vr::EColorSpace colorSpace)
{
	CS_GPU_PASS("Upscaling::SubmitReconstruct");
	ContextScope scope(context.get(), isolatedState.get());
	auto& upscaling = globals::features::upscaling;
	if (!sourceCopy) {
		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = desc.MiscFlags = 0;
		winrt::com_ptr<ID3D11Texture2D> copy;
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		winrt::check_hresult(globals::d3d::device->CreateTexture2D(&desc, nullptr, copy.put()));
		Util::SetResourceName(copy.get(), "Upscaling::SubmitSource");
		winrt::check_hresult(globals::d3d::device->CreateShaderResourceView(copy.get(), nullptr, view.put()));
		Util::SetResourceName(view.get(), "Upscaling::SubmitSource SRV");
		sourceCopy = std::move(copy);
		sourceView = std::move(view);
	}
	context->CopyResource(sourceCopy.get(), source);
	const bool gamma = colorSpace != vr::ColorSpace_Linear;
	resetHistory = lastSuccessCycle == UINT64_MAX || lastSuccessCycle + 1 != captureCycle ||
	               lastSuccessMenuScene != capturedMenuScene || lastSuccessMapScene != capturedMapScene;
	if (upscaling.pendingDLSSReset.exchange(false))
		resetHistory = true;
	for (uint32_t eye = 0; eye < 2; ++eye)
		ConvertColor(sourceView.get(), eyes[eye].color->uav.get(), plan.renderWidth, plan.renderHeight, eye * plan.renderWidth, gamma ? 1 : 0);
	const bool wasFoveated = foveatedPair;
	foveatedPair = PrepareFoveation();
	if (wasFoveated && !foveatedPair) {
		resetHistory = true;
		if (capturedMethod == uint32_t(Upscaling::UpscaleMethod::kDLSS))
			upscaling.streamline.DestroyDLSSResources();
	}
	for (uint32_t eye = 0; eye < 2; ++eye) {
		auto& fullEye = eyes[eye];
		auto& resources = foveatedPair ? foveatedEyes[eye].crop : fullEye;
		const auto input = foveatedPair ? foveatedEyes[eye].input : sl::Extent{ 0, 0, plan.renderWidth, plan.renderHeight };
		const auto output = foveatedPair ? foveatedEyes[eye].output : sl::Extent{ 0, 0, plan.outputWidth, plan.outputHeight };
		dispatching = true;
		bool success;
		if (capturedMethod == uint32_t(Upscaling::UpscaleMethod::kDLSS)) {
			CS_GPU_PASS("Upscaling::SubmitDLSS");
			auto constants = GetEyeConstants(eye);
			if (resetHistory)
				constants.reset = sl::Boolean::eTrue;
			success = upscaling.streamline.EvaluateDLSS(eye == 0 ? upscaling.streamline.viewport : upscaling.streamline.viewportRight, eye,
				resources.color->resource.get(), resources.output->resource.get(), resources.depth->resource.get(), resources.motion->resource.get(),
				resources.reactive->resource.get(), resources.transparency->resource.get(),
				{ 0, 0, input.width, input.height }, { 0, 0, output.width, output.height }, output.width, output.height, &constants);
		} else {
			CS_GPU_PASS("Upscaling::SubmitFSR");
			success = upscaling.fidelityFX.UpscaleRegion(eye, resources.color->resource.get(), resources.depth->resource.get(), resources.motion->resource.get(),
				resources.reactive->resource.get(), resources.transparency->resource.get(), resources.output->resource.get(),
				input.width, input.height, output.width, output.height, float(plan.renderWidth), float(plan.renderHeight), upscaling.settings.sharpnessFSR, foveatedPair);
		}
		dispatching = false;
		if (!success)
			return false;
		context->ClearState();
		if (foveatedPair)
			ComposeFoveatedEye(eye);
		auto* reconstructed = fullEye.output.get();
		if (capturedMethod == uint32_t(Upscaling::UpscaleMethod::kDLSS) && upscaling.settings.sharpnessEnabledDLSS && upscaling.settings.sharpnessDLSS > 0) {
			if (upscaling.rcas.ApplySharpen(fullEye.output->srv.get(), fullEye.sharpened->uav.get(), std::exp2(2 * upscaling.settings.sharpnessDLSS - 2)))
				reconstructed = fullEye.sharpened.get();
		}
		ConvertColor(reconstructed->srv.get(), fullEye.submit->uav.get(), plan.outputWidth, plan.outputHeight, 0, gamma ? 2 : 0);
	}
	if (FAILED(globals::d3d::device->GetDeviceRemovedReason()))
		return false;
	lastSuccessCycle = captureCycle;
	lastSuccessMenuScene = capturedMenuScene;
	lastSuccessMapScene = capturedMapScene;
	reconstructedPairs.fetch_add(1);
	return true;
}

vr::EVRCompositorError VRSubmitUpscaling::SubmitHook::thunk(vr::IVRCompositor* self, vr::EVREye eye,
	const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags)
{
	static thread_local bool entered = false;
	auto& owner = globals::features::upscaling.vrSubmit;
	if (entered || !owner.active || owner.failed || !texture || !texture->handle)
		return func(self, eye, texture, bounds, flags);
	if (GetCurrentThreadId() != owner.renderThread) {
		owner.SetStatus("Upscaling skipped: submission is on another thread");
		return func(self, eye, texture, bounds, flags);
	}
	SubmitScope submitScope(entered);
	std::unique_lock lock(owner.mutex, std::try_to_lock);
	if (!lock)
		return func(self, eye, texture, bounds, flags);
	auto& upscaling = globals::features::upscaling;
	if (owner.WantsNativeMenuUI()) {
		if (eye == vr::Eye_Left)
			owner.LogMenuDiagnostics(3);
		winrt::com_ptr<ID3D11Texture2D> menuSource;
		if (texture->eType == vr::TextureType_DirectX && flags == vr::Submit_Default &&
			(texture->eColorSpace == vr::ColorSpace_Auto || texture->eColorSpace == vr::ColorSpace_Gamma || texture->eColorSpace == vr::ColorSpace_Linear) &&
			SUCCEEDED(static_cast<IUnknown*>(texture->handle)->QueryInterface(menuSource.put())) &&
			owner.ValidateSource(menuSource.get(), eye, bounds) && owner.ResolveNativeMenu(menuSource.get())) {
			vr::Texture_t replacement = *texture;
			replacement.handle = owner.menuResolved->resource.get();
			owner.SetStatus(owner.menuSceneReconstructed ?
								std::format("Menu: {} scene reconstructed; post-upscale UI draws={} (unsupported={})",
									owner.capturedMethod == uint32_t(Upscaling::UpscaleMethod::kDLSS) ? "DLSS" : "FSR",
									owner.menuLayerDraws, owner.menuLayerRejected) :
								std::format("Menu: full-resolution UI; scene fallback: {}", owner.menuSceneFallback));
			const auto result = func(self, eye, &replacement, bounds, flags);
			if (result != vr::VRCompositorError_None) {
				owner.menuUnavailable = true;
				owner.menuReady = false;
				owner.SetStatus("Menu: compositor rejected native output; using engine output until reset");
			}
			return result;
		}
		owner.SetStatus("Menu: native UI pass unavailable; using engine TAA output");
		return func(self, eye, texture, bounds, flags);
	}
	if (!owner.captured)
		return func(self, eye, texture, bounds, flags);
	// Desktop Present advances frameCount before OpenVR may consume this cycle's inputs.
	if (owner.captureCycle != owner.cycle || owner.capturedMethod != uint32_t(upscaling.GetUpscaleMethod())) {
		owner.SetStatus("Upscaling skipped: inputs belong to another compositor cycle or backend");
		return func(self, eye, texture, bounds, flags);
	}
	winrt::com_ptr<ID3D11Texture2D> source;
	if (texture->eType != vr::TextureType_DirectX || flags != vr::Submit_Default ||
		FAILED(static_cast<IUnknown*>(texture->handle)->QueryInterface(source.put())) || !owner.ValidateSource(source.get(), eye, bounds) ||
		(texture->eColorSpace != vr::ColorSpace_Auto && texture->eColorSpace != vr::ColorSpace_Gamma && texture->eColorSpace != vr::ColorSpace_Linear)) {
		owner.Fail("unsupported submission layout or color space");
		return func(self, eye, texture, bounds, flags);
	}
	try {
		if (!owner.attempted) {
			owner.attempted = true;
			owner.submittedSource = source;
			owner.sourceColorSpace = texture->eColorSpace;
			owner.pairReady = owner.ReconstructPair(source.get(), texture->eColorSpace);
			if (!owner.pairReady)
				owner.Fail("vendor dispatch failed");
		}
	} catch (const std::exception& error) {
		owner.dispatching = false;
		owner.Fail(error.what());
	} catch (...) {
		owner.dispatching = false;
		owner.Fail("reconstruction failed");
	}
	if (!owner.pairReady)
		return func(self, eye, texture, bounds, flags);
	if (owner.captureCycle != owner.cycle || owner.submittedSource.get() != source.get() || owner.sourceColorSpace != texture->eColorSpace) {
		owner.Fail("stereo submission changed during reconstruction");
		return func(self, eye, texture, bounds, flags);
	}
	vr::Texture_t replacement = *texture;
	replacement.handle = owner.eyes[eye == vr::Eye_Right ? 1 : 0].submit->resource.get();
	const bool flipX = bounds->uMin > bounds->uMax;
	const bool flipY = bounds->vMin > bounds->vMax;
	const vr::VRTextureBounds_t outputBounds{ flipX ? 1.0f : 0.0f, flipY ? 1.0f : 0.0f, flipX ? 0.0f : 1.0f, flipY ? 0.0f : 1.0f };
	const auto result = func(self, eye, &replacement, &outputBounds, flags);
	if (result != vr::VRCompositorError_None)
		owner.Fail("compositor rejected reconstructed output");
	else
		owner.SetStatus(std::format("{} output submitted ({})",
			owner.capturedMethod == uint32_t(Upscaling::UpscaleMethod::kDLSS) ? "DLSS" : "FSR",
			owner.foveatedPair ? "foveated" : "full eye"));
	return result;
}
