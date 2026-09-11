#include "SpellShoutWindRouter.h"

#include "ActorWind.h"
#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindMath.h"
#include "I18n/I18n.h"
#include "ShoutWindProfiles.h"
#include "State.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <nlohmann/json.hpp>
#include <numbers>

namespace
{
	constexpr std::string_view kSkyrimMaster = "Skyrim.esm";
	constexpr std::string_view kDragonbornMaster = "Dragonborn.esm";
	constexpr RE::FormID kWhirlwindSprintFormID = 0x2F7BA;
	constexpr RE::FormID kFireBreathFormID = 0x3F9EA;
	constexpr RE::FormID kFrostBreathFormID = 0x5D16B;
	constexpr RE::FormID kCycloneFormID = 0x0200C0;
	constexpr RE::FormID kFireStormFormID = 0x7A82B;
	constexpr RE::FormID kBlizzardFormID = 0x7E8E4;
	constexpr RE::FormID kLightningStormFormID = 0x7E8E5;
	constexpr float kStrengthMinimum = 0.0f;
	constexpr float kStrengthMaximum = 5.0f;
	constexpr std::size_t kMaximumPendingCasts = 64;

	using DirectionalProfile = ShoutWindProfiles::DirectionalProfile;

	struct RadialProfile
	{
		float strength;
		float distance;
		float waveHalfWidth;
		float propagationSpeed;
		float decayTime;
	};

	constexpr std::array<DirectionalProfile, RE::TESShout::VariationIDs::kTotal> kWhirlwindProfiles{
		DirectionalProfile{ 1.2f, 900.0f, 180.0f, 2600.0f, 18.0f, 0.35f },
		DirectionalProfile{ 1.7f, 1500.0f, 240.0f, 3000.0f, 20.0f, 0.45f },
		DirectionalProfile{ 2.2f, 2300.0f, 320.0f, 3400.0f, 22.0f, 0.55f }
	};
	constexpr std::array<DirectionalProfile, RE::TESShout::VariationIDs::kTotal> kCycloneProfiles{
		DirectionalProfile{ 1.0f, 1600.0f, 300.0f, 1500.0f, 32.0f, 1.2f },
		DirectionalProfile{ 1.5f, 2400.0f, 400.0f, 1700.0f, 38.0f, 1.5f },
		DirectionalProfile{ 2.1f, 3200.0f, 520.0f, 1900.0f, 44.0f, 1.8f }
	};
	constexpr std::array<DirectionalProfile, RE::TESShout::VariationIDs::kTotal> kFrostBreathProfiles{
		DirectionalProfile{ 0.7f, 1000.0f, 240.0f, 1500.0f, 26.0f, 0.8f },
		DirectionalProfile{ 1.0f, 1500.0f, 320.0f, 1700.0f, 31.0f, 1.0f },
		DirectionalProfile{ 1.4f, 2100.0f, 420.0f, 1900.0f, 36.0f, 1.2f }
	};
	constexpr RadialProfile kFireStormProfile{ 2.5f, 2400.0f, 420.0f, 2800.0f, 1.0f };
	constexpr RadialProfile kBlizzardProfile{ 1.8f, 2600.0f, 540.0f, 1700.0f, 2.5f };
	constexpr DirectionalProfile kLightningStormProfile{ 1.8f, 3600.0f, 280.0f, 4200.0f, 14.0f, 0.4f };

	template <class T>
	T* LookupRecord(RE::TESDataHandler& a_dataHandler, std::string_view a_editorID,
		RE::FormID a_localFormID, std::string_view a_plugin)
	{
		if (auto* record = a_dataHandler.LookupForm<T>(a_localFormID, a_plugin))
			return record;
		return RE::TESForm::LookupByEditorID<T>(a_editorID);
	}

	float ConeCosine(float a_halfAngleDegrees)
	{
		return std::cos(a_halfAngleDegrees * (std::numbers::pi_v<float> / 180.0f));
	}

