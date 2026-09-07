#pragma once

#include "Buffer.h"
#include "Feature.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <winrt/base.h>

/** @brief GPU-driven, world-space airborne rain rendering. */
struct RainRendering : Feature
{
private:
	template <class T>
	struct SettingRange
	{
		T minimum;
		T maximum;
	};

	static constexpr uint32_t kMaximumDropCount = 65536;
	static constexpr uint32_t kRainComputeGroupSize = 128;
	static constexpr uint32_t kMaximumCompactionGroupCount = kMaximumDropCount / kRainComputeGroupSize;
	static constexpr uint32_t kMaximumDebugMode = 8;
	static constexpr uint32_t kMinimumRuntimeDropCount = 1;
	static constexpr float kMaximumRefractionPixels = 12.0f;
	static constexpr const char* kDefaultRainTexturePath = "Data\\Textures\\CommunityShaders\\RainRendering\\RainDrops\\Default\\RainDrop.png";
	static constexpr const char* kCustomRainTexturePath = "Data\\Textures\\CommunityShaders\\RainRendering\\RainDrops\\Custom\\RainDrop.png";

	static constexpr SettingRange<uint32_t> kDropCountRange{ 2048, kMaximumDropCount };
	static constexpr SettingRange<uint32_t> kOverheadDropCountRange{ 0, 64 };
	static constexpr SettingRange<float> kUnitRange{ 0.0f, 1.0f };
	static constexpr SettingRange<float> kDoubleUnitRange{ 0.0f, 2.0f };
	static constexpr SettingRange<float> kBudgetWeightRange{ 0.0f, 4.0f };
	static constexpr SettingRange<float> kFarDistanceRange{ 2000.0f, 30000.0f };
	static constexpr SettingRange<float> kNearCutoffDistanceRange{ 0.0f, 64.0f };
	static constexpr SettingRange<float> kFallSpeedRange{ 500.0f, 6000.0f };
	static constexpr SettingRange<float> kStreakLengthRange{ 4.0f, 500.0f };
	static constexpr SettingRange<float> kVelocityStretchRange{ 0.0f, 0.25f };
	static constexpr SettingRange<float> kStreakWidthRange{ 0.2f, 8.0f };
	static constexpr SettingRange<float> kBrightnessRange{ 0.0f, 4.0f };
	static constexpr SettingRange<float> kDensityNoiseScaleRange{ 256.0f, 16000.0f };
	static constexpr SettingRange<float> kCurtainScaleRange{ 512.0f, 30000.0f };
	static constexpr SettingRange<float> kCurtainContrastRange{ 0.25f, 4.0f };
	static constexpr SettingRange<float> kCurtainMinimumDensityRange{ 0.0f, 2.0f };
	static constexpr SettingRange<float> kCurtainMaximumDensityRange{ 0.0f, 3.0f };
	static constexpr SettingRange<float> kIntersectionFadeRange{ 1.0f, 400.0f };
	static constexpr SettingRange<float> kCoreDarkeningRange{ 0.0f, 0.8f };
	static constexpr SettingRange<float> kEdgeHighlightRange{ 0.0f, 4.0f };
	static constexpr SettingRange<float> kHighlightRoughnessRange{ 0.08f, 0.6f };
	static constexpr SettingRange<float> kTextureUVWidthRange{ 0.1f, 1.0f };
	static constexpr SettingRange<float> kRefractionDistanceRange{ 256.0f, 6000.0f };
	static constexpr SettingRange<float> kRoofFadeStartRange{ 0.0f, 0.99f };
	static constexpr SettingRange<float> kRuntimeFarDistanceRange{ 1000.0f, 50000.0f };
	static constexpr SettingRange<float> kRuntimeFallSpeedRange{ 100.0f, 10000.0f };
	static constexpr SettingRange<float> kRuntimeStreakLengthRange{ 1.0f, 1000.0f };
	static constexpr SettingRange<float> kRuntimeStreakWidthRange{ 0.05f, 20.0f };
	static constexpr SettingRange<float> kRuntimeBrightnessRange{ 0.0f, 8.0f };
	static constexpr SettingRange<float> kRuntimeDensityNoiseScaleRange{ 64.0f, 50000.0f };
	static constexpr SettingRange<float> kRuntimeCurtainScaleRange{ 64.0f, 80000.0f };
	static constexpr SettingRange<float> kRuntimeCurtainContrastRange{ 0.1f, 8.0f };
	static constexpr SettingRange<float> kRuntimeCurtainDensityRange{ 0.0f, 4.0f };
	static constexpr SettingRange<float> kRuntimeIntersectionFadeRange{ 1.0f, 1000.0f };
	static constexpr float kMinimumNearLayerDistance = 200.0f;
	static constexpr float kMaximumNearLayerDistance = 4000.0f;
	static constexpr float kNearLayerFarDistanceRatio = 0.45f;
	static constexpr float kMinimumMidLayerDistance = 500.0f;
	static constexpr float kMaximumMidLayerDistance = 12000.0f;
	static constexpr float kMidLayerNearDistanceRatio = 1.25f;
	static constexpr float kMidLayerFarDistanceRatio = 0.85f;

public:
	/** @brief Runtime-tunable airborne rain controls. */
	struct Settings
	{
		uint EnableRainRendering = 1;
		uint ForceRainRendering = 0;
		uint EnableRainRoofOcclusion = 1;
		uint EnableRainWind = 1;
		uint RainDropCount = 20495;
		uint RainOverheadDropCount = 30;
		float RainDensity = 2.0f;
		float RainFallSpeed = 2336.0f;
		float RainWindInfluence = 2.0f;

