#pragma once

namespace ActorWind
{
	/** Returns whether the actor has a dragon race keyword or dragon behavior graph. */
	[[nodiscard]] bool IsDragon(const RE::Actor& a_actor);

	/** @brief Returns the actor's visual-root position with a bounded actor-position fallback. */
	[[nodiscard]] float3 GetVisualOrigin(RE::Actor& a_actor) noexcept;

	/** @brief Returns the actor's magic-node position with an upper-body fallback. */
	[[nodiscard]] float3 GetMagicOrigin(RE::Actor& a_actor) noexcept;

	/** @brief Returns the actor's normalized aim direction in Skyrim's Z-up world space. */
	[[nodiscard]] float3 GetAimDirection(RE::Actor& a_actor) noexcept;
}
