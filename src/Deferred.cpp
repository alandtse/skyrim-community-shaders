#include "Deferred.h"

#include <DDSTextureLoader.h>

#include "GpuPass.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#include "Features/CSEditor.h"
#include "Features/DynamicCubemaps.h"
#include "Features/Effects11.h"
#include "Features/IBL.h"
#include "Features/LightLimitFix/ShadowCasterManager.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/Skylighting.h"
#include "Features/SubsurfaceScattering.h"
#include "Features/TerrainBlending.h"
#include "Features/Upscaling.h"
#include "Features/VR.h"
#include "Features/Wind/Wind.h"

#include "Hooks.h"

#ifdef TRACY_ENABLE
#	include <optional>
// GPU zone around the engine's deferred-geometry render (the G-buffer pass that VR stereo
// culling skips Eye 1 pixels in). It spans StartDeferred -> EndDeferred, across the engine's
// RenderBatches calls, so it can't be a scoped macro — open it after StartDeferred sets up the
// MRT/stencil and close it before EndDeferred runs the CS lighting. This is the zone that makes
// the reprojection geometry saving measurable (CS passes are already individually zoned).
static constexpr tracy::SourceLocationData kDeferredGeoSrcLoc{ "Deferred Geometry", "Deferred::StartDeferred", __FILE__, (uint32_t)__LINE__, 0 };
static std::optional<tracy::D3D11ZoneScope> g_deferredGeoZone;
#endif

struct DepthStates
{
	ID3D11DepthStencilState* a[6][40];
};

struct BlendStates
{
	ID3D11BlendState* a[7][2][13][2];

	static BlendStates* GetSingleton()
	{
		static auto blendStates = reinterpret_cast<BlendStates*>(REL::RelocationID(524749, 411364).address());
		return blendStates;
	}
};

void SetupRenderTarget(RE::RENDER_TARGET target, D3D11_TEXTURE2D_DESC texDesc, D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc, D3D11_RENDER_TARGET_VIEW_DESC rtvDesc, D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc, DXGI_FORMAT format, uint bindFlags)
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	texDesc.BindFlags = bindFlags;
	texDesc.Format = format;
	srvDesc.Format = format;
	rtvDesc.Format = format;
	uavDesc.Format = format;

	auto& data = renderer->GetRuntimeData().renderTargets[target];

	if (data.UAV) {
		data.UAV->Release();
		data.UAV = nullptr;
	}
	if (data.RTV) {
		data.RTV->Release();
		data.RTV = nullptr;
	}
	if (data.SRVCopy) {
		data.SRVCopy->Release();
		data.SRVCopy = nullptr;
	}
	if (data.SRV) {
		data.SRV->Release();
		data.SRV = nullptr;
	}
	if (data.textureCopy) {
		data.textureCopy->Release();
		data.textureCopy = nullptr;
	}
	if (data.texture) {
		data.texture->Release();
		data.texture = nullptr;
	}

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, &data.texture));

	if (texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
		DX::ThrowIfFailed(device->CreateShaderResourceView(data.texture, &srvDesc, &data.SRV));

	if (texDesc.BindFlags & D3D11_BIND_RENDER_TARGET)
		DX::ThrowIfFailed(device->CreateRenderTargetView(data.texture, &rtvDesc, &data.RTV));

	if (texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(data.texture, &uavDesc, &data.UAV));
}

