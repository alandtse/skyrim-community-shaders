#include "VRStereoOptimizations.h"

#include "Deferred.h"
#include "ExtendedMaterials.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <imgui.h>

// JSON enum serialization for StereoMode
NLOHMANN_JSON_SERIALIZE_ENUM(VRStereoOptimizations::StereoMode, {
																	{ VRStereoOptimizations::StereoMode::Off, "Off" },
																	{ VRStereoOptimizations::StereoMode::Enable, "Enable" },
																})

//=============================================================================
// SETTINGS MANAGEMENT
//=============================================================================

void VRStereoOptimizations::SaveSettings(json& o_json)
{
	o_json["StereoMode"] = settings.stereoMode;
	o_json["DisocclusionDepthThreshold"] = settings.disocclusionDepthThreshold;
	o_json["EdgeDepthThreshold"] = settings.edgeDepthThreshold;
	o_json["MinEdgeDistance"] = settings.minEdgeDistance;
	o_json["FullBlendDistance"] = settings.fullBlendDistance;
	o_json["FoveatedRegionRadius"] = settings.foveatedRegionRadius;
	o_json["FoveatedRegionCenterX"] = settings.foveatedRegionCenterX;
	o_json["FoveatedRegionCenterY"] = settings.foveatedRegionCenterY;
	o_json["UseEyeTracking"] = settings.useEyeTracking;
	o_json["DebugSkipMerge"] = settings.debugSkipMerge;
	o_json["DebugDepthMap"] = settings.debugDepthMap;
	o_json["DirectionalOcclusionRatio"] = settings.directionalOcclusionRatio;
}

void VRStereoOptimizations::LoadSettings(json& o_json)
{
	auto loadClampedFloat = [&](const char* key, float& dst, float lo, float hi) {
		if (auto it = o_json.find(key); it != o_json.end() && it->is_number())
			dst = std::clamp(it->get<float>(), lo, hi);
	};
	auto loadBool = [&](const char* key, bool& dst) {
		if (auto it = o_json.find(key); it != o_json.end() && it->is_boolean())
			dst = it->get<bool>();
	};

	if (o_json.contains("StereoMode"))
		settings.stereoMode = o_json["StereoMode"].get<StereoMode>();

	loadClampedFloat("DisocclusionDepthThreshold", settings.disocclusionDepthThreshold, 0.001f, 0.1f);
	loadClampedFloat("EdgeDepthThreshold", settings.edgeDepthThreshold, 0.0f, 1.0f);
	loadClampedFloat("MinEdgeDistance", settings.minEdgeDistance, 0.0f, 50000.0f);
	loadClampedFloat("FoveatedRegionRadius", settings.foveatedRegionRadius, 0.0f, 1.0f);
	loadClampedFloat("FoveatedRegionCenterX", settings.foveatedRegionCenterX, 0.0f, 1.0f);
	loadClampedFloat("FoveatedRegionCenterY", settings.foveatedRegionCenterY, 0.0f, 1.0f);
	loadClampedFloat("FullBlendDistance", settings.fullBlendDistance, 0.0f, 50000.0f);
	// ForwardOcclusionScale (shipped v2.4.0+) tuned an inverted, effectively-broken formula (see
	// af5aadb45); its saved values carry no valid meaning under the corrected formula, so we
	// intentionally reset to the new default instead of migrating a stale number.
	if (!o_json.contains("DirectionalOcclusionRatio") && o_json.contains("ForwardOcclusionScale"))
		logger::info("[VR] ForwardOcclusionScale is obsolete; resetting DirectionalOcclusionRatio to default {}", settings.directionalOcclusionRatio);
	loadClampedFloat("DirectionalOcclusionRatio", settings.directionalOcclusionRatio, 0.0f, 1.0f);

	loadBool("UseEyeTracking", settings.useEyeTracking);
	loadBool("DebugSkipMerge", settings.debugSkipMerge);
	loadBool("DebugDepthMap", settings.debugDepthMap);
}

void VRStereoOptimizations::RestoreDefaultSettings()
{
	settings = {};
}

