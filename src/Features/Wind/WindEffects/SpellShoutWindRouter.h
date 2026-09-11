#pragma once

#include "WindEffect.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/** Identifies a spell or shout family routed to a transient wind handler. */
enum class SpellShoutWindEffect : uint8_t
{
	WhirlwindSprint,
	Cyclone,
	FireBreath,
	FrostBreath,
	FireStorm,
	Blizzard,
	LightningStorm
};

/** Describes the classified spell and its rank within a shout family. */
struct SpellShoutWindRoute
{
	SpellShoutWindEffect effect;
	uint8_t rank{};
	const RE::SpellItem* spell{};
	bool shout{};
};

/** Routes major spell and shout casts into one-shot shared wind sources. */
class SpellShoutWindRouter final : public WindEffect, public RE::BSTEventSink<RE::TESSpellCastEvent>
{
public:
	struct Settings
	{
		bool enabled = true;
		float strength = 1.0f;
		bool whirlwindSprint = true;
		bool cyclone = true;
		bool elementalBreath = true;
		bool masterDestruction = true;
	};

	/** @copydoc WindEffect::GetId */
	[[nodiscard]] std::string_view GetId() const override { return "spellShout"; }
	/** @copydoc WindEffect::GetDisplayName */
	[[nodiscard]] std::string GetDisplayName() const override;
	/** @copydoc WindEffect::DrawSettings */
	void DrawSettings() override;
	/** @copydoc WindEffect::LoadSettings */
	void LoadSettings(const nlohmann::json& a_json) override;
	/** @copydoc WindEffect::SaveSettings */
	void SaveSettings(nlohmann::json& a_json) const override;
	/** @copydoc WindEffect::RestoreDefaultSettings */
	void RestoreDefaultSettings() override;
	/** @copydoc WindEffect::DataLoaded */
	void DataLoaded() override;
	/** @copydoc WindEffect::Update */
	void Update(float a_frameTime) override;
	/** @copydoc WindEffect::Reset */
	void Reset() override;

	/** Returns the routed family and shout rank for a cast spell FormID. */
	[[nodiscard]] std::optional<SpellShoutWindRoute> Classify(RE::FormID a_spell) const;

	/** Adds every bespoke spell and shout variation owned by this router to an exclusion set. */
	static void CollectOwnedMagicItems(RE::TESDataHandler& a_dataHandler,
		std::unordered_set<RE::FormID>& a_formIDs);

private:
	struct PendingCast
	{
		RE::ActorHandle actor;
		RE::FormID actorFormID{};
		RE::FormID spellFormID{};
	};

	RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
		RE::BSTEventSource<RE::TESSpellCastEvent>*) override;
	void RegisterShout(RE::TESDataHandler& a_dataHandler, std::string_view a_editorID,
		RE::FormID a_localFormID, std::string_view a_plugin, SpellShoutWindEffect a_effect);
	void RegisterSpell(RE::TESDataHandler& a_dataHandler, std::string_view a_editorID,
		RE::FormID a_localFormID, std::string_view a_plugin, SpellShoutWindEffect a_effect);
	void QueueEffect(RE::Actor& a_actor, const SpellShoutWindRoute& a_route) const;
	[[nodiscard]] bool IsEnabled(SpellShoutWindEffect a_effect) const;
	void SanitizeSettings();

	Settings settings;
	std::unordered_map<RE::FormID, SpellShoutWindRoute> routes;
	std::mutex pendingCastsMutex;
	std::vector<PendingCast> pendingCasts;
	bool registered = false;
};