void Deferred::SetupResources()
{
	auto renderer = globals::game::renderer;

	{
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.RTV->GetDesc(&rtvDesc);
		main.UAV->GetDesc(&uavDesc);

		// Available targets:
		// MAIN ONLY ALPHA
		// WATER REFLECTIONS
		// BLURFULL_BUFFER
		// LENSFLAREVIS
		// SAO DOWNSCALED
		// SAO CAMERAZ+MIP_LEVEL_0_ESRAM
		// SAO_RAWAO_DOWNSCALED
		// SAO_RAWAO_PREVIOUS_DOWNSCALDE
		// SAO_TEMP_BLUR_DOWNSCALED
		// INDIRECT
		// INDIRECT_DOWNSCALED
		// RAWINDIRECT
		// RAWINDIRECT_DOWNSCALED
		// RAWINDIRECT_PREVIOUS
		// RAWINDIRECT_PREVIOUS_DOWNSCALED
		// RAWINDIRECT_SWAP
		// VOLUMETRIC_LIGHTING_HALF_RES
		// VOLUMETRIC_LIGHTING_BLUR_HALF_RES
		// VOLUMETRIC_LIGHTING_QUARTER_RES
		// VOLUMETRIC_LIGHTING_BLUR_QUARTER_RES
		// TEMPORAL_AA_WATER_1
		// TEMPORAL_AA_WATER_2

		// VR stereo reprojection fills culled Eye 1 G-buffer texels from Eye 0 via a CS
		// reading and writing the same texture (different halves), which needs UAV access.
		uint gbufferBindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		// Only request UAV G-buffers when the fill can run; the bind flag can fail
		// texture creation on GPUs without typed-UAV-load support for these formats.
		if (globals::game::isVR &&
			globals::features::vr.stereoOpt.settings.stereoMode != VRStereoOptimizations::StereoMode::Off &&
			VRStereoOptimizations::SupportsGBufferFill())
			gbufferBindFlags |= D3D11_BIND_UNORDERED_ACCESS;

		// Albedo
		SetupRenderTarget(ALBEDO, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R10G10B10A2_UNORM, gbufferBindFlags);
		// Specular
		SetupRenderTarget(SPECULAR, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R11G11B10_FLOAT, gbufferBindFlags);
		// Reflectance
		SetupRenderTarget(REFLECTANCE, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R11G11B10_FLOAT, gbufferBindFlags);
		// Normal + Roughness
		SetupRenderTarget(NORMALROUGHNESS, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R10G10B10A2_UNORM, gbufferBindFlags);
		// Masks
		SetupRenderTarget(MASKS, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R11G11B10_FLOAT, gbufferBindFlags);
		// Masks2 (vertexAO; fp16 to allow blending)
		SetupRenderTarget(MASKS2, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R16_UNORM, gbufferBindFlags);

		// TAA water history buffers need RGBA16: alpha stores premultiplied coverage for ISWaterBlend
		SetupRenderTarget(RE::RENDER_TARGETS::kWATER_1, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
		SetupRenderTarget(RE::RENDER_TARGETS::kWATER_2, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
	}

	{
		auto device = globals::d3d::device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &linearSampler));
		Util::SetResourceName(linearSampler, "Deferred::LinearSampler");

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointSampler));
		Util::SetResourceName(pointSampler, "Deferred::PointSampler");
	}

	// Directional shadow structured buffer (t98): CPU-written each frame, read-only on GPU.
	// One element holds the sun cascade data uploaded from BSShadowDirectionalLight.
	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(DirectionalShadowLightData);
		sbDesc.ByteWidth = sizeof(DirectionalShadowLightData);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;

		delete directionalShadowLights;
		directionalShadowLights = new Buffer(sbDesc, nullptr, "Deferred::DirectionalShadowLights");
		directionalShadowLights->CreateSRV(srvDesc);
	}
}

void Deferred::ReflectionsPrepasses()
{
	CS_GPU_PASS("Deferred::ReflectionsPrepass");

	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	auto state = globals::state;

	state->activeReflections = true;
	state->UpdateSharedData(false, false);

	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	Feature::ForEachLoadedFeature("ReflectionsPrepass", [](Feature* feature) { feature->ReflectionsPrepass(); }, true);
}

