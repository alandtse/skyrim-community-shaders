#include "ExplosionWindRouter.h"

#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindMath.h"
#include "I18n/I18n.h"
#include "ProjectileHookDispatcher.h"
#include "State.h"
#include "StormCallRecords.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <nlohmann/json.hpp>

namespace
{
	constexpr float kIntensityMinimum = 0.0f;
	constexpr float kIntensityMaximum = 5.0f;
	constexpr float kRadiusMultiplierMinimum = 0.25f;
	constexpr float kRadiusMultiplierMaximum = 3.0f;
	constexpr float kMinimumRadius = 96.0f;
	constexpr float kMaximumRadius = 5000.0f;
	constexpr float kMinimumWaveHalfWidth = 64.0f;
	constexpr float kMaximumWaveHalfWidth = 700.0f;
	constexpr float kMinimumPropagationSpeed = 1400.0f;
	constexpr float kMaximumPropagationSpeed = 4500.0f;
	constexpr float kDeduplicationTime = 0.3f;
	constexpr float kCrossRecordDeduplicationTime = 0.05f;
	constexpr float kBurstWindow = 0.1f;
	constexpr std::size_t kMaximumSourcesPerBurst = 4;
	constexpr std::size_t kMaximumRecentExplosions = 24;
	constexpr std::size_t kMaximumPendingObservations = 64;
	constexpr std::string_view kSkyrimMaster = "Skyrim.esm";

	struct SpellShoutOwnedCast
	{
		RE::FormID localFormID;
		std::string_view editorID;
	};

	constexpr std::array<SpellShoutOwnedCast, 3> kSpellShoutOwnedCasts{
		SpellShoutOwnedCast{ 0x7A82B, "FireStorm" },
		SpellShoutOwnedCast{ 0x7E8E4, "Blizzard" },
		SpellShoutOwnedCast{ 0x7E8E5, "LightningStorm" }
	};

	float3 GetFallbackDirection(const RE::TESObjectREFR* a_cause, const RE::NiPoint3& a_position) noexcept
	{
		if (!a_cause)
			return { 1.0f, 0.0f, 0.0f };

		const auto causePosition = a_cause->GetPosition();
		const float x = a_position.x - causePosition.x;
		const float y = a_position.y - causePosition.y;
		const float length = std::sqrt(x * x + y * y);
		if (!std::isfinite(length) || length <= 1e-4f)
			return { 1.0f, 0.0f, 0.0f };
		return { x / length, y / length, 0.0f };
	}
}

using WindMath::ClampFiniteOrDefault;
using WindMath::SquaredDistance;

ExplosionWindRouter::ExplosionWindRouter()
{
	ProjectileHookDispatcher::GetSingleton().AddImpactObserver(this, ObserveImpactCallback);
}

ExplosionWindRouter::~ExplosionWindRouter()
{
	ProjectileHookDispatcher::GetSingleton().RemoveObservers(this);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ExplosionWindRouter::Settings,
	enabled,
	intensity,
	radiusMultiplier,
	decayTime)

std::string ExplosionWindRouter::GetDisplayName() const
{
	return T("feature.wind.wind_effect.explosions.name", "Explosive Magic");
}

void ExplosionWindRouter::DrawSettings()
{
	if (ImGui::Checkbox(T("feature.wind.wind_effect.explosions.enabled", "Enable Explosion Wind"),
			&settings.enabled) &&
		!settings.enabled) {
		Reset();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.explosions.enabled_tooltip",
			"Adds radial pressure waves for record-backed magic explosions that hit a reference."));

	ImGui::BeginDisabled(!settings.enabled);
	ImGui::SliderFloat(T("feature.wind.wind_effect.explosions.intensity", "Intensity"), &settings.intensity,
		kIntensityMinimum, kIntensityMaximum, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.explosions.radius", "Radius"), &settings.radiusMultiplier,
		kRadiusMultiplierMinimum, kRadiusMultiplierMaximum, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.explosions.decay", "Trailing Falloff"), &settings.decayTime,
		0.0f, WindField::kTransientImpulseMaximumDecayTime, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
}

void ExplosionWindRouter::LoadSettings(const nlohmann::json& a_json)
{
	settings = a_json;
	SanitizeSettings();
}