//=============================================================================
// RESOURCE SETUP
//=============================================================================

bool VRStereoOptimizations::SupportsGBufferFill()
{
	return State::SupportsTypedUAVLoad(DXGI_FORMAT_R16G16B16A16_FLOAT) &&
	       State::SupportsTypedUAVLoad(DXGI_FORMAT_R16G16_FLOAT) &&
	       State::SupportsTypedUAVLoad(DXGI_FORMAT_R10G10B10A2_UNORM) &&
	       State::SupportsTypedUAVLoad(DXGI_FORMAT_R11G11B10_FLOAT) &&
	       State::SupportsTypedUAVLoad(DXGI_FORMAT_R16_UNORM);
}

void VRStereoOptimizations::SetupResources()
{
	if (!globals::game::isVR)
		return;

	auto device = globals::d3d::device;
	auto renderer = globals::game::renderer;

	// Constant buffers
	paramsCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VRStereoOptParams>(), "VRStereoOpt::ParamsCB");

	// Get main RT dimensions for per-eye calculations
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC mainDesc;
	main.texture->GetDesc(&mainDesc);

	// Per-pixel mode texture (R8_UINT, full SBS resolution = both eyes)
	{
		D3D11_TEXTURE2D_DESC modeDesc{};
		modeDesc.Width = mainDesc.Width;
		modeDesc.Height = mainDesc.Height;
		modeDesc.MipLevels = 1;
		modeDesc.ArraySize = 1;
		modeDesc.Format = DXGI_FORMAT_R8_UINT;
		modeDesc.SampleDesc.Count = 1;
		modeDesc.SampleDesc.Quality = 0;
		modeDesc.Usage = D3D11_USAGE_DEFAULT;
		modeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		modeDesc.CPUAccessFlags = 0;
		modeDesc.MiscFlags = 0;

		texPerPixelMode = eastl::make_unique<Texture2D>(modeDesc, "VRStereoOpt::PerPixelMode");
		texPerPixelMode->CreateSRV(D3D11_SHADER_RESOURCE_VIEW_DESC{
			.Format = DXGI_FORMAT_R8_UINT,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
		texPerPixelMode->CreateUAV(D3D11_UNORDERED_ACCESS_VIEW_DESC{
			.Format = DXGI_FORMAT_R8_UINT,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 } });
	}

	// Depth-stencil state for stencil write pass:
	// Depth test OFF (not rendering geometry), depth writes OFF, stencil ALWAYS + REPLACE with ref=1.
	// We use the normal (writable) kMAIN DSV — no simultaneous SRV binding needed.
	{
		D3D11_DEPTH_STENCIL_DESC dssDesc{};
		dssDesc.DepthEnable = FALSE;
		dssDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dssDesc.StencilEnable = TRUE;
		dssDesc.StencilReadMask = 0xFF;
		dssDesc.StencilWriteMask = 0xFF;
		dssDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		dssDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		dssDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		dssDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		dssDesc.BackFace = dssDesc.FrontFace;

		DX::ThrowIfFailed(device->CreateDepthStencilState(&dssDesc, stencilWriteDSS.put()));
		Util::SetResourceName(stencilWriteDSS.get(), "VRStereoOpt::StencilWriteDSS");
	}

	// Depth-stencil state for the depth fill pass:
	// depth ALWAYS + write ALL restores depth on the masked pixels; stencil read-only
	// EQUAL ref=1 hardware-masks the fullscreen triangle to stencil-culled pixels only.
	{
		D3D11_DEPTH_STENCIL_DESC dssDesc{};
		dssDesc.DepthEnable = TRUE;
		dssDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dssDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		dssDesc.StencilEnable = TRUE;
		dssDesc.StencilReadMask = 0xFF;
		dssDesc.StencilWriteMask = 0x00;
		dssDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		dssDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		dssDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		dssDesc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
		dssDesc.BackFace = dssDesc.FrontFace;

		DX::ThrowIfFailed(device->CreateDepthStencilState(&dssDesc, depthFillDSS.put()));
		Util::SetResourceName(depthFillDSS.get(), "VRStereoOpt::DepthFillDSS");
	}

	// Rasterizer state for stencil write: no culling, no depth clip
	{
		D3D11_RASTERIZER_DESC rsDesc{};
		rsDesc.FillMode = D3D11_FILL_SOLID;
		rsDesc.CullMode = D3D11_CULL_NONE;
		rsDesc.DepthClipEnable = FALSE;

		DX::ThrowIfFailed(device->CreateRasterizerState(&rsDesc, stencilWriteRS.put()));
		Util::SetResourceName(stencilWriteRS.get(), "VRStereoOpt::StencilWriteRS");
	}

	// GBufferFillCS does typed UAV loads on these formats; without support the reads
	// return undefined data, so disable the feature (CanDispatchStencil gates on this).
	gBufferFillSupported = SupportsGBufferFill();
	if (!gBufferFillSupported)
		logger::warn("[VRStereoOptimizations] GPU lacks typed-UAV-load support for G-buffer formats; stereo reprojection disabled.");

	CompileShaders();

	logger::info("[VRStereoOptimizations] Resources created: mode tex {}x{} (full SBS)", mainDesc.Width, mainDesc.Height);
}

