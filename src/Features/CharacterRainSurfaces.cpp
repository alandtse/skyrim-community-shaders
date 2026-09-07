#include "CharacterRainSurfaces.h"

#include "Globals.h"
#include "State.h"
#include "WetnessEffects.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace CharacterRainSurfaces
{
	namespace
	{
		constexpr auto CharacterSurfaceDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::IsCharacterRainSurface);
		constexpr auto HeldWeaponDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::IsHeldWeapon);
		// Lifecycle events can precede the actor graph update, so refresh across a few frames.
		constexpr std::uint8_t kGeometryRefreshPasses = 3;
		constexpr float kFullyOpaqueAlphaThreshold = 0.999f;
		const RE::BSFixedString CharacterRainSurfaceTag = "OpenShadersCharacterRainSurface";

		enum SurfaceFlag : std::int32_t
		{
			Character = 1 << 0,
			HeldWeapon = 1 << 1
		};

		std::mutex pendingActorRefreshLock;
		std::unordered_map<RE::FormID, std::uint8_t> pendingActorRefreshes;

		void SetSurfaceFlags(RE::BSGeometry* a_geometry, std::int32_t a_flags)
		{
			if (!a_geometry)
				return;

			if (auto* extraData = a_geometry->GetExtraData(CharacterRainSurfaceTag)) {
				if (extraData->GetRTTI() == globals::rtti::NiIntegerExtraDataRTTI.get())
					static_cast<RE::NiIntegerExtraData*>(extraData)->value = a_flags;
				return;
			}

			if (auto* extraData = RE::NiIntegerExtraData::Create(CharacterRainSurfaceTag, a_flags))
				a_geometry->AddExtraData(extraData);
		}

		std::int32_t GetSurfaceFlags(const RE::BSGeometry* a_geometry)
		{
			if (!a_geometry)
				return 0;

			const auto* extraData = a_geometry->GetExtraData(CharacterRainSurfaceTag);
			return extraData && extraData->GetRTTI() == globals::rtti::NiIntegerExtraDataRTTI.get() ?
			           static_cast<const RE::NiIntegerExtraData*>(extraData)->value :
			           0;
		}

		void TagGeometryTree(RE::NiAVObject* a_root, std::int32_t a_flags)
		{
			if (!a_root)
				return;

			RE::BSVisit::TraverseScenegraphGeometries(a_root, [a_flags](RE::BSGeometry* a_geometry) {
				SetSurfaceFlags(a_geometry, a_flags);
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		void TagHeldWeapons(RE::Actor* a_actor)
		{
			const std::uint32_t bipedCount = a_actor == globals::game::player ? 2u : 1u;
			for (std::uint32_t bipedIndex = 0; bipedIndex < bipedCount; ++bipedIndex) {
				const auto& biped = a_actor->GetBiped(bipedIndex != 0);
				if (!biped)
					continue;

				for (std::uint32_t slot = RE::BIPED_OBJECTS::kOneHandSword;
					slot <= RE::BIPED_OBJECTS::kCrossbow; ++slot) {
					const auto& object = biped->objects[slot];
					if (object.item && object.item->IsWeapon() && object.partClone)
						TagGeometryTree(object.partClone.get(), Character | HeldWeapon);
				}
			}
		}

		void ClassifyActor(RE::Actor* a_actor)
		{
			if (!a_actor || !a_actor->Is3DLoaded())
				return;

			auto* thirdPersonRoot = a_actor->Get3D(false);
			auto* firstPersonRoot = a_actor->Get3D(true);
			TagGeometryTree(thirdPersonRoot, Character);
			if (firstPersonRoot != thirdPersonRoot)
				TagGeometryTree(firstPersonRoot, Character);
			TagHeldWeapons(a_actor);
		}

		void QueueActor(RE::FormID a_formID, std::uint8_t a_refreshPasses)
		{
			if (!a_formID)
				return;

			const std::scoped_lock lock(pendingActorRefreshLock);
			auto& pendingPasses = pendingActorRefreshes[a_formID];
			pendingPasses = std::max(pendingPasses, a_refreshPasses);
		}

		bool IsCharacterRainSurfaceCompatible(const RE::BSRenderPass* a_pass)
		{
			if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty)
				return false;

			const auto* lightingProperty = a_pass->shaderProperty->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ?
			                                   static_cast<const RE::BSLightingShaderProperty*>(a_pass->shaderProperty) :
			                                   nullptr;
			if (!lightingProperty || !lightingProperty->material || lightingProperty->alpha < kFullyOpaqueAlphaThreshold ||
				lightingProperty->material->GetFeature() == RE::BSShaderMaterial::Feature::kHairTint ||
				lightingProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kHairTint) ||
				lightingProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction,
					RE::BSShaderProperty::EShaderPropertyFlag::kTempRefraction))
				return false;

			const auto& alphaProperty = a_pass->geometry->GetGeometryRuntimeData().alphaProperty;
			return !alphaProperty || (!alphaProperty->GetAlphaBlending() && !alphaProperty->GetAlphaTesting());
		}

		void UpdatePermutation(RE::BSRenderPass* a_pass)
		{
			auto& descriptor = globals::state->permutationData.ExtraShaderDescriptor;
			descriptor &= ~(CharacterSurfaceDescriptor | HeldWeaponDescriptor);
			if (!globals::features::wetnessEffects.ShouldClassifyCharacterRainSurfaces() || !a_pass)
				return;

			const std::int32_t surfaceFlags = GetSurfaceFlags(a_pass->geometry);
			if (!(surfaceFlags & Character) || !IsCharacterRainSurfaceCompatible(a_pass))
				return;

			descriptor |= CharacterSurfaceDescriptor;
			if (surfaceFlags & HeldWeapon)
				descriptor |= HeldWeaponDescriptor;
		}

		class ActorLifecycleEventHandler :
			public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
			public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESObjectLoadedEvent* a_event,
				RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (a_event && a_event->loaded && RE::TESForm::LookupByID<RE::Actor>(a_event->formID))
					QueueActor(a_event->formID, kGeometryRefreshPasses);
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESEquipEvent* a_event,
				RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (a_event && a_event->actor)
					QueueActor(a_event->actor->GetFormID(), kGeometryRefreshPasses);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
			{
				UpdatePermutation(a_pass);
				func(a_shader, a_pass, a_renderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		logger::info("[Wetness Effects] Installed character rain surface classification hook");
	}

	void RegisterEvents()
	{
		auto* eventSources = RE::ScriptEventSourceHolder::GetSingleton();
		if (!eventSources) {
			logger::error("[Wetness Effects] Script event source holder unavailable for character rain classification");
			return;
		}

		static ActorLifecycleEventHandler eventHandler;
		eventSources->GetEventSource<RE::TESObjectLoadedEvent>()->AddEventSink(&eventHandler);
		eventSources->GetEventSource<RE::TESEquipEvent>()->AddEventSink(&eventHandler);
	}

	void QueueLoadedActors()
	{
		{
			const std::scoped_lock lock(pendingActorRefreshLock);
			pendingActorRefreshes.clear();
		}

		if (auto* player = globals::game::player)
			QueueActor(player->GetFormID(), kGeometryRefreshPasses);

		if (const auto* processLists = RE::ProcessLists::GetSingleton()) {
			for (const auto& actorHandle : processLists->highActorHandles) {
				if (const auto actor = actorHandle.get())
					QueueActor(actor->GetFormID(), kGeometryRefreshPasses);
			}
		}
	}

	void RefreshPendingActors()
	{
		std::unordered_map<RE::FormID, std::uint8_t> actorsToRefresh;
		{
			const std::scoped_lock lock(pendingActorRefreshLock);
			actorsToRefresh.swap(pendingActorRefreshes);
		}

		for (const auto& [formID, remainingPasses] : actorsToRefresh) {
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID))
				ClassifyActor(actor);
			if (remainingPasses > 1)
				QueueActor(formID, remainingPasses - 1);
		}
	}
}
