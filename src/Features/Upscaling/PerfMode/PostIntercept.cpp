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
// Tonemap hooks: ISHDRTonemapBlendCinematic + ...CinematicFade (Render vfunc
// 0x1 on vtable[3]) — both share RenderTonemapWithSwap.
// ============================================================================
// Installed via stl::write_vfunc<0x1> on vtable[3], chains after FrameAnnotations.
// Inner layer of two-layer swap: swaps kMAIN SRV → testTextureSRV and
// kMAIN DS → fakeDS before tonemap Render(), restores after.

template <class Hook>
void PerfMode::RenderTonemapWithSwap(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	auto& perfMode = globals::features::upscaling.perfMode;

	if (!perfMode.hookActive || !perfMode.testTextureSRV || !perfMode.fakeDSV) {
		Hook::func(imageSpaceShader, shape, param);
		return;
	}

	// Static menu/map backdrops: the engine's kTOTAL bridge leaves the BG missing
	// under renderRes; MaybeBlitMenuBG upscales+blits it in. A live VR Playroom
	// world remains on the gameplay path so it cannot become head-locked.
	if (globals::state && globals::state->IsStaticMenuBackdropOpen(globals::game::ui)) {
		Hook::func(imageSpaceShader, shape, param);
		perfMode.MaybeBlitMenuBG(RE::RENDER_TARGETS::kTOTAL);
		return;
	}

	CS_GPU_PASS("PerfMode::TonemapRender");

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& rtData = renderer->GetRuntimeData();
	auto& dsData = renderer->GetDepthStencilData();

	// --- Swap kMAIN SRV → testTextureSRV (so tonemap reads 3k upscaled color) ---
	auto& kmainRT = rtData.renderTargets[RE::RENDER_TARGETS::kMAIN];
	perfMode.savedKMainSRV = kmainRT.SRV;
	kmainRT.SRV = perfMode.testTextureSRV.get();

	// --- Also swap kMAIN_COPY SRV (refraction path reads this instead of kMAIN) ---
	auto& kmainCopyRT = rtData.renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];
	perfMode.savedKMainCopySRV = kmainCopyRT.SRV;
	kmainCopyRT.SRV = perfMode.testTextureSRV.get();

	// --- Swap kMAIN DS views → fakeDS (so 3k RT doesn't mismatch 1k DS) ---
	auto& kmainDS = dsData.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	for (int i = 0; i < 8; i++) {
		perfMode.savedKMainViews[i] = kmainDS.views[i];
		if (kmainDS.views[i])
			kmainDS.views[i] = perfMode.fakeDSV.get();
	}
	for (int i = 0; i < 8; i++) {
		perfMode.savedKMainReadOnlyViews[i] = kmainDS.readOnlyViews[i];
		if (kmainDS.readOnlyViews[i])
			kmainDS.readOnlyViews[i] = perfMode.fakeDSV.get();
	}

	// --- Call original (or FrameAnnotations chain) ---
	Hook::func(imageSpaceShader, shape, param);

	// --- Restore kMAIN SRV ---
	kmainRT.SRV = perfMode.savedKMainSRV;
	perfMode.savedKMainSRV = nullptr;

	// --- Restore kMAIN_COPY SRV ---
	kmainCopyRT.SRV = perfMode.savedKMainCopySRV;
	perfMode.savedKMainCopySRV = nullptr;

	// --- Restore kMAIN DS views ---
	for (int i = 0; i < 8; i++)
		kmainDS.views[i] = perfMode.savedKMainViews[i];
	for (int i = 0; i < 8; i++)
		kmainDS.readOnlyViews[i] = perfMode.savedKMainReadOnlyViews[i];
}

void PerfMode::TonemapRender_Hook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	PerfMode::RenderTonemapWithSwap<TonemapRender_Hook>(imageSpaceShader, shape, param);
}

void PerfMode::TonemapFadeRender_Hook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	PerfMode::RenderTonemapWithSwap<TonemapFadeRender_Hook>(imageSpaceShader, shape, param);
}

// ============================================================================
// RefractionRender_Hook: IS shader hook for ISRefraction
// ============================================================================
// Strategy: let func() run normally (1k refraction, kMAIN→kMAIN_COPY).
// After func() returns, D3D11 state is sticky (PS/CB/sampler/IA all still bound).
// We replay the draw with our own RT (testTexture 3k), VP (3k), and SRV (refraTempTex 3k).

