#pragma once

#include "Buffer.h"
#include "Features/Wind/Settings/WindSettings.h"
#include "Utils/LazyShader.h"

#include <array>
#include <cstddef>
#include <memory>

struct alignas(16) GrassWindSpringFieldData
{
	float2 fieldMinimum;
	float2 previousFieldMinimum;
	float fieldHeight;
	float frameTime;
	float responseRadians;
	float maximumTiltRadians;
	float sensitivity;
	float springFrequency;
	float springDamping;
	uint32_t initialize;
	uint32_t fieldAvailable;
	float fieldSize;
	uint32_t textureSize;
	float maxDistance;
};

STATIC_ASSERT_ALIGNAS_16(GrassWindSpringFieldData);
static_assert(sizeof(GrassWindSpringFieldData) == 64);

struct alignas(16) GrassWindSpringData
{
	std::array<GrassWindSpringFieldData, WindSettingsLimits::kGrassWindSpringQualityRangeCount> fields;
	uint32_t activeField;
	uint32_t transientFieldMask;
	float2 padding;
};

STATIC_ASSERT_ALIGNAS_16(GrassWindSpringData);
static_assert(sizeof(GrassWindSpringData) == 208);
static_assert(offsetof(GrassWindSpringData, transientFieldMask) == 196);

/** GPU resources and temporal field state for grass spring simulation. */
struct GrassWindState
{
	using TexturePair = std::array<std::unique_ptr<Texture2D>, 2>;

	std::unique_ptr<ConstantBuffer> springConstantBuffer;
	std::array<TexturePair, WindSettingsLimits::kGrassWindSpringQualityRangeCount> springResponseTextures;
	std::array<TexturePair, WindSettingsLimits::kGrassWindSpringQualityRangeCount> springVelocityTextures;
	winrt::com_ptr<ID3D11SamplerState> springSampler;
	std::array<uint32_t, WindSettingsLimits::kGrassWindSpringQualityRangeCount> springTextureIndices{};
	std::array<uint32_t, WindSettingsLimits::kGrassWindSpringQualityRangeCount> springTextureSizes{};
	std::array<float, WindSettingsLimits::kGrassWindSpringQualityRangeCount> springWorldSizes{};
	std::array<float2, WindSettingsLimits::kGrassWindSpringQualityRangeCount> springFieldMinimum{};
	std::array<float2, WindSettingsLimits::kGrassWindSpringQualityRangeCount> previousSpringFieldMinimum{};
	std::array<bool, WindSettingsLimits::kGrassWindSpringQualityRangeCount> springInitialized{};
	std::array<bool, WindSettingsLimits::kGrassWindSpringQualityRangeCount> springFieldAvailable{};
	Util::LazyShader<ID3D11ComputeShader> springComputeShader;
};