		float RainStreakLength = 72.0f;
		float RainVelocityStretch = 0.045f;
		float RainStreakWidth = 3.60f;

		float RainOpacity = 0.20f;
		float RainBrightness = 0.85f;
		float RainLightingResponse = 0.51f;
		float RainMinimumVisibility = 0.02f;
		float RainNearCutoffDistance = 4.0f;
		float RainFarDistance = 6000.0f;
		float RainNearLayerDistance = 1065.0f;
		float RainMidLayerDistance = 2420.0f;
		float RainNearBudgetWeight = 4.00f;
		float RainMidBudgetWeight = 1.00f;
		float RainFarBudgetWeight = 0.15f;
		float RainDensityNoiseScale = 5089.0f;
		float RainDensityNoiseStrength = 0.76f;

		float RainCurtainScale = 7500.0f;
		float RainCurtainStrength = 0.80f;
		float RainCurtainContrast = 1.75f;

		float RainCurtainMinDensity = 0.28f;
		float RainCurtainMaxDensity = 1.85f;

		float RainIntersectionFadeDistance = 200.0f;
		uint RainDebugMode = 0;

		uint EnableGlassyRain = 1;
		uint EnableRainRefraction = 1;
		float RainCoreDarkening = 0.08f;
		float RainEdgeHighlight = 1.0f;
		float RainRefractionStrength = 6.0f;
		float RainRefractionDistance = 6000.0f;
		float RainStreakVariation = 0.45f;
		float RainLocalLightResponse = 1.5f;
		uint EnableTexturedRain = 0;
		std::string RainTexturePath = kDefaultRainTexturePath;
		float RainTextureNormalStrength = 2.0f;
		float RainTextureReflectionStrength = 1.0f;
		float RainTextureUVWidth = 0.5f;
		float RainEnvironmentTransmission = 0.8f;
		float RainSceneRefractionMix = 1.0f;
		float RainHighlightRoughness = 0.18f;
		float RainLightScattering = 1.0f;
		float RainRoofOcclusionFadeStart = 0.20f;
		float RainRoofOcclusionFadeEnd = 0.75f;
	};

