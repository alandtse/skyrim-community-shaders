#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

/** Represents one independently configured source of transient wind. */
class WindEffect
{
public:
	virtual ~WindEffect() = default;

	/** Returns the stable key used for this effect in Wind settings JSON. */
	[[nodiscard]] virtual std::string_view GetId() const = 0;

	/** Returns the localized name shown in the Wind Effects UI. */
	[[nodiscard]] virtual std::string GetDisplayName() const = 0;

	/** Draws this effect's settings controls. */
	virtual void DrawSettings() = 0;

	/** Loads this effect from its settings object. */
	virtual void LoadSettings(const nlohmann::json& a_json) = 0;

	/** Saves this effect into its settings object. */
	virtual void SaveSettings(nlohmann::json& a_json) const = 0;

	/** Restores this effect's settings defaults. */
	virtual void RestoreDefaultSettings() = 0;

	/** Handles Skyrim data becoming available. */
	virtual void DataLoaded() {}

	/** Advances runtime-owned wind sources for one rendered frame. */
	virtual void Update(float) {}

	/** Clears runtime-owned sources across scene transitions or disabling. */
	virtual void Reset() {}
};
