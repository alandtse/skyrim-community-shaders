#pragma once

#include "WindEffect.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

/** Adds optional wind for projectile records owned by Weapon Throw VR. */
class WeaponThrowVRWind final : public WindEffect
{
public:
	/** User tuning for thrown-weapon flight and impact wind. */
	struct Settings
	{
		bool enabled = true;
		float strength = 1.0f;
		float minimumSpeed = 150.0f;
		float fullStrengthSpeed = 1800.0f;
		float radius = 220.0f;
		float bowLength = 320.0f;
		float wakeLength = 900.0f;
		float launchStrength = 0.65f;
		float impactStrength = 0.8f;
		float decayTime = 0.6f;
	};

	/** Unregisters optional projectile callbacks when they were installed. */
	~WeaponThrowVRWind() override;

	/** @copydoc WindEffect::GetId */
	[[nodiscard]] std::string_view GetId() const override { return "weaponThrowVR"; }

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
	struct MotionObservation
	{
		RE::NiPoint3 position{};
		RE::NiPoint3 velocity{};
	};

	struct ImpactObservation
	{
		RE::NiPoint3 position{};
		RE::NiPoint3 velocity{};
	};

	static void ObserveMotionCallback(void* a_owner, RE::Projectile& a_projectile, float a_deltaTime);
	static void ObserveImpactCallback(void* a_owner, RE::Projectile& a_projectile,
		const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity);
	void ObserveMotion(RE::Projectile& a_projectile);
	void ObserveImpact(RE::Projectile& a_projectile, const RE::NiPoint3& a_position,
		const RE::NiPoint3& a_velocity);
	[[nodiscard]] bool IsWeaponThrowProjectile(const RE::Projectile& a_projectile) const;
	void SanitizeSettings();

	Settings settings;
	std::unordered_set<RE::FormID> projectileForms;
	std::unordered_map<std::uintptr_t, MotionObservation> pendingMotion;
	std::unordered_map<std::uintptr_t, ImpactObservation> pendingImpacts;
	std::unordered_map<std::uintptr_t, float> projectileAges;
	std::unordered_map<std::uintptr_t, float> impactAges;
	std::mutex pendingMutex;
	bool pluginDetected = false;
	bool subscribed = false;
	bool recordsLoaded = false;
};
