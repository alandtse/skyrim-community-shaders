#include "Precipitation.h"

#include "Features/Skylighting.h"
#include "Globals.h"

#include <algorithm>
#include <cmath>

namespace
{
	struct Main_RenderPrecipitation
	{
		static void thunk()
		{
			auto* graphicsState = globals::game::graphicsState;
			if (!graphicsState) {
				func();
				return;
			}

			auto& runtimeData = graphicsState->GetRuntimeData();
			const auto dynamicResolutionLock = runtimeData.dynamicResolutionLock;
			runtimeData.dynamicResolutionLock = 1;

			const SKSE::stl::scope_exit restoreState([&]() noexcept {
				runtimeData.dynamicResolutionLock = dynamicResolutionLock;
			});

			auto& skylighting = globals::features::skylighting;
			if (skylighting.loaded)
				skylighting.RenderOcclusion();
			else
				func();
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool precipitationHookInstalled = false;
}

float Precipitation::GetWeatherRainIntensity(const RE::TESWeather* a_weather)
{
	if (!a_weather || !a_weather->precipitationData)
		return 0.0f;
	const float density = a_weather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
	constexpr float fullRainIntensityDensity = 3.0f;
	return std::isfinite(density) && density > 0.0f ? std::min(1.0f, density / fullRainIntensityDensity) : 0.0f;
}

void Precipitation::Install()
{
	if (precipitationHookInstalled)
		return;

	stl::write_thunk_call<Main_RenderPrecipitation>(REL::RelocationID(35560, 36559).address() + REL::Relocate<std::uintptr_t>(0x3A1, REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0)) ? 0x3BF : 0x3A1, 0x2FA));
	precipitationHookInstalled = true;
	logger::info("[Precipitation] Installed shared render hook");
}

void Precipitation::RenderOriginal()
{
	Main_RenderPrecipitation::func();
}

RE::BSParticleShaderRainEmitter* Precipitation::GetRainEmitter(RE::BSGeometry* a_precipitation)
{
	return const_cast<RE::BSParticleShaderRainEmitter*>(
		GetRainEmitter(static_cast<const RE::BSGeometry*>(a_precipitation)));
}

const RE::BSParticleShaderRainEmitter* Precipitation::GetRainEmitter(const RE::BSGeometry* a_precipitation)
{
	if (!a_precipitation)
		return nullptr;

	const auto* particleProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(
		a_precipitation->GetGeometryRuntimeData().shaderProperty.get());
	if (!particleProperty || !particleProperty->particleEmitter)
		return nullptr;

	const auto* emitter = particleProperty->particleEmitter;
	return emitter->emitterType.any(RE::BSParticleShaderEmitter::EMITTER_TYPE::kRain) ?
	           static_cast<const RE::BSParticleShaderRainEmitter*>(emitter) :
	           nullptr;
}
