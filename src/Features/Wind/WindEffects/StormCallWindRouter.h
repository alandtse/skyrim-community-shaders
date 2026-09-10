#pragma once

#include "WindEffect.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

/** Routes exact Storm Call lightning impacts into radial transient wind sources. */
class StormCallWindRouter final : public WindEffect
{
public:
	/** User tuning for the lightning-strike pressure wave. */
	struct Settings
	{
		bool enabled = true;
		float strength = 1.0f;
		float radius = 900.0f;
		float decayTime = 0.8f;
	};

	/** Registers this router with the shared projectile impact dispatcher. */
	StormCallWindRouter();

	/** Unregisters this router from the shared projectile impact dispatcher. */
	~StormCallWindRouter() override;

	/** @copydoc WindEffect::GetId */
	[[nodiscard]] std::string_view GetId() const override { return "stormCall"; }

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
		float strength{};
	};

	static void ObserveImpactCallback(void* a_owner, RE::Projectile& a_projectile,
		const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity);
	void ObserveImpact(RE::Projectile& a_projectile, const RE::NiPoint3& a_position,
		const RE::NiPoint3& a_velocity);
	[[nodiscard]] bool AcceptImpactLocked(const RE::Projectile& a_projectile,
		const RE::NiPoint3& a_position);
	void SanitizeSettings();

	Settings settings;
	std::unordered_map<RE::FormID, float> boltStrengthBySpell;
	std::unordered_map<RE::FormID, float> boltStrengthByMagicEffect;
	std::vector<RecentImpact> recentImpacts;
	std::vector<PendingImpact> pendingImpacts;
	std::mutex pendingMutex;
	bool recordsLoaded = false;
};
