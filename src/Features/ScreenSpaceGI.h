#pragma once

#include "Buffer.h"
#include "Utils/BootSnapshot.h"

struct ScreenSpaceGI : Feature
{
private:
	static constexpr std::string_view MOD_ID = "130375";

public:
	bool inline SupportsVR() override { return true; }

	virtual inline std::string GetName() override { return "Screen Space GI"; }
	virtual std::string GetDisplayName() override { return T("feature.screen_space_gi.name", "Screen Space GI"); }
	virtual inline std::string GetShortName() override { return "ScreenSpaceGI"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		std::string desc =
			T("feature.screen_space_gi.description",
				"Screen Space Global Illumination adds realistic indirect lighting and "
				"ambient occlusion to the game. This technique simulates how light "
				"bounces off surfaces to illuminate other objects naturally.");
		if (globals::game::isVR) {
			desc +=
				T("feature.screen_space_gi.vr_warning",
					"\n\nWarning: In VR, this feature may have visual artifacts and "
					"can have a significant performance impact due to the nature of "
					"screen space effects.");
		}
		return std::make_pair(
			desc,
			std::vector<std::string>{
				T("feature.screen_space_gi.key_feature_1", "Realistic indirect lighting"),
				T("feature.screen_space_gi.key_feature_2", "Enhanced ambient occlusion"),
				T("feature.screen_space_gi.key_feature_3", "Improved visual depth and atmosphere"),
				T("feature.screen_space_gi.key_feature_4", "Temporal denoising for smooth results"),
				T("feature.screen_space_gi.key_feature_5", "Configurable quality and performance settings") });
	}

	/** @brief Resets all settings to their default values and flags shaders for recompilation. */
	virtual void RestoreDefaultSettings() override;
	/** @brief Draws the ImGui settings UI with quality presets, visual parameters, and denoising options. */
	virtual void DrawSettings() override;
	virtual void DrawPerformanceSettings() override;
	/// @brief DrawPerformanceSettings() only draws the stereo reprojection toggle.
	bool PerformanceSectionRequiresVR() const override { return true; }
	std::string GetPerformanceSectionLabel() override { return GetDisplayName(); }
	int GetPerformanceOrder() const override { return 40; }
	virtual void ApplyPerformanceProfile(PerfProfile profile) override;
	bool MatchesPerformanceProfile(PerfProfile profile) const override;
	/// @brief Renders the VR stereo reprojection toggle. Shared by the SSGI panel and the
	/// Performance hub. VR-only; caller guards on isVR.
	void DrawReprojectToggle();

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/** @brief Registers the loading screen listener that resets the temporal history. */
	virtual void PostPostLoad() override;
	/** @brief Creates GPU textures, samplers, constant buffers, and compiles compute shaders. */
	virtual void SetupResources() override;
	/** @brief Releases and recompiles all SSGI compute shaders. */
	virtual void ClearShaderCache() override;
	/** @brief Compiles all SSGI compute shaders with current resolution and feature defines. */
	void CompileComputeShaders();
	/** @brief Checks whether all required compute shaders and the noise texture loaded successfully. */
	bool ShadersOK();

	/** @brief Executes the full SSGI pipeline: depth prefilter, radiance fetch, GI, blur, and upsample. */
	void DrawSSGI();
	/** @brief Updates the SSGI constant buffer with current camera, resolution, and settings data. */
	void UpdateSB();
	/** @brief Discard temporal accumulation before the next SSGI dispatch. */
	void QueueHistoryReset() { queuedResetHistory.store(true, std::memory_order_release); }

	//////////////////////////////////////////////////////////////////////////////////

	bool recompileFlag = false;
	uint outputAoIdx = 0;
	uint outputIlIdx = 0;

