#include "Wind.h"

#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindEffects/DragonWind.h"
#include "Features/Wind/WindEffects/ExplosionWindRouter.h"
#include "Features/Wind/WindEffects/FusRoDahWind.h"
#include "Features/Wind/WindEffects/HeavyImpactWindRouter.h"
#include "Features/Wind/WindEffects/ProjectileMagicWindRouter.h"
#include "Features/Wind/WindEffects/SpellShoutWindRouter.h"
#include "Features/Wind/WindEffects/StormCallWindRouter.h"
#include "Features/Wind/WindEffects/WeaponThrowVRWind.h"
#include "Features/Wind/WindMath.h"
#include "Globals.h"
#include "State.h"
#include "Trees/TreeWindPatcher.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

using namespace WindSettingsLimits;
using WindMath::ClampFiniteOrDefault;

namespace
{
	constexpr uint32_t kNearTransientFieldMask = 1u << 0u;
	constexpr uint32_t kMidTransientFieldMask = 1u << 1u;
	constexpr uint32_t kFarTransientFieldMask = 1u << 2u;
}

Wind::Wind()
{
	windEffects.emplace_back(std::make_unique<FusRoDahWind>());
	windEffects.emplace_back(std::make_unique<DragonWind>());
	windEffects.emplace_back(std::make_unique<SpellShoutWindRouter>());
	windEffects.emplace_back(std::make_unique<ProjectileMagicWindRouter>());
	windEffects.emplace_back(std::make_unique<ExplosionWindRouter>());
	windEffects.emplace_back(std::make_unique<StormCallWindRouter>());
	windEffects.emplace_back(std::make_unique<WeaponThrowVRWind>());
	windEffects.emplace_back(std::make_unique<HeavyImpactWindRouter>());
}

void Wind::SanitizeSettings(Settings& a_settings)
{
	const Settings defaults{};
	a_settings.trunkWindBendSensitivity = ClampFiniteOrDefault(a_settings.trunkWindBendSensitivity,
		kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, defaults.trunkWindBendSensitivity);
	a_settings.treeLeafBaseWindFlutterGain = ClampFiniteOrDefault(a_settings.treeLeafBaseWindFlutterGain,
		kTreeLeafBaseWindFlutterGainMin, kTreeLeafBaseWindFlutterGainMax, defaults.treeLeafBaseWindFlutterGain);
	a_settings.treeWindGustScale = ClampFiniteOrDefault(a_settings.treeWindGustScale,
		kTreeWindGustScaleMin, kTreeWindGustScaleMax, defaults.treeWindGustScale);
	a_settings.treeWindGustSoftLimit = ClampFiniteOrDefault(a_settings.treeWindGustSoftLimit,
		kTreeWindGustSoftLimitMin, kTreeWindGustSoftLimitMax, defaults.treeWindGustSoftLimit);
	a_settings.treeWindSpringFrequency = ClampFiniteOrDefault(a_settings.treeWindSpringFrequency,
		kTreeWindSpringFrequencyMin, kTreeWindSpringFrequencyMax, defaults.treeWindSpringFrequency);
	a_settings.treeWindSpringDamping = ClampFiniteOrDefault(a_settings.treeWindSpringDamping,
		kTreeWindSpringDampingMin, kTreeWindSpringDampingMax, defaults.treeWindSpringDamping);
	a_settings.treeTransientSpringFrequency = ClampFiniteOrDefault(a_settings.treeTransientSpringFrequency,
		kTreeTransientSpringFrequencyMin, kTreeTransientSpringFrequencyMax, defaults.treeTransientSpringFrequency);
	a_settings.treeTransientSpringDamping = ClampFiniteOrDefault(a_settings.treeTransientSpringDamping,
		kTreeTransientSpringDampingMin, kTreeTransientSpringDampingMax, defaults.treeTransientSpringDamping);
	a_settings.windFieldGustScale = ClampFiniteOrDefault(a_settings.windFieldGustScale,
		kWindFieldGustScaleMin, kWindFieldGustScaleMax, defaults.windFieldGustScale);
	a_settings.windFieldGustCrosswindScale = ClampFiniteOrDefault(a_settings.windFieldGustCrosswindScale,
		kWindFieldGustCrosswindScaleMin, kWindFieldGustCrosswindScaleMax, defaults.windFieldGustCrosswindScale);
	a_settings.windFieldGustAmplitude = ClampFiniteOrDefault(a_settings.windFieldGustAmplitude,
		kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, defaults.windFieldGustAmplitude);
	a_settings.windFieldGustAdvectionMultiplier = ClampFiniteOrDefault(a_settings.windFieldGustAdvectionMultiplier,
		kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax, defaults.windFieldGustAdvectionMultiplier);
	a_settings.windFieldDirectionTransitionDuration = ClampFiniteOrDefault(a_settings.windFieldDirectionTransitionDuration,
		kWindFieldDirectionTransitionDurationMin, kWindFieldDirectionTransitionDurationMax,
		defaults.windFieldDirectionTransitionDuration);
	SanitizeGrassWindSettings(a_settings);
}

