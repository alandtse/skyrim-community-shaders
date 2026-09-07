// Unit tests for Util::Subrect::Controller.
//
// Focus: API contract for the stereo extension (PR-1 of the DLSS-PR-PLAN decomposition).
// Cannot exercise DrawEditor (needs an ImGui context); covers everything else:
// JSON load/save round-trips, mirror math, preset apply, mono/stereo back-compat.

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <imgui.h>  // ImDrawCallback declared in Subrect.h signature

#include "Utils/Subrect.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>    // std::abs
#include <utility>  // std::pair
#include <vector>   // std::vector — degenerate-dimensions cases enumerate (w,h) pairs

using Util::Subrect::Controller;
using Util::Subrect::Preset;
using Util::Subrect::UVRegion;

namespace
{
	bool UVApprox(const UVRegion& a, const UVRegion& b, float eps = 1e-5f)
	{
		return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps &&
		       std::abs(a.w - b.w) < eps && std::abs(a.h - b.h) < eps;
	}
}

TEST_CASE("Controller defaults to mono mode", "[subrect]")
{
	Controller c;
	REQUIRE_FALSE(c.IsStereoEnabled());
	// Right-eye accessor folds onto primary UV in mono mode.
	REQUIRE(UVApprox(c.GetUV(), c.GetRightEyeUV()));
}

TEST_CASE("SeedDefaultPresets selects the named first-run preset", "[subrect][defaults]")
{
	Controller c;
	c.LoadSettings(json::object());
	c.SeedDefaultPresets({
							 Preset{ .name = "Full Eye", .uv = { 0.0f, 0.0f, 1.0f, 1.0f } },
							 Preset{ .name = "Center 75%", .uv = { 0.125f, 0.125f, 0.75f, 0.75f } },
						 },
		"Center 75%");
	c.MaterializeNewDefaults();

	REQUIRE(UVApprox(c.GetUV(), { 0.125f, 0.125f, 0.75f, 0.75f }));
	json saved;
	c.SaveSettings(saved);
	REQUIRE(saved["SelectedPresetIndex"] == 1);
}

TEST_CASE("SaveSettings in mono mode emits no right-eye keys", "[subrect][backcompat]")
{
	// Pre-stereo screenshot JSON shape must round-trip bit-identically: this is
	// the core back-compat contract for the existing ScreenshotFeature consumer.
	// EnsureDefaultPreset is lazy (only runs in LoadSettings/ApplyPreset/DrawEditor),
	// so prime it with an empty load to realize the placeholder preset.
	Controller c;
	c.LoadSettings(json::object());
	json out;
	c.SaveSettings(out);

	REQUIRE(out.contains("CropX"));
	REQUIRE(out.contains("CropY"));
	REQUIRE(out.contains("CropW"));
	REQUIRE(out.contains("CropH"));
	REQUIRE_FALSE(out.contains("CropRightX"));
	REQUIRE_FALSE(out.contains("CropRightY"));
	REQUIRE_FALSE(out.contains("CropRightW"));
	REQUIRE_FALSE(out.contains("CropRightH"));

	REQUIRE(out["CropPresets"].is_array());
	REQUIRE(out["CropPresets"].size() >= 1);
	for (const auto& entry : out["CropPresets"]) {
		REQUIRE(entry.contains("uv"));
		REQUIRE_FALSE(entry.contains("right_uv"));
	}
}

TEST_CASE("LoadSettings reads legacy mono JSON unchanged", "[subrect][backcompat]")
{
	// Replicates the screenshot feature's existing on-disk schema.
	json in = {
		{ "CropX", 0.25f },
		{ "CropY", 0.10f },
		{ "CropW", 0.5f },
		{ "CropH", 0.8f },
		{ "CropPresets", json::array({
							 {
								 { "name", "Full Frame" },
								 { "uv", { 0.0f, 0.0f, 1.0f, 1.0f } },
							 },
						 }) },
		{ "SelectedPresetIndex", -1 },
	};

	Controller c;
	c.LoadSettings(in);

	REQUIRE(UVApprox(c.GetUV(), { 0.25f, 0.10f, 0.5f, 0.8f }));
	REQUIRE_FALSE(c.IsStereoEnabled());
}

