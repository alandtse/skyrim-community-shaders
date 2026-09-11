#include "State.h"

#include <algorithm>
#include <cmath>

#include "Features/Wind/Trees/TreeWindPatcher.h"
#include "Features/Wind/Wind.h"
#include "Globals.h"

namespace
{
	struct WindSelection
	{
		float3 direction;
		float speed;
	};

	WindSelection SelectWind(const Wind& a_wind, const float3& a_ambientVelocity,
		const float3& a_fallbackDirection)
	{
		const float ambientSpeed = std::sqrt(
			a_ambientVelocity.x * a_ambientVelocity.x +
			a_ambientVelocity.y * a_ambientVelocity.y +
			a_ambientVelocity.z * a_ambientVelocity.z);
		const float speed = !a_wind.loaded || a_wind.ShouldUseRealWindSpeed() ?
		                        (std::isfinite(ambientSpeed) ? std::max(ambientSpeed, 0.0f) : 0.0f) :
		                        std::max(a_wind.GetEffectiveWindOverrideSpeed(), 0.0f);
		const bool useRealDirection = !a_wind.loaded || a_wind.runtimeState.windFieldUseRealDirection;
		const float directionLength = std::hypot(a_ambientVelocity.x, a_ambientVelocity.y);
		float3 direction = a_fallbackDirection;
		if (useRealDirection && std::isfinite(directionLength) && directionLength > 0.0001f) {
			direction = float3{
				a_ambientVelocity.x / directionLength,
				a_ambientVelocity.y / directionLength,
				0.0f
			};
		} else if (!useRealDirection) {
			const float directionRadians = DirectX::XMConvertToRadians(a_wind.runtimeState.windFieldAppliedDirectionDegrees);
			direction = float3{ std::cos(directionRadians), std::sin(directionRadians), 0.0f };
		}
		return { direction, speed };
	}
}

void State::UpdateWind()
{
	auto& wind = globals::features::wind;
	windFieldTuning.gustAmplitude = wind.GetEffectiveWindGustAmplitude();
	windFieldTuning.gustScale = wind.GetEffectiveWindGustScale();
	windFieldTuning.frontAspectRatio =
		wind.settings.windFieldGustCrosswindScale / windFieldTuning.gustScale;
	windFieldTuning.gustAdvectionMultiplier = wind.GetEffectiveWindGustAdvectionMultiplier();
	const bool gamePaused = globals::game::ui && globals::game::ui->GameIsPaused();
	const float frameTime = gamePaused ? 0.0f : std::max(RE::GetSecondsSinceLastFrame(), 0.0f);
	wind.UpdateWindEffects(frameTime);
	AdvanceWindHistory(frameTime);
	UpdateWeatherWind();
	const float3 fallbackDirection =
		windFieldHasPreviousSample ? windFieldCurrent.direction : float3{ 1.0f, 0.0f, 0.0f };
	const auto selectedWind = SelectWind(wind, ambientWindVelocity, fallbackDirection);
	UpdateWindField(selectedWind.direction, selectedWind.speed, frameTime);
}

void State::AdvanceWindHistory(float a_frameTime)
{
	windFieldFrameTime = a_frameTime;
	UpdateTransientWindImpulses(a_frameTime);
	previousWindFieldSelectedVelocity = windFieldSelectedVelocity;
	previousWindFieldGustTravelDistance = windFieldGustTravelDistance;
	previousWindFieldCurrent = windFieldCurrent;
	previousWindFieldTransition = windFieldTransition;
	previousWindFieldTransitionBlend = windFieldTransitionBlend;
}

