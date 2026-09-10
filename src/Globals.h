#pragma once

#include <atomic>

struct CloudShadows;
struct CloudRelight;
struct DynamicCubemaps;
struct VolumetricShadows;
struct ExtendedMaterials;
struct GrassCollision;
struct GrassLighting;
struct FoliageLighting;
struct HairSpecular;
struct HorizonFix;
struct IBL;
struct LightLimitFix;
struct LinearLighting;
struct LODBlending;
struct InteriorSun;
struct InverseSquareLighting;
struct ScreenSpaceGI;
struct ScreenSpaceShadows;
struct Skylighting;
struct TerrainVariation;
struct SkySync;
struct SubsurfaceScattering;
struct TerrainBlending;
struct TerrainHelper;
struct TerrainShadows;
struct UnifiedWater;
struct VanillaFresnel;
struct VolumetricLighting;
struct VR;
struct WaterEffects;
struct SceneSelector;
struct PerformanceOverlay;
struct WetnessEffects;
struct ExtendedTranslucency;
struct Upscaling;
class Profiler;
struct CSEditor;
struct CSUtility;
struct Wind;
#if defined(ENABLE_EFFECTS11)
struct Effects11;
#endif
struct ExponentialHeightFog;
struct HDRDisplay;
struct PostProcessing;
struct ScreenshotFeature;
struct Skin;

class State;
class Deferred;
struct TruePBR;
class RenderDoc;
class RemoteControl;
class Menu;
class WeatherManager;
class SceneSettingsManager;

namespace SIE
{
	class ShaderCache;
	class ShaderFileDependencyTracker;
}

/**
 * @brief Initializes core singletons (ShaderCache, State, Menu, Deferred).
 */
void OnInit();

/**
 * @brief Resolves runtime game pointers, RTTI relocations, and D3D device references.
 */
void ReInit();

/**
 * @brief Caches late-binding game singletons (player, sky, INI settings).
 */
void OnDataLoaded();

/**
 * @brief Signals shader compilation to stop.
 */
void OnGameWindowClose();

/**
 * @brief Installs Detours hooks on the device context's Map/Unmap vtable slots to capture per-frame constant buffer data.
 * @param a_context The D3D11 device context to hook.
 */
void InstallD3DHooks(ID3D11DeviceContext* a_context);
namespace globals
{
	namespace d3d
	{
		extern ID3D11Device* device;
		extern ID3D11DeviceContext* context;
		extern IDXGISwapChain* swapChain;
	}

	namespace features
	{
		extern CloudShadows cloudShadows;
		extern CloudRelight cloudRelight;
		extern DynamicCubemaps dynamicCubemaps;
		extern VolumetricShadows volumetricShadows;
		extern ExtendedMaterials extendedMaterials;
		extern GrassCollision grassCollision;
		extern GrassLighting grassLighting;
		extern FoliageLighting foliageLighting;
		extern HairSpecular hairSpecular;
		extern HorizonFix horizonFix;
		extern IBL ibl;
		extern LightLimitFix lightLimitFix;
		extern LinearLighting linearLighting;
		extern LODBlending lodBlending;
		extern InteriorSun interiorSun;
		extern InverseSquareLighting inverseSquareLighting;
		extern ScreenSpaceGI screenSpaceGI;
		extern ScreenSpaceShadows screenSpaceShadows;
		extern Skylighting skylighting;
		extern TerrainVariation terrainVariation;
		extern SkySync skySync;
		extern SubsurfaceScattering subsurfaceScattering;
		extern TerrainBlending terrainBlending;
		extern TerrainHelper terrainHelper;
		extern TerrainShadows terrainShadows;
		extern UnifiedWater unifiedWater;
		extern VanillaFresnel vanillaFresnel;
		extern VolumetricLighting volumetricLighting;
		extern VR vr;
		extern WaterEffects waterEffects;
		extern SceneSelector sceneSelector;
		extern PerformanceOverlay performanceOverlay;
		extern WetnessEffects wetnessEffects;
		extern ExtendedTranslucency extendedTranslucency;
		extern Upscaling upscaling;
		extern HDRDisplay hdrDisplay;
#if defined(ENABLE_EFFECTS11)
		extern Effects11 effects11;
#endif
		extern RenderDoc renderDoc;
		extern RemoteControl remoteControl;
		extern ScreenshotFeature screenshotFeature;
		extern CSEditor csEditor;
		extern CSUtility csUtility;
		extern Wind wind;
		extern ExponentialHeightFog exponentialHeightFog;
		extern TruePBR truePBR;
		extern Skin skin;
		extern PostProcessing postProcessing;

