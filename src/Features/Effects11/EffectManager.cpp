#include "EffectManager.h"

#include "D3D11StateBackup.h"
#include "Features/Effects11.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"

#include "PresetManager.h"
#include "SettingManager.h"
#include "TextureManager.h"
#include "Utils/D3D.h"
#include "WeatherManager.h"

#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
	using namespace Effects11Util;
}

EffectManager& EffectManager::GetSingleton()
{
	static EffectManager instance;
	return instance;
}

uint32_t EffectManager::GetFailedEffectCount() const
{
	uint32_t count = 0;
	const Effect* allEffects[] = { &enbBloom, &enbLens, &enbAdaptation, &enbEffect, &enbEffectPostPass };
	for (const auto* effect : allEffects)
		if (effect->IsFilePresent() && !effect->GetErrors().empty())
			count++;
	return count;
}

std::vector<std::string> EffectManager::GetAllErrors() const
{
	std::vector<std::string> result;
	const Effect* allEffects[] = { &enbBloom, &enbLens, &enbAdaptation, &enbEffect, &enbEffectPostPass };
	for (const auto* effect : allEffects)
		if (effect->IsFilePresent() && !effect->GetErrors().empty())
			for (const auto& err : effect->GetErrors())
				result.push_back(fmt::format("{}: {}", effect->GetName(), err));
	return result;
}

void EffectManager::Initialize()
{
	TextureManager::GetSingleton().Initialize();
	RegisterSettings();
	SettingManager::GetSingleton().Load();
	CreateCommonResources();
	Apply();

	// Verify all critical common resources are initialized correctly
	struct ResourceCheck
	{
		const void* resource;
		const char* name;
	};

	const ResourceCheck checks[] = {
		{ quadVertexBuffer.get(), "quadVertexBuffer" },
		{ inputLayout.get(), "inputLayout" },
		{ rasterizerState.get(), "rasterizerState" },
		{ blendState.get(), "blendState" },
		{ copyVertexShader.get(), "copyVertexShader" },
		{ copyPixelShader.get(), "copyPixelShader" },
		{ colorCorrectionComputeShader.get(), "colorCorrectionComputeShader" },
		{ colorCorrectionConstantBuffer.get(), "colorCorrectionConstantBuffer" },
		{ ditherConstantBuffer.get(), "ditherConstantBuffer" },
	};

	bool resourcesValid = true;
	for (const auto& [resource, name] : checks) {
		if (!resource) {
			logger::error("[EffectManager] {} failed to initialize", name);
			resourcesValid = false;
		}
	}

	if (!resourcesValid) {
		logger::error("[EffectManager] Initialization failed due to missing resources");
		initialized = false;
	} else {
		initialized = true;
	}
}

void EffectManager::LogPresetStatus() const
{
	auto& presetManager = PresetManager::GetSingleton();

	if (IsPresetLoaded()) {
		logger::info("[EFFECTS11] Preset loaded from '{}', settings from '{}'",
			presetManager.GetENBSeriesPath().string(),
			presetManager.GetENBSeriesIniPath().string());
		return;
	}

	// Effects11 stays inert without a preset, so make the reason findable
	if (!enbEffect.IsFilePresent()) {
		logger::warn("[EFFECTS11] No preset in use: '{}' not found", enbEffect.GetFilePath().string());
	} else {
		const auto& errors = enbEffect.GetErrors();
		logger::error("[EFFECTS11] No preset in use: '{}' failed to load: {}", enbEffect.GetFilePath().string(),
			errors.empty() ? "unknown error" : errors.back());
	}
}

void EffectManager::Apply()
{
	globals::features::effects11.LoadRaindropTexture();

	enbBloom.Apply();
	enbLens.Apply();
	enbAdaptation.Apply();
	enbEffect.Apply();
	enbEffectPostPass.Apply();
	LogPresetStatus();
}

void EffectManager::Load()
{
	Effect* allEffects[] = { &enbBloom, &enbLens, &enbAdaptation, &enbEffect, &enbEffectPostPass };
	for (auto* effect : allEffects) {
		effect->Load();
		effect->UpdateUIVariables();
	}
}

void EffectManager::Save()
{
	enbBloom.Save();
	enbLens.Save();
	enbAdaptation.Save();
	enbEffect.Save();
	enbEffectPostPass.Save();
}

