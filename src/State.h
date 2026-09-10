#pragma once

#include <Tracy/Tracy.hpp>
#include <Tracy/TracyC.h>
#include <Tracy/TracyD3D11.hpp>

#include <Buffer.h>
#include <array>
#include <atomic>
#include <format>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>

using json = nlohmann::json;

#include "GpuPass.h"
#include <FeatureBuffer.h>

#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindField.h"
#include <Hooks.h>
#include <mutex>

class State
{
public:
	State()
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		for (auto& v : smoothDrawCalls) v = 0.0;
		for (auto& v : drawCalls) v = 0;
		for (auto& v : frameTimePerType) v = 0.0f;
		for (auto& v : smoothFrameTimePerType) v = 0.0f;
		for (auto& v : enabledClasses) v = true;

		// Initialize QueryPerformanceCounter frequency
		frameTimingFrequency.QuadPart = 0;
		frameStartTime.QuadPart = 0;
	}
	/** @brief Acquires the stats mutex and returns a scoped lock guard. */
	std::lock_guard<std::mutex> Lock() { return std::lock_guard<std::mutex>(statsMutex); }

	static State* GetSingleton()
	{
		static State singleton;
		return &singleton;
	}

	bool enabledClasses[RE::BSShader::Type::Total - 1];
	bool enablePShaders = true;
	bool enableVShaders = true;
	bool enableCShaders = true;

	bool updateShader = true;
	bool settingCustomShader = false;
	RE::BSShader* currentShader = nullptr;
	std::string adapterDescription = "";

	uint32_t currentVertexDescriptor = 0;
	uint32_t currentPixelDescriptor = 0;
	spdlog::level::level_enum logLevel = spdlog::level::info;
	bool enableDeveloperMode = false;  ///< Explicit developer mode toggle; also enabled when log level is debug/trace.
	std::string shaderDefinesString = "";
	std::vector<std::pair<std::string, std::string>> shaderDefines{};  // data structure to parse string into; needed to avoid dangling pointers

	float timer = 0;
	float windFieldFrameTime = 0.0f;
	float windFieldAmbientSpeed = 0.0f;
	float windFieldAdvectionSpeed = 0.0f;
	float windFieldGustTravelDistance = 0.0f;
	float previousWindFieldGustTravelDistance = 0.0f;
	float windFieldTravelDelta = 0.0f;
	float3 ambientWindVelocity = {};
	float3 windFieldSelectedVelocity = {};
	float3 previousWindFieldSelectedVelocity = {};
	WindField::Field windFieldCurrent{};
	WindField::Field previousWindFieldCurrent{};
	WindField::Field windFieldTransition{};
	WindField::Field previousWindFieldTransition{};
	float windFieldTransitionElapsed = 0.0f;
	float windFieldTransitionBlend = 1.0f;
	float previousWindFieldTransitionBlend = 1.0f;
	bool windFieldTransitionActive = false;
	float windFieldSelectedSpeed = 0.0f;
	bool windFieldHasPreviousSample = false;
	WindField::WindTuning windFieldTuning{};
	double smoothDrawCalls[RE::BSShader::Type::Total + 1];
	int drawCalls[RE::BSShader::Type::Total + 1];

	// Frame time tracking per shader type (in milliseconds)
	float frameTimePerType[RE::BSShader::Type::Total + 1];        ///< Per-type frame time in milliseconds.
	float smoothFrameTimePerType[RE::BSShader::Type::Total + 1];  ///< EMA-smoothed per-type frame time in milliseconds.

	// Timing state for per-type frame time tracking using QueryPerformanceCounter
	LARGE_INTEGER frameTimingFrequency;
	LARGE_INTEGER frameStartTime;
	bool frameTimingActive = false;
	std::optional<ScopedGpuPass> grassGpuPass;

	enum ConfigMode
	{
		DEFAULT,
		USER,
		TEST,
		THEME
	};

	/** @brief Per-draw-call hook: updates feature state, constant buffers, and overlay. */
	void Draw();
	/** @brief Keeps one GPU timer open across each contiguous batch of Grass draw calls. */
	void UpdateGrassGpuPass();
	/** @brief Accumulates per-shader-type draw call counts and frame timing for the performance overlay. */
	void Debug();
	/** @brief Per-frame reset: advances timer, caches menu state, resets descriptors and frame counters. */
	void Reset();
	/** @brief Samples the current shared wind field at an absolute world position. */
	[[nodiscard]] WindField::WindSample SampleWind(const float3& a_worldPosition) const noexcept;
	/** @brief Samples the shared field with explicit ambient direction and speed inputs. */
	[[nodiscard]] WindField::WindSample SampleWind(const float3& a_worldPosition,
		const float3& a_windDirection, float a_windSpeed) const noexcept;

	/** Identifies the producer that owns a transient wind source. */
	enum class TransientWindSourceOwner : uint8_t
	{
		Generic,
		FusRoDah,
		Dragon,
		SpellShout,
		StormCall,
		ProjectileMagic,
		WeaponThrowVR,
		Explosion,
		HeavyImpact
	};

	/** Controls which sources survive when the shared pool reaches capacity. */
	enum class TransientWindSourcePriority : uint8_t
	{
		Wingbeat = 10,
		Flight = 20,
		Impact = 40,
		Breath = 50,
		FusRoDah = 60
	};

	/** A frame-submitted source paired with its saturation priority. */
	struct TransientWindSourceSubmission
	{
		WindField::TransientWindSource source;
		TransientWindSourcePriority priority;
	};

	/** @brief Queues a transient XYZ pressure impulse for insertion into the shared wind field. */
	void QueueTransientWindImpulse(const WindField::TransientWindSource& a_impulse);
	/** @brief Queues a typed one-shot source with explicit ownership and saturation priority. */
	void QueueTransientWindSource(const WindField::TransientWindSource& a_source,
		TransientWindSourceOwner a_owner, TransientWindSourcePriority a_priority);
	/** @brief Replaces one producer's frame-updated attached sources. */
	void SetAttachedTransientWindSources(TransientWindSourceOwner a_owner,
		std::span<const TransientWindSourceSubmission> a_sources);
	/** @brief Removes active, pending, and attached sources owned by one producer. */
	void ClearTransientWindSources(TransientWindSourceOwner a_owner);
	/** @brief Removes all active and pending transient wind impulses. */
	void ClearTransientWindImpulses();
	/** @brief One-time post-D3D setup: creates resources, probes GPU caps, initializes features. */
	void Setup();

	/** Identifies the feature that owns the HDR tonemap pass for the current frame. */
	enum class TonemapOwner
	{
		kVanilla,
		kPostProcessing,
		kEffects11
	};

	/** Resolves and caches the tonemap owner for the current frame. */
	TonemapOwner GetTonemapOwner();
	/** Dispatches the HDR tonemap pass when a feature replaces vanilla rendering. */
	bool HandlePostProcessing(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output);
	/** Binds a_output as the sole render target (no depth-stencil) and syncs shadowState's
	    cached bookkeeping to match, so the engine's lazy DIRTY_RENDERTARGET rebind agrees. */
	void SetOutputRenderTarget(RE::RENDER_TARGET a_output);

	/**
	 * @brief Loads settings from disk (default, then user, then overrides).
	 * @param a_configMode Which config file to load.
	 * @param a_allowReload If true, retries once after a parse error.
	 */
	void Load(ConfigMode a_configMode = ConfigMode::USER, bool a_allowReload = true);
	/**
	 * @brief Persists current settings to the config file for the given mode.
	 * @param a_configMode Which config file to write.
	 * @param a_isExplicitUserSave False for Load()'s own internal migration/recovery
	 *  re-saves, which must not also persist Effects11's separate ENB-side state.
	 */
	void Save(ConfigMode a_configMode = ConfigMode::USER, bool a_isExplicitUserSave = true);

	/** @brief Saves a feature's boot preference immediately; returns false on failure. */
	bool SetFeatureBootEnabled(const std::string& featureName, bool enabled);
	/** @brief Saves a loaded feature's favorite status immediately; returns false on failure. */
	bool SetFeatureFavorite(const std::string& featureName, bool favorite);
	/** @brief Returns the saved favorite status, including for unloaded features. */
	bool IsFeatureFavorite(const std::string& featureName) const;

	/**
	 * @brief Serializes all settings to a JSON object (in-memory, no disk I/O).
	 * @param o_json Output JSON object to populate.
	 */
	void SaveToJson(nlohmann::json& o_json);
	/**
	 * @brief Restores settings from a JSON object (in-memory, no disk I/O).
	 * @param i_json Input JSON object to read from.
	 */
	void LoadFromJson(nlohmann::json& i_json);

	/** @brief Loads the active theme preset from the menu settings. */
	void LoadTheme();

	/**
	 * @brief Validates the disk shader cache against all loaded features.
	 * @param a_ini The cache INI to validate against.
	 * @return True if all feature cache entries are still valid.
	 */
	bool ValidateCache(CSimpleIniA& a_ini);
	/**
	 * @brief Writes each feature's cache metadata into the disk cache INI.
	 * @param a_ini The cache INI to write into.
	 */
	void WriteDiskCacheInfo(CSimpleIniA& a_ini);

	/**
	 * @brief Sets the global log level and flushes on that level.
	 * @param a_level The spdlog severity level to apply.
	 */
	void SetLogLevel(spdlog::level::level_enum a_level = spdlog::level::info);
	spdlog::level::level_enum GetLogLevel();

	/**
	 * @brief Parses a semicolon-delimited "NAME=VALUE" string into shader defines.
	 * @param defines Semicolon-separated define string (e.g. "FOO=1;BAR=2").
	 */
	void SetDefines(std::string defines);
	std::vector<std::pair<std::string, std::string>>* GetDefines();

	/**
	 * @brief Checks whether the given shader type is enabled.
	 * @param a_type The type of shader to check.
	 * @return True if the shader type is enabled.
	 */
	bool ShaderEnabled(const RE::BSShader::Type a_type);

	/**
	 * @brief Checks whether the given shader is enabled.
	 * @param a_shader The shader to check.
	 * @return True if the shader is enabled.
	 */
	bool IsShaderEnabled(const RE::BSShader& a_shader);

	/**
	 * @brief Checks whether developer mode is active.
	 *
	 * Active when Enable Developer Mode is on, or when log level is debug/trace.
	 * Developer mode enables advanced options. Use at your own risk.
	 * @return True if in developer mode.
	 */
	bool IsDeveloperMode();

	/**
	 * @brief Adds UAV access support to a render target.
	 * @param a_targetIndex The render target to modify.
	 * @param a_properties The target's properties (modified in place).
	 */
	void ModifyRenderTarget(RE::RENDER_TARGETS::RENDER_TARGET a_targetIndex, RE::BSGraphics::RenderTargetProperties& a_properties);

	/** @brief Allocates constant buffers, Tracy context, and profiler resources. */
	void SetupResources();

	/**
	 * @brief Logs per-format support for D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD.
	 *
	 * We perform typed UAV loads on a number of non-guaranteed formats; on GPUs
	 * that lack TypedUAVLoadAdditionalFormats those reads return undefined data.
	 * Called once at startup; emits one info line per supported format and one
	 * warn line per unsupported format with the feature that needs it.
	 */
	void CheckTypedUAVLoadSupport();
	/// @brief Returns true if the current device supports typed UAV loads (RWTexture<T>
	///        subscript reads) for @p a_format. Use to gate features that need it.
	static bool SupportsTypedUAVLoad(DXGI_FORMAT a_format);

	/**
	 * @brief Strips and rewrites shader descriptor bits for Community Shaders' pipeline.
	 * @param a_shader The shader being compiled.
	 * @param a_vertexDescriptor Vertex descriptor flags (modified in place).
	 * @param a_pixelDescriptor Pixel descriptor flags (modified in place).
	 * @param a_forceDeferred If true, forces the Deferred flag regardless of current pass.
	 */
	void ModifyShaderLookup(const RE::BSShader& a_shader, uint& a_vertexDescriptor, uint& a_pixelDescriptor, bool a_forceDeferred = false);

	/** @brief Opens a named GPU performance event (D3D annotation + Tracy zone). */
	void BeginPerfEvent(std::string_view title);
	/** @brief Formats the title into a reused buffer (no per-call heap allocation)
	 *  and opens the event. Prefer over BeginPerfEvent(std::format(...)) so annotation
	 *  string-building does not contend the heap lock with shader-compile workers. */
	template <class... Args>
	void BeginPerfEvent(std::format_string<Args...> fmt, Args&&... args)
	{
		thread_local std::string s_perfTitle;
		s_perfTitle.clear();
		std::format_to(std::back_inserter(s_perfTitle), fmt, std::forward<Args>(args)...);
		BeginPerfEvent(std::string_view{ s_perfTitle });
	}
	/** @brief Closes the most recent GPU performance event. */
	void EndPerfEvent();
	/** @brief Per-draw GPU-capture marker only (RenderDoc/PIX), no Tracy zone --
	 *  safe at the thousands-per-frame volume that would OOM Tracy. */
	void BeginDrawEvent(std::string_view title);
	/** @brief Formats the title into a reused buffer (no per-call heap allocation)
	 *  and opens a per-draw GPU-capture marker (no Tracy zone). */
	template <class... Args>
	void BeginDrawEvent(std::format_string<Args...> fmt, Args&&... args)
	{
		thread_local std::string s_drawTitle;
		s_drawTitle.clear();
		std::format_to(std::back_inserter(s_drawTitle), fmt, std::forward<Args>(args)...);
		BeginDrawEvent(std::string_view{ s_drawTitle });
	}
	/** @brief Closes a BeginDrawEvent marker (RenderDoc/PIX only). */
	void EndDrawEvent();
	/** @brief Inserts a single-point GPU performance marker. */
	void SetPerfMarker(std::string_view title);
	/** @brief Formats the marker text into a reused buffer (no per-call heap
	 *  allocation) before inserting it. */
	template <class... Args>
	void SetPerfMarker(std::format_string<Args...> fmt, Args&&... args)
	{
		thread_local std::string s_markerText;
		s_markerText.clear();
		std::format_to(std::back_inserter(s_markerText), fmt, std::forward<Args>(args)...);
		SetPerfMarker(std::string_view{ s_markerText });
	}

	/// RenderDoc/PIX annotation only - no Tracy CPU zone.
	/// Used by ScopedGpuPass which manages its own Tracy CPU zone unconditionally.
	void BeginAnnotation(std::string_view title);
	void EndAnnotation();

	/** @brief Converts and stores the GPU adapter description from wide string. */
	void SetAdapterDescription(const std::wstring& description);

	bool frameAnnotations = false;

	// Multiplies ISRefraction.hlsl's heat-shimmer strength. 1.0 preserves current/vanilla
	// behavior; lower values reduce warping, 0 disables it.
	float refractionScale = 1.0f;

	// Pass D3DCOMPILE_PARTIAL_PRECISION to fxc. With explicit min16float types this is
	// mostly belt-and-braces in SM5, but it lets the compiler downgrade unmarked float
	// ops to FP16 where it can prove safety. On by default; toggle off when reversing
	// shaders or chasing a precision bug.
	// Atomic: written from the UI thread, read from compilation pool workers.
	std::atomic_bool enablePartialPrecision{ false };

	// Pass D3DCOMPILE_AVOID_FLOW_CONTROL to fxc. Forces the compiler to flatten branches
	// into predicated ops instead of using dynamic flow control. Can win on uniform-branch
	// or short-body branches; can lose on long divergent branches that vanilla flow
	// control would skip. Transient (session-only); not saved to config because the
	// right setting depends on the current scene/work, not the user.
	// Atomic: written from the UI thread, read from compilation pool workers.
	std::atomic_bool enableAvoidFlowControl{ false };

	uint lastVertexDescriptor = 0;
	uint lastPixelDescriptor = 0;
	uint modifiedVertexDescriptor = 0;
	uint modifiedPixelDescriptor = 0;
	uint lastModifiedVertexDescriptor = 0;
	uint lastModifiedPixelDescriptor = 0;
	uint lastExtraDescriptor = 0;
	uint lastExtraFeatureDescriptor = 0;

	/**
	 * Updates Lighting shader permutation state from the current render pass.
	 * @param a_pass The Lighting render pass whose blend state should be inspected.
	 */
	void UpdateLightingShaderPermutation(RE::BSRenderPass* a_pass);

	/**
	 * @brief Bitflags describing extra shader-specific properties.
	 * Upstream sequentially claims low bits for its own flags; keep every
	 * upstream-derived flag at upstream's own bit position so future syncs
	 * land without a collision. Fork-only flags go in the reserved high end
	 * (top bit down) instead, so they never compete with upstream's next one.
	 */
	enum class ExtraShaderDescriptors : uint32_t
	{
		InWorld = 1 << 0,
		IsReflections = 1 << 1,
		IsBeastRace = 1 << 2,
		GrassSphereNormal = 1 << 3,
		IsSun = 1 << 4,
		SuppressExternalEmittance = 1 << 5,
		AdditiveLighting = 1 << 6,
		// --- Open Shaders fork-only flags below: reserved high end, not upstream's sequence. ---
		TreeBend = 1u << 29,
		IsEye = 1u << 31
	};

	/** @brief Bitflags describing extra feature-specific properties related to terrain displacement and material models. */
	enum class ExtraFeatureDescriptors : uint32_t
	{
		THLand0HasDisplacement = 1 << 0,
		THLand1HasDisplacement = 1 << 1,
		THLand2HasDisplacement = 1 << 2,
		THLand3HasDisplacement = 1 << 3,
		THLand4HasDisplacement = 1 << 4,
		THLand5HasDisplacement = 1 << 5,
		ETMaterialModel = 0b111 << 6,
		THLandHasDisplacement = 1 << 9
	};

	bool inWorld = false;
	bool activeReflections = false;

	// Latched by Main_RenderWorld through frame end, so Post-time code can tell
	// a real scene rendered this frame even under a menu (e.g. VR Playroom).
	bool worldRenderedThisFrame = false;
	// Set after all DataLoaded work that can block startup rendering completes.
	std::atomic_bool startupMenuInitializationComplete{ false };
	// Set after the first complete Present following startup initialization.
	bool startupMenuBlurSourceReady = false;

	// Cached menu open states, updated once per frame in Reset().
	// Avoids repeated IsMenuOpen calls (each constructs a BSFixedString).
	bool isMainMenuOpen = false;
	bool isLoadingMenuOpen = false;
	bool isMapMenuOpen = false;
	bool isStatsMenuOpen = false;
	/** @brief Returns true if the cached main-menu or loading-menu state is open. */
	bool IsMainOrLoadingMenuOpen() const { return isMainMenuOpen || isLoadingMenuOpen; }
	/** @brief Returns true if main/loading menu is open, with a live fallback query via the UI pointer. */
	bool IsMainOrLoadingMenuOpen(RE::UI* ui) const
	{
		return IsMainOrLoadingMenuOpen() ||
		       (ui && (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)));
	}
	/** @brief Full-screen menus drawing their own art, which must not be graded by post-process effects. */
	bool IsFullScreenMenuOpen() const { return IsMainOrLoadingMenuOpen() || isMapMenuOpen || isStatsMenuOpen; }
	/** @brief Gameplay is paused or suspended behind a menu. Cached menus are kept explicit in case a mod clears kPausesGame. */
	bool IsPausedOrMenuOpen(RE::UI* ui) const
	{
		return (ui && ui->GameIsPaused()) || IsMainOrLoadingMenuOpen(ui) || isMapMenuOpen;
	}

	/**
	 * @brief Updates the shared constant buffer data based on world state and rendering pass.
	 * @param a_inWorld Whether the camera is in world space.
	 * @param a_prepass Whether this is a prepass rendering phase.
	 */
	void UpdateSharedData(bool a_inWorld, bool a_prepass);
	/**
	 * @brief Updates sky shader permutation based on the current render pass.
	 * @param a_pass The render pass to inspect.
	 */
	void UpdateSkyShaderPermutation(RE::BSRenderPass* a_pass);
	void UpdatePermutationBuffer();
	/**
	 * @brief Checks whether directional shadows are available for the current scene.
	 * @returns true if directional shadows are present, false otherwise.
	 */
	bool HasDirectionalShadows() const;

	struct PermutationCB
	{
		uint VertexShaderDescriptor;
		uint PixelShaderDescriptor;
		uint ExtraShaderDescriptor;
		uint ExtraFeatureDescriptor;

		float EffectRadius;
		float WindIntensityOverride;
		uint OverrideWindIntensity;
		float pad0;

		float TreeWindUpperBendRange;
		float TreeWindMaximumDisplacementPercent;
		float TreeBendModelSensitivity;
		float TreeLeafModelSensitivity;

		float TreeTransientWindInfluence;
		float TrunkWindBendSensitivity;
		float TreeLeafBaseWindFlutterGain;
		uint EnableAmbientGrassWind;

		float GrassWindBendProfile;
		float GrassWindFlutterStrength;
		float GrassWindFlutterFrequency;
		float GrassWindSensitivity;

		float TreeWindBoundsBase;
		float TreeWindBoundsHeight;
		float TreeWindTrunkGustInfluence;
		float TreeLeafGustInfluence;

		float TreeTransientMaximumBendMultiplier;
		float TreeLeafTransientWindInfluence;
		float TreeLeafTransientFlutterMaximum;
		float GrassWindCompressionToBend;

		float4 TreeWindProbeBase;
		float4 TreeWindProbeTop;

		bool operator==(const PermutationCB& other) const
		{
			return PixelShaderDescriptor == other.PixelShaderDescriptor &&
			       ExtraShaderDescriptor == other.ExtraShaderDescriptor &&
			       ExtraFeatureDescriptor == other.ExtraFeatureDescriptor && EffectRadius == other.EffectRadius &&
			       WindIntensityOverride == other.WindIntensityOverride &&
			       OverrideWindIntensity == other.OverrideWindIntensity &&
			       TreeWindUpperBendRange == other.TreeWindUpperBendRange &&
			       TreeWindMaximumDisplacementPercent == other.TreeWindMaximumDisplacementPercent &&
			       TreeBendModelSensitivity == other.TreeBendModelSensitivity &&
			       TreeLeafModelSensitivity == other.TreeLeafModelSensitivity &&
			       TreeTransientWindInfluence == other.TreeTransientWindInfluence &&
			       TreeLeafTransientWindInfluence == other.TreeLeafTransientWindInfluence &&
			       TreeLeafTransientFlutterMaximum == other.TreeLeafTransientFlutterMaximum &&
			       TreeTransientMaximumBendMultiplier == other.TreeTransientMaximumBendMultiplier &&
			       TrunkWindBendSensitivity == other.TrunkWindBendSensitivity &&
			       TreeLeafBaseWindFlutterGain == other.TreeLeafBaseWindFlutterGain &&
			       EnableAmbientGrassWind == other.EnableAmbientGrassWind &&
			       GrassWindSensitivity == other.GrassWindSensitivity &&
			       GrassWindBendProfile == other.GrassWindBendProfile &&
			       GrassWindCompressionToBend == other.GrassWindCompressionToBend &&
			       GrassWindFlutterStrength == other.GrassWindFlutterStrength &&
			       GrassWindFlutterFrequency == other.GrassWindFlutterFrequency &&
			       TreeWindBoundsBase == other.TreeWindBoundsBase &&
			       TreeWindBoundsHeight == other.TreeWindBoundsHeight &&
			       TreeWindTrunkGustInfluence == other.TreeWindTrunkGustInfluence &&
			       TreeLeafGustInfluence == other.TreeLeafGustInfluence &&
			       TreeWindProbeBase.x == other.TreeWindProbeBase.x &&
			       TreeWindProbeBase.y == other.TreeWindProbeBase.y &&
			       TreeWindProbeBase.z == other.TreeWindProbeBase.z &&
			       TreeWindProbeTop.x == other.TreeWindProbeTop.x &&
			       TreeWindProbeTop.y == other.TreeWindProbeTop.y &&
			       TreeWindProbeTop.z == other.TreeWindProbeTop.z;
		}
	};
	static_assert(offsetof(PermutationCB, EnableAmbientGrassWind) == 60);
	static_assert(offsetof(PermutationCB, GrassWindFlutterStrength) == 68);
	static_assert(offsetof(PermutationCB, TreeTransientMaximumBendMultiplier) == 96);
	static_assert(offsetof(PermutationCB, TreeLeafTransientWindInfluence) == 100);
	static_assert(offsetof(PermutationCB, TreeLeafTransientFlutterMaximum) == 104);
	static_assert(offsetof(PermutationCB, TreeWindProbeBase) == 112);
	static_assert(offsetof(PermutationCB, TreeWindProbeTop) == 128);
	static_assert(sizeof(PermutationCB) == 144);
	STATIC_ASSERT_ALIGNAS_16(PermutationCB);

	ConstantBuffer* permutationCB = nullptr;

	struct alignas(16) SharedDataCB
	{
		float4 WaterData[25];
		float4 DirLightDirection;
		float4 DirLightColor;
		float4 SunDirection;
		float4 SunColor;
		float4 MasserDirection;
		float4 MasserColor;
		float4 SecundaDirection;
		float4 SecundaColor;
		float4 CameraData;
		float4 BufferDim;
		float Timer;
		uint FrameCount;
		uint FrameCountAlwaysActive;
		uint InInterior;
		uint HasDirectionalShadows;
		uint InMapMenu;
		uint HideSky;
		float MipBias;
		float WaterSystemHeight;  // TES::GetWaterHeight at eye-0 in camera-relative Z; -NI_INFINITY when no water body found (VR only)
		float3 pad0;
		float4 AmbientSHR;
		float4 AmbientSHG;
		float4 AmbientSHB;
		float4 VRFoveationData0;          // x=center coverage scale, y=feather, z=horizontal scale, w=SSR raymarch mode (0 off, 1 feathered, 2 hard cutoff)
		float4 VRFoveationCenterOffsets;  // xy=left eye center offset, zw=right eye center offset
		float4 HDRData;                   // xyz + menu scene encoding in w — see HDRDisplay::GetSharedDataHDR
		float RefractionScale;            // ISRefraction.hlsl heat-shimmer multiplier; 1.0 = unmodified vanilla strength
		float3 pad1;
		WindField::WindTuning WindFieldTuning;
		float4 WindFieldAmbient;  // xyz: selected mean weather velocity, w: accumulated gust travel
		float4 WindFieldPreviousAmbient;
		WindField::Field WindFieldCurrent;
		WindField::Field WindFieldPrevious;
		WindField::Field WindFieldTransition;
		WindField::Field WindFieldPreviousTransition;
		float4 WindFieldTransitionData;  // x/y: current/previous blend, z/w: reserved
		float4 WindFieldSpringDebug;     // xy: field minimum, z: field size, w: maximum tilt radians
		std::array<uint32_t, 4> WindFieldActiveCounts;
		std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> WindFieldTransientImpulses;
		std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> WindFieldPreviousTransientImpulses;
	};
	STATIC_ASSERT_ALIGNAS_16(SharedDataCB);
	// Each float4 cbuffer field must start on a 16-byte boundary to match the HLSL SharedData
	// layout — a stray scalar inserted above would silently shift these and corrupt shader reads.
	static_assert(offsetof(SharedDataCB, VRFoveationData0) % 16 == 0);
	static_assert(offsetof(SharedDataCB, VRFoveationCenterOffsets) % 16 == 0);
	static_assert(offsetof(SharedDataCB, HDRData) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldTuning) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldAmbient) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldPreviousAmbient) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldCurrent) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldPrevious) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldTransition) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldPreviousTransition) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldTransitionData) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldSpringDebug) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldActiveCounts) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldTransientImpulses) % 16 == 0);
	static_assert(offsetof(SharedDataCB, WindFieldPreviousTransientImpulses) % 16 == 0);
	static_assert(sizeof(SharedDataCB) <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16);

	ConstantBuffer* sharedDataCB = nullptr;
	ConstantBuffer* featureDataCB = nullptr;

	PermutationCB permutationData{};
	PermutationCB permutationDataPrevious{};

	Util::FrameChecker frameChecker;
	uint frameCount = 0;
	// Thread-safe mirror of frameCount maintained by the render thread.
	// Off-thread readers (MCP listener, future telemetry) must read this
	// instead of touching frameCount directly to avoid a data race.
	std::atomic<uint32_t> frameCountAtomic{ 0 };

	// Skyrim constants
	float2 screenSize = {};
	D3D_FEATURE_LEVEL featureLevel;

	TracyD3D11Ctx tracyCtx = nullptr;  // Tracy context

	// Moon and Stars mod detection
	inline static bool moonAndStarsLoaded = false;

	void ClearDisabledFeatures();
	bool SetFeatureDisabled(const std::string& featureName, bool isDisabled);
	bool IsFeatureDisabled(const std::string& featureName);
	std::unordered_map<std::string, bool>& GetDisabledFeatures();

	bool useFrameAnnotations = false;

	// --- Utility Methods ---
	/**
	 * @brief Gets the total smoothed draw calls from the global state
	 * @return Total number of draw calls as float
	 */
	float GetTotalSmoothedDrawCalls() const;

	/**
	 * @brief Base helper that iterates through valid shader types (excluding None and Total)
	 * @param callback Function to call for each valid shader type with parameters: (type, typeIndex, classIndex)
	 */
	template <typename Callback>
	static void ForEachValidShaderType(Callback callback)
	{
		for (auto type : magic_enum::enum_values<RE::BSShader::Type>()) {
			if (type == RE::BSShader::Type::None || type == RE::BSShader::Type::Total)
				continue;
			int typeIndex = magic_enum::enum_integer(type);
			int classIndex = typeIndex - 1;
			callback(type, typeIndex, classIndex);
		}
	}

	/**
	 * @brief Iterates through valid shader types with performance metrics
	 * @param callback Function to call for each shader type with parameters: (type, typeIndex, drawCalls, frameTime, percent, costPerCall)
	 */
	template <typename Callback>
	static void ForEachShaderTypeWithMetrics(Callback callback)
	{
		ForEachValidShaderType([&](auto type, int typeIndex, [[maybe_unused]] int classIndex) {
			float drawCalls = static_cast<float>(GetSingleton()->smoothDrawCalls[typeIndex]);
			float frameTime = static_cast<float>(GetSingleton()->smoothFrameTimePerType[typeIndex]);
			float percent = (frameTime > 0.0f && GetSingleton()->smoothFrameTimePerType[magic_enum::enum_integer(RE::BSShader::Type::Total)] > 0.0f) ?
			                    (frameTime / GetSingleton()->smoothFrameTimePerType[magic_enum::enum_integer(RE::BSShader::Type::Total)] * 100.0f) :
			                    0.0f;
			float costPerCall = (drawCalls > 0.0f) ? (frameTime / drawCalls) : 0.0f;
			callback(type, typeIndex, drawCalls, frameTime, percent, costPerCall);
		});
	}

	/**
	 * @brief Iterates through valid shader types with class indices for UI operations
	 * @param callback Function to call for each shader type with parameters: (type, classIndex)
	 */
	template <typename Callback>
	static void ForEachShaderTypeWithIndex(Callback callback)
	{
		ForEachValidShaderType([&](auto type, [[maybe_unused]] int typeIndex, int classIndex) {
			callback(type, classIndex);
		});
	}

	std::unordered_map<std::string, bool> disabledFeatures;
	std::mutex m_mutex;

	inline ~State()
	{
#ifdef TRACY_ENABLE
		if (tracyCtx)
			TracyD3D11Destroy(tracyCtx);
#endif
	}

