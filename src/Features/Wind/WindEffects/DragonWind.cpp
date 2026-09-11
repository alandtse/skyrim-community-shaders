#include "DragonWind.h"

#include "ActorWind.h"
#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindMath.h"
#include "FusRoDahWind.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "ShoutWindProfiles.h"
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
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DragonWindRuntime
{
	namespace
	{
		constexpr std::size_t kFallbackActorsPerScan = 64;
		constexpr float kFallbackScanInterval = 0.5f;
		constexpr float kTrackingRefreshInterval = 0.25f;
		constexpr float kMaximumTrackedVelocity = 10000.0f;
		constexpr float kTeleportDistance = 4096.0f;
		constexpr float kVelocitySmoothingTime = 0.15f;
		constexpr float kHoverWingbeatInterval = 1.0f;
		constexpr float kCrashImpactFallbackTime = 0.15f;
		constexpr float kStrengthMin = 0.0f;
		constexpr float kStrengthMax = 5.0f;
		constexpr float kDistanceMin = 100.0f;
		constexpr float kDistanceMax = 30000.0f;
		constexpr float kSpeedMin = 0.0f;
		constexpr float kSpeedMax = 10000.0f;
		constexpr float kTimeMin = 0.0f;
		constexpr float kTimeMax = WindField::kTransientImpulseMaximumDecayTime;
		constexpr float kWingbeatConeHalfAngle = 50.0f;
		constexpr float kWingbeatHorizontalDrift = 0.35f;

		float LengthSquared(const float3& a_value) noexcept
		{
			return a_value.x * a_value.x + a_value.y * a_value.y + a_value.z * a_value.z;
		}

		float Length(const float3& a_value) noexcept
		{
			return std::sqrt(std::max(LengthSquared(a_value), 0.0f));
		}

		float3 Normalize(const float3& a_value) noexcept
		{
			const float length = Length(a_value);
			return std::isfinite(length) && length > 1e-4f ? a_value / length : float3{};
		}

		bool IsFinite(const float3& a_value) noexcept
		{
			return std::isfinite(a_value.x) && std::isfinite(a_value.y) && std::isfinite(a_value.z);
		}

		bool IsHovering(RE::Actor& a_actor)
		{
			std::int32_t animationState{};
			std::int32_t hoveringState{};
			if (a_actor.GetGraphVariableInt("iState", animationState) &&
				a_actor.GetGraphVariableInt("iState_DragonHovering", hoveringState)) {
				return animationState == hoveringState;
			}

			return a_actor.GetFlyState() == RE::FLY_STATE::kHovering;
		}

		enum class EventType
		{
			Thrust,
			WingSound,
			NormalImpact,
			ForcefulImpact,
			CrashStart,
			Shout
		};

		struct QueuedEvent
		{
			RE::ActorHandle actor;
			EventType type;
			RE::FormID spellFormID{};
		};

		struct DragonShoutRoute
		{
			uint8_t rank{};
		};

		struct DragonState
		{
			RE::ActorHandle handle;
			float3 previousPosition{};
			float3 filteredVelocity{};
			float thrustElapsed = std::numeric_limits<float>::max();
			float wingbeatElapsed = std::numeric_limits<float>::max();
			float impactElapsed = std::numeric_limits<float>::max();
			float crashFallbackRemaining{};
			bool hasPreviousPosition{};
			bool crashImpactArmed{};
		};

		struct TrackingEvent
		{
			RE::ActorHandle actor;
			RE::FormID formID{};
			bool loaded{};
		};

		class Manager :
			public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
			public RE::BSTEventSink<RE::BSAnimationGraphEvent>,
			public RE::BSTEventSink<RE::TESSpellCastEvent>
		{
		public:
			static Manager& GetSingleton()
			{
				static Manager singleton;
				return singleton;
			}

			void Register()
			{
				if (registered)
					return;
				auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
				auto* dataHandler = RE::TESDataHandler::GetSingleton();
				if (!eventSourceHolder || !dataHandler) {
					logger::warn("Unable to register dragon wind: game event source is unavailable");
					return;
				}
				if (auto* dragonKeywordForm = RE::TESForm::LookupByEditorID("ActorTypeDragon"))
					dragonKeyword = dragonKeywordForm->As<RE::BGSKeyword>();

				std::unordered_set<RE::FormID> fusRoDahSpells;
				FusRoDahWind::CollectOwnedMagicItems(*dataHandler, fusRoDahSpells);
				dragonShoutRoutes.clear();
				for (const auto* shout : dataHandler->GetFormArray<RE::TESShout>()) {
					if (!shout)
						continue;
					for (uint8_t rank = 0; rank < RE::TESShout::VariationIDs::kTotal; ++rank) {
						const auto* spell = shout->variations[rank].spell;
						if (spell && !fusRoDahSpells.contains(spell->GetFormID()))
							dragonShoutRoutes.insert_or_assign(spell->GetFormID(), DragonShoutRoute{ rank });
					}
				}
				eventSourceHolder->AddEventSink<RE::TESObjectLoadedEvent>(this);
				eventSourceHolder->AddEventSink<RE::TESSpellCastEvent>(this);
				registered = true;
				logger::info("Registered dragon wind tracking{} with {} non-Fus shout variations",
					dragonKeyword ? "" : " with behavior-graph fallback", dragonShoutRoutes.size());
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event,
				RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (!a_event)
					return RE::BSEventNotifyControl::kContinue;

				if (!a_event->loaded) {
					std::lock_guard lock(eventMutex);
					if (subscribedDragonIDs.erase(a_event->formID) != 0)
						trackingEvents.push_back({ {}, a_event->formID, false });
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_event->formID);
				if (!IsDragon(actor))
					return RE::BSEventNotifyControl::kContinue;

				bool subscribe = false;
				{
					std::lock_guard lock(eventMutex);
					subscribe = subscribedDragonIDs.insert(a_event->formID).second;
					trackingEvents.push_back({ actor->GetHandle(), a_event->formID, true });
				}
				if (subscribe)
					actor->AddAnimationGraphEventSink(this);
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

				const std::string_view tag = a_event->tag.c_str();
				QueuedEvent queuedEvent{ actor->GetHandle(), EventType::Thrust };
				bool queue = true;
				if (tag == "FlapThrustBegin") {
					queuedEvent.type = EventType::Thrust;
				} else if (tag.find("SoundPlay.NPCDragonWingFlap") != std::string_view::npos) {
					queuedEvent.type = EventType::WingSound;
				} else if (tag == "DragonLandEffect") {
					queuedEvent.type = EventType::NormalImpact;
				} else if (tag == "DragonForcefulLandEffect") {
					queuedEvent.type = EventType::ForcefulImpact;
				} else if (tag == "FlightCrashLandStart") {
					queuedEvent.type = EventType::CrashStart;
				} else {
					queue = false;
				}

				if (queue) {
					std::lock_guard lock(eventMutex);
					queuedEvents.push_back(queuedEvent);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
				RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (!a_event || !a_event->object || !dragonShoutRoutes.contains(a_event->spell))
					return RE::BSEventNotifyControl::kContinue;
				auto* actor = a_event->object->As<RE::Actor>();
				if (!IsDragon(actor))
					return RE::BSEventNotifyControl::kContinue;

				std::lock_guard lock(eventMutex);
				const auto duplicate = std::ranges::find_if(queuedEvents, [&](const QueuedEvent& a_pending) {
					return a_pending.type == EventType::Shout &&
					       a_pending.actor == actor->GetHandle() && a_pending.spellFormID == a_event->spell;
				});
				if (duplicate == queuedEvents.end())
					queuedEvents.push_back({ actor->GetHandle(), EventType::Shout, a_event->spell });
				return RE::BSEventNotifyControl::kContinue;
			}

			void Update(float a_frameTime, const DragonWind::Settings& a_settings)
			{
				const bool trackingChanged = DrainTrackingEvents();
				if (!a_settings.enabled || a_settings.strength <= 0.0f) {
					Reset();
					return;
				}

				const float frameTime = std::isfinite(a_frameTime) ? std::max(a_frameTime, 0.0f) : 0.0f;
				fallbackScanRemaining -= frameTime;
				bool fallbackChanged = false;
				if (fallbackScanRemaining <= 0.0f) {
					fallbackChanged = ScanFallbackActors();
					fallbackScanRemaining = kFallbackScanInterval;
				}

				trackingRefreshRemaining -= frameTime;
				const bool trackingDistanceChanged =
					!std::isfinite(lastTrackingDistance) || lastTrackingDistance != a_settings.trackingDistance;
				if (trackingChanged || fallbackChanged || trackingDistanceChanged || trackingRefreshRemaining <= 0.0f) {
					RefreshTrackedDragons(a_settings.trackingDistance);
					lastTrackingDistance = a_settings.trackingDistance;
					trackingRefreshRemaining = kTrackingRefreshInterval;
				}
				AdvanceTimers(frameTime);
				DrainEvents(a_settings);

				for (auto& entry : dragonStates) {
					auto& dragonState = entry.second;
					if (auto actor = dragonState.handle.get()) {
						UpdateVelocity(*actor, dragonState, frameTime);
						if (a_settings.wingbeatsEnabled &&
							IsHovering(*actor) &&
							dragonState.wingbeatElapsed >= kHoverWingbeatInterval) {
							QueueWingbeat(*actor, dragonState, a_settings);
							dragonState.wingbeatElapsed = 0.0f;
						}
					}
				}
			}

			void Reset()
			{
				{
					std::lock_guard lock(eventMutex);
					queuedEvents.clear();
				}
				dragonStates.clear();
				trackingRefreshRemaining = 0.0f;
				State::GetSingleton()->ClearTransientWindSources(State::TransientWindSourceOwner::Dragon);
			}

		private:
			bool IsDragon(RE::Actor* a_actor) const
			{
				if (!a_actor)
					return false;
				auto* race = a_actor->GetRace();
				if (!race)
					return false;
				if (dragonKeyword)
					return race->HasKeyword(dragonKeyword);
				for (const auto& behaviorGraph : race->behaviorGraphs) {
					const char* model = behaviorGraph.GetModel();
					if (!model)
						continue;
					const std::string_view path(model);
					constexpr std::string_view dragonGraph = "dragonbehavior.hkx";
					const auto match = std::search(path.begin(), path.end(), dragonGraph.begin(), dragonGraph.end(),
						[](unsigned char left, unsigned char right) {
							return std::tolower(left) == std::tolower(right);
						});
					if (match != path.end())
						return true;
				}
				return false;
			}

			bool DrainTrackingEvents()
			{
				std::vector<TrackingEvent> events;
				{
					std::lock_guard lock(eventMutex);
					events.swap(trackingEvents);
				}
				for (const auto& event : events) {
					if (event.loaded)
						knownDragons[event.formID] = event.actor;
					else {
						knownDragons.erase(event.formID);
						dragonStates.erase(event.formID);
					}
				}
				return !events.empty();
			}

			bool ScanFallbackActors()
			{
				auto* processLists = RE::ProcessLists::GetSingleton();
				if (!processLists || processLists->highActorHandles.empty()) {
					fallbackActorCursor = 0;
					return false;
				}

				const auto actorCount = processLists->highActorHandles.size();
				fallbackActorCursor %= actorCount;
				const auto scanCount = std::min<std::size_t>(actorCount, kFallbackActorsPerScan);
				bool changed = false;
				for (std::size_t offset = 0; offset < scanCount; ++offset) {
					const auto index = static_cast<std::uint32_t>(
						(fallbackActorCursor + offset) % actorCount);
					const auto& handle = processLists->highActorHandles[index];
					auto actor = handle.get();
					if (!actor || !actor->Is3DLoaded() || actor->IsDead() || !IsDragon(actor.get()))
						continue;

					const auto formID = actor->GetFormID();
					bool subscribe = false;
					{
						std::lock_guard lock(eventMutex);
						subscribe = subscribedDragonIDs.insert(formID).second;
					}
					if (subscribe)
						actor->AddAnimationGraphEventSink(this);
					changed |= knownDragons.insert_or_assign(formID, handle).second;
				}
				fallbackActorCursor = (fallbackActorCursor + scanCount) % actorCount;
				return changed;
			}

			void RefreshTrackedDragons(float a_trackingDistance)
			{
				auto* player = globals::game::player;
				if (!player) {
					dragonStates.clear();
					return;
				}
				const float trackingDistance = std::isfinite(a_trackingDistance) ?
				                                   std::max(a_trackingDistance, 0.0f) :
				                                   0.0f;
				const float squaredTrackingDistance = trackingDistance * trackingDistance;
				const auto playerPosition = player->GetPosition();
				std::unordered_set<RE::FormID> trackedDragonIDs;
				trackedDragonIDs.reserve(knownDragons.size());
				for (auto iterator = knownDragons.begin(); iterator != knownDragons.end();) {
					auto actor = iterator->second.get();
					if (!actor || !actor->Is3DLoaded()) {
						const auto staleFormID = iterator->first;
						iterator = knownDragons.erase(iterator);
						dragonStates.erase(staleFormID);
						std::lock_guard lock(eventMutex);
						subscribedDragonIDs.erase(staleFormID);
						continue;
					}

					const auto formID = iterator->first;
					++iterator;
					if (actor->IsDead())
						continue;
					const float squaredDistance = actor->GetPosition().GetSquaredDistance(playerPosition);
					if (squaredDistance > squaredTrackingDistance)
						continue;

					trackedDragonIDs.insert(formID);
					auto state = dragonStates.try_emplace(formID).first;
					state->second.handle = actor->GetHandle();
				}
				std::erase_if(dragonStates, [&](const auto& entry) {
					return !trackedDragonIDs.contains(entry.first);
				});
			}

			void AdvanceTimers(float a_frameTime)
			{
				for (auto& entry : dragonStates) {
					auto& state = entry.second;
					state.thrustElapsed += a_frameTime;
					state.wingbeatElapsed += a_frameTime;
					state.impactElapsed += a_frameTime;
					if (state.crashImpactArmed && state.crashFallbackRemaining > 0.0f)
						state.crashFallbackRemaining -= a_frameTime;
				}
			}

			void DrainEvents(const DragonWind::Settings& a_settings)
			{
				std::vector<QueuedEvent> events;
				{
					std::lock_guard lock(eventMutex);
					events.swap(queuedEvents);
				}
				for (const auto& event : events) {
					auto actor = event.actor.get();
					if (!actor)
						continue;
					auto iterator = dragonStates.find(actor->GetFormID());
					if (iterator == dragonStates.end())
						continue;
					auto& state = iterator->second;
					switch (event.type) {
					case EventType::Thrust:
						if (!a_settings.wingbeatsEnabled)
							break;
						if (state.wingbeatElapsed >= a_settings.wingbeatFallbackCooldown)
							QueueWingbeat(*actor, state, a_settings);
						state.thrustElapsed = 0.0f;
						state.wingbeatElapsed = 0.0f;
						break;
					case EventType::WingSound:
						if (!a_settings.wingbeatsEnabled)
							break;
						if (state.thrustElapsed >= a_settings.wingbeatFallbackCooldown &&
							state.wingbeatElapsed >= a_settings.wingbeatFallbackCooldown) {
							QueueWingbeat(*actor, state, a_settings);
							state.wingbeatElapsed = 0.0f;
						}
						break;
					case EventType::NormalImpact:
					case EventType::ForcefulImpact:
						{
							const bool crashImpact = state.crashImpactArmed && a_settings.crashesEnabled;
							if (!crashImpact && !a_settings.landingsEnabled) {
								state.crashImpactArmed = false;
								break;
							}
							const auto& impactProfile = crashImpact ?
							                                a_settings.crashImpact :
							                            event.type == EventType::NormalImpact ?
							                                a_settings.normalImpact :
							                                a_settings.forcefulImpact;
							QueueImpact(*actor, state, impactProfile, a_settings);
							state.crashImpactArmed = false;
							break;
						}
					case EventType::CrashStart:
						if (!a_settings.crashesEnabled) {
							state.crashImpactArmed = false;
							break;
						}
						state.crashImpactArmed = true;
						state.crashFallbackRemaining = kCrashImpactFallbackTime;
						break;
					case EventType::Shout:
						if (a_settings.shoutsEnabled)
							QueueShout(*actor, event, a_settings);
						break;
					}
				}

				for (auto& entry : dragonStates) {
					auto& state = entry.second;
					if (state.crashImpactArmed && state.crashFallbackRemaining <= 0.0f) {
						if (a_settings.crashesEnabled) {
							if (auto actor = state.handle.get())
								QueueImpact(*actor, state, a_settings.crashImpact, a_settings);
						}
						state.crashImpactArmed = false;
					}
				}
			}

			void UpdateVelocity(RE::Actor& a_actor, DragonState& a_state, float a_frameTime)
			{
				const float3 position = ActorWind::GetVisualOrigin(a_actor);
				float3 sampledVelocity{};
				RE::NiPoint3 linearVelocity{};
				a_actor.GetLinearVelocity(linearVelocity);
				const float3 engineVelocity{ linearVelocity.x, linearVelocity.y, linearVelocity.z };
				if (IsFinite(engineVelocity) && Length(engineVelocity) <= kMaximumTrackedVelocity)
					sampledVelocity = engineVelocity;

				if (a_state.hasPreviousPosition && a_frameTime > 1e-4f) {
					const float3 positionDelta = position - a_state.previousPosition;
					const float deltaLength = Length(positionDelta);
					if (std::isfinite(deltaLength) && deltaLength <= kTeleportDistance) {
						const float3 positionVelocity = positionDelta / a_frameTime;
						if (IsFinite(positionVelocity) && Length(positionVelocity) <= kMaximumTrackedVelocity)
							sampledVelocity = positionVelocity;
					} else {
						a_state.filteredVelocity = {};
					}
				}
				if (a_frameTime > 0.0f) {
					const float blend = 1.0f - std::exp(-a_frameTime / kVelocitySmoothingTime);
					a_state.filteredVelocity += (sampledVelocity - a_state.filteredVelocity) * blend;
				}
				a_state.previousPosition = position;
				a_state.hasPreviousPosition = true;
			}

			void QueueWingbeat(RE::Actor& a_actor, const DragonState& a_state,
				const DragonWind::Settings& a_settings)
			{
				float3 horizontalDirection = Normalize(
					{ a_state.filteredVelocity.x, a_state.filteredVelocity.y, 0.0f });
				if (LengthSquared(horizontalDirection) <= 1e-6f) {
					const auto aimDirection = ActorWind::GetAimDirection(a_actor);
					horizontalDirection = Normalize({ aimDirection.x, aimDirection.y, 0.0f });
				}
				const float3 downwashDirection = Normalize({ horizontalDirection.x * kWingbeatHorizontalDrift,
					horizontalDirection.y * kWingbeatHorizontalDrift,
					-1.0f });
				const float coneHalfAngleRadians = kWingbeatConeHalfAngle *
				                                   (std::numbers::pi_v<float> / 180.0f);
				const auto source = WindField::MakeDirectionalWave(
					ActorWind::GetVisualOrigin(a_actor), downwashDirection,
					a_settings.strength * a_settings.wingbeatStrength,
					a_settings.wingbeatDistance, a_settings.wingbeatWaveHalfWidth,
					a_settings.wingbeatPropagationSpeed, std::cos(coneHalfAngleRadians),
					a_settings.wingbeatDecayTime);
				State::GetSingleton()->QueueTransientWindSource(source,
					State::TransientWindSourceOwner::Dragon,
					State::TransientWindSourcePriority::Wingbeat);
			}

			void QueueImpact(RE::Actor& a_actor, DragonState& a_state,
				const DragonWind::ImpactProfile& a_profile,
				const DragonWind::Settings& a_settings)
			{
				if (a_state.impactElapsed < a_settings.impactDeduplicationTime)
					return;
				const auto actorPosition = a_actor.GetPosition();
				float3 fallbackDirection = Normalize(
					{ a_state.filteredVelocity.x, a_state.filteredVelocity.y, 0.0f });
				const auto source = WindField::MakeRadialWave(
					{ actorPosition.x, actorPosition.y, actorPosition.z }, fallbackDirection,
					a_settings.strength * a_profile.strength, a_profile.distance,
					a_profile.waveHalfWidth, a_profile.propagationSpeed,
					a_settings.impactDecayTime);
				State::GetSingleton()->QueueTransientWindSource(source,
					State::TransientWindSourceOwner::Dragon,
					State::TransientWindSourcePriority::Impact);
				a_state.impactElapsed = 0.0f;
			}

			void QueueShout(RE::Actor& a_actor, const QueuedEvent& a_event,
				const DragonWind::Settings& a_settings) const
			{
				const auto route = dragonShoutRoutes.find(a_event.spellFormID);
				if (route == dragonShoutRoutes.end())
					return;
				const std::size_t rank = std::min<std::size_t>(
					route->second.rank, ShoutWindProfiles::kGenericDragonShoutProfiles.size() - 1);
				const auto& profile = ShoutWindProfiles::kGenericDragonShoutProfiles[rank];
				const float coneRadians = profile.coneHalfAngle * (std::numbers::pi_v<float> / 180.0f);
				const auto source = WindField::MakeDirectionalWave(
					ActorWind::GetMagicOrigin(a_actor), ActorWind::GetAimDirection(a_actor),
					a_settings.strength * profile.strength, profile.distance, profile.waveHalfWidth,
					profile.propagationSpeed, std::cos(coneRadians), profile.decayTime);
				State::GetSingleton()->QueueTransientWindSource(source,
					State::TransientWindSourceOwner::Dragon,
					State::TransientWindSourcePriority::Breath);
			}

			RE::BGSKeyword* dragonKeyword{};
			std::unordered_map<RE::FormID, RE::ActorHandle> knownDragons;
			std::unordered_map<RE::FormID, DragonState> dragonStates;
			std::unordered_map<RE::FormID, DragonShoutRoute> dragonShoutRoutes;
			std::vector<QueuedEvent> queuedEvents;
			std::vector<TrackingEvent> trackingEvents;
			std::unordered_set<RE::FormID> subscribedDragonIDs;
			std::mutex eventMutex;
			std::size_t fallbackActorCursor{};
			float fallbackScanRemaining{};
			float trackingRefreshRemaining{};
			float lastTrackingDistance = std::numeric_limits<float>::quiet_NaN();
			bool registered{};
		};
	}

	void Register()
	{
		Manager::GetSingleton().Register();
	}

	void Update(float a_frameTime, const DragonWind::Settings& a_settings)
	{
		Manager::GetSingleton().Update(a_frameTime, a_settings);
	}

	void Reset()
	{
		Manager::GetSingleton().Reset();
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DragonWind::ImpactProfile,
	strength,
	distance,
	waveHalfWidth,
	propagationSpeed)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DragonWind::Settings,
	enabled,
	wingbeatsEnabled,
	landingsEnabled,
	crashesEnabled,
	shoutsEnabled,
	strength,
	trackingDistance,
	wingbeatStrength,
	wingbeatDistance,
	wingbeatWaveHalfWidth,
	wingbeatPropagationSpeed,
	wingbeatDecayTime,
	wingbeatFallbackCooldown,
	normalImpact,
	forcefulImpact,
	crashImpact,
	impactDecayTime,
	impactDeduplicationTime)

std::string DragonWind::GetDisplayName() const
{
	return T("feature.wind.wind_effect.dragon.name", "Dragons");
}

void DragonWind::DrawSettings()
{
	if (ImGui::Checkbox(T("feature.wind.wind_effect.dragon.enabled", "Enable Dragon Wind"), &settings.enabled) &&
		!settings.enabled)
		Reset();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.dragon.enabled_tooltip",
			"Makes nearby dragon wingbeats, landings, crashes, and shouts disturb shared visual wind consumers."));

	ImGui::BeginDisabled(!settings.enabled);
	ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.strength", "Master Strength"), &settings.strength,
		DragonWindRuntime::kStrengthMin, DragonWindRuntime::kStrengthMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.tracking_distance", "Tracking Distance"),
		&settings.trackingDistance, DragonWindRuntime::kDistanceMin, DragonWindRuntime::kDistanceMax,
		"%.0f units", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);

	if (ImGui::CollapsingHeader(T("feature.wind.wind_effect.dragon.wingbeats", "Wingbeat Pulses"),
			ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T("feature.wind.wind_effect.dragon.wingbeats_enabled", "Enable Wingbeat Pulses"),
			&settings.wingbeatsEnabled);
		ImGui::BeginDisabled(!settings.wingbeatsEnabled);
		ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.wingbeat_strength", "Strength##DragonWingbeat"), &settings.wingbeatStrength,
			DragonWindRuntime::kStrengthMin, DragonWindRuntime::kStrengthMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.wingbeat_distance", "Travel Distance##DragonWingbeat"), &settings.wingbeatDistance,
			DragonWindRuntime::kDistanceMin, DragonWindRuntime::kDistanceMax, "%.0f units",
			ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.wingbeat_width", "Wave Half-Width##DragonWingbeat"), &settings.wingbeatWaveHalfWidth,
			DragonWindRuntime::kDistanceMin, DragonWindRuntime::kDistanceMax, "%.0f units",
			ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.wingbeat_speed", "Propagation Speed##DragonWingbeat"), &settings.wingbeatPropagationSpeed,
			DragonWindRuntime::kSpeedMin, DragonWindRuntime::kSpeedMax, "%.0f units/s", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.wingbeat_decay", "Trailing Falloff##DragonWingbeat"), &settings.wingbeatDecayTime,
			DragonWindRuntime::kTimeMin, DragonWindRuntime::kTimeMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.wingbeat_fallback_cooldown", "Sound Fallback Cooldown##DragonWingbeat"), &settings.wingbeatFallbackCooldown,
			DragonWindRuntime::kTimeMin, DragonWindRuntime::kTimeMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader(T("feature.wind.wind_effect.dragon.impacts", "Landing and Crash Shockwaves"),
			ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T("feature.wind.wind_effect.dragon.landings_enabled", "Enable Landing Shockwaves"),
			&settings.landingsEnabled);
		ImGui::Checkbox(T("feature.wind.wind_effect.dragon.crashes_enabled", "Enable Crash Shockwaves"),
			&settings.crashesEnabled);
		const auto drawImpactProfile = [&](const char* a_label, ImpactProfile& a_profile, bool a_enabled) {
			if (!ImGui::TreeNode(a_label))
				return;
			ImGui::BeginDisabled(!a_enabled);
			ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.impact_strength", "Strength"), &a_profile.strength, DragonWindRuntime::kStrengthMin,
				DragonWindRuntime::kStrengthMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.impact_distance", "Travel Distance"), &a_profile.distance, DragonWindRuntime::kDistanceMin,
				DragonWindRuntime::kDistanceMax, "%.0f units",
				ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
			ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.impact_width", "Wave Half-Width"), &a_profile.waveHalfWidth, DragonWindRuntime::kDistanceMin,
				DragonWindRuntime::kDistanceMax, "%.0f units",
				ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
			ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.impact_speed", "Propagation Speed"), &a_profile.propagationSpeed, DragonWindRuntime::kSpeedMin,
				DragonWindRuntime::kSpeedMax, "%.0f units/s", ImGuiSliderFlags_AlwaysClamp);
			ImGui::EndDisabled();
			ImGui::TreePop();
		};
		drawImpactProfile(T("feature.wind.wind_effect.dragon.normal_landing", "Normal Landing"), settings.normalImpact, settings.landingsEnabled);
		drawImpactProfile(T("feature.wind.wind_effect.dragon.forceful_landing", "Forceful Landing"), settings.forcefulImpact, settings.landingsEnabled);
		drawImpactProfile(T("feature.wind.wind_effect.dragon.crash_landing", "Crash Landing"), settings.crashImpact, settings.crashesEnabled);
		ImGui::BeginDisabled(!settings.landingsEnabled && !settings.crashesEnabled);
		ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.impact_decay", "Trailing Falloff##DragonImpact"), &settings.impactDecayTime,
			DragonWindRuntime::kTimeMin, DragonWindRuntime::kTimeMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T("feature.wind.wind_effect.dragon.impact_deduplication", "Event Deduplication Window##DragonImpact"), &settings.impactDeduplicationTime,
			DragonWindRuntime::kTimeMin, DragonWindRuntime::kTimeMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndDisabled();
	}

	ImGui::Checkbox(T("feature.wind.wind_effect.dragon.shouts_enabled", "Enable Shout and Breath Forces"),
		&settings.shoutsEnabled);

	ImGui::EndDisabled();
}

