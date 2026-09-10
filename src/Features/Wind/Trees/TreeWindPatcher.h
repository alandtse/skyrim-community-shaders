#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RE
{
	class BSGeometry;
}

namespace TreeWindPatcher
{
	struct Sensitivities
	{
		float bend = 1.0f;
		float leafAmbient = 1.0f;
		float upperBendRange = 100.0f;
		float maximumDisplacementPercent = 3.0f;
		float trunkGustInfluence = 0.5f;
		float leafGustInfluence = 0.99f;
		float transientWindInfluence = 2.01f;
		float leafTransientWindInfluence = 5.0f;
		float leafTransientFlutterMaximum = 20.0f;
		float transientMaximumBendMultiplier = 2.5f;
		float boundMinimumZ = 0.0f;
		float boundHeight = 0.0f;
		float3 probeBase{};
		float3 probeTop{};
		bool hasBounds = false;
	};

	struct RuleSnapshot
	{
		std::uint32_t id = 0;
		std::string_view mesh;
		float bend = 1.0f;
		float leafAmbient = 1.0f;
		float upperBendRange = 100.0f;
		float maximumDisplacementPercent = 3.0f;
		float trunkGustInfluence = 0.5f;
		float leafGustInfluence = 0.99f;
		float transientWindInfluence = 2.01f;
		float leafTransientWindInfluence = 5.0f;
		float leafTransientFlutterMaximum = 20.0f;
		float transientMaximumBendMultiplier = 2.5f;
		bool unsaved = false;
	};

	struct SaveResult
	{
		bool success = false;
		std::size_t savedRuleCount = 0;
		std::string path;
		std::string error;
	};

	/** @brief Loads tree wind patch files and installs model metadata during NIF creation. */
	void LoadAndInstall();

	/** @brief Reads live sensitivities and startup-cached model bounds for a tree geometry. */
	[[nodiscard]] Sensitivities GetSensitivities(const RE::BSGeometry* a_geometry);

	/** @return Number of mesh rules currently available for live tuning. */
	[[nodiscard]] std::size_t GetRuleCount();

	/** @return A stable snapshot for the requested zero-based rule index. */
	[[nodiscard]] RuleSnapshot GetRule(std::size_t a_index);

	/** @brief Applies live sensitivity values to a rule. */
	[[nodiscard]] bool SetRule(std::size_t a_index, float a_bend, float a_leafAmbient,
		float a_upperBendRange, float a_maximumDisplacementPercent,
		float a_trunkGustInfluence, float a_leafGustInfluence,
		float a_transientWindInfluence, float a_leafTransientWindInfluence,
		float a_leafTransientFlutterMaximum,
		float a_transientMaximumBendMultiplier);

	/** @brief Applies live sensitivity values to an exact normalized mesh path. */
	[[nodiscard]] bool SetRule(std::string_view a_mesh, float a_bend, float a_leafAmbient,
		float a_upperBendRange, float a_maximumDisplacementPercent,
		float a_trunkGustInfluence, float a_leafGustInfluence,
		float a_transientWindInfluence, float a_leafTransientWindInfluence,
		float a_leafTransientFlutterMaximum,
		float a_transientMaximumBendMultiplier);

	/** @brief Enables or disables one runtime-only response applied to every tree. */
	void SetUniversalOverride(bool a_enabled, const Sensitivities& a_values);

	/** @return The current runtime-only universal response and its enabled state. */
	[[nodiscard]] std::pair<bool, Sensitivities> GetUniversalOverride();

	/** @brief Restores all live values to their last saved values. */
	void RevertUnsavedChanges();

	/** @brief Writes changed values back to each rule's deterministic source JSON. */
	[[nodiscard]] SaveResult SaveRules();

	/** @return Number of rules changed since the editable JSON was loaded or saved. */
	[[nodiscard]] std::size_t GetUnsavedRuleCount();

	/** @return Deterministically scanned JSON file names that define overlapping tree parameters. */
	[[nodiscard]] std::vector<std::string> GetConflictingFiles();
}
