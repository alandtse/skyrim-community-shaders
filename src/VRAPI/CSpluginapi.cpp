#include "VRAPI/CSpluginapi.h"

#include "Feature.h"
#include "Features/LightLimitFix.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/Upscaling.h"
#include "Features/VolumetricLighting.h"
#include "Globals.h"
#include "SceneSettingsManager.h"
#include "Utils/SettingsPatch.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>

namespace CSPluginAPI
{
	namespace
	{
		constexpr const char* kFeatureUpscaling = "Upscaling";
		constexpr const char* kFeatureScreenSpaceGI = "ScreenSpaceGI";
		constexpr const char* kFeatureScreenSpaceShadows = "ScreenSpaceShadows";
		constexpr const char* kFeatureLightLimitFix = "LightLimitFix";
		constexpr const char* kFeatureVolumetricLighting = "VolumetricLighting";

		// Reserved staging key. The target field depends on Streamline's DLSS availability,
		// which isn't final until the D3D device-creation hook runs -- later than the
		// kPostLoad handshake a consumer may call from -- so resolution is deferred to
		// ProcessStagedSettings on the render thread instead of read at stage time.
		constexpr const char* kDeferredUpscaleMethod = "__deferredUpscaleMethod";

		std::mutex stagedMutex;
		// Per-feature coalesced patches: a later write to the same key supersedes the
		// earlier one, and the whole map lands at the next frame boundary.
		std::map<std::string, json> stagedPatches;

		CSInterface001 g_interface001;

		bool IsValidInterfaceRequest(const SKSE::MessagingInterface::Message* message)
		{
			return message &&
			       message->type == CSMessage::kMessage_GetInterface &&
			       message->data &&
			       message->dataLen >= sizeof(CSMessage);
		}

		// kHoshipa/kUltraQuality are ABI-valid but have no matching quality mode in this
		// build; values 0-4 map 1:1 onto Upscaling::QualityMode.
		std::optional<uint32_t> UpscalePresetToQualityMode(UpscalePreset preset)
		{
			switch (preset) {
			case UpscalePreset::kNativeAA:
			case UpscalePreset::kQuality:
			case UpscalePreset::kBalanced:
			case UpscalePreset::kPerformance:
			case UpscalePreset::kUltraPerformance:
				return static_cast<uint32_t>(preset);
			case UpscalePreset::kHoshipa:
			case UpscalePreset::kUltraQuality:
				logger::warn("[CS API] Upscaler preset {} is not supported by this build; ignoring", static_cast<uint32_t>(preset));
				return std::nullopt;
			default:
				logger::warn("[CS API] Ignoring invalid upscaler preset value {}", static_cast<uint32_t>(preset));
				return std::nullopt;
			}
		}

		// API kJ..kM map to presetDLSS combo slots 1..4 (slot 0 = Default/auto).
		// kF is ABI-valid but this build's DLSS integration has no preset F.
		std::optional<uint32_t> DLSSProfileToPresetDLSS(DLSSProfile profile)
		{
			switch (profile) {
			case DLSSProfile::kJ:
				return 1u;
			case DLSSProfile::kK:
				return 2u;
			case DLSSProfile::kL:
				return 3u;
			case DLSSProfile::kM:
				return 4u;
			case DLSSProfile::kF:
				logger::warn("[CS API] DLSS profile F is not supported by this build; ignoring");
				return std::nullopt;
			default:
				logger::warn("[CS API] Ignoring invalid DLSS profile value {}", static_cast<uint32_t>(profile));
				return std::nullopt;
			}
		}

		DLSSProfile PresetDLSSToDLSSProfile(uint32_t presetDLSS)
		{
			switch (presetDLSS) {
			case 2:
				return DLSSProfile::kK;
			case 3:
				return DLSSProfile::kL;
			case 4:
				return DLSSProfile::kM;
			default:
				// 1 = J; 0 = Default, whose Streamline auto-selection also resolves to J.
				return DLSSProfile::kJ;
			}
		}

		// API and internal UpscaleMethod enums share values 0-3; validate rather than trust.
		std::optional<uint32_t> ValidateUpscaleMethod(UpscaleMethod method)
		{
			switch (method) {
			case UpscaleMethod::kNone:
			case UpscaleMethod::kTAA:
			case UpscaleMethod::kFSR:
			case UpscaleMethod::kDLSS:
				return static_cast<uint32_t>(method);
			default:
				logger::warn("[CS API] Ignoring invalid upscaler method value {}", static_cast<uint32_t>(method));
				return std::nullopt;
			}
		}