void Deferred::EarlyPrepasses()
{
	CS_GPU_PASS("Deferred::EarlyPrepass");

	if (globals::game::isVR)
		globals::features::vr.stereoOpt.SnapshotFinalDepthHistory(sceneDepthFinal);

	sceneDepthFinal = false;

	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	globals::state->UpdateSharedData(false, true);

	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	// Shadow maps have just been rendered — upload BSShadowDirectionalLight data to t98.
	CopyShadowLightData();

	Feature::ForEachLoadedFeature("EarlyPrepass", [](Feature* feature) { feature->EarlyPrepass(); }, true);
}

void Deferred::PrepassPasses()
{
	CS_GPU_PASS("Deferred::Prepass");

	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	// Run any pending LoadingMenu scene-transition resets on this (render) thread before features
	// iterate their caches, so the clear can't race the menu/Prepass iteration.
	Feature::DrainSceneTransitions();

	Feature::ForEachLoadedFeature("Prepass", [](Feature* feature) { feature->Prepass(); }, true);
}

void Deferred::StartDeferred()
{
	sceneDepthFinal = false;
	if (!globals::state->inWorld)
		return;
	globals::state->UpdateSharedData(true, false);

	auto shadowState = globals::game::shadowState;
	GET_INSTANCE_MEMBER(renderTargets, shadowState)
	GET_INSTANCE_MEMBER(setRenderTargetMode, shadowState)
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

	// Backup original render targets
	for (uint i = 0; i < 4; i++) {
		forwardRenderTargets[i] = renderTargets[i];
	}

	RE::RENDER_TARGET targets[8]{
		RE::RENDER_TARGET::kMAIN,
		RE::RENDER_TARGET::kMOTION_VECTOR,
		NORMALROUGHNESS,
		ALBEDO,
		SPECULAR,
		REFLECTANCE,
		MASKS,
		MASKS2
	};

	for (uint i = 2; i < 8; i++) {
		renderTargets[i] = targets[i];                                             // We must use unused targets to be indexable
		setRenderTargetMode[i] = RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR;  // Dirty from last frame, this calls ClearRenderTargetView once
	}

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	deferredPass = true;

	{
		auto context = globals::d3d::context;

		ID3D11Buffer* buffers[1] = { *globals::game::perFrame.get() };

		ID3D11Buffer* vrBuffer = nullptr;

		if (globals::game::isVR) {
			static REL::Relocation<ID3D11Buffer**> VRValues{ REL::Offset(0x3180688) };
			vrBuffer = *VRValues.get();
		}
		if (vrBuffer) {
			context->CSSetConstantBuffers(12, 1, buffers);
			context->CSSetConstantBuffers(13, 1, &vrBuffer);
		} else {
			context->CSSetConstantBuffers(12, 1, buffers);
		}
	}

	PrepassPasses();

	OverrideBlendStates();

	// Classifies Eye 1 for SSGI/Shadows reuse too, not just VR's own stereo optimization.
	if (globals::game::isVR && globals::features::vr.IsStereoOptimizationDispatchReady()) {
		globals::features::vr.stereoOpt.DispatchStencil();
	}

#ifdef TRACY_ENABLE
	// Bracket the engine deferred-geometry render that follows (closed in EndDeferred).
	if (globals::state->tracyCtx) {
		g_deferredGeoZone.reset();
		g_deferredGeoZone.emplace(globals::state->tracyCtx, &kDeferredGeoSrcLoc, true);
	}
#endif
}

