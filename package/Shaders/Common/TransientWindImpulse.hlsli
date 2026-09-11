#ifndef TRANSIENT_WIND_IMPULSE_HLSLI
#define TRANSIENT_WIND_IMPULSE_HLSLI

#include "Common/Math.hlsli"

namespace WindField
{
	// Keep this mirrored with WindField::kTransientImpulseCapacity; use spatial indexing or
	// a rasterized shared field if source counts make per-sample scans expensive.
	static const uint TransientImpulseCapacity = 96u;
	static const float TransientMaximumDecayTime = 5.0f;
	static const uint TransientWindSourceTypeDirectionalWave = 0u;
	static const uint TransientWindSourceTypeRadialWave = 1u;
	static const uint TransientWindSourceTypeDirectionalCone = 2u;
	static const uint TransientWindSourceTypeOrientedFlow = 3u;
	static const uint TransientWindSourceTypeDirectionalOvalWave = 4u;

	struct TransientWindSource
	{
		float3 origin;
		float wavefrontDistance;
		float3 direction;
		float strength;
		float maxDistance;
		float waveHalfWidth;
		float propagationSpeed;
		float coneSpreadTangent;
		float decayTime;
		float collisionRadius;
		float sideSpreadTangent;
		float sideStrength;
		uint type;
		float verticalScale;
		float falloffWidth;
		float padding;
	};

	struct TransientImpulseSample
	{
		float3 velocity;
		float intensity;
	};

	float EvaluateTransientWaveFalloff(float distance, TransientWindSource source)
	{
		float waveHalfWidth = max(abs(source.waveHalfWidth), EPSILON_WIND_GEOMETRY);
		float distanceFromWavefront = distance - max(source.wavefrontDistance, 0.0f);
		float trailingWidth = waveHalfWidth +
		                      max(source.propagationSpeed, 0.0f) *
		                          clamp(source.decayTime, 0.0f, TransientMaximumDecayTime);
		float localWaveWidth = distanceFromWavefront < 0.0f ? trailingWidth : waveHalfWidth;
		return 1.0f - smoothstep(0.0f, localWaveWidth, abs(distanceFromWavefront));
	}