void VRStereoOptimizations::CompileShaders()
{
	std::vector<std::pair<const char*, const char*>> csDefines = {
		{ "VR", nullptr },
		{ "FRAMEBUFFER", nullptr }
	};

	std::vector<std::pair<const char*, const char*>> vspsDefines = {
		{ "VR", nullptr }
	};

	if (auto* ptr = Util::CompileShader(L"Data\\Shaders\\VRStereoOptimizations\\StencilCS.hlsl", csDefines, "cs_5_0"))
		stencilCS.attach(reinterpret_cast<ID3D11ComputeShader*>(ptr));
	else
		logger::error("[VRStereoOptimizations] Failed to compile StencilCS");

	{
		auto debugDefines = csDefines;
		debugDefines.push_back({ "DEBUG_DEPTH_MAP", nullptr });
		if (auto* ptr = Util::CompileShader(L"Data\\Shaders\\VRStereoOptimizations\\StencilCS.hlsl", debugDefines, "cs_5_0"))
			stencilDebugDepthMapCS.attach(reinterpret_cast<ID3D11ComputeShader*>(ptr));
		else
			logger::error("[VRStereoOptimizations] Failed to compile StencilCS (DEBUG_DEPTH_MAP)");
	}

	if (auto* ptr = Util::CompileShader(L"Data\\Shaders\\VRStereoOptimizations\\StencilWriteVS.hlsl", vspsDefines, "vs_5_0"))
		stencilWriteVS.attach(reinterpret_cast<ID3D11VertexShader*>(ptr));
	else
		logger::error("[VRStereoOptimizations] Failed to compile StencilWriteVS");

	if (auto* ptr = Util::CompileShader(L"Data\\Shaders\\VRStereoOptimizations\\StencilWritePS.hlsl", vspsDefines, "ps_5_0"))
		stencilWritePS.attach(reinterpret_cast<ID3D11PixelShader*>(ptr));
	else
		logger::error("[VRStereoOptimizations] Failed to compile StencilWritePS");

	if (auto* ptr = Util::CompileShader(L"Data\\Shaders\\VRStereoOptimizations\\DepthFillPS.hlsl", vspsDefines, "ps_5_0"))
		depthFillPS.attach(reinterpret_cast<ID3D11PixelShader*>(ptr));
	else
		logger::error("[VRStereoOptimizations] Failed to compile DepthFillPS");

	if (auto* ptr = Util::CompileShader(L"Data\\Shaders\\VRStereoOptimizations\\GBufferFillCS.hlsl", csDefines, "cs_5_0"))
		gBufferFillCS.attach(reinterpret_cast<ID3D11ComputeShader*>(ptr));
	else
		logger::error("[VRStereoOptimizations] Failed to compile GBufferFillCS");
}