	std::string_view GetEffectName(SpellShoutWindEffect a_effect)
	{
		switch (a_effect) {
		case SpellShoutWindEffect::WhirlwindSprint:
			return "Whirlwind Sprint";
		case SpellShoutWindEffect::Cyclone:
			return "Cyclone";
		case SpellShoutWindEffect::FireBreath:
			return "Fire Breath";
		case SpellShoutWindEffect::FrostBreath:
			return "Frost Breath";
		case SpellShoutWindEffect::FireStorm:
			return "Fire Storm";
		case SpellShoutWindEffect::Blizzard:
			return "Blizzard";
		case SpellShoutWindEffect::LightningStorm:
			return "Lightning Storm";
		}
		return "Unknown";
	}
}

using WindMath::ClampFiniteOrDefault;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SpellShoutWindRouter::Settings,
	enabled,
	strength,
	whirlwindSprint,
	cyclone,
	elementalBreath,
	masterDestruction)

std::string SpellShoutWindRouter::GetDisplayName() const
{
	return T("feature.wind.wind_effect.spell_shout.name", "Spells and Shouts");
}

void SpellShoutWindRouter::DrawSettings()
{
	if (ImGui::Checkbox(T("feature.wind.wind_effect.spell_shout.enabled", "Enable Spell and Shout Wind"),
			&settings.enabled) &&
		!settings.enabled) {
		Reset();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.wind.wind_effect.spell_shout.enabled_tooltip",
			"Adds focused transient wind to major shouts and master Destruction spell activations."));

	ImGui::BeginDisabled(!settings.enabled);
	ImGui::SliderFloat(T("feature.wind.wind_effect.spell_shout.strength", "Master Strength"),
		&settings.strength, kStrengthMinimum, kStrengthMaximum, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	ImGui::Checkbox(T("feature.wind.wind_effect.spell_shout.whirlwind_sprint", "Whirlwind Sprint"), &settings.whirlwindSprint);
	ImGui::Checkbox(T("feature.wind.wind_effect.spell_shout.cyclone", "Cyclone"), &settings.cyclone);
	ImGui::Checkbox(T("feature.wind.wind_effect.spell_shout.elemental_breath", "Fire and Frost Breath"), &settings.elementalBreath);
	ImGui::Checkbox(T("feature.wind.wind_effect.spell_shout.master_destruction", "Master Destruction Activations"), &settings.masterDestruction);
	ImGui::EndDisabled();
}

void SpellShoutWindRouter::LoadSettings(const nlohmann::json& a_json)
{
	settings = a_json;
	SanitizeSettings();
}

void SpellShoutWindRouter::SaveSettings(nlohmann::json& a_json) const
{
	a_json = settings;
}

void SpellShoutWindRouter::RestoreDefaultSettings()
{
	settings = {};
}

void SpellShoutWindRouter::DataLoaded()
{
	if (registered)
		return;
	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
	if (!dataHandler || !eventSourceHolder) {
		logger::warn("Unable to register spell and shout wind: game data is unavailable");
		return;
	}

	routes.clear();
	RegisterShout(*dataHandler, "WhirlwindSprintShout", kWhirlwindSprintFormID,
		kSkyrimMaster, SpellShoutWindEffect::WhirlwindSprint);
	RegisterShout(*dataHandler, "DLC2CycloneShout", kCycloneFormID,
		kDragonbornMaster, SpellShoutWindEffect::Cyclone);
	RegisterShout(*dataHandler, "FireBreathShout", kFireBreathFormID,
		kSkyrimMaster, SpellShoutWindEffect::FireBreath);
	RegisterShout(*dataHandler, "FrostBreathShout", kFrostBreathFormID,
		kSkyrimMaster, SpellShoutWindEffect::FrostBreath);
	RegisterSpell(*dataHandler, "FireStorm", kFireStormFormID,
		kSkyrimMaster, SpellShoutWindEffect::FireStorm);
	RegisterSpell(*dataHandler, "Blizzard", kBlizzardFormID,
		kSkyrimMaster, SpellShoutWindEffect::Blizzard);
	RegisterSpell(*dataHandler, "LightningStorm", kLightningStormFormID,
		kSkyrimMaster, SpellShoutWindEffect::LightningStorm);

	eventSourceHolder->AddEventSink<RE::TESSpellCastEvent>(this);
	registered = true;
	logger::info("Registered spell and shout wind router with {} cast records", routes.size());
}