	/** @brief Samples one typed analytic transient wind source. */
	TransientImpulseSample SampleTransientImpulse(float3 worldPosition, TransientWindSource source)
	{
		TransientImpulseSample sample;
		sample.velocity = 0.0f;
		sample.intensity = 0.0f;
		if (source.strength <= 0.0f || source.maxDistance <= 0.0f)
			return sample;

		float3 offset = worldPosition - source.origin;
		float distanceSquared = dot(offset, offset);
		float distance = sqrt(max(distanceSquared, 0.0f));
		float directionLengthSquared = dot(source.direction, source.direction);
		float3 direction = directionLengthSquared > EPSILON_WIND_LENGTH_SQ ?
		                       source.direction / sqrt(directionLengthSquared) :
		                       0.0f;

		if (source.type == TransientWindSourceTypeRadialWave) {
			if (distance > source.maxDistance)
				return sample;
			float2 radialXY = offset.xy;
			float radialLengthSquared = dot(radialXY, radialXY);
			float2 fallbackXY = source.direction.xy;
			float fallbackLengthSquared = dot(fallbackXY, fallbackXY);
			float2 radialDirection = radialLengthSquared > EPSILON_WIND_LENGTH_SQ ?
			                             radialXY / sqrt(radialLengthSquared) :
			                             (fallbackLengthSquared > EPSILON_WIND_LENGTH_SQ ? fallbackXY / sqrt(fallbackLengthSquared) : 0.0f);
			float distanceFalloff = 1.0f - smoothstep(0.0f, source.maxDistance, distance);
			sample.intensity = saturate(EvaluateTransientWaveFalloff(distance, source) * distanceFalloff);
			sample.velocity = float3(radialDirection, 0.0f) * (source.strength * sample.intensity);
			return sample;
		}

		if (source.type == TransientWindSourceTypeDirectionalCone) {
			float alongDistance = dot(offset, direction);
			if (alongDistance < 0.0f || alongDistance > source.maxDistance)
				return sample;
			float lateralDistance = length(offset - direction * alongDistance);
			float radius = max(source.collisionRadius + alongDistance * max(source.coneSpreadTangent, 0.0f), EPSILON_WIND_GEOMETRY);
			if (lateralDistance > radius)
				return sample;
			float radialFalloff = 1.0f - smoothstep(0.0f, radius, lateralDistance);
			float axialFalloff = 1.0f - smoothstep(0.0f, source.maxDistance, alongDistance);
			sample.intensity = saturate(radialFalloff * axialFalloff);
			sample.velocity = direction * (source.strength * sample.intensity);
			return sample;
		}

		if (source.type == TransientWindSourceTypeOrientedFlow) {
			float alongDistance = dot(offset, direction);
			float radialDistance = length(offset - direction * alongDistance);
			float radius = max(source.maxDistance, EPSILON_WIND_GEOMETRY);
			float axialExtent = max(alongDistance >= 0.0f ? source.waveHalfWidth : source.propagationSpeed, EPSILON_WIND_GEOMETRY);
			if (radialDistance > radius || abs(alongDistance) > axialExtent)
				return sample;
			float radialFalloff = 1.0f - smoothstep(0.0f, radius, radialDistance);
			float axialFalloff = 1.0f - smoothstep(0.0f, axialExtent, abs(alongDistance));
			sample.intensity = saturate(radialFalloff * axialFalloff);
			sample.velocity = direction * (source.strength * sample.intensity);
			return sample;
		}

		if (source.type == TransientWindSourceTypeDirectionalOvalWave) {
			float alongDistance = dot(offset, direction);
			if (alongDistance < 0.0f || alongDistance > source.maxDistance)
				return sample;
			float horizontalLengthSquared = dot(direction.xy, direction.xy);
			float3 horizontalAxis = horizontalLengthSquared > EPSILON_WIND_LENGTH_SQ ?
			                            float3(direction.y, -direction.x, 0.0f) * rsqrt(horizontalLengthSquared) :
			                            float3(1.0f, 0.0f, 0.0f);
			float3 verticalAxis = cross(direction, horizontalAxis);
			float horizontalOffset = dot(offset, horizontalAxis);
			float verticalOffset = dot(offset, verticalAxis) / max(source.verticalScale, EPSILON_WIND_GEOMETRY);
			float ovalDistance = length(float2(horizontalOffset, verticalOffset));
			float coreRadius = max(source.collisionRadius +
									   alongDistance * max(source.coneSpreadTangent, 0.0f),
				EPSILON_WIND_GEOMETRY);
			float distanceRatio = saturate(alongDistance / source.maxDistance);
			float falloffWidth = max(source.falloffWidth, 0.0f) * (0.25f + 0.75f * distanceRatio);
			float outerRadius = max(
				coreRadius + falloffWidth,
				source.collisionRadius + alongDistance * max(source.sideSpreadTangent, 0.0f));
			float geometryFalloff = 1.0f - smoothstep(coreRadius, max(outerRadius, coreRadius + EPSILON_WIND_GEOMETRY), ovalDistance);
			if (geometryFalloff <= 0.0f)
				return sample;
			float distanceFalloff = 1.0f - smoothstep(0.0f, source.maxDistance, alongDistance);
			float waveStrength = EvaluateTransientWaveFalloff(alongDistance, source) *
			                     distanceFalloff * geometryFalloff;
			sample.intensity = saturate(waveStrength);
			sample.velocity = direction * (source.strength * waveStrength);
			return sample;
		}

		float propagationDistance = 0.0f;
		float3 pressureDirection = 0.0f;
		float geometryFalloff = 1.0f;
		if (directionLengthSquared <= EPSILON_WIND_LENGTH_SQ) {
			propagationDistance = distance;
			if (propagationDistance > source.maxDistance)
				return sample;
			pressureDirection = propagationDistance > EPSILON_WIND_GEOMETRY ? offset / propagationDistance : 0.0f;
		} else {
			propagationDistance = dot(offset, direction);
			if (propagationDistance < 0.0f || propagationDistance > source.maxDistance)
				return sample;
			float3 lateralOffset = offset - direction * propagationDistance;
			float lateralDistance = length(lateralOffset);
			float collisionRadius = max(abs(source.collisionRadius), EPSILON_WIND_GEOMETRY);
			float coreRadius = collisionRadius + propagationDistance * max(source.coneSpreadTangent, 0.0f);
			float sideRadius = max(
				collisionRadius + propagationDistance * max(source.sideSpreadTangent, 0.0f),
				coreRadius + EPSILON_WIND_GEOMETRY);
			float coreFalloff = 1.0f - smoothstep(
										   max(coreRadius - collisionRadius, 0.0f), coreRadius, lateralDistance);
			float sideFalloff = (1.0f - coreFalloff) *
			                    (1.0f - smoothstep(coreRadius, sideRadius, lateralDistance)) *
			                    saturate(source.sideStrength);
			if (coreFalloff <= 0.0f && sideFalloff <= 0.0f)
				return sample;
			float3 sideDirection = lateralDistance > EPSILON_WIND_GEOMETRY ? lateralOffset / lateralDistance : 0.0f;
			pressureDirection = direction * coreFalloff + sideDirection * sideFalloff;
			geometryFalloff = max(coreFalloff, sideFalloff);
		}

		float distanceFalloff = 1.0f - smoothstep(0.0f, source.maxDistance, propagationDistance);
		float waveStrength = EvaluateTransientWaveFalloff(propagationDistance, source) * distanceFalloff;
		sample.intensity = saturate(geometryFalloff * waveStrength);
		sample.velocity = pressureDirection * (source.strength * waveStrength);
		return sample;
	}
}

#endif
