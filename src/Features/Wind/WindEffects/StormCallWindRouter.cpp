#include "StormCallWindRouter.h"

#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindMath.h"
#include "I18n/I18n.h"
#include "ProjectileHookDispatcher.h"
#include "State.h"
#include "StormCallRecords.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace
{
	constexpr float kMinimumStrength = 0.0f;
	constexpr float kMaximumStrength = 5.0f;
	constexpr float kMinimumRadius = 256.0f;
	constexpr float kMaximumRadius = 2400.0f;
	constexpr float kDuplicateWindow = 0.25f;
	constexpr float kCrossImpactWindow = 0.05f;
	constexpr float kBurstWindow = 0.1f;
	constexpr std::size_t kMaximumSourcesPerBurst = 16;
	constexpr std::size_t kMaximumRecentImpacts = 96;

}

using WindMath::ClampFiniteOrDefault;
using WindMath::GetHorizontalVelocityDirection;
using WindMath::SquaredDistance;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	StormCallWindRouter::Settings,
	enabled,
	strength,
	radius,
	decayTime)

StormCallWindRouter::StormCallWindRouter()
{
	ProjectileHookDispatcher::GetSingleton().AddImpactObserver(this, ObserveImpactCallback);
}

StormCallWindRouter::~StormCallWindRouter()
{
	ProjectileHookDispatcher::GetSingleton().RemoveObservers(this);
}

std::string StormCallWindRouter::GetDisplayName() const
{
	return T("feature.wind.wind_effect.storm_call.name", "Storm Call Strikes");
}

void StormCallWindRouter::DrawSettings()
{
	if (ImGui::Checkbox(T("feature.wind.wind_effect.storm_call.enabled", "Enable Storm Call Wind"),
			&settings.enabled) &&
		!settings.enabled) {
		Reset();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.storm_call.enabled_tooltip",
			"Adds a radial pressure wave at each exact Storm Call lightning impact."));

	ImGui::BeginDisabled(!settings.enabled);
	ImGui::SliderFloat(T("feature.wind.wind_effect.storm_call.strength", "Strength"),
		&settings.strength, kMinimumStrength, kMaximumStrength, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.storm_call.radius", "Radius"),
		&settings.radius, kMinimumRadius, kMaximumRadius, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.wind.wind_effect.storm_call.decay", "Trailing Falloff"),
		&settings.decayTime, 0.0f, WindField::kTransientImpulseMaximumDecayTime, "%.2f s",
		ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
}

void StormCallWindRouter::LoadSettings(const nlohmann::json& a_json)
{
	settings = a_json;
	SanitizeSettings();
}

void StormCallWindRouter::SaveSettings(nlohmann::json& a_json) const
{
	a_json = settings;
}

void StormCallWindRouter::RestoreDefaultSettings()
{
	settings = {};
}

void StormCallWindRouter::DataLoaded()
{
	if (recordsLoaded)
		return;
	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler) {
		logger::warn("Unable to register Storm Call strike wind: game data is unavailable");
		return;
	}

	boltStrengthBySpell.clear();
	boltStrengthByMagicEffect.clear();
	for (const auto& bolt : StormCallRecords::kBolts) {
		auto* spell = StormCallRecords::ResolveBolt(*dataHandler, bolt);
		if (!spell)
			continue;

		boltStrengthBySpell.emplace(spell->GetFormID(), bolt.strength);
		for (const auto* effectItem : spell->effects) {
			if (!effectItem || !effectItem->baseEffect)
				continue;
			boltStrengthByMagicEffect.insert_or_assign(effectItem->baseEffect->GetFormID(), bolt.strength);
			if (effectItem->baseEffect->data.projectileBase)
				ProjectileHookDispatcher::GetSingleton().ObserveProjectileType(
					*effectItem->baseEffect->data.projectileBase, ProjectileHookDispatcher::Event::Impact);
		}
	}

	if (auto* effect = dataHandler->LookupForm<RE::EffectSetting>(
			StormCallRecords::kEffectFormID, StormCallRecords::kPlugin);
		effect && effect->data.projectileBase)
		ProjectileHookDispatcher::GetSingleton().ObserveProjectileType(
			*effect->data.projectileBase, ProjectileHookDispatcher::Event::Impact);

	recordsLoaded = true;
	logger::info("Registered Storm Call strike wind for {} bolt spells and {} magic effects",
		boltStrengthBySpell.size(), boltStrengthByMagicEffect.size());
}