TEST_CASE("SetStereoEnabled toggles mirror sync", "[subrect][stereo]")
{
	// Stereo enable on a fresh controller mirrors the default UV (which is
	// {0,0,1,1}); the mirror of a full-frame is still full-frame.
	Controller c;
	c.SetStereoEnabled(true);
	REQUIRE(c.IsStereoEnabled());

	// Re-enable is a no-op (no double-sync, no state corruption).
	c.SetStereoEnabled(true);
	REQUIRE(c.IsStereoEnabled());

	c.SetStereoEnabled(false);
	REQUIRE_FALSE(c.IsStereoEnabled());
}

TEST_CASE("Stereo save then load round-trips right-eye keys", "[subrect][stereo]")
{
	// Seed presets with explicit asymmetric right-eye to confirm the on-disk
	// schema preserves it (rather than always mirroring on load).
	Controller src;
	src.SetStereoEnabled(true);
	src.SeedDefaultPresets({
		Preset{ .name = "Asym", .uv = { 0.1f, 0.0f, 0.5f, 1.0f }, .rightUV = UVRegion{ 0.3f, 0.0f, 0.4f, 0.9f } },
	});
	// Realize the seed and copy it into currentUV/currentRightUV.
	src.LoadSettings(json::object());

	json saved;
	src.SaveSettings(saved);

	REQUIRE(saved.contains("CropRightX"));
	REQUIRE(saved["CropPresets"][0].contains("right_uv"));

	Controller dst;
	dst.SetStereoEnabled(true);
	dst.LoadSettings(saved);

	// After load, applying the asymmetric preset must restore both eyes
	// exactly (not mirror-overwrite the right eye).
	REQUIRE(UVApprox(dst.GetUV(), { 0.1f, 0.0f, 0.5f, 1.0f }));
	REQUIRE(UVApprox(dst.GetRightEyeUV(), { 0.3f, 0.0f, 0.4f, 0.9f }));
}

TEST_CASE("Stereo load mirrors right-eye when JSON lacks CropRight keys", "[subrect][stereo][backcompat]")
{
	// A user upgrading from a mono build has CropX/Y/W/H but no CropRight*.
	// In stereo mode, the controller mirrors the primary UV around x=0.5.
	json legacy = {
		{ "CropX", 0.2f },
		{ "CropY", 0.0f },
		{ "CropW", 0.5f },
		{ "CropH", 1.0f },
	};

	Controller c;
	c.SetStereoEnabled(true);
	c.LoadSettings(legacy);

	REQUIRE(UVApprox(c.GetUV(), { 0.2f, 0.0f, 0.5f, 1.0f }));
	// Mirror: x = 1 - 0.2 - 0.5 = 0.3
	REQUIRE(UVApprox(c.GetRightEyeUV(), { 0.3f, 0.0f, 0.5f, 1.0f }));
}

TEST_CASE("GetStereoPixelRegions splits SBS width per eye", "[subrect][stereo]")
{
	// Stereo regions resolve against half-width per eye (the texture is SBS).
	// Caller passes the full stereo texture size; controller divides W by 2.
	Controller c;
	c.SetStereoEnabled(true);

	// Make eyes asymmetric so we can tell them apart.
	c.SeedDefaultPresets({
		Preset{
			.name = "Asym",
			.uv = { 0.0f, 0.0f, 1.0f, 1.0f },               // full left eye
			.rightUV = UVRegion{ 0.0f, 0.0f, 0.5f, 1.0f },  // left half of right eye
		},
	});
	// Realize the seed so currentUV/currentRightUV pick up the preset values.
	c.LoadSettings(json::object());

	const auto regions = c.GetStereoPixelRegions(2000, 1000);
	// Left eye spans the full 1000-wide left half.
	REQUIRE(regions.leftEye.x == 0);
	REQUIRE(regions.leftEye.w == 1000);
	REQUIRE(regions.leftEye.h == 1000);
	// Right eye spans only half the 1000-wide right half = 500 px.
	REQUIRE(regions.rightEye.w == 500);
}