void Deferred::DeferredPasses()
{
	CS_GPU_PASS("Deferred::DeferredPasses");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	{
		ID3D11Buffer* buffers[1] = { *globals::game::perFrame };
		ID3D11Buffer* vrBuffer = nullptr;

		if (globals::game::isVR) {
			static REL::Relocation<ID3D11Buffer**> VRValues{ REL::Offset(0x3180688) };
			vrBuffer = *VRValues.get();
		}
		if (vrBuffer) {
			context->CSSetConstantBuffers(12, 1, buffers);
			context->CSSetConstantBuffers(13, 1, &vrBuffer);
		} else {
			context->CSSetConstantBuffers(12, 1, buffers);
		}
	}

	// VR: geometry rendering is complete — repair the stencil-culled Eye 1 pixels (restore
	// depth + reproject the G-buffer) BEFORE any consumer (SSGI, composite, water) reads
	// them, so downstream passes run unmodified on complete data. No-op when culling is off.
	if (globals::game::isVR)
		globals::features::vr.stereoOpt.RepairCulledEye1();

	auto specular = renderer->GetRuntimeData().renderTargets[SPECULAR];
	auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
	auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto masks = renderer->GetRuntimeData().renderTargets[MASKS];
	auto masks2 = renderer->GetRuntimeData().renderTargets[MASKS2];

	auto main = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[0]];
	auto normals = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[2]];
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto finalDepthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	// Water samples this texture for edge fade and refraction; a partial or
	// dynamic-resolution-sized copy here left it stale and caused hard water intersections.
	context->CopyResource(finalDepthCopy.texture, depth.texture);
	sceneDepthFinal = true;
	auto reflectance = renderer->GetRuntimeData().renderTargets[REFLECTANCE];

	auto motionVectors = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	bool interior = Util::IsInterior();

	auto& skylighting = globals::features::skylighting;

	auto& ssgi = globals::features::screenSpaceGI;
	if (ssgi.loaded)
		ssgi.DrawSSGI();
	auto [ssgi_ao, ssgi_y, ssgi_cocg, ssgi_gi_spec] = ssgi.GetOutputTextures();
	bool ssgi_hq_spec = ssgi.IsSpecularGIActive();

	auto dispatchCount = Util::GetScreenDispatchCount(true);

	auto& sss = globals::features::subsurfaceScattering;
	if (sss.loaded)
		sss.DrawSSS();

	auto& dynamicCubemaps = globals::features::dynamicCubemaps;
	if (dynamicCubemaps.loaded)
		dynamicCubemaps.UpdateCubemap();

	auto& ibl = globals::features::ibl;

	// Deferred Composite
	{
		CS_GPU_PASS("Deferred::DeferredComposite");

		ID3D11ShaderResourceView* srvs[16]{
			specular.SRV,                                                                                    // t0  SpecularTexture
			albedo.SRV,                                                                                      // t1  AlbedoTexture
			normalRoughness.SRV,                                                                             // t2  NormalRoughnessTexture
			masks.SRV,                                                                                       // t3  MasksTexture
			dynamicCubemaps.loaded || globals::game::isVR ? Util::GetCurrentSceneDepthSRV(false) : nullptr,  // t4  DepthTexture (24/32-bit; HLSL type baked at compile via TERRAIN_BLENDING)
			dynamicCubemaps.loaded ? reflectance.SRV : nullptr,                                              // t5  ReflectanceTexture
			dynamicCubemaps.loaded ? dynamicCubemaps.envTexture->srv.get() : nullptr,                        // t6  EnvTexture
			dynamicCubemaps.loaded ? dynamicCubemaps.envReflectionsTexture->srv.get() : nullptr,             // t7  EnvReflectionsTexture
			dynamicCubemaps.loaded && skylighting.loaded ? skylighting.texProbeArray->srv.get() : nullptr,   // t8  SkylightingProbeArray
			masks2.SRV,                                                                                      // t9  Masks2Texture (vertexAO in .x)
			ssgi_ao,                                                                                         // t10 SsgiAoTexture
			ssgi_hq_spec ? nullptr : ssgi_y,                                                                 // t11 SsgiYTexture
			ssgi_hq_spec ? nullptr : ssgi_cocg,                                                              // t12 SsgiCoCgTexture
			ssgi_hq_spec ? ssgi_gi_spec : nullptr,                                                           // t13 SsgiSpecularTexture
			ibl.loaded ? ibl.envIBLTexture->srv.get() : nullptr,                                             // t14 EnvIBLTexture
			ibl.loaded ? ibl.skyIBLTexture->srv.get() : nullptr,                                             // t15 SkyIBLTexture
		};

		if (dynamicCubemaps.loaded)
			context->CSSetSamplers(0, 1, &linearSampler);

		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		// G-buffer fill materializes valid Eye 1 data before this point, so composite
		// lights Eye 1 natively — no mode-texture skip (null SRV reads 0 = MODE_DISOCCLUDED).
		ID3D11ShaderResourceView* modeSRV = nullptr;
		context->CSSetShaderResources(16, 1, &modeSRV);
		ID3D11ShaderResourceView* springDebugSRV = globals::features::wind.GetGrassWindSpringDebugSRV();
		context->CSSetShaderResources(18, 1, &springDebugSRV);

		ID3D11UnorderedAccessView* uavs[3]{ main.UAV, normals.UAV, motionVectors.UAV };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		ID3D11Buffer* sharedBuffers[]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
		context->CSSetConstantBuffers(5, ARRAYSIZE(sharedBuffers), sharedBuffers);

		if (auto* shader = interior ? GetComputeMainCompositeInterior() : GetComputeMainComposite()) {
			context->CSSetShader(shader, nullptr, 0);
			context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
		}

		// Unbind mode texture SRV
		ID3D11ShaderResourceView* nullSRV = nullptr;
		context->CSSetShaderResources(16, 1, &nullSRV);
		context->CSSetShaderResources(18, 1, &nullSRV);
	}

	// VR: Bilateral stereo blend (the reprojection color-overwrite path is gone —
	// Eye 1 is lit natively from the G-buffer fill done before SSGI/composite).
	if (globals::game::isVR) {
		CS_GPU_PASS("VR::StereoBlend");
		globals::features::vr.DrawStereoBlend();
	}

	// Clear
	{
		ID3D11ShaderResourceView* views[16]{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[3]{ nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11Buffer* buffers[1] = { nullptr };
		context->CSSetConstantBuffers(12, 1, buffers);

		context->CSSetShader(nullptr, nullptr, 0);
	}

	if (dynamicCubemaps.loaded)
		dynamicCubemaps.PostDeferred();

#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded)
		globals::features::effects11.DrawVolumetricRays();
#endif
}

void Deferred::EndDeferred()
{
	if (!globals::state->inWorld)
		return;

#ifdef TRACY_ENABLE
	// Close the engine deferred-geometry GPU zone opened in StartDeferred, before CS lighting.
	g_deferredGeoZone.reset();
#endif

	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	auto shadowState = globals::game::shadowState;
	GET_INSTANCE_MEMBER(renderTargets, shadowState)
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

	// Do not render to our targets past this point
	for (uint i = 0; i < 4; i++) {
		renderTargets[i] = forwardRenderTargets[i];
	}

	for (uint i = 4; i < 8; i++) {
		renderTargets[i] = RE::RENDER_TARGET::kNONE;
	}

	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	DeferredPasses();  // Perform deferred passes and composite forward buffers

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	deferredPass = false;

	ResetBlendStates();
}

void Deferred::OverrideBlendStates()
{
	auto blendStates = BlendStates::GetSingleton();

	static std::once_flag setup;
	std::call_once(setup, [&]() {
		auto device = globals::d3d::device;

		for (int a = 0; a < 7; a++) {
			for (int b = 0; b < 2; b++) {
				for (int c = 0; c < 13; c++) {
					for (int d = 0; d < 2; d++) {
						forwardBlendStates[a][b][c][d] = blendStates->a[a][b][c][d];

						if (auto blendState = forwardBlendStates[a][b][c][d]) {
							D3D11_BLEND_DESC blendDesc;
							forwardBlendStates[a][b][c][d]->GetDesc(&blendDesc);

							blendDesc.IndependentBlendEnable = true;

							// Default to original blending method
							for (int i = 1; i < 8; i++) {
								blendDesc.RenderTarget[i].BlendEnable = blendDesc.RenderTarget[0].BlendEnable;
								blendDesc.RenderTarget[i].SrcBlend = blendDesc.RenderTarget[0].SrcBlend;
								blendDesc.RenderTarget[i].DestBlend = blendDesc.RenderTarget[0].DestBlend;
								blendDesc.RenderTarget[i].BlendOp = blendDesc.RenderTarget[0].BlendOp;
								blendDesc.RenderTarget[i].SrcBlendAlpha = blendDesc.RenderTarget[0].SrcBlendAlpha;
								blendDesc.RenderTarget[i].DestBlendAlpha = blendDesc.RenderTarget[0].DestBlendAlpha;
								blendDesc.RenderTarget[i].BlendOpAlpha = blendDesc.RenderTarget[0].BlendOpAlpha;
								blendDesc.RenderTarget[i].RenderTargetWriteMask = blendDesc.RenderTarget[0].RenderTargetWriteMask;
							}

							// Normals and motion vectors must use alpha blending
							for (int i = 1; i < 3; i++) {
								blendDesc.RenderTarget[i].BlendEnable = blendDesc.RenderTarget[0].BlendEnable;
								blendDesc.RenderTarget[i].SrcBlend = D3D11_BLEND_SRC_ALPHA;
								blendDesc.RenderTarget[i].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
								blendDesc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
								blendDesc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
								blendDesc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
								blendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
								blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
							}

							DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[a][b][c][d]));
						} else {
							deferredBlendStates[a][b][c][d] = nullptr;
						}
					}
				}
			}
		}
	});

	// Set modified blend states
	for (int a = 0; a < 7; a++) {
		for (int b = 0; b < 2; b++) {
			for (int c = 0; c < 13; c++) {
				for (int d = 0; d < 2; d++) {
					blendStates->a[a][b][c][d] = deferredBlendStates[a][b][c][d];
				}
			}
		}
	}

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
}

