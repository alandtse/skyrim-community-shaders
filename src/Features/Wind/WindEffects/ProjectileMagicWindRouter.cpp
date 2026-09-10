#include "ProjectileMagicWindRouter.h"

#include "ActorWind.h"
#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindMath.h"
#include "FusRoDahWind.h"
#include "I18n/I18n.h"
#include "ProjectileHookDispatcher.h"
#include "SpellShoutWindRouter.h"
#include "State.h"
#include "StormCallRecords.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <numbers>

namespace
{
	constexpr float kMinimumStrength = 0.0f;
	constexpr float kMaximumStrength = 5.0f;
	constexpr float kMinimumRadiusMultiplier = 0.25f;
	constexpr float kMaximumRadiusMultiplier = 3.0f;
	constexpr float kMinimumRadius = 96.0f;
	constexpr float kMaximumRadius = 2400.0f;
	constexpr float kMinimumRange = 400.0f;
	constexpr float kMaximumRange = 6000.0f;
	constexpr float kDuplicateWindow = 0.25f;
	constexpr float kCrossImpactWindow = 0.04f;
	constexpr float kBurstWindow = 0.1f;
	constexpr std::size_t kMaximumSourcesPerBurst = 16;
	constexpr std::size_t kMaximumRecentImpacts = 96;
	constexpr std::size_t kMaximumPendingCasts = 96;

}

using WindMath::ClampFiniteOrDefault;
using WindMath::GetHorizontalVelocityDirection;
using WindMath::SquaredDistance;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ProjectileMagicWindRouter::Settings,
	enabled,
	launchStrength,
	impactStrength,
	dragonStrength,
	radiusMultiplier,
	decayTime)

ProjectileMagicWindRouter::ProjectileMagicWindRouter()
{
	ProjectileHookDispatcher::GetSingleton().AddImpactObserver(this, ObserveImpactCallback);
}

ProjectileMagicWindRouter::~ProjectileMagicWindRouter()
{
	ProjectileHookDispatcher::GetSingleton().RemoveObservers(this);
}

std::string ProjectileMagicWindRouter::GetDisplayName() const
{
	return T("feature.wind.wind_effect.projectile_magic.name", "Projectile Magic");
}

void ProjectileMagicWindRouter::DrawSettings()
{
	if (ImGui::Checkbox(T("feature.wind.wind_effect.projectile_magic.enabled", "Enable Projectile Wind"),
			&settings.enabled) &&
		!settings.enabled) {
		Reset();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.projectile_magic.enabled_tooltip",
			"Automatically adds launch and impact wind to loaded projectile spells, including mod-added magic."));

	ImGui::BeginDisabled(!settings.enabled);
	ImGui::SliderFloat(T("feature.wind.wind_effect.projectile_magic.launch", "Launch Strength"),
		&settings.launchStrength, kMinimumStrength, kMaximumStrength, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.projectile_magic.impact", "Impact Strength"),
		&settings.impactStrength, kMinimumStrength, kMaximumStrength, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.projectile_magic.dragon", "Dragon Strength"),
		&settings.dragonStrength, kMinimumStrength, kMaximumStrength, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.projectile_magic.radius", "Impact Radius"),
		&settings.radiusMultiplier, kMinimumRadiusMultiplier, kMaximumRadiusMultiplier, "%.2fx",
		ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.projectile_magic.decay", "Trailing Falloff"),
		&settings.decayTime, 0.0f, WindField::kTransientImpulseMaximumDecayTime, "%.2f s",
		ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
}

void ProjectileMagicWindRouter::LoadSettings(const nlohmann::json& a_json)
{
	settings = a_json;
	SanitizeSettings();
}

void ProjectileMagicWindRouter::SaveSettings(nlohmann::json& a_json) const
{
	a_json = settings;
}

void ProjectileMagicWindRouter::RestoreDefaultSettings()
{
	settings = {};
}