void State::UpdateWeatherWind()
{
	ambientWindVelocity = float3{};
	const auto* sky = globals::game::sky;
	const float activeWindIntensity = sky && std::isfinite(sky->windSpeed) ?
	                                      std::clamp(sky->windSpeed, 0.0f, 1.0f) :
	                                      0.0f;
	float2 weatherDirection{};
	if (sky && std::isfinite(sky->windAngle)) {
		weatherDirection = float2{ std::cos(sky->windAngle), std::sin(sky->windAngle) };
	}
	const float directionLength = std::hypot(weatherDirection.x, weatherDirection.y);
	if (std::isfinite(directionLength) && directionLength > 0.0001f && std::isfinite(activeWindIntensity)) {
		weatherDirection.x /= directionLength;
		weatherDirection.y /= directionLength;
		ambientWindVelocity = float3{ weatherDirection.x * activeWindIntensity, weatherDirection.y * activeWindIntensity, 0.0f };
	}
}

void State::UpdateWindField(const float3& a_direction, float a_speed, float a_frameTime)
{
	const auto& wind = globals::features::wind;
	const float gustAdvectionSpeed = a_speed * windFieldTuning.gustAdvectionBaseSpeed *
	                                 windFieldTuning.gustAdvectionMultiplier;
	windFieldAdvectionSpeed = std::isfinite(gustAdvectionSpeed) ? std::max(gustAdvectionSpeed, 0.0f) : 0.0f;
	windFieldTravelDelta = std::isfinite(a_frameTime) ? windFieldAdvectionSpeed * std::max(a_frameTime, 0.0f) : 0.0f;
	if (!windFieldHasPreviousSample) {
		windFieldCurrent = WindField::CreateField(a_direction, a_speed);
		previousWindFieldCurrent = windFieldCurrent;
		windFieldTransition = windFieldCurrent;
		previousWindFieldTransition = windFieldCurrent;
		windFieldTransitionBlend = 1.0f;
		previousWindFieldTransitionBlend = 1.0f;
		windFieldHasPreviousSample = true;
	} else {
		constexpr float kDirectionChangeCosine = 0.9998477f;  // one degree
		const float directionDot = windFieldCurrent.direction.x * a_direction.x +
		                           windFieldCurrent.direction.y * a_direction.y;
		if (!windFieldTransitionActive && directionDot < kDirectionChangeCosine) {
			windFieldTransition = windFieldCurrent;
			previousWindFieldTransition = previousWindFieldCurrent;
			windFieldCurrent = WindField::CreateField(a_direction, a_speed);
			windFieldTransitionElapsed = 0.0f;
			windFieldTransitionBlend = 0.0f;
			windFieldTransitionActive = true;
		}
	}
	WindField::SetFieldSpeed(windFieldCurrent, a_speed);
	WindField::AdvanceField(windFieldCurrent, windFieldTravelDelta);
	if (windFieldTransitionActive) {
		WindField::SetFieldSpeed(windFieldTransition, a_speed);
		WindField::AdvanceField(windFieldTransition, windFieldTravelDelta);
		windFieldTransitionElapsed += a_frameTime;
		const float configuredTransitionDuration = wind.settings.windFieldDirectionTransitionDuration;
		const float transitionDuration = std::isfinite(configuredTransitionDuration) ?
		                                     std::clamp(configuredTransitionDuration, 0.0f, 30.0f) :
		                                     1.0f;
		windFieldTransitionBlend = transitionDuration > 0.0f ?
		                               std::clamp(windFieldTransitionElapsed / transitionDuration, 0.0f, 1.0f) :
		                               1.0f;
		if (windFieldTransitionBlend >= 1.0f)
			windFieldTransitionActive = false;
	} else {
		windFieldTransitionBlend = 1.0f;
	}
	windFieldSelectedSpeed = windFieldCurrent.speed;
	windFieldSelectedVelocity = windFieldCurrent.direction * windFieldCurrent.speed;
	windFieldAmbientSpeed = windFieldCurrent.speed;
	windFieldGustTravelDistance = windFieldCurrent.travelDistance;
	previousWindFieldSelectedVelocity = previousWindFieldCurrent.direction * previousWindFieldCurrent.speed;
	previousWindFieldGustTravelDistance = previousWindFieldCurrent.travelDistance;
}

