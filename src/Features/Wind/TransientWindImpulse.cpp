#include "TransientWindImpulse.h"

#include <algorithm>
#include <cmath>

namespace WindField
{
	namespace
	{
		constexpr float kMinimumDivisor = 1e-4f;

		float Smoothstep(float a_minimum, float a_maximum, float a_value) noexcept
		{
			const float range = std::max(a_maximum - a_minimum, kMinimumDivisor);
			const float amount = std::clamp((a_value - a_minimum) / range, 0.0f, 1.0f);
			return amount * amount * (3.0f - 2.0f * amount);
		}

		float3 NormalizeDirection(const float3& a_direction) noexcept
		{
			const float lengthSquared = a_direction.x * a_direction.x + a_direction.y * a_direction.y +
			                            a_direction.z * a_direction.z;
			return lengthSquared > 1e-6f ? a_direction / std::sqrt(lengthSquared) : float3{};
		}

		float EvaluateWaveFalloff(float a_distance, const TransientWindSource& a_source) noexcept
		{
			const float waveHalfWidth = std::max(std::abs(a_source.waveHalfWidth), kMinimumDivisor);
			const float distanceFromWavefront = a_distance - std::max(a_source.wavefrontDistance, 0.0f);
			const float decayTime = std::clamp(
				std::isfinite(a_source.decayTime) ? a_source.decayTime : 0.0f, 0.0f,
				kTransientImpulseMaximumDecayTime);
			const float propagationSpeed = std::isfinite(a_source.propagationSpeed) ?
			                                   std::max(a_source.propagationSpeed, 0.0f) :
			                                   0.0f;
			const float trailingWidth = waveHalfWidth + propagationSpeed * decayTime;
			const float localWaveWidth = distanceFromWavefront < 0.0f ? trailingWidth : waveHalfWidth;
			return 1.0f - Smoothstep(0.0f, localWaveWidth, std::abs(distanceFromWavefront));
		}
	}

