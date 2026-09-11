#ifndef __TREE_WIND_DEPENDENCY_HLSL__
#define __TREE_WIND_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/TreeWindSpring.hlsli"
#include "Common/WindField.hlsli"

namespace TreeWind
{
	namespace Detail
	{
		static const float MAXIMUM_AMBIENT_RESPONSE = 2.0;
		static const float TRANSIENT_LEAF_FLUTTER_GAIN = 3.0;
		static const float TRANSIENT_LEAF_STRUCTURAL_COUPLING = 0.75;

		/** @brief Returns model-adjusted ambient leaf flutter gain. */
		float GetLeafFlutterGain()
		{
			return max(Permutation::TreeLeafBaseWindFlutterGain, 0.0) *
			       max(Permutation::TreeLeafModelSensitivity, 0.0);
		}
	}

	namespace Detail
	{
		WindField::TransientImpulseSample UnpackTransientSample(float4 packedSample, bool immediate)
		{
			WindField::TransientImpulseSample sample;
			float2 velocity = immediate ? packedSample.zw : packedSample.xy;
			sample.velocity = float3(velocity, 0.0f);
			sample.intensity = length(velocity);
			return sample;
		}

		WindField::TransientImpulseSample SampleCurrentTransientAt(float3 worldPosition)
		{
			float4 packedSample = 0.0f.xxxx;
			TreeWindSpring::TrySampleCurrentTransient(worldPosition, packedSample);
			return UnpackTransientSample(packedSample, false);
		}

		WindField::TransientImpulseSample SamplePreviousTransientAt(float3 worldPosition)
		{
			float4 packedSample = 0.0f.xxxx;
			TreeWindSpring::TrySamplePreviousTransient(worldPosition, packedSample);
			return UnpackTransientSample(packedSample, false);
		}

		WindField::TransientImpulseSample SampleCurrentImmediateTransientAt(float3 worldPosition)
		{
			float4 packedSample = 0.0f.xxxx;
			TreeWindSpring::TrySampleCurrentTransient(worldPosition, packedSample);
			return UnpackTransientSample(packedSample, true);
		}

		WindField::TransientImpulseSample SamplePreviousImmediateTransientAt(float3 worldPosition)
		{
			float4 packedSample = 0.0f.xxxx;
			TreeWindSpring::TrySamplePreviousTransient(worldPosition, packedSample);
			return UnpackTransientSample(packedSample, true);
		}

		/** @brief Reduces base, middle, and top probes into one tree impulse sample. */
		WindField::TransientImpulseSample ResolveTransientSample(
			float treeHeight,
			WindField::TransientImpulseSample baseSample,
			WindField::TransientImpulseSample middleSample,
			WindField::TransientImpulseSample topSample)
		{
			WindField::TransientImpulseSample sample;
			sample.velocity = 0.0.xxx;
			sample.intensity = max(baseSample.intensity,
				max(middleSample.intensity, topSample.intensity));
			if (treeHeight <= 1e-3 || sample.intensity <= 1e-4)
				return sample;

			float2 combinedVelocity = baseSample.velocity.xy + middleSample.velocity.xy + topSample.velocity.xy;
			float combinedSpeed = length(combinedVelocity);
			float strongestSpeed = max(length(baseSample.velocity.xy),
				max(length(middleSample.velocity.xy), length(topSample.velocity.xy)));
			sample.velocity.xy =
				combinedSpeed > 1e-5 ? combinedVelocity * (strongestSpeed / combinedSpeed) : 0.0.xx;
			return sample;
		}

		/** @brief Returns the configured root-anchored quadratic trunk flexibility. */
		float GetBendFlexibility(float localHeight)
		{
			float treeHeight = Permutation::TreeWindBoundsHeight;
			if (treeHeight <= 1e-3)
				return 0.0;

			float normalizedHeight = saturate(
				(localHeight - Permutation::TreeWindBoundsBase) / treeHeight);
			float upperBendRange = max(Permutation::TreeWindUpperBendRange * 0.01, 0.05);
			float bendStartHeight = 1.0 - upperBendRange;
			float bendProgress = saturate((normalizedHeight - bendStartHeight) / upperBendRange);
			return bendProgress * bendProgress;
		}
	}

