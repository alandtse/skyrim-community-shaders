#include "VR.h"
#include "Menu.h"
#include "RE/B/BSOpenVR.h"
#include "RE/P/PlayerCharacter.h"
#include "ScreenSpaceGI.h"
#include "Upscaling.h"
#include "VR/OpenVRDetection.h"
#include "VR/VRVariableRateShading.h"

#include "State.h"
#include "Utils/D3D.h"
#include "Utils/VRUtils.h"

#include <d3d11.h>
#include <imgui_impl_dx11.h>
#include <openvr.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VR::Settings,
	EnableDepthBufferCullingInterior,
	EnableDepthBufferCullingExterior,
	MinOccludeeBoxExtent,
	mouseDeadzone,
	VROverlayOpenKeys,
	VROverlayCloseKeys,
	EnableStereoBlend,
	StereoBlendDepthSigma,
	StereoBlendMaxFactor,
	StereoBlendColorThreshold,
	ReprojectDebugMode,
	EnableSSRFoveation,
	EnableSSRFoveationHardCutoff,
	EnableVariableRateShading,
	VrsRadiusScale,
	VrsInnerRadius,
	VrsMidRadius,
	VrsDebugVisualize,
	VrsDitherStrength)

//=============================================================================
// FEATURE BASE CLASS OVERRIDES
//=============================================================================

void VR::LoadSettings(json& o_json)
{
	settings = o_json.get<Settings>();
	settings.ClampToValidRanges();
	if (o_json.contains("StereoOptimizations")) {
		json stereoOptJson = o_json["StereoOptimizations"];
		stereoOpt.LoadSettings(stereoOptJson);
	}
}

void VR::SaveSettings(json& o_json)
{
	o_json = settings;
	{
		json stereoOptJson;
		stereoOpt.SaveSettings(stereoOptJson);
		o_json["StereoOptimizations"] = stereoOptJson;
	}
}

json VR::GetDiagnostics()
{
	return stereoOpt.GetDiagnostics();
}

void VR::RestoreDefaultSettings()
{
	settings = {};
	stereoOpt.RestoreDefaultSettings();
}

void VR::SetupResources()
{
	// Runs here, not on the first EarlyPrepass, so IsAvailable() is accurate
	// before shadow maps first render (can be long after boot).
	VRFeatures::VRVariableRateShading::GetSingleton()->Initialize();

	CompileStereoBlendShaders();

	auto renderer = globals::game::renderer;
	auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC mainDesc;
	mainTex.texture->GetDesc(&mainDesc);
	mainDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	mainDesc.MiscFlags = 0;
	stereoBlendCopyTex = eastl::make_unique<Texture2D>(mainDesc, "VR::StereoBlendCopyTex");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = mainDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	stereoBlendCopyTex->CreateSRV(srvDesc);
	stereoBlendCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<StereoBlendCB>(), "VR::StereoBlendCB");

	// stereoMode is restart-gated, so a consumer enabled after boot falls back to its own
	// check until the next restart.
	bool needsClassification = stereoOpt.settings.stereoMode != VRStereoOptimizations::StereoMode::Off ||
	                           (globals::features::screenSpaceGI.settings.Enabled && globals::features::screenSpaceGI.settings.UseStereoReproject);
	if (globals::game::isVR && needsClassification) {
		stereoOpt.SetupResources();
		stereoOpt.loaded = stereoOpt.GetModeTextureSRV() != nullptr;
	} else {
		stereoOpt.loaded = false;
	}

	DetectOpenVRInfo();

	if (openVRInfo.isAvailable) {
		logger::info("OpenVR DLL detected:");
		logger::info("  Path: {}", openVRInfo.dllPath);
		logger::info("  Version: {}", openVRInfo.version);
		logger::info("  Size: {} bytes", openVRInfo.fileSize);
		logger::info("  Modified: {}", openVRInfo.modificationTime);
		logger::info("  Runtime: {}", VRDetection::RuntimeTypeToString(openVRInfo.runtimeType));
		logger::info("  Interface probing: {}", openVRInfo.probingSucceeded ? "Passed" : "Failed");
		logger::info("    Overlay (IVROverlay_016): {}", openVRInfo.hasOverlayInterface ? "Yes" : "No");
		logger::info("    System (IVRSystem_017): {}", openVRInfo.hasSystemInterface ? "Yes" : "No");
		logger::info("    Compositor (IVRCompositor_021): {}", openVRInfo.hasCompositorInterface ? "Yes" : "No");
		logger::info("  Compatible: {}", openVRInfo.isCompatible ? "Yes" : "No");

		if (!openVRInfo.isCompatible) {
			if (globals::state->IsDeveloperMode()) {
				logger::info("OpenVR not natively compatible, but developer mode is active - VR menus enabled");
			} else {
				logger::info("OpenVR version is incompatible. Open Shaders VR menus will be disabled for stability");
			}
		}
	} else {
		logger::info("OpenVR DLL not available in current process");
	}
}