void PerfMode::RefractionRender_Hook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	auto& perfMode = globals::features::upscaling.perfMode;

	if (!perfMode.hookActive || !perfMode.testTextureRTV || !perfMode.refraTempSRV) {
		func(imageSpaceShader, shape, param);
		return;
	}

	CS_GPU_PASS("PerfMode::RefractionRender");

	// --- Pass 1: engine's normal 1k refraction (untouched) ---
	func(imageSpaceShader, shape, param);

	// --- Pass 2: our 3k refraction replay ---
	// func() left PS/CB/sampler/IA/VB/IB all bound on the D3D context.
	// We only change RT, VP, and t0 SRV, then DrawIndexed with the same geometry.

	auto* context = globals::d3d::context;

	// Save current RT so we can restore after our draw
	ID3D11RenderTargetView* savedRTV = nullptr;
	ID3D11DepthStencilView* savedDSV = nullptr;
	context->OMGetRenderTargets(1, &savedRTV, &savedDSV);

	// Save the full viewport stack rather than a single VP — RSSetViewports/
	// RSGetViewports work on arrays sized up to D3D11_VIEWPORT_AND_SCISSORRECT_
	// OBJECT_COUNT_PER_PIPELINE (16). Truncating to one would silently drop
	// extra bound viewports if a later engine pass relied on multi-VP.
	UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	context->RSGetViewports(&numVP, savedVP);

	// Save current t0 SRV (kMAIN.SRV used by ISRefraction as scene input)
	ID3D11ShaderResourceView* savedSRV0 = nullptr;
	context->PSGetShaderResources(0, 1, &savedSRV0);

	// Set 3k output: testTexture RTV, no DS needed for fullscreen IS shader
	ID3D11RenderTargetView* rtv3k = perfMode.testTextureRTV.get();
	context->OMSetRenderTargets(1, &rtv3k, nullptr);

	// Set 3k VP
	D3D11_VIEWPORT vp3k = {};
	vp3k.TopLeftX = 0.0f;
	vp3k.TopLeftY = 0.0f;
	vp3k.Width = static_cast<float>(perfMode.displayEyeWidth * 2);
	vp3k.Height = static_cast<float>(perfMode.displayEyeHeight);
	vp3k.MinDepth = 0.0f;
	vp3k.MaxDepth = 1.0f;
	context->RSSetViewports(1, &vp3k);

	// Set 3k input: refraTempTex as t0 (scene color for refraction sampling)
	ID3D11ShaderResourceView* srv3k = perfMode.refraTempSRV.get();
	context->PSSetShaderResources(0, 1, &srv3k);

	// Draw with the same geometry (BSTriShape fullscreen quad, IA still bound)
	context->DrawIndexed(6, 0, 0);

	// --- Restore D3D state so engine continues normally ---
	context->OMSetRenderTargets(1, &savedRTV, savedDSV);
	// RSGetViewports may have returned 0 if the prior pass left no viewport
	// bound; skip the restore in that case rather than pushing a zero-init VP.
	if (numVP > 0) {
		context->RSSetViewports(numVP, savedVP);
	}
	context->PSSetShaderResources(0, 1, &savedSRV0);

	// Release COM refs from Get calls
	if (savedRTV)
		savedRTV->Release();
	if (savedDSV)
		savedDSV->Release();
	if (savedSRV0)
		savedSRV0->Release();
}

// ============================================================================
// ISCopyRender_Hook: stretch ISCopy when source < dest (menu compositor fix)
// ============================================================================
// The VR menu compositor uses a single ISCopy draw to blit the rendered scene
// (kMAIN — RenderRes under PerfMode) into a fixed-size projection surface
// (kPROJECTEDMENU 2048², or kMENUBG which PerfMode enlarges to DisplayRes).
// Engine ISCopy uses a 1:1 viewport sized to the source, so the small source
// is stamped into the top-left of the larger dest. Symptom: "main menu image
// looks downscaled."
//
// Fix: after func() runs, if dest > current VP we replay the draw with the
// VP expanded to the dest's full dims. ISCopy's PS/CB/IA/sampler are sticky
// on the D3D context after func() returns (same pattern RefractionRender_Hook
// relies on), so the replay needs only a viewport change + DrawIndexed and
// the engine's clamp-sampler stretches the source naturally.
//
// In-game ISCopy (where source.w == dest.w under PerfMode — kMAIN renderRes
// → kMAIN_COPY renderRes) takes the early-out branch and the engine's draw
// is the final pixel.