uint32_t Wind::GetTransientFieldMask() const
{
	return kNearTransientFieldMask |
	       (settings.processMidRangeTransients ? kMidTransientFieldMask : 0u) |
	       (settings.processFarRangeTransients ? kFarTransientFieldMask : 0u);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Wind::Settings::GrassWindSpringQualityRange,
	textureSize,
	maxDistance)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Wind::Settings,
	enableTrunkBend,
	overrideTrunkWindIntensity,
	trunkWindIntensityOverride,
	trunkWindBendSensitivity,
	treeLeafBaseWindFlutterGain,
	treeWindGustScale,
	treeWindGustSoftLimit,
	treeWindSpringFrequency,
	treeWindSpringDamping,
	treeTransientSpringFrequency,
	treeTransientSpringDamping,
	windFieldGustScale,
	windFieldGustCrosswindScale,
	windFieldGustAmplitude,
	windFieldGustAdvectionMultiplier,
	windFieldDirectionTransitionDuration,
	processMidRangeTransients,
	processFarRangeTransients,
	enableAmbientGrassWind,
	grassWindResponse,
	grassWindSensitivity,
	grassWindMaximumTilt,
	grassWindBendProfile,
	grassWindCompressionToBend,
	grassWindSpringFrequency,
	grassWindSpringDamping,
	grassWindSpringQuality,
	grassWindFlutterStrength,
	grassWindFlutterFrequency)

void Wind::SetTreeWindTestEnabled(bool a_enabled)
{
	if (runtimeState.treeWindTest.enabled == a_enabled)
		return;

	if (a_enabled) {
		const auto* state = globals::state;
		runtimeState.treeWindTest.speed = ClampFiniteOrDefault(state ? state->windFieldSelectedSpeed : runtimeState.windFieldOverrideSpeed, 0.0f, 2.0f, 1.0f);
		runtimeState.treeWindTest.gustScale = ClampFiniteOrDefault(state ? state->windFieldTuning.gustScale : settings.windFieldGustScale,
			kWindFieldGustScaleMin, kWindFieldGustScaleMax, settings.windFieldGustScale);
		runtimeState.treeWindTest.gustAmplitude = ClampFiniteOrDefault(state ? state->windFieldTuning.gustAmplitude : settings.windFieldGustAmplitude,
			kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, settings.windFieldGustAmplitude);
		runtimeState.treeWindTest.gustAdvectionMultiplier = ClampFiniteOrDefault(
			state ? state->windFieldTuning.gustAdvectionMultiplier : settings.windFieldGustAdvectionMultiplier,
			kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax, settings.windFieldGustAdvectionMultiplier);
	}
	runtimeState.treeWindTest.enabled = a_enabled;
}

