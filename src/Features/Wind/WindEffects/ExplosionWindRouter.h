#pragma once

#include "WindEffect.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/** Routes record-backed magic explosions into radial transient wind sources. */
class ExplosionWindRouter final :
	public WindEffect,
	public RE::BSTEventSink<RE::TESHitEvent>,
	public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>,
	public RE::BSTEventSink<RE::TESSpellCastEvent>
{
public:
	/** User tuning applied after explosion-record classification. */
	struct Settings
	{
		bool enabled = true;
		float intensity = 1.0f;
		float radiusMultiplier = 1.0f;
		float decayTime = 0.9f;
	};

	/** Registers this router with the shared projectile impact dispatcher. */
	ExplosionWindRouter();

	/** Unregisters this router from the shared projectile impact dispatcher. */
	~ExplosionWindRouter() override;

	/** @copydoc WindEffect::GetId */
	[[nodiscard]] std::string_view GetId() const override { return "explosions"; }

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

	/** Routes a known explosion record at an externally observed world-impact position. */
	void RouteExplosionAt(const RE::BGSExplosion* a_explosion, const RE::NiPoint3& a_position,
		const RE::TESObjectREFR* a_cause = nullptr);

private:
	struct ExplosionProfile
	{
		RE::FormID identity{};
		float radius{};
		float strength{};
	};

	struct RecentExplosion
	{
		RE::FormID identity{};
		RE::FormID cause{};
		RE::NiPoint3 position{};
		float radius{};
		std::chrono::steady_clock::time_point time{};
	};

	struct ExplosionObservation
	{
		ExplosionProfile profile{};
		RE::NiPoint3 position{};
		float3 direction{};
		RE::FormID cause{};
	};

	RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event,
		RE::BSTEventSource<RE::TESHitEvent>*) override;
	RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event,
		RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override;
	RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
		RE::BSTEventSource<RE::TESSpellCastEvent>*) override;
	static void ObserveImpactCallback(void* a_owner, RE::Projectile& a_projectile,
		const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity);
	void ObserveImpact(RE::Projectile& a_projectile, const RE::NiPoint3& a_position);

	[[nodiscard]] static std::optional<ExplosionProfile> ClassifyExplosion(const RE::BGSExplosion* a_explosion);
	[[nodiscard]] static std::optional<ExplosionProfile> ClassifyProjectile(const RE::BGSProjectile* a_projectile);
	[[nodiscard]] static std::optional<ExplosionProfile> ClassifyMagicEffect(const RE::EffectSetting* a_effect);
	[[nodiscard]] static std::optional<ExplosionProfile> ClassifyMagicItem(const RE::MagicItem* a_magicItem);
	[[nodiscard]] static ExplosionProfile CreateProfile(RE::FormID a_identity, float a_radius, float a_force);
	[[nodiscard]] std::optional<ExplosionProfile> FindProjectileProfile(RE::FormID a_formID) const;
	[[nodiscard]] std::optional<ExplosionProfile> FindMagicEffectProfile(RE::FormID a_formID) const;
	[[nodiscard]] std::optional<ExplosionProfile> FindSourceProfile(RE::FormID a_formID) const;
	[[nodiscard]] bool AcceptExplosion(const ExplosionProfile& a_profile, const RE::NiPoint3& a_position,
		RE::FormID a_cause);
	void QueueObservation(const ExplosionProfile& a_profile, const RE::NiPoint3& a_position,
		const RE::TESObjectREFR* a_cause);
	void EmitExplosion(const ExplosionObservation& a_observation);
	void SanitizeSettings();

	Settings settings;
	std::unordered_map<RE::FormID, ExplosionProfile> projectileProfiles;
	std::unordered_map<RE::FormID, ExplosionProfile> magicEffectProfiles;
	std::unordered_map<RE::FormID, ExplosionProfile> sourceProfiles;
	std::unordered_set<RE::FormID> reservedMagicEffects;
	std::unordered_set<RE::FormID> reservedCastForms;
	std::deque<ExplosionObservation> pendingObservations;
	mutable std::mutex pendingObservationsMutex;
	std::vector<RecentExplosion> recentExplosions;
	bool registered = false;
};
