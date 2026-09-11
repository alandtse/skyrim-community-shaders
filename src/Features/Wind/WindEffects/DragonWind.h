#pragma once

#include "WindEffect.h"

/** Adds dragon wingbeat, landing, crash, and shout forces to shared wind. */
class DragonWind final : public WindEffect
{
public:
	/** Tunable parameters for one dragon landing shockwave class. */
	struct ImpactProfile
	{
		float strength = 1.0f;
		float distance = 1400.0f;
		float waveHalfWidth = 240.0f;
		float propagationSpeed = 1800.0f;
	};

	/** Advanced tuning for all dragon-owned visual wind sources. */
	struct Settings
	{
		bool enabled = true;
		bool wingbeatsEnabled = true;
		bool landingsEnabled = true;
		bool crashesEnabled = true;
		bool shoutsEnabled = true;
		float strength = 1.0f;
		float trackingDistance = 12000.0f;
		float wingbeatStrength = 0.73f;
		float wingbeatDistance = 2392.0f;
		float wingbeatWaveHalfWidth = 238.0f;
		float wingbeatPropagationSpeed = 2200.0f;
		float wingbeatDecayTime = 1.12f;
		float wingbeatFallbackCooldown = 0.10f;
		ImpactProfile normalImpact{ 1.5f, 1400.0f, 240.0f, 1800.0f };
		ImpactProfile forcefulImpact{ 1.8f, 2200.0f, 360.0f, 2200.0f };
		ImpactProfile crashImpact{ 3.0f, 3200.0f, 500.0f, 2600.0f };
		float impactDecayTime = 2.0f;
		float impactDeduplicationTime = 0.3f;
	};

	/** @copydoc WindEffect::GetId */
	[[nodiscard]] std::string_view GetId() const override { return "dragon"; }
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
	void SanitizeSettings();

	Settings settings;
};
