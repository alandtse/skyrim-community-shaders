#pragma once

#include "WindEffect.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/** Adds an Unrelenting Force pressure wave to the shared wind field. */
class FusRoDahWind final : public WindEffect, public RE::BSTEventSink<RE::TESSpellCastEvent>
{
public:
	struct Settings
	{
		bool enabled = true;
		float intensity = 1.0f;
		float decayTime = 1.49f;
		float distanceMultiplier = 3.0f;
		float widthMultiplier = 1.61f;
		float speedMultiplier = 1.0f;
	};

	/** @copydoc WindEffect::GetId */
	[[nodiscard]] std::string_view GetId() const override { return "fusRoDah"; }

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

	/** Adds every Unrelenting Force variation, including dragon records, to an exclusion set. */
	static void CollectOwnedMagicItems(RE::TESDataHandler& a_dataHandler,
		std::unordered_set<RE::FormID>& a_formIDs);

private:
	struct Route
	{
		uint8_t rank{};
		const RE::SpellItem* canonicalSpell{};
	};

	struct PendingCast
	{
		RE::ActorHandle actor;
		RE::FormID actorFormID{};
		RE::FormID spellFormID{};
	};

	RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
		RE::BSTEventSource<RE::TESSpellCastEvent>*) override;
	void QueueEffect(RE::Actor& a_actor, const Route& a_route) const;
	void SanitizeSettings();

	Settings settings;
	std::unordered_map<RE::FormID, Route> routes;
	std::mutex pendingCastsMutex;
	std::vector<PendingCast> pendingCasts;
	bool registered = false;
};
