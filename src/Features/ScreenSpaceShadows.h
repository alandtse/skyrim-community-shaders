#pragma once

#include "Buffer.h"
#include "Utils/LazyShader.h"

struct ScreenSpaceShadows : Feature
{
public:
	virtual inline std::string GetName() override { return "Screen Space Shadows"; }
	virtual std::string GetDisplayName() override { return T("feature.screen_space_shadows.name", "Screen Space Shadows"); }
	virtual inline std::string GetShortName() override { return "ScreenSpaceShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SCREEN_SPACE_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.screen_space_shadows.description", "Screen Space Shadows enhances shadow quality by adding detailed contact shadows and improving shadow accuracy.\nThis technique adds fine-detail shadows that traditional shadow mapping might miss."),
			{ T("feature.screen_space_shadows.key_feature_1", "Enhanced contact shadows"),
				T("feature.screen_space_shadows.key_feature_2", "Improved shadow detail"),
				T("feature.screen_space_shadows.key_feature_3", "Better shadow accuracy"),
				T("feature.screen_space_shadows.key_feature_4", "Fine-scale shadow effects"),
				T("feature.screen_space_shadows.key_feature_5", "Configurable shadow contrast") } };
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct BendSettings
	{
		float SurfaceThickness = !globals::game::isVR ? 0.02f : 0.010f;
		float BilinearThreshold = 0.02f;
		float ShadowContrast = !globals::game::isVR ? 1.0f : 4.0f;
		uint Enable = 1;
		uint SampleCount = 1;
		uint EnableFoveated = 0;
		uint pad0[2];
	};

	BendSettings bendSettings;

	struct alignas(16) RaymarchCB
	{
		// Runtime data returned from BuildDispatchList():
		float LightCoordinate[4];  // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
		int WaveOffset[2];         // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

		// Renderer Specific Values:
		float FarDepthValue;   // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
		float NearDepthValue;  // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

		// Sampling data:
		float InvDepthTextureSize[2];  // Inverse of the texture dimensions for 'DepthTexture' (used to convert from pixel coordinates to UVs)
									   // If 'PointBorderSampler' is an Unnormalized sampler, then this value can be hard-coded to 1.
									   // The 'USE_HALF_PIXEL_OFFSET' macro might need to be defined if sampling at exact pixel coordinates isn't precise (e.g., if odd patterns appear in the shadow).

		float2 DynamicRes;
		float FoveatedData0[4];         // x=centerScale, y=centerFeather, z=centerHorizontalScale, w=enabled
		float FoveatedCenterOffset[4];  // xy=current eye's center offset (selected per-eye at dispatch), zw=padding

		BendSettings settings;
	};
	STATIC_ASSERT_ALIGNAS_16(RaymarchCB);

	bool enableStereoSync = true;

	struct alignas(16) StereoSyncCB
	{
		float FrameDim[2];
		float RcpFrameDim[2];
		float DispatchBase[2];
		float DispatchExtent[2];
		float FoveatedData0[4];  // x=centerScale, y=centerFeather, z=centerHorizontalScale, w=enabled
		float FoveatedCenterOffset[4];
	};
	STATIC_ASSERT_ALIGNAS_16(StereoSyncCB);

	ID3D11SamplerState* pointBorderSampler = nullptr;

	ConstantBuffer* raymarchCB = nullptr;
	Util::LazyShader<ID3D11ComputeShader> raymarchCS;
	Util::LazyShader<ID3D11ComputeShader> raymarchRightCS;

	Texture2D* screenSpaceShadowsTexture = nullptr;

	// VR stereo sync resources
	Texture2D* stereoSyncCopyTex = nullptr;
	ConstantBuffer* stereoSyncCB = nullptr;
	Util::LazyShader<ID3D11ComputeShader> stereoSyncCS;

	/** @brief Creates the raymarch constant buffer, point border sampler, and shadow output texture. */
	virtual void SetupResources() override;

	/** @brief Draws the ImGui settings UI for screen-space shadow configuration. */
	virtual void DrawSettings() override;
	virtual void DrawPerformanceSettings() override;
	/// @brief DrawPerformanceSettings() only draws the stereo sync toggle.
	bool PerformanceSectionRequiresVR() const override { return true; }
	std::string GetPerformanceSectionLabel() override { return GetDisplayName(); }
	int GetPerformanceOrder() const override { return 30; }
	virtual void ApplyPerformanceProfile(PerfProfile profile) override;
	bool MatchesPerformanceProfile(PerfProfile profile) const override;
	/// @brief Surfaces the FOV Screen Space Shadows toggle in the Performance hub, mirroring
	/// the SSS panel's own control.
	void DrawPerformancePresets() override;
	/// @brief Renders the VR stereo sync toggle. Shared by the SSS panel and
	/// the Performance hub. VR-only; caller guards on isVR.
	void DrawStereoToggles();
	/// @brief Renders the FOV Screen Space Shadows checkbox + tooltip + unavailable-reason
	/// line. Shared by the SSS panel and the Performance hub. VR-only; caller guards on isVR.
	void DrawFoveatedToggle();

	/** @brief Releases the compiled raymarch compute shader for recompilation. */
	virtual void ClearShaderCache() override;
	/** @brief Releases the raymarch compute shader so it is recompiled on next use. */
	void InvalidateRaymarchShaders();
	/** @brief Calculates the resolution-scaled and quantized sample count for the raymarch shader. */
	uint GetScaledSampleCount();
	uint lastCompiledSampleCount = 0;
	/**
	 * @brief Returns the compiled raymarch compute shader, recompiling if the sample count changed.
	 * @return The compiled ID3D11ComputeShader, or nullptr on failure.
	 */
	ID3D11ComputeShader* GetComputeRaymarch();
	ID3D11ComputeShader* GetComputeRaymarchRight();

	/** @brief Clears the shadow texture and dispatches shadow ray marching if conditions are met. */
	virtual void Prepass() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/** @brief Dispatches the Bend SSS compute shader to generate screen-space contact shadows. */
	void DrawShadows();
	void DrawStereoSync();

	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; };
};