	/** @brief Per-draw constants mirrored by RainRendering.hlsl. */
	struct alignas(16) PerFrame
	{
		float4 HeadPositionAndTime;
		float4 VolumeSizeAndDensity;
		float4 WeatherFallDepth;
		float4 Streak;
		float4 Appearance;
		float4 DistanceNoise;
		float4 Curtain;
		float4 CurtainDensity;
		float4 LightColor;
		float4 CameraData;
		std::array<uint32_t, 4> GridAndDebug;
		float4 Glassy;
		float4 Refraction;
		float4 ScreenSize;
		float4 LocalLighting;
		std::array<uint32_t, 4> LightGrid;
		float4 TexturedRain;
		float4 RainTextureShape;
		float4 LayerRadii;
		std::array<uint32_t, 4> LayerCounts;
		float4 MaterialLighting;
		float4 RoofOcclusion;
		float4 VanillaWind;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);
	static_assert(sizeof(PerFrame) == 368, "RainRendering::PerFrame must match the rain shaders");

	/** @brief Shared particle-shader controls for replacing Skyrim rain. */
	struct alignas(16) CommonBuffer
	{
		uint DisableVanillaRain = 0;
		uint pad[3]{};
	};
	STATIC_ASSERT_ALIGNAS_16(CommonBuffer);

	/** @brief GPU render record produced once per drop and consumed by both eyes. */
	struct alignas(16) DropData
	{
		float4 PositionLength;
		float4 VelocityWidth;
		float4 ColorOpacity;
		float4 LightDirection;
	};
	STATIC_ASSERT_ALIGNAS_16(DropData);
	static_assert(sizeof(DropData) == 64, "RainRendering::DropData must match RainRendering.hlsl");

	Settings settings;

	std::string GetName() override { return "Rain Rendering"; }
	std::string GetShortName() override { return "RainRendering"; }
	std::string GetDisplayName() override { return "Airborne Rain"; }
	std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	std::string_view GetShaderDefineName() override { return "RAIN_RENDERING"; }
	bool HasShaderDefine(RE::BSShader::Type a_shaderType) override { return a_shaderType == RE::BSShader::Type::Particle; }
	bool SupportsVR() override { return true; }
	/** @brief Returns the per-frame vanilla-rain replacement state. */
	CommonBuffer GetCommonBufferData() const;
	/** @brief Returns whether airborne rain currently owns visible rain rendering. */
	bool IsReplacingVanillaRain() const;

	/** @brief Returns the feature description and principal visual guarantees. */
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	/** @brief Creates the constant buffer and fixed-function render states. */
	void SetupResources() override;
	/** @brief Draws airborne rain controls. */
	void DrawSettings() override;
	/** @brief Draws the rain density and range controls in the VR performance panel. */
	void DrawPerformanceSettings() override;
	std::string GetPerformanceSectionLabel() override { return GetDisplayName(); }
	int GetPerformanceOrder() const override { return 45; }
	/** @brief Applies the selected rain GPU-budget tier. */
	void ApplyPerformanceProfile(PerfProfile a_profile) override;
	/** @brief Returns whether the rain GPU budget matches the selected tier. */
	bool MatchesPerformanceProfile(PerfProfile a_profile) const override;
	/** @brief Describes the rain GPU budget selected by a performance tier. */
	std::string GetProfilePreviewText(PerfProfile a_profile) const override;
	/** @brief Loads persisted rain settings. */
	void LoadSettings(json& o_json) override;
	/** @brief Saves persisted rain settings. */
	void SaveSettings(json& o_json) override;
	/** @brief Restores the feature defaults. */
	void RestoreDefaultSettings() override;
	/** @brief Releases runtime-compiled shaders so they can be rebuilt on demand. */
	void ClearShaderCache() override;

	/** @brief Draws rain before water when no water composite was observed recently. */
	void DrawBeforeWater();
	/** @brief Draws rain after the water composite and records the water-active frame. */
	void DrawAfterWater();

private:
	struct WeatherRainState
	{
		float intensity = 0.0f;
		float fallSpeedScale = 1.0f;
		float2 windSlope{};
	};