		namespace llf
		{
			extern void** normalDepthBuffer;
			extern void** readOnlyDepthBuffer;
		}
	}

	/** @brief GPU constant buffer layout matching Skyrim's per-frame camera data. */
	struct FrameBuffer
	{
		Matrix CameraView;
		Matrix CameraProj;
		Matrix CameraViewProj;
		Matrix CameraViewProjUnjittered;
		Matrix CameraPreviousViewProjUnjittered;
		Matrix CameraProjUnjittered;
		Matrix CameraProjUnjitteredInverse;
		Matrix CameraViewInverse;
		Matrix CameraViewProjInverse;
		Matrix CameraProjInverse;
		float4 CameraPosAdjust;
		float4 CameraPreviousPosAdjust;
		float4 FrameParams;
		float4 DynamicResolutionParams1;
		float4 DynamicResolutionParams2;
	};

	struct FrameBufferVR
	{
		// Must match HLSL VR layout exactly - packoffsets c0 to c86
		Matrix CameraView[2];
		Matrix CameraProj[2];
		Matrix CameraViewProj[2];
		Matrix CameraViewProjUnjittered[2];
		Matrix CameraPreviousViewProjUnjittered[2];
		Matrix CameraProjUnjittered[2];
		Matrix CameraProjUnjitteredInverse[2];
		Matrix CameraViewInverse[2];
		Matrix CameraViewProjInverse[2];
		Matrix CameraProjInverse[2];
		float4 CameraPosAdjust[2];
		float4 CameraPreviousPosAdjust[2];
		float4 FrameParams;
		float4 DynamicResolutionParams1;
		float4 DynamicResolutionParams2;
	};

	union FrameBufferCache
	{
		FrameBuffer nonVR;
		FrameBufferVR vr;

		// Helper functions for VR-agnostic access to eye 0 (or single eye).
		// Keep raw module VR checks in this header block to avoid relying on game::isVR declaration order.
		const Matrix& GetCameraView(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraView[eyeIndex] : nonVR.CameraView;
		}
		const Matrix& GetCameraProj(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraProj[eyeIndex] : nonVR.CameraProj;
		}
		const Matrix& GetCameraViewProj(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraViewProj[eyeIndex] : nonVR.CameraViewProj;
		}
		const Matrix& GetCameraViewProjUnjittered(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraViewProjUnjittered[eyeIndex] : nonVR.CameraViewProjUnjittered;
		}
		const Matrix& GetCameraPreviousViewProjUnjittered(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraPreviousViewProjUnjittered[eyeIndex] : nonVR.CameraPreviousViewProjUnjittered;
		}
		const Matrix& GetCameraProjUnjittered(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraProjUnjittered[eyeIndex] : nonVR.CameraProjUnjittered;
		}
		const Matrix& GetCameraProjUnjitteredInverse(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraProjUnjitteredInverse[eyeIndex] : nonVR.CameraProjUnjitteredInverse;
		}
		const Matrix& GetCameraViewInverse(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraViewInverse[eyeIndex] : nonVR.CameraViewInverse;
		}
		const Matrix& GetCameraViewProjInverse(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraViewProjInverse[eyeIndex] : nonVR.CameraViewProjInverse;
		}
		const Matrix& GetCameraProjInverse(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraProjInverse[eyeIndex] : nonVR.CameraProjInverse;
		}
		const float4& GetCameraPosAdjust(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraPosAdjust[eyeIndex] : nonVR.CameraPosAdjust;
		}
		const float4& GetCameraPreviousPosAdjust(uint32_t eyeIndex = 0) const
		{
			return REL::Module::IsVR() ? vr.CameraPreviousPosAdjust[eyeIndex] : nonVR.CameraPreviousPosAdjust;
		}
		const float4& GetFrameParams() const
		{
			return REL::Module::IsVR() ? vr.FrameParams : nonVR.FrameParams;
		}
		const float4& GetDynamicResolutionParams1() const
		{
			return REL::Module::IsVR() ? vr.DynamicResolutionParams1 : nonVR.DynamicResolutionParams1;
		}
		const float4& GetDynamicResolutionParams2() const
		{
			return REL::Module::IsVR() ? vr.DynamicResolutionParams2 : nonVR.DynamicResolutionParams2;
		}
	};