void ProjectileMagicWindRouter::DataLoaded()
{
	if (recordsLoaded)
		return;
	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
	if (!dataHandler || !eventSourceHolder) {
		logger::warn("Unable to register projectile magic wind: game data is unavailable");
		return;
	}

	projectileProfiles.clear();
	magicEffectProfiles.clear();
	magicItemProfiles.clear();
	excludedMagicItems.clear();
	excludedMagicEffects.clear();
	shoutMagicItems.clear();
	for (const auto* shout : dataHandler->GetFormArray<RE::TESShout>()) {
		if (!shout)
			continue;
		for (const auto& variation : shout->variations) {
			if (variation.spell)
				shoutMagicItems.emplace(variation.spell->GetFormID());
		}
	}
	for (const auto& bolt : StormCallRecords::kBolts) {
		if (const auto* spell = StormCallRecords::ResolveBolt(*dataHandler, bolt))
			excludedMagicItems.emplace(spell->GetFormID());
	}
	SpellShoutWindRouter::CollectOwnedMagicItems(*dataHandler, excludedMagicItems);
	FusRoDahWind::CollectOwnedMagicItems(*dataHandler, excludedMagicItems);
	for (const auto formID : excludedMagicItems) {
		const auto* form = RE::TESForm::LookupByID(formID);
		const auto* magicItem = form ? form->As<RE::MagicItem>() : nullptr;
		if (!magicItem)
			continue;
		for (const auto* effect : magicItem->effects) {
			if (effect && effect->baseEffect)
				excludedMagicEffects.emplace(effect->baseEffect->GetFormID());
		}
	}
	for (const auto* effect : dataHandler->GetFormArray<RE::EffectSetting>()) {
		if (!effect)
			continue;
		if (const auto profile = ClassifyMagicEffect(effect)) {
			magicEffectProfiles.emplace(effect->GetFormID(), *profile);
			if (effect->data.projectileBase) {
				projectileProfiles.insert_or_assign(effect->data.projectileBase->GetFormID(), *profile);
				ProjectileHookDispatcher::GetSingleton().ObserveProjectileType(
					*effect->data.projectileBase, ProjectileHookDispatcher::Event::Impact);
			}
		}
	}

	const auto indexMagicItems = [&](const auto& a_items) {
		for (const auto* item : a_items) {
			if (const auto profile = ClassifyMagicItem(item))
				magicItemProfiles.emplace(item->GetFormID(), *profile);
		}
	};
	indexMagicItems(dataHandler->GetFormArray<RE::SpellItem>());
	indexMagicItems(dataHandler->GetFormArray<RE::ScrollItem>());
	indexMagicItems(dataHandler->GetFormArray<RE::EnchantmentItem>());

	eventSourceHolder->AddEventSink<RE::TESSpellCastEvent>(this);
	recordsLoaded = true;
	logger::info("Registered projectile magic wind: {} magic items, {} effects, and {} projectiles",
		magicItemProfiles.size(), magicEffectProfiles.size(), projectileProfiles.size());
}

RE::BSEventNotifyControl ProjectileMagicWindRouter::ProcessEvent(const RE::TESSpellCastEvent* a_event,
	RE::BSTEventSource<RE::TESSpellCastEvent>*)
{
	if (!a_event || !a_event->object || excludedMagicItems.contains(a_event->spell) ||
		!magicItemProfiles.contains(a_event->spell))
		return RE::BSEventNotifyControl::kContinue;
	auto* actor = a_event->object->As<RE::Actor>();
	if (!actor)
		return RE::BSEventNotifyControl::kContinue;

	const PendingCast cast{ actor->GetHandle(), actor->GetFormID(), a_event->spell };
	std::lock_guard lock(pendingMutex);
	const auto duplicate = std::ranges::find_if(pendingCasts, [&](const PendingCast& a_pending) {
		return a_pending.actorFormID == cast.actorFormID && a_pending.spellFormID == cast.spellFormID;
	});
	if (duplicate == pendingCasts.end()) {
		if (pendingCasts.size() >= kMaximumPendingCasts)
			pendingCasts.erase(pendingCasts.begin());
		pendingCasts.push_back(cast);
	}
	return RE::BSEventNotifyControl::kContinue;
}

void ProjectileMagicWindRouter::Update(float)
{
	std::vector<PendingImpact> impacts;
	std::vector<PendingCast> casts;
	{
		std::lock_guard lock(pendingMutex);
		impacts.swap(pendingImpacts);
		casts.swap(pendingCasts);
	}
	if (!settings.enabled)
		return;

	for (const auto& cast : casts)
		EmitCast(cast);
	for (const auto& impact : impacts)
		EmitImpact(impact);
}

void ProjectileMagicWindRouter::Reset()
{
	{
		std::lock_guard lock(pendingMutex);
		recentImpacts.clear();
		pendingImpacts.clear();
		pendingCasts.clear();
	}
	State::GetSingleton()->ClearTransientWindSources(State::TransientWindSourceOwner::ProjectileMagic);
}

void ProjectileMagicWindRouter::ObserveImpactCallback(void* a_owner, RE::Projectile& a_projectile,
	const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity)
{
	static_cast<ProjectileMagicWindRouter*>(a_owner)->ObserveImpact(
		a_projectile, a_position, a_velocity);
}

