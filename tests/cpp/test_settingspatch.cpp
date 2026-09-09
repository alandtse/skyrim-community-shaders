// Unit tests for the settings-patch key-validation helper
// (src/Utils/SettingsKeys.cpp). Pure nlohmann::json; no game/D3D/Feature
// dependency (ApplyPatch itself needs a live Feature and isn't unit-testable
// here -- see DevBenchBridge.cpp / VRAPI::CSpluginapi.cpp for its callers).

#include "Utils/SettingsPatch.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Export selection retains only selected complete setting paths", "[settingspatch]")
{
	const json values{ { "Group", { { "Pick", 2 }, { "Skip", false } } }, { "Vector", { 1, 2, 3 } },
		{ "0", { { "1", 4 } } }, { "a/b", { { "~key", true } } }, { "_metadata", { { "description", "skip" } } } };
	REQUIRE(Util::Settings::SelectSettingPaths(values, { "/Group/Pick", "/Vector" }) ==
			json{ { "Group", { { "Pick", 2 } } }, { "Vector", { 1, 2, 3 } } });
	REQUIRE(Util::Settings::SelectSettingPaths(values, { "/0/1", "/a~1b/~0key" }) ==
			json{ { "0", { { "1", 4 } } }, { "a/b", { { "~key", true } } } });
	REQUIRE(Util::Settings::SelectSettingPaths(values, { "/Vector/0", "/Group", "/_metadata/description" }).empty());
	REQUIRE(Util::Settings::SelectSettingPaths(values, {}).empty());
}

TEST_CASE("Restoring overwrite baselines retains unrelated user changes", "[settingspatch]")
{
	json values{ { "Group", { { "Owned", 4.0 }, { "Unowned", 6.0 }, { "Extra", true } } } };
	const json baseline{ { "Group", { { "Owned", 1.0 }, { "Unowned", 2.0 } } } };
	const json mask{ { "Group", { { "Owned", 4.0 }, { "Extra", true } } } };
	Util::Settings::RestoreSettings(values, baseline, mask);
	REQUIRE(values == json{ { "Group", { { "Owned", 1.0 }, { "Unowned", 6.0 } } } });
}

TEST_CASE("Selecting legacy values preserves only currently owned keys", "[settingspatch]")
{
	const json legacy{ { "Group", { { "Owned", 4.0 }, { "Orphan", 6.0 } } }, { "Other", true } };
	const json mask{ { "Group", { { "Owned", 1.0 } } } };
	REQUIRE(Util::Settings::SelectSettings(legacy, mask) == json{ { "Group", { { "Owned", 4.0 } } } });
}

TEST_CASE("BuildUserOverride ignores values outside the mod override", "[settingspatch]")
{
	const json current{
		{ "Disable at Boot", { { "ScreenSpaceGI", true }, { "Skylighting", true } } }
	};
	const json overridden{
		{ "Disable at Boot", { { "ScreenSpaceGI", true } } }
	};

	REQUIRE(Util::Settings::BuildUserOverride(current, overridden).empty());
}

TEST_CASE("BuildUserOverride saves only changed nested override values", "[settingspatch]")
{
	const json current{
		{ "Disable at Boot", { { "ScreenSpaceGI", false }, { "Skylighting", true } } },
		{ "General", { { "Enable Async", true } } }
	};
	const json overridden{
		{ "Disable at Boot", { { "ScreenSpaceGI", true } } }
	};
	const json expected{
		{ "Disable at Boot", { { "ScreenSpaceGI", false } } }
	};

	REQUIRE(Util::Settings::BuildUserOverride(current, overridden) == expected);
}

TEST_CASE("BuildUserOverride tolerates floating-point serialization noise", "[settingspatch]")
{
	const json current{ { "Strength", 1.000001 } };
	const json overridden{ { "Strength", 1.0 } };

	REQUIRE(Util::Settings::BuildUserOverride(current, overridden).empty());
}

TEST_CASE("CollectUnknownSettingKeys accepts an all-known flat patch", "[settingspatch]")
{
	const json known{ { "Enabled", true }, { "Quality", 1 } };
	const json patch{ { "Enabled", false } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown.empty());
}

TEST_CASE("CollectUnknownSettingKeys reports an unknown flat key", "[settingspatch]")
{
	const json known{ { "Enabled", true } };
	const json patch{ { "Enabeld", false } };  // typo
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown == std::vector<std::string>{ "Enabeld" });
}

TEST_CASE("CollectUnknownSettingKeys accepts a valid nested patch", "[settingspatch]")
{
	const json known{ { "ShadowSettings", { { "Distance", 1.0f }, { "Resolution", 512 } } } };
	const json patch{ { "ShadowSettings", { { "Distance", 2.0f } } } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown.empty());
}

TEST_CASE("CollectUnknownSettingKeys reports a mis-nested key as a dotted path", "[settingspatch]")
{
	const json known{ { "ShadowSettings", { { "Distance", 1.0f } } } };
	const json patch{ { "ShadowSettings", { { "Distnace", 2.0f } } } };  // typo, still nested
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown == std::vector<std::string>{ "ShadowSettings.Distnace" });
}

TEST_CASE("CollectUnknownSettingKeys reports a flattened key that should be nested", "[settingspatch]")
{
	// A caller that flattens {"Distance": 2.0} instead of nesting it under
	// ShadowSettings must be rejected, not silently dropped.
	const json known{ { "ShadowSettings", { { "Distance", 1.0f } } } };
	const json patch{ { "Distance", 2.0f } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown == std::vector<std::string>{ "Distance" });
}

TEST_CASE("CollectUnknownSettingKeys reports every key when known is not an object", "[settingspatch]")
{
	// A feature with no SaveSettings override reports null here. It's ApplyPatch's
	// own is_object() guard (untested here -- needs a live Feature) that skips this
	// check in that case rather than rejecting the patch outright.
	const json known = nullptr;
	const json patch{ { "Enabled", true } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown == std::vector<std::string>{ "Enabled" });
}

TEST_CASE("CollectUnknownSettingKeys on a non-object incoming patch reports nothing", "[settingspatch]")
{
	const json known{ { "Enabled", true } };
	const json patch = 5;
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown.empty());
}

TEST_CASE("CollectUnknownSettingKeys reports nesting under a known scalar field", "[settingspatch]")
{
	// Inverse of the flattened-key case: a caller that nests under a field
	// the feature defines as a scalar, not a group.
	const json known{ { "A", 1 } };
	const json patch{ { "A", { { "B", 2 } } } };
	std::vector<std::string> unknown;
	Util::Settings::CollectUnknownSettingKeys(patch, known, "", unknown);
	REQUIRE(unknown == std::vector<std::string>{ "A.B" });
}
