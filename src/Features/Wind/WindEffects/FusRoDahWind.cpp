#include "FusRoDahWind.h"

#include "ActorWind.h"
#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindMath.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <numbers>
#include <utility>

namespace
{
	constexpr RE::FormID kUnrelentingForceFormID = 0x13E07;
	constexpr std::string_view kSkyrimMaster = "Skyrim.esm";
	constexpr float kIntensityMin = 0.0f;
	constexpr float kIntensityMax = 5.0f;
	constexpr float kDecayTimeMin = 0.0f;
	constexpr float kDecayTimeMax = WindField::kTransientImpulseMaximumDecayTime;
	constexpr float kDistanceMultiplierMin = 0.25f;
	constexpr float kDistanceMultiplierMax = 3.0f;
	constexpr float kWidthMultiplierMin = 0.25f;
	constexpr float kWidthMultiplierMax = 3.0f;
	constexpr float kSpeedMultiplierMin = 0.25f;
	constexpr float kSpeedMultiplierMax = 3.0f;
	constexpr float kSideSpreadMultiplier = 2.0f;
	constexpr float kMaximumSideHalfAngle = 80.0f;
	constexpr float kVerticalScale = 0.4f;
	constexpr float kFalloffWidthScale = 0.5f;
	constexpr float kRangeScale = 2.0f;
	constexpr float kSizeScale = 1.3f;
	constexpr std::size_t kMaximumPendingCasts = 32;

	struct ResolvedVariation
	{
		RE::FormID spellFormID{};
		uint8_t rank{};
		const RE::SpellItem* canonicalSpell{};
	};

	struct RankProfile
	{
		float strength;
		float maxDistance;
		float waveHalfWidth;
		float propagationSpeed;
		float coneHalfAngle;
		float collisionRadius;
	};

	constexpr std::array<RankProfile, RE::TESShout::VariationIDs::kTotal> kRankProfiles{
		RankProfile{ 0.9f, 1000.0f, 180.0f, 1500.0f, 15.0f, 16.0f },
		RankProfile{ 1.5f, 1800.0f, 260.0f, 1800.0f, 15.0f, 16.0f },
		RankProfile{ 2.3f, 2800.0f, 360.0f, 2100.0f, 10.0f, 32.0f }
	};

	std::vector<ResolvedVariation> ResolveVariations(RE::TESDataHandler& a_dataHandler)
	{
		const auto* canonicalShout =
			a_dataHandler.LookupForm<RE::TESShout>(kUnrelentingForceFormID, kSkyrimMaster);
		if (!canonicalShout)
			return {};

		std::array<RE::FormID, RE::TESShout::VariationIDs::kTotal> wordFormIDs{};
		for (std::size_t rank = 0; rank < wordFormIDs.size(); ++rank) {
			if (const auto* word = canonicalShout->variations[rank].word)
				wordFormIDs[rank] = word->GetFormID();
		}

		std::vector<ResolvedVariation> result;
		for (const auto* shout : a_dataHandler.GetFormArray<RE::TESShout>()) {
			if (!shout)
				continue;
			for (const auto& variation : shout->variations) {
				if (!variation.word || !variation.spell)
					continue;
				const auto found = std::ranges::find(wordFormIDs, variation.word->GetFormID());
				if (found == wordFormIDs.end())
					continue;
				const auto rank = static_cast<std::size_t>(std::distance(wordFormIDs.begin(), found));
				result.push_back({ variation.spell->GetFormID(), static_cast<uint8_t>(rank),
					canonicalShout->variations[rank].spell });
			}
		}

		for (std::size_t rank = 0; rank < wordFormIDs.size(); ++rank) {
			const auto* spell = canonicalShout->variations[rank].spell;
			if (!spell)
				continue;
			const auto duplicate = std::ranges::find(result, spell->GetFormID(), &ResolvedVariation::spellFormID);
			if (duplicate == result.end())
				result.push_back({ spell->GetFormID(), static_cast<uint8_t>(rank), spell });
		}
		return result;
	}

	std::pair<float, float> GetProjectileShape(const RE::SpellItem* a_spell, const RankProfile& a_fallback)
	{
		if (a_spell) {
			for (const auto* effect : a_spell->effects) {
				const auto* projectile = effect && effect->baseEffect ? effect->baseEffect->data.projectileBase : nullptr;
				if (projectile && projectile->IsCone() && std::isfinite(projectile->data.coneSpread) &&
					projectile->data.coneSpread > 0.0f && std::isfinite(projectile->data.collisionRadius) &&
					projectile->data.collisionRadius > 0.0f) {
					return { projectile->data.coneSpread, projectile->data.collisionRadius };
				}
			}
		}
		return { a_fallback.coneHalfAngle, a_fallback.collisionRadius };
	}

}