void PerfMode::ISCopyRender_Hook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	auto& perfMode = globals::features::upscaling.perfMode;

	// Inactive / non-VR: passthrough.
	if (!perfMode.hookActive) {
		func(imageSpaceShader, shape, param);
		return;
	}

	// Let the engine draw first. After func() returns the IS shader's PS, CB,
	// sampler, IA layout, vertex/index buffers, and topology are all still
	// bound on the context (sticky D3D11 state). We only need to override the
	// viewport for the replay draw.
	func(imageSpaceShader, shape, param);

	auto* context = globals::d3d::context;

	// Inspect the current RTV's dest dimensions.
	ID3D11RenderTargetView* curRTV = nullptr;
	ID3D11DepthStencilView* curDSV = nullptr;
	context->OMGetRenderTargets(1, &curRTV, &curDSV);
	if (!curRTV) {
		if (curDSV)
			curDSV->Release();
		return;
	}

	ID3D11Resource* rtRes = nullptr;
	curRTV->GetResource(&rtRes);
	if (!rtRes) {
		curRTV->Release();
		if (curDSV)
			curDSV->Release();
		return;
	}

	D3D11_TEXTURE2D_DESC rtDesc{};
	static_cast<ID3D11Texture2D*>(rtRes)->GetDesc(&rtDesc);
	rtRes->Release();

	// Current VP (the one func() used) — full array so we can restore exactly.
	UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	context->RSGetViewports(&numVP, savedVP);

	// Only intervene when either axis of the engine's VP is smaller than the
	// dest. The +1 guard avoids float-equality issues (VPs are floats, RT
	// dims are uint32). Width-only would miss the case where the engine
	// binds a taller-than-VP RT (e.g., a square 2048² panel against a
	// renderRes-height VP).
	bool needsStretch = numVP > 0 &&
	                    (rtDesc.Width > static_cast<UINT>(savedVP[0].Width + 1.0f) ||
							rtDesc.Height > static_cast<UINT>(savedVP[0].Height + 1.0f));

	if (needsStretch) {
		CS_GPU_PASS("PerfMode::ISCopyStretch");

		// Replay viewport: full dest extent, preserve depth range from the
		// original so anything sampling depth (unlikely for ISCopy but safe)
		// keeps the same Z behavior.
		D3D11_VIEWPORT stretchVP = savedVP[0];
		stretchVP.TopLeftX = 0.0f;
		stretchVP.TopLeftY = 0.0f;
		stretchVP.Width = static_cast<float>(rtDesc.Width);
		stretchVP.Height = static_cast<float>(rtDesc.Height);
		context->RSSetViewports(1, &stretchVP);

		// ISCopy is a fullscreen quad drawn as a triangle list (6 indices).
		// Same index count RefractionRender_Hook uses for the same reason —
		// both replay the IS shader's standard fullscreen geometry.
		context->DrawIndexed(6, 0, 0);

		// Restore engine's VP so any state inspector downstream sees what it
		// expects. numVP guaranteed > 0 inside this branch.
		context->RSSetViewports(numVP, savedVP);
	}

	curRTV->Release();
	if (curDSV)
		curDSV->Release();
}

// ============================================================================
// BeginPostIntercept / EndPostIntercept
// ============================================================================
// Outer layer of two-layer swap: swaps kMAIN_COPY DS → fakeDS before the
// entire Post chain (covers the copy step #10 which binds kMAIN_COPY DS).
// Inner layer (tonemap hook) handles kMAIN DS + kMAIN SRV for step #9.

void PerfMode::BeginPostIntercept()
{
	if (!hookActive || !fakeDSV)
		return;

	ZoneScoped;
	auto state = globals::state;
	state->BeginPerfEvent("PerfMode::BeginPostIntercept");

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& dsData = renderer->GetDepthStencilData();
	auto& kmainCopyDS = dsData.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];

	postInterceptActive = true;

	// Swap kMAIN_COPY DS views → fakeDS
	for (int i = 0; i < 8; i++) {
		savedKMainCopyViews[i] = kmainCopyDS.views[i];
		if (kmainCopyDS.views[i])
			kmainCopyDS.views[i] = fakeDSV.get();
	}
	for (int i = 0; i < 8; i++) {
		savedKMainCopyReadOnlyViews[i] = kmainCopyDS.readOnlyViews[i];
		if (kmainCopyDS.readOnlyViews[i])
			kmainCopyDS.readOnlyViews[i] = fakeDSV.get();
	}

	state->EndPerfEvent();
}