	// Loading screen radiance decays at only 1/MaxAccumFrames per frame, bleeding the old scene
	// into the new one for some frames.
	std::atomic<bool> queuedResetHistory{ true };

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		static bool Register();
	};

	static constexpr int kResourceProfileFullGI = 0;
	static constexpr int kResourceProfileAOOnly = 1;

	struct Settings
	{
		bool Enabled = true;
		// Keep raw runtime check for ctor-time defaults before globals::ReInit().
		bool EnableGI = REL::Module::IsVR() ? false : true;  // AO only for VR by default
		bool EnableExperimentalSpecularGI = false;
		bool EnableVanillaSSAO = false;
		// performance/quality
		uint NumSlices = REL::Module::IsVR() ? 3u : 4u;  // AO preset for VR
		uint NumSteps = REL::Module::IsVR() ? 6u : 8u;
		bool EnableAdaptiveSampling = false;
		int ResolutionMode = 1;  // 0-full, 1-half, 2-quarter - DBF default
		// Restart-gated: default resource allocation follows the platform's default effect mode.
		int ResourceProfile = EnableGI ? kResourceProfileFullGI : kResourceProfileAOOnly;
		// visual
		float MinScreenRadius = 0.01f;
		float AORadius = 256.f;
		float GIRadius = 256.f;
		float Thickness = 32.f;
		float2 DepthFadeRange = { 4e4, 5e4 };
		// gi
		float GISaturation = 0.8f;
		float GIDistanceCompensation = 0.f;
		// mix
		float AOPower = 1.0f;
		float GIStrength = 1.0f;
		// denoise
		bool EnableTemporalDenoiser = true;
		bool EnableBlur = true;
		float DepthDisocclusion = .1f;
		float NormalDisocclusion = .1f;
		uint MaxAccumFrames = 16;
		float BlurRadius = 2.f;
		float DistanceNormalisation = 2.f;
		// VR: reproject eye 0's view-independent diffuse GI into eye 1 (skips the eye-1 march).
		// Default on; ignored when specular GI is on (specular is view-dependent).
		bool UseStereoReproject = true;
		// Debug: VR-only unjittered projection/inverse-view reconstruction (avoids TAA jitter swim).
		bool DebugUseUnjitteredCameraReconstruction = false;
	} settings;

	// Resource profile active since resource creation; a differing settings value is restart-pending.
	int activeResourceProfile = kResourceProfileFullGI;

	bool HasGIResources() const { return activeResourceProfile == kResourceProfileFullGI; }
	bool IsGIActive() const { return settings.EnableGI && HasGIResources(); }
	bool IsSpecularGIActive() const { return IsGIActive() && settings.EnableExperimentalSpecularGI; }

	inline static constexpr Util::Settings::RestartTable<Settings, 1> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, ResourceProfile, "SSGI Resource Profile"),
	} };
	Util::Settings::BootSnapshot<Settings> bootSnapshot{ kRestartFields };

	std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const override
	{
		return { kRestartFields.data(), kRestartFields.size() };
	}
	const void* GetBootValue(std::string_view jsonKey) const override { return bootSnapshot.RawBoot(jsonKey); }
	const void* GetSettingsBlob() const override { return &settings; }
	size_t GetSettingsBlobSize() const override { return sizeof(settings); }

	struct alignas(16) SSGICB
	{
		float4x4 PrevInvViewMat[2];
		float2 NDCToViewMul[2];
		float2 NDCToViewAdd[2];

		float2 TexDim;
		float2 RcpTexDim;  //
		float2 FrameDim;
		float2 RcpFrameDim;  //
		uint FrameIndex;

		uint NumSlices;
		uint NumSteps;

		float MinScreenRadius;  //
		float AORadius;
		float GIRadius;
		float EffectRadius;
		float Thickness;  //
		float2 DepthFadeRange;
		float DepthFadeScaleConst;

		float GISaturation;  //
		float GIDistanceCompensation;
		float GICompensationMaxDist;
		float pad1;

		float AOPower;  //
		float GIStrength;

		float DepthDisocclusion;
		float NormalDisocclusion;
		uint MaxAccumFrames;  //

		float BlurRadius;
		float DistanceNormalisation;

		uint UseModeTexture;  // VRStereoOptimizations' classification available this boot
		float pad;
	};
	STATIC_ASSERT_ALIGNAS_16(SSGICB);
	eastl::unique_ptr<ConstantBuffer> ssgiCB;

	/// Set once per frame by UpdateSB(); DrawSSGI() reuses it instead of re-checking
	/// VRStereoOptimizations' boot-latched classification readiness per dispatch.
	bool useModeTextureThisFrame = false;

	eastl::unique_ptr<Texture2D> texNoise = nullptr;
	eastl::unique_ptr<Texture2D> texWorkingDepth = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavWorkingDepth[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texPrevGeo = nullptr;
	eastl::unique_ptr<Texture2D> texRadiance = nullptr;
	eastl::unique_ptr<Texture2D> texRadianceTemp = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavRadiance[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texNormal = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavNormal[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texAccumFrames[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texAo[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texIlY[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texIlCoCg[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texGiSpecular[2] = { nullptr };

	/** @brief Returns the current output SRVs for AO, indirect lighting Y/CoCg, and specular GI (or nullptrs if disabled). */
	inline std::tuple<ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*> GetOutputTextures()
	{
		if (!(loaded && settings.Enabled) || outputAoIdx >= 2 || outputIlIdx >= 2 || !texAo[outputAoIdx])
			return { nullptr, nullptr, nullptr, nullptr };

		return {
			texAo[outputAoIdx]->srv.get(),
			texIlY[outputIlIdx] ? texIlY[outputIlIdx]->srv.get() : nullptr,
			texIlCoCg[outputIlIdx] ? texIlCoCg[outputIlIdx]->srv.get() : nullptr,
			texGiSpecular[outputAoIdx] ? texGiSpecular[outputAoIdx]->srv.get() : nullptr
		};
	}

	winrt::com_ptr<ID3D11SamplerState> linearClampSampler = nullptr;
	winrt::com_ptr<ID3D11SamplerState> pointClampSampler = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> prefilterDepthsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterRadianceCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterNormalCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> radianceDisoccCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> giCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> giEye0OnlyCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> blurCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> stereoSyncCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> reprojectCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> reprojectDebugCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> upsampleCompute = nullptr;
};