json Wind::GetDiagnostics()
{
	const auto [universalOverrideEnabled, universalValues] = TreeWindPatcher::GetUniversalOverride();
	return json{
		{ "treeWindRuleCount", TreeWindPatcher::GetRuleCount() },
		{ "treeWindUnsavedRuleCount", TreeWindPatcher::GetUnsavedRuleCount() },
		{ "treeWindConflictingFiles", TreeWindPatcher::GetConflictingFiles() },
		{ "treeWindTestEnabled", runtimeState.treeWindTest.enabled },
		{ "treeWindTestSpeed", runtimeState.treeWindTest.speed },
		{ "treeWindTestGustScale", runtimeState.treeWindTest.gustScale },
		{ "treeWindTestGustAmplitude", runtimeState.treeWindTest.gustAmplitude },
		{ "treeWindTestGustAdvectionMultiplier", runtimeState.treeWindTest.gustAdvectionMultiplier },
		{ "treeWindGustScale", settings.treeWindGustScale },
		{ "treeWindGustSoftLimit", settings.treeWindGustSoftLimit },
		{ "treeWindSpringFrequency", settings.treeWindSpringFrequency },
		{ "treeWindSpringDamping", settings.treeWindSpringDamping },
		{ "treeTransientSpringFrequency", settings.treeTransientSpringFrequency },
		{ "treeTransientSpringDamping", settings.treeTransientSpringDamping },
		{ "processMidRangeTransients", settings.processMidRangeTransients },
		{ "processFarRangeTransients", settings.processFarRangeTransients },
		{ "universalTreeResponseOverride", universalOverrideEnabled },
		{ "universalTreeResponse", {
									   { "bendSensitivity", universalValues.bend },
									   { "leafAmbientSensitivity", universalValues.leafAmbient },
									   { "upperBendRange", universalValues.upperBendRange },
									   { "maximumDisplacementPercent", universalValues.maximumDisplacementPercent },
									   { "trunkGustInfluence", universalValues.trunkGustInfluence },
									   { "leafGustInfluence", universalValues.leafGustInfluence },
									   { "transientWindInfluence", universalValues.transientWindInfluence },
									   { "leafTransientWindInfluence", universalValues.leafTransientWindInfluence },
									   { "leafTransientFlutterMaximum", universalValues.leafTransientFlutterMaximum },
									   { "transientMaximumBendMultiplier", universalValues.transientMaximumBendMultiplier },
								   } },
		{ "treeLeafBaseWindFlutterGain", settings.treeLeafBaseWindFlutterGain },
	};
}