void VRStereoOptimizations::ClearShaderCache()
{
	stencilCS = nullptr;
	gBufferFillCS = nullptr;
	stencilDebugDepthMapCS = nullptr;
	stencilWriteVS = nullptr;
	stencilWritePS = nullptr;
	depthFillPS = nullptr;
	dssCache.clear();

	// Framework clears caches without a follow-up SetupResources; without an immediate
	// recompile CanDispatchStencil() stays false and the feature silently disengages.
	if (loaded)
		CompileShaders();
}

void VRStereoOptimizations::Reset()
{
	stencilActive = false;
	stencilSwapCount = 0;
}

//=============================================================================
// IMGUI SETTINGS
//=============================================================================

void VRStereoOptimizations::DrawSettings()
{
	const char* modeNames[] = { T("feature.vr_stereo.off", "Off"), T("feature.vr_stereo.enable", "Enable") };
	int currentMode = static_cast<int>(settings.stereoMode);
	if (ImGui::Combo(T("feature.vr_stereo.enable_stereo_reprojection", "Enable Stereo Reprojection"), &currentMode, modeNames, IM_ARRAYSIZE(modeNames)))
		settings.stereoMode = static_cast<StereoMode>(currentMode);
	Util::AddTooltip(T("feature.vr_stereo.enable_stereo_reprojection_tooltip", "Reprojects Eye 0 (left) pixels into Eye 1 (right) using depth and motion data,\nskipping redundant full shading where the views overlap.\nReduces GPU cost in VR by shading each pixel fewer times per frame."));

	if (globals::game::isVR)
		Util::UI::DrawSettingDiff(bootSnapshot, settings, &Settings::stereoMode);
	if (settings.stereoMode == StereoMode::Off)
		return;

	ImGui::SliderFloat(T("feature.vr_stereo.disocclusion_depth_threshold", "Disocclusion Depth Threshold"), &settings.disocclusionDepthThreshold, 0.001f, 0.1f, "%.4f");

	ImGui::SliderFloat(T("feature.vr_stereo.directional_occlusion_ratio", "Directional Occlusion Ratio"), &settings.directionalOcclusionRatio, 0.0f, 1.0f, "%.2f");
	Util::AddTooltip(T("feature.vr_stereo.directional_occlusion_ratio_tooltip", "Catches silhouette edges a plain depth-match check misses.\nFires when Eye 0 depth is less than this fraction of Eye 1 depth (e.g. 0.9 = Eye 0 more than 10% closer).\nHigher = more aggressive. 0 = disabled."));

	if (globals::state->IsDeveloperMode()) {
		if (ImGui::TreeNode(T("feature.vr_stereo.debug", "Debug"))) {
			ImGui::SliderFloat(T("feature.vr_stereo.full_blend_distance", "Full Blend Distance"), &settings.fullBlendDistance, 0.0f, 10000.0f, "%.0f");
			Util::AddTooltip(T("feature.vr_stereo.full_blend_distance_tooltip", "Geometry closer than this distance (game units) is excluded from culling and rendered natively in both eyes. 0 = disabled."));

			ImGui::Checkbox(T("feature.vr_stereo.skip_pixel_reprojection", "Skip Pixel Reprojection"), &settings.debugSkipMerge);
			ImGui::Text(T("feature.vr_stereo.stencil_swaps_this_frame", "Stencil swaps this frame: %u"), stencilSwapCount);
			ImGui::TreePop();
		}
	}
}

//=============================================================================
// CONSTANT BUFFER UPDATE
//=============================================================================

json VRStereoOptimizations::GetDiagnostics() const
{
	return json{
		{ "classifiedWidth", static_cast<uint32_t>(frameDim.x) },
		{ "classifiedHeight", static_cast<uint32_t>(frameDim.y) },
		{ "modeTextureWidth", texPerPixelMode ? texPerPixelMode->desc.Width : 0u },
		{ "modeTextureHeight", texPerPixelMode ? texPerPixelMode->desc.Height : 0u },
		{ "stencilActive", stencilActive },
		{ "classifiedThisFrame", classifiedThisFrame },
	};
}