		UpscaleMethod FromInternalUpscaleMethod(Upscaling::UpscaleMethod method)
		{
			switch (method) {
			case Upscaling::UpscaleMethod::kTAA:
				return UpscaleMethod::kTAA;
			case Upscaling::UpscaleMethod::kFSR:
				return UpscaleMethod::kFSR;
			case Upscaling::UpscaleMethod::kDLSS:
				return UpscaleMethod::kDLSS;
			case Upscaling::UpscaleMethod::kNONE:
			default:
				return UpscaleMethod::kNone;
			}
		}

		void StagePatch(const char* a_shortName, const json& a_patch)
		{
			if (!a_patch.is_object())
				return;
			std::scoped_lock lock(stagedMutex);
			auto& slot = stagedPatches[a_shortName];
			// Merge into a temporary: a throwing merge must not leave a half-built
			// patch in the map for the next drain to apply.
			json merged = slot;
			merged.merge_patch(a_patch);
			slot = std::move(merged);
		}

		// Rewrites kDeferredUpscaleMethod to the field it actually targets, now that
		// Streamline's DLSS availability is known. Mirrors GetUpscaleMethod()'s
		// no-PerfMode branch: without DLSS the no-DLSS preference is the live field,
		// and DLSS there coerces to FSR.
		void ResolveDeferredKeys(json& a_patch)
		{
			const auto it = a_patch.find(kDeferredUpscaleMethod);
			if (it == a_patch.end())
				return;
			const auto method = it->get<uint32_t>();
			a_patch.erase(it);
			if (globals::features::upscaling.streamline.featureDLSS)
				a_patch["upscaleMethod"] = method;
			else
				a_patch["upscaleMethodNoDLSS"] = std::min(method, static_cast<uint32_t>(UpscaleMethod::kFSR));
		}
	}

	void* GetApi(unsigned int revisionNumber)
	{
		// Accept revision 0 as "latest" and keep older revisions available for
		// already-compiled consumers. New revisions only append vtable entries.
		if (revisionNumber != 0 &&
			revisionNumber != CSInterfaceRevision001 &&
			revisionNumber != CSInterfaceRevision002 &&
			revisionNumber != CSInterfaceRevision) {
			return nullptr;
		}

		return &g_interface001;
	}

	void ModMessageHandler(SKSE::MessagingInterface::Message* message)
	{
		if (!IsValidInterfaceRequest(message)) {
			return;
		}

		auto* csMessage = static_cast<CSMessage*>(message->data);
		csMessage->GetApiFunction = GetApi;
		logger::info("Provided Community Shaders plugin interface to {}", message->sender ? message->sender : "<unknown>");
	}

	void ProcessStagedSettings()
	{
		std::map<std::string, json> pending;
		{
			std::scoped_lock lock(stagedMutex);
			if (stagedPatches.empty())
				return;
			pending.swap(stagedPatches);
		}
		SceneSettingsManager::SceneLayerGuard sceneLayerGuard(
			*SceneSettingsManager::GetSingleton());
		for (auto& [shortName, patch] : pending) {
			auto* feature = Feature::FindFeatureByShortName(shortName);
			if (!feature) {
				logger::warn("[CS API] Dropping staged settings for unloaded feature '{}'", shortName);
				continue;
			}
			std::vector<std::string> unknown;
			try {
				ResolveDeferredKeys(patch);
				if (!Util::Settings::ApplyPatch(*feature, patch, unknown)) {
					std::string keys;
					for (const auto& key : unknown)
						keys += (keys.empty() ? "" : ", ") + key;
					logger::warn("[CS API] Dropped staged settings for '{}': unrecognized key(s) {}", shortName, keys);
				}
			} catch (const std::exception& e) {
				logger::error("[CS API] Applying staged settings for '{}' threw: {}", shortName, e.what());
			}
		}
	}

	unsigned int CSInterface001::getBuildNumber()
	{
		return CSBuildNumber;
	}

	bool CSInterface001::GetSSSEnabled()
	{
		return globals::features::screenSpaceShadows.bendSettings.Enable != 0;
	}

	void CSInterface001::SetSSSEnabled(bool enabled)
	{
		StagePatch(kFeatureScreenSpaceShadows, { { "Enable", enabled ? 1u : 0u } });
	}

	bool CSInterface001::GetSSGIEnabled()
	{
		return globals::features::screenSpaceGI.settings.Enabled;
	}

	void CSInterface001::SetSSGIEnabled(bool enabled)
	{
		StagePatch(kFeatureScreenSpaceGI, { { "Enabled", enabled } });
	}

	bool CSInterface001::GetVolumetricLightingExteriorEnabled()
	{
		return globals::features::volumetricLighting.settings.ExteriorEnabled;
	}

