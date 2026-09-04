#include "NearClipMeshExclusions.h"

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
		std::atomic<uint32_t> taggedModels{ 0 };

		void ApplyModelData(const char* modelName, RE::NiNode* root)
		{
			if (!modelName || !root || !MatchesNormalizedPath(Util::FixFilePath(modelName)))
				return;
			uint32_t count = 0;
			RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* object) {
				if (auto* geometry = object->AsGeometry()) {
					if (!geometry->GetExtraData(kIgnoreName)) {
						if (auto* data = RE::NiIntegerExtraData::Create(kIgnoreName, 1))
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
		logger::info("VR dynamic near clip: installed model metadata hook for {} excluded fog/mist NIF names", kIgnoredNames.size());
	}

	bool IsIgnored(const RE::BSGeometry* geometry)
	{
		if (!geometry)
			return false;
		static REL::Relocation<const RE::NiRTTI*> integerRTTI{ RE::NiIntegerExtraData::Ni_RTTI };
		const auto* data = geometry->GetExtraData(kIgnoreName);
		return data && data->GetRTTI() == integerRTTI.get() && static_cast<const RE::NiIntegerExtraData*>(data)->value == 1;
	}

	uint32_t TaggedModelCount()
	{
		return taggedModels.load(std::memory_order_relaxed);
	}

	void UpdateFogClearance(const RE::BSRenderPass* pass)
	{
		auto* state = globals::state;
		state->permutationData.FogClearanceRadius = 0.0f;
		const auto& vr = globals::features::vr;
		if (!globals::game::isVR || !vr.loaded || !vr.settings.FogClearance ||
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
		if (accumulator && accumulator->camera == RE::Main::WorldRootCamera())
			state->permutationData.FogClearanceRadius = vr.settings.FogClearanceRadius;
	}
}