using WindMath::ClampFiniteOrDefault;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FusRoDahWind::Settings,
	enabled,
	intensity,
	decayTime,
	distanceMultiplier,
	widthMultiplier,
	speedMultiplier)

std::string FusRoDahWind::GetDisplayName() const
{
	return T("feature.wind.wind_effect.fus_ro_dah.name", "Fus Ro Dah");
}

void FusRoDahWind::DrawSettings()
{
	if (ImGui::Checkbox(T("feature.wind.wind_effect.fus_ro_dah.enabled", "Enable Wind Impulse"), &settings.enabled) &&
		!settings.enabled)
		globals::state->ClearTransientWindSources(State::TransientWindSourceOwner::FusRoDah);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.fus_ro_dah.enabled_tooltip",
			"Adds Unrelenting Force as a directional wave traveling through the shared wind field."));

	ImGui::BeginDisabled(!settings.enabled);
	ImGui::SliderFloat(T("feature.wind.wind_effect.fus_ro_dah.intensity", "Intensity"), &settings.intensity,
		kIntensityMin, kIntensityMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.fus_ro_dah.intensity_tooltip",
			"Scales the rank-specific wind velocity added at the moving pressure wave."));
	ImGui::SliderFloat(T("feature.wind.wind_effect.fus_ro_dah.decay_time", "Trailing Falloff"), &settings.decayTime,
		kDecayTimeMin, kDecayTimeMax, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.fus_ro_dah.decay_time_tooltip",
			"Controls how quickly pressure fades behind the moving front. Zero removes the extra trailing falloff."));
	ImGui::SliderFloat(T("feature.wind.wind_effect.fus_ro_dah.distance_multiplier", "Propagation Distance"),
		&settings.distanceMultiplier, kDistanceMultiplierMin, kDistanceMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.fus_ro_dah.distance_multiplier_tooltip",
			"Scales the rank-specific distance the pressure wave can travel."));
	ImGui::SliderFloat(T("feature.wind.wind_effect.fus_ro_dah.width_multiplier", "Wave Width"),
		&settings.widthMultiplier, kWidthMultiplierMin, kWidthMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.fus_ro_dah.width_multiplier_tooltip",
			"Scales the moving pressure-front thickness and the soft falloff surrounding its oval core."));
	ImGui::SliderFloat(T("feature.wind.wind_effect.fus_ro_dah.speed_multiplier", "Propagation Speed"),
		&settings.speedMultiplier, kSpeedMultiplierMin, kSpeedMultiplierMax, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.fus_ro_dah.speed_multiplier_tooltip",
			"Scales how quickly the wavefront travels through the shared wind field."));
	ImGui::EndDisabled();
}

void FusRoDahWind::LoadSettings(const nlohmann::json& a_json)
{
	settings = a_json;
	settings.enabled = a_json.value("enableFusRoDahWind", settings.enabled);
	settings.intensity = a_json.value("fusRoDahIntensity", settings.intensity);
	settings.decayTime = a_json.value("fusRoDahDecayTime", settings.decayTime);
	settings.distanceMultiplier = a_json.value("fusRoDahDistanceMultiplier", settings.distanceMultiplier);
	settings.widthMultiplier = a_json.value("fusRoDahWidthMultiplier", settings.widthMultiplier);
	settings.speedMultiplier = a_json.value("fusRoDahSpeedMultiplier", settings.speedMultiplier);
	SanitizeSettings();
}

void FusRoDahWind::SaveSettings(nlohmann::json& a_json) const
{
	a_json = settings;
}

void FusRoDahWind::RestoreDefaultSettings()
{
	settings = {};
}

void FusRoDahWind::SanitizeSettings()
{
	const Settings defaults{};
	settings.intensity = ClampFiniteOrDefault(settings.intensity, kIntensityMin, kIntensityMax, defaults.intensity);
	settings.decayTime = ClampFiniteOrDefault(settings.decayTime, kDecayTimeMin, kDecayTimeMax, defaults.decayTime);
	settings.distanceMultiplier = ClampFiniteOrDefault(settings.distanceMultiplier,
		kDistanceMultiplierMin, kDistanceMultiplierMax, defaults.distanceMultiplier);
	settings.widthMultiplier = ClampFiniteOrDefault(settings.widthMultiplier,
		kWidthMultiplierMin, kWidthMultiplierMax, defaults.widthMultiplier);
	settings.speedMultiplier = ClampFiniteOrDefault(settings.speedMultiplier,
		kSpeedMultiplierMin, kSpeedMultiplierMax, defaults.speedMultiplier);
}

void FusRoDahWind::DataLoaded()
{
	if (registered)
		return;

	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
	if (!dataHandler || !eventSourceHolder) {
		logger::warn("Unable to register Unrelenting Force wind impulses: game data is unavailable");
		return;
	}

	routes.clear();
	for (const auto& variation : ResolveVariations(*dataHandler))
		routes.insert_or_assign(variation.spellFormID, Route{ variation.rank, variation.canonicalSpell });
	if (routes.empty()) {
		logger::warn("Unable to register Unrelenting Force wind impulses: shout record not found");
		return;
	}

	eventSourceHolder->AddEventSink<RE::TESSpellCastEvent>(this);
	registered = true;
	logger::info("Registered {} player and dragon Unrelenting Force variations", routes.size());
}