	/** @brief Converts the combined tree response into a measured-height-weighted trunk shift. */
	float2 GetWorldDisplacement(float localHeight, float2 windVelocity)
	{
		float treeHeight = Permutation::TreeWindBoundsHeight;
		if (treeHeight <= 1e-3)
			return 0.0.xx;

		float maximumDisplacement =
			treeHeight * max(Permutation::TreeWindMaximumDisplacementPercent, 0.0) * 0.01;
		return windVelocity *
		       (maximumDisplacement * Detail::GetBendFlexibility(localHeight) *
				   max(Permutation::TrunkWindBendSensitivity, 0.0) *
				   max(Permutation::TreeBendModelSensitivity, 0.0));
	}

	struct SamplePositions
	{
		float3 root;
		float3 base;
		float3 middle;
		float3 top;
	};

	/** @brief Builds consistent root and transient probe positions for one tree transform. */
	SamplePositions BuildSamplePositions(row_major float3x4 worldMatrix, float3 worldOffset)
	{
		SamplePositions positions;
		positions.base =
			mul(worldMatrix, float4(Permutation::TreeWindProbeBase.xyz, 1.0)).xyz + worldOffset;
		positions.top =
			mul(worldMatrix, float4(Permutation::TreeWindProbeTop.xyz, 1.0)).xyz + worldOffset;
		positions.middle = (positions.base + positions.top) * 0.5;
		positions.root = positions.base;
		return positions;
	}

	SamplePositions BuildSamplePositions(row_major float4x4 worldMatrix, float3 worldOffset)
	{
		return BuildSamplePositions((row_major float3x4)worldMatrix, worldOffset);
	}

	struct Sample
	{
		float3 trunkVelocity;
		float leafAnimationStrength;
	};

	namespace Detail
	{
		/** @brief Bounds extreme ambient response before model-specific displacement scaling. */
		float2 LimitAmbientVelocity(float2 ambientVelocity)
		{
			float speed = length(ambientVelocity);
			return speed > MAXIMUM_AMBIENT_RESPONSE ?
			           ambientVelocity * (MAXIMUM_AMBIENT_RESPONSE / speed) :
			           ambientVelocity;
		}

		/** @brief Applies the tree transient bend limit before the shared displacement path. */
		float3 LimitTrunkTransientVelocity(float3 transientVelocity)
		{
			float bendSensitivity = max(Permutation::TrunkWindBendSensitivity, 0.0f) *
			                        max(Permutation::TreeBendModelSensitivity, 0.0f);
			float responseSpeed = length(transientVelocity) * bendSensitivity;
			float maximumResponse = max(Permutation::TreeTransientMaximumBendMultiplier, 0.0f);
			return responseSpeed > maximumResponse && responseSpeed > EPSILON_WIND_RESPONSE ?
			           transientVelocity * (maximumResponse / responseSpeed) :
			           transientVelocity;
		}

		/** @brief Resolves the shared tree field plus structural and vertex-local transient samples. */
		Sample ResolveSample(
			WindField::TransientImpulseSample trunkTransientSample,
			WindField::TransientImpulseSample leafTransientSample,
			float2 filteredAmbientVelocity, float2 transientInfluence)
		{
			Sample sample;
			float3 trunkAmbientVelocity = float3(LimitAmbientVelocity(filteredAmbientVelocity), 0.0f);
			float3 trunkTransientVelocity = LimitTrunkTransientVelocity(
				trunkTransientSample.velocity * max(transientInfluence.x, 0.0f));
			float leafTransientInfluence = max(transientInfluence.y, 0.0f);
			float transientLeafSpeed =
				length(leafTransientSample.velocity.xy) +
				length(trunkTransientSample.velocity.xy) * TRANSIENT_LEAF_STRUCTURAL_COUPLING;
			float transientLeafAnimationStrength = min(
				transientLeafSpeed * TRANSIENT_LEAF_FLUTTER_GAIN * leafTransientInfluence,
				max(Permutation::TreeLeafTransientFlutterMaximum, 0.0f));
			sample.trunkVelocity = trunkAmbientVelocity + trunkTransientVelocity;
			sample.leafAnimationStrength =
				length(filteredAmbientVelocity) * GetLeafFlutterGain() +
				transientLeafAnimationStrength;
			return sample;
		}

