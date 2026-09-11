#include "HeavyImpactWindRouter.h"

#include "ActorWind.h"
#include "Features/Wind/TransientWindImpulse.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numbers>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr std::size_t kMaximumQueuedEvents = 64;
	constexpr std::size_t kMaximumNpcPowerAttackSources = 5;
	constexpr float kActorDiscoveryInterval = 2.0f;
	constexpr double kDeduplicationRetention = 10.0;
	constexpr float kMinimumDistance = 50.0f;
	constexpr float kMaximumDistance = 30000.0f;
	constexpr float kMaximumStrength = 10.0f;
	constexpr float kMaximumSpeed = 10000.0f;
	constexpr float kMaximumTime = WindField::kTransientImpulseMaximumDecayTime;
	constexpr float kMaximumContactOffset = 500.0f;

	enum class ActorClass
	{
		None,
		Giant,
		Dragon,
		Troll,
		Mammoth,
		Centurion,
		Werewolf,
		VampireLord
	};

	struct QueuedImpact
	{
		RE::ActorHandle actor;
		HeavyImpactWindRouter::ImpactKind kind;
		std::uint64_t key;
		float3 contactPosition{};
		bool hasContact{};
	};

	[[nodiscard]] std::uint64_t MakeImpactKey(
		RE::FormID a_actor, HeavyImpactWindRouter::ImpactKind a_kind)
	{
		return (static_cast<std::uint64_t>(a_actor) << 8) | static_cast<std::uint64_t>(a_kind);
	}

	[[nodiscard]] bool ContainsInsensitive(std::string_view a_value, std::string_view a_needle)
	{
		return std::search(a_value.begin(), a_value.end(), a_needle.begin(), a_needle.end(),
				   [](unsigned char a_left, unsigned char a_right) {
					   return static_cast<unsigned char>(std::tolower(a_left)) ==
			                  static_cast<unsigned char>(std::tolower(a_right));
				   }) != a_value.end();
	}

	[[nodiscard]] ActorClass ClassifyActor(const RE::Actor& a_actor)
	{
		const auto* race = a_actor.GetRace();
		if (!race)
			return ActorClass::None;
		const char* editorID = race->GetFormEditorID();
		const std::string_view raceID = editorID ? editorID : "";
		if (race->HasKeywordString("ActorTypeDragon") || ContainsInsensitive(raceID, "dragon"))
			return ActorClass::Dragon;
		if (ContainsInsensitive(raceID, "centurion"))
			return ActorClass::Centurion;
		if (ContainsInsensitive(raceID, "mammoth"))
			return ActorClass::Mammoth;
		if (ContainsInsensitive(raceID, "troll"))
			return ActorClass::Troll;
		if (ContainsInsensitive(raceID, "giant"))
			return ActorClass::Giant;
		if (ContainsInsensitive(raceID, "werewolf"))
			return ActorClass::Werewolf;
		if (ContainsInsensitive(raceID, "vampire") && ContainsInsensitive(raceID, "beast"))
			return ActorClass::VampireLord;
		return ActorClass::None;
	}

	[[nodiscard]] bool IsTrackedAttackClass(ActorClass a_actorClass)
	{
		switch (a_actorClass) {
		case ActorClass::Giant:
		case ActorClass::Dragon:
		case ActorClass::Troll:
		case ActorClass::Mammoth:
		case ActorClass::Centurion:
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] const RE::BGSAttackData* GetCurrentAttackData(const RE::Actor& a_actor)
	{
		const auto* highProcess = a_actor.GetHighProcess();
		return highProcess ? highProcess->attackData.get() : nullptr;
	}

	[[nodiscard]] std::string_view GetAttackEvent(const RE::BGSAttackData* a_attackData)
	{
		return a_attackData ? std::string_view(a_attackData->event.c_str()) : std::string_view{};
	}

	[[nodiscard]] bool HasAttackFlag(
		const RE::BGSAttackData* a_attackData, RE::AttackData::AttackFlag a_flag)
	{
		return a_attackData && a_attackData->data.flags.any(a_flag);
	}

	[[nodiscard]] bool IsPowerOrChargeAttack(const RE::BGSAttackData* a_attackData)
	{
		return HasAttackFlag(a_attackData, RE::AttackData::AttackFlag::kPowerAttack) ||
		       HasAttackFlag(a_attackData, RE::AttackData::AttackFlag::kChargeAttack);
	}

	[[nodiscard]] bool IsGiantClubAttack(const RE::BGSAttackData* a_attackData, const RE::TESObjectWEAP* a_weapon)
	{
		const auto attackEvent = GetAttackEvent(a_attackData);
		if (ContainsInsensitive(attackEvent, "club"))
			return true;
		if (!a_weapon)
			return false;
		const char* editorID = a_weapon->GetFormEditorID();
		const std::string_view weaponID = editorID ? editorID : "";
		return ContainsInsensitive(weaponID, "giant") && ContainsInsensitive(weaponID, "club");
	}

	[[nodiscard]] bool IsHeavyAttack(const RE::BGSAttackData* a_attackData)
	{
		if (IsPowerOrChargeAttack(a_attackData))
			return true;
		const auto attackEvent = GetAttackEvent(a_attackData);
		return ContainsInsensitive(attackEvent, "power") || ContainsInsensitive(attackEvent, "charge") ||
		       ContainsInsensitive(attackEvent, "stomp") || ContainsInsensitive(attackEvent, "slam") ||
		       ContainsInsensitive(attackEvent, "smash") || ContainsInsensitive(attackEvent, "overhead");
	}

	[[nodiscard]] bool IsDragonWingOrTailAttack(
		const RE::BGSAttackData* a_attackData, const RE::TESObjectWEAP* a_weapon)
	{
		const auto attackEvent = GetAttackEvent(a_attackData);
		const char* weaponEditorID = a_weapon ? a_weapon->GetFormEditorID() : nullptr;
		const std::string_view weaponID = weaponEditorID ? weaponEditorID : "";
		if (ContainsInsensitive(attackEvent, "bite") || ContainsInsensitive(weaponID, "bite"))
			return false;
		return ContainsInsensitive(attackEvent, "wing") || ContainsInsensitive(attackEvent, "tail") ||
		       ContainsInsensitive(weaponID, "wing") || ContainsInsensitive(weaponID, "tail");
	}

	[[nodiscard]] bool HasEquippedShield(const RE::Actor& a_actor)
	{
		auto* equippedObject = a_actor.GetEquippedObject(true);
		auto* armor = equippedObject ? equippedObject->As<RE::TESObjectARMO>() : nullptr;
		return armor && armor->IsShield();
	}

	[[nodiscard]] float3 HorizontalAimDirection(RE::Actor& a_actor)
	{
		const auto aim = ActorWind::GetAimDirection(a_actor);
		const float squaredLength = aim.x * aim.x + aim.y * aim.y;
		if (!std::isfinite(squaredLength) || squaredLength <= 1e-6f)
			return { 0.0f, 1.0f, 0.0f };
		const float inverseLength = 1.0f / std::sqrt(squaredLength);
		return { aim.x * inverseLength, aim.y * inverseLength, 0.0f };
	}

	[[nodiscard]] bool IsFinitePositive(float a_value)
	{
		return std::isfinite(a_value) && a_value > 0.0f;
	}

	[[nodiscard]] float GetForwardContactOffset(HeavyImpactWindRouter::ImpactKind a_kind)
	{
		switch (a_kind) {
		case HeavyImpactWindRouter::ImpactKind::GiantClubSmash:
			return 240.0f;
		case HeavyImpactWindRouter::ImpactKind::DragonGroundAttack:
			return 200.0f;
		case HeavyImpactWindRouter::ImpactKind::MammothHeavyAttack:
			return 180.0f;
		case HeavyImpactWindRouter::ImpactKind::CenturionHeavyAttack:
			return 140.0f;
		case HeavyImpactWindRouter::ImpactKind::TrollPowerAttack:
			return 120.0f;
		default:
			return 0.0f;
		}
	}

	[[nodiscard]] float3 GetRadialImpactOrigin(RE::Actor& a_actor,
		HeavyImpactWindRouter::ImpactKind a_kind, const std::optional<float3>& a_contactPosition)
	{
		const auto actorPosition = a_actor.GetPosition();
		float3 origin{ actorPosition.x, actorPosition.y, actorPosition.z };
		if (a_contactPosition) {
			const float3 delta{ a_contactPosition->x - origin.x, a_contactPosition->y - origin.y, 0.0f };
			const float squaredDistance = delta.x * delta.x + delta.y * delta.y;
			if (std::isfinite(squaredDistance) && squaredDistance > 1e-6f) {
				const float distance = std::sqrt(squaredDistance);
				const float offset = std::min(distance, kMaximumContactOffset);
				origin.x += delta.x * (offset / distance);
				origin.y += delta.y * (offset / distance);
				origin.z = std::min(origin.z, a_contactPosition->z);
				return origin;
			}
		}

		const auto direction = HorizontalAimDirection(a_actor);
		const float forwardOffset = GetForwardContactOffset(a_kind);
		origin.x += direction.x * forwardOffset;
		origin.y += direction.y * forwardOffset;
		return origin;
	}

	[[nodiscard]] WindField::TransientWindSource MakeRadialSource(RE::Actor& a_actor,
		HeavyImpactWindRouter::ImpactKind a_kind, const std::optional<float3>& a_contactPosition,
		const HeavyImpactWindRouter::RadialProfile& a_profile, float a_masterStrength)
	{
		return WindField::MakeRadialWave(GetRadialImpactOrigin(a_actor, a_kind, a_contactPosition),
			HorizontalAimDirection(a_actor),
			std::clamp(a_profile.strength * a_masterStrength, 0.0f, kMaximumStrength),
			std::clamp(a_profile.distance, kMinimumDistance, kMaximumDistance),
			std::clamp(a_profile.waveHalfWidth, kMinimumDistance, kMaximumDistance),
			std::clamp(a_profile.propagationSpeed, 0.0f, kMaximumSpeed),
			std::clamp(a_profile.decayTime, 0.0f, WindField::kTransientImpulseMaximumDecayTime));
	}

	[[nodiscard]] WindField::TransientWindSource MakeDirectionalSource(RE::Actor& a_actor,
		const HeavyImpactWindRouter::DirectionalProfile& a_profile, float a_masterStrength,
		bool a_horizontal)
	{
		const float3 direction = a_horizontal ? HorizontalAimDirection(a_actor) : ActorWind::GetAimDirection(a_actor);
		const float halfAngle = std::clamp(a_profile.coneHalfAngle, 5.0f, 90.0f) *
		                        (std::numbers::pi_v<float> / 180.0f);
		const float3 origin = a_horizontal ? ActorWind::GetVisualOrigin(a_actor) : ActorWind::GetMagicOrigin(a_actor);
		return WindField::MakeDirectionalWave(origin, direction,
			std::clamp(a_profile.strength * a_masterStrength, 0.0f, kMaximumStrength),
			std::clamp(a_profile.distance, kMinimumDistance, kMaximumDistance),
			std::clamp(a_profile.waveHalfWidth, kMinimumDistance, kMaximumDistance),
			std::clamp(a_profile.propagationSpeed, 0.0f, kMaximumSpeed), std::cos(halfAngle),
			std::clamp(a_profile.decayTime, 0.0f, WindField::kTransientImpulseMaximumDecayTime));
	}
}

