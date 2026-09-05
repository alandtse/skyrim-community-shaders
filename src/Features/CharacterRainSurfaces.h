#pragma once

namespace CharacterRainSurfaces
{
	/** @brief Installs character and held-weapon render-pass classification. */
	void Install();
	/** @brief Registers actor-load and equipment-change event listeners. */
	void RegisterEvents();
	/** @brief Queues actors already present after a save finishes loading. */
	void QueueLoadedActors();
	/** @brief Refreshes queued actor geometry tags on the render thread. */
	void RefreshPendingActors();
}