void FusRoDahWind::CollectOwnedMagicItems(RE::TESDataHandler& a_dataHandler,
	std::unordered_set<RE::FormID>& a_formIDs)
{
	for (const auto& variation : ResolveVariations(a_dataHandler))
		a_formIDs.emplace(variation.spellFormID);
}

void FusRoDahWind::Reset()
{
	{
		std::lock_guard lock(pendingCastsMutex);
		pendingCasts.clear();
	}
	State::GetSingleton()->ClearTransientWindSources(State::TransientWindSourceOwner::FusRoDah);
}

RE::BSEventNotifyControl FusRoDahWind::ProcessEvent(const RE::TESSpellCastEvent* a_event,
	RE::BSTEventSource<RE::TESSpellCastEvent>*)
{
	if (!a_event || !a_event->object || !routes.contains(a_event->spell))
		return RE::BSEventNotifyControl::kContinue;

	auto* actor = a_event->object->As<RE::Actor>();
	if (!actor)
		return RE::BSEventNotifyControl::kContinue;

	const PendingCast cast{ actor->GetHandle(), actor->GetFormID(), a_event->spell };
	std::lock_guard lock(pendingCastsMutex);
	const auto duplicate = std::ranges::find_if(pendingCasts, [&](const PendingCast& a_pending) {
		return a_pending.actorFormID == cast.actorFormID && a_pending.spellFormID == cast.spellFormID;
	});
	if (duplicate == pendingCasts.end() && pendingCasts.size() < kMaximumPendingCasts)
		pendingCasts.push_back(cast);
	return RE::BSEventNotifyControl::kContinue;
}

void FusRoDahWind::Update(float)
{
	std::vector<PendingCast> casts;
	{
		std::lock_guard lock(pendingCastsMutex);
		casts.swap(pendingCasts);
	}
	if (routes.empty() || !settings.enabled || settings.intensity <= 0.0f)
		return;

	for (const auto& cast : casts) {
		const auto route = routes.find(cast.spellFormID);
		if (route == routes.end())
			continue;
		if (auto actor = cast.actor.get())
			QueueEffect(*actor, route->second);
	}
}

void FusRoDahWind::QueueEffect(RE::Actor& a_actor, const Route& a_route) const
{
	const auto origin = ActorWind::GetMagicOrigin(a_actor);
	const auto forward = ActorWind::GetAimDirection(a_actor);
	const std::size_t rank = std::min<std::size_t>(a_route.rank, kRankProfiles.size() - 1);
	const auto& profile = kRankProfiles[rank];
	const auto [coneHalfAngle, collisionRadius] =
		GetProjectileShape(a_route.canonicalSpell, profile);
	const float coneHalfAngleRadians = coneHalfAngle * (std::numbers::pi_v<float> / 180.0f);
	const float sideHalfAngleRadians =
		std::min(coneHalfAngle * kSideSpreadMultiplier, kMaximumSideHalfAngle) *
		(std::numbers::pi_v<float> / 180.0f);
	const WindField::TransientWindSource source{ { origin.x, origin.y, origin.z },
		0.0f,
		{ forward.x, forward.y, forward.z },
		profile.strength * settings.intensity,
		profile.maxDistance * settings.distanceMultiplier * kRangeScale,
		profile.waveHalfWidth * settings.widthMultiplier * kSizeScale,
		profile.propagationSpeed * settings.speedMultiplier,
		std::tan(coneHalfAngleRadians) * kSizeScale,
		settings.decayTime,
		collisionRadius * kSizeScale,
		std::tan(sideHalfAngleRadians) * kSizeScale,
		1.0f,
		WindField::TransientWindSourceType::DirectionalOvalWave,
		kVerticalScale,
		profile.waveHalfWidth * settings.widthMultiplier * kFalloffWidthScale * kSizeScale,
		0.0f };
	State::GetSingleton()->QueueTransientWindSource(source,
		State::TransientWindSourceOwner::FusRoDah,
		State::TransientWindSourcePriority::FusRoDah);
	logger::info(
		"Queued Unrelenting Force wind impulse: rank {}, strength {:.3f}, core {:.1f} deg / {:.1f} radius, vertical {:.2f}x, "
		"origin ({:.1f}, {:.1f}, {:.1f}), direction ({:.3f}, {:.3f}, {:.3f})",
		rank + 1, profile.strength * settings.intensity,
		coneHalfAngle, collisionRadius, kVerticalScale, origin.x, origin.y, origin.z, forward.x, forward.y, forward.z);
}
