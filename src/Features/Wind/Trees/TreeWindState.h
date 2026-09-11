#pragma once

#include "Buffer.h"
#include "Utils/LazyShader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

// These dimensions mirror the field and transient-height counts in HLSL.
inline constexpr uint32_t kTreeWindSpringFieldCount = 3;
inline constexpr uint32_t kTreeWindTransientHeightCount = 3;
inline constexpr std::array<uint32_t, kTreeWindSpringFieldCount> kTreeWindSpringTextureSizes{ 256, 128, 128 };
inline constexpr std::array<float, kTreeWindSpringFieldCount> kTreeWindSpringMaximumDistances{ 8000.0f, 20000.0f, 60000.0f };

struct alignas(16) TreeWindSpringFieldData
{
	float2 fieldMinimum;
	float2 previousFieldMinimum;
	float fieldHeight;
	float frameTime;
	float fieldSize;
	uint32_t textureSize;
	uint32_t initialize;
	uint32_t fieldAvailable;
	float maxDistance;
	float previousFieldHeight;
};

STATIC_ASSERT_ALIGNAS_16(TreeWindSpringFieldData);
static_assert(sizeof(TreeWindSpringFieldData) == 48);
static_assert(offsetof(TreeWindSpringFieldData, fieldHeight) == 16);
static_assert(offsetof(TreeWindSpringFieldData, initialize) == 32);
static_assert(offsetof(TreeWindSpringFieldData, maxDistance) == 40);

struct alignas(16) TreeWindSpringData
{
	std::array<TreeWindSpringFieldData, kTreeWindSpringFieldCount> fields;
	uint32_t activeField;
	float springFrequency;
	float springDamping;
	float gustScale;
	float gustSoftLimit;
	uint32_t transientFieldMask;
	float transientSpringFrequency;
	float transientSpringDamping;
};

STATIC_ASSERT_ALIGNAS_16(TreeWindSpringData);
static_assert(sizeof(TreeWindSpringData) == 176);
static_assert(offsetof(TreeWindSpringData, activeField) == 144);
static_assert(offsetof(TreeWindSpringData, gustSoftLimit) == 160);
static_assert(offsetof(TreeWindSpringData, transientFieldMask) == 164);
static_assert(offsetof(TreeWindSpringData, transientSpringFrequency) == 168);

/** GPU resources and temporal field state for structural tree wind. */
struct TreeWindState
{
	using TexturePair = std::array<std::unique_ptr<Texture2D>, 2>;

	std::unique_ptr<ConstantBuffer> springConstantBuffer;
	std::array<TexturePair, kTreeWindSpringFieldCount> springResponseTextures;
	std::array<TexturePair, kTreeWindSpringFieldCount> springVelocityTextures;
	// Three array slices cache low, middle, and high transient response samples.
	std::array<TexturePair, kTreeWindSpringFieldCount> transientTextures;
	std::array<TexturePair, kTreeWindSpringFieldCount> transientVelocityTextures;
	winrt::com_ptr<ID3D11SamplerState> springSampler;
	std::array<uint32_t, kTreeWindSpringFieldCount> springTextureIndices{};
	std::array<float2, kTreeWindSpringFieldCount> springFieldMinimum{};
	std::array<float2, kTreeWindSpringFieldCount> previousSpringFieldMinimum{};
	std::array<float, kTreeWindSpringFieldCount> springFieldHeight{};
	std::array<float, kTreeWindSpringFieldCount> previousSpringFieldHeight{};
	std::array<bool, kTreeWindSpringFieldCount> springInitialized{};
	std::array<bool, kTreeWindSpringFieldCount> springFieldAvailable{};
	uint32_t lastUpdateFrame = 0;
	bool hasUpdated = false;
	Util::LazyShader<ID3D11ComputeShader> springFieldComputeShader;
};