void Wind::RegisterUxActions()
{
	FEATURE_COMMAND("setUniversalTreeResponse",
		"Set the runtime-only response used for every tree. All response params and enabled are optional.",
		[](Feature*, const json& args) {
			const auto currentOverride = TreeWindPatcher::GetUniversalOverride();
			auto enabled = currentOverride.first;
			auto values = currentOverride.second;
			if (args.contains("enabled") && args["enabled"].is_boolean())
				enabled = args["enabled"].get<bool>();
			if (args.contains("bendSensitivity") && args["bendSensitivity"].is_number())
				values.bend = args["bendSensitivity"].get<float>();
			if (args.contains("leafAmbientSensitivity") && args["leafAmbientSensitivity"].is_number())
				values.leafAmbient = args["leafAmbientSensitivity"].get<float>();
			if (args.contains("upperBendRange") && args["upperBendRange"].is_number())
				values.upperBendRange = args["upperBendRange"].get<float>();
			if (args.contains("maximumDisplacementPercent") && args["maximumDisplacementPercent"].is_number())
				values.maximumDisplacementPercent = args["maximumDisplacementPercent"].get<float>();
			if (args.contains("trunkGustInfluence") && args["trunkGustInfluence"].is_number())
				values.trunkGustInfluence = args["trunkGustInfluence"].get<float>();
			if (args.contains("leafGustInfluence") && args["leafGustInfluence"].is_number())
				values.leafGustInfluence = args["leafGustInfluence"].get<float>();
			if (args.contains("transientWindInfluence") && args["transientWindInfluence"].is_number())
				values.transientWindInfluence = args["transientWindInfluence"].get<float>();
			if (args.contains("leafTransientWindInfluence") && args["leafTransientWindInfluence"].is_number())
				values.leafTransientWindInfluence = args["leafTransientWindInfluence"].get<float>();
			if (args.contains("leafTransientFlutterMaximum") && args["leafTransientFlutterMaximum"].is_number())
				values.leafTransientFlutterMaximum = args["leafTransientFlutterMaximum"].get<float>();
			if (args.contains("transientMaximumBendMultiplier") && args["transientMaximumBendMultiplier"].is_number())
				values.transientMaximumBendMultiplier = args["transientMaximumBendMultiplier"].get<float>();
			TreeWindPatcher::SetUniversalOverride(enabled, values);
		});
	FEATURE_COMMAND("setTreeWindRule",
		"Apply live per-model tree wind tuning. Params: mesh (string), bendSensitivity and leafAmbientSensitivity (0-4), "
		"upperBendRange (5-100), maximumDisplacementPercent (0-10), trunkGustInfluence and leafGustInfluence (0-2), "
		"transientWindInfluence, leafTransientWindInfluence, "
		"leafTransientFlutterMaximum (0-20), and transientMaximumBendMultiplier (0-5).",
		[](Feature*, const json& args) {
			if (!args.contains("mesh") || !args["mesh"].is_string() ||
				!args.contains("bendSensitivity") || !args["bendSensitivity"].is_number() ||
				!args.contains("leafAmbientSensitivity") || !args["leafAmbientSensitivity"].is_number() ||
				!args.contains("upperBendRange") || !args["upperBendRange"].is_number() ||
				!args.contains("maximumDisplacementPercent") || !args["maximumDisplacementPercent"].is_number() ||
				!args.contains("trunkGustInfluence") || !args["trunkGustInfluence"].is_number() ||
				!args.contains("leafGustInfluence") || !args["leafGustInfluence"].is_number() ||
				!args.contains("transientWindInfluence") || !args["transientWindInfluence"].is_number() ||
				!args.contains("leafTransientWindInfluence") || !args["leafTransientWindInfluence"].is_number() ||
				!args.contains("leafTransientFlutterMaximum") || !args["leafTransientFlutterMaximum"].is_number() ||
				!args.contains("transientMaximumBendMultiplier") || !args["transientMaximumBendMultiplier"].is_number()) {
				logger::warn("[TreeWindPatcher] Devbench setTreeWindRule received invalid arguments");
				return;
			}
			if (!TreeWindPatcher::SetRule(args["mesh"].get<std::string>(), args["bendSensitivity"].get<float>(),
					args["leafAmbientSensitivity"].get<float>(), args["upperBendRange"].get<float>(),
					args["maximumDisplacementPercent"].get<float>(), args["trunkGustInfluence"].get<float>(),
					args["leafGustInfluence"].get<float>(), args["transientWindInfluence"].get<float>(),
					args["leafTransientWindInfluence"].get<float>(),
					args["leafTransientFlutterMaximum"].get<float>(),
					args["transientMaximumBendMultiplier"].get<float>())) {
				logger::warn("[TreeWindPatcher] Devbench setTreeWindRule did not match mesh {}", args["mesh"].get<std::string>());
			}
		});
	FEATURE_COMMAND("saveTreeWindRules", "Write changed tree response values to each tree's last-scanned source JSON.",
		[](Feature*, const json&) {
			const auto result = TreeWindPatcher::SaveRules();
			if (!result.success)
				logger::error("[TreeWindPatcher] Devbench save failed: {}", result.error);
		});
	FEATURE_COMMAND("revertTreeWindRules", "Revert live tree wind edits made since the JSON was loaded or saved.",
		[](Feature*, const json&) { TreeWindPatcher::RevertUnsavedChanges(); });
	FEATURE_COMMAND("setTreeWindTestConditions",
		"Set runtime-only tree wind test conditions. Optional params: enabled (boolean), speed (0-2), gustScale (128-16384), gustAmplitude (0-1), gustAdvectionMultiplier (0-8).",
		[](Feature* feature, const json& args) {
			auto* utility = static_cast<Wind*>(feature);
			if (args.contains("enabled") && args["enabled"].is_boolean())
				utility->SetTreeWindTestEnabled(args["enabled"].get<bool>());
			if (args.contains("speed") && args["speed"].is_number())
				utility->runtimeState.treeWindTest.speed = ClampFiniteOrDefault(args["speed"].get<float>(), 0.0f, 2.0f, utility->runtimeState.treeWindTest.speed);
			if (args.contains("gustScale") && args["gustScale"].is_number())
				utility->runtimeState.treeWindTest.gustScale = ClampFiniteOrDefault(args["gustScale"].get<float>(),
					kWindFieldGustScaleMin, kWindFieldGustScaleMax, utility->runtimeState.treeWindTest.gustScale);
			if (args.contains("gustAmplitude") && args["gustAmplitude"].is_number())
				utility->runtimeState.treeWindTest.gustAmplitude = ClampFiniteOrDefault(args["gustAmplitude"].get<float>(),
					kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, utility->runtimeState.treeWindTest.gustAmplitude);
			if (args.contains("gustAdvectionMultiplier") && args["gustAdvectionMultiplier"].is_number())
				utility->runtimeState.treeWindTest.gustAdvectionMultiplier = ClampFiniteOrDefault(args["gustAdvectionMultiplier"].get<float>(),
					kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax,
					utility->runtimeState.treeWindTest.gustAdvectionMultiplier);
		});
	FEATURE_QUERY("treeWindRules",
		"Search live tree wind rules. Params: search (string, optional), offset (integer, default 0), limit (integer, 1-500, default 100).",
		([](const Feature*, const json& args) -> json {
			const std::string search = Util::FixFilePath(args.value("search", std::string{}));
			const std::size_t offset = static_cast<std::size_t>(std::max(args.value("offset", 0), 0));
			const std::size_t limit = static_cast<std::size_t>(std::clamp(args.value("limit", 100), 1, 500));
			json matches = json::array();
			std::size_t matchIndex = 0;
			std::size_t totalMatches = 0;
			for (std::size_t index = 0; index < TreeWindPatcher::GetRuleCount(); ++index) {
				const auto rule = TreeWindPatcher::GetRule(index);
				if (!search.empty() && rule.mesh.find(search) == std::string_view::npos)
					continue;
				if (matchIndex++ >= offset && matches.size() < limit) {
					matches.push_back({
						{ "mesh", rule.mesh },
						{ "bendSensitivity", rule.bend },
						{ "leafAmbientSensitivity", rule.leafAmbient },
						{ "upperBendRange", rule.upperBendRange },
						{ "maximumDisplacementPercent", rule.maximumDisplacementPercent },
						{ "trunkGustInfluence", rule.trunkGustInfluence },
						{ "leafGustInfluence", rule.leafGustInfluence },
						{ "transientWindInfluence", rule.transientWindInfluence },
						{ "leafTransientWindInfluence", rule.leafTransientWindInfluence },
						{ "leafTransientFlutterMaximum", rule.leafTransientFlutterMaximum },
						{ "transientMaximumBendMultiplier", rule.transientMaximumBendMultiplier },
						{ "unsaved", rule.unsaved },
					});
				}
				++totalMatches;
			}
			return json{
				{ "totalRules", TreeWindPatcher::GetRuleCount() },
				{ "totalMatches", totalMatches },
				{ "unsavedRules", TreeWindPatcher::GetUnsavedRuleCount() },
				{ "rules", std::move(matches) },
			};
		}));
}