		/** @brief Reduces three current transient probes into one tree impulse sample. */
		WindField::TransientImpulseSample SampleCurrentTransient(SamplePositions positions)
		{
			return ResolveTransientSample(
				Permutation::TreeWindBoundsHeight,
				SampleCurrentTransientAt(positions.base),
				SampleCurrentTransientAt(positions.middle),
				SampleCurrentTransientAt(positions.top));
		}

		/** @brief Reduces three previous transient probes into one tree impulse sample. */
		WindField::TransientImpulseSample SamplePreviousTransient(SamplePositions positions)
		{
			return ResolveTransientSample(
				Permutation::TreeWindBoundsHeight,
				SamplePreviousTransientAt(positions.base),
				SamplePreviousTransientAt(positions.middle),
				SamplePreviousTransientAt(positions.top));
		}

		WindField::TransientImpulseSample SampleCurrentImmediateTransient(SamplePositions positions)
		{
			return ResolveTransientSample(
				Permutation::TreeWindBoundsHeight,
				SampleCurrentImmediateTransientAt(positions.base),
				SampleCurrentImmediateTransientAt(positions.middle),
				SampleCurrentImmediateTransientAt(positions.top));
		}

		WindField::TransientImpulseSample SamplePreviousImmediateTransient(SamplePositions positions)
		{
			return ResolveTransientSample(
				Permutation::TreeWindBoundsHeight,
				SamplePreviousImmediateTransientAt(positions.base),
				SamplePreviousImmediateTransientAt(positions.middle),
				SamplePreviousImmediateTransientAt(positions.top));
		}
	}

	/** @brief Samples and resolves current tree wind from precomputed world-space positions. */
	Sample SampleCurrent(SamplePositions positions, float2 transientInfluence)
	{
		return Detail::ResolveSample(
			Detail::SampleCurrentTransient(positions),
			Detail::SampleCurrentImmediateTransient(positions),
			TreeWindSpring::SampleCurrent(positions.root.xy), transientInfluence);
	}

	/** @brief Samples current tree wind with transient leaf response localized to one vertex. */
	Sample SampleCurrent(
		SamplePositions positions, float3 leafWorldPosition,
		float2 transientInfluence)
	{
		return Detail::ResolveSample(
			Detail::SampleCurrentTransient(positions),
			Detail::SampleCurrentImmediateTransientAt(leafWorldPosition),
			TreeWindSpring::SampleCurrent(positions.root.xy), transientInfluence);
	}

	/** @brief Samples and resolves previous tree wind from precomputed world-space positions. */
	Sample SamplePrevious(SamplePositions positions, float2 transientInfluence)
	{
		return Detail::ResolveSample(
			Detail::SamplePreviousTransient(positions),
			Detail::SamplePreviousImmediateTransient(positions),
			TreeWindSpring::SamplePrevious(positions.root.xy), transientInfluence);
	}

	/** @brief Samples previous tree wind with transient leaf response localized to one vertex. */
	Sample SamplePrevious(
		SamplePositions positions, float3 leafWorldPosition,
		float2 transientInfluence)
	{
		return Detail::ResolveSample(
			Detail::SamplePreviousTransient(positions),
			Detail::SamplePreviousImmediateTransientAt(leafWorldPosition),
			TreeWindSpring::SamplePrevious(positions.root.xy), transientInfluence);
	}
}

#endif  // __TREE_WIND_DEPENDENCY_HLSL__
