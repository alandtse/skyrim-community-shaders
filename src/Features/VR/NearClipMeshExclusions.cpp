#include "NearClipMeshExclusions.h"

#include "Features/ExponentialHeightFog.h"
#include "Features/VR.h"
#include "NearClipIgnoredModels.h"
#include "State.h"
#include "Utils/Format.h"

#include <atomic>

namespace VRNearClipMeshes
{
	namespace
	{
		const RE::BSFixedString kIgnoreName = "OS_NearClipIgnore";
		constexpr int32_t kIgnoredModelTag = 1;
		constexpr int32_t kCameraAttachedFogTag = 2;
		std::atomic<bool> installed{ false };
		std::atomic<uint32_t> taggedModels{ 0 };
		std::atomic<uint32_t> lastCameraFogFrame{ UINT32_MAX };
		std::atomic<uint32_t> skippedDraws{ 0 };

		const RE::NiIntegerExtraData* GetModelTag(const RE::BSGeometry* geometry)
		{
			if (!geometry)
				return nullptr;
			static REL::Relocation<const RE::NiRTTI*> integerRTTI{ RE::NiIntegerExtraData::Ni_RTTI };
			const auto* data = geometry->GetExtraData(kIgnoreName);
			return data && data->GetRTTI() == integerRTTI.get() ? static_cast<const RE::NiIntegerExtraData*>(data) : nullptr;
		}

		void ApplyModelData(const char* modelName, RE::NiNode* root)
		{
			if (!modelName || !root)
				return;
			const auto normalizedPath = Util::FixFilePath(modelName);
			if (!MatchesNormalizedPath(normalizedPath))
				return;
			const int32_t tagValue = MatchesCameraAttachedFogPath(normalizedPath) ? kCameraAttachedFogTag : kIgnoredModelTag;
			uint32_t count = 0;
			RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* object) {
				if (auto* geometry = object->AsGeometry()) {
					if (!geometry->GetExtraData(kIgnoreName)) {
						if (auto* data = RE::NiIntegerExtraData::Create(kIgnoreName, tagValue))
							geometry->AddExtraData(data);
					}
					if (IsIgnored(geometry))
						++count;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			if (count) {
				taggedModels.fetch_add(1, std::memory_order_relaxed);
				logger::debug("VR dynamic near clip: excluded model {} ({} geometries)", modelName, count);
			}
		}

		struct TESProcessorPostCreate
		{
			static void thunk(RE::TESModelDB::TESProcessor* processor, const RE::BSModelDB::DBTraits::ArgsType& args,
				const char* modelName, RE::NiPointer<RE::NiNode>& root, uint32_t& typeOut)
			{
				func(processor, args, modelName, root, typeOut);
				ApplyModelData(modelName, root.get());
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		stl::write_vfunc<0x1, TESProcessorPostCreate>(RE::VTABLE_TESModelDB____TESProcessor[0]);
		installed.store(true, std::memory_order_release);
		logger::info("VR dynamic near clip: installed model metadata hook for {} excluded fog/mist NIF names", kIgnoredNames.size());
	}

	bool IsInstalled()
	{
		return installed.load(std::memory_order_acquire);
	}

	bool IsIgnored(const RE::BSGeometry* geometry)
	{
		const auto* data = GetModelTag(geometry);
		return data && (data->value == kIgnoredModelTag || data->value == kCameraAttachedFogTag);
	}

	bool IsCameraAttachedFog(const RE::BSGeometry* geometry)
	{
		const auto* data = GetModelTag(geometry);
		return data && data->value == kCameraAttachedFogTag;
	}

	bool ShouldSkipFogMesh(const RE::BSRenderPass* pass)
	{
		const auto& vr = globals::features::vr;
		if (!globals::game::isVR || !vr.loaded || !vr.settings.DisableAllFogMeshes || !pass || !IsIgnored(pass->geometry))
			return false;
		skippedDraws.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool WasCameraFogVisibleRecently(uint32_t frame, uint32_t maximumAge)
	{
		const uint32_t lastFrame = lastCameraFogFrame.load(std::memory_order_relaxed);
		return lastFrame != UINT32_MAX && frame - lastFrame <= maximumAge;
	}

	uint32_t TaggedModelCount()
	{
		return taggedModels.load(std::memory_order_relaxed);
	}

	uint32_t SkippedDrawCount()
	{
		return skippedDraws.load(std::memory_order_relaxed);
	}

	void UpdateFogClearance(const RE::BSRenderPass* pass)
	{
		auto* state = globals::state;
		state->permutationData.FogClearanceRadius = 0.0f;
		const auto& vr = globals::features::vr;
		if (!globals::game::isVR || !vr.loaded || (!vr.settings.FogClearance && !vr.settings.ReplaceCameraFogWithVolume) ||
			state->activeReflections || state->IsFullScreenMenuOpen() || !pass || !pass->shader)
			return;
		switch (pass->shader->shaderType.get()) {
		case RE::BSShader::Type::Effect:
		case RE::BSShader::Type::Lighting:
		case RE::BSShader::Type::Utility:
			break;
		default:
			return;
		}
		if (!IsIgnored(pass->geometry))
			return;
		const auto* accumulator = *globals::game::currentAccumulator.get();
		if (!accumulator || accumulator->camera != RE::Main::WorldRootCamera())
			return;

		if (IsCameraAttachedFog(pass->geometry) && vr.settings.ReplaceCameraFogWithVolume) {
			lastCameraFogFrame.store(state->frameCount, std::memory_order_relaxed);
			if (globals::features::exponentialHeightFog.loaded && globals::features::exponentialHeightFog.IsLocalFogReplacementReady()) {
				state->permutationData.FogClearanceRadius = -1.0f;
				return;
			}
		}

		if (vr.settings.FogClearance)
			state->permutationData.FogClearanceRadius = vr.settings.FogClearanceRadius;
	}
}