json Wind::GetRuntimeFlags()
{
	const bool universalOverrideEnabled = TreeWindPatcher::GetUniversalOverride().first;
	return json{
		{ "VisualizeWindField", runtimeState.visualizeWindField },
		{ "WindFieldUseRealSpeed", runtimeState.windFieldUseRealSpeed },
		{ "WindFieldUseRealDirection", runtimeState.windFieldUseRealDirection },
		{ "TreeWindTestOverride", runtimeState.treeWindTest.enabled },
		{ "UniversalTreeResponseOverride", universalOverrideEnabled },
	};
}

bool Wind::SetRuntimeFlag(std::string_view a_name, bool a_value)
{
	if (a_name == "VisualizeWindField") {
		runtimeState.visualizeWindField = a_value;
		return true;
	}
	if (a_name == "WindFieldUseRealSpeed") {
		runtimeState.windFieldUseRealSpeed = a_value;
		return true;
	}
	if (a_name == "WindFieldUseRealDirection") {
		runtimeState.windFieldUseRealDirection = a_value;
		return true;
	}
	if (a_name == "TreeWindTestOverride") {
		SetTreeWindTestEnabled(a_value);
		return true;
	}
	if (a_name == "UniversalTreeResponseOverride") {
		const auto [enabled, values] = TreeWindPatcher::GetUniversalOverride();
		(void)enabled;
		TreeWindPatcher::SetUniversalOverride(a_value, values);
		return true;
	}
	return false;
}

