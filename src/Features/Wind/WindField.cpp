#include "WindField.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace WindField
{
	namespace
	{
		constexpr float kMinimumDivisor = 1e-4f;
		constexpr float kGradientIntegerScale = 2.0f / 16777215.0f;
		constexpr float kNoiseNormalization = 0.70710678f;

		struct Uint2
		{
			uint32_t x;
			uint32_t y;
		};

		struct Scalar2
		{
			float x;
			float y;
		};

		Uint2 Pcg2D(Uint2 a_value, const WindTuning& a_tuning) noexcept
		{
			a_value.x = a_value.x * a_tuning.pcgMultiplier + a_tuning.pcgIncrement;
			a_value.y = a_value.y * a_tuning.pcgMultiplier + a_tuning.pcgIncrement;
			a_value.x += a_value.y * a_tuning.pcgMultiplier;
			a_value.y += a_value.x * a_tuning.pcgMultiplier;
			a_value.x ^= a_value.x >> 16u;
			a_value.y ^= a_value.y >> 16u;
			a_value.x += a_value.y * a_tuning.pcgMultiplier;
			a_value.y += a_value.x * a_tuning.pcgMultiplier;
			a_value.x ^= a_value.x >> 16u;
			a_value.y ^= a_value.y >> 16u;
			return a_value;
		}

		Scalar2 GetGradient(Uint2 a_latticePosition, uint32_t a_seed, const WindTuning& a_tuning) noexcept
		{
			const Uint2 hash = Pcg2D({ a_latticePosition.x ^ a_seed,
										 a_latticePosition.y ^ (a_seed ^ a_tuning.gradientSeedMix) },
				a_tuning);
			Scalar2 gradient{
				static_cast<float>(hash.x >> 8u) * kGradientIntegerScale - 1.0f,
				static_cast<float>(hash.y >> 8u) * kGradientIntegerScale - 1.0f
			};
			const float inverseLength = 1.0f / std::sqrt(std::max(
												   gradient.x * gradient.x + gradient.y * gradient.y, kMinimumDivisor));
			gradient.x *= inverseLength;
			gradient.y *= inverseLength;
			return gradient;
		}

		float Dot(const Scalar2& a_left, const Scalar2& a_right) noexcept
		{
			return a_left.x * a_right.x + a_left.y * a_right.y;
		}

		float Lerp(float a_start, float a_end, float a_amount) noexcept
		{
			return a_start + (a_end - a_start) * a_amount;
		}

		float GradientNoise(const Scalar2& a_position, uint32_t a_seed, const WindTuning& a_tuning) noexcept
		{
			const Scalar2 latticePosition{ std::floor(a_position.x), std::floor(a_position.y) };
			const Scalar2 offset{ a_position.x - latticePosition.x, a_position.y - latticePosition.y };
			const Uint2 lattice{
				std::bit_cast<uint32_t>(static_cast<int32_t>(latticePosition.x)),
				std::bit_cast<uint32_t>(static_cast<int32_t>(latticePosition.y))
			};

			const float value00 = Dot(GetGradient(lattice, a_seed, a_tuning), offset);
			const float value10 = Dot(
				GetGradient({ lattice.x + 1u, lattice.y }, a_seed, a_tuning), { offset.x - 1.0f, offset.y });
			const float value01 = Dot(
				GetGradient({ lattice.x, lattice.y + 1u }, a_seed, a_tuning), { offset.x, offset.y - 1.0f });
			const float value11 = Dot(GetGradient({ lattice.x + 1u, lattice.y + 1u }, a_seed, a_tuning),
				{ offset.x - 1.0f, offset.y - 1.0f });

			const Scalar2 fade{
				offset.x * offset.x * offset.x * (offset.x * (offset.x * 6.0f - 15.0f) + 10.0f),
				offset.y * offset.y * offset.y * (offset.y * (offset.y * 6.0f - 15.0f) + 10.0f)
			};
			return Lerp(Lerp(value00, value10, fade.x), Lerp(value01, value11, fade.x), fade.y) *
			       kNoiseNormalization;
		}

		float Smoothstep(float a_minimum, float a_maximum, float a_value) noexcept
		{
			const float range = std::max(a_maximum - a_minimum, kMinimumDivisor);
			const float amount = std::clamp((a_value - a_minimum) / range, 0.0f, 1.0f);
			return amount * amount * (3.0f - 2.0f * amount);
		}

		float SampleAmbientGust(const float3& a_worldPosition, const Field& a_field,
			const WindTuning& a_tuning) noexcept
		{
			const Scalar2 worldPosition{ a_worldPosition.x, a_worldPosition.y };
			const Scalar2 direction{ a_field.direction.x, a_field.direction.y };
			const Scalar2 crosswind{ a_field.crosswind.x, a_field.crosswind.y };
			const float gustScale = std::max(std::abs(a_tuning.gustScale), kMinimumDivisor);
			const float frontAspectRatio = std::max(std::abs(a_tuning.frontAspectRatio), kMinimumDivisor);
			const Scalar2 frontCoordinate{
				(Dot(worldPosition, direction) - std::max(a_field.travelDistance, 0.0f)) / gustScale,
				Dot(worldPosition, crosswind) / (gustScale * frontAspectRatio)
			};
			const float broadGust = GradientNoise(frontCoordinate, a_tuning.broadGustSeed, a_tuning);
			const float detailScaleRatio = std::max(std::abs(a_tuning.detailScaleRatio), kMinimumDivisor);
			const float detailCrosswindScaleRatio =
				std::max(std::abs(a_tuning.detailCrosswindScaleRatio), kMinimumDivisor);
			const Scalar2 detailCoordinate{
				frontCoordinate.x / detailScaleRatio + frontCoordinate.y * a_tuning.turbulenceSkew,
				frontCoordinate.y / detailCrosswindScaleRatio
			};
			const float turbulentGust = GradientNoise(detailCoordinate, a_tuning.turbulentGustSeed, a_tuning);
			const float turbulenceStrength = std::max(a_tuning.turbulenceStrength, 0.0f);
			const float gustNoise =
				(broadGust + turbulentGust * turbulenceStrength) / (1.0f + turbulenceStrength);
			const float normalizedGust = gustNoise * 0.5f + 0.5f;
			const auto [contrastLow, contrastHigh] = std::minmax(a_tuning.contrastLow, a_tuning.contrastHigh);
			return Smoothstep(contrastLow, contrastHigh, normalizedGust);
		}
	}

	Field CreateField(const float3& a_direction, float a_speed, float a_travelDistance) noexcept
	{
		const float horizontalLengthSquared = a_direction.x * a_direction.x + a_direction.y * a_direction.y;
		const float inverseLength = horizontalLengthSquared > 1e-6f ? 1.0f / std::sqrt(horizontalLengthSquared) : 1.0f;
		const float3 direction = horizontalLengthSquared > 1e-6f ?
		                             float3{ a_direction.x * inverseLength, a_direction.y * inverseLength, 0.0f } :
		                             float3{ 1.0f, 0.0f, 0.0f };
		return {
			direction,
			std::isfinite(a_travelDistance) ? std::max(a_travelDistance, 0.0f) : 0.0f,
			{ -direction.y, direction.x, 0.0f },
			std::isfinite(a_speed) ? std::max(a_speed, 0.0f) : 0.0f
		};
	}

	void SetFieldSpeed(Field& a_field, float a_speed) noexcept
	{
		a_field.speed = std::isfinite(a_speed) ? std::max(a_speed, 0.0f) : 0.0f;
	}

	void AdvanceField(Field& a_field, float a_distance) noexcept
	{
		if (std::isfinite(a_distance))
			a_field.travelDistance = std::max(a_field.travelDistance + a_distance, 0.0f);
	}

	WindSample SampleField(const float3& a_worldPosition, const Field& a_field,
		const WindTuning& a_tuning) noexcept
	{
		const float ambientGust = SampleAmbientGust(a_worldPosition, a_field, a_tuning);
		const float gustMultiplier = std::max(
			1.0f + (ambientGust * 2.0f - 1.0f) * std::max(a_tuning.gustAmplitude, 0.0f), 0.0f);
		return { a_field.direction * (a_field.speed * gustMultiplier), ambientGust, 0.0f };
	}
}