void EffectManager::RegisterSettings()
{
	auto& settingManager = SettingManager::GetSingleton();

	settingManager.RegisterBoolSetting("UseEffect", "GLOBAL", false, false);

	settingManager.RegisterBoolSetting("UseOriginalPostProcessing", "EFFECT", false, false);
	settingManager.RegisterBoolSetting("EnableAdaptation", "EFFECT", false, false);
	settingManager.RegisterBoolSetting("EnableBloom", "EFFECT", false, false);
	settingManager.RegisterBoolSetting("EnableLens", "EFFECT", false, false);
	settingManager.RegisterBoolSetting("EnablePostPassShader", "EFFECT", false, false);
	settingManager.RegisterBoolSetting("EnableProceduralSun", "EFFECT", false, false);
	settingManager.RegisterBoolSetting("EnableCloudShadows", "EFFECT", false, false);
	settingManager.RegisterBoolSetting("EnableImageBasedLighting", "EFFECT", false, false);
	settingManager.RegisterBoolSetting("EnableVolumetricRays", "EFFECT", false, false);

	settingManager.RegisterFloatSetting("Brightness", "COLORCORRECTION", 1.0f, 0.0f, 10000.0f, 0.01f, false);
	settingManager.RegisterFloatSetting("GammaCurve", "COLORCORRECTION", 1.0f, 1.0f, 2.2f, 0.01f, false);

	settingManager.RegisterBoolSetting("EnableMultipleWeathers", "WEATHER", false, false);
	settingManager.RegisterBoolSetting("EnableLocationWeather", "WEATHER", false, false);

	settingManager.RegisterFloatSetting("DawnDuration", "TIMEOFDAY", 2.0f, 0.1f, 6.0f, 0.01f, false);
	settingManager.RegisterFloatSetting("SunriseTime", "TIMEOFDAY", 7.0f, 2.0f, 12.0f, 0.01f, false);
	settingManager.RegisterFloatSetting("DayTime", "TIMEOFDAY", 13.0f, 0.0f, 24.0f, 0.01f, false);
	settingManager.RegisterFloatSetting("SunsetTime", "TIMEOFDAY", 19.0f, 0.0f, 23.0f, 0.01f, false);
	settingManager.RegisterFloatSetting("DuskDuration", "TIMEOFDAY", 2.0f, 0.1f, 6.0f, 0.01f, false);
	settingManager.RegisterFloatSetting("NightTime", "TIMEOFDAY", 1.0f, 0.0f, 24.0f, 0.01f, false);

	settingManager.RegisterFloatSetting("AdaptationSensitivity", "ADAPTATION", 0.5f, 0.0f, 1.0f, 0.01f, false);
	settingManager.RegisterFloatSetting("AdaptationTime", "ADAPTATION", 1.0f, 0.05f, 100.0f, 0.01f, false);
	settingManager.RegisterBoolSetting("ForceMinMaxValues", "ADAPTATION", false, false);
	settingManager.RegisterFloatSetting("AdaptationMin", "ADAPTATION", 0.1f, 0.0f, 65536.0f, 0.01f, false);
	settingManager.RegisterFloatSetting("AdaptationMax", "ADAPTATION", 10.0f, 0.0f, 65536.0f, 0.01f, false);

	settingManager.RegisterTimeOfDaySetting("FireIntensity", "FIRE", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("FireCurve", "FIRE", 1.0f, 0.1f, 8.0f, 0.01f, true);

	settingManager.RegisterTimeOfDaySetting("Amount", "BLOOM", 0.1f, 0.0f, 10.0f, 0.01f, true);

	settingManager.RegisterTimeOfDaySetting("Amount", "LENS", 1.0f, 0.0f, 10.0f, 0.01f, true);

	settingManager.RegisterTimeOfDaySetting("DirectLightingIntensity", "ENVIRONMENT", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("DirectLightingCurve", "ENVIRONMENT", 1.0f, 0.1f, 8.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("DirectLightingDesaturation", "ENVIRONMENT", 0.0f, -1.0f, 1.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("AmbientLightingIntensity", "ENVIRONMENT", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("AmbientLightingDesaturation", "ENVIRONMENT", 0.0f, -1.0f, 1.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("PointLightingIntensity", "ENVIRONMENT", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("PointLightingCurve", "ENVIRONMENT", 1.0f, 0.1f, 4.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("PointLightingDesaturation", "ENVIRONMENT", 0.0f, -1.0f, 1.0f, 0.01f, true);
	settingManager.RegisterColorTimeOfDaySetting("DirectLightingColorFilter", "ENVIRONMENT", { 1.0f, 1.0f, 1.0f }, true);
	settingManager.RegisterTimeOfDaySetting("DirectLightingColorFilterAmount", "ENVIRONMENT", 0.0f, 0.0f, 1.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("FogColorMultiplier", "ENVIRONMENT", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("FogColorCurve", "ENVIRONMENT", 1.0f, 0.0f, 8.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("FogAmountMultiplier", "ENVIRONMENT", 1.0f, 0.0f, 10.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("FogCurveMultiplier", "ENVIRONMENT", 1.0f, 0.0f, 10.0f, 0.01f, true);
	settingManager.RegisterColorTimeOfDaySetting("FogColorFilter", "ENVIRONMENT", { 1.0f, 1.0f, 1.0f }, true);
	settingManager.RegisterTimeOfDaySetting("FogColorFilterAmount", "ENVIRONMENT", 0.0f, 0.0f, 1.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("ColorPow", "ENVIRONMENT", 1.0f, 1.0f, 2.2f, 0.01f, true);

	settingManager.RegisterBoolSetting("DisableWrongSkyMath", "SKY", false, false);
	settingManager.RegisterTimeOfDaySetting("GradientIntensity", "SKY", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("GradientDesaturation", "SKY", 0.0f, -1.0f, 1.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("GradientTopIntensity", "SKY", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("GradientTopCurve", "SKY", 1.0f, 0.1f, 8.0f, 0.01f, true);
	settingManager.RegisterColorTimeOfDaySetting("GradientTopColorFilter", "SKY", { 1.0f, 1.0f, 1.0f }, true);
	settingManager.RegisterTimeOfDaySetting("GradientMiddleIntensity", "SKY", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("GradientMiddleCurve", "SKY", 1.0f, 0.1f, 8.0f, 0.01f, true);
	settingManager.RegisterColorTimeOfDaySetting("GradientMiddleColorFilter", "SKY", { 1.0f, 1.0f, 1.0f }, true);
	settingManager.RegisterTimeOfDaySetting("GradientHorizonIntensity", "SKY", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("GradientHorizonCurve", "SKY", 1.0f, 0.1f, 8.0f, 0.01f, true);
	settingManager.RegisterColorTimeOfDaySetting("GradientHorizonColorFilter", "SKY", { 1.0f, 1.0f, 1.0f }, true);
	settingManager.RegisterTimeOfDaySetting("CloudsIntensity", "SKY", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("CloudsCurve", "SKY", 1.0f, 0.1f, 8.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("CloudsDesaturation", "SKY", 0.0f, -1.0f, 1.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("CloudsOpacity", "SKY", 1.0f, 0.0f, 5.0f, 0.01f, true);
	settingManager.RegisterColorTimeOfDaySetting("CloudsColorFilter", "SKY", { 1.0f, 1.0f, 1.0f }, true);
	settingManager.RegisterTimeOfDaySetting("SunIntensity", "SKY", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("SunDesaturation", "SKY", 0.0f, -1.0f, 1.0f, 0.01f, true);
	settingManager.RegisterColorTimeOfDaySetting("SunColorFilter", "SKY", { 1.0f, 1.0f, 1.0f }, true);
	settingManager.RegisterTimeOfDaySetting("MoonIntensity", "SKY", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("MoonDesaturation", "SKY", 0.0f, -1.0f, 1.0f, 0.01f, true);
	settingManager.RegisterColorTimeOfDaySetting("MoonColorFilter", "SKY", { 1.0f, 1.0f, 1.0f }, true);
	settingManager.RegisterTimeOfDaySetting("StarsIntensity", "SKY", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterFloatSetting("CloudsEdgeIntensity", "SKY", 2.0f, 0.0f, 10.0f, 0.01f, false);
	settingManager.RegisterFloatSetting("CloudsEdgeMoonMultiplier", "SKY", 0.0f, 0.0f, 10.0f, 0.01f, false);
	settingManager.RegisterBoolSetting("UseProceduralGradientWeights", "SKY", false, false);
	settingManager.RegisterTimeOfDaySetting("ProceduralGradientWeightCurve", "SKY", 4.0f, 1.0f, 32.0f, 0.01f, true);

	settingManager.RegisterFloatSetting("Size", "PROCEDURALSUN", 1.0f, 0.0f, 12.0f, 0.01f, false);
	settingManager.RegisterFloatSetting("EdgeSoftness", "PROCEDURALSUN", 0.4f, 0.0f, 1.0f, 0.01f, false);
	settingManager.RegisterTimeOfDaySetting("GlowIntensity", "PROCEDURALSUN", 0.4f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("GlowCurve", "PROCEDURALSUN", 10.0f, 0.0f, 100.0f, 0.01f, true);

	settingManager.RegisterTimeOfDaySetting("Intensity", "VOLUMETRICFOG", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterColorTimeOfDaySetting("ColorFilter", "VOLUMETRICFOG", { 1.0f, 1.0f, 1.0f }, true);

	settingManager.RegisterTimeOfDaySetting("MultiplicativeAmount", "IMAGEBASEDLIGHTING", 0.0f, 0.0f, 10.0f, 0.01f, true);

	settingManager.RegisterTimeOfDaySetting("GlowIntensity", "SUNGLARE", 1.0f, 0.0f, 1000.0f, 0.01f, true);

	settingManager.RegisterTimeOfDaySetting("Intensity", "PARTICLE", 1.0f, 0.0f, 30000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("LightingInfluence", "PARTICLE", 0.5f, 0.0f, 10.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("AmbientInfluence", "PARTICLE", 0.5f, 0.0f, 10.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("PointLightingInfluence", "PARTICLE", 1.0f, 0.0f, 10.0f, 0.01f, true);

	settingManager.RegisterTimeOfDaySetting("Intensity", "LIGHTSPRITE", 1.0f, 0.0f, 30000.0f, 0.01f, true);

	settingManager.RegisterTimeOfDaySetting("MotionStretch", "RAIN", 0.28f, 0.0f, 1.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("MotionTransparency", "RAIN", 0.1f, 0.0f, 1.0f, 0.01f, true);

	settingManager.RegisterTimeOfDaySetting("Amount", "CLOUDSHADOWS", 0.5f, 0.0f, 4.0f, 0.01f, true);

	settingManager.RegisterTimeOfDaySetting("Intensity", "GAMEVOLUMETRICRAYS", 1.0f, 0.0f, 1000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("RangeFactor", "GAMEVOLUMETRICRAYS", 1.0f, 0.0f, 100.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("Desaturation", "GAMEVOLUMETRICRAYS", 0.0f, -1.0f, 1.0f, 0.01f, true);
	settingManager.RegisterColorTimeOfDaySetting("ColorFilter", "GAMEVOLUMETRICRAYS", { 1.0f, 1.0f, 1.0f }, true);

	settingManager.RegisterTimeOfDaySetting("Intensity", "VOLUMETRICRAYS", 0.2f, 0.0f, 1000.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("Density", "VOLUMETRICRAYS", 1.0f, 0.1f, 100.0f, 0.01f, true);
	settingManager.RegisterTimeOfDaySetting("SkyColorAmount", "VOLUMETRICRAYS", 0.5f, 0.0f, 10.0f, 0.01f, true);

	settingManager.SetCategoryDependency("BLOOM", "EnableBloom", "EFFECT");
	settingManager.SetCategoryDependency("LENS", "EnableLens", "EFFECT");
	settingManager.SetCategoryDependency("ADAPTATION", "EnableAdaptation", "EFFECT");
	settingManager.SetCategoryDependency("PROCEDURALSUN", "EnableProceduralSun", "EFFECT");
	settingManager.SetCategoryDependency("CLOUDSHADOWS", "EnableCloudShadows", "EFFECT");
	settingManager.SetCategoryDependency("IMAGEBASEDLIGHTING", "EnableImageBasedLighting", "EFFECT");
	settingManager.SetCategoryDependency("VOLUMETRICRAYS", "EnableVolumetricRays", "EFFECT");

	settingManager.SetSettingDependency("ProceduralGradientWeightCurve", "SKY", "UseProceduralGradientWeights", "SKY");
	settingManager.SetSettingDependency("AdaptationMin", "ADAPTATION", "ForceMinMaxValues", "ADAPTATION");
	settingManager.SetSettingDependency("AdaptationMax", "ADAPTATION", "ForceMinMaxValues", "ADAPTATION");

	settingManager.SetCategoryExteriorOnly("RAIN", true);
	settingManager.SetCategoryExteriorOnly("SKYLIGHTING", true);
	settingManager.SetCategoryExteriorOnly("CLOUDSHADOWS", true);
	settingManager.SetCategoryExteriorOnly("IMAGEBASEDLIGHTING", true);
	settingManager.SetCategoryExteriorOnly("VOLUMETRICRAYS", true);
	settingManager.SetCategoryExteriorOnly("VOLUMETRICFOG", true);
	settingManager.SetCategoryExteriorOnly("GAMEVOLUMETRICRAYS", true);

	// Cache IDs for performance
	ids.useBloom = settingManager.GetSettingID("EnableBloom", "EFFECT");
	ids.useLens = settingManager.GetSettingID("EnableLens", "EFFECT");
	ids.useAdaptation = settingManager.GetSettingID("EnableAdaptation", "EFFECT");
	ids.usePostPass = settingManager.GetSettingID("EnablePostPassShader", "EFFECT");

	ids.enableMultipleWeathers = settingManager.GetSettingID("EnableMultipleWeathers", "WEATHER");
	ids.enableLocationWeather = settingManager.GetSettingID("EnableLocationWeather", "WEATHER");

	ids.nightTime = settingManager.GetSettingID("NightTime", "TIMEOFDAY");
	ids.sunriseTime = settingManager.GetSettingID("SunriseTime", "TIMEOFDAY");
	ids.dawnDuration = settingManager.GetSettingID("DawnDuration", "TIMEOFDAY");
	ids.dayTime = settingManager.GetSettingID("DayTime", "TIMEOFDAY");
	ids.sunsetTime = settingManager.GetSettingID("SunsetTime", "TIMEOFDAY");
	ids.duskDuration = settingManager.GetSettingID("DuskDuration", "TIMEOFDAY");

	ids.brightness = settingManager.GetSettingID("Brightness", "COLORCORRECTION");
	ids.gammaCurve = settingManager.GetSettingID("GammaCurve", "COLORCORRECTION");
}

void EffectManager::ExecuteEffect(Effect& a_effect, uint32_t enableSettingID)
{
	if (!a_effect.IsCompiled())
		return;

	if (enableSettingID != 0xFFFFFFFF && !SettingManager::GetSingleton().GetValue<bool>(enableSettingID))
		return;

	CS_GPU_PASS_DYNAMIC("Effects11::" + a_effect.GetName());

	a_effect.profiler = globals::profiler;
	UpdateCommonVariablesForEffect(a_effect);
	a_effect.UpdateEffectVariables();
	a_effect.Execute();
	a_effect.profiler = nullptr;
}

bool EffectManager::ExecuteEffects(RE::BSGraphics::RenderTargetData& a_input, RE::BSGraphics::RenderTargetData& a_output)
{
	if (!initialized)
		return false;

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	if (!rasterizerState || !blendState || !quadVertexBuffer || !inputLayout || !renderer)
		return false;

	// Without a preset nothing writes TextureSDRTemp, so the output RT would be copied from a
	// never-written texture, i.e. a black screen
	if (!IsPresetLoaded())
		return false;

	CS_GPU_PASS("Effects11::ExecuteEffects");

	D3D11FullStateBackup stateBackup;
	stateBackup.Save(context);

	// Mirror into kMAIN once; skip under PerfMode or it discards the DLSS/FSR+RCAS output.
	const bool perfModeDrivingThisFrame = globals::features::upscaling.perfMode.IsHookActive();
	auto& kMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!perfModeDrivingThisFrame && &a_input != &kMain && a_input.SRV && kMain.RTV) {
		D3D11_TEXTURE2D_DESC srcDesc{}, dstDesc{};
		if (a_input.texture && kMain.texture) {
			a_input.texture->GetDesc(&srcDesc);
			kMain.texture->GetDesc(&dstDesc);
		}
		if (a_input.texture && kMain.texture && srcDesc.Format == dstDesc.Format && srcDesc.Width == dstDesc.Width && srcDesc.Height == dstDesc.Height && srcDesc.SampleDesc.Count == dstDesc.SampleDesc.Count) {
			context->CopyResource(kMain.texture, a_input.texture);
		} else {
			CopyTexture(a_input.SRV, kMain.RTV);
			ID3D11RenderTargetView* nullRTV = nullptr;
			context->OMSetRenderTargets(1, &nullRTV, nullptr);
		}
	}

	auto& textureManager = TextureManager::GetSingleton();

	// Guards against an exception unwinding out of this scope and leaving currentEyeIndex
	// stuck >= 0 for every later call this session.
	struct EyeIndexResetGuard
	{
		int& index;
		~EyeIndexResetGuard() { index = -1; }
	} eyeIndexResetGuard{ currentEyeIndex };

	// Returns false on a missing source texture so the caller aborts instead of compositing
	// a partially-populated output.
	auto runEffectsPass = [&]() -> bool {
		auto& textureOriginal = GetTextureOriginal();
		if (currentEyeIndex >= 0) {
			if (!RefreshEyeSourceTexture(currentEyeIndex))
				return false;
			// No-op unless the source resolution actually changed (e.g. PerfMode's DisplayRes
			// testTexture vs. kMAIN's renderRes, or a quality-mode change mid-session).
			textureManager.EnsureSize(currentMainWidth, currentMainHeight);
			if (currentMainWidth != inputCropTargetsWidth || currentMainHeight != inputCropTargetsHeight) {
				inputCropTargets.clear();
				inputCropTargetsWidth = currentMainWidth;
				inputCropTargetsHeight = currentMainHeight;
			}
		}

		// Set our render state
		context->RSSetState(rasterizerState.get());
		context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);
		context->OMSetDepthStencilState(nullptr, 0);

		UINT stride = sizeof(float) * 5;
		UINT offset = 0;
		ID3D11Buffer* vertexBuffers[] = { quadVertexBuffer.get() };
		context->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
		context->IASetInputLayout(inputLayout.get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

		globals::profiler->BeginPass("Effects11::ColorCorrection");
		ApplyColorCorrection(textureOriginal.UAV);
		globals::profiler->EndPass();

		textureManager.UpdateDownsampledTexture(textureOriginal.SRV);

		ExecuteEffect(enbBloom, ids.useBloom);
		ExecuteEffect(enbLens, ids.useLens);
		ExecuteEffect(enbEffect);
		ExecuteEffect(enbEffectPostPass, ids.usePostPass);

		return true;
	};

	const int eyeCount = globals::game::isVR ? 2 : 1;
	bool allEyesSucceeded = true;
	for (int eye = 0; eye < eyeCount && allEyesSucceeded; ++eye) {
		currentEyeIndex = globals::game::isVR ? eye : -1;
		allEyesSucceeded = runEffectsPass();
	}

	if (!allEyesSucceeded) {
		stateBackup.Restore(context);
		stateBackup.Release();
		return false;
	}

	// Once per frame, not per eye: doubling this per eye corrupts the temporal filter (eye 1
	// would read eye 0's just-written output as "previous").
	ExecuteEffect(enbAdaptation, ids.useAdaptation);
	textureManager.IncrementTextureSwap();

	auto* textureSDRTemp = textureManager.GetCommonTexture("TextureSDRTemp");
	const bool wroteOutput = textureSDRTemp && a_output.RTV;
	if (wroteOutput) {
		globals::profiler->BeginPass("Effects11::CopyToOutput");
		CopyTexture(textureSDRTemp->srv.get(), a_output.RTV);
		globals::profiler->EndPass();
	}

	stateBackup.Restore(context);
	stateBackup.Release();

	return wroteOutput;
}

void EffectManager::CreateCommonResources()
{
	CreateQuadGeometry();
	CreateRenderStates();
	CreateCopyShaders();
	CreateColorCorrectionShader();
}

void EffectManager::CreateQuadGeometry()
{
	// Create a fullscreen quad vertex buffer that all effects can share
	struct QuadVertex
	{
		float position[3];
		float texCoord[2];
	};

	QuadVertex vertices[] = {
		{ { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },  // Bottom left
		{ { -1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },   // Top left
		{ { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } },   // Bottom right
		{ { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } }     // Top right
	};

	D3D11_BUFFER_DESC bufferDesc = {};
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.ByteWidth = sizeof(vertices);
	bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bufferDesc.CPUAccessFlags = 0;

	D3D11_SUBRESOURCE_DATA initData = {};
	initData.pSysMem = vertices;

	DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&bufferDesc, &initData, quadVertexBuffer.put()));

	// Create input layout for ENB post-processing
	D3D11_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};

	auto vertexShaderBlob = Util::CompileShaderBlob(L"Data\\Shaders\\Effects11\\QuadVS.hlsl", {}, "vs_4_0");
	if (!vertexShaderBlob) {
		logger::error("[EFFECTS11] Failed to compile input layout vertex shader");
		return;
	}

	HRESULT hr = globals::d3d::device->CreateInputLayout(inputElementDescs, ARRAYSIZE(inputElementDescs),
		vertexShaderBlob->GetBufferPointer(),
		vertexShaderBlob->GetBufferSize(),
		inputLayout.put());
	if (FAILED(hr)) {
		logger::error("[EFFECTS11] Failed to create shared input layout for ENB effects");
	}
}

void EffectManager::CreateRenderStates()
{
	// Rasterizer state for fullscreen quads
	D3D11_RASTERIZER_DESC rastDesc = {};
	rastDesc.FillMode = D3D11_FILL_SOLID;
	rastDesc.CullMode = D3D11_CULL_NONE;
	rastDesc.FrontCounterClockwise = FALSE;
	rastDesc.DepthBias = 0;
	rastDesc.DepthBiasClamp = 0.0f;
	rastDesc.SlopeScaledDepthBias = 0.0f;
	rastDesc.DepthClipEnable = TRUE;
	rastDesc.ScissorEnable = FALSE;
	rastDesc.MultisampleEnable = FALSE;
	rastDesc.AntialiasedLineEnable = FALSE;

	DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&rastDesc, rasterizerState.put()));

	// Blend state for standard rendering (no blending)
	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.AlphaToCoverageEnable = FALSE;
	blendDesc.IndependentBlendEnable = FALSE;
	blendDesc.RenderTarget[0].BlendEnable = FALSE;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

	DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&blendDesc, blendState.put()));
}

void EffectManager::CreateCopyShaders()
{
	copyVertexShader.attach(static_cast<ID3D11VertexShader*>(
		Util::CompileShader(L"Data\\Shaders\\Effects11\\QuadVS.hlsl", {}, "vs_5_0")));
	if (!copyVertexShader) {
		logger::error("[EFFECTS11] Failed to compile copy vertex shader");
		return;
	}

	copyPixelShader.attach(static_cast<ID3D11PixelShader*>(
		Util::CompileShader(L"Data\\Shaders\\Effects11\\CopyPS.hlsl", {}, "ps_5_0")));
	if (!copyPixelShader) {
		logger::error("[EFFECTS11] Failed to compile copy pixel shader");
		return;
	}

	D3D11_BUFFER_DESC cbDesc{};
	cbDesc.ByteWidth = 16;
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	globals::d3d::device->CreateBuffer(&cbDesc, nullptr, ditherConstantBuffer.put());

	logger::info("[EFFECTS11] Created texture copy shaders successfully");
}

void EffectManager::CreateColorCorrectionShader()
{
	colorCorrectionComputeShader.attach(static_cast<ID3D11ComputeShader*>(
		Util::CompileShader(L"Data\\Shaders\\Effects11\\ColorCorrectionCS.hlsl", {}, "cs_5_0")));
	if (!colorCorrectionComputeShader) {
		logger::error("[EFFECTS11] Failed to compile color correction compute shader");
		return;
	}

	// Create constant buffer
	D3D11_BUFFER_DESC cbDesc = {};
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.ByteWidth = sizeof(float) * 4;  // Brightness, GammaCurve, padding[2]
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	HRESULT hr = globals::d3d::device->CreateBuffer(&cbDesc, nullptr, colorCorrectionConstantBuffer.put());
	if (FAILED(hr)) {
		logger::error("[EFFECTS11] Failed to create color correction constant buffer");
		return;
	}

	logger::info("[EFFECTS11] Created color correction compute shader successfully");
}

void EffectManager::UpdateCommonData()
{
	commonData = {};

	auto sky = globals::game::sky;

	// Update timer
	{
		auto delta = (*globals::game::deltaTime);

		static double timer = 0.0f;
		timer += delta;

		auto modifiedTimer = std::fmodf(static_cast<float>(timer) * 1000.0f, 16777216);
		modifiedTimer /= 16777216.0f;

		commonData.timer[0] = modifiedTimer;
		commonData.timer[1] = 60.0f;
		commonData.timer[2] = static_cast<float>(frameCount % 9999);
		commonData.timer[3] = delta;

		frameCount++;
	}

	// Update weather
	{
		// Strip plugin index (2 leftmost digits) from form IDs
		auto stripPluginIndex = [](uint32_t formID) -> uint32_t {
			return formID & 0x00FFFFFF;  // Keep only the lower 6 hex digits
		};

		if (sky) {
			auto& weatherManager = WeatherManager::GetSingleton();
			uint32_t currentID = sky->currentWeather ? stripPluginIndex(sky->currentWeather->formID) : 0;
			uint32_t lastID = sky->lastWeather ? stripPluginIndex(sky->lastWeather->formID) : 0;

			commonData.weather[0] = static_cast<float>(weatherManager.GetEffectiveWeatherID(currentID));
			commonData.weather[1] = static_cast<float>(weatherManager.GetEffectiveWeatherID(lastID));
			commonData.weather[2] = sky->currentWeatherPct;
			commonData.weather[3] = sky->currentGameHour;
		}
	}

	// Update time of day
	{
		auto& settingManager = SettingManager::GetSingleton();

		// Clamp current time to valid range
		float currentTime = sky ? std::clamp(sky->currentGameHour, 0.0f, 24.0f) : 12.0f;

		// Load time of day settings using cached IDs
		const float nightTime = settingManager.GetValue<float>(ids.nightTime);
		const float sunriseTime = settingManager.GetValue<float>(ids.sunriseTime);
		const float dawnDuration = settingManager.GetValue<float>(ids.dawnDuration);
		const float dayTime = settingManager.GetValue<float>(ids.dayTime);
		const float sunsetTime = settingManager.GetValue<float>(ids.sunsetTime);
		const float duskDuration = settingManager.GetValue<float>(ids.duskDuration);

		commonData.eInteriorFactor = Util::IsInterior();

		// Initialize and set factors
		float factors[static_cast<int>(TimeOfDayFactorIndex::Count)] = { 0.0f };

		if (!commonData.eInteriorFactor) {
			// Calculate transition points
			const float dawnStart = sunriseTime - dawnDuration;
			const float dawnMid = sunriseTime - (dawnDuration * 0.5f);
			const float duskMid = sunsetTime + (duskDuration * 0.5f);
			const float duskEnd = sunsetTime + duskDuration;

			// Time points array with 24h wraparound
			const float timePoints[] = {
				nightTime, dawnStart, dawnMid, sunriseTime, dayTime, sunsetTime, duskMid, duskEnd,
				nightTime + 24.0f, dawnStart + 24.0f, dawnMid + 24.0f, sunriseTime + 24.0f,
				dayTime + 24.0f, sunsetTime + 24.0f, duskMid + 24.0f, duskEnd + 24.0f
			};

			// Find current and next time periods
			int currentIdx = 0, nextIdx = 0;
			float currentPeriodTime = 0.0f, nextPeriodTime = 24.0f;

			for (int i = 0; i < 16; i++) {
				const float t = timePoints[i];
				if (currentTime >= t && t >= currentPeriodTime) {
					currentIdx = i;
					currentPeriodTime = t;
				}
				if (t > currentTime && nextPeriodTime >= t) {
					nextIdx = i;
					nextPeriodTime = t;
				}
			}

			// Map time point indices to time of day factors
			constexpr int factorMapping[] = {
				static_cast<int>(TimeOfDayFactorIndex::Night),
				static_cast<int>(TimeOfDayFactorIndex::Night),
				static_cast<int>(TimeOfDayFactorIndex::Dawn),
				static_cast<int>(TimeOfDayFactorIndex::Sunrise),
				static_cast<int>(TimeOfDayFactorIndex::Day),
				static_cast<int>(TimeOfDayFactorIndex::Sunset),
				static_cast<int>(TimeOfDayFactorIndex::Dusk),
				static_cast<int>(TimeOfDayFactorIndex::Night)
			};
			const int currentFactor = factorMapping[currentIdx % 8];
			const int nextFactor = factorMapping[nextIdx % 8];

			// Calculate blend weight
			float timeDiff = std::abs(nextPeriodTime - currentPeriodTime);
			if (timeDiff == 0.0f)
				timeDiff = 1.0f;

			const float blend = std::abs(currentTime - currentPeriodTime) / timeDiff;

			if (currentFactor == nextFactor) {
				factors[currentFactor] = 1.0f;
			} else {
				factors[currentFactor] = std::clamp(1.0f - blend, 0.0f, 1.0f);
				factors[nextFactor] = std::clamp(blend, 0.0f, 1.0f);
			}

			constexpr float dayPowerCurve = 0.6f;
			float powDay = std::pow(factors[static_cast<int>(TimeOfDayFactorIndex::Day)], dayPowerCurve);
			powDay = std::clamp(powDay, 0.0f, 1.0f);

			if (powDay > FLT_MIN) {
				const float complement = 1.0f - powDay;

				if (factors[static_cast<int>(TimeOfDayFactorIndex::Sunrise)] > FLT_MIN) {
					factors[static_cast<int>(TimeOfDayFactorIndex::Sunrise)] = std::clamp(complement, 0.0f, 1.0f);
				}

				if (factors[static_cast<int>(TimeOfDayFactorIndex::Sunset)] > FLT_MIN) {
					factors[static_cast<int>(TimeOfDayFactorIndex::Sunset)] = std::clamp(complement, 0.0f, 1.0f);
				}
			}

			factors[static_cast<int>(TimeOfDayFactorIndex::Day)] = powDay;

			// Assign to output arrays
			commonData.timeOfDay1[static_cast<int>(TimeOfDay1Index::Dawn)] = factors[static_cast<int>(TimeOfDayFactorIndex::Dawn)];
			commonData.timeOfDay1[static_cast<int>(TimeOfDay1Index::Sunrise)] = factors[static_cast<int>(TimeOfDayFactorIndex::Sunrise)];
			commonData.timeOfDay1[static_cast<int>(TimeOfDay1Index::Day)] = factors[static_cast<int>(TimeOfDayFactorIndex::Day)];
			commonData.timeOfDay1[static_cast<int>(TimeOfDay1Index::Sunset)] = factors[static_cast<int>(TimeOfDayFactorIndex::Sunset)];
			commonData.timeOfDay2[static_cast<int>(TimeOfDay2Index::Dusk)] = factors[static_cast<int>(TimeOfDayFactorIndex::Dusk)];
			commonData.timeOfDay2[static_cast<int>(TimeOfDay2Index::Night)] = factors[static_cast<int>(TimeOfDayFactorIndex::Night)];
		}

		// Calculate distance to night time (handling 24h wraparound)
		float distToNight = std::abs(currentTime - nightTime);
		if (distToNight > 12.0f) {
			distToNight = 24.0f - distToNight;
		}

		// Calculate distance to day time (handling 24h wraparound)
		float distToDay = std::abs(currentTime - dayTime);
		if (distToDay > 12.0f) {
			distToDay = 24.0f - distToDay;
		}

		// Night/day factor: 0.0 = pure night, 1.0 = pure day
		// Based on relative proximity to day vs night times
		if (distToNight + distToDay > 0.0f) {
			commonData.eNightDayFactor = distToNight / (distToNight + distToDay);
		} else {
			commonData.eNightDayFactor = 0.5f;  // Fallback if both distances are 0
		}

		commonData.timeOfDay2[static_cast<int>(TimeOfDay2Index::InteriorDay)] = commonData.eInteriorFactor * commonData.eNightDayFactor;
		commonData.timeOfDay2[static_cast<int>(TimeOfDay2Index::InteriorNight)] = commonData.eInteriorFactor * (1.0f - commonData.eNightDayFactor);
	}
}

void EffectManager::UpdateCommonVariablesForEffect(Effect& effect)
{
	if (!effect.GetEffect())
		return;

	auto renderer = globals::game::renderer;

	auto& depthData = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	effect.SetShaderResourceVariable("TextureDepth", GetEyeCroppedDepthSRV(depthData.texture, depthData.depthSRV));

	static const char* const formatTargets[] = {
		"RenderTargetRGBA32", "RenderTargetRGBA64", "RenderTargetRGBA64F",
		"RenderTargetR16F", "RenderTargetR32F", "RenderTargetRGB32F"
	};

	for (const auto& targetName : formatTargets) {
		auto* texture = effect.GetCachedCommonTexture(targetName);
		if (texture) {
			effect.SetShaderResourceVariable(targetName, GetEyeCroppedSRV(*texture));
		}
	}

	static const char* const fixedSizeTargets[] = {
		"RenderTarget1024", "RenderTarget512", "RenderTarget256", "RenderTarget128",
		"RenderTarget64", "RenderTarget32", "RenderTarget16"
	};

	for (const auto& targetName : fixedSizeTargets) {
		auto* texture = effect.GetCachedCommonTexture(targetName);
		if (texture) {
			effect.SetShaderResourceVariable(targetName, texture->srv.get());
		}
	}

	effect.SetVectorVariable("Timer", commonData.timer, sizeof(commonData.timer));
	effect.SetVectorVariable("Weather", commonData.weather, sizeof(commonData.weather));
	effect.SetVectorVariable("TimeOfDay1", commonData.timeOfDay1, sizeof(commonData.timeOfDay1));
	effect.SetVectorVariable("TimeOfDay2", commonData.timeOfDay2, sizeof(commonData.timeOfDay2));
	effect.SetVectorVariable("ENightDayFactor", &commonData.eNightDayFactor, sizeof(commonData.eNightDayFactor));
	effect.SetVectorVariable("EInteriorFactor", &commonData.eInteriorFactor, sizeof(commonData.eInteriorFactor));
}

void EffectManager::CopyTexture(ID3D11ShaderResourceView* a_source, ID3D11RenderTargetView* a_dest)
{
	if (!a_source || !a_dest || !copyPixelShader || !copyVertexShader) {
		static bool logged = false;
		if (!logged) {
			logger::warn("[EFFECTS11] Invalid parameters or shaders not initialized for texture copy");
			logged = true;
		}
		return;
	}

	auto context = globals::d3d::context;

	// Set viewport based on destination render target
	D3D11_TEXTURE2D_DESC texDesc;
	if (!Util::GetTexture2DDesc(a_dest, texDesc)) {
		logger::error("[EFFECTS11] Failed to get Texture2D from destination render target");
		return;
	}

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = static_cast<float>(texDesc.Width);
	viewport.Height = static_cast<float>(texDesc.Height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	// Set up for copy operation
	context->OMSetRenderTargets(1, &a_dest, nullptr);
	context->OMSetDepthStencilState(nullptr, 0);
	context->RSSetState(rasterizerState.get());
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);

	// Set IA state
	UINT stride = 20;  // 3 floats position + 2 floats texcoord
	UINT offset = 0;
	ID3D11Buffer* vbs[] = { quadVertexBuffer.get() };
	context->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
	context->IASetInputLayout(inputLayout.get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// Set shaders
	context->VSSetShader(copyVertexShader.get(), nullptr, 0);
	context->PSSetShader(copyPixelShader.get(), nullptr, 0);

	// Update dither frame count
	if (ditherConstantBuffer) {
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (SUCCEEDED(context->Map(ditherConstantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			*static_cast<uint32_t*>(mapped.pData) = frameCount;
			context->Unmap(ditherConstantBuffer.get(), 0);
		}
		ID3D11Buffer* cbs[] = { ditherConstantBuffer.get() };
		context->PSSetConstantBuffers(0, 1, cbs);
	}

	// Set source texture
	context->PSSetShaderResources(0, 1, &a_source);
	ID3D11SamplerState* samplers[] = { TextureManager::GetSingleton().GetLinearSampler() };
	context->PSSetSamplers(0, 1, samplers);

	// Draw fullscreen quad
	context->Draw(4, 0);

	// Clean up SRV binding
	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->PSSetShaderResources(0, 1, &nullSRV);
}

RE::BSGraphics::RenderTargetData& EffectManager::GetTextureOriginal()
{
	if (currentEyeIndex < 0)
		return globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	return eyeSourceData;
}

// (Re)creates a half-width crop target matching a_srcDesc/a_overrideFormat when missing/stale.
// @return false on creation failure -- caller must fall back to the uncropped source.
bool EffectManager::EnsureCropTarget(winrt::com_ptr<ID3D11Texture2D>& a_texture, winrt::com_ptr<ID3D11RenderTargetView>& a_rtv, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv, winrt::com_ptr<ID3D11UnorderedAccessView>* a_uav, const D3D11_TEXTURE2D_DESC& a_srcDesc, const char* a_debugName, DXGI_FORMAT a_overrideFormat)
{
	auto device = globals::d3d::device;
	const uint32_t halfWidth = a_srcDesc.Width / 2;
	const DXGI_FORMAT format = a_overrideFormat != DXGI_FORMAT_UNKNOWN ? a_overrideFormat : a_srcDesc.Format;

	if (a_texture) {
		D3D11_TEXTURE2D_DESC existingDesc{};
		a_texture->GetDesc(&existingDesc);
		if (existingDesc.Width == halfWidth && existingDesc.Height == a_srcDesc.Height && existingDesc.Format == format)
			return true;
		a_texture = nullptr;
		a_rtv = nullptr;
		a_srv = nullptr;
		if (a_uav)
			*a_uav = nullptr;
	}

	D3D11_TEXTURE2D_DESC desc = a_srcDesc;
	desc.Width = halfWidth;
	desc.Format = format;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | (a_uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
	desc.MiscFlags = 0;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, a_texture.put())) ||
		FAILED(device->CreateRenderTargetView(a_texture.get(), nullptr, a_rtv.put())) ||
		FAILED(device->CreateShaderResourceView(a_texture.get(), nullptr, a_srv.put())) ||
		(a_uav && FAILED(device->CreateUnorderedAccessView(a_texture.get(), nullptr, a_uav->put())))) {
		logger::error("[EFFECTS11] Failed to create crop target '{}'", a_debugName);
		a_texture = nullptr;
		a_rtv = nullptr;
		a_srv = nullptr;
		if (a_uav)
			*a_uav = nullptr;
		return false;
	}
	Util::SetResourceName(a_texture.get(), a_debugName);
	Util::SetResourceName(a_rtv.get(), "%s::RTV", a_debugName);
	Util::SetResourceName(a_srv.get(), "%s::SRV", a_debugName);
	if (a_uav)
		Util::SetResourceName(a_uav->get(), "%s::UAV", a_debugName);
	return true;
}

bool EffectManager::RefreshEyeSourceTexture(int a_eyeIndex)
{
	// PerfMode's real DLSS/FSR+RCAS output lives only in its testTexture, not kMAIN.
	auto& perfMode = globals::features::upscaling.perfMode;
	const bool usePerfModeSource = perfMode.IsHookActive() && perfMode.GetTestTexture() && perfMode.GetTestTextureSRV();

	ID3D11Texture2D* sourceTexture;
	ID3D11ShaderResourceView* sourceSRV;
	if (usePerfModeSource) {
		sourceTexture = perfMode.GetTestTexture();
		sourceSRV = perfMode.GetTestTextureSRV();
	} else {
		auto& kMain = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		if (!kMain.texture || !kMain.SRV)
			return false;
		sourceTexture = kMain.texture;
		sourceSRV = kMain.SRV;
	}

	D3D11_TEXTURE2D_DESC mainDesc{};
	sourceTexture->GetDesc(&mainDesc);
	currentMainWidth = mainDesc.Width;
	currentMainHeight = mainDesc.Height;

	if (!EnsureCropTarget(eyeSourceTexture, eyeSourceRTV, eyeSourceSRV, &eyeSourceUAV, mainDesc, "Effects11::EyeSource"))
		return false;
	eyeSourceData.texture = eyeSourceTexture.get();
	eyeSourceData.textureCopy = nullptr;
	eyeSourceData.RTV = eyeSourceRTV.get();
	eyeSourceData.SRV = eyeSourceSRV.get();
	eyeSourceData.SRVCopy = nullptr;
	eyeSourceData.UAV = eyeSourceUAV.get();

	return CropCopyEyeHalf(sourceSRV, mainDesc.Width, mainDesc.Height, eyeSourceRTV.get(), a_eyeIndex, eyeCropCopyPS, eyeCropCopyPSCompileAttempted, L"Data\\Shaders\\Effects11\\EyeCropCopyPS.hlsl");
}

ID3D11ShaderResourceView* EffectManager::GetEyeCroppedSRV(TextureManager::Texture& a_source)
{
	if (currentEyeIndex < 0 || !a_source.texture || !a_source.srv)
		return a_source.srv.get();

	D3D11_TEXTURE2D_DESC srcDesc{};
	a_source.texture->GetDesc(&srcDesc);
	if (srcDesc.Width != currentMainWidth)
		return a_source.srv.get();  // not full-width -- self-contained canvas, not subject to the crop mismatch

	auto& target = inputCropTargets[a_source.texture.get()];
	if (!EnsureCropTarget(target.texture, target.rtv, target.srv, nullptr, srcDesc, "Effects11::InputCrop"))
		return a_source.srv.get();
	if (!CropCopyEyeHalf(a_source.srv.get(), srcDesc.Width, srcDesc.Height, target.rtv.get(), currentEyeIndex, eyeCropCopyPS, eyeCropCopyPSCompileAttempted, L"Data\\Shaders\\Effects11\\EyeCropCopyPS.hlsl"))
		return a_source.srv.get();
	return target.srv.get();
}

ID3D11ShaderResourceView* EffectManager::GetEyeCroppedDepthSRV(ID3D11Texture2D* a_sourceTexture, ID3D11ShaderResourceView* a_sourceSRV)
{
	if (currentEyeIndex < 0 || !a_sourceTexture || !a_sourceSRV)
		return a_sourceSRV;

	D3D11_TEXTURE2D_DESC srcDesc{};
	a_sourceTexture->GetDesc(&srcDesc);

	// No currentMainWidth check here (unlike GetEyeCroppedSRV): under PerfMode it tracks a
	// different, display-res canvas, so it would always mismatch kMAIN's render-res depth
	// and silently skip the crop, splitting one eye's depth buffer across both.

	// R32_FLOAT scratch: the source's own depth-stencil format can't be bound as a color RTV.
	if (!EnsureCropTarget(depthCropTexture, depthCropRTV, depthCropSRV, nullptr, srcDesc, "Effects11::DepthCrop", DXGI_FORMAT_R32_FLOAT))
		return a_sourceSRV;
	if (!CropCopyEyeHalf(a_sourceSRV, srcDesc.Width, srcDesc.Height, depthCropRTV.get(), currentEyeIndex, eyeCropCopyDepthPS, eyeCropCopyDepthPSCompileAttempted, L"Data\\Shaders\\Effects11\\EyeCropCopyDepthPS.hlsl"))
		return a_sourceSRV;
	return depthCropSRV.get();
}

bool EffectManager::CropCopyEyeHalf(ID3D11ShaderResourceView* a_source, uint32_t a_srcWidth, uint32_t a_srcHeight, ID3D11RenderTargetView* a_destRTV, int a_eyeIndex, winrt::com_ptr<ID3D11PixelShader>& a_pixelShader, bool& a_pixelShaderCompileAttempted, const wchar_t* a_shaderPath)
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;
	const uint32_t halfWidth = a_srcWidth / 2;

	if (!a_pixelShader) {
		if (a_pixelShaderCompileAttempted)
			return false;
		a_pixelShaderCompileAttempted = true;
		a_pixelShader.attach(static_cast<ID3D11PixelShader*>(
			Util::CompileShader(a_shaderPath, {}, "ps_5_0")));
		if (!a_pixelShader)
			return false;
	}
	if (!eyeCropCB) {
		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = 16;
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&cbDesc, nullptr, eyeCropCB.put()))) {
			logger::error("[EFFECTS11] Failed to create eyeCropCB");
			return false;
		}
		Util::SetResourceName(eyeCropCB.get(), "Effects11::EyeCropCB");
	}

	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(context->Map(eyeCropCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		logger::error("[EFFECTS11] Failed to map eyeCropCB for eye index {}", a_eyeIndex);
		return false;
	}
	*static_cast<uint32_t*>(mapped.pData) = static_cast<uint32_t>(a_eyeIndex) * halfWidth;
	context->Unmap(eyeCropCB.get(), 0);

	// Nothing re-establishes state after a mid-sequence GetEyeCroppedSRV() call, so this needs
	// its own full save/restore.
	D3D11FullStateBackup stateBackup;
	stateBackup.Save(context);

	D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(halfWidth), static_cast<float>(a_srcHeight), 0, 1 };
	context->RSSetViewports(1, &viewport);
	context->OMSetRenderTargets(1, &a_destRTV, nullptr);
	context->OMSetDepthStencilState(nullptr, 0);
	context->RSSetState(rasterizerState.get());
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);

	UINT stride = sizeof(float) * 5;
	UINT offset = 0;
	ID3D11Buffer* vbs[] = { quadVertexBuffer.get() };
	context->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
	context->IASetInputLayout(inputLayout.get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	context->VSSetShader(copyVertexShader.get(), nullptr, 0);
	context->PSSetShader(a_pixelShader.get(), nullptr, 0);
	ID3D11Buffer* cb = eyeCropCB.get();
	context->PSSetConstantBuffers(0, 1, &cb);
	context->PSSetShaderResources(0, 1, &a_source);

	context->Draw(4, 0);

	ID3D11ShaderResourceView* nullSourceSRV = nullptr;
	context->PSSetShaderResources(0, 1, &nullSourceSRV);
	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);

	stateBackup.Restore(context);
	stateBackup.Release();
	return true;
}

void EffectManager::ApplyColorCorrection(ID3D11UnorderedAccessView* textureUAV)
{
	if (!textureUAV || !colorCorrectionComputeShader || !colorCorrectionConstantBuffer) {
		logger::warn("[EFFECTS11] Invalid parameters or shaders not initialized for color correction");
		return;
	}

	auto& settingManager = SettingManager::GetSingleton();

	auto brightness = settingManager.GetValue<float>(ids.brightness);
	auto gammaCurve = settingManager.GetValue<float>(ids.gammaCurve);

	auto context = globals::d3d::context;

	// Update constant buffer with current settings
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = context->Map(colorCorrectionConstantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) {
		logger::warn("[EFFECTS11] Failed to map color correction constant buffer");
		return;
	}
	{
		struct ColorCorrectionCB
		{
			float brightness;
			float gammaCurve;
			uint32_t frameCount;
			uint32_t pad;
		};
		auto* cbData = static_cast<ColorCorrectionCB*>(mapped.pData);
		cbData->brightness = brightness;
		cbData->gammaCurve = gammaCurve;
		cbData->frameCount = frameCount;
		context->Unmap(colorCorrectionConstantBuffer.get(), 0);
	}

	// Set compute shader and resources
	context->CSSetShader(colorCorrectionComputeShader.get(), nullptr, 0);
	ID3D11Buffer* bufferArray[] = { colorCorrectionConstantBuffer.get() };
	context->CSSetConstantBuffers(0, 1, bufferArray);
	context->CSSetUnorderedAccessViews(0, 1, &textureUAV, nullptr);

	// Get texture dimensions for dispatch
	D3D11_TEXTURE2D_DESC texDesc;
	if (!Util::GetTexture2DDesc(textureUAV, texDesc)) {
		logger::error("[EFFECTS11] Failed to get Texture2D from UAV in ApplyColorCorrection");
	} else {
		// Dispatch compute shader (8x8 thread groups)
		UINT dispatchX = (texDesc.Width + 7) / 8;
		UINT dispatchY = (texDesc.Height + 7) / 8;
		context->Dispatch(dispatchX, dispatchY, 1);
	}

	// Clear bindings
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShader(nullptr, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &nullCB);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
}

void EffectManager::ReloadShaders()
{
	copyVertexShader = nullptr;
	copyPixelShader = nullptr;
	colorCorrectionComputeShader = nullptr;
	CreateCopyShaders();
	CreateColorCorrectionShader();
}

void EffectManager::RenderEffectsList()
{
	Effect* allEffects[] = { &enbBloom, &enbLens, &enbAdaptation, &enbEffect, &enbEffectPostPass };

	std::vector<Effect*> compiledEffects;
	for (auto* effect : allEffects)
		if (effect->IsCompiled())
			compiledEffects.push_back(effect);

	for (auto* effect : compiledEffects) {
		if (ImGui::TreeNodeEx(effect->GetName().c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			effect->RenderImGui();
			ImGui::TreePop();
		}
	}

	for (auto* effect : allEffects) {
		if (!effect->IsFilePresent())
			continue;
		if (!effect->GetErrors().empty()) {
			ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Error, "%s:", effect->GetName().c_str());
			for (const auto& err : effect->GetErrors())
				ImGui::TextWrapped("%s", err.c_str());
		}
	}
}
