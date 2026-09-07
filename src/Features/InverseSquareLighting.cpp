#include "InverseSquareLighting.h"
#include "CSEditor/EditorWindow.h"
#include "Features/InverseSquareLighting/Common.h"
#include "Features/InverseSquareLighting/RadiusMath.h"
#include "LightLimitFix.h"
#include <numbers>

void InverseSquareLighting::PostPostLoad()
{
	stl::detour_thunk<CreatePointLight>(REL::RelocationID(17208, 17610));
	stl::detour_thunk<BSLight_GetLuminance>(REL::RelocationID(101303, 108292));

	logger::info("[InverseSquareLighting] Installed hooks");
}

RE::NiPointLight* InverseSquareLighting::CreatePointLight::thunk(RE::TESObjectLIGH* ligh, RE::TESObjectREFR* refr, RE::NiAVObject* root, bool forceDynamic, bool useLightRadius, bool affectRequesterOnly)
{
	const auto niLight = func(ligh, refr, root, forceDynamic, useLightRadius, affectRequesterOnly);

	if (ligh && root && niLight)
		SetExtLightData(niLight, ligh);

	return niLight;
}

void InverseSquareLighting::SetExtLightData(RE::NiLight* niLight, const RE::TESObjectLIGH* ligh)
{
	const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(niLight);
	runtimeData->flags.set(LightLimitFix::LightFlags::Initialised);
	if (ligh->data.flags.any(static_cast<RE::TES_LIGHT_FLAGS>(ISLCommon::TES_LIGHT_FLAGS_EXT::kInverseSquare)))
		runtimeData->flags.set(LightLimitFix::LightFlags::InverseSquare);
	if (ligh->data.flags.any(static_cast<RE::TES_LIGHT_FLAGS>(ISLCommon::TES_LIGHT_FLAGS_EXT::kLinear)))
		runtimeData->flags.set(LightLimitFix::LightFlags::Linear);
	if (ligh->data.flags.any(RE::TES_LIGHT_FLAGS::kSpotlight, RE::TES_LIGHT_FLAGS::kSpotShadow)) {
		runtimeData->flags.set(LightLimitFix::LightFlags::Spot);
		runtimeData->flags.reset(LightLimitFix::LightFlags::OmniDirectional);
	} else {
		runtimeData->flags.reset(LightLimitFix::LightFlags::Spot);
		runtimeData->flags.set(LightLimitFix::LightFlags::OmniDirectional);
	}
	runtimeData->cutoffOverride = std::clamp(ligh->data.fallofExponent, 0.01f, 1.f);
	runtimeData->lighFormId = ligh->formID;
	const float size = ligh->data.fov >= 50.0f ? std::numbers::sqrt2_v<float> : ligh->data.fov;
	runtimeData->size = std::clamp(size, 0.01f, 50.0f);
}

void InverseSquareLighting::ProcessLight(LightLimitFix::LightData& light, RE::BSLight* bsLight, RE::NiLight* niLight) const
{
	const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(niLight);

	if (light.lightFlags.none(LightLimitFix::LightFlags::Initialised)) {
		const auto userData = niLight->GetUserData();
		logger::debug("[InverseSquareLighting] FormID: 0x{:08X} | Light*: {:p} | Name: {} - light uninitialised", userData ? userData->formID : 0, static_cast<void*>(niLight), niLight->name);
		runtimeData->flags.set(LightLimitFix::LightFlags::Initialised);
	}

	const auto& editorRef = EditorWindow::GetSingleton()->lightEditor;
	editorRef.ApplyOverrides(niLight, runtimeData);

	light.lightFlags = runtimeData->flags;
	light.color = float3{ runtimeData->diffuse.red, runtimeData->diffuse.green, runtimeData->diffuse.blue };

	const bool isInvSq = light.lightFlags.any(LightLimitFix::LightFlags::InverseSquare);
	if (bsLight->pointLight && ((isInvSq && editorRef.disableInvSqLights) || (!isInvSq && editorRef.disableRegularLights)))
		light.lightFlags.set(LightLimitFix::LightFlags::Disabled);

	if (bsLight->pointLight && isInvSq) {
		const float intensity = runtimeData->fade * 4;
		// Use the type-based helper rather than the virtual IsShadowLight():
		// SCM's Hook_IsShadowLight reports false for shadow lights converted
		// to normal-light overflow handling. If we followed
		// that hook here the cutoff would flip from DefaultShadowCasterCutoff
		// (0.022) to DefaultCutoff (0.05) when a light is converted, shrinking
		// its effective radius by ~33% and visibly reducing its lit area.
		light.radius = CalculateRadius(intensity, ShadowCasterManager::IsShadowLightType(bsLight), runtimeData->cutoffOverride, runtimeData->size);
		runtimeData->radius = light.radius;
		light.invRadius = 1.f / light.radius;
		light.fadeZone = 1.f / (light.radius * std::clamp(ISLMath::FadeZoneBase * light.invRadius, 0.f, 1.f));
		light.sizeBias = ISLMath::ScaledUnitsSq * runtimeData->size * runtimeData->size * 0.5f;
		// light.color *= intensity;
		light.fade = intensity;
	} else {
		light.radius = runtimeData->radius;
		light.invRadius = 1.f / light.radius;
		// light.color *= runtimeData->fade;
		light.fade = runtimeData->fade;
	}
}

float InverseSquareLighting::CalculateRadius(const float intensity, const bool shadowCaster, const float cutoffOverride, const float size)
{
	return ISLMath::CalculateRadius(intensity, shadowCaster, cutoffOverride, size);
}

float InverseSquareLighting::GetAttenuation(const float distance, const float radius, const float size)
{
	return ISLMath::GetAttenuation(distance, radius, size);
}

float InverseSquareLighting::BSLight_GetLuminance::thunk(RE::BSLight* bsLight, RE::NiPoint3* targetPosition, RE::NiLight* refLight)
{
	auto* niLight = bsLight->light.get();
	const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(niLight);

	if (refLight == niLight || runtimeData->flags.any(LightLimitFix::LightFlags::Disabled))
		return 0.0f;

	if (!bsLight->pointLight || runtimeData->flags.none(LightLimitFix::LightFlags::InverseSquare))
		return func(bsLight, targetPosition, refLight);

	const float dist = niLight->world.translate.GetDistance(*targetPosition);
	const float attenuation = GetAttenuation(dist, runtimeData->radius, runtimeData->size);
	const float luminance = (runtimeData->diffuse.red + runtimeData->diffuse.green + runtimeData->diffuse.blue) * runtimeData->fade * 4 * attenuation * (1.0f / 3.0f);
	bsLight->luminance = luminance;

	return luminance;
}