class HeavyImpactWindRouter::Impl :
	public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
	public RE::BSTEventSink<RE::TESHitEvent>,
	public RE::BSTEventSink<RE::TESSwitchRaceCompleteEvent>,
	public RE::BSTEventSink<RE::BSAnimationGraphEvent>
{
public:
	void Register()
	{
		if (registered)
			return;
		auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
		if (!eventSourceHolder) {
			logger::warn("Unable to register heavy-impact wind: game event source is unavailable");
			return;
		}
		if (auto* perkForm = RE::TESForm::LookupByEditorID("ShieldCharge"))
			shieldChargePerk = perkForm->As<RE::BGSPerk>();
		if (!shieldChargePerk)
			shieldChargePerk = RE::TESForm::LookupByID<RE::BGSPerk>(0x00058F6A);
		eventSourceHolder->AddEventSink<RE::TESObjectLoadedEvent>(this);
		eventSourceHolder->AddEventSink<RE::TESHitEvent>(this);
		eventSourceHolder->AddEventSink<RE::TESSwitchRaceCompleteEvent>(this);
		registered = true;
		if (auto* player = globals::game::player)
			SubscribeToActor(*player);
		DiscoverTrackedActors(12000.0f);
		logger::info("Registered heavy-impact wind routing");
	}

	void Update(float a_frameTime, const HeavyImpactWindRouter::Settings& a_settings)
	{
		if (!a_settings.enabled || !IsFinitePositive(a_settings.strength)) {
			ClearRuntimeState();
			return;
		}
		const float frameTime = std::isfinite(a_frameTime) ? std::max(a_frameTime, 0.0f) : 0.0f;
		elapsedTime += frameTime;
		discoveryElapsed += frameTime;
		if (discoveryElapsed >= kActorDiscoveryInterval) {
			DiscoverTrackedActors(a_settings.trackingDistance);
			discoveryElapsed = 0.0f;
		}

		std::vector<QueuedImpact> events;
		{
			std::lock_guard lock(eventMutex);
			events.swap(queuedEvents);
			pendingEventKeys.clear();
		}
		std::erase_if(activeNpcPowerAttackExpirations,
			[this](const auto& entry) { return entry.second <= elapsedTime; });
		for (const auto& event : events) {
			auto actor = event.actor.get();
			if (!actor || actor->IsDead() || !actor->Is3DLoaded() || !IsNearPlayer(*actor, a_settings.trackingDistance))
				continue;
			Emit(*actor, event.kind,
				event.hasContact ? std::optional<float3>{ event.contactPosition } : std::nullopt,
				a_settings);
		}
		std::erase_if(lastEmissionTimes,
			[this](const auto& entry) { return elapsedTime - entry.second > kDeduplicationRetention; });
	}

	void Reset()
	{
		ClearRuntimeState();
		discoveryElapsed = kActorDiscoveryInterval;
	}

	void Queue(RE::Actor& a_actor, HeavyImpactWindRouter::ImpactKind a_kind,
		const RE::TESObjectREFR* a_contact = nullptr)
	{
		std::lock_guard lock(eventMutex);
		const auto key = MakeImpactKey(a_actor.GetFormID(), a_kind);
		if (!pendingEventKeys.insert(key).second) {
			if (a_contact) {
				const auto position = a_contact->GetPosition();
				if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
					if (auto event = std::ranges::find(queuedEvents, key, &QueuedImpact::key);
						event != queuedEvents.end()) {
						event->contactPosition = { position.x, position.y, position.z };
						event->hasContact = true;
					}
				}
			}
			return;
		}
		if (queuedEvents.size() >= kMaximumQueuedEvents) {
			pendingEventKeys.erase(key);
			return;
		}

		QueuedImpact event{ a_actor.GetHandle(), a_kind, key };
		if (a_contact) {
			const auto position = a_contact->GetPosition();
			if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) {
				event.contactPosition = { position.x, position.y, position.z };
				event.hasContact = true;
			}
		}
		queuedEvents.push_back(event);
	}