void Deferred::ResetBlendStates()
{
	auto blendStates = BlendStates::GetSingleton();

	// Restore modified blend states
	for (int a = 0; a < 7; a++) {
		for (int b = 0; b < 2; b++) {
			for (int c = 0; c < 13; c++) {
				for (int d = 0; d < 2; d++) {
					blendStates->a[a][b][c][d] = forwardBlendStates[a][b][c][d];
				}
			}
		}
	}

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
}

template <typename T>
void Deferred::SetShadowCascadeParameters(T& lightData, DirectionalShadowLightData& dd)
{
	const auto count = std::min(lightData.shadowmapDescriptors.size(), static_cast<uint32_t>(std::size(dd.ShadowProj)));
	for (uint32_t i = 0; i < count; i++) {
		auto proj = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&lightData.shadowmapDescriptors[i].lightTransform));
		DirectX::XMStoreFloat4x4(&dd.ShadowProj[i], proj);

		DirectX::XMMATRIX invProj = DirectX::XMMatrixInverse(nullptr, proj);
		DirectX::XMStoreFloat4x4(&dd.InvShadowProj[i], invProj);
	}

	// Focus shadow matrices (one per active focus actor; engine writes them
	// to focusShadowmapDescriptors[i].lightTransform during its per-cascade
	// render). The shader samples kSHADOWMAPS slice (4 + i) for each entry
	// to recover the player/NPC high-resolution shadow.
	const auto focusCount = std::min(
		static_cast<uint32_t>(std::size(lightData.focusShadowmapDescriptors)),
		static_cast<uint32_t>(std::size(dd.FocusShadowProj)));
	// Preserve descriptor->slice correspondence by writing FocusShadowProj[i]
	// for descriptor[i] -- the LLF shader samples kSHADOWMAPS slice (4 + fi)
	// using fi as the matrix index, so packing densely (e.g. via a separate
	// counter) would pair matrix N with the wrong shadow slice when there are
	// disabled holes between descriptors. Disabled descriptors leave their
	// FocusShadowProj slot at the default-zero matrix; the shader's existing
	// `focusClip.w <= EPSILON_DIVISION` guard treats that as "no actor in
	// this slice" and skips sampling. FocusShadowCount is the upper iteration
	// bound (last enabled index + 1) so the shader still exits early when
	// trailing slots are empty.
	dd.FocusShadowCount = 0;
	for (uint32_t i = 0; i < focusCount; i++) {
		const auto& desc = lightData.focusShadowmapDescriptors[i];
		if (!desc.isEnabled)
			continue;  // descriptor unused this frame -- leave FocusShadowProj[i] at zero
		auto proj = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&desc.lightTransform));
		DirectX::XMStoreFloat4x4(&dd.FocusShadowProj[i], proj);
		dd.FocusShadowCount = i + 1;
	}
}

