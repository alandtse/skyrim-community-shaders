#pragma once

#include <cstdint>
#include <type_traits>

namespace WindField
{
	/** @brief Fixed procedural controls and seeds for stateless ambient weather wind. */
	struct WindTuning
	{
		float gustScale{ 2048.0f };
		float frontAspectRatio{ 4.0f };
		float gustAdvectionBaseSpeed{ 384.0f };
		float gustAdvectionMultiplier{ 1.0f };
		float detailScaleRatio{ 0.38f };
		float detailCrosswindScaleRatio{ 0.55f };
		float turbulenceStrength{ 0.24f };
		float turbulenceSkew{ 0.35f };
		float contrastLow{ 0.30f };
		float contrastHigh{ 0.70f };
		float gustAmplitude{ 0.35f };
		uint32_t broadGustSeed{ 0x2341316Cu };
		uint32_t turbulentGustSeed{ 0x48013EA4u };
		uint32_t gradientSeedMix{ 0x1E3779B9u };
		uint32_t pcgMultiplier{ 1664525u };
		uint32_t pcgIncrement{ 1013904223u };
	};
	static_assert(sizeof(WindTuning) == 64);
	static_assert(std::is_standard_layout_v<WindTuning>);

	/** @brief One procedural noise-field instance with an orientation fixed for its lifetime. */
	struct Field
	{
		float3 direction{ 1.0f, 0.0f, 0.0f };
		float travelDistance{};
		float3 crosswind{ 0.0f, 1.0f, 0.0f };
		float speed{};
	};
	static_assert(sizeof(Field) == 32);
	static_assert(std::is_standard_layout_v<Field>);

	/** @brief A point sample of the procedural wind field. */
	struct WindSample
	{
		float3 velocity{};
		float ambientGust{};
		float transientImpulse{};
	};

	/** @brief Constructs a field and establishes its direction/crosswind basis once. */
	[[nodiscard]] Field CreateField(const float3& a_direction, float a_speed,
		float a_travelDistance = 0.0f) noexcept;
	/** @brief Updates a field's weather strength without changing its orientation. */
	void SetFieldSpeed(Field& a_field, float a_speed) noexcept;
	/** @brief Advances a field's independent noise-scroll accumulator. */
	void AdvanceField(Field& a_field, float a_distance) noexcept;

	/** @brief Samples a field using its precomputed, never-rotated orientation basis. */
	[[nodiscard]] WindSample SampleField(const float3& a_worldPosition, const Field& a_field,
		const WindTuning& a_tuning) noexcept;
}