void PerfMode::EndPostIntercept()
{
	if (!hookActive || !fakeDSV)
		return;

	ZoneScoped;
	auto state = globals::state;
	state->BeginPerfEvent("PerfMode::EndPostIntercept");

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& dsData = renderer->GetDepthStencilData();
	auto& kmainCopyDS = dsData.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];

	postInterceptActive = false;
	postChainDone = true;

	// Restore kMAIN_COPY DS views
	for (int i = 0; i < 8; i++)
		kmainCopyDS.views[i] = savedKMainCopyViews[i];
	for (int i = 0; i < 8; i++)
		kmainCopyDS.readOnlyViews[i] = savedKMainCopyReadOnlyViews[i];

	state->EndPerfEvent();
}

// ============================================================================
// DownscaleToKMain: Box 3×3 downscale testTexture (3k) → kMAIN (1k)
// ============================================================================
// Called before the Post chain so the HDR pyramid builds from AA'd DLSS output
// instead of the raw 1k render, eliminating shimmer in bloom/exposure.
// Only kMAIN needs writing:
//   - No refraction: kMAIN is the pyramid input directly.
//   - With refraction: engine composites kMAIN → kMAIN_COPY, which enters pyramid.

void PerfMode::DownscaleToKMain()
{
	if (!hookActive || !testTextureSRV || !boxDownscalePS || !boxDownscaleVS || !linearSampler)
		return;

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto& rtData = renderer->GetRuntimeData();

	auto& kmain = rtData.renderTargets[RE::RENDER_TARGETS::kMAIN];

	// Bail before opening the perf event so we don't leak a dangling
	// Begin without End on the null-RTV early-return path.
	if (!kmain.RTV)
		return;

	CS_GPU_PASS("PerfMode::DownscaleToKMain");

	{
		Util::FullscreenPassScope stateScope(context);

		// IA: fullscreen triangle (no vertex/index buffers)
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// Shaders — clear GS/HS/DS to prevent pipeline interference
		context->VSSetShader(boxDownscaleVS.get(), nullptr, 0);
		context->PSSetShader(boxDownscalePS.get(), nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);

		ID3D11ShaderResourceView* srvs[] = { testTextureSRV.get() };
		context->PSSetShaderResources(0, 1, srvs);
		ID3D11SamplerState* samplers[] = { linearSampler.get() };
		context->PSSetSamplers(0, 1, samplers);

		// Opaque overwrite: no blending, no depth test, default rasterizer
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		context->OMSetDepthStencilState(nullptr, 0);
		context->RSSetState(nullptr);

		// Viewport at RenderRes SBS (1k)
		D3D11_VIEWPORT vp = {};
		vp.Width = static_cast<float>(renderEyeWidth * 2);
		vp.Height = static_cast<float>(renderEyeHeight);
		vp.MaxDepth = 1.0f;
		context->RSSetViewports(1, &vp);

		ID3D11RenderTargetView* rtv = kmain.RTV;
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->Draw(3, 0);
	}
}

void PerfMode::HandlePostProcessing(const std::function<void()>& enginePost)
{
	ZoneScoped;
	auto state = globals::state;
	state->BeginPerfEvent("PerfMode::HandlePostProcessing");

	// Copy testTexture → refraTempTex before Post, so ISRefraction can read 3k scene
	if (refraTempTex) {
		globals::d3d::context->CopyResource(refraTempTex.get(), testTexture.get());
	}

	// Downscale testTexture (3k AA'd) → kMAIN (1k) so the HDR pyramid and
	// bloom compute from anti-aliased content instead of raw 1k render.
	DownscaleToKMain();

	// Underwater mask analytical repair. Engine RTs (depth, mask) are at
	// renderRes under PerfMode, so the full-resolution path of UpscaleDepth
	// would apply — but routing through UpscaleDepth here leaves pipeline
	// state dirty and the trailing enginePost() loses kMAIN. Drive the
	// mask-only draw directly inside our own FullscreenPassScope so the
	// inbound engine state is restored on exit.
	{
		Util::FullscreenPassScope scope(globals::d3d::context);
		globals::features::upscaling.RunUnderwaterMaskRepair();
	}

	// Outer layer: swap kMAIN_COPY DS + SRV for refraction path coverage
	BeginPostIntercept();

	// Run full engine Post chain; IS shader hook handles tonemap step (#9) swap/restore
	enginePost();

	// Restore kMAIN_COPY DS
	EndPostIntercept();

	state->EndPerfEvent();
}
