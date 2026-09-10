#ifndef __DAMPED_SPRING_DEPENDENCY_HLSL__
#define __DAMPED_SPRING_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"

namespace DampedSpring
{
	static const float TAU = 6.28318530717958647692f;

	/** @brief Advances an exact damped oscillator over one constant-target interval. */
	void Advance(float4 position, float4 velocity, float4 target, float frameTime,
		float frequency, float damping, out float4 nextPosition, out float4 nextVelocity)
	{
		float deltaTime = max(frameTime, 0.0f);
		float angularFrequency = TAU * max(frequency, EPSILON_DAMPED_SPRING_FREQUENCY);
		float dampingRatio = max(damping, 0.0f);
		float4 displacement = position - target;

		if (dampingRatio < 0.999f) {
			float dampedFrequency = angularFrequency * sqrt(max(1.0f - dampingRatio * dampingRatio, EPSILON_DAMPED_SPRING_RADICAND));
			float phase = dampedFrequency * deltaTime;
			float phaseSin, phaseCos;
			sincos(phase, phaseSin, phaseCos);
			float decay = exp(-dampingRatio * angularFrequency * deltaTime);
			float dampingScale = dampingRatio * angularFrequency / dampedFrequency;
			float sinOverFrequency = phaseSin / dampedFrequency;
			nextPosition = target + decay *
			                            (displacement * (phaseCos + dampingScale * phaseSin) + velocity * sinOverFrequency);
			nextVelocity = decay *
			               (velocity * (phaseCos - dampingScale * phaseSin) -
							   displacement * (angularFrequency * angularFrequency * sinOverFrequency));
		} else if (dampingRatio <= 1.001f) {
			float decay = exp(-angularFrequency * deltaTime);
			nextPosition = target + decay *
			                            (displacement * (1.0f + angularFrequency * deltaTime) + velocity * deltaTime);
			nextVelocity = decay *
			               (velocity * (1.0f - angularFrequency * deltaTime) -
							   displacement * (angularFrequency * angularFrequency * deltaTime));
		} else {
			float root = sqrt(dampingRatio * dampingRatio - 1.0f);
			float rate1 = -angularFrequency * (dampingRatio - root);
			float rate2 = -angularFrequency * (dampingRatio + root);
			float inverseRateDelta = rcp(rate1 - rate2);
			float4 coefficient1 = (velocity - rate2 * displacement) * inverseRateDelta;
			float4 coefficient2 = displacement - coefficient1;
			float decay1 = exp(rate1 * deltaTime);
			float decay2 = exp(rate2 * deltaTime);
			nextPosition = target + coefficient1 * decay1 + coefficient2 * decay2;
			nextVelocity = rate1 * coefficient1 * decay1 + rate2 * coefficient2 * decay2;
		}
	}

	/** @brief Advances a three-component oscillator through the shared exact solver. */
	void Advance(float3 position, float3 velocity, float3 target, float frameTime,
		float frequency, float damping, out float3 nextPosition, out float3 nextVelocity)
	{
		float4 extendedPosition;
		float4 extendedVelocity;
		Advance(float4(position, 0.0f), float4(velocity, 0.0f), float4(target, 0.0f),
			frameTime, frequency, damping, extendedPosition, extendedVelocity);
		nextPosition = extendedPosition.xyz;
		nextVelocity = extendedVelocity.xyz;
	}

	/** @brief Advances a two-component oscillator through the shared exact solver. */
	void Advance(float2 position, float2 velocity, float2 target, float frameTime,
		float frequency, float damping, out float2 nextPosition, out float2 nextVelocity)
	{
		float4 extendedPosition;
		float4 extendedVelocity;
		Advance(float4(position, 0.0f, 0.0f), float4(velocity, 0.0f, 0.0f),
			float4(target, 0.0f, 0.0f), frameTime, frequency, damping,
			extendedPosition, extendedVelocity);
		nextPosition = extendedPosition.xy;
		nextVelocity = extendedVelocity.xy;
	}
}

#endif  // __DAMPED_SPRING_DEPENDENCY_HLSL__