	void DrawRain();
	void DrawGeneralSettings();
	void DrawVolumeSettings();
	void DrawMotionSettings();
	void DrawAppearanceSettings();
	void DrawWaterMaterialSettings();
	void DrawTextureSettings(bool a_usesWaterMaterial);
	void DrawSpatialVariationSettings();
	void DrawOcclusionSettings();
	void DrawDiagnosticsSettings();
	void ApplyGlassyReferenceSettings();
	static const Settings& GetDefaultSettings();
	static float GetNearLayerMaximum(float a_farDistance);
	static float GetMidLayerMinimum(float a_nearDistance);
	static float GetMidLayerMaximum(float a_farDistance);
	void NormalizeSettings();
	bool UsesWaterMaterial() const;
	std::array<uint32_t, 4> GetLayerDropCounts() const;
	float4 GetLayerRadii(float a_farDistance) const;
	WeatherRainState GetWeatherRainState() const;
	static const RE::BSParticleShaderRainEmitter* GetRainEmitter(const RE::BSGeometry* a_precipitation);
	float3 GetRainLightColor() const;
	bool EnsureShaders();
	bool EnsureRainSampler();
	bool EnsureRainTexture();
	std::filesystem::path GetEffects11RainTexturePath() const;
	void SelectRainTexture(const std::filesystem::path& a_path);
	bool EnsureSceneColorShaders();
	bool CanUseRoofOcclusion() const;
	ID3D11ShaderResourceView* GetRainEnvironment() const;
	bool EnsureSceneColorCopy(ID3D11Texture2D* a_source, ID3D11RenderTargetView* a_view);
	void DownsampleSceneColor(ID3D11ShaderResourceView* a_color, ID3D11ShaderResourceView* a_depth, const float2& a_size);
	void UpdateGlassyConstants(PerFrame& a_data, const D3D11_TEXTURE2D_DESC& a_description, const float2& a_size, bool a_hasSceneColor) const;
	PerFrame BuildPerFrameData(const WeatherRainState& a_weather, const D3D11_TEXTURE2D_DESC& a_description, const float2& a_size, bool a_hasSceneColor) const;

	std::unique_ptr<ConstantBuffer> perFrameCB;
	std::unique_ptr<StructuredBuffer> dropBuffer;
	std::unique_ptr<StructuredBuffer> dropLocalOffsetBuffer;
	std::unique_ptr<StructuredBuffer> dropGroupOffsetBuffer;
	std::unique_ptr<StructuredBuffer> visibleDropIndexBuffer;
	std::unique_ptr<Buffer> indirectDrawArgsBuffer;
	winrt::com_ptr<ID3D11ComputeShader> rainUpdateCS;
	winrt::com_ptr<ID3D11ComputeShader> rainCountCS;
	winrt::com_ptr<ID3D11ComputeShader> rainPrefixCS;
	winrt::com_ptr<ID3D11ComputeShader> rainScatterCS;
	winrt::com_ptr<ID3D11VertexShader> rainVS;
	winrt::com_ptr<ID3D11PixelShader> rainPS;
	winrt::com_ptr<ID3D11VertexShader> sceneColorDownsampleVS;
	winrt::com_ptr<ID3D11PixelShader> sceneColorDownsamplePS;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	winrt::com_ptr<ID3D11SamplerState> refractionSampler;
	winrt::com_ptr<ID3D11ShaderResourceView> rainTextureSRV;
	float2 rainTextureSize{};
	bool rainTextureLoadAttempted = false;
	winrt::com_ptr<ID3D11Texture2D> sceneColorCopy;
	winrt::com_ptr<ID3D11RenderTargetView> sceneColorRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> sceneColorSRV;
	winrt::com_ptr<ID3D11Texture2D> sceneDepthCopy;
	winrt::com_ptr<ID3D11RenderTargetView> sceneDepthRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> sceneDepthSRV;
	D3D11_TEXTURE2D_DESC sceneColorDescription{};
	DXGI_FORMAT sceneColorViewFormat = DXGI_FORMAT_UNKNOWN;
	bool sceneColorCopyFailed = false;
	bool shaderCompileAttempted = false;
	bool renderPathReady = false;
	uint32_t lastWaterBlendFrame = UINT32_MAX;
	uint32_t lastDrawFrame = UINT32_MAX;
};