void DragonWind::LoadSettings(const nlohmann::json& a_json)
{
	if (const auto legacy = a_json.find("dragonWind"); legacy != a_json.end() && legacy->is_object())
		settings = *legacy;
	else
		settings = a_json;
	SanitizeSettings();
}

void DragonWind::SaveSettings(nlohmann::json& a_json) const
{
	a_json = settings;
}

void DragonWind::RestoreDefaultSettings()
{
	settings = {};
	Reset();
}

void DragonWind::DataLoaded()
{
	DragonWindRuntime::Register();
}

void DragonWind::Update(float a_frameTime)
{
	DragonWindRuntime::Update(a_frameTime, settings);
}

void DragonWind::Reset()
{
	DragonWindRuntime::Reset();
}

void DragonWind::SanitizeSettings()
{
	const Settings defaults{};
	auto sanitizeImpact = [&](ImpactProfile& a_profile, const ImpactProfile& a_default) {
		a_profile.strength = WindMath::ClampFiniteOrDefault(a_profile.strength,
			DragonWindRuntime::kStrengthMin, DragonWindRuntime::kStrengthMax, a_default.strength);
		a_profile.distance = WindMath::ClampFiniteOrDefault(a_profile.distance,
			DragonWindRuntime::kDistanceMin, DragonWindRuntime::kDistanceMax, a_default.distance);
		a_profile.waveHalfWidth = WindMath::ClampFiniteOrDefault(a_profile.waveHalfWidth,
			DragonWindRuntime::kDistanceMin, DragonWindRuntime::kDistanceMax, a_default.waveHalfWidth);
		a_profile.propagationSpeed = WindMath::ClampFiniteOrDefault(a_profile.propagationSpeed,
			DragonWindRuntime::kSpeedMin, DragonWindRuntime::kSpeedMax, a_default.propagationSpeed);
	};

	settings.strength = WindMath::ClampFiniteOrDefault(settings.strength,
		DragonWindRuntime::kStrengthMin, DragonWindRuntime::kStrengthMax, defaults.strength);
	settings.trackingDistance = WindMath::ClampFiniteOrDefault(settings.trackingDistance,
		DragonWindRuntime::kDistanceMin, DragonWindRuntime::kDistanceMax, defaults.trackingDistance);
	settings.wingbeatStrength = WindMath::ClampFiniteOrDefault(settings.wingbeatStrength,
		DragonWindRuntime::kStrengthMin, DragonWindRuntime::kStrengthMax, defaults.wingbeatStrength);
	settings.wingbeatDistance = WindMath::ClampFiniteOrDefault(settings.wingbeatDistance,
		DragonWindRuntime::kDistanceMin, DragonWindRuntime::kDistanceMax, defaults.wingbeatDistance);
	settings.wingbeatWaveHalfWidth = WindMath::ClampFiniteOrDefault(settings.wingbeatWaveHalfWidth,
		DragonWindRuntime::kDistanceMin, DragonWindRuntime::kDistanceMax, defaults.wingbeatWaveHalfWidth);
	settings.wingbeatPropagationSpeed = WindMath::ClampFiniteOrDefault(settings.wingbeatPropagationSpeed,
		DragonWindRuntime::kSpeedMin, DragonWindRuntime::kSpeedMax, defaults.wingbeatPropagationSpeed);
	settings.wingbeatDecayTime = WindMath::ClampFiniteOrDefault(settings.wingbeatDecayTime,
		DragonWindRuntime::kTimeMin, DragonWindRuntime::kTimeMax, defaults.wingbeatDecayTime);
	settings.wingbeatFallbackCooldown = WindMath::ClampFiniteOrDefault(settings.wingbeatFallbackCooldown,
		DragonWindRuntime::kTimeMin, DragonWindRuntime::kTimeMax, defaults.wingbeatFallbackCooldown);
	sanitizeImpact(settings.normalImpact, defaults.normalImpact);
	sanitizeImpact(settings.forcefulImpact, defaults.forcefulImpact);
	sanitizeImpact(settings.crashImpact, defaults.crashImpact);
	settings.impactDecayTime = WindMath::ClampFiniteOrDefault(settings.impactDecayTime,
		DragonWindRuntime::kTimeMin, DragonWindRuntime::kTimeMax, defaults.impactDecayTime);
	settings.impactDeduplicationTime = WindMath::ClampFiniteOrDefault(settings.impactDeduplicationTime,
		DragonWindRuntime::kTimeMin, DragonWindRuntime::kTimeMax, defaults.impactDeduplicationTime);
}
