#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

struct Feature;

namespace Util::Settings
{
	/**
	 * @brief Builds the minimal user layer for values controlled by an override.
	 * @param a_current Current effective settings.
	 * @param a_override Mod-provided override settings.
	 * @return Changed values shaped like a_override; empty when they match.
	 */
	json BuildUserOverride(const json& a_current, const json& a_override);

	/** @brief Selects values present in both a_values and a_mask, retaining their nested structure. */
	json SelectSettings(const json& a_values, const json& a_mask);
	/** @brief Replaces masked values from a_source, removing masked keys absent from that source. */
	void RestoreSettings(json& a_target, const json& a_source, const json& a_mask);
	/** @brief Selects complete JSON leaves by pointer, preserving object keys and indivisible arrays. */
	json SelectSettingPaths(const json& values, const std::vector<std::string>& paths);

	// Collects keys of a_incoming that a_known has no counterpart for, as dotted
	// paths, recursing into nested groups. The settings serializers drop keys they
	// don't recognize, so a mis-nested or misspelled key would otherwise apply
	// nothing while still reporting success.
	void CollectUnknownSettingKeys(const json& a_incoming, const json& a_known,
		const std::string& a_prefix, std::vector<std::string>& a_out);

	/**
	 * @brief Merges a partial settings blob onto a feature's live settings and applies it.
	 * @param a_feature Feature whose settings are being patched.
	 * @param a_patch Partial settings object; unrecognized keys are rejected (see o_unknownKeys)
	 *        rather than silently dropped.
	 * @param o_unknownKeys Filled with dotted-path keys a_patch names that the feature's
	 *        SaveSettings blob doesn't, when this returns false.
	 * @return False (nothing applied) if a_patch names any unrecognized key; true otherwise.
	 * @note Main/render thread only -- calls the feature's SaveSettings/LoadSettings directly.
	 */
	bool ApplyPatch(Feature& a_feature, const json& a_patch, std::vector<std::string>& o_unknownKeys);
}