void ProjectileMagicWindRouter::ObserveImpact(RE::Projectile& a_projectile,
	const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity)
{
	if (!std::isfinite(a_position.x) || !std::isfinite(a_position.y) || !std::isfinite(a_position.z))
		return;
	const auto profile = FindRuntimeProfile(a_projectile);
	if (!profile || profile->explosive)
		return;

	std::lock_guard lock(pendingMutex);
	if (!AcceptImpactLocked(a_projectile, a_position))
		return;
	if (pendingImpacts.size() >= kMaximumRecentImpacts)
		pendingImpacts.erase(pendingImpacts.begin());
	pendingImpacts.push_back({ a_position, a_velocity, *profile });
}

std::optional<ProjectileMagicWindRouter::ProjectileProfile> ProjectileMagicWindRouter::ClassifyProjectile(
	const RE::BGSProjectile* a_projectile, float a_area)
{
	if (!a_projectile)
		return std::nullopt;
	const float range = ClampFiniteOrDefault(a_projectile->data.range, kMinimumRange, kMaximumRange, 2000.0f);
	const float collisionRadius = ClampFiniteOrDefault(
		a_projectile->data.collisionRadius * 4.0f, kMinimumRadius, kMaximumRadius, kMinimumRadius);
	const float radius = std::clamp(std::max(collisionRadius, a_area), kMinimumRadius, kMaximumRadius);
	const float force = std::isfinite(a_projectile->data.force) ? std::max(a_projectile->data.force, 0.0f) : 0.0f;
	const float strength = std::clamp(0.45f + radius / 1200.0f + std::sqrt(std::min(force, 5000.0f) / 5000.0f),
		0.5f, 2.5f);
	const float speed = ClampFiniteOrDefault(a_projectile->data.speed, 400.0f, 5000.0f, 1800.0f);
	const float coneHalfAngle = std::clamp(10.0f + radius / std::max(range, 1.0f) * 180.0f, 10.0f, 40.0f);
	const bool explosive = a_projectile->data.explosionType ||
	                       a_projectile->data.flags.all(RE::BGSProjectileData::BGSProjectileFlags::kExplosion);
	return ProjectileProfile{
		range, radius, strength, speed,
		std::cos(coneHalfAngle * std::numbers::pi_v<float> / 180.0f), explosive, false, false
	};
}

std::optional<ProjectileMagicWindRouter::ProjectileProfile> ProjectileMagicWindRouter::ClassifyMagicEffect(
	const RE::EffectSetting* a_effect, float a_area)
{
	if (!a_effect || !a_effect->data.projectileBase)
		return std::nullopt;
	auto profile = ClassifyProjectile(a_effect->data.projectileBase, a_area);
	if (profile && a_effect->data.explosion)
		profile->explosive = true;
	return profile;
}

std::optional<ProjectileMagicWindRouter::ProjectileProfile> ProjectileMagicWindRouter::ClassifyMagicItem(
	const RE::MagicItem* a_magicItem)
{
	if (!a_magicItem)
		return std::nullopt;
	std::optional<ProjectileProfile> result;
	for (const auto* effect : a_magicItem->effects) {
		if (!effect)
			continue;
		auto profile = ClassifyMagicEffect(effect->baseEffect, static_cast<float>(effect->effectItem.area));
		if (!profile)
			continue;
		if (!result || profile->radius > result->radius)
			result = profile;
	}
	if (result) {
		result->range = std::clamp(std::max(result->range, a_magicItem->GetRange()), kMinimumRange, kMaximumRange);
		result->concentration = a_magicItem->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
		result->selfDelivery = a_magicItem->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
	}
	return result;
}

std::optional<ProjectileMagicWindRouter::ProjectileProfile> ProjectileMagicWindRouter::FindRuntimeProfile(
	const RE::Projectile& a_projectile) const
{
	const auto& runtimeData = a_projectile.GetProjectileRuntimeData();
	if (!runtimeData.spell && !runtimeData.avEffect)
		return std::nullopt;
	if (runtimeData.spell && excludedMagicItems.contains(runtimeData.spell->GetFormID()))
		return std::nullopt;
	if (runtimeData.avEffect && excludedMagicEffects.contains(runtimeData.avEffect->GetFormID()))
		return std::nullopt;
	std::optional<ProjectileProfile> result;
	if (runtimeData.avEffect) {
		const auto found = magicEffectProfiles.find(runtimeData.avEffect->GetFormID());
		if (found != magicEffectProfiles.end())
			result = found->second;
	}
	const auto* base = a_projectile.GetProjectileBase();
	if (!result && runtimeData.spell) {
		const auto found = magicItemProfiles.find(runtimeData.spell->GetFormID());
		if (found != magicItemProfiles.end())
			result = found->second;
	}
	if (!result && base) {
		const auto found = projectileProfiles.find(base->GetFormID());
		if (found != projectileProfiles.end())
			result = found->second;
	}
	if (result && (runtimeData.explosion || (base && (base->data.explosionType ||
														 base->data.flags.all(RE::BGSProjectileData::BGSProjectileFlags::kExplosion)))))
		result->explosive = true;
	return result;
}