void VRStereoOptimizations::UpdateConstantBuffer()
{
	frameDim = Util::ConvertToDynamic(globals::state->screenSize);

	VRStereoOptParams params{};
	params.FrameDim[0] = frameDim.x;
	params.FrameDim[1] = frameDim.y;
	params.RcpFrameDim[0] = 1.0f / frameDim.x;
	params.RcpFrameDim[1] = 1.0f / frameDim.y;
	params.StereoModeValue = static_cast<uint32_t>(settings.stereoMode);
	params.DisocclusionThreshold = settings.disocclusionDepthThreshold;
	params.EdgeDepthThreshold = settings.edgeDepthThreshold;
	params.FoveatedRadius = settings.foveatedRegionRadius;
	params.FoveatedCenter[0] = settings.foveatedRegionCenterX;
	params.FoveatedCenter[1] = settings.foveatedRegionCenterY;
	params.MinEdgeDistance = settings.minEdgeDistance;
	params.FullBlendDistance = settings.fullBlendDistance;
	params.DirectionalOcclusionRatio = settings.directionalOcclusionRatio;

	paramsCB->Update(params);
}

//=============================================================================
// PHASE 1: STENCIL CLASSIFICATION + WRITE
//=============================================================================

void VRStereoOptimizations::DispatchStencil()
{
	classifiedThisFrame = false;

	if (!globals::game::isVR || !CanClassify())
		return;

	ZoneScoped;
	CS_GPU_PASS("VRStereoOpt::Stencil");

	auto context = globals::d3d::context;

	UpdateConstantBuffer();
	auto cbPtr = paramsCB->CB();
	// Use the same depth source as the rest of the deferred pipeline.
	// kMAIN.depthSRV is unpopulated at StartDeferred time (z-prepass has not written to it yet).
	// GetCurrentSceneDepthSRV() returns TerrainBlending's blended depth when active, or
	// kPOST_ZPREPASS_COPY otherwise — both have valid z-prepass data by this point.
	auto* depthSRV = Util::GetCurrentSceneDepthSRV();
	if (!depthSRV) {
		logger::warn("[VRStereoOptimizations] DispatchStencil: depthSRV is null, skipping");
		return;
	}

	{
		CS_GPU_PASS("StereoOpt::ModeClassify");

		{
			CS_GPU_PASS("StereoOpt::ModeClassifyBind");

			ID3D11ShaderResourceView* srvs[1]{ depthSRV };
			ID3D11UnorderedAccessView* uavs[1]{ texPerPixelMode->uav.get() };

			context->CSSetConstantBuffers(1, 1, &cbPtr);
			context->CSSetShaderResources(0, 1, srvs);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			auto* activeStencilCS = (settings.debugDepthMap && stencilDebugDepthMapCS) ? stencilDebugDepthMapCS.get() : stencilCS.get();
			context->CSSetShader(activeStencilCS, nullptr, 0);
		}

		{
			CS_GPU_PASS("StereoOpt::ModeClassifyDispatch");

			uint32_t fullWidth = texPerPixelMode->desc.Width;
			uint32_t fullHeight = texPerPixelMode->desc.Height;
			context->Dispatch((fullWidth + 7) / 8, (fullHeight + 7) / 8, 1);
		}

		// Cleanup CS bindings
		ID3D11ShaderResourceView* nullSRV = nullptr;
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ID3D11Buffer* nullCB = nullptr;
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetConstantBuffers(1, 1, &nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	classifiedThisFrame = true;

	// Only stereoMode being on culls Eye 1; other consumers just read the mode texture above.
	if (CanDispatchStencil()) {
		CS_GPU_PASS("StereoOpt::StencilWrite");
		ExecuteStencilWritePass();
		stencilActive = true;
		stencilSwapCount = 0;
	} else {
		stencilActive = false;
	}
}

void VRStereoOptimizations::SetEye1Viewport()
{
	const auto eyeWidth = static_cast<uint32_t>(frameDim.x) / 2;

	D3D11_VIEWPORT vp{};
	vp.TopLeftX = static_cast<float>(eyeWidth);
	vp.TopLeftY = 0.0f;
	vp.Width = static_cast<float>(eyeWidth);
	vp.Height = static_cast<float>(static_cast<uint32_t>(frameDim.y));
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	globals::d3d::context->RSSetViewports(1, &vp);
}

void VRStereoOptimizations::ExecuteStencilWritePass()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	// Snapshot + restore the full pipeline state this fullscreen pass clobbers
	// (incl. PS constant-buffer slot 1, the params CB this pass binds).
	Util::FullscreenPassScope scope(context);

	// ===== SET UP STENCIL WRITE PASS =====

	// Clear stencil buffer to 0 before writing classification.
	// The engine's z-prepass may have written stencil values for rendered geometry.
	// Without this clear, non-discarded pixels in StencilWritePS could inherit engine stencil
	// values that match our NOT_EQUAL ref=1 culling test and incorrectly skip geometry pixels.
	// StencilWritePS no longer binds a depth SRV, so we can use the normal writable DSV here.
	{
		auto& depthData = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		context->ClearDepthStencilView(depthData.views[0], D3D11_CLEAR_STENCIL, 1.0f, 0);
	}

	// Use the normal DSV for stencil writes — no depth SRV is bound simultaneously,
	// so there is no D3D11 resource hazard and stencil writes are not suppressed.
	auto& depthData = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	context->OMSetRenderTargets(0, nullptr, depthData.views[0]);
	context->OMSetDepthStencilState(stencilWriteDSS.get(), 1);
	context->RSSetState(stencilWriteRS.get());

	SetEye1Viewport();

	// Bind shaders and mode texture
	context->VSSetShader(stencilWriteVS.get(), nullptr, 0);
	context->PSSetShader(stencilWritePS.get(), nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);

	ID3D11ShaderResourceView* modeSRV = texPerPixelMode->srv.get();
	context->PSSetShaderResources(0, 1, &modeSRV);

	// Bind params CB to pixel shader (CS and PS have separate CB bindings)
	auto cbPtr = paramsCB->CB();
	context->PSSetConstantBuffers(1, 1, &cbPtr);

	// Fullscreen triangle: no VB/IB, procedurally generated in VS
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	context->Draw(3, 0);

	// Pipeline state restored by `scope` dtor.
}

//=============================================================================
// DSS CACHE: CLONE + STENCIL NOT_EQUAL ENFORCEMENT
//=============================================================================

ID3D11DepthStencilState* VRStereoOptimizations::GetOrCreateModifiedDSS(ID3D11DepthStencilState* originalDSS)
{
	if (!stencilActive)
		return originalDSS;

	// Check cache (nullptr is a valid key — represents D3D11 default state)
	if (auto it = dssCache.find(originalDSS); it != dssCache.end())
		return it->second.get();

	D3D11_DEPTH_STENCIL_DESC desc;
	if (originalDSS) {
		originalDSS->GetDesc(&desc);
	} else {
		// D3D11 default state: depth enabled, stencil disabled
		desc = {};
		desc.DepthEnable = TRUE;
		desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		desc.DepthFunc = D3D11_COMPARISON_LESS;
		desc.StencilEnable = FALSE;
		desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
		desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
		desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		desc.BackFace = desc.FrontFace;
	}

	desc.StencilEnable = TRUE;
	desc.StencilReadMask = 0xFF;
	desc.StencilWriteMask = 0x00;

	desc.FrontFace.StencilFunc = D3D11_COMPARISON_NOT_EQUAL;
	desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
	desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	desc.BackFace = desc.FrontFace;

	winrt::com_ptr<ID3D11DepthStencilState> modifiedDSS;
	HRESULT hr = globals::d3d::device->CreateDepthStencilState(&desc, modifiedDSS.put());
	if (FAILED(hr)) {
		logger::warn("[VRStereoOptimizations] Failed to create modified DSS (HRESULT: {:#x})", static_cast<uint32_t>(hr));
		return originalDSS;
	}

	auto* result = modifiedDSS.get();
	dssCache[originalDSS] = std::move(modifiedDSS);

	return result;
}
void VRStereoOptimizations::DeactivateStencil()
{
	if (!stencilActive)
		return;
	logger::trace("[VRStereoOptimizations] Frame: stencilSwapCount={}", stencilSwapCount);
	stencilActive = false;
}

void VRStereoOptimizations::RepairCulledEye1()
{
	if (!stencilActive)
		return;

	// Order is load-bearing: DeactivateStencil must precede the depth fill so the
	// OMSetDepthStencilState hook stops swapping in the NOT_EQUAL clone and the fill's
	// own EQUAL-ref=1 DSS survives. No engine draw or stencil clear may run between
	// these steps, or the stencil mask is lost — hence they live in one method.
	DeactivateStencil();
	ExecuteDepthFillPass();
	DispatchGBufferFill();
}

void VRStereoOptimizations::ExecuteDepthFillPass()
{
	if (!depthFillPS || !stencilWriteVS || !depthFillDSS || !stencilWriteRS)
		return;

	auto* depthSRV = Util::GetCurrentSceneDepthSRV();
	if (!depthSRV)
		return;

	ZoneScoped;
	CS_GPU_PASS("VRStereoOpt::DepthFill");

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	// Snapshot + restore the full pipeline state this fullscreen pass clobbers.
	Util::FullscreenPassScope scope(context);

	// ===== DEPTH FILL PASS =====

	auto& depthData = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	context->OMSetRenderTargets(0, nullptr, depthData.views[0]);
	context->OMSetDepthStencilState(depthFillDSS.get(), 1);
	context->RSSetState(stencilWriteRS.get());

	SetEye1Viewport();

	context->VSSetShader(stencilWriteVS.get(), nullptr, 0);
	context->PSSetShader(depthFillPS.get(), nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShaderResources(0, 1, &depthSRV);

	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	context->Draw(3, 0);

	// Pipeline state restored by `scope` dtor.
}

void VRStereoOptimizations::DispatchGBufferFill()
{
	if (!gBufferFillCS || !texPerPixelMode || !paramsCB)
		return;

	auto* depthSRV = Util::GetCurrentSceneDepthSRV();
	if (!depthSRV)
		return;

	ZoneScoped;
	CS_GPU_PASS("VRStereoOpt::GBufferFill");

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& rt = renderer->GetRuntimeData().renderTargets;

	// The deferred MRT set is bound as RTVs at this point; UAV binds on the same
	// textures would be dropped by the runtime. Unbind render targets first.
	ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* savedDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
	context->OMSetRenderTargets(0, nullptr, nullptr);

	// paramsCB already uploaded this frame by DispatchStencil (settings/resolution are
	// frame-constant); no re-upload needed.
	auto cbPtr = paramsCB->CB();

	ID3D11ShaderResourceView* srvs[2]{ depthSRV, texPerPixelMode->srv.get() };
	ID3D11UnorderedAccessView* uavs[8]{
		rt[RE::RENDER_TARGETS::kMAIN].UAV,
		rt[RE::RENDER_TARGETS::kMOTION_VECTOR].UAV,
		rt[NORMALROUGHNESS].UAV,
		rt[ALBEDO].UAV,
		rt[SPECULAR].UAV,
		rt[REFLECTANCE].UAV,
		rt[MASKS].UAV,
		rt[MASKS2].UAV,
	};

	context->CSSetConstantBuffers(1, 1, &cbPtr);
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
	context->CSSetShader(gBufferFillCS.get(), nullptr, 0);

	uint32_t eyeWidth = texPerPixelMode->desc.Width / 2;
	uint32_t height = texPerPixelMode->desc.Height;
	context->Dispatch((eyeWidth + 7) / 8, (height + 7) / 8, 1);

	ID3D11ShaderResourceView* nullSRVs[2] = {};
	ID3D11UnorderedAccessView* nullUAVs[8] = {};
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShaderResources(0, 2, nullSRVs);
	context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
	context->CSSetConstantBuffers(1, 1, &nullCB);
	context->CSSetShader(nullptr, nullptr, 0);

	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
	for (auto& rtv : savedRTVs) {
		if (rtv)
			rtv->Release();
	}
	if (savedDSV)
		savedDSV->Release();
}