	namespace game
	{
		extern RE::BSGraphics::RendererShadowState* shadowState;
		extern RE::BSGraphics::State* graphicsState;
		extern RE::BSGraphics::Renderer* renderer;
		extern RE::BSShaderManager::State* smState;
		extern RE::TES* tes;
		extern bool isVR;
		extern RE::MemoryManager* memoryManager;
		extern RE::INISettingCollection* iniSettingCollection;
		extern RE::INIPrefSettingCollection* iniPrefSettingCollection;
		extern RE::GameSettingCollection* gameSettingCollection;
		extern float* cameraNear;
		extern float* cameraFar;
		extern float* deltaTime;
		extern RE::BSUtilityShader* utilityShader;
		extern RE::PlayerCharacter* player;
		extern RE::PlayerCamera* playerCamera;
		extern RE::Sky* sky;
		extern RE::UI* ui;
		extern RE::Calendar* calendar;
		extern RE::ImageSpaceManager* imageSpaceManager;
		extern bool* bEnableVolumetricLighting;
		extern std::atomic<bool> quitGame;

		extern RE::BSGraphics::PixelShader** currentPixelShader;
		extern RE::BSGraphics::VertexShader** currentVertexShader;
		extern REX::EnumSet<RE::BSGraphics::ShaderFlags, uint32_t>* stateUpdateFlags;

		extern RE::Setting* bEnableLandFade;
		extern RE::Setting* bShadowsOnGrass;
		extern RE::Setting* shadowMaskQuarter;
		extern REL::Relocation<ID3D11Buffer**> perFrame;
		extern REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> currentAccumulator;

		extern D3D11_MAPPED_SUBRESOURCE* mappedFrameBuffer;
		extern FrameBufferCache frameBufferCached;

		extern int32_t* frameCounter;
		extern int* viewWidth;
		extern int* viewHeight;
		extern bool* drawStereo;
	}

	namespace rtti
	{
		extern REL::Relocation<const RE::NiRTTI*> NiIntegerExtraDataRTTI;
		extern REL::Relocation<const RE::NiRTTI*> BSLightingShaderPropertyRTTI;
		extern REL::Relocation<const RE::NiRTTI*> BSEffectShaderPropertyRTTI;
		extern REL::Relocation<const RE::NiRTTI*> BSWaterShaderPropertyRTTI;
		extern REL::Relocation<const RE::NiRTTI*> NiParticleSystemRTTI;
		extern REL::Relocation<const RE::NiRTTI*> NiBillboardNodeRTTI;
		extern REL::Relocation<const RE::NiRTTI*> NiAlphaPropertyRTTI;
		extern REL::Relocation<const RE::NiRTTI*> NiSourceTextureRTTI;
	}

	extern State* state;
	extern Deferred* deferred;
	extern Menu* menu;
	extern SIE::ShaderCache* shaderCache;
	extern Profiler* profiler;
	extern WeatherManager* weatherManager;
	extern SceneSettingsManager* sceneSettingsManager;

	/** @brief Initializes core singletons (ShaderCache, State, Menu, Deferred). Called once at plugin load. */
	void OnInit();
	/** @brief Resolves runtime game pointers, RTTI relocations, and D3D device references. Called when the renderer is ready. */
	void ReInit();
	/** @brief Caches late-binding game singletons (player, sky, INI settings) after Skyrim's data files are loaded. */
	void OnDataLoaded();
	/** @brief Signals shader compilation to stop when the game window is closing. */
	void OnGameWindowClose();
	/**
	 * @brief Installs Detours hooks on the device context's Map/Unmap vtable slots to capture per-frame constant buffer data.
	 * @param a_context The D3D11 device context to hook.
	 */
	void InstallD3DHooks(ID3D11DeviceContext* a_context);
}