void SpellShoutWindRouter::Reset()
{
	{
		std::lock_guard lock(pendingCastsMutex);
		pendingCasts.clear();
	}
	State::GetSingleton()->ClearTransientWindSources(State::TransientWindSourceOwner::SpellShout);
}

void SpellShoutWindRouter::CollectOwnedMagicItems(RE::TESDataHandler& a_dataHandler,
	std::unordered_set<RE::FormID>& a_formIDs)
{
	const auto addShout = [&](std::string_view a_editorID, RE::FormID a_localFormID,
							  std::string_view a_plugin) {
		const auto* shout = LookupRecord<RE::TESShout>(a_dataHandler, a_editorID, a_localFormID, a_plugin);
		if (!shout)
			return;
		for (const auto& variation : shout->variations) {
			if (variation.spell)
				a_formIDs.emplace(variation.spell->GetFormID());
		}
	};
	const auto addSpell = [&](std::string_view a_editorID, RE::FormID a_localFormID) {
		if (const auto* spell = LookupRecord<RE::SpellItem>(
				a_dataHandler, a_editorID, a_localFormID, kSkyrimMaster))
			a_formIDs.emplace(spell->GetFormID());
	};

	addShout("WhirlwindSprintShout", kWhirlwindSprintFormID, kSkyrimMaster);
	addShout("DLC2CycloneShout", kCycloneFormID, kDragonbornMaster);
	addShout("FireBreathShout", kFireBreathFormID, kSkyrimMaster);
	addShout("FrostBreathShout", kFrostBreathFormID, kSkyrimMaster);
	addSpell("FireStorm", kFireStormFormID);
	addSpell("Blizzard", kBlizzardFormID);
	addSpell("LightningStorm", kLightningStormFormID);
}

std::optional<SpellShoutWindRoute> SpellShoutWindRouter::Classify(RE::FormID a_spell) const
{
	const auto route = routes.find(a_spell);
	return route != routes.end() ? std::optional{ route->second } : std::nullopt;
}