	TransientImpulseSample SampleTransientImpulse(
		const float3& a_worldPosition, const TransientWindSource& a_source) noexcept
	{
		if (!std::isfinite(a_source.strength) || a_source.strength <= 0.0f ||
			!std::isfinite(a_source.maxDistance) || a_source.maxDistance <= 0.0f) {
			return {};
		}

		const float3 offset = a_worldPosition - a_source.origin;
		const float distanceSquared = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
		const float distance = std::sqrt(std::max(distanceSquared, 0.0f));
		const float3 direction = NormalizeDirection(a_source.direction);

		if (a_source.type == TransientWindSourceType::RadialWave) {
			if (distance > a_source.maxDistance)
				return {};
			float3 radialDirection = NormalizeDirection({ offset.x, offset.y, 0.0f });
			if (radialDirection.x == 0.0f && radialDirection.y == 0.0f)
				radialDirection = NormalizeDirection({ a_source.direction.x, a_source.direction.y, 0.0f });
			const float distanceFalloff = 1.0f - Smoothstep(0.0f, a_source.maxDistance, distance);
			const float intensity = std::clamp(EvaluateWaveFalloff(distance, a_source) * distanceFalloff, 0.0f, 1.0f);
			return { radialDirection * (a_source.strength * intensity), intensity };
		}

		if (a_source.type == TransientWindSourceType::DirectionalCone) {
			const float alongDistance = offset.x * direction.x + offset.y * direction.y + offset.z * direction.z;
			if (alongDistance < 0.0f || alongDistance > a_source.maxDistance)
				return {};
			const float3 lateralOffset = offset - direction * alongDistance;
			const float lateralDistance = std::sqrt(std::max(
				lateralOffset.x * lateralOffset.x + lateralOffset.y * lateralOffset.y + lateralOffset.z * lateralOffset.z,
				0.0f));
			const float radius = std::max(a_source.collisionRadius +
											  alongDistance * std::max(a_source.coneSpreadTangent, 0.0f),
				kMinimumDivisor);
			if (lateralDistance > radius)
				return {};
			const float radialFalloff = 1.0f - Smoothstep(0.0f, radius, lateralDistance);
			const float axialFalloff = 1.0f - Smoothstep(0.0f, a_source.maxDistance, alongDistance);
			const float intensity = std::clamp(radialFalloff * axialFalloff, 0.0f, 1.0f);
			return { direction * (a_source.strength * intensity), intensity };
		}

		if (a_source.type == TransientWindSourceType::OrientedFlow) {
			const float alongDistance = offset.x * direction.x + offset.y * direction.y + offset.z * direction.z;
			const float3 radialOffset = offset - direction * alongDistance;
			const float radialDistance = std::sqrt(std::max(
				radialOffset.x * radialOffset.x + radialOffset.y * radialOffset.y + radialOffset.z * radialOffset.z,
				0.0f));
			const float radius = std::max(a_source.maxDistance, kMinimumDivisor);
			const float axialExtent = std::max(
				alongDistance >= 0.0f ? a_source.waveHalfWidth : a_source.propagationSpeed, kMinimumDivisor);
			if (radialDistance > radius || std::abs(alongDistance) > axialExtent)
				return {};
			const float radialFalloff = 1.0f - Smoothstep(0.0f, radius, radialDistance);
			const float axialFalloff = 1.0f - Smoothstep(0.0f, axialExtent, std::abs(alongDistance));
			const float intensity = std::clamp(radialFalloff * axialFalloff, 0.0f, 1.0f);
			return { direction * (a_source.strength * intensity), intensity };
		}

		if (a_source.type == TransientWindSourceType::DirectionalOvalWave) {
			const float alongDistance = offset.x * direction.x + offset.y * direction.y + offset.z * direction.z;
			if (alongDistance < 0.0f || alongDistance > a_source.maxDistance)
				return {};
			const float horizontalLengthSquared = direction.x * direction.x + direction.y * direction.y;
			const float inverseHorizontalLength = horizontalLengthSquared > 1e-6f ?
			                                          1.0f / std::sqrt(horizontalLengthSquared) :
			                                          0.0f;
			const float3 horizontalAxis = horizontalLengthSquared > 1e-6f ?
			                                  float3{ direction.y * inverseHorizontalLength,
												  -direction.x * inverseHorizontalLength, 0.0f } :
			                                  float3{ 1.0f, 0.0f, 0.0f };
			const float3 verticalAxis{
				direction.y * horizontalAxis.z - direction.z * horizontalAxis.y,
				direction.z * horizontalAxis.x - direction.x * horizontalAxis.z,
				direction.x * horizontalAxis.y - direction.y * horizontalAxis.x
			};
			const float horizontalOffset = offset.x * horizontalAxis.x + offset.y * horizontalAxis.y +
			                               offset.z * horizontalAxis.z;
			const float verticalScale = std::max(a_source.verticalScale, kMinimumDivisor);
			const float verticalOffset = (offset.x * verticalAxis.x + offset.y * verticalAxis.y +
											 offset.z * verticalAxis.z) /
			                             verticalScale;
			const float ovalDistance = std::sqrt(
				horizontalOffset * horizontalOffset + verticalOffset * verticalOffset);
			const float coreRadius = std::max(
				a_source.collisionRadius + alongDistance * std::max(a_source.coneSpreadTangent, 0.0f),
				kMinimumDivisor);
			const float distanceRatio = std::clamp(alongDistance / a_source.maxDistance, 0.0f, 1.0f);
			const float falloffWidth = std::max(a_source.falloffWidth, 0.0f) *
			                           (0.25f + 0.75f * distanceRatio);
			const float outerRadius = std::max(
				coreRadius + falloffWidth,
				a_source.collisionRadius + alongDistance * std::max(a_source.sideSpreadTangent, 0.0f));
			const float geometryFalloff = 1.0f - Smoothstep(
													 coreRadius, std::max(outerRadius, coreRadius + kMinimumDivisor), ovalDistance);
			if (geometryFalloff <= 0.0f)
				return {};
			const float distanceFalloff = 1.0f - Smoothstep(0.0f, a_source.maxDistance, alongDistance);
			const float waveStrength = EvaluateWaveFalloff(alongDistance, a_source) *
			                           distanceFalloff * geometryFalloff;
			const float intensity = std::clamp(waveStrength, 0.0f, 1.0f);
			return { direction * (a_source.strength * waveStrength), intensity };
		}

		const float directionLengthSquared = a_source.direction.x * a_source.direction.x +
		                                     a_source.direction.y * a_source.direction.y +
		                                     a_source.direction.z * a_source.direction.z;
		float propagationDistance = 0.0f;
		float3 pressureDirection{};
		float geometryFalloff = 1.0f;
		if (directionLengthSquared <= 1e-6f) {
			propagationDistance = distance;
			if (propagationDistance > a_source.maxDistance)
				return {};
			pressureDirection = propagationDistance > kMinimumDivisor ? offset / propagationDistance : float3{};
		} else {
			propagationDistance = offset.x * direction.x + offset.y * direction.y + offset.z * direction.z;
			if (propagationDistance < 0.0f || propagationDistance > a_source.maxDistance)
				return {};
			const float3 lateralOffset = offset - direction * propagationDistance;
			const float lateralDistance = std::sqrt(std::max(
				lateralOffset.x * lateralOffset.x + lateralOffset.y * lateralOffset.y + lateralOffset.z * lateralOffset.z,
				0.0f));
			const float collisionRadius = std::max(std::abs(a_source.collisionRadius), kMinimumDivisor);
			const float coreRadius = collisionRadius +
			                         propagationDistance * std::max(a_source.coneSpreadTangent, 0.0f);
			const float sideRadius = std::max(
				collisionRadius + propagationDistance * std::max(a_source.sideSpreadTangent, 0.0f),
				coreRadius + kMinimumDivisor);
			const float coreFalloff = 1.0f - Smoothstep(
												 std::max(coreRadius - collisionRadius, 0.0f), coreRadius, lateralDistance);
			const float sideFalloff = (1.0f - coreFalloff) *
			                          (1.0f - Smoothstep(coreRadius, sideRadius, lateralDistance)) *
			                          std::clamp(a_source.sideStrength, 0.0f, 1.0f);
			if (coreFalloff <= 0.0f && sideFalloff <= 0.0f)
				return {};
			const float3 sideDirection = lateralDistance > kMinimumDivisor ?
			                                 lateralOffset / lateralDistance :
			                                 float3{};
			pressureDirection = direction * coreFalloff + sideDirection * sideFalloff;
			geometryFalloff = std::max(coreFalloff, sideFalloff);
		}

		const float distanceFalloff = 1.0f - Smoothstep(0.0f, a_source.maxDistance, propagationDistance);
		const float waveStrength = EvaluateWaveFalloff(propagationDistance, a_source) * distanceFalloff;
		const float intensity = std::clamp(geometryFalloff * waveStrength, 0.0f, 1.0f);
		return { pressureDirection * (a_source.strength * waveStrength), intensity };
	}