void ProjectileMagicWindRouter::EmitCast(const PendingCast& a_cast) const
{
	const auto route = magicItemProfiles.find(a_cast.spellFormID);
	if (route == magicItemProfiles.end() || route->second.concentration || route->second.selfDelivery ||
		settings.launchStrength <= 0.0f)
		return;
	auto actor = a_cast.actor.get();
	if (!actor)
		return;
	if (shoutMagicItems.contains(a_cast.spellFormID) && ActorWind::IsDragon(*actor))
		return;
	const float dragonScale = ActorWind::IsDragon(*actor) ? settings.dragonStrength : 1.0f;
	const auto& profile = route->second;
	const auto source = WindField::MakeDirectionalWave(
		ActorWind::GetMagicOrigin(*actor), ActorWind::GetAimDirection(*actor),
		settings.launchStrength * dragonScale * profile.strength, profile.range,
		std::clamp(profile.radius * 1.5f, 120.0f, 600.0f), profile.propagationSpeed,
		profile.coneCosine, settings.decayTime);
	State::GetSingleton()->QueueTransientWindSource(source,
		State::TransientWindSourceOwner::ProjectileMagic,
		State::TransientWindSourcePriority::Flight);
}

void ProjectileMagicWindRouter::EmitImpact(const PendingImpact& a_impact)
{
	if (settings.impactStrength <= 0.0f)
		return;
	const float radius = std::clamp(a_impact.profile.radius * settings.radiusMultiplier,
		kMinimumRadius, kMaximumRadius);
	const auto source = WindField::MakeRadialWave(
		{ a_impact.position.x, a_impact.position.y, a_impact.position.z },
		GetHorizontalVelocityDirection(a_impact.velocity), settings.impactStrength * a_impact.profile.strength,
		radius, std::clamp(radius * 0.35f, 64.0f, 600.0f),
		std::clamp(a_impact.profile.propagationSpeed, 1400.0f, 4500.0f), settings.decayTime);
	State::GetSingleton()->QueueTransientWindSource(source,
		State::TransientWindSourceOwner::ProjectileMagic,
		State::TransientWindSourcePriority::Impact);
}

bool ProjectileMagicWindRouter::AcceptImpactLocked(const RE::Projectile& a_projectile,
	const RE::NiPoint3& a_position)
{
	const auto now = std::chrono::steady_clock::now();
	const auto projectileIdentity = reinterpret_cast<std::uintptr_t>(std::addressof(a_projectile));
	const auto secondsSince = [&](const RecentImpact& a_recent) {
		return std::chrono::duration<float>(now - a_recent.time).count();
	};

	std::erase_if(recentImpacts, [&](const RecentImpact& a_recent) {
		return secondsSince(a_recent) > kDuplicateWindow;
	});
	if (std::ranges::count_if(recentImpacts, [&](const RecentImpact& a_recent) {
			return secondsSince(a_recent) <= kBurstWindow;
		}) >= kMaximumSourcesPerBurst)
		return false;

	for (const auto& recent : recentImpacts) {
		const bool sameProjectile = recent.projectile == projectileIdentity;
		if (!sameProjectile && secondsSince(recent) > kCrossImpactWindow)
			continue;
		const float deduplicationRadius = sameProjectile ? 256.0f : 96.0f;
		if (SquaredDistance(recent.position, a_position) <= deduplicationRadius * deduplicationRadius)
			return false;
	}

	recentImpacts.push_back({ projectileIdentity, a_position, now });
	if (recentImpacts.size() > kMaximumRecentImpacts)
		recentImpacts.erase(recentImpacts.begin());
	return true;
}

void ProjectileMagicWindRouter::SanitizeSettings()
{
	const Settings defaults{};
	settings.launchStrength = ClampFiniteOrDefault(
		settings.launchStrength, kMinimumStrength, kMaximumStrength, defaults.launchStrength);
	settings.impactStrength = ClampFiniteOrDefault(
		settings.impactStrength, kMinimumStrength, kMaximumStrength, defaults.impactStrength);
	settings.dragonStrength = ClampFiniteOrDefault(
		settings.dragonStrength, kMinimumStrength, kMaximumStrength, defaults.dragonStrength);
	settings.radiusMultiplier = ClampFiniteOrDefault(settings.radiusMultiplier,
		kMinimumRadiusMultiplier, kMaximumRadiusMultiplier, defaults.radiusMultiplier);
	settings.decayTime = ClampFiniteOrDefault(settings.decayTime,
		0.0f, WindField::kTransientImpulseMaximumDecayTime, defaults.decayTime);
}