void ExplosionWindRouter::SaveSettings(nlohmann::json& a_json) const
{
	a_json = settings;
}

void ExplosionWindRouter::RestoreDefaultSettings()
{
	settings = {};
}

void ExplosionWindRouter::DataLoaded()
{
	if (registered)
		return;

	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
	if (!dataHandler || !eventSourceHolder) {
		logger::warn("Unable to register explosive magic wind: game data is unavailable");
		return;
	}

	projectileProfiles.clear();
	for (const auto* projectile : dataHandler->GetFormArray<RE::BGSProjectile>()) {
		if (const auto profile = ClassifyProjectile(projectile)) {
			projectileProfiles.emplace(projectile->GetFormID(), *profile);
			ProjectileHookDispatcher::GetSingleton().ObserveProjectileType(
				*projectile, ProjectileHookDispatcher::Event::Impact);
		}
	}

	magicEffectProfiles.clear();
	for (const auto* effect : dataHandler->GetFormArray<RE::EffectSetting>()) {
		if (const auto profile = ClassifyMagicEffect(effect))
			magicEffectProfiles.emplace(effect->GetFormID(), *profile);
	}

	sourceProfiles.clear();
	const auto indexMagicItems = [&](const auto& a_items) {
		for (const auto* item : a_items) {
			if (const auto profile = ClassifyMagicItem(item))
				sourceProfiles.emplace(item->GetFormID(), *profile);
		}
	};
	indexMagicItems(dataHandler->GetFormArray<RE::SpellItem>());
	indexMagicItems(dataHandler->GetFormArray<RE::EnchantmentItem>());
	indexMagicItems(dataHandler->GetFormArray<RE::ScrollItem>());
	for (const auto& [formID, profile] : magicEffectProfiles) {
		sourceProfiles.try_emplace(formID, profile);
		sourceProfiles.try_emplace(profile.identity, profile);
	}
	for (const auto* weapon : dataHandler->GetFormArray<RE::TESObjectWEAP>()) {
		if (weapon && weapon->formEnchanting) {
			if (const auto profile = ClassifyMagicItem(weapon->formEnchanting))
				sourceProfiles.emplace(weapon->GetFormID(), *profile);
		}
	}
	for (const auto& [formID, profile] : projectileProfiles) {
		sourceProfiles.try_emplace(formID, profile);
		sourceProfiles.try_emplace(profile.identity, profile);
	}

	reservedCastForms.clear();
	reservedMagicEffects.clear();
	const auto reserveMagicItem = [&](const RE::MagicItem* a_magicItem) {
		if (!a_magicItem)
			return;
		reservedCastForms.emplace(a_magicItem->GetFormID());
		for (const auto* effect : a_magicItem->effects) {
			if (effect && effect->baseEffect)
				reservedMagicEffects.emplace(effect->baseEffect->GetFormID());
		}
	};
	for (const auto& reservedCast : kSpellShoutOwnedCasts) {
		const auto* spell = dataHandler->LookupForm<RE::SpellItem>(reservedCast.localFormID, kSkyrimMaster);
		if (!spell)
			spell = RE::TESForm::LookupByEditorID<RE::SpellItem>(reservedCast.editorID);
		reserveMagicItem(spell);
	}
	for (const auto& bolt : StormCallRecords::kBolts)
		reserveMagicItem(StormCallRecords::ResolveBolt(*dataHandler, bolt));

	eventSourceHolder->AddEventSink<RE::TESHitEvent>(this);
	eventSourceHolder->AddEventSink<RE::TESMagicEffectApplyEvent>(this);
	eventSourceHolder->AddEventSink<RE::TESSpellCastEvent>(this);
	registered = true;
	logger::info("Registered explosive magic wind: {} projectile, {} magic-effect, and {} hit-source profiles",
		projectileProfiles.size(), magicEffectProfiles.size(), sourceProfiles.size());
}

void ExplosionWindRouter::Reset()
{
	{
		std::lock_guard lock(pendingObservationsMutex);
		pendingObservations.clear();
	}
	recentExplosions.clear();
	State::GetSingleton()->ClearTransientWindSources(State::TransientWindSourceOwner::Explosion);
}