void State::UpdateWindPermutationData()
{
	const auto& wind = globals::features::wind;
	const auto& settings = wind.settings;
	permutationData.WindIntensityOverride = settings.trunkWindIntensityOverride;
	permutationData.OverrideWindIntensity = wind.loaded && settings.overrideTrunkWindIntensity;
	const auto treeBendDescriptor = static_cast<uint32_t>(ExtraShaderDescriptors::TreeBend);
	if ((permutationData.ExtraShaderDescriptor & treeBendDescriptor) == 0) {
		const TreeWindPatcher::Sensitivities treeDefaults{};
		permutationData.TreeTransientWindInfluence = treeDefaults.transientWindInfluence;
		permutationData.TreeLeafTransientWindInfluence = treeDefaults.leafTransientWindInfluence;
		permutationData.TreeLeafTransientFlutterMaximum = treeDefaults.leafTransientFlutterMaximum;
		permutationData.TreeTransientMaximumBendMultiplier = treeDefaults.transientMaximumBendMultiplier;
	}
	permutationData.TrunkWindBendSensitivity = settings.trunkWindBendSensitivity;
	permutationData.TreeLeafBaseWindFlutterGain = settings.treeLeafBaseWindFlutterGain;
	permutationData.EnableAmbientGrassWind = wind.loaded && settings.enableAmbientGrassWind;
	permutationData.GrassWindSensitivity = settings.grassWindSensitivity;
	permutationData.GrassWindBendProfile = settings.grassWindBendProfile;
	permutationData.GrassWindCompressionToBend = settings.grassWindCompressionToBend;
	permutationData.GrassWindFlutterStrength = settings.grassWindFlutterStrength;
	permutationData.GrassWindFlutterFrequency = settings.grassWindFlutterFrequency;
}

void State::UpdateWindSharedData(SharedDataCB& a_data) const
{
	a_data.WindFieldTuning = windFieldTuning;
	const float3 transitionVelocity = windFieldTransition.direction * windFieldTransition.speed;
	const float3 previousTransitionVelocity = previousWindFieldTransition.direction * previousWindFieldTransition.speed;
	const float3 blendedVelocity = transitionVelocity +
	                               (windFieldSelectedVelocity - transitionVelocity) * windFieldTransitionBlend;
	const float3 previousBlendedVelocity =
		previousTransitionVelocity +
		(previousWindFieldSelectedVelocity - previousTransitionVelocity) * previousWindFieldTransitionBlend;
	a_data.WindFieldAmbient = float4{
		blendedVelocity.x, blendedVelocity.y, blendedVelocity.z,
		windFieldGustTravelDistance
	};
	a_data.WindFieldPreviousAmbient = float4{
		previousBlendedVelocity.x, previousBlendedVelocity.y, previousBlendedVelocity.z,
		previousWindFieldGustTravelDistance
	};
	a_data.WindFieldCurrent = windFieldCurrent;
	a_data.WindFieldPrevious = previousWindFieldCurrent;
	a_data.WindFieldTransition = windFieldTransition;
	a_data.WindFieldPreviousTransition = previousWindFieldTransition;
	a_data.WindFieldTransitionData = float4{
		windFieldTransitionBlend,
		previousWindFieldTransitionBlend,
		0.0f,
		0.0f
	};
	a_data.WindFieldSpringDebug = float4{
		globals::features::wind.grassState.springFieldMinimum[0].x,
		globals::features::wind.grassState.springFieldMinimum[0].y,
		globals::features::wind.grassState.springFieldAvailable[0] ?
			globals::features::wind.grassState.springWorldSizes[0] :
			0.0f,
		DirectX::XMConvertToRadians(globals::features::wind.settings.grassWindMaximumTilt)
	};
	a_data.WindFieldActiveCounts = {
		activeTransientWindImpulseCount,
		previousActiveTransientWindImpulseCount,
		0u,
		0u
	};
	a_data.WindFieldTransientImpulses = transientWindImpulses;
	a_data.WindFieldPreviousTransientImpulses = previousTransientWindImpulses;
}