	void CSInterface001::SetVolumetricLightingExteriorEnabled(bool enabled)
	{
		StagePatch(kFeatureVolumetricLighting, { { "ExteriorEnabled", enabled } });
	}

	UpscalePreset CSInterface001::GetUpscalePreset()
	{
		auto& upscaling = globals::features::upscaling;
		// While PerfMode's render-target hook is active the boot-latched preset is the
		// one actually rendering; report it rather than a pending selection.
		const uint32_t mode = upscaling.perfMode.IsHookActive() ?
		                          upscaling.bootSnapshot.Boot(&Upscaling::Settings::qualityMode) :
		                          upscaling.settings.qualityMode;
		return static_cast<UpscalePreset>(std::min(mode, static_cast<uint32_t>(Upscaling::QualityMode::kUltraPerformance)));
	}

	void CSInterface001::SetUpscalePreset(UpscalePreset preset)
	{
		if (const auto qualityMode = UpscalePresetToQualityMode(preset))
			StagePatch(kFeatureUpscaling, { { "qualityMode", *qualityMode } });
	}

	bool CSInterface001::GetLightLimitFixContactShadowsEnabled()
	{
		return globals::features::lightLimitFix.settings.EnableContactShadows;
	}

	void CSInterface001::SetLightLimitFixContactShadowsEnabled(bool enabled)
	{
		StagePatch(kFeatureLightLimitFix, { { "EnableContactShadows", enabled } });
	}

	DLSSProfile CSInterface001::GetDLSSProfile()
	{
		return PresetDLSSToDLSSProfile(globals::features::upscaling.settings.presetDLSS);
	}

	void CSInterface001::SetDLSSProfile(DLSSProfile profile)
	{
		if (const auto presetDLSS = DLSSProfileToPresetDLSS(profile))
			StagePatch(kFeatureUpscaling, { { "presetDLSS", *presetDLSS } });
	}

	bool CSInterface001::GetRenderAtUpscaleResEnabled()
	{
		return globals::features::upscaling.settings.renderAtUpscaleRes;
	}

	void CSInterface001::SetRenderAtUpscaleResEnabled(bool enabled)
	{
		StagePatch(kFeatureUpscaling, { { "renderAtUpscaleRes", enabled } });
	}

	bool CSInterface001::GetRenderAtUpscaleResActive()
	{
		return globals::features::upscaling.perfMode.IsHookActive();
	}

	void CSInterface001::SetVRUpscalingTransitionProfile(bool renderScaleModeEnabled, UpscalePreset preset, DLSSProfile profile)
	{
		// No dynamic relatch staging in this build: validate everything, then stage the
		// plain settings writes together (restart-gated fields apply on next boot).
		const auto qualityMode = UpscalePresetToQualityMode(preset);
		const auto presetDLSS = DLSSProfileToPresetDLSS(profile);
		if (!qualityMode || !presetDLSS)
			return;

		StagePatch(kFeatureUpscaling, {
										  { "renderAtUpscaleRes", renderScaleModeEnabled },
										  { "qualityMode", *qualityMode },
										  { "presetDLSS", *presetDLSS },
									  });
	}

	UpscaleMethod CSInterface001::GetUpscaleMethod()
	{
		return FromInternalUpscaleMethod(globals::features::upscaling.GetUpscaleMethod());
	}

	void CSInterface001::SetUpscaleMethod(UpscaleMethod method)
	{
		if (const auto value = ValidateUpscaleMethod(method))
			StagePatch(kFeatureUpscaling, { { kDeferredUpscaleMethod, *value } });
	}

	void CSInterface001::SetVRUpscalingTransitionProfileForMethod(UpscaleMethod method, bool renderScaleModeEnabled, UpscalePreset preset, DLSSProfile profile)
	{
		const auto methodValue = ValidateUpscaleMethod(method);
		const auto qualityMode = UpscalePresetToQualityMode(preset);
		const auto presetDLSS = DLSSProfileToPresetDLSS(profile);
		if (!methodValue || !qualityMode || !presetDLSS)
			return;

		StagePatch(kFeatureUpscaling, {
										  { kDeferredUpscaleMethod, *methodValue },
										  { "renderAtUpscaleRes", renderScaleModeEnabled },
										  { "qualityMode", *qualityMode },
										  { "presetDLSS", *presetDLSS },
									  });
	}

	uint32_t CSInterface001::GetVRUpscalingApplyBlockReasons()
	{
		// No transition staging machinery in this build, so applies are never blocked.
		return 0;
	}

	bool CSInterface001::IsVRUpscalingProfileApplyAllowed()
	{
		return true;
	}
}  // namespace CSPluginAPI
