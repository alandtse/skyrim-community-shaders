#pragma once

#include "Effects/ENBAdaptation.h"
#include "Effects/ENBBloom.h"
#include "Effects/ENBEffect.h"
#include "Effects/ENBEffectPostPass.h"
#include "Effects/ENBLens.h"
#include "Profiler.h"
#include <unordered_map>

enum class TimeOfDay1Index : int
{
	Dawn,
	Sunrise,
	Day,
	Sunset
};

enum class TimeOfDay2Index : int
{
	Dusk,
	Night,
	InteriorDay,
	InteriorNight
};

enum class TimeOfDayFactorIndex : int
{
	Dawn,
	Sunrise,
	Day,
	Sunset,
	Dusk,
	Night,
	Count
};

class EffectManager
{
public:
	static EffectManager& GetSingleton();

	// Effect execution
	/** @brief Runs the effect chain from a_input into a_output.
		@return true only if a_output was written; false means the caller must fall back to the stock pass. */
	bool ExecuteEffects(RE::BSGraphics::RenderTargetData& a_input, RE::BSGraphics::RenderTargetData& a_output);

	// Lifecycle
	void Initialize();

	void Apply();
	void Load();
	void Save();

	void RegisterSettings();

	// Common variable management
	void UpdateCommonVariablesForEffect(Effect& effect);

public:
	ENBBloom enbBloom;
	ENBLens enbLens;
	ENBAdaptation enbAdaptation;
	ENBEffect enbEffect;
	ENBEffectPostPass enbEffectPostPass;

	// Common resources shared across effects
	void CreateCommonResources();

	// Shared D3D resources
	winrt::com_ptr<ID3D11Buffer> quadVertexBuffer;
	winrt::com_ptr<ID3D11InputLayout> inputLayout;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11BlendState> blendState;

	// Copy shader resources
	winrt::com_ptr<ID3D11VertexShader> copyVertexShader;
	winrt::com_ptr<ID3D11PixelShader> copyPixelShader;
	winrt::com_ptr<ID3D11Buffer> ditherConstantBuffer;

	// Color correction compute shader resources
	winrt::com_ptr<ID3D11ComputeShader> colorCorrectionComputeShader;
	winrt::com_ptr<ID3D11Buffer> colorCorrectionConstantBuffer;

	void CreateQuadGeometry();
	void CreateRenderStates();
	void CreateCopyShaders();
	void CreateColorCorrectionShader();

	void RenderEffectsList();

	// Common variable data (updated once, applied to all effects)
	struct CommonVariableData
	{
		float timer[4];
		float weather[4];
		float timeOfDay1[4];
		float timeOfDay2[4];
		float eNightDayFactor;
		float eInteriorFactor;
	} commonData;
	uint32_t frameCount = 0;

	void UpdateCommonData();

	struct SettingIDs
	{
		uint32_t useBloom = 0xFFFFFFFF;
		uint32_t useLens = 0xFFFFFFFF;
		uint32_t useAdaptation = 0xFFFFFFFF;
		uint32_t usePostPass = 0xFFFFFFFF;

		uint32_t enableMultipleWeathers = 0xFFFFFFFF;
		uint32_t enableLocationWeather = 0xFFFFFFFF;

		uint32_t nightTime = 0xFFFFFFFF;
		uint32_t sunriseTime = 0xFFFFFFFF;
		uint32_t dawnDuration = 0xFFFFFFFF;
		uint32_t dayTime = 0xFFFFFFFF;
		uint32_t sunsetTime = 0xFFFFFFFF;
		uint32_t duskDuration = 0xFFFFFFFF;

		uint32_t brightness = 0xFFFFFFFF;
		uint32_t gammaCurve = 0xFFFFFFFF;
	} ids;

	const CommonVariableData& GetCommonData() const { return commonData; }

	bool IsInitialized() const { return initialized; }

	/** @brief True when a usable preset is present; enbeffect.fx is required, so its absence means no preset.
		Effects11 must stay fully inert in that case, leaving the image untouched. */
	bool IsPresetLoaded() const { return enbEffect.IsCompiled(); }

	bool performanceMode = false;

	// Execute a single effect with perf events and common variable setup
	void ExecuteEffect(Effect& effect, uint32_t enableSettingID = 0xFFFFFFFF);

	// Texture copy using pixel shader
	void CopyTexture(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* destination);

	// -1 outside VR, else the eye ExecuteEffects's per-eye loop is rendering. Read by
	// GetTextureOriginal() and Effect::RenderPasses (viewport crop).
	int currentEyeIndex = -1;
	// This frame's eye-source width/height, cached once per ExecuteEffects call; lets
	// RenderPasses distinguish a full-width destination (crop it) from a fixed-size canvas.
	uint32_t currentMainWidth = 0;
	uint32_t currentMainHeight = 0;