TEST_CASE("GetStereoPixelRegions in mono mode returns identical eyes", "[subrect][stereo]")
{
	// Mono callers can use the stereo accessor and both eyes will resolve from
	// the primary UV — lets DLSS consumers stay agnostic of stereo state.
	Controller c;
	json in = {
		{ "CropX", 0.0f },
		{ "CropY", 0.0f },
		{ "CropW", 1.0f },
		{ "CropH", 1.0f },
	};
	c.LoadSettings(in);

	const auto regions = c.GetStereoPixelRegions(2000, 1000);
	REQUIRE(regions.leftEye.x == regions.rightEye.x);
	REQUIRE(regions.leftEye.w == regions.rightEye.w);
}

TEST_CASE("Stereo SaveSettings emits right_uv for every preset", "[subrect][stereo][regression]")
{
	// The Save Preset button used to drop currentRightUV, so re-applying a
	// saved preset would zero the right eye. Can't drive the ImGui button
	// from a test, so stage the same end state directly and verify it
	// round-trips both eyes through SaveSettings -> LoadSettings.
	Controller src;
	src.SetStereoEnabled(true);

	// Stage a preset with an explicit asymmetric right_uv that doesn't equal
	// MirrorUVHorizontal(uv) — otherwise we couldn't distinguish "right_uv was
	// preserved" from "default fallback mirrored it back".
	const UVRegion leftUV{ 0.10f, 0.0f, 0.40f, 1.0f };
	const UVRegion rightUV{ 0.55f, 0.0f, 0.35f, 1.0f };

	// Build the preset object via direct mutation rather than a single nested
	// initializer list. MSVC's C++23 module-aware parse cannot disambiguate
	// `json::array({ json{ {k,v}, {k,v} } })` from the
	// `initializer_list<initializer_list<json>>` overload of `json`'s
	// constructor, producing a misleading C3329 at the inner closing `)`.
	// Building element-by-element sidesteps the ambiguity entirely.
	json preset = json::object();
	preset["name"] = "Asymmetric";
	preset["uv"] = json::array({ leftUV.x, leftUV.y, leftUV.w, leftUV.h });
	preset["right_uv"] = json::array({ rightUV.x, rightUV.y, rightUV.w, rightUV.h });

	json staged = {
		{ "CropX", leftUV.x },
		{ "CropY", leftUV.y },
		{ "CropW", leftUV.w },
		{ "CropH", leftUV.h },
		{ "CropRightX", rightUV.x },
		{ "CropRightY", rightUV.y },
		{ "CropRightW", rightUV.w },
		{ "CropRightH", rightUV.h },
		{ "CropPresets", json::array({ preset }) },
		{ "SelectedPresetIndex", 0 }
	};
	src.LoadSettings(staged);

	json saved;
	src.SaveSettings(saved);
	REQUIRE(saved["CropPresets"].is_array());
	REQUIRE_FALSE(saved["CropPresets"].empty());
	for (const auto& entry : saved["CropPresets"]) {
		REQUIRE(entry.contains("right_uv"));
	}

	// Round-trip into a fresh controller and confirm the asymmetric right UV
	// survived. Before the fix the saved preset's right_uv would be missing
	// and LoadSettings would mirror left → right, equalizing the eyes.
	Controller dst;
	dst.SetStereoEnabled(true);
	dst.LoadSettings(saved);
	REQUIRE(UVApprox(dst.GetRightEyeUV(), rightUV));
}

TEST_CASE("MirrorUVHorizontal symmetry via SetStereoEnabled", "[subrect][stereo][math]")
{
	// {0.4, *, 0.6, *} mirrors to {0, *, 0.6, *} — the nose-side overlap case.
	// Exercised through SetStereoEnabled since MirrorUVHorizontal is private.
	Controller c;
	json in = {
		{ "CropX", 0.4f },
		{ "CropY", 0.0f },
		{ "CropW", 0.6f },
		{ "CropH", 1.0f },
	};
	c.LoadSettings(in);
	REQUIRE_FALSE(c.IsStereoEnabled());

	c.SetStereoEnabled(true);
	// x = 1 - 0.4 - 0.6 = 0.0
	REQUIRE(UVApprox(c.GetRightEyeUV(), { 0.0f, 0.0f, 0.6f, 1.0f }));
}