void ExplosionWindRouter::Update(float)
{
	std::deque<ExplosionObservation> observations;
	{
		std::lock_guard lock(pendingObservationsMutex);
		observations.swap(pendingObservations);
	}

	if (!settings.enabled || settings.intensity <= 0.0f)
		return;

	for (const auto& observation : observations)
		EmitExplosion(observation);
}

void ExplosionWindRouter::RouteExplosionAt(const RE::BGSExplosion* a_explosion,
	const RE::NiPoint3& a_position, const RE::TESObjectREFR* a_cause)
{
	if (const auto profile = ClassifyExplosion(a_explosion))
		QueueObservation(*profile, a_position, a_cause);
}

void ExplosionWindRouter::ObserveImpactCallback(void* a_owner, RE::Projectile& a_projectile,
	const RE::NiPoint3& a_position, const RE::NiPoint3&)
{
	static_cast<ExplosionWindRouter*>(a_owner)->ObserveImpact(a_projectile, a_position);
}

void ExplosionWindRouter::ObserveImpact(RE::Projectile& a_projectile, const RE::NiPoint3& a_position)
{
	const auto& runtimeData = a_projectile.GetProjectileRuntimeData();
	if ((runtimeData.spell && reservedCastForms.contains(runtimeData.spell->GetFormID())) ||
		(runtimeData.avEffect && reservedMagicEffects.contains(runtimeData.avEffect->GetFormID()))) {
		return;
	}

	const auto* projectile = a_projectile.GetProjectileBase();
	const auto* explosion = runtimeData.explosion;
	if (!explosion && runtimeData.avEffect)
		explosion = runtimeData.avEffect->data.explosion;
	if (!explosion && projectile)
		explosion = projectile->data.explosionType;

	auto cause = runtimeData.shooter.get();
	if (explosion) {
		RouteExplosionAt(explosion, a_position, cause.get());
	} else if (const auto projectileProfile = ClassifyProjectile(projectile)) {
		QueueObservation(*projectileProfile, a_position, cause.get());
	}
}

