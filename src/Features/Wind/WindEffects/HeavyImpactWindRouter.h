#pragma once

#include "WindEffect.h"

#include <memory>

namespace RE
{
	class Actor;
}

/**
 * Routes large physical actor events into bounded one-shot transient wind sources.
 * Grounded dragon hits require wing or tail identity in current attack or weapon metadata.
 */
class HeavyImpactWindRouter final : public WindEffect
{
public:
	/** Identifies a supported impact so additional reliable event producers can share the router. */
	enum class ImpactKind
	{
		PowerAttack,
		GiantClubSmash,
		DragonGroundAttack,
		TrollPowerAttack,
		MammothHeavyAttack,
		CenturionHeavyAttack,
		BeastTransformation,
		ShieldCharge
	};

	/** Parameters for a traveling radial shockwave. */
	struct RadialProfile
	{
		float strength;
		float distance;
		float waveHalfWidth;
		float propagationSpeed;
		float decayTime;
	};

	/** Parameters for a traveling directional pressure wave. */
	struct DirectionalProfile
	{
		float strength;
		float distance;
		float waveHalfWidth;
		float propagationSpeed;
		float coneHalfAngle;
		float decayTime;
	};

	/** Tuning shared by event classification and each emitted source class. */
	struct Settings
	{
		bool enabled = true;
		float strength = 1.0f;
		float trackingDistance = 12000.0f;
		float deduplicationTime = 0.35f;
		bool powerAttacks = true;
		DirectionalProfile powerAttack{ 0.9f, 1000.0f, 200.0f, 2800.0f, 40.0f, 0.65f };
		RadialProfile giantClub{ 2.5f, 2200.0f, 360.0f, 2200.0f, 1.4f };
		RadialProfile dragonGroundAttack{ 2.2f, 2000.0f, 340.0f, 2200.0f, 1.2f };
		RadialProfile trollPowerAttack{ 1.4f, 1300.0f, 260.0f, 1800.0f, 1.0f };
		RadialProfile mammothHeavyAttack{ 2.0f, 1900.0f, 320.0f, 2000.0f, 1.2f };
		RadialProfile centurionHeavyAttack{ 2.0f, 1800.0f, 300.0f, 2100.0f, 1.1f };
		RadialProfile beastTransformation{ 1.8f, 1700.0f, 320.0f, 1900.0f, 1.3f };
		DirectionalProfile shieldCharge{ 1.3f, 1100.0f, 220.0f, 2800.0f, 45.0f, 0.7f };
	};

	HeavyImpactWindRouter();
	~HeavyImpactWindRouter();
	HeavyImpactWindRouter(const HeavyImpactWindRouter&) = delete;
	HeavyImpactWindRouter& operator=(const HeavyImpactWindRouter&) = delete;

	/** @copydoc WindEffect::GetId */
	[[nodiscard]] std::string_view GetId() const override { return "heavyImpacts"; }

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

	/** Queues a pre-classified impact from another reliable event producer. */
	void QueueImpact(RE::Actor& a_actor, ImpactKind a_kind);

private:
	void SanitizeSettings();

	class Impl;
	std::unique_ptr<Impl> implementation;
	Settings settings;
};
