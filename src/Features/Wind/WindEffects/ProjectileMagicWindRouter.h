#pragma once

#include "WindEffect.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/** Routes loaded magic projectiles into launch and exact-impact transient wind sources. */
class ProjectileMagicWindRouter final :
	public WindEffect,
	public RE::BSTEventSink<RE::TESSpellCastEvent>
{
public:
	/** User tuning for record-driven magic projectile wind. */
	struct Settings
	{
		bool enabled = true;
		float launchStrength = 0.7f;
		float impactStrength = 0.8f;
		float dragonStrength = 1.35f;
		float radiusMultiplier = 1.0f;
		float decayTime = 0.65f;
	};

	/** Registers this router with the shared projectile impact dispatcher. */
	ProjectileMagicWindRouter();

	/** Unregisters this router from the shared projectile impact dispatcher. */
	~ProjectileMagicWindRouter() override;

	/** @copydoc WindEffect::GetId */
	[[nodiscard]] std::string_view GetId() const override { return "projectileMagic"; }

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

private:
	struct ProjectileProfile
	{
		float range{};
		float radius{};
		float strength{};
		float propagationSpeed{};
		float coneCosine{};
		bool explosive{};
		bool concentration{};
		bool selfDelivery{};
	};

	struct RecentImpact
	{
		std::uintptr_t projectile{};
		RE::NiPoint3 position{};
		std::chrono::steady_clock::time_point time{};
	};

	struct PendingImpact
	{
		RE::NiPoint3 position{};
		RE::NiPoint3 velocity{};
		ProjectileProfile profile{};
	};

	struct PendingCast
	{
		RE::ActorHandle actor;
		RE::FormID actorFormID{};
		RE::FormID spellFormID{};
	};

	RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
		RE::BSTEventSource<RE::TESSpellCastEvent>*) override;

	static void ObserveImpactCallback(void* a_owner, RE::Projectile& a_projectile,
		const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity);
	void ObserveImpact(RE::Projectile& a_projectile, const RE::NiPoint3& a_position,
		const RE::NiPoint3& a_velocity);
	[[nodiscard]] bool AcceptImpactLocked(const RE::Projectile& a_projectile,
		const RE::NiPoint3& a_position);
	[[nodiscard]] static std::optional<ProjectileProfile> ClassifyProjectile(
		const RE::BGSProjectile* a_projectile, float a_area = 0.0f);
	[[nodiscard]] static std::optional<ProjectileProfile> ClassifyMagicEffect(
		const RE::EffectSetting* a_effect, float a_area = 0.0f);
	[[nodiscard]] static std::optional<ProjectileProfile> ClassifyMagicItem(
		const RE::MagicItem* a_magicItem);
	[[nodiscard]] std::optional<ProjectileProfile> FindRuntimeProfile(
		const RE::Projectile& a_projectile) const;
	void EmitCast(const PendingCast& a_cast) const;
	void EmitImpact(const PendingImpact& a_impact);
	void SanitizeSettings();

	Settings settings;
	std::unordered_map<RE::FormID, ProjectileProfile> projectileProfiles;
	std::unordered_map<RE::FormID, ProjectileProfile> magicEffectProfiles;
	std::unordered_map<RE::FormID, ProjectileProfile> magicItemProfiles;
	std::unordered_set<RE::FormID> excludedMagicItems;
	std::unordered_set<RE::FormID> excludedMagicEffects;
	std::unordered_set<RE::FormID> shoutMagicItems;
	std::vector<RecentImpact> recentImpacts;
	std::vector<PendingImpact> pendingImpacts;
	std::vector<PendingCast> pendingCasts;
	std::mutex pendingMutex;
	bool recordsLoaded = false;
};