void StormCallWindRouter::Update(float)
{
	std::vector<PendingImpact> impacts;
	{
		std::lock_guard lock(pendingMutex);
		impacts.swap(pendingImpacts);
	}
	if (!settings.enabled || settings.strength <= 0.0f)
		return;

	for (const auto& impact : impacts) {
		const float waveHalfWidth = std::clamp(settings.radius * 0.3f, 96.0f, 500.0f);
		const float propagationSpeed = std::clamp(2200.0f + settings.radius, 2400.0f, 4500.0f);
		const auto source = WindField::MakeRadialWave(
			{ impact.position.x, impact.position.y, impact.position.z }, GetHorizontalVelocityDirection(impact.velocity),
			settings.strength * impact.strength, settings.radius, waveHalfWidth, propagationSpeed,
			settings.decayTime);
		State::GetSingleton()->QueueTransientWindSource(source,
			State::TransientWindSourceOwner::StormCall,
			State::TransientWindSourcePriority::Impact);
	}
}

void StormCallWindRouter::Reset()
{
	{
		std::lock_guard lock(pendingMutex);
		recentImpacts.clear();
		pendingImpacts.clear();
	}
	State::GetSingleton()->ClearTransientWindSources(State::TransientWindSourceOwner::StormCall);
}

void StormCallWindRouter::ObserveImpactCallback(void* a_owner, RE::Projectile& a_projectile,
	const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity)
{
	static_cast<StormCallWindRouter*>(a_owner)->ObserveImpact(a_projectile, a_position, a_velocity);
}

void StormCallWindRouter::ObserveImpact(RE::Projectile& a_projectile,
	const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity)
{
	if (!std::isfinite(a_position.x) || !std::isfinite(a_position.y) || !std::isfinite(a_position.z))
		return;
	const auto& runtimeData = a_projectile.GetProjectileRuntimeData();
	float strength = 0.0f;
	if (runtimeData.spell) {
		const auto route = boltStrengthBySpell.find(runtimeData.spell->GetFormID());
		if (route != boltStrengthBySpell.end())
			strength = route->second;
	}
	if (strength <= 0.0f && runtimeData.avEffect) {
		const auto route = boltStrengthByMagicEffect.find(runtimeData.avEffect->GetFormID());
		if (route != boltStrengthByMagicEffect.end())
			strength = route->second;
	}
	if (strength <= 0.0f)
		return;

	std::lock_guard lock(pendingMutex);
	if (!AcceptImpactLocked(a_projectile, a_position))
		return;
	if (pendingImpacts.size() >= kMaximumRecentImpacts)
		pendingImpacts.erase(pendingImpacts.begin());
	pendingImpacts.push_back({ a_position, a_velocity, strength });
}

bool StormCallWindRouter::AcceptImpactLocked(const RE::Projectile& a_projectile,
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

void StormCallWindRouter::SanitizeSettings()
{
	const Settings defaults{};
	settings.strength = ClampFiniteOrDefault(
		settings.strength, kMinimumStrength, kMaximumStrength, defaults.strength);
	settings.radius = ClampFiniteOrDefault(
		settings.radius, kMinimumRadius, kMaximumRadius, defaults.radius);
	settings.decayTime = ClampFiniteOrDefault(settings.decayTime,
		0.0f, WindField::kTransientImpulseMaximumDecayTime, defaults.decayTime);
}
