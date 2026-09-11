#pragma once

namespace Precipitation
{
	/** @brief Returns weather rain intensity on the established 0-1 wetness scale, not a particle count. */
	float GetWeatherRainIntensity(const RE::TESWeather* a_weather);
	/** @brief Installs the shared precipitation render hook. */
	void Install();

	/** @brief Invokes the engine precipitation render path captured by the shared hook. */
	void RenderOriginal();

	/** @brief Returns the rain emitter owned by precipitation geometry, if present. */
	RE::BSParticleShaderRainEmitter* GetRainEmitter(RE::BSGeometry* a_precipitation);

	/** @brief Returns the rain emitter owned by precipitation geometry, if present. */
	const RE::BSParticleShaderRainEmitter* GetRainEmitter(const RE::BSGeometry* a_precipitation);
}