void Wind::LoadSettings(json& o_json)
{
	const Settings defaults{};
	settings = o_json;
	if (!o_json.contains("windFieldGustCrosswindScale")) {
		const float legacyGustScale = ClampFiniteOrDefault(settings.windFieldGustScale,
			kWindFieldGustScaleMin, kWindFieldGustScaleMax, defaults.windFieldGustScale);
		settings.windFieldGustCrosswindScale = legacyGustScale * WindField::WindTuning{}.frontAspectRatio;
	}
	if (!o_json.contains("grassWindSpringQuality")) {
		if (o_json.contains("grassWindSpringTextureSize") && o_json["grassWindSpringTextureSize"].is_number_unsigned()) {
			const auto legacyTextureSize = o_json["grassWindSpringTextureSize"].get<uint32_t>();
			for (auto& range : settings.grassWindSpringQuality)
				range.textureSize = legacyTextureSize;
		}
		if (o_json.contains("grassWindSpringWorldSize") && o_json["grassWindSpringWorldSize"].is_number()) {
			const float legacyWorldSize = o_json["grassWindSpringWorldSize"].get<float>();
			if (std::isfinite(legacyWorldSize))
				settings.grassWindSpringQuality.back().maxDistance = legacyWorldSize * 0.5f;
		}
	}
	if (!o_json.contains("grassWindSpringFrequency") && o_json.contains("grassWindSpringLag") &&
		o_json["grassWindSpringLag"].is_number()) {
		const float legacyLag = std::max(o_json["grassWindSpringLag"].get<float>(), 0.01f);
		settings.grassWindSpringFrequency = 0.25f / legacyLag;
	}
	if (!o_json.contains("grassWindSpringDamping"))
		settings.grassWindSpringDamping = defaults.grassWindSpringDamping;
	if (!o_json.contains("treeLeafBaseWindFlutterGain")) {
		if (o_json.contains("treeLeafWindSensitivity") && o_json["treeLeafWindSensitivity"].is_number())
			settings.treeLeafBaseWindFlutterGain =
				o_json["treeLeafWindSensitivity"].get<float>() * defaults.treeLeafBaseWindFlutterGain;
		else if (o_json.contains("treeLeafAmbientSensitivity") && o_json["treeLeafAmbientSensitivity"].is_number())
			settings.treeLeafBaseWindFlutterGain =
				o_json["treeLeafAmbientSensitivity"].get<float>() * defaults.treeLeafBaseWindFlutterGain;
	}
	const auto windEffectSettings = o_json.find("windEffects");
	const bool hasWindEffectSettings = windEffectSettings != o_json.end() && windEffectSettings->is_object();
	const json emptyEffectSettings = json::object();
	for (auto& effect : windEffects) {
		const std::string effectId(effect->GetId());
		if (hasWindEffectSettings) {
			const auto effectSettings = windEffectSettings->find(effectId);
			effect->LoadSettings(effectSettings != windEffectSettings->end() && effectSettings->is_object() ?
									 *effectSettings :
									 emptyEffectSettings);
		} else {
			effect->LoadSettings(o_json);
		}
	}
	SanitizeSettings(settings);
}

void Wind::SaveSettings(json& o_json)
{
	SanitizeSettings(settings);
	o_json = settings;
	json windEffectSettings = json::object();
	for (const auto& effect : windEffects) {
		json effectSettings;
		effect->SaveSettings(effectSettings);
		windEffectSettings[std::string(effect->GetId())] = std::move(effectSettings);
	}
	o_json["windEffects"] = std::move(windEffectSettings);
}

void Wind::RestoreDefaultSettings()
{
	settings = {};
	for (auto& effect : windEffects)
		effect->RestoreDefaultSettings();
	TreeWindPatcher::SetUniversalOverride(false, {});
}

