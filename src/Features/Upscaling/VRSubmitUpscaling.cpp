#include "VRSubmitUpscaling.h"

#include "Deferred.h"
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
	return active && !failed && globals::state && !globals::state->IsPausedOrMenuOpen(globals::game::ui);
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
		for (auto& shader : encodeShaders)
			shader.Reset();
		colorShader.Reset();
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
	submittedSource = nullptr;
	if (!state->worldRenderedThisFrame || state->IsPausedOrMenuOpen(globals::game::ui)) {
		SetStatus("Upscaling paused: no active world frame");
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
		winrt::check_hresult(globals::d3d::device->CreateTexture2D(&desc, nullptr, sourceCopy.put()));
		Util::SetResourceName(sourceCopy.get(), "Upscaling::SubmitSource");
		winrt::check_hresult(globals::d3d::device->CreateShaderResourceView(sourceCopy.get(), nullptr, sourceView.put()));
		Util::SetResourceName(sourceView.get(), "Upscaling::SubmitSource SRV");
	}
	context->CopyResource(sourceCopy.get(), source);
	const bool gamma = colorSpace != vr::ColorSpace_Linear;
	resetHistory = lastSuccessCycle == UINT64_MAX || lastSuccessCycle + 1 != captureCycle;
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