private:
	std::unordered_map<std::string, bool> favoriteFeatures;
	bool SaveFeaturePreference(const json& patch);
	void UpdateWind();
	void AdvanceWindHistory(float a_frameTime);
	void UpdateWeatherWind();
	void UpdateWindField(const float3& a_direction, float a_speed, float a_frameTime);
	void UpdateWindPermutationData();
	void UpdateWindSharedData(SharedDataCB& a_data) const;
	void UpdateTransientWindImpulses(float a_frameTime);
	struct ManagedTransientWindSource
	{
		WindField::TransientWindSource source;
		TransientWindSourceOwner owner;
		TransientWindSourcePriority priority;
		uint64_t sequence;
	};
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> transientWindImpulses{};
	std::array<WindField::TransientWindSource, WindField::kTransientImpulseCapacity> previousTransientWindImpulses{};
	std::array<TransientWindSourceOwner, WindField::kTransientImpulseCapacity> transientWindImpulseOwners{};
	std::array<TransientWindSourceOwner, WindField::kTransientImpulseCapacity> previousTransientWindImpulseOwners{};
	uint32_t activeTransientWindImpulseCount = 0;
	uint32_t previousActiveTransientWindImpulseCount = 0;
	std::vector<ManagedTransientWindSource> activeTransientWindSources;
	std::vector<ManagedTransientWindSource> pendingTransientWindSources;
	std::vector<ManagedTransientWindSource> attachedTransientWindSources;
	uint64_t transientWindSourceSequence = 0;
	std::mutex transientWindImpulseMutex;
	std::shared_ptr<REX::W32::ID3DUserDefinedAnnotation> pPerf;
	std::mutex statsMutex;
};