	/** @brief Returns the "TextureOriginal" source: kMAIN outside VR, or the private
		per-eye crop refreshed by RefreshEyeSourceTexture() in VR, so eye-unaware .fx
		content samples the correct eye. */
	RE::BSGraphics::RenderTargetData& GetTextureOriginal();
	/** @brief Crop-copies kMAIN's a_eyeIndex half into GetTextureOriginal()'s backing texture.
		@param a_eyeIndex Eye to copy: 0 for left, 1 for right.
		@return false if the source texture/SRV was unavailable this frame (nothing copied). */
	bool RefreshEyeSourceTexture(int a_eyeIndex);

	/** @brief Same eye-crop as GetTextureOriginal(), for Effect::ExecuteTechniqueSequence's
		ping-pong read-back of a full-width intermediate a prior technique in the same
		sequence just wrote under a cropped viewport. No-op outside VR / non-full-width. */
	ID3D11ShaderResourceView* GetEyeCroppedSRV(TextureManager::Texture& a_source);

	/** @brief Same eye-crop as GetEyeCroppedSRV(), but for the depth-stencil source (TextureDepth),
		which can't be bound as a color RTV, so this writes into an R32_FLOAT scratch instead. */
	ID3D11ShaderResourceView* GetEyeCroppedDepthSRV(ID3D11Texture2D* a_sourceTexture, ID3D11ShaderResourceView* a_sourceSRV);

	// Color correction using compute shader
	void ApplyColorCorrection(ID3D11UnorderedAccessView* textureUAV);

	void ReloadShaders();

	// Error reporting for overlay display
	uint32_t GetFailedEffectCount() const;
	std::vector<std::string> GetAllErrors() const;

private:
	/** @brief Logs the resolved preset location, or why no preset is in use. */
	void LogPresetStatus() const;

	bool initialized = false;

	// GetTextureOriginal()'s VR backing texture; eyeSourceData mirrors these raw
	// pointers into the RenderTargetData shape kMAIN itself uses.
	winrt::com_ptr<ID3D11Texture2D> eyeSourceTexture;
	winrt::com_ptr<ID3D11RenderTargetView> eyeSourceRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> eyeSourceSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> eyeSourceUAV;
	RE::BSGraphics::RenderTargetData eyeSourceData{};
	winrt::com_ptr<ID3D11PixelShader> eyeCropCopyPS;
	bool eyeCropCopyPSCompileAttempted = false;
	winrt::com_ptr<ID3D11Buffer> eyeCropCB;

	// GetEyeCroppedSRV's backing textures, one per distinct source -- same-format sources
	// (e.g. RenderTargetRGBA64F, TextureLens) would otherwise overwrite each other's crop.
	struct CropTarget
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11RenderTargetView> rtv;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
	};
	std::unordered_map<ID3D11Texture2D*, CropTarget> inputCropTargets;
	// Canvas size inputCropTargets was last built against -- a resize reallocates the source
	// textures (new pointers), so stale entries must be dropped or they leak forever.
	uint32_t inputCropTargetsWidth = 0;
	uint32_t inputCropTargetsHeight = 0;

	// GetEyeCroppedDepthSRV's backing scratch (R32_FLOAT; the source format can't be RTV-bound).
	// A dedicated slot, not part of inputCropTargets: the engine exposes only one depth source.
	winrt::com_ptr<ID3D11Texture2D> depthCropTexture;
	winrt::com_ptr<ID3D11RenderTargetView> depthCropRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> depthCropSRV;
	winrt::com_ptr<ID3D11PixelShader> eyeCropCopyDepthPS;
	bool eyeCropCopyDepthPSCompileAttempted = false;

	// Shared draw dispatch for the crop callers; a_destRTV must be sized a_srcWidth/2 x a_srcHeight.
	// @return false on setup failure -- caller must fall back to the uncropped source.
	bool CropCopyEyeHalf(ID3D11ShaderResourceView* a_source, uint32_t a_srcWidth, uint32_t a_srcHeight, ID3D11RenderTargetView* a_destRTV, int a_eyeIndex, winrt::com_ptr<ID3D11PixelShader>& a_pixelShader, bool& a_pixelShaderCompileAttempted, const wchar_t* a_shaderPath);

	// a_overrideFormat overrides a_srcDesc.Format (needed for depth sources, which can't be
	// recreated as an RTV-bindable texture in their own format). @return false on failure.
	bool EnsureCropTarget(winrt::com_ptr<ID3D11Texture2D>& a_texture, winrt::com_ptr<ID3D11RenderTargetView>& a_rtv, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv, winrt::com_ptr<ID3D11UnorderedAccessView>* a_uav, const D3D11_TEXTURE2D_DESC& a_srcDesc, const char* a_debugName, DXGI_FORMAT a_overrideFormat = DXGI_FORMAT_UNKNOWN);
};