void VR::PostPostLoad()
{
	stereoOpt.LatchBootSnapshot();

	gDepthBufferCulling = reinterpret_cast<bool*>(REL::Offset(0x1EC6B88).address());
	if (!gDepthBufferCulling) {
		static bool s_defaultDepthBufferCulling = false;
		gDepthBufferCulling = &s_defaultDepthBufferCulling;
		logger::warn("VR: gDepthBufferCulling address not found - using fallback default (false)");
	}

	gMinOccludeeBoxExtent = reinterpret_cast<float*>(REL::Offset(0x1ED64E8).address());
	if (!gMinOccludeeBoxExtent) {
		static float s_defaultMinOccludeeBoxExtent = 10.0f;
		gMinOccludeeBoxExtent = &s_defaultMinOccludeeBoxExtent;
		logger::warn("VR: gMinOccludeeBoxExtent address not found - using fallback default (10.0)");
	}

	// Migration: Fix legacy overlay keybinds
	if (settings.VROverlayCloseKeys.size() == 1) {
		auto& closeKey = settings.VROverlayCloseKeys[0];
		if (closeKey.GetDevice() == ControllerDevice::Keyboard && closeKey.GetKey() == 32) {
			settings.VROverlayCloseKeys[0] = InputCombo::Primary(32);
			logger::info("VR: Migrated VROverlayCloseKeys from Keyboard(32) to Primary(32)");
		}
	}
	if (settings.VROverlayOpenKeys.size() == 1) {
		auto& openKey = settings.VROverlayOpenKeys[0];
		if (openKey.GetDevice() == ControllerDevice::Keyboard && openKey.GetKey() == 32) {
			settings.VROverlayOpenKeys[0] = InputCombo::Secondary(32);
			logger::info("VR: Migrated VROverlayOpenKeys from Keyboard(32) to Secondary(32)");
		}
	}

	REL::safe_write(REL::RelocationID(0, 0, 69528).address() + REL::Relocate(0, 0, 0xD9) + 0x2, 0x148);
	REL::safe_write(REL::RelocationID(0, 0, 69528).address() + REL::Relocate(0, 0, 0xE5) + 0x2, 0x14C);
	REL::safe_write(REL::RelocationID(0, 0, 69528).address() + REL::Relocate(0, 0, 0xF1) + 0x2, 0x150);

	// Connect to ImGuiVRHelper here (kPostPostLoad): the helper has registered its
	// handshake listener by now, so this reaches it regardless of load order.
	ConnectHelper();
}

void VR::DataLoaded()
{
	// Initialize occlusion culling based on user settings and current interior/exterior state.
	UpdateDepthBufferCulling();

	if (gMinOccludeeBoxExtent) {
		*gMinOccludeeBoxExtent = settings.MinOccludeeBoxExtent;
	} else {
		logger::warn("VR::DataLoaded: gMinOccludeeBoxExtent is null, skipping assignment");
	}
}