WindField::WindSample State::SampleWind(const float3& a_worldPosition) const noexcept
{
	auto sample = WindField::SampleField(a_worldPosition, windFieldCurrent, windFieldTuning);
	if (windFieldTransitionBlend < 1.0f) {
		const auto previousSample = WindField::SampleField(a_worldPosition, windFieldTransition, windFieldTuning);
		sample.velocity = previousSample.velocity + (sample.velocity - previousSample.velocity) * windFieldTransitionBlend;
		sample.ambientGust = previousSample.ambientGust +
		                     (sample.ambientGust - previousSample.ambientGust) * windFieldTransitionBlend;
	}
	const auto transientSample = WindField::SampleTransientImpulses(a_worldPosition,
		std::span{ transientWindImpulses }.first(activeTransientWindImpulseCount));
	sample.velocity += transientSample.velocity;
	sample.transientImpulse = std::max(sample.transientImpulse, transientSample.intensity);
	return sample;
}

WindField::WindSample State::SampleWind(const float3& a_worldPosition,
	const float3& a_windDirection, float a_windSpeed) const noexcept
{
	const auto field = WindField::CreateField(a_windDirection, a_windSpeed, windFieldGustTravelDistance);
	auto sample = WindField::SampleField(a_worldPosition, field, windFieldTuning);
	const auto transientSample = WindField::SampleTransientImpulses(a_worldPosition,
		std::span{ transientWindImpulses }.first(activeTransientWindImpulseCount));
	sample.velocity += transientSample.velocity;
	sample.transientImpulse = std::max(sample.transientImpulse, transientSample.intensity);
	return sample;
}

void State::QueueTransientWindImpulse(const WindField::TransientWindSource& a_impulse)
{
	QueueTransientWindSource(a_impulse, TransientWindSourceOwner::Generic,
		TransientWindSourcePriority::Wingbeat);
}

void State::QueueTransientWindSource(const WindField::TransientWindSource& a_source,
	TransientWindSourceOwner a_owner, TransientWindSourcePriority a_priority)
{
	std::lock_guard lock(transientWindImpulseMutex);
	pendingTransientWindSources.push_back({ a_source, a_owner, a_priority, ++transientWindSourceSequence });
	if (pendingTransientWindSources.size() > WindField::kTransientImpulseCapacity) {
		const auto leastImportant = std::ranges::min_element(
			pendingTransientWindSources, [](const auto& left, const auto& right) {
				if (left.priority != right.priority)
					return left.priority < right.priority;
				return left.sequence < right.sequence;
			});
		pendingTransientWindSources.erase(leastImportant);
	}
}

void State::SetAttachedTransientWindSources(TransientWindSourceOwner a_owner,
	std::span<const TransientWindSourceSubmission> a_sources)
{
	std::lock_guard lock(transientWindImpulseMutex);
	std::erase_if(attachedTransientWindSources,
		[a_owner](const auto& source) { return source.owner == a_owner; });
	for (const auto& submission : a_sources) {
		attachedTransientWindSources.push_back(
			{ submission.source, a_owner, submission.priority, ++transientWindSourceSequence });
	}
}

void State::ClearTransientWindSources(TransientWindSourceOwner a_owner)
{
	std::lock_guard lock(transientWindImpulseMutex);
	auto removeOwned = [a_owner](auto& sources) {
		std::erase_if(sources, [a_owner](const auto& source) { return source.owner == a_owner; });
	};
	removeOwned(activeTransientWindSources);
	removeOwned(pendingTransientWindSources);
	removeOwned(attachedTransientWindSources);

	auto compactSnapshot = [a_owner](auto& sources, auto& owners, uint32_t& count) {
		uint32_t outputIndex = 0;
		for (uint32_t index = 0; index < count; ++index) {
			if (owners[index] == a_owner)
				continue;
			sources[outputIndex] = sources[index];
			owners[outputIndex] = owners[index];
			++outputIndex;
		}
		for (uint32_t index = outputIndex; index < count; ++index) {
			sources[index] = {};
			owners[index] = {};
		}
		count = outputIndex;
	};
	compactSnapshot(transientWindImpulses, transientWindImpulseOwners, activeTransientWindImpulseCount);
	compactSnapshot(previousTransientWindImpulses, previousTransientWindImpulseOwners,
		previousActiveTransientWindImpulseCount);
}