void Wind::RestoreCurrentPageDefaultSettings()
{
	const Settings defaults{};
	switch (uiState.activeSettingsPage) {
	case SettingsPage::WindField:
		settings.windFieldGustScale = defaults.windFieldGustScale;
		settings.windFieldGustCrosswindScale = defaults.windFieldGustCrosswindScale;
		settings.windFieldGustAmplitude = defaults.windFieldGustAmplitude;
		settings.windFieldGustAdvectionMultiplier = defaults.windFieldGustAdvectionMultiplier;
		settings.windFieldDirectionTransitionDuration = defaults.windFieldDirectionTransitionDuration;
		break;
	case SettingsPage::WindEffects:
		settings.processMidRangeTransients = defaults.processMidRangeTransients;
		settings.processFarRangeTransients = defaults.processFarRangeTransients;
		if (uiState.activeWindEffectIndex < windEffects.size())
			windEffects[uiState.activeWindEffectIndex]->RestoreDefaultSettings();
		break;
	case SettingsPage::Trees:
		settings.enableTrunkBend = defaults.enableTrunkBend;
		settings.trunkWindBendSensitivity = defaults.trunkWindBendSensitivity;
		settings.treeLeafBaseWindFlutterGain = defaults.treeLeafBaseWindFlutterGain;
		settings.treeWindGustScale = defaults.treeWindGustScale;
		settings.treeWindGustSoftLimit = defaults.treeWindGustSoftLimit;
		settings.treeWindSpringFrequency = defaults.treeWindSpringFrequency;
		settings.treeWindSpringDamping = defaults.treeWindSpringDamping;
		settings.treeTransientSpringFrequency = defaults.treeTransientSpringFrequency;
		settings.treeTransientSpringDamping = defaults.treeTransientSpringDamping;
		break;
	case SettingsPage::TreeMeshes:
		TreeWindPatcher::RevertUnsavedChanges();
		TreeWindPatcher::SetUniversalOverride(false, {});
		break;
	case SettingsPage::Grass:
		ResetGrassWindSettings();
		break;
	}
}

bool Wind::ReapplyCurrentPageOverrideSettings()
{
	static constexpr std::array<std::string_view, 5> windFieldKeys{
		"windFieldGustScale",
		"windFieldGustCrosswindScale",
		"windFieldGustAmplitude",
		"windFieldGustAdvectionMultiplier",
		"windFieldDirectionTransitionDuration"
	};
	static constexpr std::array<std::string_view, 3> windEffectKeys{
		"processMidRangeTransients",
		"processFarRangeTransients",
		"windEffects"
	};
	static constexpr std::array<std::string_view, 9> treeKeys{
		"enableTrunkBend",
		"trunkWindBendSensitivity",
		"treeLeafBaseWindFlutterGain",
		"treeWindGustScale",
		"treeWindGustSoftLimit",
		"treeWindSpringFrequency",
		"treeWindSpringDamping",
		"treeTransientSpringFrequency",
		"treeTransientSpringDamping"
	};
	static constexpr std::array<std::string_view, 13> grassKeys{
		"overrideTrunkWindIntensity",
		"trunkWindIntensityOverride",
		"enableAmbientGrassWind",
		"grassWindResponse",
		"grassWindSensitivity",
		"grassWindMaximumTilt",
		"grassWindBendProfile",
		"grassWindCompressionToBend",
		"grassWindSpringFrequency",
		"grassWindSpringDamping",
		"grassWindSpringQuality",
		"grassWindFlutterStrength",
		"grassWindFlutterFrequency"
	};

	switch (uiState.activeSettingsPage) {
	case SettingsPage::WindField:
		return ReapplyOverrideSettingsForKeys(windFieldKeys);
	case SettingsPage::WindEffects:
		return ReapplyOverrideSettingsForKeys(windEffectKeys);
	case SettingsPage::Trees:
		return ReapplyOverrideSettingsForKeys(treeKeys);
	case SettingsPage::TreeMeshes:
		TreeWindPatcher::RevertUnsavedChanges();
		TreeWindPatcher::SetUniversalOverride(false, {});
		return true;
	case SettingsPage::Grass:
		return ReapplyOverrideSettingsForKeys(grassKeys);
	}
	return false;
}

void Wind::SetupResources()
{
	SetupGrassWindResources();
	SetupTreeWindResources();
}

Wind::PerFrameData Wind::GetCommonBufferData() const
{
	return {
		runtimeState.visualizeWindField ? 1u : 0u,
		static_cast<uint32_t>(runtimeState.windFieldDebugView),
		{}
	};
}

void Wind::PostPostLoad()
{
	TreeWindPatcher::LoadAndInstall();
}

void Wind::DataLoaded()
{
	for (auto& effect : windEffects)
		effect->DataLoaded();
}

void Wind::UpdateWindEffects(float a_frameTime)
{
	for (auto& effect : windEffects)
		effect->Update(a_frameTime);
}

void Wind::OnSceneTransitionReset(bool)
{
	for (auto& effect : windEffects)
		effect->Reset();
}