void VR::EarlyPrepass()
{
	// Apply culling setting each prepass based on current interior/exterior state.
	UpdateDepthBufferCulling();

	// Runs regardless of the setting so IsAvailable() is accurate before the
	// user ever toggles the checkbox.
	auto* vrs = VRFeatures::VRVariableRateShading::GetSingleton();
	vrs->Initialize();
	vrs->SetEnabled(settings.EnableVariableRateShading);
	if (settings.EnableVariableRateShading && vrs->IsAvailable()) {
		// Real lens center (vrperfkit's approach), not Foveated DLSS's crop --
		// that isn't always configured and doesn't describe lens geometry.
		const float2 leftOffset = Util::GetEyeLensCenterOffset(0);
		const float2 rightOffset = Util::GetEyeLensCenterOffset(1);
		VRFeatures::FoveationProfile profile{};
		profile.usingRealLensCenter = leftOffset.x != 0.0f || leftOffset.y != 0.0f || rightOffset.x != 0.0f || rightOffset.y != 0.0f;
		profile.centerOffsets[0] = leftOffset;
		profile.centerOffsets[1] = rightOffset;
		vrs->SetFoveationProfile(profile);
		vrs->SetTuning(settings.VrsRadiusScale, settings.VrsInnerRadius, settings.VrsMidRadius);
		vrs->SetDebugVisualize(settings.VrsDebugVisualize);
		vrs->SetDitherStrength(settings.VrsDitherStrength);
		vrs->ApplyForRenderTarget(globals::d3d::context);
	}
}

//=============================================================================
// DEPTH BUFFER CULLING
//=============================================================================

// Helper to centralize VR depth buffer culling logic, reducing duplication between DataLoaded, EarlyPrepass, and Settings UI.
void VR::UpdateDepthBufferCulling()
{
	if (!gDepthBufferCulling) {
		return;
	}

	const auto* tes = globals::game::tes;
	const bool inInterior = tes && tes->interiorCell != nullptr;
	const bool desired = inInterior ? settings.EnableDepthBufferCullingInterior : settings.EnableDepthBufferCullingExterior;

	const bool previous = *gDepthBufferCulling;
	*gDepthBufferCulling = desired;

	if (previous != desired) {
		logger::info("VR depth buffer culling set to {}", desired);
	}
}

//=============================================================================
// OPENVR VERSION DETECTION AND COMPATIBILITY
//=============================================================================

void VR::DetectOpenVRInfo()
{
	openVRInfo = {};

	auto result = VRDetection::Detect();

	openVRInfo.isAvailable = result.isAvailable;
	openVRInfo.isCompatible = result.isCompatible;
	openVRInfo.dllPath = result.dllPath;
	openVRInfo.version = result.version;
	openVRInfo.fileSize = result.fileSize;
	openVRInfo.modificationTime = result.modificationTime;
	openVRInfo.hasOverlayInterface = result.hasOverlayInterface;
	openVRInfo.hasSystemInterface = result.hasSystemInterface;
	openVRInfo.hasCompositorInterface = result.hasCompositorInterface;
	openVRInfo.runtimeType = result.runtimeType;
	openVRInfo.probingSucceeded = result.probingSucceeded;
}

bool VR::IsOpenVRCompatible() const
{
	return globals::game::isVR && openVRInfo.isCompatible;
}

void VR::Reset()
{
	stereoOpt.Reset();
}

float VR::GetHMDRefreshRate() const
{
	if (!globals::game::isVR)
		return 0.0f;
	auto* openvr = RE::BSOpenVR::GetSingleton();
	if (!openvr || !openvr->vrSystem)
		return 0.0f;
	vr::ETrackedPropertyError err = vr::TrackedProp_Success;
	float hz = openvr->vrSystem->GetFloatTrackedDeviceProperty(
		vr::k_unTrackedDeviceIndex_Hmd,
		vr::Prop_DisplayFrequency_Float,
		&err);
	return (err == vr::TrackedProp_Success && hz > 1.0f) ? hz : 0.0f;
}