void State::ClearTransientWindImpulses()
{
	std::lock_guard lock(transientWindImpulseMutex);
	transientWindImpulses = {};
	previousTransientWindImpulses = {};
	transientWindImpulseOwners = {};
	previousTransientWindImpulseOwners = {};
	activeTransientWindImpulseCount = 0;
	previousActiveTransientWindImpulseCount = 0;
	activeTransientWindSources.clear();
	pendingTransientWindSources.clear();
	attachedTransientWindSources.clear();
}

void State::UpdateTransientWindImpulses(float a_frameTime)
{
	previousTransientWindImpulses = transientWindImpulses;
	previousTransientWindImpulseOwners = transientWindImpulseOwners;
	previousActiveTransientWindImpulseCount = activeTransientWindImpulseCount;

	const float frameTime = std::isfinite(a_frameTime) ? std::max(a_frameTime, 0.0f) : 0.0f;
	for (std::size_t index = 0; index < activeTransientWindSources.size();) {
		auto& impulse = activeTransientWindSources[index].source;
		const float travelDelta = std::isfinite(impulse.propagationSpeed) ?
		                              std::max(impulse.propagationSpeed, 0.0f) * frameTime :
		                              0.0f;
		impulse.wavefrontDistance += travelDelta;
		const float decayTime = std::clamp(
			std::isfinite(impulse.decayTime) ? impulse.decayTime : 0.0f, 0.0f,
			WindField::kTransientImpulseMaximumDecayTime);
		const float retentionDistance = impulse.maxDistance + std::abs(impulse.waveHalfWidth) +
		                                std::max(impulse.propagationSpeed, 0.0f) * decayTime;
		if (!std::isfinite(impulse.wavefrontDistance) || impulse.wavefrontDistance >= retentionDistance) {
			activeTransientWindSources.erase(activeTransientWindSources.begin() + index);
		} else {
			++index;
		}
	}

	std::vector<ManagedTransientWindSource> pendingSources;
	std::vector<ManagedTransientWindSource> attachedSources;
	{
		std::lock_guard lock(transientWindImpulseMutex);
		pendingSources.swap(pendingTransientWindSources);
		attachedSources = attachedTransientWindSources;
	}
	for (auto& pendingSource : pendingSources) {
		pendingSource.source.wavefrontDistance = 0.0f;
		activeTransientWindSources.push_back(pendingSource);
	}
	std::ranges::stable_sort(activeTransientWindSources, [](const auto& left, const auto& right) {
		if (left.priority != right.priority)
			return left.priority > right.priority;
		return left.sequence > right.sequence;
	});
	if (activeTransientWindSources.size() > WindField::kTransientImpulseCapacity)
		activeTransientWindSources.resize(WindField::kTransientImpulseCapacity);

	std::vector<ManagedTransientWindSource> composedSources = activeTransientWindSources;
	composedSources.insert(composedSources.end(), attachedSources.begin(), attachedSources.end());
	std::ranges::stable_sort(composedSources, [](const auto& left, const auto& right) {
		if (left.priority != right.priority)
			return left.priority > right.priority;
		return left.sequence > right.sequence;
	});
	if (composedSources.size() > WindField::kTransientImpulseCapacity)
		composedSources.resize(WindField::kTransientImpulseCapacity);

	transientWindImpulses = {};
	transientWindImpulseOwners = {};
	activeTransientWindImpulseCount = static_cast<uint32_t>(composedSources.size());
	for (uint32_t index = 0; index < activeTransientWindImpulseCount; ++index) {
		transientWindImpulses[index] = composedSources[index].source;
		transientWindImpulseOwners[index] = composedSources[index].owner;
	}
}
