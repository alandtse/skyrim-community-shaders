#pragma once

#include <cstddef>
#include <span>
#include <type_traits>

namespace WindField
{
	// Future optimization: if CPU or GPU wind consumers grow beyond this bounded source scan,
	// add spatial indexing or rasterize the shared sources into a wind field before sampling.
	inline constexpr std::size_t kTransientImpulseCapacity = 96;
	inline constexpr float kTransientImpulseMaximumDecayTime = 5.0f;

	/** @brief Analytic shape used to evaluate a transient wind source. */
	enum class TransientWindSourceType : uint32_t
	{
		DirectionalWave,
		RadialWave,
		DirectionalCone,
		OrientedFlow,
		DirectionalOvalWave
	};

	/** @brief A typed analytic source contributing to the shared 3D wind field. */
	struct TransientWindSource
	{
		float3 origin{};
		float wavefrontDistance{};
		float3 direction{};
		float strength{};
		float maxDistance{};
		float waveHalfWidth{};
		float propagationSpeed{};
		float coneSpreadTangent{};
		float decayTime{};
		float collisionRadius{};
		float sideSpreadTangent{};
		float sideStrength{};
		TransientWindSourceType type{};
		float verticalScale{ 1.0f };
		float falloffWidth{};
		float padding{};
	};
	static_assert(sizeof(TransientWindSource) == 80);
	static_assert(std::is_standard_layout_v<TransientWindSource>);

	/** @brief The XYZ velocity and normalized visualization intensity contributed by one impulse. */
	struct TransientImpulseSample
	{
		float3 velocity{};
		float intensity{};
	};

	/**
	 * @brief Samples one moving pressure wave at an absolute world position.
	 * @param a_worldPosition Absolute position in Skyrim's Z-up world space.
	 * @param a_impulse Current wavefront state. A zero direction produces a radial wave.
	 * @return Additive XYZ velocity and normalized local wave intensity.
	 */
	[[nodiscard]] TransientImpulseSample SampleTransientImpulse(
		const float3& a_worldPosition, const TransientWindSource& a_impulse) noexcept;

	/**
	 * @brief Accumulates transient impulses at an absolute world position.
	 * @param a_worldPosition Absolute position in Skyrim's Z-up world space.
	 * @param a_impulses Active pressure impulses to sample.
	 * @return Additive XYZ velocity and maximum normalized local intensity.
	 */
	[[nodiscard]] TransientImpulseSample SampleTransientImpulses(
		const float3& a_worldPosition, std::span<const TransientWindSource> a_impulses) noexcept;

	/** @brief Creates a traveling directional pressure wave. */
	[[nodiscard]] TransientWindSource MakeDirectionalWave(const float3& a_origin,
		const float3& a_direction, float a_strength, float a_maxDistance, float a_waveHalfWidth,
		float a_propagationSpeed, float a_coneCosine, float a_decayTime) noexcept;

	/** @brief Creates a traveling radial pressure wave with horizontal output velocity. */
	[[nodiscard]] TransientWindSource MakeRadialWave(const float3& a_origin,
		const float3& a_fallbackDirection, float a_strength, float a_maxDistance,
		float a_waveHalfWidth, float a_propagationSpeed, float a_decayTime) noexcept;

	/** @brief Creates a filled directional cone suitable for a frame-updated attached source. */
	[[nodiscard]] TransientWindSource MakeDirectionalCone(const float3& a_origin,
		const float3& a_direction, float a_strength, float a_maxDistance,
		float a_coneCosine) noexcept;

	/** @brief Creates an oriented bow-wave and wake volume around a moving source. */
	[[nodiscard]] TransientWindSource MakeOrientedFlow(const float3& a_origin,
		const float3& a_direction, float a_strength, float a_radius, float a_bowLength,
		float a_wakeLength) noexcept;
}