RE::BSEventNotifyControl ExplosionWindRouter::ProcessEvent(const RE::TESHitEvent* a_event,
	RE::BSTEventSource<RE::TESHitEvent>*)
{
	if (!a_event || !a_event->target)
		return RE::BSEventNotifyControl::kContinue;

	if (const auto profile = FindProjectileProfile(a_event->projectile)) {
		QueueObservation(*profile, a_event->target->GetPosition(), a_event->cause.get());
	} else if (!reservedCastForms.contains(a_event->source) &&
			   !reservedMagicEffects.contains(a_event->source)) {
		if (const auto sourceProfile = FindSourceProfile(a_event->source))
			QueueObservation(*sourceProfile, a_event->target->GetPosition(), a_event->cause.get());
	}

	return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl ExplosionWindRouter::ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event,
	RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*)
{
	if (!a_event || !a_event->target)
		return RE::BSEventNotifyControl::kContinue;
	if (reservedMagicEffects.contains(a_event->magicEffect))
		return RE::BSEventNotifyControl::kContinue;

	if (const auto profile = FindMagicEffectProfile(a_event->magicEffect))
		QueueObservation(*profile, a_event->target->GetPosition(), a_event->caster.get());

	return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl ExplosionWindRouter::ProcessEvent(const RE::TESSpellCastEvent* a_event,
	RE::BSTEventSource<RE::TESSpellCastEvent>*)
{
	if (!a_event || !a_event->object)
		return RE::BSEventNotifyControl::kContinue;
	if (reservedCastForms.contains(a_event->spell))
		return RE::BSEventNotifyControl::kContinue;

	const auto* form = RE::TESForm::LookupByID(a_event->spell);
	const auto* magicItem = form ? form->As<RE::MagicItem>() : nullptr;
	if (!magicItem || magicItem->GetDelivery() != RE::MagicSystem::Delivery::kSelf)
		return RE::BSEventNotifyControl::kContinue;

	if (const auto profile = ClassifyMagicItem(magicItem))
		QueueObservation(*profile, a_event->object->GetPosition(), a_event->object.get());

	return RE::BSEventNotifyControl::kContinue;
}

std::optional<ExplosionWindRouter::ExplosionProfile> ExplosionWindRouter::ClassifyExplosion(
	const RE::BGSExplosion* a_explosion)
{
	if (!a_explosion)
		return std::nullopt;
	return CreateProfile(a_explosion->GetFormID(), a_explosion->data.radius, a_explosion->data.force);
}

std::optional<ExplosionWindRouter::ExplosionProfile> ExplosionWindRouter::ClassifyProjectile(
	const RE::BGSProjectile* a_projectile)
{
	if (!a_projectile)
		return std::nullopt;

	const bool markedExplosive =
		a_projectile->data.flags.all(RE::BGSProjectileData::BGSProjectileFlags::kExplosion);
	if (!markedExplosive && !a_projectile->data.explosionType)
		return std::nullopt;
	if (const auto profile = ClassifyExplosion(a_projectile->data.explosionType))
		return profile;
	return CreateProfile(a_projectile->GetFormID(), a_projectile->data.collisionRadius * 4.0f,
		a_projectile->data.force);
}

std::optional<ExplosionWindRouter::ExplosionProfile> ExplosionWindRouter::ClassifyMagicEffect(
	const RE::EffectSetting* a_effect)
{
	if (!a_effect)
		return std::nullopt;
	if (const auto profile = ClassifyExplosion(a_effect->data.explosion))
		return profile;
	return ClassifyProjectile(a_effect->data.projectileBase);
}

std::optional<ExplosionWindRouter::ExplosionProfile> ExplosionWindRouter::ClassifyMagicItem(
	const RE::MagicItem* a_magicItem)
{
	std::optional<ExplosionProfile> result;
	if (!a_magicItem)
		return result;

	for (const auto* effect : a_magicItem->effects) {
		if (!effect)
			continue;
		auto profile = ClassifyMagicEffect(effect->baseEffect);
		if (!profile)
			continue;
		profile->radius = std::max(profile->radius,
			ClampFiniteOrDefault(static_cast<float>(effect->effectItem.area), 0.0f, kMaximumRadius, 0.0f));
		profile->strength = std::max(profile->strength,
			std::clamp(0.65f + profile->radius / 900.0f, 0.65f, 3.0f));
		if (!result || profile->radius > result->radius)
			result = profile;
	}
	return result;
}

ExplosionWindRouter::ExplosionProfile ExplosionWindRouter::CreateProfile(
	RE::FormID a_identity, float a_radius, float a_force)
{
	const float radius = ClampFiniteOrDefault(a_radius,
		kMinimumRadius, kMaximumRadius, kMinimumRadius);
	const float force = std::isfinite(a_force) ? std::max(a_force, 0.0f) : 0.0f;
	const float radiusScale = std::clamp(radius / 900.0f, 0.0f, 2.0f);
	const float forceScale = std::sqrt(std::clamp(force, 0.0f, 5000.0f) / 500.0f);
	return {
		a_identity,
		radius,
		std::clamp(0.65f + radiusScale + 0.35f * forceScale, 0.65f, 3.0f)
	};
}

std::optional<ExplosionWindRouter::ExplosionProfile> ExplosionWindRouter::FindProjectileProfile(
	RE::FormID a_formID) const
{
	const auto found = projectileProfiles.find(a_formID);
	return found != projectileProfiles.end() ? std::optional{ found->second } : std::nullopt;
}

std::optional<ExplosionWindRouter::ExplosionProfile> ExplosionWindRouter::FindMagicEffectProfile(
	RE::FormID a_formID) const
{
	const auto found = magicEffectProfiles.find(a_formID);
	return found != magicEffectProfiles.end() ? std::optional{ found->second } : std::nullopt;
}

std::optional<ExplosionWindRouter::ExplosionProfile> ExplosionWindRouter::FindSourceProfile(
	RE::FormID a_formID) const
{
	const auto found = sourceProfiles.find(a_formID);
	return found != sourceProfiles.end() ? std::optional{ found->second } : std::nullopt;
}

bool ExplosionWindRouter::AcceptExplosion(const ExplosionProfile& a_profile,
	const RE::NiPoint3& a_position, RE::FormID a_cause)
{
	const auto now = std::chrono::steady_clock::now();
	const float effectiveRadius = std::clamp(a_profile.radius * settings.radiusMultiplier,
		kMinimumRadius, kMaximumRadius);
	const auto secondsSince = [&](const RecentExplosion& a_recent) {
		return std::chrono::duration<float>(now - a_recent.time).count();
	};

	std::erase_if(recentExplosions, [&](const RecentExplosion& a_recent) {
		return secondsSince(a_recent) > kDeduplicationTime;
	});

	const auto burstCount = std::ranges::count_if(recentExplosions, [&](const RecentExplosion& a_recent) {
		return secondsSince(a_recent) <= kBurstWindow;
	});
	if (burstCount >= kMaximumSourcesPerBurst)
		return false;

	for (const auto& recent : recentExplosions) {
		const float elapsed = secondsSince(recent);
		const bool sameDetonation = recent.identity == a_profile.identity && recent.cause == a_cause;
		if (!sameDetonation && elapsed > kCrossRecordDeduplicationTime)
			continue;
		const float deduplicationRadius = sameDetonation ?
		                                      std::clamp(std::max(recent.radius, effectiveRadius) * 2.0f,
												  256.0f, kMaximumRadius) :
		                                      std::clamp(std::max(recent.radius, effectiveRadius) * 0.2f,
												  96.0f, 256.0f);
		if (SquaredDistance(recent.position, a_position) <= deduplicationRadius * deduplicationRadius)
			return false;
	}

	recentExplosions.push_back({ a_profile.identity, a_cause, a_position, effectiveRadius, now });
	if (recentExplosions.size() > kMaximumRecentExplosions)
		recentExplosions.erase(recentExplosions.begin());
	return true;
}

void ExplosionWindRouter::QueueObservation(const ExplosionProfile& a_profile,
	const RE::NiPoint3& a_position, const RE::TESObjectREFR* a_cause)
{
	if (!std::isfinite(a_position.x) || !std::isfinite(a_position.y) || !std::isfinite(a_position.z))
		return;

	const ExplosionObservation observation{
		a_profile,
		a_position,
		GetFallbackDirection(a_cause, a_position),
		a_cause ? a_cause->GetFormID() : 0
	};
	std::lock_guard lock(pendingObservationsMutex);
	if (pendingObservations.size() >= kMaximumPendingObservations)
		pendingObservations.pop_front();
	pendingObservations.push_back(observation);
}

void ExplosionWindRouter::EmitExplosion(const ExplosionObservation& a_observation)
{
	if (!AcceptExplosion(a_observation.profile, a_observation.position, a_observation.cause))
		return;

	const float radius = std::clamp(a_observation.profile.radius * settings.radiusMultiplier,
		kMinimumRadius, kMaximumRadius);
	const float waveHalfWidth = std::clamp(radius * 0.35f,
		kMinimumWaveHalfWidth, kMaximumWaveHalfWidth);
	const float propagationSpeed = std::clamp(kMinimumPropagationSpeed + radius * 1.2f,
		kMinimumPropagationSpeed, kMaximumPropagationSpeed);
	const auto source = WindField::MakeRadialWave(
		{ a_observation.position.x, a_observation.position.y, a_observation.position.z },
		a_observation.direction, a_observation.profile.strength * settings.intensity, radius, waveHalfWidth, propagationSpeed,
		settings.decayTime);
	State::GetSingleton()->QueueTransientWindSource(source,
		State::TransientWindSourceOwner::Explosion,
		State::TransientWindSourcePriority::Impact);
}

void ExplosionWindRouter::SanitizeSettings()
{
	const Settings defaults;
	settings.intensity = ClampFiniteOrDefault(settings.intensity,
		kIntensityMinimum, kIntensityMaximum, defaults.intensity);
	settings.radiusMultiplier = ClampFiniteOrDefault(settings.radiusMultiplier,
		kRadiusMultiplierMinimum, kRadiusMultiplierMaximum, defaults.radiusMultiplier);
	settings.decayTime = ClampFiniteOrDefault(settings.decayTime,
		0.0f, WindField::kTransientImpulseMaximumDecayTime, defaults.decayTime);
}