private:
	RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event,
		RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
	{
		if (!a_event)
			return RE::BSEventNotifyControl::kContinue;
		if (!a_event->loaded) {
			std::lock_guard lock(subscriptionMutex);
			subscribedActorIDs.erase(a_event->formID);
			return RE::BSEventNotifyControl::kContinue;
		}
		if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_event->formID)) {
			if (actor == globals::game::player || IsTrackedAttackClass(ClassifyActor(*actor)))
				SubscribeToActor(*actor);
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event,
		RE::BSTEventSource<RE::TESHitEvent>*) override
	{
		if (!a_event || !a_event->cause || a_event->projectile != 0)
			return RE::BSEventNotifyControl::kContinue;
		auto* actor = a_event->cause->As<RE::Actor>();
		if (!actor)
			return RE::BSEventNotifyControl::kContinue;

		if (a_event->flags.any(RE::TESHitEvent::Flag::kBashAttack) && actor->IsSprinting()) {
			if (shieldChargePerk && actor->HasPerk(shieldChargePerk) && HasEquippedShield(*actor))
				Queue(*actor, ImpactKind::ShieldCharge, a_event->target.get());
			return RE::BSEventNotifyControl::kContinue;
		}
		if (a_event->flags.any(RE::TESHitEvent::Flag::kBashAttack))
			return RE::BSEventNotifyControl::kContinue;
		auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_event->source);
		const auto* attackData = GetCurrentAttackData(*actor);
		const bool powerAttack = a_event->flags.any(RE::TESHitEvent::Flag::kPowerAttack) ||
		                         IsPowerOrChargeAttack(attackData) || actor->IsPowerAttacking();
		switch (ClassifyActor(*actor)) {
		case ActorClass::Giant:
			if (IsGiantClubAttack(attackData, weapon))
				Queue(*actor, ImpactKind::GiantClubSmash, a_event->target.get());
			break;
		case ActorClass::Dragon:
			if (!actor->IsFlying() && IsDragonWingOrTailAttack(attackData, weapon))
				Queue(*actor, ImpactKind::DragonGroundAttack, a_event->target.get());
			break;
		case ActorClass::Troll:
			if (powerAttack || IsHeavyAttack(attackData))
				Queue(*actor, ImpactKind::TrollPowerAttack, a_event->target.get());
			break;
		case ActorClass::Mammoth:
			if (IsHeavyAttack(attackData))
				Queue(*actor, ImpactKind::MammothHeavyAttack, a_event->target.get());
			break;
		case ActorClass::Centurion:
			if (powerAttack || IsHeavyAttack(attackData))
				Queue(*actor, ImpactKind::CenturionHeavyAttack, a_event->target.get());
			break;
		default:
			if (powerAttack)
				Queue(*actor, ImpactKind::PowerAttack, a_event->target.get());
			break;
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl ProcessEvent(const RE::TESSwitchRaceCompleteEvent* a_event,
		RE::BSTEventSource<RE::TESSwitchRaceCompleteEvent>*) override
	{
		if (!a_event || !a_event->subject)
			return RE::BSEventNotifyControl::kContinue;
		if (auto* actor = a_event->subject->As<RE::Actor>()) {
			const auto actorClass = ClassifyActor(*actor);
			if (actorClass == ActorClass::Werewolf || actorClass == ActorClass::VampireLord)
				Queue(*actor, ImpactKind::BeastTransformation);
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
		RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
	{
		if (!a_event || !a_event->holder)
			return RE::BSEventNotifyControl::kContinue;
		auto* actor = a_event->holder->As<RE::Actor>();
		if (!actor)
			return RE::BSEventNotifyControl::kContinue;
		const auto actorClass = ClassifyActor(*actor);
		const std::string_view tag = a_event->tag.c_str();
		const auto* attackData = GetCurrentAttackData(*actor);
		if (tag != "HitFrame")
			return RE::BSEventNotifyControl::kContinue;

		switch (actorClass) {
		case ActorClass::Giant:
			if (IsGiantClubAttack(attackData, nullptr))
				Queue(*actor, ImpactKind::GiantClubSmash);
			break;
		case ActorClass::Dragon:
			if (!actor->IsFlying() && IsDragonWingOrTailAttack(attackData, nullptr))
				Queue(*actor, ImpactKind::DragonGroundAttack);
			break;
		case ActorClass::Troll:
			if (actor->IsPowerAttacking() || IsHeavyAttack(attackData))
				Queue(*actor, ImpactKind::TrollPowerAttack);
			break;
		case ActorClass::Mammoth:
			if (IsHeavyAttack(attackData))
				Queue(*actor, ImpactKind::MammothHeavyAttack);
			break;
		case ActorClass::Centurion:
			if (actor->IsPowerAttacking() || IsHeavyAttack(attackData))
				Queue(*actor, ImpactKind::CenturionHeavyAttack);
			break;
		default:
			if (actor == globals::game::player &&
				(actor->IsPowerAttacking() || IsPowerOrChargeAttack(attackData))) {
				Queue(*actor, ImpactKind::PowerAttack);
			}
			break;
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	void DiscoverTrackedActors(float a_trackingDistance)
	{
		auto* player = globals::game::player;
		if (!player)
			return;
		SubscribeToActor(*player);
		auto* processLists = RE::ProcessLists::GetSingleton();
		if (!processLists)
			return;
		const float trackingDistance = std::isfinite(a_trackingDistance) ?
		                                   std::max(a_trackingDistance, 0.0f) :
		                                   0.0f;
		const float squaredTrackingDistance = trackingDistance * trackingDistance;
		const auto playerPosition = player->GetPosition();
		for (const auto& handle : processLists->highActorHandles) {
			auto actor = handle.get();
			if (!actor || !actor->Is3DLoaded() || actor->IsDead() ||
				actor->GetPosition().GetSquaredDistance(playerPosition) > squaredTrackingDistance ||
				!IsTrackedAttackClass(ClassifyActor(*actor))) {
				continue;
			}
			SubscribeToActor(*actor);
		}
	}

	void SubscribeToActor(RE::Actor& a_actor)
	{
		bool shouldSubscribe;
		{
			std::lock_guard lock(subscriptionMutex);
			shouldSubscribe = subscribedActorIDs.insert(a_actor.GetFormID()).second;
		}
		if (shouldSubscribe)
			a_actor.AddAnimationGraphEventSink(this);
	}

	[[nodiscard]] bool IsNearPlayer(RE::Actor& a_actor, float a_trackingDistance) const
	{
		auto* player = globals::game::player;
		if (!player || !std::isfinite(a_trackingDistance) || a_trackingDistance <= 0.0f)
			return false;
		return a_actor.GetPosition().GetSquaredDistance(player->GetPosition()) <=
		       a_trackingDistance * a_trackingDistance;
	}

	void Emit(RE::Actor& a_actor, ImpactKind a_kind, const std::optional<float3>& a_contactPosition,
		const Settings& a_settings)
	{
		if (a_kind == ImpactKind::PowerAttack && !a_settings.powerAttacks)
			return;
		const auto key = MakeImpactKey(a_actor.GetFormID(), a_kind);
		const double deduplicationTime = std::isfinite(a_settings.deduplicationTime) ?
		                                     std::max(a_settings.deduplicationTime, 0.0f) :
		                                     0.35f;
		if (const auto iterator = lastEmissionTimes.find(key);
			iterator != lastEmissionTimes.end() && elapsedTime - iterator->second < deduplicationTime) {
			return;
		}

		WindField::TransientWindSource source;
		switch (a_kind) {
		case ImpactKind::PowerAttack:
			source = MakeDirectionalSource(a_actor, a_settings.powerAttack, a_settings.strength, true);
			break;
		case ImpactKind::GiantClubSmash:
			source = MakeRadialSource(
				a_actor, a_kind, a_contactPosition, a_settings.giantClub, a_settings.strength);
			break;
		case ImpactKind::DragonGroundAttack:
			source = MakeRadialSource(
				a_actor, a_kind, a_contactPosition, a_settings.dragonGroundAttack, a_settings.strength);
			break;
		case ImpactKind::TrollPowerAttack:
			source = MakeRadialSource(
				a_actor, a_kind, a_contactPosition, a_settings.trollPowerAttack, a_settings.strength);
			break;
		case ImpactKind::MammothHeavyAttack:
			source = MakeRadialSource(
				a_actor, a_kind, a_contactPosition, a_settings.mammothHeavyAttack, a_settings.strength);
			break;
		case ImpactKind::CenturionHeavyAttack:
			source = MakeRadialSource(
				a_actor, a_kind, a_contactPosition, a_settings.centurionHeavyAttack, a_settings.strength);
			break;
		case ImpactKind::BeastTransformation:
			source = MakeRadialSource(
				a_actor, a_kind, a_contactPosition, a_settings.beastTransformation, a_settings.strength);
			break;
		case ImpactKind::ShieldCharge:
			source = MakeDirectionalSource(a_actor, a_settings.shieldCharge, a_settings.strength, true);
			break;
		}
		if (!IsFinitePositive(source.strength))
			return;
		if (a_kind == ImpactKind::PowerAttack && &a_actor != globals::game::player) {
			const auto actorFormID = a_actor.GetFormID();
			const bool alreadyActive = activeNpcPowerAttackExpirations.contains(actorFormID);
			if (!alreadyActive && activeNpcPowerAttackExpirations.size() >= kMaximumNpcPowerAttackSources)
				return;
			const float propagationSpeed = std::max(source.propagationSpeed, 0.0f);
			const double lifetime = propagationSpeed > 1e-5f ?
			                            (std::max(source.maxDistance, 0.0f) + std::abs(source.waveHalfWidth) +
											propagationSpeed * std::max(source.decayTime, 0.0f)) /
			                                propagationSpeed :
			                            std::numeric_limits<double>::infinity();
			activeNpcPowerAttackExpirations.insert_or_assign(actorFormID, elapsedTime + lifetime);
		}
		State::GetSingleton()->QueueTransientWindSource(source, State::TransientWindSourceOwner::HeavyImpact,
			State::TransientWindSourcePriority::Impact);
		lastEmissionTimes[key] = elapsedTime;
	}

	void ClearRuntimeState()
	{
		std::lock_guard lock(eventMutex);
		queuedEvents.clear();
		pendingEventKeys.clear();
		lastEmissionTimes.clear();
		activeNpcPowerAttackExpirations.clear();
	}

	std::vector<QueuedImpact> queuedEvents;
	std::unordered_set<std::uint64_t> pendingEventKeys;
	std::unordered_map<std::uint64_t, double> lastEmissionTimes;
	std::unordered_map<RE::FormID, double> activeNpcPowerAttackExpirations;
	std::mutex eventMutex;
	std::unordered_set<RE::FormID> subscribedActorIDs;
	std::mutex subscriptionMutex;
	RE::BGSPerk* shieldChargePerk{};
	double elapsedTime{};
	float discoveryElapsed = kActorDiscoveryInterval;
	bool registered{};
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HeavyImpactWindRouter::RadialProfile,
	strength,
	distance,
	waveHalfWidth,
	propagationSpeed,
	decayTime)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HeavyImpactWindRouter::DirectionalProfile,
	strength,
	distance,
	waveHalfWidth,
	propagationSpeed,
	coneHalfAngle,
	decayTime)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HeavyImpactWindRouter::Settings,
	enabled,
	strength,
	trackingDistance,
	deduplicationTime,
	powerAttacks,
	powerAttack,
	giantClub,
	dragonGroundAttack,
	trollPowerAttack,
	mammothHeavyAttack,
	centurionHeavyAttack,
	beastTransformation,
	shieldCharge)

HeavyImpactWindRouter::HeavyImpactWindRouter() :
	implementation(std::make_unique<Impl>())
{}

HeavyImpactWindRouter::~HeavyImpactWindRouter() = default;

std::string HeavyImpactWindRouter::GetDisplayName() const
{
	return T("feature.wind.wind_effect.heavy_impacts.name", "Heavy Impacts");
}

void HeavyImpactWindRouter::DrawSettings()
{
	if (ImGui::Checkbox(T("feature.wind.wind_effect.heavy_impacts.enabled", "Enable Heavy Impacts"),
			&settings.enabled) &&
		!settings.enabled) {
		Reset();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.heavy_impacts.enabled_tooltip",
			"Adds pressure waves for power attacks, giant, dragon, troll, mammoth, Centurion, transformation, and Shield Charge events."));

	ImGui::BeginDisabled(!settings.enabled);
	ImGui::Checkbox(T("feature.wind.wind_effect.heavy_impacts.power_attacks", "Power Attack Impacts"),
		&settings.powerAttacks);
	ImGui::BeginDisabled(!settings.powerAttacks);
	ImGui::SliderFloat(T("feature.wind.wind_effect.heavy_impacts.power_attack_strength", "Power Attack Strength"),
		&settings.powerAttack.strength, 0.0f, kMaximumStrength, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::SliderFloat(T("feature.wind.wind_effect.heavy_impacts.strength", "Master Strength##HeavyImpacts"), &settings.strength, 0.0f, kMaximumStrength, "%.2fx",
		ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.heavy_impacts.tracking_distance", "Tracking Distance##HeavyImpacts"), &settings.trackingDistance, kMinimumDistance,
		kMaximumDistance, "%.0f units", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat(T("feature.wind.wind_effect.heavy_impacts.deduplication", "Deduplication Window##HeavyImpacts"), &settings.deduplicationTime, 0.0f, kMaximumTime,
		"%.2f s", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
}

void HeavyImpactWindRouter::LoadSettings(const nlohmann::json& a_json)
{
	settings = a_json;
	SanitizeSettings();
}

void HeavyImpactWindRouter::SaveSettings(nlohmann::json& a_json) const
{
	a_json = settings;
}

void HeavyImpactWindRouter::RestoreDefaultSettings()
{
	settings = {};
	Reset();
}

void HeavyImpactWindRouter::DataLoaded()
{
	implementation->Register();
}

void HeavyImpactWindRouter::Update(float a_frameTime)
{
	implementation->Update(a_frameTime, settings);
}

void HeavyImpactWindRouter::Reset()
{
	implementation->Reset();
	State::GetSingleton()->ClearTransientWindSources(State::TransientWindSourceOwner::HeavyImpact);
}

void HeavyImpactWindRouter::QueueImpact(RE::Actor& a_actor, ImpactKind a_kind)
{
	implementation->Queue(a_actor, a_kind);
}

void HeavyImpactWindRouter::SanitizeSettings()
{
	settings.strength = std::isfinite(settings.strength) ? std::clamp(settings.strength, 0.0f, kMaximumStrength) : 1.0f;
	settings.trackingDistance = std::isfinite(settings.trackingDistance) ?
	                                std::clamp(settings.trackingDistance, kMinimumDistance, kMaximumDistance) :
	                                12000.0f;
	settings.deduplicationTime = std::isfinite(settings.deduplicationTime) ?
	                                 std::clamp(settings.deduplicationTime, 0.0f, kMaximumTime) :
	                                 0.35f;

	const auto sanitizeRadial = [](RadialProfile& a_profile, const RadialProfile& a_defaults) {
		a_profile.strength = std::isfinite(a_profile.strength) ?
		                         std::clamp(a_profile.strength, 0.0f, kMaximumStrength) :
		                         a_defaults.strength;
		a_profile.distance = std::isfinite(a_profile.distance) ?
		                         std::clamp(a_profile.distance, kMinimumDistance, kMaximumDistance) :
		                         a_defaults.distance;
		a_profile.waveHalfWidth = std::isfinite(a_profile.waveHalfWidth) ?
		                              std::clamp(a_profile.waveHalfWidth, kMinimumDistance, kMaximumDistance) :
		                              a_defaults.waveHalfWidth;
		a_profile.propagationSpeed = std::isfinite(a_profile.propagationSpeed) ?
		                                 std::clamp(a_profile.propagationSpeed, 0.0f, kMaximumSpeed) :
		                                 a_defaults.propagationSpeed;
		a_profile.decayTime = std::isfinite(a_profile.decayTime) ?
		                          std::clamp(a_profile.decayTime, 0.0f, kMaximumTime) :
		                          a_defaults.decayTime;
	};
	const auto sanitizeDirectional = [&](DirectionalProfile& a_profile, const DirectionalProfile& a_defaults) {
		a_profile.strength = std::isfinite(a_profile.strength) ?
		                         std::clamp(a_profile.strength, 0.0f, kMaximumStrength) :
		                         a_defaults.strength;
		a_profile.distance = std::isfinite(a_profile.distance) ?
		                         std::clamp(a_profile.distance, kMinimumDistance, kMaximumDistance) :
		                         a_defaults.distance;
		a_profile.waveHalfWidth = std::isfinite(a_profile.waveHalfWidth) ?
		                              std::clamp(a_profile.waveHalfWidth, kMinimumDistance, kMaximumDistance) :
		                              a_defaults.waveHalfWidth;
		a_profile.propagationSpeed = std::isfinite(a_profile.propagationSpeed) ?
		                                 std::clamp(a_profile.propagationSpeed, 0.0f, kMaximumSpeed) :
		                                 a_defaults.propagationSpeed;
		a_profile.coneHalfAngle = std::isfinite(a_profile.coneHalfAngle) ?
		                              std::clamp(a_profile.coneHalfAngle, 5.0f, 90.0f) :
		                              a_defaults.coneHalfAngle;
		a_profile.decayTime = std::isfinite(a_profile.decayTime) ?
		                          std::clamp(a_profile.decayTime, 0.0f, kMaximumTime) :
		                          a_defaults.decayTime;
	};
	const Settings defaults;
	sanitizeDirectional(settings.powerAttack, defaults.powerAttack);
	sanitizeRadial(settings.giantClub, defaults.giantClub);
	sanitizeRadial(settings.dragonGroundAttack, defaults.dragonGroundAttack);
	sanitizeRadial(settings.trollPowerAttack, defaults.trollPowerAttack);
	sanitizeRadial(settings.mammothHeavyAttack, defaults.mammothHeavyAttack);
	sanitizeRadial(settings.centurionHeavyAttack, defaults.centurionHeavyAttack);
	sanitizeRadial(settings.beastTransformation, defaults.beastTransformation);
	sanitizeDirectional(settings.shieldCharge, defaults.shieldCharge);
}