TEST_CASE("SetStereoEnabled preserves explicit right UV loaded earlier", "[subrect][stereo][regression]")
{
	// Regression for the call-order trap: LoadSettings (mono) → SetStereoEnabled(true)
	// must NOT overwrite an explicit CropRight* value with the mirror of the left eye.
	Controller c;
	const UVRegion explicitRight{ 0.62f, 0.10f, 0.30f, 0.80f };
	json in = {
		{ "CropX", 0.10f },
		{ "CropY", 0.00f },
		{ "CropW", 0.40f },
		{ "CropH", 1.00f },
		{ "CropRightX", explicitRight.x },
		{ "CropRightY", explicitRight.y },
		{ "CropRightW", explicitRight.w },
		{ "CropRightH", explicitRight.h },
	};
	c.LoadSettings(in);
	REQUIRE_FALSE(c.IsStereoEnabled());

	c.SetStereoEnabled(true);
	REQUIRE(UVApprox(c.GetRightEyeUV(), explicitRight));
}

TEST_CASE("Reload without CropRight* re-enables auto-mirror", "[subrect][stereo][regression]")
{
	// The rightUVLoadedFromJson flag must reset every LoadSettings — otherwise
	// once a config with CropRight* was loaded, later loads without those
	// keys would keep suppressing the mirror.
	Controller c;
	const UVRegion explicitRight{ 0.62f, 0.10f, 0.30f, 0.80f };
	json withRight = {
		{ "CropX", 0.10f }, { "CropY", 0.00f }, { "CropW", 0.40f }, { "CropH", 1.00f },
		{ "CropRightX", explicitRight.x }, { "CropRightY", explicitRight.y },
		{ "CropRightW", explicitRight.w }, { "CropRightH", explicitRight.h }
	};
	c.LoadSettings(withRight);

	// Now load a fresh config WITHOUT CropRight*. The flag should reset to
	// false so a subsequent SetStereoEnabled(true) auto-mirrors.
	json withoutRight = {
		{ "CropX", 0.20f }, { "CropY", 0.00f }, { "CropW", 0.60f }, { "CropH", 1.00f }
	};
	c.LoadSettings(withoutRight);
	c.SetStereoEnabled(true);
	// Mirror of {0.20, 0, 0.60, 1.0} is {1 - 0.20 - 0.60, *, 0.60, *} = {0.20, *, 0.60, *}.
	REQUIRE(UVApprox(c.GetRightEyeUV(), { 0.20f, 0.0f, 0.60f, 1.0f }));
}

TEST_CASE("SaveSettings erases stale CropRight* in mono mode", "[subrect][stereo][regression]")
{
	// When a host re-uses an in-memory JSON object that previously held
	// stereo keys, the mono save must clear them or the next load looks
	// like it still had explicit stereo data.
	json carry = {
		{ "CropRightX", 0.5f }, { "CropRightY", 0.0f },
		{ "CropRightW", 0.5f }, { "CropRightH", 1.0f }
	};
	Controller c;
	REQUIRE_FALSE(c.IsStereoEnabled());
	c.SaveSettings(carry);
	REQUIRE_FALSE(carry.contains("CropRightX"));
	REQUIRE_FALSE(carry.contains("CropRightY"));
	REQUIRE_FALSE(carry.contains("CropRightW"));
	REQUIRE_FALSE(carry.contains("CropRightH"));
}

TEST_CASE("GetStereoPixelRegions returns empty for degenerate dimensions", "[subrect][stereo][edge]")
{
	// fullWidth/2 == 0 for widths 0 or 1; UVToPixelRegion's `width - 1`
	// would underflow into a huge coord without the guard.
	Controller c;
	c.SetStereoEnabled(true);
	for (auto [w, h] : std::vector<std::pair<uint32_t, uint32_t>>{ { 0, 100 }, { 1, 100 }, { 100, 0 } }) {
		const auto regions = c.GetStereoPixelRegions(w, h);
		REQUIRE(regions.leftEye.w == 0);
		REQUIRE(regions.rightEye.w == 0);
	}
}

