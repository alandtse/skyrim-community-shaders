#pragma once

#include "Feature.h"
#include "Features/Wind/Grass/GrassWindState.h"
#include "Features/Wind/Runtime/WindRuntimeState.h"
#include "Features/Wind/Settings/WindSettings.h"
#include "Features/Wind/Trees/TreeWindState.h"
#include "Features/Wind/UI/WindUIState.h"
#include "Features/Wind/WindEffects/WindEffect.h"
#include "I18n/I18n.h"

#include <memory>
#include <vector>

/** Hosts the always-on shared wind controls and runtime resources. */
struct Wind : Feature
{
	/** Creates the built-in wind effect objects. */
	Wind();

	virtual std::string GetName() override { return "Wind"; }
	virtual std::string GetDisplayName() override { return T("feature.wind.name", "Wind"); }
	virtual std::string GetShortName() override { return "Wind"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kFoliage; }
	virtual bool SupportsVR() override { return true; }
	virtual bool IsCore() const override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.wind.description", "Shared ambient, tree, grass, and transient wind controls."),
			{ T("feature.wind.key_feature_1", "Procedural ambient gust field"),
				T("feature.wind.key_feature_2", "Tree and grass response tuning"),
				T("feature.wind.key_feature_3", "Transient wind impulses") } };
	}

	using Settings = WindSettings;
	using SettingsPage = WindSettingsPage;
	using PerFrameData = WindPerFrameData;
	using GrassWindSpringFieldData = ::GrassWindSpringFieldData;
	using GrassWindSpringData = ::GrassWindSpringData;
	using WindFieldDebugView = ::WindFieldDebugView;
	using RuntimeWindTest = ::RuntimeWindTest;

	Settings settings;
	WindUIState uiState;
	WindRuntimeState runtimeState;
	GrassWindState grassState;
	TreeWindState treeState;

	// Wind effects live until process exit because their Bethesda event sinks remain registered for that lifetime.
	// ProjectileHookDispatcher observers are removed explicitly by their effect routers.
	std::vector<std::unique_ptr<WindEffect>> windEffects;

	/** @copydoc Feature::DrawSettings */
	virtual void DrawSettings() override;
	virtual json GetDiagnostics() override;
	virtual json GetRuntimeFlags() override;
	virtual bool SetRuntimeFlag(std::string_view a_name, bool a_value) override;
	virtual void RegisterUxActions() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual bool HasScopedDefaultSettings() const override { return true; }
	virtual void RestoreCurrentPageDefaultSettings() override;
	virtual bool HasScopedOverrideSettings() const override { return true; }
	virtual bool ReapplyCurrentPageOverrideSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void OnSceneTransitionReset(bool a_opening) override;

	[[nodiscard]] PerFrameData GetCommonBufferData() const;
	/** Advances all configured transient wind effects for the current frame. */
	void UpdateWindEffects(float a_frameTime);
	void SetTreeWindTestEnabled(bool a_enabled);
	[[nodiscard]] bool ShouldUseRealWindSpeed() const { return !runtimeState.treeWindTest.enabled && runtimeState.windFieldUseRealSpeed; }
	[[nodiscard]] float GetEffectiveWindOverrideSpeed() const { return runtimeState.treeWindTest.enabled ? runtimeState.treeWindTest.speed : runtimeState.windFieldOverrideSpeed; }
	[[nodiscard]] float GetEffectiveWindGustScale() const { return runtimeState.treeWindTest.enabled ? runtimeState.treeWindTest.gustScale : settings.windFieldGustScale; }
	[[nodiscard]] float GetEffectiveWindGustAmplitude() const { return runtimeState.treeWindTest.enabled ? runtimeState.treeWindTest.gustAmplitude : settings.windFieldGustAmplitude; }
	[[nodiscard]] float GetEffectiveWindGustAdvectionMultiplier() const { return runtimeState.treeWindTest.enabled ? runtimeState.treeWindTest.gustAdvectionMultiplier : settings.windFieldGustAdvectionMultiplier; }
	/** Updates the grass response field once per frame and binds it to vertex or culling compute shaders. */
	void UpdateGrassWindSpring(bool a_compute = false);
	/** Updates and binds the persistent structural tree response field once per rendered frame. */
	void UpdateTreeWindSpring();
	/** Recreates one grass response-field texture pair at the requested resolution. */
	void RecreateGrassWindSpringTextures(uint32_t a_qualityIndex, uint32_t a_textureSize);
	[[nodiscard]] ID3D11ShaderResourceView* GetGrassWindSpringDebugSRV() const;

private:
	static void SanitizeSettings(Settings& a_settings);
	static void SanitizeGrassWindSettings(Settings& a_settings);
	static uint32_t SanitizeGrassWindSpringTextureSize(uint32_t a_textureSize);
	[[nodiscard]] uint32_t GetTransientFieldMask() const;
	void DrawWindFieldSettings();
	void DrawWindEffectsSettings();
	void SpawnDebugWindEffects();
	void DrawTreeSettings();
	void DrawTreeWindTestSettings();
	void DrawTreeMeshSettings();
	void DrawTreeMeshConflictWarning();
	void DrawTreeGlobalOverrideSettings();
	void DrawTreeMeshRuleControls();
	void DrawTreeMeshRulesTable();
	void ResetGrassWindSettings();
	void DrawGrassWindSettings();
	void SetupGrassWindResources();
	void SetupTreeWindResources();
	void RecreateTreeWindSpringTextures(uint32_t a_qualityIndex);
};