RE::BSEventNotifyControl SpellShoutWindRouter::ProcessEvent(const RE::TESSpellCastEvent* a_event,
	RE::BSTEventSource<RE::TESSpellCastEvent>*)
{
	if (!a_event || !a_event->object)
		return RE::BSEventNotifyControl::kContinue;
	if (!routes.contains(a_event->spell))
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

void SpellShoutWindRouter::Update(float)
{
	std::vector<PendingCast> casts;
	{
		std::lock_guard lock(pendingCastsMutex);
		casts.swap(pendingCasts);
	}
	if (!settings.enabled || settings.strength <= 0.0f)
		return;

	for (const auto& cast : casts) {
		const auto route = Classify(cast.spellFormID);
		if (!route || !IsEnabled(route->effect))
			continue;
		auto actor = cast.actor.get();
		if (!actor)
			continue;
		if (route->shout && ActorWind::IsDragon(*actor))
			continue;
		QueueEffect(*actor, *route);
	}
}

void SpellShoutWindRouter::RegisterShout(RE::TESDataHandler& a_dataHandler,
	std::string_view a_editorID, RE::FormID a_localFormID, std::string_view a_plugin,
	SpellShoutWindEffect a_effect)
{
	auto* shout = LookupRecord<RE::TESShout>(a_dataHandler, a_editorID, a_localFormID, a_plugin);
	if (!shout) {
		logger::warn("Spell and shout wind could not resolve {}", a_editorID);
		return;
	}
	for (uint8_t rank = 0; rank < RE::TESShout::VariationIDs::kTotal; ++rank) {
		if (auto* spell = shout->variations[rank].spell)
			routes.insert_or_assign(spell->GetFormID(), SpellShoutWindRoute{ a_effect, rank, spell, true });
	}
}

void SpellShoutWindRouter::RegisterSpell(RE::TESDataHandler& a_dataHandler,
	std::string_view a_editorID, RE::FormID a_localFormID, std::string_view a_plugin,
	SpellShoutWindEffect a_effect)
{
	auto* spell = LookupRecord<RE::SpellItem>(a_dataHandler, a_editorID, a_localFormID, a_plugin);
	if (!spell) {
		logger::warn("Spell and shout wind could not resolve {}", a_editorID);
		return;
	}
	routes.insert_or_assign(spell->GetFormID(), SpellShoutWindRoute{ a_effect, 0, spell, false });
}

void SpellShoutWindRouter::QueueEffect(RE::Actor& a_actor, const SpellShoutWindRoute& a_route) const
{
	const auto queueDirectional = [&](const DirectionalProfile& a_profile) {
		const auto source = WindField::MakeDirectionalWave(
			ActorWind::GetMagicOrigin(a_actor), ActorWind::GetAimDirection(a_actor),
			settings.strength * a_profile.strength, a_profile.distance, a_profile.waveHalfWidth,
			a_profile.propagationSpeed, ConeCosine(a_profile.coneHalfAngle), a_profile.decayTime);
		const auto priority = a_route.effect == SpellShoutWindEffect::FireBreath ||
		                              a_route.effect == SpellShoutWindEffect::FrostBreath ?
		                          State::TransientWindSourcePriority::Breath :
		                          State::TransientWindSourcePriority::Flight;
		State::GetSingleton()->QueueTransientWindSource(
			source, State::TransientWindSourceOwner::SpellShout, priority);
	};
	const auto queueRadial = [&](const RadialProfile& a_profile) {
		const auto source = WindField::MakeRadialWave(
			ActorWind::GetVisualOrigin(a_actor), ActorWind::GetAimDirection(a_actor),
			settings.strength * a_profile.strength, a_profile.distance, a_profile.waveHalfWidth,
			a_profile.propagationSpeed, a_profile.decayTime);
		State::GetSingleton()->QueueTransientWindSource(source,
			State::TransientWindSourceOwner::SpellShout,
			State::TransientWindSourcePriority::Impact);
	};

	const std::size_t rank = std::min<std::size_t>(a_route.rank, RE::TESShout::VariationIDs::kTotal - 1);
	switch (a_route.effect) {
	case SpellShoutWindEffect::WhirlwindSprint:
		queueDirectional(kWhirlwindProfiles[rank]);
		break;
	case SpellShoutWindEffect::Cyclone:
		queueDirectional(kCycloneProfiles[rank]);
		break;
	case SpellShoutWindEffect::FireBreath:
		queueDirectional(ShoutWindProfiles::kFireBreathProfiles[rank]);
		break;
	case SpellShoutWindEffect::FrostBreath:
		queueDirectional(kFrostBreathProfiles[rank]);
		break;
	case SpellShoutWindEffect::FireStorm:
		queueRadial(kFireStormProfile);
		break;
	case SpellShoutWindEffect::Blizzard:
		queueRadial(kBlizzardProfile);
		break;
	case SpellShoutWindEffect::LightningStorm:
		queueDirectional(kLightningStormProfile);
		break;
	}
	logger::debug("Queued {} transient wind: rank {}, caster {:08X}",
		GetEffectName(a_route.effect), rank + 1, a_actor.GetFormID());
}

bool SpellShoutWindRouter::IsEnabled(SpellShoutWindEffect a_effect) const
{
	switch (a_effect) {
	case SpellShoutWindEffect::WhirlwindSprint:
		return settings.whirlwindSprint;
	case SpellShoutWindEffect::Cyclone:
		return settings.cyclone;
	case SpellShoutWindEffect::FireBreath:
	case SpellShoutWindEffect::FrostBreath:
		return settings.elementalBreath;
	case SpellShoutWindEffect::FireStorm:
	case SpellShoutWindEffect::Blizzard:
	case SpellShoutWindEffect::LightningStorm:
		return settings.masterDestruction;
	}
	return false;
}

void SpellShoutWindRouter::SanitizeSettings()
{
	const Settings defaults{};
	settings.strength = ClampFiniteOrDefault(
		settings.strength, kStrengthMinimum, kStrengthMaximum, defaults.strength);
}
