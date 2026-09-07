#include "../../../GpuPass.h"
#include "../PerfMode.h"

#include <algorithm>
#include <cmath>

#include "../../../State.h"
#include "../../Upscaling.h"

// Quality mode -> render-scale resolution is supplied by the FFX SDK helper
// (same one Upscaling.cpp uses), avoiding a duplicate scale table here.
#include <FidelityFX/host/ffx_fsr3.h>

// ============================================================================
// UIPassDispatch_Hook: swap KMAIN DS → fakeDS for UI pass (renderMode==24)
// ============================================================================
// UI pass draws VR HUD to kMENUBG (now 3k). Engine binds KMAIN(DS) as DS,
// which is still 1k → size mismatch. Swap to fakeDS (3k) before, restore after.

void PerfMode::UIPassDispatch_Hook::thunk(RE::BSGraphics::BSShaderAccumulator* shaderAccumulator, uint32_t renderFlags)
{
	auto& perfMode = globals::features::upscaling.perfMode;

	// Only intercept renderMode==24 (UI pass) when hook is active
	auto& rtData = shaderAccumulator->GetRuntimeData();
	if (!perfMode.hookActive || !perfMode.fakeDSV || rtData.renderMode != 24) {
		func(shaderAccumulator, renderFlags);
		return;
	}

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& dsData = renderer->GetDepthStencilData();
	auto& kmainDS = dsData.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	// Save original KMAIN DS views and swap to fakeDS
	ID3D11DepthStencilView* savedViews[8] = {};
	ID3D11DepthStencilView* savedReadOnlyViews[8] = {};
	for (int i = 0; i < 8; i++) {
		savedViews[i] = kmainDS.views[i];
		if (kmainDS.views[i])
			kmainDS.views[i] = perfMode.fakeDSV.get();
	}
	for (int i = 0; i < 8; i++) {
		savedReadOnlyViews[i] = kmainDS.readOnlyViews[i];
		if (kmainDS.readOnlyViews[i])
			kmainDS.readOnlyViews[i] = perfMode.fakeDSV.get();
	}

	// Force engine to re-bind DS from struct
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	// Force 3k VP: engine may not call UpdateViewPort during UI pass,
	// so we directly set shadowState viewport to DisplayRes and mark dirty.
	auto* ss = globals::game::shadowState;
	D3D11_VIEWPORT savedVP = {};
	if (ss) {
		auto& vp = ss->GetVRRuntimeData().viewPort;
		savedVP = vp;
		vp.TopLeftX = 0.0f;
		vp.TopLeftY = 0.0f;
		vp.Width = static_cast<float>(perfMode.displayEyeWidth * 2);
		vp.Height = static_cast<float>(perfMode.displayEyeHeight);
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
	}

	// fakeDS is only cleared at init, so without this a prior frame's UI-pass depth (controllers)
	// lingers and LessEqual-occludes the menu compositor — the persistent hand-shaped cutout.
	const uint32_t frame = globals::state ? globals::state->frameCount : 0;
	if (perfMode.lastFakeDSClearFrame != frame) {
		perfMode.ClearFakeDS();
		perfMode.lastFakeDSClearFrame = frame;
	}

	// Skip VP compression in UpdateViewPort hook during UI pass
	perfMode.postInterceptActive = true;

	func(shaderAccumulator, renderFlags);

	perfMode.postInterceptActive = false;

	// Restore viewport
	if (ss) {
		ss->GetVRRuntimeData().viewPort = savedVP;
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
	}

	// Restore original KMAIN DS views
	for (int i = 0; i < 8; i++)
		kmainDS.views[i] = savedViews[i];
	for (int i = 0; i < 8; i++)
		kmainDS.readOnlyViews[i] = savedReadOnlyViews[i];

	// Re-dirty so subsequent passes get correct DS
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

// Bridge the DLSS-reconstructed menu BG into kTOTAL/kMENUBG. Driven from
// TonemapRender_Hook post-func in menu/loading state: the engine's tonemap
// shader's UV math assumes RT.size == kMAIN.size (true for DLAA, broken
// under DLSS), so we run our own DLSS evaluate against the engine's
// menu-state inputs (jitter via Main_UpdateJitter, depth via menu BG pre-
// pass, motion vectors as ISTemporalAA reads them) and blit testTexture →
// dest. One-shot per frame via blittedFrameId (Present doesn't fire here
// and PlayerView doesn't fire in main-menu, so the frame-id guard is the
// only reliable per-frame boundary).
void PerfMode::MaybeBlitMenuBG(uint32_t boundRTIdx)
{
	const uint32_t currentFrame = globals::state ? globals::state->frameCount : 0;
	if (!hookActive || blittedFrameId == currentFrame || !menuBlitPS || !boxDownscaleVS || !linearSampler)
		return;
	if (!testTexture || !testTextureSRV)
		return;
	if (!globals::state || !globals::state->IsStaticMenuBackdropOpen(globals::game::ui))
		return;
	if (boundRTIdx != RE::RENDER_TARGETS::kTOTAL &&
		boundRTIdx != RE::RENDER_TARGETS::kMENUBG)
		return;

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto& rtData = renderer->GetRuntimeData();
	auto& dest = rtData.renderTargets[boundRTIdx];
	if (!dest.RTV || !dest.texture)
		return;

	CS_GPU_PASS("PerfMode::MenuBGBlit");

	globals::features::upscaling.Upscale();

	// Sharpening redirects DLSS's write to refraTempTex; RCAS's UAV write into testTexture
	// doesn't land from this one-shot call site, so resolve with a copy instead (unsharpened).
	if (globals::features::upscaling.IsPerfModeSharpenRedirectActive())
		context->CopyResource(testTexture.get(), refraTempTex.get());

	{
		Util::FullscreenPassScope stateScope(context);

		// IA: fullscreen triangle, no VB/IB
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		context->VSSetShader(boxDownscaleVS.get(), nullptr, 0);
		context->PSSetShader(menuBlitPS.get(), nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);

		ID3D11ShaderResourceView* srvs[] = { testTextureSRV.get() };
		context->PSSetShaderResources(0, 1, srvs);
		ID3D11SamplerState* samplers[] = { linearSampler.get() };
		context->PSSetSamplers(0, 1, samplers);

		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		context->OMSetDepthStencilState(nullptr, 0);
		context->RSSetState(nullptr);

		D3D11_TEXTURE2D_DESC destDesc{};
		static_cast<ID3D11Texture2D*>(dest.texture)->GetDesc(&destDesc);
		D3D11_VIEWPORT vp = {};
		vp.Width = static_cast<float>(destDesc.Width);
		vp.Height = static_cast<float>(destDesc.Height);
		vp.MaxDepth = 1.0f;
		context->RSSetViewports(1, &vp);

		ID3D11RenderTargetView* rtv = dest.RTV;
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->Draw(3, 0);
	}

	blittedFrameId = currentFrame;
}

void PerfMode::InstallCreateRTThunks()
{
	if (!globals::game::isVR)
		return;
	auto vrBase = REL::RelocationID(100458, 107175).address();
	stl::write_thunk_call<CreateRT_MenuBG_Hook>(vrBase + 0x6cc);
	stl::write_thunk_call<CreateRT_ImagespaceTempCopy_Hook>(vrBase + 0x7a3);
	stl::write_thunk_call<CreateRT_Total_Hook>(vrBase + 0x1547);
}

void PerfMode::BeginCreateRTEnlarge()
{
	if (!hookActive)
		return;
	enlargeWidth = displayEyeWidth * 2;
	enlargeHeight = displayEyeHeight;
	enlargeActive = true;
}

void PerfMode::EndCreateRTEnlarge()
{
	enlargeActive = false;
}

namespace
{
	void EnlargeProps(RE::BSGraphics::RenderTargetProperties* a_props)
	{
		auto& dp = globals::features::upscaling.perfMode;
		if (!dp.IsCreateRTEnlargeActive())
			return;
		a_props->width = dp.GetEnlargeWidth();
		a_props->height = dp.GetEnlargeHeight();
	}
}

void PerfMode::CreateRT_MenuBG_Hook::thunk(RE::BSGraphics::Renderer* a_this, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties)
{
	EnlargeProps(a_properties);
	func(a_this, a_target, a_properties);
}

void PerfMode::CreateRT_ImagespaceTempCopy_Hook::thunk(RE::BSGraphics::Renderer* a_this, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties)
{
	EnlargeProps(a_properties);
	func(a_this, a_target, a_properties);
}

void PerfMode::CreateRT_Total_Hook::thunk(RE::BSGraphics::Renderer* a_this, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties)
{
	EnlargeProps(a_properties);
	func(a_this, a_target, a_properties);
}