void Deferred::CopyShadowLightData()
{
	CS_GPU_PASS("Deferred::CopyShadowLightData");

	auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	if (!shadowSceneNode)
		return;

	auto* sunShadowLight = shadowSceneNode->GetRuntimeData().sunShadowDirLight;
	if (!sunShadowLight)
		return;

	DirectionalShadowLightData dd{};
	auto context = globals::d3d::context;

	auto& dirData = sunShadowLight->GetShadowDirectionalLightRuntimeData();
	dd.EndSplitDistances = float2{ dirData.endSplitDistances[0], dirData.endSplitDistances[1] };
	dd.StartSplitDistances = float2{ dirData.startSplitDistances[0], dirData.startSplitDistances[1] };

	if (globals::game::isVR)
		SetShadowCascadeParameters(sunShadowLight->GetVRRuntimeData(), dd);
	else
		SetShadowCascadeParameters(sunShadowLight->GetRuntimeData(), dd);

	D3D11_MAPPED_SUBRESOURCE mapped{};
	DX::ThrowIfFailed(context->Map(directionalShadowLights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	memcpy(mapped.pData, &dd, sizeof(DirectionalShadowLightData));
	context->Unmap(directionalShadowLights->resource.get(), 0);

	ID3D11ShaderResourceView* srv = directionalShadowLights->srv.get();
	context->PSSetShaderResources(98, 1, &srv);

	// t99: cascade depth array used by LightLimitFix::GetDirectionalShadow for PCF sampling.
	ID3D11ShaderResourceView* cascadeSRV = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	context->PSSetShaderResources(99, 1, &cascadeSRV);
}

void Deferred::ClearShaderCache()
{
	mainCompositeCS.Reset();
	mainCompositeInteriorCS.Reset();
}

ID3D11ComputeShader* Deferred::GetComputeMainComposite()
{
	std::vector<std::pair<const char*, const char*>> defines;

	if (globals::features::dynamicCubemaps.loaded)
		defines.push_back({ "DYNAMIC_CUBEMAPS", nullptr });

	if (globals::features::skylighting.loaded)
		defines.push_back({ "SKYLIGHTING", nullptr });

	if (globals::features::screenSpaceGI.loaded) {
		defines.push_back({ "SSGI", nullptr });
		if (!globals::features::screenSpaceGI.HasGIResources())
			defines.push_back({ "SSGI_AO_ONLY", nullptr });
	}

	if (globals::features::ibl.loaded)
		defines.push_back({ "IBL", nullptr });

	if (globals::game::isVR)
		defines.push_back({ "FRAMEBUFFER", nullptr });

	// TERRAIN_BLENDING flips DepthTexture's HLSL type from `Texture2D<unorm float>`
	// (R24_UNORM_X8_TYPELESS game depth) to `Texture2D<float>` (R32_FLOAT blendedDepth).
	if (globals::features::terrainBlending.loaded)
		defines.push_back({ "TERRAIN_BLENDING", nullptr });

	return mainCompositeCS.Get(L"Data\\Shaders\\DeferredCompositeCS.hlsl", defines, "cs_5_0");
}

ID3D11ComputeShader* Deferred::GetComputeMainCompositeInterior()
{
	std::vector<std::pair<const char*, const char*>> defines;
	defines.push_back({ "INTERIOR", nullptr });

	if (globals::features::dynamicCubemaps.loaded)
		defines.push_back({ "DYNAMIC_CUBEMAPS", nullptr });

	if (globals::features::screenSpaceGI.loaded) {
		defines.push_back({ "SSGI", nullptr });
		if (!globals::features::screenSpaceGI.HasGIResources())
			defines.push_back({ "SSGI_AO_ONLY", nullptr });
	}

	if (globals::features::ibl.loaded)
		defines.push_back({ "IBL", nullptr });

	if (globals::game::isVR)
		defines.push_back({ "FRAMEBUFFER", nullptr });

	// TERRAIN_BLENDING flips DepthTexture's HLSL type from `Texture2D<unorm float>`
	// (R24_UNORM_X8_TYPELESS game depth) to `Texture2D<float>` (R32_FLOAT blendedDepth).
	if (globals::features::terrainBlending.loaded)
		defines.push_back({ "TERRAIN_BLENDING", nullptr });

	return mainCompositeInteriorCS.Get(L"Data\\Shaders\\DeferredCompositeCS.hlsl", defines, "cs_5_0");
}

void Deferred::Hooks::Main_RenderShadowMaps::thunk()
{
	func();
	globals::deferred->EarlyPrepasses();
};

void Deferred::Hooks::Main_RenderWorld::thunk(bool a1)
{
	auto* const state = globals::state;
	state->permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::InWorld);
	state->inWorld = true;
	state->worldRenderedThisFrame = true;
	func(a1);

	state->inWorld = false;
	state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::InWorld);
};

