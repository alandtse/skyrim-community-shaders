#include "Features/VR.h"

#include "Deferred.h"
#include "Features/DynamicCubemaps.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"

void VR::CompileStereoBlendShaders()
{
	std::vector<std::pair<const char*, const char*>> defines = { { "VR", "" }, { "FRAMEBUFFER", "" } };
	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\VR\\StereoBlendCS.hlsl", defines, "cs_5_0")))
		stereoBlendCS.attach(rawPtr);

	auto backCheckDefines = defines;
	backCheckDefines.push_back({ "DEBUG_BACKCHECK", "" });
	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\VR\\StereoBlendCS.hlsl", backCheckDefines, "cs_5_0")))
		stereoBlendDebugBackCheckCS.attach(rawPtr);

	auto blendWeightDefines = defines;
	blendWeightDefines.push_back({ "DEBUG_BLEND_WEIGHT", "" });
	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\VR\\StereoBlendCS.hlsl", blendWeightDefines, "cs_5_0")))
		stereoBlendDebugBlendWeightCS.attach(rawPtr);

	auto edgeDetectionDefines = defines;
	edgeDetectionDefines.push_back({ "DEBUG_EDGE_DETECTION", "" });
	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\VR\\StereoBlendCS.hlsl", edgeDetectionDefines, "cs_5_0")))
		stereoBlendDebugEdgeDetectionCS.attach(rawPtr);
}

void VR::ClearShaderCache()
{
	dynamicNearClip.ClearShaderCache();
	stereoBlendCS = nullptr;
	stereoBlendDebugBackCheckCS = nullptr;
	stereoBlendDebugBlendWeightCS = nullptr;
	stereoBlendDebugEdgeDetectionCS = nullptr;
	stereoOpt.ClearShaderCache();

	// Framework calls ClearShaderCache without a follow-up SetupResources for these runtime
	// CS shaders, so recompile here to leave the feature in a usable state.
	CompileStereoBlendShaders();
}

bool VR::AnyScreenSpaceEffectLoaded()
{
	return globals::features::screenSpaceGI.loaded ||
	       globals::features::dynamicCubemaps.loaded ||
	       globals::features::screenSpaceShadows.loaded;
}

void VR::DrawStereoBlend()
{
	// Post-composite stereo-consistency bilateral blend (reduces per-eye disparity for
	// screen-space effects). The retired reprojection color-overwrite path is gone —
	// Eye 1 is now lit natively via the G-buffer fill.
	if (!globals::game::isVR || !stereoBlendCopyTex || !stereoBlendCB)
		return;

	// Debug modes 2-4 must run even with blend disabled (else a persisted config renders
	// nothing); mode 1 (Coverage) needs no Stereo Blend at all. Re-checks Developer Mode
	// here since ReprojectDebugMode persists across sessions independent of that toggle.
	const bool stereoBlendDebugActive = settings.ReprojectDebugMode >= 2 && settings.ReprojectDebugMode <= 4 && globals::state->IsDeveloperMode();
	if ((!settings.EnableStereoBlend && !stereoBlendDebugActive) || !stereoBlendCS)
		return;

	if (!AnyScreenSpaceEffectLoaded() && !globals::state->IsDeveloperMode())
		return;

	ZoneScoped;
	CS_GPU_PASS("VR::StereoBlend");

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto* depthSRV = Util::GetCurrentSceneDepthSRV();

	// Copy main color to read-only texture to avoid read/write race between eyes
	context->CopyResource(stereoBlendCopyTex->resource.get(), main.texture);

	auto dispatchCount = Util::GetScreenDispatchCount(true);
	float2 resolution = Util::ConvertToDynamic(globals::state->screenSize);

	StereoBlendCB cbData{};
	cbData.FrameDim[0] = resolution.x;
	cbData.FrameDim[1] = resolution.y;
	cbData.RcpFrameDim[0] = 1.0f / resolution.x;
	cbData.RcpFrameDim[1] = 1.0f / resolution.y;
	cbData.DepthSigma = settings.StereoBlendDepthSigma;
	cbData.MaxBlendFactor = settings.StereoBlendMaxFactor;
	cbData.ColorDiffThreshold = settings.StereoBlendColorThreshold;

	ID3D11ComputeShader* activeCS = stereoBlendCS.get();
	switch (settings.ReprojectDebugMode) {
	case 2:
		if (stereoBlendDebugBackCheckCS)
			activeCS = stereoBlendDebugBackCheckCS.get();
		break;
	case 3:
		if (stereoBlendDebugBlendWeightCS)
			activeCS = stereoBlendDebugBlendWeightCS.get();
		break;
	case 4:
		if (stereoBlendDebugEdgeDetectionCS)
			activeCS = stereoBlendDebugEdgeDetectionCS.get();
		break;
	default:
		break;
	}

	stereoBlendCB->Update(cbData);
	auto cbPtr = stereoBlendCB->CB();

	ID3D11ShaderResourceView* srvs[2]{ stereoBlendCopyTex->srv.get(), depthSRV };
	context->CSSetConstantBuffers(1, 1, &cbPtr);
	context->CSSetShaderResources(0, 2, srvs);

	ID3D11UnorderedAccessView* uavs[1]{ main.UAV };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(activeCS, nullptr, 0);
	{
		CS_GPU_PASS("StereoBlend::Bilateral");
		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
	}

	// Cleanup
	ID3D11ShaderResourceView* nullSRVs[2] = {};
	context->CSSetShaderResources(0, 2, nullSRVs);
	ID3D11UnorderedAccessView* nullUAVs[1] = {};
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetConstantBuffers(1, 1, &nullCB);
	context->CSSetShader(nullptr, nullptr, 0);
}