TEST_CASE("Seeded preset without explicit rightUV auto-mirrors in stereo", "[subrect][stereo][regression]")
{
	// Regression for the silent-full-frame bug: a Preset built with only
	// .name + .uv (rightUV omitted, defaulting to std::nullopt) should
	// auto-mirror the left eye when stereo is enabled, NOT show full frame
	// for the right eye.
	Controller c;
	c.SetStereoEnabled(true);
	c.SeedDefaultPresets({
		Preset{ .name = "Left Half", .uv = { 0.0f, 0.0f, 0.5f, 1.0f } },
	});
	c.LoadSettings(json::object());

	// Mirror of {0, 0, 0.5, 1.0} around x=0.5 is {0.5, 0, 0.5, 1.0}.
	REQUIRE(UVApprox(c.GetRightEyeUV(), { 0.5f, 0.0f, 0.5f, 1.0f }));
}

TEST_CASE("Partial CropRight* keys still allow auto-mirror", "[subrect][stereo][regression]")
{
	// Only one of the four right-eye keys was provided. The old OR semantics
	// would mark this as "explicit right eye" and suppress the mirror on
	// SetStereoEnabled(true), leaving currentRightUV with mixed stale + loaded
	// components. The fixed AND semantics treat partial as not-explicit so
	// the mirror still runs.
	Controller c;
	json partial = {
		{ "CropX", 0.20f }, { "CropY", 0.00f }, { "CropW", 0.60f }, { "CropH", 1.00f },
		{ "CropRightW", 0.50f }  // a single right-eye key — incomplete quartet
	};
	c.LoadSettings(partial);
	c.SetStereoEnabled(true);
	// Mirror of {0.20, 0, 0.60, 1.0} is {0.20, 0, 0.60, 1.0} — confirms the
	// mirror ran rather than landing the half-loaded right UV.
	REQUIRE(UVApprox(c.GetRightEyeUV(), { 0.20f, 0.0f, 0.60f, 1.0f }));
}

TEST_CASE("ApplyPresetByName resolves a seeded default despite a non-empty persisted list", "[subrect][regression]")
{
	// EnsureDefaultPreset only seeds when `presets` starts empty; a user with
	// any persisted preset must still reach a later-added default by name.
	Controller c;
	json in = {
		{ "CropPresets", json::array({
							 {
								 { "name", "Leftover" },
								 { "uv", { 0.0f, 0.0f, 1.0f, 1.0f } },
							 },
						 }) },
		{ "SelectedPresetIndex", 0 },
	};
	c.LoadSettings(in);

	c.SeedDefaultPresets({
		Preset{ .name = "Center 75%", .uv = { 0.125f, 0.125f, 0.75f, 0.75f } },
	});

	REQUIRE(c.ApplyPresetByName("Center 75%"));
	REQUIRE(UVApprox(c.GetUV(), { 0.125f, 0.125f, 0.75f, 0.75f }));

	// A second, unrelated name that was never seeded must still fail cleanly.
	REQUIRE_FALSE(c.ApplyPresetByName("Nonexistent Preset"));
}

TEST_CASE("Malformed preset right_uv falls back to auto-mirror", "[subrect][stereo][regression]")
{
	// LoadUVArray returns a default full-frame UV on malformed input. Without
	// shape validation, a bad `right_uv` payload would land the right eye as
	// full-frame AND suppress the auto-mirror — the worst of both worlds.
	// With validation, malformed input is ignored and the mirror takes over.
	Controller c;
	c.SetStereoEnabled(true);

	// Same C++23-modules-friendly construction as above (see the regression
	// test for the rationale).
	json badPreset = json::object();
	badPreset["name"] = "Bad";
	badPreset["uv"] = json::array({ 0.10f, 0.0f, 0.40f, 1.0f });
	badPreset["right_uv"] = "not an array";  // malformed

	json bad = {
		{ "CropPresets", json::array({ badPreset }) },
		{ "SelectedPresetIndex", 0 }
	};
	c.LoadSettings(bad);
	// Auto-mirror of {0.10, 0, 0.40, 1.0} = {0.50, 0, 0.40, 1.0}, NOT {0,0,1,1}.
	REQUIRE(UVApprox(c.GetRightEyeUV(), { 0.50f, 0.0f, 0.40f, 1.0f }));
}