void Deferred::Hooks::Main_RenderWorld_Start::thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
{
	if (globals::shaderCache->IsEnabled() && globals::state->inWorld) {
		// Here is where the first opaque objects start rendering
		globals::deferred->StartDeferred();
	}

	func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);  // RenderBatches
};

void Deferred::Hooks::Main_RenderWorld_BlendedDecals::thunk(RE::BSShaderAccumulator* This, uint32_t RenderFlags)
{
	auto deferred = globals::deferred;

	if (globals::shaderCache->IsEnabled() && globals::state->inWorld) {
		auto& terrainBlending = globals::features::terrainBlending;
		// Defer terrain rendering until after everything else
		if (terrainBlending.loaded && terrainBlending.settings.Enabled) {
			terrainBlending.RenderTerrainBlendingPasses();
		}
	}

	// Deferred blended decals

	func(This, RenderFlags);

	deferred->EndDeferred();
};

void Deferred::Hooks::BSCubeMapCamera_RenderCubemap::thunk(RE::NiAVObject* camera, int a2, bool a3, bool a4, bool a5)
{
	auto deferred = globals::deferred;
	auto state = globals::state;

	deferred->ReflectionsPrepasses();
	state->permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections);
	func(camera, a2, a3, a4, a5);
	state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections);
}

void Deferred::Hooks::Main_RenderFirstPersonView::thunk(bool a1, bool a2)
{
	auto* const state = globals::state;
	state->permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::InWorld);
	func(a1, a2);
	state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::InWorld);
}

void Deferred::Hooks::Renderer_ResetState::thunk(void* This)
{
	func(This);

	auto* const state = globals::state;
	auto* const context = globals::d3d::context;

	ID3D11Buffer* buffers[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
	context->PSSetConstantBuffers(4, 3, buffers);
	context->CSSetConstantBuffers(5, 2, buffers + 1);
}