	TransientImpulseSample SampleTransientImpulses(
		const float3& a_worldPosition, std::span<const TransientWindSource> a_sources) noexcept
	{
		TransientImpulseSample sample;
		for (const auto& source : a_sources) {
			const auto sourceSample = SampleTransientImpulse(a_worldPosition, source);
			sample.velocity += sourceSample.velocity;
			sample.intensity = std::max(sample.intensity, sourceSample.intensity);
		}
		return sample;
	}

	TransientWindSource MakeDirectionalWave(const float3& a_origin, const float3& a_direction,
		float a_strength, float a_maxDistance, float a_waveHalfWidth, float a_propagationSpeed,
		float a_coneCosine, float a_decayTime) noexcept
	{
		const float coneCosine = std::clamp(a_coneCosine, 0.0f, 1.0f);
		const float coneTangent = coneCosine > kMinimumDivisor ?
		                              std::sqrt(std::max(1.0f - coneCosine * coneCosine, 0.0f)) / coneCosine :
		                              0.0f;
		return { a_origin, 0.0f, a_direction, a_strength, a_maxDistance, a_waveHalfWidth,
			a_propagationSpeed, coneTangent, a_decayTime, 0.0f, coneTangent, 0.0f,
			TransientWindSourceType::DirectionalWave, 1.0f, 0.0f, 0.0f };
	}

	TransientWindSource MakeRadialWave(const float3& a_origin, const float3& a_fallbackDirection,
		float a_strength, float a_maxDistance, float a_waveHalfWidth, float a_propagationSpeed,
		float a_decayTime) noexcept
	{
		return { a_origin, 0.0f, a_fallbackDirection, a_strength, a_maxDistance, a_waveHalfWidth,
			a_propagationSpeed, 0.0f, a_decayTime, 0.0f, 0.0f, 0.0f,
			TransientWindSourceType::RadialWave, 1.0f, 0.0f, 0.0f };
	}

	TransientWindSource MakeDirectionalCone(const float3& a_origin, const float3& a_direction,
		float a_strength, float a_maxDistance, float a_coneCosine) noexcept
	{
		const float coneCosine = std::clamp(a_coneCosine, 0.0f, 1.0f);
		const float coneTangent = coneCosine > kMinimumDivisor ?
		                              std::sqrt(std::max(1.0f - coneCosine * coneCosine, 0.0f)) / coneCosine :
		                              0.0f;
		return { a_origin, 0.0f, a_direction, a_strength, a_maxDistance, 0.0f, 0.0f,
			coneTangent, 0.0f, 0.0f, 0.0f, 0.0f, TransientWindSourceType::DirectionalCone,
			1.0f, 0.0f, 0.0f };
	}

	TransientWindSource MakeOrientedFlow(const float3& a_origin, const float3& a_direction,
		float a_strength, float a_radius, float a_bowLength, float a_wakeLength) noexcept
	{
		return { a_origin, 0.0f, a_direction, a_strength, a_radius, a_bowLength, a_wakeLength,
			0.0f, 0.0f, 0.0f, 0.0f, 0.0f, TransientWindSourceType::OrientedFlow,
			1.0f, 0.0f, 0.0f };
	}
}
